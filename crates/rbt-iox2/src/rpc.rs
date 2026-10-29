//! Typed, bounded local request/response RPC over iceoryx2 0.10.
//!
//! The request and response payloads in this module are native iceoryx2
//! loans.  The two event services are advisory wakeup sidecars only; they do
//! not carry request identifiers or payload bytes.  Native
//! [`iceoryx2::pending_response::PendingResponse`] correlation remains the
//! authority for accepting a response.

use std::{
    fmt,
    marker::PhantomData,
    ops::{Deref, DerefMut},
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicU8, Ordering},
    },
    time::{Duration, Instant},
};

use iceoryx2::{
    active_request::ActiveRequest as IoxActiveRequest,
    node::{Node, NodeBuilder, NodeCreationFailure},
    pending_response::PendingResponse as IoxPendingResponse,
    port::{
        LoanError, ReceiveError, SendError,
        backpressure_strategy::BackpressureStrategy,
        client::{Client as IoxClient, RequestSendError},
        listener::{Listener as IoxListener, ListenerCreateError},
        notifier::{Notifier as IoxNotifier, NotifierCreateError},
        server::Server as IoxServer,
    },
    prelude::{
        AllocationStrategy, CallbackProgression, EventId, IceoryxSend, SignalHandlingMode,
        ZeroCopySend, ipc_threadsafe,
    },
    request_mut::RequestMut as IoxRequestMut,
    response::Response as IoxResponse,
    response_mut::ResponseMut as IoxResponseMut,
    service::{
        builder::{
            event::{EventOpenError, EventOpenOrCreateError},
            request_response::{RequestResponseOpenError, RequestResponseOpenOrCreateError},
        },
        port_factory::event::PortFactory as IoxEventFactory,
        service_name::{ServiceName, ServiceNameError},
    },
    waitset::{WaitSetAttachmentId, WaitSetBuilder},
};
use rbt_types::{
    FixedMessageAbi, MessageTypeDescriptor, RpcDescriptor, RpcReadinessProtocol, RpcResourceConfig,
    SliceMessageAbi,
};
use thiserror::Error;

use crate::rpc_pressure::{
    ClientCallGuard, ClientPressureState, ClientTerminal, ServerPressureState, ServerRequestGuard,
};
use crate::wait::consume_sidecar_activations;
use crate::{EventSource, WaitToken};
use rbt_core::{ActivationId, CapabilityInstanceId, RpcPressure, register_rpc_pressure_source};

const RPC_EVENT_ID: EventId = EventId::new(1);
const RPC_WAIT_INTERVAL: Duration = Duration::from_millis(100);
const REQUEST_READY_SUFFIX: &str = "/request-ready";
const RESPONSE_READY_SUFFIX: &str = "/response-ready";
const CLIENT_CANCEL_SUFFIX: &str = "/client-cancel";
const SERVER_CANCEL_SUFFIX: &str = "/server-cancel";

type Ipc = ipc_threadsafe::Service;
type NativeClient<Q, R, QH = (), RH = ()> = IoxClient<Ipc, Q, QH, R, RH>;
type NativeServer<Q, R, QH = (), RH = ()> = IoxServer<Ipc, Q, QH, R, RH>;
type NativeRequest<Q, R, QH = (), RH = ()> = IoxRequestMut<Ipc, Q, QH, R, RH>;
type NativePending<Q, R, QH = (), RH = ()> = IoxPendingResponse<Ipc, Q, QH, R, RH>;
type NativeResponse<R, RH = ()> = IoxResponse<Ipc, R, RH>;
type NativeActive<Q, R, QH = (), RH = ()> = IoxActiveRequest<Ipc, Q, QH, R, RH>;
type NativeResponseLoan<R, RH = ()> = IoxResponseMut<Ipc, R, RH>;
type NativeFactory<Q, R, QH = (), RH = ()> =
    iceoryx2::service::port_factory::request_response::PortFactory<Ipc, Q, QH, R, RH>;

/// A result returned by the typed RPC data-plane API.
pub type RpcResult<T> = std::result::Result<T, RpcError>;

/// Failures produced by the typed local RPC data plane.
#[derive(Debug, Error)]
pub enum RpcError {
    /// The local payload ABI did not match the provider descriptor.
    #[error(
        "RPC ABI mismatch for {message}: expected layout {expected_layout} (ABI {expected_abi_major}), provider layout {actual_layout} (ABI {actual_abi_major})"
    )]
    AbiMismatch {
        /// Request or response label.
        message: String,
        /// Locally expected ABI major.
        expected_abi_major: u16,
        /// Provider ABI major.
        actual_abi_major: u16,
        /// Locally expected layout identity.
        expected_layout: rbt_types::LayoutHash,
        /// Provider layout identity.
        actual_layout: rbt_types::LayoutHash,
        /// Complete local descriptor.
        expected: Box<MessageTypeDescriptor>,
        /// Complete provider descriptor.
        actual: Box<MessageTypeDescriptor>,
    },
    /// The provider did not advertise the required RPC sidecar protocol.
    #[error(
        "missing RPC transport feature: readiness requires {required}, provider advertises {provider}"
    )]
    MissingTransportFeature {
        /// Required readiness protocol.
        required: RpcReadinessProtocol,
        /// Advertised readiness protocol.
        provider: RpcReadinessProtocol,
    },
    /// A resource value was zero or otherwise invalid for the bounded API.
    #[error("invalid RPC resource {field}={value}; value must be greater than zero")]
    InvalidResourceConfig {
        /// Resource field name.
        field: &'static str,
        /// Rejected value.
        value: usize,
    },
    /// A sidecar listener capacity multiplication overflowed.
    #[error("RPC resource multiplication overflow for {field}")]
    ResourceOverflow {
        /// Resource expression that overflowed.
        field: &'static str,
    },
    /// A service name or sidecar name was invalid.
    #[error("invalid RPC service name: {0}")]
    ServiceName(#[from] ServiceNameError),
    /// The native node could not be created.
    #[error("failed to create RPC node: {0}")]
    Node(NodeCreationFailure),
    /// An event sidecar could not be opened or created.
    #[error("failed to open or create RPC sidecar: {0}")]
    EventOpenOrCreate(String),
    /// An existing event sidecar could not be opened.
    #[error("failed to open RPC sidecar: {0}")]
    EventOpen(String),
    /// A sidecar listener could not be created.
    #[error("failed to create RPC sidecar listener: {0}")]
    Listener(ListenerCreateError),
    /// A sidecar notifier could not be created.
    #[error("failed to create RPC sidecar notifier: {0}")]
    Notifier(NotifierCreateError),
    /// A native request/response service could not be opened or created.
    #[error("failed to open or create native RPC service: {0}")]
    RequestResponseOpenOrCreate(String),
    /// An existing native request/response service could not be opened.
    #[error("failed to open native RPC service: {0}")]
    RequestResponseOpen(String),
    /// A native client endpoint could not be created.
    #[error("failed to create native RPC client: {0}")]
    ClientCreate(String),
    /// A native server endpoint could not be created.
    #[error("failed to create native RPC server: {0}")]
    ServerCreate(String),
    /// The client has reached its configured active-call ceiling.
    #[error("RPC client capacity reached: maximum active requests are in flight")]
    ClientCapacityReached,
    /// A server could not accept the request because its bounded request path
    /// was full.
    #[error("RPC server capacity reached: request was not delivered")]
    ServerCapacityReached,
    /// The server could not deliver a response because the bounded response
    /// path was full.
    #[error("RPC response capacity reached: response was not delivered")]
    ResponseCapacityReached,
    /// The request was sent while no server connection was available.
    #[error("RPC server unavailable; request may not have executed")]
    ServerUnavailable,
    /// A native loan pool was exhausted.
    #[error("RPC loan pool exhausted: {0}")]
    LoanPoolExhausted(LoanError),
    /// A requested dynamic payload exceeds the advertised slice ceiling.
    #[error("RPC {direction} slice length {requested} exceeds maximum {maximum}")]
    InvalidSliceLength {
        /// Whether this is a request or response length.
        direction: &'static str,
        /// Requested number of elements.
        requested: usize,
        /// Advertised maximum number of elements.
        maximum: usize,
    },
    /// The caller exceeded the configured response borrow limit.
    #[error("RPC response borrow limit reached: {0}")]
    BorrowLimitReached(ReceiveError),
    /// The call exceeded its monotonic deadline. This does not prove whether
    /// the remote operation executed.
    #[error("RPC deadline exceeded; execution of the remote operation is unknown")]
    DeadlineExceeded,
    /// The individual pending call was cancelled.
    #[error("RPC call cancelled")]
    Cancelled,
    /// The client/server activation was expired or shut down.
    #[error("RPC activation expired")]
    ActivationExpired,
    /// A native operation failed without a more specific bounded category.
    #[error("RPC transport failure during {operation}: {detail}")]
    TransportFailure {
        /// Native operation being attempted.
        operation: &'static str,
        /// Native failure detail.
        detail: String,
    },
}

impl From<NodeCreationFailure> for RpcError {
    fn from(value: NodeCreationFailure) -> Self {
        Self::Node(value)
    }
}

fn transport(operation: &'static str, detail: impl fmt::Display) -> RpcError {
    RpcError::TransportFailure {
        operation,
        detail: detail.to_string(),
    }
}

fn map_loan(error: LoanError) -> RpcError {
    match error {
        LoanError::ExceedsMaxLoans | LoanError::OutOfMemory => RpcError::LoanPoolExhausted(error),
        other => transport("loan", other),
    }
}

fn map_receive(operation: &'static str, error: ReceiveError) -> RpcError {
    match error {
        ReceiveError::ExceedsMaxBorrows => RpcError::BorrowLimitReached(error),
        other => transport(operation, other),
    }
}

fn map_request_send(error: RequestSendError) -> RpcError {
    match error {
        RequestSendError::ExceedsMaxActiveRequests => RpcError::ClientCapacityReached,
        RequestSendError::SendError(SendError::UnableToDeliver) => RpcError::ServerCapacityReached,
        RequestSendError::SendError(SendError::ConnectionBrokenSinceSenderNoLongerExists)
        | RequestSendError::SendError(SendError::ConnectionError(_)) => RpcError::ServerUnavailable,
        RequestSendError::SendError(SendError::LoanError(error)) => map_loan(error),
        RequestSendError::SendError(error) => transport("send request", error),
    }
}

fn record_request_send_failure(metrics: &ClientPressureState, error: &RequestSendError) {
    match error {
        RequestSendError::ExceedsMaxActiveRequests => metrics.client_capacity_rejected(),
        RequestSendError::SendError(SendError::UnableToDeliver) => {
            metrics.server_capacity_rejected()
        }
        RequestSendError::SendError(SendError::ConnectionBrokenSinceSenderNoLongerExists)
        | RequestSendError::SendError(SendError::ConnectionError(_)) => {
            metrics.server_unavailable()
        }
        _ => metrics.failed_before_start(),
    }
}

fn validate_descriptor(
    label: &'static str,
    expected: MessageTypeDescriptor,
    actual: &MessageTypeDescriptor,
) -> RpcResult<()> {
    if expected == *actual {
        return Ok(());
    }
    Err(RpcError::AbiMismatch {
        message: label.to_owned(),
        expected_abi_major: expected.abi_major,
        actual_abi_major: actual.abi_major,
        expected_layout: expected.layout_hash,
        actual_layout: actual.layout_hash,
        expected: Box::new(expected),
        actual: Box::new(actual.clone()),
    })
}

#[derive(Clone, Copy)]
struct RpcResourceBounds {
    response_listener_capacity: usize,
    event_max_nodes: usize,
}

fn validate_resources(resources: RpcResourceConfig) -> RpcResult<RpcResourceBounds> {
    for (field, value) in [
        ("max_in_flight", resources.max_in_flight),
        ("max_clients", resources.max_clients),
        ("max_request_loans", resources.max_request_loans),
        ("response_buffer_size", resources.response_buffer_size),
        ("max_borrowed_responses", resources.max_borrowed_responses),
        ("max_response_loans", resources.max_response_loans),
        ("max_request_slice_len", resources.max_request_slice_len),
        ("max_response_slice_len", resources.max_response_slice_len),
    ] {
        if value == 0 {
            return Err(RpcError::InvalidResourceConfig { field, value });
        }
    }
    let response_listener_capacity = resources
        .max_clients
        .checked_mul(resources.max_in_flight)
        .ok_or(RpcError::ResourceOverflow {
            field: "max_clients * max_in_flight",
        })?;
    let event_max_nodes =
        resources
            .max_clients
            .checked_add(1)
            .ok_or(RpcError::ResourceOverflow {
                field: "max_clients + 1",
            })?;
    Ok(RpcResourceBounds {
        response_listener_capacity,
        event_max_nodes,
    })
}

/// Validates both fixed payload ABIs, all bounded resource values, and the
/// RPC-specific readiness protocol before any native transport object is
/// created.
pub fn preflight_rpc<Q, R>(descriptor: &RpcDescriptor) -> RpcResult<()>
where
    Q: FixedMessageAbi,
    R: FixedMessageAbi,
{
    preflight_rpc_shapes(Q::descriptor(), R::descriptor(), descriptor)
}

fn preflight_rpc_shapes(
    request: MessageTypeDescriptor,
    response: MessageTypeDescriptor,
    descriptor: &RpcDescriptor,
) -> RpcResult<()> {
    validate_descriptor("request", request, descriptor.request_type())?;
    validate_descriptor("response", response, descriptor.response_type())?;
    if descriptor.readiness() != RpcReadinessProtocol::RpcSidecarV1 {
        return Err(RpcError::MissingTransportFeature {
            required: RpcReadinessProtocol::RpcSidecarV1,
            provider: descriptor.readiness(),
        });
    }
    validate_resources(descriptor.resources())?;
    Ok(())
}

/// Validates a fixed-request/slice-response endpoint before opening native
/// transport objects.
pub fn preflight_fixed_slice_rpc<Q, R>(descriptor: &RpcDescriptor) -> RpcResult<()>
where
    Q: FixedMessageAbi,
    R: SliceMessageAbi,
{
    preflight_rpc_shapes(Q::descriptor(), R::descriptor(), descriptor)
}

/// Validates a slice-request/fixed-response endpoint before opening native
/// transport objects.
pub fn preflight_slice_fixed_rpc<Q, R>(descriptor: &RpcDescriptor) -> RpcResult<()>
where
    Q: SliceMessageAbi,
    R: FixedMessageAbi,
{
    preflight_rpc_shapes(Q::descriptor(), R::descriptor(), descriptor)
}

/// Validates a slice-request/slice-response endpoint before opening native
/// transport objects.
pub fn preflight_slice_rpc<Q, R>(descriptor: &RpcDescriptor) -> RpcResult<()>
where
    Q: SliceMessageAbi,
    R: SliceMessageAbi,
{
    preflight_rpc_shapes(Q::descriptor(), R::descriptor(), descriptor)
}

fn service_name(name: &str) -> RpcResult<ServiceName> {
    Ok(name.try_into()?)
}

fn sidecar_name(descriptor: &RpcDescriptor, suffix: &str) -> RpcResult<ServiceName> {
    service_name(&format!("{}{suffix}", descriptor.service_name()))
}

fn event_open_or_create(
    node: &Node<Ipc>,
    name: &ServiceName,
    max_listeners: usize,
    max_notifiers: usize,
    max_nodes: usize,
) -> RpcResult<IoxEventFactory<Ipc>> {
    node.service_builder(name)
        .event()
        .max_listeners(max_listeners)
        .max_notifiers(max_notifiers)
        .max_nodes(max_nodes)
        .event_id_max_value(RPC_EVENT_ID.as_value())
        .open_or_create()
        .map_err(|error: EventOpenOrCreateError| RpcError::EventOpenOrCreate(error.to_string()))
}

fn event_open(
    node: &Node<Ipc>,
    name: &ServiceName,
    max_listeners: usize,
    max_notifiers: usize,
    max_nodes: usize,
) -> RpcResult<IoxEventFactory<Ipc>> {
    node.service_builder(name)
        .event()
        .max_listeners(max_listeners)
        .max_notifiers(max_notifiers)
        .max_nodes(max_nodes)
        .event_id_max_value(RPC_EVENT_ID.as_value())
        .open()
        .map_err(|error: EventOpenError| RpcError::EventOpen(error.to_string()))
}

fn create_listener(factory: &IoxEventFactory<Ipc>) -> RpcResult<IoxListener<Ipc>> {
    factory
        .listener_builder()
        .create()
        .map_err(RpcError::Listener)
}

fn create_notifier(factory: &IoxEventFactory<Ipc>) -> RpcResult<IoxNotifier<Ipc>> {
    factory
        .notifier_builder()
        .default_event_id(RPC_EVENT_ID)
        .create()
        .map_err(RpcError::Notifier)
}

fn notify(notifier: &IoxNotifier<Ipc>) {
    if let Err(error) = notifier.notify() {
        // A sidecar edge is advisory. A native response/request has already
        // established its own ownership and correlation semantics.
        tracing::debug!(?error, "RPC sidecar notification was not delivered");
    }
}

fn rr_builder<Q, R>(
    node: &Node<Ipc>,
    name: &ServiceName,
    resources: RpcResourceConfig,
) -> iceoryx2::service::builder::request_response::Builder<Q, (), R, (), Ipc>
where
    Q: fmt::Debug + ZeroCopySend,
    R: fmt::Debug + ZeroCopySend,
{
    rr_builder_with_headers::<Q, R, (), ()>(node, name, resources)
}

fn rr_builder_with_headers<Q, R, QH, RH>(
    node: &Node<Ipc>,
    name: &ServiceName,
    resources: RpcResourceConfig,
) -> iceoryx2::service::builder::request_response::Builder<Q, QH, R, RH, Ipc>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    node.service_builder(name)
        .request_response::<Q, R>()
        .request_user_header::<QH>()
        .response_user_header::<RH>()
        .enable_safe_overflow_for_requests(false)
        .enable_safe_overflow_for_responses(false)
        .enable_fire_and_forget_requests(false)
        .max_active_requests_per_client(resources.max_in_flight)
        .max_loaned_requests(resources.max_request_loans)
        .max_response_buffer_size(resources.response_buffer_size)
        .max_borrowed_responses_per_pending_response(resources.max_borrowed_responses)
        .max_servers(1)
        .max_clients(resources.max_clients)
        // validate_resources() has already checked this addition.
        .max_nodes(resources.max_clients + 1)
}

fn terminal_error(state: u8) -> RpcError {
    match state {
        TERMINAL_CANCELLED => RpcError::Cancelled,
        TERMINAL_EXPIRED => RpcError::ActivationExpired,
        TERMINAL_DEADLINE => RpcError::DeadlineExceeded,
        _ => transport("call state", "invalid terminal state"),
    }
}

const TERMINAL_OPEN: u8 = 0;
const TERMINAL_RESPONSE: u8 = 1;
const TERMINAL_CANCELLED: u8 = 2;
const TERMINAL_EXPIRED: u8 = 3;
const TERMINAL_DEADLINE: u8 = 4;

struct CallState {
    terminal: AtomicU8,
}

impl CallState {
    fn new() -> Self {
        Self {
            terminal: AtomicU8::new(TERMINAL_OPEN),
        }
    }

    fn load(&self) -> u8 {
        self.terminal.load(Ordering::Acquire)
    }

    fn claim(&self, terminal: u8) -> bool {
        self.terminal
            .compare_exchange(TERMINAL_OPEN, terminal, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
    }
}

struct ClientState {
    expired: AtomicBool,
    notifier: Arc<IoxNotifier<Ipc>>,
    // Wake handles and pending calls retain this state. Keeping the node here
    // prevents an externally retained handle from outliving the iceoryx2 node
    // that owns its cancellation notifier.
    node: Node<Ipc>,
}

impl ClientState {
    fn expire(&self) {
        self.expired.store(true, Ordering::Release);
        notify(&self.notifier);
    }
}

struct ServerState {
    expired: AtomicBool,
    notifier: Arc<IoxNotifier<Ipc>>,
    // The wake handle may outlive `RpcServer`; retain the owning node until
    // the final notifier-bearing state reference is gone.
    _node: Node<Ipc>,
}

impl ServerState {
    fn expire(&self) {
        self.expired.store(true, Ordering::Release);
        notify(&self.notifier);
    }
}

struct RpcClientInner<Q, R, QH = (), RH = ()>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    native: Mutex<NativeClient<Q, R, QH, RH>>,
    descriptor: RpcDescriptor,
    response_ready_name: ServiceName,
    client_cancel_name: ServiceName,
    response_listener_capacity: usize,
    cancel_listener_capacity: usize,
    event_max_nodes: usize,
    max_request_slice_len: usize,
    request_notifier: Arc<IoxNotifier<Ipc>>,
    metrics: Arc<ClientPressureState>,
    state: Arc<ClientState>,
}

impl<Q, R, QH, RH> fmt::Debug for RpcClientInner<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("RpcClientInner")
            .field("service_name", &self.descriptor.service_name())
            .field("expired", &self.state.expired.load(Ordering::Acquire))
            .finish_non_exhaustive()
    }
}

/// A typed native local RPC client.
pub struct RpcClient<Q, R, QH = (), RH = ()>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    inner: Arc<RpcClientInner<Q, R, QH, RH>>,
}

impl<Q, R, QH, RH> fmt::Debug for RpcClient<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("RpcClient")
            .field("inner", &self.inner)
            .finish()
    }
}

impl<Q, R> RpcClient<Q, R>
where
    Q: FixedMessageAbi + fmt::Debug + ZeroCopySend + Default,
    R: FixedMessageAbi + fmt::Debug + ZeroCopySend,
{
    /// Opens an existing fixed request/response endpoint after validating the
    /// complete descriptor and sidecar contract.
    pub fn open(descriptor: &RpcDescriptor) -> RpcResult<Self> {
        Self::open_with_identity(descriptor, None, None)
    }

    /// Opens a client whose low-rate metrics are keyed to a physical
    /// capability instance and the activation that owns this binding.
    pub fn open_for_activation(
        descriptor: &RpcDescriptor,
        capability_instance: CapabilityInstanceId,
        activation: ActivationId,
    ) -> RpcResult<Self> {
        Self::open_with_identity(descriptor, Some(capability_instance), Some(activation))
    }

    fn open_with_identity(
        descriptor: &RpcDescriptor,
        capability_instance: Option<CapabilityInstanceId>,
        activation: Option<ActivationId>,
    ) -> RpcResult<Self> {
        preflight_rpc::<Q, R>(descriptor)?;
        let resources = descriptor.resources();
        let bounds = validate_resources(resources)?;
        let listener_capacity = bounds.response_listener_capacity;
        let event_max_nodes = bounds.event_max_nodes;
        let service = service_name(descriptor.service_name())?;
        let request_ready_name = sidecar_name(descriptor, REQUEST_READY_SUFFIX)?;
        let response_ready_name = sidecar_name(descriptor, RESPONSE_READY_SUFFIX)?;
        let client_cancel_name = sidecar_name(descriptor, CLIENT_CANCEL_SUFFIX)?;
        let node = NodeBuilder::new()
            .signal_handling_mode(SignalHandlingMode::Disabled)
            .create::<Ipc>()?;

        // The client creates its request notifier before exposing the native
        // client. Response and cancellation listeners are created per call,
        // before that call's native request is sent.
        let request_event = event_open(
            &node,
            &request_ready_name,
            1,
            resources.max_clients,
            event_max_nodes,
        )?;
        let request_notifier = Arc::new(create_notifier(&request_event)?);
        drop(request_event);

        // Opening these services up front verifies the provider's exact
        // bounded sidecar capacities before the native client is created.
        let response_event = event_open(
            &node,
            &response_ready_name,
            listener_capacity,
            1,
            event_max_nodes,
        )?;
        drop(response_event);
        let cancel_event = event_open_or_create(
            &node,
            &client_cancel_name,
            listener_capacity,
            resources.max_clients,
            event_max_nodes,
        )?;
        let cancel_notifier = Arc::new(create_notifier(&cancel_event)?);
        drop(cancel_event);
        let factory = rr_builder::<Q, R>(&node, &service, resources)
            .open()
            .map_err(|error: RequestResponseOpenError| {
                RpcError::RequestResponseOpen(error.to_string())
            })?;
        let native = factory
            .client_builder()
            .backpressure_strategy(BackpressureStrategy::DiscardData)
            .create()
            .map_err(|error| RpcError::ClientCreate(error.to_string()))?;
        let state = Arc::new(ClientState {
            expired: AtomicBool::new(false),
            notifier: Arc::clone(&cancel_notifier),
            node,
        });
        let metrics = ClientPressureState::new(
            descriptor.service_name().to_owned(),
            resources,
            capability_instance,
            activation,
        );
        register_rpc_pressure_source(&metrics);

        Ok(Self {
            inner: Arc::new(RpcClientInner {
                native: Mutex::new(native),
                descriptor: descriptor.clone(),
                response_ready_name,
                client_cancel_name,
                response_listener_capacity: listener_capacity,
                cancel_listener_capacity: listener_capacity,
                event_max_nodes,
                max_request_slice_len: resources.max_request_slice_len,
                request_notifier,
                metrics,
                state,
            }),
        })
    }

    /// Alias for [`RpcClient::open`].
    pub fn connect(descriptor: &RpcDescriptor) -> RpcResult<Self> {
        Self::open(descriptor)
    }

    /// Copies a fixed request directly into one native loan and waits for one
    /// correlated response.
    pub fn call_copy(&self, request: Q, timeout: Duration) -> RpcResult<RpcResponseGuard<R>> {
        let mut loan = self.loan_request()?;
        *loan = request;
        loan.send()?.wait(timeout)
    }

    /// Short spelling for [`RpcClient::call_copy`].
    pub fn call(&self, request: Q, timeout: Duration) -> RpcResult<RpcResponseGuard<R>> {
        self.call_copy(request, timeout)
    }
}

impl<Q, R, QH, RH> RpcClient<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    /// Returns a clonable handle that expires pending and future calls.
    pub fn wake_handle(&self) -> RpcClientWakeHandle {
        RpcClientWakeHandle {
            state: Arc::clone(&self.inner.state),
        }
    }

    /// Expires this activation and wakes every pending call.
    pub fn expire(&self) -> RpcResult<()> {
        self.inner.state.expire();
        Ok(())
    }

    /// Alias for [`RpcClient::expire`].
    pub fn shutdown(&self) -> RpcResult<()> {
        self.expire()
    }

    /// Returns a point-in-time aggregate pressure snapshot for this client.
    pub fn pressure_snapshot(&self) -> RpcPressure {
        self.inner.metrics.snapshot()
    }
}

impl<Q, R, RH> RpcClient<Q, R, (), RH>
where
    Q: FixedMessageAbi + fmt::Debug + ZeroCopySend + Default,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    RH: fmt::Debug + ZeroCopySend,
{
    /// Loans a writable fixed-size request directly from the native shared
    /// memory request pool.
    pub fn loan_request(&self) -> RpcResult<RpcRequestLoan<Q, R, (), RH>> {
        if self.inner.state.expired.load(Ordering::Acquire) {
            return Err(RpcError::ActivationExpired);
        }
        let request = self
            .inner
            .native
            .lock()
            .map_err(|_| transport("loan request", "native client mutex poisoned"))?
            .loan()
            .map_err(|error| {
                self.inner.metrics.request_loan_failed();
                map_loan(error)
            })?;
        Ok(RpcRequestLoan {
            request: Some(request),
            client: Arc::clone(&self.inner),
        })
    }
}

impl<E, R, QH, RH> RpcClient<[E], R, QH, RH>
where
    E: Default + fmt::Debug + ZeroCopySend + 'static,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: Default + fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    /// Loans an initialized request slice directly in shared memory.
    pub fn loan_request_slice(
        &self,
        element_count: usize,
    ) -> RpcResult<RpcRequestLoan<[E], R, QH, RH>> {
        if self.inner.state.expired.load(Ordering::Acquire) {
            return Err(RpcError::ActivationExpired);
        }
        if element_count > self.inner.max_request_slice_len {
            return Err(RpcError::InvalidSliceLength {
                direction: "request",
                requested: element_count,
                maximum: self.inner.max_request_slice_len,
            });
        }
        let request = self
            .inner
            .native
            .lock()
            .map_err(|_| transport("loan request slice", "native client mutex poisoned"))?
            .loan_slice(element_count)
            .map_err(|error| {
                self.inner.metrics.request_loan_failed();
                map_loan(error)
            })?;
        Ok(RpcRequestLoan {
            request: Some(request),
            client: Arc::clone(&self.inner),
        })
    }

    /// Alias for [`Self::loan_request_slice`].
    pub fn loan_slice(&self, element_count: usize) -> RpcResult<RpcRequestLoan<[E], R, QH, RH>> {
        self.loan_request_slice(element_count)
    }
}

impl<Q, R, QH, RH> Drop for RpcClient<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    fn drop(&mut self) {
        self.inner.state.expire();
    }
}

/// A clonable wake/expiry handle for a client activation.
#[derive(Clone)]
pub struct RpcClientWakeHandle {
    state: Arc<ClientState>,
}

impl fmt::Debug for RpcClientWakeHandle {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("RpcClientWakeHandle")
            .field("expired", &self.is_expired())
            .finish()
    }
}

impl RpcClientWakeHandle {
    /// Expires the client activation and wakes all pending calls.
    pub fn wake(&self) -> RpcResult<()> {
        self.state.expire();
        Ok(())
    }

    /// Alias for [`RpcClientWakeHandle::wake`].
    pub fn expire(&self) -> RpcResult<()> {
        self.wake()
    }

    /// Returns whether expiry has been requested.
    pub fn is_expired(&self) -> bool {
        self.state.expired.load(Ordering::Acquire)
    }
}

/// A writable request loan. Dropping it before [`Self::send`] returns the
/// native request capacity without publishing any request.
pub struct RpcRequestLoan<Q, R, QH = (), RH = ()>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    request: Option<NativeRequest<Q, R, QH, RH>>,
    client: Arc<RpcClientInner<Q, R, QH, RH>>,
}

impl<Q, R, QH, RH> fmt::Debug for RpcRequestLoan<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("RpcRequestLoan")
            .field("sent", &self.request.is_none())
            .finish_non_exhaustive()
    }
}

impl<Q, R, QH, RH> Deref for RpcRequestLoan<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
    NativeRequest<Q, R, QH, RH>: Deref<Target = Q>,
{
    type Target = Q;

    fn deref(&self) -> &Self::Target {
        Deref::deref(
            self.request
                .as_ref()
                .expect("RPC request loan payload accessed after send"),
        )
    }
}

impl<Q, R, QH, RH> DerefMut for RpcRequestLoan<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
    NativeRequest<Q, R, QH, RH>: DerefMut<Target = Q>,
{
    fn deref_mut(&mut self) -> &mut Self::Target {
        DerefMut::deref_mut(
            self.request
                .as_mut()
                .expect("RPC request loan payload accessed after send"),
        )
    }
}

pub trait PendingResponseReceive<
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: ?Sized + fmt::Debug + ZeroCopySend + IceoryxSend,
    QH,
    RH: fmt::Debug + ZeroCopySend,
>
{
    fn receive_rpc(&self) -> Result<Option<NativeResponse<R, RH>>, ReceiveError>;
}

impl<Q, R, QH, RH> PendingResponseReceive<Q, R, QH, RH> for NativePending<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + Sized,
    QH: Default + fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    fn receive_rpc(&self) -> Result<Option<NativeResponse<R, RH>>, ReceiveError> {
        self.receive()
    }
}

impl<Q, E, QH, RH> PendingResponseReceive<Q, [E], QH, RH> for NativePending<Q, [E], QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    E: fmt::Debug + ZeroCopySend,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    fn receive_rpc(&self) -> Result<Option<NativeResponse<[E], RH>>, ReceiveError> {
        self.receive()
    }
}

struct StartedRpcCall<Q, R, QH = (), RH = ()>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    pending: NativePending<Q, R, QH, RH>,
    response_listener: IoxListener<Ipc>,
    cancellation_listener: IoxListener<Ipc>,
    client_state: Arc<ClientState>,
    metrics: ClientCallGuard,
}

impl<Q, R, QH, RH> RpcRequestLoan<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    fn begin_send(mut self) -> RpcResult<StartedRpcCall<Q, R, QH, RH>> {
        self.client.metrics.attempted();
        if self.client.state.expired.load(Ordering::Acquire) {
            self.client.metrics.activation_expired_before_start();
            return Err(RpcError::ActivationExpired);
        }
        let response_listener = open_call_listener(
            &self.client.metrics,
            &self.client.state.node,
            &self.client.response_ready_name,
            self.client.response_listener_capacity,
            self.client.event_max_nodes,
        )?;
        let cancellation_listener = open_call_listener(
            &self.client.metrics,
            &self.client.state.node,
            &self.client.client_cancel_name,
            self.client.cancel_listener_capacity,
            self.client.event_max_nodes,
        )?;
        if self.client.state.expired.load(Ordering::Acquire) {
            self.client.metrics.activation_expired_before_start();
            return Err(RpcError::ActivationExpired);
        }
        let request = self
            .request
            .take()
            .expect("RPC request loan send called twice");
        let pending = {
            // The native request loan/send operation is the only serialized
            // client section. Pending waits never hold this mutex.
            let _client = self.client.native.lock().map_err(|_| {
                self.client.metrics.failed_before_start();
                transport("send request", "native client mutex poisoned")
            })?;
            match request.send() {
                Ok(pending) => pending,
                Err(error) => {
                    record_request_send_failure(&self.client.metrics, &error);
                    return Err(map_request_send(error));
                }
            }
        };
        let connections = pending.number_of_server_connections();
        if connections == 0 {
            self.client.metrics.server_unavailable();
            drop(pending);
            return Err(RpcError::ServerUnavailable);
        }
        let mut metrics = self.client.metrics.started();
        if self.client.state.expired.load(Ordering::Acquire) {
            metrics.finish(ClientTerminal::ActivationExpired);
            drop(pending);
            return Err(RpcError::ActivationExpired);
        }
        notify(&self.client.request_notifier);
        Ok(StartedRpcCall {
            pending,
            response_listener,
            cancellation_listener,
            client_state: Arc::clone(&self.client.state),
            metrics,
        })
    }

    /// Sends the native request and returns its native-correlated pending
    /// response handle. Per-call sidecar listeners are established before the
    /// native send.
    pub fn send(self) -> RpcResult<PendingRpcCall<Q, R, QH, RH>>
    where
        NativePending<Q, R, QH, RH>: PendingResponseReceive<Q, R, QH, RH>,
    {
        let started = self.begin_send()?;
        // A reused native response channel can contain a late response from a
        // prior timed-out call. Native PendingResponse::receive() filters by
        // the native request id and releases mismatches. Preserve a current
        // response, if one raced with the send, so it still wins terminal
        // arbitration in the returned call.
        let pre_received =
            PendingResponseReceive::receive_rpc(&started.pending).map_err(|error| {
                started.metrics.response_receive_failed();
                map_receive("receive RPC response", error)
            })?;
        Ok(PendingRpcCall {
            pending: Some(started.pending),
            pre_received,
            response_listener: started.response_listener,
            cancellation_listener: started.cancellation_listener,
            client_state: started.client_state,
            state: Arc::new(CallState::new()),
            metrics: started.metrics,
            _request: PhantomData,
        })
    }

    /// Borrows the native request user header.
    pub fn user_header(&self) -> &QH {
        self.request
            .as_ref()
            .expect("RPC request loan user header accessed after send")
            .user_header()
    }

    /// Mutably borrows the native request user header.
    pub fn user_header_mut(&mut self) -> &mut QH {
        self.request
            .as_mut()
            .expect("RPC request loan user header accessed after send")
            .user_header_mut()
    }
}

fn open_listener(
    node: &Node<Ipc>,
    name: &ServiceName,
    max_listeners: usize,
    max_notifiers: usize,
    max_nodes: usize,
) -> RpcResult<IoxListener<Ipc>> {
    let factory = event_open(node, name, max_listeners, max_notifiers, max_nodes)?;
    create_listener(&factory)
}

/// Opens a per-call sidecar listener. Listener capacity is exactly
/// `max_clients * max_in_flight` and every client is natively bounded to
/// `max_in_flight` active requests, so an exhausted listener budget means
/// this client already has its maximum number of calls in flight.
fn open_call_listener(
    metrics: &ClientPressureState,
    node: &Node<Ipc>,
    name: &ServiceName,
    max_listeners: usize,
    max_nodes: usize,
) -> RpcResult<IoxListener<Ipc>> {
    match open_listener(node, name, max_listeners, 1, max_nodes) {
        Ok(listener) => Ok(listener),
        Err(RpcError::Listener(ListenerCreateError::ExceedsMaxSupportedListeners)) => {
            metrics.client_capacity_rejected();
            Err(RpcError::ClientCapacityReached)
        }
        Err(error) => {
            metrics.failed_before_start();
            Err(error)
        }
    }
}

fn open_client_inner<Q, R, QH, RH, O, F>(
    descriptor: &RpcDescriptor,
    capability_instance: Option<CapabilityInstanceId>,
    activation: Option<ActivationId>,
    open_factory: O,
    create_client: F,
) -> RpcResult<RpcClient<Q, R, QH, RH>>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
    O: FnOnce(
        &Node<Ipc>,
        &ServiceName,
        RpcResourceConfig,
    ) -> RpcResult<NativeFactory<Q, R, QH, RH>>,
    F: FnOnce(&NativeFactory<Q, R, QH, RH>) -> RpcResult<NativeClient<Q, R, QH, RH>>,
{
    let resources = descriptor.resources();
    let bounds = validate_resources(resources)?;
    let listener_capacity = bounds.response_listener_capacity;
    let event_max_nodes = bounds.event_max_nodes;
    let service = service_name(descriptor.service_name())?;
    let request_ready_name = sidecar_name(descriptor, REQUEST_READY_SUFFIX)?;
    let response_ready_name = sidecar_name(descriptor, RESPONSE_READY_SUFFIX)?;
    let client_cancel_name = sidecar_name(descriptor, CLIENT_CANCEL_SUFFIX)?;
    let node = NodeBuilder::new()
        .signal_handling_mode(SignalHandlingMode::Disabled)
        .create::<Ipc>()?;

    let request_event = event_open(
        &node,
        &request_ready_name,
        1,
        resources.max_clients,
        event_max_nodes,
    )?;
    let request_notifier = Arc::new(create_notifier(&request_event)?);
    drop(request_event);

    let response_event = event_open(
        &node,
        &response_ready_name,
        listener_capacity,
        1,
        event_max_nodes,
    )?;
    drop(response_event);
    let cancel_event = event_open_or_create(
        &node,
        &client_cancel_name,
        listener_capacity,
        resources.max_clients,
        event_max_nodes,
    )?;
    let cancel_notifier = Arc::new(create_notifier(&cancel_event)?);
    drop(cancel_event);
    let factory = open_factory(&node, &service, resources)?;
    let native = create_client(&factory)?;
    let state = Arc::new(ClientState {
        expired: AtomicBool::new(false),
        notifier: Arc::clone(&cancel_notifier),
        node,
    });
    let metrics = ClientPressureState::new(
        descriptor.service_name().to_owned(),
        resources,
        capability_instance,
        activation,
    );
    register_rpc_pressure_source(&metrics);

    Ok(RpcClient {
        inner: Arc::new(RpcClientInner {
            native: Mutex::new(native),
            descriptor: descriptor.clone(),
            response_ready_name,
            client_cancel_name,
            response_listener_capacity: listener_capacity,
            cancel_listener_capacity: listener_capacity,
            event_max_nodes,
            max_request_slice_len: resources.max_request_slice_len,
            request_notifier,
            metrics,
            state,
        }),
    })
}

/// A pending, native-correlated RPC call.
pub struct PendingRpcCall<Q, R, QH = (), RH = ()>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    pending: Option<NativePending<Q, R, QH, RH>>,
    pre_received: Option<NativeResponse<R, RH>>,
    response_listener: IoxListener<Ipc>,
    cancellation_listener: IoxListener<Ipc>,
    client_state: Arc<ClientState>,
    state: Arc<CallState>,
    metrics: ClientCallGuard,
    _request: PhantomData<fn() -> Q>,
}

impl<Q, R, QH, RH> fmt::Debug for PendingRpcCall<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("PendingRpcCall")
            .field("terminal", &self.state.load())
            .finish_non_exhaustive()
    }
}

impl<Q, R, QH, RH> PendingRpcCall<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
    NativePending<Q, R, QH, RH>: PendingResponseReceive<Q, R, QH, RH>,
{
    /// Returns the native response sidecar as a source for an external
    /// [`WaitContext`](crate::WaitContext).  The source carries readiness only;
    /// callers must use [`Self::try_wait`] to acquire the correlated response.
    pub fn response_event_source(&self, token: WaitToken) -> EventSource<'_> {
        EventSource::new(token, &self.response_listener)
    }

    /// Returns the native cancellation sidecar as a source for an external
    /// [`WaitContext`](crate::WaitContext).  This is useful to multiplex
    /// pending calls without opening a duplicate sidecar listener.
    pub fn cancellation_event_source(&self, token: WaitToken) -> EventSource<'_> {
        EventSource::new(token, &self.cancellation_listener)
    }

    /// Drains the response sidecar's coalesced activations, if any were queued.
    pub fn consume_response_event(&self) -> RpcResult<bool> {
        consume_sidecar_activations(&self.response_listener)
            .map_err(|error| transport("consume RPC response event", error))
    }

    /// Drains the cancellation sidecar's coalesced activations, if any were queued.
    pub fn consume_cancellation_event(&self) -> RpcResult<bool> {
        consume_sidecar_activations(&self.cancellation_listener)
            .map_err(|error| transport("consume RPC cancellation event", error))
    }

    /// Returns a handle that can cancel this call from another thread.
    pub fn cancel_handle(&self) -> RpcCallCancelHandle {
        RpcCallCancelHandle {
            state: Arc::clone(&self.state),
            client_state: Arc::clone(&self.client_state),
        }
    }

    /// Returns true after a cancellation request has won the terminal-state
    /// race for this call.
    pub fn is_cancelled(&self) -> bool {
        self.state.load() == TERMINAL_CANCELLED
    }

    /// Performs one nonblocking native response probe.
    pub fn try_wait(&mut self) -> RpcResult<Option<RpcResponseGuard<R, RH>>> {
        if self.client_state.expired.load(Ordering::Acquire) && self.state.claim(TERMINAL_EXPIRED) {
            return Err(RpcError::ActivationExpired);
        }
        if self.state.load() != TERMINAL_OPEN {
            return Err(terminal_error(self.state.load()));
        }
        if let Some(response) = self.pre_received.take() {
            return self.finish_response(response).map(Some);
        }
        match self.poll_response()? {
            Some(response) => self.finish_response(response).map(Some),
            None => Ok(None),
        }
    }

    /// Waits for one response using a native WaitSet, an initial bounded
    /// probe, and a low-frequency safety interval. `Duration` is interpreted
    /// against the process monotonic clock.
    pub fn wait(mut self, timeout: Duration) -> RpcResult<RpcResponseGuard<R, RH>> {
        let started = Instant::now();

        loop {
            if self.client_state.expired.load(Ordering::Acquire)
                && self.state.claim(TERMINAL_EXPIRED)
            {
                return Err(RpcError::ActivationExpired);
            }
            if self.state.load() != TERMINAL_OPEN {
                return Err(terminal_error(self.state.load()));
            }

            if let Some(response) = self.pre_received.take() {
                return self.finish_response(response);
            }

            // A sidecar event is advisory. Always probe native correlation at
            // the start of each iteration and before claiming a deadline.
            if let Some(response) = self.poll_response()? {
                return self.finish_response(response);
            }

            let remaining = timeout.saturating_sub(started.elapsed());
            if remaining.is_zero() {
                // Final receive/check wins over a deadline when the response
                // is already visible in the native channel.
                if let Some(response) = self.poll_response()? {
                    return self.finish_response(response);
                }
                if self.state.claim(TERMINAL_DEADLINE) {
                    return Err(RpcError::DeadlineExceeded);
                }
                return Err(terminal_error(self.state.load()));
            }

            enum WaitOutcome<T> {
                Response(T),
                Cancelled,
                Expired,
                Continue,
            }

            let outcome = {
                let waitset = WaitSetBuilder::new()
                    .signal_handling_mode(SignalHandlingMode::Disabled)
                    .create::<Ipc>()
                    .map_err(|error| transport("create RPC WaitSet", error))?;
                let response_guard = waitset
                    .attach_notification(&self.response_listener)
                    .map_err(|error| transport("attach RPC response listener", error))?;
                let cancellation_guard =
                    waitset
                        .attach_notification(&self.cancellation_listener)
                        .map_err(|error| transport("attach RPC cancellation listener", error))?;
                let interval_guard = waitset
                    .attach_interval(RPC_WAIT_INTERVAL)
                    .map_err(|error| transport("attach RPC WaitSet interval", error))?;

                // Initial bounded native event drains close the create/attach
                // race. Sidecar activations are advisory; native correlation
                // is still checked below.
                let _ = consume_sidecar_activations(&self.response_listener)
                    .map_err(|error| transport("probe RPC response listener", error))?;
                let _ = consume_sidecar_activations(&self.cancellation_listener)
                    .map_err(|error| transport("probe RPC cancellation listener", error))?;
                if self.client_state.expired.load(Ordering::Acquire) {
                    WaitOutcome::Expired
                } else if self.state.load() == TERMINAL_CANCELLED {
                    WaitOutcome::Cancelled
                } else if let Some(response) = self.poll_response()? {
                    WaitOutcome::Response(response)
                } else {
                    let mut response = None;
                    let mut terminal = None;
                    let mut callback_error = None;
                    let state = Arc::clone(&self.state);
                    let client_state = Arc::clone(&self.client_state);
                    let pending = self.pending.as_ref().expect("pending RPC response");
                    let metrics = &self.metrics;
                    let response_listener = &self.response_listener;
                    let cancellation_listener = &self.cancellation_listener;
                    let wait_result = waitset
                        .wait_and_process_once_with_timeout(
                            |attachment_id: WaitSetAttachmentId<Ipc>| {
                                if attachment_id.has_event_from(&cancellation_guard) {
                                    match consume_sidecar_activations(cancellation_listener) {
                                        Ok(_) => {
                                            if client_state.expired.load(Ordering::Acquire) {
                                                state.claim(TERMINAL_EXPIRED);
                                                terminal = Some(WaitOutcome::Expired);
                                                CallbackProgression::Stop
                                            } else if state.load() == TERMINAL_CANCELLED {
                                                terminal = Some(WaitOutcome::Cancelled);
                                                CallbackProgression::Stop
                                            } else {
                                                CallbackProgression::Continue
                                            }
                                        }
                                        Err(error) => {
                                            callback_error = Some(transport(
                                                "consume RPC cancellation event",
                                                error,
                                            ));
                                            CallbackProgression::Stop
                                        }
                                    }
                                } else if attachment_id.has_event_from(&response_guard) {
                                    match consume_sidecar_activations(response_listener) {
                                        Ok(_) => {
                                            match PendingResponseReceive::receive_rpc(pending) {
                                                Ok(Some(value)) => {
                                                    response = Some(value);
                                                    CallbackProgression::Stop
                                                }
                                                Ok(None) => CallbackProgression::Continue,
                                                Err(error) => {
                                                    metrics.response_receive_failed();
                                                    callback_error = Some(map_receive(
                                                        "receive RPC response",
                                                        error,
                                                    ));
                                                    CallbackProgression::Stop
                                                }
                                            }
                                        }
                                        Err(error) => {
                                            callback_error = Some(transport(
                                                "consume RPC response event",
                                                error,
                                            ));
                                            CallbackProgression::Stop
                                        }
                                    }
                                } else if attachment_id.has_event_from(&interval_guard) {
                                    match PendingResponseReceive::receive_rpc(pending) {
                                        Ok(Some(value)) => {
                                            response = Some(value);
                                            CallbackProgression::Stop
                                        }
                                        Ok(None) => CallbackProgression::Continue,
                                        Err(error) => {
                                            metrics.response_receive_failed();
                                            callback_error =
                                                Some(map_receive("probe RPC response", error));
                                            CallbackProgression::Stop
                                        }
                                    }
                                } else {
                                    CallbackProgression::Continue
                                }
                            },
                            remaining,
                        )
                        .map_err(|error| transport("run RPC WaitSet", error));
                    wait_result?;
                    if let Some(error) = callback_error {
                        return Err(error);
                    }
                    if let Some(value) = response {
                        WaitOutcome::Response(value)
                    } else if let Some(value) = terminal {
                        value
                    } else {
                        WaitOutcome::Continue
                    }
                }
            };

            match outcome {
                WaitOutcome::Response(response) => return self.finish_response(response),
                WaitOutcome::Cancelled => return Err(RpcError::Cancelled),
                WaitOutcome::Expired => return Err(RpcError::ActivationExpired),
                WaitOutcome::Continue => {
                    if started.elapsed() >= timeout {
                        // The next loop performs the final native probe and
                        // linearizes the deadline.
                        continue;
                    }
                }
            }
        }
    }

    fn poll_response(&self) -> RpcResult<Option<NativeResponse<R, RH>>> {
        PendingResponseReceive::receive_rpc(self.pending.as_ref().expect("pending RPC response"))
            .map_err(|error| {
                self.metrics.response_receive_failed();
                map_receive("receive RPC response", error)
            })
    }

    fn finish_response(
        &mut self,
        response: NativeResponse<R, RH>,
    ) -> RpcResult<RpcResponseGuard<R, RH>> {
        if self.state.claim(TERMINAL_RESPONSE) {
            // Dropping the PendingResponse closes the native request channel;
            // the returned Response owns its borrowed sample independently.
            self.pending.take();
            self.metrics.finish(ClientTerminal::Completed);
            Ok(RpcResponseGuard {
                response,
                _client_state: Arc::clone(&self.client_state),
                _metrics: self.metrics.response_guard(),
            })
        } else {
            drop(response);
            Err(terminal_error(self.state.load()))
        }
    }
}

impl<Q, R, QH, RH> Drop for PendingRpcCall<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    fn drop(&mut self) {
        let terminal = match self.state.load() {
            TERMINAL_RESPONSE => ClientTerminal::Completed,
            TERMINAL_CANCELLED => ClientTerminal::Cancelled,
            TERMINAL_EXPIRED => ClientTerminal::ActivationExpired,
            TERMINAL_DEADLINE => ClientTerminal::TimedOut,
            _ => ClientTerminal::Failed,
        };
        self.metrics.finish(terminal);
    }
}

/// A handle that can cancel a pending call from another thread.
#[derive(Clone)]
pub struct RpcCallCancelHandle {
    state: Arc<CallState>,
    client_state: Arc<ClientState>,
}

impl fmt::Debug for RpcCallCancelHandle {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("RpcCallCancelHandle")
            .field("cancelled", &self.is_cancelled())
            .finish()
    }
}

impl RpcCallCancelHandle {
    /// Claims cancellation only when the call is still open.  A `false`
    /// result means the native response/deadline/expiry terminal state has
    /// already won the race.
    pub fn try_cancel(&self) -> bool {
        let claimed = self.state.claim(TERMINAL_CANCELLED);
        if claimed {
            notify(&self.client_state.notifier);
        }
        claimed
    }

    /// Requests cancellation. The first terminal winner determines the
    /// result; repeated cancellation is harmless.
    pub fn cancel(&self) -> RpcResult<()> {
        let _ = self.try_cancel();
        Ok(())
    }

    /// Returns whether this call has been cancelled.
    pub fn is_cancelled(&self) -> bool {
        self.state.load() == TERMINAL_CANCELLED
    }
}

/// A zero-copy response sample guard.
pub struct RpcResponseGuard<R, RH = ()>
where
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    RH: fmt::Debug + ZeroCopySend,
{
    response: NativeResponse<R, RH>,
    // Retain the native node until the borrowed response sample is released.
    _client_state: Arc<ClientState>,
    // Track response ownership through native and Python-derived views.
    _metrics: crate::rpc_pressure::ClientResponseGuard,
}

impl<R, RH> fmt::Debug for RpcResponseGuard<R, RH>
where
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    RH: fmt::Debug + ZeroCopySend,
{
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.response.fmt(formatter)
    }
}

impl<R, RH> Deref for RpcResponseGuard<R, RH>
where
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    RH: fmt::Debug + ZeroCopySend,
    NativeResponse<R, RH>: Deref<Target = R>,
{
    type Target = R;

    fn deref(&self) -> &Self::Target {
        Deref::deref(&self.response)
    }
}

impl<R, RH> RpcResponseGuard<R, RH>
where
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    RH: fmt::Debug + ZeroCopySend,
    NativeResponse<R, RH>: Deref<Target = R>,
{
    /// Returns the borrowed native payload.
    pub fn payload(&self) -> &R {
        Deref::deref(&self.response)
    }

    /// Borrows the native response user header.
    pub fn user_header(&self) -> &RH {
        self.response.user_header()
    }
}

struct RpcServerInner {
    state: Arc<ServerState>,
}

fn open_server_inner<Q, R, QH, RH, O, F>(
    descriptor: &RpcDescriptor,
    capability_instance: Option<CapabilityInstanceId>,
    open_factory: O,
    create_server: F,
) -> RpcResult<RpcServer<Q, R, QH, RH>>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
    O: FnOnce(
        &Node<Ipc>,
        &ServiceName,
        RpcResourceConfig,
    ) -> RpcResult<NativeFactory<Q, R, QH, RH>>,
    F: FnOnce(&NativeFactory<Q, R, QH, RH>) -> RpcResult<NativeServer<Q, R, QH, RH>>,
{
    let resources = descriptor.resources();
    let bounds = validate_resources(resources)?;
    let listener_capacity = bounds.response_listener_capacity;
    let event_max_nodes = bounds.event_max_nodes;
    let service = service_name(descriptor.service_name())?;
    let request_ready_name = sidecar_name(descriptor, REQUEST_READY_SUFFIX)?;
    let response_ready_name = sidecar_name(descriptor, RESPONSE_READY_SUFFIX)?;
    let server_cancel_name = sidecar_name(descriptor, SERVER_CANCEL_SUFFIX)?;
    let node = NodeBuilder::new()
        .signal_handling_mode(SignalHandlingMode::Disabled)
        .create::<Ipc>()?;

    let request_event = event_open_or_create(
        &node,
        &request_ready_name,
        1,
        resources.max_clients,
        event_max_nodes,
    )?;
    let request_listener = create_listener(&request_event)?;
    drop(request_event);

    let response_event = event_open_or_create(
        &node,
        &response_ready_name,
        listener_capacity,
        1,
        event_max_nodes,
    )?;
    let response_notifier = Arc::new(create_notifier(&response_event)?);
    drop(response_event);

    let server_cancel_event =
        event_open_or_create(&node, &server_cancel_name, 1, 1, event_max_nodes)?;
    let cancellation_listener = create_listener(&server_cancel_event)?;
    let cancellation_notifier = Arc::new(create_notifier(&server_cancel_event)?);
    drop(server_cancel_event);

    let factory = open_factory(&node, &service, resources)?;
    let native = create_server(&factory)?;
    let state = Arc::new(ServerState {
        expired: AtomicBool::new(false),
        notifier: cancellation_notifier,
        _node: node,
    });
    let inner = Arc::new(RpcServerInner { state });
    let metrics = ServerPressureState::new(
        descriptor.service_name().to_owned(),
        resources,
        capability_instance,
    );
    register_rpc_pressure_source(&metrics);

    Ok(RpcServer {
        native,
        request_listener,
        cancellation_listener,
        response_notifier,
        metrics,
        inner,
        max_response_slice_len: resources.max_response_slice_len,
    })
}

/// A typed native local RPC server.
pub struct RpcServer<Q, R, QH = (), RH = ()>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    native: NativeServer<Q, R, QH, RH>,
    request_listener: IoxListener<Ipc>,
    cancellation_listener: IoxListener<Ipc>,
    response_notifier: Arc<IoxNotifier<Ipc>>,
    metrics: Arc<ServerPressureState>,
    inner: Arc<RpcServerInner>,
    max_response_slice_len: usize,
}

impl<Q, R, QH, RH> fmt::Debug for RpcServer<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("RpcServer")
            .field("expired", &self.inner.state.expired.load(Ordering::Acquire))
            .finish_non_exhaustive()
    }
}

impl<Q, R> RpcServer<Q, R>
where
    Q: FixedMessageAbi + fmt::Debug + ZeroCopySend,
    R: FixedMessageAbi + fmt::Debug + ZeroCopySend + Default,
{
    /// Opens or creates a native server endpoint and its request/response
    /// sidecars. The request listener and response notifier are established
    /// before the native server port.
    pub fn open_or_create(descriptor: &RpcDescriptor) -> RpcResult<Self> {
        Self::open_or_create_with_identity(descriptor, None)
    }

    /// Opens or creates a server whose metrics are keyed to a physical
    /// capability publication instance.
    pub fn open_or_create_for_capability(
        descriptor: &RpcDescriptor,
        capability_instance: CapabilityInstanceId,
    ) -> RpcResult<Self> {
        Self::open_or_create_with_identity(descriptor, Some(capability_instance))
    }

    fn open_or_create_with_identity(
        descriptor: &RpcDescriptor,
        capability_instance: Option<CapabilityInstanceId>,
    ) -> RpcResult<Self> {
        preflight_rpc::<Q, R>(descriptor)?;
        let resources = descriptor.resources();
        let bounds = validate_resources(resources)?;
        let listener_capacity = bounds.response_listener_capacity;
        let event_max_nodes = bounds.event_max_nodes;
        let service = service_name(descriptor.service_name())?;
        let request_ready_name = sidecar_name(descriptor, REQUEST_READY_SUFFIX)?;
        let response_ready_name = sidecar_name(descriptor, RESPONSE_READY_SUFFIX)?;
        let server_cancel_name = sidecar_name(descriptor, SERVER_CANCEL_SUFFIX)?;
        let node = NodeBuilder::new()
            .signal_handling_mode(SignalHandlingMode::Disabled)
            .create::<Ipc>()?;

        let request_event = event_open_or_create(
            &node,
            &request_ready_name,
            1,
            resources.max_clients,
            event_max_nodes,
        )?;
        let request_listener = create_listener(&request_event)?;
        drop(request_event);

        let response_event = event_open_or_create(
            &node,
            &response_ready_name,
            listener_capacity,
            1,
            event_max_nodes,
        )?;
        let response_notifier = Arc::new(create_notifier(&response_event)?);
        drop(response_event);

        let server_cancel_event =
            event_open_or_create(&node, &server_cancel_name, 1, 1, event_max_nodes)?;
        let cancellation_listener = create_listener(&server_cancel_event)?;
        let cancellation_notifier = Arc::new(create_notifier(&server_cancel_event)?);
        drop(server_cancel_event);
        let factory = rr_builder::<Q, R>(&node, &service, resources)
            .open_or_create()
            .map_err(|error: RequestResponseOpenOrCreateError| {
                RpcError::RequestResponseOpenOrCreate(error.to_string())
            })?;
        let native = factory
            .server_builder()
            .backpressure_strategy(BackpressureStrategy::DiscardData)
            .max_loaned_responses_per_request(resources.max_response_loans)
            .create()
            .map_err(|error| RpcError::ServerCreate(error.to_string()))?;
        let state = Arc::new(ServerState {
            expired: AtomicBool::new(false),
            notifier: cancellation_notifier,
            _node: node,
        });
        let inner = Arc::new(RpcServerInner { state });
        let metrics = ServerPressureState::new(
            descriptor.service_name().to_owned(),
            resources,
            capability_instance,
        );
        register_rpc_pressure_source(&metrics);

        Ok(Self {
            native,
            request_listener,
            cancellation_listener,
            response_notifier,
            metrics,
            inner,
            max_response_slice_len: resources.max_response_slice_len,
        })
    }

    /// Alias for [`RpcServer::open_or_create`].
    pub fn open(descriptor: &RpcDescriptor) -> RpcResult<Self> {
        Self::open_or_create(descriptor)
    }

    /// Alias for [`RpcServer::open_or_create`].
    pub fn create(descriptor: &RpcDescriptor) -> RpcResult<Self> {
        Self::open_or_create(descriptor)
    }
}

pub trait NativeServerReceive<
    Q: ?Sized + fmt::Debug + ZeroCopySend + IceoryxSend,
    R: ?Sized + fmt::Debug + ZeroCopySend + IceoryxSend,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
>
{
    fn receive_rpc(&self) -> Result<Option<NativeActive<Q, R, QH, RH>>, ReceiveError>;
}

impl<Q, R, QH, RH> NativeServerReceive<Q, R, QH, RH> for NativeServer<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    fn receive_rpc(&self) -> Result<Option<NativeActive<Q, R, QH, RH>>, ReceiveError> {
        self.receive()
    }
}

impl<E, R, QH, RH> NativeServerReceive<[E], R, QH, RH> for NativeServer<[E], R, QH, RH>
where
    E: fmt::Debug + ZeroCopySend,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    fn receive_rpc(&self) -> Result<Option<NativeActive<[E], R, QH, RH>>, ReceiveError> {
        self.receive()
    }
}

impl<Q, R, QH, RH> RpcServer<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
    NativeServer<Q, R, QH, RH>: NativeServerReceive<Q, R, QH, RH>,
{
    /// Returns the server's existing request sidecar as a source for an
    /// external [`WaitContext`](crate::WaitContext).  No second listener is
    /// opened; the source borrows the listener owned by this endpoint.
    pub fn request_event_source(&self, token: WaitToken) -> EventSource<'_> {
        EventSource::new(token, &self.request_listener)
    }

    /// Drains the request sidecar's coalesced activations, if any were queued.
    pub fn consume_request_event(&self) -> RpcResult<bool> {
        consume_sidecar_activations(&self.request_listener)
            .map_err(|error| transport("consume RPC request event", error))
    }

    /// Returns a clonable wake/expiry handle for the serving task.
    pub fn wake_handle(&self) -> RpcServerWakeHandle {
        RpcServerWakeHandle {
            state: Arc::clone(&self.inner.state),
        }
    }

    /// Expires this serving endpoint and wakes a blocked receive.
    pub fn expire(&self) -> RpcResult<()> {
        self.inner.state.expire();
        Ok(())
    }

    /// Alias for [`RpcServer::expire`].
    pub fn shutdown(&self) -> RpcResult<()> {
        self.expire()
    }

    /// Returns a point-in-time aggregate pressure snapshot for this server.
    pub fn pressure_snapshot(&self) -> RpcPressure {
        self.metrics.snapshot()
    }

    /// Attempts to receive one request without waiting.
    pub fn try_receive(&mut self) -> RpcResult<Option<RpcRequest<Q, R, QH, RH>>> {
        if self.inner.state.expired.load(Ordering::Acquire) {
            return Err(RpcError::ActivationExpired);
        }
        match NativeServerReceive::receive_rpc(&self.native) {
            Ok(request) => Ok(request.map(|request| RpcRequest {
                request: Some(request),
                response_notifier: Arc::clone(&self.response_notifier),
                metrics: self.metrics.request_received(),
                server_state: Arc::clone(&self.inner.state),
                max_response_slice_len: self.max_response_slice_len,
            })),
            Err(error) => {
                self.metrics.request_borrow_failed();
                Err(map_receive("receive RPC request", error))
            }
        }
    }

    /// Receives one request using a native WaitSet and monotonic timeout.
    pub fn receive_timeout(
        &mut self,
        timeout: Duration,
    ) -> RpcResult<Option<RpcRequest<Q, R, QH, RH>>> {
        let started = Instant::now();
        loop {
            if self.inner.state.expired.load(Ordering::Acquire) {
                return Err(RpcError::ActivationExpired);
            }
            if let Some(request) = self.try_receive()? {
                return Ok(Some(request));
            }
            let remaining = timeout.saturating_sub(started.elapsed());
            if remaining.is_zero() {
                return Ok(None);
            }

            let waitset = WaitSetBuilder::new()
                .signal_handling_mode(SignalHandlingMode::Disabled)
                .create::<Ipc>()
                .map_err(|error| transport("create RPC server WaitSet", error))?;
            let request_guard = waitset
                .attach_notification(&self.request_listener)
                .map_err(|error| transport("attach RPC request listener", error))?;
            let cancellation_guard = waitset
                .attach_notification(&self.cancellation_listener)
                .map_err(|error| transport("attach RPC server cancellation listener", error))?;
            let interval_guard = waitset
                .attach_interval(RPC_WAIT_INTERVAL)
                .map_err(|error| transport("attach RPC server WaitSet interval", error))?;
            let mut request_ready = false;
            let mut cancelled = false;
            let mut callback_error = None;
            let expired = Arc::clone(&self.inner.state);
            let request_listener = &self.request_listener;
            let cancellation_listener = &self.cancellation_listener;
            waitset
                .wait_and_process_once_with_timeout(
                    |attachment_id: WaitSetAttachmentId<Ipc>| {
                        if attachment_id.has_event_from(&cancellation_guard) {
                            match consume_sidecar_activations(cancellation_listener) {
                                Ok(_) => {
                                    if expired.expired.load(Ordering::Acquire) {
                                        cancelled = true;
                                        CallbackProgression::Stop
                                    } else {
                                        CallbackProgression::Continue
                                    }
                                }
                                Err(error) => {
                                    callback_error = Some(transport(
                                        "consume RPC server cancellation event",
                                        error,
                                    ));
                                    CallbackProgression::Stop
                                }
                            }
                        } else if attachment_id.has_event_from(&request_guard) {
                            match consume_sidecar_activations(request_listener) {
                                Ok(_) => {
                                    request_ready = true;
                                    CallbackProgression::Stop
                                }
                                Err(error) => {
                                    callback_error =
                                        Some(transport("consume RPC request event", error));
                                    CallbackProgression::Stop
                                }
                            }
                        } else if attachment_id.has_event_from(&interval_guard) {
                            request_ready = true;
                            CallbackProgression::Stop
                        } else {
                            CallbackProgression::Continue
                        }
                    },
                    remaining,
                )
                .map_err(|error| transport("run RPC server WaitSet", error))?;
            drop(request_guard);
            drop(cancellation_guard);
            drop(interval_guard);
            if let Some(error) = callback_error {
                return Err(error);
            }
            if cancelled {
                return Err(RpcError::ActivationExpired);
            }
            let request = if request_ready {
                self.try_receive()?
            } else {
                None
            };
            if let Some(request) = request {
                return Ok(Some(request));
            }
            if started.elapsed() >= timeout {
                return Ok(None);
            }
        }
    }

    /// Receives one request without a caller-supplied deadline.
    pub fn receive(&mut self) -> RpcResult<RpcRequest<Q, R, QH, RH>> {
        loop {
            if let Some(request) = self.receive_timeout(Duration::from_secs(1))? {
                return Ok(request);
            }
        }
    }
}

impl<Q, R, QH, RH> Drop for RpcServer<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    fn drop(&mut self) {
        self.inner.state.expire();
    }
}

/// A clonable wake/expiry handle for a server activation.
#[derive(Clone)]
pub struct RpcServerWakeHandle {
    state: Arc<ServerState>,
}

impl fmt::Debug for RpcServerWakeHandle {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("RpcServerWakeHandle")
            .field("expired", &self.is_expired())
            .finish()
    }
}

impl RpcServerWakeHandle {
    /// Expires the server and wakes a blocked receive.
    pub fn wake(&self) -> RpcResult<()> {
        self.state.expire();
        Ok(())
    }

    /// Alias for [`RpcServerWakeHandle::wake`].
    pub fn expire(&self) -> RpcResult<()> {
        self.wake()
    }

    /// Returns whether server expiry was requested.
    pub fn is_expired(&self) -> bool {
        self.state.expired.load(Ordering::Acquire)
    }
}

/// A read-only request sample received from the native server.
pub struct RpcRequest<Q, R, QH = (), RH = ()>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    request: Option<NativeActive<Q, R, QH, RH>>,
    response_notifier: Arc<IoxNotifier<Ipc>>,
    metrics: ServerRequestGuard,
    server_state: Arc<ServerState>,
    max_response_slice_len: usize,
}

impl<Q, R, QH, RH> fmt::Debug for RpcRequest<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("RpcRequest")
            .field("received", &self.request.is_some())
            .finish_non_exhaustive()
    }
}

impl<Q, R, QH, RH> Deref for RpcRequest<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
    NativeActive<Q, R, QH, RH>: Deref<Target = Q>,
{
    type Target = Q;

    fn deref(&self) -> &Self::Target {
        Deref::deref(
            self.request
                .as_ref()
                .expect("RPC request accessed after response loan"),
        )
    }
}

impl<Q, R> RpcRequest<Q, R>
where
    Q: FixedMessageAbi + fmt::Debug + ZeroCopySend,
    R: FixedMessageAbi + fmt::Debug + ZeroCopySend + Default,
{
    /// Consumes this request and loans exactly one writable response. Holding
    /// the native ActiveRequest in the returned loan keeps the request channel
    /// alive until the response is sent or dropped.
    pub fn loan_response(mut self) -> RpcResult<RpcResponseLoan<Q, R>> {
        if self.server_state.expired.load(Ordering::Acquire) {
            return Err(RpcError::ActivationExpired);
        }
        self.metrics.response_attempted();
        let request = self.request.take().expect("RPC response already loaned");
        let response = request.loan().map_err(|error| {
            self.metrics.response_loan_failed();
            map_loan(error)
        })?;
        Ok(RpcResponseLoan {
            request: Some(request),
            response: Some(response),
            response_notifier: self.response_notifier,
            metrics: self.metrics,
            server_state: self.server_state,
        })
    }
}

impl<Q, R, QH, RH> RpcRequest<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    /// Borrows the native request user header.
    pub fn user_header(&self) -> &QH {
        self.request
            .as_ref()
            .expect("RPC request user header accessed after response loan")
            .user_header()
    }

    fn take_response_parts(mut self) -> RpcResult<ResponseLoanParts<Q, R, QH, RH>> {
        if self.server_state.expired.load(Ordering::Acquire) {
            return Err(RpcError::ActivationExpired);
        }
        self.metrics.response_attempted();
        Ok(ResponseLoanParts {
            request: self.request.take().expect("RPC response already loaned"),
            response_notifier: self.response_notifier,
            metrics: self.metrics,
            server_state: self.server_state,
        })
    }
}

struct ResponseLoanParts<Q, R, QH = (), RH = ()>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    request: NativeActive<Q, R, QH, RH>,
    response_notifier: Arc<IoxNotifier<Ipc>>,
    metrics: ServerRequestGuard,
    server_state: Arc<ServerState>,
}

/// A writable response loan that owns the native request until send/drop.
pub struct RpcResponseLoan<Q, R, QH = (), RH = ()>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    request: Option<NativeActive<Q, R, QH, RH>>,
    response: Option<NativeResponseLoan<R, RH>>,
    response_notifier: Arc<IoxNotifier<Ipc>>,
    metrics: ServerRequestGuard,
    server_state: Arc<ServerState>,
}

impl<Q, R, QH, RH> fmt::Debug for RpcResponseLoan<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("RpcResponseLoan")
            .field("sent", &self.response.is_none())
            .finish_non_exhaustive()
    }
}

impl<Q, R, QH, RH> Deref for RpcResponseLoan<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
    NativeResponseLoan<R, RH>: Deref<Target = R>,
{
    type Target = R;

    fn deref(&self) -> &Self::Target {
        Deref::deref(
            self.response
                .as_ref()
                .expect("RPC response loan accessed after send"),
        )
    }
}

impl<Q, R, QH, RH> DerefMut for RpcResponseLoan<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
    NativeResponseLoan<R, RH>: DerefMut<Target = R>,
{
    fn deref_mut(&mut self) -> &mut Self::Target {
        DerefMut::deref_mut(
            self.response
                .as_mut()
                .expect("RPC response loan accessed after send"),
        )
    }
}

impl<Q, R, QH, RH> RpcResponseLoan<Q, R, QH, RH>
where
    Q: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    R: fmt::Debug + ZeroCopySend + IceoryxSend + ?Sized,
    QH: fmt::Debug + ZeroCopySend,
    RH: fmt::Debug + ZeroCopySend,
{
    /// Borrows the native response user header.
    pub fn user_header(&self) -> &RH {
        self.response
            .as_ref()
            .expect("RPC response user header accessed after send")
            .user_header()
    }

    /// Mutably borrows the native response user header.
    pub fn user_header_mut(&mut self) -> &mut RH {
        self.response
            .as_mut()
            .expect("RPC response user header accessed after send")
            .user_header_mut()
    }

    /// Sends the one response. Consuming this value prevents a second
    /// response attempt through the typed API.
    pub fn send(mut self) -> RpcResult<()> {
        if self.server_state.expired.load(Ordering::Acquire) {
            return Err(RpcError::ActivationExpired);
        }
        let response = self.response.take().expect("RPC response loan sent twice");
        let result = response.send();
        // The request owner is dropped after the native response send and
        // closes the server-side response channel.
        self.request.take();
        match result {
            Ok(()) => {
                self.metrics.response_sent();
                self.metrics.responded();
                notify(&self.response_notifier);
                Ok(())
            }
            Err(SendError::UnableToDeliver) => {
                self.metrics.response_failed();
                self.metrics.responded();
                Err(RpcError::ResponseCapacityReached)
            }
            Err(SendError::ConnectionBrokenSinceSenderNoLongerExists)
            | Err(SendError::ConnectionError(_)) => {
                self.metrics.response_failed();
                self.metrics.responded();
                Err(RpcError::ServerUnavailable)
            }
            Err(SendError::LoanError(error)) => {
                self.metrics.response_failed();
                self.metrics.responded();
                map_loan(error).pipe(Err)
            }
            Err(error) => {
                self.metrics.response_failed();
                self.metrics.responded();
                Err(transport("send response", error))
            }
        }
    }
}

impl<Q, E, RH> RpcRequest<Q, [E], (), RH>
where
    Q: FixedMessageAbi + fmt::Debug + ZeroCopySend,
    E: Default + fmt::Debug + ZeroCopySend + 'static,
    RH: Default + fmt::Debug + ZeroCopySend,
{
    /// Loans one initialized response slice directly in shared memory.
    pub fn loan_response_slice(
        self,
        element_count: usize,
    ) -> RpcResult<RpcResponseLoan<Q, [E], (), RH>> {
        let maximum = self.max_response_slice_len;
        if element_count > maximum {
            return Err(RpcError::InvalidSliceLength {
                direction: "response",
                requested: element_count,
                maximum,
            });
        }
        let parts = self.take_response_parts()?;
        let response = parts.request.loan_slice(element_count).map_err(|error| {
            parts.metrics.response_loan_failed();
            map_loan(error)
        })?;
        Ok(RpcResponseLoan {
            request: Some(parts.request),
            response: Some(response),
            response_notifier: parts.response_notifier,
            metrics: parts.metrics,
            server_state: parts.server_state,
        })
    }

    /// Alias for [`Self::loan_response_slice`].
    pub fn loan_response(self, element_count: usize) -> RpcResult<RpcResponseLoan<Q, [E], (), RH>> {
        self.loan_response_slice(element_count)
    }
}

impl<E, R, QH> RpcRequest<[E], R, QH, ()>
where
    E: Default + fmt::Debug + ZeroCopySend + 'static,
    QH: Default + fmt::Debug + ZeroCopySend,
    R: FixedMessageAbi + fmt::Debug + ZeroCopySend + Default,
{
    /// Loans one initialized fixed response directly in shared memory.
    pub fn loan_response(self) -> RpcResult<RpcResponseLoan<[E], R, QH, ()>> {
        let parts = self.take_response_parts()?;
        let response = parts.request.loan().map_err(|error| {
            parts.metrics.response_loan_failed();
            map_loan(error)
        })?;
        Ok(RpcResponseLoan {
            request: Some(parts.request),
            response: Some(response),
            response_notifier: parts.response_notifier,
            metrics: parts.metrics,
            server_state: parts.server_state,
        })
    }
}

impl<QE, RE, QH, RH> RpcRequest<[QE], [RE], QH, RH>
where
    QE: Default + fmt::Debug + ZeroCopySend,
    RE: Default + fmt::Debug + ZeroCopySend + 'static,
    QH: Default + fmt::Debug + ZeroCopySend,
    RH: Default + fmt::Debug + ZeroCopySend,
{
    /// Loans one initialized response slice directly in shared memory.
    pub fn loan_response_slice(
        self,
        element_count: usize,
    ) -> RpcResult<RpcResponseLoan<[QE], [RE], QH, RH>> {
        let maximum = self.max_response_slice_len;
        if element_count > maximum {
            return Err(RpcError::InvalidSliceLength {
                direction: "response",
                requested: element_count,
                maximum,
            });
        }
        let parts = self.take_response_parts()?;
        let response = parts.request.loan_slice(element_count).map_err(|error| {
            parts.metrics.response_loan_failed();
            map_loan(error)
        })?;
        Ok(RpcResponseLoan {
            request: Some(parts.request),
            response: Some(response),
            response_notifier: parts.response_notifier,
            metrics: parts.metrics,
            server_state: parts.server_state,
        })
    }

    /// Alias for [`Self::loan_response_slice`].
    pub fn loan_response(
        self,
        element_count: usize,
    ) -> RpcResult<RpcResponseLoan<[QE], [RE], QH, RH>> {
        self.loan_response_slice(element_count)
    }
}

/// Type-level namespace for fixed-size typed RPC endpoints.
pub struct Rpc<Q, R>(PhantomData<fn() -> (Q, R)>);

impl<Q, R> Rpc<Q, R>
where
    Q: FixedMessageAbi,
    R: FixedMessageAbi,
{
    /// Creates a descriptor for a fixed request/response endpoint.
    pub fn descriptor(service_name: impl Into<String>) -> RpcDescriptor {
        RpcDescriptor::for_fixed::<Q, R>(service_name)
    }

    /// Opens an existing client endpoint.
    pub fn client(descriptor: &RpcDescriptor) -> RpcResult<RpcClient<Q, R>>
    where
        Q: fmt::Debug + ZeroCopySend + Default,
        R: fmt::Debug + ZeroCopySend,
    {
        RpcClient::open(descriptor)
    }

    /// Opens or creates a server endpoint.
    pub fn server(descriptor: &RpcDescriptor) -> RpcResult<RpcServer<Q, R>>
    where
        Q: fmt::Debug + ZeroCopySend,
        R: fmt::Debug + ZeroCopySend + Default,
    {
        RpcServer::open_or_create(descriptor)
    }
}

/// Client endpoint for a fixed request and dynamic slice response.
pub struct FixedSliceRpcClient<Q, M>
where
    Q: fmt::Debug + ZeroCopySend,
    M: SliceMessageAbi,
    M::Element: fmt::Debug + ZeroCopySend,
    M::Header: fmt::Debug + ZeroCopySend,
{
    inner: RpcClient<Q, [M::Element], (), M::Header>,
}

impl<Q, M> Deref for FixedSliceRpcClient<Q, M>
where
    Q: fmt::Debug + ZeroCopySend,
    M: SliceMessageAbi,
    M::Element: fmt::Debug + ZeroCopySend,
    M::Header: fmt::Debug + ZeroCopySend,
{
    type Target = RpcClient<Q, [M::Element], (), M::Header>;

    fn deref(&self) -> &Self::Target {
        &self.inner
    }
}

impl<Q, M> FixedSliceRpcClient<Q, M>
where
    Q: FixedMessageAbi + fmt::Debug + ZeroCopySend + Default,
    M: SliceMessageAbi,
    M::Header: Default + fmt::Debug + ZeroCopySend,
    M::Element: Default + fmt::Debug + ZeroCopySend,
{
    /// Opens an existing fixed-request/slice-response client.
    pub fn open(descriptor: &RpcDescriptor) -> RpcResult<Self> {
        Self::open_with_identity(descriptor, None, None)
    }

    /// Opens a client with activation identity.
    pub fn open_for_activation(
        descriptor: &RpcDescriptor,
        capability_instance: CapabilityInstanceId,
        activation: ActivationId,
    ) -> RpcResult<Self> {
        Self::open_with_identity(descriptor, Some(capability_instance), Some(activation))
    }

    fn open_with_identity(
        descriptor: &RpcDescriptor,
        capability_instance: Option<CapabilityInstanceId>,
        activation: Option<ActivationId>,
    ) -> RpcResult<Self> {
        preflight_fixed_slice_rpc::<Q, M>(descriptor)?;
        let inner = open_client_inner::<Q, [M::Element], (), M::Header, _, _>(
            descriptor,
            capability_instance,
            activation,
            |node, service, resources| {
                rr_builder_with_headers::<Q, [M::Element], (), M::Header>(node, service, resources)
                    .open()
                    .map_err(|error: RequestResponseOpenError| {
                        RpcError::RequestResponseOpen(error.to_string())
                    })
            },
            |factory| {
                factory
                    .client_builder()
                    .backpressure_strategy(BackpressureStrategy::DiscardData)
                    .create()
                    .map_err(|error| RpcError::ClientCreate(error.to_string()))
            },
        )?;
        Ok(Self { inner })
    }

    /// Alias for [`Self::open`].
    pub fn connect(descriptor: &RpcDescriptor) -> RpcResult<Self> {
        Self::open(descriptor)
    }
}

/// Server endpoint for a fixed request and dynamic slice response.
pub struct FixedSliceRpcServer<Q, M>
where
    Q: fmt::Debug + ZeroCopySend,
    M: SliceMessageAbi,
    M::Element: fmt::Debug + ZeroCopySend,
    M::Header: fmt::Debug + ZeroCopySend,
{
    inner: RpcServer<Q, [M::Element], (), M::Header>,
}

impl<Q, M> Deref for FixedSliceRpcServer<Q, M>
where
    Q: fmt::Debug + ZeroCopySend,
    M: SliceMessageAbi,
    M::Element: fmt::Debug + ZeroCopySend,
    M::Header: fmt::Debug + ZeroCopySend,
{
    type Target = RpcServer<Q, [M::Element], (), M::Header>;

    fn deref(&self) -> &Self::Target {
        &self.inner
    }
}

impl<Q, M> DerefMut for FixedSliceRpcServer<Q, M>
where
    Q: fmt::Debug + ZeroCopySend,
    M: SliceMessageAbi,
    M::Element: fmt::Debug + ZeroCopySend,
    M::Header: fmt::Debug + ZeroCopySend,
{
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.inner
    }
}

impl<Q, M> FixedSliceRpcServer<Q, M>
where
    Q: FixedMessageAbi + fmt::Debug + ZeroCopySend,
    M: SliceMessageAbi,
    M::Header: Default + fmt::Debug + ZeroCopySend,
    M::Element: Default + fmt::Debug + ZeroCopySend,
{
    /// Opens or creates a fixed-request/slice-response server.
    pub fn open_or_create(descriptor: &RpcDescriptor) -> RpcResult<Self> {
        Self::open_or_create_with_identity(descriptor, None)
    }

    /// Opens or creates a server with capability identity.
    pub fn open_or_create_for_capability(
        descriptor: &RpcDescriptor,
        capability_instance: CapabilityInstanceId,
    ) -> RpcResult<Self> {
        Self::open_or_create_with_identity(descriptor, Some(capability_instance))
    }

    fn open_or_create_with_identity(
        descriptor: &RpcDescriptor,
        capability_instance: Option<CapabilityInstanceId>,
    ) -> RpcResult<Self> {
        preflight_fixed_slice_rpc::<Q, M>(descriptor)?;
        let max_slice_len = descriptor.resources().max_response_slice_len;
        let inner = open_server_inner::<Q, [M::Element], (), M::Header, _, _>(
            descriptor,
            capability_instance,
            |node, service, resources| {
                rr_builder_with_headers::<Q, [M::Element], (), M::Header>(node, service, resources)
                    .open_or_create()
                    .map_err(|error: RequestResponseOpenOrCreateError| {
                        RpcError::RequestResponseOpenOrCreate(error.to_string())
                    })
            },
            |factory| {
                factory
                    .server_builder()
                    .initial_max_slice_len(max_slice_len)
                    .allocation_strategy(AllocationStrategy::Static)
                    .backpressure_strategy(BackpressureStrategy::DiscardData)
                    .max_loaned_responses_per_request(descriptor.resources().max_response_loans)
                    .create()
                    .map_err(|error| RpcError::ServerCreate(error.to_string()))
            },
        )?;
        Ok(Self { inner })
    }

    /// Alias for [`Self::open_or_create`].
    pub fn open(descriptor: &RpcDescriptor) -> RpcResult<Self> {
        Self::open_or_create(descriptor)
    }

    /// Alias for [`Self::open_or_create`].
    pub fn create(descriptor: &RpcDescriptor) -> RpcResult<Self> {
        Self::open_or_create(descriptor)
    }
}

/// Request loan type for a fixed request and dynamic slice response.
pub type FixedSliceRpcRequestLoan<Q, M> =
    RpcRequestLoan<Q, [<M as SliceMessageAbi>::Element], (), <M as SliceMessageAbi>::Header>;
/// Response guard type for a fixed request and dynamic slice response.
pub type FixedSliceRpcResponseGuard<M> =
    RpcResponseGuard<[<M as SliceMessageAbi>::Element], <M as SliceMessageAbi>::Header>;

/// Type-level namespace for fixed-request/slice-response RPC endpoints.
pub struct FixedSliceRpc<Q, R>(PhantomData<fn() -> (Q, R)>);

impl<Q, R> FixedSliceRpc<Q, R>
where
    Q: FixedMessageAbi,
    R: SliceMessageAbi,
{
    /// Creates a descriptor for a fixed-request/slice-response endpoint.
    pub fn descriptor(service_name: impl Into<String>) -> RpcDescriptor {
        RpcDescriptor::for_fixed_slice::<Q, R>(service_name)
    }

    /// Validates the endpoint descriptor before transport creation.
    pub fn preflight(descriptor: &RpcDescriptor) -> RpcResult<()> {
        preflight_fixed_slice_rpc::<Q, R>(descriptor)
    }
}

impl<Q, R> FixedSliceRpc<Q, R>
where
    Q: FixedMessageAbi + fmt::Debug + ZeroCopySend + Default,
    R: SliceMessageAbi,
    R::Header: Default + fmt::Debug + ZeroCopySend,
    R::Element: Default + fmt::Debug + ZeroCopySend,
{
    /// Opens the client endpoint.
    pub fn client(descriptor: &RpcDescriptor) -> RpcResult<FixedSliceRpcClient<Q, R>> {
        FixedSliceRpcClient::<Q, R>::open(descriptor)
    }

    /// Opens or creates the server endpoint.
    pub fn server(descriptor: &RpcDescriptor) -> RpcResult<FixedSliceRpcServer<Q, R>> {
        FixedSliceRpcServer::<Q, R>::open_or_create(descriptor)
    }
}

/// Client endpoint for a dynamic slice request and fixed response.
pub struct SliceFixedRpcClient<M, R>
where
    M: SliceMessageAbi,
    M::Element: fmt::Debug + ZeroCopySend,
    M::Header: fmt::Debug + ZeroCopySend,
    R: fmt::Debug + ZeroCopySend,
{
    inner: RpcClient<[M::Element], R, M::Header, ()>,
}

impl<M, R> Deref for SliceFixedRpcClient<M, R>
where
    M: SliceMessageAbi,
    M::Element: fmt::Debug + ZeroCopySend,
    M::Header: fmt::Debug + ZeroCopySend,
    R: fmt::Debug + ZeroCopySend,
{
    type Target = RpcClient<[M::Element], R, M::Header, ()>;

    fn deref(&self) -> &Self::Target {
        &self.inner
    }
}

impl<M, R> SliceFixedRpcClient<M, R>
where
    M: SliceMessageAbi,
    M::Header: Default + fmt::Debug + ZeroCopySend,
    M::Element: Default + fmt::Debug + ZeroCopySend,
    R: FixedMessageAbi + fmt::Debug + ZeroCopySend,
{
    /// Opens an existing slice-request/fixed-response client.
    pub fn open(descriptor: &RpcDescriptor) -> RpcResult<Self> {
        Self::open_with_identity(descriptor, None, None)
    }

    /// Opens a client with activation identity.
    pub fn open_for_activation(
        descriptor: &RpcDescriptor,
        capability_instance: CapabilityInstanceId,
        activation: ActivationId,
    ) -> RpcResult<Self> {
        Self::open_with_identity(descriptor, Some(capability_instance), Some(activation))
    }

    fn open_with_identity(
        descriptor: &RpcDescriptor,
        capability_instance: Option<CapabilityInstanceId>,
        activation: Option<ActivationId>,
    ) -> RpcResult<Self> {
        preflight_slice_fixed_rpc::<M, R>(descriptor)?;
        let max_slice_len = descriptor.resources().max_request_slice_len;
        let inner = open_client_inner::<[M::Element], R, M::Header, (), _, _>(
            descriptor,
            capability_instance,
            activation,
            |node, service, resources| {
                rr_builder_with_headers::<[M::Element], R, M::Header, ()>(node, service, resources)
                    .open()
                    .map_err(|error: RequestResponseOpenError| {
                        RpcError::RequestResponseOpen(error.to_string())
                    })
            },
            |factory| {
                factory
                    .client_builder()
                    .initial_max_slice_len(max_slice_len)
                    .allocation_strategy(AllocationStrategy::Static)
                    .backpressure_strategy(BackpressureStrategy::DiscardData)
                    .create()
                    .map_err(|error| RpcError::ClientCreate(error.to_string()))
            },
        )?;
        Ok(Self { inner })
    }

    /// Alias for [`Self::open`].
    pub fn connect(descriptor: &RpcDescriptor) -> RpcResult<Self> {
        Self::open(descriptor)
    }
}

/// Server endpoint for a dynamic slice request and fixed response.
pub struct SliceFixedRpcServer<M, R>
where
    M: SliceMessageAbi,
    M::Element: fmt::Debug + ZeroCopySend,
    M::Header: fmt::Debug + ZeroCopySend,
    R: fmt::Debug + ZeroCopySend,
{
    inner: RpcServer<[M::Element], R, M::Header, ()>,
}

impl<M, R> Deref for SliceFixedRpcServer<M, R>
where
    M: SliceMessageAbi,
    M::Element: fmt::Debug + ZeroCopySend,
    M::Header: fmt::Debug + ZeroCopySend,
    R: fmt::Debug + ZeroCopySend,
{
    type Target = RpcServer<[M::Element], R, M::Header, ()>;

    fn deref(&self) -> &Self::Target {
        &self.inner
    }
}

impl<M, R> DerefMut for SliceFixedRpcServer<M, R>
where
    M: SliceMessageAbi,
    M::Element: fmt::Debug + ZeroCopySend,
    M::Header: fmt::Debug + ZeroCopySend,
    R: fmt::Debug + ZeroCopySend,
{
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.inner
    }
}

impl<M, R> SliceFixedRpcServer<M, R>
where
    M: SliceMessageAbi,
    M::Header: Default + fmt::Debug + ZeroCopySend,
    M::Element: Default + fmt::Debug + ZeroCopySend,
    R: FixedMessageAbi + fmt::Debug + ZeroCopySend + Default,
{
    /// Opens or creates a slice-request/fixed-response server.
    pub fn open_or_create(descriptor: &RpcDescriptor) -> RpcResult<Self> {
        Self::open_or_create_with_identity(descriptor, None)
    }

    /// Opens or creates a server with capability identity.
    pub fn open_or_create_for_capability(
        descriptor: &RpcDescriptor,
        capability_instance: CapabilityInstanceId,
    ) -> RpcResult<Self> {
        Self::open_or_create_with_identity(descriptor, Some(capability_instance))
    }

    fn open_or_create_with_identity(
        descriptor: &RpcDescriptor,
        capability_instance: Option<CapabilityInstanceId>,
    ) -> RpcResult<Self> {
        preflight_slice_fixed_rpc::<M, R>(descriptor)?;
        let inner = open_server_inner::<[M::Element], R, M::Header, (), _, _>(
            descriptor,
            capability_instance,
            |node, service, resources| {
                rr_builder_with_headers::<[M::Element], R, M::Header, ()>(node, service, resources)
                    .open_or_create()
                    .map_err(|error: RequestResponseOpenOrCreateError| {
                        RpcError::RequestResponseOpenOrCreate(error.to_string())
                    })
            },
            |factory| {
                factory
                    .server_builder()
                    .backpressure_strategy(BackpressureStrategy::DiscardData)
                    .max_loaned_responses_per_request(descriptor.resources().max_response_loans)
                    .create()
                    .map_err(|error| RpcError::ServerCreate(error.to_string()))
            },
        )?;
        Ok(Self { inner })
    }

    /// Alias for [`Self::open_or_create`].
    pub fn open(descriptor: &RpcDescriptor) -> RpcResult<Self> {
        Self::open_or_create(descriptor)
    }

    /// Alias for [`Self::open_or_create`].
    pub fn create(descriptor: &RpcDescriptor) -> RpcResult<Self> {
        Self::open_or_create(descriptor)
    }
}

/// Request loan type for a dynamic slice request and fixed response.
pub type SliceFixedRpcRequestLoan<M, R> =
    RpcRequestLoan<[<M as SliceMessageAbi>::Element], R, <M as SliceMessageAbi>::Header, ()>;
/// Response guard type for a dynamic slice request and fixed response.
pub type SliceFixedRpcResponseGuard<R> = RpcResponseGuard<R>;

/// Type-level namespace for slice-request/fixed-response RPC endpoints.
pub struct SliceFixedRpc<Q, R>(PhantomData<fn() -> (Q, R)>);

impl<Q, R> SliceFixedRpc<Q, R>
where
    Q: SliceMessageAbi,
    R: FixedMessageAbi,
{
    /// Creates a descriptor for a slice-request/fixed-response endpoint.
    pub fn descriptor(service_name: impl Into<String>) -> RpcDescriptor {
        RpcDescriptor::for_slice_fixed::<Q, R>(service_name)
    }

    /// Validates the endpoint descriptor before transport creation.
    pub fn preflight(descriptor: &RpcDescriptor) -> RpcResult<()> {
        preflight_slice_fixed_rpc::<Q, R>(descriptor)
    }
}

impl<Q, R> SliceFixedRpc<Q, R>
where
    Q: SliceMessageAbi,
    Q::Header: Default + fmt::Debug + ZeroCopySend,
    Q::Element: Default + fmt::Debug + ZeroCopySend,
    R: FixedMessageAbi + fmt::Debug + ZeroCopySend + Default,
{
    /// Opens the client endpoint.
    pub fn client(descriptor: &RpcDescriptor) -> RpcResult<SliceFixedRpcClient<Q, R>> {
        SliceFixedRpcClient::<Q, R>::open(descriptor)
    }

    /// Opens or creates the server endpoint.
    pub fn server(descriptor: &RpcDescriptor) -> RpcResult<SliceFixedRpcServer<Q, R>> {
        SliceFixedRpcServer::<Q, R>::open_or_create(descriptor)
    }
}

/// Client endpoint for a dynamic slice request and dynamic slice response.
type SliceRpcRawClient<E, F, QH, RH> = RpcClient<[E], [F], QH, RH>;
type SliceRpcRawServer<E, F, QH, RH> = RpcServer<[E], [F], QH, RH>;

pub struct SliceRpcClient<Q, R>
where
    Q: SliceMessageAbi,
    Q::Element: fmt::Debug + ZeroCopySend,
    Q::Header: fmt::Debug + ZeroCopySend,
    R: SliceMessageAbi,
    R::Element: fmt::Debug + ZeroCopySend,
    R::Header: fmt::Debug + ZeroCopySend,
{
    inner: SliceRpcRawClient<Q::Element, R::Element, Q::Header, R::Header>,
}

impl<Q, R> Deref for SliceRpcClient<Q, R>
where
    Q: SliceMessageAbi,
    Q::Element: fmt::Debug + ZeroCopySend,
    Q::Header: fmt::Debug + ZeroCopySend,
    R: SliceMessageAbi,
    R::Element: fmt::Debug + ZeroCopySend,
    R::Header: fmt::Debug + ZeroCopySend,
{
    type Target = SliceRpcRawClient<Q::Element, R::Element, Q::Header, R::Header>;

    fn deref(&self) -> &Self::Target {
        &self.inner
    }
}

impl<Q, R> SliceRpcClient<Q, R>
where
    Q: SliceMessageAbi,
    Q::Header: Default + fmt::Debug + ZeroCopySend,
    Q::Element: Default + fmt::Debug + ZeroCopySend,
    R: SliceMessageAbi,
    R::Header: Default + fmt::Debug + ZeroCopySend,
    R::Element: Default + fmt::Debug + ZeroCopySend,
{
    /// Opens an existing slice-request/slice-response client.
    pub fn open(descriptor: &RpcDescriptor) -> RpcResult<Self> {
        Self::open_with_identity(descriptor, None, None)
    }

    /// Opens a client with activation identity.
    pub fn open_for_activation(
        descriptor: &RpcDescriptor,
        capability_instance: CapabilityInstanceId,
        activation: ActivationId,
    ) -> RpcResult<Self> {
        Self::open_with_identity(descriptor, Some(capability_instance), Some(activation))
    }

    fn open_with_identity(
        descriptor: &RpcDescriptor,
        capability_instance: Option<CapabilityInstanceId>,
        activation: Option<ActivationId>,
    ) -> RpcResult<Self> {
        preflight_slice_rpc::<Q, R>(descriptor)?;
        let max_slice_len = descriptor.resources().max_request_slice_len;
        let inner = open_client_inner::<[Q::Element], [R::Element], Q::Header, R::Header, _, _>(
            descriptor,
            capability_instance,
            activation,
            |node, service, resources| {
                rr_builder_with_headers::<[Q::Element], [R::Element], Q::Header, R::Header>(
                    node, service, resources,
                )
                .open()
                .map_err(|error: RequestResponseOpenError| {
                    RpcError::RequestResponseOpen(error.to_string())
                })
            },
            |factory| {
                factory
                    .client_builder()
                    .initial_max_slice_len(max_slice_len)
                    .allocation_strategy(AllocationStrategy::Static)
                    .backpressure_strategy(BackpressureStrategy::DiscardData)
                    .create()
                    .map_err(|error| RpcError::ClientCreate(error.to_string()))
            },
        )?;
        Ok(Self { inner })
    }

    /// Alias for [`Self::open`].
    pub fn connect(descriptor: &RpcDescriptor) -> RpcResult<Self> {
        Self::open(descriptor)
    }
}

/// Server endpoint for a dynamic slice request and dynamic slice response.
pub struct SliceRpcServer<Q, R>
where
    Q: SliceMessageAbi,
    Q::Element: fmt::Debug + ZeroCopySend,
    Q::Header: fmt::Debug + ZeroCopySend,
    R: SliceMessageAbi,
    R::Element: fmt::Debug + ZeroCopySend,
    R::Header: fmt::Debug + ZeroCopySend,
{
    inner: SliceRpcRawServer<Q::Element, R::Element, Q::Header, R::Header>,
}

impl<Q, R> Deref for SliceRpcServer<Q, R>
where
    Q: SliceMessageAbi,
    Q::Element: fmt::Debug + ZeroCopySend,
    Q::Header: fmt::Debug + ZeroCopySend,
    R: SliceMessageAbi,
    R::Element: fmt::Debug + ZeroCopySend,
    R::Header: fmt::Debug + ZeroCopySend,
{
    type Target = SliceRpcRawServer<Q::Element, R::Element, Q::Header, R::Header>;

    fn deref(&self) -> &Self::Target {
        &self.inner
    }
}

impl<Q, R> DerefMut for SliceRpcServer<Q, R>
where
    Q: SliceMessageAbi,
    Q::Element: fmt::Debug + ZeroCopySend,
    Q::Header: fmt::Debug + ZeroCopySend,
    R: SliceMessageAbi,
    R::Element: fmt::Debug + ZeroCopySend,
    R::Header: fmt::Debug + ZeroCopySend,
{
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.inner
    }
}

impl<Q, R> SliceRpcServer<Q, R>
where
    Q: SliceMessageAbi,
    Q::Header: Default + fmt::Debug + ZeroCopySend,
    Q::Element: Default + fmt::Debug + ZeroCopySend,
    R: SliceMessageAbi,
    R::Header: Default + fmt::Debug + ZeroCopySend,
    R::Element: Default + fmt::Debug + ZeroCopySend,
{
    /// Opens or creates a slice-request/slice-response server.
    pub fn open_or_create(descriptor: &RpcDescriptor) -> RpcResult<Self> {
        Self::open_or_create_with_identity(descriptor, None)
    }

    /// Opens or creates a server with capability identity.
    pub fn open_or_create_for_capability(
        descriptor: &RpcDescriptor,
        capability_instance: CapabilityInstanceId,
    ) -> RpcResult<Self> {
        Self::open_or_create_with_identity(descriptor, Some(capability_instance))
    }

    fn open_or_create_with_identity(
        descriptor: &RpcDescriptor,
        capability_instance: Option<CapabilityInstanceId>,
    ) -> RpcResult<Self> {
        preflight_slice_rpc::<Q, R>(descriptor)?;
        let max_slice_len = descriptor.resources().max_response_slice_len;
        let inner = open_server_inner::<[Q::Element], [R::Element], Q::Header, R::Header, _, _>(
            descriptor,
            capability_instance,
            |node, service, resources| {
                rr_builder_with_headers::<[Q::Element], [R::Element], Q::Header, R::Header>(
                    node, service, resources,
                )
                .open_or_create()
                .map_err(|error: RequestResponseOpenOrCreateError| {
                    RpcError::RequestResponseOpenOrCreate(error.to_string())
                })
            },
            |factory| {
                factory
                    .server_builder()
                    .initial_max_slice_len(max_slice_len)
                    .allocation_strategy(AllocationStrategy::Static)
                    .backpressure_strategy(BackpressureStrategy::DiscardData)
                    .max_loaned_responses_per_request(descriptor.resources().max_response_loans)
                    .create()
                    .map_err(|error| RpcError::ServerCreate(error.to_string()))
            },
        )?;
        Ok(Self { inner })
    }

    /// Alias for [`Self::open_or_create`].
    pub fn open(descriptor: &RpcDescriptor) -> RpcResult<Self> {
        Self::open_or_create(descriptor)
    }

    /// Alias for [`Self::open_or_create`].
    pub fn create(descriptor: &RpcDescriptor) -> RpcResult<Self> {
        Self::open_or_create(descriptor)
    }
}

/// Request loan type for a dynamic slice request and dynamic slice response.
pub type SliceRpcRequestLoan<Q, R> = RpcRequestLoan<
    [<Q as SliceMessageAbi>::Element],
    [<R as SliceMessageAbi>::Element],
    <Q as SliceMessageAbi>::Header,
    <R as SliceMessageAbi>::Header,
>;
/// Response guard type for a dynamic slice request and dynamic slice response.
pub type SliceRpcResponseGuard<R> =
    RpcResponseGuard<[<R as SliceMessageAbi>::Element], <R as SliceMessageAbi>::Header>;

/// Type-level namespace for slice-request/slice-response RPC endpoints.
pub struct SliceRpc<Q, R>(PhantomData<fn() -> (Q, R)>);

impl<Q, R> SliceRpc<Q, R>
where
    Q: SliceMessageAbi,
    R: SliceMessageAbi,
{
    /// Creates a descriptor for a slice-request/slice-response endpoint.
    pub fn descriptor(service_name: impl Into<String>) -> RpcDescriptor {
        RpcDescriptor::for_slice::<Q, R>(service_name)
    }

    /// Validates the endpoint descriptor before transport creation.
    pub fn preflight(descriptor: &RpcDescriptor) -> RpcResult<()> {
        preflight_slice_rpc::<Q, R>(descriptor)
    }
}

impl<Q, R> SliceRpc<Q, R>
where
    Q: SliceMessageAbi,
    Q::Header: Default + fmt::Debug + ZeroCopySend,
    Q::Element: Default + fmt::Debug + ZeroCopySend,
    R: SliceMessageAbi,
    R::Header: Default + fmt::Debug + ZeroCopySend,
    R::Element: Default + fmt::Debug + ZeroCopySend,
{
    /// Opens the client endpoint.
    pub fn client(descriptor: &RpcDescriptor) -> RpcResult<SliceRpcClient<Q, R>> {
        SliceRpcClient::<Q, R>::open(descriptor)
    }

    /// Opens or creates the server endpoint.
    pub fn server(descriptor: &RpcDescriptor) -> RpcResult<SliceRpcServer<Q, R>> {
        SliceRpcServer::<Q, R>::open_or_create(descriptor)
    }
}

// A tiny local helper keeps the response-send match readable without adding
// an external dependency.
trait Pipe: Sized {
    fn pipe<T>(self, f: impl FnOnce(Self) -> T) -> T {
        f(self)
    }
}

impl<T> Pipe for T {}

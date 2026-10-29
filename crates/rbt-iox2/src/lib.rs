//! Typed, zero-copy local IPC streams backed by iceoryx2 shared memory.
//!
//! The composition plane transports [`StreamDescriptor`] values and chooses
//! physical service names. This crate validates those descriptors, then
//! connects publishers and subscribers directly to iceoryx2. It never
//! serializes a message payload and never stages a payload in an intermediate
//! buffer.

#![forbid(unsafe_op_in_unsafe_fn)]

use std::{
    fmt,
    marker::PhantomData,
    mem::{MaybeUninit, size_of},
    ops::{Deref, DerefMut},
    sync::Arc,
};

mod pressure;
mod rpc;
mod rpc_pressure;

pub use iceoryx2::prelude::ZeroCopySend;
use iceoryx2::{
    node::{Node, NodeBuilder, NodeCreationFailure},
    port::{
        BackpressureAction, LoanError, ReceiveError, SendError,
        backpressure_strategy::BackpressureStrategy,
        listener::{Listener as IoxListener, ListenerCreateError},
        notifier::{Notifier as IoxNotifier, NotifierCreateError},
        publisher::{Publisher as IoxPublisher, PublisherCreateError},
        subscriber::{Subscriber as IoxSubscriber, SubscriberCreateError},
    },
    prelude::{SignalHandlingMode, ipc_threadsafe},
    sample::Sample as IoxSample,
    sample_mut::SampleMut as IoxSampleMut,
    sample_mut_uninit::SampleMutUninit as IoxSampleMutUninit,
    service::{
        builder::event::{EventOpenError, EventOpenOrCreateError},
        builder::publish_subscribe::{
            PublishSubscribeOpenError, PublishSubscribeOpenOrCreateError,
        },
        port_factory::{
            PortFactory as _, publish_subscribe::PortFactory as IoxPublishSubscribeFactory,
        },
        service_name::{ServiceName, ServiceNameError},
    },
};
pub use rbt_core::{
    ActivationId, PressureEvent, ProcessInstanceId, PublisherPressure, RpcClientMetrics,
    RpcPressure, RpcServerMetrics, StreamPressure, StreamSemanticConfig, SubscriberPressure,
};
pub use rbt_types::{
    ControlCommand, Detection, FixedMessageAbi, Frame, Image, ImageHeader, LayoutHash,
    MessageShape, MessageTypeDescriptor, PixelFormat, PointCloud, PointCloudHeader, PointXYZI,
    ReadinessProtocol, RpcDescriptor, RpcReadinessProtocol, RpcResourceConfig,
    RpcTransportFeatures, ShmSafe, SliceMessageAbi, StreamDescriptor, StreamProfile,
    StreamResourceConfig,
};
pub use rpc::{
    FixedSliceRpc, FixedSliceRpcClient, FixedSliceRpcRequestLoan, FixedSliceRpcResponseGuard,
    FixedSliceRpcServer, PendingRpcCall, Rpc, RpcCallCancelHandle, RpcClient, RpcClientWakeHandle,
    RpcError, RpcRequest, RpcRequestLoan, RpcResponseGuard, RpcResponseLoan, RpcResult, RpcServer,
    RpcServerWakeHandle, SliceFixedRpc, SliceFixedRpcClient, SliceFixedRpcRequestLoan,
    SliceFixedRpcResponseGuard, SliceFixedRpcServer, SliceRpc, SliceRpcClient, SliceRpcRequestLoan,
    SliceRpcResponseGuard, SliceRpcServer, preflight_fixed_slice_rpc, preflight_rpc,
    preflight_slice_fixed_rpc, preflight_slice_rpc,
};
use thiserror::Error;

use crate::pressure::{BorrowTracker, LoanTracker, PressureState};

mod wait;
pub use wait::{
    EventSource, MAX_DRAIN_PER_SOURCE, ReadyEvent, ReadyEventKind, WaitContext, WaitError,
    WaitExit, WaitProgress, WaitStats, WaitStatsSnapshot, WaitToken, WaitWakeHandle,
};

/// Number of unread samples retained for a Sensor subscriber.
pub const SENSOR_QUEUE_DEPTH: usize = 1;
/// Number of unread commands retained for a Control subscriber.
pub const CONTROL_QUEUE_DEPTH: usize = 4;
/// Samples retained for subscribers which connect after publication.
pub const STREAM_HISTORY_SIZE: usize = 0;
/// Samples one subscriber may borrow concurrently.
pub const MAX_BORROWED_SAMPLES: usize = 1;
/// Samples one publisher may loan concurrently.
pub const MAX_LOANED_SAMPLES: usize = 2;

/// Backwards-compatible spelling for [`StreamResourceConfig`].
#[deprecated(note = "use StreamResourceConfig")]
pub type StreamLimits = StreamResourceConfig;

fn semantic_config(profile: StreamProfile) -> StreamSemanticConfig {
    StreamSemanticConfig::for_profile(profile)
}

fn validate_resources(resources: StreamResourceConfig) -> Result<StreamResourceConfig> {
    for (field, value) in [
        ("max_loans", resources.max_loans),
        ("max_borrowed", resources.max_borrowed),
    ] {
        if value == 0 {
            return Err(StreamError::InvalidResourceConfig { field, value });
        }
    }
    Ok(resources)
}

/// Errors returned by the typed data-plane API.
#[derive(Debug, Error)]
pub enum StreamError {
    /// A producer/consumer descriptor is not the ABI compiled into this endpoint.
    #[error(
        "ABI mismatch for {message}: expected layout {expected_layout} (ABI {expected_abi_major}), provider layout {actual_layout} (ABI {actual_abi_major})"
    )]
    AbiMismatch {
        /// Locally expected logical message name.
        message: String,
        /// Locally expected ABI major.
        expected_abi_major: u16,
        /// Provider-advertised ABI major.
        actual_abi_major: u16,
        /// Locally expected layout identity.
        expected_layout: LayoutHash,
        /// Provider-advertised layout identity.
        actual_layout: LayoutHash,
        /// Complete locally expected descriptor.
        expected: Box<MessageTypeDescriptor>,
        /// Complete provider-advertised descriptor.
        actual: Box<MessageTypeDescriptor>,
    },
    /// An event-driven endpoint requires a readiness feature the provider did
    /// not advertise. This is checked before any transport object is opened.
    #[error(
        "missing transport feature: readiness requires {required}, provider advertises {provider}"
    )]
    MissingTransportFeature {
        /// Readiness protocol required by this SDK endpoint.
        required: ReadinessProtocol,
        /// Readiness protocol advertised by the provider descriptor.
        provider: ReadinessProtocol,
    },
    /// A dynamic slice exceeds this publisher's configured maximum.
    #[error("invalid payload length {requested}; maximum is {maximum}")]
    InvalidPayloadLength {
        /// Number of requested elements.
        requested: usize,
        /// Maximum elements configured when the publisher opened.
        maximum: usize,
    },
    /// A stream resource limit was zero and would be silently clamped by the transport.
    #[error("invalid stream resource {field}={value}; value must be greater than zero")]
    InvalidResourceConfig {
        /// Name of the invalid resource.
        field: &'static str,
        /// Rejected resource value.
        value: usize,
    },
    /// A requested endpoint resource does not match the advertised service
    /// ceiling or exceeds it.
    #[error("incompatible stream resource {field}: requested {requested}, advertised {advertised}")]
    IncompatibleResourceConfig {
        /// Name of the resource with incompatible sizing.
        field: &'static str,
        /// Requested local or provider value.
        requested: usize,
        /// Advertised service value.
        advertised: usize,
    },
    /// A Control queue was full; the command was not silently overwritten.
    #[error("control queue full; sample was not delivered to every subscriber")]
    QueueFull,
    /// A sample could not be delivered to every connected subscriber.
    #[error("sample was not delivered to every subscriber")]
    UnableToDeliver,
    /// The configured maximum number of concurrent publisher Loans was reached.
    #[error("publisher loan pool exhausted: {0}")]
    LoanPoolExhausted(LoanError),
    /// The subscriber attempted to exceed its configured Borrow limit.
    #[error("subscriber borrow limit reached: {0}")]
    BorrowLimitReached(ReceiveError),
    /// The service name is invalid.
    #[error("invalid service name: {0}")]
    ServiceName(#[from] ServiceNameError),
    /// The iceoryx2 node could not be created.
    #[error("failed to create iceoryx2 node: {0}")]
    Node(#[from] NodeCreationFailure),
    /// The provider could not open or create its service.
    #[error("failed to open or create publish/subscribe service: {0}")]
    OpenOrCreateService(#[from] PublishSubscribeOpenOrCreateError),
    /// The consumer could not open the existing service.
    #[error("failed to open publish/subscribe service: {0}")]
    OpenService(#[from] PublishSubscribeOpenError),
    /// The publisher endpoint could not be created.
    #[error("failed to create publisher: {0}")]
    Publisher(#[from] PublisherCreateError),
    /// The subscriber endpoint could not be created.
    #[error("failed to create subscriber: {0}")]
    Subscriber(#[from] SubscriberCreateError),
    /// The readiness sidecar could not be opened or created.
    #[error("failed to open or create readiness sidecar: {0}")]
    ReadinessOpenOrCreate(#[from] EventOpenOrCreateError),
    /// The existing readiness sidecar could not be opened.
    #[error("failed to open readiness sidecar: {0}")]
    ReadinessOpen(#[from] EventOpenError),
    /// The readiness sidecar notifier could not be created.
    #[error("failed to create readiness notifier: {0}")]
    ReadinessNotifier(#[from] NotifierCreateError),
    /// The readiness sidecar listener could not be created.
    #[error("failed to create readiness listener: {0}")]
    ReadinessListener(#[from] ListenerCreateError),
    /// The payload send completed but its advisory readiness notification did
    /// not. The payload may already have been delivered.
    #[error("failed to notify readiness sidecar: {0}")]
    ReadinessNotify(#[from] iceoryx2::port::notifier::NotifierNotifyError),
    /// A publisher could not loan shared memory.
    #[error("failed to loan sample: {0}")]
    Loan(#[from] LoanError),
    /// A loaned sample could not be sent.
    #[error("failed to send sample: {0}")]
    Send(SendError),
    /// A subscriber could not receive a sample.
    #[error("failed to receive sample: {0}")]
    Receive(#[from] ReceiveError),
}

/// Backwards-compatible short name for [`StreamError`].
pub type Error = StreamError;
/// Result type used by this crate.
pub type Result<T> = std::result::Result<T, StreamError>;

fn payload_bytes<M: SliceMessageAbi>(element_count: usize) -> u64 {
    u64::try_from(size_of::<M::Element>())
        .unwrap_or(u64::MAX)
        .saturating_mul(u64::try_from(element_count).unwrap_or(u64::MAX))
}

fn validate_publisher_resources(
    descriptor: &StreamDescriptor,
    requested: StreamResourceConfig,
) -> Result<StreamResourceConfig> {
    let requested = validate_resources(requested)?;
    let advertised = validate_resources(descriptor.resources())?;
    for (field, requested_value, advertised_value) in [
        ("max_loans", requested.max_loans, advertised.max_loans),
        (
            "max_borrowed",
            requested.max_borrowed,
            advertised.max_borrowed,
        ),
    ] {
        if requested_value != advertised_value {
            return Err(StreamError::IncompatibleResourceConfig {
                field,
                requested: requested_value,
                advertised: advertised_value,
            });
        }
    }
    Ok(advertised)
}

fn validate_subscriber_resources(
    descriptor: &StreamDescriptor,
    requested: StreamResourceConfig,
) -> Result<StreamResourceConfig> {
    let requested = validate_resources(requested)?;
    let advertised = validate_resources(descriptor.resources())?;
    if requested.max_borrowed > advertised.max_borrowed {
        return Err(StreamError::IncompatibleResourceConfig {
            field: "max_borrowed",
            requested: requested.max_borrowed,
            advertised: advertised.max_borrowed,
        });
    }
    // `max_loans` is a publisher ceiling and is not a subscriber-local
    // resource. Preserve the advertised value in pressure snapshots while
    // honoring only the requested local borrow ceiling here.
    Ok(StreamResourceConfig::new(
        advertised.max_loans,
        requested.max_borrowed,
    ))
}

fn validate_descriptor(
    expected: MessageTypeDescriptor,
    actual: &MessageTypeDescriptor,
) -> Result<()> {
    if expected == *actual {
        return Ok(());
    }

    Err(StreamError::AbiMismatch {
        message: expected.qualified_name(),
        expected_abi_major: expected.abi_major,
        actual_abi_major: actual.abi_major,
        expected_layout: expected.layout_hash,
        actual_layout: actual.layout_hash,
        expected: Box::new(expected),
        actual: Box::new(actual.clone()),
    })
}

/// Validates payload ABI and event-driven transport compatibility before any
/// iceoryx2 node, listener, or payload endpoint is created.
pub fn preflight_stream(expected: MessageTypeDescriptor, actual: &StreamDescriptor) -> Result<()> {
    validate_descriptor(expected, actual.message_type())?;
    if actual.readiness() != ReadinessProtocol::SidecarV1 {
        return Err(StreamError::MissingTransportFeature {
            required: ReadinessProtocol::SidecarV1,
            provider: actual.readiness(),
        });
    }
    Ok(())
}

fn service_name(descriptor: &StreamDescriptor) -> Result<ServiceName> {
    Ok(descriptor.service_name().try_into()?)
}

/// The suffix used for a stream's readiness sidecar service.
///
/// This is intentionally a plain ASCII suffix rather than a hash derived from
/// a process-local value.  Rust and C++ participants can therefore derive the
/// same sidecar service name from the advertised physical stream name.
const READINESS_SERVICE_SUFFIX: &str = "/ready";

fn readiness_service_name(descriptor: &StreamDescriptor) -> Result<ServiceName> {
    let name = format!("{}{}", descriptor.service_name(), READINESS_SERVICE_SUFFIX);
    Ok(name.as_str().try_into()?)
}

fn create_readiness_notifier(
    node: &Node<ipc_threadsafe::Service>,
    descriptor: &StreamDescriptor,
) -> Result<IoxNotifier<ipc_threadsafe::Service>> {
    let name = readiness_service_name(descriptor)?;
    let event = node.service_builder(&name).event().open_or_create()?;
    Ok(event.notifier_builder().create()?)
}

fn open_readiness_listener(
    node: &Node<ipc_threadsafe::Service>,
    descriptor: &StreamDescriptor,
) -> Result<IoxListener<ipc_threadsafe::Service>> {
    let name = readiness_service_name(descriptor)?;
    let event = node.service_builder(&name).event().open()?;
    Ok(event.listener_builder().create()?)
}

fn notify_readiness(
    notifier: &IoxNotifier<ipc_threadsafe::Service>,
) -> std::result::Result<(), iceoryx2::port::notifier::NotifierNotifyError> {
    notifier.notify().map(|_| ())
}

fn map_send_error(error: SendError, profile: StreamProfile) -> StreamError {
    match (profile, error) {
        (StreamProfile::Control, SendError::UnableToDeliver) => StreamError::QueueFull,
        (_, SendError::UnableToDeliver) => StreamError::UnableToDeliver,
        (_, error) => StreamError::Send(error),
    }
}

fn map_loan_error(error: LoanError) -> StreamError {
    match error {
        LoanError::ExceedsMaxLoans | LoanError::OutOfMemory => {
            StreamError::LoanPoolExhausted(error)
        }
        error => StreamError::Loan(error),
    }
}

fn map_receive_error(error: ReceiveError) -> StreamError {
    match error {
        ReceiveError::ExceedsMaxBorrows => StreamError::BorrowLimitReached(error),
        error => StreamError::Receive(error),
    }
}

fn pressure_event_for_send(profile: StreamProfile, error: SendError) -> PressureEvent {
    match (profile, error) {
        (StreamProfile::Control, SendError::UnableToDeliver) => {
            PressureEvent::QueueFull { subscriber: None }
        }
        _ => PressureEvent::UnableToDeliver,
    }
}

/// Returns pressure snapshots for all live stream wrappers in this process.
///
/// Publisher entries are transport-observed. Subscriber entries are explicitly
/// self-reported wrapper counters and contain no inferred Queue occupancy or
/// sequence-gap estimate.
pub fn local_pressure_snapshots() -> Vec<StreamPressure> {
    pressure::snapshots()
}

/// A type-level namespace for fixed-size message streams.
pub struct Stream<T>(PhantomData<fn() -> T>);

impl<T> Stream<T>
where
    T: Default + FixedMessageAbi + ZeroCopySend,
{
    /// Creates the descriptor advertised through the composition plane.
    pub fn descriptor(service_name: impl Into<String>, profile: StreamProfile) -> StreamDescriptor {
        StreamDescriptor::for_fixed::<T>(service_name, profile)
    }

    /// Opens or creates a provider using an already-advertised descriptor.
    pub fn publisher(descriptor: &StreamDescriptor) -> Result<Publisher<T>> {
        Self::publisher_with_resources(descriptor, descriptor.resources())
    }

    /// Opens or creates a provider with explicit resource sizing. Provider
    /// resources must exactly match the descriptor advertised to subscribers.
    pub fn publisher_with_resources(
        descriptor: &StreamDescriptor,
        resources: StreamResourceConfig,
    ) -> Result<Publisher<T>> {
        let resources = validate_publisher_resources(descriptor, resources)?;
        preflight_stream(T::descriptor(), descriptor)?;
        let service_name = service_name(descriptor)?;
        let profile = descriptor.profile();
        let semantic = semantic_config(profile);
        let node = NodeBuilder::new()
            .signal_handling_mode(SignalHandlingMode::Disabled)
            .create::<ipc_threadsafe::Service>()?;
        let notifier = Arc::new(create_readiness_notifier(&node, descriptor)?);
        let factory = Arc::new(
            node.service_builder(&service_name)
                .publish_subscribe::<T>()
                .enable_safe_overflow(semantic.safe_overflow)
                .subscriber_max_buffer_size(semantic.queue_depth)
                .subscriber_max_borrowed_samples(resources.max_borrowed)
                .history_size(semantic.history_size)
                .open_or_create()?,
        );
        let pressure_factory = Arc::downgrade(&factory);
        let pressure = PressureState::publisher(
            descriptor.service_name().to_owned(),
            profile,
            semantic,
            resources,
            move || {
                pressure_factory.upgrade().map_or(0, |factory| {
                    factory.dynamic_config().number_of_subscribers()
                })
            },
        );
        let endpoint_builder = factory
            .publisher_builder()
            .max_loaned_samples(resources.max_loans)
            .backpressure_strategy(BackpressureStrategy::DiscardData);
        let endpoint = match profile {
            StreamProfile::Sensor => endpoint_builder.create()?,
            StreamProfile::Control => {
                let handler_pressure = Arc::clone(&pressure);
                endpoint_builder
                    .set_backpressure_handler(move |info| {
                        handler_pressure
                            .queue_full(Some(format!("{:032x}", info.receiver_port_id)));
                        BackpressureAction::DiscardDataAndFail
                    })
                    .create()?
            }
        };

        Ok(Publisher {
            endpoint,
            notifier,
            profile,
            pressure,
            _factory: factory,
            _node: Arc::new(node),
        })
    }

    /// Deprecated alias for [`Self::publisher_with_resources`].
    #[deprecated(note = "use publisher_with_resources")]
    pub fn publisher_with_limits(
        descriptor: &StreamDescriptor,
        resources: StreamResourceConfig,
    ) -> Result<Publisher<T>> {
        Self::publisher_with_resources(descriptor, resources)
    }

    /// Opens an existing consumer service after validating the provider ABI.
    /// This uses `open`, never `open_or_create`.
    pub fn subscriber(descriptor: &StreamDescriptor) -> Result<Subscriber<T>> {
        Self::subscriber_with_resources(descriptor, descriptor.resources())
    }

    /// Opens an existing consumer service with an explicit local borrow
    /// ceiling. The native service ceiling remains the descriptor's advertised
    /// value; the smaller request is enforced before every native receive.
    pub fn subscriber_with_resources(
        descriptor: &StreamDescriptor,
        resources: StreamResourceConfig,
    ) -> Result<Subscriber<T>> {
        let resources = validate_subscriber_resources(descriptor, resources)?;
        preflight_stream(T::descriptor(), descriptor)?;
        let service_name = service_name(descriptor)?;
        let profile = descriptor.profile();
        let semantic = semantic_config(profile);
        let advertised = validate_resources(descriptor.resources())?;
        let node = NodeBuilder::new()
            .signal_handling_mode(SignalHandlingMode::Disabled)
            .create::<ipc_threadsafe::Service>()?;
        // Create the readiness listener before exposing the payload
        // subscriber.  This leaves no interval in which a queued sample can
        // be published without a source for the initial readiness pass.
        let listener = open_readiness_listener(&node, descriptor)?;
        let service = node
            .service_builder(&service_name)
            .publish_subscribe::<T>()
            .enable_safe_overflow(semantic.safe_overflow)
            .subscriber_max_buffer_size(semantic.queue_depth)
            .subscriber_max_borrowed_samples(advertised.max_borrowed)
            .history_size(semantic.history_size)
            .open()?;
        let endpoint = service
            .subscriber_builder()
            .buffer_size(semantic.queue_depth)
            .create()?;
        let pressure = PressureState::subscriber(
            descriptor.service_name().to_owned(),
            profile,
            semantic,
            resources,
        );

        Ok(Subscriber {
            endpoint,
            listener,
            pressure,
            _node: Arc::new(node),
        })
    }

    /// Deprecated alias for [`Self::subscriber_with_resources`].
    #[deprecated(note = "use subscriber_with_resources")]
    pub fn subscriber_with_limits(
        descriptor: &StreamDescriptor,
        resources: StreamResourceConfig,
    ) -> Result<Subscriber<T>> {
        Self::subscriber_with_resources(descriptor, resources)
    }
}

/// A fixed-size iceoryx2 publisher and its owning node.
pub struct Publisher<T>
where
    T: FixedMessageAbi + ZeroCopySend,
{
    endpoint: IoxPublisher<ipc_threadsafe::Service, T, ()>,
    notifier: Arc<IoxNotifier<ipc_threadsafe::Service>>,
    profile: StreamProfile,
    pressure: Arc<PressureState>,
    _factory: Arc<IoxPublishSubscribeFactory<ipc_threadsafe::Service, T, ()>>,
    _node: Arc<Node<ipc_threadsafe::Service>>,
}

impl<T> fmt::Debug for Publisher<T>
where
    T: FixedMessageAbi + ZeroCopySend,
{
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("Publisher")
            .field("endpoint", &self.endpoint)
            .field("profile", &self.profile)
            .finish_non_exhaustive()
    }
}

impl<T> Publisher<T>
where
    T: Default + FixedMessageAbi + ZeroCopySend,
{
    /// Loans one initialized sample directly from iceoryx2 shared memory.
    pub fn loan(&self) -> Result<LoanedSample<T>> {
        self.pressure.loan_attempted();
        match self.endpoint.loan() {
            Ok(sample) => Ok(LoanedSample {
                sample,
                notifier: Arc::clone(&self.notifier),
                profile: self.profile,
                pressure: Arc::clone(&self.pressure),
                _loan: PressureState::loan_succeeded_arc(
                    &self.pressure,
                    u64::try_from(size_of::<T>()).unwrap_or(u64::MAX),
                ),
                _node: Arc::clone(&self._node),
            }),
            Err(error) => {
                if matches!(error, LoanError::ExceedsMaxLoans | LoanError::OutOfMemory) {
                    self.pressure.loan_failed();
                }
                Err(map_loan_error(error))
            }
        }
    }

    /// Returns a truthful local pressure snapshot for this publisher.
    pub fn pressure(&self) -> StreamPressure {
        self.pressure.snapshot()
    }
}

/// A mutable fixed-size sample loaned directly from shared memory.
pub struct LoanedSample<T>
where
    T: FixedMessageAbi + ZeroCopySend,
{
    _loan: LoanTracker,
    sample: IoxSampleMut<ipc_threadsafe::Service, T, ()>,
    notifier: Arc<IoxNotifier<ipc_threadsafe::Service>>,
    profile: StreamProfile,
    pressure: Arc<PressureState>,
    // Dropped last (Rust drops fields in declaration order): a node destroyed
    // while one of its ports is still referenced by this sample cannot remove
    // its node directory and leaves it behind in the iceoryx2 root.
    _node: Arc<Node<ipc_threadsafe::Service>>,
}

impl<T> Deref for LoanedSample<T>
where
    T: FixedMessageAbi + ZeroCopySend,
{
    type Target = T;

    fn deref(&self) -> &Self::Target {
        &self.sample
    }
}

impl<T> DerefMut for LoanedSample<T>
where
    T: FixedMessageAbi + ZeroCopySend,
{
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.sample
    }
}

impl<T> fmt::Debug for LoanedSample<T>
where
    T: FixedMessageAbi + ZeroCopySend,
{
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.sample.fmt(formatter)
    }
}

impl<T> LoanedSample<T>
where
    T: FixedMessageAbi + ZeroCopySend,
{
    /// Sends the loan without serializing or copying it through rbt.
    pub fn send(mut self) -> Result<usize> {
        self.pressure.send_attempted();
        // Release the local metric before native send makes the allocation
        // reusable by another loaner.
        self._loan.release();
        let result = self.sample.send();
        // An UnableToDeliver result is non-transactional in iceoryx2 and may
        // still mean that one or more subscribers received the sample.  Send
        // the advisory readiness event for every completed send attempt.
        let readiness = notify_readiness(&self.notifier);
        if readiness.is_err() {
            self.pressure.readiness_notify_failed();
        }
        match result {
            Ok(recipients) => {
                self.pressure.send_succeeded();
                readiness
                    .map(|()| recipients)
                    .map_err(StreamError::ReadinessNotify)
            }
            Err(error) => {
                if let Err(readiness_error) = readiness {
                    // Preserve the original send error as the primary result;
                    // a non-transactional send may already have reached a
                    // subscriber even when its readiness event failed.
                    tracing::warn!(
                        ?readiness_error,
                        "failed to notify readiness after failed payload send"
                    );
                }
                self.pressure
                    .send_failed(pressure_event_for_send(self.profile, error));
                Err(map_send_error(error, self.profile))
            }
        }
    }
}

/// A fixed-size iceoryx2 subscriber and its owning node.
pub struct Subscriber<T>
where
    T: FixedMessageAbi + ZeroCopySend,
{
    endpoint: IoxSubscriber<ipc_threadsafe::Service, T, ()>,
    listener: IoxListener<ipc_threadsafe::Service>,
    pressure: Arc<PressureState>,
    _node: Arc<Node<ipc_threadsafe::Service>>,
}

impl<T> fmt::Debug for Subscriber<T>
where
    T: FixedMessageAbi + ZeroCopySend,
{
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("Subscriber")
            .field("endpoint", &self.endpoint)
            .finish_non_exhaustive()
    }
}

impl<T> Subscriber<T>
where
    T: FixedMessageAbi + ZeroCopySend,
{
    /// Creates a stable-token readiness source for this subscriber.
    ///
    /// The source carries only the sidecar listener.  It never receives or
    /// borrows a payload; handlers should call [`Self::receive`] after a
    /// [`ReadyEvent`] is dispatched by [`WaitContext`].
    pub fn event_source(&self, token: WaitToken) -> EventSource<'_> {
        EventSource::new(token, &self.listener)
    }

    /// Alias for [`Self::event_source`].
    pub fn wait_source(&self, token: WaitToken) -> EventSource<'_> {
        self.event_source(token)
    }

    /// Receives the next guarded shared-memory sample.
    pub fn receive(&self) -> Result<Option<ReceivedSample<T>>> {
        self.pressure.receive_attempted();
        let Some(reservation) = self.pressure.reserve_borrow() else {
            self.pressure.borrow_failed();
            return Err(StreamError::BorrowLimitReached(
                ReceiveError::ExceedsMaxBorrows,
            ));
        };
        match self.endpoint.receive() {
            Ok(Some(sample)) => Ok(Some(ReceivedSample {
                sample,
                _borrow: reservation.commit(),
                _node: Arc::clone(&self._node),
            })),
            Ok(None) => {
                drop(reservation);
                Ok(None)
            }
            Err(error) => {
                if matches!(error, ReceiveError::ExceedsMaxBorrows) {
                    self.pressure.borrow_failed();
                }
                drop(reservation);
                Err(map_receive_error(error))
            }
        }
    }

    /// Returns this subscriber wrapper's self-reported pressure snapshot.
    pub fn pressure(&self) -> StreamPressure {
        self.pressure.snapshot()
    }
}

/// A received fixed-size sample backed by iceoryx2 shared memory.
pub struct ReceivedSample<T>
where
    T: FixedMessageAbi + ZeroCopySend,
{
    sample: IoxSample<ipc_threadsafe::Service, T, ()>,
    _borrow: BorrowTracker,
    // Dropped last (Rust drops fields in declaration order): a node destroyed
    // while one of its ports is still referenced by this sample cannot remove
    // its node directory and leaves it behind in the iceoryx2 root.
    _node: Arc<Node<ipc_threadsafe::Service>>,
}

impl<T> Deref for ReceivedSample<T>
where
    T: FixedMessageAbi + ZeroCopySend,
{
    type Target = T;

    fn deref(&self) -> &Self::Target {
        &self.sample
    }
}

impl<T> fmt::Debug for ReceivedSample<T>
where
    T: FixedMessageAbi + ZeroCopySend,
{
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.sample.fmt(formatter)
    }
}

/// A type-level namespace for one user header plus one homogeneous slice.
pub struct SliceStream<M>(PhantomData<fn() -> M>);

impl<M> SliceStream<M>
where
    M: SliceMessageAbi,
    M::Header: Default + ZeroCopySend,
    M::Element: Default + ZeroCopySend,
{
    /// Creates the descriptor advertised through the composition plane.
    pub fn descriptor(service_name: impl Into<String>, profile: StreamProfile) -> StreamDescriptor {
        StreamDescriptor::for_slice::<M>(service_name, profile)
    }

    /// Opens or creates a provider with a maximum element count.
    pub fn publisher(
        descriptor: &StreamDescriptor,
        maximum_elements: usize,
    ) -> Result<SlicePublisher<M>> {
        Self::publisher_with_resources(descriptor, maximum_elements, descriptor.resources())
    }

    /// Opens or creates a provider with explicit resource sizing. Provider
    /// resources must exactly match the descriptor advertised to subscribers.
    pub fn publisher_with_resources(
        descriptor: &StreamDescriptor,
        maximum_elements: usize,
        resources: StreamResourceConfig,
    ) -> Result<SlicePublisher<M>> {
        let resources = validate_publisher_resources(descriptor, resources)?;
        preflight_stream(M::descriptor(), descriptor)?;
        let service_name = service_name(descriptor)?;
        let profile = descriptor.profile();
        let semantic = semantic_config(profile);
        let node = NodeBuilder::new()
            .signal_handling_mode(SignalHandlingMode::Disabled)
            .create::<ipc_threadsafe::Service>()?;
        let notifier = Arc::new(create_readiness_notifier(&node, descriptor)?);
        let factory = Arc::new(
            node.service_builder(&service_name)
                .publish_subscribe::<[M::Element]>()
                .user_header::<M::Header>()
                .enable_safe_overflow(semantic.safe_overflow)
                .subscriber_max_buffer_size(semantic.queue_depth)
                .subscriber_max_borrowed_samples(resources.max_borrowed)
                .history_size(semantic.history_size)
                .open_or_create()?,
        );
        let pressure_factory = Arc::downgrade(&factory);
        let pressure = PressureState::publisher(
            descriptor.service_name().to_owned(),
            profile,
            semantic,
            resources,
            move || {
                pressure_factory.upgrade().map_or(0, |factory| {
                    factory.dynamic_config().number_of_subscribers()
                })
            },
        );
        let endpoint_builder = factory
            .publisher_builder()
            .initial_max_slice_len(maximum_elements)
            .max_loaned_samples(resources.max_loans)
            .backpressure_strategy(BackpressureStrategy::DiscardData);
        let endpoint = match profile {
            StreamProfile::Sensor => endpoint_builder.create()?,
            StreamProfile::Control => {
                let pressure = Arc::clone(&pressure);
                endpoint_builder
                    .set_backpressure_handler(move |info| {
                        pressure.queue_full(Some(format!("{:032x}", info.receiver_port_id)));
                        BackpressureAction::DiscardDataAndFail
                    })
                    .create()?
            }
        };

        Ok(SlicePublisher {
            endpoint,
            notifier,
            maximum_elements,
            profile,
            pressure,
            _factory: factory,
            _message: PhantomData,
            _node: Arc::new(node),
        })
    }

    /// Deprecated alias for [`Self::publisher_with_resources`].
    #[deprecated(note = "use publisher_with_resources")]
    pub fn publisher_with_limits(
        descriptor: &StreamDescriptor,
        maximum_elements: usize,
        resources: StreamResourceConfig,
    ) -> Result<SlicePublisher<M>> {
        Self::publisher_with_resources(descriptor, maximum_elements, resources)
    }

    /// Opens an existing consumer after validating the provider ABI.
    pub fn subscriber(descriptor: &StreamDescriptor) -> Result<SliceSubscriber<M>> {
        Self::subscriber_with_resources(descriptor, descriptor.resources())
    }

    /// Opens an existing consumer with an explicit local borrow ceiling. The
    /// native service ceiling remains the descriptor's advertised value; the
    /// smaller request is enforced before every native receive.
    pub fn subscriber_with_resources(
        descriptor: &StreamDescriptor,
        resources: StreamResourceConfig,
    ) -> Result<SliceSubscriber<M>> {
        let resources = validate_subscriber_resources(descriptor, resources)?;
        preflight_stream(M::descriptor(), descriptor)?;
        let service_name = service_name(descriptor)?;
        let profile = descriptor.profile();
        let semantic = semantic_config(profile);
        let advertised = validate_resources(descriptor.resources())?;
        let node = NodeBuilder::new()
            .signal_handling_mode(SignalHandlingMode::Disabled)
            .create::<ipc_threadsafe::Service>()?;
        // Keep the listener alive before the payload subscriber is exposed so
        // a source can perform its synthetic initial pass without a startup
        // notification race.
        let listener = open_readiness_listener(&node, descriptor)?;
        let service = node
            .service_builder(&service_name)
            .publish_subscribe::<[M::Element]>()
            .user_header::<M::Header>()
            .enable_safe_overflow(semantic.safe_overflow)
            .subscriber_max_buffer_size(semantic.queue_depth)
            .subscriber_max_borrowed_samples(advertised.max_borrowed)
            .history_size(semantic.history_size)
            .open()?;
        let endpoint = service
            .subscriber_builder()
            .buffer_size(semantic.queue_depth)
            .create()?;
        let pressure = PressureState::subscriber(
            descriptor.service_name().to_owned(),
            profile,
            semantic,
            resources,
        );

        Ok(SliceSubscriber {
            endpoint,
            listener,
            pressure,
            _message: PhantomData,
            _node: Arc::new(node),
        })
    }

    /// Deprecated alias for [`Self::subscriber_with_resources`].
    #[deprecated(note = "use subscriber_with_resources")]
    pub fn subscriber_with_limits(
        descriptor: &StreamDescriptor,
        resources: StreamResourceConfig,
    ) -> Result<SliceSubscriber<M>> {
        Self::subscriber_with_resources(descriptor, resources)
    }
}

/// A dynamic-slice iceoryx2 publisher and its owning node.
pub struct SlicePublisher<M>
where
    M: SliceMessageAbi,
    M::Header: ZeroCopySend,
    M::Element: ZeroCopySend,
{
    endpoint: IoxPublisher<ipc_threadsafe::Service, [M::Element], M::Header>,
    notifier: Arc<IoxNotifier<ipc_threadsafe::Service>>,
    maximum_elements: usize,
    profile: StreamProfile,
    pressure: Arc<PressureState>,
    _factory: Arc<IoxPublishSubscribeFactory<ipc_threadsafe::Service, [M::Element], M::Header>>,
    _message: PhantomData<M>,
    _node: Arc<Node<ipc_threadsafe::Service>>,
}

impl<M> fmt::Debug for SlicePublisher<M>
where
    M: SliceMessageAbi,
    M::Header: ZeroCopySend,
    M::Element: ZeroCopySend,
{
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("SlicePublisher")
            .field("endpoint", &self.endpoint)
            .field("maximum_elements", &self.maximum_elements)
            .field("profile", &self.profile)
            .finish_non_exhaustive()
    }
}

impl<M> SlicePublisher<M>
where
    M: SliceMessageAbi,
    M::Header: Default + ZeroCopySend,
    M::Element: Default + ZeroCopySend,
{
    /// Returns the largest payload configured for this publisher.
    pub const fn maximum_elements(&self) -> usize {
        self.maximum_elements
    }

    /// Loans an initialized slice with the configured maximum length.
    pub fn loan(&self) -> Result<LoanedSliceSample<M>> {
        self.loan_slice(self.maximum_elements)
    }

    /// Loans an initialized slice directly from shared memory.
    pub fn loan_slice(&self, element_count: usize) -> Result<LoanedSliceSample<M>> {
        self.validate_length(element_count)?;
        self.pressure.loan_attempted();
        match self.endpoint.loan_slice(element_count) {
            Ok(sample) => Ok(LoanedSliceSample {
                sample,
                notifier: Arc::clone(&self.notifier),
                profile: self.profile,
                pressure: Arc::clone(&self.pressure),
                _loan: PressureState::loan_succeeded_arc(
                    &self.pressure,
                    payload_bytes::<M>(element_count),
                ),
                _message: PhantomData,
                _node: Arc::clone(&self._node),
            }),
            Err(error) => {
                if matches!(error, LoanError::ExceedsMaxLoans | LoanError::OutOfMemory) {
                    self.pressure.loan_failed();
                }
                Err(map_loan_error(error))
            }
        }
    }

    /// Loans uninitialized shared memory. Safe initialization methods on the
    /// returned guard initialize every element before it can be sent.
    pub fn loan_slice_uninit(&self, element_count: usize) -> Result<UninitLoanedSliceSample<M>> {
        self.validate_length(element_count)?;
        self.pressure.loan_attempted();
        match self.endpoint.loan_slice_uninit(element_count) {
            Ok(sample) => Ok(UninitLoanedSliceSample {
                sample,
                notifier: Arc::clone(&self.notifier),
                profile: self.profile,
                pressure: Arc::clone(&self.pressure),
                _loan: PressureState::loan_succeeded_arc(
                    &self.pressure,
                    payload_bytes::<M>(element_count),
                ),
                _message: PhantomData,
                _node: Arc::clone(&self._node),
            }),
            Err(error) => {
                if matches!(error, LoanError::ExceedsMaxLoans | LoanError::OutOfMemory) {
                    self.pressure.loan_failed();
                }
                Err(map_loan_error(error))
            }
        }
    }

    /// Returns a truthful local pressure snapshot for this publisher.
    pub fn pressure(&self) -> StreamPressure {
        self.pressure.snapshot()
    }

    fn validate_length(&self, requested: usize) -> Result<()> {
        if requested <= self.maximum_elements {
            Ok(())
        } else {
            Err(StreamError::InvalidPayloadLength {
                requested,
                maximum: self.maximum_elements,
            })
        }
    }
}

/// An initialized mutable user header plus payload slice in shared memory.
pub struct LoanedSliceSample<M>
where
    M: SliceMessageAbi,
    M::Header: ZeroCopySend,
    M::Element: ZeroCopySend,
{
    _loan: LoanTracker,
    sample: IoxSampleMut<ipc_threadsafe::Service, [M::Element], M::Header>,
    notifier: Arc<IoxNotifier<ipc_threadsafe::Service>>,
    profile: StreamProfile,
    pressure: Arc<PressureState>,
    _message: PhantomData<M>,
    // Dropped last (Rust drops fields in declaration order): a node destroyed
    // while one of its ports is still referenced by this sample cannot remove
    // its node directory and leaves it behind in the iceoryx2 root.
    _node: Arc<Node<ipc_threadsafe::Service>>,
}

impl<M> LoanedSliceSample<M>
where
    M: SliceMessageAbi,
    M::Header: ZeroCopySend,
    M::Element: ZeroCopySend,
{
    /// Borrows the fixed user header.
    pub fn header(&self) -> &M::Header {
        self.sample.user_header()
    }

    /// Mutably borrows the fixed user header.
    pub fn header_mut(&mut self) -> &mut M::Header {
        self.sample.user_header_mut()
    }

    /// Borrows the dynamic payload slice.
    pub fn payload(&self) -> &[M::Element] {
        self.sample.payload()
    }

    /// Mutably borrows the dynamic payload in its loaned SHM allocation.
    pub fn payload_mut(&mut self) -> &mut [M::Element] {
        self.sample.payload_mut()
    }

    /// Sends this exact shared-memory allocation without an rbt copy.
    pub fn send(mut self) -> Result<usize> {
        self.pressure.send_attempted();
        // Release the local metric before native send makes the allocation
        // reusable by another loaner.
        self._loan.release();
        let result = self.sample.send();
        // A failed Control send may have partially delivered to other
        // subscribers, so readiness follows every completed send attempt.
        let readiness = notify_readiness(&self.notifier);
        if readiness.is_err() {
            self.pressure.readiness_notify_failed();
        }
        match result {
            Ok(recipients) => {
                self.pressure.send_succeeded();
                readiness
                    .map(|()| recipients)
                    .map_err(StreamError::ReadinessNotify)
            }
            Err(error) => {
                if let Err(readiness_error) = readiness {
                    tracing::warn!(
                        ?readiness_error,
                        "failed to notify readiness after failed payload send"
                    );
                }
                self.pressure
                    .send_failed(pressure_event_for_send(self.profile, error));
                Err(map_send_error(error, self.profile))
            }
        }
    }
}

impl<M> fmt::Debug for LoanedSliceSample<M>
where
    M: SliceMessageAbi,
    M::Header: ZeroCopySend,
    M::Element: ZeroCopySend,
{
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.sample.fmt(formatter)
    }
}

/// An uninitialized dynamic payload loan. Dropping it is safe; it cannot be
/// sent until a safe method initializes every payload element.
pub struct UninitLoanedSliceSample<M>
where
    M: SliceMessageAbi,
    M::Header: ZeroCopySend,
    M::Element: ZeroCopySend,
{
    _loan: LoanTracker,
    sample: IoxSampleMutUninit<ipc_threadsafe::Service, [MaybeUninit<M::Element>], M::Header>,
    notifier: Arc<IoxNotifier<ipc_threadsafe::Service>>,
    profile: StreamProfile,
    pressure: Arc<PressureState>,
    _message: PhantomData<M>,
    // Dropped last (Rust drops fields in declaration order): a node destroyed
    // while one of its ports is still referenced by this sample cannot remove
    // its node directory and leaves it behind in the iceoryx2 root.
    _node: Arc<Node<ipc_threadsafe::Service>>,
}

impl<M> UninitLoanedSliceSample<M>
where
    M: SliceMessageAbi,
    M::Header: ZeroCopySend,
    M::Element: ZeroCopySend,
{
    /// Mutably borrows the initialized fixed header.
    pub fn header_mut(&mut self) -> &mut M::Header {
        self.sample.user_header_mut()
    }

    /// Borrows uninitialized element slots. Partial writes do not make this
    /// guard sendable.
    pub fn payload_mut(&mut self) -> &mut [MaybeUninit<M::Element>] {
        self.sample.payload_mut()
    }

    /// Initializes every element in place and returns a sendable loan.
    pub fn write_from_fn<F>(self, initializer: F) -> LoanedSliceSample<M>
    where
        F: FnMut(usize) -> M::Element,
    {
        LoanedSliceSample {
            sample: self.sample.write_from_fn(initializer),
            notifier: self.notifier,
            profile: self.profile,
            pressure: self.pressure,
            _loan: self._loan,
            _message: PhantomData,
            _node: self._node,
        }
    }
}

/// A dynamic-slice iceoryx2 subscriber and its owning node.
pub struct SliceSubscriber<M>
where
    M: SliceMessageAbi,
    M::Header: ZeroCopySend,
    M::Element: ZeroCopySend,
{
    endpoint: IoxSubscriber<ipc_threadsafe::Service, [M::Element], M::Header>,
    listener: IoxListener<ipc_threadsafe::Service>,
    pressure: Arc<PressureState>,
    _message: PhantomData<M>,
    _node: Arc<Node<ipc_threadsafe::Service>>,
}

impl<M> fmt::Debug for SliceSubscriber<M>
where
    M: SliceMessageAbi,
    M::Header: ZeroCopySend,
    M::Element: ZeroCopySend,
{
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("SliceSubscriber")
            .field("endpoint", &self.endpoint)
            .finish_non_exhaustive()
    }
}

impl<M> SliceSubscriber<M>
where
    M: SliceMessageAbi,
    M::Header: ZeroCopySend,
    M::Element: ZeroCopySend,
{
    /// Creates a stable-token readiness source for this subscriber.
    pub fn event_source(&self, token: WaitToken) -> EventSource<'_> {
        EventSource::new(token, &self.listener)
    }

    /// Alias for [`Self::event_source`].
    pub fn wait_source(&self, token: WaitToken) -> EventSource<'_> {
        self.event_source(token)
    }

    /// Receives one guarded shared-memory sample.
    pub fn receive(&self) -> Result<Option<ReceivedSliceSample<M>>> {
        self.pressure.receive_attempted();
        let Some(reservation) = self.pressure.reserve_borrow() else {
            self.pressure.borrow_failed();
            return Err(StreamError::BorrowLimitReached(
                ReceiveError::ExceedsMaxBorrows,
            ));
        };
        match self.endpoint.receive() {
            Ok(Some(sample)) => Ok(Some(ReceivedSliceSample {
                sample,
                _borrow: reservation.commit(),
                _message: PhantomData,
                _node: Arc::clone(&self._node),
            })),
            Ok(None) => {
                drop(reservation);
                Ok(None)
            }
            Err(error) => {
                if matches!(error, ReceiveError::ExceedsMaxBorrows) {
                    self.pressure.borrow_failed();
                }
                drop(reservation);
                Err(map_receive_error(error))
            }
        }
    }

    /// Returns this subscriber wrapper's self-reported pressure snapshot.
    pub fn pressure(&self) -> StreamPressure {
        self.pressure.snapshot()
    }
}

/// A received user header plus borrowed payload slice.
///
/// References returned by [`Self::header`] and [`Self::payload`] are tied to
/// `&self`; they cannot outlive this guard or its iceoryx2 sample.
pub struct ReceivedSliceSample<M>
where
    M: SliceMessageAbi,
    M::Header: ZeroCopySend,
    M::Element: ZeroCopySend,
{
    sample: IoxSample<ipc_threadsafe::Service, [M::Element], M::Header>,
    _borrow: BorrowTracker,
    _message: PhantomData<M>,
    // Dropped last (Rust drops fields in declaration order): a node destroyed
    // while one of its ports is still referenced by this sample cannot remove
    // its node directory and leaves it behind in the iceoryx2 root.
    _node: Arc<Node<ipc_threadsafe::Service>>,
}

impl<M> ReceivedSliceSample<M>
where
    M: SliceMessageAbi,
    M::Header: ZeroCopySend,
    M::Element: ZeroCopySend,
{
    /// Borrows the fixed user header for at most this guard's lifetime.
    pub fn header(&self) -> &M::Header {
        self.sample.user_header()
    }

    /// Borrows the payload for at most this guard's lifetime.
    pub fn payload(&self) -> &[M::Element] {
        self.sample.payload()
    }
}

impl<M> fmt::Debug for ReceivedSliceSample<M>
where
    M: SliceMessageAbi,
    M::Header: ZeroCopySend,
    M::Element: ZeroCopySend,
{
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.sample.fmt(formatter)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    static NEXT_SERVICE: AtomicU64 = AtomicU64::new(0);

    fn unique_service_name() -> String {
        format!(
            "rbt/cap/00000000-0000-4000-8000-{:012x}/stream",
            (u64::from(std::process::id()) << 24) | NEXT_SERVICE.fetch_add(1, Ordering::Relaxed)
        )
    }

    #[test]
    fn fixed_roundtrip_retains_loaned_sample() -> Result<()> {
        let descriptor = Stream::<Frame>::descriptor(unique_service_name(), StreamProfile::Sensor);
        let publisher = Stream::<Frame>::publisher(&descriptor)?;
        let subscriber = Stream::<Frame>::subscriber(&descriptor)?;

        let mut sample = publisher.loan()?;
        sample.sequence = 7;
        sample.value = 42;
        sample.send()?;

        let received = subscriber.receive()?.expect("one sent sample");
        assert_eq!(received.sequence, 7);
        assert_eq!(received.value, 42);
        Ok(())
    }

    #[test]
    fn subscriber_does_not_create_a_missing_service() {
        let descriptor = Stream::<Frame>::descriptor(unique_service_name(), StreamProfile::Sensor);
        assert!(Stream::<Frame>::subscriber(&descriptor).is_err());
    }

    #[test]
    fn mismatched_descriptor_is_rejected_before_opening_transport() {
        let descriptor = StreamDescriptor::new(
            unique_service_name(),
            Detection::descriptor(),
            StreamProfile::Sensor,
        );
        let error = Stream::<Frame>::subscriber(&descriptor).unwrap_err();
        assert!(matches!(error, StreamError::AbiMismatch { .. }));
    }

    #[test]
    fn missing_readiness_feature_is_rejected_before_transport_open() {
        let descriptor = StreamDescriptor::new_with_transport(
            unique_service_name(),
            Frame::descriptor(),
            StreamProfile::Sensor,
            rbt_types::TransportFeatures::default(),
        );

        for error in [
            Stream::<Frame>::publisher(&descriptor).unwrap_err(),
            Stream::<Frame>::subscriber(&descriptor).unwrap_err(),
        ] {
            assert!(matches!(
                error,
                StreamError::MissingTransportFeature {
                    required: ReadinessProtocol::SidecarV1,
                    provider: ReadinessProtocol::None,
                }
            ));
        }
    }
}

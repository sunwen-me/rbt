//! Composer-side snapshot control-plane server.
//!
//! The composer owns topology metadata and desired lifecycle commands. It can
//! restart without durable state because connected components immediately
//! publish complete snapshots, which are reconstructed during a short barrier.

use std::{collections::BTreeMap, sync::Arc, time::Duration};

use rbt_core::{
    ActivationId, CapabilityEndpoint, CapabilityInstanceId, CapabilityTransportKind, ComponentId,
    ComponentState, ComposerEpoch, MessageTypeDescriptor, ProcessInstanceId, ReadinessProtocol,
    Revision, RpcPressure, RpcReadinessProtocol, RpcResourceConfig, StreamPressure, StreamProfile,
    StreamResourceConfig,
};
use serde::{Deserialize, Serialize};
use tokio::{
    io::{AsyncBufReadExt, AsyncWriteExt, BufReader, BufWriter},
    net::{
        TcpListener, ToSocketAddrs,
        tcp::{OwnedReadHalf, OwnedWriteHalf},
    },
    sync::{Mutex, mpsc},
    time::sleep,
};
use tracing::{debug, info, warn};

use crate::{
    component::ComponentError,
    manifest::{ComponentSnapshot, SnapshotValidationError},
    protocol::{COMPOSER_SNAPSHOT_VERSION, CONTROL_PROTOCOL_VERSION, ClientMessage, ServerMessage},
    reconcile::{ReconcilePlan, Reconciler},
    registry::CapabilityRegistry,
};

/// Errors returned by the Composer control-plane server and component runner.
#[derive(Debug, thiserror::Error)]
pub enum ControlPlaneError {
    /// A component could not complete its one-time preparation step.
    #[error("component failed before connecting to the control plane: {0}")]
    Component(#[source] ComponentError),
    /// The component-side address could not be resolved.
    #[error("failed to resolve composer address: {0}")]
    ResolveAddress(#[source] std::io::Error),
    /// The Composer listener could not be bound.
    #[error("failed to bind composer control-plane listener: {0}")]
    Bind(#[source] std::io::Error),
    /// The Composer listener could not accept a component connection.
    #[error("failed to accept composer connection: {0}")]
    Accept(#[source] std::io::Error),
}

type SharedWriter = Arc<Mutex<BufWriter<OwnedWriteHalf>>>;

/// Composer startup phase. No destructive lifecycle commands are emitted
/// while recovering snapshots from already-running components.
#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ComposerPhase {
    /// Snapshot reconstruction barrier (two seconds per serve incarnation).
    Recovering,
    /// Normal snapshot-driven reconciliation.
    Running,
}

impl ComposerPhase {
    fn allows_lifecycle_commands(self) -> bool {
        matches!(self, Self::Running)
    }
}

/// Read-only connection status included in an inspection response.
#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum ControlConnectionStatus {
    /// The component has a live control connection.
    Connected,
    /// The component is known but has no live control connection.
    Disconnected,
    /// More than one process currently claims the component identity.
    Conflict,
}

/// One pending lifecycle operation as reported by inspection.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct PendingInspection {
    /// Operation name (`activate` or `deactivate`).
    pub operation: String,
    /// Composer revision that owns the pending operation.
    pub revision: Revision,
    /// Human-readable reason associated with the operation.
    pub reason: String,
}

/// The last activation context known for a component.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct ActiveContext {
    /// Process identity owning the activation.
    pub process_instance: ProcessInstanceId,
    /// Activation identity.
    pub activation_id: ActivationId,
    /// Requirement bindings consumed by that activation.
    pub bindings: Vec<rbt_core::ResolvedBinding>,
    /// Capabilities owned by the activation.
    pub capabilities: Vec<CapabilityInspection>,
}

/// One component's immutable diagnostic state.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct ComponentInspection {
    /// Stable component identity.
    pub component_id: ComponentId,
    /// Current trusted component snapshot, including its lifecycle state.
    pub snapshot: ComponentSnapshot,
    /// Whether the composer has received a complete snapshot on this connection.
    pub snapshot_received: bool,
    /// Current control-plane connection state.
    pub control_connection: ControlConnectionStatus,
    /// Pending lifecycle operation, if one is awaiting a new snapshot.
    pub pending: Option<PendingInspection>,
    /// Most recent active context observed for this process lifetime.
    pub last_active: Option<ActiveContext>,
}

/// One active capability in the composer topology.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct CapabilityInspection {
    /// Semantic capability role used for requirement matching.
    pub capability_type: String,
    /// Activation-owned capability publication identity.
    pub capability_instance: CapabilityInstanceId,
    /// Logical component that provides this capability.
    pub provider_component: ComponentId,
    /// Process identity that owns the publication.
    pub provider_process: ProcessInstanceId,
    /// Activation identity that owns the publication.
    pub activation: ActivationId,
    /// Physical endpoint/service name used by the local transport.
    pub physical_endpoint: String,
    /// Transport-specific endpoint metadata and observations.
    #[serde(flatten)]
    pub transport: CapabilityTransportInspection,
}

/// Transport-specific diagnostic metadata for one active capability.
///
/// The tagged shape is intentional: RPC endpoints do not have stream queue,
/// history, overflow, or pressure semantics, so the diagnostic representation
/// never fabricates those fields for RPC capabilities.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(tag = "transport_kind", rename_all = "snake_case")]
pub enum CapabilityTransportInspection {
    /// Stream metadata and low-rate pressure observations.
    Stream {
        /// Complete message ABI descriptor.
        message_type: MessageTypeDescriptor,
        /// Intended stream profile.
        profile: StreamProfile,
        /// Provider-advertised payload-free readiness protocol.
        readiness: ReadinessProtocol,
        /// Known subscriber queue depth for this profile.
        queue_depth: usize,
        /// Number of samples retained for late subscribers, when reported.
        #[serde(skip_serializing_if = "Option::is_none")]
        history_size: Option<usize>,
        /// Whether this profile enables safe overflow semantics.
        safe_overflow: bool,
        /// Descriptor-declared endpoint resource limits.
        resources: StreamResourceConfig,
        /// Most recent low-rate publisher observation for this physical endpoint.
        ///
        /// `None` means the provider has not reported an rbt wrapper snapshot;
        /// it must not be interpreted as zero pressure.
        pressure: Option<StreamPressure>,
        /// Factual age and freshness of [`StreamPressure`].
        #[serde(skip_serializing_if = "Option::is_none")]
        pressure_freshness: Option<PressureFreshness>,
        /// Current subscriber observations matched by physical service name.
        ///
        /// This vector is deliberately bounded and contains no historical
        /// observations.
        subscriber_observations: Vec<SubscriberPressureObservation>,
        /// Conservative, factual publisher-side assessment reasons.
        assessments: Vec<PressureAssessment>,
    },
    /// Request/response metadata without stream-only telemetry.
    Rpc {
        /// Complete request message ABI descriptor.
        request_type: MessageTypeDescriptor,
        /// Complete response message ABI descriptor.
        response_type: MessageTypeDescriptor,
        /// Provider-advertised RPC readiness protocol.
        readiness: RpcReadinessProtocol,
        /// Descriptor-declared request/response resource limits.
        resources: RpcResourceConfig,
        /// Most recent server-side low-rate pressure observation for this
        /// physical RPC endpoint. `None` means the provider has not reported
        /// a wrapper snapshot; it must not be interpreted as zero pressure.
        #[serde(default)]
        server_pressure: Option<RpcPressure>,
        /// Factual age and freshness of [`server_pressure`].
        #[serde(default, skip_serializing_if = "Option::is_none")]
        server_pressure_freshness: Option<PressureFreshness>,
        /// Current client-side observations matched by exact physical service
        /// name and capability instance. This vector contains no history.
        #[serde(default)]
        client_observations: Vec<RpcClientPressureObservation>,
    },
}

impl CapabilityTransportInspection {
    /// Returns the transport kind represented by this inspection.
    pub const fn transport_kind(&self) -> CapabilityTransportKind {
        match self {
            Self::Stream { .. } => CapabilityTransportKind::Stream,
            Self::Rpc { .. } => CapabilityTransportKind::Rpc,
        }
    }
}

/// Display-only freshness metadata for one stream or RPC pressure snapshot.
///
/// Freshness is intentionally not a health score.  It only reports the age
/// of the timestamp carried by the snapshot and whether that age is within
/// [`PRESSURE_FRESHNESS_WINDOW_MS`].
#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct PressureFreshness {
    /// Timestamp carried by the pressure snapshot.
    pub snapshot_timestamp_unix_ms: u64,
    /// Saturating wall-clock age at inspection time. Future timestamps have
    /// age zero.
    pub age_ms: u64,
    /// Whether `age_ms` is within the display-only freshness window.
    pub fresh: bool,
}

/// A current subscriber-side pressure observation for one physical stream.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct SubscriberPressureObservation {
    /// Stable component identity publishing this observation.
    pub component_id: ComponentId,
    /// Runtime process identity that produced the component snapshot.
    pub process_instance_id: ProcessInstanceId,
    /// Activation identity carried by the reporting component snapshot, when
    /// the endpoint belongs to an activation that is still represented there.
    pub activation_id: Option<ActivationId>,
    /// Complete subscriber-side stream pressure snapshot.
    pub pressure: StreamPressure,
    /// Saturating age derived from `pressure.snapshot_timestamp_unix_ms`.
    pub age_ms: u64,
    /// Whether `age_ms` is within the display-only freshness window.
    pub fresh: bool,
    /// Conservative, factual subscriber-side assessment reasons.
    pub assessments: Vec<PressureAssessment>,
}

/// A current client-side pressure observation for one physical RPC endpoint.
///
/// Observations are matched by physical service name and capability instance,
/// never by logical capability type. They are bounded snapshots rather than
/// per-call history or control-plane events.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct RpcClientPressureObservation {
    /// Stable component identity reporting this observation.
    pub component_id: ComponentId,
    /// Runtime process identity that produced the component snapshot.
    pub process_instance_id: rbt_core::ProcessInstanceId,
    /// Client activation identity, when the reporting endpoint is activation
    /// owned.
    pub activation_id: Option<ActivationId>,
    /// Complete client-side RPC pressure snapshot.
    pub pressure: RpcPressure,
    /// Saturating age derived from `pressure.snapshot_timestamp_unix_ms`.
    pub age_ms: u64,
    /// Whether `age_ms` is within the display-only freshness window.
    pub fresh: bool,
}

/// Maximum number of current subscriber observations retained per capability.
pub const MAX_SUBSCRIBER_PRESSURE_OBSERVATIONS: usize = 64;

/// Maximum number of current client pressure observations retained per RPC
/// capability.
pub const MAX_RPC_CLIENT_PRESSURE_OBSERVATIONS: usize = 64;

/// Display-only freshness cutoff for pressure snapshots.
pub const PRESSURE_FRESHNESS_WINDOW_MS: u64 = 2_500;

/// Typed reason codes emitted by conservative pressure inspection.
#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum PressureAssessmentReason {
    /// A loan pool reached its configured limit and an exhaustion was seen.
    LoanCapacityReached,
    /// A subscriber reached its configured borrow limit and a failure was seen.
    BorrowCapacityReached,
    /// At least one subscriber borrow remains outstanding.
    OutstandingBorrow,
    /// A queue-full or unable-to-deliver event/counter was observed.
    ControlDeliveryPressure,
    /// A readiness notification failure was observed.
    ReadinessFailuresObserved,
}

/// One conservative pressure assessment.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct PressureAssessment {
    /// Machine-readable reason code.
    pub reason: PressureAssessmentReason,
    /// Optional operator guidance based only on the observed counters.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub recommendation: Option<String>,
    /// Approximate payload-only bytes for the suggested extra loans, when a
    /// maximum payload size was observed.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub approximate_payload_only_bytes: Option<u64>,
}

/// One process-identity conflict currently quarantined by the composer.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct ProcessConflictInspection {
    /// Stable component identity claimed by both processes.
    pub component_id: ComponentId,
    /// Process that was connected first.
    pub incumbent: ProcessInstanceId,
    /// Process that attempted to supersede the incumbent.
    pub contender: ProcessInstanceId,
}

/// Immutable composer diagnostic state returned by an inspection request.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct InspectionView {
    /// Composer incarnation identity.
    pub epoch: ComposerEpoch,
    /// Current revision; inspection does not advance it.
    pub revision: Revision,
    /// Current composer startup/runtime phase.
    pub phase: ComposerPhase,
    /// Components currently known to this composer.
    pub components: Vec<ComponentInspection>,
    /// Active-only capability topology.
    pub capability_topology: Vec<CapabilityInspection>,
    /// Process conflicts currently quarantined.
    pub process_conflicts: Vec<ProcessConflictInspection>,
}

/// Serializable inspection response wrapper retained for embedders that do
/// not need the surrounding NDJSON [`ServerMessage`].
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct InspectionResponse {
    /// Immutable diagnostic view.
    pub view: InspectionView,
}

impl From<InspectionView> for InspectionResponse {
    fn from(view: InspectionView) -> Self {
        Self { view }
    }
}

/// Returns the frozen queue depth associated with a stream profile.
pub const fn queue_depth_for_profile(profile: StreamProfile) -> usize {
    match profile {
        StreamProfile::Sensor => 1,
        StreamProfile::Control => 4,
    }
}

/// Returns whether a stream profile uses safe overflow semantics.
pub const fn safe_overflow_for_profile(profile: StreamProfile) -> bool {
    matches!(profile, StreamProfile::Sensor)
}

fn now_unix_ms() -> u64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .ok()
        .and_then(|duration| u64::try_from(duration.as_millis()).ok())
        .unwrap_or(u64::MAX)
}

fn pressure_freshness(pressure: &StreamPressure) -> PressureFreshness {
    pressure_freshness_at(pressure, now_unix_ms())
}

fn pressure_freshness_at(pressure: &StreamPressure, now_ms: u64) -> PressureFreshness {
    pressure_freshness_at_timestamp(pressure.snapshot_timestamp_unix_ms, now_ms)
}

fn rpc_pressure_freshness(pressure: &RpcPressure) -> PressureFreshness {
    pressure_freshness_at_timestamp(pressure.snapshot_timestamp_unix_ms, now_unix_ms())
}

fn pressure_freshness_at_timestamp(
    snapshot_timestamp_unix_ms: u64,
    now_ms: u64,
) -> PressureFreshness {
    let age_ms = now_ms.saturating_sub(snapshot_timestamp_unix_ms);
    PressureFreshness {
        snapshot_timestamp_unix_ms,
        age_ms,
        fresh: age_ms <= PRESSURE_FRESHNESS_WINDOW_MS,
    }
}

fn event_is_loan_exhausted(pressure: &StreamPressure) -> bool {
    matches!(
        pressure.last_pressure_event,
        Some(rbt_core::PressureEvent::LoanPoolExhausted)
    )
}

fn event_is_borrow_exhausted(pressure: &StreamPressure) -> bool {
    matches!(
        pressure.last_pressure_event,
        Some(rbt_core::PressureEvent::BorrowLimitReached)
    )
}

fn event_is_control_delivery_pressure(pressure: &StreamPressure) -> bool {
    matches!(
        pressure.last_pressure_event,
        Some(rbt_core::PressureEvent::QueueFull { .. } | rbt_core::PressureEvent::UnableToDeliver)
    )
}

/// Computes conservative, typed reason codes from factual pressure counters.
///
/// No age threshold, health score, or automatic tuning decision is made here.
/// The optional payload estimate is deliberately payload-only and saturating;
/// allocator, headers, alignment, and transport overhead are excluded.
pub fn assess_stream_pressure(pressure: &StreamPressure) -> Vec<PressureAssessment> {
    let mut assessments = Vec::new();
    if let Some(publisher) = pressure.publisher {
        let max_loans = u64::try_from(pressure.resources.max_loans).unwrap_or(u64::MAX);
        let loan_exhausted = max_loans > 0
            && publisher.loans_high_water >= max_loans
            && (publisher.loan_failures > 0 || event_is_loan_exhausted(pressure));
        if loan_exhausted {
            let approximate_payload_only_bytes = (publisher.payload_bytes_max_seen > 0)
                .then(|| publisher.payload_bytes_max_seen.saturating_mul(1));
            assessments.push(PressureAssessment {
                reason: PressureAssessmentReason::LoanCapacityReached,
                recommendation: Some(
                    "if the memory budget permits, test max_loans+1 or greater; inspect the approximate payload-only estimate before changing limits".to_owned(),
                ),
                approximate_payload_only_bytes,
            });
        }

        if publisher.queue_full > 0
            || publisher.unable_to_deliver > 0
            || event_is_control_delivery_pressure(pressure)
        {
            assessments.push(PressureAssessment {
                reason: PressureAssessmentReason::ControlDeliveryPressure,
                recommendation: Some(
                    "inspect subscriber observations individually; queue_full does not imply that no subscriber received the sample".to_owned(),
                ),
                approximate_payload_only_bytes: None,
            });
        }

        if publisher.readiness_notify_failures > 0 {
            assessments.push(PressureAssessment {
                reason: PressureAssessmentReason::ReadinessFailuresObserved,
                recommendation: Some(
                    "inspect readiness sidecar connectivity and notification failures".to_owned(),
                ),
                approximate_payload_only_bytes: None,
            });
        }
    }

    if let Some(subscriber) = pressure.subscriber {
        let max_borrowed = u64::try_from(pressure.resources.max_borrowed).unwrap_or(u64::MAX);
        let borrow_exhausted = max_borrowed > 0
            && subscriber.borrowed_high_water >= max_borrowed
            && (subscriber.borrow_failures > 0 || event_is_borrow_exhausted(pressure));
        if borrow_exhausted {
            assessments.push(PressureAssessment {
                reason: PressureAssessmentReason::BorrowCapacityReached,
                recommendation: Some(
                    "inspect ownership and release retained borrows before increasing max_borrowed"
                        .to_owned(),
                ),
                approximate_payload_only_bytes: None,
            });
        }
        if subscriber.borrowed_samples > 0 {
            assessments.push(PressureAssessment {
                reason: PressureAssessmentReason::OutstandingBorrow,
                recommendation: Some(
                    "inspect ownership and release retained borrows before increasing max_borrowed"
                        .to_owned(),
                ),
                approximate_payload_only_bytes: None,
            });
        }
    }
    assessments
}

impl CapabilityInspection {
    /// Returns the transport kind represented by this inspection.
    pub const fn transport_kind(&self) -> CapabilityTransportKind {
        self.transport.transport_kind()
    }

    fn from_descriptor(
        capability_type: impl Into<String>,
        descriptor: &rbt_core::CapabilityDescriptor,
    ) -> Self {
        let transport = match descriptor.endpoint() {
            CapabilityEndpoint::Stream(stream) => {
                let profile = stream.profile();
                CapabilityTransportInspection::Stream {
                    message_type: stream.message_type().clone(),
                    profile,
                    readiness: stream.readiness(),
                    queue_depth: queue_depth_for_profile(profile),
                    history_size: None,
                    safe_overflow: safe_overflow_for_profile(profile),
                    resources: stream.resources,
                    pressure: None,
                    pressure_freshness: None,
                    subscriber_observations: Vec::new(),
                    assessments: Vec::new(),
                }
            }
            CapabilityEndpoint::Rpc(rpc) => CapabilityTransportInspection::Rpc {
                request_type: rpc.request_type().clone(),
                response_type: rpc.response_type().clone(),
                readiness: rpc.readiness(),
                resources: rpc.resources(),
                server_pressure: None,
                server_pressure_freshness: None,
                client_observations: Vec::new(),
            },
        };
        Self {
            capability_type: capability_type.into(),
            capability_instance: descriptor.capability_instance,
            provider_component: descriptor.provider_component.clone(),
            provider_process: descriptor.provider_process,
            activation: descriptor.activation,
            physical_endpoint: descriptor.service_name().to_owned(),
            transport,
        }
    }

    fn attach_pressure(&mut self, pressure: Option<StreamPressure>) {
        let Some(pressure) = pressure else {
            return;
        };
        if let CapabilityTransportInspection::Stream {
            queue_depth,
            history_size,
            safe_overflow,
            resources,
            pressure: attached_pressure,
            pressure_freshness: attached_freshness,
            assessments,
            ..
        } = &mut self.transport
        {
            *queue_depth = pressure.semantic.queue_depth;
            *history_size = Some(pressure.semantic.history_size);
            *safe_overflow = pressure.semantic.safe_overflow;
            *resources = pressure.resources;
            *attached_freshness = Some(pressure_freshness(&pressure));
            *assessments = assess_stream_pressure(&pressure);
            *attached_pressure = Some(pressure);
        }
    }

    fn attach_subscriber_observations(&mut self, observations: Vec<SubscriberPressureObservation>) {
        if let CapabilityTransportInspection::Stream {
            subscriber_observations,
            ..
        } = &mut self.transport
        {
            *subscriber_observations = observations;
        }
    }

    fn attach_rpc_server_pressure(&mut self, pressure: Option<RpcPressure>) {
        let Some(pressure) = pressure else {
            return;
        };
        if let CapabilityTransportInspection::Rpc {
            server_pressure,
            server_pressure_freshness,
            ..
        } = &mut self.transport
        {
            *server_pressure_freshness = Some(rpc_pressure_freshness(&pressure));
            *server_pressure = Some(pressure);
        }
    }

    fn attach_rpc_client_observations(&mut self, observations: Vec<RpcClientPressureObservation>) {
        if let CapabilityTransportInspection::Rpc {
            client_observations,
            ..
        } = &mut self.transport
        {
            *client_observations = observations;
        }
    }
}

#[derive(Debug)]
enum Event {
    Message {
        connection_id: u64,
        writer: SharedWriter,
        message: Box<ClientMessage>,
    },
    Disconnected {
        connection_id: u64,
    },
}

#[derive(Clone, Debug)]
struct ConnectionInfo {
    component_id: ComponentId,
    process_instance: ProcessInstanceId,
}

#[derive(Clone, Debug)]
enum PendingOperation {
    Activate {
        revision: Revision,
        bindings: Vec<rbt_core::ResolvedBinding>,
    },
    Deactivate {
        revision: Revision,
        activation: ActivationId,
        process_instance: ProcessInstanceId,
        reason: String,
    },
}

struct ComponentRecord {
    snapshot: ComponentSnapshot,
    /// Retained across Blocked/Failed snapshots so diagnostics can explain
    /// which activation and providers preceded the current state.
    last_active: Option<ActiveContext>,
    /// A Hello alone is not enough to plan: wait for the first complete
    /// snapshot so a zero-requirement placeholder cannot trigger Activate.
    snapshot_received: bool,
    connection_id: u64,
    writer: SharedWriter,
    pending: Option<PendingOperation>,
}

impl From<&PendingOperation> for PendingInspection {
    fn from(operation: &PendingOperation) -> Self {
        match operation {
            PendingOperation::Activate { revision, bindings } => Self {
                operation: "activate".to_owned(),
                revision: *revision,
                reason: if bindings.is_empty() {
                    "activation pending; no bindings declared".to_owned()
                } else {
                    format!(
                        "activation pending; waiting for snapshot after {} resolved binding(s)",
                        bindings.len()
                    )
                },
            },
            PendingOperation::Deactivate {
                revision, reason, ..
            } => Self {
                operation: "deactivate".to_owned(),
                revision: *revision,
                reason: reason.clone(),
            },
        }
    }
}

impl ComponentRecord {
    fn process_instance(&self) -> ProcessInstanceId {
        self.snapshot.process_instance
    }
}

struct ConflictRecord {
    incumbent: ProcessInstanceId,
    contender: ProcessInstanceId,
    incumbent_connection: u64,
    contender_connection: u64,
    incumbent_writer: SharedWriter,
    contender_writer: SharedWriter,
    /// Last valid snapshot from the incumbent. Snapshots received while both
    /// process identities are live are deliberately not imported.
    incumbent_snapshot: ComponentSnapshot,
    incumbent_snapshot_received: bool,
    incumbent_last_active: Option<ActiveContext>,
    /// The contender has no trusted snapshot until ambiguity is resolved.
    contender_snapshot: Option<ComponentSnapshot>,
    contender_last_active: Option<ActiveContext>,
}

struct Composer {
    epoch: ComposerEpoch,
    revision: Revision,
    phase: ComposerPhase,
    registry: CapabilityRegistry,
    reconciler: Reconciler,
    records: BTreeMap<ComponentId, ComponentRecord>,
    connections: BTreeMap<u64, ConnectionInfo>,
    conflicts: BTreeMap<ComponentId, ConflictRecord>,
}

impl Composer {
    fn new(epoch: ComposerEpoch) -> Self {
        let registry = CapabilityRegistry::new();
        Self {
            epoch,
            revision: Revision::initial(epoch),
            phase: ComposerPhase::Recovering,
            reconciler: Reconciler::new(registry.clone()),
            registry,
            records: BTreeMap::new(),
            connections: BTreeMap::new(),
            conflicts: BTreeMap::new(),
        }
    }

    /// Returns this serve incarnation's epoch.
    #[allow(dead_code)]
    pub const fn epoch(&self) -> ComposerEpoch {
        self.epoch
    }

    /// Returns the current revision in this epoch.
    #[allow(dead_code)]
    pub const fn revision(&self) -> Revision {
        self.revision
    }

    /// Returns the current startup/steady-state phase.
    #[allow(dead_code)]
    pub const fn phase(&self) -> ComposerPhase {
        self.phase
    }

    async fn process(&mut self, event: Event) {
        match event {
            Event::Message {
                connection_id,
                writer,
                message,
            } => match *message {
                ClientMessage::Hello {
                    component_id,
                    process_instance,
                    control_protocol_version,
                    snapshot_version,
                } => {
                    self.hello(
                        connection_id,
                        writer,
                        component_id,
                        process_instance,
                        control_protocol_version,
                        snapshot_version,
                    )
                    .await;
                }
                ClientMessage::Snapshot { snapshot } => {
                    self.snapshot(connection_id, snapshot).await;
                }
                ClientMessage::Inspect {
                    control_protocol_version,
                    snapshot_version,
                } => {
                    self.inspect(writer, control_protocol_version, snapshot_version)
                        .await;
                }
            },
            Event::Disconnected { connection_id } => {
                self.disconnected(connection_id).await;
            }
        }
    }

    async fn hello(
        &mut self,
        connection_id: u64,
        writer: SharedWriter,
        component_id: ComponentId,
        process_instance: ProcessInstanceId,
        control_protocol_version: u16,
        snapshot_version: u16,
    ) {
        if control_protocol_version != CONTROL_PROTOCOL_VERSION
            || snapshot_version != COMPOSER_SNAPSHOT_VERSION
        {
            let reason = version_mismatch_reason(control_protocol_version, snapshot_version);
            let mismatch = ServerMessage::ProtocolMismatch {
                expected_control_protocol_version: CONTROL_PROTOCOL_VERSION,
                expected_snapshot_version: COMPOSER_SNAPSHOT_VERSION,
                received_control_protocol_version: Some(control_protocol_version),
                received_snapshot_version: Some(snapshot_version),
                reason,
            };
            if let Err(error) = send_message(&writer, &mismatch).await {
                debug!(connection_id, %error, "failed to send protocol mismatch diagnostic");
            }
            warn!(
                connection_id,
                component = %component_id,
                control_protocol_version,
                snapshot_version,
                expected_control_protocol_version = CONTROL_PROTOCOL_VERSION,
                expected_snapshot_version = COMPOSER_SNAPSHOT_VERSION,
                "ignoring unsupported component protocol"
            );
            return;
        }
        if self.connections.contains_key(&connection_id) {
            warn!(connection_id, component = %component_id, "ignoring duplicate Hello on connection");
            return;
        }

        // If this identity is already ambiguous, a same-process reconnect
        // updates only that side's writer. Both sides remain quarantined until
        // the other live connection disappears.
        if self.conflicts.contains_key(&component_id) {
            self.conflict_hello(connection_id, writer, component_id, process_instance)
                .await;
            return;
        }

        if let Some(existing) = self.records.get(&component_id) {
            let incumbent_process = existing.process_instance();
            let incumbent_connection = existing.connection_id;
            if incumbent_process == process_instance {
                let old_connection = incumbent_connection;
                self.connections.remove(&old_connection);
                self.connections.insert(
                    connection_id,
                    ConnectionInfo {
                        component_id: component_id.clone(),
                        process_instance,
                    },
                );
                if let Some(record) = self.records.get_mut(&component_id) {
                    record.connection_id = connection_id;
                    record.writer = writer;
                }
                info!(
                    component = %component_id,
                    process_instance = %process_instance,
                    old_connection,
                    connection_id,
                    composer_epoch = %self.epoch,
                    revision = %self.revision,
                    "same-process control reconnect replaced connection"
                );
                let pending = self
                    .records
                    .get(&component_id)
                    .and_then(|record| record.pending.clone());
                if let Some(message) = pending_message(pending)
                    && let Err(error) = send_message(
                        &self
                            .records
                            .get(&component_id)
                            .expect("record exists")
                            .writer,
                        &message,
                    )
                    .await
                {
                    warn!(component = %component_id, %error, "failed to resend pending command after reconnect");
                }
                return;
            }

            // `records` is removed only after we have copied its trusted
            // snapshot. Do not silently choose the latest process identity:
            // two live processes with one ComponentId are ambiguous.
            let old_live = self
                .connections
                .get(&incumbent_connection)
                .is_some_and(|info| info.component_id == component_id);
            if old_live {
                self.start_conflict(component_id, process_instance, connection_id, writer)
                    .await;
                return;
            }

            // A record without a live connection is defensive cleanup for an
            // out-of-order event; its process liveness is already invalid.
            self.records.remove(&component_id);
            self.registry.remove_provider(&component_id);
        }

        self.connections.insert(
            connection_id,
            ConnectionInfo {
                component_id: component_id.clone(),
                process_instance,
            },
        );
        self.records.insert(
            component_id.clone(),
            ComponentRecord {
                snapshot: ComponentSnapshot::placeholder(component_id.clone(), process_instance),
                last_active: None,
                snapshot_received: false,
                connection_id,
                writer,
                pending: None,
            },
        );
        info!(
            component = %component_id,
            process_instance = %process_instance,
            connection_id,
            composer_epoch = %self.epoch,
            revision = %self.revision,
            "component Hello received"
        );
        if self.phase == ComposerPhase::Running {
            self.reconcile().await;
        }
    }

    async fn inspect(
        &self,
        writer: SharedWriter,
        control_protocol_version: u16,
        snapshot_version: u16,
    ) {
        if control_protocol_version != CONTROL_PROTOCOL_VERSION
            || snapshot_version != COMPOSER_SNAPSHOT_VERSION
        {
            let mismatch = ServerMessage::ProtocolMismatch {
                expected_control_protocol_version: CONTROL_PROTOCOL_VERSION,
                expected_snapshot_version: COMPOSER_SNAPSHOT_VERSION,
                received_control_protocol_version: Some(control_protocol_version),
                received_snapshot_version: Some(snapshot_version),
                reason: version_mismatch_reason(control_protocol_version, snapshot_version),
            };
            if let Err(error) = send_message(&writer, &mismatch).await {
                debug!(%error, "failed to send inspection protocol mismatch diagnostic");
            }
            return;
        }

        let response = ServerMessage::InspectResponse {
            view: self.inspection_view(),
        };
        if let Err(error) = send_message(&writer, &response).await {
            debug!(%error, "failed to send composer inspection response");
        }
    }

    fn inspection_view(&self) -> InspectionView {
        let mut components = self
            .records
            .values()
            .map(|record| ComponentInspection {
                component_id: record.snapshot.component_id.clone(),
                snapshot: record.snapshot.clone(),
                snapshot_received: record.snapshot_received,
                control_connection: if self.connections.get(&record.connection_id).is_some_and(
                    |connection| connection.component_id == record.snapshot.component_id,
                ) {
                    ControlConnectionStatus::Connected
                } else {
                    ControlConnectionStatus::Disconnected
                },
                pending: record.pending.as_ref().map(PendingInspection::from),
                last_active: record.last_active.clone(),
            })
            .collect::<Vec<_>>();

        // A conflicted component has no trusted record while both process
        // identities are live. Expose the incumbent snapshot as the current
        // diagnostic snapshot and surface the ambiguity separately below.
        for (component_id, conflict) in &self.conflicts {
            components.push(ComponentInspection {
                component_id: component_id.clone(),
                snapshot: conflict.incumbent_snapshot.clone(),
                snapshot_received: conflict.incumbent_snapshot_received,
                control_connection: ControlConnectionStatus::Conflict,
                pending: None,
                last_active: conflict.incumbent_last_active.clone(),
            });
        }
        components.sort_by(|left, right| left.component_id.cmp(&right.component_id));

        let pressure_by_endpoint = components
            .iter()
            .flat_map(|component| {
                component
                    .snapshot
                    .stream_pressures
                    .iter()
                    .filter_map(move |pressure| {
                        pressure.publisher.as_ref()?;
                        Some((
                            (
                                component.snapshot.process_instance,
                                pressure.service_name.clone(),
                            ),
                            pressure.clone(),
                        ))
                    })
            })
            .collect::<BTreeMap<_, _>>();
        let mut capability_topology = Vec::new();
        for (capability_type, providers) in self.registry.topology() {
            for registered in providers.into_values() {
                let service_name = registered.descriptor.service_name().to_owned();
                let mut inspection = CapabilityInspection::from_descriptor(
                    capability_type.clone(),
                    &registered.descriptor,
                );
                if inspection.transport_kind() == CapabilityTransportKind::Rpc {
                    inspection.attach_rpc_server_pressure(rpc_server_pressure(
                        &components,
                        &registered.descriptor,
                    ));
                    inspection.attach_rpc_client_observations(rpc_client_observations(
                        &components,
                        &registered.descriptor,
                    ));
                    capability_topology.push(inspection);
                    continue;
                }
                inspection.attach_pressure(
                    pressure_by_endpoint
                        .get(&(registered.descriptor.provider_process, service_name.clone()))
                        .cloned(),
                );
                inspection.attach_subscriber_observations(subscriber_observations(
                    &components,
                    &service_name,
                ));
                capability_topology.push(inspection);
            }
        }
        let process_conflicts = self
            .conflicts
            .iter()
            .map(|(component_id, conflict)| ProcessConflictInspection {
                component_id: component_id.clone(),
                incumbent: conflict.incumbent,
                contender: conflict.contender,
            })
            .collect();

        InspectionView {
            epoch: self.epoch,
            revision: self.revision,
            phase: self.phase,
            components,
            capability_topology,
            process_conflicts,
        }
    }

    async fn conflict_hello(
        &mut self,
        connection_id: u64,
        writer: SharedWriter,
        component_id: ComponentId,
        process_instance: ProcessInstanceId,
    ) {
        let Some(conflict) = self.conflicts.get_mut(&component_id) else {
            return;
        };
        let process = process_instance;
        if process == conflict.incumbent {
            self.connections.remove(&conflict.incumbent_connection);
            conflict.incumbent_connection = connection_id;
            conflict.incumbent_writer = writer;
        } else if process == conflict.contender {
            self.connections.remove(&conflict.contender_connection);
            conflict.contender_connection = connection_id;
            conflict.contender_writer = writer;
        } else {
            warn!(
                component = %component_id,
                process_instance = %process,
                incumbent = %conflict.incumbent,
                contender = %conflict.contender,
                "ignoring third process while component identity is ambiguous"
            );
            return;
        }
        self.connections.insert(
            connection_id,
            ConnectionInfo {
                component_id,
                process_instance,
            },
        );
    }

    async fn start_conflict(
        &mut self,
        component_id: ComponentId,
        contender: ProcessInstanceId,
        contender_connection: u64,
        contender_writer: SharedWriter,
    ) {
        let Some(record) = self.records.remove(&component_id) else {
            return;
        };
        let incumbent = record.process_instance();
        let incumbent_connection = record.connection_id;
        self.registry.remove_provider(&component_id);
        self.connections.insert(
            incumbent_connection,
            ConnectionInfo {
                component_id: component_id.clone(),
                process_instance: incumbent,
            },
        );
        self.connections.insert(
            contender_connection,
            ConnectionInfo {
                component_id: component_id.clone(),
                process_instance: contender,
            },
        );
        let revision = self.next_revision("process conflict");
        self.conflicts.insert(
            component_id.clone(),
            ConflictRecord {
                incumbent,
                contender,
                incumbent_connection,
                contender_connection,
                incumbent_writer: record.writer,
                contender_writer,
                incumbent_snapshot: record.snapshot,
                incumbent_snapshot_received: record.snapshot_received,
                incumbent_last_active: record.last_active,
                contender_snapshot: None,
                contender_last_active: None,
            },
        );
        self.send_process_conflict(&component_id, revision).await;
        info!(
            component = %component_id,
            incumbent = %incumbent,
            contender = %contender,
            composer_epoch = %self.epoch,
            revision = %revision,
            "component process conflict quarantined"
        );
    }

    async fn snapshot(&mut self, connection_id: u64, snapshot: ComponentSnapshot) {
        let Some(connection) = self.connections.get(&connection_id).cloned() else {
            warn!(connection_id, "ignoring component Snapshot before Hello");
            return;
        };
        let id = connection.component_id.clone();
        if self.conflicts.contains_key(&id) {
            // A conflicting process must not win merely by sending a newer
            // snapshot. Keep each side's last snapshot quarantined so the
            // survivor can be imported deterministically after the other
            // connection disappears.
            if let Some(conflict) = self.conflicts.get_mut(&id) {
                let current_active = active_context(&snapshot);
                if connection_id == conflict.incumbent_connection
                    && snapshot.process_instance == conflict.incumbent
                {
                    conflict.incumbent_snapshot = snapshot;
                    conflict.incumbent_snapshot_received = true;
                    if current_active.is_some() {
                        conflict.incumbent_last_active = current_active;
                    }
                } else if connection_id == conflict.contender_connection
                    && snapshot.process_instance == conflict.contender
                {
                    conflict.contender_snapshot = Some(snapshot);
                    if current_active.is_some() {
                        conflict.contender_last_active = current_active;
                    }
                }
            }
            debug!(component = %id, connection_id, "ignoring snapshot while process identity is ambiguous");
            return;
        }
        let Some(record) = self.records.get(&id) else {
            return;
        };
        if record.connection_id != connection_id
            || connection.process_instance != snapshot.process_instance
            || snapshot.component_id != id
        {
            debug!(component = %id, connection_id, "ignoring stale or mismatched component snapshot");
            return;
        }
        if let Err(error) = snapshot.validate() {
            warn!(component = %id, connection_id, %error, "ignoring invalid component snapshot");
            return;
        }

        // Pressure is diagnostic state, not composition truth. A periodic
        // counter update must neither advance the topology revision nor run
        // reconciliation. Keep the newest complete snapshot so inspection
        // sees it, then return before any lifecycle work.
        if self.records.get(&id).is_some_and(|record| {
            record.snapshot_received && same_component_truth(&record.snapshot, &snapshot)
        }) {
            if let Some(record) = self.records.get_mut(&id) {
                record.snapshot.stream_pressures = snapshot.stream_pressures;
                record.snapshot.rpc_pressures = snapshot.rpc_pressures;
                record.snapshot_received = true;
            }
            return;
        }

        let pending = self
            .records
            .get(&id)
            .and_then(|record| record.pending.clone());
        let pending = clear_pending(pending, &snapshot);
        let current_active = active_context(&snapshot);
        if let Some(record) = self.records.get_mut(&id) {
            record.snapshot = snapshot;
            if current_active.is_some() {
                record.last_active = current_active;
            }
            record.snapshot_received = true;
            record.pending = pending;
        }
        let revision = self.next_revision("snapshot import");
        self.rebuild_registry();
        info!(
            component = %id,
            process_instance = %connection.process_instance,
            state = ?self.records.get(&id).map(|record| &record.snapshot.state),
            composer_epoch = %self.epoch,
            revision = %revision,
            "component snapshot imported"
        );
        if self.phase == ComposerPhase::Running {
            self.reconcile().await;
        }
    }

    async fn disconnected(&mut self, connection_id: u64) {
        let Some(connection) = self.connections.remove(&connection_id) else {
            // Delayed disconnect from a superseded same-process connection.
            return;
        };
        let id = connection.component_id.clone();

        if let Some(conflict) = self.conflicts.get(&id) {
            let is_incumbent = conflict.incumbent_connection == connection_id;
            let is_contender = conflict.contender_connection == connection_id;
            if !is_incumbent && !is_contender {
                return;
            }
            let conflict = self.conflicts.remove(&id).expect("conflict existed");
            let (process_instance, writer, snapshot, last_active) = if is_incumbent {
                (
                    conflict.contender,
                    conflict.contender_writer,
                    conflict.contender_snapshot,
                    conflict.contender_last_active,
                )
            } else {
                (
                    conflict.incumbent,
                    conflict.incumbent_writer,
                    Some(conflict.incumbent_snapshot),
                    conflict.incumbent_last_active,
                )
            };
            // With one side gone the identity is no longer ambiguous. The
            // surviving process gets its last trusted snapshot (if any) and
            // may immediately publish a fresher one on its reconnect loop.
            self.records.insert(
                id.clone(),
                ComponentRecord {
                    snapshot: snapshot.unwrap_or_else(|| {
                        ComponentSnapshot::placeholder(id.clone(), process_instance)
                    }),
                    last_active,
                    snapshot_received: if is_incumbent {
                        false
                    } else {
                        conflict.incumbent_snapshot_received
                    },
                    connection_id: if is_incumbent {
                        conflict.contender_connection
                    } else {
                        conflict.incumbent_connection
                    },
                    writer,
                    pending: None,
                },
            );
            self.rebuild_registry();
            info!(component = %id, process_instance = %process_instance, "process conflict resolved after disconnect");
            if self.phase == ComposerPhase::Running {
                self.reconcile().await;
            }
            return;
        }

        let is_current = self
            .records
            .get(&id)
            .is_some_and(|record| record.connection_id == connection_id);
        if !is_current {
            return;
        }
        if let Some(record) = self.records.remove(&id) {
            self.registry
                .remove_process(&id, record.snapshot.process_instance);
        }
        // A disconnect is process liveness loss, not a component lifecycle
        // transition. Remove the record/registrations and let a future Hello
        // reconstruct local truth. During recovery no command is emitted.
        let revision = self.next_revision("component disconnect");
        info!(
            component = %id,
            composer_epoch = %self.epoch,
            revision = %revision,
            phase = ?self.phase,
            "component control liveness lost"
        );
        if self.phase == ComposerPhase::Running {
            self.reconcile().await;
        }
    }

    async fn finish_recovery(&mut self) {
        if self.phase != ComposerPhase::Recovering {
            return;
        }
        self.rebuild_registry();
        let plan = self.compute_plan();
        self.phase = ComposerPhase::Running;
        let revision = self.next_revision("recovery barrier");
        let bindings = self
            .registry
            .topology()
            .into_iter()
            .flat_map(|(capability_type, providers)| {
                providers.into_iter().map(move |(component, registered)| {
                    format!(
                        "{capability_type}:{} -> {component}",
                        registered.descriptor.capability_instance
                    )
                })
            })
            .collect::<Vec<_>>();
        info!(
            composer_epoch = %self.epoch,
            revision = %revision,
            reconstructed_bindings = ?bindings,
            "reconstructed bindings"
        );
        info!(
            composer_epoch = %self.epoch,
            revision = %revision,
            "reconciliation: {} deactivate, {} activate",
            plan.deactivate.len(),
            plan.activate.len()
        );
        self.reconcile().await;
    }

    fn compute_plan(&self) -> ReconcilePlan {
        let snapshots = self
            .records
            .values()
            .filter(|record| record.snapshot_received)
            .map(|record| &record.snapshot);
        self.reconciler
            .plan_with_registry(&self.registry, snapshots)
    }

    fn rebuild_registry(&mut self) {
        self.registry.clear();
        for (id, record) in &self.records {
            if !record.snapshot_received {
                continue;
            }
            let snapshot = &record.snapshot;
            if !matches!(snapshot.state, ComponentState::Active) {
                continue;
            }
            let Some(activation) = &snapshot.activation else {
                continue;
            };
            let capabilities = match snapshot.provided() {
                Ok(capabilities) => capabilities,
                Err(error) => {
                    warn!(component = %id, %error, "snapshot capabilities rejected during registry rebuild");
                    continue;
                }
            };
            if let Err(error) = self.registry.register_activation(
                id.clone(),
                snapshot.process_instance,
                activation.activation_id,
                capabilities,
            ) {
                warn!(component = %id, %error, "snapshot capability owner rejected during registry rebuild");
            }
        }
    }

    async fn reconcile(&mut self) {
        if !self.phase.allows_lifecycle_commands() {
            return;
        }
        // Cascading topology changes are handled one command at a time. A
        // pending marker prevents duplicate commands while awaiting a full
        // snapshot from the component.
        loop {
            let plan = self.compute_plan();
            if let Some(id) = plan.deactivate.first().cloned()
                && self
                    .records
                    .get(&id)
                    .is_some_and(|record| record.pending.is_none())
            {
                self.plan_deactivate(&id).await;
                continue;
            }
            if let Some(id) = plan.activate.first().cloned()
                && self
                    .records
                    .get(&id)
                    .is_some_and(|record| record.pending.is_none())
            {
                self.plan_activate(&id).await;
                continue;
            }
            break;
        }
    }

    async fn plan_deactivate(&mut self, id: &ComponentId) {
        let Some(record) = self.records.get(id) else {
            return;
        };
        if !matches!(record.snapshot.state, ComponentState::Active) {
            return;
        }
        let Some(activation) = record.snapshot.activation.as_ref() else {
            return;
        };
        let activation_id = activation.activation_id;
        let process_instance = record.snapshot.process_instance;
        let writer = Arc::clone(&record.writer);
        let reason = record
            .snapshot
            .requirements
            .iter()
            .find_map(|requirement| {
                let bound = requirement.binding.as_ref()?;
                match self.registry.resolve(&requirement.requirement) {
                    None => Some(format!(
                        "{} provider disappeared",
                        requirement.requirement.capability_type
                    )),
                    Some(current) if current != *bound => Some(format!(
                        "{} provider changed",
                        requirement.requirement.capability_type
                    )),
                    Some(_) => None,
                }
            })
            .unwrap_or_else(|| "capability topology changed".to_owned());
        // Withdraw this exact owner before sending the command, so dependent
        // consumers see the provider disappear in the same reconciliation
        // pass. The component remains locally Active until its next snapshot.
        self.registry
            .remove_activation(id, process_instance, activation_id);
        let revision = self.next_revision("deactivate command");
        if let Some(record) = self.records.get_mut(id) {
            record.pending = Some(PendingOperation::Deactivate {
                revision,
                activation: activation_id,
                process_instance,
                reason: reason.clone(),
            });
        }
        info!(
            component = %id,
            process_instance = %process_instance,
            activation = %activation_id,
            composer_epoch = %self.epoch,
            revision = %revision,
            "sending Deactivate"
        );
        let message = ServerMessage::Deactivate {
            revision,
            expected_process_instance: process_instance,
            expected_activation: activation_id,
            reason,
        };
        if let Err(error) = send_message(&writer, &message).await {
            warn!(component = %id, %error, "failed to send Deactivate; awaiting connection liveness event");
        }
    }

    async fn plan_activate(&mut self, id: &ComponentId) {
        let Some(record) = self.records.get(id) else {
            return;
        };
        if !matches!(
            record.snapshot.state,
            ComponentState::Preparing | ComponentState::Blocked(_)
        ) {
            return;
        }
        let requirements = record
            .snapshot
            .requirements
            .iter()
            .map(|requirement| &requirement.requirement)
            .cloned()
            .collect::<Vec<_>>();
        let Some(bindings) = requirements
            .iter()
            .map(|requirement| self.registry.resolve(requirement))
            .collect::<Option<Vec<_>>>()
        else {
            return;
        };
        let process_instance = record.snapshot.process_instance;
        let writer = Arc::clone(&record.writer);
        let revision = self.next_revision("activate command");
        if let Some(record) = self.records.get_mut(id) {
            record.pending = Some(PendingOperation::Activate {
                revision,
                bindings: bindings.clone(),
            });
        }
        info!(
            component = %id,
            process_instance = %process_instance,
            composer_epoch = %self.epoch,
            revision = %revision,
            "sending Activate"
        );
        let message = ServerMessage::Activate { revision, bindings };
        if let Err(error) = send_message(&writer, &message).await {
            warn!(component = %id, %error, "failed to send Activate; awaiting connection liveness event");
        }
    }

    fn next_revision(&mut self, reason: &'static str) -> Revision {
        self.revision = self.revision.advance();
        debug!(
            composer_epoch = %self.epoch,
            revision = %self.revision,
            reason,
            "composer revision advanced"
        );
        self.revision
    }

    async fn send_process_conflict(&self, id: &ComponentId, revision: Revision) {
        let Some(conflict) = self.conflicts.get(id) else {
            return;
        };
        let incumbent = ServerMessage::ProcessConflict {
            revision,
            component_id: id.clone(),
            incumbent: conflict.incumbent,
            contender: conflict.contender,
        };
        let contender = incumbent.clone();
        if let Err(error) = send_message(&conflict.incumbent_writer, &incumbent).await {
            warn!(component = %id, %error, "failed to send ProcessConflict to incumbent");
        }
        if let Err(error) = send_message(&conflict.contender_writer, &contender).await {
            warn!(component = %id, %error, "failed to send ProcessConflict to contender");
        }
    }
}

fn pending_message(pending: Option<PendingOperation>) -> Option<ServerMessage> {
    match pending? {
        PendingOperation::Activate { revision, bindings } => {
            Some(ServerMessage::Activate { revision, bindings })
        }
        PendingOperation::Deactivate {
            revision,
            activation,
            process_instance,
            reason,
        } => Some(ServerMessage::Deactivate {
            revision,
            expected_process_instance: process_instance,
            expected_activation: activation,
            reason,
        }),
    }
}

fn clear_pending(
    pending: Option<PendingOperation>,
    snapshot: &ComponentSnapshot,
) -> Option<PendingOperation> {
    match pending {
        Some(PendingOperation::Activate { .. })
            if !matches!(
                snapshot.state,
                ComponentState::Preparing | ComponentState::Blocked(_)
            ) =>
        {
            None
        }
        Some(PendingOperation::Deactivate { activation, .. })
            if snapshot
                .activation
                .as_ref()
                .map(|current| current.activation_id)
                != Some(activation)
                || !matches!(snapshot.state, ComponentState::Active) =>
        {
            None
        }
        other => other,
    }
}

fn same_component_truth(left: &ComponentSnapshot, right: &ComponentSnapshot) -> bool {
    left.snapshot_version == right.snapshot_version
        && left.component_id == right.component_id
        && left.process_instance == right.process_instance
        && left.state == right.state
        && left.activation == right.activation
        && left.requirements == right.requirements
        && left.provided_capabilities == right.provided_capabilities
}

fn active_context(snapshot: &ComponentSnapshot) -> Option<ActiveContext> {
    let activation = snapshot.activation.as_ref()?;
    let capabilities = snapshot
        .provided_capabilities
        .iter()
        .map(|capability| {
            CapabilityInspection::from_descriptor(
                capability.capability_type.clone(),
                &capability.descriptor,
            )
        })
        .collect();
    Some(ActiveContext {
        process_instance: snapshot.process_instance,
        activation_id: activation.activation_id,
        bindings: activation.bindings.clone(),
        capabilities,
    })
}

fn subscriber_observations(
    components: &[ComponentInspection],
    service_name: &str,
) -> Vec<SubscriberPressureObservation> {
    components
        .iter()
        .flat_map(|component| {
            let activation_id = component
                .snapshot
                .activation
                .as_ref()
                .map(|activation| activation.activation_id);
            component
                .snapshot
                .stream_pressures
                .iter()
                .filter(move |pressure| {
                    pressure.service_name == service_name && pressure.subscriber.is_some()
                })
                .map(move |pressure| {
                    let freshness = pressure_freshness(pressure);
                    SubscriberPressureObservation {
                        component_id: component.component_id.clone(),
                        process_instance_id: component.snapshot.process_instance,
                        activation_id,
                        pressure: pressure.clone(),
                        age_ms: freshness.age_ms,
                        fresh: freshness.fresh,
                        assessments: assess_stream_pressure(pressure),
                    }
                })
        })
        .take(MAX_SUBSCRIBER_PRESSURE_OBSERVATIONS)
        .collect()
}

/// Finds the server-side pressure observation for one active provider.
///
/// Provider metrics are intentionally joined to the complete physical
/// identity. A matching provider process and service name is required, and a
/// supplied capability instance must match. Bare custom RPC snapshots may
/// omit the instance and are accepted only as a fallback when no
/// instance-qualified snapshot exists.
fn rpc_server_pressure(
    components: &[ComponentInspection],
    descriptor: &rbt_core::CapabilityDescriptor,
) -> Option<RpcPressure> {
    let pressures = components
        .iter()
        .filter(|component| component.snapshot.process_instance == descriptor.provider_process)
        .flat_map(|component| component.snapshot.rpc_pressures.iter())
        .filter(|pressure| pressure.service_name == descriptor.service_name())
        .filter(|pressure| pressure.is_server())
        .collect::<Vec<_>>();

    pressures
        .iter()
        .find(|pressure| pressure.capability_instance == Some(descriptor.capability_instance))
        .or_else(|| {
            pressures
                .iter()
                .find(|pressure| pressure.capability_instance.is_none())
        })
        .map(|pressure| (*pressure).clone())
}

/// Collects current client-side observations for one active RPC capability.
///
/// A lifecycle-bound client observation must carry the exact capability
/// instance. This prevents a delayed snapshot from an old provider instance
/// from appearing on a replacement server's inspection record.
fn rpc_client_observations(
    components: &[ComponentInspection],
    descriptor: &rbt_core::CapabilityDescriptor,
) -> Vec<RpcClientPressureObservation> {
    components
        .iter()
        .flat_map(|component| {
            let snapshot_activation = component
                .snapshot
                .activation
                .as_ref()
                .map(|activation| activation.activation_id);
            component
                .snapshot
                .rpc_pressures
                .iter()
                .filter(move |pressure| {
                    pressure.service_name == descriptor.service_name()
                        && pressure.capability_instance == Some(descriptor.capability_instance)
                        && pressure.is_client()
                })
                .map(move |pressure| {
                    let freshness = rpc_pressure_freshness(pressure);
                    RpcClientPressureObservation {
                        component_id: component.component_id.clone(),
                        process_instance_id: component.snapshot.process_instance,
                        activation_id: pressure.client_activation.or(snapshot_activation),
                        pressure: pressure.clone(),
                        age_ms: freshness.age_ms,
                        fresh: freshness.fresh,
                    }
                })
        })
        .take(MAX_RPC_CLIENT_PRESSURE_OBSERVATIONS)
        .collect()
}

fn version_mismatch_reason(control_protocol_version: u16, snapshot_version: u16) -> String {
    match (
        control_protocol_version == CONTROL_PROTOCOL_VERSION,
        snapshot_version == COMPOSER_SNAPSHOT_VERSION,
    ) {
        (false, false) => format!(
            "control protocol version {control_protocol_version} and snapshot version {snapshot_version} are incompatible; expected control protocol version {CONTROL_PROTOCOL_VERSION} and snapshot version {COMPOSER_SNAPSHOT_VERSION}"
        ),
        (false, true) => format!(
            "control protocol version {control_protocol_version} is incompatible; expected {CONTROL_PROTOCOL_VERSION}"
        ),
        (true, false) => format!(
            "snapshot version {snapshot_version} is incompatible; expected {COMPOSER_SNAPSHOT_VERSION}"
        ),
        (true, true) => "protocol versions match".to_owned(),
    }
}

/// Runs a composer TCP server at `addr`.
pub async fn serve<A>(addr: A) -> std::result::Result<(), ControlPlaneError>
where
    A: ToSocketAddrs,
{
    let listener = TcpListener::bind(addr)
        .await
        .map_err(ControlPlaneError::Bind)?;
    let (events_tx, mut events_rx) = mpsc::unbounded_channel();
    let epoch = ComposerEpoch::new();
    info!(composer_epoch = %epoch, "composer serve incarnation started");
    let mut composer = Composer::new(epoch);
    let mut recovery_barrier = Box::pin(sleep(Duration::from_secs(2)));
    let mut next_connection_id = 1_u64;

    loop {
        tokio::select! {
            accepted = listener.accept() => {
                let (stream, peer) = accepted.map_err(ControlPlaneError::Accept)?;
                let connection_id = next_connection_id;
                next_connection_id = next_connection_id.saturating_add(1);
                debug!(connection_id, %peer, composer_epoch = %composer.epoch, "accepted component connection");
                let (read_half, write_half) = stream.into_split();
                let writer = Arc::new(Mutex::new(BufWriter::new(write_half)));
                let tx = events_tx.clone();
                tokio::spawn(read_connection(connection_id, read_half, writer, tx));
            }
            event = events_rx.recv() => {
                let Some(event) = event else { return Ok(()); };
                composer.process(event).await;
            }
            _ = &mut recovery_barrier, if composer.phase == ComposerPhase::Recovering => {
                composer.finish_recovery().await;
            }
        }
    }
}

async fn read_connection(
    connection_id: u64,
    read_half: OwnedReadHalf,
    writer: SharedWriter,
    events_tx: mpsc::UnboundedSender<Event>,
) {
    let mut reader = BufReader::new(read_half);
    let mut line = String::new();
    loop {
        line.clear();
        let read = match reader.read_line(&mut line).await {
            Ok(read) => read,
            Err(error) => {
                warn!(connection_id, %error, "failed to read component message");
                break;
            }
        };
        if read == 0 {
            break;
        }
        let message = match serde_json::from_str::<ClientMessage>(line.trim_end()) {
            Ok(message) => message,
            Err(error) => {
                warn!(connection_id, %error, "invalid component NDJSON message");
                break;
            }
        };
        if events_tx
            .send(Event::Message {
                connection_id,
                writer: Arc::clone(&writer),
                message: Box::new(message),
            })
            .is_err()
        {
            return;
        }
    }
    let _ = events_tx.send(Event::Disconnected { connection_id });
}

async fn send_message(writer: &SharedWriter, message: &ServerMessage) -> std::io::Result<()> {
    let encoded = serde_json::to_vec(message).map_err(std::io::Error::other)?;
    let mut writer = writer.lock().await;
    writer.write_all(&encoded).await?;
    writer.write_all(b"\n").await?;
    writer.flush().await
}

#[allow(dead_code)]
fn _snapshot_error_is_send_safe(error: SnapshotValidationError) -> String {
    error.to_string()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::manifest::{ActivationSnapshot, CapabilitySnapshot, RequirementSnapshot};
    use rbt_core::{
        CapabilityDescriptor, CapabilityInstanceId, RequirementSpec, ResolvedBinding,
        RpcClientMetrics, RpcDescriptor, RpcPressure, RpcServerMetrics, StreamDescriptor,
    };
    use rbt_types::{Frame, StreamProfile};

    #[tokio::test]
    async fn partial_recovery_never_emits_lifecycle_commands() {
        // Snapshot arrival order is intentionally irrelevant during the
        // recovery barrier: even a temporarily incomplete registry cannot
        // authorize Activate or Deactivate until the phase becomes Running.
        let epoch = ComposerEpoch::new();
        let mut composer = Composer::new(epoch);
        let detector_process = ProcessInstanceId::new();
        let camera_process = ProcessInstanceId::new();
        let camera_activation = ActivationId::first();
        let camera_capability = CapabilityInstanceId::new();
        let camera_descriptor = CapabilityDescriptor::new(
            camera_capability,
            ComponentId::from("camera-a"),
            camera_process,
            camera_activation,
            StreamDescriptor::for_fixed::<Frame>(
                camera_capability.stream_service_name(),
                StreamProfile::Sensor,
            ),
        );
        let camera_binding = ResolvedBinding::new(
            "camera",
            "Camera",
            ComponentId::from("camera-a"),
            camera_descriptor,
        );
        let detector = ComponentSnapshot::new(
            "detector",
            detector_process,
            ComponentState::Active,
            Some(ActivationSnapshot::new(
                ActivationId::first(),
                [camera_binding.clone()],
            )),
            [RequirementSnapshot::new(
                RequirementSpec::new("camera", "Camera"),
                Some(camera_binding),
            )],
            [],
        );
        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let client = tokio::net::TcpStream::connect(listener.local_addr().unwrap())
            .await
            .unwrap();
        let (_server, _) = listener.accept().await.unwrap();
        let (_read, write) = client.into_split();
        composer.records.insert(
            ComponentId::from("detector"),
            ComponentRecord {
                last_active: active_context(&detector),
                snapshot: detector,
                snapshot_received: true,
                connection_id: 1,
                writer: Arc::new(Mutex::new(BufWriter::new(write))),
                pending: None,
            },
        );

        assert_eq!(
            composer.compute_plan().deactivate,
            vec![ComponentId::from("detector")]
        );
        composer.reconcile().await;
        assert!(
            composer
                .records
                .get(&ComponentId::from("detector"))
                .unwrap()
                .pending
                .is_none()
        );
        assert_eq!(composer.phase(), ComposerPhase::Recovering);
    }

    #[test]
    fn each_composer_start_has_a_fresh_epoch() {
        assert_ne!(
            Composer::new(ComposerEpoch::new()).epoch(),
            Composer::new(ComposerEpoch::new()).epoch()
        );
    }

    #[test]
    fn rpc_capability_inspection_has_only_rpc_metadata() {
        let instance = CapabilityInstanceId::new();
        let descriptor = CapabilityDescriptor::new_rpc(
            instance,
            ComponentId::from("rpc-provider"),
            ProcessInstanceId::new(),
            ActivationId::first(),
            RpcDescriptor::for_fixed::<Frame, Frame>(instance.rpc_service_name()),
        );
        let inspection = CapabilityInspection::from_descriptor("FrameRpc", &descriptor);
        let value = serde_json::to_value(&inspection).unwrap();

        assert_eq!(value["transport_kind"], "rpc");
        assert_eq!(value["physical_endpoint"], instance.rpc_service_name());
        assert!(value.get("request_type").is_some());
        assert!(value.get("response_type").is_some());
        assert!(value.get("readiness").is_some());
        assert!(value.get("resources").is_some());
        assert!(value.get("profile").is_none());
        assert!(value.get("queue_depth").is_none());
        assert!(value.get("pressure").is_none());
        assert_eq!(
            inspection.transport.transport_kind(),
            CapabilityTransportKind::Rpc
        );
        assert_eq!(
            serde_json::from_value::<CapabilityInspection>(value).unwrap(),
            inspection
        );
    }

    #[test]
    fn pressure_changes_are_not_composition_truth_changes() {
        let process = ProcessInstanceId::new();
        let mut first = ComponentSnapshot::new(
            "camera",
            process,
            ComponentState::Preparing,
            None,
            std::iter::empty::<RequirementSpec>(),
            [],
        );
        let mut second = first.clone();
        second.stream_pressures.push(StreamPressure {
            service_name: "rbt/cap/example/stream".to_owned(),
            profile: StreamProfile::Sensor,
            semantic: rbt_core::StreamSemanticConfig {
                queue_depth: 1,
                history_size: 0,
                safe_overflow: true,
            },
            resources: rbt_core::StreamResourceConfig::new(2, 1),
            publisher: Some(rbt_core::PublisherPressure {
                send_attempts: 10,
                ..Default::default()
            }),
            subscriber: None,
            last_pressure_event: None,
            snapshot_timestamp_unix_ms: 0,
        });
        assert!(same_component_truth(&first, &second));

        first.state = ComponentState::Activating;
        assert!(!same_component_truth(&first, &second));
    }

    #[test]
    fn pressure_assessment_retains_recovered_loan_exhaustion() {
        let pressure = StreamPressure {
            service_name: "rbt/cap/example/stream".to_owned(),
            profile: StreamProfile::Sensor,
            semantic: rbt_core::StreamSemanticConfig::for_profile(StreamProfile::Sensor),
            resources: rbt_core::StreamResourceConfig::new(2, 1),
            publisher: Some(rbt_core::PublisherPressure {
                loans_in_use: 0,
                loans_high_water: 2,
                loan_failures: 1,
                payload_bytes_max_seen: 128,
                ..Default::default()
            }),
            subscriber: None,
            last_pressure_event: None,
            snapshot_timestamp_unix_ms: 1,
        };

        let assessments = assess_stream_pressure(&pressure);
        assert_eq!(assessments.len(), 1);
        assert_eq!(
            assessments[0].reason,
            PressureAssessmentReason::LoanCapacityReached
        );
        assert_eq!(assessments[0].approximate_payload_only_bytes, Some(128));
    }

    #[test]
    fn pressure_assessment_distinguishes_recovered_and_current_borrows() {
        let mut pressure = StreamPressure {
            service_name: "rbt/cap/example/stream".to_owned(),
            profile: StreamProfile::Sensor,
            semantic: rbt_core::StreamSemanticConfig::for_profile(StreamProfile::Sensor),
            resources: rbt_core::StreamResourceConfig::new(2, 1),
            publisher: None,
            subscriber: Some(rbt_core::SubscriberPressure {
                borrowed_samples: 0,
                borrowed_high_water: 1,
                borrow_failures: 1,
                ..Default::default()
            }),
            last_pressure_event: None,
            snapshot_timestamp_unix_ms: 1,
        };

        let recovered = assess_stream_pressure(&pressure);
        assert_eq!(
            recovered
                .iter()
                .map(|assessment| assessment.reason)
                .collect::<Vec<_>>(),
            vec![PressureAssessmentReason::BorrowCapacityReached]
        );

        pressure.subscriber.as_mut().unwrap().borrowed_samples = 1;
        let current = assess_stream_pressure(&pressure);
        assert!(current.iter().any(|assessment| {
            assessment.reason == PressureAssessmentReason::BorrowCapacityReached
        }));
        assert!(
            current
                .iter()
                .any(|assessment| assessment.reason == PressureAssessmentReason::OutstandingBorrow)
        );
    }

    fn pressure_with_timestamp(snapshot_timestamp_unix_ms: u64) -> StreamPressure {
        StreamPressure {
            service_name: "rbt/cap/example/stream".to_owned(),
            profile: StreamProfile::Sensor,
            semantic: rbt_core::StreamSemanticConfig::for_profile(StreamProfile::Sensor),
            resources: rbt_core::StreamResourceConfig::default(),
            publisher: Some(rbt_core::PublisherPressure::default()),
            subscriber: None,
            last_pressure_event: None,
            snapshot_timestamp_unix_ms,
        }
    }

    #[test]
    fn pressure_freshness_reports_exact_stale_age() {
        let snapshot_timestamp_unix_ms = 10_000;
        let now_ms = snapshot_timestamp_unix_ms + PRESSURE_FRESHNESS_WINDOW_MS + 17;
        let freshness =
            pressure_freshness_at(&pressure_with_timestamp(snapshot_timestamp_unix_ms), now_ms);

        assert_eq!(
            freshness.snapshot_timestamp_unix_ms,
            snapshot_timestamp_unix_ms
        );
        assert_eq!(freshness.age_ms, PRESSURE_FRESHNESS_WINDOW_MS + 17);
        assert!(!freshness.fresh);
    }

    #[test]
    fn pressure_freshness_saturates_future_timestamp_to_zero() {
        let freshness = pressure_freshness_at(&pressure_with_timestamp(10_000), 9_000);

        assert_eq!(freshness.age_ms, 0);
        assert!(freshness.fresh);
    }

    #[test]
    fn subscriber_observations_are_exact_service_bounded_and_identity_scoped() {
        let service_name = "rbt/cap/example/stream";
        let mut components = Vec::new();
        for index in 0..(MAX_SUBSCRIBER_PRESSURE_OBSERVATIONS + 1) {
            let component_id = ComponentId::new(format!("consumer-{index}"));
            let process_instance = ProcessInstanceId::from_uuid(uuid::Uuid::from_u128(
                10_000 + u128::try_from(index).unwrap(),
            ));
            let activation_id = ActivationId::new(u64::try_from(index + 1).unwrap());
            let snapshot = ComponentSnapshot::new(
                component_id.clone(),
                process_instance,
                ComponentState::Active,
                Some(ActivationSnapshot::new(activation_id, [])),
                std::iter::empty::<RequirementSpec>(),
                std::iter::empty::<CapabilitySnapshot>(),
            )
            .with_stream_pressures([StreamPressure {
                service_name: service_name.to_owned(),
                profile: StreamProfile::Sensor,
                semantic: rbt_core::StreamSemanticConfig::for_profile(StreamProfile::Sensor),
                resources: rbt_core::StreamResourceConfig::default(),
                publisher: None,
                subscriber: Some(rbt_core::SubscriberPressure::default()),
                last_pressure_event: None,
                snapshot_timestamp_unix_ms: 1,
            }]);
            components.push(ComponentInspection {
                component_id,
                snapshot,
                snapshot_received: true,
                control_connection: ControlConnectionStatus::Connected,
                pending: None,
                last_active: None,
            });
        }
        components.push(ComponentInspection {
            component_id: ComponentId::from("wrong-service"),
            snapshot: ComponentSnapshot::new(
                "wrong-service",
                ProcessInstanceId::new(),
                ComponentState::Active,
                Some(ActivationSnapshot::new(ActivationId::first(), [])),
                std::iter::empty::<RequirementSpec>(),
                std::iter::empty::<CapabilitySnapshot>(),
            )
            .with_stream_pressures([StreamPressure {
                service_name: "rbt/cap/other/stream".to_owned(),
                profile: StreamProfile::Sensor,
                semantic: rbt_core::StreamSemanticConfig::for_profile(StreamProfile::Sensor),
                resources: rbt_core::StreamResourceConfig::default(),
                publisher: None,
                subscriber: Some(rbt_core::SubscriberPressure::default()),
                last_pressure_event: None,
                snapshot_timestamp_unix_ms: 1,
            }]),
            snapshot_received: true,
            control_connection: ControlConnectionStatus::Connected,
            pending: None,
            last_active: None,
        });

        let observations = subscriber_observations(&components, service_name);
        assert_eq!(observations.len(), MAX_SUBSCRIBER_PRESSURE_OBSERVATIONS);
        assert!(
            observations
                .iter()
                .all(|observation| observation.pressure.service_name == service_name)
        );
        assert_eq!(
            observations[0].component_id,
            ComponentId::from("consumer-0")
        );
        assert_eq!(
            observations[0].process_instance_id,
            ProcessInstanceId::from_uuid(uuid::Uuid::from_u128(10_000))
        );
        assert_eq!(observations[0].activation_id, Some(ActivationId::new(1)));
    }

    #[tokio::test]
    async fn first_snapshot_matching_placeholder_still_triggers_reconciliation() {
        let mut composer = Composer::new(ComposerEpoch::new());
        composer.phase = ComposerPhase::Running;
        let component_id = ComponentId::from("camera");
        let process_instance = ProcessInstanceId::new();
        let connection_id = 1;

        let listener = TcpListener::bind("127.0.0.1:0").await.unwrap();
        let client = tokio::net::TcpStream::connect(listener.local_addr().unwrap())
            .await
            .unwrap();
        let (_server, _) = listener.accept().await.unwrap();
        let (_read, write) = client.into_split();
        composer.connections.insert(
            connection_id,
            ConnectionInfo {
                component_id: component_id.clone(),
                process_instance,
            },
        );
        composer.records.insert(
            component_id.clone(),
            ComponentRecord {
                snapshot: ComponentSnapshot::placeholder(component_id.clone(), process_instance),
                last_active: None,
                snapshot_received: false,
                connection_id,
                writer: Arc::new(Mutex::new(BufWriter::new(write))),
                pending: None,
            },
        );

        let first = ComponentSnapshot::new(
            component_id.clone(),
            process_instance,
            ComponentState::Preparing,
            None,
            std::iter::empty::<RequirementSpec>(),
            [],
        );
        composer.snapshot(connection_id, first).await;

        let record = composer.records.get(&component_id).unwrap();
        assert!(record.snapshot_received);
        assert!(matches!(
            record.pending,
            Some(PendingOperation::Activate { .. })
        ));
    }

    fn rpc_descriptor_for_test(
        capability_instance: CapabilityInstanceId,
        provider_process: ProcessInstanceId,
    ) -> CapabilityDescriptor {
        CapabilityDescriptor::new_rpc(
            capability_instance,
            ComponentId::from("rpc-provider"),
            provider_process,
            ActivationId::first(),
            RpcDescriptor::for_fixed::<Frame, Frame>(capability_instance.rpc_service_name()),
        )
    }

    fn component_with_rpc_pressure(
        component_id: impl Into<ComponentId>,
        process_instance: ProcessInstanceId,
        activation_id: ActivationId,
        pressure: RpcPressure,
    ) -> ComponentInspection {
        let component_id = component_id.into();
        ComponentInspection {
            component_id: component_id.clone(),
            snapshot: ComponentSnapshot::new(
                component_id,
                process_instance,
                ComponentState::Active,
                Some(ActivationSnapshot::new(activation_id, [])),
                std::iter::empty::<RequirementSpec>(),
                std::iter::empty::<CapabilitySnapshot>(),
            )
            .with_rpc_pressures([pressure]),
            snapshot_received: true,
            control_connection: ControlConnectionStatus::Connected,
            pending: None,
            last_active: None,
        }
    }

    #[test]
    fn rpc_provider_pressure_attaches_and_distinguishes_unavailable_from_zero() {
        let provider_process = ProcessInstanceId::new();
        let capability_instance = CapabilityInstanceId::new();
        let descriptor = rpc_descriptor_for_test(capability_instance, provider_process);
        let components = vec![ComponentInspection {
            component_id: ComponentId::from("rpc-provider"),
            snapshot: ComponentSnapshot::new(
                "rpc-provider",
                provider_process,
                ComponentState::Active,
                Some(ActivationSnapshot::new(ActivationId::first(), [])),
                std::iter::empty::<RequirementSpec>(),
                std::iter::empty::<CapabilitySnapshot>(),
            ),
            snapshot_received: true,
            control_connection: ControlConnectionStatus::Connected,
            pending: None,
            last_active: None,
        }];
        let mut inspection = CapabilityInspection::from_descriptor("Rpc", &descriptor);
        if let CapabilityTransportInspection::Rpc {
            server_pressure, ..
        } = &inspection.transport
        {
            assert!(server_pressure.is_none());
        } else {
            panic!("expected RPC inspection");
        }

        let pressure = RpcPressure {
            service_name: descriptor.service_name().to_owned(),
            capability_instance: Some(capability_instance),
            server: Some(RpcServerMetrics::default()),
            snapshot_timestamp_unix_ms: now_unix_ms(),
            ..Default::default()
        };
        let mut components = components;
        components[0].snapshot = components[0]
            .snapshot
            .clone()
            .with_rpc_pressures([pressure.clone()]);
        inspection.attach_rpc_server_pressure(rpc_server_pressure(&components, &descriptor));

        let CapabilityTransportInspection::Rpc {
            server_pressure,
            server_pressure_freshness,
            ..
        } = inspection.transport
        else {
            panic!("expected RPC inspection");
        };
        assert_eq!(server_pressure, Some(pressure));
        assert!(server_pressure_freshness.is_some_and(|freshness| freshness.fresh));
    }

    #[test]
    fn rpc_provider_pressure_is_stale_with_factual_age() {
        let provider_process = ProcessInstanceId::new();
        let capability_instance = CapabilityInstanceId::new();
        let descriptor = rpc_descriptor_for_test(capability_instance, provider_process);
        let pressure = RpcPressure {
            service_name: descriptor.service_name().to_owned(),
            capability_instance: Some(capability_instance),
            server: Some(RpcServerMetrics::default()),
            snapshot_timestamp_unix_ms: 1,
            ..Default::default()
        };
        let components = vec![component_with_rpc_pressure(
            "rpc-provider",
            provider_process,
            ActivationId::first(),
            pressure,
        )];
        let freshness = rpc_pressure_freshness(
            rpc_server_pressure(&components, &descriptor)
                .as_ref()
                .expect("server pressure should match"),
        );
        assert!(!freshness.fresh);
        assert!(freshness.age_ms > PRESSURE_FRESHNESS_WINDOW_MS);
    }

    #[test]
    fn rpc_client_observations_match_instance_and_are_bounded() {
        let provider_process = ProcessInstanceId::new();
        let capability_instance = CapabilityInstanceId::new();
        let descriptor = rpc_descriptor_for_test(capability_instance, provider_process);
        let components = (0..(MAX_RPC_CLIENT_PRESSURE_OBSERVATIONS + 1))
            .map(|index| {
                let pressure = RpcPressure {
                    service_name: descriptor.service_name().to_owned(),
                    capability_instance: Some(capability_instance),
                    client_activation: Some(ActivationId::new(index as u64 + 1)),
                    client: Some(RpcClientMetrics {
                        calls_attempted: index as u64,
                        ..Default::default()
                    }),
                    snapshot_timestamp_unix_ms: now_unix_ms(),
                    ..Default::default()
                };
                component_with_rpc_pressure(
                    format!("client-{index}"),
                    ProcessInstanceId::new(),
                    ActivationId::new(index as u64 + 1),
                    pressure,
                )
            })
            .collect::<Vec<_>>();
        let observations = rpc_client_observations(&components, &descriptor);

        assert_eq!(observations.len(), MAX_RPC_CLIENT_PRESSURE_OBSERVATIONS);
        assert_eq!(observations[0].pressure.client.unwrap().calls_attempted, 0);
        assert!(observations.iter().all(|observation| observation.fresh));
    }

    #[test]
    fn rpc_client_observations_do_not_attach_old_capability_instances() {
        let provider_process = ProcessInstanceId::new();
        let old_instance = CapabilityInstanceId::new();
        let current_instance = CapabilityInstanceId::new();
        let current_descriptor = rpc_descriptor_for_test(current_instance, provider_process);
        let old_pressure = RpcPressure {
            service_name: current_descriptor.service_name().to_owned(),
            capability_instance: Some(old_instance),
            client: Some(RpcClientMetrics {
                calls_attempted: 99,
                ..Default::default()
            }),
            ..Default::default()
        };
        let components = vec![component_with_rpc_pressure(
            "old-client",
            ProcessInstanceId::new(),
            ActivationId::first(),
            old_pressure,
        )];

        assert!(rpc_client_observations(&components, &current_descriptor).is_empty());
    }

    #[test]
    fn rpc_pressure_only_changes_are_not_composition_truth_changes() {
        let process = ProcessInstanceId::new();
        let first = ComponentSnapshot::new(
            "rpc-provider",
            process,
            ComponentState::Preparing,
            None,
            std::iter::empty::<RequirementSpec>(),
            [],
        );
        let second = first.clone().with_rpc_pressures([RpcPressure {
            service_name: "rpc-provider-service".to_owned(),
            server: Some(RpcServerMetrics {
                requests_received: 7,
                ..Default::default()
            }),
            ..Default::default()
        }]);

        assert!(same_component_truth(&first, &second));
    }
}

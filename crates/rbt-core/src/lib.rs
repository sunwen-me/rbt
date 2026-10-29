//! Shared types for declaring components and resolving their capability bindings.
//!
//! The types in this crate are deliberately independent of the transport.  In
//! particular, the descriptors used by the control plane own their strings so
//! that they can be serialized and reconstructed in another process.

use std::{
    fmt,
    marker::PhantomData,
    sync::{Arc, Mutex, OnceLock, Weak},
};

use serde::{Deserialize, Deserializer, Serialize, Serializer, de::Error as _};
use thiserror::Error;
use uuid::Uuid;

pub use rbt_types::{
    LayoutHash, MessageShape, MessageTypeDescriptor, ReadinessProtocol, RpcDescriptor,
    RpcReadinessProtocol, RpcResourceConfig, RpcTransportFeatures, StreamDescriptor, StreamProfile,
    StreamResourceConfig, TransportFeatures,
};

/// Frozen semantic settings associated with one stream endpoint observation.
///
/// These values are derived from [`StreamProfile`] by the transport. They are
/// intentionally not caller-configurable resource limits.
#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, Hash, PartialEq, Serialize)]
pub struct StreamSemanticConfig {
    /// Maximum unread samples in one subscriber queue.
    pub queue_depth: usize,
    /// Samples retained for subscribers which connect after publication.
    pub history_size: usize,
    /// Whether a full Subscriber Queue replaces its oldest unread sample.
    pub safe_overflow: bool,
}

impl StreamSemanticConfig {
    /// Returns the frozen semantic settings for a profile.
    pub const fn for_profile(profile: StreamProfile) -> Self {
        Self {
            queue_depth: match profile {
                StreamProfile::Sensor => 1,
                StreamProfile::Control => 4,
            },
            history_size: 0,
            safe_overflow: matches!(profile, StreamProfile::Sensor),
        }
    }
}

/// Backwards-compatible name for the old pressure configuration type.
#[deprecated(note = "use StreamSemanticConfig and StreamPressure.resources")]
pub type StreamPressureConfig = StreamSemanticConfig;

/// A typed pressure transition observed by an rbt stream wrapper.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum PressureEvent {
    /// A Subscriber Queue could not accept another Control sample.
    QueueFull {
        /// Transport subscriber port identity when iceoryx2 supplied it.
        subscriber: Option<String>,
    },
    /// A send did not reach every connected subscriber.
    UnableToDeliver,
    /// The configured maximum number of concurrent Loans was reached.
    LoanPoolExhausted,
    /// A receive would exceed the subscriber's configured Borrow limit.
    BorrowLimitReached,
}

/// Publisher-side counters observed by the transport wrapper.
#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
pub struct PublisherPressure {
    /// Calls that attempted to obtain a publisher Loan.
    pub loan_attempts: u64,
    /// Loans successfully obtained from the transport.
    pub loan_successes: u64,
    /// Loans currently held by application code and not yet sent or dropped.
    pub loans_in_use: u64,
    /// Maximum simultaneous Loans observed.
    pub loans_high_water: u64,
    /// Loan attempts rejected because the configured Loan limit was reached.
    pub loan_failures: u64,
    /// Bytes in the most recently successful Loan.
    pub payload_bytes_last: u64,
    /// Largest payload size observed in a successful Loan.
    pub payload_bytes_max_seen: u64,
    /// Cumulative bytes represented by successful Loans.
    pub total_loaned: u64,
    /// Calls that attempted to send a previously loaned sample.
    pub send_attempts: u64,
    /// Send calls that completed without a transport error.
    pub send_successes: u64,
    /// Send calls that returned a transport error.
    pub send_failures: u64,
    /// Send failures caused by at least one full subscriber queue.
    pub queue_full: u64,
    /// Send failures not classified as a full queue.
    pub unable_to_deliver: u64,
    /// Advisory readiness notifications that failed.
    pub readiness_notify_failures: u64,
    /// Last aggregate subscriber count observed from iceoryx2 dynamic config.
    pub subscriber_count: u32,
    /// Estimated Drops when a trustworthy estimator exists.
    ///
    /// The generic publisher wrapper cannot see safe-overflow counts, so this
    /// is normally `None` rather than a fabricated zero.
    pub estimated_dropped_samples: Option<u64>,
}

/// Subscriber-side counters self-reported by one local stream wrapper.
#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
pub struct SubscriberPressure {
    /// Calls that attempted to receive a sample.
    pub receive_attempts: u64,
    /// Samples successfully received by this wrapper.
    pub received_samples: u64,
    /// Samples currently retained as active Borrows by application code.
    pub borrowed_samples: u64,
    /// Maximum simultaneous Borrows observed.
    pub borrowed_high_water: u64,
    /// Receive attempts rejected at the configured Borrow limit.
    pub borrow_failures: u64,
    /// Number of completed Borrow lifetimes with a measured duration.
    pub borrow_duration_count: u64,
    /// Cumulative duration of completed Borrows in nanoseconds.
    pub borrow_duration_total_ns: u64,
    /// Longest completed Borrow duration in nanoseconds.
    pub borrow_duration_max_ns: u64,
    /// Age in nanoseconds of the oldest currently active Borrow, when any.
    pub oldest_borrow_age_ns: Option<u64>,
    /// Estimated Drops when a message-specific sequence analyser exists.
    ///
    /// The generic subscriber wrapper does not understand payload sequences,
    /// so this is normally `None`.
    pub estimated_dropped_samples: Option<u64>,
}

/// A point-in-time pressure snapshot for one local publisher or subscriber.
///
/// Exactly one of [`Self::publisher`] and [`Self::subscriber`] is populated.
/// The service name is the physical stream identity and therefore rotates with
/// the owning CapabilityInstanceId.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct StreamPressure {
    /// Physical iceoryx2 service name.
    pub service_name: String,
    /// Sensor or Control profile.
    pub profile: StreamProfile,
    /// Frozen queue/history/overflow semantics derived from the profile.
    #[serde(default, alias = "config")]
    pub semantic: StreamSemanticConfig,
    /// Shared-memory resource ceilings used by this endpoint.
    #[serde(default)]
    pub resources: StreamResourceConfig,
    /// Transport-observed publisher counters.
    pub publisher: Option<PublisherPressure>,
    /// Subscriber self-reported counters.
    pub subscriber: Option<SubscriberPressure>,
    /// Most recent typed pressure transition, if any.
    pub last_pressure_event: Option<PressureEvent>,
    /// Wall-clock snapshot timestamp in Unix milliseconds.
    #[serde(default)]
    pub snapshot_timestamp_unix_ms: u64,
}

/// Internal source used to take a low-rate snapshot of a live local endpoint.
///
/// This trait is public only so transport crates can register an implementation
/// without introducing a dependency from the control plane back to a concrete
/// transport. Application code should use its transport's `pressure()` API.
#[doc(hidden)]
pub trait PressureSnapshotSource: Send + Sync {
    /// Returns one point-in-time endpoint snapshot.
    fn pressure_snapshot(&self) -> StreamPressure;
}

fn pressure_sources() -> &'static Mutex<Vec<Weak<dyn PressureSnapshotSource>>> {
    static SOURCES: OnceLock<Mutex<Vec<Weak<dyn PressureSnapshotSource>>>> = OnceLock::new();
    SOURCES.get_or_init(|| Mutex::new(Vec::new()))
}

/// Registers a live endpoint as a process-local pressure source.
#[doc(hidden)]
pub fn register_pressure_source<T>(source: &Arc<T>)
where
    T: PressureSnapshotSource + 'static,
{
    let erased: Arc<dyn PressureSnapshotSource> = source.clone();
    pressure_sources()
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
        .push(Arc::downgrade(&erased));
}

/// Returns pressure snapshots for every live endpoint wrapper in this process.
///
/// The registry contains weak references and therefore cannot extend an
/// endpoint's lifetime. Dead registrations are removed while taking a
/// snapshot; no payload path serializes or publishes these values.
pub fn local_pressure_snapshots() -> Vec<StreamPressure> {
    let mut sources = pressure_sources()
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let mut snapshots = Vec::with_capacity(sources.len());
    sources.retain(|source| {
        let Some(source) = source.upgrade() else {
            return false;
        };
        snapshots.push(source.pressure_snapshot());
        true
    });
    snapshots
}

/// Aggregate client-side counters for one local RPC endpoint.
///
/// These are lifetime counters for the owning client activation.  They are
/// intentionally aggregate-only: the control plane never receives per-call
/// events or a history of individual requests.  A transport wrapper may reset
/// them when the activation is replaced, in which case the new
/// [`RpcPressure::client_activation`] identifies the new lifetime.
#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
pub struct RpcClientMetrics {
    /// Calls accepted by the client API, including calls later rejected for
    /// capacity or server unavailability.
    pub calls_attempted: u64,
    /// Calls that obtained a transport request and entered in-flight state.
    pub calls_started: u64,
    /// Calls completed with a response.
    pub calls_completed: u64,
    /// Calls ending in a framework failure other than timeout, explicit
    /// cancellation, or activation expiry.
    pub calls_failed: u64,
    /// Calls whose monotonic deadline expired before a response won the race.
    pub calls_timed_out: u64,
    /// Calls explicitly cancelled by the caller.
    pub calls_cancelled: u64,
    /// Calls terminated because the owning client activation expired.
    pub calls_activation_expired: u64,
    /// Calls currently waiting for a response.
    pub in_flight_current: u64,
    /// Maximum simultaneous in-flight calls observed during this activation.
    pub in_flight_high_water: u64,
    /// Native response guards currently retained by callers or derived views.
    #[serde(default)]
    pub response_guards_current: u64,
    /// Maximum simultaneous native response guards observed during this activation.
    #[serde(default)]
    pub response_guards_high_water: u64,
    /// Calls rejected before transport start because the local in-flight
    /// capacity was exhausted.
    pub capacity_rejections: u64,
    /// Calls rejected because the bound server reported no available capacity.
    pub server_capacity_rejections: u64,
    /// Calls rejected because the bound server was unavailable.
    pub server_unavailable: u64,
    /// Failures obtaining a transport request loan.
    pub request_loan_failures: u64,
    /// Failures receiving or decoding a transport response.
    pub response_receive_failures: u64,
    /// Number of completed calls with a measured duration.
    pub call_duration_count: u64,
    /// Cumulative measured call duration in nanoseconds.
    pub call_duration_total_ns: u64,
    /// Longest measured completed call duration in nanoseconds.
    pub call_duration_max_ns: u64,
}

/// Aggregate server-side counters for one local RPC endpoint.
///
/// Counters cover the lifetime of the owning server activation.  They do not
/// retain request payloads, request identifiers, or per-call history.
#[derive(Clone, Copy, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
pub struct RpcServerMetrics {
    /// Requests successfully received by the server wrapper.
    pub requests_received: u64,
    /// Requests currently held by application code.
    pub requests_current: u64,
    /// Maximum simultaneous requests observed.
    pub requests_high_water: u64,
    /// Attempts to loan a response for a received request.
    pub responses_attempted: u64,
    /// Responses successfully sent.
    pub responses_sent: u64,
    /// Response attempts that returned a transport/framework error.
    pub responses_failed: u64,
    /// Requests abandoned before a response could be sent, including client
    /// disconnect or request-drop paths when the transport can identify them.
    pub responses_abandoned: u64,
    /// Failures borrowing a received request.
    pub request_borrow_failures: u64,
    /// Failures obtaining a transport response loan.
    pub response_loan_failures: u64,
}

/// A low-frequency aggregate pressure snapshot for one RPC endpoint.
///
/// `client` and `server` are mutually exclusive by convention: a client
/// wrapper fills `client` and a server wrapper fills `server`.  The type keeps
/// both optional fields so one transport-neutral snapshot can represent either
/// side without inventing a transport enum or changing stream metrics.  A
/// lifecycle-bound endpoint must carry its [`CapabilityInstanceId`]; a bare
/// custom RPC may leave it absent.
#[derive(Clone, Debug, Default, Deserialize, Eq, PartialEq, Serialize)]
pub struct RpcPressure {
    /// Physical local transport service name.
    pub service_name: String,
    /// Capability publication identity, when this endpoint belongs to a
    /// lifecycle-bound capability.
    #[serde(default)]
    pub capability_instance: Option<CapabilityInstanceId>,
    /// Bounded shared-memory resource ceilings used by this endpoint.
    #[serde(default)]
    pub resources: RpcResourceConfig,
    /// Client activation that owns the client-side counters, when known.
    #[serde(default)]
    pub client_activation: Option<ActivationId>,
    /// Client-side aggregate counters.  Populate exactly one of `client` and
    /// `server` by convention.
    #[serde(default)]
    pub client: Option<RpcClientMetrics>,
    /// Server-side aggregate counters.  Populate exactly one of `client` and
    /// `server` by convention.
    #[serde(default)]
    pub server: Option<RpcServerMetrics>,
    /// Wall-clock snapshot timestamp in Unix milliseconds.
    #[serde(default)]
    pub snapshot_timestamp_unix_ms: u64,
}

impl RpcPressure {
    /// Returns whether this snapshot follows the client-side convention.
    pub const fn is_client(&self) -> bool {
        self.client.is_some() && self.server.is_none()
    }

    /// Returns whether this snapshot follows the server-side convention.
    pub const fn is_server(&self) -> bool {
        self.server.is_some() && self.client.is_none()
    }

    /// Returns whether exactly one side of the snapshot is populated.
    pub const fn has_exactly_one_side(&self) -> bool {
        self.client.is_some() != self.server.is_some()
    }
}

/// Internal source used to take a low-rate snapshot of a live local RPC
/// endpoint.  The trait is public only so transport crates can register an
/// implementation without coupling the control plane to a concrete
/// transport.  Application code should use its transport's metrics API.
#[doc(hidden)]
pub trait RpcPressureSnapshotSource: Send + Sync {
    /// Returns one point-in-time RPC endpoint snapshot.
    fn pressure_snapshot(&self) -> RpcPressure;
}

fn rpc_pressure_sources() -> &'static Mutex<Vec<Weak<dyn RpcPressureSnapshotSource>>> {
    static SOURCES: OnceLock<Mutex<Vec<Weak<dyn RpcPressureSnapshotSource>>>> = OnceLock::new();
    SOURCES.get_or_init(|| Mutex::new(Vec::new()))
}

/// Registers a live endpoint as a process-local RPC pressure source.
#[doc(hidden)]
pub fn register_rpc_pressure_source<T>(source: &Arc<T>)
where
    T: RpcPressureSnapshotSource + 'static,
{
    let erased: Arc<dyn RpcPressureSnapshotSource> = source.clone();
    rpc_pressure_sources()
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
        .push(Arc::downgrade(&erased));
}

/// Returns snapshots for every live RPC endpoint wrapper in this process.
///
/// Registrations are weak and therefore do not extend an endpoint's lifetime.
/// Dead registrations are pruned while taking a snapshot.  No RPC payload or
/// per-call history is serialized or published through this registry.
pub fn local_rpc_pressure_snapshots() -> Vec<RpcPressure> {
    let mut sources = rpc_pressure_sources()
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let mut snapshots = Vec::with_capacity(sources.len());
    sources.retain(|source| {
        let Some(source) = source.upgrade() else {
            return false;
        };
        snapshots.push(source.pressure_snapshot());
        true
    });
    snapshots
}

/// The stable identity of a component in a runtime graph.
#[derive(Clone, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
#[serde(transparent)]
pub struct ComponentId(String);

impl ComponentId {
    /// Creates an identifier from its wire representation.
    pub fn new(value: impl Into<String>) -> Self {
        Self(value.into())
    }

    /// Returns the identifier as a string slice.
    pub fn as_str(&self) -> &str {
        &self.0
    }

    /// Returns the owned wire representation.
    pub fn into_inner(self) -> String {
        self.0
    }
}

impl AsRef<str> for ComponentId {
    fn as_ref(&self) -> &str {
        self.as_str()
    }
}

impl From<&str> for ComponentId {
    fn from(value: &str) -> Self {
        Self::new(value)
    }
}

impl From<String> for ComponentId {
    fn from(value: String) -> Self {
        Self::new(value)
    }
}

impl fmt::Display for ComponentId {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

/// The identity of one running process instance.
///
/// A new value is generated for every process lifetime.  The UUID is kept
/// distinct from [`ComponentId`], whose value identifies a component across
/// process restarts.
#[derive(Clone, Copy, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
#[serde(transparent)]
pub struct ProcessInstanceId(Uuid);

impl ProcessInstanceId {
    /// Generates a fresh process-instance identifier.
    pub fn new() -> Self {
        Self(Uuid::new_v4())
    }

    /// Wraps an existing UUID.
    pub const fn from_uuid(value: Uuid) -> Self {
        Self(value)
    }

    /// Returns the wrapped UUID.
    pub const fn as_uuid(&self) -> &Uuid {
        &self.0
    }

    /// Returns the wrapped UUID by value.
    pub const fn into_uuid(self) -> Uuid {
        self.0
    }
}

impl Default for ProcessInstanceId {
    fn default() -> Self {
        Self::new()
    }
}

impl From<Uuid> for ProcessInstanceId {
    fn from(value: Uuid) -> Self {
        Self::from_uuid(value)
    }
}

impl From<ProcessInstanceId> for Uuid {
    fn from(value: ProcessInstanceId) -> Self {
        value.into_uuid()
    }
}

impl fmt::Display for ProcessInstanceId {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(formatter)
    }
}

/// The identity of one capability publication instance.
#[derive(Clone, Copy, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
#[serde(transparent)]
pub struct CapabilityInstanceId(Uuid);

impl CapabilityInstanceId {
    /// Generates a fresh capability-instance identifier.
    pub fn new() -> Self {
        Self(Uuid::new_v4())
    }

    /// Wraps an existing UUID.
    pub const fn from_uuid(value: Uuid) -> Self {
        Self(value)
    }

    /// Returns the wrapped UUID.
    pub const fn as_uuid(&self) -> &Uuid {
        &self.0
    }

    /// Returns the wrapped UUID by value.
    pub const fn into_uuid(self) -> Uuid {
        self.0
    }

    /// Returns the transport service name for this capability instance.
    ///
    /// Every activation-owned capability gets its own endpoint.  Keeping the
    /// instance identifier in the service name prevents a delayed cleanup of
    /// an old activation from removing a newly-created stream.
    pub fn stream_service_name(&self) -> String {
        format!("rbt/cap/{self}/stream")
    }

    /// Returns the transport service name for an RPC capability instance.
    pub fn rpc_service_name(&self) -> String {
        format!("rbt/cap/{self}/rpc")
    }

    /// Returns the physical stream endpoint for this capability instance.
    pub fn physical_endpoint(&self) -> String {
        self.stream_service_name()
    }

    /// Alias for [`CapabilityInstanceId::physical_endpoint`].
    pub fn endpoint(&self) -> String {
        self.physical_endpoint()
    }
}

impl Default for CapabilityInstanceId {
    fn default() -> Self {
        Self::new()
    }
}

impl From<Uuid> for CapabilityInstanceId {
    fn from(value: Uuid) -> Self {
        Self::from_uuid(value)
    }
}

impl From<CapabilityInstanceId> for Uuid {
    fn from(value: CapabilityInstanceId) -> Self {
        value.into_uuid()
    }
}

impl fmt::Display for CapabilityInstanceId {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(formatter)
    }
}

/// An incarnation of a composer.
///
/// Revisions from different epochs belong to independent histories and must
/// not be ordered against one another.
#[derive(Clone, Copy, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
#[serde(transparent)]
pub struct ComposerEpoch(Uuid);

impl ComposerEpoch {
    /// Generates a fresh composer epoch.
    pub fn new() -> Self {
        Self(Uuid::new_v4())
    }

    /// Wraps an existing UUID as a composer epoch.
    pub const fn from_uuid(value: Uuid) -> Self {
        Self(value)
    }

    /// Returns the wrapped UUID.
    pub const fn as_uuid(&self) -> &Uuid {
        &self.0
    }

    /// Returns the wrapped UUID by value.
    pub const fn into_uuid(self) -> Uuid {
        self.0
    }
}

impl Default for ComposerEpoch {
    fn default() -> Self {
        Self::new()
    }
}

impl From<Uuid> for ComposerEpoch {
    fn from(value: Uuid) -> Self {
        Self::from_uuid(value)
    }
}

impl From<ComposerEpoch> for Uuid {
    fn from(value: ComposerEpoch) -> Self {
        value.into_uuid()
    }
}

impl fmt::Display for ComposerEpoch {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(formatter)
    }
}

/// A monotonically increasing identifier for one component activation.
#[derive(
    Clone, Copy, Debug, Default, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize,
)]
#[serde(transparent)]
pub struct ActivationId(u64);

impl ActivationId {
    /// Creates an activation identifier from its wire value.
    pub const fn new(value: u64) -> Self {
        Self(value)
    }

    /// Returns the first activation identifier.
    pub const fn first() -> Self {
        Self(1)
    }

    /// Returns the numeric activation identifier.
    pub const fn value(self) -> u64 {
        self.0
    }

    /// Returns the numeric activation identifier.
    pub const fn get(self) -> u64 {
        self.value()
    }

    /// Returns the numeric activation identifier.
    pub const fn into_inner(self) -> u64 {
        self.value()
    }

    /// Returns the next activation identifier.
    ///
    /// Saturation keeps the identifier monotonic even at the numeric limit.
    pub const fn next(self) -> Self {
        Self(self.0.saturating_add(1))
    }
}

impl From<u64> for ActivationId {
    fn from(value: u64) -> Self {
        Self::new(value)
    }
}

impl From<ActivationId> for u64 {
    fn from(value: ActivationId) -> Self {
        value.into_inner()
    }
}

impl fmt::Display for ActivationId {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(formatter)
    }
}

/// A position in a composer's revision history.
///
/// The sequence is ordered only within one [`ComposerEpoch`].  `Revision`
/// intentionally does not implement `Ord` or `PartialOrd`, since revisions
/// from separate epochs have no meaningful ordering.
#[derive(Clone, Copy, Debug, Deserialize, Eq, Hash, PartialEq, Serialize)]
pub struct Revision {
    /// Composer incarnation that owns this revision history.
    pub epoch: ComposerEpoch,
    /// Monotonic sequence number within `epoch`.
    pub sequence: u64,
}

impl Revision {
    /// Creates a revision with an explicit epoch and sequence number.
    pub const fn new(epoch: ComposerEpoch, sequence: u64) -> Self {
        Self { epoch, sequence }
    }

    /// Creates the first revision in an epoch.
    pub const fn initial(epoch: ComposerEpoch) -> Self {
        Self::new(epoch, 0)
    }

    /// Returns the revision's composer epoch.
    pub const fn epoch(&self) -> ComposerEpoch {
        self.epoch
    }

    /// Returns the sequence number within the revision's epoch.
    pub const fn sequence(&self) -> u64 {
        self.sequence
    }

    /// Returns whether two revisions can be compared by sequence number.
    pub fn same_epoch(&self, other: &Self) -> bool {
        self.epoch == other.epoch
    }

    /// Advances this revision within the same epoch.
    ///
    /// Saturation preserves monotonicity if the sequence reaches `u64::MAX`.
    pub const fn advance(self) -> Self {
        Self::new(self.epoch, self.sequence.saturating_add(1))
    }

    /// Returns the next revision within the same epoch.
    pub const fn next(self) -> Self {
        self.advance()
    }

    /// Compares two revisions when they belong to the same epoch.
    pub fn cmp_same_epoch(&self, other: &Self) -> Option<std::cmp::Ordering> {
        self.same_epoch(other)
            .then(|| self.sequence.cmp(&other.sequence))
    }
}

impl fmt::Display for Revision {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "{}:{}", self.epoch, self.sequence)
    }
}

/// A capability type known to the component graph.
///
/// `TYPE_NAME` is intentionally a stable string rather than a Rust type name:
/// it is included in control-plane messages and therefore must be understood
/// by independently compiled processes.
pub trait Capability: Send + Sync + 'static {
    const TYPE_NAME: &'static str;
}

/// The transport used by one activation-owned capability publication.
#[derive(Clone, Copy, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum CapabilityTransportKind {
    /// A one-way data-plane stream.
    Stream,
    /// A typed local request/response endpoint.
    Rpc,
}

/// The transport-specific endpoint metadata of a capability publication.
#[derive(Clone, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
#[serde(
    tag = "transport_kind",
    content = "descriptor",
    rename_all = "snake_case"
)]
pub enum CapabilityEndpoint {
    /// Stream endpoint metadata.
    Stream(StreamDescriptor),
    /// RPC endpoint metadata.
    Rpc(RpcDescriptor),
}

impl CapabilityEndpoint {
    /// Returns the transport kind represented by this endpoint.
    pub const fn transport_kind(&self) -> CapabilityTransportKind {
        match self {
            Self::Stream(_) => CapabilityTransportKind::Stream,
            Self::Rpc(_) => CapabilityTransportKind::Rpc,
        }
    }

    /// Returns the physical service name of this endpoint.
    pub fn service_name(&self) -> &str {
        match self {
            Self::Stream(descriptor) => descriptor.service_name(),
            Self::Rpc(descriptor) => descriptor.service_name(),
        }
    }

    /// Returns the stream descriptor, if this is a stream endpoint.
    pub fn try_stream(&self) -> Option<&StreamDescriptor> {
        match self {
            Self::Stream(descriptor) => Some(descriptor),
            Self::Rpc(_) => None,
        }
    }

    /// Returns the RPC descriptor, if this is an RPC endpoint.
    pub fn try_rpc(&self) -> Option<&RpcDescriptor> {
        match self {
            Self::Stream(_) => None,
            Self::Rpc(descriptor) => Some(descriptor),
        }
    }
}

/// The complete identity of one activation-owned capability publication.
///
/// A capability instance is valid only for the activation that created it.
/// The provider component and process fields make that ownership explicit so
/// stale teardown events cannot remove a replacement publication.
#[derive(Clone, Debug, Eq, Hash, PartialEq)]
pub struct CapabilityDescriptor {
    /// Stable identity of this capability publication instance.
    pub capability_instance: CapabilityInstanceId,
    /// Logical component publishing the capability.
    pub provider_component: ComponentId,
    /// Process instance that owns the publication.
    pub provider_process: ProcessInstanceId,
    /// Activation that owns the publication.
    pub activation: ActivationId,
    /// Physical endpoint and transport-specific payload metadata.
    pub endpoint: CapabilityEndpoint,
}

#[derive(Deserialize)]
struct CapabilityDescriptorWire {
    capability_instance: CapabilityInstanceId,
    provider_component: ComponentId,
    provider_process: ProcessInstanceId,
    activation: ActivationId,
    #[serde(default)]
    stream: Option<StreamDescriptor>,
    #[serde(default)]
    rpc: Option<RpcDescriptor>,
}

impl Serialize for CapabilityDescriptor {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        #[derive(Serialize)]
        struct Wire<'a> {
            capability_instance: CapabilityInstanceId,
            provider_component: &'a ComponentId,
            provider_process: ProcessInstanceId,
            activation: ActivationId,
            #[serde(skip_serializing_if = "Option::is_none")]
            stream: Option<&'a StreamDescriptor>,
            #[serde(skip_serializing_if = "Option::is_none")]
            rpc: Option<&'a RpcDescriptor>,
        }

        let (stream, rpc) = match &self.endpoint {
            CapabilityEndpoint::Stream(descriptor) => (Some(descriptor), None),
            CapabilityEndpoint::Rpc(descriptor) => (None, Some(descriptor)),
        };
        Wire {
            capability_instance: self.capability_instance,
            provider_component: &self.provider_component,
            provider_process: self.provider_process,
            activation: self.activation,
            stream,
            rpc,
        }
        .serialize(serializer)
    }
}

impl<'de> Deserialize<'de> for CapabilityDescriptor {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let wire = CapabilityDescriptorWire::deserialize(deserializer)?;
        let endpoint = match (wire.stream, wire.rpc) {
            (Some(stream), None) => CapabilityEndpoint::Stream(stream),
            (None, Some(rpc)) => CapabilityEndpoint::Rpc(rpc),
            (Some(_), Some(_)) => {
                return Err(D::Error::custom(
                    "capability descriptor must contain exactly one of stream or rpc",
                ));
            }
            (None, None) => {
                return Err(D::Error::custom(
                    "capability descriptor must contain exactly one of stream or rpc",
                ));
            }
        };
        Ok(Self {
            capability_instance: wire.capability_instance,
            provider_component: wire.provider_component,
            provider_process: wire.provider_process,
            activation: wire.activation,
            endpoint,
        })
    }
}

impl CapabilityDescriptor {
    /// Creates a complete capability descriptor from its wire fields.
    pub fn new(
        capability_instance: CapabilityInstanceId,
        provider_component: ComponentId,
        provider_process: ProcessInstanceId,
        activation: ActivationId,
        stream: StreamDescriptor,
    ) -> Self {
        Self::new_stream(
            capability_instance,
            provider_component,
            provider_process,
            activation,
            stream,
        )
    }

    /// Creates a complete stream capability descriptor from its wire fields.
    pub fn new_stream(
        capability_instance: CapabilityInstanceId,
        provider_component: ComponentId,
        provider_process: ProcessInstanceId,
        activation: ActivationId,
        stream: StreamDescriptor,
    ) -> Self {
        Self {
            capability_instance,
            provider_component,
            provider_process,
            activation,
            endpoint: CapabilityEndpoint::Stream(stream),
        }
    }

    /// Creates a complete RPC capability descriptor from its wire fields.
    pub fn new_rpc(
        capability_instance: CapabilityInstanceId,
        provider_component: ComponentId,
        provider_process: ProcessInstanceId,
        activation: ActivationId,
        rpc: RpcDescriptor,
    ) -> Self {
        Self {
            capability_instance,
            provider_component,
            provider_process,
            activation,
            endpoint: CapabilityEndpoint::Rpc(rpc),
        }
    }

    /// Returns the capability instance identity.
    pub const fn capability_instance(&self) -> CapabilityInstanceId {
        self.capability_instance
    }

    /// Returns the logical provider component.
    pub fn provider_component(&self) -> &ComponentId {
        &self.provider_component
    }

    /// Returns the owning process instance.
    pub const fn provider_process(&self) -> ProcessInstanceId {
        self.provider_process
    }

    /// Returns the owning activation.
    pub const fn activation(&self) -> ActivationId {
        self.activation
    }

    /// Returns the physical endpoint metadata.
    pub fn endpoint(&self) -> &CapabilityEndpoint {
        &self.endpoint
    }

    /// Returns the transport kind of this capability.
    pub const fn transport_kind(&self) -> CapabilityTransportKind {
        match self.endpoint {
            CapabilityEndpoint::Stream(_) => CapabilityTransportKind::Stream,
            CapabilityEndpoint::Rpc(_) => CapabilityTransportKind::Rpc,
        }
    }

    /// Returns the stream descriptor, if this capability is a stream.
    pub fn try_stream(&self) -> Option<&StreamDescriptor> {
        self.endpoint.try_stream()
    }

    /// Returns the RPC descriptor, if this capability is an RPC endpoint.
    pub fn try_rpc(&self) -> Option<&RpcDescriptor> {
        self.endpoint.try_rpc()
    }

    /// Returns the physical stream descriptor.
    ///
    /// # Panics
    ///
    /// Panics when called for an RPC capability. Use [`Self::try_stream`] when
    /// the transport is not statically known.
    pub fn stream(&self) -> &StreamDescriptor {
        self.try_stream()
            .expect("CapabilityDescriptor::stream called for an RPC capability")
    }

    /// Returns the transport service name of the physical endpoint.
    pub fn service_name(&self) -> &str {
        self.endpoint.service_name()
    }

    /// Returns the complete message ABI descriptor of the physical stream.
    ///
    /// # Panics
    ///
    /// Panics when called for an RPC capability. RPC callers should use
    /// [`Self::try_rpc`] and inspect its request and response descriptors.
    pub fn message_type(&self) -> &MessageTypeDescriptor {
        self.stream().message_type()
    }

    /// Returns the intended profile of the physical stream.
    ///
    /// # Panics
    ///
    /// Panics when called for an RPC capability. Use [`Self::transport_kind`]
    /// before accessing stream-only metadata.
    pub const fn profile(&self) -> StreamProfile {
        match &self.endpoint {
            CapabilityEndpoint::Stream(stream) => stream.profile(),
            CapabilityEndpoint::Rpc(_) => {
                panic!("CapabilityDescriptor::profile called for an RPC capability")
            }
        }
    }

    /// Returns the qualified message type name of the physical stream.
    #[deprecated(note = "use message_type() or stream().message_type() instead")]
    pub fn type_name(&self) -> String {
        self.message_type().qualified_name()
    }

    /// Alias retained for callers that refer to the service name as a stream
    /// name.
    pub fn name(&self) -> &str {
        self.service_name()
    }
}

impl Ord for CapabilityDescriptor {
    fn cmp(&self, other: &Self) -> std::cmp::Ordering {
        self.capability_instance
            .cmp(&other.capability_instance)
            .then_with(|| self.provider_component.cmp(&other.provider_component))
            .then_with(|| self.provider_process.cmp(&other.provider_process))
            .then_with(|| self.activation.cmp(&other.activation))
            .then_with(|| self.endpoint.cmp(&other.endpoint))
    }
}

impl PartialOrd for CapabilityDescriptor {
    fn partial_cmp(&self, other: &Self) -> Option<std::cmp::Ordering> {
        Some(self.cmp(other))
    }
}

/// An untyped capability advertised by a component.
#[derive(Clone, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
pub struct ProvidedCapability {
    /// Stable capability role being advertised (for example, `Camera`).
    pub capability_type: String,
    /// Complete identity of the stream made available by the provider.
    pub descriptor: CapabilityDescriptor,
}

impl ProvidedCapability {
    /// Advertises a capability using its role and endpoint descriptor.
    pub fn new(capability_type: impl Into<String>, descriptor: CapabilityDescriptor) -> Self {
        Self {
            capability_type: capability_type.into(),
            descriptor,
        }
    }

    /// Advertises an independently typed capability for a capability role.
    pub fn typed<C: Capability>(descriptor: CapabilityDescriptor) -> Self {
        Self::new(C::TYPE_NAME, descriptor)
    }

    /// Returns the complete advertised capability descriptor.
    pub fn descriptor(&self) -> &CapabilityDescriptor {
        &self.descriptor
    }

    /// Returns the advertised transport kind.
    pub fn transport_kind(&self) -> CapabilityTransportKind {
        self.descriptor.transport_kind()
    }

    /// Returns the advertised stream descriptor, if this is a stream.
    pub fn try_stream(&self) -> Option<&StreamDescriptor> {
        self.descriptor.try_stream()
    }

    /// Returns the advertised RPC descriptor, if this is an RPC endpoint.
    pub fn try_rpc(&self) -> Option<&RpcDescriptor> {
        self.descriptor.try_rpc()
    }

    /// Returns the advertised physical stream descriptor.
    ///
    /// # Panics
    ///
    /// Panics for an RPC capability. Use [`Self::try_stream`] when the
    /// transport is not statically known.
    pub fn stream(&self) -> &StreamDescriptor {
        self.descriptor.stream()
    }

    /// Returns the advertised physical endpoint name.
    pub fn name(&self) -> &str {
        self.descriptor.service_name()
    }

    /// Returns the advertised capability role.
    pub fn capability_type(&self) -> &str {
        &self.capability_type
    }

    /// Converts this advertisement to its wire descriptor.
    pub fn into_descriptor(self) -> CapabilityDescriptor {
        self.descriptor
    }
}

/// Untyped form of a requirement, suitable for a control-plane message.
#[derive(Clone, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
pub struct RequirementSpec {
    /// Component-local name of the required capability.
    pub requirement_name: String,
    /// Stable capability role name.
    pub capability_type: String,
}

impl RequirementSpec {
    /// Creates a requirement specification from its wire fields.
    pub fn new(requirement_name: impl Into<String>, capability_type: impl Into<String>) -> Self {
        Self {
            requirement_name: requirement_name.into(),
            capability_type: capability_type.into(),
        }
    }

    /// Creates a requirement specification for a typed capability.
    pub fn typed<C: Capability>(requirement_name: impl Into<String>) -> Self {
        Self::new(requirement_name, C::TYPE_NAME)
    }

    /// Returns the requirement name.
    pub fn name(&self) -> &str {
        &self.requirement_name
    }

    /// Returns the required capability role name.
    pub fn requirement_name(&self) -> &str {
        &self.requirement_name
    }

    /// Returns the required capability role name.
    pub fn capability_type(&self) -> &str {
        &self.capability_type
    }

    /// Alias retained for code that treats the role as a type discriminator.
    pub fn type_name(&self) -> &str {
        self.capability_type()
    }
}

/// A typed requirement declared by a component.
#[derive(Clone, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct Requirement<C: Capability> {
    /// Untyped data carried over the control plane.
    pub spec: RequirementSpec,
    marker: PhantomData<fn() -> C>,
}

impl<C: Capability> Serialize for Requirement<C> {
    fn serialize<S: Serializer>(&self, serializer: S) -> Result<S::Ok, S::Error> {
        self.spec.serialize(serializer)
    }
}

impl<'de, C: Capability> Deserialize<'de> for Requirement<C> {
    fn deserialize<D: Deserializer<'de>>(deserializer: D) -> Result<Self, D::Error> {
        let spec = RequirementSpec::deserialize(deserializer)?;
        Self::from_spec(spec).map_err(D::Error::custom)
    }
}

impl<C: Capability> Requirement<C> {
    /// Declares a requirement for a typed capability.
    pub fn new(name: impl Into<String>) -> Self {
        Self {
            spec: RequirementSpec::typed::<C>(name),
            marker: PhantomData,
        }
    }

    /// Rehydrates a typed requirement from a wire specification.
    pub fn from_spec(spec: RequirementSpec) -> Result<Self, RequirementTypeError> {
        if spec.capability_type == C::TYPE_NAME {
            Ok(Self {
                spec,
                marker: PhantomData,
            })
        } else {
            Err(RequirementTypeError {
                expected: C::TYPE_NAME.to_owned(),
                actual: spec.capability_type,
            })
        }
    }

    /// Returns the untyped wire representation.
    pub fn spec(&self) -> &RequirementSpec {
        &self.spec
    }

    /// Consumes the typed wrapper and returns its wire representation.
    pub fn into_spec(self) -> RequirementSpec {
        self.spec
    }

    /// Returns the requirement name.
    pub fn name(&self) -> &str {
        self.spec.name()
    }

    /// Returns the requirement's wire name.
    pub fn requirement_name(&self) -> &str {
        self.spec.requirement_name()
    }

    /// Returns the statically associated capability type name.
    pub fn type_name(&self) -> &'static str {
        C::TYPE_NAME
    }

    /// Returns the requirement's capability role.
    pub fn capability_type(&self) -> &'static str {
        C::TYPE_NAME
    }
}

impl<C: Capability> From<Requirement<C>> for RequirementSpec {
    fn from(requirement: Requirement<C>) -> Self {
        requirement.into_spec()
    }
}

/// A type mismatch encountered while rehydrating a typed requirement.
#[derive(Clone, Debug, Eq, Error, PartialEq)]
#[error("requirement type mismatch: expected {expected:?}, got {actual:?}")]
pub struct RequirementTypeError {
    /// Type name required by the Rust capability parameter.
    pub expected: String,
    /// Type name received from the control plane.
    pub actual: String,
}

/// A resolved, typed binding between a requirement and a provided capability.
#[derive(Clone, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct Binding<C: Capability> {
    /// The typed requirement being satisfied.
    pub requirement: Requirement<C>,
    /// Component providing the stream.
    pub provider: ComponentId,
    /// Complete identity of the provider capability instance.
    pub descriptor: CapabilityDescriptor,
}

impl<C: Capability> Binding<C> {
    /// Creates a binding from a typed requirement and provider information.
    pub fn new(
        requirement: Requirement<C>,
        provider: ComponentId,
        descriptor: CapabilityDescriptor,
    ) -> Self {
        Self {
            requirement,
            provider,
            descriptor,
        }
    }

    /// Creates a binding from a typed requirement and an advertisement.
    pub fn from_provided(
        requirement: Requirement<C>,
        provider: ComponentId,
        provided: ProvidedCapability,
    ) -> Self {
        Self::new(requirement, provider, provided.into_descriptor())
    }

    /// Validates that the advertisement has the capability role expected by
    /// `C`, while keeping its payload type independent.
    pub fn try_new(
        requirement: Requirement<C>,
        provider: ComponentId,
        provided: ProvidedCapability,
    ) -> Result<Self, BindingTypeError> {
        if provided.capability_type == C::TYPE_NAME {
            Ok(Self::from_provided(requirement, provider, provided))
        } else {
            Err(BindingTypeError {
                expected: C::TYPE_NAME.to_owned(),
                actual: provided.capability_type,
            })
        }
    }

    /// Returns the typed requirement.
    pub fn requirement(&self) -> &Requirement<C> {
        &self.requirement
    }

    /// Returns the resolved physical stream descriptor.
    ///
    /// # Panics
    ///
    /// Panics for an RPC capability. Use [`Self::try_stream`] when the
    /// transport is not statically known.
    pub fn stream(&self) -> &StreamDescriptor {
        self.descriptor.stream()
    }

    /// Returns the transport kind of the resolved endpoint.
    pub fn transport_kind(&self) -> CapabilityTransportKind {
        self.descriptor.transport_kind()
    }

    /// Returns the resolved stream descriptor, if this is a stream.
    pub fn try_stream(&self) -> Option<&StreamDescriptor> {
        self.descriptor.try_stream()
    }

    /// Returns the resolved RPC descriptor, if this is an RPC endpoint.
    pub fn try_rpc(&self) -> Option<&RpcDescriptor> {
        self.descriptor.try_rpc()
    }

    /// Returns the complete resolved capability descriptor.
    pub fn descriptor(&self) -> &CapabilityDescriptor {
        &self.descriptor
    }

    /// Converts the typed binding to its serializable wire representation.
    pub fn into_resolved(self) -> ResolvedBinding {
        let requirement = self.requirement.into_spec();
        ResolvedBinding::new(
            requirement.requirement_name,
            requirement.capability_type,
            self.provider,
            self.descriptor,
        )
    }

    /// Converts the typed binding to its serializable wire representation.
    pub fn resolved(&self) -> ResolvedBinding {
        ResolvedBinding::new(
            self.requirement.spec.requirement_name.clone(),
            self.requirement.spec.capability_type.clone(),
            self.provider.clone(),
            self.descriptor.clone(),
        )
    }
}

/// A type mismatch encountered while constructing a typed binding.
#[derive(Clone, Debug, Eq, Error, PartialEq)]
#[error("binding type mismatch: expected {expected:?}, got {actual:?}")]
pub struct BindingTypeError {
    /// Type name required by the Rust capability parameter.
    pub expected: String,
    /// Type name advertised by the provider.
    pub actual: String,
}

/// Wire representation of a resolved binding.
#[derive(Clone, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
pub struct ResolvedBinding {
    /// Requirement name selected by the resolver.
    pub requirement_name: String,
    /// Capability role selected by the resolver.
    pub capability_type: String,
    /// Component providing the stream.
    pub provider: ComponentId,
    /// Complete identity of the capability instance selected to satisfy the
    /// requirement.
    pub descriptor: CapabilityDescriptor,
}

impl ResolvedBinding {
    /// Creates a wire binding from its untyped fields.
    pub fn new(
        requirement_name: impl Into<String>,
        capability_type: impl Into<String>,
        provider: ComponentId,
        descriptor: CapabilityDescriptor,
    ) -> Self {
        Self {
            requirement_name: requirement_name.into(),
            capability_type: capability_type.into(),
            provider,
            descriptor,
        }
    }

    /// Rehydrates a wire binding into the requested typed binding.
    pub fn into_binding<C: Capability>(self) -> Result<Binding<C>, BindingTypeError> {
        if self.capability_type != C::TYPE_NAME {
            return Err(BindingTypeError {
                expected: C::TYPE_NAME.to_owned(),
                actual: self.capability_type,
            });
        }

        let requirement = Requirement::new(self.requirement_name);
        Ok(Binding::new(requirement, self.provider, self.descriptor))
    }
}

impl<C: Capability> From<Binding<C>> for ResolvedBinding {
    fn from(binding: Binding<C>) -> Self {
        binding.into_resolved()
    }
}

/// Why a component is waiting before it can continue preparing.
#[derive(Clone, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
#[serde(transparent)]
pub struct BlockReason(String);

impl BlockReason {
    /// Creates a reason from a human-readable description.
    pub fn new(reason: impl Into<String>) -> Self {
        Self(reason.into())
    }

    /// Returns the reason text.
    pub fn as_str(&self) -> &str {
        &self.0
    }

    /// Returns the owned reason text.
    pub fn into_inner(self) -> String {
        self.0
    }
}

impl AsRef<str> for BlockReason {
    fn as_ref(&self) -> &str {
        self.as_str()
    }
}

impl From<&str> for BlockReason {
    fn from(value: &str) -> Self {
        Self::new(value)
    }
}

impl From<String> for BlockReason {
    fn from(value: String) -> Self {
        Self::new(value)
    }
}

impl fmt::Display for BlockReason {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

/// Lifecycle state of a component.
#[derive(Clone, Debug, Deserialize, Eq, Hash, Ord, PartialEq, PartialOrd, Serialize)]
pub enum ComponentState {
    /// The component has been registered but not prepared.
    Declared,
    /// Requirements and resources are being prepared.
    Preparing,
    /// Preparation cannot continue until the given condition changes.
    Blocked(BlockReason),
    /// Resources are ready and activation is in progress.
    Activating,
    /// The component is running.
    Active,
    /// The component is draining and releasing resources.
    Quiescing,
    /// The component cannot continue without an explicit restart decision.
    Failed(String),
}

impl ComponentState {
    /// Returns whether `next` is a legal immediate successor of this state.
    pub fn can_transition_to(&self, next: &Self) -> bool {
        use ComponentState::*;

        matches!(
            (self, next),
            (Declared, Preparing)
                | (Preparing, Activating)
                | (Preparing, Blocked(_))
                | (Preparing, Failed(_))
                | (Blocked(_), Activating)
                | (Blocked(_), Failed(_))
                | (Activating, Active)
                | (Activating, Failed(_))
                | (Active, Quiescing)
                | (Active, Failed(_))
                | (Quiescing, Blocked(_))
                | (Quiescing, Failed(_))
        )
    }

    /// Applies a legal transition and returns the resulting state.
    ///
    /// Returning a clone makes this convenient both for a mutable state holder
    /// (`state.transition(next)?`) and for callers that want the new value.
    pub fn transition(&mut self, next: Self) -> Result<Self, StateTransitionError> {
        if self.can_transition_to(&next) {
            *self = next.clone();
            Ok(next)
        } else {
            Err(StateTransitionError {
                from: self.clone(),
                to: next,
            })
        }
    }

    /// Alias for [`ComponentState::transition`].
    pub fn transition_to(&mut self, next: Self) -> Result<Self, StateTransitionError> {
        self.transition(next)
    }

    /// Consumes the current state and returns the result of a legal transition.
    pub fn into_transition(self, next: Self) -> Result<Self, StateTransitionError> {
        if self.can_transition_to(&next) {
            Ok(next)
        } else {
            Err(StateTransitionError {
                from: self,
                to: next,
            })
        }
    }
}

/// Error returned when a component attempts an illegal lifecycle transition.
#[derive(Clone, Debug, Eq, Error, PartialEq)]
#[error("invalid component state transition from {from:?} to {to:?}")]
pub struct StateTransitionError {
    /// State before the attempted transition.
    pub from: ComponentState,
    /// Requested state.
    pub to: ComponentState,
}

/// Short alias for callers that use the term transition error.
pub type TransitionError = StateTransitionError;

#[cfg(test)]
mod tests {
    use super::*;
    use rbt_types::{Detection, Frame};

    #[derive(Debug)]
    struct Image;

    impl Capability for Image {
        const TYPE_NAME: &'static str = "image.v1";
    }

    #[test]
    fn component_id_is_an_ordered_serializable_newtype() {
        let first = ComponentId::from("camera-a");
        let second = ComponentId::new(String::from("camera-b"));

        assert_eq!(first.to_string(), "camera-a");
        assert_eq!(first.as_str(), "camera-a");
        assert!(first < second);
        assert_eq!(
            first,
            serde_json::from_str::<ComponentId>("\"camera-a\"").unwrap()
        );
    }

    #[test]
    fn stream_semantics_are_frozen_by_profile() {
        assert_eq!(
            StreamSemanticConfig::for_profile(StreamProfile::Sensor),
            StreamSemanticConfig {
                queue_depth: 1,
                history_size: 0,
                safe_overflow: true,
            }
        );
        assert_eq!(
            StreamSemanticConfig::for_profile(StreamProfile::Control),
            StreamSemanticConfig {
                queue_depth: 4,
                history_size: 0,
                safe_overflow: false,
            }
        );
    }

    #[derive(Debug)]
    struct RpcPressureSourceStub {
        pressure: RpcPressure,
    }

    impl RpcPressureSnapshotSource for RpcPressureSourceStub {
        fn pressure_snapshot(&self) -> RpcPressure {
            self.pressure.clone()
        }
    }

    #[test]
    fn rpc_pressure_registry_is_weak_and_prunes_dead_sources() {
        let service_name = "test-rpc-pressure-registry";
        let source = Arc::new(RpcPressureSourceStub {
            pressure: RpcPressure {
                service_name: service_name.to_owned(),
                server: Some(RpcServerMetrics::default()),
                ..Default::default()
            },
        });
        let weak = Arc::downgrade(&source);
        register_rpc_pressure_source(&source);

        assert!(weak.upgrade().is_some());
        assert!(
            local_rpc_pressure_snapshots()
                .iter()
                .any(|pressure| pressure.service_name == service_name)
        );

        drop(source);
        assert!(weak.upgrade().is_none());
        assert!(
            !local_rpc_pressure_snapshots()
                .iter()
                .any(|pressure| pressure.service_name == service_name)
        );
    }

    #[test]
    fn process_and_capability_instance_ids_are_fresh_and_serializable() {
        let process = ProcessInstanceId::new();
        let capability = CapabilityInstanceId::new();

        assert_ne!(process, ProcessInstanceId::new());
        assert_ne!(capability, CapabilityInstanceId::new());

        let process_json = serde_json::to_string(&process).unwrap();
        let capability_json = serde_json::to_string(&capability).unwrap();
        assert_eq!(process, serde_json::from_str(&process_json).unwrap());
        assert_eq!(capability, serde_json::from_str(&capability_json).unwrap());
        assert_eq!(process.to_string(), process.as_uuid().to_string());
        assert_eq!(capability.to_string(), capability.as_uuid().to_string());
        assert_eq!(
            capability.stream_service_name(),
            format!("rbt/cap/{capability}/stream")
        );
        assert_eq!(
            capability.rpc_service_name(),
            format!("rbt/cap/{capability}/rpc")
        );
    }

    #[test]
    fn activation_ids_advance_numerically() {
        let first = ActivationId::first();
        let second = first.next();
        let third = second.next();

        assert_eq!(first.value(), 1);
        assert_eq!(second.value(), 2);
        assert_eq!(third.value(), 3);
        assert!(first < second);
        assert_eq!(second, serde_json::from_str("2").unwrap());
    }

    #[test]
    fn composer_epochs_are_fresh_and_revisions_only_advance_within_an_epoch() {
        let epoch = ComposerEpoch::new();
        let other_epoch = ComposerEpoch::new();
        assert_ne!(epoch, other_epoch);

        let initial = Revision::initial(epoch);
        let advanced = initial.advance();
        let other = Revision::new(other_epoch, 1);

        assert_eq!(advanced.epoch(), epoch);
        assert_eq!(advanced.sequence(), 1);
        assert!(initial.same_epoch(&advanced));
        assert!(!initial.same_epoch(&other));
        assert_eq!(
            initial.cmp_same_epoch(&advanced),
            Some(std::cmp::Ordering::Less)
        );
        assert_eq!(initial.cmp_same_epoch(&other), None);
        assert_eq!(
            advanced,
            serde_json::from_str(&serde_json::to_string(&advanced).unwrap()).unwrap()
        );
        assert_eq!(advanced.to_string(), format!("{}:1", epoch));
    }

    #[test]
    fn typed_requirements_and_bindings_round_trip_on_the_wire() {
        let requirement = Requirement::<Image>::new("frames");
        let descriptor = CapabilityDescriptor::new(
            CapabilityInstanceId::new(),
            ComponentId::from("camera-a"),
            ProcessInstanceId::new(),
            ActivationId::first(),
            StreamDescriptor::for_fixed::<Frame>("camera.frames", StreamProfile::Sensor),
        );
        let provided = ProvidedCapability::typed::<Image>(descriptor.clone());
        let binding =
            Binding::try_new(requirement, ComponentId::from("camera-a"), provided).unwrap();
        let wire = binding.resolved();
        let encoded = serde_json::to_string(&wire).unwrap();
        let decoded: ResolvedBinding = serde_json::from_str(&encoded).unwrap();
        let restored = decoded.into_binding::<Image>().unwrap();

        assert_eq!(restored.requirement.name(), "frames");
        assert_eq!(restored.provider, ComponentId::from("camera-a"));
        assert_eq!(restored.descriptor, descriptor);
        assert_eq!(restored.stream().service_name(), "camera.frames");
        assert_eq!(restored.stream().message_type(), &Frame::descriptor());
        assert_eq!(restored.descriptor.profile(), StreamProfile::Sensor);
        assert_eq!(
            restored.stream().message_type().layout_hash,
            Frame::descriptor().layout_hash
        );
        #[allow(deprecated)]
        let qualified_name = restored.descriptor.type_name();
        assert_eq!(qualified_name, "rbt.demo/Frame@1");
    }

    #[test]
    fn capability_descriptor_serializes_legacy_stream_and_new_rpc_shapes() {
        let capability_instance = CapabilityInstanceId::from_uuid(Uuid::from_u128(7));
        let common = |endpoint| CapabilityDescriptor {
            capability_instance,
            provider_component: ComponentId::from("camera"),
            provider_process: ProcessInstanceId::from_uuid(Uuid::from_u128(8)),
            activation: ActivationId::new(2),
            endpoint,
        };
        let stream = common(CapabilityEndpoint::Stream(StreamDescriptor::for_fixed::<
            Frame,
        >(
            "camera.frames",
            StreamProfile::Sensor,
        )));
        let stream_value = serde_json::to_value(&stream).unwrap();
        assert!(stream_value.get("stream").is_some());
        assert!(stream_value.get("rpc").is_none());
        assert!(stream_value.get("endpoint").is_none());
        assert_eq!(
            serde_json::from_value::<CapabilityDescriptor>(stream_value).unwrap(),
            stream
        );

        let rpc = common(CapabilityEndpoint::Rpc(RpcDescriptor::for_fixed::<
            Frame,
            Detection,
        >("camera.rpc")));
        let rpc_value = serde_json::to_value(&rpc).unwrap();
        assert!(rpc_value.get("stream").is_none());
        assert!(rpc_value.get("rpc").is_some());
        assert!(rpc_value.get("endpoint").is_none());
        assert_eq!(
            serde_json::from_value::<CapabilityDescriptor>(rpc_value).unwrap(),
            rpc
        );
    }

    #[test]
    fn capability_descriptor_rejects_ambiguous_or_missing_endpoint() {
        let capability_instance = CapabilityInstanceId::from_uuid(Uuid::from_u128(7));
        let stream = StreamDescriptor::for_fixed::<Frame>("camera.frames", StreamProfile::Sensor);
        let rpc = RpcDescriptor::for_fixed::<Frame, Detection>("camera.rpc");
        let common = serde_json::json!({
            "capability_instance": capability_instance,
            "provider_component": "camera",
            "provider_process": Uuid::from_u128(8),
            "activation": 2,
        });

        let missing = common.clone();
        assert!(serde_json::from_value::<CapabilityDescriptor>(missing).is_err());

        let both = serde_json::json!({
            "capability_instance": capability_instance,
            "provider_component": "camera",
            "provider_process": Uuid::from_u128(8),
            "activation": 2,
            "stream": stream,
            "rpc": rpc,
        });
        assert!(serde_json::from_value::<CapabilityDescriptor>(both).is_err());
    }

    #[test]
    fn capability_descriptor_order_includes_transport_resources() {
        let instance = CapabilityInstanceId::from_uuid(Uuid::from_u128(7));
        let base = |endpoint| CapabilityDescriptor {
            capability_instance: instance,
            provider_component: ComponentId::from("camera"),
            provider_process: ProcessInstanceId::from_uuid(Uuid::from_u128(8)),
            activation: ActivationId::new(2),
            endpoint,
        };
        let first = base(CapabilityEndpoint::Stream(
            StreamDescriptor::for_fixed_with_resources::<Frame>(
                "camera.frames",
                StreamProfile::Sensor,
                StreamResourceConfig::new(1, 1),
            ),
        ));
        let second = base(CapabilityEndpoint::Stream(
            StreamDescriptor::for_fixed_with_resources::<Frame>(
                "camera.frames",
                StreamProfile::Sensor,
                StreamResourceConfig::new(2, 1),
            ),
        ));
        assert_ne!(first, second);
        assert_ne!(first.cmp(&second), std::cmp::Ordering::Equal);
    }

    #[test]
    fn mismatched_wire_types_are_rejected() {
        let wrong = RequirementSpec::new("frames", "audio.v1");
        let error = Requirement::<Image>::from_spec(wrong).unwrap_err();
        assert_eq!(error.expected, Image::TYPE_NAME);
        assert_eq!(error.actual, "audio.v1");

        let error = Binding::<Image>::try_new(
            Requirement::new("frames"),
            ComponentId::from("camera-a"),
            ProvidedCapability::new(
                "audio.v1",
                CapabilityDescriptor::new(
                    CapabilityInstanceId::new(),
                    ComponentId::from("camera-a"),
                    ProcessInstanceId::new(),
                    ActivationId::first(),
                    StreamDescriptor::for_fixed::<Detection>("audio", StreamProfile::Sensor),
                ),
            ),
        )
        .unwrap_err();
        assert_eq!(error.expected, Image::TYPE_NAME);
    }

    #[test]
    fn component_state_supports_the_complete_lifecycle() {
        let blocked = BlockReason::new("waiting for camera stream");
        let mut state = ComponentState::Declared;

        state.transition(ComponentState::Preparing).unwrap();
        state
            .transition(ComponentState::Blocked(blocked.clone()))
            .unwrap();
        state.transition(ComponentState::Activating).unwrap();
        state.transition(ComponentState::Active).unwrap();
        state.transition(ComponentState::Quiescing).unwrap();
        state
            .transition(ComponentState::Blocked(blocked.clone()))
            .unwrap();

        assert_eq!(state, ComponentState::Blocked(blocked.clone()));
        assert_eq!(blocked.as_str(), "waiting for camera stream");
    }

    #[test]
    fn illegal_component_state_transitions_return_thiserror() {
        let mut state = ComponentState::Declared;
        let error = state.transition(ComponentState::Active).unwrap_err();

        assert_eq!(error.from, ComponentState::Declared);
        assert_eq!(error.to, ComponentState::Active);
        assert!(!state.can_transition_to(&ComponentState::Failed("boom".into())));
        assert!(matches!(
            ComponentState::Declared.into_transition(ComponentState::Active),
            Err(StateTransitionError { .. })
        ));
    }
}

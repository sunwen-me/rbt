//! Experimental event-driven waiting for rbt streams.
//!
//! iceoryx2 0.10 does not make a publish/subscribe `Subscriber` waitable.
//! Stream wrappers therefore expose an event sidecar listener and this module
//! multiplexes those listeners with one activation-scoped cancellation event.
//! The sidecar only moves readiness; payloads stay in the normal loan/borrow
//! path.

use std::{
    fmt,
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
    time::{Duration, Instant},
};

use iceoryx2::{
    node::{Node, NodeBuilder, NodeCreationFailure},
    port::{
        listener::{Listener, ListenerCreateError, ListenerWaitError},
        notifier::{Notifier, NotifierCreateError, NotifierNotifyError},
    },
    prelude::{CallbackProgression, SignalHandlingMode, ipc_threadsafe},
    service::{
        builder::event::{EventOpenError, EventOpenOrCreateError},
        service_name::{ServiceName, ServiceNameError},
    },
    waitset::{
        WaitSetAttachmentError, WaitSetAttachmentId, WaitSetBuilder, WaitSetCreateError,
        WaitSetRunError,
    },
};
use rbt_core::{ActivationId, ProcessInstanceId};
use thiserror::Error;

/// The maximum number of payload receive attempts an event handler should
/// perform for one ready source before returning to the wait set.
///
/// This is an implementation fairness bound, not a stream queue or QoS
/// setting.  It is public solely to make instrumentation and tests explicit;
/// callers should otherwise treat it as experimental.
#[doc(hidden)]
pub const MAX_DRAIN_PER_SOURCE: usize = 64;

const CANCELLATION_TOKEN_VALUE: u64 = u64::MAX;
const CANCELLATION_SERVICE_PREFIX: &str = "rbt/cancel/";
// This low-frequency native WaitSet interval is a cancellation-liveness
// backstop only. It never calls receive() and is not a data polling path.
const CANCELLATION_LIVENESS_INTERVAL: Duration = Duration::from_secs(1);

/// A stable rbt-assigned identifier for one readiness source.
///
/// iceoryx2 attachment identifiers contain transient wait-set implementation
/// details.  `WaitToken` is the only identity that crosses the wait callback
/// boundary and remains stable for the lifetime of a source registration.
#[derive(Clone, Copy, Debug, Default, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub struct WaitToken(u64);

impl WaitToken {
    /// The reserved token used by a [`WaitContext`] cancellation source.
    pub const CANCELLATION: Self = Self(CANCELLATION_TOKEN_VALUE);

    /// Creates a token from an application-assigned value.
    pub const fn new(value: u64) -> Self {
        Self(value)
    }

    /// Returns the numeric token value.
    pub const fn value(self) -> u64 {
        self.0
    }

    /// Alias for [`Self::value`].
    pub const fn get(self) -> u64 {
        self.value()
    }

    /// Alias for [`Self::value`].
    pub const fn into_inner(self) -> u64 {
        self.value()
    }
}

impl From<u64> for WaitToken {
    fn from(value: u64) -> Self {
        Self::new(value)
    }
}

impl From<WaitToken> for u64 {
    fn from(value: WaitToken) -> Self {
        value.into_inner()
    }
}

impl fmt::Display for WaitToken {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(formatter)
    }
}

/// Identifies why a [`ReadyEvent`] was dispatched.
#[derive(Clone, Copy, Debug, Eq, Hash, Ord, PartialEq, PartialOrd)]
pub enum ReadyEventKind {
    /// A stream readiness sidecar listener fired.
    Source,
    /// The activation cancellation event fired.
    Cancellation,
}

/// A readiness notification delivered to a wait handler.
///
/// The event contains no payload and no payload metadata.  A source handler
/// must call its stream subscriber's normal `receive()` method to borrow data.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct ReadyEvent {
    /// Stable rbt token of the source that became ready.
    pub token: WaitToken,
    /// Whether this event came from a stream source or cancellation.
    pub kind: ReadyEventKind,
    /// Whether this was the one synthetic source pass performed before the
    /// first blocking wait.
    pub synthetic: bool,
}

impl ReadyEvent {
    /// Creates a source readiness event.
    pub const fn source(token: WaitToken) -> Self {
        Self {
            token,
            kind: ReadyEventKind::Source,
            synthetic: false,
        }
    }

    /// Creates a synthetic source readiness event.
    pub const fn synthetic_source(token: WaitToken) -> Self {
        Self {
            token,
            kind: ReadyEventKind::Source,
            synthetic: true,
        }
    }

    /// Creates a cancellation readiness event.
    pub const fn cancellation() -> Self {
        Self {
            token: WaitToken::CANCELLATION,
            kind: ReadyEventKind::Cancellation,
            synthetic: false,
        }
    }

    /// Returns this event's stable token.
    pub const fn token(self) -> WaitToken {
        self.token
    }

    /// Returns this event's kind.
    pub const fn kind(self) -> ReadyEventKind {
        self.kind
    }

    /// Returns true for an activation cancellation event.
    pub const fn is_cancellation(self) -> bool {
        matches!(self.kind, ReadyEventKind::Cancellation)
    }

    /// Alias for [`Self::is_cancellation`].
    pub const fn is_cancelled(self) -> bool {
        self.is_cancellation()
    }

    /// Returns true for the synthetic initial source pass.
    pub const fn is_synthetic(self) -> bool {
        self.synthetic
    }
}

/// A borrowed event sidecar listener paired with a stable [`WaitToken`].
///
/// No payload subscriber is stored in this type and no payload operation is
/// performed by the wait layer.
pub struct EventSource<'listener> {
    token: WaitToken,
    listener: &'listener Listener<ipc_threadsafe::Service>,
}

impl fmt::Debug for EventSource<'_> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("EventSource")
            .field("token", &self.token)
            .finish_non_exhaustive()
    }
}

impl<'listener> EventSource<'listener> {
    /// Pairs a stable token with an existing stream readiness listener.
    #[doc(hidden)]
    pub fn new(token: WaitToken, listener: &'listener Listener<ipc_threadsafe::Service>) -> Self {
        Self { token, listener }
    }

    /// Returns the stable source token.
    pub const fn token(&self) -> WaitToken {
        self.token
    }

    /// Returns the underlying readiness listener.
    #[doc(hidden)]
    pub(crate) const fn listener(&self) -> &Listener<ipc_threadsafe::Service> {
        self.listener
    }
}

/// Drains one bounded native event activation batch and reports whether any
/// activation carried a positive notification count.
pub(crate) fn consume_sidecar_activations(
    listener: &Listener<ipc_threadsafe::Service>,
) -> Result<bool, ListenerWaitError> {
    let mut activated = false;
    listener.try_wait(|activation| {
        activated |= activation.count > 0;
    })?;
    Ok(activated)
}

/// Progress reported by one source handler dispatch.
///
/// `Samples` and `Empty` continue waiting.  The variants carry only local
/// accounting information; they do not transport or retain payloads.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub enum WaitProgress {
    /// Continue waiting after this dispatch without additional accounting.
    #[default]
    Continue,
    /// Stop the wait loop normally.
    Stop,
    /// Stop the wait loop after accounting for the given number of payload
    /// samples processed by this dispatch.
    StopAfterSamples(usize),
    /// Continue after processing the given number of payload samples.
    Samples(usize),
    /// Continue after a source wake found no payload sample.
    Empty,
    /// Continue after processing samples and observing empty receive attempts.
    SamplesAndEmpty {
        /// Number of samples processed.
        samples: usize,
        /// Number of empty receive attempts.
        empty_wakeups: usize,
    },
}

impl WaitProgress {
    /// Returns progress for a continuing handler.
    pub const fn continue_waiting() -> Self {
        Self::Continue
    }

    /// Returns progress that requests a normal stop.
    pub const fn stop() -> Self {
        Self::Stop
    }

    /// Returns progress that accounts for samples and then stops normally.
    pub const fn stop_after_samples(count: usize) -> Self {
        Self::StopAfterSamples(count)
    }

    /// Returns progress for processed samples.
    pub const fn samples(count: usize) -> Self {
        Self::Samples(count)
    }

    /// Returns progress for an empty/spurious wake.
    pub const fn empty() -> Self {
        Self::Empty
    }

    /// Returns the number of payload samples represented by this progress.
    pub const fn samples_processed(self) -> usize {
        match self {
            Self::Samples(count)
            | Self::StopAfterSamples(count)
            | Self::SamplesAndEmpty { samples: count, .. } => count,
            Self::Continue | Self::Stop | Self::Empty => 0,
        }
    }

    /// Returns the number of empty receive attempts represented by this
    /// progress.
    pub const fn empty_wakeups(self) -> usize {
        match self {
            Self::Empty => 1,
            Self::SamplesAndEmpty { empty_wakeups, .. } => empty_wakeups,
            Self::Continue | Self::Stop | Self::StopAfterSamples(_) | Self::Samples(_) => 0,
        }
    }

    /// Returns whether the handler requested a normal stop.
    pub const fn should_stop(self) -> bool {
        matches!(self, Self::Stop | Self::StopAfterSamples(_))
    }
}

impl From<()> for WaitProgress {
    fn from(_: ()) -> Self {
        Self::Continue
    }
}

impl From<usize> for WaitProgress {
    fn from(value: usize) -> Self {
        Self::Samples(value)
    }
}

/// Why a blocking wait loop exited.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum WaitExit {
    /// The activation cancellation event was observed.
    Cancelled,
    /// The handler requested a normal stop.
    Stopped,
}

impl WaitExit {
    /// Returns true if cancellation ended the loop.
    pub const fn is_cancelled(self) -> bool {
        matches!(self, Self::Cancelled)
    }
}

/// A point-in-time copy of [`WaitStats`] counters.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct WaitStatsSnapshot {
    /// Number of blocking wait calls entered.
    pub waits: u64,
    /// Number of source/cancellation wake dispatches observed.
    pub wakeups: u64,
    /// Number of source wake dispatches that produced no payload sample.
    pub empty_wakeups: u64,
    /// Number of cancellation wakeups observed.
    pub cancellation_wakeups: u64,
    /// Number of payload samples reported by handlers.
    pub samples_processed: u64,
}

impl WaitStatsSnapshot {
    /// Alias for [`Self::cancellation_wakeups`].
    pub const fn cancellation_wakes(self) -> u64 {
        self.cancellation_wakeups
    }

    /// Alias for [`Self::samples_processed`].
    pub const fn samples(self) -> u64 {
        self.samples_processed
    }
}

#[derive(Debug, Default)]
struct WaitStatsInner {
    waits: AtomicU64,
    wakeups: AtomicU64,
    empty_wakeups: AtomicU64,
    cancellation_wakeups: AtomicU64,
    samples_processed: AtomicU64,
}

/// Relaxed local counters for one [`WaitContext`].
#[derive(Clone, Debug, Default)]
pub struct WaitStats {
    inner: Arc<WaitStatsInner>,
}

impl WaitStats {
    fn increment_waits(&self) {
        self.inner.waits.fetch_add(1, Ordering::Relaxed);
    }

    fn increment_wakeups(&self) {
        self.inner.wakeups.fetch_add(1, Ordering::Relaxed);
    }

    fn increment_empty_wakeups(&self, count: usize) {
        self.inner
            .empty_wakeups
            .fetch_add(count as u64, Ordering::Relaxed);
    }

    fn increment_cancellation_wakeups(&self) {
        self.inner
            .cancellation_wakeups
            .fetch_add(1, Ordering::Relaxed);
    }

    fn increment_samples(&self, count: usize) {
        self.inner
            .samples_processed
            .fetch_add(count as u64, Ordering::Relaxed);
    }

    /// Returns an atomic point-in-time snapshot.
    pub fn snapshot(&self) -> WaitStatsSnapshot {
        WaitStatsSnapshot {
            waits: self.inner.waits.load(Ordering::Relaxed),
            wakeups: self.inner.wakeups.load(Ordering::Relaxed),
            empty_wakeups: self.inner.empty_wakeups.load(Ordering::Relaxed),
            cancellation_wakeups: self.inner.cancellation_wakeups.load(Ordering::Relaxed),
            samples_processed: self.inner.samples_processed.load(Ordering::Relaxed),
        }
    }

    /// Returns the number of blocking waits entered.
    pub fn waits(&self) -> u64 {
        self.snapshot().waits
    }

    /// Returns the number of wake dispatches observed.
    pub fn wakeups(&self) -> u64 {
        self.snapshot().wakeups
    }

    /// Returns the number of source wake dispatches that produced no sample.
    pub fn empty_wakeups(&self) -> u64 {
        self.snapshot().empty_wakeups
    }

    /// Returns the number of cancellation wakeups observed.
    pub fn cancellation_wakeups(&self) -> u64 {
        self.snapshot().cancellation_wakeups
    }

    /// Alias for [`Self::cancellation_wakeups`].
    pub fn cancellation_wakes(&self) -> u64 {
        self.cancellation_wakeups()
    }

    /// Returns the number of payload samples reported by handlers.
    pub fn samples_processed(&self) -> u64 {
        self.snapshot().samples_processed
    }

    /// Alias for [`Self::samples_processed`].
    pub fn samples(&self) -> u64 {
        self.samples_processed()
    }
}

/// Typed failures produced by the experimental wait abstraction.
#[derive(Debug, Error)]
pub enum WaitError {
    /// The wait context's cancellation node could not be created.
    #[error("failed to create wait node: {0}")]
    Node(#[from] NodeCreationFailure),
    /// The generated cancellation service name is invalid.
    #[error("invalid wait service name: {0}")]
    ServiceName(#[from] ServiceNameError),
    /// The cancellation event service could not be opened or created.
    #[error("failed to open or create cancellation event: {0}")]
    EventOpenOrCreate(#[from] EventOpenOrCreateError),
    /// The cancellation event service could not be opened.
    #[error("failed to open cancellation event: {0}")]
    EventOpen(#[from] EventOpenError),
    /// The cancellation listener could not be created.
    #[error("failed to create cancellation listener: {0}")]
    Listener(#[from] ListenerCreateError),
    /// The cancellation notifier could not be created.
    #[error("failed to create cancellation notifier: {0}")]
    Notifier(#[from] NotifierCreateError),
    /// A listener event could not be consumed.
    #[error("failed to consume readiness event: {0}")]
    ListenerWait(String),
    /// The wait set could not be created.
    #[error("failed to create wait set: {0}")]
    WaitSetCreate(#[from] WaitSetCreateError),
    /// A source could not be attached to the wait set.
    #[error("failed to attach wait source: {0}")]
    WaitSetAttach(#[from] WaitSetAttachmentError),
    /// The wait set failed while blocking.
    #[error("wait set failed: {0}")]
    WaitSetRun(#[from] WaitSetRunError),
    /// Two source registrations used the same stable token.
    #[error("wait token {0} is already registered")]
    DuplicateToken(WaitToken),
    /// A wake was requested but the cancellation event could not be notified.
    #[error("failed to notify cancellation event: {0}")]
    Notify(#[from] NotifierNotifyError),
}

struct WaitWakeState {
    notifier: Arc<Notifier<ipc_threadsafe::Service>>,
    cancelled: AtomicBool,
    // A wake handle may intentionally outlive the blocking WaitContext while
    // EffectScope finishes joining it. Keep the owning node alive with every
    // notifier clone so the final drop order is notifier, then node.
    _node: Node<ipc_threadsafe::Service>,
}

/// A clonable activation cancellation handle.
#[derive(Clone)]
pub struct WaitWakeHandle {
    state: Arc<WaitWakeState>,
}

impl fmt::Debug for WaitWakeHandle {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("WaitWakeHandle")
            .field("cancelled", &self.is_cancelled())
            .finish()
    }
}

impl WaitWakeHandle {
    /// Notifies the activation cancellation event.
    ///
    /// The flag is set before the IPC notification so a wake that races with
    /// listener attachment is still observed by the context's pre-wait check.
    pub fn wake(&self) -> Result<(), WaitError> {
        self.state.cancelled.store(true, Ordering::Release);
        self.state.notifier.notify()?;
        Ok(())
    }

    /// Alias for [`Self::wake`].
    pub fn cancel(&self) -> Result<(), WaitError> {
        self.wake()
    }

    /// Returns whether this handle has requested cancellation.
    pub fn is_cancelled(&self) -> bool {
        self.state.cancelled.load(Ordering::Acquire)
    }
}

/// A blocking wait context for one activation.
///
/// The context owns only its cancellation event endpoints.  Stream
/// [`EventSource`] values borrow listener endpoints from their subscribers and
/// are supplied to each call to [`Self::run`].
pub struct WaitContext {
    process_instance: ProcessInstanceId,
    activation: ActivationId,
    cancellation_listener: Listener<ipc_threadsafe::Service>,
    wake_state: Arc<WaitWakeState>,
    stats: WaitStats,
}

impl fmt::Debug for WaitContext {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("WaitContext")
            .field("process_instance", &self.process_instance)
            .field("activation", &self.activation)
            .field("stats", &self.stats.snapshot())
            .finish_non_exhaustive()
    }
}

impl WaitContext {
    /// Creates a context whose cancellation service is keyed by both process
    /// instance and activation identity.
    pub fn new(
        process_instance: ProcessInstanceId,
        activation: ActivationId,
    ) -> Result<Self, WaitError> {
        let node = NodeBuilder::new()
            .signal_handling_mode(SignalHandlingMode::Disabled)
            .create::<ipc_threadsafe::Service>()?;
        let service_name = cancellation_service_name(process_instance, activation)?;
        let event = node
            .service_builder(&service_name)
            .event()
            .open_or_create()?;
        let cancellation_listener = event.listener_builder().create()?;
        let notifier = Arc::new(event.notifier_builder().create()?);
        let wake_state = Arc::new(WaitWakeState {
            notifier,
            cancelled: AtomicBool::new(false),
            _node: node,
        });

        Ok(Self {
            process_instance,
            activation,
            cancellation_listener,
            wake_state,
            stats: WaitStats::default(),
        })
    }

    /// Returns the process identity used for cancellation isolation.
    pub const fn process_instance(&self) -> ProcessInstanceId {
        self.process_instance
    }

    /// Returns the activation identity used for cancellation isolation.
    pub const fn activation(&self) -> ActivationId {
        self.activation
    }

    /// Returns the reserved cancellation token.
    pub const fn cancellation_token(&self) -> WaitToken {
        WaitToken::CANCELLATION
    }

    /// Returns a clonable handle that wakes and cancels this context.
    pub fn wake_handle(&self) -> WaitWakeHandle {
        WaitWakeHandle {
            state: Arc::clone(&self.wake_state),
        }
    }

    /// Alias for [`Self::wake_handle`].
    pub fn wait_wake_handle(&self) -> WaitWakeHandle {
        self.wake_handle()
    }

    /// Returns a clonable stats handle for this context.
    pub fn stats(&self) -> WaitStats {
        self.stats.clone()
    }

    /// Returns whether cancellation has already been requested.
    pub fn is_cancelled(&self) -> bool {
        self.wake_state.cancelled.load(Ordering::Acquire)
    }

    /// Requests cancellation from the current thread.
    pub fn cancel(&self) -> Result<(), WaitError> {
        self.wake_handle().wake()
    }

    /// Runs the blocking source dispatch loop.
    ///
    /// The handler receives one `ReadyEvent` at a time and the private drain
    /// budget (`64`) for that source.  It should call the corresponding
    /// subscriber's ordinary `receive()` method at most that many times and
    /// return [`WaitProgress::Samples`] or [`WaitProgress::Empty`] so local
    /// statistics remain useful.  The generic error conversion lets a caller
    /// preserve its existing stream/application error composition.
    pub fn run<'source, E, F, P>(
        &mut self,
        sources: &[EventSource<'source>],
        mut handler: F,
    ) -> Result<WaitExit, E>
    where
        E: From<WaitError>,
        F: FnMut(ReadyEvent, usize) -> Result<P, E>,
        P: Into<WaitProgress>,
    {
        self.validate_sources(sources)?;
        let exit = self.run_attached(sources, &mut handler, None)?;
        Ok(exit.expect("an unbounded WaitContext run cannot time out"))
    }

    /// Runs the source dispatch loop until it exits or `timeout` elapses.
    ///
    /// `Some` carries the same cancellation or handler-requested exit as
    /// [`Self::run`]. `None` means the deadline elapsed. The deadline is
    /// enforced by the native WaitSet timeout, so callers do not need a timer
    /// thread or a synthetic cancellation event to bound the wait.
    pub fn run_with_timeout<'source, E, F, P>(
        &mut self,
        sources: &[EventSource<'source>],
        timeout: Duration,
        mut handler: F,
    ) -> Result<Option<WaitExit>, E>
    where
        E: From<WaitError>,
        F: FnMut(ReadyEvent, usize) -> Result<P, E>,
        P: Into<WaitProgress>,
    {
        self.validate_sources(sources)?;
        self.run_attached(sources, &mut handler, Some(timeout))
    }

    fn validate_sources<E>(&mut self, sources: &[EventSource<'_>]) -> Result<(), E>
    where
        E: From<WaitError>,
    {
        for (index, source) in sources.iter().enumerate() {
            if source.token() == WaitToken::CANCELLATION
                || sources[..index]
                    .iter()
                    .any(|existing| existing.token() == source.token())
            {
                return Err(E::from(WaitError::DuplicateToken(source.token())));
            }
        }
        Ok(())
    }

    fn run_attached<'source, E, F, P>(
        &mut self,
        sources: &[EventSource<'source>],
        handler: &mut F,
        timeout: Option<Duration>,
    ) -> Result<Option<WaitExit>, E>
    where
        E: From<WaitError>,
        F: FnMut(ReadyEvent, usize) -> Result<P, E>,
        P: Into<WaitProgress>,
    {
        if self.is_cancelled() {
            self.stats.increment_cancellation_wakeups();
            return Ok(Some(WaitExit::Cancelled));
        }

        let wait_started = Instant::now();
        let waitset = WaitSetBuilder::new()
            .signal_handling_mode(SignalHandlingMode::Disabled)
            .create::<ipc_threadsafe::Service>()
            .map_err(WaitError::from)
            .map_err(E::from)?;

        let mut guards = Vec::with_capacity(sources.len());
        for source in sources {
            let guard = waitset
                .attach_notification(source.listener())
                .map_err(WaitError::from)
                .map_err(E::from)?;
            guards.push(guard);
        }
        let cancellation_guard = waitset
            .attach_notification(&self.cancellation_listener)
            .map_err(WaitError::from)
            .map_err(E::from)?;
        let cancellation_liveness_guard = waitset
            .attach_interval(CANCELLATION_LIVENESS_INTERVAL)
            .map_err(WaitError::from)
            .map_err(E::from)?;

        // The initial pass is deliberately synthetic.  It closes the race in
        // which a payload was queued just before the sidecar listener became
        // attached, while still leaving all payload borrowing to the handler.
        for source in sources {
            let progress = handler(
                ReadyEvent::synthetic_source(source.token()),
                MAX_DRAIN_PER_SOURCE,
            )?;
            let progress = progress.into();
            // Synthetic probes are construction-race protection, not
            // readiness dispatches. They do not contribute wake/empty
            // counters, but a handler that actually drained payloads still
            // contributes to the total sample count.
            self.stats.increment_samples(progress.samples_processed());
            if progress.should_stop() {
                return Ok(Some(WaitExit::Stopped));
            }
            if self.is_cancelled() {
                self.stats.increment_cancellation_wakeups();
                return Ok(Some(WaitExit::Cancelled));
            }
        }

        loop {
            if self.is_cancelled() {
                self.stats.increment_cancellation_wakeups();
                return Ok(Some(WaitExit::Cancelled));
            }
            let remaining = timeout.map(|timeout| timeout.saturating_sub(wait_started.elapsed()));
            if remaining == Some(Duration::ZERO) {
                return Ok(None);
            }
            self.stats.increment_waits();

            let mut callback_error: Option<E> = None;
            let mut callback_exit = None;
            let stats = self.stats.clone();
            let cancelled = Arc::clone(&self.wake_state);
            let cancellation_listener = &self.cancellation_listener;

            let mut process_attachment =
                |attachment_id: WaitSetAttachmentId<ipc_threadsafe::Service>| {
                    stats.increment_wakeups();

                    // The shared flag is authoritative. Normally its event
                    // reaches the cancellation listener immediately. The
                    // native interval makes a failed notify bounded as well,
                    // without touching any payload subscriber.
                    if cancelled.cancelled.load(Ordering::Acquire) {
                        stats.increment_cancellation_wakeups();
                        callback_exit = Some(WaitExit::Cancelled);
                        return CallbackProgression::Stop;
                    }

                    if attachment_id.has_event_from(&cancellation_guard) {
                        // Drain the bounded native event activations. This is
                        // an event-sidecar operation, not a payload receive
                        // or borrow.
                        match consume_sidecar_activations(cancellation_listener) {
                            Ok(true) => {
                                stats.increment_cancellation_wakeups();
                                cancelled.cancelled.store(true, Ordering::Release);
                                callback_exit = Some(WaitExit::Cancelled);
                                CallbackProgression::Stop
                            }
                            Ok(false) => {
                                // Treat a readiness edge without a queued
                                // cancellation event like every other
                                // spurious source wake.  Only an explicit
                                // notifier event (or the pre-wait flag check)
                                // may cancel this activation.
                                stats.increment_empty_wakeups(1);
                                CallbackProgression::Continue
                            }
                            Err(error) => {
                                callback_error =
                                    Some(E::from(WaitError::ListenerWait(error.to_string())));
                                CallbackProgression::Stop
                            }
                        }
                    } else if attachment_id.has_event_from(&cancellation_liveness_guard) {
                        CallbackProgression::Continue
                    } else {
                        let source_index = guards
                            .iter()
                            .position(|guard| attachment_id.has_event_from(guard));
                        let Some(source_index) = source_index else {
                            // A stale/transient attachment id is harmless; in
                            // particular, never dereference an endpoint based
                            // on that implementation identity.
                            return CallbackProgression::Continue;
                        };
                        let source = &sources[source_index];

                        // One bounded native event drain may consume coalesced
                        // activations. The sidecar is advisory; the handler
                        // owns all payload drain policy.
                        let listener_empty = match consume_sidecar_activations(source.listener()) {
                            Ok(activated) => !activated,
                            Err(error) => {
                                callback_error =
                                    Some(E::from(WaitError::ListenerWait(error.to_string())));
                                return CallbackProgression::Stop;
                            }
                        };

                        match handler(ReadyEvent::source(source.token()), MAX_DRAIN_PER_SOURCE) {
                            Ok(progress) => {
                                let progress = progress.into();
                                stats.increment_samples(progress.samples_processed());
                                // Count wake-without-sample dispatches, not the
                                // ordinary final `receive() -> None` that ends
                                // a productive drain. A spurious listener edge
                                // and an empty handler result still describe
                                // the same single source dispatch.
                                let empty_wakeups = if progress.samples_processed() == 0 {
                                    progress
                                        .empty_wakeups()
                                        .max(if listener_empty { 1 } else { 0 })
                                } else {
                                    0
                                };
                                stats.increment_empty_wakeups(empty_wakeups);
                                if progress.should_stop() {
                                    callback_exit = Some(WaitExit::Stopped);
                                    CallbackProgression::Stop
                                } else {
                                    CallbackProgression::Continue
                                }
                            }
                            Err(error) => {
                                callback_error = Some(error);
                                CallbackProgression::Stop
                            }
                        }
                    }
                };
            let wait_result = match remaining {
                Some(remaining) => {
                    waitset.wait_and_process_once_with_timeout(&mut process_attachment, remaining)
                }
                None => waitset.wait_and_process_once(&mut process_attachment),
            }
            .map_err(WaitError::from)
            .map_err(E::from);

            if let Some(error) = callback_error {
                return Err(error);
            }
            wait_result?;
            if let Some(exit) = callback_exit {
                return Ok(Some(exit));
            }
            if timeout.is_some_and(|timeout| wait_started.elapsed() >= timeout) {
                return Ok(None);
            }
        }
    }
}

fn cancellation_service_name(
    process_instance: ProcessInstanceId,
    activation: ActivationId,
) -> Result<ServiceName, WaitError> {
    let name = format!(
        "{CANCELLATION_SERVICE_PREFIX}{process_instance}/{}",
        activation.value()
    );
    Ok(name.as_str().try_into()?)
}

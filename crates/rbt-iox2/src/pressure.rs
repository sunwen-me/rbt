use std::{
    sync::{
        Arc, Mutex, OnceLock,
        atomic::{AtomicU8, AtomicU64, Ordering},
    },
    time::{Instant, SystemTime, UNIX_EPOCH},
};

use rbt_core::{
    PressureEvent, PressureSnapshotSource, PublisherPressure, StreamPressure, StreamResourceConfig,
    StreamSemanticConfig, SubscriberPressure, register_pressure_source,
};
use rbt_types::StreamProfile;
use tracing::{info, warn};

const LOAN_PRESSURE: u8 = 1;
const SEND_PRESSURE: u8 = 2;
const BORROW_PRESSURE: u8 = 4;

/// Relaxed atomics are sufficient here because these values are diagnostic
/// telemetry. Counters use CAS loops instead of fetch arithmetic so
/// cumulative totals saturate and current gauges cannot wrap or underflow.
fn saturating_add(atom: &AtomicU64, amount: u64) -> u64 {
    let mut current = atom.load(Ordering::Relaxed);
    loop {
        let next = current.saturating_add(amount);
        match atom.compare_exchange_weak(current, next, Ordering::Relaxed, Ordering::Relaxed) {
            Ok(_) => return next,
            Err(observed) => current = observed,
        }
    }
}

fn saturating_sub(atom: &AtomicU64, amount: u64) -> u64 {
    let mut current = atom.load(Ordering::Relaxed);
    loop {
        let next = current.saturating_sub(amount);
        match atom.compare_exchange_weak(current, next, Ordering::Relaxed, Ordering::Relaxed) {
            Ok(_) => return next,
            Err(observed) => current = observed,
        }
    }
}

fn saturating_max(atom: &AtomicU64, candidate: u64) -> u64 {
    let mut current = atom.load(Ordering::Relaxed);
    loop {
        if candidate <= current {
            return current;
        }
        match atom.compare_exchange_weak(current, candidate, Ordering::Relaxed, Ordering::Relaxed) {
            Ok(_) => return candidate,
            Err(observed) => current = observed,
        }
    }
}

fn monotonic_origin() -> &'static Instant {
    static ORIGIN: OnceLock<Instant> = OnceLock::new();
    ORIGIN.get_or_init(Instant::now)
}

fn monotonic_ns() -> u64 {
    monotonic_origin()
        .elapsed()
        .as_nanos()
        .try_into()
        .unwrap_or(u64::MAX)
        .saturating_add(1)
        .min(BORROW_TIMESTAMP_MASK)
}

fn unix_timestamp_ms() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_millis().try_into().unwrap_or(u64::MAX))
        .unwrap_or(0)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum EndpointKind {
    Publisher,
    Subscriber,
}

/// One preallocated subscriber-borrow reservation.
///
/// The high bit marks a reservation which successfully received a sample and
/// is now an active or releasing `BorrowTracker`. Keeping reservation,
/// active, and releasing states in one atomic word makes cancellation and
/// slot reuse race-free.
struct BorrowSlot {
    state: AtomicU64,
}

impl BorrowSlot {
    fn new() -> Self {
        Self {
            state: AtomicU64::new(0),
        }
    }
}

const BORROW_COMMITTED: u64 = 1 << 63;
const BORROW_RELEASING: u64 = 1 << 62;
const BORROW_TIMESTAMP_MASK: u64 = !(BORROW_COMMITTED | BORROW_RELEASING);

pub(crate) struct PressureState {
    service_name: String,
    profile: StreamProfile,
    semantic: StreamSemanticConfig,
    resources: StreamResourceConfig,
    endpoint: EndpointKind,

    loan_attempts: AtomicU64,
    loan_successes: AtomicU64,
    loans_in_use: AtomicU64,
    loans_high_water: AtomicU64,
    loan_failures: AtomicU64,
    payload_bytes_last: AtomicU64,
    payload_bytes_max_seen: AtomicU64,
    total_loaned: AtomicU64,

    send_attempts: AtomicU64,
    send_successes: AtomicU64,
    send_failures: AtomicU64,
    queue_full: AtomicU64,
    pending_queue_full: AtomicU64,
    unable_to_deliver: AtomicU64,
    readiness_notify_failures: AtomicU64,
    subscriber_count: Option<Box<dyn Fn() -> usize + Send + Sync>>,

    receive_attempts: AtomicU64,
    received_samples: AtomicU64,
    borrowed_samples: AtomicU64,
    borrowed_high_water: AtomicU64,
    borrow_failures: AtomicU64,
    borrow_duration_count: AtomicU64,
    borrow_duration_total_ns: AtomicU64,
    borrow_duration_max_ns: AtomicU64,
    borrow_slots: Box<[BorrowSlot]>,

    pressure_flags: AtomicU8,
    last_pressure_event: Mutex<Option<PressureEvent>>,
}

impl PressureState {
    pub(crate) fn publisher(
        service_name: impl Into<String>,
        profile: StreamProfile,
        semantic: StreamSemanticConfig,
        resources: StreamResourceConfig,
        subscriber_count: impl Fn() -> usize + Send + Sync + 'static,
    ) -> Arc<Self> {
        let mut state = Self::new(
            service_name.into(),
            profile,
            semantic,
            resources,
            EndpointKind::Publisher,
        );
        state.subscriber_count = Some(Box::new(subscriber_count));
        Self::register(state)
    }

    pub(crate) fn subscriber(
        service_name: impl Into<String>,
        profile: StreamProfile,
        semantic: StreamSemanticConfig,
        resources: StreamResourceConfig,
    ) -> Arc<Self> {
        Self::register(Self::new(
            service_name.into(),
            profile,
            semantic,
            resources,
            EndpointKind::Subscriber,
        ))
    }

    fn new(
        service_name: String,
        profile: StreamProfile,
        semantic: StreamSemanticConfig,
        resources: StreamResourceConfig,
        endpoint: EndpointKind,
    ) -> Self {
        let slot_count = resources.max_borrowed;
        Self {
            service_name,
            profile,
            semantic,
            resources,
            endpoint,
            loan_attempts: AtomicU64::new(0),
            loan_successes: AtomicU64::new(0),
            loans_in_use: AtomicU64::new(0),
            loans_high_water: AtomicU64::new(0),
            loan_failures: AtomicU64::new(0),
            payload_bytes_last: AtomicU64::new(0),
            payload_bytes_max_seen: AtomicU64::new(0),
            total_loaned: AtomicU64::new(0),
            send_attempts: AtomicU64::new(0),
            send_successes: AtomicU64::new(0),
            send_failures: AtomicU64::new(0),
            queue_full: AtomicU64::new(0),
            pending_queue_full: AtomicU64::new(0),
            unable_to_deliver: AtomicU64::new(0),
            readiness_notify_failures: AtomicU64::new(0),
            subscriber_count: None,
            receive_attempts: AtomicU64::new(0),
            received_samples: AtomicU64::new(0),
            borrowed_samples: AtomicU64::new(0),
            borrowed_high_water: AtomicU64::new(0),
            borrow_failures: AtomicU64::new(0),
            borrow_duration_count: AtomicU64::new(0),
            borrow_duration_total_ns: AtomicU64::new(0),
            borrow_duration_max_ns: AtomicU64::new(0),
            borrow_slots: (0..slot_count)
                .map(|_| BorrowSlot::new())
                .collect::<Vec<_>>()
                .into_boxed_slice(),
            pressure_flags: AtomicU8::new(0),
            last_pressure_event: Mutex::new(None),
        }
    }

    fn register(state: Self) -> Arc<Self> {
        let state = Arc::new(state);
        register_pressure_source(&state);
        state
    }

    pub(crate) fn loan_attempted(&self) {
        saturating_add(&self.loan_attempts, 1);
    }

    pub(crate) fn loan_succeeded_arc(state: &Arc<Self>, payload_bytes: u64) -> LoanTracker {
        state.recovered(LOAN_PRESSURE, "Loan Pool");
        saturating_add(&state.loan_successes, 1);
        let in_use = saturating_add(&state.loans_in_use, 1);
        saturating_max(&state.loans_high_water, in_use);
        state
            .payload_bytes_last
            .store(payload_bytes, Ordering::Relaxed);
        saturating_max(&state.payload_bytes_max_seen, payload_bytes);
        saturating_add(&state.total_loaned, payload_bytes);
        LoanTracker {
            state: Arc::clone(state),
            active: true,
        }
    }

    pub(crate) fn loan_failed(&self) {
        saturating_add(&self.loan_failures, 1);
        self.pressured(LOAN_PRESSURE, PressureEvent::LoanPoolExhausted);
    }

    pub(crate) fn send_attempted(&self) {
        saturating_add(&self.send_attempts, 1);
    }

    pub(crate) fn send_succeeded(&self) {
        saturating_add(&self.send_successes, 1);
        self.recovered(SEND_PRESSURE, "send");
    }

    pub(crate) fn send_failed(&self, event: PressureEvent) {
        saturating_add(&self.send_failures, 1);
        match &event {
            PressureEvent::QueueFull { .. } => {
                let mut pending = self.pending_queue_full.load(Ordering::Relaxed);
                loop {
                    if pending == 0 {
                        saturating_add(&self.queue_full, 1);
                        break;
                    }
                    match self.pending_queue_full.compare_exchange_weak(
                        pending,
                        pending - 1,
                        Ordering::Relaxed,
                        Ordering::Relaxed,
                    ) {
                        Ok(_) => break,
                        Err(observed) => pending = observed,
                    }
                }
            }
            PressureEvent::UnableToDeliver => {
                saturating_add(&self.unable_to_deliver, 1);
            }
            _ => {}
        }
        self.record_pressure_event(event);
        self.mark_pressure(SEND_PRESSURE);
    }

    pub(crate) fn queue_full(&self, subscriber: Option<String>) {
        saturating_add(&self.queue_full, 1);
        saturating_add(&self.pending_queue_full, 1);
        self.pressured(SEND_PRESSURE, PressureEvent::QueueFull { subscriber });
    }

    pub(crate) fn readiness_notify_failed(&self) {
        saturating_add(&self.readiness_notify_failures, 1);
    }

    /// Reserves one bounded local borrow slot before calling native receive.
    pub(crate) fn reserve_borrow(self: &Arc<Self>) -> Option<BorrowReservation> {
        let start_ns = monotonic_ns();
        for (index, slot) in self.borrow_slots.iter().enumerate() {
            if slot
                .state
                .compare_exchange(0, start_ns, Ordering::Acquire, Ordering::Relaxed)
                .is_ok()
            {
                return Some(BorrowReservation {
                    state: Some(Arc::clone(self)),
                    slot: index,
                    start_ns,
                });
            }
        }
        None
    }

    pub(crate) fn receive_attempted(&self) {
        saturating_add(&self.receive_attempts, 1);
    }

    pub(crate) fn borrow_failed(&self) {
        saturating_add(&self.borrow_failures, 1);
        self.pressured(BORROW_PRESSURE, PressureEvent::BorrowLimitReached);
    }

    fn borrow_succeeded(&self) {
        self.recovered(BORROW_PRESSURE, "Borrow");
        saturating_add(&self.received_samples, 1);
        let borrowed = saturating_add(&self.borrowed_samples, 1);
        saturating_max(&self.borrowed_high_water, borrowed);
    }

    fn release_reservation(&self, slot: usize, start_ns: u64) {
        let Some(slot) = self.borrow_slots.get(slot) else {
            return;
        };
        if slot
            .state
            .compare_exchange(start_ns, 0, Ordering::Relaxed, Ordering::Relaxed)
            .is_ok()
        {
            // The receive did not produce a sample, so no Borrow lifetime
            // exists to include in the duration counters.
        }
    }

    fn commit_borrow(&self, slot: usize, start_ns: u64) {
        let Some(slot) = self.borrow_slots.get(slot) else {
            return;
        };
        let committed = slot.state.compare_exchange(
            start_ns,
            start_ns | BORROW_COMMITTED,
            Ordering::Relaxed,
            Ordering::Relaxed,
        );
        debug_assert_eq!(
            committed.ok(),
            Some(start_ns),
            "borrow reservation was released before commit"
        );
    }

    fn release_borrow(&self, slot: usize, start_ns: u64) {
        let Some(slot) = self.borrow_slots.get(slot) else {
            return;
        };
        if slot
            .state
            .compare_exchange(
                start_ns | BORROW_COMMITTED,
                start_ns | BORROW_COMMITTED | BORROW_RELEASING,
                Ordering::Relaxed,
                Ordering::Relaxed,
            )
            .is_ok()
        {
            // Keep the slot nonzero while the gauges and duration totals are
            // updated. This prevents a new reservation from incrementing
            // `borrowed_samples` before the released Borrow is accounted.
            saturating_sub(&self.borrowed_samples, 1);
            let duration = monotonic_ns().saturating_sub(start_ns);
            saturating_add(&self.borrow_duration_count, 1);
            saturating_add(&self.borrow_duration_total_ns, duration);
            saturating_max(&self.borrow_duration_max_ns, duration);
            slot.state.store(0, Ordering::Release);
        }
    }

    fn pressured(&self, flag: u8, event: PressureEvent) {
        self.record_pressure_event(event.clone());
        if self.mark_pressure(flag) {
            warn!(
                service_name = %self.service_name,
                profile = ?self.profile,
                pressure_event = ?event,
                "stream entered pressure"
            );
        }
    }

    fn record_pressure_event(&self, event: PressureEvent) {
        let mut last = self
            .last_pressure_event
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner);
        let preserve_subscriber = matches!(
            (&event, &*last),
            (
                PressureEvent::QueueFull { subscriber: None },
                Some(PressureEvent::QueueFull {
                    subscriber: Some(_)
                })
            )
        );
        if !preserve_subscriber {
            *last = Some(event);
        }
    }

    fn mark_pressure(&self, flag: u8) -> bool {
        self.pressure_flags.fetch_or(flag, Ordering::Relaxed) & flag == 0
    }

    fn recovered(&self, flag: u8, kind: &'static str) {
        if self.pressure_flags.fetch_and(!flag, Ordering::Relaxed) & flag != 0 {
            info!(
                service_name = %self.service_name,
                profile = ?self.profile,
                pressure_kind = kind,
                "stream recovered from pressure"
            );
        }
    }

    pub(crate) fn snapshot(&self) -> StreamPressure {
        let last_pressure_event = self
            .last_pressure_event
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner)
            .clone();
        let (publisher, subscriber) = match self.endpoint {
            EndpointKind::Publisher => (
                Some(PublisherPressure {
                    loan_attempts: self.loan_attempts.load(Ordering::Relaxed),
                    loan_successes: self.loan_successes.load(Ordering::Relaxed),
                    loans_in_use: self.loans_in_use.load(Ordering::Relaxed),
                    loans_high_water: self.loans_high_water.load(Ordering::Relaxed),
                    loan_failures: self.loan_failures.load(Ordering::Relaxed),
                    payload_bytes_last: self.payload_bytes_last.load(Ordering::Relaxed),
                    payload_bytes_max_seen: self.payload_bytes_max_seen.load(Ordering::Relaxed),
                    total_loaned: self.total_loaned.load(Ordering::Relaxed),
                    send_attempts: self.send_attempts.load(Ordering::Relaxed),
                    send_successes: self.send_successes.load(Ordering::Relaxed),
                    send_failures: self.send_failures.load(Ordering::Relaxed),
                    queue_full: self.queue_full.load(Ordering::Relaxed),
                    unable_to_deliver: self.unable_to_deliver.load(Ordering::Relaxed),
                    readiness_notify_failures: self
                        .readiness_notify_failures
                        .load(Ordering::Relaxed),
                    subscriber_count: self
                        .subscriber_count
                        .as_ref()
                        .map_or(0, |read| u32::try_from(read()).unwrap_or(u32::MAX)),
                    estimated_dropped_samples: None,
                }),
                None,
            ),
            EndpointKind::Subscriber => (
                None,
                Some(SubscriberPressure {
                    receive_attempts: self.receive_attempts.load(Ordering::Relaxed),
                    received_samples: self.received_samples.load(Ordering::Relaxed),
                    borrowed_samples: self.borrowed_samples.load(Ordering::Relaxed),
                    borrowed_high_water: self.borrowed_high_water.load(Ordering::Relaxed),
                    borrow_failures: self.borrow_failures.load(Ordering::Relaxed),
                    borrow_duration_count: self.borrow_duration_count.load(Ordering::Relaxed),
                    borrow_duration_total_ns: self.borrow_duration_total_ns.load(Ordering::Relaxed),
                    borrow_duration_max_ns: self.borrow_duration_max_ns.load(Ordering::Relaxed),
                    oldest_borrow_age_ns: self.oldest_borrow_age_ns(),
                    estimated_dropped_samples: None,
                }),
            ),
        };
        StreamPressure {
            service_name: self.service_name.clone(),
            profile: self.profile,
            semantic: self.semantic,
            resources: self.resources,
            publisher,
            subscriber,
            last_pressure_event,
            snapshot_timestamp_unix_ms: unix_timestamp_ms(),
        }
    }

    fn oldest_borrow_age_ns(&self) -> Option<u64> {
        let now = monotonic_ns();
        self.borrow_slots
            .iter()
            .filter_map(|slot| {
                let state = slot.state.load(Ordering::Relaxed);
                (state & BORROW_COMMITTED != 0 && state & BORROW_RELEASING == 0)
                    .then(|| now.saturating_sub(state & BORROW_TIMESTAMP_MASK))
            })
            .max()
    }
}

impl PressureSnapshotSource for PressureState {
    fn pressure_snapshot(&self) -> StreamPressure {
        self.snapshot()
    }
}

pub(crate) struct LoanTracker {
    state: Arc<PressureState>,
    active: bool,
}

impl LoanTracker {
    pub(crate) fn release(&mut self) {
        if self.active {
            self.active = false;
            saturating_sub(&self.state.loans_in_use, 1);
        }
    }
}

impl Drop for LoanTracker {
    fn drop(&mut self) {
        self.release();
    }
}

pub(crate) struct BorrowReservation {
    state: Option<Arc<PressureState>>,
    slot: usize,
    start_ns: u64,
}

impl BorrowReservation {
    pub(crate) fn commit(self) -> BorrowTracker {
        let mut reservation = self;
        let state = reservation
            .state
            .take()
            .expect("borrow reservation is committed at most once");
        state.commit_borrow(reservation.slot, reservation.start_ns);
        state.borrow_succeeded();
        BorrowTracker {
            state,
            slot: reservation.slot,
            start_ns: reservation.start_ns,
        }
    }
}

impl Drop for BorrowReservation {
    fn drop(&mut self) {
        if let Some(state) = &self.state {
            state.release_reservation(self.slot, self.start_ns);
        }
    }
}

pub(crate) struct BorrowTracker {
    state: Arc<PressureState>,
    slot: usize,
    start_ns: u64,
}

impl Drop for BorrowTracker {
    fn drop(&mut self) {
        self.state.release_borrow(self.slot, self.start_ns);
    }
}

pub(crate) fn snapshots() -> Vec<StreamPressure> {
    rbt_core::local_pressure_snapshots()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, Barrier};
    use std::thread;
    use std::time::Duration;

    fn publisher_state(max_borrowed: usize) -> Arc<PressureState> {
        PressureState::publisher(
            "rbt/test/pressure/publisher",
            StreamProfile::Sensor,
            StreamSemanticConfig::for_profile(StreamProfile::Sensor),
            StreamResourceConfig::new(2, max_borrowed),
            || 0,
        )
    }

    fn subscriber_state(max_borrowed: usize) -> Arc<PressureState> {
        PressureState::subscriber(
            "rbt/test/pressure/subscriber",
            StreamProfile::Sensor,
            StreamSemanticConfig::for_profile(StreamProfile::Sensor),
            StreamResourceConfig::new(2, max_borrowed),
        )
    }

    #[test]
    fn saturating_counters_and_loan_totals_do_not_wrap() {
        let value = AtomicU64::new(u64::MAX - 1);
        assert_eq!(saturating_add(&value, 10), u64::MAX);
        assert_eq!(saturating_add(&value, 1), u64::MAX);
        assert_eq!(saturating_max(&value, u64::MAX), u64::MAX);

        let state = publisher_state(1);
        let first = PressureState::loan_succeeded_arc(&state, u64::MAX);
        let second = PressureState::loan_succeeded_arc(&state, 1);
        let counters = state.snapshot().publisher.expect("publisher counters");
        assert_eq!(counters.payload_bytes_last, 1);
        assert_eq!(counters.payload_bytes_max_seen, u64::MAX);
        assert_eq!(counters.total_loaned, u64::MAX);
        drop(second);
        drop(first);
        assert_eq!(
            state
                .snapshot()
                .publisher
                .expect("publisher counters")
                .loans_in_use,
            0
        );
    }

    #[test]
    fn loan_tracker_updates_current_and_high_water_without_wrapping() {
        let state = publisher_state(1);
        let first = PressureState::loan_succeeded_arc(&state, 8);
        let second = PressureState::loan_succeeded_arc(&state, 16);
        let counters = state.snapshot().publisher.expect("publisher counters");
        assert_eq!(counters.loans_in_use, 2);
        assert_eq!(counters.loans_high_water, 2);

        drop(first);
        assert_eq!(
            state
                .snapshot()
                .publisher
                .expect("publisher counters")
                .loans_in_use,
            1
        );
        let third = PressureState::loan_succeeded_arc(&state, 32);
        let counters = state.snapshot().publisher.expect("publisher counters");
        assert_eq!(counters.loans_in_use, 2);
        assert_eq!(counters.loans_high_water, 2);

        drop(third);
        drop(second);
        assert_eq!(
            state
                .snapshot()
                .publisher
                .expect("publisher counters")
                .loans_in_use,
            0
        );
    }

    #[test]
    fn committed_borrows_report_oldest_age_and_completed_duration() {
        let state = subscriber_state(2);
        let oldest = state.reserve_borrow().expect("first borrow slot").commit();
        thread::sleep(Duration::from_millis(1));
        let oldest_age = state
            .snapshot()
            .subscriber
            .expect("subscriber counters")
            .oldest_borrow_age_ns
            .expect("active borrow age");
        let newest = state.reserve_borrow().expect("second borrow slot").commit();
        let counters = state.snapshot().subscriber.expect("subscriber counters");
        assert_eq!(counters.borrowed_samples, 2);
        assert_eq!(counters.borrowed_high_water, 2);
        assert!(counters.oldest_borrow_age_ns.expect("oldest age") >= oldest_age);

        drop(newest);
        assert_eq!(
            state
                .snapshot()
                .subscriber
                .expect("subscriber counters")
                .borrow_duration_count,
            1
        );
        drop(oldest);
        let counters = state.snapshot().subscriber.expect("subscriber counters");
        assert_eq!(counters.borrowed_samples, 0);
        assert_eq!(counters.borrow_duration_count, 2);
        assert!(counters.borrow_duration_total_ns > 0);
        assert!(counters.borrow_duration_max_ns > 0);
        assert!(counters.oldest_borrow_age_ns.is_none());
    }

    #[test]
    fn concurrent_borrow_ownership_is_bounded_and_released() {
        const MAX_BORROWED: usize = 4;
        const THREADS: usize = 8;
        const ITERATIONS: usize = 128;

        let state = subscriber_state(MAX_BORROWED);
        let barrier = Arc::new(Barrier::new(THREADS));
        let workers = (0..THREADS)
            .map(|_| {
                let state = Arc::clone(&state);
                let barrier = Arc::clone(&barrier);
                thread::spawn(move || {
                    barrier.wait();
                    for _ in 0..ITERATIONS {
                        let reservation = loop {
                            if let Some(reservation) = state.reserve_borrow() {
                                break reservation;
                            }
                            thread::yield_now();
                        };
                        let tracker = reservation.commit();
                        drop(tracker);
                    }
                })
            })
            .collect::<Vec<_>>();

        for worker in workers {
            worker.join().expect("borrow worker");
        }

        let counters = state.snapshot().subscriber.expect("subscriber counters");
        assert_eq!(counters.borrowed_samples, 0);
        assert!(counters.borrowed_high_water <= MAX_BORROWED as u64);
        assert_eq!(
            counters.borrow_duration_count,
            (THREADS * ITERATIONS) as u64
        );
    }

    #[test]
    fn cancelled_receive_reservation_does_not_create_borrow_metrics() {
        let state = subscriber_state(1);
        state.receive_attempted();
        let reservation = state.reserve_borrow().expect("borrow slot");
        drop(reservation);

        let counters = state.snapshot().subscriber.expect("subscriber counters");
        assert_eq!(counters.receive_attempts, 1);
        assert_eq!(counters.received_samples, 0);
        assert_eq!(counters.borrowed_samples, 0);
        assert_eq!(counters.borrowed_high_water, 0);
        assert_eq!(counters.borrow_duration_count, 0);
        assert_eq!(counters.borrow_duration_total_ns, 0);
        assert_eq!(counters.borrow_duration_max_ns, 0);
        assert!(counters.oldest_borrow_age_ns.is_none());
    }
}

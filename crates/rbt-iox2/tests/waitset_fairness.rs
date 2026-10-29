//! Fairness and cancellation coverage for the experimental public WaitSet API.

use rbt_core::{ActivationId, ProcessInstanceId};
use rbt_iox2::{
    ControlCommand, Frame, MAX_DRAIN_PER_SOURCE, Stream, StreamError, StreamProfile, Subscriber,
    WaitContext, WaitError, WaitExit, WaitProgress, WaitToken,
};
use std::{
    sync::{
        Arc, Barrier,
        atomic::{AtomicBool, AtomicU64, Ordering},
    },
    thread,
    time::{Duration, Instant},
};

static NEXT_SERVICE: AtomicU64 = AtomicU64::new(0);

fn unique_service_name(label: &str) -> String {
    let sequence = NEXT_SERVICE.fetch_add(1, Ordering::Relaxed);
    format!(
        "rbt/cap/00000000-0000-4000-8000-{:012x}/{label}",
        (u64::from(std::process::id()) << 24) | sequence
    )
}

fn stream_error(error: StreamError) -> WaitError {
    WaitError::ListenerWait(error.to_string())
}

fn send_frame_until_delivered(
    publisher: &rbt_iox2::Publisher<Frame>,
    sequence: u64,
    stop: &AtomicBool,
) -> bool {
    for _attempt in 0..2_000 {
        if stop.load(Ordering::Acquire) {
            return false;
        }
        let mut sample = publisher.loan().expect("Frame publisher loan");
        sample.sequence = sequence;
        sample.value = sequence;
        match sample.send() {
            Ok(recipients) if recipients > 0 => return true,
            Ok(_) => thread::sleep(Duration::from_millis(1)),
            Err(_) => thread::sleep(Duration::from_millis(1)),
        }
    }
    panic!("Frame sample {sequence} was not delivered before retry bound");
}

fn send_control_until_delivered(
    publisher: &rbt_iox2::Publisher<ControlCommand>,
    sequence: u64,
    stop: &AtomicBool,
) -> bool {
    for _attempt in 0..2_000 {
        if stop.load(Ordering::Acquire) {
            return false;
        }
        let mut sample = publisher.loan().expect("Control publisher loan");
        sample.sequence = sequence;
        sample.value = sequence as f32;
        sample.reserved = 0;
        match sample.send() {
            Ok(recipients) if recipients > 0 => return true,
            Ok(_) => thread::sleep(Duration::from_millis(1)),
            Err(_) => thread::sleep(Duration::from_millis(1)),
        }
    }
    panic!("Control sample {sequence} was not delivered before retry bound");
}

fn sleep_until(deadline: Instant) {
    if let Some(remaining) = deadline.checked_duration_since(Instant::now()) {
        thread::sleep(remaining);
    }
}

#[derive(Debug, Default)]
struct SharedSent {
    high: AtomicU64,
    medium: AtomicU64,
    low: AtomicU64,
}

#[derive(Debug, Default)]
struct FairnessMetrics {
    processed: [u64; 3],
    max_scheduling_gap: [u64; 3],
    max_process_gap_us: [u64; 3],
    last_sequence: [Option<u64>; 3],
    last_processed_at: [Option<Instant>; 3],
    dispatch_count: u64,
    last_dispatch: [u64; 3],
    seen_dispatch: [bool; 3],
    maximum_budget: usize,
}

impl FairnessMetrics {
    fn record_dispatch(&mut self, source: usize) {
        self.dispatch_count += 1;
        if self.seen_dispatch[source] {
            let gap = self
                .dispatch_count
                .saturating_sub(self.last_dispatch[source] + 1);
            self.max_scheduling_gap[source] = self.max_scheduling_gap[source].max(gap);
        }
        self.last_dispatch[source] = self.dispatch_count;
        self.seen_dispatch[source] = true;
    }

    fn record_sample(&mut self, source: usize, sequence: u64) {
        if let Some(previous) = self.last_sequence[source] {
            assert!(
                sequence > previous,
                "source {source} sequence regressed from {previous} to {sequence}"
            );
        }
        let now = Instant::now();
        if let Some(previous) = self.last_processed_at[source] {
            let gap_us = now
                .duration_since(previous)
                .as_micros()
                .min(u128::from(u64::MAX)) as u64;
            self.max_process_gap_us[source] = self.max_process_gap_us[source].max(gap_us);
        }
        self.last_sequence[source] = Some(sequence);
        self.last_processed_at[source] = Some(now);
        self.processed[source] += 1;
    }
}

fn drain_frame(
    subscriber: &Subscriber<Frame>,
    metrics: &mut FairnessMetrics,
    source: usize,
    budget: usize,
) -> Result<WaitProgress, WaitError> {
    let mut samples = 0_usize;
    let mut empty_wakeups = 0_usize;
    for _ in 0..budget {
        match subscriber.receive().map_err(stream_error)? {
            Some(sample) => {
                let sequence = sample.sequence;
                metrics.record_sample(source, sequence);
                samples += 1;
                // The receive guard is deliberately dropped before the next
                // receive attempt so the test also exercises the borrow limit.
                drop(sample);
            }
            None => {
                empty_wakeups = 1;
                break;
            }
        }
    }
    Ok(match (samples, empty_wakeups) {
        (0, 0) => WaitProgress::Continue,
        (0, _) => WaitProgress::Empty,
        (_, 0) => WaitProgress::Samples(samples),
        _ => WaitProgress::SamplesAndEmpty {
            samples,
            empty_wakeups,
        },
    })
}

#[test]
fn high_medium_low_sources_remain_fair_under_flood_and_cancel_promptly() {
    const HIGH_COUNT: u64 = 800;
    const MEDIUM_COUNT: u64 = 90;
    const LOW_COUNT: u64 = 8;
    const INITIAL_DELAY: Duration = Duration::from_millis(20);
    const HIGH_PERIOD: Duration = Duration::from_millis(1);
    const MEDIUM_PERIOD: Duration = Duration::from_millis(10);
    const LOW_PERIOD: Duration = Duration::from_millis(100);
    const CANCEL_AFTER: Duration = Duration::from_millis(460);

    let high_descriptor =
        Stream::<Frame>::descriptor(unique_service_name("high"), StreamProfile::Sensor);
    let medium_descriptor =
        Stream::<Frame>::descriptor(unique_service_name("medium"), StreamProfile::Sensor);
    let low_descriptor =
        Stream::<ControlCommand>::descriptor(unique_service_name("low"), StreamProfile::Control);
    let high_publisher = Stream::<Frame>::publisher(&high_descriptor).unwrap();
    let medium_publisher = Stream::<Frame>::publisher(&medium_descriptor).unwrap();
    let low_publisher = Stream::<ControlCommand>::publisher(&low_descriptor).unwrap();
    let high_subscriber = Stream::<Frame>::subscriber(&high_descriptor).unwrap();
    let medium_subscriber = Stream::<Frame>::subscriber(&medium_descriptor).unwrap();
    let low_subscriber = Stream::<ControlCommand>::subscriber(&low_descriptor).unwrap();

    let context = WaitContext::new(ProcessInstanceId::new(), ActivationId::new(701)).unwrap();
    let wake = context.wake_handle();
    let start = Arc::new(Barrier::new(4));
    let stop = Arc::new(AtomicBool::new(false));
    let sent = Arc::new(SharedSent::default());
    let cancellation_sent_at = Arc::new(AtomicU64::new(0));
    let test_start = Instant::now();

    let (exit, metrics, stats, cancellation_finished_at) = thread::scope(|scope| {
        let high_start = Arc::clone(&start);
        let high_stop = Arc::clone(&stop);
        let high_sent = Arc::clone(&sent);
        let high_producer = scope.spawn(move || {
            high_start.wait();
            let first = Instant::now() + INITIAL_DELAY;
            sleep_until(first);
            for sequence in 0..HIGH_COUNT {
                if !send_frame_until_delivered(&high_publisher, sequence, &high_stop) {
                    break;
                }
                high_sent.high.fetch_add(1, Ordering::Relaxed);
                sleep_until(first + HIGH_PERIOD.saturating_mul((sequence + 1) as u32));
            }
        });

        let medium_start = Arc::clone(&start);
        let medium_stop = Arc::clone(&stop);
        let medium_sent = Arc::clone(&sent);
        let medium_producer = scope.spawn(move || {
            medium_start.wait();
            let first = Instant::now() + INITIAL_DELAY;
            sleep_until(first);
            for sequence in 0..MEDIUM_COUNT {
                if !send_frame_until_delivered(&medium_publisher, sequence, &medium_stop) {
                    break;
                }
                medium_sent.medium.fetch_add(1, Ordering::Relaxed);
                sleep_until(first + MEDIUM_PERIOD.saturating_mul((sequence + 1) as u32));
            }
        });

        let low_start = Arc::clone(&start);
        let low_stop = Arc::clone(&stop);
        let low_sent = Arc::clone(&sent);
        let low_producer = scope.spawn(move || {
            low_start.wait();
            let first = Instant::now() + INITIAL_DELAY;
            sleep_until(first);
            for sequence in 0..LOW_COUNT {
                if !send_control_until_delivered(&low_publisher, sequence, &low_stop) {
                    break;
                }
                low_sent.low.fetch_add(1, Ordering::Relaxed);
                sleep_until(first + LOW_PERIOD.saturating_mul((sequence + 1) as u32));
            }
        });

        let cancel_stop = Arc::clone(&stop);
        let cancel_at = Arc::clone(&cancellation_sent_at);
        let canceller = scope.spawn(move || {
            start.wait();
            thread::sleep(CANCEL_AFTER);
            cancel_stop.store(true, Ordering::Release);
            cancel_at.store(
                test_start.elapsed().as_nanos().min(u128::from(u64::MAX)) as u64,
                Ordering::Release,
            );
            wake.cancel().unwrap();
        });

        // The barrier has four parties: the three paced producers and the
        // canceller. The blocker is started immediately after they are
        // released, before the paced producers can make meaningful progress.
        let blocker = scope.spawn(move || {
            let mut context = context;
            let high_source = high_subscriber.event_source(WaitToken::new(1));
            let medium_source = medium_subscriber.event_source(WaitToken::new(2));
            let low_source = low_subscriber.event_source(WaitToken::new(3));
            let mut metrics = FairnessMetrics::default();
            let exit = context
                .run(
                    &[high_source, medium_source, low_source],
                    |event, budget| {
                        assert_eq!(budget, MAX_DRAIN_PER_SOURCE);
                        metrics.maximum_budget = metrics.maximum_budget.max(budget);
                        if event.is_synthetic() {
                            return Ok::<_, WaitError>(WaitProgress::Continue);
                        }
                        let source = match event.token().value() {
                            1 => 0,
                            2 => 1,
                            3 => 2,
                            token => panic!("unexpected source token {token}"),
                        };
                        metrics.record_dispatch(source);
                        match source {
                            0 => drain_frame(&high_subscriber, &mut metrics, source, budget),
                            1 => drain_frame(&medium_subscriber, &mut metrics, source, budget),
                            2 => {
                                let mut samples = 0_usize;
                                let mut empty_wakeups = 0_usize;
                                for _ in 0..budget {
                                    match low_subscriber.receive().map_err(stream_error)? {
                                        Some(sample) => {
                                            let sequence = sample.sequence;
                                            metrics.record_sample(source, sequence);
                                            samples += 1;
                                            drop(sample);
                                        }
                                        None => {
                                            empty_wakeups = 1;
                                            break;
                                        }
                                    }
                                }
                                Ok(match (samples, empty_wakeups) {
                                    (0, 0) => WaitProgress::Continue,
                                    (0, _) => WaitProgress::Empty,
                                    (_, 0) => WaitProgress::Samples(samples),
                                    _ => WaitProgress::SamplesAndEmpty {
                                        samples,
                                        empty_wakeups,
                                    },
                                })
                            }
                            _ => unreachable!(),
                        }
                    },
                )
                .unwrap();
            (
                exit,
                metrics,
                context.stats().snapshot(),
                test_start.elapsed(),
            )
        });

        let (exit, metrics, stats, elapsed) = blocker.join().unwrap();
        high_producer.join().unwrap();
        medium_producer.join().unwrap();
        low_producer.join().unwrap();
        canceller.join().unwrap();
        (exit, metrics, stats, elapsed)
    });

    assert_eq!(exit, WaitExit::Cancelled);
    let sent_counts = [
        sent.high.load(Ordering::Acquire),
        sent.medium.load(Ordering::Acquire),
        sent.low.load(Ordering::Acquire),
    ];
    let cancellation_sent = cancellation_sent_at.load(Ordering::Acquire);
    let cancellation_latency = cancellation_finished_at
        .as_nanos()
        .saturating_sub(u128::from(cancellation_sent));
    assert!(
        sent_counts[0] >= 250,
        "high source sent too few samples: {sent_counts:?}"
    );
    assert!(
        sent_counts[1] >= 20,
        "medium source sent too few samples: {sent_counts:?}"
    );
    assert!(
        sent_counts[2] >= 3,
        "low source sent too few samples: {sent_counts:?}"
    );
    assert!(metrics.processed.iter().all(|count| *count >= 3));
    assert!(metrics.max_scheduling_gap.iter().all(|gap| *gap <= 256));
    assert!(metrics.max_process_gap_us.iter().all(|gap| *gap <= 500_000));
    assert_eq!(metrics.maximum_budget, MAX_DRAIN_PER_SOURCE);
    assert!(cancellation_sent > 0);
    assert!(cancellation_latency < Duration::from_millis(500).as_nanos());

    println!(
        concat!(
            "RBT_WAITSET_FAIRNESS sent={:?} processed={:?} max_scheduling_gap={:?} ",
            "max_process_gap_us={:?} ",
            "max_budget={} wakeups={} empty_wakeups={} cancellation_wakeups={} ",
            "cancel_latency_us={}"
        ),
        sent_counts,
        metrics.processed,
        metrics.max_scheduling_gap,
        metrics.max_process_gap_us,
        metrics.maximum_budget,
        stats.wakeups,
        stats.empty_wakeups,
        stats.cancellation_wakeups,
        cancellation_latency / 1_000,
    );
}

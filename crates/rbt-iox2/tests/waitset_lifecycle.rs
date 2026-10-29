//! Attachment teardown, repeated activation, and token validation coverage.

use rbt_core::{ActivationId, ProcessInstanceId};
use rbt_iox2::{
    Frame, MAX_DRAIN_PER_SOURCE, Stream, StreamError, StreamProfile, WaitContext, WaitError,
    WaitExit, WaitProgress, WaitToken,
};
use std::{
    fs,
    sync::{
        Arc, Barrier, Condvar, Mutex,
        atomic::{AtomicU64, Ordering},
    },
    thread,
    time::{Duration, Instant},
};

static NEXT_SERVICE: AtomicU64 = AtomicU64::new(0);
// `/proc/self` resource counts cover the whole integration-test process.
// Serialize this binary's cases so another case cannot open or close its
// endpoints between the measured baseline and final snapshot.
static TEST_SERIAL: Mutex<()> = Mutex::new(());

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

fn send_frame_until_delivered(publisher: &rbt_iox2::Publisher<Frame>, sequence: u64) {
    for _attempt in 0..2_000 {
        let mut sample = publisher.loan().expect("Frame publisher loan");
        sample.sequence = sequence;
        sample.value = sequence;
        match sample.send() {
            Ok(recipients) if recipients > 0 => return,
            Ok(_) | Err(_) => thread::sleep(Duration::from_millis(1)),
        }
    }
    panic!("Frame sample {sequence} was not delivered before retry bound");
}

fn proc_count(path: &str) -> usize {
    fs::read_dir(path)
        .unwrap_or_else(|error| panic!("failed to read {path}: {error}"))
        .count()
}

fn rss_kib() -> Option<u64> {
    let status = fs::read_to_string("/proc/self/status").ok()?;
    status.lines().find_map(|line| {
        let value = line.strip_prefix("VmRSS:")?.split_whitespace().next()?;
        value.parse().ok()
    })
}

fn wait_for_callback() -> Arc<(Mutex<bool>, Condvar)> {
    Arc::new((Mutex::new(false), Condvar::new()))
}

fn signal_callback(state: &(Mutex<bool>, Condvar)) {
    let (ready, changed) = state;
    *ready.lock().unwrap() = true;
    changed.notify_one();
}

fn await_callback(state: &(Mutex<bool>, Condvar)) {
    let (ready, changed) = state;
    let mut ready = ready.lock().unwrap();
    while !*ready {
        ready = changed.wait(ready).unwrap();
    }
}

#[test]
fn attachments_drop_after_stop_and_remaining_source_can_be_reused() {
    let _serial = TEST_SERIAL.lock().unwrap();
    let descriptor_a =
        Stream::<Frame>::descriptor(unique_service_name("lifecycle-a"), StreamProfile::Sensor);
    let descriptor_b =
        Stream::<Frame>::descriptor(unique_service_name("lifecycle-b"), StreamProfile::Sensor);
    let publisher_a = Stream::<Frame>::publisher(&descriptor_a).unwrap();
    let publisher_b = Stream::<Frame>::publisher(&descriptor_b).unwrap();
    let subscriber_a = Stream::<Frame>::subscriber(&descriptor_a).unwrap();
    let subscriber_b = Stream::<Frame>::subscriber(&descriptor_b).unwrap();
    let token_a = WaitToken::new(101);
    let token_b = WaitToken::new(102);
    let process = ProcessInstanceId::new();
    let mut context = WaitContext::new(process, ActivationId::new(801)).unwrap();

    // Run with both sources and stop only after both payloads have made the
    // ordinary subscriber.receive() round trip.
    let started = Arc::new(Barrier::new(2));
    let first_result = thread::scope(|scope| {
        let producer = scope.spawn(|| {
            started.wait();
            thread::sleep(Duration::from_millis(20));
            send_frame_until_delivered(&publisher_a, 10);
            send_frame_until_delivered(&publisher_b, 20);
        });
        let blocker = scope.spawn(|| {
            let source_a = subscriber_a.event_source(token_a);
            let source_b = subscriber_b.event_source(token_b);
            let mut seen = [false; 2];
            started.wait();
            let exit = context
                .run(&[source_a, source_b], |event, budget| {
                    assert_eq!(budget, MAX_DRAIN_PER_SOURCE);
                    if event.is_synthetic() {
                        return Ok::<_, WaitError>(WaitProgress::Continue);
                    }
                    let source = match event.token() {
                        token if token == token_a => 0,
                        token if token == token_b => 1,
                        token => panic!("unexpected token {token}"),
                    };
                    let received = match source {
                        0 => subscriber_a.receive().map_err(stream_error)?,
                        1 => subscriber_b.receive().map_err(stream_error)?,
                        _ => unreachable!(),
                    };
                    if let Some(sample) = received {
                        assert_eq!(sample.sequence, if source == 0 { 10 } else { 20 });
                        seen[source] = true;
                        drop(sample);
                        if seen.iter().all(|value| *value) {
                            return Ok(WaitProgress::stop_after_samples(1));
                        }
                        return Ok(WaitProgress::samples(1));
                    }
                    Ok(WaitProgress::Empty)
                })
                .unwrap();
            (exit, seen, context.stats().snapshot())
        });
        let result = blocker.join().unwrap();
        producer.join().unwrap();
        result
    });
    assert_eq!(first_result.0, WaitExit::Stopped);
    assert_eq!(first_result.1, [true, true]);
    assert!(first_result.2.samples_processed >= 2);

    // The source borrows existed only inside the first run. Dropping B now
    // proves that its attachment guard did not retain either endpoint.
    drop(subscriber_b);
    drop(publisher_b);

    // Leave A's sidecar event pending while consuming its payload. The next
    // run must classify this as a spurious readiness wake, then receive the
    // next payload through the normal zero-copy subscriber path.
    send_frame_until_delivered(&publisher_a, 30);
    let stale = subscriber_a.receive().unwrap().unwrap();
    assert_eq!(stale.sequence, 30);
    drop(stale);

    let synthetic_seen = Arc::new(Barrier::new(2));
    let spurious_seen = Arc::new((Mutex::new(false), Condvar::new()));
    let second_result = thread::scope(|scope| {
        let synthetic_seen_for_blocker = Arc::clone(&synthetic_seen);
        let spurious_seen_for_blocker = Arc::clone(&spurious_seen);
        let subscriber_a_for_blocker = &subscriber_a;
        let context_for_blocker = &mut context;
        let blocker = scope.spawn(move || {
            let source_a = subscriber_a_for_blocker.event_source(token_a);
            let mut saw_spurious = false;
            let exit = context_for_blocker
                .run(std::slice::from_ref(&source_a), |event, budget| {
                    assert_eq!(budget, MAX_DRAIN_PER_SOURCE);
                    if event.is_synthetic() {
                        synthetic_seen_for_blocker.wait();
                        return Ok::<_, WaitError>(WaitProgress::Continue);
                    }
                    let received = subscriber_a_for_blocker.receive().map_err(stream_error)?;
                    if let Some(sample) = received {
                        assert_eq!(sample.sequence, 40);
                        drop(sample);
                        return Ok(WaitProgress::stop_after_samples(1));
                    }
                    saw_spurious = true;
                    let (flag, changed) = &*spurious_seen_for_blocker;
                    *flag.lock().unwrap() = true;
                    changed.notify_one();
                    Ok(WaitProgress::Empty)
                })
                .unwrap();
            (exit, saw_spurious)
        });
        synthetic_seen.wait();
        await_callback(&spurious_seen);
        send_frame_until_delivered(&publisher_a, 40);
        blocker.join().unwrap()
    });
    assert_eq!(second_result.0, WaitExit::Stopped);
    assert!(second_result.1, "stale sidecar readiness was not observed");
    assert!(*spurious_seen.0.lock().unwrap());

    // Reuse has been exercised above. A fresh context with the same remaining
    // source confirms that cancellation endpoints and source guards are also
    // independently disposable between activations.
    drop(context);
    let mut replacement = WaitContext::new(process, ActivationId::new(802)).unwrap();
    let replacement_attached = Arc::new(Barrier::new(2));
    let replacement_ready = wait_for_callback();
    let replacement_result = thread::scope(|scope| {
        let attached_for_blocker = Arc::clone(&replacement_attached);
        let ready_for_blocker = Arc::clone(&replacement_ready);
        let subscriber_a_for_blocker = &subscriber_a;
        let replacement_for_blocker = &mut replacement;
        let blocker = scope.spawn(move || {
            let source_a = subscriber_a_for_blocker.event_source(token_a);
            replacement_for_blocker
                .run(std::slice::from_ref(&source_a), |event, budget| {
                    assert_eq!(budget, MAX_DRAIN_PER_SOURCE);
                    if event.is_synthetic() {
                        attached_for_blocker.wait();
                        return Ok::<_, WaitError>(WaitProgress::Continue);
                    }
                    let received = subscriber_a_for_blocker.receive().map_err(stream_error)?;
                    if let Some(sample) = received {
                        assert_eq!(sample.sequence, 50);
                        drop(sample);
                        signal_callback(&ready_for_blocker);
                        return Ok(WaitProgress::stop_after_samples(1));
                    }
                    Ok(WaitProgress::Empty)
                })
                .unwrap()
        });
        replacement_attached.wait();
        send_frame_until_delivered(&publisher_a, 50);
        await_callback(&replacement_ready);
        blocker.join().unwrap()
    });
    assert_eq!(replacement_result, WaitExit::Stopped);
}

#[test]
fn one_hundred_activation_cycles_leave_no_waitset_resource_leaks() {
    let _serial = TEST_SERIAL.lock().unwrap();
    const CYCLES: u64 = 100;
    let descriptor =
        Stream::<Frame>::descriptor(unique_service_name("cycles"), StreamProfile::Sensor);
    let publisher = Stream::<Frame>::publisher(&descriptor).unwrap();
    let subscriber = Stream::<Frame>::subscriber(&descriptor).unwrap();
    let process = ProcessInstanceId::new();
    let token = WaitToken::new(201);

    // Warm the endpoint discovery before taking the resource baseline. All
    // subsequent consumer waits are synchronized by barriers, not polling.
    send_frame_until_delivered(&publisher, 0);
    let warmup = subscriber.receive().unwrap().unwrap();
    drop(warmup);
    let mut warm_context = WaitContext::new(process, ActivationId::new(999)).unwrap();
    let warm_wake = warm_context.wake_handle();
    let warm_attached = Arc::new(Barrier::new(2));
    let warm_exit = thread::scope(|scope| {
        let warm_attached_for_blocker = Arc::clone(&warm_attached);
        let subscriber_for_blocker = &subscriber;
        let context_for_blocker = &mut warm_context;
        let blocker = scope.spawn(move || {
            let source = subscriber_for_blocker.event_source(token);
            context_for_blocker
                .run(std::slice::from_ref(&source), |event, budget| {
                    assert_eq!(budget, MAX_DRAIN_PER_SOURCE);
                    if event.is_synthetic() {
                        warm_attached_for_blocker.wait();
                        return Ok::<_, WaitError>(WaitProgress::Continue);
                    }
                    let _ = subscriber_for_blocker.receive().map_err(stream_error)?;
                    Ok(WaitProgress::Empty)
                })
                .unwrap()
        });
        warm_attached.wait();
        warm_wake.cancel().unwrap();
        blocker.join().unwrap()
    });
    assert_eq!(warm_exit, WaitExit::Cancelled);
    drop(warm_wake);
    drop(warm_context);
    let fd_before = proc_count("/proc/self/fd");
    let task_before = proc_count("/proc/self/task");
    let rss_before = rss_kib();
    let started_at = Instant::now();

    // Keep these stream endpoints persistent so each cycle isolates repeated
    // WaitSet attachment/context teardown from endpoint discovery churn.
    for cycle in 0..CYCLES {
        let mut context = WaitContext::new(process, ActivationId::new(1_000 + cycle)).unwrap();
        let wake = context.wake_handle();
        let attached = Arc::new(Barrier::new(2));
        let received = Arc::new(Barrier::new(2));

        let exit = thread::scope(|scope| {
            let attached_for_blocker = Arc::clone(&attached);
            let received_for_blocker = Arc::clone(&received);
            let subscriber_for_blocker = &subscriber;
            let context_for_blocker = &mut context;
            let blocker = scope.spawn(move || {
                let source = subscriber_for_blocker.event_source(token);
                context_for_blocker
                    .run(std::slice::from_ref(&source), |event, budget| {
                        assert_eq!(budget, MAX_DRAIN_PER_SOURCE);
                        if event.is_synthetic() {
                            // This barrier is reached only after WaitSet
                            // attachment, so cancellation cannot bypass the
                            // attachment/guard teardown path.
                            attached_for_blocker.wait();
                            return Ok::<_, WaitError>(WaitProgress::Continue);
                        }
                        let received_sample =
                            subscriber_for_blocker.receive().map_err(stream_error)?;
                        if let Some(sample) = received_sample {
                            assert_eq!(sample.sequence, cycle);
                            drop(sample);
                            received_for_blocker.wait();
                            return Ok(WaitProgress::samples(1));
                        }
                        Ok(WaitProgress::Empty)
                    })
                    .unwrap()
            });
            attached.wait();
            send_frame_until_delivered(&publisher, cycle);
            received.wait();
            wake.cancel().unwrap();
            blocker.join().unwrap()
        });
        assert_eq!(exit, WaitExit::Cancelled, "cycle {cycle}");
    }

    let elapsed = started_at.elapsed();
    let fd_after = proc_count("/proc/self/fd");
    let task_after = proc_count("/proc/self/task");
    let rss_after = rss_kib();
    let fd_delta = fd_after.abs_diff(fd_before);
    let task_delta = task_after.abs_diff(task_before);
    assert!(fd_delta <= 4, "file-descriptor count changed by {fd_delta}");
    assert!(task_delta <= 2, "thread count changed by {task_delta}");
    if let (Some(rss_before), Some(rss_after)) = (rss_before, rss_after) {
        const RSS_GROWTH_LIMIT_KIB: u64 = 16 * 1024;
        let rss_growth_kib = rss_after.saturating_sub(rss_before);
        assert!(
            rss_growth_kib <= RSS_GROWTH_LIMIT_KIB,
            "RSS grew by {rss_growth_kib} KiB over {CYCLES} cycles \
             (before={rss_before} KiB, after={rss_after} KiB)"
        );
    }
    assert!(elapsed < Duration::from_secs(5), "cycles took {elapsed:?}");

    println!(
        concat!(
            "RBT_WAITSET_CYCLES cycles={} elapsed_ms={} fd_before={} ",
            "fd_after={} fd_delta={} task_before={} task_after={} task_delta={} ",
            "rss_before_kib={:?} rss_after_kib={:?}"
        ),
        CYCLES,
        elapsed.as_millis(),
        fd_before,
        fd_after,
        fd_delta,
        task_before,
        task_after,
        task_delta,
        rss_before,
        rss_after,
    );
}

#[test]
fn duplicate_stable_token_returns_typed_error() {
    let _serial = TEST_SERIAL.lock().unwrap();
    let descriptor = Stream::<Frame>::descriptor(
        unique_service_name("duplicate-token"),
        StreamProfile::Sensor,
    );
    let publisher = Stream::<Frame>::publisher(&descriptor).unwrap();
    let subscriber = Stream::<Frame>::subscriber(&descriptor).unwrap();
    let mut context = WaitContext::new(ProcessInstanceId::new(), ActivationId::new(901)).unwrap();
    let token = WaitToken::new(77);
    let first = subscriber.event_source(token);
    let second = subscriber.event_source(token);
    let error = context
        .run(&[first, second], |_, _| {
            Ok::<_, WaitError>(WaitProgress::Continue)
        })
        .unwrap_err();
    match error {
        WaitError::DuplicateToken(actual) => assert_eq!(actual, token),
        other => panic!("expected typed duplicate-token error, got {other:?}"),
    }
    drop(publisher);
}

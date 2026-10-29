use rbt_core::{ActivationId, ProcessInstanceId};
use rbt_iox2::{
    Frame, Image, ReadyEvent, SliceStream, Stream, StreamProfile, WaitContext, WaitExit,
    WaitProgress, WaitToken,
};
use std::{
    sync::atomic::{AtomicU64, Ordering},
    thread,
    time::{Duration, Instant},
};

static NEXT_SERVICE: AtomicU64 = AtomicU64::new(0);

fn unique_service_name() -> String {
    format!(
        "rbt/cap/00000000-0000-4000-8000-{:012x}/stream",
        (u64::from(std::process::id()) << 24) | NEXT_SERVICE.fetch_add(1, Ordering::Relaxed)
    )
}

#[test]
fn fixed_source_wakes_and_receives_without_payload_in_ready_event() {
    let descriptor = Stream::<Frame>::descriptor(unique_service_name(), StreamProfile::Sensor);
    let publisher = Stream::<Frame>::publisher(&descriptor).unwrap();
    let subscriber = Stream::<Frame>::subscriber(&descriptor).unwrap();
    let token = WaitToken::new(7);
    let context = WaitContext::new(ProcessInstanceId::new(), ActivationId::first()).unwrap();

    // The blocking task owns the context and subscriber.  Only the source
    // borrow is created inside that task, so the context is not tied to a
    // subscriber lifetime and its wake handle can be handed to another task.
    let (received, stats) = thread::scope(|scope| {
        let producer = scope.spawn(move || {
            thread::sleep(Duration::from_millis(20));
            let mut sample = publisher.loan().unwrap();
            sample.value = 42;
            sample.send().unwrap();
        });
        let blocker = scope.spawn(move || {
            let mut context = context;
            let source = subscriber.event_source(token);
            let exit = context
                .run(
                    std::slice::from_ref(&source),
                    |event: ReadyEvent, budget| {
                        assert!(budget <= 64);
                        assert!(!event.is_cancellation());
                        assert_eq!(event.token(), token);
                        if event.is_synthetic() {
                            return Ok::<_, rbt_iox2::WaitError>(WaitProgress::Continue);
                        }
                        let sample = subscriber.receive().unwrap();
                        if let Some(sample) = sample {
                            assert_eq!(sample.value, 42);
                            return Ok(WaitProgress::stop_after_samples(1));
                        }
                        Ok(WaitProgress::Empty)
                    },
                )
                .unwrap();
            (exit, context.stats().snapshot())
        });
        let result = blocker.join().unwrap();
        producer.join().unwrap();
        result
    });
    assert_eq!(received, WaitExit::Stopped);
    // The synthetic initial source pass is not a blocking wakeup.
    assert!(stats.wakeups >= 1);
    assert_eq!(stats.samples_processed, 1);
    assert_eq!(stats.empty_wakeups, 0);
}

#[test]
fn cancellation_wakes_idle_wait_promptly() {
    let context = WaitContext::new(ProcessInstanceId::new(), ActivationId::new(2)).unwrap();
    let wake = context.wake_handle();
    let started = Instant::now();
    let (exit, cancellation_wakeups) = thread::scope(|scope| {
        let canceller = scope.spawn(move || {
            thread::sleep(Duration::from_millis(20));
            wake.cancel().unwrap();
        });
        let blocker = scope.spawn(move || {
            let mut context = context;
            let exit = context
                .run(&[], |_, _| {
                    Ok::<_, rbt_iox2::WaitError>(WaitProgress::Continue)
                })
                .unwrap();
            (exit, context.stats().cancellation_wakeups())
        });
        let result = blocker.join().unwrap();
        canceller.join().unwrap();
        result
    });
    assert_eq!(exit, WaitExit::Cancelled);
    assert!(started.elapsed() < Duration::from_secs(1));
    assert_eq!(cancellation_wakeups, 1);
}

#[test]
fn native_timeout_bounds_idle_wait_without_a_wake_thread() {
    let mut context = WaitContext::new(ProcessInstanceId::new(), ActivationId::new(20)).unwrap();
    let started = Instant::now();
    let exit = context
        .run_with_timeout(&[], Duration::from_millis(40), |_, _| {
            Ok::<_, rbt_iox2::WaitError>(WaitProgress::Continue)
        })
        .unwrap();
    let elapsed = started.elapsed();

    assert_eq!(exit, None);
    assert!(elapsed >= Duration::from_millis(30));
    assert!(elapsed < Duration::from_secs(1));
    let stats = context.stats().snapshot();
    assert_eq!(stats.waits, 1);
    assert_eq!(stats.wakeups, 0);
}

#[test]
fn slice_source_wakes_and_preserves_borrowed_payload() {
    let descriptor = SliceStream::<Image>::descriptor(unique_service_name(), StreamProfile::Sensor);
    let publisher = SliceStream::<Image>::publisher(&descriptor, 8).unwrap();
    let subscriber = SliceStream::<Image>::subscriber(&descriptor).unwrap();
    let token = WaitToken::new(11);
    let context = WaitContext::new(ProcessInstanceId::new(), ActivationId::new(3)).unwrap();

    let (exit, stats) = thread::scope(|scope| {
        let producer = scope.spawn(move || {
            thread::sleep(Duration::from_millis(20));
            let mut sample = publisher.loan_slice(3).unwrap();
            sample.header_mut().sequence = 19;
            sample.payload_mut().copy_from_slice(&[3, 1, 4]);
            sample.send().unwrap();
        });
        let blocker = scope.spawn(move || {
            let mut context = context;
            let source = subscriber.event_source(token);
            let exit = context
                .run(std::slice::from_ref(&source), |event, budget| {
                    assert_eq!(budget, 64);
                    assert_eq!(event.token(), token);
                    if event.is_synthetic() {
                        return Ok::<_, rbt_iox2::WaitError>(WaitProgress::Continue);
                    }
                    let sample = subscriber.receive().unwrap();
                    if let Some(sample) = sample {
                        assert_eq!(sample.header().sequence, 19);
                        assert_eq!(sample.payload(), &[3, 1, 4]);
                        return Ok(WaitProgress::stop_after_samples(1));
                    }
                    Ok(WaitProgress::Empty)
                })
                .unwrap();
            (exit, context.stats().snapshot())
        });
        let result = blocker.join().unwrap();
        producer.join().unwrap();
        result
    });

    // The producer and blocker are scoped together, so a received payload
    // must have come through the SHM subscriber rather than the wake event.
    assert_eq!(exit, WaitExit::Stopped);
    assert!(stats.samples_processed >= 1);
}

#[test]
fn two_sources_are_dispatched_by_stable_tokens() {
    let descriptor_a = Stream::<Frame>::descriptor(unique_service_name(), StreamProfile::Sensor);
    let descriptor_b = Stream::<Frame>::descriptor(unique_service_name(), StreamProfile::Sensor);
    let publisher_a = Stream::<Frame>::publisher(&descriptor_a).unwrap();
    let publisher_b = Stream::<Frame>::publisher(&descriptor_b).unwrap();
    let subscriber_a = Stream::<Frame>::subscriber(&descriptor_a).unwrap();
    let subscriber_b = Stream::<Frame>::subscriber(&descriptor_b).unwrap();
    let token_a = WaitToken::new(21);
    let token_b = WaitToken::new(22);
    let context = WaitContext::new(ProcessInstanceId::new(), ActivationId::new(4)).unwrap();

    let (exit, seen) = thread::scope(|scope| {
        let producers = scope.spawn(move || {
            thread::sleep(Duration::from_millis(15));
            let mut first = publisher_a.loan().unwrap();
            first.value = 101;
            first.send().unwrap();
            thread::sleep(Duration::from_millis(15));
            let mut second = publisher_b.loan().unwrap();
            second.value = 202;
            second.send().unwrap();
        });
        let blocker = scope.spawn(move || {
            let mut context = context;
            let source_a = subscriber_a.event_source(token_a);
            let source_b = subscriber_b.event_source(token_b);
            let mut seen = [false; 2];
            let exit = context
                .run(&[source_a, source_b], |event, _| {
                    if event.is_synthetic() {
                        return Ok::<_, rbt_iox2::WaitError>(WaitProgress::Continue);
                    }
                    if event.token() == token_a {
                        let received = subscriber_a.receive().unwrap();
                        if let Some(sample) = received {
                            assert_eq!(sample.value, 101);
                            seen[0] = true;
                            return Ok(WaitProgress::samples(1));
                        }
                    } else if event.token() == token_b {
                        let received = subscriber_b.receive().unwrap();
                        if let Some(sample) = received {
                            assert_eq!(sample.value, 202);
                            seen[1] = true;
                            if seen.iter().all(|value| *value) {
                                return Ok(WaitProgress::Stop);
                            }
                            return Ok(WaitProgress::samples(1));
                        }
                    }
                    if seen.iter().all(|value| *value) {
                        Ok(WaitProgress::Stop)
                    } else {
                        Ok(WaitProgress::Empty)
                    }
                })
                .unwrap();
            (exit, seen)
        });
        let result = blocker.join().unwrap();
        producers.join().unwrap();
        result
    });

    assert_eq!(exit, WaitExit::Stopped);
    assert_eq!(seen, [true, true]);
}

#[test]
fn spurious_readiness_is_empty_and_teardown_repeats_cleanly() {
    let descriptor = Stream::<Frame>::descriptor(unique_service_name(), StreamProfile::Sensor);
    let publisher = Stream::<Frame>::publisher(&descriptor).unwrap();
    let subscriber = Stream::<Frame>::subscriber(&descriptor).unwrap();
    let mut sample = publisher.loan().unwrap();
    sample.value = 9;
    sample.send().unwrap();
    // Consume the payload while leaving its sidecar event pending.  The wait
    // callback must tolerate that notification/payload are not one-to-one.
    assert_eq!(subscriber.receive().unwrap().unwrap().value, 9);

    let context = WaitContext::new(ProcessInstanceId::new(), ActivationId::new(5)).unwrap();
    let wake = context.wake_handle();
    let (exit, stats) = thread::scope(|scope| {
        let canceller = scope.spawn(move || {
            thread::sleep(Duration::from_millis(100));
            wake.cancel().unwrap();
        });
        let blocker = scope.spawn(move || {
            let mut context = context;
            let source = subscriber.event_source(WaitToken::new(31));
            let exit = context
                .run(std::slice::from_ref(&source), |event, _| {
                    if !event.is_synthetic() {
                        assert!(subscriber.receive().unwrap().is_none());
                    }
                    Ok::<_, rbt_iox2::WaitError>(WaitProgress::Empty)
                })
                .unwrap();
            (exit, context.stats().snapshot())
        });
        let result = blocker.join().unwrap();
        canceller.join().unwrap();
        result
    });
    assert_eq!(exit, WaitExit::Cancelled);
    assert_eq!(stats.empty_wakeups, 1);

    for cycle in 0..8 {
        let descriptor = Stream::<Frame>::descriptor(unique_service_name(), StreamProfile::Sensor);
        let publisher = Stream::<Frame>::publisher(&descriptor).unwrap();
        let subscriber = Stream::<Frame>::subscriber(&descriptor).unwrap();
        let context =
            WaitContext::new(ProcessInstanceId::new(), ActivationId::new(100 + cycle)).unwrap();
        let wake = context.wake_handle();
        let exit = thread::scope(|scope| {
            let canceller = scope.spawn(move || {
                thread::sleep(Duration::from_millis(2));
                wake.cancel().unwrap();
            });
            let blocker = scope.spawn(move || {
                let mut context = context;
                let source = subscriber.event_source(WaitToken::new(40));
                // Keep the publisher alive until after the context has built
                // its local guard set; no payload should be prefetched here.
                let _publisher = publisher;
                context
                    .run(std::slice::from_ref(&source), |_, _| {
                        Ok::<_, rbt_iox2::WaitError>(WaitProgress::Continue)
                    })
                    .unwrap()
            });
            let result = blocker.join().unwrap();
            canceller.join().unwrap();
            result
        });
        assert_eq!(exit, WaitExit::Cancelled);
    }
}

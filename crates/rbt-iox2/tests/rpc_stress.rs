//! Bounded RPC stress checks.
//!
//! These tests deliberately keep the native request/response service small
//! while exercising repeated terminal-state transitions.  Every test uses a
//! process-unique service name so that cargo's parallel integration-test
//! scheduling cannot make one endpoint observe another test's state.

use std::{
    sync::{
        Arc, Barrier,
        atomic::{AtomicU64, Ordering},
        mpsc,
    },
    thread,
    time::Duration,
};

use rbt_iox2::{
    Detection, Frame, RpcClient, RpcDescriptor, RpcError, RpcResourceConfig, RpcServer,
};

static NEXT_SERVICE: AtomicU64 = AtomicU64::new(0);

fn unique_service_name(test: &str) -> String {
    let sequence = NEXT_SERVICE.fetch_add(1, Ordering::Relaxed);
    let value = (u64::from(std::process::id()) << 20) | (sequence & 0x000f_ffff);
    format!("rbt/rpc/00000000-0000-4000-8000-{value:012x}/{test}")
}

fn descriptor(service_name: String, resources: RpcResourceConfig) -> RpcDescriptor {
    RpcDescriptor::new_with_resources(
        service_name,
        Frame::descriptor(),
        Detection::descriptor(),
        resources,
    )
}

fn marker(index: usize, base: u64) -> Frame {
    Frame {
        sequence: index as u64,
        value: base + index as u64,
    }
}

/// One thousand timeout/late-response cycles must leave the native channel
/// reusable.  The late response is intentionally sent after the pending call
/// has claimed its deadline; the next call must still receive its own marker.
#[test]
fn thousand_timeout_late_response_reuse_cycles_have_no_in_flight_calls() -> Result<(), RpcError> {
    const CYCLES: usize = 1_000;
    let resources = RpcResourceConfig::default()
        .with_max_clients(1)
        .with_max_in_flight(1)
        .with_max_request_loans(2);
    let descriptor = descriptor(unique_service_name("timeout-reuse"), resources);
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let (ready_tx, ready_rx) = mpsc::channel();
    let (release_tx, release_rx) = mpsc::channel();
    let (late_tx, late_rx) = mpsc::channel();

    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        for cycle in 0..CYCLES {
            let first = server
                .receive_timeout(Duration::from_secs(5))?
                .expect("timed-out request");
            ready_tx.send(cycle).expect("request notification");
            assert_eq!(
                release_rx.recv().expect("release late response"),
                cycle,
                "late-response release crossed cycles"
            );

            let mut late = first.loan_response()?;
            late.sequence = cycle as u64;
            late.value = 30_000 + cycle as u64;
            let _ = late.send();
            late_tx.send(cycle).expect("late-response notification");

            let second = server
                .receive_timeout(Duration::from_secs(5))?
                .expect("follow-up request");
            let mut response = second.loan_response()?;
            response.sequence = 90_000 + cycle as u64;
            response.value = 40_000 + cycle as u64;
            response.send()?;
        }
        Ok(())
    });

    let client = RpcClient::<Frame, Detection>::open(&descriptor)?;
    for cycle in 0..CYCLES {
        let pending = client.loan_request()?.send()?;
        assert_eq!(
            ready_rx
                .recv_timeout(Duration::from_secs(5))
                .expect("server received timed-out request"),
            cycle
        );
        assert!(matches!(
            pending.wait(Duration::from_millis(1)),
            Err(RpcError::DeadlineExceeded)
        ));

        release_tx.send(cycle).expect("release late response");
        assert_eq!(
            late_rx
                .recv_timeout(Duration::from_secs(5))
                .expect("server sent late response"),
            cycle
        );

        let response = client.call_copy(marker(cycle, 50_000), Duration::from_secs(5))?;
        assert_eq!(
            (response.sequence, response.value),
            (90_000 + cycle as u64, 40_000 + cycle as u64),
            "follow-up response was correlated with the wrong native request"
        );
    }

    server_thread.join().expect("server thread")?;
    let metrics = client
        .pressure_snapshot()
        .client
        .expect("client pressure metrics");
    assert_eq!(metrics.calls_attempted, (CYCLES * 2) as u64);
    assert_eq!(metrics.calls_started, (CYCLES * 2) as u64);
    assert_eq!(metrics.calls_completed, CYCLES as u64);
    assert_eq!(metrics.calls_timed_out, CYCLES as u64);
    assert_eq!(metrics.calls_cancelled, 0);
    assert_eq!(metrics.calls_failed, 0);
    assert_eq!(metrics.in_flight_current, 0);
    assert_eq!(metrics.in_flight_high_water, 1);
    assert_eq!(metrics.call_duration_count, (CYCLES * 2) as u64);
    Ok(())
}

/// Cancellation and response delivery race on every cycle.  The pending call
/// has exactly one consumed terminal result (a response or `Cancelled`), and
/// a following request proves that the race did not poison the channel.
#[test]
fn thousand_cancellation_races_have_one_terminal_result_and_recover() -> Result<(), RpcError> {
    const RACES: usize = 1_000;
    let resources = RpcResourceConfig::default()
        .with_max_clients(1)
        .with_max_in_flight(1)
        .with_max_request_loans(2);
    let descriptor = descriptor(unique_service_name("cancel-race"), resources);
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let (ready_tx, ready_rx) = mpsc::channel();
    let (go_tx, go_rx) = mpsc::channel();
    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        for race in 0..RACES {
            let request = server
                .receive_timeout(Duration::from_secs(5))?
                .expect("racing request");
            ready_tx.send(race).expect("request notification");
            assert_eq!(
                go_rx.recv().expect("release response race"),
                race,
                "response release crossed cancellation races"
            );

            let mut response = request.loan_response()?;
            response.sequence = race as u64;
            response.value = 60_000 + race as u64;
            let _ = response.send();

            let follow_up = server
                .receive_timeout(Duration::from_secs(5))?
                .expect("follow-up request after cancellation race");
            let mut follow_up_response = follow_up.loan_response()?;
            follow_up_response.sequence = 90_000 + race as u64;
            follow_up_response.value = 70_000 + race as u64;
            follow_up_response.send()?;
        }
        Ok(())
    });

    let client = Arc::new(RpcClient::<Frame, Detection>::open(&descriptor)?);
    for race in 0..RACES {
        let pending = client.loan_request()?.send()?;
        assert_eq!(
            ready_rx
                .recv_timeout(Duration::from_secs(5))
                .expect("server received racing request"),
            race
        );
        let cancel = pending.cancel_handle();
        let barrier = Arc::new(Barrier::new(3));
        let waiter_barrier = Arc::clone(&barrier);
        let waiter = thread::spawn(move || {
            waiter_barrier.wait();
            pending.wait(Duration::from_secs(5))
        });
        let cancel_barrier = Arc::clone(&barrier);
        let canceller = thread::spawn(move || {
            cancel_barrier.wait();
            cancel.cancel()
        });
        barrier.wait();
        go_tx.send(race).expect("release response race");

        let cancellation_result = waiter.join().expect("RPC waiter thread");
        canceller.join().expect("RPC canceller thread")?;
        match cancellation_result {
            Ok(response) => {
                assert_eq!(
                    (response.sequence, response.value),
                    (race as u64, 60_000 + race as u64)
                );
            }
            Err(RpcError::Cancelled) => {}
            Err(error) => {
                panic!("cancellation race produced a second/unexpected terminal outcome: {error:?}")
            }
        }

        let follow_up = client.call_copy(marker(race, 80_000), Duration::from_secs(5))?;
        assert_eq!(
            (follow_up.sequence, follow_up.value),
            (90_000 + race as u64, 70_000 + race as u64),
            "follow-up response was routed to the wrong cancellation race"
        );
    }

    server_thread.join().expect("server thread")?;
    let metrics = client
        .pressure_snapshot()
        .client
        .expect("client pressure metrics");
    assert_eq!(metrics.in_flight_current, 0);
    assert_eq!(metrics.in_flight_high_water, 1);
    assert_eq!(metrics.calls_attempted, (RACES * 2) as u64);
    assert_eq!(metrics.calls_started, (RACES * 2) as u64);
    assert_eq!(
        metrics.calls_completed + metrics.calls_cancelled,
        (RACES * 2) as u64
    );
    assert_eq!(metrics.calls_failed, 0);
    Ok(())
}

/// A server that deliberately holds the first request must expose a bounded,
/// typed capacity failure for the second request from the same client.
#[test]
fn slow_server_reports_explicit_client_capacity_and_recovers() -> Result<(), RpcError> {
    let resources = RpcResourceConfig::default()
        // Keep two sidecar listener slots so the second attempted send can
        // reach native RequestResponse and report its typed active-call
        // ceiling instead of failing while opening the advisory listener.
        .with_max_clients(2)
        .with_max_in_flight(1)
        .with_max_request_loans(2);
    let descriptor = descriptor(unique_service_name("slow-capacity"), resources);
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let (ready_tx, ready_rx) = mpsc::channel();
    let (release_tx, release_rx) = mpsc::channel();
    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        let first = server
            .receive_timeout(Duration::from_secs(5))?
            .expect("slow server first request");
        ready_tx.send(()).expect("slow server ready");
        release_rx.recv().expect("release slow server request");
        drop(first);

        let second = server
            .receive_timeout(Duration::from_secs(5))?
            .expect("request after capacity release");
        let mut response = second.loan_response()?;
        response.sequence = 2;
        response.value = 22;
        response.send()
    });

    let client = RpcClient::<Frame, Detection>::open(&descriptor)?;
    let first = client.loan_request()?.send()?;
    ready_rx
        .recv_timeout(Duration::from_secs(5))
        .expect("server holds first request");

    let second = client.loan_request()?.send();
    assert!(
        matches!(&second, Err(RpcError::ClientCapacityReached)),
        "slow-server second send returned {second:?}"
    );

    release_tx.send(()).expect("release slow server request");
    drop(first);
    let response = client.call_copy(Frame::default(), Duration::from_secs(5))?;
    assert_eq!((response.sequence, response.value), (2, 22));
    server_thread.join().expect("server thread")?;

    let metrics = client
        .pressure_snapshot()
        .client
        .expect("client pressure metrics");
    assert_eq!(metrics.capacity_rejections, 1);
    assert_eq!(metrics.in_flight_current, 0);
    Ok(())
}

/// A slow client's outstanding request must not affect a fast client's
/// response correlation.  The server returns the fast response first and the
/// slow response only after an explicit release.
#[test]
fn fast_and_slow_clients_preserve_native_routing() -> Result<(), RpcError> {
    let resources = RpcResourceConfig::default()
        .with_max_clients(2)
        .with_max_in_flight(1)
        .with_max_request_loans(2);
    let descriptor = descriptor(unique_service_name("fast-slow"), resources);
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let (slow_ready_tx, slow_ready_rx) = mpsc::channel();
    let (fast_go_tx, fast_go_rx) = mpsc::channel();
    let (fast_done_tx, fast_done_rx) = mpsc::channel();
    let (slow_go_tx, slow_go_rx) = mpsc::channel();
    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        let slow = server
            .receive_timeout(Duration::from_secs(5))?
            .expect("slow client request");
        slow_ready_tx.send(()).expect("slow request notification");
        fast_go_rx.recv().expect("allow fast client request");

        let fast = server
            .receive_timeout(Duration::from_secs(5))?
            .expect("fast client request");
        let mut fast_response = fast.loan_response()?;
        fast_response.sequence = 400;
        fast_response.value = 4_004;
        fast_response.send()?;
        fast_done_tx.send(()).expect("fast response notification");

        slow_go_rx.recv().expect("allow slow response");
        let mut slow_response = slow.loan_response()?;
        slow_response.sequence = 300;
        slow_response.value = 3_003;
        slow_response.send()
    });

    let slow_client = RpcClient::<Frame, Detection>::open(&descriptor)?;
    let fast_client = RpcClient::<Frame, Detection>::open(&descriptor)?;
    let slow_pending = slow_client.loan_request()?.send()?;
    slow_ready_rx
        .recv_timeout(Duration::from_secs(5))
        .expect("server holds slow request");

    let slow_waiter = thread::spawn(move || slow_pending.wait(Duration::from_secs(5)));
    fast_go_tx.send(()).expect("allow fast client request");
    let fast_response = fast_client.call_copy(marker(4, 44_000), Duration::from_secs(5))?;
    assert_eq!((fast_response.sequence, fast_response.value), (400, 4_004));
    fast_done_rx
        .recv_timeout(Duration::from_secs(5))
        .expect("server sent fast response");

    slow_go_tx.send(()).expect("allow slow client response");
    let slow_response = slow_waiter.join().expect("slow waiter thread")?;
    assert_eq!((slow_response.sequence, slow_response.value), (300, 3_003));
    server_thread.join().expect("server thread")?;

    let slow_metrics = slow_client
        .pressure_snapshot()
        .client
        .expect("slow client pressure metrics");
    let fast_metrics = fast_client
        .pressure_snapshot()
        .client
        .expect("fast client pressure metrics");
    assert_eq!(slow_metrics.in_flight_current, 0);
    assert_eq!(fast_metrics.in_flight_current, 0);
    assert_eq!(slow_metrics.calls_completed, 1);
    assert_eq!(fast_metrics.calls_completed, 1);
    Ok(())
}

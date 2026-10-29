//! Deterministic fixed-size RPC data-plane checks.

use std::{
    collections::BTreeMap,
    sync::atomic::{AtomicU64, Ordering},
    sync::{Arc, Barrier, mpsc},
    thread,
    time::Duration,
};

use rbt_iox2::{
    Detection, Frame, RpcClient, RpcDescriptor, RpcError, RpcResourceConfig, RpcServer,
    RpcTransportFeatures, preflight_rpc,
};

static NEXT_SERVICE: AtomicU64 = AtomicU64::new(0);

fn unique_service_name() -> String {
    let sequence = NEXT_SERVICE.fetch_add(1, Ordering::Relaxed);
    format!(
        "rbt/rpc/00000000-0000-4000-8000-{:012x}/fixed",
        (u64::from(std::process::id()) << 16) | sequence
    )
}

fn descriptor() -> RpcDescriptor {
    RpcDescriptor::for_fixed::<Frame, Detection>(unique_service_name())
}

fn descriptor_with_resources(resources: RpcResourceConfig) -> RpcDescriptor {
    RpcDescriptor::new_with_resources(
        unique_service_name(),
        Frame::descriptor(),
        Detection::descriptor(),
        resources,
    )
}

#[test]
fn preflight_rejects_request_and_response_mismatch_before_transport() {
    let request_mismatch = RpcDescriptor::new_with_resources(
        unique_service_name(),
        Detection::descriptor(),
        Detection::descriptor(),
        RpcResourceConfig::default(),
    );
    assert!(matches!(
        preflight_rpc::<Frame, Detection>(&request_mismatch),
        Err(RpcError::AbiMismatch { .. })
    ));

    let response_mismatch = RpcDescriptor::new_with_resources(
        unique_service_name(),
        Frame::descriptor(),
        Frame::descriptor(),
        RpcResourceConfig::default(),
    );
    assert!(matches!(
        preflight_rpc::<Frame, Detection>(&response_mismatch),
        Err(RpcError::AbiMismatch { .. })
    ));

    let no_sidecar = RpcDescriptor::new_with_transport(
        unique_service_name(),
        Frame::descriptor(),
        Detection::descriptor(),
        RpcTransportFeatures::default(),
    );
    assert!(matches!(
        preflight_rpc::<Frame, Detection>(&no_sidecar),
        Err(RpcError::MissingTransportFeature { .. })
    ));
}

#[test]
fn fixed_zero_copy_roundtrip() -> Result<(), RpcError> {
    let descriptor = descriptor();
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let server_thread = thread::spawn(move || {
        let request = server
            .receive_timeout(Duration::from_secs(2))?
            .expect("request");
        let mut response = request.loan_response()?;
        response.sequence = 9;
        response.value = 42;
        response.send()
    });

    let client = RpcClient::<Frame, Detection>::open(&descriptor)?;
    let response = client.call_copy(
        Frame {
            sequence: 7,
            value: 11,
        },
        Duration::from_secs(2),
    )?;
    assert_eq!(response.sequence, 9);
    assert_eq!(response.value, 42);
    server_thread.join().expect("server thread")?;
    Ok(())
}

#[test]
fn dropped_request_loan_returns_capacity() -> Result<(), RpcError> {
    let resources = RpcResourceConfig {
        max_request_loans: 1,
        max_in_flight: 1,
        ..RpcResourceConfig::default()
    };
    let descriptor = RpcDescriptor::new_with_resources(
        unique_service_name(),
        Frame::descriptor(),
        Detection::descriptor(),
        resources,
    );
    let _server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let client = RpcClient::<Frame, Detection>::open(&descriptor)?;
    let first = client.loan_request()?;
    drop(first);
    let second = client.loan_request()?;
    drop(second);
    Ok(())
}

#[test]
fn active_call_capacity_is_typed() -> Result<(), RpcError> {
    let resources = RpcResourceConfig::default()
        .with_max_in_flight(1)
        .with_max_request_loans(2);
    let descriptor = RpcDescriptor::new_with_resources(
        unique_service_name(),
        Frame::descriptor(),
        Detection::descriptor(),
        resources,
    );
    let _server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let client = RpcClient::<Frame, Detection>::open(&descriptor)?;
    let first = client.loan_request()?.send()?;
    let second = client.loan_request()?.send();
    assert!(matches!(second, Err(RpcError::ClientCapacityReached)));
    let pressure = client.pressure_snapshot();
    let metrics = pressure.client.expect("client metrics");
    assert_eq!(metrics.calls_attempted, 2);
    assert_eq!(metrics.calls_started, 1);
    assert_eq!(metrics.calls_failed, 1);
    assert_eq!(metrics.capacity_rejections, 1);
    assert_eq!(metrics.in_flight_current, 1);
    drop(first);
    let metrics = client.pressure_snapshot().client.expect("client metrics");
    assert_eq!(metrics.calls_failed, 2);
    assert_eq!(metrics.in_flight_current, 0);
    assert_eq!(metrics.call_duration_count, 1);
    Ok(())
}

#[test]
fn dropped_response_loan_leads_to_bounded_timeout() -> Result<(), RpcError> {
    let descriptor = descriptor();
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        let request = server
            .receive_timeout(Duration::from_secs(2))?
            .expect("request");
        let response = request.loan_response()?;
        drop(response);
        Ok(())
    });
    let client = RpcClient::<Frame, Detection>::open(&descriptor)?;
    let error = client
        .call_copy(Frame::default(), Duration::from_millis(30))
        .expect_err("dropped response should not fabricate a value");
    assert!(matches!(error, RpcError::DeadlineExceeded));
    server_thread.join().expect("server thread")?;
    Ok(())
}

#[test]
fn server_shutdown_wakes_receive() -> Result<(), RpcError> {
    let descriptor = descriptor();
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let wake = server.wake_handle();
    let server_thread = thread::spawn(move || server.receive_timeout(Duration::from_secs(5)));
    thread::sleep(Duration::from_millis(30));
    wake.wake()?;
    let result = server_thread.join().expect("server thread");
    assert!(matches!(result, Err(RpcError::ActivationExpired)));
    Ok(())
}

#[test]
fn timeout_late_response_does_not_poison_next_call() -> Result<(), RpcError> {
    let descriptor = descriptor();
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        let first = server
            .receive_timeout(Duration::from_secs(2))?
            .expect("first request");
        thread::sleep(Duration::from_millis(100));
        let mut late = first.loan_response()?;
        late.sequence = 1;
        late.send().ok();

        let second = server
            .receive_timeout(Duration::from_secs(2))?
            .expect("second request");
        let mut response = second.loan_response()?;
        response.sequence = 2;
        response.send()
    });

    let client = RpcClient::<Frame, Detection>::open(&descriptor)?;
    let error = client
        .call_copy(Frame::default(), Duration::from_millis(20))
        .expect_err("first call should time out");
    assert!(matches!(error, RpcError::DeadlineExceeded));
    let response = client.call_copy(Frame::default(), Duration::from_secs(2))?;
    assert_eq!(response.sequence, 2);
    server_thread.join().expect("server thread")?;
    Ok(())
}

#[test]
fn cancellation_is_typed_and_next_call_works() -> Result<(), RpcError> {
    let descriptor = descriptor();
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let (received_tx, received_rx) = mpsc::channel();
    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        let first = server
            .receive_timeout(Duration::from_secs(2))?
            .expect("first request");
        received_tx.send(()).expect("receive notification");
        thread::sleep(Duration::from_millis(100));
        drop(first);
        let second = server
            .receive_timeout(Duration::from_secs(2))?
            .expect("second request");
        let mut response = second.loan_response()?;
        response.sequence = 3;
        response.send()
    });

    let client = RpcClient::<Frame, Detection>::open(&descriptor)?;
    let pending = client.loan_request()?.send()?;
    received_rx
        .recv_timeout(Duration::from_secs(2))
        .expect("server received first request");
    let cancel = pending.cancel_handle();
    cancel.cancel()?;
    let error = pending.wait(Duration::from_secs(2)).expect_err("cancelled");
    assert!(matches!(error, RpcError::Cancelled));
    let response = client.call_copy(Frame::default(), Duration::from_secs(2))?;
    assert_eq!(response.sequence, 3);
    server_thread.join().expect("server thread")?;
    let metrics = client.pressure_snapshot().client.expect("client metrics");
    assert_eq!(metrics.calls_attempted, 2);
    assert_eq!(metrics.calls_started, 2);
    assert_eq!(metrics.calls_completed, 1);
    assert_eq!(metrics.calls_cancelled, 1);
    assert_eq!(metrics.calls_failed, 0);
    assert_eq!(metrics.in_flight_current, 0);
    assert_eq!(metrics.call_duration_count, 2);
    Ok(())
}

#[test]
fn sixteen_clients_route_markers_and_isolate_cancellation() -> Result<(), RpcError> {
    const CLIENTS: usize = 16;
    let resources = RpcResourceConfig::default()
        .with_max_clients(CLIENTS)
        .with_max_in_flight(1)
        .with_max_request_loans(2);
    let descriptor = descriptor_with_resources(resources);
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let (received_tx, received_rx) = mpsc::channel();
    let (release_tx, release_rx) = mpsc::channel();
    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        let mut requests = Vec::with_capacity(CLIENTS);
        for _ in 0..CLIENTS {
            requests.push(
                server
                    .receive_timeout(Duration::from_secs(5))?
                    .expect("all client requests"),
            );
        }
        received_tx.send(()).expect("received notification");
        release_rx.recv().expect("release responses");
        for request in requests.into_iter().rev() {
            let marker = (request.sequence, request.value);
            let mut response = request.loan_response()?;
            response.sequence = marker.0;
            response.value = marker.1;
            let _ = response.send();
        }
        Ok(())
    });

    let clients = (0..CLIENTS)
        .map(|_| RpcClient::<Frame, Detection>::open(&descriptor).map(Arc::new))
        .collect::<Result<Vec<_>, _>>()?;
    let mut pending = Vec::with_capacity(CLIENTS);
    for (index, client) in clients.iter().enumerate() {
        let mut loan = client.loan_request()?;
        loan.sequence = index as u64;
        loan.value = 10_000 + index as u64;
        pending.push((index, loan.send()?));
    }
    received_rx
        .recv_timeout(Duration::from_secs(5))
        .expect("server received all clients");

    let barrier = Arc::new(Barrier::new(CLIENTS + 1));
    let mut waiters = Vec::with_capacity(CLIENTS);
    let mut cancel = None;
    for (index, call) in pending {
        if index == 0 {
            cancel = Some(call.cancel_handle());
        }
        let barrier = Arc::clone(&barrier);
        waiters.push((
            index,
            thread::spawn(move || {
                barrier.wait();
                call.wait(Duration::from_secs(3))
                    .map(|response| (response.sequence, response.value))
            }),
        ));
    }
    barrier.wait();
    cancel.expect("target cancellation handle").cancel()?;
    release_tx.send(()).expect("release responses");

    for (index, waiter) in waiters {
        let result = waiter.join().expect("client waiter");
        if index == 0 {
            assert!(matches!(result, Err(RpcError::Cancelled)));
        } else {
            assert_eq!(result?, (index as u64, 10_000 + index as u64));
        }
    }
    server_thread.join().expect("server thread")?;
    Ok(())
}

#[test]
fn one_shared_client_completes_concurrent_calls_out_of_order() -> Result<(), RpcError> {
    const CALLS: usize = 8;
    let resources = RpcResourceConfig::default()
        .with_max_clients(1)
        .with_max_in_flight(CALLS)
        .with_max_request_loans(CALLS);
    let descriptor = descriptor_with_resources(resources);
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        let mut requests = Vec::with_capacity(CALLS);
        for _ in 0..CALLS {
            requests.push(
                server
                    .receive_timeout(Duration::from_secs(5))?
                    .expect("all concurrent requests"),
            );
        }
        for request in requests.into_iter().rev() {
            let marker = (request.sequence, request.value);
            let mut response = request.loan_response()?;
            response.sequence = marker.0;
            response.value = marker.1;
            response.send()?;
        }
        Ok(())
    });

    let client = Arc::new(RpcClient::<Frame, Detection>::open(&descriptor)?);
    let barrier = Arc::new(Barrier::new(CALLS + 1));
    let (result_tx, result_rx) = mpsc::channel();
    let mut callers = Vec::with_capacity(CALLS);
    for index in 0..CALLS {
        let client = Arc::clone(&client);
        let barrier = Arc::clone(&barrier);
        let result_tx = result_tx.clone();
        callers.push(thread::spawn(move || {
            barrier.wait();
            let marker = (index as u64, 20_000 + index as u64);
            let result = client
                .call_copy(
                    Frame {
                        sequence: marker.0,
                        value: marker.1,
                    },
                    Duration::from_secs(5),
                )
                .map(|response| (response.sequence, response.value));
            result_tx.send((index, result)).expect("call result");
        }));
    }
    barrier.wait();
    drop(result_tx);
    for caller in callers {
        caller.join().expect("caller thread");
    }

    let mut results = BTreeMap::new();
    for (index, result) in result_rx {
        results.insert(index, result?);
    }
    assert_eq!(results.len(), CALLS);
    for index in 0..CALLS {
        assert_eq!(
            results[&index],
            (index as u64, 20_000 + index as u64),
            "marker routed to wrong concurrent call"
        );
    }
    server_thread.join().expect("server thread")?;
    Ok(())
}

#[test]
fn client_expiry_wakes_a_pending_call() -> Result<(), RpcError> {
    let descriptor = descriptor();
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let (received_tx, received_rx) = mpsc::channel();
    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        let request = server
            .receive_timeout(Duration::from_secs(2))?
            .expect("pending request");
        received_tx.send(()).expect("received notification");
        drop(request);
        Ok(())
    });
    let client = RpcClient::<Frame, Detection>::open(&descriptor)?;
    let pending = client.loan_request()?.send()?;
    received_rx
        .recv_timeout(Duration::from_secs(2))
        .expect("server received request");
    client.wake_handle().wake()?;
    assert!(matches!(
        pending.wait(Duration::from_secs(2)),
        Err(RpcError::ActivationExpired)
    ));
    server_thread.join().expect("server thread")?;
    Ok(())
}

#[test]
fn dropped_unanswered_request_releases_native_request() -> Result<(), RpcError> {
    let descriptor = descriptor();
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let (dropped_tx, dropped_rx) = mpsc::channel();
    let server_thread = thread::spawn(move || {
        let first = server
            .receive_timeout(Duration::from_secs(2))?
            .expect("first request");
        drop(first);
        dropped_tx.send(()).expect("dropped notification");
        let second = server
            .receive_timeout(Duration::from_secs(2))?
            .expect("second request");
        let mut response = second.loan_response()?;
        response.sequence = 77;
        response.value = 88;
        response.send()?;
        Ok::<_, RpcError>(server.pressure_snapshot())
    });

    let client = RpcClient::<Frame, Detection>::open(&descriptor)?;
    let first = client.loan_request()?.send()?;
    assert!(matches!(
        first.wait(Duration::from_millis(20)),
        Err(RpcError::DeadlineExceeded)
    ));
    dropped_rx
        .recv_timeout(Duration::from_secs(2))
        .expect("server dropped first request");
    let response = client.call_copy(Frame::default(), Duration::from_secs(2))?;
    assert_eq!((response.sequence, response.value), (77, 88));
    let client_pressure = client.pressure_snapshot();
    let client_metrics = client_pressure.client.expect("client metrics");
    assert_eq!(client_metrics.calls_attempted, 2);
    assert_eq!(client_metrics.calls_started, 2);
    assert_eq!(client_metrics.calls_completed, 1);
    assert_eq!(client_metrics.calls_timed_out, 1);
    assert_eq!(client_metrics.in_flight_current, 0);
    assert_eq!(client_metrics.in_flight_high_water, 1);
    assert_eq!(client_metrics.call_duration_count, 2);

    let server_pressure = server_thread.join().expect("server thread")?;
    let server_metrics = server_pressure.server.expect("server metrics");
    assert_eq!(server_metrics.requests_received, 2);
    assert_eq!(server_metrics.requests_current, 0);
    assert_eq!(server_metrics.requests_high_water, 1);
    assert_eq!(server_metrics.responses_attempted, 1);
    assert_eq!(server_metrics.responses_sent, 1);
    assert_eq!(server_metrics.responses_failed, 0);
    assert_eq!(server_metrics.responses_abandoned, 1);
    Ok(())
}

#[test]
fn repeated_late_responses_do_not_poison_reused_channels() -> Result<(), RpcError> {
    const CYCLES: usize = 32;
    let resources = RpcResourceConfig::default()
        .with_max_clients(1)
        .with_max_in_flight(1)
        .with_max_request_loans(2);
    let descriptor = descriptor_with_resources(resources);
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let (ready_tx, ready_rx) = mpsc::channel();
    let (release_tx, release_rx) = mpsc::channel();
    let (late_tx, late_rx) = mpsc::channel();
    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        for cycle in 0..CYCLES {
            let first = server
                .receive_timeout(Duration::from_secs(2))?
                .expect("timed-out request");
            ready_tx.send(()).expect("request notification");
            release_rx.recv().expect("release late response");
            let mut late = first.loan_response()?;
            late.sequence = cycle as u64;
            late.value = 30_000 + cycle as u64;
            let _ = late.send();
            late_tx.send(()).expect("late response notification");

            let second = server
                .receive_timeout(Duration::from_secs(2))?
                .expect("live follow-up request");
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
        ready_rx
            .recv_timeout(Duration::from_secs(2))
            .expect("server received timed-out request");
        assert!(matches!(
            pending.wait(Duration::from_millis(5)),
            Err(RpcError::DeadlineExceeded)
        ));
        release_tx.send(()).expect("release late response");
        late_rx
            .recv_timeout(Duration::from_secs(2))
            .expect("server sent late response");

        let response = client.call_copy(
            Frame {
                sequence: cycle as u64,
                value: 50_000 + cycle as u64,
            },
            Duration::from_secs(2),
        )?;
        assert_eq!(
            (response.sequence, response.value),
            (90_000 + cycle as u64, 40_000 + cycle as u64)
        );
    }
    server_thread.join().expect("server thread")?;
    Ok(())
}

#[test]
fn repeated_cancel_response_races_have_one_terminal_result() -> Result<(), RpcError> {
    const RACES: usize = 16;
    let descriptor = descriptor_with_resources(
        RpcResourceConfig::default()
            .with_max_clients(1)
            .with_max_in_flight(1)
            .with_max_request_loans(2),
    );
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let (ready_tx, ready_rx) = mpsc::channel();
    let (barrier_tx, barrier_rx) = mpsc::channel::<Arc<Barrier>>();
    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        for _ in 0..RACES {
            let request = server
                .receive_timeout(Duration::from_secs(2))?
                .expect("racing request");
            ready_tx.send(()).expect("request notification");
            let barrier = barrier_rx.recv().expect("race barrier");
            barrier.wait();
            let marker = (request.sequence, request.value);
            let mut response = request.loan_response()?;
            response.sequence = marker.0;
            response.value = marker.1;
            let _ = response.send();
        }
        Ok(())
    });

    let client = RpcClient::<Frame, Detection>::open(&descriptor)?;
    for race in 0..RACES {
        let mut loan = client.loan_request()?;
        loan.sequence = race as u64;
        loan.value = 60_000 + race as u64;
        let pending = loan.send()?;
        ready_rx
            .recv_timeout(Duration::from_secs(2))
            .expect("server received racing request");
        let cancel = pending.cancel_handle();
        let barrier = Arc::new(Barrier::new(4));
        barrier_tx
            .send(Arc::clone(&barrier))
            .expect("send race barrier");
        let waiter_barrier = Arc::clone(&barrier);
        let waiter = thread::spawn(move || {
            waiter_barrier.wait();
            pending
                .wait(Duration::from_secs(2))
                .map(|response| (response.sequence, response.value))
        });
        let cancel_barrier = Arc::clone(&barrier);
        let canceller = thread::spawn(move || {
            cancel_barrier.wait();
            cancel.cancel()
        });
        barrier.wait();
        canceller.join().expect("canceller thread")?;
        match waiter.join().expect("waiter thread") {
            Ok(marker) => assert_eq!(marker, (race as u64, 60_000 + race as u64)),
            Err(RpcError::Cancelled) => {}
            Err(error) => panic!("unexpected cancellation race result: {error:?}"),
        }
    }
    server_thread.join().expect("server thread")?;
    Ok(())
}

#[test]
fn repeated_deadline_response_races_have_one_terminal_result() -> Result<(), RpcError> {
    const RACES: usize = 16;
    let descriptor = descriptor_with_resources(
        RpcResourceConfig::default()
            .with_max_clients(1)
            .with_max_in_flight(1)
            .with_max_request_loans(2),
    );
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let (ready_tx, ready_rx) = mpsc::channel();
    let (barrier_tx, barrier_rx) = mpsc::channel::<Arc<Barrier>>();
    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        for _ in 0..RACES {
            let request = server
                .receive_timeout(Duration::from_secs(2))?
                .expect("deadline-racing request");
            ready_tx.send(()).expect("request notification");
            let barrier = barrier_rx.recv().expect("race barrier");
            barrier.wait();
            let marker = (request.sequence, request.value);
            let mut response = request.loan_response()?;
            response.sequence = marker.0;
            response.value = marker.1;
            let _ = response.send();
        }
        Ok(())
    });

    let client = RpcClient::<Frame, Detection>::open(&descriptor)?;
    for race in 0..RACES {
        let mut loan = client.loan_request()?;
        loan.sequence = race as u64;
        loan.value = 70_000 + race as u64;
        let pending = loan.send()?;
        ready_rx
            .recv_timeout(Duration::from_secs(2))
            .expect("server received deadline-racing request");
        let barrier = Arc::new(Barrier::new(3));
        barrier_tx
            .send(Arc::clone(&barrier))
            .expect("send race barrier");
        let waiter_barrier = Arc::clone(&barrier);
        let waiter = thread::spawn(move || {
            waiter_barrier.wait();
            pending
                .wait(Duration::from_millis(1))
                .map(|response| (response.sequence, response.value))
        });
        barrier.wait();
        match waiter.join().expect("waiter thread") {
            Ok(marker) => assert_eq!(marker, (race as u64, 70_000 + race as u64)),
            Err(RpcError::DeadlineExceeded) => {}
            Err(error) => panic!("unexpected deadline race result: {error:?}"),
        }
    }
    server_thread.join().expect("server thread")?;
    Ok(())
}

//! Native RPC resource-bound checks for the pinned iceoryx2 transport.

use std::{
    sync::{
        atomic::{AtomicU64, Ordering},
        mpsc,
    },
    thread,
    time::{Duration, Instant},
};

use rbt_iox2::{
    Detection, Frame, RpcClient, RpcDescriptor, RpcError, RpcResourceConfig, RpcServer,
};

const OPERATION_TIMEOUT: Duration = Duration::from_secs(5);
const FIXED_CALL_WATCHDOG: Duration = Duration::from_secs(300);

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

#[test]
fn request_loan_pool_exhaustion_is_counted_and_recovers_after_drop() -> Result<(), RpcError> {
    let resources = RpcResourceConfig::default()
        .with_max_clients(1)
        .with_max_in_flight(1)
        .with_max_request_loans(1);
    let descriptor = descriptor(unique_service_name("request-loans"), resources);
    let _server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let client = RpcClient::<Frame, Detection>::open(&descriptor)?;

    let held = client.loan_request()?;
    assert!(matches!(
        client.loan_request(),
        Err(RpcError::LoanPoolExhausted(_))
    ));

    let failed = client.pressure_snapshot().client.expect("client metrics");
    assert_eq!(failed.request_loan_failures, 1);
    assert_eq!(failed.calls_attempted, 0);

    drop(held);
    drop(client.loan_request()?);
    let recovered = client.pressure_snapshot().client.expect("client metrics");
    assert_eq!(recovered.request_loan_failures, 1);
    Ok(())
}

#[test]
fn stale_server_request_cannot_satisfy_reopened_clients_first_pending_call() -> Result<(), RpcError>
{
    let resources = RpcResourceConfig::default()
        .with_max_clients(1)
        .with_max_in_flight(1)
        .with_max_request_loans(1);
    let descriptor = descriptor(unique_service_name("stale-client-reopen"), resources);
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let (old_received_tx, old_received_rx) = mpsc::channel();
    let (stale_send_tx, stale_send_rx) = mpsc::channel();

    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        let old_request = server
            .receive_timeout(OPERATION_TIMEOUT)?
            .expect("old client's request");
        old_received_tx
            .send(())
            .expect("notify old request received");

        // Keep the old native ActiveRequest alive while receive refreshes the
        // server's client connection slot for the replacement client.
        let fresh_request = server
            .receive_timeout(OPERATION_TIMEOUT)?
            .expect("replacement client's request");

        let mut stale_response = old_request.loan_response()?;
        stale_response.sequence = 0xaaaa;
        stale_response.value = 0xdead_beef;
        let stale_send = stale_response.send().map_err(|error| error.to_string());
        stale_send_tx
            .send(stale_send)
            .expect("report stale response send result");

        let mut fresh_response = fresh_request.loan_response()?;
        fresh_response.sequence = 0xbbbb;
        fresh_response.value = 0xcafe_babe;
        fresh_response.send()?;
        Ok(())
    });

    let old_client = RpcClient::<Frame, Detection>::open(&descriptor)?;
    let mut old_request = old_client.loan_request()?;
    old_request.sequence = 1;
    old_request.value = 10;
    let old_pending = old_request.send()?;
    old_received_rx
        .recv_timeout(OPERATION_TIMEOUT)
        .expect("server retained old request");
    drop(old_pending);
    drop(old_client);

    let fresh_client = RpcClient::<Frame, Detection>::open(&descriptor)?;
    let mut fresh_request = fresh_client.loan_request()?;
    fresh_request.sequence = 2;
    fresh_request.value = 20;
    let fresh_pending = fresh_request.send()?;
    let response = fresh_pending.wait(OPERATION_TIMEOUT)?;
    let stale_send = stale_send_rx
        .recv_timeout(OPERATION_TIMEOUT)
        .expect("server attempted stale response");
    eprintln!("stale response send result: {stale_send:?}");

    assert_eq!(
        (response.sequence, response.value),
        (0xbbbb, 0xcafe_babe),
        "replacement client's request accepted the stale response"
    );
    server_thread.join().expect("RPC server thread")?;
    Ok(())
}

/// Minimal upstream-only reproducer: no rbt endpoint, event sidecar, terminal
/// arbiter, pressure tracker, or application-level correlation is involved.
#[test]
fn native_stale_response_cannot_cross_client_connection_reuse()
-> Result<(), Box<dyn std::error::Error>> {
    use iceoryx2::prelude::*;

    let node = NodeBuilder::new()
        .signal_handling_mode(SignalHandlingMode::Disabled)
        .create::<ipc_threadsafe::Service>()?;
    let name: ServiceName = unique_service_name("native-stale-client-reopen")
        .as_str()
        .try_into()?;
    let service = node
        .service_builder(&name)
        .request_response::<Frame, Detection>()
        .max_clients(1)
        .max_servers(1)
        .max_active_requests_per_client(1)
        .max_response_buffer_size(1)
        .max_borrowed_responses_per_pending_response(1)
        .enable_safe_overflow_for_requests(false)
        .enable_safe_overflow_for_responses(false)
        .enable_fire_and_forget_requests(false)
        .create()?;
    let server = service
        .server_builder()
        .backpressure_strategy(BackpressureStrategy::DiscardData)
        .create()?;
    let old_client = service
        .client_builder()
        .backpressure_strategy(BackpressureStrategy::DiscardData)
        .create()?;
    let old_pending = old_client.send_copy(Frame {
        sequence: 1,
        value: 10,
    })?;
    let old_request = server.receive()?.expect("old native request");
    let old_client_id = old_request.header().client_id();
    drop(old_pending);
    drop(old_client);

    let fresh_client = service
        .client_builder()
        .backpressure_strategy(BackpressureStrategy::DiscardData)
        .create()?;
    let fresh_pending = fresh_client.send_copy(Frame {
        sequence: 2,
        value: 20,
    })?;
    let fresh_request = server.receive()?.expect("fresh native request");
    assert_ne!(old_client_id, fresh_request.header().client_id());

    old_request.send_copy(Detection {
        sequence: 0xaaaa,
        value: 0xdead_beef,
    })?;
    let first_response = fresh_pending.receive()?;
    assert!(
        first_response.is_none(),
        "native fresh pending request accepted an old client's response: {:?}",
        first_response
            .as_ref()
            .map(|response| (response.sequence, response.value))
    );

    fresh_request.send_copy(Detection {
        sequence: 0xbbbb,
        value: 0xcafe_babe,
    })?;
    let response = fresh_pending.receive()?.expect("fresh native response");
    assert_eq!((response.sequence, response.value), (0xbbbb, 0xcafe_babe));
    Ok(())
}

#[test]
fn in_flight_limit_is_typed_and_recovers_without_server_capacity_rejection() -> Result<(), RpcError>
{
    let resources = RpcResourceConfig::default()
        .with_max_clients(1)
        .with_max_in_flight(1)
        .with_max_request_loans(2);
    let descriptor = descriptor(unique_service_name("in-flight"), resources);
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let (received_tx, received_rx) = mpsc::channel();
    let (release_tx, release_rx) = mpsc::channel();

    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        let first = server
            .receive_timeout(OPERATION_TIMEOUT)?
            .expect("first request reached server");
        received_tx.send(()).expect("notify first request received");
        release_rx
            .recv_timeout(OPERATION_TIMEOUT)
            .expect("release first request");
        drop(first);

        let second = server
            .receive_timeout(OPERATION_TIMEOUT)?
            .expect("recovered request reached server");
        let sequence = second.sequence;
        let value = second.value;
        let mut response = second.loan_response()?;
        response.sequence = sequence;
        response.value = value + 1;
        response.send()?;

        let metrics = server.pressure_snapshot().server.expect("server metrics");
        assert_eq!(metrics.requests_received, 2);
        assert_eq!(metrics.responses_sent, 1);
        Ok(())
    });

    let client = RpcClient::<Frame, Detection>::open(&descriptor)?;
    let mut first_loan = client.loan_request()?;
    first_loan.sequence = 1;
    first_loan.value = 10;
    let first = first_loan.send()?;
    received_rx
        .recv_timeout(OPERATION_TIMEOUT)
        .expect("server holds first request");

    let rejected = client.loan_request()?.send();
    assert!(matches!(rejected, Err(RpcError::ClientCapacityReached)));
    let saturated = client.pressure_snapshot().client.expect("client metrics");
    assert_eq!(saturated.calls_attempted, 2);
    assert_eq!(saturated.calls_started, 1);
    assert_eq!(saturated.capacity_rejections, 1);
    assert_eq!(saturated.server_capacity_rejections, 0);
    assert_eq!(saturated.in_flight_current, 1);

    drop(first);
    let mut recovered_loan = client.loan_request()?;
    recovered_loan.sequence = 2;
    recovered_loan.value = 20;
    let recovered_call = recovered_loan.send()?;
    release_tx.send(()).expect("release held server request");
    let response = recovered_call.wait(OPERATION_TIMEOUT)?;
    assert_eq!((response.sequence, response.value), (2, 21));

    server_thread.join().expect("RPC server thread")?;
    let metrics = client.pressure_snapshot().client.expect("client metrics");
    assert_eq!(metrics.calls_attempted, 3);
    assert_eq!(metrics.calls_started, 2);
    assert_eq!(metrics.calls_completed, 1);
    assert_eq!(metrics.capacity_rejections, 1);
    assert_eq!(metrics.server_capacity_rejections, 0);
    assert_eq!(metrics.in_flight_current, 0);
    Ok(())
}

#[test]
#[cfg_attr(
    debug_assertions,
    ignore = "100000-call workload is gated in release by scripts/verify_rpc.sh"
)]
fn one_hundred_thousand_small_fixed_calls_complete_with_native_response_capacity_one()
-> Result<(), RpcError> {
    const CALLS: usize = 100_000;

    let resources = RpcResourceConfig::default()
        .with_max_clients(1)
        .with_max_in_flight(1)
        .with_max_request_loans(1)
        .with_response_buffer_size(1)
        .with_max_borrowed_responses(1)
        .with_max_response_loans(1);
    let descriptor = descriptor(unique_service_name("100k-fixed"), resources);
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;

    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        for _ in 0..CALLS {
            let request = server
                .receive_timeout(OPERATION_TIMEOUT)?
                .expect("request arrived before watchdog");
            let sequence = request.sequence;
            let value = request.value;
            let mut response = request.loan_response()?;
            response.sequence = sequence;
            response.value = value.wrapping_add(1);
            response.send()?;
        }
        let metrics = server.pressure_snapshot().server.expect("server metrics");
        assert_eq!(metrics.requests_received, CALLS as u64);
        assert_eq!(metrics.responses_sent, CALLS as u64);
        Ok(())
    });

    let client = RpcClient::<Frame, Detection>::open(&descriptor)?;
    let started = Instant::now();
    for index in 0..CALLS {
        assert!(
            started.elapsed() < FIXED_CALL_WATCHDOG,
            "100000-call integration workload exceeded {FIXED_CALL_WATCHDOG:?}"
        );
        let request = Frame {
            sequence: index as u64,
            value: (index as u64).wrapping_mul(3),
        };
        let response = client.call_copy(request, OPERATION_TIMEOUT)?;
        assert_eq!(response.sequence, request.sequence);
        assert_eq!(response.value, request.value.wrapping_add(1));
    }

    server_thread.join().expect("RPC server thread")?;
    let metrics = client.pressure_snapshot().client.expect("client metrics");
    assert_eq!(metrics.calls_attempted, CALLS as u64);
    assert_eq!(metrics.calls_started, CALLS as u64);
    assert_eq!(metrics.calls_completed, CALLS as u64);
    assert_eq!(metrics.in_flight_current, 0);
    Ok(())
}

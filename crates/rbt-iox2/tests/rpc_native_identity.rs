//! Native iceoryx2 RPC identity regressions without the rbt wrapper.

use std::{
    sync::atomic::{AtomicU64, Ordering},
    thread,
    time::{Duration, Instant},
};

use iceoryx2::prelude::*;

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, ZeroCopySend)]
struct Frame {
    sequence: u64,
    value: u64,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, ZeroCopySend)]
struct Detection {
    sequence: u64,
    value: u64,
}

const OPERATION_TIMEOUT: Duration = Duration::from_secs(5);
const STALE_RESPONSE_WINDOW: Duration = Duration::from_secs(1);
const POLL_INTERVAL: Duration = Duration::from_millis(1);

static NEXT_SERVICE: AtomicU64 = AtomicU64::new(0);

fn unique_service_name(test: &str) -> String {
    let sequence = NEXT_SERVICE.fetch_add(1, Ordering::Relaxed);
    let value = (u64::from(std::process::id()) << 20) | (sequence & 0x000f_ffff);
    format!("rbt/rpc-native/00000000-0000-4000-8000-{value:012x}/{test}")
}

fn receive_before_deadline<T, E: std::fmt::Debug>(
    timeout: Duration,
    mut receive: impl FnMut() -> Result<Option<T>, E>,
) -> Result<T, E> {
    let deadline = Instant::now() + timeout;
    loop {
        if let Some(value) = receive()? {
            return Ok(value);
        }

        let remaining = deadline.saturating_duration_since(Instant::now());
        assert!(
            !remaining.is_zero(),
            "timed out waiting for native RPC item"
        );
        thread::sleep(POLL_INTERVAL.min(remaining));
    }
}

fn assert_no_response_until<T: std::fmt::Debug, E: std::fmt::Debug>(
    timeout: Duration,
    mut receive: impl FnMut() -> Result<Option<T>, E>,
) -> Result<(), E> {
    let deadline = Instant::now() + timeout;
    loop {
        if let Some(response) = receive()? {
            panic!("stale native response reached replacement pending request: {response:?}");
        }

        let remaining = deadline.saturating_duration_since(Instant::now());
        if remaining.is_zero() {
            return Ok(());
        }
        thread::sleep(POLL_INTERVAL.min(remaining));
    }
}

#[test]
fn fixed_uninit_response_keeps_original_client_identity_after_reuse()
-> Result<(), Box<dyn std::error::Error>> {
    let node = NodeBuilder::new()
        .signal_handling_mode(SignalHandlingMode::Disabled)
        .create::<ipc_threadsafe::Service>()?;
    let name: ServiceName = unique_service_name("fixed-uninit").as_str().try_into()?;
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
    let old_client_id = old_client.id();
    // This is each client's only request, so both requests have request ID 0.
    let old_pending = old_client.send_copy(Frame {
        sequence: 1,
        value: 10,
    })?;
    let old_active = receive_before_deadline(OPERATION_TIMEOUT, || server.receive())?;
    assert_eq!(old_active.header().client_id(), old_client_id);
    let old_response_uninit = old_active.loan_uninit()?;
    drop(old_pending);
    drop(old_client);

    let fresh_client = service
        .client_builder()
        .backpressure_strategy(BackpressureStrategy::DiscardData)
        .create()?;
    let fresh_client_id = fresh_client.id();
    assert_ne!(old_client_id, fresh_client_id);
    let fresh_pending = fresh_client.send_copy(Frame {
        sequence: 2,
        value: 20,
    })?;
    let fresh_active = receive_before_deadline(OPERATION_TIMEOUT, || server.receive())?;
    assert_eq!(fresh_active.header().client_id(), fresh_client_id);
    assert!(!old_active.is_connected());
    assert!(fresh_active.is_connected());

    drop(old_active);
    assert!(
        fresh_active.is_connected(),
        "dropping the stale request closed the replacement client's channel"
    );

    old_response_uninit
        .write_payload(Detection {
            sequence: 0xaaaa,
            value: 0xdead_beef,
        })
        .send()?;
    assert_no_response_until(STALE_RESPONSE_WINDOW, || fresh_pending.receive())?;

    fresh_active
        .loan_uninit()?
        .write_payload(Detection {
            sequence: 0xbbbb,
            value: 0xcafe_babe,
        })
        .send()?;
    let response = receive_before_deadline(OPERATION_TIMEOUT, || fresh_pending.receive())?;
    assert_eq!((response.sequence, response.value), (0xbbbb, 0xcafe_babe));
    Ok(())
}

#[test]
fn dynamic_slice_uninit_response_keeps_original_client_identity_after_reuse()
-> Result<(), Box<dyn std::error::Error>> {
    const RESPONSE_LEN: usize = 2;

    let node = NodeBuilder::new()
        .signal_handling_mode(SignalHandlingMode::Disabled)
        .create::<ipc_threadsafe::Service>()?;
    let name: ServiceName = unique_service_name("slice-uninit").as_str().try_into()?;
    let service = node
        .service_builder(&name)
        .request_response::<Frame, [Detection]>()
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
        .initial_max_slice_len(RESPONSE_LEN)
        .backpressure_strategy(BackpressureStrategy::DiscardData)
        .create()?;

    let old_client = service
        .client_builder()
        .backpressure_strategy(BackpressureStrategy::DiscardData)
        .create()?;
    let old_client_id = old_client.id();
    // This is each client's only request, so both requests have request ID 0.
    let old_pending = old_client.send_copy(Frame {
        sequence: 1,
        value: 10,
    })?;
    let old_active = receive_before_deadline(OPERATION_TIMEOUT, || server.receive())?;
    assert_eq!(old_active.header().client_id(), old_client_id);
    let old_response_uninit = old_active.loan_slice_uninit(RESPONSE_LEN)?;
    drop(old_pending);
    drop(old_client);

    let fresh_client = service
        .client_builder()
        .backpressure_strategy(BackpressureStrategy::DiscardData)
        .create()?;
    let fresh_client_id = fresh_client.id();
    assert_ne!(old_client_id, fresh_client_id);
    let fresh_pending = fresh_client.send_copy(Frame {
        sequence: 2,
        value: 20,
    })?;
    let fresh_active = receive_before_deadline(OPERATION_TIMEOUT, || server.receive())?;
    assert_eq!(fresh_active.header().client_id(), fresh_client_id);
    assert!(!old_active.is_connected());
    assert!(fresh_active.is_connected());

    drop(old_active);
    assert!(
        fresh_active.is_connected(),
        "dropping the stale request closed the replacement client's channel"
    );

    old_response_uninit
        .write_from_fn(|index| Detection {
            sequence: 0xaaaa + index as u64,
            value: 0xdead_beef,
        })
        .send()?;
    assert_no_response_until(STALE_RESPONSE_WINDOW, || fresh_pending.receive())?;

    fresh_active
        .loan_slice_uninit(RESPONSE_LEN)?
        .write_from_fn(|index| Detection {
            sequence: 0xbbbb + index as u64,
            value: 0xcafe_babe + index as u64,
        })
        .send()?;
    let response = receive_before_deadline(OPERATION_TIMEOUT, || fresh_pending.receive())?;
    assert_eq!(
        response
            .payload()
            .iter()
            .map(|detection| (detection.sequence, detection.value))
            .collect::<Vec<_>>(),
        vec![(0xbbbb, 0xcafe_babe), (0xbbbc, 0xcafe_babf)]
    );
    Ok(())
}

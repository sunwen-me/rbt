//! StampedBytes keeps time evidence in the same zero-copy request/response
//! slices and remains independent of external clock discontinuities.

use std::{
    sync::atomic::{AtomicU64, Ordering},
    sync::mpsc,
    thread,
    time::{Duration, Instant},
};

use rbt_iox2::{Image, RpcDescriptor, RpcError, RpcResourceConfig, SliceRpc};
use rbt_time::{
    ClockDiscontinuity, ClockIdentity, ClockKind, ClockMapping, ClockMappingCalibration,
    ClockSourceId, DeviceClock, SensorTiming, TimeError, Timestamp,
};
use rbt_time_stream::{StampedBytes, TIMING_PREFIX_LEN, encode_timing_into, split_payload};

static NEXT_SERVICE: AtomicU64 = AtomicU64::new(0);

fn service_name(suffix: &str) -> String {
    let sequence = NEXT_SERVICE.fetch_add(1, Ordering::Relaxed);
    format!(
        "rbt/rpc/00000000-0000-4000-8000-{:012x}/stamped-{suffix}",
        (u64::from(std::process::id()) << 16) | sequence
    )
}

fn host_identity(seed: u8) -> ClockIdentity {
    let mut source = [0_u8; 48];
    source[..16].fill(seed);
    source[16..24].copy_from_slice(&1_u64.to_le_bytes());
    source[24..32].copy_from_slice(&2_u64.to_le_bytes());
    ClockIdentity::new(
        ClockKind::HostMonotonic,
        ClockSourceId::from_bytes(source).unwrap(),
        0,
    )
    .unwrap()
}

fn realtime_identity(seed: u8, generation: u64) -> ClockIdentity {
    let mut source = [0_u8; 48];
    source[..16].fill(seed);
    ClockIdentity::new(
        ClockKind::SystemRealtime,
        ClockSourceId::from_bytes(source).unwrap(),
        generation,
    )
    .unwrap()
}

fn timing(capture_ns: i64, received_ns: i64, generation: u64) -> SensorTiming {
    let source = ClockSourceId::from_device_id([0x42; 16]).unwrap();
    let clock = ClockIdentity::new(ClockKind::Device, source, generation).unwrap();
    SensorTiming::new(
        Some(Timestamp::new(clock, capture_ns)),
        Timestamp::new(host_identity(3), received_ns),
        Some(2),
        Some(4),
    )
    .unwrap()
}

fn mapping(source: ClockIdentity, target: ClockIdentity) -> ClockMapping {
    ClockMapping::new(ClockMappingCalibration {
        source,
        target,
        source_anchor_ns: 0,
        target_anchor_ns: 0,
        rate_numerator: 1,
        rate_denominator: 1,
        valid_from_ns: 0,
        valid_until_ns: 10_000,
        calibration_id: [0x55; 16],
        calibrated_at: Timestamp::new(host_identity(4), 1),
        expires_at: Timestamp::new(host_identity(4), 10_000),
        uncertainty_ns: Some(0),
    })
    .unwrap()
}

fn write_payload(payload: &mut [u8], timing: &SensorTiming, application: &[u8]) {
    assert_eq!(payload.len(), TIMING_PREFIX_LEN + application.len());
    encode_timing_into(timing, &mut payload[..TIMING_PREFIX_LEN]).unwrap();
    payload[TIMING_PREFIX_LEN..].copy_from_slice(application);
}

#[test]
fn stamped_slice_rpc_preserves_timing_and_rpc_deadlines_ignore_clock_steps() -> Result<(), RpcError>
{
    const REQUEST_APPLICATION: &[u8] = b"request";
    const RESPONSE_APPLICATION: &[u8] = b"response";
    let descriptor = RpcDescriptor::for_slice_with_resources::<StampedBytes, StampedBytes>(
        service_name("slice-rpc"),
        RpcResourceConfig::new(1, 1, 1, 1, 1, 1, 256, 256),
    );
    let mut server = SliceRpc::<StampedBytes, StampedBytes>::server(&descriptor)?;
    let (first_request_tx, first_request_rx) = mpsc::channel();
    let (release_first_tx, release_first_rx) = mpsc::channel();
    let (first_dropped_tx, first_dropped_rx) = mpsc::channel();
    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        let first = server
            .receive_timeout(Duration::from_secs(2))?
            .expect("first stamped RPC request");
        assert_eq!(*first.user_header(), 10);
        let (received_timing, received_application) = split_payload(&first).unwrap();
        assert_eq!(received_timing, timing(100, 125, 0));
        assert_eq!(received_application, REQUEST_APPLICATION);
        first_request_tx
            .send(())
            .expect("notify first request received");

        release_first_rx
            .recv_timeout(Duration::from_secs(2))
            .expect("release timed-out request");
        drop(first);
        first_dropped_tx
            .send(())
            .expect("notify first request dropped");

        let request = server
            .receive_timeout(Duration::from_secs(2))?
            .expect("healthy follow-up stamped RPC request");
        assert_eq!(*request.user_header(), 20);
        let (received_timing, received_application) = split_payload(&request).unwrap();
        assert_eq!(received_timing, timing(200, 225, 0));
        assert_eq!(received_application, REQUEST_APPLICATION);

        let response_timing = timing(300, 325, 0);
        let mut response =
            request.loan_response_slice(TIMING_PREFIX_LEN + RESPONSE_APPLICATION.len())?;
        *response.user_header_mut() = 30;
        write_payload(&mut response, &response_timing, RESPONSE_APPLICATION);
        response.send()
    });

    let client = SliceRpc::<StampedBytes, StampedBytes>::client(&descriptor)?;
    let mut timed_request =
        client.loan_request_slice(TIMING_PREFIX_LEN + REQUEST_APPLICATION.len())?;
    *timed_request.user_header_mut() = 10;
    write_payload(
        &mut timed_request,
        &timing(100, 125, 0),
        REQUEST_APPLICATION,
    );
    let pending = timed_request.send()?;
    first_request_rx
        .recv_timeout(Duration::from_secs(2))
        .expect("server received first request");

    let device_clock = DeviceClock::new([0x11; 16], 0).unwrap();
    let device_mapping = mapping(device_clock.identity(), realtime_identity(0x22, 0));
    let realtime_source = DeviceClock::new([0x33; 16], 0).unwrap();
    let realtime_before = realtime_identity(0x44, 0);
    let realtime_after = realtime_identity(0x44, 1);
    let realtime_mapping = mapping(realtime_source.identity(), realtime_before);
    // These are explicit synthetic observations: the realtime reading moved
    // backward while its generation advanced. The test never changes the OS clock.
    let before_step = Timestamp::new(realtime_before, 1_000);
    let after_step = Timestamp::new(realtime_after, 900);
    assert_eq!(before_step.nanoseconds(), 1_000);
    assert_eq!(after_step.nanoseconds(), 900);
    let realtime_step = ClockDiscontinuity::RealtimeStep {
        previous: realtime_before,
        current: realtime_after,
    };
    let (realtime_step_tx, realtime_step_rx) = mpsc::channel();
    let waiter = thread::spawn(move || {
        let started = Instant::now();
        realtime_step_tx
            .send(())
            .expect("notify pending RPC wait started");
        let result = pending.wait(Duration::from_millis(80));
        (started.elapsed(), result)
    });
    realtime_step_rx
        .recv_timeout(Duration::from_secs(1))
        .expect("pending RPC wait began");

    let mut reset_device = device_clock;
    let device_reset = reset_device.reset().unwrap();
    assert!(device_mapping.invalidate(&device_reset));
    assert!(!realtime_mapping.invalidate(&device_reset));
    assert!(realtime_mapping.invalidate(&realtime_step));
    assert!(!device_mapping.invalidate(&realtime_step));
    assert!(device_mapping.is_invalidated());
    assert!(realtime_mapping.is_invalidated());
    assert_eq!(
        device_mapping.convert(
            &Timestamp::new(*device_mapping.source(), 1),
            Some(0),
            device_mapping.target(),
            &Timestamp::new(host_identity(4), 2),
        ),
        Err(TimeError::MappingInvalidated)
    );

    let (wait_elapsed, wait_result) = waiter.join().expect("pending RPC waiter");
    assert!(matches!(wait_result, Err(RpcError::DeadlineExceeded)));
    assert!(wait_elapsed >= Duration::from_millis(80));
    assert!(wait_elapsed < Duration::from_secs(1));
    release_first_tx
        .send(())
        .expect("release timed-out server request");
    first_dropped_rx
        .recv_timeout(Duration::from_secs(1))
        .expect("server dropped timed-out request");

    let mut healthy_request =
        client.loan_request_slice(TIMING_PREFIX_LEN + REQUEST_APPLICATION.len())?;
    *healthy_request.user_header_mut() = 20;
    write_payload(
        &mut healthy_request,
        &timing(200, 225, 0),
        REQUEST_APPLICATION,
    );
    let response = healthy_request.send()?.wait(Duration::from_secs(1))?;
    assert_eq!(*response.user_header(), 30);
    let (received_timing, received_application) = split_payload(&response).unwrap();
    assert_eq!(received_timing, timing(300, 325, 0));
    assert_eq!(received_application, RESPONSE_APPLICATION);

    server_thread.join().expect("stamped RPC server thread")?;

    let legacy = RpcDescriptor::for_slice::<Image, Image>(service_name("legacy"));
    assert!(matches!(
        SliceRpc::<StampedBytes, StampedBytes>::preflight(&legacy),
        Err(RpcError::AbiMismatch { .. })
    ));
    Ok(())
}

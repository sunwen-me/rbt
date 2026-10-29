use std::{
    env, fs, thread,
    time::{Duration, Instant},
};

use rbt_iox2::{SliceStream, StreamProfile};
use rbt_time::{
    ClockIdentity, ClockKind, ClockMapping, ClockMappingCalibration, ClockSourceId, DeviceClock,
    HostClock, SensorTiming, TimeError, Timestamp, TimestampState,
};
use rbt_time_stream::{StampedBytes, TIMING_PREFIX_LEN, encode_timing_into, split_payload};
use serde_json::{Value, json};

const MIN_SOAK_SECONDS: u64 = 300;
const SAMPLE_PERIOD: Duration = Duration::from_secs(5);
const RSS_GROWTH_LIMIT_KIB: u64 = 16 * 1024;

fn main() {
    let result = match env::args().nth(1).as_deref() {
        Some("semantics") => semantics_probe(),
        Some("soak") => soak(env::args().skip(2).collect()),
        Some("stream-publish") => stream_publish(env::args().skip(2).collect()),
        Some("stream-consume") => stream_consume(env::args().skip(2).collect()),
        Some("camera-consume") => camera_consume(env::args().skip(2).collect()),
        Some("--help") | Some("-h") | None => {
            eprintln!(
                "usage: rbt-time-tools semantics | soak [--duration SEC] | stream-publish SERVICE | stream-consume SERVICE | camera-consume SERVICE DURATION_SEC"
            );
            Ok(())
        }
        Some(command) => Err(format!("unknown command: {command}")),
    };

    if let Err(error) = result {
        eprintln!("RBT_TIME_TOOLS ERROR {error}");
        std::process::exit(1);
    }
}

fn camera_consume(arguments: Vec<String>) -> Result<(), String> {
    let [service, duration_arg] = arguments.as_slice() else {
        return Err("camera-consume requires SERVICE and DURATION_SEC".to_owned());
    };
    if service.trim().is_empty() {
        return Err("camera-consume service name is empty".to_owned());
    }
    let duration_seconds = duration_arg
        .parse::<u64>()
        .map_err(|_| "camera-consume duration must be an integer".to_owned())?;
    if duration_seconds == 0 {
        return Err("camera-consume duration must be positive".to_owned());
    }
    let clock = HostClock::new().map_err(debug_error)?;
    let descriptor =
        SliceStream::<StampedBytes>::descriptor(service.clone(), StreamProfile::Sensor);
    let subscriber =
        SliceStream::<StampedBytes>::subscriber(&descriptor).map_err(|error| error.to_string())?;
    println!("RBT_TIME_CAMERA_CONSUMER READY");
    use std::io::Write as _;
    std::io::stdout()
        .flush()
        .map_err(|error| error.to_string())?;
    let deadline = Instant::now() + Duration::from_secs(duration_seconds);
    let mut received = 0_u64;
    let mut capture_known = 0_u64;
    let mut capture_unknown = 0_u64;
    let mut generations = std::collections::BTreeSet::new();
    while Instant::now() < deadline {
        if let Some(sample) = subscriber.receive().map_err(|error| error.to_string())? {
            let sequence = *sample.header();
            let (timing, application) =
                split_payload(sample.payload()).map_err(|error| error.to_string())?;
            let (session_generation, raw_driver_ns, driver_flags, pixel_bytes) =
                validate_camera_payload(&timing, application, clock.monotonic_identity())?;
            if timing.capture().is_some() {
                capture_known += 1;
            } else {
                capture_unknown += 1;
            }
            generations.insert(session_generation);
            println!(
                "{{\"event\":\"frame_checked\",\"sequence\":{sequence},\"driver_session_generation\":{session_generation},\"driver_timestamp_ns\":{raw_driver_ns},\"driver_flags\":{driver_flags},\"capture_present\":{},\"received_source_id\":\"{}\",\"pixel_bytes\":{},\"fnv1a64\":\"{:016x}\"}}",
                timing.capture().is_some(),
                timing.received().clock().source_id().to_hex(),
                pixel_bytes.len(),
                fnv1a64(pixel_bytes)
            );
            std::io::stdout()
                .flush()
                .map_err(|error| error.to_string())?;
            received += 1;
        } else {
            thread::sleep(Duration::from_millis(2));
        }
    }
    if received == 0 {
        return Err("no physical camera frames received".to_owned());
    }
    println!(
        "{{\"event\":\"consumer_summary\",\"received\":{received},\"capture_known\":{capture_known},\"capture_unknown\":{capture_unknown},\"session_generations\":[{}]}}",
        generations
            .iter()
            .map(u64::to_string)
            .collect::<Vec<_>>()
            .join(",")
    );
    Ok(())
}

fn validate_camera_payload<'a>(
    timing: &SensorTiming,
    application: &'a [u8],
    expected_host_clock: &ClockIdentity,
) -> Result<(u64, i64, u32, &'a [u8]), String> {
    if application.len() < 48 || &application[..8] != b"RBTVCAM1" {
        return Err("camera application prefix is absent or invalid".to_owned());
    }
    let width = u32::from_le_bytes(application[8..12].try_into().expect("fixed range"));
    let height = u32::from_le_bytes(application[12..16].try_into().expect("fixed range"));
    let stride = u32::from_le_bytes(application[16..20].try_into().expect("fixed range"));
    let fourcc = u32::from_le_bytes(application[20..24].try_into().expect("fixed range"));
    let raw_driver_ns = i64::from_le_bytes(application[24..32].try_into().expect("fixed range"));
    let driver_flags = u32::from_le_bytes(application[32..36].try_into().expect("fixed range"));
    let reserved = u32::from_le_bytes(application[36..40].try_into().expect("fixed range"));
    let session_generation =
        u64::from_le_bytes(application[40..48].try_into().expect("fixed range"));
    if width == 0
        || height == 0
        || u64::from(stride) < u64::from(width) * 2
        || fourcc != 0x5659_5559
        || reserved != 0
    {
        return Err("camera format metadata is invalid".to_owned());
    }
    let pixel_bytes = &application[48..];
    let minimum = u64::from(height - 1) * u64::from(stride) + u64::from(width) * 2;
    let maximum = u64::from(height) * u64::from(stride);
    if (pixel_bytes.len() as u64) < minimum || (pixel_bytes.len() as u64) > maximum {
        return Err("camera pixel payload size is inconsistent with format metadata".to_owned());
    }
    if timing.received().clock() != expected_host_clock
        || timing.received().clock().kind() != ClockKind::HostMonotonic
        || timing.capture_uncertainty_ns().is_some()
        || timing.received_uncertainty_ns().is_some()
    {
        return Err("camera receipt timing is not from this host monotonic clock".to_owned());
    }
    if let Some(capture) = timing.capture() {
        let timestamp_type = driver_flags & 0x0000_e000;
        let timestamp_source = driver_flags & 0x0007_0000;
        if capture.clock() != timing.received().clock()
            || capture.nanoseconds() != raw_driver_ns
            || timestamp_type != 0x0000_2000
            || timestamp_source != 0x0001_0000
        {
            return Err("camera capture stamp is not proven by V4L2 flags".to_owned());
        }
    }
    Ok((session_generation, raw_driver_ns, driver_flags, pixel_bytes))
}

fn stream_publish(arguments: Vec<String>) -> Result<(), String> {
    let service = one_service_argument(&arguments)?;
    let descriptor = SliceStream::<StampedBytes>::descriptor(service, StreamProfile::Sensor);
    let publisher = SliceStream::<StampedBytes>::publisher(&descriptor, TIMING_PREFIX_LEN + 32)
        .map_err(|error| error.to_string())?;
    println!("RBT_TIME_STREAM READY_TO_PUBLISH");
    use std::io::Write as _;
    std::io::stdout()
        .flush()
        .map_err(|error| error.to_string())?;
    thread::sleep(Duration::from_secs(5));

    let timing = interop_timing()?;
    let application = std::array::from_fn::<_, 32, _>(|index| index as u8);
    let mut loan = publisher
        .loan_slice(TIMING_PREFIX_LEN + application.len())
        .map_err(|error| error.to_string())?;
    *loan.header_mut() = INTEROP_SEQUENCE;
    encode_timing_into(&timing, &mut loan.payload_mut()[..TIMING_PREFIX_LEN])
        .map_err(|error| error.to_string())?;
    loan.payload_mut()[TIMING_PREFIX_LEN..].copy_from_slice(&application);
    loan.send().map_err(|error| error.to_string())?;
    println!(
        "{{\"status\":\"sent\",\"sequence\":{INTEROP_SEQUENCE},\"payload_len\":{}}}",
        application.len()
    );
    Ok(())
}

fn stream_consume(arguments: Vec<String>) -> Result<(), String> {
    let service = one_service_argument(&arguments)?;
    let descriptor = SliceStream::<StampedBytes>::descriptor(service, StreamProfile::Sensor);
    let subscriber =
        SliceStream::<StampedBytes>::subscriber(&descriptor).map_err(|error| error.to_string())?;
    println!("RBT_TIME_STREAM READY_TO_RECEIVE");
    use std::io::Write as _;
    std::io::stdout()
        .flush()
        .map_err(|error| error.to_string())?;

    let deadline = Instant::now() + Duration::from_secs(30);
    while Instant::now() < deadline {
        if let Some(sample) = subscriber.receive().map_err(|error| error.to_string())? {
            if *sample.header() != INTEROP_SEQUENCE {
                return Err(format!("sequence mismatch: {}", sample.header()));
            }
            let (timing, application) =
                split_payload(sample.payload()).map_err(|error| error.to_string())?;
            validate_interop_timing(timing)?;
            validate_interop_application(application)?;
            println!(
                "{{\"status\":\"received\",\"sequence\":{},\"payload_len\":{},\"fnv1a64\":\"{:016x}\",\"capture_ns\":{},\"received_ns\":{},\"capture_uncertainty_ns\":{},\"received_uncertainty_ns\":{}}}",
                sample.header(),
                application.len(),
                fnv1a64(application),
                timing.capture().expect("interop capture").nanoseconds(),
                timing.received().nanoseconds(),
                timing
                    .capture_uncertainty_ns()
                    .expect("known capture uncertainty"),
                timing
                    .received_uncertainty_ns()
                    .expect("known receipt uncertainty")
            );
            return Ok(());
        }
        thread::sleep(Duration::from_millis(5));
    }
    Err("timed out waiting for one StampedBytes sample".to_owned())
}

const INTEROP_SEQUENCE: u64 = 4242;

fn one_service_argument(arguments: &[String]) -> Result<String, String> {
    match arguments {
        [service] if !service.trim().is_empty() => Ok(service.clone()),
        _ => Err("expected one non-empty service name".to_owned()),
    }
}

fn interop_timing() -> Result<SensorTiming, String> {
    let source = ClockSourceId::from_device_id([0x44; 16]).map_err(debug_error)?;
    let clock = ClockIdentity::new(ClockKind::Device, source, 7).map_err(debug_error)?;
    SensorTiming::new(
        Some(Timestamp::new(clock, 123_456_789)),
        Timestamp::new(clock, 987_654_321),
        Some(2),
        Some(1),
    )
    .map_err(debug_error)
}

fn validate_interop_timing(timing: SensorTiming) -> Result<(), String> {
    let capture = timing
        .capture()
        .ok_or_else(|| "capture timestamp is absent".to_owned())?;
    let fixed_timing = interop_timing()?;
    let expected_clock = fixed_timing.capture().expect("fixed capture").clock();
    if capture.clock() != expected_clock
        || capture.nanoseconds() != 123_456_789
        || timing.received().clock() != expected_clock
        || timing.received().nanoseconds() != 987_654_321
        || timing.capture_uncertainty_ns() != Some(2)
        || timing.received_uncertainty_ns() != Some(1)
    {
        return Err("received timing differs from the fixed interop fixture".to_owned());
    }
    Ok(())
}

fn validate_interop_application(application: &[u8]) -> Result<(), String> {
    if application != (0_u8..32).collect::<Vec<_>>() {
        return Err("application bytes differ from the fixed interop fixture".to_owned());
    }
    Ok(())
}

fn fnv1a64(bytes: &[u8]) -> u64 {
    bytes
        .iter()
        .fold(14_695_981_039_346_656_037_u64, |hash, byte| {
            (hash ^ u64::from(*byte)).wrapping_mul(1_099_511_628_211)
        })
}

fn semantics_probe() -> Result<(), String> {
    let device_id = [0x11; 16];
    let other_device_id = [0x22; 16];
    let mut device = DeviceClock::new(device_id, 0).map_err(debug_error)?;
    let device_before = device.stamp(103);
    let device_other = DeviceClock::new(other_device_id, 0).map_err(debug_error)?;
    let device_other_stamp = device_other.stamp(103);
    let realtime = identity(ClockKind::SystemRealtime, 0x33, 0)?;
    let host_identity = calibration_host_identity()?;
    let host_at_10 = Timestamp::new(host_identity, 10);
    let host_at_200 = Timestamp::new(host_identity, 200);

    let arithmetic_identity = identity(ClockKind::Device, 0x11, 0)?;
    let arithmetic_left = Timestamp::new(arithmetic_identity, 7);
    let arithmetic_right = Timestamp::new(arithmetic_identity, 4);
    let arithmetic_add = arithmetic_right
        .checked_add_nanoseconds(-2)
        .map_err(debug_error)?;
    let add_overflow =
        error_code(Timestamp::new(arithmetic_identity, i64::MAX).checked_add_nanoseconds(1_i64));
    let negative_duration = error_code(
        Timestamp::new(arithmetic_identity, 10)
            .checked_duration_since(&Timestamp::new(arithmetic_identity, 11)),
    );

    let base_mapping = mapping(
        *device_before.clock(),
        realtime,
        host_at_10,
        host_at_200,
        3,
        2,
        5,
    )?;
    let converted = base_mapping
        .convert(
            &device_before,
            Some(2),
            &realtime,
            &Timestamp::new(host_identity, 11),
        )
        .map_err(debug_error)?;
    let expiry = error_code(base_mapping.convert(&device_before, Some(2), &realtime, &host_at_200));
    let unknown_uncertainty = error_code(base_mapping.convert(
        &device_before,
        None,
        &realtime,
        &Timestamp::new(host_identity, 11),
    ));

    let reset_event = device.reset().map_err(debug_error)?;
    let reset_old_stamp = error_code(device_before.checked_compare(&device.stamp(103)));
    let invalidated_mapping = mapping(
        *device_before.clock(),
        realtime,
        host_at_10,
        host_at_200,
        1,
        1,
        0,
    )?;
    let invalidated = invalidated_mapping.invalidate(&reset_event);
    let invalidated_error = error_code(invalidated_mapping.convert(
        &device_before,
        Some(0),
        &realtime,
        &Timestamp::new(host_identity, 11),
    ));
    let overflow_mapping = mapping(
        *device_before.clock(),
        realtime,
        host_at_10,
        host_at_200,
        2,
        1,
        1,
    )?;
    let uncertainty_overflow = error_code(overflow_mapping.convert(
        &device_before,
        Some(u64::MAX),
        &realtime,
        &Timestamp::new(host_identity, 11),
    ));
    let maximum_uncertainty_mapping = mapping(
        *device_before.clock(),
        realtime,
        host_at_10,
        host_at_200,
        u64::MAX,
        u64::MAX,
        0,
    )?;
    let maximum_uncertainty = maximum_uncertainty_mapping
        .convert(
            &device_before,
            Some(u64::MAX),
            &realtime,
            &Timestamp::new(host_identity, 11),
        )
        .map_err(debug_error)?;
    let self_mapping_offset = error_code(mapping_with_anchors(
        *device_before.clock(),
        *device_before.clock(),
        host_at_10,
        host_at_200,
        100,
        101,
        1,
        1,
        0,
    ));
    let self_mapping_rate = error_code(mapping_with_anchors(
        *device_before.clock(),
        *device_before.clock(),
        host_at_10,
        host_at_200,
        100,
        100,
        2,
        1,
        0,
    ));
    let self_mapping_identity = mapping_with_anchors(
        *device_before.clock(),
        *device_before.clock(),
        host_at_10,
        host_at_200,
        100,
        100,
        2,
        2,
        0,
    )
    .map_err(debug_error)?
    .convert(
        &device_before,
        Some(0),
        device_before.clock(),
        &Timestamp::new(host_identity, 11),
    )
    .map_err(debug_error)?
    .timestamp
    .nanoseconds();

    let capture = Some(device_before);
    let received = Timestamp::new(host_identity, 400);
    let timing = SensorTiming::new(capture, received, Some(2), Some(1)).map_err(debug_error)?;
    let legacy_state = match TimestampState::from_legacy_timestamp_ns(42) {
        TimestampState::Unknown => "unknown",
        TimestampState::Known(_) => "known",
        TimestampState::Unmapped(_) => "unmapped",
    };
    let host = live_host_observation();

    let output = json!({
        "cases": {
            "arithmetic": {
                "difference_ns": arithmetic_left.checked_difference(&arithmetic_right).map_err(debug_error)?,
                "duration_ns": arithmetic_left.checked_duration_since(&arithmetic_right).map_err(debug_error)?.as_nanos(),
                "add_result_ns": arithmetic_add.nanoseconds(),
                "add_overflow": add_overflow,
                "negative_duration": negative_duration
            },
            "identity": {
                "source_mismatch": error_code(device_before.checked_compare(&device_other_stamp)),
                "generation_mismatch": error_code(device_before.checked_compare(&device.stamp(103))),
        "domain_mismatch": error_code(device_before.checked_compare(&Timestamp::new(realtime, 103)))
            },
            "device_reset": {
                "generation": device.identity().generation(),
                "old_stamp_rejected": reset_old_stamp
            },
            "mapping": {
                "timestamp_ns": converted.timestamp.nanoseconds(),
                "uncertainty_ns": converted.uncertainty_ns,
                "expiry": expiry,
                "unknown_uncertainty": unknown_uncertainty,
                "invalidated": if invalidated && invalidated_error == "MappingInvalidated" {
                    invalidated_error
                } else {
                    "INVALIDATION_FAILED".to_owned()
                },
                "uncertainty_overflow": uncertainty_overflow,
                "uncertainty_u64_max": maximum_uncertainty.uncertainty_ns,
                "self_mapping_offset": self_mapping_offset,
                "self_mapping_rate": self_mapping_rate,
                "self_mapping_identity": self_mapping_identity
            },
            "capture_separation": {
                "capture_kind": kind_name(timing.capture().expect("capture was supplied").clock().kind()),
                "received_kind": kind_name(timing.received().clock().kind()),
                "legacy_state": legacy_state
            }
        },
        "host_monotonic": host
    });
    println!(
        "{}",
        serde_json::to_string(&output).map_err(|error| error.to_string())?
    );
    Ok(())
}

fn soak(arguments: Vec<String>) -> Result<(), String> {
    let mut duration_seconds = MIN_SOAK_SECONDS;
    let mut index = 0;
    while index < arguments.len() {
        match arguments[index].as_str() {
            "--duration" if index + 1 < arguments.len() => {
                duration_seconds = arguments[index + 1]
                    .parse::<u64>()
                    .map_err(|_| "--duration must be an integer number of seconds".to_owned())?;
                index += 2;
            }
            value => return Err(format!("unexpected soak argument: {value}")),
        }
    }
    if duration_seconds < MIN_SOAK_SECONDS {
        return Err(format!(
            "time soak duration must be at least {MIN_SOAK_SECONDS} seconds"
        ));
    }

    let clock = HostClock::new().map_err(debug_error)?;
    let baseline = resource_snapshot()?;
    let start = Instant::now();
    let end_after = Duration::from_secs(duration_seconds);
    let device = DeviceClock::new([0x55; 16], 0).map_err(debug_error)?;
    let old_stamp = device.stamp(103);
    let initial_host = clock.monotonic_now().map_err(debug_error)?;
    let expiry_seconds = duration_seconds
        .checked_add(60)
        .ok_or_else(|| "duration is too large".to_owned())?;
    let expiry_ns = initial_host
        .nanoseconds()
        .checked_add(
            i64::try_from(expiry_seconds)
                .map_err(|_| "duration is too large")?
                .checked_mul(1_000_000_000)
                .ok_or_else(|| "duration is too large".to_owned())?,
        )
        .ok_or_else(|| "soak mapping expiry is outside timestamp range".to_owned())?;
    let realtime = clock.realtime_identity();
    let map = mapping(
        *old_stamp.clock(),
        realtime,
        initial_host,
        Timestamp::new(*initial_host.clock(), expiry_ns),
        3,
        2,
        5,
    )?;
    let reset_mapping = mapping(
        *old_stamp.clock(),
        *initial_host.clock(),
        initial_host,
        Timestamp::new(*initial_host.clock(), expiry_ns),
        1,
        1,
        0,
    )?;
    let mut reset = device;
    let reset_event = reset.reset().map_err(debug_error)?;
    if !reset_mapping.invalidate(&reset_event) {
        return Err("device reset failed to invalidate the old mapping".to_owned());
    }
    let reset_error =
        error_code(reset_mapping.convert(&old_stamp, Some(0), initial_host.clock(), &initial_host));
    if reset_error != "MappingInvalidated" {
        return Err(format!("stale mapping returned {reset_error}"));
    }

    let expired_map = mapping(
        *old_stamp.clock(),
        *initial_host.clock(),
        initial_host,
        Timestamp::new(*initial_host.clock(), initial_host.nanoseconds() + 2),
        1,
        1,
        0,
    )?;
    let mut previous_mono = initial_host;
    let mut previous_realtime = clock.realtime_now().map_err(debug_error)?;
    let mut iterations = 0_u64;
    let mut realtime_backward_steps = 0_u64;
    let mut last_sample = Instant::now();
    let mut baseline_after_warmup: Option<ResourceSnapshot> = None;
    let mut peak_after_warmup: Option<ResourceSnapshot> = None;
    println!(
        "RBT_TIME_SOAK START duration_sec={duration_seconds} vmrss_kib={} threads={} fds={}",
        baseline.vmrss_kib, baseline.threads, baseline.fds
    );

    while start.elapsed() < end_after {
        let monotonic = clock.monotonic_now().map_err(debug_error)?;
        if monotonic
            .checked_compare(&previous_mono)
            .map_err(debug_error)?
            == std::cmp::Ordering::Less
        {
            return Err("host monotonic clock moved backwards".to_owned());
        }
        previous_mono = monotonic;
        let realtime_now = clock.realtime_now().map_err(debug_error)?;
        match realtime_now.checked_compare(&previous_realtime) {
            Ok(std::cmp::Ordering::Less) | Err(TimeError::ClockGenerationMismatch) => {
                realtime_backward_steps = realtime_backward_steps.saturating_add(1);
            }
            Ok(_) => {}
            Err(error) => return Err(debug_error(error)),
        }
        previous_realtime = realtime_now;

        let converted = map
            .convert(&old_stamp, Some(2), map.target(), &monotonic)
            .map_err(debug_error)?;
        if converted.timestamp.nanoseconds() != 1004 || converted.uncertainty_ns != 9 {
            return Err("calibration arithmetic changed during soak".to_owned());
        }
        if error_code(expired_map.convert(&old_stamp, Some(0), monotonic.clock(), &monotonic))
            != "ClockMappingExpired"
        {
            return Err("mapping expiry did not remain enforced".to_owned());
        }

        if error_code(reset_mapping.convert(&old_stamp, Some(0), initial_host.clock(), &monotonic))
            != "MappingInvalidated"
        {
            return Err("invalidated mapping was reused after a device reset".to_owned());
        }
        if error_code(old_stamp.checked_compare(&reset.stamp(103))) != "ClockGenerationMismatch" {
            return Err("a pre-reset timestamp was accepted in the new generation".to_owned());
        }

        // Repeatedly construct and drop short-lived calibrations. This catches
        // accidental global mapping registries and resource retention while
        // keeping the number of live calibration objects bounded at one.
        let ephemeral_map = mapping(
            *old_stamp.clock(),
            *monotonic.clock(),
            monotonic,
            Timestamp::new(*monotonic.clock(), expiry_ns),
            3,
            2,
            5,
        )?;
        let ephemeral = ephemeral_map
            .convert(&old_stamp, Some(2), monotonic.clock(), &monotonic)
            .map_err(debug_error)?;
        if ephemeral.timestamp.nanoseconds() != 1004 || ephemeral.uncertainty_ns != 9 {
            return Err("ephemeral calibration arithmetic changed during soak".to_owned());
        }
        iterations = iterations.saturating_add(1);

        if baseline_after_warmup.is_none() && start.elapsed() >= Duration::from_secs(2) {
            let warmup = resource_snapshot()?;
            baseline_after_warmup = Some(warmup);
            peak_after_warmup = Some(warmup);
        }
        if last_sample.elapsed() >= SAMPLE_PERIOD {
            let snapshot = resource_snapshot()?;
            if let Some(peak) = peak_after_warmup.as_mut() {
                peak.vmrss_kib = peak.vmrss_kib.max(snapshot.vmrss_kib);
                peak.threads = peak.threads.max(snapshot.threads);
                peak.fds = peak.fds.max(snapshot.fds);
            }
            println!(
                "RBT_TIME_SOAK SAMPLE elapsed_ms={} iterations={} vmrss_kib={} threads={} fds={}",
                start.elapsed().as_millis(),
                iterations,
                snapshot.vmrss_kib,
                snapshot.threads,
                snapshot.fds
            );
            last_sample = Instant::now();
        }
        thread::sleep(Duration::from_millis(10));
    }

    let final_warm = resource_snapshot()?;
    let warm = baseline_after_warmup.unwrap_or(final_warm);
    let final_snapshot = resource_snapshot()?;
    let peak = peak_after_warmup.unwrap_or(final_snapshot);
    let elapsed = start.elapsed();
    if elapsed < end_after {
        return Err("soak ended before the requested real monotonic duration".to_owned());
    }
    if final_snapshot.threads != baseline.threads
        || final_snapshot.fds != baseline.fds
        || peak.threads != baseline.threads
        || peak.fds != baseline.fds
    {
        return Err(format!(
            "thread/fd counts changed: baseline={}/{} peak={}/{} final={}/{}",
            baseline.threads,
            baseline.fds,
            peak.threads,
            peak.fds,
            final_snapshot.threads,
            final_snapshot.fds
        ));
    }
    let rss_growth = final_snapshot.vmrss_kib.saturating_sub(warm.vmrss_kib);
    let peak_rss_growth = peak.vmrss_kib.saturating_sub(warm.vmrss_kib);
    if rss_growth > RSS_GROWTH_LIMIT_KIB || peak_rss_growth > RSS_GROWTH_LIMIT_KIB {
        return Err(format!(
            "RSS grew by {rss_growth} KiB at final and {peak_rss_growth} KiB at peak after warmup; limit is {RSS_GROWTH_LIMIT_KIB} KiB"
        ));
    }
    println!(
        "RBT_TIME_SOAK PASS elapsed_ms={} iterations={} realtime_backward_steps={} warmup_vmrss_kib={} peak_vmrss_kib={} final_vmrss_kib={} peak_rss_growth_kib={} final_rss_growth_kib={} peak_threads={} final_threads={} peak_fds={} final_fds={} mapping_registry=none",
        elapsed.as_millis(),
        iterations,
        realtime_backward_steps,
        warm.vmrss_kib,
        peak.vmrss_kib,
        final_snapshot.vmrss_kib,
        peak_rss_growth,
        rss_growth,
        peak.threads,
        final_snapshot.threads,
        peak.fds,
        final_snapshot.fds
    );
    Ok(())
}

#[derive(Clone, Copy)]
struct ResourceSnapshot {
    vmrss_kib: u64,
    threads: u64,
    fds: u64,
}

fn resource_snapshot() -> Result<ResourceSnapshot, String> {
    let status = fs::read_to_string("/proc/self/status").map_err(|error| error.to_string())?;
    let mut vmrss_kib = None;
    let mut threads = None;
    for line in status.lines() {
        if let Some(value) = line.strip_prefix("VmRSS:") {
            vmrss_kib = value
                .split_ascii_whitespace()
                .next()
                .and_then(|v| v.parse().ok());
        } else if let Some(value) = line.strip_prefix("Threads:") {
            threads = value.trim().parse().ok();
        }
    }
    let fds = fs::read_dir("/proc/self/fd")
        .map_err(|error| error.to_string())?
        .count() as u64;
    Ok(ResourceSnapshot {
        vmrss_kib: vmrss_kib.ok_or_else(|| "VmRSS was absent from /proc/self/status".to_owned())?,
        threads: threads.ok_or_else(|| "Threads was absent from /proc/self/status".to_owned())?,
        fds,
    })
}

fn mapping(
    source: ClockIdentity,
    target: ClockIdentity,
    calibrated_at: Timestamp,
    expires_at: Timestamp,
    rate_numerator: u64,
    rate_denominator: u64,
    uncertainty_ns: u64,
) -> Result<ClockMapping, String> {
    mapping_with_anchors(
        source,
        target,
        calibrated_at,
        expires_at,
        100,
        1000,
        rate_numerator,
        rate_denominator,
        uncertainty_ns,
    )
    .map_err(debug_error)
}

#[allow(clippy::too_many_arguments)]
fn mapping_with_anchors(
    source: ClockIdentity,
    target: ClockIdentity,
    calibrated_at: Timestamp,
    expires_at: Timestamp,
    source_anchor_ns: i64,
    target_anchor_ns: i64,
    rate_numerator: u64,
    rate_denominator: u64,
    uncertainty_ns: u64,
) -> Result<ClockMapping, TimeError> {
    ClockMapping::new(ClockMappingCalibration {
        source,
        target,
        source_anchor_ns,
        target_anchor_ns,
        rate_numerator,
        rate_denominator,
        valid_from_ns: 0,
        valid_until_ns: 10_000,
        calibration_id: [1; 16],
        calibrated_at,
        expires_at,
        uncertainty_ns: Some(uncertainty_ns),
    })
}

fn identity(kind: ClockKind, first_byte: u8, generation: u64) -> Result<ClockIdentity, String> {
    let source_id = ClockSourceId::from_device_id([first_byte; 16]).map_err(debug_error)?;
    ClockIdentity::new(kind, source_id, generation).map_err(debug_error)
}

fn calibration_host_identity() -> Result<ClockIdentity, String> {
    let mut bytes = [0; 48];
    bytes[..16].fill(0x44);
    bytes[16] = 1;
    bytes[24] = 2;
    ClockIdentity::new(
        ClockKind::HostMonotonic,
        ClockSourceId::from_bytes(bytes).map_err(debug_error)?,
        0,
    )
    .map_err(debug_error)
}

fn live_host_observation() -> Value {
    match HostClock::new() {
        Ok(clock) => match clock.monotonic_now() {
            Ok(now) => json!({
                "available": true,
                "kind": kind_name(now.clock().kind()),
                "source_id": now.clock().source_id().to_hex(),
                "generation": now.clock().generation(),
                "nanoseconds": now.nanoseconds()
            }),
            Err(error) => json!({ "available": false, "error": format!("{error:?}") }),
        },
        Err(error) => json!({ "available": false, "error": format!("{error:?}") }),
    }
}

fn kind_name(kind: ClockKind) -> &'static str {
    match kind {
        ClockKind::HostMonotonic => "host_monotonic",
        ClockKind::SystemRealtime => "system_realtime",
        ClockKind::Device => "device",
    }
}

fn error_code<T>(result: Result<T, TimeError>) -> String {
    match result {
        Ok(_) => "UNEXPECTED_OK".to_owned(),
        Err(error) => format!("{error:?}"),
    }
}

fn debug_error(error: TimeError) -> String {
    format!("{error:?}")
}

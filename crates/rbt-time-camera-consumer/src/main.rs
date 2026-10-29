use std::{
    collections::BTreeMap,
    env,
    io::{self, Write},
    process, thread,
    time::{Duration, Instant},
};

use rbt_iox2::{SliceStream, StreamProfile, local_pressure_snapshots};
use rbt_time::{ClockKind, HostClock};
use rbt_time_stream::{StampedBytes, split_payload};
use serde_json::json;

const CAMERA_PREFIX_BYTES: usize = 48;
const V4L2_PIX_FMT_YUYV: u32 = u32::from_le_bytes(*b"YUYV");
const V4L2_BUF_FLAG_TIMESTAMP_MASK: u32 = 0x0000_e000;
const V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC: u32 = 0x0000_2000;
const V4L2_BUF_FLAG_TSTAMP_SRC_MASK: u32 = 0x0007_0000;
const V4L2_BUF_FLAG_TSTAMP_SRC_SOE: u32 = 0x0001_0000;
const MAX_DRIVER_GENERATIONS: usize = 8;

#[derive(Debug)]
struct Options {
    service: String,
    duration_seconds: u64,
}

fn parse_options() -> Result<Options, String> {
    let mut service = None;
    let mut duration_seconds = None;
    let mut args = env::args().skip(1);
    while let Some(argument) = args.next() {
        match argument.as_str() {
            "--service" => service = Some(args.next().ok_or("--service requires a value")?),
            "--duration-sec" => {
                let value = args.next().ok_or("--duration-sec requires a value")?;
                let parsed = value
                    .parse::<u64>()
                    .map_err(|_| "--duration-sec must be a positive integer")?;
                if parsed == 0 {
                    return Err("--duration-sec must be a positive integer".into());
                }
                duration_seconds = Some(parsed);
            }
            "--help" | "-h" => {
                println!("Usage: rbt-time-camera-consumer --service NAME --duration-sec N");
                process::exit(0);
            }
            unknown => return Err(format!("unknown argument: {unknown}")),
        }
    }
    Ok(Options {
        service: service.ok_or("--service is required")?,
        duration_seconds: duration_seconds.ok_or("--duration-sec is required")?,
    })
}

fn read_u32_le(bytes: &[u8], offset: usize) -> u32 {
    u32::from_le_bytes(
        bytes[offset..offset + 4]
            .try_into()
            .expect("fixed camera field"),
    )
}

fn read_u64_le(bytes: &[u8], offset: usize) -> u64 {
    u64::from_le_bytes(
        bytes[offset..offset + 8]
            .try_into()
            .expect("fixed camera field"),
    )
}

fn read_i64_le(bytes: &[u8], offset: usize) -> i64 {
    i64::from_le_bytes(
        bytes[offset..offset + 8]
            .try_into()
            .expect("fixed camera field"),
    )
}

fn fnv1a64(bytes: &[u8]) -> u64 {
    let mut hash = 14_695_981_039_346_656_037_u64;
    for byte in bytes {
        hash ^= u64::from(*byte);
        hash = hash.wrapping_mul(1_099_511_628_211);
    }
    hash
}

fn borrowed_samples(pressure: &rbt_iox2::StreamPressure) -> u64 {
    pressure
        .subscriber
        .as_ref()
        .map_or(0, |subscriber| subscriber.borrowed_samples)
}

fn emit_json(value: serde_json::Value) -> Result<(), String> {
    println!("{value}");
    io::stdout()
        .flush()
        .map_err(|error| format!("flush JSON output: {error}"))
}

fn run(options: Options) -> Result<(), String> {
    let host_clock = HostClock::new().map_err(|error| format!("create HostClock: {error}"))?;
    let host_identity = *host_clock.monotonic_identity();
    let host_source = host_identity.source_id();
    let namespace_offset_zero = host_source.as_bytes()[32..48].iter().all(|byte| *byte == 0);
    let resources_before = local_pressure_snapshots().len();
    let descriptor =
        SliceStream::<StampedBytes>::descriptor(options.service.clone(), StreamProfile::Sensor);
    let subscriber = SliceStream::<StampedBytes>::subscriber(&descriptor)
        .map_err(|error| format!("open StampedBytes subscriber: {error}"))?;
    let resources_during = local_pressure_snapshots().len();
    emit_json(json!({
        "event": "consumer_ready",
        "language": "rust",
        "service": options.service,
        "clock_source_id": host_source.to_hex(),
        "namespace_offset_zero": namespace_offset_zero
    }))?;

    let start = Instant::now();
    let deadline = start
        .checked_add(Duration::from_secs(options.duration_seconds))
        .ok_or("--duration-sec exceeds the steady-clock range")?;
    let mut received_count = 0_u64;
    let mut dropped_count = 0_u64;
    let mut capture_known = 0_u64;
    let mut capture_unknown = 0_u64;
    let mut generations = BTreeMap::<u64, u32>::new();

    while Instant::now() < deadline {
        let maybe_sample = subscriber
            .receive()
            .map_err(|error| format!("receive StampedBytes sample: {error}"))?;
        let Some(sample) = maybe_sample else {
            thread::sleep(Duration::from_millis(1));
            continue;
        };
        let sequence = *sample.header();
        if sequence > u64::from(u32::MAX) {
            return Err(format!(
                "sequence {sequence} exceeds the V4L2 u32 sequence range"
            ));
        }
        let (timing, application) = split_payload(sample.payload())
            .map_err(|error| format!("decode StampedBytes timing prefix: {error}"))?;
        if application.len() < CAMERA_PREFIX_BYTES || &application[..8] != b"RBTVCAM1" {
            return Err("invalid camera application prefix".into());
        }
        let camera = &application[..CAMERA_PREFIX_BYTES];
        let width = read_u32_le(camera, 8);
        let height = read_u32_le(camera, 12);
        let stride = read_u32_le(camera, 16);
        let pixel_format = read_u32_le(camera, 20);
        let raw_driver_ns = read_i64_le(camera, 24);
        let driver_flags = read_u32_le(camera, 32);
        let reserved = read_u32_le(camera, 36);
        let session_generation = read_u64_le(camera, 40);
        let pixels = &application[CAMERA_PREFIX_BYTES..];
        if width == 0
            || height == 0
            || stride < width.saturating_mul(2)
            || pixel_format != V4L2_PIX_FMT_YUYV
            || reserved != 0
        {
            return Err(
                "camera metadata contains invalid dimensions, format, or reserved bytes".into(),
            );
        }
        let expected_pixel_bytes = usize::try_from(u64::from(stride) * u64::from(height))
            .map_err(|_| "YUYV frame size exceeds this process address space")?;
        if pixels.len() != expected_pixel_bytes {
            return Err(format!(
                "YUYV pixel payload has {} bytes, expected stride*height={expected_pixel_bytes}",
                pixels.len()
            ));
        }

        let received = timing.received();
        if received.clock().kind() != ClockKind::HostMonotonic || received.clock() != &host_identity
        {
            return Err("received stamp source does not match this host monotonic clock".into());
        }
        let timestamp_type = driver_flags & V4L2_BUF_FLAG_TIMESTAMP_MASK;
        let timestamp_source = driver_flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK;
        let is_monotonic_soe = timestamp_type == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC
            && timestamp_source == V4L2_BUF_FLAG_TSTAMP_SRC_SOE;
        let capture_present = if let Some(capture) = timing.capture() {
            if !namespace_offset_zero
                || capture.clock().kind() != ClockKind::HostMonotonic
                || capture.clock() != received.clock()
                || capture.clock() != &host_identity
                || capture.nanoseconds() != raw_driver_ns
                || !is_monotonic_soe
            {
                return Err(
                    "capture stamp does not match verified MONOTONIC+SOE V4L2 metadata".into(),
                );
            }
            true
        } else {
            if is_monotonic_soe && namespace_offset_zero {
                return Err("verified MONOTONIC+SOE frame is missing its capture stamp".into());
            }
            false
        };

        let sequence_u32 = sequence as u32;
        if !generations.contains_key(&session_generation)
            && generations.len() >= MAX_DRIVER_GENERATIONS
        {
            return Err(format!(
                "more than {MAX_DRIVER_GENERATIONS} driver generations observed"
            ));
        }
        if let Some(previous) = generations.get(&session_generation).copied() {
            let delta = sequence_u32.wrapping_sub(previous);
            if delta == 0 {
                return Err(format!(
                    "duplicate sequence {sequence} in driver generation {session_generation}"
                ));
            }
            if delta >= 0x8000_0000 {
                return Err(format!(
                    "out-of-order sequence {sequence} in driver generation {session_generation}"
                ));
            }
            dropped_count += u64::from(delta - 1);
        }
        generations.insert(session_generation, sequence_u32);

        emit_json(json!({
            "event": "frame_checked",
            "language": "rust",
            "sequence": sequence,
            "driver_session_generation": session_generation,
            "width": width,
            "height": height,
            "stride": stride,
            "fourcc": "YUYV",
            "driver_timestamp_ns": raw_driver_ns,
            "driver_flags": driver_flags,
            "capture_known": capture_present,
            "capture_present": capture_present,
            "received_source_id": received.clock().source_id().to_hex(),
            "pixel_bytes": pixels.len(),
            "fnv1a64": format!("{:016x}", fnv1a64(pixels))
        }))?;
        if capture_present {
            capture_known += 1;
        } else {
            capture_unknown += 1;
        }
        received_count += 1;
    }

    let pressure_after_receive = subscriber.pressure();
    let resources_at_end = local_pressure_snapshots().len();
    if received_count == 0 {
        return Err(
            "no StampedBytes camera frames were received during the requested interval".into(),
        );
    }
    if borrowed_samples(&pressure_after_receive) != 0 {
        return Err("subscriber still reports borrowed samples after processing".into());
    }
    drop(subscriber);
    let resources_after_cleanup = local_pressure_snapshots().len();
    if resources_after_cleanup != resources_before {
        return Err(format!(
            "stream resource wrappers remain after cleanup: before={resources_before}, after={resources_after_cleanup}"
        ));
    }
    let generations: Vec<u64> = generations.keys().copied().collect();
    emit_json(json!({
        "event": "consumer_summary",
        "language": "rust",
        "received": received_count,
        "estimated_sequence_drops": dropped_count,
        "duplicate_sequences": 0,
        "out_of_order_sequences": 0,
        "capture_known": capture_known,
        "capture_unknown": capture_unknown,
        "driver_session_generations": generations,
        "resource_wrappers_before": resources_before,
        "resource_wrappers_during": resources_during,
        "resource_wrappers_at_end": resources_at_end,
        "resource_wrappers_after_cleanup": resources_after_cleanup,
        "pressure_after_receive": {
            "borrowed_samples": borrowed_samples(&pressure_after_receive),
            "borrowed_high_water": pressure_after_receive.subscriber.as_ref().map_or(0, |s| s.borrowed_high_water),
            "borrow_failures": pressure_after_receive.subscriber.as_ref().map_or(0, |s| s.borrow_failures)
        },
        "actual_elapsed_ms": start.elapsed().as_millis()
    }))?;
    Ok(())
}

fn main() {
    let result = parse_options().and_then(run);
    if let Err(error) = result {
        eprintln!("rbt-time-camera-consumer: {error}");
        process::exit(1);
    }
}

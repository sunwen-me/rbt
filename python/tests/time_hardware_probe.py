#!/usr/bin/env python3
"""Borrow and validate physical-camera StampedBytes frames for the hardware gate."""

from __future__ import annotations

import argparse
import json
import sys
import time
from collections import OrderedDict

from rbt import StreamProfile, _rbt
from rbt.time import ClockKind, HostClock
from rbt.time_stream import StampedBytesSubscriber, stamped_bytes_descriptor

CAMERA_PREFIX_BYTES = 48
V4L2_BUF_FLAG_TIMESTAMP_MASK = 0x0000_E000
V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC = 0x0000_2000
V4L2_BUF_FLAG_TSTAMP_SRC_MASK = 0x0007_0000
V4L2_BUF_FLAG_TSTAMP_SRC_SOE = 0x0001_0000
FNV_OFFSET_BASIS = 14_695_981_039_346_656_037
FNV_PRIME = 1_099_511_628_211
MASK64 = (1 << 64) - 1
MAX_DRIVER_GENERATIONS = 8


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--service", required=True)
    parser.add_argument("--duration-sec", required=True, type=int)
    options = parser.parse_args()
    if options.duration_sec <= 0:
        parser.error("--duration-sec must be a positive integer")
    if not hasattr(_rbt, "_StampedBytesSubscriber"):
        parser.error("installed native rbt extension has no StampedBytesSubscriber")
    return options


def read_u32_le(view: memoryview, offset: int) -> int:
    return int.from_bytes(view[offset : offset + 4], "little")


def read_u64_le(view: memoryview, offset: int) -> int:
    return int.from_bytes(view[offset : offset + 8], "little")


def read_i64_le(view: memoryview, offset: int) -> int:
    return int.from_bytes(view[offset : offset + 8], "little", signed=True)


def fnv1a64(view: memoryview) -> str:
    value = FNV_OFFSET_BASIS
    for byte in view:
        value = ((value ^ byte) * FNV_PRIME) & MASK64
    return f"{value:016x}"


def emit(record: dict[str, object]) -> None:
    print(json.dumps(record, separators=(",", ":")), flush=True)


def run(options: argparse.Namespace) -> None:
    host_clock = HostClock()
    host_identity = host_clock.monotonic_identity
    host_source = host_identity.source_id.value
    namespace_offset_zero = not any(host_source[32:48])
    descriptor = stamped_bytes_descriptor(options.service, profile=StreamProfile.SENSOR)
    subscriber = StampedBytesSubscriber.open(descriptor)
    pressure_before = subscriber.pressure
    emit(
        {
            "event": "consumer_ready",
            "language": "python",
            "service": options.service,
            "clock_source_id": host_source.hex(),
            "namespace_offset_zero": namespace_offset_zero,
        }
    )

    start = time.monotonic()
    deadline = start + options.duration_sec
    received_count = 0
    capture_known = 0
    capture_unknown = 0
    dropped_count = 0
    generations: OrderedDict[int, int] = OrderedDict()
    retained_view_verified = False
    pressure_after_receive = pressure_before

    try:
        while time.monotonic() < deadline:
            sample = subscriber.try_receive()
            if sample is None:
                time.sleep(0.001)
                continue

            sequence = sample.sequence
            if sequence > 0xFFFF_FFFF:
                raise ValueError(f"sequence {sequence} exceeds the V4L2 u32 sequence range")
            payload = sample.payload_view()
            if not payload.readonly:
                raise ValueError("native StampedBytes payload view is writable")
            timing = sample.timing
            application = payload
            del sample
            if not retained_view_verified:
                if not payload.readonly or bytes(payload[:8]) != b"RBTVCAM1":
                    raise ValueError("read-only native view did not survive dropping its sample wrapper")
                retained_view_verified = True

            if len(application) < CAMERA_PREFIX_BYTES or bytes(application[:8]) != b"RBTVCAM1":
                raise ValueError("invalid camera application prefix")
            camera = application[:CAMERA_PREFIX_BYTES]
            width = read_u32_le(camera, 8)
            height = read_u32_le(camera, 12)
            stride = read_u32_le(camera, 16)
            pixel_format = read_u32_le(camera, 20)
            raw_driver_ns = read_i64_le(camera, 24)
            driver_flags = read_u32_le(camera, 32)
            reserved = read_u32_le(camera, 36)
            session_generation = read_u64_le(camera, 40)
            pixels = application[CAMERA_PREFIX_BYTES:]
            if (
                width == 0
                or height == 0
                or stride < width * 2
                or pixel_format != int.from_bytes(b"YUYV", "little")
                or reserved != 0
            ):
                raise ValueError("camera metadata contains invalid dimensions, format, or reserved bytes")
            expected_pixel_bytes = stride * height
            if len(pixels) != expected_pixel_bytes:
                raise ValueError(
                    f"YUYV pixel payload has {len(pixels)} bytes, expected stride*height={expected_pixel_bytes}"
                )

            received = timing.received
            if received.clock.kind is not ClockKind.HOST_MONOTONIC or received.clock != host_identity:
                raise ValueError("received stamp source does not match this host monotonic clock")
            timestamp_type = driver_flags & V4L2_BUF_FLAG_TIMESTAMP_MASK
            timestamp_source = driver_flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK
            monotonic_soe = (
                timestamp_type == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC
                and timestamp_source == V4L2_BUF_FLAG_TSTAMP_SRC_SOE
            )
            capture = timing.capture
            if capture is not None:
                if (
                    not namespace_offset_zero
                    or capture.clock.kind is not ClockKind.HOST_MONOTONIC
                    or capture.clock != received.clock
                    or capture.clock != host_identity
                    or capture.nanoseconds != raw_driver_ns
                    or not monotonic_soe
                ):
                    raise ValueError("capture stamp does not match verified MONOTONIC+SOE V4L2 metadata")
            elif monotonic_soe and namespace_offset_zero:
                raise ValueError("verified MONOTONIC+SOE frame is missing its capture stamp")

            if session_generation not in generations and len(generations) >= MAX_DRIVER_GENERATIONS:
                raise ValueError(f"more than {MAX_DRIVER_GENERATIONS} driver generations observed")
            previous = generations.get(session_generation)
            if previous is not None:
                delta = (sequence - previous) & 0xFFFF_FFFF
                if delta == 0:
                    raise ValueError(
                        f"duplicate sequence {sequence} in driver generation {session_generation}"
                    )
                if delta >= 0x8000_0000:
                    raise ValueError(
                        f"out-of-order sequence {sequence} in driver generation {session_generation}"
                    )
                dropped_count += delta - 1
            generations[session_generation] = sequence

            emit(
                {
                    "event": "frame_checked",
                    "language": "python",
                    "sequence": sequence,
                    "driver_session_generation": session_generation,
                    "width": width,
                    "height": height,
                    "stride": stride,
                    "fourcc": "YUYV",
                    "driver_timestamp_ns": raw_driver_ns,
                    "driver_flags": driver_flags,
                    "capture_known": capture is not None,
                    "capture_present": capture is not None,
                    "received_source_id": received.clock.source_id.value.hex(),
                    "pixel_bytes": len(pixels),
                    "fnv1a64": fnv1a64(pixels),
                }
            )
            if capture is not None:
                capture_known += 1
            else:
                capture_unknown += 1
            received_count += 1
            del pixels, camera, application, payload
            pressure_after_receive = subscriber.pressure

        if received_count == 0:
            raise ValueError("no StampedBytes camera frames were received during the requested interval")
        if not retained_view_verified:
            raise ValueError("no sample was available for the retained-view ownership check")
        borrowed_after_receive = pressure_after_receive.borrowed_samples
        if borrowed_after_receive != 0:
            raise ValueError("subscriber still reports borrowed samples after processing")
        subscriber.close()
        subscriber = None
        emit(
            {
                "event": "consumer_summary",
                "language": "python",
                "received": received_count,
                "estimated_sequence_drops": dropped_count,
                "duplicate_sequences": 0,
                "out_of_order_sequences": 0,
                "capture_known": capture_known,
                "capture_unknown": capture_unknown,
                "driver_session_generations": sorted(generations),
                "actual_elapsed_ms": int((time.monotonic() - start) * 1000),
                "retained_view_readonly_after_sample_drop": retained_view_verified,
                "resources": {
                    "subscriber_closed": True,
                    "borrowed_samples_before_cleanup": borrowed_after_receive,
                },
                "pressure_before_receive": {
                    "borrowed_samples": pressure_before.borrowed_samples,
                    "borrowed_high_water": (
                        pressure_before.subscriber.borrowed_high_water
                        if pressure_before.subscriber is not None
                        else 0
                    ),
                },
                "pressure_after_receive": {
                    "borrowed_samples": pressure_after_receive.borrowed_samples,
                    "borrowed_high_water": (
                        pressure_after_receive.subscriber.borrowed_high_water
                        if pressure_after_receive.subscriber is not None
                        else 0
                    ),
                    "borrow_failures": pressure_after_receive.borrow_failures,
                },
            }
        )
    finally:
        if subscriber is not None:
            subscriber.close()


if __name__ == "__main__":
    try:
        run(parse_args())
    except Exception as error:
        print(f"time_hardware_probe: {error}", file=sys.stderr, flush=True)
        raise SystemExit(1) from error

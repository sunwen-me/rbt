#!/usr/bin/env python3
"""Publish or validate one fixed StampedBytes sample through the native stream."""

from __future__ import annotations

import argparse
import gc
import json
import pathlib
import sys
import time
import struct

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "python"))

from rbt import StreamProfile  # noqa: E402
from rbt.time import ClockIdentity, ClockKind, ClockSourceId, HostClock, SensorTiming, Timestamp  # noqa: E402
from rbt.time_stream import (  # noqa: E402
    StampedBytesPublisher,
    StampedBytesSubscriber,
    stamped_bytes_descriptor,
)

SEQUENCE = 4242
APPLICATION = bytes(range(32))
SOURCE_ID = "44" * 16 + "00" * 32
EXPECTED_FNV1A64 = "e6cb594c1a148ac5"


def timing() -> SensorTiming:
    source = ClockSourceId.from_device_uuid(bytes([0x44]) * 16)
    clock = ClockIdentity(ClockKind.DEVICE, source, 7)
    return SensorTiming(
        Timestamp(clock, 123_456_789),
        Timestamp(clock, 987_654_321),
        2,
        1,
    )


def validate(sample_sequence: int, sample_timing: SensorTiming, application: memoryview) -> None:
    capture = sample_timing.capture
    received = sample_timing.received
    if sample_sequence != SEQUENCE:
        raise RuntimeError(f"sequence mismatch: {sample_sequence}")
    if capture is None or capture.clock.kind is not ClockKind.DEVICE:
        raise RuntimeError("capture timestamp is absent or has the wrong kind")
    if (
        capture.clock.source_id.hex96 != SOURCE_ID
        or capture.clock.generation != 7
        or capture.nanoseconds != 123_456_789
    ):
        raise RuntimeError("capture timestamp differs from the fixed fixture")
    if (
        received.clock.kind is not ClockKind.DEVICE
        or received.clock.source_id.hex96 != SOURCE_ID
        or received.clock.generation != 7
        or received.nanoseconds != 987_654_321
    ):
        raise RuntimeError("receipt timestamp differs from the fixed fixture")
    if (
        sample_timing.capture_uncertainty_ns != 2
        or sample_timing.received_uncertainty_ns != 1
        or bytes(application) != APPLICATION
    ):
        raise RuntimeError("uncertainty or application bytes differ from the fixed fixture")
    if fnv1a64(application) != EXPECTED_FNV1A64:
        raise RuntimeError("application checksum differs from the fixture")


def fnv1a64(data: memoryview | bytes) -> str:
    value = 14_695_981_039_346_656_037
    for byte in data:
        value = ((value ^ byte) * 1_099_511_628_211) & ((1 << 64) - 1)
    return f"{value:016x}"


def publish(service: str) -> None:
    descriptor = stamped_bytes_descriptor(service, profile=StreamProfile.SENSOR)
    if descriptor.message_type.layout_hash != "97e9a924992dedfe83196197cccb6b35":
        raise RuntimeError("native StampedBytes layout hash differs from the frozen fixture")
    publisher = StampedBytesPublisher.open(
        descriptor, maximum_payload_bytes=len(APPLICATION)
    )
    print("RBT_TIME_STREAM READY_TO_PUBLISH", flush=True)
    time.sleep(5)
    loan = publisher.loan(timing(), len(APPLICATION), sequence=SEQUENCE)
    view = loan.payload_view()
    view[:] = APPLICATION
    view.release()
    loan.send()
    print(
        json.dumps(
            {"status": "sent", "sequence": SEQUENCE, "payload_len": len(APPLICATION)},
            separators=(",", ":"),
        ),
        flush=True,
    )
    publisher.close()


def consume(service: str) -> None:
    descriptor = stamped_bytes_descriptor(service, profile=StreamProfile.SENSOR)
    if descriptor.message_type.layout_hash != "97e9a924992dedfe83196197cccb6b35":
        raise RuntimeError("native StampedBytes layout hash differs from the frozen fixture")
    subscriber = StampedBytesSubscriber.open(descriptor)
    print("RBT_TIME_STREAM READY_TO_RECEIVE", flush=True)
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        sample = subscriber.receive(timeout=0.1)
        if sample is None:
            continue
        view = sample.payload_view()
        if not view.readonly:
            raise RuntimeError("received application view is not read-only")
        validate(sample.sequence, sample.timing, view)
        sequence = sample.sequence
        del sample
        gc.collect()
        # The native receive owner must stay alive through the exported view.
        validate(sequence, timing(), view)
        result = {
            "status": "received",
            "sequence": sequence,
            "payload_len": len(view),
            "fnv1a64": fnv1a64(view),
            "capture_ns": 123_456_789,
            "received_ns": 987_654_321,
            "capture_uncertainty_ns": 2,
            "received_uncertainty_ns": 1,
            "retained_view_readonly": view.readonly,
            "retained_view_after_sample_drop": bytes(view) == APPLICATION,
        }
        print(json.dumps(result, separators=(",", ":")), flush=True)
        subscriber.close()
        return
    raise TimeoutError("timed out waiting for a StampedBytes sample")


def camera_consume(service: str, duration_seconds: int) -> None:
    if duration_seconds < 1:
        raise ValueError("camera-consume duration must be positive")
    descriptor = stamped_bytes_descriptor(service, profile=StreamProfile.SENSOR)
    subscriber = StampedBytesSubscriber.open(descriptor)
    host_clock = HostClock()
    expected_clock = host_clock.monotonic_identity
    print("RBT_TIME_CAMERA_CONSUMER READY", flush=True)
    deadline = time.monotonic() + duration_seconds
    received = 0
    capture_known = 0
    capture_unknown = 0
    generations: set[int] = set()
    while time.monotonic() < deadline:
        sample = subscriber.receive(timeout=0.1)
        if sample is None:
            continue
        application = sample.payload_view()
        if len(application) < 48 or bytes(application[:8]) != b"RBTVCAM1":
            raise RuntimeError("camera application prefix is absent or invalid")
        width, height, stride, fourcc, raw_ns, flags, reserved, generation = struct.unpack_from(
            "<IIIIqIIQ", application, 8
        )
        if (
            width == 0
            or height == 0
            or stride < width * 2
            or fourcc != 0x56595559
            or reserved != 0
        ):
            raise RuntimeError("camera format metadata is invalid")
        pixels = application[48:]
        minimum = (height - 1) * stride + width * 2
        maximum = height * stride
        if len(pixels) < minimum or len(pixels) > maximum:
            raise RuntimeError("camera pixel payload size is inconsistent with format metadata")
        timing_value = sample.timing
        if (
            timing_value.received.clock != expected_clock
            or timing_value.received.clock.kind is not ClockKind.HOST_MONOTONIC
            or timing_value.capture_uncertainty_ns is not None
            or timing_value.received_uncertainty_ns is not None
        ):
            raise RuntimeError("camera receipt timing is not from this host monotonic clock")
        if timing_value.capture is not None:
            if (
                timing_value.capture.clock != timing_value.received.clock
                or timing_value.capture.nanoseconds != raw_ns
                or flags & 0x0000E000 != 0x00002000
                or flags & 0x00070000 != 0x00010000
            ):
                raise RuntimeError("camera capture stamp is not proven by V4L2 flags")
            capture_known += 1
        else:
            capture_unknown += 1
        sequence = sample.sequence
        generations.add(generation)
        print(
            json.dumps(
                {
                    "event": "frame_checked",
                    "sequence": sequence,
                    "driver_session_generation": generation,
                    "driver_timestamp_ns": raw_ns,
                    "driver_flags": flags,
                    "capture_present": timing_value.capture is not None,
                    "received_source_id": timing_value.received.clock.source_id.hex96,
                    "pixel_bytes": len(pixels),
                    "fnv1a64": fnv1a64(pixels),
                },
                separators=(",", ":"),
            ),
            flush=True,
        )
        received += 1
    subscriber.close()
    if received == 0:
        raise RuntimeError("no physical camera frames received")
    print(
        json.dumps(
            {
                "event": "consumer_summary",
                "received": received,
                "capture_known": capture_known,
                "capture_unknown": capture_unknown,
                "session_generations": sorted(generations),
            },
            separators=(",", ":"),
        ),
        flush=True,
    )


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("publish", "consume", "camera-consume"))
    parser.add_argument("service")
    parser.add_argument("duration", nargs="?", type=int)
    arguments = parser.parse_args()
    if arguments.mode == "publish":
        publish(arguments.service)
    elif arguments.mode == "consume":
        consume(arguments.service)
    else:
        if arguments.duration is None:
            parser.error("camera-consume requires DURATION_SEC")
        camera_consume(arguments.service, arguments.duration)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Diagnostic native-endpoint consumer for Orbbec StampedBytes frames.

Pass the actual service name copied from the driver's activation JSON or an
``rbt graph`` descriptor. This tool opens that endpoint directly; it does not
resolve Composer requirements. After provider replacement, start it with the
current service name.
"""

from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any, Sequence

import numpy as np

_DRIVER_PYTHON = Path(__file__).resolve().parent
if str(_DRIVER_PYTHON) not in sys.path:
    sys.path.insert(0, str(_DRIVER_PYTHON))

from rbt import StreamProfile  # noqa: E402
from orbbec_frames import OrbbecFrame, parse_frame  # noqa: E402
from rbt.time_stream import (  # noqa: E402
    StampedBytesSample,
    StampedBytesSubscriber,
    stamped_bytes_descriptor,
)


_STREAMS = ("color", "depth", "left-ir", "right-ir", "points", "accel", "gyro")


def _positive_count(value: str) -> int:
    try:
        result = int(value, 10)
    except ValueError as error:
        raise argparse.ArgumentTypeError("count must be a positive integer") from error
    if result <= 0:
        raise argparse.ArgumentTypeError("count must be a positive integer")
    return result


def _positive_timeout(value: str) -> float:
    try:
        result = float(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError("timeout-sec must be a positive number") from error
    if not math.isfinite(result) or result <= 0.0:
        raise argparse.ArgumentTypeError("timeout-sec must be a positive finite number")
    return result


def _arguments(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Diagnostic direct-endpoint consumer. Pass the actual service name "
            "from the driver's activation JSON or an rbt graph descriptor. "
            "It does not resolve Composer requirements."
        )
    )
    parser.add_argument("--service", required=True, help="actual StampedBytes service name")
    parser.add_argument("--stream", required=True, choices=_STREAMS)
    parser.add_argument("--count", type=_positive_count, default=100)
    parser.add_argument(
        "--timeout-sec",
        type=_positive_timeout,
        default=10.0,
        help="maximum wait for each incoming sample (per frame; default: 10 seconds)",
    )
    parser.add_argument(
        "--save-first",
        type=Path,
        help="save the first frame's raw array as an explicit .npy copy",
    )
    parser.add_argument(
        "--summary",
        action="store_true",
        help="emit one summary record after consuming the requested samples",
    )
    args = parser.parse_args(argv)
    if not args.service or args.service != args.service.strip():
        parser.error("--service must be a non-empty service name without surrounding whitespace")
    if args.save_first is not None and args.save_first.suffix != ".npy":
        parser.error("--save-first path must end in .npy")
    return args


def _frame_record(
    sample: StampedBytesSample,
    expected_stream: str,
    *,
    copy_pixels: bool,
) -> tuple[dict[str, Any], np.ndarray[Any, Any] | None]:
    """Build metadata and optionally copy pixels while the native loan is held."""
    frame: OrbbecFrame = parse_frame(sample.payload_view())
    if frame.kind != expected_stream:
        raise ValueError(
            f"service carried {frame.kind!r} frames, requested {expected_stream!r}"
        )

    capture = sample.timing.capture
    capture_fields: dict[str, Any]
    if capture is None:
        capture_fields = {
            "capture_clock": None,
            "capture_source_id": None,
            "capture_generation": None,
            "capture_ns": None,
        }
    else:
        capture_fields = {
            "capture_clock": capture.clock.kind.value,
            "capture_source_id": capture.clock.source_id.hex96,
            "capture_generation": capture.clock.generation,
            "capture_ns": capture.nanoseconds,
        }

    record = {
        "event": "frame",
        "sequence": sample.sequence,
        "frame_index": frame.frame_index,
        "stream": frame.kind,
        "format": frame.format,
        "frame_id": frame.frame_id,
        "depth_unit_m": frame.depth_unit_m,
        "shape": list(frame.shape),
        **capture_fields,
        "capture_uncertainty_ns": sample.timing.capture_uncertainty_ns,
        "received_uncertainty_ns": sample.timing.received_uncertainty_ns,
    }
    saved = np.array(frame.pixels, copy=True, order="C") if copy_pixels else None
    return record, saved


def _json_line(value: dict[str, Any]) -> None:
    print(json.dumps(value, separators=(",", ":"), allow_nan=False), flush=True)


class _SampleSummary:
    """Constant-space counters and continuity checks for received samples."""

    __slots__ = (
        "stream",
        "count",
        "first_sequence",
        "last_sequence",
        "first_frame_index",
        "last_frame_index",
        "sequence_gaps",
        "device_index_gaps",
        "duplicate_samples",
        "_last_payload",
        "capture_source_id",
        "generation",
        "first_capture_ns",
        "last_capture_ns",
        "_previous_sequence",
        "_previous_frame_index",
        "_previous_capture_identity",
        "_previous_capture_ns",
        "_sequence_error",
        "_device_index_error",
        "_capture_time_error",
    )

    def __init__(self, stream: str) -> None:
        self.stream = stream
        self.count = 0
        self.first_sequence: int | None = None
        self.last_sequence: int | None = None
        self.first_frame_index: int | None = None
        self.last_frame_index: int | None = None
        self.sequence_gaps = 0
        self.device_index_gaps = 0
        self.duplicate_samples = 0
        self._last_payload: bytes | None = None
        self.capture_source_id: str | None = None
        self.generation: int | None = None
        self.first_capture_ns: int | None = None
        self.last_capture_ns: int | None = None
        self._previous_sequence: int | None = None
        self._previous_frame_index: int | None = None
        self._previous_capture_identity: tuple[str, str, int] | None = None
        self._previous_capture_ns: int | None = None
        self._sequence_error: str | None = None
        self._device_index_error: str | None = None
        self._capture_time_error: str | None = None

    @staticmethod
    def _gap_count(previous: int, current: int) -> int:
        return max(current - previous - 1, 1)

    def observe(self, sample: StampedBytesSample, frame_index: int) -> bool:
        sequence = sample.sequence
        if self.stream in ("accel", "gyro"):
            payload = bytes(sample.payload_view())
            if self.count and sequence == self._previous_sequence:
                if payload != self._last_payload or frame_index != self._previous_frame_index:
                    raise ValueError("conflicting IMU retransmission")
                self.duplicate_samples += 1
                return False
            self._last_payload = payload
        if self.count == 0:
            self.first_sequence = sequence
            self.first_frame_index = frame_index
        else:
            assert self._previous_sequence is not None
            assert self._previous_frame_index is not None
            expected_sequence = self._previous_sequence + 1
            if sequence != expected_sequence:
                self.sequence_gaps += self._gap_count(self._previous_sequence, sequence)
                if self.stream in ("accel", "gyro") and self._sequence_error is None:
                    kind = "gap" if sequence > expected_sequence else "regression"
                    self._sequence_error = (
                        f"RBT sequence {kind}: expected {expected_sequence}, got {sequence}"
                    )

            expected_frame_index = self._previous_frame_index + 1
            if frame_index != expected_frame_index:
                self.device_index_gaps += self._gap_count(
                    self._previous_frame_index, frame_index
                )
                if self.stream in ("accel", "gyro") and self._device_index_error is None:
                    kind = "gap" if frame_index > expected_frame_index else "regression"
                    self._device_index_error = (
                        f"SDK frame_index {kind}: expected {expected_frame_index}, "
                        f"got {frame_index}"
                    )

        self._previous_sequence = sequence
        self._previous_frame_index = frame_index
        self.last_sequence = sequence
        self.last_frame_index = frame_index
        self.count += 1

        capture = sample.timing.capture
        if capture is None:
            if self.stream in ("accel", "gyro"):
                self._capture_time_error = self._capture_time_error or "IMU capture timestamp is unavailable"
            return True

        clock = capture.clock
        source_id = clock.source_id.hex96
        identity = (clock.kind.value, source_id, clock.generation)
        timestamp_ns = capture.nanoseconds
        if self.stream in ("accel", "gyro"):
            if clock.kind.value != "device":
                self._capture_time_error = self._capture_time_error or "IMU capture clock must be a device clock"
            if self._previous_capture_identity is not None and identity != self._previous_capture_identity:
                self._capture_time_error = self._capture_time_error or "IMU capture clock identity changed"
        if self.capture_source_id is None:
            self.capture_source_id = source_id
            self.generation = clock.generation
            self.first_capture_ns = timestamp_ns
        elif (
            identity == self._previous_capture_identity
            and self._previous_capture_ns is not None
            and timestamp_ns < self._previous_capture_ns
            and self._capture_time_error is None
        ):
            self._capture_time_error = (
                f"capture timestamp regressed: expected at least "
                f"{self._previous_capture_ns}, got {timestamp_ns}"
            )

        self.last_capture_ns = timestamp_ns
        self._previous_capture_identity = identity
        self._previous_capture_ns = timestamp_ns
        return True

    @property
    def errors(self) -> tuple[str, ...]:
        return tuple(
            error
            for error in (
                self._sequence_error,
                self._device_index_error,
                self._capture_time_error,
            )
            if error is not None
        )

    def record(self) -> dict[str, Any]:
        return {
            "event": "summary",
            "stream": self.stream,
            "count": self.count,
            "first_sequence": self.first_sequence,
            "last_sequence": self.last_sequence,
            "first_frame_index": self.first_frame_index,
            "last_frame_index": self.last_frame_index,
            "sequence_gaps": self.sequence_gaps,
            "device_index_gaps": self.device_index_gaps,
            "duplicate_samples": self.duplicate_samples,
            "capture_source_id": self.capture_source_id,
            "generation": self.generation,
            "first_capture_ns": self.first_capture_ns,
            "last_capture_ns": self.last_capture_ns,
        }


def _receive_sample(
    subscriber: StampedBytesSubscriber, timeout: float
) -> StampedBytesSample | None:
    sample = subscriber.try_receive()
    if sample is None:
        return subscriber.receive(timeout=timeout)
    return sample


def main(argv: Sequence[str] | None = None) -> int:
    args = _arguments(argv)
    subscriber: StampedBytesSubscriber | None = None
    error_message: str | None = None
    summary = _SampleSummary(args.stream)
    try:
        profile = (
            StreamProfile.CONTROL
            if args.stream in ("accel", "gyro")
            else StreamProfile.SENSOR
        )
        descriptor = stamped_bytes_descriptor(args.service, profile=profile)
        subscriber = StampedBytesSubscriber.open(descriptor, max_borrowed=1)
        if not args.summary:
            _json_line(
                {
                    "event": "consumer_ready",
                    "mode": "diagnostic_native_endpoint",
                    "service": args.service,
                    "stream": args.stream,
                }
            )

        saved_first = False
        while summary.count < args.count:
            sample = _receive_sample(subscriber, args.timeout_sec)
            if sample is None:
                raise TimeoutError(
                    f"no {args.stream} frame arrived within {args.timeout_sec:g} seconds"
                )
            try:
                record, saved = _frame_record(
                    sample,
                    args.stream,
                    copy_pixels=args.save_first is not None and not saved_first,
                )
                if not summary.observe(sample, record["frame_index"]):
                    continue
                if not args.summary and summary.errors:
                    raise ValueError("; ".join(summary.errors))
            finally:
                # _frame_record copied saved pixels while the sample was held.
                # Its borrowed array/view then leave scope before the next receive.
                del sample

            if saved is not None:
                assert args.save_first is not None
                np.save(args.save_first, saved, allow_pickle=False)
                saved_first = True
                del saved
            if not args.summary:
                _json_line(record)
    except Exception as error:
        error_message = f"{type(error).__name__}: {error}"
    finally:
        if subscriber is not None:
            subscriber.close()

    if args.summary:
        _json_line(summary.record())
    if error_message is None and summary.errors:
        error_message = "; ".join(summary.errors)
    if error_message is not None:
        print(f"orbbec-consume: {error_message}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

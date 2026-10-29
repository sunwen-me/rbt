"""Pure in-memory tests for the Orbbec diagnostic consumer."""

from __future__ import annotations

import contextlib
import io
import struct
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "drivers/orbbec/python"))

import consume
from rbt import StreamProfile


def _frame_payload(frame_index: int, stream: str) -> bytes:
    kind, pixel_format = {
        "accel": (6, 6),
        "gyro": (7, 7),
        "points": (5, 5),
    }[stream]
    encoded = bytearray(140)
    encoded[:8] = b"RBTORB01"
    struct.pack_into("<IIIII", encoded, 8, kind, pixel_format, 1, 1, 12)
    struct.pack_into("<QdQ", encoded, 32, frame_index, 1.0, 12)
    encoded[64:128] = b"orbbec335/depth_optical" + bytes(41)
    struct.pack_into("<3f", encoded, 128, 1.0, 2.0, 3.0)
    return bytes(encoded)


def _sample(
    sequence: int,
    frame_index: int,
    capture_ns: int | None = None,
    stream: str = "accel",
) -> SimpleNamespace:
    capture = None
    if capture_ns is not None:
        clock = SimpleNamespace(
            kind=SimpleNamespace(value="device"),
            source_id=SimpleNamespace(hex96="00112233445566778899aabb"),
            generation=3,
        )
        capture = SimpleNamespace(clock=clock, nanoseconds=capture_ns)
    timing = SimpleNamespace(
        capture=capture,
        capture_uncertainty_ns=None,
        received_uncertainty_ns=None,
    )
    return SimpleNamespace(
        sequence=sequence,
        timing=timing,
        payload_view=lambda: memoryview(_frame_payload(frame_index, stream)),
    )


class _FakeSubscriber:
    def __init__(self, samples, *, empty_try_once: bool = False) -> None:
        self._samples = iter(samples)
        self._empty_try_once = empty_try_once
        self._tried_empty = False
        self.receive_calls = 0
        self.closed = False

    def try_receive(self):
        if self._empty_try_once and not self._tried_empty:
            self._tried_empty = True
            return None
        return next(self._samples, None)

    def receive(self, timeout=None):
        self.receive_calls += 1
        return next(self._samples, None)

    def close(self):
        self.closed = True


class OrbbecConsumerTests(unittest.TestCase):
    def _run(self, subscriber, *arguments):
        records = []
        stderr = io.StringIO()
        with (
            mock.patch.object(
                consume, "stamped_bytes_descriptor", return_value=object()
            ) as descriptor,
            mock.patch.object(consume.StampedBytesSubscriber, "open", return_value=subscriber),
            mock.patch.object(consume, "_json_line", side_effect=records.append),
            contextlib.redirect_stderr(stderr),
        ):
            result = consume.main(["--service", "orbbec/test", *arguments])
        return result, records, stderr.getvalue(), descriptor

    def test_imu_uses_control_profile_and_video_uses_sensor_profile(self) -> None:
        for stream, profile in (
            ("accel", StreamProfile.CONTROL),
            ("points", StreamProfile.SENSOR),
        ):
            with self.subTest(stream=stream):
                subscriber = _FakeSubscriber([_sample(8, 12, stream=stream)])
                result, records, stderr, descriptor = self._run(
                    subscriber, "--stream", stream, "--count", "1"
                )
                self.assertEqual(result, 0, stderr)
                self.assertEqual(records[0]["event"], "consumer_ready")
                self.assertEqual(records[1]["event"], "frame")
                descriptor.assert_called_once_with("orbbec/test", profile=profile)
                self.assertTrue(subscriber.closed)

    def test_try_receive_precedes_the_bounded_wait(self) -> None:
        subscriber = _FakeSubscriber([_sample(8, 12)], empty_try_once=True)
        result, _, stderr, _ = self._run(
            subscriber, "--stream", "accel", "--count", "1"
        )
        self.assertEqual(result, 0, stderr)
        self.assertEqual(subscriber.receive_calls, 1)

    def test_per_frame_mode_fails_at_the_first_imu_sequence_gap(self) -> None:
        subscriber = _FakeSubscriber([_sample(10, 20), _sample(12, 21)])
        result, records, stderr, _ = self._run(
            subscriber, "--stream", "accel", "--count", "2"
        )
        self.assertEqual(result, 1)
        self.assertEqual(
            [record["event"] for record in records], ["consumer_ready", "frame"]
        )
        self.assertIn("RBT sequence gap: expected 11, got 12", stderr)

    def test_summary_is_constant_space_and_reports_all_samples(self) -> None:
        subscriber = _FakeSubscriber(
            (_sample(sequence, sequence + 100) for sequence in range(2000))
        )
        result, records, stderr, _ = self._run(
            subscriber,
            "--stream",
            "accel",
            "--count",
            "2000",
            "--summary",
        )
        self.assertEqual(result, 0, stderr)
        self.assertEqual(len(records), 1)
        self.assertEqual(
            records[0],
            {
                "event": "summary",
                "stream": "accel",
                "count": 2000,
                "first_sequence": 0,
                "last_sequence": 1999,
                "first_frame_index": 100,
                "last_frame_index": 2099,
                "sequence_gaps": 0,
                "device_index_gaps": 0,
                "capture_source_id": None,
                "generation": None,
                "first_capture_ns": None,
                "last_capture_ns": None,
            },
        )
        self.assertNotIn("samples", consume._SampleSummary.__slots__)

    def test_summary_reports_imu_gaps_and_capture_time_regressions(self) -> None:
        subscriber = _FakeSubscriber(
            [_sample(10, 20, 5), _sample(12, 22, 4), _sample(13, 23, 4)]
        )
        result, records, stderr, _ = self._run(
            subscriber,
            "--stream",
            "accel",
            "--count",
            "3",
            "--summary",
        )
        self.assertEqual(result, 1)
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["count"], 3)
        self.assertEqual(records[0]["sequence_gaps"], 1)
        self.assertEqual(records[0]["device_index_gaps"], 1)
        self.assertEqual(records[0]["first_capture_ns"], 5)
        self.assertEqual(records[0]["last_capture_ns"], 4)
        self.assertIn("RBT sequence gap: expected 11, got 12", stderr)
        self.assertIn("SDK frame_index gap: expected 21, got 22", stderr)
        self.assertIn("capture timestamp regressed", stderr)

    def test_imu_sequence_and_device_index_regressions_are_errors(self) -> None:
        subscriber = _FakeSubscriber(
            [_sample(10, 20, stream="gyro"), _sample(9, 19, stream="gyro")]
        )
        result, records, stderr, _ = self._run(
            subscriber,
            "--stream",
            "gyro",
            "--count",
            "2",
            "--summary",
        )
        self.assertEqual(result, 1)
        self.assertEqual(records[0]["sequence_gaps"], 1)
        self.assertEqual(records[0]["device_index_gaps"], 1)
        self.assertIn("RBT sequence regression: expected 11, got 9", stderr)
        self.assertIn("SDK frame_index regression: expected 21, got 19", stderr)

    def test_duplicate_capture_timestamps_are_allowed(self) -> None:
        subscriber = _FakeSubscriber([_sample(10, 20, 5), _sample(11, 21, 5)])
        result, records, stderr, _ = self._run(
            subscriber,
            "--stream",
            "accel",
            "--count",
            "2",
            "--summary",
        )
        self.assertEqual(result, 0, stderr)
        self.assertEqual(records[0]["first_capture_ns"], 5)
        self.assertEqual(records[0]["last_capture_ns"], 5)


if __name__ == "__main__":
    unittest.main()

from __future__ import annotations

import importlib.util
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import MagicMock, patch

spec = importlib.util.spec_from_file_location("orbbec_consume", Path(__file__).parents[1] / "python/consume.py")
consume = importlib.util.module_from_spec(spec)
spec.loader.exec_module(consume)


def sample(sequence=1, capture_ns=1000, source="a" * 96, generation=0, kind="device"):
    clock = SimpleNamespace(kind=SimpleNamespace(value=kind), source_id=SimpleNamespace(hex96=source), generation=generation)
    payload = f"{sequence}|{capture_ns}|{source}|{generation}|{kind}".encode()
    return SimpleNamespace(sequence=sequence, timing=SimpleNamespace(capture=SimpleNamespace(clock=clock, nanoseconds=capture_ns)), payload_view=lambda: memoryview(payload))


class ConsumeTests(unittest.TestCase):
    def test_contiguous_summary(self):
        summary = consume._SampleSummary("accel")
        for i in range(100):
            summary.observe(sample(i + 10, i * 5000), i + 30)
        record = summary.record()
        self.assertEqual(record["count"], 100)
        self.assertEqual(record["last_sequence"], 109)
        self.assertEqual(record["last_frame_index"], 129)
        self.assertEqual(record["sequence_gaps"], 0)
        self.assertEqual(record["device_index_gaps"], 0)
        self.assertEqual(summary.errors, ())
        self.assertFalse(hasattr(summary, "__dict__"))

    def test_each_imu_gap_and_regression_fails(self):
        for stream in ("accel", "gyro"):
            for seq, index, message in ((3, 2, "RBT sequence gap"), (0, 2, "RBT sequence regression"),
                                        (2, 3, "SDK frame_index gap"), (2, 1, "SDK frame_index regression")):
                with self.subTest(stream=stream, seq=seq, index=index):
                    summary = consume._SampleSummary(stream)
                    summary.observe(sample(), 1)
                    summary.observe(sample(seq), index)
                    self.assertTrue(any(message in error for error in summary.errors))

    def test_identical_retry_is_deduplicated_and_conflicts_fail(self):
        summary = consume._SampleSummary("accel")
        self.assertTrue(summary.observe(sample(), 1))
        self.assertFalse(summary.observe(sample(), 1))
        self.assertEqual(summary.count, 1)
        self.assertEqual(summary.duplicate_samples, 1)
        self.assertEqual(summary.errors, ())
        with self.assertRaisesRegex(ValueError, "conflicting IMU retransmission"):
            summary.observe(sample(capture_ns=2000), 1)

    def test_video_gaps_are_observed_without_strict_failure(self):
        summary = consume._SampleSummary("depth")
        summary.observe(sample(), 1)
        summary.observe(sample(5), 7)
        self.assertEqual(summary.errors, ())
        self.assertEqual(summary.sequence_gaps, 3)
        self.assertEqual(summary.device_index_gaps, 5)

    def test_imu_clock_and_timestamp_errors(self):
        for changed in (sample(2, 500), sample(2, source="b" * 96), sample(2, generation=1), sample(2, kind="system_realtime")):
            summary = consume._SampleSummary("gyro")
            summary.observe(sample(), 1)
            summary.observe(changed, 2)
            self.assertTrue(summary.errors)
        missing = sample()
        missing.timing.capture = None
        summary = consume._SampleSummary("gyro")
        summary.observe(missing, 1)
        self.assertTrue(summary.errors)

    def test_profiles_summary_and_immediate_drain(self):
        for stream in ("accel", "gyro", "depth", "color"):
            sub = MagicMock()
            sub.try_receive.return_value = sample()
            emitted = []
            with patch.object(consume.StampedBytesSubscriber, "open", return_value=sub), \
                 patch.object(consume, "stamped_bytes_descriptor", return_value="descriptor") as descriptor, \
                 patch.object(consume, "_frame_record", return_value=({"frame_index": 1}, None)), \
                 patch.object(consume, "_json_line", side_effect=emitted.append):
                code = consume.main(["--service", "test/service", "--stream", stream, "--count", "1", "--summary"])
            self.assertEqual(code, 0)
            expected = consume.StreamProfile.CONTROL if stream in ("accel", "gyro") else consume.StreamProfile.SENSOR
            self.assertEqual(descriptor.call_args.kwargs["profile"], expected)
            sub.receive.assert_not_called()
            sub.close.assert_called_once()
            self.assertEqual(len(emitted), 1)
            self.assertEqual(emitted[0]["event"], "summary")

    def test_empty_queue_uses_native_event_wait(self):
        sub = MagicMock()
        sub.try_receive.return_value = None
        expected = sample()
        sub.receive.return_value = expected
        self.assertIs(consume._receive_sample(sub, 2), expected)
        sub.receive.assert_called_once_with(timeout=2)


if __name__ == "__main__":
    unittest.main()

"""Focused tests for the hardware gate's live JSONL tail reader."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import test_tf_hardware as gate


class _FlushOnly:
    def flush(self) -> None:
        pass


def _live_reader(path: Path) -> gate.ManagedProcess:
    process = object.__new__(gate.ManagedProcess)
    process.name = "fixture"
    process.out_path = path
    process.stdout = _FlushOnly()
    process.last_offset = 0
    process.record_reader = gate.JsonlRecordReader("fixture")
    path.write_bytes(b"")
    return process


class HardwareGateJsonlTest(unittest.TestCase):
    def test_split_lines_and_multiple_records_are_delivered_once_complete(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.jsonl"
            process = _live_reader(path)

            with path.open("ab") as output:
                output.write(b'{"event":"cam')
            self.assertEqual([], process.read_records())

            with path.open("ab") as output:
                output.write(b'era_ready"}\n{"event":"frame","sequence":7}\n{"event":"tail')
            self.assertEqual(
                [{"event": "camera_ready"}, {"event": "frame", "sequence": 7}],
                process.read_records(),
            )

            with path.open("ab") as output:
                output.write(b'"}\n')
            self.assertEqual([{"event": "tail"}], process.read_records())
            self.assertEqual([], process.finish_records())

    def test_final_eof_rejects_an_unterminated_record(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.jsonl"
            process = _live_reader(path)
            with path.open("ab") as output:
                output.write(b'{"event":"unfinished"}')

            self.assertEqual([], process.read_records())
            with self.assertRaisesRegex(gate.GateFailure, "incomplete JSONL record"):
                process.finish_records()

    def test_oversized_complete_line_and_unterminated_tail_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.jsonl"
            process = _live_reader(path)
            with path.open("ab") as output:
                output.write(b"x" * (gate.MAX_RECORD_BYTES + 1) + b"\n")
            with self.assertRaisesRegex(gate.GateFailure, "oversized JSONL record"):
                process.read_records()

            process = _live_reader(path)
            with path.open("ab") as output:
                output.write(b"x" * (gate.MAX_RECORD_BYTES + 1))
            with self.assertRaisesRegex(gate.GateFailure, "oversized JSONL record"):
                process.read_records()

    def test_full_log_loader_ignores_only_the_current_partial_tail(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.jsonl"
            path.write_bytes(b'{"event":"complete"}\n{"event":"partial"')
            self.assertEqual([{"event": "complete"}], gate.load_jsonl(path))


if __name__ == "__main__":
    unittest.main()

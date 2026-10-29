from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from textwrap import dedent
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import test_tf_interop as harness  # noqa: E402


class TfInteropHarnessTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = harness.read_semantics_fixture()

    def valid_semantics(self, language: str = "rust") -> dict[str, Any]:
        result = dict(self.fixture["expected"])
        result["language"] = language
        return result

    def test_semantics_accepts_equivalent_negative_quaternion(self):
        result = self.valid_semantics("cpp")
        result["interpolated_quaternion_xyzw"] = [
            -value for value in result["interpolated_quaternion_xyzw"]
        ]

        checked = harness.validate_semantics_result(result, "cpp", self.fixture)

        self.assertEqual(checked, result)

    def test_semantics_rejects_missing_field(self):
        result = self.valid_semantics()
        del result["mapped_point"]

        with self.assertRaisesRegex(harness.HarnessError, "missing fields.*mapped_point"):
            harness.validate_semantics_result(result, "rust", self.fixture)

    def test_semantics_rejects_wrong_numeric_result(self):
        result = self.valid_semantics("python")
        result["direction_point"] = [1.01, 0.0, 1.0]

        with self.assertRaisesRegex(harness.HarnessError, "direction_point differs"):
            harness.validate_semantics_result(result, "python", self.fixture)

    def test_semantics_rejects_unapproved_extra_field(self):
        result = self.valid_semantics()
        result["actual_payload"] = "unexpected"

        with self.assertRaisesRegex(harness.HarnessError, "unexpected fields"):
            harness.validate_semantics_result(result, "rust", self.fixture)

    def test_consumer_requires_all_interop_guards(self):
        result = {
            "status": "PASS",
            "language": "rust",
            "static_snapshot": "PASS",
            "static_update": "PASS",
            "dynamic": "PASS",
            "lookup": "PASS",
            "static_revision": 11,
            "dynamic_samples": 2,
            "borrowed_samples": 0,
        }

        with self.assertRaisesRegex(harness.HarnessError, "pending_rpc"):
            harness.validate_interop_result(result, "rust", "consumer")

    def test_publisher_and_consumer_share_dynamic_sample_count_schema(self):
        result = {
            "status": "PASS",
            "language": "rust",
            "static_snapshot": "PASS",
            "static_update": "PASS",
            "dynamic": "PASS",
            "lookup": "PASS",
            "static_revision": 11,
            "dynamic_samples": 2,
            "borrowed_samples": 0,
            "pending_rpc": 0,
        }

        self.assertEqual(
            harness.validate_interop_result(result, "rust", "publisher")["dynamic_samples"],
            2,
        )
        self.assertEqual(
            harness.validate_interop_result(result, "rust", "consumer")["dynamic_samples"],
            2,
        )

    def test_publisher_must_report_both_published_dynamic_samples(self):
        result = {
            "status": "PASS",
            "language": "rust",
            "static_snapshot": "PASS",
            "static_update": "PASS",
            "dynamic": "PASS",
            "lookup": "PASS",
            "static_revision": 11,
            "dynamic_samples": 1,
            "borrowed_samples": 0,
            "pending_rpc": 0,
        }

        with self.assertRaisesRegex(
            harness.HarnessError,
            "publisher rust dynamic_samples=1, expected 2",
        ):
            harness.validate_interop_result(result, "rust", "publisher")

    def test_pair_starts_late_joiner_only_after_publisher_ready(self):
        fake_code = dedent(
            """\
            #!/usr/bin/env python3
            import json
            import pathlib
            import sys
            import time

            role = sys.argv[2]
            options = dict(zip(sys.argv[3::2], sys.argv[4::2]))
            ready = pathlib.Path(options['--ready'])
            peer = pathlib.Path(options['--peer-ready'])
            done = pathlib.Path(options['--done'])
            timeout = float(options['--timeout-sec'])
            deadline = time.monotonic() + timeout

            def wait_for(path):
                while time.monotonic() < deadline:
                    if path.is_file():
                        return
                    time.sleep(0.005)
                raise SystemExit(f'timed out waiting for {path}')

            def report(language):
                print(json.dumps({
                    'status': 'PASS', 'language': language,
                    'static_snapshot': 'PASS', 'static_update': 'PASS',
                    'dynamic': 'PASS', 'lookup': 'PASS', 'static_revision': 11,
                    'dynamic_samples': 2, 'borrowed_samples': 0, 'pending_rpc': 0,
                }), flush=True)

            language = pathlib.Path(sys.argv[0]).name
            language = language.removeprefix('fake-')
            if role == 'publish':
                ready.parent.mkdir(parents=True, exist_ok=True)
                ready.write_text('ready\\n')
                wait_for(peer)
                wait_for(done)
                report(language)
            else:
                wait_for(ready)
                peer.write_text('ready\\n')
                report(language)
                done.write_text('done\\n')
            """
        )
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            rust_probe = root / "fake-rust"
            cpp_probe = root / "fake-cpp"
            for path in (rust_probe, cpp_probe):
                path.write_text(fake_code, encoding="utf-8")
                path.chmod(0o755)
            args = argparse.Namespace(
                rust_probe=rust_probe,
                cpp_probe=cpp_probe,
                python=Path(sys.executable),
                timeout_sec=3,
            )

            pair = harness.run_pair(
                args,
                root / "logs",
                "rust",
                "cpp",
                os.environ.copy(),
                set(),
            )

            self.assertTrue(pair["publisher_ready_before_consumer_start"])
            self.assertEqual(pair["status"], "PASS")
            self.assertEqual(pair["results"]["consumer"]["dynamic_samples"], 2)
            self.assertEqual(pair["cleanup_live_processes"], {})

    def test_failure_cleanup_kills_fake_probe_process_group(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            child_pid_path = root / "child.pid"
            fake_probe = root / "fake_probe.py"
            fake_probe.write_text(
                "import pathlib, subprocess, sys, time\n"
                "child = subprocess.Popen([sys.executable, '-c', "
                "'import time; time.sleep(60)'])\n"
                "pathlib.Path(sys.argv[1]).write_text(str(child.pid))\n"
                "print('deliberate fake probe failure', flush=True)\n"
                "raise SystemExit(7)\n",
                encoding="utf-8",
            )
            process = harness.ManagedProcess.start(
                name="fake-failing-probe",
                argv=[sys.executable, fake_probe, child_pid_path],
                cwd=root,
                log_dir=root / "logs",
                env=os.environ.copy(),
            )
            try:
                self.assertEqual(process.wait(timeout=5), 7)
                child_pid = int(child_pid_path.read_text(encoding="utf-8"))
                live_members = process.terminate()
                self.assertEqual(live_members, [])
                self.assertFalse(harness.process_group_alive(process.pgid))
                self.assertNotIn(child_pid, harness.live_group_members(process.pgid))
            finally:
                process.terminate()


if __name__ == "__main__":
    unittest.main()

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

from check_rpc_interop_idle import validate_idle_text  # noqa: E402


class RpcInteropIdleValidatorTests(unittest.TestCase):
    def setUp(self) -> None:
        self.client_line = (
            "RBT_RPC_INTEROP IDLE language=rust role=async-client "
            "service=rbt/test calls=4 endpoint=client "
            "snapshot_timestamp_unix_ms=1700000000000 calls_started=4 "
            "calls_completed=4 in_flight_current=0 response_guards_current=0"
        )
        self.server_line = (
            "RBT_RPC_INTEROP IDLE language=cpp role=server service=rbt/test "
            "calls=4 endpoint=server snapshot_timestamp_unix_ms=1700000000000 "
            "requests_received=4 responses_sent=4 requests_current=0"
        )

    def validate_client(self, line: str) -> dict[str, str]:
        return validate_idle_text(
            line,
            language="rust",
            role="async-client",
            service="rbt/test",
            calls=4,
        )

    def test_accepts_client_and_server_snapshots(self) -> None:
        self.assertEqual(
            self.validate_client(self.client_line)["response_guards_current"], "0"
        )
        fields = validate_idle_text(
            self.server_line,
            language="cpp",
            role="server",
            service="rbt/test",
            calls=4,
        )
        self.assertNotIn("response_guards_current", fields)

    def test_rejects_missing_or_duplicate_idle_line(self) -> None:
        with self.assertRaisesRegex(ValueError, "found 0"):
            self.validate_client("RBT_RPC_INTEROP PASS")
        with self.assertRaisesRegex(ValueError, "found 2"):
            self.validate_client(f"{self.client_line}\n{self.client_line}")

    def test_rejects_wrong_identity_and_endpoint(self) -> None:
        with self.assertRaisesRegex(ValueError, "service mismatch"):
            self.validate_client(self.client_line.replace("rbt/test", "rbt/other"))
        with self.assertRaisesRegex(ValueError, "endpoint mismatch"):
            self.validate_client(self.client_line.replace("endpoint=client", "endpoint=server"))

    def test_rejects_missing_or_invalid_fields_and_timestamp(self) -> None:
        with self.assertRaisesRegex(ValueError, "missing response_guards_current"):
            self.validate_client(self.client_line.replace(" response_guards_current=0", ""))
        with self.assertRaisesRegex(ValueError, "snapshot_timestamp_unix_ms must be positive"):
            self.validate_client(
                self.client_line.replace(
                    "snapshot_timestamp_unix_ms=1700000000000",
                    "snapshot_timestamp_unix_ms=0",
                )
            )
        with self.assertRaisesRegex(ValueError, "non-negative integer"):
            self.validate_client(
                self.client_line.replace(
                    "snapshot_timestamp_unix_ms=1700000000000",
                    "snapshot_timestamp_unix_ms=now",
                )
            )

    def test_rejects_nonzero_gauges_and_activity_mismatch(self) -> None:
        with self.assertRaisesRegex(ValueError, "response_guards_current must be zero"):
            self.validate_client(
                self.client_line.replace(
                    "response_guards_current=0", "response_guards_current=1"
                )
            )
        with self.assertRaisesRegex(ValueError, "calls_completed mismatch"):
            self.validate_client(self.client_line.replace("calls_completed=4", "calls_completed=3"))
        with self.assertRaisesRegex(ValueError, "requests_current must be zero"):
            validate_idle_text(
                self.server_line.replace("requests_current=0", "requests_current=1"),
                language="cpp",
                role="server",
                service="rbt/test",
                calls=4,
            )

    def test_server_does_not_synthesize_client_response_guards(self) -> None:
        with self.assertRaisesRegex(ValueError, "must not include response_guards_current"):
            validate_idle_text(
                f"{self.server_line} response_guards_current=0",
                language="cpp",
                role="server",
                service="rbt/test",
                calls=4,
            )


class RpcSoakProgressAwkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        if shutil.which("awk") is None:
            raise unittest.SkipTest("awk is unavailable")
        repository = Path(__file__).resolve().parents[2]
        script = (repository / "scripts/soak_rpc.sh").read_text(encoding="utf-8")
        marker = script.index("/^RBT_RPC_SOAK_PYTHON PROGRESS /")
        awk_start = script.rfind("awk '\n", 0, marker)
        if awk_start < 0:
            raise AssertionError("could not locate the soak progress AWK program")
        program_start = awk_start + len("awk '\n")
        program_end = script.index("\n' \"${run_dir}/python.log\"", marker)
        cls.awk_program = script[program_start:program_end]

    def check_log(self, log: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["awk", self.awk_program],
            input=log,
            text=True,
            capture_output=True,
            check=False,
        )

    def test_accepts_valid_cycle_from_actual_awk_program(self) -> None:
        result = self.check_log(
            "RBT_RPC_SOAK_PYTHON PROGRESS cycles=1 client_registrations=0 "
            "server_registrations=0 in_flight_current=0 "
            "response_guards_current=0 requests_current=0\n"
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_missing_response_guard_from_actual_awk_program(self) -> None:
        result = self.check_log(
            "RBT_RPC_SOAK_PYTHON PROGRESS cycles=1 client_registrations=0 "
            "server_registrations=0 in_flight_current=0 requests_current=0\n"
        )
        self.assertNotEqual(result.returncode, 0)

    def test_rejects_nonzero_response_guard_from_actual_awk_program(self) -> None:
        result = self.check_log(
            "RBT_RPC_SOAK_PYTHON PROGRESS cycles=1 client_registrations=0 "
            "server_registrations=0 in_flight_current=0 "
            "response_guards_current=1 requests_current=0\n"
        )
        self.assertNotEqual(result.returncode, 0)

    def test_rejects_non_numeric_gauges_from_actual_awk_program(self) -> None:
        result = self.check_log(
            "RBT_RPC_SOAK_PYTHON PROGRESS cycles=1 client_registrations=0 "
            "server_registrations=0 in_flight_current=0 "
            "response_guards_current=zero requests_current=0\n"
        )
        self.assertNotEqual(result.returncode, 0)

    def test_rejects_log_without_progress_from_actual_awk_program(self) -> None:
        result = self.check_log("RBT_RPC_SOAK_PYTHON PASS\n")
        self.assertNotEqual(result.returncode, 0)


class SoakResourceSamplingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        repository = Path(__file__).resolve().parents[2]
        script = (repository / "scripts/soak_rpc.sh").read_text(encoding="utf-8")
        start = script.index("record_resource() {\n")
        end = script.index("\n}\n\nrecord_resources()", start) + 2
        cls.function = script[start:end]

    def run_function(self, pid: int, override: str = "") -> subprocess.CompletedProcess[str]:
        script = (
            "set -euo pipefail\n"
            "resource_log=/dev/stdout\n"
            f"{self.function}\n"
            f"{override}\n"
            f"record_resource test fixture {pid}\n"
        )
        return subprocess.run(
            ["bash", "--noprofile", "--norc", "-c", script],
            text=True,
            capture_output=True,
            check=False,
        )

    def test_skips_process_that_is_already_gone(self) -> None:
        result = self.run_function(99_999_999)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "")

    def test_skips_status_read_failure_without_recording_zeroes(self) -> None:
        result = self.run_function(os.getpid(), "awk() { return 1; }")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "")

    def test_skips_fd_scan_failure_without_recording_zeroes(self) -> None:
        result = self.run_function(os.getpid(), "find() { return 1; }")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, "")

    def test_records_a_live_process_with_positive_measurements(self) -> None:
        pid = os.getpid()
        result = self.run_function(pid)
        self.assertEqual(result.returncode, 0, result.stderr)
        fields = result.stdout.rstrip("\n").split("\t")
        self.assertEqual(len(fields), 6)
        self.assertEqual(fields[:3], ["test", "fixture", str(pid)])
        for value in fields[3:]:
            self.assertGreater(int(value), 0)


if __name__ == "__main__":
    unittest.main()

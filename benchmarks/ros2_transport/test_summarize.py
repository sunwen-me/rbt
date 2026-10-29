"""Focused tests for the portable ROS 2 transport result exporter."""

import csv
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from summarize import process_file, summarize_results  # noqa: E402


def trial(index: int, status: str, latency: float, trial_dir: str) -> dict:
    valid = status == "valid"
    return {
        "trial_index": index,
        "case": "imu",
        "mode": "rbt",
        "repetition": index,
        "order_in_repetition": 0,
        "status": status,
        "valid": valid,
        "reason": f"failed at {trial_dir}" if not valid else "",
        "trial_dir": trial_dir,
        "commands": ["must not be exported"],
        "alias_evidence": [{"address": "0x1234", "map": "/proc/1/maps"}],
        "metrics": {
            "valid": valid,
            "reasons": [f"detail at {trial_dir}"] if not valid else [],
            "latency_p50_us": latency,
            "total_cpu_percent": 12.0 + index,
            "median_total_pss_kib": 1000 + index,
            "peak_total_pss_kib": 1200 + index,
            "median_sum_rss_kib": 2000 + index,
            "peak_sum_rss_kib": 2400 + index,
            "publisher_rate_hz": 200.0,
            "delivery_ratio": 1.0,
            "shared_alias_matches": 8,
        },
    }


def results(trials: list[dict]) -> dict:
    return {
        "schema_version": 1,
        "created_at_utc": "2026-10-29T00:00:00Z",
        "status": "completed",
        "configuration": {
            "cases": ["imu"],
            "modes": ["rbt", "loan"],
            "rbt_executable": "/local/build/rbt_bench",
            "ros_executable": "/local/build/ros_bench",
            "ros_setup": "/opt/ros/local/setup.bash",
            "binary_sha256": {"rbt": "abc", "ros": "def"},
            "profile_sha256": {"shm": "123"},
        },
        "runtime_metadata": {"platform": "test", "rbt_sdk_prefix": "/sdk/local"},
        "trials": trials,
    }


class SummarizeTests(unittest.TestCase):
    def test_missing_configured_group_or_repetition_is_not_success(self) -> None:
        data = results([trial(1, "valid", 10.0, "/local/run/trial-1")])
        summary, _, _ = summarize_results(data, "source-revision")
        self.assertFalse(summary["all_trials_valid"])
        data["configuration"]["modes"] = ["rbt"]
        data["configuration"]["repetitions"] = 2
        summary, _, _ = summarize_results(data, "source-revision")
        self.assertFalse(summary["all_trials_valid"])
        data["configuration"]["repetitions"] = 1
        summary, _, _ = summarize_results(data, "source-revision")
        self.assertTrue(summary["all_trials_valid"])

    def test_aggregates_valid_trials_and_preserves_empty_groups(self) -> None:
        data = results(
            [
                trial(1, "valid", 10.0, "/local/run/trial-1"),
                trial(2, "valid", 30.0, "/local/run/trial-2"),
                trial(3, "invalid", 999.0, "/local/run/trial-3"),
            ]
        )
        summary, rows, _ = summarize_results(data, "source-revision")

        aggregate = summary["case_mode_aggregates"]["imu"]["rbt"]
        self.assertEqual(aggregate["valid_trial_count"], 2)
        self.assertEqual(aggregate["metrics"]["latency_p50_us"]["median"], 20.0)
        empty = summary["case_mode_aggregates"]["imu"]["loan"]
        self.assertEqual(empty["aggregate_status"], "no_valid_trials")
        self.assertIsNone(empty["metrics"]["latency_p50_us"]["median"])
        self.assertFalse(summary["all_trials_valid"])
        self.assertEqual([row["status"] for row in rows], ["valid", "valid", "invalid"])
        self.assertNotIn("trial_dir", summary)
        self.assertNotIn("commands", rows[0])
        self.assertNotIn("alias_evidence", rows[0])
        self.assertNotIn("/local/run/trial-3", rows[2]["reason"])

    def test_invalid_trials_are_written_before_nonzero_result(self) -> None:
        data = results([trial(1, "failed", 10.0, "/local/run/trial-1")])
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            input_path = root / "results.json"
            output_path = root / "summary"
            input_path.write_text(json.dumps(data), encoding="utf-8")

            self.assertEqual(process_file(input_path, output_path, "source-revision"), 1)
            self.assertNotIn(b"\r", (output_path / "trials.csv").read_bytes())
            summary = json.loads((output_path / "summary.json").read_text(encoding="utf-8"))
            with (output_path / "trials.csv").open(newline="", encoding="utf-8") as stream:
                rows = list(csv.DictReader(stream))

        self.assertFalse(summary["all_trials_valid"])
        self.assertEqual(summary["trial_status_counts"], {"failed": 1})
        self.assertEqual(rows[0]["status"], "failed")
        self.assertNotIn("/local/run/trial-1", rows[0]["quality_reasons"])


if __name__ == "__main__":
    unittest.main()

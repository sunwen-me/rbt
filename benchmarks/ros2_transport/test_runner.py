"""Focused unit tests for the ROS 2 transport benchmark runner."""

from __future__ import annotations

import os
import resource
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path
from unittest import mock

import run


class RunnerParsingTests(unittest.TestCase):
    def test_ros_profile_selectors_support_fastdds_2_and_3(self) -> None:
        environment = run.process_environment(
            {"ROS_LOCALHOST_ONLY": "1", "FASTRTPS_DEFAULT_PROFILES_FILE": "/wrong.xml"},
            "loan", 180, Path("/trial.xml")
        )
        self.assertEqual(environment["FASTDDS_DEFAULT_PROFILES_FILE"], "/trial.xml")
        self.assertEqual(environment["FASTRTPS_DEFAULT_PROFILES_FILE"], "/trial.xml")
        self.assertEqual(environment["ROS_LOCALHOST_ONLY"], "0")

    def test_topic_profile_instantiation_preserves_generic_defaults(self) -> None:
        expected_transport = {
            ("shm", "imu"): {
                "max_message_size": 65_536,
                "segment_size": 524_288,
            },
            ("shm", "cloud"): {
                "max_message_size": 2_097_152,
                "segment_size": 8_388_608,
            },
            ("shm", "video"): {
                "max_message_size": 8_388_608,
                "segment_size": 33_554_432,
            },
            ("loan", "imu"): {
                "max_message_size": 65_536,
                "segment_size": 524_288,
            },
            ("loan", "cloud"): {
                "max_message_size": 65_536,
                "segment_size": 524_288,
            },
            ("loan", "video"): {
                "max_message_size": 65_536,
                "segment_size": 524_288,
            },
        }
        source_xml = """<?xml version="1.0"?>
<dds xmlns="http://www.eprosima.com">
  <profiles>
    <transport_descriptors>
      <transport_descriptor>
        <transport_id>bench_shm</transport_id>
        <type>SHM</type>
        <segment_size>33554432</segment_size>
        <maxMessageSize>8388608</maxMessageSize>
      </transport_descriptor>
    </transport_descriptors>
    <data_writer profile_name="generic_writer" is_default_profile="true">
      <qos><data_sharing><kind>OFF</kind></data_sharing></qos>
    </data_writer>
    <data_reader profile_name="generic_reader" is_default_profile="true">
      <qos><data_sharing><kind>OFF</kind></data_sharing></qos>
    </data_reader>
    <data_writer profile_name="__BENCHMARK_TOPIC__" is_default_profile="false">
      <topic><historyQos><depth>1</depth></historyQos></topic>
      <qos><data_sharing><kind>BENCHMARK_KIND</kind></data_sharing></qos>
    </data_writer>
    <data_reader profile_name="__BENCHMARK_TOPIC__" is_default_profile="false">
      <topic><historyQos><depth>1</depth></historyQos></topic>
      <qos><data_sharing><kind>BENCHMARK_KIND</kind></data_sharing></qos>
    </data_reader>
  </profiles>
</dds>
"""
        with tempfile.TemporaryDirectory() as directory:
            template = Path(directory) / "template.xml"
            template.write_text(source_xml, encoding="utf-8")
            for mode in ("shm", "loan"):
                benchmark_kind = "OFF" if mode == "shm" else "ON"
                mode_template = Path(directory) / f"{mode}.xml"
                mode_template.write_text(
                    source_xml.replace("BENCHMARK_KIND", benchmark_kind), encoding="utf-8"
                )
                for case in run.CASES:
                    with self.subTest(mode=mode, case=case):
                        destination = Path(directory) / f"{mode}-{case}.xml"
                        run.instantiate_fastdds_profile(
                            mode_template, destination, "rbtbenchabc", mode, case
                        )
                        generated_xml = destination.read_text(encoding="utf-8")
                        self.assertIn('<dds xmlns="http://www.eprosima.com">', generated_xml)
                        self.assertNotIn("ns0:", generated_xml)
                        root = ET.parse(destination).getroot()
                        elements = [
                            element
                            for element in root.iter()
                            if element.tag.rsplit("}", 1)[-1]
                            in ("data_writer", "data_reader")
                        ]
                        generic_defaults = [
                            element
                            for element in elements
                            if element.get("is_default_profile") == "true"
                        ]
                        scoped_profiles = [
                            element
                            for element in elements
                            if element.get("is_default_profile") == "false"
                        ]
                        self.assertEqual(
                            {element.get("profile_name") for element in generic_defaults},
                            {"generic_writer", "generic_reader"},
                        )
                        self.assertEqual(
                            {element.get("profile_name") for element in scoped_profiles},
                            {"/rbtbenchabc"},
                        )
                        self.assertEqual(
                            {
                                next(
                                    child.text
                                    for child in element.iter()
                                    if child.tag.endswith("kind")
                                )
                                for element in scoped_profiles
                            },
                            {"OFF" if mode == "shm" else "ON"},
                        )
                        self.assertEqual(
                            [
                                next(
                                    child.text
                                    for child in element.iter()
                                    if child.tag.endswith("kind")
                                )
                                for element in generic_defaults
                            ],
                            ["OFF", "OFF"],
                        )
                        expected_settings = expected_transport[(mode, case)]
                        self.assertEqual(
                            run.fastdds_transport_settings(mode, run.PAYLOAD_BYTES[case]),
                            expected_settings,
                        )
                        transport = next(
                            element
                            for element in root.iter()
                            if element.tag.rsplit("}", 1)[-1] == "transport_descriptor"
                        )
                        transport_values = {
                            child.tag.rsplit("}", 1)[-1]: int(child.text)
                            for child in transport
                            if child.tag.rsplit("}", 1)[-1]
                            in ("maxMessageSize", "segment_size")
                        }
                        self.assertEqual(
                            transport_values,
                            {
                                "maxMessageSize": expected_settings["max_message_size"],
                                "segment_size": expected_settings["segment_size"],
                            },
                        )
                        self.assertEqual(len(scoped_profiles), 2)

    def test_proc_stat_parses_parenthesized_name_with_spaces_and_closing_paren(self) -> None:
        fields = ["S"] + [str(value) for value in range(4, 14)] + ["321", "45"]
        parsed = run.parse_proc_stat(f"123 (publisher ) with spaces) {' '.join(fields)}")
        self.assertEqual(parsed, (321, 45))

    def test_proc_stat_rejects_missing_accounting_fields(self) -> None:
        with self.assertRaisesRegex(ValueError, "missing utime/stime"):
            run.parse_proc_stat("5 (short name) S 1 2 3")

    def test_smaps_rollup_requires_pss_and_rss(self) -> None:
        values = run.parse_smaps_rollup("Rss: 15 kB\nPss: 12 kB\n")
        self.assertEqual(values, {"rss_kib": 15, "pss_kib": 12})
        with self.assertRaisesRegex(ValueError, "Pss and Rss"):
            run.parse_smaps_rollup("Pss: 12 kB\n")

    def test_shared_address_resolution_preserves_inode_offset_and_deleted_path(self) -> None:
        maps, shared = run.parse_proc_maps(
            "7f100000-7f101000 rw-s 00001000 00:2a 42 /dev/shm/datasharing_pool with spaces (deleted)\n"
        )
        resolved = run.resolve_shared_address(0x7F100123, maps)
        self.assertEqual(len(shared), 1)
        self.assertEqual(
            resolved,
            {
                "device": "00:2a",
                "inode": 42,
                "file_offset": 0x1123,
                "permissions": "rw-s",
                "pathname": "/dev/shm/datasharing_pool with spaces (deleted)",
            },
        )

    def test_shared_address_resolution_rejects_anonymous_and_private_mappings(self) -> None:
        mappings, _ = run.parse_proc_maps(
            "7f100000-7f101000 rw-s 00000000 00:00 0 [anon:pool]\n"
            "7f200000-7f201000 rw-p 00001000 00:2a 42 /dev/shm/copied_pool\n"
        )
        self.assertIsNone(run.resolve_shared_address(0x7F100100, mappings))
        self.assertIsNone(run.resolve_shared_address(0x7F200100, mappings))

    def test_alias_proof_joins_sequences_and_requires_same_inode_and_offset(self) -> None:
        pub_maps, _ = run.parse_proc_maps(
            "1000-2000 rw-s 00000000 00:2a 42 /dev/shm/datasharing_pool\n"
        )
        sub_maps, _ = run.parse_proc_maps(
            "3000-4000 rw-s 00000000 00:2a 42 /dev/shm/datasharing_pool\n"
        )
        evidence = run.resolve_address_probes(
            [{"sequence": 7, "address": 0x1100}],
            [{"sequence": 7, "address": 0x3100}],
            pub_maps,
            sub_maps,
        )
        self.assertEqual(evidence["shared_alias_match_count"], 1)
        self.assertEqual(evidence["shared_alias_matches"][0]["sequence"], 7)


class RunnerSchedulingTests(unittest.TestCase):
    def test_mode_order_rotates_per_repetition_for_each_case(self) -> None:
        schedule = run.build_schedule(("imu", "cloud"), ("rbt", "shm", "loan"), 3)
        for case in ("imu", "cloud"):
            orders = [
                [trial["mode"] for trial in schedule if trial["case"] == case and trial["repetition"] == rep]
                for rep in (1, 2, 3)
            ]
            self.assertEqual(
                orders,
                [["rbt", "shm", "loan"], ["shm", "loan", "rbt"], ["loan", "rbt", "shm"]],
            )

    def test_cpu_normalization_reports_one_logical_core_as_one_hundred_percent(self) -> None:
        self.assertEqual(run.cpu_percent_from_ticks(200, 100, 2_000_000_000), 100.0)

    def test_memory_aggregation_sums_roles_before_median_and_peak(self) -> None:
        metrics = run.aggregate_memory_samples(
            [
                {"total_pss_kib": 150, "sum_rss_kib": 300},
                {"total_pss_kib": 210, "sum_rss_kib": 400},
                {"total_pss_kib": 180, "sum_rss_kib": 350},
            ]
        )
        self.assertEqual(metrics["median_total_pss_kib"], 180)
        self.assertEqual(metrics["peak_total_pss_kib"], 210)
        self.assertEqual(metrics["median_sum_rss_kib"], 350)
        self.assertEqual(metrics["peak_sum_rss_kib"], 400)

    def _ready_pair(self, mode: str) -> dict[str, dict[str, object]]:
        entry = {
            "can_loan": True,
            "loan_enabled": mode in ("rbt", "loan"),
            "participant_transport": "shm_only",
            "datasharing_kind": "OFF" if mode == "shm" else "ON",
        }
        return {"pub": dict(entry), "sub": dict(entry)}

    def test_ros_ready_requires_observed_transport_and_datasharing_for_both_roles(self) -> None:
        run.validate_ready_pair(self._ready_pair("shm"), "shm")
        run.validate_ready_pair(self._ready_pair("loan"), "loan")

        rbt_ready = self._ready_pair("rbt")
        for entry in rbt_ready.values():
            entry.pop("participant_transport")
            entry.pop("datasharing_kind")
        run.validate_ready_pair(rbt_ready, "rbt")

        for mode in ("shm", "loan"):
            for role in ("pub", "sub"):
                for field in ("participant_transport", "datasharing_kind"):
                    for absent in (False, True):
                        ready = self._ready_pair(mode)
                        if absent:
                            ready[role].pop(field)
                        else:
                            ready[role][field] = None
                        with self.subTest(mode=mode, role=role, field=field, absent=absent):
                            with self.assertRaisesRegex(run.TrialError, field):
                                run.validate_ready_pair(ready, mode)

    def test_ros_ready_rejects_observed_transport_or_datasharing_mismatch(self) -> None:
        for mode in ("shm", "loan"):
            for role in ("pub", "sub"):
                expected_datasharing = "OFF" if mode == "shm" else "ON"
                for field, bad_value in (
                    ("participant_transport", "udp_only"),
                    ("datasharing_kind", "ON" if expected_datasharing == "OFF" else "OFF"),
                ):
                    ready = self._ready_pair(mode)
                    ready[role][field] = bad_value
                    with self.subTest(mode=mode, role=role, field=field):
                        with self.assertRaisesRegex(run.TrialError, field):
                            run.validate_ready_pair(ready, mode)

    def test_rbt_and_loan_require_both_loan_capability_and_enablement(self) -> None:
        for mode in ("rbt", "loan"):
            for role, field, value in (
                ("pub", "can_loan", False),
                ("sub", "loan_enabled", False),
            ):
                ready = self._ready_pair(mode)
                ready[role][field] = value
                with self.subTest(mode=mode, role=role, field=field):
                    with self.assertRaises((run.TrialError, run.UnsupportedTrial)):
                        run.validate_ready_pair(ready, mode)
        with self.assertRaisesRegex(run.TrialError, "exactly publisher and subscriber"):
            run.validate_ready_pair({"pub": self._ready_pair("rbt")["pub"]}, "rbt")

    def test_trial_quality_requires_alias_proof_for_rbt_and_loan_modes(self) -> None:
        pub = {"published": 200, "loaned_count": 200}
        sub = {
            "received": 198,
            "invalid": 0,
            "duplicates": 0,
            "regressions": 0,
            "sequence_gaps": 2,
            "latency_p50_us": 12.0,
            "latency_p99_us": 20.0,
            "loaned_count": 198,
        }
        no_alias = {"shared_alias_match_count": 0}
        alias = {"shared_alias_match_count": 1}
        self.assertFalse(
            run.validate_trial_summaries(pub, sub, 1.0, "imu", "rbt", no_alias)["valid"]
        )
        self.assertTrue(
            run.validate_trial_summaries(pub, sub, 1.0, "imu", "loan", alias)["valid"]
        )
        self.assertTrue(
            run.validate_trial_summaries(pub, sub, 1.0, "imu", "shm", no_alias)["valid"]
        )
        self.assertFalse(
            run.validate_trial_summaries(pub, sub, 1.0, "imu", "shm", alias)["valid"]
        )

    def test_loan_mode_requires_every_measured_sample_to_be_loaned(self) -> None:
        pub = {"published": 200, "loaned_count": 200}
        sub = {
            "received": 198,
            "invalid": 0,
            "duplicates": 0,
            "regressions": 0,
            "sequence_gaps": 2,
            "latency_p50_us": 12.0,
            "latency_p99_us": 20.0,
            "loaned_count": 198,
        }
        alias = {"shared_alias_match_count": 1}
        self.assertTrue(
            run.validate_trial_summaries(pub, sub, 1.0, "imu", "loan", alias)["valid"]
        )
        pub["loaned_count"] = 199
        metrics = run.validate_trial_summaries(pub, sub, 1.0, "imu", "loan", alias)
        self.assertFalse(metrics["valid"])
        self.assertIn("publisher loaned_count", metrics["reasons"][0])


class ProcessCleanupTests(unittest.TestCase):
    def test_cleanup_signals_only_child_process_groups(self) -> None:
        child = mock.Mock()
        child.pid = 9876
        child.poll.side_effect = [None, 0]
        child.wait.return_value = 0
        with mock.patch.object(run.os, "killpg") as killpg, mock.patch.object(run.os, "kill") as kill:
            run.terminate_process_groups([child], grace_seconds=0.01)
        killpg.assert_called_once_with(9876, run.signal.SIGTERM)
        kill.assert_not_called()
        child.terminate.assert_not_called()

    def test_child_setup_sets_affinity_and_disables_core_dumps_only_in_child(self) -> None:
        cpus = {min(os.sched_getaffinity(0))} if hasattr(os, "sched_getaffinity") else {0}
        parent_limit = resource.getrlimit(resource.RLIMIT_CORE)
        process = subprocess.run(
            [
                sys.executable,
                "-c",
                "import resource; print(resource.getrlimit(resource.RLIMIT_CORE))",
            ],
            preexec_fn=lambda: run.configure_child_process(cpus),
            check=True,
            capture_output=True,
            text=True,
        )
        self.assertIn("(0, 0)", process.stdout)
        self.assertEqual(resource.getrlimit(resource.RLIMIT_CORE), parent_limit)


class StartupFailureTests(unittest.TestCase):
    def test_publisher_only_readiness_barrier_waits_for_sidecar_creation(self) -> None:
        process = mock.Mock()
        process.poll.return_value = None
        ready = {"ready": True}
        with mock.patch.object(run, "read_ready", side_effect=[None, ready]), mock.patch.object(
            run.time, "sleep"
        ) as sleep:
            result = run.wait_for_ready(
                {"pub": process}, {"pub": Path("pub.ready.json")}, {}, timeout_seconds=0.5
            )
        self.assertEqual(result, {"pub": ready})
        sleep.assert_called_once()

    def test_early_exit_without_ready_file_reports_endpoint_exit(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            process = mock.Mock()
            process.poll.return_value = 127
            process.returncode = 127
            with self.assertRaisesRegex(
                run.TrialError, "exit 127"
            ):
                run.wait_for_ready(
                    {"pub": process},
                    {"pub": Path(directory) / "pub.ready.json"},
                    {"trial_dir": directory},
                    timeout_seconds=0.1,
                )


if __name__ == "__main__":
    unittest.main()

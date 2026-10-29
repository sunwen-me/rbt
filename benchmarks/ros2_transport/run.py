#!/usr/bin/env python3
"""Run controlled two-process RBT and ROS 2 transport trials."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import platform
import re
import resource
import signal
import statistics
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any, Callable, Iterable, Sequence


CASES = ("imu", "cloud", "video")
MODES = ("rbt", "shm", "loan")
NOMINAL_RATE_HZ = {"imu": 200, "cloud": 10, "video": 30}
PAYLOAD_BYTES = {"imu": 256, "cloud": 1_600_000, "video": 6_220_800}
ROS_DOMAIN_MIN = 180
ROS_DOMAIN_MAX = 220
READY_TIMEOUT_SECONDS = 15.0
EXIT_TIMEOUT_SECONDS = 5.0
MEMORY_SAMPLE_NS = 1_000_000_000
ROS_ENV_KEYS = (
    "RMW_IMPLEMENTATION",
    "RMW_FASTRTPS_USE_QOS_FROM_XML",
    "FASTDDS_DEFAULT_PROFILES_FILE",
    "ROS_DISABLE_LOANED_MESSAGES",
    "ROS_DOMAIN_ID",
    "ROS_AUTOMATIC_DISCOVERY_RANGE",
)
CONFLICTING_FASTDDS_ENV = (
    "FASTDDS_BUILTIN_TRANSPORTS",
    "FASTDDS_DEFAULT_PROFILES_FILE",
    "FASTRTPS_DEFAULT_PROFILES_FILE",
    "FASTDDS_ENVIRONMENT_FILE",
    "FASTRTPS_ENVIRONMENT_FILE",
    "RMW_FASTRTPS_PUBLICATION_MODE",
    "RMW_FASTRTPS_USE_QOS_FROM_XML",
    "ROS_DISCOVERY_SERVER",
    "ROS_STATIC_PEERS",
    "ROS_SUPER_CLIENT",
)
CSV_FIELDS = (
    "trial",
    "case",
    "mode",
    "repetition",
    "status",
    "valid",
    "reason",
    "domain_id",
    "topic",
    "published",
    "received",
    "drops",
    "sequence_gaps",
    "invalid_samples",
    "duplicates",
    "regressions",
    "publisher_rate_hz",
    "delivery_ratio",
    "publisher_cpu_percent",
    "subscriber_cpu_percent",
    "total_cpu_percent",
    "median_total_pss_kib",
    "peak_total_pss_kib",
    "median_sum_rss_kib",
    "peak_sum_rss_kib",
    "latency_p50_us",
    "latency_p99_us",
    "loaned_count",
    "shared_alias_matches",
)


class TrialError(RuntimeError):
    """A process or measurement failure that stops the scheduled matrix."""


class UnsupportedTrial(RuntimeError):
    """A trial explicitly reported as unsupported by its endpoint."""


def parse_csv_choice(value: str, choices: Sequence[str], label: str) -> list[str]:
    selected = [part.strip() for part in value.split(",") if part.strip()]
    if not selected:
        raise argparse.ArgumentTypeError(f"{label} must contain at least one value")
    if len(set(selected)) != len(selected):
        raise argparse.ArgumentTypeError(f"{label} must not contain duplicates")
    unknown = [part for part in selected if part not in choices]
    if unknown:
        raise argparse.ArgumentTypeError(
            f"unknown {label}: {', '.join(unknown)}; choose from {', '.join(choices)}"
        )
    return selected


def parse_cpus(value: str | None) -> set[int] | None:
    if value is None:
        return None
    try:
        cpus = {int(part.strip(), 10) for part in value.split(",") if part.strip()}
    except ValueError as error:
        raise argparse.ArgumentTypeError("--cpus must be a comma-separated CPU list") from error
    if not cpus or any(cpu < 0 for cpu in cpus):
        raise argparse.ArgumentTypeError("--cpus must contain non-negative CPU numbers")
    return cpus


def build_schedule(cases: Sequence[str], modes: Sequence[str], repetitions: int) -> list[dict[str, Any]]:
    schedule: list[dict[str, Any]] = []
    for case_index, case in enumerate(cases):
        for repetition in range(repetitions):
            offset = repetition % len(modes)
            rotated_modes = list(modes[offset:]) + list(modes[:offset])
            for mode_position, mode in enumerate(rotated_modes):
                schedule.append(
                    {
                        "case": case,
                        "mode": mode,
                        "repetition": repetition + 1,
                        "order_in_repetition": mode_position + 1,
                        "case_order": case_index + 1,
                    }
                )
    return schedule


def parse_proc_stat(content: str) -> tuple[int, int]:
    """Return utime/stime ticks, handling spaces and parentheses in comm."""
    opening = content.find("(")
    closing = content.rfind(")")
    if opening < 0 or closing <= opening:
        raise ValueError("malformed /proc/PID/stat command field")
    fields = content[closing + 1 :].split()
    # fields[0] is Linux stat field 3 (state); utime/stime are 14/15.
    if len(fields) <= 12:
        raise ValueError("/proc/PID/stat is missing utime/stime fields")
    try:
        return int(fields[11]), int(fields[12])
    except ValueError as error:
        raise ValueError("/proc/PID/stat utime/stime fields are not integers") from error


def read_proc_stat(pid: int) -> tuple[int, int]:
    return parse_proc_stat(Path(f"/proc/{pid}/stat").read_text(encoding="ascii"))


def parse_smaps_rollup(content: str) -> dict[str, int]:
    values: dict[str, int] = {}
    for line in content.splitlines():
        match = re.match(r"^(Pss|Rss):\s+(\d+)\s+kB\s*$", line)
        if match:
            values[match.group(1).lower() + "_kib"] = int(match.group(2))
    if "pss_kib" not in values or "rss_kib" not in values:
        raise ValueError("/proc/PID/smaps_rollup did not contain Pss and Rss")
    return values


def read_smaps_rollup(pid: int) -> dict[str, int]:
    return parse_smaps_rollup(
        Path(f"/proc/{pid}/smaps_rollup").read_text(encoding="ascii")
    )


def parse_proc_maps(content: str) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    mappings: list[dict[str, Any]] = []
    shared: list[dict[str, Any]] = []
    for line in content.splitlines():
        fields = line.split(maxsplit=5)
        if len(fields) < 5:
            continue
        address, permissions, offset, device, inode_text = fields[:5]
        pathname = fields[5] if len(fields) == 6 else ""
        try:
            inode = int(inode_text, 10)
        except ValueError:
            continue
        mapping = {
            "address": address,
            "permissions": permissions,
            "offset": offset,
            "device": device,
            "inode": inode,
            "pathname": pathname,
        }
        mappings.append(mapping)
        if len(permissions) >= 4 and permissions[3] == "s":
            shared.append(mapping)
    return mappings, shared


def resolve_shared_address(
    address: int, mappings: Sequence[dict[str, Any]]
) -> dict[str, Any] | None:
    """Resolve a virtual address to shared file backing, rejecting anonymous maps."""
    if not isinstance(address, int) or address <= 0:
        return None
    for mapping in mappings:
        try:
            start_text, end_text = mapping["address"].split("-", 1)
            start = int(start_text, 16)
            end = int(end_text, 16)
            permissions = mapping["permissions"]
            inode = int(mapping["inode"])
            device = str(mapping["device"])
            offset = int(mapping["offset"], 16)
        except (KeyError, TypeError, ValueError):
            continue
        if start <= address < end:
            if len(permissions) < 4 or permissions[3] != "s" or inode == 0:
                return None
            return {
                "device": device,
                "inode": inode,
                "file_offset": offset + address - start,
                "permissions": permissions,
                "pathname": str(mapping.get("pathname", "")),
            }
    return None


def resolve_address_probes(
    pub_probes: Any,
    sub_probes: Any,
    pub_mappings: Sequence[dict[str, Any]],
    sub_mappings: Sequence[dict[str, Any]],
) -> dict[str, Any]:
    def resolve(
        probes: Any, mappings: Sequence[dict[str, Any]], role: str
    ) -> dict[int, dict[str, Any]]:
        if not isinstance(probes, list):
            raise TrialError(f"{role} summary address_probes must be a list")
        resolved: dict[int, dict[str, Any]] = {}
        for probe in probes:
            if not isinstance(probe, dict) or "sequence" not in probe or "address" not in probe:
                raise TrialError(f"{role} address_probes entries require sequence and address")
            sequence = probe["sequence"]
            address = probe["address"]
            if not isinstance(sequence, int) or sequence < 0 or sequence in resolved:
                raise TrialError(f"{role} address probe sequence must be unique and non-negative")
            if not isinstance(address, int) or address <= 0:
                raise TrialError(f"{role} address probe address must be a positive uintptr integer")
            resolved[sequence] = {
                "sequence": sequence,
                "address": address,
                "shared_backing": resolve_shared_address(address, mappings),
            }
        return resolved

    pub_by_sequence = resolve(pub_probes, pub_mappings, "publisher")
    sub_by_sequence = resolve(sub_probes, sub_mappings, "subscriber")
    common_sequences = sorted(set(pub_by_sequence).intersection(sub_by_sequence))
    matches: list[dict[str, Any]] = []
    for sequence in common_sequences:
        pub_backing = pub_by_sequence[sequence]["shared_backing"]
        sub_backing = sub_by_sequence[sequence]["shared_backing"]
        if pub_backing is None or sub_backing is None:
            continue
        if (
            pub_backing["device"],
            pub_backing["inode"],
            pub_backing["file_offset"],
        ) == (
            sub_backing["device"],
            sub_backing["inode"],
            sub_backing["file_offset"],
        ):
            matches.append(
                {
                    "sequence": sequence,
                    "publisher": pub_by_sequence[sequence],
                    "subscriber": sub_by_sequence[sequence],
                }
            )
    return {
        "publisher": [pub_by_sequence[key] for key in sorted(pub_by_sequence)],
        "subscriber": [sub_by_sequence[key] for key in sorted(sub_by_sequence)],
        "common_sequences": common_sequences,
        "shared_alias_matches": matches,
        "shared_alias_match_count": len(matches),
    }


def capture_process_maps(pid: int, trial_dir: Path, role: str) -> dict[str, Any]:
    content = Path(f"/proc/{pid}/maps").read_text(encoding="utf-8")
    mappings, shared = parse_proc_maps(content)
    (trial_dir / f"{role}.maps.txt").write_text(content, encoding="utf-8")
    return {"pid": pid, "mappings": mappings, "shared_mappings": shared}


def cpu_percent_from_ticks(delta_ticks: int, ticks_per_second: int, elapsed_ns: int) -> float:
    if delta_ticks < 0 or ticks_per_second <= 0 or elapsed_ns <= 0:
        raise ValueError("CPU delta and elapsed time must be non-negative and non-zero")
    cpu_seconds = delta_ticks / ticks_per_second
    return 100.0 * cpu_seconds / (elapsed_ns / 1_000_000_000)


def aggregate_memory_samples(samples: Sequence[dict[str, Any]]) -> dict[str, float | int]:
    if not samples:
        raise ValueError("memory sampling produced no samples")
    total_pss = [int(sample["total_pss_kib"]) for sample in samples]
    total_rss = [int(sample["sum_rss_kib"]) for sample in samples]
    return {
        "median_total_pss_kib": statistics.median(total_pss),
        "peak_total_pss_kib": max(total_pss),
        "median_sum_rss_kib": statistics.median(total_rss),
        "peak_sum_rss_kib": max(total_rss),
    }


def atomic_write_text(path: Path, text: str) -> None:
    temporary = path.with_name(f".{path.name}.{os.getpid()}.{uuid.uuid4().hex}.tmp")
    temporary.write_text(text, encoding="utf-8")
    os.replace(temporary, path)


def atomic_write_json(path: Path, value: Any) -> None:
    atomic_write_text(path, json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n")


def fastdds_transport_settings(mode: str, payload_bytes: int) -> dict[str, int]:
    if mode not in ("shm", "loan"):
        raise ValueError(f"Fast DDS transport settings do not apply to mode {mode!r}")
    if payload_bytes <= 0:
        raise ValueError("payload_bytes must be positive")
    if mode == "loan":
        max_message_size = 65_536
        segment_size = 524_288
    else:
        required_message_size = payload_bytes + 4_096
        max_message_size = max(65_536, 1 << (required_message_size - 1).bit_length())
        segment_size = max(524_288, 4 * max_message_size)
    return {
        "max_message_size": max_message_size,
        "segment_size": segment_size,
    }


def instantiate_fastdds_profile(
    template: Path,
    destination: Path,
    topic: str,
    mode: str,
    case: str,
) -> Path:
    tree = ET.parse(template)
    root = tree.getroot()
    transport_descriptors = [
        element
        for element in root.iter()
        if element.tag.rsplit("}", 1)[-1] == "transport_descriptor"
    ]
    if len(transport_descriptors) != 1:
        raise ValueError(f"{template} must contain exactly one SHM transport descriptor")
    settings = fastdds_transport_settings(mode, PAYLOAD_BYTES[case])
    transport_descriptor = transport_descriptors[0]
    for tag, value in (
        ("maxMessageSize", settings["max_message_size"]),
        ("segment_size", settings["segment_size"]),
    ):
        elements = [
            element
            for element in transport_descriptor
            if element.tag.rsplit("}", 1)[-1] == tag
        ]
        if len(elements) != 1:
            raise ValueError(f"{template} SHM transport descriptor must contain one {tag}")
        elements[0].text = str(value)
    benchmark_profiles = [
        element
        for element in root.iter()
        if element.tag.rsplit("}", 1)[-1] in ("data_writer", "data_reader")
        and element.get("profile_name") == "__BENCHMARK_TOPIC__"
    ]
    kinds = [element.tag.rsplit("}", 1)[-1] for element in benchmark_profiles]
    if sorted(kinds) != ["data_reader", "data_writer"]:
        raise ValueError(
            f"{template} must contain one topic-scoped data_writer and data_reader "
            "profile named __BENCHMARK_TOPIC__"
        )
    profile_name = f"/{topic}"
    for element in benchmark_profiles:
        element.set("profile_name", profile_name)
    namespace = root.tag[1:].split("}", 1)[0] if root.tag.startswith("{") else ""
    if namespace:
        ET.register_namespace("", namespace)
    xml_text = ET.tostring(root, encoding="unicode", xml_declaration=True)
    atomic_write_text(destination, xml_text + "\n")
    return destination


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None


def process_environment(base_env: dict[str, str], mode: str, domain_id: int, ros_config: Path) -> dict[str, str]:
    environment = base_env.copy()
    for key in CONFLICTING_FASTDDS_ENV:
        environment.pop(key, None)
    environment["ROS_DOMAIN_ID"] = str(domain_id)
    environment["ROS_AUTOMATIC_DISCOVERY_RANGE"] = "SYSTEM_DEFAULT"
    if mode == "rbt":
        # The RBT baseline has no Fast DDS profile; remove inherited ROS selector values.
        environment.pop("RMW_IMPLEMENTATION", None)
        environment.pop("ROS_DISABLE_LOANED_MESSAGES", None)
        return environment
    environment["RMW_IMPLEMENTATION"] = "rmw_fastrtps_cpp"
    environment["RMW_FASTRTPS_USE_QOS_FROM_XML"] = "1"
    environment["FASTDDS_DEFAULT_PROFILES_FILE"] = str(ros_config)
    environment["ROS_DISABLE_LOANED_MESSAGES"] = "1" if mode == "shm" else "0"
    return environment


def source_ros_environment(base_env: dict[str, str], script: Path | None) -> dict[str, str]:
    if script is None:
        return base_env.copy()
    completed = subprocess.run(
        ["bash", "-c", 'source "$1" >/dev/null && env -0', "ros-setup", str(script)],
        cwd=script.parent,
        env=base_env,
        check=True,
        stdout=subprocess.PIPE,
        stderr=None,
    )
    environment: dict[str, str] = {}
    for item in completed.stdout.split(b"\0"):
        if not item or b"=" not in item:
            continue
        key, value = item.split(b"=", 1)
        environment[key.decode("utf-8", "surrogateescape")] = value.decode(
            "utf-8", "surrogateescape"
        )
    return environment


def safe_runtime_metadata(cpus: set[int], runtime_env: dict[str, str]) -> dict[str, Any]:
    cpu_model = None
    try:
        for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
            if line.lower().startswith(("model name", "hardware")) and ":" in line:
                cpu_model = line.split(":", 1)[1].strip()
                if cpu_model:
                    break
    except OSError:
        pass
    metadata: dict[str, Any] = {
        "platform": platform.platform(),
        "kernel": platform.release(),
        "architecture": platform.machine(),
        "cpu_model": cpu_model,
        "cpu_affinity": sorted(cpus),
        "python": platform.python_version(),
        "cc": runtime_env.get("CC"),
        "cxx": runtime_env.get("CXX"),
        "ros_distro": runtime_env.get("ROS_DISTRO"),
        "rbt_sdk_prefix": runtime_env.get("RBT_SDK_PREFIX"),
        "rbt_sdk_version": runtime_env.get("RBT_SDK_VERSION"),
    }
    return metadata


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_summary(path: Path, role: str, trial: dict[str, Any]) -> dict[str, Any]:
    summary = load_json(path)
    if not isinstance(summary, dict):
        raise TrialError(f"{role} summary is missing or not valid JSON: {path}")
    if summary.get("status") == "unsupported" or summary.get("supported") is False:
        raise UnsupportedTrial(f"{role} endpoint reported unsupported: {summary}")

    required = {
        "role",
        "backend",
        "mode",
        "case",
        "payload_bytes",
        "start_ns",
        "end_ns",
    }
    required.add("published" if role == "pub" else "received")
    required.add("address_probes")
    required.add("loaned_count")
    if role == "sub":
        required.update(
            (
                "invalid",
                "sequence_gaps",
                "duplicates",
                "regressions",
                "first_sequence",
                "last_sequence",
                "latency_p50_us",
                "latency_p99_us",
            )
        )
    missing = sorted(required.difference(summary))
    if missing:
        raise TrialError(f"{role} summary missing required fields: {', '.join(missing)}")
    if summary["role"] != role:
        raise TrialError(f"{role} summary has mismatched role: {summary['role']!r}")
    if summary["mode"] != trial["mode"] or summary["case"] != trial["case"]:
        raise TrialError(f"{role} summary mode/case does not match scheduled trial")
    if summary["payload_bytes"] != PAYLOAD_BYTES[trial["case"]]:
        raise TrialError(f"{role} summary has unexpected payload_bytes")
    if not isinstance(summary["backend"], str) or not summary["backend"]:
        raise TrialError(f"{role} summary backend must be a non-empty string")
    for field in ("start_ns", "end_ns"):
        if not isinstance(summary[field], int) or summary[field] <= 0:
            raise TrialError(f"{role} summary {field} must be a positive integer")
    if summary["end_ns"] <= summary["start_ns"]:
        raise TrialError(f"{role} summary end_ns must be after start_ns")
    if summary["start_ns"] != trial["start_ns"] or summary["end_ns"] != trial["end_ns"]:
        raise TrialError(f"{role} summary window differs from parent measurement window")
    count_field = "published" if role == "pub" else "received"
    if not isinstance(summary[count_field], int) or summary[count_field] < 0:
        raise TrialError(f"{role} summary {count_field} must be a non-negative integer")
    if not isinstance(summary["loaned_count"], int) or summary["loaned_count"] < 0:
        raise TrialError(f"{role} summary loaned_count must be a non-negative integer")
    if role == "pub":
        for field in ("invalid", "duplicates", "regressions"):
            if field in summary and (not isinstance(summary[field], int) or summary[field] < 0):
                raise TrialError(f"publisher summary {field} must be a non-negative integer")
    if role == "sub":
        for field in ("invalid", "sequence_gaps", "duplicates", "regressions"):
            if not isinstance(summary[field], int) or summary[field] < 0:
                raise TrialError(f"subscriber summary {field} must be a non-negative integer")
        for field in ("first_sequence", "last_sequence"):
            if summary[field] is not None and (
                not isinstance(summary[field], int) or summary[field] < 0
            ):
                raise TrialError(f"subscriber summary {field} must be a non-negative integer or null")
        for field in ("latency_p50_us", "latency_p99_us"):
            value = summary[field]
            if value is not None and (
                not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0
            ):
                raise TrialError(f"subscriber summary {field} must be a non-negative finite number or null")
    return summary


def read_ready(path: Path, role: str, trial: dict[str, Any], process: subprocess.Popen[Any]) -> dict[str, Any] | None:
    ready = load_json(path)
    if not isinstance(ready, dict):
        return None
    if ready.get("status") == "unsupported" or ready.get("supported") is False:
        raise UnsupportedTrial(f"{role} endpoint reported unsupported during setup: {ready}")
    required = {"ready", "role", "mode", "case", "payload_bytes", "can_loan", "loan_enabled", "pid"}
    missing = sorted(required.difference(ready))
    if missing:
        raise TrialError(f"{role} ready file missing required fields: {', '.join(missing)}")
    if ready["ready"] is not True or ready["role"] != role:
        raise TrialError(f"{role} ready file has invalid ready/role fields")
    if ready["mode"] != trial["mode"] or ready["case"] != trial["case"]:
        raise TrialError(f"{role} ready file mode/case does not match scheduled trial")
    if ready["payload_bytes"] != PAYLOAD_BYTES[trial["case"]]:
        raise TrialError(f"{role} ready file has unexpected payload_bytes")
    if not isinstance(ready["can_loan"], bool) or not isinstance(ready["loan_enabled"], bool):
        raise TrialError(f"{role} ready can_loan/loan_enabled fields must be booleans")
    if not isinstance(ready["pid"], int) or ready["pid"] != process.pid:
        raise TrialError(f"{role} ready pid does not match launched process")
    if trial["mode"] == "loan" and not ready["can_loan"]:
        raise UnsupportedTrial(f"{role} endpoint does not support loaned messages")
    if trial["mode"] == "loan" and not ready["loan_enabled"]:
        raise TrialError(f"{role} loaned-message mode was requested but is disabled")
    if trial["mode"] == "shm" and ready["loan_enabled"]:
        raise TrialError(f"{role} shared-memory mode unexpectedly enabled loaned messages")
    return ready


def _process_alive(process: subprocess.Popen[Any], label: str) -> None:
    code = process.poll()
    if code is not None:
        raise TrialError(f"{label} exited before the measurement window ended (exit {code})")


def _wait_until(target_ns: int) -> None:
    while True:
        remaining_ns = target_ns - time.clock_gettime_ns(time.CLOCK_MONOTONIC)
        if remaining_ns <= 0:
            return
        time.sleep(min(remaining_ns / 1_000_000_000, 0.1))


def terminate_process_groups(processes: Iterable[subprocess.Popen[Any]], grace_seconds: float = 2.0) -> None:
    active = [process for process in processes if process.poll() is None]
    for process in active:
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    deadline = time.monotonic() + grace_seconds
    for process in active:
        remaining = max(0.0, deadline - time.monotonic())
        try:
            process.wait(timeout=remaining)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    for process in active:
        try:
            process.wait(timeout=0.5)
        except subprocess.TimeoutExpired:
            pass


def configure_child_process(cpus: set[int]) -> None:
    if hasattr(os, "sched_setaffinity"):
        os.sched_setaffinity(0, cpus)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))


def trial_child_command(
    executable: Path,
    role: str,
    case: str,
    mode: str,
    topic: str,
    window_file: Path,
    ready_file: Path,
    output_file: Path,
) -> list[str]:
    return [
        str(executable),
        "--role",
        role,
        "--case",
        case,
        "--mode",
        mode,
        "--topic",
        topic,
        "--window-file",
        str(window_file),
        "--ready-file",
        str(ready_file),
        "--output",
        str(output_file),
    ]


def wait_for_ready(
    processes: dict[str, subprocess.Popen[Any]],
    ready_files: dict[str, Path],
    trial: dict[str, Any],
    timeout_seconds: float = READY_TIMEOUT_SECONDS,
) -> dict[str, dict[str, Any]]:
    deadline = time.monotonic() + min(timeout_seconds, READY_TIMEOUT_SECONDS)
    ready_by_role: dict[str, dict[str, Any]] = {}
    while time.monotonic() < deadline:
        for role, process in processes.items():
            if role in ready_by_role:
                _process_alive(process, role)
                continue
            ready = read_ready(ready_files[role], role, trial, process)
            if ready is not None:
                ready_by_role[role] = ready
            elif process.poll() is not None:
                result = load_json(Path(trial["trial_dir"]) / f"{role}.json")
                if isinstance(result, dict) and (
                    result.get("status") == "unsupported" or result.get("supported") is False
                ):
                    raise UnsupportedTrial(f"{role} endpoint reported unsupported: {result}")
                raise TrialError(f"{role} endpoint exited before ready (exit {process.returncode})")
        if len(ready_by_role) == len(processes):
            return ready_by_role
        time.sleep(0.05)
    raise TrialError(f"endpoints did not both become ready within {READY_TIMEOUT_SECONDS:g}s")


def sample_trial_window(
    processes: dict[str, subprocess.Popen[Any]],
    start_ns: int,
    end_ns: int,
    capture_steady_maps: Callable[[], None],
) -> tuple[dict[str, dict[str, int]], dict[str, dict[str, int]], list[dict[str, Any]], int, int]:
    memory_samples: list[dict[str, Any]] = []
    cpu_start: dict[str, dict[str, int]] = {}
    cpu_end: dict[str, dict[str, int]] = {}
    cpu_start_capture_ns = 0
    cpu_end_capture_ns = 0

    _wait_until(start_ns)
    for role, process in processes.items():
        _process_alive(process, role)
    cpu_start_capture_ns = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    cpu_start = {role: dict(zip(("utime", "stime"), read_proc_stat(process.pid))) for role, process in processes.items()}
    memory_samples.append(sample_memory(processes, "start"))

    map_capture_ns = start_ns + min(100_000_000, max(1, (end_ns - start_ns) // 2))
    _wait_until(map_capture_ns)
    for role, process in processes.items():
        _process_alive(process, role)
    capture_steady_maps()

    sample_index = 1
    while start_ns + sample_index * MEMORY_SAMPLE_NS < end_ns:
        target_ns = start_ns + sample_index * MEMORY_SAMPLE_NS
        _wait_until(target_ns)
        for role, process in processes.items():
            _process_alive(process, role)
        memory_samples.append(sample_memory(processes, str(sample_index)))
        sample_index += 1

    cpu_target_ns = max(start_ns, end_ns - 5_000_000)
    _wait_until(cpu_target_ns)
    for role, process in processes.items():
        _process_alive(process, role)
    cpu_end_capture_ns = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    cpu_end = {role: dict(zip(("utime", "stime"), read_proc_stat(process.pid))) for role, process in processes.items()}
    return cpu_start, cpu_end, memory_samples, cpu_start_capture_ns, cpu_end_capture_ns


def capture_trial_maps(
    processes: dict[str, subprocess.Popen[Any]], trial_dir: Path
) -> dict[str, Any]:
    maps = {
        role: capture_process_maps(process.pid, trial_dir, role)
        for role, process in processes.items()
    }
    pub_mappings = {
        (item["device"], item["inode"]): item
        for item in maps["pub"]["shared_mappings"]
        if item["inode"] > 0
    }
    sub_mappings = {
        (item["device"], item["inode"]): item
        for item in maps["sub"]["shared_mappings"]
        if item["inode"] > 0
    }
    common_inodes = sorted(set(pub_mappings).intersection(sub_mappings))
    maps["common_file_inodes"] = [
        {
            "device": device,
            "inode": inode,
            "pub": pub_mappings[(device, inode)],
            "sub": sub_mappings[(device, inode)],
        }
        for device, inode in common_inodes
    ]
    maps["common_datasharing_pool_inodes"] = [
        item
        for item in maps["common_file_inodes"]
        if "datasharing_pool" in item["pub"]["pathname"].lower()
        or "datasharing_pool" in item["sub"]["pathname"].lower()
    ]
    maps["snapshot_ns"] = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    atomic_write_json(trial_dir / "process_maps.json", maps)
    return maps


def sample_memory(
    processes: dict[str, subprocess.Popen[Any]], label: str
) -> dict[str, Any]:
    role_samples = {role: read_smaps_rollup(process.pid) for role, process in processes.items()}
    return {
        "label": label,
        "sample_ns": time.clock_gettime_ns(time.CLOCK_MONOTONIC),
        "roles": role_samples,
        "total_pss_kib": sum(sample["pss_kib"] for sample in role_samples.values()),
        "sum_rss_kib": sum(sample["rss_kib"] for sample in role_samples.values()),
    }


def cpu_metrics(
    cpu_start: dict[str, dict[str, int]],
    cpu_end: dict[str, dict[str, int]],
    start_capture_ns: int,
    end_capture_ns: int,
) -> dict[str, float]:
    ticks_per_second = int(os.sysconf("SC_CLK_TCK"))
    elapsed_ns = end_capture_ns - start_capture_ns
    values: dict[str, float] = {}
    for role, label in (("pub", "publisher"), ("sub", "subscriber")):
        delta = (cpu_end[role]["utime"] + cpu_end[role]["stime"]) - (
            cpu_start[role]["utime"] + cpu_start[role]["stime"]
        )
        values[f"{label}_cpu_percent"] = cpu_percent_from_ticks(
            delta, ticks_per_second, elapsed_ns
        )
    values["total_cpu_percent"] = (
        values["publisher_cpu_percent"] + values["subscriber_cpu_percent"]
    )
    return values


def validate_ready_pair(ready: dict[str, dict[str, Any]], mode: str) -> None:
    if set(ready) != {"pub", "sub"}:
        raise TrialError("ready metadata must include exactly publisher and subscriber roles")
    for role, entry in ready.items():
        if mode in ("rbt", "loan") and not entry["can_loan"]:
            raise UnsupportedTrial(f"{role} endpoint does not support loaned messages")
        if mode in ("rbt", "loan") and not entry["loan_enabled"]:
            raise TrialError(f"{role} {mode} zero-copy mode was requested but loaning is disabled")
        if mode == "shm" and entry["loan_enabled"]:
            raise TrialError(f"{role} shared-memory mode unexpectedly enabled loaned messages")

    if mode in ("shm", "loan"):
        expected_datasharing = "OFF" if mode == "shm" else "ON"
        for role, entry in ready.items():
            participant_transport = entry.get("participant_transport")
            if participant_transport != "shm_only":
                raise TrialError(
                    f"{role} participant_transport must be shm_only for ROS {mode} mode; "
                    f"got {participant_transport!r}"
                )
            datasharing_kind = entry.get("datasharing_kind")
            if datasharing_kind != expected_datasharing:
                raise TrialError(
                    f"{role} datasharing_kind must be {expected_datasharing!r} for ROS {mode} mode; "
                    f"got {datasharing_kind!r}"
                )


def validate_trial_summaries(
    pub: dict[str, Any],
    sub: dict[str, Any],
    window_seconds: float,
    case: str,
    mode: str,
    alias_evidence: dict[str, Any],
) -> dict[str, Any]:
    published = pub["published"]
    received = sub["received"]
    drops = max(0, published - received)
    delivery_ratio = received / published if published else 0.0
    publisher_rate = published / window_seconds
    error_reasons: list[str] = []
    for role, summary in (("publisher", pub), ("subscriber", sub)):
        for field in ("invalid", "duplicates", "regressions"):
            if summary.get(field, 0) != 0:
                error_reasons.append(f"{role} {field}={summary[field]}")
    if received > published:
        error_reasons.append(f"received ({received}) exceeds published ({published})")
    if delivery_ratio < 0.99:
        error_reasons.append(f"delivery ratio {delivery_ratio:.6f} is below 0.99")
    minimum_published = NOMINAL_RATE_HZ[case] * window_seconds * 0.99
    if published < minimum_published:
        error_reasons.append(
            f"publisher rate {publisher_rate:.3f} Hz is below 99% of nominal {NOMINAL_RATE_HZ[case]} Hz"
        )
    alias_matches = alias_evidence["shared_alias_match_count"]
    if mode in ("rbt", "loan") and alias_matches == 0:
        error_reasons.append(f"no shared-buffer alias proof for mode {mode}")
    if mode == "shm" and alias_matches != 0:
        error_reasons.append("shared-buffer alias found in shm-copy mode")
    if mode == "loan":
        for role, summary, sent_or_received in (
            ("publisher", pub, published),
            ("subscriber", sub, received),
        ):
            loaned_count = summary.get("loaned_count")
            if loaned_count != sent_or_received:
                error_reasons.append(
                    f"{role} loaned_count {loaned_count!r} does not equal measured "
                    f"sample count {sent_or_received}"
                )
    return {
        "valid": not error_reasons,
        "reasons": error_reasons,
        "published": published,
        "received": received,
        "drops": drops,
        "delivery_ratio": delivery_ratio,
        "publisher_rate_hz": publisher_rate,
        "sequence_gaps": sub["sequence_gaps"],
        "invalid_samples": sub["invalid"] + pub.get("invalid", 0),
        "duplicates": sub["duplicates"] + pub.get("duplicates", 0),
        "regressions": sub["regressions"] + pub.get("regressions", 0),
        "latency_p50_us": sub["latency_p50_us"],
        "latency_p99_us": sub["latency_p99_us"],
        "loaned_count": sub["loaned_count"],
        "shared_alias_matches": alias_matches,
    }


def make_trial_dirs(output_root: Path, index: int, entry: dict[str, Any]) -> Path:
    return output_root / (
        f"trial-{index:03d}-{entry['case']}-{entry['mode']}-rep{entry['repetition']:02d}"
    )


def write_results(output_root: Path, results: dict[str, Any]) -> None:
    atomic_write_json(output_root / "results.json", results)
    with (output_root / "summary.csv.tmp").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=CSV_FIELDS, extrasaction="ignore")
        writer.writeheader()
        for trial in results["trials"]:
            metrics = trial.get("metrics", {})
            row = {key: trial.get(key) for key in CSV_FIELDS}
            row["trial"] = trial.get("trial_index")
            row.update({key: metrics.get(key) for key in CSV_FIELDS if key in metrics})
            writer.writerow(row)
    os.replace(output_root / "summary.csv.tmp", output_root / "summary.csv")


def _csv_choice_argument(choices: Sequence[str], label: str):
    def parse(value: str) -> list[str]:
        return parse_csv_choice(value, choices, label)

    return parse


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rbt-executable", required=True, type=Path)
    parser.add_argument("--ros-executable", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--seconds", type=float, default=15.0)
    parser.add_argument("--warmup", type=float, default=3.0)
    parser.add_argument("--repetitions", type=int, default=3)
    parser.add_argument("--cases", type=_csv_choice_argument(CASES, "cases"), default=list(CASES))
    parser.add_argument("--modes", type=_csv_choice_argument(MODES, "modes"), default=list(MODES))
    parser.add_argument("--cpus", type=parse_cpus)
    parser.add_argument("--ros-setup", type=Path, help="optional ROS environment setup script")
    return parser


def resolve_executable(path: Path, label: str) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise ValueError(f"{label} is not an executable file: {resolved}")
    return resolved


def available_cpus(requested: set[int] | None) -> set[int]:
    try:
        allowed = set(os.sched_getaffinity(0))
    except AttributeError:
        allowed = set(range(os.cpu_count() or 1))
    if requested is None:
        return allowed
    unavailable = requested.difference(allowed)
    if unavailable:
        raise ValueError(f"requested CPU(s) are outside the current affinity: {sorted(unavailable)}")
    return requested


def trial_environment(
    base_env: dict[str, str], mode: str, domain_id: int, ros_profile: Path | None
) -> dict[str, str]:
    if mode in ("shm", "loan") and ros_profile is None:
        raise ValueError(f"ROS {mode} mode requires an instantiated Fast DDS profile")
    return process_environment(base_env, mode, domain_id, ros_profile or Path())


def launch_trial(
    trial: dict[str, Any],
    trial_dir: Path,
    output_root: Path,
    rbt_executable: Path,
    ros_executable: Path,
    base_env: dict[str, str],
    config_dir: Path,
    cpus: set[int],
    seconds: float,
    warmup: float,
) -> dict[str, Any]:
    mode = trial["mode"]
    executable = rbt_executable if mode == "rbt" else ros_executable
    topic = f"rbtbench{uuid.uuid4().hex}{trial['trial_index']:03d}{trial['case']}{mode}"
    trial_dir.mkdir(parents=True, exist_ok=False)
    trial.update({"topic": topic, "trial_dir": str(trial_dir), "executable": str(executable)})
    ros_profile: Path | None = None
    if mode in ("shm", "loan"):
        template = config_dir / f"{mode}.xml"
        transport_settings = fastdds_transport_settings(mode, PAYLOAD_BYTES[trial["case"]])
        ros_profile = instantiate_fastdds_profile(
            template, trial_dir / "fastdds.xml", topic, mode, trial["case"]
        )
        trial["fastdds_profile"] = {
            "template": str(template),
            "template_sha256": sha256_file(template),
            "path": str(ros_profile),
            "sha256": sha256_file(ros_profile),
            "profile_name": f"/{topic}",
            **transport_settings,
        }
    else:
        trial["fastdds_profile"] = None
    window_file = trial_dir / "window.txt"
    ready_files = {role: trial_dir / f"{role}.ready.json" for role in ("pub", "sub")}
    output_files = {role: trial_dir / f"{role}.json" for role in ("pub", "sub")}
    environment = trial_environment(base_env, mode, trial["domain_id"], ros_profile)
    trial["environment"] = {key: environment[key] for key in ROS_ENV_KEYS if key in environment}
    commands = {
        role: trial_child_command(
            executable,
            role,
            trial["case"],
            mode,
            topic,
            window_file,
            ready_files[role],
            output_files[role],
        )
        for role in ("pub", "sub")
    }
    trial["commands"] = {role: command for role, command in commands.items()}
    atomic_write_json(trial_dir / "trial.json", trial)
    processes: dict[str, subprocess.Popen[Any]] = {}
    log_handles: list[Any] = []

    def child_setup() -> None:
        configure_child_process(cpus)

    try:
        for role in ("pub", "sub"):
            stdout_file = (trial_dir / f"{role}.stdout.log").open("wb")
            stderr_file = (trial_dir / f"{role}.stderr.log").open("wb")
            log_handles.extend((stdout_file, stderr_file))
            processes[role] = subprocess.Popen(
                commands[role],
                cwd=output_root.parent,
                env=environment,
                stdout=stdout_file,
                stderr=stderr_file,
                start_new_session=True,
                preexec_fn=child_setup,
            )
        trial["pids"] = {role: process.pid for role, process in processes.items()}
        ready = wait_for_ready(processes, ready_files, trial)
        validate_ready_pair(ready, mode)
        trial["ready"] = ready
        maps: dict[str, Any] = {}

        start_ns = time.clock_gettime_ns(time.CLOCK_MONOTONIC) + int(warmup * 1_000_000_000)
        end_ns = start_ns + int(seconds * 1_000_000_000)
        trial.update({"start_ns": start_ns, "end_ns": end_ns, "window_seconds": (end_ns - start_ns) / 1_000_000_000})
        atomic_write_text(window_file, f"{start_ns} {end_ns}\n")

        def capture_maps_during_steady_state() -> None:
            maps.update(capture_trial_maps(processes, trial_dir))
            trial["maps_snapshot_ns"] = maps["snapshot_ns"]

        cpu_start, cpu_end, memory_samples, cpu_start_ns, cpu_end_ns = sample_trial_window(
            processes, start_ns, end_ns, capture_maps_during_steady_state
        )
        trial["cpu_sample_start_ns"] = cpu_start_ns
        trial["cpu_sample_end_ns"] = cpu_end_ns
        trial["memory_samples"] = memory_samples
        trial["cpu_ticks"] = {"start": cpu_start, "end": cpu_end}
        atomic_write_json(trial_dir / "resource_samples.json", {
            "cpu_ticks": trial["cpu_ticks"],
            "cpu_sample_start_ns": cpu_start_ns,
            "cpu_sample_end_ns": cpu_end_ns,
            "memory_samples": memory_samples,
        })

        exit_deadline = end_ns + int(EXIT_TIMEOUT_SECONDS * 1_000_000_000)
        while any(process.poll() is None for process in processes.values()):
            now_ns = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
            if now_ns > exit_deadline:
                raise TrialError(f"endpoints did not exit by end+{EXIT_TIMEOUT_SECONDS:g}s")
            for role, process in processes.items():
                if process.poll() not in (None, 0):
                    raise TrialError(f"{role} endpoint exited with status {process.returncode}")
            time.sleep(0.05)
        for role, process in processes.items():
            if process.returncode != 0:
                raise TrialError(f"{role} endpoint exited with status {process.returncode}")

        pub = parse_summary(output_files["pub"], "pub", trial)
        sub = parse_summary(output_files["sub"], "sub", trial)
        alias_evidence = resolve_address_probes(
            pub["address_probes"],
            sub["address_probes"],
            maps["pub"]["mappings"],
            maps["sub"]["mappings"],
        )
        atomic_write_json(trial_dir / "alias_evidence.json", alias_evidence)
        trial["alias_evidence"] = alias_evidence
        quality = validate_trial_summaries(
            pub,
            sub,
            trial["window_seconds"],
            trial["case"],
            mode,
            alias_evidence,
        )
        cpu = cpu_metrics(cpu_start, cpu_end, cpu_start_ns, cpu_end_ns)
        memory = aggregate_memory_samples(memory_samples)
        trial["status"] = "valid" if quality["valid"] else "invalid"
        trial["valid"] = quality["valid"]
        trial["reason"] = "; ".join(quality["reasons"])
        trial["summaries"] = {"pub": pub, "sub": sub}
        trial["metrics"] = {**quality, **cpu, **memory}
        return trial
    except UnsupportedTrial as error:
        terminate_process_groups(processes.values())
        trial["status"] = "unsupported"
        trial["valid"] = False
        trial["reason"] = str(error)
        return trial
    except KeyboardInterrupt:
        terminate_process_groups(processes.values())
        trial["status"] = "interrupted"
        trial["valid"] = False
        trial["reason"] = "interrupted by user"
        raise
    except Exception as error:
        terminate_process_groups(processes.values())
        trial["status"] = "failed"
        trial["valid"] = False
        trial["reason"] = f"{type(error).__name__}: {error}"
        raise TrialError(trial["reason"]) from error
    finally:
        for handle in log_handles:
            handle.close()
        trial["process_exit_codes"] = {
            role: process.poll() for role, process in processes.items()
        }
        atomic_write_json(trial_dir / "trial.json", trial)


def run(args: argparse.Namespace) -> int:
    if not math.isfinite(args.seconds) or args.seconds <= 0:
        raise ValueError("--seconds must be a positive finite value")
    if not math.isfinite(args.warmup) or args.warmup < 0:
        raise ValueError("--warmup must be a non-negative finite value")
    if args.repetitions < 1:
        raise ValueError("--repetitions must be at least 1")
    schedule = build_schedule(args.cases, args.modes, args.repetitions)
    if len(schedule) > ROS_DOMAIN_MAX - ROS_DOMAIN_MIN + 1:
        raise ValueError(
            f"matrix has {len(schedule)} trials but only {ROS_DOMAIN_MAX - ROS_DOMAIN_MIN + 1} unique ROS domains are available"
        )

    rbt_executable = resolve_executable(args.rbt_executable, "RBT executable")
    ros_executable = resolve_executable(args.ros_executable, "ROS executable")
    output_root = args.output.expanduser().resolve()
    if output_root.exists():
        raise ValueError(f"output path already exists: {output_root}")
    if args.ros_setup is not None:
        ros_setup = args.ros_setup.expanduser().resolve()
        if not ros_setup.is_file():
            raise ValueError(f"ROS setup script does not exist: {ros_setup}")
        args.ros_setup = ros_setup
    cpus = available_cpus(args.cpus)
    script_dir = Path(__file__).resolve().parent
    config_dir = script_dir / "config"
    profile_hashes: dict[str, str] = {}
    for mode in args.modes:
        if mode in ("shm", "loan"):
            config = config_dir / f"{mode}.xml"
            if not config.is_file():
                raise ValueError(f"required ROS profile does not exist: {config}")
            profile_hashes[mode] = sha256_file(config)

    binary_hashes = {
        "rbt": sha256_file(rbt_executable),
        "ros": sha256_file(ros_executable),
    }
    base_env = source_ros_environment(os.environ.copy(), args.ros_setup)
    output_root.parent.mkdir(parents=True, exist_ok=True)
    output_root.mkdir(parents=False, exist_ok=False)
    metadata = safe_runtime_metadata(cpus, base_env)
    results: dict[str, Any] = {
        "schema_version": 1,
        "status": "running",
        "created_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "configuration": {
            "rbt_executable": str(rbt_executable),
            "ros_executable": str(ros_executable),
            "seconds": args.seconds,
            "warmup": args.warmup,
            "repetitions": args.repetitions,
            "cases": args.cases,
            "modes": args.modes,
            "cpus": sorted(cpus),
            "ros_setup": str(args.ros_setup) if args.ros_setup else None,
            "binary_sha256": binary_hashes,
            "profile_sha256": profile_hashes,
            "rss_caveat": "sum RSS can double-count shared mappings; PSS is the primary aggregate",
        },
        "runtime_metadata": metadata,
        "trials": [],
    }
    for index, scheduled in enumerate(schedule, start=1):
        trial = {
            **scheduled,
            "trial_index": index,
            "domain_id": ROS_DOMAIN_MIN + index - 1,
            "status": "not_run",
            "valid": None,
            "reason": None,
        }
        results["trials"].append(trial)
    write_results(output_root, results)

    try:
        for trial, scheduled in zip(results["trials"], schedule):
            trial_dir = make_trial_dirs(output_root, trial["trial_index"], scheduled)
            try:
                completed_trial = launch_trial(
                    trial,
                    trial_dir,
                    output_root,
                    rbt_executable,
                    ros_executable,
                    base_env,
                    config_dir,
                    cpus,
                    args.seconds,
                    args.warmup,
                )
                trial.update(completed_trial)
            except TrialError:
                raise
            finally:
                write_results(output_root, results)
    except TrialError as error:
        results["status"] = "failed"
        results["error"] = str(error)
        write_results(output_root, results)
        print(f"benchmark stopped: {error}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        results["status"] = "interrupted"
        results["error"] = "interrupted by user"
        write_results(output_root, results)
        print("benchmark interrupted; active child process groups were stopped", file=sys.stderr)
        return 130

    invalid = sum(trial["status"] == "invalid" for trial in results["trials"])
    unsupported = sum(trial["status"] == "unsupported" for trial in results["trials"])
    if invalid or unsupported:
        results["status"] = "completed_with_invalid_trials" if invalid else "completed_with_unsupported_trials"
        results["invalid_trials"] = invalid
        results["unsupported_trials"] = unsupported
        write_results(output_root, results)
        return 1
    results["status"] = "completed"
    write_results(output_root, results)
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = make_parser()
    args = parser.parse_args(argv)
    try:
        return run(args)
    except KeyboardInterrupt:
        print("benchmark setup interrupted", file=sys.stderr)
        return 130
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Build and run the bounded three-language physical-camera timing gate."""

from __future__ import annotations

import argparse
import json
import os
import signal
import stat
import subprocess
import sys
import time
import uuid
from collections import OrderedDict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterator

REPO_ROOT = Path(__file__).resolve().parents[1]
MIN_DURATION_SEC = 300
READINESS_TIMEOUT_SEC = 10.0
MAX_RSS_KIB = 512 * 1024
MAX_POST_WARMUP_RSS_GROWTH_KIB = 32 * 1024
MAX_FDS = 128
MAX_POST_WARMUP_FD_GROWTH = 4
MAX_THREADS = 64
MAX_POST_WARMUP_THREAD_GROWTH = 4
WARMUP_SECONDS = 5.0
CHECKSUM_WINDOW = 4096
MIN_COMMON_FRAMES = 3
V4L2_BUF_FLAG_TIMESTAMP_MASK = 0x0000_E000
V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC = 0x0000_2000
V4L2_BUF_FLAG_TSTAMP_SRC_MASK = 0x0007_0000
V4L2_BUF_FLAG_TSTAMP_SRC_SOE = 0x0001_0000


class GateFailure(RuntimeError):
    pass


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the real-device Rust/C++/Python StampedBytes capture gate."
    )
    parser.add_argument("--device", help="explicit absolute V4L2 device path")
    parser.add_argument("--duration", type=int, help="hardware capture duration (at least 300 seconds)")
    parser.add_argument("--log-dir", type=Path, help="directory for command logs, JSONL logs, and summary")
    parser.add_argument("--python", dest="python_path", type=Path, help="installed verification virtualenv interpreter")
    parser.add_argument("--self-test", action="store_true", help="run capture-source contract fixtures without hardware")
    options = parser.parse_args()
    if options.self_test:
        if options.device is not None or options.duration is not None:
            parser.error("--self-test cannot be combined with --device or --duration")
        return options
    if options.device is None or options.duration is None:
        parser.error("--device and --duration are required for a hardware run")
    if not Path(options.device).is_absolute():
        parser.error("--device must be an explicit absolute path")
    if options.duration < MIN_DURATION_SEC:
        parser.error(f"--duration must be at least {MIN_DURATION_SEC} seconds")
    return options


def iter_jsonl(path: Path, *, strict: bool = True) -> Iterator[dict[str, Any]]:
    with path.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, start=1):
            if len(line) > 1_000_000:
                raise GateFailure(f"JSONL record exceeds one megabyte at {path}:{line_number}")
            stripped = line.strip()
            if not stripped:
                continue
            try:
                record = json.loads(stripped)
            except json.JSONDecodeError as error:
                if strict:
                    raise GateFailure(f"malformed JSON record at {path}:{line_number}: {error}") from error
                continue
            if not isinstance(record, dict):
                raise GateFailure(f"non-object JSON record at {path}:{line_number}")
            yield record


def zero_offset_from_source(source_id: object) -> bool:
    if not isinstance(source_id, str) or len(source_id) != 96:
        raise GateFailure("host monotonic source id must contain 96 lowercase hexadecimal digits")
    if any(character not in "0123456789abcdef" for character in source_id):
        raise GateFailure("host monotonic source id contains non-canonical hexadecimal digits")
    return not any(bytes.fromhex(source_id)[32:48])


def expected_capture_present(driver_flags: int, namespace_offset_zero: bool) -> bool:
    return (
        namespace_offset_zero
        and driver_flags & V4L2_BUF_FLAG_TIMESTAMP_MASK == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC
        and driver_flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK == V4L2_BUF_FLAG_TSTAMP_SRC_SOE
    )


def validate_capture_contract(capture_present: bool, driver_flags: int, namespace_offset_zero: bool, label: str) -> None:
    expected = expected_capture_present(driver_flags, namespace_offset_zero)
    if capture_present is not expected:
        state = "known" if expected else "unknown"
        raise GateFailure(f"{label} capture presence disagrees with the {state} MONOTONIC+SOE source contract")


def run_nonhardware_self_test() -> None:
    eof_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC
    validate_capture_contract(False, eof_flags, True, "EOF fixture")
    unknown_basis_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC | V4L2_BUF_FLAG_TSTAMP_SRC_SOE
    validate_capture_contract(False, unknown_basis_flags, False, "unknown-offset fixture")
    validate_capture_contract(
        True,
        V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC | V4L2_BUF_FLAG_TSTAMP_SRC_SOE,
        True,
        "verified SOE fixture",
    )
    try:
        validate_capture_contract(True, eof_flags, True, "fake EOF fixture")
    except GateFailure:
        pass
    else:
        raise GateFailure("capture-source self-test accepted a fake EOF capture stamp")
    print("RBT_TIME_HARDWARE_SELF_TEST PASS eof_unknown=accepted fake_eof_capture=rejected", flush=True)


def read_proc_metrics(pid: int) -> dict[str, int] | None:
    proc = Path("/proc") / str(pid)
    try:
        status_lines = (proc / "status").read_text(encoding="ascii").splitlines()
        values: dict[str, int] = {}
        for line in status_lines:
            if line.startswith("VmRSS:"):
                values["rss_kib"] = int(line.split()[1])
            elif line.startswith("Threads:"):
                values["threads"] = int(line.split()[1])
        values["fds"] = len(list((proc / "fd").iterdir()))
        if "rss_kib" not in values or "threads" not in values:
            return None
        return values
    except (FileNotFoundError, ProcessLookupError, PermissionError, ValueError, OSError):
        return None


class Child:
    def __init__(self, label: str, command: list[str], stdout_path: Path, stderr_path: Path, *, bounded: bool):
        self.label = label
        self.command = command
        self.stdout_path = stdout_path
        self.stderr_path = stderr_path
        self.stdout = stdout_path.open("w", encoding="utf-8")
        self.stderr = stderr_path.open("w", encoding="utf-8")
        self.process = subprocess.Popen(
            command,
            cwd=REPO_ROOT,
            env=CHILD_ENV,
            stdout=self.stdout,
            stderr=self.stderr,
            start_new_session=True,
            close_fds=True,
        )
        self.started_monotonic = time.monotonic()
        self.stdout_scan_offset = 0
        self.stdout_event_offset = 0
        self.stdout_event_partial = b""
        self.bounded = bounded
        self.exit_code: int | None = None
        self.metrics: dict[str, Any] = {
            "pid": self.process.pid,
            "command": command,
            "bounded_hardware_process": bounded,
            "samples": 0,
            "initial": None,
            "warmup_baseline": None,
            "peak": {"rss_kib": 0, "fds": 0, "threads": 0},
            "post_warmup_peak": {"rss_kib": 0, "fds": 0, "threads": 0},
            "final": None,
            "driver_reopen_resources": None,
            "driver_reopen_resources_stable": None,
            "pid_absent_after_reap": False,
            "process_group_empty_after_reap": False,
        }

    def sample(self) -> None:
        current = read_proc_metrics(self.process.pid)
        if current is None:
            return
        self.metrics["samples"] += 1
        if self.metrics["initial"] is None:
            self.metrics["initial"] = current
        if self.bounded and time.monotonic() - self.started_monotonic >= WARMUP_SECONDS:
            if self.metrics["warmup_baseline"] is None:
                self.metrics["warmup_baseline"] = current
            for key, value in current.items():
                self.metrics["post_warmup_peak"][key] = max(self.metrics["post_warmup_peak"][key], value)
        self.metrics["final"] = current
        for key, value in current.items():
            self.metrics["peak"][key] = max(self.metrics["peak"][key], value)

    def observe_driver_reopen(self) -> None:
        if self.label != "camera-producer" or self.metrics["driver_reopen_resources"] is not None:
            return
        with self.stdout_path.open("rb") as stream:
            stream.seek(self.stdout_scan_offset)
            content = stream.read()
            complete_length = content.rfind(b"\n") + 1
            complete = content[:complete_length]
            self.stdout_scan_offset += complete_length
        if b'"event":"driver_session_reopened"' not in complete:
            return
        current = self.metrics["final"]
        baseline = self.metrics["warmup_baseline"]
        self.metrics["driver_reopen_resources"] = current
        if current is None or baseline is None:
            self.metrics["driver_reopen_resources_stable"] = False
            raise GateFailure("producer driver reopen occurred before resource warmup completed")
        deltas = {key: current[key] - baseline[key] for key in ("rss_kib", "fds", "threads")}
        self.metrics["driver_reopen_resource_deltas"] = deltas
        stable = (
            deltas["rss_kib"] <= MAX_POST_WARMUP_RSS_GROWTH_KIB
            and deltas["fds"] <= MAX_POST_WARMUP_FD_GROWTH
            and deltas["threads"] <= MAX_POST_WARMUP_THREAD_GROWTH
        )
        self.metrics["driver_reopen_resources_stable"] = stable
        if not stable:
            raise GateFailure(f"producer resources did not return to warmup bounds after driver reopen: {deltas}")

    def read_new_stdout_records(self) -> Iterator[dict[str, Any]]:
        with self.stdout_path.open("rb") as stream:
            stream.seek(self.stdout_event_offset)
            data = stream.read()
            self.stdout_event_offset += len(data)
        pending = self.stdout_event_partial + data
        complete_length = pending.rfind(b"\n") + 1
        complete = pending[:complete_length]
        self.stdout_event_partial = pending[complete_length:]
        if len(self.stdout_event_partial) > 1_000_000:
            raise GateFailure(f"{self.label} emitted an unterminated JSONL record over one megabyte")
        for line in complete.splitlines():
            if len(line) > 1_000_000:
                raise GateFailure(f"{self.label} JSONL record exceeds one megabyte")
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except (json.JSONDecodeError, UnicodeDecodeError) as error:
                raise GateFailure(f"{self.label} emitted malformed JSONL: {error}") from error
            if not isinstance(record, dict):
                raise GateFailure(f"{self.label} emitted a non-object JSONL record")
            yield record

    def reap(self) -> int:
        if self.exit_code is None:
            self.exit_code = self.process.wait()
        self.stdout.close()
        self.stderr.close()
        self.metrics["exit_code"] = self.exit_code
        self.metrics["pid_absent_after_reap"] = not Path("/proc", str(self.process.pid)).exists()
        baseline = self.metrics["warmup_baseline"]
        if baseline is not None:
            self.metrics["post_warmup_growth"] = {
                key: self.metrics["post_warmup_peak"][key] - baseline[key]
                for key in ("rss_kib", "fds", "threads")
            }
        try:
            os.killpg(self.process.pid, 0)
        except ProcessLookupError:
            self.metrics["process_group_empty_after_reap"] = True
        except PermissionError:
            self.metrics["process_group_empty_after_reap"] = False
        else:
            self.metrics["process_group_empty_after_reap"] = False
        return self.exit_code

    def enforce_bounds(self) -> None:
        if not self.bounded:
            return
        peak = self.metrics["peak"]
        if peak["rss_kib"] > MAX_RSS_KIB:
            raise GateFailure(f"{self.label} RSS exceeded {MAX_RSS_KIB} KiB: {peak['rss_kib']} KiB")
        warmup = self.metrics["warmup_baseline"]
        post_peak = self.metrics["post_warmup_peak"]
        if warmup is not None and post_peak["rss_kib"] - warmup["rss_kib"] > MAX_POST_WARMUP_RSS_GROWTH_KIB:
            raise GateFailure(f"{self.label} post-warmup RSS grew by more than {MAX_POST_WARMUP_RSS_GROWTH_KIB} KiB")
        if peak["fds"] > MAX_FDS:
            raise GateFailure(f"{self.label} open FD count exceeded {MAX_FDS}")
        if warmup is not None and post_peak["fds"] - warmup["fds"] > MAX_POST_WARMUP_FD_GROWTH:
            raise GateFailure(f"{self.label} post-warmup open FD count grew by more than {MAX_POST_WARMUP_FD_GROWTH}")
        if peak["threads"] > MAX_THREADS:
            raise GateFailure(f"{self.label} thread count exceeded {MAX_THREADS}")
        if warmup is not None and post_peak["threads"] - warmup["threads"] > MAX_POST_WARMUP_THREAD_GROWTH:
            raise GateFailure(f"{self.label} post-warmup thread count grew by more than {MAX_POST_WARMUP_THREAD_GROWTH}")


def write_command(path: Path, command: list[str]) -> None:
    path.write_text(json.dumps(command, indent=2) + "\n", encoding="utf-8")


def run_build(label: str, command: list[str], run_dir: Path, children: list[Child], results: dict[str, Any]) -> None:
    out_path = run_dir / f"{label}.stdout.log"
    err_path = run_dir / f"{label}.stderr.log"
    write_command(run_dir / f"{label}.command.json", command)
    child = Child(label, command, out_path, err_path, bounded=False)
    children.append(child)
    deadline = time.monotonic() + 900
    while child.process.poll() is None:
        child.sample()
        if time.monotonic() >= deadline:
            raise GateFailure(f"{label} exceeded its 900-second build timeout")
        time.sleep(0.5)
    child.sample()
    exit_code = child.reap()
    results[label] = {"exit_code": exit_code, "stdout": str(out_path), "stderr": str(err_path)}
    if exit_code != 0:
        raise GateFailure(f"{label} failed with exit code {exit_code}; see {out_path} and {err_path}")


def wait_for_event(child: Child, event: str, timeout: float, children: list[Child]) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for monitored in children:
            monitored.sample()
            monitored.enforce_bounds()
            monitored.observe_driver_reopen()
        for record in child.read_new_stdout_records():
            if record.get("event") == event:
                return record
        code = child.process.poll()
        if code is not None:
            child.sample()
            child.reap()
            raise GateFailure(f"{child.label} exited with {code} before {event}; see {child.stderr_path}")
        time.sleep(0.1)
    raise GateFailure(f"{child.label} did not emit {event} within {timeout:g} seconds")


def wait_for_children(children: list[Child], timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while True:
        all_done = True
        for child in children:
            child.sample()
            child.observe_driver_reopen()
            code = child.process.poll()
            if code is None:
                all_done = False
            elif child.exit_code is None:
                child.sample()
                child.reap()
                if code != 0:
                    raise GateFailure(f"{child.label} exited with {code}; see {child.stdout_path} and {child.stderr_path}")
            child.enforce_bounds()
        if all_done:
            return
        if time.monotonic() >= deadline:
            running = [child.label for child in children if child.process.poll() is None]
            raise GateFailure(f"hardware processes exceeded their completion deadline: {', '.join(running)}")
        time.sleep(0.5)


def stop_and_reap(children: list[Child]) -> None:
    for child in children:
        if child.process.poll() is None:
            try:
                os.killpg(child.process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
    grace_deadline = time.monotonic() + 5.0
    while time.monotonic() < grace_deadline:
        running = False
        for child in children:
            if child.process.poll() is None:
                running = True
                child.sample()
        if not running:
            break
        time.sleep(0.1)
    for child in children:
        if child.process.poll() is None:
            try:
                os.killpg(child.process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        child.sample()
        child.reap()


def summarize_consumer(
    label: str, path: Path, *, cpp: bool = False, expected_duration: int, reopen_generation: int
) -> tuple[dict[str, Any], OrderedDict[tuple[int, int], str]]:
    ready: dict[str, Any] | None = None
    summary: dict[str, Any] | None = None
    frame_count = 0
    frame_capture_known = 0
    frame_capture_unknown = 0
    checksums: OrderedDict[tuple[int, int], str] = OrderedDict()
    for record in iter_jsonl(path):
        event = record.get("event")
        if event == "consumer_ready":
            if ready is not None:
                raise GateFailure(f"{label} emitted more than one consumer_ready event")
            ready = record
            source_id = ready.get("clock_source_id")
            source_offset_zero = zero_offset_from_source(source_id)
            if "namespace_offset_zero" in ready and ready["namespace_offset_zero"] is not source_offset_zero:
                raise GateFailure(f"{label} readiness offset flag disagrees with its host clock identity")
        elif event == "frame_checked":
            if ready is None:
                raise GateFailure(f"{label} emitted a frame before consumer_ready")
            frame_count += 1
            key = (int(record["driver_session_generation"]), int(record["sequence"]))
            checksum = str(record["fnv1a64"])
            if len(checksum) != 16 or any(character not in "0123456789abcdef" for character in checksum):
                raise GateFailure(f"{label} emitted a malformed FNV-1a checksum")
            flags = int(record.get("driver_flags", -1))
            capture_present = record.get("capture_present") is True
            if flags < 0 or flags > 0xFFFF_FFFF:
                raise GateFailure(f"{label} emitted invalid raw V4L2 flags")
            validate_capture_contract(capture_present, flags, source_offset_zero, label)
            if "capture_known" in record and record["capture_known"] is not capture_present:
                raise GateFailure(f"{label} capture_known disagrees with capture_present")
            if record.get("received_source_id") != source_id:
                raise GateFailure(f"{label} frame receive clock identity changed within the consumer")
            width = int(record.get("width", 0))
            height = int(record.get("height", 0))
            stride = int(record.get("stride", 0))
            if width <= 0 or height <= 0 or stride < width * 2 or record.get("fourcc") != "YUYV":
                raise GateFailure(f"{label} emitted invalid YUYV frame geometry")
            if int(record.get("pixel_bytes", -1)) != stride * height:
                raise GateFailure(f"{label} pixel byte count does not equal stride*height")
            frame_capture_known += int(capture_present)
            frame_capture_unknown += int(not capture_present)
            if key in checksums:
                raise GateFailure(f"{label} emitted duplicate frame key {key}")
            checksums[key] = checksum
            if len(checksums) > CHECKSUM_WINDOW:
                checksums.popitem(last=False)
        elif event == "consumer_summary":
            if summary is not None:
                raise GateFailure(f"{label} emitted more than one consumer_summary event")
            summary = record
    if ready is None:
        raise GateFailure(f"{label} emitted no consumer_ready event")
    if summary is None:
        raise GateFailure(f"{label} emitted no consumer_summary event")
    if frame_count == 0 or int(summary.get("received", 0)) == 0:
        raise GateFailure(f"{label} received no camera frames")
    if frame_count != int(summary.get("received", 0)):
        raise GateFailure(f"{label} frame_checked records do not match its received count")
    if summary.get("capture_known", 0) + summary.get("capture_unknown", 0) != summary.get("received"):
        raise GateFailure(f"{label} capture counts do not match its received frame count")
    if summary.get("capture_known") != frame_capture_known or summary.get("capture_unknown") != frame_capture_unknown:
        raise GateFailure(f"{label} capture summary does not match per-frame source metadata")
    if int(summary.get("actual_elapsed_ms", 0)) < expected_duration * 1000:
        raise GateFailure(f"{label} consumer ran for less than its requested duration")
    generations = summary.get("driver_session_generations", [])
    if not {0, reopen_generation}.issubset(set(generations)):
        raise GateFailure(f"{label} did not observe initial and reopened driver generations: {generations}")
    if summary.get("duplicate_sequences", 0) or summary.get("out_of_order_sequences", 0):
        raise GateFailure(f"{label} saw duplicate or out-of-order driver sequences")
    if cpp:
        if summary.get("resource_wrappers_after_cleanup") != 0:
            raise GateFailure("C++ subscriber retained stream resource wrappers after cleanup")
        if summary.get("pressure_after_receive", {}).get("borrowed_samples") != 0:
            raise GateFailure("C++ subscriber retained borrowed samples after processing")
    elif label == "rust":
        if summary.get("resource_wrappers_after_cleanup") != summary.get("resource_wrappers_before"):
            raise GateFailure("Rust subscriber retained stream resource wrappers after cleanup")
        if summary.get("pressure_after_receive", {}).get("borrowed_samples") != 0:
            raise GateFailure("Rust subscriber retained borrowed samples after processing")
    elif label == "python":
        if summary.get("retained_view_readonly_after_sample_drop") is not True:
            raise GateFailure("Python did not verify read-only view ownership after dropping the sample")
        if summary.get("resources", {}).get("subscriber_closed") is not True:
            raise GateFailure("Python subscriber did not close its native handle")
        if summary.get("resources", {}).get("borrowed_samples_before_cleanup") != 0:
            raise GateFailure("Python subscriber retained borrowed samples after processing")

    stats = {
        "ready": ready,
        "summary": summary,
        "frame_records": frame_count,
        "checksum_window_records": len(checksums),
        "stdout": str(path),
    }
    return stats, checksums


def verify_producer(path: Path, device: str, duration: int) -> dict[str, Any]:
    device_record: dict[str, Any] | None = None
    reopen_event: dict[str, Any] | None = None
    summary: dict[str, Any] | None = None
    frame_count = 0
    frame_capture_known = 0
    frame_capture_unknown = 0
    frame_generations: set[int] = set()
    sequence_discontinuities: list[dict[str, Any]] = []
    current_generation = 0
    for record in iter_jsonl(path):
        event = record.get("event")
        if event == "device":
            if device_record is not None:
                raise GateFailure("producer emitted more than one device readiness record")
            device_record = record
        elif event == "driver_session_reopened":
            if reopen_event is not None:
                raise GateFailure("producer emitted more than one driver reopen event")
            reopen_event = record
            if record.get("session_generation") != current_generation + 1:
                raise GateFailure("scheduled driver reopen did not advance the current generation")
            current_generation += 1
        elif event == "driver_sequence_discontinuity":
            if len(sequence_discontinuities) >= 7:
                raise GateFailure("too many driver sequence discontinuities")
            if record.get("session_generation") != current_generation + 1:
                raise GateFailure("driver sequence discontinuity did not advance the generation")
            if record.get("host_clock_generation_changed") is not False:
                raise GateFailure("driver sequence change was incorrectly promoted to a host-clock reset")
            current_generation += 1
            sequence_discontinuities.append(record)
        elif event == "frame_published":
            if device_record is None:
                raise GateFailure("producer published a frame before its device readiness event")
            frame_count += 1
            flags = int(record.get("driver_flags", -1))
            if flags < 0 or flags > 0xFFFF_FFFF:
                raise GateFailure("producer emitted invalid raw V4L2 flags")
            present = record.get("capture_present") is True
            basis_zero = device_record.get("host_monotonic_offset_zero") is True
            validate_capture_contract(present, flags, basis_zero, "producer")
            frame_generation = int(record.get("driver_session_generation", -1))
            if frame_generation != current_generation or current_generation >= 8:
                raise GateFailure("frame generation disagrees with recorded continuity events")
            frame_generations.add(frame_generation)
            if present and (
                record.get("driver_timestamp_type") != "monotonic"
                or record.get("driver_timestamp_source") != "start_of_exposure"
            ):
                raise GateFailure("producer marked a non-MONOTONIC or non-SOE frame as capture time")
            if record.get("capture_status") == "driver_event_is_end_of_frame" and present:
                raise GateFailure("producer promoted an EOF driver event to capture time")
            frame_capture_known += int(present)
            frame_capture_unknown += int(not present)
        elif event == "producer_summary":
            if summary is not None:
                raise GateFailure("producer emitted more than one producer_summary")
            summary = record

    if device_record is None:
        raise GateFailure("producer emitted no device readiness record")
    if device_record.get("device") != device:
        raise GateFailure(f"producer opened {device_record.get('device')!r}, expected explicit device {device!r}")
    if device_record.get("fourcc") != "YUYV":
        raise GateFailure(f"producer negotiated {device_record.get('fourcc')!r}, expected YUYV")
    width = int(device_record.get("width", 0))
    height = int(device_record.get("height", 0))
    stride = int(device_record.get("stride", 0))
    if width <= 0 or height <= 0 or stride < width * 2:
        raise GateFailure("producer reported invalid negotiated YUYV geometry")
    offset_zero = device_record.get("host_monotonic_offset_zero")
    if not isinstance(offset_zero, bool):
        raise GateFailure("producer did not report the monotonic namespace offset basis")
    source_offset_zero = zero_offset_from_source(device_record.get("host_clock_source_id"))
    if source_offset_zero is not offset_zero:
        raise GateFailure("producer offset flag disagrees with its host clock identity")
    if reopen_event is None:
        raise GateFailure("producer did not perform the expected stream stop/close/reopen")
    if not {0, int(reopen_event["session_generation"])}.issubset(frame_generations):
        raise GateFailure(f"producer did not publish frames before and after scheduled reopen: {sorted(frame_generations)}")
    if summary is None:
        raise GateFailure("producer emitted no producer_summary")
    if int(summary.get("actual_elapsed_ms", 0)) < duration * 1000:
        raise GateFailure("producer steady-clock elapsed time was shorter than the requested duration")
    if int(summary.get("frames_dequeued", 0)) <= 0 or int(summary.get("published", 0)) <= 0:
        raise GateFailure("producer did not dequeue and publish camera frames")
    if int(summary.get("driver_session_generation", -1)) != current_generation:
        raise GateFailure("producer summary generation disagrees with its continuity events")
    required_summary_fields = (
        "capture_known", "capture_unknown", "driver_to_receive_count", "driver_to_receive_negative_count",
        "driver_to_receive_arithmetic_invalid_count", "driver_to_receive_min_ns",
        "driver_to_receive_max_ns", "driver_to_receive_mean_ns", "capture_to_receive_count",
        "capture_to_receive_invalid_count", "capture_to_receive_min_ns", "capture_to_receive_max_ns",
        "capture_to_receive_mean_ns", "driver_sequence_gap_count", "driver_sequence_duplicate_count",
        "driver_sequence_out_of_order_count", "driver_timestamp_forward_step_count",
        "driver_timestamp_backwards_count", "driver_timestamp_equal_count",
    )
    if any(field not in summary for field in required_summary_fields):
        missing = [field for field in required_summary_fields if field not in summary]
        raise GateFailure(f"producer summary is missing latency or driver-step fields: {missing}")
    if int(summary.get("capture_known", 0)) + int(summary.get("capture_unknown", 0)) != int(summary.get("published", 0)):
        raise GateFailure("producer capture known/unknown counts do not match published frames")
    if frame_count != int(summary.get("published", 0)):
        raise GateFailure("producer JSONL frame records do not match its published count")
    if summary.get("capture_known") != frame_capture_known or summary.get("capture_unknown") != frame_capture_unknown:
        raise GateFailure("producer capture summary does not match per-frame V4L2 source metadata")
    if not offset_zero and (
        int(summary.get("driver_to_receive_count", 0)) != 0
        or int(summary.get("capture_to_receive_count", 0)) != 0
    ):
        raise GateFailure("producer reported latency without a verified host monotonic offset basis")
    return {
        "device": device_record,
        "summary": summary,
        "published_frame_records": frame_count,
        "frame_capture_known": frame_capture_known,
        "frame_capture_unknown": frame_capture_unknown,
        "reopen_event": reopen_event,
        "sequence_discontinuities": sequence_discontinuities,
        "stdout": str(path),
    }


def validate_python_environment(python: Path, env: dict[str, str], run_dir: Path, children: list[Child], builds: dict[str, Any]) -> None:
    code = """
import pathlib, sys
import rbt
from rbt import _rbt
import rbt.time_stream
module = pathlib.Path(rbt.__file__).resolve()
installed = any(part in {\"site-packages\", \"dist-packages\"} for part in module.parts)
develop_root = pathlib.Path(sys.argv[1]).resolve()
develop = module.is_relative_to(develop_root)
if not (installed or develop):
    raise SystemExit(\"rbt is neither installed nor loaded from this repository's develop package: \" + str(module))
if not hasattr(_rbt, \"_StampedBytesSubscriber\"):
    raise SystemExit(\"native StampedBytes subscriber is missing\")
print(module)
"""
    run_build(
        "python-native-check",
        [str(python), "-I", "-c", code, str(REPO_ROOT / "python")],
        run_dir,
        children,
        builds,
    )


def main() -> int:
    global CHILD_ENV
    options = parse_args()
    if options.self_test:
        run_nonhardware_self_test()
        return 0
    device_path = Path(options.device)
    run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
    log_root = options.log_dir or Path(os.environ.get("RBT_TIME_HARDWARE_LOG_DIR", REPO_ROOT / "target/time-hardware-gate-logs"))
    run_dir = (REPO_ROOT / log_root).resolve() if not log_root.is_absolute() else log_root.resolve()
    run_dir = run_dir / run_id
    run_dir.mkdir(parents=True, exist_ok=False)
    summary_path = run_dir / "summary.json"
    python = options.python_path or Path(os.environ.get("RBT_TIME_PYTHON", REPO_ROOT / "target/python-verify-venv/bin/python"))
    python = Path(os.path.abspath(python))

    toolchain_root = Path(os.environ.get("RBT_PHASE4_TOOLCHAIN", REPO_ROOT / "target/phase4-toolchain")).resolve()
    toolchain_bin = toolchain_root / "bin"
    compiler = Path(os.environ.get("CXX", toolchain_bin / "x86_64-conda-linux-gnu-g++")).resolve()
    c_compiler = Path(os.environ.get("CC", toolchain_bin / "x86_64-conda-linux-gnu-gcc")).resolve()
    archiver = Path(os.environ.get("AR", toolchain_bin / "x86_64-conda-linux-gnu-ar")).resolve()
    cpp_prefix = Path(os.environ.get("RBT_IOX2_PREFIX", REPO_ROOT / "target/iox2-rbt-0.10.0-cpp-install")).resolve()
    child_env = os.environ.copy()
    child_env.pop("PYTHONPATH", None)
    child_env["PATH"] = f"{toolchain_bin}:{child_env.get('PATH', '')}"
    child_env["CC"] = str(c_compiler)
    child_env["CXX"] = str(compiler)
    child_env["AR"] = str(archiver)
    child_env["CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER"] = child_env.get(
        "CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER", str(c_compiler)
    )
    lib_paths = [str(toolchain_root / "lib"), str(cpp_prefix / "lib")]
    if child_env.get("LD_LIBRARY_PATH"):
        lib_paths.append(child_env["LD_LIBRARY_PATH"])
    child_env["LD_LIBRARY_PATH"] = ":".join(lib_paths)
    library_paths = [str(toolchain_root / "lib"), str(cpp_prefix / "lib")]
    if child_env.get("LIBRARY_PATH"):
        library_paths.append(child_env["LIBRARY_PATH"])
    child_env["LIBRARY_PATH"] = ":".join(library_paths)
    CHILD_ENV = child_env

    children: list[Child] = []
    hardware_children: list[Child] = []
    builds: dict[str, Any] = {}
    status = "NOTRUN"
    reason: str | None = None
    report: dict[str, Any] = {
        "status": "NOTRUN",
        "hardware_capture": "NOTRUN",
        "gate_kind": "standalone_physical_camera_hardware",
        "software_soak_gate": False,
        "device": str(device_path),
        "duration_sec": options.duration,
        "service": f"rbt/time/hardware/{run_id}",
        "run_dir": str(run_dir),
        "reason": None,
        "producer": None,
        "consumers": {},
        "resources": {},
        "source_contract": {},
        "common_frames_count": 0,
        "physical_usb_unplug_replug": "NOTRUN",
        "builds": builds,
    }

    try:
        if not device_path.exists():
            raise GateFailure(f"explicit device path does not exist: {device_path}")
        try:
            mode = device_path.stat().st_mode
        except OSError as error:
            raise GateFailure(f"cannot stat selected device {device_path}: {error}") from error
        if not stat.S_ISCHR(mode):
            raise GateFailure(f"selected device path is not a character device: {device_path}")
        for binary, name in ((python, "Python interpreter"), (compiler, "pinned C++ compiler"), (c_compiler, "pinned C compiler"), (archiver, "pinned archiver")):
            if not binary.is_file() or not os.access(binary, os.X_OK):
                raise GateFailure(f"{name} is missing or not executable: {binary}")
        if not cpp_prefix.is_dir():
            raise GateFailure(f"C++ runtime prefix is missing: {cpp_prefix}")
        if not python.is_file() or not os.access(python, os.X_OK):
            raise GateFailure(f"installed verification Python is missing: {python}")
        validate_python_environment(python, child_env, run_dir, children, builds)

        run_build(
            "rust-consumer-build",
            ["cargo", "build", "--locked", "-p", "rbt-time-camera-consumer"],
            run_dir,
            children,
            builds,
        )
        cpp_build = Path(os.environ.get("RBT_TIME_CPP_BUILD", REPO_ROOT / "target/time-hardware-sdk-build")).resolve()
        run_build(
            "cpp-sdk-configure",
            [
                "cmake", "-S", str(REPO_ROOT / "sdk/cpp"), "-B", str(cpp_build),
                "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug", f"-DCMAKE_C_COMPILER={c_compiler}",
                f"-DCMAKE_CXX_COMPILER={compiler}", f"-DCMAKE_PREFIX_PATH={cpp_prefix}",
            ],
            run_dir,
            children,
            builds,
        )
        run_build(
            "cpp-camera-build",
            ["cmake", "--build", str(cpp_build), "--target", "rbt_time_camera", "rbt_time_camera_consumer", "--parallel"],
            run_dir,
            children,
            builds,
        )

        rust_consumer = REPO_ROOT / "target/debug/rbt-time-camera-consumer"
        cpp_producer = cpp_build / "rbt_time_camera"
        cpp_consumer = cpp_build / "rbt_time_camera_consumer"
        for binary in (rust_consumer, cpp_producer, cpp_consumer):
            if not binary.is_file() or not os.access(binary, os.X_OK):
                raise GateFailure(f"built hardware-gate executable is missing: {binary}")

        service = report["service"]
        producer_log = run_dir / "producer.jsonl"
        producer = Child(
            "camera-producer",
            [str(cpp_producer), "--device", str(device_path), "--service", service,
             "--duration-sec", str(options.duration), "--restart-after-sec", str(options.duration // 2)],
            producer_log,
            run_dir / "producer.stderr.log",
            bounded=True,
        )
        children.append(producer)
        hardware_children.append(producer)
        status = "FAIL"
        device_event = wait_for_event(producer, "device", READINESS_TIMEOUT_SEC, children)
        if device_event.get("device") != str(device_path):
            raise GateFailure("producer readiness event does not identify the explicit selected device")
        if device_event.get("fourcc") != "YUYV":
            raise GateFailure("producer readiness event does not report YUYV")

        rust_log = run_dir / "rust-consumer.jsonl"
        cpp_log = run_dir / "cpp-consumer.jsonl"
        python_log = run_dir / "python-consumer.jsonl"
        consumer_duration = options.duration + 3
        consumer_specs = [
            ("rust", [str(rust_consumer), "--service", service, "--duration-sec", str(consumer_duration)], rust_log),
            ("cpp", [str(cpp_consumer), "--service", service, "--duration-sec", str(consumer_duration)], cpp_log),
            ("python", [str(python), "-I", str(REPO_ROOT / "python/tests/time_hardware_probe.py"),
                        "--service", service, "--duration-sec", str(consumer_duration)], python_log),
        ]
        consumers: dict[str, Child] = {}
        for label, command, stdout_path in consumer_specs:
            child = Child(label, command, stdout_path, run_dir / f"{label}-consumer.stderr.log", bounded=True)
            children.append(child)
            hardware_children.append(child)
            consumers[label] = child
        ready_events: dict[str, dict[str, Any]] = {}
        for label, child in consumers.items():
            ready_events[label] = wait_for_event(child, "consumer_ready", READINESS_TIMEOUT_SEC, children)
        run_dir.joinpath("readiness.json").write_text(
            json.dumps({"device": device_event, "consumers": ready_events}, indent=2) + "\n", encoding="utf-8"
        )

        wait_for_children(hardware_children, options.duration + 45.0)
        producer_stats = verify_producer(producer_log, str(device_path), options.duration)
        consumer_stats: dict[str, Any] = {}
        checksum_maps: dict[str, OrderedDict[tuple[int, int], str]] = {}
        for label, path in (("rust", rust_log), ("cpp", cpp_log), ("python", python_log)):
            stats, checksums = summarize_consumer(
                label, path, cpp=(label == "cpp"), expected_duration=consumer_duration,
                reopen_generation=int(producer_stats["reopen_event"]["session_generation"]),
            )
            consumer_stats[label] = stats
            checksum_maps[label] = checksums

        common_keys = set(checksum_maps["rust"]).intersection(checksum_maps["cpp"], checksum_maps["python"])
        common_mismatches = [
            key for key in common_keys
            if len({checksum_maps[label][key] for label in ("rust", "cpp", "python")}) != 1
        ]
        if common_mismatches:
            raise GateFailure(f"pixel checksums differ for shared frames: {common_mismatches[:5]}")
        if len(common_keys) < MIN_COMMON_FRAMES:
            raise GateFailure(f"only {len(common_keys)} common frame checksums; need at least {MIN_COMMON_FRAMES}")

        producer_source = producer_stats["device"].get("host_clock_source_id")
        consumer_sources = {label: stats["ready"].get("clock_source_id") for label, stats in consumer_stats.items()}
        if not producer_source or any(source != producer_source for source in consumer_sources.values()):
            raise GateFailure(f"producer and consumer host clock identities differ: {producer_source!r} vs {consumer_sources!r}")
        offset_zero = producer_stats["device"].get("host_monotonic_offset_zero")
        if any(zero_offset_from_source(source) is not offset_zero for source in consumer_sources.values()):
            raise GateFailure("producer and consumers disagree on the monotonic namespace offset basis")
        report.update(
            {
                "producer": producer_stats,
                "consumers": consumer_stats,
                "common_frames_count": len(common_keys),
                "source_contract": {
                    "producer_host_clock_source_id": producer_source,
                    "consumer_host_clock_source_ids": consumer_sources,
                    "namespace_offset_zero": offset_zero,
                    "capture_policy": producer_stats["device"].get("capture_policy"),
                    "producer_capture_known": producer_stats["summary"].get("capture_known"),
                    "producer_capture_unknown": producer_stats["summary"].get("capture_unknown"),
                    "producer_flags_by_capture_presence": {
                        "capture_frames": producer_stats["frame_capture_known"],
                        "unknown_frames": producer_stats["frame_capture_unknown"],
                    },
                    "consumer_capture_counts": {
                        label: {
                            "capture_known": stats["summary"].get("capture_known"),
                            "capture_unknown": stats["summary"].get("capture_unknown"),
                        }
                        for label, stats in consumer_stats.items()
                    },
                },
            }
        )
        status = "PASS"
    except Exception as error:
        reason = str(error)
        print(f"RBT_TIME_HARDWARE {status} {reason}", file=sys.stderr, flush=True)
    finally:
        stop_and_reap(children)
        for child in hardware_children:
            try:
                child.enforce_bounds()
            except GateFailure as error:
                status = "FAIL"
                reason = reason or str(error)
            if status == "PASS" and child.metrics["warmup_baseline"] is None:
                status = "FAIL"
                reason = reason or f"{child.label} never reached its five-second resource warmup baseline"
        if status == "PASS" and producer.metrics["driver_reopen_resources_stable"] is not True:
            status = "FAIL"
            reason = reason or "producer reopen resource stability was not observed within post-warmup bounds"
        report["status"] = status
        report["reason"] = reason
        report["resources"] = {
            child.label: child.metrics for child in children
        }
        report["cleanup"] = {
            "all_children_reaped": all(child.exit_code is not None for child in children),
            "all_child_pids_absent": all(child.metrics["pid_absent_after_reap"] for child in children),
            "all_process_groups_empty": all(child.metrics["process_group_empty_after_reap"] for child in children),
        }
        if status == "PASS" and (
            not report["cleanup"]["all_children_reaped"]
            or not report["cleanup"]["all_child_pids_absent"]
            or not report["cleanup"]["all_process_groups_empty"]
        ):
            report["status"] = "FAIL"
            report["reason"] = "one or more gate child processes were not fully reaped"
        report["hardware_capture"] = report["status"]
        summary_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print(f"RBT_TIME_HARDWARE {report['status']} summary={summary_path}", flush=True)
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except GateFailure as error:
        print(f"RBT_TIME_HARDWARE NOTRUN {error}", file=sys.stderr, flush=True)
        sys.exit(2)

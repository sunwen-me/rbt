#!/usr/bin/env python3
"""Explicit-device physical-camera to static-TF acceptance gate.

The software self-test constructs only an in-memory format fixture. Hardware
is touched only by the explicit --device hardware mode.
"""

from __future__ import annotations

import argparse
from collections import deque
import json
import os
import signal
import socket
import stat
import subprocess
import sys
import time
import tomllib
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
MIN_DURATION = 300
WARMUP_SECONDS = 5.0
MAX_RSS_KIB = 512 * 1024
MAX_RSS_GROWTH_KIB = 32 * 1024
MAX_FDS = 128
MAX_FD_GROWTH = 4
MAX_THREADS = 64
MAX_THREAD_GROWTH = 4
MAX_RECORD_BYTES = 1_000_000


class GateFailure(RuntimeError):
    pass


class JsonlRecordReader:
    """Parse complete JSONL records while retaining one bounded partial tail."""

    def __init__(self, name: str):
        self.name = name
        self.partial = bytearray()

    def feed(self, data: bytes) -> list[dict[str, Any]]:
        records: list[dict[str, Any]] = []
        cursor = 0
        while cursor < len(data):
            newline = data.find(b"\n", cursor)
            if newline < 0:
                tail_size = len(self.partial) + len(data) - cursor
                if tail_size > MAX_RECORD_BYTES:
                    raise GateFailure(f"{self.name} produced an oversized JSONL record")
                self.partial.extend(data[cursor:])
                break

            segment_size = newline - cursor
            if len(self.partial) + segment_size > MAX_RECORD_BYTES:
                raise GateFailure(f"{self.name} produced an oversized JSONL record")
            if self.partial:
                self.partial.extend(data[cursor:newline])
                line = bytes(self.partial)
                self.partial.clear()
            else:
                line = data[cursor:newline]
            cursor = newline + 1
            if not line.strip():
                continue
            try:
                record = json.loads(line)
            except (json.JSONDecodeError, UnicodeDecodeError) as error:
                raise GateFailure(f"{self.name} produced malformed JSONL: {error}") from error
            if not isinstance(record, dict):
                raise GateFailure(f"{self.name} produced a non-object JSONL record")
            records.append(record)
        return records

    def finish(self) -> None:
        if self.partial:
            raise GateFailure(
                f"{self.name} ended with an incomplete JSONL record "
                f"({len(self.partial)} bytes without a newline)"
            )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", help="explicit absolute V4L2 character-device path")
    parser.add_argument("--config", type=Path, help="explicit static extrinsics TOML file")
    parser.add_argument("--duration", type=int, help="hardware test duration, at least 300 seconds")
    parser.add_argument("--frame-id", default="camera_optical", help="configured capture frame (default: camera_optical)")
    parser.add_argument("--log-dir", type=Path, help="output directory for commands and JSONL evidence")
    parser.add_argument("--self-test", choices=["softwareonly"], help="run software fixtures only; never access hardware")
    options = parser.parse_args()
    if options.self_test:
        if options.device is not None or options.config is not None or options.duration is not None:
            parser.error("--self-test softwareonly cannot be combined with hardware arguments")
        return options
    if options.device is None or options.config is None or options.duration is None:
        parser.error("hardware mode requires --device, --config, and --duration")
    if not Path(options.device).is_absolute():
        parser.error("--device must be an explicit absolute path")
    if not options.config.is_absolute():
        parser.error("--config must be an explicit absolute path")
    if options.duration < MIN_DURATION:
        parser.error(f"--duration must be at least {MIN_DURATION} seconds")
    return options


def validate_frame_name(name: str) -> bytes:
    encoded = name.encode("ascii")
    if not encoded or len(encoded) > 63:
        raise GateFailure("frame id must contain 1 to 63 ASCII bytes")
    segments = name.split("/")
    for segment in segments:
        if not segment or segment in {".", ".."}:
            raise GateFailure("frame id has an empty or reserved path segment")
        if not (segment[0].isalnum() or segment[0] == "_") or not segment[0].isascii():
            raise GateFailure("frame id segment must start with ASCII alphanumeric or underscore")
        if any(not char.isascii() or not (char.isalnum() or char in "_.-") for char in segment):
            raise GateFailure("frame id contains a non-canonical character")
    return encoded + bytes(64 - len(encoded))


def canonical_frame_is_valid(raw: bytes) -> bool:
    if len(raw) != 64:
        return False
    zero = raw.find(0)
    if zero <= 0 or any(raw[zero:]):
        return False
    try:
        return validate_frame_name(raw[:zero].decode("ascii")) == raw
    except (UnicodeDecodeError, GateFailure):
        return False


def software_self_test() -> None:
    frame_name = "camera_optical"
    canonical = validate_frame_name(frame_name)
    width, height, stride = 2, 2, 4
    flags = 0x0000_2000 | 0x0001_0000
    raw_capture_ns = 123_456_789
    pixels = bytes(range(stride * height))
    app = bytearray(112 + len(pixels))
    app[:8] = b"RBTVCAM2"
    app[8:12] = width.to_bytes(4, "little")
    app[12:16] = height.to_bytes(4, "little")
    app[16:20] = stride.to_bytes(4, "little")
    app[20:24] = int.from_bytes(b"YUYV", "little").to_bytes(4, "little")
    app[24:32] = raw_capture_ns.to_bytes(8, "little", signed=True)
    app[32:36] = flags.to_bytes(4, "little")
    app[48:112] = canonical
    app[112:] = pixels
    timing = bytearray(192)
    timing[:8] = b"RBTTIME1"
    timing[8] = 0b001
    timing[16] = 1
    source_id = bytes([0x42]) + bytes(47)
    timing[24:72] = source_id
    timing[72:80] = (3).to_bytes(8, "little")
    timing[80:88] = raw_capture_ns.to_bytes(8, "little", signed=True)
    timing[88] = 1
    timing[96:144] = source_id
    timing[144:152] = (3).to_bytes(8, "little")
    timing[152:160] = (raw_capture_ns + 5_000).to_bytes(8, "little", signed=True)
    sample = timing + app
    checksum = fnv1a64(pixels)
    if sample[:8] != b"RBTTIME1" or sample[192:200] != b"RBTVCAM2" or not canonical_frame_is_valid(sample[240:304]):
        raise GateFailure("RBTVCAM2 frame identity or pixel offset fixture failed")
    if int.from_bytes(sample[16 + 64:16 + 72], "little", signed=True) != raw_capture_ns:
        raise GateFailure("RBTTIME1 actual capture timestamp fixture failed")
    if int.from_bytes(sample[192 + 24:192 + 32], "little", signed=True) != raw_capture_ns:
        raise GateFailure("RBTVCAM2 raw driver timestamp fixture failed")
    if sample[8] != 0b001 or any(sample[9:16]) or any(sample[160:192]):
        raise GateFailure("RBTTIME1 unknown-precision and reserved-byte fixture failed")
    if sample[16] != 1 or sample[88] != 1 or sample[24:72] != sample[96:144]:
        raise GateFailure("RBTTIME1 capture and receive clock-identity fixture failed")
    if int.from_bytes(sample[152:160], "little", signed=True) < raw_capture_ns:
        raise GateFailure("RBTTIME1 software receive time precedes capture time")
    if flags & 0x0000_E000 != 0x0000_2000 or flags & 0x0007_0000 != 0x0001_0000:
        raise GateFailure("V4L2 MONOTONIC+SOE fixture failed")
    if checksum != fnv1a64(app[112:]):
        raise GateFailure("sample checksum fixture failed")
    invalid = bytearray(app)
    invalid[111] = 1
    frame_part = invalid[48:112]
    if canonical_frame_is_valid(frame_part):
        raise GateFailure("software fixture did not reject malformed frame padding")
    print("RBT_TF_CAMERA_SELF_TEST SOFTWARE_FIXTURE PASS hardware_access=none physical_camera=NOT_RUN")


def fnv1a64(payload: bytes) -> int:
    value = 14_695_981_039_346_656_037
    for byte in payload:
        value = ((value ^ byte) * 1_099_511_628_211) & 0xFFFF_FFFF_FFFF_FFFF
    return value


def check_config(path: Path, frame_id: str) -> None:
    if not path.is_file():
        raise GateFailure(f"explicit config is not a regular file: {path}")
    if path.stat().st_size > 1_048_576:
        raise GateFailure("static extrinsics config exceeds the bounded 1 MiB parser limit")
    try:
        document = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError) as error:
        raise GateFailure(f"cannot parse explicit static extrinsics config: {error}") from error
    transforms = document.get("transforms", {}).get("static")
    if not isinstance(transforms, list) or not transforms:
        raise GateFailure("config must contain a nonempty [[transforms.static]] array")
    parents: dict[str, str] = {}
    for edge in transforms:
        if not isinstance(edge, dict) or not isinstance(edge.get("parent"), str) or not isinstance(edge.get("child"), str):
            raise GateFailure("config has malformed static transform entries")
        if edge["child"] in parents:
            raise GateFailure(f"config has multiple parents for {edge['child']!r}")
        parents[edge["child"]] = edge["parent"]
    current = frame_id
    seen: set[str] = set()
    while current != "base_link" and current in parents:
        if current in seen:
            raise GateFailure("config contains a cycle on the base_link-to-capture-frame path")
        seen.add(current)
        current = parents[current]
    if current != "base_link":
        raise GateFailure(f"explicit config has no static path from base_link to {frame_id!r}")


def build_environment() -> tuple[dict[str, str], Path, Path, Path]:
    toolchain = Path(os.environ.get("RBT_PHASE4_TOOLCHAIN", ROOT / "target/phase4-toolchain")).resolve()
    prefix = Path(os.environ.get("RBT_IOX2_PREFIX", ROOT / "target/iox2-rbt-0.10.0-cpp-install")).resolve()
    cc = Path(os.environ.get("CC", toolchain / "bin/x86_64-conda-linux-gnu-gcc")).resolve()
    cxx = Path(os.environ.get("CXX", toolchain / "bin/x86_64-conda-linux-gnu-g++")).resolve()
    ar = Path(os.environ.get("AR", toolchain / "bin/x86_64-conda-linux-gnu-ar")).resolve()
    for name, path in (("pinned C compiler", cc), ("pinned C++ compiler", cxx), ("pinned archiver", ar)):
        if not path.is_file() or not os.access(path, os.X_OK):
            raise GateFailure(f"{name} is missing: {path}")
    cmake_config = prefix / "lib/cmake/iceoryx2-cxx/iceoryx2-cxxConfig.cmake"
    if not cmake_config.is_file():
        raise GateFailure(f"maintained iceoryx2 C++ prefix is missing: {cmake_config}")
    env = os.environ.copy()
    env.pop("PYTHONPATH", None)
    env["PATH"] = f"{toolchain / 'bin'}:{env.get('PATH', '')}"
    env["CC"] = str(cc)
    env["CXX"] = str(cxx)
    env["AR"] = str(ar)
    env["CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER"] = str(cc)
    env["CMAKE_PREFIX_PATH"] = f"{prefix}:{env.get('CMAKE_PREFIX_PATH', '')}".rstrip(":")
    for variable in ("LD_LIBRARY_PATH", "LIBRARY_PATH"):
        existing = env.get(variable, "")
        env[variable] = ":".join(item for item in (str(toolchain / "lib"), str(prefix / "lib"), existing) if item)
    return env, toolchain, prefix, cxx


class ManagedProcess:
    def __init__(self, name: str, command: list[str], env: dict[str, str], run_dir: Path):
        self.name = name
        self.command = command
        write_command(run_dir, name, command, env)
        self.out_path = run_dir / f"{name}.jsonl"
        self.err_path = run_dir / f"{name}.stderr.log"
        self.resources_path = run_dir / "resources.jsonl"
        self.stdout = self.out_path.open("wb")
        self.stderr = self.err_path.open("wb")
        self.process = subprocess.Popen(
            command,
            cwd=ROOT,
            env=env,
            stdout=self.stdout,
            stderr=self.stderr,
            start_new_session=True,
            close_fds=True,
        )
        self.started = time.monotonic()
        self.metrics: dict[str, Any] = {"pid": self.process.pid, "command": command, "samples": 0, "peak": {}}
        self.last_offset = 0
        self.record_reader = JsonlRecordReader(name)

    def read_records(self) -> list[dict[str, Any]]:
        self.stdout.flush()
        with self.out_path.open("rb") as source:
            source.seek(self.last_offset)
            data = source.read()
            self.last_offset += len(data)
        return self.record_reader.feed(data)

    def finish_records(self) -> list[dict[str, Any]]:
        """Drain final output and reject a partial line once the child has exited."""
        records = self.read_records()
        self.record_reader.finish()
        return records

    def sample_resources(self) -> None:
        metric = proc_metrics(self.process.pid)
        if metric is None:
            return
        with self.resources_path.open("a", encoding="utf-8") as samples:
            samples.write(json.dumps({
                "name": self.name,
                "pid": self.process.pid,
                "monotonic_elapsed_sec": time.monotonic() - self.started,
                **metric,
            }, separators=(",", ":")) + "\n")
        self.metrics["samples"] += 1
        self.metrics["final"] = metric
        for key, value in metric.items():
            self.metrics.setdefault("peak", {}).setdefault(key, value)
            self.metrics["peak"][key] = max(self.metrics["peak"][key], value)
        if time.monotonic() - self.started >= WARMUP_SECONDS:
            self.metrics.setdefault("warmup_baseline", metric)
            peak = self.metrics.setdefault("post_warmup_peak", {})
            for key, value in metric.items():
                peak[key] = max(peak.get(key, value), value)

    def close_logs(self) -> None:
        self.stdout.close()
        self.stderr.close()


def proc_metrics(pid: int) -> dict[str, int] | None:
    base = Path("/proc") / str(pid)
    try:
        status = (base / "status").read_text(encoding="ascii").splitlines()
        result: dict[str, int] = {}
        for line in status:
            if line.startswith("VmRSS:"):
                result["rss_kib"] = int(line.split()[1])
            elif line.startswith("Threads:"):
                result["threads"] = int(line.split()[1])
        result["fds"] = len(list((base / "fd").iterdir()))
        return result if len(result) == 3 else None
    except (OSError, ValueError, IndexError):
        return None


def write_command(run_dir: Path, label: str, command: list[str], env: dict[str, str] | None = None) -> None:
    row: dict[str, Any] = {"label": label, "argv": command}
    if env is not None:
        keys = (
            "RBT_COMPOSER_ADDR", "RBT_IOX2_PREFIX", "RBT_PHASE4_TOOLCHAIN", "CC", "CXX", "AR",
            "CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER", "CMAKE_PREFIX_PATH", "LD_LIBRARY_PATH",
            "LIBRARY_PATH", "PATH",
        )
        row["environment"] = {key: env[key] for key in keys if key in env}
    with (run_dir / "commands.jsonl").open("a", encoding="utf-8") as output:
        output.write(json.dumps(row, separators=(",", ":")) + "\n")


def run_logged(label: str, command: list[str], env: dict[str, str], run_dir: Path) -> None:
    write_command(run_dir, label, command, env)
    with (run_dir / f"{label}.log").open("wb") as output:
        completed = subprocess.run(command, cwd=ROOT, env=env, stdout=output, stderr=subprocess.STDOUT, check=False)
    if completed.returncode != 0:
        raise GateFailure(f"{label} failed with exit status {completed.returncode}; see {run_dir / f'{label}.log'}")


def wait_for_record(process: ManagedProcess, event_name: str, timeout: float) -> dict[str, Any]:
    deadline = time.monotonic() + timeout
    seen: list[dict[str, Any]] = []
    while time.monotonic() < deadline:
        if process.process.poll() is not None:
            raise GateFailure(f"{process.name} exited before {event_name}: {process.process.returncode}")
        records = process.read_records()
        seen.extend(records)
        for record in records:
            if record.get("event") == event_name:
                return record
        time.sleep(0.05)
    raise GateFailure(f"{process.name} did not emit {event_name} before timeout; observed={seen[-3:]}")


def wait_composer(address: tuple[str, int], timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(address, timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise GateFailure("Composer TCP endpoint did not become ready")


def terminate_process_group(child: ManagedProcess) -> dict[str, bool]:
    process = child.process
    pgid = process.pid
    if process.poll() is None:
        try:
            os.killpg(pgid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(pgid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.wait(timeout=3)
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        try:
            os.killpg(pgid, 0)
        except ProcessLookupError:
            break
        time.sleep(0.05)
    else:
        try:
            os.killpg(pgid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        kill_deadline = time.monotonic() + 2
        while time.monotonic() < kill_deadline:
            try:
                os.killpg(pgid, 0)
            except ProcessLookupError:
                break
            time.sleep(0.05)
    absent = not Path("/proc", str(process.pid)).exists()
    try:
        os.killpg(pgid, 0)
        empty = False
    except ProcessLookupError:
        empty = True
    return {"pid_absent_after_reap": absent, "process_group_empty_after_reap": empty}


def load_jsonl(path: Path) -> list[dict[str, Any]]:
    records: deque[dict[str, Any]] = deque(maxlen=65_536)
    with path.open("rb") as source:
        for line_no, line in enumerate(source, 1):
            if not line.endswith(b"\n"):
                if len(line) > MAX_RECORD_BYTES:
                    raise GateFailure(f"JSONL record over one megabyte: {path}:{line_no}")
                break
            record_line = line[:-1]
            if len(record_line) > MAX_RECORD_BYTES:
                raise GateFailure(f"JSONL record over one megabyte: {path}:{line_no}")
            try:
                value = json.loads(record_line)
            except (json.JSONDecodeError, UnicodeDecodeError) as error:
                raise GateFailure(f"malformed JSONL at {path}:{line_no}: {error}") from error
            if isinstance(value, dict):
                records.append(value)
            else:
                raise GateFailure(f"non-object JSONL at {path}:{line_no}")
    return list(records)


def verify_evidence(camera_records: list[dict[str, Any]], perception_records: list[dict[str, Any]], frame_id: str) -> dict[str, Any]:
    producer: dict[int, dict[str, Any]] = {}
    for row in camera_records:
        if row.get("event") == "frame_published":
            if row.get("frame_id") != frame_id or row.get("capture_present") is not True:
                continue
            sequence = int(row["sequence"])
            producer[sequence] = row
            if len(producer) > 4096:
                del producer[next(iter(producer))]
    received = [row for row in perception_records if row.get("event") == "tf_camera_frame"]
    lookups = [row for row in perception_records if row.get("event") == "tf_lookup"]
    by_sequence = {int(row.get("sequence", -1)): row for row in lookups if row.get("status") == "ok"}
    matches = []
    for row in received:
        sequence = int(row.get("sequence", -1))
        source = producer.get(sequence)
        lookup = by_sequence.get(sequence)
        if source is None or lookup is None:
            continue
        if row.get("sample_checksum_fnv1a64") != source.get("sample_checksum_fnv1a64"):
            continue
        if row.get("driver_session_generation") != source.get("driver_session_generation"):
            continue
        if row.get("capture_ns") != source.get("capture_ns") or row.get("capture_ns") != lookup.get("capture_ns"):
            continue
        if row.get("frame_id") != frame_id or lookup.get("requested_timestamp_preserved") is not True:
            continue
        if lookup.get("capture_uncertainty_ns") is not None or lookup.get("uncertainty_policy") != "best_effort_unknown_uncertainty":
            continue
        sources = lookup.get("sources")
        if not isinstance(sources, list) or not any(source.get("static_revision") == 1 for source in sources if isinstance(source, dict)):
            continue
        world = lookup.get("world_lookups")
        if not isinstance(world, dict) or any(world.get(name, {}).get("status") != "missing_explicitly" for name in ("odom", "map")):
            continue
        matches.append(sequence)
    if len(matches) < 3:
        raise GateFailure(f"only {len(matches)} bounded capture-time/checksum/late-join matches; need at least three")
    if any(row.get("borrowed_samples") != 0 for row in perception_records if row.get("event") == "camera_sample_released"):
        raise GateFailure("perception retained a StampedBytes borrow after processing")
    if not any(row.get("event") == "perception_ready" and row.get("static_tf_snapshot") == "subscribed_before_snapshot_request" for row in perception_records):
        raise GateFailure("perception did not report listener-before-snapshot startup ordering")
    if not all(row.get("physical_dynamic_tf") == "NOT_RUN" for row in perception_records if row.get("event") in {"perception_ready", "tf_lookup", "tf_camera_frame"}):
        raise GateFailure("physical dynamic TF must remain explicitly NOT_RUN")
    return {"matched_frames": len(matches), "matched_sequences": matches[:16], "producer_known_capture_frames": len(producer)}


def check_resources(processes: list[ManagedProcess], extra_resources: dict[int, dict[str, Any]]) -> dict[str, Any]:
    observations: dict[str, Any] = {}
    for process in processes:
        metrics = process.metrics
        baseline = metrics.get("warmup_baseline")
        peak = metrics.get("post_warmup_peak")
        if not baseline or not peak:
            raise GateFailure(f"no warmup resource samples for {process.name}")
        growth = {key: peak[key] - baseline[key] for key in ("rss_kib", "fds", "threads")}
        if peak["rss_kib"] > MAX_RSS_KIB or growth["rss_kib"] > MAX_RSS_GROWTH_KIB:
            raise GateFailure(f"{process.name} exceeded RSS bounds: peak={peak['rss_kib']} growth={growth['rss_kib']}")
        if peak["fds"] > MAX_FDS or growth["fds"] > MAX_FD_GROWTH:
            raise GateFailure(f"{process.name} exceeded FD bounds: peak={peak['fds']} growth={growth['fds']}")
        if peak["threads"] > MAX_THREADS or growth["threads"] > MAX_THREAD_GROWTH:
            raise GateFailure(f"{process.name} exceeded thread bounds: peak={peak['threads']} growth={growth['threads']}")
        observations[process.name] = {"baseline": baseline, "post_warmup_peak": peak, "growth": growth}
    for pid, metrics in extra_resources.items():
        baseline = metrics.get("warmup_baseline")
        peak = metrics.get("post_warmup_peak")
        if not baseline or not peak:
            raise GateFailure(f"no warmup resource samples for native camera child PID {pid}")
        growth = {key: peak[key] - baseline[key] for key in ("rss_kib", "fds", "threads")}
        if peak["rss_kib"] > MAX_RSS_KIB or growth["rss_kib"] > MAX_RSS_GROWTH_KIB:
            raise GateFailure(f"native camera child PID {pid} exceeded RSS bounds: {growth}")
        if peak["fds"] > MAX_FDS or growth["fds"] > MAX_FD_GROWTH:
            raise GateFailure(f"native camera child PID {pid} exceeded FD bounds: {growth}")
        if peak["threads"] > MAX_THREADS or growth["threads"] > MAX_THREAD_GROWTH:
            raise GateFailure(f"native camera child PID {pid} exceeded thread bounds: {growth}")
        observations[f"camera-child-{pid}"] = {"baseline": baseline, "post_warmup_peak": peak, "growth": growth}
    return observations


def hardware_run(options: argparse.Namespace) -> int:
    device = Path(options.device)
    if not device.exists() or not stat.S_ISCHR(device.stat().st_mode):
        raise GateFailure(f"explicit --device is not an accessible V4L2 character device: {device}")
    resolved_device = device.resolve(strict=True)
    if resolved_device.parent != Path("/dev") or not resolved_device.name.startswith("video") or not resolved_device.name[5:].isdigit():
        raise GateFailure(f"explicit --device does not resolve to a Linux V4L2 /dev/videoN node: {device}")
    if not Path("/sys/class/video4linux", resolved_device.name).exists():
        raise GateFailure(f"explicit device has no /sys/class/video4linux entry: {resolved_device.name}")
    check_config(options.config, options.frame_id)
    frame_bytes = validate_frame_name(options.frame_id)
    del frame_bytes
    env, toolchain, prefix, cxx = build_environment()
    run_root = options.log_dir or ROOT / "target/tf-camera-hardware-logs"
    run_root = run_root.resolve()
    run_root.mkdir(parents=True, exist_ok=True)
    run_dir = run_root / (datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8])
    run_dir.mkdir()
    env["RBT_IOX2_PREFIX"] = str(prefix)
    env["RBT_PHASE4_TOOLCHAIN"] = str(toolchain)
    env["CMAKE_PREFIX_PATH"] = str(prefix)
    cpp_build = ROOT / "target/tf-camera-cpp-build"
    camera_binary = cpp_build / "rbt_time_camera"
    cargo = ["cargo", "build", "-p", "composer", "-p", "rbt-tf-camera"]
    cmake_configure = [
        "cmake", "-S", str(ROOT / "sdk/cpp"), "-B", str(cpp_build), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_CXX_COMPILER={cxx}", f"-DCMAKE_PREFIX_PATH={prefix}",
    ]
    cmake_build = ["cmake", "--build", str(cpp_build), "--target", "rbt_time_camera"]
    processes: list[ManagedProcess] = []
    summary: dict[str, Any] = {
        "status": "NOTRUN",
        "gate": "physical_camera_to_static_tf",
        "device": str(device),
        "config": str(options.config),
        "frame_id": options.frame_id,
        "duration_sec": options.duration,
        "calibration_accuracy": "unverified",
        "physical_dynamic_tf": "NOT_RUN",
        "run_dir": str(run_dir),
        "commands": str(run_dir / "commands.jsonl"),
        "resource_samples": str(run_dir / "resources.jsonl"),
        "processes": {},
    }
    camera_child_pid: int | None = None
    camera_child_metrics: dict[int, dict[str, Any]] = {}
    status = 1
    try:
        run_logged("cargo-build", cargo, env, run_dir)
        run_logged("cmake-configure", cmake_configure, env, run_dir)
        run_logged("cmake-build-camera", cmake_build, env, run_dir)
        if not camera_binary.is_file():
            raise GateFailure(f"C++ camera target was not produced at {camera_binary}")
        address_port = 37000 + (os.getpid() % 20000)
        address = f"127.0.0.1:{address_port}"
        env["RBT_COMPOSER_ADDR"] = address
        composer = ManagedProcess("composer", [str(ROOT / "target/debug/composer")], env, run_dir)
        processes.append(composer)
        wait_composer(("127.0.0.1", address_port), 10)
        camera_args = [str(ROOT / "target/debug/rbt-tf-camera"), "camera", "--id", "physical-camera", "--device", str(device), "--camera-exe", str(camera_binary), "--frame-id", options.frame_id, "--duration-sec", str(options.duration + 60)]
        camera = ManagedProcess("camera-component", camera_args, env, run_dir)
        processes.append(camera)
        calibration_args = [str(ROOT / "target/debug/rbt-tf-camera"), "calibration", "--id", "camera-calibration", "--config", str(options.config)]
        calibration = ManagedProcess("calibration-component", calibration_args, env, run_dir)
        processes.append(calibration)
        camera_ready = wait_for_record(camera, "camera_ready", 15)
        calibration_ready = wait_for_record(calibration, "static_calibration_ready", 15)
        camera_child_pid = int(camera_ready["child_pid"])
        child_command = camera_ready.get("child_command")
        if not isinstance(child_command, list) or not all(isinstance(arg, str) for arg in child_command):
            raise GateFailure("camera component did not preserve the exact native camera command")
        write_command(run_dir, "native-camera-child", child_command)
        camera_child_metrics[camera_child_pid] = {"started": time.monotonic(), "samples": 0}
        if camera_ready.get("frame_id") != options.frame_id or calibration_ready.get("calibration") != "Configured extrinsic — calibration accuracy unverified":
            raise GateFailure("camera/calibration readiness metadata failed the explicit config contract")
        time.sleep(5)
        perception_args = [str(ROOT / "target/debug/rbt-tf-camera"), "perception", "--id", "tf-perception", "--frame-id", options.frame_id, "--duration-sec", str(options.duration)]
        perception = ManagedProcess("perception-component", perception_args, env, run_dir)
        processes.append(perception)
        perception_ready = wait_for_record(perception, "perception_ready", 15)
        if perception_ready.get("static_tf_snapshot") != "subscribed_before_snapshot_request":
            raise GateFailure("perception listener was not installed before its snapshot request")

        deadline = time.monotonic() + options.duration
        while time.monotonic() < deadline:
            for process in processes:
                process.sample_resources()
                if process.process.poll() is not None:
                    raise GateFailure(f"{process.name} exited during the requested capture window with {process.process.returncode}")
            if camera_child_pid is not None:
                metric = proc_metrics(camera_child_pid)
                if metric is None:
                    raise GateFailure("native camera child exited before the requested capture duration")
                child_metrics = camera_child_metrics[camera_child_pid]
                child_metrics["samples"] += 1
                with (run_dir / "resources.jsonl").open("a", encoding="utf-8") as samples:
                    samples.write(json.dumps({
                        "name": "native-camera-child",
                        "pid": camera_child_pid,
                        "monotonic_elapsed_sec": time.monotonic() - child_metrics["started"],
                        **metric,
                    }, separators=(",", ":")) + "\n")
                if time.monotonic() - child_metrics["started"] >= WARMUP_SECONDS:
                    child_metrics.setdefault("warmup_baseline", metric)
                    peak = child_metrics.setdefault("post_warmup_peak", {})
                    for key, value in metric.items():
                        peak[key] = max(peak.get(key, value), value)
            time.sleep(0.5)
        wait_for_record(perception, "perception_summary", 10)
        try:
            os.killpg(perception.process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            perception.process.wait(timeout=20)
        except subprocess.TimeoutExpired as error:
            raise GateFailure("perception did not stop after its requested capture duration") from error
        if perception.process.returncode != 0:
            raise GateFailure(f"perception exited with status {perception.process.returncode}")

        for process in processes:
            process.read_records()
            process.sample_resources()
        camera_records = load_jsonl(camera.out_path)
        perception_records = load_jsonl(perception.out_path)
        summary["evidence"] = verify_evidence(camera_records, perception_records, options.frame_id)
        perception_summary = next((row for row in perception_records if row.get("event") == "perception_summary"), None)
        if perception_summary is None or int(perception_summary.get("actual_elapsed_ms", 0)) < options.duration * 1000:
            raise GateFailure("perception summary did not cover the requested physical capture duration")
        summary["resources"] = check_resources(processes, camera_child_metrics)
        summary["late_join_delay_seconds"] = 5
        status = 0
        summary["status"] = "PASS"
        summary["hardware_capture"] = "PASS"
    finally:
        cleanup: dict[str, Any] = {}
        jsonl_failures: list[str] = []
        for process in reversed(processes):
            cleanup[process.name] = terminate_process_group(process)
            try:
                process.finish_records()
            except GateFailure as error:
                message = str(error)
                cleanup[process.name]["jsonl_finalization_error"] = message
                jsonl_failures.append(message)
            process.close_logs()
        if jsonl_failures:
            summary["jsonl_finalization_failures"] = jsonl_failures
            status = 1
        if camera_child_pid is not None:
            cleanup[f"camera-child-{camera_child_pid}"] = {"pid_absent_after_reap": not Path("/proc", str(camera_child_pid)).exists()}
        if status == 0 and any(
            not checks.get("pid_absent_after_reap", False)
            or not checks.get("process_group_empty_after_reap", True)
            for checks in cleanup.values()
        ):
            status = 1
            summary["cleanup_failure"] = True
        summary["cleanup"] = cleanup
        summary["status"] = summary.get("status", "FAIL") if status == 0 else "FAIL"
        summary["hardware_capture"] = "PASS" if status == 0 else "FAIL"
        summary["physical_dynamic_tf"] = "NOT_RUN"
        (run_dir / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(summary, sort_keys=True), flush=True)
    return status


def main() -> int:
    options = parse_args()
    if options.self_test:
        software_self_test()
        return 0
    try:
        return hardware_run(options)
    except (GateFailure, OSError, subprocess.SubprocessError) as error:
        print(f"RBT_TF_CAMERA_HARDWARE_GATE FAIL {error}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

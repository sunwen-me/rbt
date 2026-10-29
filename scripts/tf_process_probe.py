#!/usr/bin/env python3
"""Run real Composer/provider/consumer TF processes and retain their evidence."""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import math
import os
import signal
import shutil
import socket
import subprocess
import sys
import time
import traceback
from dataclasses import dataclass
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[1]
LOG_DIR = ROOT / "target" / "tf-process-logs"
TOOL = ROOT / "target" / "debug" / "rbt-tf-tools"
RSS_GROWTH_LIMIT = 32 * 1024 * 1024
RSS_LIMIT = 512 * 1024 * 1024
FD_GROWTH_LIMIT = 4
FD_LIMIT = 128
THREAD_GROWTH_LIMIT = 4
THREAD_LIMIT = 64
RESOURCE_WARMUP_SECONDS = 5
ACTIVE_RUN_DIR: Path | None = None


def pinned_env() -> dict[str, str]:
    env = os.environ.copy()
    toolchain = ROOT / "target" / "phase4-toolchain" / "bin"
    library = ROOT / "target" / "phase4-toolchain" / "lib"
    compiler = toolchain / "x86_64-conda-linux-gnu-gcc"
    env["PATH"] = f"{toolchain}:{env.get('PATH', '')}"
    env["CC"] = str(compiler)
    env["CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER"] = str(compiler)
    env["LD_LIBRARY_PATH"] = f"{library}:{env.get('LD_LIBRARY_PATH', '')}"
    return env


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def tool_source_hashes() -> dict[str, str]:
    paths = (
        Path("crates/rbt-tf-tools/src/main.rs"),
        Path("crates/rbt-tf-tools/Cargo.toml"),
        Path("Cargo.lock"),
    )
    return {str(path): sha256_file(ROOT / path) for path in paths}


def build_tool() -> None:
    subprocess.run(
        ["cargo", "build", "-p", "rbt-tf-tools"],
        cwd=ROOT,
        env=pinned_env(),
        check=True,
    )


def free_local_addr() -> str:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return f"127.0.0.1:{listener.getsockname()[1]}"


@dataclass
class Child:
    role: str
    process: subprocess.Popen[bytes]
    log_path: Path
    log_file: Any
    expected_exit: bool = False
    termination_record: dict[str, Any] | None = None

    def events(self) -> list[dict[str, Any]]:
        if not self.log_file.closed:
            self.log_file.flush()
        result: list[dict[str, Any]] = []
        try:
            lines = self.log_path.read_text(errors="replace").splitlines()
        except FileNotFoundError:
            return result
        for line in lines:
            try:
                value = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(value, dict):
                result.append(value)
        return result

    def terminate(
        self,
        sig: int,
        timeout: float = 5.0,
        reason: str = "cleanup",
    ) -> None:
        if self.process.poll() is not None:
            raise RuntimeError(f"{self.role} exited unexpectedly with status {self.process.returncode}")
        started = time.monotonic()
        forced_sigkill = False
        try:
            os.killpg(self.process.pid, sig)
        except ProcessLookupError:
            pass
        try:
            self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(self.process.pid, signal.SIGKILL)
            self.process.wait(timeout=timeout)
            forced_sigkill = True
        self.termination_record = {
            "role": self.role,
            "pid": self.process.pid,
            "requested_signal": signal.Signals(sig).name,
            "requested_signal_number": sig,
            "reason": reason,
            "timeout_seconds": timeout,
            "elapsed_seconds": round(time.monotonic() - started, 3),
            "exit_code": self.process.returncode,
            "forced_sigkill": forced_sigkill,
        }
        self.expected_exit = True
        self.log_file.close()

    def assert_alive(self) -> None:
        code = self.process.poll()
        if code is not None and not self.expected_exit:
            tail = "\n".join(self.log_path.read_text(errors="replace").splitlines()[-12:])
            raise RuntimeError(f"{self.role} exited unexpectedly ({code}); tail:\n{tail}")


class Probe:
    def __init__(self, consumers: int, mode: str):
        global ACTIVE_RUN_DIR
        self.consumers_count = consumers
        self.mode = mode
        self.addr = free_local_addr()
        self.children: list[Child] = []
        self.started_at = time.monotonic()
        self.baseline: dict[str, int] | None = None
        self.peak: dict[str, int] = {}
        self.last_per_process: dict[str, dict[str, int]] = {}
        self.per_process_baseline: dict[str, dict[str, int]] = {}
        self.per_process_peak: dict[str, dict[str, int]] = {}
        self.per_process_lifetime_peak: dict[str, dict[str, int]] = {}
        self.per_process_fd_baseline: dict[str, dict[str, str]] = {}
        self.per_process_fd_quiescent: dict[str, dict[str, str]] = {}
        self.per_process_fd_peak: dict[str, dict[str, str]] = {}
        self.per_process_fd_lifetime_peak: dict[str, dict[str, str]] = {}
        self.per_process_fd_final: dict[str, dict[str, str]] = {}
        self.per_process_final: dict[str, dict[str, int]] = {}
        self.per_process_quiescent: dict[str, dict[str, int]] = {}
        self.per_process_samples: dict[str, dict[str, list[int]]] = {}
        self.resource_windows: dict[str, dict[str, Any]] = {}
        self.per_process_fd_initial_min: dict[str, int] = {}
        self.per_process_fd_window_min: dict[str, dict[str, int]] = {}
        self.per_process_fd_window_max: dict[str, dict[str, int]] = {}
        self.per_process_fd_window_min_inventory: dict[str, dict[str, dict[str, str]]] = {}
        self.per_process_fd_window_max_inventory: dict[str, dict[str, dict[str, str]]] = {}
        self.per_process_warmup_seconds: dict[str, int] = {}
        self.baseline_warmup_seconds = 0
        self.provider_lead_seconds = 0.0
        self.composer_identities: dict[str, dict[str, Any]] | None = None
        self.startup_readiness: dict[str, Any] | None = None
        self.recovery_readiness: list[dict[str, Any]] = []
        self.final_live_snapshot: dict[str, Any] | None = None
        self.metric_samples = 0
        self.cleanup_errors: list[str] = []
        LOG_DIR.mkdir(parents=True, exist_ok=True)
        run_id = f"{int(time.time())}-{os.getpid()}-{mode}"
        self.run_dir = LOG_DIR / run_id
        self.run_dir.mkdir(parents=True, exist_ok=False)
        ACTIVE_RUN_DIR = self.run_dir
        self.commands_path = self.run_dir / "commands.jsonl"
        self.metadata_path = self.run_dir / "run-metadata.json"
        self.tool_binary = self.run_dir / "rbt-tf-tools-frozen"
        shutil.copy2(TOOL, self.tool_binary)
        self.source_snapshot_dir = self.run_dir / "source-snapshot"
        snapshot_paths = (
            Path(__file__),
            Path("crates/rbt-tf-tools/src/main.rs"),
            Path("crates/rbt-tf-tools/Cargo.toml"),
            Path("Cargo.lock"),
            Path("scripts/soak_tf.sh"),
            Path("scripts/benchmark_tf.sh"),
        )
        self.source_snapshot_dir.mkdir()
        source_snapshot_hashes = {}
        for source_path in snapshot_paths:
            relative = (
                Path("scripts/tf_process_probe.py")
                if source_path.resolve() == Path(__file__).resolve()
                else source_path
            )
            target = self.source_snapshot_dir / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, target)
            source_snapshot_hashes[str(relative)] = sha256_file(target)
        metadata = {
            "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "probe_argv": sys.argv,
            "probe_source": str(Path(__file__).resolve()),
            "probe_source_sha256": sha256_file(Path(__file__).resolve()),
            "tool_source_sha256": tool_source_hashes(),
            "tool_binary": str(TOOL),
            "tool_binary_frozen": str(self.tool_binary),
            "tool_binary_sha256": sha256_file(self.tool_binary),
            "source_snapshot_directory": str(self.source_snapshot_dir),
            "source_snapshot_sha256": source_snapshot_hashes,
            "python_version": sys.version,
        }
        self.metadata_path.write_text(
            json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )

    def launch(self, role: str, args: list[str]) -> Child:
        path = self.run_dir / f"{role}-{len(self.children) + 1}.jsonl"
        output = path.open("ab", buffering=0)
        process = subprocess.Popen(
            [str(self.tool_binary), *args],
            cwd=ROOT,
            env=pinned_env(),
            stdout=output,
            stderr=subprocess.STDOUT,
            start_new_session=True,
            close_fds=True,
        )
        with self.commands_path.open("a", encoding="utf-8") as commands:
            commands.write(json.dumps({
                "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                "role": role,
                "pid": process.pid,
                "process_group": process.pid,
                "argv": [str(self.tool_binary), *args],
            }, separators=(",", ":")) + "\n")
        child = Child(role, process, path, output)
        self.children.append(child)
        return child

    def start_composer(self) -> Child:
        child = self.launch("composer", ["composer", "--addr", self.addr])
        self.wait_for(lambda: can_connect(self.addr), 10.0, "Composer listen socket")
        return child

    def start_provider(self) -> Child:
        child = self.launch("provider", ["provider", "--addr", self.addr])
        self.wait_for(lambda: any(event.get("event") == "activation" for event in child.events()), 15.0, "provider activation")
        return child

    def start_consumer(self, index: int, slow_drain_ms: int = 0) -> Child:
        child = self.launch(
            f"consumer-{index}",
            [
                "consumer",
                "--addr",
                self.addr,
                "--id",
                f"tf-software-consumer-{index}",
                "--slow-drain-ms",
                str(slow_drain_ms),
            ],
        )
        self.wait_for(lambda: any(event.get("event") == "activation" for event in child.events()), 15.0, f"consumer {index} activation")
        return child

    def start_stack(self, slow_consumer_index: int | None = None) -> tuple[Child, Child, list[Child]]:
        composer = self.start_composer()
        provider = self.start_provider()
        provider_lead_started = time.monotonic()
        provider_lead_deadline = provider_lead_started + 5.0
        while time.monotonic() < provider_lead_deadline:
            provider.assert_alive()
            time.sleep(max(0.0, min(0.1, provider_lead_deadline - time.monotonic())))
        self.provider_lead_seconds = time.monotonic() - provider_lead_started
        consumers = [
            self.start_consumer(index, slow_drain_ms=50 if index == slow_consumer_index else 0)
            for index in range(self.consumers_count)
        ]
        self.composer_identities = wait_for_composer_components(self, expected=None, timeout=20.0)
        self.startup_readiness = wait_for_data_readiness(
            self,
            provider,
            consumers,
            self.composer_identities,
            timeout=20.0,
            what="initial TF stream, snapshot RPC, and correct lookup readiness",
        )
        self.capture_resource_window("startup_warmup", [composer, provider, *consumers])
        return composer, provider, consumers

    def wait_for(self, predicate, timeout: float, what: str) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for child in self.children:
                child.assert_alive()
            if self.baseline is not None:
                self.sample_resources()
            if predicate():
                return
            time.sleep(0.05)
        raise TimeoutError(f"timed out waiting for {what}")

    def sample_resources(self) -> dict[str, int]:
        rss = fds = threads = 0
        active = 0
        per_process: dict[str, dict[str, int]] = {}
        for child in self.children:
            if child.process.poll() is not None:
                continue
            active += 1
            values = proc_resources(child.process.pid)
            process_key = str(child.process.pid)
            per_process[process_key] = values
            rss += values["rss_bytes"]
            fds += values["fds"]
            threads += values["threads"]
            lifetime_peak = self.per_process_lifetime_peak.setdefault(process_key, dict(values))
            new_lifetime_fd_peak = values["fds"] > lifetime_peak["fds"]
            for key in ("rss_bytes", "fds", "threads"):
                lifetime_peak[key] = max(lifetime_peak[key], values[key])
            if process_key not in self.per_process_fd_lifetime_peak:
                self.per_process_fd_lifetime_peak[process_key] = proc_fd_inventory(child.process.pid)
            elif new_lifetime_fd_peak:
                self.per_process_fd_lifetime_peak[process_key] = proc_fd_inventory(child.process.pid)
            samples = self.per_process_samples.setdefault(
                process_key,
                {key: [] for key in ("rss_bytes", "fds", "threads")},
            )
            for key in ("rss_bytes", "fds", "threads"):
                samples[key].append(values[key])
        result = {"active_processes": active, "rss_bytes": rss, "fds": fds, "threads": threads}
        self.metric_samples += 1
        self.last_per_process = per_process
        if self.baseline is not None:
            for process_key, values in per_process.items():
                peak = self.per_process_peak.get(process_key)
                if peak is None:
                    continue
                new_fd_peak = values["fds"] > peak["fds"]
                for key in ("rss_bytes", "fds", "threads"):
                    peak[key] = max(peak[key], values[key])
                if new_fd_peak:
                    self.per_process_fd_peak[process_key] = proc_fd_inventory(int(process_key))
            for key in ("rss_bytes", "fds", "threads"):
                self.peak[key] = max(self.peak.get(key, result[key]), result[key])
        return result

    def record_process_quiescent(self, child: Child) -> None:
        """Record readiness metrics without changing the measured baseline."""
        if self.baseline is None or child.process.poll() is not None:
            return
        process_key = str(child.process.pid)
        values = proc_resources(child.process.pid)
        self.per_process_quiescent[process_key] = dict(values)
        self.per_process_fd_quiescent[process_key] = proc_fd_inventory(child.process.pid)

    def begin_measurement(self) -> dict[str, int]:
        if not self.startup_readiness or not self.startup_readiness.get("ready"):
            raise RuntimeError("initial TF data readiness was not proven before baseline")
        if "startup_warmup" not in self.resource_windows:
            raise RuntimeError("initial fixed warmup window was not captured before baseline")
        participants = [child for child in self.children if child.process.poll() is None]
        self.capture_resource_window("initial_baseline", participants)
        distribution = self.resource_windows["initial_baseline"]["distribution"]
        if not distribution:
            raise RuntimeError("no live processes were measured during the initial baseline window")
        initial_baseline: dict[str, dict[str, int]] = {}
        for process_key, metrics in distribution.items():
            initial_baseline[process_key] = {
                metric: values["max"] for metric, values in metrics.items()
            }
        self.per_process_baseline = initial_baseline
        self.per_process_peak = {
            process_key: dict(values) for process_key, values in initial_baseline.items()
        }
        self.per_process_fd_baseline = self.per_process_fd_window_max_inventory[
            "initial_baseline"
        ]
        self.per_process_fd_peak = {
            pid: dict(values) for pid, values in self.per_process_fd_baseline.items()
        }
        self.per_process_fd_initial_min.update(
            self.per_process_fd_window_min["initial_baseline"]
        )
        self.per_process_quiescent = {
            process_key: dict(self.last_per_process[process_key])
            for process_key in self.last_per_process
            if process_key in initial_baseline
        }
        self.per_process_fd_quiescent = {
            process_key: proc_fd_inventory(int(process_key))
            for process_key in self.per_process_quiescent
        }
        self.baseline_warmup_seconds = RESOURCE_WARMUP_SECONDS
        self.per_process_warmup_seconds = {
            process_key: RESOURCE_WARMUP_SECONDS for process_key in initial_baseline
        }
        self.baseline = {
            "active_processes": len(initial_baseline),
            **{
                key: sum(values[key] for values in initial_baseline.values())
                for key in ("rss_bytes", "fds", "threads")
            },
        }
        self.peak = {key: self.baseline[key] for key in ("rss_bytes", "fds", "threads")}
        return self.baseline

    def warmup_process(self, child: Child) -> None:
        process_key = str(child.process.pid)
        if process_key in self.per_process_baseline:
            raise RuntimeError(f"resource baseline already exists for process {process_key}")
        warmup_name = f"pid_warmup_{process_key}"
        baseline_name = f"pid_baseline_{process_key}"
        self.capture_resource_window(warmup_name, [child])
        baseline_window = self.capture_resource_window(baseline_name, [child])
        distribution = baseline_window["distribution"].get(process_key)
        if not distribution:
            raise RuntimeError(f"no baseline samples captured for process {process_key}")
        high_water = {
            metric: values["max"] for metric, values in distribution.items()
        }
        self.per_process_baseline[process_key] = high_water
        self.per_process_peak[process_key] = dict(high_water)
        self.per_process_fd_baseline[process_key] = self.per_process_fd_window_max_inventory[
            baseline_name
        ][process_key]
        self.per_process_fd_peak[process_key] = dict(
            self.per_process_fd_baseline[process_key]
        )
        self.per_process_fd_initial_min[process_key] = self.per_process_fd_window_min[
            baseline_name
        ][process_key]
        self.per_process_quiescent[process_key] = dict(self.last_per_process[process_key])
        self.per_process_fd_quiescent[process_key] = proc_fd_inventory(child.process.pid)
        self.per_process_warmup_seconds[process_key] = RESOURCE_WARMUP_SECONDS

    def capture_resource_window(
        self,
        name: str,
        participants: list[Child],
        duration: float = RESOURCE_WARMUP_SECONDS,
    ) -> dict[str, Any]:
        if name in self.resource_windows:
            raise RuntimeError(f"resource window {name!r} was already recorded")
        if duration <= 0:
            raise ValueError("resource window duration must be positive")
        expected = {str(child.process.pid): child for child in participants}
        if not expected:
            raise RuntimeError(f"resource window {name!r} has no participants")
        samples: dict[str, dict[str, list[int]]] = {
            process_key: {key: [] for key in ("rss_bytes", "fds", "threads")}
            for process_key in expected
        }
        minima: dict[str, int] = {}
        min_inventory: dict[str, dict[str, str]] = {}
        maxima: dict[str, int] = {}
        max_inventory: dict[str, dict[str, str]] = {}
        started = time.monotonic()
        deadline = started + duration
        next_sample = started
        while next_sample <= deadline + 1.0e-6:
            time.sleep(max(0.0, next_sample - time.monotonic()))
            for child in expected.values():
                if child.process.poll() is not None:
                    raise RuntimeError(
                        f"process {child.process.pid} was not alive throughout {name!r}"
                    )
                child.assert_alive()
            self.sample_resources()
            for process_key, child in expected.items():
                values = self.last_per_process.get(process_key)
                if values is None:
                    raise RuntimeError(
                        f"process {process_key} disappeared during resource window {name!r}"
                    )
                for metric in samples[process_key]:
                    samples[process_key][metric].append(values[metric])
                if process_key not in minima or values["fds"] < minima[process_key]:
                    minima[process_key] = values["fds"]
                    min_inventory[process_key] = proc_fd_inventory(child.process.pid)
                if process_key not in maxima or values["fds"] >= maxima[process_key]:
                    maxima[process_key] = values["fds"]
                    max_inventory[process_key] = proc_fd_inventory(child.process.pid)
            next_sample += 0.05
        captured = time.monotonic() - started
        if any(not values["fds"] for values in samples.values()):
            raise RuntimeError(f"resource window {name!r} captured no samples")
        self.per_process_fd_window_min[name] = minima
        self.per_process_fd_window_max[name] = maxima
        self.per_process_fd_window_min_inventory[name] = min_inventory
        self.per_process_fd_window_max_inventory[name] = max_inventory
        self.resource_windows[name] = {
            "duration_seconds": round(captured, 3),
            "sample_interval_seconds": 0.05,
            "expected_processes": {
                process_key: child.role for process_key, child in expected.items()
            },
            "participants_alive_entire_window": True,
            "distribution": process_resource_distributions(samples),
        }
        return self.resource_windows[name]

    def assert_limits(self) -> dict[str, Any]:
        if self.baseline is None:
            return {"measured": False}
        current = self.sample_resources()
        growth = {}
        for key in ("rss_bytes", "fds", "threads"):
            growth[key] = max(
                (
                    max(0, values[key] - self.per_process_baseline.get(process_key, values)[key])
                    for process_key, values in self.per_process_peak.items()
                ),
                default=0,
            )
        final_processes = (
            self.final_live_snapshot.get("processes", {})
            if self.final_live_snapshot
            else {}
        )
        retained_deltas: dict[str, dict[str, int]] = {}
        retained_window_names = ("post_recovery", "final_live")
        retained_window_complete = bool(final_processes)
        for process_key in final_processes:
            initial_min = self.per_process_fd_initial_min.get(process_key)
            if initial_min is None:
                retained_window_complete = False
                continue
            retained_deltas[process_key] = {}
            for window_name in retained_window_names:
                window_min = self.per_process_fd_window_min.get(window_name, {}).get(
                    process_key
                )
                if window_min is None:
                    retained_window_complete = False
                    continue
                retained_deltas[process_key][window_name] = max(0, window_min - initial_min)
        retained_fd_growth = max(
            (
                delta
                for windows in retained_deltas.values()
                for delta in windows.values()
            ),
            default=0,
        )
        absolute_peak = {
            key: max((values[key] for values in self.per_process_lifetime_peak.values()), default=0)
            for key in ("rss_bytes", "fds", "threads")
        }
        inventory_path = self.write_fd_inventory_snapshot()
        checks = {
            "rss_growth_le_32_mib": growth["rss_bytes"] <= RSS_GROWTH_LIMIT,
            "rss_peak_le_512_mib": absolute_peak["rss_bytes"] <= RSS_LIMIT,
            "fd_retained_growth_le_4": (
                retained_window_complete and retained_fd_growth <= FD_GROWTH_LIMIT
            ),
            "fd_peak_le_128": absolute_peak["fds"] <= FD_LIMIT,
            "thread_growth_le_4": growth["threads"] <= THREAD_GROWTH_LIMIT,
            "thread_peak_le_64": absolute_peak["threads"] <= THREAD_LIMIT,
            "all_processes_baselined": all(
                str(child.process.pid) in self.per_process_baseline
                for child in self.children
            ),
            "final_live_snapshot_captured": bool(
                self.final_live_snapshot
                and self.final_live_snapshot.get("processes")
            ),
        }
        return {
            "measured": True,
            "baseline": self.baseline,
            "peak": dict(self.peak),
            "per_process_baseline": self.per_process_baseline,
            "per_process_fd_initial_min": self.per_process_fd_initial_min,
            "baseline_warmup_seconds": self.baseline_warmup_seconds,
            "per_process_warmup_seconds": self.per_process_warmup_seconds,
            "per_process_quiescent": self.per_process_quiescent,
            "per_process_peak": self.per_process_peak,
            "per_process_lifetime_peak": self.per_process_lifetime_peak,
            "per_process_final": self.per_process_final,
            "per_process_distribution": process_resource_distributions(self.per_process_samples),
            "resource_windows": self.resource_windows,
            "per_process_fd_window_min": self.per_process_fd_window_min,
            "per_process_fd_window_max": self.per_process_fd_window_max,
            "per_process_fd_window_min_inventory": self.per_process_fd_window_min_inventory,
            "per_process_fd_window_max_inventory": self.per_process_fd_window_max_inventory,
            "retained_fd_growth": retained_fd_growth,
            "retained_fd_growth_by_process_and_window": retained_deltas,
            "retained_fd_window_complete": retained_window_complete,
            "peak_fd_growth_reported_not_gated": growth["fds"],
            "fd_metric_definition": {
                "peak_growth": "maximum sampled FD count minus each PID's initial five-second high-water baseline; reported only",
                "retained_growth": "maximum of post-recovery and final five-second window FD minima minus each live PID's immutable initial five-second minimum, floored at zero",
                "window_sample_interval_seconds": 0.05,
            },
            "final_live_snapshot": self.final_live_snapshot,
            "unbaselined_processes": {
                str(child.process.pid): child.role
                for child in self.children
                if str(child.process.pid) not in self.per_process_baseline
            },
            "absolute_peak_per_process": absolute_peak,
            "fd_inventory_file": str(inventory_path),
            "current": current,
            "growth": growth,
            "thresholds": {
                "rss_growth_bytes": RSS_GROWTH_LIMIT,
                "rss_absolute_bytes": RSS_LIMIT,
                "fd_growth": FD_GROWTH_LIMIT,
                "fd_absolute": FD_LIMIT,
                "thread_growth": THREAD_GROWTH_LIMIT,
                "thread_absolute": THREAD_LIMIT,
            },
            "checks": checks,
            "pass": all(checks.values()),
        }

    def cleanup(self) -> None:
        if self.final_live_snapshot is None:
            try:
                self.capture_live_snapshot()
            except Exception as error:  # cleanup must still stop every child
                self.cleanup_errors.append(f"final live snapshot: {error}")
        cleanup_order = sorted(
            reversed(self.children),
            key=lambda child: (
                0 if child.role.startswith("consumer-") else
                1 if child.role == "provider" else
                2 if child.role == "composer" else
                3
            ),
        )
        for child in cleanup_order:
            if child.process.poll() is None:
                try:
                    self.capture_final_fd_inventory(child)
                    child.terminate(signal.SIGTERM, timeout=6.0, reason="cleanup")
                    record = child.termination_record or {}
                    if record.get("forced_sigkill"):
                        self.cleanup_errors.append(
                            f"{child.role} required SIGKILL after SIGTERM timeout"
                        )
                    if record.get("exit_code") != 0:
                        self.cleanup_errors.append(
                            f"{child.role} exited with status {record.get('exit_code')} after SIGTERM"
                        )
                    if not any(
                        event.get("event") == "shutdown_complete"
                        and event.get("role") in (child.role, "consumer")
                        for event in child.events()
                    ):
                        self.cleanup_errors.append(
                            f"{child.role} did not emit shutdown_complete after SIGTERM"
                        )
                except Exception as error:  # retain cleanup evidence, still clean remaining children
                    self.cleanup_errors.append(f"{child.role}: {error}")
                    if child.process.poll() is None:
                        os.killpg(child.process.pid, signal.SIGKILL)
                        child.process.wait(timeout=5.0)
                        child.termination_record = {
                            "role": child.role,
                            "pid": child.process.pid,
                            "requested_signal": "SIGTERM",
                            "requested_signal_number": signal.SIGTERM,
                            "reason": "cleanup_sigkill_fallback",
                            "timeout_seconds": 6.0,
                            "elapsed_seconds": None,
                            "exit_code": child.process.returncode,
                            "forced_sigkill": True,
                        }
                        child.log_file.close()
            elif not child.log_file.closed:
                self.capture_final_fd_inventory(child)
                if not child.expected_exit:
                    self.cleanup_errors.append(
                        f"{child.role} exited unexpectedly with status {child.process.returncode}"
                    )
                    child.termination_record = {
                        "role": child.role,
                        "pid": child.process.pid,
                        "requested_signal": None,
                        "requested_signal_number": None,
                        "reason": "unexpected_exit",
                        "timeout_seconds": None,
                        "elapsed_seconds": 0.0,
                        "exit_code": child.process.returncode,
                        "forced_sigkill": False,
                    }
                child.log_file.close()
        self.write_fd_inventory_snapshot()

    def termination_reports(self) -> list[dict[str, Any]]:
        reports = []
        for child in self.children:
            record = child.termination_record or {
                "role": child.role,
                "pid": child.process.pid,
                "requested_signal": None,
                "requested_signal_number": None,
                "reason": "not_recorded",
                "timeout_seconds": None,
                "elapsed_seconds": None,
                "exit_code": child.process.poll(),
                "forced_sigkill": False,
            }
            shutdown_complete = any(
                event.get("event") == "shutdown_complete"
                and event.get("role") in (child.role, "consumer")
                for event in child.events()
            )
            reports.append({**record, "shutdown_complete_event": shutdown_complete})
        return reports

    def shutdown_assertions(self) -> dict[str, bool]:
        sigterm_reports = [
            report for report in self.termination_reports()
            if report["requested_signal"] == "SIGTERM"
        ]
        return {
            "sigterm_participants_exit_zero": bool(sigterm_reports)
            and all(report["exit_code"] == 0 for report in sigterm_reports),
            "sigterm_participants_avoid_forced_sigkill": bool(sigterm_reports)
            and all(not report["forced_sigkill"] for report in sigterm_reports),
            "sigterm_participants_report_shutdown_complete": bool(sigterm_reports)
            and all(report["shutdown_complete_event"] for report in sigterm_reports),
        }

    def write_fd_inventory_snapshot(self) -> Path:
        inventory_path = self.run_dir / "fd-inventory.json"
        inventory_snapshot = {
            "baseline": self.per_process_fd_baseline,
            "initial_min": self.per_process_fd_initial_min,
            "quiescent": self.per_process_fd_quiescent,
            "peak": self.per_process_fd_peak,
            "window_minima": self.per_process_fd_window_min,
            "window_maxima": self.per_process_fd_window_max,
            "window_minimum_symlinks": self.per_process_fd_window_min_inventory,
            "window_maximum_symlinks": self.per_process_fd_window_max_inventory,
            "lifetime_peak": self.per_process_fd_lifetime_peak,
            "final": self.per_process_fd_final,
            "final_live_snapshot": self.final_live_snapshot,
        }
        inventory_path.write_text(
            json.dumps(inventory_snapshot, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        return inventory_path

    def capture_live_snapshot(self) -> dict[str, Any]:
        aggregate = self.sample_resources()
        processes = {}
        for child in self.children:
            if child.process.poll() is not None:
                continue
            process_key = str(child.process.pid)
            resources = proc_resources(child.process.pid)
            descriptors = proc_fd_inventory(child.process.pid)
            processes[process_key] = {
                "role": child.role,
                "resources": resources,
                "fd_symlinks": descriptors,
            }
            self.per_process_final[process_key] = dict(resources)
            self.per_process_fd_final[process_key] = descriptors
        snapshot = {
            "captured_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
            "aggregate": aggregate,
            "processes": processes,
        }
        self.final_live_snapshot = snapshot
        self.write_fd_inventory_snapshot()
        return snapshot

    def capture_final_fd_inventory(self, child: Child) -> None:
        process_key = str(child.process.pid)
        if child.process.poll() is None:
            values = proc_resources(child.process.pid)
            inventory = proc_fd_inventory(child.process.pid)
            self.per_process_fd_final[process_key] = inventory
            self.per_process_final[process_key] = dict(values)
            lifetime_peak = self.per_process_lifetime_peak.setdefault(process_key, dict(values))
            new_lifetime_fd_peak = values["fds"] > lifetime_peak["fds"]
            for key in ("rss_bytes", "fds", "threads"):
                lifetime_peak[key] = max(lifetime_peak[key], values[key])
            if new_lifetime_fd_peak or process_key not in self.per_process_fd_lifetime_peak:
                self.per_process_fd_lifetime_peak[process_key] = dict(inventory)
            measured_peak = self.per_process_peak.get(process_key)
            if measured_peak is not None:
                new_fd_peak = values["fds"] > measured_peak["fds"]
                for key in ("rss_bytes", "fds", "threads"):
                    measured_peak[key] = max(measured_peak[key], values[key])
                if new_fd_peak:
                    self.per_process_fd_peak[process_key] = dict(inventory)
            samples = self.per_process_samples.setdefault(
                process_key,
                {key: [] for key in ("rss_bytes", "fds", "threads")},
            )
            for key in ("rss_bytes", "fds", "threads"):
                samples[key].append(values[key])
        else:
            self.per_process_fd_final.setdefault(
                process_key,
                dict(self.per_process_fd_peak.get(process_key, {})),
            )
            self.per_process_final.setdefault(
                process_key,
                dict(self.per_process_lifetime_peak.get(process_key, {})),
            )

    def terminate_child(self, child: Child, sig: int) -> None:
        self.capture_final_fd_inventory(child)
        child.terminate(sig, reason="fault_injection")

    def orphan_pids(self) -> list[int]:
        result = []
        for child in self.children:
            try:
                os.killpg(child.process.pid, 0)
            except ProcessLookupError:
                continue
            result.append(child.process.pid)
        return result


def can_connect(addr: str) -> bool:
    host, port_text = addr.rsplit(":", 1)
    try:
        with socket.create_connection((host, int(port_text)), timeout=0.15):
            return True
    except OSError:
        return False


def inspect_composer(addr: str) -> dict[str, Any]:
    host, port_text = addr.rsplit(":", 1)
    request = {
        "type": "inspect",
        "control_protocol_version": 2,
        "snapshot_version": 4,
    }
    with socket.create_connection((host, int(port_text)), timeout=0.5) as connection:
        connection.settimeout(0.5)
        connection.sendall(json.dumps(request, separators=(",", ":")).encode() + b"\n")
        with connection.makefile("rb") as response_stream:
            response_line = response_stream.readline()
    if not response_line:
        raise RuntimeError("Composer inspection returned no response")
    response = json.loads(response_line)
    if response.get("type") != "inspect_response" or not isinstance(response.get("view"), dict):
        raise RuntimeError(f"unexpected Composer inspection response: {response}")
    return response["view"]


def _json_identity(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def _component_identity(component: dict[str, Any]) -> dict[str, Any]:
    component_id = component.get("component_id")
    snapshot = component.get("snapshot")
    if not isinstance(snapshot, dict) or snapshot.get("component_id") != component_id:
        raise ValueError(f"component {component_id!r} has no matching current snapshot")
    if (
        component.get("snapshot_received") is not True
        or component.get("control_connection") != "connected"
        or component.get("pending") is not None
        or snapshot.get("state") != "Active"
    ):
        raise ValueError(f"component {component_id!r} is not active and connected")
    process_instance = snapshot.get("process_instance")
    activation = snapshot.get("activation")
    last_active = component.get("last_active")
    if not process_instance or not isinstance(activation, dict) or not isinstance(last_active, dict):
        raise ValueError(f"component {component_id!r} has no complete activation identity")
    activation_id = activation.get("activation_id")
    if (
        activation_id is None
        or last_active.get("process_instance") != process_instance
        or last_active.get("activation_id") != activation_id
    ):
        raise ValueError(f"component {component_id!r} has mismatched live activation identity")

    provided = {}
    for capability in snapshot.get("provided_capabilities", []):
        capability_type = capability.get("capability_type")
        capability_id = capability.get("capability_instance")
        descriptor = capability.get("descriptor", {})
        if not capability_type or capability_id is None or descriptor.get("capability_instance") != capability_id:
            raise ValueError(f"component {component_id!r} has malformed capability identity")
        provided[capability_type] = capability_id
    activation_capabilities = {
        capability.get("capability_type"): capability.get("capability_instance")
        for capability in last_active.get("capabilities", [])
    }
    if provided != activation_capabilities:
        raise ValueError(f"component {component_id!r} snapshot and active capability IDs disagree")

    bindings = {}
    for binding in activation.get("bindings", []):
        descriptor = binding.get("descriptor", {})
        bindings[binding.get("requirement_name")] = {
            "provider": binding.get("provider"),
            "capability_type": binding.get("capability_type"),
            "capability_instance": descriptor.get("capability_instance"),
        }
    active_bindings = {}
    for binding in last_active.get("bindings", []):
        descriptor = binding.get("descriptor", {})
        active_bindings[binding.get("requirement_name")] = {
            "provider": binding.get("provider"),
            "capability_type": binding.get("capability_type"),
            "capability_instance": descriptor.get("capability_instance"),
        }
    if bindings != active_bindings:
        raise ValueError(f"component {component_id!r} snapshot and active bindings disagree")
    return {
        "process_instance": process_instance,
        "activation_id": activation_id,
        "provided_capabilities": provided,
        "bindings": bindings,
    }


def wait_for_composer_components(
    probe: Probe,
    expected: dict[str, dict[str, Any]] | None,
    timeout: float,
) -> dict[str, dict[str, Any]]:
    required_ids = ["tf-software-provider"] + [
        f"tf-software-consumer-{index}" for index in range(probe.consumers_count)
    ]
    deadline = time.monotonic() + timeout
    last_error = "no inspection response"
    while time.monotonic() < deadline:
        for child in probe.children:
            child.assert_alive()
        try:
            view = inspect_composer(probe.addr)
            if view.get("phase") != "running":
                raise ValueError(f"Composer phase is {view.get('phase')!r}")
            components = {
                component.get("component_id"): component
                for component in view.get("components", [])
                if isinstance(component, dict)
            }
            if any(component_id not in components for component_id in required_ids):
                missing = [component_id for component_id in required_ids if component_id not in components]
                raise ValueError(f"Composer has not observed required components {missing}")
            identities = {
                component_id: _component_identity(components[component_id])
                for component_id in required_ids
            }
            provider_caps = identities["tf-software-provider"]["provided_capabilities"]
            expected_provider_roles = {"TfStaticUpdate", "TfStaticSnapshot", "TfDynamic"}
            if set(provider_caps) != expected_provider_roles:
                raise ValueError("provider inspection does not contain all three TF capabilities")
            for index in range(probe.consumers_count):
                consumer_id = f"tf-software-consumer-{index}"
                bindings = identities[consumer_id]["bindings"]
                expected_bindings = {
                    "tf_static_updates": "TfStaticUpdate",
                    "tf_static_snapshot": "TfStaticSnapshot",
                    "tf_dynamic": "TfDynamic",
                }
                if set(bindings) != set(expected_bindings):
                    raise ValueError(f"consumer {consumer_id!r} does not have all TF bindings")
                for requirement, capability_type in expected_bindings.items():
                    binding = bindings[requirement]
                    if (
                        binding["provider"] != "tf-software-provider"
                        or binding["capability_type"] != capability_type
                        or binding["capability_instance"] != provider_caps[capability_type]
                    ):
                        raise ValueError(f"consumer {consumer_id!r} has a stale {requirement} binding")
            if expected is not None and identities != expected:
                raise ValueError("Composer recovery changed a process, activation, or capability identity")
            for child in probe.children:
                probe.record_process_quiescent(child)
            return identities
        except (OSError, ValueError, RuntimeError, json.JSONDecodeError) as error:
            last_error = f"{type(error).__name__}: {error}"
            if probe.baseline is not None:
                probe.sample_resources()
            time.sleep(0.1)
    raise TimeoutError(f"Composer did not recover its connected TF topology: {last_error}")


def proc_resources(pid: int) -> dict[str, int]:
    status = Path(f"/proc/{pid}/status").read_text()
    fields = {}
    for line in status.splitlines():
        if line.startswith("VmRSS:"):
            fields["rss_bytes"] = int(line.split()[1]) * 1024
        elif line.startswith("Threads:"):
            fields["threads"] = int(line.split()[1])
    fields["fds"] = len(list(Path(f"/proc/{pid}/fd").iterdir()))
    return {"rss_bytes": fields.get("rss_bytes", 0), "fds": fields["fds"], "threads": fields.get("threads", 0)}


def proc_fd_inventory(pid: int) -> dict[str, str]:
    fd_dir = Path(f"/proc/{pid}/fd")
    inventory = {}
    try:
        entries = list(fd_dir.iterdir())
    except FileNotFoundError:
        return inventory
    for fd_path in sorted(entries, key=lambda path: int(path.name)):
        try:
            inventory[fd_path.name] = os.readlink(fd_path)
        except OSError as error:
            inventory[fd_path.name] = f"<unavailable: {error.strerror or type(error).__name__}>"
    return inventory


def process_resource_distributions(samples: dict[str, dict[str, list[int]]]) -> dict[str, Any]:
    result = {}
    for process_key, metrics in samples.items():
        result[process_key] = {}
        for metric, values in metrics.items():
            ordered = sorted(values)
            if not ordered:
                continue
            result[process_key][metric] = {
                "samples": len(ordered),
                "min": ordered[0],
                "p50": ordered[math.ceil(0.50 * len(ordered)) - 1],
                "p90": ordered[math.ceil(0.90 * len(ordered)) - 1],
                "p99": ordered[math.ceil(0.99 * len(ordered)) - 1],
                "max": ordered[-1],
            }
    return result


def activation_events(child: Child) -> list[dict[str, Any]]:
    return [event for event in child.events() if event.get("event") == "activation"]


def deactivation_events(child: Child) -> list[dict[str, Any]]:
    return [event for event in child.events() if event.get("event") == "deactivation"]


def lifecycle_counts(children: list[Child]) -> dict[int, dict[str, int]]:
    return {
        id(child): {
            "activations": len(activation_events(child)),
            "deactivations": len(deactivation_events(child)),
        }
        for child in children
    }


def consumer_totals(children: list[Child]) -> dict[str, int]:
    attempts = successes = verified = errors = 0
    for child in children:
        events = [event for event in child.events() if event.get("event") == "consumer_summary"]
        if events:
            event = events[-1]
            attempts += int(event.get("lookup_attempts", 0))
            successes += int(event.get("lookup_successes", 0))
            verified += int(event.get("correct_results", 0))
            errors += int(event.get("lookup_errors", 0))
    return {"lookup_attempts": attempts, "lookup_successes": successes, "correct_results": verified, "lookup_errors": errors}


def consumer_correct_counts(children: list[Child]) -> list[int]:
    counts = []
    for child in children:
        events = [event for event in child.events() if event.get("event") == "consumer_summary"]
        counts.append(int(events[-1].get("correct_results", 0)) if events else 0)
    return counts


def provider_ids(child: Child) -> list[str]:
    return [values["dynamic_id"] for values in provider_capabilities(child)]


def provider_capabilities(child: Child) -> list[dict[str, str]]:
    capabilities = []
    for event in activation_events(child):
        values = {
            "static_update_id": str(event["static_update_id"]),
            "snapshot_id": str(event["static_snapshot_id"]),
            "dynamic_id": str(event["dynamic_id"]),
        } if all(
            event.get(key) is not None
            for key in ("static_update_id", "static_snapshot_id", "dynamic_id")
        ) else {}
        if len(values) == 3:
            capabilities.append(values)
    return capabilities


def consumer_summary_events(child: Child) -> list[dict[str, Any]]:
    return [event for event in child.events() if event.get("event") == "consumer_summary"]


def data_readiness_snapshot(
    provider: Child,
    consumers: list[Child],
    identities: dict[str, dict[str, Any]],
    minimum_correct: dict[int, int] | None = None,
) -> dict[str, Any]:
    provider_identity = identities["tf-software-provider"]
    snapshot_id = provider_identity["provided_capabilities"]["TfStaticSnapshot"]
    provider_requests = 0
    for event in provider.events():
        if event.get("event") != "provider_summary":
            continue
        pressure = event.get("transport_pressure", {})
        rpc_snapshots = pressure.get("rpc", []) if isinstance(pressure, dict) else []
        for snapshot in rpc_snapshots:
            if (
                isinstance(snapshot, dict)
                and snapshot.get("capability_instance") == snapshot_id
                and isinstance(snapshot.get("server"), dict)
            ):
                provider_requests = max(
                    provider_requests,
                    int(snapshot["server"].get("requests_received", 0)),
                )

    consumer_evidence: dict[str, Any] = {}
    consumers_ready = True
    for index, child in enumerate(consumers):
        component_id = f"tf-software-consumer-{index}"
        identity = identities[component_id]
        activation_id = identity["activation_id"]
        relevant = [
            event
            for event in consumer_summary_events(child)
            if event.get("activation_id") == activation_id
        ]
        event = relevant[-1] if relevant else {}
        pressure = event.get("transport_pressure", {})
        stream_values = pressure.get("streams", []) if isinstance(pressure, dict) else []
        rpc_values = pressure.get("rpc", []) if isinstance(pressure, dict) else []
        streams = {
            snapshot.get("service_name"): snapshot
            for snapshot in stream_values
            if isinstance(snapshot, dict)
        }
        rpc = {
            snapshot.get("service_name"): snapshot
            for snapshot in rpc_values
            if isinstance(snapshot, dict)
        }
        dynamic_id = identity["bindings"]["tf_dynamic"]["capability_instance"]
        update_id = identity["bindings"]["tf_static_updates"]["capability_instance"]
        snapshot_id_for_consumer = identity["bindings"]["tf_static_snapshot"]["capability_instance"]
        dynamic_service = f"rbt/cap/{dynamic_id}/stream"
        update_service = f"rbt/cap/{update_id}/stream"
        snapshot_service = f"rbt/cap/{snapshot_id_for_consumer}/rpc"
        dynamic_subscriber = streams.get(dynamic_service, {}).get("subscriber")
        update_subscriber = streams.get(update_service, {}).get("subscriber")
        rpc_client = rpc.get(snapshot_service, {}).get("client")
        dynamic_received = int(
            dynamic_subscriber.get("received_samples", 0)
            if isinstance(dynamic_subscriber, dict)
            else 0
        )
        update_received = int(
            update_subscriber.get("received_samples", 0)
            if isinstance(update_subscriber, dict)
            else 0
        )
        rpc_completed = int(
            rpc_client.get("calls_completed", 0) if isinstance(rpc_client, dict) else 0
        )
        correct_results = int(event.get("correct_results", 0))
        required_correct = (minimum_correct or {}).get(id(child), 0)
        slow = event.get("slow_native_subscriber", {})
        if not isinstance(slow, dict):
            slow = {}
        slow_stream = slow.get("stream", {}) if isinstance(slow, dict) else {}
        slow_subscriber = slow_stream.get("subscriber") if isinstance(slow_stream, dict) else None
        slow_ready = not bool(slow.get("enabled")) or (
            isinstance(slow_subscriber, dict)
            and int(slow_subscriber.get("received_samples", 0)) > 0
        )
        ready = (
            correct_results > required_correct
            and dynamic_received > 0
            and update_received > 0
            and rpc_completed > 0
            and slow_ready
        )
        consumers_ready = consumers_ready and ready
        consumer_evidence[component_id] = {
            "activation_id": activation_id,
            "correct_results": correct_results,
            "required_correct_results_greater_than": required_correct,
            "dynamic_stream_received_samples": dynamic_received,
            "static_update_stream_received_samples": update_received,
            "snapshot_rpc_calls_completed": rpc_completed,
            "slow_native_subscriber_received_samples": (
                int(slow_subscriber.get("received_samples", 0))
                if isinstance(slow_subscriber, dict)
                else None
            ),
            "ready": ready,
        }
    return {
        "provider_snapshot_rpc_requests_received": provider_requests,
        "provider_snapshot_rpc_requests_required": len(consumers),
        "provider_snapshot_rpc_ready": provider_requests >= len(consumers),
        "consumers": consumer_evidence,
        "consumers_ready": consumers_ready,
        "ready": consumers_ready and provider_requests >= len(consumers),
    }


def wait_for_data_readiness(
    probe: Probe,
    provider: Child,
    consumers: list[Child],
    identities: dict[str, dict[str, Any]],
    minimum_correct: dict[int, int] | None = None,
    timeout: float = 20.0,
    what: str = "TF data readiness",
) -> dict[str, Any]:
    latest: dict[str, Any] = {}

    def ready() -> bool:
        nonlocal latest
        latest = data_readiness_snapshot(
            provider,
            consumers,
            identities,
            minimum_correct=minimum_correct,
        )
        return bool(latest["ready"])

    probe.wait_for(ready, timeout, what)
    return latest


def consumer_correct_count_for_activation(child: Child, activation_id: Any) -> int:
    events = [
        event for event in consumer_summary_events(child)
        if event.get("activation_id") == activation_id
    ]
    return int(events[-1].get("correct_results", 0)) if events else 0


def buffer_history_bounds(buffer: Any) -> tuple[bool, int, int]:
    if not isinstance(buffer, dict):
        return False, 0, 0
    total = int(buffer.get("stored_samples", 0))
    edges = [edge for edge in buffer.get("graph", []) if isinstance(edge, dict)]
    largest_edge = max((int(edge.get("stored_samples", 0)) for edge in edges), default=0)
    valid = total <= 8192 and all(int(edge.get("stored_samples", 0)) <= 256 for edge in edges)
    return valid, total, largest_edge


def native_transport_retirement_assertion(children: list[Child]) -> dict[str, Any]:
    activations: dict[tuple[int, int], dict[str, Any]] = {}
    summaries: list[tuple[int, dict[str, Any]]] = []
    deactivations: list[tuple[int, int, int]] = []
    retirements: list[tuple[int, dict[str, Any]]] = []
    errors: list[str] = []

    def valid_activation_id(value: Any) -> bool:
        return type(value) is int and value > 0

    def valid_capability_ids(value: Any) -> bool:
        if not isinstance(value, dict) or set(value) != {
            "dynamic", "static_update", "static_snapshot"
        }:
            return False
        identifiers = [value.get(name) for name in ("dynamic", "static_update", "static_snapshot")]
        return all(isinstance(identifier, str) and identifier for identifier in identifiers) and len(
            set(identifiers)
        ) == len(identifiers)

    for child in children:
        if not child.role.startswith("consumer-"):
            continue
        pid = child.process.pid
        for index, event in enumerate(child.events()):
            kind = event.get("event")
            if kind == "activation":
                activation_id = event.get("activation_id")
                if (
                    event.get("role") != "consumer"
                    or not valid_activation_id(activation_id)
                    or not valid_capability_ids(
                        {
                            "dynamic": event.get("dynamic_physical_id"),
                            "static_update": event.get("static_physical_id"),
                            "static_snapshot": event.get("static_snapshot_physical_id"),
                        }
                    )
                ):
                    errors.append(f"pid {pid} has malformed consumer activation identity")
                    continue
                key = (pid, activation_id)
                if key in activations:
                    errors.append(f"pid {pid} activation {activation_id} is duplicated")
                    continue
                activations[key] = {
                    "index": index,
                    "capability_ids": {
                        "dynamic": event["dynamic_physical_id"],
                        "static_update": event["static_physical_id"],
                        "static_snapshot": event["static_snapshot_physical_id"],
                    },
                }
            elif kind == "consumer_summary":
                summaries.append((pid, event))
            elif kind == "deactivation" and event.get("role") == "consumer":
                activation_id = event.get("activation_id")
                if valid_activation_id(activation_id):
                    deactivations.append((pid, activation_id, index))
            elif kind == "activation_transport_retired":
                retirements.append((pid, {"index": index, "event": event}))

    endpoint_evidence: dict[tuple[int, int], dict[str, bool]] = {}
    for key, activation in activations.items():
        pid, activation_id = key
        capability_ids = activation["capability_ids"]
        endpoint_seen = {"dynamic": False, "static_update": False, "static_snapshot": False}
        dynamic_service = f"rbt/cap/{capability_ids['dynamic']}/stream"
        update_service = f"rbt/cap/{capability_ids['static_update']}/stream"
        snapshot_service = f"rbt/cap/{capability_ids['static_snapshot']}/rpc"
        for summary_pid, event in summaries:
            if summary_pid != pid or event.get("activation_id") != activation_id:
                continue
            pressure = event.get("transport_pressure")
            if not isinstance(pressure, dict) or pressure.get("capability_ids") != capability_ids:
                errors.append(f"pid {pid} activation {activation_id} has mismatched summary capability IDs")
                continue
            for snapshot in pressure.get("streams", []):
                if not isinstance(snapshot, dict):
                    continue
                service_name = snapshot.get("service_name")
                subscriber = snapshot.get("subscriber")
                if not isinstance(subscriber, dict):
                    continue
                received = subscriber.get("received_samples")
                if type(received) is not int or received <= 0:
                    continue
                if service_name == dynamic_service:
                    endpoint_seen["dynamic"] = True
                elif service_name == update_service:
                    endpoint_seen["static_update"] = True
            for snapshot in pressure.get("rpc", []):
                if (
                    isinstance(snapshot, dict)
                    and snapshot.get("service_name") == snapshot_service
                    and snapshot.get("capability_instance") == capability_ids["static_snapshot"]
                    and isinstance(snapshot.get("client"), dict)
                ):
                    completed = snapshot["client"].get("calls_completed")
                    if type(completed) is int and completed > 0:
                        endpoint_seen["static_snapshot"] = True
        endpoint_evidence[key] = endpoint_seen
        for endpoint, observed in endpoint_seen.items():
            if not observed:
                errors.append(
                    f"pid {pid} activation {activation_id} has no active {endpoint} data evidence"
                )

    retirement_counts: dict[tuple[int, int], int] = {}
    valid_retirements = 0
    for pid, record in retirements:
        event = record["event"]
        activation_id = event.get("activation_id")
        pressure = event.get("transport_pressure")
        ids = pressure.get("capability_ids") if isinstance(pressure, dict) else None
        streams = pressure.get("streams") if isinstance(pressure, dict) else None
        rpc = pressure.get("rpc") if isinstance(pressure, dict) else None
        valid = (
            event.get("role") == "consumer"
            and valid_activation_id(activation_id)
            and valid_capability_ids(ids)
            and isinstance(streams, list)
            and not streams
            and isinstance(rpc, list)
            and not rpc
            and event.get("all_sources_released") is True
        )
        if not valid:
            errors.append(f"pid {pid} has malformed or nonempty transport retirement evidence")
            continue
        valid_retirements += 1
        key = (pid, activation_id)
        retirement_counts[key] = retirement_counts.get(key, 0) + 1
        expected = activations.get(key)
        if expected is not None and ids != expected["capability_ids"]:
            errors.append(f"pid {pid} activation {activation_id} retired mismatched capability IDs")

    for key, activation in activations.items():
        pid, activation_id = key
        count = retirement_counts.get(key, 0)
        if count != 1:
            errors.append(
                f"pid {pid} activation {activation_id} has {count} retirement events; expected one"
            )
            continue
        ended = [
            index
            for deactivation_pid, ended_id, index in deactivations
            if deactivation_pid == pid and ended_id == activation_id
        ]
        retirement_index = next(
            record["index"]
            for retirement_pid, record in retirements
            if retirement_pid == pid
            and record["event"].get("activation_id") == activation_id
        )
        if not ended or retirement_index <= max(ended):
            errors.append(f"pid {pid} activation {activation_id} retired before its deactivation")

    for key, count in retirement_counts.items():
        if count > 1:
            errors.append(f"pid {key[0]} activation {key[1]} has duplicate retirement events")

    return {
        "pass": bool(activations) and not errors,
        "successful_activations": len(activations),
        "retirement_events": len(retirements),
        "valid_retirement_events": valid_retirements,
        "active_endpoint_evidence": [
            {"pid": pid, "activation_id": activation_id, **endpoint_evidence[(pid, activation_id)]}
            for pid, activation_id in activations
        ],
        "errors": errors,
    }


def native_stream_guard_bounded(snapshot: Any) -> bool:
    if not isinstance(snapshot, dict):
        return False
    subscriber = snapshot.get("subscriber")
    resources = snapshot.get("resources")
    semantic = snapshot.get("semantic")
    if not isinstance(subscriber, dict) or not isinstance(resources, dict) or not isinstance(semantic, dict):
        return False
    maximum = resources.get("max_borrowed")
    current = subscriber.get("borrowed_samples")
    high_water = subscriber.get("borrowed_high_water")
    failures = subscriber.get("borrow_failures")
    return (
        str(snapshot.get("profile", "")).lower() == "control"
        and semantic.get("queue_depth") == 4
        and type(maximum) is int
        and maximum > 0
        and type(current) is int
        and 0 <= current <= maximum
        and type(high_water) is int
        and 0 <= high_water <= maximum
        and type(failures) is int
        and failures == 0
    )


def native_publisher_stream_snapshot_valid(snapshot: Any) -> bool:
    if not isinstance(snapshot, dict) or "subscriber" not in snapshot or snapshot["subscriber"] is not None:
        return False
    publisher = snapshot.get("publisher")
    if not isinstance(publisher, dict) or any(
        type(publisher.get(field)) is not int or publisher[field] < 0
        for field in ("send_attempts", "send_successes", "queue_full")
    ):
        return False
    resources = snapshot.get("resources")
    semantic = snapshot.get("semantic")
    maximum = resources.get("max_borrowed") if isinstance(resources, dict) else None
    return (
        str(snapshot.get("profile", "")).lower() == "control"
        and isinstance(semantic, dict)
        and semantic.get("queue_depth") == 4
        and type(maximum) is int
        and maximum > 0
    )


def pressure_observations(children: list[Child]) -> dict[str, Any]:
    stream_snapshots: list[dict[str, Any]] = []
    rpc_snapshots: list[dict[str, Any]] = []
    pre_join_rpc_snapshots: list[dict[str, Any]] = []
    slow_snapshots: list[dict[str, Any]] = []
    buffers: list[Any] = []
    provider_queue_full_by_process: dict[int, int] = {}
    provider_queue_full_after_subscribe = False
    provider_publisher_samples_with_receivers = 0
    static_updates_by_process: dict[int, int] = {}
    consumer_receipts: dict[str, set[str]] = {}
    for child in children:
        if not child.role.startswith("consumer-"):
            continue
        for event in child.events():
            if event.get("event") != "consumer_summary":
                continue
            pressure = event.get("transport_pressure", {})
            stream_values = pressure.get("streams", []) if isinstance(pressure, dict) else []
            for snapshot in stream_values:
                if not isinstance(snapshot, dict):
                    continue
                subscriber = snapshot.get("subscriber")
                if isinstance(subscriber, dict) and int(subscriber.get("received_samples", 0)) > 0:
                    consumer_receipts.setdefault(snapshot.get("service_name", ""), set()).add(
                        child.role
                    )
    for child in children:
        for event in child.events():
            if event.get("event") == "provider_summary":
                static_updates_by_process[child.process.pid] = max(
                    static_updates_by_process.get(child.process.pid, 0),
                    int(event.get("static_updates", 0)),
                )
                pressure = event.get("transport_pressure", {})
                capability_ids = pressure.get("capability_ids", {}) if isinstance(pressure, dict) else {}
                dynamic_id = capability_ids.get("dynamic") if isinstance(capability_ids, dict) else None
                dynamic_service = f"rbt/cap/{dynamic_id}/stream" if dynamic_id else None
                publishers = [
                    snapshot.get("publisher")
                    for snapshot in pressure.get("streams", [])
                    if isinstance(snapshot, dict)
                    and snapshot.get("service_name") == dynamic_service
                    and str(snapshot.get("profile", "")).lower() == "control"
                    and int(snapshot.get("semantic", {}).get("queue_depth", 0)) == 4
                    and isinstance(snapshot.get("publisher"), dict)
                ] if isinstance(pressure, dict) else []
                for publisher in publishers:
                    receivers = consumer_receipts.get(dynamic_service or "", set())
                    if len(receivers) >= 2:
                        provider_publisher_samples_with_receivers += 1
                        current_queue_full = max(
                            int(publisher.get("queue_full", 0)),
                            int(event.get("dynamic_queue_full", 0)),
                        )
                        provider_queue_full_by_process[child.process.pid] = max(
                            provider_queue_full_by_process.get(child.process.pid, 0),
                            current_queue_full,
                        )
                        if current_queue_full > 0 and int(publisher.get("send_attempts", 0)) > 0:
                            provider_queue_full_after_subscribe = True
            elif event.get("event") in ("consumer_summary", "deactivation"):
                pressure = event.get("transport_pressure", {})
                if event.get("event") == "deactivation" and isinstance(pressure, dict):
                    pre_join_rpc_snapshots.extend(
                        snapshot for snapshot in pressure.get("rpc", [])
                        if isinstance(snapshot, dict)
                    )
                buffer = event.get("buffer")
                if buffer is not None:
                    buffers.append(buffer)
                slow = event.get("slow_native_subscriber")
                if isinstance(slow, dict) and slow.get("enabled"):
                    slow_snapshots.append(slow)
                    if slow.get("buffer") is not None:
                        buffers.append(slow["buffer"])
            else:
                continue
            if not isinstance(pressure, dict):
                continue
            stream_snapshots.extend(
                snapshot for snapshot in pressure.get("streams", [])
                if isinstance(snapshot, dict)
            )
            rpc_snapshots.extend(
                snapshot for snapshot in pressure.get("rpc", [])
                if isinstance(snapshot, dict)
            )
    return {
        "streams": stream_snapshots,
        "rpc": rpc_snapshots,
        "pre_join_rpc": pre_join_rpc_snapshots,
        "slow_subscribers": slow_snapshots,
        "buffers": buffers,
        "max_provider_queue_full": max(provider_queue_full_by_process.values(), default=0),
        "provider_queue_full_by_process": provider_queue_full_by_process,
        "provider_queue_full_after_subscribe": provider_queue_full_after_subscribe,
        "provider_publisher_samples_with_two_receiving_consumers": provider_publisher_samples_with_receivers,
        "consumer_dynamic_receipts_by_service": {
            service: sorted(roles) for service, roles in consumer_receipts.items()
        },
        "static_updates_observed": sum(static_updates_by_process.values()),
        "native_transport_retirement": native_transport_retirement_assertion(children),
    }


def pressure_assertions(observations: dict[str, Any]) -> dict[str, Any]:
    stream_bounds = []
    for snapshot in observations["streams"]:
        if (
            isinstance(snapshot, dict)
            and "subscriber" in snapshot
            and snapshot["subscriber"] is None
            and isinstance(snapshot.get("publisher"), dict)
        ):
            stream_bounds.append(native_publisher_stream_snapshot_valid(snapshot))
        else:
            stream_bounds.append(native_stream_guard_bounded(snapshot))
    rpc_client_bounds = []
    rpc_server_bounds = []
    for snapshot in observations["rpc"]:
        resources = snapshot.get("resources", {})
        client = snapshot.get("client")
        if isinstance(client, dict):
            max_in_flight = int(resources.get("max_in_flight", 0))
            max_guards = int(resources.get("max_borrowed_responses", 0))
            rpc_client_bounds.append(
                int(client.get("in_flight_current", max_in_flight + 1)) <= max_in_flight
                and int(client.get("in_flight_high_water", max_in_flight + 1)) <= max_in_flight
                and int(client.get("response_guards_current", max_guards + 1)) <= max_guards
                and int(client.get("response_guards_high_water", max_guards + 1)) <= max_guards
            )
        server = snapshot.get("server")
        if isinstance(server, dict):
            rpc_server_bounds.append(
                int(server.get("requests_current", 0)) <= int(resources.get("max_clients", 0))
            )
    rpc_settled = []
    for snapshot in observations["pre_join_rpc"]:
        client = snapshot.get("client")
        if isinstance(client, dict):
            rpc_settled.append(
                int(client.get("in_flight_current", 1)) == 0
                and int(client.get("response_guards_current", 1)) == 0
            )
        server = snapshot.get("server")
        if isinstance(server, dict):
            rpc_settled.append(int(server.get("requests_current", 1)) == 0)
    history_checks = [buffer_history_bounds(buffer) for buffer in observations["buffers"]]
    history_valid = bool(history_checks) and all(check[0] for check in history_checks)
    slow = observations["slow_subscribers"]
    slow_valid = bool(slow) and all(
        int(snapshot.get("drain_delay_ms", 0)) >= 25
        and int(snapshot.get("rejected_samples", 1)) == 0
        and native_stream_guard_bounded(snapshot.get("stream"))
        for snapshot in slow
    ) and any(
        int(snapshot.get("received_samples", 0)) > 0
        and int(snapshot.get("accepted_samples", 0)) > 0
        and int(snapshot["stream"]["subscriber"].get("received_samples", 0)) > 0
        for snapshot in slow
    )
    return {
        "history_within_256_per_edge_and_8192_global": history_valid,
        "history_max_stored_samples": max((check[1] for check in history_checks), default=0),
        "history_max_edge_samples": max((check[2] for check in history_checks), default=0),
        "native_stream_guards_bounded": bool(stream_bounds) and all(stream_bounds),
        "native_rpc_pending_and_guards_bounded": bool(rpc_client_bounds) and all(rpc_client_bounds),
        "native_rpc_server_requests_bounded": bool(rpc_server_bounds) and all(rpc_server_bounds),
        "native_rpc_zero_at_pre_join_observation": bool(rpc_settled) and all(rpc_settled),
        "native_transport_retired_after_join": observations["native_transport_retirement"]["pass"],
        "native_transport_retirement_evidence": observations["native_transport_retirement"],
        "actual_slow_control_subscriber": slow_valid,
        "provider_observed_control_queue_pressure": observations["provider_queue_full_after_subscribe"],
        "provider_queue_full_count": observations["max_provider_queue_full"],
        "provider_queue_full_after_subscribe": observations["provider_queue_full_after_subscribe"],
        "provider_publisher_samples_with_two_receiving_consumers": observations[
            "provider_publisher_samples_with_two_receiving_consumers"
        ],
        "static_updates_observed": observations["static_updates_observed"],
        "pressure_stream_samples": len(stream_bounds),
        "pressure_rpc_samples": len(rpc_client_bounds) + len(rpc_server_bounds),
    }


def buffer_static_resync(event: dict[str, Any]) -> dict[str, int] | None:
    buffer = event.get("buffer")
    if not isinstance(buffer, dict):
        return None
    providers = [provider for provider in buffer.get("providers", []) if isinstance(provider, dict)]
    recovered = [
        provider for provider in providers
        if int(provider.get("active_static_edges", 0)) > 0
        and int(provider.get("resync_attempts", 0)) > 0
        and provider.get("snapshot_fresh") is True
        and provider.get("static_stale") is False
    ]
    if not recovered:
        return None
    return {
        "resync_attempts": max(int(provider.get("resync_attempts", 0)) for provider in recovered),
        "resync_failures": sum(int(provider.get("resync_failures", 0)) for provider in recovered),
    }


def replace_provider(
    probe: Probe,
    provider: Child,
    consumers: list[Child],
    resource_warmup: bool = False,
) -> Child:
    before_capabilities = provider_capabilities(provider)
    if len(before_capabilities) != 1:
        raise RuntimeError("active provider process did not advertise exactly one capability set")
    before_lifecycle = lifecycle_counts(consumers)
    probe.terminate_child(provider, signal.SIGKILL)
    replacement = probe.start_provider()
    replacement_caps = provider_capabilities(replacement)
    if len(replacement_caps) != 1:
        raise RuntimeError("replacement provider did not advertise exactly one capability set")
    target_capabilities = replacement_caps[-1]
    if any(
        target_capabilities[role] == before_capabilities[-1][role]
        for role in ("static_update_id", "snapshot_id", "dynamic_id")
    ):
        raise RuntimeError("provider replacement reused a physical capability ID")
    target_id = target_capabilities["dynamic_id"]
    activation_ids = {}
    for index, consumer in enumerate(consumers):
        probe.wait_for(
            lambda consumer=consumer: any(
                event.get("event") == "activation"
                and event.get("dynamic_physical_id") == target_id
                and event.get("static_physical_id") == target_capabilities["static_update_id"]
                for event in activation_events(consumer)
            ),
            20.0,
            f"consumer {index} rebinding to replacement provider",
        )
        current_activations = activation_events(consumer)
        active = next(
            event for event in reversed(current_activations)
            if event.get("dynamic_physical_id") == target_id
        )
        activation_ids[id(consumer)] = active["activation_id"]
        after = lifecycle_counts([consumer])[id(consumer)]
        before = before_lifecycle[id(consumer)]
        if (
            after["activations"] != before["activations"] + 1
            or after["deactivations"] != before["deactivations"] + 1
        ):
            raise RuntimeError(
                f"consumer {index} lifecycle did not advance exactly once for provider replacement"
            )
    probe.composer_identities = wait_for_composer_components(
        probe,
        expected=None,
        timeout=20.0,
    )
    correct_before_inspection = {
        id(consumer): consumer_correct_count_for_activation(
            consumer, activation_ids[id(consumer)]
        )
        for consumer in consumers
    }
    readiness = wait_for_data_readiness(
        probe,
        replacement,
        consumers,
        probe.composer_identities,
        minimum_correct=correct_before_inspection,
        timeout=20.0,
        what="replacement provider stream, snapshot RPC, and correct lookup readiness",
    )
    probe.recovery_readiness.append({
        "kind": "provider_replacement",
        "provider_pid": replacement.process.pid,
        "readiness": readiness,
    })
    if resource_warmup:
        probe.warmup_process(replacement)
    correct_before = [
        consumer_correct_count_for_activation(consumer, activation_ids[id(consumer)])
        for consumer in consumers
    ]
    probe.wait_for(
        lambda: all(
            consumer_correct_count_for_activation(consumer, activation_ids[id(consumer)]) > previous
            for consumer, previous in zip(consumers, correct_before)
        ),
        10.0,
        "fresh correct local lookups after inspected provider recovery and warmup",
    )
    probe.wait_for(
        lambda: all(
            any(
                event.get("activation_id") == activation_ids[id(consumer)]
                and buffer_static_resync(event) is not None
                for event in consumer_summary_events(consumer)
            )
            for consumer in consumers
        ),
        10.0,
        "fresh static snapshot resynchronization after provider replacement",
    )
    if len(activation_events(replacement)) != 1 or deactivation_events(replacement):
        raise RuntimeError("replacement provider lifecycle changed before shutdown")
    return replacement


def restart_composer(
    probe: Probe,
    composer: Child,
    provider: Child,
    consumers: list[Child],
) -> Child:
    expected_identities = probe.composer_identities
    if expected_identities is None:
        raise RuntimeError("initial connected Composer identities were not captured")
    probe.terminate_child(composer, signal.SIGKILL)
    replacement = probe.start_composer()
    probe.composer_identities = wait_for_composer_components(
        probe,
        expected=expected_identities,
        timeout=20.0,
    )
    correct_before_inspection = consumer_correct_counts(consumers)
    readiness = wait_for_data_readiness(
        probe,
        provider,
        consumers,
        probe.composer_identities,
        minimum_correct={
            id(consumer): count
            for consumer, count in zip(consumers, correct_before_inspection)
        },
        timeout=20.0,
        what="fresh correct lookup and native provider data after Composer inspection",
    )
    probe.recovery_readiness.append({
        "kind": "composer_restart",
        "composer_pid": replacement.process.pid,
        "readiness": readiness,
    })
    probe.warmup_process(replacement)
    # Only count lookups made after inspection proved that Composer has
    # reconnected every component to the same active physical identities.
    correct_before = consumer_correct_counts(consumers)
    probe.wait_for(
        lambda: all(
            current > previous
            for current, previous in zip(consumer_correct_counts(consumers), correct_before)
        ),
        10.0,
        "fresh correct local lookups after verified Composer recovery",
    )
    return replacement


def restart_overlapping_outage(
    probe: Probe,
    composer: Child,
    provider: Child,
    consumers: list[Child],
) -> tuple[Child, Child, dict[str, Any]]:
    old_identities = probe.composer_identities
    old_capabilities = provider_capabilities(provider)
    if old_identities is None or len(old_capabilities) != 1:
        raise RuntimeError("overlap outage requires an inspected live Composer and provider")
    old_lifecycle = lifecycle_counts(consumers)

    probe.terminate_child(composer, signal.SIGKILL)
    probe.terminate_child(provider, signal.SIGKILL)
    if composer.process.poll() is None or provider.process.poll() is None:
        raise RuntimeError("overlap outage did not stop Composer and provider before restart")

    replacement_composer = probe.start_composer()
    replacement_provider = probe.start_provider()
    replacement_capabilities = provider_capabilities(replacement_provider)
    if len(replacement_capabilities) != 1:
        raise RuntimeError("overlap outage replacement provider did not advertise one capability set")
    new_caps = replacement_capabilities[-1]
    if any(new_caps[role] == old_capabilities[-1][role] for role in new_caps):
        raise RuntimeError("overlap outage replacement reused a physical provider capability ID")

    activation_ids: dict[int, Any] = {}
    for index, consumer in enumerate(consumers):
        probe.wait_for(
            lambda consumer=consumer: any(
                event.get("event") == "activation"
                and event.get("dynamic_physical_id") == new_caps["dynamic_id"]
                and event.get("static_physical_id") == new_caps["static_update_id"]
                for event in activation_events(consumer)
            ),
            20.0,
            f"consumer {index} rebinding after overlapping Composer/provider outage",
        )
        active = next(
            event for event in reversed(activation_events(consumer))
            if event.get("dynamic_physical_id") == new_caps["dynamic_id"]
        )
        activation_ids[id(consumer)] = active["activation_id"]
        counts = lifecycle_counts([consumer])[id(consumer)]
        before = old_lifecycle[id(consumer)]
        if counts != {
            "activations": before["activations"] + 1,
            "deactivations": before["deactivations"] + 1,
        }:
            raise RuntimeError(f"consumer {index} lifecycle did not advance once during overlap outage")

    identities = wait_for_composer_components(probe, expected=None, timeout=20.0)
    old_provider_identity = old_identities["tf-software-provider"]
    new_provider_identity = identities["tf-software-provider"]
    if new_provider_identity["provided_capabilities"] == old_provider_identity["provided_capabilities"]:
        raise RuntimeError("Composer inspection retained old provider capabilities after overlap outage")
    consumers_inspected = {}
    for index in range(probe.consumers_count):
        component_id = f"tf-software-consumer-{index}"
        identity = identities[component_id]
        old_identity = old_identities[component_id]
        if identity["process_instance"] != old_identity["process_instance"]:
            raise RuntimeError(f"consumer {component_id!r} process changed during overlap outage")
        if identity["activation_id"] == old_identity["activation_id"]:
            raise RuntimeError(f"consumer {component_id!r} activation did not change after provider loss")
        bindings = identity["bindings"]
        if any(
            bindings[requirement]["capability_instance"] != new_provider_identity["provided_capabilities"][capability]
            for requirement, capability in (
                ("tf_static_updates", "TfStaticUpdate"),
                ("tf_static_snapshot", "TfStaticSnapshot"),
                ("tf_dynamic", "TfDynamic"),
            )
        ):
            raise RuntimeError(f"consumer {component_id!r} did not bind to all fresh provider capabilities")
        consumers_inspected[component_id] = identity

    correct_before_inspection = {
        id(consumer): consumer_correct_count_for_activation(
            consumer, activation_ids[id(consumer)]
        )
        for consumer in consumers
    }
    readiness = wait_for_data_readiness(
        probe,
        replacement_provider,
        consumers,
        identities,
        minimum_correct=correct_before_inspection,
        timeout=20.0,
        what="fresh provider streams, snapshot RPC, and correct lookup after overlapping outage",
    )
    probe.recovery_readiness.append({
        "kind": "overlapping_composer_provider_outage",
        "composer_pid": replacement_composer.process.pid,
        "provider_pid": replacement_provider.process.pid,
        "readiness": readiness,
    })
    probe.warmup_process(replacement_composer)
    probe.warmup_process(replacement_provider)
    correct_before = [
        consumer_correct_count_for_activation(consumer, activation_ids[id(consumer)])
        for consumer in consumers
    ]
    probe.wait_for(
        lambda: all(
            consumer_correct_count_for_activation(consumer, activation_ids[id(consumer)]) > previous
            for consumer, previous in zip(consumers, correct_before)
        ),
        10.0,
        "fresh correct local lookups after inspected overlapping outage recovery and warmup",
    )

    def all_consumers_resynced() -> bool:
        for consumer in consumers:
            events = [
                event for event in consumer_summary_events(consumer)
                if event.get("activation_id") == activation_ids[id(consumer)]
            ]
            if not events or buffer_static_resync(events[-1]) is None:
                return False
        return True

    probe.wait_for(
        all_consumers_resynced,
        10.0,
        "fresh static snapshot resynchronization after native provider outage",
    )
    return replacement_composer, replacement_provider, {
        "composer_and_provider_both_stopped_before_either_restart": True,
        "old_provider_capabilities": old_capabilities[-1],
        "new_provider_capabilities": new_caps,
        "inspected_consumer_identities": consumers_inspected,
        "native_data_readiness": readiness,
        "fresh_correct_lookups_after_warmup": True,
        "fresh_native_static_resync": True,
    }


def run_replacement_cycles(cycles: int, consumers_count: int) -> dict[str, Any]:
    probe = Probe(consumers_count, "replacement")
    composer = provider = None
    consumers: list[Child] = []
    completed = 0
    ids_seen: set[str] = set()
    capabilities_seen = {role: set() for role in ("static_update_id", "snapshot_id", "dynamic_id")}
    processes_alive_before_shutdown = False
    lifecycle_before_shutdown: dict[int, dict[str, int]] = {}
    start = time.monotonic()
    try:
        composer, provider, consumers = probe.start_stack()
        for initial in provider_capabilities(provider):
            for role, value in initial.items():
                capabilities_seen[role].add(value)
            ids_seen.add(initial["dynamic_id"])
        probe.begin_measurement()
        for _ in range(cycles):
            provider = replace_provider(
                probe,
                provider,
                consumers,
                resource_warmup=True,
            )
            for capabilities in provider_capabilities(provider):
                for role, value in capabilities.items():
                    capabilities_seen[role].add(value)
                ids_seen.add(capabilities["dynamic_id"])
            completed += 1
        resource_participants = [composer, provider, *consumers]
        probe.capture_resource_window("post_recovery", resource_participants)
        probe.capture_resource_window("final_live", resource_participants)
        probe.capture_live_snapshot()
        lifecycle_before_shutdown = lifecycle_counts([provider, *consumers])
        processes_alive_before_shutdown = (
            composer is not None
            and composer.process.poll() is None
            and provider.process.poll() is None
            and all(child.process.poll() is None for child in consumers)
        )
    finally:
        probe.cleanup()
    orphan = probe.orphan_pids()
    resources = probe.assert_limits()
    totals = consumer_totals(consumers)
    shutdown_checks = probe.shutdown_assertions()
    lifecycle_exact = (
        bool(lifecycle_before_shutdown)
        and lifecycle_before_shutdown[id(provider)] == {"activations": 1, "deactivations": 0}
        and all(
            counts == {"activations": cycles + 1, "deactivations": cycles}
            for child in consumers
            if (counts := lifecycle_before_shutdown.get(id(child))) is not None
        )
        and len(lifecycle_before_shutdown) == len(consumers) + 1
    )
    passed = (
        completed == cycles
        and len(ids_seen) >= cycles + 1
        and all(len(values) == cycles + 1 for values in capabilities_seen.values())
        and processes_alive_before_shutdown
        and lifecycle_exact
        and all(shutdown_checks.values())
        and totals["correct_results"] > 0
        and resources.get("pass", False)
        and not orphan
        and not probe.cleanup_errors
    )
    status = "PASS" if passed else "FAIL"
    return summary(probe, status, start, {
        "requested_replacements": cycles,
        "completed_replacements": completed,
        "fresh_dynamic_ids_observed": len(ids_seen),
        "fresh_capability_ids_observed": {role: len(values) for role, values in capabilities_seen.items()},
        "provider_consumer_processes_retained_until_shutdown": processes_alive_before_shutdown,
        "lifecycle_before_shutdown": lifecycle_before_shutdown,
        "lifecycle_exact": lifecycle_exact,
        "shutdown_assertions": shutdown_checks,
        "consumer_totals": totals,
        "resource_measurements": resources,
        "orphan_process_groups": orphan,
        "cleanup_errors": probe.cleanup_errors,
        "late_old_authority_rejected": "not directly observable in process mode; covered by TransformBuffer tests",
    })


def run_restart_count(count: int, consumers_count: int) -> dict[str, Any]:
    probe = Probe(consumers_count, "composer-restart")
    start = time.monotonic()
    restarts = 0
    composer = provider = None
    consumers: list[Child] = []
    processes_alive_before_shutdown = False
    initial_lifecycle: dict[int, dict[str, int]] = {}
    lifecycle_before_shutdown: dict[int, dict[str, int]] = {}
    lifecycle_after_shutdown: dict[int, dict[str, int]] = {}
    try:
        composer, provider, consumers = probe.start_stack()
        lifecycle_participants = [provider, *consumers]
        initial_lifecycle = lifecycle_counts(lifecycle_participants)
        probe.begin_measurement()
        for _ in range(count):
            composer = restart_composer(probe, composer, provider, consumers)
            restarts += 1
        resource_participants = [composer, provider, *consumers]
        probe.capture_resource_window("post_recovery", resource_participants)
        probe.capture_resource_window("final_live", resource_participants)
        probe.capture_live_snapshot()
        lifecycle_before_shutdown = lifecycle_counts(lifecycle_participants)
        processes_alive_before_shutdown = (
            composer.process.poll() is None
            and provider is not None
            and provider.process.poll() is None
            and all(child.process.poll() is None for child in consumers)
        )
    finally:
        probe.cleanup()
    if provider is not None:
        lifecycle_after_shutdown = lifecycle_counts([provider, *consumers])
    orphan = probe.orphan_pids()
    resources = probe.assert_limits()
    totals = consumer_totals(consumers)
    shutdown_checks = probe.shutdown_assertions()
    provider_before = initial_lifecycle.get(id(provider)) if provider else None
    provider_after = lifecycle_after_shutdown.get(id(provider)) if provider else None
    provider_shutdown_deactivation_delta = (
        provider_after["deactivations"] - provider_before["deactivations"]
        if provider_before is not None and provider_after is not None
        else None
    )
    lifecycle_stable = bool(initial_lifecycle) and lifecycle_before_shutdown == initial_lifecycle
    lifecycle_shutdown = (
        bool(initial_lifecycle)
        and provider_before is not None
        and provider_after is not None
        and provider_after["activations"] == provider_before["activations"]
        and provider_shutdown_deactivation_delta in (0, 1)
        and all(
            lifecycle_after_shutdown.get(id(child))
            == {
                "activations": initial_lifecycle[id(child)]["activations"],
                "deactivations": initial_lifecycle[id(child)]["deactivations"] + 1,
            }
            for child in consumers
            if id(child) in initial_lifecycle
        )
        and len(lifecycle_after_shutdown) == len(consumers) + 1
    )
    passed = (
        restarts == count
        and processes_alive_before_shutdown
        and lifecycle_stable
        and lifecycle_shutdown
        and all(shutdown_checks.values())
        and totals["correct_results"] > 0
        and resources.get("pass", False)
        and not orphan
        and not probe.cleanup_errors
    )
    return summary(probe, "PASS" if passed else "FAIL", start, {
        "requested_composer_restarts": count,
        "completed_composer_restarts": restarts,
        "provider_and_consumer_processes_retained_until_shutdown": processes_alive_before_shutdown,
        "initial_provider_consumer_lifecycle": initial_lifecycle,
        "provider_consumer_lifecycle_before_shutdown": lifecycle_before_shutdown,
        "provider_consumer_lifecycle_after_shutdown": lifecycle_after_shutdown,
        "provider_consumer_lifecycle_stable": lifecycle_stable,
        "provider_consumer_shutdown_lifecycle_complete": lifecycle_shutdown,
        "provider_shutdown_deactivation_observation": {
            "before": provider_before,
            "after": provider_after,
            "deactivation_delta": provider_shutdown_deactivation_delta,
            "accepted_delta_values": [0, 1],
            "note": "provider worker deactivation may race native endpoint teardown; shutdown_complete and exit status are asserted separately",
        },
        "shutdown_assertions": shutdown_checks,
        "consumer_totals": totals,
        "resource_measurements": resources,
        "orphan_process_groups": orphan,
        "cleanup_errors": probe.cleanup_errors,
    })


def run_mixed_soak(duration: int, consumers_count: int, combined: bool) -> dict[str, Any]:
    if consumers_count < 2:
        raise ValueError("mixed TF soak requires at least two independent consumers")
    probe = Probe(consumers_count, "combined-outage" if combined else "soak")
    start = time.monotonic()
    composer = provider = None
    consumers: list[Child] = []
    composer_restarts = 0
    replacements = 0
    overlap_evidence: dict[str, Any] | None = None
    processes_alive_before_shutdown = False
    lifecycle_before_shutdown: dict[int, dict[str, int]] = {}
    lifecycle_after_shutdown: dict[int, dict[str, int]] = {}
    try:
        composer, provider, consumers = probe.start_stack(slow_consumer_index=0)
        probe.begin_measurement()
        deadline = time.monotonic() + duration
        soak_started = time.monotonic()
        provider_replaced = False
        composer_restarted = False
        overlap_done = False
        while time.monotonic() < deadline:
            for child in probe.children:
                child.assert_alive()
            probe.sample_resources()
            elapsed = time.monotonic() - soak_started
            if combined and not overlap_done and elapsed >= 30:
                composer, provider, overlap_evidence = restart_overlapping_outage(
                    probe,
                    composer,
                    provider,
                    consumers,
                )
                overlap_done = True
                replacements += 1
                composer_restarts += 1
            if not provider_replaced and elapsed >= (90 if combined else 30):
                provider = replace_provider(
                    probe,
                    provider,
                    consumers,
                    resource_warmup=True,
                )
                replacements += 1
                provider_replaced = True
            if not composer_restarted and elapsed >= (150 if combined else 75):
                composer = restart_composer(probe, composer, provider, consumers)
                composer_restarts += 1
                composer_restarted = True
            time.sleep(1.0)
        resource_participants = [composer, provider, *consumers]
        probe.capture_resource_window("post_recovery", resource_participants)
        probe.capture_resource_window("final_live", resource_participants)
        probe.capture_live_snapshot()
        lifecycle_before_shutdown = lifecycle_counts([provider, *consumers])
        processes_alive_before_shutdown = (
            composer.process.poll() is None
            and provider.process.poll() is None
            and all(child.process.poll() is None for child in consumers)
        )
    finally:
        probe.cleanup()
    lifecycle_after_shutdown = lifecycle_counts([provider, *consumers]) if provider else {}
    orphan = probe.orphan_pids()
    resources = probe.assert_limits()
    totals = consumer_totals(consumers)
    tool_events = [event for child in probe.children for event in child.events()]
    dynamic_summaries = sum(1 for event in tool_events if event.get("event") == "provider_summary")
    static_updates = sum(
        int(event.get("static_updates", 0))
        for event in tool_events
        if event.get("event") == "provider_summary"
    )
    pressure = pressure_observations(probe.children)
    pressure_checks = pressure_assertions(pressure)
    shutdown_checks = probe.shutdown_assertions()
    summary_events = [event for event in tool_events if event.get("event") == "consumer_summary"]
    fresh_lookup_activations = {
        str(event.get("activation_id")): int(event.get("correct_results", 0))
        for event in summary_events
        if event.get("activation_id") is not None
    }
    static_resync_events = [
        {
            "role": child.role,
            "activation_id": event.get("activation_id"),
            "resync": buffer_static_resync(event),
        }
        for child in consumers
        for event in consumer_summary_events(child)
        if buffer_static_resync(event) is not None
    ]
    lifecycle_exact = (
        bool(lifecycle_before_shutdown)
        and lifecycle_before_shutdown[id(provider)] == {"activations": 1, "deactivations": 0}
        and all(
            counts == {"activations": replacements + 1, "deactivations": replacements}
            for child in consumers
            if (counts := lifecycle_before_shutdown.get(id(child))) is not None
        )
        and len(lifecycle_before_shutdown) == len(consumers) + 1
    )
    provider_before = lifecycle_before_shutdown.get(id(provider)) if provider else None
    provider_after = lifecycle_after_shutdown.get(id(provider)) if provider else None
    provider_shutdown_deactivation_delta = (
        provider_after["deactivations"] - provider_before["deactivations"]
        if provider_before is not None and provider_after is not None
        else None
    )
    shutdown_lifecycle_exact = (
        provider_before is not None
        and provider_after is not None
        and provider_after["activations"] == provider_before["activations"]
        and provider_shutdown_deactivation_delta in (0, 1)
        and all(
            lifecycle_after_shutdown.get(id(child))
            == {
                "activations": counts["activations"],
                "deactivations": counts["deactivations"] + 1,
            }
            for child in consumers
            if (counts := lifecycle_before_shutdown.get(id(child))) is not None
        )
        and len(lifecycle_after_shutdown) == len(consumers) + 1
    )
    required_duration_met = (time.monotonic() - soak_started) >= 300 if "soak_started" in locals() else False
    required_assertions = {
        "dynamic_samples_and_local_interpolation": totals["correct_results"] > 0,
        "static_revision_updates": static_updates > 0,
        "multiple_independent_consumers": len(consumers) >= 2,
        "provider_replacements": replacements > 0,
        "composer_only_restarts": composer_restarts > (1 if combined else 0),
        "verified_overlapping_composer_provider_outage": overlap_evidence is not None if combined else True,
        "actual_slow_control_subscriber": pressure_checks["actual_slow_control_subscriber"],
        "native_stream_guards_bounded": pressure_checks["native_stream_guards_bounded"],
        "native_transport_retired_after_join": pressure_checks[
            "native_transport_retired_after_join"
        ],
        "native_rpc_pending_and_guards_bounded": pressure_checks["native_rpc_pending_and_guards_bounded"],
        "native_rpc_server_requests_bounded": pressure_checks["native_rpc_server_requests_bounded"],
        "bounded_history": pressure_checks["history_within_256_per_edge_and_8192_global"],
        "periodic_static_snapshot_resync": bool(static_resync_events),
        "provider_observed_control_queue_pressure": pressure_checks["provider_observed_control_queue_pressure"],
        "provider_consumer_lifecycle_exact": lifecycle_exact,
        "processes_alive_before_cleanup": processes_alive_before_shutdown,
        "shutdown_lifecycle_exact": shutdown_lifecycle_exact,
        "all_sigterm_processes_shutdown_cleanly": all(shutdown_checks.values()),
        "minimum_300_second_soak": required_duration_met,
        "resource_limits": resources.get("pass", False),
        "no_orphan_processes": not orphan,
        "cleanup_completed": not probe.cleanup_errors,
    }
    passed = all(required_assertions.values())
    completeness = {
        **required_assertions,
    }
    status = "PASS" if passed else "FAIL"
    return summary(probe, status, start, {
        "requested_duration_seconds": duration,
        "actual_wall_seconds": round(time.monotonic() - start, 3),
        "composer_restarts": composer_restarts,
        "provider_replacements": replacements,
        "overlapping_outage": overlap_evidence,
        "provider_summary_events": dynamic_summaries,
        "static_update_count_observed": static_updates,
        "static_resync_events": static_resync_events,
        "fresh_lookup_activations": fresh_lookup_activations,
        "consumer_totals": totals,
        "assertions": completeness,
        "native_pressure_observations": pressure,
        "native_pressure_assertions": pressure_checks,
        "provider_consumer_lifecycle_before_shutdown": lifecycle_before_shutdown,
        "provider_consumer_lifecycle_after_shutdown": lifecycle_after_shutdown,
        "provider_consumer_lifecycle_exact": lifecycle_exact,
        "provider_consumer_shutdown_lifecycle_exact": shutdown_lifecycle_exact,
        "provider_shutdown_deactivation_observation": {
            "before": provider_before,
            "after": provider_after,
            "deactivation_delta": provider_shutdown_deactivation_delta,
            "accepted_delta_values": [0, 1],
            "note": "provider worker deactivation may race native endpoint teardown; shutdown_complete and exit status are asserted separately",
        },
        "shutdown_assertions": shutdown_checks,
        "processes_alive_before_cleanup": processes_alive_before_shutdown,
        "resource_measurements": resources,
        "orphan_process_groups": orphan,
        "cleanup_errors": probe.cleanup_errors,
    })


def summary(probe: Probe, status: str, start: float, values: dict[str, Any]) -> dict[str, Any]:
    return {
        "status": status,
        "mode": probe.mode,
        "fixture": "synthetic software fixture; not physical measurement",
        "actual_wall_seconds": round(time.monotonic() - start, 3),
        "logs_directory": str(probe.run_dir),
        "run_metadata_file": str(probe.metadata_path),
        "provider_lead_seconds_before_consumers": round(probe.provider_lead_seconds, 3),
        "startup_data_readiness": probe.startup_readiness,
        "recovery_data_readiness": probe.recovery_readiness,
        "child_terminations": probe.termination_reports(),
        "shutdown_assertions": probe.shutdown_assertions(),
        "measurements": values,
    }


def save_summary(report: dict[str, Any]) -> dict[str, Any]:
    run_dir = Path(report["logs_directory"])
    summary_path = run_dir / "summary.json"
    report["summary_file"] = str(summary_path)
    summary_path.write_text(
        json.dumps(report, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return report


def failure_report(mode: str, error: BaseException, invocation_started: float) -> dict[str, Any]:
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    latest = ACTIVE_RUN_DIR
    if latest is None or latest.stat().st_mtime < invocation_started:
        latest = LOG_DIR / f"{int(time.time())}-{os.getpid()}-{mode}-failure"
        latest.mkdir(parents=True, exist_ok=True)
    report = {
        "status": "INTERRUPTED" if isinstance(error, KeyboardInterrupt) else "FAIL",
        "mode": mode,
        "error": f"{type(error).__name__}: {error}",
        "traceback": traceback.format_exc(),
        "fixture": "synthetic software fixture; not physical measurement",
        "logs_directory": str(latest),
        "run_metadata_file": str(latest / "run-metadata.json"),
    }
    metadata_path = latest / "run-metadata.json"
    if not metadata_path.exists():
        metadata = {
            "started_utc": datetime.datetime.fromtimestamp(
                invocation_started, datetime.timezone.utc
            ).isoformat(),
            "probe_argv": sys.argv,
            "probe_source": str(Path(__file__).resolve()),
            "probe_source_sha256": sha256_file(Path(__file__).resolve()),
            "tool_source_sha256": tool_source_hashes(),
            "tool_binary": str(TOOL),
            "tool_binary_sha256": sha256_file(TOOL) if TOOL.exists() else None,
            "python_version": sys.version,
        }
        metadata_path.write_text(
            json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    return save_summary(report)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument("--replacement-cycles", type=int)
    modes.add_argument("--restart-count", type=int)
    modes.add_argument("--combined-outage", action="store_true")
    modes.add_argument("--soak", action="store_true")
    parser.add_argument("--duration", type=int, default=300)
    parser.add_argument("--consumers", type=int, default=2)
    return parser.parse_args()


def main() -> int:
    invocation_started = time.time()
    args = parse_args()
    if args.consumers < 1 or args.consumers > 16:
        raise SystemExit("--consumers must be in 1..=16")
    if args.consumers < 2 and (args.soak or args.combined_outage):
        raise SystemExit("--soak/--combined-outage requires at least two consumers")
    mode = "replacement" if args.replacement_cycles is not None else "composer-restart" if args.restart_count is not None else "combined-outage" if args.combined_outage else "soak"
    try:
        build_tool()
        if args.replacement_cycles is not None:
            if args.replacement_cycles < 1:
                raise SystemExit("--replacement-cycles must be positive")
            report = run_replacement_cycles(args.replacement_cycles, args.consumers)
        elif args.restart_count is not None:
            if args.restart_count < 1:
                raise SystemExit("--restart-count must be positive")
            report = run_restart_count(args.restart_count, args.consumers)
        else:
            if args.duration < 300:
                raise SystemExit("--soak/--combined-outage duration must be at least 300 seconds")
            report = run_mixed_soak(args.duration, args.consumers, args.combined_outage)
    except KeyboardInterrupt as error:
        report = failure_report(mode, error, invocation_started)
        print(json.dumps(report, separators=(",", ":"), sort_keys=True))
        return 130
    except Exception as error:
        report = failure_report(mode, error, invocation_started)
        print(json.dumps(report, separators=(",", ":"), sort_keys=True))
        return 1
    save_summary(report)
    print(json.dumps(report, separators=(",", ":"), sort_keys=True))
    return 0 if report["status"] == "PASS" else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        print(json.dumps({"status": "INTERRUPTED", "mode": "tf_process_probe"}), file=sys.stderr)
        raise SystemExit(130)

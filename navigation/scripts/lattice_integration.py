#!/usr/bin/env python3
"""Run isolated front-drive State Lattice navigation over native RBT IPC."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid


SCENARIOS = ("turn", "reverse", "tail_sweep")
SCENARIO_BUDGET_SECONDS = 90.0
STARTUP_BUDGET_SECONDS = 12.0
PROCESS_STOP_SECONDS = 0.75
REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_GEOMETRY = REPO_ROOT / "navigation/config/front-drive-geometry.yaml"
DEFAULT_LATTICE = REPO_ROOT / "navigation/config/lattice/diff-5cm-0_5m.json"
START_POSES = {
    "turn": (2.0, 2.0, 0.0),
    "reverse": (5.5, 4.0, 0.0),
    "tail_sweep": (6.0, 6.0, 0.0),
}


class IntegrationError(RuntimeError):
    pass


class ScenarioFailure(IntegrationError):
    def __init__(self, message: str, record: dict[str, object]) -> None:
        super().__init__(message)
        self.record = record


def executable(build_dir: Path, name: str) -> Path:
    candidates = (build_dir / name,
                  build_dir / "Release" / name,
                  build_dir / "RelWithDebInfo" / name,
                  build_dir / "Debug" / name)
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    raise IntegrationError(f"could not find executable {name} under {build_dir}")


def runtime_environment() -> dict[str, str]:
    environment = os.environ.copy()
    extra_libraries = (REPO_ROOT / "target/phase4-toolchain/lib",
                       REPO_ROOT / "target/map-image-sdk/lib",
                       Path("/home/sw/miniforge3/lib"))
    paths = [str(directory) for directory in extra_libraries if directory.is_dir()]
    if paths:
        existing = environment.get("LD_LIBRARY_PATH", "")
        environment["LD_LIBRARY_PATH"] = os.pathsep.join(
            paths + ([existing] if existing else []))
    return environment


def wait_for_ready(process: subprocess.Popen[bytes], log_path: Path, marker: str,
                   deadline: float) -> None:
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise IntegrationError(
                f"process exited with {process.returncode} before {marker!r}; see {log_path}")
        try:
            if marker in log_path.read_text(errors="replace"):
                return
        except FileNotFoundError:
            pass
        time.sleep(0.05)
    raise IntegrationError(f"timed out waiting for {marker!r}; see {log_path}")


def stop_process(process: subprocess.Popen[bytes]) -> int | None:
    if process.poll() is None:
        process.terminate()
        try:
            return process.wait(timeout=PROCESS_STOP_SECONDS)
        except subprocess.TimeoutExpired:
            process.kill()
            try:
                return process.wait(timeout=PROCESS_STOP_SECONDS)
            except subprocess.TimeoutExpired as error:
                raise IntegrationError(f"could not stop child process {process.pid}") from error
    return process.returncode


def unique_names() -> tuple[str, str, str]:
    nonce = uuid.uuid4().hex
    map_id = str(uuid.uuid4())
    odom_id = str(uuid.uuid4())
    while odom_id == map_id:
        odom_id = str(uuid.uuid4())
    return f"rbt/navigation/lattice/{nonce}", map_id, odom_id


def run_scenario(scenario: str, build_dir: Path, output_dir: Path,
                 geometry: Path, lattice: Path, controller: str) -> dict[str, object]:
    runtime_binary = executable(build_dir, "rbt_navigation_runtime")
    guard_binary = executable(build_dir, "rbt_velocity_guard")
    base_binary = executable(build_dir, "rbt_navigation_base_sim")
    fixture_binary = executable(build_dir, "rbt_navigation_lattice_fixture")
    prefix, map_id, odom_id = unique_names()
    cmd_vel_topic = f"/{prefix}/cmd_vel"
    if cmd_vel_topic == "/cmd_vel":
        raise IntegrationError("refusing to use the physical /cmd_vel topic")
    desired_topic = f"{prefix}/desired_cmd_vel"
    scenario_dir = output_dir / scenario
    scenario_dir.mkdir(parents=True, exist_ok=True)
    logs = {name: scenario_dir / f"{name}.log" for name in
            ("base-controller-sim", "velocity-guard", "runtime", "fixture")}
    result_file = scenario_dir / "result.json"
    deadline = time.monotonic() + SCENARIO_BUDGET_SECONDS
    startup_deadline = min(deadline, time.monotonic() + STARTUP_BUDGET_SECONDS)
    processes: list[tuple[str, subprocess.Popen[bytes]]] = []
    environment = runtime_environment()
    record: dict[str, object] = {
        "scenario": scenario,
        "prefix": prefix,
        "tf_map": map_id,
        "tf_odom": odom_id,
        "desired_topic": desired_topic,
        "cmd_vel_topic": cmd_vel_topic,
        "robot_geometry": str(geometry),
        "lattice_file": str(lattice),
        "controller": controller,
        "allow_reverse": scenario == "reverse",
        "passed": False,
        "result_file": str(result_file),
        "logs": {name: str(path) for name, path in logs.items()},
    }
    pending_error: Exception | None = None

    def start(name: str, command: list[str]) -> subprocess.Popen[bytes]:
        with logs[name].open("wb") as log_file:
            process = subprocess.Popen(command, cwd=REPO_ROOT, env=environment,
                                       stdout=log_file, stderr=subprocess.STDOUT,
                                       close_fds=True)
        processes.append((name, process))
        return process

    try:
        initial_x, initial_y, initial_yaw = START_POSES[scenario]
        base = start("base-controller-sim", [
            str(base_binary), "--prefix", prefix, "--cmd-vel-topic", cmd_vel_topic,
            "--guard-source-id", "100", "--base-timeout-ms", "250",
            "--estop-source", "2", "--initial-x", str(initial_x),
            "--initial-y", str(initial_y), "--initial-yaw", str(initial_yaw),
        ])
        wait_for_ready(base, logs["base-controller-sim"],
                       "base controller SIMULATOR ready", startup_deadline)

        guard = start("velocity-guard", [
            str(guard_binary), "--prefix", prefix, "--desired-topic", desired_topic,
            "--cmd-vel-topic", cmd_vel_topic, "--source-id", "1",
            "--guard-source-id", "100", "--estop-source", "2",
            "--guard-timeout-ms", "250", "--cmd-ttl-ms", "100",
        ])
        wait_for_ready(guard, logs["velocity-guard"], "velocity guard ready",
                       startup_deadline)

        allow_reverse = "1" if scenario == "reverse" else "0"
        runtime = start("runtime", [
            str(runtime_binary), "--prefix", prefix, "--tf-map", map_id,
            "--tf-odom", odom_id, "--source-id", "1", "--control-hz", "50",
            "--map-hz", "10", "--input-timeout-ms", "250", "--map-timeout-ms", "2000",
            "--plan-timeout-ms", "4000", "--planning-clearance", "0",
            "--max-linear", "0.3", "--max-angular", "0.8", "--controller", controller,
            "--desired-topic", desired_topic, "--planner", "smac-lattice",
            "--robot-geometry", str(geometry), "--lattice-file", str(lattice),
            "--allow-reverse", allow_reverse,
        ])
        wait_for_ready(runtime, logs["runtime"], "navigation runtime ready",
                       startup_deadline)

        fixture = start("fixture", [
            str(fixture_binary), "--scenario", scenario, "--prefix", prefix,
            "--tf-map", map_id, "--tf-odom", odom_id,
            "--robot-geometry", str(geometry), "--output", str(scenario_dir),
        ])
        wait_for_ready(fixture, logs["fixture"], "lattice native fixture ready:",
                       startup_deadline)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise IntegrationError("scenario startup exceeded its wall-clock budget")
        try:
            fixture_exit = fixture.wait(timeout=remaining)
        except subprocess.TimeoutExpired as error:
            raise IntegrationError(
                f"scenario exceeded {SCENARIO_BUDGET_SECONDS:.0f} seconds") from error
        record["fixture_exit_code"] = fixture_exit
        record["fixture_output"] = logs["fixture"].read_text(errors="replace").strip()
        if fixture_exit != 0:
            raise IntegrationError(f"fixture failed for {scenario}; see {logs['fixture']}")
        if not result_file.is_file():
            raise IntegrationError(f"fixture did not write {result_file}")
        fixture_result = json.loads(result_file.read_text())
        if fixture_result.get("passed") is not True:
            raise IntegrationError(f"fixture result did not pass: {fixture_result.get('error')}")
        for name in ("path.csv", "telemetry.csv"):
            if not (scenario_dir / name).is_file():
                raise IntegrationError(f"fixture did not write {name}")
        record["fixture_result"] = fixture_result
        record["passed"] = True
    except Exception as error:
        pending_error = error
        record["error"] = str(error)
        record["fixture_output"] = logs["fixture"].read_text(errors="replace").strip() \
            if logs["fixture"].exists() else ""
        if result_file.is_file():
            try:
                record["fixture_result"] = json.loads(result_file.read_text())
            except (OSError, json.JSONDecodeError):
                pass
    finally:
        exit_codes: dict[str, int | None] = {}
        for name, process in reversed(processes):
            try:
                exit_codes[name] = stop_process(process)
            except IntegrationError as error:
                record["cleanup_error"] = str(error)
        record["process_exit_codes"] = exit_codes
        for name, path in logs.items():
            record[f"{name.replace('-', '_')}_output"] = (
                path.read_text(errors="replace").strip() if path.exists() else "")

    if pending_error is not None:
        record["passed"] = False
        raise ScenarioFailure(f"{scenario}: {pending_error}", record) from pending_error
    unexpected = {name: code for name, code in exit_codes.items() if code != 0}
    if unexpected:
        record["passed"] = False
        record["error"] = f"unexpected process exit codes {unexpected}"
        raise ScenarioFailure(f"{scenario}: {record['error']}", record)
    if "cleanup_error" in record:
        record["passed"] = False
        record["error"] = record["cleanup_error"]
        raise ScenarioFailure(f"{scenario}: {record['error']}", record)
    return record


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path,
                        help="CMake build directory containing the native executables")
    parser.add_argument("--output-dir", type=Path,
                        help="directory for per-scenario telemetry and results")
    parser.add_argument("--scenario", choices=("all", *SCENARIOS), default="all",
                        help="scenario to run (default: all)")
    parser.add_argument("--robot-geometry", type=Path, default=DEFAULT_GEOMETRY,
                        help="front-drive geometry YAML")
    parser.add_argument("--lattice-file", type=Path, default=DEFAULT_LATTICE,
                        help="0.05 m differential-drive motion-primitive JSON")
    parser.add_argument("--controller", choices=("rpp", "rpp-dwpp", "vector-pursuit", "vector-pursuit-window"), default="rpp-dwpp",
                        help="local path controller (default: rpp-dwpp)")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    build_dir = args.build_dir.expanduser().resolve()
    geometry = args.robot_geometry.expanduser().resolve()
    lattice = args.lattice_file.expanduser().resolve()
    for name, path in (("robot geometry", geometry), ("lattice file", lattice)):
        if not path.is_file():
            print(f"FAIL lattice integration: {name} does not exist: {path}", file=sys.stderr)
            return 2
    output_dir = (args.output_dir.expanduser().resolve() if args.output_dir
                  else build_dir / "lattice-integration-logs")
    output_dir.mkdir(parents=True, exist_ok=True)
    scenarios = SCENARIOS if args.scenario == "all" else (args.scenario,)
    results: list[dict[str, object]] = []
    failure: str | None = None
    for scenario in scenarios:
        try:
            result = run_scenario(scenario, build_dir, output_dir, geometry, lattice,
                                  args.controller)
            results.append(result)
            print(f"PASS lattice integration {scenario}: {result['fixture_output']}", flush=True)
        except Exception as error:
            failure = f"FAIL lattice integration {scenario}: {error}"
            print(failure, file=sys.stderr, flush=True)
            if isinstance(error, ScenarioFailure):
                results.append(error.record)
            else:
                results.append({"scenario": scenario, "passed": False, "error": str(error)})
            break

    results_name = ("results.json" if args.scenario == "all"
                    else f"results-{args.scenario}.json")
    results_file = output_dir / results_name
    results_file.write_text(json.dumps({
        "passed": failure is None and len(results) == len(scenarios),
        "scenarios": results,
    }, indent=2) + "\n")
    print(f"results: {results_file}")
    return 1 if failure is not None else 0


if __name__ == "__main__":
    raise SystemExit(main())

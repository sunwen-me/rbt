#!/usr/bin/env python3
"""Test native navigation -> VelocityGuard -> independent mock base over RBT."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid


SCENARIOS = ("success", "obstacle", "cancel", "dropout", "watchdog", "estop",
             "guard_crash", "expired", "authority", "cmd_vel", "base_expired")
MAP_SCENARIOS = ("obstacle", "map_dropout")
SCENARIO_BUDGET_SECONDS = 31.0
PROCESS_STOP_SECONDS = 0.75
REPO_ROOT = Path(__file__).resolve().parents[2]


class IntegrationError(RuntimeError):
    pass


class ScenarioFailure(IntegrationError):
    def __init__(self, message: str, result: dict[str, object]):
        super().__init__(message)
        self.result = result


def executable(build_dir: Path, name: str) -> Path:
    candidates = (build_dir / name,
                  build_dir / "Release" / name,
                  build_dir / "RelWithDebInfo" / name,
                  build_dir / "Debug" / name)
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    raise IntegrationError(f"could not find executable {name} under {build_dir}")


def wait_for_ready(process: subprocess.Popen[bytes], log_path: Path, marker: str,
                   deadline: float) -> None:
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise IntegrationError(
                f"process exited with {process.returncode} before readiness marker {marker!r}; "
                f"see {log_path}"
            )
        try:
            if marker in log_path.read_text(errors="replace"):
                return
        except FileNotFoundError:
            pass
        time.sleep(0.05)
    raise IntegrationError(f"timed out waiting for readiness marker {marker!r}; see {log_path}")


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
    return f"rbt/navigation/test{nonce}", map_id, odom_id


def run_scenario(scenario: str, controller: str, build_dir: Path,
                 output_dir: Path, map_file: Path | None = None) -> dict[str, object]:
    if scenario == "map_dropout" and map_file is None:
        raise IntegrationError("map_dropout requires --map-file")
    if map_file is not None and scenario not in MAP_SCENARIOS:
        raise IntegrationError("--map-file is only valid for obstacle or map_dropout scenarios")
    runtime_binary = executable(build_dir, "rbt_navigation_runtime")
    guard_binary = executable(build_dir, "rbt_velocity_guard")
    base_binary = executable(build_dir, "rbt_navigation_base_sim")
    fixture_binary = executable(build_dir, "rbt_navigation_native_fixture")
    map_server_binary = executable(build_dir, "rbt_navigation_map_server") if map_file else None
    prefix, map_id, odom_id = unique_names()
    scenario_dir = output_dir / scenario
    scenario_dir.mkdir(parents=True, exist_ok=True)
    base_log = scenario_dir / "base-controller-sim.log"
    guard_log = scenario_dir / "velocity-guard.log"
    runtime_log = scenario_dir / "runtime.log"
    fixture_log = scenario_dir / "fixture.log"
    map_server_log = scenario_dir / "map-server.log" if map_file else None
    deadline = time.monotonic() + SCENARIO_BUDGET_SECONDS
    processes: list[tuple[str, subprocess.Popen[bytes]]] = []
    desired_topic = prefix + "/desired_cmd_vel"
    cmd_vel_topic = "/" + prefix + "/cmd_vel"
    # The legacy circular A* obstacle demos need room for curved RPP tracking
    # around grid corners. The separate front-drive lattice suite uses its
    # actual polygon and zero additional planning clearance.
    planning_clearance = 0.20 if scenario in ("obstacle", "map_dropout") else 0.10
    record: dict[str, object] = {
        "scenario": scenario,
        "controller": controller,
        "planning_clearance": planning_clearance,
        "prefix": prefix,
        "tf_map": map_id,
        "tf_odom": odom_id,
        "desired_topic": desired_topic,
        "cmd_vel_topic": cmd_vel_topic,
        "passed": False,
        "map_file": str(map_file) if map_file else None,
        "logs": {
            "base_controller_sim": str(base_log),
            "velocity_guard": str(guard_log),
            "runtime": str(runtime_log),
            "fixture": str(fixture_log),
        },
    }
    if map_server_log is not None:
        record["map_server_log"] = str(map_server_log)
        record["logs"]["map_server"] = str(map_server_log)
    pending_error: Exception | None = None

    def start(name: str, command: list[str], log_path: Path) -> subprocess.Popen[bytes]:
        with log_path.open("wb") as log_file:
            process = subprocess.Popen(command, cwd=REPO_ROOT, stdout=log_file,
                                       stderr=subprocess.STDOUT, close_fds=True)
        processes.append((name, process))
        return process

    try:
        base = start("base_controller_sim", [
            str(base_binary), "--prefix", prefix,
            "--cmd-vel-topic", cmd_vel_topic, "--guard-source-id", "100",
            "--base-timeout-ms", "250", "--estop-source", "2", "--initial-x", "1",
            "--initial-y", "1", "--initial-yaw", "0",
        ], base_log)
        wait_for_ready(base, base_log, "base controller SIMULATOR ready", deadline)

        guard = start("velocity_guard", [
            str(guard_binary), "--prefix", prefix, "--desired-topic", desired_topic,
            "--cmd-vel-topic", cmd_vel_topic, "--source-id", "1",
            "--guard-source-id", "100", "--estop-source", "2",
            "--guard-timeout-ms", "250", "--cmd-ttl-ms", "100",
        ], guard_log)
        wait_for_ready(guard, guard_log, "velocity guard ready", deadline)

        runtime = start("runtime", [
            str(runtime_binary), "--prefix", prefix, "--tf-map", map_id,
            "--tf-odom", odom_id, "--source-id", "1", "--control-hz", "50",
            "--map-hz", "10", "--input-timeout-ms", "250", "--map-timeout-ms", "2000",
            "--plan-timeout-ms", "500", "--controller", controller,
            "--robot-geometry", "circle",
            "--planning-clearance", f"{planning_clearance:.2f}",
            "--desired-topic", desired_topic,
        ], runtime_log)
        wait_for_ready(runtime, runtime_log, "navigation runtime ready", deadline)

        map_server: subprocess.Popen[bytes] | None = None
        if map_file is not None:
            assert map_server_binary is not None and map_server_log is not None
            map_server = start("map_server", [
                str(map_server_binary), "--map", str(map_file), "--prefix", prefix,
            ], map_server_log)
            wait_for_ready(map_server, map_server_log, "navigation map server ready:", deadline)

        fixture_command = [
            str(fixture_binary), "--scenario", scenario, "--prefix", prefix,
            "--tf-map", map_id, "--tf-odom", odom_id,
            "--runtime-pid", str(runtime.pid),
            "--guard-pid", str(guard.pid), "--desired-topic", desired_topic,
            "--cmd-vel-topic", cmd_vel_topic, "--guard-source-id", "100",
        ]
        if map_server is not None:
            fixture_command.extend(["--external-map", "1", "--map-pid", str(map_server.pid)])
        fixture = start("fixture", fixture_command, fixture_log)
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise IntegrationError("scenario startup exceeded its time budget")
        try:
            fixture_exit = fixture.wait(timeout=remaining)
        except subprocess.TimeoutExpired as error:
            raise IntegrationError(f"scenario exceeded {SCENARIO_BUDGET_SECONDS:.0f} seconds") from error
        record["fixture_exit_code"] = fixture_exit
        record["fixture_output"] = fixture_log.read_text(errors="replace").strip()
        if fixture_exit != 0:
            raise IntegrationError(f"fixture failed for {scenario}; see {fixture_log}")
        record["passed"] = True
    except Exception as error:
        pending_error = error
        record["error"] = str(error)
        record["fixture_output"] = fixture_log.read_text(errors="replace").strip() if fixture_log.exists() else ""
    finally:
        exit_codes: dict[str, int | None] = {}
        for name, process in reversed(processes):
            try:
                exit_codes[name] = stop_process(process)
            except IntegrationError as error:
                record["cleanup_error"] = str(error)
        record["process_exit_codes"] = exit_codes
        record["base_controller_output"] = base_log.read_text(errors="replace").strip() if base_log.exists() else ""
        record["guard_output"] = guard_log.read_text(errors="replace").strip() if guard_log.exists() else ""
        record["runtime_output"] = runtime_log.read_text(errors="replace").strip() if runtime_log.exists() else ""
        if map_server_log is not None:
            record["map_server_output"] = (map_server_log.read_text(errors="replace").strip()
                                            if map_server_log.exists() else "")
        record["map_server_exit_code"] = exit_codes.get("map_server")

    if pending_error is not None:
        record["passed"] = False
        raise ScenarioFailure(str(pending_error), record) from pending_error

    expected_exit_codes = {
        "base_controller_sim": 0,
        "velocity_guard": -9 if scenario in ("guard_crash", "base_expired") else 0,
        "runtime": -9 if scenario in ("watchdog", "expired") else 0,
        "fixture": 0,
    }
    if map_file is not None:
        expected_exit_codes["map_server"] = -9 if scenario == "map_dropout" else 0
    exit_codes = record["process_exit_codes"]
    unexpected = {
        name: exit_codes.get(name)
        for name, expected in expected_exit_codes.items()
        if exit_codes.get(name) != expected
    }
    if unexpected:
        record["passed"] = False
        error = f"unexpected child process exit codes: {unexpected}"
        record["error"] = error
        raise ScenarioFailure(error, record)
    if "cleanup_error" in record:
        record["passed"] = False
        raise ScenarioFailure(str(record["cleanup_error"]), record)

    return record


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path,
                        help="CMake build directory containing the native executables")
    parser.add_argument("--output-dir", type=Path,
                        help="directory for per-process logs and results.json")
    parser.add_argument("--scenario", choices=("all", *SCENARIOS, "map_dropout"), default="all",
                        help="scenario to run (default: all)")
    parser.add_argument("--controller", choices=("rpp", "rpp-dwpp"), default="rpp-dwpp",
                        help="runtime controller to use (default: rpp-dwpp)")
    parser.add_argument("--map-file", type=Path,
                        help="external occupancy map YAML for obstacle or map_dropout scenarios")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    build_dir = args.build_dir.expanduser().resolve()
    map_file = args.map_file.expanduser().resolve() if args.map_file else None
    if map_file is not None and not map_file.is_file():
        print(f"FAIL native integration: map file does not exist: {map_file}", file=sys.stderr)
        return 2
    if map_file is not None and args.scenario != "all" and args.scenario not in MAP_SCENARIOS:
        print("FAIL native integration: --map-file is only valid for obstacle or map_dropout scenarios",
              file=sys.stderr)
        return 2
    if args.scenario == "map_dropout" and map_file is None:
        print("FAIL native integration: map_dropout requires --map-file", file=sys.stderr)
        return 2
    output_dir = (args.output_dir.expanduser().resolve() if args.output_dir
                  else build_dir / "native-integration-logs")
    output_dir.mkdir(parents=True, exist_ok=True)
    if args.scenario == "all":
        scenarios = SCENARIOS + (("map_dropout",) if map_file else ())
    else:
        scenarios = (args.scenario,)
    results: list[dict[str, object]] = []
    error_message: str | None = None

    for scenario in scenarios:
        try:
            scenario_map_file = map_file if scenario in MAP_SCENARIOS else None
            result = run_scenario(scenario, args.controller, build_dir, output_dir,
                                  scenario_map_file)
            results.append(result)
            print(f"PASS native integration {scenario}: {result['fixture_output']}")
        except Exception as error:
            error_message = f"FAIL native integration {scenario}: {error}"
            print(error_message, file=sys.stderr)
            if isinstance(error, ScenarioFailure):
                results.append(error.result)
            else:
                results.append({"scenario": scenario, "controller": args.controller,
                                "passed": False, "error": str(error)})
            break

    results_path = output_dir / "results.json"
    results_path.write_text(json.dumps({
        "passed": error_message is None and len(results) == len(scenarios),
        "controller": args.controller,
        "results": results,
    }, indent=2) + "\n")
    print(f"results: {results_path}")
    if error_message is not None:
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

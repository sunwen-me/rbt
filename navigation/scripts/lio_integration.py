#!/usr/bin/env python3
"""Exercise the LIO bridge against a private Composer and simulated base."""

from __future__ import annotations

import argparse
from collections.abc import Callable
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import sys
import time
import uuid


SCENARIOS = (
    "obstacle", "avoidance", "invalid_localization", "stale", "future", "foreign_clock",
    "source_crash", "bridge_crash", "wrong_provider",
)
SCENARIO_TIMEOUT_SECONDS = 30.0
STARTUP_TIMEOUT_SECONDS = 9.0
FAULT_STOP_TIMEOUT_SECONDS = 1.5
PROCESS_STOP_SECONDS = 0.75
REPO_ROOT = Path(__file__).resolve().parents[2]
BRIDGE_READY = "lio bridge ready:"


class IntegrationError(RuntimeError):
    pass


class ScenarioFailure(IntegrationError):
    def __init__(self, message: str, result: dict[str, object]):
        super().__init__(message)
        self.result = result


def executable(build_dir: Path, name: str) -> Path:
    candidates = (build_dir / name, build_dir / "Release" / name,
                  build_dir / "RelWithDebInfo" / name, build_dir / "Debug" / name)
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    raise IntegrationError(f"could not find executable {name} under {build_dir}")


def composer_executable(path: Path | None) -> Path:
    if path is not None:
        candidate = path.expanduser().resolve()
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
        raise IntegrationError(f"Composer executable is not runnable: {candidate}")
    env_path = os.environ.get("RBT_COMPOSER_BIN")
    if env_path:
        candidate = Path(env_path).expanduser().resolve()
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    for candidate in (REPO_ROOT / "target/debug/composer",
                      REPO_ROOT / "target/release/composer"):
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    located = shutil.which("composer")
    if located:
        return Path(located).resolve()
    raise IntegrationError("could not find Composer; pass --composer or set RBT_COMPOSER_BIN")


def free_address() -> str:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return f"127.0.0.1:{listener.getsockname()[1]}"


def wait_for_log(process: subprocess.Popen[bytes], log_path: Path, marker: str,
                 deadline: float) -> None:
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise IntegrationError(
                f"process exited with {process.returncode} before readiness marker {marker!r}; "
                f"see {log_path}"
            )
        if log_path.exists() and marker in log_path.read_text(errors="replace"):
            return
        time.sleep(0.04)
    raise IntegrationError(f"timed out waiting for readiness marker {marker!r}; see {log_path}")


def wait_for_composer(address: str, process: subprocess.Popen[bytes],
                      deadline: float, log_path: Path) -> None:
    host, port_text = address.rsplit(":", 1)
    port = int(port_text)
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise IntegrationError(f"Composer exited with {process.returncode}; see {log_path}")
        try:
            with socket.create_connection((host, port), timeout=0.15):
                return
        except OSError:
            time.sleep(0.04)
    raise IntegrationError(f"Composer did not listen at {address}; see {log_path}")


def inspect_composer(address: str) -> dict[str, object]:
    host, port_text = address.rsplit(":", 1)
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
        raise IntegrationError("Composer inspection returned no response")
    response = json.loads(response_line)
    if not isinstance(response, dict):
        raise IntegrationError(f"unexpected Composer inspection response: {response}")
    view = response.get("view")
    if response.get("type") != "inspect_response" or not isinstance(view, dict):
        raise IntegrationError(f"unexpected Composer inspection response: {response}")
    return view


def wait_for_component(address: str, component_id: str,
                       predicate: Callable[[dict[str, object]], bool],
                       deadline: float) -> dict[str, object]:
    last_state: object = None
    while time.monotonic() < deadline:
        try:
            view = inspect_composer(address)
        except (OSError, json.JSONDecodeError):
            time.sleep(0.05)
            continue
        components = view.get("components")
        if isinstance(components, list):
            for component in components:
                if not isinstance(component, dict) or component.get("component_id") != component_id:
                    continue
                snapshot = component.get("snapshot")
                if isinstance(snapshot, dict):
                    last_state = snapshot.get("state")
                    if predicate(component):
                        return component
        time.sleep(0.05)
    raise IntegrationError(
        f"timed out waiting for Composer component {component_id!r}; last state={last_state!r}"
    )


def stop_process(process: subprocess.Popen[bytes], force: bool = False) -> int | None:
    if process.poll() is None:
        try:
            os.killpg(process.pid, signal.SIGKILL if force else signal.SIGTERM)
        except ProcessLookupError:
            pass
        try:
            return process.wait(timeout=PROCESS_STOP_SECONDS)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                return process.wait(timeout=PROCESS_STOP_SECONDS)
            except subprocess.TimeoutExpired as error:
                raise IntegrationError(f"could not stop owned child process {process.pid}") from error
    return process.returncode


def read_json(path: Path) -> dict[str, object] | None:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError, OSError):
        return None
    return value if isinstance(value, dict) else None


def write_control(path: Path, phase: str) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(phase + "\n", encoding="utf-8")
    temporary.replace(path)


def check_evidence(scenario: str, evidence: dict[str, object]) -> None:
    if evidence.get("scenario") != scenario or evidence.get("passed") is not True:
        raise IntegrationError(f"monitor reported incomplete evidence: {evidence}")
    if scenario == "obstacle":
        required = ("moving_seen", "goal_acknowledged", "obstacle_seen", "safe_replan",
                    "clear_seen", "static_wall_preserved")
    elif scenario == "avoidance":
        required = ("moving_seen", "goal_acknowledged", "obstacle_seen", "safe_replan",
                    "avoidance_goal_succeeded", "avoidance_goal_reached",
                    "avoidance_obstacle_clear", "avoidance_static_clear",
                    "obstacle_persisted_at_goal", "static_wall_seen", "static_wall_preserved")
    else:
        required = ("moving_before_fault", "goal_acknowledged", "nav_failed_after_fault",
                    "no_fresh_odom_after_fault", "final_zero_after_fault",
                    "base_zero_after_fault")
        for key in ("zero_latency_ms", "base_zero_latency_ms"):
            latency = evidence.get(key)
            if (not isinstance(latency, (int, float)) or latency < 0
                    or latency > FAULT_STOP_TIMEOUT_SECONDS * 1000):
                raise IntegrationError(f"stop latency {key} exceeded its bound: {latency}")
    missing = [key for key in required if evidence.get(key) is not True]
    if missing:
        raise IntegrationError(f"monitor evidence missing assertions {missing}: {evidence}")
    if scenario == "obstacle" and evidence.get("return_cell") != 100:
        raise IntegrationError(f"higher return did not create its expected hit cell: {evidence}")
    if scenario == "obstacle" and evidence.get("static_wall_seen") is not True:
        raise IntegrationError(f"fused map never contained the static wall: {evidence}")


def run_scenario(scenario: str, build_dir: Path, composer: Path,
                 output_dir: Path) -> dict[str, object]:
    bridge_binary = executable(build_dir, "rbt_navigation_lio_bridge")
    fixture_binary = executable(build_dir, "rbt_navigation_lio_replay_fixture")
    runtime_binary = executable(build_dir, "rbt_navigation_runtime")
    guard_binary = executable(build_dir, "rbt_velocity_guard")
    base_binary = executable(build_dir, "rbt_navigation_base_sim")

    nonce = uuid.uuid4().hex
    prefix = f"rbt/navigation/lio-test-{nonce}"
    base_prefix = prefix + "/base"
    static_prefix = f"rbt/navigation/lio-static-{nonce}"
    map_id = str(uuid.uuid4())
    odom_id = str(uuid.uuid4())
    scenario_dir = output_dir / f"{scenario}-{nonce}"
    scenario_dir.mkdir(parents=True, exist_ok=True)
    logs = {
        name: scenario_dir / f"{name}.log"
        for name in ("composer", "base", "guard", "runtime", "source", "bridge", "monitor")
    }
    evidence_path = scenario_dir / "evidence.json"
    control_path = scenario_dir / "control.txt"
    write_control(control_path, "valid")
    composer_addr = free_address()
    env = os.environ.copy()
    env["RBT_COMPOSER_ADDR"] = composer_addr
    desired_topic = prefix + "/desired_cmd_vel"
    final_topic = "/" + prefix + "/cmd_vel"
    record: dict[str, object] = {
        "scenario": scenario,
        "prefix": prefix,
        "base_prefix": base_prefix,
        "static_prefix": static_prefix,
        "tf_map": map_id,
        "tf_odom": odom_id,
        "composer_address": composer_addr,
        "desired_topic": desired_topic,
        "cmd_vel_topic": final_topic,
        "passed": False,
        "evidence_file": str(evidence_path),
        "logs": {name: str(path) for name, path in logs.items()},
    }
    processes: list[tuple[str, subprocess.Popen[bytes]]] = []
    pending_error: Exception | None = None
    forced_fault: str | None = None
    scenario_started = time.monotonic()
    scenario_deadline = scenario_started + SCENARIO_TIMEOUT_SECONDS

    def start(name: str, command: list[str]) -> subprocess.Popen[bytes]:
        with logs[name].open("wb") as log_file:
            process = subprocess.Popen(command, cwd=REPO_ROOT, env=env,
                                       stdout=log_file, stderr=subprocess.STDOUT,
                                       close_fds=True, start_new_session=True)
        processes.append((name, process))
        return process

    try:
        composer_process = start("composer", [str(composer)])
        startup_deadline = min(scenario_deadline, time.monotonic() + STARTUP_TIMEOUT_SECONDS)
        wait_for_composer(composer_addr, composer_process, startup_deadline, logs["composer"])

        base = start("base", [
            str(base_binary), "--prefix", base_prefix, "--cmd-vel-topic", final_topic,
            "--guard-source-id", "100", "--base-timeout-ms", "250",
            "--estop-source", "2", "--initial-x", "1", "--initial-y", "1",
            "--initial-yaw", "0",
        ])
        wait_for_log(base, logs["base"], "base controller SIMULATOR ready", startup_deadline)

        guard = start("guard", [
            str(guard_binary), "--prefix", prefix, "--desired-topic", desired_topic,
            "--cmd-vel-topic", final_topic, "--source-id", "1", "--guard-source-id", "100",
            "--estop-source", "2", "--guard-timeout-ms", "250", "--cmd-ttl-ms", "100",
        ])
        wait_for_log(guard, logs["guard"], "velocity guard ready", startup_deadline)

        runtime = start("runtime", [
            str(runtime_binary), "--prefix", prefix, "--tf-map", map_id,
            "--tf-odom", odom_id, "--source-id", "1", "--control-hz", "50",
            "--map-hz", "10", "--input-timeout-ms", "250", "--map-timeout-ms", "2000",
            "--plan-timeout-ms", "500", "--controller", "rpp-dwpp",
            # Reserve room for RPP's curved tracking of discrete A* corners.
            # The simulator's physical body and collision check stay 0.18 m.
            "--robot-geometry", "circle", "--robot-radius", "0.18", "--planning-clearance", "0.30",
            "--desired-topic", desired_topic,
        ])
        wait_for_log(runtime, logs["runtime"], "navigation runtime ready", startup_deadline)

        source_provider = "other-lio" if scenario == "wrong_provider" else "super-lio"
        source = start("source", [
            str(fixture_binary), "--role", "provider", "--scenario", scenario,
            "--provider-id", source_provider, "--prefix", prefix,
            "--base-prefix", base_prefix,
            "--static-prefix", static_prefix, "--control-file", str(control_path),
        ])
        wait_for_log(source, logs["source"], "LIO_SOURCE_ACTIVATED", startup_deadline)

        bridge = start("bridge", [
            str(bridge_binary), "--prefix", prefix, "--static-prefix", static_prefix,
            "--provider", "super-lio", "--tf-map", map_id, "--tf-odom", odom_id,
            "--max-frame-age-ms", "250",
        ])

        if scenario == "wrong_provider":
            wrong_provider_deadline = min(scenario_deadline, time.monotonic() + 3.0)
            def is_active_or_failed(row: dict[str, object]) -> bool:
                snapshot = row.get("snapshot")
                state = snapshot.get("state") if isinstance(snapshot, dict) else None
                return state == "Active" or isinstance(state, dict) and "Failed" in state

            component = wait_for_component(
                composer_addr, "rbt-navigation-lio-bridge", is_active_or_failed,
                wrong_provider_deadline,
            )
            source_log = logs["source"].read_text(errors="replace")
            bridge_log = logs["bridge"].read_text(errors="replace") if logs["bridge"].exists() else ""
            if "provider=other-lio" not in source_log:
                raise IntegrationError("wrong-provider source did not activate with its requested identity")
            if BRIDGE_READY in bridge_log:
                raise IntegrationError("wrong-provider bridge emitted its ready marker")
            expected_error = "resolved LIO provider differs from --provider"
            state = component.get("snapshot", {}).get("state")
            if not isinstance(state, dict) or state.get("Failed") != expected_error:
                raise IntegrationError(f"wrong-provider bridge did not fail activation as expected: {state}")
            if bridge.poll() is not None:
                raise IntegrationError(f"bridge process exited instead of reporting activation failure: {bridge.returncode}")
            record["wrong_provider_rejected"] = True
            record["source_provider"] = source_provider
            record["bridge_activation_failure"] = state["Failed"]
            record["passed"] = True
        else:
            wait_for_log(bridge, logs["bridge"], BRIDGE_READY, startup_deadline)
            def is_active(row: dict[str, object]) -> bool:
                snapshot = row.get("snapshot")
                return isinstance(snapshot, dict) and snapshot.get("state") == "Active"

            bridge_component = wait_for_component(
                composer_addr, "rbt-navigation-lio-bridge", is_active,
                startup_deadline,
            )
            requirements = bridge_component.get("snapshot", {}).get("requirements")
            frame_requirement = next((item for item in requirements if
                isinstance(item, dict)
                and item.get("requirement", {}).get("requirement_name") == "lio_frame"), None) \
                if isinstance(requirements, list) else None
            binding = frame_requirement.get("binding") if isinstance(frame_requirement, dict) else None
            if not isinstance(binding, dict) or binding.get("provider") != "super-lio":
                raise IntegrationError(f"bridge did not bind its LIO frame to super-lio: {binding}")
            record["lio_binding_provider"] = binding["provider"]
            monitor = start("monitor", [
                str(fixture_binary), "--role", "monitor", "--scenario", scenario,
                "--prefix", prefix, "--base-prefix", base_prefix,
                "--control-file", str(control_path),
                "--evidence-file", str(evidence_path),
            ])

            if scenario in ("source_crash", "bridge_crash"):
                while time.monotonic() < scenario_deadline:
                    evidence = read_json(evidence_path)
                    if evidence and evidence.get("moving_seen") is True \
                            and evidence.get("goal_acknowledged") is True:
                        if evidence.get("moving_before_fault") is not True:
                            raise IntegrationError("fault test never confirmed pre-fault motion")
                        forced_fault = "source" if scenario == "source_crash" else "bridge"
                        write_control(control_path, scenario)
                        target = source if forced_fault == "source" else bridge
                        stop_process(target, force=True)
                        break
                    if monitor.poll() is not None:
                        raise IntegrationError(f"monitor exited before the fault: {monitor.returncode}")
                    time.sleep(0.03)
                if forced_fault is None:
                    raise IntegrationError("no motion was observed before the process-crash fault")

            remaining = scenario_deadline - time.monotonic()
            if remaining <= 0:
                raise IntegrationError("scenario startup consumed its time budget")
            try:
                monitor_exit = monitor.wait(timeout=remaining)
            except subprocess.TimeoutExpired as error:
                raise IntegrationError(
                    f"scenario exceeded its {SCENARIO_TIMEOUT_SECONDS:.0f}-second budget"
                ) from error
            evidence = read_json(evidence_path)
            record["monitor_exit_code"] = monitor_exit
            record["evidence"] = evidence
            if monitor_exit != 0 or evidence is None:
                raise IntegrationError(f"monitor failed; see {logs['monitor']}")
            check_evidence(scenario, evidence)
            record["passed"] = True

    except Exception as error:
        pending_error = error
        record["error"] = str(error)
        record["evidence"] = read_json(evidence_path)
    finally:
        exit_codes: dict[str, int | None] = {}
        for name, process in reversed(processes):
            try:
                exit_codes[name] = stop_process(process)
            except IntegrationError as error:
                record["cleanup_error"] = str(error)
        record["process_exit_codes"] = exit_codes
        record["forced_fault"] = forced_fault
        for name, log_path in logs.items():
            record[f"{name}_output"] = log_path.read_text(errors="replace").strip() \
                if log_path.exists() else ""

    if pending_error is not None:
        record["passed"] = False
        raise ScenarioFailure(str(pending_error), record) from pending_error
    if "cleanup_error" in record:
        record["passed"] = False
        error = str(record["cleanup_error"])
        raise ScenarioFailure(error, record)
    if forced_fault is not None and exit_codes.get(forced_fault) != -signal.SIGKILL:
        record["passed"] = False
        error = f"{forced_fault} fault process did not exit by SIGKILL: {exit_codes.get(forced_fault)}"
        record["error"] = error
        raise ScenarioFailure(error, record)
    return record


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True, type=Path,
                        help="CMake build directory containing the LIO/native executables")
    parser.add_argument("--composer", type=Path,
                        help="Composer executable (default: target/debug/composer or PATH)")
    parser.add_argument("--output-dir", type=Path,
                        help="directory for per-process logs and results.json")
    parser.add_argument("--scenario", choices=("all", *SCENARIOS), default="all",
                        help="scenario to run (default: all)")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    build_dir = args.build_dir.expanduser().resolve()
    try:
        composer = composer_executable(args.composer)
    except IntegrationError as error:
        print(f"FAIL LIO integration: {error}", file=sys.stderr)
        return 2
    output_dir = (args.output_dir.expanduser().resolve() if args.output_dir
                  else build_dir / "lio-integration-logs")
    output_dir.mkdir(parents=True, exist_ok=True)
    scenarios = SCENARIOS if args.scenario == "all" else (args.scenario,)
    results: list[dict[str, object]] = []
    failure: str | None = None
    for scenario in scenarios:
        try:
            result = run_scenario(scenario, build_dir, composer, output_dir)
            results.append(result)
            print(f"PASS LIO integration {scenario}")
        except Exception as error:
            failure = f"FAIL LIO integration {scenario}: {error}"
            print(failure, file=sys.stderr)
            if isinstance(error, ScenarioFailure):
                results.append(error.result)
            else:
                results.append({"scenario": scenario, "passed": False, "error": str(error)})
            break
    results_path = (output_dir / "results.json" if args.scenario == "all" else
                    output_dir / f"results-{args.scenario}-{uuid.uuid4().hex}.json")
    results_path.write_text(json.dumps({
        "passed": failure is None and len(results) == len(scenarios),
        "composer": str(composer),
        "results": results,
    }, indent=2) + "\n", encoding="utf-8")
    print(f"results: {results_path}")
    return 1 if failure else 0


if __name__ == "__main__":
    raise SystemExit(main())

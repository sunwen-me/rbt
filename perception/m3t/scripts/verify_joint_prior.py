#!/usr/bin/env python3
"""Verify the native SO101 joint-prior path with the SOFTWARE_FIXTURE."""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import signal
import socket
import subprocess
import time
import uuid

from verify_software import fixture_config, json_events, runtime_environment, start, wait_until


TIMEOUT_SECONDS = 140.0
TRACKING_COUNT = 55
MINIMUM_APPLIED_FRAMES = 10
STALE_TAIL_FRAMES = 5
MAXIMUM_PRIOR_ANGLE_ERROR_RAD = 0.15
CONTROLLER_ID = "m3t-joint-prior-verification"


def process_group_members(pgid: int) -> list[int]:
    proc_root = Path("/proc")
    members = []
    for entry in proc_root.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text(encoding="ascii")
            fields = stat[stat.rfind(")") + 1:].split()
            if int(fields[2]) == pgid:
                members.append(int(entry.name))
        except (OSError, ValueError, IndexError):
            continue
    return members


def stop_owned_processes(processes: list[subprocess.Popen]) -> dict:
    for process in reversed(processes):
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        for process in processes:
            if process.poll() is None:
                try:
                    process.wait(timeout=0.05)
                except subprocess.TimeoutExpired:
                    pass
        if all(not process_group_members(process.pid) for process in processes):
            break
        time.sleep(0.05)

    forced_kill = []
    for process in reversed(processes):
        if process_group_members(process.pid):
            forced_kill.append(str(process.args[0]))
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
    for process in reversed(processes):
        if process.poll() is None:
            try:
                process.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                pass

    deadline = time.monotonic() + 2.0
    while time.monotonic() < deadline and any(
            process_group_members(process.pid) for process in processes):
        time.sleep(0.05)
    remaining = {
        str(process.pid): process_group_members(process.pid)
        for process in processes if process_group_members(process.pid)
    }
    return {
        "direct_children_reaped": all(process.poll() is not None for process in processes),
        "forced_kill": forced_kill,
        "remaining_process_group_members": remaining,
        "process_groups_empty": not remaining,
    }


def finite_json(value, description: str = "tracking output") -> None:
    if isinstance(value, dict):
        for child in value.values():
            finite_json(child, description)
    elif isinstance(value, list):
        for child in value:
            finite_json(child, description)
    elif isinstance(value, (int, float)) and not isinstance(value, bool):
        if not math.isfinite(float(value)):
            raise RuntimeError(f"{description} contains a non-finite number")


def estimates_from(log_path: Path) -> list[dict]:
    return [event for event in json_events(log_path) if event.get("event") == "estimate"]


def configure_joint_priors(config_path: Path, service_name: str) -> dict:
    tracking = json.loads(config_path.read_text(encoding="utf-8"))
    mapping = {
        "joint": "shoulder",
        "source_joint": "shoulder_pan",
        "scale_rad_per_unit": math.pi / 180.0,
        "offset_rad": 0.0,
        "standard_deviation_rad": 0.01,
    }
    tracking["joint_priors"] = {
        "enabled": True,
        "service_name": service_name,
        "controller_id": CONTROLLER_ID,
        "time_basis": "received",
        "required": False,
        "initialize_from_measurement": True,
        "maximum_age_ms": 100,
        "mappings": [
            {**mapping, "arm": "so101_left", "source_arm": "left"},
            {**mapping, "arm": "so101_right", "source_arm": "right"},
        ],
    }
    config_path.write_text(json.dumps(tracking, indent=2) + "\n", encoding="utf-8")
    return tracking["joint_priors"]


def verify_estimates(estimates: list[dict], expected_prior: dict,
                     stop_index: int, expected_session: str | None) -> dict:
    if len(estimates) < TRACKING_COUNT:
        raise RuntimeError(f"expected at least {TRACKING_COUNT} tracking estimates, received {len(estimates)}")
    for estimate in estimates:
        finite_json(estimate)
        if estimate.get("event") != "estimate":
            raise RuntimeError("tracker emitted a malformed estimate event")
        angles = estimate.get("joint_angles_rad")
        if not isinstance(angles, dict) or set(angles) != {"so101_left", "so101_right"}:
            raise RuntimeError("each estimate must include both fixture arms")
        for arm in ("so101_left", "so101_right"):
            joints = angles.get(arm)
            if not isinstance(joints, dict) or set(joints) != {"shoulder"}:
                raise RuntimeError(f"{arm} estimate must include only its shoulder")
            angle = joints["shoulder"]
            if isinstance(angle, bool) or not isinstance(angle, (int, float)) \
                    or not -0.8 <= angle <= 0.8:
                raise RuntimeError(f"{arm} shoulder estimate is outside finite model limits")

    reports = [estimate.get("joint_priors") for estimate in estimates]
    if any(not isinstance(report, dict) for report in reports):
        raise RuntimeError("joint-prior diagnostics are missing from a tracking estimate")
    expected_pairs = {
        (mapping["arm"], mapping["joint"])
        for mapping in expected_prior["mappings"]
    }
    applied = [report for report in reports if report.get("status") == "applied"]
    if len(applied) < MINIMUM_APPLIED_FRAMES:
        raise RuntimeError(f"expected at least {MINIMUM_APPLIED_FRAMES} applied tracking frames, received {len(applied)}")

    session_ids = set()
    applied_sequences = []
    angle_errors = {"so101_left": [], "so101_right": []}
    for estimate, report in zip(estimates, reports):
        if report.get("enabled") is not True or report.get("time_basis") != "received" \
                or report.get("capture_time_associated") is not False:
            raise RuntimeError("joint-prior report does not show receipt-time best effort association")
        if report.get("status") in {"applied", "stale"}:
            if report.get("controller_id") != CONTROLLER_ID:
                raise RuntimeError("joint-prior report has the wrong SO101 controller id")
            session = report.get("driver_session")
            try:
                parsed_session = uuid.UUID(session)
            except (ValueError, TypeError, AttributeError) as error:
                raise RuntimeError("joint-prior report driver session is not a UUID") from error
            if str(parsed_session) != session:
                raise RuntimeError("joint-prior report driver session UUID is not canonical")
            session_ids.add(session)

        count = report.get("prior_count")
        priors = report.get("priors")
        if isinstance(count, bool) or not isinstance(count, int) or not isinstance(priors, list) \
                or count != len(priors):
            raise RuntimeError("joint-prior count does not match its factor list")
        if report.get("status") == "applied":
            age_ns = report.get("age_ns")
            if count != 2 or isinstance(age_ns, bool) or not isinstance(age_ns, int) \
                    or not 0 <= age_ns <= 100_000_000:
                raise RuntimeError("applied joint-prior report has wrong factor count or age")
            prior_by_arm = {}
            observed_pairs = set()
            for prior in priors:
                arm = prior.get("arm")
                if arm not in {"so101_left", "so101_right"} or prior.get("joint") != "shoulder" \
                        or arm in prior_by_arm:
                    raise RuntimeError("joint-prior report contains an unconfigured mapping")
                prior_angle = prior.get("angle_rad")
                sigma = prior.get("standard_deviation_rad")
                if isinstance(prior_angle, bool) or not isinstance(prior_angle, (int, float)) \
                        or not -0.8 <= prior_angle <= 0.8 \
                        or isinstance(sigma, bool) or not isinstance(sigma, (int, float)) \
                        or sigma != 0.01:
                    raise RuntimeError("joint-prior factor has invalid sigma or model range")
                prior_by_arm[arm] = prior_angle
                observed_pairs.add((arm, prior["joint"]))
            if set(prior_by_arm) != {"so101_left", "so101_right"}:
                raise RuntimeError("joint-prior report omitted a configured arm mapping")
            if observed_pairs != expected_pairs:
                raise RuntimeError("joint-prior factors do not match the configured partial mappings")
            if len(angle_errors["so101_left"]) < MINIMUM_APPLIED_FRAMES:
                for arm, prior_angle in prior_by_arm.items():
                    angle_errors[arm].append(
                        abs(estimate["joint_angles_rad"][arm]["shoulder"] - prior_angle))
            sequence = report.get("sequence")
            if isinstance(sequence, bool) or not isinstance(sequence, int) or sequence < 1:
                raise RuntimeError("applied prior report has an invalid source sequence")
            applied_sequences.append(sequence)
        elif count != 0 or priors:
            raise RuntimeError("a non-applied joint-prior report retained factors")

    if len(session_ids) != 1:
        raise RuntimeError("fixture driver_session did not remain constant for the run")
    session = next(iter(session_ids))
    if expected_session is not None and session != expected_session:
        raise RuntimeError("fixture driver_session differs from the recorded run UUID")
    if applied_sequences != sorted(applied_sequences):
        raise RuntimeError("applied joint-prior source sequence moved backwards")
    for arm, errors in angle_errors.items():
        if len(errors) < MINIMUM_APPLIED_FRAMES \
                or max(errors[:MINIMUM_APPLIED_FRAMES]) > MAXIMUM_PRIOR_ANGLE_ERROR_RAD:
            raise RuntimeError(f"{arm} tracking estimates did not follow the applied prior within "
                               f"{MAXIMUM_PRIOR_ANGLE_ERROR_RAD:.2f} rad")

    tail = estimates[-STALE_TAIL_FRAMES:]
    if len(tail) != STALE_TAIL_FRAMES or any(
            estimate["joint_priors"].get("status") != "stale"
            or estimate["joint_priors"].get("prior_count") != 0
            or estimate["joint_priors"].get("priors") != []
            for estimate in tail):
        raise RuntimeError("final post-stop tracking frames retained a prior instead of reporting stale")
    if stop_index >= len(estimates) - STALE_TAIL_FRAMES:
        raise RuntimeError("tracker stopped before producing enough post-stop stale frames")

    return {
        "tracking_frames": len(estimates),
        "applied_frames": len(applied),
        "post_stop_stale_tail_frames": len(tail),
        "controller_id": CONTROLLER_ID,
        "driver_session": session,
        "first_applied_sequence": applied_sequences[0],
        "last_applied_sequence": applied_sequences[-1],
        "maximum_first_phase_angle_error_rad": {
            arm: max(errors[:MINIMUM_APPLIED_FRAMES]) for arm, errors in angle_errors.items()
        },
        "maximum_age_ns": max(report["age_ns"] for report in applied if report.get("age_ns") is not None),
    }


def main() -> int:
    started_at = time.monotonic()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--composer", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True,
                        help="new directory for fixture inputs, logs, and result.json")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    composer = args.composer.resolve()
    output = args.output_dir.resolve()
    if not build.is_dir() or not composer.is_file():
        parser.error("--build-dir and --composer must identify existing paths")
    fixture_binary = build / "rbt_m3t_rgbd_fixture"
    tracker_binary = build / "rbt-m3t"
    missing = [str(path) for path in (fixture_binary, tracker_binary) if not path.is_file()]
    if missing:
        parser.error("missing build outputs: " + ", ".join(missing))
    output.mkdir(parents=True, exist_ok=False)

    run_uuid = str(uuid.uuid4())
    service_name = f"joint-prior-verification-{run_uuid[:8]}"
    stop_file = output / "stop_joint_priors"
    result = {
        "status": "FAIL",
        "mode": "SOFTWARE_FIXTURE",
        "hardware": "NOT RUN",
        "configuration": None,
        "checks": [],
        "processes": [],
        "cleanup": {},
        "logs": {
            "composer": str(output / "composer.log"),
            "rgbd_fixture": str(output / "rgbd_fixture.log"),
            "tracker": str(output / "tracker.log"),
        },
        "forced_kill": [],
    }
    deadline = started_at + TIMEOUT_SECONDS
    processes: list[subprocess.Popen] = []
    handles = []
    environment = runtime_environment("software")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    environment["RBT_COMPOSER_ADDR"] = f"127.0.0.1:{port}"

    try:
        config = fixture_config(output / "fixture", "baseline")
        joint_config = configure_joint_priors(config, service_name)
        result["configuration"] = {
            "tracking_config": str(config),
            "stop_file": str(stop_file),
            "service_name": service_name,
            "controller_id": CONTROLLER_ID,
            "mappings": joint_config["mappings"],
            "maximum_age_ms": joint_config["maximum_age_ms"],
        }
        result["checks"].append({"name": "generated_partial_mapping_fixture", "status": "PASS"})

        composer_process = start("composer", [str(composer)], environment, output, processes, handles)
        result["processes"].append({"name": "composer", "pid": composer_process.pid,
                                     "pgid": composer_process.pid, "argv": composer_process.args})

        def composer_ready() -> bool:
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    return True
            except OSError:
                return False

        wait_until("waiting for Composer", min(deadline, time.monotonic() + 10.0),
                   composer_ready, processes)
        result["checks"].append({"name": "composer_started", "status": "PASS"})

        fixture_process = start("rgbd_fixture",
            [str(fixture_binary), "--config", str(config), "--joint-prior-stop-file", str(stop_file)],
            environment, output, processes, handles)
        result["processes"].append({"name": "rgbd_fixture", "pid": fixture_process.pid,
                                     "pgid": fixture_process.pid, "argv": fixture_process.args})
        fixture_log = output / "rgbd_fixture.log"
        wait_until("waiting for the rendered RGB-D fixture",
                   min(deadline, time.monotonic() + 20.0),
                   lambda: any(event.get("event") == "fixture_ready"
                               and event.get("mode") == "SOFTWARE_FIXTURE"
                               for event in json_events(fixture_log)), processes)
        result["checks"].append({"name": "native_fixture_provider_ready", "status": "PASS"})

        tracker_process = start("tracker",
            [str(tracker_binary), "run", "--config", str(config), "--provider", "orbbec335",
             "--count", str(TRACKING_COUNT), "--duration-sec", "120"],
            environment, output, processes, handles)
        result["processes"].append({"name": "tracker", "pid": tracker_process.pid,
                                     "pgid": tracker_process.pid, "argv": tracker_process.args})
        tracker_log = output / "tracker.log"
        wait_until("waiting for ten native tracking frames with applied priors",
                   min(deadline, time.monotonic() + 75.0),
                   lambda: sum(event.get("joint_priors", {}).get("status") == "applied"
                               for event in estimates_from(tracker_log)) >= MINIMUM_APPLIED_FRAMES,
                   processes)
        estimates_at_stop = estimates_from(tracker_log)
        applied_at_stop = sum(
            event.get("joint_priors", {}).get("status") == "applied"
            for event in estimates_at_stop)
        if applied_at_stop < MINIMUM_APPLIED_FRAMES:
            raise RuntimeError("applied tracking frame count fell below stop threshold")
        stop_file.touch()
        result["stop_requested_after_applied_frames"] = applied_at_stop
        result["stop_boundary_tracking_frames"] = len(estimates_at_stop)
        result["checks"].append({"name": "fixture_prior_publishing_stopped", "status": "PASS"})

        try:
            tracker_process.wait(timeout=max(0.0, deadline - time.monotonic()))
        except subprocess.TimeoutExpired as error:
            raise TimeoutError("tracker exceeded the bounded SOFTWARE_FIXTURE run deadline") from error
        if tracker_process.returncode != 0:
            raise RuntimeError(f"tracker exited with status {tracker_process.returncode}")
        estimates = estimates_from(tracker_log)
        result["joint_prior_verification"] = verify_estimates(
            estimates, joint_config, len(estimates_at_stop), expected_session=None)
        result["checks"].append({"name": "native_joint_prior_transport_and_estimation", "status": "PASS"})
        result["checks"].append({"name": "stale_prior_retired_from_later_frames", "status": "PASS"})
        result["status"] = "PASS"
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
    finally:
        result["cleanup"] = stop_owned_processes(processes)
        result["forced_kill"] = result["cleanup"]["forced_kill"]
        for handle in handles:
            handle.close()
        result["children_reaped"] = result["cleanup"]["direct_children_reaped"]
        result["elapsed_seconds"] = round(time.monotonic() - started_at, 3)
        result["process_group_pids"] = [process.pid for process in processes]
        if result["status"] == "PASS" and not result["cleanup"]["process_groups_empty"]:
            result["status"] = "FAIL"
            result["error"] = "owned process groups were not empty after cleanup"
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())

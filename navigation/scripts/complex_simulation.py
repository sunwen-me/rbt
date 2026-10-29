#!/usr/bin/env python3
"""Run and render real native IPC navigation in the configured complex maps."""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SCENARIOS = ROOT / "navigation/maps/complex_scenarios.json"
PROCESS_STOP_SECONDS = 2.0
STARTUP_SECONDS = 8.0
ROBOT_RADIUS = 0.18
PLANNING_CLEARANCE = 0.10


class IntegrationError(RuntimeError):
    pass


def executable(build_dir: Path, name: str) -> Path:
    candidates = (build_dir / name, build_dir / "Release" / name,
                  build_dir / "RelWithDebInfo" / name, build_dir / "Debug" / name)
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    raise IntegrationError(f"could not find executable {name} under {build_dir}")


def write_json(path: Path, value: object) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + "\n")
    temporary.replace(path)


def read_json(path: Path) -> dict[str, object]:
    value = json.loads(path.read_text())
    if not isinstance(value, dict):
        raise IntegrationError(f"expected a JSON object in {path}")
    return value


def finite_pose(value: object, label: str) -> list[float]:
    if not isinstance(value, list) or len(value) != 3:
        raise IntegrationError(f"{label} must be [x, y, yaw]")
    pose = [float(component) for component in value]
    if not all(math.isfinite(component) for component in pose):
        raise IntegrationError(f"{label} contains a non-finite coordinate")
    return pose


def load_scenarios(path: Path, selected: str) -> list[dict[str, object]]:
    try:
        document = read_json(path)
    except (OSError, json.JSONDecodeError) as error:
        raise IntegrationError(f"cannot read scenario file {path}: {error}") from error
    entries = document.get("scenarios")
    if not isinstance(entries, list):
        raise IntegrationError(f"{path} must contain a scenarios array")
    scenes: list[dict[str, object]] = []
    seen: set[str] = set()
    for entry in entries:
        if not isinstance(entry, dict):
            raise IntegrationError("each scenario must be a JSON object")
        scene_id = entry.get("id")
        name = entry.get("name")
        map_value = entry.get("map")
        if scene_id not in ("rooms", "warehouse"):
            raise IntegrationError(f"unsupported complex scenario id: {scene_id!r}")
        if scene_id in seen:
            raise IntegrationError(f"duplicate scenario id: {scene_id}")
        seen.add(str(scene_id))
        if not isinstance(name, str) or not name:
            raise IntegrationError(f"scenario {scene_id} must have a non-empty name")
        if not isinstance(map_value, str) or not map_value:
            raise IntegrationError(f"scenario {scene_id} must have a map path")
        start = finite_pose(entry.get("start"), f"{scene_id}.start")
        goal = finite_pose(entry.get("goal"), f"{scene_id}.goal")
        timeout = float(entry.get("goal_timeout_seconds", 0))
        if not math.isfinite(timeout) or timeout <= 0:
            raise IntegrationError(f"scenario {scene_id} has an invalid goal timeout")
        map_file = (path.parent / map_value).resolve()
        if not map_file.is_file():
            raise IntegrationError(f"map for scenario {scene_id} does not exist: {map_file}")
        scenes.append({"id": str(scene_id), "name": name, "map": str(map_file),
                       "start": start, "goal": goal,
                       "goal_timeout_seconds": timeout})
    if selected != "all":
        scenes = [scene for scene in scenes if scene["id"] == selected]
    if not scenes:
        raise IntegrationError(f"no scenarios selected from {path}: {selected}")
    return scenes


def runtime_environment() -> dict[str, str]:
    environment = os.environ.copy()
    extra_libraries = [ROOT / "target/phase4-toolchain/lib",
                       ROOT / "target/map-image-sdk/lib"]
    paths = [str(directory) for directory in extra_libraries if directory.is_dir()]
    if paths:
        existing = environment.get("LD_LIBRARY_PATH", "")
        environment["LD_LIBRARY_PATH"] = os.pathsep.join(paths + ([existing] if existing else []))
    return environment


def read_log(path: Path) -> str:
    try:
        return path.read_text(errors="replace")
    except FileNotFoundError:
        return ""


def assert_children_alive(processes: list[tuple[str, subprocess.Popen[bytes], Path, Path]],
                          phase: str) -> None:
    for name, process, _, _ in processes:
        code = process.poll()
        if code is not None:
            raise IntegrationError(f"{name} exited with code {code} during {phase}")


def wait_for_ready(processes: list[tuple[str, subprocess.Popen[bytes], Path, Path]],
                   name: str, marker: str, deadline: float) -> None:
    process = next(process for child_name, process, _, _ in processes if child_name == name)
    log_path = next(stdout for child_name, _, stdout, _ in processes if child_name == name)
    while time.monotonic() < deadline:
        assert_children_alive(processes, f"waiting for {name} readiness")
        if marker in read_log(log_path):
            return
        time.sleep(0.05)
    raise IntegrationError(f"timed out waiting for {name} readiness marker {marker!r}")


def stop_processes(processes: list[tuple[str, subprocess.Popen[bytes], Path, Path]]) -> str | None:
    for _, process, _, _ in reversed(processes):
        if process.poll() is None:
            try:
                process.terminate()
            except ProcessLookupError:
                pass

    stop_deadline = time.monotonic() + PROCESS_STOP_SECONDS
    while time.monotonic() < stop_deadline:
        if all(process.poll() is not None for _, process, _, _ in processes):
            break
        time.sleep(0.02)

    still_running = [(name, process) for name, process, _, _ in processes
                     if process.poll() is None]
    for _, process in reversed(still_running):
        process.kill()
    for name, process in still_running:
        try:
            process.wait(timeout=0.5)
        except subprocess.TimeoutExpired:
            return f"could not stop child process {name} ({process.pid}) after kill"
    if still_running:
        return "one or more child processes required SIGKILL during cleanup"
    return None


def validate_capture(capture: Path) -> dict[str, object]:
    map_path = capture / "map.json"
    path_path = capture / "path.csv"
    telemetry_path = capture / "telemetry.csv"
    for path in (map_path, path_path, telemetry_path):
        if not path.is_file():
            raise IntegrationError(f"telemetry recorder did not create {path.name}")

    map_data = read_json(map_path)
    width = int(map_data["width"])
    height = int(map_data["height"])
    cells = map_data["cells"]
    if width <= 0 or height <= 0 or not isinstance(cells, list) or len(cells) != width * height:
        raise IntegrationError("recorded occupancy map dimensions do not match its cell payload")

    with path_path.open(newline="") as stream:
        path_rows = list(csv.DictReader(stream))
    if len(path_rows) < 2:
        raise IntegrationError("recorded initial A* path has fewer than two points")

    times: list[float] = []
    final_row: dict[str, str] | None = None
    with telemetry_path.open(newline="") as stream:
        for row in csv.DictReader(stream):
            try:
                times.append(float(row["relative_stamp_seconds"]))
            except (KeyError, ValueError) as error:
                raise IntegrationError("telemetry has an invalid timestamp") from error
            final_row = row
    if len(times) < 50:
        raise IntegrationError(f"only {len(times)} telemetry samples were recorded; at least 50 are required")
    if not all(math.isfinite(stamp) for stamp in times) or any(
            later <= earlier for earlier, later in zip(times, times[1:])):
        raise IntegrationError("telemetry timestamps must be finite and strictly increasing")
    if final_row is None:
        raise IntegrationError("telemetry.csv contains no samples")
    try:
        final_state = int(float(final_row["nav_state"]))
        final_speed = (float(final_row["linear_x"]), float(final_row["angular_z"]))
    except (KeyError, ValueError) as error:
        raise IntegrationError("telemetry final sample is missing navigation or velocity fields") from error
    if final_state != 4:
        raise IntegrationError(f"final navigation state is {final_state}, expected Succeeded (4)")
    if not all(math.isfinite(speed) and abs(speed) <= 1e-3 for speed in final_speed):
        raise IntegrationError(f"final recorded speed is not zero: {final_speed}")
    return {"sample_count": len(times), "path_points": len(path_rows),
            "map_width": width, "map_height": height, "final_navigation_state": final_state,
            "final_velocity": list(final_speed), "recorded_duration_seconds": times[-1] - times[0]}


def scene_number(scene: dict[str, object], key: str, default: float,
                 *, allow_zero: bool = False) -> float:
    value = scene.get(key, default)
    if isinstance(value, bool):
        raise IntegrationError(f"scenario {key} must be a finite number")
    try:
        number = float(value)
    except (TypeError, ValueError) as error:
        raise IntegrationError(f"scenario {key} must be a finite number") from error
    if not math.isfinite(number) or (number < 0 if allow_zero else number <= 0):
        qualifier = "nonnegative" if allow_zero else "positive"
        raise IntegrationError(f"scenario {key} must be finite and {qualifier}")
    return number


def run_case(scene: dict[str, object], build_dir: Path, output_dir: Path) -> dict[str, object]:
    scene_id = str(scene["id"])
    case_dir = output_dir / scene_id
    if case_dir.exists():
        case_dir = output_dir / f"{scene_id}-{uuid.uuid4().hex[:8]}"
    case_dir.mkdir(parents=True)
    capture_dir = case_dir / "capture"
    capture_dir.mkdir()

    runtime_binary = executable(build_dir, "rbt_navigation_runtime")
    guard_binary = executable(build_dir, "rbt_velocity_guard")
    base_binary = executable(build_dir, "rbt_navigation_base_sim")
    map_server_binary = executable(build_dir, "rbt_navigation_map_server")
    recorder_binary = executable(build_dir, "rbt_navigation_telemetry_recorder")
    fixture_binary = executable(build_dir, "rbt_navigation_map_demo_fixture")

    nonce = uuid.uuid4().hex
    prefix = f"rbt/navigation/complex/{scene_id}/{nonce}"
    map_id = str(uuid.uuid4())
    odom_id = str(uuid.uuid4())
    cmd_vel_topic = f"/{prefix}/cmd_vel"
    desired_topic = f"{prefix}/desired_cmd_vel"
    timeout = float(scene["goal_timeout_seconds"])
    start_pose = scene["start"]
    goal_pose = scene["goal"]
    map_file = Path(str(scene["map"]))
    planning_clearance = scene_number(scene, "planning_clearance", PLANNING_CLEARANCE,
                                      allow_zero=True)
    optional_limits = {
        key: scene_number(scene, key, 0.0)
        for key in ("max_linear", "max_angular") if key in scene
    }
    output_paths = {name: case_dir / f"{name}.stdout.log" for name in
                    ("base_controller_sim", "velocity_guard", "runtime", "map_server",
                     "telemetry_recorder", "fixture")}
    error_paths = {name: case_dir / f"{name}.stderr.log" for name in output_paths}
    record: dict[str, object] = {
        "scenario": scene_id, "name": scene["name"], "prefix": prefix,
        "tf_map": map_id, "tf_odom": odom_id, "desired_topic": desired_topic,
        "cmd_vel_topic": cmd_vel_topic, "map_file": str(map_file),
        "start": start_pose, "goal": goal_pose, "goal_timeout_seconds": timeout,
        "robot_radius": ROBOT_RADIUS, "planning_clearance": planning_clearance,
        "passed": False, "case_dir": case_dir.name,
        "capture_dir": "capture", "processes": {}, "logs": {},
    }
    record.update(optional_limits)
    processes: list[tuple[str, subprocess.Popen[bytes], Path, Path]] = []
    error: str | None = None
    started = time.monotonic()
    deadline = started + timeout + 12.0
    startup_deadline = min(deadline, started + STARTUP_SECONDS)

    def start_child(name: str, command: list[str]) -> subprocess.Popen[bytes]:
        stdout_path = output_paths[name]
        stderr_path = error_paths[name]
        with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
            process = subprocess.Popen(command, cwd=ROOT, env=environment,
                                       stdout=stdout, stderr=stderr, close_fds=True)
        processes.append((name, process, stdout_path, stderr_path))
        record["processes"][name] = {"pid": process.pid}
        record["logs"][name] = {"stdout": stdout_path.name, "stderr": stderr_path.name}
        return process

    environment = runtime_environment()
    try:
        base = start_child("base_controller_sim", [
            str(base_binary), "--prefix", prefix, "--cmd-vel-topic", cmd_vel_topic,
            "--guard-source-id", "100", "--base-timeout-ms", "250", "--estop-source", "2",
            "--initial-x", format(start_pose[0], ".17g"), "--initial-y", format(start_pose[1], ".17g"),
            "--initial-yaw", format(start_pose[2], ".17g"),
        ])
        wait_for_ready(processes, "base_controller_sim", "base controller SIMULATOR ready",
                       startup_deadline)

        start_child("velocity_guard", [
            str(guard_binary), "--prefix", prefix, "--desired-topic", desired_topic,
            "--cmd-vel-topic", cmd_vel_topic, "--source-id", "1", "--guard-source-id", "100",
            "--estop-source", "2", "--guard-timeout-ms", "250", "--cmd-ttl-ms", "100",
        ])
        wait_for_ready(processes, "velocity_guard", "velocity guard ready", startup_deadline)

        runtime_command = [
            str(runtime_binary), "--prefix", prefix, "--tf-map", map_id, "--tf-odom", odom_id,
            "--source-id", "1", "--control-hz", "50", "--map-hz", "10",
            "--input-timeout-ms", "250", "--map-timeout-ms", "2000", "--plan-timeout-ms", "500",
            "--controller", "rpp-dwpp", "--desired-topic", desired_topic,
            "--robot-geometry", "circle",
            "--robot-radius", format(ROBOT_RADIUS, ".2f"),
            "--planning-clearance",
            format(planning_clearance, ".2f") if "planning_clearance" not in scene
            else repr(planning_clearance),
        ]
        for scene_key, option in (("max_linear", "--max-linear"),
                                  ("max_angular", "--max-angular")):
            if scene_key in optional_limits:
                runtime_command.extend([option, repr(optional_limits[scene_key])])
        start_child("runtime", runtime_command)
        wait_for_ready(processes, "runtime", "navigation runtime ready", startup_deadline)

        start_child("map_server", [str(map_server_binary), "--map", str(map_file), "--prefix", prefix])
        wait_for_ready(processes, "map_server", "navigation map server ready:", startup_deadline)

        start_child("telemetry_recorder", [
            str(recorder_binary), "--prefix", prefix, "--cmd-vel-topic", cmd_vel_topic,
            "--output", str(capture_dir), "--duration", format(timeout + 15.0, ".17g"),
        ])
        wait_for_ready(processes, "telemetry_recorder", "telemetry recorder ready:", startup_deadline)

        fixture_output = case_dir / "fixture-result.json"
        fixture = start_child("fixture", [
            str(fixture_binary), "--map", str(map_file), "--prefix", prefix,
            "--tf-map", map_id, "--tf-odom", odom_id,
            "--goal-x", format(goal_pose[0], ".17g"), "--goal-y", format(goal_pose[1], ".17g"),
            "--goal-yaw", format(goal_pose[2], ".17g"), "--goal-timeout", format(timeout, ".17g"),
            "--output", str(fixture_output),
        ])
        record["fixture_result"] = fixture_output.name
        wait_for_ready(processes, "fixture", "map demo fixture ready:", startup_deadline)

        fixture_deadline = deadline - 2.0
        while fixture.poll() is None:
            if time.monotonic() >= fixture_deadline:
                raise IntegrationError("case exceeded its wall-clock budget while waiting for fixture")
            for name, process, _, _ in processes:
                if name != "fixture" and process.poll() is not None:
                    raise IntegrationError(f"{name} exited with code {process.returncode} while fixture ran")
            time.sleep(0.05)
        record["fixture_exit_code"] = fixture.returncode
        if fixture.returncode != 0:
            raise IntegrationError(f"map demo fixture exited with code {fixture.returncode}")
        for name, process, _, _ in processes:
            if name != "fixture" and process.poll() is not None:
                raise IntegrationError(f"{name} exited with code {process.returncode} before cleanup")
        fixture_result = read_json(fixture_output)
        if fixture_result.get("passed") is not True:
            raise IntegrationError("map demo fixture result did not report passed=true")
        record["fixture_passed"] = True
        record["fixture"] = fixture_result

        # Stop the recorder with the rest of the children so its signal handler
        # can flush the capture before the CSV validation begins.
        cleanup_error = stop_processes(processes)
        if cleanup_error:
            record["cleanup_error"] = cleanup_error
            raise IntegrationError(cleanup_error)
        capture_summary = validate_capture(capture_dir)
        record["capture"] = capture_summary
        record["passed"] = True
    except Exception as caught:
        error = str(caught)
        record["error"] = error
        print(f"[{scene_id}] failed: {error}", flush=True)
    finally:
        cleanup_error = stop_processes(processes)
        if cleanup_error:
            record["cleanup_error"] = cleanup_error
            record["passed"] = False
        exit_codes: dict[str, int | None] = {}
        for name, process, stdout_path, stderr_path in reversed(processes):
            try:
                code = process.wait(timeout=0.1) if process.poll() is None else process.returncode
            except subprocess.TimeoutExpired:
                code = None
            exit_codes[name] = code
            record["processes"][name].update({
                "exit_code": code,
                "stdout": read_log(stdout_path),
                "stderr": read_log(stderr_path),
            })
        record["process_exit_codes"] = exit_codes
        if record["passed"]:
            unexpected = {name: code for name, code in exit_codes.items() if code != 0}
            if unexpected:
                record["passed"] = False
                record["error"] = f"unexpected child process exit codes: {unexpected}"
        if not record["passed"] and error is None:
            error = str(record.get("error") or record.get("cleanup_error") or "child process failed")
        record["finished_after_seconds"] = time.monotonic() - started
        write_json(case_dir / "result.json", record)

    if record["passed"]:
        print(f"[{scene_id}] native IPC run: PASS ({record['capture']['sample_count']} samples)", flush=True)
    return record


def render_case(case_dir: Path, scene: dict[str, object], fps: int, speed: float) -> dict[str, object]:
    import imageio_ffmpeg
    import matplotlib
    matplotlib.use("Agg")
    from matplotlib import font_manager, pyplot as plt, transforms
    from matplotlib.lines import Line2D
    from matplotlib.patches import Circle, FancyArrowPatch, Patch
    import numpy as np

    result_path = case_dir / "result.json"
    result = read_json(result_path)
    if result.get("passed") is not True or result.get("fixture_passed") is not True:
        raise IntegrationError(f"refusing to render non-passing IPC case: {result_path}")
    if result.get("scenario") != scene.get("id") or result.get("name") != scene.get("name"):
        raise IntegrationError(f"scenario id or name does not match the captured case: {result_path}")
    for field in ("start", "goal"):
        try:
            recorded_pose = [float(value) for value in result[field]]
            configured_pose = [float(value) for value in scene[field]]
        except (KeyError, TypeError, ValueError) as error:
            raise IntegrationError(f"captured or configured {field} pose is invalid") from error
        if len(recorded_pose) != 3 or len(configured_pose) != 3 or any(
                not math.isclose(actual, configured, rel_tol=0.0, abs_tol=1e-9)
                for actual, configured in zip(recorded_pose, configured_pose)):
            raise IntegrationError(f"configured {field} pose does not match the captured case")
    fixture_path = case_dir / str(result.get("fixture_result", "fixture-result.json"))
    fixture_result = read_json(fixture_path)
    if fixture_result.get("passed") is not True:
        raise IntegrationError(f"fixture did not pass: {fixture_path}")

    capture = case_dir / str(result.get("capture_dir", "capture"))
    data = np.atleast_1d(np.genfromtxt(capture / "telemetry.csv", delimiter=",", names=True))
    path = np.atleast_1d(np.genfromtxt(capture / "path.csv", delimiter=",", names=True))
    grid = read_json(capture / "map.json")
    if len(data) < 50 or len(path) < 2:
        raise IntegrationError("recording requires at least 50 telemetry samples and a two-point path")
    sample_times = np.asarray(data["relative_stamp_seconds"], dtype=float)
    if not np.all(np.isfinite(sample_times)) or not np.all(np.diff(sample_times) > 0):
        raise IntegrationError("telemetry timestamps must be finite and strictly increasing")
    if int(data["nav_state"][-1]) != 4:
        raise IntegrationError("recorded navigation did not reach Succeeded")
    final_velocity = (float(data["linear_x"][-1]), float(data["angular_z"][-1]))
    if not all(math.isfinite(value) and abs(value) <= 1e-3 for value in final_velocity):
        raise IntegrationError(f"recorded base did not stop: {final_velocity}")

    width = int(grid["width"])
    height = int(grid["height"])
    resolution = float(grid["resolution"])
    origin_x = float(grid["origin_x"])
    origin_y = float(grid["origin_y"])
    origin_yaw = float(grid["origin_yaw"])
    cells = np.asarray(grid["cells"], dtype=np.int16)
    if width <= 0 or height <= 0 or resolution <= 0 or cells.size != width * height:
        raise IntegrationError("recorded map dimensions, resolution, or cell payload are invalid")
    cells = cells.reshape(height, width)
    image = np.empty((height, width, 3), dtype=np.uint8)
    image[:] = (250, 251, 253)
    image[cells < 0] = (163, 172, 186)
    image[cells >= 50] = (24, 32, 46)

    font_path = Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc")
    if not font_path.exists():
        font_path = Path("/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc")
    if font_path.exists():
        font_manager.fontManager.addfont(str(font_path))
        plt.rcParams["font.family"] = font_manager.FontProperties(fname=str(font_path)).get_name()
    plt.rcParams.update({"axes.unicode_minus": False, "font.size": 10})

    local_x1 = origin_x + width * resolution
    local_y1 = origin_y + height * resolution
    corners = np.array([[origin_x, origin_y], [local_x1, origin_y],
                        [origin_x, local_y1], [local_x1, local_y1]])
    cosine, sine = math.cos(origin_yaw), math.sin(origin_yaw)
    rotation = np.array([[cosine, -sine], [sine, cosine]])
    world_corners = (corners - np.array([origin_x, origin_y])) @ rotation.T
    world_corners += np.array([origin_x, origin_y])
    map_bounds = [float(world_corners[:, 0].min()), float(world_corners[:, 0].max()),
                  float(world_corners[:, 1].min()), float(world_corners[:, 1].max())]
    margin = max(0.3, 0.07 * max(map_bounds[1] - map_bounds[0], map_bounds[3] - map_bounds[2]))
    map_transform = transforms.Affine2D().rotate_around(origin_x, origin_y, origin_yaw)

    times = sample_times - sample_times[0]
    duration = float(times[-1])
    if duration <= 0:
        raise IntegrationError("recording duration must be positive")
    frames = max(2, int(math.ceil(duration * fps / speed)) + 1)
    frame_stamps = np.linspace(sample_times[0], sample_times[-1], frames)
    frame_indices = np.searchsorted(sample_times, frame_stamps, side="left")
    frame_indices = np.clip(frame_indices, 0, len(sample_times) - 1)
    previous_indices = np.maximum(0, frame_indices - 1)
    use_previous = (np.abs(sample_times[previous_indices] - frame_stamps)
                    <= np.abs(sample_times[frame_indices] - frame_stamps))
    frame_indices = np.where(use_previous, previous_indices, frame_indices)

    fig = plt.figure(figsize=(12.8, 8), dpi=100, facecolor="#f4f6fa")
    fig.text(.055, .945, f"RBT 原生 IPC 复杂场景导航 · {scene['name']}",
             fontsize=21, weight="bold", color="#17243b")
    fig.text(.055, .895, "A* 规划 → RPP-DWPP 路径跟踪 → VelocityGuard → 模拟底盘",
             fontsize=11, color="#53657f")
    fig.text(.96, .947, f"实际进程采样 · {speed:g}× 回放", ha="right",
             fontsize=11, color="#53657f")

    ax = fig.add_axes([.055, .145, .61, .69], facecolor="white")
    ax.imshow(image, origin="lower", interpolation="nearest",
              extent=[origin_x, local_x1, origin_y, local_y1],
              transform=map_transform + ax.transData, zorder=1)
    ax.set(xlim=(map_bounds[0] - margin, map_bounds[1] + margin),
           ylim=(map_bounds[2] - margin, map_bounds[3] + margin),
           xlabel="x / m", ylabel="y / m", aspect="equal")
    ax.grid(color="#dfe5ed", linewidth=.6, zorder=0)
    ax.set_axisbelow(True)
    for spine in ax.spines.values():
        spine.set_color("#d2dae5")

    planned, = ax.plot([], [], "--", color="#059b87", lw=1.8, zorder=3)
    trail, = ax.plot([], [], color="#2875d3", lw=2.3, zorder=4)
    start = [float(value) for value in scene["start"]]
    goal = [float(value) for value in scene["goal"]]
    ax.plot(start[0], start[1], marker="o", ms=7, color="#64748b", zorder=6)
    ax.plot(goal[0], goal[1], marker="*", ms=16, color="#e45756", zorder=7)
    ax.annotate("起点", (start[0], start[1]), xytext=(7, 8), textcoords="offset points",
                fontsize=10, color="#53657f")
    ax.annotate("目标", (goal[0], goal[1]), xytext=(7, 8), textcoords="offset points",
                fontsize=10, color="#b44342")
    robot = Circle((start[0], start[1]), ROBOT_RADIUS, facecolor="#f4b942",
                   edgecolor="#a96a10", lw=1.5, zorder=8)
    ax.add_patch(robot)
    heading = FancyArrowPatch(start[:2], (start[0] + .28, start[1]), color="#17243b",
                              mutation_scale=12, lw=1.6, zorder=9)
    ax.add_patch(heading)
    ax.legend(handles=[Line2D([], [], color="#059b87", ls="--", lw=1.8,
                              label="初始 A* 路径（记录器首条规划路径）"),
                       Line2D([], [], color="#2875d3", lw=2.3, label="实际执行轨迹"),
                       Patch(facecolor="#f4b942", edgecolor="#a96a10",
                             label="机器人 footprint · 半径 0.18 m")],
              loc="upper left", framealpha=.96, facecolor="white", edgecolor="#dce3ec",
              fontsize=8.5)

    timer = fig.text(.70, .805, "", fontsize=20, weight="bold", color="#17243b")
    state_text = fig.text(.70, .75, "", fontsize=14, color="#2875d3")
    distance_text = fig.text(.70, .70, "", fontsize=11, color="#53657f")
    speed_axes = [fig.add_axes([.70, .42, .25, .17], facecolor="white"),
                  fig.add_axes([.70, .19, .25, .17], facecolor="white")]
    speed_artists = []
    fields = (("linear_x", "线速度 v / (m/s)", "#2875d3"),
              ("angular_z", "角速度 ω / (rad/s)", "#8a5bd1"))
    for axis, (field, label, color) in zip(speed_axes, fields):
        axis.plot(times, data[field], color="#d8e0ea", lw=1.0)
        marker, = axis.plot([], [], "o", color=color, ms=5)
        values = np.asarray(data[field], dtype=float)
        low = min(float(values.min()), 0.0)
        high = max(float(values.max()), 0.0)
        span = max(high - low, 0.05)
        axis.set(xlim=(0, max(duration, .1)), ylim=(low - .12 * span, high + .12 * span),
                 ylabel=label, xlabel="记录时间 / s")
        axis.tick_params(labelsize=8)
        axis.grid(color="#e9edf3", linewidth=.6)
        for spine in axis.spines.values():
            spine.set_color("#dce3ec")
        speed_artists.append((marker, field))
    fig.text(.055, .055, "来自原生 RBT IPC 的二维运动学仿真采样；位姿、速度、地图与首条规划路径均来自记录文件。",
             fontsize=9, color="#53657f")

    states = {-1: "等待状态", 0: "等待目标", 1: "路径规划", 2: "路径跟踪",
              3: "重新规划", 4: "已到达 · 已停车", 5: "已取消", 6: "导航失败"}
    ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    video_path = case_dir / "navigation.mp4"
    overview_path = case_dir / "overview.png"
    frame_indices_seen: set[int] = set()
    with (case_dir / "encode.log").open("wb") as encode_log:
        encoder = subprocess.Popen([
            ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-f", "rawvideo",
            "-pixel_format", "rgb24", "-video_size", "1280x800", "-framerate", str(fps),
            "-i", "pipe:0", "-an", "-c:v", "libx264", "-preset", "veryfast",
            "-crf", "20", "-pix_fmt", "yuv420p", "-movflags", "+faststart",
            str(video_path)], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=encode_log)
        try:
            for frame, (stamp, sample_index) in enumerate(zip(frame_stamps, frame_indices)):
                row = data[int(sample_index)]
                if float(row["relative_stamp_seconds"]) >= float(path["relative_stamp_seconds"][0]):
                    planned.set_data(path["x"], path["y"])
                else:
                    planned.set_data([], [])
                trail.set_data(data["x"][:sample_index + 1], data["y"][:sample_index + 1])
                robot.center = (float(row["x"]), float(row["y"]))
                heading.set_positions(robot.center,
                    (float(row["x"]) + .28 * math.cos(float(row["yaw"])),
                     float(row["y"]) + .28 * math.sin(float(row["yaw"]))))
                timer.set_text(f"t = {max(0.0, float(stamp - sample_times[0])):5.1f} s")
                nav_state = int(row["nav_state"])
                sample_stopped = (abs(float(row["linear_x"])) + abs(float(row["angular_z"]))
                                  <= 1e-3)
                if nav_state == 4 and not sample_stopped:
                    state_text.set_text("已到达 · 减速中")
                    state_text.set_color("#b7791f")
                else:
                    state_text.set_text(states.get(nav_state, f"状态 {nav_state}"))
                    state_text.set_color("#07865c" if nav_state == 4 else "#2875d3")
                distance = math.hypot(float(row["x"]) - goal[0], float(row["y"]) - goal[1])
                distance_text.set_text(f"距目标 {distance:.2f} m · 物理半径 {ROBOT_RADIUS:.2f} m")
                for marker, field in speed_artists:
                    marker.set_data([times[sample_index]], [float(row[field])])
                fig.canvas.draw()
                rgba = np.asarray(fig.canvas.buffer_rgba())
                if frame == len(frame_stamps) - 1:
                    fig.savefig(overview_path, dpi=120, facecolor=fig.get_facecolor())
                encoder.stdin.write(rgba[:, :, :3].copy().tobytes())
                frame_indices_seen.add(int(sample_index))
            encoder.stdin.close()
            if encoder.wait(timeout=30) != 0:
                raise IntegrationError(f"video encoder failed; see {case_dir / 'encode.log'}")
        finally:
            if encoder.poll() is None:
                encoder.kill()
                encoder.wait()
            plt.close(fig)

    preview_path = case_dir / "preview.gif"
    subprocess.run([ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-i", str(video_path),
                    "-filter_complex",
                    "fps=8,scale=700:-1:flags=lanczos,split[a][b];[a]palettegen[p];[b][p]paletteuse",
                    "-loop", "0", str(preview_path)], check=True)
    metadata = {
        "source": "actual RBT IPC telemetry from a 2D kinematic simulator",
        "scenario": scene["id"], "name": scene["name"], "map_file": result["map_file"],
        "playback_speed": speed, "sample_count": len(data), "path_points": len(path),
        "frames": len(frame_stamps), "fps": fps,
        "video_duration_seconds": len(frame_stamps) / fps,
        "recorded_duration_seconds": duration,
        "position_interpolation": False, "sample_selection": "nearest recorded sample",
        "unique_samples_shown": len(frame_indices_seen),
        "final_pose": [float(data[key][-1]) for key in ("x", "y", "yaw")],
        "final_velocity": list(final_velocity), "navigation_state": int(data["nav_state"][-1]),
        "map_extent": map_bounds, "robot_radius": ROBOT_RADIUS,
        "planning_clearance": float(result.get("planning_clearance", PLANNING_CLEARANCE)),
    }
    if isinstance(result.get("provenance"), dict):
        metadata["provenance"] = result["provenance"]
    write_json(case_dir / "video-metadata.json", metadata)
    result["render_passed"] = True
    result["artifacts"] = ["navigation.mp4", "preview.gif", "overview.png", "video-metadata.json"]
    write_json(result_path, result)
    print(f"[{scene['id']}] render: PASS ({video_path}, {preview_path})", flush=True)
    return metadata


def render_only_cases(output_dir: Path, selected: str) -> list[Path]:
    scene_ids = ("rooms", "warehouse") if selected == "all" else (selected,)
    found: list[Path] = []
    for scene_id in scene_ids:
        candidates = sorted(output_dir.glob(f"{scene_id}*"), key=lambda item: item.stat().st_mtime)
        passing = []
        for candidate in candidates:
            result_path = candidate / "result.json"
            if not result_path.is_file():
                continue
            try:
                result = read_json(result_path)
            except (OSError, json.JSONDecodeError):
                continue
            if result.get("scenario") == scene_id and result.get("passed") is True:
                passing.append(candidate)
        if not passing:
            raise IntegrationError(f"no passing {scene_id} case found under {output_dir}")
        found.extend(passing)
    return found


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "target/navigation-build",
                        help="CMake build directory containing the navigation executables")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "output/navigation-complex",
                        help="directory for per-case logs, captures, and rendered artifacts")
    parser.add_argument("--scenarios", type=Path, default=DEFAULT_SCENARIOS,
                        help="JSON complex-scene configuration")
    parser.add_argument("--scenario", choices=("all", "rooms", "warehouse"), default="all")
    parser.add_argument("--render-only", action="store_true",
                        help="render existing passing case folders under --output-dir")
    parser.add_argument("--no-render", action="store_true",
                        help="run and validate native IPC evidence without importing render packages")
    parser.add_argument("--fps", type=int, default=20, help="MP4 frame rate (default: 20)")
    parser.add_argument("--speed", type=float, default=2.0,
                        help="playback speed multiplier (default: 2)")
    args = parser.parse_args(argv)
    if not 5 <= args.fps <= 60:
        parser.error("fps must be 5..60")
    if not math.isfinite(args.speed) or args.speed <= 0:
        parser.error("speed must be a finite positive number")
    if args.render_only and args.no_render:
        parser.error("--render-only and --no-render cannot be used together")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    output_dir = args.output_dir.expanduser().resolve()
    scenarios_path = args.scenarios.expanduser().resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    try:
        scenes = load_scenarios(scenarios_path, args.scenario)
        summaries: list[dict[str, object]] = []
        if args.render_only:
            scene_by_id = {str(scene["id"]): scene for scene in scenes}
            cases = render_only_cases(output_dir, args.scenario)
            for case_dir in cases:
                result = read_json(case_dir / "result.json")
                scene_id = str(result["scenario"])
                scene = scene_by_id.get(scene_id)
                if scene is None:
                    raise IntegrationError(f"scenario {scene_id} is missing from {scenarios_path}")
                print(f"[{scene_id}] render starting from captured IPC evidence", flush=True)
                metadata = render_case(case_dir, scene, args.fps, args.speed)
                summaries.append({"scenario": scene_id, "case_dir": case_dir.name,
                                  "passed": True, "render_passed": True,
                                  "video_metadata": metadata})
        else:
            for scene in scenes:
                scene_id = str(scene["id"])
                print(f"[{scene_id}] native IPC case starting", flush=True)
                record = run_case(scene, args.build_dir.expanduser().resolve(), output_dir)
                summary = {"scenario": scene_id, "case_dir": record["case_dir"],
                           "passed": bool(record.get("passed")),
                           "process_exit_codes": record.get("process_exit_codes", {})}
                if record.get("passed") and args.no_render:
                    record["render_skipped"] = True
                    write_json(output_dir / str(record["case_dir"]) / "result.json", record)
                    print(f"[{scene_id}] evidence validation: PASS (render skipped)", flush=True)
                elif record.get("passed"):
                    print(f"[{scene_id}] render starting", flush=True)
                    try:
                        metadata = render_case(output_dir / str(record["case_dir"]), scene,
                                               args.fps, args.speed)
                        summary["render_passed"] = True
                        summary["video_metadata"] = metadata
                    except Exception as error:
                        summary["render_passed"] = False
                        summary["render_error"] = str(error)
                        record["render_passed"] = False
                        record["render_error"] = str(error)
                        write_json(output_dir / str(record["case_dir"]) / "result.json", record)
                        print(f"[{scene_id}] render failed: {error}", flush=True)
                summaries.append(summary)

        passed = all(bool(summary.get("passed")) and
                     (args.no_render or bool(summary.get("render_passed", args.render_only)))
                     for summary in summaries)
        output_result = {"passed": passed, "scenario_file": str(scenarios_path),
                         "render_only": args.render_only, "no_render": args.no_render,
                         "fps": args.fps, "speed": args.speed, "cases": summaries}
        write_json(output_dir / "results.json", output_result)
        if passed:
            print(f"Complex navigation cases: PASS ({len(summaries)} case(s))", flush=True)
            return 0
        print("Complex navigation cases: FAIL; see results.json and per-case logs", flush=True)
        return 1
    except Exception as error:
        write_json(output_dir / "results.json", {"passed": False, "error": str(error),
                                                   "scenario_file": str(scenarios_path)})
        print(f"complex simulation failed: {error}", file=sys.stderr, flush=True)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

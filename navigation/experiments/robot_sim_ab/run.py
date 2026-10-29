#!/usr/bin/env python3
"""Run one pursuit controller against an already running Gazebo robot sim."""

from __future__ import annotations

import argparse
import csv
import json
import math
import os
import subprocess
import sys
import threading
import time
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from typing import Any


CONTROL_PERIOD = 0.02
POSE_STALE_SECONDS = 0.25
VELOCITY_WINDOW_SECONDS = 0.10
POST_STOP_SECONDS = 1.0
ERROR_NAMES = (
    "None", "InvalidInput", "MapUnavailable", "StaleMap", "LocalizationUnavailable",
    "StaleTf", "StaleOdometry", "StartOutside", "GoalOutside", "StartOccupied",
    "GoalOccupied", "NoPath", "PlanningTimeout", "Canceled", "EmptyPath",
    "PathDeviation", "Collision", "ControllerFailure", "PlannerFailure",
    "NavigationTimeout", "GoalConflict",
)
CSV_FIELDS = (
    "time", "x", "y", "yaw", "v", "w", "cmd_v", "cmd_w", "error", "reached",
    "wheel_v", "wheel_w", "z", "roll", "pitch",
)


@dataclass(frozen=True)
class Pose:
    stamp: float
    x: float
    y: float
    z: float
    yaw: float
    roll: float
    pitch: float


@dataclass(frozen=True)
class Odom:
    stamp: float
    linear: float
    angular: float


def wrap_angle(angle: float) -> float:
    return math.atan2(math.sin(angle), math.cos(angle))


def stamp_seconds(stamp: Any) -> float:
    nanoseconds = getattr(stamp, "nanosec", getattr(stamp, "nsec", 0))
    return float(stamp.sec) + float(nanoseconds) * 1e-9


def quaternion_rpy(q: Any) -> tuple[float, float, float]:
    sinr = 2.0 * (q.w * q.x + q.y * q.z)
    cosr = 1.0 - 2.0 * (q.x * q.x + q.y * q.y)
    roll = math.atan2(sinr, cosr)

    sinp = 2.0 * (q.w * q.y - q.z * q.x)
    pitch = math.copysign(math.pi / 2.0, sinp) if abs(sinp) >= 1.0 else math.asin(sinp)

    siny = 2.0 * (q.w * q.z + q.x * q.y)
    cosy = 1.0 - 2.0 * (q.y * q.y + q.z * q.z)
    return roll, pitch, math.atan2(siny, cosy)


def short_name(name: str) -> str:
    return name.strip("/").replace("::", "/").split("/")[-1]


def load_path(path_file: Path) -> list[tuple[float, float, float]]:
    with path_file.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        expected = ("index", "x", "y", "body_yaw", "production_pose_free",
                    "production_segment_free")
        if tuple(reader.fieldnames or ()) != expected:
            raise ValueError(f"unexpected path CSV header in {path_file}")
        points = [(float(row["x"]), float(row["y"]), float(row["body_yaw"]))
                  for row in reader]
    if not points:
        raise ValueError(f"path CSV contains no poses: {path_file}")
    if not all(math.isfinite(value) for point in points for value in point):
        raise ValueError(f"path CSV contains a non-finite pose: {path_file}")
    return points


def point_to_path_distance_squared(x: float, y: float,
                                   points: list[tuple[float, float, float]]) -> float:
    if len(points) == 1:
        return (x - points[0][0]) ** 2 + (y - points[0][1]) ** 2
    nearest = math.inf
    for start, end in zip(points, points[1:]):
        dx = end[0] - start[0]
        dy = end[1] - start[1]
        length_squared = dx * dx + dy * dy
        fraction = 0.0 if length_squared == 0.0 else (
            (x - start[0]) * dx + (y - start[1]) * dy
        ) / length_squared
        fraction = min(1.0, max(0.0, fraction))
        px = start[0] + fraction * dx
        py = start[1] + fraction * dy
        nearest = min(nearest, (x - px) ** 2 + (y - py) ** 2)
    return nearest


class Bridge:
    def __init__(self, binary: Path, controller: str, path: Path, map_file: Path,
                 geometry: Path, helper_lib_dir: Path | None, allow_reverse: bool,
                 vp_use_heading_from_path: bool = True):
        env = os.environ.copy()
        if helper_lib_dir is not None:
            existing = env.get("LD_LIBRARY_PATH", "")
            env["LD_LIBRARY_PATH"] = str(helper_lib_dir) + (
                os.pathsep + existing if existing else ""
            )
        self.process = subprocess.Popen(
            [str(binary), "--controller", controller, "--path", str(path), "--map",
             str(map_file), "--geometry", str(geometry), "--allow-reverse",
             "1" if allow_reverse else "0", "--vp-use-heading-from-path",
             "1" if vp_use_heading_from_path else "0"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1, env=env,
        )
        if self.process.stdin is None or self.process.stdout is None:
            raise RuntimeError("could not open controller helper pipes")

    def command(self, now: float, pose: Pose, velocity: tuple[float, float]
                ) -> tuple[int, bool, float, float]:
        assert self.process.stdin is not None and self.process.stdout is not None
        fields = (now, pose.stamp, pose.stamp, pose.x, pose.y, pose.yaw, *velocity)
        self.process.stdin.write(" ".join(format(value, ".17g") for value in fields) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        values = line.split()
        if len(values) != 4:
            raise RuntimeError(f"controller helper returned an invalid response: {line.rstrip()!r}")
        try:
            error_code = int(values[0])
            reached_value = int(values[1])
            linear, angular = float(values[2]), float(values[3])
        except ValueError as cause:
            raise RuntimeError(f"controller helper returned an invalid response: {line.rstrip()!r}") from cause
        if reached_value not in (0, 1) or not math.isfinite(linear) or not math.isfinite(angular):
            raise RuntimeError(f"controller helper returned non-finite output: {line.rstrip()!r}")
        return error_code, bool(reached_value), linear, angular

    def close(self) -> tuple[int | None, str]:
        if self.process.stdin is not None and not self.process.stdin.closed:
            self.process.stdin.close()
        try:
            self.process.wait(timeout=5.0)
        except subprocess.TimeoutExpired:
            self.process.terminate()
            try:
                self.process.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        stderr = self.process.stderr.read() if self.process.stderr is not None else ""
        return self.process.returncode, stderr.strip()


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bin", type=Path, required=True, dest="binary",
                        help="rbt_navigation_pursuit_physics_bridge executable")
    parser.add_argument("--controller", choices=("rpp-dwpp", "vector-pursuit-window"),
                        required=True)
    parser.add_argument("--path", type=Path, required=True)
    parser.add_argument("--map", type=Path, required=True)
    parser.add_argument("--geometry", type=Path, required=True)
    parser.add_argument("--allow-reverse", action="store_true",
                        help="allow reverse motion (disabled by default)")
    parser.add_argument("--vp-use-heading-from-path", choices=("0", "1"), default="1",
                        help="VP: use stored path yaw (1) or derive segment tangent yaw (0)")
    parser.add_argument("--output", type=Path, required=True,
                        help="directory for telemetry.csv and result.json")
    parser.add_argument("--pose-name", default="dual_arm_robot",
                        help="Gazebo model name in dynamic_pose/info")
    parser.add_argument("--timeout", type=float, default=100.0,
                        help="controller run timeout in simulation seconds")
    parser.add_argument("--ready-timeout", type=float, default=60.0,
                        help="wall-clock limit for topic readiness and settling")
    parser.add_argument("--helper-lib-dir", type=Path,
                        help="prepend this directory to LD_LIBRARY_PATH for the helper only")
    return parser


def run(args: argparse.Namespace) -> dict[str, Any]:
    if args.timeout <= 0 or not math.isfinite(args.timeout):
        raise ValueError("--timeout must be a positive finite number")
    if args.ready_timeout <= 0 or not math.isfinite(args.ready_timeout):
        raise ValueError("--ready-timeout must be a positive finite number")
    for name, path in (("bin", args.binary), ("path", args.path), ("map", args.map),
                       ("geometry", args.geometry)):
        if not path.is_file():
            raise ValueError(f"--{name} file does not exist: {path}")
    if args.helper_lib_dir is not None and not args.helper_lib_dir.is_dir():
        raise ValueError(f"--helper-lib-dir is not a directory: {args.helper_lib_dir}")

    args.output.mkdir(parents=True, exist_ok=True)
    path_points = load_path(args.path)

    # Delay simulator imports so --help and basic file checks work outside ROS/Gazebo.
    import rclpy
    from geometry_msgs.msg import TwistStamped
    from nav_msgs.msg import Odometry
    from rosgraph_msgs.msg import Clock as ClockMessage
    from rclpy.parameter import Parameter
    from rclpy.qos import QoSProfile, QoSReliabilityPolicy
    from gz.transport import Node as GzNode
    from gz.msgs.pose_v_pb2 import Pose_V

    state_lock = threading.Lock()
    state: dict[str, Any] = {
        "clock": None,
        "clock_wall": time.monotonic(),
        "pose": None,
        "pose_samples": deque(),
        "odom": None,
        "pose_error": None,
        "initial_pose": None,
    }
    bridge: Bridge | None = None
    bridge_returncode: int | None = None
    bridge_stderr = ""
    status = "runner_error"
    detail: str | None = None
    terminal_error: int | None = None
    terminal_reached = False
    run_start: float | None = None
    terminal_time: float | None = None
    readiness_wall_start = time.monotonic()
    settle_start: float | None = None
    next_tick: float | None = None
    rows: list[dict[str, Any]] = []
    final_clock: float | None = None

    rclpy.init(args=None)
    node = rclpy.create_node(f"robot_sim_ab_runner_{os.getpid()}")
    node.set_parameters([Parameter("use_sim_time", Parameter.Type.BOOL, True)])
    publisher = node.create_publisher(TwistStamped, "/base_controller/cmd_vel", QoSProfile(depth=10))

    def on_clock(message: ClockMessage) -> None:
        stamp = stamp_seconds(message.clock)
        with state_lock:
            previous = state["clock"]
            state["clock"] = stamp
            if previous is None or stamp > previous:
                state["clock_wall"] = time.monotonic()

    def on_odom(message: Odometry) -> None:
        try:
            sample = Odom(
                stamp_seconds(message.header.stamp),
                float(message.twist.twist.linear.x),
                float(message.twist.twist.angular.z),
            )
            with state_lock:
                state["odom"] = sample
        except (AttributeError, TypeError, ValueError):
            pass

    def on_pose(message: Any) -> None:
        try:
            requested = args.pose_name.strip("/")
            poses = list(message.pose)
            matches = [item for item in poses if item.name.strip("/") == requested]
            if not matches:
                wanted_short = short_name(requested)
                matches = [item for item in poses if short_name(item.name) == wanted_short]
            if len(matches) != 1:
                with state_lock:
                    state["pose_error"] = (
                        f"model {args.pose_name!r} matched {len(matches)} Gazebo poses"
                    )
                return
            item = matches[0]
            roll, pitch, yaw = quaternion_rpy(item.orientation)
            header_stamp = getattr(getattr(message, "header", None), "stamp", None)
            if header_stamp is not None:
                stamp = stamp_seconds(header_stamp)
            else:
                with state_lock:
                    stamp = state["clock"]
            if stamp is None:
                return
            position = item.position
            sample = Pose(stamp, float(position.x), float(position.y), float(position.z),
                          yaw, roll, pitch)
            with state_lock:
                current: Pose | None = state["pose"]
                if current is not None and sample.stamp < current.stamp:
                    return
                state["pose"] = sample
                if state["initial_pose"] is None:
                    state["initial_pose"] = sample
                samples: deque[Pose] = state["pose_samples"]
                if not samples or sample.stamp > samples[-1].stamp:
                    samples.append(sample)
                    while len(samples) > 2 and samples[1].stamp < sample.stamp - 0.25:
                        samples.popleft()
                state["pose_error"] = None
        except (AttributeError, TypeError, ValueError, OverflowError) as error:
            with state_lock:
                state["pose_error"] = f"invalid Gazebo pose message: {error}"

    clock_qos = QoSProfile(depth=10, reliability=QoSReliabilityPolicy.BEST_EFFORT)
    node.create_subscription(ClockMessage, "/clock", on_clock, clock_qos)
    node.create_subscription(Odometry, "/base_controller/odom", on_odom, QoSProfile(depth=10))
    gz_node = GzNode()
    if not gz_node.subscribe(Pose_V, "/world/default/dynamic_pose/info", on_pose):
        raise RuntimeError("could not subscribe to /world/default/dynamic_pose/info")

    def publish(linear: float, angular: float, stamp: float) -> None:
        message = TwistStamped()
        whole = math.floor(stamp)
        message.header.stamp.sec = int(whole)
        message.header.stamp.nanosec = int((stamp - whole) * 1e9)
        message.twist.linear.x = float(linear)
        message.twist.angular.z = float(angular)
        publisher.publish(message)

    def estimate_velocity(pose: Pose) -> tuple[float, float]:
        with state_lock:
            samples = list(state["pose_samples"])
        if len(samples) < 2:
            return 0.0, 0.0
        target_stamp = pose.stamp - VELOCITY_WINDOW_SECONDS
        earlier = samples[0]
        for sample in samples:
            if sample.stamp <= target_stamp:
                earlier = sample
            else:
                break
        dt = pose.stamp - earlier.stamp
        if dt <= 1e-6:
            return 0.0, 0.0
        heading = earlier.yaw + 0.5 * wrap_angle(pose.yaw - earlier.yaw)
        dx = pose.x - earlier.x
        dy = pose.y - earlier.y
        linear = (dx * math.cos(heading) + dy * math.sin(heading)) / dt
        angular = wrap_angle(pose.yaw - earlier.yaw) / dt
        return linear, angular

    def append_row(now: float, pose: Pose, velocity: tuple[float, float],
                   cmd: tuple[float, float], error: int, reached: bool) -> None:
        with state_lock:
            odom: Odom | None = state["odom"]
        row_time = now - run_start if run_start is not None else (
            now - settle_start if settle_start is not None else 0.0
        )
        rows.append({
            "time": row_time, "x": pose.x, "y": pose.y, "yaw": pose.yaw,
            "v": velocity[0], "w": velocity[1], "cmd_v": cmd[0], "cmd_w": cmd[1],
            "error": error, "reached": int(reached),
            "wheel_v": odom.linear if odom is not None else "",
            "wheel_w": odom.angular if odom is not None else "",
            "z": pose.z, "roll": pose.roll, "pitch": pose.pitch,
        })

    try:
        while rclpy.ok():
            rclpy.spin_once(node, timeout_sec=0.01)
            now_wall = time.monotonic()
            with state_lock:
                now = state["clock"]
                pose: Pose | None = state["pose"]
                pose_error = state["pose_error"]
                clock_wall = state["clock_wall"]
            if now is None:
                if now_wall - readiness_wall_start > args.ready_timeout:
                    status = "ready_timeout"
                    detail = "no /clock received before readiness timeout"
                    break
                continue
            final_clock = now
            # A stopped clock cannot reach a simulation-time timeout, so fail cleanly.
            if now_wall - clock_wall > args.ready_timeout:
                status = "clock_stalled"
                detail = "simulation clock stopped advancing"
                break
            if pose is None or publisher.get_subscription_count() < 1:
                if now_wall - readiness_wall_start > args.ready_timeout:
                    status = "ready_timeout"
                    detail = pose_error or (
                        "groundtruth pose or cmd_vel subscriber did not become ready"
                    )
                    break
                continue

            if settle_start is None:
                settle_start = now
                next_tick = now
            assert next_tick is not None
            if now + 1e-9 < next_tick:
                continue
            velocity = estimate_velocity(pose)
            pose_age = now - pose.stamp
            if pose_age < 0.0:
                if pose_age < -0.05:
                    status = "runner_error"
                    detail = "Gazebo pose timestamp is ahead of /clock"
                    break
                continue

            if run_start is None:
                publish(0.0, 0.0, now)
                append_row(now, pose, velocity, (0.0, 0.0), 0, False)
                settled = abs(velocity[0]) <= 0.02 and abs(velocity[1]) <= 0.05
                if (now - settle_start >= 2.0 and settled
                        and pose_age <= POSE_STALE_SECONDS):
                    startup_duration = now - settle_start
                    for row in rows:
                        row["time"] = float(row["time"]) - startup_duration
                    run_start = now
                    next_tick = now
                    bridge = Bridge(args.binary, args.controller, args.path, args.map,
                                    args.geometry, args.helper_lib_dir, args.allow_reverse,
                                    args.vp_use_heading_from_path == "1")
                    continue
                if now_wall - readiness_wall_start > args.ready_timeout:
                    status = "ready_timeout"
                    detail = "robot did not settle within the readiness timeout"
                    break
                next_tick = now + CONTROL_PERIOD
                continue

            if terminal_time is not None:
                publish(0.0, 0.0, now)
                append_row(now, pose, velocity, (0.0, 0.0),
                           terminal_error if terminal_error is not None else 0,
                           terminal_reached)
                if now - terminal_time >= POST_STOP_SECONDS:
                    break
                next_tick = now + CONTROL_PERIOD
                continue

            if now - run_start >= args.timeout:
                status = "timeout"
                terminal_time = now
                publish(0.0, 0.0, now)
                append_row(now, pose, velocity, (0.0, 0.0), 0, False)
                next_tick = now + CONTROL_PERIOD
                continue

            if pose_age > POSE_STALE_SECONDS:
                publish(0.0, 0.0, now)
                append_row(now, pose, velocity, (0.0, 0.0), 0, False)
                next_tick = now + CONTROL_PERIOD
                continue

            assert bridge is not None
            error_code, reached, command_v, command_w = bridge.command(now, pose, velocity)
            terminal_error = error_code
            terminal_reached = reached
            if error_code != 0:
                status = "controller_error"
                terminal_time = now
                command_v, command_w = 0.0, 0.0
            elif reached:
                status = "reached"
                terminal_time = now
                command_v, command_w = 0.0, 0.0
            publish(command_v, command_w, now)
            append_row(now, pose, velocity, (command_v, command_w), error_code, reached)
            next_tick = now + CONTROL_PERIOD

        if status == "runner_error" and detail is None:
            status = "interrupted"
            detail = "ROS context stopped"
    except KeyboardInterrupt:
        status = "interrupted"
        detail = "interrupted by user"
    except Exception as error:
        status = "runner_error"
        detail = f"{type(error).__name__}: {error}"
    finally:
        try:
            with state_lock:
                now = state["clock"]
                final_pose: Pose | None = state["pose"]
                initial_pose: Pose | None = state["initial_pose"]
            if now is not None:
                publish(0.0, 0.0, now)
                rclpy.spin_once(node, timeout_sec=0.02)
        except Exception:
            pass
        try:
            if bridge is not None:
                bridge_returncode, bridge_stderr = bridge.close()
        finally:
            node.destroy_node()
            rclpy.shutdown()
        with state_lock:
            final_clock = state["clock"]

    telemetry_file = args.output / "telemetry.csv"
    with telemetry_file.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=CSV_FIELDS)
        writer.writeheader()
        writer.writerows(rows)

    goal = path_points[-1]
    final_errors = None
    rmse = None
    if rows:
        squared_distances = [
            point_to_path_distance_squared(float(row["x"]), float(row["y"]), path_points)
            for row in rows if run_start is not None and float(row["time"]) >= 0.0
        ]
        if squared_distances:
            rmse = math.sqrt(sum(squared_distances) / len(squared_distances))
    if final_pose is not None:
        final_errors = {
            "x_m": goal[0] - final_pose.x,
            "y_m": goal[1] - final_pose.y,
            "position_m": math.hypot(goal[0] - final_pose.x, goal[1] - final_pose.y),
            "yaw_rad": wrap_angle(goal[2] - final_pose.yaw),
        }
    initial_pose_data = None
    initial_offset = None
    if initial_pose is not None:
        initial_pose_data = {
            "x_m": initial_pose.x, "y_m": initial_pose.y, "z_m": initial_pose.z,
            "yaw_rad": initial_pose.yaw,
        }
        initial_offset = math.hypot(path_points[0][0] - initial_pose.x,
                                    path_points[0][1] - initial_pose.y)
    error_name = None
    if terminal_error is not None:
        error_name = ERROR_NAMES[terminal_error] if 0 <= terminal_error < len(ERROR_NAMES) else "Unknown"
    result = {
        "status": status,
        "detail": detail,
        "controller": args.controller,
        "vp_use_heading_from_path": args.vp_use_heading_from_path == "1",
        "controller_error": ({"code": terminal_error, "name": error_name}
                             if terminal_error not in (None, 0) else None),
        "timeout": status == "timeout",
        "sim_start_time": run_start,
        "sim_end_time": final_clock,
        "sim_duration": (final_clock - run_start if run_start is not None and final_clock is not None
                         else None),
        "pose_name": args.pose_name,
        "initial_pose": initial_pose_data,
        "initial_offset_from_path_start_m": initial_offset,
        "samples": len(rows),
        "final_errors": final_errors,
        "xy_point_to_segment_rmse_m": rmse,
        "helper_returncode": bridge_returncode,
        "helper_stderr": bridge_stderr or None,
    }
    with (args.output / "result.json").open("w", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2, sort_keys=True)
        stream.write("\n")
    print(f"status={status} controller={args.controller} samples={len(rows)}")
    return result


def main() -> int:
    args = make_parser().parse_args()
    try:
        result = run(args)
    except Exception as error:
        args.output.mkdir(parents=True, exist_ok=True)
        telemetry_file = args.output / "telemetry.csv"
        if not telemetry_file.exists():
            with telemetry_file.open("w", newline="", encoding="utf-8") as stream:
                csv.DictWriter(stream, fieldnames=CSV_FIELDS).writeheader()
        result = {"status": "runner_error", "detail": f"{type(error).__name__}: {error}",
                  "controller": args.controller, "timeout": False}
        with (args.output / "result.json").open("w", encoding="utf-8") as stream:
            json.dump(result, stream, indent=2, sort_keys=True)
            stream.write("\n")
        print(f"status=runner_error controller={args.controller}: {result['detail']}", file=sys.stderr)
        return 1
    return 0 if result["status"] in ("reached", "timeout", "controller_error") else 1


if __name__ == "__main__":
    raise SystemExit(main())

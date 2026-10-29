#!/usr/bin/env python3
"""Render side-by-side Gazebo telemetry replays and planar geometry metrics."""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

import matplotlib.animation as animation
import matplotlib.pyplot as plt
import numpy as np
import yaml
from matplotlib.colors import ListedColormap
from matplotlib.patches import Polygon

try:
    import imageio_ffmpeg

    matplotlib.rcParams["animation.ffmpeg_path"] = imageio_ffmpeg.get_ffmpeg_exe()
except ImportError:
    pass


SCENARIOS = ("S_curve", "narrow_turn", "reverse_cusp")
MODES = ("rpp-dwpp", "vector-pursuit-window")
MODE_TITLES = {
    "rpp-dwpp": "RPP + DWPP",
    "vector-pursuit-window": "VP + same velocity window",
}
FRAMES_PER_SCENARIO = 125
FPS = 15


def read_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise ValueError(f"empty CSV: {path}")
    return rows


def value(row: dict[str, str], *names: str, required: bool = True, default: float = 0.0) -> float:
    for name in names:
        raw = row.get(name)
        if raw not in (None, ""):
            return float(raw)
    if required:
        raise ValueError(f"missing required telemetry field; expected one of {names}")
    return default


def load_trace(folder: Path) -> dict:
    rows = read_rows(folder / "telemetry.csv")
    with (folder / "result.json").open() as stream:
        result = json.load(stream)
    times = np.array([value(row, "time", "sim_time_seconds") for row in rows])
    poses = np.array([
        [value(row, "x", "actual_x"), value(row, "y", "actual_y"),
         value(row, "yaw", "actual_yaw")]
        for row in rows
    ], dtype=float)
    if len(poses) == 0 or not np.all(np.isfinite(poses)) or not np.all(np.isfinite(times)):
        raise ValueError(f"non-finite or missing pose telemetry in {folder}")
    if np.any(np.diff(times) < 0):
        raise ValueError(f"telemetry time must be monotonic: {folder}")
    velocities = np.array([
        value(row, "v", "actual_linear_x", required=False, default=np.nan) for row in rows
    ])
    reached = [row.get("reached") for row in rows]
    return {"time": times, "poses": poses, "rows": rows, "reached": reached,
            "velocity": velocities, "result": result}


def load_map(path: Path) -> dict:
    rows = read_rows(path)
    first = rows[0]
    width, height = int(first["width"]), int(first["height"])
    resolution = float(first["resolution_m"])
    yaw = float(first["origin_yaw"])
    if resolution <= 0 or abs(yaw) > 1e-9:
        raise ValueError(f"renderer currently expects an axis-aligned map: {path}")
    occupied = np.zeros((height, width), dtype=bool)
    grid_x = grid_y = None
    seen = 0
    for row in rows:
        x, y = int(row["cell_x"]), int(row["cell_y"])
        if not (0 <= x < width and 0 <= y < height):
            raise ValueError(f"grid cell outside declared dimensions in {path}")
        occupied[y, x] = int(row["occupancy"]) >= 50
        if x == 0 and y == 0:
            grid_x, grid_y = float(row["world_x"]), float(row["world_y"])
        seen += 1
    if seen != width * height or grid_x is None or grid_y is None:
        raise ValueError(f"incomplete map grid: {path}")
    return {"occupied": occupied, "width": width, "height": height,
            "resolution": resolution, "x": grid_x, "y": grid_y}


def load_path(path: Path) -> np.ndarray:
    rows = read_rows(path)
    x = np.array([value(row, "x") for row in rows])
    y = np.array([value(row, "y") for row in rows])
    if "body_yaw" in rows[0] or "yaw" in rows[0]:
        yaw = np.array([value(row, "body_yaw", "yaw") for row in rows])
    else:
        dx, dy = np.diff(x), np.diff(y)
        yaw = np.r_[np.arctan2(dy, dx), math.atan2(dy[-1], dx[-1])] if len(x) > 1 else np.zeros(1)
    return np.column_stack((x, y, yaw))


def wrap_angle(angle):
    return (angle + np.pi) % (2 * np.pi) - np.pi


def nearest_path_errors(poses: np.ndarray, path: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Return point-to-segment distances and interpolated nearest-path yaw errors."""
    points = path[:, :2]
    if len(points) == 1:
        return (np.linalg.norm(poses[:, :2] - points[0], axis=1),
                wrap_angle(poses[:, 2] - path[0, 2]))
    starts, deltas = points[:-1], points[1:] - points[:-1]
    lengths2 = np.einsum("ij,ij->i", deltas, deltas)
    valid = lengths2 > 1e-18
    starts, deltas, lengths2 = starts[valid], deltas[valid], lengths2[valid]
    yaw_starts, yaw_deltas = path[:-1, 2][valid], wrap_angle(np.diff(path[:, 2]))[valid]
    if len(starts) == 0:
        return (np.linalg.norm(poses[:, None, :2] - points[0], axis=2)[:, 0],
                wrap_angle(poses[:, 2] - path[0, 2]))

    distances = np.empty(len(poses))
    heading_errors = np.empty(len(poses))
    for first in range(0, len(poses), 256):
        batch = poses[first:first + 256]
        offset = batch[:, None, :2] - starts[None, :, :]
        t = np.clip(np.einsum("bsi,si->bs", offset, deltas) / lengths2, 0, 1)
        nearest = starts[None, :, :] + t[:, :, None] * deltas[None, :, :]
        squared = np.einsum("bsi,bsi->bs", batch[:, None, :2] - nearest,
                            batch[:, None, :2] - nearest)
        indices = np.argmin(squared, axis=1)
        rows = np.arange(len(batch))
        distances[first:first + len(batch)] = np.sqrt(squared[rows, indices])
        nearest_yaw = yaw_starts[indices] + yaw_deltas[indices] * t[rows, indices]
        heading_errors[first:first + len(batch)] = wrap_angle(batch[:, 2] - nearest_yaw)
    return distances, heading_errors


def interpolate_dense(poses: np.ndarray, max_distance: float = 0.01,
                      max_angle: float = 0.02) -> np.ndarray:
    samples = [poses[0]]
    for first, second in zip(poses[:-1], poses[1:]):
        delta_angle = float(wrap_angle(second[2] - first[2]))
        distance = float(np.linalg.norm(second[:2] - first[:2]))
        count = max(1, math.ceil(distance / max_distance), math.ceil(abs(delta_angle) / max_angle))
        for index in range(1, count + 1):
            fraction = index / count
            samples.append(np.array([
                first[0] + (second[0] - first[0]) * fraction,
                first[1] + (second[1] - first[1]) * fraction,
                first[2] + delta_angle * fraction,
            ]))
    return np.asarray(samples)


def transform_footprint(vertices: np.ndarray, pose: np.ndarray) -> np.ndarray:
    c, s = math.cos(float(pose[2])), math.sin(float(pose[2]))
    rotation = np.array([[c, -s], [s, c]])
    return vertices @ rotation.T + pose[:2]


def occupied_rectangles(grid: dict) -> list[tuple[float, float, float, float]]:
    """Merge identical adjacent occupied cell runs into exact rectangles."""
    runs_by_row = {}
    for y, row in enumerate(grid["occupied"]):
        xs = np.flatnonzero(row)
        runs = []
        if len(xs):
            start = previous = int(xs[0])
            for raw_x in xs[1:]:
                x = int(raw_x)
                if x != previous + 1:
                    runs.append((start, previous))
                    start = x
                previous = x
            runs.append((start, previous))
        runs_by_row[y] = tuple(runs)

    active: dict[tuple[int, int], tuple[int, int]] = {}
    rectangles = []
    previous_y = None
    for y, row_runs in runs_by_row.items():
        runs = set(row_runs)
        if previous_y is None or y != previous_y + 1:
            rectangles.extend((x0, x1, y0, y1) for (x0, x1), (y0, y1) in active.items())
            active = {}
        for run in list(active):
            if run not in runs:
                y0, y1 = active.pop(run)
                rectangles.append((run[0], run[1], y0, y1))
        for run in runs:
            active[run] = (active[run][0], y) if run in active else (y, y)
        previous_y = y
    rectangles.extend((x0, x1, y0, y1) for (x0, x1), (y0, y1) in active.items())

    resolution, grid_x, grid_y = grid["resolution"], grid["x"], grid["y"]
    return [
        (grid_x + x0 * resolution, grid_y + y0 * resolution,
         grid_x + (x1 + 1) * resolution, grid_y + (y1 + 1) * resolution)
        for x0, x1, y0, y1 in rectangles
    ]


def point_segment_distance(point: np.ndarray, first: np.ndarray, second: np.ndarray) -> float:
    delta = second - first
    length2 = float(np.dot(delta, delta))
    if length2 <= 1e-18:
        return float(np.linalg.norm(point - first))
    t = float(np.clip(np.dot(point - first, delta) / length2, 0, 1))
    return float(np.linalg.norm(point - (first + t * delta)))


def polygon_rectangle_distance(polygon: np.ndarray, left: float, bottom: float,
                               right: float, top: float) -> tuple[bool, float]:
    rectangle = np.array([[left, bottom], [right, bottom], [right, top], [left, top]])
    axes = [np.array([1.0, 0.0]), np.array([0.0, 1.0])]
    edges = np.roll(polygon, -1, axis=0) - polygon
    axes.extend(np.column_stack((-edges[:, 1], edges[:, 0])))
    for axis in axes:
        p = polygon @ axis
        r = rectangle @ axis
        if p.max() < r.min() - 1e-12 or r.max() < p.min() - 1e-12:
            break
    else:
        return True, 0.0

    minimum = math.inf
    for point in polygon:
        for index in range(4):
            minimum = min(minimum, point_segment_distance(point, rectangle[index],
                                                          rectangle[(index + 1) % 4]))
    for point in rectangle:
        for index in range(len(polygon)):
            minimum = min(minimum, point_segment_distance(point, polygon[index],
                                                          polygon[(index + 1) % len(polygon)]))
    return False, minimum


def footprint_metrics(poses: np.ndarray, vertices: np.ndarray, grid: dict) -> dict:
    dense = interpolate_dense(poses)
    obstacles = occupied_rectangles(grid)
    overlaps = []
    min_clearance = math.inf
    for pose in dense:
        polygon = transform_footprint(vertices, pose)
        low_x, low_y = polygon.min(axis=0)
        high_x, high_y = polygon.max(axis=0)
        sample_overlap = False
        for left, bottom, right, top in obstacles:
            dx = max(left - high_x, low_x - right, 0.0)
            dy = max(bottom - high_y, low_y - top, 0.0)
            box_lower_bound = math.hypot(dx, dy)
            if box_lower_bound > 0 and box_lower_bound >= min_clearance:
                continue
            overlap, clearance = polygon_rectangle_distance(polygon, left, bottom, right, top)
            min_clearance = min(min_clearance, clearance)
            if overlap:
                sample_overlap = True
                min_clearance = 0.0
                break
        overlaps.append(sample_overlap)

    intervals, previous = 0, False
    for overlap in overlaps:
        if overlap and not previous:
            intervals += 1
        previous = overlap
    return {
        "dense_pose_samples": len(dense),
        "dense_translation_step_max_m": 0.01,
        "dense_yaw_step_max_rad": 0.02,
        "footprint_overlap_samples": int(sum(overlaps)),
        "footprint_overlap_intervals": intervals,
        "footprint_overlap_detected": any(overlaps),
        "minimum_clearance_m": None if math.isinf(min_clearance) else float(min_clearance),
        "geometry_note": "Planar footprint/grid overlap; it does not establish physical contact or penetration.",
    }


def reached_true(raw: str | None) -> bool | None:
    if raw is None or raw == "":
        return None
    return raw.strip().lower() in {"1", "1.0", "true", "yes"}


def calculate_metrics(trace: dict, path: np.ndarray, grid: dict, footprint: np.ndarray) -> dict:
    poses = trace["poses"]
    velocity = trace["velocity"]
    time_mask = trace["time"] >= 0
    if np.any(np.isfinite(velocity)):
        tracking_mask = time_mask & np.isfinite(velocity) & (np.abs(velocity) > 0.01)
        tracking_policy = "time >= 0 and abs(actual linear velocity) > 0.01 m/s"
    else:
        tracking_mask = time_mask
        tracking_policy = "all samples with time >= 0; linear velocity was not recorded"
    tracked_poses = poses[tracking_mask]
    if len(tracked_poses):
        distances, heading_errors = nearest_path_errors(tracked_poses, path)
        position_rmse = float(np.sqrt(np.mean(distances ** 2)))
        position_max = float(np.max(distances))
        heading_rmse = float(np.sqrt(np.mean(heading_errors ** 2)))
    else:
        position_rmse = position_max = heading_rmse = None
    endpoint_xy = float(np.linalg.norm(poses[-1, :2] - path[-1, :2]))
    endpoint_yaw = float(abs(wrap_angle(poses[-1, 2] - path[-1, 2])))
    reached_values = [reached_true(raw) for raw in trace["reached"]]
    recorded_reached = [value for value in reached_values if value is not None]
    result = trace["result"] if isinstance(trace["result"], dict) else {}
    result_fields = {key: result[key] for key in (
        "outcome", "status", "error_code", "error_name", "error_message",
        "controller_error", "termination_reason", "task_seconds", "vp_use_heading_from_path",
    ) if key in result}
    result = {
        "telemetry_samples": len(poses),
        "duration_seconds": float(trace["time"][-1] - trace["time"][0]),
        "tracking_samples": len(tracked_poses),
        "tracking_sample_policy": tracking_policy,
        "point_to_path_segment_xy_rmse_m": position_rmse,
        "point_to_path_segment_xy_max_m": position_max,
        "nearest_path_heading_rmse_rad": heading_rmse,
        "endpoint_xy_error_m": endpoint_xy,
        "endpoint_yaw_error_rad": endpoint_yaw,
        "reached_samples_true": int(sum(value is True for value in recorded_reached)),
        "reached_samples_recorded": len(recorded_reached),
        "reported_result": result_fields,
    }
    result.update(footprint_metrics(poses, footprint, grid))
    return result


def trace_at(trace: dict, time_point: float) -> tuple[np.ndarray, np.ndarray, int]:
    times, poses = trace["time"], trace["poses"]
    clamped_time = float(np.clip(time_point, times[0], times[-1]))
    index = max(0, min(len(times) - 1,
                       int(np.searchsorted(times, clamped_time, side="right") - 1)))
    shown = poses[:index + 1, :2]
    current = poses[index].copy()
    if index + 1 < len(times) and times[index + 1] > times[index] and clamped_time > times[index]:
        fraction = (clamped_time - times[index]) / (times[index + 1] - times[index])
        next_pose = poses[index + 1]
        dyaw = float(wrap_angle(next_pose[2] - current[2]))
        current[0] += (next_pose[0] - current[0]) * fraction
        current[1] += (next_pose[1] - current[1]) * fraction
        current[2] += dyaw * fraction
        shown = np.vstack((shown, current[:2]))
    return shown, current, index


def playback_state(trace: dict, time_point: float) -> str:
    if time_point < trace["time"][0]:
        return "waiting"
    if time_point > trace["time"][-1]:
        return "held at end"
    return "playing"


def result_label(result: dict) -> str:
    if not isinstance(result, dict):
        return "reported result: not recorded"
    parts = []
    status = result.get("outcome") or result.get("status")
    if status not in (None, ""):
        parts.append(str(status))
    if result.get("error_code") not in (None, ""):
        error = str(result["error_code"])
        name = result.get("error_name")
        if name not in (None, "", "None", "none"):
            error += f" ({name})"
        parts.append(f"error {error}")
    elif result.get("error_name") not in (None, "", "None", "none"):
        parts.append(f"error {result['error_name']}")
    controller_error = result.get("controller_error")
    if isinstance(controller_error, dict):
        code = controller_error.get("code")
        name = controller_error.get("name")
        has_code = code not in (None, "", 0, "0")
        has_name = name not in (None, "", "None", "none")
        if has_code or has_name:
            detail = str(code) if has_code else ""
            if has_name:
                detail = f"{detail} ({name})" if detail else str(name)
            parts.append(f"controller error {detail}")
    return "final recorded result: " + " · ".join(parts) if parts else "final result: not recorded"


def common_time_extent(traces: list[dict]) -> tuple[float, float]:
    return (min(float(trace["time"][0]) for trace in traces),
            max(float(trace["time"][-1]) for trace in traces))


def scenario_bounds(path: np.ndarray, traces: list[dict]) -> tuple[float, float, float, float]:
    points = [path[:, :2]] + [trace["poses"][:, :2] for trace in traces]
    points = np.vstack(points)
    low = points.min(axis=0) - 0.9
    high = points.max(axis=0) + 0.9
    return float(low[0]), float(high[0]), float(low[1]), float(high[1])


def setup_axes(axes, scenario: str, path: np.ndarray, grid: dict,
               traces: list[dict], footprint: np.ndarray, playback_time: float):
    x_min, x_max, y_min, y_max = scenario_bounds(path, traces)
    extent = (grid["x"], grid["x"] + grid["width"] * grid["resolution"],
              grid["y"], grid["y"] + grid["height"] * grid["resolution"])
    colors = ListedColormap(["#f4f5f7", "#41464d"])
    for axis, mode, trace in zip(axes, MODES, traces):
        axis.clear()
        axis.imshow(grid["occupied"].astype(np.uint8), origin="lower", extent=extent,
                    interpolation="nearest", cmap=colors, vmin=0, vmax=1, alpha=0.78,
                    zorder=0, aspect="equal")
        axis.plot(path[:, 0], path[:, 1], color="#2878b5", linewidth=1.5,
                  label="Reference path", zorder=2)
        axis.set_xlim(x_min, x_max)
        axis.set_ylim(y_min, y_max)
        axis.set_aspect("equal", adjustable="box")
        axis.grid(color="#bfc4ca", linewidth=0.35, alpha=0.55)
        axis.set_xlabel("x (m)")
        axis.set_ylabel("y (m)")
        axis.set_title(MODE_TITLES[mode])
        shown, pose, _ = trace_at(trace, playback_time)
        line, = axis.plot(shown[:, 0], shown[:, 1], color="#e36b32", linewidth=1.6,
                          label="Gazebo truth trace", zorder=3)
        polygon = Polygon(transform_footprint(footprint, pose), closed=True,
                          facecolor="#f6bf4f", edgecolor="#3b2c13", alpha=0.7,
                          linewidth=1.0, zorder=4)
        axis.add_patch(polygon)
        heading, = axis.plot(
            [pose[0], pose[0] + 0.15 * math.cos(pose[2])],
            [pose[1], pose[1] + 0.15 * math.sin(pose[2])],
            color="#a32924", linewidth=2, marker="o", markevery=[0],
            markersize=3, zorder=6,
        )
        info = axis.text(0.02, 0.02,
                         f"t = {playback_time:.2f} s · {playback_state(trace, playback_time)}\n"
                         f"{result_label(trace['result'])}",
                         transform=axis.transAxes, fontsize=7.5,
                         bbox={"facecolor": "white", "alpha": 0.85, "edgecolor": "none"},
                         zorder=5)
        axis.legend(loc="upper right", fontsize=8, framealpha=0.9)
        axis._replay_artists = (line, polygon, info, heading)
        axis._replay_trace = trace
    axes[0].figure.suptitle(
        f"Gazebo telemetry replay · {scenario} (shared simulation time, compressed playback)",
        fontsize=14,
    )


def update_axes(axes, playback_time: float):
    for axis in axes:
        trace = axis._replay_trace
        line, polygon, info, heading = axis._replay_artists
        shown, pose, _ = trace_at(trace, playback_time)
        line.set_data(shown[:, 0], shown[:, 1])
        polygon.set_xy(transform_footprint(axis._replay_footprint, pose))
        heading.set_data(
            [pose[0], pose[0] + 0.15 * math.cos(pose[2])],
            [pose[1], pose[1] + 0.15 * math.sin(pose[2])],
        )
        info.set_text(f"t = {playback_time:.2f} s · {playback_state(trace, playback_time)}\n"
                      f"{result_label(trace['result'])}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="Gazebo experiment output root")
    parser.add_argument("--output", type=Path, required=True, help="media output directory")
    args = parser.parse_args()
    source = args.input.resolve()
    output = args.output.resolve()
    fixtures = source / "fixtures"
    with (fixtures / "front-drive-geometry.yaml").open() as stream:
        geometry = yaml.safe_load(stream)
    footprint = np.asarray(geometry["footprint"], dtype=float)
    if footprint.ndim != 2 or footprint.shape[1] != 2 or len(footprint) < 3:
        raise ValueError("front-drive geometry must contain a 2D footprint polygon")

    scenario_data = {}
    metrics = {
        "source_label": "Gazebo telemetry replay",
        "playback_note": "The two panels share scenario simulation time; shorter traces hold their terminal pose.",
        "modes": list(MODES),
        "scenarios": {},
    }
    for scenario in SCENARIOS:
        fixture_dir = fixtures / scenario
        path = load_path(fixture_dir / "path.csv")
        grid = load_map(fixture_dir / "map.csv")
        traces = [load_trace(source / scenario / mode) for mode in MODES]
        mode_metrics = {}
        for mode, trace in zip(MODES, traces):
            mode_metrics[mode] = calculate_metrics(trace, path, grid, footprint)
        scenario_data[scenario] = {"path": path, "grid": grid, "traces": traces}
        metrics["scenarios"][scenario] = mode_metrics

    output.mkdir(parents=True, exist_ok=True)
    (output / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    for scenario, data in scenario_data.items():
        figure, axes = plt.subplots(1, 2, figsize=(13, 5.8), constrained_layout=True)
        _, final_time = common_time_extent(data["traces"])
        setup_axes(axes, scenario, data["path"], data["grid"], data["traces"],
                   footprint, final_time)
        for axis in axes:
            axis._replay_footprint = footprint
        figure.savefig(output / f"{scenario}.png", dpi=150)
        plt.close(figure)

    figure, axes = plt.subplots(1, 2, figsize=(13, 5.8), constrained_layout=True)
    for axis in axes:
        axis._replay_footprint = footprint

    def animate(frame: int):
        scenario_index = min(len(SCENARIOS) - 1, frame // FRAMES_PER_SCENARIO)
        scenario = SCENARIOS[scenario_index]
        local_frame = frame % FRAMES_PER_SCENARIO
        data = scenario_data[scenario]
        start_time, end_time = common_time_extent(data["traces"])
        playback_time = start_time + (local_frame / (FRAMES_PER_SCENARIO - 1)) * (end_time - start_time)
        if not hasattr(figure, "_current_scenario") or figure._current_scenario != scenario:
            setup_axes(axes, scenario, data["path"], data["grid"], data["traces"],
                       footprint, playback_time)
            figure._current_scenario = scenario
            for axis in axes:
                axis._replay_footprint = footprint
        update_axes(axes, playback_time)
        return []

    clip = animation.FuncAnimation(
        figure, animate, frames=len(SCENARIOS) * FRAMES_PER_SCENARIO,
        interval=1000 / FPS, blit=False, repeat=False,
    )
    writer = animation.FFMpegWriter(
        fps=FPS, codec="libx264", bitrate=1800,
        extra_args=["-pix_fmt", "yuv420p", "-movflags", "+faststart"],
    )
    clip.save(output / "gazebo_ab_replay.mp4", writer=writer, dpi=110)
    plt.close(figure)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Run matched pursuit controllers against the MuJoCo differential-drive model."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import random
import shutil
import subprocess
import sys
import time
from collections import Counter, deque
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Sequence

import yaml


DT = 0.02
WHEEL_RADIUS = 0.05035
GEOMETRIC_TRACK = 0.38550
WHEEL_SPEED_QUANTUM = 0.01
GOAL_POSITION_TOLERANCE = 0.08
GOAL_YAW_TOLERANCE = 0.06
GOAL_SPEED_TOLERANCE = 0.001
SETTLE_SECONDS = 1.0
POST_GOAL_OBSERVE_SECONDS = 3.0
POST_GOAL_CHECKPOINT_SECONDS = (0.0, 1.0, POST_GOAL_OBSERVE_SECONDS)
SWEEP_SPACING = 0.0025
CONTROLLERS = (
    "rpp",
    "rpp-dwpp",
    "vector-pursuit",
    "vector-pursuit-window",
)
DEFAULT_PROFILES = ("physics", "delayed", "slippery")
DEFAULT_SEEDS = (0, 1, 2, 3, 4)
PATH_PROJECTION_RULE = (
    "nearest nonzero-length segment by Euclidean projection; strict improvement only, "
    "so ties keep the first; body yaw interpolated on the shortest arc; moving-only "
    "metrics count samples with abs(true body linear speed)>0.01 m/s"
)

PROFILES: dict[str, dict[str, Any]] = {
    "physics": {
        "contact_mu": 1.0,
        "caster_mu": 0.2,
        "drive_kv": 0.4,
        "max_torque": 0.6,
        "command_delay_s": 0.02,
        "pose_update_hz": 50.0,
        "pose_delay_s": 0.0,
        "pose_noise_xy_sigma_m": 0.0,
        "pose_noise_yaw_sigma_rad": 0.0,
        "pose_bias_xy_sigma_m": 0.0,
        "pose_bias_yaw_sigma_rad": 0.0,
        "gyro_noise_sigma_radps": 0.0,
        "left_radius_scale": 1.0,
        "right_radius_scale": 1.0,
        "drive_armature": 0.001,
        "drive_tire_shape": "ellipsoid",
        "caster_tire_shape": "ellipsoid",
        "caster_trail": 0.02,
    },
    "delayed": {
        "contact_mu": 1.0,
        "caster_mu": 0.2,
        "drive_kv": 0.3,
        "max_torque": 0.45,
        "command_delay_s": 0.08,
        "pose_update_hz": 10.0,
        "pose_delay_s": 0.06,
        "pose_noise_xy_sigma_m": 0.005,
        "pose_noise_yaw_sigma_rad": 0.01,
        "pose_bias_xy_sigma_m": 0.002,
        "pose_bias_yaw_sigma_rad": 0.003,
        "gyro_noise_sigma_radps": 0.003,
        "left_radius_scale": 1.0,
        "right_radius_scale": 1.0,
        "drive_armature": 0.001,
        "drive_tire_shape": "ellipsoid",
        "caster_tire_shape": "ellipsoid",
        "caster_trail": 0.02,
    },
    "slippery": {
        "contact_mu": 0.25,
        "caster_mu": 0.2,
        "drive_kv": 0.3,
        "max_torque": 0.45,
        "command_delay_s": 0.12,
        "pose_update_hz": 10.0,
        "pose_delay_s": 0.10,
        "pose_noise_xy_sigma_m": 0.01,
        "pose_noise_yaw_sigma_rad": 0.02,
        "pose_bias_xy_sigma_m": 0.005,
        "pose_bias_yaw_sigma_rad": 0.01,
        "gyro_noise_sigma_radps": 0.003,
        "left_radius_scale": 1.0,
        "right_radius_scale": 1.0,
        "drive_armature": 0.001,
        "drive_tire_shape": "ellipsoid",
        "caster_tire_shape": "ellipsoid",
        "caster_trail": 0.02,
    },
    "wheel_fault": {
        "contact_mu": 0.25,
        "caster_mu": 0.2,
        "drive_kv": 0.3,
        "max_torque": 0.45,
        "command_delay_s": 0.12,
        "pose_update_hz": 10.0,
        "pose_delay_s": 0.10,
        "pose_noise_xy_sigma_m": 0.01,
        "pose_noise_yaw_sigma_rad": 0.02,
        "pose_bias_xy_sigma_m": 0.005,
        "pose_bias_yaw_sigma_rad": 0.01,
        "gyro_noise_sigma_radps": 0.003,
        "left_radius_scale": 1.01,
        "right_radius_scale": 0.99,
        "drive_armature": 0.001,
        "drive_tire_shape": "ellipsoid",
        "caster_tire_shape": "ellipsoid",
        "caster_trail": 0.02,
        "interpretation": (
            "rigid-contact drive-wheel unloading stress; not a nominal tire-wear prediction"
        ),
    },
}


class BenchmarkError(ValueError):
    """Invalid fixture, runner input, or bridge response."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def reject_json_constant(value: str) -> None:
    raise BenchmarkError(f"non-standard JSON number {value}")


def read_json(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as source:
            value = json.load(source, parse_constant=reject_json_constant)
    except (OSError, json.JSONDecodeError, BenchmarkError) as error:
        raise BenchmarkError(f"cannot read JSON from {path}: {error}") from error
    if not isinstance(value, dict):
        raise BenchmarkError(f"JSON root must be an object: {path}")
    return value


def write_json(path: Path, value: dict[str, Any]) -> None:
    with path.open("w", encoding="utf-8", newline="\n") as output:
        json.dump(value, output, indent=2, sort_keys=True, allow_nan=False)
        output.write("\n")


def finite_number(value: Any, label: str) -> float:
    try:
        parsed = float(value)
    except (TypeError, ValueError) as error:
        raise BenchmarkError(f"{label} is not a number") from error
    if not math.isfinite(parsed):
        raise BenchmarkError(f"{label} is not finite")
    return parsed


def wrap_angle(angle: float) -> float:
    return (angle + math.pi) % (2.0 * math.pi) - math.pi


def parse_path_csv(path: Path) -> list[tuple[float, float, float]]:
    expected = [
        "index", "x", "y", "body_yaw", "production_pose_free",
        "production_segment_free",
    ]
    points: list[tuple[float, float, float]] = []
    try:
        with path.open("r", encoding="utf-8", newline="") as source:
            reader = csv.DictReader(source)
            if reader.fieldnames != expected:
                raise BenchmarkError(f"invalid path CSV header in {path}")
            for index, row in enumerate(reader):
                if row["index"] != str(index):
                    raise BenchmarkError(f"path indices are not contiguous in {path}")
                point = tuple(finite_number(row[key], f"path {key}")
                              for key in ("x", "y", "body_yaw"))
                if row["production_pose_free"] not in ("0", "1") or \
                        row["production_segment_free"] not in ("0", "1"):
                    raise BenchmarkError(f"invalid free-space flags in {path}")
                points.append(point)  # type: ignore[arg-type]
    except OSError as error:
        raise BenchmarkError(f"cannot read path CSV {path}: {error}") from error
    if len(points) < 2:
        raise BenchmarkError(f"path CSV must contain at least two poses: {path}")
    return points


@dataclass(frozen=True)
class MapCells:
    width: int
    height: int
    resolution: float
    origin_x: float
    origin_y: float
    cells: tuple[tuple[int, int, float, float, int], ...]


def parse_map_csv(path: Path) -> MapCells:
    expected = [
        "width", "height", "resolution_m", "origin_x", "origin_y", "origin_yaw",
        "cell_x", "cell_y", "world_x", "world_y", "occupancy",
    ]
    cells: dict[tuple[int, int], tuple[float, float, int]] = {}
    metadata: tuple[int, int, float, float, float, float] | None = None
    try:
        with path.open("r", encoding="utf-8", newline="") as source:
            reader = csv.DictReader(source)
            if reader.fieldnames != expected:
                raise BenchmarkError(f"invalid map CSV header in {path}")
            for row in reader:
                try:
                    width, height = int(row["width"]), int(row["height"])
                    resolution = finite_number(row["resolution_m"], "map resolution")
                    origin_x = finite_number(row["origin_x"], "map origin x")
                    origin_y = finite_number(row["origin_y"], "map origin y")
                    origin_yaw = finite_number(row["origin_yaw"], "map origin yaw")
                    cell_x, cell_y = int(row["cell_x"]), int(row["cell_y"])
                    world_x = finite_number(row["world_x"], "cell world x")
                    world_y = finite_number(row["world_y"], "cell world y")
                    occupancy = int(row["occupancy"])
                except (KeyError, ValueError) as error:
                    raise BenchmarkError(f"invalid map CSV row in {path}") from error
                if (width <= 0 or height <= 0 or resolution <= 0 or
                        not -1 <= occupancy <= 100 or not 0 <= cell_x < width or
                        not 0 <= cell_y < height):
                    raise BenchmarkError(f"map row is outside valid ranges in {path}")
                if (abs(world_x - (origin_x + cell_x * resolution)) > 1e-9 or
                        abs(world_y - (origin_y + cell_y * resolution)) > 1e-9):
                    raise BenchmarkError(f"map world coordinates do not match the grid in {path}")
                row_metadata = (width, height, resolution, origin_x, origin_y, origin_yaw)
                if metadata is None:
                    metadata = row_metadata
                elif row_metadata != metadata:
                    raise BenchmarkError(f"map metadata changes between rows in {path}")
                key = (cell_x, cell_y)
                if key in cells:
                    raise BenchmarkError(f"duplicate map cell {key} in {path}")
                cells[key] = (world_x, world_y, occupancy)
    except OSError as error:
        raise BenchmarkError(f"cannot read map CSV {path}: {error}") from error
    if metadata is None:
        raise BenchmarkError(f"map CSV contains no cells: {path}")
    width, height, resolution, origin_x, origin_y, origin_yaw = metadata
    if len(cells) != width * height:
        raise BenchmarkError(f"map CSV does not contain every cell in {path}")
    if abs(origin_yaw) > 1e-12:
        raise BenchmarkError("rotated occupancy maps cannot be passed as axis-aligned MuJoCo boxes")
    return MapCells(
        width, height, resolution, origin_x, origin_y,
        tuple((x, y, *cells[(x, y)]) for y in range(height) for x in range(width)),
    )


def merge_obstacle_rectangles(map_cells: MapCells, threshold: int = 50) -> list[tuple[float, float, float, float]]:
    """Merge occupied cells as an exact union of axis-aligned rectangles."""
    occupied: dict[int, list[tuple[int, float, float]]] = {}
    for cell_x, cell_y, world_x, world_y, value in map_cells.cells:
        if value >= threshold:
            occupied.setdefault(cell_y, []).append((cell_x, world_x, world_y))

    # Make maximal horizontal runs for each row. Cell boundaries are taken from
    # the supplied world coordinates, so the union neither grows nor cuts corners.
    row_runs: dict[int, list[tuple[int, int, float, float, float]]] = {}
    for cell_y, row in occupied.items():
        row.sort(key=lambda cell: cell[0])
        run: list[tuple[int, float, float]] = []
        for cell in row:
            if run and cell[0] != run[-1][0] + 1:
                row_runs.setdefault(cell_y, []).append(_row_run(run, map_cells.resolution))
                run = []
            run.append(cell)
        if run:
            row_runs.setdefault(cell_y, []).append(_row_run(run, map_cells.resolution))

    active: dict[tuple[int, int], list[float]] = {}
    rectangles: list[tuple[float, float, float, float]] = []
    all_rows = sorted(row_runs)
    previous_row: int | None = None
    for cell_y in all_rows:
        runs = row_runs[cell_y]
        next_active: dict[tuple[int, int], list[float]] = {}
        if previous_row is not None and cell_y != previous_row + 1:
            rectangles.extend(tuple(box) for box in active.values())
            active.clear()
        for start, end, min_x, max_x, min_y in runs:
            key = (start, end)
            prior = active.pop(key, None)
            if prior is not None and abs(prior[3] - min_y) <= 1e-12:
                next_active[key] = [prior[0], prior[1], prior[2], min_y + map_cells.resolution]
            else:
                if prior is not None:
                    rectangles.append(tuple(prior))
                next_active[key] = [min_x, min_y, max_x, min_y + map_cells.resolution]
        rectangles.extend(tuple(box) for box in active.values())
        active = next_active
        previous_row = cell_y
    rectangles.extend(tuple(box) for box in active.values())
    return sorted(rectangles)


def _row_run(run: Sequence[tuple[int, float, float]], resolution: float) -> tuple[int, int, float, float, float]:
    first_x, world_x, world_y = run[0]
    last_x, last_world_x, _ = run[-1]
    return first_x, last_x, world_x, last_world_x + resolution, world_y


def load_footprint(path: Path) -> list[tuple[float, float]]:
    try:
        with path.open("r", encoding="utf-8") as source:
            geometry = yaml.safe_load(source)
    except (OSError, UnicodeError, yaml.YAMLError) as error:
        raise BenchmarkError(f"cannot read geometry YAML {path}: {error}") from error
    if not isinstance(geometry, dict):
        raise BenchmarkError(f"geometry root must be a mapping: {path}")
    if geometry.get("frame") != "base_footprint":
        raise BenchmarkError(f"geometry frame must be base_footprint: {path}")

    drive_to_base = geometry.get("drive_to_base")
    if not isinstance(drive_to_base, list) or len(drive_to_base) != 3:
        raise BenchmarkError(f"geometry drive_to_base must contain x, y, yaw: {path}")
    for index, value in enumerate(drive_to_base):
        if isinstance(value, bool):
            raise BenchmarkError(f"geometry drive_to_base[{index}] is not a number")
        finite_number(value, f"drive_to_base[{index}]")

    raw_points = geometry.get("footprint")
    if not isinstance(raw_points, list) or len(raw_points) < 3:
        raise BenchmarkError(f"geometry footprint must contain at least three vertices: {path}")
    points: list[tuple[float, float]] = []
    for index, vertex in enumerate(raw_points):
        if not isinstance(vertex, list) or len(vertex) != 2:
            raise BenchmarkError(f"geometry footprint[{index}] must contain x and y: {path}")
        if any(isinstance(value, bool) for value in vertex):
            raise BenchmarkError(f"geometry footprint[{index}] coordinates must be numbers")
        points.append((finite_number(vertex[0], f"footprint[{index}] x"),
                       finite_number(vertex[1], f"footprint[{index}] y")))
    return points


def project_path(path: Sequence[tuple[float, float, float]],
                 pose: tuple[float, float, float]) -> tuple[float, float, float, float]:
    x, y, yaw = pose
    best_distance_squared = math.inf
    best: tuple[float, float, float, float] | None = None
    for start, end in zip(path, path[1:]):
        dx, dy = end[0] - start[0], end[1] - start[1]
        length_squared = dx * dx + dy * dy
        if length_squared <= 1e-24:
            continue
        along = max(0.0, min(1.0, ((x - start[0]) * dx + (y - start[1]) * dy) / length_squared))
        px, py = start[0] + along * dx, start[1] + along * dy
        distance_squared = (x - px) ** 2 + (y - py) ** 2
        if distance_squared < best_distance_squared:
            desired_yaw = wrap_angle(start[2] + along * wrap_angle(end[2] - start[2]))
            best_distance_squared = distance_squared
            best = (px, py, desired_yaw, math.sqrt(distance_squared))
    if best is None:
        raise BenchmarkError("path contains no nonzero-length segment")
    return best[0], best[1], wrap_angle(best[2] - yaw), best[3]


def transform_footprint(footprint: Sequence[tuple[float, float]],
                        pose: tuple[float, float, float]) -> list[tuple[float, float]]:
    x, y, yaw = pose
    cosine, sine = math.cos(yaw), math.sin(yaw)
    return [(x + cosine * px - sine * py, y + sine * px + cosine * py)
            for px, py in footprint]


def polygon_intersects_rectangle(polygon: Sequence[tuple[float, float]],
                                 rectangle: tuple[float, float, float, float]) -> bool:
    min_x, min_y, max_x, max_y = rectangle
    rect = ((min_x, min_y), (max_x, min_y), (max_x, max_y), (min_x, max_y))
    axes = [(1.0, 0.0), (0.0, 1.0)]
    for a, b in zip(polygon, (*polygon[1:], polygon[0])):
        axes.append((-(b[1] - a[1]), b[0] - a[0]))
    for axis_x, axis_y in axes:
        polygon_projection = [x * axis_x + y * axis_y for x, y in polygon]
        rectangle_projection = [x * axis_x + y * axis_y for x, y in rect]
        if (max(polygon_projection) < min(rectangle_projection) - 1e-12 or
                max(rectangle_projection) < min(polygon_projection) - 1e-12):
            return False
    return True


def polygon_collides(polygon: Sequence[tuple[float, float]],
                     obstacles: Sequence[tuple[float, float, float, float]]) -> bool:
    poly_min_x = min(point[0] for point in polygon)
    poly_max_x = max(point[0] for point in polygon)
    poly_min_y = min(point[1] for point in polygon)
    poly_max_y = max(point[1] for point in polygon)
    for rectangle in obstacles:
        min_x, min_y, max_x, max_y = rectangle
        if (poly_max_x < min_x - 1e-12 or max_x < poly_min_x - 1e-12 or
                poly_max_y < min_y - 1e-12 or max_y < poly_min_y - 1e-12):
            continue
        if polygon_intersects_rectangle(polygon, rectangle):
            return True
    return False


def polygon_outside_map(polygon: Sequence[tuple[float, float]],
                        bounds: tuple[float, float, float, float]) -> bool:
    min_x, min_y, max_x, max_y = bounds
    return any(
        x <= min_x + 1e-12 or x >= max_x - 1e-12 or
        y <= min_y + 1e-12 or y >= max_y - 1e-12
        for x, y in polygon
    )


def swept_collision(footprint: Sequence[tuple[float, float]],
                    prior: tuple[float, float, float],
                    current: tuple[float, float, float],
                    obstacles: Sequence[tuple[float, float, float, float]]) -> bool:
    radius = max(math.hypot(x, y) for x, y in footprint)
    displacement = math.hypot(current[0] - prior[0], current[1] - prior[1])
    angle = abs(wrap_angle(current[2] - prior[2]))
    samples = max(1, math.ceil((displacement + radius * angle) / SWEEP_SPACING))
    for sample in range(samples + 1):
        fraction = sample / samples
        pose = (
            prior[0] + fraction * (current[0] - prior[0]),
            prior[1] + fraction * (current[1] - prior[1]),
            wrap_angle(prior[2] + fraction * wrap_angle(current[2] - prior[2])),
        )
        if polygon_collides(transform_footprint(footprint, pose), obstacles):
            return True
    return False


def swept_boundary_collision(footprint: Sequence[tuple[float, float]],
                             prior: tuple[float, float, float],
                             current: tuple[float, float, float],
                             bounds: tuple[float, float, float, float]) -> bool:
    radius = max(math.hypot(x, y) for x, y in footprint)
    displacement = math.hypot(current[0] - prior[0], current[1] - prior[1])
    angle = abs(wrap_angle(current[2] - prior[2]))
    samples = max(1, math.ceil((displacement + radius * angle) / SWEEP_SPACING))
    for sample in range(samples + 1):
        fraction = sample / samples
        pose = (
            prior[0] + fraction * (current[0] - prior[0]),
            prior[1] + fraction * (current[1] - prior[1]),
            wrap_angle(prior[2] + fraction * wrap_angle(current[2] - prior[2])),
        )
        if polygon_outside_map(transform_footprint(footprint, pose), bounds):
            return True
    return False


@dataclass(frozen=True)
class Observation:
    stamp: float
    pose: tuple[float, float, float]


def stable_rng(seed: int, stream: str) -> random.Random:
    digest = hashlib.sha256(f"{seed}:{stream}".encode("utf-8")).digest()
    return random.Random(int.from_bytes(digest[:8], "big"))


class PoseObserver:
    """Deterministic sampled pose stream with fixed bias, noise, and delivery delay."""

    def __init__(self, profile: dict[str, Any], seed: int,
                 initial_pose: tuple[float, float, float]):
        self.profile = profile
        self.pose_delay = float(profile["pose_delay_s"])
        self.update_ticks = max(1, round(1.0 / float(profile["pose_update_hz"]) / DT))
        bias_rng = stable_rng(seed, "pose-bias")
        self.bias_x = bias_rng.gauss(0.0, float(profile["pose_bias_xy_sigma_m"]))
        self.bias_y = bias_rng.gauss(0.0, float(profile["pose_bias_xy_sigma_m"]))
        self.bias_yaw = bias_rng.gauss(0.0, float(profile["pose_bias_yaw_sigma_rad"]))
        self.noise_rng = stable_rng(seed, "pose-noise")
        self.samples: deque[Observation] = deque()
        self.delivered: Observation | None = None
        self._append(-self.pose_delay, initial_pose)

    def _append(self, stamp: float, true_pose: tuple[float, float, float]) -> None:
        noisy_pose = (
            true_pose[0] + self.bias_x + self.noise_rng.gauss(
                0.0, float(self.profile["pose_noise_xy_sigma_m"])),
            true_pose[1] + self.bias_y + self.noise_rng.gauss(
                0.0, float(self.profile["pose_noise_xy_sigma_m"])),
            wrap_angle(true_pose[2] + self.bias_yaw + self.noise_rng.gauss(
                0.0, float(self.profile["pose_noise_yaw_sigma_rad"]))),
        )
        self.samples.append(Observation(stamp, noisy_pose))

    def observe(self, tick: int, true_pose: tuple[float, float, float]) -> Observation:
        if tick % self.update_ticks == 0:
            self._append(tick * DT, true_pose)
        now = tick * DT
        available_at = now - self.pose_delay
        while self.samples and self.samples[0].stamp <= available_at + 1e-12:
            self.delivered = self.samples.popleft()
        if self.delivered is not None:
            return self.delivered
        return self.samples[0]


class BridgeProcess:
    def __init__(self, binary: Path, controller: str, path: Path, map_file: Path,
                 geometry: Path, allow_reverse: bool):
        self.process = subprocess.Popen(
            [str(binary), "--controller", controller, "--path", str(path),
             "--map", str(map_file), "--geometry", str(geometry),
             "--allow-reverse", "1" if allow_reverse else "0"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, bufsize=1,
        )
        if self.process.stdin is None or self.process.stdout is None:
            raise BenchmarkError("failed to open bridge stdin/stdout")

    def command(self, now: float, pose_stamp: float, pose: tuple[float, float, float],
                measured_linear: float, measured_angular: float) -> tuple[int, bool, float, float]:
        assert self.process.stdin is not None and self.process.stdout is not None
        fields = (now, pose_stamp, now, *pose, measured_linear, measured_angular)
        self.process.stdin.write(" ".join(format(value, ".17g") for value in fields) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            raise BenchmarkError("controller bridge exited without a response")
        values = line.split()
        if len(values) != 4:
            raise BenchmarkError(f"invalid bridge response: {line.rstrip()!r}")
        try:
            error, reached_value = int(values[0]), int(values[1])
            linear, angular = float(values[2]), float(values[3])
        except ValueError as cause:
            raise BenchmarkError(f"invalid bridge response: {line.rstrip()!r}") from cause
        if reached_value not in (0, 1) or not math.isfinite(linear) or not math.isfinite(angular):
            raise BenchmarkError(f"non-finite or invalid bridge response: {line.rstrip()!r}")
        return error, bool(reached_value), linear, angular

    def close(self) -> str:
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
        return stderr.strip()


def bridge_error_name(code: int) -> str:
    return {
        0: "None", 1: "InvalidInput", 2: "MapUnavailable", 3: "StaleMap",
        4: "LocalizationUnavailable", 5: "StaleTf", 6: "StaleOdometry",
        7: "StartOutside", 8: "GoalOutside", 9: "StartOccupied", 10: "GoalOccupied",
        11: "NoPath", 12: "PlanningTimeout", 13: "Canceled", 14: "EmptyPath",
        15: "PathDeviation", 16: "Collision", 17: "ControllerFailure",
        18: "PlannerFailure", 19: "NavigationTimeout", 20: "GoalConflict",
    }.get(code, f"ErrorCode{code}")


def pose_from_state(state: dict[str, Any]) -> tuple[float, float, float]:
    return tuple(finite_number(state[key], f"physics state {key}")
                 for key in ("x", "y", "yaw"))  # type: ignore[return-value]


def state_number(state: dict[str, Any], key: str, default: float = 0.0) -> float:
    value = state.get(key, default)
    return finite_number(value, f"physics state {key}")


def state_list(state: dict[str, Any], key: str) -> list[float]:
    value = state.get(key, [])
    if value is None:
        return []
    if not isinstance(value, (list, tuple)):
        raise BenchmarkError(f"physics state {key} must be an array")
    return [finite_number(item, f"physics state {key}") for item in value]


def bool_state(state: dict[str, Any], key: str) -> bool:
    value = state.get(key, False)
    if isinstance(value, (list, tuple)):
        return any(bool(item) for item in value)
    return bool(value)


def state_tipped(state: dict[str, Any]) -> bool:
    return bool_state(state, "tip") or bool_state(state, "tipped")


def truth_pose_within_tolerances(final_pose: tuple[float, float, float],
                                 goal_pose: tuple[float, float, float]) -> bool:
    position_error = math.hypot(final_pose[0] - goal_pose[0], final_pose[1] - goal_pose[1])
    yaw_error = abs(wrap_angle(final_pose[2] - goal_pose[2]))
    return bool(position_error <= GOAL_POSITION_TOLERANCE and
                yaw_error <= GOAL_YAW_TOLERANCE)


def truth_stopped_within_tolerances(linear_speed: float, lateral_speed: float,
                                    angular_speed: float) -> bool:
    return max(abs(linear_speed), abs(lateral_speed), abs(angular_speed)) <= GOAL_SPEED_TOLERANCE


def truth_goal_status(reported_goal: bool, final_pose: tuple[float, float, float],
                      goal_pose: tuple[float, float, float], linear_speed: float,
                      lateral_speed: float, angular_speed: float,
                      collision_any: bool) -> bool:
    return bool(
        reported_goal and truth_pose_within_tolerances(final_pose, goal_pose) and
        truth_stopped_within_tolerances(linear_speed, lateral_speed, angular_speed) and
        not collision_any
    )


def truth_checkpoint(name: str, seconds_after_report: float,
                     pose: tuple[float, float, float], state: dict[str, Any],
                     goal_pose: tuple[float, float, float], collision_any: bool) -> dict[str, Any]:
    linear = state_number(state, "linear_x")
    lateral = state_number(state, "lateral_y")
    angular = state_number(state, "angular_z")
    return {
        "checkpoint": name,
        "seconds_after_report": seconds_after_report,
        "pose": list(pose),
        "position_error_m": math.hypot(pose[0] - goal_pose[0], pose[1] - goal_pose[1]),
        "yaw_error_rad": abs(wrap_angle(pose[2] - goal_pose[2])),
        "linear_speed_mps": linear,
        "lateral_speed_mps": lateral,
        "angular_speed_radps": angular,
        "ground_truth_pose_within_existing_tolerances": truth_pose_within_tolerances(
            pose, goal_pose
        ),
        "ground_truth_stopped_within_existing_tolerances": truth_stopped_within_tolerances(
            linear, lateral, angular
        ),
        "collision_any": collision_any,
    }


def load_physics_base() -> type[Any]:
    scripts = str(Path(__file__).resolve().parent)
    if scripts not in sys.path:
        sys.path.insert(0, scripts)
    try:
        from pursuit_physics_model import PhysicsBase  # type: ignore[import-not-found]
    except ImportError as error:
        raise BenchmarkError(f"cannot import pursuit_physics_model.PhysicsBase: {error}") from error
    return PhysicsBase


def make_physics_base(physics_base: type[Any], footprint: list[tuple[float, float]],
                      obstacles: list[tuple[float, float, float, float]],
                      profile: dict[str, Any]) -> Any:
    params = {
        "nominal_mu": float(profile["contact_mu"]),
        "caster_mu": float(profile["caster_mu"]),
        "drive_kv": float(profile["drive_kv"]),
        "max_torque": float(profile["max_torque"]),
        "physics_dt": 0.002,
        "wheel_radius": WHEEL_RADIUS,
        "nominal_track": GEOMETRIC_TRACK,
        "left_radius_scale": float(profile["left_radius_scale"]),
        "right_radius_scale": float(profile["right_radius_scale"]),
        "drive_armature": float(profile["drive_armature"]),
        "drive_tire_shape": profile["drive_tire_shape"],
        "caster_tire_shape": profile["caster_tire_shape"],
        "caster_trail": float(profile["caster_trail"]),
    }
    return physics_base(footprint, obstacles, params)


def qpos_values(base: Any) -> list[float]:
    data = getattr(base, "data", None)
    if data is None or not hasattr(data, "qpos"):
        raise BenchmarkError("PhysicsBase does not expose data.qpos")
    return [finite_number(value, "qpos") for value in data.qpos]


def write_csv_row(writer: csv.writer, values: Iterable[Any]) -> None:
    writer.writerow([format(value, ".17g") if isinstance(value, float) else value
                     for value in values])


def empty_metrics() -> dict[str, Any]:
    return {
        "tracking_samples_moving": 0,
        "position_square_sum": 0.0,
        "position_max_m": None,
        "body_yaw_square_sum": 0.0,
        "body_yaw_max_rad": None,
        "body_yaw_all_square_sum": 0.0,
        "body_yaw_all_samples": 0,
        "command_linear_square_sum": 0.0,
        "command_angular_square_sum": 0.0,
        "lateral_speed_square_sum": 0.0,
        "lateral_speed_max_mps": None,
        "torque_samples": 0,
        "torque_saturation_count": 0,
        "pose_error_square_sum": 0.0,
        "pose_yaw_error_square_sum": 0.0,
        "pose_age_sum_s": 0.0,
        "pose_observations": 0,
        "collision_samples": 0,
        "collision_any": False,
        "independent_swept_collision": False,
        "map_boundary_collision": False,
        "physical_obstacle_contact": False,
        "tipped": False,
    }


def finalize_metrics(metrics: dict[str, Any], steps: int, profile: dict[str, Any]) -> dict[str, Any]:
    moving = int(metrics["tracking_samples_moving"])
    observations = int(metrics["pose_observations"])
    torque_samples = int(metrics["torque_samples"])
    return {
        "tracking_samples_moving": moving,
        "position_rms_m": math.sqrt(metrics["position_square_sum"] / moving) if moving else None,
        "position_max_m": metrics["position_max_m"],
        "body_yaw_rms_rad": math.sqrt(metrics["body_yaw_square_sum"] / moving) if moving else None,
        "body_yaw_max_rad": metrics["body_yaw_max_rad"],
        "body_yaw_rms_all_rad": (
            math.sqrt(metrics["body_yaw_all_square_sum"] / metrics["body_yaw_all_samples"])
            if metrics["body_yaw_all_samples"] else None
        ),
        "body_yaw_all_samples": metrics["body_yaw_all_samples"],
        "command_actual_linear_rms_mps": (
            math.sqrt(metrics["command_linear_square_sum"] / steps) if steps else None
        ),
        "command_actual_angular_rms_radps": (
            math.sqrt(metrics["command_angular_square_sum"] / steps) if steps else None
        ),
        "lateral_speed_rms_mps": (
            math.sqrt(metrics["lateral_speed_square_sum"] / steps) if steps else None
        ),
        "lateral_speed_max_mps": metrics["lateral_speed_max_mps"],
        "torque_saturation_fraction": (
            metrics["torque_saturation_count"] / torque_samples if torque_samples else None
        ),
        "torque_saturation_count": metrics["torque_saturation_count"],
        "torque_samples": torque_samples,
        "pose_error_rms_m": (
            math.sqrt(metrics["pose_error_square_sum"] / observations) if observations else None
        ),
        "pose_yaw_error_rms_rad": (
            math.sqrt(metrics["pose_yaw_error_square_sum"] / observations)
            if observations else None
        ),
        "pose_observation_age_mean_s": (
            metrics["pose_age_sum_s"] / observations if observations else None
        ),
        "pose_observations": observations,
        "collision_samples": metrics["collision_samples"],
        "collision_any": bool(metrics["collision_any"]),
        "independent_swept_collision": bool(metrics["independent_swept_collision"]),
        "map_boundary_collision": bool(metrics["map_boundary_collision"]),
        "physical_obstacle_contact": bool(metrics["physical_obstacle_contact"]),
        "tipped": bool(metrics["tipped"]),
        "wheel_speed_quantization_radps": WHEEL_SPEED_QUANTUM,
        "command_delay_s": profile["command_delay_s"],
    }


def summarized_outcomes(runs: Sequence[dict[str, Any]]) -> dict[str, Any]:
    grouped: dict[str, list[dict[str, Any]]] = {}
    for run in runs:
        grouped.setdefault(run["controller"], []).append(run)
    summary: dict[str, Any] = {}
    for controller, items in sorted(grouped.items()):
        summary[controller] = {
            "denominator": len(items),
            "outcomes": dict(sorted(Counter(item["outcome"] for item in items).items())),
            "reported_goal": sum(item["outcome"] == "reported_goal" for item in items),
            "truth_goal": sum(item.get("truth_outcome") == "truth_goal" for item in items),
            "collision": sum(item["outcome"] == "collision" for item in items),
            "controller_error": sum(item["outcome"] == "controller_error" for item in items),
            "runner_error": sum(item["outcome"] == "runner_error" for item in items),
            "timeout": sum(item["outcome"] == "timeout" for item in items),
            "collision_any": sum(bool(item.get("collision_any")) for item in items),
        }
    return summary


def select_source_runs(summary: dict[str, Any], scenarios: Sequence[str] | None,
                       summary_path: Path) -> list[dict[str, Any]]:
    source_runs = summary.get("runs")
    if not isinstance(source_runs, list):
        raise BenchmarkError("baseline summary is missing its runs array")
    names = sorted({run.get("scenario") for run in source_runs if isinstance(run, dict)})
    requested = list(scenarios) if scenarios else names
    selected: list[dict[str, Any]] = []
    for name in requested:
        matches = [run for run in source_runs if isinstance(run, dict) and
                   run.get("scenario") == name and run.get("controller") == "rpp" and
                   run.get("dynamics") == "ideal" and run.get("start_variant") == "perturbed"]
        if len(matches) != 1:
            raise BenchmarkError(
                f"expected exactly one ideal/perturbed/rpp source run for {name}, got {len(matches)}"
            )
        row = matches[0]
        artifacts = row.get("artifacts")
        if not isinstance(artifacts, dict):
            raise BenchmarkError(f"baseline source run {row.get('id')} has no artifacts")
        resolved: dict[str, Path] = {}
        for key in ("path_csv", "map_csv", "result_json"):
            relative = artifacts.get(key)
            if not isinstance(relative, str) or not relative:
                raise BenchmarkError(f"baseline source run {row.get('id')} is missing {key}")
            source = (summary_path.parent / relative).resolve()
            try:
                source.relative_to(summary_path.parent.resolve())
            except ValueError as error:
                raise BenchmarkError(f"baseline artifact escapes its directory: {relative}") from error
            if not source.is_file():
                raise BenchmarkError(f"baseline artifact is missing: {source}")
            resolved[key] = source
        result = read_json(resolved["result_json"])
        row_copy = dict(row)
        row_copy["_path_source"] = resolved["path_csv"]
        row_copy["_map_source"] = resolved["map_csv"]
        row_copy["_result_source"] = resolved["result_json"]
        row_copy["_allow_reverse"] = bool(result.get("allow_reverse", False))
        row_copy["_controller_config"] = result.get("experiment_config", {})
        row_copy["_path"] = parse_path_csv(resolved["path_csv"])
        row_copy["_map"] = parse_map_csv(resolved["map_csv"])
        selected.append(row_copy)
    return selected


def bridge_controller_name(controller: str) -> str:
    if controller not in CONTROLLERS:
        raise BenchmarkError(f"unknown controller {controller}")
    return controller


def run_case(source: dict[str, Any], profile_name: str, seed: int,
             controller: str, output_root: Path, bridge_binary: Path,
             geometry_path: Path, footprint: list[tuple[float, float]],
             physics_base_class: type[Any], max_seconds: float,
             source_hashes: dict[str, str]) -> dict[str, Any]:
    profile = dict(PROFILES[profile_name])
    scenario = str(source["scenario"])
    run_id = f"{scenario}__{profile_name}__seed{seed}__{controller}"
    run_directory = output_root / scenario / profile_name / f"seed-{seed}" / controller
    run_directory.mkdir(parents=True, exist_ok=True)
    path_copy = run_directory / "path.csv"
    map_copy = run_directory / "map.csv"
    telemetry_path = run_directory / "telemetry.csv"
    qpos_path = run_directory / "physics_qpos.csv"
    model_path = run_directory / "model.xml"
    result_path = run_directory / "result.json"
    shutil.copyfile(source["_path_source"], path_copy)
    shutil.copyfile(source["_map_source"], map_copy)
    if sha256_file(path_copy) != sha256_file(source["_path_source"]) or \
            sha256_file(map_copy) != sha256_file(source["_map_source"]):
        raise BenchmarkError(f"source fixture changed while copying for {run_id}")

    intended_pose = tuple(float(value) for value in source["initial_pose"])
    if len(intended_pose) != 3 or not all(math.isfinite(value) for value in intended_pose):
        raise BenchmarkError(f"invalid initial pose in source run {source.get('id')}")
    path_points = source["_path"]
    map_cells: MapCells = source["_map"]
    obstacles = merge_obstacle_rectangles(map_cells)
    map_bounds = (
        map_cells.origin_x,
        map_cells.origin_y,
        map_cells.origin_x + map_cells.width * map_cells.resolution,
        map_cells.origin_y + map_cells.height * map_cells.resolution,
    )
    bridge: BridgeProcess | None = None
    bridge_stderr = ""
    wall_start = time.monotonic()
    start_truth = intended_pose
    final_state: dict[str, Any] = {}
    final_pose = intended_pose
    terminal_outcome = "runner_error"
    error_code: int | None = None
    error_message = ""
    reported_goal = False
    steps = 0
    task_seconds = 0.0
    settle_steps = 0
    ground_truth_checkpoints: list[dict[str, Any]] = []
    metrics = empty_metrics()
    model_metadata: Any = {}
    model_xml_sha256 = ""

    try:
        base = make_physics_base(physics_base_class, footprint, obstacles, profile)
        base.reset(intended_pose, caster_yaws=(0.0, 0.0))
        for _ in range(round(SETTLE_SECONDS / DT)):
            base.step((0.0, 0.0), dt=DT)
        initial_state = base.state()
        start_truth = pose_from_state(initial_state)
        final_state = dict(initial_state)
        final_pose = start_truth
        initial_obstacle_contact = bool_state(initial_state, "obstacle_contact")
        initial_independent_collision = polygon_collides(
            transform_footprint(footprint, start_truth), obstacles
        )
        initial_boundary_collision = polygon_outside_map(
            transform_footprint(footprint, start_truth), map_bounds
        )
        initial_tipped = state_tipped(initial_state)
        startup_collision = (
            initial_obstacle_contact or initial_independent_collision or
            initial_boundary_collision or initial_tipped
        )
        if startup_collision:
            terminal_outcome = "collision"
            error_code = 16
            error_message = (
                "physics obstacle contact during settling" if initial_obstacle_contact else
                "independent footprint collision after settling" if initial_independent_collision else
                "map boundary collision after settling" if initial_boundary_collision else
                "physics model tipped during settling"
            )
            metrics["collision_any"] = True
            metrics["collision_samples"] = 1
            metrics["independent_swept_collision"] = initial_independent_collision
            metrics["map_boundary_collision"] = initial_boundary_collision
            metrics["physical_obstacle_contact"] = initial_obstacle_contact
            metrics["tipped"] = initial_tipped
        model_xml = str(base.model_xml)
        model_path.write_text(model_xml, encoding="utf-8")
        model_xml_sha256 = sha256_file(model_path)
        model_metadata = base.metadata
        if not isinstance(model_metadata, (dict, list, str, int, float, bool, type(None))):
            model_metadata = str(model_metadata)

        observer = PoseObserver(profile, seed, start_truth)
        gyro_rng = stable_rng(seed, "gyro-noise")
        command_delay_ticks = max(0, math.ceil(float(profile["command_delay_s"]) / DT - 1e-12))
        pending_commands: deque[tuple[float, float]] = deque(
            [(0.0, 0.0)] * command_delay_ticks
        )
        bridge_clock_offset = 1.0
        bridge = BridgeProcess(
            bridge_binary, bridge_controller_name(controller), path_copy, map_copy,
            geometry_path, bool(source["_allow_reverse"]),
        )
        with telemetry_path.open("w", encoding="utf-8", newline="") as telemetry_file, \
                qpos_path.open("w", encoding="utf-8", newline="") as qpos_file:
            telemetry_writer = csv.writer(telemetry_file)
            qpos_writer = csv.writer(qpos_file)
            telemetry_writer.writerow([
                "step", "phase", "sim_time_s", "truth_x", "truth_y", "truth_yaw",
                "truth_linear_x", "truth_lateral_y", "truth_angular_z", "observed_x",
                "observed_y", "observed_yaw", "pose_stamp_s", "odom_stamp_s",
                "pose_age_s", "desired_x", "desired_y", "desired_yaw",
                "path_position_error_m", "path_yaw_error_rad", "command_linear_x",
                "command_angular_z", "delayed_linear_x", "delayed_angular_z",
                "wheel_left_radps", "wheel_right_radps", "measured_linear_x",
                "measured_angular_z", "torque_left_nm", "torque_right_nm",
                "caster_yaw_left_rad", "caster_yaw_right_rad", "obstacle_contact",
                "independent_collision", "map_boundary_collision", "tip", "reached_goal",
                "controller_error",
            ])
            qpos_count = len(qpos_values(base))
            qpos_writer.writerow(["sim_time_s", "phase", "step"] +
                                 [f"qpos_{index}" for index in range(qpos_count)])

            steps_limit = max(1, math.ceil(max_seconds / DT))
            for tick in range(steps_limit):
                now = tick * DT
                state = base.state()
                truth_pose = pose_from_state(state)
                final_state = dict(state)
                final_pose = truth_pose
                write_csv_row(qpos_writer, [now, "control_before", tick, *qpos_values(base)])
                observed = observer.observe(tick, truth_pose)
                angular_true = state_number(state, "angular_z")
                wheel_left, wheel_right = state_number(state, "wheel_left"), state_number(state, "wheel_right")
                quantized_left = round(wheel_left / WHEEL_SPEED_QUANTUM) * WHEEL_SPEED_QUANTUM
                quantized_right = round(wheel_right / WHEEL_SPEED_QUANTUM) * WHEEL_SPEED_QUANTUM
                measured_linear = WHEEL_RADIUS * (quantized_left + quantized_right) / 2.0
                measured_angular = angular_true + gyro_rng.gauss(
                    0.0, float(profile["gyro_noise_sigma_radps"]))
                bridge_now = bridge_clock_offset + now
                bridge_pose_stamp = bridge_clock_offset + observed.stamp
                if startup_collision:
                    projection_x, projection_y, path_yaw_error, position_error = project_path(
                        path_points, truth_pose
                    )
                    desired_yaw = wrap_angle(truth_pose[2] + path_yaw_error)
                    write_csv_row(telemetry_writer, _telemetry_values(
                        tick, "initial_collision", now, state, truth_pose, observed, now,
                        projection_x, projection_y, desired_yaw, position_error,
                        path_yaw_error, (0.0, 0.0), (0.0, 0.0), measured_linear,
                        measured_angular, False, initial_independent_collision,
                        initial_boundary_collision, 0,
                    ))
                    break
                error, reached, command_linear, command_angular = bridge.command(
                    bridge_now, bridge_pose_stamp, observed.pose,
                    measured_linear, measured_angular,
                )
                projection_x, projection_y, path_yaw_error, position_error = project_path(
                    path_points, truth_pose
                )
                desired_yaw = wrap_angle(truth_pose[2] + path_yaw_error)
                observation_position_error = math.hypot(
                    observed.pose[0] - truth_pose[0], observed.pose[1] - truth_pose[1]
                )
                observation_yaw_error = wrap_angle(observed.pose[2] - truth_pose[2])
                pose_age = max(0.0, now - observed.stamp)
                metrics["pose_error_square_sum"] += observation_position_error ** 2
                metrics["pose_yaw_error_square_sum"] += observation_yaw_error ** 2
                metrics["pose_age_sum_s"] += pose_age
                metrics["pose_observations"] += 1
                if error != 0:
                    terminal_outcome = "controller_error"
                    error_code = error
                    error_message = f"controller returned {bridge_error_name(error)}"
                    steps = tick
                    task_seconds = now
                    write_csv_row(telemetry_writer, _telemetry_values(
                        tick, "control", now, state, truth_pose, observed, now, projection_x,
                        projection_y, desired_yaw, position_error, path_yaw_error,
                        (command_linear, command_angular), (0.0, 0.0), measured_linear,
                        measured_angular, False, False, False, error,
                    ))
                    base.step((0.0, 0.0), dt=DT)
                    final_state = dict(base.state())
                    final_pose = pose_from_state(final_state)
                    write_csv_row(qpos_writer, [now + DT, "controller_error_stop", tick,
                                                *qpos_values(base)])
                    break
                if reached:
                    terminal_outcome = "reported_goal"
                    reported_goal = True
                    steps = tick
                    task_seconds = now
                    ground_truth_checkpoints.append(truth_checkpoint(
                        "reported_goal", 0.0, truth_pose, state, tuple(path_points[-1]),
                        bool(metrics["collision_any"]),
                    ))
                    write_csv_row(telemetry_writer, _telemetry_values(
                        tick, "control", now, state, truth_pose, observed, now, projection_x,
                        projection_y, desired_yaw, position_error, path_yaw_error,
                        (command_linear, command_angular), (0.0, 0.0), measured_linear,
                        measured_angular, True, False, False, error,
                    ))
                    break

                if not math.isfinite(command_linear) or not math.isfinite(command_angular):
                    terminal_outcome = "controller_error"
                    error_code = 17
                    error_message = "controller returned a non-finite command"
                    steps = tick
                    task_seconds = now
                    break

                pending_commands.append((command_linear, command_angular))
                if command_delay_ticks:
                    delayed_command = pending_commands.popleft()
                else:
                    delayed_command = pending_commands.pop()
                base.step(delayed_command, dt=DT)
                after_state = base.state()
                after_pose = pose_from_state(after_state)
                independent_collision = swept_collision(footprint, truth_pose, after_pose, obstacles)
                boundary_collision = swept_boundary_collision(
                    footprint, truth_pose, after_pose, map_bounds
                )
                obstacle_contact = bool_state(after_state, "obstacle_contact")
                tipped = state_tipped(after_state)
                physical_collision = (
                    obstacle_contact or independent_collision or boundary_collision or tipped
                )
                metrics["collision_samples"] += int(physical_collision)
                metrics["collision_any"] = bool(metrics["collision_any"] or physical_collision)
                metrics["independent_swept_collision"] = bool(
                    metrics["independent_swept_collision"] or independent_collision
                )
                metrics["map_boundary_collision"] = bool(
                    metrics["map_boundary_collision"] or boundary_collision
                )
                metrics["physical_obstacle_contact"] = bool(
                    metrics["physical_obstacle_contact"] or obstacle_contact
                )
                metrics["tipped"] = bool(metrics["tipped"] or tipped)
                next_projection_x, next_projection_y, next_yaw_error, next_position_error = project_path(
                    path_points, after_pose
                )
                metrics["body_yaw_all_square_sum"] += next_yaw_error ** 2
                metrics["body_yaw_all_samples"] += 1
                after_linear = state_number(after_state, "linear_x")
                after_lateral = state_number(after_state, "lateral_y")
                after_angular = state_number(after_state, "angular_z")
                metrics["command_linear_square_sum"] += (command_linear - after_linear) ** 2
                metrics["command_angular_square_sum"] += (command_angular - after_angular) ** 2
                metrics["lateral_speed_square_sum"] += after_lateral ** 2
                prior_max_lateral = metrics["lateral_speed_max_mps"]
                metrics["lateral_speed_max_mps"] = max(
                    abs(after_lateral), prior_max_lateral if prior_max_lateral is not None else 0.0
                )
                if abs(after_linear) > 0.01:
                    metrics["tracking_samples_moving"] += 1
                    metrics["position_square_sum"] += next_position_error ** 2
                    metrics["position_max_m"] = max(
                        next_position_error,
                        metrics["position_max_m"] if metrics["position_max_m"] is not None else 0.0,
                    )
                    metrics["body_yaw_square_sum"] += next_yaw_error ** 2
                    metrics["body_yaw_max_rad"] = max(
                        abs(next_yaw_error),
                        metrics["body_yaw_max_rad"]
                        if metrics["body_yaw_max_rad"] is not None else 0.0,
                    )
                torques = state_list(after_state, "torques")
                if torques:
                    metrics["torque_samples"] += len(torques)
                    metrics["torque_saturation_count"] += sum(
                        abs(torque) >= float(profile["max_torque"]) * (1.0 - 1e-6)
                        for torque in torques
                    )
                write_csv_row(telemetry_writer, _telemetry_values(
                    tick + 1, "control", now + DT, after_state, after_pose, observed,
                    now, next_projection_x, next_projection_y,
                    wrap_angle(after_pose[2] + next_yaw_error), next_position_error,
                    next_yaw_error, (command_linear, command_angular), delayed_command,
                    measured_linear, measured_angular, False, independent_collision,
                    boundary_collision, 0,
                ))
                write_csv_row(qpos_writer, [now + DT, "post_step", tick + 1, *qpos_values(base)])
                final_state = dict(after_state)
                final_pose = after_pose
                steps = tick + 1
                task_seconds = (tick + 1) * DT
                if physical_collision:
                    terminal_outcome = "collision"
                    error_code = 16
                    error_message = (
                        "physics obstacle contact" if obstacle_contact else
                        "independent swept-footprint collision" if independent_collision else
                        "map boundary collision" if boundary_collision else
                        "physics model tipped"
                    )
                    break
            else:
                terminal_outcome = "timeout"
                error_code = 19
                error_message = f"{max_seconds:g} second virtual task budget exceeded"

            if reported_goal:
                # Post-goal observation records physical settling only; it cannot
                # change the controller's already-reported completion result or task time.
                previous_settle_pose = final_pose
                checkpoint_steps = {
                    round(seconds / DT): seconds
                    for seconds in POST_GOAL_CHECKPOINT_SECONDS[1:]
                }
                for settle_tick in range(round(POST_GOAL_OBSERVE_SECONDS / DT)):
                    base.step((0.0, 0.0), dt=DT)
                    final_state = dict(base.state())
                    final_pose = pose_from_state(final_state)
                    settle_steps += 1
                    settle_projection = project_path(path_points, final_pose)
                    settle_contact = bool_state(final_state, "obstacle_contact")
                    settle_tipped = state_tipped(final_state)
                    settle_collision = swept_collision(
                        footprint, previous_settle_pose, final_pose, obstacles,
                    )
                    settle_boundary_collision = swept_boundary_collision(
                        footprint, previous_settle_pose, final_pose, map_bounds,
                    )
                    write_csv_row(qpos_writer, [task_seconds + settle_steps * DT, "settle",
                                                settle_tick, *qpos_values(base)])
                    write_csv_row(telemetry_writer, _telemetry_values(
                        steps + settle_tick + 1, "settle", task_seconds + settle_steps * DT,
                        final_state, final_pose, observed, task_seconds,
                        settle_projection[0], settle_projection[1],
                        wrap_angle(final_pose[2] + settle_projection[2]),
                        settle_projection[3], settle_projection[2], (0.0, 0.0), (0.0, 0.0),
                        state_number(final_state, "linear_x"),
                        state_number(final_state, "angular_z"), False,
                        settle_collision, settle_boundary_collision, 0,
                    ))
                    metrics["collision_any"] = bool(metrics["collision_any"] or settle_contact or
                        settle_tipped or settle_collision or settle_boundary_collision)
                    metrics["independent_swept_collision"] = bool(
                        metrics["independent_swept_collision"] or settle_collision
                    )
                    metrics["map_boundary_collision"] = bool(
                        metrics["map_boundary_collision"] or settle_boundary_collision
                    )
                    metrics["physical_obstacle_contact"] = bool(
                        metrics["physical_obstacle_contact"] or settle_contact
                    )
                    metrics["tipped"] = bool(metrics["tipped"] or settle_tipped)
                    metrics["collision_samples"] += int(
                        settle_contact or settle_tipped or settle_collision or
                        settle_boundary_collision
                    )
                    previous_settle_pose = final_pose
                    checkpoint_seconds = checkpoint_steps.get(settle_steps)
                    if checkpoint_seconds is not None:
                        ground_truth_checkpoints.append(truth_checkpoint(
                            f"post_goal_{checkpoint_seconds:g}s", checkpoint_seconds,
                            final_pose, final_state, tuple(path_points[-1]),
                            bool(metrics["collision_any"]),
                        ))
            final_state = dict(base.state())
            final_pose = pose_from_state(final_state)

            telemetry_file.flush()
            qpos_file.flush()
    except Exception as error:
        terminal_outcome = "runner_error"
        error_code = None
        error_message = f"runner error: {type(error).__name__}: {error}"
        if not telemetry_path.exists():
            with telemetry_path.open("w", encoding="utf-8", newline="") as output:
                csv.writer(output).writerow(["step", "phase", "sim_time_s"])
        if not qpos_path.exists():
            with qpos_path.open("w", encoding="utf-8", newline="") as output:
                csv.writer(output).writerow(["sim_time_s", "phase", "step"])
        if not model_path.exists():
            model_path.write_text("", encoding="utf-8")
    finally:
        if bridge is not None:
            bridge_stderr = bridge.close()

    truth_position_error = math.hypot(final_pose[0] - path_points[-1][0],
                                      final_pose[1] - path_points[-1][1])
    truth_yaw_error = abs(wrap_angle(final_pose[2] - path_points[-1][2]))
    truth_speeds = (
        abs(state_number(final_state, "linear_x")),
        abs(state_number(final_state, "lateral_y")),
        abs(state_number(final_state, "angular_z")),
    )
    truth_pose_within = truth_pose_within_tolerances(final_pose, tuple(path_points[-1]))
    truth_stopped_within = truth_stopped_within_tolerances(*truth_speeds)
    truth_goal = truth_goal_status(
        reported_goal, final_pose, tuple(path_points[-1]), *truth_speeds,
        bool(metrics["collision_any"]),
    )
    truth_outcome = "truth_goal" if truth_goal else ("truth_miss" if reported_goal else "not_reported")
    result = {
        "schema_version": 1,
        "id": run_id,
        "scenario": scenario,
        "source_run_id": source["id"],
        "profile": profile_name,
        "seed": seed,
        "controller": controller,
        "bridge_controller": bridge_controller_name(controller),
        "outcome": terminal_outcome,
        "reported_goal": reported_goal,
        "truth_outcome": truth_outcome,
        "ground_truth_pose_within_existing_tolerances": truth_pose_within,
        "ground_truth_stopped_within_existing_tolerances": truth_stopped_within,
        "ground_truth_goal_within_existing_tolerances": truth_goal,
        "ground_truth_checkpoints": ground_truth_checkpoints,
        "error_code": error_code,
        "error_message": error_message,
        "bridge_stderr": bridge_stderr,
        "intended_initial_pose": list(intended_pose),
        "actual_initial_pose_after_settle": list(start_truth),
        "goal_pose": list(path_points[-1]),
        "final_pose_after_settle": list(final_pose),
        "final_pose_after_post_goal_observation": list(final_pose),
        "final_position_error_m": truth_position_error,
        "final_yaw_error_rad": truth_yaw_error,
        "final_linear_speed_mps": truth_speeds[0],
        "final_lateral_speed_mps": truth_speeds[1],
        "final_angular_speed_radps": truth_speeds[2],
        "steps": steps,
        "task_seconds": task_seconds,
        "settle_seconds": settle_steps * DT,
        "initial_settle_seconds": SETTLE_SECONDS,
        "post_goal_observe_seconds": settle_steps * DT,
        "wall_seconds": time.monotonic() - wall_start,
        "tracking_projection_rule": PATH_PROJECTION_RULE,
        "controller_configuration": {
            "source_experiment_config": source.get("_controller_config", {}),
            "allow_reverse": bool(source["_allow_reverse"]),
            "bridge_default_lookahead_time_s": 1.5,
            "bridge_default_min_lookahead_m": 0.3,
            "bridge_default_vector_pursuit_k": 5.0,
        },
        "physics_parameters": {
            **profile,
            "physics_dt_s": 0.002,
            "control_dt_s": DT,
            "nominal_wheel_radius_m": WHEEL_RADIUS,
            "geometric_track_m": GEOMETRIC_TRACK,
            "caster_yaws_initial_rad": [0.0, 0.0],
            "initial_settle_seconds": SETTLE_SECONDS,
            "post_goal_observe_seconds": POST_GOAL_OBSERVE_SECONDS,
        },
        "physics_metadata": model_metadata,
        "model_xml_sha256": model_xml_sha256,
        "source_hashes": {
            **source_hashes,
            "path_csv_sha256": sha256_file(path_copy),
            "map_csv_sha256": sha256_file(map_copy),
        },
        **finalize_metrics(metrics, steps, profile),
        "artifacts": {
            "path_csv": "path.csv",
            "map_csv": "map.csv",
            "telemetry_csv": "telemetry.csv",
            "physics_qpos_csv": "physics_qpos.csv",
            "model_xml": "model.xml",
            "result_json": "result.json",
        },
    }
    # A setup failure may happen before the model is available; retain this run as
    # a visible failure record with empty physical-state metrics.
    write_json(result_path, result)
    return result


def _telemetry_values(step: int, phase: str, sim_time: float,
                      state: dict[str, Any], truth_pose: tuple[float, float, float],
                      observed: Observation, odom_stamp: float,
                      desired_x: float, desired_y: float, desired_yaw: float,
                      position_error: float, yaw_error: float,
                      command: tuple[float, float], delayed_command: tuple[float, float],
                      measured_linear: float, measured_angular: float,
                      reached: bool, collision: bool, boundary_collision: bool,
                      controller_error: int) -> list[Any]:
    wheels = (state_number(state, "wheel_left"), state_number(state, "wheel_right"))
    torques = state_list(state, "torques")
    casters = state_list(state, "caster_yaws")
    return [
        step, phase, sim_time, *truth_pose,
        state_number(state, "linear_x"), state_number(state, "lateral_y"),
        state_number(state, "angular_z"), *observed.pose, observed.stamp, odom_stamp,
        max(0.0, sim_time - observed.stamp), desired_x, desired_y, desired_yaw,
        position_error, yaw_error, command[0], command[1], delayed_command[0],
        delayed_command[1], *wheels, measured_linear, measured_angular,
        torques[0] if torques else 0.0, torques[1] if len(torques) > 1 else 0.0,
        casters[0] if casters else 0.0, casters[1] if len(casters) > 1 else 0.0,
        int(bool_state(state, "obstacle_contact")), int(collision),
        int(boundary_collision), int(state_tipped(state)),
        int(reached), controller_error,
    ]


def parse_seed_list(value: str) -> tuple[int, ...]:
    try:
        seeds = tuple(int(part.strip()) for part in value.split(",") if part.strip())
    except ValueError as error:
        raise argparse.ArgumentTypeError("seeds must be a comma-separated list of integers") from error
    if not seeds or len(seeds) != len(set(seeds)):
        raise argparse.ArgumentTypeError("seeds must be nonempty and unique")
    return seeds


def parse_arguments(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summary", required=True, type=Path,
                        help="baseline pursuit experiment summary.json")
    parser.add_argument("--bridge", required=True, type=Path,
                        help="rbt_navigation_pursuit_physics_bridge executable")
    parser.add_argument("--geometry", required=True, type=Path,
                        help="front-drive geometry YAML")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--scenario", action="append", dest="scenarios")
    parser.add_argument(
        "--profile", action="append", dest="profiles",
        help="profile to include (physics, delayed, slippery, or optional wheel_fault stress)",
    )
    parser.add_argument("--seeds", type=parse_seed_list, default=DEFAULT_SEEDS)
    parser.add_argument("--max-seconds", type=float, default=120.0)
    args = parser.parse_args(argv)
    if not math.isfinite(args.max_seconds) or args.max_seconds <= 0:
        parser.error("--max-seconds must be finite and positive")
    args.profiles = args.profiles or list(DEFAULT_PROFILES)
    if args.scenarios and len(args.scenarios) != len(set(args.scenarios)):
        parser.error("--scenario values must be unique")
    invalid_profiles = sorted(set(args.profiles) - set(PROFILES))
    if invalid_profiles:
        parser.error(f"unknown profiles: {', '.join(invalid_profiles)}")
    if len(args.profiles) != len(set(args.profiles)):
        parser.error("--profile values must be unique")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_arguments(argv)
    if args.output.joinpath("summary.json").exists():
        print(f"pursuit physics benchmark: refusing to overwrite {args.output / 'summary.json'}",
              file=sys.stderr)
        return 2
    for label, path in (("baseline summary", args.summary), ("bridge", args.bridge),
                        ("geometry", args.geometry)):
        if not path.is_file():
            print(f"pursuit physics benchmark: {label} does not exist: {path}", file=sys.stderr)
            return 2
    try:
        baseline = read_json(args.summary)
        sources = select_source_runs(baseline, args.scenarios, args.summary)
        footprint = load_footprint(args.geometry)
        physics_base_class = load_physics_base()
        args.output.mkdir(parents=True, exist_ok=True)
    except (BenchmarkError, OSError) as error:
        print(f"pursuit physics benchmark: {error}", file=sys.stderr)
        return 2

    source_hashes = {
        "baseline_summary_sha256": sha256_file(args.summary),
        "geometry_sha256": sha256_file(args.geometry),
        "bridge_sha256": sha256_file(args.bridge),
    }
    runs: list[dict[str, Any]] = []
    for source in sources:
        for profile_name in args.profiles:
            for seed in args.seeds:
                for controller in CONTROLLERS:
                    result = run_case(
                        source, profile_name, seed, controller, args.output,
                        args.bridge.resolve(), args.geometry.resolve(), footprint,
                        physics_base_class, args.max_seconds, source_hashes,
                    )
                    runs.append(result)

    summary = {
        "schema_version": 1,
        "experiment": "matched_mujoco_pursuit_physics_ab",
        "mode": "physics_closed_loop_controller_experiment",
        "run_count": len(runs),
        "scenario_names": [source["scenario"] for source in sources],
        "controllers": list(CONTROLLERS),
        "controller_configuration_by_scenario": {
            source["scenario"]: {
                "source_experiment_config": source.get("_controller_config", {}),
                "allow_reverse": bool(source["_allow_reverse"]),
                "bridge_default_lookahead_time_s": 1.5,
                "bridge_default_min_lookahead_m": 0.3,
                "bridge_default_vector_pursuit_k": 5.0,
            }
            for source in sources
        },
        "profiles": {name: PROFILES[name] for name in args.profiles},
        "profile_run_counts": {
            name: sum(run["profile"] == name for run in runs) for name in args.profiles
        },
        "outcome_counts_by_profile": {
            name: summarized_outcomes([run for run in runs if run["profile"] == name])
            for name in args.profiles
        },
        "seeds": list(args.seeds),
        "dt_seconds": DT,
        "max_seconds": args.max_seconds,
        "initial_settle_seconds": SETTLE_SECONDS,
        "post_goal_observe_seconds": POST_GOAL_OBSERVE_SECONDS,
        "post_goal_checkpoint_seconds": list(POST_GOAL_CHECKPOINT_SECONDS),
        "outcome_denominator_policy": "every scheduled controller/profile/seed run is counted, including all failures",
        "outcome_counts_by_controller": summarized_outcomes(runs),
        "path_projection_rule": PATH_PROJECTION_RULE,
        "ground_truth_goal_tolerances": {
            "position_m": GOAL_POSITION_TOLERANCE,
            "yaw_rad": GOAL_YAW_TOLERANCE,
            "linear_lateral_angular_speed": GOAL_SPEED_TOLERANCE,
        },
        "assumptions": {
            "scope": "controller-to-rigid-body simulation only; no LiDAR, LIO, or ROS sensor stack",
            "wheel_command_conversion": "nominal radius 0.05035 m; geometric contact-center track 0.38550 m",
            "wheel_encoder": "wheel rates rounded to 0.01 rad/s; linear odometry uses nominal radius and mean rates",
            "yaw_odometry": "physics angular velocity plus configured gyro noise",
            "pose_observation": "sampled noisy/bias pose; update/delay profile; held newest delivered sample",
            "command_delay": "queued at 50 Hz; ceil(delay / 0.02 s) control ticks",
            "obstacles": "occupied map cells merged into exact axis-aligned rectangle unions",
            "caster_yaw": "zero at reset for all runs; no seed-dependent caster offsets",
            "rounded_tire_contact": "ellipsoid drive and caster tire shapes are simulation assumptions",
            "drive_armature": "0.001 kg m^2 assumed on each drive wheel; applied uniformly across profiles",
            "caster_trail": "0.02 m assumed; supplied experiment geometry must include the matching rear caster sweep envelope",
            "geometry_scope": "--geometry is an experiment-only footprint file matched to the supplied frozen fixture summary",
            "actuator_model": "wheel torque and contact simulation only, without velocity override",
            "post_goal_observation": "fixed 3 s zero-command physical observation after reported_goal; 0 s/report, 1 s, and 3 s truth checkpoints; excluded from task_seconds",
        },
        "sources": {
            **source_hashes,
            "source_runs": [
                {
                    "scenario": source["scenario"],
                    "source_run_id": source["id"],
                    "path_csv_sha256": sha256_file(source["_path_source"]),
                    "map_csv_sha256": sha256_file(source["_map_source"]),
                }
                for source in sources
            ],
        },
        "physics_model_metadata_by_profile": {
            name: next((run.get("physics_metadata") for run in runs
                        if run["profile"] == name and run.get("physics_metadata") not in (None, {})), {})
            for name in args.profiles
        },
        "runs": runs,
    }
    write_json(args.output / "summary.json", summary)
    print(f"pursuit physics benchmark: wrote {len(runs)} runs to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Render and aggregate recorded pure-pursuit A/B experiment evidence.

The renderer uses only paths, maps, and telemetry recorded by the benchmark.
Failed or incomplete runs remain visible in the matrix and are never included
in a paired metric unless both runs in that pair succeeded.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

import matplotlib

matplotlib.use("Agg")

import matplotlib.pyplot as plt
import numpy as np
import yaml
from matplotlib.collections import PolyCollection
from matplotlib.patches import Polygon, Rectangle


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_GEOMETRY = ROOT / "navigation/config/front-drive-geometry.yaml"
CONTROLLERS = ("rpp", "rpp_dwpp", "vp", "vp_window")
SCENARIOS = ("S_curve", "narrow_turn", "reverse_cusp", "rotation_translate", "tail_sweep")
WIDE_SCENARIOS = frozenset(("S_curve", "reverse_cusp"))
DYNAMICS = ("ideal", "lagged")
START_VARIANTS = ("aligned", "perturbed")
CONTROLLER_COLORS = {
    "rpp": "#007f86",
    "rpp_dwpp": "#d07a18",
    "vp": "#6f58a5",
    "vp_window": "#bf4b54",
}
RESOLUTION_FALLBACK_M = 0.05
MAX_VIDEO_FRAMES = 240
FPS = 20


@dataclass(frozen=True)
class Pose:
    x: float
    y: float
    yaw: float


@dataclass(frozen=True)
class Telemetry:
    step: int
    seconds: float
    phase: str
    actual: Pose
    desired: Pose
    position_error_m: float
    yaw_error_rad: float
    command_linear_x: float
    command_angular_z: float
    actual_linear_x: float
    actual_angular_z: float
    full_footprint_clearance_m: float | None
    rear_zone_clearance_m: float | None
    collision: bool


@dataclass(frozen=True)
class GridMap:
    occupied: tuple[tuple[float, float, float, float], ...]
    resolution_m: float
    bounds: tuple[float, float, float, float] | None


@dataclass
class Run:
    raw: dict[str, Any]
    path: list[Pose]
    telemetry: list[Telemetry]
    map_data: GridMap
    artifacts: dict[str, Path]


def finite_float(value: Any, name: str) -> float:
    if value is None or value == "":
        raise ValueError(f"missing numeric value for {name}")
    number = float(value)
    if not math.isfinite(number):
        raise ValueError(f"non-finite value for {name}")
    return number


def optional_float(value: Any, name: str) -> float | None:
    if value is None or value == "":
        return None
    return finite_float(value, name)


def read_rows(path: Path, *, allow_empty: bool = False) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if not reader.fieldnames:
            raise ValueError(f"{path} has no CSV header")
        rows = list(reader)
    if not rows and not allow_empty:
        raise ValueError(f"{path} contains no rows")
    return rows


def first(row: dict[str, str], *names: str, required: bool = True) -> Any:
    for name in names:
        if name in row and row[name] not in (None, ""):
            return row[name]
    if required:
        raise ValueError(f"CSV is missing required column (one of {', '.join(names)})")
    return None


def read_path(path: Path) -> list[Pose]:
    rows = read_rows(path)
    poses: list[Pose] = []
    for row in rows:
        poses.append(
            Pose(
                finite_float(first(row, "x", "desired_x", "path_x"), "path x"),
                finite_float(first(row, "y", "desired_y", "path_y"), "path y"),
                finite_float(first(row, "yaw", "body_yaw", "desired_yaw", "path_yaw"), "path yaw"),
            )
        )
    return poses


def read_telemetry(path: Path) -> list[Telemetry]:
    rows = read_rows(path, allow_empty=True)
    samples: list[Telemetry] = []
    for index, row in enumerate(rows):
        samples.append(
            Telemetry(
                step=int(first(row, "step", "index", required=False) or index),
                seconds=finite_float(first(row, "sim_time_seconds", "time_seconds", "sim_time"), "simulation time"),
                phase=str(first(row, "phase", required=False) or ""),
                actual=Pose(
                    finite_float(first(row, "actual_x"), "actual x"),
                    finite_float(first(row, "actual_y"), "actual y"),
                    finite_float(first(row, "actual_yaw"), "actual yaw"),
                ),
                desired=Pose(
                    finite_float(first(row, "desired_x"), "desired x"),
                    finite_float(first(row, "desired_y"), "desired y"),
                    finite_float(first(row, "desired_yaw"), "desired yaw"),
                ),
                position_error_m=finite_float(first(row, "position_error_m"), "position error"),
                yaw_error_rad=finite_float(first(row, "yaw_error_rad"), "yaw error"),
                command_linear_x=finite_float(first(row, "command_linear_x"), "command linear x"),
                command_angular_z=finite_float(first(row, "command_angular_z"), "command angular z"),
                actual_linear_x=finite_float(first(row, "actual_linear_x"), "actual linear x"),
                actual_angular_z=finite_float(first(row, "actual_angular_z"), "actual angular z"),
                full_footprint_clearance_m=optional_float(
                    first(row, "full_footprint_clearance_m", required=False),
                    "full footprint clearance",
                ),
                rear_zone_clearance_m=optional_float(
                    first(row, "rear_zone_clearance_m", required=False),
                    "rear zone clearance",
                ),
                collision=str(first(row, "collision", required=False) or "false").lower()
                in {"1", "true", "yes"},
            )
        )
    if any(later.seconds < earlier.seconds for earlier, later in zip(samples, samples[1:])):
        raise ValueError(f"{path} simulation timestamps are not monotonic")
    return samples


def load_geometry(path: Path) -> list[tuple[float, float]]:
    with path.open(encoding="utf-8") as stream:
        data = yaml.safe_load(stream)
    if not isinstance(data, dict) or data.get("frame") != "base_footprint":
        raise ValueError(f"{path} must define base_footprint geometry")
    points = data.get("footprint")
    if not isinstance(points, list) or len(points) < 3:
        raise ValueError("geometry footprint must contain at least three vertices")
    footprint = [(finite_float(point[0], "footprint x"), finite_float(point[1], "footprint y")) for point in points]
    return footprint


def resolve_artifact(summary_path: Path, value: Any, field: str) -> Path:
    if not isinstance(value, str) or not value:
        raise ValueError(f"run artifact {field!r} must be a non-empty path")
    candidate = Path(value).expanduser()
    if candidate.is_absolute():
        options = [candidate]
    else:
        # The benchmark writes artifact paths relative to the summary file.
        options = [summary_path.parent / candidate, ROOT / candidate, summary_path.parent.parent / candidate]
    for option in options:
        if option.is_file():
            return option.resolve()
    raise FileNotFoundError(f"could not resolve {field} artifact {value!r} from {summary_path}")


def truthy(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    return str(value).strip().lower() in {"1", "true", "yes", "occupied", "blocked"}


def read_map(path: Path, experiment_config: dict[str, Any]) -> GridMap:
    """Read obstacle cells/rectangles from the benchmark's map CSV."""
    rows = read_rows(path)
    resolution_value: Any = rows[0].get("resolution_m") or experiment_config.get(
        "map_resolution_m", experiment_config.get("resolution_m")
    )
    resolution = finite_float(resolution_value, "map resolution") if resolution_value is not None else RESOLUTION_FALLBACK_M
    occupied_threshold = finite_float(experiment_config.get("occupied_threshold", 50), "occupied threshold")
    occupied: list[tuple[float, float, float, float]] = []
    min_x = min_y = math.inf
    max_x = max_y = -math.inf
    headers = set(rows[0])

    if {"cell_x", "cell_y", "world_x", "world_y", "occupancy"}.issubset(headers):
        # pursuit_benchmark.cpp records each cell's lower-left world coordinate
        # and the OccupancyGrid value (50 and above is occupied).
        for row in rows:
            x = finite_float(row["world_x"], "map world x")
            y = finite_float(row["world_y"], "map world y")
            occupancy = finite_float(row["occupancy"], "map occupancy")
            min_x, min_y = min(min_x, x), min(min_y, y)
            max_x, max_y = max(max_x, x + resolution), max(max_y, y + resolution)
            if occupancy >= occupied_threshold:
                occupied.append((x, y, resolution, resolution))
    elif {"x", "y", "width", "height"}.issubset(headers):
        for row in rows:
            if "occupied" in row and not truthy(row["occupied"]):
                continue
            x = finite_float(row["x"], "map x")
            y = finite_float(row["y"], "map y")
            width = finite_float(row["width"], "map width")
            height = finite_float(row["height"], "map height")
            if width <= 0 or height <= 0:
                raise ValueError(f"{path} has non-positive map rectangle size")
            occupied.append((x, y, width, height))
            min_x, min_y = min(min_x, x), min(min_y, y)
            max_x, max_y = max(max_x, x + width), max(max_y, y + height)
    elif {"x", "y", "occupied"}.issubset(headers):
        # Coordinates in this schema are world-space cell centers in metres.
        for row in rows:
            x = finite_float(row["x"], "map x")
            y = finite_float(row["y"], "map y")
            occupied_flag = truthy(row["occupied"])
            min_x, min_y = min(min_x, x - resolution / 2), min(min_y, y - resolution / 2)
            max_x, max_y = max(max_x, x + resolution / 2), max(max_y, y + resolution / 2)
            if occupied_flag:
                occupied.append((x - resolution / 2, y - resolution / 2, resolution, resolution))
    elif {"cell_x", "cell_y", "occupied"}.issubset(headers):
        origin_x = finite_float(experiment_config.get("map_origin_x_m", 0.0), "map origin x")
        origin_y = finite_float(experiment_config.get("map_origin_y_m", 0.0), "map origin y")
        for row in rows:
            x = origin_x + finite_float(row["cell_x"], "map cell x") * resolution
            y = origin_y + finite_float(row["cell_y"], "map cell y") * resolution
            min_x, min_y = min(min_x, x), min(min_y, y)
            max_x, max_y = max(max_x, x + resolution), max(max_y, y + resolution)
            if truthy(row["occupied"]):
                occupied.append((x, y, resolution, resolution))
    else:
        raise ValueError(
            f"unsupported map CSV schema in {path}; expected x,y,occupied; cell_x,cell_y,occupied; "
            "or x,y,width,height columns"
        )
    bounds = None if not math.isfinite(min_x) else (min_x, min_y, max_x, max_y)
    return GridMap(tuple(occupied), resolution, bounds)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def artifact_label(path: Path) -> str:
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return str(path.resolve())


def wrap_angle(value: float) -> float:
    return math.remainder(value, 2.0 * math.pi)


def world_footprint(footprint: list[tuple[float, float]], pose: Pose) -> np.ndarray:
    cosine, sine = math.cos(pose.yaw), math.sin(pose.yaw)
    return np.asarray([(pose.x + cosine * x - sine * y, pose.y + sine * x + cosine * y) for x, y in footprint])


def is_success(run: dict[str, Any] | None) -> bool:
    if run is None:
        return False
    outcome = run.get("outcome")
    if isinstance(outcome, bool):
        return outcome
    return str(outcome or "").strip().lower() in {
        "success", "succeeded", "pass", "passed", "complete", "completed", "goal_reached"
    }


def error_label(run: dict[str, Any] | None) -> str:
    if run is None:
        return ""
    value = run.get("error_code")
    return "" if value in (None, "", 0, "0") else f" [{value}]"


def pose_from_record(run: dict[str, Any] | None, key: str) -> Pose | None:
    if run is None:
        return None
    value = run.get(key)
    if not isinstance(value, (list, tuple)) or len(value) < 3:
        return None
    try:
        return Pose(*(finite_float(component, f"{key} component") for component in value[:3]))
    except (TypeError, ValueError):
        return None


def finite_metric(run: dict[str, Any] | None, key: str) -> float | None:
    if run is None:
        return None
    aliases = {
        "position_rms": ("position_rms", "position_rms_m"),
        "yaw_rms": ("yaw_rms", "body_yaw_rms_rad"),
    }
    value = next((run.get(name) for name in aliases.get(key, (key,)) if run.get(name) not in (None, "")), None)
    if value is None or value == "":
        return None
    number = float(value)
    return number if math.isfinite(number) else None


def metric_text(value: float | None, precision: int = 3) -> str:
    return "—" if value is None else f"{value:.{precision}f}"


def case_key(run: dict[str, Any]) -> tuple[str, str, str]:
    return (str(run["scenario"]), str(run["dynamics"]), str(run["start_variant"]))


def controller_pair_metrics(runs: dict[tuple[str, str, str, str], dict[str, Any]], cases: Iterable[tuple[str, str, str]]) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    case_list = list(cases)
    for left_index, left in enumerate(CONTROLLERS):
        for right in CONTROLLERS[left_index + 1 :]:
            common: list[tuple[float, float]] = []
            common_success = 0
            for scenario, dynamics, start_variant in case_list:
                a = runs.get((scenario, dynamics, start_variant, left))
                b = runs.get((scenario, dynamics, start_variant, right))
                if is_success(a) and is_success(b):
                    common_success += 1
                    pos_a, pos_b = finite_metric(a, "position_rms"), finite_metric(b, "position_rms")
                    yaw_a, yaw_b = finite_metric(a, "yaw_rms"), finite_metric(b, "yaw_rms")
                    if pos_a is not None and pos_b is not None and yaw_a is not None and yaw_b is not None:
                        common.append((pos_a - pos_b, yaw_a - yaw_b))
            rows.append(
                {
                    "controller_a": left,
                    "controller_b": right,
                    "common_success_cases": common_success,
                    "eligible_matched_cases": len(case_list),
                    "metric_complete_common_cases": len(common),
                    "mean_position_rms_delta_a_minus_b_m": (
                        sum(row[0] for row in common) / len(common) if common else None
                    ),
                    "mean_yaw_rms_delta_a_minus_b_rad": (
                        sum(row[1] for row in common) / len(common) if common else None
                    ),
                }
            )
    return rows


def build_aggregate(
    runs: dict[tuple[str, str, str, str], dict[str, Any]],
    cases: list[tuple[str, str, str]],
    all_run_count: int,
    selected_run_count: int,
    selection: dict[str, str | None],
    source_summary: Path,
    geometry_path: Path,
) -> dict[str, Any]:
    controller_summary: dict[str, Any] = {}
    for controller in CONTROLLERS:
        matching = [
            runs.get((*case, controller))
            for case in cases
        ]
        outcomes: dict[str, int] = {}
        for run in matching:
            label = "missing" if run is None else str(run.get("outcome", "unknown"))
            outcomes[label] = outcomes.get(label, 0) + 1
        successes = sum(is_success(run) for run in matching)
        controller_summary[controller] = {
            "matched_cases_denominator": len(cases),
            "recorded_runs": sum(run is not None for run in matching),
            "missing_runs": sum(run is None for run in matching),
            "successes": successes,
            "failures_or_non_successes": sum(run is not None and not is_success(run) for run in matching),
            "success_rate_over_matched_cases": successes / len(cases) if cases else None,
            "outcomes": outcomes,
        }
    common_all = [
        case for case in cases
        if all(is_success(runs.get((*case, controller))) for controller in CONTROLLERS)
    ]
    common_all_metrics: dict[str, Any] = {}
    for controller in CONTROLLERS:
        values_pos = [finite_metric(runs.get((*case, controller)), "position_rms") for case in common_all]
        values_yaw = [finite_metric(runs.get((*case, controller)), "yaw_rms") for case in common_all]
        pos_values = [value for value in values_pos if value is not None]
        yaw_values = [value for value in values_yaw if value is not None]
        common_all_metrics[controller] = {
            "all_controller_common_success_cases": len(common_all),
            "position_rms_metric_denominator": len(pos_values),
            "position_rms_mean_m": sum(pos_values) / len(pos_values) if pos_values else None,
            "yaw_rms_metric_denominator": len(yaw_values),
            "yaw_rms_mean_rad": sum(yaw_values) / len(yaw_values) if yaw_values else None,
        }
    return {
        "schema_version": 1,
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "selection": selection,
        "denominators": {
            "source_summary_runs": all_run_count,
            "selected_source_runs": selected_run_count,
            "expected_controller_run_cells": len(cases) * len(CONTROLLERS),
            "matched_cases": len(cases),
        },
        "controllers": controller_summary,
        "all_controller_common_success_metrics": {
            "common_success_cases": len(common_all),
            "eligible_matched_cases": len(cases),
            "by_controller": common_all_metrics,
        },
        "pairwise_common_success_metrics": controller_pair_metrics(runs, cases),
        "provenance": {
            "summary_json": {"path": artifact_label(source_summary), "sha256": sha256_file(source_summary)},
            "geometry_yaml": {"path": artifact_label(geometry_path), "sha256": sha256_file(geometry_path)},
        },
    }


def load_run_records(summary_path: Path, summary: dict[str, Any]) -> tuple[dict[tuple[str, str, str, str], dict[str, Any]], dict[tuple[str, str, str, str], Run]]:
    config = summary.get("experiment_config", summary.get("common_controller_config", {}))
    records = summary.get("runs")
    if not isinstance(config, dict) or not isinstance(records, list):
        raise ValueError("summary.json must contain common_controller_config (or experiment_config) and runs")
    raw_runs: dict[tuple[str, str, str, str], dict[str, Any]] = {}
    loaded: dict[tuple[str, str, str, str], Run] = {}
    path_cache: dict[str, list[Pose]] = {}
    map_cache: dict[str, GridMap] = {}
    for record in records:
        if not isinstance(record, dict):
            raise ValueError("every summary run must be an object")
        key = case_key(record) + (str(record["controller"]),)
        if key in raw_runs:
            raise ValueError(f"duplicate run record for {key}")
        raw_runs[key] = record
        artifacts = record.get("artifacts")
        if not isinstance(artifacts, dict):
            raise ValueError(f"run {record.get('id')} has no artifacts object")
        resolved = {
            name: resolve_artifact(summary_path, artifacts.get(name), name)
            for name in ("path_csv", "map_csv", "telemetry_csv")
        }
        path_hash = sha256_file(resolved["path_csv"])
        map_hash = sha256_file(resolved["map_csv"])
        if path_hash not in path_cache:
            path_cache[path_hash] = read_path(resolved["path_csv"])
        if map_hash not in map_cache:
            map_cache[map_hash] = read_map(resolved["map_csv"], config)
        loaded[key] = Run(
            raw=record,
            path=path_cache[path_hash],
            telemetry=read_telemetry(resolved["telemetry_csv"]),
            map_data=map_cache[map_hash],
            artifacts=resolved,
        )
    return raw_runs, loaded


def make_case_matrix(raw_runs: dict[tuple[str, str, str, str], dict[str, Any]]) -> list[tuple[str, str, str]]:
    cases = {
        (scenario, dynamics, start)
        for scenario in SCENARIOS
        for dynamics in DYNAMICS
        for start in START_VARIANTS
    }
    extra = {key[:3] for key in raw_runs} - cases
    if extra:
        raise ValueError(f"unexpected cases in summary: {sorted(extra)}")
    return sorted(cases, key=lambda case: (SCENARIOS.index(case[0]), DYNAMICS.index(case[1]), START_VARIANTS.index(case[2])))


def selected_cases(
    cases: list[tuple[str, str, str]],
    scenario: str | None,
    dynamics: str | None,
    start_variant: str | None,
) -> list[tuple[str, str, str]]:
    return [
        case for case in cases
        if (scenario is None or case[0] == scenario)
        and (dynamics is None or case[1] == dynamics)
        and (start_variant is None or case[2] == start_variant)
    ]


def add_map(ax: Any, map_data: GridMap) -> None:
    if map_data.occupied:
        cells = np.asarray(
            [[(x, y), (x + width, y), (x + width, y + height), (x, y + height)]
             for x, y, width, height in map_data.occupied],
            dtype=float,
        )
        ax.add_collection(
            PolyCollection(cells, facecolors="#bfc7d1", edgecolors="#7d8998", linewidths=0.35, zorder=0)
        )
    if map_data.bounds is not None:
        min_x, min_y, max_x, max_y = map_data.bounds
        ax.add_patch(Rectangle((min_x, min_y), max_x - min_x, max_y - min_y, fill=False, edgecolor="#8b95a3", linewidth=0.6, zorder=1))


def axes_limits(ax: Any, paths: list[list[Pose]], runs: list[Run], footprint: list[tuple[float, float]]) -> None:
    poses = [pose for path in paths for pose in path]
    poses.extend(sample.actual for run in runs for sample in run.telemetry)
    poses.extend(
        pose
        for run in runs
        for pose in (pose_from_record(run.raw, "initial_pose"), pose_from_record(run.raw, "final_pose"))
        if pose is not None
    )
    if not poses:
        return
    footprint_radius = max(math.hypot(x, y) for x, y in footprint)
    margin = max(footprint_radius + 0.2, 0.4)
    ax.set_xlim(min(pose.x for pose in poses) - margin, max(pose.x for pose in poses) + margin)
    ax.set_ylim(min(pose.y for pose in poses) - margin, max(pose.y for pose in poses) + margin)
    ax.set_aspect("equal", adjustable="box")
    ax.set_xlabel("Map x (m)")
    ax.set_ylabel("Map y (m)")
    ax.grid(color="#aab3c0", linewidth=0.45, alpha=0.28)


def render_trajectory_overview(
    output_path: Path,
    case: tuple[str, str, str],
    raw_runs: dict[tuple[str, str, str, str], dict[str, Any]],
    loaded: dict[tuple[str, str, str, str], Run],
    footprint: list[tuple[float, float]],
) -> None:
    scenario, dynamics, start_variant = case
    wide = scenario in WIDE_SCENARIOS
    figure_dpi = 128 if wide else 120
    figure_size = (14, 7) if wide else (13, 11)
    figure, axes = plt.subplots(2, 2, figsize=figure_size, dpi=figure_dpi, constrained_layout=True)
    all_loaded = [loaded[(scenario, dynamics, start_variant, c)] for c in CONTROLLERS if (scenario, dynamics, start_variant, c) in loaded]
    paths = [run.path for run in all_loaded]
    for ax, controller in zip(axes.flat, CONTROLLERS):
        key = (scenario, dynamics, start_variant, controller)
        raw = raw_runs.get(key)
        run = loaded.get(key)
        if run is None:
            ax.text(0.5, 0.5, "NO RECORDED RUN", transform=ax.transAxes, ha="center", va="center", color="#9b3a3a")
            ax.set_title(controller, loc="left")
            continue
        add_map(ax, run.map_data)
        plan = run.path
        ax.plot([p.x for p in plan], [p.y for p in plan], color="#333e4e", linestyle="--", linewidth=1.5, label="Fixed planned path", zorder=2)
        samples = run.telemetry
        color = CONTROLLER_COLORS[controller]
        if samples:
            start, finish = samples[0].actual, samples[-1].actual
            ax.plot([s.actual.x for s in samples], [s.actual.y for s in samples], color=color, linewidth=2.0, label="Recorded actual trace", zorder=4)
        else:
            start = pose_from_record(raw, "initial_pose")
            finish = pose_from_record(raw, "final_pose")
            ax.text(0.02, 0.02, "No telemetry samples; showing summary poses", transform=ax.transAxes, fontsize=7, color="#6c7787")
        if start is not None:
            ax.add_patch(Polygon(world_footprint(footprint, start), closed=True, fill=False, edgecolor="#26374d", linewidth=1.1, linestyle="--", zorder=5))
            ax.scatter([start.x], [start.y], marker="o", color="#26374d", s=26, zorder=6, label="Recorded start")
        if finish is not None:
            failed = not is_success(raw)
            endpoint_color = "#c62836" if failed else color
            ax.add_patch(Polygon(world_footprint(footprint, finish), closed=True, facecolor=endpoint_color, edgecolor=endpoint_color, alpha=0.22, linewidth=1.0, zorder=5))
            ax.scatter([finish.x], [finish.y], marker="X" if failed else "x", color=endpoint_color, s=54, linewidth=1.5, zorder=7,
                       label="Failure stop" if failed else "Recorded final pose")
        axes_limits(ax, [plan], [run], footprint)
        outcome = str(raw.get("outcome", "unknown")) if raw else "unknown"
        outcome_label = f"{outcome}{error_label(raw)}"
        ax.set_title(f"{controller} · {outcome_label}", loc="left", fontsize=11)
        ax.legend(loc="upper left", fontsize=8, framealpha=0.95)
    figure.suptitle(f"Recorded trajectory comparison · {scenario} · {dynamics} · {start_variant}", fontsize=15)
    figure.savefig(output_path, dpi=figure_dpi if wide else 170, facecolor="white")
    plt.close(figure)


def render_comparison(
    output_path: Path,
    runs: dict[tuple[str, str, str, str], dict[str, Any]],
    cases: list[tuple[str, str, str]],
    aggregate: dict[str, Any],
) -> None:
    figure = plt.figure(figsize=(16, max(10.0, 0.53 * len(cases) + 5.7)), dpi=110, constrained_layout=True)
    grid = figure.add_gridspec(3, 1, height_ratios=(0.85, max(4.0, 0.47 * len(cases)), 1.65))
    summary_ax = figure.add_subplot(grid[0])
    summary_ax.axis("off")
    summary_ax.set_title("Recorded A/B results · all matched cases remain in the matrix", loc="left", fontsize=15, pad=8)
    summary_cells = [[
        f"{controller}: {aggregate['controllers'][controller]['successes']}/"
        f"{aggregate['controllers'][controller]['matched_cases_denominator']} success"
        f"  ·  {aggregate['controllers'][controller]['failures_or_non_successes']} non-success"
        f"  ·  {aggregate['controllers'][controller]['missing_runs']} missing"
        for controller in CONTROLLERS
    ]]
    summary_table = summary_ax.table(cellText=summary_cells, cellLoc="center", colWidths=[0.25] * 4, loc="center")
    summary_table.auto_set_font_size(False)
    summary_table.set_fontsize(10)
    summary_table.scale(1, 1.7)

    matrix_ax = figure.add_subplot(grid[1])
    matrix_ax.axis("off")
    columns = ["Matched case"] + [f"{controller}\noutcome · position RMS (m)\nyaw RMS (rad)" for controller in CONTROLLERS]
    cell_rows: list[list[str]] = []
    cell_colors: list[list[str]] = []
    for scenario, dynamics, start_variant in cases:
        case_label = f"{scenario}\n{dynamics} · {start_variant}"
        row = [case_label]
        colors = ["#eef1f5"]
        for controller in CONTROLLERS:
            run = runs.get((scenario, dynamics, start_variant, controller))
            if run is None:
                row.append("MISSING RUN")
                colors.append("#f8d7da")
                continue
            outcome = str(run.get("outcome", "unknown"))
            pos = metric_text(finite_metric(run, "position_rms"))
            yaw = metric_text(finite_metric(run, "yaw_rms"))
            prefix = "PASS" if is_success(run) else "FAIL"
            label = f"{prefix} · {outcome}{error_label(run)}"
            row.append(f"{label}\npos {pos}  ·  yaw {yaw}")
            colors.append("#e1f1ec" if is_success(run) else "#f8d7da")
        cell_rows.append(row)
        cell_colors.append(colors)
    table = matrix_ax.table(cellText=cell_rows, colLabels=columns, cellLoc="center", colLoc="center", cellColours=cell_colors, colWidths=[0.17, 0.2075, 0.2075, 0.2075, 0.2075], loc="center")
    table.auto_set_font_size(False)
    table.set_fontsize(8.1 if len(cases) > 12 else 9.2)
    table.scale(1.0, 1.55 if len(cases) > 12 else 1.75)
    for (row, column), cell in table.get_celld().items():
        if row == 0:
            cell.set_facecolor("#dce3ec")
            cell.set_text_props(weight="bold", color="#1d2d40")
        elif column == 0:
            cell.set_text_props(weight="bold", color="#26374d")
        cell.set_edgecolor("#c5ced8")

    pair_ax = figure.add_subplot(grid[2])
    pair_ax.axis("off")
    pair_ax.set_title("Pairwise deltas on common successful runs with complete RMS metrics", loc="left", fontsize=11)
    pair_rows = []
    for item in aggregate["pairwise_common_success_metrics"]:
        pair_rows.append([
            f"{item['controller_a']} − {item['controller_b']}",
            f"{item['common_success_cases']}/{item['eligible_matched_cases']} both succeed"
            f" · {item['metric_complete_common_cases']} metric-complete",
            metric_text(item["mean_position_rms_delta_a_minus_b_m"], 4),
            metric_text(item["mean_yaw_rms_delta_a_minus_b_rad"], 4),
        ])
    pair_table = pair_ax.table(
        cellText=pair_rows,
        colLabels=["Controller pair", "Common success denominator", "Mean Δ position RMS (m) · A−B", "Mean Δ yaw RMS (rad) · A−B"],
        cellLoc="center", colLoc="center", colWidths=[0.2, 0.23, 0.285, 0.285], loc="center"
    )
    pair_table.auto_set_font_size(False)
    pair_table.set_fontsize(8.7)
    pair_table.scale(1.0, 1.35)
    for (row, _column), cell in pair_table.get_celld().items():
        if row == 0:
            cell.set_facecolor("#dce3ec")
            cell.set_text_props(weight="bold", color="#1d2d40")
        cell.set_edgecolor("#c5ced8")
    figure.savefig(output_path, dpi=170, facecolor="white")
    plt.close(figure)


def frame_indices(count: int) -> np.ndarray:
    if count <= MAX_VIDEO_FRAMES:
        return np.arange(count, dtype=int)
    return np.unique(np.linspace(0, count - 1, MAX_VIDEO_FRAMES, dtype=int))


def sample_at(samples: list[Telemetry], time_s: float) -> Telemetry:
    if time_s <= samples[0].seconds:
        return samples[0]
    if time_s >= samples[-1].seconds:
        return samples[-1]
    idx = int(np.searchsorted([sample.seconds for sample in samples], time_s, side="right"))
    before, after = samples[idx - 1], samples[idx]
    span = after.seconds - before.seconds
    if span <= 0:
        return after
    ratio = (time_s - before.seconds) / span
    lerp = lambda a, b: a + ratio * (b - a)
    yaw_delta = wrap_angle(after.actual.yaw - before.actual.yaw)
    actual = Pose(lerp(before.actual.x, after.actual.x), lerp(before.actual.y, after.actual.y), wrap_angle(before.actual.yaw + ratio * yaw_delta))
    desired = Pose(lerp(before.desired.x, after.desired.x), lerp(before.desired.y, after.desired.y), wrap_angle(before.desired.yaw + ratio * wrap_angle(after.desired.yaw - before.desired.yaw)))
    return Telemetry(
        step=after.step,
        seconds=time_s,
        phase=after.phase,
        actual=actual,
        desired=desired,
        position_error_m=lerp(before.position_error_m, after.position_error_m),
        yaw_error_rad=lerp(before.yaw_error_rad, after.yaw_error_rad),
        command_linear_x=lerp(before.command_linear_x, after.command_linear_x),
        command_angular_z=lerp(before.command_angular_z, after.command_angular_z),
        actual_linear_x=lerp(before.actual_linear_x, after.actual_linear_x),
        actual_angular_z=lerp(before.actual_angular_z, after.actual_angular_z),
        full_footprint_clearance_m=None,
        rear_zone_clearance_m=None,
        collision=before.collision or after.collision,
    )


def render_video(
    output_path: Path,
    case: tuple[str, str, str],
    raw_runs: dict[tuple[str, str, str, str], dict[str, Any]],
    loaded: dict[tuple[str, str, str, str], Run],
    footprint: list[tuple[float, float]],
) -> None:
    import imageio_ffmpeg

    scenario, dynamics, start_variant = case
    wide = scenario in WIDE_SCENARIOS
    keys = [(scenario, dynamics, start_variant, controller) for controller in CONTROLLERS]
    case_runs = [loaded.get(key) for key in keys]
    available = [run for run in case_runs if run is not None]
    telemetry_runs = [run for run in available if run.telemetry]
    duration = max((run.telemetry[-1].seconds for run in telemetry_runs), default=0.0)
    frame_times = np.linspace(0.0, duration, min(MAX_VIDEO_FRAMES, max(2, int(math.ceil(duration * FPS)) + 1)))
    figure, axes = plt.subplots(
        2, 2,
        figsize=(14, 7) if wide else (12, 10),
        dpi=128 if wide else 100,
        constrained_layout=True,
    )
    for ax, controller, key, run in zip(axes.flat, CONTROLLERS, keys, case_runs):
        raw = raw_runs.get(key)
        if run is None:
            ax.text(0.5, 0.5, "NO RECORDED RUN", transform=ax.transAxes, ha="center", va="center", color="#9b3a3a")
            ax.set_title(controller, loc="left")
            continue
        add_map(ax, run.map_data)
        ax.plot([p.x for p in run.path], [p.y for p in run.path], color="#3e4a59", linestyle="--", linewidth=1.2, label="Fixed path", zorder=2)
        outcome = str(raw.get("outcome", "unknown")) if raw else "unknown"
        ax.set_title(f"{controller} · {outcome}{error_label(raw)}", loc="left", fontsize=10)
        if not run.telemetry:
            final_pose = pose_from_record(raw, "final_pose")
            if final_pose is not None:
                failed = not is_success(raw)
                endpoint_color = "#c62836" if failed else CONTROLLER_COLORS[controller]
                endpoint_marker = "X" if failed else "x"
                endpoint_label = "Failure stop (summary pose)" if failed else "Final pose (summary)"
                ax.add_patch(Polygon(world_footprint(footprint, final_pose), closed=True, facecolor=endpoint_color, edgecolor=endpoint_color, alpha=0.2, linewidth=1.2, zorder=5))
                ax.scatter([final_pose.x], [final_pose.y], marker=endpoint_marker, color=endpoint_color, s=55, zorder=7, label=endpoint_label)
            ax.text(0.02, 0.02, "NO TELEMETRY SAMPLES", transform=ax.transAxes, fontsize=7, color="#9b3a3a")
            axes_limits(ax, [run.path], [run], footprint)
            ax.legend(loc="lower left", fontsize=7)
            continue
        trace, = ax.plot([], [], color=CONTROLLER_COLORS[controller], linewidth=2.0, label="Recorded actual trace", zorder=4)
        chassis = Polygon(world_footprint(footprint, run.telemetry[0].actual), closed=True, facecolor=CONTROLLER_COLORS[controller], edgecolor="#172b43", linewidth=1.4, alpha=0.4, zorder=5)
        ax.add_patch(chassis)
        point = ax.scatter([], [], marker="o", color=CONTROLLER_COLORS[controller], edgecolor="white", linewidth=0.5, s=35, zorder=6)
        failure_label = "Failure stop" if not is_success(raw) else "_nolegend_"
        failure_point = ax.scatter([], [], marker="X", color="#c62836", edgecolor="white", linewidth=0.5, s=65, zorder=8, label=failure_label)
        clock = ax.text(0.99, 0.99, "", transform=ax.transAxes, ha="right", va="top", fontsize=8, bbox={"boxstyle": "round,pad=0.25", "facecolor": "white", "alpha": 0.92, "edgecolor": "#c7d0dc"}, zorder=9)
        axes_limits(ax, [run.path], [run], footprint)
        ax.legend(loc="lower left", fontsize=7)
        # Stash artists and data for the synchronized frame loop.
        ax._vp_artists = (trace, chassis, point, failure_point, clock, run, raw)  # type: ignore[attr-defined]
    figure.suptitle(f"Synchronized virtual time · {scenario} · {dynamics} · {start_variant}", fontsize=14)
    width, height = (int(value) for value in figure.canvas.get_width_height())
    writer = imageio_ffmpeg.write_frames(
        str(output_path),
        (width, height),
        pix_fmt_in="rgb24",
        pix_fmt_out="yuv420p",
        fps=FPS,
        codec="libx264",
        macro_block_size=16,
        output_params=["-movflags", "+faststart"],
    )
    writer.send(None)
    try:
        for frame_index in frame_indices(len(frame_times)):
            t = float(frame_times[int(frame_index)])
            for ax in axes.flat:
                artists = getattr(ax, "_vp_artists", None)
                if artists is None:
                    continue
                trace, chassis, point, failure_point, clock, run, raw = artists
                last_time = run.telemetry[-1].seconds
                sample_time = min(t, last_time)
                sample = sample_at(run.telemetry, sample_time)
                trace_samples = [item for item in run.telemetry if item.seconds <= sample_time]
                if not trace_samples or trace_samples[-1].seconds < sample_time:
                    trace_samples.append(sample)
                trace.set_data([item.actual.x for item in trace_samples], [item.actual.y for item in trace_samples])
                chassis.set_xy(world_footprint(footprint, sample.actual))
                point.set_offsets([[sample.actual.x, sample.actual.y]])
                status = "RUNNING"
                if t >= last_time:
                    outcome = str(raw.get("outcome", "unknown")) if raw else "unknown"
                    status = f"FROZEN AT {last_time:.2f}s · {outcome.upper()}"
                    if not is_success(raw):
                        status = f"FROZEN ON FAILURE · {outcome.upper()} · {last_time:.2f}s"
                        failure_point.set_offsets([[sample.actual.x, sample.actual.y]])
                clock.set_text(f"Virtual t = {t:.2f}s\n{status}")
            figure.canvas.draw()
            pixels = np.asarray(figure.canvas.buffer_rgba(), dtype=np.uint8)[..., :3]
            if pixels.shape[1] != width or pixels.shape[0] != height:
                raise RuntimeError("matplotlib canvas changed size while rendering")
            writer.send(np.ascontiguousarray(pixels))
    finally:
        writer.close()
        plt.close(figure)


def render(
    summary_path: Path,
    output_dir: Path,
    geometry_path: Path,
    png_only: bool,
    scenario: str | None,
    dynamics: str | None,
    start_variant: str | None,
) -> dict[str, Any]:
    summary_path = summary_path.resolve()
    geometry_path = geometry_path.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    with summary_path.open(encoding="utf-8") as stream:
        summary = json.load(stream)
    raw_runs, loaded = load_run_records(summary_path, summary)
    all_cases = make_case_matrix(raw_runs)
    cases = selected_cases(all_cases, scenario, dynamics, start_variant)
    if not cases:
        raise ValueError("the requested scenario/dynamics/start selection has no matched cases")
    selected_run_count = sum(
        (case[0], case[1], case[2], controller) in raw_runs
        for case in cases for controller in CONTROLLERS
    )
    selection = {"scenario": scenario, "dynamics": dynamics, "start_variant": start_variant}
    selected_run_index = {
        key: value for key, value in raw_runs.items() if key[:3] in set(cases)
    }
    aggregate = build_aggregate(
        selected_run_index,
        cases,
        len(summary.get("runs", [])),
        selected_run_count,
        selection,
        summary_path,
        geometry_path,
    )
    aggregate_path = output_dir / "aggregate.json"
    comparison_path = output_dir / "comparison.png"
    aggregate["outputs"] = {
        "comparison_png": {"path": artifact_label(comparison_path)},
        "trajectory_overviews": [],
        "videos": [],
    }
    render_comparison(comparison_path, selected_run_index, cases, aggregate)

    footprint = load_geometry(geometry_path)
    # Three deliberately chosen geometries expose curved tracking, tight turns,
    # and the rear-corner sweep. CLI filters can narrow or select another case.
    preferred_scenarios = ("S_curve", "narrow_turn", "tail_sweep")
    trajectory_cases = [
        (name, dynamics or "ideal", start_variant or "aligned")
        for name in preferred_scenarios
        if (name, dynamics or "ideal", start_variant or "aligned") in cases
    ]
    if scenario is not None:
        trajectory_cases = [case for case in cases if case[0] == scenario]
    elif not trajectory_cases:
        trajectory_cases = [cases[0]]
    for case in trajectory_cases:
        path = output_dir / f"trajectory-{case[0]}-{case[1]}-{case[2]}.png"
        render_trajectory_overview(path, case, selected_run_index, loaded, footprint)
        aggregate["outputs"]["trajectory_overviews"].append({"path": artifact_label(path)})
    if not png_only:
        for case in trajectory_cases:
            path = output_dir / f"trajectory-{case[0]}-{case[1]}-{case[2]}.mp4"
            render_video(path, case, selected_run_index, loaded, footprint)
            aggregate["outputs"]["videos"].append({"path": artifact_label(path)})

    source_hashes: dict[str, dict[str, str]] = {
        artifact_label(summary_path): {"sha256": sha256_file(summary_path)},
        artifact_label(geometry_path): {"sha256": sha256_file(geometry_path)},
    }
    for key in selected_run_index:
        run = loaded.get(key)
        if run:
            for artifact_path in run.artifacts.values():
                source_hashes[artifact_label(artifact_path)] = {"sha256": sha256_file(artifact_path)}
    # Store final paths and hashes after all outputs have been generated.
    aggregate["outputs"]["comparison_png"]["sha256"] = sha256_file(comparison_path)
    for collection in ("trajectory_overviews", "videos"):
        for entry in aggregate["outputs"][collection]:
            output_path = output_dir / Path(entry["path"]).name
            entry["sha256"] = sha256_file(output_path)
    aggregate["provenance"]["source_artifacts"] = source_hashes
    aggregate["outputs"]["aggregate_json"] = {"path": artifact_label(aggregate_path)}
    # The companion metadata records the aggregate's hash after the final write.
    aggregate_path.write_text(json.dumps(aggregate, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    metadata_path = output_dir / "render-metadata.json"
    metadata = {
        "schema_version": 1,
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "renderer": {"path": artifact_label(Path(__file__)), "sha256": sha256_file(Path(__file__))},
        "sources": source_hashes,
        "outputs": {
            "aggregate_json": {"path": artifact_label(aggregate_path), "sha256": sha256_file(aggregate_path)},
            "comparison_png": {"path": artifact_label(comparison_path), "sha256": sha256_file(comparison_path)},
            "trajectory_overviews": aggregate["outputs"]["trajectory_overviews"],
            "videos": aggregate["outputs"]["videos"],
        },
    }
    metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return aggregate


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summary", required=True, type=Path, help="benchmark summary.json")
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--geometry", type=Path, default=DEFAULT_GEOMETRY)
    parser.add_argument("--png-only", action="store_true", help="omit MP4 output")
    parser.add_argument("--scenario", choices=SCENARIOS)
    parser.add_argument("--dynamics", choices=DYNAMICS)
    parser.add_argument("--start-variant", choices=START_VARIANTS)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    render(
        args.summary,
        args.output_dir,
        args.geometry,
        args.png_only,
        args.scenario,
        args.dynamics,
        args.start_variant,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

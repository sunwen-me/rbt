#!/usr/bin/env python3
"""Render recorded native lattice evidence as a top view and MP4.

The paths and poses come only from the fixture CSV files. The chassis outline
is transformed at each recorded drive pose using the configured footprint.
Scenario occupancy is copied from lattice_native_fixture.cpp::makeCells.
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
from typing import Any

import matplotlib

matplotlib.use("Agg")

import imageio_ffmpeg
import matplotlib.pyplot as plt
import numpy as np
import yaml
from matplotlib.patches import Polygon, Rectangle


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_GEOMETRY = ROOT / "navigation/config/front-drive-geometry.yaml"
RESOLUTION = 0.05


@dataclass(frozen=True)
class Pose:
    x: float
    y: float
    yaw: float


@dataclass(frozen=True)
class Telemetry:
    seconds: float
    drive: Pose
    base_link: Pose
    linear_x: float
    angular_z: float
    nav_state: int
    nav_error: int


@dataclass(frozen=True)
class ScenarioMap:
    width: int
    height: int
    occupied_rectangles: tuple[tuple[float, float, float, float], ...]
    tail_cell: tuple[float, float, float, float] | None = None


SCENARIO_MAPS = {
    # Fixture makeScenario/makeCells use 5 cm square grids, origin (0, 0).
    "turn": ScenarioMap(160, 160, ()),
    "reverse": ScenarioMap(
        160,
        160,
        (
            (0.0, 0.0, 8.0, 3.7),
            (0.0, 4.3, 8.0, 3.7),
        ),
    ),
    "tail_sweep": ScenarioMap(
        240,
        240,
        (),
        tail_cell=(112 * RESOLUTION, 120 * RESOLUTION, RESOLUTION, RESOLUTION),
    ),
}

FIGURE_SIZES = {
    "turn": (10.0, 10.0),
    "reverse": (14.4, 4.8),
    "tail_sweep": (8.0, 9.6),
}


def number(row: dict[str, str], name: str) -> float:
    value = float(row[name])
    if not math.isfinite(value):
        raise ValueError(f"non-finite value in CSV column {name}")
    return value


def read_path(path: Path) -> list[Pose]:
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    poses = [
        Pose(number(row, "drive_x"), number(row, "drive_y"), number(row, "drive_yaw"))
        for row in rows
    ]
    if not poses:
        raise ValueError(f"{path} contains no planned poses")
    return poses


def read_telemetry(path: Path) -> list[Telemetry]:
    with path.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    telemetry = [
        Telemetry(
            seconds=number(row, "relative_stamp_seconds"),
            drive=Pose(
                number(row, "drive_x"),
                number(row, "drive_y"),
                number(row, "drive_yaw"),
            ),
            base_link=Pose(
                number(row, "base_link_x"),
                number(row, "base_link_y"),
                number(row, "base_link_yaw"),
            ),
            linear_x=number(row, "linear_x"),
            angular_z=number(row, "angular_z"),
            nav_state=int(row["nav_state"]),
            nav_error=int(row["nav_error"]),
        )
        for row in rows
    ]
    if not telemetry:
        raise ValueError(f"{path} contains no recorded telemetry poses")
    if any(later.seconds < earlier.seconds for earlier, later in zip(telemetry, telemetry[1:])):
        raise ValueError(f"{path} timestamps are not monotonic")
    return telemetry


def load_geometry(path: Path) -> tuple[list[tuple[float, float]], Pose]:
    with path.open(encoding="utf-8") as stream:
        data = yaml.safe_load(stream)
    if not isinstance(data, dict) or data.get("frame") != "base_footprint":
        raise ValueError(f"{path} must define a base_footprint geometry")
    offset = data.get("drive_to_base")
    points = data.get("footprint")
    if not isinstance(offset, list) or len(offset) != 3:
        raise ValueError("geometry drive_to_base must contain x, y, yaw")
    if not isinstance(points, list) or len(points) < 3:
        raise ValueError("geometry footprint must contain at least three vertices")
    drive_to_base = Pose(*(float(value) for value in offset))
    footprint = [(float(point[0]), float(point[1])) for point in points]
    if not all(math.isfinite(value) for point in footprint for value in point):
        raise ValueError("geometry footprint contains a non-finite vertex")
    return footprint, drive_to_base


def compose(first: Pose, second: Pose) -> Pose:
    cosine = math.cos(first.yaw)
    sine = math.sin(first.yaw)
    return Pose(
        first.x + cosine * second.x - sine * second.y,
        first.y + sine * second.x + cosine * second.y,
        normalize_angle(first.yaw + second.yaw),
    )


def inverse(pose: Pose) -> Pose:
    cosine = math.cos(pose.yaw)
    sine = math.sin(pose.yaw)
    return Pose(
        -cosine * pose.x - sine * pose.y,
        sine * pose.x - cosine * pose.y,
        normalize_angle(-pose.yaw),
    )


def base_to_drive(base_pose: Pose, drive_to_base: Pose) -> Pose:
    return compose(base_pose, inverse(drive_to_base))


def normalize_angle(angle: float) -> float:
    return math.remainder(angle, 2.0 * math.pi)


def world_footprint(footprint: list[tuple[float, float]], pose: Pose) -> np.ndarray:
    cosine = math.cos(pose.yaw)
    sine = math.sin(pose.yaw)
    return np.asarray(
        [
            (pose.x + cosine * x - sine * y, pose.y + sine * x + cosine * y)
            for x, y in footprint
        ],
        dtype=float,
    )


def map_patches(ax: Any, scenario: ScenarioMap) -> None:
    for x, y, width, height in scenario.occupied_rectangles:
        ax.add_patch(
            Rectangle(
                (x, y),
                width,
                height,
                facecolor="#c8ced7",
                edgecolor="none",
                zorder=0,
            )
        )
    if scenario.tail_cell is not None:
        x, y, width, height = scenario.tail_cell
        ax.add_patch(
            Rectangle(
                (x, y),
                width,
                height,
                facecolor="#e45745",
                edgecolor="#a92d24",
                linewidth=1.2,
                zorder=3,
            )
        )
    ax.add_patch(
        Rectangle(
            (0.0, 0.0),
            scenario.width * RESOLUTION,
            scenario.height * RESOLUTION,
            fill=False,
            edgecolor="#8b95a3",
            linewidth=0.8,
            zorder=1,
        )
    )


def set_view(
    ax: Any,
    scenario: ScenarioMap,
    planned: list[Pose],
    telemetry: list[Telemetry],
    footprint: list[tuple[float, float]],
) -> None:
    radius = max(math.hypot(x, y) for x, y in footprint)
    poses = planned + [sample.drive for sample in telemetry]
    margin = max(0.45, radius + 0.18)
    min_x = min(pose.x for pose in poses) - margin
    max_x = max(pose.x for pose in poses) + margin
    min_y = min(pose.y for pose in poses) - margin
    max_y = max(pose.y for pose in poses) + margin
    map_max_x = scenario.width * RESOLUTION
    map_max_y = scenario.height * RESOLUTION
    min_x = max(0.0, min_x)
    min_y = max(0.0, min_y)
    max_x = min(map_max_x, max_x)
    max_y = min(map_max_y, max_y)
    ax.set_xlim(min_x, max_x)
    ax.set_ylim(min_y, max_y)
    ax.set_aspect("equal", adjustable="box")
    ax.set_xlabel("Map x (m)")
    ax.set_ylabel("Map y (m)")
    ax.grid(color="#aab3c0", linewidth=0.45, alpha=0.25)


def status_label(result: dict[str, Any]) -> str:
    if result.get("passed") is True:
        return "RECORDED NAVIGATION: PASS"
    error = result.get("error") or "no successful result recorded"
    return f"RECORDED NAVIGATION: NOT PASSED · {error}"


def tail_sweep_label(result: dict[str, Any]) -> str | None:
    required = (
        "direct_turn_start_free",
        "direct_turn_end_free",
        "direct_turn_sweep_free",
    )
    if not all(key in result for key in required):
        return None
    start = "free" if result["direct_turn_start_free"] else "blocked"
    end = "free" if result["direct_turn_end_free"] else "blocked"
    sweep = "free" if result["direct_turn_sweep_free"] else "blocked"
    return f"Fixture direct-turn check: start {start} · end {end} · sweep {sweep}"


def prepare_axes(
    ax: Any,
    scenario_name: str,
    scenario: ScenarioMap,
    planned: list[Pose],
    telemetry: list[Telemetry],
    footprint: list[tuple[float, float]],
    result: dict[str, Any],
) -> None:
    map_patches(ax, scenario)
    plan_x = [pose.x for pose in planned]
    plan_y = [pose.y for pose in planned]
    ax.plot(
        plan_x,
        plan_y,
        color="#d78a26",
        linestyle=(0, (5, 3)),
        linewidth=1.8,
        marker=".",
        markersize=3,
        label="Planned drive path",
        zorder=2,
    )
    ax.plot(
        [sample.base_link.x for sample in telemetry],
        [sample.base_link.y for sample in telemetry],
        color="#9a70b3",
        linewidth=1.0,
        linestyle=":",
        alpha=0.7,
        label="Recorded base_link trace",
        zorder=3,
    )
    ax.plot(
        [sample.drive.x for sample in telemetry],
        [sample.drive.y for sample in telemetry],
        color="#087e8b",
        linewidth=2.0,
        label="Recorded drive-axle trace",
        zorder=4,
    )

    start = telemetry[0]
    finish = telemetry[-1]
    start_polygon = world_footprint(footprint, start.drive)
    finish_polygon = world_footprint(footprint, finish.drive)
    ax.add_patch(
        Polygon(
            start_polygon,
            closed=True,
            fill=False,
            edgecolor="#53657d",
            linewidth=1.4,
            linestyle="--",
            label="Chassis at recorded start",
            zorder=5,
        )
    )
    ax.add_patch(
        Polygon(
            finish_polygon,
            closed=True,
            facecolor="#28a9b3",
            edgecolor="#086a73",
            linewidth=1.3,
            alpha=0.28,
            label="Chassis at recorded end",
            zorder=5,
        )
    )
    ax.scatter(
        [start.drive.x],
        [start.drive.y],
        color="#26374d",
        marker="*",
        s=90,
        label="Drive axle reference",
        zorder=7,
    )
    ax.scatter(
        [start.base_link.x],
        [start.base_link.y],
        color="#9a70b3",
        edgecolor="white",
        linewidth=0.7,
        marker="o",
        s=36,
        label="base_link reference",
        zorder=7,
    )
    goal_base_data = result.get("goal_base")
    drive_to_base_data = result.get("geometry_drive_to_base")
    if isinstance(goal_base_data, list) and len(goal_base_data) == 3 and isinstance(
        drive_to_base_data, list
    ) and len(drive_to_base_data) == 3:
        goal_base = Pose(*(float(value) for value in goal_base_data))
        drive_to_base = Pose(*(float(value) for value in drive_to_base_data))
        goal_drive = base_to_drive(goal_base, drive_to_base)
        ax.scatter(
            [goal_drive.x],
            [goal_drive.y],
            color="#202a38",
            marker="*",
            s=110,
            label="Goal drive reference",
            zorder=8,
        )
        ax.scatter(
            [goal_base.x],
            [goal_base.y],
            color="#9a70b3",
            marker="x",
            linewidth=1.8,
            s=46,
            label="Goal base_link reference",
            zorder=8,
        )

    set_view(ax, scenario, planned, telemetry, footprint)
    ax.set_title(f"Native lattice · {scenario_name.replace('_', ' ')}", loc="left", pad=14)
    ax.text(
        0.01,
        0.99,
        status_label(result),
        transform=ax.transAxes,
        ha="left",
        va="top",
        fontsize=9,
        color="#17324d",
        bbox={"boxstyle": "round,pad=0.35", "facecolor": "white", "alpha": 0.9, "edgecolor": "#c7d0dc"},
        zorder=10,
    )
    if scenario_name == "tail_sweep":
        detail = tail_sweep_label(result)
        if detail:
            ax.text(
                0.99,
                0.90,
                detail,
                transform=ax.transAxes,
                ha="right",
                va="top",
                fontsize=8,
                color="#8a322a",
                bbox={"boxstyle": "round,pad=0.35", "facecolor": "#fff8f2", "alpha": 0.95, "edgecolor": "#e7b8a5"},
                zorder=10,
            )
    ax.legend(loc="best", fontsize=8, framealpha=0.95, ncol=2)


def render_overview(
    output_path: Path,
    scenario_name: str,
    scenario: ScenarioMap,
    planned: list[Pose],
    telemetry: list[Telemetry],
    footprint: list[tuple[float, float]],
    result: dict[str, Any],
) -> None:
    figure, ax = plt.subplots(
        figsize=FIGURE_SIZES[scenario_name], dpi=120, constrained_layout=True
    )
    prepare_axes(ax, scenario_name, scenario, planned, telemetry, footprint, result)
    figure.savefig(output_path, dpi=180, facecolor="white")
    plt.close(figure)


def frame_indices(count: int, maximum: int = 180) -> np.ndarray:
    if count <= maximum:
        return np.arange(count, dtype=int)
    return np.unique(np.linspace(0, count - 1, maximum, dtype=int))


def render_animation(
    output_path: Path,
    scenario_name: str,
    scenario: ScenarioMap,
    planned: list[Pose],
    telemetry: list[Telemetry],
    footprint: list[tuple[float, float]],
    result: dict[str, Any],
) -> None:
    figure, ax = plt.subplots(
        figsize=FIGURE_SIZES[scenario_name], dpi=120, constrained_layout=True
    )
    prepare_axes(ax, scenario_name, scenario, planned, telemetry, footprint, result)
    actual_drive, = ax.plot([], [], color="#006d77", linewidth=2.8, zorder=6)
    actual_base, = ax.plot([], [], color="#9a70b3", linewidth=1.5, linestyle=":", zorder=6)
    current_chassis = Polygon(
        world_footprint(footprint, telemetry[0].drive),
        closed=True,
        facecolor="#11a6ad",
        edgecolor="#075b64",
        linewidth=1.7,
        alpha=0.42,
        zorder=9,
    )
    ax.add_patch(current_chassis)
    drive_point = ax.scatter([], [], marker="*", s=115, color="#13283f", zorder=11)
    base_point = ax.scatter([], [], marker="o", s=48, color="#9a70b3", edgecolor="white", zorder=11)
    heading_arrow = ax.quiver([], [], [], [], angles="xy", scale_units="xy", scale=1,
                              color="#13283f", width=0.006, zorder=11)
    clock_text = ax.text(
        0.99,
        0.76 if scenario_name == "tail_sweep" else 0.93,
        "",
        transform=ax.transAxes,
        ha="right",
        va="top",
        fontsize=10,
        color="#17324d",
        bbox={"boxstyle": "round,pad=0.3", "facecolor": "white", "alpha": 0.9, "edgecolor": "#c7d0dc"},
        zorder=12,
    )
    frame_size = tuple(int(value) for value in figure.canvas.get_width_height())
    writer = imageio_ffmpeg.write_frames(
        str(output_path),
        frame_size,
        pix_fmt_in="rgb24",
        pix_fmt_out="yuv420p",
        fps=20,
        codec="libx264",
        macro_block_size=16,
        output_params=["-movflags", "+faststart"],
    )
    writer.send(None)
    try:
        indices = frame_indices(len(telemetry))
        for frame_number, index in enumerate(indices):
            sample = telemetry[int(index)]
            trace = telemetry[: int(index) + 1]
            actual_drive.set_data([item.drive.x for item in trace], [item.drive.y for item in trace])
            actual_base.set_data(
                [item.base_link.x for item in trace],
                [item.base_link.y for item in trace],
            )
            current_chassis.set_xy(world_footprint(footprint, sample.drive))
            drive_point.set_offsets([[sample.drive.x, sample.drive.y]])
            base_point.set_offsets([[sample.base_link.x, sample.base_link.y]])
            arrow_length = 0.26
            heading_arrow.set_offsets([[sample.drive.x, sample.drive.y]])
            heading_arrow.set_UVC(
                arrow_length * math.cos(sample.drive.yaw),
                arrow_length * math.sin(sample.drive.yaw),
            )
            clock_text.set_text(
                f"Recorded t = {sample.seconds:.2f} s\n"
                f"v = {sample.linear_x:+.2f} m/s   ω = {sample.angular_z:+.2f} rad/s\n"
                f"Frame {frame_number + 1}/{len(indices)}"
            )
            figure.canvas.draw()
            pixels = np.asarray(figure.canvas.buffer_rgba(), dtype=np.uint8)[..., :3]
            if pixels.shape[1] != frame_size[0] or pixels.shape[0] != frame_size[1]:
                raise RuntimeError("matplotlib canvas changed size while rendering")
            writer.send(np.ascontiguousarray(pixels))
    finally:
        writer.close()
        plt.close(figure)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def artifact_path(path: Path) -> str:
    resolved = path.resolve()
    try:
        return resolved.relative_to(ROOT).as_posix()
    except ValueError:
        return str(resolved)


def render(
    evidence_dir: Path,
    output_dir: Path,
    geometry_path: Path,
    png_only: bool = False,
) -> tuple[Path, Path | None, Path]:
    evidence_dir = evidence_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    result_path = evidence_dir / "result.json"
    path_path = evidence_dir / "path.csv"
    telemetry_path = evidence_dir / "telemetry.csv"
    for required in (result_path, path_path, telemetry_path):
        if not required.is_file():
            raise FileNotFoundError(f"missing fixture evidence file: {required}")
    with result_path.open(encoding="utf-8") as stream:
        result = json.load(stream)
    scenario_name = str(result.get("scenario", ""))
    if scenario_name not in SCENARIO_MAPS:
        raise ValueError(f"unsupported lattice scenario in {result_path}: {scenario_name!r}")
    planned = read_path(path_path)
    telemetry = read_telemetry(telemetry_path)
    geometry_path = geometry_path.resolve()
    footprint, _drive_to_base = load_geometry(geometry_path)

    overview_path = output_dir / f"{scenario_name}-overview.png"
    animation_path = output_dir / f"{scenario_name}.mp4"
    render_overview(
        overview_path,
        scenario_name,
        SCENARIO_MAPS[scenario_name],
        planned,
        telemetry,
        footprint,
        result,
    )
    if not png_only:
        render_animation(
            animation_path,
            scenario_name,
            SCENARIO_MAPS[scenario_name],
            planned,
            telemetry,
            footprint,
            result,
        )

    metadata_path = output_dir / f"{scenario_name}-metadata.json"
    source_files = {
        "result_json": result_path,
        "path_csv": path_path,
        "telemetry_csv": telemetry_path,
        "geometry_yaml": geometry_path,
    }
    outputs = {"overview_png": overview_path}
    if not png_only:
        outputs["animation_mp4"] = animation_path
    metadata = {
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "scenario": scenario_name,
        "recorded_navigation_passed": result.get("passed") is True,
        "recorded_error": result.get("error", ""),
        "result_summary": {
            key: result[key]
            for key in (
                "direct_turn_start_free",
                "direct_turn_end_free",
                "direct_turn_sweep_free",
                "final_position_error",
                "final_yaw_error",
            )
            if key in result
        },
        "evidence": {
            name: {"path": artifact_path(path), "sha256": sha256_file(path)}
            for name, path in source_files.items()
        },
        "renderer": {
            "path": artifact_path(Path(__file__)),
            "sha256": sha256_file(Path(__file__)),
        },
        "map": {
            "width": SCENARIO_MAPS[scenario_name].width,
            "height": SCENARIO_MAPS[scenario_name].height,
            "resolution_m": RESOLUTION,
            "occupied_rectangles_m": SCENARIO_MAPS[scenario_name].occupied_rectangles,
            "tail_cell_m": SCENARIO_MAPS[scenario_name].tail_cell,
        },
        "recorded_samples": {
            "path_poses": len(planned),
            "telemetry_poses": len(telemetry),
            "first_stamp_seconds": telemetry[0].seconds,
            "last_stamp_seconds": telemetry[-1].seconds,
        },
        "outputs": {
            name: {"path": artifact_path(path), "sha256": sha256_file(path)}
            for name, path in outputs.items()
        },
    }
    with metadata_path.open("w", encoding="utf-8") as stream:
        json.dump(metadata, stream, indent=2, sort_keys=True)
        stream.write("\n")
    return overview_path, None if png_only else animation_path, metadata_path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", required=True, type=Path,
                        help="directory with result.json, path.csv, and telemetry.csv")
    parser.add_argument("--output-dir", required=True, type=Path,
                        help="directory for the PNG overview and MP4 animation")
    parser.add_argument("--geometry", type=Path, default=DEFAULT_GEOMETRY,
                        help=f"robot footprint YAML (default: {DEFAULT_GEOMETRY})")
    parser.add_argument("--png-only", action="store_true",
                        help="render only the overview PNG and metadata")
    args = parser.parse_args()
    overview, animation, metadata = render(
        args.evidence, args.output_dir, args.geometry, args.png_only
    )
    print(f"PNG overview: {overview}")
    if animation is not None:
        print(f"MP4 animation: {animation}")
    print(f"Render metadata: {metadata}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

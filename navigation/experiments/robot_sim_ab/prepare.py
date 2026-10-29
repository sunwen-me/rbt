#!/usr/bin/env python3
"""Prepare static Gazebo worlds and immutable fixtures for the A/B replay."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import shutil
import xml.etree.ElementTree as ET
from collections import defaultdict
from pathlib import Path


SCENARIOS = ("S_curve", "narrow_turn", "reverse_cusp")
SOURCE_ROOT = Path(__file__).resolve().parents[3]
RELEASE_ROOT = SOURCE_ROOT / "output/navigation-vp-experiment/release"
GEOMETRY_SOURCE = SOURCE_ROOT / "navigation/config/front-drive-geometry.yaml"
DISABLED_PLUGINS = {
    "RGLServerPluginManager",
    "gz-sim-sensors-system",
    "gz-sim-imu-system",
}


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def map_data(path: Path) -> tuple[dict, dict[int, dict[int, dict]], dict]:
    """Read the grid and index its rows and cells, validating its coordinates."""
    with path.open(newline="") as stream:
        reader = csv.DictReader(stream)
        rows = list(reader)
    if not rows:
        raise ValueError(f"empty map: {path}")
    meta = {
        key: float(rows[0][key])
        for key in ("width", "height", "resolution_m", "origin_x", "origin_y", "origin_yaw")
    }
    width, height = int(meta["width"]), int(meta["height"])
    resolution = meta["resolution_m"]
    if resolution <= 0 or abs(meta["origin_yaw"]) > 1e-9:
        raise ValueError(f"expected a positive-resolution, axis-aligned map: {path}")

    cells: dict[int, dict[int, dict]] = defaultdict(dict)
    coordinates: dict[tuple[int, int], tuple[float, float]] = {}
    for row in rows:
        x, y = int(row["cell_x"]), int(row["cell_y"])
        if not (0 <= x < width and 0 <= y < height) or x in cells[y]:
            raise ValueError(f"invalid or duplicate grid cell ({x}, {y}) in {path}")
        cell = {"occupancy": int(row["occupancy"]), "world_x": float(row["world_x"]),
                "world_y": float(row["world_y"])}
        cells[y][x] = cell
        coordinates[(x, y)] = (cell["world_x"], cell["world_y"])
    if len(rows) != width * height or any(len(row) != width for row in cells.values()):
        raise ValueError(f"map does not contain exactly width*height cells: {path}")

    base_x, base_y = coordinates[(0, 0)]
    for (x, y), (world_x, world_y) in coordinates.items():
        if (abs(world_x - (base_x + x * resolution)) > 1e-8
                or abs(world_y - (base_y + y * resolution)) > 1e-8):
            raise ValueError(f"world_x/world_y do not describe an axis-aligned grid: {path}")
    meta["grid_x"] = base_x
    meta["grid_y"] = base_y
    meta["width"], meta["height"] = width, height
    return meta, cells, coordinates


def occupied_rectangles(meta: dict, cells: dict[int, dict[int, dict]], coordinates: dict):
    """Merge identical horizontal occupied runs across adjacent rows."""
    row_runs: dict[int, tuple[tuple[int, int], ...]] = {}
    for y, row in cells.items():
        xs = sorted(x for x, cell in row.items() if cell["occupancy"] >= 50)
        runs = []
        if xs:
            start = previous = xs[0]
            for x in xs[1:]:
                if x != previous + 1:
                    runs.append((start, previous))
                    start = x
                previous = x
            runs.append((start, previous))
        row_runs[y] = tuple(runs)

    rectangles = []
    active: dict[tuple[int, int], tuple[int, int]] = {}
    previous_y = None
    for y in sorted(row_runs):
        runs = set(row_runs[y])
        if previous_y is None or y != previous_y + 1:
            rectangles.extend((x0, x1, y0, y1) for (x0, x1), (y0, y1) in active.items())
            active = {}
        for run in list(active):
            if run not in runs:
                y0, y1 = active.pop(run)
                rectangles.append((run[0], run[1], y0, y1))
        for run in runs:
            if run in active:
                active[run] = (active[run][0], y)
            else:
                active[run] = (y, y)
        previous_y = y
    rectangles.extend((x0, x1, y0, y1) for (x0, x1), (y0, y1) in active.items())

    occupied_count = sum(cell["occupancy"] >= 50 for row in cells.values() for cell in row.values())
    rectangle_cell_count = sum((x1 - x0 + 1) * (y1 - y0 + 1)
                               for x0, x1, y0, y1 in rectangles)
    if rectangle_cell_count != occupied_count:
        raise AssertionError("merged rectangles do not preserve occupied cell count")
    resolution = meta["resolution_m"]
    result = []
    for x0, x1, y0, y1 in rectangles:
        left, bottom = coordinates[(x0, y0)]
        result.append({
            "x": left + (x1 - x0 + 1) * resolution / 2,
            "y": bottom + (y1 - y0 + 1) * resolution / 2,
            "width": (x1 - x0 + 1) * resolution,
            "depth": (y1 - y0 + 1) * resolution,
            "cells": (x1 - x0 + 1) * (y1 - y0 + 1),
        })
    return result, occupied_count


def add_box(world: ET.Element, index: int, rectangle: dict) -> None:
    model = ET.SubElement(world, "model", {"name": f"occupied_cells_{index:04d}"})
    ET.SubElement(model, "static").text = "true"
    ET.SubElement(model, "pose").text = (
        f"{rectangle['x']:.12g} {rectangle['y']:.12g} 0.1 0 0 0"
    )
    link = ET.SubElement(model, "link", {"name": "link"})
    for kind in ("collision", "visual"):
        item = ET.SubElement(link, kind, {"name": kind})
        geometry = ET.SubElement(item, "geometry")
        box = ET.SubElement(geometry, "box")
        ET.SubElement(box, "size").text = (
            f"{rectangle['width']:.12g} {rectangle['depth']:.12g} 0.2"
        )
        if kind == "visual":
            material = ET.SubElement(item, "material")
            ET.SubElement(material, "ambient").text = "0.28 0.30 0.32 1"
            ET.SubElement(material, "diffuse").text = "0.35 0.37 0.40 1"


def initial_pose(path: Path) -> list[float]:
    with path.open(newline="") as stream:
        row = next(csv.DictReader(stream), None)
    if row is None:
        raise ValueError(f"empty path fixture: {path}")
    yaw_key = "body_yaw" if "body_yaw" in row else "yaw"
    return [float(row["x"]), float(row["y"]), float(row[yaw_key])]


def build_world(template: Path, output: Path, rectangles: list[dict]) -> list[dict]:
    tree = ET.parse(template)
    root = tree.getroot()
    world = root.find("world")
    if world is None:
        raise ValueError(f"SDF has no world element: {template}")
    removed = []
    for plugin in list(world.findall("plugin")):
        filename = plugin.get("filename", "")
        if filename in DISABLED_PLUGINS:
            removed.append({"filename": filename, "name": plugin.get("name", "")})
            world.remove(plugin)
    for index, rectangle in enumerate(rectangles):
        add_box(world, index, rectangle)
    output.parent.mkdir(parents=True, exist_ok=True)
    ET.indent(tree, space="  ")
    tree.write(output, encoding="utf-8", xml_declaration=True)
    return removed


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--world-template", type=Path, required=True,
                        help="Gazebo empty.sdf template")
    parser.add_argument("--output", type=Path, required=True,
                        help="experiment task directory")
    args = parser.parse_args()
    template = args.world_template.resolve()
    if not template.is_file():
        parser.error(f"world template does not exist: {template}")

    fixtures_dir = args.output / "fixtures"
    worlds_dir = args.output / "worlds"
    fixtures_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(GEOMETRY_SOURCE, fixtures_dir / "front-drive-geometry.yaml")
    manifest = {
        "schema_version": 1,
        "world_template": str(template),
        "world_template_sha256": sha256(template),
        "geometry_source": str(GEOMETRY_SOURCE),
        "geometry_sha256": sha256(GEOMETRY_SOURCE),
        "scenarios": {},
    }
    for scenario in SCENARIOS:
        source = RELEASE_ROOT / scenario / "ideal/aligned/rpp_dwpp"
        scenario_fixtures = fixtures_dir / scenario
        scenario_fixtures.mkdir(parents=True, exist_ok=True)
        path_source, map_source = source / "path.csv", source / "map.csv"
        path_output, map_output = scenario_fixtures / "path.csv", scenario_fixtures / "map.csv"
        shutil.copy2(path_source, path_output)
        shutil.copy2(map_source, map_output)
        meta, cells, coordinates = map_data(map_source)
        rectangles, occupied_count = occupied_rectangles(meta, cells, coordinates)
        world_output = worlds_dir / f"{scenario}.sdf"
        disabled = build_world(template, world_output, rectangles)
        manifest["scenarios"][scenario] = {
            "world": str(world_output.relative_to(args.output)),
            "world_sha256": sha256(world_output),
            "path_sha256": sha256(path_output),
            "map_sha256": sha256(map_output),
            "initial_pose": initial_pose(path_output),
            "occupied_cell_threshold": 50,
            "occupied_cell_count": occupied_count,
            "obstacle_box_count": len(rectangles),
            "obstacle_box_cells": sum(rect["cells"] for rect in rectangles),
            "resolution_m": meta["resolution_m"],
            "grid_lower_left_xy": [meta["grid_x"], meta["grid_y"]],
            "disabled_sensor_world_plugins": disabled,
        }
    manifest_path = args.output / "manifest.json"
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Exercise the experiment-only physics controller subprocess bridge."""

from __future__ import annotations

import argparse
import hashlib
import math
import subprocess
import tempfile
from pathlib import Path


CONTROLLERS = ("rpp", "rpp-dwpp", "vector-pursuit", "vector-pursuit-window")


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_fixtures(directory: Path) -> tuple[Path, Path]:
    path_file = directory / "path.csv"
    map_file = directory / "map.csv"
    path_file.write_text(
        "index,x,y,body_yaw,production_pose_free,production_segment_free\n"
        "0,1.0,2.0,0.0,1,1\n"
        "1,2.0,2.0,0.0,1,1\n",
        encoding="utf-8",
    )
    width = height = 80
    resolution = 0.05
    with map_file.open("w", encoding="utf-8", newline="") as output:
        output.write(
            "width,height,resolution_m,origin_x,origin_y,origin_yaw,cell_x,cell_y,"
            "world_x,world_y,occupancy\n"
        )
        for y in range(height):
            for x in range(width):
                output.write(
                    f"{width},{height},{resolution:.17g},0,0,0,{x},{y},"
                    f"{x * resolution:.17g},{y * resolution:.17g},0\n"
                )
    return path_file, map_file


def write_curved_path(directory: Path) -> Path:
    path_file = directory / "curved-path.csv"
    path_file.write_text(
        "index,x,y,body_yaw,production_pose_free,production_segment_free\n"
        "0,1.0,2.0,0.0,1,1\n"
        "1,2.0,2.5,1.0,1,1\n",
        encoding="utf-8",
    )
    return path_file


def command(binary: Path, geometry: Path, path_file: Path, map_file: Path,
            controller: str, *options: str) -> list[str]:
    return [
        str(binary), "--controller", controller,
        "--path", str(path_file), "--map", str(map_file),
        "--geometry", str(geometry),
        *options,
    ]


def parse_rows(stdout: str, expected: int) -> list[tuple[int, bool, float, float]]:
    lines = stdout.splitlines()
    if len(lines) != expected:
        raise AssertionError(f"expected {expected} response rows, got {len(lines)}: {stdout!r}")
    result = []
    for line in lines:
        fields = line.split()
        if len(fields) != 4:
            raise AssertionError(f"response must have four tokens: {line!r}")
        error = int(fields[0])
        reached_value = int(fields[1])
        linear = float(fields[2])
        angular = float(fields[3])
        if reached_value not in (0, 1) or not math.isfinite(linear) or not math.isfinite(angular):
            raise AssertionError(f"response contains invalid values: {line!r}")
        result.append((error, bool(reached_value), linear, angular))
    return result


def run(binary: Path, geometry: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="pursuit-physics-bridge-") as temporary:
        directory = Path(temporary)
        path_file, map_file = write_fixtures(directory)
        curved_path_file = write_curved_path(directory)
        original_hashes = (digest(path_file), digest(map_file))

        inputs = "0 0 0 1 2 0 0 0\n0.02 0.02 0.02 1 2 0 0 0\n"
        for controller in CONTROLLERS:
            result = subprocess.run(
                command(binary, geometry, path_file, map_file, controller),
                input=inputs, text=True, capture_output=True, check=False, timeout=10,
            )
            if result.returncode != 0:
                raise AssertionError(f"{controller} failed: {result.stderr}")
            rows = parse_rows(result.stdout, 2)
            if any(error != 0 or reached for error, reached, _, _ in rows):
                raise AssertionError(f"{controller} returned an unexpected status: {rows}")
            if not (0.0 < rows[0][2] < rows[1][2] <= 0.2):
                raise AssertionError(f"{controller} did not retain its previous command: {rows}")

        orientation_input = "".join(
            f"{index * 0.02:.2f} {index * 0.02:.2f} {index * 0.02:.2f} 1 2 0 0 0\n"
            for index in range(6)
        )

        def run_orientation_option(controller: str, *options: str) -> subprocess.CompletedProcess[str]:
            return subprocess.run(
                command(binary, geometry, curved_path_file, map_file, controller, *options),
                input=orientation_input, text=True, capture_output=True, check=False, timeout=10,
            )

        vp_default = run_orientation_option("vector-pursuit")
        vp_enabled = run_orientation_option("vector-pursuit", "--vp-use-heading-from-path", "1")
        vp_disabled = run_orientation_option("vector-pursuit", "--vp-use-heading-from-path", "0")
        for result in (vp_default, vp_enabled, vp_disabled):
            if result.returncode != 0:
                raise AssertionError(f"vector pursuit heading option failed: {result.stderr}")
            rows = parse_rows(result.stdout, 6)
            if rows[0][0] != 0 or rows[0][1]:
                raise AssertionError(f"vector pursuit returned an unexpected status: {rows}")
        if vp_default.stdout != vp_enabled.stdout:
            raise AssertionError("omitted vector pursuit heading option differs from explicit 1")
        if vp_default.stdout == vp_disabled.stdout:
            raise AssertionError("path heading switch had no effect for a curved VP fixture")

        rpp_default = run_orientation_option("rpp")
        rpp_disabled = run_orientation_option("rpp", "--vp-use-heading-from-path", "0")
        if rpp_default.returncode != 0 or rpp_disabled.returncode != 0:
            raise AssertionError("RPP rejected the vector pursuit heading option")
        if rpp_default.stdout != rpp_disabled.stdout:
            raise AssertionError("vector pursuit heading option changed pure pursuit output")

        invalid_orientation = run_orientation_option(
            "vector-pursuit", "--vp-use-heading-from-path", "2")
        if invalid_orientation.returncode == 0 or not invalid_orientation.stderr:
            raise AssertionError("invalid vector pursuit heading value was not rejected")

        duplicate_orientation = run_orientation_option(
            "vector-pursuit", "--vp-use-heading-from-path", "0",
            "--vp-use-heading-from-path", "1")
        if duplicate_orientation.returncode == 0 or not duplicate_orientation.stderr:
            raise AssertionError("duplicate vector pursuit heading option was not rejected")

        stale_then_fresh = subprocess.run(
            command(binary, geometry, path_file, map_file, "rpp"),
            input="1 0 1 1 2 0 0 0\n1.02 1.02 1.02 1 2 0 0 0\n",
            text=True, capture_output=True, check=False, timeout=10,
        )
        if stale_then_fresh.returncode != 0:
            raise AssertionError(f"stale-pose process failed: {stale_then_fresh.stderr}")
        stale_rows = parse_rows(stale_then_fresh.stdout, 2)
        if stale_rows[0] != (5, False, 0.0, 0.0):
            raise AssertionError(f"stale pose did not produce a finite StaleTf response: {stale_rows}")
        if stale_rows[1][0] != 0 or stale_rows[1][2] <= 0.0:
            raise AssertionError(f"controller did not recover after a stale pose: {stale_rows}")

        invalid_mode = subprocess.run(
            command(binary, geometry, path_file, map_file, "invalid"),
            input="", text=True, capture_output=True, check=False, timeout=10,
        )
        if invalid_mode.returncode == 0 or not invalid_mode.stderr:
            raise AssertionError("invalid controller mode was not rejected")

        invalid_input = subprocess.run(
            command(binary, geometry, path_file, map_file, "rpp"),
            input="0 0 0 1 2 0 0\n", text=True,
            capture_output=True, check=False, timeout=10,
        )
        if invalid_input.returncode == 0 or not invalid_input.stderr:
            raise AssertionError("malformed stdin input was not rejected")

        if (digest(path_file), digest(map_file)) != original_hashes:
            raise AssertionError("bridge modified a source CSV fixture")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--geometry", type=Path, required=True)
    args = parser.parse_args()
    run(args.binary, args.geometry)
    print("physics bridge subprocess checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

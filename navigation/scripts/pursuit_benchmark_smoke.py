#!/usr/bin/env python3
"""Run and validate the small deterministic pursuit benchmark experiment."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any


VARIANTS = {"rpp", "rpp_dwpp", "vp", "vp_window"}
METRIC_FIELDS = (
    "position_rms_m",
    "position_max_m",
    "body_yaw_rms_rad",
    "body_yaw_max_rad",
    "command_actual_linear_rms_mps",
    "command_actual_angular_rms_radps",
    "full_footprint_min_clearance_m",
    "rear_zone_min_clearance_m",
    "final_position_error_m",
    "final_yaw_error_rad",
)
COUNT_FIELDS = (
    "steps",
    "tracking_samples_moving",
    "command_linear_increment_exceedances",
    "command_angular_increment_exceedances",
)


def fail(message: str) -> None:
    raise ValueError(message)


def reject_constant(value: str) -> None:
    fail(f"non-standard JSON numeric constant: {value}")


def reject_non_finite(value: Any, location: str = "JSON") -> None:
    if isinstance(value, float) and not math.isfinite(value):
        fail(f"non-finite number at {location}")
    if isinstance(value, dict):
        for key, child in value.items():
            reject_non_finite(child, f"{location}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            reject_non_finite(child, f"{location}[{index}]")


def read_json(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as stream:
            value = json.load(stream, parse_constant=reject_constant)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        fail(f"cannot read strict JSON from {path}: {error}")
    if not isinstance(value, dict):
        fail(f"JSON root must be an object: {path}")
    reject_non_finite(value, path.name)
    return value


def artifact_file(root: Path, relative: Any, label: str) -> Path:
    if not isinstance(relative, str) or not relative:
        fail(f"missing {label} artifact path")
    resolved_root = root.resolve()
    path = (root / relative).resolve()
    try:
        path.relative_to(resolved_root)
    except ValueError:
        fail(f"{label} artifact escapes the output directory: {relative}")
    if not path.is_file():
        fail(f"{label} artifact is missing: {path}")
    return path


def validate_metric_fields(document: dict[str, Any], source: str) -> None:
    for field in METRIC_FIELDS:
        if field not in document:
            fail(f"{source} is missing metric {field}")
        value = document[field]
        if value is not None and (type(value) not in (int, float) or not math.isfinite(value)):
            fail(f"{source}.{field} is not finite or null")
    for field in COUNT_FIELDS:
        value = document.get(field)
        if type(value) is not int or value < 0:
            fail(f"{source}.{field} must be a nonnegative integer")
    task_seconds = document.get("task_seconds")
    if type(task_seconds) not in (int, float) or not math.isfinite(task_seconds) or task_seconds < 0:
        fail(f"{source}.task_seconds must be a finite nonnegative number")


def validate_output(output: Path) -> None:
    summary_path = output / "summary.json"
    summary = read_json(summary_path)
    runs = summary.get("runs")
    if not isinstance(runs, list) or len(runs) != 4:
        fail("smoke summary must contain exactly four runs")
    if summary.get("run_count") != 4:
        fail("smoke summary run_count must be four")

    by_variant: dict[str, dict[str, Any]] = {}
    expected_ids = {f"S_curve__{variant}__ideal__aligned" for variant in VARIANTS}
    for run in runs:
        if not isinstance(run, dict):
            fail("every summary run must be an object")
        variant = run.get("controller")
        if not isinstance(variant, str) or variant not in VARIANTS or variant in by_variant:
            fail(f"unexpected or duplicate controller variant: {variant}")
        by_variant[variant] = run
        if run.get("scenario") != "S_curve" or run.get("dynamics") != "ideal" or run.get("start_variant") != "aligned":
            fail(f"smoke run has unexpected fixture dimensions: {run.get('id')}")
        if run.get("outcome") not in {"success", "error", "timeout"}:
            fail(f"smoke run has invalid outcome: {run.get('id')}")
        validate_metric_fields(run, f"summary run {run.get('id')}")

    if set(by_variant) != VARIANTS:
        fail("smoke summary does not contain each required controller variant")
    if {run.get("id") for run in runs} != expected_ids:
        fail("smoke summary run ids do not match the four expected variants")

    path_hashes: set[str] = set()
    for variant in sorted(VARIANTS):
        run = by_variant[variant]
        artifacts = run.get("artifacts")
        if not isinstance(artifacts, dict):
            fail(f"summary run {run['id']} has no artifact map")
        paths = {
            key: artifact_file(output, artifacts.get(key), f"{run['id']} {key}")
            for key in ("path_csv", "map_csv", "telemetry_csv", "result_json")
        }
        path_hashes.add(hashlib.sha256(paths["path_csv"].read_bytes()).hexdigest())

        result = read_json(paths["result_json"])
        if result.get("id") != run.get("id"):
            fail(f"result JSON id does not match summary for {run['id']}")
        if result.get("outcome") != run.get("outcome"):
            fail(f"result JSON outcome does not match summary for {run['id']}")
        validate_metric_fields(result, f"result {run['id']}")
        result_artifacts = result.get("artifacts")
        if not isinstance(result_artifacts, dict):
            fail(f"result JSON for {run['id']} has no artifact map")
        run_directory = paths["result_json"].parent
        for name in ("path_csv", "map_csv", "telemetry_csv"):
            relative = result_artifacts.get(name)
            artifact_file(run_directory, relative, f"result {run['id']} {name}")

    if len(path_hashes) != 1:
        fail("the four controller variants did not receive byte-identical fixed paths")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--geometry", required=True, type=Path)
    parser.add_argument("--lattice", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    for label, path in (("binary", args.binary), ("geometry", args.geometry), ("lattice", args.lattice)):
        if not path.is_file():
            print(f"pursuit benchmark smoke: {label} does not exist: {path}", file=sys.stderr)
            return 1
    try:
        args.output.mkdir(parents=True, exist_ok=True)
        run_output = Path(tempfile.mkdtemp(prefix="pursuit-benchmark-smoke-", dir=args.output))
    except OSError as error:
        print(f"pursuit benchmark smoke: cannot create output directory: {error}", file=sys.stderr)
        return 1
    command = [
        str(args.binary),
        "--geometry", str(args.geometry),
        "--lattice", str(args.lattice),
        "--output", str(run_output),
        "--smoke",
    ]
    completed = subprocess.run(command, check=False)
    if completed.returncode != 0:
        print(f"pursuit benchmark smoke: benchmark exited with {completed.returncode}", file=sys.stderr)
        return completed.returncode or 1
    try:
        validate_output(run_output)
    except (OSError, ValueError) as error:
        print(f"pursuit benchmark smoke: {error}", file=sys.stderr)
        return 1
    print(f"pursuit benchmark smoke: four controller artifacts validated in {run_output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

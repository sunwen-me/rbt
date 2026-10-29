#!/usr/bin/env python3
"""Audit the saved navigation map against its source PCD and LIO configuration.

This is an offline consistency check. It does not change map data, calibration,
or configuration values, and it does not replace physical verification.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
from pathlib import Path
from typing import Any

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import yaml
from PIL import Image


Z_MIN_M = 0.30
Z_MAX_M = 2.00
PLANE_QUANTILE = 0.20
PLANE_MAX_TILT_DEG = 10.0
PLANE_INLIER_THRESHOLD_M = 0.03
PLANE_RANSAC_ITERATIONS = 1000


class AuditError(RuntimeError):
    """Raised when an input cannot be audited unambiguously."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_yaml(path: Path) -> dict[str, Any]:
    try:
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as exc:
        raise AuditError(f"cannot read YAML {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise AuditError(f"expected a YAML mapping in {path}")
    return value


def read_pcd_float32_xyzi(path: Path) -> np.ndarray:
    """Read the binary float32 XYZI PCD format used by the supplied map."""
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise AuditError(f"cannot read PCD {path}: {exc}") from exc

    data_header_start = data.find(b"\nDATA ")
    if data_header_start < 0:
        raise AuditError(f"unsupported PCD {path}: missing DATA header")
    header_end = data.find(b"\n", data_header_start + 1)
    if header_end < 0:
        raise AuditError(f"unsupported PCD {path}: DATA header is not terminated")
    payload_start = header_end + 1
    try:
        header = data[:header_end].decode("ascii").splitlines()
    except UnicodeDecodeError as exc:
        raise AuditError(f"unsupported PCD {path}: header is not ASCII") from exc

    values: dict[str, list[str]] = {}
    for line in header:
        fields = line.split()
        if fields:
            values[fields[0].upper()] = fields[1:]
    if values.get("DATA") != ["binary"]:
        mode = " ".join(values.get("DATA", [])) or "missing"
        raise AuditError(
            f"unsupported PCD {path}: expected DATA binary, found {mode}"
        )
    names = values.get("FIELDS")
    if names != ["x", "y", "z", "intensity"]:
        raise AuditError(
            "unsupported PCD fields: expected float32 x y z intensity in that order"
        )
    if values.get("SIZE") != ["4", "4", "4", "4"]:
        raise AuditError("unsupported PCD sizes: expected 4-byte fields")
    if values.get("TYPE") != ["F", "F", "F", "F"]:
        raise AuditError("unsupported PCD types: expected four floating-point fields")
    if values.get("COUNT", ["1"] * 4) != ["1", "1", "1", "1"]:
        raise AuditError("unsupported PCD counts: expected COUNT 1 for each field")
    try:
        width = int(values["WIDTH"][0])
        height = int(values["HEIGHT"][0])
        point_count = int(values["POINTS"][0])
    except (KeyError, IndexError, ValueError) as exc:
        raise AuditError(f"unsupported PCD {path}: invalid WIDTH/HEIGHT/POINTS") from exc
    if width <= 0 or height <= 0 or width * height != point_count:
        raise AuditError(f"unsupported PCD {path}: inconsistent point dimensions")

    payload = data[payload_start:]
    expected_bytes = point_count * 4 * 4
    if len(payload) != expected_bytes:
        raise AuditError(
            f"unsupported PCD {path}: expected {expected_bytes} binary point bytes, "
            f"found {len(payload)}"
        )
    return np.frombuffer(payload, dtype="<f4").reshape(point_count, 4).copy()


def _metadata_geometry(
    metadata: dict[str, Any],
) -> tuple[float, float, float, float, float, int, int, int]:
    try:
        xmin, xmax, ymin, ymax = map(float, metadata["xy_bounds_m"])
        resolution = float(metadata["resolution_m"])
        min_returns = int(metadata["minimum_returns_per_occupied_cell"])
    except (KeyError, TypeError, ValueError) as exc:
        raise AuditError(f"invalid projection geometry in metadata: {exc}") from exc
    if not all(math.isfinite(v) for v in (xmin, xmax, ymin, ymax, resolution)):
        raise AuditError("projection bounds and resolution must be finite")
    if xmax <= xmin or ymax <= ymin or resolution <= 0 or min_returns < 1:
        raise AuditError("projection bounds, resolution, or return threshold are invalid")
    width_float = (xmax - xmin) / resolution
    height_float = (ymax - ymin) / resolution
    width_steps = round(width_float)
    height_steps = round(height_float)
    if not math.isclose(width_float, width_steps, abs_tol=1e-6):
        raise AuditError("x bounds are not an integer number of grid cells apart")
    if not math.isclose(height_float, height_steps, abs_tol=1e-6):
        raise AuditError("y bounds are not an integer number of grid cells apart")
    return xmin, xmax, ymin, ymax, resolution, min_returns, width_steps + 1, height_steps + 1


def project_occupied_cells(
    points: np.ndarray, metadata: dict[str, Any]
) -> tuple[np.ndarray, dict[str, int]]:
    """Rebuild the original raw-z, minimum-return grid and flip it for PGM rows."""
    xmin, xmax, ymin, ymax, resolution, minimum_returns, width, height = (
        _metadata_geometry(metadata)
    )
    xyz = np.asarray(points[:, :3], dtype=np.float64)
    finite_xy = np.isfinite(xyz[:, :2]).all(axis=1)
    finite_xyz = np.isfinite(xyz).all(axis=1)
    in_crop = (
        finite_xy
        & (xyz[:, 0] >= xmin)
        & (xyz[:, 0] <= xmax)
        & (xyz[:, 1] >= ymin)
        & (xyz[:, 1] <= ymax)
    )
    projection_points = (
        in_crop
        & np.isfinite(xyz[:, 2])
        & (xyz[:, 2] >= Z_MIN_M)
        & (xyz[:, 2] < Z_MAX_M)
    )
    selected = xyz[projection_points]
    ix = np.floor((selected[:, 0] - xmin) / resolution).astype(np.int64)
    iy = np.floor((selected[:, 1] - ymin) / resolution).astype(np.int64)
    # The metadata bounds are inclusive; guard floating-point roundoff at max edges.
    ix = np.clip(ix, 0, width - 1)
    iy = np.clip(iy, 0, height - 1)
    counts = np.bincount(iy * width + ix, minlength=width * height).reshape(height, width)
    occupied = np.flipud(counts >= minimum_returns)
    return occupied, {
        "input_points": int(points.shape[0]),
        "finite_xyz_points": int(finite_xyz.sum()),
        "finite_xy_crop_points": int(in_crop.sum()),
        "z_filtered_projection_points": int(projection_points.sum()),
        "occupied_source_cells": int(np.count_nonzero(occupied)),
        "minimum_returns_per_occupied_cell": int(minimum_returns),
    }


def compare_occupancy(
    reconstructed: np.ndarray, map_pixels: np.ndarray
) -> dict[str, int | bool]:
    if reconstructed.shape != map_pixels.shape:
        raise AuditError(
            f"projection dimensions {reconstructed.shape[::-1]} do not match "
            f"map dimensions {map_pixels.shape[::-1]}"
        )
    if not np.isin(map_pixels, (0, 255)).all():
        values = np.unique(map_pixels)
        raise AuditError(
            "map PGM must contain only black (0) occupied and white (255) free cells; "
            f"found values {values.tolist()}"
        )
    map_occupied = map_pixels == 0
    false_positive = reconstructed & ~map_occupied
    false_negative = ~reconstructed & map_occupied
    return {
        "map_occupied_cells": int(map_occupied.sum()),
        "map_free_cells": int((~map_occupied).sum()),
        "matching_occupied_cells": int((reconstructed & map_occupied).sum()),
        "false_positive_cells": int(false_positive.sum()),
        "false_negative_cells": int(false_negative.sum()),
        "difference_cells": int(false_positive.sum() + false_negative.sum()),
        "exact_match": bool(np.array_equal(reconstructed, map_occupied)),
    }


def _fit_plane_least_squares(points: np.ndarray) -> np.ndarray | None:
    design = np.column_stack((points[:, 0], points[:, 1], np.ones(points.shape[0])))
    try:
        coeffs, _, rank, _ = np.linalg.lstsq(design, points[:, 2], rcond=None)
    except np.linalg.LinAlgError:
        return None
    if rank < 3 or not np.isfinite(coeffs).all():
        return None
    return coeffs


def estimate_candidate_plane(
    points: np.ndarray,
    map_origin_xy: tuple[float, float],
    *,
    seed: int = 0,
    quantile: float = PLANE_QUANTILE,
    threshold_m: float = PLANE_INLIER_THRESHOLD_M,
    max_tilt_deg: float = PLANE_MAX_TILT_DEG,
    iterations: int = PLANE_RANSAC_ITERATIONS,
) -> dict[str, Any]:
    """Fit a diagnostic low-z plane; it is deliberately not a floor calibration."""
    xyz = np.asarray(points[:, :3], dtype=np.float64)
    finite = np.isfinite(xyz).all(axis=1)
    xyz = xyz[finite]
    if xyz.shape[0] < 3:
        return {"available": False, "reason": "fewer than three finite in-crop points"}

    z_limit = float(np.quantile(xyz[:, 2], quantile))
    low = xyz[xyz[:, 2] <= z_limit]
    if low.shape[0] < 3:
        return {"available": False, "reason": "fewer than three low-z quantile points"}

    rng = np.random.default_rng(seed)
    best_mask: np.ndarray | None = None
    best_count = 0
    tangent_limit = math.tan(math.radians(max_tilt_deg))
    for _ in range(iterations):
        sample = low[rng.choice(low.shape[0], size=3, replace=False)]
        coeffs = _fit_plane_least_squares(sample)
        if coeffs is None or math.hypot(float(coeffs[0]), float(coeffs[1])) > tangent_limit:
            continue
        residuals = np.abs(low[:, 2] - (coeffs[0] * low[:, 0] + coeffs[1] * low[:, 1] + coeffs[2]))
        inliers = residuals <= threshold_m
        count = int(inliers.sum())
        if count > best_count:
            best_count = count
            best_mask = inliers
    if best_mask is None or best_count < 3:
        return {
            "available": False,
            "reason": "RANSAC found no supported plane within the tilt and residual limits",
            "low_z_quantile": quantile,
            "low_z_limit_m": z_limit,
            "candidate_points": int(low.shape[0]),
            "ransac_seed": int(seed),
            "ransac_iterations": int(iterations),
        }

    inliers = best_mask
    coeffs = None
    for _ in range(3):
        coeffs = _fit_plane_least_squares(low[inliers])
        if coeffs is None:
            return {"available": False, "reason": "least-squares plane fit was singular"}
        if math.hypot(float(coeffs[0]), float(coeffs[1])) > tangent_limit:
            return {
                "available": False,
                "reason": "least-squares refinement exceeds the maximum tilt",
                "ransac_support_points": int(best_mask.sum()),
            }
        residuals = np.abs(
            low[:, 2] - (coeffs[0] * low[:, 0] + coeffs[1] * low[:, 1] + coeffs[2])
        )
        updated = residuals <= threshold_m
        if np.array_equal(updated, inliers):
            break
        inliers = updated
    assert coeffs is not None
    support = low[inliers]
    residuals = np.abs(
        support[:, 2] - (coeffs[0] * support[:, 0] + coeffs[1] * support[:, 1] + coeffs[2])
    )
    a, b, c = map(float, coeffs)
    normal = np.array([-a, -b, 1.0], dtype=np.float64)
    normal /= np.linalg.norm(normal)
    return {
        "available": True,
        "role": "diagnostic candidate only",
        "physical_verification": False,
        "is_calibration": False,
        "low_z_quantile": quantile,
        "low_z_limit_m": z_limit,
        "candidate_points": int(low.shape[0]),
        "ransac_seed": int(seed),
        "ransac_iterations": int(iterations),
        "max_tilt_deg": float(max_tilt_deg),
        "ransac_support_points": int(best_mask.sum()),
        "support_points_after_refinement": int(support.shape[0]),
        "support_fraction_of_candidate_points": float(support.shape[0] / low.shape[0]),
        "equation": "z = a*x + b*y + c",
        "coefficients": {"a": a, "b": b, "c": c},
        "unit_normal": normal.tolist(),
        "tilt_deg": float(math.degrees(math.atan(math.hypot(a, b)))),
        "inlier_threshold_m": float(threshold_m),
        "median_absolute_residual_m": float(np.median(residuals)),
        "p95_absolute_residual_m": float(np.percentile(residuals, 95)),
        "xy_span_m": {
            "x_min": float(np.min(support[:, 0])),
            "x_max": float(np.max(support[:, 0])),
            "x_range": float(np.ptp(support[:, 0])),
            "y_min": float(np.min(support[:, 1])),
            "y_max": float(np.max(support[:, 1])),
            "y_range": float(np.ptp(support[:, 1])),
        },
        "height_at_map_origin_m": float(a * map_origin_xy[0] + b * map_origin_xy[1] + c),
        "interpretation": (
            "This low-z plane is not a physically verified floor and must not be "
            "used to change map coordinates, ground height, or extrinsics."
        ),
    }


def _rotation_matrix_xyz_radians(rpy: np.ndarray) -> np.ndarray:
    roll, pitch, yaw = map(float, rpy)
    cr, sr = math.cos(roll), math.sin(roll)
    cp, sp = math.cos(pitch), math.sin(pitch)
    cy, sy = math.cos(yaw), math.sin(yaw)
    rx = np.array([[1.0, 0.0, 0.0], [0.0, cr, -sr], [0.0, sr, cr]])
    ry = np.array([[cp, 0.0, sp], [0.0, 1.0, 0.0], [-sp, 0.0, cp]])
    rz = np.array([[cy, -sy, 0.0], [sy, cy, 0.0], [0.0, 0.0, 1.0]])
    return rz @ ry @ rx


def _rotation_validation(rotation: np.ndarray) -> dict[str, Any]:
    orthogonality_error = float(np.linalg.norm(rotation.T @ rotation - np.eye(3), ord="fro"))
    determinant = float(np.linalg.det(rotation))
    return {
        "orthogonality_error_fro": orthogonality_error,
        "determinant": determinant,
        "valid": bool(
            np.isfinite(rotation).all()
            and orthogonality_error <= 1e-6
            and abs(determinant - 1.0) <= 1e-6
        ),
    }


def audit_lio_extrinsics(config: dict[str, Any]) -> dict[str, Any]:
    try:
        lidar_values = np.asarray(config["lio.extrinsic.lidar_imu"], dtype=np.float64)
        odom_values = np.asarray(config["lio.extrinsic.odom_robo"], dtype=np.float64)
    except (KeyError, TypeError, ValueError) as exc:
        raise AuditError(f"invalid LIO extrinsic values: {exc}") from exc
    if lidar_values.shape != (12,) or odom_values.shape != (6,):
        raise AuditError(
            "expected lio.extrinsic.lidar_imu to have 12 values and "
            "lio.extrinsic.odom_robo to have 6 values"
        )
    if not np.isfinite(lidar_values).all() or not np.isfinite(odom_values).all():
        raise AuditError("LIO extrinsic values must all be finite")

    t_lidar_imu = lidar_values[:3]
    r_lidar_imu = lidar_values[3:].reshape(3, 3)
    lidar_rotation_check = _rotation_validation(r_lidar_imu)

    t_odom_robo = odom_values[:3]
    rpy_rad = np.radians(odom_values[3:])
    rpy_rotation = _rotation_matrix_xyz_radians(rpy_rad)
    # This matches the stored odom_robo convention in the LIO source.
    stored_rotation = rpy_rotation.T
    odom_rotation_check = _rotation_validation(stored_rotation)
    r_imu_base = -stored_rotation @ t_odom_robo
    r_odom_imu = stored_rotation.T
    sensor_offset = r_odom_imu @ (t_lidar_imu - r_imu_base)
    valid = bool(lidar_rotation_check["valid"] and odom_rotation_check["valid"])
    return {
        "valid": valid,
        "source_yaml_values_unchanged": True,
        "lidar_imu": {
            "translation_m": t_lidar_imu.tolist(),
            "rotation_row_major": r_lidar_imu.tolist(),
            **lidar_rotation_check,
        },
        "odom_robo": {
            "translation_values_m": t_odom_robo.tolist(),
            "rpy_values_deg": odom_values[3:].tolist(),
            "stored_rotation_convention": "(Rz(yaw) @ Ry(pitch) @ Rx(roll)).T",
            "stored_rotation": stored_rotation.tolist(),
            **odom_rotation_check,
        },
        "nominal_sensor_origin_relative_to_flat_base": {
            "translation_m": sensor_offset.tolist(),
            "calibrated": False,
            "interpretation": (
                "Computed from the current YAML extrinsics under the configured flat-base "
                "transform convention; it does not establish physical mounting accuracy."
            ),
        },
    }


def _load_map_image(map_yaml_path: Path) -> tuple[dict[str, Any], Path, np.ndarray]:
    map_config = read_yaml(map_yaml_path)
    try:
        image_name = str(map_config["image"])
        resolution = float(map_config["resolution"])
        origin = np.asarray(map_config["origin"], dtype=np.float64)
    except (KeyError, TypeError, ValueError) as exc:
        raise AuditError(f"invalid map YAML geometry: {exc}") from exc
    if not math.isfinite(resolution) or resolution <= 0:
        raise AuditError("map YAML resolution must be positive and finite")
    if origin.shape != (3,) or not np.isfinite(origin).all():
        raise AuditError("map YAML origin must contain three finite values")
    image_path = Path(image_name)
    if not image_path.is_absolute():
        image_path = map_yaml_path.parent / image_path
    try:
        with Image.open(image_path) as image:
            pixels = np.asarray(image.convert("L"), dtype=np.uint8)
    except OSError as exc:
        raise AuditError(f"cannot read map image {image_path}: {exc}") from exc
    return {
        "resolution_m": resolution,
        "origin_m": origin.tolist(),
        "mode": map_config.get("mode"),
        "occupied_thresh": map_config.get("occupied_thresh"),
        "free_thresh": map_config.get("free_thresh"),
        "negate": map_config.get("negate"),
        "width_cells": int(pixels.shape[1]),
        "height_cells": int(pixels.shape[0]),
    }, image_path, pixels


def _geometry_check(
    metadata: dict[str, Any], map_geometry: dict[str, Any]
) -> dict[str, Any]:
    xmin, xmax, ymin, ymax, resolution, _, width, height = _metadata_geometry(metadata)
    origin = map_geometry["origin_m"]
    checks = {
        "resolution_matches": math.isclose(
            resolution, map_geometry["resolution_m"], rel_tol=0.0, abs_tol=1e-9
        ),
        "origin_xy_matches_bounds_min": math.isclose(
            xmin, origin[0], rel_tol=0.0, abs_tol=1e-9
        )
        and math.isclose(ymin, origin[1], rel_tol=0.0, abs_tol=1e-9),
        "yaw_is_zero": math.isclose(origin[2], 0.0, rel_tol=0.0, abs_tol=1e-9),
        "dimensions_match_bounds": (
            width == map_geometry["width_cells"] and height == map_geometry["height_cells"]
        ),
    }
    return {
        **checks,
        "valid": all(checks.values()),
        "projection_bounds_m": [xmin, xmax, ymin, ymax],
        "projection_resolution_m": resolution,
        "projection_dimensions_cells": [width, height],
        "map_dimensions_cells": [map_geometry["width_cells"], map_geometry["height_cells"]],
    }


def write_overlay(
    path: Path,
    reconstructed: np.ndarray,
    map_pixels: np.ndarray,
    map_geometry: dict[str, Any],
    comparison: dict[str, int | bool],
) -> None:
    map_occupied = map_pixels == 0
    false_positive = reconstructed & ~map_occupied
    false_negative = ~reconstructed & map_occupied
    rgba = np.zeros((*reconstructed.shape, 4), dtype=np.float32)
    rgba[false_positive] = (1.0, 0.0, 0.75, 0.9)
    rgba[false_negative] = (1.0, 0.68, 0.0, 0.95)

    origin_x, origin_y, _ = map_geometry["origin_m"]
    resolution = map_geometry["resolution_m"]
    width, height = map_geometry["width_cells"], map_geometry["height_cells"]
    extent = (
        origin_x,
        origin_x + width * resolution,
        origin_y,
        origin_y + height * resolution,
    )
    fig, ax = plt.subplots(figsize=(11, 7), constrained_layout=True)
    ax.imshow(map_pixels, cmap="gray", vmin=0, vmax=255, origin="upper", extent=extent)
    ax.imshow(rgba, origin="upper", extent=extent, interpolation="nearest")
    ax.set_xlabel("map x (m)")
    ax.set_ylabel("map y (m)")
    ax.set_title(
        "PCD projection vs navigation map — "
        f"{comparison['difference_cells']} differing cells"
    )
    from matplotlib.patches import Patch

    ax.legend(
        handles=[
            Patch(color=(1.0, 0.0, 0.75), label="PCD occupied / map free"),
            Patch(color=(1.0, 0.68, 0.0), label="Map occupied / PCD free"),
        ],
        loc="best",
    )
    ax.set_aspect("equal", adjustable="box")
    fig.savefig(path, dpi=160)
    plt.close(fig)


def run_audit(args: argparse.Namespace) -> tuple[Path, Path, dict[str, Any]]:
    map_yaml_path = args.map.resolve()
    pcd_path = args.pcd.resolve()
    metadata_path = args.metadata.resolve()
    lio_config_path = args.lio_config.resolve()
    output_dir = args.output_dir.resolve()
    for path in (map_yaml_path, pcd_path, metadata_path, lio_config_path):
        if not path.is_file():
            raise AuditError(f"input file does not exist: {path}")

    try:
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise AuditError(f"cannot read metadata JSON {metadata_path}: {exc}") from exc
    if not isinstance(metadata, dict):
        raise AuditError("generation metadata must be a JSON object")

    map_geometry, map_image_path, map_pixels = _load_map_image(map_yaml_path)
    geometry = _geometry_check(metadata, map_geometry)
    points = read_pcd_float32_xyzi(pcd_path)
    reconstructed, point_counts = project_occupied_cells(points, metadata)
    comparison = compare_occupancy(reconstructed, map_pixels)
    lio_extrinsics = audit_lio_extrinsics(read_yaml(lio_config_path))
    source_xyz = np.asarray(points[:, :3], dtype=np.float64)
    xmin, xmax, ymin, ymax, _, _, _, _ = _metadata_geometry(metadata)
    finite_crop = (
        np.isfinite(source_xyz).all(axis=1)
        & (source_xyz[:, 0] >= xmin)
        & (source_xyz[:, 0] <= xmax)
        & (source_xyz[:, 1] >= ymin)
        & (source_xyz[:, 1] <= ymax)
    )
    plane = estimate_candidate_plane(
        source_xyz[finite_crop], (map_geometry["origin_m"][0], map_geometry["origin_m"][1])
    )
    source_hash_matches = metadata.get("sha256") == sha256_file(pcd_path)
    metadata_counts_match = (
        point_counts["input_points"] == int(metadata.get("input_points", -1))
        and point_counts["finite_xy_crop_points"]
        == int(metadata.get("finite_xy_crop_points", -1))
    )
    alignment_valid = bool(
        geometry["valid"]
        and comparison["exact_match"]
        and source_hash_matches
        and metadata_counts_match
    )
    report: dict[str, Any] = {
        "schema_version": 1,
        "audit_kind": "offline geometric consistency audit",
        "alignment_verdict": "PASS_EXACT_GRID_MATCH" if alignment_valid else "FAIL_ALIGNMENT_MISMATCH",
        "alignment_valid": alignment_valid,
        "inputs": {
            "map_yaml": str(map_yaml_path),
            "map_image": str(map_image_path.resolve()),
            "pcd": str(pcd_path),
            "generation_metadata": str(metadata_path),
            "lio_config": str(lio_config_path),
            "sha256": {
                "map_yaml": sha256_file(map_yaml_path),
                "map_image": sha256_file(map_image_path),
                "pcd": sha256_file(pcd_path),
                "generation_metadata": sha256_file(metadata_path),
                "lio_config": sha256_file(lio_config_path),
            },
        },
        "map_geometry": {**map_geometry, **geometry},
        "projection": {
            **point_counts,
            "metadata_input_points": metadata.get("input_points"),
            "metadata_finite_xy_crop_points": metadata.get("finite_xy_crop_points"),
            "metadata_counts_match_input": bool(metadata_counts_match),
            "raw_z_range_m": [Z_MIN_M, Z_MAX_M],
            "z_upper_bound_inclusive": False,
            "row_orientation": "flip image rows after mapping increasing y to grid rows",
            "occupied_cell_rule": "at least metadata minimum returns in one grid cell",
            "metadata_source_pcd_sha256": metadata.get("sha256"),
            "metadata_source_pcd_sha256_matches_input": bool(source_hash_matches),
        },
        "occupancy_comparison": comparison,
        "lio_extrinsic_audit": lio_extrinsics,
        "candidate_low_z_plane": plane,
        "scope": {
            "map_or_extrinsic_values_modified": False,
            "physical_hardware_input_used": False,
            "floor_physically_verified": False,
            "calibration_claimed": False,
            "note": (
                "This checks internal PCD-to-map projection consistency and reports "
                "nominal YAML geometry. It cannot physically verify floor height, "
                "extrinsics, or localization alignment."
            ),
        },
    }

    output_dir.mkdir(parents=True, exist_ok=True)
    report_path = output_dir / "alignment-audit.json"
    overlay_path = output_dir / "alignment-overlay.png"
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_overlay(overlay_path, reconstructed, map_pixels, map_geometry, comparison)
    return report_path, overlay_path, report


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Rebuild a binary float32 XYZI PCD grid from generation metadata, "
            "compare it with the map PGM, and report read-only LIO extrinsic geometry."
        )
    )
    parser.add_argument("--map", required=True, type=Path, help="navigation map YAML")
    parser.add_argument("--pcd", required=True, type=Path, help="binary float32 XYZI PCD")
    parser.add_argument("--metadata", required=True, type=Path, help="map generation metadata JSON")
    parser.add_argument("--lio-config", required=True, type=Path, help="LIO YAML configuration")
    parser.add_argument("--output-dir", required=True, type=Path, help="directory for JSON and PNG outputs")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        report_path, overlay_path, report = run_audit(args)
    except AuditError as exc:
        print(f"map alignment audit: error: {exc}", file=sys.stderr)
        return 2
    print(f"alignment verdict: {report['alignment_verdict']}")
    print(f"occupied cells: {report['occupancy_comparison']['map_occupied_cells']}")
    print(f"difference cells: {report['occupancy_comparison']['difference_cells']}")
    print(f"report: {report_path}")
    print(f"overlay: {overlay_path}")
    return 0 if report["alignment_valid"] and report["lio_extrinsic_audit"]["valid"] else 1


if __name__ == "__main__":
    sys.exit(main())

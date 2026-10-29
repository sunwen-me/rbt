#!/usr/bin/env python3
"""Audit the simulator LiDAR mount against the existing LIO extrinsic formulas.

This is an offline source/config consistency check. It does not expand xacro,
start ROS or Gazebo, or verify the physical robot mount.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import sys
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import Any

import numpy as np
import yaml


TRANSLATION_TOLERANCE_M = 1e-6
ROTATION_TOLERANCE_DEG = 1e-5


class AuditError(RuntimeError):
    """Raised when a source transform cannot be read unambiguously."""


def _local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1].split(":")[-1]


def _elements(parent: ET.Element, name: str) -> list[ET.Element]:
    return [element for element in parent.iter() if _local_name(element.tag) == name]


def _direct_children(parent: ET.Element, name: str) -> list[ET.Element]:
    return [element for element in parent if _local_name(element.tag) == name]


def _read_xml(path: Path) -> ET.Element:
    try:
        return ET.parse(path).getroot()
    except (OSError, ET.ParseError) as exc:
        raise AuditError(f"cannot parse XML {path}: {exc}") from exc


def _read_yaml(path: Path) -> dict[str, Any]:
    try:
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as exc:
        raise AuditError(f"cannot read YAML {path}: {exc}") from exc
    if not isinstance(value, dict):
        raise AuditError(f"expected a YAML mapping in {path}")
    return value


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
    except OSError as exc:
        raise AuditError(f"cannot hash input {path}: {exc}") from exc
    return digest.hexdigest()


def _numbers(text: str | None, count: int, description: str) -> np.ndarray:
    if text is None or not text.strip():
        raise AuditError(f"{description} is missing")
    try:
        values = np.asarray([float(value) for value in text.split()], dtype=np.float64)
    except ValueError as exc:
        raise AuditError(f"{description} must contain explicit numeric values: {text!r}") from exc
    if values.shape != (count,):
        raise AuditError(f"{description} must contain {count} values, found {values.size}")
    if not np.isfinite(values).all():
        raise AuditError(f"{description} must contain only finite values")
    return values


def _xyz_rpy(element: ET.Element, description: str) -> tuple[np.ndarray, np.ndarray]:
    unsupported = set(element.attrib) - {"xyz", "rpy"}
    if unsupported:
        raise AuditError(f"{description} has unsupported attributes: {sorted(unsupported)}")
    xyz = _numbers(element.get("xyz"), 3, f"{description} xyz")
    rpy = _numbers(element.get("rpy"), 3, f"{description} rpy")
    return xyz, rpy


def _rotation_xyz(rpy: np.ndarray) -> np.ndarray:
    roll, pitch, yaw = map(float, rpy)
    cr, sr = math.cos(roll), math.sin(roll)
    cp, sp = math.cos(pitch), math.sin(pitch)
    cy, sy = math.cos(yaw), math.sin(yaw)
    rx = np.array([[1.0, 0.0, 0.0], [0.0, cr, -sr], [0.0, sr, cr]])
    ry = np.array([[cp, 0.0, sp], [0.0, 1.0, 0.0], [-sp, 0.0, cp]])
    rz = np.array([[cy, -sy, 0.0], [sy, cy, 0.0], [0.0, 0.0, 1.0]])
    return rz @ ry @ rx


def _transform(translation: np.ndarray, rotation: np.ndarray) -> np.ndarray:
    result = np.eye(4, dtype=np.float64)
    result[:3, :3] = rotation
    result[:3, 3] = translation
    return result


def _transform_from_xyz_rpy(xyz: np.ndarray, rpy: np.ndarray) -> np.ndarray:
    return _transform(xyz, _rotation_xyz(rpy))


def _transform_report(matrix: np.ndarray) -> dict[str, Any]:
    return {
        "translation_m": matrix[:3, 3].tolist(),
        "rotation_row_major": matrix[:3, :3].tolist(),
        "matrix_row_major": matrix.tolist(),
    }


def _rotation_error_deg(expected: np.ndarray, actual: np.ndarray) -> float:
    relative = actual.T @ expected
    cosine = (float(np.trace(relative)) - 1.0) / 2.0
    return math.degrees(math.acos(max(-1.0, min(1.0, cosine))))


def _compare_transforms(expected: np.ndarray, actual: np.ndarray) -> dict[str, Any]:
    translation_delta = expected[:3, 3] - actual[:3, 3]
    translation_error = float(np.linalg.norm(translation_delta))
    rotation_error = _rotation_error_deg(expected[:3, :3], actual[:3, :3])
    return {
        "translation_delta_m": translation_delta.tolist(),
        "translation_error_m": translation_error,
        "rotation_error_deg": rotation_error,
        "within_tolerance": bool(
            translation_error <= TRANSLATION_TOLERANCE_M
            and rotation_error <= ROTATION_TOLERANCE_DEG
        ),
    }


def _rotation_is_valid(rotation: np.ndarray, description: str) -> None:
    orthogonality_error = float(np.linalg.norm(rotation.T @ rotation - np.eye(3), ord="fro"))
    determinant = float(np.linalg.det(rotation))
    if (
        not np.isfinite(rotation).all()
        or orthogonality_error > 1e-6
        or abs(determinant - 1.0) > 1e-6
    ):
        raise AuditError(
            f"{description} must be a finite proper rotation matrix "
            f"(orthogonality error {orthogonality_error:.3g}, determinant {determinant:.9g})"
        )


def _joint_chain(
    root: ET.Element, base_frame: str, target_frame: str, source: str
) -> tuple[np.ndarray, list[dict[str, Any]]]:
    links = {
        link.get("name")
        for link in _elements(root, "link")
        if link.get("name") is not None
    }
    if base_frame not in links:
        raise AuditError(f"{source} does not explicitly declare base link {base_frame!r}")
    if target_frame not in links:
        raise AuditError(f"{source} does not explicitly declare target link {target_frame!r}")

    by_child: dict[str, tuple[str, ET.Element]] = {}
    for joint in _elements(root, "joint"):
        parents = _direct_children(joint, "parent")
        children = _direct_children(joint, "child")
        if len(parents) != 1 or len(children) != 1:
            continue
        parent = parents[0].get("link")
        child = children[0].get("link")
        if parent is None or child is None:
            continue
        if child in by_child:
            raise AuditError(f"{source} has multiple joints parenting link {child!r}")
        by_child[child] = (parent, joint)

    reverse_steps: list[tuple[str, str, ET.Element]] = []
    seen = {target_frame}
    current = target_frame
    while current != base_frame:
        item = by_child.get(current)
        if item is None:
            raise AuditError(
                f"{source} has no explicit joint path from {base_frame!r} to {target_frame!r}; "
                f"stopped at {current!r}"
            )
        parent, joint = item
        if parent not in links:
            raise AuditError(f"{source} joint parent link {parent!r} is undeclared")
        if parent in seen:
            raise AuditError(f"{source} joint path contains a cycle at {parent!r}")
        seen.add(parent)
        reverse_steps.append((parent, current, joint))
        current = parent

    matrix = np.eye(4, dtype=np.float64)
    chain: list[dict[str, Any]] = []
    for parent, child, joint in reversed(reverse_steps):
        name = joint.get("name") or "<unnamed>"
        joint_type = joint.get("type")
        if joint_type != "fixed":
            raise AuditError(
                f"{source} joint {name!r} from {parent!r} to {child!r} is {joint_type!r}, not fixed"
            )
        origins = _direct_children(joint, "origin")
        if len(origins) != 1:
            raise AuditError(f"{source} fixed joint {name!r} needs one explicit origin")
        xyz, rpy = _xyz_rpy(origins[0], f"{source} joint {name!r} origin")
        step = _transform_from_xyz_rpy(xyz, rpy)
        matrix = matrix @ step
        chain.append(
            {
                "joint": name,
                "type": "fixed",
                "parent_frame": parent,
                "child_frame": child,
                "origin_xyz_m": xyz.tolist(),
                "origin_rpy_rad": rpy.tolist(),
            }
        )
    return matrix, chain


def _gazebo_sensor_transform(
    root: ET.Element,
    xacro_root: ET.Element,
    base_frame: str,
    sensor_frame: str,
    sensor_name: str,
) -> tuple[np.ndarray, dict[str, Any]]:
    matches: list[tuple[ET.Element, ET.Element]] = []
    for gazebo in _elements(root, "gazebo"):
        for sensor in _direct_children(gazebo, "sensor"):
            if sensor.get("name") == sensor_name:
                matches.append((gazebo, sensor))
    if len(matches) != 1:
        raise AuditError(
            f"expected one Gazebo sensor named {sensor_name!r}, found {len(matches)}"
        )

    gazebo, sensor = matches[0]
    reference = gazebo.get("reference")
    if reference is None:
        raise AuditError(f"Gazebo sensor {sensor_name!r} must be inside a referenced link")
    reference_transform, reference_chain = _joint_chain(
        xacro_root, base_frame, reference, "robot xacro"
    )

    poses = _direct_children(sensor, "pose")
    if len(poses) != 1:
        raise AuditError(f"Gazebo sensor {sensor_name!r} needs one explicit numeric pose")
    pose_values = _numbers(poses[0].text, 6, f"Gazebo sensor {sensor_name!r} pose")
    pose_xyz, pose_rpy = pose_values.reshape(2, 3)
    if poses[0].attrib:
        raise AuditError(
            f"Gazebo sensor {sensor_name!r} pose has unsupported attributes: {poses[0].attrib}"
        )

    frames = [
        (plugin, frame)
        for plugin in _direct_children(sensor, "plugin")
        for frame in _elements(plugin, "frame")
    ]
    if len(frames) != 1:
        raise AuditError(
            f"Gazebo sensor {sensor_name!r} needs exactly one plugin frame, found {len(frames)}"
        )
    frame_text = (frames[0][1].text or "").strip()
    if frame_text != sensor_frame:
        raise AuditError(
            f"Gazebo sensor {sensor_name!r} plugin frame is {frame_text!r}; "
            f"expected {sensor_frame!r}"
        )
    world_point_settings = _direct_children(frames[0][0], "publish_world_points")
    if len(world_point_settings) != 1:
        raise AuditError(
            f"Gazebo sensor {sensor_name!r} plugin needs one explicit publish_world_points setting"
        )
    publish_world_points = (world_point_settings[0].text or "").strip().lower()
    if publish_world_points != "false":
        raise AuditError(
            f"Gazebo sensor {sensor_name!r} must publish points in its local frame; "
            f"publish_world_points is {publish_world_points!r}"
        )

    sensor_pose = _transform_from_xyz_rpy(pose_xyz, pose_rpy)
    matrix = reference_transform @ sensor_pose
    return matrix, {
        "sensor_name": sensor_name,
        "reference_frame": reference,
        "plugin_frame": frame_text,
        "publish_world_points": False,
        "reference_joint_chain": reference_chain,
        "pose_relative_to_reference": {
            "translation_m": pose_xyz.tolist(),
            "rpy_rad": pose_rpy.tolist(),
        },
    }


def _lio_transform(config: dict[str, Any], base_frame: str) -> tuple[np.ndarray, dict[str, Any]]:
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

    t_imu_lidar = lidar_values[:3]
    r_imu_lidar = lidar_values[3:].reshape(3, 3)
    _rotation_is_valid(r_imu_lidar, "lio.extrinsic.lidar_imu rotation")

    t_base_imu = odom_values[:3]
    rpy_rad = np.radians(odom_values[3:])
    r_stored = _rotation_xyz(rpy_rad).T
    _rotation_is_valid(r_stored, "lio.extrinsic.odom_robo stored rotation")

    # Existing LIO source stores IMU-in-base rotation transposed from its XYZ RPY.
    r_base_imu = r_stored.T
    r_base_lidar = r_base_imu @ r_imu_lidar
    t_base_lidar = t_base_imu + r_base_imu @ t_imu_lidar
    configured_base = config.get("lio.frames.base")
    return _transform(t_base_lidar, r_base_lidar), {
        "configured_base_frame": configured_base,
        "base_frame_matches_requested": configured_base == base_frame,
        "odom_robo": {
            "translation_m": t_base_imu.tolist(),
            "rpy_values_deg": odom_values[3:].tolist(),
            "stored_rotation_convention": "(Rz(yaw) @ Ry(pitch) @ Rx(roll)).T",
            "stored_rotation": r_stored.tolist(),
            "base_from_imu_rotation": r_base_imu.tolist(),
        },
        "lidar_imu": {
            "translation_m": t_imu_lidar.tolist(),
            "rotation_row_major": r_imu_lidar.tolist(),
        },
    }


def audit_robot_mount(
    robot_xacro: Path,
    robot_gazebo: Path,
    lio_config: Path,
    *,
    base_frame: str = "base_footprint",
    sensor_frame: str = "mid360_rgl_frame",
    sensor_name: str = "mid360_rgl",
) -> dict[str, Any]:
    for path in (robot_xacro, robot_gazebo, lio_config):
        if not path.is_file():
            raise AuditError(f"input file does not exist: {path}")

    xacro_root = _read_xml(robot_xacro)
    gazebo_root = _read_xml(robot_gazebo)
    config = _read_yaml(lio_config)
    expected, joint_chain = _joint_chain(xacro_root, base_frame, sensor_frame, "robot xacro")
    actual, gazebo_details = _gazebo_sensor_transform(
        gazebo_root, xacro_root, base_frame, sensor_frame, sensor_name
    )
    effective, lio_details = _lio_transform(config, base_frame)

    t_imu_lidar = np.asarray(lio_details["lidar_imu"]["translation_m"], dtype=np.float64)
    r_imu_lidar = np.asarray(lio_details["lidar_imu"]["rotation_row_major"], dtype=np.float64)
    required_r_base_imu = expected[:3, :3] @ r_imu_lidar.T
    required_t_base_imu = expected[:3, 3] - required_r_base_imu @ t_imu_lidar
    urdf_gazebo_comparison = _compare_transforms(expected, actual)
    lio_comparison = _compare_transforms(expected, effective)
    frames_valid = bool(lio_details["base_frame_matches_requested"])
    valid = bool(
        urdf_gazebo_comparison["within_tolerance"]
        and lio_comparison["within_tolerance"]
        and frames_valid
    )

    return {
        "schema_version": 1,
        "audit_kind": "offline simulator LiDAR mount and LIO extrinsic consistency audit",
        "verdict": "PASS_MOUNT_MATCH" if valid else "FAIL_MOUNT_MISMATCH",
        "valid": valid,
        "frames": {
            "base": base_frame,
            "sensor": sensor_frame,
            "sensor_name": sensor_name,
            "lio_configured_base": lio_details["configured_base_frame"],
        },
        "inputs": {
            "robot_xacro": str(robot_xacro.resolve()),
            "robot_gazebo": str(robot_gazebo.resolve()),
            "lio_config": str(lio_config.resolve()),
            "sha256": {
                "robot_xacro": _sha256(robot_xacro),
                "robot_gazebo": _sha256(robot_gazebo),
                "lio_config": _sha256(lio_config),
            },
        },
        "joint_chain": joint_chain,
        "gazebo_sensor": gazebo_details,
        "expected_base_to_sensor": _transform_report(expected),
        "actual_gazebo_base_to_sensor": _transform_report(actual),
        "effective_lio_base_to_sensor": _transform_report(effective),
        "comparisons": {
            "joint_chain_vs_gazebo_sensor": urdf_gazebo_comparison,
            "joint_chain_vs_effective_lio": lio_comparison,
            "base_frame_matches_requested": frames_valid,
            "translation_tolerance_m": TRANSLATION_TOLERANCE_M,
            "rotation_tolerance_deg": ROTATION_TOLERANCE_DEG,
        },
        "required_lio_extrinsics_for_joint_chain": {
            "odom_robo_translation_m": required_t_base_imu.tolist(),
            "base_from_imu_rotation_row_major": required_r_base_imu.tolist(),
            "lidar_imu_calibration_modified": False,
        },
        "scope": {
            "xacro_expanded": False,
            "ros_or_gazebo_started": False,
            "physical_packets_used": False,
            "physical_mount_verified": False,
            "calibration_claimed": False,
            "note": (
                "Checks explicit numeric source transforms and existing LIO composition. "
                "It cannot establish the physical robot mount or validate sensor packets."
            ),
        },
    }


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Compare the explicit simulator LiDAR mount with effective LIO YAML geometry."
    )
    parser.add_argument("--robot-xacro", required=True, type=Path, help="robot description xacro")
    parser.add_argument("--robot-gazebo", required=True, type=Path, help="robot Gazebo xacro")
    parser.add_argument("--lio-config", required=True, type=Path, help="active LIO YAML config")
    parser.add_argument("--output", required=True, type=Path, help="JSON report path")
    parser.add_argument(
        "--base-frame", default="base_footprint", help="base frame (default: base_footprint)"
    )
    parser.add_argument(
        "--sensor-frame",
        default="mid360_rgl_frame",
        help="LiDAR frame (default: mid360_rgl_frame)",
    )
    parser.add_argument(
        "--sensor-name", default="mid360_rgl", help="Gazebo sensor name (default: mid360_rgl)"
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        report = audit_robot_mount(
            args.robot_xacro,
            args.robot_gazebo,
            args.lio_config,
            base_frame=args.base_frame,
            sensor_frame=args.sensor_frame,
            sensor_name=args.sensor_name,
        )
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    except (AuditError, OSError) as exc:
        print(f"robot mount audit: error: {exc}", file=sys.stderr)
        return 2

    print(f"mount verdict: {report['verdict']}")
    print(
        "translation errors (joint/Gazebo, joint/LIO): "
        f"{report['comparisons']['joint_chain_vs_gazebo_sensor']['translation_error_m']:.9g} m, "
        f"{report['comparisons']['joint_chain_vs_effective_lio']['translation_error_m']:.9g} m"
    )
    print(f"report: {args.output}")
    return 0 if report["valid"] else 1


if __name__ == "__main__":
    sys.exit(main())

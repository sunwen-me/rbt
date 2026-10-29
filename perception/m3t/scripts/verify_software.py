#!/usr/bin/env python3
"""Run the native SOFTWARE_FIXTURE RGB-D to M3T to tracking-output path."""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import time


TIMEOUT_SECONDS = 90.0

REGION_METADATA = """%YAML:1.2
n_lines_max: 64
use_adaptive_coverage: 0
reference_contour_length: 0.0
min_continuous_distance: 3.0
function_length: 8
distribution_length: 12
function_amplitude: 0.43
function_slope: 0.5
learning_rate: 1.3
n_global_iterations: 1
scales: [6, 4, 2, 1]
standard_deviations: [15.0, 5.0, 3.5, 1.5]
n_histogram_bins: 16
learning_rate_f: 0.2
learning_rate_b: 0.2
unconsidered_line_length: 0.5
max_considered_line_length: 20.0
measured_depth_offset_radius: 0.01
measured_occlusion_radius: 0.01
measured_occlusion_threshold: 0.03
modeled_depth_offset_radius: 0.01
modeled_occlusion_radius: 0.01
modeled_occlusion_threshold: 0.03
n_unoccluded_iterations: 10
min_n_unoccluded_lines: 0
visualize_pose_result: 0
visualize_gradient_optimization: 0
visualize_hessian_optimization: 0
visualize_lines_correspondence: 0
visualize_points_correspondence: 0
visualize_points_depth_image_correspondence: 0
visualize_points_depth_rendering_correspondence: 0
visualize_points_silhouette_rendering_correspondence: 0
visualize_points_optimization: 0
visualize_points_histogram_image_optimization: 0
visualize_points_result: 0
visualize_points_histogram_image_result: 0
visualization_min_depth: 0.0
visualization_max_depth: 1.0
display_visualization: 0
save_visualizations: 0
save_directory: "./"
save_image_type: "png"
"""

DEPTH_METADATA = """%YAML:1.2
n_points_max: 64
use_adaptive_coverage: 0
use_depth_scaling: 1
reference_surface_area: 0.0
stride_length: 0.005
considered_distances: [0.05, 0.02, 0.01]
standard_deviations: [0.05, 0.03, 0.02]
measure_occlusions: 1
measured_depth_offset_radius: 0.01
measured_occlusion_radius: 0.01
measured_occlusion_threshold: 0.03
modeled_depth_offset_radius: 0.01
modeled_occlusion_radius: 0.01
modeled_occlusion_threshold: 0.03
n_unoccluded_iterations: 10
min_n_unoccluded_points: 0
visualize_pose_result: 0
visualize_gradient_optimization: 0
visualize_hessian_optimization: 0
visualize_correspondences_correspondence: 0
visualize_points_correspondence: 0
visualize_points_depth_rendering_correspondence: 0
visualize_points_silhouette_rendering_correspondence: 0
visualize_points_optimization: 0
visualize_points_result: 0
visualization_min_depth: 0.0
visualization_max_depth: 1.0
display_visualization: 0
save_visualizations: 0
save_directory: "./"
save_image_type: "png"
"""

TEXTURE_METADATA = """%YAML:1.2
descriptor_type: 4
focused_image_size: 200
descriptor_distance_threshold: 0.7
tukey_norm_constant: 20.0
standard_deviations: [10.0, 5.0]
max_keyframe_rotation_difference: 0.34906585039885557
max_keyframe_age: 1000
n_keyframes: 1
orb_n_features: 300
orb_scale_factor: 1.2
orb_n_levels: 3
brisk_threshold: 25
brisk_octave: 3
brisk_pattern_scale: 0.6
daisy_radius: 7.0
daisy_q_radius: 3
daisy_q_theta: 4
daisy_q_hist: 8
freak_orientation_normalized: 1
freak_scale_normalized: 1
freak_pattern_scale: 18.0
freak_n_octaves: 4
sift_n_features: 0
sift_n_octave_layers: 5
sift_contrast_threshold: 0.04
sift_edge_threshold: 10.0
sift_sigma: 0.7
measured_occlusion_radius: 0.01
measured_occlusion_threshold: 0.03
modeled_occlusion_radius: 0.01
modeled_occlusion_threshold: 0.03
visualize_pose_result: 0
visualize_gradient_optimization: 0
visualize_hessian_optimization: 0
visualize_correspondences_correspondence: 0
visualize_correspondences_optimization: 0
visualize_points_result: 0
visualize_points_depth_image_result: 0
visualize_points_silhouette_rendering_result: 0
visualize_points_depth_rendering_result: 0
visualization_min_depth: 0.0
visualization_max_depth: 1.0
display_visualization: 0
save_visualizations: 0
save_directory: "./"
save_image_type: "png"
"""

OPTIMIZER_METADATA = """%YAML:1.2
tikhonov_parameter_rotation: 5000.0
tikhonov_parameter_translation: 500000.0
"""

REFINER_METADATA = """%YAML:1.2
n_corr_iterations: 2
n_update_iterations: 2
visualization_time: 1
"""

CONSTRAINT_METADATA_TEMPLATE = """%YAML:1.2
constraint_directions: [{constraint_directions}]
body12joint1_pose: !!opencv-matrix
  rows: 4
  cols: 4
  dt: d
  data: [1, 0, 0, -0.045,
         0, {cosine:.12g}, {sine:.12g}, 0,
         0, {negative_sine:.12g}, {cosine:.12g}, 0,
         0, 0, 0, 1]
body22joint2_pose: !!opencv-matrix
  rows: 4
  cols: 4
  dt: d
  data: [1, 0, 0, 0,
         0, 1, 0, 0,
         0, 0, 1, 0,
         0, 0, 0, 1]
max_distance_rotation: 0.5
max_distance_translation: 0.05
standard_deviation_rotation: 10.0
standard_deviation_translation: 10.0
"""


def box_obj(path: Path, bounds: tuple[float, float, float, float, float, float]) -> None:
    x0, x1, y0, y1, z0, z1 = bounds
    vertices = [
        (x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0),
        (x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1),
    ]
    triangles = [
        (1, 3, 2), (1, 4, 3), (5, 6, 7), (5, 7, 8),
        (1, 2, 6), (1, 6, 5), (2, 3, 7), (2, 7, 6),
        (3, 4, 8), (3, 8, 7), (4, 1, 5), (4, 5, 8),
    ]
    with path.open("w", encoding="utf-8") as output:
        output.write("# SOFTWARE_FIXTURE asymmetric visual mesh\n")
        for x, y, z in vertices:
            output.write(f"v {x:.8f} {y:.8f} {z:.8f}\n")
        for a, b, c in triangles:
            output.write(f"f {a} {b} {c}\n")


def fixture_config(directory: Path, profile: str = "baseline",
                   texture_backend: str = "cpu") -> Path:
    if profile not in {"baseline", "extended", "hard-constraint"}:
        raise ValueError(f"unsupported verification profile: {profile}")
    if texture_backend not in {"cpu", "dsp"}:
        raise ValueError(f"unsupported texture backend: {texture_backend}")
    if texture_backend == "dsp" and profile == "baseline":
        raise ValueError("DSP texture verification requires the extended or hard-constraint profile")
    directory.mkdir(parents=True)
    box_obj(directory / "base.obj", (-0.032, 0.032, -0.035, 0.035, -0.032, 0.040))
    box_obj(directory / "forearm.obj", (0.0, 0.18, -0.021, 0.025, -0.020, 0.030))

    angle = 0.55
    cosine, sine = math.cos(angle), math.sin(angle)
    model = {
        "schema_version": 1,
        "name": "software_fixture_dual_arm",
        "root_link": "base",
        "units": "metres_radians",
        "links": [
            {"name": "base", "mesh": "base.obj", "visuals": 1},
            {"name": "forearm", "mesh": "forearm.obj", "visuals": 1},
        ],
        "joints": [{
            "name": "shoulder",
            "type": "revolute",
            "parent": "base",
            "child": "forearm",
            "origin": [1, 0, 0, 0.045, 0, cosine, -sine, 0,
                       0, sine, cosine, 0, 0, 0, 0, 1],
            "axis": [0, 0, 1],
            "lower": -0.8,
            "upper": 0.8,
        }],
    }
    left_pose = [1, 0, 0, -0.24, 0, 1, 0, 0, 0, 0, 1, 0.72, 0, 0, 0, 1]
    right_pose = [-1, 0, 0, 0.24, 0, -1, 0, 0, 0, 0, 1, 0.72, 0, 0, 0, 1]
    initialization = {
        "schema_version": 1,
        "coordinate_frame": "fixture/color_optical",
        "serial": "SOFTWARE_FIXTURE",
        "arms": [
            {"name": "so101_left", "base_in_camera": left_pose,
             "joint_angles": {"shoulder": 0.0}, "fixed_base": True},
            {"name": "so101_right", "base_in_camera": right_pose,
             "joint_angles": {"shoulder": 0.0}, "fixed_base": True},
        ],
        "provenance": {"mode": "SOFTWARE_FIXTURE", "hardware": "NOT RUN"},
    }
    config = {
        "schema_version": 1,
        "robot_model": "robot.json",
        "initialization": "initialization.json",
        "cache_directory": "cache",
        "model_divides": 0,
        "model_points": 64,
        "model_image_size": 256,
        "correspondence_iterations": 3,
        "update_iterations": 2,
    }
    if profile in {"extended", "hard-constraint"}:
        constraint_type = "soft" if profile == "extended" else "hard"
        constraint_directions = ("1, 1, 0, 1, 1, 1" if constraint_type == "soft"
                                 else "0, 0, 1, 0, 0, 0")
        metadata = {
            "region.yaml": REGION_METADATA,
            "depth.yaml": DEPTH_METADATA,
            "texture.yaml": (TEXTURE_METADATA if texture_backend == "cpu" else
                              TEXTURE_METADATA.replace("descriptor_type: 4",
                                                       "descriptor_type: 6", 1)),
            "optimizer.yaml": OPTIMIZER_METADATA,
            "refiner.yaml": REFINER_METADATA,
            "constraint_left.yaml": CONSTRAINT_METADATA_TEMPLATE.format(
                cosine=cosine, sine=sine, negative_sine=-sine,
                constraint_directions=constraint_directions),
            "constraint_right.yaml": CONSTRAINT_METADATA_TEMPLATE.format(
                cosine=cosine, sine=sine, negative_sine=-sine,
                constraint_directions=constraint_directions),
        }
        for name, contents in metadata.items():
            (directory / name).write_text(contents, encoding="utf-8")
        config.update({
            "modalities": {"region": True, "depth": True, "texture": True},
            "occlusion": {"measured": True, "modeled": True,
                          "region_checking": True, "image_size": 256},
            "metadata": {"region": "region.yaml", "depth": "depth.yaml",
                         "texture": "texture.yaml", "optimizer": "optimizer.yaml",
                         "refiner": "refiner.yaml"},
            "initialization_flow": {"static_detector": True, "refine": True},
            "constraints": [
                {"name": f"{constraint_type}_left_shoulder", "arm": "so101_left",
                 "link1": "base", "link2": "forearm", "type": constraint_type,
                 "metadata": "constraint_left.yaml"},
                {"name": f"{constraint_type}_right_shoulder", "arm": "so101_right",
                 "link1": "base", "link2": "forearm", "type": constraint_type,
                 "metadata": "constraint_right.yaml"},
            ],
        })
    for name, value in (("robot.json", model), ("initialization.json", initialization),
                        ("tracking.json", config)):
        (directory / name).write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    (directory / "cache").mkdir()
    return directory / "tracking.json"


def json_events(path: Path) -> list[dict]:
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except FileNotFoundError:
        return []
    records = []
    for line in lines:
        try:
            value = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(value, dict):
            records.append(value)
    return records


def verify_dsp_audit(path: Path) -> dict:
    events = json_events(path)
    ready = [event for event in events if event.get("event") == "m3t_dsp_ready"]
    extracts = [event for event in events if event.get("event") == "m3t_dsp_extract"]
    matches = [event for event in events if event.get("event") == "m3t_dsp_match"]
    if not ready:
        raise RuntimeError("DSP tracker audit did not report m3t_dsp_ready")
    if not extracts:
        raise RuntimeError("DSP tracker audit did not report extraction events")
    if not matches:
        raise RuntimeError("DSP tracker audit did not report matching events")

    def integer(event: dict, field: str, minimum: int = 0) -> int:
        value = event.get(field)
        if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
            raise RuntimeError(f"DSP audit {field} must be an integer >= {minimum}")
        return value

    for event in ready:
        if integer(event, "implversion", 1) != 1 \
                or integer(event, "featuresmask", 0) != 0x7f:
            raise RuntimeError("DSP ready event does not report implementation 1 and feature mask 0x7f")

    extract_features = []
    for event in extracts:
        integer(event, "width", 1)
        integer(event, "height", 1)
        feature_count = integer(event, "features", 0)
        if integer(event, "rpc_calls", 0) != 1:
            raise RuntimeError("DSP extract event did not use exactly one RPC call")
        extract_features.append(feature_count)
    if not any(count > 0 for count in extract_features):
        raise RuntimeError("DSP tracker audit contains no nonzero extraction feature data")

    match_queries = []
    match_train = []
    match_neighbors = []
    for event in matches:
        queries = integer(event, "queries", 1)
        train = integer(event, "train", 2)
        neighbors = integer(event, "neighbors", 1)
        if integer(event, "rpc_calls", 0) != 1:
            raise RuntimeError("DSP match event did not use exactly one RPC call")
        if neighbors > queries * 2:
            raise RuntimeError("DSP match event reports more than two neighbors per query")
        match_queries.append(queries)
        match_train.append(train)
        match_neighbors.append(neighbors)

    return {
        "ready_events": len(ready),
        "feature_mask": 0x7f,
        "implementation_version": 1,
        "extract_events": len(extracts),
        "extract_rpc_calls": len(extracts),
        "extract_features_total": sum(extract_features),
        "extract_features_max": max(extract_features),
        "match_events": len(matches),
        "match_rpc_calls": len(matches),
        "match_queries_total": sum(match_queries),
        "match_train_total": sum(match_train),
        "match_neighbors_total": sum(match_neighbors),
    }


def verify_tracking_samples(events: list[dict], summary: dict,
                            profile: str = "baseline") -> dict:
    expected_algorithm = ("m3t_region_depth_articulated" if profile == "baseline"
                          else "m3t_articulated")
    expected_modalities = ["region", "depth"]
    if profile in {"extended", "hard-constraint"}:
        expected_modalities.append("texture")
    elif profile != "baseline":
        raise ValueError(f"unsupported verification profile: {profile}")
    if summary.get("event") != "tracking_samples_consumed" \
            or summary.get("mode") != "SOFTWARE_FIXTURE" \
            or summary.get("hardware") != "NOT RUN" \
            or summary.get("provider") != "m3t-so101" or summary.get("count") != 20:
        raise RuntimeError("native consumer summary did not report 20 SOFTWARE_FIXTURE samples")
    samples = [event for event in events if event.get("event") == "tracking_sample"]
    if len(samples) != 20:
        raise RuntimeError(f"expected 20 raw native samples, received {len(samples)}")

    expected_links = {
        ("so101_left", "base"), ("so101_left", "forearm"),
        ("so101_right", "base"), ("so101_right", "forearm"),
    }
    arms = ("so101_left", "so101_right")
    errors = {arm: [] for arm in arms}
    zero_seed_errors = {arm: [] for arm in arms}
    observed_angles = {arm: [] for arm in arms}
    reference_angles = {arm: [] for arm in arms}
    previous_sequence = None
    previous_capture_ns = None
    clock_source = "534f4654574152455f46495854555245" + "0" * 64
    frame_period_ns = 1_000_000_000 // 30

    def finite_number(value, field: str) -> float:
        if isinstance(value, bool) or not isinstance(value, (int, float)) \
                or not math.isfinite(value):
            raise RuntimeError(f"{field} must be finite numeric data")
        return float(value)

    def integer(value, field: str, minimum: int = 0) -> int:
        if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
            raise RuntimeError(f"{field} must be an integer >= {minimum}")
        return value

    for sample in samples:
        if sample.get("mode") != "SOFTWARE_FIXTURE" or sample.get("hardware") != "NOT RUN" \
                or sample.get("provider") != "m3t-so101":
            raise RuntimeError("native sample is not explicitly labeled SOFTWARE_FIXTURE")
        sequence = integer(sample.get("sequence"), "native sequence")
        capture_ns = integer(sample.get("capture_ns"), "native capture_ns", 1)
        integer(sample.get("received_ns"), "native received_ns", 1)
        if sequence <= (previous_sequence if previous_sequence is not None else -1):
            raise RuntimeError("native sample sequence did not strictly increase")
        if capture_ns <= (previous_capture_ns if previous_capture_ns is not None else 0):
            raise RuntimeError("native capture timestamp did not strictly increase")
        if sample.get("capture_clock_kind") != "Device" \
                or sample.get("received_clock_kind") != "HostMonotonic" \
                or sample.get("capture_clock_source") != clock_source \
                or integer(sample.get("capture_clock_generation"),
                           "native capture_clock_generation") != 0 \
                or sample.get("capture_uncertainty_ns") is not None \
                or sample.get("received_uncertainty_ns") is not None:
            raise RuntimeError("native clock identity or unknown-uncertainty metadata is invalid")

        estimate = sample.get("estimate")
        if not isinstance(estimate, dict) \
                or integer(estimate.get("schema_version"), "schema_version", 1) != 1 \
                or estimate.get("event") != "estimate" \
                or estimate.get("algorithm") != expected_algorithm:
            raise RuntimeError("tracking payload is not an M3T estimate JSON object")
        if estimate.get("modalities") != expected_modalities:
            raise RuntimeError(f"tracking estimate modalities do not match {profile} profile")
        if estimate.get("frame_id") != "fixture/color_optical" \
                or integer(estimate.get("frame_sequence"), "estimate frame_sequence") != sequence \
                or integer(estimate.get("capture_ns"), "estimate capture_ns", 1) != capture_ns:
            raise RuntimeError("tracking estimate does not match its native frame identity")
        color_capture_ns = integer(estimate.get("color_capture_ns"), "color_capture_ns", 1)
        pair_skew_ns = integer(estimate.get("pair_skew_ns"), "pair_skew_ns")
        if pair_skew_ns != abs(capture_ns - color_capture_ns) \
                or estimate.get("capture_clock_source") != clock_source \
                or integer(estimate.get("capture_clock_generation"),
                           "estimate capture_clock_generation") != 0 \
                or estimate.get("uncertainty_ns") is not None:
            raise RuntimeError("tracking estimate capture pair or clock metadata is inconsistent")

        links = estimate.get("links")
        if not isinstance(links, list) or len(links) != len(expected_links):
            raise RuntimeError("tracking estimate must contain all four arm-link poses")
        observed_links = set()
        for link in links:
            if not isinstance(link, dict):
                raise RuntimeError("tracking link entry must be an object")
            arm = link.get("arm")
            name = link.get("link")
            frame = link.get("frame_id")
            if (arm, name) not in expected_links or frame != f"{arm}/{name}" \
                    or (arm, name) in observed_links:
                raise RuntimeError("tracking estimate contains an unexpected or duplicate link frame")
            observed_links.add((arm, name))
            pose = link.get("pose")
            if not isinstance(pose, list) or len(pose) != 16:
                raise RuntimeError(f"{frame} pose must contain 16 row-major numbers")
            pose = [finite_number(value, f"{frame} pose") for value in pose]
            tolerance = 5.0e-3
            if any(abs(pose[index]) > tolerance for index in (12, 13, 14)) \
                    or abs(pose[15] - 1.0) > tolerance:
                raise RuntimeError(f"{frame} pose has an invalid homogeneous bottom row")
            orthogonality_error = max(
                abs(sum(pose[axis * 4 + row] * pose[axis * 4 + column] for axis in range(3))
                    - (1.0 if row == column else 0.0))
                for row in range(3) for column in range(3))
            determinant = (
                pose[0] * (pose[5] * pose[10] - pose[6] * pose[9])
                - pose[1] * (pose[4] * pose[10] - pose[6] * pose[8])
                + pose[2] * (pose[4] * pose[9] - pose[5] * pose[8]))
            if orthogonality_error > tolerance or abs(determinant - 1.0) > tolerance:
                raise RuntimeError(f"{frame} pose rotation is not a proper rigid transform")
            if any(abs(pose[index]) > 1.5 for index in (3, 7, 11)):
                raise RuntimeError(f"{frame} translation exceeds metre-scale fixture bounds")
        if observed_links != expected_links:
            raise RuntimeError("tracking estimate omitted an articulated link")

        joint_angles = estimate.get("joint_angles_rad")
        if not isinstance(joint_angles, dict) or set(joint_angles) != set(arms):
            raise RuntimeError("tracking estimate must report both arm joint sets")
        angles = {}
        for arm in arms:
            joints = joint_angles[arm]
            if not isinstance(joints, dict) or set(joints) != {"shoulder"}:
                raise RuntimeError(f"{arm} must report exactly its shoulder angle")
            angle = finite_number(joints["shoulder"], f"{arm} shoulder angle")
            if not -0.8 <= angle <= 0.8:
                raise RuntimeError(f"{arm} shoulder angle is outside the model limits")
            angles[arm] = angle

        if capture_ns % frame_period_ns != 0:
            raise RuntimeError("capture timestamp does not map to a rendered fixture frame")
        frame_index = capture_ns // frame_period_ns
        phase = ((frame_index - 1) % 100) / 100.0
        triangle = -1.0 + 4.0 * phase if phase < 0.5 else 3.0 - 4.0 * phase
        left_truth = 0.15 + 0.22 * triangle
        truth = {"so101_left": left_truth, "so101_right": -left_truth}
        for arm in arms:
            errors[arm].append((angles[arm] - truth[arm]) ** 2)
            zero_seed_errors[arm].append(truth[arm] ** 2)
            observed_angles[arm].append(angles[arm])
            reference_angles[arm].append(truth[arm])
        previous_sequence = sequence
        previous_capture_ns = capture_ns

    rmse = {arm: math.sqrt(math.fsum(errors[arm]) / len(samples)) for arm in arms}
    zero_seed_rmse = {
        arm: math.sqrt(math.fsum(zero_seed_errors[arm]) / len(samples)) for arm in arms}
    motion = {arm: max(observed_angles[arm]) - min(observed_angles[arm]) for arm in arms}
    reference_motion = {
        arm: max(reference_angles[arm]) - min(reference_angles[arm]) for arm in arms}
    if profile == "hard-constraint":
        for arm in arms:
            if max(abs(angle) for angle in observed_angles[arm]) > 1.0e-3:
                raise RuntimeError(f"{arm} hard-constrained joint estimate moved from zero")
            if reference_motion[arm] < 0.015:
                raise RuntimeError(f"{arm} fixture input did not exercise the moving joint")
    else:
        for arm in arms:
            if zero_seed_rmse[arm] < 0.02 or rmse[arm] > 0.8 * zero_seed_rmse[arm]:
                raise RuntimeError(f"{arm} RMSE {rmse[arm]:.6f} did not beat zero-seed baseline "
                                   f"{zero_seed_rmse[arm]:.6f}")
            if motion[arm] < 0.015:
                raise RuntimeError(f"{arm} joint estimate did not show articulated motion")

    return {
        "mode": "SOFTWARE_FIXTURE",
        "hardware": "NOT RUN",
        "algorithm": expected_algorithm,
        "modalities": expected_modalities,
        "provider": "m3t-so101",
        "count": len(samples),
        "first_sequence": samples[0]["sequence"],
        "last_sequence": samples[-1]["sequence"],
        "first_joint_angles_rad": {
            arm: samples[0]["estimate"]["joint_angles_rad"][arm]["shoulder"] for arm in arms},
        "last_joint_angles_rad": {
            arm: samples[-1]["estimate"]["joint_angles_rad"][arm]["shoulder"] for arm in arms},
        "joint_rmse_rad": rmse,
        "zero_seed_rmse_rad": zero_seed_rmse,
        "joint_motion_rad": motion,
        "reference_joint_motion_rad": reference_motion,
    }


def start(name: str, command: list[str], env: dict[str, str], output: Path,
          processes: list[subprocess.Popen], handles: list) -> subprocess.Popen:
    handle = (output / f"{name}.log").open("w", encoding="utf-8")
    handles.append(handle)
    process = subprocess.Popen(command, env=env, stdout=handle, stderr=subprocess.STDOUT,
                               start_new_session=True, text=True)
    processes.append(process)
    return process


def wait_until(description: str, deadline: float, predicate, processes: list[subprocess.Popen]) -> None:
    while time.monotonic() < deadline:
        if predicate():
            return
        for process in processes:
            if process.poll() is not None:
                raise RuntimeError(f"child process {process.args[0]} exited {process.returncode} while {description}")
        time.sleep(0.05)
    raise TimeoutError(f"timed out while {description}")


def renderer_audit(path: Path) -> dict | None:
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except FileNotFoundError:
        return None
    records = []
    for line in lines:
        if "RBT_GL_AUDIT" not in line:
            continue
        if "error=" in line:
            raise RuntimeError(f"OpenGL renderer audit failed in {path.name}: {line.strip()}")
        match = re.search(r"RBT_GL_AUDIT pid=(\d+) renderer=(.*?) version=(.*)$", line)
        if match is None:
            raise RuntimeError(f"malformed OpenGL renderer audit line in {path.name}")
        renderer, version = match.group(2).strip(), match.group(3).strip()
        if not renderer or not version:
            raise RuntimeError(f"incomplete OpenGL renderer audit line in {path.name}")
        records.append({"renderer": renderer, "version": version})
    if len(records) > 1:
        raise RuntimeError(f"OpenGL renderer audit logged more than once in {path.name}")
    return records[0] if records else None


def is_software_renderer(renderer: str) -> bool:
    renderer = renderer.casefold()
    return any(marker in renderer for marker in (
        "llvmpipe", "softpipe", "swrast", "software", "swiftshader"))


def verify_renderers(rendering: str, audit_library: Path,
                     tracker_log: Path, fixture_log: Path) -> dict | None:
    if not audit_library.is_file():
        if rendering == "hardware":
            raise RuntimeError(
                f"hardware rendering requires the OpenGL audit library: {audit_library}")
        return None
    actual = {
        "tracker": renderer_audit(tracker_log),
        "fixture": renderer_audit(fixture_log),
    }
    missing = [name for name, record in actual.items() if record is None]
    if missing:
        raise RuntimeError("OpenGL renderer audit did not report a context for "
                           + " and ".join(missing))
    if rendering == "hardware":
        software = [name for name, record in actual.items()
                    if is_software_renderer(record["renderer"])]
        if software:
            raise RuntimeError("hardware rendering used a software OpenGL renderer for "
                               + " and ".join(software))
    return actual


def runtime_environment(rendering: str) -> dict[str, str]:
    env = os.environ.copy()
    repo_root = Path(__file__).resolve().parents[3]
    dependency_root = repo_root / "target/m3t-deps-prefix/usr/lib/x86_64-linux-gnu"
    ros_root = repo_root / "target/orbbec-perf-20261020/ros2-deps/usr"
    runtime_dirs = [
        dependency_root,
        ros_root / "lib/x86_64-linux-gnu",
        ros_root / "lib/x86_64-linux-gnu/openblas-pthread",
        ros_root / "lib",
    ]
    existing = [str(path) for path in runtime_dirs if path.is_dir()]
    inherited = env.get("LD_LIBRARY_PATH")
    if inherited:
        existing.append(inherited)
    if existing:
        env["LD_LIBRARY_PATH"] = os.pathsep.join(existing)

    plugin_root = dependency_root / "qt5/plugins"
    platform_plugins = plugin_root / "platforms"
    plugin_paths = [str(plugin_root)]
    inherited_plugins = env.get("QT_PLUGIN_PATH")
    if inherited_plugins:
        plugin_paths.append(inherited_plugins)
    env["QT_PLUGIN_PATH"] = os.pathsep.join(plugin_paths)
    old_platform_path = env.get("QT_QPA_PLATFORM_PLUGIN_PATH")
    env["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(platform_plugins) + (
        os.pathsep + old_platform_path if old_platform_path else "")
    env["PYTHONUNBUFFERED"] = "1"
    if rendering == "software":
        env["LIBGL_ALWAYS_SOFTWARE"] = "1"
        env["GALLIUM_DRIVER"] = "llvmpipe"
        env.pop("MESA_LOADER_DRIVER_OVERRIDE", None)
    else:
        env.pop("LIBGL_ALWAYS_SOFTWARE", None)
        env.pop("GALLIUM_DRIVER", None)
    return env


def audited_environment(env: dict[str, str], audit_library: Path) -> dict[str, str]:
    result = env.copy()
    inherited = result.get("LD_PRELOAD")
    result["LD_PRELOAD"] = str(audit_library) + (
        os.pathsep + inherited if inherited else "")
    return result


def stop_children(processes: list[subprocess.Popen], forced_kill: list[str]) -> None:
    for process in reversed(processes):
        if process.poll() is None:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
    deadline = time.monotonic() + 8.0
    for process in reversed(processes):
        if process.poll() is None:
            try:
                process.wait(timeout=max(0.0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                forced_kill.append(str(process.args[0]))
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                process.wait(timeout=1.0)


def main() -> int:
    started_at = time.monotonic()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--composer", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True,
                        help="new directory for generated fixture inputs, logs, and result.json")
    parser.add_argument("--rendering", choices=("software", "hardware"), default="software")
    parser.add_argument("--profile", choices=("baseline", "extended", "hard-constraint"),
                        default="baseline")
    parser.add_argument("--texture-backend", choices=("cpu", "dsp"), default="cpu")
    parser.add_argument("--exercise-reinitialization", action="store_true")
    args = parser.parse_args()
    if args.texture_backend == "dsp" and args.profile == "baseline":
        parser.error("--texture-backend dsp requires --profile extended or hard-constraint")
    build = args.build_dir.resolve()
    composer = args.composer.resolve()
    output = args.output_dir.resolve()
    if not build.is_dir() or not composer.is_file():
        parser.error("--build-dir and --composer must identify existing paths")
    fixture_binary = build / "rbt_m3t_rgbd_fixture"
    tracker_binary = build / "rbt-m3t"
    consumer_binary = build / "rbt_m3t_tracking_output_consumer_test"
    audit_library = build / "librbt_m3t_renderer_audit.so"
    missing = [str(path) for path in (fixture_binary, tracker_binary, consumer_binary)
               if not path.is_file()]
    if missing:
        parser.error("missing build outputs: " + ", ".join(missing))
    if args.rendering == "hardware" and not audit_library.is_file():
        parser.error("--rendering hardware requires the renderer audit library: "
                     + str(audit_library))
    if args.texture_backend == "dsp":
        for variable in ("RBT_M3T_DSP_STUB", "RBT_M3T_DSP_MODULE"):
            requested = os.environ.get(variable)
            if not requested:
                parser.error(f"--texture-backend dsp requires {variable} to name the device library")
            library = Path(requested)
            if not library.is_absolute() or not library.is_file() or not os.access(library, os.R_OK):
                parser.error(f"{variable} must be an absolute readable file: {requested}")
    output.mkdir(parents=True, exist_ok=False)

    result = {"status": "FAIL", "mode": "SOFTWARE_FIXTURE", "hardware": "NOT RUN",
              "rendering_mode": args.rendering, "profile": args.profile,
              "texture_backend": args.texture_backend, "dsp_device": "NOT RUN",
              "actual_renderer": None, "checks": [], "children_reaped": False,
              "forced_kill": []}
    deadline = started_at + TIMEOUT_SECONDS
    processes: list[subprocess.Popen] = []
    handles = []
    env = runtime_environment(args.rendering)
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    env["RBT_COMPOSER_ADDR"] = f"127.0.0.1:{port}"
    fixture_env = audited_environment(env, audit_library) if audit_library.is_file() else env
    tracker_env = audited_environment(env, audit_library) if audit_library.is_file() else env
    if args.texture_backend == "dsp":
        tracker_env["RBT_M3T_DSP_AUDIT"] = "1"

    try:
        config = fixture_config(output / "fixture", args.profile,
                                args.texture_backend)
        result["checks"].append({"name": "generated_two_arm_fixture", "status": "PASS"})
        start("composer", [str(composer)], env, output, processes, handles)

        def composer_ready() -> bool:
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                    return True
            except OSError:
                return False

        wait_until("waiting for Composer", min(deadline, time.monotonic() + 10.0),
                   composer_ready, processes)
        result["checks"].append({"name": "composer_started", "status": "PASS"})

        start("rgbd_fixture", [str(fixture_binary), "--config", str(config)],
              fixture_env, output, processes, handles)
        fixture_log = output / "rgbd_fixture.log"
        wait_until("waiting for the rendered RGB-D fixture", min(deadline, time.monotonic() + 20.0),
                   lambda: any(event.get("event") == "fixture_ready"
                               and event.get("mode") == "SOFTWARE_FIXTURE"
                               for event in json_events(fixture_log)), processes)
        result["checks"].append({"name": "native_fixture_provider_ready", "status": "PASS"})

        consumer_process = start("tracking_output_consumer",
            [str(consumer_binary), "--count", "20", "--timeout-sec", "80",
             "--provider", "m3t-so101"], env, output, processes, handles)
        time.sleep(0.2)
        if consumer_process.poll() is not None:
            raise RuntimeError("tracking output consumer exited before tracker startup")
        result["checks"].append({"name": "consumer_started_before_tracker", "status": "PASS"})

        tracker_command = [str(tracker_binary), "run", "--config", str(config),
                           "--provider", "orbbec335", "--count", "120",
                           "--duration-sec", "85"]
        if args.exercise_reinitialization:
            tracker_command.append("--reload-on-sighup")
        tracker_process = start("tracker", tracker_command, tracker_env,
                                output, processes, handles)
        tracker_log = output / "tracker.log"
        consumer_log = output / "tracking_output_consumer.log"
        if args.exercise_reinitialization:
            wait_until("waiting for five native tracking samples", deadline,
                       lambda: sum(event.get("event") == "tracking_sample"
                                   for event in json_events(consumer_log)) >= 5,
                       [tracker_process, consumer_process])
            try:
                os.kill(tracker_process.pid, signal.SIGHUP)
            except ProcessLookupError as error:
                raise RuntimeError("owned tracker exited before SIGHUP reinitialization") from error
            result["checks"].append({"name": "tracking_reinitialization_requested",
                                     "status": "PASS"})

            def reinitialization_succeeded() -> bool:
                events = json_events(tracker_log)
                failures = [event for event in events
                            if event.get("event") == "reinitialization_failed"]
                if failures:
                    raise RuntimeError("tracker reported reinitialization_failed: "
                                       + str(failures[-1].get("reason", "unknown reason")))
                return any(event.get("event") == "tracking_reinitialized"
                           for event in events)

            wait_until("waiting for successful tracking reinitialization", deadline,
                       reinitialization_succeeded, [tracker_process])
            result["checks"].append({"name": "tracking_reinitialized_event",
                                     "status": "PASS"})
        wait_until("waiting for native tracking samples", deadline,
                   lambda: any(event.get("event") == "tracking_samples_consumed"
                               for event in json_events(consumer_log)), processes)
        consumer_events = json_events(consumer_log)
        summaries = [event for event in consumer_events
                     if event.get("event") == "tracking_samples_consumed"]
        tracking_output = verify_tracking_samples(consumer_events, summaries[-1], args.profile)
        result["tracking_output"] = tracking_output
        result["checks"].append({"name": "native_stamped_bytes_consumed", "status": "PASS"})
        result["checks"].append({"name": "native_m3t_tracking_output", "status": "PASS"})
        if args.texture_backend == "dsp":
            result["dsp_audit"] = verify_dsp_audit(tracker_log)
            result["dsp_device"] = "USED"
            result["checks"].append({"name": "dsp_texture_device_audit", "status": "PASS"})
        result["actual_renderer"] = verify_renderers(
            args.rendering, audit_library, tracker_log, fixture_log)
        if result["actual_renderer"] is not None:
            result["checks"].append({"name": "renderer_audit", "status": "PASS"})
        result["status"] = "PASS"
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
    finally:
        stop_children(processes, result["forced_kill"])
        for handle in handles:
            handle.close()
        result["children_reaped"] = all(process.poll() is not None for process in processes)
        result["elapsed_seconds"] = round(time.monotonic() - started_at, 3)
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())

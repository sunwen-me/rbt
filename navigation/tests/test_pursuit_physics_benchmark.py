#!/usr/bin/env python3
"""Unit checks for the matched MuJoCo pursuit experiment runner."""

from __future__ import annotations

import importlib.util
import math
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "pursuit_physics_benchmark.py"
SPEC = importlib.util.spec_from_file_location("pursuit_physics_benchmark", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
benchmark = importlib.util.module_from_spec(SPEC)
import sys

sys.modules[SPEC.name] = benchmark
SPEC.loader.exec_module(benchmark)


class PursuitPhysicsBenchmarkTest(unittest.TestCase):
    def test_slippery_and_wheel_fault_profiles_are_separated(self) -> None:
        self.assertEqual(benchmark.DEFAULT_PROFILES, ("physics", "delayed", "slippery"))
        self.assertIn("wheel_fault", benchmark.PROFILES)
        slippery = benchmark.PROFILES["slippery"]
        wheel_fault = benchmark.PROFILES["wheel_fault"]

        self.assertEqual(slippery["contact_mu"], 0.25)
        self.assertEqual(slippery["left_radius_scale"], 1.0)
        self.assertEqual(slippery["right_radius_scale"], 1.0)
        self.assertEqual(wheel_fault["contact_mu"], 0.25)
        self.assertEqual(wheel_fault["left_radius_scale"], 1.01)
        self.assertEqual(wheel_fault["right_radius_scale"], 0.99)
        self.assertIn("rigid-contact drive-wheel unloading stress", wheel_fault["interpretation"])
        self.assertIn("not a nominal tire-wear prediction", wheel_fault["interpretation"])
        for profile in benchmark.PROFILES.values():
            self.assertEqual(profile["drive_armature"], 0.001)
        arguments = benchmark.parse_arguments([
            "--summary", "baseline.json", "--bridge", "bridge",
            "--geometry", "geometry.yaml", "--output", "results",
            "--profile", "wheel_fault",
        ])
        self.assertEqual(arguments.profiles, ["wheel_fault"])

    def test_drive_armature_is_supported_by_physics_base(self) -> None:
        base = benchmark.make_physics_base(
            benchmark.load_physics_base(),
            [(-0.2, -0.15), (0.2, -0.15), (0.2, 0.15), (-0.2, 0.15)],
            [],
            benchmark.PROFILES["slippery"],
        )

        self.assertEqual(base.params["drive_armature"], 0.001)
        self.assertEqual(base.metadata["drive_armature_kg_m2_assumed"], 0.001)

    def test_block_and_flow_geometry_yaml_load_identically(self) -> None:
        block_yaml = """\
frame: base_footprint
drive_to_base:
  - -0.196
  - 0.0
  - 0.0
footprint:
  - [-0.3, -0.2]
  - [0.1, -0.2]
  - [0.1, 0.2]
  - [-0.3, 0.2]
"""
        flow_yaml = """\
{frame: base_footprint, drive_to_base: [-0.196, 0.0, 0.0], footprint: [[-0.3, -0.2], [0.1, -0.2], [0.1, 0.2], [-0.3, 0.2]]}
"""
        with tempfile.TemporaryDirectory(prefix="pursuit-geometry-yaml-") as directory:
            root = Path(directory)
            block_path = root / "block.yaml"
            flow_path = root / "flow.yaml"
            block_path.write_text(block_yaml, encoding="utf-8")
            flow_path.write_text(flow_yaml, encoding="utf-8")

            block = benchmark.load_footprint(block_path)
            flow = benchmark.load_footprint(flow_path)

        self.assertEqual(block, flow)
        self.assertEqual(len(block), 4)

    def test_geometry_yaml_validates_frame_and_vertex_schema(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pursuit-invalid-geometry-") as directory:
            path = Path(directory) / "geometry.yaml"
            path.write_text(
                "frame: base_link\ndrive_to_base: [0, 0, 0]\n"
                "footprint: [[0, 0], [1, 0], [0, 1]]\n",
                encoding="utf-8",
            )
            with self.assertRaises(benchmark.BenchmarkError):
                benchmark.load_footprint(path)

            path.write_text(
                "frame: base_footprint\ndrive_to_base: [0, 0, 0]\n"
                "footprint: [[0, 0], [1, 0, 0], [0, 1]]\n",
                encoding="utf-8",
            )
            with self.assertRaises(benchmark.BenchmarkError):
                benchmark.load_footprint(path)

    def test_merged_rectangles_preserve_occupied_cell_union(self) -> None:
        resolution = 0.5
        occupied = {(0, 0), (1, 0), (0, 1)}
        cells = tuple(
            (x, y, x * resolution, y * resolution, 100 if (x, y) in occupied else 0)
            for y in range(2)
            for x in range(3)
        )
        grid = benchmark.MapCells(3, 2, resolution, 0.0, 0.0, cells)

        rectangles = benchmark.merge_obstacle_rectangles(grid)

        self.assertEqual(len(rectangles), 2)
        self.assertAlmostEqual(
            sum((xmax - xmin) * (ymax - ymin)
                for xmin, ymin, xmax, ymax in rectangles),
            len(occupied) * resolution * resolution,
        )
        for y in range(2):
            for x in range(3):
                center = ((x + 0.5) * resolution, (y + 0.5) * resolution)
                covered = any(
                    xmin <= center[0] < xmax and ymin <= center[1] < ymax
                    for xmin, ymin, xmax, ymax in rectangles
                )
                self.assertEqual(covered, (x, y) in occupied)

    def test_map_boundary_is_an_exact_collision_surface(self) -> None:
        footprint = [(-0.1, -0.1), (0.1, -0.1), (0.1, 0.1), (-0.1, 0.1)]
        bounds = (0.0, 0.0, 1.0, 1.0)

        inside = benchmark.transform_footprint(footprint, (0.2, 0.5, 0.0))
        outside = benchmark.transform_footprint(footprint, (0.05, 0.5, 0.0))

        self.assertFalse(benchmark.polygon_outside_map(inside, bounds))
        self.assertTrue(benchmark.polygon_outside_map(outside, bounds))
        self.assertTrue(benchmark.swept_boundary_collision(
            footprint, (0.3, 0.5, 0.0), (0.05, 0.5, 0.0), bounds
        ))

    def test_delayed_pose_stream_has_seed_paired_noise_and_timestamps(self) -> None:
        profile = benchmark.PROFILES["delayed"]
        first = benchmark.PoseObserver(profile, 7, (1.0, 2.0, 0.2))
        paired = benchmark.PoseObserver(profile, 7, (1.0, 2.0, 0.2))
        observations = []
        for tick in range(10):
            truth = (1.0 + tick * 0.01, 2.0 - tick * 0.005, 0.2 + tick * 0.002)
            a = first.observe(tick, truth)
            b = paired.observe(tick, truth)
            self.assertEqual(a, b)
            observations.append(a)

        self.assertAlmostEqual(observations[0].stamp, -0.06)
        self.assertAlmostEqual(0.0 - observations[0].stamp, 0.06)
        self.assertAlmostEqual(observations[3].stamp, 0.0)
        self.assertAlmostEqual(observations[8].stamp, 0.10)

    def test_projection_interpolates_yaw_across_wrap(self) -> None:
        path = [(0.0, 0.0, math.radians(179.0)),
                (1.0, 0.0, math.radians(-179.0))]

        _, _, yaw_error, position_error = benchmark.project_path(
            path, (0.5, 0.0, math.pi)
        )

        self.assertAlmostEqual(yaw_error, 0.0, places=10)
        self.assertAlmostEqual(position_error, 0.0)
        self.assertAlmostEqual(benchmark.wrap_angle(-math.pi), -math.pi)

    def test_tipped_alias_and_settle_collision_prevent_truth_goal(self) -> None:
        self.assertTrue(benchmark.state_tipped({"tipped": True}))
        self.assertTrue(benchmark.state_tipped({"tip": True}))
        goal = (2.0, 3.0, 0.2)

        self.assertTrue(benchmark.truth_goal_status(True, goal, goal, 0.0, 0.0, 0.0, False))
        self.assertFalse(benchmark.truth_goal_status(True, goal, goal, 0.0, 0.0, 0.0, True))
        self.assertFalse(benchmark.truth_goal_status(False, goal, goal, 0.0, 0.0, 0.0, False))

    def test_post_goal_checkpoints_separate_pose_and_stopped_tolerances(self) -> None:
        goal = (0.0, 0.0, 0.0)
        pose = (0.079, 0.0, 0.02)
        one_second_state = {"linear_x": 0.0002, "lateral_y": 0.0, "angular_z": 0.0015}
        three_second_state = {"linear_x": 0.0001, "lateral_y": 0.0, "angular_z": 0.0005}

        report = benchmark.truth_checkpoint(
            "reported_goal", 0.0, pose, one_second_state, goal, False
        )
        one_second = benchmark.truth_checkpoint(
            "post_goal_1s", 1.0, pose, one_second_state, goal, False
        )
        three_seconds = benchmark.truth_checkpoint(
            "post_goal_3s", 3.0, pose, three_second_state, goal, False
        )

        self.assertTrue(report["ground_truth_pose_within_existing_tolerances"])
        self.assertFalse(report["ground_truth_stopped_within_existing_tolerances"])
        self.assertFalse(one_second["ground_truth_stopped_within_existing_tolerances"])
        self.assertTrue(three_seconds["ground_truth_stopped_within_existing_tolerances"])
        self.assertFalse(benchmark.truth_goal_status(
            True, pose, goal, 0.0002, 0.0, 0.0015, False
        ))
        self.assertTrue(benchmark.truth_goal_status(
            True, pose, goal, 0.0001, 0.0, 0.0005, False
        ))
        self.assertEqual(benchmark.POST_GOAL_CHECKPOINT_SECONDS, (0.0, 1.0, 3.0))
        self.assertEqual(benchmark.POST_GOAL_OBSERVE_SECONDS, 3.0)

    def test_parser_and_summary_keep_failures_visible(self) -> None:
        with tempfile.TemporaryDirectory(prefix="pursuit-physics-unit-") as directory:
            root = Path(directory)
            invalid_json = root / "invalid.json"
            invalid_json.write_text('{"run_count": NaN}\n', encoding="utf-8")
            with self.assertRaises(benchmark.BenchmarkError):
                benchmark.read_json(invalid_json)

            invalid_path = root / "invalid-path.csv"
            invalid_path.write_text(
                "index,x,y,body_yaw,production_pose_free,production_segment_free\n"
                "0,0,0,0,1,1\n"
                "1,nan,0,0,1,1\n",
                encoding="utf-8",
            )
            with self.assertRaises(benchmark.BenchmarkError):
                benchmark.parse_path_csv(invalid_path)

        runs = [
            {"controller": "rpp", "outcome": "reported_goal", "truth_outcome": "truth_goal"},
            {"controller": "rpp", "outcome": "collision", "truth_outcome": "not_reported",
             "collision_any": True},
            {"controller": "rpp", "outcome": "runner_error", "truth_outcome": "not_reported"},
            {"controller": "rpp", "outcome": "controller_error", "truth_outcome": "not_reported"},
        ]
        counts = benchmark.summarized_outcomes(runs)["rpp"]
        self.assertEqual(counts["denominator"], 4)
        self.assertEqual(counts["reported_goal"], 1)
        self.assertEqual(counts["truth_goal"], 1)
        self.assertEqual(counts["runner_error"], 1)
        self.assertEqual(counts["controller_error"], 1)
        self.assertEqual(counts["collision"], 1)
        self.assertEqual(counts["collision_any"], 1)
        self.assertEqual(sum(counts["outcomes"].values()), 4)


if __name__ == "__main__":
    unittest.main()

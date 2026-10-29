"""Focused tests for the pure-Python URDF to M3T geometry converter."""

from __future__ import annotations

import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parents[1] / "scripts"
sys.path.insert(0, str(SCRIPT_DIR))

from prepare_robot import ConversionError, convert_urdf  # noqa: E402


USER_URDF = Path("/home/sw/so101_sim/so101_new_calib_with_handeye.urdf")


def write_ascii_stl(path: Path, triangles: list[tuple[tuple[float, float, float], ...]]) -> None:
    lines = ["solid synthetic"]
    for triangle in triangles:
        lines.extend(("  facet normal 0 0 1", "    outer loop"))
        lines.extend(f"      vertex {x} {y} {z}" for x, y, z in triangle)
        lines.extend(("    endloop", "  endfacet"))
    lines.append("endsolid synthetic")
    path.write_text("\n".join(lines) + "\n", encoding="ascii")


def write_urdf(path: Path, links: str, joints: str = "", name: str = "fixture") -> None:
    path.write_text(f'<robot name="{name}">{links}{joints}</robot>\n', encoding="utf-8")


def read_obj_vertices(path: Path) -> list[tuple[float, float, float]]:
    vertices = []
    for line in path.read_text(encoding="ascii").splitlines():
        if line.startswith("v "):
            vertices.append(tuple(float(value) for value in line.split()[1:4]))
    return vertices


class PrepareRobotTests(unittest.TestCase):
    def test_visual_meshes_merge_with_scale_and_urdf_origin(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_ascii_stl(
                root / "first.stl",
                [((1, 0, 0), (0, 1, 0), (0, 0, 1))],
            )
            write_ascii_stl(
                root / "second.stl",
                [((-2, 0, 0), (0, -2, 0), (0, 0, -2))],
            )
            urdf = root / "robot.urdf"
            write_urdf(
                urdf,
                '<link name="base">'
                '<visual><origin xyz="1 2 3" rpy="1.5707963267948966 0 1.5707963267948966"/>'
                '<geometry><mesh filename="first.stl" scale="2 3 4"/></geometry></visual>'
                '<visual><origin xyz="-1 -1 -1"/><geometry>'
                '<mesh filename="second.stl" scale="0.5 0.5 0.5"/></geometry></visual>'
                '</link>',
            )

            robot_json = convert_urdf(urdf, root / "out")
            robot = json.loads(robot_json.read_text(encoding="utf-8"))
            link = robot["links"][0]
            self.assertEqual(link["visuals"], 2)
            self.assertEqual(link["mesh"], "meshes/base.obj")
            vertices = read_obj_vertices(robot_json.parent / link["mesh"])
            self.assertEqual(len(vertices), 6)
            expected_first = [(1, 4, 3), (1, 2, 6), (5, 2, 3)]
            for actual, expected in zip(vertices[:3], expected_first):
                for actual_value, expected_value in zip(actual, expected):
                    self.assertAlmostEqual(actual_value, expected_value, places=7)
            bounds = [
                (min(vertex[axis] for vertex in vertices), max(vertex[axis] for vertex in vertices))
                for axis in range(3)
            ]
            self.assertEqual(bounds, [(-2.0, 5.0), (-2.0, 4.0), (-2.0, 6.0)])
            self.assertEqual(len([line for line in (robot_json.parent / link["mesh"]).read_text().splitlines() if line.startswith("f ")]), 2)
            self.assertEqual(len(robot["mesh_sources"]), 2)
            self.assertTrue(all(Path(source["path"]).is_absolute() for source in robot["mesh_sources"]))
            self.assertTrue(all(len(source["sha256"]) == 64 for source in robot["mesh_sources"]))

    def test_joint_origin_axis_limits_and_numeric_joint_names(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            write_urdf(
                root / "robot.urdf",
                '<link name="base"/><link name="1"/>',
                '<joint name="6" type="revolute"><parent link="base"/><child link="1"/>'
                '<origin xyz="1 2 3" rpy="0 0 1.5707963267948966"/><axis xyz="0 0 2"/>'
                '<limit lower="-0.5" upper="1.25"/></joint>',
            )
            robot = json.loads(convert_urdf(root / "robot.urdf", root / "out").read_text())
            joint = robot["joints"][0]
            self.assertEqual(joint["name"], "6")
            self.assertEqual(joint["axis"], [0.0, 0.0, 1.0])
            self.assertEqual((joint["lower"], joint["upper"]), (-0.5, 1.25))
            self.assertEqual(joint["origin"][3:12:4], [1.0, 2.0, 3.0])
            self.assertAlmostEqual(joint["origin"][0], 0.0, places=12)
            self.assertAlmostEqual(joint["origin"][1], -1.0, places=12)
            self.assertAlmostEqual(joint["origin"][4], 1.0, places=12)
            self.assertEqual(joint["origin"][15], 1.0)

    def test_default_fov_exclusion_keeps_fixed_kinematic_node(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            urdf = root / "robot.urdf"
            write_urdf(
                urdf,
                '<link name="base"/><link name="handeye_cam">'
                '<visual><geometry><mesh filename="intentionally-missing.stl"/></geometry></visual></link>',
                '<joint name="camera_mount" type="fixed"><parent link="base"/><child link="handeye_cam"/>'
                '<origin xyz="0.1 0.2 0.3"/></joint>',
            )
            robot = json.loads(convert_urdf(urdf, root / "out").read_text(encoding="utf-8"))
            camera = next(link for link in robot["links"] if link["name"] == "handeye_cam")
            self.assertEqual(camera, {"name": "handeye_cam", "mesh": "", "visuals": 0})
            self.assertEqual(robot["root_link"], "base")
            self.assertEqual(len(robot["joints"]), 1)
            joint = robot["joints"][0]
            self.assertEqual((joint["name"], joint["type"], joint["parent"], joint["child"]),
                             ("camera_mount", "fixed", "base", "handeye_cam"))
            self.assertEqual((joint["lower"], joint["upper"]), (0.0, 0.0))

    def test_malformed_mesh_and_graph_fail_without_publishing_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bad_mesh = root / "bad.stl"
            bad_mesh.write_text("solid broken\n facet normal 0 0 1\nendsolid broken\n", encoding="ascii")
            bad_mesh_urdf = root / "bad-mesh.urdf"
            write_urdf(
                bad_mesh_urdf,
                '<link name="base"><visual><geometry><mesh filename="bad.stl"/></geometry></visual></link>',
            )
            bad_mesh_output = root / "bad-mesh-out"
            with self.assertRaisesRegex(ConversionError, "outer loop"):
                convert_urdf(bad_mesh_urdf, bad_mesh_output)
            self.assertFalse(bad_mesh_output.exists())
            self.assertFalse(any(root.glob(".bad-mesh-out.tmp-*")))

            bad_graph_urdf = root / "bad-graph.urdf"
            write_urdf(
                bad_graph_urdf,
                '<link name="base"/><link name="arm"/>',
                '<joint name="broken" type="fixed"><parent link="missing"/><child link="arm"/></joint>',
            )
            with self.assertRaisesRegex(ConversionError, "missing parent link"):
                convert_urdf(bad_graph_urdf, root / "bad-graph-out")

    def test_existing_output_and_unsupported_geometry_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            urdf = root / "robot.urdf"
            write_urdf(urdf, '<link name="base"><visual><geometry><box size="1 1 1"/></geometry></visual></link>')
            existing = root / "existing"
            existing.mkdir()
            marker = existing / "keep.txt"
            marker.write_text("preserve", encoding="utf-8")
            with self.assertRaisesRegex(ConversionError, "already exists"):
                convert_urdf(urdf, existing)
            self.assertEqual(marker.read_text(encoding="utf-8"), "preserve")

            with self.assertRaisesRegex(ConversionError, "unsupported geometry <box>"):
                convert_urdf(urdf, root / "unsupported")
            self.assertFalse((root / "unsupported").exists())

    @unittest.skipUnless(USER_URDF.is_file(), "the user-provided SO-101 URDF is unavailable")
    def test_actual_so101_model_generation_is_read_only(self) -> None:
        repo_root = Path(__file__).resolve().parents[3]
        target = repo_root / "target"
        target.mkdir(exist_ok=True)
        assets = sorted((USER_URDF.parent / "assets").glob("*.stl"))
        paths = [USER_URDF, *assets]
        before = {path: hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}
        with tempfile.TemporaryDirectory(prefix="m3t-robot-converter-test-", dir=target) as trial:
            robot_json = convert_urdf(USER_URDF, Path(trial) / "converted")
            robot = json.loads(robot_json.read_text(encoding="utf-8"))
            self.assertEqual((len(robot["links"]), len(robot["joints"])), (10, 9))
            self.assertEqual(robot["root_link"], "base")
            self.assertEqual(sum(joint["type"] == "revolute" for joint in robot["joints"]), 6)
            self.assertEqual(sum(joint["type"] == "fixed" for joint in robot["joints"]), 3)
            self.assertTrue(all(Path(robot_json.parent / link["mesh"]).is_file()
                                for link in robot["links"] if link["mesh"]))
            camera = next(link for link in robot["links"] if link["name"] == "handeye_cam")
            self.assertEqual((camera["mesh"], camera["visuals"]), ("", 0))
            self.assertTrue(any(joint["name"] == "module_to_cam" and joint["type"] == "fixed"
                                for joint in robot["joints"]))
        after = {path: hashlib.sha256(path.read_bytes()).hexdigest() for path in paths}
        self.assertEqual(after, before)


if __name__ == "__main__":
    unittest.main()

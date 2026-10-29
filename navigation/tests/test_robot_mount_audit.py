from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

import numpy as np
import yaml


REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "navigation" / "scripts"))

from robot_mount_audit import AuditError, audit_robot_mount  # noqa: E402


ROBOT_XACRO = """\
<robot name="robot" xmlns:xacro="http://www.ros.org/wiki/xacro">
  <link name="base_footprint"/>
  <link name="base_link"/>
  <link name="mid360_rgl_frame"/>
  <joint name="base_footprint_joint" type="fixed">
    <origin xyz="-0.196 0 0" rpy="0 0 0"/>
    <parent link="base_footprint"/>
    <child link="base_link"/>
  </joint>
  <joint name="mid360_rgl_joint" type="fixed">
    <origin xyz="0.143 0 0.735" rpy="3.141592653589793 0 0"/>
    <parent link="base_link"/>
    <child link="mid360_rgl_frame"/>
  </joint>
</robot>
"""

ROBOT_GAZEBO = """\
<robot name="robot" xmlns:xacro="http://www.ros.org/wiki/xacro">
  <xacro:unless value="$(arg use_gazebo_classic)">
    <gazebo reference="base_link">
      <sensor name="mid360_rgl" type="custom">
        <pose>0.143 0 0.735 3.141592653589793 0 0</pose>
        <plugin name="rgl" filename="RGLServerPluginInstance">
          <publish_world_points>false</publish_world_points>
          <frame>mid360_rgl_frame</frame>
        </plugin>
      </sensor>
    </gazebo>
  </xacro:unless>
</robot>
"""


def _config(
    odom_translation: list[float],
    odom_rpy_deg: list[float],
    lidar_translation: list[float] | None = None,
    lidar_rotation: np.ndarray | None = None,
) -> dict[str, object]:
    if lidar_translation is None:
        lidar_translation = [-0.011, -0.02329, 0.04412]
    if lidar_rotation is None:
        lidar_rotation = np.eye(3)
    return {
        "lio.extrinsic.lidar_imu": lidar_translation + lidar_rotation.reshape(-1).tolist(),
        "lio.extrinsic.odom_robo": odom_translation + odom_rpy_deg,
        "lio.frames.base": "base_footprint",
    }


class RobotMountAuditTest(unittest.TestCase):
    def _audit_config(
        self,
        config: dict[str, object],
        gazebo_text: str | None = None,
        xacro_text: str = ROBOT_XACRO,
    ):
        with tempfile.TemporaryDirectory() as directory:
            temp_dir = Path(directory)
            xacro_path = temp_dir / "robot.xacro"
            xacro_path.write_text(xacro_text, encoding="utf-8")
            gazebo_path = temp_dir / "robot.gazebo"
            gazebo_path.write_text(
                ROBOT_GAZEBO if gazebo_text is None else gazebo_text,
                encoding="utf-8",
            )
            config_path = temp_dir / "lio.yaml"
            config_path.write_text(yaml.safe_dump(config), encoding="utf-8")
            return audit_robot_mount(xacro_path, gazebo_path, config_path)

    def test_legacy_mount_translation_is_reported_as_about_51_mm_error(self) -> None:
        report = self._audit_config(_config([-0.053, 0.0, 0.735], [180.0, 0.0, 0.0]))
        self.assertFalse(report["valid"])
        error = report["comparisons"]["joint_chain_vs_effective_lio"]["translation_error_m"]
        self.assertAlmostEqual(error, 0.05108814441727164, places=9)

    def test_corrected_translation_and_roll_match_authoritative_mount(self) -> None:
        report = self._audit_config(
            _config([-0.042, -0.02329, 0.77912], [180.0, 0.0, 0.0])
        )
        self.assertTrue(report["valid"], report["comparisons"])
        self.assertTrue(
            report["comparisons"]["joint_chain_vs_gazebo_sensor"]["within_tolerance"]
        )
        self.assertLess(
            report["comparisons"]["joint_chain_vs_effective_lio"]["translation_error_m"],
            1e-6,
        )

    def test_gazebo_sensor_pose_mismatch_is_rejected(self) -> None:
        mismatched = ROBOT_GAZEBO.replace(
            "<pose>0.143 0 0.735 3.141592653589793 0 0</pose>",
            "<pose>0.144 0 0.735 3.141592653589793 0 0</pose>",
            1,
        )
        self.assertNotEqual(ROBOT_GAZEBO, mismatched)
        report = self._audit_config(
            _config([-0.042, -0.02329, 0.77912], [180.0, 0.0, 0.0]), mismatched
        )
        self.assertFalse(report["valid"])
        self.assertFalse(
            report["comparisons"]["joint_chain_vs_gazebo_sensor"]["within_tolerance"]
        )
        self.assertAlmostEqual(
            report["comparisons"]["joint_chain_vs_gazebo_sensor"]["translation_error_m"],
            0.001,
            places=8,
        )

    def test_internal_lidar_rotation_is_composed_into_required_imu_pose(self) -> None:
        roll_90 = np.array(
            [[1.0, 0.0, 0.0], [0.0, 0.0, -1.0], [0.0, 1.0, 0.0]],
            dtype=np.float64,
        )
        lidar_translation = np.array([-0.011, -0.02329, 0.04412])
        target_rotation = np.diag([1.0, -1.0, -1.0])
        required_base_imu_rotation = target_rotation @ roll_90.T
        required_base_imu_translation = np.array([-0.053, 0.0, 0.735]) - (
            required_base_imu_rotation @ lidar_translation
        )
        report = self._audit_config(
            _config(
                required_base_imu_translation.tolist(),
                [90.0, 0.0, 0.0],
                lidar_rotation=roll_90,
            )
        )
        self.assertTrue(report["valid"], report["comparisons"])
        actual_rotation = np.asarray(
            report["effective_lio_base_to_sensor"]["rotation_row_major"]
        )
        np.testing.assert_allclose(actual_rotation, target_rotation, atol=1e-12)
        self.assertFalse(
            report["required_lio_extrinsics_for_joint_chain"]["lidar_imu_calibration_modified"]
        )

    def test_macro_and_nonfinite_joint_offsets_fail_explicitly(self) -> None:
        config = _config([-0.042, -0.02329, 0.77912], [180.0, 0.0, 0.0])
        macro_offset = ROBOT_XACRO.replace(
            'xyz="-0.196 0 0"', 'xyz="$(arg base_offset) 0 0"'
        )
        with self.assertRaisesRegex(AuditError, "explicit numeric values"):
            self._audit_config(config, xacro_text=macro_offset)

        nonfinite_offset = ROBOT_XACRO.replace('xyz="-0.196 0 0"', 'xyz="nan 0 0"')
        with self.assertRaisesRegex(AuditError, "finite"):
            self._audit_config(config, xacro_text=nonfinite_offset)


if __name__ == "__main__":
    unittest.main()

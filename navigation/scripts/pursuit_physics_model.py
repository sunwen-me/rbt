"""A small MuJoCo model of the robot's source-backed differential drive base.

The model deliberately contains only the chassis, drive wheels, rear casters,
and a fixed lidar mass.  The robot is free floating in six degrees of freedom;
translation and rotation come from wheel torques and contact forces.
"""

from __future__ import annotations

import math
from typing import Any, Iterable

import mujoco
import numpy as np


_DEFAULTS: dict[str, float | str] = {
    "wheel_radius": 0.05035,
    "nominal_track": 0.38550,
    "left_radius_scale": 1.0,
    "right_radius_scale": 1.0,
    "nominal_mu": 1.0,
    "caster_mu": 0.2,
    "drive_kv": 0.4,
    "max_torque": 0.6,
    "drive_armature": 0.00001,
    "physics_dt": 0.002,
    "caster_trail": 0.0,
    "tip_angle": math.radians(30.0),
    "drive_tire_shape": "cylinder",
    "caster_tire_shape": "cylinder",
}

_SOURCE_CONTROLLER_TRACK = 0.320
_SOURCE_CONTROLLER_TRACK_MULTIPLIER = 3.9
_AXLE_HEIGHT = 0.05035
_DRIVE_JOINT_Y = {"left": 0.15999, "right": -0.16001}
_DRIVE_CONTACT_Y = {"left": 0.19274, "right": -0.19276}
_DRIVE_WHEEL_MASS = 0.0369474
_DRIVE_WHEEL_INERTIA = (2.7e-5, 4.2e-5, 2.7e-5)
_CASTER_PIVOT_X = -0.301
_CASTER_PIVOT_Y = (0.20175, -0.20175)
_CASTER_PIVOT_Z_FROM_AXLE = 0.00665
_CASTER_WHEEL_Z_FROM_PIVOT = -0.0305
_CASTER_WHEEL_RADIUS = 0.026


def _finite_float(value: Any, name: str) -> float:
    try:
        result = float(value)
    except (TypeError, ValueError) as exc:
        raise ValueError(f"{name} must be a finite number") from exc
    if not math.isfinite(result):
        raise ValueError(f"{name} must be a finite number")
    return result


def _format(value: float) -> str:
    return format(float(value), ".12g")


def _vector(values: Iterable[float]) -> str:
    return " ".join(_format(value) for value in values)


def _footprint_points(footprint: Iterable[Iterable[float]]) -> list[tuple[float, float]]:
    points: list[tuple[float, float]] = []
    try:
        for point in footprint:
            if len(point) != 2:
                raise ValueError("footprint points must contain x and y")
            points.append((_finite_float(point[0], "footprint x"), _finite_float(point[1], "footprint y")))
    except TypeError as exc:
        raise ValueError("footprint must be a sequence of x/y points") from exc
    if len(points) < 3:
        raise ValueError("footprint must have at least three points")

    area = sum(
        points[index][0] * points[(index + 1) % len(points)][1]
        - points[(index + 1) % len(points)][0] * points[index][1]
        for index in range(len(points))
    )
    if abs(area) < 1e-10:
        raise ValueError("footprint polygon must have non-zero area")

    turn_sign = 0
    for index, current in enumerate(points):
        previous = points[index - 1]
        following = points[(index + 1) % len(points)]
        cross = (current[0] - previous[0]) * (following[1] - current[1]) - (
            current[1] - previous[1]
        ) * (following[0] - current[0])
        if abs(cross) < 1e-10:
            continue
        sign = 1 if cross > 0 else -1
        if turn_sign and sign != turn_sign:
            raise ValueError("footprint polygon must be convex")
        turn_sign = sign
    if not turn_sign:
        raise ValueError("footprint polygon must have non-zero area")
    if area < 0:
        points.reverse()
    return points


def _obstacle_boxes(obstacles: Iterable[Iterable[float]]) -> list[tuple[float, float, float, float]]:
    boxes: list[tuple[float, float, float, float]] = []
    try:
        for obstacle in obstacles:
            if len(obstacle) != 4:
                raise ValueError("obstacles must be xmin/ymin/xmax/ymax tuples")
            xmin, ymin, xmax, ymax = (
                _finite_float(value, "obstacle bound") for value in obstacle
            )
            if xmin >= xmax or ymin >= ymax:
                raise ValueError("obstacle boxes must have positive width and height")
            boxes.append((xmin, ymin, xmax, ymax))
    except TypeError as exc:
        raise ValueError("obstacles must be a sequence of rectangle bounds") from exc
    return boxes


def _composite_inertia() -> tuple[float, np.ndarray, np.ndarray]:
    """Combine source body, base-link, and lidar inertials at axle coordinates."""
    pieces = [
        (
            11.525313075929766,
            np.array((-0.196 + 0.0024747785463641757, -5.224869372213094e-7, 0.105 + 0.4359614866624879 - _AXLE_HEIGHT)),
            np.array(
                (
                    (0.993235, 1e-6, 0.036668),
                    (1e-6, 0.82793, -3e-6),
                    (0.036668, -3e-6, 0.366075),
                )
            ),
        ),
        (
            0.0014564559221618755,
            np.array((-0.196, 0.0, 0.1045 - _AXLE_HEIGHT)),
            np.zeros((3, 3)),
        ),
        (
            0.41131899137404504,
            np.array((-0.196 + 0.143 - 0.033387927053794855, 1.7037999626763844e-5, 0.105 + 0.63 - 0.029784945322632983 - _AXLE_HEIGHT)),
            np.array(
                (
                    (0.000237, 0.0, 7e-6),
                    (0.0, 0.000251, 0.0),
                    (7e-6, 0.0, 0.000164),
                )
            ),
        ),
    ]
    mass = sum(piece[0] for piece in pieces)
    center = sum((piece_mass * position for piece_mass, position, _ in pieces), np.zeros(3)) / mass
    inertia = np.zeros((3, 3))
    for piece_mass, position, own_inertia in pieces:
        offset = position - center
        inertia += own_inertia + piece_mass * (
            np.dot(offset, offset) * np.eye(3) - np.outer(offset, offset)
        )
    return mass, center, inertia


class PhysicsBase:
    """Physics-backed differential-drive base used by pursuit experiments.

    Args:
        footprint: Convex polygon in base_footprint coordinates, in metres.
        obstacles: Axis-aligned map rectangles ``(xmin, ymin, xmax, ymax)``.
        params: Optional physical and servo parameter overrides.  The command
            passed to :meth:`step` is ``(linear_x_m_s, angular_z_rad_s)``.
    """

    def __init__(
        self,
        footprint: list[list[float]] | list[tuple[float, float]],
        obstacles: list[tuple[float, float, float, float]],
        params: dict[str, Any] | None = None,
    ) -> None:
        self.footprint = _footprint_points(footprint)
        self.obstacles = _obstacle_boxes(obstacles)
        supplied = dict(params or {})
        aliases = {"mu": "nominal_mu", "torque": "max_torque", "kv": "drive_kv"}
        for alias, key in aliases.items():
            if alias in supplied:
                if key in supplied:
                    raise ValueError(f"use either {alias} or {key}, not both")
                supplied[key] = supplied.pop(alias)
        unknown = set(supplied) - set(_DEFAULTS) - {
            "controller_nominal_track",
            "controller_track_multiplier",
        }
        if unknown:
            raise ValueError(f"unknown physics parameter(s): {', '.join(sorted(unknown))}")

        self.params = dict(_DEFAULTS)
        for key, value in supplied.items():
            if key in self.params and key.endswith("_shape"):
                shape = str(value).strip().lower()
                if shape not in {"cylinder", "ellipsoid"}:
                    raise ValueError(f"{key} must be 'cylinder' or 'ellipsoid'")
                self.params[key] = shape
            elif key in self.params:
                self.params[key] = _finite_float(value, key)
        for key in ("wheel_radius", "nominal_track", "left_radius_scale", "right_radius_scale", "physics_dt", "tip_angle"):
            if self.params[key] <= 0:
                raise ValueError(f"{key} must be greater than zero")
        for key in ("nominal_mu", "caster_mu", "drive_kv", "max_torque", "drive_armature"):
            if self.params[key] < 0:
                raise ValueError(f"{key} must be non-negative")
        if self.params["caster_trail"] < 0:
            raise ValueError("caster_trail must be non-negative")
        if self.params["tip_angle"] >= math.pi / 2:
            raise ValueError("tip_angle must be less than 90 degrees")

        self.controller_nominal_track = _finite_float(
            supplied.get("controller_nominal_track", _SOURCE_CONTROLLER_TRACK),
            "controller_nominal_track",
        )
        self.controller_track_multiplier = _finite_float(
            supplied.get("controller_track_multiplier", _SOURCE_CONTROLLER_TRACK_MULTIPLIER),
            "controller_track_multiplier",
        )
        if self.controller_nominal_track <= 0 or self.controller_track_multiplier <= 0:
            raise ValueError("controller track metadata values must be greater than zero")

        self.model_xml = self._build_xml()
        self.model = mujoco.MjModel.from_xml_string(self.model_xml)
        self.data = mujoco.MjData(self.model)
        self._root_joint = self._joint("root_free")
        self._drive_joints = {side: self._joint(f"drive_{side}") for side in ("left", "right")}
        self._caster_yaw_joints = tuple(self._joint(f"caster_yaw_{index}") for index in (0, 1))
        self._caster_wheel_joints = tuple(self._joint(f"caster_wheel_{index}") for index in (0, 1))
        self._drive_actuators = {
            side: mujoco.mj_name2id(self.model, mujoco.mjtObj.mjOBJ_ACTUATOR, f"drive_motor_{side}")
            for side in ("left", "right")
        }
        self._robot_geom_ids = {
            geom_id
            for geom_id in range(self.model.ngeom)
            if not self.model.geom(geom_id).name.startswith("obs_")
            and self.model.geom(geom_id).name != "ground"
            and self.model.geom_contype[geom_id] != 0
        }
        self._obstacle_geom_ids = {
            geom_id
            for geom_id in range(self.model.ngeom)
            if self.model.geom(geom_id).name.startswith("obs_")
        }
        self._obstacle_contact = False
        self._roll_tip = False
        self._pitch_tip = False
        self._max_abs_roll = 0.0
        self._max_abs_pitch = 0.0
        self._last_torques = (0.0, 0.0)
        self._target_wheels = (0.0, 0.0)
        self.metadata = self._metadata()
        self.reset((0.0, 0.0, 0.0))

    def _joint(self, name: str) -> int:
        joint_id = mujoco.mj_name2id(self.model, mujoco.mjtObj.mjOBJ_JOINT, name)
        if joint_id < 0:
            raise RuntimeError(f"MuJoCo model is missing joint {name}")
        return joint_id

    def _build_xml(self) -> str:
        p = self.params
        mass, com, inertia = _composite_inertia()
        principal_values = (
            inertia[0, 0],
            inertia[1, 1],
            inertia[2, 2],
            inertia[0, 1],
            inertia[0, 2],
            inertia[1, 2],
        )
        vertices: list[tuple[float, float, float]] = []
        low_z, high_z = 0.09, 0.62
        vertices.extend((x, y, low_z) for x, y in self.footprint)
        vertices.extend((x, y, high_z) for x, y in self.footprint)
        count = len(self.footprint)
        faces: list[tuple[int, int, int]] = []
        for index in range(1, count - 1):
            faces.append((0, index + 1, index))
            faces.append((count, count + index, count + index + 1))
        for index in range(count):
            next_index = (index + 1) % count
            faces.extend(
                (
                    (index, next_index, count + next_index),
                    (index, count + next_index, count + index),
                )
            )

        xml = [
            '<mujoco model="pursuit_physics_base">',
            '  <compiler angle="radian" inertiafromgeom="false" autolimits="true"/>',
            f'  <option timestep="{_format(p["physics_dt"])}" gravity="0 0 -9.81" integrator="implicitfast" solver="Newton" cone="elliptic" impratio="4" tolerance="1e-10" iterations="80" noslip_iterations="0"/>',
            '  <size nconmax="400" njmax="2000"/>',
            '  <default>',
            '    <geom condim="3" solref="0.005 1" solimp="0.95 0.99 0.001"/>',
            '    <joint damping="0.001" armature="0.00001"/>',
            '  </default>',
            '  <asset>',
            '    <mesh name="chassis_hull" vertex="' + " ".join(_vector(v) for v in vertices) + '" face="' + " ".join(_vector(v) for v in faces) + '"/>',
            '  </asset>',
            '  <worldbody>',
            '    <geom name="ground" type="plane" pos="0 0 0" size="100 100 0.1" contype="1" conaffinity="1" friction="1 1 0.001"/>',
        ]
        for index, (xmin, ymin, xmax, ymax) in enumerate(self.obstacles):
            center = ((xmin + xmax) / 2, (ymin + ymax) / 2, 0.25)
            half = ((xmax - xmin) / 2, (ymax - ymin) / 2, 0.25)
            xml.append(
                f'    <geom name="obs_{index:03d}" type="box" pos="{_vector(center)}" size="{_vector(half)}" contype="1" conaffinity="1" priority="0" friction="{_format(p["nominal_mu"])} {_format(p["nominal_mu"])} 0.001"/>'
            )
        xml.extend(
            (
                '    <body name="base_footprint" pos="0 0 0.05035">',
                f'      <freejoint name="root_free"/>',
                f'      <inertial pos="{_vector(com)}" mass="{_format(mass)}" fullinertia="{_vector(principal_values)}"/>',
                f'      <geom name="chassis" type="mesh" mesh="chassis_hull" contype="1" conaffinity="1" priority="1" friction="{_format(p["nominal_mu"])} {_format(p["nominal_mu"])} 0.001"/>',
                '      <geom name="lidar_lumped_visual" type="box" pos="-0.086388 0 0.600215" size="0.035 0.035 0.035" contype="0" conaffinity="0" rgba="0.4 0.5 0.7 0.15"/>',
            )
        )
        for side in ("left", "right"):
            sign = 1.0 if side == "left" else -1.0
            radius = p["wheel_radius"] * p[f"{side}_radius_scale"]
            joint_y = _DRIVE_JOINT_Y[side]
            contact_offset = _DRIVE_CONTACT_Y[side] - joint_y
            drive_shape = p["drive_tire_shape"]
            drive_size = (
                f'{_format(radius)} {_format(0.03275)}'
                if drive_shape == "cylinder"
                else f'{_format(radius)} {_format(radius)} {_format(0.03275)}'
            )
            xml.extend(
                (
                    f'      <body name="drive_wheel_{side}" pos="0 {_format(joint_y)} 0">',
                    f'        <joint name="drive_{side}" type="hinge" axis="0 1 0" damping="0.001" armature="{_format(p["drive_armature"])}"/>',
                    f'        <inertial pos="0 {_format(sign * 0.03698837)} 0" mass="{_format(_DRIVE_WHEEL_MASS)}" diaginertia="{_vector(_DRIVE_WHEEL_INERTIA)}"/>',
                    f'        <geom name="drive_tire_{side}" type="{drive_shape}" pos="0 {_format(contact_offset)} 0" quat="0.7071067811865476 -0.7071067811865476 0 0" size="{drive_size}" contype="1" conaffinity="1" priority="1" friction="{_format(p["nominal_mu"])} {_format(p["nominal_mu"])} 0.001"/>',
                    '      </body>',
                )
            )
        for index, caster_y in enumerate(_CASTER_PIVOT_Y):
            caster_shape = p["caster_tire_shape"]
            caster_size = (
                f'{_format(_CASTER_WHEEL_RADIUS)} 0.013'
                if caster_shape == "cylinder"
                else f'{_format(_CASTER_WHEEL_RADIUS)} {_format(_CASTER_WHEEL_RADIUS)} 0.013'
            )
            xml.extend(
                (
                    f'      <body name="caster_swivel_{index}" pos="{_format(_CASTER_PIVOT_X)} {_format(caster_y)} {_format(_CASTER_PIVOT_Z_FROM_AXLE)}">',
                    f'        <joint name="caster_yaw_{index}" type="hinge" axis="0 0 1" damping="0.0005" armature="0.000001"/>',
                    '        <inertial pos="0 0 -0.018906678" mass="0.0217138743" diaginertia="0.000005 0.000004 0.000002"/>',
                    '        <geom name="caster_bracket_' + str(index) + '" type="box" pos="0 0 -0.012" size="0.008 0.012 0.008" contype="1" conaffinity="1" priority="1" friction="0.5 0.5 0.001"/>',
                    f'        <body name="caster_rolling_wheel_{index}" pos="{_format(-p["caster_trail"])} 0 {_format(_CASTER_WHEEL_Z_FROM_PIVOT)}">',
                    f'          <joint name="caster_wheel_{index}" type="hinge" axis="0 1 0" damping="0.0005" armature="0.000001"/>',
                    '          <inertial pos="0 0 0" mass="0.0173372718" diaginertia="0.000002 0.000003 0.000002"/>',
                    f'          <geom name="caster_tire_{index}" type="{caster_shape}" quat="0.7071067811865476 0.7071067811865476 0 0" size="{caster_size}" contype="1" conaffinity="1" priority="1" friction="{_format(p["caster_mu"])} {_format(p["caster_mu"])} 0.001"/>',
                    '        </body>',
                    '      </body>',
                )
            )
        xml.extend(
            (
                '    </body>',
                '  </worldbody>',
                '  <actuator>',
                f'    <velocity name="drive_motor_left" joint="drive_left" kv="{_format(p["drive_kv"])}" forcelimited="true" forcerange="{_format(-p["max_torque"])} {_format(p["max_torque"])}"/>',
                f'    <velocity name="drive_motor_right" joint="drive_right" kv="{_format(p["drive_kv"])}" forcelimited="true" forcerange="{_format(-p["max_torque"])} {_format(p["max_torque"])}"/>',
                '  </actuator>',
                '</mujoco>',
            )
        )
        return "\n".join(xml)

    def _metadata(self) -> dict[str, Any]:
        mass, com, inertia = _composite_inertia()
        return {
            "source": "output/navigation-lio/readiness/robot-mount/source/robot.xacro",
            "source_mesh_directory": "output/navigation-lio/readiness/robot-footprint-20261028/source/robot_description/meshes",
            "frame": "base_footprint at front drive axle midpoint",
            "drive_joint_centers_y_m": dict(_DRIVE_JOINT_Y),
            "drive_contact_centers_y_m": dict(_DRIVE_CONTACT_Y),
            "drive_contact_track_m": _DRIVE_CONTACT_Y["left"] - _DRIVE_CONTACT_Y["right"],
            "controller_nominal_track_m": self.controller_nominal_track,
            "controller_track_multiplier_source": self.controller_track_multiplier,
            "controller_effective_track_source_m": self.controller_nominal_track * self.controller_track_multiplier,
            "command_track_parameter": "nominal_track (geometric contact track default)",
            "wheel_radius_m": self.params["wheel_radius"],
            "wheel_radius_scale_left": self.params["left_radius_scale"],
            "wheel_radius_scale_right": self.params["right_radius_scale"],
            "physical_wheel_radius_left_m": self.params["wheel_radius"] * self.params["left_radius_scale"],
            "physical_wheel_radius_right_m": self.params["wheel_radius"] * self.params["right_radius_scale"],
            "chassis_sensor_mass_kg": mass,
            "chassis_sensor_com_from_axle_m": com.tolist(),
            "chassis_sensor_inertia_at_com_kg_m2": inertia.tolist(),
            "source_body_com_from_axle_m": [-0.196 + 0.0024747785463641757, -5.224869372213094e-7, 0.4359614866624879 + 0.105 - _AXLE_HEIGHT],
            "source_total_non_arm_mass_kg": 12.090086,
            "source_body_mass_kg": 11.525313075929766,
            "source_lidar_mass_kg_lumped_in_chassis": 0.41131899137404504,
            "source_base_link_mass_kg_lumped_in_chassis": 0.0014564559221618755,
            "caster_pivot_x_m": _CASTER_PIVOT_X,
            "caster_pivot_y_m": list(_CASTER_PIVOT_Y),
            "caster_source_trail_m": 0.0,
            "caster_trail_m_assumed_override": self.params["caster_trail"],
            "caster_wheel_radius_m": _CASTER_WHEEL_RADIUS,
            "drive_tire_shape": self.params["drive_tire_shape"],
            "caster_tire_shape": self.params["caster_tire_shape"],
            "rounded_tire_shape_is_assumed": self.params["drive_tire_shape"] == "ellipsoid" or self.params["caster_tire_shape"] == "ellipsoid",
            "contact_geometry_note": "Source collision meshes are retained as selectable cylinders; ellipsoid rounding is an unmeasured contact-shape assumption.",
            "chassis_collision_z_from_axle_m": [0.09, 0.62],
            "drive_servo": {
                "kind": "bounded wheel-velocity servo",
                "kv_nm_s_per_rad": self.params["drive_kv"],
                "max_torque_nm_assumed": self.params["max_torque"],
            },
            "drive_armature_kg_m2_assumed": self.params["drive_armature"],
            "drive_armature_note": "Reflected motor inertia is an explicit experiment assumption, not source-identified wheel inertia.",
            "wheel_friction_coefficient": self.params["nominal_mu"],
            "caster_friction_coefficient": self.params["caster_mu"],
            "integrator_timestep_s": self.params["physics_dt"],
            "wheel_velocity_units": "rad/s",
            "command_units": "linear_x m/s, angular_z rad/s",
            "servo_and_friction_are_experiment_parameters": True,
        }

    @staticmethod
    def _quaternion_yaw(yaw: float) -> tuple[float, float, float, float]:
        return math.cos(yaw / 2), 0.0, 0.0, math.sin(yaw / 2)

    @staticmethod
    def _rpy(quaternion: np.ndarray) -> tuple[float, float, float]:
        w, x, y, z = (float(value) for value in quaternion)
        sinr_cosp = 2.0 * (w * x + y * z)
        cosr_cosp = 1.0 - 2.0 * (x * x + y * y)
        roll = math.atan2(sinr_cosp, cosr_cosp)
        sinp = 2.0 * (w * y - z * x)
        pitch = math.copysign(math.pi / 2, sinp) if abs(sinp) >= 1 else math.asin(sinp)
        siny_cosp = 2.0 * (w * z + x * y)
        cosy_cosp = 1.0 - 2.0 * (y * y + z * z)
        return roll, pitch, math.atan2(siny_cosp, cosy_cosp)

    def reset(
        self,
        pose: tuple[float, float, float],
        caster_yaws: tuple[float, float] = (0.0, 0.0),
    ) -> None:
        if len(pose) != 3:
            raise ValueError("pose must be (x, y, yaw)")
        if len(caster_yaws) != 2:
            raise ValueError("caster_yaws must contain two yaw angles")
        x, y, yaw = (_finite_float(value, "pose value") for value in pose)
        caster_values = tuple(_finite_float(value, "caster yaw") for value in caster_yaws)
        mujoco.mj_resetData(self.model, self.data)
        root_qpos = int(self.model.jnt_qposadr[self._root_joint])
        root_qvel = int(self.model.jnt_dofadr[self._root_joint])
        self.data.qpos[root_qpos : root_qpos + 7] = (x, y, _AXLE_HEIGHT, *self._quaternion_yaw(yaw))
        self.data.qvel[root_qvel : root_qvel + 6] = 0.0
        for joint_id, value in zip(self._caster_yaw_joints, caster_values):
            self.data.qpos[int(self.model.jnt_qposadr[joint_id])] = value
        mujoco.mj_forward(self.model, self.data)
        self._obstacle_contact = False
        self._roll_tip = False
        self._pitch_tip = False
        self._max_abs_roll = 0.0
        self._max_abs_pitch = 0.0
        self._last_torques = (0.0, 0.0)
        self._target_wheels = (0.0, 0.0)
        self._sample_episode_flags()

    def step(self, command: tuple[float, float], dt: float = 0.02) -> None:
        if len(command) != 2:
            raise ValueError("command must be (linear_x_m_s, angular_z_rad_s)")
        linear, angular = (_finite_float(value, "command value") for value in command)
        duration = _finite_float(dt, "dt")
        if duration <= 0:
            raise ValueError("dt must be greater than zero")
        # Controller/encoder conversion uses its nominal radius.  Radius scales
        # alter the physical tires only, so mismatch profiles can reveal slip.
        radius_left = self.params["wheel_radius"]
        radius_right = self.params["wheel_radius"]
        track = self.params["nominal_track"]
        target_left = (linear - angular * track / 2.0) / radius_left
        target_right = (linear + angular * track / 2.0) / radius_right
        self._target_wheels = (target_left, target_right)

        elapsed = 0.0
        while elapsed < duration - 1e-12:
            substep = min(self.params["physics_dt"], duration - elapsed)
            self.model.opt.timestep = substep
            for side, target in zip(("left", "right"), self._target_wheels):
                self.data.ctrl[self._drive_actuators[side]] = target
            mujoco.mj_step(self.model, self.data)
            self._last_torques = tuple(
                float(self.data.actuator_force[self._drive_actuators[side]])
                for side in ("left", "right")
            )
            self._sample_episode_flags()
            elapsed += substep

    def _sample_episode_flags(self) -> None:
        for contact_index in range(self.data.ncon):
            contact = self.data.contact[contact_index]
            first = int(contact.geom1)
            second = int(contact.geom2)
            if (first in self._robot_geom_ids and second in self._obstacle_geom_ids) or (
                second in self._robot_geom_ids and first in self._obstacle_geom_ids
            ):
                self._obstacle_contact = True

        root_id = mujoco.mj_name2id(self.model, mujoco.mjtObj.mjOBJ_BODY, "base_footprint")
        roll, pitch, _ = self._rpy(self.data.xquat[root_id])
        self._max_abs_roll = max(self._max_abs_roll, abs(roll))
        self._max_abs_pitch = max(self._max_abs_pitch, abs(pitch))
        self._roll_tip = self._roll_tip or abs(roll) >= self.params["tip_angle"]
        self._pitch_tip = self._pitch_tip or abs(pitch) >= self.params["tip_angle"]

    def state(self) -> dict[str, Any]:
        root_id = mujoco.mj_name2id(self.model, mujoco.mjtObj.mjOBJ_BODY, "base_footprint")
        root_qpos = int(self.model.jnt_qposadr[self._root_joint])
        root_qvel = int(self.model.jnt_dofadr[self._root_joint])
        position = self.data.qpos[root_qpos : root_qpos + 3]
        quaternion = self.data.qpos[root_qpos + 3 : root_qpos + 7]
        rotation = np.empty(9)
        mujoco.mju_quat2Mat(rotation, quaternion)
        rotation = rotation.reshape((3, 3))
        linear_world = self.data.qvel[root_qvel : root_qvel + 3]
        linear_body = rotation.T @ linear_world
        # MuJoCo represents the free-joint rotational velocity in the body frame.
        angular_body = self.data.qvel[root_qvel + 3 : root_qvel + 6]
        roll, pitch, yaw = self._rpy(quaternion)
        caster_yaws = tuple(
            float(self.data.qpos[int(self.model.jnt_qposadr[joint_id])])
            for joint_id in self._caster_yaw_joints
        )
        wheel_speeds = tuple(
            float(self.data.qvel[int(self.model.jnt_dofadr[self._drive_joints[side]])])
            for side in ("left", "right")
        )
        wheel_positions = tuple(
            float(self.data.qpos[int(self.model.jnt_qposadr[self._drive_joints[side]])])
            for side in ("left", "right")
        )
        obstacle_contact_now = False
        for contact_index in range(self.data.ncon):
            contact = self.data.contact[contact_index]
            first, second = int(contact.geom1), int(contact.geom2)
            if (first in self._robot_geom_ids and second in self._obstacle_geom_ids) or (
                second in self._robot_geom_ids and first in self._obstacle_geom_ids
            ):
                obstacle_contact_now = True
                break
        return {
            "x": float(position[0]),
            "y": float(position[1]),
            "yaw": yaw,
            "linear_x": float(linear_body[0]),
            "lateral_y": float(linear_body[1]),
            "angular_z": float(angular_body[2]),
            "roll": roll,
            "pitch": pitch,
            "wheel_left": wheel_speeds[0],
            "wheel_right": wheel_speeds[1],
            "wheel_left_position": wheel_positions[0],
            "wheel_right_position": wheel_positions[1],
            "wheel_left_target": self._target_wheels[0],
            "wheel_right_target": self._target_wheels[1],
            "caster_yaws": caster_yaws,
            "torques": self._last_torques,
            "obstacle_contact": self._obstacle_contact,
            "obstacle_contact_now": obstacle_contact_now,
            "tipped": self._roll_tip or self._pitch_tip,
            "roll_tip": self._roll_tip,
            "pitch_tip": self._pitch_tip,
            "max_abs_roll": self._max_abs_roll,
            "max_abs_pitch": self._max_abs_pitch,
        }

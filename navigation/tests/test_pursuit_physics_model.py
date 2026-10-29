import math

import pytest

from navigation.scripts.pursuit_physics_model import PhysicsBase


FOOTPRINT = [
    [-0.339, -0.2105],
    [-0.33007589, -0.23082589],
    [-0.27192411, -0.23082589],
    [0.050350579, -0.225509995],
    [0.050350579, 0.225490005],
    [-0.27192411, 0.23082589],
    [-0.33007589, 0.23082589],
    [-0.339, 0.2105],
]
ROUNDED_CONTACT = {
    "drive_tire_shape": "ellipsoid",
    "caster_tire_shape": "ellipsoid",
    "caster_trail": 0.02,
}


def test_rest_state_is_finite_and_stable_without_obstacle_contacts():
    base = PhysicsBase(FOOTPRINT, [])

    for _ in range(300):
        base.step((0.0, 0.0))

    state = base.state()
    assert all(math.isfinite(value) for value in state.values() if isinstance(value, float))
    assert abs(state["linear_x"]) < 0.01
    assert abs(state["lateral_y"]) < 0.01
    assert not state["obstacle_contact"]
    assert not state["tipped"]


def test_forward_reverse_and_zero_command_stop_are_contact_driven():
    base = PhysicsBase(FOOTPRINT, [])

    base.step((0.2, 0.0), 0.7)
    forward = base.state()
    assert forward["x"] > 0.1
    assert forward["linear_x"] > 0.15
    assert forward["wheel_left"] > 0
    assert forward["wheel_right"] > 0

    base.step((0.0, 0.0), 1.0)
    stopped = base.state()
    assert abs(stopped["linear_x"]) < 0.01
    assert abs(stopped["wheel_left"]) < 0.05
    assert abs(stopped["wheel_right"]) < 0.05

    base.reset((0.0, 0.0, 0.0))
    base.step((-0.2, 0.0), 0.7)
    reverse = base.state()
    assert reverse["x"] < -0.1
    assert reverse["linear_x"] < -0.15
    assert reverse["wheel_left"] < 0
    assert reverse["wheel_right"] < 0


def test_positive_angular_command_turns_left_with_explicit_contact_assumptions():
    base = PhysicsBase(FOOTPRINT, [], ROUNDED_CONTACT)
    base.step((0.0, 0.5), 1.0)

    state = base.state()
    assert state["yaw"] > 0.2
    assert state["angular_z"] > 0.3
    assert base.metadata["rounded_tire_shape_is_assumed"]
    assert base.metadata["caster_trail_m_assumed_override"] == pytest.approx(0.02)


def test_motor_torques_obey_configured_bound():
    base = PhysicsBase(FOOTPRINT, [], {**ROUNDED_CONTACT, "max_torque": 0.3})
    base.step((1.0, 5.0), 0.02)

    torques = base.state()["torques"]
    assert max(abs(value) for value in torques) <= 0.3 + 1e-9
    assert max(abs(value) for value in torques) == pytest.approx(0.3)


def test_lower_drive_friction_changes_physical_motion():
    normal = PhysicsBase(FOOTPRINT, [], ROUNDED_CONTACT)
    slippery = PhysicsBase(FOOTPRINT, [], {**ROUNDED_CONTACT, "nominal_mu": 0.05})

    normal.step((0.2, 0.0), 1.0)
    slippery.step((0.2, 0.0), 1.0)

    assert slippery.state()["x"] < normal.state()["x"] - 0.05


def test_radius_mismatch_is_not_compensated_by_command_conversion():
    nominal = PhysicsBase(FOOTPRINT, [], ROUNDED_CONTACT)
    mismatched = PhysicsBase(
        FOOTPRINT,
        [],
        {**ROUNDED_CONTACT, "left_radius_scale": 0.9},
    )

    nominal.step((0.2, 0.0), 1.0)
    mismatched.step((0.2, 0.0), 1.0)

    assert mismatched.state()["wheel_left_target"] == pytest.approx(
        nominal.state()["wheel_left_target"]
    )
    assert abs(mismatched.state()["yaw"]) > 0.1
    assert mismatched.metadata["physical_wheel_radius_left_m"] == pytest.approx(
        0.05035 * 0.9
    )


def test_obstacle_contact_is_classified_separately_from_ground_contact():
    clear = PhysicsBase(FOOTPRINT, [], ROUNDED_CONTACT)
    clear.step((0.0, 0.0), 0.1)
    assert not clear.state()["obstacle_contact"]

    blocked = PhysicsBase(FOOTPRINT, [(0.2, -0.4, 0.25, 0.4)], ROUNDED_CONTACT)
    blocked.step((0.2, 0.0), 2.0)
    assert blocked.state()["obstacle_contact"]
    assert blocked.state()["obstacle_contact_now"]
    assert blocked.state()["x"] < 0.2


def test_contact_friction_priority_preserves_tire_parameters():
    base = PhysicsBase(
        FOOTPRINT,
        [],
        {"nominal_mu": 0.7, "caster_mu": 0.15},
    )
    base.step((0.0, 0.0), 0.1)
    drive = []
    caster = []
    for index in range(base.data.ncon):
        contact = base.data.contact[index]
        names = {base.model.geom(contact.geom1).name, base.model.geom(contact.geom2).name}
        if any(name.startswith("drive_tire_") for name in names):
            drive.append(float(contact.friction[0]))
        if any(name.startswith("caster_tire_") for name in names):
            caster.append(float(contact.friction[0]))

    assert drive and all(value == pytest.approx(0.7) for value in drive)
    assert caster and all(value == pytest.approx(0.15) for value in caster)


def test_state_velocities_use_base_frame_at_nonzero_yaw():
    base = PhysicsBase(FOOTPRINT, [])
    base.reset((0.0, 0.0, math.pi / 2))
    root_velocity = int(base.model.jnt_dofadr[base._root_joint])
    base.data.qvel[root_velocity : root_velocity + 6] = (0.0, 1.0, 0.0, 0.0, 0.0, 0.7)

    state = base.state()
    assert state["linear_x"] == pytest.approx(1.0)
    assert state["lateral_y"] == pytest.approx(0.0, abs=1e-12)
    assert state["angular_z"] == pytest.approx(0.7)


def test_source_track_and_contact_metadata_are_explicit():
    source = PhysicsBase(FOOTPRINT, [])
    assert source.metadata["controller_nominal_track_m"] == pytest.approx(0.320)
    assert source.metadata["controller_track_multiplier_source"] == pytest.approx(3.9)
    assert source.metadata["drive_contact_track_m"] == pytest.approx(0.3855)
    assert source.metadata["drive_joint_centers_y_m"]["left"] == pytest.approx(0.15999)
    assert source.metadata["drive_contact_centers_y_m"]["left"] == pytest.approx(0.19274)
    assert source.metadata["drive_tire_shape"] == "cylinder"
    assert source.metadata["caster_source_trail_m"] == 0.0
    assert source.metadata["drive_armature_kg_m2_assumed"] == pytest.approx(0.00001)
    assert "explicit experiment assumption" in source.metadata["drive_armature_note"]


def test_tire_shape_and_trail_parameters_are_validated():
    with pytest.raises(ValueError, match="drive_tire_shape"):
        PhysicsBase(FOOTPRINT, [], {"drive_tire_shape": "mesh"})
    with pytest.raises(ValueError, match="caster_trail"):
        PhysicsBase(FOOTPRINT, [], {"caster_trail": -0.01})
    with pytest.raises(ValueError, match="drive_armature"):
        PhysicsBase(FOOTPRINT, [], {"drive_armature": -0.001})


def test_reflected_armature_stabilizes_forward_wheels_and_step_converges_with_radius_wear():
    profile = {
        **ROUNDED_CONTACT,
        "drive_armature": 0.001,
        "left_radius_scale": 0.9,
    }
    bases = [
        PhysicsBase(FOOTPRINT, [], {**profile, "physics_dt": physics_dt})
        for physics_dt in (0.002, 0.001)
    ]
    minimum_wheels = [float("inf"), float("inf")]
    maximum_torque = 0.0

    # Sample at each physical step so servo chatter cannot hide between control
    # interval snapshots.  The asymmetric radius profile is expected to yaw.
    for _ in range(200):
        for base_index, base in enumerate(bases):
            step_dt = 0.002 if base_index == 0 else 0.001
            for _ in range(round(0.02 / step_dt)):
                base.step((0.2, 0.0), step_dt)
                state = base.state()
                minimum_wheels[0] = min(minimum_wheels[0], state["wheel_left"])
                minimum_wheels[1] = min(minimum_wheels[1], state["wheel_right"])
                maximum_torque = max(maximum_torque, *(abs(value) for value in state["torques"]))
                assert max(abs(state["wheel_left"]), abs(state["wheel_right"])) < 5.0

    coarse, fine = (base.state() for base in bases)
    position_difference = math.hypot(coarse["x"] - fine["x"], coarse["y"] - fine["y"])
    yaw_difference = abs(coarse["yaw"] - fine["yaw"])
    assert min(minimum_wheels) > 0.0
    assert maximum_torque <= 0.6 + 1e-9
    assert abs(coarse["yaw"]) > 0.1
    assert abs(fine["yaw"]) > 0.1
    assert position_difference < 0.003
    assert yaw_difference < 0.01
    assert abs(coarse["wheel_left"] - fine["wheel_left"]) < 0.02
    assert abs(coarse["wheel_right"] - fine["wheel_right"]) < 0.02


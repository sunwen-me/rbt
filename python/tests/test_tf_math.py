from __future__ import annotations

import math
import sys

import pytest

from rbt.tf_math import FrameId, Pose3, Quaternion, TfError, Transform3, Vec3


def _close_vec(actual: Vec3, expected: Vec3) -> None:
    assert actual.x == pytest.approx(expected.x, abs=1.0e-9)
    assert actual.y == pytest.approx(expected.y, abs=1.0e-9)
    assert actual.z == pytest.approx(expected.z, abs=1.0e-9)


def _quat_z(degrees: float) -> Quaternion:
    half_angle = math.radians(degrees) / 2.0
    return Quaternion(0.0, 0.0, math.sin(half_angle), math.cos(half_angle))


def _assert_tf_code(code: str, operation: object) -> None:
    with pytest.raises(TfError) as raised:
        operation()  # type: ignore[operator]
    assert raised.value.code == code


def test_frame_id_canonical_fixed64_bytes_and_name_rules() -> None:
    frame = FrameId("robot/base_link")
    assert str(frame) == "robot/base_link"
    assert len(frame.to_bytes()) == 64
    assert frame.to_bytes() == b"robot/base_link" + bytes(49)
    assert FrameId.from_bytes(frame.to_bytes()) == frame
    assert FrameId("Robot/base_link") != frame
    assert FrameId("a" * 63).to_bytes() == b"a" * 63 + b"\0"

    for invalid in (
        "",
        "/base",
        "base/",
        "base//link",
        ".",
        "..",
        "base/.",
        "base/..",
        "-base",
        "base link",
        "base\0link",
        "base/é",
        "a" * 64,
    ):
        _assert_tf_code("invalid_frame_id", lambda value=invalid: FrameId(value))

    _assert_tf_code("invalid_frame_id", lambda: FrameId.from_bytes(b"short"))
    _assert_tf_code("invalid_frame_id", lambda: FrameId.from_bytes(bytes(64)))
    malformed_padding = bytearray(frame.to_bytes())
    malformed_padding[-1] = ord("x")
    _assert_tf_code("invalid_frame_id", lambda: FrameId.from_bytes(malformed_padding))
    non_ascii = bytearray(64)
    non_ascii[:2] = b"a\xff"
    _assert_tf_code("invalid_frame_id", lambda: FrameId.from_bytes(non_ascii))


def test_numeric_objects_are_finite_normalized_and_immutable() -> None:
    vector = Vec3(1, -2, 3.5)
    assert vector == Vec3(1.0, -2.0, 3.5)
    with pytest.raises((AttributeError, TypeError)):
        vector.x = 3.0  # type: ignore[misc]
    _assert_tf_code("invalid_transform", lambda: Vec3(float("nan"), 0.0, 0.0))
    _assert_tf_code("invalid_transform", lambda: Vec3(True, 0.0, 0.0))
    _assert_tf_code("invalid_transform", lambda: Vec3(10**400, 0.0, 0.0))

    corrected = Quaternion(0.0, 0.0, 0.0, 1.0 + 5.0e-7)
    assert (corrected.x, corrected.y, corrected.z, corrected.w) == (0.0, 0.0, 0.0, 1.0)
    _assert_tf_code("invalid_quaternion", lambda: Quaternion(0.0, 0.0, 0.0, 0.0))
    _assert_tf_code("invalid_quaternion", lambda: Quaternion(0.0, 0.0, 0.0, 1.000002))
    _assert_tf_code("invalid_quaternion", lambda: Quaternion(0.0, 0.0, float("inf"), 1.0))
    _assert_tf_code("invalid_transform", lambda: Transform3(vector, object()))
    _assert_tf_code("invalid_transform", lambda: Pose3(vector, object()))


def test_directional_composition_inverse_points_vectors_and_poses() -> None:
    target_from_middle = Transform3(Vec3(1.0, 0.0, 0.0), _quat_z(90.0))
    middle_from_source = Transform3(Vec3(1.0, 0.0, 0.0), Quaternion.identity())
    target_from_source = target_from_middle.compose(middle_from_source)
    _close_vec(target_from_source.translation, Vec3(1.0, 1.0, 0.0))

    transform = Transform3(Vec3(3.0, 4.0, 5.0), _quat_z(90.0))
    _close_vec(transform.apply_vector(Vec3(1.0, 0.0, 0.0)), Vec3(0.0, 1.0, 0.0))
    _close_vec(transform.apply_point(Vec3(1.0, 0.0, 0.0)), Vec3(3.0, 5.0, 5.0))
    pose_result = transform.apply_pose(Pose3(Vec3(1.0, 0.0, 0.0), Quaternion.identity()))
    _close_vec(pose_result.position, Vec3(3.0, 5.0, 5.0))
    _close_vec(
        Transform3(Vec3(0.0, 0.0, 0.0), pose_result.orientation).apply_vector(Vec3(1.0, 0.0, 0.0)),
        Vec3(0.0, 1.0, 0.0),
    )

    sample = Vec3(-2.0, 4.0, 1.5)
    _close_vec(transform.inverse().apply_point(transform.apply_point(sample)), sample)
    identity = transform.compose(transform.inverse())
    _close_vec(identity.apply_vector(sample), sample)


def test_interpolation_uses_shortest_path_and_stable_unit_slerp() -> None:
    identity = Transform3.identity()
    halfway = identity.interpolate(Transform3(Vec3(2.0, 4.0, 6.0), _quat_z(90.0)), 0.5)
    _close_vec(halfway.translation, Vec3(1.0, 2.0, 3.0))
    _close_vec(halfway.apply_vector(Vec3(1.0, 0.0, 0.0)), Vec3(math.sqrt(0.5), math.sqrt(0.5), 0.0))

    negated_identity = Quaternion(0.0, 0.0, 0.0, -1.0)
    same_rotation = identity.interpolate(Transform3(Vec3(0.0, 0.0, 0.0), negated_identity), 0.5)
    _close_vec(same_rotation.apply_vector(Vec3(1.0, 0.0, 0.0)), Vec3(1.0, 0.0, 0.0))

    for dot in (0.9996, 0.9995):
        angle = 2.0 * math.acos(dot)
        actual = identity.interpolate(Transform3(Vec3(0.0, 0.0, 0.0), _quat_z(math.degrees(angle))), 0.5)
        _close_vec(
            actual.apply_vector(Vec3(1.0, 0.0, 0.0)),
            Vec3(math.cos(angle / 2.0), math.sin(angle / 2.0), 0.0),
        )

    near_half_turn = Transform3(Vec3(0.0, 0.0, 0.0), _quat_z(180.0 - 1.0e-6)).interpolate(
        Transform3(Vec3(0.0, 0.0, 0.0), _quat_z(180.0 + 1.0e-6)), 0.5
    )
    _close_vec(near_half_turn.apply_vector(Vec3(1.0, 0.0, 0.0)), Vec3(-1.0, 0.0, 0.0))
    for invalid in (float("nan"), float("inf"), -0.1, 1.1):
        _assert_tf_code("invalid_interpolation_fraction", lambda amount=invalid: identity.interpolate(identity, amount))


def test_large_finite_values_and_arithmetic_overflow() -> None:
    largest = sys.float_info.max
    translation = Transform3(Vec3(largest, 0.0, 0.0), Quaternion.identity())
    assert translation.apply_point(Vec3(0.0, 0.0, 0.0)).x == largest
    _assert_tf_code("numeric_overflow", lambda: translation.compose(translation))
    _assert_tf_code("numeric_overflow", lambda: translation.apply_point(Vec3(largest, 0.0, 0.0)))

    cancelled = translation.compose(translation.inverse())
    _close_vec(cancelled.translation, Vec3(0.0, 0.0, 0.0))
    opposite = Transform3(Vec3(-largest, 0.0, 0.0), Quaternion.identity())
    midpoint = translation.interpolate(opposite, 0.5)
    _close_vec(midpoint.translation, Vec3(0.0, 0.0, 0.0))

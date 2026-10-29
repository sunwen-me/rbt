"""Validated pure-Python coordinate frames and rigid-transform math.

Transforms use ``T_target<-source`` direction: rotation is applied first,
then translation. Coordinates are right handed, in metres and radians, and
quaternion components are always in x, y, z, w order.
"""

from __future__ import annotations

import math
from dataclasses import dataclass
from numbers import Real

_FRAME_SIZE = 64
_QUATERNION_TOLERANCE = 1.0e-6
_SLERP_LINEAR_THRESHOLD = 0.9995


class TfError(Exception):
    """A frame or transform operation failed with a stable error code."""

    def __init__(self, code: str, message: str = "") -> None:
        self.code = code
        super().__init__(message or code)


def _fail(code: str, message: str) -> TfError:
    return TfError(code, message)


def _finite_float(value: object, *, code: str, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, Real):
        raise _fail(code, f"{name} must be a real number")
    try:
        result = float(value)
    except (OverflowError, TypeError, ValueError) as error:
        raise _fail(code, f"{name} must be a finite f64 value") from error
    if not math.isfinite(result):
        raise _fail(code, f"{name} must be a finite f64 value")
    return result


def _checked_result(value: float, *, operation: str) -> float:
    if not math.isfinite(value):
        raise _fail("numeric_overflow", f"{operation} produced a non-finite value")
    return value


def _validate_frame_name(value: object) -> str:
    if not isinstance(value, str):
        raise _fail("invalid_frame_id", "frame id must be a string")
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as error:
        raise _fail("invalid_frame_id", "frame id must contain ASCII bytes only") from error
    if not 1 <= len(encoded) <= _FRAME_SIZE - 1:
        raise _fail("invalid_frame_id", "frame id must contain 1 to 63 bytes")

    for segment in value.split("/"):
        if not segment or segment in (".", ".."):
            raise _fail("invalid_frame_id", "frame id contains an invalid path segment")
        first = segment[0]
        if not (first.isascii() and (first.isalnum() or first == "_")):
            raise _fail("invalid_frame_id", "frame segments must start with an ASCII letter, digit, or underscore")
        if any(
            not (character.isascii() and (character.isalnum() or character in "_.-"))
            for character in segment[1:]
        ):
            raise _fail("invalid_frame_id", "frame segment contains an invalid character")
    if "\x00" in value:
        raise _fail("invalid_frame_id", "frame id cannot contain NUL")
    return value


@dataclass(frozen=True, slots=True)
class FrameId:
    """A case-sensitive frame name with a canonical 64-byte encoding."""

    value: str

    def __post_init__(self) -> None:
        object.__setattr__(self, "value", _validate_frame_name(self.value))

    @classmethod
    def from_bytes(cls, value: bytes | bytearray | memoryview) -> FrameId:
        if not isinstance(value, (bytes, bytearray, memoryview)):
            raise _fail("invalid_frame_id", "frame encoding must be bytes")
        encoded = bytes(value)
        if len(encoded) != _FRAME_SIZE:
            raise _fail("invalid_frame_id", "frame encoding must contain exactly 64 bytes")
        terminator = encoded.find(b"\0")
        if terminator < 0 or any(encoded[terminator:]):
            raise _fail("invalid_frame_id", "frame encoding must use canonical zero padding")
        try:
            name = encoded[:terminator].decode("ascii")
        except UnicodeDecodeError as error:
            raise _fail("invalid_frame_id", "frame id must contain ASCII bytes only") from error
        return cls(name)

    def to_bytes(self) -> bytes:
        encoded = self.value.encode("ascii")
        return encoded + bytes(_FRAME_SIZE - len(encoded))

    def __str__(self) -> str:
        return self.value


@dataclass(frozen=True, slots=True)
class Vec3:
    """A finite three-dimensional vector with public x, y, and z attributes."""

    x: float
    y: float
    z: float

    def __post_init__(self) -> None:
        object.__setattr__(self, "x", _finite_float(self.x, code="invalid_transform", name="x"))
        object.__setattr__(self, "y", _finite_float(self.y, code="invalid_transform", name="y"))
        object.__setattr__(self, "z", _finite_float(self.z, code="invalid_transform", name="z"))


def _unit_quaternion(
    values: tuple[float, float, float, float],
    *,
    error_code: str = "invalid_quaternion",
) -> Quaternion:
    if any(not math.isfinite(value) for value in values):
        raise _fail(error_code, "quaternion components must be finite")
    norm = math.hypot(*values)
    if not math.isfinite(norm) or norm == 0.0:
        raise _fail(error_code, "quaternion norm must be finite and nonzero")
    normalized = tuple(value / norm for value in values)
    result = object.__new__(Quaternion)
    for attribute, value in zip(("x", "y", "z", "w"), normalized):
        object.__setattr__(result, attribute, value)
    return result


@dataclass(frozen=True, slots=True)
class Quaternion:
    """A normalized quaternion in x, y, z, w component order."""

    x: float
    y: float
    z: float
    w: float

    def __post_init__(self) -> None:
        values = tuple(
            _finite_float(value, code="invalid_quaternion", name=name)
            for value, name in zip((self.x, self.y, self.z, self.w), ("x", "y", "z", "w"))
        )
        norm = math.hypot(*values)
        if norm == 0.0 or abs(norm - 1.0) > _QUATERNION_TOLERANCE:
            raise _fail("invalid_quaternion", "quaternion norm must be within 1e-6 of one")
        for attribute, value in zip(("x", "y", "z", "w"), (component / norm for component in values)):
            object.__setattr__(self, attribute, value)

    @classmethod
    def identity(cls) -> Quaternion:
        return cls(0.0, 0.0, 0.0, 1.0)

    def _conjugate(self) -> Quaternion:
        return _unit_quaternion((-self.x, -self.y, -self.z, self.w))

    def _multiply(self, other: Quaternion) -> Quaternion:
        return _unit_quaternion(
            (
                self.w * other.x + self.x * other.w + self.y * other.z - self.z * other.y,
                self.w * other.y - self.x * other.z + self.y * other.w + self.z * other.x,
                self.w * other.z + self.x * other.y - self.y * other.x + self.z * other.w,
                self.w * other.w - self.x * other.x - self.y * other.y - self.z * other.z,
            )
        )

    def _slerp(self, other: Quaternion, amount: float) -> Quaternion:
        if amount == 0.0:
            return self
        if amount == 1.0:
            return other

        rhs = other
        dot = self.x * rhs.x + self.y * rhs.y + self.z * rhs.z + self.w * rhs.w
        if dot < 0.0:
            rhs = _unit_quaternion((-rhs.x, -rhs.y, -rhs.z, -rhs.w))
            dot = -dot
        dot = min(1.0, max(-1.0, dot))

        if dot > _SLERP_LINEAR_THRESHOLD:
            return _unit_quaternion(
                tuple(
                    _interpolate_scalar(left, right, amount)
                    for left, right in zip((self.x, self.y, self.z, self.w), (rhs.x, rhs.y, rhs.z, rhs.w))
                )
            )

        theta = math.acos(dot)
        sin_theta = math.sin(theta)
        if sin_theta == 0.0 or not math.isfinite(sin_theta):
            raise _fail("invalid_quaternion", "quaternion interpolation is numerically unstable")
        lhs_weight = math.sin((1.0 - amount) * theta) / sin_theta
        rhs_weight = math.sin(amount * theta) / sin_theta
        return _unit_quaternion(
            (
                self.x * lhs_weight + rhs.x * rhs_weight,
                self.y * lhs_weight + rhs.y * rhs_weight,
                self.z * lhs_weight + rhs.z * rhs_weight,
                self.w * lhs_weight + rhs.w * rhs_weight,
            )
        )

    def _rotation_matrix(self) -> tuple[tuple[float, float, float], ...]:
        x, y, z, w = self.x, self.y, self.z, self.w
        rows = (
            (1.0 - 2.0 * (y * y + z * z), 2.0 * (x * y - z * w), 2.0 * (x * z + y * w)),
            (2.0 * (x * y + z * w), 1.0 - 2.0 * (x * x + z * z), 2.0 * (y * z - x * w)),
            (2.0 * (x * z - y * w), 2.0 * (y * z + x * w), 1.0 - 2.0 * (x * x + y * y)),
        )
        # Roundoff may put an orthogonal-matrix entry a few ulps beyond ±1.
        return tuple(tuple(min(1.0, max(-1.0, entry)) for entry in row) for row in rows)


def _scaled_linear_combination(
    coefficients: tuple[float, float, float],
    values: tuple[float, float, float],
    offset: float,
) -> float:
    scale = max(abs(values[0]), abs(values[1]), abs(values[2]), abs(offset))
    if scale == 0.0:
        return 0.0
    normalized = math.fsum(
        (
            coefficients[0] * (values[0] / scale),
            coefficients[1] * (values[1] / scale),
            coefficients[2] * (values[2] / scale),
            offset / scale,
        )
    )
    return _checked_result(scale * normalized, operation="transform arithmetic")


def _rotate_values(rotation: Quaternion, vector: Vec3, offset: Vec3 | None = None) -> Vec3:
    matrix = rotation._rotation_matrix()
    values = (vector.x, vector.y, vector.z)
    translation = offset or Vec3(0.0, 0.0, 0.0)
    offsets = (translation.x, translation.y, translation.z)
    return Vec3(
        *(
            _scaled_linear_combination(row, values, component_offset)
            for row, component_offset in zip(matrix, offsets)
        )
    )


def _interpolate_scalar(left: float, right: float, amount: float) -> float:
    if amount == 0.0:
        return left
    if amount == 1.0:
        return right
    return _scaled_linear_combination((1.0 - amount, amount, 0.0), (left, right, 0.0), 0.0)


@dataclass(frozen=True, slots=True)
class Transform3:
    """A rigid transform ``T_target<-source`` with translation and rotation."""

    translation: Vec3
    rotation: Quaternion

    def __post_init__(self) -> None:
        if not isinstance(self.translation, Vec3) or not isinstance(self.rotation, Quaternion):
            raise _fail("invalid_transform", "transform requires a Vec3 translation and Quaternion rotation")

    @classmethod
    def identity(cls) -> Transform3:
        return cls(Vec3(0.0, 0.0, 0.0), Quaternion.identity())

    def inverse(self) -> Transform3:
        inverse_rotation = self.rotation._conjugate()
        negated = Vec3(-self.translation.x, -self.translation.y, -self.translation.z)
        return Transform3(_rotate_values(inverse_rotation, negated), inverse_rotation)

    def compose(self, other: Transform3) -> Transform3:
        if not isinstance(other, Transform3):
            raise _fail("invalid_transform", "transform composition requires another Transform3")
        translation = _rotate_values(self.rotation, other.translation, self.translation)
        return Transform3(translation, self.rotation._multiply(other.rotation))

    def apply_point(self, point: Vec3) -> Vec3:
        if not isinstance(point, Vec3):
            raise _fail("invalid_transform", "point must be a Vec3")
        return _rotate_values(self.rotation, point, self.translation)

    def apply_vector(self, vector: Vec3) -> Vec3:
        if not isinstance(vector, Vec3):
            raise _fail("invalid_transform", "vector must be a Vec3")
        return _rotate_values(self.rotation, vector)

    def apply_pose(self, pose: Pose3) -> Pose3:
        if not isinstance(pose, Pose3):
            raise _fail("invalid_transform", "pose must be a Pose3")
        return Pose3(self.apply_point(pose.position), self.rotation._multiply(pose.orientation))

    def interpolate(self, other: Transform3, amount: float) -> Transform3:
        if not isinstance(other, Transform3):
            raise _fail("invalid_transform", "transform interpolation requires another Transform3")
        fraction = _finite_float(amount, code="invalid_interpolation_fraction", name="amount")
        if not 0.0 <= fraction <= 1.0:
            raise _fail("invalid_interpolation_fraction", "amount must be between zero and one")
        return Transform3(
            Vec3(
                _interpolate_scalar(self.translation.x, other.translation.x, fraction),
                _interpolate_scalar(self.translation.y, other.translation.y, fraction),
                _interpolate_scalar(self.translation.z, other.translation.z, fraction),
            ),
            self.rotation._slerp(other.rotation, fraction),
        )


@dataclass(frozen=True, slots=True)
class Pose3:
    """A rigid pose containing a position and orientation."""

    position: Vec3
    orientation: Quaternion

    def __post_init__(self) -> None:
        if not isinstance(self.position, Vec3) or not isinstance(self.orientation, Quaternion):
            raise _fail("invalid_transform", "pose requires a Vec3 position and Quaternion orientation")

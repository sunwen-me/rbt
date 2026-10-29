"""Zero-copy views for the Orbbec ``RBTORB01`` StampedBytes application frame.

The returned NumPy arrays borrow the caller's read-only sample buffer. Keep the
sample (or an array derived from its payload view) alive while reading them.
Point frames preserve XYZ values, including invalid/non-finite points; filter
valid rows with ``numpy.isfinite(points).all(axis=1)`` when needed.
"""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass, field
from typing import Any

import numpy as np

from rbt.tf_math import FrameId, TfError


FRAME_HEADER_BYTES = 128
MAXIMUM_FRAME_BYTES = 32 * 1024 * 1024
_MAGIC = b"RBTORB01"
_MAXIMUM_DIMENSION = 4096

_KINDS = {
    1: "color",
    2: "depth",
    3: "left-ir",
    4: "right-ir",
    5: "points",
    6: "accel",
    7: "gyro",
}
_FORMATS = {
    1: "rgb8",
    2: "mono8",
    3: "mono16",
    4: "depth16",
    5: "xyz32f",
    6: "accel32f",
    7: "gyro32f",
}
_FORMATS_BY_KIND = {
    1: {1},
    2: {4},
    3: {2, 3},
    4: {2, 3},
    5: {5},
    6: {6},
    7: {7},
}
_BYTES_PER_PIXEL = {1: 3, 2: 1, 3: 2, 4: 2, 5: 12, 6: 12, 7: 12}


@dataclass(frozen=True, slots=True)
class OrbbecFrame:
    """Validated header plus a read-only array borrowing one sample payload."""

    kind_id: int
    kind: str
    format_id: int
    format: str
    width: int
    height: int
    stride: int
    frame_index: int
    unit_scale: float
    frame_id: str
    pixels: np.ndarray[Any, Any]
    _application: memoryview = field(repr=False, compare=False)

    @property
    def shape(self) -> tuple[int, ...]:
        return self.pixels.shape

    @property
    def depth_unit_m(self) -> float | None:
        """Metres represented by one raw integer unit, for depth frames only."""
        return self.unit_scale if self.kind_id == 2 else None


def parse_frame(application: bytes | bytearray | memoryview) -> OrbbecFrame:
    """Validate and view one RBTORB01 application payload without copying pixels."""
    try:
        view = memoryview(application).cast("B").toreadonly()
    except (TypeError, ValueError) as error:
        raise TypeError("application payload must be a contiguous bytes-like value") from error

    if view.nbytes < FRAME_HEADER_BYTES:
        raise ValueError("frame envelope is shorter than its 128-byte header")
    if view.nbytes > MAXIMUM_FRAME_BYTES:
        raise ValueError("frame envelope exceeds the 32 MiB limit")
    if bytes(view[:8]) != _MAGIC:
        raise ValueError("frame magic is invalid")
    if any(view[28:32]) or any(view[56:64]):
        raise ValueError("reserved frame header bytes must be zero")

    kind_id, format_id, width, height, stride = struct.unpack_from("<IIIII", view, 8)
    try:
        kind = _KINDS[kind_id]
    except KeyError as error:
        raise ValueError(f"frame kind {kind_id} is invalid") from error
    try:
        pixel_format = _FORMATS[format_id]
    except KeyError as error:
        raise ValueError(f"frame format {format_id} is invalid") from error
    if format_id not in _FORMATS_BY_KIND[kind_id]:
        raise ValueError(f"frame kind {kind} and format {pixel_format} do not match")

    if not 1 <= width <= _MAXIMUM_DIMENSION or not 1 <= height <= _MAXIMUM_DIMENSION:
        raise ValueError("frame dimensions must be between 1 and 4096")
    frame_index = struct.unpack_from("<Q", view, 32)[0]
    unit_scale = struct.unpack_from("<d", view, 40)[0]
    if not math.isfinite(unit_scale):
        raise ValueError("frame unit scale must be finite")
    if kind_id == 2:
        if unit_scale <= 0.0:
            raise ValueError("depth unit scale must be positive")
    elif unit_scale != 1.0:
        raise ValueError("non-depth frame unit scale must equal 1.0")

    data_size = struct.unpack_from("<Q", view, 48)[0]
    maximum_data_bytes = MAXIMUM_FRAME_BYTES - FRAME_HEADER_BYTES
    if data_size > maximum_data_bytes:
        raise ValueError("frame payload exceeds the 32 MiB limit")
    if data_size != view.nbytes - FRAME_HEADER_BYTES:
        raise ValueError("frame payload length does not match the envelope")

    minimum_row_bytes = width * _BYTES_PER_PIXEL[format_id]
    if kind_id == 5:
        if stride != minimum_row_bytes:
            raise ValueError("point frame stride must exactly match XYZ row size")
    elif kind_id in (6, 7):
        if width != 1 or height != 1 or stride != 12:
            raise ValueError("IMU frames must contain one three-float sample")
    elif stride < minimum_row_bytes:
        raise ValueError("frame stride is shorter than a row")

    expected_bytes = stride * height
    if expected_bytes > maximum_data_bytes:
        raise ValueError("frame byte size exceeds the 32 MiB limit")
    if expected_bytes != data_size:
        raise ValueError("frame payload size does not match dimensions and stride")

    try:
        frame_id = FrameId.from_bytes(view[64:128]).value
    except TfError as error:
        raise ValueError(f"frame id is invalid: {error}") from error
    if kind_id in (6, 7):
        imu_values = struct.unpack_from("<3f", view, FRAME_HEADER_BYTES)
        if not all(math.isfinite(value) for value in imu_values):
            raise ValueError("IMU samples must contain only finite floats")

    dtype, shape, strides = _array_layout(
        kind_id, format_id, width, height, stride
    )
    pixels = np.ndarray(
        shape=shape,
        dtype=dtype,
        buffer=view,
        offset=FRAME_HEADER_BYTES,
        strides=strides,
    )
    return OrbbecFrame(
        kind_id=kind_id,
        kind=kind,
        format_id=format_id,
        format=pixel_format,
        width=width,
        height=height,
        stride=stride,
        frame_index=frame_index,
        unit_scale=unit_scale,
        frame_id=frame_id,
        pixels=pixels,
        _application=view,
    )


def _array_layout(
    kind_id: int, format_id: int, width: int, height: int, stride: int
) -> tuple[np.dtype[Any], tuple[int, ...], tuple[int, ...]]:
    if format_id == 1:
        return np.dtype("u1"), (height, width, 3), (stride, 3, 1)
    if format_id == 2:
        return np.dtype("u1"), (height, width), (stride, 1)
    if format_id in (3, 4):
        return np.dtype("<u2"), (height, width), (stride, 2)
    if kind_id == 5:
        return np.dtype("<f4"), (width * height, 3), (12, 4)
    return np.dtype("<f4"), (3,), (4,)


def depth_metres(frame: OrbbecFrame) -> np.ndarray[Any, Any]:
    """Return an explicit float32 copy of depth pixels scaled into metres."""
    if frame.kind_id != 2:
        raise ValueError("depth_metres requires a depth frame")
    return np.multiply(frame.pixels, np.float32(frame.unit_scale), dtype=np.float32)


__all__ = ["FRAME_HEADER_BYTES", "MAXIMUM_FRAME_BYTES", "OrbbecFrame", "depth_metres", "parse_frame"]

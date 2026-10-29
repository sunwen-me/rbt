from __future__ import annotations

import sys

import numpy as np
import pytest

import rbt


class _ImageOwner(bytes):
    def __new__(
        cls,
        payload_size: int,
        *,
        width: int,
        height: int,
        stride: int,
        encoding: int,
    ) -> _ImageOwner:
        owner = super().__new__(cls, payload_size)
        owner.sequence = 7
        owner.timestamp_ns = 11
        owner.width = width
        owner.height = height
        owner.stride = stride
        owner.encoding = encoding
        owner.payload_nbytes = payload_size
        return owner

    def _debug_payload_address(self) -> int:
        return 4


class _PointCloudOwner(bytes):
    def __new__(
        cls,
        payload_size: int,
        *,
        width: int,
        height: int,
        count: int,
        address: int = 4,
    ) -> _PointCloudOwner:
        owner = super().__new__(cls, payload_size)
        owner.sequence = 7
        owner.timestamp_ns = 11
        owner.width = width
        owner.height = height
        owner.dense = True
        owner.point_count = count
        owner.payload_nbytes = payload_size
        owner.payload_alignment = 4
        owner._address = address
        return owner

    def _debug_payload_address(self) -> int:
        return self._address


@pytest.mark.parametrize(
    ("encoding", "width", "height", "stride", "payload_size", "shape", "strides"),
    (
        (rbt.PixelFormat.RGB8, 3, 2, 12, 24, (2, 3, 3), (12, 3, 1)),
        (rbt.PixelFormat.BGR8, 3, 2, 12, 24, (2, 3, 3), (12, 3, 1)),
        (rbt.PixelFormat.GRAY8, 3, 2, 4, 8, (2, 3), (4, 1)),
        (rbt.PixelFormat.RGB8, 3, 0, 12, 0, (0, 3, 3), (12, 3, 1)),
        (rbt.PixelFormat.GRAY8, 0, 2, 4, 8, (2, 0), (4, 1)),
    ),
)
def test_image_format_mapping_and_zero_dimensions_are_bounds_checked(
    encoding: rbt.PixelFormat,
    width: int,
    height: int,
    stride: int,
    payload_size: int,
    shape: tuple[int, ...],
    strides: tuple[int, ...],
) -> None:
    owner = _ImageOwner(
        payload_size,
        width=width,
        height=height,
        stride=stride,
        encoding=int(encoding),
    )
    image = rbt.ReceivedImageSample(owner).numpy()
    assert image.shape == shape
    assert image.strides == strides
    assert image.dtype == np.uint8
    assert not image.flags.owndata
    assert not image.flags.writeable


@pytest.mark.parametrize(
    ("owner", "error", "message"),
    (
        (
            _ImageOwner(24, width=4, height=2, stride=12, encoding=999),
            rbt.UnsupportedPixelFormatError,
            "pixel format value 999",
        ),
        (
            _ImageOwner(24, width=4, height=2, stride=11, encoding=1),
            rbt.InvalidImageLayoutError,
            "smaller than row width",
        ),
        (
            _ImageOwner(23, width=4, height=2, stride=12, encoding=1),
            rbt.InvalidImageLayoutError,
            "requires 24 payload bytes",
        ),
        (
            _ImageOwner(0, width=sys.maxsize, height=2, stride=0, encoding=1),
            rbt.InvalidImageLayoutError,
            "exceed platform address space",
        ),
    ),
)
def test_corrupt_image_headers_are_rejected_before_array_construction(
    owner: _ImageOwner, error: type[Exception], message: str
) -> None:
    with pytest.raises(error, match=message):
        rbt.ReceivedImageSample(owner).numpy()


@pytest.mark.parametrize(
    ("owner", "error", "message"),
    (
        (
            _PointCloudOwner(16, width=2, height=1, count=1),
            rbt.InvalidPayloadError,
            "does not match payload element count",
        ),
        (
            _PointCloudOwner(15, width=1, height=1, count=1),
            rbt.InvalidPayloadError,
            "expected 16",
        ),
        (
            _PointCloudOwner(16, width=1, height=1, count=1, address=1),
            rbt.InvalidPayloadAlignmentError,
            "is not aligned",
        ),
        (
            _PointCloudOwner(0, width=sys.maxsize, height=1, count=sys.maxsize),
            rbt.InvalidPayloadError,
            "expected",
        ),
    ),
)
def test_corrupt_pointcloud_headers_are_rejected_before_array_construction(
    owner: _PointCloudOwner, error: type[Exception], message: str
) -> None:
    with pytest.raises(error, match=message):
        rbt.ReceivedPointCloudSample(owner).numpy()

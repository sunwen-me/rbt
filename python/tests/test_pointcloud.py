from __future__ import annotations

import gc

import numpy as np
import pytest

import rbt

from conftest import background_process, cpp_interop_binary, unique_service


def _cpp_checksum(points: np.ndarray) -> int:
    checksum = np.zeros(points.shape, dtype=np.uint64)
    for field in ("x", "y", "z", "intensity"):
        checksum = checksum * np.uint64(31) + points[field].view(np.uint32).astype(np.uint64)
    return int(checksum.sum(dtype=np.uint64))


def test_cpp_pointcloud_zero_copy_structured_dtype() -> None:
    service = unique_service("pointcloud")
    with background_process(cpp_interop_binary(), "pointcloud-publisher", service):
        subscriber = rbt.PointCloudSubscriber.open(
            rbt.StreamDescriptor.point_cloud(service)
        )
        expected_checksums = (0, 66_571_993_088, 3_386_543_084_142_592, 3_677_129_335_508_477_952)
        for expected_count, expected_checksum in zip(
            (0, 1, 100, 100_000), expected_checksums, strict=True
        ):
            sample = subscriber.receive(timeout=10)
            assert sample is not None
            points = sample.numpy()
            assert points.shape == (expected_count,)
            assert points.dtype == rbt.POINT_XYZI_DTYPE
            assert points.dtype.itemsize == 16
            assert points.dtype.fields is not None
            assert points.dtype.fields["x"][1] == 0
            assert points.dtype.fields["y"][1] == 4
            assert points.dtype.fields["z"][1] == 8
            assert points.dtype.fields["intensity"][1] == 12
            assert points.__array_interface__["data"][0] == sample._debug_payload_address()
            assert not points.flags.owndata
            assert not points.flags.writeable
            with pytest.raises(ValueError):
                points.flags.writeable = True
            if expected_count:
                with pytest.raises(ValueError):
                    points[0]["intensity"] = 42.0
            assert _cpp_checksum(points) == expected_checksum
            print(
                "POINTCLOUD_EVIDENCE",
                f"count={expected_count}",
                f"checksum={expected_checksum}",
                f"native_ptr=0x{sample._debug_payload_address():x}",
                f"numpy_ptr=0x{points.__array_interface__['data'][0]:x}",
                f"owndata={points.flags.owndata}",
                f"writeable={points.flags.writeable}",
            )
            if expected_count:
                assert float(points[0]["x"]) == 0.0
                assert float(points[0]["y"]) == 0.0
                assert float(points[0]["z"]) == 0.0
                assert float(points[0]["intensity"]) == 0.0
                assert float(points[-1]["x"]) == float(expected_count - 1)
                assert float(points[-1]["y"]) == float((expected_count - 1) * 2)
                assert float(points[-1]["z"]) == float(-(expected_count - 1))
                assert float(points[-1]["intensity"]) == float((expected_count - 1) % 31)
            del sample
            gc.collect()
            assert _cpp_checksum(points) == expected_checksum
            del points
            gc.collect()

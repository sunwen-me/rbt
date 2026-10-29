from __future__ import annotations

import os
from pathlib import Path
import sys
import uuid

from rbt.tf import StaticEdge, StaticSet, TransformBuffer
from rbt.tf_math import FrameId, Quaternion, Transform3, Vec3
from rbt.time import ClockIdentity, ClockKind, ClockSourceId, Timestamp


def test_installed_wheel_uses_native_buffer_and_math() -> None:
    source_root = Path(os.environ["RBT_TF_SOURCE_ROOT"]).resolve()
    source_package = source_root / "python" / "rbt"
    package_file = Path(__import__("rbt").__file__).resolve()
    native_file = Path(__import__("rbt._rbt", fromlist=["_rbt"]).__file__).resolve()
    assert source_package not in package_file.parents
    assert source_package not in native_file.parents
    expected_venv = Path(os.environ["RBT_TF_EXPECTED_VENV"]).resolve()
    assert expected_venv in package_file.parents
    assert expected_venv in native_file.parents
    assert package_file.is_file()
    assert native_file.is_file()

    transform = Transform3(Vec3(1.0, 2.0, 3.0), Quaternion.identity())
    point = transform.apply_point(Vec3(4.0, 5.0, 6.0))
    assert point == Vec3(5.0, 7.0, 9.0)
    assert FrameId.from_bytes(FrameId("robot/base_link").to_bytes()) == FrameId(
        "robot/base_link"
    )

    buffer = TransformBuffer()
    authority = buffer.bind_provider(
        "external", uuid.UUID("00112233-4455-6677-8899-aabbccddeeff")
    )
    clock = ClockIdentity(
        ClockKind.DEVICE,
        ClockSourceId.from_device_uuid(bytes([1]) + bytes(15)),
        7,
    )
    stamp = Timestamp(clock, 42)
    buffer.apply_static_snapshot(
        authority,
        StaticSet(
            1,
            stamp,
            0,
            (StaticEdge("world", "base", transform),),
        ),
    )
    result = buffer.lookup_at("world", "base", stamp, uncertainty_ns=0)
    assert result.transform.translation == Vec3(1.0, 2.0, 3.0)
    assert result.sources[0].static_revision == 1
    assert buffer.pressure_snapshot()["active_providers"] == 1

    buffer.unbind_provider(authority)
    pressure = buffer.pressure_snapshot()
    assert pressure["active_providers"] == 0
    assert pressure["graph"] == []


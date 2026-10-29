from __future__ import annotations

import uuid

import pytest

from rbt.tf import (
    Authority,
    DynamicRecord,
    FrameId,
    LookupPolicy,
    Pose3,
    Quaternion,
    StaticEdge,
    StaticSet,
    TfError,
    TfResourceConfig,
    Transform3,
    TransformBuffer,
    Vec3,
)
from rbt.time import (
    ClockDiscontinuity,
    ClockIdentity,
    ClockKind,
    ClockMapping,
    ClockMappingCalibration,
    ClockSourceId,
    Timestamp,
)


def _clock(kind: ClockKind, marker: int, generation: int = 0) -> ClockIdentity:
    return ClockIdentity(kind, ClockSourceId(bytes([marker]) + bytes(47)), generation)


def _assert_code(code: str, operation: object) -> None:
    with pytest.raises(TfError) as raised:
        operation()  # type: ignore[operator]
    assert raised.value.code == code


def _buffer() -> tuple[TransformBuffer, Authority, ClockIdentity]:
    buffer = TransformBuffer()
    authority = buffer.bind_provider("camera", uuid.UUID("00112233-4455-6677-8899-aabbccddeeff"))
    return buffer, authority, _clock(ClockKind.DEVICE, 1)


def test_buffer_applies_authoritative_static_snapshot_and_reports_sources() -> None:
    buffer, authority, clock = _buffer()
    stamp = Timestamp(clock, 10)
    edge = StaticEdge("world", "base", Transform3(Vec3(1.0, 2.0, 3.0), Quaternion.identity()))
    buffer.apply_static_snapshot(authority, StaticSet(1, stamp, 0, (edge,)))

    result = buffer.lookup_at("world", "base", stamp, uncertainty_ns=0)
    assert result.transform.translation == Vec3(1.0, 2.0, 3.0)
    assert result.requested_timestamp == stamp
    assert result.timing_uncertainty_ns == 0
    assert len(result.sources) == 1
    assert result.sources[0].authority.slot == FrameId("camera")
    assert result.sources[0].static_revision == 1
    assert result.transform.apply_pose(Pose3(Vec3(0.0, 0.0, 0.0), Quaternion.identity())).position == Vec3(1.0, 2.0, 3.0)
    pressure = buffer.pressure_snapshot()
    assert pressure["lookup_successes"] == 1
    assert pressure["missing_frame_count"] == 0
    assert pressure["clock_mapping_failure_count"] == 0
    assert pressure["authority_conflict_count"] == 0
    provider = pressure["providers"][0]
    assert provider["missing_frame_count"] == 0
    assert provider["clock_mapping_failure_count"] == 0
    assert provider["authority_conflict_count"] == 0


def test_dynamic_ingest_is_local_and_unknown_precision_stays_strict() -> None:
    buffer, authority, clock = _buffer()
    stamp = Timestamp(clock, 20)
    record = DynamicRecord(
        FrameId("world"),
        FrameId("base"),
        stamp,
        0,
        0,
        Transform3(Vec3(0.0, 0.0, 2.0), Quaternion.identity()),
    )
    buffer.ingest_dynamic(authority, record)
    result = buffer.lookup_at("world", "base", stamp, uncertainty_ns=0)
    assert result.transform.translation == Vec3(0.0, 0.0, 2.0)
    _assert_code("time_unknown_uncertainty", lambda: buffer.lookup_at("world", "base", stamp))
    _assert_code("unknown_frame", lambda: buffer.lookup_at("missing", "base", stamp, uncertainty_ns=0))
    pressure = buffer.pressure_snapshot()
    assert pressure["missing_frame_count"] == 1
    assert pressure["providers"][0]["missing_frame_count"] == 1

    conflicting_authority = buffer.bind_provider(
        "other-camera", uuid.UUID("20112233-4455-6677-8899-aabbccddeeff")
    )
    conflicting_record = DynamicRecord(
        FrameId("world"),
        FrameId("base"),
        stamp,
        0,
        0,
        Transform3(Vec3(0.0, 0.0, 3.0), Quaternion.identity()),
    )
    _assert_code(
        "conflicting_authority",
        lambda: buffer.ingest_dynamic(conflicting_authority, conflicting_record),
    )
    pressure = buffer.pressure_snapshot()
    assert pressure["authority_conflict_count"] >= 1
    assert any(provider["authority_conflict_count"] >= 1 for provider in pressure["providers"])


def test_provider_replacement_stale_snapshots_and_bounds() -> None:
    buffer, authority, clock = _buffer()
    assert buffer.needs_resync(authority)
    buffer.mark_snapshot_stale(authority)
    stamp = Timestamp(clock, 1)
    buffer.apply_static_snapshot(authority, StaticSet(1, stamp, 0, ()))
    assert not buffer.needs_resync(authority)

    replacement = buffer.bind_provider("camera", uuid.UUID("10112233-4455-6677-8899-aabbccddeeff"))
    assert replacement != authority
    _assert_code(
        "provider_unavailable",
        lambda: buffer.mark_snapshot_stale(authority),
    )
    _assert_code("invalid_config", lambda: TfResourceConfig(max_providers=True))
    _assert_code("invalid_config", lambda: LookupPolicy(allow_unknown_uncertainty=1))
    with pytest.raises(TfError):
        StaticSet(0, stamp, 0, tuple(StaticEdge("p", f"c{index}", Transform3.identity()) for index in range(4097)))


def test_explicit_time_mapping_is_used_and_invalidated_mapping_is_preserved() -> None:
    buffer, authority, device_clock = _buffer()
    query_clock = _clock(ClockKind.SYSTEM_REALTIME, 2)
    host_clock = _clock(ClockKind.HOST_MONOTONIC, 3)
    calibration = ClockMappingCalibration(
        query_clock,
        device_clock,
        0,
        0,
        1,
        1,
        -100,
        100,
        b"\x01" + bytes(15),
        Timestamp(host_clock, 1),
        Timestamp(host_clock, 100),
        0,
    )
    mapping = ClockMapping(calibration)
    buffer.apply_static_snapshot(
        authority,
        StaticSet(1, Timestamp(device_clock, 5), 0, (StaticEdge("world", "base", Transform3.identity()),)),
    )
    _assert_code(
        "time_clock_mapping_unavailable",
        lambda: buffer.lookup_at(
            "world",
            "base",
            Timestamp(query_clock, 5),
            uncertainty_ns=0,
        ),
    )
    pressure = buffer.pressure_snapshot()
    assert pressure["clock_mapping_failure_count"] == 1
    assert pressure["providers"][0]["clock_mapping_failure_count"] == 1

    result = buffer.lookup_at(
        "world",
        "base",
        Timestamp(query_clock, 5),
        uncertainty_ns=0,
        mappings=(mapping,),
        now=Timestamp(host_clock, 2),
    )
    assert result.requested_timestamp == Timestamp(query_clock, 5)

    mapping.invalidate(ClockDiscontinuity.external_step(device_clock))
    before_invalidated_mapping = buffer.pressure_snapshot()
    _assert_code(
        "time_mapping_invalidated",
        lambda: buffer.lookup_at(
            "world",
            "base",
            Timestamp(query_clock, 5),
            uncertainty_ns=0,
            mappings=(mapping,),
            now=Timestamp(host_clock, 2),
        ),
    )
    pressure = buffer.pressure_snapshot()
    assert pressure["lookup_attempts"] == before_invalidated_mapping["lookup_attempts"]
    assert (
        pressure["providers"][0]["buffer_observed_lookup_attempts"]
        == before_invalidated_mapping["providers"][0]["buffer_observed_lookup_attempts"]
    )
    assert pressure["clock_mapping_failure_count"] == 1
    assert pressure["providers"][0]["clock_mapping_failure_count"] == 1


def test_typed_inputs_reject_bools_and_clock_mapping_bound_is_checked() -> None:
    _assert_code("invalid_config", lambda: TfResourceConfig(max_total_samples=False))
    _assert_code("invalid_config", lambda: LookupPolicy(max_timestamp_uncertainty_ns=True))
    buffer, _, clock = _buffer()
    _assert_code(
        "invalid_config",
        lambda: buffer.lookup_at("a", "b", Timestamp(clock, 0), uncertainty_ns=True),
    )
    _assert_code(
        "capacity_exceeded",
        lambda: buffer.lookup_at(
            "a",
            "b",
            Timestamp(clock, 0),
            uncertainty_ns=0,
            mappings=(None,) * 257,
        ),
    )

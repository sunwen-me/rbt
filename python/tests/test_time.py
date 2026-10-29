from __future__ import annotations

import dataclasses
import io
import uuid
from types import SimpleNamespace

import pytest

import rbt
import rbt.time as rbt_time
from rbt.time import (
    ClockDiscontinuity,
    ClockIdentity,
    ClockKind,
    ClockMapping,
    ClockMappingCalibration,
    ClockSourceId,
    DeviceClock,
    HostClock,
    SensorTiming,
    TimeError,
    TimeErrorCode,
    Timestamp,
    TimestampState,
    checked_add_nanoseconds,
    checked_compare,
    checked_difference,
    checked_duration_since,
)

I64_MIN = -(1 << 63)
I64_MAX = (1 << 63) - 1
U64_MAX = (1 << 64) - 1


def _identity(kind: ClockKind, marker: int, generation: int = 0) -> ClockIdentity:
    source_id = ClockSourceId(bytes([marker]) + bytes(47))
    return ClockIdentity(kind, source_id, generation)


def _calibration(
    *,
    source: ClockIdentity | None = None,
    target: ClockIdentity | None = None,
    source_anchor_ns: int = 10,
    target_anchor_ns: int = 20,
    rate_numerator: int = 3,
    rate_denominator: int = 2,
    valid_from_ns: int = -100,
    valid_until_ns: int = 100,
    uncertainty_ns: int | None = 0,
) -> ClockMappingCalibration:
    host = _identity(ClockKind.HOST_MONOTONIC, 3)
    return ClockMappingCalibration(
        source or _identity(ClockKind.DEVICE, 1),
        target or _identity(ClockKind.SYSTEM_REALTIME, 2),
        source_anchor_ns,
        target_anchor_ns,
        rate_numerator,
        rate_denominator,
        valid_from_ns,
        valid_until_ns,
        b"\x01" + bytes(15),
        Timestamp(host, 5),
        Timestamp(host, 100),
        uncertainty_ns,
    )


def _convert(
    mapping: ClockMapping,
    nanoseconds: int,
    uncertainty_ns: int | None = 0,
    now_ns: int = 10,
) -> object:
    calibration = mapping.calibration
    return mapping.convert(
        Timestamp(calibration.source, nanoseconds),
        uncertainty_ns,
        calibration.target,
        Timestamp(calibration.calibrated_at.clock, now_ns),
    )


def _assert_code(code: TimeErrorCode, operation: object) -> None:
    with pytest.raises(TimeError) as raised:
        operation()  # type: ignore[operator]
    assert raised.value.code is code


def test_package_reexports_pure_python_time_api() -> None:
    assert rbt.Timestamp is Timestamp
    assert rbt.ClockSourceId is ClockSourceId
    assert rbt.checked_compare is checked_compare
    assert rbt.TimeError is TimeError


def test_clock_source_id_layout_and_hex96_helpers() -> None:
    device_uuid = uuid.UUID("00112233-4455-6677-8899-aabbccddeeff")
    source = ClockSourceId.from_device_uuid(device_uuid)
    assert len(source.value) == 48
    assert source.value == device_uuid.bytes + bytes(32)
    assert source.hex96 == source.value.hex()
    assert ClockSourceId.from_hex96(source.hex96) == source
    assert source.device_uuid() == device_uuid
    _assert_code(TimeErrorCode.InvalidClockSourceId, lambda: ClockSourceId(bytes(48)))
    _assert_code(TimeErrorCode.InvalidClockSourceId, lambda: ClockSourceId.from_hex("ab"))
    _assert_code(
        TimeErrorCode.InvalidClockSourceId,
        lambda: ClockSourceId.from_hex("AB" + "00" * 47),
    )
    _assert_code(
        TimeErrorCode.InvalidClockIdentity,
        lambda: ClockIdentity(
            ClockKind.DEVICE,
            ClockSourceId(b"\x01" + bytes(16) + b"\x01" + bytes(30)),
        ),
    )
    _assert_code(
        TimeErrorCode.InvalidClockIdentity,
        lambda: ClockIdentity(
            ClockKind.HOST_MONOTONIC,
            ClockSourceId.for_host_monotonic(uuid.uuid4(), 1, 2, 0, 1_000_000_000),
        ),
    )


def test_timestamp_validation_serialization_and_no_implicit_ordering() -> None:
    clock = _identity(ClockKind.HOST_MONOTONIC, 4)
    timestamp = Timestamp(clock, 42)
    assert Timestamp.from_json(timestamp.to_json()) == timestamp
    clock.checked_compatible(ClockIdentity.from_dict(clock.to_dict()))
    _assert_code(TimeErrorCode.InvalidTimestamp, lambda: Timestamp(clock, True))
    _assert_code(TimeErrorCode.InvalidTimestamp, lambda: Timestamp(clock, 2.0))
    _assert_code(TimeErrorCode.TimestampOutOfRange, lambda: Timestamp(clock, I64_MAX + 1))
    with pytest.raises(TypeError):
        _ = timestamp < Timestamp(clock, 43)  # type: ignore[operator]
    with pytest.raises(TypeError):
        _ = timestamp - Timestamp(clock, 1)  # type: ignore[operator]


def test_checked_timestamp_arithmetic_rejects_domain_and_overflow() -> None:
    clock = _identity(ClockKind.DEVICE, 1)
    earlier = Timestamp(clock, -4)
    later = Timestamp(clock, 9)
    assert checked_compare(earlier, later) == -1
    assert checked_difference(later, earlier) == 13
    assert checked_duration_since(later, earlier) == 13
    assert checked_add_nanoseconds(later, -2).nanoseconds == 7
    _assert_code(
        TimeErrorCode.NegativeDuration,
        lambda: checked_duration_since(earlier, later),
    )
    _assert_code(
        TimeErrorCode.TimestampOverflow,
        lambda: checked_difference(Timestamp(clock, I64_MAX), Timestamp(clock, I64_MIN)),
    )
    _assert_code(
        TimeErrorCode.TimestampOverflow,
        lambda: checked_add_nanoseconds(Timestamp(clock, I64_MAX), 1),
    )
    _assert_code(
        TimeErrorCode.ClockDomainMismatch,
        lambda: checked_compare(
            Timestamp(clock, 0),
            Timestamp(_identity(ClockKind.SYSTEM_REALTIME, 1), 0),
        ),
    )
    _assert_code(
        TimeErrorCode.ClockSourceMismatch,
        lambda: checked_compare(
            Timestamp(clock, 0),
            Timestamp(_identity(ClockKind.DEVICE, 2), 0),
        ),
    )
    _assert_code(
        TimeErrorCode.ClockGenerationMismatch,
        lambda: checked_compare(
            Timestamp(clock, 0),
            Timestamp(_identity(ClockKind.DEVICE, 1, generation=1), 0),
        ),
    )


def test_device_clock_stamps_and_emits_explicit_generation_discontinuity() -> None:
    clock = DeviceClock(uuid.UUID("10112233-4455-6677-8899-aabbccddeeff"))
    before = clock.stamp(50)
    discontinuity = clock.confirm_reset()
    after = clock.stamp(50)
    assert isinstance(discontinuity, ClockDiscontinuity)
    assert discontinuity.previous == before.clock
    assert discontinuity.current == after.clock
    assert after.clock.generation == before.clock.generation + 1
    _assert_code(
        TimeErrorCode.ClockGenerationMismatch,
        lambda: checked_compare(before, after),
    )
    _assert_code(
        TimeErrorCode.TimeSourceReset,
        lambda: ClockDiscontinuity(before.clock, ClockIdentity(before.clock.kind, before.clock.source_id, 2)),
    )


def test_external_step_affects_same_identity_without_relaxing_reset_validation() -> None:
    clock = _identity(ClockKind.DEVICE, 1)
    event = ClockDiscontinuity.external_step(clock)
    assert event.event_kind == "external_step"
    assert event.previous == event.current == clock
    assert event.affects(clock)
    assert not event.affects(_identity(ClockKind.DEVICE, 2))
    _assert_code(TimeErrorCode.TimeSourceReset, lambda: ClockDiscontinuity(clock, clock))
    assert ClockDiscontinuity(clock, clock, "external_step") == event


def test_mapping_invalidation_matches_both_discontinuity_generations() -> None:
    source_previous = _identity(ClockKind.DEVICE, 1, 0)
    source_current = _identity(ClockKind.DEVICE, 1, 1)
    mapping = ClockMapping(_calibration(source=source_current))
    event = ClockDiscontinuity(source_previous, source_current, "device_reset")
    assert mapping.invalidate(event)
    assert mapping.invalidated
    _assert_code(TimeErrorCode.MappingInvalidated, lambda: _convert(mapping, 11))

    target_current = _identity(ClockKind.SYSTEM_REALTIME, 2, 1)
    target_mapping = ClockMapping(_calibration(target=target_current))
    target_event = ClockDiscontinuity(
        _identity(ClockKind.SYSTEM_REALTIME, 2, 0),
        target_current,
        "realtime_step",
    )
    assert target_mapping.invalidate(target_event)


def test_sensor_timing_keeps_legacy_capture_time_unknown() -> None:
    received = Timestamp(_identity(ClockKind.HOST_MONOTONIC, 5), 200)
    timing = SensorTiming.from_legacy(received, received_uncertainty_ns=0)
    assert timing.capture is None
    assert timing.capture_state == TimestampState.unknown()
    assert timing.capture_state.kind == "unknown"
    assert timing.received == received
    assert timing.received_state == TimestampState.known(received)
    assert timing.received_uncertainty_ns == 0
    known_capture = SensorTiming(Timestamp(received.clock, 190), received, 4, 0)
    assert known_capture.capture_state.kind == "known"
    assert known_capture.capture_uncertainty_ns == 4
    unmapped = TimestampState.unmapped(known_capture.capture)
    assert TimestampState.from_json(unmapped.to_json()) == unmapped


def test_host_clock_identity_uses_boot_namespace_and_explicit_offset_basis(monkeypatch: pytest.MonkeyPatch) -> None:
    host = HostClock()
    namespace = rbt_time._read_namespace_stat()
    offset_seconds, offset_nanoseconds = rbt_time._read_monotonic_offset()
    expected = ClockSourceId.for_host_monotonic(
        rbt_time._read_boot_uuid(),
        namespace.st_dev,
        namespace.st_ino,
        offset_seconds,
        offset_nanoseconds,
    )
    assert host.monotonic_identity.kind is ClockKind.HOST_MONOTONIC
    assert host.monotonic_identity.source_id == expected
    monkeypatch.setattr(rbt_time._time, "clock_gettime_ns", lambda _clock: 123)
    assert host.monotonic_now() == Timestamp(host.monotonic_identity, 123)
    assert host.realtime_now().clock == host.realtime_identity
    reset = host.confirm_realtime_reset()
    assert reset.current.generation == reset.previous.generation + 1
    assert host.realtime_now().clock == reset.current


def test_host_clock_fails_when_namespace_offset_basis_is_unknown() -> None:
    def unknown_basis() -> tuple[int, int]:
        raise TimeError(TimeErrorCode.UnknownClockDomain, "unknown monotonic offset")

    with pytest.MonkeyPatch.context() as monkeypatch:
        monkeypatch.setattr(rbt_time, "_read_monotonic_offset", unknown_basis)
        with pytest.raises(TimeError) as raised:
            HostClock()
    assert raised.value.code is TimeErrorCode.UnknownClockDomain


@pytest.mark.parametrize(
    "value",
    [
        "not-a-uuid\n",
        "00000000-0000-0000-0000-000000000000\n",
        "AAAAAAAA-AAAA-AAAA-AAAA-AAAAAAAAAAAA\n",
    ],
)
def test_host_clock_malformed_boot_uuid_is_unknown_domain(
    monkeypatch: pytest.MonkeyPatch,
    value: str,
) -> None:
    monkeypatch.setattr("builtins.open", lambda *args, **kwargs: io.StringIO(value))
    _assert_code(TimeErrorCode.UnknownClockDomain, rbt_time._read_boot_uuid)


def test_host_clock_boot_uuid_io_failure_is_unavailable(monkeypatch: pytest.MonkeyPatch) -> None:
    def unavailable(*_args: object, **_kwargs: object) -> io.StringIO:
        raise OSError("unavailable")

    monkeypatch.setattr("builtins.open", unavailable)
    _assert_code(TimeErrorCode.ClockUnavailable, rbt_time._read_boot_uuid)


def test_host_monotonic_read_checks_namespace_after_clock_read(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(rbt_time, "_read_monotonic_offset", lambda: (0, 0))
    host = HostClock()
    namespace = rbt_time._read_namespace_stat()
    changed = SimpleNamespace(st_dev=namespace.st_dev, st_ino=namespace.st_ino + 1)
    reads = iter((namespace, changed))
    monkeypatch.setattr(rbt_time, "_read_namespace_stat", lambda: next(reads))
    monkeypatch.setattr(rbt_time._time, "clock_gettime_ns", lambda _clock: 123)
    _assert_code(TimeErrorCode.TimeSourceReset, host.monotonic_now)


def test_realtime_reset_during_read_is_reported(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setattr(rbt_time, "_read_monotonic_offset", lambda: (0, 0))
    host = HostClock()

    def reset_during_read(clock_id: int) -> int:
        if clock_id == rbt_time._time.CLOCK_REALTIME:
            host.confirm_realtime_reset()
        return 123

    monkeypatch.setattr(rbt_time._time, "clock_gettime_ns", reset_during_read)
    _assert_code(TimeErrorCode.TimeSourceReset, host.realtime_now)


def test_mapping_rounding_exact_uncertainty_and_negative_values() -> None:
    mapping = ClockMapping(_calibration())
    assert ClockMappingCalibration.from_json(mapping.calibration.to_json()) == mapping.calibration
    converted = _convert(mapping, 11, uncertainty_ns=0)
    assert converted.timestamp.nanoseconds == 21
    assert converted.uncertainty_ns == 1
    exact = _convert(mapping, 12, uncertainty_ns=0)
    assert exact.timestamp.nanoseconds == 23
    assert exact.uncertainty_ns == 0
    negative = _convert(mapping, 9, uncertainty_ns=0)
    assert negative.timestamp.nanoseconds == 19
    assert negative.uncertainty_ns == 1
    scaled = _convert(mapping, 11, uncertainty_ns=1)
    assert scaled.uncertainty_ns == 3
    _assert_code(TimeErrorCode.UnknownUncertainty, lambda: _convert(mapping, 11, None))
    _assert_code(
        TimeErrorCode.UnknownUncertainty,
        lambda: _convert(ClockMapping(_calibration(uncertainty_ns=None)), 11, 0),
    )


def test_mapping_checks_identity_expiry_range_and_invalidation() -> None:
    mapping = ClockMapping(_calibration())
    calibration = mapping.calibration
    _assert_code(
        TimeErrorCode.ClockSourceMismatch,
        lambda: mapping.convert(
            Timestamp(_identity(ClockKind.DEVICE, 9), 11),
            0,
            calibration.target,
            Timestamp(calibration.calibrated_at.clock, 10),
        ),
    )
    _assert_code(
        TimeErrorCode.ClockSourceMismatch,
        lambda: mapping.convert(
            Timestamp(calibration.source, 11),
            0,
            _identity(ClockKind.SYSTEM_REALTIME, 8),
            Timestamp(calibration.calibrated_at.clock, 10),
        ),
    )
    _assert_code(
        TimeErrorCode.ClockMappingExpired,
        lambda: _convert(mapping, 11, now_ns=100),
    )
    _assert_code(
        TimeErrorCode.TimestampOutOfRange,
        lambda: _convert(mapping, 101),
    )
    _assert_code(
        TimeErrorCode.ClockMappingUnavailable,
        lambda: _convert(mapping, 11, now_ns=4),
    )

    device = DeviceClock(calibration.source)
    discontinuity = device.confirm_reset()
    assert mapping.invalidate(discontinuity)
    assert mapping.invalidated
    _assert_code(TimeErrorCode.MappingInvalidated, lambda: _convert(mapping, 11))


def test_mapping_rejects_bad_rates_and_signed_128_or_output_overflow() -> None:
    with pytest.raises(TimeError) as raised:
        _calibration(rate_numerator=0)
    assert raised.value.code is TimeErrorCode.InvalidConversionRate

    huge_delta = ClockMapping(
        _calibration(
            source_anchor_ns=I64_MIN,
            rate_numerator=U64_MAX,
            rate_denominator=1,
            valid_from_ns=I64_MIN,
            valid_until_ns=I64_MAX,
        )
    )
    _assert_code(TimeErrorCode.TimestampOverflow, lambda: _convert(huge_delta, I64_MAX))

    output_overflow = ClockMapping(
        _calibration(
            source_anchor_ns=10,
            target_anchor_ns=I64_MAX,
            rate_numerator=1,
            rate_denominator=1,
        )
    )
    _assert_code(TimeErrorCode.TimestampOverflow, lambda: _convert(output_overflow, 11))

    uncertainty_overflow = ClockMapping(
        _calibration(rate_numerator=U64_MAX, rate_denominator=1)
    )
    _assert_code(
        TimeErrorCode.TimestampOverflow,
        lambda: _convert(uncertainty_overflow, 10, uncertainty_ns=U64_MAX),
    )


def test_mapping_uncertainty_uses_unsigned_128_product_and_requires_known_mapping_error() -> None:
    maximum_rate = ClockMapping(
        _calibration(
            source_anchor_ns=10,
            rate_numerator=U64_MAX,
            rate_denominator=U64_MAX,
        )
    )
    converted = _convert(maximum_rate, 10, uncertainty_ns=U64_MAX)
    assert converted.timestamp.nanoseconds == 20
    assert converted.uncertainty_ns == U64_MAX
    _assert_code(
        TimeErrorCode.UnknownUncertainty,
        lambda: ClockMapping(_calibration(uncertainty_ns=None)),
    )


def test_mapping_rejects_warping_a_clock_to_itself() -> None:
    clock = _identity(ClockKind.DEVICE, 6)
    identity_calibration = {
        "source": clock,
        "target": clock,
        "source_anchor_ns": 10,
        "target_anchor_ns": 10,
    }
    offset = _calibration(**{**identity_calibration, "target_anchor_ns": 11})
    rate_distortion = _calibration(
        **identity_calibration,
        rate_numerator=3,
        rate_denominator=2,
    )
    _assert_code(TimeErrorCode.ClockMappingInvalid, lambda: ClockMapping(offset))
    _assert_code(TimeErrorCode.ClockMappingInvalid, lambda: ClockMapping(rate_distortion))

    equivalent_rate = ClockMapping(
        _calibration(
            **identity_calibration,
            rate_numerator=2,
            rate_denominator=2,
        )
    )
    converted = _convert(equivalent_rate, 13, uncertainty_ns=0)
    assert converted.timestamp.clock == clock
    assert converted.timestamp.nanoseconds == 13


def test_per_sample_device_and_mapping_reads_do_not_take_writer_locks() -> None:
    class NoReadLock:
        def __enter__(self) -> object:
            raise AssertionError("read path acquired writer lock")

        def __exit__(self, *_args: object) -> None:
            raise AssertionError("read path acquired writer lock")

    device = DeviceClock(uuid.UUID("10112233-4455-6677-8899-aabbccddeeff"))
    device._lock = NoReadLock()  # type: ignore[assignment]
    assert device.stamp(20).clock == device.identity

    mapping = ClockMapping(_calibration())
    mapping._lock = NoReadLock()  # type: ignore[assignment]
    assert not mapping.invalidated
    assert _convert(mapping, 12).timestamp.nanoseconds == 23


def test_calibration_requires_matching_host_monotonic_lifetime_evidence() -> None:
    calibration = _calibration()
    _assert_code(
        TimeErrorCode.InvalidCalibrationEvidence,
        lambda: dataclasses.replace(
            calibration,
            calibrated_at=Timestamp(_identity(ClockKind.DEVICE, 4), 5),
        ),
    )
    _assert_code(
        TimeErrorCode.InvalidCalibrationEvidence,
        lambda: dataclasses.replace(
            calibration,
            expires_at=Timestamp(_identity(ClockKind.HOST_MONOTONIC, 4), 100),
        ),
    )
    _assert_code(
        TimeErrorCode.InvalidCalibrationEvidence,
        lambda: dataclasses.replace(
            calibration,
            expires_at=Timestamp(calibration.calibrated_at.clock, 5),
        ),
    )


def test_bad_serde_like_inputs_keep_stable_time_errors() -> None:
    _assert_code(
        TimeErrorCode.UnknownClockDomain,
        lambda: ClockIdentity.from_dict(
            {"kind": "future_clock", "source_id": "01" * 48, "generation": 0}
        ),
    )
    _assert_code(
        TimeErrorCode.InvalidTimestamp,
        lambda: Timestamp.from_dict(
            {
                "clock": {
                    "kind": "device",
                    "source_id": ("01" + "00" * 47),
                    "generation": 0,
                },
                "nanoseconds": True,
            }
        ),
    )

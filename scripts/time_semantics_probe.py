#!/usr/bin/env python3
"""Exercise Python clock semantics or compare three language probe outputs."""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import time
from collections.abc import Callable
from typing import Any

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "python"))

from rbt.time import (  # noqa: E402
    ClockIdentity,
    ClockKind,
    ClockMapping,
    ClockMappingCalibration,
    ClockSourceId,
    DeviceClock,
    HostClock,
    SensorTiming,
    TimeError,
    Timestamp,
)


def error_code(operation: Callable[[], Any]) -> str:
    try:
        operation()
    except TimeError as error:
        return error.code.value
    return "UNEXPECTED_OK"


def make_mapping(
    source: ClockIdentity,
    target: ClockIdentity,
    host: ClockIdentity,
    numerator: int,
    denominator: int,
    uncertainty: int,
    source_anchor_ns: int = 100,
    target_anchor_ns: int = 1000,
) -> ClockMapping:
    calibration = ClockMappingCalibration(
        source=source,
        target=target,
        source_anchor_ns=source_anchor_ns,
        target_anchor_ns=target_anchor_ns,
        rate_numerator=numerator,
        rate_denominator=denominator,
        valid_from_ns=0,
        valid_until_ns=10_000,
        calibration_id=bytes([1]) * 16,
        calibrated_at=Timestamp(host, 10),
        expires_at=Timestamp(host, 200),
        uncertainty_ns=uncertainty,
    )
    return ClockMapping(calibration)


def live_host_observation() -> dict[str, Any]:
    try:
        clock = HostClock()
        now = clock.monotonic_now()
    except TimeError as error:
        return {"available": False, "error": error.code.value}
    return {
        "available": True,
        "kind": now.clock.kind.value,
        "source_id": now.clock.source_id.hex96,
        "generation": now.clock.generation,
        "nanoseconds": now.nanoseconds,
    }


def python_semantics() -> dict[str, Any]:
    device_id = bytes([0x11]) * 16
    other_device_id = bytes([0x22]) * 16
    device = DeviceClock(device_id)
    device_before = device.stamp(103)
    other_device_stamp = DeviceClock(other_device_id).stamp(103)

    host_source = ClockSourceId.for_host_monotonic(bytes([0x44]) * 16, 1, 2, 0, 0)
    host_identity = ClockIdentity(ClockKind.HOST_MONOTONIC, host_source)
    realtime_source = ClockSourceId.for_system_realtime(bytes([0x33]) * 16)
    realtime = ClockIdentity(ClockKind.SYSTEM_REALTIME, realtime_source)

    arithmetic_identity = device.identity
    arithmetic_left = Timestamp(arithmetic_identity, 7)
    arithmetic_right = Timestamp(arithmetic_identity, 4)
    arithmetic_add = arithmetic_right.checked_add_nanoseconds(-2)
    add_overflow = error_code(
        lambda: Timestamp(arithmetic_identity, (1 << 63) - 1).checked_add_nanoseconds(1)
    )
    negative_duration = error_code(
        lambda: Timestamp(arithmetic_identity, 10).checked_duration_since(Timestamp(arithmetic_identity, 11))
    )

    base_mapping = make_mapping(device_before.clock, realtime, host_identity, 3, 2, 5)
    converted = base_mapping.convert(
        device_before, 2, realtime, Timestamp(host_identity, 11)
    )
    expiry = error_code(
        lambda: base_mapping.convert(device_before, 2, realtime, Timestamp(host_identity, 200))
    )
    unknown_uncertainty = error_code(
        lambda: base_mapping.convert(device_before, None, realtime, Timestamp(host_identity, 11))
    )

    reset_event = device.confirm_reset()
    reset_old_stamp = error_code(lambda: device_before.checked_compare(device.stamp(103)))
    invalidated_mapping = make_mapping(device_before.clock, realtime, host_identity, 1, 1, 0)
    invalidated = invalidated_mapping.invalidate(reset_event)
    invalidated_error = error_code(
        lambda: invalidated_mapping.convert(device_before, 0, realtime, Timestamp(host_identity, 11))
    )
    overflow_mapping = make_mapping(device_before.clock, realtime, host_identity, 2, 1, 1)
    uncertainty_overflow = error_code(
        lambda: overflow_mapping.convert(device_before, (1 << 64) - 1, realtime, Timestamp(host_identity, 11))
    )
    maximum_uncertainty_mapping = make_mapping(
        device_before.clock, realtime, host_identity, (1 << 64) - 1, (1 << 64) - 1, 0
    )
    maximum_uncertainty = maximum_uncertainty_mapping.convert(
        device_before, (1 << 64) - 1, realtime, Timestamp(host_identity, 11)
    )
    self_mapping_offset = error_code(
        lambda: make_mapping(
            device_before.clock,
            device_before.clock,
            host_identity,
            1,
            1,
            0,
            100,
            101,
        )
    )
    self_mapping_rate = error_code(
        lambda: make_mapping(
            device_before.clock,
            device_before.clock,
            host_identity,
            2,
            1,
            0,
            100,
            100,
        )
    )
    self_mapping_identity = make_mapping(
        device_before.clock,
        device_before.clock,
        host_identity,
        2,
        2,
        0,
        100,
        100,
    ).convert(device_before, 0, device_before.clock, Timestamp(host_identity, 11))

    timing = SensorTiming(device_before, Timestamp(host_identity, 400), 2, 1)
    legacy_state = SensorTiming.from_legacy(Timestamp(host_identity, 401)).capture_state.kind

    return {
        "cases": {
            "arithmetic": {
                "difference_ns": arithmetic_left.checked_difference(arithmetic_right),
                "duration_ns": arithmetic_left.checked_duration_since(arithmetic_right),
                "add_result_ns": arithmetic_add.nanoseconds,
                "add_overflow": add_overflow,
                "negative_duration": negative_duration,
            },
            "identity": {
                "source_mismatch": error_code(lambda: device_before.checked_compare(other_device_stamp)),
                "generation_mismatch": error_code(lambda: device_before.checked_compare(device.stamp(103))),
                "domain_mismatch": error_code(
                    lambda: device_before.checked_compare(Timestamp(realtime, 103))
                ),
            },
            "device_reset": {
                "generation": device.identity.generation,
                "old_stamp_rejected": reset_old_stamp,
            },
            "mapping": {
                "timestamp_ns": converted.timestamp.nanoseconds,
                "uncertainty_ns": converted.uncertainty_ns,
                "expiry": expiry,
                "unknown_uncertainty": unknown_uncertainty,
                "invalidated": invalidated_error if invalidated else "INVALIDATION_FAILED",
                "uncertainty_overflow": uncertainty_overflow,
                "uncertainty_u64_max": maximum_uncertainty.uncertainty_ns,
                "self_mapping_offset": self_mapping_offset,
                "self_mapping_rate": self_mapping_rate,
                "self_mapping_identity": self_mapping_identity.timestamp.nanoseconds,
            },
            "capture_separation": {
                "capture_kind": timing.capture.clock.kind.value,
                "received_kind": timing.received.clock.kind.value,
                "legacy_state": legacy_state,
            },
        },
        "host_monotonic": live_host_observation(),
    }


def read_json(path: str) -> dict[str, Any]:
    try:
        value = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise SystemExit(f"cannot read JSON probe {path}: {error}") from error
    if not isinstance(value, dict):
        raise SystemExit(f"probe output is not a JSON object: {path}")
    return value


def compare_outputs(fixture_path: str, rust_path: str, cpp_path: str, python_path: str) -> None:
    fixture = read_json(fixture_path)
    expected = fixture.get("expected")
    if not isinstance(expected, dict):
        raise SystemExit("time fixture has no expected object")
    reports = {
        "rust": read_json(rust_path),
        "cpp": read_json(cpp_path),
        "python": read_json(python_path),
    }
    for language, report in reports.items():
        if report.get("cases") != expected:
            raise SystemExit(f"{language} semantics differ from hand-calculated fixture: {report.get('cases')!r}")
    host_values = {language: report.get("host_monotonic") for language, report in reports.items()}
    available = [value for value in host_values.values() if isinstance(value, dict) and value.get("available") is True]
    if len(available) != len(reports):
        reasons = {
            language: (value or {}).get("error", "missing host clock result")
            for language, value in host_values.items()
            if not isinstance(value, dict) or value.get("available") is not True
        }
        raise SystemExit(f"host monotonic identity is required but unavailable: {reasons}")
    identities = {
        (
            value.get("kind"),
            value.get("source_id"),
            value.get("generation"),
        )
        for value in available
    }
    if len(identities) != 1:
        raise SystemExit(f"Rust/C++/Python host monotonic identities disagree: {host_values}")
    observed_ns = []
    for language, value in host_values.items():
        if not isinstance(value.get("nanoseconds"), int):
            raise SystemExit(f"{language} host monotonic observation is missing an integer reading")
        observed_ns.append(value["nanoseconds"])
    spread_ns = max(observed_ns) - min(observed_ns)
    if observed_ns != sorted(observed_ns) or spread_ns > 300_000_000_000:
        raise SystemExit(f"host monotonic readings are out of order or too far apart: {observed_ns}")
    local_now_ns = time.monotonic_ns()
    if max(observed_ns) > local_now_ns or local_now_ns - min(observed_ns) > 300_000_000_000:
        raise SystemExit(
            "host monotonic readings do not share this process's nanosecond clock origin: "
            f"observations={observed_ns} local_now={local_now_ns}"
        )
    print("RBT_TIME_SEMANTICS PASS languages=rust,cpp,python fixture=hand-calculated host_identity=equal")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--emit-json", action="store_true", help="emit this process's Python semantics probe")
    parser.add_argument("--compare", action="store_true", help="compare Rust, C++, and Python probe output")
    parser.add_argument("--fixture", default=str(REPO_ROOT / "tests/fixtures/time/semantics.json"))
    parser.add_argument("--rust-json")
    parser.add_argument("--cpp-json")
    parser.add_argument("--python-json")
    arguments = parser.parse_args()
    if arguments.compare:
        missing = [name for name in ("rust_json", "cpp_json", "python_json") if not getattr(arguments, name)]
        if missing:
            parser.error(f"--compare requires --{' --'.join(name.replace('_', '-') for name in missing)}")
        compare_outputs(arguments.fixture, arguments.rust_json, arguments.cpp_json, arguments.python_json)
    else:
        print(json.dumps(python_semantics(), separators=(",", ":"), sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except TimeError as error:
        raise SystemExit(f"RBT_TIME_SEMANTICS ERROR {error.code.value}: {error}") from error

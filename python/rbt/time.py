"""Explicit clock identities and checked timestamp arithmetic.

This module keeps absolute timestamps tied to a declared clock identity. It
does not infer a clock for legacy sensor timestamps or change runtime timeout
behavior.
"""

from __future__ import annotations

import json
import operator
import os
import threading
import time as _time
import uuid
from collections.abc import Mapping
from dataclasses import dataclass
from enum import Enum
from typing import Any

_I64_MIN = -(1 << 63)
_I64_MAX = (1 << 63) - 1
_U64_MAX = (1 << 64) - 1
_I128_MIN = -(1 << 127)
_I128_MAX = (1 << 127) - 1
_U128_MAX = (1 << 128) - 1
_CLOCK_SOURCE_ID_SIZE = 48
_DEVICE_UUID_SIZE = 16


class TimeErrorCode(str, Enum):
    """Stable error codes for checked time operations."""

    ClockDomainMismatch = "ClockDomainMismatch"
    ClockSourceMismatch = "ClockSourceMismatch"
    ClockGenerationMismatch = "ClockGenerationMismatch"
    UnknownClockDomain = "UnknownClockDomain"
    ClockMappingUnavailable = "ClockMappingUnavailable"
    ClockMappingExpired = "ClockMappingExpired"
    ClockMappingInvalid = "ClockMappingInvalid"
    MappingInvalidated = "MappingInvalidated"
    TimestampOutOfRange = "TimestampOutOfRange"
    TimestampOverflow = "TimestampOverflow"
    InvalidTimestamp = "InvalidTimestamp"
    InvalidConversionRate = "InvalidConversionRate"
    UnknownUncertainty = "UnknownUncertainty"
    TimeSourceReset = "TimeSourceReset"
    ClockUnavailable = "ClockUnavailable"
    InvalidCalibrationEvidence = "InvalidCalibrationEvidence"
    InvalidClockSourceId = "InvalidClockSourceId"
    InvalidClockIdentity = "InvalidClockIdentity"
    NegativeDuration = "NegativeDuration"


class TimeError(Exception):
    """A checked time operation failed with a stable machine-readable code."""

    def __init__(self, code: TimeErrorCode | str, message: str = "") -> None:
        self.code = TimeErrorCode(code)
        super().__init__(message or self.code.value)


class ClockKind(str, Enum):
    HOST_MONOTONIC = "host_monotonic"
    SYSTEM_REALTIME = "system_realtime"
    DEVICE = "device"


def _integer(
    value: Any,
    *,
    name: str,
    minimum: int,
    maximum: int,
    code: TimeErrorCode = TimeErrorCode.InvalidTimestamp,
    range_code: TimeErrorCode | None = None,
) -> int:
    if isinstance(value, bool):
        raise TimeError(code, f"{name} must be an integer")
    try:
        result = operator.index(value)
    except TypeError as error:
        raise TimeError(code, f"{name} must be an integer") from error
    if result < minimum or result > maximum:
        raise TimeError(range_code or code, f"{name} is outside its supported range")
    return int(result)


def _bytes(value: Any, *, name: str, size: int, code: TimeErrorCode) -> bytes:
    if not isinstance(value, (bytes, bytearray, memoryview)):
        raise TimeError(code, f"{name} must be bytes")
    result = bytes(value)
    if len(result) != size:
        raise TimeError(code, f"{name} must contain exactly {size} bytes")
    return result


def _mapping(
    value: Any,
    *,
    name: str,
    code: TimeErrorCode = TimeErrorCode.InvalidTimestamp,
) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise TimeError(code, f"{name} must be an object")
    return value


@dataclass(frozen=True)
class ClockSourceId:
    """A non-zero 48-byte source identifier with compact hexadecimal helpers."""

    value: bytes

    def __post_init__(self) -> None:
        value = _bytes(
            self.value,
            name="clock source id",
            size=_CLOCK_SOURCE_ID_SIZE,
            code=TimeErrorCode.InvalidClockSourceId,
        )
        if not any(value):
            raise TimeError(TimeErrorCode.InvalidClockSourceId, "clock source id cannot be zero")
        object.__setattr__(self, "value", value)

    @classmethod
    def from_hex(cls, value: str) -> ClockSourceId:
        if (
            not isinstance(value, str)
            or len(value) != _CLOCK_SOURCE_ID_SIZE * 2
            or any(character not in "0123456789abcdef" for character in value)
        ):
            raise TimeError(
                TimeErrorCode.InvalidClockSourceId,
                "clock source id must be 96 lowercase hex digits",
            )
        return cls(bytes.fromhex(value))

    @classmethod
    def from_hex96(cls, value: str) -> ClockSourceId:
        return cls.from_hex(value)

    @classmethod
    def from_device_uuid(cls, value: uuid.UUID | bytes | bytearray | memoryview | str) -> ClockSourceId:
        if isinstance(value, uuid.UUID):
            device_uuid = value.bytes
        elif isinstance(value, str):
            try:
                device_uuid = uuid.UUID(value).bytes
            except (ValueError, AttributeError) as error:
                raise TimeError(TimeErrorCode.InvalidClockSourceId, "device UUID is invalid") from error
        else:
            device_uuid = _bytes(
                value,
                name="device UUID",
                size=_DEVICE_UUID_SIZE,
                code=TimeErrorCode.InvalidClockSourceId,
            )
        return cls(device_uuid + bytes(_CLOCK_SOURCE_ID_SIZE - _DEVICE_UUID_SIZE))

    @classmethod
    def for_host_monotonic(
        cls,
        boot_uuid: uuid.UUID | bytes,
        namespace_device: int,
        namespace_inode: int,
        offset_seconds: int,
        offset_nanoseconds: int,
    ) -> ClockSourceId:
        boot_bytes = boot_uuid.bytes if isinstance(boot_uuid, uuid.UUID) else _bytes(
            boot_uuid,
            name="boot UUID",
            size=_DEVICE_UUID_SIZE,
                code=TimeErrorCode.InvalidClockSourceId,
        )
        device = _integer(
            namespace_device, name="namespace device", minimum=0, maximum=_U64_MAX,
            code=TimeErrorCode.UnknownClockDomain,
        )
        inode = _integer(
            namespace_inode, name="namespace inode", minimum=0, maximum=_U64_MAX,
            code=TimeErrorCode.UnknownClockDomain,
        )
        seconds = _integer(
            offset_seconds, name="monotonic offset seconds", minimum=_I64_MIN, maximum=_I64_MAX,
            code=TimeErrorCode.UnknownClockDomain,
        )
        nanoseconds = _integer(
            offset_nanoseconds,
            name="monotonic offset nanoseconds",
            minimum=0,
            maximum=999_999_999,
            code=TimeErrorCode.InvalidClockIdentity,
        )
        return cls(
            boot_bytes
            + device.to_bytes(8, "little")
            + inode.to_bytes(8, "little")
            + seconds.to_bytes(8, "little", signed=True)
            + nanoseconds.to_bytes(8, "little", signed=True)
        )

    @classmethod
    def for_system_realtime(cls, boot_uuid: uuid.UUID | bytes) -> ClockSourceId:
        boot_bytes = boot_uuid.bytes if isinstance(boot_uuid, uuid.UUID) else _bytes(
            boot_uuid,
            name="boot UUID",
            size=_DEVICE_UUID_SIZE,
            code=TimeErrorCode.InvalidClockSourceId,
        )
        return cls(boot_bytes + bytes(_CLOCK_SOURCE_ID_SIZE - _DEVICE_UUID_SIZE))

    @property
    def hex96(self) -> str:
        return self.value.hex()

    def to_hex(self) -> str:
        return self.hex96

    def to_bytes(self) -> bytes:
        return self.value

    def device_uuid(self) -> uuid.UUID:
        if any(self.value[_DEVICE_UUID_SIZE:]):
            raise TimeError(TimeErrorCode.InvalidClockIdentity, "source id is not a device UUID source")
        return uuid.UUID(bytes=self.value[:_DEVICE_UUID_SIZE])


@dataclass(frozen=True)
class ClockIdentity:
    kind: ClockKind
    source_id: ClockSourceId
    generation: int = 0

    def __post_init__(self) -> None:
        if not isinstance(self.kind, ClockKind):
            raise TimeError(TimeErrorCode.InvalidClockIdentity, "clock kind is invalid")
        if not isinstance(self.source_id, ClockSourceId):
            raise TimeError(TimeErrorCode.InvalidClockIdentity, "clock source id is invalid")
        source = self.source_id.value
        if not any(source[:_DEVICE_UUID_SIZE]):
            raise TimeError(TimeErrorCode.InvalidClockIdentity, "clock source prefix cannot be zero")
        if self.kind in (ClockKind.SYSTEM_REALTIME, ClockKind.DEVICE):
            if any(source[_DEVICE_UUID_SIZE:]):
                raise TimeError(TimeErrorCode.InvalidClockIdentity, "clock source layout is invalid")
        elif not 0 <= int.from_bytes(source[40:48], "little", signed=True) <= 999_999_999:
            raise TimeError(TimeErrorCode.InvalidClockIdentity, "host monotonic offset nanoseconds are invalid")
        generation = _integer(
            self.generation,
            name="clock generation",
            minimum=0,
            maximum=_U64_MAX,
            code=TimeErrorCode.InvalidClockIdentity,
        )
        object.__setattr__(self, "generation", generation)

    def checked_compatible(self, other: ClockIdentity) -> None:
        """Raise the matching clock error unless both complete identities match."""
        if not isinstance(other, ClockIdentity):
            raise TimeError(TimeErrorCode.UnknownClockDomain, "clock identity is unknown")
        _same_clock(self, other)

    def to_dict(self) -> dict[str, Any]:
        return {
            "kind": self.kind.value,
            "source_id": self.source_id.hex96,
            "generation": self.generation,
        }

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> ClockIdentity:
        data = _mapping(
            value,
            name="clock identity",
            code=TimeErrorCode.InvalidClockIdentity,
        )
        try:
            kind = ClockKind(data["kind"])
        except (KeyError, TypeError, ValueError) as error:
            raise TimeError(TimeErrorCode.UnknownClockDomain, "clock kind is unknown") from error
        try:
            source_id = ClockSourceId.from_hex(data["source_id"])
            generation = data["generation"]
        except KeyError as error:
            raise TimeError(TimeErrorCode.InvalidClockIdentity, "clock identity is incomplete") from error
        return cls(kind, source_id, generation)


def _same_clock(left: ClockIdentity, right: ClockIdentity) -> None:
    if left.kind is not right.kind:
        raise TimeError(TimeErrorCode.ClockDomainMismatch, "timestamps use different clock domains")
    if left.source_id != right.source_id:
        raise TimeError(TimeErrorCode.ClockSourceMismatch, "timestamps use different clock sources")
    if left.generation != right.generation:
        raise TimeError(TimeErrorCode.ClockGenerationMismatch, "timestamps use different clock generations")


@dataclass(frozen=True)
class Timestamp:
    clock: ClockIdentity
    nanoseconds: int

    def __post_init__(self) -> None:
        if not isinstance(self.clock, ClockIdentity):
            raise TimeError(TimeErrorCode.InvalidTimestamp, "timestamp clock is invalid")
        value = _integer(
            self.nanoseconds,
            name="timestamp nanoseconds",
            minimum=_I64_MIN,
            maximum=_I64_MAX,
            range_code=TimeErrorCode.TimestampOutOfRange,
        )
        object.__setattr__(self, "nanoseconds", value)

    def checked_compare(self, other: Timestamp) -> int:
        return checked_compare(self, other)

    def checked_difference(self, other: Timestamp) -> int:
        return checked_difference(self, other)

    def checked_duration_since(self, earlier: Timestamp) -> int:
        return checked_duration_since(self, earlier)

    def checked_add_nanoseconds(self, nanoseconds: int) -> Timestamp:
        return checked_add_nanoseconds(self, nanoseconds)

    def to_dict(self) -> dict[str, Any]:
        return {"clock": self.clock.to_dict(), "nanoseconds": self.nanoseconds}

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), separators=(",", ":"), sort_keys=True)

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> Timestamp:
        data = _mapping(value, name="timestamp")
        try:
            clock = ClockIdentity.from_dict(data["clock"])
            nanoseconds = data["nanoseconds"]
        except KeyError as error:
            raise TimeError(TimeErrorCode.InvalidTimestamp, "timestamp is incomplete") from error
        return cls(clock, nanoseconds)

    @classmethod
    def from_json(cls, value: str | bytes | bytearray) -> Timestamp:
        try:
            decoded = json.loads(value)
        except (TypeError, ValueError) as error:
            raise TimeError(TimeErrorCode.InvalidTimestamp, "timestamp JSON is invalid") from error
        return cls.from_dict(decoded)


def _timestamps(left: Timestamp, right: Timestamp) -> None:
    if not isinstance(left, Timestamp) or not isinstance(right, Timestamp):
        raise TimeError(TimeErrorCode.InvalidTimestamp, "operation requires timestamps")
    _same_clock(left.clock, right.clock)


def checked_compare(left: Timestamp, right: Timestamp) -> int:
    """Compare timestamps only when their complete clock identities match."""
    _timestamps(left, right)
    return (left.nanoseconds > right.nanoseconds) - (left.nanoseconds < right.nanoseconds)


def checked_difference(left: Timestamp, right: Timestamp) -> int:
    """Return signed left-minus-right nanoseconds, checked as an i64."""
    _timestamps(left, right)
    difference = left.nanoseconds - right.nanoseconds
    if difference < _I64_MIN or difference > _I64_MAX:
        raise TimeError(TimeErrorCode.TimestampOverflow, "timestamp difference exceeds i64")
    return difference


def checked_duration_since(later: Timestamp, earlier: Timestamp) -> int:
    """Return a non-negative duration in nanoseconds, bounded to i64."""
    difference = checked_difference(later, earlier)
    if difference < 0:
        raise TimeError(TimeErrorCode.NegativeDuration, "earlier timestamp follows later timestamp")
    return difference


def checked_add_nanoseconds(timestamp: Timestamp, nanoseconds: int) -> Timestamp:
    if not isinstance(timestamp, Timestamp):
        raise TimeError(TimeErrorCode.InvalidTimestamp, "operation requires a timestamp")
    delta = _integer(
        nanoseconds,
        name="nanosecond delta",
        minimum=_I64_MIN,
        maximum=_I64_MAX,
    )
    result = timestamp.nanoseconds + delta
    if result < _I64_MIN or result > _I64_MAX:
        raise TimeError(TimeErrorCode.TimestampOverflow, "timestamp addition exceeds i64")
    return Timestamp(timestamp.clock, result)


@dataclass(frozen=True)
class TimestampState:
    """A timestamp that is known, unknown, or known but not mapped."""

    kind: str
    timestamp: Timestamp | None = None

    def __post_init__(self) -> None:
        if self.kind not in {"known", "unknown", "unmapped"}:
            raise TimeError(TimeErrorCode.InvalidTimestamp, "timestamp state is invalid")
        if self.kind == "unknown":
            if self.timestamp is not None:
                raise TimeError(TimeErrorCode.InvalidTimestamp, "unknown timestamp state has no timestamp")
        elif not isinstance(self.timestamp, Timestamp):
            raise TimeError(TimeErrorCode.InvalidTimestamp, "timestamp state requires a timestamp")

    @classmethod
    def known(cls, timestamp: Timestamp) -> TimestampState:
        return cls("known", timestamp)

    @classmethod
    def unknown(cls) -> TimestampState:
        return cls("unknown")

    @classmethod
    def unmapped(cls, timestamp: Timestamp) -> TimestampState:
        return cls("unmapped", timestamp)

    def to_dict(self) -> str | dict[str, Any]:
        if self.kind == "unknown":
            return "Unknown"
        variant = "Known" if self.kind == "known" else "Unmapped"
        assert self.timestamp is not None
        return {variant: self.timestamp.to_dict()}

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), separators=(",", ":"), sort_keys=True)

    @classmethod
    def from_dict(cls, value: Any) -> TimestampState:
        if value == "Unknown":
            return cls.unknown()
        if not isinstance(value, Mapping) or len(value) != 1:
            raise TimeError(TimeErrorCode.InvalidTimestamp, "timestamp state encoding is invalid")
        variant, timestamp = next(iter(value.items()))
        if variant == "Known":
            return cls.known(Timestamp.from_dict(timestamp))
        if variant == "Unmapped":
            return cls.unmapped(Timestamp.from_dict(timestamp))
        raise TimeError(TimeErrorCode.InvalidTimestamp, "timestamp state variant is invalid")

    @classmethod
    def from_json(cls, value: str | bytes | bytearray) -> TimestampState:
        try:
            decoded = json.loads(value)
        except (TypeError, ValueError) as error:
            raise TimeError(TimeErrorCode.InvalidTimestamp, "timestamp state JSON is invalid") from error
        return cls.from_dict(decoded)


@dataclass(frozen=True)
class SensorTiming:
    capture: Timestamp | None
    received: Timestamp
    capture_uncertainty_ns: int | None = None
    received_uncertainty_ns: int | None = None

    def __post_init__(self) -> None:
        if self.capture is not None and not isinstance(self.capture, Timestamp):
            raise TimeError(TimeErrorCode.InvalidTimestamp, "capture timestamp is invalid")
        if not isinstance(self.received, Timestamp):
            raise TimeError(TimeErrorCode.InvalidTimestamp, "received timestamp is invalid")
        object.__setattr__(
            self,
            "capture_uncertainty_ns",
            _optional_uncertainty(self.capture_uncertainty_ns, name="capture uncertainty"),
        )
        object.__setattr__(
            self,
            "received_uncertainty_ns",
            _optional_uncertainty(self.received_uncertainty_ns, name="received uncertainty"),
        )

    @property
    def capture_state(self) -> TimestampState:
        return TimestampState.unknown() if self.capture is None else TimestampState.known(self.capture)

    @property
    def received_state(self) -> TimestampState:
        return TimestampState.known(self.received)

    @classmethod
    def from_legacy(cls, received: Timestamp, received_uncertainty_ns: int | None = None) -> SensorTiming:
        """Represent a legacy sample without inventing a capture timestamp."""
        return cls(None, received, None, received_uncertainty_ns)


def _optional_uncertainty(value: Any, *, name: str) -> int | None:
    if value is None:
        return None
    return _integer(
        value,
        name=name,
        minimum=0,
        maximum=_U64_MAX,
        code=TimeErrorCode.InvalidTimestamp,
    )


@dataclass(frozen=True)
class ClockDiscontinuity:
    previous: ClockIdentity
    current: ClockIdentity
    event_kind: str | None = None

    def __post_init__(self) -> None:
        if not isinstance(self.previous, ClockIdentity) or not isinstance(self.current, ClockIdentity):
            raise TimeError(TimeErrorCode.TimeSourceReset, "discontinuity identities are invalid")
        event_kind = self.event_kind
        if event_kind is None:
            event_kind = {
                ClockKind.DEVICE: "device_reset",
                ClockKind.SYSTEM_REALTIME: "realtime_step",
            }.get(self.previous.kind, "generation_change")
        valid_event_kinds = ("device_reset", "realtime_step", "external_step", "generation_change")
        if event_kind not in valid_event_kinds:
            raise TimeError(TimeErrorCode.TimeSourceReset, "discontinuity event kind is invalid")
        if event_kind == "external_step":
            if self.previous != self.current:
                raise TimeError(TimeErrorCode.TimeSourceReset, "external step must preserve one exact clock identity")
        elif (
            self.previous.kind is not self.current.kind
            or self.previous.source_id != self.current.source_id
            or self.previous.generation == _U64_MAX
            or self.current.generation != self.previous.generation + 1
        ):
            raise TimeError(TimeErrorCode.TimeSourceReset, "discontinuity must advance one clock generation")
        elif event_kind == "device_reset" and self.previous.kind is not ClockKind.DEVICE:
            raise TimeError(TimeErrorCode.TimeSourceReset, "device reset requires a device clock")
        elif event_kind == "realtime_step" and self.previous.kind is not ClockKind.SYSTEM_REALTIME:
            raise TimeError(TimeErrorCode.TimeSourceReset, "realtime step requires a system realtime clock")
        object.__setattr__(self, "event_kind", event_kind)

    @classmethod
    def external_step(cls, clock: ClockIdentity) -> ClockDiscontinuity:
        """Create a caller-confirmed external step affecting one exact identity."""
        return cls(clock, clock, "external_step")

    def affects(self, clock: ClockIdentity) -> bool:
        return clock == self.previous or clock == self.current


class DeviceClock:
    """Explicitly stamped device clock; reset is caller-confirmed."""

    def __init__(
        self,
        source: ClockSourceId | ClockIdentity | uuid.UUID | bytes | bytearray | memoryview | str,
        generation: int = 0,
    ) -> None:
        if isinstance(source, ClockIdentity):
            explicit_generation = _integer(
                generation,
                name="clock generation",
                minimum=0,
                maximum=_U64_MAX,
            )
            if source.kind is not ClockKind.DEVICE:
                raise TimeError(TimeErrorCode.UnknownClockDomain, "DeviceClock requires a device identity")
            if explicit_generation != 0:
                raise TimeError(TimeErrorCode.InvalidTimestamp, "generation is already part of the identity")
            identity = source
            identity.source_id.device_uuid()
        else:
            if isinstance(source, ClockSourceId):
                source_id = source
            else:
                source_id = ClockSourceId.from_device_uuid(source)
            source_id.device_uuid()
            identity = ClockIdentity(ClockKind.DEVICE, source_id, generation)
        self._identity = identity
        self._lock = threading.Lock()

    @property
    def identity(self) -> ClockIdentity:
        """Return the current immutable identity without taking the reset lock.

        A concurrent reset may make this read observe either the old or new
        identity. Drivers must serialize their hardware events with resets.
        """
        return self._identity

    def stamp(self, nanoseconds: int) -> Timestamp:
        return Timestamp(self.identity, nanoseconds)

    def confirm_reset(self) -> ClockDiscontinuity:
        with self._lock:
            previous = self._identity
            if previous.generation == _U64_MAX:
                raise TimeError(TimeErrorCode.TimestampOverflow, "clock generation is exhausted")
            current = ClockIdentity(previous.kind, previous.source_id, previous.generation + 1)
            self._identity = current
        return ClockDiscontinuity(previous, current)

    def reset(self) -> ClockDiscontinuity:
        """Alias for an explicit, caller-confirmed device reset."""
        return self.confirm_reset()


def _read_boot_uuid() -> uuid.UUID:
    try:
        with open("/proc/sys/kernel/random/boot_id", "rt", encoding="ascii") as stream:
            raw = stream.read()
    except OSError as error:
        raise TimeError(TimeErrorCode.ClockUnavailable, "Linux boot UUID is unavailable") from error
    except UnicodeError as error:
        raise TimeError(TimeErrorCode.UnknownClockDomain, "Linux boot UUID is invalid") from error
    if not isinstance(raw, str):
        raise TimeError(TimeErrorCode.UnknownClockDomain, "Linux boot UUID is invalid")
    value = raw[:-1] if raw.endswith("\n") else raw
    try:
        boot_uuid = uuid.UUID(value)
    except (TypeError, ValueError) as error:
        raise TimeError(TimeErrorCode.UnknownClockDomain, "Linux boot UUID is invalid") from error
    if str(boot_uuid) != value or boot_uuid.int == 0:
        raise TimeError(TimeErrorCode.UnknownClockDomain, "Linux boot UUID is invalid")
    return boot_uuid


def _read_namespace_stat() -> os.stat_result:
    try:
        return os.stat("/proc/thread-self/ns/time")
    except OSError as error:
        raise TimeError(TimeErrorCode.ClockUnavailable, "current thread time namespace is unavailable") from error


def _read_monotonic_offset() -> tuple[int, int]:
    try:
        thread_path = os.readlink("/proc/thread-self").strip("/").split("/")
    except OSError as error:
        raise TimeError(TimeErrorCode.ClockUnavailable, "current thread proc path is unavailable") from error
    if len(thread_path) < 3 or thread_path[-2] != "task":
        raise TimeError(TimeErrorCode.UnknownClockDomain, "current thread proc path is invalid")
    try:
        thread_id = int(thread_path[-1], 10)
    except ValueError as error:
        raise TimeError(TimeErrorCode.UnknownClockDomain, "current thread id is invalid") from error
    if thread_id <= 0:
        raise TimeError(TimeErrorCode.UnknownClockDomain, "current thread id is invalid")
    try:
        with open(f"/proc/{thread_id}/timens_offsets", "rt", encoding="ascii") as stream:
            lines = stream.read().splitlines()
    except OSError as error:
        raise TimeError(TimeErrorCode.UnknownClockDomain, "Linux monotonic namespace offset basis is unavailable") from error
    found: tuple[int, int] | None = None
    for line in lines:
        parts = line.split()
        if not parts or parts[0] != "monotonic":
            continue
        if len(parts) != 3 or found is not None:
            raise TimeError(TimeErrorCode.UnknownClockDomain, "Linux monotonic namespace offset is invalid")
        try:
            seconds, nanoseconds = int(parts[1], 10), int(parts[2], 10)
        except ValueError as error:
            raise TimeError(TimeErrorCode.UnknownClockDomain, "Linux monotonic namespace offset is invalid") from error
        if not (_I64_MIN <= seconds <= _I64_MAX and _I64_MIN <= nanoseconds <= _I64_MAX):
            raise TimeError(TimeErrorCode.UnknownClockDomain, "Linux monotonic namespace offset is out of range")
        found = seconds, nanoseconds
    if found is None:
        raise TimeError(TimeErrorCode.UnknownClockDomain, "Linux monotonic namespace offset basis is unknown")
    return found


class HostClock:
    """Host monotonic and realtime clocks tied to Linux boot and namespace IDs."""

    def __init__(self) -> None:
        self._boot_uuid = _read_boot_uuid()
        namespace_before = _read_namespace_stat()
        offset_seconds, offset_nanoseconds = _read_monotonic_offset()
        namespace_after = _read_namespace_stat()
        if (
            int(namespace_before.st_dev) != int(namespace_after.st_dev)
            or int(namespace_before.st_ino) != int(namespace_after.st_ino)
        ):
            raise TimeError(TimeErrorCode.TimeSourceReset, "current thread time namespace changed during initialization")
        self._namespace_device = int(namespace_before.st_dev)
        self._namespace_inode = int(namespace_before.st_ino)
        self._monotonic_identity = ClockIdentity(
            ClockKind.HOST_MONOTONIC,
            ClockSourceId.for_host_monotonic(
                self._boot_uuid,
                self._namespace_device,
                self._namespace_inode,
                offset_seconds,
                offset_nanoseconds,
            ),
            0,
        )
        self._realtime_source = ClockSourceId.for_system_realtime(self._boot_uuid)
        self._realtime_generation = 0
        self._lock = threading.Lock()

    @property
    def monotonic_identity(self) -> ClockIdentity:
        return self._monotonic_identity

    @property
    def realtime_identity(self) -> ClockIdentity:
        with self._lock:
            return ClockIdentity(
                ClockKind.SYSTEM_REALTIME,
                self._realtime_source,
                self._realtime_generation,
            )

    def monotonic_now(self) -> Timestamp:
        before = _read_namespace_stat()
        self._check_namespace(before)
        try:
            nanoseconds = _time.clock_gettime_ns(_time.CLOCK_MONOTONIC)
        except (AttributeError, OSError) as error:
            raise TimeError(TimeErrorCode.ClockUnavailable, "CLOCK_MONOTONIC is unavailable") from error
        after = _read_namespace_stat()
        self._check_namespace(after)
        return Timestamp(self._monotonic_identity, nanoseconds)

    def realtime_now(self) -> Timestamp:
        generation_before = self._realtime_generation
        try:
            nanoseconds = _time.clock_gettime_ns(_time.CLOCK_REALTIME)
        except (AttributeError, OSError) as error:
            raise TimeError(TimeErrorCode.ClockUnavailable, "CLOCK_REALTIME is unavailable") from error
        generation_after = self._realtime_generation
        if generation_before != generation_after:
            raise TimeError(TimeErrorCode.TimeSourceReset, "system realtime generation changed during read")
        return Timestamp(
            ClockIdentity(
                ClockKind.SYSTEM_REALTIME,
                self._realtime_source,
                generation_before,
            ),
            nanoseconds,
        )

    def confirm_realtime_reset(self) -> ClockDiscontinuity:
        with self._lock:
            if self._realtime_generation == _U64_MAX:
                raise TimeError(TimeErrorCode.TimestampOverflow, "clock generation is exhausted")
            previous = ClockIdentity(
                ClockKind.SYSTEM_REALTIME,
                self._realtime_source,
                self._realtime_generation,
            )
            self._realtime_generation += 1
            current = ClockIdentity(
                ClockKind.SYSTEM_REALTIME,
                self._realtime_source,
                self._realtime_generation,
            )
        return ClockDiscontinuity(previous, current)

    def _check_namespace(self, namespace: os.stat_result) -> None:
        if int(namespace.st_dev) != self._namespace_device or int(namespace.st_ino) != self._namespace_inode:
            raise TimeError(TimeErrorCode.TimeSourceReset, "current thread time namespace changed")


@dataclass(frozen=True)
class ClockMappingCalibration:
    source: ClockIdentity
    target: ClockIdentity
    source_anchor_ns: int
    target_anchor_ns: int
    rate_numerator: int
    rate_denominator: int
    valid_from_ns: int
    valid_until_ns: int
    calibration_id: bytes
    calibrated_at: Timestamp
    expires_at: Timestamp
    uncertainty_ns: int | None = None

    def __post_init__(self) -> None:
        if not isinstance(self.source, ClockIdentity) or not isinstance(self.target, ClockIdentity):
            raise TimeError(TimeErrorCode.ClockMappingInvalid, "mapping clock identity is invalid")
        for field_name in ("source_anchor_ns", "target_anchor_ns", "valid_from_ns", "valid_until_ns"):
            object.__setattr__(
                self,
                field_name,
                _integer(
                    getattr(self, field_name),
                    name=field_name,
                    minimum=_I64_MIN,
                    maximum=_I64_MAX,
                    code=TimeErrorCode.ClockMappingInvalid,
                ),
            )
        numerator = _integer(
            self.rate_numerator,
            name="rate numerator",
            minimum=1,
            maximum=_U64_MAX,
            code=TimeErrorCode.InvalidConversionRate,
        )
        denominator = _integer(
            self.rate_denominator,
            name="rate denominator",
            minimum=1,
            maximum=_U64_MAX,
            code=TimeErrorCode.InvalidConversionRate,
        )
        object.__setattr__(self, "rate_numerator", numerator)
        object.__setattr__(self, "rate_denominator", denominator)
        if self.valid_from_ns > self.valid_until_ns:
            raise TimeError(TimeErrorCode.ClockMappingInvalid, "mapping source range is reversed")
        calibration_id = _bytes(
            self.calibration_id,
            name="calibration id",
            size=16,
            code=TimeErrorCode.InvalidCalibrationEvidence,
        )
        if not any(calibration_id):
            raise TimeError(TimeErrorCode.InvalidCalibrationEvidence, "calibration id cannot be zero")
        object.__setattr__(self, "calibration_id", calibration_id)
        if not isinstance(self.calibrated_at, Timestamp) or not isinstance(self.expires_at, Timestamp):
            raise TimeError(TimeErrorCode.ClockMappingInvalid, "calibration lifetime timestamps are invalid")
        for stamp in (self.calibrated_at, self.expires_at):
            if stamp.clock.kind is not ClockKind.HOST_MONOTONIC:
                raise TimeError(
                    TimeErrorCode.InvalidCalibrationEvidence,
                    "calibration lifetime must use host monotonic time",
                )
        try:
            _same_clock(self.calibrated_at.clock, self.expires_at.clock)
        except TimeError as error:
            raise TimeError(
                TimeErrorCode.InvalidCalibrationEvidence,
                "calibration lifetime clocks must match",
            ) from error
        if self.calibrated_at.nanoseconds >= self.expires_at.nanoseconds:
            raise TimeError(
                TimeErrorCode.InvalidCalibrationEvidence,
                "calibration expiry must follow calibration time",
            )
        object.__setattr__(
            self,
            "uncertainty_ns",
            _optional_uncertainty(self.uncertainty_ns, name="mapping uncertainty"),
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "source": self.source.to_dict(),
            "target": self.target.to_dict(),
            "source_anchor_ns": self.source_anchor_ns,
            "target_anchor_ns": self.target_anchor_ns,
            "rate_numerator": self.rate_numerator,
            "rate_denominator": self.rate_denominator,
            "valid_from_ns": self.valid_from_ns,
            "valid_until_ns": self.valid_until_ns,
            "calibration_id": list(self.calibration_id),
            "calibrated_at": self.calibrated_at.to_dict(),
            "expires_at": self.expires_at.to_dict(),
            "uncertainty_ns": self.uncertainty_ns,
        }

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), separators=(",", ":"), sort_keys=True)

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> ClockMappingCalibration:
        data = _mapping(
            value,
            name="clock mapping calibration",
            code=TimeErrorCode.ClockMappingInvalid,
        )
        try:
            calibration_id_value = data["calibration_id"]
            if not isinstance(calibration_id_value, (list, tuple)):
                raise TimeError(
                    TimeErrorCode.InvalidCalibrationEvidence,
                    "calibration id must be a 16-byte array",
                )
            calibration_id = bytes(
                _integer(
                    item,
                    name="calibration id byte",
                    minimum=0,
                    maximum=0xFF,
                    code=TimeErrorCode.InvalidCalibrationEvidence,
                )
                for item in calibration_id_value
            )
            return cls(
                source=ClockIdentity.from_dict(data["source"]),
                target=ClockIdentity.from_dict(data["target"]),
                source_anchor_ns=data["source_anchor_ns"],
                target_anchor_ns=data["target_anchor_ns"],
                rate_numerator=data["rate_numerator"],
                rate_denominator=data["rate_denominator"],
                valid_from_ns=data["valid_from_ns"],
                valid_until_ns=data["valid_until_ns"],
                calibration_id=calibration_id,
                calibrated_at=Timestamp.from_dict(data["calibrated_at"]),
                expires_at=Timestamp.from_dict(data["expires_at"]),
                uncertainty_ns=data.get("uncertainty_ns"),
            )
        except KeyError as error:
            raise TimeError(TimeErrorCode.ClockMappingInvalid, "calibration is incomplete") from error

    @classmethod
    def from_json(cls, value: str | bytes | bytearray) -> ClockMappingCalibration:
        try:
            decoded = json.loads(value)
        except (TypeError, ValueError) as error:
            raise TimeError(TimeErrorCode.ClockMappingInvalid, "calibration JSON is invalid") from error
        return cls.from_dict(decoded)


@dataclass(frozen=True)
class ConvertedTimestamp:
    timestamp: Timestamp
    uncertainty_ns: int

    def __post_init__(self) -> None:
        if not isinstance(self.timestamp, Timestamp):
            raise TimeError(TimeErrorCode.InvalidTimestamp, "converted timestamp is invalid")
        object.__setattr__(
            self,
            "uncertainty_ns",
            _integer(
                self.uncertainty_ns,
                name="converted uncertainty",
                minimum=0,
                maximum=_U64_MAX,
                code=TimeErrorCode.UnknownUncertainty,
            ),
        )


class ClockMapping:
    """A bounded, checked affine mapping backed by one calibration."""

    def __init__(self, calibration: ClockMappingCalibration) -> None:
        if not isinstance(calibration, ClockMappingCalibration):
            raise TimeError(TimeErrorCode.ClockMappingInvalid, "mapping calibration is invalid")
        if calibration.source == calibration.target and (
            calibration.source_anchor_ns != calibration.target_anchor_ns
            or calibration.rate_numerator != calibration.rate_denominator
        ):
            raise TimeError(TimeErrorCode.ClockMappingInvalid, "a clock cannot be mapped to a warped version of itself")
        if calibration.uncertainty_ns is None:
            raise TimeError(TimeErrorCode.UnknownUncertainty, "mapping uncertainty is unknown")
        self._calibration = calibration
        self._invalidated = False
        self._lock = threading.Lock()

    @property
    def calibration(self) -> ClockMappingCalibration:
        return self._calibration

    @property
    def invalidated(self) -> bool:
        """Read the invalidation flag without locking on per-sample conversion."""
        return self._invalidated

    def invalidate(self, discontinuity: ClockDiscontinuity) -> bool:
        if not isinstance(discontinuity, ClockDiscontinuity):
            raise TimeError(TimeErrorCode.ClockMappingInvalid, "mapping invalidation requires a discontinuity")
        calibration = self.calibration
        matches_source = discontinuity.affects(calibration.source)
        matches_target = discontinuity.affects(calibration.target)
        if matches_source or matches_target:
            with self._lock:
                self._invalidated = True
            return True
        return False

    def convert(
        self,
        input: Timestamp,
        input_uncertainty_ns: int | None,
        requested_target: ClockIdentity,
        now: Timestamp,
    ) -> ConvertedTimestamp:
        if not isinstance(input, Timestamp) or not isinstance(requested_target, ClockIdentity):
            raise TimeError(TimeErrorCode.InvalidTimestamp, "mapping input is invalid")
        if not isinstance(now, Timestamp):
            raise TimeError(TimeErrorCode.ClockMappingInvalid, "mapping time is invalid")
        if self.invalidated:
            raise TimeError(TimeErrorCode.MappingInvalidated, "clock mapping was invalidated")
        calibration = self.calibration
        _same_clock(input.clock, calibration.source)
        _same_clock(requested_target, calibration.target)
        _same_clock(now.clock, calibration.calibrated_at.clock)
        if now.nanoseconds < calibration.calibrated_at.nanoseconds:
            raise TimeError(TimeErrorCode.ClockMappingUnavailable, "mapping calibration is not active yet")
        if now.nanoseconds >= calibration.expires_at.nanoseconds:
            raise TimeError(TimeErrorCode.ClockMappingExpired, "clock mapping calibration has expired")
        if not calibration.valid_from_ns <= input.nanoseconds <= calibration.valid_until_ns:
            raise TimeError(TimeErrorCode.TimestampOutOfRange, "timestamp is outside the calibrated source range")
        input_uncertainty = _required_uncertainty(input_uncertainty_ns, name="input uncertainty")
        delta = input.nanoseconds - calibration.source_anchor_ns
        product = delta * calibration.rate_numerator
        _check_i128(product, "timestamp conversion product")
        quotient, remainder = _trunc_divmod(product, calibration.rate_denominator)
        mapped = calibration.target_anchor_ns + quotient
        _check_i128(mapped, "timestamp conversion result")
        if mapped < _I64_MIN or mapped > _I64_MAX:
            raise TimeError(TimeErrorCode.TimestampOverflow, "converted timestamp exceeds i64")

        uncertainty_product = input_uncertainty * calibration.rate_numerator
        if uncertainty_product > _U128_MAX:
            raise TimeError(TimeErrorCode.TimestampOverflow, "uncertainty conversion product exceeds u128")
        scaled_uncertainty, uncertainty_remainder = divmod(
            uncertainty_product,
            calibration.rate_denominator,
        )
        if uncertainty_remainder:
            scaled_uncertainty += 1
        output_uncertainty = (
            scaled_uncertainty
            + calibration.uncertainty_ns
            + (1 if remainder else 0)
        )
        if output_uncertainty > _U64_MAX:
            raise TimeError(TimeErrorCode.TimestampOverflow, "converted uncertainty exceeds u64")
        return ConvertedTimestamp(Timestamp(requested_target, mapped), output_uncertainty)


def _required_uncertainty(value: Any, *, name: str) -> int:
    if value is None:
        raise TimeError(TimeErrorCode.UnknownUncertainty, f"{name} is unknown")
    return _integer(
        value,
        name=name,
        minimum=0,
        maximum=_U64_MAX,
        code=TimeErrorCode.UnknownUncertainty,
    )


def _check_i128(value: int, name: str) -> None:
    if value < _I128_MIN or value > _I128_MAX:
        raise TimeError(TimeErrorCode.TimestampOverflow, f"{name} exceeds signed i128")


def _trunc_divmod(numerator: int, denominator: int) -> tuple[int, int]:
    magnitude = abs(numerator)
    quotient, remainder = divmod(magnitude, denominator)
    if numerator < 0:
        quotient = -quotient
        remainder = -remainder
    return quotient, remainder


__all__ = [
    "ClockDiscontinuity",
    "ClockIdentity",
    "ClockKind",
    "ClockMapping",
    "ClockMappingCalibration",
    "ClockSourceId",
    "ConvertedTimestamp",
    "DeviceClock",
    "HostClock",
    "SensorTiming",
    "TimeError",
    "TimeErrorCode",
    "Timestamp",
    "TimestampState",
    "checked_add_nanoseconds",
    "checked_compare",
    "checked_difference",
    "checked_duration_since",
]

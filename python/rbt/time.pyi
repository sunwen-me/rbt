from dataclasses import dataclass
from enum import Enum
from typing import Any, Mapping
from uuid import UUID

class TimeErrorCode(str, Enum):
    ClockDomainMismatch: TimeErrorCode
    ClockSourceMismatch: TimeErrorCode
    ClockGenerationMismatch: TimeErrorCode
    UnknownClockDomain: TimeErrorCode
    ClockMappingUnavailable: TimeErrorCode
    ClockMappingExpired: TimeErrorCode
    ClockMappingInvalid: TimeErrorCode
    MappingInvalidated: TimeErrorCode
    TimestampOutOfRange: TimeErrorCode
    TimestampOverflow: TimeErrorCode
    InvalidTimestamp: TimeErrorCode
    InvalidConversionRate: TimeErrorCode
    UnknownUncertainty: TimeErrorCode
    TimeSourceReset: TimeErrorCode
    ClockUnavailable: TimeErrorCode
    InvalidCalibrationEvidence: TimeErrorCode
    InvalidClockSourceId: TimeErrorCode
    InvalidClockIdentity: TimeErrorCode
    NegativeDuration: TimeErrorCode

class TimeError(Exception):
    code: TimeErrorCode
    def __init__(self, code: TimeErrorCode | str, message: str = ...) -> None: ...

class ClockKind(str, Enum):
    HOST_MONOTONIC: ClockKind
    SYSTEM_REALTIME: ClockKind
    DEVICE: ClockKind

@dataclass(frozen=True)
class ClockSourceId:
    value: bytes
    def __init__(self, value: bytes | bytearray | memoryview) -> None: ...
    @classmethod
    def from_hex(cls, value: str) -> ClockSourceId: ...
    @classmethod
    def from_hex96(cls, value: str) -> ClockSourceId: ...
    @classmethod
    def from_device_uuid(
        cls, value: UUID | bytes | bytearray | memoryview | str
    ) -> ClockSourceId: ...
    @classmethod
    def for_host_monotonic(
        cls,
        boot_uuid: UUID | bytes,
        namespace_device: int,
        namespace_inode: int,
        offset_seconds: int,
        offset_nanoseconds: int,
    ) -> ClockSourceId: ...
    @classmethod
    def for_system_realtime(cls, boot_uuid: UUID | bytes) -> ClockSourceId: ...
    @property
    def hex96(self) -> str: ...
    def to_hex(self) -> str: ...
    def to_bytes(self) -> bytes: ...
    def device_uuid(self) -> UUID: ...

@dataclass(frozen=True)
class ClockIdentity:
    kind: ClockKind
    source_id: ClockSourceId
    generation: int
    def __init__(
        self, kind: ClockKind, source_id: ClockSourceId, generation: int = ...
    ) -> None: ...
    def checked_compatible(self, other: ClockIdentity) -> None: ...
    def to_dict(self) -> dict[str, Any]: ...
    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> ClockIdentity: ...

@dataclass(frozen=True)
class Timestamp:
    clock: ClockIdentity
    nanoseconds: int
    def __init__(self, clock: ClockIdentity, nanoseconds: int) -> None: ...
    def checked_compare(self, other: Timestamp) -> int: ...
    def checked_difference(self, other: Timestamp) -> int: ...
    def checked_duration_since(self, earlier: Timestamp) -> int: ...
    def checked_add_nanoseconds(self, nanoseconds: int) -> Timestamp: ...
    def to_dict(self) -> dict[str, Any]: ...
    def to_json(self) -> str: ...
    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> Timestamp: ...
    @classmethod
    def from_json(cls, value: str | bytes | bytearray) -> Timestamp: ...

def checked_compare(left: Timestamp, right: Timestamp) -> int: ...
def checked_difference(left: Timestamp, right: Timestamp) -> int: ...
def checked_duration_since(later: Timestamp, earlier: Timestamp) -> int: ...
def checked_add_nanoseconds(timestamp: Timestamp, nanoseconds: int) -> Timestamp: ...

@dataclass(frozen=True)
class TimestampState:
    kind: str
    timestamp: Timestamp | None = ...
    def __init__(self, kind: str, timestamp: Timestamp | None = ...) -> None: ...
    @classmethod
    def known(cls, timestamp: Timestamp) -> TimestampState: ...
    @classmethod
    def unknown(cls) -> TimestampState: ...
    @classmethod
    def unmapped(cls, timestamp: Timestamp) -> TimestampState: ...
    def to_dict(self) -> str | dict[str, Any]: ...
    def to_json(self) -> str: ...
    @classmethod
    def from_dict(cls, value: Any) -> TimestampState: ...
    @classmethod
    def from_json(cls, value: str | bytes | bytearray) -> TimestampState: ...

@dataclass(frozen=True)
class SensorTiming:
    capture: Timestamp | None
    received: Timestamp
    capture_uncertainty_ns: int | None
    received_uncertainty_ns: int | None
    def __init__(
        self,
        capture: Timestamp | None,
        received: Timestamp,
        capture_uncertainty_ns: int | None = ...,
        received_uncertainty_ns: int | None = ...,
    ) -> None: ...
    @property
    def capture_state(self) -> TimestampState: ...
    @property
    def received_state(self) -> TimestampState: ...
    @classmethod
    def from_legacy(
        cls,
        received: Timestamp,
        received_uncertainty_ns: int | None = ...,
    ) -> SensorTiming: ...

@dataclass(frozen=True)
class ClockDiscontinuity:
    previous: ClockIdentity
    current: ClockIdentity
    event_kind: str
    def __init__(
        self,
        previous: ClockIdentity,
        current: ClockIdentity,
        event_kind: str | None = ...,
    ) -> None: ...
    @classmethod
    def external_step(cls, clock: ClockIdentity) -> ClockDiscontinuity: ...
    def affects(self, clock: ClockIdentity) -> bool: ...

class DeviceClock:
    def __init__(
        self,
        source: ClockSourceId | ClockIdentity | UUID | bytes | bytearray | memoryview | str,
        generation: int = ...,
    ) -> None: ...
    @property
    def identity(self) -> ClockIdentity: ...
    def stamp(self, nanoseconds: int) -> Timestamp: ...
    def confirm_reset(self) -> ClockDiscontinuity: ...
    def reset(self) -> ClockDiscontinuity: ...

class HostClock:
    def __init__(self) -> None: ...
    @property
    def monotonic_identity(self) -> ClockIdentity: ...
    @property
    def realtime_identity(self) -> ClockIdentity: ...
    def monotonic_now(self) -> Timestamp: ...
    def realtime_now(self) -> Timestamp: ...
    def confirm_realtime_reset(self) -> ClockDiscontinuity: ...

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
    uncertainty_ns: int | None
    def __init__(
        self,
        source: ClockIdentity,
        target: ClockIdentity,
        source_anchor_ns: int,
        target_anchor_ns: int,
        rate_numerator: int,
        rate_denominator: int,
        valid_from_ns: int,
        valid_until_ns: int,
        calibration_id: bytes,
        calibrated_at: Timestamp,
        expires_at: Timestamp,
        uncertainty_ns: int | None = ...,
    ) -> None: ...
    def to_dict(self) -> dict[str, Any]: ...
    def to_json(self) -> str: ...
    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> ClockMappingCalibration: ...
    @classmethod
    def from_json(cls, value: str | bytes | bytearray) -> ClockMappingCalibration: ...

@dataclass(frozen=True)
class ConvertedTimestamp:
    timestamp: Timestamp
    uncertainty_ns: int
    def __init__(self, timestamp: Timestamp, uncertainty_ns: int) -> None: ...

class ClockMapping:
    calibration: ClockMappingCalibration
    def __init__(self, calibration: ClockMappingCalibration) -> None: ...
    @property
    def invalidated(self) -> bool: ...
    def invalidate(self, discontinuity: ClockDiscontinuity) -> bool: ...
    def convert(
        self,
        input: Timestamp,
        input_uncertainty_ns: int | None,
        requested_target: ClockIdentity,
        now: Timestamp,
    ) -> ConvertedTimestamp: ...

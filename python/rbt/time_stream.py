"""Zero-copy stamped byte streams with a strict in-sample timing prefix."""

from __future__ import annotations

import json
import math
import operator
import sys
from typing import Any

from . import (
    MessageTypeDescriptor,
    StreamDescriptor,
    StreamProfile,
    StreamPressure,
    StreamResourceConfig,
    TransportFeatures,
    _effective_resources,
    _nonnegative_index,
    _rbt,
    _u64,
)
from .time import ClockIdentity, ClockKind, ClockSourceId, SensorTiming, Timestamp

TIMING_PREFIX_LEN = 192
_MAGIC = b"RBTTIME1"
_U64_MAX = (1 << 64) - 1
_I64_MIN = -(1 << 63)
_I64_MAX = (1 << 63) - 1
_BLOCKING_WAIT_SECONDS = 86_400.0


def _source_bytes(clock: ClockIdentity) -> bytes:
    source = clock.source_id.value
    if len(source) != 48 or not any(source[:16]):
        raise ValueError("clock source id has an invalid encoding")
    if clock.kind is ClockKind.HOST_MONOTONIC:
        offset_nanoseconds = int.from_bytes(source[40:48], "little", signed=True)
        if not 0 <= offset_nanoseconds <= 999_999_999:
            raise ValueError("monotonic clock source has an invalid namespace offset")
    elif any(source[16:]):
        raise ValueError("realtime and device clock sources require a zero-filled tail")
    return source


def _encode_timestamp(timestamp: Timestamp, output: bytearray, offset: int) -> None:
    if not isinstance(timestamp, Timestamp):
        raise TypeError("timing timestamps must be Timestamp values")
    clock = timestamp.clock
    if not isinstance(clock, ClockIdentity):
        raise ValueError("timestamp clock identity is invalid")
    kind = {
        ClockKind.HOST_MONOTONIC: 1,
        ClockKind.SYSTEM_REALTIME: 2,
        ClockKind.DEVICE: 3,
    }.get(clock.kind)
    if kind is None:
        raise ValueError("timestamp clock kind is invalid")
    source = _source_bytes(clock)
    generation = _checked_integer(clock.generation, "clock generation", 0, _U64_MAX)
    nanoseconds = _checked_integer(timestamp.nanoseconds, "nanoseconds", _I64_MIN, _I64_MAX)
    output[offset] = kind
    output[offset + 8 : offset + 56] = source
    output[offset + 56 : offset + 64] = generation.to_bytes(8, "little")
    output[offset + 64 : offset + 72] = nanoseconds.to_bytes(8, "little", signed=True)


def _decode_timestamp(encoded: memoryview) -> Timestamp:
    if len(encoded) != 72:
        raise ValueError("timestamp record must be exactly 72 bytes")
    if any(encoded[1:8]):
        raise ValueError("timestamp reserved bytes must be zero")
    kind = {
        1: ClockKind.HOST_MONOTONIC,
        2: ClockKind.SYSTEM_REALTIME,
        3: ClockKind.DEVICE,
    }.get(encoded[0])
    if kind is None:
        raise ValueError(f"timestamp kind {encoded[0]} is invalid")
    source = bytes(encoded[8:56])
    source_id = ClockSourceId(source)
    identity = ClockIdentity(
        kind,
        source_id,
        int.from_bytes(encoded[56:64], "little"),
    )
    _source_bytes(identity)
    nanoseconds = int.from_bytes(encoded[64:72], "little", signed=True)
    return Timestamp(identity, nanoseconds)


def _checked_integer(value: Any, name: str, minimum: int, maximum: int) -> int:
    if isinstance(value, bool):
        raise TypeError(f"{name} must be an integer")
    try:
        result = operator.index(value)
    except TypeError as error:
        raise TypeError(f"{name} must be an integer") from error
    if result < minimum or result > maximum:
        raise ValueError(f"{name} is outside its supported range")
    return int(result)


def encode_timing_prefix(timing: SensorTiming) -> bytes:
    """Encode one strict, fixed-width timing record."""
    if not isinstance(timing, SensorTiming):
        raise TypeError("timing must be a SensorTiming")
    if timing.capture is None and timing.capture_uncertainty_ns is not None:
        raise ValueError("capture uncertainty cannot be known when capture time is absent")
    output = bytearray(TIMING_PREFIX_LEN)
    output[:8] = _MAGIC
    flags = 0
    if timing.capture is not None:
        flags |= 1
        _encode_timestamp(timing.capture, output, 16)
    if timing.capture_uncertainty_ns is not None:
        flags |= 1 << 1
        value = _checked_integer(
            timing.capture_uncertainty_ns, "capture uncertainty", 0, _U64_MAX
        )
        output[160:168] = value.to_bytes(8, "little")
    if timing.received_uncertainty_ns is not None:
        flags |= 1 << 2
        value = _checked_integer(
            timing.received_uncertainty_ns, "received uncertainty", 0, _U64_MAX
        )
        output[168:176] = value.to_bytes(8, "little")
    output[8] = flags
    _encode_timestamp(timing.received, output, 88)
    return bytes(output)


def decode_timing_prefix(prefix: bytes | bytearray | memoryview) -> SensorTiming:
    """Decode one exact 192-byte prefix, rejecting reserved or invalid fields."""
    try:
        encoded = memoryview(prefix).cast("B")
    except (TypeError, ValueError) as error:
        raise TypeError("timing prefix must be a contiguous bytes-like value") from error
    if len(encoded) != TIMING_PREFIX_LEN:
        raise ValueError(
            f"timing prefix must be exactly {TIMING_PREFIX_LEN} bytes, got {len(encoded)}"
        )
    if bytes(encoded[:8]) != _MAGIC:
        raise ValueError("timing prefix magic is invalid")
    if any(encoded[9:16]) or any(encoded[176:192]):
        raise ValueError("timing prefix reserved bytes must be zero")

    flags = encoded[8]
    if flags & ~0b111 or (flags & 0b010 and not flags & 0b001):
        raise ValueError("timing prefix flags are invalid")
    has_capture = bool(flags & 0b001)
    capture_bytes = encoded[16:88]
    if not has_capture and any(capture_bytes):
        raise ValueError("missing capture timestamp must be encoded as 72 zero bytes")
    capture_uncertainty = _optional_u64(encoded, flags, 0b010, 160)
    received_uncertainty = _optional_u64(encoded, flags, 0b100, 168)
    capture = _decode_timestamp(capture_bytes) if has_capture else None
    received = _decode_timestamp(encoded[88:160])
    return SensorTiming(
        capture,
        received,
        capture_uncertainty,
        received_uncertainty,
    )


def _optional_u64(encoded: memoryview, flags: int, flag: int, offset: int) -> int | None:
    value = int.from_bytes(encoded[offset : offset + 8], "little")
    if flags & flag:
        return value
    if value:
        raise ValueError("unknown uncertainty must use a zero value")
    return None


def split_payload(
    payload: bytes | bytearray | memoryview,
) -> tuple[SensorTiming, memoryview]:
    """Return decoded timing and a borrowed memoryview of application bytes."""
    try:
        view = memoryview(payload).cast("B")
    except (TypeError, ValueError) as error:
        raise TypeError("payload must be a contiguous bytes-like value") from error
    if len(view) < TIMING_PREFIX_LEN:
        raise ValueError(
            f"StampedBytes payload must contain a {TIMING_PREFIX_LEN}-byte timing prefix"
        )
    return decode_timing_prefix(view[:TIMING_PREFIX_LEN]), view[TIMING_PREFIX_LEN:]


def stamped_bytes_descriptor(
    service_name: str,
    *,
    profile: StreamProfile = StreamProfile.SENSOR,
    resources: StreamResourceConfig | None = None,
) -> StreamDescriptor:
    """Build a descriptor from the native StampedBytes ABI and service name."""
    if not isinstance(service_name, str) or not service_name.strip():
        raise ValueError("service_name must be non-empty text")
    selected_profile = profile if isinstance(profile, StreamProfile) else StreamProfile(profile)
    selected_resources = resources or StreamResourceConfig()
    if not isinstance(selected_resources, StreamResourceConfig):
        raise TypeError("resources must be a StreamResourceConfig")
    message = MessageTypeDescriptor.from_dict(
        json.loads(_rbt._stamped_bytes_descriptor_json())
    )
    return StreamDescriptor(
        service_name,
        message,
        selected_profile,
        TransportFeatures.sidecar_v1(),
        selected_resources,
    )


class StampedBytesLoan:
    """One publisher loan whose writable application view keeps the loan active."""

    __slots__ = ("_native",)

    def __init__(self, native: Any) -> None:
        self._native = native

    @property
    def sequence(self) -> int:
        return self._native.sequence

    @property
    def payload_nbytes(self) -> int:
        return self._native.payload_nbytes

    def payload_view(self) -> memoryview:
        return memoryview(self._native._new_lease())

    def send(self) -> None:
        self._native.send()

    def _debug_export_count(self) -> int:
        return self._native._debug_export_count()

    def _copy_forbidden(self, *args: Any, **kwargs: Any) -> None:
        raise TypeError("stamped byte loans cannot be copied")

    __copy__ = _copy_forbidden
    __deepcopy__ = _copy_forbidden
    __reduce__ = _copy_forbidden
    __reduce_ex__ = _copy_forbidden


class StampedBytesPublisher:
    """Publisher for one in-sample timing prefix followed by application bytes."""

    __slots__ = ("_native", "descriptor", "maximum_payload_bytes")

    def __init__(self, native: Any, descriptor: StreamDescriptor, maximum: int) -> None:
        self._native = native
        self.descriptor = descriptor
        self.maximum_payload_bytes = maximum

    @classmethod
    def open(
        cls,
        descriptor: StreamDescriptor,
        *,
        maximum_payload_bytes: int = 6_220_800,
        max_loans: int | None = None,
        max_borrowed: int | None = None,
    ) -> StampedBytesPublisher:
        descriptor, resources = _effective_resources(
            descriptor, max_loans=max_loans, max_borrowed=max_borrowed
        )
        maximum = _nonnegative_index(maximum_payload_bytes, "maximum_payload_bytes", ValueError)
        if maximum > sys.maxsize - TIMING_PREFIX_LEN:
            raise ValueError("maximum_payload_bytes exceeds platform address space")
        native = _rbt._StampedBytesPublisher.open(
            descriptor.to_json(),
            maximum + TIMING_PREFIX_LEN,
            max_loans=resources.max_loans,
            max_borrowed=resources.max_borrowed,
        )
        return cls(native, descriptor, maximum)

    @property
    def resources(self) -> StreamResourceConfig:
        return self.descriptor.resources

    @property
    def pressure(self) -> StreamPressure:
        return StreamPressure._from_native(self._native.pressure())

    def loan(
        self,
        timing: SensorTiming,
        payload_bytes: int,
        *,
        sequence: int = 0,
    ) -> StampedBytesLoan:
        if not isinstance(timing, SensorTiming):
            raise TypeError("timing must be a SensorTiming")
        count = _nonnegative_index(payload_bytes, "payload_bytes", ValueError)
        if count > self.maximum_payload_bytes:
            raise ValueError(
                f"payload is {count} bytes, above maximum {self.maximum_payload_bytes}"
            )
        native = self._native.loan(_u64(sequence, "sequence"), count)
        native._set_timing_prefix(encode_timing_prefix(timing))
        return StampedBytesLoan(native)

    def close(self) -> None:
        self._native.close()


class StampedBytesSample:
    """One received sample; its read-only views retain the native borrow."""

    __slots__ = ("_native", "timing")

    def __init__(self, native: Any) -> None:
        self._native = native
        self.timing = decode_timing_prefix(native._timing_prefix())

    @property
    def sequence(self) -> int:
        return self._native.sequence

    @property
    def payload_nbytes(self) -> int:
        return self._native.payload_nbytes

    def payload_view(self) -> memoryview:
        return memoryview(self._native)

    def __repr__(self) -> str:
        return (
            f"<StampedBytesSample seq={self.sequence} "
            f"payload_nbytes={self.payload_nbytes} borrowed=True>"
        )


class StampedBytesSubscriber:
    """Synchronous bounded subscriber for StampedBytes samples."""

    __slots__ = ("_native", "descriptor")

    def __init__(self, native: Any, descriptor: StreamDescriptor) -> None:
        self._native = native
        self.descriptor = descriptor

    @classmethod
    def open(
        cls,
        descriptor: StreamDescriptor,
        *,
        max_borrowed: int | None = None,
    ) -> StampedBytesSubscriber:
        descriptor, resources = _effective_resources(
            descriptor, max_borrowed=max_borrowed
        )
        native = _rbt._StampedBytesSubscriber.open(
            descriptor.to_json(),
            max_loans=resources.max_loans,
            max_borrowed=resources.max_borrowed,
        )
        return cls(native, descriptor)

    @property
    def resources(self) -> StreamResourceConfig:
        return self.descriptor.resources

    @property
    def pressure(self) -> StreamPressure:
        return StreamPressure._from_native(self._native.pressure())

    def try_receive(self) -> StampedBytesSample | None:
        native = self._native.try_receive()
        return None if native is None else StampedBytesSample(native)

    def receive(self, timeout: float | None = None) -> StampedBytesSample | None:
        if timeout is None:
            while True:
                native = self._native.wait_receive(_BLOCKING_WAIT_SECONDS)
                if native is not None:
                    return StampedBytesSample(native)
        if isinstance(timeout, bool):
            raise TypeError("timeout must be a finite non-negative number")
        try:
            seconds = float(timeout)
        except (TypeError, ValueError) as error:
            raise TypeError("timeout must be a finite non-negative number") from error
        if not math.isfinite(seconds) or seconds < 0:
            raise ValueError("timeout must be finite and non-negative")
        if seconds == 0:
            return self.try_receive()
        native = self._native.wait_receive(seconds)
        return None if native is None else StampedBytesSample(native)

    def close(self) -> None:
        self._native.close()


__all__ = [
    "TIMING_PREFIX_LEN",
    "StampedBytesLoan",
    "StampedBytesPublisher",
    "StampedBytesSample",
    "StampedBytesSubscriber",
    "decode_timing_prefix",
    "encode_timing_prefix",
    "split_payload",
    "stamped_bytes_descriptor",
]

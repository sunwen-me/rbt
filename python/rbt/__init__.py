"""Zero-copy NumPy access to rbt shared-memory streams.

Received NumPy views are read-only and retain their native receive owner.
Publisher NumPy views are writable and retain an explicit native loan lease
until every derived alias is gone.
"""

from __future__ import annotations

import asyncio
import dataclasses
import json
import operator
import os
import sys
import threading
import time as _time
from collections.abc import Iterable as IterableABC
from collections.abc import Mapping as MappingABC
from dataclasses import dataclass
from enum import Enum, IntEnum
from typing import Any, Mapping

import numpy as np

from . import _rbt
from ._generated.sensor import (
    IMAGE_DESCRIPTOR as _IMAGE_DESCRIPTOR,
    POINT_CLOUD_DESCRIPTOR as _POINT_CLOUD_DESCRIPTOR,
    POINT_XYZI_DTYPE_SPEC,
)


__version__ = _rbt.__version__

RbtError = _rbt.RbtError
AbiMismatchError = _rbt.AbiMismatchError
BorrowLimitReachedError = _rbt.BorrowLimitReachedError
WaitError = _rbt.WaitError
InvalidPayloadError = _rbt.InvalidPayloadError
InvalidImageLayoutError = _rbt.InvalidImageLayoutError
InvalidPayloadAlignmentError = _rbt.InvalidPayloadAlignmentError
UnsupportedPixelFormatError = _rbt.UnsupportedPixelFormatError
MissingTransportFeatureError = _rbt.MissingTransportFeatureError
LoanPoolExhaustedError = _rbt.LoanPoolExhaustedError
LoanStillMappedError = _rbt.LoanStillMappedError
LoanConsumedError = _rbt.LoanConsumedError
QueueFullError = _rbt.QueueFullError
UnableToDeliverError = _rbt.UnableToDeliverError
ReadinessNotifyError = _rbt.ReadinessNotifyError

InvalidResourceConfigError = getattr(
    _rbt,
    "InvalidResourceConfigError",
    type("InvalidResourceConfigError", (RbtError,), {}),
)
IncompatibleResourceConfigError = getattr(
    _rbt,
    "IncompatibleResourceConfigError",
    type("IncompatibleResourceConfigError", (RbtError,), {}),
)
# Short names mirror the native error variants and make matching the Rust
# control-plane vocabulary convenient while retaining the package's Error
# suffix convention.
InvalidResourceConfig = InvalidResourceConfigError
IncompatibleResourceConfig = IncompatibleResourceConfigError

# These are aliases rather than Python-side exception types so callers can
# catch the same errors raised by the native adapter.  The fallback keeps the
# pure-Python wrapper importable with an older extension while wheels are being
# upgraded; installed extensions expose the authoritative native classes.
ConcurrentReceiveError = getattr(
    _rbt,
    "ConcurrentReceiveError",
    type("ConcurrentReceiveError", (RbtError,), {}),
)
AsyncRuntimeClosedError = getattr(
    _rbt,
    "AsyncRuntimeClosedError",
    type("AsyncRuntimeClosedError", (RbtError,), {}),
)

ComponentRuntimeError = _rbt.ComponentRuntimeError
ComponentPreparationError = _rbt.ComponentPreparationError
ComponentActivationError = _rbt.ComponentActivationError
TaskError = _rbt.TaskError
ActivationExpiredError = _rbt.ActivationExpiredError

RpcError = getattr(_rbt, "RpcError", type("RpcError", (RbtError,), {}))
RpcUnavailableError = getattr(
    _rbt, "RpcUnavailableError", type("RpcUnavailableError", (RpcError,), {})
)
RpcDeadlineExceededError = getattr(
    _rbt,
    "RpcDeadlineExceededError",
    type("RpcDeadlineExceededError", (RpcError,), {}),
)
RpcCancelledError = getattr(
    _rbt, "RpcCancelledError", type("RpcCancelledError", (RpcError,), {})
)
RpcCapacityError = getattr(
    _rbt, "RpcCapacityError", type("RpcCapacityError", (RpcError,), {})
)
RpcActivationExpiredError = getattr(
    _rbt,
    "RpcActivationExpiredError",
    type("RpcActivationExpiredError", (ActivationExpiredError,), {}),
)
RpcTransportError = getattr(
    _rbt, "RpcTransportError", type("RpcTransportError", (RpcError,), {})
)
# Descriptive aliases keep the common server/client vocabulary available while
# preserving one native exception identity for each bounded category.
RpcServerUnavailableError = RpcUnavailableError
RpcClientCapacityError = RpcCapacityError
RpcServerCapacityError = RpcCapacityError
RpcResponseCapacityError = RpcCapacityError


class MessageShape(str, Enum):
    FIXED = "fixed"
    SLICE = "slice"


class StreamProfile(str, Enum):
    SENSOR = "sensor"
    CONTROL = "control"


class ReadinessProtocol(str, Enum):
    NONE = "none"
    SIDECAR_V1 = "sidecar-v1"


class RpcReadinessProtocol(str, Enum):
    NONE = "none"
    RPC_SIDECAR_V1 = "rpc-sidecar-v1"


@dataclass(frozen=True)
class RpcTransportFeatures:
    readiness: RpcReadinessProtocol = RpcReadinessProtocol.NONE

    @classmethod
    def rpc_sidecar_v1(cls) -> RpcTransportFeatures:
        return cls(RpcReadinessProtocol.RPC_SIDECAR_V1)


class PixelFormat(IntEnum):
    RGB8 = 1
    BGR8 = 2
    GRAY8 = 3


def _resource_limit(value: Any, name: str) -> int:
    """Validate one native ``usize`` resource setting.

    Resource values are deliberately stricter than ordinary Python integer
    arguments: booleans are not useful limits, zero would make the endpoint
    unusable, and values larger than the local ``usize`` address range cannot
    be represented by the native adapter.
    """
    if isinstance(value, bool):
        raise TypeError(f"{name} must be an integer")
    try:
        result = operator.index(value)
    except TypeError as error:
        raise TypeError(f"{name} must be an integer") from error
    if result <= 0:
        raise InvalidResourceConfigError(f"{name} must be greater than zero")
    if result > sys.maxsize:
        raise ValueError(f"{name} exceeds platform address space")
    return int(result)


@dataclass(frozen=True)
class StreamResourceConfig:
    """Explicit shared-memory resource ceilings for one stream service."""

    max_loans: int = 2
    max_borrowed: int = 1

    def __post_init__(self) -> None:
        object.__setattr__(self, "max_loans", _resource_limit(self.max_loans, "max_loans"))
        object.__setattr__(
            self, "max_borrowed", _resource_limit(self.max_borrowed, "max_borrowed")
        )

    @classmethod
    def from_dict(cls, value: Mapping[str, Any] | None) -> StreamResourceConfig:
        if value is None:
            return cls()
        if not isinstance(value, MappingABC):
            raise ValueError("StreamDescriptor resources must contain an object")
        return cls(
            max_loans=value.get("max_loans", 2),
            max_borrowed=value.get("max_borrowed", 1),
        )

    def to_dict(self) -> dict[str, int]:
        return {"max_loans": self.max_loans, "max_borrowed": self.max_borrowed}


@dataclass(frozen=True)
class MessageTypeDescriptor:
    namespace: str
    name: str
    abi_major: int
    shape: MessageShape
    header_size: int
    header_align: int
    element_size: int
    element_align: int
    layout_hash: str

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> MessageTypeDescriptor:
        return cls(
            namespace=str(value["namespace"]),
            name=str(value["name"]),
            abi_major=int(value["abi_major"]),
            shape=MessageShape(value["shape"]),
            header_size=int(value["header_size"]),
            header_align=int(value["header_align"]),
            element_size=int(value["element_size"]),
            element_align=int(value["element_align"]),
            layout_hash=str(value["layout_hash"]),
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "namespace": self.namespace,
            "name": self.name,
            "abi_major": self.abi_major,
            "shape": self.shape.value,
            "header_size": self.header_size,
            "header_align": self.header_align,
            "element_size": self.element_size,
            "element_align": self.element_align,
            "layout_hash": self.layout_hash,
        }

    @property
    def qualified_name(self) -> str:
        return f"{self.namespace}/{self.name}@{self.abi_major}"


def _rpc_resource(value: Any, name: str) -> int:
    return _resource_limit(value, name)


@dataclass(frozen=True)
class RpcResourceConfig:
    """Bounded request/response shared-memory capacities."""

    max_in_flight: int = 4
    max_clients: int = 16
    max_request_loans: int = 2
    response_buffer_size: int = 1
    max_borrowed_responses: int = 1
    max_response_loans: int = 1
    max_request_slice_len: int = 1
    max_response_slice_len: int = 1

    def __post_init__(self) -> None:
        for field in dataclasses.fields(self):
            object.__setattr__(self, field.name, _rpc_resource(getattr(self, field.name), field.name))

    @classmethod
    def from_dict(cls, value: Mapping[str, Any] | None) -> RpcResourceConfig:
        if value is None:
            return cls()
        if not isinstance(value, MappingABC):
            raise ValueError("RpcDescriptor resources must contain an object")
        defaults = cls()
        return cls(
            **{
                field.name: value.get(field.name, getattr(defaults, field.name))
                for field in dataclasses.fields(cls)
            }
        )

    def to_dict(self) -> dict[str, int]:
        return {field.name: getattr(self, field.name) for field in dataclasses.fields(self)}


@dataclass(frozen=True)
class TransportFeatures:
    readiness: ReadinessProtocol = ReadinessProtocol.NONE

    @classmethod
    def sidecar_v1(cls) -> TransportFeatures:
        return cls(ReadinessProtocol.SIDECAR_V1)


@dataclass(frozen=True)
class StreamDescriptor:
    service_name: str
    message_type: MessageTypeDescriptor
    profile: StreamProfile
    transport: TransportFeatures = TransportFeatures()
    resources: StreamResourceConfig = StreamResourceConfig()

    @classmethod
    def from_json(cls, value: str | bytes | bytearray) -> StreamDescriptor:
        parsed = json.loads(value)
        if not isinstance(parsed, dict):
            raise ValueError("StreamDescriptor JSON must contain an object")
        transport_value = parsed.get("transport", {})
        if not isinstance(transport_value, dict):
            raise ValueError("StreamDescriptor transport must contain an object")
        return cls(
            service_name=str(parsed["service_name"]),
            message_type=MessageTypeDescriptor.from_dict(parsed["message_type"]),
            profile=StreamProfile(parsed["profile"]),
            # A missing pre-v0.2 field intentionally advertises no readiness.
            transport=TransportFeatures(
                ReadinessProtocol(transport_value.get("readiness", "none"))
            ),
            resources=StreamResourceConfig.from_dict(parsed.get("resources")),
        )

    @classmethod
    def image(cls, service_name: str) -> StreamDescriptor:
        return cls(
            service_name,
            MessageTypeDescriptor.from_dict(_IMAGE_DESCRIPTOR),
            StreamProfile.SENSOR,
            TransportFeatures.sidecar_v1(),
            StreamResourceConfig(),
        )

    @classmethod
    def point_cloud(cls, service_name: str) -> StreamDescriptor:
        return cls(
            service_name,
            MessageTypeDescriptor.from_dict(_POINT_CLOUD_DESCRIPTOR),
            StreamProfile.SENSOR,
            TransportFeatures.sidecar_v1(),
            StreamResourceConfig(),
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "service_name": self.service_name,
            "message_type": self.message_type.to_dict(),
            "profile": self.profile.value,
            "transport": {"readiness": self.transport.readiness.value},
            "resources": self.resources.to_dict(),
        }

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), separators=(",", ":"), sort_keys=True)


@dataclass(frozen=True)
class RpcDescriptor:
    """Complete typed request/response RPC endpoint metadata."""

    service_name: str
    request_type: MessageTypeDescriptor
    response_type: MessageTypeDescriptor
    transport: RpcTransportFeatures = RpcTransportFeatures()
    resources: RpcResourceConfig = RpcResourceConfig()

    @property
    def request(self) -> MessageTypeDescriptor:
        return self.request_type

    @property
    def response(self) -> MessageTypeDescriptor:
        return self.response_type

    @classmethod
    def from_dict(cls, value: Mapping[str, Any]) -> RpcDescriptor:
        if not isinstance(value, MappingABC):
            raise ValueError("RpcDescriptor JSON must contain an object")
        transport_value = value.get("transport", {})
        if transport_value is None:
            transport_value = {}
        if not isinstance(transport_value, MappingABC):
            raise ValueError("RpcDescriptor transport must contain an object")
        request_value = value.get("request_type", value.get("request"))
        response_value = value.get("response_type", value.get("response"))
        if not isinstance(request_value, MappingABC) or not isinstance(
            response_value, MappingABC
        ):
            raise ValueError("RpcDescriptor must contain request and response types")
        return cls(
            service_name=str(value["service_name"]),
            request_type=MessageTypeDescriptor.from_dict(request_value),
            response_type=MessageTypeDescriptor.from_dict(response_value),
            transport=RpcTransportFeatures(
                RpcReadinessProtocol(
                    transport_value.get("readiness", RpcReadinessProtocol.NONE.value)
                )
            ),
            resources=RpcResourceConfig.from_dict(value.get("resources")),
        )

    @classmethod
    def from_json(cls, value: str | bytes | bytearray) -> RpcDescriptor:
        parsed = json.loads(value)
        return cls.from_dict(parsed)

    @classmethod
    def fixed(
        cls,
        service_name: str,
        request_type: MessageTypeDescriptor | Mapping[str, Any],
        response_type: MessageTypeDescriptor | Mapping[str, Any],
        *,
        resources: RpcResourceConfig | None = None,
        transport: RpcTransportFeatures | None = None,
    ) -> RpcDescriptor:
        request = _coerce_message_descriptor(request_type)
        response = _coerce_message_descriptor(response_type)
        if request.shape is not MessageShape.FIXED or response.shape is not MessageShape.FIXED:
            raise ValueError("RPC request and response message types must be fixed-size")
        return cls(
            service_name=_nonempty_text(service_name, "service_name"),
            request_type=request,
            response_type=response,
            transport=transport or RpcTransportFeatures.rpc_sidecar_v1(),
            resources=resources or RpcResourceConfig(),
        )

    for_fixed = fixed

    @classmethod
    def fixed_slice(
        cls,
        service_name: str,
        request_type: MessageTypeDescriptor | Mapping[str, Any],
        response_type: MessageTypeDescriptor | Mapping[str, Any],
        *,
        resources: RpcResourceConfig | None = None,
        transport: RpcTransportFeatures | None = None,
    ) -> RpcDescriptor:
        request = _coerce_message_descriptor(request_type)
        response = _coerce_message_descriptor(response_type)
        if request.shape is not MessageShape.FIXED or response.shape is not MessageShape.SLICE:
            raise ValueError("RPC shape must be fixed request and slice response")
        return cls(
            service_name=_nonempty_text(service_name, "service_name"),
            request_type=request,
            response_type=response,
            transport=transport or RpcTransportFeatures.rpc_sidecar_v1(),
            resources=resources or RpcResourceConfig(),
        )

    for_fixed_slice = fixed_slice
    fixed_to_slice = fixed_slice

    @classmethod
    def slice_fixed(
        cls,
        service_name: str,
        request_type: MessageTypeDescriptor | Mapping[str, Any],
        response_type: MessageTypeDescriptor | Mapping[str, Any],
        *,
        resources: RpcResourceConfig | None = None,
        transport: RpcTransportFeatures | None = None,
    ) -> RpcDescriptor:
        request = _coerce_message_descriptor(request_type)
        response = _coerce_message_descriptor(response_type)
        if request.shape is not MessageShape.SLICE or response.shape is not MessageShape.FIXED:
            raise ValueError("RPC shape must be slice request and fixed response")
        return cls(
            service_name=_nonempty_text(service_name, "service_name"),
            request_type=request,
            response_type=response,
            transport=transport or RpcTransportFeatures.rpc_sidecar_v1(),
            resources=resources or RpcResourceConfig(),
        )

    for_slice_fixed = slice_fixed
    slice_to_fixed = slice_fixed

    @classmethod
    def slice(
        cls,
        service_name: str,
        request_type: MessageTypeDescriptor | Mapping[str, Any],
        response_type: MessageTypeDescriptor | Mapping[str, Any],
        *,
        resources: RpcResourceConfig | None = None,
        transport: RpcTransportFeatures | None = None,
    ) -> RpcDescriptor:
        request = _coerce_message_descriptor(request_type)
        response = _coerce_message_descriptor(response_type)
        if request.shape is not MessageShape.SLICE or response.shape is not MessageShape.SLICE:
            raise ValueError("RPC request and response message types must both be slices")
        return cls(
            service_name=_nonempty_text(service_name, "service_name"),
            request_type=request,
            response_type=response,
            transport=transport or RpcTransportFeatures.rpc_sidecar_v1(),
            resources=resources or RpcResourceConfig(),
        )

    for_slice = slice
    slice_to_slice = slice

    def to_dict(self) -> dict[str, Any]:
        return {
            "service_name": self.service_name,
            "request_type": self.request_type.to_dict(),
            "response_type": self.response_type.to_dict(),
            "transport": {"readiness": self.transport.readiness.value},
            "resources": self.resources.to_dict(),
        }

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), separators=(",", ":"), sort_keys=True)


IMAGE_DESCRIPTOR = MessageTypeDescriptor.from_dict(_IMAGE_DESCRIPTOR)
POINT_CLOUD_DESCRIPTOR = MessageTypeDescriptor.from_dict(_POINT_CLOUD_DESCRIPTOR)
POINT_XYZI_DTYPE = np.dtype(POINT_XYZI_DTYPE_SPEC)


@dataclass(frozen=True)
class PublisherPressure:
    """Point-in-time counters for a publisher endpoint."""

    loans_in_use: int = 0
    loan_failures: int = 0
    send_attempts: int = 0
    send_successes: int = 0
    send_failures: int = 0
    subscriber_count: int = 0
    estimated_dropped_samples: int | None = None
    loan_attempts: int = 0
    loan_successes: int = 0
    loans_high_water: int = 0
    payload_bytes_last: int = 0
    payload_bytes_max_seen: int = 0
    total_loaned: int = 0
    queue_full: int = 0
    unable_to_deliver: int = 0
    readiness_failures: int = 0

    @classmethod
    def _from_native(cls, value: Any) -> PublisherPressure:
        if isinstance(value, MappingABC):
            fields = {
                field.name: value.get(field.name, field.default)
                for field in dataclasses.fields(cls)
                if field.default is not dataclasses.MISSING
            }
            fields.update(
                {
                    "loan_attempts": value.get("loan_attempts", fields["loan_attempts"]),
                    "loan_successes": value.get(
                        "loan_successes", fields["loan_successes"]
                    ),
                    "loans_high_water": value.get(
                        "loans_high_water",
                        value.get("loan_high_water", fields["loans_high_water"]),
                    ),
                    "payload_bytes_last": value.get(
                        "payload_bytes_last", fields["payload_bytes_last"]
                    ),
                    "payload_bytes_max_seen": value.get(
                        "payload_bytes_max_seen", fields["payload_bytes_max_seen"]
                    ),
                    "total_loaned": value.get("total_loaned", fields["total_loaned"]),
                    "queue_full": value.get("queue_full", fields["queue_full"]),
                    "unable_to_deliver": value.get(
                        "unable_to_deliver", fields["unable_to_deliver"]
                    ),
                    "readiness_failures": value.get(
                        "readiness_failures",
                        value.get(
                            "readiness_notify_failures", fields["readiness_failures"]
                        ),
                    ),
                }
            )
            return cls(**fields)
        values = tuple(value)
        # Native v0.3 exposed only the original seven publisher counters.
        names = (
            "loans_in_use",
            "loan_failures",
            "send_attempts",
            "send_successes",
            "send_failures",
            "subscriber_count",
            "estimated_dropped_samples",
            "loan_attempts",
            "loan_successes",
            "loans_high_water",
            "payload_bytes_last",
            "payload_bytes_max_seen",
            "total_loaned",
            "queue_full",
            "unable_to_deliver",
            "readiness_failures",
        )
        return cls(**dict(zip(names, values, strict=False)))

    @property
    def loan_errors(self) -> int:
        """Compatibility alias for the rejected-loan counter."""
        return self.loan_failures

    @property
    def loan_high_water(self) -> int:
        return self.loans_high_water

    @property
    def payload_bytes(self) -> int:
        return self.payload_bytes_last

    @property
    def queue_full_events(self) -> int:
        return self.queue_full

    @property
    def unable_to_deliver_events(self) -> int:
        return self.unable_to_deliver

    @property
    def readiness_notify_failures(self) -> int:
        return self.readiness_failures


@dataclass(frozen=True)
class SubscriberPressure:
    """Point-in-time counters for a subscriber endpoint."""

    received_samples: int = 0
    borrowed_samples: int = 0
    borrow_failures: int = 0
    estimated_dropped_samples: int | None = None
    receive_attempts: int = 0
    borrowed_high_water: int = 0
    borrow_duration_count: int = 0
    borrow_duration_total_ns: int = 0
    borrow_duration_max_ns: int = 0
    oldest_borrow_age_ns: int | None = None

    @classmethod
    def _from_native(cls, value: Any) -> SubscriberPressure:
        if isinstance(value, MappingABC):
            fields = {
                field.name: value.get(field.name, field.default)
                for field in dataclasses.fields(cls)
                if field.default is not dataclasses.MISSING
            }
            # Accept the descriptive names used by the native pressure model
            # as well as the compact aliases used by early Python previews.
            fields["borrow_duration_count"] = value.get(
                "borrow_duration_count",
                value.get("duration_count", fields["borrow_duration_count"]),
            )
            fields["borrow_duration_total_ns"] = value.get(
                "borrow_duration_total_ns",
                value.get("duration_total_ns", fields["borrow_duration_total_ns"]),
            )
            fields["borrow_duration_max_ns"] = value.get(
                "borrow_duration_max_ns",
                value.get("duration_max_ns", fields["borrow_duration_max_ns"]),
            )
            return cls(**fields)
        values = tuple(value)
        names = (
            "received_samples",
            "borrowed_samples",
            "borrow_failures",
            "estimated_dropped_samples",
            "receive_attempts",
            "borrowed_high_water",
            "borrow_duration_count",
            "borrow_duration_total_ns",
            "borrow_duration_max_ns",
            "oldest_borrow_age_ns",
        )
        return cls(**dict(zip(names, values, strict=False)))

    @property
    def receive_duration_count(self) -> int:
        return self.borrow_duration_count

    @property
    def receive_duration_total_ns(self) -> int:
        return self.borrow_duration_total_ns

    @property
    def receive_duration_max_ns(self) -> int:
        return self.borrow_duration_max_ns

    @property
    def duration_count(self) -> int:
        return self.borrow_duration_count

    @property
    def duration_total_ns(self) -> int:
        return self.borrow_duration_total_ns

    @property
    def duration_max_ns(self) -> int:
        return self.borrow_duration_max_ns


@dataclass(frozen=True)
class StreamPressure:
    """A typed, ownership-neutral pressure snapshot for one endpoint."""

    service_name: str
    resources: StreamResourceConfig = StreamResourceConfig()
    publisher: PublisherPressure | None = None
    subscriber: SubscriberPressure | None = None
    snapshot_timestamp_ns: int = 0
    snapshot_timestamp_unix_ms: int = 0

    @classmethod
    def _from_native(cls, value: Any) -> StreamPressure:
        if isinstance(value, MappingABC):
            resources = StreamResourceConfig.from_dict(value.get("resources"))
            publisher = value.get("publisher")
            subscriber = value.get("subscriber")
            return cls(
                service_name=str(value.get("service_name", "<unknown>")),
                resources=resources,
                publisher=(
                    None if publisher is None else PublisherPressure._from_native(publisher)
                ),
                subscriber=(
                    None if subscriber is None else SubscriberPressure._from_native(subscriber)
                ),
                snapshot_timestamp_ns=int(
                    value.get(
                        "snapshot_timestamp_ns",
                        value.get("timestamp_ns", value.get("snapshot_timestamp_unix_ms", 0) * 1_000_000),
                    )
                ),
                snapshot_timestamp_unix_ms=int(
                    value.get("snapshot_timestamp_unix_ms", 0)
                ),
            )

        values = tuple(value)
        if len(values) == 5 and not isinstance(values[1], (tuple, list, MappingABC)):
            # Compatibility with the v0.3 subscriber-only tuple.
            service, received, borrowed, failures, maximum = values
            return cls(
                service_name=str(service),
                resources=StreamResourceConfig(max_borrowed=maximum),
                subscriber=SubscriberPressure(
                    received_samples=int(received),
                    borrowed_samples=int(borrowed),
                    borrow_failures=int(failures),
                ),
            )
        service = str(values[0]) if values else "<unknown>"
        resources_value = values[1] if len(values) > 1 else None
        if isinstance(resources_value, MappingABC):
            resources = StreamResourceConfig.from_dict(resources_value)
        elif isinstance(resources_value, (tuple, list)):
            resources = StreamResourceConfig(*resources_value)
        else:
            resources = StreamResourceConfig()
        publisher_value = values[2] if len(values) > 2 else None
        subscriber_value = values[3] if len(values) > 3 else None
        return cls(
            service_name=service,
            resources=resources,
            publisher=(
                None
                if publisher_value is None
                else PublisherPressure._from_native(publisher_value)
            ),
            subscriber=(
                None
                if subscriber_value is None
                else SubscriberPressure._from_native(subscriber_value)
            ),
            snapshot_timestamp_ns=int(values[4]) if len(values) > 4 else 0,
        )

    def __call__(self) -> StreamPressure:
        """Allow both the historical ``pressure`` property and ``pressure()``."""
        return self

    @property
    def received_samples(self) -> int:
        return 0 if self.subscriber is None else self.subscriber.received_samples

    @property
    def borrowed_samples(self) -> int:
        return 0 if self.subscriber is None else self.subscriber.borrowed_samples

    @property
    def borrow_failures(self) -> int:
        return 0 if self.subscriber is None else self.subscriber.borrow_failures

    @property
    def max_borrowed_samples(self) -> int:
        return self.resources.max_borrowed

    @property
    def timestamp_ns(self) -> int:
        return self.snapshot_timestamp_ns


@dataclass(frozen=True)
class RpcClientPressure:
    calls_attempted: int = 0
    calls_started: int = 0
    calls_completed: int = 0
    calls_failed: int = 0
    calls_timed_out: int = 0
    calls_cancelled: int = 0
    calls_activation_expired: int = 0
    in_flight_current: int = 0
    in_flight_high_water: int = 0
    response_guards_current: int = 0
    response_guards_high_water: int = 0
    capacity_rejections: int = 0
    server_capacity_rejections: int = 0
    server_unavailable: int = 0
    request_loan_failures: int = 0
    response_receive_failures: int = 0
    call_duration_count: int = 0
    call_duration_total_ns: int = 0
    call_duration_max_ns: int = 0

    @classmethod
    def _from_native(cls, value: Any) -> RpcClientPressure:
        if not isinstance(value, MappingABC):
            return cls(*tuple(value))
        return cls(
            **{
                field.name: int(value.get(field.name, field.default))
                for field in dataclasses.fields(cls)
            }
        )


@dataclass(frozen=True)
class RpcServerPressure:
    requests_received: int = 0
    requests_current: int = 0
    requests_high_water: int = 0
    responses_attempted: int = 0
    responses_sent: int = 0
    responses_failed: int = 0
    responses_abandoned: int = 0
    request_borrow_failures: int = 0
    response_loan_failures: int = 0

    @classmethod
    def _from_native(cls, value: Any) -> RpcServerPressure:
        if not isinstance(value, MappingABC):
            return cls(*tuple(value))
        return cls(
            **{
                field.name: int(value.get(field.name, field.default))
                for field in dataclasses.fields(cls)
            }
        )


@dataclass(frozen=True)
class RpcPressure:
    service_name: str
    capability_instance_id: str | None = None
    resources: RpcResourceConfig = RpcResourceConfig()
    client_activation: int | None = None
    client: RpcClientPressure | None = None
    server: RpcServerPressure | None = None
    snapshot_timestamp_unix_ms: int = 0

    @classmethod
    def _from_native(cls, value: Any) -> RpcPressure:
        if not isinstance(value, MappingABC):
            values = tuple(value)
            return cls(service_name=str(values[0]) if values else "<unknown>")
        client = value.get("client")
        server = value.get("server")
        return cls(
            service_name=str(value.get("service_name", "<unknown>")),
            capability_instance_id=(
                None
                if value.get("capability_instance_id") is None
                else str(value["capability_instance_id"])
            ),
            resources=RpcResourceConfig.from_dict(value.get("resources")),
            client_activation=(
                None
                if value.get("client_activation") is None
                else int(value["client_activation"])
            ),
            client=None if client is None else RpcClientPressure._from_native(client),
            server=None if server is None else RpcServerPressure._from_native(server),
            snapshot_timestamp_unix_ms=int(value.get("snapshot_timestamp_unix_ms", 0)),
        )

    @property
    def timestamp_unix_ms(self) -> int:
        return self.snapshot_timestamp_unix_ms


def _effective_resources(
    descriptor: StreamDescriptor,
    *,
    max_loans: Any = None,
    max_borrowed: Any = None,
) -> tuple[StreamDescriptor, StreamResourceConfig]:
    if not isinstance(descriptor, StreamDescriptor):
        raise TypeError("descriptor must be a StreamDescriptor")
    resources = StreamResourceConfig(
        max_loans=(
            descriptor.resources.max_loans
            if max_loans is None
            else _resource_limit(max_loans, "max_loans")
        ),
        max_borrowed=(
            descriptor.resources.max_borrowed
            if max_borrowed is None
            else _resource_limit(max_borrowed, "max_borrowed")
        ),
    )
    return dataclasses.replace(descriptor, resources=resources), resources


@dataclass(frozen=True)
class WaitStats:
    waits: int
    wakeups: int
    empty_wakeups: int
    cancellation_wakeups: int
    samples_processed: int

    @classmethod
    def _from_native(cls, value: tuple[int, int, int, int, int]) -> WaitStats:
        return cls(*value)


def _checked_required_bytes(height: int, row_bytes: int, stride: int) -> int:
    if height < 0 or row_bytes < 0 or stride < 0:
        raise InvalidImageLayoutError("negative Image dimensions or stride")
    if height == 0 or row_bytes == 0:
        return 0
    if stride > sys.maxsize or row_bytes > sys.maxsize:
        raise InvalidImageLayoutError("Image dimensions exceed platform address space")
    required = (height - 1) * stride + row_bytes
    if required > sys.maxsize:
        raise InvalidImageLayoutError("Image layout size overflows platform address space")
    return required


def _index(value: Any, name: str) -> int:
    try:
        return operator.index(value)
    except TypeError as error:
        raise TypeError(f"{name} must be an integer") from error


def _nonnegative_index(value: Any, name: str, error_type: type[Exception]) -> int:
    result = _index(value, name)
    if result < 0:
        raise error_type(f"{name} must be non-negative")
    return result


def _u64(value: Any, name: str) -> int:
    result = _nonnegative_index(value, name, ValueError)
    if result > (1 << 64) - 1:
        raise ValueError(f"{name} exceeds the u64 ABI")
    return result


def _checked_image_loan_layout(
    width: Any,
    height: Any,
    encoding: Any,
    stride: Any,
    maximum_payload_bytes: int,
) -> tuple[int, int, int, int | None, int, int]:
    width_value = _nonnegative_index(width, "width", InvalidImageLayoutError)
    height_value = _nonnegative_index(height, "height", InvalidImageLayoutError)
    if width_value > (1 << 32) - 1 or height_value > (1 << 32) - 1:
        raise InvalidImageLayoutError("Image dimensions exceed the u32 ABI")

    encoding_value = _index(encoding, "encoding")
    if encoding_value not in (
        int(PixelFormat.RGB8),
        int(PixelFormat.BGR8),
        int(PixelFormat.GRAY8),
    ):
        raise UnsupportedPixelFormatError(
            f"unsupported Image pixel format value {encoding_value}"
        )
    channels = 3 if encoding_value in (int(PixelFormat.RGB8), int(PixelFormat.BGR8)) else 1
    if width_value > sys.maxsize // channels:
        raise InvalidImageLayoutError("Image row bytes exceed platform address space")
    row_bytes = width_value * channels

    stride_value: int | None
    if stride is None:
        stride_value = None
        effective_stride = row_bytes
    else:
        stride_int = _nonnegative_index(stride, "stride", InvalidImageLayoutError)
        if stride_int > (1 << 32) - 1:
            raise InvalidImageLayoutError("Image stride exceeds the u32 ABI")
        stride_value = stride_int
        effective_stride = stride_int
    if width_value and height_value and effective_stride < row_bytes:
        raise InvalidImageLayoutError(
            f"Image stride {effective_stride} is smaller than row width {row_bytes}"
        )
    if height_value and effective_stride > sys.maxsize // height_value:
        raise InvalidImageLayoutError("Image payload length overflows platform address space")
    payload_len = effective_stride * height_value
    if payload_len > maximum_payload_bytes:
        raise InvalidPayloadError(
            f"invalid Image payload length {payload_len}; maximum is {maximum_payload_bytes}"
        )
    return (
        width_value,
        height_value,
        encoding_value,
        stride_value,
        payload_len,
        channels,
    )


class ReceivedImageSample:
    """An Image header plus an owned iceoryx2 receive guard."""

    __slots__ = ("_owner",)

    def __init__(self, owner: Any) -> None:
        self._owner = owner

    @property
    def sequence(self) -> int:
        return self._owner.sequence

    @property
    def timestamp_ns(self) -> int:
        return self._owner.timestamp_ns

    @property
    def width(self) -> int:
        return self._owner.width

    @property
    def height(self) -> int:
        return self._owner.height

    @property
    def stride(self) -> int:
        return self._owner.stride

    @property
    def encoding(self) -> PixelFormat | int:
        value = self._owner.encoding
        try:
            return PixelFormat(value)
        except ValueError:
            return value

    @property
    def payload_nbytes(self) -> int:
        return self._owner.payload_nbytes

    def payload_view(self) -> memoryview:
        """Return a read-only, zero-copy byte view retaining this sample."""
        return memoryview(self._owner)

    def numpy(self) -> np.ndarray[Any, np.dtype[np.uint8]]:
        """Return a read-only zero-copy view with shape and stride from the header.

        A NumPy view owns the received shared-memory sample for as long as the
        view or any derived view can still access that memory.
        """
        width = self.width
        height = self.height
        stride = self.stride
        encoding = self.encoding
        if encoding in (PixelFormat.RGB8, PixelFormat.BGR8):
            channels = 3
            shape = (height, width, channels)
            strides = (stride, channels, 1)
        elif encoding == PixelFormat.GRAY8:
            channels = 1
            shape = (height, width)
            strides = (stride, 1)
        else:
            raise UnsupportedPixelFormatError(
                f"unsupported Image pixel format value {int(encoding)}"
            )

        row_bytes = width * channels
        required = _checked_required_bytes(height, row_bytes, stride)
        if height > 1 and stride < row_bytes:
            raise InvalidImageLayoutError(
                f"Image stride {stride} is smaller than row width {row_bytes}"
            )
        if required > self.payload_nbytes:
            raise InvalidImageLayoutError(
                f"Image layout requires {required} payload bytes but sample has "
                f"{self.payload_nbytes}"
            )
        return np.ndarray(
            shape=shape,
            dtype=np.uint8,
            buffer=self._owner,
            offset=0,
            strides=strides,
            order="C",
        )

    def _debug_payload_address(self) -> int:
        return self._owner._debug_payload_address()

    def __repr__(self) -> str:
        return (
            f"<ReceivedImageSample seq={self.sequence} shape={self.height}x{self.width} "
            f"encoding={self.encoding!s} borrowed=True>"
        )


class ReceivedPointCloudSample:
    """A PointCloud header plus an owned iceoryx2 receive guard."""

    __slots__ = ("_owner",)

    def __init__(self, owner: Any) -> None:
        self._owner = owner

    @property
    def sequence(self) -> int:
        return self._owner.sequence

    @property
    def timestamp_ns(self) -> int:
        return self._owner.timestamp_ns

    @property
    def width(self) -> int:
        return self._owner.width

    @property
    def height(self) -> int:
        return self._owner.height

    @property
    def dense(self) -> bool:
        return self._owner.dense

    @property
    def point_count(self) -> int:
        return self._owner.point_count

    @property
    def payload_nbytes(self) -> int:
        return self._owner.payload_nbytes

    def payload_view(self) -> memoryview:
        """Return a read-only, zero-copy byte view retaining this sample."""
        return memoryview(self._owner)

    def numpy(self) -> np.ndarray[Any, np.dtype[np.void]]:
        """Return a read-only structured zero-copy view of PointXYZI elements.

        A NumPy view owns the received shared-memory sample for as long as the
        view or any derived view can still access that memory.
        """
        count = self.point_count
        if self.width * self.height != count:
            raise InvalidPayloadError(
                f"PointCloud width*height={self.width * self.height} does not match "
                f"payload element count {count}"
            )
        expected_bytes = count * POINT_XYZI_DTYPE.itemsize
        if expected_bytes > sys.maxsize or expected_bytes != self.payload_nbytes:
            raise InvalidPayloadError(
                f"PointCloud payload has {self.payload_nbytes} bytes; expected {expected_bytes}"
            )
        address = self._debug_payload_address()
        alignment = int(self._owner.payload_alignment)
        if address % alignment != 0:
            raise InvalidPayloadAlignmentError(
                f"PointCloud payload address 0x{address:x} is not aligned to {alignment} bytes"
            )
        return np.ndarray(
            shape=(count,),
            dtype=POINT_XYZI_DTYPE,
            buffer=self._owner,
            offset=0,
            strides=(POINT_XYZI_DTYPE.itemsize,),
        )

    def _debug_payload_address(self) -> int:
        return self._owner._debug_payload_address()

    def __repr__(self) -> str:
        return (
            f"<ReceivedPointCloudSample seq={self.sequence} points={self.point_count} "
            "borrowed=True>"
        )


_SIGNAL_QUANTUM_SECONDS = 0.5


class _SubscriberBase:
    _native: Any

    def _init_receive_state(self) -> None:
        self._receive_lock = threading.Lock()
        self._receive_owner: object | None = None

    def _claim_receive(self) -> object:
        owner = object()
        with self._receive_lock:
            if self._receive_owner is not None:
                raise ConcurrentReceiveError(
                    "only one receive may be outstanding per subscriber"
                )
            self._receive_owner = owner
        return owner

    def _release_receive(self, owner: object) -> None:
        with self._receive_lock:
            # A stale timeout/cancellation cleanup must never release a newer
            # receive that has already claimed this subscriber.
            if self._receive_owner is owner:
                self._receive_owner = None

    def _activation_cancelled(self) -> bool:
        activation_cancelled = getattr(self._native, "activation_cancelled", None)
        if callable(activation_cancelled):
            return bool(activation_cancelled())
        return bool(activation_cancelled)

    def _receive_native(self, timeout: float | None) -> Any | None:
        if timeout is not None:
            if not isinstance(timeout, (int, float)) or not np.isfinite(timeout) or timeout < 0:
                raise ValueError("timeout must be None or a finite non-negative number")
            if timeout == 0:
                return self._native.try_receive()
            deadline = _time.monotonic() + float(timeout)
        else:
            deadline = None

        while True:
            if deadline is None:
                quantum = _SIGNAL_QUANTUM_SECONDS
            else:
                remaining = deadline - _time.monotonic()
                if remaining <= 0:
                    return None
                quantum = min(_SIGNAL_QUANTUM_SECONDS, remaining)
            sample = self._native.wait_receive(quantum)
            if sample is not None:
                return sample
            # Activation-bound waits are woken immediately by the native
            # activation guard.  Treat that wake as cooperative cancellation;
            # a subsequent use of the stale handle raises ActivationExpired.
            activation_cancelled = getattr(
                self._native, "activation_cancelled", None
            )
            if activation_cancelled is not None and activation_cancelled():
                return None

    @property
    def pressure(self) -> StreamPressure:
        return StreamPressure._from_native(self._native.pressure())

    @property
    def wait_stats(self) -> WaitStats:
        return WaitStats._from_native(self._native.wait_stats())

    async def _receive_async(
        self, timeout: float | None, wrap: Any
    ) -> Any | None:
        supported = getattr(self._native, "async_receive_supported", None)
        if callable(supported) and not supported():
            raise AsyncRuntimeClosedError(
                "receive_async is available only for activation-bound subscribers"
            )
        if timeout is not None:
            if not isinstance(timeout, (int, float)) or not np.isfinite(timeout) or timeout < 0:
                raise ValueError("timeout must be None or a finite non-negative number")
            timeout_value = float(timeout)
        else:
            timeout_value = None

        owner = self._claim_receive()
        try:
            # Keep the fast path synchronous and do not register a readiness
            # future when the sample is already available.
            sample = self._native.try_receive()
            if sample is not None:
                return wrap(sample)
            if timeout_value == 0:
                return None

            loop = asyncio.get_running_loop()
            deadline = (
                None
                if timeout_value is None
                else _time.monotonic() + timeout_value
            )

            while True:
                if deadline is not None:
                    remaining = deadline - _time.monotonic()
                    if remaining <= 0:
                        return None
                else:
                    remaining = None

                # Avoid registering against an activation that expired just
                # after the initial direct receive.  The post-registration
                # check below closes the small race with teardown.
                if self._activation_cancelled():
                    return None
                future = loop.create_future()
                try:
                    generation = self._native.begin_receive_async(loop, future)
                except ActivationExpiredError:
                    future.cancel()
                    if self._activation_cancelled():
                        return None
                    raise
                registered = True
                if self._activation_cancelled():
                    registered = False
                    self._native.cancel_receive_async(generation)
                    future.cancel()
                    return None
                try:
                    # Shield the native-owned future.  wait_for/task
                    # cancellation must not cancel an object that a native
                    # readiness callback may still complete.
                    waitable = asyncio.shield(future)
                    if remaining is None:
                        await waitable
                    else:
                        await asyncio.wait_for(waitable, remaining)
                except asyncio.TimeoutError:
                    registered = False
                    self._native.cancel_receive_async(generation)
                    future.cancel()
                    return None
                except asyncio.CancelledError:
                    registered = False
                    self._native.cancel_receive_async(generation)
                    future.cancel()
                    # Native activation cancellation completes the Future
                    # normally.  Reaching this branch therefore always means
                    # ordinary asyncio Task.cancel()/wait_for cancellation and
                    # must remain observable to user code and the supervisor.
                    raise
                except BaseException:
                    registered = False
                    self._native.cancel_receive_async(generation)
                    future.cancel()
                    raise
                finally:
                    if registered:
                        self._native.finish_receive_async(generation)

                # Readiness is only a token.  Acquire and wrap the existing
                # zero-copy sample after the notification, never in the
                # asyncio scheduling path.
                if self._activation_cancelled():
                    return None
                try:
                    sample = self._native.try_receive()
                except ActivationExpiredError:
                    if self._activation_cancelled():
                        return None
                    raise
                if sample is not None:
                    return wrap(sample)
                # A native wake can be spurious.  Re-register and await the
                # next notification without polling or sleeping.
        finally:
            self._release_receive(owner)


class ImageSubscriber(_SubscriberBase):
    __slots__ = (
        "_native",
        "descriptor",
        "provider_component_id",
        "capability_instance_id",
        "activation_id",
    )

    def __init__(
        self,
        native: Any,
        descriptor: StreamDescriptor,
        *,
        provider_component_id: str | None = None,
        capability_instance_id: str | None = None,
        activation_id: int | None = None,
    ) -> None:
        self._init_receive_state()
        self._native = native
        self.descriptor = descriptor
        self.provider_component_id = provider_component_id
        self.capability_instance_id = capability_instance_id
        self.activation_id = activation_id

    @classmethod
    def open(
        cls,
        descriptor: StreamDescriptor,
        *,
        max_borrowed: int | None = None,
    ) -> ImageSubscriber:
        if not isinstance(descriptor, StreamDescriptor):
            raise TypeError("descriptor must be a StreamDescriptor")
        resources = StreamResourceConfig(
            max_loans=descriptor.resources.max_loans,
            max_borrowed=(
                descriptor.resources.max_borrowed
                if max_borrowed is None
                else _resource_limit(max_borrowed, "max_borrowed")
            ),
        )
        native = _rbt._ImageSubscriber.open(
            descriptor.to_json(), max_borrowed=resources.max_borrowed
        )
        return cls(native, dataclasses.replace(descriptor, resources=resources))

    def try_receive(self) -> ReceivedImageSample | None:
        owner = self._claim_receive()
        try:
            sample = self._native.try_receive()
            return None if sample is None else ReceivedImageSample(sample)
        finally:
            self._release_receive(owner)

    def receive(self, timeout: float | None = None) -> ReceivedImageSample | None:
        owner = self._claim_receive()
        try:
            sample = self._receive_native(timeout)
            return None if sample is None else ReceivedImageSample(sample)
        finally:
            self._release_receive(owner)

    async def receive_async(
        self, timeout: float | None = None
    ) -> ReceivedImageSample | None:
        return await self._receive_async(timeout, ReceivedImageSample)

    @property
    def resources(self) -> StreamResourceConfig:
        return self.descriptor.resources


class PointCloudSubscriber(_SubscriberBase):
    __slots__ = (
        "_native",
        "descriptor",
        "provider_component_id",
        "capability_instance_id",
        "activation_id",
    )

    def __init__(
        self,
        native: Any,
        descriptor: StreamDescriptor,
        *,
        provider_component_id: str | None = None,
        capability_instance_id: str | None = None,
        activation_id: int | None = None,
    ) -> None:
        self._init_receive_state()
        self._native = native
        self.descriptor = descriptor
        self.provider_component_id = provider_component_id
        self.capability_instance_id = capability_instance_id
        self.activation_id = activation_id

    @classmethod
    def open(
        cls,
        descriptor: StreamDescriptor,
        *,
        max_borrowed: int | None = None,
    ) -> PointCloudSubscriber:
        if not isinstance(descriptor, StreamDescriptor):
            raise TypeError("descriptor must be a StreamDescriptor")
        resources = StreamResourceConfig(
            max_loans=descriptor.resources.max_loans,
            max_borrowed=(
                descriptor.resources.max_borrowed
                if max_borrowed is None
                else _resource_limit(max_borrowed, "max_borrowed")
            ),
        )
        native = _rbt._PointCloudSubscriber.open(
            descriptor.to_json(), max_borrowed=resources.max_borrowed
        )
        return cls(native, dataclasses.replace(descriptor, resources=resources))

    def try_receive(self) -> ReceivedPointCloudSample | None:
        owner = self._claim_receive()
        try:
            sample = self._native.try_receive()
            return None if sample is None else ReceivedPointCloudSample(sample)
        finally:
            self._release_receive(owner)

    def receive(self, timeout: float | None = None) -> ReceivedPointCloudSample | None:
        owner = self._claim_receive()
        try:
            sample = self._receive_native(timeout)
            return None if sample is None else ReceivedPointCloudSample(sample)
        finally:
            self._release_receive(owner)

    async def receive_async(
        self, timeout: float | None = None
    ) -> ReceivedPointCloudSample | None:
        return await self._receive_async(timeout, ReceivedPointCloudSample)

    @property
    def resources(self) -> StreamResourceConfig:
        return self.descriptor.resources


_RPC_DEFAULT_TIMEOUT_SECONDS = 5.0


def _checked_rpc_timeout(value: Any, name: str = "timeout") -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise TypeError(f"{name} must be a finite non-negative number")
    result = float(value)
    if not np.isfinite(result) or result < 0:
        raise ValueError(f"{name} must be a finite non-negative number")
    return result


def _rpc_element_count(value: Any) -> int:
    result = _nonnegative_index(value, "element_count", RpcCapacityError)
    if result > sys.maxsize:
        raise ValueError("element_count exceeds platform address space")
    return result


class _RpcImageReadable:
    _owner: Any

    def _rpc_image_header(self) -> tuple[int, int, int, int, int, int]:
        values = self._owner._image_header()
        return tuple(int(value) for value in values)  # type: ignore[return-value]

    @property
    def sequence(self) -> int:
        return self._rpc_image_header()[0]

    @property
    def timestamp_ns(self) -> int:
        return self._rpc_image_header()[1]

    @property
    def width(self) -> int:
        return self._rpc_image_header()[2]

    @property
    def height(self) -> int:
        return self._rpc_image_header()[3]

    @property
    def stride(self) -> int:
        return self._rpc_image_header()[4]

    @property
    def encoding(self) -> PixelFormat | int:
        value = self._rpc_image_header()[5]
        try:
            return PixelFormat(value)
        except ValueError:
            return value


class _RpcImageWritable(_RpcImageReadable):
    def set_image_header(
        self,
        *,
        sequence: int = 0,
        timestamp_ns: int = 0,
        width: int,
        height: int,
        stride: int,
        encoding: PixelFormat | int,
    ) -> None:
        sequence_value = _nonnegative_index(sequence, "sequence", InvalidImageLayoutError)
        timestamp_value = _nonnegative_index(
            timestamp_ns, "timestamp_ns", InvalidImageLayoutError
        )
        width_value, height_value, encoding_value, stride_value, required, _ = (
            _checked_image_loan_layout(
                width,
                height,
                encoding,
                stride,
                self.payload_nbytes,
            )
        )
        if required > self.payload_nbytes:
            raise InvalidImageLayoutError(
                f"Image layout requires {required} payload bytes but loan has "
                f"{self.payload_nbytes}"
            )
        assert stride_value is not None
        self._owner._set_image_header(
            sequence=sequence_value,
            timestamp_ns=timestamp_value,
            width=width_value,
            height=height_value,
            stride=stride_value,
            encoding=encoding_value,
        )

    def _new_lease(self) -> Any:
        return self._owner._new_lease()

    def as_image(self) -> LoanedImageSample:
        self._rpc_image_header()
        return LoanedImageSample(self)


class RpcRequestLoan(_RpcImageWritable):
    """Writable, zero-copy request loan owned by a synchronous RPC client."""

    __slots__ = ("_owner",)

    def __init__(self, owner: Any) -> None:
        self._owner = owner

    @property
    def payload_nbytes(self) -> int:
        return int(self._owner.payload_nbytes)

    def payload_view(self) -> memoryview:
        """Return a writable view retaining this request loan."""
        return memoryview(self._owner._new_lease())

    def send(self, timeout: float = _RPC_DEFAULT_TIMEOUT_SECONDS) -> RpcResponse:
        return RpcResponse(self._owner.send(_checked_rpc_timeout(timeout)))

    def _send_async(self, loop: Any, future: Any) -> Any:
        return self._owner.send_async(loop, future)

    def _debug_payload_address(self) -> int:
        return int(self._owner._debug_payload_address())

    def _debug_export_count(self) -> int:
        return int(self._owner._debug_export_count())


class RpcResponse(_RpcImageReadable):
    """Read-only, zero-copy response guard retaining native ownership."""

    __slots__ = ("_owner",)

    def __init__(self, owner: Any) -> None:
        self._owner = owner

    @property
    def payload_nbytes(self) -> int:
        return int(self._owner.payload_nbytes)

    def payload_view(self) -> memoryview:
        """Return a read-only view retaining this response guard."""
        return memoryview(self._owner._new_lease())

    def as_image(self) -> ReceivedImageSample:
        self._rpc_image_header()
        return ReceivedImageSample(self._owner._new_lease())

    def _debug_payload_address(self) -> int:
        return int(self._owner._debug_payload_address())


class RpcResponseLoan(_RpcImageWritable):
    """Writable, zero-copy response loan owned by an RPC server request."""

    __slots__ = ("_owner",)

    def __init__(self, owner: Any) -> None:
        self._owner = owner

    @property
    def payload_nbytes(self) -> int:
        return int(self._owner.payload_nbytes)

    def payload_view(self) -> memoryview:
        """Return a writable view retaining this response loan."""
        return memoryview(self._owner._new_lease())

    def send(self) -> None:
        self._owner.send()

    def _debug_payload_address(self) -> int:
        return int(self._owner._debug_payload_address())

    def _debug_export_count(self) -> int:
        return int(self._owner._debug_export_count())


class RpcRequest(_RpcImageReadable):
    """Read-only request received by a synchronous RPC server."""

    __slots__ = ("_owner",)

    def __init__(self, owner: Any) -> None:
        self._owner = owner

    @property
    def payload_nbytes(self) -> int:
        return int(self._owner.payload_nbytes)

    def _debug_payload_address(self) -> int:
        return int(self._owner._debug_payload_address())

    def payload_view(self) -> memoryview:
        """Return a read-only view retaining this request guard."""
        return memoryview(self._owner._new_lease())

    def as_image(self) -> ReceivedImageSample:
        self._rpc_image_header()
        return ReceivedImageSample(self._owner._new_lease())

    def loan_response(self, element_count: int | None = None) -> RpcResponseLoan:
        if element_count is None:
            return RpcResponseLoan(self._owner.loan_response())
        return RpcResponseLoan(self._owner.loan_response(_rpc_element_count(element_count)))

    def respond(
        self, fill: Any = None, *, element_count: int | None = None
    ) -> RpcResponseLoan:
        loan = self.loan_response(element_count)
        if fill is not None:
            if not callable(fill):
                raise TypeError("fill must be callable")
            fill(loan)
        return loan


class RpcClient:
    """Synchronous typed local RPC client.

    The descriptor is the canonical generated request/response contract.  A
    call callback receives a writable :class:`RpcRequestLoan`; no Python
    ``bytes`` or intermediate payload object is created.
    """

    __slots__ = (
        "_native",
        "descriptor",
        "provider_component_id",
        "capability_instance_id",
        "activation_id",
    )

    def __init__(
        self,
        native: Any,
        descriptor: RpcDescriptor,
        *,
        provider_component_id: str | None = None,
        capability_instance_id: str | None = None,
        activation_id: int | None = None,
    ) -> None:
        self._native = native
        self.descriptor = descriptor
        self.provider_component_id = provider_component_id
        self.capability_instance_id = capability_instance_id
        self.activation_id = activation_id

    @classmethod
    def open(cls, descriptor: RpcDescriptor) -> RpcClient:
        if not isinstance(descriptor, RpcDescriptor):
            raise TypeError("descriptor must be an RpcDescriptor")
        return cls(_rbt._RpcClient.open(descriptor.to_json()), descriptor)

    def loan_request(self, element_count: int | None = None) -> RpcRequestLoan:
        if element_count is None:
            return RpcRequestLoan(self._native.loan_request())
        return RpcRequestLoan(self._native.loan_request(_rpc_element_count(element_count)))

    def call(
        self,
        fill: Any = None,
        *,
        timeout: float = _RPC_DEFAULT_TIMEOUT_SECONDS,
        element_count: int | None = None,
    ) -> RpcResponse:
        loan = self.loan_request(element_count)
        if fill is not None:
            if not callable(fill):
                raise TypeError("fill must be callable")
            fill(loan)
        return loan.send(timeout)

    async def call_async(
        self,
        fill: Any = None,
        *,
        timeout: float | None = _RPC_DEFAULT_TIMEOUT_SECONDS,
        element_count: int | None = None,
    ) -> RpcResponse:
        loan = self.loan_request(element_count)
        if fill is not None:
            if not callable(fill):
                raise TypeError("fill must be callable")
            fill(loan)
        if timeout is not None:
            timeout = _checked_rpc_timeout(timeout)
        loop = asyncio.get_running_loop()
        future = loop.create_future()
        call = loan._send_async(loop, future)
        try:
            waitable = asyncio.shield(future)
            if timeout is None:
                await waitable
            else:
                await asyncio.wait_for(waitable, timeout)
        except asyncio.TimeoutError:
            # The native zero-duration wait performs the final response vs
            # deadline arbitration.  A response already visible in native
            # memory therefore still wins this race.
            call._deadline()
            future.cancel()
            return RpcResponse(call._result())
        except asyncio.CancelledError:
            call._cancel()
            future.cancel()
            raise
        except BaseException:
            call._cancel()
            future.cancel()
            raise
        return RpcResponse(call._result())

    def _debug_async_registration_count(self) -> int:
        return int(self._native._debug_async_registration_count())

    @property
    def pressure(self) -> RpcPressure:
        return RpcPressure._from_native(self._native.pressure())

    def close(self) -> None:
        self._native.expire()


class RpcServer:
    """Synchronous typed local RPC server."""

    __slots__ = (
        "_native",
        "descriptor",
        "provider_component_id",
        "capability_instance_id",
    )

    def __init__(
        self,
        native: Any,
        descriptor: RpcDescriptor,
        *,
        provider_component_id: str | None = None,
        capability_instance_id: str | None = None,
    ) -> None:
        self._native = native
        self.descriptor = descriptor
        self.provider_component_id = provider_component_id
        self.capability_instance_id = capability_instance_id

    @classmethod
    def open(
        cls,
        descriptor: RpcDescriptor,
        *,
        capability_instance_id: str | None = None,
    ) -> RpcServer:
        if not isinstance(descriptor, RpcDescriptor):
            raise TypeError("descriptor must be an RpcDescriptor")
        native = _rbt._RpcServer.open(
            descriptor.to_json(), capability_instance_id=capability_instance_id
        )
        return cls(native, descriptor, capability_instance_id=capability_instance_id)

    def try_receive(self) -> RpcRequest | None:
        request = self._native.try_receive()
        return None if request is None else RpcRequest(request)

    def receive(self, timeout: float | None = None) -> RpcRequest | None:
        if timeout is not None:
            timeout = _checked_rpc_timeout(timeout)
        request = self._native.receive(timeout)
        return None if request is None else RpcRequest(request)

    async def receive_async(self, timeout: float | None = None) -> RpcRequest | None:
        if timeout is not None:
            timeout = _checked_rpc_timeout(timeout)
        request = self._native.try_receive()
        if request is not None:
            return RpcRequest(request)
        if timeout == 0:
            return None
        loop = asyncio.get_running_loop()
        future = loop.create_future()
        receiver = self._native.begin_receive_async(loop, future)
        try:
            waitable = asyncio.shield(future)
            if timeout is None:
                await waitable
            else:
                await asyncio.wait_for(waitable, timeout)
        except asyncio.TimeoutError:
            won = receiver._cancel()
            future.cancel()
            if won:
                return None
            return RpcRequest(receiver._result())
        except asyncio.CancelledError:
            receiver._cancel()
            future.cancel()
            raise
        except BaseException:
            receiver._cancel()
            future.cancel()
            raise
        return RpcRequest(receiver._result())

    def _debug_async_registration_count(self) -> int:
        return int(self._native._debug_async_registration_count())

    @property
    def pressure(self) -> RpcPressure:
        return RpcPressure._from_native(self._native.pressure())

    def close(self) -> None:
        self._native.expire()


class LoanedImageSample:
    """An initialized writable Image payload loaned from shared memory.

    Each call to :meth:`numpy` or :meth:`payload_view` creates one lease root.
    Derived arrays and memoryviews retain that root through their normal base
    chain; the live-root count is what prevents :meth:`send` from consuming a
    sample while writable aliases still exist.
    """

    __slots__ = ("_owner",)

    def __init__(self, owner: Any) -> None:
        self._owner = owner

    @property
    def sequence(self) -> int:
        return self._owner.sequence

    @property
    def timestamp_ns(self) -> int:
        return self._owner.timestamp_ns

    @property
    def width(self) -> int:
        return self._owner.width

    @property
    def height(self) -> int:
        return self._owner.height

    @property
    def stride(self) -> int:
        return self._owner.stride

    @property
    def encoding(self) -> PixelFormat | int:
        value = self._owner.encoding
        try:
            return PixelFormat(value)
        except ValueError:
            return value

    @property
    def payload_nbytes(self) -> int:
        return self._owner.payload_nbytes

    def payload_view(self) -> memoryview:
        """Return a writable zero-copy byte view retaining this loan."""
        return memoryview(self._owner._new_lease())

    def numpy(self) -> np.ndarray[Any, np.dtype[np.uint8]]:
        """Return a writable, non-owning NumPy view of the Image payload."""
        width = self.width
        height = self.height
        stride = self.stride
        encoding = self.encoding
        if encoding in (PixelFormat.RGB8, PixelFormat.BGR8):
            channels = 3
            shape = (height, width, channels)
            strides = (stride, channels, 1)
        elif encoding == PixelFormat.GRAY8:
            channels = 1
            shape = (height, width)
            strides = (stride, 1)
        else:
            raise UnsupportedPixelFormatError(
                f"unsupported Image pixel format value {int(encoding)}"
            )
        return np.ndarray(
            shape=shape,
            dtype=np.uint8,
            buffer=self._owner._new_lease(),
            offset=0,
            strides=strides,
            order="C",
        )

    def send(self) -> None:
        """Consume and send this exact shared-memory allocation."""
        self._owner.send()

    def _debug_payload_address(self) -> int:
        return self._owner._debug_payload_address()

    def _debug_export_count(self) -> int:
        return self._owner._debug_export_count()

    def _copy_forbidden(self, *args: Any, **kwargs: Any) -> None:
        raise TypeError("publisher loan handles cannot be copied")

    __copy__ = _copy_forbidden
    __deepcopy__ = _copy_forbidden
    __reduce__ = _copy_forbidden
    __reduce_ex__ = _copy_forbidden

    def __repr__(self) -> str:
        try:
            return (
                f"<LoanedImageSample seq={self.sequence} shape={self.height}x{self.width} "
                f"encoding={self.encoding!s} mapped={self._debug_export_count() > 0}>"
            )
        except LoanConsumedError:
            return "<LoanedImageSample consumed>"


class LoanedPointCloudSample:
    """An initialized writable PointCloud payload loaned from shared memory."""

    __slots__ = ("_owner",)

    def __init__(self, owner: Any) -> None:
        self._owner = owner

    @property
    def sequence(self) -> int:
        return self._owner.sequence

    @property
    def timestamp_ns(self) -> int:
        return self._owner.timestamp_ns

    @property
    def width(self) -> int:
        return self._owner.width

    @property
    def height(self) -> int:
        return self._owner.height

    @property
    def dense(self) -> bool:
        return self._owner.dense

    @property
    def point_count(self) -> int:
        return self._owner.point_count

    @property
    def payload_nbytes(self) -> int:
        return self._owner.payload_nbytes

    @property
    def payload_alignment(self) -> int:
        return self._owner.payload_alignment

    def payload_view(self) -> memoryview:
        """Return a writable zero-copy byte view retaining this loan."""
        return memoryview(self._owner._new_lease())

    def numpy(self) -> np.ndarray[Any, np.dtype[np.void]]:
        """Return a writable, non-owning structured PointXYZI view."""
        address = self._debug_payload_address()
        alignment = self.payload_alignment
        if address % alignment != 0:
            raise InvalidPayloadAlignmentError(
                f"PointCloud payload address 0x{address:x} is not aligned to {alignment} bytes"
            )
        return np.ndarray(
            shape=(self.point_count,),
            dtype=POINT_XYZI_DTYPE,
            buffer=self._owner._new_lease(),
            offset=0,
            strides=(POINT_XYZI_DTYPE.itemsize,),
        )

    def send(self) -> None:
        """Consume and send this exact shared-memory allocation."""
        self._owner.send()

    def _debug_payload_address(self) -> int:
        return self._owner._debug_payload_address()

    def _debug_export_count(self) -> int:
        return self._owner._debug_export_count()

    def _copy_forbidden(self, *args: Any, **kwargs: Any) -> None:
        raise TypeError("publisher loan handles cannot be copied")

    __copy__ = _copy_forbidden
    __deepcopy__ = _copy_forbidden
    __reduce__ = _copy_forbidden
    __reduce_ex__ = _copy_forbidden

    def __repr__(self) -> str:
        try:
            return (
                f"<LoanedPointCloudSample seq={self.sequence} points={self.point_count} "
                f"mapped={self._debug_export_count() > 0}>"
            )
        except LoanConsumedError:
            return "<LoanedPointCloudSample consumed>"


class ImagePublisher:
    __slots__ = (
        "_native",
        "descriptor",
        "maximum_payload_bytes",
        "provider_component_id",
        "capability_instance_id",
        "activation_id",
    )

    def __init__(
        self,
        native: Any,
        descriptor: StreamDescriptor,
        maximum_payload_bytes: int,
        *,
        provider_component_id: str | None = None,
        capability_instance_id: str | None = None,
        activation_id: int | None = None,
    ) -> None:
        self._native = native
        self.descriptor = descriptor
        self.maximum_payload_bytes = maximum_payload_bytes
        self.provider_component_id = provider_component_id
        self.capability_instance_id = capability_instance_id
        self.activation_id = activation_id

    @classmethod
    def open(
        cls,
        descriptor: StreamDescriptor,
        *,
        maximum_payload_bytes: int = 6_220_800,
        max_loans: int | None = None,
        max_borrowed: int | None = None,
    ) -> ImagePublisher:
        descriptor, resources = _effective_resources(
            descriptor, max_loans=max_loans, max_borrowed=max_borrowed
        )
        maximum = _nonnegative_index(
            maximum_payload_bytes, "maximum_payload_bytes", ValueError
        )
        if maximum > sys.maxsize:
            raise ValueError("maximum_payload_bytes exceeds platform address space")
        native = _rbt._ImagePublisher.open(
            descriptor.to_json(),
            maximum,
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
        width: int,
        height: int,
        encoding: PixelFormat | int,
        *,
        stride: int | None = None,
        sequence: int = 0,
        timestamp_ns: int = 0,
    ) -> LoanedImageSample:
        (
            width_value,
            height_value,
            encoding_value,
            stride_value,
            _payload_len,
            _channels,
        ) = _checked_image_loan_layout(
            width,
            height,
            encoding,
            stride,
            self.maximum_payload_bytes,
        )
        native = self._native.loan(
            width_value,
            height_value,
            encoding_value,
            stride_value,
            _u64(sequence, "sequence"),
            _u64(timestamp_ns, "timestamp_ns"),
        )
        return LoanedImageSample(native)


class PointCloudPublisher:
    __slots__ = (
        "_native",
        "descriptor",
        "maximum_points",
        "provider_component_id",
        "capability_instance_id",
        "activation_id",
    )

    def __init__(
        self,
        native: Any,
        descriptor: StreamDescriptor,
        maximum_points: int,
        *,
        provider_component_id: str | None = None,
        capability_instance_id: str | None = None,
        activation_id: int | None = None,
    ) -> None:
        self._native = native
        self.descriptor = descriptor
        self.maximum_points = maximum_points
        self.provider_component_id = provider_component_id
        self.capability_instance_id = capability_instance_id
        self.activation_id = activation_id

    @classmethod
    def open(
        cls,
        descriptor: StreamDescriptor,
        *,
        maximum_points: int = 100_000,
        max_loans: int | None = None,
        max_borrowed: int | None = None,
    ) -> PointCloudPublisher:
        descriptor, resources = _effective_resources(
            descriptor, max_loans=max_loans, max_borrowed=max_borrowed
        )
        maximum = _nonnegative_index(maximum_points, "maximum_points", ValueError)
        if maximum > sys.maxsize:
            raise ValueError("maximum_points exceeds platform address space")
        native = _rbt._PointCloudPublisher.open(
            descriptor.to_json(),
            maximum,
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
        num_points: int,
        *,
        sequence: int = 0,
        timestamp_ns: int = 0,
        dense: bool = True,
    ) -> LoanedPointCloudSample:
        count = _nonnegative_index(num_points, "num_points", InvalidPayloadError)
        if count > (1 << 32) - 1:
            raise InvalidPayloadError("PointCloud point count exceeds the u32 width ABI")
        if count > self.maximum_points:
            raise InvalidPayloadError(
                f"invalid payload length {count}; maximum is {self.maximum_points}"
            )
        if not isinstance(dense, (bool, np.bool_)):
            raise TypeError("dense must be a boolean")
        native = self._native.loan(
            count,
            _u64(sequence, "sequence"),
            _u64(timestamp_ns, "timestamp_ns"),
            bool(dense),
        )
        return LoanedPointCloudSample(native)


def _nonempty_text(value: Any, name: str) -> str:
    if not isinstance(value, str):
        raise TypeError(f"{name} must be a string")
    if not value.strip():
        raise ValueError(f"{name} must be non-empty")
    return value


def _coerce_message_descriptor(value: Any) -> MessageTypeDescriptor:
    if isinstance(value, MessageTypeDescriptor):
        return value
    if isinstance(value, MappingABC):
        return MessageTypeDescriptor.from_dict(value)
    descriptor_factory = getattr(value, "descriptor", None)
    if callable(descriptor_factory):
        descriptor = descriptor_factory()
        if isinstance(descriptor, MessageTypeDescriptor):
            return descriptor
        if isinstance(descriptor, MappingABC):
            return MessageTypeDescriptor.from_dict(descriptor)
    descriptor = getattr(value, "DESCRIPTOR", None)
    if isinstance(descriptor, MessageTypeDescriptor):
        return descriptor
    if isinstance(descriptor, MappingABC):
        return MessageTypeDescriptor.from_dict(descriptor)
    raise TypeError("message type must be a MessageTypeDescriptor or descriptor mapping")


def _normalise_requirements(value: Any) -> list[tuple[str, str]]:
    """Validate the small requirement wire representation."""
    if value is None:
        return []
    if isinstance(value, MappingABC):
        entries = value.items()
    elif isinstance(value, IterableABC) and not isinstance(
        value, (str, bytes, bytearray)
    ):
        entries = value
    else:
        raise TypeError("requirements must be a mapping or iterable of pairs")

    result: list[tuple[str, str]] = []
    names: set[str] = set()
    for entry in entries:
        if isinstance(entry, MappingABC) or isinstance(
            entry, (str, bytes, bytearray)
        ):
            raise TypeError("each requirement must be a (name, capability_type) pair")
        try:
            pair = tuple(entry)
        except TypeError as error:
            raise TypeError(
                "each requirement must be a (name, capability_type) pair"
            ) from error
        if len(pair) != 2:
            raise ValueError("each requirement must contain exactly two values")
        requirement_name = _nonempty_text(pair[0], "requirement name")
        capability_type = _nonempty_text(pair[1], "capability type")
        if requirement_name in names:
            raise ValueError(f"duplicate requirement name {requirement_name!r}")
        names.add(requirement_name)
        result.append((requirement_name, capability_type))
    return result


def _native_value(native: Any, *names: str) -> Any:
    for name in names:
        try:
            value = getattr(native, name)
        except AttributeError:
            continue
        return value() if callable(value) else value
    raise AttributeError(
        f"native context does not expose any of: {', '.join(names)}"
    )


def _subscriber_from_endpoint(
    value: Any, activation_id: int
) -> ImageSubscriber | PointCloudSubscriber | RpcClient:
    """Wrap the exact activation-bound endpoint returned by the native adapter."""
    if not isinstance(value, tuple) or len(value) != 5:
        raise ComponentRuntimeError(
            "native require() returned an invalid endpoint contract"
        )
    kind, native, descriptor_json, provider_component_id, capability_instance_id = value
    metadata = {
        "provider_component_id": str(provider_component_id),
        "capability_instance_id": str(capability_instance_id),
        "activation_id": activation_id,
    }
    if kind == "rpc":
        return RpcClient(
            native,
            RpcDescriptor.from_json(descriptor_json),
            **metadata,
        )
    descriptor = StreamDescriptor.from_json(descriptor_json)
    if kind == "image":
        return ImageSubscriber(native, descriptor, **metadata)
    if kind == "point_cloud":
        return PointCloudSubscriber(native, descriptor, **metadata)
    raise ComponentRuntimeError(
        f"native require() returned unsupported endpoint kind {kind!r}"
    )


def _publisher_from_endpoint(
    value: Any,
    kind: str,
    maximum: int,
    component_id: str,
    activation_id: int,
) -> ImagePublisher | PointCloudPublisher | RpcServer:
    """Wrap a native activation-owned publisher without reopening its service."""
    if not isinstance(value, tuple) or len(value) != 3:
        raise ComponentRuntimeError(
            f"native provide_{kind}() returned an invalid endpoint contract"
        )
    native, descriptor_json, capability_instance_id = value
    if kind == "rpc":
        return RpcServer(
            native,
            RpcDescriptor.from_json(descriptor_json),
            capability_instance_id=str(capability_instance_id),
        )
    descriptor = StreamDescriptor.from_json(descriptor_json)
    metadata = {
        "provider_component_id": component_id,
        "capability_instance_id": str(capability_instance_id),
        "activation_id": activation_id,
    }
    if kind == "image":
        return ImagePublisher(native, descriptor, maximum, **metadata)
    if kind == "point_cloud":
        return PointCloudPublisher(native, descriptor, maximum, **metadata)
    raise ComponentRuntimeError(f"unsupported publisher endpoint kind {kind!r}")


class Component:
    """Small synchronous base class for a native-managed component."""

    def prepare(self, config: Any) -> None:
        """Prepare process-lifetime state once before the first activation."""
        return None

    def activate(self, ctx: ActivationContext) -> None:
        """Activate against one resolved binding set."""
        raise NotImplementedError


class TaskContext:
    """Cooperative cancellation view passed to an activation task."""

    __slots__ = ("_native",)

    def __init__(self, native: Any) -> None:
        self._native = native

    @property
    def cancelled(self) -> bool:
        return bool(_native_value(self._native, "cancelled", "is_cancelled"))

    def is_cancelled(self) -> bool:
        return self.cancelled


class _AsyncTaskFailure(Exception):
    """Normal Exception wrapper keeping BaseException out of asyncio internals."""

    __slots__ = ("_rbt_original",)

    def __init__(self, original: BaseException) -> None:
        super().__init__(str(original))
        self._rbt_original = original


async def _run_supervised_coroutine(
    coroutine_function: Any,
    context: TaskContext,
    args: tuple[Any, ...],
) -> Any:
    """Run one user coroutine while preserving asyncio cancellation semantics.

    asyncio deliberately lets ``SystemExit`` and ``KeyboardInterrupt`` escape
    a Task's step callback.  Converting those user failures to an ordinary
    private Exception keeps the managed loop alive; the native supervisor
    unwraps ``_rbt_original`` for the original class/message/traceback.
    """
    try:
        awaitable = coroutine_function(context, *args)
        return await awaitable
    except asyncio.CancelledError:
        raise
    except BaseException as error:
        raise _AsyncTaskFailure(error) from None


class AsyncTaskHandle:
    """Handle for one native-supervised asyncio task."""

    __slots__ = ("_native",)

    def __init__(self, native: Any) -> None:
        self._native = native

    def cancel(self) -> None:
        self._native.cancel()

    @property
    def cancelled(self) -> bool:
        return bool(_native_value(self._native, "cancelled", "is_cancelled"))

    @property
    def done(self) -> bool:
        return bool(_native_value(self._native, "done", "is_done"))


class ActivationContext:
    """Thin public view over one native activation context."""

    __slots__ = ("_native",)

    def __init__(self, native: Any) -> None:
        self._native = native

    @property
    def component_id(self) -> str:
        return _native_value(self._native, "component_id", "component")

    @property
    def process_instance_id(self) -> str:
        return _native_value(self._native, "process_instance_id", "process_instance", "process")

    @property
    def activation_id(self) -> int:
        return _native_value(self._native, "activation_id", "activation")

    @property
    def component(self) -> str:
        return self.component_id

    @property
    def process_instance(self) -> str:
        return self.process_instance_id

    @property
    def cancelled(self) -> bool:
        return bool(_native_value(self._native, "cancelled", "is_cancelled"))

    def require(
        self,
        name: str,
        *,
        max_borrowed: int | None = None,
    ) -> ImageSubscriber | PointCloudSubscriber | RpcClient:
        requirement_name = _nonempty_text(name, "requirement name")
        require = getattr(self._native, "require")
        if max_borrowed is None:
            endpoint = require(requirement_name)
        else:
            requested = _resource_limit(max_borrowed, "max_borrowed")
            endpoint = require(requirement_name, max_borrowed=requested)
        return _subscriber_from_endpoint(endpoint, self.activation_id)

    def require_rpc(self, name: str) -> RpcClient:
        """Resolve one fixed-size RPC requirement for this activation."""
        requirement_name = _nonempty_text(name, "requirement name")
        endpoint = self._native.require_rpc(requirement_name)
        wrapped = _subscriber_from_endpoint(endpoint, self.activation_id)
        if not isinstance(wrapped, RpcClient):
            raise ComponentRuntimeError(
                f"native require_rpc() returned endpoint kind {type(wrapped).__name__!r}"
            )
        return wrapped

    def provide_dynamic_tf(self, capability_type: str) -> Any:
        """Stage a typed dynamic TF publisher owned by this activation.

        The stream is not drained by an implicit runtime task. A component
        that consumes it should use ``ctx.spawn_background`` with the
        endpoint's bounded ``wait_and_drain`` method.
        """
        from .tf import DynamicTfPublisher, _capability_uuid

        capability = _nonempty_text(capability_type, "capability type")
        native, descriptor_json, physical_id = self._native.provide_dynamic_tf(
            capability
        )
        return DynamicTfPublisher(
            native,
            StreamDescriptor.from_json(descriptor_json),
            _capability_uuid(physical_id, "authority"),
        )

    def provide_static_tf(
        self,
        update_role: str,
        snapshot_role: str,
        initial: Any,
        maximum_transforms: int,
    ) -> Any:
        """Stage one paired static update stream and snapshot RPC.

        The provider endpoint is activation-owned. Components should run
        ``serve_one`` from a managed ``ctx.spawn_background`` task so scope
        shutdown can expire its bounded RPC wait.
        """
        from .tf import (
            StaticTfProvider,
            _capability_uuid,
            _require_static_set,
            _static_transform_bound,
        )

        update_capability = _nonempty_text(update_role, "update role")
        snapshot_capability = _nonempty_text(snapshot_role, "snapshot role")
        if update_capability == snapshot_capability:
            from .tf import TfTransportError

            raise TfTransportError(
                "pairing_invalid",
                "static update and snapshot must use distinct capability roles",
            )
        selected_initial = _require_static_set(initial)
        maximum = _static_transform_bound(maximum_transforms)
        (
            native,
            update_json,
            snapshot_json,
            update_id,
            snapshot_id,
            maximum,
        ) = self._native.provide_static_tf(
            update_capability,
            snapshot_capability,
            selected_initial,
            maximum,
        )
        return StaticTfProvider(
            native,
            StreamDescriptor.from_json(update_json),
            RpcDescriptor.from_json(snapshot_json),
            _capability_uuid(update_id, "update_authority"),
            _capability_uuid(snapshot_id, "snapshot_capability"),
            maximum,
        )

    def require_dynamic_tf(self, requirement: str, slot: Any, buffer: Any) -> Any:
        """Resolve one canonical dynamic TF stream into a buffer slot.

        Run repeated bounded ``wait_and_drain`` calls in a managed
        ``ctx.spawn_background`` task when the component needs continuous
        ingestion. The native wait is event-driven and is woken at scope end.
        """
        from .tf import (
            DynamicTfSubscriber,
            _authority_from_native,
            _capability_uuid,
            _frame,
            _require_buffer,
        )

        requirement_name = _nonempty_text(requirement, "requirement name")
        slot_id = _frame(slot, "slot")
        selected_buffer = _require_buffer(buffer)
        native, descriptor_json = self._native.require_dynamic_tf(
            requirement_name,
            str(slot_id),
            selected_buffer._native,
        )
        return DynamicTfSubscriber(
            native,
            StreamDescriptor.from_json(descriptor_json),
            _authority_from_native(native.authority),
            _capability_uuid(native.physical_authority, "physical_authority"),
            selected_buffer,
        )

    def require_static_tf(
        self,
        update_requirement: str,
        snapshot_requirement: str,
        slot: Any,
        buffer: Any,
        maximum_transforms: int,
    ) -> Any:
        """Resolve a paired static TF stream/RPC into a buffer slot.

        Start a managed ``ctx.spawn_background`` task to call bounded snapshot
        refreshes and ``wait_and_drain`` for update ingestion. Scope shutdown
        wakes both waits and unbinds the local authority.
        """
        from .tf import (
            StaticTfClient,
            _authority_from_native,
            _capability_uuid,
            _frame,
            _require_buffer,
            _static_transform_bound,
        )

        update_name = _nonempty_text(update_requirement, "update requirement")
        snapshot_name = _nonempty_text(snapshot_requirement, "snapshot requirement")
        if update_name == snapshot_name:
            from .tf import TfTransportError

            raise TfTransportError(
                "pairing_invalid",
                "static update and snapshot must use distinct requirements",
            )
        slot_id = _frame(slot, "slot")
        selected_buffer = _require_buffer(buffer)
        maximum = _static_transform_bound(maximum_transforms)
        (
            native,
            update_json,
            snapshot_json,
            update_id,
            _snapshot_id,
            maximum,
        ) = self._native.require_static_tf(
            update_name,
            snapshot_name,
            str(slot_id),
            selected_buffer._native,
            maximum,
        )
        return StaticTfClient(
            native,
            StreamDescriptor.from_json(update_json),
            RpcDescriptor.from_json(snapshot_json),
            _authority_from_native(native.authority),
            _capability_uuid(update_id, "physical_authority"),
            maximum,
            selected_buffer,
        )

    def provide_image(
        self,
        capability_type: str,
        *,
        maximum_payload_bytes: int = 6_220_800,
        max_loans: int | None = None,
        max_borrowed: int | None = None,
    ) -> ImagePublisher:
        capability = _nonempty_text(capability_type, "capability type")
        descriptor = StreamDescriptor.image("placeholder")
        # The native context owns the physical service name; this temporary
        # descriptor is only used to apply the same omission/override rules as
        # standalone publishers before crossing the PyO3 boundary.
        _, resources = _effective_resources(
            descriptor, max_loans=max_loans, max_borrowed=max_borrowed
        )
        maximum = _nonnegative_index(
            maximum_payload_bytes, "maximum_payload_bytes", ValueError
        )
        if maximum > sys.maxsize:
            raise ValueError("maximum_payload_bytes exceeds platform address space")
        kwargs: dict[str, Any] = {"maximum_payload_bytes": maximum}
        if max_loans is not None or max_borrowed is not None:
            kwargs.update(
                max_loans=resources.max_loans,
                max_borrowed=resources.max_borrowed,
            )
        result = self._native.provide_image(capability, **kwargs)
        return _publisher_from_endpoint(
            result, "image", maximum, self.component_id, self.activation_id
        )

    def provide_point_cloud(
        self,
        capability_type: str,
        *,
        maximum_points: int = 100_000,
        max_loans: int | None = None,
        max_borrowed: int | None = None,
    ) -> PointCloudPublisher:
        capability = _nonempty_text(capability_type, "capability type")
        descriptor = StreamDescriptor.point_cloud("placeholder")
        _, resources = _effective_resources(
            descriptor, max_loans=max_loans, max_borrowed=max_borrowed
        )
        maximum = _nonnegative_index(maximum_points, "maximum_points", ValueError)
        if maximum > sys.maxsize:
            raise ValueError("maximum_points exceeds platform address space")
        kwargs = {"maximum_points": maximum}
        if max_loans is not None or max_borrowed is not None:
            kwargs.update(
                max_loans=resources.max_loans,
                max_borrowed=resources.max_borrowed,
            )
        result = self._native.provide_point_cloud(capability, **kwargs)
        return _publisher_from_endpoint(
            result, "point_cloud", maximum, self.component_id, self.activation_id
        )

    def provide_rpc(
        self,
        capability_type: str,
        descriptor: RpcDescriptor,
    ) -> RpcServer:
        """Publish a canonical fixed request/response RPC capability."""
        capability = _nonempty_text(capability_type, "capability type")
        if not isinstance(descriptor, RpcDescriptor):
            raise TypeError("descriptor must be an RpcDescriptor")
        result = self._native.provide_rpc(capability, descriptor.to_json())
        wrapped = _publisher_from_endpoint(
            result, "rpc", 0, self.component_id, self.activation_id
        )
        if not isinstance(wrapped, RpcServer):
            raise ComponentRuntimeError(
                f"native provide_rpc() returned endpoint kind {type(wrapped).__name__!r}"
            )
        return wrapped

    def _spawn(self, method_name: str, name: str, task: Any) -> Any:
        task_name = _nonempty_text(name, "task name")
        if not callable(task):
            raise TypeError("task must be callable")

        def invoke(native_task_context: Any) -> Any:
            context = (
                native_task_context
                if isinstance(native_task_context, TaskContext)
                else TaskContext(native_task_context)
            )
            return task(context)

        return getattr(self._native, method_name)(task_name, invoke)

    def spawn_critical(self, name: str, callable: Any) -> Any:
        return self._spawn("spawn_critical", name, callable)

    def spawn_background(self, name: str, callable: Any) -> Any:
        return self._spawn("spawn_background", name, callable)

    def _spawn_async(
        self, method_name: str, name: str, coroutine_function: Any, args: tuple[Any, ...]
    ) -> AsyncTaskHandle:
        task_name = _nonempty_text(name, "task name")
        if not callable(coroutine_function):
            raise TypeError("coroutine_function must be callable")

        def factory(native_task_context: Any) -> Any:
            context = (
                native_task_context
                if isinstance(native_task_context, TaskContext)
                else TaskContext(native_task_context)
            )
            # Both the function call and its await happen inside the managed
            # Task.  This produces a fresh one-shot coroutine per activation
            # and safely contains user BaseException subclasses.
            return _run_supervised_coroutine(coroutine_function, context, args)

        native_handle = getattr(self._native, method_name)(task_name, factory)
        return AsyncTaskHandle(native_handle)

    def spawn_critical_async(
        self, name: str, coroutine_function: Any, *args: Any
    ) -> AsyncTaskHandle:
        return self._spawn_async(
            "spawn_critical_async", name, coroutine_function, args
        )

    def spawn_background_async(
        self, name: str, coroutine_function: Any, *args: Any
    ) -> AsyncTaskHandle:
        return self._spawn_async(
            "spawn_background_async", name, coroutine_function, args
        )


class _ComponentAdapter:
    """Native callback adapter retaining one user component instance."""

    __slots__ = ("_component",)

    def __init__(self, component: Component) -> None:
        self._component = component

    def prepare(self, config: Any) -> Any:
        return self._component.prepare(config)

    def activate(self, native_context: Any) -> Any:
        context = (
            native_context
            if isinstance(native_context, ActivationContext)
            else ActivationContext(native_context)
        )
        return self._component.activate(context)


def run_component(
    component: Component,
    *,
    component_id: str | None = None,
    requirements: Any = None,
    config: Any = None,
    composer_address: str | None = None,
) -> Any:
    """Run a synchronous Python component using the native runtime."""
    if not isinstance(component, Component):
        raise TypeError("component must be a Component")

    selected_id = component_id
    if selected_id is None:
        selected_id = getattr(component, "component_id", None)
    selected_id = _nonempty_text(selected_id, "component_id")

    selected_requirements = requirements
    if selected_requirements is None:
        selected_requirements = getattr(component, "requirements", None)
        if callable(selected_requirements):
            selected_requirements = selected_requirements()
    normalised_requirements = _normalise_requirements(selected_requirements)

    address = composer_address
    if address is None:
        address = os.environ.get("RBT_COMPOSER_ADDR") or "127.0.0.1:7447"
    address = _nonempty_text(address, "composer_address")

    native_runner = getattr(_rbt, "_run_component", None)
    if native_runner is None:
        raise ComponentRuntimeError(
            "the installed rbt extension does not provide the Component runtime"
        )

    adapter = _ComponentAdapter(component)
    try:
        return native_runner(
            adapter,
            selected_id,
            normalised_requirements,
            config,
            address,
        )
    except KeyboardInterrupt:
        # The native runner performs graceful shutdown before reporting a
        # process-main interrupt.  Worker exceptions are captured by native
        # task supervision and are intentionally not handled here.
        return None


__all__ = [
    "__version__",
    "AbiMismatchError",
    "ActivationContext",
    "ActivationExpiredError",
    "AsyncRuntimeClosedError",
    "AsyncTaskHandle",
    "BorrowLimitReachedError",
    "Component",
    "ComponentActivationError",
    "ComponentPreparationError",
    "ComponentRuntimeError",
    "ConcurrentReceiveError",
    "LoanConsumedError",
    "LoanPoolExhaustedError",
    "LoanStillMappedError",
    "IMAGE_DESCRIPTOR",
    "ImagePublisher",
    "ImageSubscriber",
    "IncompatibleResourceConfig",
    "IncompatibleResourceConfigError",
    "InvalidImageLayoutError",
    "InvalidPayloadAlignmentError",
    "InvalidPayloadError",
    "InvalidResourceConfig",
    "InvalidResourceConfigError",
    "MessageShape",
    "MessageTypeDescriptor",
    "MissingTransportFeatureError",
    "POINT_CLOUD_DESCRIPTOR",
    "POINT_XYZI_DTYPE",
    "PixelFormat",
    "PointCloudPublisher",
    "PointCloudSubscriber",
    "PublisherPressure",
    "QueueFullError",
    "ReadinessNotifyError",
    "RbtError",
    "ReadinessProtocol",
    "RpcActivationExpiredError",
    "RpcCancelledError",
    "RpcCapacityError",
    "RpcClient",
    "RpcClientCapacityError",
    "RpcClientPressure",
    "RpcDeadlineExceededError",
    "RpcDescriptor",
    "RpcError",
    "RpcPressure",
    "RpcReadinessProtocol",
    "RpcRequest",
    "RpcRequestLoan",
    "RpcResponse",
    "RpcResponseCapacityError",
    "RpcResponseLoan",
    "RpcServer",
    "RpcServerCapacityError",
    "RpcServerPressure",
    "RpcServerUnavailableError",
    "RpcTransportError",
    "RpcUnavailableError",
    "RpcResourceConfig",
    "RpcTransportFeatures",
    "ReceivedImageSample",
    "ReceivedPointCloudSample",
    "LoanedImageSample",
    "LoanedPointCloudSample",
    "StreamDescriptor",
    "StreamPressure",
    "StreamProfile",
    "StreamResourceConfig",
    "SubscriberPressure",
    "TransportFeatures",
    "UnableToDeliverError",
    "UnsupportedPixelFormatError",
    "WaitError",
    "WaitStats",
    "TaskContext",
    "TaskError",
    "run_component",
]

# Pure-Python time types intentionally stay independent of runtime deadlines
# and the native adapter.
from .time import (
    ClockDiscontinuity,
    ClockIdentity,
    ClockKind,
    ClockMapping,
    ClockMappingCalibration,
    ClockSourceId,
    ConvertedTimestamp,
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

__all__ += [
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

# In-sample timing metadata and zero-copy stamped byte endpoints.
from .time_stream import (
    TIMING_PREFIX_LEN as TIMING_PREFIX_LEN,
    StampedBytesLoan as StampedBytesLoan,
    StampedBytesPublisher as StampedBytesPublisher,
    StampedBytesSample as StampedBytesSample,
    StampedBytesSubscriber as StampedBytesSubscriber,
    decode_timing_prefix as decode_timing_prefix,
    encode_timing_prefix as encode_timing_prefix,
    split_payload as split_payload,
    stamped_bytes_descriptor as stamped_bytes_descriptor,
)

__all__ += [
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

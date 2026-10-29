"""Local, bounded transform lookup backed by the safe Rust ``rbt-tf`` buffer.

Duration configuration values are seconds. Coordinate values and frame/math
types are re-exported from :mod:`rbt.tf_math`.
"""

from __future__ import annotations

import math
import numbers
import uuid
from collections.abc import Sequence
from dataclasses import dataclass, field
from typing import Any

from . import (
    MessageTypeDescriptor,
    RpcDescriptor,
    RpcResourceConfig,
    RpcTransportFeatures,
    StreamDescriptor,
    StreamProfile,
    StreamResourceConfig,
    TransportFeatures,
    _rbt,
)
from ._generated.tf import (
    DYNAMIC_TRANSFORM_DESCRIPTOR,
    STATIC_SNAPSHOT_DESCRIPTOR,
    STATIC_SNAPSHOT_REQUEST_DESCRIPTOR,
    STATIC_UPDATE_DESCRIPTOR,
)
from .time import (
    ClockIdentity,
    ClockKind,
    ClockMapping,
    ClockSourceId,
    Timestamp,
)
from .tf_math import FrameId, Pose3, Quaternion, TfError, Transform3, Vec3

_MAX_PROVIDERS = 256
_MAX_EDGES = 4096
_MAX_SAMPLES = 1_000_000
_MAX_PENDING_UPDATES = 4096
_MAX_STATIC_TRANSFORMS = 4096
_MAX_MAPPINGS = 256
_U64_MAX = (1 << 64) - 1


class TfTransportError(Exception):
    """A native TF endpoint or transport operation failed."""

    def __init__(self, code: str, message: str = "") -> None:
        self.code = code
        super().__init__(message or code)


@dataclass(frozen=True, slots=True)
class TfDrainReport:
    """Counts and typed errors from one bounded native stream drain."""

    received: int
    accepted: int
    rejected: int
    failures: tuple[TfError, ...] = ()

    @classmethod
    def _from_native(cls, value: Any) -> TfDrainReport:
        if not isinstance(value, dict):
            raise _error("invalid_config", "native drain report is malformed")
        failures = value.get("failures", ())
        if not isinstance(failures, (tuple, list)):
            raise _error("invalid_config", "native drain failures are malformed")
        return cls(
            _uint(value.get("received"), "received"),
            _uint(value.get("accepted"), "accepted"),
            _uint(value.get("rejected"), "rejected"),
            tuple(TfError(code, message) for code, message in failures),
        )


def _error(code: str, message: str) -> TfError:
    return TfError(code, message)


def _uint(value: object, name: str, maximum: int = _U64_MAX) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise _error("invalid_config", f"{name} must be an integer")
    if not 0 <= value <= maximum:
        raise _error("invalid_config", f"{name} is outside its supported range")
    return value


def _positive_limit(value: object, name: str, maximum: int) -> int:
    result = _uint(value, name, maximum)
    if result == 0:
        raise _error("invalid_config", f"{name} must be greater than zero")
    return result


def _seconds(value: object, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, numbers.Real):
        raise _error("invalid_config", f"{name} must be a duration in seconds")
    try:
        result = float(value)
    except (OverflowError, TypeError, ValueError) as error:
        raise _error("invalid_config", f"{name} must be finite and greater than zero") from error
    if not math.isfinite(result) or result <= 0.0:
        raise _error("invalid_config", f"{name} must be finite and greater than zero")
    return result


def _frame(value: FrameId | str, name: str) -> FrameId:
    if isinstance(value, FrameId):
        return value
    if isinstance(value, str):
        return FrameId(value)
    raise _error("invalid_frame_id", f"{name} must be a FrameId or string")


def _optional_uncertainty(value: int | None, name: str) -> int | None:
    if value is None:
        return None
    return _uint(value, name)


@dataclass(frozen=True, slots=True)
class TfResourceConfig:
    """Bounded local buffer capacities and freshness limits.

    Duration fields are finite positive seconds and default to the corresponding
    Rust ``Duration`` values.
    """

    max_providers: int = 16
    max_total_edges: int = 128
    max_samples_per_edge: int = 256
    max_total_samples: int = 8192
    max_static_transforms_per_provider: int = 64
    max_pending_static_updates: int = 8
    max_history_duration: float = 10.0
    resync_interval: float = 1.0
    snapshot_max_age: float = 5.0
    max_dynamic_age: float = 5.0

    def __post_init__(self) -> None:
        object.__setattr__(self, "max_providers", _positive_limit(self.max_providers, "max_providers", _MAX_PROVIDERS))
        object.__setattr__(self, "max_total_edges", _positive_limit(self.max_total_edges, "max_total_edges", _MAX_EDGES))
        object.__setattr__(self, "max_samples_per_edge", _positive_limit(self.max_samples_per_edge, "max_samples_per_edge", _MAX_SAMPLES))
        object.__setattr__(self, "max_total_samples", _positive_limit(self.max_total_samples, "max_total_samples", _MAX_SAMPLES))
        object.__setattr__(
            self,
            "max_static_transforms_per_provider",
            _positive_limit(self.max_static_transforms_per_provider, "max_static_transforms_per_provider", _MAX_EDGES),
        )
        object.__setattr__(
            self,
            "max_pending_static_updates",
            _positive_limit(self.max_pending_static_updates, "max_pending_static_updates", _MAX_PENDING_UPDATES),
        )
        if self.max_samples_per_edge > self.max_total_samples:
            raise _error("invalid_config", "max_samples_per_edge cannot exceed max_total_samples")
        if self.max_static_transforms_per_provider > self.max_total_edges:
            raise _error("invalid_config", "max_static_transforms_per_provider cannot exceed max_total_edges")
        for name in (
            "max_history_duration",
            "resync_interval",
            "snapshot_max_age",
            "max_dynamic_age",
        ):
            object.__setattr__(self, name, _seconds(getattr(self, name), name))


@dataclass(frozen=True, slots=True)
class LookupPolicy:
    """Precision and interpolation rules applied by local lookups."""

    max_timestamp_uncertainty_ns: int | None = 1_000_000
    max_interpolation_gap: float = 1.0
    allow_unknown_uncertainty: bool = False

    def __post_init__(self) -> None:
        object.__setattr__(
            self,
            "max_timestamp_uncertainty_ns",
            _optional_uncertainty(self.max_timestamp_uncertainty_ns, "max_timestamp_uncertainty_ns"),
        )
        object.__setattr__(self, "max_interpolation_gap", _seconds(self.max_interpolation_gap, "max_interpolation_gap"))
        if not isinstance(self.allow_unknown_uncertainty, bool):
            raise _error("invalid_config", "allow_unknown_uncertainty must be a bool")


@dataclass(frozen=True, slots=True)
class Authority:
    """A provider slot's current physical capability binding."""

    slot: FrameId
    physical_id: uuid.UUID
    _native: Any = field(default=None, init=False, compare=False, repr=False)

    def __post_init__(self) -> None:
        object.__setattr__(self, "slot", _frame(self.slot, "slot"))
        physical = self.physical_id
        if isinstance(physical, str):
            try:
                physical = uuid.UUID(physical)
            except (ValueError, AttributeError) as error:
                raise _error("invalid_config", "physical_id must be a UUID") from error
        if not isinstance(physical, uuid.UUID) or physical.int == 0:
            raise _error("invalid_config", "physical_id must be a non-nil UUID")
        object.__setattr__(self, "physical_id", physical)

    @property
    def physical(self) -> uuid.UUID:
        """Return the physical capability UUID."""
        return self.physical_id

    @classmethod
    def _bound(cls, slot: FrameId, physical_id: uuid.UUID, native: Any) -> Authority:
        value = cls(slot, physical_id)
        object.__setattr__(value, "_native", native)
        return value

    def _handle(self) -> Any:
        if self._native is None:
            raise _error("provider_unavailable", "authority was not returned by a TransformBuffer")
        return self._native


@dataclass(frozen=True, slots=True)
class StaticEdge:
    """A static ``T_parent<-child`` transform."""

    parent: FrameId
    child: FrameId
    transform: Transform3

    def __post_init__(self) -> None:
        object.__setattr__(self, "parent", _frame(self.parent, "parent"))
        object.__setattr__(self, "child", _frame(self.child, "child"))
        if not isinstance(self.transform, Transform3):
            raise _error("invalid_transform", "transform must be a Transform3")


@dataclass(frozen=True, slots=True)
class DynamicRecord:
    """One identified dynamic ``T_parent<-child`` sample."""

    parent: FrameId
    child: FrameId
    stamp: Timestamp
    uncertainty_ns: int | None
    transform_epoch: int
    transform: Transform3

    def __post_init__(self) -> None:
        object.__setattr__(self, "parent", _frame(self.parent, "parent"))
        object.__setattr__(self, "child", _frame(self.child, "child"))
        if not isinstance(self.stamp, Timestamp):
            raise _error("invalid_transform", "stamp must be a Timestamp")
        object.__setattr__(self, "uncertainty_ns", _optional_uncertainty(self.uncertainty_ns, "uncertainty_ns"))
        object.__setattr__(self, "transform_epoch", _uint(self.transform_epoch, "transform_epoch"))
        if not isinstance(self.transform, Transform3):
            raise _error("invalid_transform", "transform must be a Transform3")


@dataclass(frozen=True, slots=True)
class StaticSet:
    """The complete static state at one authority-local revision."""

    revision: int
    effective_from: Timestamp
    effective_uncertainty_ns: int | None
    transforms: tuple[StaticEdge, ...]

    def __post_init__(self) -> None:
        object.__setattr__(self, "revision", _uint(self.revision, "revision"))
        if not isinstance(self.effective_from, Timestamp):
            raise _error("invalid_transform", "effective_from must be a Timestamp")
        object.__setattr__(
            self,
            "effective_uncertainty_ns",
            _optional_uncertainty(self.effective_uncertainty_ns, "effective_uncertainty_ns"),
        )
        if not isinstance(self.transforms, (list, tuple)):
            raise _error("snapshot_too_large", "transforms must be a bounded list or tuple")
        if len(self.transforms) > _MAX_STATIC_TRANSFORMS:
            raise _error("snapshot_too_large", "static set exceeds the 4096 transform hard limit")
        transforms = tuple(self.transforms)
        if any(not isinstance(edge, StaticEdge) for edge in transforms):
            raise _error("invalid_transform", "transforms must contain StaticEdge values")
        object.__setattr__(self, "transforms", transforms)


@dataclass(frozen=True, slots=True)
class TransformSource:
    """Temporal and authority provenance for one resolved graph edge."""

    authority: Authority
    parent: FrameId
    child: FrameId
    timestamp: Timestamp
    bracket_start: Timestamp
    bracket_end: Timestamp
    static_revision: int | None
    transform_epoch: int | None


@dataclass(frozen=True, slots=True)
class LookupResult:
    """A resolved transform and the exact temporal sources used."""

    transform: Transform3
    requested_timestamp: Timestamp
    timing_uncertainty_ns: int | None
    sources: tuple[TransformSource, ...]


def _time_from_native(value: tuple[str, bytes, int, int]) -> Timestamp:
    kind, source, generation, nanoseconds = value
    identity = ClockIdentity(ClockKind(kind), ClockSourceId(source), generation)
    return Timestamp(identity, nanoseconds)


def _transform_from_native(value: dict[str, Any]) -> Transform3:
    return Transform3(
        Vec3(*value["translation"]),
        Quaternion(*value["rotation"]),
    )


def _authority_from_native(value: tuple[str, str]) -> Authority:
    slot, physical_id = value
    return Authority(FrameId(slot), uuid.UUID(physical_id))


def _lookup_from_native(value: dict[str, Any]) -> LookupResult:
    sources = tuple(
        TransformSource(
            authority=_authority_from_native(source["authority"]),
            parent=FrameId(source["parent"]),
            child=FrameId(source["child"]),
            timestamp=_time_from_native(source["timestamp"]),
            bracket_start=_time_from_native(source["bracket_start"]),
            bracket_end=_time_from_native(source["bracket_end"]),
            static_revision=source["static_revision"],
            transform_epoch=source["transform_epoch"],
        )
        for source in value["sources"]
    )
    return LookupResult(
        transform=_transform_from_native(value["transform"]),
        requested_timestamp=_time_from_native(value["requested_timestamp"]),
        timing_uncertainty_ns=value["timing_uncertainty_ns"],
        sources=sources,
    )


class TransformBuffer:
    """Thread-safe local transform graph backed by Rust's bounded buffer."""

    def __init__(
        self,
        resources: TfResourceConfig | None = None,
        policy: LookupPolicy | None = None,
    ) -> None:
        self.resources = TfResourceConfig() if resources is None else resources
        self.policy = LookupPolicy() if policy is None else policy
        if not isinstance(self.resources, TfResourceConfig):
            raise _error("invalid_config", "resources must be a TfResourceConfig")
        if not isinstance(self.policy, LookupPolicy):
            raise _error("invalid_config", "policy must be a LookupPolicy")
        self._native = _rbt._TfTransformBuffer(self.resources, self.policy)

    def bind_provider(self, slot: str, physical_id: uuid.UUID | str) -> Authority:
        """Bind a logical slot to one physical capability UUID."""
        if not isinstance(slot, str):
            raise _error("invalid_frame_id", "slot must be a string")
        if isinstance(physical_id, str):
            try:
                physical = uuid.UUID(physical_id)
            except (ValueError, AttributeError) as error:
                raise _error("invalid_config", "physical_id must be a UUID") from error
        elif isinstance(physical_id, uuid.UUID):
            physical = physical_id
        else:
            raise _error("invalid_config", "physical_id must be a UUID or UUID string")
        if physical.int == 0:
            raise _error("invalid_config", "physical_id must be non-nil")
        frame_id = FrameId(slot)
        native = self._native.bind_provider(str(frame_id), str(physical))
        return Authority._bound(frame_id, physical, native)

    def unbind_provider(self, authority: Authority) -> None:
        self._native.unbind_provider(_authority_handle(authority))

    def ingest_dynamic(self, authority: Authority, record: DynamicRecord) -> None:
        if not isinstance(record, DynamicRecord):
            raise _error("invalid_transform", "record must be a DynamicRecord")
        self._native.ingest_dynamic(_authority_handle(authority), record)

    def apply_static_snapshot(self, authority: Authority, snapshot: StaticSet) -> None:
        if not isinstance(snapshot, StaticSet):
            raise _error("invalid_transform", "snapshot must be a StaticSet")
        self._native.apply_static_snapshot(_authority_handle(authority), snapshot)

    def apply_static_update(self, authority: Authority, update: StaticSet) -> None:
        if not isinstance(update, StaticSet):
            raise _error("invalid_transform", "update must be a StaticSet")
        self._native.apply_static_update(_authority_handle(authority), update)

    def mark_snapshot_stale(self, authority: Authority) -> None:
        self._native.mark_snapshot_stale(_authority_handle(authority))

    def needs_resync(self, authority: Authority) -> bool:
        return self._native.needs_resync(_authority_handle(authority))

    def lookup_at(
        self,
        target: FrameId | str,
        source: FrameId | str,
        stamp: Timestamp,
        *,
        uncertainty_ns: int | None = None,
        mappings: Sequence[ClockMapping] = (),
        now: Timestamp | None = None,
    ) -> LookupResult:
        target_frame = _frame(target, "target")
        source_frame = _frame(source, "source")
        if not isinstance(stamp, Timestamp):
            raise _error("invalid_transform", "stamp must be a Timestamp")
        if not isinstance(mappings, (tuple, list)):
            raise _error("invalid_config", "mappings must be a bounded list or tuple")
        if len(mappings) > _MAX_MAPPINGS:
            raise _error("capacity_exceeded", "at most 256 clock mappings may be supplied")
        if any(not isinstance(mapping, ClockMapping) for mapping in mappings):
            raise _error("invalid_config", "mappings must contain ClockMapping values")
        if now is not None and not isinstance(now, Timestamp):
            raise _error("invalid_transform", "now must be a Timestamp")
        uncertainty = _optional_uncertainty(uncertainty_ns, "uncertainty_ns")
        result = self._native.lookup_at(
            str(target_frame),
            str(source_frame),
            stamp,
            uncertainty,
            tuple(mappings),
            now,
        )
        return _lookup_from_native(result)

    def pressure_snapshot(self) -> dict[str, Any]:
        """Return bounded local buffer and lookup counters."""
        return self._native.pressure_snapshot()


def dynamic_transform_descriptor(
    service_name: str,
    profile: StreamProfile | str,
) -> StreamDescriptor:
    """Build the generated fixed-message descriptor for dynamic transforms."""
    service = _service_name(service_name)
    selected_profile = _stream_profile(profile)
    return StreamDescriptor(
        service,
        MessageTypeDescriptor.from_dict(DYNAMIC_TRANSFORM_DESCRIPTOR),
        selected_profile,
        TransportFeatures.sidecar_v1(),
        StreamResourceConfig(),
    )


def static_update_descriptor(
    service_name: str,
    profile: StreamProfile | str,
) -> StreamDescriptor:
    """Build the generated static full-set update stream descriptor."""
    service = _service_name(service_name)
    selected_profile = _stream_profile(profile)
    return StreamDescriptor(
        service,
        MessageTypeDescriptor.from_dict(STATIC_UPDATE_DESCRIPTOR),
        selected_profile,
        TransportFeatures.sidecar_v1(),
        StreamResourceConfig(),
    )


def static_snapshot_descriptor(service_name: str, maximum_transforms: int) -> RpcDescriptor:
    """Build a bounded full-snapshot RPC descriptor for a static provider."""
    service = _service_name(service_name)
    maximum = _static_transform_bound(maximum_transforms)
    resources = RpcResourceConfig(max_response_slice_len=maximum)
    return RpcDescriptor.fixed_slice(
        service,
        MessageTypeDescriptor.from_dict(STATIC_SNAPSHOT_REQUEST_DESCRIPTOR),
        MessageTypeDescriptor.from_dict(STATIC_SNAPSHOT_DESCRIPTOR),
        resources=resources,
        transport=RpcTransportFeatures.rpc_sidecar_v1(),
    )


class DynamicTfPublisher:
    """Typed native publisher for one physical dynamic-TF stream."""

    __slots__ = ("descriptor", "authority", "_native")

    def __init__(self, native: Any, descriptor: StreamDescriptor, authority: uuid.UUID) -> None:
        self._native = native
        self.descriptor = descriptor
        self.authority = authority

    @classmethod
    def open(
        cls,
        descriptor: StreamDescriptor,
        authority: uuid.UUID | str,
    ) -> DynamicTfPublisher:
        descriptor = _require_stream_descriptor(descriptor, "dynamic transform")
        physical = _capability_uuid(authority, "authority")
        native = _rbt._DynamicTfPublisher.open(descriptor.to_json(), str(physical))
        return cls(native, descriptor, physical)

    @property
    def current_metrics(self) -> dict[str, Any]:
        return self._native.pressure_snapshot()

    def publish(self, record: DynamicRecord) -> int:
        if not isinstance(record, DynamicRecord):
            raise _error("invalid_transform", "record must be a DynamicRecord")
        return self._native.publish(record)

    def close(self) -> None:
        self._native.close()


class DynamicTfSubscriber:
    """Bounded native dynamic-TF subscriber that copies into a local buffer."""

    __slots__ = ("descriptor", "authority", "physical_authority", "buffer", "_native")

    def __init__(
        self,
        native: Any,
        descriptor: StreamDescriptor,
        authority: Authority,
        physical_authority: uuid.UUID,
        buffer: TransformBuffer,
    ) -> None:
        self._native = native
        self.descriptor = descriptor
        self.authority = authority
        self.physical_authority = physical_authority
        self.buffer = buffer

    @classmethod
    def open(
        cls,
        descriptor: StreamDescriptor,
        expected_authority: uuid.UUID | str,
        slot: str,
        buffer: TransformBuffer,
    ) -> DynamicTfSubscriber:
        descriptor = _require_stream_descriptor(descriptor, "dynamic transform")
        physical = _capability_uuid(expected_authority, "expected_authority")
        selected_buffer = _require_buffer(buffer)
        slot_id = _frame(slot, "slot")
        native = _rbt._DynamicTfSubscriber.open(
            descriptor.to_json(),
            str(physical),
            str(slot_id),
            selected_buffer._native,
        )
        authority = _authority_from_native(native.authority)
        return cls(native, descriptor, authority, physical, selected_buffer)

    def drain_bounded(self, budget: int = 64) -> TfDrainReport:
        limit = _drain_limit(budget)
        return TfDrainReport._from_native(self._native.drain_bounded(limit))

    def wait_and_drain(self, timeout: float = 5.0) -> TfDrainReport:
        seconds = _timeout_seconds(timeout, "timeout")
        return TfDrainReport._from_native(self._native.wait_and_drain(seconds))

    @property
    def current_metrics(self) -> dict[str, Any]:
        return self._native.pressure_snapshot()

    @property
    def buffer_metrics(self) -> dict[str, Any]:
        return self._native.buffer_pressure_snapshot()

    def close(self) -> None:
        self._native.close()


class StaticTfProvider:
    """Static full-set update publisher and snapshot RPC provider."""

    __slots__ = (
        "update_descriptor",
        "snapshot_descriptor",
        "update_authority",
        "snapshot_capability",
        "maximum_transforms",
        "_native",
    )

    def __init__(
        self,
        native: Any,
        update_descriptor: StreamDescriptor,
        snapshot_descriptor: RpcDescriptor,
        update_authority: uuid.UUID,
        snapshot_capability: uuid.UUID,
        maximum_transforms: int,
    ) -> None:
        self._native = native
        self.update_descriptor = update_descriptor
        self.snapshot_descriptor = snapshot_descriptor
        self.update_authority = update_authority
        self.snapshot_capability = snapshot_capability
        self.maximum_transforms = maximum_transforms

    @classmethod
    def open(
        cls,
        update_descriptor: StreamDescriptor,
        snapshot_descriptor: RpcDescriptor,
        update_authority: uuid.UUID | str,
        snapshot_capability: uuid.UUID | str,
        initial: StaticSet,
        maximum_transforms: int,
    ) -> StaticTfProvider:
        update_descriptor = _require_stream_descriptor(update_descriptor, "static update")
        snapshot_descriptor = _require_rpc_descriptor(snapshot_descriptor)
        update_id = _capability_uuid(update_authority, "update_authority")
        snapshot_id = _capability_uuid(snapshot_capability, "snapshot_capability")
        initial = _require_static_set(initial)
        maximum = _static_transform_bound(maximum_transforms)
        native = _rbt._StaticTfProvider.open(
            update_descriptor.to_json(),
            snapshot_descriptor.to_json(),
            str(update_id),
            str(snapshot_id),
            initial,
            maximum,
        )
        return cls(
            native,
            update_descriptor,
            snapshot_descriptor,
            update_id,
            snapshot_id,
            maximum,
        )

    @property
    def authority(self) -> uuid.UUID:
        return self.update_authority

    @property
    def current(self) -> StaticSet:
        return _static_set_from_native(self._native.current)

    @property
    def current_metrics(self) -> dict[str, Any]:
        return self._native.pressure_snapshot()

    def replace(self, replacement: StaticSet) -> None:
        self._native.replace(_require_static_set(replacement))

    def serve_one(self, timeout: float = 0.1) -> str | None:
        seconds = _timeout_seconds(timeout, "timeout")
        return self._native.serve_one(seconds)

    def close(self) -> None:
        self._native.close()


class StaticTfClient:
    """Bounded static update subscriber with explicit snapshot refresh."""

    __slots__ = (
        "update_descriptor",
        "snapshot_descriptor",
        "authority",
        "physical_authority",
        "maximum_transforms",
        "buffer",
        "_native",
    )

    def __init__(
        self,
        native: Any,
        update_descriptor: StreamDescriptor,
        snapshot_descriptor: RpcDescriptor,
        authority: Authority,
        physical_authority: uuid.UUID,
        maximum_transforms: int,
        buffer: TransformBuffer,
    ) -> None:
        self._native = native
        self.update_descriptor = update_descriptor
        self.snapshot_descriptor = snapshot_descriptor
        self.authority = authority
        self.physical_authority = physical_authority
        self.maximum_transforms = maximum_transforms
        self.buffer = buffer

    @classmethod
    def open(
        cls,
        expected_authority: uuid.UUID | str,
        update_descriptor: StreamDescriptor,
        snapshot_descriptor: RpcDescriptor,
        snapshot_capability: uuid.UUID | str,
        activation: int,
        slot: str,
        maximum_transforms: int,
        buffer: TransformBuffer,
    ) -> StaticTfClient:
        physical = _capability_uuid(expected_authority, "expected_authority")
        update_descriptor = _require_stream_descriptor(update_descriptor, "static update")
        snapshot_descriptor = _require_rpc_descriptor(snapshot_descriptor)
        snapshot_id = _capability_uuid(snapshot_capability, "snapshot_capability")
        activation_id = _uint(activation, "activation")
        if activation_id == 0:
            raise _error("invalid_config", "activation must be greater than zero")
        slot_id = _frame(slot, "slot")
        maximum = _static_transform_bound(maximum_transforms)
        selected_buffer = _require_buffer(buffer)
        native = _rbt._StaticTfClient.open(
            str(physical),
            update_descriptor.to_json(),
            snapshot_descriptor.to_json(),
            str(snapshot_id),
            activation_id,
            str(slot_id),
            maximum,
            selected_buffer._native,
        )
        authority = _authority_from_native(native.authority)
        return cls(
            native,
            update_descriptor,
            snapshot_descriptor,
            authority,
            physical,
            maximum,
            selected_buffer,
        )

    def drain_updates(self, budget: int = 64) -> TfDrainReport:
        limit = _drain_limit(budget)
        return TfDrainReport._from_native(self._native.drain_updates(limit))

    def refresh_snapshot(self, timeout: float = 1.0) -> None:
        self._native.refresh_snapshot(_timeout_seconds(timeout, "timeout"))

    def refresh_if_due(self, timeout: float = 1.0) -> bool:
        return self._native.refresh_if_due(_timeout_seconds(timeout, "timeout"))

    def wait_and_drain(self, timeout: float = 5.0) -> TfDrainReport:
        return TfDrainReport._from_native(
            self._native.wait_and_drain(_timeout_seconds(timeout, "timeout"))
        )

    def needs_resync(self) -> bool:
        return bool(self.buffer.needs_resync(self.authority))

    @property
    def current_metrics(self) -> dict[str, Any]:
        return self._native.pressure_snapshot()

    def close(self) -> None:
        self._native.close()


def _service_name(value: object) -> str:
    if not isinstance(value, str):
        raise TypeError("service_name must be a string")
    if not value.strip() or "\x00" in value:
        raise ValueError("service_name must be non-empty text without NUL")
    return value


def _stream_profile(value: StreamProfile | str) -> StreamProfile:
    if isinstance(value, StreamProfile):
        return value
    try:
        return StreamProfile(value)
    except (TypeError, ValueError) as error:
        raise ValueError("profile must be a StreamProfile or valid profile string") from error


def _require_stream_descriptor(value: object, name: str) -> StreamDescriptor:
    if not isinstance(value, StreamDescriptor):
        raise _error("invalid_config", f"{name} descriptor must be a StreamDescriptor")
    return value


def _require_rpc_descriptor(value: object) -> RpcDescriptor:
    if not isinstance(value, RpcDescriptor):
        raise _error("invalid_config", "snapshot descriptor must be an RpcDescriptor")
    return value


def _require_buffer(value: object) -> TransformBuffer:
    if not isinstance(value, TransformBuffer):
        raise _error("invalid_config", "buffer must be a TransformBuffer")
    return value


def _require_static_set(value: object) -> StaticSet:
    if not isinstance(value, StaticSet):
        raise _error("invalid_transform", "static set must be a StaticSet")
    if len(value.transforms) > _MAX_STATIC_TRANSFORMS:
        raise _error("capacity_exceeded", "static set exceeds 4096 transforms")
    return value


def _capability_uuid(value: object, name: str) -> uuid.UUID:
    if isinstance(value, uuid.UUID):
        result = value
    elif isinstance(value, str):
        try:
            result = uuid.UUID(value)
        except ValueError as error:
            raise _error("invalid_config", f"{name} must be a UUID") from error
    else:
        raise _error("invalid_config", f"{name} must be a UUID or UUID string")
    if result.int == 0:
        raise _error("invalid_config", f"{name} must be a non-nil UUID")
    return result


def _static_transform_bound(value: object) -> int:
    return _positive_limit(value, "maximum_transforms", _MAX_STATIC_TRANSFORMS)


def _drain_limit(value: object) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise _error("invalid_config", "budget must be an integer")
    if not 0 <= value <= 64:
        raise _error("invalid_config", "budget must be between 0 and 64")
    return value


def _timeout_seconds(value: object, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, numbers.Real):
        raise _error("invalid_config", f"{name} must be a finite non-negative number")
    try:
        seconds = float(value)
    except (OverflowError, TypeError, ValueError) as error:
        raise _error("invalid_config", f"{name} must be finite and non-negative") from error
    if not math.isfinite(seconds) or seconds < 0.0:
        raise _error("invalid_config", f"{name} must be finite and non-negative")
    return seconds


def _authority_from_native(native: Any) -> Authority:
    if native is None:
        raise _error("provider_unavailable", "native buffer authority is missing")
    if isinstance(native, (tuple, list)) and len(native) == 2:
        try:
            slot, physical_id = native
            return Authority(FrameId(slot), uuid.UUID(physical_id))
        except (AttributeError, TypeError, ValueError) as error:
            raise _error("provider_unavailable", "native buffer authority is malformed") from error
    try:
        slot = FrameId(native.slot)
        physical = uuid.UUID(native.physical_id)
    except (AttributeError, TypeError, ValueError) as error:
        raise _error("provider_unavailable", "native buffer authority is malformed") from error
    return Authority._bound(slot, physical, native)


def _timestamp_from_native(value: object) -> Timestamp:
    try:
        kind, source, generation, nanoseconds = value  # type: ignore[misc]
        clock = ClockIdentity(ClockKind(kind), ClockSourceId(source), generation)
        return Timestamp(clock, nanoseconds)
    except (TypeError, ValueError) as error:
        raise _error("invalid_transform", "native timestamp is malformed") from error


def _transform_from_native(value: object) -> Transform3:
    if not isinstance(value, dict):
        raise _error("invalid_transform", "native transform is malformed")
    try:
        translation = Vec3(*value["translation"])
        rotation = Quaternion(*value["rotation"])
    except (KeyError, TypeError, ValueError) as error:
        raise _error("invalid_transform", "native transform is malformed") from error
    return Transform3(translation, rotation)


def _static_set_from_native(value: Any) -> StaticSet:
    try:
        transforms = tuple(
            StaticEdge(item["parent"], item["child"], _transform_from_native(item["transform"]))
            for item in value["transforms"]
        )
        return StaticSet(
            value["revision"],
            _timestamp_from_native(value["effective_from"]),
            value["effective_uncertainty_ns"],
            transforms,
        )
    except (KeyError, TypeError, ValueError) as error:
        raise _error("invalid_transform", "native static set is malformed") from error


def _authority_handle(authority: Authority) -> Any:
    if not isinstance(authority, Authority):
        raise _error("provider_unavailable", "authority must be an Authority returned by bind_provider")
    return authority._handle()


__all__ = [
    "Authority",
    "DynamicTfPublisher",
    "DynamicTfSubscriber",
    "DynamicRecord",
    "FrameId",
    "LookupPolicy",
    "LookupResult",
    "Pose3",
    "Quaternion",
    "StaticEdge",
    "StaticTfClient",
    "StaticTfProvider",
    "StaticSet",
    "TfError",
    "TfDrainReport",
    "TfResourceConfig",
    "TfTransportError",
    "Transform3",
    "TransformBuffer",
    "TransformSource",
    "Vec3",
    "dynamic_transform_descriptor",
    "static_snapshot_descriptor",
    "static_update_descriptor",
]

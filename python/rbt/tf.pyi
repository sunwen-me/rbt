from __future__ import annotations

import uuid
from collections.abc import Sequence
from dataclasses import dataclass
from typing import Any

from . import RpcDescriptor, StreamDescriptor, StreamProfile
from .time import ClockMapping, Timestamp
from .tf_math import FrameId, Pose3, Quaternion, TfError, Transform3, Vec3

@dataclass(frozen=True, slots=True)
class TfResourceConfig:
    max_providers: int = ...
    max_total_edges: int = ...
    max_samples_per_edge: int = ...
    max_total_samples: int = ...
    max_static_transforms_per_provider: int = ...
    max_pending_static_updates: int = ...
    max_history_duration: float = ...
    resync_interval: float = ...
    snapshot_max_age: float = ...
    max_dynamic_age: float = ...

@dataclass(frozen=True, slots=True)
class LookupPolicy:
    max_timestamp_uncertainty_ns: int | None = ...
    max_interpolation_gap: float = ...
    allow_unknown_uncertainty: bool = ...

@dataclass(frozen=True, slots=True)
class Authority:
    slot: FrameId
    physical_id: uuid.UUID
    @property
    def physical(self) -> uuid.UUID: ...

@dataclass(frozen=True, slots=True)
class StaticEdge:
    parent: FrameId
    child: FrameId
    transform: Transform3

@dataclass(frozen=True, slots=True)
class DynamicRecord:
    parent: FrameId
    child: FrameId
    stamp: Timestamp
    uncertainty_ns: int | None
    transform_epoch: int
    transform: Transform3

@dataclass(frozen=True, slots=True)
class StaticSet:
    revision: int
    effective_from: Timestamp
    effective_uncertainty_ns: int | None
    transforms: tuple[StaticEdge, ...]

@dataclass(frozen=True, slots=True)
class TransformSource:
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
    transform: Transform3
    requested_timestamp: Timestamp
    timing_uncertainty_ns: int | None
    sources: tuple[TransformSource, ...]

class TfTransportError(Exception):
    code: str

@dataclass(frozen=True, slots=True)
class TfDrainReport:
    received: int
    accepted: int
    rejected: int
    failures: tuple[TfError, ...]

class DynamicTfPublisher:
    descriptor: StreamDescriptor
    authority: uuid.UUID
    @classmethod
    def open(cls, descriptor: StreamDescriptor, authority: uuid.UUID | str) -> DynamicTfPublisher: ...
    @property
    def current_metrics(self) -> dict[str, Any]: ...
    def publish(self, record: DynamicRecord) -> int: ...
    def close(self) -> None: ...

class DynamicTfSubscriber:
    descriptor: StreamDescriptor
    authority: Authority
    physical_authority: uuid.UUID
    buffer: TransformBuffer
    @classmethod
    def open(
        cls,
        descriptor: StreamDescriptor,
        expected_authority: uuid.UUID | str,
        slot: str,
        buffer: TransformBuffer,
    ) -> DynamicTfSubscriber: ...
    def drain_bounded(self, budget: int = ...) -> TfDrainReport: ...
    def wait_and_drain(self, timeout: float = ...) -> TfDrainReport: ...
    @property
    def current_metrics(self) -> dict[str, Any]: ...
    @property
    def buffer_metrics(self) -> dict[str, Any]: ...
    def close(self) -> None: ...

class StaticTfProvider:
    update_descriptor: StreamDescriptor
    snapshot_descriptor: RpcDescriptor
    update_authority: uuid.UUID
    snapshot_capability: uuid.UUID
    maximum_transforms: int
    @classmethod
    def open(
        cls,
        update_descriptor: StreamDescriptor,
        snapshot_descriptor: RpcDescriptor,
        update_authority: uuid.UUID | str,
        snapshot_capability: uuid.UUID | str,
        initial: StaticSet,
        maximum_transforms: int,
    ) -> StaticTfProvider: ...
    @property
    def authority(self) -> uuid.UUID: ...
    @property
    def current(self) -> StaticSet: ...
    @property
    def current_metrics(self) -> dict[str, Any]: ...
    def replace(self, replacement: StaticSet) -> None: ...
    def serve_one(self, timeout: float = ...) -> str | None: ...
    def close(self) -> None: ...

class StaticTfClient:
    update_descriptor: StreamDescriptor
    snapshot_descriptor: RpcDescriptor
    authority: Authority
    physical_authority: uuid.UUID
    maximum_transforms: int
    buffer: TransformBuffer
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
    ) -> StaticTfClient: ...
    def drain_updates(self, budget: int = ...) -> TfDrainReport: ...
    def refresh_snapshot(self, timeout: float = ...) -> None: ...
    def refresh_if_due(self, timeout: float = ...) -> bool: ...
    def wait_and_drain(self, timeout: float = ...) -> TfDrainReport: ...
    def needs_resync(self) -> bool: ...
    @property
    def current_metrics(self) -> dict[str, Any]: ...
    def close(self) -> None: ...

def dynamic_transform_descriptor(service_name: str, profile: StreamProfile | str) -> StreamDescriptor: ...
def static_update_descriptor(service_name: str, profile: StreamProfile | str) -> StreamDescriptor: ...
def static_snapshot_descriptor(service_name: str, maximum_transforms: int) -> RpcDescriptor: ...

class TransformBuffer:
    def __init__(
        self,
        resources: TfResourceConfig | None = ...,
        policy: LookupPolicy | None = ...,
    ) -> None: ...
    def bind_provider(self, slot: str, physical_id: uuid.UUID | str) -> Authority: ...
    def unbind_provider(self, authority: Authority) -> None: ...
    def ingest_dynamic(self, authority: Authority, record: DynamicRecord) -> None: ...
    def apply_static_snapshot(self, authority: Authority, snapshot: StaticSet) -> None: ...
    def apply_static_update(self, authority: Authority, update: StaticSet) -> None: ...
    def mark_snapshot_stale(self, authority: Authority) -> None: ...
    def needs_resync(self, authority: Authority) -> bool: ...
    def lookup_at(
        self,
        target: FrameId | str,
        source: FrameId | str,
        stamp: Timestamp,
        *,
        uncertainty_ns: int | None = ...,
        mappings: Sequence[ClockMapping] = ...,
        now: Timestamp | None = ...,
    ) -> LookupResult: ...
    def pressure_snapshot(self) -> dict[str, Any]: ...

__all__: list[str]

from . import (
    StreamDescriptor,
    StreamProfile,
    StreamPressure,
    StreamResourceConfig,
)
from .time import SensorTiming

TIMING_PREFIX_LEN: int

def encode_timing_prefix(timing: SensorTiming) -> bytes: ...
def decode_timing_prefix(prefix: bytes | bytearray | memoryview) -> SensorTiming: ...
def split_payload(
    payload: bytes | bytearray | memoryview,
) -> tuple[SensorTiming, memoryview]: ...
def stamped_bytes_descriptor(
    service_name: str,
    *,
    profile: StreamProfile = ...,
    resources: StreamResourceConfig | None = ...,
) -> StreamDescriptor: ...

class StampedBytesLoan:
    @property
    def sequence(self) -> int: ...
    @property
    def payload_nbytes(self) -> int: ...
    def payload_view(self) -> memoryview: ...
    def send(self) -> None: ...
    def _debug_export_count(self) -> int: ...

class StampedBytesPublisher:
    descriptor: StreamDescriptor
    maximum_payload_bytes: int
    @classmethod
    def open(
        cls,
        descriptor: StreamDescriptor,
        *,
        maximum_payload_bytes: int = ...,
        max_loans: int | None = ...,
        max_borrowed: int | None = ...,
    ) -> StampedBytesPublisher: ...
    @property
    def resources(self) -> StreamResourceConfig: ...
    @property
    def pressure(self) -> StreamPressure: ...
    def loan(
        self,
        timing: SensorTiming,
        payload_bytes: int,
        *,
        sequence: int = ...,
    ) -> StampedBytesLoan: ...
    def close(self) -> None: ...

class StampedBytesSample:
    timing: SensorTiming
    @property
    def sequence(self) -> int: ...
    @property
    def payload_nbytes(self) -> int: ...
    def payload_view(self) -> memoryview: ...

class StampedBytesSubscriber:
    descriptor: StreamDescriptor
    @classmethod
    def open(
        cls,
        descriptor: StreamDescriptor,
        *,
        max_borrowed: int | None = ...,
    ) -> StampedBytesSubscriber: ...
    @property
    def resources(self) -> StreamResourceConfig: ...
    @property
    def pressure(self) -> StreamPressure: ...
    def try_receive(self) -> StampedBytesSample | None: ...
    def receive(self, timeout: float | None = ...) -> StampedBytesSample | None: ...
    def close(self) -> None: ...

__all__: list[str]

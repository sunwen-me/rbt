import gc
import time
import uuid

import pytest

from rbt import AbiMismatchError, InvalidPayloadError, LoanStillMappedError, StreamDescriptor
from rbt.time import ClockIdentity, ClockKind, ClockSourceId, SensorTiming, Timestamp
from rbt.time_stream import (
    TIMING_PREFIX_LEN,
    StampedBytesPublisher,
    StampedBytesSubscriber,
    decode_timing_prefix,
    encode_timing_prefix,
    split_payload,
    stamped_bytes_descriptor,
)


def _monotonic(ns: int, *, generation: int = 4) -> Timestamp:
    identity = ClockIdentity(
        ClockKind.HOST_MONOTONIC,
        ClockSourceId.for_host_monotonic(uuid.UUID(int=1), 2, 3, 4, 5),
        generation,
    )
    return Timestamp(identity, ns)


def _service_name() -> str:
    return f"rbt/cap/00000000-0000-4000-8000-{(time.time_ns() & 0xFFFFFFFFFFFF):012x}/stream"


def test_timing_prefix_preserves_distinct_times_and_missing_capture() -> None:
    received = _monotonic(900)
    timing = SensorTiming(_monotonic(700), received, 11, None)
    prefix = encode_timing_prefix(timing)
    assert len(prefix) == TIMING_PREFIX_LEN
    assert decode_timing_prefix(prefix) == timing

    missing = SensorTiming(None, received, None, 0)
    missing_prefix = encode_timing_prefix(missing)
    assert missing_prefix[8] == 0b100
    assert missing_prefix[16:88] == bytes(72)
    assert decode_timing_prefix(missing_prefix) == missing


def test_timing_prefix_preserves_realtime_and_device_clock_kinds() -> None:
    realtime = Timestamp(
        ClockIdentity(
            ClockKind.SYSTEM_REALTIME,
            ClockSourceId.for_system_realtime(uuid.UUID(int=2)),
            8,
        ),
        -1,
    )
    device = Timestamp(
        ClockIdentity(
            ClockKind.DEVICE,
            ClockSourceId.from_device_uuid(uuid.UUID(int=3)),
            9,
        ),
        10,
    )
    timing = SensorTiming(realtime, device)
    prefix = encode_timing_prefix(timing)
    assert prefix[16] == 2
    assert prefix[88] == 3
    assert decode_timing_prefix(prefix) == timing


@pytest.mark.parametrize(
    ("offset", "value"),
    [
        (0, ord("X")),
        (8, 0x80),
        (9, 1),
        (16, 1),
        (89, 1),
        (160, 1),
        (176, 1),
    ],
)
def test_timing_prefix_rejects_invalid_or_reserved_data(offset: int, value: int) -> None:
    prefix = bytearray(encode_timing_prefix(SensorTiming(None, _monotonic(1))))
    prefix[offset] = value
    with pytest.raises(ValueError):
        decode_timing_prefix(prefix)


def test_split_payload_returns_a_borrowed_application_view() -> None:
    timing = SensorTiming(None, _monotonic(7))
    payload = encode_timing_prefix(timing) + b"application"
    decoded, application = split_payload(payload)
    assert decoded == timing
    assert isinstance(application, memoryview)
    assert application == b"application"


def test_image_descriptor_cannot_open_as_stamped_bytes() -> None:
    legacy = StreamDescriptor.image(_service_name())
    with pytest.raises(AbiMismatchError):
        StampedBytesSubscriber.open(legacy)


def test_stamped_bytes_loan_and_received_views_keep_native_ownership() -> None:
    descriptor = stamped_bytes_descriptor(_service_name())
    publisher = StampedBytesPublisher.open(
        descriptor,
        maximum_payload_bytes=64,
    )
    subscriber = StampedBytesSubscriber.open(descriptor)
    try:
        deadline = time.monotonic() + 2
        while publisher.pressure.publisher.subscriber_count == 0:
            assert time.monotonic() < deadline, "subscriber did not connect"
            time.sleep(0.005)

        timing = SensorTiming(_monotonic(100), _monotonic(125), 2, 3)
        loan = publisher.loan(timing, 5, sequence=23)
        writable = loan.payload_view()
        assert memoryview(writable.obj).nbytes == 5
        assert writable.obj._debug_buffer_address() == loan._native._debug_payload_address()
        backing = memoryview(writable.obj)
        backing[:] = b"world"
        assert writable == b"world"
        del backing
        writable[:] = b"hello"
        assert not writable.readonly
        assert loan.sequence == 23
        assert loan.payload_nbytes == 5
        assert loan._debug_export_count() == 1
        with pytest.raises(LoanStillMappedError):
            loan.send()
        derived = writable[1:]
        del writable
        gc.collect()
        assert loan._debug_export_count() == 1
        with pytest.raises(LoanStillMappedError):
            loan.send()
        del derived
        gc.collect()
        assert loan._debug_export_count() == 0
        loan.send()

        sample = subscriber.receive(timeout=2)
        assert sample is not None
        assert sample.sequence == 23
        assert sample.timing == timing
        view = sample.payload_view()
        assert view.readonly
        assert view == b"hello"
        prefix = sample._native._timing_prefix()
        prefix_view = memoryview(prefix)
        assert prefix_view.readonly
        assert prefix_view.nbytes == TIMING_PREFIX_LEN
        assert prefix_view[:8] == b"RBTTIME1"
        assert prefix._debug_payload_address() + TIMING_PREFIX_LEN == sample._native._debug_payload_address()
        derived = view[:]
        del view
        del sample
        gc.collect()
        assert derived.readonly
        assert derived == b"hello"
        assert decode_timing_prefix(prefix_view) == timing
        del derived
        del prefix_view, prefix
        gc.collect()
        assert subscriber.pressure.borrowed_samples == 0
    finally:
        publisher.close()
        subscriber.close()


def test_native_stamped_loan_rejects_invalid_prefixes_and_live_prefix_writes() -> None:
    descriptor = stamped_bytes_descriptor(_service_name())
    publisher = StampedBytesPublisher.open(descriptor, maximum_payload_bytes=8)
    try:
        timing = SensorTiming(None, _monotonic(1))
        loan = publisher.loan(timing, 1)
        writable = loan.payload_view()
        with pytest.raises(LoanStillMappedError):
            loan._native._set_timing_prefix(encode_timing_prefix(timing))
        del writable
        gc.collect()
        loan.send()

        malformed = publisher._native.loan(2, 0)
        with pytest.raises(InvalidPayloadError, match="timing prefix is invalid"):
            malformed.send()

        invalid_prefix = bytearray(encode_timing_prefix(timing))
        invalid_prefix[0] ^= 1
        with pytest.raises(InvalidPayloadError, match="timing prefix is invalid"):
            malformed._set_timing_prefix(bytes(invalid_prefix))
    finally:
        publisher.close()

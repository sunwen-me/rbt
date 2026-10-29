"""Native transform transport integration tests."""

from __future__ import annotations

import threading
import time
import uuid

import pytest

import rbt
from rbt import StreamProfile
from rbt.tf import (
    DynamicRecord,
    DynamicTfPublisher,
    DynamicTfSubscriber,
    Quaternion,
    StaticEdge,
    StaticSet,
    StaticTfClient,
    StaticTfProvider,
    TfTransportError,
    Transform3,
    TransformBuffer,
    Vec3,
    dynamic_transform_descriptor,
    static_snapshot_descriptor,
    static_update_descriptor,
)
from rbt.time import ClockIdentity, ClockKind, ClockSourceId, Timestamp


pytestmark = pytest.mark.skipif(
    not all(
        hasattr(rbt._rbt, name)
        for name in (
            "_DynamicTfPublisher",
            "_DynamicTfSubscriber",
            "_StaticTfProvider",
            "_StaticTfClient",
        )
    ),
    reason="native TF transport extension is not installed",
)


def _clock() -> ClockIdentity:
    return ClockIdentity(
        ClockKind.DEVICE,
        ClockSourceId.from_device_uuid(uuid.UUID(int=42)),
        7,
    )


def _stream(capability: uuid.UUID, descriptor_factory: object) -> object:
    return descriptor_factory(  # type: ignore[operator]
        f"rbt/cap/{capability}/stream",
        StreamProfile.CONTROL,
    )


def _snapshot(capability: uuid.UUID) -> object:
    return static_snapshot_descriptor(f"rbt/cap/{capability}/rpc", 64)


def _static_set(revision: int, stamp: Timestamp) -> StaticSet:
    return StaticSet(
        revision,
        stamp,
        0,
        (StaticEdge("base_link", "camera_link", Transform3(Vec3(1.0, 0.0, 0.0), Quaternion.identity())),),
    )


def test_dynamic_native_publish_drain_lookup_and_release() -> None:
    physical = uuid.uuid4()
    descriptor = _stream(physical, dynamic_transform_descriptor)
    buffer = TransformBuffer()
    publisher = DynamicTfPublisher.open(descriptor, physical)
    subscriber = DynamicTfSubscriber.open(descriptor, physical, "dynamic-camera", buffer)
    stamp = Timestamp(_clock(), 100)
    try:
        assert publisher.publish(
            DynamicRecord(
                "world",
                "camera",
                stamp,
                0,
                3,
                Transform3(Vec3(1.0, 2.0, 3.0), Quaternion.identity()),
            )
        ) > 0
        report = subscriber.wait_and_drain(2.0)
        assert (report.received, report.accepted, report.rejected) == (1, 1, 0)
        result = buffer.lookup_at("world", "camera", stamp, uncertainty_ns=0)
        assert result.transform.translation == Vec3(1.0, 2.0, 3.0)
        assert result.sources[0].authority.physical_id == physical
        assert subscriber.current_metrics["subscriber"]["borrowed_samples"] == 0
        assert publisher.current_metrics["publisher"]["loans_in_use"] == 0
    finally:
        subscriber.close()
        publisher.close()

    with pytest.raises(TfTransportError) as raised:
        publisher.publish(
            DynamicRecord("world", "camera", stamp, 0, 3, Transform3.identity())
        )
    assert raised.value.code == "transport_closed"


def test_transport_rejects_physical_service_identity_mismatch() -> None:
    descriptor_id = uuid.uuid4()
    other_id = uuid.uuid4()
    descriptor = _stream(descriptor_id, dynamic_transform_descriptor)
    with pytest.raises(TfTransportError) as raised:
        DynamicTfPublisher.open(descriptor, other_id)
    assert raised.value.code == "service_identity_mismatch"


def test_static_snapshot_then_update_are_applied_and_guards_are_released() -> None:
    update_id = uuid.uuid4()
    snapshot_id = uuid.uuid4()
    update_descriptor = _stream(update_id, static_update_descriptor)
    snapshot_descriptor = _snapshot(snapshot_id)
    provider = StaticTfProvider.open(
        update_descriptor,
        snapshot_descriptor,
        update_id,
        snapshot_id,
        _static_set(10, Timestamp(_clock(), 0)),
        64,
    )
    client_buffer = TransformBuffer()
    client = StaticTfClient.open(
        update_id,
        update_descriptor,
        snapshot_descriptor,
        snapshot_id,
        1,
        "static-camera",
        64,
        client_buffer,
    )
    refresh_errors: list[BaseException] = []

    def refresh() -> None:
        try:
            client.refresh_snapshot(3.0)
        except BaseException as error:
            refresh_errors.append(error)

    refresher = threading.Thread(target=refresh)
    refresher.start()
    try:
        assert provider.serve_one(3.0) == "success"
        refresher.join(3.0)
        assert not refresher.is_alive()
        assert not refresh_errors
        assert client_buffer.pressure_snapshot()["providers"][0]["static_revision"] == 10
        assert not client.refresh_if_due(0.0)

        provider.replace(_static_set(11, Timestamp(_clock(), 0)))
        report = client.wait_and_drain(3.0)
        assert (report.received, report.accepted, report.rejected) == (1, 1, 0)
        assert client_buffer.pressure_snapshot()["providers"][0]["static_revision"] == 11
        assert client.current_metrics["update"]["subscriber"]["borrowed_samples"] == 0
        rpc = client.current_metrics["snapshot_rpc"]["client"]
        assert rpc["in_flight_current"] == 0
        assert rpc["response_guards_current"] == 0
        assert provider.current.revision == 11
    finally:
        client.close()
        provider.close()


def test_closing_static_client_cancels_a_pending_snapshot_and_releases_endpoints() -> None:
    update_id = uuid.uuid4()
    snapshot_id = uuid.uuid4()
    update_descriptor = _stream(update_id, static_update_descriptor)
    snapshot_descriptor = _snapshot(snapshot_id)
    provider = StaticTfProvider.open(
        update_descriptor,
        snapshot_descriptor,
        update_id,
        snapshot_id,
        _static_set(1, Timestamp(_clock(), 0)),
        64,
    )
    client = StaticTfClient.open(
        update_id,
        update_descriptor,
        snapshot_descriptor,
        snapshot_id,
        2,
        "cancel-camera",
        64,
        TransformBuffer(),
    )
    entered = threading.Event()
    errors: list[BaseException] = []

    def refresh() -> None:
        entered.set()
        try:
            client.refresh_snapshot(60.0)
        except BaseException as error:
            errors.append(error)

    refresher = threading.Thread(target=refresh)
    refresher.start()
    try:
        assert entered.wait(1.0)
        deadline = time.monotonic() + 2.0
        while time.monotonic() < deadline:
            if client._native.snapshot_in_flight:
                break
            time.sleep(0.01)
        assert client._native.snapshot_in_flight
        with pytest.raises(TfTransportError) as raised:
            client.refresh_snapshot(1.0)
        assert raised.value.code == "snapshot_in_flight"

        started = time.monotonic()
        client.close()
        close_duration = time.monotonic() - started
        refresher.join(2.0)
        assert close_duration < 1.0
        assert not refresher.is_alive()
        assert len(errors) == 1
        assert isinstance(errors[0], rbt.RpcActivationExpiredError)
        with pytest.raises(TfTransportError) as raised:
            client.drain_updates()
        assert raised.value.code == "transport_closed"
    finally:
        client.close()
        provider.close()


def test_closing_static_provider_cancels_a_long_receive() -> None:
    update_id = uuid.uuid4()
    snapshot_id = uuid.uuid4()
    provider = StaticTfProvider.open(
        _stream(update_id, static_update_descriptor),
        _snapshot(snapshot_id),
        update_id,
        snapshot_id,
        _static_set(1, Timestamp(_clock(), 0)),
        64,
    )
    entered = threading.Event()
    errors: list[BaseException] = []

    def serve() -> None:
        entered.set()
        try:
            provider.serve_one(60.0)
        except BaseException as error:
            errors.append(error)

    server = threading.Thread(target=serve)
    server.start()
    try:
        assert entered.wait(1.0)
        time.sleep(0.1)
        started = time.monotonic()
        provider.close()
        close_duration = time.monotonic() - started
        server.join(2.0)
        assert close_duration < 1.0
        assert not server.is_alive()
        assert len(errors) == 1
        assert isinstance(errors[0], rbt.RpcActivationExpiredError)
        with pytest.raises(TfTransportError) as raised:
            provider.serve_one(0.0)
        assert raised.value.code == "transport_closed"
    finally:
        provider.close()


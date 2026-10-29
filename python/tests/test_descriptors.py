from __future__ import annotations

import dataclasses
import json

import pytest

import rbt

from conftest import unique_service


def test_descriptor_round_trip_and_generated_hashes() -> None:
    descriptor = rbt.StreamDescriptor.image(unique_service("descriptor"))
    assert rbt.StreamDescriptor.from_json(descriptor.to_json()) == descriptor
    assert descriptor.transport.readiness is rbt.ReadinessProtocol.SIDECAR_V1
    assert rbt.IMAGE_DESCRIPTOR.layout_hash == "943fa4008cf0f55f89a281b051c69a6f"
    assert rbt.POINT_CLOUD_DESCRIPTOR.layout_hash == "d9da2bb29bb2cf217c40f1a56d1fb755"
    assert rbt.POINT_XYZI_DTYPE.itemsize == 16
    assert rbt.POINT_XYZI_DTYPE.fields is not None
    assert rbt.POINT_XYZI_DTYPE.fields["x"][1] == 0
    assert rbt.POINT_XYZI_DTYPE.fields["y"][1] == 4
    assert rbt.POINT_XYZI_DTYPE.fields["z"][1] == 8
    assert rbt.POINT_XYZI_DTYPE.fields["intensity"][1] == 12


def test_resource_defaults_and_custom_values_are_descriptor_metadata_only() -> None:
    descriptor = rbt.StreamDescriptor.image(unique_service("resource-defaults"))
    assert descriptor.resources == rbt.StreamResourceConfig(max_loans=2, max_borrowed=1)
    wire = json.loads(descriptor.to_json())
    assert wire["resources"] == {"max_loans": 2, "max_borrowed": 1}

    custom = dataclasses.replace(
        descriptor,
        resources=rbt.StreamResourceConfig(max_loans=4, max_borrowed=3),
    )
    assert rbt.StreamDescriptor.from_json(custom.to_json()) == custom
    assert custom.message_type.layout_hash == descriptor.message_type.layout_hash
    assert rbt.IMAGE_DESCRIPTOR.layout_hash == "943fa4008cf0f55f89a281b051c69a6f"


def test_zero_resource_values_use_the_typed_invalid_config_error() -> None:
    with pytest.raises(rbt.InvalidResourceConfigError):
        rbt.StreamResourceConfig(max_loans=0)
    with pytest.raises(rbt.InvalidResourceConfigError):
        rbt.StreamResourceConfig(max_borrowed=0)


def test_legacy_descriptor_fails_readiness_preflight_before_transport_open() -> None:
    current = rbt.StreamDescriptor.image(unique_service("legacy"))
    wire = current.to_dict()
    del wire["transport"]
    legacy = rbt.StreamDescriptor.from_json(json.dumps(wire))
    assert legacy.transport.readiness is rbt.ReadinessProtocol.NONE
    with pytest.raises(rbt.MissingTransportFeatureError) as raised:
        rbt.ImageSubscriber.open(legacy)
    assert "sidecar-v1" in str(raised.value)
    assert "provider advertises none" in str(raised.value)


def test_abi_mismatch_fails_before_transport_open() -> None:
    wrong_message = dataclasses.replace(
        rbt.IMAGE_DESCRIPTOR,
        layout_hash="043fa4008cf0f55f89a281b051c69a6f",
    )
    descriptor = rbt.StreamDescriptor(
        unique_service("abi-mismatch"),
        wrong_message,
        rbt.StreamProfile.SENSOR,
        rbt.TransportFeatures.sidecar_v1(),
    )
    with pytest.raises(rbt.AbiMismatchError) as raised:
        rbt.ImageSubscriber.open(descriptor)
    message = str(raised.value)
    assert "rbt.sensor/Image@1" in message
    assert rbt.IMAGE_DESCRIPTOR.layout_hash in message
    assert wrong_message.layout_hash in message
    assert "ABI 1" in message

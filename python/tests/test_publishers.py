from __future__ import annotations

import copy
import dataclasses
import gc
import pickle

import numpy as np
import pytest

import rbt

from conftest import unique_service


def test_image_loan_is_writable_nonowning_and_send_is_mapping_gated() -> None:
    publisher = rbt.ImagePublisher.open(
        rbt.StreamDescriptor.image(unique_service("publisher-image")),
        maximum_payload_bytes=128,
    )
    loan = publisher.loan(
        4,
        3,
        rbt.PixelFormat.RGB8,
        stride=16,
        sequence=7,
        timestamp_ns=11,
    )
    image = loan.numpy()
    assert image.shape == (3, 4, 3)
    assert image.strides == (16, 3, 1)
    assert image.dtype == np.uint8
    assert image.flags.writeable
    assert not image.flags.owndata
    assert image.__array_interface__["data"][0] == loan._debug_payload_address()
    assert loan._debug_export_count() == 1
    image[...] = 0x3C
    assert int(image[2, 3, 2]) == 0x3C

    derived = image.reshape(6, 6).transpose()
    byte_view = memoryview(image)
    owned = image.copy()
    assert owned.flags.owndata
    assert loan._debug_export_count() == 1
    with pytest.raises(rbt.LoanStillMappedError):
        loan.send()

    del image, derived, byte_view
    gc.collect()
    assert loan._debug_export_count() == 0
    loan.send()
    for operation in (loan.send, loan.numpy, loan.payload_view, loan._debug_payload_address):
        with pytest.raises(rbt.LoanConsumedError):
            operation()

    for operation in (copy.copy, copy.deepcopy, pickle.dumps):
        with pytest.raises(TypeError):
            operation(loan)


def test_image_empty_dimensions_and_pointcloud_pointer_identity() -> None:
    image_publisher = rbt.ImagePublisher.open(
        rbt.StreamDescriptor.image(unique_service("publisher-empty-image")),
        maximum_payload_bytes=64,
    )
    empty = image_publisher.loan(0, 4, rbt.PixelFormat.GRAY8)
    empty_array = empty.numpy()
    assert empty_array.shape == (4, 0)
    assert empty_array.strides == (0, 1)
    assert empty_array.flags.writeable
    del empty_array
    gc.collect()
    empty.send()

    point_publisher = rbt.PointCloudPublisher.open(
        rbt.StreamDescriptor.point_cloud(unique_service("publisher-pointcloud")),
        maximum_points=100_000,
    )
    for count in (0, 1, 100, 100_000):
        loan = point_publisher.loan(count, sequence=count, dense=False)
        points = loan.numpy()
        assert points.shape == (count,)
        assert points.dtype == rbt.POINT_XYZI_DTYPE
        assert points.flags.writeable
        assert not points.flags.owndata
        assert points.__array_interface__["data"][0] == loan._debug_payload_address()
        if count:
            points["x"] = np.arange(count, dtype=np.float32)
        del points
        gc.collect()
        loan.send()


def test_unsent_loan_is_reclaimed_after_last_view_and_pool_exhaustion_is_typed() -> None:
    publisher = rbt.ImagePublisher.open(
        rbt.StreamDescriptor.image(unique_service("publisher-pool")),
        maximum_payload_bytes=64,
    )
    first = publisher.loan(1, 1, rbt.PixelFormat.RGB8)
    view = first.numpy()
    del first
    gc.collect()
    second = publisher.loan(1, 1, rbt.PixelFormat.RGB8)
    with pytest.raises(rbt.LoanPoolExhaustedError):
        publisher.loan(1, 1, rbt.PixelFormat.RGB8)
    del view
    gc.collect()
    third = publisher.loan(1, 1, rbt.PixelFormat.RGB8)
    del second, third


def test_custom_resource_config_reports_loan_exhaustion_and_recovery() -> None:
    descriptor = dataclasses.replace(
        rbt.StreamDescriptor.image(unique_service("publisher-pressure")),
        resources=rbt.StreamResourceConfig(max_loans=3, max_borrowed=2),
    )
    publisher = rbt.ImagePublisher.open(descriptor, maximum_payload_bytes=64)
    assert publisher.resources == descriptor.resources

    loans = [publisher.loan(1, 1, rbt.PixelFormat.RGB8) for _ in range(3)]
    with pytest.raises(rbt.LoanPoolExhaustedError):
        publisher.loan(1, 1, rbt.PixelFormat.RGB8)
    exhausted = publisher.pressure
    assert exhausted.publisher is not None
    assert exhausted.resources == descriptor.resources
    assert exhausted.publisher.loans_in_use == 3
    assert exhausted.publisher.loan_failures >= 1

    del loans
    gc.collect()
    recovered = publisher.pressure
    assert recovered.publisher is not None
    assert recovered.publisher.loans_in_use == 0
    assert recovered.publisher.loan_successes >= 3


def test_publisher_layout_and_readiness_validation() -> None:
    publisher = rbt.ImagePublisher.open(
        rbt.StreamDescriptor.image(unique_service("publisher-validation")),
        maximum_payload_bytes=64,
    )
    with pytest.raises(rbt.UnsupportedPixelFormatError):
        publisher.loan(1, 1, 999)
    with pytest.raises(rbt.InvalidImageLayoutError):
        publisher.loan(4, 2, rbt.PixelFormat.RGB8, stride=11)
    with pytest.raises(rbt.InvalidPayloadError):
        publisher.loan(4, 2, rbt.PixelFormat.RGB8, stride=40)
    with pytest.raises(rbt.InvalidImageLayoutError):
        publisher.loan(-1, 1, rbt.PixelFormat.GRAY8)

    descriptor = rbt.StreamDescriptor.image(unique_service("publisher-legacy"))
    legacy = dataclasses.replace(
        descriptor, transport=rbt.TransportFeatures(rbt.ReadinessProtocol.NONE)
    )
    with pytest.raises(rbt.MissingTransportFeatureError):
        rbt.ImagePublisher.open(legacy)

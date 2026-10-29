from __future__ import annotations

import gc
import queue
import threading
import time

import numpy as np
import pytest

import rbt

from conftest import background_process, fixture_binary, unique_service


def _fixture(service: str, width: int, height: int, stride: int, frames: int, period_ms: int):
    return background_process(
        fixture_binary(), service, width, height, stride, frames, period_ms
    )


def _assert_read_only(array: np.ndarray) -> None:
    assert not array.flags.owndata
    assert not array.flags.writeable
    with pytest.raises(ValueError):
        array.flags.writeable = True
    if array.size:
        with pytest.raises(ValueError):
            array.flat[0] = 123


def test_rust_image_1080p_zero_copy_ten_frames() -> None:
    service = unique_service("image-1080p")
    with _fixture(service, 1920, 1080, 5760, 10, 100):
        subscriber = rbt.ImageSubscriber.open(rbt.StreamDescriptor.image(service))
        for _ in range(10):
            sample = subscriber.receive(timeout=10)
            assert sample is not None
            image = sample.numpy()
            assert image.shape == (1080, 1920, 3)
            assert image.strides == (5760, 3, 1)
            assert image.dtype == np.uint8
            assert sample.payload_nbytes == 6_220_800
            assert image.__array_interface__["data"][0] == sample._debug_payload_address()
            _assert_read_only(image)
            assert int(image.sum(dtype=np.uint64)) == 793_152_000
            assert int(image[0, 0, 0]) == sample.sequence % 256
            assert int(image[-1, -1, -1]) == (
                sample.sequence + sample.payload_nbytes - 1
            ) % 256
            if sample.sequence == 0:
                print(
                    "IMAGE_EVIDENCE",
                    f"shape={image.shape}",
                    f"strides={image.strides}",
                    f"bytes={sample.payload_nbytes}",
                    "checksum=793152000",
                    f"native_ptr=0x{sample._debug_payload_address():x}",
                    f"numpy_ptr=0x{image.__array_interface__['data'][0]:x}",
                    f"owndata={image.flags.owndata}",
                    f"writeable={image.flags.writeable}",
                )
            del image, sample
            gc.collect()


def test_padded_image_uses_header_stride_without_copy() -> None:
    service = unique_service("image-padded")
    with _fixture(service, 640, 480, 2048, 1, 0):
        subscriber = rbt.ImageSubscriber.open(rbt.StreamDescriptor.image(service))
        sample = subscriber.receive(timeout=10)
        assert sample is not None
        image = sample.numpy()
        assert image.shape == (480, 640, 3)
        assert image.strides == (2048, 3, 1)
        assert not image.flags.c_contiguous
        assert image.__array_interface__["data"][0] == sample._debug_payload_address()
        _assert_read_only(image)
        checksum = int(image.sum(dtype=np.uint64))
        assert checksum == 113_571_840
        for row, column, channel in ((0, 0, 0), (1, 0, 0), (479, 639, 2)):
            payload_offset = row * 2048 + column * 3 + channel
            assert int(image[row, column, channel]) == payload_offset % 256
        print(
            "PADDED_IMAGE_EVIDENCE",
            f"shape={image.shape}",
            f"strides={image.strides}",
            f"checksum={checksum}",
            f"c_contiguous={image.flags.c_contiguous}",
            f"pointer_match={image.__array_interface__['data'][0] == sample._debug_payload_address()}",
        )


def test_derived_view_retains_native_borrow_then_release_recovers() -> None:
    service = unique_service("derived-owner")
    with _fixture(service, 640, 480, 1920, 3, 100):
        subscriber = rbt.ImageSubscriber.open(rbt.StreamDescriptor.image(service))
        sample = subscriber.receive(timeout=10)
        assert sample is not None
        image = sample.numpy()
        crop = image[100:200, 100:200]
        expected = int(crop[0, 0, 0])
        del image, sample
        gc.collect()
        assert crop.shape == (100, 100, 3)
        assert int(crop[0, 0, 0]) == expected
        time.sleep(0.25)
        with pytest.raises(rbt.BorrowLimitReachedError, match="max borrowed=1"):
            subscriber.try_receive()
        assert subscriber.pressure.borrow_failures >= 1
        del crop
        gc.collect()
        recovered = subscriber.receive(timeout=10)
        assert recovered is not None
        assert recovered.sequence >= 1
        print(
            "LIFETIME_EVIDENCE",
            "sample_deleted=true derived_view_retained=true",
            "borrow_limit_reached=true recovery=true",
        )


def test_memoryview_retains_native_borrow() -> None:
    service = unique_service("memoryview-owner")
    with _fixture(service, 64, 4, 192, 2, 100):
        subscriber = rbt.ImageSubscriber.open(rbt.StreamDescriptor.image(service))
        sample = subscriber.receive(timeout=10)
        assert sample is not None
        view = sample.payload_view()
        assert view.readonly
        assert view[0] == 0
        del sample
        gc.collect()
        time.sleep(0.15)
        with pytest.raises(rbt.BorrowLimitReachedError):
            subscriber.try_receive()
        del view
        gc.collect()
        assert subscriber.receive(timeout=10) is not None


def test_multiple_arrays_each_retain_the_native_sample_owner() -> None:
    service = unique_service("multiple-views")
    with _fixture(service, 64, 4, 192, 3, 150):
        subscriber = rbt.ImageSubscriber.open(rbt.StreamDescriptor.image(service))
        sample = subscriber.receive(timeout=10)
        assert sample is not None
        first = sample.numpy()
        second = sample.numpy()
        native_pointer = sample._debug_payload_address()
        assert first.__array_interface__["data"][0] == native_pointer
        assert second.__array_interface__["data"][0] == native_pointer

        base_chain: list[object] = [second]
        while getattr(base_chain[-1], "base", None) is not None:
            base_chain.append(base_chain[-1].base)
        assert any(type(owner).__module__ == "rbt._rbt" for owner in base_chain)

        expected = int(second.sum(dtype=np.uint64))
        del sample, first
        gc.collect()
        assert int(second.sum(dtype=np.uint64)) == expected
        time.sleep(0.2)
        with pytest.raises(rbt.BorrowLimitReachedError):
            subscriber.try_receive()

        del second, base_chain
        gc.collect()
        recovered = subscriber.receive(timeout=10)
        assert recovered is not None


def test_last_array_reference_can_be_dropped_on_another_python_thread() -> None:
    service = unique_service("cross-thread-owner")
    with _fixture(service, 64, 4, 192, 2, 150):
        subscriber = rbt.ImageSubscriber.open(rbt.StreamDescriptor.image(service))
        sample = subscriber.receive(timeout=10)
        assert sample is not None
        image = sample.numpy()
        handoff: queue.Queue[np.ndarray] = queue.Queue()
        completed = threading.Event()

        def consume_and_drop() -> None:
            transferred = handoff.get(timeout=5)
            assert int(transferred.sum(dtype=np.uint64)) == 97_920
            del transferred
            gc.collect()
            completed.set()

        thread = threading.Thread(target=consume_and_drop)
        thread.start()
        handoff.put(image)
        del image, sample
        gc.collect()
        thread.join(timeout=5)
        assert completed.is_set()
        recovered = subscriber.receive(timeout=10)
        assert recovered is not None

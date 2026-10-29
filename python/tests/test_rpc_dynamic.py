"""Dynamic Image RPC ownership, shape, and asyncio integration gates."""

from __future__ import annotations

import asyncio
import gc
import threading
import uuid
from collections.abc import Callable
from typing import Any

import numpy as np
import pytest

import rbt
from rbt._generated.demo import FRAME_DESCRIPTOR


pytestmark = pytest.mark.skipif(
    not hasattr(rbt.RpcDescriptor, "fixed_slice"),
    reason="dynamic RPC Python surface is not installed",
)


def _resources(request_max: int, response_max: int) -> rbt.RpcResourceConfig:
    return rbt.RpcResourceConfig(
        max_in_flight=4,
        max_clients=4,
        max_request_loans=2,
        response_buffer_size=2,
        max_borrowed_responses=1,
        max_response_loans=1,
        max_request_slice_len=max(1, request_max),
        max_response_slice_len=max(1, response_max),
    )


def _service(label: str) -> str:
    return f"rbt-python-dynamic-{label}-{uuid.uuid4().hex}"


def _fixed_slice(count: int) -> rbt.RpcDescriptor:
    return rbt.RpcDescriptor.fixed_slice(
        _service("fixed-slice"),
        FRAME_DESCRIPTOR,
        rbt.IMAGE_DESCRIPTOR,
        resources=_resources(1, count),
    )


def _slice_fixed(count: int) -> rbt.RpcDescriptor:
    return rbt.RpcDescriptor.slice_fixed(
        _service("slice-fixed"),
        rbt.IMAGE_DESCRIPTOR,
        FRAME_DESCRIPTOR,
        resources=_resources(count, 1),
    )


def _slice_slice(request_count: int, response_count: int) -> rbt.RpcDescriptor:
    return rbt.RpcDescriptor.slice(
        _service("slice-slice"),
        rbt.IMAGE_DESCRIPTOR,
        rbt.IMAGE_DESCRIPTOR,
        resources=_resources(request_count, response_count),
    )


def _join(thread: threading.Thread, failures: list[BaseException]) -> None:
    thread.join(10)
    assert not thread.is_alive(), "RPC server thread did not finish"
    if failures:
        raise failures[0]


def _server_thread(function: Callable[[], None]) -> tuple[threading.Thread, list[BaseException]]:
    failures: list[BaseException] = []

    def run() -> None:
        try:
            function()
        except BaseException as error:  # surfaced in the test thread below
            failures.append(error)

    thread = threading.Thread(target=run, name="rbt-python-dynamic-server")
    thread.start()
    return thread, failures


@pytest.mark.parametrize("count", [0, 1, 100, 100_000])
def test_slice_request_fixed_response_all_required_lengths(count: int) -> None:
    descriptor = _slice_fixed(count)
    server = rbt.RpcServer.open(descriptor)
    client = rbt.RpcClient.open(descriptor)
    seen: list[tuple[int, int, int]] = []

    def serve() -> None:
        request = server.receive(5.0)
        assert request is not None
        image = request.as_image()
        array = image.numpy()
        assert array.flags.owndata is False
        assert array.flags.writeable is False
        assert array.__array_interface__["data"][0] == request._debug_payload_address()
        assert image.sequence == 41
        assert image.payload_nbytes == count
        checksum = int(array.sum(dtype=np.uint64))
        seen.append((image.width, image.height, checksum))
        del array, image
        request.loan_response().send()

    thread, failures = _server_thread(serve)
    try:
        loan = client.loan_request(count)
        loan.set_image_header(
            sequence=41,
            timestamp_ns=99,
            width=count,
            height=0 if count == 0 else 1,
            stride=count,
            encoding=rbt.PixelFormat.GRAY8,
        )
        writable = loan.as_image().numpy()
        assert writable.flags.owndata is False
        assert writable.flags.writeable is True
        assert writable.__array_interface__["data"][0] == loan._debug_payload_address()
        if count:
            writable.reshape(-1)[:] = np.arange(count, dtype=np.uint64) % 251
        expected = int(writable.sum(dtype=np.uint64))
        del writable
        response = loan.send(5.0)
        assert response.payload_nbytes == 16
        _join(thread, failures)
        assert seen == [(count, 0 if count == 0 else 1, expected)]
    finally:
        client.close()
        server.close()


def test_fixed_request_dynamic_response_header_and_retained_views() -> None:
    count = 100_000
    descriptor = _fixed_slice(count)
    server = rbt.RpcServer.open(descriptor)
    client = rbt.RpcClient.open(descriptor)

    def serve() -> None:
        request = server.receive(5.0)
        assert request is not None
        response = request.loan_response(count)
        response.set_image_header(
            sequence=77,
            timestamp_ns=123,
            width=count,
            height=1,
            stride=count,
            encoding=rbt.PixelFormat.GRAY8,
        )
        array = response.as_image().numpy()
        array.reshape(-1)[:] = np.arange(count, dtype=np.uint64) % 239
        del array
        response.send()

    thread, failures = _server_thread(serve)
    try:
        response = client.call(timeout=5.0)
        image = response.as_image()
        array = image.numpy()
        assert image.sequence == 77
        assert image.timestamp_ns == 123
        assert array.flags.owndata is False
        assert array.flags.writeable is False
        assert array.__array_interface__["data"][0] == response._debug_payload_address()
        derived = array.reshape(-1)[5::7]
        expected = (np.arange(count, dtype=np.uint64) % 239)[5::7]
        del array, image, response
        gc.collect()
        metrics = client.pressure.client
        assert metrics is not None
        assert metrics.response_guards_current == 1
        assert metrics.response_guards_high_water == 1
        np.testing.assert_array_equal(derived, expected)

        holder: list[Any] = [derived]
        del derived

        def release() -> None:
            holder.clear()
            gc.collect()

        releaser = threading.Thread(target=release)
        releaser.start()
        releaser.join(5)
        assert not releaser.is_alive()
        metrics = client.pressure.client
        assert metrics is not None
        assert metrics.response_guards_current == 0
        _join(thread, failures)
    finally:
        client.close()
        server.close()


def test_writable_request_alias_and_cross_thread_final_release_gate_send() -> None:
    descriptor = _slice_fixed(64)
    server = rbt.RpcServer.open(descriptor)
    client = rbt.RpcClient.open(descriptor)

    def serve() -> None:
        request = server.receive(5.0)
        assert request is not None
        request.loan_response().send()

    thread, failures = _server_thread(serve)
    try:
        loan = client.loan_request(64)
        loan.set_image_header(
            width=64,
            height=1,
            stride=64,
            encoding=rbt.PixelFormat.GRAY8,
        )
        root = loan.as_image().numpy()
        derived = root[:, 1:]
        del root
        with pytest.raises(rbt.LoanStillMappedError):
            loan.send(0.1)

        holder: list[Any] = [derived]
        del derived

        def release() -> None:
            holder.clear()
            gc.collect()

        releaser = threading.Thread(target=release)
        releaser.start()
        releaser.join(5)
        assert not releaser.is_alive()
        assert loan._debug_export_count() == 0
        response = loan.send(5.0)
        assert response.payload_nbytes == 16
        _join(thread, failures)
    finally:
        client.close()
        server.close()


def test_dynamic_count_validation_and_response_retry_after_bad_count() -> None:
    fixed = rbt.RpcDescriptor.fixed(
        _service("fixed"), FRAME_DESCRIPTOR, FRAME_DESCRIPTOR
    )
    fixed_server = rbt.RpcServer.open(fixed)
    fixed_client = rbt.RpcClient.open(fixed)
    try:
        with pytest.raises(ValueError, match="dynamic RPC request"):
            fixed_client.loan_request(1)
    finally:
        fixed_client.close()
        fixed_server.close()

    descriptor = _slice_slice(4, 4)
    server = rbt.RpcServer.open(descriptor)
    client = rbt.RpcClient.open(descriptor)
    try:
        with pytest.raises(ValueError, match="required"):
            client.loan_request()
        with pytest.raises(rbt.RpcCapacityError):
            client.loan_request(5)
        loan = client.loan_request(1)
        loan.set_image_header(
            width=1,
            height=1,
            stride=1,
            encoding=rbt.PixelFormat.GRAY8,
        )

        def serve() -> None:
            request = server.receive(5.0)
            assert request is not None
            with pytest.raises(ValueError, match="required"):
                request.loan_response()
            response = request.loan_response(1)
            response.set_image_header(
                width=1,
                height=1,
                stride=1,
                encoding=rbt.PixelFormat.GRAY8,
            )
            response.send()

        thread, failures = _server_thread(serve)
        response = loan.send(5.0)
        assert response.as_image().payload_nbytes == 1
        _join(thread, failures)
    finally:
        client.close()
        server.close()


def test_one_mib_request_twenty_mib_response_exact_content() -> None:
    request_count = 1 << 20
    response_count = 20 << 20
    descriptor = _slice_slice(request_count, response_count)
    server = rbt.RpcServer.open(descriptor)
    client = rbt.RpcClient.open(descriptor)
    request_checksum: list[int] = []

    def serve() -> None:
        request = server.receive(10.0)
        assert request is not None
        request_array = request.as_image().numpy().reshape(-1)
        request_checksum.append(int(request_array.sum(dtype=np.uint64)))
        del request_array
        response = request.loan_response(response_count)
        response.set_image_header(
            sequence=9001,
            width=response_count,
            height=1,
            stride=response_count,
            encoding=rbt.PixelFormat.GRAY8,
        )
        response_array = response.as_image().numpy().reshape(-1)
        response_array[:] = 0xA5
        del response_array
        response.send()

    thread, failures = _server_thread(serve)
    try:
        loan = client.loan_request(request_count)
        loan.set_image_header(
            sequence=9000,
            width=request_count,
            height=1,
            stride=request_count,
            encoding=rbt.PixelFormat.GRAY8,
        )
        request_array = loan.as_image().numpy().reshape(-1)
        request_array[:] = 0x5A
        expected_request_checksum = request_count * 0x5A
        del request_array
        response = loan.send(10.0)
        response_array = response.as_image().numpy().reshape(-1)
        assert response.as_image().sequence == 9001
        assert response_array.size == response_count
        assert int(response_array.sum(dtype=np.uint64)) == response_count * 0xA5
        del response_array
        _join(thread, failures)
        assert request_checksum == [expected_request_checksum]
    finally:
        client.close()
        server.close()


def _run_async(function: Callable[..., Any]) -> Callable[..., Any]:
    def wrapper(*args: Any, **kwargs: Any) -> Any:
        return asyncio.run(function(*args, **kwargs))

    return wrapper


@_run_async
async def test_async_slice_to_slice_returns_live_zero_copy_response() -> None:
    count = 4096
    descriptor = _slice_slice(count, count)
    server = rbt.RpcServer.open(descriptor)
    client = rbt.RpcClient.open(descriptor)

    async def serve() -> None:
        request = await server.receive_async(5.0)
        assert request is not None
        array = request.as_image().numpy().reshape(-1)
        assert int(array.sum(dtype=np.uint64)) == count * 7
        del array
        response = request.loan_response(count)
        response.set_image_header(
            sequence=2,
            width=count,
            height=1,
            stride=count,
            encoding=rbt.PixelFormat.GRAY8,
        )
        output = response.as_image().numpy().reshape(-1)
        output[:] = 11
        del output
        response.send()

    serving = asyncio.create_task(serve())
    try:
        def fill(loan: rbt.RpcRequestLoan) -> None:
            loan.set_image_header(
                sequence=1,
                width=count,
                height=1,
                stride=count,
                encoding=rbt.PixelFormat.GRAY8,
            )
            array = loan.as_image().numpy().reshape(-1)
            array[:] = 7

        response = await client.call_async(
            fill, timeout=5.0, element_count=count
        )
        await serving
        image = response.as_image()
        view = image.numpy()
        assert image.sequence == 2
        assert view.__array_interface__["data"][0] == response._debug_payload_address()
        assert int(view.sum(dtype=np.uint64)) == count * 11
        assert client._debug_async_registration_count() == 0
        assert server._debug_async_registration_count() == 0
    finally:
        client.close()
        server.close()

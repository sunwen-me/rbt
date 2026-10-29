"""Native synchronous RPC integration checks.

These tests intentionally exercise the extension rather than the light-weight
Python wrappers in ``test_rpc_api.py``.  They are skipped when a source tree is
tested against an older installed extension that predates synchronous RPC.
"""

from __future__ import annotations

import threading
import time
import uuid

import pytest

import rbt
from rbt._generated.demo import DETECTION_DESCRIPTOR, FRAME_DESCRIPTOR


pytestmark = pytest.mark.skipif(
    not hasattr(rbt._rbt, "_RpcClient") or not hasattr(rbt._rbt, "_RpcServer"),
    reason="native synchronous RPC extension is not installed",
)


def _descriptor(*, resources: rbt.RpcResourceConfig | None = None) -> rbt.RpcDescriptor:
    return rbt.RpcDescriptor.fixed(
        f"rbt-python-test-{uuid.uuid4().hex}",
        FRAME_DESCRIPTOR,
        DETECTION_DESCRIPTOR,
        resources=resources,
    )


def _raise_thread_error(errors: list[BaseException]) -> None:
    if errors:
        raise errors[0]


def test_native_rpc_roundtrip_is_zero_copy_and_alias_safe() -> None:
    descriptor = _descriptor()
    server = rbt.RpcServer.open(descriptor)
    client = rbt.RpcClient.open(descriptor)
    errors: list[BaseException] = []

    def serve() -> None:
        try:
            request = server.receive(3.0)
            assert request is not None
            request_view = request.payload_view()
            assert request_view.readonly
            assert request_view.nbytes == request.payload_nbytes
            assert request._debug_payload_address() == request_view.obj._debug_payload_address()

            # Rejecting a consume must preserve the exact outstanding borrow
            # count; a second rejection while the same view is alive must not
            # consume the request.
            for _ in range(2):
                with pytest.raises(rbt.LoanStillMappedError):
                    request.loan_response()
            del request_view

            response_loan = request.loan_response()
            response_view = response_loan.payload_view()
            assert not response_view.readonly
            assert response_loan._debug_payload_address() == response_view.obj._debug_buffer_address()
            response_view[:4] = b"pong"
            with pytest.raises(rbt.LoanStillMappedError):
                response_loan.send()
            del response_view
            response_loan.send()
        except BaseException as error:  # propagate assertion failures to caller
            errors.append(error)

    server_thread = threading.Thread(target=serve)
    server_thread.start()
    try:
        request_loan = client.loan_request()
        request_view = request_loan.payload_view()
        assert not request_view.readonly
        assert request_loan._debug_payload_address() == request_view.obj._debug_buffer_address()
        request_view[:4] = b"ping"
        with pytest.raises(rbt.LoanStillMappedError):
            request_loan.send(0.05)
        del request_view

        response = request_loan.send(3.0)
        response_view = response.payload_view()
        assert response_view.readonly
        assert response_view.nbytes == response.payload_nbytes
        assert response._debug_payload_address() == response_view.obj._debug_payload_address()
        assert bytes(response_view[:4]) == b"pong"
        with pytest.raises(TypeError):
            response_view[0] = 0
        del response_view
        server_thread.join(3.0)
        assert not server_thread.is_alive()
        _raise_thread_error(errors)
    finally:
        client.close()
        server.close()
        server_thread.join(3.0)


def test_native_rpc_final_lease_drop_may_run_on_another_thread() -> None:
    descriptor = _descriptor()
    server = rbt.RpcServer.open(descriptor)
    client = rbt.RpcClient.open(descriptor)
    errors: list[BaseException] = []

    def serve() -> None:
        try:
            request = server.receive(3.0)
            assert request is not None
            response = request.respond()
            response.send()
        except BaseException as error:
            errors.append(error)

    server_thread = threading.Thread(target=serve)
    server_thread.start()
    try:
        request_loan = client.loan_request()
        view = request_loan.payload_view()
        released = threading.Event()
        ready = threading.Event()

        def drop_on_worker(worker_view: memoryview) -> None:
            ready.set()
            released.wait(2.0)
            del worker_view

        worker = threading.Thread(target=drop_on_worker, args=(view,))
        worker.start()
        assert ready.wait(2.0)
        del view
        assert request_loan._debug_export_count() == 1
        released.set()
        worker.join(3.0)
        assert not worker.is_alive()
        assert request_loan._debug_export_count() == 0

        response = request_loan.send(3.0)
        response_view = response.payload_view()
        del response_view
        server_thread.join(3.0)
        assert not server_thread.is_alive()
        _raise_thread_error(errors)
    finally:
        client.close()
        server.close()
        server_thread.join(3.0)


def test_native_rpc_wait_releases_the_gil() -> None:
    descriptor = _descriptor()
    server = rbt.RpcServer.open(descriptor)
    client = rbt.RpcClient.open(descriptor)
    errors: list[BaseException] = []
    received = threading.Event()
    result: list[object] = []

    def serve() -> None:
        try:
            request = server.receive(3.0)
            assert request is not None
            received.set()
            time.sleep(0.4)
            request.respond().send()
        except BaseException as error:
            errors.append(error)

    def call() -> None:
        try:
            result.append(client.call(timeout=3.0))
        except BaseException as error:
            errors.append(error)

    server_thread = threading.Thread(target=serve)
    call_thread = threading.Thread(target=call)
    server_thread.start()
    call_thread.start()
    counter = 0
    try:
        assert received.wait(2.0)
        deadline = time.monotonic() + 0.2
        while time.monotonic() < deadline:
            counter += 1
        call_thread.join(3.0)
        server_thread.join(3.0)
        assert not call_thread.is_alive()
        assert not server_thread.is_alive()
        assert counter > 1_000
        assert result
        _raise_thread_error(errors)
    finally:
        client.close()
        server.close()
        call_thread.join(3.0)
        server_thread.join(3.0)


def test_native_rpc_typed_timeout_capacity_and_activation_errors() -> None:
    descriptor = _descriptor()
    server = rbt.RpcServer.open(descriptor)
    client = rbt.RpcClient.open(descriptor)
    try:
        with pytest.raises(rbt.RpcDeadlineExceededError):
            client.call(timeout=0.05)

        limited = _descriptor(resources=rbt.RpcResourceConfig(max_request_loans=1))
        limited_server = rbt.RpcServer.open(limited)
        limited_client = rbt.RpcClient.open(limited)
        try:
            loan = limited_client.loan_request()
            with pytest.raises(rbt.RpcCapacityError):
                limited_client.loan_request()
            del loan
        finally:
            limited_client.close()
            limited_server.close()

        client.close()
        with pytest.raises(rbt.RpcActivationExpiredError):
            client.loan_request()
    finally:
        client.close()
        server.close()


def test_native_rpc_client_expiry_wakes_pending_call() -> None:
    descriptor = _descriptor()
    server = rbt.RpcServer.open(descriptor)
    client = rbt.RpcClient.open(descriptor)
    received = threading.Event()
    release = threading.Event()
    result: list[BaseException] = []

    def serve() -> None:
        try:
            request = server.receive(3.0)
            assert request is not None
            received.set()
            release.wait(2.0)
        except BaseException as error:
            result.append(error)

    def call() -> None:
        try:
            client.call(timeout=10.0)
        except BaseException as error:
            result.append(error)

    server_thread = threading.Thread(target=serve)
    call_thread = threading.Thread(target=call)
    server_thread.start()
    call_thread.start()
    try:
        assert received.wait(2.0)
        client.close()
        release.set()
        call_thread.join(3.0)
        server_thread.join(3.0)
        assert not call_thread.is_alive()
        assert not server_thread.is_alive()
        assert any(isinstance(error, rbt.RpcActivationExpiredError) for error in result)
    finally:
        release.set()
        client.close()
        server.close()
        call_thread.join(3.0)
        server_thread.join(3.0)


def test_native_rpc_server_receive_wakes_on_activation_expiry() -> None:
    descriptor = _descriptor()
    server = rbt.RpcServer.open(descriptor)
    result: list[BaseException] = []
    started = threading.Event()

    def receive() -> None:
        started.set()
        try:
            server.receive(10.0)
        except BaseException as error:
            result.append(error)

    thread = threading.Thread(target=receive)
    thread.start()
    try:
        assert started.wait(2.0)
        time.sleep(0.05)
        server.close()
        thread.join(3.0)
        assert not thread.is_alive()
        assert result
        assert isinstance(result[0], rbt.RpcActivationExpiredError)
    finally:
        server.close()
        thread.join(3.0)


def test_canonical_generated_rpc_descriptor_roundtrips() -> None:
    descriptor = rbt.RpcDescriptor.fixed(
        "rbt-python-generated-descriptor",
        FRAME_DESCRIPTOR,
        DETECTION_DESCRIPTOR,
    )
    assert descriptor.request_type.qualified_name == "rbt.demo/Frame@1"
    assert descriptor.response_type.qualified_name == "rbt.demo/Detection@1"
    roundtrip = rbt.RpcDescriptor.from_json(descriptor.to_json())
    assert roundtrip.request_type.layout_hash == FRAME_DESCRIPTOR["layout_hash"]
    assert roundtrip.response_type.layout_hash == DETECTION_DESCRIPTOR["layout_hash"]


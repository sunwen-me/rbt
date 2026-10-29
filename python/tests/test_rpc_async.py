"""Fixed-message asyncio RPC integration checks."""

from __future__ import annotations

import asyncio
import os
import threading
import uuid
from typing import Any

import pytest

import rbt
from rbt._generated.demo import DETECTION_DESCRIPTOR, FRAME_DESCRIPTOR


pytestmark = pytest.mark.skipif(
    not hasattr(rbt._rbt, "_RpcAsyncCall")
    or not hasattr(rbt._rbt, "_RpcAsyncReceive"),
    reason="native asyncio RPC extension is not installed",
)


def _run_async(function: Any) -> Any:
    def wrapper(*args: Any, **kwargs: Any) -> Any:
        return asyncio.run(function(*args, **kwargs))

    return wrapper


def _descriptor() -> rbt.RpcDescriptor:
    return rbt.RpcDescriptor.fixed(
        f"rbt-python-async-{uuid.uuid4().hex}",
        FRAME_DESCRIPTOR,
        DETECTION_DESCRIPTOR,
    )


def _native_thread_count() -> int:
    """Count OS tasks, including Rust threads invisible to threading.active_count()."""
    try:
        return len(os.listdir("/proc/self/task"))
    except OSError:
        return threading.active_count()


@_run_async
async def test_async_client_server_roundtrip() -> None:
    descriptor = _descriptor()
    server = rbt.RpcServer.open(descriptor)
    client = rbt.RpcClient.open(descriptor)

    async def serve() -> None:
        request = await server.receive_async(3.0)
        assert request is not None
        request.respond().send()

    serving = asyncio.create_task(serve())
    try:
        response = await client.call_async(timeout=3.0)
        assert response.payload_nbytes > 0
        await serving
        assert client._debug_async_registration_count() == 0
        assert server._debug_async_registration_count() == 0
    finally:
        client.close()
        server.close()


@_run_async
async def test_server_receive_ownership_rejects_async_and_sync_competitors() -> None:
    server = rbt.RpcServer.open(_descriptor())
    first = asyncio.create_task(server.receive_async(timeout=3.0))
    try:
        for _ in range(10):
            await asyncio.sleep(0)
            if server._debug_async_registration_count() == 1:
                break
        assert server._debug_async_registration_count() == 1
        with pytest.raises(rbt.ConcurrentReceiveError):
            await server.receive_async(timeout=0.01)
        with pytest.raises(rbt.ConcurrentReceiveError):
            await asyncio.to_thread(server.try_receive)
    finally:
        first.cancel()
        await asyncio.gather(first, return_exceptions=True)
        assert server._debug_async_registration_count() == 0
        server.close()


@_run_async
async def test_task_cancel_late_response_and_next_call() -> None:
    descriptor = _descriptor()
    server = rbt.RpcServer.open(descriptor)
    client = rbt.RpcClient.open(descriptor)
    first_received = asyncio.Event()

    async def serve() -> None:
        request = await server.receive_async(3.0)
        assert request is not None
        first_received.set()
        await asyncio.sleep(0.05)
        # The cancelled client's late response is deliberately harmless.
        try:
            request.respond().send()
        except rbt.RpcError:
            pass
        request = await server.receive_async(3.0)
        assert request is not None
        request.respond().send()

    serving = asyncio.create_task(serve())
    try:
        cancelled = asyncio.create_task(client.call_async(timeout=3.0))
        await first_received.wait()
        cancelled.cancel()
        with pytest.raises(asyncio.CancelledError):
            await cancelled
        assert client._debug_async_registration_count() == 0
        response = await client.call_async(timeout=3.0)
        assert response.payload_nbytes > 0
        await serving
    finally:
        client.close()
        server.close()


@_run_async
async def test_wait_for_and_native_deadline_leave_client_reusable() -> None:
    descriptor = _descriptor()
    server = rbt.RpcServer.open(descriptor)
    client = rbt.RpcClient.open(descriptor)
    try:
        with pytest.raises(rbt.RpcDeadlineExceededError):
            await client.call_async(timeout=0.02)
        assert client._debug_async_registration_count() == 0
        with pytest.raises(asyncio.TimeoutError):
            await asyncio.wait_for(client.call_async(timeout=3.0), 0.02)
        assert client._debug_async_registration_count() == 0

        async def serve() -> None:
            request = await server.receive_async(3.0)
            assert request is not None
            request.respond().send()

        serving = asyncio.create_task(serve())
        response = await client.call_async(timeout=3.0)
        assert response.payload_nbytes > 0
        await serving
    finally:
        client.close()
        server.close()


@_run_async
async def test_activation_expiry_wakes_async_client_and_server() -> None:
    descriptor = _descriptor()
    server = rbt.RpcServer.open(descriptor)
    client = rbt.RpcClient.open(descriptor)
    client_task: asyncio.Task[Any] | None = None
    server_task = asyncio.create_task(server.receive_async(timeout=3.0))
    await asyncio.sleep(0.02)
    try:
        with pytest.raises(rbt.ConcurrentReceiveError):
            server.try_receive()
        server.close()
        with pytest.raises(rbt.RpcActivationExpiredError):
            await server_task
        client_task = asyncio.create_task(client.call_async(timeout=3.0))
        await asyncio.sleep(0.02)
        client.close()
        with pytest.raises(rbt.RpcActivationExpiredError):
            await client_task
        assert client._debug_async_registration_count() == 0
        assert server._debug_async_registration_count() == 0
    finally:
        client.close()
        server.close()
        for task in (client_task, server_task):
            if task is None:
                continue
            if not task.done():
                task.cancel()
        await asyncio.gather(
            *(task for task in (client_task, server_task) if task is not None),
            return_exceptions=True,
        )


@_run_async
async def test_repeated_cancellation_has_bounded_native_threads_and_registrations() -> None:
    descriptor = _descriptor()
    server = rbt.RpcServer.open(descriptor)
    client = rbt.RpcClient.open(descriptor)
    before = _native_thread_count()
    try:
        concurrent = [
            asyncio.create_task(client.call_async(timeout=3.0)) for _ in range(4)
        ]
        for _ in range(20):
            await asyncio.sleep(0)
            if client._debug_async_registration_count() == 4:
                break
        assert client._debug_async_registration_count() == 4
        during = _native_thread_count()
        # One client bridge services all calls; it must not create one native
        # thread per pending request.
        assert during <= before + 2, (before, during)
        for task in concurrent:
            task.cancel()
        await asyncio.gather(*concurrent, return_exceptions=True)
        assert client._debug_async_registration_count() == 0
        for _ in range(100):
            task = asyncio.create_task(client.call_async(timeout=3.0))
            await asyncio.sleep(0)
            task.cancel()
            with pytest.raises(asyncio.CancelledError):
                await task
            assert client._debug_async_registration_count() == 0
    finally:
        client.close()
        server.close()
        await asyncio.sleep(0.02)
        assert _native_thread_count() <= before

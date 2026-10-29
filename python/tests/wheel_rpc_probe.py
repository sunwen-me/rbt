#!/usr/bin/env python3
"""Exercise RPC behavior through an installed wheel.

This probe is intentionally independent of the source package.  It is invoked
with the wheel-smoke interpreter and refuses an ``rbt`` import from the
repository, so the checks cover the artifact that was actually installed in
the fresh environment.
"""

from __future__ import annotations

import argparse
import asyncio
import struct
import sys
import uuid
from pathlib import Path
from typing import Any

import rbt
from rbt._generated.demo import DETECTION_DESCRIPTOR, FRAME_DESCRIPTOR


CALL_TIMEOUT = 5.0
PAYLOAD = struct.Struct("<QQ")
NONCE_BASE = 0xBADC_0000_0000_0000
MARKER_BASE = 0xC0DE_0000
OPERAND_BASE = 0x0100
OPERAND_STEP = 17
TRANSFORM_MULTIPLIER = 0x9E37_79B1_85EB_CA87
TRANSFORM_XOR = 0xD1CE_F00D_CAFE_BEEF
MASK = (1 << 64) - 1
IMAGE_REQUEST_BYTES = 256
IMAGE_RESPONSE_BYTES = 512


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", type=Path, required=True)
    parser.add_argument("--rust-bin", type=Path, required=True)
    parser.add_argument("--run-id", required=True)
    return parser.parse_args()


def service(run_id: str, label: str) -> str:
    return f"rbt/python/wheel-rpc/{run_id}/{label}/{uuid.uuid4().hex}"


def fixed_resources() -> rbt.RpcResourceConfig:
    return rbt.RpcResourceConfig(
        max_in_flight=2,
        max_clients=1,
        max_request_loans=2,
        response_buffer_size=2,
        max_borrowed_responses=2,
        max_response_loans=2,
        max_request_slice_len=1,
        max_response_slice_len=1,
    )


def interop_resources() -> rbt.RpcResourceConfig:
    # Keep this identical to crates/rbt-rpc-interop's native descriptor.
    return rbt.RpcResourceConfig(
        max_in_flight=1,
        max_clients=1,
        max_request_loans=1,
        response_buffer_size=1,
        max_borrowed_responses=1,
        max_response_loans=1,
        max_request_slice_len=1,
        max_response_slice_len=1,
    )


def fixed_descriptor(name: str, *, interop: bool = False) -> rbt.RpcDescriptor:
    return rbt.RpcDescriptor.fixed(
        name,
        FRAME_DESCRIPTOR,
        DETECTION_DESCRIPTOR,
        resources=interop_resources() if interop else fixed_resources(),
    )


def image_descriptor(name: str) -> rbt.RpcDescriptor:
    return rbt.RpcDescriptor.slice(
        name,
        rbt.IMAGE_DESCRIPTOR,
        rbt.IMAGE_DESCRIPTOR,
        resources=rbt.RpcResourceConfig(
            max_in_flight=1,
            max_clients=1,
            max_request_loans=1,
            response_buffer_size=1,
            max_borrowed_responses=1,
            max_response_loans=1,
            max_request_slice_len=IMAGE_REQUEST_BYTES,
            max_response_slice_len=IMAGE_RESPONSE_BYTES,
        ),
    )


def request_for(index: int) -> tuple[int, int]:
    index_value = index & MASK
    marker = MARKER_BASE | index_value
    operand = OPERAND_BASE + index * OPERAND_STEP
    return NONCE_BASE | index_value, ((marker << 32) | operand) & MASK


def expected_value(nonce: int, value: int) -> int:
    rotated = ((value << 23) | (value >> (64 - 23))) & MASK
    return ((nonce * TRANSFORM_MULTIPLIER) & MASK) ^ rotated ^ TRANSFORM_XOR


def fill_fixed(loan: Any, index: int = 0) -> None:
    view = loan.payload_view()
    try:
        if view.nbytes != PAYLOAD.size:
            raise RuntimeError(f"fixed request has {view.nbytes} bytes")
        PAYLOAD.pack_into(view, 0, *request_for(index))
    finally:
        del view


def fill_fixed_response(response: Any, index: int = 0) -> None:
    view = response.payload_view()
    try:
        nonce, value = request_for(index)
        PAYLOAD.pack_into(view, 0, nonce, expected_value(nonce, value))
    finally:
        del view


def check_fixed_response(response: Any, index: int = 0) -> None:
    view = response.payload_view()
    try:
        if view.nbytes != PAYLOAD.size:
            raise RuntimeError(f"fixed response has {view.nbytes} bytes")
        actual_nonce, actual_value = PAYLOAD.unpack_from(view)
    finally:
        del view
    nonce, value = request_for(index)
    expected = expected_value(nonce, value)
    if (actual_nonce, actual_value) != (nonce, expected):
        raise RuntimeError(
            f"fixed response mismatch expected={(nonce, expected)} "
            f"actual={(actual_nonce, actual_value)}"
        )


def image_request_byte(offset: int) -> int:
    return (offset * 31 + 7) & 0xFF


def image_response_byte(offset: int) -> int:
    return (offset * 13 + 0xA5) & 0xFF


def image_bytes(view: memoryview) -> bytes:
    return bytes(view.cast("B"))


def assert_wheel_provenance(repo_root: Path) -> None:
    repo_root = repo_root.resolve()
    package_path = Path(rbt.__file__).resolve()
    native_path = Path(rbt._rbt.__file__).resolve()
    if repo_root == package_path or repo_root in package_path.parents:
        raise RuntimeError(f"rbt package came from source tree: {package_path}")
    if repo_root == native_path or repo_root in native_path.parents:
        raise RuntimeError(f"rbt native extension came from source tree: {native_path}")
    if not hasattr(rbt.RpcClient, "call_async") or not hasattr(
        rbt.RpcServer, "receive_async"
    ):
        raise RuntimeError("installed wheel does not expose asyncio RPC")
    if not hasattr(rbt.RpcDescriptor, "slice"):
        raise RuntimeError("installed wheel does not expose dynamic RPC")
    print(
        f"RBT_WHEEL_PROVENANCE package={package_path} native={native_path}",
        flush=True,
    )


async def fixed_async_success(run_id: str) -> None:
    server = rbt.RpcServer.open(fixed_descriptor(service(run_id, "async-success")))
    client = rbt.RpcClient.open(server.descriptor)

    async def serve() -> None:
        request = await server.receive_async(CALL_TIMEOUT)
        if request is None:
            raise RuntimeError("async fixed server receive timed out")
        response = request.loan_response()
        fill_fixed_response(response)
        response.send()

    serving = asyncio.create_task(serve())
    try:
        response = await client.call_async(fill_fixed, timeout=CALL_TIMEOUT)
        check_fixed_response(response)
        await serving
        if client._debug_async_registration_count() != 0:
            raise RuntimeError("async fixed client registration leaked")
        if server._debug_async_registration_count() != 0:
            raise RuntimeError("async fixed server registration leaked")
    finally:
        if not serving.done():
            serving.cancel()
        await asyncio.gather(serving, return_exceptions=True)
        client.close()
        server.close()
    print("RBT_WHEEL_ASYNC_SUCCESS fixed=1", flush=True)


async def native_deadline(run_id: str) -> None:
    server = rbt.RpcServer.open(fixed_descriptor(service(run_id, "deadline")))
    client = rbt.RpcClient.open(server.descriptor)
    try:
        try:
            await client.call_async(fill_fixed, timeout=0.03)
        except rbt.RpcDeadlineExceededError:
            pass
        else:
            raise RuntimeError("native deadline did not win without a response")
        if client._debug_async_registration_count() != 0:
            raise RuntimeError("deadline registration leaked")
    finally:
        client.close()
        server.close()
    print("RBT_WHEEL_ASYNC_DEADLINE fixed=1", flush=True)


async def cancel_and_reuse(run_id: str) -> None:
    server = rbt.RpcServer.open(fixed_descriptor(service(run_id, "cancel-reuse")))
    client = rbt.RpcClient.open(server.descriptor)
    first_received = asyncio.Event()

    async def serve() -> None:
        first = await server.receive_async(CALL_TIMEOUT)
        if first is None:
            raise RuntimeError("cancel probe first receive timed out")
        first_received.set()
        await asyncio.sleep(0.05)
        try:
            response = first.loan_response()
            fill_fixed_response(response)
            response.send()
        except rbt.RpcError:
            # The cancelled call owns the terminal state; a late response is
            # deliberately harmless.
            pass
        second = await server.receive_async(CALL_TIMEOUT)
        if second is None:
            raise RuntimeError("cancel probe reuse receive timed out")
        response = second.loan_response()
        fill_fixed_response(response)
        response.send()

    serving = asyncio.create_task(serve())
    try:
        cancelled = asyncio.create_task(client.call_async(fill_fixed, timeout=CALL_TIMEOUT))
        await asyncio.wait_for(first_received.wait(), CALL_TIMEOUT)
        cancelled.cancel()
        try:
            await cancelled
        except asyncio.CancelledError:
            pass
        else:
            raise RuntimeError("Task.cancel did not produce CancelledError")
        if client._debug_async_registration_count() != 0:
            raise RuntimeError("cancelled call registration leaked")
        response = await client.call_async(fill_fixed, timeout=CALL_TIMEOUT)
        check_fixed_response(response)
        await serving
        if server._debug_async_registration_count() != 0:
            raise RuntimeError("cancel server registration leaked")
    finally:
        if not serving.done():
            serving.cancel()
        await asyncio.gather(serving, return_exceptions=True)
        client.close()
        server.close()
    print("RBT_WHEEL_ASYNC_CANCEL_REUSE cancelled=1 late_response=1", flush=True)


async def dynamic_image_roundtrip(run_id: str) -> None:
    server = rbt.RpcServer.open(image_descriptor(service(run_id, "dynamic-image")))
    client = rbt.RpcClient.open(server.descriptor)
    sequence = 41
    timestamp = 0x1020_3040_5060_7000

    async def serve() -> None:
        request = await server.receive_async(CALL_TIMEOUT)
        if request is None:
            raise RuntimeError("dynamic server receive timed out")
        image = request.as_image()
        if (
            image.sequence != sequence
            or image.width != IMAGE_REQUEST_BYTES
            or image.height != 1
            or image.stride != IMAGE_REQUEST_BYTES
            or image.encoding != rbt.PixelFormat.GRAY8
            or image.payload_nbytes != IMAGE_REQUEST_BYTES
        ):
            raise RuntimeError("dynamic request header mismatch")
        request_view = image.payload_view()
        try:
            if image_bytes(request_view) != bytes(
                image_request_byte(offset) for offset in range(IMAGE_REQUEST_BYTES)
            ):
                raise RuntimeError("dynamic request payload mismatch")
        finally:
            del request_view
        del image
        response = request.loan_response(IMAGE_RESPONSE_BYTES)
        response.set_image_header(
            sequence=sequence + 1,
            timestamp_ns=timestamp ^ TRANSFORM_XOR,
            width=IMAGE_RESPONSE_BYTES,
            height=1,
            stride=IMAGE_RESPONSE_BYTES,
            encoding=rbt.PixelFormat.GRAY8,
        )
        response_view = response.as_image().payload_view()
        try:
            response_view.cast("B")[:] = bytes(
                image_response_byte(offset) for offset in range(IMAGE_RESPONSE_BYTES)
            )
        finally:
            del response_view
        response.send()

    serving = asyncio.create_task(serve())
    try:
        def fill(loan: Any) -> None:
            loan.set_image_header(
                sequence=sequence,
                timestamp_ns=timestamp,
                width=IMAGE_REQUEST_BYTES,
                height=1,
                stride=IMAGE_REQUEST_BYTES,
                encoding=rbt.PixelFormat.GRAY8,
            )
            view = loan.as_image().payload_view()
            try:
                view.cast("B")[:] = bytes(
                    image_request_byte(offset) for offset in range(IMAGE_REQUEST_BYTES)
                )
            finally:
                del view

        response = await client.call_async(
            fill, timeout=CALL_TIMEOUT, element_count=IMAGE_REQUEST_BYTES
        )
        image = response.as_image()
        if (
            image.sequence != sequence + 1
            or image.timestamp_ns != timestamp ^ TRANSFORM_XOR
            or image.width != IMAGE_RESPONSE_BYTES
            or image.height != 1
            or image.stride != IMAGE_RESPONSE_BYTES
            or image.encoding != rbt.PixelFormat.GRAY8
            or image.payload_nbytes != IMAGE_RESPONSE_BYTES
        ):
            raise RuntimeError("dynamic response header mismatch")
        response_view = image.payload_view()
        try:
            if image_bytes(response_view) != bytes(
                image_response_byte(offset) for offset in range(IMAGE_RESPONSE_BYTES)
            ):
                raise RuntimeError("dynamic response payload mismatch")
        finally:
            del response_view
        del image, response
        await serving
        if client._debug_async_registration_count() != 0:
            raise RuntimeError("dynamic client registration leaked")
        if server._debug_async_registration_count() != 0:
            raise RuntimeError("dynamic server registration leaked")
    finally:
        if not serving.done():
            serving.cancel()
        await asyncio.gather(serving, return_exceptions=True)
        client.close()
        server.close()
    print(
        f"RBT_WHEEL_DYNAMIC_IMAGE request_bytes={IMAGE_REQUEST_BYTES} "
        f"response_bytes={IMAGE_RESPONSE_BYTES}",
        flush=True,
    )


async def start_rust(rust_bin: Path, role: str, name: str) -> asyncio.subprocess.Process:
    process = await asyncio.create_subprocess_exec(
        str(rust_bin),
        role,
        name,
        "1",
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    assert process.stdout is not None
    try:
        ready = await asyncio.wait_for(process.stdout.readline(), CALL_TIMEOUT)
    except BaseException:
        if process.returncode is None:
            process.terminate()
        await process.wait()
        raise
    if b"RBT_RPC_INTEROP READY" not in ready:
        stderr = b""
        if process.stderr is not None:
            stderr = await process.stderr.read()
        await process.wait()
        raise RuntimeError(
            f"Rust {role} did not become ready: {ready!r}; stderr={stderr.decode(errors='replace')}"
        )
    return process


async def finish_rust(process: asyncio.subprocess.Process, role: str) -> None:
    stdout, stderr = await asyncio.wait_for(process.communicate(), CALL_TIMEOUT)
    if process.returncode != 0 or b"RBT_RPC_INTEROP PASS" not in stdout:
        raise RuntimeError(
            f"Rust {role} failed rc={process.returncode}: "
            f"stdout={stdout.decode(errors='replace')} "
            f"stderr={stderr.decode(errors='replace')}"
        )
    print(stdout.decode(errors="replace"), end="", flush=True)


async def stop_rust(process: asyncio.subprocess.Process | None) -> None:
    if process is None or process.returncode is not None:
        return
    process.terminate()
    try:
        await asyncio.wait_for(process.wait(), 2.0)
    except asyncio.TimeoutError:
        process.kill()
        await process.wait()


async def rust_server_python_client(run_id: str, rust_bin: Path) -> None:
    name = service(run_id, "rust-server-python-client")
    process: asyncio.subprocess.Process | None = None
    client: rbt.RpcClient | None = None
    try:
        process = await start_rust(rust_bin, "server", name)
        client = rbt.RpcClient.open(fixed_descriptor(name, interop=True))
        response = await client.call_async(fill_fixed, timeout=CALL_TIMEOUT)
        check_fixed_response(response)
        await finish_rust(process, "server")
        process = None
    finally:
        if client is not None:
            client.close()
        await stop_rust(process)
    print("RBT_WHEEL_RUST_PYTHON_INTEROP rust_server=1 python_client=1", flush=True)


async def python_server_rust_client(run_id: str, rust_bin: Path) -> None:
    name = service(run_id, "python-server-rust-client")
    server = rbt.RpcServer.open(fixed_descriptor(name, interop=True))
    process: asyncio.subprocess.Process | None = None
    receive: asyncio.Task[rbt.RpcRequest | None] | None = None
    try:
        receive = asyncio.create_task(server.receive_async(CALL_TIMEOUT))
        process = await start_rust(rust_bin, "client", name)
        request = await receive
        receive = None
        if request is None:
            raise RuntimeError("Python interop server receive timed out")
        view = request.payload_view()
        try:
            if PAYLOAD.unpack_from(view) != request_for(0):
                raise RuntimeError("Python interop request mismatch")
        finally:
            del view
        response = request.loan_response()
        fill_fixed_response(response)
        response.send()
        await finish_rust(process, "client")
        process = None
    finally:
        if receive is not None and not receive.done():
            receive.cancel()
        if receive is not None:
            await asyncio.gather(receive, return_exceptions=True)
        await stop_rust(process)
        server.close()
    print("RBT_WHEEL_RUST_PYTHON_INTEROP rust_client=1 python_server=1", flush=True)


async def run(args: argparse.Namespace) -> None:
    assert_wheel_provenance(args.repo_root)
    if not args.rust_bin.is_file() or not args.rust_bin.stat().st_mode & 0o111:
        raise RuntimeError(f"prebuilt Rust interop binary is missing: {args.rust_bin}")
    await fixed_async_success(args.run_id)
    await native_deadline(args.run_id)
    await cancel_and_reuse(args.run_id)
    await dynamic_image_roundtrip(args.run_id)
    await rust_server_python_client(args.run_id, args.rust_bin)
    await python_server_rust_client(args.run_id, args.rust_bin)
    print("RBT_WHEEL_RPC_PROBE_OK", flush=True)


def main() -> int:
    args = parse_args()
    try:
        asyncio.run(run(args))
    except BaseException as error:
        print(f"RBT_WHEEL_RPC_PROBE_FAIL error={error}", file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

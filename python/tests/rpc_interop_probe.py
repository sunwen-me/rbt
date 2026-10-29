#!/usr/bin/env python3
"""Fixed and dynamic synchronous/asyncio RPC interop probe.

The probe writes and reads the canonical Frame/Detection fields directly on
the native Python buffer views.  It is intentionally a standalone process so
the shell harness can pair it with Rust or C++ endpoints; async lanes can add
their own probe entry points without changing this synchronous contract.
"""

from __future__ import annotations

import argparse
import asyncio
import os
import struct
import sys
from typing import Any

import rbt
from rbt._generated.demo import DETECTION_DESCRIPTOR, FRAME_DESCRIPTOR


DEFAULT_CALLS = 4
MAX_CALLS = 1_000_000
CALL_TIMEOUT = 3.0
NONCE_BASE = 0xBADC_0000_0000_0000
MARKER_BASE = 0xC0DE_0000
OPERAND_BASE = 0x0100
OPERAND_STEP = 17
TRANSFORM_MULTIPLIER = 0x9E37_79B1_85EB_CA87
TRANSFORM_XOR = 0xD1CE_F00D_CAFE_BEEF
MASK = (1 << 64) - 1
PAYLOAD = struct.Struct("<QQ")
DYNAMIC_REQUEST_BYTES = 4_096
DYNAMIC_RESPONSE_BYTES = 65_536
DYNAMIC_TIMESTAMP_BASE = 0x1020_3040_5060_7000


def report_index(index: int) -> bool:
    return os.environ.get("RBT_RPC_INTEROP_QUIET") != "1" or index % 100 == 0


def report_idle(
    endpoint: Any,
    role: str,
    service: str,
    calls: int,
    *,
    client_side: bool,
) -> None:
    pressure = endpoint.pressure
    if pressure.service_name != service:
        raise RuntimeError(
            f"idle snapshot service mismatch expected={service} actual={pressure.service_name}"
        )
    if pressure.snapshot_timestamp_unix_ms <= 0:
        raise RuntimeError("idle snapshot timestamp must be positive")

    if client_side:
        if pressure.client is None or pressure.server is not None:
            raise RuntimeError("idle snapshot is missing client metrics or has server metrics")
        metrics = pressure.client
        print(
            f"RBT_RPC_INTEROP IDLE language=python role={role} service={pressure.service_name} "
            f"calls={calls} endpoint=client "
            f"snapshot_timestamp_unix_ms={pressure.snapshot_timestamp_unix_ms} "
            f"calls_started={metrics.calls_started} calls_completed={metrics.calls_completed} "
            f"in_flight_current={metrics.in_flight_current} "
            f"response_guards_current={metrics.response_guards_current}",
            flush=True,
        )
        if (
            metrics.calls_started != calls
            or metrics.calls_completed != calls
            or metrics.in_flight_current != 0
            or metrics.response_guards_current != 0
        ):
            raise RuntimeError(
                f"idle client counters mismatch expected_calls={calls} "
                f"started={metrics.calls_started} completed={metrics.calls_completed} "
                f"in_flight_current={metrics.in_flight_current} "
                f"response_guards_current={metrics.response_guards_current}"
            )
    else:
        if pressure.server is None or pressure.client is not None:
            raise RuntimeError("idle snapshot is missing server metrics or has client metrics")
        metrics = pressure.server
        print(
            f"RBT_RPC_INTEROP IDLE language=python role={role} service={pressure.service_name} "
            f"calls={calls} endpoint=server "
            f"snapshot_timestamp_unix_ms={pressure.snapshot_timestamp_unix_ms} "
            f"requests_received={metrics.requests_received} "
            f"responses_sent={metrics.responses_sent} "
            f"requests_current={metrics.requests_current}",
            flush=True,
        )
        if (
            metrics.requests_received != calls
            or metrics.responses_sent != calls
            or metrics.requests_current != 0
        ):
            raise RuntimeError(
                f"idle server counters mismatch expected_calls={calls} "
                f"requests_received={metrics.requests_received} "
                f"responses_sent={metrics.responses_sent} "
                f"requests_current={metrics.requests_current}"
            )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "role",
        choices=(
            "client",
            "server",
            "async-client",
            "async-server",
            "async-dynamic-client",
        ),
    )
    parser.add_argument("service")
    parser.add_argument("calls", nargs="?", type=int, default=DEFAULT_CALLS)
    args = parser.parse_args()
    if not args.service:
        parser.error("service name must not be empty")
    if not 1 <= args.calls <= MAX_CALLS:
        parser.error(f"calls must be between 1 and {MAX_CALLS}")
    return args


def resources() -> rbt.RpcResourceConfig:
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


def descriptor(service: str) -> rbt.RpcDescriptor:
    return rbt.RpcDescriptor.fixed(
        service,
        FRAME_DESCRIPTOR,
        DETECTION_DESCRIPTOR,
        resources=resources(),
    )


def dynamic_resources() -> rbt.RpcResourceConfig:
    return rbt.RpcResourceConfig(
        max_in_flight=1,
        max_clients=1,
        max_request_loans=1,
        response_buffer_size=1,
        max_borrowed_responses=1,
        max_response_loans=1,
        max_request_slice_len=DYNAMIC_REQUEST_BYTES,
        max_response_slice_len=DYNAMIC_RESPONSE_BYTES,
    )


def dynamic_descriptor(service: str) -> rbt.RpcDescriptor:
    return rbt.RpcDescriptor.slice(
        service,
        rbt.IMAGE_DESCRIPTOR,
        rbt.IMAGE_DESCRIPTOR,
        resources=dynamic_resources(),
    )


def dynamic_request_byte(index: int, offset: int) -> int:
    return (index * 17 + offset * 31 + 7) & 0xFF


def dynamic_response_byte(index: int, offset: int) -> int:
    return (index * 29 + offset * 13 + 0xA5) & 0xFF


def dynamic_checksum(view: memoryview) -> int:
    checksum = 14_695_981_039_346_656_037
    for byte in view.cast("B"):
        checksum = ((checksum ^ byte) * 1_099_511_628_211) & MASK
    return checksum


def request_for(index: int) -> tuple[int, int]:
    marker = MARKER_BASE | index
    operand = OPERAND_BASE + index * OPERAND_STEP
    return NONCE_BASE | index, ((marker << 32) | operand) & MASK


def rotate_left(value: int, shift: int) -> int:
    return ((value << shift) | (value >> (64 - shift))) & MASK


def expected_value(nonce: int, value: int) -> int:
    return ((nonce * TRANSFORM_MULTIPLIER) & MASK) ^ rotate_left(value, 23) ^ TRANSFORM_XOR


def unpack_request(index: int, view: memoryview) -> tuple[int, int]:
    if view.nbytes != PAYLOAD.size:
        raise RuntimeError(f"request index={index} has {view.nbytes} bytes")
    nonce, value = PAYLOAD.unpack_from(view)
    expected = request_for(index)
    if (nonce, value) != expected:
        raise RuntimeError(
            f"request mismatch index={index} expected_nonce={expected[0]} "
            f"actual_nonce={nonce} expected_value={expected[1]} actual_value={value}"
        )
    return nonce, value


def open_server(service: str) -> rbt.RpcServer:
    return rbt.RpcServer.open(descriptor(service))


def open_client(service: str) -> rbt.RpcClient:
    return rbt.RpcClient.open(descriptor(service))


def run_server(service: str, calls: int) -> None:
    server = open_server(service)
    try:
        print(
            f"RBT_RPC_INTEROP READY language=python role=server service={service} calls={calls}",
            flush=True,
        )
        for index in range(calls):
            request = server.receive(CALL_TIMEOUT)
            if request is None:
                raise RuntimeError(f"receive index={index}: timed out")
            request_view: memoryview | None = request.payload_view()
            try:
                nonce, value = unpack_request(index, request_view)
            finally:
                del request_view
            marker = value >> 32
            operand = value & 0xFFFF_FFFF
            if report_index(index):
                print(
                    f"RBT_RPC_INTEROP REQUEST language=python role=server index={index} "
                    f"nonce={nonce} marker={marker} operand={operand}",
                    flush=True,
                )
            response = request.loan_response()
            response_view: memoryview | None = response.payload_view()
            try:
                if response_view.nbytes != PAYLOAD.size:
                    raise RuntimeError(
                        f"response index={index} has {response_view.nbytes} bytes"
                    )
                PAYLOAD.pack_into(response_view, 0, nonce, expected_value(nonce, value))
            finally:
                del response_view
            response.send()
            del response
            del request
        report_idle(server, "server", service, calls, client_side=False)
        print(
            f"RBT_RPC_INTEROP PASS language=python role=server service={service} calls={calls}",
            flush=True,
        )
    finally:
        server.close()


def run_client(service: str, calls: int) -> None:
    client = open_client(service)
    try:
        print(
            f"RBT_RPC_INTEROP READY language=python role=client service={service} calls={calls}",
            flush=True,
        )
        for index in range(calls):
            nonce, value = request_for(index)

            def fill(loan: Any) -> None:
                request_view: memoryview | None = loan.payload_view()
                try:
                    if request_view.nbytes != PAYLOAD.size:
                        raise RuntimeError(
                            f"request index={index} has {request_view.nbytes} bytes"
                        )
                    PAYLOAD.pack_into(request_view, 0, nonce, value)
                finally:
                    del request_view

            response = client.call(fill, timeout=CALL_TIMEOUT)
            response_view: memoryview | None = response.payload_view()
            try:
                if response_view.nbytes != PAYLOAD.size:
                    raise RuntimeError(
                        f"response index={index} has {response_view.nbytes} bytes"
                    )
                actual_nonce, checksum = PAYLOAD.unpack_from(response_view)
            finally:
                del response_view
            expected = expected_value(nonce, value)
            if actual_nonce != nonce or checksum != expected:
                raise RuntimeError(
                    f"response mismatch index={index} expected_nonce={nonce} "
                    f"actual_nonce={actual_nonce} expected_value={expected} "
                    f"actual_value={checksum}"
                )
            if report_index(index):
                print(
                    f"RBT_RPC_INTEROP RESPONSE language=python role=client index={index} "
                    f"nonce={actual_nonce} checksum={checksum}",
                    flush=True,
                )
            # The native response guard owns the single borrowed response
            # slot.  Release it before the next call with the bounded
            # max_borrowed_responses=1 resource configuration.
            del response
        report_idle(client, "client", service, calls, client_side=True)
        print(
            f"RBT_RPC_INTEROP PASS language=python role=client service={service} calls={calls}",
            flush=True,
        )
    finally:
        client.close()


async def run_async_server(service: str, calls: int) -> None:
    server = open_server(service)
    try:
        print(
            f"RBT_RPC_INTEROP READY language=python role=async-server service={service} calls={calls}",
            flush=True,
        )
        for index in range(calls):
            request = await server.receive_async(CALL_TIMEOUT)
            if request is None:
                raise RuntimeError(f"receive index={index}: timed out")
            request_view = request.payload_view()
            try:
                nonce, value = unpack_request(index, request_view)
            finally:
                del request_view
            response = request.loan_response()
            response_view = response.payload_view()
            try:
                PAYLOAD.pack_into(response_view, 0, nonce, expected_value(nonce, value))
            finally:
                del response_view
            response.send()
            del response, request
        report_idle(server, "async-server", service, calls, client_side=False)
        print(
            f"RBT_RPC_INTEROP PASS language=python role=async-server service={service} calls={calls}",
            flush=True,
        )
    finally:
        server.close()


async def run_async_client(service: str, calls: int) -> None:
    client = open_client(service)
    try:
        print(
            f"RBT_RPC_INTEROP READY language=python role=async-client service={service} calls={calls}",
            flush=True,
        )
        for index in range(calls):
            nonce, value = request_for(index)

            def fill(loan: Any) -> None:
                view = loan.payload_view()
                try:
                    PAYLOAD.pack_into(view, 0, nonce, value)
                finally:
                    del view

            response = await client.call_async(fill, timeout=CALL_TIMEOUT)
            view = response.payload_view()
            try:
                actual_nonce, checksum = PAYLOAD.unpack_from(view)
            finally:
                del view
            expected = expected_value(nonce, value)
            if actual_nonce != nonce or checksum != expected:
                raise RuntimeError(
                    f"async response mismatch index={index} expected_nonce={nonce} "
                    f"actual_nonce={actual_nonce} expected_value={expected} actual_value={checksum}"
                )
            if report_index(index):
                print(
                    f"RBT_RPC_INTEROP RESPONSE language=python role=async-client index={index} "
                    f"nonce={actual_nonce} checksum={checksum}",
                    flush=True,
                )
            del response
        report_idle(client, "async-client", service, calls, client_side=True)
        print(
            f"RBT_RPC_INTEROP PASS language=python role=async-client service={service} calls={calls}",
            flush=True,
        )
    finally:
        client.close()


async def run_async_dynamic_client(service: str, calls: int) -> None:
    client = rbt.RpcClient.open(dynamic_descriptor(service))
    try:
        print(
            f"RBT_RPC_INTEROP READY language=python role=async-dynamic-client service={service} calls={calls}",
            flush=True,
        )
        for index in range(calls):
            nonce = NONCE_BASE | index

            def fill(loan: rbt.RpcRequestLoan) -> None:
                loan.set_image_header(
                    sequence=nonce,
                    timestamp_ns=DYNAMIC_TIMESTAMP_BASE + index,
                    width=DYNAMIC_REQUEST_BYTES,
                    height=1,
                    stride=DYNAMIC_REQUEST_BYTES,
                    encoding=rbt.PixelFormat.GRAY8,
                )
                view = loan.payload_view().cast("B")
                try:
                    for offset in range(DYNAMIC_REQUEST_BYTES):
                        view[offset] = dynamic_request_byte(index, offset)
                finally:
                    del view

            response = await client.call_async(
                fill,
                timeout=CALL_TIMEOUT,
                element_count=DYNAMIC_REQUEST_BYTES,
            )
            image = response.as_image()
            if (
                image.sequence != nonce
                or image.timestamp_ns
                != ((DYNAMIC_TIMESTAMP_BASE + index) ^ TRANSFORM_XOR)
                or image.width != DYNAMIC_RESPONSE_BYTES
                or image.height != 1
                or image.stride != DYNAMIC_RESPONSE_BYTES
                or image.encoding != rbt.PixelFormat.GRAY8
                or image.payload_nbytes != DYNAMIC_RESPONSE_BYTES
            ):
                raise RuntimeError(f"dynamic response header mismatch index={index}")
            view = image.payload_view().cast("B")
            try:
                for offset in range(DYNAMIC_RESPONSE_BYTES):
                    actual = view[offset]
                    expected = dynamic_response_byte(index, offset)
                    if actual != expected:
                        raise RuntimeError(
                            f"dynamic response byte mismatch index={index} offset={offset} "
                            f"expected={expected} actual={actual}"
                        )
                checksum = dynamic_checksum(view)
            finally:
                del view
            if report_index(index):
                print(
                    f"RBT_RPC_INTEROP DYNAMIC language=python role=async-client index={index} "
                    f"nonce={nonce} request_bytes={DYNAMIC_REQUEST_BYTES} "
                    f"response_bytes={DYNAMIC_RESPONSE_BYTES} response_checksum={checksum}",
                    flush=True,
                )
            del image, response
        report_idle(client, "async-dynamic-client", service, calls, client_side=True)
        print(
            f"RBT_RPC_INTEROP PASS language=python role=async-dynamic-client service={service} calls={calls}",
            flush=True,
        )
    finally:
        client.close()


def main() -> int:
    args = parse_args()
    try:
        if args.role == "server":
            run_server(args.service, args.calls)
        elif args.role == "client":
            run_client(args.service, args.calls)
        elif args.role == "async-server":
            asyncio.run(run_async_server(args.service, args.calls))
        elif args.role == "async-client":
            asyncio.run(run_async_client(args.service, args.calls))
        else:
            asyncio.run(run_async_dynamic_client(args.service, args.calls))
    except BaseException as error:
        print(
            f"RBT_RPC_INTEROP FAIL language=python role={args.role} error={error}",
            file=sys.stderr,
            flush=True,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


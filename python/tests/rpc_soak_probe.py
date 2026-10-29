#!/usr/bin/env python3
"""Standalone asyncio RPC ownership and terminal-arbitration soak probe.

The probe deliberately keeps native RPC waits behind ``receive_async`` and
``call_async``.  It never polls readiness from Python: native wait sets wake
the one asyncio event loop when a request, response, deadline, cancellation,
or activation expiry wins.
"""

from __future__ import annotations

import argparse
import asyncio
import os
import resource
import struct
import time
import uuid
import warnings
from dataclasses import dataclass
from typing import Any

import rbt
from rbt._generated.demo import DETECTION_DESCRIPTOR, FRAME_DESCRIPTOR


DEFAULT_CALL_TIMEOUT = 2.0
DEADLINE_TIMEOUT = 0.002
LATE_RESPONSE_DELAY = 0.015
PAYLOAD = struct.Struct("<QQ")
NONCE_BASE = 0xBADC_0000_0000_0000
MARKER_BASE = 0xC0DE_0000
OPERAND_BASE = 0x0100
OPERAND_STEP = 17
TRANSFORM_MULTIPLIER = 0x9E37_79B1_85EB_CA87
TRANSFORM_XOR = 0xD1CE_F00D_CAFE_BEEF
MASK = (1 << 64) - 1


@dataclass
class Counters:
    cycles: int = 0
    responses: int = 0
    deadlines: int = 0
    cancellations: int = 0
    late_response_attempts: int = 0
    late_response_failures: int = 0


@dataclass(frozen=True)
class Resources:
    rss_kib: int
    fds: int
    threads: int


@dataclass(frozen=True)
class IdleSnapshot:
    client_registrations: int
    server_registrations: int
    in_flight_current: int
    response_guards_current: int
    requests_current: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--duration-seconds",
        type=float,
        default=float(os.environ.get("RBT_RPC_SOAK_SECONDS", "300")),
    )
    parser.add_argument("--cycles", type=int)
    parser.add_argument(
        "--service",
        default=os.environ.get("RBT_RPC_SOAK_SERVICE"),
        help="optional fixed native service name (otherwise a unique name is used)",
    )
    args = parser.parse_args()
    if args.duration_seconds <= 0:
        parser.error("--duration-seconds must be positive")
    if args.cycles is not None and args.cycles <= 0:
        parser.error("--cycles must be positive")
    if args.service is not None and not args.service:
        parser.error("--service must not be empty")
    return args


def native_thread_count() -> int:
    try:
        return len(os.listdir("/proc/self/task"))
    except OSError:
        return 1


def resources() -> Resources:
    # ru_maxrss is the useful monotonic memory high-water mark.  FD and OS
    # thread counts are current values and are sampled again after close.
    return Resources(
        rss_kib=resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
        fds=len(os.listdir("/proc/self/fd")),
        threads=native_thread_count(),
    )


def descriptor(service: str) -> rbt.RpcDescriptor:
    return rbt.RpcDescriptor.fixed(
        service,
        FRAME_DESCRIPTOR,
        DETECTION_DESCRIPTOR,
        resources=rbt.RpcResourceConfig(
            max_in_flight=2,
            max_clients=1,
            max_request_loans=2,
            response_buffer_size=2,
            max_borrowed_responses=2,
            max_response_loans=2,
            max_request_slice_len=1,
            max_response_slice_len=1,
        ),
    )


def request_for(index: int) -> tuple[int, int]:
    marker = MARKER_BASE | index
    operand = OPERAND_BASE + index * OPERAND_STEP
    return NONCE_BASE | index, ((marker << 32) | operand) & MASK


def rotate_left(value: int, shift: int) -> int:
    return ((value << shift) | (value >> (64 - shift))) & MASK


def expected_value(nonce: int, value: int) -> int:
    return ((nonce * TRANSFORM_MULTIPLIER) & MASK) ^ rotate_left(value, 23) ^ TRANSFORM_XOR


def read_payload(owner: Any, index: int, kind: str) -> tuple[int, int]:
    view: memoryview | None = owner.payload_view()
    try:
        if view.nbytes != PAYLOAD.size:
            raise RuntimeError(f"{kind} index={index} has {view.nbytes} bytes")
        nonce, value = PAYLOAD.unpack_from(view)
    finally:
        del view
    expected_nonce, expected_input = request_for(index)
    if kind == "request":
        if (nonce, value) != (expected_nonce, expected_input):
            raise RuntimeError(
                f"request mismatch index={index} expected_nonce={expected_nonce} "
                f"actual_nonce={nonce} expected_value={expected_input} actual_value={value}"
            )
    else:
        expected = expected_value(expected_nonce, expected_input)
        if (nonce, value) != (expected_nonce, expected):
            raise RuntimeError(
                f"response mismatch index={index} expected_nonce={expected_nonce} "
                f"actual_nonce={nonce} expected_checksum={expected} actual_checksum={value}"
            )
    return nonce, value


def fill_request(loan: Any, index: int) -> None:
    view: memoryview | None = loan.payload_view()
    try:
        if view.nbytes != PAYLOAD.size:
            raise RuntimeError(f"request loan index={index} has {view.nbytes} bytes")
        PAYLOAD.pack_into(view, 0, *request_for(index))
    finally:
        del view


def fill_response(loan: Any, index: int) -> None:
    nonce, value = request_for(index)
    view: memoryview | None = loan.payload_view()
    try:
        if view.nbytes != PAYLOAD.size:
            raise RuntimeError(f"response loan index={index} has {view.nbytes} bytes")
        PAYLOAD.pack_into(view, 0, nonce, expected_value(nonce, value))
    finally:
        del view


async def respond(
    server: rbt.RpcServer,
    index: int,
    *,
    delay: float = 0.0,
    received: asyncio.Event | None = None,
    counters: Counters,
) -> None:
    request = await server.receive_async(timeout=DEFAULT_CALL_TIMEOUT)
    if request is None:
        raise RuntimeError(f"server receive timed out index={index}")
    try:
        read_payload(request, index, "request")
        if received is not None:
            received.set()
        if delay:
            await asyncio.sleep(delay)
        counters.late_response_attempts += int(delay > 0)
        try:
            response = request.respond(lambda loan: fill_response(loan, index))
            response.send()
        except (rbt.RpcError, rbt.RpcActivationExpiredError):
            # Deadline/cancellation owns the terminal state. A response
            # arriving afterwards is intentionally harmless and cannot revive
            # the completed logical call.
            counters.late_response_failures += int(delay > 0)
        finally:
            if "response" in locals():
                del response
    finally:
        del request


async def successful_call(
    client: rbt.RpcClient, server: rbt.RpcServer, counters: Counters, index: int
) -> None:
    serving = asyncio.create_task(respond(server, index, counters=counters))
    try:
        response = await client.call_async(
            lambda loan: fill_request(loan, index), timeout=DEFAULT_CALL_TIMEOUT
        )
        read_payload(response, index, "response")
        del response
        counters.responses += 1
    finally:
        await serving


async def deadline_call(
    client: rbt.RpcClient, server: rbt.RpcServer, counters: Counters, index: int
) -> None:
    serving = asyncio.create_task(
        respond(server, index, delay=LATE_RESPONSE_DELAY, counters=counters)
    )
    try:
        try:
            await client.call_async(
                lambda loan: fill_request(loan, index), timeout=DEADLINE_TIMEOUT
            )
        except rbt.RpcDeadlineExceededError:
            counters.deadlines += 1
        else:
            raise RuntimeError("native deadline call unexpectedly completed")
    finally:
        await serving


async def cancelled_call(
    client: rbt.RpcClient, server: rbt.RpcServer, counters: Counters, index: int
) -> None:
    received = asyncio.Event()
    serving = asyncio.create_task(
        respond(
            server,
            index,
            delay=LATE_RESPONSE_DELAY,
            received=received,
            counters=counters,
        )
    )
    call = asyncio.create_task(
        client.call_async(
            lambda loan: fill_request(loan, index), timeout=DEFAULT_CALL_TIMEOUT
        )
    )
    try:
        await asyncio.wait_for(received.wait(), timeout=DEFAULT_CALL_TIMEOUT)
        if client._debug_async_registration_count() != 1:
            raise RuntimeError("cancelled call did not register with native bridge")
        call.cancel()
        try:
            await call
        except asyncio.CancelledError:
            counters.cancellations += 1
        else:
            raise RuntimeError("cancelled asyncio call unexpectedly completed")
    finally:
        if not call.done():
            call.cancel()
        await asyncio.gather(call, return_exceptions=True)
        await serving


def assert_idle(
    client: rbt.RpcClient, server: rbt.RpcServer, label: str
) -> IdleSnapshot:
    client_registrations = client._debug_async_registration_count()
    server_registrations = server._debug_async_registration_count()
    client_pressure = client.pressure.client
    server_pressure = server.pressure.server
    if client_pressure is None or server_pressure is None:
        raise RuntimeError(f"{label}: native pressure snapshot is missing endpoint metrics")
    snapshot = IdleSnapshot(
        client_registrations=client_registrations,
        server_registrations=server_registrations,
        in_flight_current=client_pressure.in_flight_current,
        response_guards_current=client_pressure.response_guards_current,
        requests_current=server_pressure.requests_current,
    )
    if (
        snapshot.client_registrations != 0
        or snapshot.server_registrations != 0
        or snapshot.in_flight_current != 0
        or snapshot.response_guards_current != 0
        or snapshot.requests_current != 0
    ):
        raise RuntimeError(
            f"{label}: RPC ownership did not drain: "
            f"client_registrations={snapshot.client_registrations} "
            f"server_registrations={snapshot.server_registrations} "
            f"in_flight_current={snapshot.in_flight_current} "
            f"response_guards_current={snapshot.response_guards_current} "
            f"requests_current={snapshot.requests_current}"
        )
    return snapshot


def assert_no_pending_tasks(label: str) -> None:
    current = asyncio.current_task()
    pending = [
        task for task in asyncio.all_tasks() if task is not current and not task.done()
    ]
    if pending:
        raise RuntimeError(f"{label}: pending asyncio tasks={len(pending)}")


async def run(
    duration_seconds: float, cycle_limit: int | None, service_name: str | None
) -> None:
    service = service_name or f"rbt-python-rpc-soak-{uuid.uuid4().hex}"
    endpoint = descriptor(service)
    threads_before = native_thread_count()
    server = rbt.RpcServer.open(endpoint)
    client = rbt.RpcClient.open(endpoint)
    counters = Counters()
    started = time.monotonic()
    baseline: Resources | None = None
    initial = resources()
    maximum = initial
    next_index = 0
    try:
        while (
            time.monotonic() - started < duration_seconds
            and (cycle_limit is None or counters.cycles < cycle_limit)
        ):
            await successful_call(client, server, counters, next_index)
            next_index += 1
            await deadline_call(client, server, counters, next_index)
            next_index += 1
            await cancelled_call(client, server, counters, next_index)
            next_index += 1
            # A successful call immediately after cancellation proves that a
            # late response cannot poison the reusable client endpoint.
            await successful_call(client, server, counters, next_index)
            next_index += 1
            counters.cycles += 1
            cycle_idle = assert_idle(client, server, f"cycle={counters.cycles}")

            snapshot = resources()
            baseline = baseline or snapshot
            maximum = Resources(
                max(maximum.rss_kib, snapshot.rss_kib),
                max(maximum.fds, snapshot.fds),
                max(maximum.threads, snapshot.threads),
            )
            print(
                "RBT_RPC_SOAK_PYTHON PROGRESS "
                f"cycles={counters.cycles} responses={counters.responses} "
                f"deadlines={counters.deadlines} cancellations={counters.cancellations} "
                f"late_response_attempts={counters.late_response_attempts} "
                f"client_registrations={cycle_idle.client_registrations} "
                f"server_registrations={cycle_idle.server_registrations} "
                f"in_flight_current={cycle_idle.in_flight_current} "
                f"response_guards_current={cycle_idle.response_guards_current} "
                f"requests_current={cycle_idle.requests_current} "
                f"rss_kib={snapshot.rss_kib} fds={snapshot.fds} threads={snapshot.threads}",
                flush=True,
            )

        final_idle = assert_idle(client, server, "pre-close")
        assert_no_pending_tasks("pre-close")
        final_open = resources()
        baseline = baseline or final_open
        maximum = Resources(
            max(maximum.rss_kib, final_open.rss_kib),
            max(maximum.fds, final_open.fds),
            max(maximum.threads, final_open.threads),
        )
        if maximum.rss_kib > baseline.rss_kib + 64 * 1024:
            raise RuntimeError(
                f"RSS growth exceeded 64 MiB: baseline={baseline.rss_kib} maximum={maximum.rss_kib}"
            )
        if maximum.fds > baseline.fds + 64:
            raise RuntimeError(
                f"FD growth exceeded 64: baseline={baseline.fds} maximum={maximum.fds}"
            )
        if maximum.threads > baseline.threads + 32:
            raise RuntimeError(
                f"thread growth exceeded 32: baseline={baseline.threads} maximum={maximum.threads}"
            )
    finally:
        client.close()
        server.close()
        # close() expires native wait registrations synchronously. A short
        # event-loop yield lets the bridge thread observe expiry without a
        # Python-side readiness poll.
        await asyncio.sleep(0)
        await asyncio.sleep(0.02)

    if client._debug_async_registration_count() != 0:
        raise RuntimeError("client native async registrations remained after close")
    if server._debug_async_registration_count() != 0:
        raise RuntimeError("server native async registrations remained after close")
    assert_no_pending_tasks("post-close")
    # This bounded shutdown observation is about thread teardown, not native
    # readiness; all request/response readiness above is event-driven.
    thread_deadline = time.monotonic() + 2.0
    threads_after = native_thread_count()
    while threads_after > threads_before and time.monotonic() < thread_deadline:
        await asyncio.sleep(0.02)
        threads_after = native_thread_count()
    if threads_after > threads_before:
        raise RuntimeError(
            f"native RPC threads did not return after close: before={threads_before} after={threads_after}"
        )
    final = resources()
    print(
        "RBT_RPC_SOAK_PYTHON PASS "
        f"service={service} cycles={counters.cycles} responses={counters.responses} "
        f"deadlines={counters.deadlines} cancellations={counters.cancellations} "
        f"late_response_attempts={counters.late_response_attempts} "
        f"late_response_failures={counters.late_response_failures} pending=0 "
        f"client_registrations={final_idle.client_registrations} "
        f"server_registrations={final_idle.server_registrations} "
        f"in_flight_current={final_idle.in_flight_current} "
        f"response_guards_current={final_idle.response_guards_current} "
        f"requests_current={final_idle.requests_current} "
        f"rss_initial_kib={initial.rss_kib} rss_baseline_kib={baseline.rss_kib} "
        f"rss_max_kib={maximum.rss_kib} rss_final_kib={final.rss_kib} "
        f"fds_initial={initial.fds} fds_max={maximum.fds} fds_final={final.fds} "
        f"threads_initial={threads_before} threads_max={maximum.threads} "
        f"threads_final={threads_after} warnings=0",
        flush=True,
    )


def main() -> int:
    args = parse_args()
    with warnings.catch_warnings(record=True) as emitted:
        warnings.simplefilter("always")
        try:
            asyncio.run(run(args.duration_seconds, args.cycles, args.service))
        except BaseException as error:
            print(f"RBT_RPC_SOAK_PYTHON FAIL error={error}", flush=True)
            return 1
        if emitted:
            details = "; ".join(str(item.message) for item in emitted)
            print(f"RBT_RPC_SOAK_PYTHON FAIL warnings={details}", flush=True)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

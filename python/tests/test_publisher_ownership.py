from __future__ import annotations

import gc
import os
import queue
import signal
import subprocess
import sys
import threading
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Any, Iterator

import numpy as np
import pytest

import rbt

from conftest import REPO_ROOT, unique_service


def _image_publisher(label: str, maximum_payload_bytes: int = 256) -> rbt.ImagePublisher:
    return rbt.ImagePublisher.open(
        rbt.StreamDescriptor.image(unique_service(label)),
        maximum_payload_bytes=maximum_payload_bytes,
    )


def _resource_snapshot() -> tuple[int, int, int]:
    rss_kib = 0
    with open("/proc/self/status", encoding="utf-8") as status_file:
        for line in status_file:
            if line.startswith("VmRSS:"):
                rss_kib = int(line.split()[1])
                break
    return rss_kib, len(os.listdir("/proc/self/fd")), len(os.listdir("/proc/self/task"))


def _wait_for_output(
    process: subprocess.Popen[str], marker: str, timeout: float = 5
) -> str:
    assert process.stdout is not None
    lines: list[str] = []
    output: queue.Queue[str | None] = queue.Queue()

    def read_lines() -> None:
        assert process.stdout is not None
        for line in process.stdout:
            output.put(line)
        output.put(None)

    threading.Thread(target=read_lines, daemon=True).start()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        try:
            line = output.get(timeout=remaining)
        except queue.Empty:
            break
        if line is None:
            break
        lines.append(line)
        if marker in line:
            return "".join(lines)
    pytest.fail(f"process did not emit {marker!r}:\n{''.join(lines)}")


def test_derived_only_alias_keeps_loan_mapped_until_final_drop() -> None:
    publisher = _image_publisher("derived-only")
    loan = publisher.loan(4, 3, rbt.PixelFormat.RGB8, sequence=7)
    root = loan.numpy()
    derived = root.reshape(6, 6).transpose()
    derived[0, 0] = 0xA5

    del root
    gc.collect()
    assert loan._debug_export_count() == 1
    with pytest.raises(rbt.LoanStillMappedError):
        loan.send()

    del derived
    gc.collect()
    assert loan._debug_export_count() == 0
    loan.send()


def test_reshape_transpose_and_memoryview_chain_retains_one_lease() -> None:
    publisher = _image_publisher("view-chain")
    loan = publisher.loan(4, 3, rbt.PixelFormat.RGB8)
    root = loan.numpy()
    reshaped = root.reshape(6, 6)
    transposed = reshaped.transpose()
    chained = memoryview(transposed)
    chained[0, 0] = 0x3C

    assert not chained.readonly
    assert loan._debug_export_count() == 1
    del root, reshaped, transposed
    gc.collect()
    assert loan._debug_export_count() == 1
    with pytest.raises(rbt.LoanStillMappedError):
        loan.send()

    chained.release()
    del chained
    gc.collect()
    assert loan._debug_export_count() == 0
    loan.send()


def test_two_independent_roots_have_independent_release_counts() -> None:
    publisher = _image_publisher("two-roots")
    loan = publisher.loan(4, 3, rbt.PixelFormat.RGB8)
    first = loan.numpy()
    second = loan.payload_view()
    assert first.__array_interface__["data"][0] == loan._debug_payload_address()
    assert second.readonly is False
    assert loan._debug_export_count() == 2

    del first
    gc.collect()
    assert loan._debug_export_count() == 1
    with pytest.raises(rbt.LoanStillMappedError):
        loan.send()

    del second
    gc.collect()
    assert loan._debug_export_count() == 0
    loan.send()


def test_owned_copy_does_not_keep_shared_memory_loan_mapped() -> None:
    publisher = _image_publisher("copy-escape")
    loan = publisher.loan(4, 3, rbt.PixelFormat.RGB8)
    shared = loan.numpy()
    owned = shared.copy()
    assert owned.flags.owndata
    assert owned.__array_interface__["data"][0] != loan._debug_payload_address()

    del shared
    gc.collect()
    assert loan._debug_export_count() == 0
    loan.send()
    assert int(owned[0, 0, 0]) == 0
    del owned


def test_array_outlives_loan_handle_and_pool_recovers_after_final_alias_drop() -> None:
    publisher = _image_publisher("loan-handle-lifetime")
    first = publisher.loan(4, 3, rbt.PixelFormat.RGB8)
    first_array = first.numpy()
    derived_only = first_array[:, :2, :]
    del first
    del first_array
    gc.collect()

    # The view chain owns memory lifetime even though send authority is gone.
    derived_only[...] = 0x6B
    assert int(derived_only[-1, -1, -1]) == 0x6B

    second = publisher.loan(4, 3, rbt.PixelFormat.RGB8)
    with pytest.raises(rbt.LoanPoolExhaustedError):
        publisher.loan(4, 3, rbt.PixelFormat.RGB8)

    # Releasing the derived-only array returns one pool slot while the
    # second public loan remains active.
    del derived_only
    gc.collect()
    recovered = publisher.loan(4, 3, rbt.PixelFormat.RGB8)
    del second, recovered


def test_cross_thread_final_alias_drop_unblocks_send() -> None:
    publisher = _image_publisher("cross-thread-final-drop")
    loan = publisher.loan(4, 3, rbt.PixelFormat.RGB8)
    image = loan.numpy()
    handoff: queue.Queue[np.ndarray[Any, np.dtype[np.uint8]]] = queue.Queue()
    failures: queue.Queue[BaseException] = queue.Queue()
    completed = threading.Event()

    def drop_on_worker() -> None:
        try:
            transferred = handoff.get(timeout=5)
            assert transferred.__array_interface__["data"][0] == loan._debug_payload_address()
            del transferred
            gc.collect()
        except BaseException as error:  # propagate assertions to the parent thread
            failures.put(error)
        finally:
            completed.set()

    worker = threading.Thread(target=drop_on_worker)
    worker.start()
    handoff.put(image)
    del image
    gc.collect()
    assert completed.wait(timeout=5)
    worker.join(timeout=5)
    assert not worker.is_alive()
    assert failures.empty()
    assert loan._debug_export_count() == 0
    loan.send()


def _run_atomic_race(iterations: int) -> tuple[int, int]:
    publisher = _image_publisher(f"atomic-race-{iterations}")
    requests: queue.Queue[tuple[rbt.LoanedImageSample, bool]] = queue.Queue()
    outcomes: queue.Queue[tuple[str, type[BaseException] | None]] = queue.Queue()
    worker_errors: queue.Queue[BaseException] = queue.Queue()
    barrier = threading.Barrier(2)

    def send_worker() -> None:
        try:
            for _ in range(iterations):
                current, prefer_numpy = requests.get(timeout=10)
                barrier.wait(timeout=10)
                if prefer_numpy:
                    # Yield only to vary which atomic state transition wins;
                    # correctness never relies on a duration or discovery.
                    time.sleep(0)
                try:
                    current.send()
                except BaseException as error:
                    outcomes.put(("error", type(error)))
                else:
                    outcomes.put(("sent", None))
                finally:
                    del current
        except BaseException as error:
            worker_errors.put(error)

    worker = threading.Thread(target=send_worker)
    worker.start()
    numpy_wins = 0
    send_wins = 0
    try:
        for sequence in range(iterations):
            loan = publisher.loan(4, 3, rbt.PixelFormat.RGB8, sequence=sequence)
            prefer_numpy = sequence % 2 == 0
            requests.put((loan, prefer_numpy))
            barrier.wait(timeout=10)
            if not prefer_numpy:
                time.sleep(0)
            try:
                image = loan.numpy()
            except rbt.LoanConsumedError:
                send_wins += 1
                outcome, error_type = outcomes.get(timeout=10)
                assert outcome == "sent"
                assert error_type is None
                with pytest.raises(rbt.LoanConsumedError):
                    loan.send()
            else:
                numpy_wins += 1
                image[0, 0, 0] = sequence & 0xFF
                outcome, error_type = outcomes.get(timeout=10)
                assert outcome == "error"
                assert error_type is rbt.LoanStillMappedError
                del image
                gc.collect()
                loan.send()
            del loan
    finally:
        worker.join(timeout=10)
    assert not worker.is_alive()
    assert worker_errors.empty()
    assert numpy_wins + send_wins == iterations
    assert numpy_wins > 0 and send_wins > 0
    return numpy_wins, send_wins


def test_concurrent_numpy_and_send_are_atomic_quick() -> None:
    numpy_wins, send_wins = _run_atomic_race(64)
    print("PUBLISHER_RACE_EVIDENCE", f"iterations=64 numpy_wins={numpy_wins} send_wins={send_wins}")


@pytest.mark.full
def test_concurrent_numpy_and_send_are_atomic_full() -> None:
    numpy_wins, send_wins = _run_atomic_race(2_000)
    print(
        "PUBLISHER_RACE_EVIDENCE",
        f"iterations=2000 numpy_wins={numpy_wins} send_wins={send_wins}",
    )


def test_unsent_loan_is_not_delivered_and_returns_to_pool() -> None:
    descriptor = rbt.StreamDescriptor.image(unique_service("unsent-not-delivered"))
    publisher = rbt.ImagePublisher.open(descriptor, maximum_payload_bytes=256)
    subscriber = rbt.ImageSubscriber.open(descriptor)
    loan = publisher.loan(4, 3, rbt.PixelFormat.RGB8, sequence=91)
    image = loan.numpy()
    image[...] = 0xD1
    del image, loan
    gc.collect()
    assert subscriber.try_receive() is None

    first = publisher.loan(4, 3, rbt.PixelFormat.RGB8)
    second = publisher.loan(4, 3, rbt.PixelFormat.RGB8)
    with pytest.raises(rbt.LoanPoolExhaustedError):
        publisher.loan(4, 3, rbt.PixelFormat.RGB8)
    del first, second
    gc.collect()
    recovered = publisher.loan(4, 3, rbt.PixelFormat.RGB8)
    del recovered


def _run_cycle_stress(iterations: int) -> tuple[tuple[int, int, int], tuple[int, int, int]]:
    publisher = _image_publisher(f"cycle-stress-{iterations}", maximum_payload_bytes=256)
    before = _resource_snapshot()
    for sequence in range(iterations):
        loan = publisher.loan(8, 1, rbt.PixelFormat.RGB8, sequence=sequence)
        image = loan.numpy()
        image[0, 0, 0] = sequence & 0xFF
        del image
        if sequence % 3:
            loan.send()
        del loan
        if sequence % 100 == 0:
            gc.collect()
    gc.collect()
    after = _resource_snapshot()
    assert after[0] <= before[0] + 64 * 1024
    assert after[1] <= before[1] + 4
    assert after[2] <= before[2] + 2
    return before, after


def test_send_drop_cycle_stress_quick() -> None:
    before, after = _run_cycle_stress(256)
    print("PUBLISHER_RESOURCE_EVIDENCE", f"cycles=256 before={before} after={after}")


@pytest.mark.full
def test_send_drop_cycle_stress_full() -> None:
    before, after = _run_cycle_stress(5_000)
    print("PUBLISHER_RESOURCE_EVIDENCE", f"cycles=5000 before={before} after={after}")


@pytest.mark.full
def test_interpreter_shutdown_with_outstanding_unsent_publisher_view() -> None:
    service = unique_service("publisher-clean-exit")
    probe = REPO_ROOT / "python" / "tests" / "publisher_exit_probe.py"
    completed = subprocess.run(
        [sys.executable, str(probe), service],
        cwd="/tmp",
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=10,
        check=False,
    )
    assert completed.returncode == 0, completed.stdout
    assert "PUBLISHER_EXIT_PROBE" in completed.stdout


@pytest.mark.full
def test_sigterm_with_outstanding_unsent_publisher_view_is_prompt() -> None:
    service = unique_service("publisher-sigterm")
    probe = REPO_ROOT / "python" / "tests" / "publisher_exit_probe.py"
    process = subprocess.Popen(
        [sys.executable, str(probe), service, "block"],
        cwd="/tmp",
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        assert process.stdout is not None
        _wait_for_output(process, "PUBLISHER_EXIT_PROBE")
        os.kill(process.pid, signal.SIGTERM)
        process.wait(timeout=2)
        assert process.returncode == -signal.SIGTERM
    finally:
        if process.poll() is None:
            process.kill()
            process.wait(timeout=2)


@contextmanager
def _readiness_fixture(service: str, deadline_ms: int) -> Iterator[subprocess.Popen[str]]:
    configured = os.environ.get("RBT_READINESS_DEADLINE_FIXTURE")
    fixture = (
        Path(configured)
        if configured
        else REPO_ROOT / "target" / "debug" / "readiness_deadline_fixture"
    )
    if not fixture.is_file():
        pytest.fail(f"readiness deadline fixture is missing: {fixture}")
    process = subprocess.Popen(
        [str(fixture), service, str(deadline_ms)],
        cwd="/tmp",
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )
    try:
        assert process.stdout is not None
        _wait_for_output(process, "READINESS_DEADLINE_READY")
        yield process
    finally:
        if process.stdin is not None:
            process.stdin.close()
        if process.poll() is None:
            process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


@pytest.mark.full
def test_readiness_notify_failure_consumes_loan_after_payload_delivery() -> None:
    service = unique_service("readiness-deadline")
    descriptor = rbt.StreamDescriptor.image(service)
    deadline_ms = 10
    with _readiness_fixture(service, deadline_ms):
        publisher = rbt.ImagePublisher.open(descriptor, maximum_payload_bytes=256)
        subscriber = rbt.ImageSubscriber.open(descriptor)
        time.sleep(0.1)

        loan = publisher.loan(4, 3, rbt.PixelFormat.RGB8, sequence=123)
        image = loan.numpy()
        image[...] = 0x4E
        del image
        gc.collect()
        with pytest.raises(rbt.ReadinessNotifyError):
            loan.send()

        received = subscriber.receive(timeout=5)
        assert received is not None
        assert received.sequence == 123
        received_image = received.numpy()
        assert int(received_image[0, 0, 0]) == 0x4E
        del received_image, received
        with pytest.raises(rbt.LoanConsumedError):
            loan.send()
        with pytest.raises(rbt.LoanConsumedError):
            loan.numpy()

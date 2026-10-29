from __future__ import annotations

import gc
import re
import subprocess
import time

import pytest

import rbt

from conftest import REPO_ROOT, unique_service


MIXED_PRESSURE = REPO_ROOT / "target" / "release" / "mixed_pressure"


def _start(*args: object) -> subprocess.Popen[str]:
    return subprocess.Popen(
        [str(argument) for argument in args],
        cwd="/tmp",
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def _finish(process: subprocess.Popen[str], timeout: float = 8) -> str:
    try:
        output, _ = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        output, _ = process.communicate(timeout=2)
        pytest.fail(f"process timed out:\n{output}")
    assert process.returncode == 0, output
    return output


def _cleanup(*processes: subprocess.Popen[str]) -> None:
    for process in processes:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)


def _received_count(output: str) -> int:
    matched = re.search(r"MIXED_RUST_SUBSCRIBER complete .*received=(\d+)", output)
    if matched is None:
        matched = re.search(r"MIXED_RUST_SUBSCRIBER complete .*frames=(\d+)", output)
    assert matched is not None, output
    return int(matched.group(1))


@pytest.mark.full
def test_stalled_python_subscriber_does_not_block_rust_peer_or_publisher() -> None:
    service = unique_service("stalled-pressure")
    publisher = _start(MIXED_PRESSURE, "publisher", service, 2)
    fast = _start(MIXED_PRESSURE, "subscriber", service, 0)
    try:
        time.sleep(0.15)
        stalled = rbt.ImageSubscriber.open(rbt.StreamDescriptor.image(service))
        time.sleep(0.8)
        publisher_output = _finish(publisher)
        fast_output = _finish(fast)
        assert "frames=40" in publisher_output
        assert "send_errors=0" in publisher_output
        assert _received_count(fast_output) >= 20
        assert stalled.pressure.received_samples == 0
        print("PRESSURE_STALLED_EVIDENCE", "publisher_frames=40 rust_fast>=20 python_received=0")
    finally:
        _cleanup(publisher, fast)


@pytest.mark.full
def test_retained_python_array_preserves_pressure_and_rust_peer_progress() -> None:
    service = unique_service("retained-pressure")
    publisher = _start(MIXED_PRESSURE, "publisher", service, 2)
    fast = _start(MIXED_PRESSURE, "subscriber", service, 0)
    try:
        time.sleep(0.15)
        subscriber = rbt.ImageSubscriber.open(rbt.StreamDescriptor.image(service))
        sample = subscriber.receive(timeout=3)
        assert sample is not None
        image = sample.numpy()
        del sample
        gc.collect()
        time.sleep(0.5)
        with pytest.raises(rbt.BorrowLimitReachedError):
            subscriber.try_receive()
        publisher_output = _finish(publisher)
        fast_output = _finish(fast)
        assert "frames=40" in publisher_output
        assert "send_errors=0" in publisher_output
        assert _received_count(fast_output) >= 20
        del image
        gc.collect()
        recovered = subscriber.try_receive()
        assert recovered is not None
        print("PRESSURE_RETENTION_EVIDENCE", "publisher_frames=40 rust_fast>=20 recovery=true")
    finally:
        _cleanup(publisher, fast)


@pytest.mark.full
def test_retained_derived_array_reports_oldest_age_until_final_release() -> None:
    service = unique_service("retained-age")
    descriptor = rbt.StreamDescriptor.image(service)
    publisher = rbt.ImagePublisher.open(descriptor, maximum_payload_bytes=64)
    subscriber = rbt.ImageSubscriber.open(descriptor)

    loan = publisher.loan(2, 1, rbt.PixelFormat.RGB8, sequence=1)
    loan.numpy()[...] = 0x4A
    # The temporary array is released before send; the receive-side array is
    # the only owner intentionally retained for the age measurement.
    loan.send()
    sample = subscriber.receive(timeout=2)
    assert sample is not None
    derived = sample.numpy()[:, :1, :]
    del sample
    gc.collect()

    time.sleep(2.1)
    aged = subscriber.pressure
    assert aged.subscriber is not None
    assert aged.subscriber.borrowed_samples == 1
    assert aged.subscriber.borrowed_high_water == 1
    oldest_borrow_age_ns = aged.subscriber.oldest_borrow_age_ns
    assert isinstance(oldest_borrow_age_ns, int)
    assert oldest_borrow_age_ns >= 2_000_000_000
    print(
        "PRESSURE_RETAINED_AGE_EVIDENCE",
        f"borrowed_samples={aged.subscriber.borrowed_samples}",
        f"borrowed_high_water={aged.subscriber.borrowed_high_water}",
        f"oldest_borrow_age_ns={oldest_borrow_age_ns}",
        "threshold_ns=2000000000",
    )

    del derived
    gc.collect()
    released = subscriber.pressure
    assert released.subscriber is not None
    assert released.subscriber.borrowed_samples == 0
    assert released.subscriber.oldest_borrow_age_ns is None
    print(
        "PRESSURE_RETAINED_AGE_RELEASE_EVIDENCE",
        f"borrowed_samples={released.subscriber.borrowed_samples}",
        f"oldest_borrow_age_ns={released.subscriber.oldest_borrow_age_ns}",
    )


@pytest.mark.full
def test_slow_python_subscriber_observes_sequence_gaps_not_backlog() -> None:
    service = unique_service("slow-pressure")
    publisher = _start(MIXED_PRESSURE, "publisher", service, 1)
    try:
        time.sleep(0.15)
        subscriber = rbt.ImageSubscriber.open(rbt.StreamDescriptor.image(service))
        sequences: list[int] = []
        for _ in range(3):
            sample = subscriber.receive(timeout=2)
            assert sample is not None
            sequences.append(sample.sequence)
            del sample
            gc.collect()
            time.sleep(0.2)
        publisher_output = _finish(publisher)
        assert "frames=40" in publisher_output
        assert "send_errors=0" in publisher_output
        assert any(current > previous + 1 for previous, current in zip(sequences, sequences[1:]))
        assert subscriber.pressure.received_samples == 3
        print("PRESSURE_SLOW_EVIDENCE", f"sequences={sequences}")
    finally:
        _cleanup(publisher)

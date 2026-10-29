from __future__ import annotations

import gc
import os
import subprocess
import sys
import time

import pytest

import rbt

from conftest import REPO_ROOT, background_process, fixture_binary, unique_service


def _resource_snapshot() -> tuple[int, int, int]:
    rss_kib = 0
    with open("/proc/self/status", encoding="utf-8") as status_file:
        for line in status_file:
            if line.startswith("VmRSS:"):
                rss_kib = int(line.split()[1])
                break
    return rss_kib, len(os.listdir("/proc/self/fd")), len(os.listdir("/proc/self/task"))


@pytest.mark.full
def test_gc_stress_has_bounded_resources_and_no_borrow_failures() -> None:
    service = unique_service("gc-stress")
    before = _resource_snapshot()
    # Publish extra queue=1 frames so deliberate GC pauses may create sequence
    # gaps without reducing the number of ownership cycles exercised here.
    with background_process(fixture_binary(), service, 64, 4, 192, 1000, 10):
        subscriber = rbt.ImageSubscriber.open(rbt.StreamDescriptor.image(service))
        checksums = 0
        for index in range(500):
            sample = subscriber.receive(timeout=3)
            assert sample is not None
            image = sample.numpy()
            checksums += int(image[0, 0, 0])
            del image, sample
            if index % 50 == 0:
                gc.collect()
        gc.collect()
        assert checksums > 0
        assert subscriber.pressure.borrow_failures == 0
        del subscriber
        gc.collect()
    after = _resource_snapshot()
    assert after[0] <= before[0] + 64 * 1024
    assert after[1] <= before[1] + 4
    assert after[2] <= before[2] + 2
    print("GC_EVIDENCE", f"cycles=500 before={before} after={after}")


@pytest.mark.full
def test_idle_wait_remains_event_driven_for_ten_seconds() -> None:
    service = unique_service("idle")
    provider = REPO_ROOT / "target" / "release" / "phase4-interop"
    with background_process(provider, "image-provider-hold", service, 11_500):
        subscriber = rbt.ImageSubscriber.open(rbt.StreamDescriptor.image(service))
        resources_before = _resource_snapshot()
        cpu_before = time.process_time()
        wall_before = time.monotonic()
        assert subscriber.receive(timeout=10.0) is None
        wall_seconds = time.monotonic() - wall_before
        cpu_seconds = time.process_time() - cpu_before
        resources_after = _resource_snapshot()
        assert wall_seconds >= 9.5
        assert cpu_seconds < 0.5
        assert resources_after[1] <= resources_before[1] + 2
        assert resources_after[2] <= resources_before[2] + 1
        assert subscriber.wait_stats.samples_processed == 0
        print(
            "IDLE_EVIDENCE",
            f"wall_seconds={wall_seconds:.6f}",
            f"cpu_seconds={cpu_seconds:.6f}",
            f"before={resources_before}",
            f"after={resources_after}",
            f"waits={subscriber.wait_stats.waits}",
            f"wakeups={subscriber.wait_stats.wakeups}",
        )


@pytest.mark.full
def test_clean_interpreter_exit_with_live_sample_and_views() -> None:
    service = unique_service("clean-exit")
    probe = REPO_ROOT / "python" / "tests" / "exit_probe.py"
    with background_process(fixture_binary(), service, 64, 4, 192, 1, 0):
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

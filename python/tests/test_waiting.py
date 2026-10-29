from __future__ import annotations

import threading

import rbt

from conftest import REPO_ROOT, background_process, unique_service


def test_waitset_receive_releases_gil() -> None:
    service = unique_service("gil")
    provider = REPO_ROOT / "target" / "release" / "phase4-interop"
    with background_process(provider, "image-provider-hold", service, 2500):
        subscriber = rbt.ImageSubscriber.open(rbt.StreamDescriptor.image(service))
        stop = threading.Event()
        started = threading.Event()
        counter = 0

        def worker() -> None:
            nonlocal counter
            started.set()
            while not stop.is_set():
                counter += 1

        thread = threading.Thread(target=worker)
        thread.start()
        started.wait(timeout=2)
        try:
            assert subscriber.receive(timeout=1.0) is None
        finally:
            stop.set()
            thread.join(timeout=2)
        assert counter > 1_000
        assert subscriber.wait_stats.waits >= 1
        print(
            "GIL_EVIDENCE",
            f"worker_counter={counter}",
            f"waits={subscriber.wait_stats.waits}",
            f"wakeups={subscriber.wait_stats.wakeups}",
        )

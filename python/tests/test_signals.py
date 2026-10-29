from __future__ import annotations

import os
import signal
import subprocess
import sys
import time

import pytest

from conftest import REPO_ROOT, background_process, unique_service


def _wait_until_blocking(process: subprocess.Popen[str]) -> str:
    assert process.stdout is not None
    lines: list[str] = []
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        line = process.stdout.readline()
        if not line:
            break
        lines.append(line)
        if line.strip() == "BLOCKING":
            return "".join(lines)
    pytest.fail(f"signal probe did not enter blocking receive:\n{''.join(lines)}")


@pytest.mark.full
def test_sigint_interrupts_unbounded_wait_with_bounded_latency() -> None:
    service = unique_service("sigint")
    provider = REPO_ROOT / "target" / "release" / "phase4-interop"
    probe = REPO_ROOT / "python" / "tests" / "signal_probe.py"
    with background_process(provider, "image-provider-hold", service, 3000):
        process = subprocess.Popen(
            [sys.executable, str(probe), service],
            cwd="/tmp",
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            prefix = _wait_until_blocking(process)
            started = time.monotonic()
            os.kill(process.pid, signal.SIGINT)
            output, _ = process.communicate(timeout=5)
            latency = time.monotonic() - started
            assert process.returncode == 0, prefix + output
            assert "KEYBOARD_INTERRUPT" in output
            assert latency < 1.0
            print("SIGNAL_EVIDENCE", f"sigint_latency_seconds={latency:.6f}")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)


@pytest.mark.full
def test_sigterm_does_not_leave_blocked_process_hanging() -> None:
    service = unique_service("sigterm")
    provider = REPO_ROOT / "target" / "release" / "phase4-interop"
    probe = REPO_ROOT / "python" / "tests" / "signal_probe.py"
    with background_process(provider, "image-provider-hold", service, 3000):
        process = subprocess.Popen(
            [sys.executable, str(probe), service],
            cwd="/tmp",
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        try:
            _wait_until_blocking(process)
            os.kill(process.pid, signal.SIGTERM)
            process.wait(timeout=5)
            assert process.returncode == -signal.SIGTERM
        finally:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)

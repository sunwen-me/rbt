from __future__ import annotations

import contextlib
import os
import subprocess
import tempfile
import time
import uuid
from pathlib import Path
from typing import Iterator

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]


def unique_service(label: str) -> str:
    return f"rbt/python/{label}-{uuid.uuid4().hex}"


def fixture_binary() -> Path:
    return Path(
        os.environ.get(
            "RBT_PYTHON_IMAGE_FIXTURE",
            REPO_ROOT / "target" / "release" / "image_fixture",
        )
    )


def cpp_interop_binary() -> Path:
    configured = os.environ.get("RBT_CPP_INTEROP")
    if configured:
        return Path(configured)
    candidates = (
        REPO_ROOT / "target" / "python-cpp-build" / "interop_cpp",
        REPO_ROOT / "target" / "v02-cpp-readiness" / "interop_cpp",
        REPO_ROOT / "target" / "phase4-cpp-build" / "interop_cpp",
    )
    return next((path for path in candidates if path.is_file()), candidates[0])


@contextlib.contextmanager
def background_process(*args: object) -> Iterator[subprocess.Popen[str]]:
    log = tempfile.TemporaryFile(mode="w+", encoding="utf-8")
    process = subprocess.Popen(
        [str(argument) for argument in args],
        cwd="/tmp",
        stdin=subprocess.DEVNULL,
        stdout=log,
        stderr=subprocess.STDOUT,
        text=True,
    )
    try:
        time.sleep(0.15)
        if process.poll() is not None:
            log.seek(0)
            pytest.fail(f"background process exited early ({process.returncode}):\n{log.read()}")
        yield process
        try:
            return_code = process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            log.seek(0)
            pytest.fail(f"background process timed out:\n{log.read()}")
        if return_code != 0:
            log.seek(0)
            pytest.fail(f"background process failed ({return_code}):\n{log.read()}")
    finally:
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
        log.close()

#!/usr/bin/env python3
"""Focused SOFTWARE_FIXTURE checks. Never opens an Orbbec device."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--composer", type=Path, required=True)
    parser.add_argument("--python", type=Path, default=Path(sys.executable))
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    build, composer, python = args.build_dir.resolve(), args.composer.resolve(), args.python.absolute()
    driver = Path(__file__).resolve().parents[1]
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=False)
    result = {"status": "FAIL", "mode": "SOFTWARE_FIXTURE", "hardware": "NOT RUN", "checks": []}
    processes: list[subprocess.Popen] = []
    handles = []
    env = os.environ.copy()
    env["PYTHONPATH"] = str(driver / "python")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        env["RBT_COMPOSER_ADDR"] = f"127.0.0.1:{sock.getsockname()[1]}"

    def run(name: str, command: list[str], timeout=15) -> str:
        started = time.monotonic()
        completed = subprocess.run(command, env=env, capture_output=True, text=True, timeout=timeout)
        (output / f"{name}.log").write_text(completed.stdout + completed.stderr)
        if completed.returncode:
            raise RuntimeError(f"{name} exited {completed.returncode}: {completed.stderr[-2000:]}")
        result["checks"].append({"name": name, "status": "PASS", "seconds": round(time.monotonic()-started, 3)})
        return completed.stdout

    def start(name: str, command: list[str]):
        handle = (output / f"{name}.log").open("w")
        handles.append(handle)
        child = subprocess.Popen(command, env=env, stdout=handle, stderr=subprocess.STDOUT, start_new_session=True)
        processes.append(child)
        return child

    try:
        run("cpp_core_and_component", ["ctest", "--test-dir", str(build), "--output-on-failure"])
        run("python_codec", [str(python), "-m", "unittest", "discover", "-s", str(driver / "tests"), "-p", "test_*.py", "-v"])
        start("composer", [str(composer)])
        provider = start("fixture_provider", [str(build / "rbt_orbbec_component_test"), "--serve-software-fixture"])
        activation = None
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and provider.poll() is None:
            for line in (output / "fixture_provider.log").read_text().splitlines():
                try:
                    value = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if value.get("event") == "activation":
                    activation = value
                    break
            if activation:
                break
            time.sleep(.02)
        if not activation:
            raise RuntimeError("fixture provider did not activate")
        service = next(stream["service"] for stream in activation["streams"] if stream["kind"] == "depth")
        cpp = run("cpp_composer_consumer_tf", [str(build / "rbt-orbbec-consumer"), "--stream", "color",
            "--count", "4", "--timeout-sec", "5", "--tf-target", "orbbec335/depth_optical", "--allow-unknown-uncertainty"])
        frames = [json.loads(line) for line in cpp.splitlines() if line.startswith('{')]
        frames = [frame for frame in frames if frame.get("event") == "frame"]
        if len(frames) != 4 or any(frame.get("translation_m") != [.1, .2, .3] for frame in frames):
            raise RuntimeError("C++ consumer did not recover expected fixture TF snapshot")
        saved = output / "depth.npy"
        data = run("python_native_consumer", [str(python), str(driver / "python/consume.py"), "--service", service,
            "--stream", "depth", "--count", "4", "--timeout-sec", "2", "--save-first", str(saved)])
        python_frames = [json.loads(line) for line in data.splitlines() if line.startswith('{')]
        python_frames = [frame for frame in python_frames if frame.get("event") == "frame"]
        if len(python_frames) != 4 or any(frame["capture_clock"] != "device" or frame["depth_unit_m"] != .001
                                         or frame["capture_uncertainty_ns"] is not None for frame in python_frames):
            raise RuntimeError("Python frame timing or depth units differ from fixture")
        run("saved_depth_pixels", [str(python), "-c",
            "import numpy as np,sys; a=np.load(sys.argv[1],allow_pickle=False); assert a.shape==(2,2) and a[0,0]==1000 and a.dtype==np.dtype('<u2')", str(saved)])
        result["status"] = "PASS"
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
    finally:
        for child in reversed(processes):
            if child.poll() is None:
                try:
                    os.killpg(child.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        for child in reversed(processes):
            try:
                child.wait(timeout=3)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                child.wait(timeout=3)
        for handle in handles:
            handle.close()
        result["children_reaped"] = all(child.poll() is not None for child in processes)
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())

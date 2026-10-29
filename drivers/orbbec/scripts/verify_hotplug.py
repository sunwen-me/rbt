#!/usr/bin/env python3
"""Observe a user-operated USB unplug/replug on one explicit Gemini 335."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import time

from verify_processing import (
    _check_live, _composer_address, _json_records, _require_executable,
    _start, _stop_child, _wait_for_activation,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--consumer", type=Path, required=True)
    parser.add_argument("--composer", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--timeout-sec", type=int, default=180)
    args = parser.parse_args()
    if not args.serial.strip() or args.serial != args.serial.strip():
        parser.error("--serial must be an explicit nonempty serial without surrounding whitespace")
    if not 30 <= args.timeout_sec <= 600:
        parser.error("--timeout-sec must be in 30..600")
    driver = _require_executable(args.driver, "--driver")
    consumer = _require_executable(args.consumer, "--consumer")
    composer = _require_executable(args.composer, "--composer")
    output = args.output_dir.absolute()
    output.mkdir(parents=True, exist_ok=False)
    env = os.environ.copy()
    env["RBT_COMPOSER_ADDR"] = _composer_address()
    deadline = time.monotonic() + args.timeout_sec
    processes, logs = {}, {}
    result = {"status": "FAIL", "mode": "REAL_HARDWARE", "serial": args.serial, "logs": logs}
    disconnected = False

    def frames():
        return [v for v in _json_records(output / "consumer.log") if v.get("event") == "frame"]

    try:
        _start("composer", [str(composer)], env=env, output=output, processes=processes, logs=logs)
        _start("camera", [str(driver), "run", "--serial", args.serial,
            "--duration-sec", str(args.timeout_sec), "--align-depth-to-color", "--points", "--imu",
            "--depth-filter", "SpatialAdvancedFilter:magnitude=1,alpha=0.5,disp_diff=160,radius=1",
            "--depth-filter", "TemporalFilter:diff_scale=0.1,weight=0.4",
            "--depth-filter", "HoleFillingFilter:hole_filling_mode=2"],
            env=env, output=output, processes=processes, logs=logs)
        first, _ = _wait_for_activation(output / "camera.log", processes=processes, deadline=deadline)
        _start("consumer", [str(consumer), "--provider", "orbbec335", "--stream", "depth",
            "--count", "80000", "--timeout-sec", str(args.timeout_sec),
            "--tf-target", "orbbec335/depth_optical", "--allow-unknown-uncertainty"],
            env=env, output=output, processes=processes, logs=logs)
        ready = False
        baseline = 0
        second = None
        while time.monotonic() < deadline:
            _check_live(processes, "composer", "camera", "consumer")
            records = _json_records(output / "camera.log")
            received = frames()
            if not ready and len(received) >= 5:
                ready = True
                print(json.dumps({"event": "hotplug_ready", "serial": args.serial,
                    "instruction": "Unplug USB, wait two seconds, then reinsert the same camera."}), flush=True)
            if not disconnected and any(v.get("event") == "disconnected" for v in records):
                disconnected = True
                baseline = len(received)
                print(json.dumps({"event": "unplug_observed"}), flush=True)
            activations = [v for v in records if v.get("event") == "activation"]
            if disconnected and len(activations) >= 2:
                if second is None:
                    second = activations[1]
                    baseline = len(received)
                if len(received) >= baseline + 5:
                    break
            time.sleep(0.05)
        else:
            if not disconnected:
                result["status"] = "NOT_RUN"
                raise TimeoutError("no physical unplug was observed within the user-operation window")
            raise TimeoutError("device disconnected but native consumer did not resume")

        def services(activation):
            return {v["kind"]: v["service"] for v in activation["streams"]}

        if (second is None or first["serial"] != args.serial or second["serial"] != args.serial
                or first["process_instance"] == second["process_instance"]
                or first["sdk_clock_source"] == second["sdk_clock_source"]
                or services(first).keys() != services(second).keys()
                or any(services(first)[kind] == service for kind, service in services(second).items())):
            raise ValueError("replug did not create fresh runtime, device clock, and stream capabilities")
        if any(v.get("frame_id") != "orbbec335/color_optical" or v.get("width") != 640
                or v.get("height") != 480 or v.get("tf_target") != "orbbec335/depth_optical"
                for v in frames()[-5:]):
            raise ValueError("resumed frames did not retain aligned geometry and factory TF")
        result.update(status="PASS", physical_unplug_observed=True,
            consumer_resumed=True, fresh_runtime=True, fresh_clock=True, fresh_services=True,
            resumed_frames=5, activations=[first, second])
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
    finally:
        cleanup = {}
        for name in ("consumer", "camera", "composer"):
            if name in processes:
                cleanup[name] = _stop_child(processes[name], 3, allow_sigterm_exit=name == "composer")
        for child in processes.values():
            child._rbt_log_handle.close()
        result["cleanup"] = cleanup
        if result["status"] == "PASS" and any(not v["expected_exit"] for v in cleanup.values()):
            result.update(status="FAIL", error="owned process did not stop cleanly")
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": result["status"], "result_file": str(output / "result.json"),
        "error": result.get("error")}), flush=True)
    return 0 if result["status"] == "PASS" else 2 if result["status"] == "NOT_RUN" else 1


if __name__ == "__main__":
    raise SystemExit(main())

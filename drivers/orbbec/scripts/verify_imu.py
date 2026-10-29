#!/usr/bin/env python3
"""Run a focused REAL_HARDWARE Gemini 335 IMU and concurrent-stream check."""
from __future__ import annotations

import argparse
import json
import math
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import time
import uuid


_MAX_COUNT = 20_000
_MAX_DURATION_SEC = 180
_ACTIVATION_TIMEOUT_SEC = 15
_PYTHON_SAMPLE_TIMEOUT_SEC = 2
_CPP_TIMEOUT_SEC = 30
_DEPTH_COUNT = 20
_TF_COUNT = 30


def _bounded_int(value: str, *, name: str, minimum: int, maximum: int) -> int:
    try:
        result = int(value, 10)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"{name} must be an integer") from error
    if not minimum <= result <= maximum:
        raise argparse.ArgumentTypeError(f"{name} must be in {minimum}..{maximum}")
    return result


def _count(value: str) -> int:
    return _bounded_int(value, name="count", minimum=1, maximum=_MAX_COUNT)


def _duration(value: str) -> int:
    return _bounded_int(value, name="duration-sec", minimum=45, maximum=_MAX_DURATION_SEC)


def _arguments(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True, help="exact Gemini 335 serial number")
    parser.add_argument("--driver", type=Path, required=True, help="rbt-orbbec-camera executable")
    parser.add_argument("--consumer", type=Path, required=True, help="rbt-orbbec-consumer executable")
    parser.add_argument("--composer", type=Path, required=True, help="Composer executable")
    parser.add_argument("--python", type=Path, required=True, help="Python with RBT bindings and NumPy")
    parser.add_argument("--output-dir", type=Path, required=True, help="new directory for logs and result.json")
    parser.add_argument("--count", type=_count, default=2000, help="samples per IMU check (1..20000; default: 2000)")
    parser.add_argument(
        "--duration-sec",
        type=_duration,
        default=45,
        help="bounded camera runtime (45..180 seconds; default: 45)",
    )
    parser.add_argument(
        "--allow-unknown-uncertainty",
        action="store_true",
        required=True,
        help="explicitly allow TF lookup with unknown capture-time uncertainty",
    )
    args = parser.parse_args(argv)
    if not args.serial or args.serial != args.serial.strip():
        parser.error("--serial must be non-empty and have no surrounding whitespace")
    if args.duration_sec < math.ceil(args.count / 200) + 20:
        parser.error("--duration-sec must allow count/200 + 20 seconds")
    return args


def _json_objects(path: Path) -> list[dict[str, object]]:
    objects: list[dict[str, object]] = []
    try:
        with path.open(encoding="utf-8", errors="replace") as stream:
            for line in stream:
                candidate = line.strip()
                if not candidate.startswith("{"):
                    continue
                try:
                    value = json.loads(candidate)
                except json.JSONDecodeError:
                    continue
                if isinstance(value, dict):
                    objects.append(value)
    except FileNotFoundError:
        pass
    return objects


def _event_records(path: Path, event: str) -> list[dict[str, object]]:
    return [item for item in _json_objects(path) if item.get("event") == event]


def _tail(path: Path, limit: int = 2000) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")[-limit:]
    except OSError:
        return ""


def _require_executable(path: Path, label: str) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise ValueError(f"{label} must be an executable file: {resolved}")
    return resolved


def _require_python(path: Path) -> Path:
    resolved = path.expanduser().absolute()
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise ValueError(f"--python must be an executable file: {resolved}")
    return resolved


def _composer_address() -> str:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return f"127.0.0.1:{sock.getsockname()[1]}"


def _start(
    name: str,
    command: list[str],
    *,
    env: dict[str, str],
    output: Path,
    processes: dict[str, subprocess.Popen[bytes]],
    handles: list[object],
) -> subprocess.Popen[bytes]:
    handle = (output / f"{name}.log").open("wb")
    handles.append(handle)
    try:
        child = subprocess.Popen(
            command,
            env=env,
            stdout=handle,
            stderr=subprocess.STDOUT,
            start_new_session=True,
            cwd=output,
        )
    except Exception:
        handle.close()
        raise
    child._rbt_log_path = output / f"{name}.log"
    processes[name] = child
    return child


def _check_live(processes: dict[str, subprocess.Popen[bytes]], *names: str) -> None:
    for name in names:
        process = processes.get(name)
        if process is None:
            continue
        code = process.poll()
        if code is not None:
            raise RuntimeError(f"{name} exited early with status {code}: {_tail(process._rbt_log_path)}")


def _wait_for_activation(
    camera_log: Path,
    *,
    processes: dict[str, subprocess.Popen[bytes]],
    deadline: float,
) -> tuple[dict[str, object], float]:
    activation_deadline = min(deadline, time.monotonic() + _ACTIVATION_TIMEOUT_SEC)
    while time.monotonic() < activation_deadline:
        _check_live(processes, "composer", "camera")
        events = _event_records(camera_log, "activation")
        if events:
            return events[0], time.monotonic()
        time.sleep(0.05)
    raise TimeoutError("camera activation was not reported within 15 seconds")


def _wait_for_children(
    names: list[str],
    *,
    processes: dict[str, subprocess.Popen[bytes]],
    output: Path,
    deadline: float,
) -> None:
    pending = set(names)
    while pending:
        if time.monotonic() >= deadline:
            raise TimeoutError(f"overall hardware verification deadline expired waiting for {', '.join(sorted(pending))}")
        _check_live(processes, "composer", "camera")
        for name in list(pending):
            process = processes[name]
            code = process.poll()
            if code is None:
                continue
            if code != 0:
                raise RuntimeError(f"{name} exited {code}: {_tail(output / f'{name}.log')}")
            pending.remove(name)
        time.sleep(0.05)


def _wait_until_activation_age(
    activation_time: float,
    *,
    processes: dict[str, subprocess.Popen[bytes]],
    deadline: float,
) -> None:
    minimum_time = activation_time + 5.0
    while time.monotonic() < minimum_time:
        if time.monotonic() >= deadline:
            raise TimeoutError("overall deadline expired before the required five-second activation interval")
        _check_live(processes, "composer", "camera")
        time.sleep(max(0.0, min(0.05, minimum_time - time.monotonic())))


def _summary(path: Path, stream: str) -> dict[str, object]:
    summaries = [item for item in _event_records(path, "summary") if item.get("stream") == stream]
    if len(summaries) != 1:
        raise ValueError(f"expected one {stream} summary in {path.name}, got {len(summaries)}")
    return summaries[0]


def _check_summary(
    summary: dict[str, object],
    *,
    stream: str,
    count: int,
    expected_source_id: str,
    expected_generation: int,
) -> dict[str, object]:
    if summary.get("count") != count:
        raise ValueError(f"{stream} summary count was {summary.get('count')!r}, expected {count}")
    for field in ("first_sequence", "last_sequence", "first_frame_index", "last_frame_index"):
        if not isinstance(summary.get(field), int):
            raise ValueError(f"{stream} summary omitted integer {field}")
    if summary.get("last_sequence") != summary["first_sequence"] + count - 1:
        raise ValueError(f"{stream} sequence range is not contiguous")
    if summary.get("last_frame_index") != summary["first_frame_index"] + count - 1:
        raise ValueError(f"{stream} device-index range is not contiguous")
    if summary.get("sequence_gaps") != 0 or summary.get("device_index_gaps") != 0:
        raise ValueError(f"{stream} reported sequence or device-index gaps")
    if summary.get("capture_source_id") != expected_source_id:
        raise ValueError(f"{stream} capture source id does not match this activation")
    if summary.get("generation") != expected_generation:
        raise ValueError(f"{stream} capture generation does not match this activation")
    first_capture = summary.get("first_capture_ns")
    last_capture = summary.get("last_capture_ns")
    if not isinstance(first_capture, int) or not isinstance(last_capture, int) or last_capture < first_capture:
        raise ValueError(f"{stream} capture timestamps are absent or regressed")
    return {
        "stream": stream,
        "count": count,
        "sequence_contiguous": True,
        "device_index_contiguous": True,
        "capture_identity_matches_activation": True,
        "capture_monotonic": True,
        "first_sequence": summary["first_sequence"],
        "last_sequence": summary["last_sequence"],
        "first_frame_index": summary["first_frame_index"],
        "last_frame_index": summary["last_frame_index"],
        "first_capture_ns": first_capture,
        "last_capture_ns": last_capture,
    }


def _frame_records(path: Path) -> list[dict[str, object]]:
    return _event_records(path, "frame")


def _check_cpp_imu(path: Path, count: int) -> dict[str, object]:
    frames = _frame_records(path)
    if len(frames) != count:
        raise ValueError(f"C++ accel consumer emitted {len(frames)} frames, expected {count}")
    previous_sequence: int | None = None
    previous_index: int | None = None
    previous_capture: int | None = None
    for frame in frames:
        if frame.get("stream") != "accel":
            raise ValueError("C++ IMU consumer emitted a non-accel frame")
        sequence = frame.get("sequence")
        device_index = frame.get("device_index")
        capture_ns = frame.get("capture_ns")
        if not isinstance(sequence, int) or not isinstance(device_index, int):
            raise ValueError("C++ IMU frame omitted integer sequence or device_index")
        if not isinstance(capture_ns, int):
            raise ValueError("C++ IMU frame omitted capture_ns")
        if previous_sequence is not None and sequence != previous_sequence + 1:
            raise ValueError("C++ IMU sequence is not contiguous")
        if previous_index is not None and device_index != previous_index + 1:
            raise ValueError("C++ IMU device index is not contiguous")
        if previous_capture is not None and capture_ns < previous_capture:
            raise ValueError("C++ IMU capture timestamp regressed")
        previous_sequence = sequence
        previous_index = device_index
        previous_capture = capture_ns
    return {
        "stream": "accel",
        "count": count,
        "sequence_contiguous": True,
        "device_index_contiguous": True,
        "capture_monotonic": True,
    }


def _check_depth(path: Path, saved_path: Path, count: int) -> dict[str, object]:
    frames = _frame_records(path)
    if len(frames) != count:
        raise ValueError(f"depth consumer emitted {len(frames)} frames, expected {count}")
    if any(frame.get("stream") != "depth" or frame.get("shape") != [480, 640] for frame in frames):
        raise ValueError("depth frames did not have the expected 640x480 image shape")
    if not saved_path.is_file() or saved_path.stat().st_size == 0:
        raise ValueError("depth consumer did not save its first frame")
    return {"stream": "depth", "count": count, "shape": [480, 640], "saved_first_frame": True}


def _check_tf(path: Path, count: int) -> dict[str, object]:
    frames = _frame_records(path)
    if len(frames) != count:
        raise ValueError(f"C++ color TF consumer emitted {len(frames)} frames, expected {count}")
    target = "orbbec335/depth_optical"
    for frame in frames:
        if frame.get("stream") != "color" or frame.get("tf_target") != target:
            raise ValueError("C++ color consumer did not report the requested TF target")
        translation = frame.get("translation_m")
        if (
            not isinstance(translation, list)
            or len(translation) != 3
            or any(not isinstance(value, (int, float)) or not math.isfinite(value) for value in translation)
        ):
            raise ValueError("C++ color consumer omitted a finite TF translation")
    return {"stream": "color", "count": count, "tf_target": target, "tf_lookup": "PASS"}


def _stop_child(process: subprocess.Popen[bytes], timeout: float, *, allow_sigterm: bool = False) -> dict[str, object]:
    sigterm_sent = False
    sigkill_sent = False
    if process.poll() is None:
        try:
            os.killpg(process.pid, signal.SIGTERM)
            sigterm_sent = True
        except ProcessLookupError:
            pass
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(process.pid, signal.SIGKILL)
                sigkill_sent = True
            except ProcessLookupError:
                pass
            process.wait(timeout=3)
    return {
        "reaped": process.poll() is not None,
        "returncode": process.returncode,
        "sigterm_sent": sigterm_sent,
        "sigkill_sent": sigkill_sent,
        "graceful_exit": process.returncode == 0 and not sigkill_sent,
        "expected_exit": not sigkill_sent and (process.returncode == 0 or (allow_sigterm and sigterm_sent and process.returncode == -signal.SIGTERM)),
    }


def main(argv: list[str] | None = None) -> int:
    args = _arguments(argv)
    driver = _require_executable(args.driver, "--driver")
    consumer = _require_executable(args.consumer, "--consumer")
    composer = _require_executable(args.composer, "--composer")
    python = _require_python(args.python)
    output = args.output_dir.expanduser().resolve()
    output.mkdir(parents=True, exist_ok=False)

    result: dict[str, object] = {
        "status": "FAIL",
        "mode": "REAL_HARDWARE",
        "serial": args.serial,
        "requested_count": args.count,
        "counts": {},
        "clock_checks": {},
        "resources_cleanup": {},
        "logs": {},
    }
    processes: dict[str, subprocess.Popen[bytes]] = {}
    handles: list[object] = []
    test_succeeded = False
    env = os.environ.copy()
    driver_python = Path(__file__).resolve().parents[1] / "python"
    prior_pythonpath = env.get("PYTHONPATH")
    env["PYTHONPATH"] = str(driver_python) + (os.pathsep + prior_pythonpath if prior_pythonpath else "")
    env["RBT_COMPOSER_ADDR"] = _composer_address()
    logs = result["logs"]
    assert isinstance(logs, dict)
    camera_started = time.monotonic()
    deadline = camera_started + args.duration_sec - 1

    try:
        _start("composer", [str(composer)], env=env, output=output, processes=processes, handles=handles)
        time.sleep(0.2)
        _check_live(processes, "composer")
        camera = _start(
            "camera",
            [
                str(driver),
                "run",
                "--serial",
                args.serial,
                "--duration-sec",
                str(args.duration_sec),
                "--imu",
                "--points",
                "--left-ir",
                "--right-ir",
            ],
            env=env,
            output=output,
            processes=processes,
            handles=handles,
        )
        del camera
        logs.update({name: f"{name}.log" for name in ("composer", "camera")})

        activation, activation_time = _wait_for_activation(
            output / "camera.log", processes=processes, deadline=deadline
        )
        if activation.get("serial") != args.serial:
            raise ValueError("camera activation serial did not match the requested serial")
        source_uuid = uuid.UUID(str(activation.get("sdk_clock_source")))
        expected_source_id = source_uuid.hex + "0" * 64
        generation = 0
        streams_raw = activation.get("streams")
        if not isinstance(streams_raw, list):
            raise ValueError("camera activation omitted its stream service list")
        services = {
            item.get("kind"): item.get("service")
            for item in streams_raw
            if isinstance(item, dict) and isinstance(item.get("kind"), str)
        }
        required_streams = {"accel", "gyro", "depth", "color", "points", "left_ir", "right_ir"}
        if not required_streams.issubset(services):
            raise ValueError(f"camera activation omitted required streams: {sorted(required_streams - services.keys())}")
        result["clock_checks"] = {
            "expected_capture_source_id": expected_source_id,
            "expected_generation": generation,
            "activation_reported": True,
        }

        python_script = str(driver_python / "consume.py")
        accel_python = _start(
            "python_accel",
            [str(python), python_script, "--service", str(services["accel"]), "--stream", "accel",
             "--count", str(args.count), "--timeout-sec", str(_PYTHON_SAMPLE_TIMEOUT_SEC), "--summary"],
            env=env,
            output=output,
            processes=processes,
            handles=handles,
        )
        gyro_python = _start(
            "python_gyro",
            [str(python), python_script, "--service", str(services["gyro"]), "--stream", "gyro",
             "--count", str(args.count), "--timeout-sec", str(_PYTHON_SAMPLE_TIMEOUT_SEC), "--summary"],
            env=env,
            output=output,
            processes=processes,
            handles=handles,
        )
        cpp_accel = _start(
            "cpp_accel",
            [str(consumer), "--provider", "orbbec335", "--stream", "accel", "--count", str(args.count),
             "--timeout-sec", str(max(_CPP_TIMEOUT_SEC, math.ceil(args.count / 200) + 10))],
            env=env,
            output=output,
            processes=processes,
            handles=handles,
        )
        depth_saved = output / "depth-first.npy"
        depth_python = _start(
            "python_depth",
            [str(python), python_script, "--service", str(services["depth"]), "--stream", "depth",
             "--count", str(_DEPTH_COUNT), "--timeout-sec", str(_PYTHON_SAMPLE_TIMEOUT_SEC),
             "--save-first", str(depth_saved)],
            env=env,
            output=output,
            processes=processes,
            handles=handles,
        )
        del accel_python, gyro_python, cpp_accel, depth_python
        logs.update({
            name: f"{name}.log"
            for name in ("python_accel", "python_gyro", "cpp_accel", "python_depth")
        })
        _wait_for_children(
            ["python_accel", "python_gyro", "cpp_accel", "python_depth"],
            processes=processes,
            output=output,
            deadline=deadline,
        )

        accel_summary = _check_summary(
            _summary(output / "python_accel.log", "accel"), stream="accel", count=args.count,
            expected_source_id=expected_source_id, expected_generation=generation,
        )
        gyro_summary = _check_summary(
            _summary(output / "python_gyro.log", "gyro"), stream="gyro", count=args.count,
            expected_source_id=expected_source_id, expected_generation=generation,
        )
        cpp_imu_check = _check_cpp_imu(output / "cpp_accel.log", args.count)
        depth_check = _check_depth(output / "python_depth.log", depth_saved, _DEPTH_COUNT)
        result["counts"] = {
            "python_accel": args.count,
            "python_gyro": args.count,
            "cpp_accel": args.count,
            "depth": _DEPTH_COUNT,
        }
        clock_checks = result["clock_checks"]
        assert isinstance(clock_checks, dict)
        clock_checks.update({"python_accel": accel_summary, "python_gyro": gyro_summary, "cpp_accel": cpp_imu_check})

        _wait_until_activation_age(activation_time, processes=processes, deadline=deadline)
        tf_command = [
            str(consumer),
            "--provider",
            "orbbec335",
            "--stream",
            "color",
            "--count",
            str(_TF_COUNT),
            "--timeout-sec",
            str(max(_CPP_TIMEOUT_SEC, math.ceil(args.count / 200) + 10)),
            "--tf-target",
            "orbbec335/depth_optical",
        ]
        if args.allow_unknown_uncertainty:
            tf_command.append("--allow-unknown-uncertainty")
        _start("cpp_color_tf", tf_command, env=env, output=output, processes=processes, handles=handles)
        logs["cpp_color_tf"] = "cpp_color_tf.log"
        _wait_for_children(
            ["cpp_color_tf"], processes=processes, output=output, deadline=deadline
        )
        tf_check = _check_tf(output / "cpp_color_tf.log", _TF_COUNT)
        counts = result["counts"]
        assert isinstance(counts, dict)
        counts["color_tf"] = _TF_COUNT
        clock_checks["color_tf"] = tf_check
        _check_live(processes, "camera", "composer")
        test_succeeded = True
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
    finally:
        cleanup: dict[str, object] = {}
        # Stop only process groups created by this verifier, then reap every child.
        for name, process in reversed(list(processes.items())):
            if name in ("camera", "composer"):
                continue
            cleanup[name] = _stop_child(process, 3)
        for name in ("camera", "composer"):
            process = processes.get(name)
            if process is not None:
                cleanup[name] = _stop_child(process, 10, allow_sigterm=name == "composer")
        for handle in handles:
            handle.close()  # type: ignore[attr-defined]
        result["resources_cleanup"] = {
            "processes": cleanup,
            "all_reaped": all(item.get("reaped") is True for item in cleanup.values()),
            "camera_graceful": cleanup.get("camera", {}).get("graceful_exit", False)
            if isinstance(cleanup.get("camera"), dict)
            else False,
            "composer_expected_exit": cleanup.get("composer", {}).get("expected_exit", False)
            if isinstance(cleanup.get("composer"), dict)
            else False,
        }
        resources = result["resources_cleanup"]
        assert isinstance(resources, dict)
        clean = (
            resources["all_reaped"] is True
            and resources["camera_graceful"] is True
            and resources["composer_expected_exit"] is True
            and all(item.get("sigkill_sent") is False for item in cleanup.values())
        )
        if test_succeeded and clean:
            result["status"] = "PASS"
        elif test_succeeded and not clean:
            result["error"] = "one or more owned processes did not exit as expected or were not reaped"
        (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")

    summary_keys = ("status", "mode", "serial", "counts", "resources_cleanup")
    print(json.dumps({key: result[key] for key in summary_keys}, separators=(",", ":")))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())

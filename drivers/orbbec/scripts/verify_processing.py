#!/usr/bin/env python3
"""Run bounded REAL_HARDWARE Gemini 335 processing and native-stream checks."""
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


_OVERALL_TIMEOUT_SEC = 90.0
_FILTER_TIMEOUT_SEC = 8.0
_PROBE_TIMEOUT_SEC = 12.0
_ACTIVATION_TIMEOUT_SEC = 15.0
_SAMPLE_COUNT = 30
_IMU_COUNT = 1000
_IMU_TIMEOUT_SEC = 3.0
_VIDEO_TIMEOUT_SEC = 5.0
_CPP_TIMEOUT_SEC = 30
_TF_TARGET = "orbbec335/color_optical"


def _arguments(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True, help="exact Gemini 335 serial number")
    parser.add_argument("--probe", type=Path, required=True, help="direct SDK hardware processing probe")
    parser.add_argument("--driver", type=Path, required=True, help="rbt-orbbec-camera executable")
    parser.add_argument("--consumer", type=Path, required=True, help="rbt-orbbec-consumer executable")
    parser.add_argument("--composer", type=Path, required=True, help="Composer executable")
    parser.add_argument("--python", type=Path, required=True, help="absolute Python with RBT bindings and NumPy")
    parser.add_argument("--output-dir", type=Path, required=True, help="new directory for logs and result.json")
    args = parser.parse_args(argv)
    if not args.serial or args.serial != args.serial.strip():
        parser.error("--serial must be non-empty and have no surrounding whitespace")
    if not args.python.is_absolute():
        parser.error("--python must be an absolute path")
    return args


def _require_executable(path: Path, label: str) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise ValueError(f"{label} must be an executable file: {resolved}")
    return resolved


def _require_python(path: Path) -> Path:
    absolute = path.expanduser().absolute()
    if not absolute.is_file() or not os.access(absolute, os.X_OK):
        raise ValueError(f"--python must be an executable file: {absolute}")
    return absolute


def _remaining(deadline: float, limit: float, label: str) -> float:
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError(f"overall 90-second deadline expired before {label}")
    return min(remaining, limit)


def _composer_address() -> str:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return f"127.0.0.1:{sock.getsockname()[1]}"


def _json_records(path: Path) -> list[dict[str, object]]:
    records: list[dict[str, object]] = []
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
                    records.append(value)
    except FileNotFoundError:
        pass
    return records


def _tail(path: Path, limit: int = 3000) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")[-limit:]
    except OSError:
        return ""


def _run_saved(
    name: str,
    command: list[str],
    *,
    env: dict[str, str],
    output: Path,
    deadline: float,
    timeout: float,
    logs: dict[str, str],
) -> subprocess.CompletedProcess[bytes]:
    log_path = output / f"{name}.log"
    logs[name] = log_path.name
    try:
        with log_path.open("wb") as log:
            completed = subprocess.run(
                command,
                env=env,
                cwd=output,
                stdout=log,
                stderr=subprocess.STDOUT,
                timeout=_remaining(deadline, timeout, name),
                check=False,
            )
    except subprocess.TimeoutExpired as error:
        raise TimeoutError(f"{name} exceeded its bounded runtime: {_tail(log_path)}") from error
    if completed.returncode != 0:
        raise RuntimeError(f"{name} exited {completed.returncode}: {_tail(log_path)}")
    return completed


def _start(
    name: str,
    command: list[str],
    *,
    env: dict[str, str],
    output: Path,
    processes: dict[str, subprocess.Popen[bytes]],
    logs: dict[str, str],
) -> subprocess.Popen[bytes]:
    log_path = output / f"{name}.log"
    handle = log_path.open("wb")
    try:
        child = subprocess.Popen(
            command,
            env=env,
            cwd=output,
            stdout=handle,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
    except Exception:
        handle.close()
        raise
    child._rbt_log_path = log_path  # type: ignore[attr-defined]
    child._rbt_log_handle = handle  # type: ignore[attr-defined]
    processes[name] = child
    logs[name] = log_path.name
    return child


def _check_live(processes: dict[str, subprocess.Popen[bytes]], *names: str) -> None:
    for name in names:
        process = processes.get(name)
        if process is None:
            continue
        code = process.poll()
        if code is not None:
            path = process._rbt_log_path  # type: ignore[attr-defined]
            raise RuntimeError(f"{name} exited early with status {code}: {_tail(path)}")


def _wait_for_activation(
    camera_log: Path,
    *,
    processes: dict[str, subprocess.Popen[bytes]],
    deadline: float,
) -> tuple[dict[str, object], float]:
    activation_deadline = min(deadline, time.monotonic() + _ACTIVATION_TIMEOUT_SEC)
    while time.monotonic() < activation_deadline:
        _check_live(processes, "composer", "camera")
        activations = [
            record for record in _json_records(camera_log) if record.get("event") == "activation"
        ]
        if activations:
            return activations[0], time.monotonic()
        time.sleep(0.05)
    raise TimeoutError("camera activation was not reported within 15 seconds")


def _wait_activation_age(
    activation_time: float,
    *,
    processes: dict[str, subprocess.Popen[bytes]],
    deadline: float,
) -> None:
    minimum_time = activation_time + 5.0
    while time.monotonic() < minimum_time:
        if time.monotonic() >= deadline:
            raise TimeoutError("overall deadline expired before the five-second late join")
        _check_live(processes, "composer", "camera")
        time.sleep(min(0.05, max(0.0, minimum_time - time.monotonic())))


def _wait_children(
    names: list[str],
    *,
    processes: dict[str, subprocess.Popen[bytes]],
    deadline: float,
) -> None:
    pending = set(names)
    while pending:
        _remaining(deadline, 1.0, f"waiting for {', '.join(sorted(pending))}")
        _check_live(processes, "composer", "camera")
        for name in list(pending):
            process = processes[name]
            code = process.poll()
            if code is None:
                continue
            if code != 0:
                path = process._rbt_log_path  # type: ignore[attr-defined]
                raise RuntimeError(f"{name} exited {code}: {_tail(path)}")
            pending.remove(name)
        if pending:
            time.sleep(0.05)


def _check_filter_schema(value: dict[str, object], serial: str) -> dict[str, object]:
    if value.get("serial") != serial:
        raise ValueError("filters response serial did not match the explicit device serial")
    filters = value.get("filters")
    if not isinstance(filters, list):
        raise ValueError("filters response omitted the SDK filter schema list")
    by_name: dict[str, dict[str, object]] = {}
    for entry in filters:
        if isinstance(entry, dict) and isinstance(entry.get("name"), str):
            name = entry["name"]
            if name in by_name:
                raise ValueError(f"filters response duplicated {name}")
            by_name[name] = entry

    requested = {
        "DecimationFilter": {"decimate": 2.0},
        "ThresholdFilter": {"min": 200.0, "max": 4000.0},
    }
    checked: dict[str, object] = {}
    for filter_name, parameters in requested.items():
        entry = by_name.get(filter_name)
        if entry is None or not isinstance(entry.get("parameters"), list):
            raise ValueError(f"SDK filter schema does not expose required {filter_name}")
        schemas = {
            item.get("name"): item
            for item in entry["parameters"]
            if isinstance(item, dict) and isinstance(item.get("name"), str)
        }
        for parameter_name, requested_value in parameters.items():
            schema = schemas.get(parameter_name)
            if schema is None:
                raise ValueError(f"{filter_name} schema does not expose {parameter_name}")
            minimum = schema.get("min")
            maximum = schema.get("max")
            step = schema.get("step")
            if not all(isinstance(number, (int, float)) and math.isfinite(number)
                       for number in (minimum, maximum, step)):
                raise ValueError(f"{filter_name}.{parameter_name} schema has invalid bounds")
            if requested_value < minimum or requested_value > maximum:
                raise ValueError(
                    f"{filter_name}.{parameter_name}={requested_value:g} is outside SDK bounds "
                    f"{minimum:g}..{maximum:g}"
                )
            if step > 0.0 and not math.isclose(
                (requested_value - minimum) / step,
                round((requested_value - minimum) / step),
                rel_tol=0.0,
                abs_tol=1e-8,
            ):
                raise ValueError(f"{filter_name}.{parameter_name}={requested_value:g} violates SDK step {step:g}")
        checked[filter_name] = {"enabled": entry.get("enabled"), "parameters": parameters}
    return checked


def _check_probe(path: Path, *, mode: str, serial: str) -> dict[str, object]:
    try:
        result = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"{mode} direct probe result is not valid JSON") from error
    if not isinstance(result, dict):
        raise ValueError(f"{mode} direct probe did not emit a JSON object")
    if result.get("status") != "PASS" or result.get("mode") != "REAL_HARDWARE":
        raise ValueError(f"{mode} direct hardware probe failed: {result.get('error', result)}")
    if result.get("serial") != serial or result.get("processing_mode") != mode:
        raise ValueError(f"{mode} direct probe result identity did not match the request")
    calibrations = result.get("calibration")
    frames = result.get("frames")
    if not isinstance(calibrations, dict) or not isinstance(frames, dict):
        raise ValueError(f"{mode} direct probe omitted calibration or frame evidence")
    color = calibrations.get("color")
    depth = calibrations.get("depth")
    if not isinstance(color, dict) or not isinstance(depth, dict):
        raise ValueError(f"{mode} direct probe omitted color or depth calibration")
    expected_depth = (320, 240) if mode == "decimate" else (640, 480)
    if (depth.get("width"), depth.get("height")) != expected_depth:
        raise ValueError(f"{mode} direct probe reported unexpected depth calibration dimensions")
    if (color.get("width"), color.get("height")) != (640, 480):
        raise ValueError(f"{mode} direct probe reported unexpected color calibration dimensions")
    for stream in ("depth", "points"):
        evidence = frames.get(stream)
        if not isinstance(evidence, dict) or evidence.get("count", 0) < _SAMPLE_COUNT:
            raise ValueError(f"{mode} direct probe did not collect {_SAMPLE_COUNT} {stream} frames")
    depth_evidence = frames["depth"]
    points_evidence = frames["points"]
    if not isinstance(depth_evidence.get("unit_scale_m"), (int, float)) or depth_evidence["unit_scale_m"] <= 0:
        raise ValueError(f"{mode} direct probe reported an invalid depth scale")
    if depth_evidence.get("nonzero_values", 0) <= 0:
        raise ValueError(f"{mode} direct probe saw no nonzero depth pixels")
    if points_evidence.get("unit_scale_m") != 1.0 or points_evidence.get("finite_nonzero_points", 0) <= 0:
        raise ValueError(f"{mode} direct probe did not validate metric finite points")
    if mode in ("align", "both") and result.get("align_depth_to_color") is not True:
        raise ValueError(f"{mode} direct probe did not report alignment enabled")
    return result


def _activation_services(activation: dict[str, object], serial: str) -> dict[str, str]:
    if activation.get("serial") != serial:
        raise ValueError("camera activation serial did not match the requested serial")
    services_raw = activation.get("streams")
    if not isinstance(services_raw, list):
        raise ValueError("camera activation omitted its stream service list")
    services: dict[str, str] = {}
    for item in services_raw:
        if not isinstance(item, dict):
            continue
        kind, service = item.get("kind"), item.get("service")
        if isinstance(kind, str) and isinstance(service, str) and service:
            if kind in services:
                raise ValueError(f"camera activation duplicated the {kind} stream")
            services[kind] = service
    required = {"color", "depth", "points", "accel", "gyro"}
    missing = required - services.keys()
    if missing:
        raise ValueError(f"camera activation omitted required streams: {sorted(missing)}")
    return services


def _check_video_frames(path: Path, *, stream: str, expected_shape: list[int]) -> dict[str, object]:
    frames = [record for record in _json_records(path) if record.get("event") == "frame"]
    if len(frames) != _SAMPLE_COUNT:
        raise ValueError(f"{stream} consumer emitted {len(frames)} frames, expected {_SAMPLE_COUNT}")
    expected_frame_id = "orbbec335/color_optical"
    previous_capture: int | None = None
    previous_index: int | None = None
    for frame in frames:
        if frame.get("stream") != stream or frame.get("frame_id") != expected_frame_id:
            raise ValueError(f"{stream} frame did not use the aligned color_optical frame id")
        if frame.get("shape") != expected_shape:
            raise ValueError(f"{stream} frame shape did not match 640x480 output")
        capture_ns = frame.get("capture_ns")
        index = frame.get("frame_index")
        if not isinstance(capture_ns, int) or capture_ns <= 0 or not isinstance(index, int) or index <= 0:
            raise ValueError(f"{stream} frame omitted a positive capture timestamp or device index")
        if previous_capture is not None and capture_ns < previous_capture:
            raise ValueError(f"{stream} capture timestamp regressed")
        if previous_index is not None and index < previous_index:
            raise ValueError(f"{stream} device frame index regressed")
        previous_capture = capture_ns
        previous_index = index
        if stream == "depth":
            unit = frame.get("depth_unit_m")
            if not isinstance(unit, (int, float)) or not math.isfinite(unit) or unit <= 0.0:
                raise ValueError("aligned depth frame did not report a positive metre scale")
    return {
        "stream": stream,
        "count": len(frames),
        "shape": expected_shape,
        "frame_id": expected_frame_id,
        "capture_monotonic": True,
        "depth_unit_m_positive": stream == "depth",
        "point_unit_scale_codec_validated": stream == "points",
    }


def _check_imu_summary(path: Path, *, stream: str, source_id: str) -> dict[str, object]:
    summaries = [
        item for item in _json_records(path)
        if item.get("event") == "summary" and item.get("stream") == stream
    ]
    if len(summaries) != 1:
        raise ValueError(f"{stream} consumer emitted {len(summaries)} summary records, expected one")
    summary = summaries[0]
    if summary.get("count") != _IMU_COUNT:
        raise ValueError(f"{stream} summary count was {summary.get('count')!r}, expected {_IMU_COUNT}")
    if summary.get("sequence_gaps") != 0 or summary.get("device_index_gaps") != 0:
        raise ValueError(f"{stream} summary reported IMU gaps")
    if summary.get("capture_source_id") != source_id or summary.get("generation") != 0:
        raise ValueError(f"{stream} capture clock did not match this activation")
    first_ns, last_ns = summary.get("first_capture_ns"), summary.get("last_capture_ns")
    if not isinstance(first_ns, int) or not isinstance(last_ns, int) or first_ns <= 0 or last_ns < first_ns:
        raise ValueError(f"{stream} capture timestamps are absent or regressed")
    first_sequence, last_sequence = summary.get("first_sequence"), summary.get("last_sequence")
    first_index, last_index = summary.get("first_frame_index"), summary.get("last_frame_index")
    if not all(isinstance(value, int) for value in (first_sequence, last_sequence, first_index, last_index)):
        raise ValueError(f"{stream} summary omitted sequence or device-index boundaries")
    if last_sequence != first_sequence + _IMU_COUNT - 1 or last_index != first_index + _IMU_COUNT - 1:
        raise ValueError(f"{stream} IMU sequence or SDK index range was not contiguous")
    return {
        "stream": stream,
        "count": _IMU_COUNT,
        "sequence_gaps": 0,
        "device_index_gaps": 0,
        "capture_source_id": source_id,
        "generation": 0,
        "capture_monotonic": True,
    }


def _check_cpp_depth(path: Path) -> dict[str, object]:
    frames = [record for record in _json_records(path) if record.get("event") == "frame"]
    if len(frames) != _SAMPLE_COUNT:
        raise ValueError(f"C++ Composer consumer emitted {len(frames)} depth frames, expected {_SAMPLE_COUNT}")
    previous_capture: int | None = None
    previous_index: int | None = None
    for frame in frames:
        if frame.get("stream") != "depth" or frame.get("frame_id") != _TF_TARGET:
            raise ValueError("C++ Composer consumer did not receive aligned depth and query the color TF target")
        if (frame.get("width"), frame.get("height")) != (640, 480):
            raise ValueError("C++ Composer consumer received a depth frame other than 640x480")
        unit = frame.get("unit_scale")
        capture_ns = frame.get("capture_ns")
        index = frame.get("device_index")
        if not isinstance(unit, (int, float)) or not math.isfinite(unit) or unit <= 0.0:
            raise ValueError("C++ Composer consumer received depth without a positive unit scale")
        if not isinstance(capture_ns, int) or capture_ns <= 0 or not isinstance(index, int) or index <= 0:
            raise ValueError("C++ Composer consumer frame omitted a positive index or capture timestamp")
        if previous_capture is not None and capture_ns < previous_capture:
            raise ValueError("C++ Composer consumer depth capture timestamp regressed")
        if previous_index is not None and index < previous_index:
            raise ValueError("C++ Composer consumer depth device index regressed")
        previous_capture = capture_ns
        previous_index = index
        translation = frame.get("translation_m")
        if (
            frame.get("tf_target") != _TF_TARGET
            or not isinstance(translation, list)
            or len(translation) != 3
            or any(not isinstance(value, (int, float)) or not math.isfinite(value) for value in translation)
        ):
            raise ValueError("C++ Composer consumer did not return the requested factory TF lookup")
    return {"stream": "depth", "count": _SAMPLE_COUNT, "frame_id": _TF_TARGET, "tf_target": _TF_TARGET}


def _stop_child(
    process: subprocess.Popen[bytes], timeout: float, *, allow_sigterm_exit: bool = False
) -> dict[str, object]:
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
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                pass
    code = process.poll()
    graceful = code == 0 and not sigkill_sent
    expected = graceful or (allow_sigterm_exit and sigterm_sent and code == -signal.SIGTERM and not sigkill_sent)
    return {
        "reaped": code is not None,
        "returncode": code,
        "sigterm_sent": sigterm_sent,
        "sigkill_sent": sigkill_sent,
        "graceful_exit": graceful,
        "expected_exit": expected,
    }


def main(argv: list[str] | None = None) -> int:
    args = _arguments(argv)
    probe = _require_executable(args.probe, "--probe")
    driver = _require_executable(args.driver, "--driver")
    consumer = _require_executable(args.consumer, "--consumer")
    composer = _require_executable(args.composer, "--composer")
    python = _require_python(args.python)
    output = args.output_dir.expanduser().resolve()
    output.mkdir(parents=True, exist_ok=False)
    hard_deadline = time.monotonic() + _OVERALL_TIMEOUT_SEC
    deadline = hard_deadline - 20.0

    result: dict[str, object] = {
        "status": "FAIL",
        "mode": "REAL_HARDWARE",
        "serial": args.serial,
        "requested_count": _SAMPLE_COUNT,
        "imu_count": _IMU_COUNT,
        "direct_probes": {},
        "native_streams": {},
        "logs": {},
        "resources_cleanup": {},
    }
    logs = result["logs"]
    assert isinstance(logs, dict)
    processes: dict[str, subprocess.Popen[bytes]] = {}
    success = False
    env = os.environ.copy()
    driver_python = Path(__file__).resolve().parents[1] / "python"
    prior_pythonpath = env.get("PYTHONPATH")
    env["PYTHONPATH"] = str(driver_python) + (os.pathsep + prior_pythonpath if prior_pythonpath else "")

    try:
        filter_command = [str(driver), "filters", "--serial", args.serial]
        _run_saved(
            "filters",
            filter_command,
            env=env,
            output=output,
            deadline=deadline,
            timeout=_FILTER_TIMEOUT_SEC,
            logs=logs,
        )
        filter_records = _json_records(output / "filters.log")
        if len(filter_records) != 1:
            raise ValueError("driver filters command did not emit one JSON schema object")
        (output / "filters.json").write_text(
            json.dumps(filter_records[0], indent=2, allow_nan=False) + "\n", encoding="utf-8"
        )
        logs["filters_json"] = "filters.json"
        filter_schema = _check_filter_schema(filter_records[0], args.serial)
        result["filter_schema"] = filter_schema

        direct_probes: dict[str, object] = {}
        result["direct_probes"] = direct_probes
        for mode in ("align", "decimate", "both"):
            probe_path = output / f"probe_{mode}.json"
            command = [str(probe), "--serial", args.serial, "--mode", mode, "--count", str(_SAMPLE_COUNT)]
            _run_saved(
                f"probe_{mode}",
                command,
                env=env,
                output=output,
                deadline=deadline,
                timeout=_PROBE_TIMEOUT_SEC,
                logs=logs,
            )
            records = _json_records(output / f"probe_{mode}.log")
            if len(records) != 1:
                raise ValueError(f"{mode} direct probe did not emit one JSON result")
            probe_path.write_text(json.dumps(records[0], indent=2, allow_nan=False) + "\n", encoding="utf-8")
            logs[f"probe_{mode}_json"] = probe_path.name
            direct_probes[mode] = _check_probe(probe_path, mode=mode, serial=args.serial)

        composer_address = _composer_address()
        env["RBT_COMPOSER_ADDR"] = composer_address
        _start("composer", [str(composer)], env=env, output=output, processes=processes, logs=logs)
        time.sleep(min(0.2, _remaining(deadline, 0.2, "starting Composer")))
        _check_live(processes, "composer")
        _start(
            "camera",
            [
                str(driver), "run", "--serial", args.serial, "--duration-sec", "80",
                "--align-depth-to-color",
                "--depth-filter", "DecimationFilter:decimate=2",
                "--depth-filter", "ThresholdFilter:min=200,max=4000",
                "--points", "--imu", "--no-reconnect",
            ],
            env=env,
            output=output,
            processes=processes,
            logs=logs,
        )
        activation, activation_time = _wait_for_activation(
            output / "camera.log", processes=processes, deadline=deadline
        )
        services = _activation_services(activation, args.serial)
        clock_text = activation.get("sdk_clock_source")
        source_id = uuid.UUID(str(clock_text)).hex + "0" * 64
        result["activation"] = {
            "serial": args.serial,
            "activation_id": activation.get("activation_id"),
            "sdk_clock_source": clock_text,
            "required_services": {kind: services[kind] for kind in sorted(services)},
        }

        python_script = str(driver_python / "consume.py")
        python_consumers: dict[str, list[str]] = {
            "python_color": [str(python), python_script, "--service", services["color"], "--stream", "color",
                             "--count", str(_SAMPLE_COUNT), "--timeout-sec", str(_VIDEO_TIMEOUT_SEC)],
            "python_depth": [str(python), python_script, "--service", services["depth"], "--stream", "depth",
                             "--count", str(_SAMPLE_COUNT), "--timeout-sec", str(_VIDEO_TIMEOUT_SEC)],
            "python_points": [str(python), python_script, "--service", services["points"], "--stream", "points",
                              "--count", str(_SAMPLE_COUNT), "--timeout-sec", str(_VIDEO_TIMEOUT_SEC)],
            "python_accel": [str(python), python_script, "--service", services["accel"], "--stream", "accel",
                             "--count", str(_IMU_COUNT), "--timeout-sec", str(_IMU_TIMEOUT_SEC), "--summary"],
            "python_gyro": [str(python), python_script, "--service", services["gyro"], "--stream", "gyro",
                            "--count", str(_IMU_COUNT), "--timeout-sec", str(_IMU_TIMEOUT_SEC), "--summary"],
        }
        for name, command in python_consumers.items():
            _start(name, command, env=env, output=output, processes=processes, logs=logs)

        _wait_activation_age(activation_time, processes=processes, deadline=deadline)
        remaining_for_cpp = math.floor(_remaining(deadline, _CPP_TIMEOUT_SEC, "starting late C++ consumer"))
        cpp_timeout = min(_CPP_TIMEOUT_SEC, max(1, remaining_for_cpp))
        _start(
            "cpp_depth_tf",
            [
                str(consumer), "--provider", "orbbec335", "--stream", "depth", "--count", str(_SAMPLE_COUNT),
                "--timeout-sec", str(cpp_timeout), "--tf-target", _TF_TARGET,
                "--allow-unknown-uncertainty",
            ],
            env=env,
            output=output,
            processes=processes,
            logs=logs,
        )
        _wait_children(list(python_consumers) + ["cpp_depth_tf"], processes=processes, deadline=deadline)

        native_streams = {
            "color": _check_video_frames(output / "python_color.log", stream="color", expected_shape=[480, 640, 3]),
            "depth": _check_video_frames(output / "python_depth.log", stream="depth", expected_shape=[480, 640]),
            "points": _check_video_frames(output / "python_points.log", stream="points", expected_shape=[640 * 480, 3]),
            "accel": _check_imu_summary(output / "python_accel.log", stream="accel", source_id=source_id),
            "gyro": _check_imu_summary(output / "python_gyro.log", stream="gyro", source_id=source_id),
            "cpp_depth_tf": _check_cpp_depth(output / "cpp_depth_tf.log"),
        }
        result["native_streams"] = native_streams
        _check_live(processes, "composer", "camera")
        success = True
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
    finally:
        cleanup: dict[str, object] = {}
        for name, process in reversed(list(processes.items())):
            if name in ("camera", "composer"):
                continue
            cleanup[name] = _stop_child(process, 1)
        for name, allow_sigterm in (("camera", False), ("composer", True)):
            process = processes.get(name)
            if process is not None:
                cleanup[name] = _stop_child(process, 2, allow_sigterm_exit=allow_sigterm)
        for process in processes.values():
            handle = getattr(process, "_rbt_log_handle", None)
            if handle is not None:
                handle.close()
        clean = all(
            isinstance(item, dict) and item.get("reaped") is True and item.get("sigkill_sent") is False
            for item in cleanup.values()
        )
        camera_clean = cleanup.get("camera", {}).get("graceful_exit") is True if isinstance(cleanup.get("camera"), dict) else False
        composer_clean = cleanup.get("composer", {}).get("expected_exit") is True if isinstance(cleanup.get("composer"), dict) else False
        result["resources_cleanup"] = {
            "processes": cleanup,
            "all_reaped": clean,
            "camera_graceful": camera_clean,
            "composer_expected_exit": composer_clean,
        }
        if success and clean and camera_clean and composer_clean:
            result["status"] = "PASS"
        elif success:
            result["error"] = "owned process cleanup did not finish gracefully"
        (output / "result.json").write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")

    summary = {
        "status": result["status"],
        "mode": result["mode"],
        "serial": args.serial,
        "direct_probe_modes": sorted(result.get("direct_probes", {})),
        "native_streams": sorted(result.get("native_streams", {})),
        "resources_cleanup": result["resources_cleanup"],
        "result_file": str(output / "result.json"),
    }
    if result.get("error"):
        summary["error"] = result["error"]
    print(json.dumps(summary, separators=(",", ":"), allow_nan=False))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())

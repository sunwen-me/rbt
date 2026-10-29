#!/usr/bin/env python3
"""Commission live MID360-to-Super-LIO localization without starting motion control."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
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


STARTUP_TIMEOUT_SECONDS = 15.0
SHUTDOWN_TIMEOUT_SECONDS = 10.0


class IntegrationError(RuntimeError):
    pass


def free_tcp_address() -> str:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return f"127.0.0.1:{listener.getsockname()[1]}"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_log(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def wait_for_composer(address: str, process: subprocess.Popen[bytes],
                      deadline: float, log_path: Path) -> None:
    host, port_text = address.rsplit(":", 1)
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise IntegrationError(f"Composer exited with {process.returncode}; see {log_path}")
        try:
            with socket.create_connection((host, int(port_text)), timeout=0.15):
                return
        except OSError:
            time.sleep(0.04)
    raise IntegrationError(f"Composer did not listen at {address}; see {log_path}")


def wait_for_marker(process: subprocess.Popen[bytes], log_path: Path,
                    marker: str, deadline: float) -> None:
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise IntegrationError(
                f"{log_path.name} exited with {process.returncode} before {marker!r}; see {log_path}"
            )
        if marker in read_log(log_path):
            return
        time.sleep(0.04)
    raise IntegrationError(f"timed out waiting for {marker!r}; see {log_path}")


def runnable(path: Path, name: str) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise IntegrationError(f"{name} is not executable: {resolved}")
    return resolved


def config_file(path: Path, name: str) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file():
        raise IntegrationError(f"{name} is not a file: {resolved}")
    return resolved


def write_result(path: Path, record: dict[str, object]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(record, indent=2, ensure_ascii=False) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--composer", type=Path, required=True,
                        help="Composer executable")
    parser.add_argument("--driver", type=Path, required=True,
                        help="native MID360 driver executable")
    parser.add_argument("--lio", type=Path, required=True,
                        help="Super-LIO executable")
    parser.add_argument("--probe", type=Path, required=True,
                        help="native localized LIO source probe executable")
    parser.add_argument("--driver-config", type=Path, required=True,
                        help="MID360 driver configuration file")
    parser.add_argument("--lio-config", type=Path, required=True,
                        help="Super-LIO configuration file")
    parser.add_argument("--output-dir", type=Path, required=True,
                        help="directory for a unique run folder, logs, and JSON evidence")
    parser.add_argument("--duration-sec", type=float, default=20.0,
                        help="localized frame observation window in seconds (default: 20)")
    args = parser.parse_args(argv)
    if not math.isfinite(args.duration_sec) or not 0.1 <= args.duration_sec <= 25.0:
        parser.error("duration-sec must be finite and in [0.1, 25]")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    output_dir = args.output_dir.expanduser().resolve()
    try:
        output_dir.mkdir(parents=True, exist_ok=True)
        run_dir = output_dir / (
            f"localization-{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}-"
            f"{uuid.uuid4().hex[:8]}"
        )
        run_dir.mkdir(parents=False, exist_ok=False)
        log_dir = run_dir / "logs"
        log_dir.mkdir()
    except OSError as error:
        print(f"localization commissioning: cannot create output directory: {error}",
              file=sys.stderr, flush=True)
        return 2

    log_paths = {name: log_dir / f"{name}.log"
                 for name in ("composer", "driver", "super_lio", "probe")}
    probe_output = run_dir / "probe-result.json"
    record: dict[str, object] = {
        "mode": "live sensor-only, no motors",
        "scope": "Live MID360 and Super-LIO localization observation; no navigation, motor, or velocity-guard process is started.",
        "passed": False,
        "run_directory": str(run_dir),
        "evidence_path": str(run_dir / "result.json"),
        "started_at_utc": datetime.now(timezone.utc).isoformat(),
        "duration_sec": args.duration_sec,
        "logs": {name: str(path) for name, path in log_paths.items()},
        "probe_result_path": str(probe_output),
        "input_paths": {
            "composer": str(args.composer.expanduser().resolve()),
            "driver": str(args.driver.expanduser().resolve()),
            "lio": str(args.lio.expanduser().resolve()),
            "probe": str(args.probe.expanduser().resolve()),
            "driver_config": str(args.driver_config.expanduser().resolve()),
            "lio_config": str(args.lio_config.expanduser().resolve()),
        },
    }
    children: list[tuple[str, subprocess.Popen[bytes]]] = []
    signaled: set[str] = set()
    forced_kills: list[str] = []
    started = time.monotonic()

    def add_error(message: str) -> None:
        previous = record.get("error")
        record["error"] = f"{previous}; {message}" if previous else message
        record["passed"] = False

    def start_child(name: str, command: list[str], environment: dict[str, str]
                    ) -> subprocess.Popen[bytes]:
        with log_paths[name].open("wb") as log_file:
            process = subprocess.Popen(command, env=environment, cwd=Path.cwd(),
                                       stdin=subprocess.DEVNULL, stdout=log_file,
                                       stderr=subprocess.STDOUT, close_fds=True)
        children.append((name, process))
        record.setdefault("processes", {})
        record["processes"][name] = {"pid": process.pid, "command": command}
        return process

    try:
        binaries = {
            "composer": runnable(args.composer, "Composer"),
            "driver": runnable(args.driver, "MID360 driver"),
            "lio": runnable(args.lio, "Super-LIO"),
            "probe": runnable(args.probe, "LIO source probe"),
        }
        configs = {
            "driver": config_file(args.driver_config, "MID360 driver config"),
            "lio": config_file(args.lio_config, "Super-LIO config"),
        }
        record["binaries"] = {name: str(path) for name, path in binaries.items()}
        record["configs"] = {
            name: {"path": str(path), "sha256": sha256(path)}
            for name, path in configs.items()
        }

        composer_address = free_tcp_address()
        environment = os.environ.copy()
        environment["RBT_COMPOSER_ADDR"] = composer_address
        record["composer_address"] = composer_address

        composer = start_child("composer", [str(binaries["composer"]),], environment)
        wait_for_composer(composer_address, composer,
                          time.monotonic() + STARTUP_TIMEOUT_SECONDS,
                          log_paths["composer"])

        driver = start_child("driver", [str(binaries["driver"]), "--config",
                                         str(configs["driver"])], environment)
        wait_for_marker(driver, log_paths["driver"], "MID360 UDP ready",
                        time.monotonic() + STARTUP_TIMEOUT_SECONDS)

        lio = start_child("super_lio", [str(binaries["lio"]), "--config",
                                         str(configs["lio"]), "--component-id", "super-lio"],
                          environment)
        wait_for_marker(lio, log_paths["super_lio"], "LIO ACTIVE",
                        time.monotonic() + STARTUP_TIMEOUT_SECONDS)

        if composer.poll() is not None or driver.poll() is not None or lio.poll() is not None:
            raise IntegrationError("Composer, MID360 driver, or Super-LIO exited before the source probe")
        probe = start_child("probe", [
            str(binaries["probe"]), "--expect", "localized",
            "--duration-sec", repr(args.duration_sec), "--output", str(probe_output),
        ], environment)
        wait_for_marker(probe, log_paths["probe"], "LIO_SOURCE_PROBE_READY",
                        time.monotonic() + STARTUP_TIMEOUT_SECONDS)

        probe_deadline = time.monotonic() + args.duration_sec + STARTUP_TIMEOUT_SECONDS
        while probe.poll() is None and time.monotonic() < probe_deadline:
            for name, process in (("Composer", composer), ("MID360 driver", driver),
                                  ("Super-LIO", lio)):
                if process.poll() is not None:
                    raise IntegrationError(f"{name} exited while the localized probe was running")
            time.sleep(0.05)
        probe_status = probe.poll()
        if probe_status is None:
            raise IntegrationError("localized source probe exceeded its observation deadline")
        record["probe_exit_code"] = probe_status
        if probe_output.is_file():
            try:
                probe_record = json.loads(probe_output.read_text(encoding="utf-8"))
            except (OSError, json.JSONDecodeError) as error:
                raise IntegrationError(f"cannot parse native probe result {probe_output}: {error}") from error
            if not isinstance(probe_record, dict):
                raise IntegrationError(f"native probe result is not a JSON object: {probe_output}")
            record["probe"] = probe_record
        else:
            raise IntegrationError(f"native probe did not write {probe_output}; see {log_paths['probe']}")

        probe_record = record["probe"]
        if probe_status != 0:
            raise IntegrationError(
                f"localized source probe exited {probe_status}: {probe_record.get('error', 'see probe result and log')}"
            )
        if (probe_record.get("passed") is not True
                or probe_record.get("expect") != "localized"
                or probe_record.get("provider") != "super-lio"
                or probe_record.get("activated") is not True):
            raise IntegrationError("native probe did not report a passing localized super-lio capability")
        localized_value = probe_record.get("localized_valid_frames")
        age_value = probe_record.get("ending_valid_frame_age_ns")
        if (isinstance(localized_value, bool) or not isinstance(localized_value, int)
                or isinstance(age_value, bool) or not isinstance(age_value, int)):
            raise IntegrationError("native probe omitted localized frame count or ending freshness")
        localized_frames = localized_value
        ending_age_ns = age_value
        if localized_frames < 5:
            raise IntegrationError(f"native probe reported only {localized_frames} localized valid frames")
        if not 0 <= ending_age_ns <= 250_000_000:
            raise IntegrationError(
                f"native probe ending localized frame age is {ending_age_ns} ns; expected 0..250000000 ns"
            )
        if probe_record.get("runtime_status") != 0:
            raise IntegrationError("native probe runtime status was not zero")
        for name, process in (("Composer", composer), ("MID360 driver", driver),
                              ("Super-LIO", lio)):
            if process.poll() is not None:
                raise IntegrationError(f"{name} exited before commissioning completed")
        record["passed"] = True
    except Exception as error:
        add_error(f"{type(error).__name__}: {error}")
        print(f"localization commissioning: {error}", file=sys.stderr, flush=True)
    finally:
        for name, process in reversed(children):
            if process.poll() is None:
                try:
                    process.send_signal(signal.SIGTERM)
                    signaled.add(name)
                except ProcessLookupError:
                    pass
        shutdown_deadline = time.monotonic() + SHUTDOWN_TIMEOUT_SECONDS
        while time.monotonic() < shutdown_deadline:
            if all(process.poll() is not None for _, process in children):
                break
            time.sleep(0.05)
        for name, process in children:
            if process.poll() is None:
                try:
                    process.kill()
                    forced_kills.append(name)
                except ProcessLookupError:
                    pass
        for _, process in children:
            try:
                process.wait(timeout=0.5)
            except subprocess.TimeoutExpired:
                pass

        exit_codes = {name: process.poll() for name, process in children}
        record["child_exit_codes"] = exit_codes
        record["forced_kills"] = forced_kills
        if forced_kills:
            add_error("forced child termination was required: " + ", ".join(forced_kills))
        expected_exit_codes = {"composer": -signal.SIGTERM, "driver": 0,
                               "super_lio": 0}
        for name in signaled:
            # An aborted startup may still be waiting for the probe's ready marker.
            # Its termination is captured below, but only a passing run requires probe=0.
            if name == "probe":
                continue
            expected = expected_exit_codes[name]
            actual = exit_codes.get(name)
            if actual != expected:
                add_error(f"{name} exited with {actual} after SIGTERM; expected {expected}")
        if record.get("passed") is True:
            for name, expected in expected_exit_codes.items():
                if name in exit_codes and exit_codes[name] != expected:
                    add_error(f"{name} exited with {exit_codes[name]}; expected {expected}")
        if "probe" in exit_codes:
            probe_code = exit_codes["probe"]
            record["probe_exit_code"] = probe_code
            if record.get("passed") is True and probe_code != 0:
                add_error(f"source probe exited with {probe_code} after a passing result")
        record["runtime_seconds"] = round(time.monotonic() - started, 3)
        record["finished_at_utc"] = datetime.now(timezone.utc).isoformat()
        write_result(run_dir / "result.json", record)

    print(f"Localization commissioning: {'PASS' if record['passed'] else 'FAIL'}", flush=True)
    print(f"evidence: {run_dir / 'result.json'}", flush=True)
    return 0 if record["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

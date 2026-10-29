#!/usr/bin/env python3
"""Exercise real MID360 UDP, Super-LIO, and the normalized frame capability."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import json
import math
import os
from pathlib import Path
import re
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import uuid


MODE_TIMEOUT_SECONDS = 25.0
SIM_DEVICE_EPOCH_NS = 1_700_000_000_000_000_000
UDP_HEADER = struct.Struct("<BHHHHBBB12sIQ")
IMU_PAYLOAD = struct.Struct("<6f")
POINT_PAYLOAD = struct.Struct("<iiiBB")


class IntegrationError(RuntimeError):
    pass


def runnable(path: Path, name: str) -> Path:
    resolved = path.expanduser().resolve()
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise IntegrationError(f"{name} is not executable: {resolved}")
    return resolved


def free_tcp_address() -> str:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return f"127.0.0.1:{listener.getsockname()[1]}"


def free_udp_port(exclude: set[int]) -> int:
    while True:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.bind(("127.0.0.1", 0))
            port = sock.getsockname()[1]
        if port not in exclude:
            return port


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


def read_log(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


def wait_for_marker(process: subprocess.Popen[bytes], log_path: Path,
                    marker: str, deadline: float) -> None:
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise IntegrationError(
                f"{log_path.name} process exited with {process.returncode} before {marker!r}; "
                f"see {log_path}"
            )
        if marker in read_log(log_path):
            return
        time.sleep(0.04)
    raise IntegrationError(f"timed out waiting for {marker!r}; see {log_path}")


def replace_config(base_config: Path, destination: Path) -> None:
    text = base_config.read_text(encoding="utf-8")
    lines = text.splitlines()
    replacements = {
        "lio.map.save_map": "false",
        "lio.sensor.blind": "0.3",
        "lio.sensor.maxrange": "60.0",
        "lio.sensor.filter_rate": "1",
        "lio.sensor.voxel_fliter_size": "0.3",
        "lio.sensor.gravity_norm": "9.81",
        "lio.hash_map.vox_resolution": "0.3",
        "lio.output.map": "true",
        "lio.output.dense": "true",
        "lio.output.pub_step": "1",
        "lio.ground_robot.planar_constraint": "true",
        "lio.ground_robot.base_z": "0.0",
        "lio.output.base_pose": "true",
        "lio.frames.map": "map",
        "lio.frames.odom": "odom",
        "lio.frames.base": "base_footprint",
        "lio.frames.imu": "imu",
        "lio.sensor.time_offset": "0.0",
        "lio.prior_map_localization.enable": "false",
        "lio.backend.use_localization_factor": "false",
        "lio.backend.drive_map_to_odom": "false",
        "lio.kiss_matcher.enable": "false",
        "rbt.input_namespace": "navtest",
        "rbt.component_id": "super-lio",
        "rbt.navigation_output": "true",
        "rbt.max_output_points": "200000",
    }
    scalar_lines: dict[str, int] = {}
    for index, line in enumerate(lines):
        if not line or line[0].isspace() or line.lstrip().startswith("#"):
            continue
        key, separator, _ = line.partition(":")
        if separator:
            scalar_lines[key.strip()] = index
    for key, value in replacements.items():
        if key in scalar_lines:
            lines[scalar_lines[key]] = f"{key}: {value}"
        else:
            lines.append(f"{key}: {value}")

    def replace_sequence(key: str, values: list[str]) -> None:
        nonlocal lines
        found = next((index for index, line in enumerate(lines)
                      if line.startswith(f"{key}:")), None)
        new_line = f"{key}: [{', '.join(values)}]"
        if found is None:
            lines.append(new_line)
            return
        end = found + 1
        while end < len(lines):
            line = lines[end]
            if line and not line[0].isspace() and re.match(r"^[A-Za-z0-9_.-]+\s*:", line):
                break
            if line and not line.lstrip().startswith("-") and not line.lstrip().startswith("#"):
                break
            end += 1
        lines[found:end] = [new_line]

    replace_sequence("lio.extrinsic.lidar_imu",
                     ["0", "0", "0", "1", "0", "0", "0", "1", "0", "0", "0", "1"])
    replace_sequence("lio.extrinsic.odom_robo", ["0", "0", "0.735", "0", "0", "0"])
    destination.write_text("\n".join(lines) + "\n", encoding="utf-8")


def room_points() -> list[tuple[int, int, int, int, int]]:
    axis = [int(round((index - 10) * 200.0)) for index in range(21)]
    points: list[tuple[int, int, int, int, int]] = []
    for x in axis:
        for y in axis:
            points.append((x, y, -2000, 35, 0))
    for y in axis:
        for z in axis:
            points.append((2000, y, z, 35, 0))
    for x in axis:
        for z in axis:
            points.append((x, 2000, z, 35, 0))
    return points


ROOM_POINTS = room_points()


def udp_header(packet_length: int, count: int, point_kind: int,
               time_type: int, timestamp_ns: int) -> bytes:
    return UDP_HEADER.pack(0, packet_length, 50_000, count, 0, 0, point_kind,
                           time_type, bytes(12), 0, timestamp_ns)


def cloud_packet(time_type: int, timestamp_ns: int) -> bytes:
    payload = b"".join(POINT_PAYLOAD.pack(*point) for point in ROOM_POINTS)
    size = UDP_HEADER.size + len(payload)
    return udp_header(size, len(ROOM_POINTS), 1, time_type, timestamp_ns) + payload


def imu_packet(time_type: int, timestamp_ns: int) -> bytes:
    payload = IMU_PAYLOAD.pack(0.0, 0.0, 0.0, 0.0, 0.0, 1.0)
    size = UDP_HEADER.size + len(payload)
    return udp_header(size, 1, 0, time_type, timestamp_ns) + payload


@dataclass
class UdpSource:
    cloud_socket: socket.socket
    imu_socket: socket.socket
    cloud_port: int
    imu_port: int
    cloud_destination: int
    imu_destination: int
    cloud_packets: int = 0
    imu_packets: int = 0

    @classmethod
    def open(cls) -> UdpSource:
        cloud = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        imu = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            cloud.bind(("127.0.0.1", 0))
            imu.bind(("127.0.0.1", 0))
            cloud_source = cloud.getsockname()[1]
            imu_source = imu.getsockname()[1]
            excluded = {cloud_source, imu_source}
            cloud_destination = free_udp_port(excluded)
            excluded.add(cloud_destination)
            imu_destination = free_udp_port(excluded)
            return cls(cloud, imu, cloud_source, imu_source,
                       cloud_destination, imu_destination)
        except Exception:
            cloud.close()
            imu.close()
            raise

    def close(self) -> None:
        self.cloud_socket.close()
        self.imu_socket.close()

    def send(self, stop: threading.Event, time_type: int) -> None:
        cloud_address = ("127.0.0.1", self.cloud_destination)
        imu_address = ("127.0.0.1", self.imu_destination)
        started_ns = time.monotonic_ns()
        next_imu = time.monotonic()
        next_cloud = next_imu
        imu_datagram = imu_packet(time_type, 0)
        cloud_datagram = cloud_packet(time_type, 0)
        while not stop.is_set():
            now = time.monotonic()
            relative_ns = time.monotonic_ns() - started_ns
            stamp_ns = SIM_DEVICE_EPOCH_NS + relative_ns if time_type != 0 else 0
            self.imu_socket.sendto(imu_packet(time_type, stamp_ns) if time_type else imu_datagram,
                                  imu_address)
            self.imu_packets += 1
            if now >= next_cloud:
                cloud_stamp = SIM_DEVICE_EPOCH_NS + relative_ns if time_type != 0 else 0
                self.cloud_socket.sendto(
                    cloud_packet(time_type, cloud_stamp) if time_type else cloud_datagram,
                    cloud_address,
                )
                self.cloud_packets += 1
                next_cloud = now + 0.05
            next_imu += 0.005
            delay = next_imu - time.monotonic()
            if delay > 0:
                stop.wait(delay)
            else:
                next_imu = time.monotonic()


def terminate(process: subprocess.Popen[bytes]) -> int | None:
    if process.poll() is not None:
        return process.returncode
    try:
        os.killpg(process.pid, signal.SIGTERM)
    except ProcessLookupError:
        pass
    try:
        return process.wait(timeout=1.0)
    except subprocess.TimeoutExpired:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        try:
            return process.wait(timeout=2.0)
        except subprocess.TimeoutExpired as error:
            raise IntegrationError(f"could not stop child process {process.pid}") from error


def check_lio_readiness(driver_log: Path, lio_log: Path) -> None:
    driver = read_log(driver_log)
    lio = read_log(lio_log)
    if "MID360 selected 127.0.0.1" not in driver:
        raise IntegrationError("actual MID360 driver did not select the localhost UDP source")
    if "LIO ACTIVE" not in lio or "KF init done" not in lio or "LIO ODOM sequence=" not in lio:
        raise IntegrationError("Super-LIO did not log activation, KF init, and odometry readiness")
    synchronized = re.findall(r"LIO INPUT clouds=(\d+) imus=(\d+) synchronized=(\d+)", lio)
    if not any(int(clouds) > 0 and int(imus) > 0 and int(sync) > 0
               for clouds, imus, sync in synchronized):
        raise IntegrationError("Super-LIO did not report synchronized synthetic UDP input")


def run_mode(mode: str, time_type: int, options: argparse.Namespace,
             run_dir: Path, environment: dict[str, str]) -> dict[str, object]:
    started = time.monotonic()
    deadline = started + MODE_TIMEOUT_SECONDS
    mode_dir = run_dir / mode
    mode_dir.mkdir(parents=True, exist_ok=False)
    paths = {name: mode_dir / f"{name}.log"
             for name in ("composer", "driver", "super_lio", "probe", "udp_source")}
    config_path = mode_dir / "super_lio.yaml"
    probe_output = mode_dir / "probe.json"
    replace_config(options.config, config_path)
    result: dict[str, object] = {
        "mode": mode,
        "time_type": time_type,
        "passed": False,
        "started_monotonic": started,
        "logs": {name: str(path) for name, path in paths.items()},
        "probe_json": str(probe_output),
        "config": str(config_path),
    }
    children: list[tuple[str, subprocess.Popen[bytes]]] = []
    log_handles = []
    sender_stop = threading.Event()
    sender_thread: threading.Thread | None = None
    udp_source: UdpSource | None = None
    sender_error: list[str] = []
    mode_env = environment.copy()
    composer_address = free_tcp_address()
    mode_env["RBT_COMPOSER_ADDR"] = composer_address

    def start(name: str, command: list[str]) -> subprocess.Popen[bytes]:
        handle = paths[name].open("wb")
        log_handles.append(handle)
        child = subprocess.Popen(command, cwd=options.cwd, env=mode_env,
                                 stdout=handle, stderr=subprocess.STDOUT,
                                 close_fds=True, start_new_session=True)
        children.append((name, child))
        return child

    try:
        composer = start("composer", [str(options.composer)])
        wait_for_composer(composer_address, composer,
                          min(deadline, time.monotonic() + 5.0), paths["composer"])

        udp_source = UdpSource.open()
        driver = start("driver", [
            str(options.driver), "--host-ip", "127.0.0.1", "--lidar-ip", "127.0.0.1",
            "--component-id", "mid360-driver",
            "--pointcloud-port", str(udp_source.cloud_destination),
            "--imu-port", str(udp_source.imu_destination),
            "--pointcloud-source-port", str(udp_source.cloud_port),
            "--imu-source-port", str(udp_source.imu_port),
            "--max-points", "10000", "--capability-namespace", "navtest",
            "--lidar-publish-time-interval", "0.05",
        ])
        wait_for_marker(driver, paths["driver"], "MID360 UDP ready",
                        min(deadline, time.monotonic() + 5.0))

        lio = start("super_lio", [str(options.lio), "--config", str(config_path),
                                   "--component-id", "super-lio"])
        wait_for_marker(lio, paths["super_lio"], "LIO ACTIVE",
                        min(deadline, time.monotonic() + 6.0))

        def send_udp() -> None:
            try:
                assert udp_source is not None
                udp_source.send(sender_stop, time_type)
            except Exception as error:  # surfaced after the sender thread is joined
                sender_error.append(f"{type(error).__name__}: {error}")
                sender_stop.set()

        sender_thread = threading.Thread(target=send_udp, name=f"udp-source-{mode}", daemon=True)
        sender_thread.start()

        duration = options.frames_duration_sec if mode == "frames" else options.none_duration_sec
        readiness_deadline = deadline - duration - 5.0
        if time.monotonic() >= readiness_deadline:
            raise IntegrationError("mode startup exceeded its 25-second run budget")
        while time.monotonic() < readiness_deadline:
            if driver.poll() is not None or lio.poll() is not None:
                raise IntegrationError("driver or Super-LIO exited before source probe startup")
            current_driver = read_log(paths["driver"])
            current_lio = read_log(paths["super_lio"])
            status_rows = re.findall(
                r"LIO INPUT clouds=(\d+) imus=(\d+) synchronized=(\d+)", current_lio)
            has_synchronized_input = any(
                int(clouds) > 0 and int(imus) > 0 and int(sync) > 0
                for clouds, imus, sync in status_rows)
            if ("MID360 selected 127.0.0.1" in current_driver
                    and "KF init done" in current_lio
                    and "LIO ODOM sequence=" in current_lio
                    and has_synchronized_input):
                break
            time.sleep(0.05)
        else:
            raise IntegrationError("timed out waiting for synchronized LIO initialization/output")
        check_lio_readiness(paths["driver"], paths["super_lio"])

        probe = start("probe", [str(options.probe), "--expect", mode,
                                 "--duration-sec", str(duration), "--output", str(probe_output)])
        wait_for_marker(probe, paths["probe"], "LIO_SOURCE_PROBE_READY",
                        min(deadline, time.monotonic() + 4.0))
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise IntegrationError("mode exceeded its 25-second run budget before probe completion")
        try:
            probe_status = probe.wait(timeout=remaining)
        except subprocess.TimeoutExpired as error:
            raise IntegrationError("source probe exceeded the 25-second mode limit") from error
        if probe_status != 0:
            raise IntegrationError(f"source probe exited {probe_status}; see {paths['probe']}")
        if sender_error:
            raise IntegrationError("UDP source failed: " + sender_error[0])
        if udp_source is None or udp_source.cloud_packets < 5 or udp_source.imu_packets < 100:
            raise IntegrationError("synthetic source did not send enough real UDP traffic")
        check_lio_readiness(paths["driver"], paths["super_lio"])
        probe_record = json.loads(probe_output.read_text(encoding="utf-8"))
        if probe_record.get("passed") is not True or probe_record.get("expect") != mode:
            raise IntegrationError(f"source probe evidence failed: {probe_record}")
        if probe_record.get("provider") != "super-lio" or probe_record.get("activated") is not True:
            raise IntegrationError("probe did not subscribe to the actual super-lio capability")
        if mode == "frames" and int(probe_record.get("frames", 0)) < 5:
            raise IntegrationError("source probe received fewer than five normalized frames")
        if mode == "none" and int(probe_record.get("frames", -1)) != 0:
            raise IntegrationError("normalized frame leaked in unsupported device-clock mode")
        result.update({
            "passed": True,
            "probe": probe_record,
            "composer_address": composer_address,
            "udp_ports": {
                "cloud_destination": udp_source.cloud_destination,
                "imu_destination": udp_source.imu_destination,
                "cloud_source": udp_source.cloud_port,
                "imu_source": udp_source.imu_port,
            },
            "udp_packets": {
                "cloud": udp_source.cloud_packets,
                "imu": udp_source.imu_packets,
            },
            "runtime_seconds": round(time.monotonic() - started, 3),
        })
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
        result["runtime_seconds"] = round(time.monotonic() - started, 3)
    finally:
        sender_stop.set()
        if sender_thread is not None:
            sender_thread.join(timeout=1.0)
        if udp_source is not None:
            try:
                udp_source.close()
            except OSError:
                pass
        results = {}
        for name, child in reversed(children):
            results[name] = terminate(child)
        for handle in log_handles:
            handle.close()
        result["child_exit_codes"] = results
        result["children_reaped"] = all(child.poll() is not None for _, child in children)
        if udp_source is not None:
            result["udp_packets"] = {"cloud": udp_source.cloud_packets, "imu": udp_source.imu_packets}
        (mode_dir / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        with (mode_dir / "udp_source.log").open("w", encoding="utf-8") as log:
            log.write(f"mode={mode} time_type={time_type} source=127.0.0.1\n")
            log.write(f"cloud_points_per_packet={len(ROOM_POINTS)}\n")
            if sender_error:
                log.write(f"error={sender_error[0]}\n")
    return result


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--composer", type=Path, required=True)
    parser.add_argument("--driver", type=Path, required=True)
    parser.add_argument("--lio", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True,
                        help="baseline Super-LIO YAML; the runner writes an isolated override")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--frames-duration-sec", type=float, default=8.0)
    parser.add_argument("--none-duration-sec", type=float, default=4.0)
    parser.add_argument("--cwd", type=Path, default=Path.cwd(),
                        help="working directory used to start the supplied executables")
    return parser.parse_args()


def main() -> int:
    options = parse_args()
    try:
        options.composer = runnable(options.composer, "Composer")
        options.driver = runnable(options.driver, "MID360 driver")
        options.lio = runnable(options.lio, "Super-LIO node")
        options.probe = runnable(options.probe, "LIO source probe")
        options.config = options.config.expanduser().resolve()
        options.cwd = options.cwd.expanduser().resolve()
        if not options.config.is_file():
            raise IntegrationError(f"Super-LIO config is not a file: {options.config}")
        for duration in (options.frames_duration_sec, options.none_duration_sec):
            if not math.isfinite(duration) or duration < 0.2 or duration > 18.0:
                raise IntegrationError("probe durations must be in [0.2, 18] seconds")
        options.output_dir = options.output_dir.expanduser().resolve()
        options.output_dir.mkdir(parents=True, exist_ok=True)
        run_dir = options.output_dir / f"source-integration-{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}-{uuid.uuid4().hex[:8]}"
        run_dir.mkdir(parents=True, exist_ok=False)
    except Exception as error:
        print(f"source integration: {type(error).__name__}: {error}", file=sys.stderr)
        return 2

    environment = os.environ.copy()
    records = []
    try:
        records.append(run_mode("frames", 0, options, run_dir, environment))
        records.append(run_mode("none", 1, options, run_dir, environment))
    except Exception as error:
        records.append({"mode": "runner", "passed": False,
                        "error": f"{type(error).__name__}: {error}"})
    passed = len(records) == 2 and all(record.get("passed") is True for record in records)
    summary = {
        "passed": passed,
        "pipeline": "localhost synthetic MID360 UDP -> real driver -> real Super-LIO -> LioNavigationFrameV1",
        "hardware": "NOT USED",
        "mode_timeout_sec": MODE_TIMEOUT_SECONDS,
        "runs": records,
    }
    summary_path = run_dir / "result.json"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    print(f"evidence: {summary_path}")
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())

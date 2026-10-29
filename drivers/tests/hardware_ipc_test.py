#!/usr/bin/env python3
"""Exercise the installed RBT hardware drivers through PTYs and real IPC."""

from __future__ import annotations

import argparse
import errno
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import threading
import time
import tty
import uuid


REPO_ROOT = Path(__file__).resolve().parents[2]
STOP_TIMEOUT_SECONDS = 1.5


class IntegrationError(RuntimeError):
    pass


def executable(build_dir: Path, name: str) -> Path:
    for candidate in (build_dir / name, build_dir / "Release" / name,
                      build_dir / "RelWithDebInfo" / name, build_dir / "Debug" / name):
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
    raise IntegrationError(f"could not find executable {name} under {build_dir}")


def crc8_maxim(data: bytes | bytearray) -> int:
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc >> 1) ^ 0x8C) if crc & 1 else crc >> 1
    return crc & 0xFF


def crc16_dm_imu(data: bytes | bytearray) -> int:
    crc = 0xFFFF
    for byte in data:
        index = ((crc >> 8) ^ byte) & 0xFF
        contribution = index << 8
        for _ in range(8):
            contribution = (((contribution << 1) ^ 0x1021) if contribution & 0x8000
                            else contribution << 1) & 0xFFFF
        crc = ((crc << 1) ^ contribution) & 0xFFFF
    return crc


def make_imu_record(tag: int, values: tuple[float, float, float], tail: int,
                    corrupt_crc: bool = False) -> bytearray:
    record = bytearray(19)
    record[:4] = bytes((0x55, 0xAA, 0x01, tag))
    record[4:16] = struct.pack("<fff", *values)
    checksum = crc16_dm_imu(record[:16]) ^ (1 if corrupt_crc else 0)
    record[16:18] = struct.pack("<H", checksum)
    record[18] = tail
    return record


def make_imu_packet(tail: int, corrupt_crc: bool = False, gyro_z: float = 0.2) -> bytes:
    # The first record's 55 AA 01 01 prefix follows the reference driver. Tags
    # 2 and 3 are synthetic test records, not a vendor protocol claim.
    return bytes(
        make_imu_record(1, (0.0, 0.0, 9.80665), tail, corrupt_crc)
        + make_imu_record(2, (0.0, 0.0, gyro_z), tail, corrupt_crc)
        + make_imu_record(3, (0.0, 0.0, 0.0), tail, corrupt_crc)
    )


def write_all(fd: int, data: bytes) -> None:
    offset = 0
    while offset < len(data):
        offset += os.write(fd, data[offset:])


class Pty:
    def __init__(self) -> None:
        self.master, slave = pty.openpty()
        tty.setraw(slave)
        self.path = os.ttyname(slave)
        os.close(slave)
        flags = fcntl.fcntl(self.master, fcntl.F_GETFL)
        fcntl.fcntl(self.master, fcntl.F_SETFL, flags | os.O_NONBLOCK)

    def close(self) -> None:
        try:
            os.close(self.master)
        except OSError:
            pass


class HatResponder:
    def __init__(self, pty_device: Pty) -> None:
        self.device = pty_device
        self.stop_event = threading.Event()
        self.thread = threading.Thread(target=self._run, name="hat-pty-responder", daemon=True)
        self.lock = threading.Lock()
        self.commands: list[dict[str, int]] = []
        self.error: str | None = None
        self.fragmented = False
        self.thread.start()

    def snapshot(self) -> list[dict[str, int]]:
        with self.lock:
            return list(self.commands)

    @staticmethod
    def _signed_i16(value: bytes) -> int:
        return int.from_bytes(value, "big", signed=True)

    def _feedback(self, command: bytes) -> bytes:
        sequence = int.from_bytes(command[4:6], "big")
        left = self._signed_i16(command[8:10]) if command[3] == 2 else 0
        right = self._signed_i16(command[10:12]) if command[3] == 2 else 0
        feedback = bytearray(32)
        feedback[0:4] = bytes((0xA5, 0x5A, 1, 0x81 if command[3] == 1 else 0x82))
        feedback[4:6] = sequence.to_bytes(2, "big")
        feedback[6] = 0x03
        feedback[7] = 115
        feedback[8] = 1
        feedback[9] = 0x01
        feedback[10] = 2
        feedback[14:16] = (left & 0xFFFF).to_bytes(2, "big")
        feedback[18] = 2
        feedback[19] = 0x01
        feedback[20] = 2
        feedback[24:26] = (right & 0xFFFF).to_bytes(2, "big")
        feedback[31] = crc8_maxim(feedback[:31])
        return bytes(feedback)

    def _handle(self, command: bytes) -> None:
        if command[0:3] != bytes((0xA5, 0x5A, 1)) or command[3] not in (1, 2):
            raise IntegrationError("unexpected frame on the virtual DDSM port")
        if crc8_maxim(command[:19]) != command[19]:
            raise IntegrationError("driver sent a DDSM command with an invalid CRC")
        if command[3] == 1:
            if command[6:8] != b"\x01\x02" or command[14] != 115:
                raise IntegrationError("driver sent unexpected HAT motor configuration")
            if command[16:18] != b"\x03\xe8":
                raise IntegrationError("driver sent unexpected HAT heartbeat configuration")
        trace: dict[str, int] = {
            "monotonic_ns": time.monotonic_ns(),
            "type": command[3],
            "sequence": int.from_bytes(command[4:6], "big"),
            "left_rpm": self._signed_i16(command[8:10]) if command[3] == 2 else 0,
            "right_rpm": self._signed_i16(command[10:12]) if command[3] == 2 else 0,
            "flags": command[15],
        }
        with self.lock:
            self.commands.append(trace)
        response = self._feedback(command)
        if command[3] == 2 and not self.fragmented:
            self.fragmented = True
            write_all(self.device.master, b"\x99\x00\xa5")
            invalid = bytearray(response)
            invalid[-1] ^= 0x01
            write_all(self.device.master, bytes(invalid))
            write_all(self.device.master, response[:11])
            time.sleep(0.001)
            write_all(self.device.master, response[11:])
        else:
            write_all(self.device.master, response)

    def _run(self) -> None:
        buffer = bytearray()
        while not self.stop_event.is_set():
            readable, _, _ = select.select([self.device.master], [], [], 0.01)
            if not readable:
                continue
            try:
                chunk = os.read(self.device.master, 4096)
            except BlockingIOError:
                continue
            except OSError as error:
                if self.stop_event.is_set():
                    return
                if error.errno == errno.EIO:
                    self.stop_event.wait(0.005)
                    continue
                self.error = str(error)
                return
            if not chunk:
                continue
            buffer.extend(chunk)
            while buffer:
                marker = buffer.find(b"\xa5\x5a")
                if marker < 0:
                    buffer[:] = buffer[-1:] if buffer[-1:] == b"\xa5" else b""
                    break
                if marker:
                    del buffer[:marker]
                if len(buffer) < 20:
                    break
                command = bytes(buffer[:20])
                if crc8_maxim(command[:19]) != command[19]:
                    del buffer[0]
                    continue
                del buffer[:20]
                try:
                    self._handle(command)
                except Exception as error:  # reported on the owning test thread
                    self.error = str(error)
                    self.stop_event.set()
                    return

    def close(self) -> None:
        self.stop_event.set()
        self.thread.join(timeout=1)
        if self.thread.is_alive():
            raise IntegrationError("DDSM PTY responder did not stop")


class ImuResponder:
    def __init__(self, pty_device: Pty, tail: int, gyro_z: float) -> None:
        self.device = pty_device
        self.tail = tail
        self.gyro_z = gyro_z
        self.stop_event = threading.Event()
        self.mode_lock = threading.Lock()
        self.mode = "valid"
        self.error: str | None = None
        self.packets_sent = 0
        self.outgoing = bytearray()
        self.thread = threading.Thread(target=self._run, name="dm-imu-pty-responder", daemon=True)
        self.thread.start()

    def set_mode(self, mode: str) -> None:
        with self.mode_lock:
            self.mode = mode

    def _current_mode(self) -> str:
        with self.mode_lock:
            return self.mode

    def _run(self) -> None:
        first = True
        while not self.stop_event.is_set():
            mode = self._current_mode()
            current = make_imu_packet(self.tail, corrupt_crc=(mode == "bad"),
                                      gyro_z=self.gyro_z)
            try:
                if first:
                    write_all(self.device.master, b"\x00\x55\x99\xaa")
                    write_all(self.device.master, current[:7])
                    time.sleep(0.002)
                    write_all(self.device.master, current[7:23])
                    time.sleep(0.002)
                    write_all(self.device.master, current[23:])
                    first = False
                else:
                    if self.packets_sent % 7 == 0:
                        write_all(self.device.master, b"\x13\x55\x00")
                    write_all(self.device.master, current)
                self.packets_sent += 1
            except BlockingIOError:
                pass
            except OSError as error:
                if self.stop_event.is_set():
                    return
                if error.errno == errno.EIO:
                    self.stop_event.wait(0.005)
                    continue
                self.error = str(error)
                return
            # The sensor stream runs at a modest test rate; the driver contract
            # concerns parsing and units, not the device's advertised 1 kHz mode.
            readable, _, _ = select.select([self.device.master], [], [], 0.02)
            if readable:
                try:
                    outgoing = os.read(self.device.master, 1024)
                    if outgoing:
                        self.outgoing.extend(outgoing)
                except BlockingIOError:
                    pass
                except OSError as error:
                    if self.stop_event.is_set():
                        return
                    if error.errno == errno.EIO:
                        self.stop_event.wait(0.005)
                        continue
                    self.error = str(error)
                    return

    def close(self) -> None:
        self.stop_event.set()
        self.thread.join(timeout=1)
        if self.thread.is_alive():
            raise IntegrationError("IMU PTY responder did not stop")


class Runner:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.output = args.output_dir.resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self.prefix = "rbt/hardware/test" + uuid.uuid4().hex
        self.map_tf = str(uuid.uuid4())
        self.odom_tf = str(uuid.uuid4())
        self.desired_topic = self.prefix + "/desired_cmd_vel"
        self.cmd_topic = "/" + self.prefix + "/cmd_vel"
        self.imu_topic = "/" + self.prefix + "/imu"
        self.phase_file = self.output / "phases.txt"
        self.phase_file.unlink(missing_ok=True)
        self.fixture_log = self.output / "fixture.log"
        self.children: dict[str, subprocess.Popen[bytes]] = {}
        self.logs: dict[str, Path] = {}
        self.process_exit_codes: dict[str, int | None] = {}
        self.base_shutdown_marker_ns: int | None = None
        self.phase_times: dict[str, int] = {}
        self.phase_read_position = 0
        self.base_pty = Pty()
        self.imu_pty = Pty()
        self.hat = HatResponder(self.base_pty)
        gyro_z = 0.2 if args.scenario == "pipeline" else 0.0
        self.imu = ImuResponder(self.imu_pty, args.imu_tail_byte, gyro_z)
        self.started = time.monotonic()

    def _start(self, name: str, command: list[str], log_path: Path) -> subprocess.Popen[bytes]:
        self.logs[name] = log_path
        with log_path.open("wb") as log_file:
            process = subprocess.Popen(command, cwd=REPO_ROOT, stdout=log_file,
                                       stderr=subprocess.STDOUT, close_fds=True)
        self.children[name] = process
        return process

    def _read_log(self, name: str) -> str:
        path = self.logs.get(name)
        if path is None:
            return ""
        try:
            return path.read_text(errors="replace")
        except FileNotFoundError:
            return ""

    def _refresh_phases(self) -> None:
        try:
            with self.phase_file.open("r", encoding="utf-8") as stream:
                stream.seek(self.phase_read_position)
                lines = stream.readlines()
                self.phase_read_position = stream.tell()
        except FileNotFoundError:
            return
        if lines and not lines[-1].endswith("\n"):
            self.phase_read_position -= len(lines[-1])
            lines.pop()
        for line in lines:
            fields = line.split()
            if not fields:
                continue
            if len(fields) != 2:
                raise IntegrationError(f"invalid fixture phase record: {line.rstrip()!r}")
            phase, timestamp_text = fields
            try:
                timestamp_ns = int(timestamp_text)
            except ValueError as error:
                raise IntegrationError(f"invalid fixture phase timestamp: {line.rstrip()!r}") from error
            if phase in self.phase_times:
                raise IntegrationError(f"duplicate fixture phase record: {phase}")
            if self.phase_times and timestamp_ns <= max(self.phase_times.values()):
                raise IntegrationError("fixture phase timestamps did not increase monotonically")
            self.phase_times[phase] = timestamp_ns
            if phase == "imu_invalid_only_prepare":
                self.imu.set_mode("bad")
            elif phase == "imu_valid_resume":
                self.imu.set_mode("valid")

    def _check_responders(self) -> None:
        if self.hat.error:
            raise IntegrationError("DDSM PTY responder failed: " + self.hat.error)
        if self.imu.error:
            raise IntegrationError("DM-IMU PTY responder failed: " + self.imu.error)

    def _check_children(self, fixture_may_finish: bool = False) -> None:
        self._refresh_phases()
        self._check_responders()
        for name, process in self.children.items():
            code = process.poll()
            if code is None:
                continue
            if name == "fixture" and fixture_may_finish and code == 0:
                continue
            if name == "guard" and "guard_crash" in self.phase_times and code == -signal.SIGKILL:
                continue
            if (name == "runtime" and "navigation_runtime_crash" in self.phase_times
                    and code == -signal.SIGKILL):
                continue
            raise IntegrationError(
                f"unexpected {name} exit status {code}; see {self.logs.get(name, self.fixture_log)}"
            )

    def _wait_for_marker(self, name: str, marker: str, deadline: float) -> None:
        while time.monotonic() < deadline:
            self._check_children()
            if marker in self._read_log(name):
                return
            time.sleep(0.025)
        raise IntegrationError(f"timed out waiting for {name} readiness marker {marker!r}")

    @staticmethod
    def _stop(process: subprocess.Popen[bytes]) -> int:
        if process.poll() is None:
            process.terminate()
            try:
                return process.wait(timeout=STOP_TIMEOUT_SECONDS)
            except subprocess.TimeoutExpired:
                process.kill()
                try:
                    return process.wait(timeout=STOP_TIMEOUT_SECONDS)
                except subprocess.TimeoutExpired as error:
                    raise IntegrationError(f"could not stop child process {process.pid}") from error
        return process.returncode if process.returncode is not None else 0

    def _assert_phase_controls(self, first: str, last: str, *, moving: bool = False,
                               zero: bool = False, brake: bool = False) -> None:
        segment = self._trace_segment(first, last)
        controls = [item for item in segment if item["type"] == 2]
        if not controls:
            raise IntegrationError(f"no real HAT control frames between {first} and {last}")
        has_zero = any(item["left_rpm"] == 0 and item["right_rpm"] == 0 for item in controls)
        has_motion = any(item["left_rpm"] != 0 or item["right_rpm"] != 0 for item in controls)
        if zero and not has_zero:
            raise IntegrationError(f"serial trace had no zero motor frames between {first} and {last}")
        if moving and not has_motion:
            raise IntegrationError(f"serial trace had no motion motor frames between {first} and {last}")
        if brake and not any(item["flags"] & 1 and item["left_rpm"] == 0
                             and item["right_rpm"] == 0 for item in controls):
            raise IntegrationError("graceful base shutdown did not send a zero brake command")

    def _trace_segment(self, first: str, last: str) -> list[dict[str, int]]:
        self._refresh_phases()
        if first not in self.phase_times or last not in self.phase_times:
            raise IntegrationError(f"missing serial trace phases {first!r} and {last!r}")
        start_ns = self.phase_times[first]
        end_ns = self.phase_times[last]
        if start_ns >= end_ns:
            raise IntegrationError(f"serial trace phases are out of order: {first!r}, {last!r}")
        commands = self.hat.snapshot()
        return [item for item in commands if start_ns <= item["monotonic_ns"] < end_ns]

    def _phase_offsets(self) -> dict[str, int]:
        commands = self.hat.snapshot()
        return {phase: sum(item["monotonic_ns"] < timestamp_ns for item in commands)
                for phase, timestamp_ns in self.phase_times.items()}

    def _assert_pipeline_trace(self) -> None:
        for first, last in (("raw_stop", "raw_resume"),
                            ("guard_crash", "invalid_probe"),
                            ("invalid_probe", "rearm_source100"),
                            ("estop", "estop_release"),
                            ("estop_release", "rearm_after_release"),
                            ("shutdown_stop", "pipeline_pass")):
            self._assert_phase_controls(first, last, zero=True)
        self._assert_phase_controls("rearm_source100", "estop", moving=True)
        self._assert_phase_controls("rearm_after_release", "shutdown_stop", moving=True)
        commands = self.hat.snapshot()
        signed_values = [value for command in commands if command["type"] == 2
                         for value in (command["left_rpm"], command["right_rpm"])]
        if not any(value < 0 for value in signed_values) or not any(value > 0 for value in signed_values):
            raise IntegrationError("HAT trace did not preserve both positive and negative motor RPM")

    def _assert_navigation_trace(self) -> None:
        self._assert_phase_controls("navigation_goal", "navigation_pass", moving=True)
        commands = self.hat.snapshot()
        pass_time = self.phase_times["navigation_pass"]
        controls = [item for item in commands
                    if item["type"] == 2 and item["monotonic_ns"] <= pass_time]
        final_controls = controls[-5:]
        if len(final_controls) < 5 or any(item["left_rpm"] != 0 or item["right_rpm"] != 0
                                          for item in final_controls):
            raise IntegrationError("navigation completion did not produce five final real zero motor frames")

    def _assert_navigation_crash_trace(self) -> None:
        self._assert_phase_controls("navigation_runtime_crash", "navigation_crash_pass", zero=True)
        segment = self._trace_segment("navigation_runtime_crash", "navigation_crash_pass")
        zeros = [item for item in segment if item["type"] == 2
                 and item["left_rpm"] == 0 and item["right_rpm"] == 0]
        if len(zeros) < 5:
            raise IntegrationError("runtime crash produced fewer than five real serial zero frames")

    def run(self) -> dict[str, object]:
        base_dir = self.args.base_build_dir.resolve()
        imu_dir = self.args.imu_build_dir.resolve()
        nav_dir = self.args.navigation_build_dir.resolve()
        base_binary = executable(base_dir, "rbt_base_controller")
        imu_binary = executable(imu_dir, "rbt_dm_imu")
        guard_binary = executable(nav_dir, "rbt_velocity_guard")
        runtime_binary = executable(nav_dir, "rbt_navigation_runtime")
        fixture_binary = self.args.fixture.resolve()
        if not fixture_binary.is_file() or not os.access(fixture_binary, os.X_OK):
            raise IntegrationError(f"fixture executable is unavailable: {fixture_binary}")

        fixture_log = self.output / "fixture.log"
        base_log = self.output / "base-controller.log"
        imu_log = self.output / "dm-imu.log"
        guard_log = self.output / "velocity-guard.log"
        runtime_log = self.output / "navigation-runtime.log"
        config = REPO_ROOT / "drivers/base_controller/config/ddsm_robot_current.yaml"
        base = self._start("base", [str(base_binary), "--config", str(config),
            "--serial-port", self.base_pty.path, "--prefix", self.prefix,
            "--cmd-vel-topic", self.cmd_topic, "--imu-topic", self.imu_topic], base_log)
        imu_duration = "20" if self.args.scenario == "pipeline" else "30"
        imu = self._start("imu", [str(imu_binary), "--serial-port", self.imu_pty.path,
            "--topic", self.imu_topic, "--duration", imu_duration], imu_log)
        guard = self._start("guard", [str(guard_binary), "--prefix", self.prefix,
            "--desired-topic", self.desired_topic, "--cmd-vel-topic", self.cmd_topic,
            "--source-id", "1", "--guard-source-id", "100", "--estop-source", "2",
            "--guard-timeout-ms", "250", "--cmd-ttl-ms", "100"], guard_log)
        self._wait_for_marker("base", "rbt_base_controller ready", time.monotonic() + 5)
        self._wait_for_marker("guard", "velocity guard ready", time.monotonic() + 5)

        if self.args.scenario in ("navigation", "navigation_crash"):
            self._start("runtime", [str(runtime_binary), "--prefix", self.prefix,
                "--tf-map", self.map_tf, "--tf-odom", self.odom_tf, "--source-id", "1",
                "--control-hz", "50", "--map-hz", "10", "--input-timeout-ms", "250",
                "--map-timeout-ms", "2000", "--plan-timeout-ms", "500",
                "--desired-topic", self.desired_topic], runtime_log)
            self._wait_for_marker("runtime", "navigation runtime ready", time.monotonic() + 5)

        self._check_children()
        fixture_args = [str(fixture_binary), "--scenario", self.args.scenario,
                        "--prefix", self.prefix, "--phase-file", str(self.phase_file)]
        if self.args.scenario == "pipeline":
            fixture_args += ["--guard-pid", str(guard.pid)]
        else:
            fixture_args += ["--tf-map", self.map_tf, "--tf-odom", self.odom_tf]
            if self.args.scenario == "navigation_crash":
                fixture_args += ["--runtime-pid", str(self.children["runtime"].pid)]
        self._start("fixture", fixture_args, fixture_log)
        self._wait_for_marker("fixture", "hardware IPC fixture ready", time.monotonic() + 5)
        deadline = time.monotonic() + (28 if self.args.scenario == "navigation" else 22)
        fixture = self.children["fixture"]
        while time.monotonic() < deadline:
            self._check_children(fixture_may_finish=True)
            code = fixture.poll()
            if code is not None:
                if code != 0:
                    raise IntegrationError(f"fixture exited with status {code}; see {fixture_log}")
                break
            time.sleep(0.01)
        else:
            raise IntegrationError(f"{self.args.scenario} hardware IPC fixture timed out")

        if self.args.scenario == "pipeline":
            if guard.poll() != -signal.SIGKILL:
                raise IntegrationError("fixture did not kill its supplied velocity-guard child")
            self._assert_pipeline_trace()
        elif self.args.scenario == "navigation":
            self._assert_navigation_trace()
        else:
            if self.children["runtime"].poll() != -signal.SIGKILL:
                raise IntegrationError("fixture did not kill its supplied navigation-runtime child")
            self._assert_navigation_crash_trace()

        return {
            "scenario": self.args.scenario,
            "prefix": self.prefix,
            "passed": True,
            "phase_times": self.phase_times,
            "phase_offsets": self._phase_offsets(),
            "hat_commands": self.hat.snapshot(),
            "imu_packets_sent": self.imu.packets_sent,
            "imu_serial_output_bytes": list(self.imu.outgoing),
            "logs": {name: str(path) for name, path in self.logs.items()},
        }

    def close(self) -> None:
        errors: list[str] = []
        expected_sigkill = {
            "pipeline": "guard",
            "navigation_crash": "runtime",
        }.get(self.args.scenario)
        for name in ("runtime", "guard", "base", "imu", "fixture"):
            process = self.children.get(name)
            if process is None:
                continue
            if name == "base":
                self.base_shutdown_marker_ns = time.monotonic_ns()
            if process.poll() is None:
                try:
                    self._stop(process)
                except Exception as stop_error:
                    errors.append(f"could not stop {name} child: {stop_error}")
            code = process.poll()
            self.process_exit_codes[name] = code
            if name == expected_sigkill and code == -signal.SIGKILL:
                continue
            if code != 0:
                errors.append(f"{name} child did not stop cleanly (status {code})")
        self.hat.close()
        self.imu.close()
        self.base_pty.close()
        self.imu_pty.close()
        commands = self.hat.snapshot()
        shutdown_stop = self.base_shutdown_marker_ns is not None and any(
            item["monotonic_ns"] >= self.base_shutdown_marker_ns
            and item["type"] == 2 and item["flags"] & 1
            and item["left_rpm"] == 0 and item["right_rpm"] == 0
            for item in commands)
        if not shutdown_stop:
            errors.append("base shutdown did not send a zero brake stopPair frame after the shutdown marker")
        if errors:
            raise IntegrationError("; ".join(errors))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario", choices=("pipeline", "navigation", "navigation_crash"), required=True)
    parser.add_argument("--fixture", type=Path, required=True)
    parser.add_argument("--base-build-dir", type=Path, default=REPO_ROOT / "target/base-controller-build")
    parser.add_argument("--imu-build-dir", type=Path, default=REPO_ROOT / "target/dm-imu-build")
    parser.add_argument("--navigation-build-dir", type=Path, default=REPO_ROOT / "target/navigation-build")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--imu-tail-byte", type=lambda value: int(value, 0), default=0x0D,
                        help="synthetic IMU record trailer byte (default 0x0d)")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    runner = Runner(args)
    result: dict[str, object] = {}
    error: Exception | None = None
    try:
        result = runner.run()
    except Exception as exception:
        error = exception
    finally:
        try:
            runner.close()
        except Exception as close_error:
            if error is None:
                error = close_error
        result.setdefault("scenario", args.scenario)
        result["passed"] = error is None
        result["elapsed_seconds"] = round(time.monotonic() - runner.started, 3)
        result["phase_times"] = runner.phase_times
        result["phase_offsets"] = runner._phase_offsets()
        result["process_exit_codes"] = runner.process_exit_codes
        result["base_shutdown_marker_ns"] = runner.base_shutdown_marker_ns
        result["hat_commands"] = runner.hat.snapshot()
        result["imu_packets_sent"] = runner.imu.packets_sent
        result["imu_serial_output_bytes"] = list(runner.imu.outgoing)
        result["logs"] = {name: str(path) for name, path in runner.logs.items()}
        output_path = runner.output / "result.json"
        output_path.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if error is not None:
        print(f"FAIL hardware_ipc_{args.scenario}: {error}")
        print(f"evidence: {runner.output / 'result.json'}")
        return 1
    print(f"PASS hardware_ipc_{args.scenario}: {runner.output / 'result.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

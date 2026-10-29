#!/usr/bin/env python3
"""Run SOFTWARE_FIXTURE hotplug checks. Hardware is never opened or tested."""
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


def _address() -> str:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return f"127.0.0.1:{sock.getsockname()[1]}"


def _objects(path: Path) -> list[dict[str, object]]:
    result: list[dict[str, object]] = []
    try:
        with path.open(encoding="utf-8", errors="replace") as source:
            for line in source:
                if not line.startswith("{"):
                    continue
                try:
                    item = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if isinstance(item, dict):
                    result.append(item)
    except FileNotFoundError:
        pass
    return result


def _count(path: Path, event: str) -> int:
    return sum(item.get("event") == event for item in _objects(path))


def _frames(path: Path) -> list[dict[str, object]]:
    return [item for item in _objects(path) if item.get("event") == "frame"]


def _tail(path: Path, limit: int = 2500) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")[-limit:]
    except OSError:
        return ""


class FixtureRun:
    def __init__(self, name: str, output: Path, composer: Path, fixture: Path, consumer: Path) -> None:
        self.name = name
        self.output = output
        self.composer = composer
        self.fixture = fixture
        self.consumer = consumer
        self.state = output / f"{name}.state"
        self.generation = output / f"{name}.generation"
        self.processes: list[subprocess.Popen[bytes]] = []
        self.handles: list[object] = []
        self.env = os.environ.copy()
        self.env["RBT_COMPOSER_ADDR"] = _address()
        self.env["RBT_ORBBEC_SOFTWARE_FIXTURE_STATE"] = str(self.state)
        self.env["RBT_ORBBEC_SOFTWARE_FIXTURE_GENERATION"] = str(self.generation)
        self.set_generations(0, 0)

    def _start(self, name: str, command: list[str]) -> subprocess.Popen[bytes]:
        handle = (self.output / f"{self.name}_{name}.log").open("wb")
        self.handles.append(handle)
        child = subprocess.Popen(
            command,
            env=self.env,
            stdout=handle,
            stderr=subprocess.STDOUT,
            start_new_session=True,
            cwd=self.output,
        )
        child._fixture_log = self.output / f"{self.name}_{name}.log"
        self.processes.append(child)
        return child

    def start_composer(self) -> subprocess.Popen[bytes]:
        return self._start("composer", [str(self.composer)])

    def start_fixture(self, *arguments: str) -> subprocess.Popen[bytes]:
        return self._start("camera", [str(self.fixture), *arguments])

    def start_consumer(self) -> subprocess.Popen[bytes]:
        return self._start("consumer", [str(self.consumer), "--provider", "orbbec335", "--stream", "depth",
                                         "--count", "500", "--timeout-sec", "20",
                                         "--tf-target", "orbbec335/color_optical", "--allow-unknown-uncertainty"])

    def set_presence(self, value: str) -> None:
        previous = self.state.read_text(encoding="utf-8").strip() if self.state.exists() else ""
        temporary = self.state.with_suffix(".next")
        temporary.write_text(value + "\n", encoding="utf-8")
        temporary.replace(self.state)
        if value == "absent" and previous != "absent":
            self.bump_target_generation()

    def read_generations(self) -> tuple[int, int]:
        values: dict[str, int] = {}
        for line in self.generation.read_text(encoding="utf-8").splitlines():
            key, separator, value = line.partition("=")
            if not separator or key not in ("target", "other"):
                raise RuntimeError("invalid SOFTWARE_FIXTURE generation file")
            values[key] = int(value)
        if set(values) != {"target", "other"}:
            raise RuntimeError("incomplete SOFTWARE_FIXTURE generation file")
        return values["target"], values["other"]

    def set_generations(self, target: int, other: int) -> None:
        temporary = self.generation.with_suffix(".next")
        temporary.write_text(f"target={target}\nother={other}\n", encoding="utf-8")
        temporary.replace(self.generation)

    def bump_target_generation(self) -> None:
        target, other = self.read_generations()
        self.set_generations(target + 1, other)

    def bump_other_generation(self) -> None:
        target, other = self.read_generations()
        self.set_generations(target, other + 1)

    def wait_for(self, condition, description: str, timeout: float = 8.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self._check_children()
            if condition():
                return
            time.sleep(0.025)
        raise TimeoutError(f"{self.name}: timed out waiting for {description}")

    def _check_children(self) -> None:
        for child in self.processes:
            code = child.poll()
            if code is not None:
                raise RuntimeError(f"{self.name}: {child.args[0]} exited early with status {code}: "
                                   f"{_tail(child._fixture_log)}")

    def stop(self, child: subprocess.Popen[bytes], expected: int | None = None, timeout: float = 5.0) -> int:
        if child.poll() is None:
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        try:
            code = child.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(child.pid, signal.SIGKILL)
            code = child.wait(timeout=3)
        if expected is not None and code != expected:
            raise RuntimeError(f"{self.name}: expected exit {expected}, got {code}: {_tail(child._fixture_log)}")
        return code

    def close(self) -> None:
        for child in reversed(self.processes):
            if child.poll() is None:
                try:
                    os.killpg(child.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
        for child in reversed(self.processes):
            try:
                child.wait(timeout=2)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(child.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                child.wait(timeout=3)
        for handle in self.handles:
            handle.close()


def _wait_for_activations(run: FixtureRun, camera_log: Path, count: int) -> list[dict[str, object]]:
    run.wait_for(lambda: _count(camera_log, "activation") >= count, f"activation {count}")
    return [item for item in _objects(camera_log) if item.get("event") == "activation"]


def _exercise_reconnect(output: Path, composer: Path, fixture: Path, consumer: Path) -> dict[str, object]:
    run = FixtureRun("reconnect", output, composer, fixture, consumer)
    camera_log = output / "reconnect_camera.log"
    consumer_log = output / "reconnect_consumer.log"
    run.state.write_text("present\n", encoding="utf-8")
    try:
        run.start_composer()
        camera = run.start_fixture()
        run.wait_for(lambda: _count(camera_log, "activation") >= 1, "initial activation")
        receive = run.start_consumer()
        run.wait_for(lambda: len(_frames(consumer_log)) >= 5, "initial consumer frames")

        resumptions: list[int] = []
        for activation_count in (2, 3):
            run.set_presence("absent")
            run.wait_for(lambda: _count(camera_log, "disconnected") >= activation_count - 1,
                         f"disconnect {activation_count - 1}")
            time.sleep(0.2)
            absent_baseline = len(_frames(consumer_log))
            time.sleep(0.35)
            if len(_frames(consumer_log)) != absent_baseline:
                raise RuntimeError("C++ receiver observed frames while SOFTWARE_FIXTURE was absent")

            run.set_presence("present")
            activations = _wait_for_activations(run, camera_log, activation_count)
            run.wait_for(lambda: len(_frames(consumer_log)) >= absent_baseline + 4,
                         f"consumer resume after activation {activation_count}")
            resumptions.append(len(_frames(consumer_log)) - absent_baseline)
            if activations[-1].get("serial") != "SOFTWARE_FIXTURE":
                raise RuntimeError("fixture activation omitted its SOFTWARE_FIXTURE identity")

        quick_replug_baseline = len(_frames(consumer_log))
        run.bump_target_generation()
        run.wait_for(lambda: _count(camera_log, "disconnected") >= 3,
                     "quick target removal generation while presence remains true")
        quick_activations = _wait_for_activations(run, camera_log, 4)
        run.wait_for(lambda: len(_frames(consumer_log)) >= quick_replug_baseline + 4,
                     "consumer frames after quick target replug", timeout=4.0)
        if run.state.read_text(encoding="utf-8").strip() != "present":
            raise RuntimeError("quick target replug unexpectedly changed the SOFTWARE_FIXTURE presence file")

        run.bump_other_generation()
        time.sleep(0.6)
        if _count(camera_log, "activation") != 4:
            raise RuntimeError("an unrelated-device generation event restarted the selected target")
        if len(quick_activations) < 4 or quick_activations[-1].get("serial") != "SOFTWARE_FIXTURE":
            raise RuntimeError("quick replug activation omitted its SOFTWARE_FIXTURE identity")

        activations = [item for item in _objects(camera_log) if item.get("event") == "activation"][:4]
        clocks = [item.get("sdk_clock_source") for item in activations]
        services = [next(stream.get("service") for stream in item.get("streams", [])
                         if stream.get("kind") == "depth") for item in activations]
        if len(activations) != 4 or len(set(clocks)) != 4 or len(set(services)) != 4:
            raise RuntimeError("reconnect did not create fresh activation clocks and depth services")

        run.set_presence("absent")
        run.wait_for(lambda: _count(camera_log, "disconnected") >= 4, "disconnect before signal shutdown")
        time.sleep(0.2)
        final_absence_baseline = len(_frames(consumer_log))
        time.sleep(0.35)
        if len(_frames(consumer_log)) != final_absence_baseline:
            raise RuntimeError("C++ receiver observed frames during final SOFTWARE_FIXTURE absence")
        signal_started = time.monotonic()
        run.stop(camera, expected=0, timeout=4.0)
        signal_seconds = time.monotonic() - signal_started
        if signal_seconds > 2.0:
            raise RuntimeError(f"SIGTERM while disconnected took {signal_seconds:.2f}s")
        run.stop(receive, timeout=3.0)
        return {"status": "PASS", "activations": 4, "fresh_clocks": True,
                "fresh_depth_services": True, "consumer_resumed": True,
                "absence_frame_checks": 3, "quick_replug_while_present": True,
                "other_device_event_ignored": True, "signal_shutdown_seconds": round(signal_seconds, 3),
                "consumer_frame_counts_after_resume": resumptions}
    finally:
        run.close()


def _exercise_total_duration(output: Path, composer: Path, fixture: Path, consumer: Path) -> dict[str, object]:
    run = FixtureRun("duration", output, composer, fixture, consumer)
    camera_log = output / "duration_camera.log"
    started = time.monotonic()
    run.state.write_text("present\n", encoding="utf-8")
    try:
        run.start_composer()
        # Leave time for two real Composer handshakes and activation teardown;
        # the deadline is still measured once from process startup.
        camera = run.start_fixture("--duration-sec", "6", "--reconnect-delay-ms", "300")
        run.wait_for(lambda: _count(camera_log, "activation") >= 1, "duration test activation")
        time.sleep(0.55)
        run.set_presence("absent")
        run.wait_for(lambda: _count(camera_log, "disconnected") >= 1, "duration test disconnect")
        time.sleep(0.65)
        run.set_presence("present")
        run.wait_for(lambda: _count(camera_log, "activation") >= 2, "duration test reconnect")
        try:
            code = camera.wait(timeout=7.0)
        except subprocess.TimeoutExpired as error:
            raise TimeoutError("total-duration camera did not stop at its original deadline") from error
        if code != 0:
            raise RuntimeError(f"total-duration camera exited {code}: {_tail(camera_log)}")
        elapsed = time.monotonic() - started
        if not 5.5 <= elapsed <= 6.8:
            raise RuntimeError(f"total duration was reset or expired early ({elapsed:.2f}s for 6s duration)")
        return {"status": "PASS", "configured_seconds": 6, "elapsed_seconds": round(elapsed, 3),
                "duration_spanned_disconnect": True}
    finally:
        run.close()


def _exercise_misconfiguration(output: Path, composer: Path, fixture: Path, consumer: Path) -> dict[str, object]:
    run = FixtureRun("misconfiguration", output, composer, fixture, consumer)
    camera_log = output / "misconfiguration_camera.log"
    run.state.write_text("present\n", encoding="utf-8")
    try:
        run.start_composer()
        camera = run.start_fixture("--width", "3")
        try:
            code = camera.wait(timeout=6.0)
        except subprocess.TimeoutExpired as error:
            raise TimeoutError("misconfigured profile retried instead of failing while device was present") from error
        if code != 1:
            raise RuntimeError(f"misconfiguration returned {code}, expected fatal status 1: {_tail(camera_log)}")
        if _count(camera_log, "reconnecting") != 0:
            raise RuntimeError("misconfigured profile caused an inappropriate reconnect retry")
        return {"status": "PASS", "fatal_status": 1, "retry_count": 0}
    finally:
        run.close()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--composer", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    composer = args.composer.resolve()
    output = args.output_dir.resolve()
    if not composer.is_file() or not os.access(composer, os.X_OK):
        parser.error(f"--composer must name an executable file: {composer}")
    output.mkdir(parents=True, exist_ok=False)
    fixture = build / "rbt_orbbec_supervisor_fixture"
    consumer = build / "rbt-orbbec-consumer"
    for path, label in ((fixture, "SOFTWARE_FIXTURE camera"), (consumer, "C++ consumer")):
        if not path.is_file() or not os.access(path, os.X_OK):
            parser.error(f"{label} executable is missing: {path}")

    result: dict[str, object] = {
        "status": "FAIL",
        "mode": "SOFTWARE_FIXTURE",
        "hardware": "NOT RUN",
        "checks": [],
    }
    try:
        for name, exercise in (
            ("hotplug_and_signal", _exercise_reconnect),
            ("total_duration", _exercise_total_duration),
            ("misconfiguration", _exercise_misconfiguration),
        ):
            started = time.monotonic()
            check = exercise(output, composer, fixture, consumer)
            result["checks"].append({"name": name, **check,
                                     "seconds": round(time.monotonic() - started, 3)})
        result["status"] = "PASS"
    except Exception as error:  # noqa: BLE001 - report a single focused fixture failure.
        result["error"] = f"{type(error).__name__}: {error}"
    result["children_reaped"] = True
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())

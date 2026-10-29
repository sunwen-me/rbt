"""Native Python Component lifecycle coverage for TF endpoints.

The integration tests run child Components from an isolated wheel overlay and
use a real Composer process.  Set ``RBT_TF_COMPONENT_OVERLAY`` to that overlay
when invoking pytest.
"""

from __future__ import annotations

import json
import os
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import Any

import pytest


REPOSITORY = (
    Path(os.environ["RBT_TF_COMPONENT_REPOSITORY"]).resolve()
    if "RBT_TF_COMPONENT_REPOSITORY" in os.environ
    else Path(__file__).resolve().parents[2]
)
TEST_FILE = Path(__file__).resolve()
COMPOSER = REPOSITORY / "target" / "debug" / "composer"
RBT = REPOSITORY / "target" / "debug" / "rbt"
OVERLAY = os.environ.get("RBT_TF_COMPONENT_OVERLAY")

pytestmark = pytest.mark.skipif(
    not OVERLAY or not COMPOSER.is_file() or not RBT.is_file(),
    reason="requires a fresh wheel overlay and native Composer/CLI binaries",
)


def _touch(directory: Path, name: str, contents: str = "ok\n") -> None:
    (directory / name).write_text(contents, encoding="utf-8")


def _child() -> int:
    import rbt
    from rbt.tf import (
        DynamicRecord,
        StaticEdge,
        StaticSet,
        TfError,
        TfTransportError,
        Transform3,
        TransformBuffer,
        Vec3,
        Quaternion,
    )
    from rbt.time import ClockIdentity, ClockKind, ClockSourceId, Timestamp

    overlay = Path(os.environ["RBT_TF_COMPONENT_OVERLAY"]).resolve()
    extension = Path(rbt._rbt.__file__).resolve()
    if not extension.is_relative_to(overlay):
        raise RuntimeError(f"loaded a non-overlay rbt extension: {extension}")

    markers = Path(os.environ["RBT_TF_COMPONENT_MARKERS"])
    markers.mkdir(parents=True, exist_ok=True)
    mode = sys.argv[2]
    address = sys.argv[3]
    clock = ClockIdentity(
        ClockKind.DEVICE,
        ClockSourceId.from_device_uuid("00000000-0000-0000-0000-000000000042"),
        7,
    )
    stamp = Timestamp(clock, 100)

    def static_set(revision: int, effective_ns: int, x: float) -> StaticSet:
        return StaticSet(
            revision,
            Timestamp(clock, effective_ns),
            0,
            (StaticEdge("world", "base", Transform3(Vec3(x, 0.0, 0.0), Quaternion.identity())),),
        )

    class TfProvider(rbt.Component):
        component_id = mode

        def __init__(self) -> None:
            self.recovery_activation = 0
            self.recovery_buffer = (
                TransformBuffer() if mode == "recovery-consumer" else None
            )
            self.old_dynamic_authority = None
            self.old_static_authority = None

        def assert_old_authorities_rejected(self) -> None:
            assert self.recovery_buffer is not None
            assert self.old_dynamic_authority is not None
            assert self.old_static_authority is not None
            for operation in (
                lambda: self.recovery_buffer.ingest_dynamic(
                    self.old_dynamic_authority,
                    DynamicRecord(
                        "world",
                        "moving",
                        stamp,
                        0,
                        1,
                        Transform3(Vec3(99.0, 0.0, 0.0), Quaternion.identity()),
                    ),
                ),
                lambda: self.recovery_buffer.apply_static_update(
                    self.old_static_authority, static_set(2, 200, 99.0)
                ),
            ):
                try:
                    operation()
                except TfError as error:
                    if error.code != "provider_unavailable":
                        raise
                else:
                    raise AssertionError("stale TF authority was accepted")

        def assert_old_history_removed(self) -> None:
            assert self.recovery_buffer is not None
            for source in ("moving", "base"):
                try:
                    self.recovery_buffer.lookup_at(
                        "world", source, stamp, uncertainty_ns=0
                    )
                except TfError:
                    continue
                raise AssertionError(f"old TF history for {source!r} survived teardown")
            self.assert_old_authorities_rejected()

        def activate(self, ctx: rbt.ActivationContext) -> None:
            if mode == "provider":
                dynamic = ctx.provide_dynamic_tf("Dynamic")
                static = ctx.provide_static_tf(
                    "Update", "Snapshot", static_set(1, 100, 1.0), 8
                )
                _touch(markers, "provider_activated")

                def serve(task: rbt.TaskContext) -> None:
                    try:
                        while not task.cancelled:
                            if (
                                (markers / "consumer_activated").exists()
                                and not (markers / "dynamic_received").exists()
                            ):
                                dynamic.publish(
                                    DynamicRecord(
                                        "world",
                                        "moving",
                                        stamp,
                                        0,
                                        1,
                                        Transform3(Vec3(3.0, 0.0, 0.0), Quaternion.identity()),
                                    )
                                )
                            if (
                                (markers / "snapshot_received").exists()
                                and not (markers / "static_update_published").exists()
                            ):
                                static.replace(static_set(2, 200, 2.0))
                                _touch(markers, "static_update_published")
                            static.serve_one(0.1)
                    except (TfTransportError, rbt.RpcActivationExpiredError):
                        pass
                    finally:
                        _touch(markers, "provider_worker_exited")

                ctx.spawn_background("tf-provider", serve)
                return

            if mode == "recovery-provider":
                generation = int(os.environ["RBT_TF_COMPONENT_GENERATION"])
                value = 1.0 if generation == 1 else 9.0
                dynamic = ctx.provide_dynamic_tf("Dynamic")
                static = ctx.provide_static_tf(
                    "Update", "Snapshot", static_set(1, 100, value), 8
                )
                _touch(
                    markers,
                    f"recovery_provider_activated_{generation}",
                    json.dumps(
                        {
                            "process": ctx.process_instance_id,
                            "activation": ctx.activation_id,
                        }
                    ),
                )

                def serve_recovery(task: rbt.TaskContext) -> None:
                    activated = markers / f"recovery_consumer_activated_{generation}"
                    published = False
                    try:
                        while not task.cancelled:
                            if activated.exists() and not published:
                                dynamic.publish(
                                    DynamicRecord(
                                        "world",
                                        "moving",
                                        stamp,
                                        0,
                                        generation,
                                        Transform3(
                                            Vec3(value + 2.0, 0.0, 0.0),
                                            Quaternion.identity(),
                                        ),
                                    )
                                )
                                published = True
                            static.serve_one(0.1)
                    except (TfTransportError, rbt.RpcActivationExpiredError):
                        pass
                    finally:
                        _touch(markers, f"recovery_provider_worker_exited_{generation}")

                ctx.spawn_background("tf-recovery-provider", serve_recovery)
                return

            if mode == "bad-dynamic-provider":
                ctx.provide_point_cloud("Dynamic", maximum_points=4)
                _touch(markers, "bad_dynamic_provider_activated")
                return

            if mode in {"pair-provider-a", "pair-provider-b"}:
                update_role, snapshot_role = (
                    ("Update", "OtherSnapshot")
                    if mode == "pair-provider-a"
                    else ("OtherUpdate", "Snapshot")
                )
                ctx.provide_static_tf(
                    update_role, snapshot_role, static_set(1, 100, 1.0), 8
                )
                _touch(markers, f"{mode}_activated")
                return

            if mode == "invalid-provider":
                invalid = StaticSet(
                    1,
                    stamp,
                    0,
                    (
                        StaticEdge("world", "base-a", Transform3.identity()),
                        StaticEdge("world", "base-b", Transform3.identity()),
                    ),
                )
                ctx.provide_static_tf("Update", "Snapshot", invalid, 1)
                return

            if mode == "consumer":
                buffer = TransformBuffer()
                dynamic = ctx.require_dynamic_tf("dynamic", "dynamic-slot", buffer)
                static = ctx.require_static_tf(
                    "updates", "snapshots", "static-slot", buffer, 8
                )
                _touch(markers, "consumer_activated")

                def consume(task: rbt.TaskContext) -> None:
                    try:
                        static.refresh_snapshot(2.0)
                        snapshot = buffer.lookup_at(
                            "world", "base", stamp, uncertainty_ns=0
                        )
                        if snapshot.sources[0].static_revision == 1:
                            _touch(markers, "snapshot_received")
                        while not task.cancelled:
                            dynamic.wait_and_drain(0.2)
                            try:
                                result = buffer.lookup_at(
                                    "world", "moving", stamp, uncertainty_ns=0
                                )
                            except Exception:
                                result = None
                            if result is not None:
                                _touch(markers, "dynamic_received")

                            static.wait_and_drain(0.2)
                            try:
                                result = buffer.lookup_at(
                                    "world", "base", Timestamp(clock, 200), uncertainty_ns=0
                                )
                            except Exception:
                                result = None
                            if (
                                result is not None
                                and result.sources[0].static_revision == 2
                                and result.transform.translation.x == 2.0
                            ):
                                _touch(markers, "static_update_received")
                            if (
                                (markers / "dynamic_received").exists()
                                and (markers / "static_update_received").exists()
                            ):
                                _touch(markers, "tf_traffic_complete")
                                _touch(markers, "consumer_waiting")
                                dynamic.wait_and_drain(5.0)
                    except TfTransportError as error:
                        if error.code != "transport_closed":
                            raise
                    finally:
                        _touch(markers, "consumer_worker_exited")

                ctx.spawn_background("tf-consumer", consume)
                return

            if mode == "recovery-consumer":
                self.recovery_activation += 1
                generation = self.recovery_activation
                buffer = self.recovery_buffer
                assert buffer is not None
                dynamic = ctx.require_dynamic_tf(
                    "dynamic", "dynamic-slot", buffer
                )
                static = ctx.require_static_tf(
                    "updates", "snapshots", "static-slot", buffer, 8
                )
                if generation == 1:
                    self.old_dynamic_authority = dynamic.authority
                    self.old_static_authority = static.authority
                elif generation == 2:
                    self.assert_old_authorities_rejected()

                _touch(
                    markers,
                    f"recovery_consumer_activated_{generation}",
                    json.dumps(
                        {
                            "process": ctx.process_instance_id,
                            "activation": ctx.activation_id,
                            "dynamic_physical": str(dynamic.physical_authority),
                            "static_physical": str(static.physical_authority),
                        }
                    ),
                )

                def consume_recovery(task: rbt.TaskContext) -> None:
                    try:
                        static.refresh_snapshot(2.0)
                        dynamic.wait_and_drain(2.0)
                        dynamic_result = buffer.lookup_at(
                            "world", "moving", stamp, uncertainty_ns=0
                        )
                        static_result = buffer.lookup_at(
                            "world", "base", stamp, uncertainty_ns=0
                        )
                        if generation == 1:
                            if dynamic_result.transform.translation.x != 3.0:
                                raise AssertionError("initial dynamic lookup was incorrect")
                            if static_result.transform.translation.x != 1.0:
                                raise AssertionError("initial static lookup was incorrect")
                        else:
                            if dynamic_result.transform.translation.x != 11.0:
                                raise AssertionError("recovered dynamic lookup was incorrect")
                            if static_result.transform.translation.x != 9.0:
                                raise AssertionError("recovered static lookup was incorrect")
                            if (
                                dynamic_result.sources[0].authority.physical_id
                                != dynamic.physical_authority
                            ):
                                raise AssertionError(
                                    "recovered dynamic source has stale authority"
                                )
                            if (
                                static_result.sources[0].authority.physical_id
                                != static.physical_authority
                            ):
                                raise AssertionError(
                                    "recovered static source has stale authority"
                                )
                            _touch(
                                markers,
                                "recovery_lookup_results",
                                json.dumps(
                                    {
                                        "dynamic": dynamic_result.transform.translation.x,
                                        "static": static_result.transform.translation.x,
                                        "dynamic_physical": str(
                                            dynamic_result.sources[0].authority.physical_id
                                        ),
                                        "static_physical": str(
                                            static_result.sources[0].authority.physical_id
                                        ),
                                    }
                                ),
                            )
                        _touch(markers, f"recovery_dynamic_received_{generation}")
                        _touch(markers, f"recovery_traffic_{generation}")
                        _touch(markers, f"recovery_waiting_{generation}")
                        while not task.cancelled:
                            dynamic.wait_and_drain(0.1)
                            static.wait_and_drain(0.1)
                    except TfTransportError as error:
                        if error.code != "transport_closed":
                            raise
                    finally:
                        if generation == 1:
                            self.assert_old_history_removed()
                            _touch(markers, "recovery_old_state_invalidated")
                        _touch(
                            markers,
                            f"recovery_consumer_worker_exited_{generation}",
                        )

                ctx.spawn_background("tf-recovery-consumer", consume_recovery)
                return

            if mode in {"bad-dynamic-consumer", "pair-consumer"}:
                buffer = TransformBuffer()
                if mode == "bad-dynamic-consumer":
                    ctx.require_dynamic_tf("dynamic", "dynamic-slot", buffer)
                else:
                    ctx.require_static_tf(
                        "updates", "snapshots", "static-slot", buffer, 8
                    )
                raise AssertionError("invalid TF binding was accepted")

            raise ValueError(f"unsupported child mode: {mode}")

    if mode in {"consumer", "recovery-consumer"}:
        requirements: Any = {
            "dynamic": "Dynamic",
            "updates": "Update",
            "snapshots": "Snapshot",
        }
    elif mode == "bad-dynamic-consumer":
        requirements = {"dynamic": "Dynamic"}
    elif mode == "pair-consumer":
        requirements = {"updates": "Update", "snapshots": "Snapshot"}
    else:
        requirements = {}

    rbt.run_component(
        TfProvider(),
        component_id=mode,
        requirements=requirements,
        composer_address=address,
    )
    return 0


def _port() -> int:
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def _environment(address: str, markers: Path) -> dict[str, str]:
    if OVERLAY is None:
        raise RuntimeError("RBT_TF_COMPONENT_OVERLAY is required")
    environment = os.environ.copy()
    environment["RBT_COMPOSER_ADDR"] = address
    environment["RBT_TF_COMPONENT_OVERLAY"] = OVERLAY
    environment["RBT_TF_COMPONENT_MARKERS"] = str(markers)
    environment["PYTHONPATH"] = OVERLAY
    return environment


def _query(address: str, *arguments: str) -> subprocess.CompletedProcess[str]:
    environment = os.environ.copy()
    environment["RBT_COMPOSER_ADDR"] = address
    return subprocess.run(
        [str(RBT), *arguments],
        cwd="/tmp",
        env=environment,
        capture_output=True,
        text=True,
        timeout=2,
        check=True,
    )


def _capabilities(address: str) -> list[dict[str, Any]]:
    return json.loads(_query(address, "capability", "list", "--json").stdout)


def _component_capabilities(address: str, component: str) -> list[dict[str, Any]]:
    return [
        item
        for item in _capabilities(address)
        if item["provider_component"] == component
    ]


def _component_record(address: str, component: str) -> dict[str, Any] | None:
    view = json.loads(_query(address, "ps", "--json").stdout)
    return next(
        (
            item
            for item in view["components"]
            if item["component_id"] == component
        ),
        None,
    )


def _component_state(address: str, component: str) -> str | None:
    record = _component_record(address, component)
    return None if record is None else record["state"]


def _tf_status(address: str) -> dict[str, Any]:
    return json.loads(_query(address, "tf", "status", "--json").stdout)


def _wait_for(
    predicate: Any,
    description: str,
    processes: tuple[subprocess.Popen[str], ...] = (),
    timeout: float = 15.0,
) -> Any:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for process in processes:
            if process.poll() is not None:
                log = getattr(process, "_rbt_log", None)
                detail = Path(log).read_text(encoding="utf-8") if log else ""
                pytest.fail(f"{description}: process exited ({process.returncode})\n{detail}")
        try:
            result = predicate()
        except (OSError, subprocess.SubprocessError, json.JSONDecodeError):
            result = None
        if result:
            return result
        time.sleep(0.05)
    pytest.fail(f"timed out waiting for {description}")


def _wait_for_activation_error(
    address: str,
    component: str,
    process: subprocess.Popen[str],
    message: str,
) -> str:
    log_path = Path(process._rbt_log)  # type: ignore[attr-defined]

    def failed() -> str | None:
        output = log_path.read_text(encoding="utf-8")
        if message in output and _component_state(address, component) == "FAILED":
            return output
        return None

    return _wait_for(failed, f"{component} activation rejection", (process,))


def _start_process(
    command: list[str], log_path: Path, environment: dict[str, str], cwd: Path
) -> subprocess.Popen[str]:
    with log_path.open("w", encoding="utf-8") as log:
        process = subprocess.Popen(
            command,
            cwd=cwd,
            env=environment,
            stdout=log,
            stderr=subprocess.STDOUT,
            text=True,
        )
    process._rbt_log = log_path  # type: ignore[attr-defined]
    return process


def _start_stack(tmp_path: Path) -> tuple[str, subprocess.Popen[str]]:
    address = f"127.0.0.1:{_port()}"
    log = tmp_path / "composer.log"
    composer = _start_process(
        [str(COMPOSER)],
        log,
        _environment(address, tmp_path / "markers"),
        tmp_path,
    )
    _wait_for(lambda: _query(address, "ps", "--json").returncode == 0, "Composer", (composer,))
    return address, composer


def _start_component(
    mode: str,
    address: str,
    markers: Path,
    tmp_path: Path,
    extra_environment: dict[str, str] | None = None,
) -> subprocess.Popen[str]:
    markers.mkdir(parents=True, exist_ok=True)
    environment = _environment(address, markers)
    if extra_environment is not None:
        environment.update(extra_environment)
    return _start_process(
        [sys.executable, str(TEST_FILE), "--child", mode, address],
        tmp_path / f"{mode}.log",
        environment,
        tmp_path,
    )


def _stop(process: subprocess.Popen[str], *, graceful: bool = True) -> None:
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGINT if graceful else signal.SIGTERM)
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)
        pytest.fail(f"process did not stop after {'SIGINT' if graceful else 'SIGTERM'}")


def _cleanup(processes: list[subprocess.Popen[str]]) -> None:
    for process in reversed(processes):
        if process.poll() is None:
            process.terminate()
    for process in reversed(processes):
        if process.poll() is None:
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)


def test_native_extension_imports_from_fresh_overlay(tmp_path: Path) -> None:
    assert sys.version_info[:2] == (3, 12)
    environment = os.environ.copy()
    environment["PYTHONPATH"] = str(OVERLAY)
    output = subprocess.run(
        [
            sys.executable,
            "-c",
            "import pathlib, rbt; print(pathlib.Path(rbt._rbt.__file__).resolve())",
        ],
        cwd=tmp_path,
        env=environment,
        capture_output=True,
        text=True,
        timeout=5,
        check=True,
    ).stdout.strip()
    assert Path(output).is_relative_to(Path(OVERLAY).resolve())
    assert not Path(output).is_relative_to(REPOSITORY / "python")


def test_python_tf_lifecycle_traffic_cancellation_and_cleanup(tmp_path: Path) -> None:
    address, composer = _start_stack(tmp_path)
    processes = [composer]
    markers = tmp_path / "markers"
    try:
        provider = _start_component("provider", address, markers, tmp_path)
        processes.append(provider)
        capabilities = _wait_for(
            lambda: (
                items
                if len(items := _component_capabilities(address, "provider")) == 3
                else None
            ),
            "TF provider advertisements",
            (composer, provider),
        )
        assert {item["capability_type"] for item in capabilities} == {
            "Dynamic",
            "Update",
            "Snapshot",
        }
        assert len({item["capability_instance"] for item in capabilities}) == 3
        update = next(item for item in capabilities if item["capability_type"] == "Update")
        snapshot = next(item for item in capabilities if item["capability_type"] == "Snapshot")
        assert update["provider_process"] == snapshot["provider_process"]
        assert update["activation"] == snapshot["activation"]

        consumer = _start_component("consumer", address, markers, tmp_path)
        processes.append(consumer)
        _wait_for(
            lambda: (markers / "tf_traffic_complete").is_file(),
            "dynamic TF, static snapshot, and static update traffic",
            (composer, provider, consumer),
        )
        _wait_for(
            lambda: (markers / "consumer_waiting").is_file(),
            "consumer entering a native TF wait",
            (consumer,),
        )

        _stop(consumer)
        assert (markers / "consumer_worker_exited").is_file()
        assert consumer.returncode == 0

        _stop(provider)
        assert (markers / "provider_worker_exited").is_file()
        assert provider.returncode == 0
        _wait_for(
            lambda: not _component_capabilities(address, "provider"),
            "TF capability cleanup after provider shutdown",
            (composer,),
        )
    finally:
        _cleanup(processes)


def test_tf_consumer_recovers_after_provider_process_loss(tmp_path: Path) -> None:
    address, composer = _start_stack(tmp_path)
    processes = [composer]
    markers = tmp_path / "markers"
    try:
        first_provider = _start_component(
            "recovery-provider",
            address,
            markers,
            tmp_path,
            {"RBT_TF_COMPONENT_GENERATION": "1"},
        )
        processes.append(first_provider)
        first_capabilities = _wait_for(
            lambda: (
                items
                if len(items := _component_capabilities(address, "recovery-provider"))
                == 3
                else None
            ),
            "initial recovery provider advertisements",
            (composer, first_provider),
        )
        first_ids = {item["capability_instance"] for item in first_capabilities}
        first_processes = {item["provider_process"] for item in first_capabilities}
        first_activations = {str(item["activation"]) for item in first_capabilities}
        assert len(first_ids) == 3
        assert len(first_processes) == 1
        assert len(first_activations) == 1

        consumer = _start_component(
            "recovery-consumer", address, markers, tmp_path
        )
        processes.append(consumer)
        _wait_for(
            lambda: (markers / "recovery_traffic_1").is_file(),
            "initial TF lookups from the provider",
            (composer, first_provider, consumer),
        )
        first_consumer_activation = json.loads(
            (markers / "recovery_consumer_activated_1").read_text(encoding="utf-8")
        )
        first_consumer_record = _wait_for(
            lambda: (
                record
                if (record := _component_record(address, "recovery-consumer"))
                and record["state"] == "ACTIVE"
                and record["activation_id"] == 1
                else None
            ),
            "initial consumer activation",
            (composer, consumer),
        )
        assert first_consumer_record["process_instance_id"] == first_consumer_activation[
            "process"
        ]
        assert first_consumer_record["activation_id"] == first_consumer_activation[
            "activation"
        ]
        _wait_for(
            lambda: (markers / "recovery_waiting_1").is_file(),
            "initial consumer entering a native wait",
            (consumer,),
        )

        first_provider.kill()
        first_provider.wait(timeout=5)
        assert first_provider.returncode is not None
        _wait_for(
            lambda: (
                record
                if (record := _component_record(address, "recovery-consumer"))
                and record["state"] == "BLOCKED"
                and record["activation_id"] is None
                else None
            ),
            "consumer deactivation after provider loss",
            (composer, consumer),
        )
        _wait_for(
            lambda: (markers / "recovery_old_state_invalidated").is_file()
            and (markers / "recovery_consumer_worker_exited_1").is_file(),
            "old buffer history and authorities invalidated during teardown",
            (consumer,),
        )
        _wait_for(
            lambda: not _component_capabilities(address, "recovery-provider"),
            "provider capability removal after process loss",
            (composer,),
        )

        second_provider = _start_component(
            "recovery-provider",
            address,
            markers,
            tmp_path,
            {"RBT_TF_COMPONENT_GENERATION": "2"},
        )
        processes.append(second_provider)
        second_capabilities = _wait_for(
            lambda: (
                items
                if len(items := _component_capabilities(address, "recovery-provider"))
                == 3
                else None
            ),
            "replacement TF provider advertisements",
            (composer, second_provider),
        )
        second_ids = {item["capability_instance"] for item in second_capabilities}
        second_processes = {item["provider_process"] for item in second_capabilities}
        second_activations = {str(item["activation"]) for item in second_capabilities}
        assert len(second_ids) == 3
        assert first_ids.isdisjoint(second_ids)
        assert len(second_processes) == 1
        assert first_processes.isdisjoint(second_processes)
        assert len(second_activations) == 1

        _wait_for(
            lambda: (markers / "recovery_traffic_2").is_file(),
            "TF lookups from the replacement provider",
            (composer, consumer, second_provider),
        )
        second_consumer_activation = json.loads(
            (markers / "recovery_consumer_activated_2").read_text(encoding="utf-8")
        )
        second_consumer_record = _wait_for(
            lambda: (
                record
                if (record := _component_record(address, "recovery-consumer"))
                and record["state"] == "ACTIVE"
                and record["activation_id"] == 2
                else None
            ),
            "consumer reactivation with the same process and next activation",
            (composer, consumer, second_provider),
        )
        assert second_consumer_record["process_instance_id"] == first_consumer_record[
            "process_instance_id"
        ]
        assert second_consumer_activation["process"] == first_consumer_activation[
            "process"
        ]
        assert second_consumer_activation["activation"] == 2
        capabilities_by_type = {
            item["capability_type"]: item for item in second_capabilities
        }
        assert second_consumer_activation["dynamic_physical"] == capabilities_by_type[
            "Dynamic"
        ]["capability_instance"]
        assert second_consumer_activation["static_physical"] == capabilities_by_type[
            "Update"
        ]["capability_instance"]

        lookup_results = json.loads(
            (markers / "recovery_lookup_results").read_text(encoding="utf-8")
        )
        assert lookup_results == {
            "dynamic": 11.0,
            "static": 9.0,
            "dynamic_physical": capabilities_by_type["Dynamic"][
                "capability_instance"
            ],
            "static_physical": capabilities_by_type["Update"][
                "capability_instance"
            ],
        }

        def replacement_bindings_seen() -> dict[str, Any] | None:
            report = _tf_status(address)
            providers = [
                item
                for item in report["providers"]
                if item["provider_component"] == "recovery-provider"
            ]
            if len(providers) != 3:
                return None
            if not all(
                any(
                    consumer_binding["component"] == "recovery-consumer"
                    and consumer_binding["process"]
                    == second_consumer_record["process_instance_id"]
                    and consumer_binding["activation"] == 2
                    for consumer_binding in provider["consumers"]
                )
                for provider in providers
            ):
                return None
            return report

        tf_status = _wait_for(
            replacement_bindings_seen,
            "Composer bindings to the replacement provider activation",
            (composer, consumer, second_provider),
        )
        assert {
            item["capability_type"]
            for item in tf_status["providers"]
            if item["provider_component"] == "recovery-provider"
        } == {"Dynamic", "Update", "Snapshot"}
    finally:
        _cleanup(processes)


def test_tf_pairing_and_descriptor_validation_fail_closed(tmp_path: Path) -> None:
    address, composer = _start_stack(tmp_path)
    processes = [composer]
    markers = tmp_path / "markers"
    try:
        bad_provider = _start_component(
            "bad-dynamic-provider", address, markers, tmp_path
        )
        processes.append(bad_provider)
        _wait_for(
            lambda: _component_capabilities(address, "bad-dynamic-provider"),
            "malformed dynamic stream advertisement",
            (composer, bad_provider),
        )
        bad_consumer = _start_component("bad-dynamic-consumer", address, markers, tmp_path)
        processes.append(bad_consumer)
        error_log = _wait_for_activation_error(
            address,
            "bad-dynamic-consumer",
            bad_consumer,
            "canonical Control stream",
        )
        assert "dynamic TF requirement descriptor" in error_log
        _stop(bad_consumer)

        first = _start_component("pair-provider-a", address, markers, tmp_path)
        second = _start_component("pair-provider-b", address, markers, tmp_path)
        processes.extend((first, second))
        _wait_for(
            lambda: (
                len(_component_capabilities(address, "pair-provider-a")) == 2
                and len(_component_capabilities(address, "pair-provider-b")) == 2
            ),
            "paired static capabilities from separate providers",
            (composer, first, second),
        )
        pair_consumer = _start_component("pair-consumer", address, markers, tmp_path)
        processes.append(pair_consumer)
        _wait_for_activation_error(
            address,
            "pair-consumer",
            pair_consumer,
            "same provider process and activation",
        )
        _stop(pair_consumer)
    finally:
        _cleanup(processes)


def test_invalid_static_initial_set_is_not_advertised(tmp_path: Path) -> None:
    address, composer = _start_stack(tmp_path)
    processes = [composer]
    markers = tmp_path / "markers"
    try:
        invalid = _start_component("invalid-provider", address, markers, tmp_path)
        processes.append(invalid)
        error_log = _wait_for_activation_error(
            address,
            "invalid-provider",
            invalid,
            "static set is outside the configured transform bound",
        )
        assert "TfTransportError" in error_log
        assert not _component_capabilities(address, "invalid-provider")
        _stop(invalid)
    finally:
        _cleanup(processes)


if __name__ == "__main__" and len(sys.argv) > 1 and sys.argv[1] == "--child":
    raise SystemExit(_child())

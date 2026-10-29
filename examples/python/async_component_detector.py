#!/usr/bin/env python3
"""Deterministic Image-to-PointCloud Python asyncio Component example.

The lifecycle hooks remain synchronous.  Activation-owned application work is
registered as coroutines, and ``receive_async`` returns the existing borrowed
shared-memory sample directly; no payload is staged in asyncio.
"""

from __future__ import annotations

import argparse
import asyncio
import json
import os
import threading
import time
from collections.abc import Mapping
from pathlib import Path
from typing import Any

import numpy as np

import rbt


COMPONENT_ID = "detector"
INPUT_CAPABILITY = "Camera"
OUTPUT_CAPABILITY = "Detections"
OUTPUT_POINT_COUNT = 4
REQUIREMENTS = {"camera": INPUT_CAPABILITY}
_U64_MASK = (1 << 64) - 1
RETAINED_INPUT_VIEWS: list[np.ndarray[Any, np.dtype[np.uint8]]] = []


def _optional_nonnegative_int(name: str) -> int | None:
    """Read one of this example's test-only integer environment hooks."""
    value = os.environ.get(name)
    if value is None or value == "":
        return None
    try:
        parsed = int(value, 10)
    except ValueError as error:
        raise ValueError(f"{name} must be a non-negative integer") from error
    if parsed < 0:
        raise ValueError(f"{name} must be a non-negative integer")
    return parsed


def _point_checksum(points: np.ndarray[Any, np.dtype[np.void]]) -> int:
    """Return the viewer-compatible checksum without copying the loan."""
    words = points.view(np.uint32).reshape(OUTPUT_POINT_COUNT, 4)
    checksum = 0
    for point_words in words:
        point_checksum = int(point_words[0])
        for word in point_words[1:]:
            point_checksum = (point_checksum * 31 + int(word)) & _U64_MASK
        checksum = (checksum + point_checksum) & _U64_MASK
    return checksum


def _debug_async_wait_stats(camera: Any) -> tuple[int, int, int, bool]:
    """Read native readiness counters without touching the sample payload."""
    debug_stats = getattr(camera, "_debug_async_wait_stats", None)
    if not callable(debug_stats):
        native = getattr(camera, "_native", None)
        debug_stats = getattr(native, "_debug_async_wait_stats", None)
    if not callable(debug_stats):
        return 0, 0, 0, False
    try:
        readiness_events, readiness_callbacks, last_readiness_unix_ns = debug_stats()
        return (
            int(readiness_events),
            int(readiness_callbacks),
            int(last_readiness_unix_ns),
            True,
        )
    except (TypeError, ValueError):
        # Keep an older installed wheel usable while making the missing
        # evidence explicit in the detector log.
        return 0, 0, 0, False


def _native_thread_ids(prefix: str) -> list[int]:
    """Return Linux task IDs whose kernel thread name starts with prefix."""
    task_root = Path("/proc/self/task")
    result: list[int] = []
    for task_dir in task_root.iterdir():
        try:
            name = (task_dir / "comm").read_text(encoding="utf-8").strip()
            task_id = int(task_dir.name)
        except (FileNotFoundError, PermissionError, ValueError):
            continue
        if name.startswith(prefix):
            result.append(task_id)
    return sorted(result)


async def _bounded_work(work_ms: int | None) -> None:
    """Simulate bounded cancellable application work without polling."""
    if work_ms is None or work_ms == 0:
        return
    await asyncio.sleep(work_ms / 1000.0)


class Detector(rbt.Component):
    """A small async detector with the synchronous lifecycle contract."""

    component_id = COMPONENT_ID
    requirements = REQUIREMENTS

    def __init__(self) -> None:
        self.model: object | None = None
        self.model_id: int | None = None
        self.model_load_count = 0
        self.activation_count = 0
        self.fail_after: int | None = None
        self.background_fail_after: int | None = None
        self.work_ms: int | None = None
        self.old_camera: rbt.ImageSubscriber | None = None
        self.retain_input_view = False
        self._zero_copy_evidence_logged = False
        self._retained_input_pointer: int | None = None
        self._retained_input_checksum: int | None = None
        self._managed_loop_id: int | None = None
        self._managed_loop_thread_id: int | None = None
        self._managed_loop_native_thread_id: int | None = None
        self._managed_loop_thread_count: int | None = None
        self._coroutine_generation = 0
        self._active_async_tasks: dict[int, set[int]] = {}

    def _register_async_task(
        self, activation: int, role: str
    ) -> tuple[int, int, int, int]:
        """Record stable managed-loop/task identity for harness diagnostics."""
        loop = asyncio.get_running_loop()
        current = asyncio.current_task(loop)
        if current is None:
            raise RuntimeError("async detector task has no current asyncio task")

        loop_id = id(loop)
        loop_thread_id = threading.get_ident()
        native_loop_thread_id = threading.get_native_id()
        managed_loop_thread_ids = _native_thread_ids("rbt-python-asyn")
        wait_bridge_thread_ids = _native_thread_ids("rbt-wait-bridg")
        managed_loop_thread_count = len(managed_loop_thread_ids)
        if (
            managed_loop_thread_count != 1
            or managed_loop_thread_ids[0] != native_loop_thread_id
        ):
            raise RuntimeError(
                "async detector did not find exactly its current managed loop thread"
            )
        if self._managed_loop_id is None:
            self._managed_loop_id = loop_id
            self._managed_loop_thread_id = loop_thread_id
            self._managed_loop_native_thread_id = native_loop_thread_id
            self._managed_loop_thread_count = managed_loop_thread_count
        elif (
            loop_id != self._managed_loop_id
            or loop_thread_id != self._managed_loop_thread_id
            or native_loop_thread_id != self._managed_loop_native_thread_id
            or managed_loop_thread_count != self._managed_loop_thread_count
        ):
            raise RuntimeError(
                "async detector managed loop/thread identity changed across activations"
            )

        self._coroutine_generation += 1
        coroutine_id = id(current)
        self._active_async_tasks.setdefault(activation, set()).add(coroutine_id)
        active_adapter_tasks = sum(
            len(task_ids) for task_ids in self._active_async_tasks.values()
        )
        if role == "inference":
            print(
                f"DETECTOR ASYNC LOOP IDENTITY activation={activation} "
                f"loop_id={loop_id} managed_loop_id={loop_id} "
                f"loop_thread_id={loop_thread_id} thread_id={loop_thread_id} "
                f"managed_loop_thread_id={loop_thread_id} "
                f"native_loop_thread_id={native_loop_thread_id} "
                f"loop_thread_count={managed_loop_thread_count} "
                f"managed_loop_thread_count={managed_loop_thread_count} "
                f"wait_bridge_thread_count={len(wait_bridge_thread_ids)} "
                f"wait_bridge_thread_ids="
                f"{','.join(map(str, wait_bridge_thread_ids)) or 'none'}",
                flush=True,
            )
        print(
            f"DETECTOR ASYNC TASK START activation={activation} role={role} "
            f"loop_id={loop_id} loop_thread_id={loop_thread_id} "
            f"coroutine_id={coroutine_id} "
            f"coroutine_generation={self._coroutine_generation} "
            f"active_adapter_tasks={active_adapter_tasks}",
            flush=True,
        )
        return loop_id, loop_thread_id, coroutine_id, self._coroutine_generation

    def _finish_async_task(
        self,
        activation: int,
        role: str,
        loop_id: int,
        loop_thread_id: int,
        coroutine_id: int,
        coroutine_generation: int,
    ) -> None:
        task_ids = self._active_async_tasks.get(activation)
        if task_ids is not None:
            task_ids.discard(coroutine_id)
            if not task_ids:
                del self._active_async_tasks[activation]
        active_adapter_tasks = sum(
            len(task_ids) for task_ids in self._active_async_tasks.values()
        )
        print(
            f"DETECTOR ASYNC TASK FINALLY activation={activation} role={role} "
            f"loop_id={loop_id} loop_thread_id={loop_thread_id} "
            f"coroutine_id={coroutine_id} "
            f"coroutine_generation={coroutine_generation} "
            f"activation_tasks_remaining="
            f"{len(self._active_async_tasks.get(activation, ()))} "
            f"active_adapter_tasks={active_adapter_tasks}",
            flush=True,
        )

    def prepare(self, config: Any) -> None:
        """Load the tiny model once for the lifetime of this component."""
        del config
        self.model = object()
        self.model_id = id(self.model)
        self.model_load_count += 1
        # Keep the established hooks used by the synchronous mixed harness.
        # The async-specific spelling is also accepted for the async gate.
        self.fail_after = _optional_nonnegative_int("RBT_TEST_PY_FAIL_AFTER")
        async_fail_after = _optional_nonnegative_int("RBT_TEST_PY_ASYNC_FAIL_AFTER")
        if async_fail_after is not None:
            self.fail_after = async_fail_after
        self.background_fail_after = _optional_nonnegative_int(
            "RBT_TEST_PY_BACKGROUND_FAIL_AFTER"
        )
        self.work_ms = _optional_nonnegative_int("RBT_TEST_PY_WORK_MS")
        self.retain_input_view = os.environ.get("RBT_TEST_PY_RETAIN_INPUT_VIEW") == "1"
        print(
            f"MODEL LOADED component={COMPONENT_ID} "
            f"model_load_count={self.model_load_count} model_id={self.model_id}",
            flush=True,
        )

    def activate(self, ctx: rbt.ActivationContext) -> None:
        """Resolve the input and create activation-scoped output/coroutines."""
        previous_camera = self.old_camera
        if previous_camera is not None:
            try:
                previous_camera.try_receive()
            except rbt.ActivationExpiredError:
                print(
                    f"STALE HANDLE REJECTED component={ctx.component_id} "
                    f"activation={ctx.activation_id}",
                    flush=True,
                )
            else:
                raise RuntimeError("old activation camera handle remained usable")
            if RETAINED_INPUT_VIEWS:
                retained = RETAINED_INPUT_VIEWS[0]
                retained_pointer = int(retained.ctypes.data)
                retained_checksum = int(retained.sum(dtype=np.uint64))
                if (
                    retained_pointer != self._retained_input_pointer
                    or retained_checksum != self._retained_input_checksum
                    or retained.flags.owndata
                    or retained.flags.writeable
                ):
                    raise RuntimeError(
                        "retained async NumPy input view changed after activation teardown"
                    )
                print(
                    f"RETAINED INPUT VIEW VALID component={ctx.component_id} "
                    f"activation={ctx.activation_id} pointer={retained_pointer} "
                    f"checksum={retained_checksum} OWNDATA={retained.flags.owndata} "
                    f"WRITEABLE={retained.flags.writeable}",
                    flush=True,
                )
        camera = ctx.require("camera")
        self.old_camera = camera
        detections = ctx.provide_point_cloud(
            OUTPUT_CAPABILITY, maximum_points=OUTPUT_POINT_COUNT
        )
        self.activation_count += 1

        component = ctx.component_id
        process_instance = ctx.process_instance_id
        activation = ctx.activation_id
        model_id = self.model_id
        model_load_count = self.model_load_count
        activation_count = self.activation_count
        print(
            f"DETECTOR ACTIVATED component={component} "
            f"process_instance={process_instance} activation={activation} "
            f"input_capability={INPUT_CAPABILITY} "
            f"output_capability={OUTPUT_CAPABILITY} model_id={model_id} "
            f"model_load_count={model_load_count} "
            f"activation_count={activation_count}",
            flush=True,
        )

        if previous_camera is not None:
            ctx.spawn_background_async(
                "stale-async-handle",
                self._stale_async_handle,
                previous_camera,
                component,
                process_instance,
                activation,
            )

        ctx.spawn_critical_async(
            "inference",
            self._inference,
            camera,
            detections,
            component,
            process_instance,
            activation,
            model_id,
            activation_count,
        )
        ctx.spawn_background_async(
            "health",
            self._background,
            component,
            process_instance,
            activation,
            model_id,
        )

    async def _stale_async_handle(
        self,
        task: rbt.TaskContext,
        camera: rbt.ImageSubscriber,
        component: str,
        process_instance: str,
        activation: int,
    ) -> None:
        """Prove a replaced activation's async endpoint is expired promptly."""
        del task
        loop_id, loop_thread_id, coroutine_id, coroutine_generation = (
            self._register_async_task(activation, "stale-async")
        )
        sample: rbt.ReceivedImageSample | None = None
        try:
            try:
                sample = await camera.receive_async()
            except rbt.ActivationExpiredError:
                print(
                    f"STALE ASYNC HANDLE REJECTED component={component} "
                    f"process_instance={process_instance} activation={activation} "
                    "expected=ActivationExpiredError",
                    flush=True,
                )
                return
            if sample is None:
                raise RuntimeError("expired async camera returned None")
            raise RuntimeError("old activation async camera remained usable")
        except asyncio.CancelledError:
            raise
        finally:
            del sample
            self._finish_async_task(
                activation,
                "stale-async",
                loop_id,
                loop_thread_id,
                coroutine_id,
                coroutine_generation,
            )

    async def _inference(
        self,
        task: rbt.TaskContext,
        camera: rbt.ImageSubscriber,
        detections: rbt.PointCloudPublisher,
        component: str,
        process_instance: str,
        activation: int,
        model_id: int | None,
        activation_count: int,
    ) -> None:
        processed = 0
        retained_this_activation = False
        loop_id, loop_thread_id, coroutine_id, coroutine_generation = (
            self._register_async_task(activation, "inference")
        )
        try:
            if self.fail_after == 0:
                raise RuntimeError("test critical failure after processing 0 frames")

            while not task.cancelled:
                # Readiness is bridged by the native WaitSet.  The coroutine
                # receives the existing sample only after the await resumes.
                wait_stats_before = _debug_async_wait_stats(camera)
                frame = await camera.receive_async()
                wait_stats_after = _debug_async_wait_stats(camera)
                continuation_time_ns = time.time_ns()
                if task.cancelled:
                    break
                if frame is None:
                    continue

                image = frame.numpy()
                image_checksum = int(image.sum(dtype=np.uint64))
                sequence = frame.sequence
                loan = detections.loan(
                    OUTPUT_POINT_COUNT,
                    sequence=sequence,
                    timestamp_ns=sequence * 1_000_000,
                    dense=True,
                )
                output = loan.numpy()
                indices = np.arange(OUTPUT_POINT_COUNT, dtype=np.float32)
                output["x"] = indices
                output["y"] = indices * np.float32(2.0)
                output["z"] = -indices
                output["intensity"] = np.mod(indices, np.float32(31.0))
                point_checksum = _point_checksum(output)
                readiness_latency_ns = (
                    continuation_time_ns - wait_stats_after[2]
                    if (
                        wait_stats_after[3]
                        and wait_stats_after[2] > 0
                        and wait_stats_after[0] > wait_stats_before[0]
                    )
                    else -1
                )
                print(
                    f"DETECTOR ASYNC WAIT component={component} "
                    f"activation={activation} sequence={sequence} "
                    f"native_readiness_events={wait_stats_after[0]} "
                    f"scheduled_readiness_callbacks={wait_stats_after[1]} "
                    f"readiness_events={wait_stats_after[0]} "
                    f"readiness_callbacks={wait_stats_after[1]} "
                    f"readiness_events_delta="
                    f"{wait_stats_after[0] - wait_stats_before[0]} "
                    f"readiness_callbacks_delta="
                    f"{wait_stats_after[1] - wait_stats_before[1]} "
                    f"last_readiness_unix_ns={wait_stats_after[2]} "
                    f"readiness_latency_ns={readiness_latency_ns} "
                    f"readiness_continuation_latency_ns={readiness_latency_ns} "
                    f"wait_stats_supported={int(wait_stats_after[3])}",
                    flush=True,
                )

                if self.retain_input_view and not RETAINED_INPUT_VIEWS:
                    RETAINED_INPUT_VIEWS.append(image)
                    retained_this_activation = True
                    self._retained_input_pointer = int(image.ctypes.data)
                    self._retained_input_checksum = image_checksum
                    print(
                        f"RETAINED INPUT VIEW component={component} "
                        f"activation={activation} pointer={image.ctypes.data}",
                        flush=True,
                    )

                if not self._zero_copy_evidence_logged:
                    self._zero_copy_evidence_logged = True
                    print(
                        f"DETECTOR ZERO-COPY EVIDENCE component={component} "
                        f"activation={activation} input_pointer="
                        f"{frame._debug_payload_address()} input_numpy_pointer="
                        f"{image.ctypes.data} input_OWNDATA={image.flags.owndata} "
                        f"input_WRITEABLE={image.flags.writeable} "
                        f"loan_pointer={loan._debug_payload_address()} "
                        f"loan_numpy_pointer={output.ctypes.data} "
                        f"loan_OWNDATA={output.flags.owndata} "
                        f"loan_WRITEABLE={output.flags.writeable}",
                        flush=True,
                    )

                # No writable NumPy view may survive into send().  Release the
                # input borrow before publishing the output loan.
                del indices
                del output
                del image
                del frame
                loan.send()

                processed += 1
                print(
                    f"DETECTOR SAMPLE component={component} "
                    f"process_instance={process_instance} activation={activation} "
                    f"input_capability={INPUT_CAPABILITY} "
                    f"output_capability={OUTPUT_CAPABILITY} model_id={model_id} "
                    f"activation_count={activation_count} "
                    f"processed_sequence={sequence} checksum={point_checksum} "
                    f"image_checksum={image_checksum} points={OUTPUT_POINT_COUNT} "
                    f"native_readiness_events={wait_stats_after[0]} "
                    f"scheduled_readiness_callbacks={wait_stats_after[1]} "
                    f"readiness_latency_ns={readiness_latency_ns}",
                    flush=True,
                )

                if self.fail_after is not None and processed >= self.fail_after:
                    raise RuntimeError(
                        f"test critical failure after processing {processed} frames"
                    )
                # The full lifecycle harness keeps the first NumPy alias
                # across deactivation.  Sensor's built-in max_borrowed=1
                # means a second receive would correctly fail while that
                # alias is alive, so suspend this test-only activation until
                # native cancellation wakes it.  Later activations follow
                # the normal receive loop.
                if retained_this_activation:
                    await asyncio.Event().wait()
                await _bounded_work(self.work_ms)
        except asyncio.CancelledError:
            # Keep ordinary asyncio cancellation visible to the native task
            # supervisor; activation cancellation is classified there.
            raise
        finally:
            print(
                f"DETECTOR CANCELLED component={component} "
                f"process_instance={process_instance} activation={activation} "
                f"monotonic_ns={time.monotonic_ns()} processed={processed}",
                flush=True,
            )
            self._finish_async_task(
                activation,
                "inference",
                loop_id,
                loop_thread_id,
                coroutine_id,
                coroutine_generation,
            )

    async def _background(
        self,
        task: rbt.TaskContext,
        component: str,
        process_instance: str,
        activation: int,
        model_id: int | None,
    ) -> None:
        """Run a cooperative background hook; its failure is non-critical."""
        iterations = 0
        loop_id, loop_thread_id, coroutine_id, coroutine_generation = (
            self._register_async_task(activation, "health")
        )
        try:
            if self.background_fail_after is None:
                # A healthy background task is fully event-driven.  Task
                # cancellation wakes this Event wait; no timer tick is needed.
                blocker = asyncio.Event()
                await blocker.wait()
                return
            while not task.cancelled:
                if (
                    self.background_fail_after is not None
                    and iterations >= self.background_fail_after
                ):
                    print(
                        f"DETECTOR BACKGROUND FAILURE component={component} "
                        f"process_instance={process_instance} activation={activation} "
                        f"model_id={model_id} iterations={iterations}",
                        flush=True,
                    )
                    raise RuntimeError(
                        "test background failure after " f"{iterations} iterations"
                    )
                iterations += 1
                await asyncio.sleep(0)
        except asyncio.CancelledError:
            raise
        finally:
            print(
                f"DETECTOR BACKGROUND CANCELLED component={component} "
                f"process_instance={process_instance} activation={activation} "
                f"monotonic_ns={time.monotonic_ns()} iterations={iterations}",
                flush=True,
            )
            self._finish_async_task(
                activation,
                "health",
                loop_id,
                loop_thread_id,
                coroutine_id,
                coroutine_generation,
            )


def _read_config(path: str | None) -> dict[str, Any]:
    if path is None:
        return {}
    with open(path, encoding="utf-8") as config_file:
        config = json.load(config_file)
    if not isinstance(config, Mapping):
        raise ValueError("component config must be a JSON object")
    return dict(config)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--composer-address",
        help="Composer endpoint (defaults to RBT_COMPOSER_ADDR or 127.0.0.1:7447)",
    )
    parser.add_argument(
        "--config",
        metavar="PATH",
        help="JSON object passed to Detector.prepare()",
    )
    args = parser.parse_args(argv)
    return int(
        rbt.run_component(
            Detector(),
            component_id=COMPONENT_ID,
            requirements=REQUIREMENTS,
            config=_read_config(args.config),
            composer_address=args.composer_address,
        )
        or 0
    )


if __name__ == "__main__":
    raise SystemExit(main())

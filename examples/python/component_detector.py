#!/usr/bin/env python3
"""Deterministic Image-to-PointCloud Python Component example.

The example deliberately uses only the public :mod:`rbt` Component API.  The
model is a tiny process-lifetime object; the NumPy views are the actual
received/loaned buffers and are never copied.
"""

from __future__ import annotations

import argparse
import json
import os
import time
from collections.abc import Mapping
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
    # POINT_XYZI_DTYPE is four little-endian float32 values (16 bytes).  A
    # dtype view changes interpretation only; it does not copy the loaned
    # payload.  Keep this view alive only until the writable loan is sent.
    words = points.view(np.uint32).reshape(OUTPUT_POINT_COUNT, 4)
    checksum = 0
    for point_words in words:
        point_checksum = int(point_words[0])
        for word in point_words[1:]:
            point_checksum = (point_checksum * 31 + int(word)) & _U64_MASK
        checksum = (checksum + point_checksum) & _U64_MASK
    return checksum


def _bounded_work(task: rbt.TaskContext, work_ms: int | None) -> None:
    """Simulate bounded application work while remaining cancellable."""
    if work_ms is None or work_ms == 0:
        return
    deadline = time.monotonic() + work_ms / 1000.0
    while not task.cancelled:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return
        time.sleep(min(remaining, 0.001))


class Detector(rbt.Component):
    """A small synchronous detector used by the mixed-language example."""

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

    def prepare(self, config: Any) -> None:
        """Load the tiny model once for the lifetime of this component."""
        del config
        self.model = object()
        self.model_id = id(self.model)
        self.model_load_count += 1
        self.fail_after = _optional_nonnegative_int("RBT_TEST_PY_FAIL_AFTER")
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
        """Resolve the input and create activation-scoped output/tasks."""
        if self.old_camera is not None:
            try:
                self.old_camera.try_receive()
            except rbt.ActivationExpiredError:
                print(
                    f"STALE HANDLE REJECTED component={ctx.component_id} "
                    f"activation={ctx.activation_id}",
                    flush=True,
                )
            else:
                raise RuntimeError("old activation camera handle remained usable")
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

        ctx.spawn_critical(
            "inference",
            lambda task: self._inference(
                task,
                camera,
                detections,
                component,
                process_instance,
                activation,
                model_id,
                activation_count,
            ),
        )
        ctx.spawn_background(
            "health",
            lambda task: self._background(
                task, component, process_instance, activation, model_id
            ),
        )

    def _inference(
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
        if self.fail_after == 0:
            raise RuntimeError("test critical failure after processing 0 frames")

        while not task.cancelled:
            # This is intentionally the blocking public receive call.  The
            # native activation wait wakes it when cooperative cancellation
            # starts.
            frame = camera.receive()
            if task.cancelled:
                break
            if frame is None:
                continue

            image = frame.numpy()
            image_checksum = int(image.sum(dtype=np.uint64))
            if self.retain_input_view and not RETAINED_INPUT_VIEWS:
                RETAINED_INPUT_VIEWS.append(image)
                retained_this_activation = True
                print(
                    f"RETAINED INPUT VIEW component={component} "
                    f"activation={activation} pointer={image.ctypes.data}",
                    flush=True,
                )
            sequence = frame.sequence
            timestamp_ns = sequence * 1_000_000

            # The deterministic model emits four PointXYZI values.  The
            # points are filled directly in the writable shared-memory loan.
            loan = detections.loan(
                OUTPUT_POINT_COUNT,
                sequence=sequence,
                timestamp_ns=timestamp_ns,
                dense=True,
            )
            output = loan.numpy()
            indices = np.arange(OUTPUT_POINT_COUNT, dtype=np.float32)
            output["x"] = indices
            output["y"] = indices * np.float32(2.0)
            output["z"] = -indices
            output["intensity"] = np.mod(indices, np.float32(31.0))
            point_checksum = _point_checksum(output)

            # No writable NumPy view may survive into send().  Also release
            # the input borrow before publishing the output.
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
                f"image_checksum={image_checksum} points={OUTPUT_POINT_COUNT}",
                flush=True,
            )

            if self.fail_after is not None and processed >= self.fail_after:
                raise RuntimeError(
                    f"test critical failure after processing {processed} frames"
                )
            # The full lifecycle harness deliberately keeps the first NumPy
            # alias alive across deactivation.  The built-in Sensor ceiling is
            # max_borrowed=1, so wait cooperatively for that deactivation
            # instead of issuing an invalid second receive while the alias is
            # retained.  Later activations continue through the normal loop.
            if retained_this_activation:
                while not task.cancelled:
                    time.sleep(0.001)
                break
            _bounded_work(task, self.work_ms)

        print(
            f"DETECTOR CANCELLED component={component} "
            f"process_instance={process_instance} activation={activation} "
            f"monotonic_ns={time.monotonic_ns()} processed={processed}",
            flush=True,
        )

    def _background(
        self,
        task: rbt.TaskContext,
        component: str,
        process_instance: str,
        activation: int,
        model_id: int | None,
    ) -> None:
        """Run a cooperative background hook; its failure is non-critical."""
        iterations = 0
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
            time.sleep(0.001)
        print(
            f"DETECTOR BACKGROUND CANCELLED component={component} "
            f"process_instance={process_instance} activation={activation} "
            f"monotonic_ns={time.monotonic_ns()} iterations={iterations}",
            flush=True,
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

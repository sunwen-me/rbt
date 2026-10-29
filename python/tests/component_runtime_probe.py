#!/usr/bin/env python3
"""Small public-API component probes used by the lifecycle shell harness."""

from __future__ import annotations

import argparse
import gc
import threading
import time
import weakref
from typing import Any

import rbt


_STORM_TASKS = 32


class _FrameSentinel:
    pass


class Probe(rbt.Component):
    def __init__(self, mode: str, component_id: str) -> None:
        self.mode = mode
        self.component_id = component_id
        self.requirements = (
            {"camera": "Camera"}
            if mode in {
                "require-only",
                "critical-sibling",
                "compute",
                "loan-cancel",
            }
            else {}
        )
        self.prepare_count = 0
        self.model: object | None = None
        self._storm_lock = threading.Lock()
        self._storm_raised = 0
        self._storm_released = 0

    def prepare(self, config: Any) -> None:
        del config
        self.prepare_count += 1
        self.model = object()
        print(
            f"PROBE PREPARED mode={self.mode} component={self.component_id} "
            f"prepare_count={self.prepare_count} model_id={id(self.model)}",
            flush=True,
        )
        if self.mode == "prepare-fail":
            raise RuntimeError("intentional probe preparation failure")

    def activate(self, ctx: rbt.ActivationContext) -> None:
        print(
            f"PROBE ACTIVATED mode={self.mode} component={ctx.component_id} "
            f"process_instance={ctx.process_instance_id} "
            f"activation={ctx.activation_id}",
            flush=True,
        )
        if self.mode == "no-stream":
            ctx.spawn_critical("idle", self._idle)
            return
        if self.mode == "critical-return":
            ctx.spawn_critical("returns", self._return_normally)
            return
        if self.mode == "critical-system-exit":
            ctx.spawn_critical("system-exit", self._raise_system_exit)
            return
        if self.mode == "background-exception-storm":
            for index in range(_STORM_TASKS):
                ctx.spawn_background(
                    f"storm-{index}",
                    lambda task, index=index: self._storm_failure(task, index),
                )
            ctx.spawn_critical("storm-monitor", self._storm_monitor)
            return
        if self.mode == "provide-only":
            ctx.provide_point_cloud("Detections", maximum_points=4)
            ctx.spawn_critical("idle", self._idle)
            return
        if self.mode == "partial-fail":
            ctx.provide_point_cloud("Detections", maximum_points=4)
            ctx.spawn_background("never-started", self._unexpected_start)
            raise RuntimeError("intentional partial activation failure")
        if self.mode == "require-only":
            camera = ctx.require("camera")
            ctx.spawn_critical(
                "receive", lambda task: self._receive(task, camera, "require-only")
            )
            return
        if self.mode == "critical-sibling":
            camera = ctx.require("camera")
            entered_wait = threading.Event()
            ctx.spawn_critical(
                "blocked-sibling",
                lambda task: self._blocked_sibling(task, camera, entered_wait),
            )
            ctx.spawn_critical(
                "failing-sibling",
                lambda task: self._failing_sibling(task, entered_wait),
            )
            return
        if self.mode == "compute":
            ctx.require("camera")
            ctx.spawn_critical("compute", self._compute)
            return
        if self.mode == "loan-cancel":
            camera = ctx.require("camera")
            output = ctx.provide_point_cloud("ProbeOutput", maximum_points=4)
            ctx.spawn_critical(
                "loan-holder",
                lambda task: self._hold_loan(task, camera, output),
            )
            return
        raise ValueError(f"unsupported probe mode {self.mode!r}")

    @staticmethod
    def _idle(task: rbt.TaskContext) -> None:
        while not task.cancelled:
            time.sleep(0.005)

    @staticmethod
    def _return_normally(task: rbt.TaskContext) -> None:
        del task
        print("PROBE CRITICAL RETURN", flush=True)

    @staticmethod
    def _raise_system_exit(task: rbt.TaskContext) -> None:
        del task
        raise SystemExit("intentional worker SystemExit")

    def _record_storm_release(self) -> None:
        with self._storm_lock:
            self._storm_released += 1

    def _storm_failure(self, task: rbt.TaskContext, index: int) -> None:
        del task
        sentinel = _FrameSentinel()
        weakref.finalize(sentinel, self._record_storm_release)
        with self._storm_lock:
            self._storm_raised += 1
        raise RuntimeError(f"intentional background storm failure {index}")

    def _storm_monitor(self, task: rbt.TaskContext) -> None:
        deadline = time.monotonic() + 10
        while not task.cancelled:
            gc.collect()
            with self._storm_lock:
                raised = self._storm_raised
                released = self._storm_released
            if raised == _STORM_TASKS and released == _STORM_TASKS:
                print(
                    f"PROBE BACKGROUND STORM raised={raised} released={released}",
                    flush=True,
                )
                while not task.cancelled:
                    time.sleep(0.005)
                return
            if time.monotonic() >= deadline:
                raise RuntimeError(
                    "background exception tracebacks retained frame-local objects: "
                    f"raised={raised} released={released}"
                )
            time.sleep(0.01)

    @staticmethod
    def _unexpected_start(task: rbt.TaskContext) -> None:
        del task
        print("PROBE ERROR staged task started after activation failure", flush=True)

    @staticmethod
    def _receive(
        task: rbt.TaskContext, camera: rbt.ImageSubscriber, label: str
    ) -> None:
        while not task.cancelled:
            sample = camera.receive()
            if sample is not None:
                print(
                    f"PROBE SAMPLE label={label} sequence={sample.sequence} "
                    f"pointer={sample._debug_payload_address()}",
                    flush=True,
                )
                del sample
        print(
            f"PROBE RECEIVE CANCELLED label={label} "
            f"monotonic_ns={time.monotonic_ns()}",
            flush=True,
        )

    @staticmethod
    def _blocked_sibling(
        task: rbt.TaskContext,
        camera: rbt.ImageSubscriber,
        entered_wait: threading.Event,
    ) -> None:
        entered_wait.set()
        while not task.cancelled:
            camera.receive()
        print(
            "PROBE SIBLING CANCELLED "
            f"monotonic_ns={time.monotonic_ns()}",
            flush=True,
        )

    @staticmethod
    def _failing_sibling(
        task: rbt.TaskContext, entered_wait: threading.Event
    ) -> None:
        del task
        if not entered_wait.wait(timeout=5):
            raise RuntimeError("blocked sibling did not enter its receive")
        time.sleep(0.05)
        raise RuntimeError("intentional critical sibling failure")

    @staticmethod
    def _compute(task: rbt.TaskContext) -> None:
        iterations = 0
        while not task.cancelled:
            # A bounded pure-Python chunk demonstrates flag visibility without
            # claiming forceful preemption of arbitrary application code.
            deadline = time.monotonic_ns() + 2_000_000
            while time.monotonic_ns() < deadline:
                iterations += 1
        print(
            f"PROBE COMPUTE CANCELLED iterations={iterations} "
            f"monotonic_ns={time.monotonic_ns()}",
            flush=True,
        )

    @staticmethod
    def _hold_loan(
        task: rbt.TaskContext,
        camera: rbt.ImageSubscriber,
        output: rbt.PointCloudPublisher,
    ) -> None:
        frame = None
        while not task.cancelled and frame is None:
            frame = camera.receive()
        if frame is None:
            return
        loan = output.loan(4, sequence=frame.sequence)
        view = loan.numpy()
        view["x"] = 1.0
        print(
            f"PROBE HOLDING WRITABLE LOAN pointer={view.ctypes.data} "
            f"sequence={frame.sequence}",
            flush=True,
        )
        del frame
        while not task.cancelled:
            time.sleep(0.001)
        del view
        del loan
        print(
            f"PROBE UNSENT LOAN DROPPED monotonic_ns={time.monotonic_ns()}",
            flush=True,
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "mode",
        choices=(
            "no-stream",
            "critical-return",
            "critical-system-exit",
            "background-exception-storm",
            "provide-only",
            "partial-fail",
            "prepare-fail",
            "require-only",
            "critical-sibling",
            "compute",
            "loan-cancel",
        ),
    )
    parser.add_argument("--component-id")
    parser.add_argument("--composer-address")
    args = parser.parse_args()
    component_id = args.component_id or f"python-{args.mode}"
    component = Probe(args.mode, component_id)
    rbt.run_component(
        component,
        component_id=component_id,
        requirements=component.requirements,
        config={"probe": args.mode},
        composer_address=args.composer_address,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

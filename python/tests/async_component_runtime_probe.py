#!/usr/bin/env python3
"""Focused public-API probes for the Python asyncio Component adapter."""

from __future__ import annotations

import argparse
import asyncio
import gc
import threading
import time
import weakref
from typing import Any

import rbt


_STORM_TASKS = 32
_TIMEOUT_ATTEMPTS = 3
_TIMEOUT_SECONDS = 0.01
_SLOW_CONSUMER_SECONDS = 0.2


class _FrameSentinel:
    pass


def _debug_async_wait_stats(camera: Any) -> tuple[int, int, int]:
    """Read readiness-only counters; no sample or payload is retained."""
    native = getattr(camera, "_native", camera)
    debug_stats = getattr(native, "_debug_async_wait_stats", None)
    if not callable(debug_stats):
        return 0, 0, 0
    try:
        events, callbacks, last_readiness_unix_ns = debug_stats()
        return int(events), int(callbacks), int(last_readiness_unix_ns)
    except (TypeError, ValueError):
        return 0, 0, 0


_ALIASES = {
    # The short names retain compatibility with component_runtime_probe.py.
    "no-stream": "minimal-async-cancel",
    "critical-return": "critical-normal-return",
    "critical-system-exit": "critical-async-system-exit",
    "background-exception-storm": "background-async-exception-storm",
    "require-only": "async-receive-loop",
    "critical-sibling": "sync-critical-async-sibling",
    "compute": "sync-compute",
    "loan-cancel": "unsent-loan-cancel",
    # Readable spellings used by the async acceptance harness.
    "async-cancel": "minimal-async-cancel",
    "minimal-async-cancellation": "minimal-async-cancel",
    "critical-normal-return": "critical-normal-return",
    "critical-async-error": "critical-async-exception",
    "background-async-error": "background-async-exception",
    "background-normal-return": "background-normal-return",
    "background-async-storm": "background-async-exception-storm",
    "async-provider-cancel": "async-receive-provider-cancel",
    "async-receive-provider-cancellation": "async-receive-provider-cancel",
    "receive-provider-cancel": "async-receive-provider-cancel",
    "repeated-receive-timeouts": "async-receive-timeouts",
    "receive-timeouts": "async-receive-timeouts",
    "background-task-handle-cancel": "background-handle-cancel",
    "critical-task-handle-cancel": "critical-handle-cancel",
    "sync-critical-cancels-async-receive": "sync-critical-async-sibling",
    "async-critical-cancels-sync-receive": "async-critical-sync-sibling",
    "unsent-loan-cancellation": "unsent-loan-cancel",
    # Focused cancellation, timeout, sibling, and readiness stress names.
    "async-user-cancel": "background-handle-cancel",
    "user-background-cancel": "background-handle-cancel",
    "async-critical-user-cancel": "critical-handle-cancel",
    "user-critical-cancel": "critical-handle-cancel",
    "repeated-timeouts": "async-receive-timeouts",
    "async-timeout": "async-receive-timeouts",
    "sync-async-sibling": "sync-critical-async-sibling",
    "async-sync-sibling": "async-critical-sync-sibling",
    "slow-consumer": "async-slow-consumer",
    "async-slow-sensor-consumer": "async-slow-consumer",
    "wake-storm": "async-wake-storm",
    "async-wake-storm": "async-wake-storm",
    "cancellation-finally": "async-cancellation-finally",
    "async-cancel-finally": "async-cancellation-finally",
    "async-receive-user-cancel": "async-receive-user-cancel",
    "receive-user-cancel": "async-receive-user-cancel",
    "stalled-consumer": "async-stalled-consumer",
    "async-stalled-consumer": "async-stalled-consumer",
}


def _canonical_mode(mode: str) -> str:
    mode = mode.strip().lower().replace("_", "-")
    mode = _ALIASES.get(mode, mode)
    valid = {
        "minimal-async-cancel",
        "critical-normal-return",
        "critical-async-exception",
        "critical-async-system-exit",
        "background-async-exception",
        "background-normal-return",
        "background-async-exception-storm",
        "async-receive-provider-cancel",
        "async-receive-timeouts",
        "background-handle-cancel",
        "critical-handle-cancel",
        "sync-critical-async-sibling",
        "async-critical-sync-sibling",
        "unsent-loan-cancel",
        "async-slow-consumer",
        "async-wake-storm",
        "async-cancellation-finally",
        "async-receive-user-cancel",
        "async-stalled-consumer",
        "provider-idle-camera",
        "sync-idle-receive",
        "async-multi-source",
        "async-stale-completion",
        # Small compatibility probes from the synchronous fixture.
        "async-receive-loop",
        "sync-compute",
        "provide-only",
        "partial-fail",
        "prepare-fail",
    }
    if mode not in valid:
        raise ValueError(f"unsupported probe mode {mode!r}")
    return mode


class Probe(rbt.Component):
    """A compact Component exercising async task and receive boundaries."""

    def __init__(self, mode: str, component_id: str) -> None:
        self.mode = mode
        self.component_id = component_id
        if mode == "async-multi-source":
            self.requirements = {
                "camera": "Camera",
                "detections": "Detections",
            }
        elif mode in {
            "async-receive-provider-cancel",
            "async-receive-timeouts",
            "async-receive-loop",
            "sync-critical-async-sibling",
            "async-critical-sync-sibling",
            "unsent-loan-cancel",
            "async-slow-consumer",
            "async-wake-storm",
            "async-receive-user-cancel",
            "async-stalled-consumer",
            "sync-compute",
            "sync-idle-receive",
            "async-stale-completion",
        }:
            self.requirements = {"camera": "Camera"}
        else:
            self.requirements = {}
        self.prepare_count = 0
        self.activation_count = 0
        self.model: object | None = None
        self._background_handle: rbt.AsyncTaskHandle | None = None
        self._critical_handle: rbt.AsyncTaskHandle | None = None
        self._receive_handle: rbt.AsyncTaskHandle | None = None
        self._storm_lock = threading.Lock()
        self._storm_raised = 0
        self._storm_released = 0
        self._storm_release_event: asyncio.Event | None = None

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
        self.activation_count += 1
        print(
            f"PROBE ACTIVATED mode={self.mode} component={ctx.component_id} "
            f"process_instance={ctx.process_instance_id} "
            f"activation={ctx.activation_id} activation_count={self.activation_count} "
            f"monotonic_ns={time.monotonic_ns()}",
            flush=True,
        )

        if self.mode == "minimal-async-cancel":
            ctx.spawn_critical_async("minimal", self._minimal_cancel)
            return
        if self.mode == "critical-normal-return":
            ctx.spawn_critical_async("returns", self._critical_return)
            return
        if self.mode == "critical-async-system-exit":
            ctx.spawn_critical_async("system-exit", self._critical_system_exit)
            return
        if self.mode == "critical-async-exception":
            ctx.spawn_critical_async("async-failing", self._critical_failure)
            return
        if self.mode == "background-async-exception":
            ctx.spawn_background_async("async-background-failing", self._background_failure)
            ctx.spawn_critical_async("background-holder", self._minimal_cancel)
            return
        if self.mode == "background-normal-return":
            ctx.spawn_background_async("background-return", self._background_return)
            ctx.spawn_critical_async("background-return-holder", self._minimal_cancel)
            return
        if self.mode == "background-async-exception-storm":
            for index in range(_STORM_TASKS):
                ctx.spawn_background_async("storm-%d" % index, self._storm_failure, index)
            ctx.spawn_critical_async("storm-monitor", self._storm_monitor)
            return
        if self.mode == "async-receive-provider-cancel":
            camera = ctx.require("camera")
            ctx.spawn_critical_async(
                "receive-provider-cancel",
                self._receive_once,
                camera,
                "provider-cancel",
            )
            return
        if self.mode == "async-receive-timeouts":
            camera = ctx.require("camera")
            ctx.spawn_background_async("receive-timeouts", self._receive_timeouts, camera)
            return
        if self.mode == "async-slow-consumer":
            camera = ctx.require("camera")
            ctx.spawn_background_async("slow-consumer", self._slow_consumer, camera)
            return
        if self.mode == "async-wake-storm":
            camera = ctx.require("camera")
            ctx.spawn_background_async("wake-storm", self._wake_storm, camera)
            return
        if self.mode == "async-cancellation-finally":
            ctx.spawn_background_async(
                "cancellation-finally", self._cancellation_finally
            )
            return
        if self.mode == "async-receive-user-cancel":
            camera = ctx.require("camera")
            entered = asyncio.Event()
            self._receive_handle = ctx.spawn_background_async(
                "user-receive", self._user_cancel_receive, camera, entered
            )
            ctx.spawn_background_async(
                "user-receive-canceller",
                self._cancel_receive_handle,
                camera,
                entered,
            )
            ctx.spawn_critical_async("user-receive-holder", self._minimal_cancel)
            return
        if self.mode == "async-stalled-consumer":
            ctx.require("camera")
            ctx.spawn_background_async("stalled-consumer", self._stalled_consumer)
            ctx.spawn_critical_async("stalled-holder", self._minimal_cancel)
            return
        if self.mode == "provider-idle-camera":
            ctx.provide_image("Camera", maximum_payload_bytes=64)
            ctx.spawn_critical_async("idle-provider", self._minimal_cancel)
            return
        if self.mode == "sync-idle-receive":
            camera = ctx.require("camera")
            ctx.spawn_critical(
                "sync-idle-receive",
                lambda task: self._sync_idle_receive(task, camera),
            )
            return
        if self.mode == "async-multi-source":
            camera = ctx.require("camera")
            detections = ctx.require("detections")
            ctx.spawn_background_async(
                "camera-receive", self._receive_loop, camera, "camera"
            )
            ctx.spawn_background_async(
                "detections-receive", self._receive_loop, detections, "detections"
            )
            ctx.spawn_critical_async("multi-source-holder", self._minimal_cancel)
            return
        if self.mode == "async-stale-completion":
            camera = ctx.require("camera")
            ctx.spawn_critical_async(
                "stale-current-receive",
                self._receive_loop,
                camera,
                "stale-current",
            )
            if self.activation_count == 1:
                ctx.spawn_background_async(
                    "late-stale-completion",
                    self._late_stale_completion,
                    ctx.activation_id,
                    self.activation_count,
                )
            return
        if self.mode == "background-handle-cancel":
            self._background_handle = ctx.spawn_background_async(
                "user-background", self._background_handle_cancel
            )
            return
        if self.mode == "critical-handle-cancel":
            self._critical_handle = ctx.spawn_critical_async(
                "user-critical", self._critical_handle_cancel
            )
            return
        if self.mode == "sync-critical-async-sibling":
            camera = ctx.require("camera")
            entered_wait = threading.Event()
            ctx.spawn_critical_async(
                "async-receive-sibling",
                self._async_receive_sibling,
                camera,
                entered_wait,
            )
            ctx.spawn_critical(
                "sync-failing-sibling",
                lambda task: self._sync_failing_sibling(task, entered_wait),
            )
            return
        if self.mode == "async-critical-sync-sibling":
            camera = ctx.require("camera")
            ctx.spawn_critical(
                "sync-receive-sibling",
                lambda task: self._sync_receive_sibling(task, camera),
            )
            ctx.spawn_critical_async("async-failing-sibling", self._critical_failure_after_yield)
            return
        if self.mode == "unsent-loan-cancel":
            camera = ctx.require("camera")
            output = ctx.provide_point_cloud("ProbeOutput", maximum_points=4)
            ctx.spawn_critical_async("loan-holder", self._hold_loan, camera, output)
            return

        # Compatibility modes used by the existing Python Component harness.
        if self.mode == "async-receive-loop":
            camera = ctx.require("camera")
            ctx.spawn_critical_async("receive", self._receive_loop, camera, "require-only")
            return
        if self.mode == "sync-compute":
            ctx.require("camera")
            ctx.spawn_critical("compute", self._compute)
            return
        if self.mode == "provide-only":
            ctx.provide_point_cloud("Detections", maximum_points=4)
            ctx.spawn_critical_async("idle", self._minimal_cancel)
            return
        if self.mode == "partial-fail":
            ctx.provide_point_cloud("Detections", maximum_points=4)
            ctx.spawn_background_async("never-started", self._unexpected_start)
            raise RuntimeError("intentional partial activation failure")
        raise ValueError(f"unsupported probe mode {self.mode!r}")

    async def _minimal_cancel(self, task: rbt.TaskContext) -> None:
        del task
        blocker = asyncio.Event()
        try:
            await blocker.wait()
        except asyncio.CancelledError:
            raise
        finally:
            print(
                f"PROBE ASYNC CANCELLED mode={self.mode} "
                f"monotonic_ns={time.monotonic_ns()}",
                flush=True,
            )

    async def _critical_return(self, task: rbt.TaskContext) -> None:
        del task
        print(f"PROBE CRITICAL RETURN mode={self.mode} async=1", flush=True)

    async def _critical_system_exit(self, task: rbt.TaskContext) -> None:
        del task
        raise SystemExit("intentional worker SystemExit (async)")

    async def _critical_failure(self, task: rbt.TaskContext) -> None:
        del task
        raise RuntimeError("intentional async critical failure")

    async def _background_failure(self, task: rbt.TaskContext) -> None:
        del task
        raise RuntimeError("intentional async background failure")

    async def _background_return(self, task: rbt.TaskContext) -> None:
        del task
        print("PROBE BACKGROUND NORMAL RETURN", flush=True)

    async def _user_cancel_receive(
        self,
        task: rbt.TaskContext,
        camera: rbt.ImageSubscriber,
        entered: asyncio.Event,
    ) -> None:
        del task
        attempt = 0
        try:
            while True:
                attempt += 1
                print(
                    f"PROBE ASYNC USER RECEIVE WAITING attempt={attempt}", flush=True
                )
                entered.set()
                sample = await camera.receive_async()
                del sample
        except asyncio.CancelledError:
            print("PROBE ASYNC USER RECEIVE CANCELLED", flush=True)
            raise
        finally:
            print(
                "PROBE ASYNC USER RECEIVE FINALLY "
                f"monotonic_ns={time.monotonic_ns()}",
                flush=True,
            )

    async def _cancel_receive_handle(
        self,
        task: rbt.TaskContext,
        camera: rbt.ImageSubscriber,
        entered: asyncio.Event,
    ) -> None:
        del task
        await entered.wait()
        # Let the receiver enter receive_async() before cancellation is queued.
        await asyncio.sleep(0)
        handle = self._receive_handle
        if handle is None:
            raise RuntimeError("async receive task handle was not retained")
        print(
            f"PROBE ASYNC USER RECEIVE CANCEL REQUEST monotonic_ns={time.monotonic_ns()}",
            flush=True,
        )
        handle.cancel()
        while not handle.done:
            await asyncio.sleep(0)
        sample = await camera.receive_async(timeout=2.0)
        if sample is None:
            raise RuntimeError("subscriber did not recover after user Task.cancel()")
        print(
            f"PROBE ASYNC USER RECEIVE RECOVERED sequence={sample.sequence} "
            f"pointer={sample._debug_payload_address()}",
            flush=True,
        )
        del sample

    async def _stalled_consumer(self, task: rbt.TaskContext) -> None:
        del task
        blocker = asyncio.Event()
        try:
            print("PROBE ASYNC STALLED CONSUMER STARTED receive_calls=0", flush=True)
            await blocker.wait()
        except asyncio.CancelledError:
            raise
        finally:
            print("PROBE ASYNC STALLED CONSUMER FINALLY receive_calls=0", flush=True)

    async def _late_stale_completion(
        self,
        task: rbt.TaskContext,
        activation_id: int,
        activation_count: int,
    ) -> None:
        del task
        blocker = asyncio.Event()
        print(
            f"PROBE STALE TASK STARTED activation={activation_id} "
            f"activation_count={activation_count}",
            flush=True,
        )
        try:
            await blocker.wait()
        except asyncio.CancelledError:
            print(
                f"PROBE STALE TASK CAUGHT CANCELLATION activation={activation_id} "
                f"activation_count={activation_count} "
                f"monotonic_ns={time.monotonic_ns()}",
                flush=True,
            )
            # Intentionally outlive EffectScope's 100 ms async-wrapper grace.
            # The eventual background completion belongs to activation 1 and
            # must not affect a replacement activation which is already live.
            await asyncio.sleep(0.5)
            print(
                f"PROBE STALE TASK LATE COMPLETION activation={activation_id} "
                f"activation_count={activation_count} "
                f"monotonic_ns={time.monotonic_ns()}",
                flush=True,
            )

    def _record_storm_release(self) -> None:
        with self._storm_lock:
            self._storm_released += 1
            released = self._storm_released
        if released == _STORM_TASKS and self._storm_release_event is not None:
            try:
                self._storm_release_event.set()
            except RuntimeError:
                # A process shutdown may close the managed loop just as the
                # final traceback-owned sentinel is collected.
                pass

    async def _storm_failure(self, task: rbt.TaskContext, index: int) -> None:
        del task
        if self._storm_release_event is None:
            self._storm_release_event = asyncio.Event()
        sentinel = _FrameSentinel()
        weakref.finalize(sentinel, self._record_storm_release)
        with self._storm_lock:
            self._storm_raised += 1
        raise RuntimeError(f"intentional async background storm failure {index}")

    async def _storm_monitor(self, task: rbt.TaskContext) -> None:
        del task
        if self._storm_release_event is None:
            self._storm_release_event = asyncio.Event()
        blocker = asyncio.Event()
        try:
            await self._storm_release_event.wait()
            with self._storm_lock:
                raised = self._storm_raised
                released = self._storm_released
            print(
                f"PROBE BACKGROUND STORM raised={raised} released={released}",
                flush=True,
            )
            await blocker.wait()
        except asyncio.CancelledError:
            raise
        finally:
            print(
                f"PROBE BACKGROUND STORM CANCELLED monotonic_ns={time.monotonic_ns()}",
                flush=True,
            )

    async def _receive_once(
        self,
        task: rbt.TaskContext,
        camera: rbt.ImageSubscriber,
        label: str,
    ) -> None:
        sample: rbt.ReceivedImageSample | None = None
        try:
            print(f"PROBE ASYNC RECEIVE WAITING label={label}", flush=True)
            while not task.cancelled:
                sample = await camera.receive_async()
                if sample is not None:
                    print(
                        f"PROBE ASYNC RECEIVE SAMPLE label={label} "
                        f"sequence={sample.sequence} "
                        f"pointer={sample._debug_payload_address()}",
                        flush=True,
                    )
                    del sample
                    sample = None
                elif task.cancelled:
                    print(f"PROBE RECEIVE CANCELLED label={label}", flush=True)
                    break
        except asyncio.CancelledError:
            raise
        finally:
            print(
                f"PROBE ASYNC RECEIVE CANCELLED label={label} "
                f"monotonic_ns={time.monotonic_ns()}",
                flush=True,
            )
            del sample

    async def _receive_timeouts(
        self, task: rbt.TaskContext, camera: rbt.ImageSubscriber
    ) -> None:
        timeout_count = 0
        try:
            for index in range(_TIMEOUT_ATTEMPTS):
                # Exercise timeout composition around the native await.  The
                # receive future is shielded by the adapter, so this only
                # cancels this wait and leaves the subscriber usable.
                try:
                    sample = await asyncio.wait_for(
                        camera.receive_async(), _TIMEOUT_SECONDS
                    )
                except asyncio.TimeoutError:
                    sample = None
                if sample is None:
                    timeout_count += 1
                    print(f"PROBE ASYNC TIMEOUT index={index}", flush=True)
                else:
                    print(
                        f"PROBE ASYNC TIMEOUT EARLY SAMPLE index={index} "
                        f"sequence={sample.sequence}",
                        flush=True,
                    )
                    del sample
            sample = await asyncio.wait_for(camera.receive_async(), 2.0)
            if sample is None:
                if task.cancelled:
                    return
                raise RuntimeError("async timeout probe did not receive a later sample")
            print(
                f"PROBE ASYNC TIMEOUT SUCCESS timeouts={timeout_count} "
                f"sequence={sample.sequence} pointer={sample._debug_payload_address()}",
                flush=True,
            )
            del sample
        except asyncio.CancelledError:
            raise
        finally:
            print(
                f"PROBE ASYNC TIMEOUTS FINALLY count={timeout_count} "
                f"monotonic_ns={time.monotonic_ns()}",
                flush=True,
            )

    async def _slow_consumer(
        self, task: rbt.TaskContext, camera: rbt.ImageSubscriber
    ) -> None:
        processed = 0
        sequence_gaps = 0
        max_sequence_gap = 0
        last_sequence: int | None = None
        try:
            print(
                f"PROBE ASYNC SLOW CONSUMER STARTED delay_s={_SLOW_CONSUMER_SECONDS}",
                flush=True,
            )
            while not task.cancelled:
                sample = await camera.receive_async()
                if sample is None:
                    break
                sequence = sample.sequence
                if last_sequence is not None and sequence > last_sequence + 1:
                    gap = sequence - last_sequence - 1
                    sequence_gaps += gap
                    max_sequence_gap = max(max_sequence_gap, gap)
                last_sequence = sequence
                processed += 1
                print(
                    f"PROBE ASYNC SLOW SAMPLE sequence={sequence} "
                    f"samples_processed={processed}",
                    flush=True,
                )
                del sample
                # This delay models a slow application consumer after the
                # direct receive; it is not receive polling or a payload queue.
                await asyncio.sleep(_SLOW_CONSUMER_SECONDS)
        except asyncio.CancelledError:
            raise
        finally:
            print(
                f"PROBE ASYNC SLOW CONSUMER FINALLY samples_processed={processed} "
                f"sequence_gaps={sequence_gaps} "
                f"max_sequence_gap={max_sequence_gap} backlog_bounded=1 "
                f"monotonic_ns={time.monotonic_ns()}",
                flush=True,
            )

    async def _wake_storm(
        self, task: rbt.TaskContext, camera: rbt.ImageSubscriber
    ) -> None:
        samples_processed = 0
        sequence_gaps = 0
        max_sequence_gap = 0
        last_sequence: int | None = None
        try:
            print("PROBE ASYNC WAKE STORM STARTED", flush=True)
            while not task.cancelled:
                sample = await camera.receive_async()
                events, callbacks, last_readiness_unix_ns = (
                    _debug_async_wait_stats(camera)
                )
                if sample is None:
                    break
                sequence = sample.sequence
                if last_sequence is not None and sequence > last_sequence + 1:
                    gap = sequence - last_sequence - 1
                    sequence_gaps += gap
                    max_sequence_gap = max(max_sequence_gap, gap)
                last_sequence = sequence
                samples_processed += 1
                print(
                    f"PROBE ASYNC WAKE STORM SAMPLE sequence={sequence} "
                    f"samples_processed={samples_processed} wake_count={events} "
                    f"native_readiness_events={events} "
                    f"scheduled_readiness_callbacks={callbacks} "
                    f"last_readiness_unix_ns={last_readiness_unix_ns}",
                    flush=True,
                )
                del sample
                await asyncio.sleep(_SLOW_CONSUMER_SECONDS)
        except asyncio.CancelledError:
            raise
        finally:
            events, callbacks, last_readiness_unix_ns = _debug_async_wait_stats(camera)
            print(
                f"PROBE ASYNC WAKE STORM FINALLY wake_count={events} "
                f"native_readiness_events={events} "
                f"scheduled_readiness_callbacks={callbacks} "
                f"samples_processed={samples_processed} "
                f"sequence_gaps={sequence_gaps} "
                f"max_sequence_gap={max_sequence_gap} "
                f"last_readiness_unix_ns={last_readiness_unix_ns} "
                f"backlog_bounded=1 monotonic_ns={time.monotonic_ns()}",
                flush=True,
            )

    async def _cancellation_finally(self, task: rbt.TaskContext) -> None:
        del task
        blocker = asyncio.Event()
        cancelled = False
        try:
            print("PROBE ASYNC CANCELLATION FINALLY STARTED", flush=True)
            await blocker.wait()
        except asyncio.CancelledError:
            cancelled = True
            raise
        finally:
            print(
                f"PROBE ASYNC CANCELLATION FINALLY ran=1 cancelled={int(cancelled)} "
                f"monotonic_ns={time.monotonic_ns()}",
                flush=True,
            )

    async def _background_handle_cancel(self, task: rbt.TaskContext) -> None:
        del task
        blocker = asyncio.Event()
        try:
            print("PROBE BACKGROUND HANDLE STARTED", flush=True)
            # Yield once so cancellation is delivered to a running coroutine,
            # rather than cancelling a staged task before it ever starts.
            await asyncio.sleep(0)
            handle = self._background_handle
            if handle is None:
                raise RuntimeError("background task handle was not retained")
            handle.cancel()
            await blocker.wait()
        except asyncio.CancelledError:
            print("PROBE BACKGROUND HANDLE CANCELLED", flush=True)
            raise
        finally:
            print(
                f"PROBE BACKGROUND HANDLE FINALLY monotonic_ns={time.monotonic_ns()}",
                flush=True,
            )

    async def _critical_handle_cancel(self, task: rbt.TaskContext) -> None:
        del task
        blocker = asyncio.Event()
        try:
            print("PROBE CRITICAL HANDLE STARTED", flush=True)
            await asyncio.sleep(0)
            handle = self._critical_handle
            if handle is None:
                raise RuntimeError("critical task handle was not retained")
            handle.cancel()
            await blocker.wait()
        except asyncio.CancelledError:
            print("PROBE CRITICAL HANDLE CANCELLED", flush=True)
            raise
        finally:
            print(
                f"PROBE CRITICAL HANDLE FINALLY monotonic_ns={time.monotonic_ns()}",
                flush=True,
            )

    async def _async_receive_sibling(
        self,
        task: rbt.TaskContext,
        camera: rbt.ImageSubscriber,
        entered_wait: threading.Event,
    ) -> None:
        entered_wait.set()
        try:
            while not task.cancelled:
                sample = await camera.receive_async()
                del sample
        except asyncio.CancelledError:
            raise
        finally:
            print(
                f"PROBE SIBLING CANCELLED async=1 monotonic_ns={time.monotonic_ns()}",
                flush=True,
            )

    @staticmethod
    def _sync_failing_sibling(
        task: rbt.TaskContext, entered_wait: threading.Event
    ) -> None:
        del task
        if not entered_wait.wait(timeout=5):
            raise RuntimeError("async sibling did not enter its receive")
        raise RuntimeError("intentional critical sibling failure (sync)")

    @staticmethod
    def _sync_receive_sibling(
        task: rbt.TaskContext, camera: rbt.ImageSubscriber
    ) -> None:
        try:
            while not task.cancelled:
                camera.receive()
        finally:
            print(
                f"PROBE SYNC WAIT CANCELLED monotonic_ns={time.monotonic_ns()}",
                flush=True,
            )

    @staticmethod
    def _sync_idle_receive(
        task: rbt.TaskContext, camera: rbt.ImageSubscriber
    ) -> None:
        try:
            print("PROBE SYNC IDLE RECEIVE STARTED", flush=True)
            while not task.cancelled:
                camera.receive()
        finally:
            print(
                f"PROBE SYNC IDLE RECEIVE FINALLY monotonic_ns={time.monotonic_ns()}",
                flush=True,
            )

    async def _critical_failure_after_yield(self, task: rbt.TaskContext) -> None:
        del task
        await asyncio.sleep(0)
        raise RuntimeError("intentional async critical sibling failure")

    async def _hold_loan(
        self,
        task: rbt.TaskContext,
        camera: rbt.ImageSubscriber,
        output: rbt.PointCloudPublisher,
    ) -> None:
        frame: rbt.ReceivedImageSample | None = None
        loan: rbt.LoanedPointCloudSample | None = None
        view: Any = None
        try:
            frame = await camera.receive_async()
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
            blocker = asyncio.Event()
            await blocker.wait()
        except asyncio.CancelledError:
            raise
        finally:
            # Release the writable alias before the unsent loan itself.  The
            # loan then returns to the transport pool without being published.
            del view
            del loan
            del frame
            print(
                f"PROBE UNSENT LOAN DROPPED monotonic_ns={time.monotonic_ns()}",
                flush=True,
            )

    async def _receive_loop(
        self,
        task: rbt.TaskContext,
        camera: rbt.ImageSubscriber,
        label: str,
    ) -> None:
        processed = 0
        try:
            while not task.cancelled:
                sample = await camera.receive_async()
                if sample is None:
                    break
                print(
                    f"PROBE SAMPLE label={label} sequence={sample.sequence} "
                    f"pointer={sample._debug_payload_address()}",
                    flush=True,
                )
                processed += 1
                del sample
        except asyncio.CancelledError:
            raise
        finally:
            print(
                f"PROBE RECEIVE CANCELLED label={label} "
                f"monotonic_ns={time.monotonic_ns()} processed={processed}",
                flush=True,
            )

    @staticmethod
    def _compute(task: rbt.TaskContext) -> None:
        iterations = 0
        try:
            while not task.cancelled:
                deadline = time.monotonic_ns() + 2_000_000
                while time.monotonic_ns() < deadline:
                    iterations += 1
        finally:
            print(
                f"PROBE COMPUTE CANCELLED iterations={iterations} "
                f"monotonic_ns={time.monotonic_ns()}",
                flush=True,
            )

    @staticmethod
    def _unexpected_start(task: rbt.TaskContext) -> None:
        del task
        print("PROBE ERROR staged task started after activation failure", flush=True)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode")
    parser.add_argument("--component-id")
    parser.add_argument("--composer-address")
    args = parser.parse_args(argv)
    mode = _canonical_mode(args.mode)
    component_id = args.component_id or f"python-{args.mode}"
    component = Probe(mode, component_id)
    rbt.run_component(
        component,
        component_id=component_id,
        requirements=component.requirements,
        config={"probe": mode},
        composer_address=args.composer_address,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

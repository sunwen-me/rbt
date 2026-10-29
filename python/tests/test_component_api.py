from __future__ import annotations

import asyncio
import inspect
from typing import Any

import pytest

import rbt


def _run_async(function: Any) -> Any:
    def wrapper(*args: Any, **kwargs: Any) -> Any:
        return asyncio.run(function(*args, **kwargs))

    return wrapper


class _NativeEndpoint:
    def __init__(self) -> None:
        self.wait_calls = 0
        self.cancelled = False

    def try_receive(self) -> None:
        return None

    def wait_receive(self, timeout: float) -> None:
        assert timeout > 0
        self.wait_calls += 1
        self.cancelled = True
        return None

    def activation_cancelled(self) -> bool:
        return self.cancelled

    def pressure(self) -> tuple[str, int, int, int, int]:
        return ("test", 0, 0, 0, 8)

    def wait_stats(self) -> tuple[int, int, int, int, int]:
        return (1, 1, 0, 1, 0)


class _NativeActivation:
    component_id = "detector"
    process_instance_id = "00000000-0000-0000-0000-000000000123"
    activation_id = 7
    cancelled = False

    def __init__(self) -> None:
        self.image_endpoint = _NativeEndpoint()
        self.pointcloud_publisher = object()
        self.spawned: list[tuple[str, str, Any]] = []

    def require(self, name: str) -> tuple[str, Any, str, str, str]:
        assert name == "camera"
        return (
            "image",
            self.image_endpoint,
            rbt.StreamDescriptor.image("bound-camera").to_json(),
            "camera-a",
            "00000000-0000-0000-0000-000000000456",
        )

    def provide_point_cloud(
        self, capability_type: str, *, maximum_points: int
    ) -> tuple[Any, str, str]:
        assert capability_type == "Detections"
        assert maximum_points == 4
        return (
            self.pointcloud_publisher,
            rbt.StreamDescriptor.point_cloud("provided-detections").to_json(),
            "00000000-0000-0000-0000-000000000789",
        )

    def spawn_critical(self, name: str, task: Any) -> None:
        self.spawned.append(("critical", name, task))

    def spawn_background(self, name: str, task: Any) -> None:
        self.spawned.append(("background", name, task))


class _SampleOwner:
    sequence = 11
    timestamp_ns = 22
    width = 0
    height = 0
    stride = 0
    encoding = int(rbt.PixelFormat.GRAY8)
    payload_nbytes = 0


class _AsyncEndpoint:
    def __init__(self) -> None:
        self.results: list[Any] = []
        self.waits: list[tuple[int, asyncio.Future[Any]]] = []
        self.finished: list[int] = []
        self.cancelled_generations: list[int] = []
        self.activation_expired = False
        self.next_generation = 1

    def try_receive(self) -> Any | None:
        return self.results.pop(0) if self.results else None

    def begin_receive_async(
        self, loop: asyncio.AbstractEventLoop, future: asyncio.Future[Any]
    ) -> int:
        generation = self.next_generation
        self.next_generation += 1
        self.waits.append((generation, future))
        return generation

    def finish_receive_async(self, generation: int) -> None:
        self.finished.append(generation)

    def cancel_receive_async(self, generation: int) -> None:
        self.cancelled_generations.append(generation)

    def activation_cancelled(self) -> bool:
        return self.activation_expired

    def async_receive_supported(self) -> bool:
        return True

    def wake(self, index: int = -1) -> None:
        _generation, future = self.waits[index]
        if not future.done():
            future.set_result(None)

    def pressure(self) -> tuple[str, int, int, int, int]:
        return ("async", 0, 0, 0, 8)

    def wait_stats(self) -> tuple[int, int, int, int, int]:
        return (0, 0, 0, 0, 0)


class _FailingAsyncEndpoint(_AsyncEndpoint):
    def finish_receive_async(self, generation: int) -> None:
        super().finish_receive_async(generation)
        raise rbt.WaitError("injected native WaitSet failure")


class _ExpiryRaceAsyncEndpoint(_AsyncEndpoint):
    def begin_receive_async(
        self, loop: asyncio.AbstractEventLoop, future: asyncio.Future[Any]
    ) -> int:
        del loop, future
        self.activation_expired = True
        raise rbt.ActivationExpiredError("injected activation-expiry race")


class _NativeAsyncHandle:
    def __init__(self) -> None:
        self.cancel_calls = 0
        self.cancelled = False
        self.done = False

    def cancel(self) -> str:
        self.cancel_calls += 1
        self.cancelled = True
        return "cancelled"


class _NativeAsyncActivation:
    def __init__(self) -> None:
        self.factories: list[tuple[str, Any]] = []
        self.handles: list[_NativeAsyncHandle] = []

    def spawn_critical_async(self, name: str, factory: Any) -> _NativeAsyncHandle:
        handle = _NativeAsyncHandle()
        self.factories.append((name, factory))
        self.handles.append(handle)
        return handle

    def spawn_background_async(self, name: str, factory: Any) -> _NativeAsyncHandle:
        handle = _NativeAsyncHandle()
        self.factories.append((name, factory))
        self.handles.append(handle)
        return handle


def test_component_exception_hierarchy_is_typed() -> None:
    assert issubclass(rbt.ComponentPreparationError, rbt.ComponentRuntimeError)
    assert issubclass(rbt.ComponentActivationError, rbt.ComponentRuntimeError)
    assert issubclass(rbt.TaskError, rbt.ComponentRuntimeError)
    assert issubclass(rbt.ActivationExpiredError, rbt.ComponentRuntimeError)


def test_activation_context_wraps_exact_guarded_native_endpoints() -> None:
    native = _NativeActivation()
    context = rbt.ActivationContext(native)

    camera = context.require("camera")
    output = context.provide_point_cloud("Detections", maximum_points=4)

    assert isinstance(camera, rbt.ImageSubscriber)
    assert camera._native is native.image_endpoint
    assert camera.descriptor.service_name == "bound-camera"
    assert camera.provider_component_id == "camera-a"
    assert camera.capability_instance_id.endswith("0456")
    assert camera.activation_id == 7

    assert isinstance(output, rbt.PointCloudPublisher)
    assert output._native is native.pointcloud_publisher
    assert output.descriptor.service_name == "provided-detections"
    assert output.provider_component_id == "detector"
    assert output.capability_instance_id.endswith("0789")
    assert output.activation_id == 7


def test_activation_cancelled_native_wait_returns_promptly() -> None:
    native = _NativeEndpoint()
    subscriber = rbt.ImageSubscriber(
        native, rbt.StreamDescriptor.image("cancel-wait")
    )

    assert subscriber.receive() is None
    assert native.wait_calls == 1
    assert subscriber.wait_stats.cancellation_wakeups == 1


def test_spawn_wraps_native_task_context_once() -> None:
    native = _NativeActivation()
    context = rbt.ActivationContext(native)
    seen: list[rbt.TaskContext] = []

    context.spawn_critical("inference", seen.append)
    context.spawn_background("metrics", seen.append)

    class NativeTask:
        cancelled = True

    for kind, name, task in native.spawned:
        assert kind in {"critical", "background"}
        assert name in {"inference", "metrics"}
        task(NativeTask())
    assert len(seen) == 2
    assert all(task.cancelled for task in seen)


@_run_async
async def test_spawn_async_factory_defers_coroutine_and_injects_context() -> None:
    native = _NativeAsyncActivation()
    context = rbt.ActivationContext(native)
    calls: list[tuple[rbt.TaskContext, tuple[Any, ...]]] = []

    async def run(task: rbt.TaskContext, *args: Any) -> tuple[bool, tuple[Any, ...]]:
        calls.append((task, args))
        return task.cancelled, args

    handle = context.spawn_critical_async("inference", run, "camera", 3)
    assert isinstance(handle, rbt.AsyncTaskHandle)
    assert len(native.factories) == 1
    assert calls == []

    class NativeTask:
        cancelled = True

    coroutine = native.factories[0][1](NativeTask())
    assert inspect.iscoroutine(coroutine)
    assert await coroutine == (True, ("camera", 3))
    assert calls[0][0].cancelled is True
    assert handle.cancel() is None
    assert handle.cancelled is True
    assert handle.done is False


@_run_async
async def test_spawn_async_contains_base_exception_with_original_diagnostic() -> None:
    native = _NativeAsyncActivation()
    context = rbt.ActivationContext(native)

    async def stop(task: rbt.TaskContext) -> None:
        del task
        raise SystemExit("contained")

    context.spawn_critical_async("stop", stop)

    class NativeTask:
        cancelled = False

    coroutine = native.factories[0][1](NativeTask())
    with pytest.raises(Exception) as caught:
        await coroutine
    assert type(caught.value).__name__ == "_AsyncTaskFailure"
    assert isinstance(caught.value._rbt_original, SystemExit)
    assert str(caught.value._rbt_original) == "contained"


@_run_async
async def test_receive_async_timeout_cleans_generation_and_allows_later_receive() -> None:
    native = _AsyncEndpoint()
    subscriber = rbt.ImageSubscriber(native, rbt.StreamDescriptor.image("async-timeout"))

    assert await subscriber.receive_async(timeout=0.001) is None
    assert native.cancelled_generations == [1]
    assert native.finished == []
    assert native.waits[0][1].cancelled() is True

    native.results.append(_SampleOwner())
    sample = await subscriber.receive_async()
    assert isinstance(sample, rbt.ReceivedImageSample)
    assert sample.sequence == 11


@_run_async
async def test_receive_async_repeated_timeout_stress_leaves_no_stale_waiter() -> None:
    native = _AsyncEndpoint()
    subscriber = rbt.ImageSubscriber(native, rbt.StreamDescriptor.image("timeout-stress"))

    for _index in range(32):
        assert await subscriber.receive_async(timeout=0.0001) is None
    assert native.cancelled_generations == list(range(1, 33))
    assert all(future.cancelled() for _generation, future in native.waits)

    native.results.append(_SampleOwner())
    assert (await subscriber.receive_async()).sequence == 11


@_run_async
async def test_receive_async_task_cancel_propagates_and_cleans_generation() -> None:
    native = _AsyncEndpoint()
    subscriber = rbt.ImageSubscriber(native, rbt.StreamDescriptor.image("async-cancel"))
    task = asyncio.create_task(subscriber.receive_async())
    await asyncio.sleep(0)
    assert len(native.waits) == 1

    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task
    assert native.cancelled_generations == [1]
    assert native.finished == []

    native.results.append(_SampleOwner())
    assert (await subscriber.receive_async()).sequence == 11


@_run_async
async def test_receive_async_stale_generation_cannot_wake_replacement() -> None:
    native = _AsyncEndpoint()
    subscriber = rbt.ImageSubscriber(native, rbt.StreamDescriptor.image("async-stale"))

    first = asyncio.create_task(subscriber.receive_async())
    await asyncio.sleep(0)
    first.cancel()
    with pytest.raises(asyncio.CancelledError):
        await first

    second = asyncio.create_task(subscriber.receive_async())
    await asyncio.sleep(0)
    assert [generation for generation, _future in native.waits] == [1, 2]
    native.wake(0)
    await asyncio.sleep(0)
    assert second.done() is False

    native.results.append(_SampleOwner())
    native.wake(1)
    assert (await second).sequence == 11
    assert native.cancelled_generations == [1]
    assert native.finished == [2]


@_run_async
async def test_receive_async_retries_spurious_wake_without_payload_polling() -> None:
    native = _AsyncEndpoint()
    subscriber = rbt.ImageSubscriber(native, rbt.StreamDescriptor.image("async-spurious"))
    task = asyncio.create_task(subscriber.receive_async())
    await asyncio.sleep(0)
    native.wake(0)
    await asyncio.sleep(0)
    await asyncio.sleep(0)
    assert task.done() is False
    assert len(native.waits) == 2
    assert native.finished == [1]

    native.results.append(_SampleOwner())
    native.wake(1)
    assert (await task).sequence == 11
    assert native.finished == [1, 2]


@_run_async
async def test_receive_async_surfaces_typed_native_wait_failure() -> None:
    native = _FailingAsyncEndpoint()
    subscriber = rbt.ImageSubscriber(native, rbt.StreamDescriptor.image("async-failure"))
    task = asyncio.create_task(subscriber.receive_async())
    await asyncio.sleep(0)
    native.wake()

    with pytest.raises(rbt.WaitError, match="injected native WaitSet failure"):
        await task


@_run_async
async def test_receive_async_spurious_wake_then_timeout_remains_timeout() -> None:
    native = _AsyncEndpoint()
    subscriber = rbt.ImageSubscriber(native, rbt.StreamDescriptor.image("spurious-timeout"))
    task = asyncio.create_task(subscriber.receive_async(timeout=0.01))
    await asyncio.sleep(0)
    native.wake()

    assert await task is None
    assert native.finished == [1]
    assert native.cancelled_generations == [2]


@_run_async
async def test_receive_async_activation_cancel_returns_none() -> None:
    native = _AsyncEndpoint()
    subscriber = rbt.ImageSubscriber(native, rbt.StreamDescriptor.image("async-activation"))
    task = asyncio.create_task(subscriber.receive_async())
    await asyncio.sleep(0)
    native.activation_expired = True
    native.wake()
    assert await task is None
    assert native.finished == [1]


@_run_async
async def test_receive_async_activation_expiry_during_registration_is_normal_cancel() -> None:
    native = _ExpiryRaceAsyncEndpoint()
    subscriber = rbt.ImageSubscriber(native, rbt.StreamDescriptor.image("async-expiry-race"))

    assert await subscriber.receive_async() is None
    assert native.waits == []


@_run_async
async def test_receive_async_rejects_concurrent_sync_receive() -> None:
    native = _AsyncEndpoint()
    subscriber = rbt.ImageSubscriber(native, rbt.StreamDescriptor.image("async-concurrent"))
    task = asyncio.create_task(subscriber.receive_async())
    await asyncio.sleep(0)

    with pytest.raises(rbt.ConcurrentReceiveError):
        subscriber.try_receive()
    with pytest.raises(rbt.ConcurrentReceiveError):
        subscriber.receive(timeout=0)

    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task


@_run_async
async def test_receive_async_rejects_second_async_waiter() -> None:
    native = _AsyncEndpoint()
    subscriber = rbt.ImageSubscriber(native, rbt.StreamDescriptor.image("async-pair"))
    first = asyncio.create_task(subscriber.receive_async())
    await asyncio.sleep(0)

    with pytest.raises(rbt.ConcurrentReceiveError):
        await subscriber.receive_async()

    first.cancel()
    with pytest.raises(asyncio.CancelledError):
        await first


@_run_async
async def test_receive_async_cancelled_after_wake_does_not_receive_hidden_sample() -> None:
    native = _AsyncEndpoint()
    subscriber = rbt.ImageSubscriber(native, rbt.StreamDescriptor.image("cancelled-wake"))
    task = asyncio.create_task(subscriber.receive_async())
    await asyncio.sleep(0)

    native.results.append(_SampleOwner())
    native.wake()
    task.cancel()
    with pytest.raises(asyncio.CancelledError):
        await task

    # The cancellation path unregisters the generation without performing the
    # post-wake receive; the exact same native sample remains available.
    assert (await subscriber.receive_async()).sequence == 11


@_run_async
async def test_receive_async_rejects_standalone_native_subscriber() -> None:
    native = _AsyncEndpoint()
    native.async_receive_supported = lambda: False  # type: ignore[method-assign]
    subscriber = rbt.ImageSubscriber(native, rbt.StreamDescriptor.image("standalone"))

    with pytest.raises(rbt.AsyncRuntimeClosedError):
        await subscriber.receive_async()
    assert native.waits == []


def test_run_component_passes_one_adapter_and_validated_requirements(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    calls: list[tuple[Any, ...]] = []

    def fake_runner(*args: Any) -> None:
        calls.append(args)
        adapter, _component_id, _requirements, config, _address = args
        adapter.prepare(config)

    monkeypatch.setattr(rbt._rbt, "_run_component", fake_runner)

    class Example(rbt.Component):
        component_id = "example"
        requirements = {"camera": "Camera"}

        def __init__(self) -> None:
            self.prepare_count = 0

        def prepare(self, config: Any) -> None:
            assert config == {"scale": 2}
            self.prepare_count += 1

        def activate(self, ctx: rbt.ActivationContext) -> None:
            del ctx

    component = Example()
    assert rbt.run_component(component, config={"scale": 2}) is None
    assert component.prepare_count == 1
    assert len(calls) == 1
    adapter, component_id, requirements, config, address = calls[0]
    assert adapter._component is component
    assert component_id == "example"
    assert requirements == [("camera", "Camera")]
    assert config == {"scale": 2}
    assert address == "127.0.0.1:7447"


@pytest.mark.parametrize(
    "requirements, error",
    [
        ([("camera", "Camera"), ("camera", "Camera")], ValueError),
        ([('camera', 'Camera'), ('camera', 'Clock')], ValueError),
        ([('camera',)], ValueError),
        ([('', 'Camera')], ValueError),
        ("camera", TypeError),
    ],
)
def test_run_component_rejects_invalid_requirements(
    monkeypatch: pytest.MonkeyPatch, requirements: Any, error: type[Exception]
) -> None:
    monkeypatch.setattr(
        rbt._rbt,
        "_run_component",
        lambda *args: pytest.fail(f"native runner called unexpectedly: {args}"),
    )
    with pytest.raises(error):
        rbt.run_component(
            rbt.Component(), component_id="test", requirements=requirements
        )

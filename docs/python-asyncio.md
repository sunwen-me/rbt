# Python asyncio adapter

See the [documentation index](README.md) for current SDK versions and related
feature guides.

The Python asyncio adapter adds coroutine-based application tasks and
event-driven receive waits to the existing Python Component runtime. It is an
adapter, not a second lifecycle runtime: Rust still owns component state,
activation, capability registration, task supervision, and teardown. The
existing synchronous `prepare`, `activate`, `spawn_critical`,
`spawn_background`, and `receive` APIs remain supported.

The adapter uses ordinary CPython `asyncio` and a runtime-managed loop. It
does not require application code to create a loop with `asyncio.run()`.
This page describes Version 1 of the asyncio API contract in the current
`rbt` 0.8.0 SDK; Version 1 is the adapter API level, not the SDK release.

## Runtime shape

There is one asyncio event loop per Python process/runtime domain. The runtime
creates it lazily, when the first activation-owned async task is committed, and
runs it on one managed loop thread until that component runtime shuts down. The
loop is reused across activations and Composer reconnects; it is not recreated
for an activation, stream, or individual receive. At process shutdown the
native runtime cancels activation work, gives supervised async tasks a bounded
cooperative cleanup window, and requires arbitrary blocking/native work to
return cooperatively before it stops the loop and joins its thread.

Activation-owned native state contains one bounded WaitSet bridge for that
active activation when async subscribers are used. The bridge can multiplex the
activation's stream sources with at most one native bridge thread per
activation; sync-only activations do not start that thread. It sends the loop
only small control events:

```text
source-ready token
native wait error
activation cancellation
```

The lifecycle/supervisor side crosses to the loop with thread-safe control
notifications. Neither the loop nor the bridge owns a capability's lifetime;
the native `Activation`/`EffectScope` does. Native task completions carry their
component/process/activation identity, while receive callbacks are scoped to
their activation bridge and pending-wait generation. Thus an old activation
cannot complete a new activation's wait. A Composer disconnect by itself
affects only the control plane; it does not stop a healthy active data-plane
task. A later valid lifecycle command can still deactivate that activation.

```text
Rust lifecycle runtime / EffectScope
                 |
                 v
          PyO3 asyncio adapter
                 |
                 v
  one managed asyncio loop thread (process lifetime)
       |                    |
       | coroutine tasks     | readiness/error/cancel tokens
       v                    v
  activation tasks <--- readiness/error/cancel token only
       |                    bounded native WaitSet bridge
       |                                |
       | direct receive after wake      | sidecar readiness
       v                                v
  subscriber.receive()          native readiness service
       |
       v
  existing SHM sample owner
```

The bridge is event-driven. It does not create a native thread per await or a
thread per stream, and it does not replace the native WaitSet with an
asyncio payload selector. Readiness may be coalesced: one notification does
not mean one sample, and a spurious readiness event may find no sample.
Sensor and Control queue, overflow, ordering, and pressure semantics remain
the native transport semantics.

## Spawning async application tasks

`prepare` and `activate` remain synchronous lifecycle hooks. Register an async
application task from an ordinary `activate` method:

```python
import rbt


class Detector(rbt.Component):
    def prepare(self, config):
        self.model = load_model(config["model"])

    def activate(self, ctx):
        camera = ctx.require("camera")
        output = ctx.provide_point_cloud("Detections", maximum_points=4)

        ctx.spawn_critical_async(
            "inference",
            self.inference,
            camera,
            output,
        )

    async def inference(self, task, camera, output):
        while not task.cancelled:
            sample = await camera.receive_async()
            if sample is None:
                continue

            image = sample.numpy()
            loan = output.loan(4, sequence=sample.sequence)
            points = loan.numpy()
            self.model(image, points)

            del points, image, sample
            loan.send()
```

`spawn_critical_async(name, factory, *args)` and
`spawn_background_async(name, factory, *args)` register activation-owned
coroutine factories. The factory is called when the activation starts, so
each activation receives fresh one-shot coroutine objects. Do not pass an
already-created coroutine object or reuse one across provider replacement.
The resulting task carries its component, process, activation, name, and
critical/background kind and is supervised by the same native EffectScope as
synchronous tasks.

An exception or unexpected normal return from a critical coroutine follows
the existing critical path: the activation quiesces and becomes `FAILED`.
An exception or normal completion from a background coroutine is diagnosed
and logged while the activation remains `ACTIVE`. A stale completion from an
older activation is ignored. Async lifecycle hooks such as `async def
activate` are not required by this adapter.

## `receive_async()` and zero-copy data

`receive_async()` first tries the existing nonblocking native receive. If no
sample is available, it waits for readiness and then directly calls that native
subscriber receive operation:

```python
sample = await camera.receive_async()
if sample is not None:
    image = sample.numpy()
```

The readiness event contains no sample and no payload. The coroutine does not
receive a prefetched object from the bridge: after readiness, the subscriber
itself acquires and returns the existing `ReceivedImageSample` or
`ReceivedPointCloudSample` owner. Read-only NumPy views, borrow limits,
derived-view lifetime, and explicit-copy rules are exactly those of the
synchronous data SDK. A retained view can retain a shared-memory borrow, but
it does not retain an expired `ActivationContext` or migrate to a replacement
provider.

`receive_async()` does not busy-poll, retry with `asyncio.sleep()`, or move
payload through a callback layer. In particular:

> `receive_async()` waits for readiness asynchronously but does not move samples through an asyncio queue. The payload remains in iceoryx2 shared memory until the coroutine directly receives the sample.

There is one outstanding receive wait per subscriber. A second concurrent
`receive_async()` on the same subscriber is rejected, as is an overlapping
sync `receive()` and async receive. This prevents ambiguous ownership and
silent fan-out. Separate subscribers/streams may wait concurrently.

## Timeouts and cancellation

There are three distinct cancellation domains:

1. **rbt activation cancellation** — provider loss, deactivation, a critical
   sibling failure, or process shutdown. This is a normal rbt lifecycle event:
   the native cancellation source wakes the WaitSet bridge immediately. A
   pending `receive_async()` observes activation cancellation and returns
   `None`; the activation supervisor treats the resulting task cancellation as
   normal, not as an unexpected critical failure.
2. **`asyncio` task cancellation** — `Task.cancel()` targets the Python task
   or await being canceled: this is Python coroutine cancellation. The
   coroutine observes ordinary `asyncio.CancelledError`; it does not cancel the
   activation merely by sharing the native wake mechanism. If an externally
   canceled registered critical coroutine terminates while the activation is
   otherwise healthy, that unexpected critical exit follows the critical
   failure policy. Cancellation of a background coroutine is logged and does
   not invalidate the activation. The public `AsyncTaskHandle.cancel()`
   requests the same Python-task cancellation.
3. **process shutdown** — the runtime requests activation cancellation,
   wakes native waits, gives cooperative coroutine `finally` blocks an
   opportunity to run, then waits for activation cleanup before stopping and
   joining the managed loop thread. Async task wrappers have a bounded
   supervisor grace period; arbitrary blocking/native work remains cooperative.

The adapter officially supports composing a receive wait with
`asyncio.wait_for()`:

```python
try:
    sample = await asyncio.wait_for(camera.receive_async(), timeout=0.1)
except asyncio.TimeoutError:
    sample = None
```

When the `TimeoutError` is handled as shown, `asyncio.wait_for()` cancellation
affects only that pending receive await. It cleans the subscriber's pending
Future/WaitSet registration, does not cancel the activation, and does not
prefetch or hold a sample. A later `receive_async()` can be issued normally.
The direct form
`await camera.receive_async(timeout=1.0)` follows the synchronous timeout
contract and returns `None` when its deadline expires. Timeout, spurious
readiness, and activation cancellation are not interchangeable outcomes.

Cancellation is cooperative. Native rbt waits are woken by activation
cancellation, but arbitrary CPU/native work is not forcibly preempted. CPU-heavy
code must yield at await points or split work into bounded chunks that check
`task.cancelled`. A coroutine that suppresses cancellation indefinitely can
outlive the activation's bounded supervisor grace period; async tasks must not
rely on indefinite cancellation suppression for cleanup. The runtime does not
kill the loop thread or inject an asynchronous exception, so ordinary
cooperative `finally` cleanup has an opportunity to run.

After activation expiry, retained contexts and stream handles continue to
raise the existing typed expiry/closed errors. They never gain access to the
next activation's endpoints.

## Synchronous and asynchronous tasks together

A single Component may register both kinds of activation-owned work:

```python
def activate(self, ctx):
    camera = ctx.require("camera")
    ctx.spawn_critical(
        "sync-monitor", lambda task: self.sync_monitor(task, camera)
    )
    ctx.spawn_background_async("metrics", self.async_metrics, camera)
```

Both tasks share the same native activation and cancellation boundary. A
critical failure in either kind cancels the activation and wakes its async and
native-waiting siblings. Existing synchronous code remains valid and does not
need to be migrated. Async output continues to use the existing synchronous
loan/send ownership API; this adapter does not add an implicit or
`await`-based publisher path.

## Version 1 boundaries

The Version 1 API contract supports only the runtime-managed loop described
above for registered async application tasks. It has no external-loop mode or
`run_component_async()` integration, so it does not attach rbt lifecycle
ownership to a loop that application code created. It also limits
`receive_async()` to activation-bound subscribers returned by
`ActivationContext.require()`: subscribers opened directly with
`ImageSubscriber.open()` or `PointCloudSubscriber.open()` remain synchronous
and raise `AsyncRuntimeClosedError` instead of registering an async readiness
wait. It also does not add a Python lifecycle authority, resolver, registry,
Composer, custom event loop/executor, Trio/AnyIO/Twisted support, async
generators, or readiness callbacks.

The adapter carries readiness, completion, cancellation, errors, and other
small control metadata through asyncio. It never carries `ReceivedSample`,
Image/PointCloud bytes, a loan, or a copied payload through `asyncio.Queue`,
Tokio channels, Python `bytes`, serializers, or a hidden bounded queue. The
transport's existing shared-memory queue remains the only payload queue.

For the underlying Component lifecycle and data ownership contracts, see
[`Python Component runtime`](python-component.md), [`Event-driven waiting`](waiting.md),
and [`Python Data SDK`](python.md). Current identified time and transform
semantics are documented in [Time and clocks](time.md) and
[spatial transforms](tf.md).

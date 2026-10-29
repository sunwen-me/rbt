# Python Component runtime

See the [documentation index](README.md) for current SDK versions and related
feature guides.

The synchronous Python Component API was introduced in v0.3. The current
`rbt` 0.8.0 wheel runs these Components through the same Rust lifecycle and
composition runtime used by native Rust Components. Python does not implement
a resolver, registry, identity allocator, control client, snapshot reconciler,
or lifecycle state machine.

```text
Python Component object
        |
        v
PyO3 lifecycle adapter
        |
        v
Rust Component / ActivationCtx / EffectScope
        |
        v
existing control client <----> Composer

Python task
        |
        v
Python Image/PointCloud data SDK
        |
        v
iceoryx2 shared memory
```

This page describes the current synchronous, thread-based API. The same
`rbt` 0.8.0 wheel also provides the separately documented
[asyncio Component API](python-asyncio.md); neither API adds payload callbacks
or a second lifecycle state machine.

## Defining and running a Component

Subclass `rbt.Component`, keep process-lifetime setup in `prepare`, and create
activation-owned resources in `activate`:

```python
import rbt


class Detector(rbt.Component):
    component_id = "detector"
    requirements = {"camera": "Camera"}

    def prepare(self, config):
        self.model = load_model(config["model"])

    def activate(self, ctx):
        camera = ctx.require("camera")
        output = ctx.provide_point_cloud("Detections", maximum_points=4)

        def inference(task):
            while not task.cancelled:
                frame = camera.receive()
                if frame is None:
                    continue

                image = frame.numpy()
                loan = output.loan(4, sequence=frame.sequence)
                points = loan.numpy()
                self.model(image, points)

                del points, image, frame
                loan.send()

        ctx.spawn_critical("inference", inference)


rbt.run_component(Detector(), config={"model": "small"})
```

`component_id` and `requirements` may instead be supplied as keyword arguments
to `run_component`. A requirement maps its semantic name to a capability role;
the example declares `camera -> Camera`. Application code does not choose the
provider, physical service name, process identity, activation identity, or
capability identity.

`config` is passed to `prepare` as-is. It is application state, not part of the
Composer snapshot. `composer_address` defaults to `RBT_COMPOSER_ADDR`, then to
`127.0.0.1:7447`.

## Lifecycle

One Python Component object is retained for one process lifetime.

- `prepare(config)` runs exactly once, before the first control connection.
  The same object and its prepared fields survive provider replacements,
  activation retries, and Composer restarts.
- `activate(ctx)` runs for each Composer-accepted activation attempt. An
  accepted attempt receives a fresh monotonically allocated `ActivationId`;
  gaps are allowed and identifiers are never reused within a process.
- A prepared process with unresolved requirements reports `BLOCKED`. When all
  requirements resolve, Composer sends the normal native activation command.
- Provider disappearance drives `ACTIVE -> QUIESCING -> BLOCKED`. The current
  EffectScope is cancelled and its provided capabilities are withdrawn.
- A replacement provider produces a new activation, new bound endpoints, and
  new capability instances without rerunning `prepare`.

Losing the Composer connection alone does not cancel a healthy activation.
The existing Rust control client reconnects, sends HELLO and the complete
snapshot, and participates in the normal recovery barrier. A consistent graph
is reconstructed with zero deactivate and zero activate commands. No Python
application heap or model object is serialized into that snapshot.

## ActivationContext

`ActivationContext` is a thin view of the active Rust `ActivationCtx`:

```python
ctx.component_id          # logical ComponentId
ctx.process_instance_id   # runtime-generated ProcessInstanceId
ctx.activation_id         # runtime-generated ActivationId
ctx.cancelled

camera = ctx.require("camera")
image_out = ctx.provide_image("Preview", maximum_payload_bytes=6_220_800)
cloud_out = ctx.provide_point_cloud("Detections", maximum_points=100_000)

ctx.spawn_critical("inference", inference)
ctx.spawn_background("metrics", metrics)
```

For stream requirements, the binding descriptor determines whether `require`
returns an `ImageSubscriber` or `PointCloudSubscriber`; an RPC requirement
returns an `RpcClient` and can also be resolved with `ctx.require_rpc()`.
Unsupported stream message kinds are rejected; the Component API does not fall
back to arbitrary service names. Stream provision uses the existing Sensor
profile, SidecarV1 readiness descriptor, and runtime-owned
`CapabilityInstanceId`.

The current ActivationContext also exposes `ctx.require_rpc()` and
`ctx.provide_rpc()` for managed RPC, plus `ctx.provide_dynamic_tf()`,
`ctx.provide_static_tf()`, `ctx.require_dynamic_tf()`, and
`ctx.require_static_tf()` for activation-bound TF endpoints. TF's bounded
wait/drain and snapshot operations belong in runtime-managed tasks so the
EffectScope can wake and clean them up. See [typed local RPC](rpc.md),
[Time and clocks](time.md), and [spatial transforms](tf.md) for those
contracts.

Bound stream wrappers expose diagnostic metadata:

- `provider_component_id`;
- `capability_instance_id`;
- `activation_id`.

These values are informational and cannot be supplied to allocate identity.
The returned wrapper owns the exact native endpoint created for that
activation; it does not reopen the descriptor. Once an activation is sealed or
ended, retained contexts and stream handles raise `ActivationExpiredError`.
They never migrate to a replacement provider.

## Tasks and failures

`spawn_critical` and `spawn_background` register synchronous callables in the
current Rust EffectScope. Each callable runs on a runtime-owned, joinable
blocking worker and receives a `TaskContext` with the cheap, thread-safe
`cancelled` flag.

Arbitrary Python code executes while attached to CPython and therefore obeys
the GIL. Native rbt receive waits detach from the GIL. A Python background
thread can continue making progress while another task is blocked in
`camera.receive()`.

An uncaught `BaseException`, including `SystemExit` or `KeyboardInterrupt`, in
a worker is converted once into a bounded Rust-owned diagnostic containing the
phase, exception class, message, and formatted traceback. The Python exception
and traceback frames are not retained by the supervisor.

- An uncaught critical-task exception, panic boundary, or unexpected normal
  return follows the existing critical event path. The activation quiesces,
  capabilities are removed, and the Component becomes `FAILED`. Dependents
  reconcile to `BLOCKED`.
- An uncaught background-task exception is logged with component, process,
  activation, and task identity. The activation remains `ACTIVE`.
- A critical task that returns after cancellation is not misclassified as a
  failure.

A `prepare` exception is reported as `ComponentPreparationError` with
`phase=prepare`; the Component never connects as healthy. An `activate`
exception follows the native activation-failure path. Endpoints, staged task
references, and partial capability drafts are closed or discarded before the
failed snapshot is published.

## Cancellation and teardown

rbt cancellation is immediate while a task is blocked in a native rbt wait.
Activation-bound subscribers register their native WaitSet wake handle with an
activation guard, so cancellation does not wait for the next payload and does
not poll with `try_receive` plus sleeps.

rbt does not forcibly preempt arbitrary Python or native-extension application
code. There is no `PyThreadState_SetAsyncExc`, asynchronous exception
injection, signal-as-task-cancellation API, detached worker, or daemon-thread
escape hatch. Long work should be split into bounded chunks that check
`task.cancelled`. A third-party call that blocks for ten minutes may delay
teardown for ten minutes; that is part of the cooperative contract.

EffectScope shutdown preserves the ownership order:

1. cancel the activation and expire its guard;
2. wake every registered native wait;
3. let Python tasks return or capture their exception;
4. join all runtime-owned workers and release their callables;
5. close stream endpoints;
6. publish the snapshot without the activation and provided capabilities.

No endpoint is destroyed while an owned worker may still use it. An unsent
writable loan drops without publishing after its views are released. A
received NumPy array intentionally retained beyond task return remains memory
safe: its native received-sample owner retains the iceoryx sample and shared
receiver state independently of the activation wrapper. Such retention can
still consume bounded shared-memory borrow capacity and should not be used as
general application storage.

## Zero-copy rules are unchanged

Component-bound Image and PointCloud handles are the existing data SDK
wrappers. Payload bytes do not enter lifecycle arguments, Composer messages,
NDJSON, or Tokio channels.

- Received NumPy arrays are read-only and retain the native borrowed sample.
- Publisher arrays are writable views of a native loan.
- Every writable alias must be released before `loan.send()`;
  `LoanStillMappedError` is raised instead of copying.
- There is no Component-specific auto-send or implicit publish on teardown.

See [Python Data SDK](python.md) for full buffer, pressure, and ABI rules.

## Signals, packaging, and verification

The Rust runtime loop owns process control while `run_component` detaches the
calling thread from the GIL. SIGINT and SIGTERM use the existing graceful
shutdown path: cancellation, WaitSet wake, worker join, endpoint cleanup, and
normal return. Process-main `KeyboardInterrupt` is converted to the same clean
return only after native cleanup; worker `KeyboardInterrupt` remains a
supervised task exit.

The Component API was introduced in the `rbt` 0.3 wheel and ships in the
current `rbt` 0.8.0 wheel; its declarations are included in
`rbt/__init__.pyi` and `py.typed`. The deterministic mixed example is
[component_detector.py](../examples/python/component_detector.py).

```bash
./scripts/verify_python_component.sh --quick
./scripts/verify_python_component.sh --full
```

Quick mode covers the real Rust Camera -> Python Detector -> C++ Viewer graph,
initial blocking, replacement, failure classification, cancellation,
SIGINT/SIGTERM, unexpected critical return, worker `SystemExit`, repeated
exception-frame release, Composer recovery, partial rollback, duplicate
identity, a fresh installed wheel, and inherited quick gates. Full mode adds
100 activations, retained sample/stale-handle checks, a five-minute
mixed-process soak, sanitizers, and the inherited full
Rust/C++/Python/WaitSet/pressure/v0.1 suite.

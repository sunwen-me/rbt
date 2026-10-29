# Python Component runtime acceptance report

Date: 2026-09-12

Status: **accepted**. The synchronous/thread-based Python Component runtime
passes all 27 milestone acceptance checks. The canonical retained run is
[`python-component-full-20260912T052514Z-164424`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/).
A subsequent fresh-wheel quick run added direct SIGINT, unexpected-critical-
return, and worker-`SystemExit` probes and also passed; it is retained at
[`python-component-quick-20260912T104931Z-267560`](../target/python-component-verify-logs/python-component-quick-20260912T104931Z-267560/).

## Architecture and implementation

There is one lifecycle state machine, in Rust:

```text
Python Component object
        |
        v
PyO3 lifecycle adapter
        |
        v
rbt-compose Component / ActivationCtx / EffectScope
        |
        v
existing Rust control client <----> Composer

Python task
        |
        v
existing Python Image/PointCloud data SDK
        |
        v
rbt-iox2 / iceoryx2 shared memory
```

[`component_runtime.rs`](../crates/rbt-python/src/component_runtime.rs) is an
adapter, not a second runtime. It stores the process-lifetime Python object as
`Py<PyAny>`, calls `prepare` once, translates each native `ActivationCtx` into
a thin guarded Python context, and commits endpoints, capabilities, and tasks
to the native `EffectScope`. Resolver selection, capability registration,
identity allocation, HELLO/reconnect, snapshots, revisions, reconciliation,
and lifecycle transitions remain in `rbt-compose`.

The public additions in [`rbt/__init__.py`](../python/rbt/__init__.py) are:

- `Component`;
- `ActivationContext`;
- `TaskContext`;
- `run_component`;
- five lifecycle/expiry exception types.

There is no asyncio API, Python scheduler, payload callback model, Python
resolver/registry/Composer, forceful thread interruption, daemon/detached
worker, or signal-as-task-cancellation API. The production source audits found
zero forbidden runtime mechanisms and zero payload-copy/serialization
primitives in the Component path.

## Accepted mixed-language graph

The full gate started three real independent processes from a fresh installed
wheel:

```text
Rust camera-a -[camera:Camera]-> Python detector
Python detector -[detections:Detections]-> C++ viewer
```

The exact graph is retained in [`initial.graph`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/initial.graph).
The initial `rbt ps --json` view showed all three processes active:

```text
camera-a  Active  process=7a087101-da22-40c5-9dba-fc3401fb25b4  activation=1
detector  Active  process=a4a533bf-4967-4a3d-a2f5-5aaf2ba36bd7  activation=1
viewer    Active  process=8a032af4-3207-4dd2-a580-04a48e793f5c  activation=1
```

Python emitted a four-point `PointCloud` directly into a writable loan. Both
Python and C++ independently calculated checksum `99323559804928`; the C++
viewer recorded the bound Python capability and all four points in
[`viewer.log`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/viewer.log).

## Lifecycle and provider replacement

The detector began `BLOCKED` before a Camera existed, and `rbt why detector`
reported `Camera`, requirement `camera`, `no active provider`. The retained
output is [`initial-blocked.why`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/initial-blocked.why).

Actual detector evidence from [`detector.log`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/detector.log):

```text
MODEL LOADED model_load_count=1 model_id=135290791900512
DETECTOR ACTIVATED process_instance=a4a533bf-... activation=1
                  model_id=135290791900512 model_load_count=1
DETECTOR CANCELLED activation=1
STALE HANDLE REJECTED activation=2
DETECTOR ACTIVATED process_instance=a4a533bf-... activation=2
                  model_id=135290791900512 model_load_count=1
```

Camera A loss drove `ACTIVE -> QUIESCING -> BLOCKED`, woke and joined the
Python worker, removed `Detections`, and blocked the C++ viewer. Camera B then
produced activation 2. The process UUID and model object were unchanged; the
provided capability changed from
`5d08062b-f70a-413c-8c7a-461825b0c520` to
`818cb60d-ab12-469e-ab8f-4476b1c31590`. The old activation-bound subscriber
raised `ActivationExpiredError` rather than migrating to Camera B.

## Task supervision, cancellation, and rollback

Python callables run on activation-owned `spawn_blocking` workers. The workers
are joinable and are cancelled through the existing `EffectScope`. Each task
event carries component, process, activation, task name, kind, and outcome.
Teardown marks the activation expired, wakes native WaitSet registrations,
joins all workers, releases Python callable references, then closes endpoints
and withdraws capabilities.

The full fresh-wheel run measured:

```text
native receive cancellation -> BLOCKED: 10 ms
bounded Python compute cancellation -> BLOCKED: 140 ms
```

[`critical-sibling.log`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/critical-sibling.log)
shows that a second critical task exception woke a sibling blocked in native
`receive`. [`loan-cancel.log`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/loan-cancel.log)
shows an unsent writable loan was released without publishing during
cancellation. The partial-activation probe created a publisher and staged a
background task, then raised; no capability appeared and the staged task never
started. Its diagnostic is retained in [`partial-fail.log`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/partial-fail.log).

The fresh-wheel supplemental run also sent SIGTERM and SIGINT to active Python
Components blocked in native receive; both waits woke, logged cooperative
cancellation, joined, and exited with status zero. A critical callable that
returned normally produced native `outcome=Completed` and `FAILED`. A critical
worker that raised `SystemExit` produced `exception=SystemExit` and `FAILED`,
while the process remained alive until a later graceful shutdown. The exact
logs are [`require-only.log`](../target/python-component-verify-logs/python-component-quick-20260912T104931Z-267560/fresh-wheel-runtime/runtime-quick.Rvqk4t/require-only.log),
[`sigint.log`](../target/python-component-verify-logs/python-component-quick-20260912T104931Z-267560/fresh-wheel-runtime/runtime-quick.Rvqk4t/sigint.log),
[`critical-return.log`](../target/python-component-verify-logs/python-component-quick-20260912T104931Z-267560/fresh-wheel-runtime/runtime-quick.Rvqk4t/critical-return.log),
and [`critical-system-exit.log`](../target/python-component-verify-logs/python-component-quick-20260912T104931Z-267560/fresh-wheel-runtime/runtime-quick.Rvqk4t/critical-system-exit.log).

The same fresh-wheel run launched 32 failing background workers. Each task
created a frame-local weak-reference sentinel before raising. The activation
remained `ACTIVE`, exactly 32 supervised exits were logged, and all 32
sentinels finalized after traceback formatting (`raised=32 released=32`). This
directly verifies that the native supervisor retains Rust-owned diagnostics,
not Python exception/frame cycles. See
[`background-exception-storm.log`](../target/python-component-verify-logs/python-component-quick-20260912T104931Z-267560/fresh-wheel-runtime/runtime-quick.Rvqk4t/background-exception-storm.log).

Cancellation is deliberately cooperative for arbitrary application code.
rbt immediately wakes tasks blocked in native rbt waits, but it does not and
cannot forcibly preempt arbitrary Python bytecode or a third-party native call.
Long-running application work must use bounded chunks and check
`task.cancelled`.

## Critical, background, and preparation exceptions

An injected critical `RuntimeError` after three samples followed the ordinary
Rust supervisor path:

```text
ACTIVE -> QUIESCING
removing capability ... capability_type=Detections
phase=task; exception=RuntimeError
message=test critical failure after processing 3 frames
traceback=Traceback (most recent call last): ...
activation FAILED ... task=inference
```

The diagnostic includes component, process UUID, activation, task, exception
class/message, and bounded traceback. The capability topology no longer
contained `Detections`, and the native viewer was `BLOCKED`. See
[`detector-critical.log`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/detector-critical.log),
[`critical-failure.json`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/critical-failure.json),
and [`critical-failure.why`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/critical-failure.why).

An injected background `RuntimeError` was logged as
`background task exited; activation remains unchanged`; the detector and
viewer stayed `ACTIVE` and subsequent samples arrived. See
[`detector-background.log`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/detector-background.log).

A preparation exception exited with typed `ComponentPreparationError` and
`phase=prepare`, class, message, and traceback. It never connected as a healthy
component. See [`prepare-fail.log`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/prepare-fail.log).
Uncaught worker `BaseException` subclasses, including worker `SystemExit` and
`KeyboardInterrupt`, use the same supervised conversion and cannot terminate
the process outside the runtime.

## Composer disconnect and reconstruction

Payload counts continued increasing while Composer was killed. On the healthy
restart, the detector retained:

```text
ProcessInstanceId     a4a533bf-4967-4a3d-a2f5-5aaf2ba36bd7
ActivationId          2
CapabilityInstanceId  818cb60d-ab12-469e-ab8f-4476b1c31590
model_load_count      1
reconciliation        0 deactivate, 0 activate
```

The before/after snapshots are
[`before-composer-restart.json`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/before-composer-restart.json)
and [`after-composer-restart.json`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/after-composer-restart.json).

When Composer and Camera were both down, restarting Composer reconstructed the
snapshots and issued `1 deactivate, 0 activate`. The detector and viewer then
became `BLOCKED`, and `Detections` was absent. This uses the standard Phase 2
reconciliation path; there is no Python process monitor or recovery shortcut.
See [`composer-after-provider-loss.log`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/composer-after-provider-loss.log).

The final 306-second soak restarted Composer ten times. Every reconstruction
reported `0 deactivate, 0 activate`, while the detector retained process
`3dc01553-6b94-46e6-aede-72a10310e113`, activation `101`, and capability
`be99184c-aab3-4210-a73a-8928ad867eec` from
[`soak-before.json`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/soak-before.json)
through [`soak-after.json`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/soak-after.json).

## Stale events and activation ownership

Three independent checks cover stale activation behavior:

1. The full harness asserted 100 ordered `ACTIVATED(N) -> CANCELLED(N)` pairs;
   activation `N+1` never began before worker `N` was joined.
2. `stale_critical_exit_does_not_change_current_activation` injects a delayed
   activation-1 critical completion while activation 2 is current and verifies
   that state, snapshot, and activation 2 remain unchanged. The equivalent
   background-event regression also passes.
3. The real Python example retains the activation-1 camera handle and proves
   its use is rejected during activation 2.

Thus ordinary Python task completion cannot cross an activation boundary, and
the process/activation identity filter remains a defensive second layer for a
delayed event.

## Zero-copy and retained-view ownership

Component handles wrap the exact endpoints created from resolved native
bindings; they do not reopen service strings. The lifecycle DTO contains only
control metadata and `StreamDescriptor` JSON. Image/PointCloud bytes remain on
the existing iceoryx2 data path.

The inherited full Python gate passed 66 tests and retained these pointer and
ownership observations in [`python-pytest-full.log`](../target/python-verify-logs/python-full-20260912T052539Z-166096/python-pytest-full.log):

```text
Image native_ptr == numpy_ptr, owndata=False, writeable=False
PointCloud native_ptr == numpy_ptr for N=0,1,100,100000
publisher arrays match loan._debug_payload_address(), writeable=True
LoanStillMapped gating and atomic map/send races passed
```

The Component example retains one received ndarray globally across endpoint
and activation teardown. This is safe because the buffer owner retains the
`ReceivedSliceSample`, which retains the iceoryx sample and receiver shared
state independently of the subscriber wrapper. The retained borrow can still
consume finite transport capacity. Old sample memory does not confer old
context, requirement, endpoint, or publisher authority.

## 100 activations and resource soak

The installed-wheel detector alternated Camera A/B for exactly 100 accepted
activations in one process. The harness required 100 activation log entries,
100 joined inference-task cancellation entries, strict ordering, one model
load, a rejected stale handle, a retained input view, no `Detections`
capability after each teardown, and no stale topology binding.

```text
prepare_count       1
activation_count    100
RSS KiB             37,784 -> 39,116  (+1,332)
file descriptors    19 -> 19
threads             38 -> 38
orphan capabilities 0
stale bindings      0
```

Resource snapshots are
[`cycles-resource-baseline.txt`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/cycles-resource-baseline.txt)
and [`cycles-resource-final.txt`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/cycles-resource-final.txt).
These bounded observations are a practical leak check, not a mathematical leak
proof; Python and C++ ASan/UBSan gates provide the complementary memory-safety
evidence.

## Packaging and CLI

The canonical artifact is
[`rbt-0.2.0-cp312-cp312-linux_x86_64.whl`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/wheel/rbt-0.2.0-cp312-cp312-linux_x86_64.whl),
tested with CPython 3.12.14 and NumPy 2.5.3. It was installed into a fresh venv;
`PYTHONPATH`, `LD_LIBRARY_PATH`, and `LIBRARY_PATH` were unset for install and
import, and the Python example/probe ran from a temporary directory outside the
checkout. The extension had no RPATH/RUNPATH, unresolved dependency, embedded
repository path, or bytecode artifact.

CLI evidence uses the ordinary language-neutral interfaces:

- `rbt ps --json`: [`initial-active.json`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/initial-active.json);
- `rbt graph`: [`initial.graph`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/initial.graph);
- `rbt why detector` while blocked: [`initial-blocked.why`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/initial-blocked.why);
- `rbt why detector` after critical failure: [`critical-failure.why`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-runtime/runtime-full.xzZ4qL/critical-failure.why).

## Quality gates

All commands below passed in the canonical full run:

| Gate | Result and retained evidence |
| --- | --- |
| Rust formatting/tests/strict Clippy | `cargo fmt --check`, `cargo test --workspace`, all-target/all-feature Clippy with `-D warnings`; [`top-level logs`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/) |
| Python API/RX/TX | 52 quick tests; 66 full tests; [`python-full-20260912T052539Z-166096`](../target/python-verify-logs/python-full-20260912T052539Z-166096/) |
| Python ASan | ASan-instrumented wheel; 36 ownership/buffer tests passed |
| C++ Debug/Release | Six CTest tests passed in both configurations with warnings as errors |
| C++ ASan/UBSan | Six of six CTest tests passed; [`sanitizer-ctest.log`](../target/v01-logs/verify-full-20260912T052824Z-172181/sanitizer-ctest.log) |
| IDL | Two generations were byte-identical and matched checked-in Rust/C++/descriptor/Python outputs |
| Phase 1-4 and v0.1 | Phase4, external install, mixed runtime, recovery, soak, and benchmark passed; [`verify-full-20260912T052824Z-172181`](../target/v01-logs/verify-full-20260912T052824Z-172181/) |
| WaitSet | semantics, fairness, lifecycle, 100 cycles, benchmark matrix, and 45-second idle soak passed; [`waitset-full-20260912T052751Z-169651`](../target/waitset-verify-logs/waitset-full-20260912T052751Z-169651/) |
| Pressure | loan/borrow limits, process-death recovery, mixed-speed subscribers, graph pressure, replacement, and Composer recovery passed; [`pressure.6puGL3`](../target/pressure-logs/pressure.6puGL3/) |
| Python Component | fresh wheel, real mixed graph, quick scenarios, 100 cycles, 306-second/10-restart soak passed; [`fresh-wheel-component.log`](../target/python-component-verify-logs/python-component-full-20260912T052514Z-164424/fresh-wheel-component.log) |

Production unsafe source locations remained:

```text
old=49
new=49
delta=0
```

No sanitizer was skipped.

## Compatibility and semantic change audit

| Axis | Result |
| --- | --- |
| Payload ABI/layout hashes | unchanged; frozen layout tests and cross-language interop passed |
| IDL and generated outputs | unchanged and byte-deterministic |
| Sensor/Control QoS | unchanged |
| WaitSet/SidecarV1 | unchanged; the adapter only registers the existing native wake handle with activation expiry |
| Loan/borrow and NumPy ownership | unchanged; activation wrappers add authority expiry without copying or shortening live buffer-owner lifetime |
| Control protocol/snapshot/descriptor versions | unchanged (`2`/`2`/`1`); protocol source and DTOs were not modified |
| Identity and stale-event rules | unchanged; Python uses native allocation and filtering |
| Lifecycle authority | unchanged; Rust remains authoritative |

One language-neutral correctness adjustment was necessary: after preparation
and first control connection, Rust and C++ components with unresolved bindings
now publish `BLOCKED` instead of remaining indefinitely `PREPARING`. Activation
allocation, transition legality, reconciliation, failure, and recovery rules
are otherwise unchanged. This is the state already used after provider loss
and is required for the milestone's initial-blocking contract.

## Final 27-item acceptance audit

| # | Acceptance item | Evidence |
| ---: | --- | --- |
| 1 | Python is a real Component language | public subclass/run API and independent mixed process passed |
| 2 | `prepare()` once per process | model load count 1 across replacement, 100 cycles, and Composer restarts |
| 3 | `activate()` per activation | activation 1/2 and exactly 100-cycle count |
| 4 | Require native capability | `Camera` resolved by native `ActivationCtx`; missing provider was `BLOCKED` |
| 5 | Provide to Rust/C++ | Python `Detections`/PointCloud consumed and validated by C++ viewer |
| 6 | Provider loss gives ACTIVE→BLOCKED, not FAILED | provider-loss snapshot, native cancellation, viewer blocked |
| 7 | Replacement gets new activation without prepare | activation 1→2, same process/model, load count 1 |
| 8 | Critical exception gives FAILED | injected `RuntimeError` and native critical supervisor log |
| 9 | Critical failure revokes capability | `Detections` absent from critical-failure topology |
| 10 | Native dependent blocks | C++ viewer `BLOCKED` in critical snapshot |
| 11 | Background exception remains ACTIVE | error log followed by continued detector/viewer samples |
| 12 | Native WaitSet wakes on cancellation | 10 ms receive-to-BLOCKED path and sibling wake probe |
| 13 | Arbitrary compute is cooperative only | 140 ms bounded-loop probe; forbidden-forceful-source audit zero |
| 14 | Critical sibling wakes blocked Python task | `PROBE SIBLING CANCELLED`, no leaked worker |
| 15 | Old task completion cannot affect new activation | join-before-next-activation assertion plus injected stale critical/background event tests |
| 16 | Composer crash preserves healthy activation | data continued and all three detector identities remained unchanged |
| 17 | Healthy restart is 0/0 | initial recovery and ten soak recoveries logged `0 deactivate, 0 activate` |
| 18 | Provider death while Composer down reconciles | restart logged `1 deactivate, 0 activate`; detector/viewer blocked, capability absent |
| 19 | Rust→Python→C++ real processes | accepted graph and matching payload checksum |
| 20 | Input zero-copy/read-only | pointer identity, non-owning/read-only arrays, retained-borrow tests |
| 21 | Output zero-copy/writable-loan | loan pointer identity, writable views, `LoanStillMapped`, no implicit send |
| 22 | No payload in lifecycle/control paths | source audit zero; lifecycle DTO carries descriptors/control metadata only |
| 23 | 100 cycles/resources clean | prepare 1, activate/join 100, RSS +1,332 KiB, FD/thread flat, no orphan/stale topology |
| 24 | Fresh wheel outside repository | CPython 3.12.14 venv with unset source/library paths passed full graph |
| 25 | ABI/protocol unchanged | deterministic/frozen IDL/layout and protocol regressions passed |
| 26 | Unsafe boundary did not grow | 49→49, delta 0 |
| 27 | All inherited gates pass | Rust/C++/Python/WaitSet/pressure/Phase1-4/v0.1/install/sanitizer gates passed |

## Remaining work

The milestone stops here. Deferred work remains the asyncio adapter, MCAP,
ROS2 bridge, network gateway, schema evolution, and advanced pool tuning.

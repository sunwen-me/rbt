# Python asyncio adapter final report

Status: **accepted** on 2026-09-13 after the official quick and full verification
runs completed successfully. This milestone adds only the Python asyncio
adapter. MCAP, the ROS2 bridge, the network gateway, schema evolution, and
advanced pool tuning were not started.

The authoritative run artifacts are:

- quick: `target/python-asyncio-verify-logs/python-asyncio-quick-20260912T223221Z-156405`
- full: `target/python-asyncio-verify-logs/python-asyncio-full-20260912T223851Z-175218`
- full asyncio runtime: `target/python-asyncio-verify-logs/python-asyncio-full-20260912T223851Z-175218/async-runtime/runtime-full.crJ03L`
- full mixed-graph harness: `target/python-asyncio-verify-logs/python-asyncio-full-20260912T223851Z-175218/async-runtime/runtime-full.crJ03L/component-harness/runtime-full.k8m0n2`

All measurements below come from the authoritative full run unless stated
otherwise.

## Architecture and ownership

The adapter keeps the existing Rust component runtime as the only lifecycle
authority:

```text
Rust lifecycle authority / EffectScope
                  |
                  v
           PyO3 async adapter
                  |
                  v
     managed asyncio event loop
                  |
                  v
          readiness Future
                  ^
                  |
       native WaitSet bridge
                  |
                  v
      direct subscriber.try_receive()
                  |
                  v
       existing SHM sample owner
```

There is one lazily created CPython event loop per component runtime/process.
It runs on the managed native thread named `rbt-python-asyncio`, is reused
across activations and Composer reconnects, and is stopped and joined only
after activation-owned work has been cancelled and drained during process
shutdown. Activation commits register fresh coroutine objects with that loop;
the native EffectScope remains responsible for task identity, supervision,
cancellation, and teardown.

An activation that uses async subscribers owns one bounded native WaitSet
bridge thread named `rbt-wait-bridge-*`. That single bridge multiplexes all of
the activation's subscriber sources. A sync-only activation does not create a
bridge, and there is no native thread per await or per stream. The measured
multi-source probe concurrently received Camera and Detections data with one
loop thread and one bridge thread.

The bridge crosses into the loop with control information only:

```text
source-ready token
native wait error
activation cancellation
```

It never carries or prefetches payloads. After a readiness Future completes,
the coroutine invokes the subscriber's existing nonblocking native receive
path. Readiness is deliberately coalesced, so a wake can be spurious and does
not imply a one-event-to-one-sample relationship.

## Python API surface

The new public surface is:

- `ActivationContext.spawn_critical_async(name, factory, *args)`
- `ActivationContext.spawn_background_async(name, factory, *args)`
- `AsyncTaskHandle`, including Python-task cancellation through `cancel()`
- `ImageSubscriber.receive_async(timeout=None)`
- `PointCloudSubscriber.receive_async(timeout=None)`

`prepare` and `activate` remain synchronous lifecycle hooks. Existing
`spawn_critical`, `spawn_background`, `receive`, publisher loan/send, and data
buffer APIs are unchanged. Registered async functions are factories invoked
once per activation, not reusable pre-created coroutine objects.

Both `receive_async(timeout=...)` and composition with
`asyncio.wait_for(subscriber.receive_async(), ...)` are supported. A handled
timeout cancels and unregisters that wait only; a later receive on the same
subscriber remains valid.

## Installed-wheel mixed graph

The full run built and installed
`rbt-0.2.0-cp312-cp312-linux_x86_64.whl` into a fresh CPython 3.12.14 virtual
environment. Import and runtime probes ran outside the repository checkout
with `PYTHONPATH`, `LD_LIBRARY_PATH`, and `LIBRARY_PATH` unset. The graph was:

```text
Rust Camera -> Python Async Detector -> C++ Viewer
```

The graph passed activation, data flow, provider replacement, Composer outage
and restart, shutdown, and ownership checks from the installed wheel. This
prevents the repository's source tree or an in-place extension from masking a
packaging error.

## Lifecycle and provider replacement

The component harness recorded `prepare=1` and `activate=100` for its 100-cycle
summary. Across provider loss and replacement it observed the expected
sequence:

```text
prepare once
activation 1 ACTIVE
Camera provider disappears
ACTIVE -> QUIESCING -> BLOCKED
activation 2 ACTIVE
same Python process and prepared model; new activation/capability identity
```

For the initial replacement, the detector retained one process instance and
one model object with `model_load_count=1`; only the activation and capability
identities changed. A retained input NumPy view was still valid in activation
2 with pointer `136917918777560`, checksum `10304`, `OWNDATA=False`, and
`WRITEABLE=False`. The view retained its original shared-memory borrow; it did
not retain or migrate the expired activation context.

The loop identity and its native loop thread remained constant throughout the
100-cycle run (`loop_ids=1`, `native_loop_threads=1`). Each old activation's
tasks reached `finally` before its activation drained, while the loop itself
survived for the next activation.

## Cancellation and teardown

The implementation preserves three distinct domains:

| Domain | Observable contract | Full-run evidence |
| --- | --- | --- |
| rbt activation cancellation | A normal lifecycle event. Provider loss wakes the native wait; `receive_async()` returns `None`; the supervisor quiesces to `BLOCKED`. | Provider loss to `BLOCKED`: 120 ms. |
| Python `asyncio` cancellation | `Task.cancel()` raises ordinary `asyncio.CancelledError` in that coroutine and removes only that Future/registration. It does not cancel the activation merely by using the same native wake mechanism. | Request to coroutine `finally`: 208,396 ns, reported as 0 ms after integer rounding; a subsequent receive on the same subscriber succeeded. |
| Process shutdown | The runtime requests activation cancellation, wakes waits, allows cooperative `finally` cleanup, drains the activation, then stops and joins the managed loop thread. | Shutdown path: 60 ms, with the cleanup probe's `finally` observed. |

Arbitrary CPU-bound or blocking native work is cooperative and is not forcibly
preempted. The adapter does not inject exceptions into threads, use daemon
threads as a shutdown shortcut, or kill the loop. A coroutine that suppresses
cancellation indefinitely can therefore outlive the bounded supervisor grace
period and is application misuse rather than a forcefully recoverable case.

Repeated direct timeouts and `asyncio.wait_for()` timeouts were followed by a
successful receive. Cancellation of a pending receive was likewise followed
by a successful receive on the same subscriber. The unsent-loan cancellation
probe confirmed that dropping a coroutine also drops its unsent output loan.

## Supervision and error fidelity

Critical and background async tasks use the existing EffectScope outcomes:

- A critical `RuntimeError("intentional async critical failure")` caused
  `ACTIVE -> QUIESCING -> FAILED`. The native failure retained the Python
  exception class, message, and traceback through `_run_supervised_coroutine`
  to the original probe line.
- `SystemExit("intentional worker SystemExit (async)")` was captured as the
  original `SystemExit` failure with its traceback; it did not terminate the
  managed event-loop thread.
- Unexpected normal completion of a critical coroutine produced
  `outcome=Completed` and `FAILED`.
- Background exception and unexpected background completion logged
  `background task exited; activation remains unchanged`; the component stayed
  `ACTIVE`.
- The background exception storm raised 32 exceptions and released all 32
  coroutine-owned resources.
- A critical synchronous failure cancelled an async sibling waiting in
  `receive_async()`, and a critical async failure cancelled a synchronous
  sibling in a native wait. Both directions reached `finally`/cleanup and the
  activation failed through the ordinary critical-task path.

## Receive races and concurrency

Each subscriber permits exactly one pending receive operation across the sync
and async APIs. A concurrent async waiter, or an overlapping sync receive, is
rejected with a typed error instead of silently fan-outing a sample.

Pending waits have an activation-scoped source identity and a monotonically
changing registration generation. Cancellation follows this order:

```text
cancel/remove old Future and native registration
ignore a wake carrying the old activation/generation token
install a new Future with a new generation
allow only the matching token to complete it
```

The live stale-completion regression intentionally allowed activation 1 to
finish after activation 2 was active. Activation 2 became active at monotonic
time `3152480809655` ns; activation 1 completed late at `3152812451429` ns.
The component remained `Active`, proving that an old activation completion did
not drive the current activation's state. Separate wake-generation tests
covered cancelled Future, stale wake, and replacement Future cleanup.

The multi-source probe received at least three Camera and three Detections
samples concurrently with one loop and one bridge. A stalled consumer made
zero receives while a fast peer continued to receive at least three samples,
showing that a stalled async task did not consume or prefetch peer data.

## Zero-copy and absence of hidden payload queues

The async receive path returned the same native sample owner used by the
synchronous API. Pointer evidence from the fresh-wheel mixed graph was:

| Buffer | Native pointer | NumPy pointer | `OWNDATA` | `WRITEABLE` |
| --- | ---: | ---: | --- | --- |
| received Image input | `131845611548888` | `131845611548888` | `False` | `False` |
| PointCloud output loan | `131845611536600` | `131845611536600` | `False` | `True` |

Thus RX remains a read-only borrowed shared-memory view and TX remains the
existing writable shared-memory loan. No payload copy or adapter-owned sample
object was introduced.

The architecture prevents a hidden payload queue: the bridge has no payload
type and sends only source-ready/error/cancel tokens; only the coroutine calls
the existing subscriber receive. The official source audits reported:

```text
RBT_PYTHON_ASYNCIO_PAYLOAD_AUDIT forbidden_matches=0
RBT_PYTHON_ASYNCIO_BUSY_POLL_AUDIT forbidden_matches=0
RBT_PYTHON_ASYNCIO_PER_AWAIT_THREAD_AUDIT forbidden_matches=0 managed_native_threads=2
RBT_PYTHON_ASYNCIO_FORCEFUL_INTERRUPTION_AUDIT forbidden_matches=0
```

The prohibited set includes sample-carrying `asyncio.Queue` use, executor or
`to_thread` receive shims, sleep-retry polling, per-await native threads, and
forceful Python thread interruption. Runtime evidence also showed 24 readiness
events and 24 callbacks without a payload transfer through the bridge.

## Pressure and coalescing

The inherited Sensor and Control pressure suites passed unchanged. Async
pressure probes additionally observed a slow consumer receiving sequences
`2, 4, 6` with gaps of two and a bounded backlog, matching native Sensor
discard/overflow behavior. In a wake storm, six native readiness events were
coalesced to two callbacks while three samples were received. These results
are evidence that readiness notification is advisory and coalesced while
queueing, ordering, overflow, and backpressure remain native transport
semantics.

## Composer recovery

During a healthy Composer outage and restart, the data plane continued with
the same component process, model, activation, async task, and managed loop
identities. The restarted Composer reported:

```text
reconciliation: 0 deactivate, 0 activate
```

The 306-second soak included ten Composer restarts; every healthy restart had
the same 0/0 reconciliation result. When the provider died while Composer was
down, the component independently left `ACTIVE`, and the restarted Composer
correctly reported `1 deactivate, 0 activate`. The control-plane outage did
not become a second lifecycle authority.

## Resources and measured performance

The 100-cycle fresh-wheel component summary was:

| Metric | Before | After | Observation |
| --- | ---: | ---: | --- |
| RSS | 43,256 KiB | 45,080 KiB | +1,824 KiB; bounded in this run, not an allocation-leak proof by itself |
| file descriptors | 38 | 38 | no growth |
| process threads | 37 | 37 | no growth |
| async task wrappers | 302 starts | 302 `finally` completions | balanced |
| explicit zero-task drains | — | 147 | no adapter task remained at sampled drain points |
| loop identities | — | 1 | one loop reused |
| native loop thread identities | — | 1 | one loop thread reused |
| unexpected pending-task warnings | — | 0 | clean shutdown audit |

The initial mixed graph separately observed five async task starts, five
`finally` completions, one loop thread, and one bridge thread. The per-activation
bridge is joined and replaced; it does not accumulate across cycles.

Idle and wake figures are diagnostic observations, not service-level
objectives:

| Measurement | Result |
| --- | ---: |
| minimal idle async task, 10 s | 1 process CPU tick |
| no-data sync receive, 10 s | 2 user + 2 system CPU ticks |
| no-data async receive, 10 s | 0 user + 0 system CPU ticks |
| sync no-data threads / FDs | 38 -> 38 / 34 -> 34 |
| async no-data threads / FDs | 38 -> 38 / 45 -> 45 |
| readiness handoff sample | 399,160 ns |
| wake latency, 3,159 samples | p50 341,384 ns; p90 514,668 ns; p99 674,640 ns; max 1,727,593 ns |

The eleven additional async-case FDs are the active loop/bridge event
descriptors; their count remained stable. Equal total thread counts in the
sync and async no-data cases are incidental to their different runtime thread
composition. The relevant async invariant is the directly observed one
managed loop thread plus one multiplexing bridge thread, with no growth.

## Compatibility and unsafe audit

| Surface | Result |
| --- | --- |
| payload ABI | unchanged |
| IDL and generated schemas | unchanged |
| Sensor QoS | unchanged |
| Control QoS | unchanged |
| native WaitSet behavior/API | unchanged; reused by the adapter |
| SidecarV1 | unchanged |
| Python buffer ownership | unchanged |
| Composer protocol | unchanged |
| snapshot semantics | unchanged |
| lifecycle authority | unchanged; Rust remains authoritative |
| new surface | Python asyncio adapter only |

The asyncio milestone's Rust unsafe count was `old=49`, `new=49`, `delta=0`.
The implementation introduces no new `unsafe` block or lifetime shortcut.

## Verification matrix

| Gate | Result | Authoritative evidence |
| --- | --- | --- |
| official quick verifier | PASS | `python-asyncio-quick-20260912T223221Z-156405` |
| official full verifier | PASS | `python-asyncio-full-20260912T223851Z-175218` |
| fresh CPython 3.12 wheel build/install/import outside checkout | PASS | full `fresh-wheel-*.log` files |
| Rust workspace and `rbt-python` checks | PASS | inherited full logs plus final `cargo check`, Clippy with warnings denied, and formatting check |
| Python API tests | 25 passed | `async-unit-tests.log` |
| Rust Camera -> Python Async Detector -> C++ Viewer | PASS | `async-runtime.log` and `component-runtime-harness.log` |
| provider replacement / retained view | PASS | mixed-graph `detector.log` and `cycles-detector.log` |
| critical/background supervision and traceback fidelity | PASS | mixed-graph and `async-probes` logs |
| cancellation, timeout, stale wake/completion, waiter exclusivity | PASS | `async-probes` logs |
| sync/async sibling cancellation in both directions | PASS | `sync-async-sibling.log`, `async-sync-sibling.log` |
| async pressure/coalescing and stalled peer | PASS | `slow-consumer.log`, `wake-storm.log`, `stalled-consumer.log`, `fast-peer.log` |
| 100 cycles, 306-second soak, ten Composer restarts | PASS | full component harness |
| inherited v0.1, WaitSet, pressure, Python RX/TX and Component gates | PASS | `inherited-python-component.log` |
| C++ and Python sanitizer gates | PASS | inherited full verification |
| forbidden-pattern source audits | PASS, zero matches | top-level `async-*-audit.log` files |
| Rust unsafe delta | PASS, 0 | `inherited-python-component.log` final audit |

## Final acceptance checklist

| # | Requirement | Result and evidence |
| ---: | --- | --- |
| 1 | Existing synchronous Python Component API still works. | PASS — inherited Python Component full suite and live sync-only/mixed probes. |
| 2 | Async Python application tasks are activation-owned. | PASS — EffectScope registration plus per-activation start/finally accounting. |
| 3 | `receive_async()` uses native WaitSet readiness. | PASS — live readiness callbacks and native bridge evidence. |
| 4 | `receive_async()` performs no payload polling. | PASS — event-driven idle result and busy-poll audit 0. |
| 5 | No payload enters `asyncio.Queue` or another hidden queue. | PASS — control-token-only bridge and payload audit 0. |
| 6 | Returned sample is the existing zero-copy SHM sample owner. | PASS — exact native/NumPy pointer equality. |
| 7 | Provider loss while awaiting wakes coroutine and gives `BLOCKED`. | PASS — live provider-loss probe, 120 ms to `BLOCKED`. |
| 8 | Provider replacement gives a new activation without re-prepare. | PASS — prepare 1, repeated activation, stable model/process identity. |
| 9 | Critical async exception gives `FAILED`. | PASS — `RuntimeError` probe with original traceback. |
| 10 | Critical async normal return gives `FAILED`. | PASS — `outcome=Completed` critical-return probe. |
| 11 | Background async exception leaves activation `ACTIVE`. | PASS — background error and 32-task storm probes. |
| 12 | rbt activation cancellation is distinguished from user asyncio cancellation. | PASS — separate native-cancel and `CancelledError` paths. |
| 13 | `asyncio.Task.cancel()` does not corrupt subscriber/WaitSet state. | PASS — cancel followed by receive on the same subscriber. |
| 14 | Timeout/cancelled await does not secretly receive or hold a sample. | PASS — three timeouts followed by later receive; stalled consumer received zero while fast peer progressed. |
| 15 | Stale wake cannot complete a replacement Future. | PASS — activation/generation filtering and stale-race probes. |
| 16 | One subscriber cannot accumulate uncontrolled concurrent waiters. | PASS — sync/async and async/async overlap rejection tests. |
| 17 | Sync and async tasks can coexist. | PASS — mixed task activation probe. |
| 18 | Critical sync failure cancels async sibling. | PASS — live sync-to-async sibling failure probe. |
| 19 | Critical async failure cancels sync sibling. | PASS — live async-to-sync sibling failure probe. |
| 20 | Composer outage leaves async data path alive. | PASS — samples continued with stable identities. |
| 21 | Healthy Composer restart remains 0 deactivate / 0 activate. | PASS — initial restart and all ten soak restarts. |
| 22 | Provider death while Composer is down reconciles correctly. | PASS — restart reported 1 deactivate / 0 activate. |
| 23 | Rust Camera -> Python Async Detector -> C++ Viewer passes from a fresh installed wheel. | PASS — CPython 3.12.14 outside-checkout graph. |
| 24 | Python RX zero-copy ownership remains unchanged. | PASS — equal pointers, `OWNDATA=False`, `WRITEABLE=False`, retained-view probe. |
| 25 | Python TX writable-loan ownership remains unchanged. | PASS — equal pointers, `OWNDATA=False`, `WRITEABLE=True`, unsent-loan cleanup. |
| 26 | Sensor/Control pressure semantics remain unchanged. | PASS — inherited pressure gates and async slow/coalesced probes. |
| 27 | 100 activations do not leak loop threads, Futures, Python tasks, FDs, or obvious RSS. | PASS — one loop identity, balanced 302/302 tasks, FD/thread stability, no pending warnings, bounded +1,824 KiB RSS observation. |
| 28 | Idle operation remains event-driven. | PASS — 1 tick minimal idle; 0 user/0 system ticks for no-data async receive over 10 s. |
| 29 | Payload ABI, IDL, protocol, and snapshot semantics are unchanged. | PASS — compatibility audit and inherited interop gates. |
| 30 | No new unsafe lifetime hack was introduced. | PASS — unsafe 49 -> 49, delta 0. |
| 31 | All inherited v0.1/WaitSet/pressure/Python RX/TX/Python Component gates pass. | PASS — official full verifier. |

All 31 acceptance conditions are satisfied. This milestone stops here as
required; follow-on transport, recording, schema, and pool-tuning work remains
out of scope.

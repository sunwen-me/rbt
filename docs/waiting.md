# Event-driven waiting

> Status: experimental/internal milestone API. `WaitContext`, `EventSource`,
> `WaitToken`, `ReadyEvent`, `WaitProgress`, `WaitExit`, `WaitError`, and
> `WaitWakeHandle` are implementation-facing 0.x interfaces. They are not a
> stable latency, scheduling, or QoS contract.

This guide describes the event-driven receive path for streams backed by the
maintained iceoryx2 [`0.10.0-rbt.2`](iceoryx2-unified-report.md) baseline. It
preserves the v0.1 lifecycle, capability, queue, and message-ABI rules. The
change is how a component waits for local data-plane work; it does not move
lifecycle decisions or payloads into the Composer.

For physical sample timestamps and identified clock behavior, see [time.md](time.md);
TF records reuse those time identities in the [TF guide](tf.md). Typed local
RPC has its own endpoint WaitSet bridge and deadline/cancellation contract in
[rpc.md](rpc.md).

## Why a WaitSet

The preserved polling baseline calls `Subscriber::receive()`, finds no sample,
sleeps, and tries again. That sleep bounds idle work and cancellation
responsiveness, even when no data is arriving. The current WaitSet path instead
blocks on file-descriptor-backed readiness and returns when a source or
cancellation event wakes it.

The active `0.10.0-rbt.2` baseline retains one relevant API limitation:
publish/subscribe `Subscriber` has `has_samples()` and `receive()`, but does
not implement `FileDescriptorBased` or `SynchronousMultiplexing`. It cannot be
attached directly to a `WaitSet`. iceoryx2 `Listener` does implement the
waitable interfaces, so rbt gives every stream a small event sidecar:

```text
publisher loan/send -> iceoryx2 publish/subscribe SHM
                                      |
                                      | readiness notification only
                                      v
                                  WaitSet wakes
                                      |
                                      v
subscriber.receive() -> borrowed SHM sample -> processing
```

The sidecar solves the wakeup problem without changing the transport payload
path. A sidecar event is an advisory indication that the associated source
may have work. It is not a sample, sample metadata, sequence number, or sample
count.

The complete relationship between the owners and the two paths is:

```text
+---------------------+       loan/send        +----------------------+
| component publisher | ----------------------> | iceoryx2 SHM stream  |
| loan + sidecar      |                         +----------+-----------+
+----------+----------+                                    |
           | notify after send attempt                       | receive/borrow
           v                                                v
+---------------------+   FD readiness   +---------------+  +-------------+
| event sidecar        | ---------------> | WaitSet /     |  | component  |
| notifier -> listener |                  | WaitContext   |  | processing |
+---------------------+                   +-------+-------+  +-------------+
                                                    |
                                                    | ReadyEvent: token only
                                                    v
                                           subscriber.receive()
                                           -> borrowed SHM guard

+---------------------+   cancellation event  +-----+----------------+
| EffectScope         | --------------------> | same WaitContext    |
| Tokio or C++ task   |   wake, then join     | cancellation source |
+---------------------+                       +--------------------+
```

The callback receives only a stable rbt `WaitToken` and event kind. It then
calls the ordinary subscriber `receive()` method. `WaitSet` moves wakeups, not
payloads.

## What wakes a component

There are two logical local wake sources in one wait context. The native
WaitSet also carries a private liveness interval used only as a cancellation
fallback; it is not a payload source.

### Stream readiness sidecar

The publisher and subscriber use the same sidecar service name, written
`<payload>/ready` and derived from the physical stream service name with the
`/ready` suffix. The publisher owns
an iceoryx2 event `Notifier`; each subscriber owns the corresponding event
`Listener`. After every completed publish/subscribe send attempt that could
have delivered a sample, the publisher notifies the sidecar. This includes a
non-transactional Control send error: iceoryx2 may have delivered to one or
more subscribers before returning the error.

The sidecar is not a second payload transport. Its notification count is not
required to equal the number of samples. One notification may precede several
samples, and a notification may be observed after the queue has become empty.
In short, readiness != sample.
The normal queue depth, history, overflow policy, borrow limit, loan limit,
message layout, and layout hash remain properties of the physical
publish/subscribe stream.

Every current publisher advertises `transport.readiness=sidecar-v1` in its
`StreamDescriptor`. Rust, C++, and Python WaitSet subscribers require that
feature and reject `none` or a legacy descriptor with no transport field
before creating either the listener or payload endpoint. This deployment
preflight prevents a new event-driven consumer from connecting to an old
provider and then sleeping forever. Readiness remains outside the message
descriptor and never changes a payload layout hash.

Readiness notification is a separate post-send operation. If it fails after a
payload send completed, rbt returns the typed `StreamError::ReadinessNotify`;
the payload sample may already have been delivered. When a non-transactional
payload send itself reports an error, rbt preserves that original send error
and logs any notification failure because delivery may already have occurred.

The subscriber creates its listener before exposing the payload endpoint. A
wait loop performs one synthetic initial drain before its first blocking wait.
That pass closes the startup race in which a sample is queued just before the
listener is attached; it still calls `receive()` in the handler and does not
prefetch or retain a sample in the waiting layer.

### Activation cancellation

Each `WaitContext` owns a cancellation event listener and notifier. Its event
service identity contains both `ProcessInstanceId` and `ActivationId`, using
the form `rbt/cancel/<process-instance>/<activation-id>`. A wake handle from an
old activation therefore cannot cancel a replacement activation in the same
process.

The cancellation source is attached to the same `WaitSet` as stream listeners.
`WaitWakeHandle::wake()` sets the cancellation flag and notifies that event.
The blocked wait consumes the event and returns `WaitExit::Cancelled`; it does
not turn an intentional lifecycle operation into a task failure.

The notifier (Rust) or `std::stop_callback` bridge (C++) is the normal
immediate cancellation path. An internal one-second native WaitSet liveness
interval is only a fallback for a failed notifier operation: it checks the
cancellation flag and never polls a payload subscriber or calls `receive()`.

The cancellation and ownership sequence is:

```text
activation cancellation requested
    -> activation token/stop request is marked
    -> WaitWakeHandle notifies the cancellation event
    -> blocked WaitSet wakes
    -> wait loop consumes the event and returns Cancelled
    -> managed blocking task returns
    -> EffectScope joins the task
    -> WaitSet guards detach and are dropped
    -> listener, subscriber, and WaitContext RAII owners are dropped
    -> existing lifecycle code removes activation-owned capabilities
```

The wake is required before the join. The active `0.10.0-rbt.2` WaitSet API
provides a timed wait but no direct stop/cancel method, so a cancellation flag
or `std::stop_token` by itself cannot unblock a thread already inside
`wait_and_process_once`.

On Unix, Rust's component runner handles both `SIGINT` and `SIGTERM` by
shutting down active `EffectScope`s. C++ `ComponentRuntime` installs handlers
for the same signals; its cooperative stop reaches the waiting task through
the `std::stop_callback`, which notifies the cancellation sidecar. Both SDKs
then join the blocking task before endpoint teardown.

## The zero-copy payload contract

The exact data path is unchanged:

1. A publisher calls `loan()` or `loan_slice()`, receiving an allocation from
   the iceoryx2 shared-memory segment.
2. The publisher writes the fixed message, user header, and/or slice elements
   in that loaned allocation.
3. `send()` consumes that native loan. rbt does not serialize it, copy it to a
   `Vec`, or send it through the Composer.
4. The sidecar notifier emits readiness after the send attempt. This event has
   no reference to the loan or to a payload object.
5. A waiting component receives the readiness event, selects the source by
   `WaitToken`, and calls the subscriber's ordinary `receive()`.
6. `receive()` returns an iceoryx2-backed `ReceivedSample` or
   `ReceivedSliceSample` guard. The component reads the fields and borrowed
   slice while that guard is alive, then drops it according to the existing
   borrow-limit and ownership rules.

The handler may drain more than one sample after one wake, subject to the
fairness bound below. It must not assume that one wake means one sample.

Tokio carries no local payload samples in this design. Rust uses Tokio for the
async component/control-plane runner and for the `spawn_blocking` task handle;
the blocking task owns the `WaitContext` and performs the wait and receive
work. There is no Tokio payload channel, intermediate `Vec`, serializer, or
callback-copy layer between `receive()` and component processing. C++ follows
the same rule with a managed `std::jthread`.

## Observable compatibility

The sidecar changes only how a subscriber is woken. Payload ABI, payload QoS
(queue, history, overflow, loan, and borrow policy), and lifecycle/capability
semantics remain unchanged. A subscriber upgraded to the WaitSet path must be
paired with an upgraded publisher that emits the matching `<payload>/ready`
notification; this is a wake-path deployment requirement, not a payload ABI or
QoS change.

## Dispatch, fairness, and stale readiness

`WaitContext` dispatches a source callback with the internal
`MAX_DRAIN_PER_SOURCE = 64` budget. The handler may call `receive()` at most 64
times for that source before returning to the native `WaitSet`. Returning to
the wait set lets another ready source and cancellation get a bounded
opportunity to run when one source is flooded.

The value 64 is an experimental implementation fairness bound. It is not a
stream queue depth, delivery guarantee, deadline, priority, or QoS setting;
applications must not use it as one.

Readiness is deliberately treated as fallible observation:

- The handler calls `receive()` until it sees `None` or reaches the bound.
- `receive() -> None` after a source wake is a normal empty/spurious wake and
  increments local empty-wakeup accounting; it is not a transport failure.
- The wait callback consumes one event-listener notification per dispatched
  source. Leaving the notification pending would cause an immediate repeated
  wake on the same file descriptor.
- An attachment id or source token that no longer maps to a live registration
  is ignored without dereferencing a torn-down endpoint. WaitSet attachment
  identities are transient implementation details; only the rbt-assigned
  `WaitToken` crosses the callback boundary.
- A synthetic initial source pass is also allowed to find no sample. It is
  race protection, not a promise that a sample exists.

`WaitStats` contains local/debug counters for waits, wakeups, empty wakeups,
cancellation wakeups, and samples reported by handlers. These counters do not
add a Composer message or extend a payload lifetime.

## Rust and C++ usage parity

The two SDKs expose the same conceptual operations and lifecycle rules:

| Concept | Rust | C++ |
| --- | --- | --- |
| Create a context | `WaitContext::new(process, activation)` | `WaitContext::create(process, activation)` |
| Register a stream source | `subscriber.event_source(WaitToken::new(n))` | `subscriber.event_source(rbt::WaitToken::new_token(n))` |
| Dispatch | `context.run(sources, handler)` | `context.run(sources, handler)` |
| Readiness value | `ReadyEvent { token, kind, synthetic }` | `ReadyEvent { token, kind, synthetic }` |
| Per-source progress | `WaitProgress` | `WaitProgress` |
| Normal exits | `WaitExit::Cancelled` / `WaitExit::Stopped` | `WaitExit::Cancelled` / `WaitExit::Stopped` |
| Typed wait failures | `WaitError` | `WaitError` inside `WaitResult<T>` |
| Wake/cancel handle | `WaitWakeHandle` | `WaitWakeHandle` |

The Rust facade re-exports these names under `rbt::stream`; C++ applications
that opt into this experimental layer include `<rbt/wait.hpp>` in addition to
the typed stream headers. The API is intentionally not part of the stable
v0.1 compatibility promise.

Rust also exposes the experimental `WaitContext::run_with_timeout` primitive
used by the synchronous Python bridge. It processes readiness exactly like
`run`, but returns `None` when its monotonic deadline expires before a normal
`WaitExit`. The deadline is implemented with iceoryx2's native timed WaitSet
operation; it does not create a timer thread, poll a payload subscriber, or
change Sensor/Control semantics. This is a local wait deadline, not a stream
QoS setting or a receive-delivery guarantee.

### Rust shape

The receive task is activation-owned and blocking. The source contains only a
borrowed sidecar listener; the handler owns the receive loop:

```rust
let subscriber = Stream::<Frame>::subscriber(binding.stream())?;
let context = WaitContext::new(ctx.process_instance(), ctx.activation_id())?;
let wake = context.wake_handle();

ctx.spawn_critical_blocking(
    "receive",
    move |cancel| {
        let mut context = context;
        let source = subscriber.event_source(WaitToken::new(1));
        let exit = context.run(std::slice::from_ref(&source), |event, budget| {
            if event.is_cancellation() || cancel.is_cancelled() {
                return Ok(WaitProgress::continue_waiting());
            }

            let mut received = 0;
            for _ in 0..budget {
                match subscriber.receive()? {
                    Some(sample) => {
                        process(sample); // process while the receive guard lives
                        received += 1;
                    }
                    None => break,
                }
            }
            Ok(if received == 0 {
                WaitProgress::empty()
            } else {
                WaitProgress::samples(received)
            })
        })?;

        match exit {
            WaitExit::Cancelled | WaitExit::Stopped => Ok(()),
        }
    },
    move || {
        let _ = wake.wake();
    },
);
```

The real component should preserve its existing critical/background task
classification and application error type. A normal scope cancellation is
handled as `WaitExit::Cancelled`; a receive, listener, or handler failure is
returned to the existing task-supervision path.

### C++ shape

C++ has the equivalent move-only context and source. Its overload accepting a
`std::stop_token` installs a `std::stop_callback` that notifies the context's
cancellation event:

```cpp
#include <rbt/wait.hpp>

auto context_result = rbt::WaitContext::create(process_instance, activation);
auto context = std::move(context_result).value();
auto wake = context.wake_handle();
auto source = subscriber.event_source(rbt::WaitToken::new_token(1U));

auto result = context.run(
    stop_token,
    {source},
    [&](rbt::ReadyEvent event, std::size_t budget) {
        if (event.is_cancellation() || stop_token.stop_requested()) {
            return rbt::WaitResult<rbt::WaitProgress>(
                rbt::WaitProgress::continue_waiting());
        }

        std::size_t received = 0U;
        for (std::size_t i = 0U; i < budget; ++i) {
            auto sample = subscriber.receive();
            if (!sample.has_value()) {
                return rbt::WaitResult<rbt::WaitProgress>(sample.error());
            }
            auto received_sample = std::move(sample).value();
            if (!received_sample.has_value()) {
                break;
            }
            process(std::move(received_sample).value());
            ++received;
        }
        return rbt::WaitResult<rbt::WaitProgress>(
            received == 0U ? rbt::WaitProgress::empty()
                           : rbt::WaitProgress::samples(received));
    });
```

The snippet is intentionally abbreviated around application-specific
`Result` handling. `EffectScope` still owns the task and its stream objects;
the task is a `std::jthread`, is never detached, and is joined during
shutdown. The C++ stop-token bridge is a wake mechanism, not a second payload
path.

## Cancellation, timeout, and failure are distinct

The waiting layer keeps lifecycle cancellation, normal handler termination,
timing policy, and actual failures separate:

- **Cancellation:** `WaitWakeHandle` or the C++ `std::stop_token` bridge wakes
  the context, which returns `WaitExit::Cancelled`. Provider disappearance,
  deactivation, activation replacement, and runtime shutdown use this normal
  path.
- **Normal stop:** a handler may return `WaitProgress::stop()` (or the
  equivalent `Stop` value), producing `WaitExit::Stopped`. This is not a task
  failure either.
- **Timeout:** the native iceoryx2 finite-timeout operation has no distinct
  `Timeout` run error; an expired timed wait is reported as an otherwise
  handled wait with no callback. The current rbt `WaitContext::run` API is a
  blocking source loop and does not invent a timeout variant. If a higher
  layer imposes a deadline, it remains that layer's timing observation and
  must not be confused with cancellation or transport failure.
- **Failure:** construction, service-name, listener/notifier, attachment,
  listener-consumption, notification, and native WaitSet errors are typed as
  `WaitError` (or C++ `WaitResult` containing `WaitError`). A handler can
  return its application/stream error while preserving the wait error as its
  source. An unexpected error from a critical wait task follows the existing
  `CriticalTaskExited -> QUIESCING -> FAILED` lifecycle path; a background wait
  task retains the existing log-only invalidation policy.

## Blocking-task ownership and RAII teardown

The wait set and its attachment guards have borrowed relationships that must
not outlive their endpoints. `WaitContext` creates the native `WaitSet` and
attaches all stream listeners plus its cancellation listener inside one
managed blocking dispatch scope. The guards are dropped before the listeners
or the `WaitSet` leave scope.

The required ownership order is:

1. Keep each receive guard alive only while component code reads its borrowed
   fields or slice.
2. Keep each `EventSource` alive only while its subscriber listener is alive.
3. Attach listeners to a `WaitSet` only for the duration of
   `WaitContext::run`; drop the `WaitSetGuard` values before the listeners and
   context-owned node.
4. On deactivation, replacement, or shutdown, request cancellation and notify
   the cancellation event before joining the blocking task.
5. Join the task before dropping the task's `WaitContext`, subscribers,
   publishers, or capability-owned endpoints. No detached wait thread may
   retain an endpoint.
6. Let the existing activation owner remove activation-owned capabilities and
   run its strict LIFO cleanup after the task has stopped. C++ endpoint,
   factory/backpressure-handler, and node destruction order remains the
   deliberate RAII order documented by the SDK.

Rust's `EffectScope` uses `spawn_critical_blocking`/`spawn_background_blocking`
with a wake callback and joins the Tokio blocking handle during shutdown.
C++ `EffectScope` uses `std::jthread`, requests cooperative stop, and joins it
before LIFO cleanup continues. A dropped Rust scope still sends the wake as a
synchronous safety net; explicit shutdown is the path that can await all
cleanup.

## What this does not wait on

The WaitSet layer is limited to local stream data-plane readiness and the
activation cancellation source. It does not replace or attach to the
Composer's control-plane TCP/NDJSON connection, reconnect/backoff logic,
snapshot watcher, lifecycle commands, registry reconciliation, CLI
inspection, or low-rate pressure snapshot publication. Tokio continues to
drive the Rust control-plane async reader/snapshot `select!`; C++ retains its
existing runtime/control loop.

No control-plane message carries a local payload sample, and no WaitSet wake
causes the Composer to enter the payload path. A disconnected Composer still
does not by itself close a healthy local stream or invoke activation cleanup.

## Scope and validation boundary

This document records the architecture and usage contract. Exact local
measurements are collected in the [WaitSet milestone report](waitset-report.md);
they are evidence for the recorded host and method, not an SLA, scheduling
guarantee, or universal performance claim. The accepted unified verifier and
fresh sanitizer evidence are retained at the exact paths recorded in that
report.
The pinned upstream behavior and the separation between source evidence and rbt
policy are recorded in [`waitset-notes.md`](waitset-notes.md).

Related lifecycle and ownership rules remain in
[`architecture.md`](architecture.md), [`rust-sdk.md`](rust-sdk.md), and
[`cpp-sdk.md`](cpp-sdk.md).

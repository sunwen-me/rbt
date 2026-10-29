# iceoryx2 0.9.3 WaitSet research notes

This note records the evidence used for the rbt event-driven waiting spike. It
deliberately separates upstream documentation, behavior observed in the pinned
source, rbt policy, and local experimental results. The workspace pins the Rust
crate to `iceoryx2 = "=0.9.3"`; the C++ bootstrap pins tag `v0.9.3`, commit
`6ef7c55176a8487ae7ca53c2e0649d5df9a1b3b4`.

The non-negotiable data-path invariant is:

> **WaitSet moves wakeups, not payloads.**

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

There is no Tokio payload channel, intermediate `Vec`, serializer, or
callback-copy layer in this design.

## iceoryx2 documented behavior

The Rust API is defined in the pinned crate's `src/waitset.rs`.

- `WaitSetBuilder::new().create::<Service>()` creates a WaitSet
  (`waitset.rs:477-520`). The builder can select signal handling mode.
- `attach_notification`, `attach_deadline`, and `attach_interval` are the only
  attachment operations (`waitset.rs:648-712`). There is no `attach_guard`,
  guard-condition, `stop`, `cancel`, or explicit wake method in 0.9.3.
- Notification and deadline sources must implement
  `SynchronousMultiplexing`, which extends `FileDescriptorBased`.
- `wait_and_process` repeats until the callback returns `Stop` or signal
  handling ends it. `wait_and_process_once` blocks once and processes the
  ready set. `wait_and_process_once_with_timeout` also accepts a timeout
  (`waitset.rs:715-932`).
- A callback receives a `WaitSetAttachmentId`, not an event payload. It maps
  the id back to a guard with `has_event_from` or an equivalent guard-derived
  lookup.
- Dropping `WaitSetGuard` detaches the source. The guard borrows both the
  WaitSet and the attached object, so both must outlive it
  (`waitset.rs:443-472`).
- `WaitSetRunError` contains `InsufficientPermissions`, `InternalError`, and
  `NoAttachments`; attachment errors contain `InsufficientCapacity`,
  `AlreadyAttached`, `InternalError`, and `InsufficientResources`
  (`waitset.rs:239-306`). A finite timeout has no distinct `Timeout` result;
  it returns `AllEventsHandled` with no callback if nothing fired.
- Event `Listener` implements the required FD-backed interfaces and provides
  `try_wait_one`, `try_wait_all`, timed wait, and blocking wait operations.
  The application must consume listener notifications after a WaitSet wake.
- The event-multiplexing examples attach multiple listeners to one WaitSet.
  They warn that failing to consume a listener notification causes immediate
  repeated wakeups.

The installed C++ API in
`target/iox2-cpp-install/include/iceoryx2/v0.9.3/iox2/waitset.hpp` has the same
model:

- `WaitSetBuilder::create<S>()`, `attach_notification`, `attach_deadline`,
  `attach_interval`, and the three wait variants are exposed
  (`waitset.hpp:117-244,257-280`).
- `WaitSet`, `WaitSetGuard`, `WaitSetAttachmentId`, `Listener`, and `Notifier`
  are move-only RAII types. A guard destructor detaches its source.
- Listener overloads are backed by its file descriptor; arbitrary
  `FileDescriptorBased` objects can also be attached.
- There is no C++ WaitSet stop/cancel/wakeup method. A `std::stop_token` alone
  cannot wake a blocked WaitSet.

## iceoryx2 source-observed behavior

These points come from the exact 0.9.3 sources in the local Cargo registry and
are not treated as broader API promises.

- Publish/subscribe `Subscriber` exposes `has_samples()` and `receive()` but
  does **not** implement `FileDescriptorBased` or
  `SynchronousMultiplexing` (`src/port/subscriber.rs:384-408`). The C++
  subscriber has the same limitation. A pub/sub subscriber therefore cannot
  be attached directly to WaitSet in 0.9.3.
- Event `Listener` does implement those interfaces
  (`src/port/listener.rs:139-154`). IPC listeners use a Unix datagram-backed
  event primitive.
- On Linux the recommended IPC reactor is epoll and sources are attached for
  read readiness. One epoll collection is capped at 512 ready descriptors.
- `wait_and_process_once` is the timeout variant with `Duration::MAX`.
  Waiting on an empty set returns `NoAttachments`.
- The implementation first collects all ready file descriptors, resets
  deadlines, handles missed deadlines, then invokes the notification callbacks
  (`waitset.rs:616-645`). Multiple descriptors can therefore be reported by
  one wait call.
- A `WaitSetAttachmentId` internally includes WaitSet address/FD information.
  rbt must not expose or persist that implementation identity. It is used only
  transiently to map to an rbt token while the guards are alive.
- `WaitSet` contains `RefCell` state and is not concurrently usable. The
  WaitSet, its guards, its listeners, `receive()`, and sample processing remain
  on one managed blocking task/thread.
- The WaitSet callback does not consume an event listener. If rbt leaves the
  datagram pending, epoll reports it again. Conversely, consuming a readiness
  notification does not receive or borrow a pub/sub sample.
- A notification is not documented as meaning exactly one pub/sub sample.
  It means only that the associated readiness source fired. A subsequent
  `receive()` may return `None`, and one wake may allow more than one sample to
  be drained.
- Rust's default WaitSet reactor does not provide a cross-thread stop handle.
  C++ has the same limitation. Callback `Stop` matters only after some attached
  source has already woken the call.

## rbt chosen policy

### Stream readiness

Because a 0.9.3 pub/sub subscriber is not attachable, every rbt stream owns a
small iceoryx2 **event sidecar** named `<payload>/ready`, derived from the
physical stream service name. The publisher owns a notifier and each
subscriber owns a listener. After every pub/sub send attempt that can have
delivered a sample, including Control's non-transactional partial-delivery
error, the publisher emits a readiness notification.

The sidecar is not a payload transport. Its event id and notification count
are not sample identity or sample count. The existing publisher loan,
iceoryx2 SHM service, subscriber borrow, queue depth, history, overflow,
borrow limit, loan limit, message layout, layout hash, and ABI remain the only
payload path and semantics.

Readiness notification is a separate post-send operation. A failure is exposed
as the new typed `StreamError::ReadinessNotify` when the payload send completed;
the sample may already have been delivered. For a non-transactional payload
send error, rbt preserves the original send error (and logs a readiness failure
if notification also fails) because delivery may already have occurred.

The listener is created before the subscriber is exposed. A wait loop performs
one synthetic initial drain before its first blocking wait, closing the
construction race where a sample was queued just before the readiness listener
connected. Synthetic readiness still calls the ordinary `subscriber.receive()`;
it does not prefetch or hold a sample in the waiting layer.

### Thin wait abstraction

rbt uses experimental/internal `WaitContext`, `EventSource`, `WaitToken`,
`ReadyEvent`, `WaitStats`, and typed `WaitError` concepts. `WaitToken` is an
rbt-assigned stable integer, never a pointer address. `EventSource` contains
only a borrowed readiness listener and its token. `ReadyEvent` says which
source woke; it never carries a sample.

The underlying guards cannot be stored self-referentially with their WaitSet,
so `WaitContext` owns its event endpoints and runs a blocking dispatch scope:
it creates the WaitSet, attaches all listeners, processes events through a
callback, then drops guards before sources and the WaitSet leave scope. User
and runtime code never sees attachment internals.

The handler receives a private `MAX_DRAIN_PER_SOURCE = 64` budget. It calls
the original subscriber's `receive()` repeatedly until empty or that budget is
used. Returning to WaitSet after the budget gives other ready sources and
cancellation another bounded opportunity. This number is an internal fairness
experiment, not stream QoS or stable API behavior.

Spurious and stale readiness are normal: `receive() -> None` increments an
empty-wakeup counter and is not fatal. Readiness for a removed token is ignored
without dereferencing a torn-down endpoint.

### Cancellation and lifecycle

Each wait context has an event notifier/listener pair whose service identity
contains both `ProcessInstanceId` and `ActivationId`. An old activation's wake
handle cannot cancel a new activation.

Rust runs one blocking WaitSet/data-processing loop as an activation-owned
managed blocking task. `EffectScope` cancellation performs this order:

```text
cancel activation token
-> notify the cancellation event source
-> blocked WaitSet wakes
-> processing task returns Cancelled
-> join the blocking task
-> drop WaitSet guards/listeners/subscribers
-> remove capability according to the existing lifecycle path
```

The cancellation notifier is the normal immediate wake path. The native
WaitSet also has an internal one-second liveness interval as a fallback if a
notifier operation fails; that interval checks cancellation state only and
never polls a payload subscriber or calls `receive()`.

C++ registers a `std::stop_callback` that notifies the equivalent cancellation
event. It uses the existing `std::jthread`/EffectScope ownership and joins; no
detached cancellation thread is added.

On Unix, Rust's component runner handles both `SIGINT` and `SIGTERM` by
shutting down active `EffectScope`s. C++ installs the same two signal handlers;
the runtime requests cooperative stop, the `std::stop_callback` notifies the
WaitSet cancellation source, and the task is joined. Signals therefore use the
same graceful wake-before-join path as lifecycle cancellation.

Normal provider disappearance, activation replacement, deactivate, and
runtime shutdown produce `Cancelled` and do not become critical failure.
Create/attach/wait/listener failures are typed `WaitError`s. An unexpected
error returned by a critical wait task flows through the existing
`CriticalTaskExited -> QUIESCING -> EffectScope shutdown -> capability removal
-> FAILED` path. Background tasks retain the existing log-only invalidation
policy.

### Statistics

`WaitStats` uses relaxed `AtomicU64` counters for waits, wakeups, empty wakeups,
cancellation wakeups, and samples processed. Stats are local/debug evidence;
no per-wake Composer message is introduced.

## Observable compatibility

The readiness sidecar is additive to the local receive path. Payload ABI,
payload QoS (queue/history/overflow/loan/borrow policy), and lifecycle and
capability semantics are unchanged. A subscriber upgraded to the WaitSet path
must pair with an upgraded publisher: the publisher must emit the matching
`<payload>/ready` notification. This is a deployment compatibility requirement
for the wake path, not a payload ABI or QoS change.

v0.2 makes that requirement explicit as
`StreamDescriptor.transport.readiness=sidecar-v1`. A missing pre-v0.2 field is
parsed as `none`, and current Rust/C++/Python consumers return the typed
missing-feature error before opening transport.

## Local milestone observation

The current implementation and its exact local measurements are summarized in
[`waitset-report.md`](waitset-report.md). The report links the retained
benchmark, fairness, lifecycle, idle-soak, mixed-signal, and pressure-soak
logs. Those observations validate the implementation on the recorded host;
they are not inferred from upstream source and do not establish an SLA or a
universal performance result.

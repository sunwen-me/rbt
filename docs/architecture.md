# Architecture

RBT 0.8 keeps the local, two-plane runtime introduced in v0.1. The Composer
makes lifecycle and binding decisions; maintained native iceoryx2 moves typed
payloads between Rust, C++, and Python component processes. The current SDK
also provides typed local RPC, identified real-clock time APIs, and local TF
buffers. See the [documentation index](README.md) and
[developer guide](development.md) for entry points.

```text
                         TCP + NDJSON
                 lifecycle / snapshots / bindings
                              │
                    ┌─────────▼─────────┐
                    │      Composer     │
                    └───┬───────────┬────┘
                        │           │
       snapshot/control │           │ snapshot/control
                        │           │
       ┌────────────────▼─┐     ┌───▼────────────────┐
       │ Rust / C++ /      │     │ Rust / C++ /       │
       │ Python Component  │     │ Python Component   │
       └──────────┬────────┘     └─────────┬───────────┘
                  │  iceoryx2 shared memory│
                  └───────────┬────────────┘
                              ▼
                     typed Streams and RPC
```

The control plane carries `ComponentSnapshot`, `RequirementSpec`, resolved
bindings, stream and RPC capability descriptors, revisions, and diagnostics.
It never carries `Frame`, `Detection`, `Image`, `PointCloud`, or any other
payload. Stream samples and RPC requests/responses stay on their native local
endpoints. A component can therefore keep a valid activation and continue
using its local data-plane endpoints while the Composer is unavailable.

Typed RPC is bounded local request/response IPC with native loans and guards;
the existing `ActivationCtx::rpc_server` and `ActivationCtx::require_rpc`
helpers bind endpoints to capability roles and activation lifetime. Python's
`ActivationContext` exposes `provide_rpc` and `require_rpc`; C++'s
`ActivationContext` exposes `provide_rpc`, with resolved RPC bindings available
through its existing `require` API. See
[`rpc.md`](rpc.md) for endpoint, cancellation, and capacity behavior.

Time APIs use the existing `HostClock`, `DeviceClock`, `Timestamp`, and
`ClockMapping` objects in Rust, C++, and Python. Runtime deadlines and
cancellation continue to use real monotonic time; there is no simulation-time
mode. See [`time.md`](time.md) for clock identity and mapping rules.

TF reuses those identified timestamps and mapping checks. The Rust
`TransformActivationExt` methods `provide_dynamic_tf`, `provide_static_tf`,
`require_dynamic_tf`, and `require_static_tf` use the existing `ActivationCtx`
and `EffectScope`. Dynamic transforms and complete static updates use native
Streams; a static provider also serves a bounded full-set snapshot over RPC.
Each consumer ingests into its own bounded `TransformBuffer`, and each lookup
is local. TF does not route payload through Composer, issue an RPC per lookup,
or introduce a global server or a new runtime. See [`tf.md`](tf.md) for the
Rust, C++, and Python transport and lifecycle APIs.

## Runtime identities

These names have separate meanings and must not be collapsed into one counter.

| Identity | Meaning | Lifetime |
| --- | --- | --- |
| `ComponentId` | Logical name such as `detector` | Stable across process restarts |
| `ProcessInstanceId` | UUID for one running component process | New on every process start |
| `ActivationId` | Process-local accepted activation number | New for every accepted activation attempt |
| `CapabilityInstanceId` | UUID for one activation-owned capability | New whenever the capability is recreated |
| `ComposerEpoch` | UUID for one Composer process | New on every Composer start |
| `Revision { epoch, sequence }` | Ordered control revision inside one epoch | Sequence is comparable only within its epoch |

`CapabilityInstanceId` is part of the physical endpoint name:

```text
rbt/cap/<CapabilityInstanceId>/stream  # Stream
rbt/cap/<CapabilityInstanceId>/rpc     # RPC
```

That prevents delayed cleanup from an old activation from colliding with a
replacement endpoint.

## Lifecycle and ownership

The Rust runner and the C++ runtime expose the same ownership model:

```text
process
└── prepared component instance
    └── activation
        ├── resolved requirement bindings
        ├── EffectScope
        │   ├── critical tasks
        │   ├── background tasks
        │   └── LIFO cleanup callbacks
        ├── data-plane endpoints
        └── provided capabilities
```

Preparation is process-lifetime work. It is appropriate for loading a model or
opening immutable configuration and runs once before the first activation. An
activation is a repeatable, binding-specific lifetime. `activate` creates
endpoints, publishes capabilities, and starts tasks in its `EffectScope`.
Deactivation requests cooperative stop, joins managed tasks, runs cleanup in
strict LIFO order, and removes every capability owned by that activation.

The state machine is:

```text
DECLARED -> PREPARING -> BLOCKED <-> ACTIVATING -> ACTIVE
                              ▲         │            │
                              │         └────────────┘
                              │              QUIESCING
                              └─────────────────┘

PREPARING/ACTIVATING failures may become FAILED.
ACTIVE critical-task failure: ACTIVE -> QUIESCING -> FAILED.
```

`BLOCKED` means that a requirement cannot currently be resolved; it is not a
process crash. `FAILED` records a component or activation error that needs
operator attention or a new process. A background task exit is observable but
does not invalidate an otherwise active activation. A critical task exit does.

## Composition and reconciliation

1. A component prepares and announces its requirements and complete snapshot.
2. Composer registers snapshots and builds a capability registry from active
   activation-owned capabilities.
3. A requirement is eligible only when an active provider advertises the exact
   capability type and a complete descriptor for its Stream or RPC endpoint.
4. Reconciliation emits an `Activate` or `Deactivate` command with an
   epoch-scoped revision and expected process/activation identity.
5. The component accepts only a newer revision in the current epoch and only
   deactivates the expected current process and activation.
6. Every local lifecycle change publishes a fresh complete snapshot.

The current snapshot version 4 carries low-rate `stream_pressures` and
`rpc_pressures` observations with their effective resource metadata. Stream
pressure first appeared in snapshot version 3; version 4 added RPC pressure.
Each entry is attached to an active capability only when the reporting
`ProcessInstanceId` and physical service name both match its descriptor. The
service name contains `CapabilityInstanceId`, so telemetry from a replaced
provider cannot migrate to the replacement. Components publish changed
pressure snapshots at most once per second; these diagnostic updates do not
advance topology revision, start reconciliation, or enter the payload path.

Two live processes claiming one `ComponentId` are quarantined as a process
identity conflict. The Composer does not silently choose a winner. When the
conflict disappears, the surviving process's validated snapshot is imported.

## Composer outage and recovery

The component's control client reconnects with `100 ms`, `200 ms`, `500 ms`,
and `1 s` backoff. A disconnected control plane changes connection status and
logging only; it does not close an active stream or invoke activation cleanup.

On reconnect the component sends `HELLO` followed by a complete snapshot. A
new Composer starts with a fresh `ComposerEpoch` and enters `Recovering`. It
imports snapshots without issuing lifecycle commands during the recovery
barrier. It then rebuilds the registry from active snapshots, validates
bindings, and reconciles only what is actually missing.

For a healthy graph this is intentionally a no-op:

```text
reconciliation: 0 deactivate, 0 activate
```

If a provider died during the outage, it cannot reconnect or provide a
snapshot. Its dependent binding is stale after recovery; reconciliation
deactivates the dependent and downstream dependents. Recovery reconstructs
reality rather than recreating healthy resources.

The current local implementation uses a two-second recovery barrier. As in
the v0.1 runtime, there is no persisted expected-component set or explicit
end-of-snapshot marker, so the timeout is not a distributed membership
guarantee.

## C++ ownership and thread semantics

The C++ SDK follows RAII and move-only ownership:

- `LoanedSample`/`LoanedSliceSample` owns a native loan until `send()` consumes
  it or the guard is destroyed. An unsent loan is released by its destructor.
- `ReceivedSample`/`ReceivedSliceSample` owns the borrowed native sample while
  the guard is alive. `ReceivedSliceSample::payload()` returns a
  `std::span<const Element>` into that sample; the span must not outlive the
  guard.
- `ActivationContext` borrows the runtime-owned activation scope. The runtime
  owns the `EffectScope`, tasks, endpoints, and capability registrations.
- `EffectScope` uses `std::jthread` and `std::stop_token`, requests cooperative
  stop, joins every managed thread, and runs cleanup in LIFO order. Runtime
  tasks must not be detached.
- C++ publishers/subscribers and capability handles are non-copyable and
  move-only where they own native resources. `Node`, factory, backpressure
  handler, and endpoint destruction order is deliberate.

The generated C++ structs live under `namespace rbt::<schema namespace>`; the
runtime/component types live under `namespace rbt`. If an underlying transport
API uses a `Node`, that is an iceoryx2 transport object. An rbt `Component` is
the lifecycle unit and is not interchangeable with that transport term.

## Logging and diagnostics

Lifecycle and binding events should include structured fields for `component`,
`process_instance`, `activation`, `capability_instance`, and
`composer_epoch`. Normal operation should log lifecycle, binding, failure, and
recovery changes; per-frame payload logs belong to debug/test-only modes. Rust
uses `tracing`; C++ event data uses the same identity vocabulary so mixed
process logs can be correlated.

`rbt stream pressure <CapabilityInstanceId>` renders the matched endpoint's
declared resource policy and available local counters. Aggregate subscriber
count is transport-observed; per-subscriber queue occupancy and transport Drop
counts are explicitly unavailable unless a message-aware test estimates gaps.

## Architectural invariants

These are correctness properties, not performance claims:

1. Composer failure does not imply provider failure.
2. Capability existence is backed by a live valid activation.
3. Critical task failure invalidates its activation.
4. Healthy Composer recovery reconstructs reality rather than recreating it.
5. Payload does not pass through Composer.
6. Local message ABI is not a network wire format.

Tests exercising the lifecycle/recovery invariants live under
`crates/rbt-compose/tests`; data-plane and ABI tests are covered by
`crates/rbt-iox2/tests` and `sdk/cpp/tests`. The mapping and commands are in
[`testing.md`](testing.md).

# Core concepts

The RBT 0.8 SDK uses one vocabulary across Rust, C++, Python, Composer, and the
CLI. See the [documentation index](README.md) and
[developer guide](development.md) for entry points.
Each concept below has a definition, a small example, and an anti-pattern.

## Component

**Definition.** A `Component` is one independently running process-level
implementation with a stable `ComponentId`, a one-time preparation step, a set
of requirements, and repeatable activations.

**Example.** `detector` prepares its fake model once, requires `Camera` under
the name `camera`, and starts an inference task for each accepted activation.

**Anti-pattern.** Treating every activation as a new process or loading the
same model in every `activate` call defeats preparation lifetime and makes
recovery recreate work unnecessarily.

## Capability

**Definition.** A `Capability` is a typed resource a live activation provides
to other components. Its semantic type is stable; each publication also has a
fresh `CapabilityInstanceId` and a transport descriptor for either a Stream
or an RPC endpoint.

**Example.** A camera provides capability type `Camera` backed by an `Image`
stream at `rbt/cap/<uuid>/stream`; an `EchoRpc` provider can expose its bounded
request/response endpoint at `rbt/cap/<uuid>/rpc`.

**Anti-pattern.** Registering a capability before activation succeeds or
leaving it in the registry after its activation has been torn down creates a
phantom provider.

## Requirement

**Definition.** A `Requirement` is a named, typed dependency that must resolve
to one active provider before an activation may start.

**Example.** The detector declares `RequirementSpec::typed::<Camera>("camera")`.
Composer binds it to one active `Camera` capability and includes that binding
in the activation command.

**Anti-pattern.** Looking up a provider by an untyped display name or silently
accepting two providers for one requirement makes the graph ambiguous.

## Activation

**Definition.** An `Activation` is one binding-specific lifetime owned by a
component process. It owns its `ActivationId`, `EffectScope`, tasks, endpoints,
bindings, and provided capabilities.

**Example.** Replacing `camera-a` with `camera-b` deactivates the detector's
old activation and accepts a new activation with a new output capability; the
detector process and prepared model remain.

**Anti-pattern.** Reusing an old activation's endpoint or allowing a delayed
deactivation to act on a newer activation risks tearing down the replacement.

## EffectScope

**Definition.** `EffectScope` is the activation-owned cancellation and cleanup
scope. It supervises critical/background tasks and runs registered cleanup in
strict LIFO order.

**Example.** A component registers its subscriber, publisher, and task in the
activation scope. Shutdown requests stop, joins managed tasks, then invokes the
last cleanup callback first.

**Anti-pattern.** Calling `tokio::spawn` or `std::thread::detach` for an
activation task leaves work alive after deactivation and bypasses failure
supervision.

## Stream

**Definition.** A `Stream` is a typed local iceoryx2 publish/subscribe endpoint
described by service name, message ABI, and `StreamProfile`. It is the data
plane, not a Composer topic.

**Example.** `Stream::<Frame>::publisher(&descriptor)` loans a `Frame` directly
from shared memory; a subscriber receives a guard over the same native sample.
`SliceStream::<Image>` represents one fixed header plus a homogeneous payload
slice.

**Anti-pattern.** Serializing a payload through Composer, opening a subscriber
with a made-up descriptor, or treating an endpoint name as a globally stable
topic makes local ownership and ABI checks unreliable.

## Typed RPC

**Definition.** A typed RPC endpoint is bounded local request/response IPC
over native iceoryx2. Its descriptor records request and response message
contracts and resource limits; request and response payloads do not pass
through Composer.

**Example.** A Rust provider uses `ActivationCtx::rpc_server` and a client uses
`ActivationCtx::require_rpc` for the same capability role. Both endpoints are
owned by their activations. See [`rpc.md`](rpc.md) for the Rust, C++, and
Python APIs and their capacity and cancellation behavior.

**Anti-pattern.** Treating a timeout or client cancellation as proof that the
provider did not execute the request, or retrying without an application-level
idempotency rule, can duplicate side effects.

## Time

**Definition.** Time readings use an explicit clock identity, signed
nanoseconds, and generation. `HostClock`, `DeviceClock`, `Timestamp`, and
`ClockMapping` are shared concepts across Rust, C++, and Python.

**Example.** A driver can retain a device capture timestamp and map it to the
host clock using a validated `ClockMapping`; runtime deadlines still use real
monotonic clocks. See [`time.md`](time.md) for construction, comparison, and
mapping rules.

**Anti-pattern.** Comparing raw nanosecond values from unrelated clocks or
using sensor timestamps as runtime shutdown deadlines invents an epoch or
lets data time stall lifecycle work.

## TransformBuffer

**Definition.** A `TransformBuffer` is one consumer's bounded local graph and
history for validated transforms. Dynamic records and complete static updates
arrive on Streams; a static provider also supplies a full snapshot over RPC.

**Example.** The activation helpers ingest the selected provider streams and
snapshot into a local buffer, then application code calls `lookup_at` with an
explicit identified timestamp. Lookups use only that local buffer. See
[`tf.md`](tf.md) for frame, time, transport, and lifecycle behavior.

**Anti-pattern.** Making a lookup call a remote provider, keeping one process-
global transform database, or routing transform payload through Composer adds
a central data dependency and escapes the existing activation ownership.

## Message ABI

**Definition.** A Message ABI is the canonical identity and native layout of a
fixed or header-plus-slice message: namespace/name, ABI major, shape, sizes,
alignments, field signature, and `LayoutHash`.

**Example.** `rbt.sensor/Image@1` is a slice with a 32-byte `ImageHeader`,
one-byte elements, and layout hash
`943fa4008cf0f55f89a281b051c69a6f`. Generated Rust and C++ code assert the
same offsets before the endpoint opens.

**Anti-pattern.** Comparing only `Image` by short name, changing field order
without changing the descriptor/hash, or treating the native shared-memory
layout as a network serialization format silently crosses an ABI boundary.

## Composer

**Definition.** Composer is the low-rate control-plane authority that imports
component snapshots, resolves requirements, reconciles lifecycle state, and
serves read-only inspection. It is not in the payload path.

**Example.** When `Camera` disappears, Composer sends `Deactivate` to the
detector and viewer. When Composer itself is killed, valid local activations
continue and reconnecting snapshots let a new Composer reconstruct the graph.

**Anti-pattern.** Making a data task wait on a Composer socket, or using a
Composer restart as a reason to deactivate every component, turns a control
plane outage into a payload outage.

## Terms that must remain distinct

- `ComponentId` is logical identity; `ProcessInstanceId` is one process start.
- `ActivationId` identifies one accepted local activation; `Revision` orders
  control commands inside one `ComposerEpoch`.
- `CapabilityInstanceId` identifies a physical activation-owned endpoint; the
  semantic capability type is separate.
- `Stream` is the public data-plane term. `service` is reserved for transport
  or internal iceoryx2 wording.
- `Component` is the rbt lifecycle unit. A transport `Node` is an iceoryx2
  object and is not an rbt component.

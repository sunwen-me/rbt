# Rust SDK

The Rust SDK package is version `0.8.0`. Application components should depend
on the `rbt` facade. The workspace crates remain available for advanced
transport or Composer work, but the facade is the intended public namespace.
See the [developer docs index](README.md) for the guide map and current
version axes.

## Supported facade versus runtime internals

The supported application surface is what `rbt` re-exports through
`rbt::component`, `rbt::control`, `rbt::error`, `rbt::rpc`, `rbt::stream`,
`rbt::time`, `rbt::time_stream`, `rbt::tf`, `rbt::types`, and the small
prelude. The workspace crates remain separately public where cross-crate
implementation and advanced transport work require it, but their un-reexported
items are runtime/internal surface, not an application compatibility promise.
Depending on them directly is an explicit opt-in to lower-level 0.x churn.

This is a 0.x API: source APIs may evolve. The message ABI, layout-hash,
control-version, and snapshot-version rules in `compatibility.md` are hard
boundaries and must never change silently.

## Public surface

```text
rbt::prelude
├── Result
├── component::{ActivationCtx, Component}
├── rpc::{Rpc, FixedSliceRpc, SliceFixedRpc, SliceRpc}
├── stream::Stream
└── types::{Capability, Requirement, StreamProfile}

rbt::component
├── Component, ActivationCtx, EffectScope
├── run_component
└── lifecycle/error/task types

rbt::control
├── serve
├── CONTROL_PROTOCOL_VERSION
└── COMPOSER_SNAPSHOT_VERSION

rbt::rpc
├── fixed/fixed-slice/slice-fixed/slice request-response endpoints
├── clients, servers, loans, response guards, errors, and resource limits
└── descriptor preflight helpers

rbt::stream
├── Stream<T> / Publisher<T> / Subscriber<T>
├── SliceStream<M> / SlicePublisher<M> / SliceSubscriber<M>
└── loan and receive guards

rbt::time / rbt::time_stream
├── clock identities, timestamps, mappings, and checked arithmetic
└── timing metadata in ordinary byte-slice streams

rbt::tf
├── transform math, bounded local history, and lookup APIs
├── native TF transport endpoints
└── TransformActivationExt for activation-owned TF bindings

rbt::types
├── identity, capability, requirement, binding, and lifecycle types
└── generated/built-in message and descriptor types
```

The main workspace packages are:

| Package | Role |
| --- | --- |
| `rbt` | User-facing facade and aggregate error/result types |
| `rbt-core` | `ComponentId`, `Capability`, `RequirementSpec`, bindings, state, revisions |
| `rbt-types` | `ShmSafe`, fixed/slice ABI descriptors, built-in message types |
| `rbt-iox2` | Typed iceoryx2 data-plane endpoints and loan guards |
| `rbt-compose` | Component runner, snapshots, protocol, Composer, reconciliation |
| `rbt-time` / `rbt-time-stream` | Clock and timestamp APIs; timing prefixes for byte-slice streams |
| `rbt-tf` / `rbt-tf-transport` | Transform math and local buffers; native transport and activation bindings |
| `rbt-idl` | `.rbt` parser and deterministic Rust/C++/descriptor/file-codec generation (IDL language v1, descriptor v1) |
| `rbt-mcap` | Bounded MCAP reader/writer tooling using the `rbt-canonical-v1` message encoding |
| `rbt-cli` | Project scaffolding and component/message templates, static checks, IDL tooling, local build/run, Composer inspection, and MCAP record/info/play |

The facade's `RbtError` keeps component, requirement, stream, RPC, RPC endpoint,
control-plane, time, TF, and TF transport failures as separate
source-preserving variants. Stream errors include
`AbiMismatch`, `InvalidPayloadLength`, `InvalidStreamLimit`, `QueueFull`,
`UnableToDeliver`, `LoanPoolExhausted`, `BorrowLimitReached`, and the remaining
transport/open/loan/receive failures.

## Component lifecycle

Implement `Component` with four decisions:

1. `prepare(&Config)` returns one process-lifetime component instance.
2. `id()` returns the stable logical `ComponentId`.
3. `requirements()` declares named capability types.
4. `activate(&mut ActivationCtx)` opens endpoints, provides capabilities, and
   starts activation-owned tasks.

The runner calls `prepare` once. Composer may then send multiple activations to
the same process as providers appear, disappear, or are replaced. A successful
activation owns its `ActivationId`, bindings, `EffectScope`, endpoints, and
provided capabilities. The runner removes all of them during deactivation.

## Minimal provider

This is the shape used by `examples/rust/camera` (error handling and payload loop
shortened only for readability):

```rust
use std::time::Duration;

use anyhow::Result;
use async_trait::async_trait;
use rbt::{
    component::{run_component, ActivationCtx, Component},
    stream::SliceStream,
    types::{
        Capability, CapabilityInstanceId, ComponentId, Image, PixelFormat,
        RequirementSpec, StreamProfile,
    },
};

struct Camera;
impl Capability for Camera {
    const TYPE_NAME: &'static str = "Camera";
}

struct CameraComponent;

#[async_trait]
impl Component for CameraComponent {
    type Config = ();
    type Error = anyhow::Error;

    async fn prepare(_: &Self::Config) -> Result<Self> {
        Ok(Self)
    }

    fn id(&self) -> ComponentId {
        ComponentId::from("camera")
    }

    fn requirements(&self) -> Vec<RequirementSpec> {
        Vec::new()
    }

    async fn activate(&mut self, ctx: &mut ActivationCtx<'_>) -> Result<()> {
        let capability = CapabilityInstanceId::new();
        let descriptor = SliceStream::<Image>::descriptor(
            capability.stream_service_name(), StreamProfile::Sensor);
        let publisher = SliceStream::<Image>::publisher(&descriptor, 64)?;
        ctx.provide::<Camera>(capability, descriptor);

        ctx.spawn_critical("publish", move |token| async move {
            let mut sequence = 0_u64;
            let mut tick = tokio::time::interval(Duration::from_millis(100));
            loop {
                tokio::select! {
                    _ = token.cancelled() => return Ok(()),
                    _ = tick.tick() => {
                        let mut sample = publisher.loan_slice(64)?;
                        sample.header_mut().sequence = sequence;
                        sample.header_mut().width = 64;
                        sample.header_mut().height = 1;
                        sample.header_mut().stride = 64;
                        sample.header_mut().encoding = PixelFormat::GRAY8;
                        sample.payload_mut().fill(0xA1);
                        sample.send()?;
                        sequence += 1;
                    }
                }
            }
        });
        Ok(())
    }
}

#[tokio::main]
async fn main() -> Result<(), rbt::ControlPlaneError> {
    run_component::<CameraComponent, _>((), "127.0.0.1:7447").await
}
```

`examples/rust/camera/src/main.rs` is the exact runnable reference; it adds
structured fields to its logs and uses `camera-a` as its component id. The
detector and viewer show the consumer side with `ctx.binding::<Camera>(...)`,
`SliceStream::<Image>::subscriber(...)`, and another activation-owned publisher.

## Requirements, bindings, and capabilities

Declare a requirement with the stable capability role, not a Rust type name
that another process cannot know:

```rust
fn requirements(&self) -> Vec<RequirementSpec> {
    vec![RequirementSpec::typed::<Camera>("camera")]
}

let binding = ctx.binding::<Camera>("camera")?;
let subscriber = Stream::<Frame>::subscriber(binding.stream())?;
```

`ctx.provide` records the complete owner tuple:
`CapabilityInstanceId`, `ComponentId`, `ProcessInstanceId`, `ActivationId`,
and `StreamDescriptor`. It is published to Composer only after `activate`
returns `Ok(())`. `binding` rejects a missing name, duplicate binding, or
capability-type mismatch with `RequirementError`.

## RPC, time, and transforms

`ActivationCtx` supports activation-owned RPC through `rpc_server` and
`require_rpc`, as well as lower-level `provide_rpc`. The typed local RPC
surface and its bounded request/response shapes are described in
[`rpc.md`](rpc.md). Clock identities, timestamps, mappings, and stamped byte
streams are covered in [`time.md`](time.md).

The facade's `rbt::tf` module re-exports local transform math, bounded buffers,
and native TF transport. Import `rbt::tf::TransformActivationExt` to add
`provide_dynamic_tf`, `provide_static_tf`, `require_dynamic_tf`, and
`require_static_tf` to `ActivationCtx`; these helpers bind native endpoints to
the activation lifecycle. See [`tf.md`](tf.md). Physical-camera hardware
acceptance remains NOT RUN, and generic TF MCAP recording/replay remains
unverified.

## Task policy

Use `ctx.spawn_critical` for work whose exit means the activation is no longer
valid (inference, a required receiver, or a required publisher). Use
`ctx.spawn_background` for auxiliary work such as metrics. Both are owned by
the activation's `EffectScope` and receive a cancellation token.

Do not use `tokio::spawn` for an activation task. A process-global task is the
only exception, and it must be documented as process-global rather than hidden
from the activation owner. Cancellation during normal teardown is not reported
as an unexpected task exit. A critical task that returns an error or panics
emits one supervisor event and drives `ACTIVE -> QUIESCING -> FAILED`.

## Typed local streams

For a fixed message:

```rust
let descriptor = Stream::<Frame>::descriptor("rbt/example/frame", StreamProfile::Sensor);
let publisher = Stream::<Frame>::publisher(&descriptor)?;
let mut sample = publisher.loan()?;
sample.sequence = 1;
sample.value = 42;
sample.send()?;
```

For a header-plus-slice message:

```rust
let descriptor = SliceStream::<Image>::descriptor("rbt/example/image", StreamProfile::Sensor);
let publisher = SliceStream::<Image>::publisher(&descriptor, 1920 * 1080 * 3)?;
let mut sample = publisher.loan_slice(3)?;
sample.header_mut().encoding = PixelFormat::RGB8;
sample.payload_mut().copy_from_slice(&[0, 1, 2]);
sample.send()?;
```

The producer uses `open_or_create`; a consumer uses `open`. Both validate the
complete descriptor before transport open. `Sensor` uses queue depth 1 and
safe overflow; `Control` uses queue depth 4 and reports `QueueFull` rather than
silently overwriting an unread command. Both profiles use history 0, maximum
Borrow 1, and maximum concurrent Loans 2. `StreamResourceConfig` exposes only
`max_loans` and `max_borrowed`; the `*_with_resources` constructors support
explicit endpoint sizing and reject zero or incompatible values. Queue depth,
history, and safe-overflow remain profile semantics.

Every fixed and slice publisher/subscriber exposes `pressure()`. Publisher
snapshots include exact loan attempts/successes/current/high-water/failures,
payload byte totals, send attempts/successes/failures, queue/delivery and
readiness failures, and aggregate connected subscribers. Subscriber snapshots
include receive/current-Borrow/high-water/failure counters, monotonic borrow
durations, and the oldest current borrow age. The wrapper cannot observe
per-subscriber queue occupancy or safe-overflow replacement counts, so those
values are not invented. `local_pressure_snapshots()` exists for low-rate
runtime diagnostics; application code normally inspects its endpoint directly.

`ReceivedSample` and `ReceivedSliceSample` are guards over borrowed shared
memory. Read/copy data while the guard is alive; do not retain references after
it is dropped. A second simultaneous receive on the same subscriber returns
`BorrowLimitReached`; holding two unsent publisher Loans makes the next attempt
return `LoanPoolExhausted`. Dropping either guard releases its resource. No rbt
serialization or intermediate payload buffer is used.

## Version and generated types

Generated message modules are included by `rbt-types` and re-exported through
`rbt::types`. User schemas should be compiled with `rbt-idl`; do not hand-edit
generated files. See [`idl.md`](idl.md) for the command, supported shapes, and
layout hash policy.

The SDK package version `0.8.0` is separate from the native iceoryx2 version
`0.10.0-rbt.2` and from its compatibility axes: IDL language version 1,
message descriptor version 1, Composer control protocol version 2, and
component snapshot version 4. A layout-affecting message change must update
the ABI major/layout hash, and control-protocol or snapshot mismatches must be
rejected. See [`compatibility.md`](compatibility.md).

## Install from a local prefix

`scripts/install_v01.sh --prefix <path>` stages the current facade and its internal
path dependencies under `<path>/share/rbt/rust`. A local consumer can depend
on `<path>/share/rbt/rust/crates/rbt`; it does not need the source checkout.
`scripts/test_install.sh` creates and runs an installed generated project and
such a fresh consumer outside the repository, verifies bounded launcher
cleanup, and also tests the C++ package and packaged documentation/licenses.

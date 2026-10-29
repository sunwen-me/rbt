# Component authoring

The shortest path to a normal component is:

```bash
rbt new component detector
```

The generated Rust file is deliberately small but shows the four important
ideas: preparation, requirements, activation, and cancellation-aware work.
This page uses Rust examples. The same process and activation ownership model
is available in C++ and Python; see the [Rust SDK](rust-sdk.md),
[C++ SDK](cpp-sdk.md), and [Python component guide](python-component.md).
Start from the [documentation index](README.md) or the
[developer guide](development.md) for other entry points.

## The lifecycle

1. `prepare` runs once for the process. Load configuration and expensive,
   process-lifetime resources here.
2. `id` returns the stable logical `ComponentId`.
3. `requirements` declares named capability roles. A requirement is a stable
   contract, not a physical iceoryx2 service name.
4. `activate` resolves bindings, opens streams, provides capabilities, and
   starts activation-owned tasks.

Composer may activate the same prepared process more than once as providers
   appear, disappear, or are replaced.  Activation owns its bindings,
   endpoints, capabilities, and tasks.  Deactivation cancels and joins that
   scope before the next activation is accepted.

## Consumer and provider shape

Declare the semantic role the component needs:

```rust
fn requirements(&self) -> Vec<RequirementSpec> {
    vec![RequirementSpec::typed::<Camera>("front")]
}
```

Resolve that role during activation, rather than constructing a service name:

```rust
let binding = ctx.binding::<Camera>("front")?;
let subscriber = Stream::<Frame>::subscriber(binding.stream())?;
```

A provider creates an activation-owned capability and publishes its complete
descriptor through `ctx.provide`:

```rust
let capability = CapabilityInstanceId::new();
let descriptor =
    Stream::<Frame>::descriptor(capability.stream_service_name(), StreamProfile::Sensor);
let publisher = Stream::<Frame>::publisher(&descriptor)?;
ctx.provide::<Camera>(capability, descriptor);
```

The capability is visible to Composer only after `activate` returns `Ok(())`.
If activation fails, its partially created resources are cleaned up and no
half-published endpoint is treated as a provider.

For request/response work, declare a typed capability role and use the
activation-scoped RPC helpers:

```rust
let client = ctx.require_rpc::<EchoRpc, Frame, Detection>("echo")?;

let server = ctx.rpc_server::<EchoRpc, Frame, Detection>(
    CapabilityInstanceId::new(),
)?;
```

Requests and responses remain native local payloads. RPC timeout and
cancellation rules, resource limits, and Python/C++ usage are covered in
[`rpc.md`](rpc.md).

## Time and transform data

Use `rbt::time::HostClock` for local monotonic timestamps and the existing
`Timestamp`, `DeviceClock`, and `ClockMapping` types for identified sensor
time. Sensor timestamps describe data; lifecycle deadlines continue to use
real monotonic clocks. See [`time.md`](time.md) for timestamp and mapping
contracts.

For coordinate transforms, use a bounded `TransformBuffer` in each consumer.
The existing `TransformActivationExt` helpers bind dynamic transform Streams
and static update Stream/snapshot RPC pairs to that consumer's activation.
Lookups read the local buffer. See [`tf.md`](tf.md) for Rust, C++, and Python
APIs and the actual transport behavior.

## Tasks and cancellation

Activation-owned work belongs in `spawn_critical` or `spawn_background`:

```rust
ctx.spawn_critical("receive", move |token| async move {
    loop {
        tokio::select! {
            _ = token.cancelled() => return Ok(()),
            result = receive_one_frame() => {
                result?;
            }
        }
    }
});
```

Use a critical task when its failure means that the activation cannot continue
(for example, a required input loop or publisher).  Its error or panic
invalidates the activation and downstream consumers observe the resulting
failure/blocking state.  Use a background task for auxiliary metrics or
optional housekeeping; a background failure is reported without changing the
activation's validity.

Do not hide activation work in raw `tokio::spawn`, detached threads, or a
process-global task.  Every task must have a cancellation path and a bounded
join through the activation's `EffectScope`.  Cancellation during ordinary
teardown is not an unexpected task failure.

## A minimal publisher loop

The generated `hello-source` is a complete runnable reference.  Its critical
task follows this pattern:

```rust
ctx.spawn_critical("publish", move |token| async move {
    let mut sequence = 1_u64;
    let mut ticker = tokio::time::interval(Duration::from_millis(100));
    loop {
        tokio::select! {
            _ = token.cancelled() => return Ok(()),
            _ = ticker.tick() => {
                let mut sample = publisher.loan()?;
                sample.sequence = sequence;
                sample.value = sequence;
                sample.send()?;
                sequence = sequence.saturating_add(1);
            }
        }
    }
});
```

`StreamProfile::Sensor` and `StreamProfile::Control` retain their existing
queue, overflow, and ABI semantics.  Do not retry `QueueFull` blindly, raise
borrow limits to hide retained samples, or put Composer in the payload loop.
See [Stream pressure](pressure.md) for bounded behavior and diagnostics.

## Add a schema first

Message layout belongs in IDL, not in a handwritten generated type:

```bash
rbt new message Detection
rbt idl fmt
rbt idl check
rbt idl generate
rbt check
```

The formatter is deterministic and `rbt idl fmt --check` is suitable for CI.
Source locations in parser/semantic errors identify the file, line, column,
source line, and a caret.  A changed `.rbt` file is not complete until the
generation manifest and all configured outputs are refreshed.

## Verify before running

From any project subdirectory:

```bash
rbt check
cargo build --workspace
rbt run
```

`rbt check` does not start Composer.  It catches malformed `rbt.toml`, duplicate
component IDs, the package/command choice, missing executables, IDL errors,
output-stem collisions, stale hashes, and missing generated files.  `rbt run`
then owns the local Composer and component process tree and handles Ctrl-C;
the launcher is only a convenience and is not part of runtime lifecycle truth.

## Provider replacement is normal

When a provider disappears, a consumer can become `BLOCKED` without its
prepared process being recreated.  Use:

```bash
rbt ps
rbt why detector
rbt graph
```

`why` reports the unresolved named requirement and retains last-active binding
context where the snapshot has it.  Starting a compatible provider allows
Composer to activate the existing consumer again.  The process, preparation,
and data-plane semantics remain the same; only the activation-owned binding and
capability identities change.


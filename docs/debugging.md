# Debugging runtime state

This guide covers the v0.8 runtime. Use the [CLI reference](cli.md) for command
options and the [documentation index](README.md) for feature and SDK guides.

Begin with:

```bash
rbt ps --full
rbt graph
rbt why <component>
rbt capability list
```

Use `RUST_LOG=debug` or `RBT_TEST_TRACE_FRAMES=1` only for bounded tracing;
normal examples log lifecycle/binding changes at info and avoid per-frame info
logs.

For an environment or project problem, begin outside the graph:

```bash
rbt doctor
rbt check
```

Then use the read-only runtime projection. `ps` and `graph` describe current
Composer truth; `why` explains a single component and retains last-active
binding context where the snapshot provides it. None of these commands scans
`/dev/shm` or infers a graph from log text.

## Component is `BLOCKED`

`BLOCKED` means the process is connected but at least one declared requirement
cannot currently resolve to exactly one active capability. `rbt why` lists each
missing requirement and the last active binding. Check that the provider is
`ACTIVE`, has no process conflict, advertises the exact capability type, and
publishes a complete Stream or RPC descriptor. Starting a replacement provider should
cause a new activation rather than a component process restart.

## Component is `FAILED`

`FAILED` is an activation/component failure rather than ordinary dependency
absence. `rbt why` prints the cause. A critical task error, exception/panic, or
unexpected exit invalidates the activation, joins owned tasks, runs cleanup,
withdraws capabilities, and blocks downstream dependents. A background task
failure is observable but does not invalidate the activation. Restart the
failed process after addressing the cause.

## `AbiMismatch`

Producer and consumer disagree on message namespace/name, ABI major, shape,
size/alignment, field signature, element layout, or layout hash. rbt rejects
this before transport open; warning and continuing would risk interpreting
shared memory with the wrong layout. Inspect the stream, regenerate from the
same IDL, and compare checked-in descriptors. Do not edit a generated hash.

## `QueueFull`

The Control profile has depth 4 with safe overflow disabled, so saturation is
reported as `QueueFull`. The producer must retry, reject, or surface the
condition according to application policy. Sensor streams have depth 1 and
safe overflow enabled, so slow consumers observe sequence gaps rather than a
control-style queue error.

## `BorrowLimitReached` or `LoanPoolExhausted`

These are bounded resource outcomes, not generic transport failures. A held
receive guard consumes one borrow until it is dropped; an unsent publisher loan
consumes one loan slot until it is sent or released. Inspect the live owner:

```bash
rbt stream pressure <capability-instance-id>
rbt stream pressure <capability-instance-id> --json
```

Look for current/high-water loans or borrows and the oldest borrow age. Release
the retained sample or unsent loan before changing resource configuration. Do
not automatically increase `max_borrowed`, retry indefinitely, or claim that
an unavailable generic Sensor drop count is zero.

## Pressure metrics are stale or unavailable

Pressure is low-rate telemetry attached to one provider process and
`CapabilityInstanceId`. A missing or old snapshot is reported as unavailable
or stale; it is not converted to a zero. Confirm the provider is still
`ACTIVE`, inspect `rbt ps`, and query again. A provider replacement creates a
new metric owner, so compare capability instance identities before comparing
counters.

## Composer is unavailable

The control/inspection connection can fail while an already-active data path
continues. Run:

```bash
rbt doctor
RBT_COMPOSER_ADDR=127.0.0.1:7447 rbt ps
```

If Composer was intentionally stopped, restart it or use `rbt run` to own a
fresh local session. A control connection error is not evidence that payload
delivery stopped. Protocol and snapshot version mismatches fail closed and are
reported as compatibility diagnostics.

## Duplicate `ComponentId` quarantine

Two live process UUIDs claiming one logical component id are ambiguous.
Composer quarantines the contender and reports a process conflict; it does not
silently select a winner or merge snapshots. Stop the unintended process, then
inspect again. The surviving validated snapshot becomes authoritative after
the conflict clears.

## Composer disconnected or recovering

`disconnected` means the low-rate TCP control link is down. A valid active
component keeps its iceoryx2 endpoints and tasks; payloads continue. On
reconnect it sends `HELLO` plus a complete snapshot. A new Composer begins in
`recovering`, imports snapshots during a two-second barrier, rebuilds topology,
then reconciles reality. A healthy graph should log:

```text
reconciliation: 0 deactivate, 0 activate
```

If a provider died while Composer was down, that process cannot resubmit a
snapshot; the now-stale dependency is removed during reconciliation.

## Stale event or command ignored

Revisions are ordered only within a Composer epoch. The runtime ignores an old
revision, a deactivation for another process/activation, and a task event from
an earlier activation. This prevents delayed cleanup or control traffic from
destroying replacement resources. Correlate logs using component, process,
activation, capability instance, Composer epoch, and revision.

## Reproduce fault paths

The Rust and C++ detector examples understand these test-only variables:

| Variable | Meaning |
| --- | --- |
| `RBT_TEST_FAIL_AFTER=N` | critical task returns an error after N frames |
| `RBT_TEST_EXIT_AFTER=N` | process exits nonzero after N frames |
| `RBT_TEST_PAUSE_MS=N` | delay processing for each frame |
| `RBT_TEST_TRACE_FRAMES=1` | C++ per-frame trace for a bounded test |

Use `scripts/test_mixed_runtime.sh` for a deterministic provider replacement,
Composer restart, and critical failure. Use `scripts/soak_v01.sh --smoke` for a
short multi-role fault cycle; retained logs are printed on completion/failure.

## MCAP playback rejection

Inspect a recording without starting a graph:

```bash
rbt mcap info recording.mcap
rbt mcap info recording.mcap --json
```

For a corrupt container, unsupported encoding, malformed payload, or ABI/schema
mismatch, `rbt play` reports the file and channel/schema when known, the typed
reason, and the suggested action. It does not create a publisher before the
complete preflight succeeds. `--skip-unsupported` skips only unrelated
external channels; malformed rbt channels still fail.

## RPC failure or timeout

Inspect the capability's request/response types, physical endpoint, and
resource limits:

```bash
rbt rpc info <capability-id-or-service> --verbose --json
```

Compare the current activation and capability instance before diagnosing a
replacement. A deadline or cancellation does not permit replaying a
non-idempotent operation automatically. Check request/response guard lifetimes
and bounded in-flight limits using the [RPC guide](rpc.md).

## Time or transform unavailable

Inspect host clocks locally and the current TF diagnostic view:

```bash
rbt time status --json
rbt tf status --json
rbt tf tree --json
```

`time status` reads local clock diagnostics. Composer's TF view is partial;
`tf status` and `tf tree` can instead read a supplied bounded consumer snapshot
with `--snapshot PATH`. They do not perform a remote lookup or discover every
process-local buffer. See [TF diagnostics](tf.md) for the snapshot contract.

For a failed lookup, check target/source direction, physical authority, static
snapshot freshness, timestamp clock identity/generation, valid mappings,
dynamic history bounds, and continuity epoch. Preserve the reported error:
missing history cannot become an implicit latest lookup, and unknown timing
precision cannot become zero uncertainty. See [Time](time.md) and [TF](tf.md).

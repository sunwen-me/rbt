# v0.8 spatial transforms: architecture and repository audit

Status: v0.8 software implementation and acceptance are complete. This document
records the architecture and repository audit; verification evidence belongs
in `tf-report.md`. Physical-camera hardware acceptance remains NOT RUN.
The current SDK is 0.8.0; use the [documentation index](README.md) and
[framework development guide](development.md) for current build and API entrypoints.

## Baseline observed before implementation

The checkout has no commits and all project files are untracked. A SHA-256
manifest and copies of 326 pre-existing files were saved under
`target/tf-baseline/` before implementing TF. Reviews must compare against that
archive rather than assume `git diff` contains the existing project.

At the pre-TF audit, `Cargo.toml`, CMake, and Python reported product/SDK 0.7.0. The maintained native
source is `third_party/iceoryx2-rbt/source`, iceoryx2 0.10.0-rbt.2. Its dependency
pin is 0.10.0; the maintenance suffix and fingerprint are tracked separately.
No native source changes were required for TF. IDL language 1, descriptor 1, Composer
protocol 2, snapshot 4, and canonical MCAP encoding `rbt-canonical-v1` remain
frozen. The original 49 production unsafe locations are preserved; the user
approved exactly 24 additional generated TF safety declarations, for a reported
total of 73, with no new hand-written unsafe. See `tf-unsafe-review.md`.
At that audit, documentation still contained v0.6 labels and a six-test CTest
count; CMake registered eight tests before TF. The current suite has 12,
including four TF tests. The v0.8 documentation update corrects the maintained
guides while preserving historical reports. These documentation corrections
do not change protocol versions. There is no `crates/rbt-runtime`; lifecycle lives in
`crates/rbt-compose` and graph types in `crates/rbt-core`.

## Source evidence and integration points

* `crates/rbt-time/src/lib.rs`: `Timestamp` is an identified signed i64
  nanosecond reading. `ClockIdentity` includes kind, 48-byte source ID and
  generation. Checked arithmetic never compares unrelated numeric readings.
  `ClockMapping::convert(input, uncertainty, target, now)` validates source and
  target identities, calibration evidence, source interval, monotonic expiry,
  invalidation and uncertainty. It uses checked rational integer arithmetic.
  Unknown precision cannot be converted as if it were zero. TF reuses these
  objects and errors, including generation/expiry errors; it defines no clock.
* `crates/rbt-time-stream/src/lib.rs`: `StampedBytes@1` has a u64 header and a
  u8 slice containing a 192-byte `RBTTIME1` prefix followed by application
  bytes. Its hash is `97e9a924992dedfe83196197cccb6b35`. A received guard owns the
  native sample; the decoded timing value is copied. TF will not alter this ABI.
* `crates/rbt-idl/src/lib.rs` and `docs/idl.md`: fixed records, fixed arrays,
  fixed messages and header/element slice messages already support the needed
  shapes. Nested dynamic fields and optionals are unsupported. Explicit flags
  encode unknown timing precision. Generated bindings provide layout metadata,
  Rust SHM safety markers, C++ typed transport bindings and Python descriptors.
* `crates/rbt-iox2/src/lib.rs`, `rpc.rs`, `wait.rs`: fixed and slice Streams,
  `FixedSliceRpc`, native loans/guards, request/response event sources, and
  bounded WaitSet drains already exist. Sensor/Control queues remain 1/4;
  `StreamResourceConfig` bounds loans and borrows, not queue depth. TF does not
  assume reliable delivery of every revision. The initial WaitSet drain closes
  the listener-attachment readiness window; notifications are advisory.
* `crates/rbt-core/src/lib.rs`: `CapabilityInstanceId` supplies fresh physical
  endpoint names. `CapabilityDescriptor` also identifies provider component,
  process and activation. `FrameId` is a logical coordinate name and never an
  endpoint identifier. The registry permits distinct roles from one provider,
  but only one endpoint per role/provider. Static snapshot and update therefore
  use distinct roles. Pairing requires the same provider/process/activation.
* `crates/rbt-compose/src/component.rs`, `effect_scope.rs`, `reconciler.rs`:
  advertisements are staged until successful activation, tasks and resources
  belong to `EffectScope`, and binding replacement invokes the existing
  lifecycle. Fixed/slice RPC has lower-level activation-aware constructors;
  the existing convenience RPC helper covers fixed/fixed only. TF adapters use
  the existing lower-level constructor rather than changing runtime semantics.
  A healthy control connection loss retains active effects; reconnect sends a
  complete snapshot. Composer recovery has a two-second barrier. TF restart
  testing must exercise this actual behavior, not only an in-memory mock.
* `sdk/cpp/include/rbt/{stream,rpc,time,effect_scope}.hpp` and CMake provide
  native C++20 RAII transport and installed headers. C++ TF uses that native
  path, not Rust payload forwarding. The baseline managed C++ capability DTOs
  modelled streams only. v0.8 adds the existing protocol-2 RPC descriptor variant
  to those DTOs, with exactly one transport descriptor per capability, and TF
  helpers validate the static RPC/update association explicitly.
* `python/rbt/{time,time_stream}.py`, `crates/rbt-python/src/{lib,rpc}.rs`:
  Python has validated v0.7 time objects and bounded native stream/RPC wrappers.
  RPC routing is a closed list, so descriptor generation alone is insufficient.
  Python TF provides typed adapters, preserves native guard release, and uses the
  existing single-loop bridge when attached to asyncio.
* Existing stream/RPC weak pressure registries are sampled about once a second
  by component snapshots. Composer joins by physical identity; pressure-only
  changes do not reconcile the graph. TF lookup counters remain local and are
  exposed as bounded, explicit observations. CLI reports partial visibility and
  missing measurements honestly. No per-lookup control-plane event is added.

## Mathematical and frame contract

All coordinates are right handed, metres and radians; internal arithmetic is
f64/double. `T_target<-source` maps `p_target = R*p_source + t`. A tree edge
parent/child stores `T_parent<-child`. `T_A<-C = T_A<-B * T_B<-C`; inverse
reverses direction. Points include translation, vectors do not, and poses
transform both position and orientation. Quaternion component order is xyzw
in all APIs, configuration and generated records. Constructors reject nonfinite
values, zero norm and norms outside 1e-6 of one. Only accepted roundoff is
normalized. Interpolation is linear translation plus shortest-path SLERP, with
a stable near-identical branch and explicit interval bounds.

`FrameId` is case sensitive, 1–63 ASCII bytes in a fixed 64-byte record with
canonical zero padding. Slash-separated segments start with an ASCII letter,
digit or underscore and continue with those characters, dot or hyphen. Empty,
`.`/`..`, leading/trailing slash, NUL and non-ASCII names are invalid. Equality
compares bytes, not a process hash. There is no global intern registry.

## Data and ownership paths

```
static provider -> native StaticSnapshot RPC + StaticUpdate Stream
dynamic provider -> native DynamicTransform Stream
                            |
                      consumer ingest
                      validate and copy
                      promptly drop guard
                            |
                  bounded local TransformBuffer
                            |
           lookup_at -> local graph/history/math -> result
```

Canonical `types/tf.rbt` defines full fixed records and native slice messages.
The audit uncovered two existing codegen limits. Rust arrays longer than 32
need generated manual defaults and element-by-element canonical codec calls;
that generator extension preserves the public codec contract and all old
generated output bytes. C++ slice bindings use the header/element type pair as
their identity, so `StaticUpdateHeader` repeats `StaticHeader` fields to give
updates a distinct generated binding. Both headers are 112 bytes, alignment 8.
`TfTimestamp.flags` bit 0 means known uncertainty, including known zero; all
other bits and reserved bytes must be zero. Unknown uncertainty requires a zero
storage value. Status 0 means success with exact count, 1 means snapshot too
large, and 2 means provider unavailable; errors carry no partial elements.
No JSON/protobuf TF payload transport is introduced. Each dynamic sample carries
parent, child, complete identified timestamp/uncertainty, transform, continuity
epoch and the expected update capability identity atomically. Static snapshot
and update contain an authority, revision, typed status, exact element count,
effective-from timestamp and a complete bounded set. Success is never truncated;
oversize snapshots produce an explicit typed status with no partial graph.

Rust `rbt-tf` owns the safe math, validation and buffer. Transport adapters build
on `rbt-iox2`; lifecycle adapters build on existing composition. C++ implements
the same local semantics over generated types and native C++ endpoints. Python
provides natural validated objects and typed native adapters. No new executor,
central manager, lookup RPC, serializer or transport protocol is required.

## Authority, graph and replacement

A consumer explicitly binds bounded logical provider slots to physical
`CapabilityInstanceId`s. Rebinding atomically removes old owned edges/history
and resets that slot's observations. Every ingest token and wire authority must
match the current binding. Late old samples fail even if their numeric times or
revisions are greater. Static revisions compare only within one physical
authority. Snapshot/update endpoints must be associated by the same component,
process and activation, with the update identity as the graph authority.

Graph transactions validate self edges, duplicate parents and cycles before
publishing a complete local view. Competing physical claims are retained only
within configured bounds as conflicts, and affected paths fail closed until a
claim is removed or the binding changes. Arrival order never elects authority.
There is one synchronization boundary around a bounded mutation/query; no user
callback runs under it and no sample guard is retained in history.

## Static bootstrap and freshness

Open the update subscriber/readiness listener before issuing the snapshot RPC.
Stage a bounded sequence of observed full updates. Merge the snapshot and
consecutive newer revisions under one buffer lock, then expose the complete
view. An older returning snapshot cannot overwrite a newer accepted revision.
A revision gap or pending-update overflow makes the provider stale and requests
a bounded snapshot resync. One pending RPC and a steady-time minimum interval
bound retries. Periodic snapshot verification catches a lost final notification;
the maximum verified age fails closed if resync stops succeeding.

Static replacement/revision invalidates the previous set atomically. The new
set's explicit effective-from timestamp bounds its applicability. Queries before
that bound are refused; no unbounded calibration history is retained or silently
applied to historical samples. Effective-time comparisons also require an
identified compatible clock or explicit valid mapping.

## Dynamic history and time path

Each edge has bounded chronological history, a current monotonically increasing
continuity epoch, and full clock identity. The buffer bounds providers, edges,
samples per edge, total samples, duration and pending static work. Eviction is
deterministic and counted. Epoch or clock-generation advancement isolates old
history; delayed older epochs/generations are rejected. No interpolation spans a
reset. Out-of-order or contradictory duplicate samples are explicit errors.

`lookup_at` is immediate and local. It requires exact samples or valid bracketing
samples within the interpolation-gap limit. Before/after history produces past/
future extrapolation errors. It never substitutes latest. Query and sample
uncertainty are preserved and checked against an explicit policy; default strict
queries reject unknown precision. Best effort is an explicit option and returns
unknown uncertainty. The reported bound is the maximum absolute timestamp
uncertainty among the query and used samples; it is not a relative alignment
bound or a spatial-error estimate. Cross-domain conversion maps the query into
each edge's history/effective-from clock and always calls v0.7 `ClockMapping`
with caller-supplied calibration and host-monotonic `now`; no numeric comparison
or fabricated mapping is allowed. Temporal uncertainty is not spatial accuracy.

## Validation boundaries and exclusions

Verification must include real native three-language pairs, installed packages,
snapshot races/gaps, 100 physical replacements, concurrency, ten healthy
Composer restarts, fault recovery, sanitizers, inherited gates, a measured 300 s
mixed software soak and correct-result lookup percentiles. Synthetic fixtures
are labelled software tests. Hardware mode requires an explicitly selected
device and measured/configured extrinsics; calibration accuracy remains
unverified unless supported by evidence. No hardware run is authorized merely
because `/dev/video0` exists or a previous milestone used it.

Simulation time, networking, central TF queries, global databases/consensus,
parameters, calibration/kinematics solvers, URDF and ROS compatibility remain
outside this milestone. Existing message hashes, MCAP encoding, RPC/WaitSet
semantics, Stream profiles and native maintenance source remain unchanged.

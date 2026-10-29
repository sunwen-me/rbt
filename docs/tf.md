# Transform frames and lookups (v0.8 implementation)

See the [documentation index](README.md) for the full SDK and development
guides, including the underlying [Time](time.md), [RPC](rpc.md), and
[component lifecycle](component-authoring.md) contracts.

This guide describes the accepted v0.8 TF APIs; product and packages report
version 0.8.0. Software verification evidence is recorded in the
[TF acceptance report](tf-report.md). Physical-camera hardware acceptance
remains NOT RUN. This is user documentation, not an acceptance report; see
[`tf-architecture.md`](tf-architecture.md) for the architecture and the report
for release verification evidence.

## Run the local example

The Rust example loads only the file passed to `--config`, validates a complete
static tree, and performs local forward and inverse point lookups. It opens no
camera and publishes no transport endpoint:

```sh
cargo run -p rbt-tf-local -- --config docs/tf-config-example.toml
```

The TOML values in [`tf-config-example.toml`](tf-config-example.toml) are an
illustrative software fixture. They are not measured extrinsics, calibration
output, or a claim about any hardware. The example prints “Configured extrinsic
— calibration accuracy unverified”. It explicitly permits unknown *time*
precision and keeps the result's uncertainty as `None`; that says nothing about
spatial calibration accuracy. The configuration becomes effective at the
identified host-monotonic timestamp read by the example, so earlier lookups
are rejected.

The parser accepts the `[[transforms.static]]` table with `parent`, `child`,
`translation_m`, and `rotation_xyzw`. It rejects unknown fields, invalid frames
or rotations, duplicate children, cycles, files over 1 MiB, and transform counts
over the caller's limit. There is no default config path. See the
[example README](../examples/advanced/tf-local/README.md) for the full behavior.

## Frames and transform direction

`FrameId` is a case-sensitive logical coordinate name, separate from a
capability UUID or transport service name. A name is 1–63 ASCII bytes in a
canonical 64-byte zero-padded representation. Names use slash-separated
segments. Each segment starts with an ASCII letter, digit, or underscore;
subsequent characters may also include `.` and `-`. Empty segments, `.` and
`..`, leading or trailing slashes, NUL, and non-ASCII bytes are rejected. For
example, `robot/base_link` is valid; `/base`, `base/`, `base//camera`, and
`base/../camera` are not.

Coordinates are right handed, in metres and radians. A transform is written
`T_target<-source` and maps a point as
`p_target = R * p_source + t`. A tree edge with `parent = base_link` and
`child = camera_optical` stores `T_base_link<-camera_optical`. Thus
`T_A<-C = T_A<-B * T_B<-C`; inversion reverses the direction. Applying a
transform to a point rotates and translates it; applying it to a vector only
rotates it; applying it to a pose transforms its position and orientation.

Quaternions are always in **x, y, z, w** order in the APIs, configuration, and
wire records. Inputs must be finite and have nonzero norm within `1e-6` of one;
accepted roundoff is normalized. Arbitrary non-unit, zero, NaN, and infinite
quaternions are rejected. Transform interpolation linearly interpolates
translation and uses shortest-path SLERP for rotation, including the equivalent
`q`/`-q` representation. Its interpolation fraction must be finite and within
`[0, 1]`. Math that overflows to a non-finite value is rejected.

The local value types are distinct from generated wire types. In C++, use
`<rbt/tf_math.hpp>` for math and `<rbt/transforms.hpp>` for the local buffer.
Generated wire records such as `TfVec3` and `TfTransform3` are in
`<rbt/tf.hpp>`; do not use that header in place of the local buffer header.
Rust exposes local types from `rbt_tf`, and Python exposes them from
`rbt.tf_math` and `rbt.tf`. All three reuse the v0.7 `Timestamp`, clock identity,
and `ClockMapping` APIs; TF defines no replacement clock model.

## A common robot frame tree

One common arrangement is:

```text
map
 └── odom
      └── base_link
           ├── camera_link
           │    └── camera_optical
           ├── lidar_link
           └── imu_link
```

The localization authority publishes `T_map<-odom`; the odometry authority
publishes `T_odom<-base_link`. These are dynamic edges with measurement times
and continuity epochs. The camera, lidar and IMU mounting edges are static
calibration relationships. `T_camera_link<-camera_optical` is an explicitly
configured static optical-frame convention.

This is a conceptual example. Each robot must supply its actual frames,
authorities, measured extrinsics and valid time mappings. Frame names do not
create edges. The local configuration example does not supply localization or
odometry; a lookup requiring either absent edge fails explicitly.

## A bounded local buffer

The core API is the same in each language: bind a logical slot to a fresh
physical provider identity, ingest complete dynamic records or static sets,
then call `lookup_at` for an explicit timestamp. A local buffer does not
advertise a capability merely because it binds a provider.

Here is a self-contained Rust example using one static edge and a real
identified host-monotonic timestamp. This is a software math example, not a
calibration:

```rust
use rbt_core::CapabilityInstanceId;
use rbt_tf::{
    FrameId, LookupOptions, LookupPolicy, StaticEdge, StaticSet, TfResourceConfig,
    Transform3, TransformBuffer, Vec3, Quaternion,
};
use rbt_time::HostClock;

fn main() -> Result<(), Box<dyn std::error::Error>> {
    let stamp = HostClock::new()?.monotonic_now()?;
    let mut policy = LookupPolicy::default();
    // Best effort is explicit; unknown timing precision remains None.
    policy.allow_unknown_uncertainty = true;
    let buffer = TransformBuffer::new(TfResourceConfig::default(), policy)?;
    let authority = buffer.bind_provider("example_extrinsics", CapabilityInstanceId::new())?;

    buffer.apply_static_snapshot(
        &authority,
        StaticSet {
            revision: 1,
            effective_from: stamp,
            effective_uncertainty_ns: None,
            transforms: vec![StaticEdge {
                parent: FrameId::new("base_link")?,
                child: FrameId::new("camera_optical")?,
                transform: Transform3::new(Vec3::new(0.0, 0.0, 0.0)?, Quaternion::identity()),
            }],
        },
    )?;

    let result = buffer.lookup_at_with(
        "base_link",
        "camera_optical",
        stamp,
        LookupOptions { timestamp_uncertainty_ns: None, mappings: &[], now: None },
    )?;
    let point_in_base = result.apply_point(Vec3::new(0.2, 0.0, 1.0)?)?;
    println!("point in base_link: {:?}; timing uncertainty: {:?}",
             point_in_base.xyz(), result.timing_uncertainty_ns);
    Ok(())
}
```

The C++ local API uses `rbt::tf::TransformBuffer`; the following follows the
same direction and makes best-effort unknown precision explicit:

```cpp
#include <optional>

#include <rbt/time.hpp>
#include <rbt/transforms.hpp>

int main() {
    auto host = rbt::time::HostClock::create();
    if (!host) return 1;
    auto stamp_result = host.value().now_monotonic();
    if (!stamp_result) return 1;
    const auto stamp = stamp_result.value();

    rbt::tf::LookupPolicy policy;
    policy.allow_unknown_uncertainty = true;
    rbt::tf::TransformBuffer buffer({}, policy);
    const auto authority = buffer.bind_provider(
        "example_extrinsics", "00000000-0000-0000-0000-000000000001");
    buffer.apply_static_snapshot(authority, rbt::tf::StaticSet {
        1U,
        stamp,
        std::nullopt,
        { rbt::tf::StaticEdge {
            rbt::tf::FrameId("base_link"),
            rbt::tf::FrameId("camera_optical"),
            rbt::tf::Transform3(rbt::tf::Vec3(0.0, 0.0, 0.0),
                                rbt::tf::Quaternion::identity()),
        } },
    });
    const auto result = buffer.lookup_at("base_link", "camera_optical", stamp);
    const auto point = result.apply_point(rbt::tf::Vec3(0.2, 0.0, 1.0));
    return point.x() >= 0.0 ? 0 : 1;
}
```

Python exposes the same local buffer model with immutable value objects:

```python
from uuid import UUID

from rbt.tf import LookupPolicy, StaticEdge, StaticSet, TransformBuffer
from rbt.tf_math import Transform3, Vec3
from rbt.time import HostClock

stamp = HostClock().monotonic_now()
buffer = TransformBuffer(policy=LookupPolicy(allow_unknown_uncertainty=True))
authority = buffer.bind_provider(
    "example_extrinsics", UUID("00000000-0000-0000-0000-000000000001")
)
buffer.apply_static_snapshot(
    authority,
    StaticSet(
        1,
        stamp,
        None,
        (StaticEdge("base_link", "camera_optical", Transform3.identity()),),
    ),
)
result = buffer.lookup_at("base_link", "camera_optical", stamp)
point = result.transform.apply_point(Vec3(0.2, 0.0, 1.0))
print(point, result.timing_uncertainty_ns)  # uncertainty remains None
buffer.unbind_provider(authority)
```

In production, use the time uncertainty actually known from the timestamp
source. A known zero is valid only when zero is known. The default policy is
strict: it rejects unknown query or sample precision. Best effort requires
`allow_unknown_uncertainty = true` and preserves unknown precision as `None`;
it does not turn unknown into zero. A known uncertainty above
`max_timestamp_uncertainty_ns` is rejected. The default maximum is 1,000,000
ns, and the default maximum interpolation gap is one second.

`LookupResult` contains the composed transform, the original requested
timestamp, resulting timing uncertainty, and per-edge provenance. Rust's result
offers `apply_point`, `apply_vector`, and `apply_pose`; C++ has the same methods;
Python exposes the transform for those operations. Use the returned source list
to inspect authority, parent/child, timestamp bracket, static revision, and
dynamic epoch.

## Time, interpolation, and reset rules

Every sample and query uses the v0.7 identified `Timestamp`. A timestamp is not
just a nanosecond number: its clock kind, 48-byte source identity, and generation
must match before readings can be compared. If the query clock is already the
edge-history clock, the buffer resolves it directly. Otherwise, provide a
valid direct `ClockMapping` from the query clock to the clock of each edge used
by the lookup, along with a current timestamp in the mapping's `now` clock.
The buffer converts the **query once per edge into the history/effective-time
clock** using the existing `ClockMapping::convert` checks. It does not convert
stored history to the query clock, invert mappings, or chain mappings. For a
multi-clock path, supply direct mappings from the query clock to each distinct
edge clock. Missing, expired, invalidated, wrong-domain, wrong-generation,
out-of-range, and unknown-uncertainty conversions keep their typed time errors.

The output uncertainty is the maximum absolute timestamp uncertainty among
the query and sources used. It is not a spatial accuracy estimate or a bound on
relative alignment. Unknown input precision remains unknown under explicit
best effort.

`lookup_at` is an immediate local query. It accepts exact samples or a pair of
bracketing samples when their gap is within policy. Before/after available
dynamic history yields past/future extrapolation errors; an excessive bracket
gap is an interpolation error. There is no `latest` fallback. A caller that
wants a latest value must select a timestamp and policy outside this lookup
contract rather than silently receiving a different time.

Dynamic records carry a complete `T_parent<-child`, identified stamp,
uncertainty, and `transform_epoch`. Samples are chronological per edge; exact
duplicates are idempotent, contradictory duplicates and out-of-order samples
are errors. A higher epoch or a new compatible clock generation resets that
edge's history and establishes a boundary. Older epochs/generations are
rejected, and interpolation does not cross a reset. A history that exceeds its
steady-time age limit is unavailable even if it still has samples.

## Static sets, authorities, and graph behavior

Static data is published as a **complete set**, with an authority-local revision,
an identified `effective_from`, optional timing uncertainty, and every static
edge in that state. A replacement supersedes the previous set atomically; the
buffer does not retain an unbounded calibration history. Lookup before
`effective_from` fails, so a new calibration cannot silently be applied to an
older sample. Revisions advance one at a time on a compatible clock with a
monotonic effective time.

The buffer binds a logical provider slot to a non-nil physical
`CapabilityInstanceId` (a UUID in Python/C++ APIs). The slot is where an
application expects a provider; the physical ID identifies one activation's
actual endpoint. Rebinding a slot to a new physical ID removes the old
provider's graph and history. Old authority tokens and delayed samples are
rejected even if their timestamp or revision is numerically newer.

A slot describes one binding, rather than a reference-counted endpoint lease.
Binding the same slot and physical ID again is idempotent and returns the same
token. Unbinding that token removes the slot for every holder. Assign one
activation-owned ingestor to each slot; use buffer clones for concurrent
lookups and distinct slots for independent provider bindings.

The graph is a bounded tree: an edge child has one parent and an edge cannot
create a cycle or be a self edge. Competing parent or physical-authority claims
are recorded within bounds. Affected lookup paths fail closed with a conflict
error; arrival order does not select a winner. Remove the conflicting provider
or replace its binding to recover.

A static consumer establishes the update-stream listener before requesting a
snapshot. Updates received during snapshot bootstrap are staged within a bound;
the snapshot and consecutive newer updates are merged atomically. An older
snapshot cannot replace a newer revision. A revision gap, lost notification,
or pending-update overflow marks the provider stale and requires snapshot
resynchronization. Periodic verification is throttled by `resync_interval`; a
snapshot older than `snapshot_max_age` makes the provider stale. Defaults are
one second between resync attempts and five seconds maximum snapshot age.

For native transport, a static provider uses two different physical endpoints:
the full-set update stream is also the graph authority, while the complete
snapshot uses a distinct RPC capability. Those two capability descriptors must
belong to the same provider component, process, and activation. Dynamic streams
also carry and validate their expected physical authority.

## Resource limits and pressure

`TfResourceConfig` bounds state before it is accepted. Rust, C++, and Python
share these defaults:

| Limit | Default |
| --- | ---: |
| Provider slots | 16 |
| Total graph edges | 128 |
| Samples per dynamic edge | 256 |
| Total dynamic samples | 8,192 |
| Static transforms per provider | 64 |
| Pending static updates per provider | 8 |
| Dynamic history duration | 10 s |
| Static resync interval | 1 s |
| Maximum verified static snapshot age | 5 s |
| Maximum dynamic update age | 5 s |

Hard implementation caps are 256 providers, 4,096 edges, 1,000,000 dynamic
samples, and 4,096 pending static revisions. The combined pending full-set
storage is limited to 65,536 transform records. Configuration validates the
limits and the pending-set product before allocating provider state. Eviction
is deterministic and counted.

`pressure_snapshot()` exposes bounded buffer totals and counters, per-provider
snapshot/dynamic freshness and ages, revisions/epochs, evictions, conflicts,
lookup outcomes, resync attempts/failures, and the known graph. It is local
observation, not a request to a remote provider.

Counters describe their owning lifetime. Buffer lookup counters start after
frame-name validation; provider `buffer_observed_*` counters cover all buffer
queries since that binding was created, including queries on other providers.
Python argument conversion and validation run before entering the native buffer;
failures there, including rejection of an already-invalidated mapping, do not
increment buffer query counters.
Edge/frame gauges include retained conflicting claims; they do not certify that
every path can be queried. Eviction counts include history discarded at an
epoch boundary. Rebinding removes the old provider's observations.

The failure categories have separate counters as well as the original
aggregates. `missing_frame_count` counts `UnknownFrame` lookup errors;
`missing_transform_count` also includes disconnected paths, missing transforms
and insufficient history. `clock_mapping_failure_count` counts clock
domain/source/generation mismatches, unknown clock domains, unavailable,
expired or invalid mappings, and mappings invalidated by discontinuity.
`clock_error_count` includes every time error, including unknown uncertainty
and timestamp arithmetic errors. `authority_conflict_count` counts only
`ConflictingAuthority`; `conflict_count` also includes parent conflicts. Both
conflict counters include rejected claims and failed lookups at their existing
observation points, so they count events, not unique conflicted edges.
Per-provider category counters cover buffer-observed events since that binding,
with the same scope as the existing aggregate counters.

`resync_attempts` counts retry permits granted by `needs_resync`; manual snapshot
calls bypass that scheduler. Buffer `resync_failures` counts rejected static
continuity/consistency checks. Native snapshot RPC pressure separately records
completed, failed, cancelled and timed-out calls, current in-flight calls and
response guards. Stream pressure records loans, borrows and queue pressure.
Read these endpoint counters alongside buffer counters when diagnosing a failed
snapshot. Ages use local steady time since reception or successful snapshot
verification; they are not comparisons of unrelated measurement clocks.

The `rbt` CLI deliberately reports only what its selected source can know:

```sh
cargo run -p rbt-cli -- tf status --json
cargo run -p rbt-cli -- tf tree --json --snapshot /path/to/consumer-tf-snapshot.json
```

Without `--snapshot`, Composer inspection can show TF capability descriptors,
bindings, and provider identity, but Composer does not expose TF payload edges
or history. The report therefore says `graph_scope: partial`, reports graph
edges unavailable, and leaves local lookup metrics null. With `--snapshot`, the
CLI reads only that explicitly selected, bounded consumer-local observation
file; it is not payload transport and is never auto-discovered. Observation
freshness is judged only when its `observed_at` timestamp is compatible with
the CLI's current host-monotonic identity: at most 2.5 seconds is fresh,
older is stale, and missing/unrelated time is unavailable. Missing data is not
reported as a measured zero. Provider-level `static_stale`, `snapshot_fresh`,
and `dynamic_fresh` describe local buffer state, not end-to-end hardware or
calibration accuracy.
Older local observation files without the three separate category fields remain
readable; those absent counters are reported as unavailable rather than zero.

## Native transport and component lifecycle

Rust components use `rbt-tf-transport`'s `TransformActivationExt` on the
existing `ActivationCtx`. Capability role types implement the existing
`Capability` trait. A provider stages a dynamic stream or a paired static
update-stream/snapshot-RPC pair; a consumer resolves those roles, validates
the static pair, and passes clones of one `TransformBuffer` to the helpers.
The helpers own their tasks and teardown in the activation scope:

```rust
use rbt_iox2::StreamProfile;
use rbt_tf::TransformBuffer;
use rbt_tf_transport::TransformActivationExt;

// Inside a provider's activate(&mut self, ctx: &mut ActivationCtx<'_>):
let dynamic = ctx.provide_dynamic_tf::<DynamicRole>(StreamProfile::Control)?;
let static_tf = ctx.provide_static_tf::<UpdateRole, SnapshotRole>(
    initial_static_set, 64, StreamProfile::Control,
)?;
// Publish DynamicRecord values with dynamic.publisher. Replace the full static
// state through static_tf.control; the activation owns the snapshot server.

// Inside a consumer activation:
let buffer = TransformBuffer::new(resources, policy)?;
let static_binding = ctx.require_static_tf::<UpdateRole, SnapshotRole>(
    "tf_static_updates", "tf_static_snapshot", "camera_extrinsics", 64, buffer.clone(),
)?;
let dynamic_binding = ctx.require_dynamic_tf::<DynamicRole>(
    "tf_dynamic", "camera_motion", buffer.clone(),
)?;
// Keep `buffer` for lookup_at_with; the activation owns the ingestor tasks.
```

C++20 offers matching `provide_dynamic_tf`, `provide_static_tf`,
`require_dynamic_tf`, and `require_static_tf` helpers in
`<rbt/tf_component.hpp>`. They validate generated descriptors and physical
identities and register endpoint cleanup with `EffectScope`. The scoped wrappers
expose `publish`, `replace`, bounded `drain`/`drain_updates`,
`serve_requests`, and readiness `event_source` methods. Schedule those operations
in the component's existing event loop; these helpers do not create a hidden
executor. Subscriber/client endpoints own a copy of `TransformBuffer` that
shares its state, so buffer observation and activation cleanup remain connected
if the original C++ buffer value is moved or destroyed.

Python `ActivationContext` offers `provide_dynamic_tf`, `provide_static_tf`,
`require_dynamic_tf`, and `require_static_tf`. They stage canonical capability
descriptors, verify the static pair's provider/process/activation, and own native
endpoint teardown. A retained Python handle expires when its activation ends.
The component uses existing managed tasks for serving snapshots and ingestion;
these helpers do not start an executor. For example, inside a consumer's
`activate` hook, with the three requirements declared through `run_component`:

```python
import rbt
from rbt.tf import TransformBuffer, TfTransportError

self.tf = TransformBuffer()
dynamic = ctx.require_dynamic_tf("dynamic", "odometry", self.tf)
static = ctx.require_static_tf("updates", "snapshots", "extrinsics", self.tf, 64)

def ingest(task):
    try:
        while not task.cancelled:
            dynamic.wait_and_drain(0.1)
            static.wait_and_drain(0.1)
            try:
                static.refresh_if_due(0.25)
            except rbt.RpcDeadlineExceededError:
                pass  # The next due permit retries; stale lookups fail closed.
    except (TfTransportError, rbt.RpcActivationExpiredError):
        if not task.cancelled:
            raise

ctx.spawn_background("tf-ingest", ingest)
```

The finite waits release the GIL and use native readiness; this is not a Python
payload polling queue. Each stream may carry many frame edges. Use bounded
managed source tasks, rather than a task or thread per edge or lookup. Static
providers similarly run `serve_one(timeout)` in an existing managed task.
The native transport methods return bounded `TfDrainReport` observations.

Direct native endpoints also remain available in `rbt.tf`: `DynamicTfPublisher`,
`DynamicTfSubscriber`, `StaticTfProvider`, and `StaticTfClient`, with descriptor
helpers `dynamic_transform_descriptor`, `static_update_descriptor`, and
`static_snapshot_descriptor`. Standalone endpoint construction requires explicit
close and does not advertise Composer roles. The native lifecycle example in
`python/tests/test_tf_component.py` demonstrates complete role declarations,
serving, ingestion, and cancellation using real component processes.

For TF transport, the generated IDL is in [`types/tf.rbt`](../types/tf.rbt).
It uses fixed records and bounded slice messages; samples are validated and
copied into local history, then native guards are released. C++ has separate
headers for local math/buffer and generated `Tf*` wire values, as described
above.

## Errors

Handle failures by category rather than substituting a different transform or
time. The local typed error families include:

- Frame/math/configuration: `InvalidFrame`, `InvalidTransform`,
  `InvalidQuaternion`, `InvalidConfig`, `CapacityExceeded`, and
  `TimestampUncertaintyExceeded`.
- Graph/provider: `UnknownFrame`, `DisconnectedFrames`, `MissingTransform`,
  `ConflictingParent`, `ConflictingAuthority`, `CycleDetected`, and
  `ProviderUnavailable`.
- History: `PastExtrapolation`, `FutureExtrapolation`,
  `InsufficientHistory`, `InterpolationGapTooLarge`,
  `TransformEpochMismatch`, and `OutOfOrderSample`.
- Static snapshot: `SnapshotStale`, `SnapshotRevisionGap`, and
  `SnapshotTooLarge`.
- Time: the underlying v0.7 clock/timestamp error, such as unknown uncertainty,
  mapping unavailable/expired/invalidated, clock-domain mismatch, or generation
  mismatch.

Rust carries time failures as `TfError::Time(rbt_time::TimeError)`. C++ exposes
`TfErrorCode` and retains an optional underlying `rbt::time::TimeError`; named
clock failures have corresponding codes. Python raises `TfError` with a stable
snake-case `.code`, including `time_`-prefixed clock errors. Native stream, RPC,
wait, and inactive-endpoint failures are separate transport errors. Rust uses
`TransportError`, and C++ uses `TfTransportError`. Python preserves existing
`rbt.RpcError`, `rbt.StreamError` and `rbt.WaitError` families, and uses
`TfTransportError` for TF-specific endpoint/pairing failures. These are distinct
from graph lookup errors.

Failed snapshot RPCs, invalid responses and rejected static updates leave the
provider view stale, so lookups cannot use it as a fresh calibration. If an
update is rejected while a snapshot request is in flight, that captured
snapshot is not accepted as a successful refresh. A valid revision gap can be
repaired by a complete authoritative snapshot covering the observed revision.
Retry through the bounded steady-time resync policy; lookup itself never
requests a snapshot.

## Camera example and hardware gate

[`examples/10-tf-camera`](../examples/10-tf-camera/README.md) connects the
existing V4L2 camera stream to capture-time lookup using a supplied static
extrinsic. The software-only check does not open a device:

```sh
scripts/test_tf_hardware.py --self-test softwareonly
```

It reports `SOFTWARE_FIXTURE`; it is not a physical-camera pass. The camera
example exercises a static extrinsic at the camera's capture timestamp. It does
not claim a measured calibration or a physical dynamic-pose TF source.

The 300-second hardware gate is explicitly opt-in. Select the device and a
reviewed config file yourself; the command does not scan for or choose hardware.
The file must contain the required static path from `base_link` to the selected
frame. A placeholder path is shown below so the command requires an explicit
config rather than silently using fixture values:

```sh
export RBT_PHASE4_TOOLCHAIN="$PWD/target/phase4-toolchain"
export RBT_IOX2_PREFIX="$PWD/target/iox2-rbt-0.10.0-cpp-install"
export PATH="$RBT_PHASE4_TOOLCHAIN/bin:$PATH"
export CC="$RBT_PHASE4_TOOLCHAIN/bin/x86_64-conda-linux-gnu-gcc"
export CXX="$RBT_PHASE4_TOOLCHAIN/bin/x86_64-conda-linux-gnu-g++"
export AR="$RBT_PHASE4_TOOLCHAIN/bin/x86_64-conda-linux-gnu-ar"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="$CC"
export LD_LIBRARY_PATH="$RBT_PHASE4_TOOLCHAIN/lib:$RBT_IOX2_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LIBRARY_PATH="$RBT_PHASE4_TOOLCHAIN/lib:$RBT_IOX2_PREFIX/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"

cargo build -p composer -p rbt-tf-camera
cmake -S sdk/cpp -B target/tf-camera-cpp-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_PREFIX_PATH="$RBT_IOX2_PREFIX"
cmake --build target/tf-camera-cpp-build --target rbt_time_camera

scripts/test_tf_hardware.py \
  --device /dev/video2 \
  --config /absolute/path/to/reviewed-camera-extrinsics.toml \
  --frame-id camera_optical \
  --duration 300
```

Replace `/dev/video2` with a device you selected and the config placeholder with
a reviewed static extrinsics file; do not use the example fixture as measured
calibration. The gate records its commands and component/resource observations.
Any successful result must be read from that run's evidence; the guide makes no
claim that this hardware run has been performed. See the example README for
manual component launch details and the acceptance conditions.

## Version and safety note

The TF API and examples are part of the accepted v0.8 software release, with
product and package version 0.8.0. The unsafe audit context is 49 baseline
production markers plus 24 approved generated TF binding markers, 73 total;
no handwritten unsafe code was added for TF. Generated wire bindings own
those additional markers. This guide does not replace the repository's
release or acceptance report.

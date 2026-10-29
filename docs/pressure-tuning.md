# Pressure observability and tuning

This guide describes the bounded pool and pressure instrumentation introduced
in rbt v0.4 and maintained in the current Rust, C++, and Python local
shared-memory APIs and Composer/CLI diagnostics. Production uses maintained
iceoryx2 `0.10.0-rbt.2` from
[`third_party/iceoryx2-rbt/source`](../third_party/iceoryx2-rbt/source); see the
[unified baseline report](iceoryx2-unified-report.md). The older 0.9.3 source
audit is historical and is linked where relevant below.

The architectural boundary is deliberately small:

> rbt pressure semantics define what happens under resource contention;
> resource tuning only controls how much bounded capacity is provisioned
> before those semantics apply.

> Observability must explain pressure without changing pressure behavior.

The original pressure milestone added no blocking retry, automatic resize,
adaptive tuner, allocator change, or payload-schema change.

## Profile semantics are not resource capacity

StreamProfile describes what a stream does when its bounded resources are under
pressure. StreamResourceConfig describes only two explicit capacity ceilings.
Do not create profile variants for cameras, Python, recorders, or large
payloads.

| Profile | Queue depth | History | Safe overflow | Default max_loans | Default max_borrowed |
| --- | ---: | ---: | --- | ---: | ---: |
| Sensor | 1 | 0 | true; replace an unread sample per slow subscriber | 2 | 1 |
| Control | 4 | 0 | false; report delivery pressure | 2 | 1 |

These values are frozen. A Sensor slow consumer must not indefinitely block a
producer; a Control queue must not silently overwrite an unread command.

Control remains bounded, non-transactional multicast: one QueueFull result
means at least one delivery failed, not that no other subscriber received it.

The default constructors still apply the table above. Existing code that does
not mention StreamResourceConfig therefore retains the existing behavior.

## Resource configuration

StreamResourceConfig has exactly these public capacity fields:

| Field | Meaning | Unit | Valid values | Scope and matching |
| --- | --- | --- | --- | --- |
| max_loans | Maximum unsent loans held by one publisher | samples | positive native usize/size_t | Provider/service ceiling. A publisher must match the descriptor value when opening. |
| max_borrowed | Maximum active receives retained by a subscriber | samples | positive native usize/size_t | Advertised service ceiling plus a subscriber-local lower ceiling. A subscriber request may be smaller, never larger. |

The provider applies max_loans to its publisher builder and max_borrowed to
the service subscriber ceiling. A subscriber's local borrow reservation is
enforced by rbt before each native receive, so lowering it does not require
changing the service. The publisher's max_loans is not a subscriber-local
setting. The effective snapshot keeps the advertised loan ceiling and reports
the subscriber's local borrow ceiling.

Resource values are fixed at endpoint creation. There is no set_max_loans or
runtime grow/shrink operation. Zero, values that cannot be represented by the
native platform type, and incompatible descriptor requests are rejected early
with typed errors; the transport is never silently clamped.

The following are deliberately not exposed as stream resource knobs:

~~~
queue depth, history, safe overflow, pool_bytes, allocator pages,
shared-memory segment bytes, fragment count, retry policy
~~~

Queue/history/overflow come from the profile. The other names have no truthful
portable rbt implementation.

### Descriptor and compatibility rules

The effective resources are carried in StreamDescriptor.resources and in the
descriptor JSON/control metadata. They are transport metadata, not part of
MessageTypeDescriptor and never part of a payload layout hash. A descriptor
written before resource metadata existed deserializes with the defaults.

Changing max_loans or max_borrowed therefore does not create a new message ABI.
The message hashes remain:

~~~
Image          943fa4008cf0f55f89a281b051c69a6f
PointCloud     d9da2bb29bb2cf217c40f1a56d1fb755
ControlCommand 9381cf15842a48ac0964111f0f45ef72
~~~

The provider publishes the descriptor it used to create the physical service.
Rust and C++ publishers reject a request whose resource values do not match
that advertised service. Rust and C++ subscribers reject a local
max_borrowed above the advertised ceiling and may choose a smaller value.
Python's public subscriber wrappers expose only max_borrowed; publisher and
component-provision APIs expose both fields. IncompatibleResourceConfig
(StreamError in Rust/C++) identifies a mismatch; InvalidResourceConfig
identifies a non-positive or unrepresentable value.

## SDK examples

### Rust

Create a descriptor with explicit transport resources, then use the matching
publisher and a subscriber-local borrow ceiling:

~~~
use rbt_iox2::{Image, SliceStream, StreamDescriptor, StreamProfile, StreamResourceConfig};

let resources = StreamResourceConfig::new(4, 2);
let descriptor = StreamDescriptor::for_slice_with_resources::<Image>(
    "rbt/camera/image", StreamProfile::Sensor, resources,
);
let publisher = SliceStream::<Image>::publisher_with_resources(
    &descriptor, 1920 * 1080 * 3, resources,
)?;
let subscriber = SliceStream::<Image>::subscriber_with_resources(
    &descriptor, StreamResourceConfig::new(resources.max_loans, 1),
)?;
~~~

Stream::publisher() and subscriber() remain the simple default constructors.
The equivalent fixed-message APIs are Stream<T>::publisher_with_resources and
Stream<T>::subscriber_with_resources.

### C++

Use the same resource names and profile semantics in the native SDK:

~~~
rbt::StreamResourceConfig resources{4U, 2U};
using Header = rbt::sensor::ImageHeader;  // from generated/cpp/rbt/sensor.hpp
const auto& image_type = rbt::sensor::IMAGE_DESCRIPTOR;
rbt::StreamDescriptor descriptor{
    "rbt/camera/image", image_type, rbt::StreamProfile::Sensor, resources};

auto publisher = rbt::SlicePublisher<Header, std::uint8_t>::create(
    descriptor, image_type, 1920U * 1080U * 3U, resources);
auto subscriber = rbt::SliceSubscriber<Header, std::uint8_t>::create(
    descriptor, image_type, rbt::StreamResourceConfig{resources.max_loans, 1U});
~~~

The exact generated binding type varies with the schema; fixed publishers and
subscribers use the corresponding create(..., StreamResourceConfig) overload.
Endpoints and loan/receive guards remain move-only RAII owners.

### Python

Python publishers can select both ceilings. Public data subscribers select the
local borrow ceiling:

~~~
import rbt

descriptor = rbt.StreamDescriptor.image("rbt/camera/image")
publisher = rbt.ImagePublisher.open(
    descriptor, max_loans=4, max_borrowed=2,
)
subscriber = rbt.ImageSubscriber.open(descriptor, max_borrowed=1)
~~~

ActivationContext.provide_image() and provide_point_cloud() accept both
keyword arguments. ActivationContext.require() and the public
ImageSubscriber.open()/PointCloudSubscriber.open() accept max_borrowed.
InvalidResourceConfigError and IncompatibleResourceConfigError are typed
native exceptions. The NumPy ownership model is unchanged: received arrays
and derived views retain the native sample owner, while writable publisher
arrays retain the loan lease until every alias is gone.

## Pressure snapshots

Each live Rust or C++ endpoint owns a small metrics state. The data path only
performs bounded atomic updates and ownership-guard bookkeeping. Composer does
not receive one event per sample. In addition to existing lifecycle snapshots,
a component exports periodic aggregate pressure refreshes at most once per
second; snapshot updates do not increment a topology revision, reconcile
dependencies, activate/deactivate components, or carry payloads.

Pressure instrumentation first advanced the component snapshot schema from v2
to v3 in the v0.4 milestone. The current component snapshot is v4 and carries
both Stream and RPC pressure observations; the Composer TCP/NDJSON control
protocol remains v2. Payload IDL and descriptor versions, layout hashes,
SidecarV1, and MCAP encoding are unchanged. Rust and C++ negotiate control and
snapshot versions independently and reject incompatible peers explicitly.
This guide details Stream metrics; see [rpc.md](rpc.md) for RPC metrics and
their call semantics.

The process-local registry uses weak endpoint references. Composer outage does
not stop data-plane accounting; counters continue locally and the next
snapshot reflects the latest values. A snapshot is diagnostic telemetry, not a
transactionally consistent accounting ledger: individual atomics can be read
at slightly different instants. Current ownership gauges, however, are exact
and saturating; they must not underflow or leak.

Within an available endpoint snapshot, a numeric counter value of zero means
that no matching events have been counted for that endpoint. Optional `null`
values are field-specific: `oldest_borrow_age_ns` is null when there is no
active borrow, while generic Stream wrappers leave `estimated_dropped_samples`
null because they cannot infer a count. A missing or stale Composer report is
unavailable or stale, not a zero-valued observation.

Counters and high-water values live for the physical endpoint's
CapabilityInstanceId. Provider replacement creates a new physical service and
a fresh metric lifetime. A retained old sample keeps only the small metric
owner needed to finish its accounting and cannot decrement a replacement's
counters. Historical endpoint state is bounded and reclaimable after the last
loan/borrow owner is released.

## Metric contract

The native field names below are the stable names in Rust/C++ snapshots and
JSON. Python preserves compatibility aliases such as loan_high_water,
loan_errors, duration_count, and readiness_notify_failures; use the
descriptive names in new code.

| Name | Scope | Unit | Meaning | Lifetime / reset | Exactness |
| --- | --- | --- | --- | --- | --- |
| loan_attempts | publisher | attempts | Calls that tried to obtain a loan | capability lifetime; no automatic reset | exact wrapper calls |
| loan_successes | publisher | loans | Successful native loans | capability lifetime | exact |
| loan_failures | publisher | failures | Loan attempts classified as pool exhaustion/out-of-memory | capability lifetime | exact classified failures |
| loans_in_use | publisher | loans | Current unsent loan ownership | live ownership | exact, returns to zero after release |
| loans_high_water | publisher | loans | Maximum simultaneous loans | capability lifetime | exact high-water |
| payload_bytes_last | publisher | bytes | Payload bytes in last successful loan | latest sample; zero before first success | exact for wrapper-reported payload |
| payload_bytes_max_seen | publisher | bytes | Largest successful loan payload | capability lifetime | exact observed maximum |
| total_loaned | publisher | bytes | Saturating sum of successful loan payload sizes | capability lifetime; no automatic reset | exact until saturating overflow |
| send_attempts | publisher | sends | Calls that attempted send | capability lifetime | exact |
| send_successes | publisher | sends | Sends without transport error | capability lifetime | exact API outcome |
| send_failures | publisher | sends | Sends with a transport error | capability lifetime | exact API outcome |
| queue_full | publisher | events | Control delivery failure due to a full subscriber queue | capability lifetime | exact observed callback/failure classification |
| unable_to_deliver | publisher | events | Other incomplete delivery outcomes | capability lifetime | exact classification |
| readiness_notify_failures | publisher | events | Readiness sidecar notification failures | capability lifetime | exact; separate from payload result |
| subscriber_count | publisher | subscribers | Aggregate transport-observed subscribers | latest observation | transport observation, not identity-preserving |
| receive_attempts | subscriber | attempts | Calls that attempted receive | capability lifetime | exact |
| received_samples | subscriber | samples | Successful receives | capability lifetime | exact |
| borrowed_samples | subscriber | samples | Current sample guards retained by application | live ownership | exact, returns to zero after release |
| borrowed_high_water | subscriber | samples | Maximum simultaneous active borrows | capability lifetime | exact high-water |
| borrow_failures | subscriber | failures | Receive attempts rejected at the borrow limit | capability lifetime | exact |
| borrow_duration_count | subscriber | completed borrows | Completed borrow lifetimes with timing | capability lifetime | exact |
| borrow_duration_total_ns | subscriber | nanoseconds | Saturating sum of completed borrow durations | capability lifetime | monotonic-clock duration |
| borrow_duration_max_ns | subscriber | nanoseconds | Longest completed borrow | capability lifetime | monotonic-clock duration |
| oldest_borrow_age_ns | subscriber | nanoseconds | Age of the oldest active borrow, if one exists | current outstanding borrows | exact bounded scan; null means none |
| estimated_dropped_samples | either | samples | Message-aware sequence-gap estimate, when available | capability lifetime | absent/null for generic wrappers, never fabricated as zero |
| snapshot_timestamp_unix_ms | snapshot | Unix milliseconds | Wall-clock timestamp carried by the low-rate snapshot | latest snapshot | exact timestamp metadata |
| age_ms | CLI/Composer | milliseconds | Saturating inspection-time age of a snapshot | latest inspection | exact display calculation |
| fresh | CLI/Composer | boolean | Whether age_ms is at most 2,500 ms | latest inspection | display label, not health |

All cumulative totals use saturating arithmetic rather than silently wrapping.
loans_in_use and borrowed_samples are not totals and are maintained by RAII
guards on every success, send/error/drop, move, and final alias release.
Borrow start times use a monotonic process-local clock. The Unix-ms snapshot
timestamp is for display freshness, not for measuring borrow duration.

### Ownership paths

The metrics owner is attached to the object that owns the transport resource:

~~~
Rust TX: loan() ──> LoanTracker ──> send / error / Drop ──> loans_in_use - 1
Rust RX: receive() ──> BorrowTracker ──> guard Drop ──> duration + borrowed - 1

C++ TX: LoanedSample (move-only RAII) ──> send / destructor ──> LoanTracker
C++ RX: ReceivedSample (move-only RAII) ──> destructor ──> BorrowTracker

Python RX:
  NumPy array / memoryview
      └─> native PyO3 owner
          └─> ReceivedSliceSample
              └─> iceoryx2 guard + BorrowTracker
~~~

Deleting the original Python ndarray while a crop or other derived view is alive
does not release the borrow. The final alias release does. The same principle
applies to writable Python publisher aliases: send() while a mapping remains
raises LoanStillMappedError, and the loan remains accounted until all aliases
disappear.

## CLI and JSON diagnostics

Use the read-only Composer inspection commands while a stream is active:

~~~
rbt stream info Camera/Image
rbt stream pressure Camera/Image
rbt stream pressure Camera/Image --json
~~~

These live CLI commands query Composer, so Composer must be reachable for a
current result. The data plane and local accounting do not depend on Composer:
if it is absent, streams continue and local counters accumulate, while the CLI
reports the snapshot as unavailable or stale instead of manufacturing zeros.

Human output separates semantics (queue/history/overflow), resources,
publisher counters, subscriber observations, and pressure assessments. JSON
contains stable metadata fields including capability_instance,
provider_component, provider_process, activation, profile, resources,
snapshot/pressure, snapshot_freshness, and bounded
subscriber_observations. A missing snapshot is null/unavailable, not a
zero-filled snapshot.

The display freshness cutoff is 2,500 ms. It is a conservative label only:
fresh means the snapshot timestamp is within that window, while stale means it
is older. Freshness is not a health score and does not alter runtime behavior.
If Composer has no matching provider report, the CLI says unavailable; it does
not imply measured zero.

Assessment reason codes are factual and stable:

~~~
loan_capacity_reached
borrow_capacity_reached
outstanding_borrow
control_delivery_pressure
readiness_failures_observed
~~~

Guidance is advisory. It never mutates a running endpoint, retries a failed
send, or promises an optimal size. For a publisher that reached its loan high
water and recorded failures, it may suggest testing max_loans + 1 if the memory
budget permits. For an outstanding borrow it first points to ownership and
oldest_borrow_age_ns; increasing max_borrowed can otherwise mask a retention
bug. QueueFull guidance reminds operators that other subscribers may already
have received the sample. Readiness failures remain separate from payload send
outcomes because a payload may already have been delivered.

For a known maximum payload, the CLI may show a payload-only estimate for one
additional loan:

~~~
additional payload-only capacity ≈ payload_bytes_max_seen × added_loans
~~~

This excludes allocator metadata, alignment, queues, and transport overhead.
For a 20 MiB PointCloud, moving max_loans from 2 to 4 can therefore reserve
roughly 40 MiB of additional payload capacity, subject to the actual
configuration and middleware allocation. It is an estimate, not a memory
budget or a universal sizing formula.

## Tuning workflow

1. Run with the frozen defaults and keep the profile unchanged.
2. Inspect rbt stream pressure and its JSON form.
3. Distinguish a loan capacity event, a retained borrow, Sensor replacement,
   Control delivery pressure, and a readiness failure.
4. Check high-water values, failure totals, payload_bytes_max_seen, and
   oldest_borrow_age_ns before changing a limit.
5. Release stale Python/C++/Rust owners; copy a Python array when long-term
   ownership is intended.
6. If capacity is genuinely required, create a new endpoint with explicit
   max_loans or a subscriber-local max_borrowed, after checking the memory
   budget. Re-run the workload and compare counters, RSS, and ownership gauges.
7. Verify that current loans/borrows return to zero after cleanup and that a
   provider replacement starts a new CapabilityInstanceId metric lifetime.

Increasing max_loans or max_borrowed is not automatically better. Larger
limits consume more bounded memory and can allow longer retention. There is no
universal FPS × latency formula: occupancy also depends on borrow ownership,
multiple subscribers, queue semantics, and middleware behavior.

### 1080p Sensor example

For a 1920×1080 RGB8 image (6,220,800 payload bytes), suppose defaults show
loans_high_water=2 and repeated loan_failures. Test a new endpoint with
max_loans=3 and the same Sensor profile. The payload-only increment is
approximately 6,220,800 bytes for one additional loan; allocator and transport
overhead are extra. Record loans_high_water, failures, borrow ages, Sensor
sequence-gap estimates, RSS, and CPU before deciding whether the change is
useful. This example is a workflow, not a host-independent result.

## What remains deliberately unknown

The current generic wrappers cannot see every per-subscriber queue occupancy
or exact Sensor overwrite count in maintained iceoryx2 `0.10.0-rbt.2`. They
report `estimated_dropped_samples` as unknown/null rather than zero; it is not a
transport counter. The former 0.9.3 source audit is preserved in
[runtime-pressure-notes.md](runtime-pressure-notes.md). MCAP recorder drops
remain recording-layer metrics and are not merged into stream pressure.

The low-rate snapshot path also cannot make a crash emit a final perfect
counter set. A process death can trigger normal middleware cleanup, but rbt
does not claim an unmeasured cleanup SLA. Pressure telemetry is operational
diagnostics, not crash-proof distributed accounting.

WaitSet, SidecarV1 readiness, MCAP schemas/encoding, payload layouts, Python
ownership, and existing error meanings are unchanged. The current unsafe audit
is 49 legacy locations plus 24 generated TF `ShmSafe`/`ZeroCopySend` marker
locations, for 73 total; no handwritten unsafe locations were added. See the
[TF unsafe review](tf-unsafe-review.md) for the approved exception and audit
requirement.

See pressure-tuning-report.md for the evidence matrix and for results that have
actually been run on a named host.

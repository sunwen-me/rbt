# Real-hardware time

Time support was introduced in v0.7 and is part of the current v0.8.0 SDK.
The [documentation index](README.md) links the language guides and
[TF](tf.md), which uses these same timestamps and mappings for local lookups.

Runtime time keeps the system alive and responsive. Robot data time describes
when physical measurements occurred. Sensor timestamps never become runtime
deadlines. RPC, WaitSet, cancellation, liveness and component shutdown retain
their existing real monotonic clocks.

The pre-change contracts and historical/current dependency distinction are in
[time-audit.md](time-audit.md). Test evidence and remaining limitations are in
[time-report.md](time-report.md).

## Clock identities and readings

`rbt::time`, C++ `rbt/time.hpp` (`rbt::time`) and Python `rbt.time` expose three
kinds: host monotonic, system realtime, and device. A timestamp has an explicit
clock identity and signed 64-bit integer nanoseconds. An identity includes the
kind, a fixed 48-byte source ID and a 64-bit generation. No core representation
uses floating-point seconds.

Linux host monotonic means `clock_gettime(CLOCK_MONOTONIC)`, not a serialized
Rust `Instant` or an arbitrary C++ `steady_clock` origin. It advances while the
machine runs, is unaffected by discontinuous wall-clock corrections, and does
not count suspend time. Its rate can be disciplined by the operating system.
Use it for elapsed time. Realtime means `CLOCK_REALTIME`, with Unix epoch
nanoseconds. It is suitable for wall-clock logging and can step forward or
backward. A realtime subtraction is a difference of clock readings; it is not
proof of physical elapsed time.

Rust reads the bound local clocks directly:

```rust
use rbt::time::{HostClock, SensorTiming};
let clock = HostClock::new()?;
let start = clock.monotonic_now()?;
let received = clock.monotonic_now()?;
let elapsed = received.checked_duration_since(&start)?;
let timing = SensorTiming::new(None, received, None, None)?;
// timing.capture() remains None; elapsed is a std::time::Duration.
```

C++ uses the same identified kernel readings and chrono durations:

```cpp
#include <rbt/time.hpp>
auto clock = rbt::time::HostClock::create();
if (!clock) { /* handle clock.error() */ }
auto start = clock.value().now_monotonic();
auto end = clock.value().now_monotonic();
auto elapsed = rbt::time::checked_duration_since(end.value(), start.value());
// elapsed.value() is std::chrono::nanoseconds; check results before value().
```

Use the C++ `create` factories for validated values. Default identities and raw
timestamp/timing aggregates are unvalidated carriers; checked operations,
mapping construction and the transport codec reject malformed identities.

Python exposes integer nanoseconds and the full identity:

```python
from rbt.time import HostClock, SensorTiming
clock = HostClock()
start = clock.monotonic_now()
received = clock.monotonic_now()
elapsed_ns = received.checked_duration_since(start)
timing = SensorTiming(capture=None, received=received)
print(received.clock, received.nanoseconds)
```

`HostClock` is a local bound object. Construction reads Linux boot and current
thread time-namespace metadata; monotonic reads validate the namespace before
and after sampling. Reading time needs no Composer, daemon, RPC or global clock
registry. Native Rust/C++ reads and checked arithmetic avoid per-reading heap
allocation and mutexes; Python retains its normal small immutable timestamp
objects, and reads do not take writer locks. An unknown host basis fails with a
typed error. There is no fallback to a process-relative epoch or to an
unlabelled wall timestamp.

The C++ `HostClock::now_monotonic` hot path reads
`/proc/thread-self/ns/time` with `readlink` into a fixed stack buffer and parses
the canonical `time:[inode]` name before and after reading the clock. This
optimization relies on Linux v5.15 `fs/nsfs.c`: `ns_get_name` formats the
namespace type and inode in that form, while `ns_match` uses the namespace
inode and nsfs superblock; `nsfs_mnt` supplies the boot-global nsfs mount and
device. `HostClock::create` still obtains the full `st_dev` and inode identity
with `stat`, while each C++ sample can validate the inode alone against that
bound identity. The checks remain on both sides because `setns(2)` permits a
single-threaded process to join a time namespace. A failed, malformed or
truncated link read returns `ClockUnavailable`; an inode mismatch returns
`TimeSourceReset`. This readlink optimization is specific to the C++ SDK; Rust
and Python retain their existing implementations.

Sources: [Linux v5.15 nsfs implementation](https://raw.githubusercontent.com/torvalds/linux/v5.15/fs/nsfs.c)
and [`setns(2)`](https://man7.org/linux/man-pages/man2/setns.2.html).

The 48 source bytes are canonical and identical across languages:

| Bytes | Host monotonic | System realtime | Device |
| --- | --- | --- | --- |
| 0–15 | Linux boot UUID, textual UUID byte order | same boot UUID | caller-supplied stable nonzero device UUID |
| 16–23 | time namespace `st_dev`, u64 little endian | zero | zero |
| 24–31 | time namespace inode, u64 little endian | zero | zero |
| 32–39 | monotonic namespace offset seconds, i64 little endian | zero | zero |
| 40–47 | monotonic namespace offset nanoseconds, i64 little endian | zero | zero |

The current namespace is `/proc/thread-self/ns/time`, and its offset basis is
`/proc/<TID>/timens_offsets`, resolving the current kernel thread ID from
`/proc/thread-self` (the task-directory `timens_offsets` path is not present
on all kernels). Linux freezes offsets after a process joins
a namespace. Realtime is not time-namespaced. Hex source IDs contain exactly
96 lowercase digits. The source ID is an identity, not an accuracy certificate.
Different hosts, boots or incompatible namespace bases cannot be silently
compared. See the [Linux clock documentation](https://man7.org/linux/man-pages/man2/clock_gettime.2.html)
and [time namespaces](https://man7.org/linux/man-pages/man7/time_namespaces.7.html).

Device UUIDs must identify a particular clock source, not merely a sensor
model or an application topic name. The driver owns its continuity contract.
On a confirmed reset, `DeviceClock::reset` advances the generation with checked
arithmetic. Timestamp generation 1 cannot be compared with generation 2 even
if both contain plausible numeric readings. If device ticks are not already
nanoseconds, the driver must explicitly scale them with checked integer math
and retain its documented tick rate and raw source evidence.

## Comparisons and arithmetic

There is no implicit ordering or subtraction of timestamp objects. Checked
comparison and difference validate clock kind, then source, then generation.
They return distinct errors for these mismatches. Addition and subtraction
reject overflow. `checked_duration_since` rejects a negative difference;
`checked_difference` supports signed differences within i64 nanoseconds.

Two camera and IMU hardware clocks cannot be compared merely because their
readings have the same units. Matching numeric values do not establish a
shared epoch, rate, synchronization or device generation. Host realtime and
host monotonic are different domains even on one machine.

Legacy Image and PointCloud `timestamp_ns` contain no clock identity, and
their capture/receive meaning is **UNKNOWN**. Existing messages, APIs, layout
hashes and canonical encodings retain their old behavior. The time-aware
legacy helper returns `TimestampState::Unknown`, including for a zero value.
An explicitly identified reading can be `Known`; a device reading requiring
conversion can be represented as `Unmapped`. No missing mapping implies an
offset of zero.

## Capture, driver event and receive time

`SensorTiming` keeps optional capture and mandatory receive timestamps
separate, with independently optional uncertainty bounds. Missing capture
remains absent. A software receive timestamp is not exposure or measurement
time. Publication, if an application measures it, is another event.

Driver metadata must establish both clock domain and event semantics. V4L2
`TIMESTAMP_MONOTONIC` labels a clock; its `TSTAMP_SRC_SOE` flag labels start of
exposure. EOF is a driver frame-end event and does not establish exposure
start. Unknown/COPY timestamp flags do not establish an acquisition domain.
The physical-camera example preserves raw driver nanoseconds, flags and
session generation even when capture is unknown. It only sets capture for
verified SOE/monotonic metadata with an established host basis. The example
conservatively requires zero namespace offset before equating a driver kernel
timestamp with the user clock. See [V4L2 buffer flags](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/buffer.html).

An observed repeated or backwards driver sequence establishes a new driver
session generation, with the raw transition recorded and its cause unconfirmed.
It does not change a verified host-monotonic clock identity. This prevents
reused driver sequence numbers from being associated with an earlier session.

Receive time is sampled after dequeue in userspace. Driver-to-receive latency
can be computed only for proven compatible identities; it is separately named
and is not relabelled capture-to-receive latency. A nanosecond integer format
does not imply one-nanosecond physical accuracy. Unknown acquisition time and
unknown accuracy remain visible.

## Explicit clock mappings

`ClockMappingCalibration` accepts a trusted external calibration, not an
automatic estimator. It specifies full source/target identities, source and
target anchor readings, a positive rational rate, an inclusive source validity
interval, a nonzero evidence ID, a real host-monotonic calibration/expiry
interval and a known uncertainty bound. Evidence IDs identify the producer's
calibration records; rbt cannot certify the calibration merely from a token.

A mapping whose source and target have the same full identity must preserve
that clock: the anchors must match and the rate numerator must equal its
denominator. Equivalent identity rates such as 2/2 are accepted. An offset or
rate change requires a distinct clock identity.

Conversion uses:

```text
delta = source_ns - source_anchor_ns
target_ns = target_anchor_ns + trunc_toward_zero(delta * rate_num / rate_den)
```

Signed 128-bit intermediate arithmetic and final i64 conversion are checked.
The mapping uses anchored integer math rather than multiplying a large Unix
timestamp by a floating-point rate. The nominal source reading must fall in
the calibration interval. Calibration uncertainty must bound the supplied
relationship over that declared interval.

For known input uncertainty `u`, the returned conservative bound is:

```text
ceil(u * rate_num / rate_den) + calibration_uncertainty
    + (1 ns if the timestamp conversion discarded a fractional nanosecond)
```

Products, additions and u64 uncertainty bounds are checked. Known zero remains
zero where no rounding is needed. Unknown input or calibration uncertainty is
rejected for validated conversion. It is never silently promoted to 1 ns.

Every conversion verifies full source identity, requested target identity,
calibration clock identity, `calibrated_at <= now < expires_at`, source range
and invalidation state. Wrong source, generation, target, rate, range, missing
mapping, expiry, unknown uncertainty and overflow produce typed failures.
Objects hold one calibration each: there is no unbounded mapping history,
clock registry or per-sample tracker.

## Confirmed discontinuities

Device reset and confirmed realtime correction carry the previous and current
identities in `ClockDiscontinuity`; the generation advances by one. For an
externally confirmed step that does not provide a new generation, use
`ClockDiscontinuity.external_step(clock)`, which marks that exact identity as
affected. `ClockMapping::invalidate` checks both identities on a generation
change and the exact identity on an external step; conversion then returns
`MappingInvalidated`. A new generation also fails identity validation against
an old mapping automatically.

Device identity reads and mapping validity checks avoid locking on each sample.
A concurrent reset can make a device identity read observe either the old or
new immutable identity; drivers must serialize hardware events with resets.
Mapping invalidation writes remain serialized.

`HostClock` supports explicit confirmation of a realtime discontinuity and
advances its observer's realtime generation. This is not a system-wide
detector or a cross-process authority. Drivers/synchronization services must
propagate confirmed resets to the objects using their calibrations. rbt does
not claim to observe every NTP/PTP step or to change system time. A camera
driver restart does not reset the host kernel monotonic clock; its driver
session generation and device mappings are reported separately.

## Stamped sample transport

`rbt_time_stream::StampedBytes` is a new message identity
`rbt.time/StampedBytes@1`. It uses existing native Stream/RPC slice capabilities,
a u64 sequence header and a byte slice. It reuses the existing safe primitive
ABI implementations and adds no unsafe. The first 192 payload bytes contain
versioned timing metadata; application data follows in the same native sample.
Loan and borrowed view ownership, pressure, readiness and cancellation retain
their existing behavior. Timing metadata never travels as an unrelated
per-sample sidecar or through Composer.

Its layout hash is `97e9a924992dedfe83196197cccb6b35`; the existing five frozen
message hashes are unchanged. The Rust facade reexports it as `rbt::time_stream`.

| Prefix offset | Field |
| --- | --- |
| 0–7 | ASCII `RBTTIME1` |
| 8 | flags: capture present=1, capture uncertainty known=2, received uncertainty known=4 |
| 9–15 | reserved zero |
| 16–87 | optional capture timestamp (all zero when absent) |
| 88–159 | receive timestamp |
| 160–167 | capture uncertainty u64 LE; flag distinguishes unknown from known zero |
| 168–175 | receive uncertainty u64 LE; same distinction |
| 176–191 | reserved zero |

A 72-byte timestamp has kind at byte 0 (1 monotonic, 2 realtime, 3 device),
reserved zero bytes 1–7, source bytes 8–55, generation u64 LE at 56 and signed
nanoseconds i64 LE at 64. Codecs validate magic, length, kind, source encoding,
flags, reserved bytes and absent-field invariants. Application payload views
start at byte 192 and borrow their native sample; image bytes are not copied
by this slicing operation. Python writable aliases retain their native loan
and prevent send while exported; received views remain read-only.
The Python timing-prefix decoder also borrows a fixed 192-byte read-only native
view whose owner retains the sample; it does not duplicate the transport bytes.

This distinct descriptor cannot be opened as legacy Image or PointCloud.
Its layout signature includes every timing field and application offset. The
physical-camera application adds a 48-byte `RBTVCAM1` metadata block containing
geometry, pixel FourCC, raw driver timestamp/flags and driver-session
generation before camera bytes. Copying from the device's DMA buffer into a
native loan is an ingress operation; subsequent rbt transport uses the same
loan and borrow ownership.

No old IDL message or encoding changes. New custom stamped byte payloads do
not silently extend the recorder's existing supported-type list. MCAP
`rbt-canonical-v1`, `log_time`, `publish_time` and preserved legacy sensor
fields retain their existing behavior. Recording time is not hardware
acquisition time. No clock authority or virtual time is derived from MCAP.

## Local diagnostics

```bash
rbt time status
rbt time status --json
rbt time status --sources observations.json --json
```

Host clock availability and full source identity are read locally without
Composer. Optional `--sources` is an explicit local observation snapshot,
bounded to 1 MiB and 128 unique sources. Nothing scans for or invents sensor
sources. Installing chrony/PTP alone produces no synchronization claim.

The JSON retains the v0.5 `platform`, `checks`, `success` contract. Checks have
stable `check_id`, uppercase status and summary. Additive `host_clocks` and
`sources` include `source_id`, `clock_domain`, `generation`, `mapping_status`,
`uncertainty`, `freshness` and `reason_code`. Known zero, unknown, unavailable,
stale, invalid and expired remain distinct. Host availability is not evidence
that an independent device is synchronized.

Source snapshots contain `sources` entries with `name`, `clock`, optional
`observed_at` (identified host monotonic), `last_timestamp`, `uncertainty_ns`,
`mapping` (explicit `ClockMappingCalibration`) and `invalidated`. The report
validates calibration and last-sample identity and checks observation age
against 2.5 real monotonic seconds. Missing, future or incompatible observation
times cannot establish current synchronization. The input file remains the
producer's evidence, not an rbt synchronization service.

## Verification and future consumers

`scripts/verify_time.sh --quick` checks the time model, errors, three language
APIs, prefix/ownership, JSON, frozen hashes, unsafe count and dependency
provenance. `--full` adds actual cross-language samples, installed SDK/wheel
consumers, inherited regression suites, sanitizers and a minimum 300-second
software steady-time soak. Hardware is a separate gate with an explicit
device: `--hardware --device /dev/video0`.

Hardware tests must record actual timestamp flags and source evidence. A
software-only pass cannot count as physical acquisition. Stream close/reopen
proves an observed driver-session restart, not a physical unplug/replug. If a
device cannot be accessed, report `NOT RUN / HARDWARE UNAVAILABLE` with its
actual reason. Never replace it with a synthetic camera.

Future TF can accept these identified timestamps, explicit mappings and
uncertainty bounds. No TransformBuffer or TF distribution is introduced here.
Parameters remain a separate milestone.

## TODO: Simulation Time

- simulated clock source
- pause/resume
- seek/rewind
- replay-driven clock
- simulation-time timers
- clock epoch after rewind
- deterministic stepping/barrier

These are documentation TODOs only. v0.7 introduces no SimClock, virtual
provider, synchronization daemon or iceoryx2 PAL/CAL change.

# MCAP recording and playback

See the [documentation index](README.md) for current SDK versions and related
feature guides.

MCAP recording, inspection, and playback were introduced in v0.3 and remain
part of the current rbt SDK release, 0.8.0, on the maintained iceoryx2
0.10.0-rbt.2 baseline. The complete supported message set is `Image`,
`PointCloud`, and `ControlCommand`; generic recording or replay of the new TF
wire messages is not verified. MCAP is a durable file boundary; it is not a
replacement data plane, network transport, or lifecycle authority. The MCAP
file encoding remains version 1; the [documentation index](README.md) lists
the current independent frozen version axes.

## What a recording contains

`rbt record` takes one immutable Composer inspection snapshot, resolves a
fixed set of active physical streams, validates their exact ABI and SidecarV1
readiness contract, and opens ordinary side subscribers. It records:

- canonical message headers and payload values;
- a logical topic and capability type for each source;
- the recorded Sensor or Control profile;
- MCAP `log_time` and `publish_time` values;
- source component, process, activation, and capability identities as
  historical provenance;
- per-channel message, byte, transport-gap, and recorder-drop counters;
- file completeness, writer queue high-water/budget, and writer status.

It does not record the Composer graph, lifecycle commands, task events,
subscriber identities, transport queue contents, or physical shared-memory
objects. The selected source set is fixed for one invocation. If a
provider is replaced, the recorder never silently follows the new physical
identity; start a new recording to select it.

The recorder is a side subscriber:

```text
Publisher ───────────────→ normal Subscriber
     │
     └──→ Recorder → bounded byte queue → MCAP writer
```

It never enters the publisher-to-consumer path and does not change Sensor or
Control QoS. Composer is needed only to discover the initial descriptors. Once
the subscribers are open, a Composer outage does not stop local capture.

## Record

Select one or more capabilities by capability type, provider component,
capability UUID, or physical endpoint:

```bash
rbt record --output run.mcap --capability Camera
rbt record --output run.mcap \
  --capability 7f451d5e-0b08-4d0d-86f6-ff58f7a3f122 \
  --capability MotionCommand
```

A selector must resolve exactly once. Use a UUID or physical endpoint when a
display/type selector is ambiguous. To record every currently active supported
stream:

```bash
rbt record --output mixed.mcap --all
```

Useful bounded-run options are:

```bash
rbt record --output run.mcap --all --buffer-bytes 67108864 --duration 10
```

The writer queue limit is charged in encoded payload bytes, not item count.
The default is 64 MiB. Capture uses the native WaitSet; a readiness event only
moves readiness and the recorder then performs an ordinary receive. One
canonical buffer is produced while the borrow is live, the SHM borrow is
released, and the owned buffer is offered to the nonblocking writer queue.
The writer runs on one owned thread and is joined on shutdown.

SIGINT and SIGTERM wake capture, drain the bounded queue, write final metadata
and indexes, flush, sync, and print the JSON summary. SIGKILL cannot promise a
finalized file.

### Overload and completeness

Sensor capture must never backpressure or alter the live Sensor publisher. If
the recorder's own queue cannot accept an encoded Sensor sample, it increments
`recorder_drops`, marks the file `complete=false`, and continues. The file is
still inspectable and replayable, but the incompleteness is explicit both in
CLI output and `rbt.recording.summary` metadata.

Control capture must not silently lose a command. If its encoded command does
not fit in the writer queue, recording stops with a nonzero error after
finalizing an incomplete file when possible. Writer/disk failures likewise
stop the recorder and do not invalidate unrelated robot components.

`transport_sequence_gaps` describes gaps already visible at the selected
source. It is separate from `recorder_drops`; neither counter is fabricated
from transport behavior that the wrapper cannot observe.

## Inspect without Composer

Inspection is only a file operation and works without a running Composer:

```bash
rbt mcap info run.mcap
rbt mcap info run.mcap --json
```

It reports file size, duration and time range, schemas, channels, encodings,
layout hashes, message/byte counts, maximum slice lengths, and recording
metadata. The reader linearly validates the data and summary sections, CRCs,
record bounds, schema identity, and every supported payload; a corrupt file is
not reported as healthy merely because it has a summary.

## Play

Playback is a native rbt Component and therefore requires the usual Composer
runtime environment:

```bash
rbt play run.mcap               # relative real time, rate 1.0
rbt play run.mcap --rate 2.0    # twice the recorded speed
rbt play run.mcap --rate 0      # as fast as possible
```

Negative, infinite, and NaN rates are rejected. Nonzero scheduling blocks on
a monotonic condition-variable deadline; it never busy-spins. All channels
share one global schedule derived from MCAP `log_time`. Equal log times retain
file order. Message header timestamps are payload data and are never rewritten
or used as the replay clock.

Before creating any publisher, the player scans the complete file and checks
the MCAP profile, schema/message encoding, file encoding version, IDL language
and descriptor versions, exact ABI major, exact layout hash, canonical schema,
metadata bounds, payload size, and message-specific invariants. Unsupported
external channels fail by default. `--skip-unsupported` skips only genuinely
external channels; a malformed rbt channel never becomes skippable.

Every playback process receives a new `ProcessInstanceId`; its activation and
each provided capability also receive new identities. Historical physical IDs
remain provenance only. Logical channel metadata recreates meaningful
capability types on fresh physical endpoints. If equivalent live providers are
also present, normal Composer resolution/conflict behavior applies—there is no
special replay priority. Stop or isolate live providers when deterministic
provider selection is required.

Playback preserves the recorded Sensor/Control profile. It decodes headers and
payload elements directly into the final publisher loan and performs exactly
one send attempt. Control `QueueFull`/`UnableToDeliver` and
`ReadinessNotify` may follow partial payload delivery, so the player reports
the error and stops without retrying or duplicating that message. Sensor sends
also have no replay-only retry loop.

```text
MCAP → validated canonical bytes → final loaned SHM → Subscriber
```

File-to-SHM playback is not zero-copy: bytes must move from the file/page cache
into the final shared-memory loan. There is no complete intermediate payload
message copy.

## Schema and channel mapping

| rbt type | MCAP `Schema.name` | ABI/hash |
| --- | --- | --- |
| Image | `rbt.sensor/Image@1` | `1` / `943fa4008cf0f55f89a281b051c69a6f` |
| PointCloud | `rbt.sensor/PointCloud@1` | `1` / `d9da2bb29bb2cf217c40f1a56d1fb755` |
| ControlCommand | `rbt.control/ControlCommand@1` | `1` / `9381cf15842a48ac0964111f0f45ef72` |

This is the current complete MCAP type set. The recorder and player dispatch
only these generated types; support for generic TF recording and replay has
not been verified. See the [TF guide](tf.md) and its [acceptance report](tf-report.md)
for the current TF scope and verification status.

Every schema uses `Schema.encoding=rbt-idl-v1`; `Schema.data` is deterministic
canonical `.rbt` text generated from the same `CanonicalSchema` as all
language bindings and descriptors. Every channel uses
`message_encoding=rbt-canonical-v1`. Its topic is a logical
`/<component>/<capability>` slug, never a historical endpoint UUID.

Channel metadata keys are:

```text
rbt.message_namespace        rbt.message_name
rbt.abi_major                rbt.layout_hash
rbt.stream_profile           rbt.capability_type
rbt.capability_name          rbt.capability_qualifier
rbt.source_component         rbt.source_process_instance
rbt.source_activation        rbt.source_capability_instance
```

File metadata under `rbt.recording` separately records SDK, IDL language,
message descriptor, file encoding, control protocol, architecture,
endianness, and recording start versions/values. Exact-schema replay is the
v0.3 rule and remains current; schema evolution and migration are explicitly
deferred.

## `rbt-canonical-v1` bytes

The MCAP payload is not raw `repr(C)` memory. Every integer and IEEE-754 float
bit pattern is written little-endian in IDL declaration order, fixed arrays are
element order, and there are no compiler alignment bytes. This avoids leaking
uninitialized padding and decouples durable files from a compiler's local ABI.

A fixed message is the concatenation of its generated fields. A slice message
is:

```text
generated canonical header fields
u64 little-endian element_count
element[0] generated canonical fields
...
```

Thus `ControlCommand` is 16 bytes. Image has a 32-byte canonical header, an
8-byte count, then bytes. PointCloud has a 32-byte header, an 8-byte count,
then 16 bytes per `PointXYZI`. Decoding requires exact consumption and checked
arithmetic. The v0.3 limits remain in force: a message record is limited to
256 MiB, an Image payload to 128 MiB, PointCloud to 2,000,000 points, schema
text to 1 MiB, and channel metadata to 64 KiB.

The independent MCAP/file encoding version is `1`. It is not the IDL language
version, message ABI major, descriptor version, control protocol, or snapshot
version.

## Fixtures and verification

Small checked-in fixtures live in `tests/fixtures/mcap`; large fixtures are
generated under the verification log directory and never checked in:

```bash
./scripts/generate_mcap_fixtures.sh
./scripts/verify_mcap.sh --quick
./scripts/verify_mcap.sh --full
./scripts/benchmark_mcap.sh
```

The selected MCAP library contains process-randomized hash maps in some summary
groups, so the fixture contract is semantic determinism: exact schemas,
channels, file-order messages, log times, metadata values, and canonical
payload bytes. Single-channel fixtures happen to compare byte-for-byte; the
verifier does not overstate that as a mixed-file format guarantee.

`benchmark_mcap.sh` records source-paced 64-byte canonical Image,
6,220,800-byte Image, and 20 MiB PointCloud workloads and retains message and
byte throughput inputs, process user/system CPU, maximum RSS, queue high-water,
drop count, and final file size under `target/mcap-benchmark-logs`. These are
host-local smoke measurements, not cross-framework or hardware-independent
claims. Filesystem, page cache, storage, compiler profile, host scheduling, and
the required uncompressed encoding all affect the numbers.

rbt's live local IPC remains zero-copy. Recording and playback are file
serialization boundaries and therefore copy/encode data as required by durable
storage. For identified timestamps and clock mappings, see
[Time and clocks](time.md); the [TF guide](tf.md) describes the current
transform API and its limits.

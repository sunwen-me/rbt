# MCAP investigation and file-format decision

Date: 2026-09-13

This note records the investigation that precedes the rbt v0.3 MCAP
implementation. MCAP is a serialization-agnostic container at the recording
boundary; it is not a candidate transport for rbt's live local data plane.

## Maintained implementation

rbt uses the maintained Foxglove Rust `mcap` crate, pinned exactly to
`0.25.0` with default features disabled. The release declares Rust 1.81 as its
minimum version, below rbt's Rust 1.85 floor. Disabling default features leaves
the library's indexed/chunked, uncompressed writer and reader while avoiding
unneeded LZ4 and zstd dependencies. The v0.3 acceptance path uses compression
`none`.

Primary references:

- [MCAP format specification](https://mcap.dev/spec)
- [MCAP format registry](https://mcap.dev/spec/registry)
- [MCAP implementation notes](https://mcap.dev/spec/notes)
- [Foxglove MCAP repository](https://github.com/foxglove/mcap)
- [Rust `mcap` 0.25.0 API](https://docs.rs/mcap/0.25.0/mcap/)
- [Rust 0.25.0 release notes](https://github.com/foxglove/mcap/releases/tag/releases%2Frust%2Fv0.25.0)

The crate implements the binary format; rbt does not manually implement the
MCAP container.

## Container records and API consequences

- `Schema` gives a nonzero file-local ID, name, schema encoding, and encoded
  schema bytes. A schema must precede a channel that references it.
- `Channel` gives a file-local ID, schema ID, human-readable topic, message
  encoding, and string metadata. A channel must precede its messages.
- `Message` contains channel ID, a `u32` sequence, `log_time`, `publish_time`,
  and the channel-encoded bytes. `publish_time` must equal `log_time` when a
  source publish time is unavailable.
- `Metadata` is a named string map. rbt uses `rbt.recording` for portable file
  provenance and `rbt.recording.summary` for final completeness counters.
- `Chunk` batches schema, channel, and message records and may be compressed.
  Message indexes locate per-channel messages within a chunk. Chunk indexes in
  the summary locate chunks by log-time range and topic.
- The optional summary duplicates schemas/channels and contains statistics and
  indexes. Summary offsets make record groups directly addressable. rbt keeps
  the library's normal chunk, message-index, chunk-index, statistics, repeated
  schema/channel, and summary-offset behavior enabled.
- The writer must be explicitly finalized with `Writer::finish()`. Dropping the
  writer also attempts finalization but cannot surface an error, so rbt never
  relies on drop for a successful recording. Graceful shutdown drains the
  bounded queue, calls `finish`, flushes, and syncs the file.
- `MessageStream` validates the container, decompresses chunks when necessary,
  associates each message with its preceding schema/channel, and yields
  messages in file order. That order is the deterministic tie-breaker for
  equal `log_time` values.
- `Summary::read` supports fast inspection of finalized files. Inspection also
  performs a linear record/message pass so missing/inconsistent summaries and
  corrupt payloads are not mistaken for a healthy rbt recording.
- `read::Options::IgnoreEndMagic` can help tools scan some interrupted files,
  and `Writer::flush` closes the current chunk so a prefix is streamable. This
  is a library capability only: v0.3 makes no SIGKILL or WAL guarantee.

The MCAP timestamp is an unsigned nanosecond count. rbt records `log_time` as

```text
record_start_wall_time_ns + (monotonic_now - record_start_monotonic)
```

and sets `publish_time = log_time`. The original rbt header timestamp remains
inside the encoded message. Playback is scheduled from global MCAP `log_time`
deltas on a monotonic clock; payload timestamps are never used for scheduling.

## Raw shared-memory layout audit

The canonical IDL and generated compile-time assertions currently prove:

| Type | Size/alignment | Field coverage | Implicit padding |
| --- | --- | --- | --- |
| `ImageHeader` | 32 / 8 | 32 bytes | none |
| `PointCloudHeader` | 32 / 8 | 25 bytes plus explicit `reserved[7]` | none |
| `PointXYZI` | 16 / 4 | 16 bytes | none |
| `ControlCommand` | 16 / 8 | 16 bytes | none |

These facts are asserted in generated Rust and C++ code and are part of the
current layout signatures. Nevertheless, raw `repr(C)` bytes are rejected as
the MCAP payload representation. Raw bytes would couple durable files to the
local compiler ABI and would make a future padding-bearing layout easy to leak
accidentally. Explicit reserved fields are values and are preserved; compiler
padding is never read or written.

## Stable rbt mapping

The v0.3 convention is:

```text
Schema.name       = <namespace>/<message>@<abi-major>
Schema.encoding   = rbt-idl-v1
Schema.data       = deterministic canonical IDL dependency closure
Channel.topic     = logical capability topic, never a physical UUID
message_encoding  = rbt-canonical-v1
```

Channel metadata uses stable string keys:

```text
rbt.message_namespace
rbt.message_name
rbt.abi_major
rbt.layout_hash
rbt.stream_profile
rbt.capability_type
rbt.capability_name
rbt.capability_qualifier
rbt.source_component
rbt.source_process_instance
rbt.source_activation
rbt.source_capability_instance
```

The final four keys are historical provenance. Playback ignores them when
creating endpoints and always receives fresh process, activation, and
capability identities.

## `rbt-canonical-v1` payloads

Every integer is unsigned or signed little-endian with its IDL width. Floating
point values are encoded as their IEEE-754 bit pattern in little-endian order,
including NaN payloads. Struct fields appear in canonical IDL declaration
order with no alignment bytes. Fixed arrays contain their elements in order.
Primitive newtypes use the underlying primitive encoding.

Fixed messages are only their field encoding. Slice messages are:

```text
canonical header fields
u64 little-endian element_count
element[0] canonical fields
...
element[element_count-1] canonical fields
```

The decoder requires exact byte consumption, uses checked arithmetic, enforces
per-type payload limits before loan/allocation, validates Image dimensions,
encoding and stride, and validates PointCloud count and width/height semantics.
It decodes directly into the final publisher loan where the runtime API permits.

The schema text and codec implementations are emitted by `rbt-idl` from the
same `CanonicalSchema` that emits Rust, C++, Python, and descriptor metadata.
There is no handwritten Image or PointCloud MCAP schema table. File encoding
version 1 is independent of the IDL language version, message descriptor
version, ABI major, and Composer protocol version.

## Determinism

Input schema/channel ordering and every rbt map use stable source order or a
`BTreeMap`. Message ordering and timestamps are explicit. The selected `mcap`
writer stores its returned summary schemas/channels in hash maps, and their
duplicate ordering in the summary is therefore not promised byte-for-byte
stable by the library. rbt fixtures promise semantic determinism: identical
schemas, channels, messages, timestamps, counters, and payload checksums.
Fixture verification compares those semantics rather than silently replacing
checked-in files. No build path or live UUID is included in deterministic
fixture metadata.

## Runtime boundary decisions

- Recording resolves a fixed logical stream set from one Composer snapshot,
  validates and opens ordinary side subscribers, then captures with the native
  WaitSet. It never becomes a publisher-to-consumer hop. Existing subscribers
  remain usable during a Composer outage.
- Provider replacement is not followed in v0.3. The recording remains bound to
  the physical provider resolved at start and never silently attaches to a new
  identity. A replacement requires a new recording invocation.
- A capture handler performs one canonical encode into an owned buffer, drops
  the SHM borrow, then tries a byte-budgeted writer queue. Sensor queue overload
  increments an explicit recorder-drop counter; Control queue overload is a
  fatal recording error.
- Playback is a native rbt component/provider. It preflights every selected
  schema and layout before opening any publisher, groups compatible provenance
  segments by logical topic, creates fresh identities, and relies on ordinary
  resolver conflict behavior. `QueueFull`, `UnableToDeliver`, and
  `ReadinessNotify` are reported and consumed without automatic retry.

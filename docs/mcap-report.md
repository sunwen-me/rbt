# rbt v0.3 MCAP acceptance report

Date: 2026-09-14

This report records the architecture decision and acceptance evidence for native
MCAP record, inspect, and play support. The commands used to reproduce the
evidence are:

```bash
./scripts/generate_mcap_fixtures.sh
./scripts/verify_mcap.sh --quick
./scripts/verify_mcap.sh --full
./scripts/benchmark_mcap.sh
```

The retained full-gate evidence described below is under
`target/mcap-verify-logs/mcap-full-20260913T225026Z-12279`; the benchmark
evidence is under
`target/mcap-benchmark-logs/mcap-benchmark-20260913T182927Z-443440`.
The full gate completed with `RBT_MCAP_FULL_PASS`.

## Architecture and boundary

```text
              LIVE
Publisher ────────────→ Subscriber
     │
     └──→ Recorder → MCAP


MCAP → Player → loaned SHM → Subscriber
```

The recorder is an ordinary side subscriber selected from one immutable
Composer snapshot. It never enters the normal publisher-to-subscriber payload
path, never changes Sensor or Control QoS, and remains attached to the selected
physical stream if Composer temporarily disappears. Its writer handoff is a
bounded byte queue.

Playback is a native component. It validates the complete file before creating
publishers, obtains new runtime process, activation, capability and endpoint
identities, and decodes directly into final publisher loans. Recorded physical
identities are historical provenance only. A Composer restart during record or
play reconstructed the unchanged live registrations with `0 deactivate, 0
activate`; the 180-message outage recording completed with no gaps or drops.

rbt's live local IPC remains zero-copy. Recording and playback are file
serialization boundaries and therefore copy/encode data as required by durable
storage.

## Actual schema and channel mapping

The mixed canonical fixture supplies the following concrete mapping. Live
recordings use the same schema fields and metadata but derive the topic and
provenance values from the selected capability.

| Message | MCAP `Schema.name` | `Schema.encoding` | Example `Channel.topic` | `message_encoding` | ABI / layout hash |
| --- | --- | --- | --- | --- | --- |
| Image | `rbt.sensor/Image@1` | `rbt-idl-v1` | `/fixture/camera` | `rbt-canonical-v1` | `1` / `943fa4008cf0f55f89a281b051c69a6f` |
| PointCloud | `rbt.sensor/PointCloud@1` | `rbt-idl-v1` | `/fixture/lidar` | `rbt-canonical-v1` | `1` / `d9da2bb29bb2cf217c40f1a56d1fb755` |
| ControlCommand | `rbt.control/ControlCommand@1` | `rbt-idl-v1` | `/fixture/motion-command` | `rbt-canonical-v1` | `1` / `9381cf15842a48ac0964111f0f45ef72` |

Every channel contains these metadata keys:

| Key | Image fixture value | PointCloud fixture value | Control fixture value |
| --- | --- | --- | --- |
| `rbt.message_namespace` | `rbt.sensor` | `rbt.sensor` | `rbt.control` |
| `rbt.message_name` | `Image` | `PointCloud` | `ControlCommand` |
| `rbt.abi_major` | `1` | `1` | `1` |
| `rbt.layout_hash` | `943fa4008cf0f55f89a281b051c69a6f` | `d9da2bb29bb2cf217c40f1a56d1fb755` | `9381cf15842a48ac0964111f0f45ef72` |
| `rbt.stream_profile` | `sensor` | `sensor` | `control` |
| `rbt.capability_type` | `Camera` | `Lidar` | `MotionCommand` |
| `rbt.capability_name` | `fixture-camera` | `fixture-lidar` | `fixture-motion-command` |
| `rbt.capability_qualifier` | `fixture` | `fixture` | `fixture` |
| `rbt.source_component` | `fixture-generator` | `fixture-generator` | `fixture-generator` |
| `rbt.source_process_instance` | fixed fixture UUID | fixed fixture UUID | fixed fixture UUID |
| `rbt.source_activation` | `1` | `1` | `1` |
| `rbt.source_capability_instance` | fixed fixture UUID | fixed fixture UUID | fixed fixture UUID |

`Schema.data` is canonical `.rbt` source emitted from the same IDL model as
the Rust, C++ and Python bindings. There is no second handwritten MCAP schema.
File metadata separately records file encoding version `1`, IDL language
version, descriptor version, SDK version, control protocol, architecture and
endianness. Exact schema, ABI and layout hash matching is required; v0.3 does
not attempt implicit schema evolution.

## Canonical file encoding

Raw shared-memory representation was deliberately rejected. Although the
current generated assertions prove no implicit padding in the three supported
layouts, raw `repr(C)` would bind durable data to one compiler ABI and could
leak uninitialized padding after a future layout change.

`rbt-canonical-v1` serializes every integer and IEEE-754 floating-point bit
pattern little-endian, in IDL declaration order. Fixed arrays retain element
order. No compiler alignment bytes are read or written. Fixed messages are the
concatenation of their fields; slice messages are:

```text
canonical header fields
u64 little-endian element_count
element[0] canonical fields
...
```

Consequently, ControlCommand is 16 bytes; Image is a 32-byte header, an 8-byte
count and byte elements; PointCloud is a 32-byte header, an 8-byte count and
16 bytes per PointXYZI. Decoding uses checked arithmetic, per-record and
payload bounds, invariant validation and exact byte consumption. Corrupt
container data, unsupported encodings, oversized inputs and ABI/schema/hash
mismatches return typed errors instead of panicking.

MCAP `log_time` is the only replay schedule. Original rbt timestamps remain
unchanged payload data. All channels share one global monotonic schedule; file
order resolves equal `log_time` values.

## Fixture corpus

Small fixtures are checked in under `tests/fixtures/mcap` and are regenerated
and semantically compared by `./scripts/generate_mcap_fixtures.sh`.

| Fixture | Messages | Canonical payload bytes | Max elements | File bytes | SHA-256 |
| --- | ---: | ---: | ---: | ---: | --- |
| `control-small.mcap` | 3 | 48 | 0 | 2,726 | `c135c591aa99fe279f5b5b26bec6abe9e57545b8fcb2106e2be6f5efadba6320` |
| `image-small.mcap` | 2 | 272 | 96 | 3,825 | `9ca6b392cd4129630abab558493529301650ea3993aeeb6c53c057255c1d587b` |
| `pointcloud-small.mcap` | 3 | 200 | 4 | 3,819 | `38f71443c6378954c8248cccd8c26c7fe9eb606f404f3f12ea9f6355f4c9cec6` |
| `mixed-small.mcap` | 5 | 408 | 96 | 8,194 | `116a5f8c9dbd02d7807c086a9c9f916954585eb4297b857dddbd791c630fde6e` |

Large fixtures are generated, verified and retained under a verification run's
`large-fixtures/` directory, not checked in:

```bash
cargo run --offline -p rbt-mcap --bin rbt-mcap-fixtures -- \
  generate-large target/mcap-large-fixtures
```

| Generated fixture | Messages | Canonical payload bytes | Elements | File bytes | SHA-256 from retained run |
| --- | ---: | ---: | ---: | ---: | --- |
| `image-1920x1080-generated.mcap` | 1 | 6,220,840 | 6,220,800 bytes | 6,224,368 | `67c26e5206d4d789639077a30d29c6150164e90e5959b82d718d4c6540b8f468` |
| `pointcloud-100000-generated.mcap` | 1 | 1,600,040 | 100,000 points | 1,603,584 | `28b307a9b4a34d587573f1812c2351d3a4ac0905893304799db2f8408803d311` |
| `pointcloud-20mib-generated.mcap` | 1 | 20,971,560 | 1,310,720 points | 20,975,103 | `3273a56384c410c7de2fd59c6bd7c919c7924000f3e6208f4091eec16707b4e4` |

The format contract is semantic determinism: schema/channel content, message
order, timestamps, metadata and canonical payload bytes. The selected MCAP
library uses process-randomized maps for some duplicated summary groups, so a
mixed file is not claimed to be byte-identical across process hash seeds.
SHA-256 values identify this retained corpus and provide exact checks for the
single-channel cases.

## Cross-language round trips

| Source language | Message | Recorded count | Replay consumer | Exact value/checksum evidence | Result |
| --- | --- | ---: | --- | --- | --- |
| Rust | Image in mixed stream | 25 | C++ detector | 25/25 samples satisfied `value=(sequence % 256)*10` | PASS |
| C++ | Image | 8 | Rust detector | sequences 2–9; derived value `1950` observed | PASS |
| Python | PointCloud | 8 | C++ viewer | 8/8 four-point samples, checksum `99323559804928` | PASS |
| Rust | PointCloud in mixed stream | 25 | C++ viewer | 25/25 four-point samples, checksum `99323559804928` | PASS |
| Rust | ControlCommand in mixed stream | 24 | native Control consumer | sequences 2–25 and exact value/reserved fields | PASS |
| Rust | Image in mixed stream | 25 | Python component | payload pointer exposed; image checksum `4944`; derived point checksum `99323559804928` | PASS |

The mixed recording contained 74 messages and 6,384 canonical payload bytes:
25 Image, 25 PointCloud and 24 ControlCommand messages. Replay preserved global
file interleaving.
The Python retained-view test intentionally retained its first zero-copy NumPy
view; the next receive produced the pre-existing `BorrowLimitReached` behavior
at `max_borrowed=1`, confirming that Python ownership semantics were not
weakened to make replay pass.

## Timing and benchmark evidence

The timing fixture has a 100,000,000 ns recorded interval:

| Mode | Observed elapsed | Result |
| --- | ---: | --- |
| `--rate 1` | 100,493,251 ns | PASS |
| `--rate 2` | 50,378,671 ns | PASS |
| `--rate 0`, 74-message mixed live recording | 3,405,731 ns; approximately 21,728 messages/s | PASS |

Nonzero playback uses monotonic condition-variable deadlines and does not
busy-spin. These are integration tolerances, not hard-real-time guarantees:
host scheduling, startup, consumer readiness and timer granularity add jitter.

The independent source-paced benchmark recorded the following host-local
smoke results:

| Case | Messages / payload | Wall / CPU user+system | Max RSS | Queue high-water | Drops |
| --- | --- | --- | ---: | ---: | ---: |
| 64-byte canonical Image | 960 / 61,440 B | 2.02 s / 0.07+0.02 s | 12,868 KiB | 64 B | 0 |
| 6,220,840-byte canonical Image | 7 / 43,545,880 B | 1.58 s / 1.41+0.04 s | 30,960 KiB | 6,220,840 B | 0 |
| 20,971,560-byte canonical PointCloud | 4 / 83,886,240 B | 2.40 s / 0.87+0.08 s | 53,352 KiB | 20,971,560 B | 0 |

This benchmark is source-paced and uncompressed. Filesystem, page cache,
storage, compiler profile and host scheduling affect the result; it is neither
a cross-framework comparison nor a hardware-independent throughput claim.

## Pressure, bounds and failure semantics

| Case | Messages / bytes | Queue high-water / budget | RSS first → last (max) | FD | Threads | Drops / outcome |
| --- | --- | --- | --- | --- | --- | --- |
| ordinary mixed record | 74 / 6,384 | 256 B / 64 MiB | — | — | — | 0; complete |
| live 1920x1080 Image | 7 / 43,545,880 | 6,220,840 B / 64 MiB | 29,076 → 31,596 KiB (31,596) | 34→34 | 21→21 | 0; complete |
| live 20 MiB PointCloud | 4 / 83,886,240 | 20,971,560 B / 64 MiB | 13,348 → 54,332 KiB (54,332) | 34→33 | 21→21 | 0; complete |
| Sensor overload, 6.22 MiB sample / 1 MiB budget | 0 / 0 | 0 B / 1 MiB | — | — | — | 6 explicit recorder drops; incomplete |
| Control overload, 16-byte command / 8-byte budget | 0 accepted | bounded at 8 B | — | — | — | fatal nonzero error; incomplete |

The large PointCloud RSS increase is bounded by the retained large writer and
codec buffers rather than cumulative growth. The simultaneous fast Sensor peer
received five 6,220,800-byte frames with zero sequence gaps while the recorder
dropped six samples, showing that recorder overload did not backpressure or
disrupt the normal live path. `rbt.recording.summary.complete=false` is present
in both the Sensor and Control overload files. A `/dev/full` writer failure was
typed and nonzero while the unrelated source remained alive.

The player makes exactly one send attempt. Unit tests inject all relevant
post/partial-delivery failures and count the send closure calls:

| Player error | Attempts | Retry | Result |
| --- | ---: | --- | --- |
| Control `QueueFull` | 1 | none | PASS |
| Control `UnableToDeliver` | 1 | none | PASS |
| `ReadinessNotify(MissedDeadline)` | 1 | none | PASS |

No generic retry is safe here because payload delivery may already be partial;
retrying could duplicate a Control command or readiness notification.

## 300-second record soak and replay soak

| Metric | Result |
| --- | ---: |
| Recorded interval | 299,962,103,866 ns |
| Total messages | 17,925 |
| Image / PointCloud / ControlCommand | 5,975 / 5,975 / 5,975 |
| Canonical payload bytes | 1,529,600 |
| Recorder RSS first → last (max) | 14,508 → 14,748 KiB (14,748) |
| Recorder FD first → last | 60 → 60 |
| Recorder threads first → last | 21 → 21 |
| Source RSS first → last | 12,484 → 12,484 KiB |
| Source FD / threads | 49 → 49 / 20 → 20 |
| Transport gaps / recorder drops / writer errors | 0 / 0 / 0 |
| Writer queue high-water / budget | 256 B / 64 MiB |
| Final MCAP size | 2,380,311 B |

The replay soak launched and completed 100 player processes. Composer RSS was
7,796 → 7,796 KiB, FD count 10 → 10 and thread count 19 → 19, with no
remaining `rbt-player` capability after the last run. Neither soak showed
obvious cumulative resource growth.

## Compatibility and unsafe audit

| Existing contract | v0.3 result |
| --- | --- |
| Payload ABI | unchanged |
| Layout hashes | unchanged |
| IDL language | unchanged |
| Sensor QoS | unchanged |
| Control QoS | unchanged |
| WaitSet | unchanged |
| SidecarV1 | unchanged |
| Composer protocol | unchanged |
| snapshot | unchanged |
| Python ownership | unchanged |
| lifecycle authority | unchanged; Rust remains authoritative |

The additions are confined to MCAP file encoding, native record/info/play
tooling and optional IDL-derived file-codec generation. Inherited v0.1,
WaitSet, Python sync/async, publisher and component gates continue to exercise
the unchanged contracts. Fresh-install verification runs offline and builds
external Rust and C++ consumers without relying on the repository target
directory.

The production Rust unsafe audit uses the same matching method on the frozen
baseline and current tree:

| Audit | Matches |
| --- | ---: |
| Frozen baseline | 49 |
| Current production tree | 49 |
| Delta | 0 |
| `rbt-mcap` | 0 (`#![forbid(unsafe_code)]`) |

## Gate conclusion

Formatting, workspace tests, strict all-target/all-feature Clippy, deterministic
IDL/code generation, MCAP codec/container tests, fixture regeneration, corrupt
and unsupported-file tests, Debug/Release CMake, CTest, sanitizers, cross-language
record/play, timing, pressure, Composer recovery, 300-second recording, replay
resource soak, fresh installation and all inherited regression suites are
covered by `./scripts/verify_mcap.sh --full`.

The implementation satisfies the 39-item MCAP acceptance checklist without
changing live SHM into serialization, routing subscribers through the recorder,
silently dropping Control, retrying partial Control/readiness delivery, writing
raw padding, inventing another schema, using an unbounded queue, or accepting
arbitrary message sizes without bounds checks.

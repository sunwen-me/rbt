# Compatibility policy

This document describes the independent compatibility axes for RBT and the
current v0.8 local runtime. These values are policy identifiers for this
release; they are not a promise of stability for later 0.x releases. See the
[documentation index](README.md), [developer guide](development.md), and
the [TF guide](tf.md) for related current behavior.

## Independent version axes

The six version axes must be checked separately. A match on one axis never
makes a mismatch on another axis compatible.

| Identifier | v0.8 value | Scope |
| --- | ---: | --- |
| `SDK_VERSION` | `0.8.0` | Public Rust/C++/Python SDK and workspace release. It does not identify message layout or the Composer protocol. |
| `IDL_LANGUAGE_VERSION` | `1` | Accepted `.rbt` grammar and layout rules, initially introduced with the v0.1 generator. This is distinct from each message's `abi_major`. |
| `MESSAGE_DESCRIPTOR_VERSION` | `1` | Generated message-descriptor representation (the current descriptor JSON `version`). This is distinct from a message's `abi_major` and from `LayoutHash`. |
| `CONTROL_PROTOCOL_VERSION` | `2` | NDJSON composer control protocol (`rbt-compose::protocol::CONTROL_PROTOCOL_VERSION`; `PROTOCOL_VERSION` is a deprecated alias). It covers control messages and metadata, not shared-memory payloads. |
| `COMPOSER_SNAPSHOT_VERSION` | `4` | The current `ComponentSnapshot` shape and validation semantics. Version 4 carries low-rate `stream_pressures` and typed `rpc_pressures` with bounded-resource metadata; a snapshot carries its version independently from the control protocol. |
| `MCAP_FILE_ENCODING_VERSION` | `1` | Durable `rbt-canonical-v1` payload bytes in rbt MCAP files. It is independent of runtime SHM layout and every control/version axis above. |

Protocol and snapshot mismatches are rejected before they become trusted
composer state. A control-plane peer must use the exact
`CONTROL_PROTOCOL_VERSION`; an unsupported Hello or `inspect` request receives
a `protocol_mismatch` diagnostic and is not accepted for composition. Hello
and inspect messages explicitly carry both `control_protocol_version` and
`snapshot_version`. A snapshot that does not match the expected
`COMPOSER_SNAPSHOT_VERSION`, or fails snapshot validation, must be ignored or
rejected rather than imported or coerced.

## Readiness transport compatibility

### Maintained native baseline

The active native dependency is `iceoryx2 0.10.0-rbt.2`: RPC correlation/FFI
fixes and network fixes share one source fingerprint. Its Cargo upstream
package version remains pinned to `=0.10.0`; the `-rbt.2` maintenance suffix
describes the bundled maintained sources. Rust, C++, Python, installed SDKs,
and new generated projects must use that same source. The CLI reports this
native version independently of the six rbt compatibility axes above.

This is not native 0.9/0.10 shared-memory compatibility. Rebuild all processes
in a graph against the new SDK and stop/restart the old graph before switching.
Do not mix an old wheel, old C++ prefix, or unpatched registry iceoryx2 with the
new graph. SDK crate dependencies directly select the bundled maintained
source, including when consumed from an external workspace: a root Cargo patch
alone would not propagate to those consumers. Existing external Cargo projects
that also reference iceoryx2 directly must use `iceoryx2 = "=0.10.0"` and a
workspace `[patch.crates-io]` pointing to the SDK's
`third_party/iceoryx2-rbt/source/iceoryx2`. `rbt new` writes both a direct native
path for its types crate and the workspace patch automatically. Frozen layout
hashes for Image, PointCloud, ControlCommand, Frame, Detection, and StampedBytes
do not certify the middleware's internal ABI across native versions.

### SidecarV1

v0.2 added `StreamDescriptor.transport.readiness`, with values `none` and
`sidecar-v1`. This is deployment capability metadata, not payload ABI, and is
therefore excluded from `MessageTypeDescriptor`, canonical layout text, and
`LayoutHash`. Rust, C++, and Python event-driven consumers require SidecarV1
and reject another value before opening the payload or readiness transport.

This additive field did not change the control protocol or snapshot version
when introduced in v0.2. Existing Rust serde and C++ parsers accept a missing
field and interpret it as `none`; an old descriptor therefore fails new
WaitSet preflight explicitly instead of connecting and potentially sleeping
forever. The payload message descriptor format remains version 1 and control
protocol remains 2. Stream pressure first appeared in snapshot 3; current
snapshot 4 adds typed low-rate RPC pressure with bounded-resource metadata. It
does not change payload layout or either transport's data plane.

## Frozen layout semantics

The local message ABI keeps the existing frozen hash envelope. Its canonical
text remains:

```text
domain=rbt-shm-abi
version=1
endianness=little
shape=fixed|slice
namespace=<namespace>
name=<name>
abi_major=<decimal major>
header_size=<decimal bytes>
header_align=<decimal bytes>
element_size=<decimal bytes>
element_align=<decimal bytes>
signature=<canonical field types, offsets, and padding>
```

`LayoutHash` is the first 16 bytes of BLAKE3 over that canonical text.
Generator and SDK version numbers are not hash inputs and must never affect a
`LayoutHash`. The hash is derived from the canonical message identity and
layout semantics only. The six non-TF message hashes for Image, PointCloud,
ControlCommand, Frame, Detection, and StampedBytes remain unchanged by the
current SDK release. See the [ABI guide](abi.md) and [Time guide](time.md) for
their scope and hash details.

Layout is a hard compatibility boundary and must never change silently. Any
layout-affecting change—message shape, field types or order, sizes,
alignments, padding, namespace/name, ABI major, or canonical signature—must
produce a reviewed descriptor/hash outcome. Producer and consumer preflight
must reject an expected/actual descriptor mismatch before opening the local
endpoint; warning and continuing is not compatible behavior. The existing
Phase 3 envelope and hashes remain frozen.

## Local ABI boundary

The Rust/C++ shared-memory ABI is a local iceoryx2 data-plane contract. It
does not serialize message payloads and it is not a network wire format. Native
shared-memory structs, their `LayoutHash` values, and local control metadata
must not be treated as a portable network encoding. A future network gateway
needs a separate serialization and compatibility boundary.

MCAP is a distinct durable boundary. The current format retains generated
canonical `.rbt` schema text with
`Schema.encoding=rbt-idl-v1` and encodes field values with
`message_encoding=rbt-canonical-v1`, file encoding version 1. Playback requires
the exact generated namespace/name, ABI major, layout hash, schema text, and
supported file encoding. It never treats a compatible file encoding as proof
that a local SHM ABI or Composer protocol is compatible. Current playback does
not attempt schema migration. Recording and playback are stream-only; RPC
calls are not captured or replayed. Stream support does not establish that TF
MCAP recording or playback has been verified for every TF message or use.

This policy is limited to the current v0.8 behavior and supported local
profile. APIs in the 0.x series may evolve; this document does not define
future schema negotiation, network compatibility, cross-endian support,
cross-architecture support, or a stable ABI beyond v0.8.

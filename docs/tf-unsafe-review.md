# TF generated marker review — approved exception

## Decision: generated-marker exception approved

The user explicitly approved this proposal on 2026-10-18: permit the 24 new
generated declarations, preserve the original 49, and prohibit new hand-written
unsafe. Verification must report legacy 49 + TF generated 24 = total 73.

The v0.8 specification requires both canonical generated TF records and a total
production unsafe source-location count of 49. The current generator emits two
marker locations for each fixed record/message: `ShmSafe` and native
`ZeroCopySend` (including its unsafe `type_name` method on the same line).
Adding TF by that existing mechanism produces 24 new locations. The unchanged
audit counts generated files explicitly, so the observed total is **73**, not
49. Evidence: `target/tf-unsafe-audit.json`.

The approval is limited to these generated markers. No generated file may be
silently excluded from the audit. Product/SDK 0.8.0 has completed software
acceptance. This exception remains limited to 24 generated markers and adds
no handwritten unsafe code.

## Why the marker obligation exists

`FixedMessageAbi` requires `ShmSafe`, and slice messages require `ShmSafe`
headers/elements. Existing native Stream/RPC endpoints also require
iceoryx2's unsafe `ZeroCopySend` contract. A newly generated record does not
inherit either trait merely because all its fields are numeric. Rust requires
an implementation for the new type. The existing generator supplies those
implementations along with ABI descriptors and layout assertions.

Reusing the byte-slice marker instead would abandon the requested native typed
TF message shape and its generated descriptor. Hiding implementations inside
macros, excluding `tf.rs`, or removing unrelated old unsafe locations just to
preserve the numeric total would not establish the requested safety invariant.
None of those approaches has been used.

## Scope and safety evidence

All 24 new locations are in `generated/rust/tf.rs`; none is in the TF math,
buffer, configuration, transport adapter or Python binding implementation.
Each type is generated as `Copy` with `repr(C)`. Fields are fixed-width
integers, f64 values, fixed arrays, or other records of the same restricted
schema. There are no references, pointers, strings, vectors, destructors,
process handles, virtual tables or ownership transfers in these wire types.
Generated size/alignment/offset assertions and descriptor hashes freeze their
representations on the supported little-endian platform.

| Generated type | Size / alignment | Contents and obligation |
| --- | --- | --- |
| TfFrameId | 64 / 1 | Fixed u8[64], canonical name validation at decode |
| TfVec3 | 24 / 8 | Three f64 values, finite check at decode |
| TfQuaternion | 32 / 8 | Four f64 xyzw values, unit validation at decode |
| TfTransform3 | 56 / 8 | Translation and quaternion records |
| TfTimestamp | 80 / 8 | Numeric kind/flags/reserved/source/generation/time/uncertainty |
| StaticTransform | 184 / 8 | Two frame records and one rigid transform |
| StaticHeader | 112 / 8 | Authority bytes, revision/status/count and effective time |
| StaticUpdateHeader | 112 / 8 | Same fields, distinct C++ slice binding identity |
| DynamicTransform | 288 / 8 | Authority, frames, identified time, transform and epoch |
| StaticSnapshotRequest | 24 / 4 | Authority bytes, maximum count and reserved bytes |
| StaticSnapshot | 0 / 1 | Slice message marker; header/element own the payload ABI |
| StaticUpdate | 0 / 1 | Slice message marker; distinct descriptor identity |

Every primitive bit pattern is a valid Rust scalar value; NaN, invalid frame
names, zero quaternions, unknown flags and invalid clock identities can still
be semantically invalid. Safe wire decoders validate those conditions before
buffer insertion. Marker traits do not substitute for semantic validation.
Native sample loans/guards retain their existing ownership implementation;
the local buffer copies small validated values and releases received guards.

The large-array generator repair emits manual `Default` and per-element calls
to the existing canonical codec. It adds no unsafe implementation mechanism or
new serialization format. Regenerating the old schemas yields byte-identical
Rust/C++/Python/descriptor output. All 157 external lockfile package tuples
(name, version, source) also remain unchanged relative to the saved baseline.

## Approved exception and audit requirement

Retain and individually audit the original 49 locations,
permit exactly these 24 generated marker locations, and keep **zero new
hand-written unsafe**. Report both numbers and the total 73 in every gate,
rather than claiming “production unsafe remains 49”. The generated count must
be checked independently, alongside deterministic code generation and ABI tests.

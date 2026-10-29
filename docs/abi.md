# Local message ABI

The v0.8 rbt message ABI is a local shared-memory contract used by Rust, C++,
and the Python native extension. Native Stream and RPC endpoints exchange
these payloads without a serialization step. Durable MCAP encoding is a
separate contract; this layout is not a network wire format. See the
[documentation index](README.md), [IDL guide](idl.md), and
[compatibility axes](compatibility.md).

## Shapes

A fixed message is one `repr(C)`/standard-layout object such as `Frame` or
`ControlCommand`. A slice message has one fixed header plus a runtime-length
contiguous element region, such as `ImageHeader + u8[]` or
`PointCloudHeader + PointXYZI[]`.

The generated descriptor records:

- package and message name;
- message ABI major and fixed/slice shape;
- header size/alignment;
- element size/alignment for a slice;
- canonical field signature including explicit padding;
- 128-bit layout hash.

Rust generated types use `#[repr(C)]`; C++ generated types are
standard-layout/trivially-copyable and checked with `static_assert`. `ShmSafe`
implementations are unsafe contracts and generated implementations carry a
specific safety rationale. The [TF unsafe review](tf-unsafe-review.md) records
the audited generated TF additions and the current production count.

## Frozen hash envelope

The hash contract introduced in v0.1 remains unchanged: it is the first 16
bytes of BLAKE3 over this canonical text:

```text
domain=rbt-shm-abi
version=1
endianness=little
shape=fixed|slice
namespace=<namespace>
name=<name>
abi_major=<decimal>
header_size=<bytes>
header_align=<bytes>
element_size=<bytes>
element_align=<bytes>
signature=<canonical fields, offsets, padding>
```

SDK, generator, IDL-language, descriptor-format, control-protocol, and snapshot
versions are not hash inputs. Whitespace/comments in IDL do not change layout.
Field order/type, array length, shape, namespace/name, ABI major, alignment,
padding, or header/element layout do.

Layout mismatch is a hard error. Increment/review the message ABI major for an
intentionally incompatible schema; never force two layouts to share a hash.

## Zero-copy ownership

A publisher loans transport memory and initializes it in place. Sending
consumes the loan; dropping an unsent guard releases it. A subscriber receives
a borrowed guard. References or spans into the sample must not outlive that
guard. Slice payload length is validated against the declared element capacity
before access.

Rust exposes `LoanedSample`, `LoanedSliceSample`, `ReceivedSample`, and
`ReceivedSliceSample`. C++ exposes equivalent move-only RAII guards; spans must
stay within the guard lifetime. Python received NumPy views retain their native
sample owner, while writable publisher aliases must be released before send.
See [Python ownership](python.md) and [RPC guard ownership](rpc.md).

`StampedBytes@1` carries timing metadata and application bytes in one native
slice sample. TF's fixed and slice records are generated from
[`types/tf.rbt`](../types/tf.rbt); wire timestamp records decode through the
existing Time types. The six pre-TF message layout hashes remain frozen. The
[Time](time.md) and [TF](tf.md) guides describe semantic validation beyond
byte layout. Generated TF records do not establish generic MCAP support.

## Profiles

| Profile | Queue depth | Safe overflow | Saturation behavior |
| --- | ---: | --- | --- |
| Sensor | 1 | true | newest data progresses; sequence gaps are observable |
| Control | 4 | false | send reports `QueueFull`/delivery failure |

Descriptor validation occurs before `ServiceName`, node, or transport open.
The Phase 4 verifier proves cross-language layout equality, Rust→C++ Image,
C++→Rust PointCloud, preflight mismatch rejection, Control saturation, and
Sensor drop behavior.

The current contract is little-endian and local to the supported ABI target.
A future network gateway needs its own serialization and compatibility model.

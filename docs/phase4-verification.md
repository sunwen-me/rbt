# Phase 4 cross-language ABI verification

This report records the Phase 4 Rust/C++ shared-memory ABI evidence. The ABI
is intentionally local and bounded to Linux x86_64 little-endian, stable Rust,
C++20 GCC/Clang, and iceoryx2 `0.9.3`. The schemas in
[`types/sensor.rbt`](../types/sensor.rbt),
[`types/control.rbt`](../types/control.rbt), and
[`types/demo.rbt`](../types/demo.rbt) are the source of truth. This detailed
Phase 4 report is retained as historical evidence; `testing.md` and
`verify_v01.sh` define the current v0.1 release gate.

The current event-driven waiting milestone is documented separately in
[`waitset-report.md`](waitset-report.md). It adds readiness sidecars and
WaitSet coverage without changing the frozen payload ABI, QoS, or lifecycle
semantics.

## Source, generation, and target

The reproducible pipeline is:

```text
types/*.rbt
    │
    ▼  rbt-idl: parse → resolve names → natural C layout → canonical hash
generated/rust/*.rs ─── generated/cpp/rbt/*.hpp ─── generated/descriptors/*.json
        │                         │                          │
        └──── Rust/C++ data plane ┴── control-plane metadata ┘
```

The deterministic generator command is:

```bash
cargo run -p rbt-idl -- generate \
  types/sensor.rbt types/control.rbt types/demo.rbt \
  --rust generated/rust \
  --cpp generated/cpp \
  --descriptor generated/descriptors
```

Running the equivalent built binary twice into separate temporary directories
gave byte-identical trees. The v0.1 set adds a Rust module, C++ header, and JSON
descriptor for `demo` to these six original Phase 4 outputs:

```text
cpp/rbt/control.hpp
cpp/rbt/sensor.hpp
descriptors/control.json
descriptors/sensor.json
rust/control.rs
rust/sensor.rs
```

`write_if_changed` leaves already matching output untouched. The canonical
model excludes comments, insignificant whitespace, and input filenames.

The IDL accepts fixed-width primitives (`u8/u16/u32/u64`, `i8/i16/i32/i64`,
`f32/f64`), integer `primitive`/`newtype` declarations, one-dimensional fixed
arrays `T[N]`, non-empty fixed `struct` declarations, non-empty fixed
`message` declarations, and exactly one named fixed struct header plus one
homogeneous `Element[]` payload in a slice message. It rejects dynamic fields
outside the terminal payload, nested arrays, arrays as slice elements, a
second dynamic field, `usize`/`isize`/`bool`, pointers/references,
`String`/`std::string`, `Vec`/`std::vector`, `Box`, `Arc`, `Rc`,
`HashMap`/maps, optionals/variants/unions, ad-hoc enums, recursive/unknown/message
field types, empty declarations, zero-length arrays, and floating-point
newtype underlyings. `types/sensor.rbt` defines `PixelFormat`, `ImageHeader`/`Image`,
`PointCloudHeader`/`PointXYZI`/`PointCloud`; `types/control.rbt` defines the
fixed `ControlCommand` (`sequence:u64`, `value:f32`, `reserved:u32`).

The generated layout is natural C layout: declaration order, alignment before
each field, final size rounded to maximum alignment, and no packed attributes.
Rust emits `#[repr(C)]` and compile-time size/alignment/offset assertions;
C++ emits standard-layout/trivially-copyable assertions and `offsetof`
assertions. Primitive mapping is:

| RBT | C++ | Size/alignment |
| --- | --- | ---: |
| `u8`, `u16`, `u32`, `u64` | `std::uint8_t`, `std::uint16_t`, `std::uint32_t`, `std::uint64_t` | 1/1, 2/2, 4/4, 8/8 |
| `i8`, `i16`, `i32`, `i64` | `std::int8_t`, `std::int16_t`, `std::int32_t`, `std::int64_t` | 1/1, 2/2, 4/4, 8/8 |
| `f32`, `f64` | `float`, `double` | 4/4, 8/8 |

`PixelFormat` remains a `u32`-sized semantic newtype (`value`/`new()` APIs),
not an anonymous `u32`. The complete message hashes are the first 16 BLAKE3
bytes over the frozen Phase 3 envelope. That envelope is deliberately
unchanged so the Phase 3 hashes remain frozen; completeness is carried by the
canonical type identities and field offsets plus the envelope's total
header/element layouts. Descriptor JSON also records every field's size and
alignment. Consequently, a same-sized newtype versus primitive still has a
distinct semantic identity and hash.

## Exact layout and hash evidence

`target/phase4-layout-rust.txt` and `target/phase4-layout-cpp.txt` are byte
identical. The measured table is:

```bash
./target/debug/phase4-interop layout > /tmp/phase4-layout-rust-now.txt
./target/rbt-cpp-build/layout_cpp > /tmp/phase4-layout-cpp-now.txt
diff -u /tmp/phase4-layout-rust-now.txt /tmp/phase4-layout-cpp-now.txt
# exit status 0
```

| Object | Size | Align | Offsets | Owning message hash |
| --- | ---: | ---: | --- | --- |
| `PixelFormat` | 4 | 4 | `value=0` | — |
| `ImageHeader` | 32 | 8 | `sequence=0`, `timestamp_ns=8`, `width=16`, `height=20`, `stride=24`, `encoding=28` | `943fa4008cf0f55f89a281b051c69a6f` (`Image`) |
| `PointCloudHeader` | 32 | 8 | `sequence=0`, `timestamp_ns=8`, `width=16`, `height=20`, `dense=24`, `reserved=25` | `d9da2bb29bb2cf217c40f1a56d1fb755` (`PointCloud`) |
| `PointXYZI` | 16 | 4 | `x=0`, `y=4`, `z=8`, `intensity=12` | `d9da2bb29bb2cf217c40f1a56d1fb755` (`PointCloud`) |
| `ControlCommand` | 16 | 8 | `sequence=0`, `value=8`, `reserved=12` | `9381cf15842a48ac0964111f0f45ef72` (`ControlCommand`) |

The generated descriptor signatures are also retained in the JSON artifacts:

```text
Image: header(sequence:u64@0,timestamp_ns:u64@8,width:u32@16,height:u32@20,stride:u32@24,encoding:PixelFormat@28,padding:none);element:u8@0
PointCloud: header(sequence:u64@0,timestamp_ns:u64@8,width:u32@16,height:u32@20,dense:u8@24,reserved:[u8;7]@25,padding:none);element:PointXYZI(x:f32@0,y:f32@4,z:f32@8,intensity:f32@12)
ControlCommand: sequence:u64@0;value:f32@8;reserved:u32@12;padding:none
```

## C++ bootstrap, SDK, and quality checks

The native quality/verification setup script is
[`scripts/bootstrap_iceoryx2_cpp.sh`](../scripts/bootstrap_iceoryx2_cpp.sh);
run it before the CMake checks below:

```bash
./scripts/bootstrap_iceoryx2_cpp.sh
cmake -S sdk/cpp -B target/rbt-cpp-build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/target/iox2-cpp-install"
cmake --build target/rbt-cpp-build --parallel
ctest --test-dir target/rbt-cpp-build --output-on-failure
```

The bootstrap script pins tag `v0.9.3` and verifies commit
`6ef7c55176a8487ae7ca53c2e0649d5df9a1b3b4`. The SDK follows the actual C++
API in this release: `NodeBuilder` creates `ServiceType::Ipc`, a moved service
builder calls `publish_subscribe<T>()` and (for slices) `user_header<Header>()`,
publisher creation uses `open_or_create()`, subscribers use `open()`, dynamic
publishers set `initial_max_slice_len()` and call `loan_slice()`, and send
consumes the native sample with `iox2::send(std::move(sample))`. RAII wrappers
are move-only and retain Node, factory, endpoint, and Control's backpressure
handler in a destruction-safe order (endpoint, factory, handler, Node).
`Result<T>` is a move-only `std::variant<T, StreamError>`. Message-descriptor
preflight occurs before `ServiceName`, Node, or transport open; the SDK's
profile-aware overload can additionally compare the selected policy. Generated
`FixedMessageBinding<T>` and `SliceMessageBinding<Header, Element>`
specializations prevent template payload types from advertising a different
descriptor. Explicit moves clear source native handles, moved-from fallible
operations return `Transport`, and an unsent loan is safely released by RAII.

Observed quality results from the current checked C++ build:

```text
cmake --build target/rbt-cpp-build --parallel: ninja: no work to do.
ctest: 6/6 tests passed (descriptor_tests, stream_compile, pressure_tests, effect_scope_tests, waitset_tests, runtime_tests)
```

The current CTest suite has six cases. The v0.1 full verifier configures Debug
with `-fsanitize=address,undefined -fno-omit-frame-pointer` and runs the suite
with leak detection and UBSan stack traces enabled when the selected compiler
supports it.

The Rust quality gate is the following command set (the same categories are
included in the workspace gate):

```bash
cargo fmt --all -- --check
cargo test --workspace
cargo clippy --workspace --all-targets --all-features -- -D warnings
```

The final accepted verifier run is
`target/phase4-logs/rbt-phase4.dGmoxX`. It passed deterministic generation,
Rust/C++ builds, CTest, byte-identical layout output, all five process pairs,
formatting, the full workspace tests, and Clippy with warnings denied.

This historical Phase 4 run predates the WaitSet milestone and does not claim
its later evidence. The milestone was subsequently accepted by
`target/waitset-verify-logs/waitset-full-20260910T234120Z-98790`, with fresh
sanitizer evidence under
`target/v01-logs/verify-full-20260910T234126Z-99689/sanitizer-ctest.log`.

## Rust/C++ interop captures

All paths below are real captured files under
`target/phase4-logs/rbt-phase4.dGmoxX`. The service suffix is run-specific; it
is intentionally not a global topic.

### Image, 1080p slice

Files: [`image-publisher-rust.log`](../target/phase4-logs/rbt-phase4.dGmoxX/image-publisher-rust.log)
and [`image-subscriber-cpp.log`](../target/phase4-logs/rbt-phase4.dGmoxX/image-subscriber-cpp.log).

Commands (run with the same `SERVICE`, publisher and subscriber in separate
terminals):

```bash
./target/release/phase4-interop image-publisher "$SERVICE"
./target/phase4-cpp-build/interop_cpp image-subscriber "$SERVICE" 3 0
```

```text
IMAGE_PUBLISHER ready service=rbt/phase4/rbt-phase4.dGmoxX/image-1 payload=6220800 hash=943fa4008cf0f55f89a281b051c69a6f frames=3
IMAGE_PUBLISHER sequence=0 payload=6220800 checksum=793152000 hash=943fa4008cf0f55f89a281b051c69a6f recipients=1
IMAGE_PUBLISHER sequence=1 payload=6220800 checksum=793152000 hash=943fa4008cf0f55f89a281b051c69a6f recipients=1
IMAGE_PUBLISHER sequence=2 payload=6220800 checksum=793152000 hash=943fa4008cf0f55f89a281b051c69a6f recipients=1
IMAGE_CONSUMER bound message=rbt.sensor/Image@1 layout=943fa4008cf0f55f89a281b051c69a6f payload=6220800 service=rbt/phase4/rbt-phase4.dGmoxX/image-1
IMAGE_CONSUMER seq=0 timestamp_ns=0 encoding=1 payload=6220800 checksum=793152000 selected=0,0,255 pointer=0x74125e2c20d8 gap=0
IMAGE_CONSUMER seq=1 timestamp_ns=1000000 encoding=1 payload=6220800 checksum=793152000 selected=1,1,0 pointer=0x74125e2c20d8 gap=0
IMAGE_CONSUMER seq=2 timestamp_ns=2000000 encoding=1 payload=6220800 checksum=793152000 selected=2,2,1 pointer=0x74125e2c20d8 gap=0
IMAGE_CONSUMER complete service=rbt/phase4/rbt-phase4.dGmoxX/image-1 frames=3 gaps=0 estimated_dropped=0
```

Both sides validated the 1,920 × 1,080 × 3 payload, header, pattern, and
checksum while the consumer held its receive guard.

### PointCloud variable-size slice

Files: [`pointcloud-publisher-cpp.log`](../target/phase4-logs/rbt-phase4.dGmoxX/pointcloud-publisher-cpp.log)
and [`pointcloud-subscriber-rust.log`](../target/phase4-logs/rbt-phase4.dGmoxX/pointcloud-subscriber-rust.log).

Commands:

```bash
./target/release/phase4-interop pointcloud-subscriber "$SERVICE"
./target/phase4-cpp-build/interop_cpp pointcloud-publisher "$SERVICE"
```

```text
POINTCLOUD_SUBSCRIBER ready service=rbt/phase4/rbt-phase4.dGmoxX/pointcloud-1 counts=0,1,100,100000 hash=d9da2bb29bb2cf217c40f1a56d1fb755
POINTCLOUD_SUBSCRIBER sequence=0 elements=0 first=none last=none checksum=0 hash=d9da2bb29bb2cf217c40f1a56d1fb755
POINTCLOUD_SUBSCRIBER sequence=1 elements=1 first=(0,0,-0,0) last=(0,0,-0,0) checksum=66571993088 hash=d9da2bb29bb2cf217c40f1a56d1fb755
POINTCLOUD_SUBSCRIBER sequence=2 elements=100 first=(0,0,-0,0) last=(99,198,-99,6) checksum=3386543084142592 hash=d9da2bb29bb2cf217c40f1a56d1fb755
POINTCLOUD_SUBSCRIBER sequence=3 elements=100000 first=(0,0,-0,0) last=(99999,199998,-99999,24) checksum=3677129335508477952 hash=d9da2bb29bb2cf217c40f1a56d1fb755
POINTCLOUD_SUBSCRIBER complete service=rbt/phase4/rbt-phase4.dGmoxX/pointcloud-1 frames=4 hash=d9da2bb29bb2cf217c40f1a56d1fb755
POINTCLOUD_PUBLISHER elements=100000 checksum=3677129335508477952 first=0 last=99999 recipients=1 sequence=3 timestamp_ns=3000000
```

The four counts (`0`, `1`, `100`, `100000`) are valid payload lengths; `0` is
an empty slice, not a sentinel or a temporary vector.

### Control pressure

Files: [`control-subscriber-rust.log`](../target/phase4-logs/rbt-phase4.dGmoxX/control-subscriber-rust.log)
and [`control-publisher-cpp.log`](../target/phase4-logs/rbt-phase4.dGmoxX/control-publisher-cpp.log).

Commands:

```bash
./target/release/phase4-interop control-subscriber "$SERVICE" 1500
./target/phase4-cpp-build/interop_cpp control-publisher "$SERVICE"
```

```text
CONTROL_SUBSCRIBER ready service=rbt/phase4/rbt-phase4.dGmoxX/control-1 hold_millis=1500 queue_depth=4 hash=9381cf15842a48ac0964111f0f45ef72
[control-test] queue saturation sequence=4 error=QueueFull
[control-test] sent=5 delivered=4 profile=Control
```

The subscriber remained unread. Queue depth four delivered the first four
commands; the fifth send returned `QueueFull` and did not overwrite an old
command.

### Intentional preflight mismatch

Files: [`mismatch-provider-rust.log`](../target/phase4-logs/rbt-phase4.dGmoxX/mismatch-provider-rust.log)
and [`abi-mismatch-image-cpp.log`](../target/phase4-logs/rbt-phase4.dGmoxX/abi-mismatch-image-cpp.log).

Commands:

```bash
./target/release/phase4-interop image-provider-hold "$SERVICE" 1500
./target/phase4-cpp-build/abi_mismatch_cpp "$SERVICE" "$ACTUAL_PROVIDER_HASH"
```

The generated fixture `types/fixtures/image_mismatch.rbt` retains the provider's
namespace/name (`rbt.sensor/Image`) and ABI major `1`, but adds
`ImageHeader.exposure_us: u32`. Its header becomes 40 bytes and its complete
message hash changes. The C++ fixture binding consumes that generated
descriptor and rejects the original Rust provider metadata before
`ServiceName`, Node, or transport open:

```text
binding rejected: message=rbt.sensor/Image@1 expected_layout=f88f32069378e1234107664b4557e99f actual_layout=943fa4008cf0f55f89a281b051c69a6f expected_size=40 actual_size=32 expected_align=8 actual_align=8 error=AbiMismatch stage=preflight transport_opened=false
```

The provider side remained alive during the check:

```text
IMAGE_PROVIDER_HOLD ready service=rbt/phase4/rbt-phase4.dGmoxX/mismatch-1 millis=1500 payload=6220800 hash=943fa4008cf0f55f89a281b051c69a6f
IMAGE_PROVIDER_HOLD complete service=rbt/phase4/rbt-phase4.dGmoxX/mismatch-1 millis=1500
```

### Sensor pressure: 30 Hz producer, 5 Hz consumer

Files: [`image-publisher-smoke-rust.log`](../target/phase4-logs/rbt-phase4.dGmoxX/image-publisher-smoke-rust.log)
and [`image-subscriber-smoke-cpp.log`](../target/phase4-logs/rbt-phase4.dGmoxX/image-subscriber-smoke-cpp.log).

Commands:

```bash
./target/release/phase4-interop image-publisher-smoke "$SERVICE"
./target/phase4-cpp-build/interop_cpp image-subscriber "$SERVICE" 6 200
```

The Rust publisher used period `33333333 ns` (approximately 30 Hz). The C++
consumer processed one sample every `200 ms` (approximately 5 Hz), while
Sensor's one-slot safe-overflow policy replaced stale unread samples:

```text
IMAGE_PUBLISHER_SMOKE ready service=rbt/phase4/rbt-phase4.dGmoxX/smoke-1 payload=6220800 frames=60 period_ns=33333333 hash=943fa4008cf0f55f89a281b051c69a6f
IMAGE_PUBLISHER_SMOKE complete service=rbt/phase4/rbt-phase4.dGmoxX/smoke-1 sent=60 delivered=32 undelivered=28 payload=6220800 hash=943fa4008cf0f55f89a281b051c69a6f
IMAGE_CONSUMER seq=4 timestamp_ns=4000000 encoding=1 payload=6220800 checksum=793152000 selected=4,4,3 pointer=0x751823f360d8 gap=0
IMAGE_CONSUMER seq=10 timestamp_ns=10000000 encoding=1 payload=6220800 checksum=793152000 selected=10,10,9 pointer=0x751824b13978 gap=5
IMAGE_CONSUMER seq=29 timestamp_ns=29000000 encoding=1 payload=6220800 checksum=793152000 selected=29,29,28 pointer=0x751824b13978 gap=6
IMAGE_CONSUMER complete service=rbt/phase4/rbt-phase4.dGmoxX/smoke-1 frames=6 gaps=5 estimated_dropped=26
```

The gaps are an observable estimate, not a transport lost-sample counter;
iceoryx2 `0.9.3` does not expose a lost-sample flag for this profile.

## Regression categories

The Phase 4 gate retains the earlier categories:

| Phase | Regression category | Evidence/tests |
| --- | --- | --- |
| 1 | Identity, lifecycle transitions, typed requirements/bindings, serialization round trips | `rbt-core` unit tests |
| 2 | Activation ownership/LIFO teardown, critical-task failure, stale events, provider replacement, Composer restart/recovery, registry reconstruction and conflict handling | `rbt-compose` unit/integration tests |
| 3 | Fixed and slice loan/receive, 1080p image, zero/large PointCloud, Sensor replacement pressure, Control saturation, descriptor mismatch-before-open, max length, layouts, and receive-guard lifetime | `rbt-types`, `rbt-iox2`, `phase3_data_plane`, trybuild |
| 4 | IDL parsing/validation, natural layouts, newtype semantic hashing, frozen hash envelope, deterministic Rust/C++/JSON generation, C++ descriptor/preflight/Result/move tests, and Rust↔C++ image/PointCloud/Control/mismatch captures | `rbt-idl` tests, current six-test `sdk/cpp` CTest, `target/phase4-logs/rbt-phase4.dGmoxX` |

The v0.1 C++ build reports all SDK tests passing:

```text
1/6 Test #1: descriptor_tests ................. Passed
2/6 Test #2: stream_compile ................... Passed
3/6 Test #3: pressure_tests ................... Passed
4/6 Test #4: effect_scope_tests ............... Passed
5/6 Test #5: waitset_tests .................... Passed
6/6 Test #6: runtime_tests .................... Passed
100% tests passed, 0 tests failed out of 6
```

The accepted run passed the following Rust quality gate:

```bash
cargo fmt --all -- --check
cargo test --workspace
cargo clippy --workspace --all-targets --all-features -- -D warnings
```

## Non-guarantees and v0.2 questions

This verification does not establish cross-endian/architecture operation,
arbitrary compiler packing compatibility, a network wire format or automatic
schema evolution, GPU/DMA zero-copy, hard-real-time behavior, or stable ABI
outside the stated target. Python/NumPy borrowed views, MCAP recording, and a
network gateway remain intentionally unimplemented v0.2 work.

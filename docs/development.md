# Framework development — v0.8

This guide is for contributors changing RBT itself. Application authors should
start with the [quickstart](quickstart.md) and [component authoring](component-authoring.md).
The [documentation index](README.md) links all maintained guides and retained
milestone evidence.

This repository owns the framework runtime, Rust/C++/Python SDKs, IDL and
generated bindings, time and TF APIs, Composer, and the `rbt` CLI. Robot-specific
drivers, navigation, and perception are maintained in the separate robot
workspace, located at `../rbt_robot_ws/` beside this checkout in the local
development layout. Local compatibility links to that workspace are ignored
and are not part of framework source releases.

## Repository map

| Path | Responsibility |
| --- | --- |
| `crates/rbt` | Public Rust facade and typed public errors |
| `crates/rbt-core`, `crates/rbt-compose` | Graph/identity types, activation/task ownership, composition, control snapshots, and recovery |
| `crates/rbt-iox2` | Native Stream, RPC, WaitSet, loan/borrow ownership, and resource limits |
| `crates/rbt-types`, `crates/rbt-idl` | Message descriptors, built-in types, IDL compiler, and deterministic generation |
| `crates/rbt-time`, `crates/rbt-time-stream` | Identified timestamps, clock mapping, and native stamped payloads |
| `crates/rbt-tf`, `crates/rbt-tf-transport` | Local transform math/history and activation-owned native TF transport |
| `crates/rbt-cli`, `crates/rbt-mcap` | Project/diagnostic CLI and canonical durable recording/playback |
| `sdk/cpp` | Native C++20 SDK, CMake package, and tests |
| `crates/rbt-python`, `python/rbt`, `python/tests` | PyO3 extension, Python API/type stubs, and Python tests |
| `types`, `generated` | Canonical production schemas and generated Rust/C++/descriptor artifacts |
| `examples`, `tests` | Learning examples, external consumers, and regression fixtures |
| `scripts`, `packaging` | Build/provenance checks, installation, verification gates, and staged SDK metadata |
| `third_party/iceoryx2-rbt` | Maintained native patch series and immutable source fingerprints |
| `target` | Local builds, reconstructed dependencies, logs, and run evidence |

Read [architecture](architecture.md) and [compatibility](compatibility.md) before
changing a shared contract. Protocol definitions are in
[`crates/rbt-compose/src/protocol.rs`](../crates/rbt-compose/src/protocol.rs);
[`Cargo.toml`](../Cargo.toml), [`version.hpp`](../sdk/cpp/include/rbt/version.hpp),
and [`pyproject.toml`](../python/pyproject.toml) carry current product versions.

## Bootstrap and build

The supported first environment is Linux x86_64 with Rust 1.89+ and Git.
C++ development also needs CMake 3.22+, Ninja, and C++20. Python package metadata
requires Python 3.10+ and NumPy; the v0.8 release verification used CPython 3.12.
See the [Python guide](python.md) for build dependencies and ownership rules.

Reconstruct the pinned native source before compiling a fresh checkout:

```bash
./scripts/bootstrap_iceoryx2_rbt.sh --source-only
cargo build --locked -p rbt-cli -p rbt-idl -p composer
export PATH="$PWD/target/debug:$PATH"
export RBT_RUST_SDK="$PWD"
rbt doctor
rbt version --verbose
```

All language bindings use **iceoryx2 0.10.0-rbt.2**. The native Cargo package
version remains `0.10.0`; its maintained source and provenance identify the
patches. The [native guide](../third_party/iceoryx2-rbt/README.md) explains source
reconstruction, fingerprint checks, and deliberate native changes.

For C++, provision the repository's Phase 4 toolchain at
`target/phase4-toolchain`, or point `RBT_PHASE4_TOOLCHAIN` to an equivalent
provisioned directory containing `bin/x86_64-conda-linux-gnu-{gcc,g++,ar}`.
The bootstrap uses those tools and installs a stamped native C++ prefix:

```bash
./scripts/bootstrap_iceoryx2_cpp.sh
./scripts/check_iceoryx2_rbt_prefix.sh "$PWD/target/iox2-rbt-0.10.0-cpp-install"
export RBT_PHASE4_TOOLCHAIN="${RBT_PHASE4_TOOLCHAIN:-$PWD/target/phase4-toolchain}"
export CC="$RBT_PHASE4_TOOLCHAIN/bin/x86_64-conda-linux-gnu-gcc"
export CXX="$RBT_PHASE4_TOOLCHAIN/bin/x86_64-conda-linux-gnu-g++"
export AR="$RBT_PHASE4_TOOLCHAIN/bin/x86_64-conda-linux-gnu-ar"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="$CC"
export LD_LIBRARY_PATH="$RBT_PHASE4_TOOLCHAIN/lib:$PWD/target/iox2-rbt-0.10.0-cpp-install/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LIBRARY_PATH="$RBT_PHASE4_TOOLCHAIN/lib:$PWD/target/iox2-rbt-0.10.0-cpp-install/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"
cmake -S sdk/cpp -B target/dev-cpp -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_PREFIX_PATH="$PWD/target/iox2-rbt-0.10.0-cpp-install"
cmake --build target/dev-cpp --parallel
ctest --test-dir target/dev-cpp --output-on-failure
```

`RBT_IOX2_PREFIX` selects another maintained native prefix for the scripts; use
the same prefix in CMake. A stock or old native installation cannot replace the
provenance-checked build. Avoid reusing CMake caches from a different native
source. See [C++ SDK](cpp-sdk.md) for installed consumers.

The installer retains its historical filename and stages the current release:

```bash
./scripts/install_v01.sh --prefix "$PWD/target/dev-install"
```

It builds the tools, installs the C++ package, and copies the Rust SDK and docs.
`--python-wheel PATH` can include a built wheel. Its Cargo install runs offline,
so provision the locked dependencies first. Use [installation testing](testing.md)
when changing packaging or an external-consumer contract.

For the local SDK, source archive, and Python wheel release set, see the
[distribution guide](distribution.md). Its release packager expects a
provisioned native prefix, Phase 4 toolchain, and packaging tools; it does not
publish the generated artifacts.

## Schemas and generated code

Edit canonical schemas in `types`, then regenerate all production outputs
together from the repository root:

```bash
cargo run --locked -p rbt-idl -- generate \
  types/sensor.rbt types/control.rbt types/demo.rbt types/tf.rbt \
  --rust generated/rust \
  --cpp generated/cpp \
  --descriptor generated/descriptors \
  --python python/rbt/_generated
```

These four production files are distinct from negative fixtures in
`types/fixtures`. Built-in `StampedBytes` is implemented by `rbt-time-stream`;
there is no production `time.rbt` or `rpc.rbt` input. Generated output should be
reviewed with the schema, rather than maintained independently. The
[IDL](idl.md) and [ABI](abi.md) guides define layout and hash rules.

The existing Image, PointCloud, ControlCommand, Frame, Detection, and
StampedBytes layout hashes stay frozen. Current axes are SDK `0.8.0`, IDL `1`,
descriptor `1`, Composer protocol `2`, snapshot `4`, and MCAP file encoding `1`
(`rbt-canonical-v1`). A public API addition alone does not bump these axes.

## Shared contracts to preserve

Activation owns endpoints, resolved bindings, tasks, and teardown. Keep
process-lifetime preparation separate. Use existing `ActivationContext` and
`EffectScope` helpers so cancellation joins activation work before its resources
are released. Healthy Composer recovery retains active physical identities.
See [component authoring](component-authoring.md).

Native loans and received guards bound shared-memory ownership. Python NumPy
views retain or borrow those guards; writable aliases must disappear before
send. Data notification uses the existing event-driven WaitSet. Changes to a
language wrapper must preserve the same ownership and cancellation semantics
as the native API. See [Python SDK](python.md), [waiting](waiting.md), and
[pressure](pressure.md).

Stream and RPC payloads move directly between component processes. Composer
resolves capabilities and carries low-rate diagnostics, never the payload.
RPC request/response correlation, deadlines, and cancellation remain native
endpoint responsibilities. See [RPC](rpc.md).

Measurement time is an identified `Timestamp`; comparisons across clock
identities require a valid explicit `ClockMapping`. Runtime deadlines retain
real monotonic time. TF consumes those existing time contracts: static state
uses snapshot RPC plus update Stream, dynamic samples use Stream, and lookups
run in each consumer's bounded `TransformBuffer`. Missing history, conflicting
authority, and invalid mappings fail explicitly. See [Time](time.md) and [TF](tf.md).

The approved production unsafe boundary is 49 legacy locations plus exactly
24 generated TF declarations, 73 total. The [TF unsafe review](tf-unsafe-review.md)
records the rationale and scope; this does not authorize additional handwritten
unsafe or a broader generator exception.

## Choose verification for the change

Start with the affected package or behavior, then expand when the change crosses
a shared contract. Use the [testing guide](testing.md) for environment setup,
gate inheritance, logs, and complete acceptance criteria.

| Change | First useful check |
| --- | --- |
| Documentation | Commands/API against source, local links/anchors, whitespace, and any changed generation example |
| IDL/compiler or descriptors | `cargo test --locked -p rbt-idl -p rbt-types`, regenerate and compare artifacts |
| Rust activation/composition | `cargo test --locked -p rbt-core -p rbt-compose` plus the affected lifecycle scenario |
| Native Stream/RPC/WaitSet | Affected `rbt-iox2` tests and the focused RPC/WaitSet/pressure gate |
| Time | `cargo test --locked -p rbt-time -p rbt-time-stream`, then the relevant language/interop check |
| TF math/history or transport | `cargo test --locked -p rbt-tf -p rbt-tf-transport`, then the relevant language/interop check |
| C++ | Build affected targets and run the matching CTest filter in the configured build |
| Python | Run the affected `python/tests` with a freshly rebuilt extension or installed wheel |
| CLI/project tooling | `cargo test --locked -p rbt-cli`; use the DX or first-ten-minutes gate for scaffolding changes |
| Installation/public SDK | `./scripts/test_install.sh`; TF packaging also has `./scripts/test_tf_packages.sh` |

Formatting and strict Rust checks are:

```bash
cargo fmt --all -- --check
cargo clippy --workspace --all-targets --all-features --locked -- -D warnings
```

For v0.8 integrated software acceptance, the aggregate entrypoint is
`./scripts/verify_tf.sh --quick` or `--full`. It inherits Time, which inherits
RPC; full mode includes the older lifecycle, WaitSet, pressure, Python, MCAP,
DX, and installation lanes. Run one aggregate full gate for the release
candidate; choose focused checks during iteration and after a concrete repair.
The detailed scripts remain useful independently, and their historical names
do not indicate the SDK version they currently build.

Physical-device checks are separate and require an explicitly selected device
and, for TF, supplied extrinsics. The [camera example](../examples/10-tf-camera/README.md)
describes that path. Current TF software acceptance does not establish physical
calibration accuracy, physical dynamic-TF acceptance, or generic TF MCAP support.

## Documentation changes

Keep the [index](README.md), relevant feature guide, SDK guide, and command
reference aligned with the public behavior. Verify command spelling in
`rbt help <command>` or the standalone tool's usage; check API names against
Rust exports, C++ headers, and Python `.pyi` files. Check local Markdown targets
and anchors, and regenerate into a temporary directory when a generation
example changes. These checks are sufficient for prose-only changes; rerun a
runtime gate when executable behavior changes or a specific concern needs it.

Treat reports, audits, benchmark samples, and investigation notes as retained
evidence. Keep the recorded versions, failures, and NOT RUN results intact;
link a new report for new results. Preserve independent version axes and label
feature introduction history clearly. The [index](README.md#release-evidence-and-retained-history)
separates current guidance from these records.

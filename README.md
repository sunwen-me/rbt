# rbt

`rbt` is a zero-copy, capability-oriented robot runtime built on iceoryx2.
The Composer decides what should run; typed payloads move directly between
component processes through shared memory. The Composer is never in the
payload path.

This repository is the installable v0.8 real-hardware runtime and SDK for Rust, C++20,
and CPython/NumPy publishers and consumers. It includes lifecycle runtimes,
generated message bindings, runtime diagnostics, external-consumer installation
tests, mixed-language fault tests, soak tests, and repeatable verification and
benchmark harnesses.

This repository owns the framework core: the runtime crates, Rust/C++/Python
SDKs, IDL and generated bindings, time and TF contracts, Composer, and the `rbt`
CLI. See [project status](PROJECT_STATUS.md) for the current repository scope.
Robot-specific drivers, navigation, and perception modules live in a separate
robot workspace; in the local development layout it is the sibling checkout
`../rbt_robot_ws/`.

For a new application, start with the [quickstart](docs/quickstart.md):
`rbt doctor` → `rbt new` → `rbt check` → `rbt run`. The generated project is
the supported first path; the mixed Rust/C++ commands below are an advanced
reference and regression workflow.

Contributors preparing local SDK, source, and Python wheel bundles can follow
the [distribution guide](docs/distribution.md).

The local robot workspace contains the Orbbec Gemini 335 and M3T integration
guides under `../rbt_robot_ws/drivers/orbbec/` and
`../rbt_robot_ws/perception/m3t/`.

## Why use rbt on top of iceoryx2?

[iceoryx2](https://github.com/eclipse-iceoryx/iceoryx2) already provides the
native zero-copy data plane: publish/subscribe, request/response and events,
loans, WaitSets, discovery, resource cleanup, diagnostics, and language APIs.
RBT uses those primitives and adds shared application contracts around them.

| iceoryx2 provides | RBT adds |
|---|---|
| Native shared-memory streams, loans, events, and RPC | One IDL with generated bindings and cross-language descriptor/ABI checks ([IDL](docs/idl.md), [ABI](docs/abi.md)) |
| Transport queues, delivery outcomes, and resource limits | Sensor and Control profiles with typed pressure and saturation errors ([pressure semantics](docs/runtime-pressure.md)) |
| Native service discovery and lifecycle resource handling | Composer binds typed capability requirements; each activation's `EffectScope` joins tasks and cleans up its endpoints and capabilities ([concepts](docs/concepts.md)) |
| Transported application payloads | Identified timestamps, explicit clock mappings, and local TF history and lookup ([Time](docs/time.md), [TF](docs/tf.md)) |
| Native inspection and runtime primitives | Application graph and `rbt why` views, plus MCAP for Image, PointCloud, and ControlCommand; TF recording/playback remains unverified ([CLI](docs/cli.md), [MCAP](docs/mcap.md)) |

Payloads stay on the native shared-memory path; Composer handles graph and
lifecycle control and never forwards samples. The maintained native baseline,
iceoryx2 `0.10.0-rbt.2`, is reconstructed from six [tracked patches](third_party/iceoryx2-rbt/README.md).
Those lower-level fixes can also be used directly without RBT's upper layers.

RBT adds schema checks, diagnostics, an optional control plane, and maintenance
cost. There is no matched benchmark showing RBT is faster than calling native
iceoryx2 directly. A small application with fixed processes may prefer direct
native APIs; a growing mixed robot system can benefit from common message,
lifecycle, time, and TF contracts. Current adoption in the sibling robot
workspace is mixed: base, IMU, and navigation mostly use standalone RBT SDK
endpoints and the time, TF, or cleanup facilities they need, rather than
Composer-managed graphs.

## RBT and ROS 2 transport measurements

On 2026-10-29, a matched two-process experiment compared RBT SDK 0.8.0 with
ROS 2 Lyrical / Fast DDS 3.6.2 on an Intel Core Ultra 5 125H Linux x86_64
host. Each case used identical synthetic fixed-layout messages, payload fill
and sparse validation, best-effort depth 1, three repetitions, a 3-second
warmup and a 15-second measurement window. Values below are medians across
repetitions: **combined publisher + subscriber CPU% / combined PSS MiB**.
100% CPU means one logical core.

| Workload | RBT native loan | ROS 2 SHM transport, copy | ROS 2 loan + Data Sharing |
|---|---:|---:|---:|
| IMU-sized: 256 B, 200 Hz | 3.00% / 7.01 | 1.60% / 35.28 | 1.40% / 35.27 |
| Point-cloud-sized: 1.60 MB, 10 Hz | 0.27% / 8.44 | 1.20% / 96.46 | 0.60% / 39.88 |
| Raw video-sized: 1080p RGB8, 30 Hz | 2.73% / 12.94 | 9.80% / 286.16 | 2.73% / 53.21 |

All 27 trials reached the target rate with 100% delivery and no detected
payload/sequence errors. RBT and ROS loan trials each verified eight sampled
shared-buffer aliases; ROS loan counts matched every measured sample. RBT
used less PSS in these configurations. CPU has no universal winner: video
CPU was similar for both loan paths, and IMU CPU varied substantially
(RBT 0.87–3.07%). Buffer sizing and node/runtime overhead affect these results.

These are custom fixed-size messages, not standard `sensor_msgs` or a sensor
driver/whole-robot benchmark. The standard Image, PointCloud2 and Imu loan
capability probes returned false on this ROS installation. This is not a
QCS8550 measurement. See the [full report, ranges and per-trial data](benchmarks/ros2_transport/results/2026-10-29/README.md)
and [reproduction instructions](benchmarks/ros2_transport/README.md).

**RBT's practical advantage for native sensor IPC:** its existing
[`Image` and `PointCloud` schemas](types/sensor.rbt) support contiguous,
variable-length payloads within a configured capacity, with direct
[loan/receive access](docs/cpp-sdk.md#data-plane-ownership). Applications can
use these native sensor contracts on the iceoryx2 shared-memory path, while
standard `sensor_msgs` lack native loan support in the tested ROS 2/Fast DDS
configuration. RBT's schemas have their own layout; driver or format
conversion may still copy data. The performance table uses matched custom
messages on both stacks; standard-message CPU/memory was not measured.

## Install and run the first graph

The Rust-only first path needs Rust 1.89+ and a Linux x86_64 host. From a source
checkout, build only the three tools used by the generated graph, select this
checkout as the SDK, and let the doctor verify the environment:

```bash
./scripts/bootstrap_iceoryx2_rbt.sh --source-only
cargo build --locked -p rbt-cli -p rbt-idl -p composer
export PATH="$PWD/target/debug:$PATH"
export RBT_RUST_SDK="$PWD"
rbt doctor
```

An installed release provides the same three executables and its installed
Rust SDK, so no environment variable is needed. C++, Python, and NumPy are not
prerequisites for this first graph; the full mixed-language installation is
covered below and in [installation testing](docs/testing.md).

Create and run a real project outside this checkout:

```bash
mkdir -p /tmp/rbt-work
cd /tmp/rbt-work
rbt new hello-robot
cd hello-robot
rbt check
rbt run
```

The generated source and sink become `ACTIVE` and print `HELLO` samples. In a
second terminal, use `rbt ps`, `rbt graph`, and `rbt why hello-sink`; press
Ctrl-C in the launcher terminal to stop and reap the graph. See the
[quickstart](docs/quickstart.md) for message/component creation, provider
replacement, stream pressure, and MCAP record/replay. The
[project-layout guide](docs/project-layout.md) and
[component-authoring guide](docs/component-authoring.md) are the next normal
steps.

When developing directly from a checkout instead of an installed prefix, keep
`RBT_RUST_SDK` set until after `rbt new` so generated manifests record the
explicit SDK location.

For a fixed local request/response capability, use the
[Rust RPC onboarding example](docs/rpc.md).  It demonstrates activation-owned
server/client endpoints, loaned request/response payloads, bounded deadlines,
and cancellation without putting Composer in the payload path.

## Real-hardware timestamps

Use [the time guide](docs/time.md) for identified host/device timestamps,
checked arithmetic, device reset generations and explicit calibrated mappings.
`rbt time status --json` reads local clock diagnostics without Composer.
`rbt::time_stream::StampedBytes`, C++ `rbt/time_stream.hpp` and Python
`rbt.time_stream` carry capture and receive metadata in the same native sample.
Unknown capture time stays unknown; legacy Image/PointCloud timestamps retain
their existing ABI and have no implicit clock-domain guarantee.

The [physical V4L2 camera example](examples/09-time-camera/README.md) requires an
explicit device. Run `scripts/verify_time.sh --quick` or `--full`; hardware
acceptance is a separate `--hardware --device /dev/video0` run. Evidence is
recorded in [the time report](docs/time-report.md).

## Spatial transforms

The [TF guide](docs/tf.md) describes the accepted v0.8 software release:
validated 3D transforms, explicit frame direction, bounded local history,
capture-time lookups, static snapshot recovery, and Rust/C++/Python APIs.
Software verification is recorded in the [TF acceptance report](docs/tf-report.md);
physical-camera TF hardware acceptance remains NOT RUN.

Run the local configuration example with
`cargo run -p rbt-tf-local -- --config docs/tf-config-example.toml`.
The [camera/TF example](examples/10-tf-camera/README.md) requires an explicitly
selected device and supplied extrinsics; configured values do not establish
physical calibration accuracy. TF queries stay inside each consumer process.

## Advanced mixed-language reference

The remaining repository examples and release gates also use CMake 3.22+, a
C++20 compiler, Git, Ninja, and our unified iceoryx2 0.10.0-rbt.2 C++ binding.

### 1. Build the mixed fixtures

```bash
./scripts/bootstrap_iceoryx2_cpp.sh
export RBT_PHASE4_TOOLCHAIN="${RBT_PHASE4_TOOLCHAIN:-$PWD/target/phase4-toolchain}"
export CXX="$RBT_PHASE4_TOOLCHAIN/bin/x86_64-conda-linux-gnu-g++"
export LD_LIBRARY_PATH="$RBT_PHASE4_TOOLCHAIN/lib:$PWD/target/iox2-rbt-0.10.0-cpp-install/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cargo build -p camera-a -p viewer
cmake -S sdk/cpp -B target/v01-cpp-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_PREFIX_PATH="$PWD/target/iox2-rbt-0.10.0-cpp-install"
cmake --build target/v01-cpp-debug --target detector_component
```

The unified native bootstrap uses `target/phase4-toolchain`; select another
provisioned toolchain directory with `RBT_PHASE4_TOOLCHAIN`.

To build and install the Python data SDK:

```bash
python -m build --wheel python
python -m venv /tmp/rbt-python
/tmp/rbt-python/bin/pip install python/dist/rbt-0.8.0-*.whl
```

```python
import rbt

sub = rbt.ImageSubscriber.open(
    rbt.StreamDescriptor.image("rbt/camera/image")
)
sample = sub.receive()
image = sample.numpy()
print(image.shape)
```

`image` is a borrowed, read-only NumPy view. It owns the received shared-memory
sample through its base object and does not copy the payload.

Python publishers use an explicit loan/mapping/send sequence. Keep the
publisher and loan alive, delete every writable NumPy alias, then call
`loan.send()`; an alias that survives the send attempt is rejected rather than
silently copied.

### 2. Start a mixed Rust/C++ graph

Run each command in its own terminal from the repository root:

```bash
target/debug/composer
target/debug/camera-a
target/v01-cpp-debug/detector_component
target/debug/viewer
```

The live graph is:

```text
Rust Camera -- Camera/Image --> C++ Detector -- Detections/Detection --> Rust Viewer
```

Set `RBT_COMPOSER_ADDR=127.0.0.1:7555` consistently in all terminals to use a
different control address.

### 3. Inspect it

```bash
rbt ps
rbt graph
rbt capability list
```

`rbt ps` reports component, process, activation, lifecycle state, and control
connection. `rbt graph` shows the two resolved edges. Copy a capability UUID or
endpoint from `rbt capability list`, then inspect its ABI and queue policy:

```bash
rbt stream info <capability-id-or-service>
```

Machine-readable views are available through `rbt ps --json` and
`rbt capability list --json`; graph output supports `--dot`.

### 4. Remove and replace the provider

Terminate the Rust camera process and ask Composer why the detector stopped:

```bash
kill -KILL <camera-a-pid>
rbt why detector
```

The detector becomes `BLOCKED` because `Camera` has no active provider, and
the viewer becomes `BLOCKED` transitively. Start the replacement fixture:

```bash
cargo run -p camera-b
rbt ps
rbt graph
```

The existing C++ detector process activates against the new capability. Its
process identity and prepared model remain unchanged; its `ActivationId` and
activation-owned output `CapabilityInstanceId` are new.

Restart Composer itself while the graph is active:

```bash
composer_pid="$(pgrep -n -x composer)"
kill -KILL "${composer_pid}"
target/debug/composer
rbt ps
rbt graph
```

The component processes and shared-memory endpoints stay alive during the
outage. After reconnect and snapshot reconstruction, the new Composer reaches
the same graph with `0 deactivate, 0 activate`; process, activation, and
capability identities are unchanged.

The complete automated version, including a Composer restart and a critical
task failure, is:

```bash
./scripts/test_mixed_runtime.sh
```

## Frozen runtime semantics inherited from v0.1

The v0.1 runtime freezes these architectural boundaries:

1. Composer failure does not imply provider failure.
2. A capability exists only while its owning activation is valid.
3. A critical-task failure invalidates its activation and withdraws its
   endpoints and capabilities; a background-task failure does not.
4. Healthy Composer recovery imports complete snapshots and reconstructs the
   existing graph instead of recreating it.
5. Payloads never pass through Composer.
6. The local shared-memory message ABI is not a network wire format.

Preparation is process-lifetime work. Activation owns resolved bindings,
streams, capabilities, tasks, and cleanup. Rust and C++ implement the same
contract. Python delegates lifecycle and task ownership to that runtime. On a healthy Composer restart, processes keep moving data, reconnect,
and reconcile with `0 deactivate, 0 activate`.

The independent version axes for the v0.8 release are:

| Axis | Value |
| --- | ---: |
| SDK | `0.8.0` |
| IDL language | `1` |
| Message descriptor format | `1` |
| Control protocol | `2` |
| Composer snapshot | `4` |
| MCAP/file encoding | `1` (`rbt-canonical-v1`) |

Control and snapshot mismatches are rejected exactly. Generator/SDK versions
are not inputs to the frozen message layout hash.

## Public SDKs

Rust applications normally depend on `crates/rbt` and use its curated
`component`, `control`, `stream`, `types`, `rpc`, `time`, `time_stream`, and `tf`
modules. Its prelude is
intentionally small, and public boundaries use typed errors instead of
exposing internal `anyhow` errors.

C++ applications include headers below `include/rbt`, implement
`rbt::Component`, and run it with `rbt::ComponentRuntime`. Activation tasks are
owned by `EffectScope`, use `std::jthread`/`std::stop_token`, and are joined
during LIFO teardown. Installed consumers use:

```cmake
find_package(rbt CONFIG REQUIRED)
target_link_libraries(my_node PRIVATE rbt::runtime rbt::iox2)
```

Run `./scripts/test_install.sh` to install into a temporary prefix, verify the
installed docs/licenses, create and run a generated Rust graph with the staged
`rbt`/`rbt-idl`/Composer, and build fresh Rust and C++ consumers outside the
repository. The generated project and Rust consumer resolve only the staged
public SDK; the C++ consumer resolves the installed CMake package.

Python consumers use `rbt.ImageSubscriber` and `rbt.PointCloudSubscriber`, and
producers use `rbt.ImagePublisher` and `rbt.PointCloudPublisher`. Received
NumPy views retain the native receive guard and remain read-only; publisher
views are writable, non-owning, and require all aliases to be dropped before
send. Both directions use the existing event-driven WaitSet while releasing
the GIL. Python can also subclass `rbt.Component`; both its synchronous and
asyncio adapters delegate lifecycle, identity, cancellation, and composition
to the existing Rust runtime. The Python package also exposes typed RPC,
identified time, stamped payloads, and local TF through `rbt.tf`.

## MCAP record, inspect, and play

Record a fixed set of supported active capabilities as a bounded side
subscriber, inspect the file without Composer, and replay it through fresh
native Component identities:

```bash
rbt record --output run.mcap --capability Camera
rbt record --output mixed.mcap --all --duration 10
rbt mcap info mixed.mcap --json
rbt play mixed.mcap --rate 1.0
rbt play mixed.mcap --rate 2.0
rbt play mixed.mcap --rate 0
```

Image, PointCloud, and ControlCommand use IDL-generated canonical durable
bytes, not raw `repr(C)` memory. MCAP `log_time` drives one global monotonic
replay schedule; message timestamps stay unchanged inside the payload. Sensor
recorder overflow is visible and marks the file incomplete, while Control
loss and non-transactional replay send errors are terminal and never retried.
See [MCAP recording and playback](docs/mcap.md).

## Examples

```text
examples/
├── rust/
│   ├── camera/          Rust provider (`camera-a` package)
│   ├── detector/        Rust detector alternative
│   ├── viewer/          Rust consumer
│   └── composer/        Composer binary package
├── cpp/
│   ├── camera/          C++ Camera component
│   ├── detector/        C++ detector used by the mixed test
│   ├── viewer/          C++ viewer component
│   └── python-viewer/   C++ consumer for Python-provided PointCloud
├── python/
│   ├── component_detector.py Rust Camera -> Python -> C++ example
│   ├── image_stats.py   borrowed Image checksum example
│   └── pointcloud_stats.py structured PointCloud centroid example
├── interop/
│   ├── rust-image-cpp-consumer/
│   ├── cpp-pointcloud-rust-consumer/
│   └── phase4-runtime/  shared executable harness
└── advanced/            replacement, MCAP source, and low-level transport fixtures
```

The C++ examples also contain standalone `CMakeLists.txt` files for use against
an installed package. Low-level Phase 4 fixtures remain available under
`advanced`/`interop` but no longer dominate the public example layout.

## Build, generate, verify

Use the [framework development guide](docs/development.md) for repository
ownership, native bootstrap, language builds, generated artifacts, and choosing
checks for a change. Cargo and CMake build the implementation; the `rbt` CLI
scaffolds projects, delegates their build/run, and inspects runtime state.

Regenerate the four production schemas together:

```bash
cargo run --locked -p rbt-idl -- generate \
  types/sensor.rbt types/control.rbt types/demo.rbt types/tf.rbt \
  --rust generated/rust \
  --cpp generated/cpp \
  --descriptor generated/descriptors \
  --python python/rbt/_generated
```

For v0.8 integrated software verification, choose the required mode:

```bash
./scripts/verify_tf.sh --quick
# Release acceptance, including inherited full gates:
./scripts/verify_tf.sh --full
```

The aggregate TF gate inherits Time, then RPC. Full mode includes the existing
lifecycle, WaitSet, pressure, Python, MCAP, DX, installation, interop, and soak
lanes. During development, use the affected package or subsystem checks in
[testing](docs/testing.md); a documentation change needs command, API, and link
checks. The SDK registers 12 CTest cases, with four focused TF cases.
Physical-device gates are separate and require explicit device selection.

## Documentation

The **[developer documentation index](docs/README.md)** is the complete entrypoint
for current v0.8 guides and retained release evidence.

| Start here | Purpose |
| --- | --- |
| [Quickstart](docs/quickstart.md) | Generate, check, and run the first application |
| [Framework development](docs/development.md) | Repository map, builds, code generation, and focused verification |
| [Architecture](docs/architecture.md) and [concepts](docs/concepts.md) | Planes, lifecycle, identity, ownership, and recovery |
| [Component authoring](docs/component-authoring.md) and [project layout](docs/project-layout.md) | Requirements, bindings, tasks, cancellation, and project files |
| [Rust](docs/rust-sdk.md), [C++](docs/cpp-sdk.md), [Python](docs/python.md) | Language APIs and native ownership |
| [RPC](docs/rpc.md), [Time](docs/time.md), [TF](docs/tf.md) | Typed requests, capture-time semantics, and local transform queries |
| [IDL](docs/idl.md) and [ABI](docs/abi.md) | Canonical schemas, generated bindings, and frozen layouts |
| [CLI](docs/cli.md) and [debugging](docs/debugging.md) | Graph, stream, RPC, time, and TF diagnostics |
| [Examples](examples/README.md) and [testing](docs/testing.md) | Runnable learning path and verification scope |
| [Compatibility](docs/compatibility.md) | Product, protocol, file-format, and native version axes |

The index separates maintained guidance from historical investigations and
reports. The [v0.8 TF report](docs/tf-report.md) is the current software acceptance
record; earlier reports retain their original versions and test results.

The project is licensed under MIT or Apache-2.0. Third-party provenance is
listed in `THIRD_PARTY_NOTICES.md`; that file is engineering notice material,
not legal advice.

## Scope after v0.8

Simulator time, Parameters, a network gateway or cross-host TF, ROS 2 bridging,
calibration solvers, a global TF query service, schema evolution/migration, and
adaptive allocator redesign remain outside this release. Explicit bounded
loan/borrow tuning is covered by the [pressure guide](docs/pressure.md).
Benchmark results require the recorded method and environment metadata; see
[benchmark methodology](docs/benchmarks.md).

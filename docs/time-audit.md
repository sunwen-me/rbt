# v0.7 clock audit (before implementation)

Audited 2026-10-16. This document records the pre-v0.7 behavior. Runtime
deadlines and the legacy message contracts must retain that behavior.

## Actual baseline

The checkout starts at SDK 0.6.0. Production Cargo and C++ use the maintained
iceoryx2 **0.10.0-rbt.2**, upstream base
`135d09dd8b29f321f1725920d434864c4e512378`, with the unified patch source and
fingerprint checks in `scripts/bootstrap_iceoryx2_rbt.sh` and
`scripts/check_iceoryx2_rbt_prefix.sh`. `docs/rpc-report.md` describes historical
0.9.3 acceptance and explicitly points to `docs/iceoryx2-unified-report.md` for
the current production baseline. Its historical dependency row is not current
dependency wiring. No native dependency update is required for v0.7.

IDL language 1; descriptor 1; Composer protocol 2; snapshot 4;
MCAP `rbt-canonical-v1`; production unsafe source locations **49**, including
generated Rust. The count is not restricted to handwritten code.

Frozen layout hashes:

| Type | Hash |
| --- | --- |
| Image | `943fa4008cf0f55f89a281b051c69a6f` |
| PointCloud | `d9da2bb29bb2cf217c40f1a56d1fb755` |
| ControlCommand | `9381cf15842a48ac0964111f0f45ef72` |
| Frame | `5d2c102a797d5b0ab64b4db8844297c0` |
| Detection | `d8e60d2ca65e6b5b74f0981d34884a4d` |

There is no Git commit in this checkout and all source files are untracked.
The modification baseline is retained in `target/time-baseline/source.tar`;
final review must compare actual files against that archive as well as inspect
Git status. Nothing in the existing checkout may be reset or removed.

## Clock contracts by use

“Cross-process” below distinguishes transported durations/numbers from a
comparable absolute clock reading. A transported integer alone establishes no
clock domain.

| Source / locations | Clock domain | Unit | Monotonic? | Serialized? | Cross-process? | Can jump? | Consumer assumptions |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `types/sensor.rbt:11-32`; `generated/rust/sensor.rs`; generated C++/Python sensor bindings | **UNKNOWN** for Image/PointCloud `timestamp_ns` | ns by field name | UNKNOWN | SHM and canonical MCAP payload | Numeric value only | UNKNOWN | Caller-written u64; no source, epoch, generation or capture/receive guarantee |
| `types/demo.rbt`; generated demo bindings | Frame and Detection have **no timestamp** | n/a | n/a | sequence/value only | n/a | n/a | Do not invent a Frame timestamp contract |
| `crates/rbt-iox2/src/rpc.rs:1347-1375,2001-2015`; C++ `rpc.hpp:1624-1671` | Rust Instant / C++ steady_clock, runtime-local | Duration / ns / relative timeout | yes | Absolute deadline no | Duration/config only | Not wall-clock steps | Deadline arbitration, cancellation and expiry stay independent of sensor time |
| `crates/rbt-iox2/src/wait.rs:714,766,875-892`; C++ `wait.hpp` | Instant / native monotonic waits | Duration/ns | yes | no | Wake events, not instant origin | Not wall-clock steps | Relative WaitSet timeout and cancellation |
| Composer/component runner and connection health (`crates/rbt-compose`, `sdk/cpp/src/runtime.cpp:1991-2018`) | Tokio/Instant / steady_clock | relative Duration | yes | Not absolute instant | Liveness protocol carries observations | Not wall-clock steps | Shutdown, retry, heartbeat and health loops retain real steady time |
| `crates/rbt-iox2/src/pressure.rs:58-70,308-324,343-396,507-515`; `rpc_pressure.rs:154-161`; C++ `stream.hpp:86-95,286-297,525-542`, `rpc.hpp:950-987,1864-1950` | Local Instant / steady_clock elapsed | ns | yes | Aggregated durations/ages yes | Durations only | Not wall-clock steps | Wrapper-lifetime metrics, saturating accumulators; no cross-process Instant |
| `pressure.rs:73-78,503`; `rpc_pressure.rs:50-57,214,365`; C++ `stream.hpp:97-101`, `rpc.hpp:218-222` | SystemTime / system_clock, Unix epoch | ms | no | JSON/control snapshots | yes, wall-clock observation | yes | Display freshness only, not scheduling or sensor acquisition |
| `crates/rbt-core/src/lib.rs:163-165,328-330`; Composer freshness `composer.rs:373-402` | Unix wall-clock snapshot | ms | no | serde/control | yes | yes | `now.saturating_sub(snapshot)`; future observations get age zero; no synchronization proof |
| Python `StreamPressure` aliases (`python/rbt/__init__.py:708-718,734-742,781-806`) | Current native Unix ms; compatibility alias scales to ns | ms/native, ns/alias | no | returned diagnostic dictionaries | yes | yes | Ambiguous old alias names do not change the underlying wall-clock contract |
| `python/rbt/__init__.py:1245-1264,1308-1318`; native `checked_timeout` in `crates/rbt-python/src/lib.rs:1837-1844` | time.monotonic / native Instant | Python seconds at timeout boundary, native Duration | yes | no | Relative timeout only | Not wall-clock steps | Python waits do not use robot data timestamps |
| `crates/rbt-python/src/async_runtime.rs:625-650,750-786`; `_debug_async_wait_stats` | SystemTime Unix epoch | ns | no | Private debug stats | No public robot timestamp contract | yes | Readiness diagnostics only |
| Recorder `crates/rbt-cli/src/recording.rs:115-131,227-246,283-297,369-372` | One Unix wall anchor + Instant elapsed | ns | Increases with steady elapsed after anchor | MCAP log_time | File timeline only | Anchor can be wrong; subsequent wall steps not followed | Receipt/record time, **not hardware acquisition time** |
| `crates/rbt-mcap/src/container.rs:233-260` | Recorder timeline | ns | Per input recording | MCAP log_time and publish_time | File timeline | As supplied | publish_time = log_time; neither reads capture timestamp |
| `crates/rbt-mcap/src/codec.rs:133-215` | Preserves legacy raw timestamp | ns field | UNKNOWN | canonical payload | Numeric value only | UNKNOWN | Validates shape, not time source or synchronization |
| `crates/rbt-cli/src/playback.rs:462-495` | MCAP log_time differences scheduled by Instant | ns/Duration | Scheduler steady | Input file yes; scheduler instant no | File timeline | Reject/order behavior is file-specific | Replay pacing is not a robot clock authority |
| Rust/C++ camera, Phase4 and interop fixtures | Synthetic sequence-derived counters (`sequence * 1_000_000`) | ns-shaped integers | Test sequence only | SHM/fixtures | Test expectations only | Fixture-defined | They are not physical cameras or evidence of acquisition time |
| `examples/advanced/image-publisher/src/main.rs`; Rust image benchmarks | SystemTime Unix ns, generated pixels; Instant pacing | ns | Wall no; pacing yes | Image numeric field | Numeric value only | yes | Specific publisher convention does not define all Image timestamps |
| Scripts, tests and experiments | Real timeout clocks, wall logging, synthetic payload constants, benchmark elapsed | Explicit at call site | Depends on source | Logs/fixtures | No independent synchronization guarantee | Depends on source | Must not promote a fixture's clock contract into a production message guarantee |

## Sensor and integration conclusions

Legacy Image and PointCloud have 32-byte headers with timestamp at offset 8.
Their IDL, generated canonical codecs, Python loan defaults (zero) and test
fixtures establish neither Unix epoch nor physical capture semantics. Their
guarantee is **UNKNOWN**. Zero is not evidence of a known Unix timestamp.
Capture time, software receipt time and publication time cannot be recovered
from this field without an independently documented source contract.

Existing camera demos synthesize data (`examples/README.md` says no hardware).
No existing maintained physical camera/IMU acquisition interface was found.
Inventory found Integrated RGB Camera nodes `/dev/video0`–`/dev/video3`; the
user explicitly selected `/dev/video0`. Node presence is not hardware PASS.
V4L2 timestamp type and event-source flags must be examined: EOF driver time
does not establish exposure-start capture time. Unknown flags remain unknown.

A new safe `SliceMessageAbi` marker can reuse existing primitive header and
element traits without adding unsafe. A distinct versioned message identity
and layout signature are required for an in-sample timing prefix. It must
never be sent with the legacy Image identity or interpreted as legacy pixels.
Timing and payload must share one native loan/borrow; an unrelated sidecar is
not an acceptable per-sample association. C++ and Python require actual new
endpoint bindings, not merely a new descriptor value.

Host robot timestamps require explicit Linux CLOCK_MONOTONIC, boot identity,
time namespace and its offset basis. Rust Instant is a local runtime handle,
not a serializable robot timestamp. Linux time namespaces virtualize monotonic
clocks; an identical clock name across machines/namespaces proves nothing.
See [Linux time namespaces](https://man7.org/linux/man-pages/man7/time_namespaces.7.html)
and [V4L2 buffer flags](https://www.kernel.org/doc/html/latest/userspace-api/media/v4l/buffer.html).

## Scope freeze

No changes to runtime deadlines, lifecycle, pressure semantics, loan/borrow
ownership, Sidecar, WaitSet, Composer protocols, MCAP encoding, native PAL/CAL
or frozen message layouts. No simulation clock, TF, Parameters, synchronization
algorithm or daemon. Wall-clock freshness limitations above are documented,
not silently redesigned in this milestone.

## Pre-change search inventory

Files matched by the clock/timestamp/deadline inventory (including test and
benchmark consumers). Detailed contracts are grouped in the table above.

```text
crates/rbt-bench/src/lib.rs
crates/rbt-bench/src/main.rs
crates/rbt-cli/src/doctor.rs
crates/rbt-cli/src/lib.rs
crates/rbt-cli/src/playback.rs
crates/rbt-cli/src/project.rs
crates/rbt-cli/src/recording.rs
crates/rbt-cli/src/runner.rs
crates/rbt-cli/templates/source-main.rs
crates/rbt-compose/src/component.rs
crates/rbt-compose/src/composer.rs
crates/rbt-core/src/lib.rs
crates/rbt-idl/src/main.rs
crates/rbt-iox2/src/bin/mixed_pressure.rs
crates/rbt-iox2/src/pressure.rs
crates/rbt-iox2/src/rpc.rs
crates/rbt-iox2/src/rpc_pressure.rs
crates/rbt-iox2/src/wait.rs
crates/rbt-iox2/tests/node_cleanup.rs
crates/rbt-iox2/tests/phase3_data_plane.rs
crates/rbt-iox2/tests/pressure_fault.rs
crates/rbt-iox2/tests/pressure_semantics.rs
crates/rbt-iox2/tests/rpc_dynamic.rs
crates/rbt-iox2/tests/rpc_fault.rs
crates/rbt-iox2/tests/rpc_fixed.rs
crates/rbt-iox2/tests/rpc_native_identity.rs
crates/rbt-iox2/tests/rpc_resources.rs
crates/rbt-iox2/tests/rpc_stress.rs
crates/rbt-iox2/tests/waitset.rs
crates/rbt-iox2/tests/waitset_fairness.rs
crates/rbt-iox2/tests/waitset_lifecycle.rs
crates/rbt-mcap/src/bin/rbt-mcap-fixtures.rs
crates/rbt-mcap/src/codec.rs
crates/rbt-mcap/src/container.rs
crates/rbt-mcap/src/queue.rs
crates/rbt-python/src/async_runtime.rs
crates/rbt-python/src/bin/image_fixture.rs
crates/rbt-python/src/bin/readiness_deadline_fixture.rs
crates/rbt-python/src/lib.rs
crates/rbt-python/src/rpc.rs
crates/rbt-rpc-bench/src/lib.rs
crates/rbt-rpc-bench/src/main.rs
crates/rbt-rpc-interop/src/main.rs
examples/08-rpc/src/main.rs
examples/advanced/camera-replacement/src/main.rs
examples/advanced/image-consumer/src/main.rs
examples/advanced/image-publisher/src/main.rs
examples/advanced/mcap-source/src/main.rs
examples/cpp/camera/main.cpp
examples/cpp/detector/main.cpp
examples/cpp/python-viewer/main.cpp
examples/cpp/viewer/main.cpp
examples/interop/phase4-runtime/src/main.rs
examples/python/async_component_detector.py
examples/python/component_detector.py
examples/rust/camera/src/main.rs
examples/rust/detector/src/main.rs
examples/rust/viewer/src/main.rs
experiments/cross-host-upstream/src/lib.rs
experiments/cross-host/src/lib.rs
experiments/iceoryx2-rbt/src/lifecycle.rs
experiments/iceoryx2-rbt/src/sdk_transport.rs
experiments/iceoryx2-rbt/src/transport.rs
generated/cpp/rbt/sensor.hpp
generated/mismatch/cpp/rbt/sensor.hpp
generated/mismatch/rust/sensor.rs
generated/rust/sensor.rs
python/rbt/__init__.py
python/tests/async_component_runtime_probe.py
python/tests/component_runtime_probe.py
python/tests/rpc_interop_probe.py
python/tests/rpc_soak_probe.py
python/tests/test_component_api.py
python/tests/test_publisher_interop.py
python/tests/test_publisher_ownership.py
python/tests/test_publishers.py
python/tests/test_resources.py
python/tests/test_rpc_async.py
python/tests/test_rpc_dynamic.py
python/tests/test_rpc_interop_idle.py
python/tests/test_rpc_native.py
python/tests/test_signals.py
python/tests/test_validation.py
python/tests/wheel_rpc_probe.py
scripts/benchmark_pressure.sh
scripts/benchmark_python_publisher.py
scripts/benchmark_rpc.sh
scripts/benchmark_v01.sh
scripts/benchmark_waitset.sh
scripts/check_rpc_interop_idle.py
scripts/soak_pressure.sh
scripts/soak_rpc.sh
scripts/soak_v01.sh
scripts/soak_waitset_idle.sh
scripts/test_mixed_runtime.sh
scripts/test_python_asyncio_runtime.sh
scripts/test_python_component_runtime.sh
scripts/test_rpc_interop.sh
scripts/verify_cross_host_upstream.sh
scripts/verify_python.sh
scripts/verify_rpc.sh
sdk/cpp/include/rbt/rpc.hpp
sdk/cpp/include/rbt/stream.hpp
sdk/cpp/include/rbt/types.hpp
sdk/cpp/src/abi_mismatch_cpp.cpp
sdk/cpp/src/interop_cpp.cpp
sdk/cpp/src/layout_cpp.cpp
sdk/cpp/src/runtime.cpp
sdk/cpp/tests/rpc_tests.cpp
sdk/cpp/tools/rpc_interop.cpp
tests/consumers/cpp/main.cpp
types/fixtures/image_mismatch.rbt
types/sensor.rbt
```

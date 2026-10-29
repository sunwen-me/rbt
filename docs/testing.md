# Verification and testing

The current v0.8.0 software acceptance entry point is the TF verifier. Its
quick mode covers TF, Time, and RPC; full mode also runs inherited release
coverage:

```bash
./scripts/verify_tf.sh --quick
./scripts/verify_tf.sh --full
```

Quick mode checks the product version, TF Rust/C++/Python/CLI behavior,
cross-language math fixtures, native-prefix provenance, and unsafe/hash
audits, then runs `scripts/verify_time.sh --quick`; the Time gate runs
`scripts/verify_rpc.sh --quick`. Full mode runs TF's full interop,
external-package, stress, benchmark, and soak coverage together with the full
Time/RPC composition, including the inherited Phase 4, runtime, install, DX,
v0.1, pressure, WaitSet, MCAP, Python, and asyncio gates. A successful
aggregate run is recorded beneath `target/tf-verify-logs`.

`verify_time.sh` and `verify_rpc.sh` also provide focused `--quick` and `--full`
modes for work scoped to those subsystems. The TF quick gate already runs Time
quick, which runs RPC quick; the TF full gate composes their full coverage.
Rerunning those nested full gates separately is not needed to reproduce the
aggregate result. The scripts use the unified iceoryx2
`0.10.0-rbt.2` baseline for Rust, C++, and Python. They expect the pinned
`target/phase4-toolchain`, its fingerprint-verified C++ package, and the
provisioned CPython 3.12 verification environment by default; set
`RBT_PHASE4_TOOLCHAIN`, `RBT_IOX2_PREFIX`, or the verifier Python overrides to
use other locations.

Physical TF hardware acceptance remains **NOT RUN**. The software gate does
not select a camera, and generic TF MCAP recording/replay remains unverified.

## Developer-experience gate

The retained developer-experience gate is:

```bash
./scripts/verify_dx.sh --quick
./scripts/verify_dx.sh --full
./scripts/verify_first_ten_minutes.sh
```

The quick gate builds the CLI and exercises a clean project under a temporary
path containing spaces: doctor, project/component/message creation, IDL
check/format/generation, project build, `rbt run`, structured inspection, and
bounded Ctrl-C cleanup. The full report is
[`dx-report.md`](dx-report.md); it records the JSON contract and links the
inherited Rust/C++/Python/asyncio/WaitSet/pressure/MCAP/packaging/sanitizer
evidence. This gate defaults Cargo and pip to offline mode. Its full mode can
include inherited coverage; use the v0.8.0 aggregate entry point above for
current release acceptance.

The historical v0.1 regression gate remains available under its original
filename:

```bash
./scripts/verify_v01.sh --quick
./scripts/verify_v01.sh --full
```

The v0.1 gate preserves the frozen runtime baseline and historical reports; it
is one of the inherited checks in the current full gate.

The retained Python data gate is:

```bash
./scripts/verify_python.sh --quick
./scripts/verify_python.sh --full
```

Quick mode covers extension/C++ builds, strict Rust checks, deterministic
Python IDL metadata, source-copy/unsafe audits, real Rust Image and C++
PointCloud producers, Python writable Image/PointCloud loans, pointer identity,
read-only enforcement, sample and derived-view lifetime, native borrow
pressure/recovery, GIL progress, and ABI and readiness preflight. Full mode
adds slow/stalled/retained peer pressure, GC/resources, cross-thread loan
races, signal latency, ten-second idle CPU, interpreter shutdown,
wheel/fresh-venv publisher/consumer connection, an ASan-instrumented PyO3
ownership run, and the existing full regression gate.

The dedicated writable-loan wrapper runs the same focused/full gate and the
publisher timing sanity check:

```bash
./scripts/verify_python_publisher.sh --quick
./scripts/verify_python_publisher.sh --full
```

The standalone timing check keeps mapping separate from user payload filling
and send, and covers 64-byte, 6,220,800-byte, and 20-MiB payloads:

```bash
target/python-verify-venv/bin/python scripts/benchmark_python_publisher.py
```

It checks that mapping time does not grow like a payload copy. The publisher
milestone report ([`python-publisher-report.md`](python-publisher-report.md))
records the accepted full-run evidence and retained log locations.

## MCAP gate

The durable recording/playback release entrypoint is:

```bash
./scripts/verify_mcap.sh --quick
./scripts/verify_mcap.sh --full
```

Quick mode runs the generated canonical codec and container tests, strict
Clippy, two byte-identical IDL/codegen trees, two semantically exact fixture
generations, checked-in fixture verification, `mcap info` without Composer,
corrupt-file failure, a real Rust Image/PointCloud/Control `--all` recording,
rate-zero replay, and Rust → MCAP → C++ exact Image validation.

Full mode adds C++ and Python record sources, Rust/C++/Python playback
consumers, mixed native replay, 1x/2x timing, generated 1920×1080 and 100k/~20
MiB PointCloud files, bounded live large-message recording, Sensor isolation,
Control and disk failures, recorder and player Composer recovery, 100-process
replay cleanup, a real 300-second mixed record/resource soak, and every
inherited full verification entrypoint. Logs and generated large files are
retained under `target/mcap-verify-logs`; large files are never checked in.

## Python Component gate

The synchronous Python lifecycle adapter has a dedicated release entrypoint:

```bash
./scripts/verify_python_component.sh --quick
./scripts/verify_python_component.sh --full
```

Quick mode runs workspace tests and strict Clippy, the existing Python RX/TX
quick gate, API tests, a retained wheel build, and a fresh-venv mixed graph. The
graph covers initial BLOCKED state, Rust Camera -> Python Detector -> C++
Viewer data, provider loss/replacement, stale handles, critical/background
exceptions, native-wait and bounded-compute cancellation, sibling wake/join,
SIGINT/SIGTERM shutdown, unexpected critical return, worker `SystemExit`,
repeated-exception frame release, partial activation rollback, Composer
crash/recovery, provider death while Composer is down,
lifecycle-only/require-only/provide-only Components, and duplicate ComponentId
quarantine.

Full mode uses the installed wheel for 100 activation cycles and a five-minute
mixed-process soak, then inherits the complete Python, WaitSet, pressure, v0.1,
Debug/Release CTest, native sanitizer, deterministic IDL, and external install
suite. Logs are retained beneath `target/python-component-verify-logs` and
`target/python-component-logs`; see
[`python-component-report.md`](python-component-report.md) for the accepted run.

## Historical v0.1 quick gate details

Quick mode runs:

1. `cargo fmt --all -- --check`;
2. `cargo test --workspace`;
3. strict workspace Clippy with all targets/features and `-D warnings`;
4. two clean IDL generations plus byte comparison against checked-in outputs;
5. CMake Debug build/CTest with warnings-as-errors (the current CMake suite has
   12 tests per configuration, including four TF tests);
6. CMake Release build/CTest with warnings-as-errors.

This includes unit/integration coverage for core types, lifecycle/composition,
protocol mismatch, CLI rendering, IDL diagnostics, stream policies, C++
descriptors, EffectScope, and the C++ runtime codec/state machine.

## Historical v0.1 full gate details

Full mode first runs quick mode, then:

1. `verify_phase4.sh` for real cross-language data-plane ABI/interop and frozen
   Phase 1–4 regression;
2. `test_mixed_runtime.sh` for Rust Camera → C++ Detector → Rust Viewer,
   provider replacement, Composer recovery, and critical task failure;
3. `test_install.sh` for clean-prefix install, an installed generated graph,
   and fresh external Rust/C++ consumers;
4. `soak_v01.sh --smoke` (or configured duration/cycles);
5. C++ ASan+UBSan build/CTest when the selected compiler supports it;
6. `benchmark_v01.sh --smoke`.

Pressure verification is kept as an explicit companion gate so frozen v0.1
verification remains recognizable. Run the deterministic semantics/fault
tests, both mixed-language publisher directions, the graph-under-pressure soak,
and the scaling baseline with the commands below.

Set `RBT_V01_SKIP_SANITIZER=1` only when the compiler/runtime cannot support
the sanitizer configuration. Full verification reports the skip explicitly.

## WaitSet gate

The event-driven receive path has a focused entrypoint. It checks the Rust
WaitContext/source/semantics/fairness/lifecycle tests, the Rust compose tests,
the C++ Debug and Release builds with the current 12-test CTest suite
(including `waitset_tests` and four TF tests), and the production unsafe-source
audit:

```bash
./scripts/verify_waitset.sh --quick
./scripts/verify_waitset.sh --full
RBT_WAITSET_PRESSURE_FULL=1 ./scripts/verify_waitset.sh --full
```

Full mode additionally runs the v0.1 regression gate, the full WaitSet
benchmark matrix, the 45-second idle soak, and either the pressure smoke or the
300-second pressure soak. The standalone comparison and idle commands are:

```bash
./scripts/benchmark_waitset.sh --smoke
./scripts/benchmark_waitset.sh
./scripts/soak_waitset_idle.sh --smoke
```

The exact completed WaitSet observations are summarized in
[`waitset-report.md`](waitset-report.md). The accepted historical unified run
is retained at `target/waitset-verify-logs/waitset-full-20260910T234120Z-98790`;
its six-test ASan/UBSan/leak result predates the four TF CTest cases and is
retained at
`target/v01-logs/verify-full-20260910T234126Z-99689/sanitizer-ctest.log`.

## Individual checks

```bash
./scripts/test_mixed_runtime.sh
RBT_MIXED_ENABLE_PRESSURE=1 ./scripts/test_mixed_runtime.sh
./scripts/test_mixed_pressure.sh
./scripts/soak_pressure.sh --smoke
./scripts/benchmark_pressure.sh --smoke

# v0.4 resource/pressure observability gate
./scripts/verify_pressure_tuning.sh --quick
./scripts/test_install.sh
./scripts/soak_v01.sh --duration-seconds 300
./scripts/benchmark_v01.sh --smoke
./scripts/verify_phase4.sh
cargo test -p rbt-iox2 --test pressure_semantics -- --test-threads=1
cargo test -p rbt-iox2 --test pressure_fault -- --test-threads=1
```

The mixed and soak scripts choose isolated control ports, terminate their child
processes on exit, and retain evidence under `target/v01-logs`. Phase 4 logs are
retained under `target/phase4-logs`.

`soak_pressure.sh` requires the real mixed graph by default. One cycle runs the
Sensor fast/slow/held/never-receive topology, restarts held and stalled
subscribers, exercises Control QueueFull and process-death recovery, replaces a
provider and restarts Composer while pressure remains active, and checks the
frozen v0.1 orphan/stale-binding/resource invariants. Only an explicit
`RBT_PRESSURE_SKIP_GRAPH=1` may omit the graph portion. A full pressure
benchmark covers 1/2/4/8 subscribers for 64 B, 6.22 MiB, and bounded 20 MiB
payloads plus the mixed-speed case.

## Installation smoke contract

`test_install.sh` creates a temporary prefix, temporary home, and projects
outside the source tree. It verifies the installed `rbt`, `rbt-idl`, and
`composer` binaries plus packaged docs/licenses; runs installed
`doctor -> new -> check -> IDL -> build -> run`; waits for the generated graph
to become active; and proves bounded SIGTERM cleanup with exact PID/process
group auditing. It also compiles/runs a Rust consumer whose only rbt path is
under the prefix and configures/builds/runs a C++ consumer with
`find_package(rbt CONFIG REQUIRED)` and installed targets `rbt::runtime` and
`rbt::iox2`.

## Soak invariants and resources

The default soak duration is 300 seconds and can be changed with
`--duration-seconds` or bounded by `--cycles`. Each cycle kills/replaces a
camera, kills/restarts the detector, restarts Composer while viewer data
continues, and kills/restarts the viewer.

At every stable checkpoint it uses the JSON inspection view to reject:

- duplicate authoritative component ids/process conflicts;
- capabilities not backed by an active owner;
- active requirements with missing/stale topology bindings;
- unexpected `FAILED` components.

Linux `/proc` sampling records RSS KiB, fd count, and thread count per role in
`resources.tsv`. The harness reports heuristic obvious-growth flags; it does
not present them as a proof of leak absence. A sanitizer run supplies the
memory/undefined-behavior check for bounded C++ tests.

## Test-only fault controls

`RBT_TEST_FAIL_AFTER`, `RBT_TEST_EXIT_AFTER`, and `RBT_TEST_PAUSE_MS` are
recognized by detector examples. The advanced image consumer additionally
recognizes `RBT_TEST_HOLD_SAMPLE_MS`, `RBT_TEST_STOP_RECEIVING`,
`RBT_TEST_CONSUMER_RATE_HZ`, and `RBT_TEST_MAX_HELD` for bounded retention,
stall, rate, and simultaneous-Borrow experiments. They are deterministic
harness controls, not production configuration or public SDK surface.
When `RBT_TEST_MAX_HELD` is greater than one, set it on the advanced image
publisher as well so the service and subscriber agree on Borrow capacity.
Activation examples must put tasks under `EffectScope`; raw unowned
`tokio::spawn`/detached C++ threads are audit failures.

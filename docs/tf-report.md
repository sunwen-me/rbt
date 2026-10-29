# rbt v0.8 Spatial Transforms — acceptance report

## Decision / Status

ACCEPTED — software and release artifacts. Product and SDK are **0.8.0**.
The full software gate completed 2026-10-19 18:55:18 UTC with exit 0:
**50/50 labels passed** in 8,165.669 seconds, with no source drift across its
350 inputs. Evidence: `target/tf-systemd-full-20261019T163912Z/result.json`
and `target/tf-systemd-full-20261019T163912Z/root-acceptance-summary.json`.
The accepted software manifest SHA-256 is
`667784d52ec862b94a358a31bbbdf1e4c9525797128a2ee0568d9848a7aa8efd`.

The full gate used the pre-release 0.7.0 version as required. After software
acceptance, the reviewed 15-file version patch advanced product/SDK to 0.8.0;
32 local Cargo.lock package versions changed and all 157 sourced dependencies
remained identical. Release verification then passed all five gates with exit 0
at 2026-10-20 01:43:22 UTC in 1297.666 seconds. Every gate had
empty source drift before and after execution. Its 350-input manifest SHA-256 is
`53f436f8f566918403151a7c25107f0c003c3483eec95d4f4544544600f1a9d1`.
Evidence: `target/tf-release-verification/run-20261020T012144Z-522975/result.json`.

| 0.8.0 release check | Status | Seconds |
| --- | --- | ---: |
| verify-tf-quick | PASS | 524.407 |
| tf-packages | PASS | 248.916 |
| python-component-quick | PASS | 358.536 |
| dx-quick | PASS | 42.770 |
| time-external-consumers | PASS | 122.893 |

Validated SDK prefix: `target/tf-release-verification/run-20261020T012144Z-522975/packages/sdk/`.
Fresh wheel: `target/tf-release-verification/run-20261020T012144Z-522975/logs/tf-packages/tf-packages-20261020T013029Z-555802/wheel/rbt-0.8.0-cp312-cp312-manylinux_2_28_x86_64.whl`.

Hardware is **NOT RUN**: no device was selected or opened. Physical dynamic TF
is also NOT RUN. The approved unsafe boundary is 49 legacy locations plus 24
generated TF markers, 73 total, with no new hand-written unsafe. See
[tf-unsafe-review.md](tf-unsafe-review.md).

## Repository audit

See [tf-architecture.md](tf-architecture.md). The checkout has no commits; all
project files were untracked before work. Pre-change copies and a SHA-256
manifest of 326 files are in `target/tf-baseline/`. Actual runtime implementation
is `rbt-compose`, not a separate `rbt-runtime` crate. The pre-change product
baseline was 0.7.0; product/SDK metadata advanced to 0.8.0 after software
acceptance. The maintained iceoryx2 is 0.10.0-rbt.2. Existing documentation has
stale labels; no silent baseline correction is claimed.

## Architecture

Distributed native publishers and per-consumer bounded local buffers. Queries
perform graph traversal, interpolation and composition locally, with no RPC or
Composer payload path. Static authority offers separate related snapshot RPC
and update Stream capabilities. See the architecture document for actual source
integration points and ownership.

## Math conventions

T_target<-source, parent<-child edges, metres/radians/right-handed coordinates,
f64 arithmetic and xyzw quaternion order. Input quaternion norm tolerance is
1e-6; only accepted roundoff is normalized. Internal SLERP results use a separate
normalization path. Rust math/wire tests pass (8 + 8), C++ math passes and Python
math has 5 passing tests. Both the complete quick gate and the accepted full
candidate passed the shared math fixture in Rust, C++ and Python. The accepted
full run also passed native transport and cross-language acceptance; its C++
TF lanes and process evidence are summarized below.

## FrameId contract

1–63 case-sensitive ASCII bytes, slash-separated validated segments and exactly
64 canonical zero-padded wire bytes. No global registry or hash-based equality.

## Static TF semantics

Complete bounded sets, authority-scoped revisions, explicit effective-from time,
bounded staged updates, atomic publication, gap detection and snapshot resync.
Late snapshots cannot overwrite accepted newer state. Root ran all seven native
transport tests, including snapshot/update races, missing revisions, late join,
size rejection, lost-final-notification refresh and scoped cleanup; all passed.
The complete quick gate repeated all seven tests, including the measured
five-second native late join.
The specification follow-up fixes failed resync handling: rejected queued
updates abort application of the captured snapshot, while a valid revision gap
may be healed by an authoritative snapshot covering the highest observed
revision. Failed RPC/response/native receive paths leave the binding stale;
a concurrent Rust `SnapshotInFlight` result does not invalidate the active
request. A new actual-native malformed-update race fails against the frozen
pre-repair Rust implementation and passes after repair. Root then ran all eight
native tests, including failed-refresh guard release and recovery
(`target/tf-root-repair-transport-tests.log`), and all four C++ TF targets
(`target/tf-root-repair-cpp.log`). The accepted full candidate repeated the
native Rust and C++ TF checks; see
`target/tf-systemd-full-20261019T163912Z/logs/tf-full-20261019T163912Z-13601/manifest.tsv`.

## Dynamic TF semantics

Bounded identified timestamp histories, explicit monotonic continuity epochs,
precise extrapolation/gap errors and no implicit latest. Epoch/generation changes
isolate history. The accepted full process gate completed 100/100 provider
replacements in 1,383.388 seconds, with final observed consumer counters of 47/47 correct lookups, zero lookup
errors, exact lifecycle and 101 fresh IDs for each dynamic, snapshot and
static-update endpoint. Resource checks passed with retained FD growth 0, RSS
growth 1,654,784 bytes and thread growth 0; per-process peaks were 96 FDs,
20,193,280 RSS bytes and 21 threads. Evidence:
`target/tf-process-logs/1792433903-399236-replacement/summary.json`.

## TransformBuffer design / Graph invariants

Arc/Mutex Rust local state; provider/edge/history/pending-work limits; no retained
SHM guards or callbacks under the lock. One parent and one authority per child,
no cycles/self edges; competing claims fail closed. Source review and strict
concurrent snapshot/dynamic-ingest lookup tests passed. The accepted full run
completed 100 provider replacements and ten healthy Composer restarts. Its
combined-outage and mixed-soak runs also passed exact post-join retirement checks;
current process evidence is recorded in the soak and resource sections below.
The accepted full run passed 10/10 healthy restarts in 168.629 seconds with
stable lifecycle and 322/322 correct lookups. Resource checks passed with
retained FD growth 2, RSS growth 131,072 bytes and thread growth 0; per-process
peaks were 95 FDs, 18,677,760 RSS bytes and 21 threads. Evidence:
`target/tf-process-logs/1792435288-411119-composer-restart/summary.json`.

## Time/Clock integration / Interpolation and discontinuities

Uses existing rbt-time objects. Queries must be converted into each edge clock
using a direct explicit valid mapping, rather than comparing numeric readings.
Unknown uncertainty remains unknown and requires explicit best effort. v0.7
mapping generation/expiry/range errors remain distinguishable. Query-conversion
direction was corrected and the focused Rust suites passed. The reported bound
is the maximum absolute timestamp uncertainty of the inputs used; it is not a
relative time-alignment bound or spatial accuracy.

## Authority conflict behavior / Snapshot consistency / Provider replacement

Physical CapabilityInstanceId is distinct from logical FrameId. Binding changes
remove old owned data and reset current-provider observations. The accepted full
run completed 100 actual process replacements and observed 101 fresh capability
IDs for each dynamic, snapshot and static-update endpoint, with exact lifecycle.
Late old-authority rejection is not directly observable in this process mode; it
is covered by the separate TransformBuffer/native tests. The combined-outage and
mixed-soak runs passed exact post-join retirement checks for all six and four
successful activations, respectively.

## Rust API

rbt-tf math/buffer/wire and native rbt-tf-transport adapters are implemented;
rbt facade exports rbt::tf. Natural local lookup returns transform plus temporal
and authority provenance. Activation integration uses existing EffectScope.

## C++ API

C++20 local math/buffer uses rbt::tf and native generated transport bindings.
Public local API header is rbt/transforms.hpp; generated rbt/tf.hpp holds wire
records. It does not forward TF payloads through Rust FFI. The accepted full run
passed all four TF C++ tests (math, buffer, transport and component) in each of
Debug, Release, ASan and UBSan: 4/4 per lane with zero skips. The logs and
completeness records are in
`target/tf-systemd-full-20261019T163912Z/logs/tf-full-20261019T163912Z-13601/`.

## Python API

Validated math, Rust-backed local buffer wrappers and typed native transport are
implemented. A fresh installed wheel passed the focused TF tests with zero
skips, including the installed-module origin check and component lifecycle
tests. Component activation integration passed five tests, including real
native-process traffic, pair validation, invalid initial configuration,
cancellation, cleanup and provider SIGKILL/recovery. Recovery keeps the consumer
process, observes BLOCKED then a new activation, invalidates old history/tokens
and checks fresh provider identities and lookup provenance. The accepted full
run's 19-step package manifest passed fresh-wheel tests, external Rust/C++
consumers and six-pair installed-wheel interop. Evidence is in
`target/tf-systemd-full-20261019T163912Z/logs/tf-full-20261019T163912Z-13601/packages/tf-packages-20261019T181430Z-384704/manifest.tsv`;
the installed-wheel matrix is recorded at
`target/tf-systemd-full-20261019T163912Z/logs/tf-full-20261019T163912Z-13601/packages/tf-packages-20261019T181430Z-384704/external/wheel-interop/tf-interop-20261019T181817Z-398999/summary.json`.
Product/SDK 0.8.0 also passed installed-wheel and component-packaging
verification. The new package manifest is `target/tf-release-verification/run-20261020T012144Z-522975/logs/tf-packages/tf-packages-20261020T013029Z-555802/manifest.tsv`.
No per-edge/per-query thread or unbounded queue is permitted.

## IDL and ABI

Canonical types/tf.rbt uses existing IDL1/descriptor1. Large fixed arrays exposed
a codegen Default/codec gap; repaired generation preserves all historical output
bytes, as checked by temporary regeneration. New fixed hashes:

| Message | Hash |
| --- | --- |
| DynamicTransform | b1a02959970fa0ecd30b6219ab2bcb56 |
| StaticSnapshotRequest | 85b655e27004d7d38eb10d720e035d7b |
| StaticSnapshot | cd373f3a4bdf5d680aec5d208da98e82 |
| StaticUpdate | 8e4aaeb6d5c1bd7c040a9ff2384432b8 |

Old message hash tests pass. The accepted full run passed the inherited
StampedBytes, protocol, legacy-MCAP and native compatibility checks. Generic
recording/replay of the new TF wire messages remains NOT VERIFIED: the inherited
MCAP checks exercise frozen legacy messages. No complete TF recorder/replay
semantics are claimed.

## CLI diagnostics / Pressure metrics

Composer observations are labelled partial; missing local history/metrics are
unavailable, not zero. Root ran eight focused CLI parser/diagnostic tests,
including impossible physical identities, and they passed
(`target/tf-root-cli-tests.log`). Explicit bounded local snapshots may
show a selected consumer view, with freshness checked in host steady time.
The specification follow-up adds distinct `missing_frame_count`,
`clock_mapping_failure_count` and `authority_conflict_count` fields while
retaining the original aggregate counters. Root reviewed their Rust/C++ event
classification and provider-replacement reset behavior. All 48 Rust TF tests,
57 CLI library tests and two CLI command tests passed in
`target/tf-root-repair-metrics-tests-rerun.log`. CLI files predating the new
fields remain readable; absent categories stay null, measured zero stays zero,
and impossible category totals are rejected. The accepted full candidate passed
the fresh-wheel package gate, including the new categories and the distinction
between core lookup errors and Python argument validation; see
`target/tf-systemd-full-20261019T163912Z/logs/tf-full-20261019T163912Z-13601/packages/tf-packages-20261019T181430Z-384704/manifest.tsv`.

## Cross-language matrix

The accepted full run passed all six native static/dynamic publisher-consumer
pairs and shared math semantics in Rust, C++ and Python. The matrix is recorded
in `target/tf-systemd-full-20261019T163912Z/logs/tf-full-20261019T163912Z-13601/interop/tf-interop-20261019T181425Z-383754/summary.json`.
The external installed-wheel matrix also passed in
`target/tf-systemd-full-20261019T163912Z/logs/tf-full-20261019T163912Z-13601/packages/tf-packages-20261019T181430Z-384704/external/wheel-interop/tf-interop-20261019T181817Z-398999/summary.json`.
These are synthetic software fixtures, not physical measurements.

## Physical-hardware test

Real camera: NOT RUN. Physical dynamic TF: NOT RUN. No measured extrinsics or
real dynamic pose hardware have been supplied. Example configured values must
say “Configured extrinsic — calibration accuracy unverified”. Hardware mode must
require an explicit device and at least 300 seconds; no automatic device choice.

## Software-only tests

The inherited v0.1 full gate runs its deterministic fault/recovery check with
`--smoke` / `max_cycles=1`. Its configured 300-second limit is an upper
bound: two recorded runs measured 5 and 4 seconds, respectively
(`target/v01-logs/verify-full-20261018T194835Z-345749/soak.log` and
`target/v01-logs/verify-full-20261018T195909Z-376710/soak.log`). Report this as
a one-cycle smoke check. The accepted full run's separate TF 300-second process
gates are detailed below.

Root focused integration run passed Rust buffer/config/math/wire, six camera
format/input-bound tests, seven native TF transport tests, CLI tests and all 15
message-descriptor tests (`target/tf-root-integration-tests.log`). A subsequent
strict concurrency regression adds two ingest writers and three lookup readers;
all 44 TF buffer/config/math/wire tests passed after the resync-cooldown and
mixed-uncertainty policy fixes. Explicit best effort permits unknown precision
while still rejecting any known uncertainty above the configured limit.
Workspace formatting and strict all-target/all-feature Clippy passed
(`target/tf-root-workspace-clippy.log`). Fresh-wheel Python, installed native
Rust/C++ consumers and six native interop pairs passed the earlier provisional
package gate; the accepted full run repeated those checks below. The next
inherited full run passed workspace tests but ASan found a test-owned dangling
reference in `sdk/cpp/tests/runtime_tests.cpp`;
the fixture held a reference across replacement of its parsed message. That
test repair passed Debug, Release, ASan and UBSan in
`target/tf-verify-logs/runtime-tests-uaf-repair-20261018/`. The frozen 190743
candidate later passed all inherited full categories. Preserve
the failure under
`target/tf-verify-logs/inherited-time-full-after-clippy-pass/`.

The guide's local Rust command was also executed with the pinned toolchain:
`cargo run --locked -p rbt-tf-local -- --config docs/tf-config-example.toml`.
It exited 0, printed the explicit unverified-calibration notice, preserved
unknown timing precision and mapped the point back through the inverse within
floating-point tolerance. Evidence: `target/tf-local-example-acceptance.log`.

The supervised inherited Python full suite passed 181 tests and skipped 5 in
`target/python-verify-logs/python-full-20261018T094907Z-32032/python-pytest-full.log`.
All five are `test_tf_component.py` cases whose inherited process has no
`RBT_TF_COMPONENT_OVERLAY`; the separate fresh-wheel TF gate supplies that
environment and executed all five within its 21 passing, zero-skip cases.
The inherited Python ASan wheel build and its focused ownership/borrow tests
also passed (`python-asan-ownership.log` in the same directory). The frozen
190743 candidate passed all inherited categories; its wrapper ended FAIL at the
combined-outage process assertion.

An earlier supervised run also passed Python asyncio full verification in
`target/python-asyncio-verify-logs/python-asyncio-full-20261018T102900Z-149912/`:
25 unit tests, 100 component cycles, 304 seconds of component mixed soak,
10 Composer restarts, cancellation/timeout/ownership checks and zero unexpected
pending-task warnings. Its inherited RPC soak passed exactly 300 seconds,
10 Composer restarts and three provider replacements, with all pending-task,
in-flight RPC, response-guard, resource-growth-violation and orphan counters zero
(`target/rpc-soak-logs/20261018T105809Z-270556/`). These inherited tests do not
replace the TF-specific long process gates below.

Pressure Tuning full also passed in
`target/pressure-tuning-verify-logs/pressure-tuning-full-20261018T111319Z-367419/`.
Its pressure soak ran 326 seconds across five complete cycles, recorded 50
resource samples, and ended with current loans/borrows, orphan capabilities and
stale bindings all zero (`target/pressure-logs/pressure.jwqQTL/`). The subsequent
matrix validated the full 20 MiB pointcloud payload for all eight fast
subscribers. These are inherited Stream/pressure checks, not TF process-soak
evidence.

The accepted 163912 full candidate passed all 50 exact top-level labels in
8,165.669 seconds with exit 0 and no source drift across 350 frozen inputs.
The terminal result and independently reviewed category summary are
`target/tf-systemd-full-20261019T163912Z/result.json` and
`target/tf-systemd-full-20261019T163912Z/root-acceptance-summary.json`; its
full manifest is
`target/tf-systemd-full-20261019T163912Z/logs/tf-full-20261019T163912Z-13601/manifest.tsv`.

## 300-second soak / Performance

The accepted full candidate's combined-outage and mixed-soak gates both passed.
The combined-outage run
(`target/tf-process-logs/1792435457-411562-combined-outage/summary.json`)
ran for 330.057 seconds, with final observed consumer counters of 440/440
correct lookups, zero lookup errors and
exact post-join retirement evidence for all six successful activations. The
mixed soak
(`target/tf-process-logs/1792435788-415589-soak/summary.json`) completed
329.892 seconds, with final observed consumer counters of 560/560 correct
lookups, zero errors and exact retirement
evidence for all four successful activations. Each run passed all 22 top-level
assertions, bounded Stream/RPC capacity and guards, and the 256-sample history
limits. Both recorded zero cleanup errors and orphan groups; their four
SIGTERM-cleanup participants exited zero with `shutdown_complete` and no forced
SIGKILL.

The combined-outage run's retained FD growth was 2, RSS growth was 1,703,936
bytes, and thread growth was 0; per-process peaks were 125 FDs, 21,278,720 RSS
bytes and 22 threads. The mixed soak's retained FD growth was 2, RSS growth was
1,183,744 bytes, and thread growth was 0; peaks were 126 FDs, 20,774,912 RSS
bytes and 22 threads. Both passed the resource thresholds below.

The older 190743 combined-outage result remains archived FAIL. Its negative
control still rejects the missing post-join proof
(`target/tf-root-pressure-old-log-negative-control.json`). The accepted full
candidate independently passed the repaired combined-outage and mixed-soak gates.

Resource thresholds are retained FD growth ≤4, absolute FDs ≤128, RSS growth
≤32 MiB and absolute RSS ≤512 MiB, thread growth ≤4 and absolute threads ≤64.
Each PID first establishes real dynamic/snapshot traffic and correct lookups,
then a fixed five-second warmup and a distinct five-second initial measurement
window. FD retention compares immutable initial minima with post-recovery and
final live five-second minima, sampled every 50 ms. Full peaks and FD symlink
inventories are retained; peak FD growth is reported separately. RSS and thread
growth use high-water measurements. A new PID receives its own initial window;
long-lived consumer baselines are never reset after replacement/restart.

Earlier diagnostic failures used FD high-water growth as a retention test;
some lacked a retained-FD inventory or post-reconnect progress proof. Those
failures remain archived and are not relabelled PASS. An early minimum-window
attempt started before native traffic readiness and also failed; the fixed
readiness/warmup/baseline procedure above addresses that measurement defect.
The old incomplete 100-cycle attempt lacks a saved terminal exception and
complete provenance; its cause remains unverified, not an accepted runtime
defect or a PASS.

The accepted full candidate's Debug local-lookup benchmark passed with 10,000
correct results in each case. Transport is excluded, and no transport latency
claim follows from these measurements. The summary is
`target/tf-systemd-full-20261019T163912Z/logs/tf-full-20261019T163912Z-13601/benchmark/20261019T181822Z-399200/summary.json`;
the benchmark binary SHA-256 is
`cb4b27a9898e52c07265839ae6b4302f5788b783586da67260270bda24c548d5`.

| Case | p50 / p90 / p99 / max (µs) | Queries/s | RSS before → after (KiB) |
| --- | --- | --- | --- |
| Static, 1 edge | 8.532 / 8.805 / 16.693 / 94.032 | 121,498 | 7,528 → 8,196 |
| Static, 3 edges | 11.916 / 12.032 / 17.132 / 366.761 | 82,787 | 9,028 → 9,104 |
| Static, 10 edges | 30.549 / 30.901 / 38.945 / 73.839 | 32,385 | 9,104 → 9,104 |
| History interpolation | 8.303 / 8.397 / 12.874 / 65.263 | 118,509 | 9,104 → 9,172 |

## Resource cleanup

The accepted full run's four TF process stages recorded clean child cleanup.
For replacement, healthy Composer restart, combined-outage and mixed soak,
cleanup errors and orphan process groups were zero. The four SIGTERM-cleanup
participants in each stage exited zero, reported `shutdown_complete` and needed
no forced SIGKILL. Process summaries are
`target/tf-process-logs/1792433903-399236-replacement/summary.json`,
`target/tf-process-logs/1792435288-411119-composer-restart/summary.json`,
`target/tf-process-logs/1792435457-411562-combined-outage/summary.json` and
`target/tf-process-logs/1792435788-415589-soak/summary.json`. Software full-gate
and 0.8.0 release artifact acceptance are complete.

## Versions / hashes / unsafe

Product/SDK is 0.8.0. The full software gate passed before the version bump;
the subsequent five-gate release verification passed with no source drift:
`target/tf-release-verification/run-20261020T012144Z-522975/result.json`. The release source manifest SHA-256 is
`53f436f8f566918403151a7c25107f0c003c3483eec95d4f4544544600f1a9d1`. Only the reviewed
15-file version patch differs from the accepted full-run source snapshot.
All 157 sourced dependencies are unchanged.

Frozen axes remain IDL1, descriptor1, protocol2, snapshot4, rbt-canonical-v1
and native iceoryx2 0.10.0-rbt.2. The approved unsafe boundary is
legacy49 + generatedTF24 = total73, with zero new hand-written unsafe.

## Acceptance checklist

Software and release acceptance are complete: 77 checklist requirements PASS;
the conditional real-camera row is NOT RUN. The frozen full software candidate
passed all 50 labels and the 0.8.0 release passed all five checks, with no source
drift. Earlier failed and interrupted runs remain archived below. Physical
camera and dynamic-hardware measurements are not claimed.

| Requirement | Status | Evidence |
| --- | --- | --- |
| Transform direction convention is frozen. | PASS | Rust/C++/Python math tests and shared native semantics fixture |
| Point/vector/pose semantics are correct. | PASS | Rust/C++/Python math tests and shared native semantics fixture |
| Quaternion order is consistent. | PASS | Rust/C++/Python math tests and shared native semantics fixture |
| Quaternion validation is safe. | PASS | Rust/C++/Python math tests and shared native semantics fixture |
| Inverse and composition pass. | PASS | Rust/C++/Python math tests and shared native semantics fixture |
| SLERP passes numerical edge cases. | PASS | Rust/C++/Python math tests and shared native semantics fixture |
| FrameId is bounded and validated. | PASS | Rust/C++/Python math tests and shared native semantics fixture |
| Frame graph has no cycles. | PASS | Rust buffer and C++ tf_tests; current Rust buffer suite 27/27 |
| Every child has at most one parent. | PASS | Rust buffer and C++ tf_tests; current Rust buffer suite 27/27 |
| Authority conflicts are detected. | PASS | Rust buffer and C++ tf_tests; current Rust buffer suite 27/27 |
| Conflicted paths fail closed. | PASS | Rust buffer and C++ tf_tests; current Rust buffer suite 27/27 |
| Static transforms work. | PASS | Rust buffer and C++ tf_tests; current Rust buffer suite 27/27 |
| Dynamic transforms work. | PASS | Rust buffer and C++ tf_tests; current Rust buffer suite 27/27 |
| Static late-join works. | PASS | Eight native transport tests; late join waits at least 5 s before client creation |
| Snapshot/update race is handled. | PASS | Eight native transport tests; late join waits at least 5 s before client creation |
| Missing static revisions trigger resync. | PASS | Eight native transport tests; late join waits at least 5 s before client creation |
| Snapshot size is bounded. | PASS | Eight native transport tests; late join waits at least 5 s before client creation |
| Snapshot is never silently truncated. | PASS | Eight native transport tests; late join waits at least 5 s before client creation |
| Dynamic history is bounded. | PASS | Rust buffer and C++ tf_tests; current Rust buffer suite 27/27 |
| Interpolation works. | PASS | Rust buffer and C++ tf_tests; current Rust buffer suite 27/27 |
| Past extrapolation is rejected. | PASS | Rust buffer and C++ tf_tests; current Rust buffer suite 27/27 |
| Future extrapolation is rejected. | PASS | Rust buffer and C++ tf_tests; current Rust buffer suite 27/27 |
| Excessive interpolation gaps are rejected. | PASS | Rust buffer and C++ tf_tests; current Rust buffer suite 27/27 |
| Transform epoch discontinuities are respected. | PASS | Rust buffer and C++ tf_tests; current Rust buffer suite 27/27 |
| No implicit latest fallback occurs. | PASS | Rust buffer and C++ tf_tests; current Rust buffer suite 27/27 |
| v0.7 Timestamp is reused. | PASS | Existing rbt-time API, Rust/C++ mapping tests and shared semantics fixture |
| ClockIdentity is validated. | PASS | Existing rbt-time API, Rust/C++ mapping tests and shared semantics fixture |
| Cross-domain timestamps require mapping. | PASS | Existing rbt-time API, Rust/C++ mapping tests and shared semantics fixture |
| Expired mapping is rejected. | PASS | Existing rbt-time API, Rust/C++ mapping tests and shared semantics fixture |
| Wrong-generation mapping is rejected. | PASS | Existing rbt-time API, Rust/C++ mapping tests and shared semantics fixture |
| Unknown timing precision is not fabricated. | PASS | Existing rbt-time API, Rust/C++ mapping tests and shared semantics fixture |
| Local lookup performs no RPC. | PASS | Source review: local buffer has no transport handle; Composer carries descriptors only |
| Composer is outside TF payload path. | PASS | Source review: local buffer has no transport handle; Composer carries descriptors only |
| Native Stream zero-copy path is preserved. | PASS | Eight native transport tests; late join waits at least 5 s before client creation |
| TransformBuffer memory is bounded. | PASS | Rust buffer and C++ tf_tests; current Rust buffer suite 27/27 |
| No unbounded per-message sample retention exists. | PASS | Eight native transport tests; late join waits at least 5 s before client creation |
| Provider replacement invalidates old data. | PASS | Rust replacement tests and fresh-wheel Python provider SIGKILL/recovery test |
| Old physical identities are rejected. | PASS | Rust replacement tests and fresh-wheel Python provider SIGKILL/recovery test |
| New provider snapshots are accepted. | PASS | Rust replacement tests and fresh-wheel Python provider SIGKILL/recovery test |
| 100 replacement cycles pass. | PASS | Accepted 163912 run: 100/100 replacements, exact lifecycle, final observed 47/47 correct lookups and 101 fresh IDs per endpoint; target/tf-process-logs/1792433903-399236-replacement/summary.json |
| Healthy Composer restart causes no lifecycle churn. | PASS | Accepted 163912 run: 10/10 restarts, stable lifecycle and 322/322 correct lookups; target/tf-process-logs/1792435288-411119-composer-restart/summary.json |
| Provider disappearance follows existing Requirement rules. | PASS | Rust replacement tests and fresh-wheel Python provider SIGKILL/recovery test |
| Shutdown and cancellation are bounded. | PASS | Native scope tests, five Python component tests, C++ scope tests and measured SIGTERM shutdown smoke |
| Rust API passes. | PASS | Accepted 163912 full run passed its Rust and native transport lanes; target/tf-systemd-full-20261019T163912Z/logs/tf-full-20261019T163912Z-13601/manifest.tsv |
| C++ API passes. | PASS | Accepted 163912 full run: four TF targets passed in Debug, Release, ASan and UBSan, 4/4 per lane with zero skips; target/tf-systemd-full-20261019T163912Z/logs/tf-full-20261019T163912Z-13601/manifest.tsv |
| Python API passes. | PASS | Accepted 163912 package gate passed all 19 steps, including installed-wheel TF tests, no-skips contract and origin checks; target/tf-systemd-full-20261019T163912Z/logs/tf-full-20261019T163912Z-13601/packages/tf-packages-20261019T181430Z-384704/manifest.tsv |
| Cross-language math agrees. | PASS | Accepted 163912 matrix passed shared semantics in Rust/C++/Python; target/tf-systemd-full-20261019T163912Z/logs/tf-full-20261019T163912Z-13601/interop/tf-interop-20261019T181425Z-383754/summary.json |
| Cross-language transport agrees. | PASS | Accepted 163912 matrix passed all six native pairs; target/tf-systemd-full-20261019T163912Z/logs/tf-full-20261019T163912Z-13601/interop/tf-interop-20261019T181425Z-383754/summary.json |
| Static extrinsics can be loaded from validated config. | PASS | Five static-config and six camera input/format tests; strict bounded config validation |
| No fake physical calibration accuracy is claimed. | PASS | Guide and camera example explicitly state configured extrinsics have unverified calibration accuracy |
| Real-camera test reports real evidence or NOT RUN. | NOT RUN | No explicitly selected device or measured extrinsic configuration; no device opened |
| Dynamic real-hardware test is not faked. | PASS | No dynamic pose device selected; synthetic fixture is explicitly software-only |
| TF CLI JSON is structured. | PASS | Seven TF status tests and two actual CLI integration tests; bounded local snapshot DTO |
| Diagnostics distinguish stale/unavailable/zero. | PASS | Seven TF status tests and two actual CLI integration tests; bounded local snapshot DTO |
| TF pressure metrics are bounded. | PASS | Resource-capped buffer observations, capacity/eviction tests, native loan/borrow metrics and bounded ingest work |
| All old layout hashes are unchanged. | PASS | All 16 historical generated files byte-identical; frozen layout-hash tests pass |
| StampedBytes layout hash is unchanged. | PASS | All 16 historical generated files byte-identical; frozen layout-hash tests pass |
| MCAP encoding is unchanged. | PASS | Frozen canonical fixtures, large image/pointcloud native recording/replay, 100 replay cases and 300-second record soak; target/tf-mcap-sampler-full-20261018T115554Z/ |
| RPC semantics are unchanged. | PASS | Accepted 163912 inherited full lane passed; target/tf-systemd-full-20261019T163912Z/logs/tf-full-20261019T163912Z-13601/inherited-time-full.log and manifest.tsv |
| WaitSet semantics are unchanged. | PASS | Accepted 163912 inherited full lane passed; target/tf-systemd-full-20261019T163912Z/logs/tf-full-20261019T163912Z-13601/inherited-time-full.log and manifest.tsv |
| Existing Stream profiles are unchanged. | PASS | Full Pressure Tuning: native Sensor/Control semantics, 326-second pressure soak and full-payload benchmark matrix; target/pressure-tuning-verify-logs/pressure-tuning-full-20261018T111319Z-367419/ |
| iceoryx2 maintenance provenance passes. | PASS | Current maintained prefix provenance check and native source diff fingerprint match |
| Original 49 unsafe locations remain unchanged; exactly 24 generated TF markers are approved (total 73), with no new hand-written unsafe. | PASS | Shared audit and nine auditor regressions |
| Fresh wheel passes. | PASS | Accepted 163912 package manifest: fresh wheel, installed TF tests, zero-skips contract and six-pair wheel interop passed; target/tf-systemd-full-20261019T163912Z/logs/tf-full-20261019T163912Z-13601/packages/tf-packages-20261019T181430Z-384704/manifest.tsv |
| External Rust consumer passes. | PASS | Accepted 163912 external Rust consumer and installed SDK origin checks passed; target/tf-systemd-full-20261019T163912Z/logs/tf-full-20261019T163912Z-13601/packages/tf-packages-20261019T181430Z-384704/manifest.tsv |
| External C++ consumer passes. | PASS | Accepted 163912 external C++ configure/build/consumer checks passed; target/tf-systemd-full-20261019T163912Z/logs/tf-full-20261019T163912Z-13601/packages/tf-packages-20261019T181430Z-384704/manifest.tsv |
| ASan passes. | PASS | Accepted 163912 full run: all four TF C++ tests passed under ASan with zero skips; target/tf-systemd-full-20261019T163912Z/logs/tf-full-20261019T163912Z-13601/cpp-address-ctest.log |
| UBSan passes. | PASS | Accepted 163912 full run: all four TF C++ tests passed under UBSan with zero skips; target/tf-systemd-full-20261019T163912Z/logs/tf-full-20261019T163912Z-13601/cpp-undefined-ctest.log |
| 300-second software soak passes. | PASS | Accepted 163912 combined-outage 330.057 s and mixed soak 329.892 s, final observed 440/440 and 560/560 correct lookups, all 22 assertions each; target/tf-process-logs/1792435457-411562-combined-outage/summary.json and target/tf-process-logs/1792435788-415589-soak/summary.json |
| No unbounded RSS growth. | PASS | Both accepted runs passed: RSS growth 1,703,936 / 1,183,744 bytes; peaks 21,278,720 / 20,774,912 bytes; target/tf-process-logs/1792435457-411562-combined-outage/summary.json and target/tf-process-logs/1792435788-415589-soak/summary.json |
| No FD/thread leak. | PASS | Both runs passed: retained FD growth 2 / 2, peaks 125 / 126 FDs and 22 / 22 threads, thread growth 0 / 0; target/tf-process-logs/1792435457-411562-combined-outage/summary.json and target/tf-process-logs/1792435788-415589-soak/summary.json |
| No orphan processes. | PASS | Both accepted process summaries report zero orphan groups and cleanup errors; four SIGTERM-cleanup participants per run exited zero without forced SIGKILL; target/tf-process-logs/1792435457-411562-combined-outage/summary.json and target/tf-process-logs/1792435788-415589-soak/summary.json |
| No stale authority after recovery. | PASS | Six combined-outage and four mixed-soak activations have exact post-join retirement evidence; target/tf-process-logs/1792435457-411562-combined-outage/summary.json, target/tf-process-logs/1792435788-415589-soak/summary.json; historical negative control still rejects missing proof |
| No global TF query server was introduced. | PASS | Architecture/source review: distributed typed publishers and per-consumer local buffers |
| No new transport protocol was introduced. | PASS | Architecture/source review: distributed typed publishers and per-consumer local buffers |
| No simulator clock was implemented. | PASS | Architecture/source review: distributed typed publishers and per-consumer local buffers |
| No parameter system was implemented. | PASS | Architecture/source review: distributed typed publishers and per-consumer local buffers |
| No calibration solver was implemented. | PASS | Architecture/source review: distributed typed publishers and per-consumer local buffers |

## Verification history

Status statements in this chronology apply to the time of the described run. Its
initial 0.7.0/not-accepted state predates the software acceptance recorded above.
At the start of this chronology, v0.8 was not accepted and product/SDK were
0.7.0. The complete quick gate passed at
`target/tf-verify-logs/quick-stable-20261018/tf-quick-20261018T090926Z-32582/`.
The first full run under `target/tf-verify-logs/full-final-20261018/` is
INCOMPLETE: its runner/session disappeared during inherited Python tests with
no terminal exit record. Its cause is unverified and its logs are preserved.
A separately supervised full run under
`target/tf-detached-full-20261018T094231Z/` exited FAIL with code 1. It reached
the inherited v0.1 generated-tree comparison, whose old explicit schema list
omitted `types/tf.rbt`; the regenerated tree therefore lacked `tf.rs`.
Strict full-tree checking is retained while all applicable canonical-schema
lists are updated. `launch.json` records the PIDs and `result.json` records the
terminal status. At that point, full acceptance remained pending.
After reviewing that fix, a fresh supervised full run was launched under
`target/tf-detached-full-20261018T100112Z/`. Its source manifest and source
snapshot preserve 340 code, schema, generated and verification input files.
This run exited FAIL with code 1 after 6,344.55 seconds at the inherited MCAP
full step on 2026-10-18 11:46:56 UTC. The large-image recording itself is
complete (55 messages, zero drops/writer errors), and its saved resource samples
pass the original bounds. The verifier stopped before printing the recording
summary or evaluating those bounds. Unchecked `/proc` reads in its sampler can
race normal process exit; that was the repair hypothesis at the time, not a
proven historical failing command because no shell trace was saved. No
task-owned processes remained for that archived run. Its inherited v0.1 full
gate completed successfully, including
deterministic full-tree IDL comparison,
Debug/Release C++, installed Rust/C++ consumers, mixed-runtime recovery,
fault soak and 12/12 C++ sanitizer tests. Evidence:
`target/v01-logs/verify-full-20261018T101336Z-108225/`.
WaitSet, DX, Python/asyncio and Pressure full checks also passed in this run.
The frozen 190743 candidate later passed every inherited full category; its
wrapper failed only at the combined-outage process assertion, and the final
mixed soak was not reached.
The sampler repair now rejects live collector failures while skipping incomplete
samples from real process exit, with no fabricated zero metrics. A deterministic
real-process regression fails against the frozen old sampler (exit 1) and passes
the repaired helper. The historical command remains unproven. Focused MCAP full
verification passed with exit 0 under
`target/tf-mcap-sampler-full-20261018T115554Z/`, with 341 frozen inputs. Its
300-second record soak contains 17,834 messages with zero drops, gaps or writer
errors. Recorder FDs/threads remained 65/21 and source FDs/threads 52/20; recorder
RSS was 19,132→19,372 KiB and source RSS 13,612→13,612 KiB.
The full wrapper also removes repeated Python/WaitSet ancestor checks through
existing composition flags while keeping every independent full category,
including the initial Component full lane. Its default step completion timeout
is six hours; all soak minimums, cycle counts and acceptance bounds are unchanged.
The total full run at `target/tf-detached-full-20261018T120323Z/` is INCOMPLETE:
the host reboot interrupted it during asyncio runtime tests. Inherited logs end
with `Terminated` at 12:23:48 UTC; the prior-boot systemd journal records shutdown
and reboot at 12:23:50 UTC. Its archived `host-reboot-journal.log` and
`interruption-observed.json` preserve that evidence. There is no terminal gate
result and no test failure is inferred from the interruption.
The full run under the separate user service at
`target/tf-systemd-full-20261018T182531Z/` is INCOMPLETE. Root deliberately
stopped it at 18:46:13 UTC after the specification audit found required repairs:
rejecting invalid updates during static resync, separate diagnostic failure
categories, and the common robot frame-tree documentation. Its atomic result
records signal 15 and 1,241.38 seconds, not a failed test assertion. Twenty
top-level steps passed before withdrawal, including the 303-second Component
full lane, its 100 cycles and ten Composer restarts. The asyncio-specific soak
and later stages did not finish. No task-owned processes remained for this stopped
run. The service preserves the 341-input source snapshot, boot/process identity and
logs; a new candidate must be frozen and tested after these repairs.
A repaired full candidate at
`target/tf-systemd-full-20261018T190353Z/` exited FAIL with code 1 after
88.41 seconds. Its manifest freezes 349 inputs with
SHA-256 `8d7fc12e53e6f75873f431fa7acbfbe80521c1d8bc2c07187d92246bd2611671`;
the added inputs include package metadata, compile-fail expected output and
the four inherited MCAP fixture files. The runner verifies source hashes
before and after execution and preserves an atomic terminal result. No source
drift occurred. Its fresh-wheel tests passed 20/21: the new mapping-counter
assertion incorrectly expected a Python argument-conversion failure to enter
the core buffer's counters. Python rejects already-invalidated mappings before
calling the buffer. The test must exercise an actual unavailable-mapping lookup
and separately verify that pre-buffer validation leaves counters unchanged.
At that point, full acceptance remained pending.
After the scoped test repair, root repeated all 21 focused Python tests against
that fresh wheel: PASS, zero skips, including installed-module origin checks
(`target/tf-root-repair-python.log` and `.xml`). Production code was unchanged.
The replacement full run at
`target/tf-systemd-full-20261018T190743Z/` exited FAIL with code 1 at
2026-10-18 21:30:19 UTC after 8,555.76 seconds. Its 349-input manifest has
SHA-256 `c9bf8597ad5075a625b46ea9c9bfd15402740d54885e4afa038784ae82873d2c`;
no source drift occurred. Of 47 top-level steps, 46 passed. The combined-outage
step alone failed; the final mixed soak was not reached. All inherited full
categories, six native cross-language pairs, external package consumers,
the benchmark, 100 provider replacements and ten healthy Composer restarts
passed before that failure.
The combined-outage run lasted 329.95 seconds and completed 440/440 correct
lookups. Its sole failed assertion required every live subscriber pressure
snapshot to contain zero borrowed samples. Six of 1,618 snapshots instead
showed one active borrow, within the configured maximum of one, with zero
borrow failures; the following snapshots reconciled to zero. This is a verifier
timing defect, not an observed runtime leak. The repair retains all live
capacity/high-water/failure bounds and adds activation-scoped evidence after
all ingestor tasks join: the exact previously observed native endpoints must
have no remaining Stream or RPC pressure sources. Missing evidence fails.
The 190743 combined-outage result remains archived FAIL. The old-log negative
control confirms it is not retroactively accepted: live bounds pass, but missing
post-join retirement proof is rejected
(`target/tf-root-pressure-old-log-negative-control.json`).
The repair adds twelve passing Python verifier regressions and an actual native
Rust ownership test. That test deliberately retains a received sample after
dropping both endpoints: its pressure source remains observable until the
sample is dropped. This validates the post-join registry check. Root repeated
these tests, workspace formatting and the 73-location unsafe audit. A two-consumer
native one-replacement smoke then passed in 41.65 seconds, with all four
successful activations matched to exactly one complete retirement and no source
drift (`target/tf-root-pressure-smoke-20261019T023136Z/`). The focused
300-second combined-outage and mixed-soak checks both passed with empty source
drift in `target/tf-root-pressure-soaks-20261019T023250Z/result.json`; detailed
process evidence is below. The next frozen 350-input full candidate at
`target/tf-systemd-full-20261019T023900Z/` under unit
`rbt-tf-full-20261019t023900z.service`, using source manifest SHA-256
`554b77ded7782c30876416021f5f25369290c7fad3d214d116931d5681531a5d`,
exited FAIL at 2026-10-19 03:51:55 UTC after 4,374.28 seconds, with no source
drift. Its initial TF checks, Python/asyncio full checks, 300-second RPC soak,
DX, runtime, Pressure and WaitSet stages passed. The inherited MCAP full stage
reported incomplete `/proc/332876/status` metrics during the 2.2-second large
pointcloud recording. That recording is complete (three messages, zero drops,
sequence gaps or writer errors), but this does not make the sampler gate pass.
No raw status snapshot was saved, so the exact historical transition is unproven.
Linux permits a task to retain a non-zombie state after its memory map is
detached during exit; the old sampler checked only the state letter. The
[kernel exit path](https://github.com/torvalds/linux/blob/master/kernel/exit.c)
sets `PF_EXITING` before detaching the memory map, and the
[proc status implementation](https://github.com/torvalds/linux/blob/master/fs/proc/array.c)
omits memory fields without that map. The repaired sampler reads the independent
task state and validated flags word from `/proc/PID/stat`, including
`PF_EXITING`, while retaining fatal errors for malformed metrics from a stable
live process. Root reviewed the two-file diff and independently passed shell
syntax, sampler regressions and the unchanged unsafe audit. A deterministic
old-source negative control reproduces the incomplete-metrics error after an
actual child exit; the repaired case skips that sample without writing a row.
Evidence is under `target/tf-mcap-exit-repair/`. The focused MCAP full run at
`target/tf-systemd-mcap-full-20261019T040440Z/` passed after 403.88 seconds, with
no source drift across 350 frozen inputs
and manifest SHA-256
`667784d52ec862b94a358a31bbbdf1e4c9525797128a2ee0568d9848a7aa8efd`.
Its 300-second record soak contains 17,837 messages, zero drops, sequence gaps
or writer errors. Recorder/source FDs remained 65/52 and threads 21/20; recorder
RSS was 18,772→19,012 KiB and source RSS 13,392→13,392 KiB. Its explicit
ancestor-skip flag avoids repeating unchanged inherited checks already executed
in the preceding candidate. The next complete TF full run at
`target/tf-systemd-full-20261019T041229Z/`, unit
`rbt-tf-full-20261019t041229z.service`, with the same 350-input manifest hash,
was interrupted at 2026-10-19 04:17:09 UTC after 279.66 seconds. Its atomic
result is INCOMPLETE, recording supervisor signal 15 rather than a failed test
assertion. Twenty-three top-level steps passed before interruption. The goal
subsequently became usage-limited. At that point, no complete full-gate PASS
existed and the release metadata had not yet been upgraded. Work resumed with a new
full run at `target/tf-systemd-full-20261019T163912Z/`, unit
`rbt-tf-full-20261019t163912z.service`, freezing the same 350 inputs with manifest
SHA-256 `667784d52ec862b94a358a31bbbdf1e4c9525797128a2ee0568d9848a7aa8efd`.
The 16:39Z run completed at 2026-10-19 18:55:18 UTC with exit 0; all 50/50
manifest labels passed, with no source drift across the 350 frozen inputs. Its
terminal result and independent acceptance summary are
`target/tf-systemd-full-20261019T163912Z/result.json` and
`target/tf-systemd-full-20261019T163912Z/root-acceptance-summary.json`. The
earlier 02:39Z failure did not reach the later TF matrix, packages, benchmark or
long process stages; no task-owned processes remain for that archived run. After
the software PASS, product/SDK metadata advanced to 0.8.0. Release artifact
verification subsequently passed all five checks at 2026-10-20 01:43:22 UTC,
with exit 0 and no source drift; see `target/tf-release-verification/run-20261020T012144Z-522975/result.json`. Hardware remains NOT RUN; no device has been selected or opened.
Synthetic fixtures are software evidence only.

The user approved a generated-marker exception on 2026-10-18: preserve the old
49 locations, permit exactly 24 generated TF type declarations, and prohibit new
hand-written unsafe. The shared auditor now reports legacy=49, tf_generated=24,
total=73 and checks the exact marker set. See [tf-unsafe-review.md](tf-unsafe-review.md).
Nine auditor tests pass, including rejection of unexpected locations and clean
checkout portability. The exception alone does not establish release acceptance.

## Remaining TODOs

No required software or release-verification work remains for v0.8. The
conditional physical-camera and physical dynamic-TF checks remain NOT RUN;
no device was selected. Generic recording/replay of TF messages remains
NOT VERIFIED. Parameters, simulation time, calibration solvers and networking
remain outside this milestone. Development of this milestone stops here.

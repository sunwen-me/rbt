# v0.7 Real-Hardware Time Foundations — acceptance report

## Decision / Status

**PASS — v0.7 software acceptance and the observed physical-camera gate.**
The final quick and full gates pass, including cross-language semantics and
transport, installed consumers, sanitizers, inherited regressions and a
300-second software time soak. The user-selected `/dev/video0` also passes
300 seconds of physical acquisition with Rust, C++ and Python consumers and
an observed V4L2 stream stop/close/reopen.

Physical USB unplug/replug and kernel driver detach/rebind are **NOT RUN**:
the selected camera is integrated and driver bind/unbind requires privileges
unavailable to this session. Stream reopen evidence is reported separately
and does not establish physical unplug behavior. No synthetic camera is used.

## Actual repository clock audit

The pre-implementation inventory is [time-audit.md](time-audit.md). The
historical RPC report used 0.9.3 at report time; current production already
uses maintained iceoryx2 0.10.0-rbt.2. No native source change is part of v0.7.
The checkout had no commits and all source files were untracked. Review uses
`target/time-baseline/source.tar` and its extracted tree, as well as Git status.

## Implemented time domains and timestamp source contracts

The design and public contracts are in [time.md](time.md). Host monotonic,
realtime and device clocks have explicit source/generation identity, signed
integer nanoseconds and checked arithmetic. Unknown legacy capture time stays
unknown. Capture and receive are independent; raw V4L2 event metadata remains
available even when capture cannot be established.

## Hardware clock model and identity/generation

Device clock IDs identify a source; reset advances the generation. Host
monotonic IDs include boot, current thread namespace and offset basis. Linux
thread offset files are read through `/proc/<TID>/timens_offsets`, not an
assumed task-directory file. Confirmed realtime steps advance the bound
observer generation and invalidate affected mappings explicitly; no global
step detector or synchronization authority is claimed.

## Clock mapping mathematics and uncertainty

Calibrations are explicit, immutable, bounded by source and real host-monotonic
validity intervals, and carry evidence IDs and known uncertainty. Mapping uses
checked signed-128 anchored rational arithmetic and unsigned-128 uncertainty
products. It rejects wrong domains, sources, generations, expiry, invalid
calibrations, unknown uncertainty and overflow. Full-width u64 uncertainty
products must agree across Rust, C++ and Python.

```text
target = target_anchor + trunc_toward_zero((source - source_anchor) * numerator / denominator)
uncertainty = ceil(input_uncertainty * numerator / denominator)
              + calibration_uncertainty + (1 ns if nominal conversion rounded)
```

The shared fixture converts source 103, anchored at 100→1000 with rate 3/2,
to 1004 ns; input uncertainty 2 plus calibration uncertainty 5 returns 9 ns.
Equal full source/target identities permit only matching anchors and equal
numerator/denominator. Calibration time uses an identified host-monotonic
interval with `calibrated_at <= now < expires_at`.

## Rust / C++ / Python APIs

Rust: `rbt::time` and `rbt::time_stream`; C++: `rbt/time.hpp` and
`rbt/time_stream.hpp`; Python: `rbt.time` and `rbt.time_stream`. All three
agree on the shared arithmetic/error fixture and host clock identity. All six
directed stamped-stream language pairs pass. External Rust and C++ consumers
use the installed SDK; Python uses a freshly installed 0.7.0 wheel outside the
repository with `PYTHONPATH` cleared. Python timing metadata and application
bytes retain native read-only sample ownership without a prefix copy.

## Existing message compatibility

The five frozen message types and MCAP canonical encoding are unchanged.
`rbt.time/StampedBytes@1` is a distinct new typed slice identity with layout
hash `97e9a924992dedfe83196197cccb6b35`. Its 192-byte timing prefix and application
bytes occupy one native sample. No Composer payload path, per-message service
call, unrelated timestamp sidecar or new unsafe implementation is introduced.

| Existing message | Preserved layout hash |
| --- | --- |
| Image | `943fa4008cf0f55f89a281b051c69a6f` |
| PointCloud | `d9da2bb29bb2cf217c40f1a56d1fb755` |
| ControlCommand | `9381cf15842a48ac0964111f0f45ef72` |
| Frame | `5d2c102a797d5b0ab64b4db8844297c0` |
| Detection | `d8e60d2ca65e6b5b74f0981d34884a4d` |

## CLI diagnostics

`rbt time status [--json] [--sources PATH]` reads local clocks directly and
accepts only an explicitly selected bounded source snapshot. The v0.5
checks/status/success contract is preserved. Known zero, unknown precision,
unavailable mapping, stale observation, expired mapping and invalidated mapping
remain distinct. The command does not infer synchronization from installed
software. CLI unit tests and the final quick/full JSON contract checks pass.
An end-to-end explicit snapshot tests known zero, unknown precision,
unavailable, expired, stale, wrong generation, future observation and same-host
comparison: all eight states pass (`target/time-cli-evidence/status.json`).

The full harness selects the C++ directory rebuilt by the inherited RPC gate,
preventing legacy soak scripts from reusing an unrelated stale executable.
Its external interop watchdog is 60 seconds for the complete 4096-call Debug
workload. All 4096 calls, response checks and final idle counters remain
required; individual RPC timeout/deadline code and probe settings are unchanged.
The final full gate passes all ten RPC directions and both 4096-call burst
directions, with final in-flight and response-guard counters zero.

## Evidence ledger

| Evidence | Status | Authoritative result |
| --- | --- | --- |
| Native C++ prefix provenance | PASS | Parent ran `scripts/check_iceoryx2_rbt_prefix.sh`; maintained 0.10.0-rbt.2 |
| Native source fingerprint | PASS | Parent ran `scripts/bootstrap_iceoryx2_rbt.sh --verify`; base `135d09dd8b29f321f1725920d434864c4e512378`, six patches, diff `b0e90730fee476487b0c47dba0a272b0d0df9c5480629f8ac64849b8882d91d1` |
| CLI targeted unit tests / strict Clippy | PASS | Parent ran `cargo test -p rbt-cli time_status --lib`; 5 passed; strict CLI Clippy passed after fixes |
| Rust/C++/Python time API component checks | PASS | Final quick gate: Rust unit/integration, C++ CTest, Python 24 core + 13 stamped tests |
| Cross-language math, errors and host identity | PASS | Final full `time-semantics-cross-language.log`: shared hand-calculated fixture, typed errors and equal host identity |
| Stamped Stream cross-language data | PASS | `target/time-interop-logs/interop-matrix-20261017T071224Z-3638947`; six directed pairs, sequence/timing/source/generation/uncertainty and payload checksum verified |
| Quick gate | PASS | `target/time-verify-logs/time-quick-20261017T035840Z-2947374`; workspace test/fmt/strict Clippy and inherited RPC quick passed against the final native prefix view |
| Full inherited regressions, packages, fresh wheel, sanitizers | PASS | `target/time-verify-logs/time-full-20261017T051438Z-3173039`; every root command step passes; inherited Rust/C++/Python, RPC, lifecycle, WaitSet, Pressure, MCAP, DX and packaging gates pass |
| C++ time ASan/UBSan and native interop | PASS | `target/time-interop-logs/interop-sanitizers-20261017T071817Z-3639892`; semantics and actual C++/Rust shared-memory pairs pass; inherited C++ Debug/Release/ASan/UBSan each pass 8/8 CTests |
| Python native ownership under ASan | PASS | Final inherited Python sanitizer run passes 50 tests, including all 13 stamped-stream tests |
| Installed time consumers | PASS | `target/time-interop-logs/interop-external-20261017T071828Z-3640007`; standalone Rust facade, C++ `find_package(rbt 0.7.0 EXACT)` and fresh Python wheel pass |
| 300-second software steady-time soak | PASS | `target/time-soak-logs/soak-20261017T071317Z-3639510/soak.log`; 300004 ms, 28980 iterations, no post-warmup RSS/FD/thread growth |
| Selected physical device | PASS | User selected `/dev/video0`; uvcvideo, Integrated RGB Camera, usb-0000:00:14.0-9; acquired YUYV 640×480, stride 1280 |
| 300-second physical acquisition | PASS | `target/time-verify-logs/time-hardware-20261017T035842Z-2948177`; physical acquisition 300030 ms, 2969 published frames; Rust/C++ 2969 each, Python 2968, all 2968 shared checksums equal |
| Observed stream close/reopen | PASS | Scheduled V4L2 stop/close/reopen at 150 seconds, session generations [0,1]; host-monotonic generation remains 0 |
| Physical USB unplug/replug / kernel detach | NOT RUN | Selected camera is integrated; sysfs uvcvideo bind/unbind is root-only and `sudo -n true` requires interactive authentication. No physical unplug or kernel detach/rebind is claimed |

## Resource cleanup

Physical-run evidence is retained in
`target/time-interop-logs/interop-hardware-20261017T035842Z-2948199/hardware/20261017T035842Z-d93e892c/summary.json`.
All four processes had zero post-warmup FD/thread growth and at most 600 KiB
RSS growth. Rust/C++ native borrowed counts and wrapper counts returned to
zero; Python's retained read-only view remained valid after sample-wrapper
release, then its borrow count returned to zero and subscriber closed. All
children were reaped, their PIDs absent and process groups empty. Python
received 2968 frames with zero between-sample sequence gaps; one initial frame
was missed before its subscription was ready. The Sensor stream retains its existing
pressure/drop semantics. No duplicate or out-of-order sequence was accepted.

The producer observed MONOTONIC+SOE capture for all 2969 frames, with unknown
physical precision preserved. Comparable capture-to-receive latency was
99905972–284222838 ns, mean 101150695 ns, with no negative/overflow samples.
The driver reported a 1/30-second frame interval; the observed acquisition
rate was approximately 10 fps. No actual 30 fps performance is claimed.
There were no backwards driver timestamp steps in this accepted run. An
earlier physical run observed raw sequence 627→0 with increasing host-based
timestamps; the example now records such continuity breaks, advances the
driver-session generation and leaves the cause unconfirmed. It never promotes
that observation into a host-clock reset or a physical unplug claim.

The full-gate software time soak runs for 300004 ms and 28980 iterations.
Warmup, peak and final RSS are all 4744 KiB; peak/final thread count is 1 and
FD count is 4. Each iteration checks actual host clock reads, expired and
invalidated mapping rejection, generation mismatch and bounded temporary
mapping creation/destruction. No new clock-source or mapping registry exists.

The inherited 300-second RPC soak also passes with 10 Composer restarts,
3 provider replacements, no resource-growth violations or orphan processes,
and zero final pending requests, in-flight calls and response guards
(`target/rpc-soak-logs/20261017T060055Z-3362938`). Inherited Component and
asyncio soaks, cancellation, process death, Ctrl-C and WaitSet wake tests pass.

## Version/hash/unsafe audit

Product version moves from 0.6.0 to 0.7.0. Frozen protocol and encoding axes
remain IDL 1, descriptor 1, Composer protocol 2, snapshot 4 and
`rbt-canonical-v1`. Final quick/full audits preserve all five historical hashes
and report `expected=49 observed=49 delta=0` for production unsafe. Maintained
native provenance passes. All 157 external Cargo.lock package entries are
unchanged, compared by name, version and source.

The 45 protected baseline files remain byte-identical, including old message
definitions/generated layouts, runtime/RPC/WaitSet/MCAP and maintained native
source. Evidence is `target/time-compatibility-audit.json`; the baseline archive
SHA-256 is
`de5acfdda2515acb05a27933fe3e61906c07c03b56020a5b41b00c3a37676f55`.

## Remaining TODOs and limits

Physical USB unplug/replug and privileged kernel detach/rebind remain
unverified. Capture precision remains unknown for the selected camera; its
documented driver source is preserved without inventing synchronization or an
exposure-accuracy guarantee. Tests use explicit discontinuity inputs without
changing the development host's system time; no universal clock-step detector
is claimed.

Simulation time, TF, Parameters and synchronization algorithms/daemons remain
future work. The seven simulation TODOs are listed in [time.md](time.md);
none is implemented. No required software gate remains pending. Optional
ShellCheck was unavailable in the inherited pressure script; shell syntax and
its required runtime tests pass.

## Final acceptance checklist

All 43 requested checklist entries are accepted against source review and the
final quick, full and hardware evidence below. The hardware entry explicitly
retains the physical-unplug NOT RUN boundary.

| Checklist evidence group | Inspected proof |
| --- | --- |
| Identity, checked arithmetic, mapping, uncertainty, reset and expiry | `rust-time-tests.log`, C++ `time_tests`, Python `test_time.py`, and the hand-calculated three-language fixture in the final quick run |
| Capture/receive, missing capture, legacy UNKNOWN and runtime separation | `rust-time-stream-tests.log`, including the pending RPC realtime-step case; Python stamped ownership tests; actual V4L2 flags and timing in the final hardware run |
| Frozen messages, runtime deadlines, RPC/WaitSet and MCAP source | `target/time-compatibility-audit.json`: 45 protected files unchanged; final quick/full hash, native, RPC, WaitSet and MCAP gates |
| Local conversion, bounded state and excluded features | Reviewed time cores, fixed-width prefix adapters and bounded probes; no Composer dependency in the clock core, registry, daemon, simulation, TF or Parameters implementation |
| Diagnostics | Final quick JSON contract plus the eight-state explicit source snapshot in `target/time-cli-evidence/status.json` |
| Native provenance and unsafe | Final quick/full fingerprint/prefix checks and `expected=49 observed=49 delta=0`; no external package changes in Cargo.lock |
| Full regressions, packaging and steady-time resources | Final full gate, installed Rust/C++/Python consumers, ASan/UBSan and the distinct 300004 ms software time soak |

- [x] Different clock domains cannot be silently compared.
- [x] Device identity is explicit.
- [x] Device reset invalidates old time generation.
- [x] Monotonic elapsed time remains separate from realtime.
- [x] Runtime deadlines remain unchanged.
- [x] Capture time is distinct from receive time.
- [x] Missing capture timestamps are not fabricated.
- [x] Unknown time source is represented honestly.
- [x] Clock mapping requires explicit calibration evidence.
- [x] Unmapped device time is rejected for cross-domain use.
- [x] Expired mapping is rejected.
- [x] Wrong-generation mapping is rejected.
- [x] Conversion arithmetic is overflow-safe.
- [x] Mapping uncertainty is preserved.
- [x] Rust time API passes.
- [x] C++ time API passes.
- [x] Python time API passes.
- [x] Cross-language semantics agree.
- [x] Existing Image ABI is unchanged.
- [x] Existing PointCloud ABI is unchanged.
- [x] Other frozen hashes are unchanged.
- [x] Existing MCAP format is unchanged.
- [x] Existing RPC semantics are unchanged.
- [x] No Composer payload path is introduced.
- [x] No per-message time service call is introduced.
- [x] No unbounded per-sample tracking is introduced.
- [x] Time diagnostics distinguish known/unknown/unavailable.
- [x] Structured JSON diagnostics pass.
- [x] Device reset test passes.
- [x] Clock-domain mismatch test passes.
- [x] Mapping expiry test passes.
- [x] Realtime discontinuity test passes.
- [x] 300-second software soak passes.
- [x] Real hardware gate reports actual evidence or NOT RUN.
- [x] Existing regression suites pass.
- [x] Production unsafe remains 49.
- [x] iceoryx2 maintained dependency provenance remains valid.
- [x] SimClock is NOT implemented.
- [x] Simulation pause/resume is NOT implemented.
- [x] Replay clock is NOT implemented.
- [x] TF is NOT implemented.
- [x] Parameters are NOT implemented.
- [x] No new time synchronization daemon is implemented.

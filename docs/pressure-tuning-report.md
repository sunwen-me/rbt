# v0.4 Pool & Pressure Observability / Tuning acceptance report

Status: **ACCEPTED** on 2026-09-24. The composed full verifier passed on the
Intel Core Ultra 5 125H acceptance host (18 logical CPUs, Ubuntu 26.04,
Linux 7.0.0-31-generic x86_64, rustc 1.98.1). The retained root log is
[`pressure-tuning-full-20260924T113241Z-12202`](../target/pressure-tuning-verify-logs/pressure-tuning-full-20260924T113241Z-12202/).

The product/SDK version is 0.4.0, the Composer control protocol remains v2,
the component snapshot schema is v3, and the transport dependency remains
iceoryx2 0.9.3. Payload IDL, SidecarV1, MCAP encoding, and payload ABI versions
are unchanged.

The milestone boundary is local bounded resource configuration and pressure
diagnostics. It contains no NUMA, huge-page, allocator, GPU/external-memory,
network-gateway, schema-evolution, or automatic-tuning work.

> rbt pressure semantics define what happens under resource contention;
> resource tuning only controls how much bounded capacity is provisioned
> before those semantics apply.

> Observability must explain pressure without changing pressure behavior.

## Frozen defaults and ABI

| Profile | Queue | History | Safe overflow | Max borrowed | Max loans | Result |
| --- | ---: | ---: | --- | ---: | ---: | --- |
| Sensor | 1 | 0 | true | 1 | 2 | unchanged |
| Control | 4 | 0 | false | 1 | 2 | unchanged |

Sensor still replaces stale queued data and never waits indefinitely for a
slow reader. Control still preserves its bounded non-overwrite behavior: one
send can reach some subscribers and return `QueueFull` for another; rbt does
not automatically retry that non-transactional multicast.

Resource metadata is carried by the stream descriptor but is excluded from
the message layout hash. The full verifier confirmed these frozen hashes:

| Message | Layout hash |
| --- | --- |
| Image | `943fa4008cf0f55f89a281b051c69a6f` |
| PointCloud | `d9da2bb29bb2cf217c40f1a56d1fb755` |
| ControlCommand | `9381cf15842a48ac0964111f0f45ef72` |

## Resource configuration contract

| Setting | Default | Valid range | Scope | Must match peer? | Runtime mutable? |
| --- | ---: | --- | --- | --- | --- |
| `max_loans` | 2 | positive native `usize`/`size_t` | publisher and advertised service ceiling | the publisher must match the descriptor | no; fixed at endpoint creation |
| `max_borrowed` | 1 | positive native `usize`/`size_t` | advertised service ceiling plus subscriber-local ceiling | subscriber request may be lower, never higher | no; fixed at endpoint creation |
| queue depth | profile value | profile-fixed | service semantics | yes, through the profile | no |
| history | 0 | profile-fixed | service semantics | yes, through the profile | no |
| safe overflow | profile value | profile-fixed | service semantics | yes, through the profile | no |

The Rust, C++, and Python APIs retain their simple default constructors/openers
and add explicit resource-aware variants. Zero values and incompatible
publisher/subscriber/service values return typed errors before transport open;
there is no panic and no partially opened endpoint. Resource settings remain
separate from `StreamProfile` and there is no live resize API.

The exact cross-language cases exercised by the Python interoperability module
were:

| Publisher | Subscriber | Advertised resources | Subscriber-local ceiling | Result |
| --- | --- | --- | --- | --- |
| Rust | C++ | `max_loans=4`, `max_borrowed=2` | 2 | PASS |
| C++ | Python | `max_loans=3`, `max_borrowed=2` | 2 | PASS |
| Python | Rust | `max_loans=4`, `max_borrowed=1` | 1 | PASS |

All three used the same explicit descriptor identity and completed payload
validation. The old command forms remain backward-compatible.

## Metric register

All cumulative counters and high-water values are keyed to one physical
`CapabilityInstanceId`. They saturate instead of wrapping and have no
automatic reset during that capability lifetime. Current gauges return to zero
when the final wrapper-owned guard releases.

| Name | Scope | Meaning | Unit | Lifetime / reset | Exactness |
| --- | --- | --- | --- | --- | --- |
| `loan_attempts` | publisher | calls that tried to obtain a loan | attempts | capability lifetime; no reset | exact wrapper calls |
| `loan_successes` | publisher | native loans obtained | loans | capability lifetime; no reset | exact |
| `loan_failures` | publisher | pool/out-of-memory loan failures | failures | capability lifetime; no reset | exact classified failures |
| `loans_in_use` | publisher | unsent loans currently owned by application code | loans | live gauge; zero after release | exact wrapper ownership |
| `loans_high_water` | publisher | maximum concurrent loans | loans | capability lifetime; no reset | exact high-water |
| `payload_bytes_last` | publisher | most recent successful loan size | bytes | latest successful loan | exact |
| `payload_bytes_max_seen` | publisher | largest successful loan | bytes | capability lifetime; no reset | exact observed maximum |
| `total_loaned` | publisher | cumulative successful-loan bytes | bytes | capability lifetime; no reset | exact until saturation |
| `send_attempts` | publisher | calls that tried to send a loan | attempts | capability lifetime; no reset | exact wrapper calls |
| `send_successes` | publisher | sends without transport error | sends | capability lifetime; no reset | exact API outcomes |
| `send_failures` | publisher | sends with a transport error | failures | capability lifetime; no reset | exact API outcomes |
| `queue_full` | publisher | Control sends failing because at least one queue was full | events | capability lifetime; no reset | exact classified outcomes |
| `unable_to_deliver` | publisher | other incomplete delivery failures | events | capability lifetime; no reset | exact classified outcomes |
| `readiness_notify_failures` | publisher | advisory Sidecar notification failures | events | capability lifetime; no reset | exact and separate from payload result |
| `subscriber_count` | publisher | latest aggregate transport subscriber count | subscribers | latest observation | aggregate, not identity preserving |
| `estimated_dropped_samples` | publisher/subscriber | sequence-aware drop estimate where one exists | samples | capability lifetime; no reset | approximate; null for generic wrappers |
| `receive_attempts` | subscriber | calls that tried to receive | attempts | capability lifetime; no reset | exact wrapper calls |
| `received_samples` | subscriber | successful receives | samples | capability lifetime; no reset | exact |
| `borrowed_samples` | subscriber | active retained sample guards | samples | live gauge; zero after release | exact wrapper ownership |
| `borrowed_high_water` | subscriber | maximum concurrent borrows | samples | capability lifetime; no reset | exact high-water |
| `borrow_failures` | subscriber | receive attempts rejected by the local borrow limit | failures | capability lifetime; no reset | exact |
| `borrow_duration_count` | subscriber | completed timed borrow lifetimes | borrows | capability lifetime; no reset | exact |
| `borrow_duration_total_ns` | subscriber | total completed borrow duration | ns | capability lifetime; no reset | monotonic clock; exact until saturation |
| `borrow_duration_max_ns` | subscriber | longest completed borrow | ns | capability lifetime; no reset | monotonic clock |
| `oldest_borrow_age_ns` | subscriber | age of the oldest active borrow | ns | current borrows; null when none | exact bounded scan |
| `last_pressure_event` | endpoint | latest typed pressure transition | typed event | capability lifetime; replaced by newer event | exact observed transition |
| `snapshot_timestamp_unix_ms` | snapshot | snapshot wall-clock timestamp | Unix ms | latest snapshot | exact metadata |
| freshness `age_ms` | CLI/Composer | saturating inspection-time snapshot age | ms | latest inspection | exact derived age |
| freshness `fresh` | CLI/Composer | whether `age_ms <= 2500` | boolean | latest inspection | derived display label, not health |

The generic wrapper cannot observe exact Sensor overwrite counts or per-reader
transport queue occupancy. Those fields remain unknown/null instead of
fabricating zero. Low-rate snapshots are control-plane observations; Composer
does not receive one event for every loan or borrow, and pressure updates do
not increment composition revisions or trigger reconciliation.

## Ownership state diagrams

```text
Rust publisher
  loan() -> LoanTracker -> send / error / Drop
                       -> decrement loans_in_use exactly once

Rust subscriber
  receive() -> BorrowTracker -> sample guard Drop
                           -> decrement borrowed_samples and record duration

C++ publisher
  move-only LoanedSample -> send / destructor -> release loan owner once

C++ subscriber
  move-only ReceivedSample -> destructor -> release borrow owner once

Python receive
  NumPy alias / memoryview
    -> native PyO3 owner
      -> ReceivedSliceSample
        -> iceoryx2 guard + BorrowTracker

Python publish
  writable NumPy aliases
    -> Active / Mapped / Consumed loan state
      -> LoanedSliceSample
        -> iceoryx2 loan + LoanTracker
```

A derived NumPy view keeps the native owner alive after the original array is
deleted. Cross-thread final release updates the original physical metric owner.
A writable publisher alias keeps the loan mapped, so `send()` returns
`LoanStillMapped` without consuming or undercounting the loan; final alias
release restores capacity.

The historical-instance test preserves this sequence:

```text
sample borrowed from capability A
  -> A is replaced and capability B becomes active with clean counters
  -> old sample remains owned by A
  -> releasing the sample decrements A only
  -> A is reclaimed after its final owner disappears; B is unchanged
```

The Rust identity test, mixed-runtime provider replacement, and the 100-cycle
Python component run all passed this invariant. The runtime evidence reported
distinct old/new capability IDs, continued counters across Composer restart,
and `retired_state_removed=true`.

## Exact pressure and large-payload evidence

The retained semantics log is
[`pressure-tests-1.log`](../target/pressure-logs/pressure.nN4D22/pressure-tests-1.log).
Every capacity row fully initialized and validated the payload, deliberately
exhausted the configured pool, asserted exact counters/high-water, recovered,
and ended with current ownership at zero.

| Workload | Max loans | Payload | Elapsed ms | CPU ticks | RSS before/after/peak bytes | FD before/after/peak | Threads before/after/peak |
| --- | ---: | ---: | ---: | ---: | --- | --- | --- |
| 1080p RGB8, 30 frames at 30 Hz | 2 | 6,220,800 | 2571 | 257 | 9,723,904 / 10,276,864 / 16,670,720 | 4 / 4 / 30 | 2 / 2 / 2 |
| 1080p RGB8, 30 frames at 30 Hz | 3 | 6,220,800 | 2637 | 264 | 10,276,864 / 10,276,864 / 16,674,816 | 4 / 4 / 30 | 2 / 2 / 2 |
| 1080p RGB8, 30 frames at 30 Hz | 4 | 6,220,800 | 2598 | 260 | 10,276,864 / 10,276,864 / 16,678,912 | 4 / 4 / 30 | 2 / 2 / 2 |
| PointCloud, 1,310,720 XYZI points | 2 | 20,971,520 | 89 | 9 | 8,376,320 / 9,293,824 / 51,281,920 | 4 / 4 / 30 | 2 / 2 / 2 |
| PointCloud, 1,310,720 XYZI points | 3 | 20,971,520 | 85 | 8 | 9,293,824 / 9,326,592 / 51,318,784 | 4 / 4 / 30 | 2 / 2 / 2 |
| PointCloud, 1,310,720 XYZI points | 4 | 20,971,520 | 85 | 9 | 9,326,592 / 9,363,456 / 51,359,744 | 4 / 4 / 30 | 2 / 2 / 2 |

The deterministic Rust Control recovery workload ran 1,000 cycles and asserted:

```text
loan_attempts=4000 loan_successes=3000 loan_failures=1000
loans_high_water=2 loans_in_use=0
send_attempts=1000 send_successes=1000
receive_attempts=2000 received_samples=1000 borrow_failures=1000
borrowed_high_water=1 borrow_duration_count=1000 borrowed_samples=0
```

The C++ ownership test independently ran 1,000 loan-exhaustion and 1,000
borrow-limit recovery cycles. It asserted `loan_attempts=2001`,
`loan_successes=1001`, `loan_failures=1000`, `borrow_attempts=3000`,
`borrow_successes=2000`, `borrow_failures=1000`, and final current gauges zero.
Debug, release, ASan, and UBSan executions passed.

Python retained a derived receive view for more than two seconds and reported
`borrowed_samples=1`, `borrowed_high_water=1`, and
`oldest_borrow_age_ns=2119923821`; final release reported
`borrowed_samples=0` and no oldest borrow. GC storms, cross-thread final release,
writable-loan aliases, a 2,000-iteration send/alias race, and cancelled waiting
receives also passed.

## Soak and resource evidence

The primary mixed graph was:

```text
Rust camera / Image
  -> Python detector
    -> PointCloud/Detections
      -> native C++ viewer

MCAP recorder: rbt record --all
Composer: stopped and restarted ten times
```

The retained runtime directory is
[`runtime-full.Ghz9kz`](../target/python-component-verify-logs/python-component-full-20260924T115307Z-55529/fresh-wheel-runtime/runtime-full.Ghz9kz/).
It ran 302 seconds. The Python image consumer and native point-cloud consumer
each advanced by 3,025 samples. MCAP recorded 6,000 messages on exactly two
channels over 299.902 seconds with zero transport gaps, recorder drops, and
writer errors; queue high-water was 104 bytes against a 67,108,864-byte budget.

| Role | RSS initial/final/max KiB | FD initial/final | Threads initial/final | Measured samples |
| --- | --- | --- | --- | ---: |
| camera | 12,540 / 12,584 / 12,588 | 25 / 23 | 19 / 19 | 55 |
| Python detector | 43,320 / 43,340 / 43,344 | 58 / 56 | 38 / 38 | 55 |
| native viewer | 13,772 / 13,772 / 13,772 | 28 / 28 | 3 / 3 | 55 |
| recorder | 14,276 / 14,468 / 14,468 | 47 / 47 | 21 / 21 | 54 |
| Composer restarts | 8,392 / 8,352 / 8,464 | 13 / 13 | 19 / 19 | 65 |

Across 56 pressure snapshots, publisher loan high-water and subscriber borrow
high-water were both 1; loan failures, send failures, and borrow failures were
all zero. Control was not present in this graph. Final current loans and borrows
were both zero. After shutdown, detector/viewer components, stream-pressure
records, detections topology, and subscriber observations were all reclaimed.

Before the soak, 100 provider activation/rebind cycles completed with one
intentionally retained old detector borrow and no other retained ownership:
`activate=100`, detector `borrowed_current=1`, detector pressure streams 1,
FD 19→19, threads 38→38, RSS 41,652→43,248 KiB. Final release reclaimed the
component and all five associated observation categories to zero.

The separate pressure soak is
[`pressure.nN4D22`](../target/pressure-logs/pressure.nN4D22/). It ran 333
seconds and five complete test/Sensor/Control/graph cycles. Fast readers
continued, slow readers showed bounded sequence gaps, held/stalled readers were
SIGKILLed and replaced, Control queue-full behavior was observed, publishers
and subscribers recovered after process death, provider identities rotated,
Composer restarts caused no activation churn, and no orphan capability or stale
binding remained. Its 55 resource samples showed no obvious unbounded growth;
final loans and borrows were zero.

The inherited MCAP soak recorded 17,870 messages on three channels over
299.960 seconds with zero transport gaps, recorder drops, or writer errors.
Queue high-water was 256 bytes of 64 MiB. Recorder RSS was
14,768→15,008 KiB (max 15,008), FD 60→60, threads 21→21; source RSS was
12,908→12,908 KiB, FD 49→49, threads 20→20.

## Performance and instrumentation boundary

The accepted 20-row instrumented baseline is in
[`docs/benchmarks.md`](benchmarks.md) and its retained raw log is
[`pressure-benchmark-matrix.log`](../target/pressure-tuning-verify-logs/pressure-tuning-full-20260924T113241Z-12202/pressure-benchmark-matrix.log).
Every fast subscriber validated the full payload, received all 20 samples, and
reported zero gaps; every publisher observed the requested fanout on every
measured send.

Fanout-one representatives show the five required payload sizes:

| Payload | Effective interval us | Publisher Hz / CPU s / RSS MiB | Subscriber Hz / CPU s / RSS MiB |
| --- | ---: | --- | --- |
| 64 B Image | 1,000 | 907.271 / 0.004527 / 5.50 | 943.844 / 0.006519 / 5.96 |
| 1 KiB Image | 1,000 | 907.968 / 0.004140 / 5.47 | 939.498 / 0.006470 / 6.03 |
| 1 MiB Image | 41,944 | 23.527 / 0.015001 / 6.47 | 24.784 / 0.072449 / 6.96 |
| 6.22 MiB Image | 248,832 | 3.981 / 0.060549 / 11.38 | 4.190 / 0.323464 / 11.86 |
| 20 MiB PointCloud | 838,861 | 1.187 / 0.099394 / 25.48 | 1.249 / 0.992739 / 26.00 |

The successful path adds relaxed saturating counter/gauge/high-water updates
and a bounded ownership guard. It adds no per-sample heap allocation or mutex.
Only pressure-transition/event recording takes its bounded event mutex;
snapshots are low-rate aggregates.

There is no retained pre-instrumentation v0.4 binary built with the same
compiler, WaitSet, and harness, so a paired before/after overhead ratio is not
available. The table above is the accepted instrumented baseline, not an
overhead percentage. Historical v0.1 and WaitSet measurements are same-project
context only and are not an uninstrumented control. No cross-framework claim
is made, and accounting correctness was not weakened to improve a benchmark.

## Verification evidence register

| Gate | Result | Retained evidence |
| --- | --- | --- |
| formatting, workspace build/tests, focused Rust pressure/fault/CLI/Composer | PASS | [root full logs](../target/pressure-tuning-verify-logs/pressure-tuning-full-20260924T113241Z-12202/) |
| Rust pressure semantics | 14/14 PASS | [`rust-iox2-pressure.log`](../target/pressure-tuning-verify-logs/pressure-tuning-full-20260924T113241Z-12202/rust-iox2-pressure.log) and [nocapture soak evidence](../target/pressure-logs/pressure.nN4D22/pressure-tests-1.log) |
| Rust process-death fault tests | 4/4 PASS | [`rust-iox2-fault.log`](../target/pressure-tuning-verify-logs/pressure-tuning-full-20260924T113241Z-12202/rust-iox2-fault.log) |
| C++ debug/release API and ownership tests | 6/6 PASS in each build | [`cpp-ctest.log`](../target/pressure-tuning-verify-logs/pressure-tuning-full-20260924T113241Z-12202/cpp-ctest.log) |
| Python full tests | 88 PASS | [`python-pytest-full.log`](../target/python-publisher-verify-logs/python-publisher-full-20260924T115021Z-52998/base/python-full-20260924T115021Z-53005/python-pytest-full.log) |
| Python fresh wheel and publisher interop | PASS | [`python-publisher-full-20260924T115021Z-52998`](../target/python-publisher-verify-logs/python-publisher-full-20260924T115021Z-52998/) |
| Python component 100 cycles and 302 s mixed graph | PASS | [`python-component-full-20260924T115307Z-55529`](../target/python-component-verify-logs/python-component-full-20260924T115307Z-55529/) |
| Python asyncio 100 cycles, 300 s graph, cancellation and idle checks | PASS | [`python-asyncio-full-20260924T120003Z-91065`](../target/python-asyncio-verify-logs/python-asyncio-full-20260924T120003Z-91065/) |
| external Rust/C++ consumers, packaging, smoke runtime, v0.1 regression | PASS | [`verify-full-20260924T113450Z-14760`](../target/v01-logs/verify-full-20260924T113450Z-14760/) |
| C++ ASan/UBSan | 6/6 PASS | [`inherited-v01.log`](../target/pressure-tuning-verify-logs/pressure-tuning-full-20260924T113241Z-12202/inherited-v01.log) |
| WaitSet tests, fairness, lifecycle, benchmark, idle and pressure soak | PASS | [`waitset-full-20260924T113841Z-29772`](../target/waitset-verify-logs/waitset-full-20260924T113841Z-29772/) |
| MCAP unit/live/overload/outage/replay/300 s record soak | PASS | [`mcap-full-20260924T114333Z-42408`](../target/mcap-verify-logs/mcap-full-20260924T114333Z-42408/) |
| Rust↔C++ mixed Sensor pressure | PASS | [`mixed-language-pressure.log`](../target/pressure-tuning-verify-logs/pressure-tuning-full-20260924T113241Z-12202/mixed-language-pressure.log) |
| standalone 333 s pressure soak | PASS | [`pressure.nN4D22`](../target/pressure-logs/pressure.nN4D22/) |
| 20-row pressure benchmark and mixed-speed case | PASS | [`pressure-benchmark-matrix.log`](../target/pressure-tuning-verify-logs/pressure-tuning-full-20260924T113241Z-12202/pressure-benchmark-matrix.log) |
| frozen hashes and production unsafe baseline | PASS; unsafe=49 | [`layout-hash-rust-tests.log`](../target/pressure-tuning-verify-logs/pressure-tuning-full-20260924T113241Z-12202/layout-hash-rust-tests.log), [`unsafe-audit.log`](../target/pressure-tuning-verify-logs/pressure-tuning-full-20260924T113241Z-12202/unsafe-audit.log) |
| shell syntax | PASS | full verifier and final documentation audit |
| shellcheck | unavailable | command is not installed on the acceptance host |

The supported entry points are:

```bash
./scripts/verify_pressure_tuning.sh --quick
./scripts/verify_pressure_tuning.sh --full
```

## Final 60-item acceptance audit

| # | Acceptance question | Result and evidence |
| ---: | --- | --- |
| 1 | Sensor defaults unchanged? | PASS — frozen-default table and hash/profile tests |
| 2 | Control defaults unchanged? | PASS — frozen-default table and queue tests |
| 3 | Queue/history/overflow semantics unchanged? | PASS — Sensor/Control live and unit tests |
| 4 | Explicit `max_loans`? | PASS — all three SDKs and 2/3/4 matrices |
| 5 | Explicit `max_borrowed`? | PASS — all three SDKs and interop cases |
| 6 | Resources separate from `StreamProfile`? | PASS — descriptor/resource API contract |
| 7 | Invalid configs return typed errors? | PASS — Rust/C++/Python validation suites |
| 8 | Layout hashes unchanged? | PASS — exact three-hash gate |
| 9 | Resource config excluded from payload ABI? | PASS — IDL/hash regression tests |
| 10 | Loan attempts observable? | PASS — exact Rust/C++ counters |
| 11 | `LoanPoolExhausted` countable? | PASS — repeated 1,000-cycle tests |
| 12 | Current loans observable? | PASS — exact gauge and final zero |
| 13 | Loan high-water observable? | PASS — exact high-water assertions |
| 14 | Current borrows observable? | PASS — Rust/C++/Python retained owners |
| 15 | Borrow high-water observable? | PASS — exact high-water assertions |
| 16 | `BorrowLimitReached` countable? | PASS — repeated 1,000-cycle tests |
| 17 | Borrow lifetime measurable? | PASS — count/total/max duration assertions |
| 18 | Oldest active borrow diagnosable? | PASS — Python 2.119923821 s evidence |
| 19 | Historical tracking bounded? | PASS — bounded owner set and reclamation tests |
| 20 | ndarray retention keeps accounting alive? | PASS — retained-array tests |
| 21 | Derived NumPy views preserve accounting? | PASS — derived-view test |
| 22 | Cross-thread final Python release? | PASS — full Python suite |
| 23 | Writable loan alias preserves accounting? | PASS — publisher ownership suite |
| 24 | `LoanStillMapped` accounting correct? | PASS — alias/send tests |
| 25 | Send success/error consumes once? | PASS — Rust/C++/Python RAII tests |
| 26 | Unsent Drop releases once? | PASS — ownership and cancellation probes |
| 27 | C++ move-only ownership correct? | PASS — debug/release/sanitizer tests |
| 28 | Async receive uses identical accounting? | PASS — full asyncio gate |
| 29 | Cancelled waiting receive creates no false borrow? | PASS — cancellation metric test |
| 30 | Counters keyed to `CapabilityInstanceId`? | PASS — identity rotation tests |
| 31 | Old retained sample isolated from new capability? | PASS — A/B historical-instance test |
| 32 | Old metrics reclaimed after final owner? | PASS — `retired_state_removed=true` and final reclaim |
| 33 | Metrics continue during Composer outage? | PASS — restart/outage tests |
| 34 | Composer restart avoids activation churn? | PASS — zero recovery activate/deactivate |
| 35 | Snapshots low-frequency? | PASS — periodic snapshot path |
| 36 | Metrics avoid the payload path through Composer? | PASS — aggregate snapshots only |
| 37 | CLI shows effective resources? | PASS — human/JSON CLI tests |
| 38 | CLI shows pressure counters? | PASS — human/JSON CLI tests |
| 39 | CLI distinguishes stale/unavailable from zero? | PASS — freshness/unavailable tests |
| 40 | JSON exposes structured diagnostics? | PASS — schema and reason-code tests |
| 41 | Long-held samples identifiable? | PASS — age and `outstanding_borrow` diagnosis |
| 42 | Repeated pool exhaustion identifiable? | PASS — `loan_capacity_reached` diagnosis |
| 43 | Memory implications visible? | PASS — payload/resource estimates and warnings |
| 44 | No automatic tuning? | PASS — recommendation-only workflow |
| 45 | Fast+slow Sensor behavior correct? | PASS — mixed live and benchmark cases |
| 46 | MCAP pressure remains separate? | PASS — recorder counters stay separate |
| 47 | Repeated exhaustion recovers? | PASS — Rust/C++ 1,000-cycle tests |
| 48 | 1080p Image correct? | PASS — full initialization/validation matrix |
| 49 | 20 MiB PointCloud correct? | PASS — full XYZI validation/checksum matrix |
| 50 | RSS/FD/thread counts stable? | PASS — mixed/MCAP/pressure resource tables |
| 51 | Current ownership ends at zero? | PASS — all cleanup summaries |
| 52 | 100 activation cycles avoid leaks? | PASS — component and asyncio gates |
| 53 | Provider replacement gets clean metrics? | PASS — distinct A/B identities and reset |
| 54 | Inherited Rust/C++/Python/WaitSet/MCAP gates? | PASS — composed root verifier |
| 55 | ASan/UBSan? | PASS — all six C++ tests |
| 56 | Fresh wheel? | PASS — install, import, receive, publish, component, asyncio |
| 57 | Production unsafe remains 49? | PASS — exact audit |
| 58 | No allocator/NUMA/GPU work? | PASS — milestone boundary respected |
| 59 | No network/schema work? | PASS — protocol/ABI boundary respected |
| 60 | Pressure diagnosable without semantic change? | PASS — typed metrics, CLI, JSON, docs, and unchanged runtime gates |

The success criterion is explanation, not suppression: when
`LoanPoolExhausted` or `BorrowLimitReached` occurs, rbt identifies the bounded
resource, the observed workload path, and the explicit memory/capacity tradeoff
the user may choose. It does not resize or retry automatically.

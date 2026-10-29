# WaitSet milestone report

This is the concise final report for the event-driven receive milestone on the
recorded Linux host. The API remains experimental/internal; the measurements
below are evidence for the stated method, not an SLA or a universal
performance claim.

## Architecture and behavioral changes

iceoryx2 `0.9.3` publish/subscribe payload subscribers are not waitable:
`Subscriber` exposes `receive()` but not `FileDescriptorBased` or
`SynchronousMultiplexing`. Each stream therefore has an event sidecar named
`<payload>/ready`. The sidecar wakes the WaitSet only; the payload still uses
the existing publisher loan/send and subscriber receive/borrow path. A
`ReadyEvent` carries only a stable rbt `WaitToken`, never a sample or payload
metadata.

The subscriber listener is created before the payload endpoint. Each wait loop
performs one synthetic initial drain before its first blocking wait. A source
handler drains at most `MAX_DRAIN_PER_SOURCE = 64` receives before returning
to the WaitSet. Empty or spurious readiness is safe: `receive() -> None` is
accounted for and is not a failure; stale tokens are ignored.

Cancellation is scoped to both `ProcessInstanceId` and `ActivationId`. The
normal Rust notifier or C++ `std::stop_callback` bridge wakes immediately. An
internal one-second WaitSet liveness interval is only a failed-notifier
fallback: it checks cancellation state and never polls payload or calls
`receive()`. Cancellation always wakes before the blocking task is joined.
Rust and C++ bridge both `SIGINT` and `SIGTERM` into this graceful
wake-before-join path.

Payload ABI, payload QoS (queue/history/overflow/loan/borrow policy), and
lifecycle/capability semantics are unchanged. A WaitSet-upgraded subscriber
must pair with a WaitSet-upgraded publisher that emits the matching sidecar
notification. This is a wake-path deployment requirement, not a payload ABI
or QoS change. Readiness notification is a new typed post-send error
(`StreamError::ReadinessNotify`) when the payload send completed; the sample
may already have been delivered. For a non-transactional payload send error,
rbt preserves the original send error because delivery may already have
occurred.

Rust implements this in `crates/rbt-iox2/src/wait.rs` and integrates one
activation-owned blocking loop with `EffectScope`. C++ implements the same
model directly with the iceoryx2 C++ API in `sdk/cpp/include/rbt/wait.hpp`;
there is no Rust FFI wait path and no detached forwarding thread. Detector,
viewer, interop, pressure, and benchmark consumers in both languages now use
the thin wait abstraction.

The payload path is still:

```text
publisher loaned SHM
-> iceoryx2 publish/subscribe
-> readiness-only sidecar wake
-> subscriber.receive()
-> borrowed SHM sample
-> in-place application processing
```

There is no intermediate `Vec`, serializer, callback copy, Tokio payload
channel, or Composer payload hop. The distinction between upstream documented
behavior, source-observed behavior, rbt policy, and local observations is
recorded in [`waitset-notes.md`](waitset-notes.md).

## Verification commands

```bash
./scripts/verify_waitset.sh --quick
./scripts/verify_waitset.sh --full
RBT_WAITSET_PRESSURE_FULL=1 ./scripts/verify_waitset.sh --full
./scripts/benchmark_waitset.sh --smoke
./scripts/soak_waitset_idle.sh --smoke
```

The current CMake suite has six tests: `descriptor_tests`, `stream_compile`,
`pressure_tests`, `effect_scope_tests`, `waitset_tests`, and `runtime_tests`.
The `--full` WaitSet gate runs the full benchmark matrix; `--smoke` is the
short standalone benchmark option.
The final unified gate passed at
`target/waitset-verify-logs/waitset-full-20260910T234120Z-98790` with the
300-second pressure option enabled. Its nested fresh ASan/UBSan/leak run passed
all six C++ tests, including `waitset_tests`; the retained sanitizer evidence is
`target/v01-logs/verify-full-20260910T234126Z-99689/sanitizer-ctest.log`.

## Polling audit

Short-cycle empty-receive polling was removed from the active Rust and C++
detector/viewer paths, C++ interop consumer, Phase 4 runtime consumer,
advanced image consumer, mixed-pressure subscriber, and benchmark subscriber.
Those paths now block on a native WaitSet and call `receive()` only from the
synthetic first pass or a readiness dispatch.

The remaining sleeps and intervals are intentional and outside local payload
readiness: publisher rate pacing, bounded open/discovery/send retries,
test-only slow/held-sample behavior, Composer TCP reconnect backoff, lifecycle
control loops, low-rate pressure snapshots, and shell watchdogs. Direct
receive polling remains only in transport-level tests that deliberately test
the underlying stream API rather than WaitSet. The internal one-second
cancellation-liveness interval checks only the cancellation flag and never
calls payload `receive()`; it is not a data-plane polling fallback.

### Full benchmark evidence

[`target/waitset-benchmark-full-final.log`](../target/waitset-benchmark-full-final.log)
run `waitset-20260910T183644Z-66797` used fixed order `polling,waitset`, a 10 s
idle case, 500 latency samples at 100 Hz, and the full payload/fanout matrix.

| 10 s idle metric | Polling | WaitSet |
| --- | ---: | ---: |
| User CPU (s) | .06 | 0 |
| System CPU (s) | .13 | 0 |
| RSS (KiB) | 5284 | 6064 |
| Threads | 1 | 2 |
| FDs | 12 | 27 |

64 B latency p50/p90/p99/max was
`80.718/144.287/155.237/max267.771 us` for polling and
`36.190/104.933/153.240/max188.429 us` for WaitSet.

Loaded WaitSet/polling ratios were:

- publish rate: average `.984`, minimum `.933`, maximum `1.022`;
- receive rate: average `.979`, minimum `.928`, maximum `1.032`;
- subscriber CPU: average `.320`, range `.140-.572`;
- subscriber RSS: average `1.151`, range `1.125-1.175`;
- p99 latency: average ratio `.767`; worst ratio `1.832` at 1 MiB/fanout 1,
  with absolute WaitSet p99 `278.745 us`.

The fixed order and single recorded environment are material caveats. These
numbers carry no SLA significance and do not support a universal performance
claim. They do show the required real idle improvement on this run: combined
recorded CPU fell from `.19 s` to `0`, and voluntary context switches fell
from `65133` to `10`. The tradeoff was one additional process thread, 15 file
descriptors, and `780 KiB` more RSS.

The loaded matrix shows no broad throughput collapse or unexplained severe
regression: the worst publish/receive ratios were `.933/.928`, while subscriber
CPU was lower on every summarized ratio. The isolated `1.832` p99 ratio is
reported rather than hidden; its absolute value was `278.745 us`, and the
fixed strategy order and single short run make scheduler/order effects a
material limitation.

The accepted unified gate repeated the complete matrix in
[`benchmark-full.log`](../target/waitset-verify-logs/waitset-full-20260910T234120Z-98790/benchmark-full.log).
It again measured polling/WaitSet idle CPU at `.20/0 s`. Its independent 64 B
latency p50/p90/p99/max was
`79.585/142.864/157.148/242.067 us` for polling and
`99.673/131.452/165.081/188.988 us` for WaitSet. Across the loaded matrix, the
WaitSet/polling average/minimum ratios were `.989/.919` for publish rate and
`.982/.899` for receive rate; subscriber CPU averaged `.317`, while p99
latency averaged `.867`. The largest individual p99 ratio was `4.377` for
64 B/fanout 8/subscriber 7, but the absolute WaitSet p99 was `676.618 us`, the
case completed without loss or starvation, and the earlier complete run did
not reproduce it. This isolated sub-millisecond tail outlier is disclosed as
fixed-order host-scheduling/fanout variance; it is not treated as a throughput
collapse or an SLA claim.

## Fairness table

[`target/waitset-stats-semantics-final.log`](../target/waitset-stats-semantics-final.log)
reported the following high/medium/low source results:

| Source | Nominal publish rate | Sent | Processed | Max scheduling gap | Max processing gap (us) | Starved? |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| High | 1000 Hz | 441 | 440 | 2 | 1115 | No |
| Medium | 100 Hz | 44 | 44 | 11 | 10166 | No |
| Low/Control | 10 Hz | 5 | 5 | 112 | 100060 | No |

The same run recorded budget `64`, `490` wakeups, `0` empty wakeups, and
`148 us` cancellation latency. Sensor overflow means exact high-rate delivery
is not required; the medium and low sources continued to receive bounded
processing opportunities, and cancellation remained responsive under the
high-rate source.

## Cancellation evidence

[`target/waitset-focused-metrics-final.log`](../target/waitset-focused-metrics-final.log)
recorded 100 activation cycles in `328 ms`, FDs `30->30`, tasks `2->2`, and
RSS `9244->9252 KiB`. Its critical cancellation timing was deactivation
request `13 ms`, wake `13 ms`, wait exit `13 ms`, task exit `13 ms`, scope
completion `14 ms`, and shutdown `0 ms`.

[`target/waitset-mixed-signal-final.log`](../target/waitset-mixed-signal-final.log)
recorded blocked C++ `SIGTERM` handling in `130 ms` with status `0` and blocked
Rust shutdown in `10 ms` with status `0`; provider replacement and Composer
restart/recovery also passed. In its idle-provider case, the C++ detector had
already been idle for `1000 ms` and reached `BLOCKED` `120 ms` after provider
kill. That mixed run did not expose a separate internal wake timestamp, so the
report does not fabricate one; the isolated wait test above measured the same
notifier path at `148 us`, while `120 ms` is the real end-to-end
provider-kill-to-`BLOCKED` result.

The critical-sibling test woke the blocked wait at `13 ms`, observed wait and
task exit at `13 ms`, completed the scope at `14 ms`, removed the activation's
capability through the existing failure path, and left no blocked task.

## Explicit regression PASS list

- Fairness and wait statistics: sent `[441,44,5]`, processed `[440,44,5]`,
  with `0` empty wakeups (stats log above).
- Attachment/lifecycle regression: 100-cycle resource result remained stable
  at FDs `30->30`, tasks `2->2`, RSS `9244->9252 KiB` (focused log above).
- Idle soak: [`idle-soak-bounded.log`](../target/waitset-verify-logs/waitset-full-20260910T234120Z-98790/idle-soak-bounded.log)
  passed after 45 s with CPU `0`, RSS `6104->6104 KiB`, FDs `27->27`,
  threads `2->2`, and no unbounded growth.
- Mixed Rust/C++ signals and recovery: mixed-signal log above passed both
  blocked shutdowns plus provider replacement and Composer restart/recovery.
- Pressure soak: [`pressure-soak-300s.log`](../target/waitset-verify-logs/waitset-full-20260910T234120Z-98790/pressure-soak-300s.log)
  passed in `328 s`, `7` cycles, `84` resource samples, with orphan
  capabilities `0`, stale bindings `0`, no obvious growth, and `PASS`.
- Sensor multi-subscriber isolation remained intact: fast/medium/slow/stalled
  roles, bounded backlog, slow-reader gaps, and continuing publishers passed
  during every live pressure cycle.
- Control depth-4, `safe_overflow=false`, fifth-send `QueueFull`, and
  non-transactional partial delivery passed. The C++ WaitSet fixed-payload
  tests now use the Control profile rather than Sensor defaults.
- Borrow retention/`BorrowLimitReached` and release recovery passed with
  WaitSet active; loan #3 `LoanPoolExhausted` and drop/reloan recovery also
  passed. Readiness never prefetches or holds a payload sample.
- Provider replacement preserved prepare-once and activation `1 -> 2`;
  Composer restart reconstructed the graph with `0` deactivate and `0`
  activate while payload continued.
- The Rust Camera -> C++ WaitSet Detector -> Rust WaitSet Viewer graph,
  fixed and slice ABI interop, Phase 1-4 layout checks, deterministic IDL,
  CLI checks, and clean-prefix installed Rust/C++ consumers passed. The
  installed consumers compile the public experimental WaitSet surface.
- Production Rust unsafe locations remain `31`, delta `0`; `wait.rs`,
  `effect_scope.rs`, and `component.rs` contain no `unsafe` token.

The older `target/waitset-mixed-runtime-rerun.log` contains a pre-fix
status-143 shutdown failure. It is retained as development history, not final
evidence; `target/waitset-mixed-signal-final.log` is the later passing result
after the SIGTERM bridge was implemented.

## Breaking and observable changes

There is no payload ABI, message layout/hash, Sensor/Control QoS, loan/borrow,
capability, activation, or Composer recovery break. Observable changes are:

- upgraded WaitSet subscribers require an upgraded publisher that provides
  `<payload>/ready`;
- a successful payload send can now return typed `ReadinessNotify` if its
  subsequent advisory wake fails, and the sample may already be delivered;
- receive scheduling uses a synthetic first pass and an internal maximum of
  64 receive attempts per ready source before returning to WaitSet;
- component `SIGINT`/`SIGTERM` now performs graceful cancellation, join, and
  cleanup with status `0` instead of default signal termination.

## Remaining work

After final verification, the remaining roadmap is limited to Python/NumPy
borrowed views, MCAP, a ROS 2 bridge, a network gateway, schema evolution, and
advanced loan-pool tuning. The WaitSet milestone does not start any of them.

The fixed-order benchmark remains a recorded comparison, not an SLA or
universal performance statement.

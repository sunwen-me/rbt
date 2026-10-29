# Benchmark methodology

This guide retains the original transport/pressure measurement methods and
their dated results. The current SDK is v0.8.0; historical measurements below
are not fresh v0.8 numbers. Use the [documentation index](README.md) and
[testing guide](testing.md) for current development and verification scope.

## Matched RBT / ROS 2 sensor-sized transport experiment

The [optional ROS 2 transport harness](../benchmarks/ros2_transport/README.md)
compares standalone RBT SDK endpoints with ROS 2 Fast DDS SHM-copy and
loan + Data Sharing paths using identical synthetic fixed-size payloads.
The [2026-10-29 report](../benchmarks/ros2_transport/results/2026-10-29/README.md)
contains the measured CPU/PSS ranges, all 27 trial rows, loan evidence and
standard-message capability limits. This is a Linux x86_64 host result,
separate from the historical native transport results below.

## Current RPC and TF harnesses

`./scripts/benchmark_rpc.sh --smoke` measures process-separated native RPC
round trips and validates every response. `./scripts/benchmark_tf.sh --queries 10000`
measures correctness-checked **local** lookup for one-, three-, and ten-edge
static paths plus history interpolation. The TF script currently builds the
debug tool and records that mode, source/binary hashes, query count, and output.
Its local lookup latency is separate from TF Stream/RPC transport latency.
Retained acceptance results are in the [RPC report](rpc-report.md) and
[TF report](tf-report.md); preserve their build modes and environment when
comparing results.

## Original transport baseline

`scripts/benchmark_v01.sh` is the preserved polling two-process data-plane
baseline. It does not benchmark Composer, serialization, ROS 2, Dora, or a
network path, and v0.1 makes no cross-framework performance claim.

## Topology

For every case the harness starts one publisher process, waits for its ready
line, then starts one subscriber process on a unique iceoryx2 service. The
payload carries a sequence and publisher timestamp. The subscriber reports
one-way publisher-timestamp-to-receive latency using the same host system
clock and derives drops from sequence gaps.

Sensor cases use generated slice ABIs and these payload sizes:

```text
64 B
1 KiB
1 MiB
6,220,800 B
20,971,520 B
```

The first four use `ImageHeader + u8[]`; 6,220,800 bytes is a representative
1920×1080 RGB payload. The 20,971,520-byte case uses a real
`PointCloudHeader + 1,310,720 PointXYZI[]` generated ABI. The Control case uses
the fixed 16-byte generated `ControlCommand` ABI.

## Run

```bash
./scripts/benchmark_v01.sh --smoke
./scripts/benchmark_v01.sh --count 1000 --warmup 100 --interval-us 1000
./scripts/benchmark_v01.sh --profile sensor --count 500

./scripts/benchmark_waitset.sh --smoke
./scripts/benchmark_waitset.sh
```

Release is the default; `--debug` exists for harness development. `--smoke`
runs Sensor Image 64 B, Sensor PointCloud 16 B (one `PointXYZI`), and Control
16 B so every generated message path is exercised without the large payloads.
Timeouts, startup delay, interval, profile, count, and warmup have flags and
matching `RBT_BENCH_*` overrides.

For multi-subscriber pressure, use the separate bounded Sensor harness:

```bash
./scripts/benchmark_pressure.sh --smoke
./scripts/benchmark_pressure.sh
```

The full pressure matrix runs subscriber counts 1, 2, 4, and 8 for 64 B,
1 KiB, 1 MiB, and 6,220,800 B generated `Image` slices plus a 20,971,520 B
generated `PointCloud` slice. It also runs a 64 B Sensor case with two fast
subscribers, one subscriber delayed by 200 ms between receives, and one
subscriber that stays connected without receiving. `--smoke` keeps the matrix
to 64 B while retaining the mixed-speed case. The pressure harness defaults to
20 measured samples, 3 warmup samples, a 5 s base timeout, a 500 ms publisher
startup delay, and a 1 ms *minimum* send interval; `--count`, `--warmup`,
`--timeout-ms`, `--startup-delay-ms`, `--interval-us`, and `--slow-delay-us`
override those defaults.

The uniform full matrix performs full-linear payload validation in every
subscriber. To keep correctness checks deterministic under concurrent large
payloads, its effective interval is the maximum of the requested minimum,
`ceil(payload_bytes / 25)` microseconds, and `fanout * 1,000` microseconds. The
per-case timeout scales with that effective interval. The deliberately
overloaded 64 B mixed-speed case retains the requested interval so that the
slow and stalled roles exercise Sensor replacement. Every
`RBT_PRESSURE_CASE` and `RBT_PRESSURE_RESULT` records `effective_interval_us`;
the reported rates are observations at that pacing, not peak-throughput
claims. Result lines also record process CPU time and RSS for the publisher and
each subscriber role.

Before measurement the publisher waits for the actual iceoryx2 aggregate
subscriber count, not merely the requested shell-process count. Every measured
send must then report exactly that fanout. Results also record the observed
subscriber count, minimum/maximum recipients per send, received count, and
sequence-gap Drop estimate. The script fails if a fast subscriber misses data
or if the mixed-speed roles do not exhibit the expected bounded behavior.

## Verified pressure baseline

The accepted v0.4 run on 2026-09-24 used a release build, 20 measured samples,
3 warmup samples, a 500 ms publisher startup delay, and the validation-aware
effective intervals described above. The host was an Intel Core Ultra 5 125H
(18 logical CPUs), Ubuntu 26.04, Linux 7.0.0-31-generic x86_64, and rustc
1.98.1. The retained result is
[`pressure-benchmark-matrix.log`](../target/pressure-tuning-verify-logs/pressure-tuning-full-20260924T113241Z-12202/pressure-benchmark-matrix.log),
run `rbt-bench-pressure.kdAz3l-151425`.

Publisher rate and the range across the fast subscribers are samples/s. CPU is
process CPU consumed during that process's measured interval. RSS is the final
process resident set, so the subscriber columns report the largest value in
the fanout rather than a sum. Every fast subscriber received all 20 samples,
reported zero sequence gaps, and validated the full payload. Every publisher
observed the requested fanout on every measured send.

| Payload | Fanout | Interval (us) | Publisher Hz | Publisher CPU s | Publisher RSS MiB | Subscriber Hz range | Max subscriber CPU s | Max subscriber RSS MiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 B Image | 1 | 1000 | 907.271 | 0.004527 | 5.50 | 943.844-943.844 | 0.006519 | 5.96 |
| 64 B Image | 2 | 2000 | 475.986 | 0.004080 | 5.48 | 497.383-498.870 | 0.006750 | 5.99 |
| 64 B Image | 4 | 4000 | 243.916 | 0.004016 | 5.50 | 255.579-255.978 | 0.006866 | 6.02 |
| 64 B Image | 8 | 8000 | 123.058 | 0.005193 | 5.46 | 129.254-129.432 | 0.008754 | 6.04 |
| 1 KiB Image | 1 | 1000 | 907.968 | 0.004140 | 5.47 | 939.498-939.498 | 0.006470 | 6.03 |
| 1 KiB Image | 2 | 2000 | 477.836 | 0.004118 | 5.47 | 498.671-500.285 | 0.006375 | 5.93 |
| 1 KiB Image | 4 | 4000 | 241.735 | 0.004739 | 5.51 | 252.875-253.501 | 0.006884 | 6.04 |
| 1 KiB Image | 8 | 8000 | 122.744 | 0.004838 | 5.48 | 128.726-129.145 | 0.008404 | 6.04 |
| 1 MiB Image | 1 | 41944 | 23.527 | 0.015001 | 6.47 | 24.784-24.784 | 0.072449 | 6.96 |
| 1 MiB Image | 2 | 41944 | 23.547 | 0.014892 | 6.44 | 24.789-24.792 | 0.061090 | 6.98 |
| 1 MiB Image | 4 | 41944 | 23.521 | 0.016011 | 6.50 | 24.754-24.789 | 0.085596 | 7.06 |
| 1 MiB Image | 8 | 41944 | 23.490 | 0.017095 | 6.57 | 24.692-24.754 | 0.091262 | 7.07 |
| 6.22 MiB Image | 1 | 248832 | 3.981 | 0.060549 | 11.38 | 4.190-4.190 | 0.323464 | 11.86 |
| 6.22 MiB Image | 2 | 248832 | 3.980 | 0.062067 | 11.43 | 4.192-4.195 | 0.351333 | 11.85 |
| 6.22 MiB Image | 4 | 248832 | 3.982 | 0.060139 | 11.39 | 4.188-4.191 | 0.400465 | 11.92 |
| 6.22 MiB Image | 8 | 248832 | 3.978 | 0.065081 | 11.50 | 4.182-4.193 | 0.427948 | 11.97 |
| 20 MiB PointCloud | 1 | 838861 | 1.187 | 0.099394 | 25.48 | 1.249-1.249 | 0.992739 | 26.00 |
| 20 MiB PointCloud | 2 | 838861 | 1.187 | 0.105004 | 25.45 | 1.249-1.249 | 1.030126 | 25.95 |
| 20 MiB PointCloud | 4 | 838861 | 1.187 | 0.099867 | 25.48 | 1.249-1.251 | 1.144347 | 25.98 |
| 20 MiB PointCloud | 8 | 838861 | 1.187 | 0.103065 | 25.52 | 1.248-1.250 | 1.369013 | 26.00 |

In the deliberately overloaded 64 B mixed-speed case, the publisher reported
896.899 samples/s with all four subscribers connected. The two fast
subscribers received all 20 samples at 934.399 and 935.696 samples/s with zero
gaps; the 200 ms subscriber received 2 and estimated 18 overwritten samples;
the stalled subscriber received 0 and estimated all 20 as overwritten.
Relative to the paced uniform four-subscriber case, the publisher and minimum
fast-reader rate ratios were 3.677 and 3.656. This difference is expected: the
uniform fanout-4 case uses a 4 ms validation interval while the overloaded case
deliberately keeps the requested 1 ms interval. It is not an instrumentation
speedup claim.

There is no retained pre-instrumentation v0.4 build with the same WaitSet and
compiler, so this run cannot honestly produce a paired before/after overhead
ratio. The table is the accepted instrumented baseline. Source-level overhead
and that evidence limitation are recorded in
[`pressure-tuning-report.md`](pressure-tuning-report.md); the historical v0.1
and WaitSet results below must not be presented as an uninstrumented control.

## Measurements

Both processes report:

- build mode, service, profile, payload size, count and warmup;
- queue configuration and expected subscriber count;
- sampling duration and publish/receive/delivery rate;
- published/delivered/received/drop counts;
- process CPU time and final RSS where Linux exposes them;
- message layout hash.

The subscriber additionally reports nearest-rank p50, p90, p99, and maximum
one-way latency in microseconds plus the number of latency samples. Warmup
samples are excluded. Publisher rate and subscriber rate use their respective
measured intervals, so small smoke runs should not be compared as if they used
an identical window.

The shell harness records run id, exact executable/build mode, CPU model,
logical CPU count, kernel/OS, Rust compiler, C++ compiler, and all method
parameters before cases begin. Keep this metadata with any published result.

## WaitSet milestone comparison

The WaitSet harness compares the preserved polling binary with the current
WaitSet binary using the same topology and arguments. The full run used a
fixed strategy order (`polling,waitset`), a 10 s no-data idle case, 500 latency
samples at 100 Hz with a 64 B payload, and the full payload/fanout matrix
(64 B, 1 KiB, 1 MiB, 6,220,800 B, and 20,971,520 B; fanouts 1, 2, 4, and 8).
Loaded cases perform full-linear validation and use an interval equal to the
maximum of the requested minimum, `ceil(payload_bytes / 100)` microseconds,
and `fanout * 1,000` microseconds. Each loaded result records that interval, so
strategy comparisons remain paired at the same pacing.
The exact retained output is
[`target/waitset-benchmark-full-final.log`](../target/waitset-benchmark-full-final.log),
run `waitset-20260910T183644Z-66797`.

Idle process observations from that run were:

| Metric (10 s idle) | Polling | WaitSet |
| --- | ---: | ---: |
| User CPU (s) | .06 | 0 |
| System CPU (s) | .13 | 0 |
| RSS (KiB) | 5284 | 6064 |
| Threads | 1 | 2 |
| File descriptors | 12 | 27 |

The 64 B latency case reported p50/p90/p99/max of `80.718/144.287/155.237/max
267.771 us` for polling and `36.190/104.933/153.240/max 188.429 us` for
WaitSet.

Across the loaded matrix, WaitSet/polling ratios were publish rate average
`.984` (minimum `.933`, maximum `1.022`) and receive rate average `.979`
(minimum `.928`, maximum `1.032`). Subscriber CPU ratio averaged `.320` with
range `.140-.572`; subscriber RSS ratio averaged `1.151` with range
`1.125-1.175`. The p99 latency ratio averaged `.767`; the worst ratio was
`1.832` at 1 MiB/fanout 1, with an absolute WaitSet p99 of `278.745 us`.

These are fixed-order observations from one recorded environment. They have no
SLA significance and do not support a universal performance claim.

## Interpretation limits

This is a correctness-oriented baseline harness. Scheduler placement, CPU
frequency, NUMA, container limits, background load, memory locking, and
iceoryx2 resource state can materially affect results. Run enough samples,
repeat runs, report the full environment, and present distributions rather
than one best number. Do not turn the smoke output into a throughput or latency
claim.

Sensor safe overflow intentionally allows replacement when a subscriber is
slow. Control safe overflow is disabled; a send reports typed `QueueFull` when
at least one bounded subscriber queue cannot accept the command, while other
subscribers may already have received it. rbt does not automatically retry or
duplicate that non-transactional multicast. Those are different semantics and
should not be combined into one ranking.

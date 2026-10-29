# RBT / ROS 2 transport results — 2026-10-29

All **27/27 trials passed**, with target rates reached, 100% delivery, and
zero detected invalid samples, sequence gaps, duplicates or regressions.
RBT used less PSS for all three workloads in this configuration. CPU results
are mixed: the video loan paths were similar, while IMU CPU varied enough
that this run does not establish a stable CPU ranking.

## Environment and scope

| Item | Measured configuration |
|---|---|
| Host | Intel Core Ultra 5 125H, 18 logical CPUs, 31 GiB RAM; Ubuntu 26.04 x86_64 |
| Kernel / libc | Linux 7.0.0-38-generic / glibc 2.43 |
| CPU placement | Both endpoint processes allowed CPUs 1 and 6: distinct physical cores, SMT siblings 1–2 and 6–7; `powersave` governor |
| RBT | SDK 0.8.0, standalone C++ endpoints, no Composer |
| Native dependency | iceoryx2 0.10.0-rbt.2, upstream `135d09dd8b29f321f1725920d434864c4e512378` plus tracked patch series |
| ROS 2 | Lyrical; rclcpp 32.0.2; rmw_fastrtps_cpp/shared_cpp 9.4.9; Fast DDS 3.6.2.0; sensor_msgs 5.9.3 |
| Build | GCC 15.2.0, CMake Release, `-O3 -DNDEBUG`, C++20; installed RBT SDK also built Release |
| Source | [`e2742798bcd1df601c05b8ab86ee8efb984b054d`](https://github.com/sunwen-me/rbt/tree/e2742798bcd1df601c05b8ab86ee8efb984b054d/benchmarks/ros2_transport) |
| Schedule | 3 s warmup + 15 s measurement, three repetitions per case/mode; mode order rotated per repetition |

This is an interactive host, with no CPU isolation or fixed clock. Builds
and other benchmark tests were paused during measurement. The experiment
measures the endpoint processes, including their runtime/node overhead;
the collector is excluded. It does not measure the navigation stack,
sensor drivers, video decoding, network transport, or the QCS8550 board.

Each message has the same 16-byte sequence/timestamp prefix and fixed byte
payload on both stacks. Every publisher fills the entire payload; every
subscriber checks one byte per 4096 bytes and the final byte. These are
synthetic messages, not standard ROS sensor messages. See the
[method and build/run instructions](../../README.md).

## CPU and memory

Values are **median [min–max] across three repetitions**. CPU adds both
processes; 100% is one logical core. PSS adds both processes before taking
the median of approximately 1 Hz memory samples in each trial. The peak
column is the highest sampled total PSS among the three trials, not an
instrumented allocation peak. RSS is retained in the CSV and may count
shared pages more than once.

| Workload | Path | CPU % | Total PSS MiB | Sampled peak PSS MiB |
|---|---|---:|---:|---:|
| 256 B IMU-sized, 200 Hz | RBT loan | 3.00 [0.87–3.07] | 7.01 [6.97–7.09] | 7.11 |
| | ROS SHM copy | 1.60 [1.53–3.80] | 35.28 [35.18–35.28] | 35.31 |
| | ROS loan + Data Sharing | 1.40 [1.20–2.40] | 35.27 [35.25–35.30] | 35.33 |
| 1,600,000 B cloud-sized, 10 Hz | RBT loan | 0.27 [0.27–0.40] | 8.44 [8.38–8.53] | 9.20 |
| | ROS SHM copy | 1.20 [1.20–1.27] | 96.46 [96.45–96.94] | 96.94 |
| | ROS loan + Data Sharing | 0.60 [0.60–0.67] | 39.88 [39.72–39.92] | 40.62 |
| 6,220,800 B video-sized, 30 Hz | RBT loan | 2.73 [2.73–2.87] | 12.94 [12.90–12.99] | 12.99 |
| | ROS SHM copy | 9.80 [9.34–10.14] | 286.16 [286.14–286.32] | 286.32 |
| | ROS loan + Data Sharing | 2.73 [2.53–2.87] | 53.21 [53.02–53.68] | 56.60 |

The cloud payload resembles 100,000 XYZI points; video resembles an
uncompressed 1920×1080 RGB8 frame. This is fixed-rate resource usage, not
maximum throughput. CPU uses `/proc` accounting over about 14.995 seconds
with 10 ms accounting ticks; very small differences are not meaningful.
Latency percentiles are also retained in the artifacts, measured after
payload fill; they are not driver-to-application sensor latency.

RBT uses Sensor defaults: queue depth 1, no history, two concurrent
publisher loans and one subscriber borrow. ROS uses best-effort, volatile,
depth 1, and a topic-specific four-sample pool plus two extra samples.
SHM-copy transport maximum-message/segment sizes are respectively
64 KiB/512 KiB for IMU, 2 MiB/8 MiB for cloud, and 8 MiB/32 MiB for video.
ROS loan mode uses Data Sharing for payloads and a 64 KiB/512 KiB SHM
transport for discovery/other ROS traffic. These are measured buffer
configurations, not identical allocator internals or universal defaults.
Do not extrapolate the memory ratios to every DDS configuration.

## Correctness and loan evidence

Every repetition published/received exactly 3,000 IMU, 150 cloud or 450
video messages. Both ROS endpoints reported SHM-only participant transport;
Data Sharing readback was OFF in copy mode and ON in loan mode. No UDP
transport was enabled. Every ROS loan publish/take used native RCL loan
APIs with matching return calls; measured loan counts equalled sample
counts at both endpoints.

For each RBT/ROS-loan trial, eight sequence-matched application addresses
resolved to the same shared backing device, inode and byte offset in both
processes. Copy trials had zero aliases. This is sampled shared-buffer
evidence, not a trace of every packet. The portable summary includes the
readback and counts; full addresses and process maps are retained locally.

The separate [standard-message capability probe](standard-loan-probe.jsonl)
reported `publisher_can_loan=false` and `subscriber_can_loan=false` for
`sensor_msgs/Imu`, `sensor_msgs/PointCloud2` and `sensor_msgs/Image` on this
installation, with loans enabled. That probe does not transfer samples or
measure resources. The successful fixed-array loan results do not imply
that standard sensor messages automatically use this zero-copy path.

This shows a native-schema and API usability advantage for RBT in this tested
setup. RBT's [`Image` and `PointCloud` schemas](../../../../types/sensor.rbt)
are header-plus-contiguous-slice messages, and its [C++ SDK ownership model](../../../../docs/cpp-sdk.md)
exposes direct fixed-message and slice loan/receive access ([C++ stream API](../../../../sdk/cpp/include/rbt/stream.hpp)).
Slice payload length can vary per send within the publisher's configured
capacity, without replacing these native sensor schemas with fixed large arrays.
Unbounded string/sequence fields in the ROS sensor messages, including
`Imu.header.frame_id`, do not satisfy this loan route's plain, bounded type
requirement ([Fast DDS loan requirements](https://fast-dds.docs.eprosima.com/en/stable/fastdds/use_cases/zero_copy/zero_copy.html)).
The [capability probe](standard-loan-probe.jsonl) reports `can_loan=false` at
both endpoints for all three standard messages. This advantage is specific
to the tested native APIs and middleware configuration; other ROS 2
middleware or application designs can offer zero-copy paths.

The fixed-array loan benchmark used matched custom messages on both stacks.
Standard `sensor_msgs` were capability-probed only, not benchmarked for CPU or
memory. RBT schemas are not wire/ABI-compatible with `sensor_msgs`; copies in
external drivers or conversions are outside this IPC result. The zero-copy
data plane beneath RBT is provided by iceoryx2.

## Artifacts and reproduction

- [All 27 trial rows](trials.csv): CPU, PSS/RSS, counts, rates, delivery, latency and alias counts.
- [Portable aggregate and verification metadata](summary.json): medians/ranges, binary/profile SHA-256, observed transport/loan settings and source revision.
- [Build and software provenance](environment.json).
- [Standard ROS message loan capability output](standard-loan-probe.jsonl).

The accepted raw run is `output/ros2-transport-benchmark/formal-20261029-2/`
in the local checkout. It retains endpoint logs, per-trial XML, resource
samples, process maps and alias evidence; `output/` stays Git-ignored.
Development smoke failures and an earlier interrupted attempt remain
locally recorded. The earlier attempt was stopped after discovering an
unused copy buffer allocated in the loan subscriber; this measured revision
removes it. No repetitions from that attempt were selected for this matrix.

Use the method README's build instructions, then run:

```bash
python3 benchmarks/ros2_transport/run.py \
  --rbt-executable "$BUILD_DIR/rbt_transport_bench" \
  --ros-executable "$BUILD_DIR/ros2_transport_bench" \
  --cpus 1,6 --seconds 15 --warmup 3 --repetitions 3 \
  --output /path/to/new-raw-results
python3 benchmarks/ros2_transport/summarize.py /path/to/new-raw-results/results.json \
  --source-revision <measured-commit> --output /path/to/new-portable-results
```

Choose a valid CPU set for another host and record the change. Configuration
and software changes require a fresh complete matrix before comparison.

Harness validation: Release build passed; the final Python suite passed
21/21 tests, and source-release packaging checks passed 8/8.

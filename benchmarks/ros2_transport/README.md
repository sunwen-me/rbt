# Optional ROS 2 transport benchmark

This directory contains an isolated, process-separated comparison between the
standalone RBT SDK data path and a ROS 2 `rclcpp`/Fast DDS data path. It does not
use Composer, sensor drivers, physical devices, or production navigation
components. ROS 2 is an optional benchmark dependency, not a production
navigation dependency.

The [2026-10-29 measured report](results/2026-10-29/README.md) includes all
27 accepted trials, CPU/PSS ranges and the standard-message loan probe.
The [2026-10-31 QCS8550 report](results/2026-10-31-qcs8550/README.md) adds
an ARM64 board run using ROS 2 Humble / Fast DDS 2.6.11.

## Workload

Each trial launches one publisher process and one subscriber process. Both
endpoints use the same synthetic fixed-layout payload: a 16-byte prefix
containing a `uint64` sequence and monotonic timestamp, followed by a fixed
byte array. These custom messages are sized to resemble sensor traffic; they
are not `sensor_msgs` types, a video codec, or measurements from real sensors.

| Case | Payload bytes | Nominal rate | Analogy |
| --- | ---: | ---: | --- |
| `imu` | 256 | 200 Hz | IMU-sized sample |
| `cloud` | 1,600,000 | 10 Hz | 100k-point XYZI-sized sample |
| `video` | 6,220,800 | 30 Hz | 1080p RGB8-sized frame |

Every publisher fully fills the payload with `memset` in every mode. The
subscriber checks the expected byte value every 4096 bytes and at the final
byte, then checks sequence and timestamp consistency. The timestamp is written
after the fill, so latency measures from that point to the receive/take
timestamp; it excludes payload fill. Publisher CPU includes the full fill.

## Compared paths and evidence

The modes use these paths:

| Mode | Publisher and subscriber path | Required runtime evidence |
| --- | --- | --- |
| `rbt` | Standalone RBT C++ SDK endpoints using the Sensor stream profile and the SDK's normal loan/receive ownership APIs. No Composer is started. | At least one sampled publisher/subscriber message address must resolve to the same shared backing device, inode, and byte offset. |
| `shm` | ROS 2 `rclcpp` copy publish/take with loaned messages disabled; Fast DDS Shared Memory Transport only and Data Sharing OFF. | Both endpoints must report the expected native transport and Data Sharing QoS; sampled addresses must not alias. |
| `loan` | ROS 2 `rclcpp` node using `rcl_borrow_loaned_message`, `rcl_publish_loaned_message`, `rcl_take_loaned_message`, and the matching loan-return calls; Data Sharing ON. | Both endpoints must report loan support and the expected native transport/QoS; native loan counts must equal sent/received sample counts, and at least one sampled address must alias. |

The ROS endpoint reads Fast DDS writer/reader QoS and participant transport
configuration through `rmw_fastrtps_shared_cpp`'s
`CustomPublisherInfo`/`CustomSubscriberInfo` structures. These are
version-coupled RMW internals. The runner rejects a wrong RMW implementation,
missing or unexpected transport/QoS readback, unsupported loans, and failed
trials instead of treating them as successful comparisons. Build and run
against a matching `rmw_fastrtps_cpp` installation. The endpoint supports the
Fast DDS 2.x `.h` transport descriptor and Humble wait-set API as well as
the newer Fast DDS 3.x headers.

The runner samples up to eight sequence-matched message addresses per process
and resolves them against `/proc/<pid>/maps`, comparing shared backing device,
inode, and exact offset. This is sampled alias evidence, not a trace of every
packet or a claim that every message followed the same path. Shared Memory
Transport and Data Sharing are distinct Fast DDS mechanisms; the `shm` mode
tests the former with Data Sharing disabled, while `loan` combines application
buffer loans with Data Sharing. See the [Fast DDS zero-copy communication documentation](https://fast-dds.docs.eprosima.com/en/stable/fastdds/use_cases/zero_copy/zero_copy.html)
and the [pinned `rmw_fastrtps` 9.4.9 README](https://github.com/ros2/rmw_fastrtps/blob/9.4.9/README.md)
for middleware configuration details.

ROS publishers and subscriptions use best-effort, volatile, keep-last depth 1
QoS. Each topic-specific writer and reader keeps the four-sample pool
(`max_samples` and `allocated_samples` are four) plus two extra samples, with
`PREALLOCATED_WITH_REALLOC`; the writer publishes synchronously. These pool
settings are shared by all cases.

The Shared Memory Transport buffers are tuned to these synthetic payloads.
In `shm` copy mode, `maxMessageSize` / `segment_size` are 64 KiB / 512 KiB for
`imu`, 2 MiB / 8 MiB for `cloud`, and 8 MiB / 32 MiB for `video`. In `loan`
mode they are 64 KiB / 512 KiB for every case: payload samples use the Fast DDS
Data Sharing pool, while Shared Memory Transport is retained for discovery and
ROS metadata. These are workload-specific benchmark settings, not universal
Fast DDS defaults. The `shm` profile sets Data Sharing OFF; `loan` sets it ON.
Both disable built-in transports and configure only Shared Memory Transport,
so UDP is not an enabled fallback. Generic writer/reader defaults set Data
Sharing OFF without imposing a history depth or pool limit; this avoids
applying benchmark pool limits to unrelated ROS endpoints such as `rosout`.
The runner selects the per-trial XML profile with
`FASTDDS_DEFAULT_PROFILES_FILE` and enables its QoS settings with
`RMW_FASTRTPS_USE_QOS_FROM_XML=1`. It also sets the historical
`FASTRTPS_DEFAULT_PROFILES_FILE` selector to the same file for Fast DDS 2.x,
and `ROS_LOCALHOST_ONLY=0` so Humble does not override the configured transports.
The actual SHM-only readback remains mandatory.

The RBT endpoint uses the SDK's default Sensor resources. See the [C++ SDK
data-plane ownership notes](../../docs/cpp-sdk.md#data-plane-ownership).
`standard_loan_probe` separately reports `can_loan_messages()` for standard
`sensor_msgs` Image, Imu, and PointCloud2 endpoints. For that capability-only
probe, set `ROS_DISABLE_LOANED_MESSAGES=0` and use the SHM XML profile's generic
Data Sharing OFF defaults; it does not publish or receive benchmark samples.
After building, run it with the sourced ROS environment:

```bash
ROS_DISABLE_LOANED_MESSAGES=0 \
RMW_IMPLEMENTATION=rmw_fastrtps_cpp \
RMW_FASTRTPS_USE_QOS_FROM_XML=1 \
ROS_AUTOMATIC_DISCOVERY_RANGE=SYSTEM_DEFAULT \
FASTDDS_DEFAULT_PROFILES_FILE="$PWD/benchmarks/ros2_transport/config/shm.xml" \
  "$BUILD_DIR/standard_loan_probe"
```

## Trial schedule and interpretation

Defaults are a 3-second warmup, a 15-second measurement window, and three
repetitions. The runner rotates mode order across repetitions for each case.
`--cpus` optionally applies the same allowed CPU set to both endpoint
processes; the local comparison uses `--cpus 1,6` when those are distinct
permitted physical cores. With no option, the runner uses the invoking
process's allowed CPU set. Endpoint initialization and discovery complete
before the measurement window. The publisher readiness file must appear
before the subscriber starts, because RBT opens an existing readiness sidecar;
this barrier is outside warmup and measurement.

CPU is read from each endpoint's `/proc/<pid>/stat` user and system time near
the beginning and 5 ms before the end of the window. The reported total adds
both processes; 100% means one logical core. Memory samples read both
processes' `smaps_rollup` about once per second; the primary aggregate is the
sum of PSS, reported as median and peak. Summed RSS is also reported, but can
double-count shared mappings. The result does not estimate a universal CPU
maximum, and collection overhead is not included in endpoint CPU time.

A trial is valid only when publish rate and delivery ratio are each at least
99% of their configured targets, with no invalid samples, duplicate
sequences, or sequence regressions. Sequence gaps are reported separately.
RBT and loan trials also require sampled alias evidence; loan trials require
native loan counts to match their sample counts; copy-mode trials reject an
observed alias. Unsupported or failed trials remain non-success statuses and
make the run fail. Do not call a high-CPU result a winner when validity checks
fail. Measured results are kept in the dated report linked above.

## Build and run

The build requires a C and C++ compiler, CMake, the installed RBT C++ SDK and
its native dependency prefix, and a sourced ROS 2 installation providing
`ament_cmake`, `rosidl_default_generators`, `rclcpp`, `sensor_msgs`, and
`rmw_fastrtps_shared_cpp`. CMake generates the ROS message types from the `.msg`
files here; the RBT generated headers are checked in under `generated/cpp/`.
Use the same ROS/RMW/Fast DDS installation for compilation and execution:

```bash
source /opt/ros/lyrical/setup.bash  # replace with the installed ROS 2 setup
export RBT_PREFIX=/path/to/rbt-sdk-install
export NATIVE_PREFIX=/path/to/rbt-native-install
export CMAKE_PREFIX_PATH="$RBT_PREFIX:$NATIVE_PREFIX${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
BUILD_DIR="$PWD/target/ros2-transport-benchmark/build"

cmake -S benchmarks/ros2_transport -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --parallel

export LD_LIBRARY_PATH="$BUILD_DIR:$RBT_PREFIX/lib:$NATIVE_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
python3 benchmarks/ros2_transport/run.py \
  --rbt-executable "$BUILD_DIR/rbt_transport_bench" \
  --ros-executable "$BUILD_DIR/ros2_transport_bench" \
  --output /path/to/a/new-results-directory \
  --seconds 15 --warmup 3 --repetitions 3
```

The output path must not already exist. Add `--cpus 1,6` only when those CPUs
are available and form the intended placement. Set `--ros-setup` to
`/path/to/setup.bash` when the runner should source ROS itself. Results include
`results.json`, `summary.csv`, per-trial
logs, runtime metadata, QoS/profile records, resource samples, process maps,
and alias evidence.

Export portable per-trial rows and aggregates with the exact measured source
revision (the exporter retains failures and exits nonzero for an incomplete
or invalid matrix):

```bash
python3 benchmarks/ros2_transport/summarize.py /path/to/results.json \
  --output /path/to/a/new-summary-directory \
  --source-revision <measured-git-commit>
python3 -m unittest discover -s benchmarks/ros2_transport -p 'test_*.py'
```

The current local build environment is Ubuntu 26.04 with ROS 2 Lyrical, Fast
DDS 3.6.2, `rmw_fastrtps` 9.4.9, `rclcpp` 32.0.2, and GCC 15.2. It is not an
ARM or robot-hardware measurement. The ignored local script
`target/ros2-transport-benchmark/build-local.sh` records its GCC 15.2 toolchain,
`CMAKE_SYSROOT=/`, and host include/linker compatibility flags; those are
specific to that host, not required by the portable command above. Results
from this setup must not be presented as performance claims for deployed
hardware.

## Regenerate the report charts

Plotting uses optional Matplotlib; it is not a benchmark or runtime dependency.
The retained charts were rendered with Matplotlib 3.11.2.
The script reads `summary.json` and `trials.csv`, checks that the aggregates
match all three repetitions, and generates CPU and PSS charts as SVG and PNG.
Bars show medians, dots show individual trials and whiskers show min–max
ranges. Axes are linear and start at zero.

```bash
python3 -m venv target/ros2-transport-benchmark/plot-venv
target/ros2-transport-benchmark/plot-venv/bin/python -m pip install matplotlib==3.11.2
target/ros2-transport-benchmark/plot-venv/bin/python benchmarks/ros2_transport/plot.py \
  --results-dir benchmarks/ros2_transport/results/2026-10-29
```

The Chinese main README uses separate `*-zh.svg`/`*-zh.png` chart variants.
Generate them with `--language zh`; a CJK font such as Noto Sans CJK must be
installed. The Chinese SVGs embed glyph outlines so readers do not need the
font locally. The default English figures and measured data remain separate.

```bash
target/ros2-transport-benchmark/plot-venv/bin/python benchmarks/ros2_transport/plot.py \
  --results-dir benchmarks/ros2_transport/results/2026-10-29 --language zh
```

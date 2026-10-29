# Gemini 335：RBT 与 ROS 2 的同机性能测量

这是独立的真机测量工具，不修改 RBT 核心或正式相机驱动。只打开命令中显式指定序列号的设备。正式结果见同目录的 `RESULTS.md`；临时构建、完整进程日志和原始 JSON 保存在 `target/orbbec-perf-20261020/`。

## 比较范围

- 同一台 Gemini 335，同一 USB 连接，SDK 2.10.6；两份 SDK 动态库 SHA-256 相同。
- 彩色 RGB8 与深度 Y16，均为 640×480、30 fps。关闭 IR、IMU、点云、对齐、额外软件滤波与帧同步；保留设备已有的硬件/SDK 降噪配置。
- 一个驱动进程和一个 C++ 消费进程。RBT 消费者由 Composer 管理，Composer 资源也计入总数。
- 主对照中 RBT 使用现有 Sensor profile；ROS 2 使用 Fast DDS 和官方默认的可靠 QoS（KEEP_LAST、depth=10），消费者也使用可靠 QoS。这是当前驱动在常用配置下的应用路径对比，队列和可靠性契约并不相同，不是严格隔离变量的中间件微基准。
- 主对照限定 ROS 发现范围为 `LOCALHOST`。附加调优组改为 `SYSTEM_DEFAULT`，使用独立 domain 187 和 Fast DDS `LARGE_DATA` 配置（1 MB 最大消息与 socket 缓冲），可靠 QoS 保持不变。`LOCALHOST` 在此 RMW 版本会覆盖内置传输，因此直接在该模式设置 `LARGE_DATA` 不会生效。调优组仍在本机两个进程之间传输；发现范围允许子网发现。
- 另保留 `ros2-sensor.yaml`，用于 best effort、KEEP_LAST、depth=1 的尝试；消费者需同时指定 `--ros-qos sensor`。本机该配置的初步试跑未达到满帧率，不能把它较少的接收工作量当成 CPU 优势。
- 不运行显示、磁盘图像保存、每帧日志或下游视觉算法。两个消费者使用相同的稀疏字节读取和统计函数；它们不执行全图像校验和或图像处理。
- 每轮先预热，再测固定时间；两版交错运行。正式测量期间停止所有构建工作。

## 指标与边界

CPU 使用 `/proc/<pid>/stat` 的窗口差分，100% 表示一个逻辑核；统计自有驱动、消费者和 Composer。内存读取 `smaps_rollup` 的 PSS，按进程求和，避免将共享页重复全额计算；PSS 是实际驻留内存，不是预留地址空间或共享内存容量。

接收帧率按窗口内帧数除以窗口长度。SDK 帧号间隙、重复与倒序分别记录。只检查窗口内部连续性，不能据此推断 USB/SDK 上游的所有潜在丢帧。

延迟统一使用本机 `CLOCK_MONOTONIC`。起点在驱动开始封装和拷贝原始像素前，终点在消费进程取得样本/进入 ROS 回调后、解码与统计前。它包含封装、拷贝、传输和调度，不包含曝光、USB 传输及 SDK 内部排队，也不是纯中间件延迟。

RBT 正式驱动已有的 `SensorTiming.received` 正好处于上述起点，无需改动。ROS 2 的临时源码副本只增加基准计时：记录封装前的 steady-clock 纳秒，放入 `Image.header.stamp`，并在 `frame_id` 后附 SDK 帧号。此临时消息不再携带正常的 ROS 采集时间，**只能用于本项测量**，不能用于 TF、SLAM 或正式部署。原始源码与补丁随构建记录保留。它不修改像素处理和传输路径。

## 构建和运行

RBT 基准消费者使用已安装的 SDK：

```bash
cmake -S drivers/orbbec/benchmarks \
  -B target/orbbec-perf-20261020/rbt-consumer-build/ninja -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$PWD/target/phase4-toolchain/bin/ninja" \
  -DCMAKE_CXX_COMPILER="$PWD/target/phase4-toolchain/bin/x86_64-conda-linux-gnu-g++" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/target/release-build-distribution-20261020/sdk-root"
cmake --build target/orbbec-perf-20261020/rbt-consumer-build/ninja --parallel 2
```

ROS 2 使用官方提交 `c153462518ad674650bafa4464fda72c27ab797a` 的独立副本、Lyrical 与 Fast DDS。缺少的依赖通过 `.deb` 解压到本地前缀，没有安装系统软件或 udev 规则。构建入口和环境脚本保留在临时目录。

`ros2_consumer.cpp` 的独立 CMake 入口为 `benchmarks/ros2/`。本机使用仓库的 GCC 15.2 工具链，ROS 依赖来自宿主系统；因此该构建需选择宿主 sysroot 和库路径，具体缓存与命令保留在 `ros2-consumer-build/`。

准备好两版可执行文件后，在仓库根目录运行：

```bash
python3 drivers/orbbec/benchmarks/run.py \
  --serial '<完整设备序列号>' \
  --output target/orbbec-perf-20261020/measurement-new \
  --seconds 20 --warmup 5
```

输出目录必须不存在。默认顺序为 RBT、ROS 2、ROS 2、RBT、RBT、ROS 2，每版三轮。脚本只清理自己启动的进程，完整保留配置、命令、资源采样及接收统计；出错时停止后续轮次，不把未运行项计入结果。

附加调优组的可重复命令：

```bash
python3 drivers/orbbec/benchmarks/run.py \
  --serial '<完整设备序列号>' \
  --output target/orbbec-perf-20261020/tuned-new \
  --order ros2,ros2,ros2 --seconds 20 --warmup 5 \
  --ros-discovery SYSTEM_DEFAULT \
  --ros-transports 'LARGE_DATA?max_msg_size=1MB&sockets_size=1MB&non_blocking=true&tcp_negotiation_timeout=50'
```

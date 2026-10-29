# Gemini 335：RBT 与 ROS 2 同机性能实测

日期：2026-10-20。**REAL_HARDWARE；三组均完成 3 轮，每轮测量 20 秒、预热 5 秒。** 下表为各轮统计值的中位数；p99 也是三轮 p99 的中位数，不是合并全部样本重新计算的分位数。

| 指标 | RBT | ROS2 基础配置 | ROS2 大帧调优 |
| --- | ---: | ---: | ---: |
| 总 CPU（单核=100%） | 20.6% | 31.1% | 27.8% |
| 总驻留内存 PSS | 57.7 MiB | 146.2 MiB | 185.5 MiB |
| 彩色接收帧率 | 29.95 fps | 30.00 fps | 29.95 fps |
| 彩色封装到接收 p50 | 0.439 ms | 12.411 ms | 1.823 ms |
| 彩色封装到接收 p99 | 0.650 ms | 23.635 ms | 2.822 ms |
| 深度接收帧率 | 29.95 fps | 29.95 fps | 29.95 fps |
| 深度封装到接收 p50 | 0.319 ms | 12.288 ms | 1.671 ms |
| 深度封装到接收 p99 | 0.521 ms | 56.232 ms | 2.612 ms |
| 窗口内 SDK 帧号间隙 | 0 | 0 | 0 |

在本次配置与测量窗口内，相比大帧调优后的 ROS 2，RBT 的总 CPU 低 **25.9%**，总 PSS 低 **68.9%**。相比基础配置，分别低 33.8% 和 60.5%。三组均约 30 fps，没有观察到窗口内部 SDK 帧号间隙、重复或倒序；帧率受相机配置限制，不能据此比较饱和吞吐上限。

## 相同条件与不同配置

- 主机：Intel Core Ultra 5 125H，18 个逻辑 CPU，约 31 GiB RAM；Ubuntu 26.04 x86_64。未绑核、未修改系统电源设置，桌面程序继续运行；每轮原始记录包含系统 loadavg。
- 相机：Gemini 335，序列号 `CP15641000AT`，同一 USB3.2 连接；此前设备记录固件为 1.4.60。使用相同 Orbbec SDK 2.10.6，SDK 动态库 SHA-256 均为 `ade25a2a2fd5a3a5d4a8142d62b03fddb0594e1ab04096cdc4cc03c02ed74d2d`。
- RGB8 彩色和 Y16 深度，各 640×480、30 fps；关闭 IR、IMU、点云、对齐、帧同步与额外软件滤波。保留设备已有的硬件/SDK 降噪状态。深度原始值与比例的处理方式保留两版现有实现。
- 一个驱动、一个独立 C++ 消费者。RBT 使用 SDK 0.8.0、驱动 0.1.0 和现有 Sensor profile；总资源包含 Composer。正式 RBT 驱动没有修改。
- ROS 2：Lyrical，`rclcpp` 32.0.2、`rmw_fastrtps_cpp` 9.4.9、Fast DDS 3.6.2；官方 wrapper 固定提交 `c153462518ad674650bafa4464fda72c27ab797a`，Release 构建。两组均使用官方默认的可靠 QoS，KEEP_LAST、depth=10，消费端相同。
- ROS2 基础配置：发现范围 `LOCALHOST`，不设置大帧传输选项。RBT 与该组按 RBT、ROS2、ROS2、RBT、RBT、ROS2 的顺序交错运行。
- ROS2 大帧调优：发现范围 `SYSTEM_DEFAULT`、专用 domain 187，`FASTDDS_BUILTIN_TRANSPORTS='LARGE_DATA?max_msg_size=1MB&sockets_size=1MB&non_blocking=true&tcp_negotiation_timeout=50'`，其他图像/QoS 配置相同；主对照结束后追加三轮。

`LOCALHOST` 在此 RMW 版本会关闭内置传输、创建自己的 SHM 与 UDP 描述符，因此该模式下设置 `LARGE_DATA` 不会生效。调优组允许内置传输配置生效，仍由本机两个进程收发；发现范围可以覆盖子网。依据：[rmw_fastrtps 9.4.9 源码](https://github.com/ros2/rmw_fastrtps/blob/9.4.9/rmw_fastrtps_shared_cpp/src/participant.cpp)、[Fast DDS 环境变量](https://fast-dds.docs.eprosima.com/en/3.x/fastdds/env_vars/env_vars.html)及[大帧传输配置](https://fast-dds.docs.eprosima.com/en/3.x/fastdds/use_cases/tcp/tcp_large_data_with_options.html)。未通过抓包或传输层 tracing 确认每个样本实际使用 SHM、UDP 或 TCP，不从配置名直接推断传输路径。

## 延迟含义和测量边界

起点是驱动开始封装及拷贝原始像素前的本机单调时刻，终点是消费者取得样本/进入回调时、解码统计前。**不包含曝光、USB 传输及 SDK 内部排队；不是相机端到端延迟，也不是纯中间件延迟。** 两版驱动的封装、深度处理和传输实现不同，不能把差异全部归因于 RBT 或 DDS 本身。

RBT 使用正式驱动已有的 `SensorTiming.received`；ROS 2 的临时副本在对应起点写入单调时间，并在 frame_id 后附 SDK 帧号。该基准专用消息不能用于正常 ROS TF/SLAM。原始源码与 [计时补丁](ros2-timing.patch)均保留，像素处理和传输代码路径未改动。两个消费者用同一个统计函数、相同的稀疏像素读取方法，不显示图像、不逐帧打印、不保存像素文件、不运行视觉算法。

CPU 是所有自有进程的用户态加内核态 CPU 时间差分，100% 为一个逻辑核。PSS 是约每 0.5 秒读取的比例驻留内存，先按进程相加再按时间平均；不等同于预留内存或共享内存池容量。每组资源指标均含完整消费进程，RBT 还含 Composer。

## 各轮原始统计

| 组别与轮次 | 总 CPU | 总 PSS MiB | 彩色/深度帧数 | 彩色/深度 p50 ms | 彩色/深度 p99 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| RBT 1 | 20.600% | 57.641 | 599 / 599 | 0.454 / 0.321 | 0.650 / 0.592 |
| RBT 2 | 21.050% | 57.689 | 599 / 599 | 0.439 / 0.312 | 0.630 / 0.521 |
| RBT 3 | 20.097% | 58.022 | 599 / 599 | 0.439 / 0.319 | 0.696 / 0.506 |
| ROS2 基础配置 1 | 30.875% | 145.614 | 600 / 599 | 12.411 / 12.288 | 23.635 / 45.824 |
| ROS2 基础配置 2 | 31.423% | 146.204 | 599 / 599 | 12.406 / 12.297 | 23.459 / 56.232 |
| ROS2 基础配置 3 | 31.124% | 147.880 | 600 / 599 | 12.423 / 12.240 | 23.812 / 56.340 |
| ROS2 大帧调优 1 | 27.869% | 185.483 | 599 / 599 | 1.823 / 1.709 | 2.881 / 2.612 |
| ROS2 大帧调优 2 | 27.366% | 185.462 | 599 / 599 | 1.712 / 1.632 | 2.397 / 2.275 |
| ROS2 大帧调优 3 | 27.817% | 185.819 | 599 / 599 | 1.831 / 1.671 | 2.822 / 2.644 |

## 失败与范围限制

正式成功组之外，`measurement-default/01-rbt` 记录了一次 RBT 彩色启动超时：深度已经到达，彩色在默认 5 秒期限内未进入输出；驱动明确报错并退出。没有修改驱动、超时阈值或设备固件，重新启动后完成了上表三轮。原因尚未定位；性能数字只代表成功运行窗口，不能作为启动可靠性或长期稳定性的证明。

早期将 ROS 两端设为 best effort、KEEP_LAST、depth=1 的试跑中，接收仅约 4–8 fps；使用 LOCALHOST 时，单设 LARGE_DATA 也没有改善。该组未作为主 CPU/延迟比较基线，完整数据保留。另一次启动尝试在 ROS 安装尚未完成时因缺少库退出，未打开相机，不属于性能样本。中止的 `measurement-final` 保留其已完成窗口和后续中止记录。

本次没有测试 IMU/点云负载、多个消费者、跨主机、其他 RMW、ROS 进程内通信、DDS Data Sharing/loan 优化、图像算法、USB 拔插和长时间运行。没有修改现有 RBT runtime、消息格式、IDL、TF 或原生依赖；没有运行与本项无关的整体验证。

ROS 驱动所需依赖仅解压在本地临时前缀，未安装系统软件或 udev 规则。相机节点和共享库完成编译安装；完整上游 all-target 构建另有一个依赖 HighGUI 的 GUI 示例未链接，安装时仅跳过该示例。不能将相机节点成功构建表述为上游全部构建目标通过。

## 可重复入口与原始记录

命令与指标定义见 [README](README.md)。原始文件均相对于仓库根目录：

- `target/orbbec-perf-20261020/measurement-default-2/result.json`：RBT 与 ROS2 基础配置的六轮记录。
- `target/orbbec-perf-20261020/measurement-tuned/result.json`：ROS2 大帧调优三轮记录。
- `target/orbbec-perf-20261020/summary.json`：机器可读汇总。
- 每轮目录包含 `commands.json`、实际相机参数、`runtime-env.json`、消费者 JSON、资源采样和进程日志；SDK/驱动/消费者二进制 SHA-256 保留在组级 metadata。
- `target/orbbec-perf-20261020/ros2-src/.benchmark/`：官方原始源码和计时补丁；`ros2-log/` 为构建记录，`ros2-env.sh` 为独立环境。

正式九轮的相机驱动与消费者均退出 0；所有自有进程回收，没有通过 SIGKILL 清理。RBT Composer 由脚本主动 SIGTERM 结束。

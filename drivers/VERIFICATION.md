# 原生底盘与 IMU 驱动验收（2026-10-22）

后续进展：2026-10-29，原生 DM IMU 驱动已部署至车载 AidLux，并通过真实串口、原生
`/imu/data` 发布及后台服务重启验证；修复了实机暴露的 CRC 移植错误。
后续已将 DM IMU 的 RBT 发布频率改为 200 Hz，并部署时钟查询优化；
详见 [IMU 实机部署记录](dm_imu/DEPLOYMENT.md) 和
[CPU 与频率优化实测](../output/dm-imu-optimization/20261028T180357Z/REPORT.md)。下文保留 2026-10-22 的原始软件验收范围；
真实底盘电机验收仍未执行。

`rbt_base_controller` 和 `rbt_dm_imu` 已实现、编译并安装到当前工作区的 `target/robot-sdk/bin`。本次使用真实 RBT IPC 和串口伪终端验证驱动进程，没有打开真实 `/dev/ddsm`、`/dev/dm_imu`，没有驱动真实电机。架构图、构建步骤见 [README](README.md)，完整文件清单见 [FILES](FILES.md)。

## 实现范围

- 底盘复用 Downloads 中 DDSM 包的串口协议、运动学、PID、反馈与里程计核心，读取原 `ddsm_robot_current.yaml`。新增原生 RBT 进程，消费最终 `/cmd_vel`，独立串口线程落实命令期限、Watchdog、急停和反馈故障停车；不发布 TF。
- IMU 使用独立 POSIX 串口和完整帧解析器，三个 CRC 与全部数值通过后才发布原生 IMU 消息。输出采用 rad/s、m/s²、归一化四元数，时间戳为主机接收时间。默认不更改设备设置、不保存设备参数。
- 复用现有 `VelocityCommand`、`EmergencyStop`、`Odom2D` 及 RBT Publisher/Subscriber、HostClock、EffectScope。新增 `rbt.hardware` IDL 只承载 IMU、轮式里程计和底盘诊断；未引入 ROS 2。
- A*、ESDF、RPP、DWPP、NavigationManager 和原有测试保留。导航与 Guard 不承担电机控制。

## 实际测试结果

工具链为 GCC 15.2 / C++20。Release 测试保留断言。下列日志均为本次实际执行结果；证据目录是当前工作区的构建产物，不是仓库内预置结果。

| 配置 | 结果 | 证据（相对工作区根目录） |
|---|---|---|
| 底盘 Debug | **8/8 PASS** | `target/hardware-port-acceptance/base-debug.log` |
| 底盘 Release | **8/8 PASS** | `target/hardware-port-acceptance/base-release.log` |
| 底盘 ASan + UBSan | **8/8 PASS** | `target/hardware-port-acceptance/base-sanitizers.log` |
| IMU Debug | **1/1 PASS** | `target/hardware-port-acceptance/imu-debug.log` |
| IMU Release | **1/1 PASS** | `target/hardware-port-acceptance/imu-release.log` |
| IMU ASan | **1/1 PASS** | `target/hardware-port-acceptance/imu-asan.log` |
| IMU UBSan | **1/1 PASS** | `target/hardware-port-acceptance/imu-ubsan.log` |
| 原生 RBT 跨进程 + 两个真实驱动可执行程序 + PTY | **3/3 PASS，6.76 s** | `target/hardware-port-acceptance/hardware-ipc-final.log` |
| 底盘和 IMU 均启用 ASan + UBSan 的跨进程 pipeline | **PASS，1.613 s**；两个驱动正常退出，无 Sanitizer 诊断 | `target/hardware-port-acceptance/sanitizer-pipeline/` |

底盘 8 项包含全部 7 项原测试和新增原生命令安全测试；IMU 的一项 CTest 内含分片、噪声、各段 CRC 损坏、非有限值、恢复、可选 tag/终止字节、单位和四元数检查。Sanitizer 使用 `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`。跨进程 Sanitizer 场景中 Guard 和 fixture 是普通 Debug 构建，只有两个新驱动同时启用了 ASan + UBSan。

原有导航的独立验收仍有效：Debug + 原生 IPC **22/22 PASS**，Release / ASan / UBSan 纯核心分别 **9/9 PASS**。按“底盘不发布 TF”修改后的完整 Debug 回归为 **22/22 PASS，48.07 s**。具体配置、范围和历史日志见 [导航验收记录](../navigation/docs/verification.md)；本次没有把旧日志表述为重新执行的测试。

## 跨进程场景与判定

测试使用唯一 IPC 前缀和 `/<prefix>/cmd_vel`，只对本次启动的子进程注入故障。底盘和 IMU 都运行各自真实原生可执行程序，串口对端是 Python PTY 协议响应器。测试实际解析底盘发出的 HAT 命令与 CRC，同时读取真实 RBT 消息；不是直接调用控制函数。

| 场景 | 实际覆盖 |
|---|---|
| `pipeline` | 原始期望速度经 Guard 转成最终速度并产生实际串口运动命令；原始命令停止后 Guard 输出零；恢复后 SIGKILL Guard，底盘自身 Watchdog 停车。错误来源 99、已过期的来源 100 命令均拒绝且不续期；新鲜命令重新使能。急停锁存、释放后需新命令、终态零速均验证。仅损坏 IMU 帧持续 300 ms 时不发布新样本，恢复有效帧后重新发布。 |
| `navigation` | 真实导航 runtime 默认 RPP + DWPP、Guard、底盘、IMU 和外部地图/TF fixture 组成闭环。轮反馈驱动里程计；1 m 目标成功，最终位姿约 `(0.927077, 0.00497003)`，确认新鲜零速遥测和连续零速串口帧。观察 383 条底盘状态、383 条轮里程计、189 条 IMU 消息。 |
| `navigation_crash` | 确认导航已经驱动运动后，只 SIGKILL 导航；Guard 仍存活并报告 Watchdog，持续发最终零速；底盘接收零速，其自身 Watchdog 保持未触发。观察 9 条连续零速底盘状态、11 条轮状态及 6 条 Guard Watchdog 状态。 |

两个进程崩溃场景均保留至少 200 ms 的停车观察及 500 ms 上限、多帧新鲜状态与真实串口零速断言。这里测的是软件确认窗口，不是物理制动距离或电机停转时间。导航被杀后不读 TF，外部 fixture 实际出现 map→odom / odom→base_link 队列满，分别记录 37 / 18 次丢弃；只有 QueueFull 被计数处理，其他 TF 错误仍失败。

每个 `result.json` 包含各阶段 HostClock 时间、完整串口发送轨迹和进程退出码。正常场景所有进程退出码为 0；pipeline 只有 Guard 为预期 -9；navigation_crash 只有 runtime 为预期 -9。底盘关闭判定要求在本次关闭标记时间之后出现零速加制动帧，避免把启动制动帧误当关闭结果。IMU 正常退出码同样纳入判定。

普通构建的各场景证据在 `target/hardware-ipc-build/evidence/{pipeline,navigation,navigation_crash}/`；Sanitizer 链路证据在 `target/hardware-port-acceptance/sanitizer-pipeline/`。原始结果汇总为 `target/hardware-port-acceptance/driver-matrix.json`。

最初联调暴露并修复了 SDK C++ ClockKind 与现有 Stamp 线格式数值不同的问题；线格式 HostMonotonic 固定为 1，未改导航 ABI。测试夹具的 TF 预热、崩溃后 QueueFull 处理和串口轨迹阶段对齐也经过修正；最终通过保留了时钟身份、期限、新鲜度和停车断言。不是首次运行即全部通过。

## 保留、生成与依赖审计

- DDSM 核心、原测试、当前 YAML 和来源清单共 29 个文件与解压参考逐字节一致；原 ROS 节点没有移入生产目标。IMU 原包许可证未完备，新解析器和串口实现按协议重新编写；来源说明见各包 NOTICE。
- 导航算法、Manager 和原测试共 16 个文件与解耦前快照逐字节一致；审计为 `target/hardware-port-acceptance/preservation.json`。
- 使用仓库 `rbt-idl` 再生成 hardware 的 C++、Rust、descriptor，三份输出逐字节相同；核对目录为 `target/hardware-idl-check`。
- 两个驱动 `ldd` 的库均解析成功且无 ROS 库。底盘只依赖导航消息接口，没有链接导航核心或导航运行库；证据为 `target/hardware-port-acceptance/dependencies.json`。

## 复现与未验证项

普通构建与三个原生 IPC 用例的命令见 [README](README.md)。底盘 Sanitizer 的 CMake 开关为 `RBT_BASE_CONTROLLER_SANITIZE=ON`；IMU 为 `RBT_DM_IMU_SANITIZERS=address,undefined`。本次底盘 Release / Sanitizer 目录分别为 `/tmp/rbt-base-controller-build2`、`/tmp/rbt-base-controller-sanitize`；相应 CMakeCache 已复制到证据目录。对已构建的两个 Sanitizer 驱动重跑进程链路：

```bash
export LD_LIBRARY_PATH="$PWD/target/phase4-toolchain/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
python3 drivers/tests/hardware_ipc_test.py --scenario pipeline \
  --fixture "$PWD/target/hardware-ipc-build/rbt_hardware_ipc_fixture" \
  --base-build-dir /tmp/rbt-base-controller-sanitize \
  --imu-build-dir "$PWD/target/dm-imu-sanitizer-build" \
  --navigation-build-dir "$PWD/target/navigation-build" \
  --output-dir "$PWD/target/hardware-port-acceptance/sanitizer-pipeline"
```

真实设备测试 **NOT RUN**。仍需实车核对电机方向和制动、HAT 固件、设备端通信中断停车、IMU 协议未知字段/物理输入单位/安装轴向、实际串口速率、footprint 和定位。软件已提供 ROS 2 标准的 IMU 输出单位，但参考代码没有证明设备原始量的物理单位。底盘的独立进程 Watchdog 已验证；设备或桥接固件在底盘进程本身崩溃后如何停车，必须由设备端保障并另行验收。

# 底盘接口解耦验收记录（2026-10-21～22）

当前链路为 runtime 原始期望速度 → 独立 VelocityGuard → 最终 `/cmd_vel` → 独立底盘。2026-10-21 解耦验收使用 `rbt_navigation_base_sim`，当时尚未移植硬件驱动。2026-10-22 已新增原生 `rbt_base_controller` 和 `rbt_dm_imu`，并通过串口伪终端与真实 RBT IPC 验证，详见 [驱动验收记录](../../drivers/VERIFICATION.md)。实车验证仍未执行。以下保留各次验收的原始范围与结果。

## 2026-10-21 底盘解耦验收

环境为 GCC 15.2.0、C++20，`-Wall -Wextra -Wpedantic -Werror`。现有非 IPC 测试全部保留，新增 guard 核心测试；原有六种 IPC 场景扩展为四进程，并增加五种场景及 RPP-only 对照。

| 验证 | 真实结果 | 工作区证据 |
|---|---|---|
| Debug + 原生 RBT IPC | **22/22 PASS**，48.57 s | `target/navigation-acceptance/guard-ctest-debug.log` |
| Release，纯核心 | **9/9 PASS**，0.20 s；断言保持开启 | `target/navigation-acceptance/guard-ctest-release.log` |
| AddressSanitizer，纯核心 | **9/9 PASS**，0.87 s；`detect_leaks=1:halt_on_error=1` | `target/navigation-acceptance/guard-ctest-asan.log` |
| UndefinedBehaviorSanitizer，纯核心 | **9/9 PASS**，0.66 s；`halt_on_error=1` | `target/navigation-acceptance/guard-ctest-ubsan.log` |
| 双层过期消息定向回归 | **2/2 PASS**；随后完整 Debug 再通过 | `target/navigation-acceptance/guard-ctest-expiry-regression.log` |
| 安装与独立消息消费者 | `find_package` + 只链接 `rbt_navigation::messages` + 原生消息往返 PASS | `target/navigation-guard-install`、`target/navigation-guard-consumer` |
| 保留范围与 ABI 审计 | 算法/Manager 与全部原有非 IPC 测试逐字节不变；已有消息描述逐字节不变，仅新增 GuardState | `target/navigation-acceptance/guard-preservation-audit.json` |
| IDL 确定性再生成 | C++、Rust、descriptor 三份输出逐字节相同 | `target/navigation-idl-check` |
| ROS 依赖检查 | 源码/CMake 无禁止依赖；runtime/guard 的 `ldd` 无 ROS 库 | 未引入 ROS 2 |

Release/ASan/UBSan 均使用现有 `RBT_NAVIGATION_NATIVE=OFF` 配置，覆盖纯核心、安全校验及运动学仿真；原生进程和真实 RBT IPC 在 Debug 配置验证。独立消息消费者的链接命令只包含现有 iceoryx2 SDK 库，不链接导航算法或 Guard 库。测试用底盘和 fixture 不安装到生产前缀。

## 原生 IPC 证据

每次 runner 启动四个独立进程：模拟底盘、Guard、导航和 fixture。使用唯一 service prefix、绝对命名的最终 `/<prefix>/cmd_vel`、两组 TF UUID，避免测试间串流。故障仅注入本次启动并已确认运动的子进程。

| 场景 | 验证行为与最终证据 |
|---|---|
| success | 目标位置/朝向完成；实际观察 253 条新鲜最终命令和终态零速 |
| obstacle | 原墙体和 0.18 m footprint 不变，44 点路径绕障；路径、执行位姿与相邻位姿线段碰撞断言保留；终态零速 |
| cancel | 受控取消并停止，Guard 持续输出零；确认窗口 140 ms |
| dropout | 定位输入中断触发导航失败，Guard/底盘停止；确认窗口 582 ms，小于原有 1 s 限制 |
| watchdog | SIGKILL 导航，Guard watchdog=1，持续最终零速；底盘正常接收零速，因此其 watchdog=0；确认窗口 201 ms，小于 500 ms |
| guard_crash | SIGKILL Guard，导航仍发出至少两条新状态；底盘自身 watchdog=1 且零速；确认窗口 200 ms，小于 500 ms |
| expired | 导航强杀后注入 11 条高序号、同钟域但已到期的原始命令；Guard 拒绝计数增加 9，watchdog=1，零速；确认窗口 201 ms |
| base_expired | Guard 强杀后注入 11 条高序号、同钟域但已到期的最终命令；底盘拒绝计数增加 10，自身 watchdog=1，零速；确认窗口 201 ms |
| authority | 注入来源 99 与配置来源 1 冲突的原始命令；Guard 拒绝 15 条，最终输出仍由来源 100 发布；随后取消并停止 |
| estop | Guard 与底盘分别锁存急停，最终零速，导航继续发布状态；确认窗口 40 ms |
| cmd_vel | 实际订阅最终速度，验证完整时钟身份、来源 100、递增序列、有限值、速度边界、正且不超过 100 ms 的 TTL，观察运动和终态零速 |
| rpp-success | RPP-only 对照同样通过四进程成功闭环与最终速度检查 |

各场景 JSON 和四份进程日志位于 `target/navigation-build/native-evidence/<case>/`。正常进程退出码均为 0；watchdog/expired 的导航退出码、guard_crash/base_expired 的 Guard 退出码为预期 SIGKILL（-9），模拟底盘始终正常退出。表中确认窗口包含多帧停止确认和故障用例的至少 200 ms 观察，不是首次物理制动时间测量。

该次 watchdog 场景保留不读取的 TF 订阅者，日志实际出现 `dropped tf publication: QueueFull`。当时由模拟底盘发布 TF，堵塞时仍接收命令、执行周期安全检查并发布新状态。2026-10-22 按用户要求将 TF 发布移至外部 fixture，底盘只发布 Odom2D/BaseState；该堵塞回归仍保留。Guard 崩溃场景没有任何上游零速发布者，停车由独立底盘命令期限检查完成。

初版新增的过期探针测试曾因发送计数和停止发送条件冲突而使两个场景超时。现改为在原有 500 ms 窗内持续注入，保留至少 10 次探针、拒绝计数、Watchdog 和新鲜零速断言；探针序号高于活跃发布者水位，保证实际检查过期逻辑。定向回归及最终 22 项回归均通过。

## 复现

完整配置命令见 README。当前工作区配置好后执行：

```bash
export LD_LIBRARY_PATH="$PWD/target/phase4-toolchain/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
cmake --build target/navigation-build --parallel 4
ctest --test-dir target/navigation-build --output-on-failure
cmake --build target/navigation-release --parallel 3
ctest --test-dir target/navigation-release --output-on-failure
cmake --build target/navigation-asan --parallel 3
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 ctest --test-dir target/navigation-asan --output-on-failure
cmake --build target/navigation-ubsan --parallel 3
UBSAN_OPTIONS=halt_on_error=1 ctest --test-dir target/navigation-ubsan --output-on-failure
```

## 验证范围

固定配置单一原始控制来源是本次控制权限策略，没有增加动态租约或密码学身份认证。真实底盘必须落实独立的来源/序列/时钟/期限验证、周期 Watchdog、驱动限值、急停和驱动生命周期；详见 [底盘接口契约](base-controller-contract.md)。

该次 2026-10-21 验收未包含硬件驱动实现。现在原生底盘驱动已在工作区；真实机器人、电机/制动、硬件急停、物理定位与 LiDAR 仍为 **NOT RUN**。软件与模拟验收不代替实车验收。

## 仿真视频录制补充

2026-10-21 应用户要求，新增只读遥测记录器并重新运行原生 obstacle 闭环；场景 PASS，四个原有子进程与记录器均正常退出。采集实际底盘状态 812 条、A* 路径 44 点，最终位姿约 `(4.957,1.060,0.036)`、速度为零、导航状态 Succeeded。额外按采样位姿到原始占据栅格方形的几何距离核对，最小中心净距约 0.2861 m，大于物理半径 0.18 m。

视频使用同一次运行的地图、路径发布时间、位姿、执行速度与状态消息，未更改导航算法。消息和进程日志位于 `output/navigation-simulation/capture`、`output/navigation-simulation/ipc`，可用 `navigation/scripts/simulation_video.py --render-only` 重新编码。此补充为二维运动学数据回放，不是实车录像。

## 2026-10-22：底盘不发布 TF 的回归

按用户要求，模拟底盘移除 DynamicTfPublisher 和必需的 `--tf-odom` 参数，保留 Odom2D/BaseState 输出与独立命令期限、watchdog、急停处理。外部 fixture 从新收到的 BaseState 发布 `odom→base_link`，保留原始 HostClock 采样时间，验证时钟身份、递增序列、新鲜度和有限位姿；只有 TF 队列满被计数丢弃，其余 TF 错误仍使测试失败。导航算法、Manager 与原有非 IPC 测试的 16 个文件再次对照重构前快照，逐字节一致。

重新构建 Debug 原生目标并执行完整 CTest：**22/22 PASS，48.07 s**，证据为 `target/navigation-acceptance/tf-ownership-ctest-debug.log`。包括绕障、RPP-only、导航强杀、Guard 强杀、两层消息过期、控制来源冲突、急停与最终 cmd_vel 检查。watchdog/expired 日志实际记录外部 fixture 的 `QueueFull`，仍分别在 201 ms 确认新鲜零速；Guard 强杀和最终流过期也在 201 ms 确认底盘自身 watchdog=1。原有 500 ms 上限、至少 200 ms 观察、多帧停止与碰撞断言保持不变。

该轮代码修改仅涉及两个测试进程及其启动脚本；Release/ASan/UBSan 覆盖的纯核心未改变，沿用上方已执行的各 9/9 记录，没有重复声称运行这些配置。硬件 YAML 选择、TF 外部归属及 ROS 2 IMU 单位已写入接入契约。当时原生驱动尚未移植；下面记录后续实现。

## 2026-10-22：原生底盘与 IMU 驱动补齐

新增独立 `rbt_base_controller`，读取原 DDSM 当前 YAML、消费现有最终速度消息、独立执行超时/急停/反馈故障停车并发布轮式里程计，不发布 TF。新增独立 `rbt_dm_imu`，串口完整帧校验后发布 SI 单位 IMU。两个驱动均不依赖 ROS 2，导航算法与 Manager 未修改。

底盘 Debug / Release / ASan+UBSan 分别 **8/8 PASS**；IMU Debug / Release / ASan / UBSan 分别 **1/1 PASS**。真实驱动可执行程序、Guard、导航和串口伪终端的原生 RBT IPC 场景 **3/3 PASS，6.76 s**，涵盖导航目标到达、导航崩溃、Guard 崩溃、最终消息过期/来源冲突、急停及实际串口零速。两个驱动同时启用 ASan+UBSan 的 pipeline 也通过，两个驱动均正常退出且无 Sanitizer 诊断。完整证据、限制与复现见 [驱动验收记录](../../drivers/VERIFICATION.md)；实车尚未运行。

## 2026-10-22：通用栅格地图文件接入

新增独立 `map_io` 组件和 `rbt_navigation_map_server`，直接读取常见 YAML + PGM/PNG，发布已有 OccupancyMap。支持 PGM P2/P5（含 16 位）、PNG、相对图像路径、分辨率、旋转原点、negate、阈值和 trinary/scale/raw 模式。现有核心、算法、Manager 和 wire 布局未改变；图像/YAML 依赖不进入原消息接口。

| 本次实际执行 | 结果 | 工作区证据 |
|---|---|---|
| Debug，全套原生测试 | **25/25 PASS，68.55 s** | `target/map-io-acceptance/ctest-debug.log` |
| Release，核心 + 文件加载 | **10/10 PASS，0.22 s** | `target/map-io-acceptance/release-ctest.log` |
| ASan，核心 + 文件加载 | **10/10 PASS，0.88 s** | `target/map-io-acceptance/asan-ctest.log` |
| UBSan，核心 + 文件加载 | **10/10 PASS，0.68 s** | `target/map-io-acceptance/ubsan-ctest.log` |
| 安装后 map_io 消费者 | **PASS**，从安装接口加载样例 | `target/map-io-acceptance/consumer-map_io.log` |
| 安装后消息消费者 | **PASS**，显式禁用 PNG/YAML 查找，链接也没有这些库 | `target/map-io-acceptance/consumer-messages.log` |
| CLI 正常/异常输入 | **7/7 PASS**；help、短时运行，以及缺参、文件不存在、NaN、重复参数、空 prefix | `target/map-io-acceptance/cli.json` |

Debug 是之前 22 项加文件加载单元测试与两个五进程场景；原有测试没有删除。Release/ASan/UBSan 使用 `RBT_NAVIGATION_NATIVE=OFF`，包含新的 `map_io` 测试；外部 yaml-cpp、libpng、zlib 本身未由这些配置重新插桩。Sanitizer 环境仍为 `detect_leaks=1:halt_on_error=1` 和 `halt_on_error=1:print_stacktrace=1`。单元测试覆盖上下翻转、元数据、三种模式、透明度、16 位像素、二进制空白字节、浮点阈值边界、重复 YAML 字段、超限、非有限几何与截断的图像数据。

五进程场景中 fixture 完全不创建地图 publisher，地图唯一来源是读取 YAML/PGM 的独立进程。`file-map-obstacle` 使用原墙体、0.18 m footprint 和全部路径/位姿线段碰撞断言，44 点路径绕墙，目标成功；最终位姿约 `(4.95655, 1.06057, 0.03614)`，速度为零，全部五个进程正常退出。`map_dropout` 在实际运动后只杀地图进程；TF、runtime、Guard 和底盘继续运行，导航以 StaleMap 失败，2531 ms 内确认 5 条新鲜零速底盘状态及 7 条最终零速命令，Guard/底盘 Watchdog 均为 0。该窗口包括 2 s 地图超时与减速/多帧确认，原断言上限为 3 s，不是物理制动时间。只有地图进程退出码为预期 -9。

JSON 和各进程日志分别在 `target/navigation-build/native-evidence/file-map-obstacle/`、`file-map-map_dropout/`。本次矩阵汇总为 `target/map-io-acceptance/matrix.json`。样例仍是合成地图，不代表已经完成真实 SLAM、定位或实车验收。

### 当前工作区的图像依赖

直接使用系统 libpng 1.6.57 曾在旧 sysroot 链接阶段报 `pow@GLIBC_2.29` 未解析。现使用同一 GCC 15.2 工具链从官方 v1.6.57 源码构建独立 `target/map-image-sdk`；不是通过绕过链接错误解决。源码归档 SHA-256 为 `4cbb7b0746edc1683c9581b373365e955133b7f1243f171b7d1535b4415dfedb`。URL、编译器、安装路径与保留的 LICENSE 位于 `target/map-io-acceptance/image-dependency.json`。通常使用系统编译器和开发包的环境无需此步骤。

本工作区已有 `target/libpng-1.6.57.tar.gz` 和解压源码，依赖构建命令如下。归档来自 `https://codeload.github.com/pnggroup/libpng/tar.gz/refs/tags/v1.6.57`，未将第三方源码复制到 navigation 包。

```bash
cmake -S target/map-image-source/libpng-1.6.57 -B target/map-image-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$PWD/target/phase4-toolchain/bin/x86_64-conda-linux-gnu-gcc" \
  -DCMAKE_MAKE_PROGRAM="$PWD/target/phase4-toolchain/bin/ninja" \
  -DCMAKE_INSTALL_PREFIX="$PWD/target/map-image-sdk" \
  -DPNG_SHARED=ON -DPNG_STATIC=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF \
  -DZLIB_INCLUDE_DIR="$PWD/target/phase4-toolchain/include" \
  -DZLIB_LIBRARY="$PWD/target/phase4-toolchain/lib/libz.so"
cmake --build target/map-image-build --parallel 3
cmake --install target/map-image-build
```

随后按 README 的工作区 PNG/ZLIB 路径配置 navigation。测试使用 `LD_LIBRARY_PATH` 中的 `target/phase4-toolchain/lib` 和 `target/map-image-sdk/lib`；实际可执行程序的依赖解析也已检查。消息消费者的隔离检查显式设置 `CMAKE_DISABLE_FIND_PACKAGE_PNG=TRUE` 与 `CMAKE_DISABLE_FIND_PACKAGE_yaml-cpp=TRUE`，并未只依据 CMake 链接声明推断。

## 2026-10-22：复杂 PGM 地图仿真

新增多房间与仓库两张标准 YAML/PGM 地图、确定性生成脚本、通用地图仿真 fixture 和六进程运行/视频脚本。原生地图服务器是唯一地图发布者；fixture 从同一 YAML 读取地图，仅发布外部 TF 和目标，检查全部收到的路径、执行位姿及相邻位姿线段。A*、ESDF、RPP/DWPP、Manager、速度与碰撞参数没有修改。

使用实际 C++ 地图读入器和 A*、0.28 m 规划半径预检，两张地图的直达线段均被挡住，规划长度分别为 11.5196 m / 11.8326 m。随后新增两个原生 CTest 场景 **2/2 PASS，52.47 s**；证据为 `target/complex-map-check/ctest-complex-final.log`。本次另执行核心、地图文件加载和原生适配的 11 项检查，均通过，见 `ctest-complex.log`。该初次运行还记录了两个新场景的启动脚本命名遮蔽错误；修复脚本后单独重跑两个场景通过，没有放宽路径、碰撞、时间或停车断言。

| 实际运行指标 | 多房间穿门 | 仓库绕行 |
|---|---:|---:|
| 地图大小 / 分辨率 | 12 × 8 m / 0.1 m | 12 × 8 m / 0.1 m |
| 首条 A* 路径点 | 94 | 105 |
| 实际执行轨迹长度 | 11.1980 m | 11.3650 m |
| 目标执行时间（含到达后零速确认） | 23.875 s | 25.618 s |
| 记录的底盘状态 | 1243 | 1331 |
| 最终位置误差 | 0.07654 m | 0.07420 m |
| 采样位姿的最小轮廓到障碍间隙（独立几何复核） | 0.17089 m | 0.15075 m |
| 最终执行速度 v / ω | 0 / 0 | 0 / 0 |
| 导航重规划转换次数 | 0 | 0 |
| 子进程退出码 | 全部 6 个为 0 | 全部 6 个为 0 |

物理圆形半径保持 0.18 m，规划净距保持 0.10 m。fixture 严格核对来源 100、递增序列、完整 HostClock 身份、新鲜度、正且不超过 100 ms 的最终命令 TTL；到达后要求至少 5 个连续新鲜停车底盘状态和 3 个实际为零的最终速度命令。轮廓与路径检查覆盖所有接收样本和相邻样本间线段。独立复核另外使用原栅格方形区域的精确点到矩形距离，验证采样位姿的障碍间隙；它不单独声称连续时间物理制动保证。

原始成功用例位于 `target/navigation-build/complex-evidence/rooms-cfb9a1d0/` 和 `warehouse-ee90c239/`。视频阶段将成功证据逐文件核对后复制到 `output/navigation-complex/{rooms,warehouse}/`，来源散列见 `evidence-index.json`。独立几何审计为 `output/navigation-complex/independent-audit.json`。

两个 MP4/GIF 都从同次真实 IPC 采样渲染，标注 **2× 回放**，位置取最近的实际样本，无插值；地图来自捕获消息，初始 A* 路径只在记录的发布时间后显示。状态为 Succeeded 但速度尚未归零时显示“减速中”。渲染结束、最终总览图经视觉检查；`video-metadata.json` 记录帧数、采样数、末帧位姿和零速。复现命令见 README 的复杂 PGM 场景章节。

本次只新增测试/演示和地图资源；未重跑未改变的 Release/ASan/UBSan 配置，沿用上节各 10/10 记录。没有接入真实机器人、SLAM、传感器误差或动态障碍物。

## 2026-10-27：MID360 / SuperLIO 原生接入

新增原子 `LioNavigationFrameV1` 能力、LIO 组件桥接和实时障碍叠加。A*、ESDF、RPP、DWPP、NavigationManager、runtime、Guard 与原测试源码未改；完整文件对照见 [changes.json](../../output/navigation-lio/changes.json)。架构图和消息契约见 [lio-integration.md](lio-integration.md)。

最终构建与 CTest 均返回 0，包含全部原有用例和新增的 13 项测试：

| 配置 | 结果 | CTest 并行度 | CTest 实际耗时 |
|---|---|---|---|
| Debug | 40/40 | 3 | 103.18 s |
| Release | 40/40 | 3 | 101.31 s |
| ASan | 40/40 | 1 | 192.19 s |
| UBSan | 40/40 | 3 | 100.36 s |

使用 GCC 15.2，SDK 为现有 RBT 0.8.0。ASan 设置 `detect_leaks=1:halt_on_error=1`，UBSan 设置 `halt_on_error=1:print_stacktrace=1`；无最终 sanitizer 报告。插桩覆盖 navigation 构建目标，预构建的 SDK/iceoryx2 和 ARM GTSAM 不宣称全库插桩。命令、初次失败、最终运行与日志路径均保留在 [matrix.json](../../target/navigation-lio-matrix/matrix.json)；独立 Debug 日志为 `target/navigation-lio-debug/ctest-final-40.log`。

四配置全量完成后，坐标复核修正了新增生产者的车体高度基准，并修正了单次超限时钟测量造成永久停发的问题。按影响范围重新构建四配置，各自的 `navigation_lio_receipt_clock` 与 `navigation_lio_output` **2/2 通过**，记录在 [clock-recovery.json](../../target/navigation-lio-matrix/clock-recovery.json)。ARM 同样重新构建 LIO/导航，四项新增单元测试 **4/4**，随后执行下述带结束新鲜度检查的真实生产者链路。未改变的算法与 IPC 场景使用上表全量结果；没有把这些定向复测称为第二次全量执行。

新增 IPC 覆盖：动态障碍出现、受观测射线支持的清除、持续障碍绕行并到达目标、定位标志失效、旧帧、未来帧、外来时钟、LIO 源崩溃、桥接崩溃和错误 provider。崩溃测试先证明机器人已运动，再验证无新鲜里程计、导航失败和最终速度/模拟车体停车；最终速度话题使用独立测试前缀隔离，避免向真实 `/cmd_vel` 发测试运动。底盘模拟继续有自身命令 watchdog。持续障碍场景检查实际车体与扫掠线段碰撞，目标成功时障碍仍存在。轨迹和原始证据见 [avoidance](../../output/navigation-lio/avoidance/provenance.json)。

AidLux ARM64 GCC 11.4 验证：

| 检查 | 真实结果 |
|---|---|
| 现有 MID360 / SuperLIO 测试 | 9/9，通过原生 UDP、Composer 恢复和 provider 替换用例 |
| 新增导航单元测试 | 4/4 |
| 新增导航 IPC 场景 | 9/9，31.34 s |
| 模拟 UDP → 真实 MID360 驱动 → 真实 SuperLIO → 新导航能力 | mode 0 在 8 s 探测窗口收到 152 帧，最大时基映射不确定度 218,063 ns；结束前最近有效帧年龄 6,287,476 ns；0.735 m 安装高度断言通过 |
| 未校准设备时钟 mode 1 | 旧 LIO 里程计活跃，新导航能力 4 s 内输出 0 帧，正确拒绝 |

这两种真实生产者链路检查仍使用 localhost 合成 UDP；先验定位关闭，因此收到的导航帧 flags=0，不能驱动导航。这证明了真实驱动/算法/协议路径，不能替代真实雷达与已有地图定位联调。最终日志和 JSON 在 [生产者结果](../../output/navigation-lio/aidlux-evidence/source-integration-20261027T040536Z-2f51ba00/result.json)。断流反例先收到 56 帧，UDP 在 4 s 停止；窗口结束前最近有效帧已老化约 5.13 s，probe 明确返回 1。反例验收通过，见 [negative-result.json](../../output/navigation-lio/aidlux-evidence/source-stop-negative-20261027T040556Z/negative-result.json)。`ldd` 审计在现有 `scripts/env.sh` 环境下无缺失库，也无 rcl/rmw/ROSIDL 运行库依赖；复用既有 GTSAM/METIS 安装路径，不引入 ROS 2 runtime。

过程中的真实失败与处理：

- 新测试最初复用了上次 JSON，崩溃场景提前杀死源；改为每次独立路径，并隔离模拟底盘与 LIO 的里程计前缀。provider 拒绝通过 Composer 的真实状态快照确认。
- 初始 0.10 m 全局余量在持续障碍拐角处出现 StartOccupied；统一膨胀足迹的尝试仍触发碰撞重规划上限。最终保持算法不变，以 0.18 m 车体、0.30 m 全局余量为离散 A* 拐角的 RPP 曲线跟踪留空间；上述四配置与 ARM IPC 均重新通过。该配置仍需按实车尺寸、速度和场地验证。
- ARM namespace 校验使 20 μs 相关窗口不足；改为测量窗口最多 200 μs，并把实际窗口半宽纳入误差界。最坏 230,512 ns 仍低于 250 μs 帧上限；大于 200 μs 的窗口依旧拒绝。
- 最后复核发现旧里程计固定 `base_z` 的做法不能用于与原始点云同基准的新导航帧。增量补丁只去掉新增帧的 z 钳制，旧里程计不变；新增 0.735 m 安装高度、非零 map 高度与 0.20 m 障碍的投影/高度过滤回归。
- 一次 ARM 运行收到 56 帧后，单次相关读数超过 200 μs 导致永久停发；旧的“至少 5 帧”判据曾漏掉它。日志保留在 `source-integration-20261027T004113Z-c8f23dd3`，不作为最终持续输出验收。现改为超限测量不刷新校准、拒绝当次转换，后续测量仍须通过上次有效采样的 200 ms 间隔和跳变检查。测试另要求窗口结束前 250 ms 内有有效帧，并以真实 UDP 断流反例验证不会误报成功。
- 新 ARM 构建首次未找到既有 GTSAM 配置、未加载环境时找不到 METIS；指定既有配置并 source 原包环境后，原有 9 项全部通过。
- ASan 并行全量曾在功能场景已成功后，因原测试 0.75 s 清理宽限触发 SIGKILL。未删除测试或放宽检测；保留失败，完整串行重跑 40/40，通过泄漏检查。

远端源补丁只涉及既有 LIO 的 7 个文件，并经 SHA-256 检查和备份。独立构建为 `/home/aidlux/mid360_lio_rbt/build-navigation`；导航交付在 `/home/aidlux/rbt-navigation-20261027`。原 YAML 未改，新能力默认关闭；单独的 `super_lio-prior-review.yaml` 已通过 `--check-config`，未启动实机。

用户地图 `/home/sw/Downloads/jetson-map-review.zip` 中，同源 PCD 与 PGM 已提取并校验 SHA-256。PGM 269×179、0.05 m 分辨率，含 7,980 个障碍格、40,171 个未知格、0 个自由格；原生 map_server 加载成功。归档无轨迹或可确认传感器原点，不能据此生成有观测依据的自由空间。没有把未知区涂白来使规划通过。地面参考、安装外参和实时先验定位也尚未实机确认。当前不能把这份地图作为可行驶导航地图，尚未运行真实电机或雷达。审计见 [map-audit.md](../../output/navigation-lio/map-audit.md)，完整最终结果见 [acceptance.json](../../output/navigation-lio/acceptance.json)。


### 2026-10-27：按用户确认修正未知格

用户确认未知格来自PCD转栅格程序，要求改成白色自由格。新增 `output/navigation-lio/maps/navigation-map.pgm/yaml`，将40,171个205像素改为255，保留7,980个黑色障碍格、全部地图几何和原始图。当前地图自由格40,171、障碍格7,980、未知格0。

使用现有Release `map_io` 库逐格核对原图与新图，转换、障碍和几何检查通过；现有原生map_server加载/发布通过。证据为 `output/navigation-lio/maps/navigation-map-validation.log`、`navigation-map-server.log`。本次只修改地图资产与说明，未修改导航算法或重新执行上表40项矩阵。此前“0自由格”的描述仅对应保留的原图；当前导航使用 `navigation-map.yaml`。


### 2026-10-27：真实地图闭环与首次先验定位补齐

当前PGM已通过原生六进程闭环：右侧绕障路线目标成功并停车，1388条采样，全部车体/路径点与线段碰撞断言通过，全部进程退出0。原rooms/warehouse场景回归2/2、52.28 s。新localized探针在Debug/Release/ASan/UBSan各通过有效帧与过期帧两种原生能力场景，无Sanitizer诊断。

发现早期构建和review配置均关闭KISS，缺少首次map→odom初始化路径；新增独立KISS构建与主动配置，原构建/原YAML保留。AidLux真实PriorMapLocalization使用PCD派生的非零变换扫描初始化通过，位姿误差4.984 mm/0.037672°，原门限0.01/0.1不变，2.2 s后新鲜度失效；新增1/1，原MID/LIO9/9通过。新构建的真实驱动/算法合成UDP两种时钟模式也通过，mode0为153帧、结束年龄21.137077 ms，mode1为0帧。

本次增量未重跑此前完整40项矩阵。离线点云/栅格48151格完全一致，原外参保持不变。用户确认雷达未上电连接，因此实际传感器验收是“0帧，失败”的负例，不作为实机通过。完整证据、限制和上电操作见 [map-readiness.md](map-readiness.md) 与 `output/navigation-lio/readiness/`。

### 2026-10-27：按实机一致的 robot-sim 校正安装配置

SSH只读检查 `192.168.1.153:/home/sw/robot-sim` 的实际启动模型；源码、安装文件与留存快照4/4哈希一致。`base_footprint←mid360_rgl_frame` 为平移 `[-0.053,0,0.735] m`、roll=180°，Gazebo实际雷达pose与固定关节链一致。模型的虚拟IMU不作为实机内部标定参考。

保留 `lidar_imu`，仅将活动定位YAML中的有效IMU车体外参 `odom_robo` 换算为 `[-0.042,-0.02329,0.77912,180,0,0]`。旧配置对实际雷达测量原点产生51.088 mm偏差，审计按预期失败；新配置审计通过，数值平移误差约1.55e-17 m、旋转误差0。独立测试5/5、0.031 s，覆盖旧配置偏差、新配置匹配、传感器pose冲突、非零内部旋转和非数值偏移；测试自带XML夹具。首次测试运行中的夹具变量名错误已修复，初次日志保留。AidLux实际KISS构建的SuperLIO加载活动YAML，`--check-config` 返回0。按新配置复查PCD/PGM仍为48151格一致、差异0。

活动配置同步至AidLux原来的上电检查入口。此次只涉及配置、离线审计脚本/测试和文档，C++算法、原生IPC、生产者和底盘未修改，没有重新执行完整Debug/Release/Sanitizer矩阵，也未启动雷达或底盘。来源、换算和全部增量证据见 [robot-mount.md](robot-mount.md) 与 `output/navigation-lio/readiness/robot-mount/`。

### 2026-10-28：真实MID360网络与先验定位只读观察

AidLux `br-lan` 为 `192.168.5.5/24`，实际MID360为 `192.168.5.3`。原生驱动活动配置已同步为接收IP `192.168.5.5`、雷达IP `192.168.5.3`，点云/IMU端口56301/56401、雷达源端口56300/56400；原ROS 2 `MID360_config.json`快照记录host IP `192.168.5.5`和历史设备IP `192.168.5.170`，不能据此确认当前ROS 2启动参数。

5 s被动监听实际收到10,418个点云包和1,000个IMU包，时基mode 0。随后25 s真实传感器只读定位观察收到242帧，全部未定位，有效定位帧0，最大时基映射不确定度211,623 ns。KISS候选被VGICP初次fitness门限0.01拒绝（记录值1.66769），没有产生 `map→odom`；探针返回1。驱动与SuperLIO退出码为0，Composer按流程以-15结束，无强制杀进程。该次结果确认真实数据链路，不是定位通过；当时是否位于与 `prior.pcd` 对应的场景仍待确认。

本次未启动导航、Guard、底盘或电机，未发送 `/cmd_vel`，也未重跑完整40项测试矩阵或重建代码。证据见 [被动网络记录](../../output/navigation-lio/readiness/hardware-20261028/passive-network.json)、[定位结果](../../output/navigation-lio/readiness/hardware-20261028/localization-20261027T174853Z-d8eb1bf7/result.json)、[探针结果](../../output/navigation-lio/readiness/hardware-20261028/localization-20261027T174853Z-d8eb1bf7/probe-result.json) 与 [ROS 2配置快照](../../output/navigation-lio/readiness/hardware-20261028/ros2-MID360_config.json)。

### 2026-10-28：新场地一分钟建图及重新定位检查

用户确认场地已更换。使用当前原生驱动和LIO，地图初始化后采集60.000476 s，正常退出保存PCD：12片段合并为5,871有限XYZ点、94,122字节，SHA-256为 `fc4ba3ee1857a3710891b6da6ed7e2804516257a2aae6d9a83f88cb3c234a55a`。新进程成功加载本次地图，原门限下25 s检查未通过：242帧均未定位，最好注册分数0.0101882大于初次门限0.01，有效定位帧0。原始失败结果保留，未放宽门限或宣称成功。

旧地图保留，新地图未替换活动导航地图；两个阶段均未启动导航、Guard、真实底盘或ROS 2运行时。C++和原生协议未修改，未重新执行全量Debug/Release/Sanitizer矩阵。地图、配置与各进程日志见 [acceptance.json](../../output/navigation-lio/new-scene/mapping-20261027T190444Z-69ee692b/acceptance.json)。

### 2026-10-28：稠密重采及真实先验定位通过

重采时取消输入抽点（filter_rate=1），使用现有map.if_filter=false保存完整有效的去畸变扫描，在线匹配0.5 m体素和外参不变。初始化后计时60.000428 s，正常保存6,303,245个有限XYZI点、100,852,112字节，SHA-256为 `8c873501e6dc8a5c81c22fe9efb1f95ef4152743cf916a164f1c6c1f5f0066bf`；现成PCL工具以5 cm体素导出16,921点地图，原始文件保留。

实际冷启动LIO重新加载本次5 cm PCD（内部0.1 m体素后4,141点），25 s定位验收通过：242帧中233帧有效定位、9帧等待，结束新鲜度21.668270 ms，最大时基映射不确定度224,049 ns，runtime状态0。47次接受注册的fitness为0.000741976至0.00109322；初次0.00109322低于保留的0.01门限。驱动、LIO和probe均正常退出0，未强杀。

本次没有修改C++或消息协议、没有重跑全量Debug/Release/Sanitizer矩阵，也没有启动导航、Guard或底盘。验证范围为本次场景位置附近的真实PCD重载定位；不宣称移动定位、绝对定位精度或整机导航已验收。全部证据见 [稠密地图验收](../../output/navigation-lio/new-scene/dense-mapping-20261027T191536Z-d9d6148e/acceptance.json)。
## 2026-10-28 前驱底盘 State Lattice

本轮新增整车多边形、轮轴参考点、State Lattice 和旋转/换向路径处理。完整构建、消息契约、独立扫尾反例、原生 IPC 与安装消费验证见 [lattice-verification.md](lattice-verification.md)。历史记录保留在本文；本轮结果以新记录为准。

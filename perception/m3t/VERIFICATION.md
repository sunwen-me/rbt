# 验证记录

## 2026-10-26：Pitch 相机运动补偿接入

证据位于 `target/so101-base-reference-20261025/integration/`；板端构建记录位于 `~/rbt-work/manifests/pitch-integration-20261026/`，修正配置后的实机采集位于 `~/rbt-work/runs/pitch-integration-20261026/capture-06/`。capture-05 保留为错误配置的对照证据。

| 项目 | 状态 | 证据与范围 |
| --- | --- | --- |
| 定向软件测试与 ARM64 部署 | PASS | 本机定向测试通过；ARM64 最终 6/6（0.72 秒），覆盖最近样本、单样本、旋转组合、时钟/epoch、并发接收及原有帧/先验路径。当前二进制 SHA-256 为 `f4af6df362aa89f61e35cc4edb3d715e45634ae1445871d2ca7d588926d8bb67`。 |
| 真实 RGB-D / pitch TF / 双臂编码器 / M3T | PASS：链路 | capture-06 的 30 帧估计全部携带 nearest pitch 及 10 项实测 q1–q5 先验。反馈时间偏移范围 −41.615 至 +11.466 ms，绝对值中位数 13.097 ms；采用 HostMonotonic received best-effort，精度仍未知。 |
| 队列与资源退出 | PASS：本次窗口 | 初次联机暴露 GL 计算阻塞反馈接收、导致 pitch Control 队列满而退出的问题，现改为 activation-owned 独立接收任务及有界互斥缓存；最终窗口无 QueueFull，所有自有进程组已回收。 |
| 电机读写范围 | PASS：只读 | 两臂 `writes_enabled=false`；pitch 仅轮询寄存器，未下发目标、使能或力矩控制命令。用户认证后只启用了 can0 的 1 Mbps 通信。 |
| 轴向修正接入错误 | FIXED：配置一致性 | 最初预览采用绕 colour 光心的 180° roll，但 capture-05 错用了绕 depth 光心的 roll。设备 SDK 的 depth←colour 外参使 colour 光心偏移 28.238 mm。现按 `M_fixed=M_original E S E^-1` 修正 mount，保持 SDK 权威外参 E，恢复原预览的 colour 光心。只改 mount 配置，未拟合外参。推导与设备只读查询见 `integration/rotation-audit/`。 |
| 实拍模型对齐 / 空间精度 | NOT ACCEPTED | capture-05 同一冻结帧、同一编码器 FK 对照：修正前后深度残差中位数左 150.2→14.0 mm、右 162.2→85.0 mm；这些是包含遮挡和背景的图像诊断，不是真值位姿误差。右臂仍明显失配；固定 world 底座及强关节先验不能补偿错误安装/零位。capture-06 彩色图几乎全黑，仅接受链路结果；q6 未标定。 |
| 云台主动运动验证 | NOT RUN | 本轮保持实物姿态，只接收实时反馈。 |

同帧配置对照图及审计 JSON：`integration/rotation-audit/pitch-capture-05-same-frame-camera-audit.*`；修正后真实运行图：`integration/overlay/pitch-capture-06-three-panel.*`。原始失败窗口、错误配置、缓存/路径修复记录与光学朝向候选均保留。配置及最近样本策略见 [README](README.md)。

### 去掉新增外参的对照

按用户要求关闭 `camera_motion`，保留相机内参与 SDK D2C 对齐、相同关节先验和模型参数。`capture-07-no-extrinsics` 仅关闭运动/外参链，保持两底座固定；`capture-08-original-baseline` 使用原 `diagnostic-tracking.json`，同时恢复 `fixed_base:false`，完整恢复接入前的行为。两次真实运行各输出 30 帧、每帧 10 项先验，输出不含 camera_motion；未启动 pitch provider，电机只读，所有自有进程组已回收。

`integration/no-extrinsics/encoder-fk-comparison.*` 在 capture-05 同一明亮帧、同一编码器角度上对比当前外参链和旧 `base_in_camera` 初始化。投影深度残差中位数左 14.0→13.6 mm、右 85.0→44.3 mm；右臂投影改善，仍未完全重合。这是 FK 投影诊断；两次实时 M3T 运行使用不同图像，不能当成同帧追踪精度 benchmark。完整记录见 `integration/no-extrinsics/comparison-status.json`。新增对照配置为板端 `~/rbt-work/config/m3t/pitch-disabled-comparison.json`；原 pitch 配置保留。

## 2026-10-24：实机同窗口 RGB-D / 编码器采集

`target/m3t-so101-live-prior-20261024/STATUS.json` 记录本轮状态。Gemini 335（`CP15641000AT`）收到 30 对 RGB-D，同时从双臂原生 SO101 `StampedBytes` 接收 145 组状态和对应原始编码器读数。保存图像与关节状态共享 HostMonotonic source/generation；所选因果样本序号 123，早于图像接收时间 22,332,710 ns。采集期间所有关节读数范围均为零。该结果验证同一窗口中的接收时间关联，不代表曝光同步。

相机、驱动、Composer 与探针均由脚本退出并回收，进程组为空，未发送电机目标或配置写入。新增快照时间字段经 AidLux 原生 OpenCV 4.5.4 `FileStorage` 读取检查通过；未知不确定度用 `*_uncertainty_known: false` 和缺省数值字段表示。

**实机对齐仍未验收。** 两侧肘关节直接换算约 91.52° / 93.10°，超过模型 90° 上限，原始数据未夹紧。当前腕部几何候选无法同时解释腕部相机固定分支；近似 PCB 中心的独立检查也未通过，尚不能确定是对应点、姿态还是固定安装关系的问题。未用这些候选启动实机 M3T 追踪。第一组诊断图为 `target/m3t-so101-live-prior-20261024/pose-A-diagnostic.png`；第二姿态需待用户完成腕旋转调整后采集。软件先验功能的验收与该物理标定问题分别记录如下。

## 2026-10-24：SO-101 关节先验路径

关节先验的原生单元、原生软件集成和 ARM64 CPU-only 构建检查已通过。所有软件运行均使用合成 fixture；真实 SO-101 驱动映射、相机时间关联和机械臂精度仍未验证。

| 项目 | 状态 | 证据与范围 |
| --- | --- | --- |
| 原生模块单元测试 | PASS | `target/m3t-joint-prior-20261024/ctest-final.log`：5/5，0.71 秒；覆盖 frame source、model、app config、joint prior 和 joint-prior optimizer。 |
| 原生 SO-101 `StampedBytes` 先验集成 | PASS | `target/m3t-joint-prior-20261024/native-integration-final/result.json`：原生 Composer、RGB-D/关节状态流与 optimizer，55 帧，13 帧应用先验；前 10 个应用帧最大角度差为左 0.005649 rad、右 0.004851 rad。停止发布后最后 5 帧均为 stale 且无先验因子。配置显式采用 `received` best-effort 关联，`capture_time_associated` 为 false，不代表曝光时间同步。 |
| 集成进程清理 | PASS | 同一 `native-integration-final/result.json`：所有子进程已回收、进程组为空、`forced_kill` 为空。 |
| 先验默认关闭的基线 | PASS | `target/m3t-joint-prior-20261024/default-disabled/result.json`：独立原生消费者收到 20 帧双臂 baseline 输出，序列 0–19；子进程均回收且无强制终止。 |
| ARM64 CPU-only 构建及安装 | PASS | `target/m3t-joint-prior-20261024/arm64/REPORT.md` 与 `arm64/remote-logs/final/run.log`：GCC 11.4，5/5 聚焦 CTest（0.45 秒）；隔离 stage 安装成功，`rbt-m3t` 与 `rbt-m3t-initialize` 的 `--help` smoke checks 通过。未运行相机、驱动或机械臂。 |
| 实际驱动 service、相机时间关联和模型映射 | NOT_RUN | 合成软件 fixture 不验证真实驱动数据、相机曝光同步或实测模型映射。 |
| 实际相机与机械臂先验精度 | NOT_RUN | 需要使用真实硬件和经校准的显式关节映射进行验证。 |

需要重跑时，使用新建的输出目录：

```bash
ctest --test-dir build/m3t --output-on-failure -R 'frame_source|model|app_config|joint_prior'
python3 perception/m3t/scripts/verify_joint_prior.py \
  --build-dir build/m3t --composer "$RBT_PREFIX/bin/composer" \
  --output-dir target/m3t-joint-prior-verify-rerun-1
```

该 verifier 把 native Composer、RGB-D fixture、SO-101 状态发布器与 `rbt-m3t` 连接起来，检查先验路径和 stale 样本丢弃。输出目录必须尚不存在。上述软件验证只覆盖合成输入；此前下方的构建、DSP 与合成追踪记录保留原样，不代表真实机械臂精度。

2026-10-21：软件 pipeline 已通过；按用户要求，真实双臂的初始化和追踪效果留待后续实机验证。

| 项目 | 状态 | 证据与范围 |
| --- | --- | --- |
| 固定上游及集成构建 | PASS | M3T 固定提交的 30 个核心源文件、`rbt-m3t`、`rbt-m3t-initialize` 和测试程序均完成编译链接。 |
| C++ 模块检查 | PASS | `target/m3t-ctest.log`：3/3，覆盖像素所有权、配对、时钟/标定、FK、关节限位/跨 π 展开及配置/输出。 |
| URDF 转换器 | PASS | 6/6，包含实际外部 URDF 的只读转换；生成 10 links、9 joints、6 个活动关节、19 个视觉元素和 400,798 面。 |
| 原生软件 pipeline | PASS | `target/m3t-software-fixture-20261021-final/result.json`：渲染 RGB-D → 原生 RBT Color/Depth/CameraInfo → M3T Region+Depth → 独立原生 RBT 消费者，接收 20 批双臂结果。 |
| 软件轨迹及结果正确性 | PASS | fixture 每臂 2 links、1 个活动关节。检查完整 SE(3)、单位、限位、时间戳和未知不确定度。左右臂 RMSE 分别 0.01174 / 0.04999 rad，冻结零角度的基线均为 0.27051 rad；两臂都有可测运动。此数据不是实际 SO101 的精度。 |
| 进程清理 | PASS | 软件验证全部子进程已回收，无 SIGKILL；未操作电机。 |
| Gemini 335 RGB-D 输入 | PASS | `target/m3t-hardware-20261021-light/result.json`：精确序列号 CP15641000AT，60 对 640×480 图像、D2C/标定/去畸变、深度单位 0.001 m、最大采集时间差 121,000 ns。保留实际设备时钟。 |
| OpenGL/Qt 基础设施 | PASS | 上游 RendererGeometry::SetUp、Mesa OpenGL 4.6、OpenCV 4.10、PNG 写入及 Qt 窗口 smoke；不是初始化器点选/PnP 验收。 |
| 上游构建 overlay | PASS | 三处修正：未知 Body ID 的返回路径、Renderer 空 vector 引用、Linux GLFW/GLEW 的 X11 选择。checkout 保持干净，构建目录生成的副本使用精确文本匹配。 |
| 本地安装 | PASS | `target/m3t-install`；已安装程序可运行，`ldd -r` 无缺失库或未定义符号，不链接厂商相机 SDK 或 ROS。运行库来自当前本地依赖前缀，该安装不是可独立搬运的发行包。 |
| 初始化文件发布 | PASS | `target/m3t-initialization-publish-test.log`：新文件、已有文件、悬空符号链接、已有临时文件及 16 个并发写入者；不覆盖原文件。检查直接提取生产函数，未运行 GUI。 |
| 真实双臂初始化/PnP | NOT_RUN | 用户决定先跑软件 pipeline，后续提供实际关节角和场景对应点。未生成虚构的实测初始位姿。 |
| 真实 SO101 追踪精度/遮挡恢复 | NOT_RUN | 软件 fixture 不能代替真实双臂；没有自动检测或丢失重定位的实现/精度声明。 |

运行方式见 [README](README.md)。本地集成构建使用 phase4 GCC/Ninja 和现有 RBT 0.8 SDK；此模块未修改 RBT 核心、协议、IDL 生成器或手写 unsafe。第三方完整许可和 overlay provenance 见 [NOTICE](NOTICE.md)、[UPSTREAM.toml](UPSTREAM.toml) 与 `licenses/`。


## 2026-10-23：ARM64 M3T 0.2.0 GPU-renderer 验证

此轮在 AidLux ARM64 设备上针对合成双臂 fixture 完成；不覆盖真实相机输入或实物机械臂精度。tracker 和 fixture 都实际使用 Mesa Zink / Turnip Adreno 740，OpenGL 4.6。编译、安装完成，`build-test-install.log` 中 3/3 CTest 通过（0.49 秒）。

| Profile | 状态与检查 | 关节结果 | 整次运行时间 |
| --- | --- | --- | --- |
| baseline | PASS；Region+Depth，20 批，序列 0–19 | RMSE 左/右 0.00926 / 0.05088 rad；两臂都有可测运动 | 7.153 秒 |
| extended + reinitialize | PASS；Region+Depth+Texture、modeled occlusion、region checking、YAML metadata、soft constraint；显式 SIGHUP 重初始化事件通过；20 批 | RMSE 左/右 0.01088 / 0.01428 rad；两臂都有可测运动 | 6.833 秒 |
| hard constraint | PASS；20 批 | 输入各臂移动 0.2112 rad，输出运动约 2e-8 rad，RMSE 与冻结零角基线相同；这是约束生效的专用检查，不表示精度提升 | 6.765 秒 |

以上时间是端到端运行时间，不据此推导 FPS。三个结果 JSON 均记录实际 renderer、`children_reaped: true` 和空 `forced_kill`。证据位于 `target/aidlux-m3t-0.2.0-validation/{build-test-install.log,baseline-gpu/result.json,extended-gpu-reinitialize/result.json,hard-constraint-gpu/result.json}`。

本环境验证了 CPU ORB；BRISK/SIFT 编译通过，但未分别进行运行时验证。CUDA 与 OpenCV contrib 不可用，CUDA ORB、DAISY/FREAK 未验证。Gemini 335 实机采集、真实 SO101 初始化/精度/性能均为 NOT_RUN。上游数据集 benchmark 示例未移植；每个约束只作用于一个 arm 内的 link pair，不支持跨 arm 或跨独立 optimizer coupling。此验证不表示完整自动检测器或全 GPU optimizer 已接入。

## 2026-10-23：ARM64 M3T 0.3.0 Hexagon DSP ORB

此轮验证使用 AidLux ARM64 / Adreno 740 设备。Renderer 与 GPU fixture 均报告 Zink / Turnip Adreno 740；`hardware: NOT RUN` 表示 RGB-D 和双臂跟踪数据来自合成 fixture。DSP skeleton 在 CDSP 实际加载并执行，因此以下结果确认设备 DSP 后端功能，但不代表真实相机或 SO-101 验收。

| 项目 | 结果 | 范围 |
| --- | --- | --- |
| DSP-off build / software | PASS | ARM64 6/6 CTest；baseline 与 extended 合成 CPU texture profile 各 20 批。 |
| DSP-on build / install | PASS | ARM64 7/7 CTest；安装版本、runtime hash 与五个 DSP license 文件检查通过。 |
| V73 CDSP 独立 ORB 测试 | PASS | 七个提取案例、四个批量匹配案例；300×300 descriptor pair 返回 600 个邻居，每个非空矩阵操作一次 RPC，无失败。七个提取案例与 portable CPU core 的 keypoint 全字段及 descriptor 字节一致。 |
| CPU extended fixture | PASS | 20 批；Zink/Turnip Adreno 740；整次运行 6.716 秒；子进程均回收且无强制终止。该 profile 不执行 DSP。 |
| DSP extended + SIGHUP reinit fixture | PASS | 20 批；DSP audit 记录 112 次提取事件/RPC / 7,513 个特征、88 次匹配事件/RPC / 977 queries / 6,292 train / 1,954 neighbors，即每个非空 operation 一次 RPC；重初始化请求与事件通过；整次运行 13.855 秒，子进程均回收且无强制终止。 |

fixture 的两组整次耗时包含不同工作，不是 FPS，也不是性能对照。真实 Gemini 335 RGB-D 采集、SO-101 初始化和物理追踪精度仍为 **NOT_RUN**。

独立设备 microbenchmark 在 AidLux V73 上以 200×200 图像、300 个特征、3 层、5 次预热和 20 次采样执行，数值为毫秒 p50 / p95：

| 操作 | DSP facade | portable CPU core | OpenCV CPU |
| --- | ---: | ---: | ---: |
| ORB 提取 | 45.3244 / 45.4193 | 10.6742 / 14.1625 | 1.93681 / 5.7585 |
| 300×300 匹配 | 2.32049 / 2.49559 | 0.174942 / 0.185931 | 0.291188 / 0.489879 |

这组 microbenchmark 中 DSP facade 的提取和匹配均慢于所测 CPU 路径，不支持加速结论。DSP 数值实现使用固定点重采样/配额、Gaussian rounding 和 CORDIC，不能声称逐位兼容 OpenCV ORB；设备案例中的精确相等对象是 portable CPU core。主机仍负责 ROI、Lowe ratio、几何检查与 M3T optimizer，optimizer 在 CPU 上；本轮未实现 HVX 向量路径或 NPU。

M3T skeleton 为 Hexagon V68 交叉编译，在 V73 CDSP 上验证；这不证明它兼容所有 Hexagon 固件。最终 skeleton 和 ARM64 stub 的 SHA-256 如下：

| Artifact | SHA-256 |
| --- | --- |
| `librbt_m3t_orb_skel.so` | `10d2e3ecfebacb6b578d665b4e7d1118094fe2fcdd105d2e532019b2c82349d5` |
| `librbt_m3t_orb_stub.so` | `d229c3f5e0e63d6d3e3bcb619c675d48c527b01a4806a6b16d580d3a4ab65b4e` |

最终设备结果、CPU/DSP profile JSON 和验证证明随源码包放在 `verification/`。完整构建、安装和运行原始日志仍保存在源码仓库的 `target/aidlux-m3t-0.3.0-dsp-validation/final/`，不随包分发。最终独立设备 oracle 与 microbenchmark 输出为 `target/aidlux-m3t-0.3.0-dsp-validation/phase3/evidence/final-device-benchmark.stdout`，包含全部设备 oracle 检查；较早的 `target/m3t-hexagon-bootstrap-20261023/evidence/production-device-test.stdout` 是基线历史输出，不是最终完整结果。安装包已包含 `compiler-rt-LICENSE.TXT`、`fastrpc-LICENSE.txt`、`opencv-orb.txt`、`qaic-LICENSE.txt` 与 `qaic-NOTICE`。

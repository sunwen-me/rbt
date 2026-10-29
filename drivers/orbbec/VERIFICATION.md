# Gemini 335 驱动验证记录

更新日期：2026-10-21。当前驱动版本 `0.2.0`，RBT SDK `0.8.0`，固定 Orbbec SDK `2.10.6`，平台 Linux x86_64。前几节保留 0.1.0 的既有验证记录。

## 已执行的软件检查

| 检查 | 结果 |
| --- | --- |
| 使用真实 Orbbec 头文件及共享库编译、链接驱动，C++20 警告视为错误 | PASS |
| 不依赖 Orbbec SDK 的核心、C++ 消费者及测试构建 | PASS |
| C++ 编解码测试：格式、边界、单位、非法旋转、FrameId | PASS |
| 组件测试：真实原生 Stream、CameraInfo / Control RPC、静态快照 RPC、TF 方向 | PASS，SOFTWARE_FIXTURE |
| RPC 参数错误、不支持、SDK 错误及客户端超时不停止图像流 | PASS，SOFTWARE_FIXTURE |
| 任务停止与设备清理、重激活后新 capability 和时钟身份 | PASS，SOFTWARE_FIXTURE |
| 时间回退、彩色继续到达但深度断流时明确失败 | PASS，SOFTWARE_FIXTURE |
| Python 帧编解码测试，包括深度小端与比例、stride、边界、IMU 非有限值 | PASS，8 项（原编解码测试） |
| Composer + 测试发布组件 + C++ 消费组件跨进程运行，恢复静态 TF 快照 | PASS，SOFTWARE_FIXTURE |
| Python 原生端点接收、NumPy 数值及 `.npy` 文件 | PASS，SOFTWARE_FIXTURE |
| 测试子进程清理 | PASS |
| SDK 固定提交的全新部分克隆、逐文件校验及安装 | PASS |
| SDK 已有前缀重复校验、篡改前缀拒绝且保留原文件 | PASS |
| SDK-off 安装内容检查 | PASS |
| CLI 版本、参数检查、未激活时限时运行返回非零状态 | PASS |

可重复的软件入口：`scripts/verify_software.py`，命令见 [README](README.md)。
当前跨进程记录位于仓库 `target/orbbec-verification-acceptance/`，底层构建及 SDK 检查记录位于 `target/orbbec-upstream/`。

测试后端只链接进 `rbt_orbbec_component_test`，不会链接进 `rbt-orbbec-camera`；软件数据不能证明相机图像、标定精度或硬件时序。

## 首次硬件检查（IMU 修复前）

2026-10-20 首次枚举时，SDK 报告 USB 节点 `/dev/bus/usb/002/002` 访问被拒，序列号为空；安装 SDK 随附的厂商 udev 规则并重新插拔后，`list` 和 `profiles` 成功返回设备。设备为 Gemini 335（VID `2bc5`、PID `0800`、USB3.2），序列号 `CP15641000AT`，固件 `1.4.60`。SDK 在不同接口返回的名称为 `Gemini 335` 或 `Orbbec Gemini 335`；驱动只接受这两个精确名称，并继续校验 VID、PID 和完整序列号。空序列号时，CLI 会提示检查对应 USB 节点权限和 SDK 日志。

下表中的短路径均相对于仓库 `target/orbbec-hardware-20261020/`。

| 硬件检查 | 结果 |
| --- | --- |
| 设备发现、型号、序列号和固件 | PASS；`devices-after-udev.json`、`profiles.json`、`baseline/Log/OrbbecSDK.log.txt` |
| 彩色和深度，640×480、30 fps | PASS；彩色 RGB8 与深度 16 位数据实际输出，深度比例为 `0.001 m`；见 `extended/python_color.log`、`python_depth.log`、`pixel-stats.json` |
| C++ 消费者晚 5 秒加入及工厂 TF 查询 | PASS；取得连续 30 帧彩色图，并在实际采集时间查询到深度光学帧变换，采集时钟标识与 activation 一致。使用 `--allow-unknown-uncertainty` 显式允许采集时间精度未知；记录的平移为 `(0.014119, 0.000083, 0.001791) m`。见 `extended/cpp_color_tf.log` |
| IR、点云和 IMU | PASS；Python 诊断消费者对彩色、深度、左右 IR、XYZ、加速度和陀螺仪各接收 30 个样本。视频、IR、点云样本无序列间隙；XYZ 为有限的 float32 米制坐标。IMU 诊断样本观察到序列间隙：accel 63、gyro 40；一个加速度样本的模长为 `9.819 m/s²`。见 `extended/python_*.log`、`pixel-stats.json`、总记录 `result.json` |
| 只读相机 RPC | PASS；CameraInfo 两个流均成功，Control GET 8/8 均返回状态码 0。见 `acceptance/read_only_rpc.log`、`acceptance/result.json` |
| 25 秒限时运行与清理 | PASS；驱动退出码 0，所有子进程已回收且驱动和 Composer 进程均已退出。驱动报告的 loan/send failures 为 0；SDK 日志在启动阶段记录了队列满、停止期间记录了帧数据长度异常丢弃，因此不据此声称全程采集无丢帧。见 `extended/camera.log`、`extended/result.json`、`extended/Log/OrbbecSDK.log.txt` |

尚未运行：长时间 soak、USB 拔插与重连、Control SET、标定和外参的物理精度验证。本节保留修复前的 IMU 样本间隙，修复后结果见下一节。原权限诊断和 SDK `Access denied` 记录保留于 `target/orbbec-hardware-20261020/permission-diagnosis.json`；软件测试夹具结果仍与以上真实硬件检查分开记录。

## IMU 独立采集与按序接收修复

IMU 已移出视频 Pipeline，改由两路独立 SDK Sensor 回调采集，使用 200 Hz 配置；独立 EffectScope 任务负责发布。回调 FIFO 固定上限为合计 256 条、单批最多 64 条。IMU 端点改用现有 `Control` profile（每个接收者 4 条、非覆盖），视频/IR/点云继续使用 `Sensor`。数据格式、采集时钟来源、米制单位和旧消息布局保持原样。

采集 FIFO 溢出、SDK 帧号不连续、IMU 发布 loan 失败、队列满持续超过 100 毫秒、断流或时间回退都会明确失败。C++ 和 Python 消费者同时检查发布序号、SDK 帧号和采集时钟；不会把缺失样本重新编号为连续样本。队列短暂满时重发相同序号及完全相同的 payload；消费者逐字节核对并去重，以处理多接收者部分发送成功的情形。发送压力中的 `send_failures` 包括后来恢复的 `QueueFull` 尝试，不能直接当成最终漏样数。Python `--summary` 保持常量内存，只在结束时输出汇总。

初次软件回归记录：`target/orbbec-imu-software-20261020/result.json`；最终重发/去重改动后另行执行的 C++ 3/3 和 Python 15/15 均通过，汇总于 `target/orbbec-imu-hardware-20261020-final/acceptance.json`。检查包括 FIFO 顺序/容量/错误/关闭唤醒、视频处理延迟期间 IMU 独立推进、源帧号断档、时间回退、单路断流、采集队列错误传播和慢接收者导致 `QueueFull` 的显式失败。另行通过了 45 毫秒慢接收者恢复及多接收者相同副本去重检查。已有 Stream、CameraInfo/Control、TF、重激活时钟与退出测试继续通过。SDK 部分启动失败的回滚已实现，但未注入真实 SDK 启动失败进行硬件测试。

修复后的真机记录：`target/orbbec-imu-hardware-20261020-final/result.json` 和 `acceptance.json`，结果 **PASS**。仍使用设备 `CP15641000AT`，启用彩色、深度、左右红外、点云和 IMU。

| 检查 | 最终实测 |
| --- | --- |
| Python 加速度计 | 2000 个唯一样本，RBT 序号与 SDK 帧号均为 25–2024，均无跳号；原样重发副本 0 |
| Python 陀螺仪 | 2000 个唯一样本，RBT 序号与 SDK 帧号均为 29–2028，均无跳号；原样重发副本 0 |
| C++ Composer 托管 IMU 消费者 | 同时接收加速度计 2000 个唯一样本，序号与 SDK 帧号均为 1–2000，无跳号 |
| 时间来源 | 两路采集时钟均匹配此次 activation，generation 为 0，采集时间不回退；未知精度仍保留为未知 |
| 视频与 TF | 同时接收 20 个 640×480 深度帧并保存第一帧；晚加入 C++ 消费者取得 30 个彩色帧并按实际采集时间查询工厂静态 TF |
| 退出 | 驱动正常返回 0，所有自有子进程回收，无 SIGKILL；Composer 由测试脚本主动 SIGTERM 结束 |

两路 IMU 的 2000 个样本各覆盖约 9.9 秒设备时间，配置为 200 Hz；这证明本次接收窗口内连续，不代表任意负载或长时间运行均无漏样。SDK 回调队列溢出、持续超过 100 毫秒的接收端拥塞、源帧号断档仍会明确报错。真实 SDK 部分启动失败、拔插恢复、长稳和物理标定精度未在本轮验收。

## 对现有 RBT 的影响

驱动独立位于 `drivers/orbbec`，使用已安装的 RBT C++ SDK。未修改 Rust/C++/Python 运行时、现有消息布局、IDL 生成器、原生中间件补丁、版本轴或生产 unsafe 边界。共享修改仅为文档入口，以及让后续源码分发包含 `drivers` 的文件白名单；已生成的 0.8.0 分发包保持原样。

## 0.2.0 深度处理与重连

2026-10-21，使用同一台 Gemini 335（`CP15641000AT`）。仅执行驱动范围的检查，未重跑 RBT 全项目验收。

| 检查 | 状态与证据 |
| --- | --- |
| C++ 参数、编解码、IMU 队列、处理参数 schema、处理后几何信息和 CameraInfo RPC | PASS，4/4；`target/orbbec-software-20261021/cpp_core_and_component.log` |
| Python 编解码及诊断、C++ 跨进程消费和 TF、NumPy 像素 | PASS，Python 15/15；`target/orbbec-software-20261021/result.json` |
| 重连软件夹具：两次断开恢复、快速拔插事件、忽略其他设备事件、带 TF 的消费者重新绑定、等待期间 SIGTERM | PASS，SOFTWARE_FIXTURE；`target/orbbec-reconnect-tf-20261021/result.json`；4 次 activation 的时钟和服务身份均不同 |
| 重连总时限及配置错误 | PASS，6 秒总时限实际 6.161 秒，未在重连时重新计时；设备存在时配置错误退出 1、不重试 |
| SDK 推荐滤波器与完整参数 schema | PASS，真机返回 10 种滤波器、18 个参数；`target/orbbec-processing-20261021/filters.json` |
| 软件 D2C、降采样、降采样后 D2C | PASS，REAL_HARDWARE；3 种组合各检查 30 帧深度和点云；`target/orbbec-processing-20261021/result.json` |
| 处理后的 RBT Stream、Python 数组、C++ 晚加入及 TF | PASS，彩色/深度/点云各 30 帧；D2C 深度及点云使用 640×480 彩色网格和 color_optical，降采样单独启用时为 320×240 |
| D2C 与滤波同时启用时的 IMU | PASS，accel/gyro 各 1000 条，无 RBT 序号或 SDK 帧号跳号，采集时钟与 activation 一致 |
| 物理 USB 拔插后的自动恢复 | PASS，REAL_HARDWARE，一次实际拔下并重新插回；`target/orbbec-hotplug-20261021-user-retry/result.json` |

D2C 显式启用 SDK `TargetDistortion=1`，投影到实际发布的原始 RGB 像素网格；真机检查严格比较处理后深度与彩色的分辨率、内参、畸变模型和系数。滤波后的 SDK 时间戳、帧号和数值比例均保留。点云每个非零点的米制 Z 与对应处理后深度像素一致。阈值在 D2C 前应用，变换后的彩色相机 Z 不再受原始深度相机 Z 区间直接约束。

当前设备推荐列表为 DecimationFilter、ThresholdFilter、HDRMerge、SequenceIdFilter、SpatialFastFilter、SpatialModerateFilter、SpatialAdvancedFilter、TemporalFilter、HoleFillingFilter、DisparityTransform。全部参数可在启动时显式配置；未指定的参数沿用 SDK 默认值。HDR/序列专用滤波器的实际成像效果未验证，驱动不会自动配置设备 HDR 模式；没有在线滤波器配置 RPC。

真机处理检查的相机退出 0、各消费者退出 0，Composer 以 SIGTERM 退出，所有进程已回收且没有 SIGKILL。长期 TF 消费每 2 秒重新获取静态快照，保持 RBT 既有的 5 秒新鲜度契约；activation 关闭时的取消不再被误报为消费失败。

重连软件夹具命令（`--build-dir` 中须已有夹具和 C++ 消费者；输出目录须不存在）：

```bash
python3 drivers/orbbec/scripts/verify_reconnect.py \
  --build-dir target/orbbec-build \
  --composer "$RBT_PREFIX/bin/composer" \
  --output-dir target/orbbec-reconnect-check
```

深度处理真机命令（指定设备完整序列号；`--python` 须为包含 RBT 绑定和 NumPy 的绝对可执行路径；输出目录须不存在）：

```bash
python3 drivers/orbbec/scripts/verify_processing.py \
  --serial '<完整序列号>' \
  --probe target/orbbec-build/rbt_orbbec_hardware_processing_probe \
  --driver target/orbbec-install/bin/rbt-orbbec-camera \
  --consumer target/orbbec-install/bin/rbt-orbbec-consumer \
  --composer "$RBT_PREFIX/bin/composer" \
  --python '/absolute/path/to/python-with-rbt-and-numpy' \
  --output-dir target/orbbec-processing-check
```

物理拔插检查可运行 `scripts/verify_hotplug.py`，显式指定 `--serial`、`--driver`、`--consumer`、`--composer`、`--output-dir`，默认操作窗口为 180 秒。出现 `hotplug_ready` 后人工拔下 USB，等待 2 秒再插回。该入口同时启用 D2C、空间/时间/空洞滤波、点云和 IMU，并检查原 C++ 消费者恢复及新的 runtime、时钟与 capability；没有观察到拔插时记录 NOT_RUN。

持续组合检查：空间高级滤波、时间滤波、空洞填充及 D2C，同时启用点云和 IMU；同一个 C++ 消费者收到 5219 帧对齐深度并查询工厂 TF，覆盖约 177.0 秒采集时间，没有 SnapshotStale。此项只证明该窗口内运行正常，不是长期 soak 或物理拔插验证。相机和消费者均退出 0，所有进程回收且没有 SIGKILL。汇总为 `target/orbbec-features-acceptance-20261021.json`。

物理拔插补充验收：用户实际拔下并插回同一台 335 后，记录到 disconnected → waiting → reconnecting → 新 activation。原 C++ 消费者未重启，自动重新绑定并恢复至少 5 帧 640×480 对齐深度及到物理深度光学帧的 TF 查询。新的 RBT runtime 身份、设备时钟和 Color/Depth/Points/Accel/Gyro 服务身份均与拔插前不同。D2C、空间/时间/空洞滤波、点云和 IMU 全程配置启用；本次恢复后的逐帧消费检查针对 Depth 与 TF，不另外声称逐路重新验证彩色、点云和 IMU 的接收连续性。相机与消费者退出 0，全部测试进程已回收且无 SIGKILL。早先未观察到拔插的窗口记录仍保留为 NOT_RUN，此次结果是独立的真实硬件 PASS。

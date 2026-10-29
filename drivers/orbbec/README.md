# RBT 原生 Orbbec Gemini 335 驱动

驱动版本 `0.2.0`，使用 RBT SDK `0.8.0` 和 Orbbec SDK `2.10.6`。

此驱动面向 Linux x86_64 和 ARM64，可连接一台由序列号明确指定的 Gemini 335。默认请求彩色图和深度图的 640×480、30 fps 配置；实际配置须由设备支持。左/右红外图、深度派生点云和 IMU 数据可选；更改视频模式前请先查询设备支持的配置。

```bash
rbt-orbbec-camera list
rbt-orbbec-camera profiles --serial '<设备序列号>'
rbt-orbbec-camera run --serial '<设备序列号>' [--left-ir --right-ir --points --imu]
```

`run` 必须指定完整序列号。`--component-id` 设置 Composer 组件名，`--frame-prefix` 设置 TF 帧名前缀。驱动提供相机标定 RPC 和 8 个固定的曝光、增益、白平衡控制；这些控制不是 RBT 参数。相机信息和控制响应共用状态码：`0` 成功、`1` 不支持、`2` 参数无效、`3` SDK 错误。

## 深度处理与设备重连

深度到彩色的 SDK 软件对齐默认关闭。启用 `--align-depth-to-color` 后，驱动按命令行顺序执行所有深度滤波器，再执行 D2C，并使用彩色畸变模型投影到原始 RGB 像素网格。启用对齐时，Depth 和 Points 使用 `color_optical` 帧；CameraInfo 返回处理后 SDK 提供的内参与分辨率。出厂深度到彩色的物理外参保持原样。未启用对齐时，深度滤波器可改变 Depth 和 Points 的分辨率及内参；D2C 会在滤波后将结果映射回彩色图网格尺寸。阈值滤波约束的是 D2C 之前的深度相机 Z；变换后彩色相机 Z 的范围可能略有变化。

用 `filters` 查询当前设备上 SDK 推荐的滤波器名称和完整参数 schema。每个 `--depth-filter` 指定一个滤波器，重复该选项可按给定顺序串联；显式给出的参数必须通过 SDK schema 的类型、范围和步长校验。未知或不支持的名称及无效参数会被拒绝；省略的参数保留 SDK 默认值。未指定滤波器时不应用滤波器。示例名称及参数如下，实际名称和 schema 以所连接设备运行时返回的推荐列表为准：

```bash
rbt-orbbec-camera filters --serial '<设备序列号>'
rbt-orbbec-camera run --serial '<设备序列号>' \
  --depth-filter 'ThresholdFilter:min=200,max=4000' \
  --depth-filter 'DecimationFilter:decimate=2' \
  --align-depth-to-color
```

滤波器配置仅在启动时读取；没有在线修改滤波器的 RPC。驱动不依赖 ROS、不使用全局 ParameterServer，也不修改共享 schema 或 RBT 核心。

同一滤波器在链中只能出现一次；布尔参数使用 `0` 或 `1`。最多配置 16 个滤波器、合计 64 个参数。

这台 335 的 SDK 实际返回 10 种滤波器：Decimation、Threshold、HDRMerge、SequenceId、SpatialFast、SpatialModerate、SpatialAdvanced、Temporal、HoleFilling 和 DisparityTransform；CLI 使用查询结果中的完整名称。HDR 合并与序列选择需要设备提供相应的 HDR/序列数据，启用滤波器不会自动设置设备 HDR 模式。

设备重连默认启用，基于 SDK DeviceMonitor 事件。`--reconnect-delay-ms` 设置重试等待时间，默认为 `1000`，有效范围为 `100–30000` 毫秒；`--no-reconnect` 恢复设备缺失或断开时快速失败的行为。设备初始不存在时，驱动等待指定序列号出现。断开后，驱动先停止旧 activation 并丢弃其回调，再为完全相同的序列号创建新的 ComponentRuntime；组件 ID 保持不变，OS 进程继续运行，而新的 ComponentRuntime 会获得新的 RBT runtime process identity UUID、capability 和设备时钟 UUID。设备仍在但出现无法恢复的配置错误、IMU 连续性错误或时间故障时，驱动会失败退出。

`--duration-sec` 是所有启动及重连尝试共用的总时限；外部 `SIGINT` 或 `SIGTERM` 在等待设备期间也可取消运行。Composer 托管的 C++ 消费组件会重新绑定新 activation。Python 直接端点诊断工具不自动重绑定，activation 被替换后须重新启动诊断工具。

如果 `list` 显示设备但 `serial` 为空，表示 SDK 此次没有提供可读序列号；这也可能是 SDK 读取错误。请检查 Orbbec SDK 日志，并核对该设备对应的 `/dev/bus/usb/<BUS>/<DEVICE>` 节点对当前用户是否有读写权限。需要持久配置时，可安装 SDK 前缀随附的厂商规则（下面使用默认前缀，在仓库根目录执行），然后重新插拔相机：

```bash
sudo install -m 0644 target/orbbec-sdk-2.10.6/share/orbbec/udev/99-obsensor-libusb.rules /etc/udev/rules.d/99-obsensor-libusb.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

若需临时验证权限，可只对匹配节点运行 `sudo setfacl -m "u:$USER:rw" '/dev/bus/usb/<BUS>/<DEVICE>'`。之后重新运行 `rbt-orbbec-camera list`。

相机通过 RBT `StampedBytes` 发布驱动自有的 `RBTORB01` 应用格式：128 字节小端序头部，后接帧数据。格式和解码约定见 [`driver.hpp`](include/rbt_orbbec/driver.hpp)。未对齐时深度帧 ID 为 `<前缀>/depth_optical`；启用 D2C 后，Depth 和 Points 改用 `<前缀>/color_optical`。Color 帧 ID 为 `<前缀>/color_optical`，左右红外帧 ID 为 `<前缀>/left_ir_optical` 和 `<前缀>/right_ir_optical`，IMU 帧为 `<前缀>/accel` 和 `<前缀>/gyro`。静态变换仍使用相机出厂外参，连接深度光学帧与其他启用的传感器帧，包括原有的物理深度到彩色变换；SDK 未提供的外参保持缺失，不推造 `base_link` 或安装位姿。

深度图中的原始值 `0` 表示无效深度。点云中的零行无效；使用点云时应只保留坐标均为有限值且 `z > 0` 的点。

设备提供的采集时钟以微秒转换为纳秒，时钟不确定度标记为未知；接收时刻使用单调时钟。若采集时间回退，组件会关闭；每次重新激活会创建新的设备时钟标识。

`received` 是驱动处理帧时读取的主机单调时刻，不是经过标定的 USB 到达时刻。驱动不把 SDK 的 realtime/global 时间戳当成主机单调时钟，也不自动构造设备到主机的时钟映射。

IMU 使用独立 SDK 回调与独立生命周期任务，选择 200 Hz 采样配置，不经过视频/点云处理循环。回调只把数值、SDK 帧号和时间戳写入最多 256 条的共享 FIFO，每次最多取 64 条。采集队列溢出、SDK 帧号断档或时间回退会使组件明确失败，不补造样本。

IMU 发布采用现有 `Control` profile：每个接收者有 4 条按序、非覆盖的队列。队列满时，对同一个样本最多重试 100 毫秒，保留完全相同的序号、时间戳和 payload；超时或 loan 失败会报错并停止组件。多接收者的发送可能部分成功，因此重试可能产生重复样本。随附 C++/Python 消费者只去除与上一条序号及字节完全相同的副本，冲突副本或真正的断档仍会报错；自定义消费者也应按此规则去重。该策略没有修改 RBT 的既有 profile 契约，也不等同于 ROS 2 的可靠传输。图像、红外和点云仍采用 `Sensor` 最新帧策略。消费端应及时读取；跨进程 C++ 消费者会检查 IMU 的发布序号、SDK 帧号和时钟连续性。

| 流 | 数据与单位 |
| --- | --- |
| Color | RGB8；深度到彩色对齐默认关闭 |
| Depth | 小端 u16；乘以每帧 `unit_scale` 得到米。启用 D2C 时位于彩色网格并使用彩色光学帧 |
| LeftIr / RightIr | Mono8 或小端 Mono16，保留原始强度 |
| Points | 小端 float32 XYZ，单位米；默认位于深度光学坐标系，启用 D2C 时位于彩色光学坐标系；不含 RGB |
| Accel / Gyro | 小端 float32 XYZ，分别为 m/s²、rad/s；使用 SDK 200 Hz 配置，不额外估计偏置 |

光学坐标采用 x 向右、y 向下、z 向前的右手系。内参、畸变模型和系数通过 `CameraInfo` RPC 按流查询，IDL 位于 [`types/orbbec.rbt`](types/orbbec.rbt)。控制属性 ID `1–8` 依次为彩色自动曝光、彩色曝光、彩色增益、深度自动曝光、深度曝光、深度增益、自动白平衡、白平衡；曝光等数值保留 SDK 属性单位并校验设备返回的范围。

## 在 Composer 中消费

`rbt-orbbec-consumer` 是供应用集成使用的 Composer 托管 C++ 消费组件。需要先启动 Composer，再分别启动相机和消费者；流 requirement 由 Composer 自动解析。消费者通过 `--provider` 校验 Composer 选中的组件名，不能用它改选 provider。单相机流程如下（`composer` 来自已安装的 RBT SDK）：

```bash
# 终端一：启动 Composer
composer

# 终端二：启动相机组件
rbt-orbbec-camera run --serial '<设备序列号>'

# 终端三：启动 Composer 托管消费者，读取 30 个深度帧
rbt-orbbec-consumer --provider orbbec335 --stream depth --count 30 --timeout-sec 15
```

需要查询 TF 时，可用以下示例读取彩色帧并查询到深度光学帧的变换：

```bash
rbt-orbbec-consumer --provider orbbec335 --stream color --tf-target orbbec335/depth_optical \
  --allow-unknown-uncertainty --count 30 --timeout-sec 15
```

该选项允许时间不确定度未知；它不表示已经验证了具体设备上的时钟映射。多相机选择尚未验证。

Python 编解码器位于 `drivers/orbbec/python/`。`consume.py` 是直接打开原生端点的诊断工具；从激活 JSON 或 `rbt graph` 描述符复制实际服务名，再选择流：

```bash
python3 drivers/orbbec/python/consume.py \
  --service '<实际流服务名>' --stream depth --count 30
```

IMU 的 Python 诊断工具会按 `--stream accel/gyro` 自动选择 `Control` profile，对重发副本去重后检查连续性，发现序号断档即返回非零状态。加 `--summary` 可只输出常量内存的汇总，避免逐样本打印：

```bash
python3 drivers/orbbec/python/consume.py \
  --service '<实际 accel 服务名>' --stream accel --count 2000 --summary
```

请使用已安装 RBT Python 绑定和 NumPy 的 Python 环境。该工具不解析 Composer requirement；Python 组件绑定也不解码此 `StampedBytes` 应用格式，因此不宣称支持 Composer 托管的 Python 组件，也不会在激活替换后自动重连。需要应用集成时请使用上面的 C++ 消费组件。

此适配器不宣称与 ROS 功能等同。当前仍未移植网络或 GMSL 相机、录制与回放、固件更新及完整 ROS 参数接口。深度处理的真机结果、重连软件检查及物理拔插项的状态见 [验证记录](VERIFICATION.md)。

## 构建

可选的 Orbbec SDK 二进制不属于 RBT 核心发行包。使用脚本将固定版本的 2.10.6 Linux x86_64 SDK 安装到一个新的本地目录：

```bash
./drivers/orbbec/scripts/bootstrap_sdk.sh
```

默认 SDK 前缀为 `target/orbbec-sdk-2.10.6`；可用 `--prefix PATH` 指定其他新目录。脚本校验上游固定提交和逐文件清单，并保留 SDK、第三方组件及 wrapper 的许可证和声明。udev 规则只放入前缀供人工检查和安装；脚本不会修改系统 udev 规则，也不会刷写固件。来源和许可见 [`UPSTREAM.toml`](UPSTREAM.toml) 与 [`NOTICE.md`](NOTICE.md)。

### Linux ARM64

在 ARM64 Linux 上脚本会自动选择对应的 SDK 2.10.6 清单，并安装到 `target/orbbec-sdk-2.10.6-arm64`。在 x86_64 打包机上也可显式运行 `--arch arm64` 来暂存 ARM64 SDK；两种 ARM64 名称 `aarch64` 和 `arm64` 都受支持。

使用 ARM64 版 RBT 0.8 SDK 和本机 ARM64 C++ 工具链配置构建：

```bash
./drivers/orbbec/scripts/bootstrap_sdk.sh --arch arm64
export RBT_PREFIX="/path/to/arm64/rbt-sdk-0.8"
source "$RBT_PREFIX/env.sh"
export ORBBEC_SDK_ROOT="$PWD/target/orbbec-sdk-2.10.6-arm64"
cmake -S drivers/orbbec -B target/orbbec-build-arm64 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$RBT_PREFIX" \
  -DORBBEC_SDK_ROOT="$ORBBEC_SDK_ROOT"
cmake --build target/orbbec-build-arm64 --parallel
```

`--arch arm64` 在 x86_64 上用于准备 SDK 文件；要编译 ARM64 驱动，须使用与 ARM64 RBT SDK 匹配的 ARM64 工具链，或配置相应的交叉编译工具链。

以下示例针对已安装的 RBT SDK 和外置 Orbbec SDK：

```bash
export RBT_PREFIX="$PWD/target/release-build-distribution-20261020/sdk-root" # 或你的 RBT SDK 解压目录
source "$RBT_PREFIX/env.sh"
export ORBBEC_SDK_ROOT="$PWD/target/orbbec-sdk-2.10.6"
export RBT_PHASE4_TOOLCHAIN="${RBT_PHASE4_TOOLCHAIN:-$PWD/target/phase4-toolchain}"
export PATH="$RBT_PHASE4_TOOLCHAIN/bin:$PATH"
export CC="$RBT_PHASE4_TOOLCHAIN/bin/x86_64-conda-linux-gnu-gcc"
export CXX="$RBT_PHASE4_TOOLCHAIN/bin/x86_64-conda-linux-gnu-g++"
export AR="$RBT_PHASE4_TOOLCHAIN/bin/x86_64-conda-linux-gnu-ar"
export LD_LIBRARY_PATH="$RBT_PHASE4_TOOLCHAIN/lib:$RBT_PREFIX/lib:$ORBBEC_SDK_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LIBRARY_PATH="$RBT_PHASE4_TOOLCHAIN/lib:$RBT_PREFIX/lib:$ORBBEC_SDK_ROOT/lib${LIBRARY_PATH:+:$LIBRARY_PATH}"

cmake -S drivers/orbbec -B target/orbbec-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_COMPILER="$CXX" \
  -DCMAKE_PREFIX_PATH="$RBT_PREFIX" \
  -DORBBEC_SDK_ROOT="$ORBBEC_SDK_ROOT"
cmake --build target/orbbec-build --parallel
ctest --test-dir target/orbbec-build --output-on-failure
cmake --install target/orbbec-build --prefix "$PWD/target/orbbec-install"
export PATH="$PWD/target/orbbec-install/bin:$PATH"
```

相机程序将 Orbbec SDK 作为外部运行时依赖；安装 RPATH 会包含配置的 SDK 库目录。设置 `-DRBT_ORBBEC_BUILD_DEVICE=OFF` 可不查找 SDK，构建核心测试和 Composer 消费者；两种配置都需要已安装的 RBT CMake 包。

本地 Python 测试需使用包含 RBT Python 绑定与 NumPy 的活动环境：

```bash
PYTHONPATH="$PWD/drivers/orbbec/python${PYTHONPATH:+:$PYTHONPATH}" \
  python3 -m unittest discover -s drivers/orbbec/tests -p 'test_*.py' -v
```

以上命令为软件检查。已完成的 Gemini 335 真机结果和未执行项见 [验证记录](VERIFICATION.md)。

带跨进程 Stream、RPC、静态 TF 和 Python NumPy 检查的软件入口（不打开相机）：

```bash
python3 drivers/orbbec/scripts/verify_software.py \
  --build-dir target/orbbec-build \
  --composer "$RBT_PREFIX/bin/composer" \
  --python /path/to/python-with-rbt-and-numpy \
  --output-dir target/orbbec-software-check
```

输出目录必须是新目录。测试后端仅链接到测试程序，不链接到相机驱动。
`run` 发生组件错误时以非零状态退出；`--duration-sec` 限制为 0–86400 秒（0 或省略表示持续运行），并覆盖初次等待和后续重连时间。总时限内未激活也会返回非零状态。

## IMU 真机检查

以下入口只打开显式指定的设备，启动独立 Composer，并在视频、左右红外和点云运行时同时检查 Python 两路 IMU、C++ IMU 和视频/TF。`--allow-unknown-uncertainty` 仅允许工厂静态 TF 的未知时间精度，不构建设备与主机之间的映射。

```bash
python3 drivers/orbbec/scripts/verify_imu.py \
  --serial '<完整序列号>' \
  --driver target/orbbec-install/bin/rbt-orbbec-camera \
  --consumer target/orbbec-install/bin/rbt-orbbec-consumer \
  --composer "$RBT_PREFIX/bin/composer" \
  --python '<已安装 RBT 和 NumPy 的 Python 路径>' \
  --output-dir target/orbbec-imu-check-new \
  --count 2000 --allow-unknown-uncertainty
```

目录必须不存在。脚本保留每个进程日志和 `result.json`，并回收它启动的进程。默认相机运行上限为 45 秒；样本检查完成后提前停止。实际验收范围见 [验证记录](VERIFICATION.md)。

## 与 ROS 2 的性能对比

Gemini 335 同机彩色/深度双流的实测数据见 [性能报告](benchmarks/RESULTS.md)，包含 RBT、ROS 2 本机基础配置和 Fast DDS 大帧调优三组，各三轮、每轮 20 秒。指标定义、计时边界、配置差异及可重复命令见 [基准说明](benchmarks/README.md)。报告同时保留一次 RBT 彩色启动超时和早期 ROS 2 丢帧试跑记录。

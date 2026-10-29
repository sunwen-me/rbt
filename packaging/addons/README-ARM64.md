# RBT 相机与 M3T 源码包（Linux ARM64）

本包适用于已有 **ARM64 RBT 0.8.0 C++ SDK** 的电脑。包含 Orbbec 驱动 0.2.0、M3T 适配模块 0.3.0，以及固定提交的 M3T 核心源码。无须安装 ROS 2。

`vendor/orbbec-sdk-2.10.6-arm64/` 是 Orbbec 官方 ARM64 SDK 的头文件和二进制依赖；其中扩展库不是源码，许可原文随包保留。RBT 两个模块和 M3T 会在目标机上编译。本包没有附带 x86_64 程序，也不包含 RBT 核心安装包。

## 1. 准备依赖

需要 Linux ARM64/aarch64、CMake 3.22+、支持 C++20 的 GCC 11+ 或相应 Clang，以及 OpenCV 4.3+、Eigen、GLFW、GLEW、OpenGL 和 OpenMP。Ubuntu/Debian 上可安装发行版提供的开发包：

```bash
sudo apt update
sudo apt install build-essential cmake python3 \
  libeigen3-dev libopencv-dev libglfw3-dev libglew-dev \
  libgl1-mesa-dev libglu1-mesa-dev libegl1
```

若 RBT 安装时仅安装了 CLI/Python，需要先补齐同架构的 RBT **C++ SDK** 和其维护版 iceoryx2 原生前缀。`--rbt-prefix` 目录应包含 `lib/cmake/rbt/rbtConfig.cmake`（或发行版对应的 lib64/lib/<架构> 路径）；不要传入 x86_64 SDK。

Texture 的 CPU ORB、BRISK、SIFT 不需要 CUDA。DAISY/FREAK 只有在 OpenCV 带 `xfeatures2d` 时可用。CUDA ORB 默认关闭；只有 NVIDIA CUDA 环境、OpenCV `cudafeatures2d` 可用且显式设置 `RBT_M3T_ENABLE_CUDA=ON` 才能启用。AidLux 的 Adreno 740 可通过 Zink 使用 OpenGL，但它不是 NVIDIA CUDA 设备。

厂商 SDK 内的可选 `enhanced_depth_filter/libengine_infer_encry.so` 另依赖 CUDA 12 与 TensorRT 10；普通 ARM64 主机不应启用这一增强滤波器。它们不是编译这两个 RBT 模块的基础依赖。包保留厂商完整目录和许可，没有删改扩展二进制。

## 2. 编译与安装

解压后在包的根目录执行。将 RBT 路径换成另一台电脑上的实际目录：

```bash
python3 verify_source.py
./build.sh --rbt-prefix "$HOME/opt/rbt-sdk" --jobs 2
source "$HOME/opt/rbt-sdk/env.sh"
source ./install-arm64/env.sh
rbt-orbbec-camera --version
rbt-m3t --version
```

默认安装到当前源码目录的 `install-arm64/`，编译目录为 `build-arm64/`；均无须 sudo。`--prefix DIR` 可改安装目录，`--tests` 会额外构建并运行两个模块的 CTest 检查。低内存设备可用 `--jobs 1`。只编译一个模块可加 `--only orbbec` 或 `--only m3t`。

Hexagon ORB 是可选功能。启用时必须提供已有且可读的两个常规文件：Hexagon DSP 上运行的 skeleton，以及运行在 ARM64 应用处理器（AP）上的 FastRPC stub。例如：

```bash
./build.sh --rbt-prefix "$HOME/opt/rbt-sdk" --only m3t \
  --hexagon-orb \
  --dsp-skel "$PWD/runtime/dsp/hexagon-v68/librbt_m3t_orb_skel.so" \
  --dsp-stub "$PWD/runtime/dsp/linux-aarch64/librbt_m3t_orb_stub.so"
```

本源码包附带可选的预编译 DSP runtime：V68 skeleton 运行于 Hexagon DSP，ARM64 stub 运行于 Linux 应用处理器。这里的 `--dsp-skel` 和 `--dsp-stub` 是 `./build.sh` 的构建参数，不是 `rbt-m3t` 运行命令参数。不能将这些 DSP 选项与 `--only orbbec` 一起使用；只给其中一个文件或未指定 `--hexagon-orb` 也会报错。构建后，M3T Texture modality 可按需将配置中的 `descriptor_type` 设为 `6` 以选择 Hexagon ORB。运行时不会回退到 CPU ORB；安装后库位于 `lib/rbt-m3t/dsp/`。如果运行时库部署在其他位置，设置 `RBT_M3T_DSP_MODULE` 和 `RBT_M3T_DSP_STUB` 为各自的绝对路径。V68 skeleton 已在 AidLux V73 CDSP 上验证；此结果不代表所有 Hexagon 固件版本均兼容。跨编译、链接和设备验证步骤见源码中的 `perception/m3t/dsp/README.md`。

也可直接使用 CMake，例如：

```bash
cmake -S . -B build-arm64 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$HOME/opt/rbt-sdk" \
  -DCMAKE_INSTALL_PREFIX="$PWD/install-arm64" -DBUILD_TESTING=OFF
cmake --build build-arm64 --parallel 2
cmake --install build-arm64
```

CMake 默认使用包内的 ARM64 Orbbec SDK 和 M3T 源码，无须在目标机下载它们。编译器、系统开发库及既有 RBT SDK 仍须事先安装。两个原模块的 README 保留了开发机历史路径示例；迁移编译请以本文件为准。

## 3. 相机 USB 权限

规则文件在 `vendor/orbbec-sdk-2.10.6-arm64/share/orbbec/udev/99-obsensor-libusb.rules`，安装后也会复制到本地前缀的 `share/orbbec/udev/`。可审阅后手动安装：

```bash
sudo install -m 0644 vendor/orbbec-sdk-2.10.6-arm64/share/orbbec/udev/99-obsensor-libusb.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=usb
```

然后重新插入相机。编译脚本不会修改系统规则、刷固件或驱动电机。

## 4. 运行

每个终端先加载现有 RBT 环境，再加载本包安装环境。相机序列号必须从 `list` 的实际输出中选择：

```bash
rbt-orbbec-camera list
```

分别在三个终端启动：

```bash
# 终端一
composer

# 终端二：替换为实际序列号
rbt-orbbec-camera run --serial '<相机完整序列号>' --align-depth-to-color

# 终端三：验证 M3T 的 RGB-D 输入
rbt-m3t probe --count 30 --duration-sec 20
```

跟踪和初始化会使用 OpenGL 3.3+，即使不开 `--preview` 也需要有效显示环境。Linux 上使用 X11/XWayland；SSH 纯终端不会自动提供可用的 OpenGL context。AidLux GPU 的已知测试方法见下文；其他 ARM 设备仍需在目标机验证。

### AidLux GPU（Adreno 740）

AidLux GPU 环境可通过 Mesa Zink 创建 OpenGL context。2026-10-23 的 M3T 0.2.0 ARM64 验证中，tracker 和 fixture 实际使用 `zink Vulkan 1.4 (Turnip Adreno 740)`，OpenGL 4.6；测试配方如下：

若系统没有 `glxinfo` 或 `xvfb-run`，安装 `mesa-utils` 和 `xvfb`。

```bash
MESA_LOADER_DRIVER_OVERRIDE=zink LIBGL_KOPPER_DRI2=true \
  xvfb-run -a glxinfo -B
```

运行 M3T 或渲染验证时，将同一环境变量传给进程：

```bash
MESA_LOADER_DRIVER_OVERRIDE=zink LIBGL_KOPPER_DRI2=true \
  xvfb-run -a rbt-m3t run --config /path/to/tracking.json
```

Zink 的 Adreno OpenGL 路径只用于渲染，不会启用 CUDA ORB，也不代表 M3T optimizer 在 GPU 上运行。

实际追踪需把自己的 SO101 URDF/网格复制到目标机，再用 `perception/m3t/scripts/prepare_robot.py` 转换，并填写当前关节角、运行 `rbt-m3t-initialize`。把模型、快照、初始化 JSON 和本机配置放在 `target/m3t-so101-local/` 等工作目录；不要写入 `perception/m3t/` 源码树，以免私有模型或校准 JSON 被源码打包带走。模型、快照和机器外参不随本源码包传输；不会用示例数值冒充实测位姿。完整流程见 `perception/m3t/README.md`。

## 验证范围

包内源文件和 ARM64 厂商 SDK 有 SHA-256 清单；`python3 verify_source.py` 会检查内容、符号链接及厂商 ELF 架构。下列 M3T 0.2.0 结果是历史记录，当前 DSP 验证范围见 0.3.0 小节。

### M3T 0.2.0 历史验证

三个 profile 的整次运行分别为 7.153、6.833、6.765 秒；这些是端到端耗时，不是 FPS。baseline 关节 RMSE 为 0.00926 / 0.05088 rad，extended 为 0.01088 / 0.01428 rad。hard-constraint 用例中输入关节移动 0.2112 rad，受硬约束的输出仍约为零，RMSE 与零角基线相同；它仅验证约束生效，不表示跟踪精度改善。验证使用合成 fixture，`hardware` 输入仍是 NOT RUN：Gemini 335 实机采集及真实 SO101 初始化、精度和性能尚未测试。

0.2.0 验证记录位于 `target/aidlux-m3t-0.2.0-validation/`。该轮 CPU ORB 已运行验证；BRISK/SIFT 编译通过但未分别运行。CUDA 和 OpenCV contrib 不可用，CUDA ORB 与 DAISY/FREAK 未验证。约束仅支持单个 arm 内的 link pair，不能跨独立 optimizer；上游数据集 benchmark 示例未移植，也不宣称完整自动检测器或全 GPU optimizer。目标机可用 verifier 复测 extended profile；输出目录应为新目录：

```bash
MESA_LOADER_DRIVER_OVERRIDE=zink LIBGL_KOPPER_DRI2=true \
  xvfb-run -a python3 perception/m3t/scripts/verify_software.py \
    --build-dir build-arm64 --composer "$HOME/opt/rbt-sdk/bin/composer" \
    --output-dir target/m3t-verify-extended \
    --rendering hardware --profile extended --exercise-reinitialization
```

保存 CMake 和运行日志以便设备环境排查。上游数据集 benchmark 示例未移植；static detector 从实测 seed 启动，不是完整自动识别器。

### M3T 0.3.0 Hexagon DSP ORB

ARM64 AidLux final build 通过 7/7 CTest、安装检查和 DSP runtime/license 文件检查。独立 V73 CDSP ORB oracle 通过七个提取案例、四个匹配案例及 300×300 descriptor batch（返回 600 个邻居，每个非空操作一次 RPC）。ARM64 extended DSP software fixture 使用 Zink/Turnip Adreno 740 渲染，通过 20 批输出与 SIGHUP 重初始化检查；112 次提取 RPC 和 88 次匹配 RPC 均有审计记录，所有子进程均已回收且没有强制终止。该结果验证 DSP 后端可运行于软件 fixture，`hardware: NOT RUN`；真实 Gemini 相机采集和 SO-101 初始化、追踪、精度仍为 NOT_RUN。

最终 CPU extended profile 也通过 20 批检查，整次运行 6.716 秒；DSP+重初始化 profile 整次运行 13.855 秒。两次 profile 的重初始化工作不同，整次时间不是 FPS 或性能对比。独立 microbenchmark 显示当前 DSP 提取与匹配都慢于测得的 CPU 路径，不宣称加速。验证记录见 `perception/m3t/VERIFICATION.md`，设备文件证据位于源码仓库 `target/aidlux-m3t-0.3.0-dsp-validation/final/`；这些 target 文件不包含在源码包中。

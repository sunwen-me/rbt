# Hexagon DSP ORB

RBT M3T 0.3.0 提供可选的 Hexagon DSP ORB Texture 后端。它默认关闭，保留 CPU ORB `4` 和 CUDA ORB `5`；DSP 后端使用 overlay 编号 `ORB_DSP = 6`，配置见 [`../configs/texture_modality.dsp.example.yaml`](../configs/texture_modality.dsp.example.yaml)。RBT C++ SDK 仍为 0.8.0，上游 DLR-RM commit 未变。

## 状态

DSP-enabled ARM64 build、安装和设备功能检查为 **PASS**：AidLux V73 CDSP 上的独立 ORB 测试通过 7 个提取案例和 4 个批量匹配案例；300×300 descriptor pair 返回 600 个邻居，非空操作各使用一次 RPC。最终 M3T build 通过 7/7 CTest，extended 合成软件 profile 使用 DSP 完成 20 批输出检查和 SIGHUP 重初始化检查；审计记录 112 次提取 RPC、88 次匹配 RPC，所有子进程均已回收且没有强制终止。这里的 profile 输入是合成 fixture，不能代表真实相机或 SO-101 验收，二者仍为 **NOT_RUN**。

跨编译与链接步骤见 [`BUILD.md`](BUILD.md)，QAIC、FastRPC、compiler-rt 和 ELD 来源及许可证见 [`PROVENANCE.md`](PROVENANCE.md)。构建输出必须写到 DSP 源码目录之外。

## 启用与运行时库

配置 M3T 时，需要预先准备目标设备的 Hexagon skeleton 和 ARM FastRPC stub shared libraries，并显式打开后端：

```bash
cmake -S perception/m3t -B build/m3t-dsp -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DCMAKE_PREFIX_PATH="$RBT_PREFIX" \
  -DCMAKE_INSTALL_PREFIX="$PWD/target/m3t-dsp-install" \
  -DRBT_M3T_ENABLE_HEXAGON_ORB=ON \
  -DRBT_M3T_DSP_SKEL=/abs/path/librbt_m3t_orb_skel.so \
  -DRBT_M3T_DSP_STUB=/abs/path/librbt_m3t_orb_stub.so
cmake --build build/m3t-dsp --parallel 4
cmake --install build/m3t-dsp
```

这两个 CMake 路径必须指向已有文件。M3T 安装时将其复制到 `lib/rbt-m3t/dsp/`。运行时会在可执行文件旁或安装目录中查找库；也可以用绝对路径覆盖：

```bash
export RBT_M3T_DSP_MODULE=/abs/path/librbt_m3t_orb_skel.so
export RBT_M3T_DSP_STUB=/abs/path/librbt_m3t_orb_stub.so
```

设置 `RBT_M3T_DSP_AUDIT=1` 会输出 backend ready、提取和匹配审计事件。DSP 路径不会静默回退 CPU；加载、握手、提取或匹配失败会使本次纹理处理失败并向上报错。没有检测到特征的图像和空特征集是正常结果。

在 Texture modality YAML 中设置 `descriptor_type: 6`，并按支持范围设置 `orb_n_features`、`orb_scale_factor` 与 `orb_n_levels`。使用示例 [`../configs/texture_modality.dsp.example.yaml`](../configs/texture_modality.dsp.example.yaml)。

## 执行范围与限制

主机端先裁剪 Texture ROI、转灰度并生成关注区域图像；DSP 对该图像执行完整 ORB 金字塔、FAST-9 与 NMS、Harris 排序、方向计算、Gaussian 预处理和旋转 BRIEF 描述子。每个非空 keyframe descriptor pair 的 Hamming 2-NN 矩阵匹配通过一次 FastRPC 批量调用。主机继续处理 Lowe ratio 筛选、几何检查和 M3T optimizer；optimizer 仍在 CPU 上。

此实现采用定点/整数路径，数值细节与 OpenCV 不同，包括金字塔重采样和 quota、Harris/Gaussian rounding，以及 CORDIC 方向和 pattern 旋转。因此它不保证与 OpenCV ORB 检测器逐位一致。设备 oracle 的七个案例中，DSP 输出与 portable CPU core 的每个 keypoint 字段及 descriptor 字节完全相同；OpenCV oracle 只检查特征数量和有效性，不代表与 OpenCV 检测器逐位一致。

AidLux V73 的合成 microbenchmark 使用 200×200 图像、300 个特征、3 层、5 次预热和 20 次采样。提取 p50/p95 为 DSP 45.3244/45.4193 ms、portable CPU core 10.6742/14.1625 ms、OpenCV CPU ORB 1.93681/5.7585 ms；300×300 匹配 p50/p95 为 DSP 2.32049/2.49559 ms、portable CPU 0.174942/0.185931 ms、OpenCV CPU BF 0.291188/0.489879 ms。此 microbenchmark 显示当前 DSP facade 慢于测得的 CPU 路径，不代表端到端 tracker 性能，也不应解读为加速或精度保证。

实现边界为：图像最大 1024×1024；每次最多 2048 个特征；1–8 层；scale factor 1.05–2.0；ORB 固定 WTA_K=2、patch size 31、edge threshold 31、FAST threshold 20。DSP workspace 要求 128 字节对齐，大小不超过 16 MiB。超出范围会报错。

## 设备检查与软件验证

`rbt_m3t_dsp_device_test` 用于直接检查设备 skeleton/stub；可选 `--benchmark` 输出本地测量：

```bash
build/m3t-dsp/rbt_m3t_dsp_device_test \
  "$RBT_M3T_DSP_STUB" "$RBT_M3T_DSP_MODULE" --benchmark
```

省略 `--benchmark` 可只运行功能检查，不输出计时数据。

验证原生 tracker profile 时，`verify_software.py --texture-backend dsp` 要求两个环境变量是绝对、可读的设备库路径，并检查非空 DSP 审计事件。DSP 需要 extended 或 hard-constraint profile：

```bash
export RBT_M3T_DSP_STUB=/abs/path/librbt_m3t_orb_stub.so
export RBT_M3T_DSP_MODULE=/abs/path/librbt_m3t_orb_skel.so
python3 perception/m3t/scripts/verify_software.py \
  --build-dir build/m3t-dsp --composer "$RBT_PREFIX/bin/composer" \
  --output-dir target/m3t-dsp-extended --rendering hardware \
  --profile extended --texture-backend dsp
```

该 verifier 会为 tracker 打开 `RBT_M3T_DSP_AUDIT=1` 并确认 extraction 与 matching 都产生实际事件。最终 ARM64 extended DSP+reinitialization profile 通过 20 批检查，整次运行 13.855 秒；该值是包含 fixture、tracker 和重初始化步骤的端到端时间，不是 FPS。软件 fixture 的关节误差也只说明本次合成输入结果，不能代替物理 SO-101 精度或相机验收。

验证记录见 `perception/m3t/VERIFICATION.md`；本次 ARM64 文件证据保存在源码仓库 `target/aidlux-m3t-0.3.0-dsp-validation/final/`，独立 DSP oracle 和 benchmark 输出位于 `target/m3t-hexagon-bootstrap-20261023/evidence/` 与 `target/aidlux-m3t-0.3.0-dsp-validation/phase3/evidence/`。这些 target 目录不属于源码包。

ORB pattern 的 BSD-3-Clause 声明见 [`licenses/opencv-orb.txt`](licenses/opencv-orb.txt)；其他 M3T 与 DSP 声明见 [`../NOTICE.md`](../NOTICE.md)。QAIC 生成代码使用 [`licenses/qaic-LICENSE.txt`](licenses/qaic-LICENSE.txt)，FastRPC 公共头文件使用 [`licenses/fastrpc-LICENSE.txt`](licenses/fastrpc-LICENSE.txt)。工具链来源和生成代码哈希见 [`PROVENANCE.md`](PROVENANCE.md)。

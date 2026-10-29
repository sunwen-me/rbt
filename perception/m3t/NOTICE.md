# M3T 第三方声明

本模块静态链接 DLR-RM 的 `3DObjectTracking` 中选定的 M3T 源文件，固定版本、源码范围和许可证见 [`UPSTREAM.toml`](UPSTREAM.toml)。上游 `LICENSE` 声明 MIT 许可，版权行如下：

```text
Copyright (c) 2023 Manuel Stoiber, German Aerospace Center (DLR)
```

上游许可证要求在软件副本或重要部分中保留版权和许可声明；固定上游 checkout 的 `LICENSE` 文件应随源码分发。

构建时，[`cmake/UpstreamM3T.cmake`](cmake/UpstreamM3T.cmake) 在构建目录生成有精确范围检查的 overlay：修正未知 `Body::IDType` 的返回路径、Renderer 的临时对象引用及 Linux GLFW/GLEW 平台选择；Texture 的头文件和实现则按 OpenCV 版本及可选组件选择 SIFT、DAISY/FREAK 和 CUDA ORB，对不可用描述器明确报错。该 overlay 也将 M3T 的 optimizer 接入标量 Gaussian 关节角先验。脚本确认每个预期代码块恰好出现一次；原始固定版本 checkout 和已固定的上游提交保持未修改。overlay 不改变上游来源或许可证，完整改动清单见 `UPSTREAM.toml`。

SO-101 `StampedBytes` 状态 JSON 解码器私有使用未修改的 nlohmann/json v3.11.3 单头文件，源码位于 [`third_party/nlohmann/json.hpp`](third_party/nlohmann/json.hpp)。它用于跨 OpenCV 版本正确读取 `fault: null` 等 JSON 值及完整的无符号 64 位整数序号；此前 OpenCV `FileStorage` 对 `null` 的处理存在兼容问题。上游 tag 与文件来源、SHA-256 及用途见 [`third_party/nlohmann/PROVENANCE.txt`](third_party/nlohmann/PROVENANCE.txt)，MIT 许可见 [`third_party/nlohmann/LICENSE.MIT`](third_party/nlohmann/LICENSE.MIT)。安装时两份声明文件复制到 `share/rbt-m3t/licenses/nlohmann-json/`；本 NOTICE 与 README、`UPSTREAM.toml` 和 `VERIFICATION.md` 一同安装到 `share/rbt-m3t/`。

SO-101 描述与网格来自外部用户模型，不随本模块复制或分发。其包清单当前未给出确定许可；使用者需自行向权利人确认。本模块的 M3T 许可证声明不授予 SO-101 网格或其他相机/机器人资产的再分发权。

上游内嵌的 tinyobjloader（MIT）与 nanoflann 及其 adaptor（BSD）许可文本，以及 M3T 的完整 MIT 文本，保存在 `licenses/`，安装时一并复制到 `share/rbt-m3t/licenses/`。

可选 Hexagon DSP ORB 的本地实现采用 MIT 许可。`dsp/src/orb_pattern_31.h` 中的 ORB pattern 数据取自 OpenCV 4.10.0 `modules/features2d/src/orb.cpp` 的 `bit_pattern_31_`；其 BSD-3-Clause 声明保存在 [`dsp/licenses/opencv-orb.txt`](dsp/licenses/opencv-orb.txt)。

QAIC 生成 DSP skeleton 和 ARM AP stub；stub 使用 FastRPC runtime、公共头文件及其 session-control API，并非由 FastRPC 生成。QAIC 与 FastRPC 许可文本及声明、复制的 FastRPC 公共头文件许可、compiler-rt helper 许可和 OpenCV ORB pattern 声明均随源码保存在 `dsp/licenses/`；M3T 安装时也会安装这些文件到 `share/rbt-m3t/dsp/licenses/`。分发 DSP 构建时须保留适用许可声明。来源与哈希见 [`dsp/PROVENANCE.md`](dsp/PROVENANCE.md)。

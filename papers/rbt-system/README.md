# RBT：系统设计与工程验证

本文对应 RBT 0.8.0，介绍通用机器人系统设计，并汇总软件工程验收结果。作者当前写作占位为“RBT 项目组”；修改 `main.tex` 中的 `\author{RBT 项目组}` 即可替换。

## 构建

需要本机已安装 XeLaTeX、latexmk、ctex、TikZ、booktabs，以及以下字体：Noto Serif CJK SC、Noto Sans CJK SC、Noto Sans Mono CJK SC、TeX Gyre Pagella、TeX Gyre Heros 和 DejaVu Sans Mono。构建只调用本地 TeX 工具和字体，不会下载依赖。

在本目录运行以下命令，latexmk 会按需运行 XeLaTeX 多遍，并将 PDF 放入本地 `build/`：

```sh
latexmk -xelatex -interaction=nonstopmode -halt-on-error -outdir=build main.tex
mkdir -p ../../output/pdf
cp build/main.pdf ../../output/pdf/rbt-system-zh.pdf
```

也可直接运行 `make` 生成并复制 PDF。若需手动控制两遍 XeLaTeX，可运行：

```sh
mkdir -p build
xelatex -no-shell-escape -interaction=nonstopmode -halt-on-error -output-directory=build main.tex
xelatex -no-shell-escape -interaction=nonstopmode -halt-on-error -output-directory=build main.tex
mkdir -p ../../output/pdf
cp build/main.pdf ../../output/pdf/rbt-system-zh.pdf
```

使用 Makefile 需要 GNU make；`make sourcezip` 会生成仅含 `main.tex`、`results.tex`、本 README、Makefile 和 `evidence.json` 的源码包 `../../output/pdf/rbt-system-latex.zip`。`make clean` 只删除本目录的 `build/`。

## 证据与范围

`evidence.json` 汇总验收结果、选定指标、证据文件相对路径及 SHA-256。完整原始日志保留在仓库的 `target/` 目录中，该目录不纳入源码包。汇总数据和校验值便于定位、核对记录，但不能代替原始日志或相应源码快照。

软件全量门禁在 0.7.0 预发布源码快照上通过，之后单独完成 0.8.0 发布复核；局部坐标变换基准为 Debug 构建，每种查询 10,000 次且不包含传输。历史记录未提供 CPU、内核和系统负载元数据。进程实验使用合成软件夹具。物理相机 TF、真实动态位姿设备均未运行，通用 TF 消息的 MCAP 录制回放也未验证。本文的构建说明只覆盖论文 PDF 生成，不声称由源码包重现全部软件验收或基准环境。

外部背景资料均指向原始或官方来源：

- ROS 2 系统设计综述：[Robot Operating System 2: Design, architecture, and uses in the wild](https://doi.org/10.1126/scirobotics.abm6074)。
- iceoryx2 v0.10.0 README：[版本固定的原始文件](https://raw.githubusercontent.com/eclipse-iceoryx/iceoryx2/v0.10.0/README.md)。
- ROS 2 生命周期设计：[Managed nodes](https://design.ros2.org/articles/node_lifecycle.html)。
- ROS 2 Jazzy tf2 文档：[官方仓库中的 About-Tf2.rst](https://github.com/ros2/ros2_documentation/blob/jazzy/source/Concepts/Intermediate/About-Tf2.rst)。
- MCAP 格式：[官方规范](https://mcap.dev/spec)。

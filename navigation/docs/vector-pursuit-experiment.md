# Vector Pursuit 局部跟踪实验（2026-10-28）

本轮先做软件仿真对比，不切换默认控制器。固定参数和公共 RBT 封装下，80 次闭环全部到达；VP 没有显示出整体跟踪优势，继续使用 RPP + DWPP 作为默认配置。完整回归与原生 IPC 结果在本报告后半部分记录。

## 实验边界

移植的是 [Vector Pursuit 固定版本](https://github.com/blackcoffeerobotics/vector_pursuit_controller/blob/0a8b8ee32b2ffcc25826be61fd289d2a0e72efbc/src/vector_pursuit_controller.cpp#L269-L303) 的转弯半径计算。四种组合共用现有路径分段、RPP 速度调节、整车 footprint、命令轨迹及实测速度制动检查；不引入 ROS 2，不修改底盘消息、Guard、真实驱动或 Dock。这是几何核的受控对比，不能当作原版 VP 插件完整速度策略与 Nav2 RPP 的排名。

| 记录名 | 几何引导 | 速度选择 |
| --- | --- | --- |
| `rpp` | Pure Pursuit XY 曲率 | 公共调速后逐轴限幅 |
| `rpp_dwpp` | Pure Pursuit XY 曲率 | 公共调速后动态速度窗口 |
| `vp` | 前视目标 XY 与车身 yaw | 公共调速后逐轴限幅 |
| `vp_window` | 前视目标 XY 与车身 yaw | 公共调速后动态速度窗口 |

默认入口仍为 `rpp-dwpp`。新增可选入口 `--controller vector-pursuit`、`--controller vector-pursuit-window`，以及纯 C++ `VectorPursuit` 包装器。VP 的朝向按所在路径段的实际比例、最短角差插值，倒车 yaw 仍表示车身朝向。近零横向偏移、无限转弯半径和最小半径分支保持有限输出；没有新增到达门槛。

本地 DWPP 的窗口以上一条速度命令为中心，在窗口内解析选择接近 `ω=κv` 的命令；没有做完整 DWA 候选轨迹评分，也没有把响应滞后模型当作实机下一周期的可达性模型。

## 固定路径与底盘模型

五个场景各生成一次路径和栅格地图，再由四种控制器分别运行。每个场景有两种响应模型、两种初始位姿，共 20 个匹配条件、80 次运行。已验证每个匹配条件下路径 CSV、地图 CSV 逐字节相同，初始实际位姿也相同；扰动不会改写参考路径。

| 场景 | 路径来源 |
| --- | --- |
| S 弯 | 固定版本官方差速 Lattice 原语拼接 |
| 窄弯 | 同一原语集的 90° 弯，地图按整车扫掠轮廓加 0.08 m 生成通道 |
| 前进后倒车 | 人工构造的直线差速可执行段，车身 yaw 不翻转，通道宽 0.60 m |
| 旋转后平移 | 人工构造的原地 90° 旋转及直线段 |
| 车尾扫掠 | 当前 StateLatticePlanner 实际搜索出的路径；直接原地转动会扫到障碍 |

统一使用 [前驱底盘几何](../config/front-drive-geometry.yaml)，不包含机械臂；地图为 0.05 m/格，内部参考点为前驱轮轴。公共参数为 50 Hz、线速度上限 0.20 m/s、角速度上限 0.50 rad/s、线加减速度 0.8 m/s²、角加减速度 2.5 rad/s²；VP `k=5`、最小转弯半径 0.10 m。完整参数保存在各次 `result.json`，没有按某组结果单独调优。

响应模型：`ideal` 立即执行速度；`lagged` 使用 0.10 s 命令延迟、时间常数 0.12 s 的一阶响应，并限制实际加速度。后者是灵敏度实验假设，尚未用实机辨识。初始位姿为对齐状态，或横向偏移 0.03 m、yaw 偏移 0.05 rad。模拟使用虚拟时钟和现有 SimulatedBaseAdapter，不进行实时休眠或硬件运动。

## 结果与解释

以下是相同 20 个条件下，**每次运行的移动样本 RMS 再等权取均值**；不是把所有采样点混合计算，也不是多次随机试验的统计结论。移动样本定义为实测线速度绝对值大于 0.01 m/s，原地旋转不计入这两个路径跟踪 RMS；终点仍使用已有的位置、朝向和近零实测/命令速度判定。

| 组合 | 到达 | 位置 RMS 均值 | 车身朝向 RMS 均值 | 平均虚拟任务时间 |
| --- | ---: | ---: | ---: | ---: |
| RPP | 20/20 | 0.782 cm | 2.403° | 29.087 s |
| RPP + DWPP | 20/20 | 0.788 cm | 2.417° | 29.017 s |
| VP | 20/20 | 1.011 cm | 2.803° | 29.132 s |
| VP + 窗口 | 20/20 | 1.012 cm | 2.746° | 29.205 s |

表中时间为虚拟闭环时间；完整 80 次实验程序的墙钟耗时为 24.231 s，不是实机执行时间或控制循环最坏耗时。表中 RMS 是路径跟踪误差，终点仍沿用原有 0.08 m / 0.06 rad 容差。

80 次运行均通过独立的填充多边形/占据格 SAT 与运动中间姿态检查，采样位移尺度不大于 2.5 mm；没有实际碰撞，命令线/角速度增量超限计数均为零。控制器本身的生产碰撞检查之外，实验另行计算整车及车体 `x≤−0.196 m` 后部区域到障碍与地图边界的间隙。

四组全局最小后部间隙均为 9.563 mm，来自共同的车尾扫掠扰动起点；该数值受共同初始几何主导，不能用来区分四种控制器。S 弯和窄弯的 VP 位置及朝向 RMS 均高于当前 RPP + DWPP；VP + 窗口相对 VP 有少量变化，未改变本轮整体结论。关闭窗口的 RPP 与 RPP + DWPP 很接近，这些低速参数下速度窗口介入较少，不能据此推断更强加减速约束下的差异。

因此本轮保留默认 RPP + DWPP，VP 两种组合作为可选实验模式。这个结果适用于本轮几何适配、公共调速、参数和假设响应模型，尚未比较原版 VP 速度策略、实机延迟、轮胎打滑或定位噪声。

原始数据见 [80 次运行摘要](../../output/navigation-vp-experiment/release/summary.json)、[主代理独立汇总](../../output/navigation-vp-experiment/verification/root-aggregate.json)。S 弯四种控制器还分别在独立 smoke 与完整实验中运行，12 份路径/地图/遥测 CSV 的 SHA-256 完全一致，见 [重复性记录](../../output/navigation-vp-experiment/verification/repeatability-s-curve.json)。

主代理另从 116,521 条 CSV 遥测独立重算 RMS，并核对每个速度命令的绝对上限、相邻增量（含首条命令）、时间顺序和碰撞标记，结果全部一致，见 [CSV 核对记录](../../output/navigation-vp-experiment/verification/csv-audit.json)。

可视化见 [20 条件完整对照图](../../output/navigation-vp-experiment/demo/comparison.png)、[S 弯轨迹](../../output/navigation-vp-experiment/demo/trajectory-S_curve-ideal-aligned.png)、[窄弯轨迹](../../output/navigation-vp-experiment/demo/trajectory-narrow_turn-ideal-aligned.png)、[扫尾轨迹](../../output/navigation-vp-experiment/demo/trajectory-tail_sweep-ideal-aligned.png)。[可视化聚合](../../output/navigation-vp-experiment/demo/aggregate.json) 保留每个控制器 20 条件的分母，六对算法的共同成功、指标完整条件也均为 20。

[四组同步视频](../../output/navigation-vp-experiment/demo/video/trajectory-S_curve-lagged-perturbed.mp4) 展示 S 弯、滞后响应、起点扰动这一条件。四个窗口按同一虚拟时刻播放，从 CSV 实际位姿绘制完整底盘 footprint；到达后冻结，不能把视频播放时长当作任务执行时间。视频的单条件聚合另存于 `demo/video/`，未覆盖上面 20 条件的完整结果。来源与文件 SHA-256 见 [视频元数据](../../output/navigation-vp-experiment/demo/video/render-metadata.json)。

最终 MP4 为 1792×896、20 fps、240 帧、12 s，覆盖最长 24.44 s 的虚拟任务。主代理独立解码全部帧并抽查画面，核对主图 242 份、视频 14 份来源文件及所有输出的 SHA-256，见 [媒体核验](../../output/navigation-vp-experiment/verification/media-root-audit.json)。

## 回归与原生 IPC

| 配置 | 结果 | 记录 |
| --- | --- | --- |
| Debug | 49/49，196.39 s | [日志](../../output/navigation-vp-experiment/verification/debug.log) |
| Release | 49/49，190.88 s | [日志](../../output/navigation-vp-experiment/verification/release.log) |
| ASan + leak detection | 49/49，291.47 s | [日志](../../output/navigation-vp-experiment/verification/asan.log) |
| UBSan | 49/49，206.33 s | [日志](../../output/navigation-vp-experiment/verification/ubsan.log) |

[JUnit 汇总](../../output/navigation-vp-experiment/verification/matrix-summary.json) 与 [实际构建/执行命令及退出码](../../output/navigation-vp-experiment/verification/matrix-runs.json) 均保留在输出目录。

另外实跑两种 VP 模式各三个原生跨进程场景：转弯、倒车、车尾扫掠，共 **6/6 通过**。每次均由独立导航、VelocityGuard、模拟底盘和数据源进程通信，观察到 Guard 后的最终速度消息及模拟底盘消费；规划路径和实际运动的整车中间姿态检查均通过。运行使用独立服务命名空间和最终速度话题，不连接真实底盘。见 [原生 IPC 结果](../../output/navigation-vp-experiment/verification/native-summary.json) 和 [执行命令](../../output/navigation-vp-experiment/verification/native-runs.json)。这组复用场景的速度上限为 0.30 m/s、0.80 rad/s；不混入前述 0.20 m/s、0.50 rad/s 的 80 次固定路径指标。

关闭 RBT 原生通信和地图文件依赖的 core-only 构建 **13/13 通过**，见 [JUnit](../../output/navigation-vp-experiment/verification/core-only.xml)。Release 安装后，另建消费项目通过已安装的 CMake package 链接公共 `VectorPursuit` 接口，加载 8 顶点 footprint 与 112 个原语，输出有限且向前的速度；速度消息 ABI 仍为 112 字节。安装、配置、编译和运行退出码均为 0，见 [安装验证记录](../../output/navigation-vp-experiment/verification/export-runs.json) 与 [消费程序输出](../../output/navigation-vp-experiment/verification/consumer-run.log)。

新增两项为 VP 几何/路径动作/碰撞回归和实验 artifact smoke；保留原有 47 项。smoke 独立解析 JSON、核对四种模式和文件存在性、验证同一路径，允许实验控制器失败作为有效记录，不把“生成结果”当成“算法成功”。首轮实验工具修正了旋转速度默认值与低速上限不一致、summary JSON 漏引号问题；原始记录分别保存在 `initial-focused.log` 和 `initial-smoke-invalid-summary.txt`，没有修改到达容差或碰撞判断来修复。

## 复现与修改文件

```bash
LD_LIBRARY_PATH=/home/sw/rbt/target/phase4-toolchain/lib:/home/sw/rbt/target/map-image-sdk/lib \
  /home/sw/rbt/target/navigation-lio-release/rbt_navigation_pursuit_benchmark \
  --geometry /home/sw/rbt/navigation/config/front-drive-geometry.yaml \
  --lattice /home/sw/rbt/navigation/config/lattice/diff-5cm-0_5m.json \
  --output /home/sw/rbt/target/pursuit-experiment-replay
```

本机已有绘图环境的渲染命令如下；绘图依赖不进入导航 C++ 核心。

```bash
/home/sw/rbt/target/navigation-video-tools/bin/python \
  /home/sw/rbt/navigation/scripts/render_pursuit_experiment.py \
  --summary /home/sw/rbt/output/navigation-vp-experiment/release/summary.json \
  --output-dir /home/sw/rbt/target/pursuit-video-replay \
  --scenario S_curve --dynamics lagged --start-variant perturbed
```

新增模块为 `controller/vector_pursuit.hpp`、`src/vector_pursuit.cpp`、`tests/vector_pursuit_test.cpp`、`tests/pursuit_benchmark.cpp`、`scripts/pursuit_benchmark_smoke.py`、`scripts/render_pursuit_experiment.py` 和 `licenses/vector-pursuit/`。修改已有 ControllerConfig/RPP 的几何选择、runtime 控制器选择、参数帮助、Lattice IPC runner、CMake、README 和 NOTICE。现有 RPP/DWPP 默认配置、A*、Lattice 规划器、整车几何及消息契约保持原值。

来源许可见 [Vector Pursuit 记录](../licenses/vector-pursuit/README.md)。测试前冻结的源码与资源 SHA-256 见 [tested-source.json](../../output/navigation-vp-experiment/verification/tested-source.json)。

本轮 8 个修改文件和 9 个新增文件的完整路径、用途与 SHA-256 见 [修改清单](../../output/navigation-vp-experiment/verification/changed-files.json)；本次测试、实验、安装与媒体核验的汇总见 [final-results.json](../../output/navigation-vp-experiment/verification/final-results.json)。

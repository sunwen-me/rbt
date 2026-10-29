# Nav2 RPP + DWPP 移植记录

依据用户在 2026-10-21 的修正，默认控制器改为 Regulated Pure Pursuit（RPP）并启用 Dynamic Window Pure Pursuit（DWPP）。参考是 [Nav2 navigation2](https://github.com/ros-navigation/navigation2) 的固定提交 `cea9ed9d2e55b4e084be705a495a721368021f19`，不是对旧 Pure Pursuit 的改名。

## 原始调用与依赖

`ControllerServer / FeasiblePathHandler` 负责路径裁剪、进度与局部坐标转换；`RegulatedPurePursuitController::computeVelocityCommands` 计算 carrot/curvature、旋转或 RPP 速度调节，跟踪分支可选 DWPP，最后由 CollisionChecker 检查命令。原类依赖 ROS 生命周期、TF、ROS 消息、pluginlib、Nav2 costmap 和参数系统。这些运行时依赖均不移植。

| 上游文件 | 本地对应与处理 |
|---|---|
| `nav2_util/src/controller_utils.cpp` | 最近投影后按剩余路径弧长插值 carrot；当前固定版本的 getLookAheadPoint 使用弧长，早期圆交点实现不适用于此版本 |
| `nav2_regulated_pure_pursuit_controller/src/regulated_pure_pursuit_controller.cpp` | `src/pure_pursuit.cpp`：真正的 RegulatedPurePursuit 类，路径几何、旋转、速度约束和 DWPP 调用 |
| `include/.../regulation_functions.hpp` | 曲率半径约束、距离约束、末端减速；地图使用现有 ESDF，直接读保守障碍净距，不伪造 Nav2 inflation cost |
| `include/.../dynamic_window_pure_pursuit_functions.hpp` | `src/dynamic_window.cpp`：解析动态窗口与 `ω=κv` 的最近可达速度解；以最后发出的命令构造窗口 |
| `src/collision_checker.cpp` | 命令轨迹受时间和 carrot 径向距离限制；本地用精确差速圆弧、最多四分之一栅格间距采样；旧模式用圆，前驱配置用旋转多边形，未知/越界始终拒绝 |
| `src/parameter_handler.cpp` | 转为纯 C++ ControllerConfig；无 ROS 参数回调，构造时严格验证参数 |

## 算法与有意差异

RPP 的自适应前视为 `clamp(abs(v_measured)*lookahead_time, min, max)`。carrot 转到 base_link 后计算 `κ=2y/(x²+y²)`；曲率半径小于阈值时按半径比例降速。距离调节使用 ESDF 的保守中心净距。末端调节在剩余弧长低于阈值后，按欧氏目标距离减速，并保留可配置最低接近速度。最终朝向和大角度路径朝向使用旋转分支。

官方 DWPP 是解析解，不是 DWA 轨迹评分采样器。动态窗口由最后命令、周期 dt、线/角速度和加减速度约束给出；再与 RPP 的调节速度区间求交。与 `ω=κv` 相交时选择行进方向上最快的交点；不相交时选择最接近该曲率线的窗口角点。RBT 配置中的减速度使用正的幅值，适配为相同的约束语义。不可瞬间达到的降速保持可达制动边界，随后仍必须通过碰撞检查。

本地保留额外的有限值、地图版本、新鲜 TF/Odom、路径偏离和当前 footprint 检查。实测运动单独检查反应时间加制动距离，避免在最后命令与真实底盘速度不同时忽略停车距离；此检查不受 carrot 截断。RPP-only 模式也施加 RBT 的加速度约束。原生运行默认 `--controller rpp-dwpp`，`--controller rpp` 可作对照。`DiffDrivePurePursuit` 仅保留为初版 API 名称的别名，实际实现和原生默认实例均为 RegulatedPurePursuit。

本地保留圆形兼容模式，前驱配置使用多边形 footprint、不可变地图快照以及 RBT 的 TF 查询，不能声称与 Nav2 的 inflation costmap 或整套 controller server 完全等价。ROS 2 不参与本包编译和运行。

规划与执行的半径明确分开：原生示例的物理半径为 0.18 m，规划半径为物理半径加可配置 `planning-clearance`（默认 0.10 m）。这给 RPP 在栅格折角处的连续跟踪保留净距，不修改墙体或放宽执行碰撞检查。纯 A* 的默认参数和最短路径测试不因示例配置改变；没有足够跟踪净距的路线仍可能以 Collision 安全失败。

2026-10-28 的旧圆形 `obstacle` / `map_dropout` runner 场景显式使用 0.20 m 规划余量，避免重规划起点进入额外规划余量区；其他旧场景仍使用 0.10 m。前驱 State Lattice 使用真实多边形和零额外规划余量。这是演示配置差异，不是放宽实体碰撞检查或修改 A* 算法。

## 许可

RPP 包的 package.xml 声明 Apache-2.0。保留 Shrijit Singh（2020）、Samsung Research America（2020/2022）、Fumiya Ohnishi（2025）对应源码的版权头，随包提供 Apache-2.0 全文与上游许可策略文件。没有复制 Nav2 中其他许可的包。

参考论文：[Regulated Pure Pursuit for Robot Path Tracking](https://arxiv.org/abs/2305.20026)；[DWPP: Dynamic Window Pure Pursuit Considering Velocity and Acceleration Constraints](https://arxiv.org/abs/2601.15006)。
## 2026-10-28 路径动作顺序接入

前驱 State Lattice 配置启用原生 `FeasiblePathHandler`，参考同一固定提交的
[官方实现](https://github.com/ros-navigation/navigation2/blob/cea9ed9d2e55b4e084be705a495a721368021f19/nav2_controller/plugins/feasible_path_handler.cpp)。
保留原地旋转与换向 cusp 的顺序，分段使用既有 RPP 容差执行，内部段完成后推进路径，
只有末段完成才报告导航成功。没有移植 ROS TF、参数回调或 pluginlib 生命周期代码。
当前原语的旋转检测阈值为 0.01 rad；未使用上游默认的 0.785 rad，避免跳过细分旋转动作。
多边形车体下，现有命令预测和实测停车轨迹改用完整车体及角运动查询；DWPP 解析速度窗口保持原算法。
实现与验证入口见 [State Lattice 说明](state-lattice-design.md)。

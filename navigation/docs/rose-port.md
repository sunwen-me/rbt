# ROSE 源码调查与移植边界

上游：[hyheiyue/rose_navigation](https://github.com/hyheiyue/rose_navigation)，固定提交 `c979eb8e36d1621b31a04884af66ee3e3f5e8afd`。

调查调用链为 `RosePlanner::Impl → RoseMap + AStar → ESDF → BinMap`；`RoseMap` 还持有三维 `OccMap`。`rose_map.cpp` 点云回调将 PointCloud2 转为目标坐标系，再入 Occupancy 帧队列；里程计移动滑动窗口。`OccMap` 有 log-odds、hit/free worker 和可选三维 DDA raycast；通过 `ParamsNode` 间接依赖 ROS 参数、`rclcpp::ok()`、TBB 与线程锁。

`BinMap` 的静态加载依赖 yaml-cpp/OpenCV；动态投影使用高度、点数、法向和坡度过滤，依赖 small_gicp/TBB。上游 `rose_map.cpp` 的 process_thread 中 `bin_map_->update(...)` 被注释，因此不能假设当前上游已经连通动态 OccMap→BinMap 更新。`ESDF::rebuild_signed` 从二值快照计算到占据与空闲的两个距离场相减，距离传播是八邻域前后扫描近似，并非精确欧氏距离。

上游 A* 直接依赖 `RoseMap::esdf()`、Eigen、`ParamsNode` 和 ROS 日志；有不安全起终点 BFS 投影、越界目标投影和倒数 clearance 代价。其越界占据查询返回 false，八邻域没有要求的角点阻挡检查，启发函数增加 clearance 后也不保证最短栅格路径。直接拷贝无法满足本任务。

| 调查的上游文件 | 本地实现 | 复用/改写 |
|---|---|---|
| `src/map/occ_map.{hpp,cpp}` | `OccupancyGrid` 输入 | 不移植三维建图、队列、raycast、ROS 节点；已有地图源通过原生切片输入 |
| `src/map/bin_map.hpp` | `src/grid_map.cpp` | 借鉴二值占据表示；改为有界稠密二维网格，未知保守占据，校验分辨率/原点/尺寸 |
| `src/map/esdf.{hpp,cpp}` | `src/grid_map.cpp` | 保留双距离场相减语义；重写为精确二维 separable EDT；碰撞查询再减半格对角线/查询点偏移，加入地图边界 |
| `src/map/voxel_map.hpp` | `GridMap::worldToGrid/gridToWorld` | 不复制滑动/哈希容器；重写边界和旋转原点转换 |
| `src/planner/path_search/a*.{hpp,cpp}` | `src/a_star.cpp` | 改写优先队列/父节点网格搜索；使用 admissible octile 启发、严格失败结果、角点阻挡、连续边验证、超时/取消 |
| `src/planner/rose_planner.cpp` | `src/navigation_manager.cpp` | 仅调查调度/安全关系；独立实现差速状态机，不移植优化器或全向 MPC |
| `src/utils/*` | 无拷贝 | 不引入 ROS ParamsNode、ROS 日志、消息、TBB 或新队列框架 |
| `CMakeLists.txt` / `LICENSE.txt` / `3rdparty/*` | `CMakeLists.txt` / `NOTICE.md` / `licenses/ROSE-MIT.txt` | 不用递归 glob 引入整个上游；保存 MIT 版权，排除未使用第三方代码 |

控制器采用纯 C++ 适配的 Nav2 RPP + DWPP，仅输出 v/ω；来源与差异见 [nav2-port.md](nav2-port.md)。所有原生消息转换位于 `runtime/native.hpp` 和 `src/native.cpp`，算法头文件不包含 ROS 或 iceoryx2。

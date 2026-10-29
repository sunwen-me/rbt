# 文件清单

本清单的完整文件计数只覆盖 `navigation/` 包。导航速度边界拆分阶段新增 6 个、修改 17 个、删除 1 个文件；同仓库独立硬件驱动与消息包不计入下方 navigation 文件数。

## 本次新增

- [docs/base-controller-contract.md](../docs/base-controller-contract.md)
- [include/rbt_navigation/safety/velocity_guard.hpp](../include/rbt_navigation/safety/velocity_guard.hpp)
- [include/rbt_navigation/simulation/simulated_base.hpp](../include/rbt_navigation/simulation/simulated_base.hpp)
- [src/velocity_guard_main.cpp](../src/velocity_guard_main.cpp)
- [tests/base_controller_sim.cpp](../tests/base_controller_sim.cpp)
- [tests/velocity_guard_test.cpp](../tests/velocity_guard_test.cpp)

## 本次修改

- [CMakeLists.txt](../CMakeLists.txt)
- [README.md](../README.md)
- [config/demo.conf](../config/demo.conf)
- [docs/architecture.md](../docs/architecture.md)
- [docs/files.md](../docs/files.md)
- [docs/verification.md](../docs/verification.md)
- [generated/cpp/rbt/navwire.hpp](../generated/cpp/rbt/navwire.hpp)
- [generated/descriptors/navwire.json](../generated/descriptors/navwire.json)
- [generated/rust/navwire.rs](../generated/rust/navwire.rs)
- [include/rbt_navigation/runtime/native.hpp](../include/rbt_navigation/runtime/native.hpp)
- [include/rbt_navigation/safety/safe_base.hpp](../include/rbt_navigation/safety/safe_base.hpp)
- [scripts/native_integration.py](../scripts/native_integration.py)
- [src/native.cpp](../src/native.cpp)
- [src/runtime_main.cpp](../src/runtime_main.cpp)
- [src/safe_base.cpp](../src/safe_base.cpp)
- [tests/native_fixture.cpp](../tests/native_fixture.cpp)
- [types/navigation.rbt](../types/navigation.rbt)

## 本次删除

- `src/safe_base_main.cpp`：原混合安全/模拟底盘进程，由 Guard 与测试底盘分开替代。

## 视频录制补充

新增以下只读记录与绘图工具；另更新 CMake、README 和验收记录。导航算法与原测试不变。

- [tests/telemetry_recorder.cpp](../tests/telemetry_recorder.cpp)
- [scripts/simulation_video.py](../scripts/simulation_video.py)

## 硬件源码调查补充

新增 [docs/hardware-sources.md](../docs/hardware-sources.md)，并更新 README 和底盘接口契约。参考压缩包及独立 driver 包源码不计入 navigation 包源码清单。

## 硬件约定与 TF 归属修正（2026-10-22）

按用户指定的 current YAML 确定硬件参数和 ROS 2 IMU 单位。将测试底盘的 TF 发布移到外部 fixture，底盘保留里程计与状态输出、独立期限检查、watchdog 和急停。修改 `tests/base_controller_sim.cpp`、`tests/native_fixture.cpp`、`scripts/native_integration.py`，并同步 README、architecture、base-controller-contract、hardware-sources、verification 与本清单。没有新增源码文件或改动导航算法。

## 原生硬件包补充（2026-10-22）

上方 navigation 包清单不包含以下同仓库独立 package 源码；它们是与 navigation 分开构建的原生硬件进程与 wire 契约。真实设备、单位、轴向和硬件急停仍未验证；PTY 集成只模拟串口。

- [drivers/base_controller](../../drivers/base_controller/)：`rbt_base_controller` DDSM 原生进程；配置见 [ddsm_robot_current.yaml](../../drivers/base_controller/config/ddsm_robot_current.yaml)。
- [drivers/dm_imu](../../drivers/dm_imu/)：`rbt_dm_imu` 原生串口进程及解析器，来源/协议限制见 [NOTICE.md](../../drivers/dm_imu/NOTICE.md)。
- [drivers/hardware_messages](../../drivers/hardware_messages/)：`rbt_hardware::messages` 导出的 `Imu`、`WheelOdometry`、`BaseStatus` ABI。
- [drivers/tests](../../drivers/tests/)：hardware-process PTY 集成测试入口；它们不是实际电机或 IMU 测试。

底盘通过 `rbt_navigation::messages` 和 `rbt_hardware::messages` 复用 wire，不链接导航算法、不发布 TF；外部 TF 发布组件维护所需动态 TF。接口细节、当前 YAML、设备 wire 单位/tag/轴向限制及原始 license 声明见 [hardware-sources.md](../docs/hardware-sources.md) 与 [base-controller-contract.md](../docs/base-controller-contract.md)。测试结果由根工作区最终验收维护，本文件不声明测试通过。

## 通用栅格地图文件接入（2026-10-22）

新增文件地图加载库、独立地图发布进程、加载测试与通用 YAML/PGM 样例；修改 CMake、安装组件配置、fixture/runner 和相关说明。`src/native.cpp` 只增加测试参数名；算法、Manager、原非 IPC 测试和 IDL 不变。

- [include/rbt_navigation/map/map_io.hpp](../include/rbt_navigation/map/map_io.hpp)
- [src/map_io.cpp](../src/map_io.cpp)
- [src/map_server_main.cpp](../src/map_server_main.cpp)
- [tests/map_io_test.cpp](../tests/map_io_test.cpp)
- [docs/map-files.md](map-files.md)
- [maps/README.md](../maps/README.md)
- [maps/simulation_obstacle.yaml](../maps/simulation_obstacle.yaml)
- [maps/simulation_obstacle.pgm](../maps/simulation_obstacle.pgm)

## 复杂 PGM 场景与视频（2026-10-22）

新增通用六进程仿真 fixture、实际采样运行/视频工具，以及多房间和仓库的 PGM/YAML/场景配置与确定性生成器。修改 CMake、README、地图说明、验收记录和本清单；原算法、Manager、已有测试和 wire 未修改。

- [tests/map_demo_fixture.cpp](../tests/map_demo_fixture.cpp)
- [scripts/complex_simulation.py](../scripts/complex_simulation.py)
- [scripts/generate_complex_maps.py](../scripts/generate_complex_maps.py)
- [maps/complex_scenarios.json](../maps/complex_scenarios.json)
- [maps/complex_rooms.yaml](../maps/complex_rooms.yaml)
- [maps/complex_rooms.pgm](../maps/complex_rooms.pgm)
- [maps/complex_warehouse.yaml](../maps/complex_warehouse.yaml)
- [maps/complex_warehouse.pgm](../maps/complex_warehouse.pgm)

## 当前包的完整文件清单

共 77 个 navigation 交付文件；构建、独立驱动包、外部参考源码与视频产物不计入本清单。

- [.gitignore](../.gitignore)
- [CMakeLists.txt](../CMakeLists.txt)
- [NOTICE.md](../NOTICE.md)
- [README.md](../README.md)
- [cmake/rbt_navigationConfig.cmake.in](../cmake/rbt_navigationConfig.cmake.in)
- [config/demo.conf](../config/demo.conf)
- [docs/architecture.md](../docs/architecture.md)
- [docs/base-controller-contract.md](../docs/base-controller-contract.md)
- [docs/files.md](../docs/files.md)
- [docs/hardware-sources.md](../docs/hardware-sources.md)
- [docs/map-files.md](../docs/map-files.md)
- [docs/nav2-port.md](../docs/nav2-port.md)
- [docs/rose-port.md](../docs/rose-port.md)
- [docs/verification.md](../docs/verification.md)
- [generated/cpp/rbt/navwire.hpp](../generated/cpp/rbt/navwire.hpp)
- [generated/descriptors/navwire.json](../generated/descriptors/navwire.json)
- [generated/rust/navwire.rs](../generated/rust/navwire.rs)
- [include/rbt_navigation/controller/dynamic_window.hpp](../include/rbt_navigation/controller/dynamic_window.hpp)
- [include/rbt_navigation/controller/pure_pursuit.hpp](../include/rbt_navigation/controller/pure_pursuit.hpp)
- [include/rbt_navigation/controller/regulated_pure_pursuit.hpp](../include/rbt_navigation/controller/regulated_pure_pursuit.hpp)
- [include/rbt_navigation/core/types.hpp](../include/rbt_navigation/core/types.hpp)
- [include/rbt_navigation/map/grid_map.hpp](../include/rbt_navigation/map/grid_map.hpp)
- [include/rbt_navigation/map/map_io.hpp](../include/rbt_navigation/map/map_io.hpp)
- [include/rbt_navigation/planner/a_star.hpp](../include/rbt_navigation/planner/a_star.hpp)
- [include/rbt_navigation/runtime/native.hpp](../include/rbt_navigation/runtime/native.hpp)
- [include/rbt_navigation/runtime/navigation_manager.hpp](../include/rbt_navigation/runtime/navigation_manager.hpp)
- [include/rbt_navigation/safety/safe_base.hpp](../include/rbt_navigation/safety/safe_base.hpp)
- [include/rbt_navigation/safety/velocity_guard.hpp](../include/rbt_navigation/safety/velocity_guard.hpp)
- [include/rbt_navigation/simulation/simulated_base.hpp](../include/rbt_navigation/simulation/simulated_base.hpp)
- [licenses/Nav2-APACHE.txt](../licenses/Nav2-APACHE.txt)
- [licenses/Nav2-LICENSE-POLICY.txt](../licenses/Nav2-LICENSE-POLICY.txt)
- [licenses/RBT-APACHE.txt](../licenses/RBT-APACHE.txt)
- [licenses/RBT-MIT.txt](../licenses/RBT-MIT.txt)
- [licenses/RBT-THIRD-PARTY-NOTICES.md](../licenses/RBT-THIRD-PARTY-NOTICES.md)
- [licenses/ROSE-MIT.txt](../licenses/ROSE-MIT.txt)
- [licenses/iceoryx2-APACHE.txt](../licenses/iceoryx2-APACHE.txt)
- [licenses/iceoryx2-MIT.txt](../licenses/iceoryx2-MIT.txt)
- [licenses/iceoryx2-NOTICE.md](../licenses/iceoryx2-NOTICE.md)
- [maps/README.md](../maps/README.md)
- [maps/complex_rooms.pgm](../maps/complex_rooms.pgm)
- [maps/complex_rooms.yaml](../maps/complex_rooms.yaml)
- [maps/complex_scenarios.json](../maps/complex_scenarios.json)
- [maps/complex_warehouse.pgm](../maps/complex_warehouse.pgm)
- [maps/complex_warehouse.yaml](../maps/complex_warehouse.yaml)
- [maps/simulation_obstacle.pgm](../maps/simulation_obstacle.pgm)
- [maps/simulation_obstacle.yaml](../maps/simulation_obstacle.yaml)
- [scripts/complex_simulation.py](../scripts/complex_simulation.py)
- [scripts/generate_complex_maps.py](../scripts/generate_complex_maps.py)
- [scripts/native_integration.py](../scripts/native_integration.py)
- [scripts/simulation_video.py](../scripts/simulation_video.py)
- [src/a_star.cpp](../src/a_star.cpp)
- [src/dynamic_window.cpp](../src/dynamic_window.cpp)
- [src/grid_map.cpp](../src/grid_map.cpp)
- [src/map_io.cpp](../src/map_io.cpp)
- [src/map_server_main.cpp](../src/map_server_main.cpp)
- [src/native.cpp](../src/native.cpp)
- [src/navigation_manager.cpp](../src/navigation_manager.cpp)
- [src/pure_pursuit.cpp](../src/pure_pursuit.cpp)
- [src/runtime_main.cpp](../src/runtime_main.cpp)
- [src/safe_base.cpp](../src/safe_base.cpp)
- [src/velocity_guard_main.cpp](../src/velocity_guard_main.cpp)
- [tests/base_controller_sim.cpp](../tests/base_controller_sim.cpp)
- [tests/controller_test.cpp](../tests/controller_test.cpp)
- [tests/core_test.cpp](../tests/core_test.cpp)
- [tests/dynamic_window_test.cpp](../tests/dynamic_window_test.cpp)
- [tests/manager_test.cpp](../tests/manager_test.cpp)
- [tests/map_demo_fixture.cpp](../tests/map_demo_fixture.cpp)
- [tests/map_io_test.cpp](../tests/map_io_test.cpp)
- [tests/map_test.cpp](../tests/map_test.cpp)
- [tests/native_fixture.cpp](../tests/native_fixture.cpp)
- [tests/native_test.cpp](../tests/native_test.cpp)
- [tests/planner_test.cpp](../tests/planner_test.cpp)
- [tests/safety_test.cpp](../tests/safety_test.cpp)
- [tests/simulation_test.cpp](../tests/simulation_test.cpp)
- [tests/telemetry_recorder.cpp](../tests/telemetry_recorder.cpp)
- [tests/velocity_guard_test.cpp](../tests/velocity_guard_test.cpp)
- [types/navigation.rbt](../types/navigation.rbt)

## 2026-10-27：MID360 / SuperLIO 接入

原有算法、runtime、Guard 和测试源码保留；新增原子导航帧、组件桥接、障碍融合、验证与运行配置。完整散列对照见 `output/navigation-lio/changes.json`。

新增文件：

- `config/lio-bridge.conf`
- `config/lio-navigation.conf`
- `docs/lio-integration.md`
- `include/rbt_navigation/map/obstacle_layer.hpp`
- `include/rbt_navigation/runtime/lio_bridge.hpp`
- `include/rbt_navigation/runtime/lio_contract.hpp`
- `include/rbt_navigation/runtime/lio_output.hpp`
- `include/rbt_navigation/runtime/lio_receipt_clock.hpp`
- `integrations/mid360_lio/README.md`
- `integrations/mid360_lio/apply.sh`
- `integrations/mid360_lio/height-datum-fix.patch`
- `integrations/mid360_lio/source_integration.py`
- `integrations/mid360_lio/super_lio_rbt_navigation.patch`
- `scripts/lio_integration.py`
- `src/lio_bridge.cpp`
- `src/lio_bridge_main.cpp`
- `src/obstacle_layer.cpp`
- `tests/lio_bridge_test.cpp`
- `tests/lio_output_test.cpp`
- `tests/lio_receipt_clock_test.cpp`
- `tests/lio_replay_fixture.cpp`
- `tests/lio_source_probe.cpp`
- `tests/obstacle_layer_test.cpp`

修改：CMake、IDL 与三份生成文件、README、架构/文件清单/验收文档。

AidLux 现有 `/home/aidlux/mid360_lio_rbt` 另有 7 个源文件由 `integrations/mid360_lio/super_lio_rbt_navigation.patch` 与 `height-datum-fix.patch` 扩展：SuperLIO CMake、PriorMapLocalization 头/实现、LIOWrapper 头/实现、SuperLIO Output 及组件适配。应用前后有 SHA-256 检查和独立备份，原配置保持不变，新能力默认关闭。


### 地图与定位联调补充

- `scripts/user_map_simulation.py`：使用任意用户PGM/YAML执行现有原生IPC仿真并录像。
- `scripts/map_alignment_audit.py`：PCD投影、栅格、YAML外参与低位平面候选审计。
- `integrations/mid360_lio/check_localization.py`：只启动传感器链路的定位验收。
- `integrations/mid360_lio/prior_map_acceptance/{CMakeLists.txt,prior_map_acceptance.cpp,README.md}`：独立KISS构建与已知变换初始化测试。
- `docs/map-readiness.md`：本次结果与上电命令。

修改 `scripts/complex_simulation.py` 以接收每场景余量/速度上限，保留原默认值；`tests/lio_source_probe.cpp` 增加只读localized模式，CMake链接现有LIO投影库。原始frames/none检查保留。A*、ESDF、RPP/DWPP、Manager、Guard和真实底盘源码未改。

### robot-sim 安装参考补充

- `scripts/robot_mount_audit.py`：核对模型固定关节链、实际雷达传感器pose与LIO有效外参。
- `tests/test_robot_mount_audit.py`：旧参数偏差、新参数匹配及旋转/模型冲突反例。
- `docs/robot-mount.md`：模型来源、坐标约定、内部标定换算与验收范围。

修改README、地图联调/验收文档和本清单；活动 `output/navigation-lio/super_lio-localization.yaml` 仅换算 `odom_robo`，原值、模型快照、哈希和审计输出留存在 `output/navigation-lio/readiness/robot-mount/`。算法、协议、原生生产者和底盘源码未改。

### 前驱差速 State Lattice 实现（2026-10-28）

本节列出前驱 State Lattice 的实际新增和修改文件。SHA-256 与字节数快照见 [source-manifest.json](../../output/navigation-lattice/verification/source-manifest.json)；其中不包含文档散列，避免文档验收更新造成代码快照变化。实现说明见 [state-lattice-design.md](state-lattice-design.md)，Nav2 代码边界见 [lattice-port.md](lattice-port.md)。本清单不记录测试通过结论。

#### 新增实现、配置和许可文件

| 类型 | 仓库相对路径 |
| --- | --- |
| 头文件 | [navigation/include/rbt_navigation/controller/feasible_path_handler.hpp](../include/rbt_navigation/controller/feasible_path_handler.hpp) |
| 头文件 | [navigation/include/rbt_navigation/geometry/footprint.hpp](../include/rbt_navigation/geometry/footprint.hpp) |
| 头文件 | [navigation/include/rbt_navigation/geometry/geometry_io.hpp](../include/rbt_navigation/geometry/geometry_io.hpp) |
| 头文件 | [navigation/include/rbt_navigation/planner/state_lattice.hpp](../include/rbt_navigation/planner/state_lattice.hpp) |
| 源文件 | [navigation/src/feasible_path_handler.cpp](../src/feasible_path_handler.cpp) |
| 源文件 | [navigation/src/footprint.cpp](../src/footprint.cpp) |
| 源文件 | [navigation/src/geometry_io.cpp](../src/geometry_io.cpp) |
| 源文件 | [navigation/src/lattice_io.cpp](../src/lattice_io.cpp) |
| 源文件 | [navigation/src/state_lattice.cpp](../src/state_lattice.cpp) |
| 配置 | [navigation/config/front-drive-geometry.yaml](../config/front-drive-geometry.yaml) |
| 配置 | [navigation/config/front-drive-navigation.conf](../config/front-drive-navigation.conf) |
| 运动原语 | [navigation/config/lattice/diff-5cm-0_5m.json](../config/lattice/diff-5cm-0_5m.json) |
| 许可记录 | [navigation/licenses/nav2-lattice/LICENSE](../licenses/nav2-lattice/LICENSE) |
| 许可文本 | [navigation/licenses/nav2-lattice/LICENSE-2.0.txt](../licenses/nav2-lattice/LICENSE-2.0.txt) |
| 来源与散列记录 | [navigation/licenses/nav2-lattice/README.md](../licenses/nav2-lattice/README.md) |

#### 新增验证工具

| 类型 | 仓库相对路径 |
| --- | --- |
| 单元测试 | [navigation/tests/footprint_test.cpp](../tests/footprint_test.cpp) |
| 单元测试 | [navigation/tests/geometry_io_test.cpp](../tests/geometry_io_test.cpp) |
| 单元测试 | [navigation/tests/lattice_tracking_test.cpp](../tests/lattice_tracking_test.cpp) |
| 单元测试 | [navigation/tests/state_lattice_test.cpp](../tests/state_lattice_test.cpp) |
| IPC fixture | [navigation/tests/lattice_native_fixture.cpp](../tests/lattice_native_fixture.cpp) |
| IPC runner | [navigation/scripts/lattice_integration.py](../scripts/lattice_integration.py) |
| 非生产渲染器 | [navigation/scripts/render_lattice.py](../scripts/render_lattice.py)：只影响输出图像布局，不参与规划、控制或运行时构建 |

#### 修改的既有实现文件

| 变更范围 | 仓库相对路径 |
| --- | --- |
| 构建与安装 | `navigation/CMakeLists.txt` |
| CMake 配置导出 | `navigation/cmake/rbt_navigationConfig.cmake.in` |
| 核心上下文 | `navigation/include/rbt_navigation/core/types.hpp` |
| 地图原点访问 | `navigation/include/rbt_navigation/map/grid_map.hpp` |
| 路径跟踪接口 | `navigation/include/rbt_navigation/controller/pure_pursuit.hpp` |
| RPP 几何与动作跟踪 | `navigation/src/pure_pursuit.cpp` |
| Manager 几何传递 | `navigation/include/rbt_navigation/runtime/navigation_manager.hpp` |
| Manager 实现 | `navigation/src/navigation_manager.cpp` |
| Runtime 参数读取 | `navigation/src/native.cpp` |
| Runtime 坐标适配 | `navigation/src/runtime_main.cpp` |
| 原生 IPC 集成验收 | `navigation/scripts/native_integration.py` |

#### 文档路径

文档不进入源代码散列快照；验收文档由根工作区更新。

- `navigation/README.md`
- `navigation/NOTICE.md`
- `navigation/docs/architecture.md`
- `navigation/docs/base-controller-contract.md`
- `navigation/docs/files.md`
- `navigation/docs/lattice-port.md`
- `navigation/docs/lattice-verification.md`
- `navigation/docs/nav2-port.md`
- `navigation/docs/state-lattice-design.md`
- `navigation/docs/verification.md`

本次清单不包含 Dock 相关文件或无关 driver 包；导航 IDL、生成消息布局和真实底盘驱动均未改。

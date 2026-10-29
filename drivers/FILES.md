# 原生驱动修改文件清单

本次新增三个独立 CMake 工程和一个测试工程；没有修改 Downloads 中的参考压缩包。DDSM 原核心与原测试的来源见 [NOTICE](base_controller/NOTICE.md)。导航本体的完整清单仍见 [navigation/docs/files.md](../navigation/docs/files.md)。

## 共享硬件消息与 SDK 适配（8 个文件）

- [hardware_messages/CMakeLists.txt](hardware_messages/CMakeLists.txt)
- [hardware_messages/README.md](hardware_messages/README.md)
- [hardware_messages/cmake/rbt_hardwareConfig.cmake.in](hardware_messages/cmake/rbt_hardwareConfig.cmake.in)
- [hardware_messages/generated/cpp/rbt/hardware.hpp](hardware_messages/generated/cpp/rbt/hardware.hpp)
- [hardware_messages/generated/descriptors/hardware.json](hardware_messages/generated/descriptors/hardware.json)
- [hardware_messages/generated/rust/hardware.rs](hardware_messages/generated/rust/hardware.rs)
- [hardware_messages/include/rbt_hardware/native.hpp](hardware_messages/include/rbt_hardware/native.hpp)
- [hardware_messages/types/hardware.rbt](hardware_messages/types/hardware.rbt)

## 独立 DDSM 底盘驱动（37 个文件）

- [base_controller/CMakeLists.txt](base_controller/CMakeLists.txt)
- [base_controller/NOTICE.md](base_controller/NOTICE.md)
- [base_controller/README.md](base_controller/README.md)
- [base_controller/config/ddsm_robot_current.yaml](base_controller/config/ddsm_robot_current.yaml)
- [base_controller/include/ddsm_driver/.gitkeep](base_controller/include/ddsm_driver/.gitkeep)
- [base_controller/include/ddsm_driver/ddsm_control.hpp](base_controller/include/ddsm_driver/ddsm_control.hpp)
- [base_controller/include/ddsm_driver/ddsm_feedback_buffer.hpp](base_controller/include/ddsm_driver/ddsm_feedback_buffer.hpp)
- [base_controller/include/ddsm_driver/ddsm_motor.hpp](base_controller/include/ddsm_driver/ddsm_motor.hpp)
- [base_controller/include/ddsm_driver/ddsm_protocol.hpp](base_controller/include/ddsm_driver/ddsm_protocol.hpp)
- [base_controller/include/ddsm_driver/diff_drive_kinematics.hpp](base_controller/include/ddsm_driver/diff_drive_kinematics.hpp)
- [base_controller/include/ddsm_driver/native_config.hpp](base_controller/include/ddsm_driver/native_config.hpp)
- [base_controller/include/ddsm_driver/native_safety.hpp](base_controller/include/ddsm_driver/native_safety.hpp)
- [base_controller/include/ddsm_driver/odometry.hpp](base_controller/include/ddsm_driver/odometry.hpp)
- [base_controller/include/ddsm_driver/pid_controller.hpp](base_controller/include/ddsm_driver/pid_controller.hpp)
- [base_controller/include/ddsm_driver/serial_comm.hpp](base_controller/include/ddsm_driver/serial_comm.hpp)
- [base_controller/include/ddsm_driver/yaw_auto_tuner.hpp](base_controller/include/ddsm_driver/yaw_auto_tuner.hpp)
- [base_controller/provenance/upstream-package.xml](base_controller/provenance/upstream-package.xml)
- [base_controller/src/ddsm_control.cpp](base_controller/src/ddsm_control.cpp)
- [base_controller/src/ddsm_feedback_buffer.cpp](base_controller/src/ddsm_feedback_buffer.cpp)
- [base_controller/src/ddsm_motor.cpp](base_controller/src/ddsm_motor.cpp)
- [base_controller/src/ddsm_protocol.cpp](base_controller/src/ddsm_protocol.cpp)
- [base_controller/src/diff_drive_kinematics.cpp](base_controller/src/diff_drive_kinematics.cpp)
- [base_controller/src/native_config.cpp](base_controller/src/native_config.cpp)
- [base_controller/src/native_main.cpp](base_controller/src/native_main.cpp)
- [base_controller/src/odometry.cpp](base_controller/src/odometry.cpp)
- [base_controller/src/pid_controller.cpp](base_controller/src/pid_controller.cpp)
- [base_controller/src/serial_comm.cpp](base_controller/src/serial_comm.cpp)
- [base_controller/src/yaw_auto_tuner.cpp](base_controller/src/yaw_auto_tuner.cpp)
- [base_controller/test/test_common.hpp](base_controller/test/test_common.hpp)
- [base_controller/test/test_control_planner.cpp](base_controller/test/test_control_planner.cpp)
- [base_controller/test/test_ddsm_protocol.cpp](base_controller/test/test_ddsm_protocol.cpp)
- [base_controller/test/test_diff_drive_kinematics.cpp](base_controller/test/test_diff_drive_kinematics.cpp)
- [base_controller/test/test_feedback_buffer.cpp](base_controller/test/test_feedback_buffer.cpp)
- [base_controller/test/test_native_safety.cpp](base_controller/test/test_native_safety.cpp)
- [base_controller/test/test_pid_controller.cpp](base_controller/test/test_pid_controller.cpp)
- [base_controller/test/test_serial_transport.cpp](base_controller/test/test_serial_transport.cpp)
- [base_controller/test/test_yaw_auto_tuner.cpp](base_controller/test/test_yaw_auto_tuner.cpp)

## 独立 DM IMU 驱动（15 个文件）

- [dm_imu/CMakeLists.txt](dm_imu/CMakeLists.txt)
- [dm_imu/DEPLOYMENT.md](dm_imu/DEPLOYMENT.md)
- [dm_imu/NOTICE.md](dm_imu/NOTICE.md)
- [dm_imu/README.md](dm_imu/README.md)
- [dm_imu/config/dm_imu.yaml](dm_imu/config/dm_imu.yaml)
- [dm_imu/include/rbt_dm_imu/config.hpp](dm_imu/include/rbt_dm_imu/config.hpp)
- [dm_imu/include/rbt_dm_imu/parser.hpp](dm_imu/include/rbt_dm_imu/parser.hpp)
- [dm_imu/include/rbt_dm_imu/serial_port.hpp](dm_imu/include/rbt_dm_imu/serial_port.hpp)
- [dm_imu/src/config.cpp](dm_imu/src/config.cpp)
- [dm_imu/src/main.cpp](dm_imu/src/main.cpp)
- [dm_imu/src/parser.cpp](dm_imu/src/parser.cpp)
- [dm_imu/src/probe.cpp](dm_imu/src/probe.cpp)
- [dm_imu/src/serial_port.cpp](dm_imu/src/serial_port.cpp)
- [dm_imu/tests/config_test.cpp](dm_imu/tests/config_test.cpp)
- [dm_imu/tests/parser_test.cpp](dm_imu/tests/parser_test.cpp)

## 跨进程验收夹具（3 个文件）

- [tests/CMakeLists.txt](tests/CMakeLists.txt)
- [tests/hardware_ipc_fixture.cpp](tests/hardware_ipc_fixture.cpp)
- [tests/hardware_ipc_test.py](tests/hardware_ipc_test.py)

## 工作区说明与验收记录

- [README.md](README.md)：架构图、构建、运行与测试入口。
- [VERIFICATION.md](VERIFICATION.md)：真实执行结果、证据与未验证范围。
- [FILES.md](FILES.md)：本清单。

本次同时更新以下导航文档，说明现有驱动、消息契约、外部 TF 归属和当前验收状态：

- [navigation/README.md](../navigation/README.md)
- [navigation/docs/architecture.md](../navigation/docs/architecture.md)
- [navigation/docs/base-controller-contract.md](../navigation/docs/base-controller-contract.md)
- [navigation/docs/hardware-sources.md](../navigation/docs/hardware-sources.md)
- [navigation/docs/files.md](../navigation/docs/files.md)
- [navigation/docs/verification.md](../navigation/docs/verification.md)

本次驱动移植没有修改导航算法、Manager、已有消息布局或原测试；保留审计见 [验收记录](VERIFICATION.md)。

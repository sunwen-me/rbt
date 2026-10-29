# Provenance and notices

The standalone driver reuses the ROS-free DDSM implementation from `target/hardware-ros2-reference/ddsm_driver/ddsm_driver`. Its package manifest is preserved byte-for-byte at [provenance/upstream-package.xml](provenance/upstream-package.xml); that manifest identifies the upstream package license as MIT.

The following upstream files are copied without modification:

- `include/ddsm_driver/`: `ddsm_control.hpp`, `ddsm_feedback_buffer.hpp`, `ddsm_motor.hpp`, `ddsm_protocol.hpp`, `diff_drive_kinematics.hpp`, `odometry.hpp`, `pid_controller.hpp`, `serial_comm.hpp`, and `yaw_auto_tuner.hpp`.
- `src/`: `ddsm_control.cpp`, `ddsm_feedback_buffer.cpp`, `ddsm_motor.cpp`, `ddsm_protocol.cpp`, `diff_drive_kinematics.cpp`, `odometry.cpp`, `pid_controller.cpp`, `serial_comm.cpp`, and `yaw_auto_tuner.cpp`.
- `test/`: all seven original `test_*.cpp` files and `test_common.hpp`.
- `config/ddsm_robot_current.yaml`, copied byte-for-byte.

`src/native_main.cpp`, `src/native_config.cpp`, `include/ddsm_driver/native_config.hpp`, `include/ddsm_driver/native_safety.hpp`, `test/test_native_safety.cpp`, this notice, and the standalone CMake and README files are new native-port files. The ROS node source and ROS launch files are not included.

# RBT native DDSM base controller

`rbt_base_controller` is a standalone C++ driver for the DDSM differential-drive base. It reads `rbt.navwire/VelocityCommand` from `/cmd_vel`, reads `rbt.navwire/EmergencyStop` from `PREFIX/estop`, accepts `rbt.hardware/Imu` from `/imu/data`, and publishes wheel telemetry under the configured prefix. It has no ROS runtime dependency.

The default configuration is the byte-exact `config/ddsm_robot_current.yaml` from the ROS reference package. The driver reads its original `ddsm_driver_node.ros__parameters` mapping. `--config FILE` selects another file with the same structure.

```sh
rbt_base_controller \
  --config drivers/base_controller/config/ddsm_robot_current.yaml \
  --serial-port /dev/ddsm \
  --prefix rbt/navigation/demo \
  --cmd-vel-topic /cmd_vel
```

The executable also accepts `--imu-topic`, `--estop-topic`, `--command-source` (alias `--guard-source`), `--estop-source`, `--max-linear-velocity`, `--max-angular-velocity`, and `--duration`. Defaults are command source 100, estop source 2, maximum linear speed 0.7 m/s, maximum angular speed 1.8 rad/s, and no duration limit. The command topic defaults to `/cmd_vel`, the IMU topic to `/imu/data`, and estop to `PREFIX/estop`.

The controller accepts only finite commands with a matching HostMonotonic clock identity, source 100, a new nonzero sequence, a stamp no older than 600 ms, and a positive TTL no longer than 250 ms. The independent serial thread checks the command deadline and 600 ms watchdog before every motor exchange. It sends zero after estop, command expiry, stale wheel feedback, or an I/O fault. Estop is latched until a fresh inactive message from source 2 arrives; that release clears the command, so a new velocity command is required to move again.

Only the I/O thread opens, configures, exchanges with, and stops the serial transport. The 100 Hz control period is a target; the original transport can wait up to 25 ms for a response per exchange, so the physical exchange rate can be lower. Odometry integrates only consecutive healthy feedback intervals within `feedback_timeout`; after an invalid sample or a longer gap it retains pose and resumes integration on the next pair. Telemetry stamps are HostMonotonic receive stamps and are emitted only for newly validated wheel feedback. Wheel and status sequence counters start from current HostClock monotonic time so they continue increasing across a driver restart. The wheel pose origin resets to zero when the driver restarts; consumers must treat that as an odometry reset. The yaw PID reads `angular_velocity_z` directly from the configured IMU topic and requires its z axis to align with the base frame, or an upstream transformed IMU stream; this driver does not apply a mounting transform. The process does not publish TF or start yaw auto-tuning. `use_imu_orientation: true` is rejected because this port does not implement fresh, reset-aware orientation input.

Telemetry endpoints are `PREFIX/wheel_odom` (`rbt.hardware/WheelOdometry`), `PREFIX/odom` (`rbt.navwire/Odom2D`), and `PREFIX/base_status` (`rbt.hardware/BaseStatus`). No hardware device was exercised as part of the software verification.

Build and run software tests with the target toolchain and installed SDK:

```sh
cmake -S drivers/base_controller -B target/base-controller-build \
  -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$PWD/target/phase4-toolchain/bin/ninja" \
  -DCMAKE_CXX_COMPILER="$PWD/target/phase4-toolchain/bin/x86_64-conda-linux-gnu-g++" \
  -DCMAKE_PREFIX_PATH="$PWD/target/robot-sdk;$PWD/target/release-build-distribution-20261020/sdk-root"
cmake --build target/base-controller-build
ctest --test-dir target/base-controller-build --output-on-failure
```

See [NOTICE.md](NOTICE.md) for the copied core and test provenance.

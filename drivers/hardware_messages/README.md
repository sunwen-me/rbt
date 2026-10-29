# RBT hardware telemetry contracts

`rbt_hardware::messages` exports generated native RBT bindings and small adapters
over the existing `FixedPublisher`, `FixedSubscriber`, and `HostClock` APIs.
It contains no navigation algorithms, motor control, TF publisher or ROS runtime.

`types/hardware.rbt` is the ABI source. Generate bindings with the repository IDL
compiler; do not edit generated layouts or descriptors:

```bash
target/debug/rbt-idl generate drivers/hardware_messages/types/hardware.rbt \
  --cpp drivers/hardware_messages/generated/cpp \
  --rust drivers/hardware_messages/generated/rust \
  --descriptor drivers/hardware_messages/generated/descriptors
```

| Message | Publisher | Meaning |
|---|---|---|
| `Imu` | `rbt_dm_imu`, default `/imu/data` | Sensor-frame angular velocity in rad/s, acceleration in m/s², normalized quaternion x/y/z/w; covariance semantics follow ROS 2 Imu |
| `WheelOdometry` | `rbt_base_controller`, `prefix/wheel_odom` | New healthy wheel-feedback pair, wheel RPM, integrated planar pose and body speed, default `odom` / `base_footprint` |
| `BaseStatus` | `rbt_base_controller`, `prefix/base_status` | Current command, rejection/failure counters, watchdog, estop, I/O fault and feedback freshness diagnostics |

Every stamp carries the complete 48-byte HostMonotonic identity, generation,
kind (`1` for HostMonotonic, as in navigation Stamp) and nanoseconds. This wire
value is independent of the SDK's C++ ClockKind enum. IMU and wheel telemetry use **host receive time**, after
validating the received frame/pair; device capture time is not available in the
supplied protocols. No fresh measurement is implied by periodic diagnostics.
Stale or invalid feedback must not be republished with a new measurement stamp.
The IMU frame defaults to `imu_link`; mounting extrinsics and all TF publication
belong to external components. A fixed transform between `base_footprint` and
`base_link` must preserve the navigation velocity-frame convention.

Orientation covariance is in rad², gyro covariance in (rad/s)² and acceleration
covariance in (m/s²)². All-zero covariance means unknown; a missing estimate uses
-1 in its first covariance element. Unit and covariance conventions are from
the [ROS 2 Imu definition](https://github.com/ros2/common_interfaces/blob/rolling/sensor_msgs/msg/Imu.msg).
Choosing these output units does not establish the physical device's wire units;
the driver records its configured input-unit assumption separately.

The base consumes the existing `rbt.navwire::VelocityCommand` and
`EmergencyStop` contracts from `rbt_navigation::messages`, and publishes the
existing `Odom2D` for navigation. These layouts are reused without alteration.
`BaseStatus` is diagnostic and never replaces independent command expiry or
hardware safety.

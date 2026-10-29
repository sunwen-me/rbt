# MID360 Super-LIO navigation output

This integration adds an opt-in `LioNavigationFrameV1` capability to the
existing Super-LIO component. It publishes the generated
`rbt.navwire/LioNavigationFrame@1` header and its matching XYZI scan as one
slice. Existing odometry, world cloud, map, and path outputs keep their current
types and publication path.

The normalized frame uses the configured odom and base frame names and keeps
the world scan points in odom coordinates. Its pose, twist, sensor origin, and
scan come from the same Super-LIO process callback. The stamp is the source
receipt epoch converted to this process's HostMonotonic clock; it carries the
clock source ID, generation, nanoseconds, and mapper uncertainty. Only MID360
time mode 0 is supported. A timestamp outside the activation's observed epoch,
an invalid clock mapping, or a mapper sampling gap suppresses the normalized
frame. Linear and angular velocity are expressed in the emitted base frame;
linear velocity includes the IMU-to-base lever-arm term:
`v_base = R_base^T (v_imu + R_imu (omega_imu x r_imu_base))`, with
`r_imu_base = -R_odom_robo t_odom_robo`. `R_base` is the same full or planar
rotation written to the frame. Angular velocity is rotated into that emitted
base frame. The receipt mapper is sampled before input is released and every
10 ms by an activation-owned critical task, independent of the estimate loop.
If that task is stopped or starved long enough to exceed the mapper's 200 ms
sample-gap limit, it latches until component reactivation.

For this new frame only, planar mode yaw-levels the base orientation while
preserving the physical base-origin height computed from the IMU pose and
`lio.extrinsic.odom_robo`. The legacy odometry output retains its existing
`lio.ground_robot.base_z` clamp. Keeping the normalized frame base origin at
the same vertical datum as its odom-frame scan and sensor origin is required
for height filtering and obstacle projection.

The `map_odom_valid` flag is set only while the prior-map localization module
reports a fresh accepted registration. Without that state, the producer may
publish its odom pose and scan with the flag clear and zero map-to-odom fields;
the navigation bridge rejects those frames. No identity map transform is
substituted. Accepted registration freshness is two seconds; backend map
refinement does not refresh it, and a LOST transition clears it immediately.

## Enablement

The feature is disabled by default. Add this YAML setting to opt in:

```yaml
rbt.navigation_output: true
```

Keep these existing algorithm settings enabled:

```yaml
lio.output.map: true
lio.output.dense: true
lio.output.pub_step: 1
lio.output.base_pose: true
lio.ground_robot.planar_constraint: true
lio.frames.map: map
lio.frames.odom: odom
lio.frames.base: base_footprint # base_link is also supported
lio.sensor.time_offset: 0.0
```

The component rejects an enabled configuration that does not satisfy those
settings. Configure the Super-LIO workspace with the navigation source tree:

```sh
source scripts/env.sh
cmake -S . -B build-navigation \
  -DLIO_WITH_RBT_NAVIGATION=ON \
  -DRBT_NAVIGATION_SOURCE_DIR=/path/to/rbt/navigation
cmake --build build-navigation --parallel 2
```

`RBT_NAVIGATION_SOURCE_DIR` must point at the repository's `navigation`
directory, containing both `include/rbt_navigation` and `generated/cpp/rbt`.
This adds only header and generated-message include paths; Super-LIO does not
link the navigation algorithms.

## Apply the source patch

The patch targets the existing Super-LIO workspace layout. Run the script with
that workspace root (the directory containing `src/super_lio`):

```sh
bash /path/to/rbt/navigation/integrations/mid360_lio/apply.sh /path/to/mid360-lio
```

The script verifies SHA-256 for every touched source file, creates a
timestamped backup under `.rbt-navigation-backups/`, applies the source
integration and base-height patches, and verifies the resulting hashes. It
accepts either the untouched snapshot or the earlier navigation-patched
state, so it can upgrade an existing installation safely. It exits without
changes when the final patch state is already present and refuses unexpected
source changes.

The reusable frame publisher and header helpers are in
`navigation/include/rbt_navigation/runtime/lio_output.hpp`. The navigation
contract fixture in `navigation/tests/lio_output_test.cpp` checks wire-stamp
round-trip, point/pose projection, and fail-closed localization flags.

## Native producer acceptance

The standalone `source_integration.py` runs only synthetic localhost UDP. It
starts the actual MID360 driver and SuperLIO in private Composer sessions and
observes the new capability with `rbt_navigation_lio_source_probe`. In mode 0,
the runner configures a neutral synthetic base translation of 0.735 m and the
probe verifies `sensor_origin.z - odom_base_position.z` is 0.735 m within
0.02 m, in addition to identified, fresh, unlocalized frames. This checks that
the normalized pose preserves the physical height datum when planar
orientation is enabled. It also requires a valid frame within 250 ms before
the observation window ends, recorded as `ending_valid_frame_age_ns`, so an
initial burst followed by a stopped producer cannot pass. Uncalibrated
device mode 1 must produce no frames
while legacy LIO odometry remains active. Preserve the generated JSON, YAML
and per-process logs as evidence. The runner does not start real hardware.

On AidLux, source the existing `scripts/env.sh` before building/testing so the
pre-existing standalone GTSAM and METIS libraries can be found. A fresh build
may need `-DGTSAM_DIR=/opt/ros/humble/lib/cmake/GTSAM`; this is the location of
the installed third-party library, not a ROS runtime dependency.

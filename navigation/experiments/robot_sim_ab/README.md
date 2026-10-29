# Existing robot-sim controller comparison

This optional experiment sends the existing pure C++ RBT controller's output to
the existing `/home/sw/robot-sim` Gazebo base controller. ROS interfaces belong
only to this simulator adapter; the navigation core gains no ROS dependency.
It does not exercise NavigationManager, VelocityGuard, LIO, or hardware.

The comparison is **RPP + DWPP** against **Vector Pursuit geometry + the same
dynamic velocity window**. The latter is the repository's geometry port, not
the complete upstream ROS plugin. Paths, map, footprint and controller limits
are identical: 0.2 m/s, 0.5 rad/s, 0.8 m/s² and 2.5 rad/s².

Only three scenarios are used: S bend, narrow turn and forward/reverse cusp.
Each run starts a fresh simulator. The original model, contact parameters,
inertias and controller YAML remain unchanged. World camera/IMU systems and the
world RGL manager are omitted because tracking uses ground truth. Model-level
RGL plugins can still publish; no perception data enters the controller.
Arms remain in the original model but are excluded from the planar footprint.
Obstacle boxes are 0.2 m high and exactly cover occupied grid cells.

## Prepare locally

Run from `/home/sw/rbt`, after copying the existing simulator's `empty.sdf`:

```bash
python navigation/experiments/robot_sim_ab/prepare.py \
  --world-template output/navigation-robot-sim-ab/reference/empty.sdf \
  --output output/navigation-robot-sim-ab
```

Copy `fixtures/`, `worlds/`, `manifest.json`, `run.py` and `run_all.sh` into a
dedicated remote task directory. Put the existing
`rbt_navigation_pursuit_physics_bridge` executable under `bin/` and its runtime
libraries under `lib/`. The runner prepends that library directory only for the
C++ child process, preserving the simulator's library environment.

## Run on the robot-sim host

```bash
env -i HOME=/home/sw USER=sw PATH=/usr/bin:/bin \
  bash /home/sw/robot-sim/.codex-nav-ab-20261028/run_all.sh
```

The script uses ROS domains 87–92 and a dedicated Gazebo partition per run. It
publishes `geometry_msgs/msg/TwistStamped` on `/base_controller/cmd_vel` and
records wheel odometry from `/base_controller/odom`. Ground truth comes from
Gazebo `/world/default/dynamic_pose/info`; its timestamps and ROS `/clock` use
the same simulation clock. Ground-truth pose differences over approximately
0.1 s provide measured body velocity. Commands run nominally at 50 Hz.

Each run records two seconds of initial zero command and one second of zero
command after reaching, controller error or 100 s simulation timeout. Original
controller success criteria and safety checks are retained. A controller error
is a recorded experiment outcome, not a runner failure. Startup/transport
failures are recorded separately and are not valid algorithm comparisons.
Optional positional arguments select one scenario and controller, for example
`run_all.sh S_curve rpp-dwpp` to repeat a failed startup. Existing output for that
selection must be archived before rerunning. Earlier exploratory runs used
domain 87 for every run; separate domains avoid stale service discovery between
restarts.

## Render locally

Copy each `<scenario>/<controller>/` directory back beside `fixtures/`, then:

```bash
target/navigation-video-tools/bin/python \
  navigation/experiments/robot_sim_ab/render.py \
  --input output/navigation-robot-sim-ab \
  --output output/navigation-robot-sim-ab/media
```

The MP4 is a **Gazebo telemetry replay**, not a recording of the Gazebo camera.
Both panes show the same simulation time; playback is compressed. Planar
footprint overlap against occupied cells is checked along the recorded trace
with interpolation steps at most 0.01 m / 0.02 rad. This is a geometry check,
not a physical contact sensor. Clearance is evaluated against exact merged
occupied rectangles. Tracking errors use moving samples at nonnegative run
times; a single run per controller and scene is an initial comparison rather
than a statistical ranking or hardware validation.

## VP heading source comparison

The C++ configuration `vector_pursuit_use_heading_from_path` defaults to `true`.
The experiment option `--vp-use-heading-from-path 1` uses interpolated path pose
yaw; `0` derives yaw from the selected nonzero XY segment's tangent. This follows
the heading-source meaning of upstream VP's `use_heading_from_path` option.
Both modes retain the existing arc-length lookahead implementation. The option
changes only the VP target orientation: original path poses, reverse/cusp
handling and final goal yaw remain unchanged. It does not remove the heading
term from VP's formula. Pure Pursuit ignores this VP-specific option.

For the three-scene comparison, copy `run_heading_ablation.sh` beside `run.py`,
the rebuilt helper, fixtures and worlds in a fresh remote task directory. Run
that script instead of `run_all.sh`; it uses ROS domains 101–106 and separate
Gazebo partitions. Results are stored as `<scenario>/heading-on/` and
`<scenario>/heading-off/`. Render them with `render_heading_ablation.py` using
the same `--input` / `--output` arguments as the original renderer. The original
RPP/VP experiment artifacts are retained separately.

#!/usr/bin/env bash
# Run only in an isolated task directory on the existing robot-sim machine.
set -eo pipefail
task_dir=$(cd "$(dirname "$0")" && pwd)
export ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST
source /opt/ros/lyrical/setup.bash
source /home/sw/robot-sim/install/local_setup.bash
set -u
mkdir -p "$task_dir/log"
launch_pid=
cleanup() {
  if [[ -n "$launch_pid" ]]; then
    kill -INT -- "-$launch_pid" 2>/dev/null || true
    for attempt in {1..100}; do
      kill -0 "$launch_pid" 2>/dev/null || break
      sleep 0.1
    done
    kill -TERM -- "-$launch_pid" 2>/dev/null || true
    wait "$launch_pid" 2>/dev/null || true
    launch_pid=
  fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM
runner_failed=0
run_index=0
for scenario in S_curve narrow_turn reverse_cusp; do
  case "$scenario" in
    S_curve) start_x=3.0; start_y=3.0; reverse_args=() ;;
    narrow_turn) start_x=4.0; start_y=4.0; reverse_args=() ;;
    reverse_cusp) start_x=4.0; start_y=4.0; reverse_args=(--allow-reverse) ;;
  esac
  for controller in rpp-dwpp vector-pursuit-window; do
    run_index=$((run_index + 1))
    [[ -z "${1:-}" || "$scenario" == "$1" ]] || continue
    [[ -z "${2:-}" || "$controller" == "$2" ]] || continue
    export ROS_DOMAIN_ID=$((86 + run_index))
    export GZ_PARTITION="codex_nav_ab_20261028_${scenario}_${controller}"
    echo "Starting $scenario / $controller"
    setsid ros2 launch robot_gazebo_sim sim.launch.py \
      controllers_file:=/home/sw/robot-sim/src/robot_description/config/ros2_controllers.yaml \
      world:="$task_dir/worlds/$scenario.sdf" headless:=true arm_controllers:=false \
      x:="$start_x" y:="$start_y" z:=0.02 yaw:=0.0 \
      > "$task_dir/log/${scenario}__${controller}.log" 2>&1 < /dev/null &
    launch_pid=$!
    if ! python3 "$task_dir/run.py" \
      --bin "$task_dir/bin/rbt_navigation_pursuit_physics_bridge" \
      --helper-lib-dir "$task_dir/lib" --controller "$controller" \
      --path "$task_dir/fixtures/$scenario/path.csv" \
      --map "$task_dir/fixtures/$scenario/map.csv" \
      --geometry "$task_dir/fixtures/front-drive-geometry.yaml" \
      --output "$task_dir/$scenario/$controller" \
      --timeout 100 --ready-timeout 60 "${reverse_args[@]}"; then
      runner_failed=1
    fi
    cleanup
  done
done
exit "$runner_failed"

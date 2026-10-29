#!/usr/bin/env bash
set -euo pipefail

if (($# == 0)); then
    printf 'Usage: %s COMMAND [ARG ...]\n' "${0##*/}" >&2
    exit 2
fi

readonly script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly repo_root="$(cd -- "${script_dir}/../../.." && pwd)"
deps_prefix="${RBT_M3T_DEPS_PREFIX:-${repo_root}/target/m3t-deps-prefix}"
ros2_deps_root="${RBT_M3T_ROS2_DEPS_ROOT:-${RBT_M3T_OPENCV_PREFIX:-${repo_root}/target/orbbec-perf-20261020/ros2-deps/usr}}"
if [[ "$ros2_deps_root" == */usr ]]; then
    ros2_deps_root="${ros2_deps_root%/usr}"
fi

readonly qt_plugins="${deps_prefix}/usr/lib/x86_64-linux-gnu/qt5/plugins"
export LD_LIBRARY_PATH="${deps_prefix}/usr/lib/x86_64-linux-gnu:${ros2_deps_root}/usr/lib/x86_64-linux-gnu:${ros2_deps_root}/usr/lib/x86_64-linux-gnu/openblas-pthread:${ros2_deps_root}/usr/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export QT_PLUGIN_PATH="${qt_plugins}${QT_PLUGIN_PATH:+:${QT_PLUGIN_PATH}}"
export QT_QPA_PLATFORM_PLUGIN_PATH="${QT_QPA_PLATFORM_PLUGIN_PATH:-${qt_plugins}/platforms}"

exec "$@"

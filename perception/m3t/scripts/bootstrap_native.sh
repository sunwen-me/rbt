#!/usr/bin/env bash
set -euo pipefail

# Prepare the unmodified upstream checkout used by the native M3T target.
# The checkout is pinned so the explicit source list in UpstreamM3T.cmake
# remains reviewable and reproducible.
readonly upstream_commit="f02106186a1e756450226b3b793e309506817c58"
readonly upstream_url="https://github.com/DLR-RM/3DObjectTracking.git"
readonly repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
upstream_dir="${RBT_M3T_UPSTREAM_ROOT:-${repo_root}/target/m3t-upstream}"

fail() {
    printf 'bootstrap_native.sh: %s\n' "$*" >&2
    exit 1
}

verify_checkout() {
    local checkout_dir="$1"
    local actual_commit
    local status

    git -C "$checkout_dir" rev-parse --is-inside-work-tree >/dev/null 2>&1 \
        || fail "existing path is not a Git checkout: ${checkout_dir}"
    status="$(git -C "$checkout_dir" status --porcelain --untracked-files=all)"
    [[ -z "$status" ]] \
        || fail "refusing to use dirty checkout; preserve local changes in ${checkout_dir}"
    actual_commit="$(git -C "$checkout_dir" rev-parse HEAD)"
    [[ "$actual_commit" == "$upstream_commit" ]] \
        || fail "${checkout_dir} is at ${actual_commit}; expected ${upstream_commit}; leaving it untouched"
    [[ -f "${checkout_dir}/LICENSE" ]] \
        || fail "pinned checkout is missing LICENSE: ${checkout_dir}"
    [[ -f "${checkout_dir}/M3T/include/m3t/tracker.h" ]] \
        || fail "pinned checkout is missing the M3T source tree: ${checkout_dir}"
}

if [[ -e "$upstream_dir" || -L "$upstream_dir" ]]; then
    verify_checkout "$upstream_dir"
    printf 'Verified M3T checkout at %s (%s)\n' "$upstream_dir" "$upstream_commit"
else
    mkdir -p "$(dirname "$upstream_dir")"
    temporary_dir="$(mktemp -d "$(dirname "$upstream_dir")/.m3t-upstream.XXXXXX")"
    trap 'rm -rf -- "$temporary_dir"' EXIT

    git -C "$temporary_dir" init --quiet
    git -C "$temporary_dir" remote add origin "$upstream_url"
    git -C "$temporary_dir" fetch --quiet --depth 1 origin "$upstream_commit"
    git -C "$temporary_dir" checkout --quiet --detach FETCH_HEAD
    verify_checkout "$temporary_dir"

    # -n prevents a concurrently-created destination from being overwritten.
    mv --no-target-directory --no-clobber "$temporary_dir" "$upstream_dir"
    [[ ! -e "$temporary_dir" ]] \
        || fail "${upstream_dir} appeared during bootstrap; existing data was left untouched"
    trap - EXIT
    printf 'Checked out M3T at %s (%s)\n' "$upstream_dir" "$upstream_commit"
fi

# Optional isolated dependency prefix for CMake. OpenCV is available in the
# local ROS dependency cache; GLFW/GLEW/OpenGL development packages may be
# extracted under target/m3t-deps-prefix without installing them system-wide.
deps_prefix="${RBT_M3T_DEPS_PREFIX:-${repo_root}/target/m3t-deps-prefix}"
opencv_prefix="${RBT_M3T_OPENCV_PREFIX:-${repo_root}/target/orbbec-perf-20261020/ros2-deps}"
printf 'Optional local dependencies: set CMAKE_PREFIX_PATH="%s/usr;%s/usr"\n' \
    "$deps_prefix" "$opencv_prefix"
printf 'For the extracted OpenCV subset, use OpenCV_DIR="%s/usr/lib/x86_64-linux-gnu/cmake/opencv4-minimal" and RBT_M3T_OPENCV_PREFIX="%s/usr"\n' \
    "$deps_prefix" "$opencv_prefix"

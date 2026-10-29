#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
usage: scripts/benchmark_v01.sh [options]

Run the bounded v0.1 publisher/subscriber benchmark matrix. Sensor cases use
Image for 64 B through 6,220,800 B and PointCloud for 20,971,520 B.

options:
  --smoke                 run Image, PointCloud, and Control quick cases
  --count N               measured samples per case (default: 20)
  --warmup N              warmup samples per case (default: 3)
  --timeout-ms N          open/receive/QueueFull timeout (default: 30000)
  --startup-delay-ms N    publisher delay after READY (default: 1000)
  --interval-us N         delay between sends (default: 1000)
  --profile NAME          sensor, control, or all (default: all)
  --debug                 use target/debug/rbt-bench instead of release
  --help                  show this help

The same values may be supplied with RBT_BENCH_COUNT, RBT_BENCH_WARMUP,
RBT_BENCH_TIMEOUT_MS, RBT_BENCH_STARTUP_DELAY_MS, and RBT_BENCH_INTERVAL_US.
RBT_BENCH_BINARY may point at an already-built rbt-bench executable.
EOF
}

require_positive_integer() {
    local name="$1"
    local value="$2"
    [[ "${value}" =~ ^[1-9][0-9]*$ ]] || die "${name} must be a positive integer: ${value}"
}

require_nonnegative_integer() {
    local name="$1"
    local value="$2"
    [[ "${value}" =~ ^[0-9]+$ ]] || die "${name} must be a non-negative integer: ${value}"
}

smoke=0
build_mode="release"
count="${RBT_BENCH_COUNT:-20}"
warmup="${RBT_BENCH_WARMUP:-3}"
timeout_ms="${RBT_BENCH_TIMEOUT_MS:-30000}"
startup_delay_ms="${RBT_BENCH_STARTUP_DELAY_MS:-1000}"
interval_us="${RBT_BENCH_INTERVAL_US:-1000}"
profile_filter="all"

while (($# > 0)); do
    case "$1" in
        --smoke)
            smoke=1
            shift
            ;;
        --debug)
            build_mode="debug"
            shift
            ;;
        --count)
            (($# >= 2)) || die "--count requires a value"
            count="$2"
            shift 2
            ;;
        --warmup)
            (($# >= 2)) || die "--warmup requires a value"
            warmup="$2"
            shift 2
            ;;
        --timeout-ms)
            (($# >= 2)) || die "--timeout-ms requires a value"
            timeout_ms="$2"
            shift 2
            ;;
        --startup-delay-ms)
            (($# >= 2)) || die "--startup-delay-ms requires a value"
            startup_delay_ms="$2"
            shift 2
            ;;
        --interval-us)
            (($# >= 2)) || die "--interval-us requires a value"
            interval_us="$2"
            shift 2
            ;;
        --profile)
            (($# >= 2)) || die "--profile requires a value"
            profile_filter="$2"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1"
            ;;
    esac
done

require_positive_integer count "${count}"
require_nonnegative_integer warmup "${warmup}"
require_positive_integer timeout_ms "${timeout_ms}"
require_nonnegative_integer startup_delay_ms "${startup_delay_ms}"
require_nonnegative_integer interval_us "${interval_us}"
case "${profile_filter}" in
    all|sensor|control) ;;
    *) die "--profile must be sensor, control, or all" ;;
esac

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
if [[ -d "${toolchain_root}/bin" ]]; then
    toolchain_bin="${toolchain_root}/bin"
    export PATH="${toolchain_bin}${PATH:+:${PATH}}"
    if [[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-gcc" ]]; then
        export CC="${toolchain_bin}/x86_64-conda-linux-gnu-gcc"
        export CXX="${toolchain_bin}/x86_64-conda-linux-gnu-g++"
        export AR="${toolchain_bin}/x86_64-conda-linux-gnu-ar"
        export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
        export LD_LIBRARY_PATH="${toolchain_root}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
    fi
fi

command -v cargo >/dev/null 2>&1 || die "cargo is required"
command -v rustc >/dev/null 2>&1 || die "rustc is required"
command -v timeout >/dev/null 2>&1 || die "timeout is required"
command -v grep >/dev/null 2>&1 || die "grep is required"

if [[ -n "${RBT_BENCH_BINARY:-}" ]]; then
    binary="${RBT_BENCH_BINARY}"
else
    if [[ "${build_mode}" == "release" ]]; then
        cargo build --release -p rbt-bench
        binary="${repo_root}/target/release/rbt-bench"
    else
        cargo build -p rbt-bench
        binary="${repo_root}/target/debug/rbt-bench"
    fi
fi
[[ -x "${binary}" ]] || die "rbt-bench executable is missing or not executable: ${binary}"

tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/rbt-bench-v01.XXXXXX")"
run_id="$(basename "${tmp_root}")-$$"
declare -a child_pids=()

cleanup() {
    local pid
    for pid in "${child_pids[@]:-}"; do
        if kill -0 "${pid}" 2>/dev/null; then
            kill -TERM "${pid}" 2>/dev/null || true
        fi
    done
    for pid in "${child_pids[@]:-}"; do
        wait "${pid}" 2>/dev/null || true
    done
    rm -rf -- "${tmp_root}"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

printf 'RBT_BENCH_RUN run_id=%s build_mode=%s binary=%s\n' "${run_id}" "${build_mode}" "${binary}"
printf 'RBT_BENCH_METHOD count=%s warmup=%s timeout_ms=%s startup_delay_ms=%s interval_us=%s profile=%s\n' \
    "${count}" "${warmup}" "${timeout_ms}" "${startup_delay_ms}" "${interval_us}" "${profile_filter}"
printf 'RBT_BENCH_TOOLCHAIN root=%s\n' "${toolchain_root}"
cpu_model="$(awk -F: '/^model name/ { sub(/^[[:space:]]*/, "", $2); print $2; exit }' /proc/cpuinfo 2>/dev/null)"
logical_cpus="$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf 'unknown')"
os_name="$(sed -n 's/^PRETTY_NAME=//p' /etc/os-release 2>/dev/null | tr -d '\"')"
kernel="$(uname -srm 2>/dev/null || printf 'unknown')"
rust_compiler="$(rustc --version)"
if [[ -n "${CXX:-}" && -x "${CXX}" ]]; then
    cpp_compiler="$(${CXX} --version | sed -n '1p')"
elif command -v c++ >/dev/null 2>&1; then
    cpp_compiler="$(c++ --version | sed -n '1p')"
else
    cpp_compiler="unavailable"
fi
printf 'RBT_BENCH_HOST cpu_model=%s logical_cpus=%s os=%s kernel=%s\n' \
    "${cpu_model:-unknown}" "${logical_cpus}" "${os_name:-unknown}" "${kernel}"
printf 'RBT_BENCH_COMPILERS rust=%s cpp=%s\n' "${rust_compiler}" "${cpp_compiler}"

wait_for_ready() {
    local log_file="$1"
    local pid="$2"
    local deadline=$((SECONDS + (timeout_ms + startup_delay_ms + 5000) / 1000 + 1))
    while ((SECONDS < deadline)); do
        if grep -q 'RBT_BENCH_READY' "${log_file}" 2>/dev/null; then
            return 0
        fi
        if ! kill -0 "${pid}" 2>/dev/null; then
            cat "${log_file}" >&2 || true
            die "benchmark publisher exited before READY"
        fi
        sleep 0.02
    done
    cat "${log_file}" >&2 || true
    die "timed out waiting for benchmark publisher READY"
}

wait_for_pair() {
    local publisher_pid="$1"
    local subscriber_pid="$2"
    local publisher_log="$3"
    local subscriber_log="$4"
    local publisher_status=0
    local subscriber_status=0
    wait "${publisher_pid}" || publisher_status=$?
    wait "${subscriber_pid}" || subscriber_status=$?
    # Both jobs have been reaped, so do not retain their PIDs for the final
    # cleanup trap (which could otherwise encounter a reused PID).
    child_pids=()
    cat "${publisher_log}"
    cat "${subscriber_log}"
    if ((publisher_status != 0 || subscriber_status != 0)); then
        die "benchmark pair failed (publisher=${publisher_status}, subscriber=${subscriber_status})"
    fi
}

run_case() {
    local profile="$1"
    local message="$2"
    local payload="$3"
    local label="$4"
    local service="rbt/bench/v01/${run_id}/${label}"
    local publisher_log="${tmp_root}/${label}.publisher.log"
    local subscriber_log="${tmp_root}/${label}.subscriber.log"
    local -a common_args=(
        --service "${service}"
        --profile "${profile}"
        --message "${message}"
        --payload "${payload}"
        --count "${count}"
        --warmup "${warmup}"
        --subscriber-count 1
        --timeout-ms "${timeout_ms}"
        --startup-delay-ms "${startup_delay_ms}"
        --interval-us "${interval_us}"
    )

    printf 'RBT_BENCH_CASE label=%s profile=%s message=%s payload=%s service=%s\n' \
        "${label}" "${profile}" "${message}" "${payload}" "${service}"
    printf '+ %q publisher' "${binary}"
    printf ' %q' "${common_args[@]}"
    printf '\n'

    local case_timeout_seconds=$((timeout_ms / 1000 + startup_delay_ms / 1000 + 10))
    ((case_timeout_seconds < 10)) && case_timeout_seconds=10
    timeout --signal=TERM --kill-after=2s "${case_timeout_seconds}s" \
        "${binary}" publisher "${common_args[@]}" >"${publisher_log}" 2>&1 &
    local publisher_pid=$!
    child_pids+=("${publisher_pid}")
    wait_for_ready "${publisher_log}" "${publisher_pid}"

    printf '+ %q subscriber' "${binary}"
    printf ' %q' "${common_args[@]}"
    printf '\n'
    timeout --signal=TERM --kill-after=2s "${case_timeout_seconds}s" \
        "${binary}" subscriber "${common_args[@]}" >"${subscriber_log}" 2>&1 &
    local subscriber_pid=$!
    child_pids+=("${subscriber_pid}")

    wait_for_pair "${publisher_pid}" "${subscriber_pid}" "${publisher_log}" "${subscriber_log}"
}

if ((smoke)); then
    if [[ "${profile_filter}" == all || "${profile_filter}" == sensor ]]; then
        run_case sensor image 64 sensor-image-64
        run_case sensor pointcloud 16 sensor-pointcloud-16
    fi
    if [[ "${profile_filter}" == all || "${profile_filter}" == control ]]; then
        run_case control control-command 16 control-16
    fi
else
    if [[ "${profile_filter}" == all || "${profile_filter}" == sensor ]]; then
        run_case sensor image 64 sensor-image-64
        run_case sensor image 1024 sensor-image-1024
        run_case sensor image 1048576 sensor-image-1048576
        run_case sensor image 6220800 sensor-image-6220800
        run_case sensor pointcloud 20971520 sensor-pointcloud-20971520
    fi
    if [[ "${profile_filter}" == all || "${profile_filter}" == control ]]; then
        run_case control control-command 16 control-16
    fi
fi

printf 'RBT_BENCH_COMPLETE run_id=%s\n' "${run_id}"

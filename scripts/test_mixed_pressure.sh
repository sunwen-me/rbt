#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

print_command() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
}

require_command cargo
require_command cmake
require_command timeout
require_command grep

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
toolchain_bin="${toolchain_root}/bin"
if [[ -d "${toolchain_bin}" ]]; then
    export PATH="${toolchain_bin}${PATH:+:${PATH}}"
    if [[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-g++" ]]; then
        export CC="${toolchain_bin}/x86_64-conda-linux-gnu-gcc"
        export CXX="${toolchain_bin}/x86_64-conda-linux-gnu-g++"
        export AR="${toolchain_bin}/x86_64-conda-linux-gnu-ar"
        export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="x86_64-conda-linux-gnu-gcc"
        export LD_LIBRARY_PATH="${toolchain_root}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
    fi
fi

cpp_prefix="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"
"${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${cpp_prefix}"
cpp_build_dir="${RBT_MIXED_PRESSURE_CPP_BUILD_DIR:-${repo_root}/target/rbt-cpp-build}"
case_timeout_seconds="${RBT_MIXED_PRESSURE_TIMEOUT_SECONDS:-15}"
kill_after_seconds="${RBT_MIXED_PRESSURE_KILL_AFTER_SECONDS:-2}"
[[ "${case_timeout_seconds}" =~ ^[1-9][0-9]*$ ]] || die "RBT_MIXED_PRESSURE_TIMEOUT_SECONDS must be positive"
[[ "${kill_after_seconds}" =~ ^[1-9][0-9]*$ ]] || die "RBT_MIXED_PRESSURE_KILL_AFTER_SECONDS must be positive"
[[ -d "${cpp_prefix}" ]] || die "iceoryx2 C++ prefix is missing: ${cpp_prefix}"

rust_binary="${RBT_MIXED_PRESSURE_RUST_BINARY:-${repo_root}/target/release/mixed_pressure}"
cpp_binary="${RBT_MIXED_PRESSURE_CPP_BINARY:-${cpp_build_dir}/interop_cpp}"

print_command cargo build --release -p rbt-iox2 --bin mixed_pressure
cargo build --release -p rbt-iox2 --bin mixed_pressure

if [[ ! -f "${cpp_build_dir}/CMakeCache.txt" ]]; then
    cmake_args=(
        -S "${repo_root}/sdk/cpp"
        -B "${cpp_build_dir}"
        -DCMAKE_BUILD_TYPE=Debug
        -DCMAKE_PREFIX_PATH="${cpp_prefix}"
    )
    if command -v ninja >/dev/null 2>&1; then
        cmake_args+=(-G Ninja)
    fi
    if [[ -n "${CXX:-}" ]]; then
        cmake_args+=(-DCMAKE_CXX_COMPILER="${CXX}")
    fi
    print_command cmake "${cmake_args[@]}"
    cmake "${cmake_args[@]}"
fi
print_command cmake --build "${cpp_build_dir}" --target interop_cpp --parallel
cmake --build "${cpp_build_dir}" --target interop_cpp --parallel

[[ -x "${rust_binary}" ]] || die "Rust mixed-pressure binary is missing: ${rust_binary}"
[[ -x "${cpp_binary}" ]] || die "C++ interop_cpp binary is missing: ${cpp_binary}"

tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/rbt-mixed-pressure.XXXXXX")"
run_id="$(basename "${tmp_root}")"
declare -a child_pids=()
declare -A child_labels=()
declare -A child_logs=()
last_pid=''

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

printf 'MIXED_PRESSURE_RUN run_id=%s timeout_seconds=%s\n' "${run_id}" "${case_timeout_seconds}"

run_background() {
    local label="$1"
    shift
    local log_file="${tmp_root}/${label}.log"
    print_command timeout --signal=TERM --kill-after="${kill_after_seconds}s" \
        "${case_timeout_seconds}s" "$@"
    timeout --signal=TERM --kill-after="${kill_after_seconds}s" \
        "${case_timeout_seconds}s" "$@" >"${log_file}" 2>&1 &
    last_pid="$!"
    child_pids+=("${last_pid}")
    child_labels["${last_pid}"]="${label}"
    child_logs["${last_pid}"]="${log_file}"
}

forget_pid() {
    local pid="$1"
    local -a remaining=()
    local candidate
    for candidate in "${child_pids[@]}"; do
        if [[ "${candidate}" != "${pid}" ]]; then
            remaining+=("${candidate}")
        fi
    done
    child_pids=("${remaining[@]}")
    unset 'child_labels[$pid]'
    unset 'child_logs[$pid]'
}

wait_background() {
    local pid="$1"
    local label="${child_labels[${pid}]}"
    local log_file="${child_logs[${pid}]}"
    local status=0
    wait "${pid}" || status=$?
    forget_pid "${pid}"
    cat "${log_file}"
    ((status == 0)) || die "${label} failed with exit ${status}; see ${log_file}"
}

wait_for_ready() {
    local pid="$1"
    local pattern="$2"
    local label="${child_labels[${pid}]}"
    local log_file="${child_logs[${pid}]}"
    local attempts=$((case_timeout_seconds * 20))
    local attempt
    for ((attempt = 0; attempt < attempts; attempt++)); do
        if grep -Fq -- "${pattern}" "${log_file}" 2>/dev/null; then
            return 0
        fi
        if ! kill -0 "${pid}" 2>/dev/null; then
            wait_background "${pid}"
            die "${label} exited before readiness line ${pattern@Q}"
        fi
        sleep 0.05
    done
    cat "${log_file}" >&2 || true
    die "timed out waiting for ${label} readiness line ${pattern@Q}"
}

assert_log_contains() {
    local label="$1"
    local pattern="$2"
    local log_file="${tmp_root}/${label}.log"
    if ! grep -Fq -- "${pattern}" "${log_file}"; then
        cat "${log_file}" >&2 || true
        die "${label} is missing expected log text ${pattern@Q}"
    fi
}

assert_log_matches() {
    local label="$1"
    local pattern="$2"
    local log_file="${tmp_root}/${label}.log"
    if ! grep -Eq -- "${pattern}" "${log_file}"; then
        cat "${log_file}" >&2 || true
        die "${label} is missing expected log pattern ${pattern@Q}"
    fi
}

new_service() {
    printf 'rbt/mixed-pressure/%s/%s' "${run_id}" "$1"
}

# Case 1: a Rust Sensor publisher feeds a Rust fast reader, a slow C++ reader,
# and a stalled C++ reader.  The Rust publisher waits for all three endpoints
# before measuring its fixed 40-sample run.
case_one_service="$(new_service rust-publisher)"
run_background case1-rust-publisher "${rust_binary}" publisher "${case_one_service}" 3
case1_publisher_pid="${last_pid}"
wait_for_ready "${case1_publisher_pid}" 'MIXED_RUST_PUBLISHER ready'

run_background case1-rust-fast "${rust_binary}" subscriber "${case_one_service}" 0
case1_fast_pid="${last_pid}"
wait_for_ready "${case1_fast_pid}" 'MIXED_RUST_SUBSCRIBER ready'

run_background case1-cpp-slow "${cpp_binary}" image-subscriber "${case_one_service}" 3 50
case1_slow_pid="${last_pid}"
wait_for_ready "${case1_slow_pid}" 'IMAGE_CONSUMER ready'

run_background case1-cpp-stalled "${cpp_binary}" image-subscriber-hold "${case_one_service}" 2000
case1_stalled_pid="${last_pid}"
wait_for_ready "${case1_stalled_pid}" 'IMAGE_CONSUMER_HOLD ready'

wait_background "${case1_publisher_pid}"
wait_background "${case1_fast_pid}"
wait_background "${case1_slow_pid}"
wait_background "${case1_stalled_pid}"
assert_log_contains case1-rust-publisher 'MIXED_RUST_PUBLISHER subscribers=3 expected=3'
assert_log_matches case1-rust-publisher 'MIXED_RUST_PUBLISHER complete .*frames=40 .*send_errors=0 '
assert_log_matches case1-rust-fast 'MIXED_RUST_SUBSCRIBER complete .*frames=([2-9][0-9]*|1[0-9]{2,})'
assert_log_matches case1-cpp-slow 'IMAGE_CONSUMER complete .*frames=3 '
assert_log_contains case1-cpp-stalled 'IMAGE_CONSUMER_HOLD complete service='"${case_one_service}"' hold_ms=2000 received=0'

# Case 2: a C++ Sensor publisher feeds a Rust fast reader and a Rust stalled
# reader.  The C++ publisher uses a typed send result for its bounded warmup,
# then runs the same fixed 40-sample Sensor sequence.
case_two_service="$(new_service cpp-publisher)"
run_background case2-cpp-publisher "${cpp_binary}" image-publisher "${case_two_service}" 2
case2_publisher_pid="${last_pid}"
wait_for_ready "${case2_publisher_pid}" 'IMAGE_PUBLISHER ready'

run_background case2-rust-fast "${rust_binary}" subscriber "${case_two_service}" 0
case2_fast_pid="${last_pid}"
wait_for_ready "${case2_fast_pid}" 'MIXED_RUST_SUBSCRIBER ready'

run_background case2-rust-stalled "${rust_binary}" stalled "${case_two_service}" 2000
case2_stalled_pid="${last_pid}"
wait_for_ready "${case2_stalled_pid}" 'MIXED_RUST_STALLED ready'

wait_background "${case2_publisher_pid}"
wait_background "${case2_fast_pid}"
wait_background "${case2_stalled_pid}"
assert_log_contains case2-cpp-publisher 'IMAGE_PUBLISHER subscribers=2 '
assert_log_matches case2-cpp-publisher 'IMAGE_PUBLISHER complete .*frames=40 .*send_errors=0 '
assert_log_matches case2-rust-fast 'MIXED_RUST_SUBSCRIBER complete .*frames=([2-9][0-9]*|1[0-9]{2,})'
assert_log_contains case2-rust-stalled 'MIXED_RUST_STALLED complete service='"${case_two_service}"' hold_ms=2000 received=0'

printf 'PASS: mixed-language Sensor pressure cases completed\n'

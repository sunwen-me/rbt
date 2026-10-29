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

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
toolchain_bin="${toolchain_root}/bin"
cpp_prefix="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"
"${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${cpp_prefix}"
cpp_build_dir="${RBT_PHASE4_CPP_BUILD_DIR:-${repo_root}/target/phase4-cpp-build}"
log_root="${RBT_PHASE4_LOG_DIR:-${repo_root}/target/phase4-logs}"
pair_timeout_seconds="${RBT_PHASE4_TIMEOUT_SECONDS:-30}"
kill_after_seconds="${RBT_PHASE4_KILL_AFTER_SECONDS:-5}"

[[ -d "${toolchain_bin}" ]] || die "Phase4 toolchain bin directory is missing: ${toolchain_bin}"
[[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-gcc" ]] || die "Phase4 C compiler is missing: ${toolchain_bin}/x86_64-conda-linux-gnu-gcc"
[[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-g++" ]] || die "Phase4 C++ compiler is missing: ${toolchain_bin}/x86_64-conda-linux-gnu-g++"
[[ -x "${toolchain_bin}/ninja" ]] || die "Phase4 Ninja is missing: ${toolchain_bin}/ninja"
[[ -d "${cpp_prefix}" ]] || die "iceoryx2 C++ prefix is missing: ${cpp_prefix}"
[[ -f "${cpp_prefix}/lib/cmake/iceoryx2-cxx/iceoryx2-cxxConfig.cmake" ]] || die "iceoryx2 C++ package config is missing under: ${cpp_prefix}"
[[ -f "${cpp_prefix}/lib/libiceoryx2_cxx.a" ]] || die "iceoryx2 C++ static library is missing under: ${cpp_prefix}"
[[ "${pair_timeout_seconds}" =~ ^[1-9][0-9]*$ ]] || die "RBT_PHASE4_TIMEOUT_SECONDS must be a positive integer"
[[ "${kill_after_seconds}" =~ ^[1-9][0-9]*$ ]] || die "RBT_PHASE4_KILL_AFTER_SECONDS must be a positive integer"

require_command cargo
require_command rustc
require_command cmake
require_command ctest
require_command timeout
require_command diff
require_command cmp

export PATH="${toolchain_bin}${PATH:+:${PATH}}"
export CC="${toolchain_bin}/x86_64-conda-linux-gnu-gcc"
export CXX="${toolchain_bin}/x86_64-conda-linux-gnu-g++"
export AR="${toolchain_bin}/x86_64-conda-linux-gnu-ar"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
export LD_LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"
export CMAKE_PREFIX_PATH="${cpp_prefix}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"

tmp_root=''
declare -a background_pids=()
declare -A background_labels=()
declare -A background_logs=()
last_background_pid=''

forget_background_pid() {
    local pid="$1"
    local -a remaining=()
    local candidate
    for candidate in "${background_pids[@]}"; do
        if [[ "${candidate}" != "${pid}" ]]; then
            remaining+=("${candidate}")
        fi
    done
    background_pids=("${remaining[@]}")
    unset 'background_labels[$pid]'
    unset 'background_logs[$pid]'
}

cleanup_on_exit() {
    local pid
    for pid in "${background_pids[@]}"; do
        if kill -0 "${pid}" 2>/dev/null; then
            kill -TERM "${pid}" 2>/dev/null || true
        fi
    done
    for pid in "${background_pids[@]}"; do
        wait "${pid}" 2>/dev/null || true
    done
    if [[ -n "${tmp_root}" && -d "${tmp_root}" ]]; then
        rm -rf -- "${tmp_root}"
    fi
}

trap cleanup_on_exit EXIT
trap 'exit 130' INT TERM

tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/rbt-phase4.XXXXXX")"
run_id="$(basename "${tmp_root}")"
run_log_dir="${log_root}/${run_id}"
mkdir -p "${run_log_dir}"
generation_one="$(mktemp -d "${tmp_root}/generation-one.XXXXXX")"
generation_two="$(mktemp -d "${tmp_root}/generation-two.XXXXXX")"
mismatch_generation_one="$(mktemp -d "${tmp_root}/mismatch-generation-one.XXXXXX")"
mismatch_generation_two="$(mktemp -d "${tmp_root}/mismatch-generation-two.XXXXXX")"

printf 'Phase4 verifier run=%s\n' "${run_id}"
printf '  toolchain=%s\n  prefix=%s\n  logs=%s\n' "${toolchain_root}" "${cpp_prefix}" "${run_log_dir}"

run_logged() {
    local label="$1"
    shift
    local log_file="${run_log_dir}/${label}.log"
    print_command "$@"
    set +e
    "$@" >"${log_file}" 2>&1
    local status=$?
    set -e
    cat "${log_file}"
    if ((status != 0)); then
        die "${label} failed with exit ${status}; see ${log_file}"
    fi
}

run_foreground() {
    local label="$1"
    shift
    local log_file="${run_log_dir}/${label}.log"
    local timeout_value="${pair_timeout_seconds}s"
    print_command timeout --signal=TERM --kill-after="${kill_after_seconds}s" "${timeout_value}" "$@"
    set +e
    timeout --signal=TERM --kill-after="${kill_after_seconds}s" "${timeout_value}" "$@" >"${log_file}" 2>&1
    local status=$?
    set -e
    cat "${log_file}"
    if ((status != 0)); then
        die "${label} failed with exit ${status}; see ${log_file}"
    fi
}

run_background() {
    local label="$1"
    shift
    local log_file="${run_log_dir}/${label}.log"
    local timeout_value="${pair_timeout_seconds}s"
    print_command timeout --signal=TERM --kill-after="${kill_after_seconds}s" "${timeout_value}" "$@"
    timeout --signal=TERM --kill-after="${kill_after_seconds}s" "${timeout_value}" "$@" >"${log_file}" 2>&1 &
    last_background_pid="$!"
    background_pids+=("${last_background_pid}")
    background_labels["${last_background_pid}"]="${label}"
    background_logs["${last_background_pid}"]="${log_file}"
}

wait_background() {
    local pid="$1"
    local label="${background_labels[${pid}]}"
    local log_file="${background_logs[${pid}]}"
    set +e
    wait "${pid}"
    local status=$?
    set -e
    forget_background_pid "${pid}"
    cat "${log_file}"
    if ((status != 0)); then
        die "${label} failed with exit ${status}; see ${log_file}"
    fi
}

wait_for_log_line() {
    local pid="$1"
    local pattern="$2"
    local label="${background_labels[${pid}]}"
    local log_file="${background_logs[${pid}]}"
    local attempts=$((pair_timeout_seconds * 10))
    local attempt
    for ((attempt = 0; attempt < attempts; attempt++)); do
        if grep -Fq -- "${pattern}" "${log_file}" 2>/dev/null; then
            return 0
        fi
        if ! kill -0 "${pid}" 2>/dev/null; then
            wait_background "${pid}"
            die "${label} exited before readiness line ${pattern@Q}"
        fi
        sleep 0.1
    done
    die "timed out waiting for ${label} readiness line ${pattern@Q}; see ${log_file}"
}

assert_log_contains() {
    local label="$1"
    local pattern="$2"
    local log_file="${run_log_dir}/${label}.log"
    if ! grep -Fq -- "${pattern}" "${log_file}"; then
        cat "${log_file}" >&2
        die "${label} is missing expected log text ${pattern@Q}; see ${log_file}"
    fi
}

assert_log_matches() {
    local label="$1"
    local pattern="$2"
    local log_file="${run_log_dir}/${label}.log"
    if ! grep -Eq -- "${pattern}" "${log_file}"; then
        cat "${log_file}" >&2
        die "${label} is missing expected log pattern ${pattern@Q}; see ${log_file}"
    fi
}

compare_tree_bytes() {
    local label="$1"
    local left="$2"
    local right="$3"
    local diff_log="${run_log_dir}/${label}.diff"
    local status
    set +e
    diff -ru -- "${left}" "${right}" >"${diff_log}" 2>&1
    status=$?
    set -e
    if ((status != 0)); then
        cat "${diff_log}" >&2
        die "${label} differs; see ${diff_log}"
    fi
    while IFS= read -r -d '' left_file; do
        local relative_file="${left_file#"${left}"/}"
        if ! cmp -s -- "${left_file}" "${right}/${relative_file}"; then
            printf 'byte comparison failed: %s\n' "${relative_file}" >&2
            die "${label} byte comparison failed; see ${diff_log}"
        fi
    done < <(find "${left}" -type f -print0)
}

run_logged "generate-one" cargo run --quiet -p rbt-idl -- generate \
    types/sensor.rbt types/control.rbt types/demo.rbt types/tf.rbt \
    --rust "${generation_one}/rust" \
    --cpp "${generation_one}/cpp" \
    --descriptor "${generation_one}/descriptors"
run_logged "generate-two" cargo run --quiet -p rbt-idl -- generate \
    types/sensor.rbt types/control.rbt types/demo.rbt types/tf.rbt \
    --rust "${generation_two}/rust" \
    --cpp "${generation_two}/cpp" \
    --descriptor "${generation_two}/descriptors"
run_logged "generate-mismatch-one" cargo run --quiet -p rbt-idl -- generate \
    types/fixtures/image_mismatch.rbt \
    --rust "${mismatch_generation_one}/rust" \
    --cpp "${mismatch_generation_one}/cpp" \
    --descriptor "${mismatch_generation_one}/descriptors"
run_logged "generate-mismatch-two" cargo run --quiet -p rbt-idl -- generate \
    types/fixtures/image_mismatch.rbt \
    --rust "${mismatch_generation_two}/rust" \
    --cpp "${mismatch_generation_two}/cpp" \
    --descriptor "${mismatch_generation_two}/descriptors"

compare_tree_bytes generated-rust-twice "${generation_one}/rust" "${generation_two}/rust"
compare_tree_bytes generated-cpp-twice "${generation_one}/cpp" "${generation_two}/cpp"
compare_tree_bytes generated-descriptors-twice "${generation_one}/descriptors" "${generation_two}/descriptors"
compare_tree_bytes generated-rust-checked-in "${generation_one}/rust" "${repo_root}/generated/rust"
compare_tree_bytes generated-cpp-checked-in "${generation_one}/cpp" "${repo_root}/generated/cpp"
compare_tree_bytes generated-descriptors-checked-in "${generation_one}/descriptors" "${repo_root}/generated/descriptors"
compare_tree_bytes generated-mismatch-rust-twice "${mismatch_generation_one}/rust" "${mismatch_generation_two}/rust"
compare_tree_bytes generated-mismatch-cpp-twice "${mismatch_generation_one}/cpp" "${mismatch_generation_two}/cpp"
compare_tree_bytes generated-mismatch-descriptors-twice "${mismatch_generation_one}/descriptors" "${mismatch_generation_two}/descriptors"
compare_tree_bytes generated-mismatch-rust-checked-in "${mismatch_generation_one}/rust" "${repo_root}/generated/mismatch/rust"
compare_tree_bytes generated-mismatch-cpp-checked-in "${mismatch_generation_one}/cpp" "${repo_root}/generated/mismatch/cpp"
compare_tree_bytes generated-mismatch-descriptors-checked-in "${mismatch_generation_one}/descriptors" "${repo_root}/generated/mismatch/descriptors"

run_logged "rust-build-release" cargo build --release -p phase4-interop

rust_interop="${repo_root}/target/release/phase4-interop"
cpp_layout="${cpp_build_dir}/layout_cpp"
cpp_interop="${cpp_build_dir}/interop_cpp"
cpp_abi_mismatch="${cpp_build_dir}/abi_mismatch_cpp"
[[ -x "${rust_interop}" ]] || die "release phase4-interop binary is missing: ${rust_interop}"

run_logged "cpp-configure" cmake \
    -S "${repo_root}/sdk/cpp" \
    -B "${cpp_build_dir}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_COMPILER="${CXX}" \
    -DCMAKE_PREFIX_PATH="${cpp_prefix}"
run_logged "cpp-build" cmake --build "${cpp_build_dir}" --parallel
run_logged "cpp-ctest" ctest --test-dir "${cpp_build_dir}" --output-on-failure
[[ -x "${cpp_layout}" ]] || die "C++ layout binary is missing: ${cpp_layout}"
[[ -x "${cpp_interop}" ]] || die "C++ interop binary is missing: ${cpp_interop}"
[[ -x "${cpp_abi_mismatch}" ]] || die "C++ ABI mismatch binary is missing: ${cpp_abi_mismatch}"

run_logged "layout-rust" "${rust_interop}" layout
run_logged "layout-cpp" "${cpp_layout}"
if ! cmp -s -- "${run_log_dir}/layout-rust.log" "${run_log_dir}/layout-cpp.log"; then
    diff -u -- "${run_log_dir}/layout-rust.log" "${run_log_dir}/layout-cpp.log" | tee "${run_log_dir}/layout.diff" >&2 || true
    die "Rust and C++ layout output differs; see ${run_log_dir}/layout.diff"
fi

new_service() {
    service_counter=$((service_counter + 1))
    printf 'rbt/phase4/%s/%s-%s' "${run_id}" "$1" "${service_counter}"
}

service_counter=0

image_service="$(new_service image)"
run_background image-publisher-rust "${rust_interop}" image-publisher "${image_service}"
image_publisher_pid="${last_background_pid}"
wait_for_log_line "${image_publisher_pid}" 'IMAGE_PUBLISHER ready'
run_foreground image-subscriber-cpp "${cpp_interop}" image-subscriber "${image_service}" 3 0
wait_background "${image_publisher_pid}"
assert_log_contains image-subscriber-cpp 'IMAGE_CONSUMER complete service='"${image_service}"' frames=3'

pointcloud_service="$(new_service pointcloud)"
run_background pointcloud-subscriber-rust "${rust_interop}" pointcloud-subscriber "${pointcloud_service}"
pointcloud_subscriber_pid="${last_background_pid}"
run_foreground pointcloud-publisher-cpp "${cpp_interop}" pointcloud-publisher "${pointcloud_service}"
wait_background "${pointcloud_subscriber_pid}"

mismatch_service="$(new_service mismatch)"
run_background mismatch-provider-rust "${rust_interop}" image-provider-hold "${mismatch_service}" 1500
mismatch_provider_pid="${last_background_pid}"
wait_for_log_line "${mismatch_provider_pid}" 'IMAGE_PROVIDER_HOLD ready'
actual_image_hash="$(sed -n 's/.*IMAGE_PROVIDER_HOLD ready.*hash=\([[:xdigit:]]\{32\}\).*/\1/p' "${background_logs[${mismatch_provider_pid}]}")"
[[ "${actual_image_hash}" =~ ^[0-9a-f]{32}$ ]] || die "could not extract original Image provider hash"
run_foreground abi-mismatch-image-cpp "${cpp_abi_mismatch}" "${mismatch_service}" "${actual_image_hash}"
wait_background "${mismatch_provider_pid}"
assert_log_contains abi-mismatch-image-cpp 'error=AbiMismatch stage=preflight transport_opened=false'

control_service="$(new_service control)"
run_background control-subscriber-rust "${rust_interop}" control-subscriber "${control_service}" 1500
control_subscriber_pid="${last_background_pid}"
run_foreground control-publisher-cpp "${cpp_interop}" control-publisher "${control_service}"
wait_background "${control_subscriber_pid}"
assert_log_contains control-publisher-cpp '[control-test] queue saturation sequence=4 error=QueueFull'
assert_log_contains control-publisher-cpp '[control-test] sent=5 delivered=4 profile=Control'

smoke_service="$(new_service smoke)"
run_background image-publisher-smoke-rust "${rust_interop}" image-publisher-smoke "${smoke_service}"
smoke_publisher_pid="${last_background_pid}"
wait_for_log_line "${smoke_publisher_pid}" 'IMAGE_PUBLISHER_SMOKE ready'
run_foreground image-subscriber-smoke-cpp "${cpp_interop}" image-subscriber "${smoke_service}" 6 200
wait_background "${smoke_publisher_pid}"
assert_log_matches image-subscriber-smoke-cpp 'IMAGE_CONSUMER complete .* frames=6 gaps=[1-9][0-9]* estimated_dropped=[1-9][0-9]*'

run_logged "cargo-fmt-check" cargo fmt --check
run_logged "cargo-test-workspace" cargo test --workspace
run_logged "cargo-clippy-workspace" cargo clippy --workspace --all-targets --all-features -- -D warnings

printf 'PASS: Phase4 verifier completed successfully; logs=%s\n' "${run_log_dir}"

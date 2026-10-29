#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

die() {
    printf 'RBT_TIME_INTEROP FAIL %s\n' "$*" >&2
    printf 'RBT_TIME_INTEROP logs=%s\n' "${run_dir:-not-started}" >&2
    exit 1
}

mode=matrix
device=''
duration=300
while (($#)); do
    case "$1" in
        --build-only) mode=build ;;
        --sanitizers-only) mode=sanitizers ;;
        --external-consumers-only) mode=external ;;
        --hardware) mode=hardware ;;
        --device) (($# >= 2)) || die '--device requires PATH'; device="$2"; shift ;;
        --duration|--duration-sec) (($# >= 2)) || die '--duration requires seconds'; duration="$2"; shift ;;
        --help|-h)
            cat <<'USAGE'
usage: scripts/test_time_interop.sh [--build-only|--sanitizers-only|--external-consumers-only]
       scripts/test_time_interop.sh --hardware --device PATH --duration SEC

The default runs the directed six-pair Rust/C++/Python StampedBytes matrix.
USAGE
            exit 0
            ;;
        *) die "unknown argument: $1" ;;
    esac
    shift
done

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
toolchain_bin="${toolchain_root}/bin"
export PATH="${toolchain_bin}:${PATH}"
export CC="${CC:-${toolchain_bin}/x86_64-conda-linux-gnu-gcc}"
export CXX="${CXX:-${toolchain_bin}/x86_64-conda-linux-gnu-g++}"
export AR="${AR:-${toolchain_bin}/x86_64-conda-linux-gnu-ar}"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER:-${CC}}"
cpp_prefix="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"
export LD_LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"
[[ -x "${CXX}" ]] || die 'pinned CXX is missing'
[[ -x "${CC}" ]] || die 'pinned CC is missing'
[[ -x "${AR}" ]] || die 'pinned AR is missing'
python="${RBT_TIME_PYTHON:-${repo_root}/target/python-verify-venv/bin/python}"
[[ -x "${python}" ]] || die "Python interpreter is missing: ${python}"
log_root="${RBT_TIME_INTEROP_LOG_DIR:-${RBT_TIME_LOG_DIR:-${repo_root}/target/time-interop-logs}}"
mkdir -p "${log_root}"
run_id="interop-${mode}-$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="${log_root}/${run_id}"
mkdir -p "${run_dir}"
cpp_build="${RBT_TIME_CPP_BUILD:-${repo_root}/target/time-interop-build-v2}"
cpp_probe="${cpp_build}/rbt-time-probe"
rust_probe="${repo_root}/target/debug/rbt-time-tools"

run_logged() {
    local label="$1"
    shift
    local log_file="${run_dir}/${label}.log"
    local command_file="${run_dir}/${label}.command.log"
    { printf '+'; printf ' %q' "$@"; printf '\n'; } | tee "${command_file}"
    set +e
    "$@" >"${log_file}" 2>&1
    local status=$?
    set -e
    cat "${log_file}"
    printf 'RBT_TIME_INTEROP_STEP label=%s status=%s exit=%s log=%s\n' "${label}" \
        "$([[ ${status} == 0 ]] && printf PASS || printf FAIL)" "${status}" "${log_file}" \
        | tee -a "${command_file}"
    ((status == 0)) || die "${label} failed; see ${log_file}"
}

child_pids=()
forget_child() {
    local forgotten="$1"
    local pid
    local -a remaining=()
    for pid in "${child_pids[@]}"; do
        [[ "${pid}" == "${forgotten}" ]] || remaining+=("${pid}")
    done
    child_pids=("${remaining[@]}")
}

cleanup_children() {
    local exit_status=$?
    local pid
    trap - EXIT INT TERM
    set +e
    for pid in "${child_pids[@]}"; do
        kill -TERM "${pid}" 2>/dev/null
    done
    for pid in "${child_pids[@]}"; do
        wait "${pid}" 2>/dev/null
    done
    exit "${exit_status}"
}

trap cleanup_children EXIT
trap 'exit 130' INT TERM

write_cmake_project() {
    local source_dir="$1"
    mkdir -p "${source_dir}"
    cat >"${source_dir}/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.22)
project(rbt_time_probe_standalone LANGUAGES CXX)
if(NOT DEFINED RBT_REPO_ROOT)
    message(FATAL_ERROR "RBT_REPO_ROOT is required")
endif()
add_subdirectory("${RBT_REPO_ROOT}/sdk/cpp" rbt_sdk)
add_executable(rbt-time-probe "${RBT_REPO_ROOT}/sdk/cpp/tools/time_probe.cpp")
target_link_libraries(rbt-time-probe PRIVATE rbt-cpp)
target_compile_features(rbt-time-probe PRIVATE cxx_std_20)
target_compile_options(rbt-time-probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
CMAKE
}

build_cpp_probe() {
    local build_path="$1"
    local source_dir="${repo_root}/target/time-interop-cmake-source-$(basename "${build_path}")"
    write_cmake_project "${source_dir}"
    local -a sanitizer_flags=()
    if [[ "${build_path}" == *sanitized* ]]; then
        sanitizer_flags=("-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer"
            "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined")
    fi
    run_logged "cpp-configure-$(basename "${build_path}")" cmake -S "${source_dir}" -B "${build_path}" \
        -G Ninja -DRBT_REPO_ROOT="${repo_root}" -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_COMPILER="${CXX}" -DCMAKE_PREFIX_PATH="${cpp_prefix}" "${sanitizer_flags[@]}"
    run_logged "cpp-build-$(basename "${build_path}")" cmake --build "${build_path}" \
        --target rbt-time-probe --parallel
}

build_common() {
    run_logged rust-tool-build cargo build --locked -p rbt-time-tools
    build_cpp_probe "${cpp_build}"
}

ensure_python_extension() {
    run_logged python-time-native-extension env -u CONDA_PREFIX \
        VIRTUAL_ENV="$(dirname "$(dirname "${python}")")" \
        PYO3_PYTHON="${python}" \
        bash -c 'cd "$1" && exec "$2" -m maturin develop --release' \
        _ "${repo_root}/python" "${python}"
}

wait_for_marker() {
    local pid="$1"
    local logfile="$2"
    local marker="$3"
    local seconds="$4"
    for ((i = 0; i < seconds * 10; i++)); do
        grep -Fq "${marker}" "${logfile}" && return 0
        kill -0 "${pid}" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

command_for() {
    local language="$1"
    local operation="$2"
    local service="$3"
    case "${language}" in
        rust) printf '%s\0' "${rust_probe}" "stream-${operation}" "${service}" ;;
        cpp) printf '%s\0' "${cpp_probe}" "stream-${operation}" "${service}" ;;
        python)
            local py_mode=publish
            [[ "${operation}" != consume ]] || py_mode=consume
            printf '%s\0' "${python}" "${repo_root}/python/tests/time_stream_probe.py" "${py_mode}" "${service}"
            ;;
        *) die "unknown language: ${language}" ;;
    esac
}

validate_consumer_log() {
    local logfile="$1"
    local consumer="$2"
    "${python}" - "${logfile}" "${consumer}" \
        "${repo_root}/tests/fixtures/time/stamped_bytes.json" <<'PY'
import json
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
consumer = sys.argv[2]
fixture = json.loads(pathlib.Path(sys.argv[3]).read_text(encoding="utf-8"))
records = []
for line in path.read_text(encoding="utf-8").splitlines():
    if line.lstrip().startswith("{"):
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError:
            pass
if not records:
    raise SystemExit(f"{consumer} emitted no JSON result")
result = records[-1]
sample = fixture["sample"]
expected = {
    "status": "received",
    "sequence": sample["sequence"],
    "payload_len": len(bytes.fromhex(sample["application_hex"])),
    "fnv1a64": sample["application_fnv1a64"],
    "capture_ns": sample["capture"]["nanoseconds"],
    "received_ns": sample["received"]["nanoseconds"],
    "capture_uncertainty_ns": sample["capture_uncertainty_ns"],
    "received_uncertainty_ns": sample["received_uncertainty_ns"],
}
for key, value in expected.items():
    if result.get(key) != value:
        raise SystemExit(f"{consumer} result field {key}={result.get(key)!r}, expected {value!r}")
if consumer == "python":
    if result.get("retained_view_readonly") is not True or result.get("retained_view_after_sample_drop") is not True:
        raise SystemExit("Python subscriber did not retain a read-only native view after sample release")
print(f"RBT_TIME_INTEROP_CONSUMER PASS language={consumer} sequence={sample['sequence']} checksum={expected['fnv1a64']}")
PY
}

validate_publisher_log() {
    local logfile="$1"
    local publisher="$2"
    "${python}" - "${logfile}" "${publisher}" \
        "${repo_root}/tests/fixtures/time/stamped_bytes.json" <<'PY'
import json
import pathlib
import sys

records = []
for line in pathlib.Path(sys.argv[1]).read_text(encoding="utf-8").splitlines():
    if line.lstrip().startswith("{"):
        try:
            records.append(json.loads(line))
        except json.JSONDecodeError:
            pass
if not records:
    raise SystemExit(f"{sys.argv[2]} emitted no JSON publish result")
fixture = json.loads(pathlib.Path(sys.argv[3]).read_text(encoding="utf-8"))
sample = fixture["sample"]
expected = {"status": "sent", "sequence": sample["sequence"], "payload_len": len(bytes.fromhex(sample["application_hex"]))}
if records[-1] != expected:
    raise SystemExit(f"{sys.argv[2]} result {records[-1]!r} differs from fixture {expected!r}")
print(f"RBT_TIME_INTEROP_PUBLISHER PASS language={sys.argv[2]} sequence={sample['sequence']}")
PY
}

run_pair() {
    local publisher="$1"
    local consumer="$2"
    local suffix="${publisher}-to-${consumer}"
    local service="rbt/time/verify/${run_id}/${suffix}"
    local pub_log="${run_dir}/${suffix}.publisher.log"
    local sub_log="${run_dir}/${suffix}.subscriber.log"
    local -a pub_command sub_command
    mapfile -d '' -t pub_command < <(command_for "${publisher}" publish "${service}")
    mapfile -d '' -t sub_command < <(command_for "${consumer}" consume "${service}")
    { printf '+'; printf ' %q' "${pub_command[@]}"; printf '\n'; } >"${run_dir}/${suffix}.publisher.command.log"
    "${pub_command[@]}" >"${pub_log}" 2>&1 &
    local publisher_pid=$!
    child_pids+=("${publisher_pid}")
    if ! wait_for_marker "${publisher_pid}" "${pub_log}" READY_TO_PUBLISH 40; then
        kill "${publisher_pid}" 2>/dev/null || true
        wait "${publisher_pid}" 2>/dev/null || true
        forget_child "${publisher_pid}"
        die "${suffix} publisher did not open its stream; see ${pub_log}"
    fi
    { printf '+'; printf ' %q' "${sub_command[@]}"; printf '\n'; } >"${run_dir}/${suffix}.subscriber.command.log"
    "${sub_command[@]}" >"${sub_log}" 2>&1 &
    local subscriber_pid=$!
    child_pids+=("${subscriber_pid}")
    if ! wait_for_marker "${subscriber_pid}" "${sub_log}" READY_TO_RECEIVE 40; then
        kill "${publisher_pid}" "${subscriber_pid}" 2>/dev/null || true
        wait "${publisher_pid}" 2>/dev/null || true
        wait "${subscriber_pid}" 2>/dev/null || true
        forget_child "${publisher_pid}"
        forget_child "${subscriber_pid}"
        die "${suffix} subscriber did not open its stream; see ${sub_log}"
    fi
    local publisher_status=0 subscriber_status=0
    set +e
    wait "${publisher_pid}"; publisher_status=$?
    forget_child "${publisher_pid}"
    wait "${subscriber_pid}"; subscriber_status=$?
    forget_child "${subscriber_pid}"
    set -e
    cat "${pub_log}" "${sub_log}"
    printf 'RBT_TIME_INTEROP_PAIR publisher=%s subscriber=%s publisher_exit=%s subscriber_exit=%s\n' \
        "${publisher}" "${consumer}" "${publisher_status}" "${subscriber_status}" \
        | tee "${run_dir}/${suffix}.status.log"
    ((publisher_status == 0 && subscriber_status == 0)) || die "${suffix} process failed"
    validate_publisher_log "${pub_log}" "${publisher}" | tee "${run_dir}/${suffix}.publisher-validation.log"
    validate_consumer_log "${sub_log}" "${consumer}" | tee "${run_dir}/${suffix}.validation.log"
}

run_matrix() {
    ensure_python_extension
    for pair in rust:cpp rust:python cpp:rust cpp:python python:rust python:cpp; do
        run_pair "${pair%%:*}" "${pair#*:}"
    done
    printf 'RBT_TIME_INTEROP PASS matrix=6 directed_pairs layout_hash=97e9a924992dedfe83196197cccb6b35 logs=%s\n' "${run_dir}"
}

run_sanitizers() {
    local sanitized_build="${repo_root}/target/time-interop-build-sanitized"
    build_cpp_probe "${sanitized_build}"
    run_logged sanitized-cpp-semantics "${sanitized_build}/rbt-time-probe" semantics
    cpp_probe="${sanitized_build}/rbt-time-probe"
    export ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}"
    run_pair cpp rust
    run_pair rust cpp
}

run_external_consumers() {
    local external_dir="${run_dir}/external"
    local sdk_prefix="${external_dir}/sdk"
    local wheel_dir="${external_dir}/wheel"
    local python_venv="${TMPDIR:-/tmp}/${run_id}-python-venv"
    mkdir -p "${external_dir}/rust/src" "${external_dir}/cpp" "${wheel_dir}"
    run_logged external-sdk-install "${repo_root}/scripts/install_v01.sh" --prefix "${sdk_prefix}"
    local installed_rust_sdk="${sdk_prefix}/share/rbt/rust"
    [[ -f "${installed_rust_sdk}/crates/rbt-time/Cargo.toml" ]] \
        || die 'installed Rust SDK is missing rbt-time'
    [[ -f "${installed_rust_sdk}/crates/rbt-time-stream/Cargo.toml" ]] \
        || die 'installed Rust SDK is missing rbt-time-stream'
    cat >"${external_dir}/rust/Cargo.toml" <<EOF
[package]
name = "rbt-time-external-consumer"
version = "0.0.0"
edition = "2024"
[workspace]
[dependencies]
rbt = { path = "${installed_rust_sdk}/crates/rbt" }
EOF
    cat >"${external_dir}/rust/src/main.rs" <<'RUST'
use rbt::time::{ClockIdentity, ClockKind, ClockSourceId, Timestamp};
use rbt::time_stream::{StampedBytes, TIMING_PREFIX_LEN};
fn main() {
    let source = ClockSourceId::from_device_id([1; 16]).expect("source");
    let clock = ClockIdentity::new(ClockKind::Device, source, 7).expect("clock");
    let now = Timestamp::new(clock, 42);
    assert_eq!(now.nanoseconds(), 42);
    assert_eq!(StampedBytes::descriptor().layout_hash.to_string(), "97e9a924992dedfe83196197cccb6b35");
    assert_eq!(TIMING_PREFIX_LEN, 192);
}
RUST
    if rg -Fq -- "${repo_root}/crates/" "${external_dir}/rust/Cargo.toml"; then
        die 'external Rust consumer refers to the source repository'
    fi
    run_logged external-rust-consumer env CARGO_TARGET_DIR="${external_dir}/rust-target" \
        cargo run --offline --manifest-path "${external_dir}/rust/Cargo.toml"
    local installed_native_id
    run_logged external-rust-package-id cargo pkgid --offline \
        --manifest-path "${external_dir}/rust/Cargo.toml" -p rbt-time
    installed_native_id="$(cat "${run_dir}/external-rust-package-id.log")"
    [[ "${installed_native_id}" == "path+file://${installed_rust_sdk}/crates/rbt-time#0.8.0" ]] \
        || die "external Rust consumer resolved source crate: ${installed_native_id}"

    cat >"${external_dir}/cpp/CMakeLists.txt" <<EOF
cmake_minimum_required(VERSION 3.22)
project(rbt_time_external_consumer LANGUAGES CXX)
find_package(rbt 0.8.0 EXACT REQUIRED CONFIG)
add_executable(time_external main.cpp)
target_compile_features(time_external PRIVATE cxx_std_20)
target_compile_options(time_external PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(time_external PRIVATE rbt::runtime rbt::iox2)
EOF
    cat >"${external_dir}/cpp/main.cpp" <<'CPP'
#include <rbt/time_stream.hpp>
#include <array>
#include <cassert>
#include <string>
int main() {
    assert(rbt::time::kStampedBytesPrefixBytes == 192U);
    constexpr char digits[] = "0123456789abcdef";
    std::string hash;
    for (auto byte : rbt::time::STAMPED_BYTES_DESCRIPTOR.layout_hash) {
        hash.push_back(digits[(byte >> 4U) & 15U]);
        hash.push_back(digits[byte & 15U]);
    }
    assert(hash == "97e9a924992dedfe83196197cccb6b35");
    return 0;
}
CPP
    run_logged external-sdk-configure cmake -S "${repo_root}/sdk/cpp" \
        -B "${external_dir}/sdk-build" -G Ninja \
        -DCMAKE_INSTALL_PREFIX="${sdk_prefix}" -DCMAKE_PREFIX_PATH="${cpp_prefix}" \
        -DCMAKE_CXX_COMPILER="${CXX}"
    run_logged external-sdk-build cmake --build "${external_dir}/sdk-build" --target rbt-cpp --parallel
    run_logged external-cpp-install cmake --install "${external_dir}/sdk-build" --prefix "${sdk_prefix}"
    run_logged external-cpp-configure cmake -S "${external_dir}/cpp" -B "${external_dir}/cpp/build" \
        -G Ninja -DCMAKE_PREFIX_PATH="${sdk_prefix};${cpp_prefix}" -DCMAKE_CXX_COMPILER="${CXX}"
    run_logged external-cpp-build cmake --build "${external_dir}/cpp/build" --parallel
    run_logged external-cpp-consumer "${external_dir}/cpp/build/time_external"

    run_logged external-python-wheel env -u CONDA_PREFIX \
        VIRTUAL_ENV="$(dirname "$(dirname "${python}")")" PYO3_PYTHON="${python}" \
        bash -c 'cd "$1" && exec "$2" -m maturin build --release --out "$3"' \
        _ "${repo_root}/python" "${python}" "${wheel_dir}"
    local wheel
    wheel="$(find "${wheel_dir}" -maxdepth 1 -type f -name 'rbt-*.whl' -print -quit)"
    [[ -n "${wheel}" ]] || die 'fresh Python time wheel was not produced'
    run_logged external-python-venv-create "${python}" -m venv --system-site-packages "${python_venv}"
    run_logged external-python-wheel-install "${python_venv}/bin/python" -m pip install --no-deps --force-reinstall "${wheel}"
    run_logged external-python-consumer env -u PYTHONPATH -u CONDA_PREFIX PYTHONNOUSERSITE=1 \
        RBT_TIME_SOURCE_ROOT="${repo_root}" \
        bash -c 'cd "$1" && exec "$2" -c "$3"' _ "${external_dir}" "${python_venv}/bin/python" \
        'import os,pathlib,rbt; from rbt import StreamProfile; from rbt.time import ClockIdentity,ClockKind,ClockSourceId,Timestamp; from rbt.time_stream import TIMING_PREFIX_LEN,stamped_bytes_descriptor; root=pathlib.Path(os.environ["RBT_TIME_SOURCE_ROOT"]).resolve(); module=pathlib.Path(rbt.__file__).resolve(); c=ClockIdentity(ClockKind.DEVICE,ClockSourceId.from_device_uuid(bytes([1])*16),7); assert Timestamp(c,42).nanoseconds==42; assert TIMING_PREFIX_LEN==192; assert stamped_bytes_descriptor("rbt/time/external",profile=StreamProfile.SENSOR).message_type.layout_hash=="97e9a924992dedfe83196197cccb6b35"; assert root not in module.parents; print("external Python installed-wheel time APIs PASS",module)'
    run_logged external-python-venv-cleanup rm -rf -- "${python_venv}"
}

case "${mode}" in
    build)
        build_common
        printf 'RBT_TIME_INTEROP BUILD PASS probe=%s logs=%s\n' "${cpp_probe}" "${run_dir}"
        ;;
    matrix)
        build_common
        run_matrix
        ;;
    sanitizers)
        run_sanitizers
        ;;
    external)
        run_external_consumers
        ;;
    hardware)
        [[ "${device}" == /* ]] || die '--hardware requires an explicit absolute --device PATH'
        [[ "${duration}" =~ ^[0-9]+$ ]] && ((duration >= 300)) || die '--duration must be at least 300 seconds'
        if [[ ! -c "${device}" ]]; then
            printf 'RBT_TIME_HARDWARE status=NOTRUN reason=selected_device_unavailable path=%s\n' "${device}" \
                | tee "${run_dir}/hardware-notrun.log"
            die 'selected device is unavailable; no hardware pass was recorded'
        fi
        run_logged hardware-camera-and-language-consumers \
            "${repo_root}/scripts/test_time_hardware.py" \
            --device "${device}" --duration "${duration}" --log-dir "${run_dir}/hardware" --python "${python}"
        ;;
esac

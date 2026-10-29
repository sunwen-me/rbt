#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

die() {
    printf 'RBT_TIME_VERIFY FAIL %s\n' "$*" >&2
    [[ -z "${run_dir:-}" || ! -d "${run_dir}" ]] || printf 'RBT_TIME_VERIFY logs=%s\n' "${run_dir}" >&2
    exit 1
}

usage() {
    cat <<'USAGE'
usage: scripts/verify_time.sh [--quick|--full|--hardware] [--device PATH] [--duration SEC]

--quick     clock semantics, Rust/C++/Python tests, CLI JSON, frozen/hash/native audits
--full      quick plus cross-language stamped streams, packaged consumers and a 300s soak
--hardware  physical V4L2 camera producer and independent language consumers

Hardware mode requires an explicit --device PATH and --duration of at least 300 seconds.
No device is inferred from the host. Every command and result is retained in the log directory.
USAGE
}

mode=quick
mode_seen=0
device_path=''
duration_seconds=300
while (($#)); do
    case "$1" in
        --quick|--full|--hardware)
            ((mode_seen == 0)) || die 'select exactly one mode'
            mode="${1#--}"
            mode_seen=1
            shift
            ;;
        --device)
            (($# >= 2)) || die '--device requires a path'
            device_path="$2"
            shift 2
            ;;
        --duration)
            (($# >= 2)) || die '--duration requires seconds'
            duration_seconds="$2"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            usage >&2
            die "unknown argument: $1"
            ;;
    esac
done

if [[ "${mode}" == hardware ]]; then
    [[ -n "${device_path}" ]] || die '--hardware requires an explicit --device PATH'
    [[ "${device_path}" == /* ]] || die '--device must be an absolute path'
    [[ "${duration_seconds}" =~ ^[0-9]+$ ]] || die '--duration must be an integer number of seconds'
    ((duration_seconds >= 300)) || die '--hardware duration must be at least 300 seconds'
else
    [[ -z "${device_path}" ]] || die '--device is only valid with --hardware'
fi

for command_name in awk bash cargo cmake ctest date env find git mktemp python3 rg sha256sum tee; do
    command -v "${command_name}" >/dev/null 2>&1 || die "required command not found: ${command_name}"
done

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
toolchain_bin="${toolchain_root}/bin"
cpp_prefix="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"
cc_path="${toolchain_bin}/x86_64-conda-linux-gnu-gcc"
cxx_path="${toolchain_bin}/x86_64-conda-linux-gnu-g++"
ar_path="${toolchain_bin}/x86_64-conda-linux-gnu-ar"
[[ -x "${cc_path}" ]] || die "pinned C compiler is missing: ${cc_path}"
[[ -x "${cxx_path}" ]] || die "pinned C++ compiler is missing: ${cxx_path}"
[[ -x "${ar_path}" ]] || die "pinned archiver is missing: ${ar_path}"
[[ -f "${cpp_prefix}/lib/cmake/iceoryx2-cxx/iceoryx2-cxxConfig.cmake" ]] || die "maintained iceoryx2 C++ package config is missing under ${cpp_prefix}"

export PATH="${toolchain_bin}:${PATH}"
export CC="${cc_path}"
export CXX="${cxx_path}"
export AR="${ar_path}"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
export LD_LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"
export CMAKE_PREFIX_PATH="${cpp_prefix}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"

# Inherited soak scripts otherwise accept any executable in v01-cpp-debug,
# including a binary left from an older native ABI. The inherited RPC gate
# rebuilds this complete SDK directory before those scripts run.
legacy_cpp_build="${RBT_RPC_VERIFY_CPP_BUILD_DIR:-${repo_root}/target/rpc-verify-cpp-debug}"
export RBT_V01_CPP_BUILD_DIR="${RBT_V01_CPP_BUILD_DIR:-${legacy_cpp_build}}"
export RBT_PRESSURE_CPP_BUILD_DIR="${RBT_PRESSURE_CPP_BUILD_DIR:-${legacy_cpp_build}}"
# This watchdog supervises the complete 4096-call Debug interop workload.
# Individual RPC deadlines remain the unchanged probe/runtime deadlines.
export RBT_RPC_INTEROP_WATCHDOG_SECONDS="${RBT_RPC_INTEROP_WATCHDOG_SECONDS:-60}"

python="${RBT_TIME_PYTHON:-${repo_root}/target/python-verify-venv/bin/python}"
[[ -x "${python}" ]] || die "required Python interpreter is missing: ${python}"
log_root="${RBT_TIME_LOG_DIR:-${repo_root}/target/time-verify-logs}"
mkdir -p "${log_root}"
run_id="time-${mode}-$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="${log_root}/${run_id}"
mkdir -p "${run_dir}"
printf 'RBT_TIME_VERIFY mode=%s run_id=%s logs=%s toolchain=%s cpp_prefix=%s python=%s\n' \
    "${mode}" "${run_id}" "${run_dir}" "${toolchain_root}" "${cpp_prefix}" "${python}" \
    | tee "${run_dir}/environment.log"

run_logged() {
    local label="$1"
    shift
    local log_file="${run_dir}/${label}.log"
    local command_log="${run_dir}/${label}.command.log"
    {
        printf '+'
        printf ' %q' "$@"
        printf '\n'
    } | tee "${command_log}"
    set +e
    "$@" >"${log_file}" 2>&1
    local status=$?
    set -e
    cat "${log_file}"
    printf 'RBT_TIME_STEP label=%s status=%s exit=%s log=%s\n' \
        "${label}" "$([[ ${status} == 0 ]] && printf PASS || printf FAIL)" "${status}" "${log_file}" \
        | tee -a "${command_log}"
    ((status == 0)) || die "${label} failed with exit ${status}; see ${log_file}"
}

semantics_probes() {
    local rust_json="${run_dir}/rust-semantics.json"
    local cpp_json="${run_dir}/cpp-semantics.json"
    local python_json="${run_dir}/python-semantics.json"
    local cpp_probe="${repo_root}/target/time-interop-build-v2/rbt-time-probe"
    local cpp_build="${RBT_TIME_CPP_BUILD:-${repo_root}/target/time-verify-cpp-debug}"

    run_logged rust-time-tests cargo test --locked -p rbt-time
    run_logged rust-time-stream-tests cargo test --locked -p rbt-time-stream
    run_logged rust-time-tools-tests cargo test --locked -p rbt-time-tools
    run_logged cpp-time-configure cmake -S "${repo_root}/sdk/cpp" -B "${cpp_build}" \
        -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER="${CXX}" \
        -DCMAKE_PREFIX_PATH="${cpp_prefix}"
    run_logged cpp-time-build cmake --build "${cpp_build}" --target time_tests --parallel
    run_logged cpp-time-ctest ctest --test-dir "${cpp_build}" --output-on-failure -R '^time_tests$'
    run_logged python-time-tests "${python}" -m pytest -q "${repo_root}/python/tests/test_time.py"
    run_logged python-time-stream-tests env PYTHONPATH="${repo_root}/python${PYTHONPATH:+:${PYTHONPATH}}" \
        "${python}" -m pytest -q "${repo_root}/python/tests/test_time_stream.py"

    run_logged cpp-probe-build env RBT_TIME_INTEROP_LOG_DIR="${run_dir}/interop" \
        "${repo_root}/scripts/test_time_interop.sh" --build-only
    run_logged rust-semantics-output cargo run --locked --quiet -p rbt-time-tools -- semantics
    cp "${run_dir}/rust-semantics-output.log" "${rust_json}"
    run_logged cpp-semantics-output "${cpp_probe}" semantics
    cp "${run_dir}/cpp-semantics-output.log" "${cpp_json}"
    run_logged python-semantics-output env PYTHONPATH="${repo_root}/python${PYTHONPATH:+:${PYTHONPATH}}" \
        "${python}" "${repo_root}/scripts/time_semantics_probe.py" --emit-json
    cp "${run_dir}/python-semantics-output.log" "${python_json}"
    run_logged time-semantics-cross-language env PYTHONPATH="${repo_root}/python${PYTHONPATH:+:${PYTHONPATH}}" \
        "${python}" "${repo_root}/scripts/time_semantics_probe.py" --compare \
        --fixture "${repo_root}/tests/fixtures/time/semantics.json" \
        --rust-json "${rust_json}" --cpp-json "${cpp_json}" --python-json "${python_json}"
}

check_cli_json() {
    run_logged time-status-json cargo run --locked -p rbt-cli --bin rbt -- time status --json
    run_logged time-status-json-contract "${python}" -c 'import json,pathlib,sys; lines=pathlib.Path(sys.argv[1]).read_text().splitlines(); start=next((i for i,line in enumerate(lines) if line=="{"),None); assert start is not None,"missing top-level JSON object"; d=json.loads(chr(10).join(lines[start:])); assert isinstance(d,dict),"JSON report is not an object"; assert all(k in d for k in ("platform","checks","success","host_clocks","sources","source_observations")),"missing fields"; assert d["success"] is True,"host clocks unavailable"; assert all(d["host_clocks"][k]["availability"]=="available" and isinstance(d["host_clocks"][k]["timestamp"],dict) for k in ("monotonic","realtime")),"missing host timestamps"; print("RBT_TIME_CLI_JSON PASS")' "${run_dir}/time-status-json.log"
}

quick() {
    run_logged native-prefix-provenance "${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${cpp_prefix}"
    run_logged inherited-rpc-quick env RBT_RPC_VERIFY_PYTHON="${python}" \
        RBT_RPC_VERIFY_LOG_DIR="${run_dir}/inherited-rpc" \
        "${repo_root}/scripts/verify_rpc.sh" --quick
    semantics_probes
    check_cli_json
}

full() {
    local soak_seconds="${RBT_TIME_SOAK_SECONDS:-300}"
    [[ "${soak_seconds}" =~ ^[0-9]+$ ]] || die 'RBT_TIME_SOAK_SECONDS must be an integer'
    ((soak_seconds >= 300)) || die 'full software time soak must be at least 300 seconds'
    run_logged inherited-rpc-full env RBT_RPC_VERIFY_LOG_DIR="${run_dir}/inherited-rpc" \
        "${repo_root}/scripts/verify_rpc.sh" --full
    semantics_probes
    check_cli_json
    run_logged stamped-byte-interop "${repo_root}/scripts/test_time_interop.sh"
    run_logged time-soak "${repo_root}/scripts/soak_time.sh" --duration "${soak_seconds}"
    run_logged full-time-cpp-sanitizers "${repo_root}/scripts/test_time_interop.sh" --sanitizers-only
    run_logged external-time-consumers "${repo_root}/scripts/test_time_interop.sh" --external-consumers-only
}

hardware() {
    local device_log="${run_dir}/hardware-device.log"
    if [[ ! -c "${device_path}" ]]; then
        {
            printf 'RBT_TIME_HARDWARE status=NOTRUN reason=selected_device_not_available path=%s\n' "${device_path}"
            if [[ -e "${device_path}" ]]; then
                printf 'device_type=not_character_device\n'
            else
                printf 'device_type=missing\n'
            fi
        } | tee "${device_log}"
        die "selected hardware device is unavailable; evidence is retained in ${device_log}"
    fi
    run_logged hardware-camera-and-language-consumers "${repo_root}/scripts/test_time_interop.sh" \
        --hardware --device "${device_path}" --duration "${duration_seconds}"
}

case "${mode}" in
    quick) quick ;;
    full) full ;;
    hardware) hardware ;;
esac

printf 'PASS: time %s verification completed; logs=%s\n' "${mode}" "${run_dir}"

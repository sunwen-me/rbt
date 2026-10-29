#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

die() {
    printf 'RBT_TIME_SOAK_SCRIPT FAIL %s\n' "$*" >&2
    printf 'RBT_TIME_SOAK_SCRIPT logs=%s\n' "${run_dir:-not-started}" >&2
    exit 1
}

duration="${RBT_TIME_SOAK_SECONDS:-300}"
while (($#)); do
    case "$1" in
        --duration|--duration-sec)
            (($# >= 2)) || die '--duration requires seconds'
            duration="$2"
            shift 2
            ;;
        --help|-h)
            printf 'usage: scripts/soak_time.sh [--duration SEC] (minimum 300 seconds)\n'
            exit 0
            ;;
        *) die "unknown argument: $1" ;;
    esac
done
[[ "${duration}" =~ ^[0-9]+$ ]] || die '--duration must be an integer number of seconds'
((duration >= 300)) || die 'software time soak must be at least 300 seconds'

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
export PATH="${toolchain_root}/bin:${PATH}"
export CC="${CC:-${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc}"
export CXX="${CXX:-${toolchain_root}/bin/x86_64-conda-linux-gnu-g++}"
export AR="${AR:-${toolchain_root}/bin/x86_64-conda-linux-gnu-ar}"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER:-${CC}}"
cpp_prefix="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"
export LD_LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"

log_root="${RBT_TIME_SOAK_LOG_DIR:-${RBT_TIME_LOG_DIR:-${repo_root}/target/time-soak-logs}}"
mkdir -p "${log_root}"
run_dir="${log_root}/soak-$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir -p "${run_dir}"
binary="${repo_root}/target/debug/rbt-time-tools"
{
    printf '+ cargo build --locked -p rbt-time-tools\n'
} | tee "${run_dir}/build.command.log"
set +e
cargo build --locked -p rbt-time-tools >"${run_dir}/build.log" 2>&1
build_status=$?
set -e
cat "${run_dir}/build.log"
printf 'RBT_TIME_SOAK_BUILD status=%s exit=%s log=%s\n' \
    "$([[ ${build_status} == 0 ]] && printf PASS || printf FAIL)" "${build_status}" "${run_dir}/build.log" \
    | tee -a "${run_dir}/build.command.log"
((build_status == 0)) || die "fresh rbt-time-tools build failed with exit ${build_status}"
[[ -x "${binary}" ]] || die "fresh rbt-time-tools binary is missing: ${binary}"
command=("${binary}" soak --duration "${duration}")
{
    printf 'RBT_TIME_SOAK duration_sec=%s logs=%s\n' "${duration}" "${run_dir}"
    printf '+'
    printf ' %q' "${command[@]}"
    printf '\n'
} | tee "${run_dir}/command.log"
set +e
"${command[@]}" >"${run_dir}/soak.log" 2>&1
status=$?
set -e
cat "${run_dir}/soak.log"
if ((status != 0)); then
    printf 'RBT_TIME_SOAK_SCRIPT status=FAIL exit=%s log=%s\n' "${status}" "${run_dir}/soak.log" \
        | tee -a "${run_dir}/command.log"
    die "soak process failed with exit ${status}"
fi
if ! rg -q '^RBT_TIME_SOAK PASS ' "${run_dir}/soak.log"; then
    die 'soak process did not emit its mandatory PASS evidence'
fi
printf 'RBT_TIME_SOAK_SCRIPT status=PASS log=%s\n' "${run_dir}/soak.log" \
    | tee -a "${run_dir}/command.log"

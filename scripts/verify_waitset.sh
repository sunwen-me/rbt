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
usage: scripts/verify_waitset.sh [--quick|--full]

--quick  WaitSet Rust/C++ quality and focused tests (default)
--full   quick plus the v0.1 regression suite, the full WaitSet benchmark
         matrix, a bounded 30-60 second idle soak, and pressure checks

Environment overrides:
  RBT_PHASE4_TOOLCHAIN       Phase4 compiler/toolchain root
  RBT_IOX2_PREFIX            iceoryx2 C++ install prefix
  RBT_WAITSET_VERIFY_LOG_DIR retained verifier evidence root
  RBT_WAITSET_CPP_BUILD_ROOT Debug/Release CMake build root
  RBT_WAITSET_SKIP_V01=1    skip the inherited v0.1 gate when a parent
                             verifier already ran it in the same composition
  RBT_WAITSET_PRESSURE_FULL=1
                              run the exact 300-second pressure soak in --full
                              as a separately retained final artifact
                              (otherwise the bounded pressure smoke check is used)
EOF
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

print_command() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
}

mode=quick
if (($# > 1)); then
    usage >&2
    exit 2
fi
if (($# == 1)); then
    case "$1" in
        --quick) mode=quick ;;
        --full) mode=full ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
fi

pressure_full="${RBT_WAITSET_PRESSURE_FULL:-0}"
[[ "${pressure_full}" =~ ^[01]$ ]] \
    || die "RBT_WAITSET_PRESSURE_FULL must be 0 or 1"
skip_v01="${RBT_WAITSET_SKIP_V01:-0}"
[[ "${skip_v01}" =~ ^[01]$ ]] \
    || die "RBT_WAITSET_SKIP_V01 must be 0 or 1"

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
toolchain_bin="${toolchain_root}/bin"
cpp_prefix="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"
"${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${cpp_prefix}"
cpp_build_root="${RBT_WAITSET_CPP_BUILD_ROOT:-${repo_root}/target/waitset-verify-cpp}"
log_root="${RBT_WAITSET_VERIFY_LOG_DIR:-${repo_root}/target/waitset-verify-logs}"

for command_name in cargo rustc cmake ctest awk grep rg date sed tr; do
    require_command "${command_name}"
done

[[ -d "${toolchain_bin}" ]] \
    || die "Phase4 toolchain bin directory is missing: ${toolchain_bin}"
[[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-gcc" ]] \
    || die "Phase4 C compiler is missing: ${toolchain_bin}/x86_64-conda-linux-gnu-gcc"
[[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-g++" ]] \
    || die "Phase4 C++ compiler is missing: ${toolchain_bin}/x86_64-conda-linux-gnu-g++"
[[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-ar" ]] \
    || die "Phase4 archiver is missing: ${toolchain_bin}/x86_64-conda-linux-gnu-ar"
[[ -x "${toolchain_bin}/ninja" ]] \
    || die "Phase4 Ninja is missing: ${toolchain_bin}/ninja"
[[ -d "${cpp_prefix}" ]] || die "iceoryx2 C++ prefix is missing: ${cpp_prefix}"
[[ -f "${cpp_prefix}/lib/cmake/iceoryx2-cxx/iceoryx2-cxxConfig.cmake" ]] \
    || die "iceoryx2 C++ package config is missing under: ${cpp_prefix}"
[[ -f "${cpp_prefix}/lib/libiceoryx2_cxx.a" ]] \
    || die "iceoryx2 C++ static library is missing under: ${cpp_prefix}"

export PATH="${toolchain_bin}${PATH:+:${PATH}}"
export CC="${toolchain_bin}/x86_64-conda-linux-gnu-gcc"
export CXX="${toolchain_bin}/x86_64-conda-linux-gnu-g++"
export AR="${toolchain_bin}/x86_64-conda-linux-gnu-ar"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
export LD_LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"
export CMAKE_PREFIX_PATH="${cpp_prefix}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"

mkdir -p "${log_root}"
run_id="waitset-${mode}-$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="${log_root}/${run_id}"
mkdir -p "${run_dir}"

run_logged() {
    local label="$1"
    shift
    local log_file="${run_dir}/${label}.log"
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

{
    printf 'RBT_WAITSET_VERIFY_RUN run_id=%s mode=%s logs=%s\n' \
        "${run_id}" "${mode}" "${run_dir}"
    printf 'RBT_WAITSET_VERIFY_TOOLCHAIN root=%s prefix=%s cc=%s cxx=%s\n' \
        "${toolchain_root}" "${cpp_prefix}" "${CC}" "${CXX}"
    printf 'RBT_WAITSET_VERIFY_ENV rust=%s cmake=%s ninja=%s\n' \
        "$(rustc --version)" "$(cmake --version | sed -n '1p')" "$(ninja --version)"
} | tee "${run_dir}/environment.log"

run_logged rust-fmt cargo fmt --all -- --check
run_logged rust-check cargo check -p rbt-iox2 -p rbt-compose --tests
run_logged rust-test-waitset cargo test -p rbt-iox2 --test waitset
run_logged rust-test-waitset-fairness cargo test -p rbt-iox2 --test waitset_fairness
run_logged rust-test-waitset-lifecycle cargo test -p rbt-iox2 --test waitset_lifecycle
run_logged rust-test-compose cargo test -p rbt-compose --lib
run_logged rust-clippy cargo clippy -p rbt-iox2 -p rbt-compose \
    --all-targets --all-features -- -D warnings

for build_type in Debug Release; do
    build_key="$(printf '%s' "${build_type}" | tr '[:upper:]' '[:lower:]')"
    build_dir="${cpp_build_root}/${build_key}"
    run_logged "cpp-${build_key}-configure" cmake \
        -S "${repo_root}/sdk/cpp" \
        -B "${build_dir}" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE="${build_type}" \
        -DCMAKE_CXX_COMPILER="${CXX}" \
        -DCMAKE_PREFIX_PATH="${cpp_prefix}"
    run_logged "cpp-${build_key}-build" cmake --build "${build_dir}" --parallel 2
    run_logged "cpp-${build_key}-ctest" ctest \
        --test-dir "${build_dir}" --output-on-failure
    run_logged "cpp-${build_key}-waitset-ctest" ctest \
        --test-dir "${build_dir}" --output-on-failure -R '^waitset_tests$'
done

unsafe_audit() {
    local audit_log="${run_dir}/unsafe-audit.log"
    python3 "${repo_root}/scripts/audit_unsafe.py" --label RBT_WAITSET_UNSAFE_AUDIT 2>&1 \
        | tee "${audit_log}" \
        || die "production unsafe audit failed; see ${audit_log}"

    local protected_file
    local protected_matches="${run_dir}/unsafe-protected-matches.log"
    local protected_audit="${run_dir}/unsafe-protected-audit.log"
    : >"${protected_matches}"
    for protected_file in \
        crates/rbt-iox2/src/wait.rs \
        crates/rbt-compose/src/effect_scope.rs \
        crates/rbt-compose/src/component.rs; do
        [[ -f "${protected_file}" ]] || die "protected source file is missing: ${protected_file}"
        if rg -n '\bunsafe\b' "${protected_file}" >>"${protected_matches}"; then
            {
                printf 'RBT_WAITSET_UNSAFE_PROTECTED files=wait.rs,effect_scope.rs,component.rs matches=unexpected\n'
                cat "${protected_matches}"
            } >"${protected_audit}"
            cat "${protected_matches}" >&2
            die "unexpected unsafe token in ${protected_file}; see ${protected_audit}"
        else
            local status=$?
            ((status == 1)) || die "protected unsafe audit could not read ${protected_file}"
        fi
    done
    printf 'RBT_WAITSET_UNSAFE_PROTECTED files=wait.rs,effect_scope.rs,component.rs matches=0\n' \
        | tee "${protected_audit}"
}

unsafe_audit

if [[ "${mode}" == full ]]; then
    if [[ "${skip_v01}" == 1 ]]; then
        printf 'SKIP: inherited v0.1 gate already ran in the parent composition\n' \
            | tee "${run_dir}/verify-v01.log"
    else
        run_logged verify-v01 ./scripts/verify_v01.sh --full
    fi
    run_logged benchmark-full ./scripts/benchmark_waitset.sh
    run_logged idle-soak-bounded ./scripts/soak_waitset_idle.sh --smoke
    if [[ "${pressure_full}" == 1 ]]; then
        run_logged pressure-soak-300s ./scripts/soak_pressure.sh
    else
        run_logged pressure-smoke-bounded ./scripts/soak_pressure.sh --smoke
    fi
fi

printf 'PASS: WaitSet %s verification completed; logs=%s\n' "${mode}" "${run_dir}"

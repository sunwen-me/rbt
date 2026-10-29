#!/usr/bin/env bash
set -euo pipefail

# The RPC gate is intentionally a composition script.  It owns the small set
# of RPC-specific audits and delegates inherited lanes to their existing
# verifiers so that each lane keeps its own fixtures, process cleanup, and
# retained evidence.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
usage: scripts/verify_rpc.sh [--quick|--full]

--quick  Run the fixed/dynamic/stress/fault Rust RPC tests, the current C++
         RPC CTest, sync/async Python RPC smoke tests, CLI human/JSON
         diagnostics, source/hash/unsafe audits, and workspace fmt/test/clippy.
--full   Quick plus all Rust RPC tests, C++ Debug/Release/ASan/UBSan,
         fresh-wheel and full Python lanes, the RPC interop/replacement/
         process-death/capacity matrix, benchmark matrix, 300-second soak,
         Composer restart, inherited gates, and external consumers.

Required artifacts are never silently skipped.  Override their locations when
the checkout supplies a differently named gate:

  RBT_RPC_VERIFY_LOG_DIR       retained logs (default target/rpc-verify-logs)
  RBT_PHASE4_TOOLCHAIN         pinned compiler/toolchain root
  RBT_IOX2_PREFIX              iceoryx2 C++ install prefix
  RBT_RPC_VERIFY_PYTHON        Python containing pytest and the RPC extension
  RBT_RPC_SYNC_TEST_FILE       sync Python RPC smoke test
  RBT_RPC_ASYNC_TEST_FILE      async Python RPC smoke test
  RBT_RPC_INTEROP_SCRIPT       full cross-language RPC matrix script
  RBT_RPC_SOAK_SCRIPT          full 300-second RPC soak script

The full gate is intentionally not implied by --quick.  Every command log is
retained below the run directory printed at startup; temporary child state is
cleaned on exit.
EOF
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
        --help|-h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
fi

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

for command_name in \
    awk bash cargo cmake ctest date env find git mktemp rg sha256sum tee \
    tr wc xargs; do
    require_command "${command_name}"
done

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
toolchain_bin="${toolchain_root}/bin"
cpp_prefix="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"

[[ -d "${toolchain_bin}" ]] \
    || die "pinned toolchain directory is missing: ${toolchain_bin}"

if [[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-gcc" ]]; then
    cc_path="${toolchain_bin}/x86_64-conda-linux-gnu-gcc"
elif [[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-cc" ]]; then
    cc_path="${toolchain_bin}/x86_64-conda-linux-gnu-cc"
else
    die "pinned C compiler is missing under ${toolchain_bin} (expected x86_64-conda-linux-gnu-gcc or -cc)"
fi
if [[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-g++" ]]; then
    cxx_path="${toolchain_bin}/x86_64-conda-linux-gnu-g++"
elif [[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-c++" ]]; then
    cxx_path="${toolchain_bin}/x86_64-conda-linux-gnu-c++"
else
    die "pinned C++ compiler is missing under ${toolchain_bin} (expected x86_64-conda-linux-gnu-g++ or -c++)"
fi
ar_path="${toolchain_bin}/x86_64-conda-linux-gnu-ar"
[[ -x "${ar_path}" ]] || die "pinned archiver is missing: ${ar_path}"
[[ -f "${cpp_prefix}/lib/cmake/iceoryx2-cxx/iceoryx2-cxxConfig.cmake" ]] \
    || die "iceoryx2 C++ package config is missing under ${cpp_prefix}"

export PATH="${toolchain_bin}${PATH:+:${PATH}}"
export CC="${cc_path}"
export CXX="${cxx_path}"
export AR="${ar_path}"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
export LD_LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"
export CMAKE_PREFIX_PATH="${cpp_prefix}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"
require_command ninja

log_root="${RBT_RPC_VERIFY_LOG_DIR:-${repo_root}/target/rpc-verify-logs}"
mkdir -p "${log_root}"
run_id="rpc-${mode}-$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="${log_root}/${run_id}"
mkdir -p "${run_dir}"
tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/rbt-rpc-verify.XXXXXX")"
declare -a child_pids=()

cleanup() {
    local pid
    for pid in "${child_pids[@]:-}"; do
        [[ -n "${pid}" ]] || continue
        if kill -0 "${pid}" 2>/dev/null; then
            kill -TERM "${pid}" 2>/dev/null || true
        fi
    done
    for pid in "${child_pids[@]:-}"; do
        [[ -n "${pid}" ]] || continue
        wait "${pid}" 2>/dev/null || true
    done
    if [[ -d "${tmp_root}" ]]; then
        rmdir "${tmp_root}" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

run_logged() {
    local label="$1"
    shift
    local log_file="${run_dir}/${label}.log"
    {
        printf '+'
        printf ' %q' "$@"
        printf '\n'
    } | tee "${run_dir}/${label}.command.log"
    set +e
    "$@" >"${log_file}" 2>&1
    local status=$?
    set -e
    cat "${log_file}"
    ((status == 0)) || die "${label} failed with exit ${status}; see ${log_file}"
}

require_file() {
    [[ -f "$1" ]] || die "required artifact is missing: $1"
}

require_executable() {
    [[ -x "$1" ]] || die "required executable artifact is missing: $1"
}

require_test_symbol() {
    local source_file="$1"
    local symbol="$2"
    require_file "${source_file}"
    rg -n -Fq "${symbol}" "${source_file}" \
        || die "required RPC coverage symbol is missing: ${symbol} (${source_file})"
}

source_audit() {
    local iox2_source="${RBT_IOX2_FORK_SOURCE:-${repo_root}/third_party/iceoryx2-rbt/source}"
    local source_log="${run_dir}/source-audit.log"
    require_file "${repo_root}/Cargo.toml"
    require_file "${repo_root}/Cargo.lock"
    require_file "${repo_root}/docs/rpc-iox2-notes.md"
    require_file "${repo_root}/docs/rpc.md"
    require_file "${repo_root}/third_party/iceoryx2-rbt/UPSTREAM.toml"
    require_file "${repo_root}/third_party/iceoryx2-rbt/SOURCE_DIFF_SHA256"
    require_file "${iox2_source}/iceoryx2/src/service/builder/request_response.rs"
    require_file "${iox2_source}/iceoryx2/src/pending_response.rs"
    [[ "$(git -C "${iox2_source}" rev-parse HEAD 2>/dev/null || true)" == \
        135d09dd8b29f321f1725920d434864c4e512378 ]] \
        || die "iceoryx2 source audit is not the pinned unified v0.10.0 base: ${iox2_source}"
    rg -n -Fq 'fork_version = "0.10.0-rbt.2"' \
        "${repo_root}/third_party/iceoryx2-rbt/UPSTREAM.toml" \
        || die 'unified iceoryx2 fork metadata no longer identifies 0.10.0-rbt.2'
    rg -n -Fq 'base_commit = "135d09dd8b29f321f1725920d434864c4e512378"' \
        "${repo_root}/third_party/iceoryx2-rbt/UPSTREAM.toml" \
        || die 'unified iceoryx2 fork metadata no longer identifies the pinned v0.10.0 base'
    rg -n -Fq 'iceoryx2 = { version = "=0.10.0", path = "third_party/iceoryx2-rbt/source/iceoryx2" }' "${repo_root}/Cargo.toml" \
        || die 'Cargo.toml no longer directly pins the maintained iceoryx2 source'
    awk '
        $0 == "[[package]]" { in_iox2 = 0 }
        $0 == "name = \"iceoryx2\"" { count++; in_iox2 = 1; next }
        in_iox2 && $0 == "version = \"0.10.0\"" { version_count++ }
        in_iox2 && $0 ~ /^source = / { source_count++ }
        END { exit !(count == 1 && version_count == 1 && source_count == 0) }
    ' "${repo_root}/Cargo.lock" \
        || die 'Cargo.lock must contain only one path-sourced iceoryx2 package at 0.10.0'
    rg -n -Fq 'iceoryx2 = { path = "third_party/iceoryx2-rbt/source/iceoryx2" }' \
        "${repo_root}/Cargo.toml" \
        || die 'Cargo.toml no longer patches iceoryx2 to the unified rbt source'
    require_file "${cpp_prefix}/lib/cmake/iceoryx2-cxx/iceoryx2-cxxConfig.cmake"
    require_file "${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh"
    run_logged cpp-prefix-provenance bash \
        "${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${cpp_prefix}"
    run_logged iox2-source-only env RBT_IOX2_FORK_SOURCE="${iox2_source}" \
        "${repo_root}/scripts/bootstrap_iceoryx2_rbt.sh" --source-only
    run_logged iox2-source-fingerprint-verify env RBT_IOX2_FORK_SOURCE="${iox2_source}" \
        "${repo_root}/scripts/bootstrap_iceoryx2_rbt.sh" --verify
    {
        printf 'RBT_RPC_SOURCE_AUDIT fork=0.10.0-rbt.2 base_commit=%s source_diff_sha256=%s\n' \
            "$(git -C "${iox2_source}" rev-parse HEAD)" \
            "$(awk 'NR == 1 { print $1 }' "${repo_root}/third_party/iceoryx2-rbt/SOURCE_DIFF_SHA256")"
        printf 'Cargo.toml pin: '
        rg -n -F 'iceoryx2 = { version = "=0.10.0", path = "third_party/iceoryx2-rbt/source/iceoryx2" }' "${repo_root}/Cargo.toml"
        printf 'Cargo patch: '
        rg -n -F 'iceoryx2 = { path = "third_party/iceoryx2-rbt/source/iceoryx2" }' \
            "${repo_root}/Cargo.toml"
        printf 'Source bootstrap and strict fingerprint verification:\n'
        cat "${run_dir}/iox2-source-only.log" "${run_dir}/iox2-source-fingerprint-verify.log"
        printf 'RequestResponse sources:\n'
        printf '%s\n' \
            "${iox2_source}/iceoryx2/src/service/builder/request_response.rs" \
            "${iox2_source}/iceoryx2/src/pending_response.rs"
        printf 'RPC audit note: '
        rg -n -m1 'Current unified source identity' \
            "${repo_root}/docs/rpc-iox2-notes.md"
        printf 'status=complete\n'
    } | tee "${source_log}"
}

unsafe_audit() {
    local audit_log="${run_dir}/unsafe-audit.log"
    python3 "${repo_root}/scripts/audit_unsafe.py" --label RBT_RPC_UNSAFE_AUDIT 2>&1 \
        | tee "${audit_log}" \
        || die "production unsafe audit failed; see ${audit_log}"
}

hash_audit() {
    local hash_log="${run_dir}/hash-audit.log"
    local hash_sources=(
        "${repo_root}/crates/rbt-types/src/lib.rs"
        "${repo_root}/generated/descriptors/sensor.json"
        "${repo_root}/generated/descriptors/control.json"
        "${repo_root}/generated/descriptors/demo.json"
    )
    local expected_hashes=(
        5d2c102a797d5b0ab64b4db8844297c0
        d8e60d2ca65e6b5b74f0981d34884a4d
        943fa4008cf0f55f89a281b051c69a6f
        d9da2bb29bb2cf217c40f1a56d1fb755
        9381cf15842a48ac0964111f0f45ef72
    )
    local hash_value hash_source
    : >"${hash_log}"
    for hash_source in "${hash_sources[@]}"; do
        require_file "${hash_source}"
    done
    for hash_value in "${expected_hashes[@]}"; do
        if ! rg -Fq "${hash_value}" "${hash_sources[@]}"; then
            die "frozen layout hash is missing: ${hash_value}"
        fi
        printf 'frozen_hash=%s\n' "${hash_value}" >>"${hash_log}"
    done
    printf 'RBT_RPC_HASH_AUDIT expected=%s observed=%s status=pass\n' \
        "${#expected_hashes[@]}" "${#expected_hashes[@]}" | tee -a "${hash_log}"
}

rpc_symbol_audit() {
    local fixed="${repo_root}/crates/rbt-iox2/tests/rpc_fixed.rs"
    local dynamic="${repo_root}/crates/rbt-iox2/tests/rpc_dynamic.rs"
    local stress="${repo_root}/crates/rbt-iox2/tests/rpc_stress.rs"
    local fault="${repo_root}/crates/rbt-iox2/tests/rpc_fault.rs"
    require_test_symbol "${fixed}" preflight_rejects_request_and_response_mismatch_before_transport
    require_test_symbol "${fixed}" timeout_late_response_does_not_poison_next_call
    require_test_symbol "${fixed}" cancellation_is_typed_and_next_call_works
    require_test_symbol "${fixed}" sixteen_clients_route_markers_and_isolate_cancellation
    require_test_symbol "${dynamic}" fixed_request_slice_response_is_bounded_zero_copy
    require_test_symbol "${dynamic}" slice_request_fixed_response_is_bounded_zero_copy
    require_test_symbol "${dynamic}" slice_request_slice_response_is_bounded_zero_copy
    require_test_symbol "${dynamic}" pointcloud_slice_request_slice_response_is_bounded_zero_copy
    require_test_symbol "${stress}" thousand_timeout_late_response_reuse_cycles_have_no_in_flight_calls
    require_test_symbol "${stress}" thousand_cancellation_races_have_one_terminal_result_and_recover
    require_test_symbol "${stress}" slow_server_reports_explicit_client_capacity_and_recovers
    require_test_symbol "${stress}" fast_and_slow_clients_preserve_native_routing
    require_test_symbol "${stress}" in_flight_current
    require_test_symbol "${stress}" ClientCapacityReached
    require_test_symbol "${fault}" server_sigkill_wakes_call_and_replacement_server_works
    require_test_symbol "${fault}" client_sigkill_releases_server_request_and_fresh_client_works
    require_test_symbol "${fault}" requests_current
    local resources="${repo_root}/crates/rbt-iox2/tests/rpc_resources.rs"
    require_test_symbol "${resources}" stale_server_request_cannot_satisfy_reopened_clients_first_pending_call
    require_test_symbol "${resources}" native_stale_response_cannot_cross_client_connection_reuse
    require_test_symbol "${resources}" in_flight_limit_is_typed_and_recovers_without_server_capacity_rejection
    local cpp="${repo_root}/sdk/cpp/tests/rpc_tests.cpp"
    require_test_symbol "${cpp}" pending_move_assignment_releases_both_calls
    require_test_symbol "${cpp}" pending_move_assignment_preserves_in_flight_high_water
    require_test_symbol "${cpp}" cancelled_pending_move_assignment_records_cancelled_destination
    require_test_symbol "${cpp}" request_move_assignment_releases_both_requests
    require_test_symbol "${cpp}" response_loan_move_assignment_releases_both_requests
    require_test_symbol "${cpp}" request_loan_move_assignment_releases_capacity_and_handles_self_move
    require_test_symbol "${cpp}" client_move_assignment_expires_old_activation_and_keeps_source_working
    require_test_symbol "${cpp}" server_move_assignment_expires_old_activation_and_keeps_source_working
    printf 'RBT_RPC_COVERAGE_AUDIT fixed=abi/deadline/cancel/routing dynamic=4/image+pointcloud stress=1000x2/capacity/routing fault=sigkill-server/sigkill-client cpp_move=live-target/expiry status=pass\n' \
        | tee "${run_dir}/rpc-coverage-audit.log"
}

rust_rpc_quick() {
    run_logged rust-rpc-fixed cargo test -p rbt-iox2 --test rpc_fixed --locked -- --test-threads=1
    run_logged rust-rpc-dynamic cargo test -p rbt-iox2 --test rpc_dynamic --locked -- --test-threads=1
    run_logged rust-rpc-stress cargo test -p rbt-iox2 --test rpc_stress --locked -- --test-threads=1
    run_logged rust-rpc-fault cargo test -p rbt-iox2 --test rpc_fault --locked -- --test-threads=1
    # Native correlation regressions; they fail on unpatched iceoryx2 0.9.3.
    run_logged rust-rpc-resources cargo test --release -p rbt-iox2 --test rpc_resources --locked -- --test-threads=1
}

cpp_rpc_quick() {
    local build_dir="${RBT_RPC_VERIFY_CPP_BUILD_DIR:-${repo_root}/target/rpc-verify-cpp-debug}"
    require_file "${repo_root}/sdk/cpp/tests/rpc_tests.cpp"
    run_logged cpp-debug-configure cmake -S "${repo_root}/sdk/cpp" -B "${build_dir}" \
        -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER="${CXX}" \
        -DCMAKE_PREFIX_PATH="${cpp_prefix}"
    run_logged cpp-debug-build cmake --build "${build_dir}" --parallel
    run_logged cpp-rpc-ctest ctest --test-dir "${build_dir}" --output-on-failure \
        -R '^rpc_tests$'
}

resolve_python_rpc_tests() {
    local sync_override="${RBT_RPC_SYNC_TEST_FILE:-}"
    local async_override="${RBT_RPC_ASYNC_TEST_FILE:-}"
    local candidate
    if [[ -n "${sync_override}" ]]; then
        sync_test_file="${sync_override}"
    else
        sync_test_file=''
        for candidate in \
            "${repo_root}/python/tests/test_rpc_sync.py" \
            "${repo_root}/python/tests/test_rpc_api.py" \
            "${repo_root}/python/tests/test_rpc.py"; do
            if [[ -f "${candidate}" ]]; then
                sync_test_file="${candidate}"
                break
            fi
        done
    fi
    if [[ -n "${async_override}" ]]; then
        async_test_file="${async_override}"
    else
        async_test_file=''
        for candidate in \
            "${repo_root}/python/tests/test_rpc_async.py" \
            "${repo_root}/python/tests/test_rpc_asyncio.py" \
            "${repo_root}/python/tests/test_async_rpc.py"; do
            if [[ -f "${candidate}" ]]; then
                async_test_file="${candidate}"
                break
            fi
        done
    fi
    [[ -n "${sync_test_file}" ]] \
        || die "missing required Python sync RPC smoke artifact; set RBT_RPC_SYNC_TEST_FILE"
    [[ -f "${sync_test_file}" ]] \
        || die "Python sync RPC smoke artifact is missing: ${sync_test_file}"
    [[ -n "${async_test_file}" ]] \
        || die "missing required Python async RPC smoke artifact; set RBT_RPC_ASYNC_TEST_FILE"
    [[ -f "${async_test_file}" ]] \
        || die "Python async RPC smoke artifact is missing: ${async_test_file}"
}

python_rpc_quick() {
    resolve_python_rpc_tests
    local verify_python="${RBT_RPC_VERIFY_PYTHON:-${repo_root}/target/python-verify-venv/bin/python}"
    require_executable "${verify_python}"
    run_logged python-rpc-fresh-extension env -u CONDA_PREFIX \
        VIRTUAL_ENV="$(dirname "$(dirname "${verify_python}")")" \
        PYO3_PYTHON="${verify_python}" \
        bash -c 'cd "$1" && exec "$2" -m maturin develop --release' \
        _ "${repo_root}/python" "${verify_python}"
    run_logged python-rpc-focused "${verify_python}" -m pytest -q \
        "${sync_test_file}" \
        "${repo_root}/python/tests/test_rpc_native.py" \
        "${async_test_file}" \
        "${repo_root}/python/tests/test_rpc_dynamic.py"
    require_file "${repo_root}/scripts/check_rpc_interop_idle.py"
    require_file "${repo_root}/python/tests/test_rpc_interop_idle.py"
    run_logged rpc-interop-idle-validator "${verify_python}" -m pytest -q \
        "${repo_root}/python/tests/test_rpc_interop_idle.py"
}

cli_rpc_diagnostics() {
    require_test_symbol "${repo_root}/crates/rbt-cli/src/lib.rs" \
        rpc_info_reports_typed_metrics_without_fabricating_unavailable_zeroes
    run_logged cli-rpc-human-json cargo test -p rbt-cli --lib --locked \
        tests::rpc_info_reports_typed_metrics_without_fabricating_unavailable_zeroes -- --exact
}

workspace_quick() {
    run_logged workspace-fmt cargo fmt --all -- --check
    run_logged frozen-hash-test cargo test -p rbt-types --lib --locked \
        tests::built_in_layout_hashes_are_fixed -- --exact
    run_logged workspace-test cargo test --workspace --locked
    run_logged workspace-clippy cargo clippy --workspace --all-targets --all-features \
        --locked -- -D warnings
}

script_static_checks() {
    run_logged shell-syntax env -u LD_LIBRARY_PATH -u LIBRARY_PATH bash -c '
        set -euo pipefail
        while IFS= read -r -d "" script; do
            bash -n "${script}"
        done < <(find scripts -type f -name "*.sh" -print0)
    '
    if command -v shellcheck >/dev/null 2>&1; then
        run_logged shellcheck env -u LD_LIBRARY_PATH -u LIBRARY_PATH \
            shellcheck --format=gcc "${repo_root}/scripts/verify_rpc.sh"
    else
        {
            printf 'status=unavailable\n'
            printf 'reason=shellcheck command not found\n'
        } | tee "${run_dir}/shellcheck.log"
    fi
}

full_cpp_matrix() {
    local build_type build_key build_dir
    for build_type in Debug Release; do
        build_key="${build_type,,}"
        build_dir="${repo_root}/target/rpc-verify-cpp-${build_key}"
        run_logged "cpp-${build_key}-configure" cmake -S "${repo_root}/sdk/cpp" \
            -B "${build_dir}" -G Ninja -DCMAKE_BUILD_TYPE="${build_type}" \
            -DCMAKE_CXX_COMPILER="${CXX}" -DCMAKE_PREFIX_PATH="${cpp_prefix}"
        run_logged "cpp-${build_key}-build" cmake --build "${build_dir}" --parallel
        run_logged "cpp-${build_key}-ctest" ctest --test-dir "${build_dir}" --output-on-failure
    done

    local sanitizer sanitizer_key sanitizer_flags sanitizer_dir
    for sanitizer in address undefined; do
        sanitizer_key="${sanitizer}"
        sanitizer_dir="${repo_root}/target/rpc-verify-cpp-${sanitizer_key}"
        sanitizer_flags="-fsanitize=${sanitizer} -fno-omit-frame-pointer"
        run_logged "cpp-${sanitizer_key}-configure" cmake -S "${repo_root}/sdk/cpp" \
            -B "${sanitizer_dir}" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
            -DCMAKE_CXX_COMPILER="${CXX}" -DCMAKE_PREFIX_PATH="${cpp_prefix}" \
            "-DCMAKE_CXX_FLAGS=${sanitizer_flags}" \
            "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=${sanitizer}"
        run_logged "cpp-${sanitizer_key}-build" cmake --build "${sanitizer_dir}" --parallel
        if [[ "${sanitizer}" == address ]]; then
            run_logged "cpp-${sanitizer_key}-ctest" env \
                ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
                ctest --test-dir "${sanitizer_dir}" --output-on-failure
        else
            run_logged "cpp-${sanitizer_key}-ctest" env \
                UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
                ctest --test-dir "${sanitizer_dir}" --output-on-failure
        fi
    done
}

full_rpc_rust() {
    run_logged rust-rpc-all cargo test -p rbt-iox2 --locked -- --test-threads=1
    run_logged rust-rpc-timeout-1000 cargo test -p rbt-iox2 --test rpc_stress --locked \
        thousand_timeout_late_response_reuse_cycles_have_no_in_flight_calls -- --exact --test-threads=1
    run_logged rust-rpc-cancel-1000 cargo test -p rbt-iox2 --test rpc_stress --locked \
        thousand_cancellation_races_have_one_terminal_result_and_recover -- --exact --test-threads=1
    run_logged rust-rpc-process-death cargo test -p rbt-iox2 --test rpc_fault --locked \
        -- --test-threads=1
    require_test_symbol "${repo_root}/crates/rbt-compose/src/component.rs" \
        rpc_provider_replacement_stress_keeps_prepared_client_and_rotates_lifecycle
    run_logged rust-rpc-replacement-100 cargo test -p rbt-compose --lib --locked \
        component::tests::rpc_provider_replacement_stress_keeps_prepared_client_and_rotates_lifecycle \
        -- --exact --test-threads=1
}

full_required_artifact() {
    local value="$1"
    local description="$2"
    [[ -n "${value}" ]] || die "missing required artifact for ${description}"
    [[ -x "${value}" ]] || die "required ${description} is missing or not executable: ${value}"
}

full_rpc_external() {
    local interop_script="${RBT_RPC_INTEROP_SCRIPT:-${repo_root}/scripts/test_rpc_interop.sh}"
    local soak_script="${RBT_RPC_SOAK_SCRIPT:-${repo_root}/scripts/soak_rpc.sh}"
    local soak_seconds="${RBT_RPC_SOAK_SECONDS:-300}"
    [[ "${soak_seconds}" =~ ^[1-9][0-9]*$ ]] \
        || die "RBT_RPC_SOAK_SECONDS must be a positive integer: ${soak_seconds}"
    full_required_artifact "${interop_script}" 'RPC cross-language matrix'
    full_required_artifact "${soak_script}" 'RPC 300-second soak'
    run_logged rpc-cross-language-interop "${interop_script}"
    run_logged rpc-300s-soak env RBT_RPC_SOAK_SECONDS="${soak_seconds}" "${soak_script}"

    require_file "${repo_root}/scripts/verify_phase4.sh"
    run_logged inherited-interop ./scripts/verify_phase4.sh
    require_file "${repo_root}/scripts/test_mixed_runtime.sh"
    run_logged composer-restart ./scripts/test_mixed_runtime.sh
    run_logged packaging-external-consumers ./scripts/test_install.sh
    run_logged inherited-dx env RBT_DX_SKIP_INHERITED=1 ./scripts/verify_dx.sh --full
    run_logged inherited-v01 ./scripts/verify_v01.sh --full
    run_logged inherited-pressure env RBT_PRESSURE_TUNING_DEDUP_LONG_SOAKS=1 \
        ./scripts/verify_pressure_tuning.sh --full
    run_logged inherited-waitset env RBT_WAITSET_SKIP_V01=1 ./scripts/verify_waitset.sh --full
    run_logged inherited-mcap env RBT_MCAP_SKIP_INHERITED=1 ./scripts/verify_mcap.sh --full
    run_logged inherited-python ./scripts/verify_python.sh --full
    run_logged inherited-python-asyncio env RBT_PYTHON_ASYNCIO_SKIP_INHERITED=1 \
        ./scripts/verify_python_asyncio.sh --full
}

full_benchmark() {
    run_logged rpc-benchmark env RBT_RPC_BENCH_LOG_DIR="${run_dir}/benchmark" \
        ./scripts/benchmark_rpc.sh --include-dynamic-20m \
        --count "${RBT_RPC_BENCH_COUNT:-20}" --warmup "${RBT_RPC_BENCH_WARMUP:-3}"
}

printf 'RBT_RPC_VERIFY mode=%s run_id=%s logs=%s toolchain=%s cpp_prefix=%s\n' \
    "${mode}" "${run_id}" "${run_dir}" "${toolchain_root}" "${cpp_prefix}" \
    | tee "${run_dir}/environment.log"

source_audit
rpc_symbol_audit
hash_audit
unsafe_audit
script_static_checks
rust_rpc_quick
cpp_rpc_quick
python_rpc_quick
cli_rpc_diagnostics
workspace_quick

if [[ "${mode}" == full ]]; then
    full_cpp_matrix
    full_rpc_rust
    run_logged python-sync-full ./scripts/verify_python.sh --full
    run_logged python-async-full ./scripts/verify_python_asyncio.sh --full
    full_benchmark
    full_rpc_external
fi

printf 'PASS: RPC %s verification completed; logs=%s\n' "${mode}" "${run_dir}"

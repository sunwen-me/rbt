#!/usr/bin/env bash
set -euo pipefail

# Run the fixed-message native RPC matrix as separate processes.  Each lane is
# described by a server/client language pair, so future asynchronous probes can
# be added to run_case without changing process supervision or cleanup.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

die() {
    printf 'RBT_RPC_INTEROP FAIL harness=%s\n' "$*" >&2
    exit 1
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

for command_name in bash cargo cmake date mktemp rg sleep timeout; do
    require_command "${command_name}"
done

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
toolchain_bin="${toolchain_root}/bin"
cpp_prefix="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"
"${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${cpp_prefix}"
cpp_build="${RBT_RPC_INTEROP_CPP_BUILD:-${repo_root}/target/rpc-interop-cpp}"
log_root="${RBT_RPC_INTEROP_LOG_DIR:-${repo_root}/target/rpc-interop-logs}"
python_probe="${RBT_RPC_INTEROP_PYTHON_PROBE:-${repo_root}/python/tests/rpc_interop_probe.py}"
calls="${RBT_RPC_INTEROP_CALLS:-4}"
watchdog_seconds="${RBT_RPC_INTEROP_WATCHDOG_SECONDS:-15}"

[[ "${calls}" =~ ^[1-9][0-9]*$ ]] || die "calls must be a positive integer"
((calls <= 1000000)) || die "calls must not exceed 1000000"
[[ "${watchdog_seconds}" =~ ^[1-9][0-9]*$ ]] \
    || die "watchdog seconds must be a positive integer"
[[ -f "${python_probe}" ]] || die "Python probe is missing: ${python_probe}"

if [[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-g++" ]]; then
    cpp_compiler="${toolchain_bin}/x86_64-conda-linux-gnu-g++"
else
    if [[ -n "${CXX:-}" ]]; then
        cpp_compiler="$(command -v "${CXX}" 2>/dev/null || printf '%s' "${CXX}")"
    else
        cpp_compiler="$(command -v c++ || true)"
    fi
fi
[[ -x "${cpp_compiler}" ]] || die "C++ compiler is missing: ${cpp_compiler}"
[[ -f "${cpp_prefix}/lib/cmake/iceoryx2-cxx/iceoryx2-cxxConfig.cmake" ]] \
    || die "iceoryx2 C++ package config is missing under ${cpp_prefix}"

if [[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-gcc" ]]; then
    export PATH="${toolchain_bin}:${PATH}"
    export CC="${toolchain_bin}/x86_64-conda-linux-gnu-gcc"
    export AR="${toolchain_bin}/x86_64-conda-linux-gnu-ar"
    export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
else
    export CC="${CC:-$(command -v cc || command -v gcc || true)}"
fi
[[ -n "${CC}" ]] || die "C compiler is missing"
export CXX="${cpp_compiler}"
export CMAKE_PREFIX_PATH="${cpp_prefix}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"
if [[ -d "${toolchain_root}/lib" ]]; then
    export LD_LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
else
    export LD_LIBRARY_PATH="${cpp_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
fi

if [[ -n "${RBT_RPC_INTEROP_RUST_BIN:-}" ]]; then
    rust_bin="${RBT_RPC_INTEROP_RUST_BIN}"
else
    rust_bin="${repo_root}/target/debug/rbt-rpc-interop"
fi

if [[ -n "${RBT_RPC_INTEROP_PYTHON:-}" ]]; then
    interop_python="${RBT_RPC_INTEROP_PYTHON}"
elif [[ -x "${repo_root}/target/python-verify-venv/bin/python" ]]; then
    interop_python="${repo_root}/target/python-verify-venv/bin/python"
elif [[ -x "/home/sw/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3" ]]; then
    interop_python="/home/sw/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3"
else
    interop_python="$(command -v python3 || true)"
fi
[[ -x "${interop_python}" ]] || die "Python interpreter is missing: ${interop_python}"
export PYTHONPATH="${repo_root}/python${PYTHONPATH:+:${PYTHONPATH}}"

mkdir -p "${log_root}"
run_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="${log_root}/${run_id}"
mkdir -p "${run_dir}"
tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/rbt-rpc-interop.XXXXXX")"
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
    # tmp_root is a unique mktemp directory owned by this run; staged Python
    # extension symlinks live below it when an existing native build is used.
    rm -rf -- "${tmp_root}"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

stage_existing_python_extension() {
    local candidate candidate_python candidate_suffix candidate_root
    local candidate_index=0
    local -a python_candidates=("${interop_python}")
    local -a extension_candidates=()

    if [[ -x "/home/sw/miniforge3/bin/python3" \
        && "/home/sw/miniforge3/bin/python3" != "${interop_python}" ]]; then
        python_candidates+=("/home/sw/miniforge3/bin/python3")
    fi
    shopt -s nullglob
    extension_candidates=("${repo_root}"/target/rbt-python-native*/debug/lib_rbt.so)
    shopt -u nullglob
    for candidate in "${extension_candidates[@]}"; do
        for candidate_python in "${python_candidates[@]}"; do
            candidate_suffix="$(${candidate_python} -c \
                'import sysconfig; print(sysconfig.get_config_var("EXT_SUFFIX"))')"
            candidate_root="${tmp_root}/python-extension-${candidate_index}"
            candidate_index=$((candidate_index + 1))
            mkdir -p "${candidate_root}/rbt"
            ln -s "${repo_root}/python/rbt/__init__.py" \
                "${candidate_root}/rbt/__init__.py"
            ln -s "${repo_root}/python/rbt/_generated" \
                "${candidate_root}/rbt/_generated"
            ln -s "${candidate}" "${candidate_root}/rbt/_rbt${candidate_suffix}"
            if PYTHONPATH="${candidate_root}:${repo_root}/python" \
                "${candidate_python}" -c \
                'import inspect, rbt; assert hasattr(rbt._rbt, "_RpcClient") and hasattr(rbt._rbt, "_RpcServer"); assert "element_count" in str(inspect.signature(rbt._rbt._RpcClient.loan_request))' \
                >/dev/null 2>&1; then
                interop_python="${candidate_python}"
                export PYTHONPATH="${candidate_root}:${repo_root}/python${PYTHONPATH:+:${PYTHONPATH}}"
                printf 'RBT_RPC_INTEROP BUILD python_extension=current path=%s interpreter=%s\n' \
                    "${candidate}" "${interop_python}"
                return 0
            fi
        done
    done
    return 1
}

wait_ready() {
    local log_file="$1"
    local pid="$2"
    local label="$3"
    local deadline=$((SECONDS + watchdog_seconds))
    while ((SECONDS < deadline)); do
        if rg -Fq 'RBT_RPC_INTEROP READY' "${log_file}"; then
            return 0
        fi
        if ! kill -0 "${pid}" 2>/dev/null; then
            cat "${log_file}" >&2 || true
            die "${label} server exited before READY"
        fi
        sleep 0.05
    done
    cat "${log_file}" >&2 || true
    die "${label} server did not emit READY within ${watchdog_seconds}s"
}

wait_for_exit() {
    local pid="$1"
    local label="$2"
    local deadline=$((SECONDS + watchdog_seconds))
    while kill -0 "${pid}" 2>/dev/null; do
        if ((SECONDS >= deadline)); then
            kill -TERM "${pid}" 2>/dev/null || true
            wait "${pid}" 2>/dev/null || true
            die "${label} server watchdog expired"
        fi
        sleep 0.05
    done
    wait "${pid}"
}

run_case() {
    local label="$1"
    local server_language="$2"
    local client_language="$3"
    local server_role="${4:-server}"
    local client_role="${5:-client}"
    local case_calls="${6:-${calls}}"
    local quiet_logs="${RBT_RPC_INTEROP_QUIET:-0}"
    local service="rbt/rpc/interop/${run_id}/${label}"
    local server_log="${run_dir}/${label}.server.log"
    local client_log="${run_dir}/${label}.client.log"
    local server_pid server_status client_status
    local -a server_cmd client_cmd

    [[ "${case_calls}" =~ ^[1-9][0-9]*$ ]] \
        || die "${label} calls must be a positive integer"
    ((case_calls <= 1000000)) || die "${label} calls must not exceed 1000000"
    if ((case_calls > 100)); then
        quiet_logs=1
    fi

    case "${server_language}" in
        rust) server_cmd=("${rust_bin}" "${server_role}" "${service}" "${case_calls}") ;;
        cpp) server_cmd=("${cpp_binary}" "${server_role}" "${service}" "${case_calls}") ;;
        python) server_cmd=("${interop_python}" "${python_probe}" "${server_role}" "${service}" "${case_calls}") ;;
        *) die "unsupported server language ${server_language}" ;;
    esac
    case "${client_language}" in
        rust) client_cmd=("${rust_bin}" "${client_role}" "${service}" "${case_calls}") ;;
        cpp) client_cmd=("${cpp_binary}" "${client_role}" "${service}" "${case_calls}") ;;
        python) client_cmd=("${interop_python}" "${python_probe}" "${client_role}" "${service}" "${case_calls}") ;;
        *) die "unsupported client language ${client_language}" ;;
    esac

    printf 'RBT_RPC_INTEROP CASE label=%s server=%s client=%s service=%s\n' \
        "${label}" "${server_language}" "${client_language}" "${service}"
    RBT_RPC_INTEROP_QUIET="${quiet_logs}" "${server_cmd[@]}" >"${server_log}" 2>&1 &
    server_pid=$!
    child_pids+=("${server_pid}")
    wait_ready "${server_log}" "${server_pid}" "${label}"

    set +e
        RBT_RPC_INTEROP_QUIET="${quiet_logs}" timeout --foreground "${watchdog_seconds}s" \
        "${client_cmd[@]}" >"${client_log}" 2>&1
    client_status=$?
    set -e
    if ((client_status != 0)); then
        cat "${client_log}" >&2 || true
        kill -TERM "${server_pid}" 2>/dev/null || true
        wait "${server_pid}" 2>/dev/null || true
        die "${label} client failed with exit ${client_status}"
    fi

    set +e
    wait_for_exit "${server_pid}" "${label}"
    server_status=$?
    set -e
    cat "${server_log}"
    cat "${client_log}"
    ((server_status == 0)) || die "${label} server failed with exit ${server_status}"
    "${interop_python}" "${repo_root}/scripts/check_rpc_interop_idle.py" \
        --log "${server_log}" --language "${server_language}" \
        --role "${server_role}" --service "${service}" --calls "${case_calls}" \
        || die "${label} server did not report valid final idle metrics"
    "${interop_python}" "${repo_root}/scripts/check_rpc_interop_idle.py" \
        --log "${client_log}" --language "${client_language}" \
        --role "${client_role}" --service "${service}" --calls "${case_calls}" \
        || die "${label} client did not report valid final idle metrics"
    rg -Fq 'RBT_RPC_INTEROP PASS' "${server_log}" \
        || die "${label} server did not report PASS"
    rg -Fq 'RBT_RPC_INTEROP PASS' "${client_log}" \
        || die "${label} client did not report PASS"
    printf 'RBT_RPC_INTEROP CASE_PASS label=%s\n' "${label}"
}

printf 'RBT_RPC_INTEROP BUILD rust package=rbt-rpc-interop\n'
cargo fmt --package rbt-rpc-interop -- --check
cargo check --locked -p rbt-rpc-interop
cargo build --locked -p rbt-rpc-interop
[[ -x "${rust_bin}" ]] || die "Rust interop binary is missing: ${rust_bin}"

printf 'RBT_RPC_INTEROP BUILD cpp target=rpc_interop\n'
cmake -S "${repo_root}/sdk/cpp" -B "${cpp_build}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER="${CXX}" \
    -DCMAKE_PREFIX_PATH="${cpp_prefix}"
cmake --build "${cpp_build}" --target rpc_interop --parallel
cpp_binary="${cpp_build}/rpc_interop"
[[ -x "${cpp_binary}" ]] || die "C++ interop binary is missing: ${cpp_binary}"

printf 'RBT_RPC_INTEROP BUILD python probe=%s interpreter=%s\n' \
    "${python_probe}" "${interop_python}"
"${interop_python}" -c \
    'import pathlib, sys; compile(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"), sys.argv[1], "exec")' \
    "${python_probe}"
if ! "${interop_python}" -c \
    'import inspect, rbt; assert hasattr(rbt._rbt, "_RpcClient") and hasattr(rbt._rbt, "_RpcServer"); assert "element_count" in str(inspect.signature(rbt._rbt._RpcClient.loan_request))' \
    >/dev/null 2>&1; then
    if ! stage_existing_python_extension \
        && "${interop_python}" -m maturin --version >/dev/null 2>&1; then
        printf 'RBT_RPC_INTEROP BUILD python_extension=missing action=maturin_develop\n'
        (
            cd "${repo_root}/python"
            export VIRTUAL_ENV="$(dirname "$(dirname "${interop_python}")")"
            export PATH="$(dirname "${interop_python}"):${PATH}"
            PYO3_PYTHON="${interop_python}" "${interop_python}" -m maturin develop --release
        )
    elif ! "${interop_python}" -c \
        'import inspect, rbt; assert hasattr(rbt._rbt, "_RpcClient") and hasattr(rbt._rbt, "_RpcServer"); assert "element_count" in str(inspect.signature(rbt._rbt._RpcClient.loan_request))' \
        >/dev/null 2>&1; then
        die "Python rbt native RPC extension is unavailable; set RBT_RPC_INTEROP_PYTHON or build it with maturin"
    fi
fi

run_case rust-client-cpp-server cpp rust
run_case cpp-client-rust-server rust cpp
run_case rust-client-python-server python rust
run_case python-client-rust-server rust python
run_case python-async-client-cpp-server cpp python server async-client
run_case cpp-client-python-async-server python cpp async-server client
run_case rust-dynamic-client-cpp-dynamic-server \
    cpp rust dynamic-server dynamic-client
run_case python-async-dynamic-client-rust-dynamic-server \
    rust python dynamic-server async-dynamic-client
run_case rust-burst-client-cpp-burst-server \
    cpp rust burst-server burst-client 4096
run_case cpp-burst-client-rust-burst-server \
    rust cpp burst-server burst-client 4096

printf 'RBT_RPC_INTEROP PASS directions=10 dynamic=2 asyncio=3 burst_calls=4096 logs=%s\n' "${run_dir}"


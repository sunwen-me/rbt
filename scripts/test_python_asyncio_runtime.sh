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
usage: scripts/test_python_asyncio_runtime.sh [--quick|--full]

Run the existing mixed Python Component graph with the asyncio example and
probe, then exercise the asyncio-specific cancellation boundaries.

Environment overrides:
  RBT_PYTHON_ASYNCIO_PYTHON       Python interpreter with rbt installed
  RBT_PYTHON_ASYNCIO_EXAMPLE      asyncio detector example
  RBT_PYTHON_ASYNCIO_PROBE        asyncio runtime probe
  RBT_PYTHON_ASYNCIO_CPP_BUILD    C++ fixture build directory
  RBT_PYTHON_ASYNCIO_LOG_DIR      retained log directory
  RBT_PYTHON_ASYNCIO_TIMEOUT_SECONDS (default 25)
  RBT_PYTHON_ASYNCIO_SOAK_SECONDS (default 300 in --full)
  RBT_PYTHON_ASYNCIO_IDLE_SECONDS (default 10 in --full)
  RBT_PYTHON_ASYNCIO_PORT         Composer port
  RBT_PYTHON_ASYNCIO_PROCESS_CWD  working directory for spawned graph processes
  RBT_SKIP_BUILD=1                reuse existing Composer/fixture binaries
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

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
cpp_prefix="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"
"${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${cpp_prefix}"
cpp_build="${RBT_PYTHON_ASYNCIO_CPP_BUILD:-${repo_root}/target/python-cpp-build}"
python="${RBT_PYTHON_ASYNCIO_PYTHON:-${repo_root}/target/python-verify-venv/bin/python}"
detector_script="${RBT_PYTHON_ASYNCIO_EXAMPLE:-${repo_root}/examples/python/async_component_detector.py}"
probe_script="${RBT_PYTHON_ASYNCIO_PROBE:-${repo_root}/python/tests/async_component_runtime_probe.py}"
timeout_seconds="${RBT_PYTHON_ASYNCIO_TIMEOUT_SECONDS:-25}"
soak_seconds="${RBT_PYTHON_ASYNCIO_SOAK_SECONDS:-${RBT_PYTHON_COMPONENT_SOAK_SECONDS:-300}}"
idle_seconds="${RBT_PYTHON_ASYNCIO_IDLE_SECONDS:-10}"
port="${RBT_PYTHON_ASYNCIO_PORT:-$((18000 + $$ % 1000))}"
composer_addr="127.0.0.1:${port}"
log_root="${RBT_PYTHON_ASYNCIO_LOG_DIR:-${repo_root}/target/python-asyncio-logs}"
process_cwd="${RBT_PYTHON_ASYNCIO_PROCESS_CWD:-${repo_root}}"
mkdir -p "${log_root}"
run_dir="$(mktemp -d "${log_root}/runtime-${mode}.XXXXXX")"
harness_log_root="${run_dir}/component-harness"
probe_log_root="${run_dir}/async-probes"
mkdir -p "${harness_log_root}" "${probe_log_root}"

[[ "${timeout_seconds}" =~ ^[1-9][0-9]*$ ]] || die "timeout must be positive"
[[ "${soak_seconds}" =~ ^[1-9][0-9]*$ ]] || die "soak duration must be positive"
[[ "${idle_seconds}" =~ ^[1-9][0-9]*$ ]] || die "idle duration must be positive"
[[ "${port}" =~ ^[1-9][0-9]*$ ]] || die "Composer port must be positive"
[[ -x "${python}" ]] || die "Python interpreter is missing: ${python}"
[[ -f "${detector_script}" ]] || die "async detector example is missing: ${detector_script}"
[[ -f "${probe_script}" ]] || die "async runtime probe is missing: ${probe_script}"
[[ -d "${process_cwd}" ]] || die "process working directory is missing: ${process_cwd}"
for command_name in cargo cmake jq rg awk diff find sed timeout getconf; do
    command -v "${command_name}" >/dev/null 2>&1 \
        || die "required command not found: ${command_name}"
done

export PATH="${toolchain_root}/bin${PATH:+:${PATH}}"
export CC="${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc"
export CXX="${toolchain_root}/bin/x86_64-conda-linux-gnu-g++"
export AR="${toolchain_root}/bin/x86_64-conda-linux-gnu-ar"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
export LD_LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"
export CMAKE_PREFIX_PATH="${cpp_prefix}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"

if [[ "${RBT_SKIP_BUILD:-0}" != 1 ]]; then
    cargo build -p composer -p rbt-cli -p camera-a -p camera-b
    cmake -S "${repo_root}/sdk/cpp" -B "${cpp_build}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER="${CXX}" \
        -DCMAKE_PREFIX_PATH="${cpp_prefix}"
    cmake --build "${cpp_build}" --target python_component_viewer --parallel 2
fi

composer="${repo_root}/target/debug/composer"
rbt_cli="${repo_root}/target/debug/rbt"
camera_a="${repo_root}/target/debug/camera-a"
camera_b="${repo_root}/target/debug/camera-b"
cpp_viewer="${cpp_build}/python_component_viewer"
for binary in "${composer}" "${rbt_cli}" "${camera_a}" "${camera_b}" "${cpp_viewer}"; do
    [[ -x "${binary}" ]] || die "required binary is missing: ${binary}"
done
"${python}" -c \
    'import asyncio, rbt; assert hasattr(rbt, "run_component"); assert hasattr(rbt, "AsyncTaskHandle")' \
    || die "selected Python does not have the asyncio Component runtime"

run_logged() {
    local label="$1"
    shift
    local log_file="${run_dir}/${label}.log"
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    set +e
    "$@" >"${log_file}" 2>&1
    local status=$?
    set -e
    cat "${log_file}"
    ((status == 0)) || die "${label} failed with exit ${status}; see ${log_file}"
}

# The synchronous harness already owns the complete lifecycle graph, resource
# checks, Composer recovery, and full 100-cycle/soak scenario.  Substitute the
# asyncio example and probe so those existing assertions exercise this adapter.
harness_args=(--quick)
[[ "${mode}" == full ]] && harness_args=(--full)
harness_port="$((port + 1))"
run_logged component-runtime-harness env -u PYTHONPATH \
    RBT_SKIP_BUILD=1 \
    RBT_PYTHON_COMPONENT_PYTHON="${python}" \
    RBT_PYTHON_COMPONENT_EXAMPLE="${detector_script}" \
    RBT_PYTHON_COMPONENT_PROBE="${probe_script}" \
    RBT_PYTHON_COMPONENT_CPP_BUILD="${cpp_build}" \
    RBT_PYTHON_COMPONENT_TIMEOUT_SECONDS="${timeout_seconds}" \
    RBT_PYTHON_COMPONENT_SOAK_SECONDS="${soak_seconds}" \
    RBT_PYTHON_COMPONENT_PORT="${harness_port}" \
    RBT_PYTHON_COMPONENT_LOG_DIR="${harness_log_root}" \
    RBT_PYTHON_COMPONENT_PROCESS_CWD="${process_cwd}" \
    ./scripts/test_python_component_runtime.sh "${harness_args[@]}"

harness_dir="$(find "${harness_log_root}" -mindepth 1 -maxdepth 1 -type d \
    -name "runtime-${mode}.*" -print -quit)"
[[ -n "${harness_dir}" ]] || die "existing runtime harness did not retain a run directory"
async_detector_log="${harness_dir}/detector.log"
[[ -f "${async_detector_log}" ]] || die "async detector log is missing from the harness"
evidence_line="$(rg -F 'DETECTOR ZERO-COPY EVIDENCE' "${async_detector_log}" | head -1 || true)"
[[ -n "${evidence_line}" ]] || die "async detector did not emit zero-copy evidence"
for field in input_pointer input_numpy_pointer loan_pointer loan_numpy_pointer; do
    value="$(sed -n "s/.*${field}=\([0-9][0-9]*\).*/\1/p" <<<"${evidence_line}")"
    [[ -n "${value}" ]] || die "async zero-copy evidence omitted ${field}"
    printf '%s=%s\n' "${field}" "${value}" >>"${probe_log_root}/zero-copy-evidence.txt"
done
input_pointer="$(sed -n 's/.*input_pointer=\([0-9][0-9]*\).*/\1/p' <<<"${evidence_line}")"
input_numpy_pointer="$(sed -n 's/.*input_numpy_pointer=\([0-9][0-9]*\).*/\1/p' <<<"${evidence_line}")"
loan_pointer="$(sed -n 's/.*loan_pointer=\([0-9][0-9]*\).*/\1/p' <<<"${evidence_line}")"
loan_numpy_pointer="$(sed -n 's/.*loan_numpy_pointer=\([0-9][0-9]*\).*/\1/p' <<<"${evidence_line}")"
[[ "${input_pointer}" == "${input_numpy_pointer}" ]] \
    || die "async received NumPy view changed the payload pointer"
[[ "${loan_pointer}" == "${loan_numpy_pointer}" ]] \
    || die "async output NumPy view changed the loan pointer"
rg -F --quiet \
    'input_OWNDATA=False input_WRITEABLE=False' "${async_detector_log}" \
    || die "async received view ownership flags changed"
rg -F --quiet \
    'loan_OWNDATA=False loan_WRITEABLE=True' "${async_detector_log}" \
    || die "async writable loan ownership flags changed"
printf 'RBT_PYTHON_ASYNCIO_ZERO_COPY input_pointer=%s loan_pointer=%s\n' \
    "${input_pointer}" "${loan_pointer}"

# A fresh milestone wheel must expose readiness-only diagnostics.  Select a
# post-wait sample rather than the possible initial fast-path sample.
async_wait_line="$(rg -F 'DETECTOR ASYNC WAIT' "${async_detector_log}" \
    | rg 'wait_stats_supported=1' | tail -1 || true)"
[[ -n "${async_wait_line}" ]] || die "async detector emitted no native readiness evidence"
wait_stats_supported="$(sed -n 's/.*wait_stats_supported=\([01]\).*/\1/p' <<<"${async_wait_line}")"
readiness_events="$(sed -n 's/.*native_readiness_events=\([0-9][0-9]*\).*/\1/p' <<<"${async_wait_line}")"
readiness_callbacks="$(sed -n 's/.*scheduled_readiness_callbacks=\([0-9][0-9]*\).*/\1/p' <<<"${async_wait_line}")"
readiness_latency_ns="$(sed -n 's/.* readiness_latency_ns=\(-\?[0-9][0-9]*\) readiness_continuation.*/\1/p' <<<"${async_wait_line}")"
[[ "${wait_stats_supported}" == 1 && -n "${readiness_events}" \
    && -n "${readiness_callbacks}" && -n "${readiness_latency_ns}" ]] \
    || die "async readiness marker omitted diagnostic fields"
((readiness_events > 0 && readiness_callbacks > 0)) \
    || die "async readiness marker reported no native wake/callback"
printf 'RBT_PYTHON_ASYNCIO_READINESS supported=%s events=%s callbacks=%s latency_ns=%s\n' \
    "${wait_stats_supported}" "${readiness_events}" \
    "${readiness_callbacks}" "${readiness_latency_ns}"

async_loop_line="$(rg -F 'DETECTOR ASYNC LOOP IDENTITY' "${async_detector_log}" | head -1 || true)"
[[ -n "${async_loop_line}" ]] || die "async detector emitted no managed-loop evidence"
loop_thread_count="$(sed -n 's/.*managed_loop_thread_count=\([0-9][0-9]*\).*/\1/p' <<<"${async_loop_line}")"
bridge_thread_count="$(sed -n 's/.*wait_bridge_thread_count=\([0-9][0-9]*\).*/\1/p' <<<"${async_loop_line}")"
[[ "${loop_thread_count}" == 1 ]] || die "asyncio did not own exactly one managed loop thread"
[[ "${bridge_thread_count}" == 1 ]] || die "async activation did not own exactly one WaitSet bridge thread"
loop_id_count="$(sed -n 's/.* managed_loop_id=\([0-9][0-9]*\).*/\1/p' "${async_detector_log}" \
    | sort -u | wc -l)"
native_loop_thread_id_count="$(sed -n 's/.* native_loop_thread_id=\([0-9][0-9]*\).*/\1/p' \
    "${async_detector_log}" | sort -u | wc -l)"
[[ "${loop_id_count}" == 1 && "${native_loop_thread_id_count}" == 1 ]] \
    || die "managed asyncio loop identity changed across replacement/recovery"
task_start_count="$(rg -F --count 'DETECTOR ASYNC TASK START' "${async_detector_log}" || true)"
task_finally_count="$(rg -F --count 'DETECTOR ASYNC TASK FINALLY' "${async_detector_log}" || true)"
[[ "${task_start_count}" =~ ^[0-9]+$ && "${task_finally_count}" =~ ^[0-9]+$ ]] \
    || die "async task markers were not countable"
((task_finally_count == task_start_count)) \
    || die "async task cleanup markers are incomplete"
printf 'RBT_PYTHON_ASYNCIO_TASKS managed_loop_threads=%s bridge_threads=%s starts=%s finally=%s\n' \
    "${loop_thread_count}" "${bridge_thread_count}" \
    "${task_start_count}" "${task_finally_count}"

if [[ "${mode}" == full ]]; then
    cycles_detector_log="${harness_dir}/cycles-detector.log"
    [[ -f "${cycles_detector_log}" ]] || die "100-cycle async detector log is missing"
    rg -F --quiet 'RETAINED INPUT VIEW VALID' "${cycles_detector_log}" \
        || die "retained async input view did not survive activation replacement"
    cycle_loop_ids="$(sed -n 's/.* managed_loop_id=\([0-9][0-9]*\).*/\1/p' \
        "${cycles_detector_log}" | sort -u | wc -l)"
    cycle_native_loop_threads="$(sed -n 's/.* native_loop_thread_id=\([0-9][0-9]*\).*/\1/p' \
        "${cycles_detector_log}" | sort -u | wc -l)"
    cycle_bad_loop_counts="$(awk '
        /DETECTOR ASYNC LOOP IDENTITY/ \
            && $0 !~ /managed_loop_thread_count=1 .*wait_bridge_thread_count=1/ \
            { count++ }
        END { print count + 0 }
    ' "${cycles_detector_log}")"
    [[ "${cycle_loop_ids}" == 1 && "${cycle_native_loop_threads}" == 1 \
        && "${cycle_bad_loop_counts}" == 0 ]] \
        || die "managed loop/bridge ownership changed during 100-cycle soak"
    cycle_task_starts="$(rg -F --count 'DETECTOR ASYNC TASK START' "${cycles_detector_log}" || true)"
    cycle_task_finally="$(rg -F --count 'DETECTOR ASYNC TASK FINALLY' "${cycles_detector_log}" || true)"
    [[ "${cycle_task_starts}" == "${cycle_task_finally}" ]] \
        || die "100-cycle soak leaked a Python asyncio task"
    cycle_zero_task_lines="$(rg -F --count 'active_adapter_tasks=0' "${cycles_detector_log}" || true)"
    ((cycle_zero_task_lines >= 100)) \
        || die "100-cycle soak did not drain tasks after each activation"

    latency_values="${probe_log_root}/readiness-latency-ns.txt"
    sed -n 's/.* readiness_latency_ns=\([0-9][0-9]*\) readiness_continuation.*/\1/p' \
        "${cycles_detector_log}" | awk '$1 >= 0 { print $1 }' | sort -n >"${latency_values}"
    latency_count="$(wc -l <"${latency_values}")"
    ((latency_count >= 200)) \
        || die "full soak recorded fewer than 200 readiness-continuation samples"
    percentile_value() {
        local percentile="$1"
        local rank=$(((latency_count * percentile + 99) / 100))
        sed -n "${rank}p" "${latency_values}"
    }
    latency_p50_ns="$(percentile_value 50)"
    latency_p90_ns="$(percentile_value 90)"
    latency_p99_ns="$(percentile_value 99)"
    latency_max_ns="$(tail -1 "${latency_values}")"
    printf 'RBT_PYTHON_ASYNCIO_WAKE_LATENCY samples=%s p50_ns=%s p90_ns=%s p99_ns=%s max_ns=%s\n' \
        "${latency_count}" "${latency_p50_ns}" "${latency_p90_ns}" \
        "${latency_p99_ns}" "${latency_max_ns}"
    printf 'RBT_PYTHON_ASYNCIO_100_CYCLE_TASKS starts=%s finally=%s zero_task_drains=%s loop_ids=1 native_loop_threads=1\n' \
        "${cycle_task_starts}" "${cycle_task_finally}" "${cycle_zero_task_lines}"
fi

declare -a child_pids=()
last_pid=''
last_status=0
composer_pid=''

cleanup() {
    local pid
    for pid in "${child_pids[@]:-}"; do
        [[ -n "${pid}" ]] || continue
        if kill -0 "${pid}" 2>/dev/null; then
            kill -CONT "${pid}" 2>/dev/null || true
            kill -TERM "${pid}" 2>/dev/null || true
        fi
    done
    for pid in "${child_pids[@]:-}"; do
        [[ -n "${pid}" ]] || continue
        wait "${pid}" 2>/dev/null || true
    done
}
trap cleanup EXIT
trap 'exit 130' INT TERM

start_process() {
    local label="$1"
    shift
    printf '+'
    printf ' %q' "$@"
    printf ' > %s/%s.log\n' "${probe_log_root}" "${label}"
    (
        cd "${process_cwd}"
        exec "$@"
    ) >"${probe_log_root}/${label}.log" 2>&1 &
    last_pid="$!"
    child_pids+=("${last_pid}")
}

stop_process() {
    local signal="$1"
    local pid="$2"
    local deadline=$((SECONDS + timeout_seconds))
    if kill -0 "${pid}" 2>/dev/null; then
        kill "-${signal}" "${pid}" 2>/dev/null || true
    fi
    while kill -0 "${pid}" 2>/dev/null; do
        local state
        state="$(awk '{print $3}' "/proc/${pid}/stat" 2>/dev/null || true)"
        [[ -z "${state}" || "${state}" == Z ]] && break
        ((SECONDS < deadline)) || die "${signal} did not stop pid ${pid}"
        sleep 0.05
    done
    set +e
    wait "${pid}" 2>/dev/null
    last_status=$?
    set -e
}

ps_json() {
    RBT_COMPOSER_ADDR="${composer_addr}" "${rbt_cli}" ps --json
}

wait_ps_query() {
    local description="$1"
    local query="$2"
    local output="${3:-${probe_log_root}/current.ps.json}"
    local deadline=$((SECONDS + timeout_seconds))
    while ((SECONDS < deadline)); do
        if ps_json >"${output}" 2>/dev/null \
            && jq -e "${query}" "${output}" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    [[ -f "${output}" ]] && jq . "${output}" >&2 || true
    die "timed out waiting for ${description}; logs=${probe_log_root}"
}

ps_state_query() {
    local component="$1"
    local state="$2"
    local public_state="${state^^}"
    case "${state}" in
        Active)
            printf '[.components[] | select(.component_id == "%s" and .state == "%s")] | length == 1' \
                "${component}" "${public_state}"
            ;;
        Blocked|Failed)
            printf '[.components[] | select(.component_id == "%s" and .state == "%s")] | length == 1' \
                "${component}" "${public_state}"
            ;;
        *) die "unsupported state: ${state}" ;;
    esac
}

wait_state() {
    wait_ps_query "$1 $2" "$(ps_state_query "$1" "$2")" \
        "${3:-${probe_log_root}/current.ps.json}"
}

wait_log() {
    local file="$1"
    local pattern="$2"
    local deadline=$((SECONDS + timeout_seconds))
    while ((SECONDS < deadline)); do
        if rg -F --quiet -- "${pattern}" "${file}" 2>/dev/null; then
            return 0
        fi
        sleep 0.05
    done
    die "timed out waiting for ${pattern@Q} in ${file}"
}

line_count() {
    local pattern="$1"
    local file="$2"
    rg -F --count -- "${pattern}" "${file}" 2>/dev/null || printf '0\n'
}

wait_line_count() {
    local file="$1"
    local pattern="$2"
    local expected="$3"
    local deadline=$((SECONDS + timeout_seconds))
    while ((SECONDS < deadline)); do
        if (( $(line_count "${pattern}" "${file}") >= expected )); then
            return 0
        fi
        sleep 0.05
    done
    die "timed out waiting for ${expected} occurrences of ${pattern@Q} in ${file}"
}

thread_name_count() {
    local pid="$1"
    local prefix="$2"
    local count=0
    local comm_path name
    for comm_path in "/proc/${pid}/task/"*/comm; do
        [[ -r "${comm_path}" ]] || continue
        IFS= read -r name <"${comm_path}" || continue
        [[ "${name}" == "${prefix}"* ]] && count=$((count + 1))
    done
    printf '%s\n' "${count}"
}

process_resource_snapshot() {
    local pid="$1"
    local user_ticks system_ticks threads fds
    read -r user_ticks system_ticks < <(awk '{print $14, $15}' "/proc/${pid}/stat")
    threads="$(find "/proc/${pid}/task" -mindepth 1 -maxdepth 1 -type d -printf '.\n' | wc -l)"
    fds="$(find "/proc/${pid}/fd" -mindepth 1 -maxdepth 1 -printf '.\n' | wc -l)"
    printf '%s %s %s %s\n' "${user_ticks}" "${system_ticks}" "${threads}" "${fds}"
}

start_composer() {
    local label="$1"
    start_process "${label}" env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info "${composer}"
    composer_pid="${last_pid}"
    wait_ps_query "Composer ${label}" '.phase == "recovering" or .phase == "running"'
}

start_probe() {
    local label="$1"
    shift
    start_process "${label}" env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
        "${python}" "${probe_script}" "$@"
}

start_camera() {
    local label="$1"
    local binary="$2"
    start_process "${label}" env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=warn "${binary}"
}

start_detector() {
    local label="$1"
    start_process "${label}" env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
        "${python}" "${detector_script}"
}

monotonic_ms() {
    awk '{printf "%.0f\n", $1 * 1000}' /proc/uptime
}

printf 'RBT_PYTHON_ASYNCIO_RUN mode=%s address=%s logs=%s\n' \
    "${mode}" "${composer_addr}" "${run_dir}"
start_composer composer

# A managed asyncio task is activation-owned and receives a cooperative
# cancellation during ordinary process shutdown.
start_probe task-cancel minimal-async-cancel
task_cancel_pid="${last_pid}"
wait_state python-minimal-async-cancel Active
wait_log "${probe_log_root}/task-cancel.log" 'PROBE ACTIVATED mode=minimal-async-cancel'
loop_threads="$(thread_name_count "${task_cancel_pid}" rbt-python-asyn)"
bridge_threads="$(thread_name_count "${task_cancel_pid}" rbt-wait-bridg)"
[[ "${loop_threads}" == 1 ]] || die "minimal async task did not own exactly one loop thread"
[[ "${bridge_threads}" == 0 ]] || die "stream-free async task unexpectedly started a WaitSet bridge"
if [[ "${mode}" == full ]]; then
    task_cancel_before="$(awk '{print $14 + $15}' "/proc/${task_cancel_pid}/stat")"
    sleep "${idle_seconds}"
    kill -0 "${task_cancel_pid}" 2>/dev/null || die "idle async task exited unexpectedly"
    task_cancel_after="$(awk '{print $14 + $15}' "/proc/${task_cancel_pid}/stat")"
    clock_ticks="$(getconf CLK_TCK)"
    idle_tick_limit=$((clock_ticks * idle_seconds / 2 + clock_ticks))
    idle_tick_delta=$((task_cancel_after - task_cancel_before))
    ((idle_tick_delta >= 0 && idle_tick_delta <= idle_tick_limit)) \
        || die "idle asyncio task consumed too much CPU: ticks=${idle_tick_delta} limit=${idle_tick_limit}"
    printf 'RBT_PYTHON_ASYNCIO_IDLE seconds=%s cpu_ticks=%s limit=%s status=pass\n' \
        "${idle_seconds}" "${idle_tick_delta}" "${idle_tick_limit}"
fi
process_shutdown_start_ms="$(monotonic_ms)"
stop_process TERM "${task_cancel_pid}"
process_shutdown_end_ms="$(monotonic_ms)"
[[ "${last_status}" == 0 ]] || die "minimal async task did not stop gracefully"
wait_log "${probe_log_root}/task-cancel.log" 'PROBE ASYNC CANCELLED mode=minimal-async-cancel'
process_shutdown_ms=$((process_shutdown_end_ms - process_shutdown_start_ms))

# Ordinary cooperative process cancellation runs coroutine finally blocks.
start_probe cancellation-finally async-cancellation-finally
cancellation_finally_pid="${last_pid}"
wait_state python-async-cancellation-finally Active
wait_log "${probe_log_root}/cancellation-finally.log" \
    'PROBE ASYNC CANCELLATION FINALLY STARTED'
stop_process TERM "${cancellation_finally_pid}"
[[ "${last_status}" == 0 ]] || die "cancellation-finally probe did not stop gracefully"
wait_log "${probe_log_root}/cancellation-finally.log" \
    'PROBE ASYNC CANCELLATION FINALLY ran=1 cancelled=1'

# A background coroutine may return normally without invalidating activation.
start_probe background-return background-normal-return
background_return_pid="${last_pid}"
wait_state python-background-normal-return Active
wait_log "${probe_log_root}/background-return.log" 'PROBE BACKGROUND NORMAL RETURN'
wait_log "${probe_log_root}/background-return.log" \
    'background task exited; activation remains unchanged'
wait_state python-background-normal-return Active
stop_process TERM "${background_return_pid}"
[[ "${last_status}" == 0 ]] || die "background normal-return probe did not stop gracefully"

# User cancellation of a registered critical task is an unexpected critical
# exit; cancellation of a background task is diagnosed while ACTIVE remains.
start_probe critical-handle-cancel critical-handle-cancel
critical_handle_pid="${last_pid}"
wait_state python-critical-handle-cancel Failed
wait_log "${probe_log_root}/critical-handle-cancel.log" 'PROBE CRITICAL HANDLE STARTED'
wait_log "${probe_log_root}/critical-handle-cancel.log" 'PROBE CRITICAL HANDLE CANCELLED'
stop_process TERM "${critical_handle_pid}"
[[ "${last_status}" == 0 ]] || die "critical task-handle probe did not stop gracefully"

start_probe background-handle-cancel background-handle-cancel
background_handle_pid="${last_pid}"
wait_state python-background-handle-cancel Active
wait_log "${probe_log_root}/background-handle-cancel.log" 'PROBE BACKGROUND HANDLE STARTED'
wait_log "${probe_log_root}/background-handle-cancel.log" 'PROBE BACKGROUND HANDLE CANCELLED'
wait_state python-background-handle-cancel Active
stop_process TERM "${background_handle_pid}"
[[ "${last_status}" == 0 ]] || die "background task-handle probe did not stop gracefully"

# Cancel a Task while it owns receive_async(), then use the same subscriber
# again.  This is the live counterpart to the deterministic stale-generation
# unit tests.
start_camera user-receive-camera "${camera_a}"
user_receive_camera_pid="${last_pid}"
start_probe user-receive-cancel async-receive-user-cancel
user_receive_pid="${last_pid}"
wait_state python-async-receive-user-cancel Active
wait_log "${probe_log_root}/user-receive-cancel.log" \
    'PROBE ASYNC USER RECEIVE CANCEL REQUEST'
wait_log "${probe_log_root}/user-receive-cancel.log" \
    'PROBE ASYNC USER RECEIVE CANCELLED'
wait_log "${probe_log_root}/user-receive-cancel.log" \
    'PROBE ASYNC USER RECEIVE RECOVERED'
wait_state python-async-receive-user-cancel Active
user_cancel_request_ns="$(sed -n 's/.*PROBE ASYNC USER RECEIVE CANCEL REQUEST monotonic_ns=\([0-9][0-9]*\).*/\1/p' \
    "${probe_log_root}/user-receive-cancel.log" | tail -1)"
user_cancel_finally_ns="$(sed -n 's/.*PROBE ASYNC USER RECEIVE FINALLY monotonic_ns=\([0-9][0-9]*\).*/\1/p' \
    "${probe_log_root}/user-receive-cancel.log" | tail -1)"
[[ -n "${user_cancel_request_ns}" && -n "${user_cancel_finally_ns}" ]] \
    || die "user receive cancellation did not record latency markers"
user_cancel_latency_ms=$(((user_cancel_finally_ns - user_cancel_request_ns) / 1000000))
((user_cancel_latency_ms >= 0)) || die "user cancellation latency was negative"
stop_process TERM "${user_receive_pid}"
[[ "${last_status}" == 0 ]] || die "user receive-cancel probe did not stop gracefully"
stop_process KILL "${user_receive_camera_pid}"

# Critical async exception remains visible to the native Component supervisor.
start_probe critical-async-error critical-async-exception
critical_error_pid="${last_pid}"
wait_state python-critical-async-exception Failed
wait_log "${probe_log_root}/critical-async-error.log" 'exception=RuntimeError'
wait_log "${probe_log_root}/critical-async-error.log" 'intentional async critical failure'
kill -0 "${critical_error_pid}" 2>/dev/null \
    || die "critical async failure terminated the Component process"
stop_process TERM "${critical_error_pid}"
[[ "${last_status}" == 0 ]] || die "critical async failure probe did not stop gracefully"

# Provider loss while receive_async() is pending must wake the coroutine and
# leave the Component BLOCKED without prefetching a payload.
start_probe provider-cancel async-receive-provider-cancel
provider_cancel_pid="${last_pid}"
wait_state python-async-receive-provider-cancel Blocked
start_camera provider-cancel-camera "${camera_a}"
provider_camera_pid="${last_pid}"
wait_state python-async-receive-provider-cancel Active
wait_log "${probe_log_root}/provider-cancel.log" 'PROBE ASYNC RECEIVE WAITING label=provider-cancel'
loop_threads="$(thread_name_count "${provider_cancel_pid}" rbt-python-asyn)"
bridge_threads="$(thread_name_count "${provider_cancel_pid}" rbt-wait-bridg)"
[[ "${loop_threads}" == 1 ]] || die "receive probe did not own exactly one loop thread"
[[ "${bridge_threads}" == 1 ]] || die "receive probe did not own exactly one activation bridge thread"
provider_kill_ms="$(monotonic_ms)"
stop_process KILL "${provider_camera_pid}"
wait_state python-async-receive-provider-cancel Blocked \
    "${probe_log_root}/provider-cancel.ps.json"
provider_blocked_ms="$(monotonic_ms)"
wait_log "${probe_log_root}/provider-cancel.log" 'PROBE ASYNC RECEIVE CANCELLED label=provider-cancel'
provider_latency_ms=$((provider_blocked_ms - provider_kill_ms))
stop_process TERM "${provider_cancel_pid}"
[[ "${last_status}" == 0 ]] || die "provider-cancel probe did not stop gracefully"
printf 'RBT_PYTHON_ASYNCIO_LATENCY provider_cancel_to_blocked_ms=%s\n' \
    "${provider_latency_ms}"

# Repeated native receive_async(timeout=...) calls must clean their wait
# registrations and permit a later sample to be acquired normally.
start_probe timeout async-receive-timeouts
timeout_pid="${last_pid}"
wait_state python-async-receive-timeouts Blocked
start_camera timeout-camera "${camera_a}"
timeout_camera_pid="${last_pid}"
wait_state python-async-receive-timeouts Active
wait_log "${probe_log_root}/timeout.log" 'PROBE ASYNC TIMEOUT index=2'
wait_log "${probe_log_root}/timeout.log" 'PROBE ASYNC TIMEOUT SUCCESS timeouts=3'
wait_log "${probe_log_root}/timeout.log" 'PROBE ASYNC TIMEOUTS FINALLY count=3'
stop_process TERM "${timeout_pid}"
[[ "${last_status}" == 0 ]] || die "async timeout probe did not stop gracefully"
stop_process KILL "${timeout_camera_pid}"

# A synchronous critical failure wakes an async receive sibling.  Full mode
# also verifies the reverse direction below.
start_camera sibling-camera "${camera_a}"
sibling_camera_pid="${last_pid}"
start_probe sync-async-sibling sync-critical-async-sibling
sync_async_sibling_pid="${last_pid}"
wait_state python-sync-critical-async-sibling Failed
wait_log "${probe_log_root}/sync-async-sibling.log" 'PROBE SIBLING CANCELLED async=1'
wait_log "${probe_log_root}/sync-async-sibling.log" \
    'intentional critical sibling failure (sync)'
stop_process TERM "${sync_async_sibling_pid}"
[[ "${last_status}" == 0 ]] || die "sync-to-async sibling probe did not stop gracefully"
stop_process KILL "${sibling_camera_pid}"

if [[ "${mode}" == full ]]; then
    start_camera async-sync-sibling-camera "${camera_a}"
    async_sync_sibling_camera_pid="${last_pid}"
    start_probe async-sync-sibling async-critical-sync-sibling
    async_sync_sibling_pid="${last_pid}"
    wait_state python-async-critical-sync-sibling Failed
    wait_log "${probe_log_root}/async-sync-sibling.log" 'PROBE SYNC WAIT CANCELLED'
    wait_log "${probe_log_root}/async-sync-sibling.log" \
        'intentional async critical sibling failure'
    stop_process TERM "${async_sync_sibling_pid}"
    [[ "${last_status}" == 0 ]] || die "async-to-sync sibling probe did not stop gracefully"
    stop_process KILL "${async_sync_sibling_camera_pid}"

    # A deliberately delayed activation-1 background completion must be
    # harmless after activation 2 becomes current.  The task suppresses one
    # cancellation only long enough to outlive EffectScope's wrapper grace.
    start_probe stale-completion async-stale-completion
    stale_completion_pid="${last_pid}"
    wait_state python-async-stale-completion Blocked
    start_camera stale-camera-a "${camera_a}"
    stale_camera_a_pid="${last_pid}"
    wait_state python-async-stale-completion Active
    wait_log "${probe_log_root}/stale-completion.log" \
        'PROBE STALE TASK STARTED activation=1 activation_count=1'
    stop_process KILL "${stale_camera_a_pid}"
    wait_state python-async-stale-completion Blocked
    wait_log "${probe_log_root}/stale-completion.log" \
        'PROBE STALE TASK CAUGHT CANCELLATION activation=1 activation_count=1'
    start_camera stale-camera-b "${camera_b}"
    stale_camera_b_pid="${last_pid}"
    wait_state python-async-stale-completion Active \
        "${probe_log_root}/stale-active-2.ps.json"
    jq -e '
        [.components[]
         | select(.component_id == "python-async-stale-completion")
         | .activation_id] == [2]
    ' "${probe_log_root}/stale-active-2.ps.json" >/dev/null \
        || die "replacement activation was not activation 2 during stale completion probe"
    wait_log "${probe_log_root}/stale-completion.log" \
        'PROBE STALE TASK LATE COMPLETION activation=1 activation_count=1'
    stale_activation_two_ns="$(sed -n \
        's/.*PROBE ACTIVATED mode=async-stale-completion .* activation=2 activation_count=2 monotonic_ns=\([0-9][0-9]*\).*/\1/p' \
        "${probe_log_root}/stale-completion.log" | tail -1)"
    stale_late_ns="$(sed -n \
        's/.*PROBE STALE TASK LATE COMPLETION activation=1 activation_count=1 monotonic_ns=\([0-9][0-9]*\).*/\1/p' \
        "${probe_log_root}/stale-completion.log" | tail -1)"
    [[ -n "${stale_activation_two_ns}" && -n "${stale_late_ns}" ]] \
        || die "stale completion probe omitted monotonic ordering evidence"
    ((stale_late_ns > stale_activation_two_ns)) \
        || die "stale async completion did not occur after activation 2 became current"
    wait_state python-async-stale-completion Active
    stop_process KILL "${stale_camera_b_pid}"
    wait_state python-async-stale-completion Blocked
    stop_process TERM "${stale_completion_pid}"
    [[ "${last_status}" == 0 ]] || die "stale completion probe did not stop gracefully"
    printf 'RBT_PYTHON_ASYNCIO_STALE_COMPLETION activation_2_ns=%s late_activation_1_ns=%s state=Active status=pass\n' \
        "${stale_activation_two_ns}" "${stale_late_ns}"

    # Sensor pressure remains transport-owned: a deliberately slow coroutine
    # skips sequences while the publisher and process remain healthy.
    start_camera pressure-camera "${camera_a}"
    pressure_camera_pid="${last_pid}"
    start_probe slow-consumer async-slow-consumer
    slow_consumer_pid="${last_pid}"
    wait_state python-async-slow-consumer Active
    wait_line_count "${probe_log_root}/slow-consumer.log" \
        'PROBE ASYNC SLOW SAMPLE' 3
    stop_process TERM "${slow_consumer_pid}"
    [[ "${last_status}" == 0 ]] || die "slow async consumer did not stop gracefully"
    slow_final="$(rg -F 'PROBE ASYNC SLOW CONSUMER FINALLY' \
        "${probe_log_root}/slow-consumer.log" | tail -1 || true)"
    slow_gaps="$(sed -n 's/.*sequence_gaps=\([0-9][0-9]*\).*/\1/p' <<<"${slow_final}")"
    [[ -n "${slow_gaps}" ]] || die "slow async consumer omitted pressure evidence"
    ((slow_gaps > 0)) || die "slow async Sensor consumer accumulated a hidden backlog"

    # Multiple native readiness events are coalesced into bounded callbacks;
    # the callback count must never grow beyond events plus the one retained
    # ready-bit replay allowed by a receive-generation boundary.
    start_probe wake-storm async-wake-storm
    wake_storm_pid="${last_pid}"
    wait_state python-async-wake-storm Active
    wait_line_count "${probe_log_root}/wake-storm.log" \
        'PROBE ASYNC WAKE STORM SAMPLE' 3
    stop_process TERM "${wake_storm_pid}"
    [[ "${last_status}" == 0 ]] || die "async wake-storm probe did not stop gracefully"
    wake_final="$(rg -F 'PROBE ASYNC WAKE STORM FINALLY' \
        "${probe_log_root}/wake-storm.log" | tail -1 || true)"
    wake_events="$(sed -n 's/.*native_readiness_events=\([0-9][0-9]*\).*/\1/p' <<<"${wake_final}")"
    wake_callbacks="$(sed -n 's/.*scheduled_readiness_callbacks=\([0-9][0-9]*\).*/\1/p' <<<"${wake_final}")"
    wake_samples="$(sed -n 's/.*samples_processed=\([0-9][0-9]*\).*/\1/p' <<<"${wake_final}")"
    [[ -n "${wake_events}" && -n "${wake_callbacks}" && -n "${wake_samples}" ]] \
        || die "wake-storm probe omitted coalescing evidence"
    ((wake_events > wake_samples)) || die "wake-storm did not exercise readiness coalescing"
    ((wake_callbacks <= wake_events + 1)) \
        || die "async readiness callbacks grew faster than native readiness events"

    # Two distinct subscribers wait concurrently on the same activation-owned
    # bridge.  The fixed detector component supplies Detections from Camera.
    start_detector multi-source-detector
    multi_detector_pid="${last_pid}"
    wait_state detector Active
    start_probe multi-source async-multi-source \
        --component-id python-async-multi-source
    multi_source_pid="${last_pid}"
    wait_state python-async-multi-source Active
    wait_line_count "${probe_log_root}/multi-source.log" \
        'PROBE SAMPLE label=camera' 3
    wait_line_count "${probe_log_root}/multi-source.log" \
        'PROBE SAMPLE label=detections' 3
    multi_loop_threads="$(thread_name_count "${multi_source_pid}" rbt-python-asyn)"
    multi_bridge_threads="$(thread_name_count "${multi_source_pid}" rbt-wait-bridg)"
    [[ "${multi_loop_threads}" == 1 ]] \
        || die "multi-source probe did not own exactly one managed loop thread"
    [[ "${multi_bridge_threads}" == 1 ]] \
        || die "multi-source probe did not multiplex streams on one WaitSet bridge"
    stop_process TERM "${multi_source_pid}"
    [[ "${last_status}" == 0 ]] || die "multi-source probe did not stop gracefully"
    stop_process TERM "${multi_detector_pid}"
    [[ "${last_status}" == 0 ]] || die "multi-source detector did not stop gracefully"
    printf 'RBT_PYTHON_ASYNCIO_MULTI_SOURCE camera_samples=3 detections_samples=3 loop_threads=%s bridge_threads=%s status=pass\n' \
        "${multi_loop_threads}" "${multi_bridge_threads}"

    # A subscriber which never calls receive_async() must not impede an
    # independent fast subscriber on the same Sensor transport.
    start_probe stalled-consumer async-stalled-consumer
    stalled_consumer_pid="${last_pid}"
    start_probe fast-peer async-receive-loop --component-id python-fast-peer
    fast_peer_pid="${last_pid}"
    wait_state python-async-stalled-consumer Active
    wait_state python-fast-peer Active
    wait_log "${probe_log_root}/stalled-consumer.log" \
        'PROBE ASYNC STALLED CONSUMER STARTED receive_calls=0'
    wait_line_count "${probe_log_root}/fast-peer.log" 'PROBE SAMPLE label=require-only' 3
    stop_process TERM "${stalled_consumer_pid}"
    [[ "${last_status}" == 0 ]] || die "stalled async consumer did not stop gracefully"
    stop_process TERM "${fast_peer_pid}"
    [[ "${last_status}" == 0 ]] || die "fast peer did not stop gracefully"

    # Cancellation while a writable loan is mapped unwinds aliases and drops
    # the unsent loan without publishing it.
    start_probe unsent-loan unsent-loan-cancel
    unsent_loan_pid="${last_pid}"
    wait_state python-unsent-loan-cancel Active
    wait_log "${probe_log_root}/unsent-loan.log" 'PROBE HOLDING WRITABLE LOAN'
    stop_process KILL "${pressure_camera_pid}"
    wait_state python-unsent-loan-cancel Blocked
    wait_log "${probe_log_root}/unsent-loan.log" 'PROBE UNSENT LOAN DROPPED'
    stop_process TERM "${unsent_loan_pid}"
    [[ "${last_status}" == 0 ]] || die "unsent async loan probe did not stop gracefully"

    # Compare an honestly idle native synchronous receive with an honestly
    # idle async receive against a provider which advertises Camera but sends
    # no payload.  Report raw user/system CPU, thread, and FD deltas.
    start_probe idle-camera-provider provider-idle-camera
    idle_provider_pid="${last_pid}"
    wait_state python-provider-idle-camera Active

    start_probe sync-idle-receive sync-idle-receive \
        --component-id python-sync-idle-receive
    sync_idle_pid="${last_pid}"
    wait_state python-sync-idle-receive Active
    wait_log "${probe_log_root}/sync-idle-receive.log" \
        'PROBE SYNC IDLE RECEIVE STARTED'
    [[ "$(thread_name_count "${sync_idle_pid}" rbt-python-asyn)" == 0 ]] \
        || die "sync idle receiver unexpectedly started an asyncio loop"
    [[ "$(thread_name_count "${sync_idle_pid}" rbt-wait-bridg)" == 0 ]] \
        || die "sync idle receiver unexpectedly started an async WaitSet bridge"
    read -r sync_user_before sync_system_before sync_threads_before sync_fds_before \
        < <(process_resource_snapshot "${sync_idle_pid}")
    sleep "${idle_seconds}"
    kill -0 "${sync_idle_pid}" 2>/dev/null || die "sync idle receiver exited unexpectedly"
    read -r sync_user_after sync_system_after sync_threads_after sync_fds_after \
        < <(process_resource_snapshot "${sync_idle_pid}")
    sync_user_delta=$((sync_user_after - sync_user_before))
    sync_system_delta=$((sync_system_after - sync_system_before))
    sync_total_delta=$((sync_user_delta + sync_system_delta))
    ((sync_user_delta >= 0 && sync_system_delta >= 0 \
        && sync_total_delta <= idle_tick_limit)) \
        || die "sync idle WaitSet consumed too much CPU"
    [[ "${sync_threads_before}" == "${sync_threads_after}" \
        && "${sync_fds_before}" == "${sync_fds_after}" ]] \
        || die "sync idle WaitSet resources changed while waiting"
    stop_process TERM "${sync_idle_pid}"
    [[ "${last_status}" == 0 ]] || die "sync idle receiver did not stop gracefully"

    start_probe async-idle-receive async-receive-loop \
        --component-id python-async-idle-receive
    async_idle_pid="${last_pid}"
    wait_state python-async-idle-receive Active
    [[ "$(thread_name_count "${async_idle_pid}" rbt-python-asyn)" == 1 ]] \
        || die "async idle receiver did not own exactly one managed loop thread"
    [[ "$(thread_name_count "${async_idle_pid}" rbt-wait-bridg)" == 1 ]] \
        || die "async idle receiver did not own exactly one WaitSet bridge"
    read -r async_user_before async_system_before async_threads_before async_fds_before \
        < <(process_resource_snapshot "${async_idle_pid}")
    sleep "${idle_seconds}"
    kill -0 "${async_idle_pid}" 2>/dev/null || die "async idle receiver exited unexpectedly"
    read -r async_user_after async_system_after async_threads_after async_fds_after \
        < <(process_resource_snapshot "${async_idle_pid}")
    async_user_delta=$((async_user_after - async_user_before))
    async_system_delta=$((async_system_after - async_system_before))
    async_total_delta=$((async_user_delta + async_system_delta))
    ((async_user_delta >= 0 && async_system_delta >= 0 \
        && async_total_delta <= idle_tick_limit)) \
        || die "async idle WaitSet consumed too much CPU"
    [[ "${async_threads_before}" == "${async_threads_after}" \
        && "${async_fds_before}" == "${async_fds_after}" ]] \
        || die "async idle WaitSet resources changed while waiting"
    stop_process TERM "${async_idle_pid}"
    [[ "${last_status}" == 0 ]] || die "async idle receiver did not stop gracefully"
    stop_process TERM "${idle_provider_pid}"
    [[ "${last_status}" == 0 ]] || die "idle Camera provider did not stop gracefully"
    printf 'RBT_PYTHON_ASYNCIO_IDLE_WAIT_COMPARE seconds=%s sync_user_ticks=%s sync_system_ticks=%s sync_threads_before=%s sync_threads_after=%s sync_fds_before=%s sync_fds_after=%s async_user_ticks=%s async_system_ticks=%s async_threads_before=%s async_threads_after=%s async_fds_before=%s async_fds_after=%s limit=%s status=pass\n' \
        "${idle_seconds}" "${sync_user_delta}" "${sync_system_delta}" \
        "${sync_threads_before}" "${sync_threads_after}" \
        "${sync_fds_before}" "${sync_fds_after}" \
        "${async_user_delta}" "${async_system_delta}" \
        "${async_threads_before}" "${async_threads_after}" \
        "${async_fds_before}" "${async_fds_after}" \
        "${idle_tick_limit}"
fi

stop_process TERM "${composer_pid}"
[[ "${last_status}" == 0 || "${last_status}" == 143 ]] \
    || die "async probe Composer did not stop after SIGTERM"

printf 'RBT_PYTHON_ASYNCIO_QUICK task_cancel=pass timeout=pass provider_cancel=pass \
sibling_sync_to_async=pass zero_copy=pass finally=pass background_return=pass\n'
printf 'RBT_PYTHON_ASYNCIO_CANCELLATION activation_ms=%s user_task_ms=%s process_ms=%s\n' \
    "${provider_latency_ms}" "${user_cancel_latency_ms}" "${process_shutdown_ms}"
if [[ "${mode}" == full ]]; then
    printf 'RBT_PYTHON_ASYNCIO_FULL cycles=100 soak=%ss idle=%ss \
activation_latency_ms=%sms sibling_async_to_sync=pass pressure=pass coalescing=pass \
stale_completion=pass multi_source=pass idle_wait_compare=pass\n' \
        "${soak_seconds}" "${idle_seconds}" "${provider_latency_ms}"
fi

unexpected_asyncio_log="${probe_log_root}/unexpected-asyncio-warnings.log"
if rg -n -i \
    'Task was destroyed but it is pending|coroutine .* was never awaited|RuntimeWarning:.*never awaited|Task exception was never retrieved|event loop is closed|Exception in callback None|managed asyncio task completion was disconnected' \
    "${run_dir}" >"${unexpected_asyncio_log}"; then
    cat "${unexpected_asyncio_log}" >&2
    die "unexpected asyncio cleanup/runtime warning was logged"
fi
printf 'RBT_PYTHON_ASYNCIO_PENDING_TASK_AUDIT unexpected_warnings=0\n'
printf 'PASS: Python asyncio runtime %s; logs=%s\n' "${mode}" "${run_dir}"

#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

usage() {
    printf 'usage: %s [--quick|--full]\n' "$0"
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
cpp_build="${RBT_PYTHON_COMPONENT_CPP_BUILD:-${repo_root}/target/python-cpp-build}"
python="${RBT_PYTHON_COMPONENT_PYTHON:-${repo_root}/target/python-verify-venv/bin/python}"
detector_script="${RBT_PYTHON_COMPONENT_EXAMPLE:-${repo_root}/examples/python/component_detector.py}"
probe_script="${RBT_PYTHON_COMPONENT_PROBE:-${repo_root}/python/tests/component_runtime_probe.py}"
timeout_seconds="${RBT_PYTHON_COMPONENT_TIMEOUT_SECONDS:-25}"
soak_seconds="${RBT_PYTHON_COMPONENT_SOAK_SECONDS:-300}"
port="${RBT_PYTHON_COMPONENT_PORT:-$((18000 + $$ % 1000))}"
composer_addr="127.0.0.1:${port}"
log_root="${RBT_PYTHON_COMPONENT_LOG_DIR:-${repo_root}/target/python-component-logs}"
process_cwd="${RBT_PYTHON_COMPONENT_PROCESS_CWD:-${repo_root}}"
mkdir -p "${log_root}"
run_dir="$(mktemp -d "${log_root}/runtime-${mode}.XXXXXX")"

[[ "${timeout_seconds}" =~ ^[1-9][0-9]*$ ]] || die "timeout must be positive"
[[ "${soak_seconds}" =~ ^[1-9][0-9]*$ ]] || die "soak duration must be positive"
[[ -x "${python}" ]] || die "Python interpreter is missing: ${python}"
[[ -f "${detector_script}" ]] || die "detector example is missing: ${detector_script}"
[[ -f "${probe_script}" ]] || die "component probe is missing: ${probe_script}"
[[ -d "${process_cwd}" ]] || die "process working directory is missing: ${process_cwd}"
for command_name in cargo cmake jq rg awk diff timeout; do
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
"${python}" -c 'import rbt; assert hasattr(rbt, "run_component")' \
    || die "selected Python does not have the current rbt Component runtime"

declare -a child_pids=()
last_pid=''
last_status=0
composer_pid=''

cleanup() {
    local pid
    for pid in "${child_pids[@]:-}"; do
        if kill -0 "${pid}" 2>/dev/null; then
            kill -CONT "${pid}" 2>/dev/null || true
            kill -TERM "${pid}" 2>/dev/null || true
        fi
    done
    for pid in "${child_pids[@]:-}"; do
        wait "${pid}" 2>/dev/null || true
    done
}
trap cleanup EXIT
trap 'exit 130' INT TERM

monotonic_ms() {
    awk '{printf "%.0f\n", $1 * 1000}' /proc/uptime
}

start_process() {
    local label="$1"
    shift
    printf '+'
    printf ' %q' "$@"
    printf ' > %s/%s.log\n' "${run_dir}" "${label}"
    (
        cd "${process_cwd}"
        exec "$@"
    ) >"${run_dir}/${label}.log" 2>&1 &
    last_pid="$!"
    child_pids+=("${last_pid}")
}

start_split_process() {
    local label="$1"
    shift
    printf '+'
    printf ' %q' "$@"
    printf ' > %s/%s.json 2> %s/%s.err\n' \
        "${run_dir}" "${label}" "${run_dir}" "${label}"
    (
        cd "${process_cwd}"
        exec "$@"
    ) >"${run_dir}/${label}.json" 2>"${run_dir}/${label}.err" &
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

capability_list_json() {
    RBT_COMPOSER_ADDR="${composer_addr}" "${rbt_cli}" capability list --json
}

stream_pressure_json() {
    local capability="$1"
    RBT_COMPOSER_ADDR="${composer_addr}" "${rbt_cli}" stream pressure \
        "${capability}" --json
}

capture_ps() {
    local output="$1"
    ps_json >"${output}"
}

capture_capabilities() {
    local output="$1"
    capability_list_json >"${output}"
    jq -e 'type == "array"' "${output}" >/dev/null \
        || die "capability list projection was not a top-level array: ${output}"
}

capture_stream_pressure() {
    local capability="$1"
    local output="$2"
    stream_pressure_json "${capability}" >"${output}"
}

wait_ps_query() {
    local description="$1"
    local query="$2"
    local output="${3:-${run_dir}/current.ps.json}"
    local deadline=$((SECONDS + timeout_seconds))
    while ((SECONDS < deadline)); do
        if ps_json >"${output}" 2>/dev/null \
            && jq -e "${query}" "${output}" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    [[ -f "${output}" ]] && jq . "${output}" >&2 || true
    die "timed out waiting for ${description}; logs=${run_dir}"
}

wait_capability_query() {
    local description="$1"
    local query="$2"
    local output="${3:-${run_dir}/current.capabilities.json}"
    local deadline=$((SECONDS + timeout_seconds))
    while ((SECONDS < deadline)); do
        if capability_list_json >"${output}" 2>/dev/null \
            && jq -e 'type == "array"' "${output}" >/dev/null 2>&1 \
            && jq -e "${query}" "${output}" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    [[ -f "${output}" ]] && jq . "${output}" >&2 || true
    die "timed out waiting for ${description}; logs=${run_dir}"
}

ps_state_query() {
    local component="$1"
    local state="$2"
    local public_state="${state^^}"
    case "${state}" in
        Active|Preparing|Activating|Quiescing|Declared)
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
        "${3:-${run_dir}/current.ps.json}"
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

start_composer() {
    local label="$1"
    start_process "${label}" env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info "${composer}"
    composer_pid="${last_pid}"
    wait_ps_query "Composer ${label}" '.phase == "recovering" or .phase == "running"'
}

ps_component_field() {
    local file="$1"
    local component="$2"
    local expression="$3"
    jq -r ".components[] | select(.component_id == \"${component}\") | ${expression}" "${file}"
}

capability_field() {
    local file="$1"
    local component="$2"
    local capability_type="$3"
    local expression="$4"
    jq -r --arg component "${component}" --arg capability_type "${capability_type}" \
        '.[] | select(.provider_component == $component and .capability_type == $capability_type) | '"${expression}" \
        "${file}"
}

record_resources() {
    local pid="$1"
    local output="$2"
    local rss threads fds
    rss="$(awk '/^VmRSS:/ {print $2}' "/proc/${pid}/status")"
    threads="$(awk '/^Threads:/ {print $2}' "/proc/${pid}/status")"
    fds="$(find "/proc/${pid}/fd" -mindepth 1 -maxdepth 1 -printf . 2>/dev/null | wc -c)"
    printf 'rss_kib=%s threads=%s fds=%s\n' "${rss}" "${threads}" "${fds}" >"${output}"
}

resource_value() {
    local file="$1"
    local name="$2"
    sed -n "s/.*${name}=\([0-9][0-9]*\).*/\1/p" "${file}"
}

process_is_running() {
    local pid="$1"
    [[ -r "/proc/${pid}/status" ]] || return 1
    local state
    state="$(awk '/^State:/ {print $2; exit}' "/proc/${pid}/status" 2>/dev/null || true)"
    [[ -n "${state}" && "${state}" != Z ]]
}

record_mixed_resource_sample() {
    local pid="$1"
    local output="$2"
    local label="$3"
    process_is_running "${pid}" || return 0
    local rss threads fds
    rss="$(awk '/^VmRSS:/ {print $2}' "/proc/${pid}/status" 2>/dev/null || true)"
    threads="$(awk '/^Threads:/ {print $2}' "/proc/${pid}/status" 2>/dev/null || true)"
    fds="$(find "/proc/${pid}/fd" -mindepth 1 -maxdepth 1 -printf . 2>/dev/null | wc -c)"
    printf '%s\t%s\t%s\t%s\t%s\n' \
        "${SECONDS}" "${label}" "${rss:-0}" "${fds:-0}" "${threads:-0}" >>"${output}"
}

summarize_mixed_resources() {
    local input="$1"
    local output="$2"
    awk -F '\t' '
        NR == 1 && $1 == "seconds" { next }
        {
            label = $2
            raw_samples[label]++
            if (raw_samples[label] <= 2) next
            samples[label]++
            if (!(label in initial_rss)) {
                initial_rss[label] = $3
                initial_fds[label] = $4
                initial_threads[label] = $5
            }
            final_rss[label] = $3
            final_fds[label] = $4
            final_threads[label] = $5
            if (!(label in max_rss) || $3 > max_rss[label]) max_rss[label] = $3
        }
        END {
            status = 0
            for (label in initial_rss) {
                if (samples[label] < 3) {
                    print label ": insufficient mixed-soak resource samples" > "/dev/stderr"
                    status = 1
                    continue
                }
                if (final_rss[label] > initial_rss[label] + 65536 ||
                    max_rss[label] > initial_rss[label] + 131072 ||
                    final_fds[label] > initial_fds[label] + 8 ||
                    final_threads[label] > initial_threads[label] + 2) {
                    printf "%s: resource growth initial_rss=%s final_rss=%s max_rss=%s fd=%s->%s threads=%s->%s\n",
                        label, initial_rss[label], final_rss[label], max_rss[label],
                        initial_fds[label], final_fds[label], initial_threads[label],
                        final_threads[label] > "/dev/stderr"
                    status = 1
                    continue
                }
                printf "%s\tinitial_rss_kib=%s\tfinal_rss_kib=%s\tmax_rss_kib=%s\tinitial_fds=%s\tfinal_fds=%s\tinitial_threads=%s\tfinal_threads=%s\tsamples=%s\twarmup_samples=2\n",
                    label, initial_rss[label], final_rss[label], max_rss[label],
                    initial_fds[label], final_fds[label], initial_threads[label],
                    final_threads[label], samples[label]
            }
            exit status
        }
    ' "${input}" >"${output}"
}

assert_mixed_pressure_capabilities() {
    local file="$1"
    jq -e '
        ([.[]?.pressure?,
          .[]?.subscriber_observations[]?.pressure?]
         | map(select(. != null))) as $pressures
        | ($pressures | length > 0)
        and all($pressures[];
            (.resources.max_loans == 2)
            and (.resources.max_borrowed == 1)
            and ((.publisher.loans_in_use // 0)
                 <= .resources.max_loans)
            and ((.subscriber.borrowed_samples // 0)
                 <= .resources.max_borrowed))
    ' "${file}" >/dev/null \
        || die "mixed-soak capability pressure bounds/configuration failed: ${file}"
}

assert_stream_pressure_projection() {
    local file="$1"
    local capability="$2"
    jq -e --arg capability "${capability}" '
        .capability_instance == $capability
        and (.resources.max_loans == 2)
        and (.resources.max_borrowed == 1)
        and ((.publisher.loans_in_use // 0) <= .resources.max_loans)
        and (([.subscriber_observations[]?.pressure.subscriber.borrowed_samples // 0]
              | max // 0) <= .resources.max_borrowed)
    ' "${file}" >/dev/null \
        || die "stream pressure projection failed: ${file}"
}

printf 'RBT_PYTHON_COMPONENT_RUN mode=%s address=%s logs=%s\n' \
    "${mode}" "${composer_addr}" "${run_dir}"
start_composer composer

# Initial blocking and the mandatory Rust -> Python -> C++ graph.
start_process detector env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
    "${python}" "${detector_script}"
detector_pid="${last_pid}"
start_process viewer env RBT_COMPOSER_ADDR="${composer_addr}" "${cpp_viewer}"
viewer_pid="${last_pid}"
wait_state detector Blocked
wait_state viewer Blocked "${run_dir}/initial-blocked.ps.json"
[[ "$(line_count 'MODEL LOADED' "${run_dir}/detector.log")" == 1 ]] \
    || die "prepare did not run exactly once before initial blocking"
RBT_COMPOSER_ADDR="${composer_addr}" "${rbt_cli}" why detector \
    >"${run_dir}/initial-blocked.why"
RBT_COMPOSER_ADDR="${composer_addr}" "${rbt_cli}" why detector --json \
    >"${run_dir}/initial-blocked.why.json"
rg -F --quiet 'camera (Camera)' "${run_dir}/initial-blocked.why" \
    && rg -F --quiet 'missing/unresolved' "${run_dir}/initial-blocked.why" \
    || die "rbt why omitted the missing Python requirement"
jq -e '
    .component == "detector"
    and .component_state == "blocked"
    and (.reason_codes | index("requirement_unresolved") != null)
    and ([.requirements[]
          | select(.requirement_name == "camera"
                   and .capability_type == "Camera"
                   and .resolved == false)] | length == 1)
' "${run_dir}/initial-blocked.why.json" >/dev/null \
    || die "rbt why JSON omitted the unresolved camera requirement"

start_process camera-a env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info "${camera_a}"
camera_pid="${last_pid}"
wait_state camera-a Active
wait_state detector Active
wait_state viewer Active "${run_dir}/initial-active.ps.json"
wait_log "${run_dir}/detector.log" 'DETECTOR SAMPLE'
wait_log "${run_dir}/viewer.log" 'PYTHON_VIEWER SAMPLE'
capture_ps "${run_dir}/initial-active.ps.json"
capture_capabilities "${run_dir}/initial-active.capabilities.json"
RBT_COMPOSER_ADDR="${composer_addr}" "${rbt_cli}" graph >"${run_dir}/initial.graph"
rg -F --quiet 'camera-a -[camera:Camera:Stream]-> detector' "${run_dir}/initial.graph" \
    || die "graph lacks Rust Camera -> Python Detector"
rg -F --quiet 'detector -[detections:Detections:Stream]-> viewer' "${run_dir}/initial.graph" \
    || die "graph lacks Python Detector -> C++ Viewer"
initial_process="$(ps_component_field "${run_dir}/initial-active.ps.json" detector '.process_instance_id')"
initial_activation="$(ps_component_field "${run_dir}/initial-active.ps.json" detector '.activation_id')"
initial_capability="$(capability_field "${run_dir}/initial-active.capabilities.json" detector Detections '.capability_instance')"
capture_stream_pressure "${initial_capability}" \
    "${run_dir}/initial-detections.pressure.json"
assert_stream_pressure_projection "${run_dir}/initial-detections.pressure.json" \
    "${initial_capability}"
initial_model="$(sed -n 's/.*model_id=\([0-9][0-9]*\).*/\1/p' "${run_dir}/detector.log" | head -1)"
record_resources "${detector_pid}" "${run_dir}/resources-initial.txt"

provider_kill_ms="$(monotonic_ms)"
stop_process KILL "${camera_pid}"
wait_state detector Blocked
blocked_ms="$(monotonic_ms)"
wait_state viewer Blocked "${run_dir}/provider-loss.ps.json"
wait_log "${run_dir}/detector.log" 'DETECTOR CANCELLED'
loss_latency_ms=$((blocked_ms - provider_kill_ms))
wait_capability_query 'provider loss capability cleanup' \
    '[.[] | select(.capability_type == "Detections")] | length == 0' \
    "${run_dir}/provider-loss.capabilities.json"

start_process camera-b env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info "${camera_b}"
camera_pid="${last_pid}"
wait_state camera-b Active
wait_state detector Active
wait_state viewer Active "${run_dir}/replacement.ps.json"
wait_log "${run_dir}/detector.log" 'STALE HANDLE REJECTED'
wait_log "${run_dir}/viewer.log" 'provider=detector'
capture_ps "${run_dir}/replacement.ps.json"
capture_capabilities "${run_dir}/replacement.capabilities.json"
replacement_process="$(ps_component_field "${run_dir}/replacement.ps.json" detector '.process_instance_id')"
replacement_activation="$(ps_component_field "${run_dir}/replacement.ps.json" detector '.activation_id')"
replacement_capability="$(capability_field "${run_dir}/replacement.capabilities.json" detector Detections '.capability_instance')"
[[ "${replacement_process}" == "${initial_process}" ]] || die "replacement changed ProcessInstanceId"
[[ "${replacement_activation}" != "${initial_activation}" ]] || die "replacement reused ActivationId"
[[ "${replacement_capability}" != "${initial_capability}" ]] || die "replacement reused CapabilityInstanceId"
[[ "$(line_count 'MODEL LOADED' "${run_dir}/detector.log")" == 1 ]] || die "replacement reran prepare"
[[ "$(line_count 'DETECTOR ACTIVATED' "${run_dir}/detector.log")" == 2 ]] || die "replacement activation count is not two"
rg -F --quiet "model_id=${initial_model}" "${run_dir}/detector.log" \
    || die "model identity was not retained"
record_resources "${detector_pid}" "${run_dir}/resources-replacement.txt"

# Healthy Composer loss/recovery does not touch local activation truth.
viewer_before="$(line_count 'PYTHON_VIEWER SAMPLE' "${run_dir}/viewer.log")"
capture_ps "${run_dir}/before-composer-restart.ps.json"
capture_capabilities "${run_dir}/before-composer-restart.capabilities.json"
stop_process KILL "${composer_pid}"
deadline=$((SECONDS + timeout_seconds))
while ((SECONDS < deadline)); do
    viewer_after="$(line_count 'PYTHON_VIEWER SAMPLE' "${run_dir}/viewer.log")"
    ((viewer_after > viewer_before)) && break
    sleep 0.1
done
((viewer_after > viewer_before)) || die "payload flow stopped with Composer down"
start_composer composer-restarted
wait_ps_query 'healthy recovered graph' \
    '.phase == "running" and ([.components[] | select(.state == "ACTIVE")] | length == 3)' \
    "${run_dir}/after-composer-restart.ps.json"
wait_log "${run_dir}/composer-restarted.log" 'reconciliation: 0 deactivate, 0 activate'
capture_ps "${run_dir}/after-composer-restart.ps.json"
capture_capabilities "${run_dir}/after-composer-restart.capabilities.json"
for expression in \
    '.process_instance_id' \
    '.activation_id'; do
    before="$(ps_component_field "${run_dir}/before-composer-restart.ps.json" detector "${expression}")"
    after="$(ps_component_field "${run_dir}/after-composer-restart.ps.json" detector "${expression}")"
    [[ "${before}" == "${after}" ]] || die "healthy recovery changed ${expression}"
done
before="$(capability_field "${run_dir}/before-composer-restart.capabilities.json" detector Detections '.capability_instance')"
after="$(capability_field "${run_dir}/after-composer-restart.capabilities.json" detector Detections '.capability_instance')"
[[ "${before}" == "${after}" ]] || die "healthy recovery changed capability identity"
[[ "$(line_count 'DETECTOR ACTIVATED' "${run_dir}/detector.log")" == 2 ]] \
    || die "healthy recovery reactivated Python"

# Provider death while Composer is absent is reconciled after restart.
stop_process KILL "${composer_pid}"
stop_process KILL "${camera_pid}"
sleep 0.3
kill -0 "${detector_pid}" 2>/dev/null || die "detector exited while Composer was down"
start_composer composer-after-provider-loss
wait_state detector Blocked
wait_state viewer Blocked "${run_dir}/provider-down-recovered.ps.json"
wait_capability_query 'provider-down recovery capability cleanup' \
    '[.[] | select(.capability_type == "Detections")] | length == 0' \
    "${run_dir}/provider-down-recovered.capabilities.json"
[[ "$(line_count 'MODEL LOADED' "${run_dir}/detector.log")" == 1 ]] \
    || die "recovery reran prepare"

stop_process TERM "${detector_pid}"
[[ "${last_status}" == 0 ]] || die "detector did not stop gracefully: ${last_status}"
stop_process TERM "${viewer_pid}"
[[ "${last_status}" == 0 ]] || die "C++ viewer did not stop gracefully: ${last_status}"

# Critical Python exception revokes capability and blocks the native dependent.
start_process camera-critical env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info "${camera_a}"
camera_pid="${last_pid}"
wait_state camera-a Active
start_process detector-critical env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
    RBT_TEST_PY_FAIL_AFTER=3 "${python}" "${detector_script}"
critical_pid="${last_pid}"
start_process viewer-critical env RBT_COMPOSER_ADDR="${composer_addr}" "${cpp_viewer}"
critical_viewer_pid="${last_pid}"
wait_state detector Failed
wait_state viewer Blocked "${run_dir}/critical-failure.ps.json"
wait_capability_query 'critical failure capability cleanup' \
    '[.[] | select(.capability_type == "Detections")] | length == 0' \
    "${run_dir}/critical-failure.capabilities.json"
wait_log "${run_dir}/detector-critical.log" 'exception=RuntimeError'
wait_log "${run_dir}/detector-critical.log" 'Traceback'
RBT_COMPOSER_ADDR="${composer_addr}" "${rbt_cli}" why detector \
    >"${run_dir}/critical-failure.why"
rg -F --quiet 'critical task inference exited' "${run_dir}/critical-failure.why" \
    || die "critical failure reason omitted task name"
rg -F --quiet 'RuntimeError' "${run_dir}/critical-failure.why" \
    || die "critical failure reason omitted Python exception class"
stop_process TERM "${critical_pid}"
stop_process TERM "${critical_viewer_pid}"

# Background BaseException is reported but does not invalidate the activation.
start_process detector-background env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
    RBT_TEST_PY_BACKGROUND_FAIL_AFTER=0 "${python}" "${detector_script}"
background_pid="${last_pid}"
start_process viewer-background env RBT_COMPOSER_ADDR="${composer_addr}" "${cpp_viewer}"
background_viewer_pid="${last_pid}"
wait_state detector Active
wait_state viewer Active "${run_dir}/background-active.ps.json"
wait_log "${run_dir}/detector-background.log" 'DETECTOR BACKGROUND FAILURE'
wait_log "${run_dir}/detector-background.log" 'background task exited; activation remains unchanged'
wait_log "${run_dir}/viewer-background.log" 'PYTHON_VIEWER SAMPLE'
wait_state detector Active "${run_dir}/background-still-active.ps.json"
stop_process TERM "${background_pid}"
stop_process TERM "${background_viewer_pid}"

# Critical sibling failure wakes and joins a second Python task blocked in a native receive.
start_process critical-sibling env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
    "${python}" "${probe_script}" critical-sibling
sibling_pid="${last_pid}"
wait_state python-critical-sibling Failed
wait_log "${run_dir}/critical-sibling.log" 'PROBE SIBLING CANCELLED'
wait_log "${run_dir}/critical-sibling.log" 'intentional critical sibling failure'
stop_process TERM "${sibling_pid}"

# Cooperative bounded computation observes the same native cancellation flag.
start_process require-only env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
    "${python}" "${probe_script}" require-only
require_only_pid="${last_pid}"
wait_state python-require-only Active
wait_log "${run_dir}/require-only.log" 'PROBE SAMPLE label=require-only'
stop_process TERM "${require_only_pid}"
[[ "${last_status}" == 0 ]] || die "active receive component did not stop gracefully on SIGTERM"
wait_log "${run_dir}/require-only.log" 'PROBE RECEIVE CANCELLED label=require-only'

start_process sigint env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
    "${python}" "${probe_script}" require-only --component-id python-sigint
sigint_pid="${last_pid}"
wait_state python-sigint Active
wait_log "${run_dir}/sigint.log" 'PROBE SAMPLE label=require-only'
stop_process INT "${sigint_pid}"
[[ "${last_status}" == 0 ]] || die "active receive component did not stop gracefully on SIGINT"
wait_log "${run_dir}/sigint.log" 'PROBE RECEIVE CANCELLED label=require-only'

start_process compute env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
    "${python}" "${probe_script}" compute
compute_pid="${last_pid}"
wait_state python-compute Active
start_process loan-cancel env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
    "${python}" "${probe_script}" loan-cancel
loan_cancel_pid="${last_pid}"
wait_state python-loan-cancel Active
wait_log "${run_dir}/loan-cancel.log" 'PROBE HOLDING WRITABLE LOAN'
compute_kill_ms="$(monotonic_ms)"
stop_process KILL "${camera_pid}"
wait_state python-compute Blocked
wait_state python-loan-cancel Blocked
compute_blocked_ms="$(monotonic_ms)"
wait_log "${run_dir}/compute.log" 'PROBE COMPUTE CANCELLED'
wait_log "${run_dir}/loan-cancel.log" 'PROBE UNSENT LOAN DROPPED'
compute_latency_ms=$((compute_blocked_ms - compute_kill_ms))
stop_process TERM "${compute_pid}"
stop_process TERM "${loan_cancel_pid}"

# Stream-independent lifecycle shapes and partial activation rollback.
start_process no-stream env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
    "${python}" "${probe_script}" no-stream
no_stream_pid="${last_pid}"
wait_state python-no-stream Active
stop_process TERM "${no_stream_pid}"

# Unexpected critical normal return and worker BaseException both use the
# ordinary native supervisor path; neither may escape or terminate the process.
start_process critical-return env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
    "${python}" "${probe_script}" critical-return
critical_return_pid="${last_pid}"
wait_state python-critical-return Failed
wait_log "${run_dir}/critical-return.log" 'PROBE CRITICAL RETURN'
wait_log "${run_dir}/critical-return.log" 'outcome=Completed'
kill -0 "${critical_return_pid}" 2>/dev/null \
    || die "unexpected critical return terminated the Component process"
stop_process TERM "${critical_return_pid}"
[[ "${last_status}" == 0 ]] || die "critical-return component did not stop gracefully"

start_process critical-system-exit env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
    "${python}" "${probe_script}" critical-system-exit
critical_system_exit_pid="${last_pid}"
wait_state python-critical-system-exit Failed
wait_log "${run_dir}/critical-system-exit.log" 'exception=SystemExit'
wait_log "${run_dir}/critical-system-exit.log" 'intentional worker SystemExit'
kill -0 "${critical_system_exit_pid}" 2>/dev/null \
    || die "worker SystemExit terminated the Component process"
stop_process TERM "${critical_system_exit_pid}"
[[ "${last_status}" == 0 ]] || die "SystemExit component did not stop gracefully"

start_process background-exception-storm env \
    RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
    "${python}" "${probe_script}" background-exception-storm
background_storm_pid="${last_pid}"
wait_state python-background-exception-storm Active
wait_log "${run_dir}/background-exception-storm.log" \
    'PROBE BACKGROUND STORM raised=32 released=32'
[[ "$(line_count 'background task exited; activation remains unchanged' \
    "${run_dir}/background-exception-storm.log")" == 32 ]] \
    || die "background exception storm did not report exactly 32 supervised exits"
kill -0 "${background_storm_pid}" 2>/dev/null \
    || die "background exception storm terminated the Component process"
stop_process TERM "${background_storm_pid}"
[[ "${last_status}" == 0 ]] || die "background exception storm did not stop gracefully"

start_process provide-only env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
    "${python}" "${probe_script}" provide-only
provide_pid="${last_pid}"
wait_state python-provide-only Active "${run_dir}/provide-only.ps.json"
wait_capability_query 'provide-only capability publication' \
    '[.[] | select(.provider_component == "python-provide-only" and .capability_type == "Detections")] | length == 1' \
    "${run_dir}/provide-only.capabilities.json"
stop_process TERM "${provide_pid}"
wait_capability_query 'provide-only disconnect cleanup' \
    '[.[] | select(.capability_type == "Detections")] | length == 0' \
    "${run_dir}/provide-only-disconnect.capabilities.json"

start_process partial-fail env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
    "${python}" "${probe_script}" partial-fail
partial_pid="${last_pid}"
wait_state python-partial-fail Failed "${run_dir}/partial-failure.ps.json"
wait_capability_query 'partial activation capability rollback' \
    '[.[] | select(.provider_component == "python-partial-fail")] | length == 0' \
    "${run_dir}/partial-failure.capabilities.json"
! rg -F --quiet 'staged task started' "${run_dir}/partial-fail.log" \
    || die "partial activation started a staged task"
stop_process TERM "${partial_pid}"

start_process prepare-fail env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
    "${python}" "${probe_script}" prepare-fail
prepare_pid="${last_pid}"
wait_log "${run_dir}/prepare-fail.log" 'rbt.ComponentPreparationError'
wait_log "${run_dir}/prepare-fail.log" 'phase=prepare'
stop_process TERM "${prepare_pid}"
[[ "${last_status}" != 0 ]] || die "prepare failure exited successfully"

# Existing duplicate ComponentId quarantine remains language neutral.
start_process duplicate-a env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
    "${python}" "${probe_script}" no-stream --component-id python-duplicate
duplicate_a_pid="${last_pid}"
wait_state python-duplicate Active
start_process duplicate-b env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
    "${python}" "${probe_script}" no-stream --component-id python-duplicate
duplicate_b_pid="${last_pid}"
wait_ps_query 'duplicate Python ComponentId conflict' \
    '[.process_conflicts[] | select(.component_id == "python-duplicate")] | length == 1' \
    "${run_dir}/duplicate.ps.json"
stop_process TERM "${duplicate_b_pid}"
stop_process TERM "${duplicate_a_pid}"

printf 'RBT_PYTHON_COMPONENT_QUICK initial_activation=%s replacement_activation=%s process=%s\n' \
    "${initial_activation}" "${replacement_activation}" "${initial_process}"
printf 'RBT_PYTHON_COMPONENT_QUICK initial_capability=%s replacement_capability=%s model_loads=1\n' \
    "${initial_capability}" "${replacement_capability}"
printf 'RBT_PYTHON_COMPONENT_CANCELLATION native_wait_to_blocked_ms=%s compute_to_blocked_ms=%s\n' \
    "${loss_latency_ms}" "${compute_latency_ms}"
printf 'RBT_PYTHON_COMPONENT_QUICK critical=pass background=pass sibling=pass partial_rollback=pass duplicate=pass composer_recovery=pass\n'

if [[ "${mode}" == full ]]; then
    ((soak_seconds >= 300)) || die "full-mode soak duration must be at least 300 seconds"

    # One process, exactly one prepare, and 100 provider-driven activations.
    start_process cycles-detector env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
        RBT_TEST_PY_RETAIN_INPUT_VIEW=1 "${python}" "${detector_script}"
    cycles_detector_pid="${last_pid}"
    start_process cycles-viewer env RBT_COMPOSER_ADDR="${composer_addr}" "${cpp_viewer}"
    cycles_viewer_pid="${last_pid}"
    wait_state detector Blocked
    wait_state viewer Blocked
    base_resources=''
    final_resources=''
    for cycle in $(seq 1 100); do
        if ((cycle % 2 == 1)); then
            camera_label="camera-a-cycle-${cycle}"
            camera_component=camera-a
            camera_binary="${camera_a}"
        else
            camera_label="camera-b-cycle-${cycle}"
            camera_component=camera-b
            camera_binary="${camera_b}"
        fi
        start_process "${camera_label}" env RBT_COMPOSER_ADDR="${composer_addr}" \
            RUST_LOG=warn "${camera_binary}"
        cycle_camera_pid="${last_pid}"
        wait_state "${camera_component}" Active
        wait_state detector Active
        wait_state viewer Active
        stop_process KILL "${cycle_camera_pid}"
        wait_state detector Blocked
        wait_state viewer Blocked
        if ((cycle == 1)); then
            record_resources "${cycles_detector_pid}" "${run_dir}/cycles-resource-baseline.txt"
            base_resources="${run_dir}/cycles-resource-baseline.txt"
        fi
        if ((cycle % 10 == 0)); then
            printf 'RBT_PYTHON_COMPONENT_CYCLE cycle=%s status=pass\n' "${cycle}"
        fi
    done
    record_resources "${cycles_detector_pid}" "${run_dir}/cycles-resource-final.txt"
    final_resources="${run_dir}/cycles-resource-final.txt"
    [[ "$(line_count 'MODEL LOADED' "${run_dir}/cycles-detector.log")" == 1 ]] \
        || die "100 cycles reran prepare"
    [[ "$(line_count 'DETECTOR ACTIVATED' "${run_dir}/cycles-detector.log")" == 100 ]] \
        || die "did not observe exactly 100 Python activations"
    [[ "$(line_count 'DETECTOR CANCELLED' "${run_dir}/cycles-detector.log")" == 100 ]] \
        || die "did not join exactly 100 Python inference tasks"
    awk '
        $1 == "DETECTOR" && $2 == "ACTIVATED" {
            for (field_index = 1; field_index <= NF; ++field_index) {
                if ($field_index ~ /^activation=[0-9]+$/) {
                    split($field_index, field, "=")
                    activation = field[2] + 0
                }
            }
            if (activation != activated + 1 || (activated > 0 && cancelled != activated)) {
                exit 1
            }
            activated = activation
        }
        $1 == "DETECTOR" && $2 == "CANCELLED" {
            for (field_index = 1; field_index <= NF; ++field_index) {
                if ($field_index ~ /^activation=[0-9]+$/) {
                    split($field_index, field, "=")
                    activation = field[2] + 0
                }
            }
            if (activation != activated || cancelled + 1 != activation) {
                exit 1
            }
            cancelled = activation
        }
        END {
            if (activated != 100 || cancelled != 100) {
                exit 1
            }
        }
    ' "${run_dir}/cycles-detector.log" \
        || die "old Python task completion crossed an activation boundary"
    wait_log "${run_dir}/cycles-detector.log" 'RETAINED INPUT VIEW'
    wait_log "${run_dir}/cycles-detector.log" 'STALE HANDLE REJECTED'

    base_rss="$(resource_value "${base_resources}" rss_kib)"
    final_rss="$(resource_value "${final_resources}" rss_kib)"
    base_threads="$(resource_value "${base_resources}" threads)"
    final_threads="$(resource_value "${final_resources}" threads)"
    base_fds="$(resource_value "${base_resources}" fds)"
    final_fds="$(resource_value "${final_resources}" fds)"
    ((final_rss <= base_rss + 65536)) || die "RSS grew by more than 64 MiB"
    ((final_threads <= base_threads + 2)) || die "worker thread count grew unexpectedly"
    ((final_fds <= base_fds + 8)) || die "fd count grew unexpectedly"
    capture_ps "${run_dir}/cycles-final.ps.json"
    capture_capabilities "${run_dir}/cycles-final.capabilities.json"
    jq -e '[.[] | select(.capability_type == "Detections")] | length == 0' \
        "${run_dir}/cycles-final.capabilities.json" >/dev/null \
        || die "100 cycles left an orphan capability"
    jq -e '
        ([.components[] | select(.component_id == "detector")] | length == 1)
        and ([.components[]
              | select(.component_id == "detector")
              | .stream_pressures[]?] | length == 1)
        and ([.components[]
              | select(.component_id == "detector")
              | .stream_pressures[]?
              | select(.publisher == null
                       and (.subscriber | type) == "object"
                       and .subscriber.borrowed_samples == 1
                       and .subscriber.borrow_failures == 0)] | length == 1)
        and ([.components[]
              | select(.component_id != "detector")
              | .stream_pressures[]?
              | .subscriber?
              | select(. != null and (.borrowed_samples // 0) != 0)] | length == 0)
        and ([.components[]
              | .stream_pressures[]?
              | select(.publisher != null)] | length == 0)
    ' "${run_dir}/cycles-final.ps.json" >/dev/null \
        || die "100 cycles did not retain exactly one bounded detector borrow"
    cycles_pressure_borrowed="$(jq -r '[.components[]
        | select(.component_id == "detector")
        | .stream_pressures[]?
        | select(.subscriber != null)
        | .subscriber.borrowed_samples][0]' "${run_dir}/cycles-final.ps.json")"
    jq -e '
        ([.[]?
          | select((.capability_type == "Detections")
                   or (.capability_type == "Camera")
                   or ((.provider_component // "") | test("^camera-"))
                   or ((.provider_component // "") | test("record"; "i")))] | length == 0)
        and ([.[]? | (.subscriber_observations // [])[]?
              | select(.component_id == "detector" or .component_id == "viewer"
                       or ((.component_id // "") | test("record"; "i")))] | length == 0)
    ' "${run_dir}/cycles-final.capabilities.json" >/dev/null \
        || die "100 cycles left an orphan capability observation"
    printf 'RBT_PYTHON_COMPONENT_100_CYCLE_PRESSURE detector_stream_pressures=1 detector_borrowed_current=%s detector_borrow_failures=0 detector_publisher=none other_subscriber_borrows=0\n' \
        "${cycles_pressure_borrowed}"

    # Keep the same mixed graph processes alive for a five-minute resource
    # soak. Composer is restarted repeatedly; healthy local activation and
    # data flow must retain identity each time.
    start_process soak-camera env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=warn "${camera_a}"
    soak_camera_pid="${last_pid}"
    wait_state camera-a Active
    wait_state detector Active
    wait_state viewer Active
    capture_ps "${run_dir}/soak-before.ps.json"
    capture_capabilities "${run_dir}/soak-before.capabilities.json"
    soak_activation="$(ps_component_field "${run_dir}/soak-before.ps.json" detector '.activation_id')"
    soak_capability="$(capability_field "${run_dir}/soak-before.capabilities.json" detector Detections '.capability_instance')"
    capture_stream_pressure "${soak_capability}" \
        "${run_dir}/soak-before-detections.pressure.json"
    assert_stream_pressure_projection "${run_dir}/soak-before-detections.pressure.json" \
        "${soak_capability}"
    mixed_active_ps_query='
        .phase == "running"
        and ([.components[] | select(.state == "ACTIVE")] | length == 3)
    '
    mixed_active_capability_query='
        ([.[] | select(.capability_type == "Camera" and .message_type.name == "Image")] | length == 1)
        and ([.[] | select(.capability_type == "Detections" and .message_type.name == "PointCloud")] | length == 1)
    '
    jq -e "${mixed_active_ps_query}" "${run_dir}/soak-before.ps.json" >/dev/null \
        && jq -e "${mixed_active_capability_query}" "${run_dir}/soak-before.capabilities.json" >/dev/null \
        || die "mixed-soak initial graph did not expose Image and PointCloud capabilities"
    mixed_detector_samples_before="$(line_count 'DETECTOR SAMPLE' "${run_dir}/cycles-detector.log")"
    mixed_viewer_samples_before="$(line_count 'PYTHON_VIEWER SAMPLE' "${run_dir}/cycles-viewer.log")"

    mixed_pressure_dir="${run_dir}/mixed-soak-pressure"
    mkdir -p "${mixed_pressure_dir}"
    cp "${run_dir}/soak-before.capabilities.json" \
        "${mixed_pressure_dir}/capabilities-000.json"
    assert_mixed_pressure_capabilities "${mixed_pressure_dir}/capabilities-000.json"
    mixed_resources="${run_dir}/mixed-soak-resources.tsv"
    mixed_resource_summary="${run_dir}/mixed-soak-resource-summary.tsv"
    printf 'seconds\tlabel\trss_kib\tfds\tthreads\n' >"${mixed_resources}"
    record_mixed_resource_sample "${soak_camera_pid}" "${mixed_resources}" camera
    record_mixed_resource_sample "${cycles_detector_pid}" "${mixed_resources}" detector
    record_mixed_resource_sample "${cycles_viewer_pid}" "${mixed_resources}" viewer
    record_mixed_resource_sample "${composer_pid}" "${mixed_resources}" composer

    soak_started=$SECONDS
    start_split_process mixed-soak-record env RBT_COMPOSER_ADDR="${composer_addr}" \
        "${rbt_cli}" record --output "${run_dir}/mixed-soak.mcap" --all \
        --duration "${soak_seconds}"
    mixed_record_pid="${last_pid}"
    record_mixed_resource_sample "${mixed_record_pid}" "${mixed_resources}" recorder
    restarts=0
    mixed_snapshot_index=0
    while ((SECONDS - soak_started < soak_seconds)); do
        if ! process_is_running "${mixed_record_pid}"; then
            ((SECONDS - soak_started + 2 >= soak_seconds)) \
                || die "MCAP recorder exited early during mixed soak"
            break
        fi
        record_mixed_resource_sample "${soak_camera_pid}" "${mixed_resources}" camera
        record_mixed_resource_sample "${cycles_detector_pid}" "${mixed_resources}" detector
        record_mixed_resource_sample "${cycles_viewer_pid}" "${mixed_resources}" viewer
        record_mixed_resource_sample "${mixed_record_pid}" "${mixed_resources}" recorder
        record_mixed_resource_sample "${composer_pid}" "${mixed_resources}" composer
        mixed_snapshot_index=$((mixed_snapshot_index + 1))
        mixed_ps="${mixed_pressure_dir}/ps-$(printf '%03d' "${mixed_snapshot_index}").json"
        mixed_capabilities="${mixed_pressure_dir}/capabilities-$(printf '%03d' "${mixed_snapshot_index}").json"
        wait_ps_query "mixed-soak lifecycle sample ${mixed_snapshot_index}" \
            "${mixed_active_ps_query}" "${mixed_ps}"
        wait_capability_query "mixed-soak capability pressure sample ${mixed_snapshot_index}" \
            "${mixed_active_capability_query}" "${mixed_capabilities}"
        assert_mixed_pressure_capabilities "${mixed_capabilities}"
        sleep 5
        if (((SECONDS - soak_started) % 30 < 5)); then
            stop_process KILL "${composer_pid}"
            start_composer "soak-composer-${restarts}"
            wait_ps_query 'soak lifecycle recovery' "${mixed_active_ps_query}"
            wait_capability_query 'soak capability recovery' \
                "${mixed_active_capability_query}"
            record_mixed_resource_sample "${composer_pid}" "${mixed_resources}" composer
            restarts=$((restarts + 1))
        fi
        kill -0 "${cycles_detector_pid}" 2>/dev/null || die "detector exited during soak"
        kill -0 "${cycles_viewer_pid}" 2>/dev/null || die "viewer exited during soak"
    done
    record_mixed_resource_sample "${soak_camera_pid}" "${mixed_resources}" camera
    record_mixed_resource_sample "${cycles_detector_pid}" "${mixed_resources}" detector
    record_mixed_resource_sample "${cycles_viewer_pid}" "${mixed_resources}" viewer
    record_mixed_resource_sample "${mixed_record_pid}" "${mixed_resources}" recorder
    record_mixed_resource_sample "${composer_pid}" "${mixed_resources}" composer
    wait_ps_query 'mixed-soak final lifecycle' "${mixed_active_ps_query}" \
        "${run_dir}/soak-after.ps.json"
    wait_capability_query 'mixed-soak final capability pressure' \
        "${mixed_active_capability_query}" "${run_dir}/soak-after.capabilities.json"
    assert_mixed_pressure_capabilities "${run_dir}/soak-after.capabilities.json"
    [[ "$(ps_component_field "${run_dir}/soak-after.ps.json" detector '.activation_id')" == "${soak_activation}" ]] \
        || die "healthy soak Composer restart changed ActivationId"
    [[ "$(capability_field "${run_dir}/soak-after.capabilities.json" detector Detections '.capability_instance')" == "${soak_capability}" ]] \
        || die "healthy soak Composer restart changed CapabilityInstanceId"

    mixed_record_deadline=$((SECONDS + timeout_seconds))
    while process_is_running "${mixed_record_pid}"; do
        ((SECONDS < mixed_record_deadline)) \
            || die "MCAP recorder did not finish after ${soak_seconds}s"
        sleep 0.1
    done
    set +e
    wait "${mixed_record_pid}" 2>/dev/null
    mixed_record_status=$?
    set -e
    [[ "${mixed_record_status}" == 0 ]] \
        || die "MCAP recorder failed with exit ${mixed_record_status}; see ${run_dir}/mixed-soak-record.err"
    soak_elapsed=$((SECONDS - soak_started))
    jq -e --argjson seconds "${soak_seconds}" '
        .complete == true
        and (.channels | length == 2)
        and ([.channels[].topic | ascii_downcase | endswith("/camera")] | map(select(.)) | length == 1)
        and ([.channels[].topic | ascii_downcase | endswith("/detections")] | map(select(.)) | length == 1)
        and .messages_recorded >= ($seconds * 10)
        and .recorder_drops == 0
        and .writer_errors == 0
        and .transport_sequence_gaps == 0
        and .queue_high_water_bytes <= .queue_budget_bytes
        and all(.channels[]; .recorder_drops == 0 and .transport_sequence_gaps == 0)
    ' "${run_dir}/mixed-soak-record.json" >/dev/null \
        || die "mixed-soak MCAP summary failed completeness/channel/pressure bounds"
    summarize_mixed_resources "${mixed_resources}" "${mixed_resource_summary}" \
        || die "mixed-soak process resources exceeded bounded growth"
    for mixed_resource_label in camera detector viewer recorder composer; do
        mixed_resource_samples="$(awk -F '\t' -v label="${mixed_resource_label}" \
            '$2 == label { count++ } END { print count + 0 }' "${mixed_resources}")"
        ((mixed_resource_samples >= 3)) \
            || die "mixed-soak resource samples missing for ${mixed_resource_label}"
    done
    mixed_pressure_summary="${run_dir}/mixed-soak-pressure-summary.json"
    jq -s '
        [ .[]
          | [.[]?
             | select(.capability_type == "Camera" or .capability_type == "Detections")] as $capabilities
          | {
              publisher_loans_high_water:
                ([$capabilities[]?.pressure.publisher?.loans_high_water // 0] | max // 0),
              publisher_loan_failures:
                ([$capabilities[]?.pressure.publisher?.loan_failures // 0] | add // 0),
              publisher_send_failures:
                ([$capabilities[]?.pressure.publisher?.send_failures // 0] | add // 0),
              subscriber_borrowed_high_water:
                ([$capabilities[]?.subscriber_observations[]?.pressure.subscriber?.borrowed_high_water // 0]
                 | max // 0),
              subscriber_borrow_failures:
                ([$capabilities[]?.subscriber_observations[]?.pressure.subscriber?.borrow_failures // 0]
                 | add // 0)
            }
        ] as $samples
        | {
            snapshots: ($samples | length),
            publisher_loans_high_water: ([$samples[].publisher_loans_high_water] | max // 0),
            publisher_loan_failures: ([$samples[].publisher_loan_failures] | max // 0),
            publisher_send_failures: ([$samples[].publisher_send_failures] | max // 0),
            subscriber_borrowed_high_water: ([$samples[].subscriber_borrowed_high_water] | max // 0),
            subscriber_borrow_failures: ([$samples[].subscriber_borrow_failures] | max // 0)
        }
    ' "${mixed_pressure_dir}"/capabilities-*.json >"${mixed_pressure_summary}"
    mixed_messages="$(jq -r '.messages_recorded' "${run_dir}/mixed-soak-record.json")"
    mixed_drops="$(jq -r '.recorder_drops' "${run_dir}/mixed-soak-record.json")"
    mixed_writer_errors="$(jq -r '.writer_errors' "${run_dir}/mixed-soak-record.json")"
    mixed_gaps="$(jq -r '.transport_sequence_gaps' "${run_dir}/mixed-soak-record.json")"
    mixed_queue_high_water="$(jq -r '.queue_high_water_bytes' "${run_dir}/mixed-soak-record.json")"
    mixed_queue_budget="$(jq -r '.queue_budget_bytes' "${run_dir}/mixed-soak-record.json")"
    mixed_publisher_loans_high_water="$(jq -r '.publisher_loans_high_water' "${mixed_pressure_summary}")"
    mixed_publisher_loan_failures="$(jq -r '.publisher_loan_failures' "${mixed_pressure_summary}")"
    mixed_publisher_send_failures="$(jq -r '.publisher_send_failures' "${mixed_pressure_summary}")"
    mixed_subscriber_borrowed_high_water="$(jq -r '.subscriber_borrowed_high_water' "${mixed_pressure_summary}")"
    mixed_subscriber_borrow_failures="$(jq -r '.subscriber_borrow_failures' "${mixed_pressure_summary}")"
    mixed_detector_samples_after="$(line_count 'DETECTOR SAMPLE' "${run_dir}/cycles-detector.log")"
    mixed_viewer_samples_after="$(line_count 'PYTHON_VIEWER SAMPLE' "${run_dir}/cycles-viewer.log")"
    mixed_detector_samples=$((mixed_detector_samples_after - mixed_detector_samples_before))
    mixed_viewer_samples=$((mixed_viewer_samples_after - mixed_viewer_samples_before))
    ((mixed_detector_samples > 0)) || die "Python detector received no Image samples during mixed soak"
    ((mixed_viewer_samples > 0)) || die "native viewer received no PointCloud samples during mixed soak"

    mixed_drained_ps_query='
        .phase == "running"
        and ([.components[] | select(.state == "ACTIVE")] | length == 3)
    '
    mixed_drained_capability_query='[
        .[] | select(.capability_type == "Camera" or .capability_type == "Detections")
    ] as $active_capabilities
    | ($active_capabilities | length == 2)
      and all($active_capabilities[];
          (.pressure.publisher.loans_in_use // 0) == 0
          and all(.subscriber_observations[]?;
              (.pressure.subscriber.borrowed_samples // 0) == 0))'
    wait_ps_query 'active mixed graph pressure counters drained' \
        "${mixed_drained_ps_query}" "${run_dir}/mixed-soak-drained.ps.json"
    wait_capability_query 'active mixed graph capability counters drained' \
        "${mixed_drained_capability_query}" \
        "${run_dir}/mixed-soak-drained.capabilities.json"
    stop_process KILL "${soak_camera_pid}"
    wait_state detector Blocked
    wait_state viewer Blocked
    stop_process TERM "${cycles_detector_pid}"
    stop_process TERM "${cycles_viewer_pid}"
    wait_ps_query '100-cycle pressure owner reclamation' \
        '([.components[]? | select(.component_id == "detector" or .component_id == "viewer")] | length == 0)
         and ([.components[]?.stream_pressures[]?] | length == 0)
         and ([.components[]? | select((.component_id // "") | test("record"; "i"))] | length == 0)' \
        "${run_dir}/cycles-reclaimed.ps.json"
    wait_capability_query '100-cycle capability owner reclamation' \
        '[.[]? | select((.capability_type == "Detections")
                       or ((.provider_component // "") | test("^camera-"))
                       or ((.provider_component // "") | test("record"; "i")))] | length == 0
         and ([.[]? | (.subscriber_observations // [])[]?
               | select(.component_id == "detector" or .component_id == "viewer"
                        or ((.component_id // "") | test("record"; "i")))] | length == 0)' \
        "${run_dir}/cycles-reclaimed.capabilities.json"
    printf 'RBT_PYTHON_COMPONENT_MIXED_SOAK seconds=%s messages=%s channels=2 python_image_samples=%s native_pointcloud_samples=%s recorder_drops=%s writer_errors=%s transport_sequence_gaps=%s queue_high_water_bytes=%s queue_budget_bytes=%s loans_current_final=0 borrowed_current_final=0 publisher_loans_high_water=%s publisher_loan_failures=%s publisher_send_failures=%s subscriber_borrowed_high_water=%s subscriber_borrow_failures=%s control_failures=not_present resources_tsv=%s resource_summary=%s pressure_summary=%s\n' \
        "${soak_elapsed}" "${mixed_messages}" "${mixed_detector_samples}" \
        "${mixed_viewer_samples}" "${mixed_drops}" "${mixed_writer_errors}" \
        "${mixed_gaps}" "${mixed_queue_high_water}" "${mixed_queue_budget}" \
        "${mixed_publisher_loans_high_water}" "${mixed_publisher_loan_failures}" \
        "${mixed_publisher_send_failures}" "${mixed_subscriber_borrowed_high_water}" \
        "${mixed_subscriber_borrow_failures}" "${mixed_resources}" \
        "${mixed_resource_summary}" "${mixed_pressure_summary}"
    printf 'RBT_PYTHON_COMPONENT_100_CYCLE_RECLAIM detector_component=0 viewer_component=0 detector_pressure_streams=0 detections_topology=0 subscriber_observations=0\n'
    printf 'RBT_PYTHON_COMPONENT_100_CYCLES prepare=1 activate=100 rss_before_kib=%s rss_after_kib=%s fd_before=%s fd_after=%s threads_before=%s threads_after=%s\n' \
        "${base_rss}" "${final_rss}" "${base_fds}" "${final_fds}" \
        "${base_threads}" "${final_threads}"
    printf 'RBT_PYTHON_COMPONENT_SOAK seconds=%s composer_restarts=%s status=pass\n' \
        "${soak_elapsed}" "${restarts}"
fi

printf 'PASS: Python Component runtime %s; logs=%s\n' "${mode}" "${run_dir}"

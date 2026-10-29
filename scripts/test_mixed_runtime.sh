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

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
cpp_prefix="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"
"${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${cpp_prefix}"
cpp_build_dir="${RBT_V01_CPP_BUILD_DIR:-${repo_root}/target/v01-cpp-debug}"
composer_port="${RBT_MIXED_PORT:-17647}"
composer_addr="127.0.0.1:${composer_port}"
wait_seconds="${RBT_MIXED_TIMEOUT_SECONDS:-20}"
waitset_idle_before_kill_seconds="${RBT_WAITSET_IDLE_BEFORE_KILL_SECONDS:-1}"
pressure_enabled="${RBT_MIXED_ENABLE_PRESSURE:-0}"
pressure_pause_ms="${RBT_MIXED_PRESSURE_PAUSE_MS:-200}"
pressure_hold_ms="${RBT_MIXED_PRESSURE_HOLD_MS:-60000}"

[[ "${composer_port}" =~ ^[0-9]+$ ]] || die "RBT_MIXED_PORT must be numeric"
[[ "${wait_seconds}" =~ ^[1-9][0-9]*$ ]] || die "RBT_MIXED_TIMEOUT_SECONDS must be positive"
[[ "${waitset_idle_before_kill_seconds}" =~ ^[1-9][0-9]*$ ]] \
    || die "RBT_WAITSET_IDLE_BEFORE_KILL_SECONDS must be positive"
[[ "${pressure_enabled}" =~ ^[01]$ ]] || die "RBT_MIXED_ENABLE_PRESSURE must be 0 or 1"
[[ "${pressure_pause_ms}" =~ ^[1-9][0-9]*$ ]] || die "RBT_MIXED_PRESSURE_PAUSE_MS must be positive"
[[ "${pressure_hold_ms}" =~ ^[1-9][0-9]*$ ]] || die "RBT_MIXED_PRESSURE_HOLD_MS must be positive"
[[ -x "${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc" ]] || die "Phase4 C compiler is missing"
[[ -x "${toolchain_root}/bin/x86_64-conda-linux-gnu-g++" ]] || die "Phase4 C++ compiler is missing"
[[ -f "${cpp_prefix}/lib/cmake/iceoryx2-cxx/iceoryx2-cxxConfig.cmake" ]] || die "iceoryx2 C++ package is missing"

require_command cargo
require_command cmake
require_command awk
require_command jq
require_command timeout

export PATH="${toolchain_root}/bin${PATH:+:${PATH}}"
export CC="${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc"
export CXX="${toolchain_root}/bin/x86_64-conda-linux-gnu-g++"
export AR="${toolchain_root}/bin/x86_64-conda-linux-gnu-ar"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
export LD_LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"
export CMAKE_PREFIX_PATH="${cpp_prefix}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"

log_root="${RBT_V01_LOG_DIR:-${repo_root}/target/v01-logs}"
mkdir -p "${log_root}"
run_dir="$(mktemp -d "${log_root}/mixed.XXXXXX")"
declare -a child_pids=()
last_pid=''
last_process_status=0

cleanup() {
    local pid
    for pid in "${child_pids[@]:-}"; do
        if kill -0 "${pid}" 2>/dev/null; then
            # A test may deliberately leave a provider stopped while an
            # assertion fails. Resume it so TERM can be observed and cleanup
            # can never block on a stopped child.
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

process_state() {
    local pid="$1"
    awk '{print $3}' "/proc/${pid}/stat" 2>/dev/null || true
}

wait_for_process_exit() {
    local pid="$1"
    local deadline_ms="$2"
    local state=''
    local now_ms=''
    while :; do
        state="$(process_state "${pid}")"
        if [[ -z "${state}" || "${state}" == Z ]]; then
            return 0
        fi
        now_ms="$(monotonic_ms)"
        if ((now_ms >= deadline_ms)); then
            return 1
        fi
        sleep 0.05
    done
}

start_process() {
    local label="$1"
    shift
    printf '+ %q' "$@"
    printf ' > %s/%s.log\n' "${run_dir}" "${label}"
    "$@" >"${run_dir}/${label}.log" 2>&1 &
    last_pid="$!"
    child_pids+=("${last_pid}")
}

kill_and_wait() {
    local signal="$1"
    local pid="$2"
    local deadline_ms
    if kill -0 "${pid}" 2>/dev/null; then
        kill "-${signal}" "${pid}" 2>/dev/null || true
    fi
    deadline_ms=$(( $(monotonic_ms) + wait_seconds * 1000 ))
    if wait_for_process_exit "${pid}" "${deadline_ms}"; then
        set +e
        wait "${pid}" 2>/dev/null
        last_process_status=$?
        set -e
        return 0
    fi
    kill -KILL "${pid}" 2>/dev/null || true
    die "${signal} did not terminate pid ${pid} within ${wait_seconds}s; forced KILL"
}

inspect_json() {
    local output="$1"
    RBT_COMPOSER_ADDR="${composer_addr}" "${repo_root}/target/debug/rbt" ps --json >"${output}" 2>/dev/null
}

inspect_graph_json() {
    local output="$1"
    RBT_COMPOSER_ADDR="${composer_addr}" "${repo_root}/target/debug/rbt" graph --json >"${output}" 2>/dev/null
}

inspect_pressure_json() {
    local capability="$1"
    local output="$2"
    RBT_COMPOSER_ADDR="${composer_addr}" "${repo_root}/target/debug/rbt" stream pressure \
        "${capability}" --json >"${output}" 2>/dev/null
}

wait_for_query() {
    local description="$1"
    local query="$2"
    local deadline=$((SECONDS + wait_seconds))
    local current="${run_dir}/current.json"
    while ((SECONDS < deadline)); do
        if inspect_json "${current}" && jq -e "${query}" "${current}" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    [[ -f "${current}" ]] && jq . "${current}" >&2 || true
    die "timed out waiting for ${description}; logs=${run_dir}"
}

wait_for_pressure_query() {
    local description="$1"
    local capability="$2"
    local query="$3"
    local deadline=$((SECONDS + wait_seconds))
    local current="${run_dir}/pressure.current.json"
    while ((SECONDS < deadline)); do
        if inspect_pressure_json "${capability}" "${current}" \
            && jq -e "${query}" "${current}" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    [[ -f "${current}" ]] && jq . "${current}" >&2 || true
    die "timed out waiting for ${description}; logs=${run_dir}"
}

wait_for_log() {
    local file="$1"
    local text="$2"
    local deadline=$((SECONDS + wait_seconds))
    while ((SECONDS < deadline)); do
        if grep -Fq -- "${text}" "${file}" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    die "timed out waiting for ${text@Q} in ${file}"
}

assert_sensor_resource_report() {
    local file="$1"
    local stream_name="$2"
    grep -Fq 'resource config source: descriptor/provider transport report' "${file}" \
        || die "${stream_name} stream info did not identify a live provider resource report"
    grep -Fq 'queue depth: 1' "${file}" \
        || die "${stream_name} Sensor stream reported the wrong queue depth"
    grep -Fq 'history: 0' "${file}" \
        || die "${stream_name} Sensor stream reported the wrong history size"
    grep -Fq 'safe overflow: true' "${file}" \
        || die "${stream_name} Sensor stream reported the wrong safe-overflow policy"
    grep -Fq 'max borrowed: 1' "${file}" \
        || die "${stream_name} Sensor stream reported the wrong maximum borrowed count"
    grep -Fq 'max loans: 2' "${file}" \
        || die "${stream_name} Sensor stream reported the wrong maximum loan count"
}

assert_pressure_report() {
    local file="$1"
    local capability="$2"
    local service="$3"
    jq -e --arg capability "${capability}" --arg service "${service}" '
        .capability_instance == $capability and
        .physical_endpoint == $service and
        .["snapshot"] != null and
        .["snapshot"].service_name == .physical_endpoint and
        .["snapshot"].publisher != null and
        .publisher != null and
        (.publisher.send_successes | type == "number")
    ' "${file}" >/dev/null \
        || die "rbt stream pressure omitted live provider identity"
    for counter in send_attempts send_failures loans_in_use loan_failures; do
        jq -e --arg counter "${counter}" \
            '.publisher[$counter] | type == "number"' "${file}" >/dev/null \
            || die "rbt stream pressure omitted numeric ${counter} counter"
    done
    jq -e '.publisher.subscriber_count >= 2' "${file}" >/dev/null \
        || die "rbt stream pressure did not report the slow and stalled subscribers"
}

component_state_query() {
    local component="$1"
    local state="$2"
    case "${state}" in
        Active|Preparing|Activating|Quiescing|Declared)
            printf '[.components[] | select(.component_id == "%s" and .state == "%s")] | length == 1' \
                "${component}" "${state^^}"
            ;;
        Blocked)
            printf '[.components[] | select(.component_id == "%s" and .state == "BLOCKED" and .block_reason != null)] | length == 1' \
                "${component}"
            ;;
        Failed)
            printf '[.components[] | select(.component_id == "%s" and .state == "FAILED" and .failure_reason != null)] | length == 1' \
                "${component}"
            ;;
        *)
            die "unsupported state query: ${state}"
            ;;
    esac
}

wait_component_state() {
    local component="$1"
    local state="$2"
    wait_for_query "${component} ${state}" "$(component_state_query "${component}" "${state}")"
}

graph_capability_field() {
    local graph_file="$1"
    local provider="$2"
    local capability_type="$3"
    local field="$4"
    jq -er --arg provider "${provider}" --arg capability_type "${capability_type}" --arg field "${field}" '
        [.nodes[] | select(
            .kind == "capability" and
            .status == "active" and
            .provider_component == $provider and
            .capability_type == $capability_type
        )] as $matches |
        if ($matches | length) == 1 then
            $matches[0][$field]
        else
            error("expected exactly one active capability")
        end
    ' "${graph_file}"
}

normalise_runtime_identity() {
    local ps_file="$1"
    local graph_file="$2"
    jq -S --slurpfile graph "${graph_file}" '
        ($graph[0]) as $graph_view |
        [.components[] | . as $component | {
            component_id,
            process: .process_instance_id,
            state,
            activation: .activation_id,
            capabilities: [
                $graph_view.nodes[] |
                select(
                    .kind == "capability" and
                    .status == "active" and
                    .provider_component == $component.component_id
                ) |
                .capability_instance
            ]
        }]
    ' "${ps_file}"
}

if [[ "${RBT_SKIP_BUILD:-0}" != 1 ]]; then
    cargo build -p composer -p rbt-cli -p camera-a -p camera-b -p viewer
    if ((pressure_enabled)); then
        cargo build -p rbt-iox2 --bin mixed_pressure
    fi
    cmake -S "${repo_root}/sdk/cpp" -B "${cpp_build_dir}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_COMPILER="${CXX}" \
        -DCMAKE_PREFIX_PATH="${cpp_prefix}"
    cmake --build "${cpp_build_dir}" --target detector_component runtime_tests --parallel 2
fi

composer="${repo_root}/target/debug/composer"
camera_a="${repo_root}/target/debug/camera-a"
camera_b="${repo_root}/target/debug/camera-b"
viewer="${repo_root}/target/debug/viewer"
detector="${cpp_build_dir}/detector_component"
pressure_stalled="${repo_root}/target/debug/mixed_pressure"
for binary in "${composer}" "${camera_a}" "${camera_b}" "${viewer}" "${detector}"; do
    [[ -x "${binary}" ]] || die "required binary is missing: ${binary}"
done
if ((pressure_enabled)); then
    [[ -x "${pressure_stalled}" ]] \
        || die "pressure helper is missing: ${pressure_stalled}"
fi

printf 'RBT_MIXED_RUN address=%s logs=%s\n' "${composer_addr}" "${run_dir}"
start_process composer env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info "${composer}"
composer_pid="${last_pid}"
wait_for_query 'composer inspection endpoint' '.phase == "recovering" or .phase == "running"'

start_process camera-a env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info "${camera_a}"
camera_a_pid="${last_pid}"
wait_component_state camera-a Active

if ((pressure_enabled)); then
    start_process detector env \
        RBT_COMPOSER_ADDR="${composer_addr}" \
        RBT_TEST_PAUSE_MS="${pressure_pause_ms}" \
        RBT_TEST_TRACE_FRAMES=1 \
        "${detector}"
else
    start_process detector env RBT_COMPOSER_ADDR="${composer_addr}" "${detector}"
fi
detector_pid="${last_pid}"
wait_component_state detector Active

start_process viewer env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info "${viewer}"
viewer_pid="${last_pid}"
wait_component_state viewer Active
inspect_json "${run_dir}/initial.json"
RBT_COMPOSER_ADDR="${composer_addr}" "${repo_root}/target/debug/rbt" graph >"${run_dir}/initial.graph"
inspect_graph_json "${run_dir}/initial.graph.json"
grep -Fq 'camera-a -[camera:Camera:Stream]-> detector' "${run_dir}/initial.graph" \
    || die "initial graph lacks Rust Camera -> C++ Detector edge"
grep -Fq 'detector -[detections:Detections:Stream]-> viewer' "${run_dir}/initial.graph" \
    || die "initial graph lacks C++ Detector -> Rust Viewer edge"

initial_detector_process="$(jq -er '.components[] | select(.component_id == "detector") | .process_instance_id' "${run_dir}/initial.json")"
initial_detector_activation="$(jq -er '.components[] | select(.component_id == "detector") | .activation_id' "${run_dir}/initial.json")"
initial_detector_capability="$(graph_capability_field "${run_dir}/initial.graph.json" detector Detections capability_instance)"
initial_camera_capability="$(graph_capability_field "${run_dir}/initial.graph.json" camera-a Camera capability_instance)"
initial_camera_service="$(graph_capability_field "${run_dir}/initial.graph.json" camera-a Camera physical_endpoint)"
initial_camera_attempts=''
if ((pressure_enabled)); then
    start_process camera-a-stalled "${pressure_stalled}" stalled \
        "${initial_camera_service}" "${pressure_hold_ms}"
    camera_a_stalled_pid="${last_pid}"
    wait_for_log "${run_dir}/camera-a-stalled.log" 'MIXED_RUST_STALLED ready'
    wait_for_pressure_query 'CameraA pressure with slow and stalled subscribers' \
        "${initial_camera_capability}" \
        '.publisher.subscriber_count >= 2 and .publisher.send_attempts >= 30'
    inspect_pressure_json "${initial_camera_capability}" "${run_dir}/initial-pressure.json"
    initial_camera_attempts="$(jq -er '.publisher.send_attempts' "${run_dir}/initial-pressure.json")"
    RBT_COMPOSER_ADDR="${composer_addr}" "${repo_root}/target/debug/rbt" stream pressure \
        "${initial_camera_capability}" >"${run_dir}/camera-a.pressure"
    assert_pressure_report "${run_dir}/initial-pressure.json" \
        "${initial_camera_capability}" "${initial_camera_service}"
fi
RBT_COMPOSER_ADDR="${composer_addr}" "${repo_root}/target/debug/rbt" ps --full >"${run_dir}/initial.ps"
RBT_COMPOSER_ADDR="${composer_addr}" "${repo_root}/target/debug/rbt" capability list >"${run_dir}/initial.capabilities"
RBT_COMPOSER_ADDR="${composer_addr}" "${repo_root}/target/debug/rbt" stream info \
    "${initial_detector_capability}" >"${run_dir}/detections.stream"
RBT_COMPOSER_ADDR="${composer_addr}" "${repo_root}/target/debug/rbt" stream info \
    "${initial_camera_capability}" >"${run_dir}/camera.stream"
grep -Fq $'detector\t' "${run_dir}/initial.ps" || die "rbt ps omitted detector"
grep -Fq 'Detections' "${run_dir}/initial.capabilities" || die "rbt capability list omitted Detections"
grep -Fq 'message ABI: rbt.demo/Detection@1' "${run_dir}/detections.stream" \
    || die "rbt stream info omitted the Detection ABI"
grep -Fq 'message ABI: rbt.sensor/Image@1' "${run_dir}/camera.stream" \
    || die "mixed Camera capability is not backed by the generated Image ABI"
assert_sensor_resource_report "${run_dir}/detections.stream" Detections
assert_sensor_resource_report "${run_dir}/camera.stream" Camera
[[ "$(grep -Fc 'MODEL LOADED' "${run_dir}/detector.log")" == 1 ]] || die "C++ prepare did not run exactly once"

kill -STOP "${camera_a_pid}"
provider_idle_started_ms="$(monotonic_ms)"
sleep "${waitset_idle_before_kill_seconds}"
provider_idle_finished_ms="$(monotonic_ms)"
provider_idle_duration_ms=$((provider_idle_finished_ms - provider_idle_started_ms))
provider_kill_started_ms="$(monotonic_ms)"
kill_and_wait KILL "${camera_a_pid}"
wait_component_state detector Blocked
provider_detector_blocked_ms="$(monotonic_ms)"
provider_kill_to_blocked_ms=$((provider_detector_blocked_ms - provider_kill_started_ms))
wait_component_state viewer Blocked
RBT_COMPOSER_ADDR="${composer_addr}" "${repo_root}/target/debug/rbt" why detector >"${run_dir}/camera-missing.why"
grep -Eq 'missing(:|/unresolved)' "${run_dir}/camera-missing.why" \
    || die "rbt why did not explain the missing Camera"
grep -Fq 'last active:' "${run_dir}/camera-missing.why" || die "rbt why lost the prior activation context"
grep -Fq 'provider for camera (Camera): camera-a' "${run_dir}/camera-missing.why" \
    || die "rbt why lost the prior provider identity"

start_process camera-b env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info "${camera_b}"
camera_b_pid="${last_pid}"
wait_component_state camera-b Active
wait_component_state detector Active
wait_component_state viewer Active
inspect_json "${run_dir}/replacement.json"
inspect_graph_json "${run_dir}/replacement.graph.json"
replacement_detector_process="$(jq -er '.components[] | select(.component_id == "detector") | .process_instance_id' "${run_dir}/replacement.json")"
replacement_detector_activation="$(jq -er '.components[] | select(.component_id == "detector") | .activation_id' "${run_dir}/replacement.json")"
replacement_detector_capability="$(graph_capability_field "${run_dir}/replacement.graph.json" detector Detections capability_instance)"
replacement_camera_capability="$(graph_capability_field "${run_dir}/replacement.graph.json" camera-b Camera capability_instance)"
replacement_camera_service="$(graph_capability_field "${run_dir}/replacement.graph.json" camera-b Camera physical_endpoint)"
[[ "${replacement_detector_process}" == "${initial_detector_process}" ]] || die "provider replacement restarted the C++ process"
[[ "${replacement_detector_activation}" != "${initial_detector_activation}" ]] || die "provider replacement did not create a new C++ activation"
[[ "${replacement_detector_capability}" != "${initial_detector_capability}" ]] || die "replacement retained an activation-owned capability instance"
[[ "${replacement_camera_capability}" != "${initial_camera_capability}" ]] || die "replacement Camera retained the old capability instance"
[[ "$(grep -Fc 'MODEL LOADED' "${run_dir}/detector.log")" == 1 ]] || die "provider replacement re-ran C++ prepare"

replacement_camera_attempts=''
if ((pressure_enabled)); then
    if RBT_COMPOSER_ADDR="${composer_addr}" "${repo_root}/target/debug/rbt" stream pressure \
        "${initial_camera_capability}" --json >"${run_dir}/retired-camera.pressure" 2>&1; then
        die "retired Camera capability remained queryable after provider replacement"
    fi
    start_process camera-b-stalled "${pressure_stalled}" stalled \
        "${replacement_camera_service}" "${pressure_hold_ms}"
    camera_b_stalled_pid="${last_pid}"
    wait_for_log "${run_dir}/camera-b-stalled.log" 'MIXED_RUST_STALLED ready'
    wait_for_pressure_query 'CameraB pressure with rebound slow and stalled subscribers' \
        "${replacement_camera_capability}" \
        '.publisher.subscriber_count >= 2 and .publisher.send_attempts >= 1'
    inspect_pressure_json "${replacement_camera_capability}" "${run_dir}/replacement-pressure.json"
    replacement_camera_attempts="$(jq -er '.publisher.send_attempts' "${run_dir}/replacement-pressure.json")"
    ((replacement_camera_attempts < initial_camera_attempts)) \
        || die "replacement Camera pressure counters did not start a fresh per-capability lifetime (old=${initial_camera_attempts}, new=${replacement_camera_attempts})"
fi

inspect_json "${run_dir}/before-composer-restart.json"
inspect_graph_json "${run_dir}/before-composer-restart.graph.json"
normalise_runtime_identity "${run_dir}/before-composer-restart.json" \
    "${run_dir}/before-composer-restart.graph.json" >"${run_dir}/before-composer-restart.identity.json"
viewer_lines_before="$(grep -Fc 'VIEW component=' "${run_dir}/viewer.log" || true)"
pressure_attempts_before_restart=''
if ((pressure_enabled)); then
    inspect_pressure_json "${replacement_camera_capability}" \
        "${run_dir}/before-composer-restart.pressure.json"
    pressure_attempts_before_restart="$(jq -er '.publisher.send_attempts' \
        "${run_dir}/before-composer-restart.pressure.json")"
fi
kill_and_wait KILL "${composer_pid}"
data_deadline=$((SECONDS + wait_seconds))
while ((SECONDS < data_deadline)); do
    viewer_lines_after="$(grep -Fc 'VIEW component=' "${run_dir}/viewer.log" || true)"
    if ((viewer_lines_after > viewer_lines_before)); then
        break
    fi
    sleep 0.1
done
((viewer_lines_after > viewer_lines_before)) || die "data did not continue while composer was down"

start_process composer-restarted env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info "${composer}"
composer_restart_pid="${last_pid}"
wait_for_query 'recovered mixed graph' \
    '.phase == "running" and ([.components[] | select(.state == "ACTIVE")] | length == 3)'
wait_for_log "${run_dir}/composer-restarted.log" 'reconciliation: 0 deactivate, 0 activate'
inspect_json "${run_dir}/after-composer-restart.json"
inspect_graph_json "${run_dir}/after-composer-restart.graph.json"
normalise_runtime_identity "${run_dir}/after-composer-restart.json" \
    "${run_dir}/after-composer-restart.graph.json" >"${run_dir}/after-composer-restart.identity.json"
cmp -s "${run_dir}/before-composer-restart.identity.json" "${run_dir}/after-composer-restart.identity.json" \
    || {
        diff -u "${run_dir}/before-composer-restart.identity.json" "${run_dir}/after-composer-restart.identity.json" >&2 || true
        die "healthy composer recovery changed process/activation/capability identity"
    }
! grep -Fq 'sending Activate' "${run_dir}/composer-restarted.log" || die "healthy recovery emitted Activate"
! grep -Fq 'sending Deactivate' "${run_dir}/composer-restarted.log" || die "healthy recovery emitted Deactivate"
[[ "$(grep -Fc 'MODEL LOADED' "${run_dir}/detector.log")" == 1 ]] || die "composer restart re-ran C++ prepare"
if ((pressure_enabled)); then
    wait_for_pressure_query 'pressure counters continuing after Composer restart' \
        "${replacement_camera_capability}" \
        '.publisher.subscriber_count >= 2 and .publisher.send_attempts > '"${pressure_attempts_before_restart}"
    inspect_pressure_json "${replacement_camera_capability}" \
        "${run_dir}/camera-b-after-composer-restart.pressure.json"
    RBT_COMPOSER_ADDR="${composer_addr}" "${repo_root}/target/debug/rbt" stream pressure \
        "${replacement_camera_capability}" >"${run_dir}/camera-b-after-composer-restart.pressure"
    assert_pressure_report "${run_dir}/camera-b-after-composer-restart.pressure.json" \
        "${replacement_camera_capability}" "${replacement_camera_service}"
fi

kill -STOP "${camera_b_pid}"
shutdown_idle_started_ms="$(monotonic_ms)"
sleep "${waitset_idle_before_kill_seconds}"
shutdown_idle_finished_ms="$(monotonic_ms)"
shutdown_idle_duration_ms=$((shutdown_idle_finished_ms - shutdown_idle_started_ms))
shutdown_started_ms="$(monotonic_ms)"
kill_and_wait TERM "${detector_pid}"
shutdown_exit_status="${last_process_status}"
[[ "${shutdown_exit_status}" == 0 ]] \
    || die "blocked C++ detector did not exit cleanly after TERM (status=${shutdown_exit_status})"
shutdown_finished_ms="$(monotonic_ms)"
shutdown_latency_ms=$((shutdown_finished_ms - shutdown_started_ms))
kill -CONT "${camera_b_pid}"
wait_component_state viewer Blocked
start_process detector-failing env \
    RBT_COMPOSER_ADDR="${composer_addr}" \
    RBT_TEST_FAIL_AFTER=3 \
    "${detector}"
failing_detector_pid="${last_pid}"
wait_component_state detector Failed
wait_component_state viewer Blocked
inspect_json "${run_dir}/critical-failure.json"
inspect_graph_json "${run_dir}/critical-failure.graph.json"
jq -e '[.nodes[] | select(.kind == "capability" and .capability_type == "Detections" and .status == "active")] | length == 0' \
    "${run_dir}/critical-failure.graph.json" >/dev/null \
    || die "failed critical task left an orphan Detections capability"
RBT_COMPOSER_ADDR="${composer_addr}" "${repo_root}/target/debug/rbt" why detector >"${run_dir}/critical-failure.why"
grep -Fq 'critical task inference exited' "${run_dir}/critical-failure.why" \
    || die "rbt why omitted the critical-task cause"

rust_shutdown_started_ms="$(monotonic_ms)"
kill_and_wait TERM "${viewer_pid}"
rust_shutdown_exit_status="${last_process_status}"
[[ "${rust_shutdown_exit_status}" == 0 ]] \
    || die "blocked Rust viewer did not exit cleanly after TERM (status=${rust_shutdown_exit_status})"
rust_shutdown_finished_ms="$(monotonic_ms)"
rust_shutdown_latency_ms=$((rust_shutdown_finished_ms - rust_shutdown_started_ms))

printf 'RBT_MIXED_RESULT initial_detector_process=%s initial_activation=%s replacement_activation=%s\n' \
    "${initial_detector_process}" "${initial_detector_activation}" "${replacement_detector_activation}"
printf 'RBT_MIXED_RESULT initial_capability=%s replacement_capability=%s model_loads=1\n' \
    "${initial_detector_capability}" "${replacement_detector_capability}"
printf 'RBT_MIXED_RESULT composer_restart_deactivate=0 composer_restart_activate=0 data_continued=true\n'
printf 'RBT_WAITSET_IDLE_PROVIDER_RESULT idle_duration_ms=%s kill_to_detector_blocked_ms=%s shutdown_idle_duration_ms=%s shutdown_latency_ms=%s shutdown_exit_status=%s\n' \
    "${provider_idle_duration_ms}" "${provider_kill_to_blocked_ms}" \
    "${shutdown_idle_duration_ms}" "${shutdown_latency_ms}" "${shutdown_exit_status}"
printf 'RBT_WAITSET_RUST_SHUTDOWN_RESULT shutdown_latency_ms=%s shutdown_exit_status=%s\n' \
    "${rust_shutdown_latency_ms}" "${rust_shutdown_exit_status}"
if ((pressure_enabled)); then
    printf 'RBT_MIXED_PRESSURE_RESULT slow_subscriber=C++ stalled_subscriber=Rust old_capability=%s old_send_attempts=%s new_capability=%s new_initial_send_attempts=%s retired_state_removed=true composer_restart_counters_continued=true\n' \
        "${initial_camera_capability}" "${initial_camera_attempts}" \
        "${replacement_camera_capability}" "${replacement_camera_attempts}"
fi
printf 'RBT_MIXED_RESULT critical_failure_withdrew_detections=true viewer_blocked=true\n'
printf 'PASS: mixed-language Component runtime; logs=%s\n' "${run_dir}"

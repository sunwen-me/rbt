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
usage: scripts/soak_pressure.sh [--smoke] [--duration-seconds N] [--cycles N]

Run the pressure/recovery harness. The default duration is 300 seconds;
--smoke runs one bounded cycle. Each cycle runs the deterministic pressure
    tests, starts/stops slow and stalled subscribers, and records Linux RSS/fd/
thread observations. When the mixed graph prerequisites are available it also
runs one v0.1 soak cycle for provider replacement and Composer recovery.

Environment overrides:
  RBT_PRESSURE_DURATION_SECONDS, RBT_PRESSURE_CYCLES
  RBT_PRESSURE_LOG_DIR, RBT_PRESSURE_PORT
  RBT_PRESSURE_STEP_TIMEOUT_SECONDS, RBT_PRESSURE_TEST_TIMEOUT_SECONDS
  RBT_PRESSURE_SKIP_GRAPH=1              explicitly omit the graph-under-pressure cycle
  RBT_PRESSURE_REQUIRE_GRAPH=0           allow missing graph prerequisites
  RBT_PRESSURE_SKIP_BUILD=0              let soak_v01 build missing graph binaries
  RBT_PRESSURE_IMAGE_PUBLISHER_BINARY, RBT_PRESSURE_IMAGE_CONSUMER_BINARY
  RBT_PRESSURE_PHASE4_BINARY, RBT_PRESSURE_CPP_BINARY
EOF
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

require_positive_integer() {
    local name="$1"
    local value="$2"
    [[ "${value}" =~ ^[1-9][0-9]*$ ]] || die "${name} must be a positive integer: ${value}"
}

require_nonnegative_integer() {
    local name="$1"
    local value="$2"
    [[ "${value}" =~ ^[0-9]+$ ]] || die "${name} must be a non-negative integer: ${value}"
}

smoke=0
duration_seconds="${RBT_PRESSURE_DURATION_SECONDS:-300}"
max_cycles="${RBT_PRESSURE_CYCLES:-0}"
while (($# > 0)); do
    case "$1" in
        --smoke)
            smoke=1
            max_cycles=1
            shift
            ;;
        --duration-seconds)
            (($# >= 2)) || die "--duration-seconds requires a value"
            duration_seconds="$2"
            shift 2
            ;;
        --cycles)
            (($# >= 2)) || die "--cycles requires a value"
            max_cycles="$2"
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1"
            ;;
    esac
done

if ((smoke)); then
    max_cycles=1
fi
require_positive_integer duration_seconds "${duration_seconds}"
require_nonnegative_integer max_cycles "${max_cycles}"

step_timeout_seconds="${RBT_PRESSURE_STEP_TIMEOUT_SECONDS:-20}"
test_timeout_seconds="${RBT_PRESSURE_TEST_TIMEOUT_SECONDS:-60}"
pressure_port="${RBT_PRESSURE_PORT:-17651}"
pressure_skip_graph="${RBT_PRESSURE_SKIP_GRAPH:-0}"
pressure_require_graph="${RBT_PRESSURE_REQUIRE_GRAPH:-1}"
pressure_skip_build="${RBT_PRESSURE_SKIP_BUILD:-1}"
for setting in step_timeout_seconds test_timeout_seconds pressure_port; do
    require_positive_integer "${setting}" "${!setting}"
done
[[ "${pressure_skip_graph}" =~ ^[01]$ ]] || die "RBT_PRESSURE_SKIP_GRAPH must be 0 or 1"
[[ "${pressure_require_graph}" =~ ^[01]$ ]] || die "RBT_PRESSURE_REQUIRE_GRAPH must be 0 or 1"
[[ "${pressure_skip_build}" =~ ^[01]$ ]] || die "RBT_PRESSURE_SKIP_BUILD must be 0 or 1"
if ((pressure_skip_graph)) && [[ -z "${RBT_PRESSURE_REQUIRE_GRAPH:-}" ]]; then
    pressure_require_graph=0
fi
((pressure_port <= 65535)) || die "RBT_PRESSURE_PORT must be at most 65535"

if ((smoke)); then
    image_frames=90
    image_consumer_frames=3
    image_slow_delay_ms=200
    image_held_delay_ms=5000
    image_start_delay_ms=2000
    control_hold_ms=1000
else
    image_frames=120
    image_consumer_frames=6
    image_slow_delay_ms=250
    image_held_delay_ms=5000
    image_start_delay_ms=2000
    control_hold_ms=2000
fi
image_frames="${RBT_PRESSURE_IMAGE_FRAMES:-${image_frames}}"
image_consumer_frames="${RBT_PRESSURE_IMAGE_CONSUMER_FRAMES:-${image_consumer_frames}}"
image_slow_delay_ms="${RBT_PRESSURE_IMAGE_SLOW_DELAY_MS:-${image_slow_delay_ms}}"
image_held_delay_ms="${RBT_PRESSURE_IMAGE_HELD_DELAY_MS:-${image_held_delay_ms}}"
image_start_delay_ms="${RBT_PRESSURE_IMAGE_START_DELAY_MS:-${image_start_delay_ms}}"
control_hold_ms="${RBT_PRESSURE_CONTROL_HOLD_MS:-${control_hold_ms}}"
require_positive_integer image_frames "${image_frames}"
require_positive_integer image_consumer_frames "${image_consumer_frames}"
require_nonnegative_integer image_slow_delay_ms "${image_slow_delay_ms}"
require_positive_integer image_held_delay_ms "${image_held_delay_ms}"
require_nonnegative_integer image_start_delay_ms "${image_start_delay_ms}"
require_positive_integer control_hold_ms "${control_hold_ms}"

require_command cargo
require_command timeout
require_command awk
require_command find
require_command grep

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
cpp_prefix="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"
"${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${cpp_prefix}"
cpp_build_dir="${RBT_PRESSURE_CPP_BUILD_DIR:-${RBT_V01_CPP_BUILD_DIR:-${repo_root}/target/v01-cpp-debug}}"

if [[ -d "${toolchain_root}/bin" ]]; then
    export PATH="${toolchain_root}/bin${PATH:+:${PATH}}"
    if [[ -x "${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc" ]]; then
        export CC="${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc"
        export CXX="${toolchain_root}/bin/x86_64-conda-linux-gnu-g++"
        export AR="${toolchain_root}/bin/x86_64-conda-linux-gnu-ar"
        export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
    fi
fi
export LD_LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"

resolve_binary() {
    local override="$1"
    shift
    local candidate
    if [[ -n "${override}" ]]; then
        [[ -x "${override}" ]] || return 1
        printf '%s\n' "${override}"
        return 0
    fi
    for candidate in "$@"; do
        if [[ -x "${candidate}" ]]; then
            printf '%s\n' "${candidate}"
            return 0
        fi
    done
    return 1
}

image_publisher=''
image_consumer=''
phase4_runtime=''
cpp_interop=''
if image_publisher="$(resolve_binary "${RBT_PRESSURE_IMAGE_PUBLISHER_BINARY:-}" \
        "${repo_root}/target/release/image-publisher" \
        "${repo_root}/target/debug/image-publisher")"; then
    :
else
    image_publisher=''
fi
if image_consumer="$(resolve_binary "${RBT_PRESSURE_IMAGE_CONSUMER_BINARY:-}" \
        "${repo_root}/target/release/image-consumer" \
        "${repo_root}/target/debug/image-consumer")"; then
    :
else
    image_consumer=''
fi
if phase4_runtime="$(resolve_binary "${RBT_PRESSURE_PHASE4_BINARY:-}" \
        "${repo_root}/target/release/phase4-interop" \
        "${repo_root}/target/debug/phase4-interop")"; then
    :
else
    phase4_runtime=''
fi
if cpp_interop="$(resolve_binary "${RBT_PRESSURE_CPP_BINARY:-}" \
        "${cpp_build_dir}/interop_cpp" \
        "${repo_root}/target/phase4-cpp-build/interop_cpp" \
        "${repo_root}/target/rbt-cpp-build/interop_cpp")"; then
    :
else
    cpp_interop=''
fi
sensor_available=0
sensor_reason='image-publisher/image-consumer binaries are missing'
if [[ -n "${image_publisher}" && -n "${image_consumer}" ]]; then
    sensor_available=1
    sensor_reason='available'
fi
control_available=0
control_reason='phase4-interop/interop_cpp binaries are missing'
if [[ -n "${phase4_runtime}" && -n "${cpp_interop}" ]]; then
    control_available=1
    control_reason='available'
fi

graph_available=1
graph_reason='available'
if ((pressure_skip_graph)); then
    graph_available=0
    graph_reason='disabled by RBT_PRESSURE_SKIP_GRAPH=1'
elif [[ ! -x "${repo_root}/scripts/soak_v01.sh" \
    || ! -x "${repo_root}/scripts/test_mixed_runtime.sh" ]]; then
    graph_available=0
    graph_reason='graph recovery scripts are missing or not executable'
elif ! command -v jq >/dev/null 2>&1; then
    graph_available=0
    graph_reason='jq is missing'
elif [[ ! -x "${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc" ]]; then
    graph_available=0
    graph_reason='Phase4 compiler toolchain is missing'
elif [[ ! -f "${cpp_prefix}/lib/cmake/iceoryx2-cxx/iceoryx2-cxxConfig.cmake" ]]; then
    graph_available=0
    graph_reason='iceoryx2 C++ package is missing'
elif ((pressure_skip_build)); then
    for graph_binary in \
        "${repo_root}/target/debug/composer" \
        "${repo_root}/target/debug/camera-a" \
        "${repo_root}/target/debug/camera-b" \
        "${repo_root}/target/debug/viewer" \
        "${repo_root}/target/debug/mixed_pressure" \
        "${cpp_build_dir}/detector_component"; do
        if [[ ! -x "${graph_binary}" ]]; then
            graph_available=0
            graph_reason="graph binary is missing: ${graph_binary}"
            break
        fi
    done
fi
if ((pressure_require_graph)) && ((graph_available == 0)); then
    die "optional graph cycle is required but unavailable: ${graph_reason}"
fi

log_root="${RBT_PRESSURE_LOG_DIR:-${repo_root}/target/pressure-logs}"
mkdir -p "${log_root}"
run_dir="$(mktemp -d "${log_root}/pressure.XXXXXX")"
resource_log="${run_dir}/resources.tsv"
printf 'cycle\tcheckpoint\trole\tpid\trss_kib\tfds\tthreads\n' >"${resource_log}"

declare -a child_pids=()
declare -A pid_labels=()
last_pid=''

cleanup() {
    local pid
    for pid in "${child_pids[@]}"; do
        if kill -0 "${pid}" 2>/dev/null; then
            kill -TERM "${pid}" 2>/dev/null || true
        fi
    done
    for pid in "${child_pids[@]}"; do
        wait "${pid}" 2>/dev/null || true
    done
}
trap cleanup EXIT
trap 'exit 130' INT TERM

print_command() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
}

start_process() {
    local label="$1"
    shift
    local log_file="${run_dir}/${label}.log"
    printf 'RBT_PRESSURE_PROCESS label=%s log=%s command=' "${label}" "${log_file}"
    printf ' %q' "$@"
    printf '\n'
    "$@" >"${log_file}" 2>&1 &
    last_pid="$!"
    child_pids+=("${last_pid}")
    pid_labels["${last_pid}"]="${label}"
    printf 'RBT_PRESSURE_PROCESS label=%s pid=%s\n' "${label}" "${last_pid}"
}

wait_for_log() {
    local log_file="$1"
    local text="$2"
    local deadline=$((SECONDS + step_timeout_seconds))
    while ((SECONDS < deadline)); do
        if grep -Fq -- "${text}" "${log_file}" 2>/dev/null; then
            return 0
        fi
        sleep 0.05
    done
    cat "${log_file}" >&2 || true
    die "timed out waiting for ${text@Q} in ${log_file}"
}

forget_child() {
    local forgotten="$1"
    local -a remaining=()
    local pid
    for pid in "${child_pids[@]}"; do
        if [[ "${pid}" != "${forgotten}" ]]; then
            remaining+=("${pid}")
        fi
    done
    child_pids=("${remaining[@]}")
    unset 'pid_labels['"${forgotten}"']'
}

wait_success() {
    local pid="$1"
    local label="$2"
    local status=0
    if wait "${pid}"; then
        status=0
    else
        status=$?
    fi
    if ((status != 0)); then
        printf '%s\n' "--- ${label}.log ---" >&2
        cat "${run_dir}/${label}.log" >&2 || true
        die "${label} exited with status ${status}"
    fi
    forget_child "${pid}"
    printf 'RBT_PRESSURE_PROCESS_DONE label=%s status=0\n' "${label}"
}

kill_and_wait() {
    local signal="$1"
    local pid="$2"
    local label="${pid_labels[${pid}]:-pid-${pid}}"
    local status=0
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
        kill "-${signal}" "${pid}" 2>/dev/null || true
    fi
    if [[ -n "${pid}" ]]; then
        if wait "${pid}"; then
            status=0
        else
            status=$?
        fi
    fi
    forget_child "${pid}"
    printf 'RBT_PRESSURE_PROCESS_STOP label=%s signal=%s status=%s\n' \
        "${label}" "${signal}" "${status}"
}

record_resource() {
    local cycle="$1"
    local checkpoint="$2"
    local role="$3"
    local pid="$4"
    [[ -n "${pid}" && -r "/proc/${pid}/status" ]] || return 0
    local rss_kib fds threads
    rss_kib="$(awk '/^VmRSS:/ {print $2; exit}' "/proc/${pid}/status" 2>/dev/null || true)"
    fds="$({ find "/proc/${pid}/fd" -mindepth 1 -maxdepth 1 2>/dev/null || true; } | wc -l)"
    threads="$({ find "/proc/${pid}/task" -mindepth 1 -maxdepth 1 2>/dev/null || true; } | wc -l)"
    # The process can exit between the initial /proc check and these reads.
    # Treat that as a missed observation instead of recording a fake zero.
    [[ -n "${rss_kib}" && "${fds}" -gt 0 && "${threads}" -gt 0 ]] || return 0
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "${cycle}" "${checkpoint}" "${role}" "${pid}" \
        "${rss_kib}" "${fds}" "${threads}" >>"${resource_log}"
}

record_children_resources() {
    local cycle="$1"
    local checkpoint="$2"
    local pid
    for pid in "${child_pids[@]}"; do
        record_resource "${cycle}" "${checkpoint}" "${pid_labels[${pid}]:-unknown}" "${pid}"
    done
}

run_pressure_tests() {
    local cycle="$1"
    local log_file="${run_dir}/pressure-tests-${cycle}.log"
    local fault_log="${run_dir}/pressure-fault-${cycle}.log"
    local test_name
    local -a pressure_tests=(
        explicit_loan_pool_exhaustion_is_typed_nonblocking_and_recovers
        borrow_limit_is_distinct_from_queue_and_recovers_after_release
        control_queue_failure_can_partially_deliver_to_other_subscribers
        sensor_image_four_speed_subscribers_are_bounded_and_isolated
        retention_matrix_covers_64_bytes_and_full_hd_image
        pointcloud_retention_reaches_borrow_limit_without_starving_fast_subscriber
    )

    printf 'RBT_PRESSURE_TEST cycle=%s command=' "${cycle}"
    print_command timeout --signal=TERM --kill-after=5s "${test_timeout_seconds}s" \
        cargo test -p rbt-iox2 --test pressure_semantics -- --nocapture --test-threads=1
    if ! timeout --signal=TERM --kill-after=5s "${test_timeout_seconds}s" \
        cargo test -p rbt-iox2 --test pressure_semantics -- --nocapture --test-threads=1 \
        >"${log_file}" 2>&1; then
        printf '%s\n' "--- pressure-tests-${cycle}.log ---" >&2
        cat "${log_file}" >&2 || true
        die "deterministic pressure tests failed in cycle ${cycle}"
    fi
    for test_name in "${pressure_tests[@]}"; do
        grep -Fq "test ${test_name}" "${log_file}" \
            || die "pressure test was not executed for ${test_name}; see ${log_file}"
    done
    printf 'RBT_PRESSURE_TEST cycle=%s command=' "${cycle}"
    print_command timeout --signal=TERM --kill-after=5s "${test_timeout_seconds}s" \
        cargo test -p rbt-iox2 --test pressure_fault -- --nocapture --test-threads=1
    if ! timeout --signal=TERM --kill-after=5s "${test_timeout_seconds}s" \
        cargo test -p rbt-iox2 --test pressure_fault -- --nocapture --test-threads=1 \
        >"${fault_log}" 2>&1; then
        printf '%s\n' "--- pressure-fault-${cycle}.log ---" >&2
        cat "${fault_log}" >&2 || true
        die "process-death pressure tests failed in cycle ${cycle}"
    fi
    grep -Fq 'test test_g_subscriber_sigkill_reclaims_held_sample_and_publisher_recovers ...' "${fault_log}" \
        || die "subscriber SIGKILL recovery test was not executed; see ${fault_log}"
    grep -Fq 'test test_h_publisher_sigkill_reclaims_unsent_loans_and_fresh_publisher_delivers ...' "${fault_log}" \
        || die "publisher SIGKILL recovery test was not executed; see ${fault_log}"
    printf 'RBT_PRESSURE_TEST_RESULT cycle=%s loan_recovery=pass borrow_release=pass subscriber_sigkill=pass publisher_sigkill=pass subscriber_matrix=pass retention=pass semantics_log=%s fault_log=%s\n' \
        "${cycle}" "${log_file}" "${fault_log}"
}

run_sensor_process_cycle() {
    local cycle="$1"
    local service="rbt/pressure/${run_dir##*/}/sensor-${cycle}"
    local publisher_pid fast_pid slow_pid held_pid stalled_pid
    local replacement_held_pid replacement_stalled_pid
    local publisher_log fast_log slow_log stalled_log replacement_stalled_log
    local slow_rate_hz

    slow_rate_hz="$(awk -v delay_ms="${image_slow_delay_ms}" \
        'BEGIN { if (delay_ms == 0) print 1000; else printf "%.6f", 1000 / delay_ms }')"

    start_process "sensor-${cycle}-publisher" env \
        RBT_IMAGE_SERVICE="${service}" \
        RBT_IMAGE_FRAMES="${image_frames}" \
        RBT_IMAGE_RATE_HZ=30 \
        RBT_IMAGE_START_DELAY_MS="${image_start_delay_ms}" \
        "${image_publisher}"
    publisher_pid="${last_pid}"
    sleep 0.2
    if ! kill -0 "${publisher_pid}" 2>/dev/null; then
        cat "${run_dir}/sensor-${cycle}-publisher.log" >&2 || true
        die "sensor publisher exited before subscribers started"
    fi

    start_process "sensor-${cycle}-fast" env \
        RBT_IMAGE_SERVICE="${service}" \
        RBT_IMAGE_FRAMES="${image_consumer_frames}" \
        RBT_IMAGE_TIMEOUT_SECS="${step_timeout_seconds}" \
        RBT_IMAGE_PROCESSING_DELAY_MS=0 \
        "${image_consumer}"
    fast_pid="${last_pid}"
    start_process "sensor-${cycle}-slow" env \
        RBT_IMAGE_SERVICE="${service}" \
        RBT_IMAGE_FRAMES="${image_consumer_frames}" \
        RBT_IMAGE_TIMEOUT_SECS="${step_timeout_seconds}" \
        RBT_TEST_CONSUMER_RATE_HZ="${slow_rate_hz}" \
        "${image_consumer}"
    slow_pid="${last_pid}"
    start_process "sensor-${cycle}-held" env \
        RBT_IMAGE_SERVICE="${service}" \
        RBT_IMAGE_FRAMES="${image_consumer_frames}" \
        RBT_IMAGE_TIMEOUT_SECS="${step_timeout_seconds}" \
        RBT_TEST_HOLD_SAMPLE_MS="${image_held_delay_ms}" \
        "${image_consumer}"
    held_pid="${last_pid}"
    start_process "sensor-${cycle}-stalled" env \
        RBT_IMAGE_SERVICE="${service}" \
        RBT_IMAGE_TIMEOUT_SECS="${step_timeout_seconds}" \
        RBT_TEST_STOP_RECEIVING=1 \
        "${image_consumer}"
    stalled_pid="${last_pid}"
    wait_for_log "${run_dir}/sensor-${cycle}-held.log" 'IMAGE_CONSUMER seq='
    wait_for_log "${run_dir}/sensor-${cycle}-stalled.log" 'IMAGE_CONSUMER stalled'
    stalled_log="${run_dir}/sensor-${cycle}-stalled.log"
    grep -Eq '^IMAGE_CONSUMER stalled .* received=0$' "${stalled_log}" \
        || die "sensor stalled subscriber did not report zero received frames; see ${stalled_log}"
    record_children_resources "${cycle}" sensor-start

    sleep 1
    kill_and_wait KILL "${held_pid}"
    kill_and_wait KILL "${stalled_pid}"
    record_children_resources "${cycle}" sensor-held-release

    start_process "sensor-${cycle}-held-replacement" env \
        RBT_IMAGE_SERVICE="${service}" \
        RBT_IMAGE_FRAMES="${image_consumer_frames}" \
        RBT_IMAGE_TIMEOUT_SECS="${step_timeout_seconds}" \
        RBT_TEST_HOLD_SAMPLE_MS="${image_held_delay_ms}" \
        "${image_consumer}"
    replacement_held_pid="${last_pid}"
    start_process "sensor-${cycle}-stalled-replacement" env \
        RBT_IMAGE_SERVICE="${service}" \
        RBT_IMAGE_TIMEOUT_SECS="${step_timeout_seconds}" \
        RBT_TEST_STOP_RECEIVING=1 \
        "${image_consumer}"
    replacement_stalled_pid="${last_pid}"
    wait_for_log "${run_dir}/sensor-${cycle}-held-replacement.log" \
        'IMAGE_CONSUMER seq='
    wait_for_log "${run_dir}/sensor-${cycle}-stalled-replacement.log" \
        'IMAGE_CONSUMER stalled'
    replacement_stalled_log="${run_dir}/sensor-${cycle}-stalled-replacement.log"
    grep -Eq '^IMAGE_CONSUMER stalled .* received=0$' "${replacement_stalled_log}" \
        || die "replacement stalled subscriber did not report zero received frames; see ${replacement_stalled_log}"
    sleep 1
    kill_and_wait KILL "${replacement_held_pid}"
    kill_and_wait KILL "${replacement_stalled_pid}"
    record_children_resources "${cycle}" sensor-subscriber-restart

    publisher_log="${run_dir}/sensor-${cycle}-publisher.log"
    fast_log="${run_dir}/sensor-${cycle}-fast.log"
    slow_log="${run_dir}/sensor-${cycle}-slow.log"
    wait_success "${publisher_pid}" "sensor-${cycle}-publisher"
    wait_success "${fast_pid}" "sensor-${cycle}-fast"
    wait_success "${slow_pid}" "sensor-${cycle}-slow"
    grep -Eq 'IMAGE_CONSUMER received=[0-9]+ sequence_gaps=[1-9][0-9]*' "${slow_log}" \
        || die "slow subscriber did not observe a sequence gap; see ${slow_log}"
    grep -Fq "IMAGE_PUBLISHER sent=${image_frames} " "${publisher_log}" \
        || die "sensor publisher did not complete all ${image_frames} configured frames; see ${publisher_log}"
    grep -Eq 'recipients=[4-9][0-9]*|recipients=4' "${publisher_log}" \
        || die "sensor publisher never observed all fast/slow/held/stalled subscribers; see ${publisher_log}"
    grep -Fq "IMAGE_CONSUMER received=${image_consumer_frames} " "${fast_log}" \
        || die "fast subscriber did not complete all ${image_consumer_frames} configured frames; see ${fast_log}"
    grep -Eq '^IMAGE_PUBLISHER_PRESSURE loans_in_use=0 ' "${publisher_log}" \
        || die "sensor publisher ended with an outstanding loan; see ${publisher_log}"
    grep -Eq '^IMAGE_CONSUMER_PRESSURE borrowed_samples=0 ' "${fast_log}" \
        || die "fast subscriber ended with an outstanding borrow; see ${fast_log}"
    grep -Eq '^IMAGE_CONSUMER_PRESSURE borrowed_samples=0 ' "${slow_log}" \
        || die "slow subscriber ended with an outstanding borrow; see ${slow_log}"
    record_children_resources "${cycle}" sensor-complete
    printf 'RBT_PRESSURE_LIVE_RESULT cycle=%s sensor=pass slow_sequence_gaps=observed held_subscriber=stopped_and_restarted stalled_subscriber=stopped_and_restarted fanout_four=observed service=%s\n' \
        "${cycle}" "${service}"
}

run_control_process_cycle() {
    local cycle="$1"
    local service="rbt/pressure/${run_dir##*/}/control-${cycle}"
    local publisher_pid stalled_pid replacement_publisher_pid replacement_stalled_pid
    local publisher_log replacement_publisher_log

    start_process "control-${cycle}-publisher" "${cpp_interop}" control-publisher "${service}"
    publisher_pid="${last_pid}"
    sleep 1
    start_process "control-${cycle}-stalled" "${phase4_runtime}" control-subscriber "${service}" "${control_hold_ms}"
    stalled_pid="${last_pid}"
    record_children_resources "${cycle}" control-stalled-start
    wait_success "${publisher_pid}" "control-${cycle}-publisher"
    publisher_log="${run_dir}/control-${cycle}-publisher.log"
    grep -Eq 'queue saturation .*error=QueueFull' "${publisher_log}" \
        || die "stalled Control subscriber did not produce QueueFull; see ${publisher_log}"
    kill_and_wait KILL "${stalled_pid}"

    start_process "control-${cycle}-publisher-replacement" "${cpp_interop}" control-publisher "${service}"
    replacement_publisher_pid="${last_pid}"
    sleep 1
    start_process "control-${cycle}-stalled-replacement" "${phase4_runtime}" control-subscriber \
        "${service}" "${control_hold_ms}"
    replacement_stalled_pid="${last_pid}"
    record_children_resources "${cycle}" control-replacement-start
    wait_success "${replacement_publisher_pid}" "control-${cycle}-publisher-replacement"
    replacement_publisher_log="${run_dir}/control-${cycle}-publisher-replacement.log"
    grep -Eq 'queue saturation .*error=QueueFull' "${replacement_publisher_log}" \
        || die "replacement stalled Control subscriber did not produce QueueFull; see ${replacement_publisher_log}"
    kill_and_wait KILL "${replacement_stalled_pid}"
    record_children_resources "${cycle}" control-complete
    printf 'RBT_PRESSURE_LIVE_RESULT cycle=%s control=pass queue_full=observed stalled_subscriber=stopped_and_restarted service=%s\n' \
        "${cycle}" "${service}"
}

run_graph_cycle() {
    local cycle="$1"
    local graph_log="${run_dir}/v01-cycle-${cycle}.log"
    local pressure_graph_log="${run_dir}/pressure-graph-${cycle}.log"
    local graph_port=$((pressure_port + cycle - 1))
    ((graph_port <= 65535)) || die "graph port exceeded 65535 in cycle ${cycle}"
    local graph_log_dir="${run_dir}/v01/cycle-${cycle}"
    mkdir -p "${graph_log_dir}"

    printf 'RBT_PRESSURE_GRAPH cycle=%s port=%s mode=simultaneous-pressure command=' \
        "${cycle}" "${graph_port}"
    print_command env RBT_SKIP_BUILD="${pressure_skip_build}" \
        RBT_MIXED_PORT="${graph_port}" \
        RBT_MIXED_TIMEOUT_SECONDS="${step_timeout_seconds}" \
        RBT_MIXED_ENABLE_PRESSURE=1 \
        RBT_MIXED_PRESSURE_HOLD_MS="$((step_timeout_seconds * 3000))" \
        "${repo_root}/scripts/test_mixed_runtime.sh"
    if ! env RBT_SKIP_BUILD="${pressure_skip_build}" \
        RBT_MIXED_PORT="${graph_port}" \
        RBT_MIXED_TIMEOUT_SECONDS="${step_timeout_seconds}" \
        RBT_MIXED_ENABLE_PRESSURE=1 \
        RBT_MIXED_PRESSURE_HOLD_MS="$((step_timeout_seconds * 3000))" \
        "${repo_root}/scripts/test_mixed_runtime.sh" >"${pressure_graph_log}" 2>&1; then
        printf '%s\n' "--- pressure-graph-${cycle}.log ---" >&2
        cat "${pressure_graph_log}" >&2 || true
        die "provider/Composer recovery under simultaneous pressure failed in cycle ${cycle}"
    fi
    grep -Fq 'RBT_MIXED_RESULT composer_restart_deactivate=0 composer_restart_activate=0 data_continued=true' \
        "${pressure_graph_log}" \
        || die "pressure graph did not preserve Composer 0/0 recovery in cycle ${cycle}"
    grep -Fq 'retired_state_removed=true composer_restart_counters_continued=true' \
        "${pressure_graph_log}" \
        || die "pressure graph did not rotate/reset pressure identity in cycle ${cycle}"

    printf 'RBT_PRESSURE_GRAPH cycle=%s port=%s mode=v01-soak command=' \
        "${cycle}" "${graph_port}"
    print_command env RBT_SKIP_BUILD="${pressure_skip_build}" RBT_SOAK_PORT="${graph_port}" \
        RBT_SOAK_LOG_DIR="${graph_log_dir}" \
        RBT_SOAK_STEP_TIMEOUT_SECONDS="${step_timeout_seconds}" \
        "${repo_root}/scripts/soak_v01.sh" --cycles 1 --duration-seconds "${duration_seconds}"
    if ! env RBT_SKIP_BUILD="${pressure_skip_build}" RBT_SOAK_PORT="${graph_port}" \
        RBT_SOAK_LOG_DIR="${graph_log_dir}" \
        RBT_SOAK_STEP_TIMEOUT_SECONDS="${step_timeout_seconds}" \
        "${repo_root}/scripts/soak_v01.sh" --cycles 1 --duration-seconds "${duration_seconds}" \
        >"${graph_log}" 2>&1; then
        printf '%s\n' "--- v01-cycle-${cycle}.log ---" >&2
        cat "${graph_log}" >&2 || true
        die "v0.1 provider/recovery soak failed in pressure cycle ${cycle}"
    fi
    grep -Eq 'RBT_SOAK_CYCLE cycle=1 .* invariants=pass' "${graph_log}" \
        || die "v0.1 soak did not complete its replacement/recovery cycle ${cycle}"
    grep -Fq 'RBT_SOAK_RESULT recovery_deactivate=0 recovery_activate=0' "${graph_log}" \
        || die "v0.1 soak did not preserve Composer 0/0 recovery in cycle ${cycle}"
    grep -Fq 'orphan_capabilities=0 stale_bindings=0 duplicate_components=0 unexpected_failed=0' "${graph_log}" \
        || die "v0.1 soak invariant summary is not clean in cycle ${cycle}"
    grep -Fq 'obvious_unbounded_growth=0' "${graph_log}" \
        || die "v0.1 soak resource summary is not bounded in cycle ${cycle}"
    grep 'RBT_SOAK_RESULT' "${graph_log}" || true
    grep 'RBT_MIXED_RESULT\|RBT_MIXED_PRESSURE_RESULT' "${pressure_graph_log}" || true
    printf 'RBT_PRESSURE_GRAPH_RESULT cycle=%s simultaneous_pressure=pass provider_replacement=pass pressure_identity_rotation=pass counter_reset=pass composer_restart=pass counters_continued=pass invariants=pass resource_growth=0 pressure_log=%s soak_log=%s\n' \
        "${cycle}" "${pressure_graph_log}" "${graph_log}"
}

printf 'RBT_PRESSURE_RUN duration_seconds=%s max_cycles=%s smoke=%s log_dir=%s\n' \
    "${duration_seconds}" "${max_cycles}" "${smoke}" "${run_dir}"
printf 'RBT_PRESSURE_CAPABILITY sensor=%s (%s) control=%s (%s) graph=%s (%s)\n' \
    "${sensor_available}" "${sensor_reason}" "${control_available}" "${control_reason}" \
    "${graph_available}" "${graph_reason}"
if ((graph_available == 0)); then
    printf 'RBT_PRESSURE_GRAPH_SKIP reason=%s\n' "${graph_reason}"
fi
if ((sensor_available == 0)); then
    printf 'RBT_PRESSURE_SENSOR_SKIP reason=%s\n' "${sensor_reason}"
fi
if ((control_available == 0)); then
    printf 'RBT_PRESSURE_CONTROL_SKIP reason=%s\n' "${control_reason}"
fi

started_at="${SECONDS}"
cycle=0
pressure_test_runs=0
sensor_runs=0
control_runs=0
graph_runs=0
while true; do
    cycle=$((cycle + 1))
    printf 'RBT_PRESSURE_CYCLE_START cycle=%s elapsed_seconds=%s\n' \
        "${cycle}" "$((SECONDS - started_at))"
    run_pressure_tests "${cycle}"
    pressure_test_runs=$((pressure_test_runs + 1))

    if ((sensor_available)); then
        run_sensor_process_cycle "${cycle}"
        sensor_runs=$((sensor_runs + 1))
    fi
    if ((control_available)); then
        run_control_process_cycle "${cycle}"
        control_runs=$((control_runs + 1))
    fi
    if ((graph_available)); then
        run_graph_cycle "${cycle}"
        graph_runs=$((graph_runs + 1))
    fi

    printf 'RBT_PRESSURE_CYCLE cycle=%s elapsed_seconds=%s tests=pass sensor=%s control=%s graph=%s\n' \
        "${cycle}" "$((SECONDS - started_at))" \
        "$([[ ${sensor_available} -eq 1 ]] && printf pass || printf skipped)" \
        "$([[ ${control_available} -eq 1 ]] && printf pass || printf skipped)" \
        "$([[ ${graph_available} -eq 1 ]] && printf pass || printf skipped)"
    if ((max_cycles > 0 && cycle >= max_cycles)); then
        break
    fi
    if ((SECONDS - started_at >= duration_seconds)); then
        break
    fi
done

resource_samples=$(( $(wc -l <"${resource_log}") - 1 ))
if ((sensor_runs > 0 || control_runs > 0)); then
    ((resource_samples >= cycle * 3)) \
        || die "too few live resource samples (${resource_samples}) for ${cycle} cycles; see ${resource_log}"
fi
resource_growth_violations="$(awk -F '\t' '
    NR == 1 { next }
    {
        role = $3
        sub(/^[^-]+-[0-9]+-/, "", role)
        if (!(role in min_rss)) {
            min_rss[role] = $5
            min_fd[role] = $6
            min_threads[role] = $7
            max_rss[role] = $5
            max_fd[role] = $6
            max_threads[role] = $7
        }
        if ($5 < min_rss[role]) min_rss[role] = $5
        if ($6 < min_fd[role]) min_fd[role] = $6
        if ($7 < min_threads[role]) min_threads[role] = $7
        if ($5 > max_rss[role]) max_rss[role] = $5
        if ($6 > max_fd[role]) max_fd[role] = $6
        if ($7 > max_threads[role]) max_threads[role] = $7
    }
    END {
        violations = 0
        for (role in min_rss) {
            if (max_rss[role] - min_rss[role] > 65536 ||
                max_fd[role] - min_fd[role] > 64 ||
                max_threads[role] - min_threads[role] > 32) {
                violations++
            }
        }
        print violations + 0
    }' "${resource_log}")"
((resource_growth_violations == 0)) \
    || die "resource observations show obvious growth; see ${resource_log}"

resource_summary="${run_dir}/resource-summary.log"
awk -F '\t' '
    NR == 1 { next }
    {
        role = $3
        sub(/^[^-]+-[0-9]+-/, "", role)
        if (!(role in samples)) {
            initial_rss[role] = $5
            initial_fd[role] = $6
            initial_threads[role] = $7
            max_rss[role] = $5
            max_fd[role] = $6
            max_threads[role] = $7
        }
        samples[role]++
        final_rss[role] = $5
        final_fd[role] = $6
        final_threads[role] = $7
        if ($5 > max_rss[role]) max_rss[role] = $5
        if ($6 > max_fd[role]) max_fd[role] = $6
        if ($7 > max_threads[role]) max_threads[role] = $7
    }
    END {
        for (role in samples) {
            printf "RBT_PRESSURE_RESOURCE role=%s samples=%d initial_rss_kib=%s final_rss_kib=%s max_rss_kib=%s initial_fds=%s final_fds=%s max_fds=%s initial_threads=%s final_threads=%s max_threads=%s\n",
                role, samples[role], initial_rss[role], final_rss[role], max_rss[role],
                initial_fd[role], final_fd[role], max_fd[role], initial_threads[role],
                final_threads[role], max_threads[role]
        }
    }' "${resource_log}" | sort >"${resource_summary}"
cat "${resource_summary}"

pressure_summary="${run_dir}/pressure-summary.log"
if ((sensor_runs > 0)); then
    awk '
        /^IMAGE_PUBLISHER_PRESSURE |^IMAGE_CONSUMER_PRESSURE / {
            for (field = 2; field <= NF; ++field) {
                split($field, pair, "=")
                value[pair[1]] += pair[2]
                if (pair[1] == "loans_high_water" && pair[2] > loan_high) loan_high = pair[2]
                if (pair[1] == "borrowed_high_water" && pair[2] > borrow_high) borrow_high = pair[2]
            }
        }
        END {
            printf "RBT_PRESSURE_METRIC_FINAL loans_current=%d borrowed_current=%d loans_high_water=%d borrowed_high_water=%d loan_failures=%d borrow_limit_hits=%d send_failures=%d queue_full=%d unable_to_deliver=%d readiness_notify_failures=%d\n",
                value["loans_in_use"], value["borrowed_samples"], loan_high,
                borrow_high, value["loan_failures"], value["borrow_failures"],
                value["send_failures"], value["queue_full"],
                value["unable_to_deliver"], value["readiness_notify_failures"]
        }' "${run_dir}"/sensor-*.log >"${pressure_summary}"
else
    printf 'RBT_PRESSURE_METRIC_FINAL sensor_metrics=not-run reason=%s\n' \
        "${sensor_reason}" >"${pressure_summary}"
fi
cat "${pressure_summary}"

printf 'RBT_PRESSURE_RESULT duration_seconds=%s cycles=%s tests=%s sensor_live_cycles=%s control_live_cycles=%s graph_cycles=%s\n' \
    "$((SECONDS - started_at))" "${cycle}" "${pressure_test_runs}" \
    "${sensor_runs}" "${control_runs}" "${graph_runs}"
printf 'RBT_PRESSURE_RESULT loan_recovery=verified borrow_release=verified subscriber_sigkill_recovery=verified publisher_sigkill_recovery=verified slow_stalled_start_stop=%s\n' \
    "$(((sensor_runs > 0 || control_runs > 0)) && printf verified || printf not-run)"
if ((graph_runs > 0)); then
    printf 'RBT_PRESSURE_RESULT orphan_capabilities=0 stale_bindings=0 provider_replacement=verified composer_restart=verified\n'
else
    printf 'RBT_PRESSURE_RESULT graph_invariants=not-run reason=%s\n' "${graph_reason}"
fi
printf 'RBT_PRESSURE_RESULT resource_samples=%s obvious_unbounded_growth=%s resource_log=%s\n' \
    "${resource_samples}" "${resource_growth_violations}" "${resource_log}"
printf 'PASS: pressure soak; logs=%s\n' "${run_dir}"

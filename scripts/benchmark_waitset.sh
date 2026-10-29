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
usage: scripts/benchmark_waitset.sh [--smoke]

Compare the preserved polling benchmark with the current WaitSet benchmark.
Both strategies use the same service topology and benchmark arguments.  A
normal run records a 10 second no-data idle sample, a 500-sample 100 Hz
latency case, and the supported Sensor payload/fanout matrix.  --smoke keeps
the loaded matrix to 64 B and fanouts 1 and 2 while retaining idle and
latency coverage.

Environment overrides:
  RBT_WAITSET_BASELINE_BINARY       preserved polling binary
                                    (default: target/waitset-baseline/rbt-bench-polling)
  RBT_WAITSET_CURRENT_BINARY        current WaitSet binary
                                    (default: target/release/rbt-bench)
  RBT_WAITSET_BENCH_LOG_DIR         retained output root
                                    (default: target/waitset-bench-logs)
  RBT_WAITSET_IDLE_SECONDS          idle sample duration (default: 10)
  RBT_WAITSET_LATENCY_COUNT         measured latency samples (default: 500)
  RBT_WAITSET_LATENCY_HZ            latency send rate, 10..100 (default: 100)
  RBT_WAITSET_COUNT                 loaded measured samples (default: 20)
  RBT_WAITSET_WARMUP                loaded warmup samples (default: 3)
  RBT_WAITSET_TIMEOUT_MS            open/receive timeout (default: 30000)
  RBT_WAITSET_STARTUP_DELAY_MS      publisher startup delay (default: 500)
  RBT_WAITSET_INTERVAL_US           minimum loaded send interval (default: 1000;
                                    fanout and large payloads are paced for
                                    full validation)
  RBT_WAITSET_SKIP_BUILD=1          require the current binary to exist

RBT_WAITSET_BASELINE_BINARY and RBT_WAITSET_CURRENT_BINARY may point to
already-built executables.  Results and per-strategy process logs are never
overwritten by this script; every run gets a unique directory.
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

print_command() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
}

smoke=0
if (($# > 1)); then
    usage >&2
    exit 2
fi
if (($# == 1)); then
    case "$1" in
        --smoke)
            smoke=1
            ;;
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

require_command awk
require_command cargo
require_command grep
require_command rustc
require_command sleep
require_command sha256sum
require_command timeout
require_command getconf
require_command find
require_command date

idle_seconds="${RBT_WAITSET_IDLE_SECONDS:-10}"
latency_count="${RBT_WAITSET_LATENCY_COUNT:-500}"
latency_hz="${RBT_WAITSET_LATENCY_HZ:-100}"
loaded_count="${RBT_WAITSET_COUNT:-20}"
warmup="${RBT_WAITSET_WARMUP:-3}"
timeout_ms="${RBT_WAITSET_TIMEOUT_MS:-30000}"
startup_delay_ms="${RBT_WAITSET_STARTUP_DELAY_MS:-500}"
interval_us="${RBT_WAITSET_INTERVAL_US:-1000}"
validation_floor_bytes_per_us=100

require_positive_integer RBT_WAITSET_IDLE_SECONDS "${idle_seconds}"
require_positive_integer RBT_WAITSET_LATENCY_COUNT "${latency_count}"
((latency_count >= 500)) || die "RBT_WAITSET_LATENCY_COUNT must be at least 500"
require_positive_integer RBT_WAITSET_LATENCY_HZ "${latency_hz}"
((latency_hz >= 10 && latency_hz <= 100)) || die "RBT_WAITSET_LATENCY_HZ must be between 10 and 100"
require_positive_integer RBT_WAITSET_COUNT "${loaded_count}"
require_nonnegative_integer RBT_WAITSET_WARMUP "${warmup}"
require_positive_integer RBT_WAITSET_TIMEOUT_MS "${timeout_ms}"
require_nonnegative_integer RBT_WAITSET_STARTUP_DELAY_MS "${startup_delay_ms}"
require_nonnegative_integer RBT_WAITSET_INTERVAL_US "${interval_us}"

loaded_interval_for_payload() {
    local payload="$1"
    local fanout="$2"
    local validation_floor_us=$(((payload + validation_floor_bytes_per_us - 1) / validation_floor_bytes_per_us))
    local fanout_floor_us=$((fanout * 1000))
    local effective_interval_us="${interval_us}"
    ((validation_floor_us > effective_interval_us)) \
        && effective_interval_us="${validation_floor_us}"
    ((fanout_floor_us > effective_interval_us)) \
        && effective_interval_us="${fanout_floor_us}"
    printf '%s\n' "${effective_interval_us}"
}

[[ -d /proc ]] || die "the WaitSet benchmark requires Linux /proc process statistics"
[[ -r /proc/self/stat ]] || die "the WaitSet benchmark requires readable /proc process statistics"

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
if [[ -d "${toolchain_root}/bin" ]]; then
    toolchain_bin="${toolchain_root}/bin"
    export PATH="${toolchain_bin}${PATH:+:${PATH}}"
    if [[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-gcc" ]]; then
        export CC="${toolchain_bin}/x86_64-conda-linux-gnu-gcc"
        export CXX="${toolchain_bin}/x86_64-conda-linux-gnu-g++"
        export AR="${toolchain_bin}/x86_64-conda-linux-gnu-ar"
        export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
        export LD_LIBRARY_PATH="${toolchain_root}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
    fi
fi

baseline_binary="${RBT_WAITSET_BASELINE_BINARY:-${repo_root}/target/waitset-baseline/rbt-bench-polling}"
current_binary="${RBT_WAITSET_CURRENT_BINARY:-${repo_root}/target/release/rbt-bench}"
[[ -x "${baseline_binary}" ]] \
    || die "preserved polling benchmark is missing or not executable: ${baseline_binary}"

if [[ "${RBT_WAITSET_SKIP_BUILD:-0}" == 1 ]]; then
    [[ -x "${current_binary}" ]] \
        || die "RBT_WAITSET_SKIP_BUILD=1 but current WaitSet benchmark is missing: ${current_binary}"
else
    if [[ -z "${RBT_WAITSET_CURRENT_BINARY:-}" ]]; then
        print_command cargo build --release -p rbt-bench
        cargo build --release -p rbt-bench
    fi
fi
[[ -x "${current_binary}" ]] \
    || die "current WaitSet benchmark is missing or not executable: ${current_binary}"

baseline_hash="$(sha256sum "${baseline_binary}" | awk '{print $1}')"
current_hash="$(sha256sum "${current_binary}" | awk '{print $1}')"
[[ "${baseline_hash}" != "${current_hash}" ]] \
    || die "polling and WaitSet benchmark binaries are byte-identical"

log_root="${RBT_WAITSET_BENCH_LOG_DIR:-${repo_root}/target/waitset-bench-logs}"
mkdir -p "${log_root}"
run_id="waitset-$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="${log_root}/${run_id}"
mkdir -p "${run_dir}/polling" "${run_dir}/waitset"

declare -a child_pids=()
declare -A child_labels=()
declare -A child_logs=()

cleanup() {
    local pid
    for pid in "${child_pids[@]:-}"; do
        if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
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

forget_pid() {
    local pid="$1"
    local -a remaining=()
    local candidate
    for candidate in "${child_pids[@]:-}"; do
        if [[ "${candidate}" != "${pid}" ]]; then
            remaining+=("${candidate}")
        fi
    done
    child_pids=("${remaining[@]}")
    unset 'child_labels[$pid]'
    unset 'child_logs[$pid]'
}

start_process() {
    local label="$1"
    local log_file="$2"
    shift 2
    : >"${log_file}"
    {
        printf 'RBT_WAITSET_PROCESS label=%s command=' "${label}"
        print_command "$@"
    } >>"${log_file}"
    print_command "$@"
    "$@" >>"${log_file}" 2>&1 &
    local pid="$!"
    child_pids+=("${pid}")
    child_labels["${pid}"]="${label}"
    child_logs["${pid}"]="${log_file}"
    last_pid="${pid}"
}

wait_ready() {
    local pid="$1"
    local log_file="${child_logs[${pid}]}"
    local label="${child_labels[${pid}]}"
    local deadline=$((SECONDS + (timeout_ms + startup_delay_ms + 5000) / 1000 + 10))
    while ((SECONDS < deadline)); do
        if grep -Fq 'RBT_BENCH_READY' "${log_file}" 2>/dev/null; then
            return 0
        fi
        if ! kill -0 "${pid}" 2>/dev/null; then
            wait_process "${pid}" || true
            die "${label} exited before RBT_BENCH_READY; see ${log_file}"
        fi
        sleep 0.02
    done
    die "timed out waiting for ${label} RBT_BENCH_READY; see ${log_file}"
}

validate_subscriber_strategy() {
    local strategy="$1"
    local log_file="$2"
    if [[ "${strategy}" == waitset ]]; then
        grep -Eq '^RBT_BENCH_READY role=subscriber .*strategy=waitset([[:space:]]|$)' "${log_file}" \
            || die "WaitSet subscriber omitted strategy=waitset provenance; see ${log_file}"
    else
        if grep -Eq '^RBT_BENCH_READY role=subscriber .*strategy=waitset([[:space:]]|$)' "${log_file}"; then
            die "polling baseline unexpectedly reports strategy=waitset; see ${log_file}"
        fi
    fi
}

require_numeric_value() {
    local description="$1"
    local value="$2"
    [[ "${value}" =~ ^[0-9]+([.][0-9]+)?$ ]] \
        || die "${description} must be numeric, got ${value:-<empty>}"
}

require_latency_percentiles() {
    local description="$1"
    local p50="$2"
    local p90="$3"
    local p99="$4"
    local maximum="$5"

    require_numeric_value "${description} p50_latency_us" "${p50}"
    require_numeric_value "${description} p90_latency_us" "${p90}"
    require_numeric_value "${description} p99_latency_us" "${p99}"
    require_numeric_value "${description} max_latency_us" "${maximum}"
    if ! awk -v p50="${p50}" -v p90="${p90}" -v p99="${p99}" \
        -v maximum="${maximum}" \
        'BEGIN {
            exit !((p50 + 0) <= (p90 + 0) &&
                   (p90 + 0) <= (p99 + 0) &&
                   (p99 + 0) <= (maximum + 0))
        }'; then
        die "${description} latency percentiles must be ordered p50<=p90<=p99<=max " \
            "(p50=${p50} p90=${p90} p99=${p99} max=${maximum})"
    fi
}

require_latency_percentile_pair() {
    local description="$1"
    local p50="$2"
    local p99="$3"

    require_numeric_value "${description} p50_latency_us" "${p50}"
    require_numeric_value "${description} p99_latency_us" "${p99}"
    if ! awk -v p50="${p50}" -v p99="${p99}" \
        'BEGIN { exit !((p50 + 0) <= (p99 + 0)) }'; then
        die "${description} latency percentiles must be ordered p50<=p99 " \
            "(p50=${p50} p99=${p99})"
    fi
}

wait_process() {
    local pid="$1"
    local label="${child_labels[${pid}]:-unknown}"
    local log_file="${child_logs[${pid}]:-}"
    local status=0
    wait "${pid}" || status=$?
    forget_pid "${pid}"
    if [[ -n "${log_file}" ]]; then
        cat "${log_file}"
    fi
    if ((status != 0)); then
        printf 'ERROR: %s failed with exit %s; see %s\n' "${label}" "${status}" "${log_file}" >&2
        return "${status}"
    fi
    return 0
}

kill_process() {
    local pid="$1"
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
        kill -TERM "${pid}" 2>/dev/null || true
    fi
    wait_process "${pid}" >/dev/null 2>&1 || true
}

proc_snapshot() {
    local pid="$1"
    local stat_file="/proc/${pid}/stat"
    local status_file="/proc/${pid}/status"
    [[ -r "${stat_file}" && -r "${status_file}" ]] || return 1

    local cpu_ticks
    cpu_ticks="$(awk '{print $14, $15}' "${stat_file}")" || return 1
    local utime_ticks stime_ticks
    read -r utime_ticks stime_ticks <<<"${cpu_ticks}"
    [[ "${utime_ticks}" =~ ^[0-9]+$ && "${stime_ticks}" =~ ^[0-9]+$ ]] || return 1

    local rss_kib threads voluntary involuntary
    rss_kib="$(awk '/^VmRSS:/ {print $2; exit}' "${status_file}")"
    threads="$(awk '/^Threads:/ {print $2; exit}' "${status_file}")"
    voluntary="$(awk '/^voluntary_ctxt_switches:/ {print $2; exit}' "${status_file}")"
    involuntary="$(awk '/^nonvoluntary_ctxt_switches:/ {print $2; exit}' "${status_file}")"
    local fds
    fds="$(find "/proc/${pid}/fd" -mindepth 1 -maxdepth 1 -print 2>/dev/null | wc -l)"
    rss_kib="${rss_kib:-0}"
    threads="${threads:-0}"
    voluntary="${voluntary:-0}"
    involuntary="${involuntary:-0}"
    fds="${fds//[[:space:]]/}"
    [[ "${rss_kib}" =~ ^[0-9]+$ && "${threads}" =~ ^[0-9]+$ ]] || return 1
    [[ "${voluntary}" =~ ^[0-9]+$ && "${involuntary}" =~ ^[0-9]+$ ]] || return 1
    [[ "${fds}" =~ ^[0-9]+$ ]] || return 1
    printf '%s %s %s %s %s %s %s %s\n' \
        "${utime_ticks}" "${stime_ticks}" "${rss_kib}" "${fds}" \
        "${threads}" "${voluntary}" "${involuntary}" "${pid}"
}

summary_result_field() {
    local file="$1"
    local record="$2"
    local key="$3"
    awk -v record="${record}" -v key="${key}" '
        $1 == record {
            for (field = 1; field <= NF; ++field) {
                if (index($field, key "=") == 1) {
                    print substr($field, length(key) + 2)
                    exit
                }
            }
        }
    ' "${file}"
}

summary_result_field_for_subscriber() {
    local file="$1"
    local subscriber="$2"
    local key="$3"
    awk -v subscriber="${subscriber}" -v key="${key}" '
        $1 == "RBT_WAITSET_LOADED_SUBSCRIBER" {
            matches = 0
            for (field = 1; field <= NF; ++field) {
                if ($field == "subscriber=" subscriber) {
                    matches = 1
                }
            }
            if (matches) {
                for (field = 1; field <= NF; ++field) {
                    if (index($field, key "=") == 1) {
                        print substr($field, length(key) + 2)
                        exit
                    }
                }
            }
        }
    ' "${file}"
}

bench_result_field() {
    local file="$1"
    local role="$2"
    local key="$3"
    awk -v role="${role}" -v key="${key}" '
        $1 == "RBT_BENCH_RESULT" {
            has_role = 0
            for (field = 2; field <= NF; ++field) {
                if ($field == "role=" role) {
                    has_role = 1
                }
            }
            if (has_role) {
                for (field = 2; field <= NF; ++field) {
                    if (index($field, key "=") == 1) {
                        print substr($field, length(key) + 2)
                        exit
                    }
                }
            }
        }
    ' "${file}"
}

result_is_valid() {
    local file="$1"
    local role="$2"
    local key="$3"
    local minimum="$4"
    local value
    value="$(bench_result_field "${file}" "${role}" "${key}")"
    require_numeric_value "${file} ${key} for role=${role}" "${value}"
    awk -v value="${value}" -v minimum="${minimum}" \
        'BEGIN { exit !(value >= minimum) }' \
        || die "${file} ${key}=${value} is below ${minimum}"
    printf '%s\n' "${value}"
}

print_host_metadata() {
    local cpu_model logical_cpus os_name kernel
    cpu_model="$(awk -F: '/^model name/ { sub(/^[[:space:]]*/, "", $2); print $2; exit }' /proc/cpuinfo 2>/dev/null)"
    logical_cpus="$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf 'unknown')"
    os_name="$(sed -n 's/^PRETTY_NAME=//p' /etc/os-release 2>/dev/null | tr -d '"')"
    kernel="$(uname -srm 2>/dev/null || printf 'unknown')"
    printf 'RBT_WAITSET_BENCH_HOST cpu_model=%s logical_cpus=%s os=%s kernel=%s\n' \
        "${cpu_model:-unknown}" "${logical_cpus}" "${os_name:-unknown}" "${kernel}"
    printf 'RBT_WAITSET_BENCH_TOOLCHAIN root=%s rust=%s\n' \
        "${toolchain_root}" "$(rustc --version)"
}

run_idle_case() {
    local strategy="$1"
    local binary="$2"
    local strategy_dir="${run_dir}/${strategy}"
    local service="rbt/waitset/${run_id}/idle/${strategy}"
    local publisher_log="${strategy_dir}/idle-publisher.log"
    local subscriber_log="${strategy_dir}/idle-subscriber.log"
    local result_file="${strategy_dir}/idle.txt"
    local publisher_pid subscriber_pid
    local publisher_startup_ms=$((idle_seconds * 1000 + 5000))
    local idle_timeout_ms=$((idle_seconds * 1000 + 10000))

    printf 'RBT_WAITSET_IDLE_CASE strategy=%s duration_s=%s service=%s\n' \
        "${strategy}" "${idle_seconds}" "${service}"
    # Snapshot the benchmark process itself. Wrapping this role in `timeout`
    # would make /proc report the watchdog's CPU and RSS instead.
    start_process "${strategy}-idle-publisher" "${publisher_log}" \
        "${binary}" publisher \
        --service "${service}" --profile sensor --message image --payload 64 \
        --count 1 --warmup 0 --subscriber-count 1 \
        --timeout-ms "${idle_timeout_ms}" --startup-delay-ms "${publisher_startup_ms}" \
        --interval-us 0
    publisher_pid="${last_pid}"
    wait_ready "${publisher_pid}"

    start_process "${strategy}-idle-subscriber" "${subscriber_log}" \
        "${binary}" subscriber \
        --service "${service}" --profile sensor --message image --payload 64 \
        --count 1 --warmup 0 --subscriber-count 1 \
        --timeout-ms "${idle_timeout_ms}" --startup-delay-ms 0 --interval-us 0
    subscriber_pid="${last_pid}"
    wait_ready "${subscriber_pid}"
    validate_subscriber_strategy "${strategy}" "${subscriber_log}"
    sleep 1
    kill -0 "${publisher_pid}" 2>/dev/null \
        || die "${strategy} idle publisher exited before connectivity sampling; see ${publisher_log}"

    local ticks_per_second="$(getconf CLK_TCK)"
    [[ "${ticks_per_second}" =~ ^[1-9][0-9]*$ ]] || die "getconf CLK_TCK returned an invalid value"
    local before after
    before="$(proc_snapshot "${subscriber_pid}")" \
        || die "could not read subscriber /proc statistics before idle sample"
    sleep "${idle_seconds}"
    after="$(proc_snapshot "${subscriber_pid}")" \
        || die "subscriber exited before the ${idle_seconds}s idle sample completed"
    kill -0 "${publisher_pid}" 2>/dev/null \
        || die "${strategy} idle publisher exited during the sample; see ${publisher_log}"

    local bu bv br bf bt bc bi bp
    local au av ar af at ac ai ap
    read -r bu bv br bf bt bc bi bp <<<"${before}"
    read -r au av ar af at ac ai ap <<<"${after}"
    local user_cpu system_cpu voluntary_delta involuntary_delta
    user_cpu="$(awk -v ticks="$((au - bu))" -v hz="${ticks_per_second}" \
        'BEGIN { printf "%.6f", ticks / hz }')"
    system_cpu="$(awk -v ticks="$((av - bv))" -v hz="${ticks_per_second}" \
        'BEGIN { printf "%.6f", ticks / hz }')"
    voluntary_delta=$((ac - bc))
    involuntary_delta=$((ai - bi))
    {
        printf 'RBT_WAITSET_IDLE strategy=%s duration_s=%s user_cpu_s=%s system_cpu_s=%s ' \
            "${strategy}" "${idle_seconds}" "${user_cpu}" "${system_cpu}"
        printf 'rss_kib=%s rss_start_kib=%s rss_delta_kib=%s threads=%s fds=%s ' \
            "${ar}" "${br}" "$((ar - br))" "${at}" "${af}"
        printf 'voluntary_ctx_switches=%s involuntary_ctx_switches=%s ' \
            "${voluntary_delta}" "${involuntary_delta}"
        printf 'sample_pid=%s publisher_log=%s subscriber_log=%s\n' \
            "${subscriber_pid}" "${publisher_log}" "${subscriber_log}"
    } | tee "${result_file}"

    kill_process "${subscriber_pid}"
    kill_process "${publisher_pid}"
    grep -Fq 'RBT_BENCH_READY' "${subscriber_log}" \
        || die "${strategy} idle subscriber never became ready"
}

run_latency_case() {
    local strategy="$1"
    local binary="$2"
    local strategy_dir="${run_dir}/${strategy}"
    local service="rbt/waitset/${run_id}/latency/${strategy}"
    local publisher_log="${strategy_dir}/latency-publisher.log"
    local subscriber_log="${strategy_dir}/latency-subscriber.log"
    local interval_latency_us=$((1000000 / latency_hz))
    local case_timeout_seconds=$((timeout_ms / 1000 + latency_count / latency_hz + 30))
    local publisher_pid subscriber_pid
    ((case_timeout_seconds < 30)) && case_timeout_seconds=30

    printf 'RBT_WAITSET_LATENCY_CASE strategy=%s count=%s hz=%s payload=64 service=%s\n' \
        "${strategy}" "${latency_count}" "${latency_hz}" "${service}"
    start_process "${strategy}-latency-publisher" "${publisher_log}" \
        timeout --signal=TERM --kill-after=2s "${case_timeout_seconds}s" \
        "${binary}" publisher \
        --service "${service}" --profile sensor --message image --payload 64 \
        --count "${latency_count}" --warmup 10 --subscriber-count 1 \
        --timeout-ms "${timeout_ms}" --startup-delay-ms "${startup_delay_ms}" \
        --interval-us "${interval_latency_us}"
    publisher_pid="${last_pid}"
    wait_ready "${publisher_pid}"
    start_process "${strategy}-latency-subscriber" "${subscriber_log}" \
        timeout --signal=TERM --kill-after=2s "${case_timeout_seconds}s" \
        "${binary}" subscriber \
        --service "${service}" --profile sensor --message image --payload 64 \
        --count "${latency_count}" --warmup 10 --subscriber-count 1 \
        --timeout-ms "${timeout_ms}" --startup-delay-ms 0 --interval-us 0
    subscriber_pid="${last_pid}"
    wait_ready "${subscriber_pid}"
    validate_subscriber_strategy "${strategy}" "${subscriber_log}"

    local failed=0
    wait_process "${publisher_pid}" || failed=1
    wait_process "${subscriber_pid}" || failed=1
    ((failed == 0)) || die "${strategy} latency case failed"

    local samples p50 p90 p99 max_latency
    samples="$(result_is_valid "${subscriber_log}" subscriber latency_samples "${latency_count}")"
    p50="$(bench_result_field "${subscriber_log}" subscriber p50_latency_us)"
    p90="$(bench_result_field "${subscriber_log}" subscriber p90_latency_us)"
    p99="$(bench_result_field "${subscriber_log}" subscriber p99_latency_us)"
    max_latency="$(bench_result_field "${subscriber_log}" subscriber max_latency_us)"
    require_latency_percentiles "${strategy} latency" \
        "${p50}" "${p90}" "${p99}" "${max_latency}"
    {
        printf 'RBT_WAITSET_LATENCY strategy=%s payload=64 count=%s hz=%s latency_samples=%s ' \
            "${strategy}" "${latency_count}" "${latency_hz}" "${samples}"
        printf 'p50_latency_us=%s p90_latency_us=%s p99_latency_us=%s max_latency_us=%s ' \
            "${p50}" "${p90}" "${p99}" "${max_latency}"
        printf 'publisher_log=%s subscriber_log=%s\n' "${publisher_log}" "${subscriber_log}"
    } | tee "${strategy_dir}/latency.txt"
}

run_loaded_case() {
    local strategy="$1"
    local binary="$2"
    local message="$3"
    local payload="$4"
    local label="$5"
    local fanout="$6"
    local strategy_dir="${run_dir}/${strategy}"
    local case_dir="${strategy_dir}/loaded-${label}-fanout-${fanout}"
    local service="rbt/waitset/${run_id}/loaded/${strategy}/${label}/${fanout}"
    local publisher_log="${case_dir}.publisher.log"
    local case_timeout_seconds=$((timeout_ms / 1000 + startup_delay_ms / 1000 + loaded_count + 30))
    local case_interval_us
    case_interval_us="$(loaded_interval_for_payload "${payload}" "${fanout}")"
    local publisher_pid
    local -a subscriber_pids=()
    local index
    ((case_timeout_seconds < 30)) && case_timeout_seconds=30

    local -a common_args=(
        --service "${service}"
        --profile sensor
        --message "${message}"
        --payload "${payload}"
        --count "${loaded_count}"
        --warmup "${warmup}"
        --subscriber-count "${fanout}"
        --timeout-ms "${timeout_ms}"
        --startup-delay-ms "${startup_delay_ms}"
        --interval-us "${case_interval_us}"
    )

    printf 'RBT_WAITSET_LOADED_CASE strategy=%s label=%s payload=%s fanout=%s interval_us=%s service=%s\n' \
        "${strategy}" "${label}" "${payload}" "${fanout}" "${case_interval_us}" "${service}"
    start_process "${strategy}-${label}-fanout-${fanout}-publisher" "${publisher_log}" \
        timeout --signal=TERM --kill-after=2s "${case_timeout_seconds}s" \
        "${binary}" publisher "${common_args[@]}"
    publisher_pid="${last_pid}"
    wait_ready "${publisher_pid}"

    for ((index = 1; index <= fanout; index++)); do
        local subscriber_log="${case_dir}.subscriber-${index}.log"
        start_process "${strategy}-${label}-fanout-${fanout}-subscriber-${index}" \
            "${subscriber_log}" \
            timeout --signal=TERM --kill-after=2s "${case_timeout_seconds}s" \
            "${binary}" subscriber "${common_args[@]}"
        subscriber_pids+=("${last_pid}")
        wait_ready "${last_pid}"
        validate_subscriber_strategy "${strategy}" "${subscriber_log}"
    done

    local failed=0
    for index in "${!subscriber_pids[@]}"; do
        wait_process "${subscriber_pids[${index}]}" || failed=1
    done
    wait_process "${publisher_pid}" || failed=1
    ((failed == 0)) || die "${strategy} loaded case ${label} fanout ${fanout} failed"

    local publish_rate publisher_cpu publisher_rss
    publish_rate="$(result_is_valid "${publisher_log}" publisher publish_rate_hz 0)"
    publisher_cpu="$(bench_result_field "${publisher_log}" publisher cpu_time_s)"
    publisher_rss="$(bench_result_field "${publisher_log}" publisher rss_bytes)"
    require_numeric_value "${strategy} ${label} fanout ${fanout} publisher cpu_time_s" "${publisher_cpu}"
    require_numeric_value "${strategy} ${label} fanout ${fanout} publisher rss_bytes" "${publisher_rss}"
    {
        printf 'RBT_WAITSET_LOADED strategy=%s label=%s payload=%s fanout=%s interval_us=%s ' \
            "${strategy}" "${label}" "${payload}" "${fanout}" "${case_interval_us}"
        printf 'publish_rate_hz=%s publisher_cpu_time_s=%s publisher_rss_bytes=%s publisher_log=%s\n' \
            "${publish_rate}" "${publisher_cpu}" "${publisher_rss}" "${publisher_log}"
    } | tee "${case_dir}.txt"

    for ((index = 1; index <= fanout; index++)); do
        local subscriber_log="${case_dir}.subscriber-${index}.log"
        local received
        received="$(result_is_valid "${subscriber_log}" subscriber received "${loaded_count}")"
        local subscriber_rate p50 p99 subscriber_cpu subscriber_rss
        subscriber_rate="$(result_is_valid "${subscriber_log}" subscriber receive_rate_hz 0)"
        p50="$(bench_result_field "${subscriber_log}" subscriber p50_latency_us)"
        p99="$(bench_result_field "${subscriber_log}" subscriber p99_latency_us)"
        subscriber_cpu="$(bench_result_field "${subscriber_log}" subscriber cpu_time_s)"
        subscriber_rss="$(bench_result_field "${subscriber_log}" subscriber rss_bytes)"
        require_latency_percentile_pair "${subscriber_log}" "${p50}" "${p99}"
        require_numeric_value "${subscriber_log} cpu_time_s" "${subscriber_cpu}"
        require_numeric_value "${subscriber_log} rss_bytes" "${subscriber_rss}"
        {
            printf 'RBT_WAITSET_LOADED_SUBSCRIBER strategy=%s label=%s payload=%s fanout=%s subscriber=%s ' \
                "${strategy}" "${label}" "${payload}" "${fanout}" "${index}"
            printf 'received=%s receive_rate_hz=%s p50_latency_us=%s p99_latency_us=%s ' \
                "${received}" "${subscriber_rate}" "${p50}" "${p99}"
            printf 'cpu_time_s=%s rss_bytes=%s log=%s\n' \
                "${subscriber_cpu}" "${subscriber_rss}" "${subscriber_log}"
        } | tee -a "${case_dir}.txt"
    done
}

compare_idle() {
    local polling_file="${run_dir}/polling/idle.txt"
    local waitset_file="${run_dir}/waitset/idle.txt"
    local field
    for field in user_cpu_s system_cpu_s rss_kib threads voluntary_ctx_switches involuntary_ctx_switches; do
        local polling waitset
        polling="$(summary_result_field "${polling_file}" RBT_WAITSET_IDLE "${field}")"
        waitset="$(summary_result_field "${waitset_file}" RBT_WAITSET_IDLE "${field}")"
        [[ -n "${polling}" && -n "${waitset}" ]] \
            || die "idle result comparison is missing ${field}"
        require_numeric_value "idle comparison polling ${field}" "${polling}"
        require_numeric_value "idle comparison waitset ${field}" "${waitset}"
        printf 'RBT_WAITSET_COMPARE strategy_pair=idle metric=%s polling=%s waitset=%s\n' \
            "${field}" "${polling}" "${waitset}"
    done
}

compare_latency() {
    local polling_file="${run_dir}/polling/latency.txt"
    local waitset_file="${run_dir}/waitset/latency.txt"
    local field
    for field in p50_latency_us p90_latency_us p99_latency_us max_latency_us; do
        local polling waitset
        polling="$(summary_result_field "${polling_file}" RBT_WAITSET_LATENCY "${field}")"
        waitset="$(summary_result_field "${waitset_file}" RBT_WAITSET_LATENCY "${field}")"
        [[ -n "${polling}" && -n "${waitset}" ]] \
            || die "latency result comparison is missing ${field}"
        require_numeric_value "latency comparison polling ${field}" "${polling}"
        require_numeric_value "latency comparison waitset ${field}" "${waitset}"
        printf 'RBT_WAITSET_COMPARE strategy_pair=latency metric=%s polling=%s waitset=%s\n' \
            "${field}" "${polling}" "${waitset}"
    done
}

print_matrix() {
    local payload label
    if ((smoke)); then
        printf 'RBT_WAITSET_MATRIX mode=smoke payloads=64 fanouts=1,2\n'
        return
    fi
    printf 'RBT_WAITSET_MATRIX mode=full payloads=64,1024,1048576,6220800,20971520 fanouts=1,2,4,8\n'
    for payload in 64 1024 1048576 6220800 20971520; do
        case "${payload}" in
            64) label=image-64 ;;
            1024) label=image-1024 ;;
            1048576) label=image-1048576 ;;
            6220800) label=image-6220800 ;;
            20971520) label=pointcloud-20971520 ;;
            *) die "internal payload matrix error: ${payload}" ;;
        esac
        printf 'RBT_WAITSET_MATRIX_CASE label=%s payload=%s\n' "${label}" "${payload}"
    done
}

compare_loaded_one() {
    local label="$1"
    local payload="$2"
    local fanout="$3"
    local polling_file="${run_dir}/polling/loaded-${label}-fanout-${fanout}.txt"
    local waitset_file="${run_dir}/waitset/loaded-${label}-fanout-${fanout}.txt"
    local polling waitset index field
    for field in publish_rate_hz publisher_cpu_time_s publisher_rss_bytes; do
        polling="$(summary_result_field "${polling_file}" RBT_WAITSET_LOADED "${field}")"
        waitset="$(summary_result_field "${waitset_file}" RBT_WAITSET_LOADED "${field}")"
        [[ -n "${polling}" && -n "${waitset}" ]] \
            || die "loaded comparison is missing ${field} for ${label} fanout ${fanout}"
        require_numeric_value "loaded comparison ${label} fanout ${fanout} polling ${field}" \
            "${polling}"
        require_numeric_value "loaded comparison ${label} fanout ${fanout} waitset ${field}" \
            "${waitset}"
        printf 'RBT_WAITSET_COMPARE strategy_pair=loaded label=%s payload=%s fanout=%s metric=%s polling=%s waitset=%s\n' \
            "${label}" "${payload}" "${fanout}" "${field}" "${polling}" "${waitset}"
    done
    for ((index = 1; index <= fanout; index++)); do
        for field in receive_rate_hz p50_latency_us p99_latency_us cpu_time_s rss_bytes; do
            polling="$(summary_result_field_for_subscriber "${polling_file}" "${index}" "${field}")"
            waitset="$(summary_result_field_for_subscriber "${waitset_file}" "${index}" "${field}")"
            [[ -n "${polling}" && -n "${waitset}" ]] \
                || die "loaded comparison is missing ${field} for ${label} fanout ${fanout} subscriber ${index}"
            require_numeric_value \
                "loaded comparison ${label} fanout ${fanout} subscriber ${index} polling ${field}" \
                "${polling}"
            require_numeric_value \
                "loaded comparison ${label} fanout ${fanout} subscriber ${index} waitset ${field}" \
                "${waitset}"
            printf 'RBT_WAITSET_COMPARE strategy_pair=loaded label=%s payload=%s fanout=%s subscriber=%s metric=%s polling=%s waitset=%s\n' \
                "${label}" "${payload}" "${fanout}" "${index}" "${field}" "${polling}" "${waitset}"
        done
    done
}

compare_loaded() {
    local payload label fanout
    if ((smoke)); then
        for fanout in 1 2; do
            compare_loaded_one image-64 64 "${fanout}"
        done
        return
    fi
    for payload in 64 1024 1048576 6220800 20971520; do
        if ((payload == 20971520)); then
            label=pointcloud-20971520
        else
            label="image-${payload}"
        fi
        for fanout in 1 2 4 8; do
            compare_loaded_one "${label}" "${payload}" "${fanout}"
        done
    done
}

run_strategy() {
    local strategy="$1"
    local binary="$2"
    local payload label message fanout
    run_idle_case "${strategy}" "${binary}"
    run_latency_case "${strategy}" "${binary}"

    if ((smoke)); then
        for fanout in 1 2; do
            run_loaded_case "${strategy}" "${binary}" image 64 image-64 "${fanout}"
        done
        return
    fi

    for payload in 64 1024 1048576 6220800 20971520; do
        if ((payload == 20971520)); then
            message=pointcloud
            label=pointcloud-20971520
        else
            message=image
            label="image-${payload}"
        fi
        for fanout in 1 2 4 8; do
            run_loaded_case "${strategy}" "${binary}" "${message}" "${payload}" "${label}" "${fanout}"
        done
    done
}

printf 'RBT_WAITSET_BENCH_RUN run_id=%s mode=%s logs=%s\n' \
    "${run_id}" "$([[ ${smoke} -eq 1 ]] && printf smoke || printf full)" "${run_dir}"
printf 'RBT_WAITSET_BENCH_STRATEGIES polling=%s waitset=%s\n' \
    "${baseline_binary}" "${current_binary}"
printf 'RBT_WAITSET_BENCH_BINARY strategy=polling sha256=%s path=%s\n' \
    "${baseline_hash}" "${baseline_binary}"
printf 'RBT_WAITSET_BENCH_BINARY strategy=waitset sha256=%s path=%s\n' \
    "${current_hash}" "${current_binary}"
printf 'RBT_WAITSET_BENCH_ORDER order=polling,waitset caveat=fixed-order-cache-and-scheduler-bias\n'
printf 'RBT_WAITSET_BENCH_METHOD idle_seconds=%s latency_count=%s latency_hz=%s count=%s warmup=%s timeout_ms=%s startup_delay_ms=%s minimum_interval_us=%s validation_floor_bytes_per_us=%s\n' \
    "${idle_seconds}" "${latency_count}" "${latency_hz}" "${loaded_count}" "${warmup}" \
    "${timeout_ms}" "${startup_delay_ms}" "${interval_us}" "${validation_floor_bytes_per_us}"
print_host_metadata
print_matrix

run_strategy polling "${baseline_binary}"
run_strategy waitset "${current_binary}"
compare_idle
compare_latency
compare_loaded

printf 'RBT_WAITSET_BENCH_COMPLETE run_id=%s strategies=polling,waitset logs=%s\n' \
    "${run_id}" "${run_dir}"

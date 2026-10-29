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
usage: scripts/soak_waitset_idle.sh [--smoke] [--duration-seconds N]

Keep a real connected WaitSet subscriber idle while its publisher emits no
data.  Linux /proc samples record CPU, RSS, file descriptors, threads, and
context switches.  The normal duration is 600 seconds (10 minutes); the
normal range is 600..3600 seconds.  --smoke uses 45 seconds and accepts only
30..60 seconds, making a bounded 30..60 second check.

Environment overrides:
  RBT_WAITSET_SOAK_BINARY          rbt-bench executable
                                    (default: target/release/rbt-bench)
  RBT_WAITSET_SOAK_LOG_DIR         retained output root
                                    (default: target/waitset-idle-logs)
  RBT_WAITSET_SOAK_DURATION_SECONDS
                                    normal duration (default: 600)
  RBT_WAITSET_SOAK_SMOKE_SECONDS   smoke duration (default: 45)
  RBT_WAITSET_SOAK_SAMPLE_SECONDS  sampling interval (default: 1)
  RBT_WAITSET_SOAK_TIMEOUT_SECONDS readiness/cleanup timeout (default: 20)
  RBT_WAITSET_SKIP_BUILD=1          require the binary to exist

The script does not send data.  A publisher is held in its startup delay so
the subscriber is connected to a real iceoryx2 service throughout the idle
sample.  Logs and resources.tsv remain under a unique run directory.
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

print_command() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
}

smoke=0
duration_seconds="${RBT_WAITSET_SOAK_DURATION_SECONDS:-600}"
smoke_seconds="${RBT_WAITSET_SOAK_SMOKE_SECONDS:-45}"
while (($# > 0)); do
    case "$1" in
        --smoke)
            smoke=1
            shift
            ;;
        --duration-seconds)
            (($# >= 2)) || die "--duration-seconds requires a value"
            duration_seconds="$2"
            shift 2
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
done

require_command awk
require_command cargo
require_command date
require_command find
require_command grep
require_command rustc
require_command getconf
require_command sha256sum
require_command sleep

if ((smoke)); then
    if [[ -z "${RBT_WAITSET_SOAK_DURATION_SECONDS:-}" ]]; then
        duration_seconds="${smoke_seconds}"
    fi
    [[ "${duration_seconds}" =~ ^[1-9][0-9]*$ ]] \
        || die "smoke duration must be a positive integer"
    ((duration_seconds >= 30 && duration_seconds <= 60)) \
        || die "smoke duration must be between 30 and 60 seconds"
else
    require_positive_integer RBT_WAITSET_SOAK_DURATION_SECONDS "${duration_seconds}"
    ((duration_seconds >= 600 && duration_seconds <= 3600)) \
        || die "normal duration must be between 600 and 3600 seconds"
fi

sample_seconds="${RBT_WAITSET_SOAK_SAMPLE_SECONDS:-1}"
timeout_seconds="${RBT_WAITSET_SOAK_TIMEOUT_SECONDS:-20}"
require_positive_integer RBT_WAITSET_SOAK_SAMPLE_SECONDS "${sample_seconds}"
require_positive_integer RBT_WAITSET_SOAK_TIMEOUT_SECONDS "${timeout_seconds}"
[[ -d /proc && -r /proc/self/stat ]] \
    || die "the WaitSet idle soak requires Linux /proc process statistics"

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

binary="${RBT_WAITSET_SOAK_BINARY:-${repo_root}/target/release/rbt-bench}"
if [[ "${RBT_WAITSET_SKIP_BUILD:-0}" != 1 && -z "${RBT_WAITSET_SOAK_BINARY:-}" ]]; then
    print_command cargo build --release -p rbt-bench
    cargo build --release -p rbt-bench
fi
[[ -x "${binary}" ]] || die "rbt-bench executable is missing or not executable: ${binary}"
binary_hash="$(sha256sum "${binary}" | awk '{print $1}')"

log_root="${RBT_WAITSET_SOAK_LOG_DIR:-${repo_root}/target/waitset-idle-logs}"
mkdir -p "${log_root}"
run_id="waitset-idle-$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="${log_root}/${run_id}"
mkdir -p "${run_dir}"
resource_log="${run_dir}/resources.tsv"
publisher_log="${run_dir}/publisher.log"
subscriber_log="${run_dir}/subscriber.log"
service="rbt/waitset/idle-soak/${run_id}"
publisher_pid=''
subscriber_pid=''
publisher_reaped=0
subscriber_reaped=0
last_wait_status=0

cleanup() {
    local pid
    for pid in "${subscriber_pid}" "${publisher_pid}"; do
        [[ -n "${pid}" ]] || continue
        if kill -0 "${pid}" 2>/dev/null; then
            kill -TERM "${pid}" 2>/dev/null || true
        fi
    done
    if [[ -n "${subscriber_pid}" && "${subscriber_reaped}" != 1 ]]; then
        wait "${subscriber_pid}" 2>/dev/null || true
    fi
    if [[ -n "${publisher_pid}" && "${publisher_reaped}" != 1 ]]; then
        wait "${publisher_pid}" 2>/dev/null || true
    fi
}

trap cleanup EXIT
trap 'exit 130' INT TERM

proc_snapshot() {
    local pid="$1"
    local stat_file="/proc/${pid}/stat"
    local status_file="/proc/${pid}/status"
    [[ -r "${stat_file}" && -r "${status_file}" ]] || return 1
    local cpu_ticks utime_ticks stime_ticks
    cpu_ticks="$(awk '{print $14, $15}' "${stat_file}")" || return 1
    read -r utime_ticks stime_ticks <<<"${cpu_ticks}"
    [[ "${utime_ticks}" =~ ^[0-9]+$ && "${stime_ticks}" =~ ^[0-9]+$ ]] || return 1
    local rss_kib threads voluntary involuntary fds
    rss_kib="$(awk '/^VmRSS:/ {print $2; exit}' "${status_file}")"
    threads="$(awk '/^Threads:/ {print $2; exit}' "${status_file}")"
    voluntary="$(awk '/^voluntary_ctxt_switches:/ {print $2; exit}' "${status_file}")"
    involuntary="$(awk '/^nonvoluntary_ctxt_switches:/ {print $2; exit}' "${status_file}")"
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

wait_for_ready() {
    local pid="$1"
    local log_file="$2"
    local label="$3"
    local deadline=$((SECONDS + timeout_seconds))
    while ((SECONDS < deadline)); do
        if grep -Fq 'RBT_BENCH_READY' "${log_file}" 2>/dev/null; then
            return 0
        fi
        if ! kill -0 "${pid}" 2>/dev/null; then
            wait "${pid}" 2>/dev/null || true
            die "${label} exited before RBT_BENCH_READY; see ${log_file}"
        fi
        sleep 0.05
    done
    die "timed out waiting for ${label} RBT_BENCH_READY; see ${log_file}"
}

wait_for_exit() {
    local pid="$1"
    local label="$2"
    local deadline=$((SECONDS + timeout_seconds))
    local state=''
    while kill -0 "${pid}" 2>/dev/null && ((SECONDS < deadline)); do
        state="$(awk '{print $3}' "/proc/${pid}/stat" 2>/dev/null || true)"
        [[ "${state}" == Z ]] && break
        sleep 0.05
    done
    state="$(awk '{print $3}' "/proc/${pid}/stat" 2>/dev/null || true)"
    if kill -0 "${pid}" 2>/dev/null && [[ "${state}" != Z ]]; then
        kill -KILL "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
        die "${label} did not exit after SIGTERM"
    fi
    set +e
    wait "${pid}" 2>/dev/null
    last_wait_status=$?
    set -e
    if [[ "${last_wait_status}" != 0 && "${last_wait_status}" != 143 ]]; then
        die "${label} exited with unexpected status ${last_wait_status}"
    fi
}

analyze_resources() {
    local sample_count
    sample_count="$(awk 'NR > 1 { count++ } END { print count + 0 }' "${resource_log}")"
    ((sample_count >= 2)) || die "too few /proc samples (${sample_count}); see ${resource_log}"
    local growth
    growth="$(awk '
        NR == 2 { first_rss = $6; max_rss = $6; first_fd = $7; max_fd = $7; first_threads = $8; max_threads = $8 }
        NR > 1 {
            if ($6 > max_rss) max_rss = $6
            if ($7 > max_fd) max_fd = $7
            if ($8 > max_threads) max_threads = $8
            last_rss = $6
        }
        END {
            rss_limit = first_rss + 65536
            fd_limit = first_fd + 64
            thread_limit = first_threads + 32
            print (max_rss > rss_limit || max_fd > fd_limit || max_threads > thread_limit) ? 1 : 0
        }
    ' "${resource_log}")"
    [[ "${growth}" == 0 ]] \
        || die "obvious idle resource growth detected; see ${resource_log}"
    printf 'RBT_WAITSET_IDLE_SOAK_RESOURCES samples=%s obvious_unbounded_growth=0 resource_log=%s\n' \
        "${sample_count}" "${resource_log}"
}

printf 'RBT_WAITSET_IDLE_SOAK_RUN run_id=%s mode=%s duration_seconds=%s sample_seconds=%s service=%s logs=%s\n' \
    "${run_id}" "$([[ ${smoke} -eq 1 ]] && printf smoke || printf full)" \
    "${duration_seconds}" "${sample_seconds}" "${service}" "${run_dir}"
printf 'RBT_WAITSET_IDLE_SOAK_STRATEGY strategy=waitset binary=%s\n' "${binary}"
printf 'RBT_WAITSET_IDLE_SOAK_BINARY strategy=waitset sha256=%s path=%s\n' \
    "${binary_hash}" "${binary}"
printf 'elapsed_s\tpid\trole\tuser_cpu_ticks\tsystem_cpu_ticks\trss_kib\tfds\tthreads\tvoluntary_ctx_switches\tinvoluntary_ctx_switches\n' \
    >"${resource_log}"

publisher_startup_ms=$((duration_seconds * 1000 + timeout_seconds * 1000 + 5000))
publisher_timeout_ms=$((duration_seconds * 1000 + timeout_seconds * 1000 + 10000))
subscriber_timeout_ms=$((duration_seconds * 1000 + timeout_seconds * 1000 + 10000))
print_command "${binary}" publisher --service "${service}" --profile sensor --message image \
    --payload 64 --count 1 --warmup 0 --subscriber-count 1 \
    --timeout-ms "${publisher_timeout_ms}" --startup-delay-ms "${publisher_startup_ms}" --interval-us 0
"${binary}" publisher --service "${service}" --profile sensor --message image \
    --payload 64 --count 1 --warmup 0 --subscriber-count 1 \
    --timeout-ms "${publisher_timeout_ms}" --startup-delay-ms "${publisher_startup_ms}" --interval-us 0 \
    >"${publisher_log}" 2>&1 &
publisher_pid="$!"
wait_for_ready "${publisher_pid}" "${publisher_log}" publisher

print_command "${binary}" subscriber --service "${service}" --profile sensor --message image \
    --payload 64 --count 1 --warmup 0 --subscriber-count 1 \
    --timeout-ms "${subscriber_timeout_ms}" --startup-delay-ms 0 --interval-us 0
"${binary}" subscriber --service "${service}" --profile sensor --message image \
    --payload 64 --count 1 --warmup 0 --subscriber-count 1 \
    --timeout-ms "${subscriber_timeout_ms}" --startup-delay-ms 0 --interval-us 0 \
    >"${subscriber_log}" 2>&1 &
subscriber_pid="$!"
wait_for_ready "${subscriber_pid}" "${subscriber_log}" subscriber
grep -Eq '^RBT_BENCH_READY role=subscriber .*strategy=waitset([[:space:]]|$)' "${subscriber_log}" \
    || die "subscriber omitted strategy=waitset provenance; see ${subscriber_log}"
kill -0 "${publisher_pid}" 2>/dev/null \
    || die "publisher exited before idle connectivity sampling; see ${publisher_log}"
sleep 1

ticks_per_second="$(getconf CLK_TCK)"
[[ "${ticks_per_second}" =~ ^[1-9][0-9]*$ ]] || die "getconf CLK_TCK returned an invalid value"
started_at="${SECONDS}"
while ((SECONDS - started_at < duration_seconds)); do
    kill -0 "${publisher_pid}" 2>/dev/null \
        || die "publisher exited during idle soak; see ${publisher_log}"
    snapshot="$(proc_snapshot "${subscriber_pid}")" \
        || die "connected WaitSet subscriber exited during idle soak"
    read -r user_ticks system_ticks rss_kib fds threads voluntary involuntary pid <<<"${snapshot}"
    printf '%s\t%s\tsubscriber\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$((SECONDS - started_at))" "${pid}" "${user_ticks}" "${system_ticks}" \
        "${rss_kib}" "${fds}" "${threads}" "${voluntary}" "${involuntary}" \
        >>"${resource_log}"
    sleep "${sample_seconds}"
done

snapshot="$(proc_snapshot "${subscriber_pid}")" \
    || die "connected WaitSet subscriber exited before idle soak completion"
kill -0 "${publisher_pid}" 2>/dev/null \
    || die "publisher exited before idle soak completion; see ${publisher_log}"
read -r user_ticks system_ticks rss_kib fds threads voluntary involuntary pid <<<"${snapshot}"
printf '%s\t%s\tsubscriber\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$((SECONDS - started_at))" "${pid}" "${user_ticks}" "${system_ticks}" \
    "${rss_kib}" "${fds}" "${threads}" "${voluntary}" "${involuntary}" \
    >>"${resource_log}"

if grep -Eq '^RBT_BENCH_RESULT ' "${subscriber_log}"; then
    die "idle soak subscriber reported a data result; the no-data contract was violated"
fi

printf 'RBT_WAITSET_IDLE_SOAK_SIGNAL signal=TERM role=subscriber pid=%s\n' "${subscriber_pid}"
kill -TERM "${subscriber_pid}" 2>/dev/null || true
wait_for_exit "${subscriber_pid}" subscriber
subscriber_reaped=1
subscriber_exit_status="${last_wait_status}"
if kill -0 "${subscriber_pid}" 2>/dev/null; then
    die "subscriber still exists after SIGTERM"
fi

kill -TERM "${publisher_pid}" 2>/dev/null || true
wait_for_exit "${publisher_pid}" publisher
publisher_reaped=1
publisher_exit_status="${last_wait_status}"

analyze_resources
elapsed_seconds=$((SECONDS - started_at))
read -r first_user first_system first_rss first_fds first_threads \
    <<<"$(awk 'NR == 2 { print $4, $5, $6, $7, $8 }' "${resource_log}")"
read -r last_user last_system last_rss last_fds last_threads \
    <<<"$(awk 'END { print $4, $5, $6, $7, $8 }' "${resource_log}")"
user_cpu_seconds="$(awk -v ticks="$((last_user - first_user))" -v hz="${ticks_per_second}" \
    'BEGIN { printf "%.6f", ticks / hz }')"
system_cpu_seconds="$(awk -v ticks="$((last_system - first_system))" -v hz="${ticks_per_second}" \
    'BEGIN { printf "%.6f", ticks / hz }')"
printf 'RBT_WAITSET_IDLE_SOAK_RESULT duration_seconds=%s elapsed_seconds=%s ' \
    "${duration_seconds}" "${elapsed_seconds}"
printf 'user_cpu_s=%s system_cpu_s=%s rss_start_kib=%s rss_end_kib=%s ' \
    "${user_cpu_seconds}" "${system_cpu_seconds}" "${first_rss}" "${last_rss}"
printf 'fds_start=%s fds_end=%s threads_start=%s threads_end=%s ' \
    "${first_fds}" "${last_fds}" "${first_threads}" "${last_threads}"
printf 'signal_exit=pass subscriber_exit_status=%s publisher_exit_status=%s data_observed=0 resource_log=%s\n' \
    "${subscriber_exit_status}" "${publisher_exit_status}" "${resource_log}"
printf 'PASS: WaitSet idle soak completed; logs=%s\n' "${run_dir}"

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
usage: scripts/benchmark_pressure.sh [options]

Run the bounded Sensor multi-subscriber pressure matrix. The normal matrix
runs 64 B, 1 KiB, 1 MiB, and 6,220,800 B Image payloads plus a 20,971,520 B
PointCloud payload with 1, 2, 4, and 8 subscribers. It then runs a 64 B case with two fast, one delayed, and
one stalled subscriber.

options:
  --smoke                 run the 64 B matrix plus the mixed-speed case
  --count N               measured samples per case (default: 20)
  --warmup N              warmup samples per case (default: 3)
  --timeout-ms N          subscriber/retry timeout (default: 5000)
  --startup-delay-ms N    publisher delay after READY (default: 500)
  --interval-us N         minimum delay between sends (default: 1000;
                          fanout and large payloads are paced for full
                          validation)
  --slow-delay-us N       delay after each slow receive (default: 200000)
  --debug                 use target/debug/rbt-bench instead of release
  --help                  show this help

The same values may be supplied with RBT_PRESSURE_COUNT, RBT_PRESSURE_WARMUP,
RBT_PRESSURE_TIMEOUT_MS, RBT_PRESSURE_STARTUP_DELAY_MS,
RBT_PRESSURE_INTERVAL_US, and RBT_PRESSURE_SLOW_DELAY_US. RBT_BENCH_BINARY
may point at an already-built rbt-bench executable.
EOF
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
build_mode="release"
count="${RBT_PRESSURE_COUNT:-20}"
warmup="${RBT_PRESSURE_WARMUP:-3}"
timeout_ms="${RBT_PRESSURE_TIMEOUT_MS:-5000}"
startup_delay_ms="${RBT_PRESSURE_STARTUP_DELAY_MS:-500}"
interval_us="${RBT_PRESSURE_INTERVAL_US:-1000}"
# Full-linear validation runs concurrently in every subscriber.  Keep the
# acceptance matrix conservative enough for eight 20 MiB validators under
# scheduler contention; the effective interval is emitted with every result.
validation_floor_bytes_per_us=25
slow_delay_us="${RBT_PRESSURE_SLOW_DELAY_US:-200000}"

while (($# > 0)); do
    case "$1" in
        --smoke)
            smoke=1
            shift
            ;;
        --debug)
            build_mode="debug"
            shift
            ;;
        --count)
            (($# >= 2)) || die "--count requires a value"
            count="$2"
            shift 2
            ;;
        --warmup)
            (($# >= 2)) || die "--warmup requires a value"
            warmup="$2"
            shift 2
            ;;
        --timeout-ms)
            (($# >= 2)) || die "--timeout-ms requires a value"
            timeout_ms="$2"
            shift 2
            ;;
        --startup-delay-ms)
            (($# >= 2)) || die "--startup-delay-ms requires a value"
            startup_delay_ms="$2"
            shift 2
            ;;
        --interval-us)
            (($# >= 2)) || die "--interval-us requires a value"
            interval_us="$2"
            shift 2
            ;;
        --slow-delay-us)
            (($# >= 2)) || die "--slow-delay-us requires a value"
            slow_delay_us="$2"
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

require_positive_integer count "${count}"
((count >= 2)) || die "count must be at least 2 for the mixed-speed drop case"
require_nonnegative_integer warmup "${warmup}"
require_positive_integer timeout_ms "${timeout_ms}"
require_nonnegative_integer startup_delay_ms "${startup_delay_ms}"
require_nonnegative_integer interval_us "${interval_us}"
require_nonnegative_integer slow_delay_us "${slow_delay_us}"

case_interval_for_payload() {
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

command -v cargo >/dev/null 2>&1 || die "cargo is required"
command -v rustc >/dev/null 2>&1 || die "rustc is required"
command -v timeout >/dev/null 2>&1 || die "timeout is required"
command -v grep >/dev/null 2>&1 || die "grep is required"
command -v awk >/dev/null 2>&1 || die "awk is required"

if [[ -n "${RBT_BENCH_BINARY:-}" ]]; then
    binary="${RBT_BENCH_BINARY}"
else
    if [[ "${build_mode}" == "release" ]]; then
        cargo build --release -p rbt-bench
        binary="${repo_root}/target/release/rbt-bench"
    else
        cargo build -p rbt-bench
        binary="${repo_root}/target/debug/rbt-bench"
    fi
fi
[[ -x "${binary}" ]] || die "rbt-bench executable is missing or not executable: ${binary}"

tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/rbt-bench-pressure.XXXXXX")"
run_id="$(basename "${tmp_root}")-$$"
declare -a child_pids=()

cleanup() {
    local pid
    for pid in "${child_pids[@]:-}"; do
        if kill -0 "${pid}" 2>/dev/null; then
            kill -TERM "${pid}" 2>/dev/null || true
        fi
    done
    for pid in "${child_pids[@]:-}"; do
        wait "${pid}" 2>/dev/null || true
    done
    rm -rf -- "${tmp_root}"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

printf 'RBT_PRESSURE_RUN run_id=%s build_mode=%s binary=%s smoke=%s\n' \
    "${run_id}" "${build_mode}" "${binary}" "${smoke}"
printf 'RBT_PRESSURE_METHOD count=%s warmup=%s timeout_ms=%s startup_delay_ms=%s minimum_interval_us=%s validation_floor_bytes_per_us=%s slow_delay_us=%s\n' \
    "${count}" "${warmup}" "${timeout_ms}" "${startup_delay_ms}" "${interval_us}" \
    "${validation_floor_bytes_per_us}" "${slow_delay_us}"
if ((smoke)); then
    matrix_payloads='64'
else
    matrix_payloads='64,1024,1048576,6220800,20971520'
fi
printf 'RBT_PRESSURE_MATRIX payloads=%s subscriber_counts=1,2,4,8\n' "${matrix_payloads}"
printf 'RBT_PRESSURE_TOOLCHAIN root=%s\n' "${toolchain_root}"
cpu_model="$(awk -F: '/^model name/ { sub(/^[[:space:]]*/, "", $2); print $2; exit }' /proc/cpuinfo 2>/dev/null)"
logical_cpus="$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf 'unknown')"
os_name="$(sed -n 's/^PRETTY_NAME=//p' /etc/os-release 2>/dev/null | tr -d '\"')"
kernel="$(uname -srm 2>/dev/null || printf 'unknown')"
rust_compiler="$(rustc --version)"
printf 'RBT_PRESSURE_HOST cpu_model=%s logical_cpus=%s os=%s kernel=%s\n' \
    "${cpu_model:-unknown}" "${logical_cpus}" "${os_name:-unknown}" "${kernel}"
printf 'RBT_PRESSURE_COMPILERS rust=%s\n' "${rust_compiler}"

case_timeout_seconds=$(( (timeout_ms + startup_delay_ms + 999) / 1000 + 10 ))
((case_timeout_seconds < 10)) && case_timeout_seconds=10

wait_for_ready() {
    local log_file="$1"
    local pid="$2"
    local deadline=$((SECONDS + (timeout_ms + startup_delay_ms + 5000) / 1000 + 1))
    while ((SECONDS < deadline)); do
        if grep -q 'RBT_BENCH_READY' "${log_file}" 2>/dev/null; then
            return 0
        fi
        if ! kill -0 "${pid}" 2>/dev/null; then
            cat "${log_file}" >&2 || true
            die "benchmark process exited before READY"
        fi
        sleep 0.02
    done
    cat "${log_file}" >&2 || true
    die "timed out waiting for benchmark process READY"
}

wait_for_pids() {
    local failed=0
    local pid status
    for pid in "$@"; do
        set +e
        wait "${pid}"
        status=$?
        set -e
        if ((status != 0)); then
            failed=1
        fi
    done
    child_pids=()
    return "${failed}"
}

result_field() {
    local log_file="$1"
    local key="$2"
    awk -v key="${key}" '
        /^RBT_BENCH_RESULT / {
            for (field = 1; field <= NF; ++field) {
                if (substr($field, 1, length(key) + 1) == key "=") {
                    print substr($field, length(key) + 2)
                    exit
                }
            }
        }
    ' "${log_file}"
}

record_result() {
    local label="$1"
    local profile="$2"
    local message="$3"
    local payload="$4"
    local subscriber_count="$5"
    local role="$6"
    local index="$7"
    local log_file="$8"
    local effective_interval_us="$9"
    local rate_key="receive_rate_hz"
    [[ "${role}" == "publisher" ]] && rate_key="publish_rate_hz"
    local rate_value cpu_value rss_value received_value dropped_value
    local observed_subscribers min_recipients max_recipients
    local payload_validation payload_validation_samples payload_fingerprint
    rate_value="$(result_field "${log_file}" "${rate_key}")"
    cpu_value="$(result_field "${log_file}" cpu_time_s)"
    rss_value="$(result_field "${log_file}" rss_bytes)"
    received_value="$(result_field "${log_file}" received)"
    dropped_value="$(result_field "${log_file}" dropped)"
    observed_subscribers="$(result_field "${log_file}" observed_subscriber_count)"
    min_recipients="$(result_field "${log_file}" min_recipients_per_send)"
    max_recipients="$(result_field "${log_file}" max_recipients_per_send)"
    payload_validation="$(result_field "${log_file}" payload_validation)"
    payload_validation_samples="$(result_field "${log_file}" payload_validation_samples)"
    payload_fingerprint="$(result_field "${log_file}" payload_fingerprint)"
    [[ -n "${rate_value}" ]] || die "${role} result is missing ${rate_key}: ${log_file}"
    [[ -n "${cpu_value}" ]] || die "${role} result is missing cpu_time_s: ${log_file}"
    [[ -n "${rss_value}" ]] || die "${role} result is missing rss_bytes: ${log_file}"
    local named_rate_key="${role//-/_}_rate_hz"
    local named_cpu_key="${role//-/_}_cpu_time_s"
    local named_rss_key="${role//-/_}_rss_bytes"
    printf 'RBT_PRESSURE_RESULT label=%s profile=%s message=%s payload=%s subscriber_count=%s effective_interval_us=%s role=%s index=%s rate_hz=%s cpu_time_s=%s rss_bytes=%s received=%s dropped=%s observed_subscribers=%s min_recipients=%s max_recipients=%s payload_validation=%s payload_validation_samples=%s payload_fingerprint=%s %s=%s %s=%s %s=%s\n' \
        "${label}" "${profile}" "${message}" "${payload}" "${subscriber_count}" \
        "${effective_interval_us}" "${role}" "${index}" "${rate_value}" "${cpu_value}" "${rss_value}" \
        "${received_value:-na}" "${dropped_value:-na}" \
        "${observed_subscribers:-na}" "${min_recipients:-na}" "${max_recipients:-na}" \
        "${payload_validation:-na}" "${payload_validation_samples:-na}" "${payload_fingerprint:-na}" \
        "${named_rate_key}" "${rate_value}" "${named_cpu_key}" "${cpu_value}" \
        "${named_rss_key}" "${rss_value}"
}

assert_result_field() {
    local log_file="$1"
    local key="$2"
    local expected="$3"
    local actual
    actual="$(result_field "${log_file}" "${key}")"
    [[ "${actual}" == "${expected}" ]] \
        || die "expected ${key}=${expected}, got ${actual:-missing}; see ${log_file}"
}

baseline_fast4_publisher_rate=''
baseline_fast4_subscriber_rate=''

run_matrix_case() {
    local message="$1"
    local payload="$2"
    local label_prefix="$3"
    local subscriber_count
    for subscriber_count in 1 2 4 8; do
        local case_interval_us
        case_interval_us="$(case_interval_for_payload "${payload}" "${subscriber_count}")"
        local case_transport_timeout_ms=$((
            timeout_ms + startup_delay_ms
            + (count + warmup) * case_interval_us / 1000
            + 5000
        ))
        local matrix_case_timeout_seconds=$(((case_transport_timeout_ms + 999) / 1000 + 10))
        local label="${label_prefix}-subs-${subscriber_count}"
        local service="rbt/bench/pressure/${run_id}/${label}"
        local publisher_log="${tmp_root}/${label}.publisher.log"
        local -a subscriber_logs=()
        local -a pids=()
        local -a common_args=(
            --service "${service}"
            --profile sensor
            --message "${message}"
            --payload "${payload}"
            --count "${count}"
            --warmup "${warmup}"
            --subscriber-count "${subscriber_count}"
            --timeout-ms "${case_transport_timeout_ms}"
            --startup-delay-ms "${startup_delay_ms}"
            --interval-us "${case_interval_us}"
        )

        printf 'RBT_PRESSURE_CASE label=%s profile=sensor message=%s payload=%s subscriber_count=%s effective_interval_us=%s service=%s topology=fast-x%s\n' \
            "${label}" "${message}" "${payload}" "${subscriber_count}" "${case_interval_us}" "${service}" "${subscriber_count}"
        printf '+ %q publisher' "${binary}"
        printf ' %q' "${common_args[@]}"
        printf '\n'
        timeout --signal=TERM --kill-after=2s "${matrix_case_timeout_seconds}s" \
            "${binary}" publisher "${common_args[@]}" >"${publisher_log}" 2>&1 &
        local publisher_pid=$!
        child_pids+=("${publisher_pid}")
        pids+=("${publisher_pid}")
        wait_for_ready "${publisher_log}" "${publisher_pid}"

        local index
        for ((index = 1; index <= subscriber_count; ++index)); do
            local subscriber_log="${tmp_root}/${label}.subscriber-${index}.log"
            subscriber_logs+=("${subscriber_log}")
            printf '+ %q subscriber index=%s' "${binary}" "${index}"
            printf ' %q' "${common_args[@]}" --subscriber-mode fast
            printf '\n'
            timeout --signal=TERM --kill-after=2s "${matrix_case_timeout_seconds}s" \
                "${binary}" subscriber "${common_args[@]}" --subscriber-mode fast \
                >"${subscriber_log}" 2>&1 &
            local subscriber_pid=$!
            child_pids+=("${subscriber_pid}")
            pids+=("${subscriber_pid}")
        done
        for ((index = 1; index <= subscriber_count; ++index)); do
            wait_for_ready "${subscriber_logs[index - 1]}" "${pids[index]}"
        done
        sleep 0.05

        if ! wait_for_pids "${pids[@]}"; then
            cat "${publisher_log}" >&2 || true
            for subscriber_log in "${subscriber_logs[@]}"; do
                cat "${subscriber_log}" >&2 || true
            done
            die "pressure matrix case failed: ${label}"
        fi
        assert_result_field "${publisher_log}" observed_subscriber_count "${subscriber_count}"
        assert_result_field "${publisher_log}" min_recipients_per_send "${subscriber_count}"
        assert_result_field "${publisher_log}" max_recipients_per_send "${subscriber_count}"
        for subscriber_log in "${subscriber_logs[@]}"; do
            assert_result_field "${subscriber_log}" received "${count}"
            assert_result_field "${subscriber_log}" dropped 0
            assert_result_field "${subscriber_log}" payload_validation full_linear_fnv1a
            assert_result_field "${subscriber_log}" payload_validation_samples "$((count + warmup))"
            [[ "$(result_field "${subscriber_log}" payload_fingerprint)" != na ]] \
                || die "subscriber payload fingerprint is unavailable: ${subscriber_log}"
        done
        cat "${publisher_log}"
        for subscriber_log in "${subscriber_logs[@]}"; do
            cat "${subscriber_log}"
        done
        record_result "${label}" sensor "${message}" "${payload}" "${subscriber_count}" \
            publisher 0 "${publisher_log}" "${case_interval_us}"
        index=1
        for subscriber_log in "${subscriber_logs[@]}"; do
            record_result "${label}" sensor "${message}" "${payload}" "${subscriber_count}" \
                fast-subscriber "${index}" "${subscriber_log}" "${case_interval_us}"
            index=$((index + 1))
        done
        if ((subscriber_count == 4)) && [[ "${message}" == image && "${payload}" == 64 ]]; then
            baseline_fast4_publisher_rate="$(result_field "${publisher_log}" publish_rate_hz)"
            baseline_fast4_subscriber_rate="$(for subscriber_log in "${subscriber_logs[@]}"; do
                result_field "${subscriber_log}" receive_rate_hz
            done | awk 'NR == 1 || $1 < minimum { minimum = $1 } END { print minimum }')"
        fi
    done
}

run_mixed_speed_case() {
    local message="image"
    local payload=64
    local label="sensor-image-64-mixed-speed"
    local subscriber_count=4
    local service="rbt/bench/pressure/${run_id}/${label}"
    local publisher_log="${tmp_root}/${label}.publisher.log"
    local fast_a_log="${tmp_root}/${label}.fast-1.log"
    local fast_b_log="${tmp_root}/${label}.fast-2.log"
    local slow_log="${tmp_root}/${label}.slow.log"
    local stalled_log="${tmp_root}/${label}.stalled.log"
    local -a common_args=(
        --service "${service}"
        --profile sensor
        --message "${message}"
        --payload "${payload}"
        --count "${count}"
        --warmup "${warmup}"
        --subscriber-count "${subscriber_count}"
        --timeout-ms "${timeout_ms}"
        --startup-delay-ms "${startup_delay_ms}"
        --interval-us "${interval_us}"
    )
    local -a pids=()
    local -a logs=("${publisher_log}" "${fast_a_log}" "${fast_b_log}" "${slow_log}" "${stalled_log}")

    printf 'RBT_PRESSURE_CASE label=%s profile=sensor message=%s payload=%s subscriber_count=%s service=%s topology=fast-x2,slow-x1,stalled-x1 slow_delay_us=%s\n' \
        "${label}" "${message}" "${payload}" "${subscriber_count}" "${service}" "${slow_delay_us}"
    printf '+ %q publisher' "${binary}"
    printf ' %q' "${common_args[@]}"
    printf '\n'
    timeout --signal=TERM --kill-after=2s "${case_timeout_seconds}s" \
        "${binary}" publisher "${common_args[@]}" >"${publisher_log}" 2>&1 &
    local publisher_pid=$!
    child_pids+=("${publisher_pid}")
    pids+=("${publisher_pid}")
    wait_for_ready "${publisher_log}" "${publisher_pid}"

    printf '+ %q subscriber role=fast index=1' "${binary}"
    printf ' %q' "${common_args[@]}" --subscriber-mode fast
    printf '\n'
    timeout --signal=TERM --kill-after=2s "${case_timeout_seconds}s" \
        "${binary}" subscriber "${common_args[@]}" --subscriber-mode fast \
        >"${fast_a_log}" 2>&1 &
    local fast_a_pid=$!
    child_pids+=("${fast_a_pid}")
    pids+=("${fast_a_pid}")

    printf '+ %q subscriber role=fast index=2' "${binary}"
    printf ' %q' "${common_args[@]}" --subscriber-mode fast
    printf '\n'
    timeout --signal=TERM --kill-after=2s "${case_timeout_seconds}s" \
        "${binary}" subscriber "${common_args[@]}" --subscriber-mode fast \
        >"${fast_b_log}" 2>&1 &
    local fast_b_pid=$!
    child_pids+=("${fast_b_pid}")
    pids+=("${fast_b_pid}")

    printf '+ %q subscriber role=slow' "${binary}"
    printf ' %q' "${common_args[@]}" --subscriber-mode slow --receive-delay-us "${slow_delay_us}"
    printf '\n'
    timeout --signal=TERM --kill-after=2s "${case_timeout_seconds}s" \
        "${binary}" subscriber "${common_args[@]}" --subscriber-mode slow \
        --receive-delay-us "${slow_delay_us}" >"${slow_log}" 2>&1 &
    local slow_pid=$!
    child_pids+=("${slow_pid}")
    pids+=("${slow_pid}")

    printf '+ %q subscriber role=stalled' "${binary}"
    printf ' %q' "${common_args[@]}" --subscriber-mode stalled
    printf '\n'
    timeout --signal=TERM --kill-after=2s "${case_timeout_seconds}s" \
        "${binary}" subscriber "${common_args[@]}" --subscriber-mode stalled \
        >"${stalled_log}" 2>&1 &
    local stalled_pid=$!
    child_pids+=("${stalled_pid}")
    pids+=("${stalled_pid}")

    wait_for_ready "${fast_a_log}" "${fast_a_pid}"
    wait_for_ready "${fast_b_log}" "${fast_b_pid}"
    wait_for_ready "${slow_log}" "${slow_pid}"
    wait_for_ready "${stalled_log}" "${stalled_pid}"
    sleep 0.05

    if ! wait_for_pids "${pids[@]}"; then
        for log in "${logs[@]}"; do
            cat "${log}" >&2 || true
        done
        die "mixed-speed pressure case failed: ${label}"
    fi
    assert_result_field "${publisher_log}" observed_subscriber_count "${subscriber_count}"
    assert_result_field "${publisher_log}" min_recipients_per_send "${subscriber_count}"
    assert_result_field "${publisher_log}" max_recipients_per_send "${subscriber_count}"
    assert_result_field "${fast_a_log}" received "${count}"
    assert_result_field "${fast_a_log}" dropped 0
    assert_result_field "${fast_b_log}" received "${count}"
    assert_result_field "${fast_b_log}" dropped 0
    assert_result_field "${stalled_log}" received 0
    assert_result_field "${stalled_log}" dropped "${count}"
    slow_received="$(result_field "${slow_log}" received)"
    slow_dropped="$(result_field "${slow_log}" dropped)"
    ((slow_received > 0 && slow_received < count)) \
        || die "slow subscriber did not demonstrate bounded progress with loss; see ${slow_log}"
    ((slow_dropped > 0)) || die "slow subscriber did not report drops; see ${slow_log}"
    for log in "${logs[@]}"; do
        cat "${log}"
    done
    record_result "${label}" sensor "${message}" "${payload}" "${subscriber_count}" \
        publisher 0 "${publisher_log}" "${interval_us}"
    record_result "${label}" sensor "${message}" "${payload}" "${subscriber_count}" \
        fast-subscriber 1 "${fast_a_log}" "${interval_us}"
    record_result "${label}" sensor "${message}" "${payload}" "${subscriber_count}" \
        fast-subscriber 2 "${fast_b_log}" "${interval_us}"
    record_result "${label}" sensor "${message}" "${payload}" "${subscriber_count}" \
        slow-subscriber 1 "${slow_log}" "${interval_us}"
    record_result "${label}" sensor "${message}" "${payload}" "${subscriber_count}" \
        stalled-subscriber 1 "${stalled_log}" "${interval_us}"

    mixed_publisher_rate="$(result_field "${publisher_log}" publish_rate_hz)"
    mixed_fast_min_rate="$(printf '%s\n%s\n' \
        "$(result_field "${fast_a_log}" receive_rate_hz)" \
        "$(result_field "${fast_b_log}" receive_rate_hz)" \
        | awk 'NR == 1 || $1 < minimum { minimum = $1 } END { print minimum }')"
    [[ -n "${baseline_fast4_publisher_rate}" && -n "${baseline_fast4_subscriber_rate}" ]] \
        || die "4-fast baseline is unavailable for mixed-speed comparison"
    publisher_ratio="$(awk -v mixed="${mixed_publisher_rate}" -v baseline="${baseline_fast4_publisher_rate}" \
        'BEGIN { printf "%.3f", (baseline > 0 ? mixed / baseline : 0) }')"
    fast_ratio="$(awk -v mixed="${mixed_fast_min_rate}" -v baseline="${baseline_fast4_subscriber_rate}" \
        'BEGIN { printf "%.3f", (baseline > 0 ? mixed / baseline : 0) }')"
    printf 'RBT_PRESSURE_COMPARISON baseline=fast-x4 mixed=fast-x2,slow-x1,stalled-x1 publisher_rate_ratio=%s fast_min_rate_ratio=%s slow_received=%s slow_dropped=%s stalled_received=0 stalled_dropped=%s\n' \
        "${publisher_ratio}" "${fast_ratio}" "${slow_received}" "${slow_dropped}" "${count}"
}

run_matrix_case image 64 sensor-image-64
if (( ! smoke )); then
    run_matrix_case image 1024 sensor-image-1024
    run_matrix_case image 1048576 sensor-image-1048576
    run_matrix_case image 6220800 sensor-image-6220800
    run_matrix_case pointcloud 20971520 sensor-pointcloud-20971520
fi
run_mixed_speed_case

printf 'RBT_PRESSURE_COMPLETE run_id=%s\n' "${run_id}"

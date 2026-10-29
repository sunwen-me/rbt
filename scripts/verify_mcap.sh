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
usage: scripts/verify_mcap.sh <--quick|--full>

Environment overrides:
  RBT_MCAP_SKIP_INHERITED=1  skip inherited full gates when a parent verifier
                              schedules those gates separately
EOF
}

mode=''
case "${1:-}" in
    --quick) mode=quick ;;
    --full) mode=full ;;
    --help|-h) usage; exit 0 ;;
    *) usage >&2; exit 2 ;;
esac
[[ $# == 1 ]] || die "unexpected extra arguments"

skip_inherited="${RBT_MCAP_SKIP_INHERITED:-0}"
[[ "${skip_inherited}" =~ ^[01]$ ]] \
    || die "RBT_MCAP_SKIP_INHERITED must be 0 or 1"

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
cpp_prefix="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"
"${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${cpp_prefix}"
cpp_build="${RBT_MCAP_CPP_BUILD:-${repo_root}/target/mcap-cpp-build}"
log_root="${RBT_MCAP_LOG_DIR:-${repo_root}/target/mcap-verify-logs}"
run_id="mcap-${mode}-$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="${log_root}/${run_id}"
tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/rbt-mcap-verify.XXXXXX")"
mkdir -p "${run_dir}"

for command in cargo cmake ctest jq timeout diff cmp sha256sum awk truncate; do
    command -v "${command}" >/dev/null 2>&1 || die "required command not found: ${command}"
done
[[ -x "${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc" ]] \
    || die "configured C compiler is missing under ${toolchain_root}"
[[ -f "${cpp_prefix}/lib/cmake/iceoryx2-cxx/iceoryx2-cxxConfig.cmake" ]] \
    || die "iceoryx2 C++ package is missing under ${cpp_prefix}"

export PATH="${toolchain_root}/bin${PATH:+:${PATH}}"
export CC="${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc"
export CXX="${toolchain_root}/bin/x86_64-conda-linux-gnu-g++"
export AR="${toolchain_root}/bin/x86_64-conda-linux-gnu-ar"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
export LD_LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"
export CMAKE_PREFIX_PATH="${cpp_prefix}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"

declare -a child_pids=()
declare -A child_labels=()
last_pid=''
composer_pid=''
composer_addr=''

forget_pid() {
    local target="$1"
    local -a kept=()
    local pid
    for pid in "${child_pids[@]}"; do
        [[ "${pid}" == "${target}" ]] || kept+=("${pid}")
    done
    child_pids=("${kept[@]}")
    unset 'child_labels[$target]'
}

cleanup() {
    local pid
    for pid in "${child_pids[@]}"; do
        kill -TERM "${pid}" 2>/dev/null || true
    done
    for pid in "${child_pids[@]}"; do
        wait "${pid}" 2>/dev/null || true
    done
    rm -rf -- "${tmp_root}"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

print_command() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
}

run_logged() {
    local label="$1"
    shift
    local log="${run_dir}/${label}.log"
    print_command "$@"
    set +e
    "$@" >"${log}" 2>&1
    local status=$?
    set -e
    cat "${log}"
    ((status == 0)) || die "${label} failed with exit ${status}; see ${log}"
}

expect_failure() {
    local label="$1"
    shift
    local log="${run_dir}/${label}.log"
    print_command "$@"
    set +e
    "$@" >"${log}" 2>&1
    local status=$?
    set -e
    cat "${log}"
    ((status != 0)) || die "${label} unexpectedly succeeded; see ${log}"
}

start_process() {
    local label="$1"
    shift
    local log="${run_dir}/${label}.log"
    print_command "$@"
    "$@" >"${log}" 2>&1 &
    last_pid=$!
    child_pids+=("${last_pid}")
    child_labels["${last_pid}"]="${label}"
}

start_split_process() {
    local label="$1"
    shift
    print_command "$@"
    "$@" >"${run_dir}/${label}.json" 2>"${run_dir}/${label}.err" &
    last_pid=$!
    child_pids+=("${last_pid}")
    child_labels["${last_pid}"]="${label}"
}

wait_pid() {
    local pid="$1"
    local label="${child_labels[${pid}]:-process-${pid}}"
    set +e
    wait "${pid}"
    local status=$?
    set -e
    forget_pid "${pid}"
    if [[ -f "${run_dir}/${label}.log" ]]; then
        cat "${run_dir}/${label}.log"
    else
        cat "${run_dir}/${label}.err" || true
        cat "${run_dir}/${label}.json" || true
    fi
    ((status == 0)) || die "${label} failed with exit ${status}"
}

stop_pid() {
    local pid="$1"
    local signal="${2:-TERM}"
    local label="${child_labels[${pid}]:-process-${pid}}"
    if kill -0 "${pid}" 2>/dev/null; then
        kill -"${signal}" "${pid}" 2>/dev/null || true
    fi
    set +e
    wait "${pid}"
    local status=$?
    set -e
    forget_pid "${pid}"
    printf 'stopped %s pid=%s signal=%s status=%s\n' "${label}" "${pid}" "${signal}" "${status}"
}

wait_log() {
    local log="$1"
    local pattern="$2"
    local attempts="${3:-300}"
    local attempt
    for ((attempt = 0; attempt < attempts; ++attempt)); do
        grep -Fq -- "${pattern}" "${log}" 2>/dev/null && return 0
        sleep 0.05
    done
    [[ -f "${log}" ]] && cat "${log}" >&2
    die "timed out waiting for ${pattern@Q} in ${log}"
}

inspect_json() {
    RBT_COMPOSER_ADDR="${composer_addr}" target/debug/rbt ps --json
}

capability_list_json() {
    RBT_COMPOSER_ADDR="${composer_addr}" target/debug/rbt capability list --json
}

wait_capabilities() {
    local provider="$1"
    local expected="$2"
    local output="$3"
    local attempt
    for ((attempt = 0; attempt < 300; ++attempt)); do
        if capability_list_json >"${output}" 2>/dev/null \
            && jq -e --arg provider "${provider}" --argjson expected "${expected}" \
                '[.[] | select(.provider_component == $provider)] | length == $expected' \
                "${output}" >/dev/null; then
            return 0
        fi
        sleep 0.05
    done
    [[ -f "${output}" ]] && cat "${output}" >&2
    die "provider ${provider} did not reach ${expected} active capabilities"
}

start_composer() {
    local label="$1"
    start_process "${label}" env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info target/debug/composer
    composer_pid="${last_pid}"
    local attempt
    for ((attempt = 0; attempt < 300; ++attempt)); do
        if inspect_json >"${run_dir}/${label}-ready.json" 2>/dev/null; then
            return 0
        fi
        kill -0 "${composer_pid}" 2>/dev/null || die "${label} exited before accepting inspection"
        sleep 0.05
    done
    die "${label} did not accept inspection at ${composer_addr}"
}

record_foreground() {
    local label="$1"
    shift
    print_command env RBT_COMPOSER_ADDR="${composer_addr}" target/debug/rbt record "$@"
    env RBT_COMPOSER_ADDR="${composer_addr}" target/debug/rbt record "$@" \
        >"${run_dir}/${label}.json" 2>"${run_dir}/${label}.err"
    cat "${run_dir}/${label}.err"
    cat "${run_dir}/${label}.json"
}

play_foreground() {
    local label="$1"
    shift
    print_command env RBT_COMPOSER_ADDR="${composer_addr}" target/debug/rbt play "$@"
    env RBT_COMPOSER_ADDR="${composer_addr}" target/debug/rbt play "$@" \
        >"${run_dir}/${label}.json" 2>"${run_dir}/${label}.err"
    cat "${run_dir}/${label}.err"
    cat "${run_dir}/${label}.json"
}

parse_proc_stat_exit_state() {
    local pid="$1"
    local stat_line="$2"
    local stat_pid stat_tail
    local -a stat_fields=()
    local state flags

    if [[ ! "${pid}" =~ ^[1-9][0-9]*$ \
        || ! "${stat_line}" =~ ^([0-9]+)[[:space:]]\(.*\)[[:space:]](.*)$ ]]; then
        printf 'ERROR: malformed process stat for pid %s\n' "${pid}" >&2
        return 2
    fi
    stat_pid="${BASH_REMATCH[1]}"
    stat_tail="${BASH_REMATCH[2]}"
    if [[ "${stat_pid}" != "${pid}" ]]; then
        printf 'ERROR: process stat pid mismatch for pid %s\n' "${pid}" >&2
        return 2
    fi

    read -r -a stat_fields <<<"${stat_tail}"
    if ((${#stat_fields[@]} < 7)); then
        printf 'ERROR: malformed process stat fields for pid %s\n' "${pid}" >&2
        return 2
    fi
    state="${stat_fields[0]}"
    flags="${stat_fields[6]}"
    if [[ ! "${state}" =~ ^[A-Za-z]$ || ! "${flags}" =~ ^[0-9]+$ ]]; then
        printf 'ERROR: malformed process stat fields for pid %s\n' "${pid}" >&2
        return 2
    fi
    while [[ "${#flags}" -gt 1 && "${flags}" == 0* ]]; do
        flags="${flags#0}"
    done
    if [[ "${#flags}" -gt 10 ]] \
        || { [[ "${#flags}" == 10 ]] && [[ "${flags}" > 4294967295 ]]; }; then
        printf 'ERROR: malformed process stat flags for pid %s\n' "${pid}" >&2
        return 2
    fi

    case "${state}" in
        Z|X|x) return 1 ;;
    esac
    # PF_EXITING is set before exit_mm removes VmRSS, even while State is R/S.
    (( (10#${flags}) & 4 )) && return 1
    return 0
}

proc_stat_exit_state() {
    local pid="$1"
    local stat_path="/proc/${pid}/stat"
    local stat_line

    [[ "${pid}" =~ ^[1-9][0-9]*$ ]] || {
        printf 'ERROR: invalid process pid for stat observation: %s\n' "${pid}" >&2
        return 2
    }
    [[ -e "${stat_path}" ]] || return 1
    if ! IFS= read -r stat_line <"${stat_path}"; then
        [[ -e "${stat_path}" ]] || return 1
        printf 'ERROR: failed to read process stat for pid %s at %s\n' \
            "${pid}" "${stat_path}" >&2
        return 2
    fi
    parse_proc_stat_exit_state "${pid}" "${stat_line}"
}

proc_status_fields() {
    local pid="$1"
    local status_path="/proc/${pid}/status"
    local fields state rss threads status
    [[ -e "${status_path}" ]] || return 1
    if ! fields="$(awk '
        /^State:/ { state=$2 }
        /^VmRSS:/ { rss=$2 }
        /^Threads:/ { threads=$2 }
        END { print state "\t" rss "\t" threads }
    ' "${status_path}" 2>/dev/null)"; then
        if proc_stat_exit_state "${pid}"; then
            :
        else
            status=$?
            ((status == 1)) && return 1
            return "${status}"
        fi
        printf 'ERROR: failed to read process status for pid %s at %s\n' \
            "${pid}" "${status_path}" >&2
        return 2
    fi
    IFS=$'\t' read -r state rss threads <<<"${fields}"

    if proc_stat_exit_state "${pid}"; then
        :
    else
        status=$?
        ((status == 1)) && return 1
        return "${status}"
    fi

    case "${state}" in
        [A-Za-z]) ;;
        *)
            printf 'ERROR: invalid process status for pid %s at %s\n' \
                "${pid}" "${status_path}" >&2
            return 2
            ;;
    esac
    if [[ ! "${rss}" =~ ^[0-9]+$ || ! "${threads}" =~ ^[0-9]+$ ]]; then
        printf 'ERROR: incomplete process metrics for live pid %s at %s\n' \
            "${pid}" "${status_path}" >&2
        return 2
    fi
    printf '%s\t%s\t%s\n' "${state}" "${rss}" "${threads}"
}

sample_process() {
    local pid="$1"
    local output="$2"
    local label="$3"
    local fd_path="/proc/${pid}/fd"
    local fields state rss threads fd_output fds status
    if fields="$(proc_status_fields "${pid}")"; then
        :
    else
        status=$?
        ((status == 1)) && return 0
        return "${status}"
    fi
    IFS=$'\t' read -r state rss threads <<<"${fields}"

    if fd_output="$(find "${fd_path}" -mindepth 1 -maxdepth 1 2>/dev/null)"; then
        fds="$(awk 'NF { count++ } END { print count+0 }' <<<"${fd_output}")"
    else
        if process_is_running "${pid}"; then
            printf 'ERROR: failed to enumerate file descriptors for live pid %s at %s\n' \
                "${pid}" "${fd_path}" >&2
            return 2
        else
            status=$?
            ((status == 1)) && return 0
            return "${status}"
        fi
    fi

    if fields="$(proc_status_fields "${pid}")"; then
        :
    else
        status=$?
        ((status == 1)) && return 0
        return "${status}"
    fi
    printf '%(%s)T\t%s\t%s\t%s\t%s\n' -1 "${label}" "${rss}" "${fds}" "${threads}" >>"${output}"
}

process_is_running() {
    local pid="$1"
    local fields status
    if fields="$(proc_status_fields "${pid}")"; then
        return 0
    else
        status=$?
        return "${status}"
    fi
}

record_with_resources() {
    local label="$1"
    shift
    start_split_process "${label}" env RBT_COMPOSER_ADDR="${composer_addr}" target/debug/rbt record "$@"
    local pid="${last_pid}"
    local resources="${run_dir}/${label}-resources.tsv"
    local status
    while true; do
        if process_is_running "${pid}"; then
            sample_process "${pid}" "${resources}" recorder
        else
            status=$?
            if ((status == 1)); then
                break
            fi
            return "${status}"
        fi
        sleep 0.2
    done
    wait_pid "${pid}"
}

check_resource_bound() {
    local resources="$1"
    local label="$2"
    local warmup_samples="${3:-2}"
    awk -F '\t' -v label="${label}" -v warmup="${warmup_samples}" '
        NR <= warmup { next }
        !started { first_rss=$3; first_fd=$4; first_threads=$5; started=1 }
        { last_rss=$3; last_fd=$4; last_threads=$5; if ($3 > max_rss) max_rss=$3 }
        END {
            samples=NR-warmup
            if (samples < 2) { print label ": insufficient post-warmup resource samples" > "/dev/stderr"; exit 1 }
            if (last_rss > first_rss + 65536 || max_rss > first_rss + 131072) {
                print label ": obvious RSS growth first=" first_rss " last=" last_rss " max=" max_rss > "/dev/stderr"; exit 1
            }
            if (last_fd > first_fd + 8 || last_threads > first_threads + 2) {
                print label ": resource growth fd=" first_fd "->" last_fd " threads=" first_threads "->" last_threads > "/dev/stderr"; exit 1
            }
            print label " resources warmup_samples=" warmup " measured_samples=" samples " first_rss_kib=" first_rss " last_rss_kib=" last_rss " max_rss_kib=" max_rss " fd=" first_fd "->" last_fd " threads=" first_threads "->" last_threads
        }
    ' "${resources}" | tee "${run_dir}/${label}-resource-summary.txt"
}

mcap_unsafe_audit() {
    local audit_log="${run_dir}/unsafe-audit.log"
    python3 "${repo_root}/scripts/audit_unsafe.py" --label RBT_MCAP_UNSAFE_AUDIT 2>&1 \
        | tee "${audit_log}" \
        || die "production unsafe audit failed; see ${audit_log}"

    local mcap_unsafe
    mcap_unsafe="$({ rg -n '\bunsafe\b' crates/rbt-mcap 2>/dev/null || true; } | wc -l)"
    mcap_unsafe="${mcap_unsafe//[[:space:]]/}"
    printf 'RBT_MCAP_UNSAFE mcap=%s\n' "${mcap_unsafe}" | tee -a "${audit_log}"
    [[ "${mcap_unsafe}" == 0 ]] \
        || die "MCAP crate contains an unsafe token; see ${audit_log}"
}

run_quick() {
    run_logged sampler-regression bash tests/mcap/test_verify_mcap_sampler.sh
    run_logged cargo-fmt cargo fmt --all -- --check
    run_logged mcap-tests cargo test --offline -p rbt-types -p rbt-idl -p rbt-mcap -p rbt-cli -p rbt-compose
    run_logged mcap-clippy cargo clippy --offline -p rbt-mcap -p rbt-cli -p mcap-source --all-targets -- -D warnings
    mcap_unsafe_audit

    local generation_one="${tmp_root}/generation-one"
    local generation_two="${tmp_root}/generation-two"
    mkdir -p "${generation_one}" "${generation_two}"
    run_logged idl-generate-one cargo run --offline --quiet -p rbt-idl -- generate \
        types/sensor.rbt types/control.rbt types/demo.rbt types/tf.rbt \
        --rust "${generation_one}/rust" --cpp "${generation_one}/cpp" \
        --descriptor "${generation_one}/descriptors" --python "${generation_one}/python"
    run_logged idl-generate-two cargo run --offline --quiet -p rbt-idl -- generate \
        types/sensor.rbt types/control.rbt types/demo.rbt types/tf.rbt \
        --rust "${generation_two}/rust" --cpp "${generation_two}/cpp" \
        --descriptor "${generation_two}/descriptors" --python "${generation_two}/python"
    diff -ru "${generation_one}" "${generation_two}" >"${run_dir}/idl-determinism.diff"
    diff -ru "${generation_one}/rust" generated/rust >"${run_dir}/idl-rust-checkin.diff"
    diff -ru "${generation_one}/cpp" generated/cpp >"${run_dir}/idl-cpp-checkin.diff"
    diff -ru "${generation_one}/descriptors" generated/descriptors >"${run_dir}/idl-descriptor-checkin.diff"
    diff -ru --exclude=__pycache__ --exclude=__init__.py \
        "${generation_one}/python" python/rbt/_generated \
        >"${run_dir}/idl-python-checkin.diff"

    local fixture_one="${tmp_root}/fixture-one"
    local fixture_two="${tmp_root}/fixture-two"
    run_logged fixture-generate-one cargo run --offline --quiet -p rbt-mcap --bin rbt-mcap-fixtures -- generate "${fixture_one}"
    run_logged fixture-generate-two cargo run --offline --quiet -p rbt-mcap --bin rbt-mcap-fixtures -- generate "${fixture_two}"
    run_logged fixture-checkin cargo run --offline --quiet -p rbt-mcap --bin rbt-mcap-fixtures -- verify tests/fixtures/mcap
    for fixture in image-small.mcap pointcloud-small.mcap control-small.mcap; do
        cmp "${fixture_one}/${fixture}" "${fixture_two}/${fixture}"
    done
    # The mcap crate serializes a mixed fixture's metadata HashMaps in a
    # process-randomized order. Both generations above are independently
    # checked against exact expected topics, order, log times, and payloads.
    sha256sum "${fixture_one}"/*.mcap "${fixture_two}"/*.mcap \
        >"${run_dir}/fixture-sha256.txt"

    run_logged rust-build cargo build --offline -p composer -p rbt-cli -p mcap-source -p control-stream -p image-consumer
    run_logged cpp-configure cmake -S sdk/cpp -B "${cpp_build}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER="${CXX}" -DCMAKE_PREFIX_PATH="${cpp_prefix}"
    run_logged cpp-build cmake --build "${cpp_build}" --target detector_component camera_component python_component_viewer --parallel 2

    run_logged info-without-composer env RBT_COMPOSER_ADDR=127.0.0.1:1 \
        target/debug/rbt mcap info tests/fixtures/mcap/mixed-small.mcap --json
    env RBT_COMPOSER_ADDR=127.0.0.1:1 target/debug/rbt mcap info \
        tests/fixtures/mcap/mixed-small.mcap --json >"${run_dir}/fixture-info.json"
    jq -e '.message_count == 5 and .duration_ns == 100000000 and (.channels | length) == 3' \
        "${run_dir}/fixture-info.json" >/dev/null

    cp tests/fixtures/mcap/mixed-small.mcap "${tmp_root}/corrupt.mcap"
    truncate -s -1 "${tmp_root}/corrupt.mcap"
    expect_failure corrupt-info target/debug/rbt mcap info "${tmp_root}/corrupt.mcap"
    expect_failure corrupt-play env RBT_COMPOSER_ADDR=127.0.0.1:1 \
        target/debug/rbt play "${tmp_root}/corrupt.mcap" --rate 0

    local port=$((20000 + $$ % 20000))
    composer_addr="127.0.0.1:${port}"
    start_composer composer
    start_process mixed-source env RBT_COMPOSER_ADDR="${composer_addr}" \
        RBT_MCAP_SOURCE_RATE_HZ=30 target/debug/mcap-source mixed
    local source_pid="${last_pid}"
    wait_capabilities mcap-source 3 "${run_dir}/mixed-source-active.json"
    record_foreground rust-mixed-record --output "${run_dir}/rust-mixed.mcap" --all --duration 0.8
    jq -e '.complete == true and .messages_recorded > 50 and (.channels | length) == 3 and .file_size_bytes > 0' \
        "${run_dir}/rust-mixed-record.json" >/dev/null
    target/debug/rbt mcap info "${run_dir}/rust-mixed.mcap" --json >"${run_dir}/rust-mixed-info.json"
    jq -e '.message_count > 50 and (.channels | length) == 3' "${run_dir}/rust-mixed-info.json" >/dev/null

    start_split_process sigint-record env RBT_COMPOSER_ADDR="${composer_addr}" target/debug/rbt record \
        --output "${run_dir}/sigint.mcap" --capability Camera
    local sigint_record_pid="${last_pid}"
    wait_log "${run_dir}/sigint-record.err" 'selected 1 fixed physical recording source(s)'
    sleep 0.3
    kill -INT "${sigint_record_pid}"
    wait_pid "${sigint_record_pid}"
    jq -e '.complete == true and .messages_recorded > 0 and .file_size_bytes > 0' \
        "${run_dir}/sigint-record.json" >/dev/null
    stop_pid "${source_pid}"

    play_foreground rate-zero "${run_dir}/rust-mixed.mcap" --rate 0
    jq -e --slurpfile info "${run_dir}/rust-mixed-info.json" \
        '.messages_published == $info[0].message_count and .elapsed_ns < 1000000000' \
        "${run_dir}/rate-zero.json" >/dev/null

    start_process cpp-detector env RBT_COMPOSER_ADDR="${composer_addr}" RBT_TEST_TRACE_FRAMES=1 \
        "${cpp_build}/detector_component"
    local detector_pid="${last_pid}"
    play_foreground rust-to-cpp "${run_dir}/rust-mixed.mcap" --rate 1
    wait_log "${run_dir}/cpp-detector.log" 'DETECTOR PROCESS'
    awk '
        /DETECTOR PROCESS/ {
            sequence=""; value="";
            for (i=1; i<=NF; ++i) {
                if ($i ~ /^sequence=/) { split($i, a, "="); sequence=a[2] }
                if ($i ~ /^value=/) { split($i, a, "="); value=a[2] }
            }
            if (sequence == "" || value != (sequence % 256) * 10) exit 1;
            count++
        }
        END { if (count < 3) exit 1; print "RUST_MCAP_CPP exact_samples=" count }
    ' "${run_dir}/cpp-detector.log" | tee "${run_dir}/rust-to-cpp-evidence.txt"
    stop_pid "${detector_pid}"

    capability_list_json >"${run_dir}/quick-final-topology.json"
    jq -e '[.[] | select(.provider_component == "rbt-player")] | length == 0' \
        "${run_dir}/quick-final-topology.json" >/dev/null
    printf 'RBT_MCAP_QUICK_PASS logs=%s\n' "${run_dir}" | tee "${run_dir}/result.txt"
}

run_full_extensions() {
    run_logged large-fixtures cargo run --offline --quiet -p rbt-mcap --bin rbt-mcap-fixtures -- \
        generate-large "${run_dir}/large-fixtures"

    play_foreground timing-1x tests/fixtures/mcap/mixed-small.mcap --rate 1
    play_foreground timing-2x tests/fixtures/mcap/mixed-small.mcap --rate 2
    play_foreground timing-fast tests/fixtures/mcap/mixed-small.mcap --rate 0
    jq -e '.elapsed_ns >= 80000000 and .elapsed_ns < 1000000000' "${run_dir}/timing-1x.json" >/dev/null
    jq -e '.elapsed_ns >= 35000000 and .elapsed_ns < 800000000' "${run_dir}/timing-2x.json" >/dev/null
    jq -e --slurpfile one "${run_dir}/timing-1x.json" '.elapsed_ns < $one[0].elapsed_ns' \
        "${run_dir}/timing-2x.json" >/dev/null

    run_logged full-rust-build cargo build --offline -p camera-a -p detector
    start_process cpp-camera env RBT_COMPOSER_ADDR="${composer_addr}" "${cpp_build}/camera_component"
    local cpp_camera_pid="${last_pid}"
    wait_capabilities cpp-camera 1 "${run_dir}/cpp-camera-active.json"
    record_foreground cpp-image-record --output "${run_dir}/cpp-image.mcap" --capability Camera --duration 0.8
    stop_pid "${cpp_camera_pid}"
    jq -e '.complete == true and .messages_recorded >= 5' "${run_dir}/cpp-image-record.json" >/dev/null
    start_process rust-detector env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=debug target/debug/detector
    local rust_detector_pid="${last_pid}"
    play_foreground cpp-to-rust "${run_dir}/cpp-image.mcap" --rate 1
    wait_log "${run_dir}/rust-detector.log" 'published detection'
    grep -Eq 'value=1950([^0-9]|$)' "${run_dir}/rust-detector.log" \
        || die "Rust detector did not preserve the C++ camera payload"
    stop_pid "${rust_detector_pid}"

    local python="${RBT_MCAP_PYTHON:-${repo_root}/target/python-verify-venv/bin/python}"
    [[ -x "${python}" ]] || die "full MCAP gate requires the verified Python environment at ${python}"
    "${python}" -c 'import rbt, numpy; print(rbt.__version__, numpy.__version__)' \
        >"${run_dir}/python-environment.txt"
    start_process rust-camera env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info target/debug/camera-a
    local rust_camera_pid="${last_pid}"
    wait_capabilities camera-a 1 "${run_dir}/rust-camera-active.json"
    start_process python-detector-source env RBT_COMPOSER_ADDR="${composer_addr}" PYTHONUNBUFFERED=1 \
        "${python}" examples/python/component_detector.py
    local python_source_pid="${last_pid}"
    wait_capabilities detector 1 "${run_dir}/python-detector-active.json"
    record_foreground python-point-record --output "${run_dir}/python-point.mcap" --capability Detections --duration 0.8
    stop_pid "${python_source_pid}"
    stop_pid "${rust_camera_pid}"
    jq -e '.complete == true and .messages_recorded >= 5' "${run_dir}/python-point-record.json" >/dev/null
    start_process cpp-point-viewer env RBT_COMPOSER_ADDR="${composer_addr}" "${cpp_build}/python_component_viewer"
    local cpp_viewer_pid="${last_pid}"
    play_foreground python-to-cpp "${run_dir}/python-point.mcap" --rate 1
    wait_log "${run_dir}/cpp-point-viewer.log" 'PYTHON_VIEWER SAMPLE'
    stop_pid "${cpp_viewer_pid}"

    start_process python-replay-consumer env RBT_COMPOSER_ADDR="${composer_addr}" PYTHONUNBUFFERED=1 \
        RBT_TEST_PY_RETAIN_INPUT_VIEW=1 \
        "${python}" examples/python/component_detector.py
    local python_consumer_pid="${last_pid}"
    play_foreground mcap-to-python "${run_dir}/rust-mixed.mcap" --rate 1
    wait_log "${run_dir}/python-replay-consumer.log" 'DETECTOR SAMPLE'
    wait_log "${run_dir}/python-replay-consumer.log" 'RETAINED INPUT VIEW'
    grep -Eq 'pointer=[0-9]+' "${run_dir}/python-replay-consumer.log" \
        || die "Python replay consumer did not expose its zero-copy NumPy payload pointer"
    stop_pid "${python_consumer_pid}"

    start_process mixed-cpp-viewer env RBT_COMPOSER_ADDR="${composer_addr}" "${cpp_build}/python_component_viewer"
    local mixed_viewer_pid="${last_pid}"
    start_process mixed-control-consumer env RBT_COMPOSER_ADDR="${composer_addr}" target/debug/control-stream consumer
    local mixed_control_pid="${last_pid}"
    play_foreground mixed-native-replay "${run_dir}/rust-mixed.mcap" --rate 1
    wait_log "${run_dir}/mixed-cpp-viewer.log" 'PYTHON_VIEWER SAMPLE'
    wait_log "${run_dir}/mixed-control-consumer.log" 'CONTROL_SINK sequence='
    stop_pid "${mixed_viewer_pid}"
    stop_pid "${mixed_control_pid}"

    start_process large-image-source env RBT_COMPOSER_ADDR="${composer_addr}" \
        RBT_MCAP_SOURCE_RATE_HZ=8 target/debug/mcap-source large-image
    local large_image_source_pid="${last_pid}"
    wait_capabilities mcap-source 1 "${run_dir}/large-image-active.json"
    record_with_resources large-image-record --output "${run_dir}/large-image.mcap" \
        --capability Camera --buffer-bytes 67108864 --duration 1.5
    stop_pid "${large_image_source_pid}"
    jq -e '.messages_recorded >= 5 and .payload_bytes >= 31104000 and .queue_high_water_bytes <= .queue_budget_bytes' \
        "${run_dir}/large-image-record.json" >/dev/null
    check_resource_bound "${run_dir}/large-image-record-resources.tsv" large-image-record

    start_process large-point-source env RBT_COMPOSER_ADDR="${composer_addr}" \
        RBT_MCAP_SOURCE_RATE_HZ=2 RBT_MCAP_SOURCE_POINT_COUNT=1310720 \
        target/debug/mcap-source large-pointcloud
    local large_point_source_pid="${last_pid}"
    wait_capabilities mcap-source 1 "${run_dir}/large-point-active.json"
    record_with_resources large-point-record --output "${run_dir}/large-point.mcap" \
        --capability Detections --buffer-bytes 67108864 --duration 2.2
    stop_pid "${large_point_source_pid}"
    jq -e '.messages_recorded >= 2 and .payload_bytes >= 41943040 and .queue_high_water_bytes <= .queue_budget_bytes' \
        "${run_dir}/large-point-record.json" >/dev/null
    check_resource_bound "${run_dir}/large-point-record-resources.tsv" large-point-record

    start_process pressure-image-source env RBT_COMPOSER_ADDR="${composer_addr}" \
        RBT_MCAP_SOURCE_RATE_HZ=10 target/debug/mcap-source large-image
    local pressure_source_pid="${last_pid}"
    wait_capabilities mcap-source 1 "${run_dir}/pressure-image-active.json"
    local pressure_service
    pressure_service="$(jq -r '.[] | select(.provider_component == "mcap-source") | .physical_endpoint' \
        "${run_dir}/pressure-image-active.json")"
    start_process pressure-fast-peer env RBT_IMAGE_SERVICE="${pressure_service}" RBT_IMAGE_FRAMES=5 \
        RBT_IMAGE_START_DELAY_MS=0 target/debug/image-consumer
    local pressure_peer_pid="${last_pid}"
    record_foreground sensor-overload --output "${run_dir}/sensor-overload.mcap" \
        --capability Camera --buffer-bytes 1048576 --duration 1.0
    wait_pid "${pressure_peer_pid}"
    stop_pid "${pressure_source_pid}"
    jq -e '.complete == false and .recorder_drops > 0 and .queue_high_water_bytes <= .queue_budget_bytes' \
        "${run_dir}/sensor-overload.json" >/dev/null
    grep -Fq 'IMAGE_CONSUMER received=5' "${run_dir}/pressure-fast-peer.log" \
        || die "fast Sensor peer was disrupted by recorder overload"

    start_process control-pressure-source env RBT_COMPOSER_ADDR="${composer_addr}" \
        RBT_MCAP_SOURCE_RATE_HZ=20 target/debug/mcap-source mixed
    local control_source_pid="${last_pid}"
    wait_capabilities mcap-source 3 "${run_dir}/control-pressure-active.json"
    expect_failure control-overload env RBT_COMPOSER_ADDR="${composer_addr}" target/debug/rbt record \
        --output "${run_dir}/control-overload.mcap" --capability MotionCommand --buffer-bytes 8 --duration 1
    grep -Fq 'was not silently dropped' "${run_dir}/control-overload.log" \
        || die "Control recorder overload did not report its terminal policy"
    target/debug/rbt mcap info "${run_dir}/control-overload.mcap" --json \
        >"${run_dir}/control-overload-info.json"
    jq -e '
        [.metadata[] | select(.[0] == "rbt.recording.summary") | .[1].complete] == ["false"]
    ' "${run_dir}/control-overload-info.json" >/dev/null
    expect_failure disk-full env RBT_COMPOSER_ADDR="${composer_addr}" target/debug/rbt record \
        --output /dev/full --capability Camera --duration 0.5
    kill -0 "${control_source_pid}" 2>/dev/null || die "writer failure terminated an unrelated source component"
    stop_pid "${control_source_pid}"

    start_process outage-source env RBT_COMPOSER_ADDR="${composer_addr}" \
        RBT_MCAP_SOURCE_RATE_HZ=20 target/debug/mcap-source mixed
    local outage_source_pid="${last_pid}"
    wait_capabilities mcap-source 3 "${run_dir}/outage-source-active.json"
    start_split_process outage-record env RBT_COMPOSER_ADDR="${composer_addr}" target/debug/rbt record \
        --output "${run_dir}/outage.mcap" --all --duration 3.0
    local outage_record_pid="${last_pid}"
    sleep 0.7
    stop_pid "${composer_pid}" KILL
    composer_pid=''
    sleep 0.8
    start_composer composer-record-restarted
    wait_capabilities mcap-source 3 "${run_dir}/outage-source-reconstructed.json"
    wait_pid "${outage_record_pid}"
    jq -e '.complete == true and .messages_recorded > 120 and .transport_sequence_gaps == 0' \
        "${run_dir}/outage-record.json" >/dev/null
    wait_log "${run_dir}/composer-record-restarted.log" \
        'reconciliation: 0 deactivate, 0 activate'

    start_split_process player-restart env RBT_COMPOSER_ADDR="${composer_addr}" target/debug/rbt play \
        "${run_dir}/outage.mcap" --rate 1
    local player_restart_pid="${last_pid}"
    wait_capabilities rbt-player 3 "${run_dir}/player-before-restart.json"
    inspect_json >"${run_dir}/player-before-restart-ps.json"
    jq -e '
        ([.components[] | select(.component_id == "rbt-player" and .state == "ACTIVE") | .process_instance_id] | length) == 1 and
        ([.components[] | select(.component_id == "mcap-source" and .state == "ACTIVE") | .process_instance_id] | length) == 1 and
        ([.components[] | select(.component_id == "rbt-player" and .state == "ACTIVE") | .process_instance_id][0] !=
         [.components[] | select(.component_id == "mcap-source" and .state == "ACTIVE") | .process_instance_id][0])
    ' "${run_dir}/player-before-restart-ps.json" >/dev/null
    jq -e '
        ([.[] | select(.provider_component == "rbt-player") | .capability_instance] as $player_ids |
         [.[] | select(.provider_component == "mcap-source") | .capability_instance] as $source_ids |
         ($player_ids | length) == 3 and
         ([$player_ids[] as $id | $source_ids[] | select(. == $id)] | length) == 0)
    ' "${run_dir}/player-before-restart.json" >/dev/null
    stop_pid "${composer_pid}" KILL
    composer_pid=''
    sleep 0.5
    start_composer composer-player-restarted
    wait_capabilities rbt-player 3 "${run_dir}/player-after-restart.json"
    inspect_json >"${run_dir}/player-after-restart-ps.json"
    jq -e --slurpfile before "${run_dir}/player-before-restart-ps.json" '
        ([.components[] | select(.component_id == "rbt-player" and .state == "ACTIVE") | .process_instance_id] | length) == 1 and
        ([.components[] | select(.component_id == "rbt-player" and .state == "ACTIVE") | .activation_id] | length) == 1 and
        ([.components[] | select(.component_id == "rbt-player" and .state == "ACTIVE") | .process_instance_id] ==
         [$before[0].components[] | select(.component_id == "rbt-player" and .state == "ACTIVE") | .process_instance_id]) and
        ([.components[] | select(.component_id == "rbt-player" and .state == "ACTIVE") | .activation_id] ==
         [$before[0].components[] | select(.component_id == "rbt-player" and .state == "ACTIVE") | .activation_id])
    ' "${run_dir}/player-after-restart-ps.json" >/dev/null
    jq -e --slurpfile before "${run_dir}/player-before-restart.json" '
        ([.[] | select(.provider_component == "rbt-player") | .capability_instance] | sort) ==
        ([$before[0][] | select(.provider_component == "rbt-player") | .capability_instance] | sort)
    ' "${run_dir}/player-after-restart.json" >/dev/null
    wait_pid "${player_restart_pid}"
    wait_log "${run_dir}/composer-player-restarted.log" \
        'reconciliation: 0 deactivate, 0 activate'
    stop_pid "${outage_source_pid}"

    local composer_resources="${run_dir}/replay-soak-composer.tsv"
    sample_process "${composer_pid}" "${composer_resources}" composer
    local iteration
    for iteration in $(seq 1 100); do
        env RBT_COMPOSER_ADDR="${composer_addr}" target/debug/rbt play \
            tests/fixtures/mcap/control-small.mcap --rate 0 \
            >"${run_dir}/replay-soak-${iteration}.json" 2>"${run_dir}/replay-soak-${iteration}.err"
        jq -e '.messages_published == 3' "${run_dir}/replay-soak-${iteration}.json" >/dev/null
        if ((iteration % 10 == 0)); then
            sample_process "${composer_pid}" "${composer_resources}" composer
        fi
    done
    sample_process "${composer_pid}" "${composer_resources}" composer
    check_resource_bound "${composer_resources}" replay-soak
    capability_list_json >"${run_dir}/replay-soak-final.json"
    jq -e '[.[] | select(.provider_component == "rbt-player")] | length == 0' \
        "${run_dir}/replay-soak-final.json" >/dev/null

    start_process soak-source env RBT_COMPOSER_ADDR="${composer_addr}" \
        RBT_MCAP_SOURCE_RATE_HZ=20 target/debug/mcap-source mixed
    local soak_source_pid="${last_pid}"
    wait_capabilities mcap-source 3 "${run_dir}/soak-source-active.json"
    local soak_seconds="${RBT_MCAP_SOAK_SECONDS:-300}"
    [[ "${soak_seconds}" =~ ^[0-9]+$ ]] && ((soak_seconds >= 300)) \
        || die "RBT_MCAP_SOAK_SECONDS must be an integer of at least 300 for the full gate"
    start_split_process record-soak env RBT_COMPOSER_ADDR="${composer_addr}" target/debug/rbt record \
        --output "${run_dir}/record-soak.mcap" --all --duration "${soak_seconds}"
    local soak_record_pid="${last_pid}"
    local soak_resources="${run_dir}/record-soak-resources.tsv"
    local process_status
    while true; do
        if process_is_running "${soak_record_pid}"; then
            sample_process "${soak_record_pid}" "${soak_resources}" recorder
            sample_process "${soak_source_pid}" "${soak_resources}" source
        else
            process_status=$?
            if ((process_status == 1)); then
                break
            fi
            return "${process_status}"
        fi
        sleep 10
    done
    wait_pid "${soak_record_pid}"
    stop_pid "${soak_source_pid}"
    awk -F '\t' '$2 == "recorder"' "${soak_resources}" >"${run_dir}/record-soak-recorder.tsv"
    awk -F '\t' '$2 == "source"' "${soak_resources}" >"${run_dir}/record-soak-source.tsv"
    check_resource_bound "${run_dir}/record-soak-recorder.tsv" record-soak-recorder
    check_resource_bound "${run_dir}/record-soak-source.tsv" record-soak-source
    jq -e --argjson seconds "${soak_seconds}" '
        .complete == true and .recorder_drops == 0 and .writer_errors == 0 and
        .messages_recorded >= ($seconds * 40) and .queue_high_water_bytes <= .queue_budget_bytes
    ' "${run_dir}/record-soak.json" >/dev/null

    stop_pid "${composer_pid}"
    composer_pid=''
    if [[ "${skip_inherited}" == 1 ]]; then
        printf 'SKIP: inherited full gates are scheduled by the parent composition\n' \
            | tee "${run_dir}/inherited-gates.log"
    else
        run_logged inherited-v01 ./scripts/verify_v01.sh --full
        run_logged inherited-waitset ./scripts/verify_waitset.sh --full
        run_logged inherited-python ./scripts/verify_python.sh --full
        run_logged inherited-python-publisher ./scripts/verify_python_publisher.sh --full
        run_logged inherited-python-component ./scripts/verify_python_component.sh --full
        run_logged inherited-python-asyncio ./scripts/verify_python_asyncio.sh --full
    fi
    printf 'RBT_MCAP_FULL_PASS logs=%s\n' "${run_dir}" | tee -a "${run_dir}/result.txt"
}

printf 'rbt MCAP verifier mode=%s logs=%s\n' "${mode}" "${run_dir}"
run_quick
if [[ "${mode}" == full ]]; then
    run_full_extensions
fi

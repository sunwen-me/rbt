#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
export PATH="${toolchain_root}/bin${PATH:+:${PATH}}"
export CC="${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc"
export CXX="${toolchain_root}/bin/x86_64-conda-linux-gnu-g++"
export AR="${toolchain_root}/bin/x86_64-conda-linux-gnu-ar"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
export LD_LIBRARY_PATH="${toolchain_root}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export LIBRARY_PATH="${toolchain_root}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"

for command in cargo jq /usr/bin/time; do
    command -v "${command}" >/dev/null 2>&1 || {
        printf 'missing required command: %s\n' "${command}" >&2
        exit 1
    }
done

run_id="mcap-benchmark-$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="${RBT_MCAP_BENCH_LOG_DIR:-${repo_root}/target/mcap-benchmark-logs}/${run_id}"
mkdir -p "${run_dir}"
composer_addr="127.0.0.1:$((40000 + $$ % 20000))"
composer_pid=''
source_pid=''

cleanup() {
    [[ -z "${source_pid}" ]] || kill -TERM "${source_pid}" 2>/dev/null || true
    [[ -z "${composer_pid}" ]] || kill -TERM "${composer_pid}" 2>/dev/null || true
    [[ -z "${source_pid}" ]] || wait "${source_pid}" 2>/dev/null || true
    [[ -z "${composer_pid}" ]] || wait "${composer_pid}" 2>/dev/null || true
}
trap cleanup EXIT

cargo build --offline -p composer -p rbt-cli -p mcap-source
env RBT_COMPOSER_ADDR="${composer_addr}" target/debug/composer \
    >"${run_dir}/composer.log" 2>&1 &
composer_pid=$!
for _ in $(seq 1 300); do
    if env RBT_COMPOSER_ADDR="${composer_addr}" target/debug/rbt ps --json \
        >"${run_dir}/ready.json" 2>/dev/null; then
        break
    fi
    sleep 0.05
done
env RBT_COMPOSER_ADDR="${composer_addr}" target/debug/rbt ps --json >/dev/null

stop_source() {
    kill -TERM "${source_pid}" 2>/dev/null || true
    wait "${source_pid}" 2>/dev/null || true
    source_pid=''
    for _ in $(seq 1 300); do
        local count
        count="$(env RBT_COMPOSER_ADDR="${composer_addr}" target/debug/rbt ps --json 2>/dev/null \
            | jq '[.capability_topology[] | select(.provider_component == "mcap-source")] | length')"
        [[ "${count}" == 0 ]] && return 0
        sleep 0.05
    done
    printf 'mcap-source topology did not clear\n' >&2
    exit 1
}

run_case() {
    local label="$1"
    local mode="$2"
    local rate="$3"
    local capability="$4"
    local duration="$5"
    local point_count="${6:-}"
    local -a source_env=(RBT_COMPOSER_ADDR="${composer_addr}" RBT_MCAP_SOURCE_RATE_HZ="${rate}")
    [[ -z "${point_count}" ]] || source_env+=(RBT_MCAP_SOURCE_POINT_COUNT="${point_count}")
    env "${source_env[@]}" target/debug/mcap-source "${mode}" \
        >"${run_dir}/${label}-source.log" 2>&1 &
    source_pid=$!

    for _ in $(seq 1 300); do
        local count
        count="$(env RBT_COMPOSER_ADDR="${composer_addr}" target/debug/rbt ps --json 2>/dev/null \
            | jq '[.capability_topology[] | select(.provider_component == "mcap-source")] | length')"
        [[ "${count}" == 1 ]] && break
        sleep 0.05
    done
    /usr/bin/time -o "${run_dir}/${label}.time" \
        -f $'user_cpu_s=%U\nsystem_cpu_s=%S\nmax_rss_kib=%M\nwall_s=%e' \
        env RBT_COMPOSER_ADDR="${composer_addr}" target/debug/rbt record \
        --output "${run_dir}/${label}.mcap" --capability "${capability}" \
        --buffer-bytes 67108864 --duration "${duration}" \
        >"${run_dir}/${label}.json" 2>"${run_dir}/${label}.err"
    stop_source
}

# Image's 32-byte canonical header + 8-byte count + 24-byte RGB payload
# gives an exact 64-byte canonical record payload.
run_case record-64b small-image 500 Camera 2.0
run_case record-6220800b large-image 8 Camera 1.5
run_case record-20mib large-pointcloud 2 Detections 2.2 1310720

jq -e '.complete and .messages_recorded > 100 and
       .payload_bytes == (.messages_recorded * 64)' \
    "${run_dir}/record-64b.json" >/dev/null
jq -e '.complete and .messages_recorded >= 5 and .recorder_drops == 0' \
    "${run_dir}/record-6220800b.json" >/dev/null
jq -e '.complete and .messages_recorded >= 2 and .recorder_drops == 0' \
    "${run_dir}/record-20mib.json" >/dev/null

for label in record-64b record-6220800b record-20mib; do
    jq -r --arg label "${label}" '
        "RBT_MCAP_BENCH case=\($label) messages=\(.messages_recorded) payload_bytes=\(.payload_bytes) duration_ns=\(.duration_ns) file_size_bytes=\(.file_size_bytes) queue_high_water_bytes=\(.queue_high_water_bytes) drops=\(.recorder_drops)"
    ' "${run_dir}/${label}.json"
    tr '\n' ' ' <"${run_dir}/${label}.time"
    printf '\n'
done | tee "${run_dir}/results.txt"

printf 'RBT_MCAP_BENCH_PASS logs=%s\n' "${run_dir}" | tee -a "${run_dir}/results.txt"

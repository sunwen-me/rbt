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
usage: scripts/benchmark_rpc.sh [options]

Run the local, process-separated native rbt RPC benchmark matrix. The
reported latency is the client-side monotonic round trip; every response is
validated for nonce, result, and checksum. This benchmark intentionally does
not compare frameworks.

matrix:
  frame-16       16 B request / 16 B response
  image-1k-1k    1 KiB request / 1 KiB response
  image-1k-1m    1 KiB request / 1 MiB response

options:
  --smoke                 run the fixed matrix with the supplied count
  --include-dynamic-20m  run the optional 1 KiB / 20 MiB Image case when
                          the binary reports dynamic RPC support
  --count N               measured calls per case (default: 20)
  --warmup N              validated but unmeasured calls (default: 3)
  --timeout-ms N          per-call deadline (default: 30000)
  --debug                 use target/debug/rbt-rpc-bench
  --help                  show this help

Environment overrides:
  RBT_RPC_BENCH_BINARY, RBT_RPC_BENCH_COUNT, RBT_RPC_BENCH_WARMUP,
  RBT_RPC_BENCH_TIMEOUT_MS, RBT_RPC_BENCH_LOG_DIR, RBT_RPC_BENCH_SKIP_BUILD.
EOF
}

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

require_positive_integer() {
    local name="$1"
    local value="$2"
    [[ "$value" =~ ^[1-9][0-9]*$ ]] || die "$name must be a positive integer: $value"
}

require_nonnegative_integer() {
    local name="$1"
    local value="$2"
    [[ "$value" =~ ^[0-9]+$ ]] || die "$name must be a non-negative integer: $value"
}

smoke=0
include_dynamic_20m=0
build_mode="release"
count="${RBT_RPC_BENCH_COUNT:-20}"
warmup="${RBT_RPC_BENCH_WARMUP:-3}"
timeout_ms="${RBT_RPC_BENCH_TIMEOUT_MS:-30000}"

while (($# > 0)); do
    case "$1" in
        --smoke)
            smoke=1
            shift
            ;;
        --include-dynamic-20m)
            include_dynamic_20m=1
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
        --help|-h)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1"
            ;;
    esac
done

require_positive_integer count "$count"
require_nonnegative_integer warmup "$warmup"
require_positive_integer timeout_ms "$timeout_ms"

require_command awk
require_command cargo
require_command grep
require_command rustc
require_command sleep
require_command timeout
require_command date

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
if [[ -d "${toolchain_root}/bin" ]]; then
    toolchain_bin="${toolchain_root}/bin"
    export PATH="${toolchain_bin}${PATH:+:${PATH}}"
    if [[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-cc" ]]; then
        export CC="${toolchain_bin}/x86_64-conda-linux-gnu-cc"
        export CXX="${toolchain_bin}/x86_64-conda-linux-gnu-c++"
        export AR="${toolchain_bin}/x86_64-conda-linux-gnu-ar"
        export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
        export LD_LIBRARY_PATH="${toolchain_root}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
    fi
fi

if [[ -n "${RBT_RPC_BENCH_BINARY:-}" ]]; then
    binary="${RBT_RPC_BENCH_BINARY}"
elif [[ "${RBT_RPC_BENCH_SKIP_BUILD:-0}" == 1 ]]; then
    if [[ "$build_mode" == "release" ]]; then
        binary="${repo_root}/target/release/rbt-rpc-bench"
    else
        binary="${repo_root}/target/debug/rbt-rpc-bench"
    fi
else
    if [[ "$build_mode" == "release" ]]; then
        cargo build --release -p rbt-rpc-bench
        binary="${repo_root}/target/release/rbt-rpc-bench"
    else
        cargo build -p rbt-rpc-bench
        binary="${repo_root}/target/debug/rbt-rpc-bench"
    fi
fi
[[ -x "$binary" ]] || die "rbt-rpc-bench executable is missing or not executable: $binary"

log_root="${RBT_RPC_BENCH_LOG_DIR:-${repo_root}/target/rpc-bench-logs}"
mkdir -p "$log_root"
run_id="rpc-$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="${log_root}/${run_id}"
mkdir -p "$run_dir"

declare -a child_pids=()

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

printf 'RBT_RPC_BENCH_RUN run_id=%s build_mode=%s binary=%s smoke=%s\n' \
    "$run_id" "$build_mode" "$binary" "$smoke"
printf 'RBT_RPC_BENCH_METHOD count=%s warmup=%s timeout_ms=%s processes=server,client clock=monotonic direction=round_trip\n' \
    "$count" "$warmup" "$timeout_ms"

capability_line="$("$binary" capabilities 2>/dev/null || true)"
if [[ "$capability_line" == *"dynamic_20m=available"* ]]; then
    printf '%s\n' "$capability_line"
    dynamic_available=1
else
    dynamic_available=0
    printf 'RBT_RPC_BENCH_CAPABILITY native_rpc=unknown dynamic_image=unknown dynamic_20m=unavailable reason=dynamic_rpc_capability_not_reported\n'
fi

wait_for_ready() {
    local log_file="$1"
    local pid="$2"
    local deadline=$((SECONDS + (timeout_ms + 5000) / 1000 + 5))
    while ((SECONDS < deadline)); do
        if grep -q 'RBT_RPC_BENCH_READY' "$log_file" 2>/dev/null; then
            return 0
        fi
        if ! kill -0 "$pid" 2>/dev/null; then
            cat "$log_file" >&2 || true
            die "RPC benchmark server exited before READY"
        fi
        sleep 0.02
    done
    cat "$log_file" >&2 || true
    die "timed out waiting for RPC benchmark server READY"
}

wait_for_pid() {
    local pid="$1"
    local status=0
    wait "$pid" || status=$?
    local remaining=()
    local candidate
    for candidate in "${child_pids[@]:-}"; do
        [[ "${candidate}" == "${pid}" ]] || remaining+=("${candidate}")
    done
    child_pids=("${remaining[@]}")
    return "$status"
}

run_case() {
    local label="$1"
    local service="rbt/bench/rpc/${run_id}/${label}"
    local server_log="${run_dir}/${label}.server.log"
    local client_log="${run_dir}/${label}.client.log"
    local server_err="${run_dir}/${label}.server.err"
    local client_err="${run_dir}/${label}.client.err"
    local timeout_seconds=$(( (timeout_ms + 999) / 1000 + 20 ))

    printf 'RBT_RPC_BENCH_CASE case=%s service=%s\n' "$label" "$service"
    : >"$server_log"
    : >"$server_err"
    : >"$client_log"
    : >"$client_err"
    "$binary" server \
        --service "$service" \
        --case "$label" \
        --count "$count" \
        --warmup "$warmup" \
        --timeout-ms "$timeout_ms" \
        >"$server_log" 2>"$server_err" &
    local server_pid=$!
    child_pids+=("$server_pid")
    wait_for_ready "$server_log" "$server_pid"

    local client_status=0
    timeout "${timeout_seconds}s" "$binary" client \
        --service "$service" \
        --case "$label" \
        --count "$count" \
        --warmup "$warmup" \
        --timeout-ms "$timeout_ms" \
        >"$client_log" 2>"$client_err" || client_status=$?

    if ((client_status != 0)) && kill -0 "$server_pid" 2>/dev/null; then
        kill -TERM "$server_pid" 2>/dev/null || true
    fi
    local server_status=0
    wait_for_pid "$server_pid" || server_status=$?

    cat "$server_log"
    cat "$client_log"
    if [[ -s "$server_err" ]]; then
        cat "$server_err" >&2
    fi
    if [[ -s "$client_err" ]]; then
        cat "$client_err" >&2
    fi
    if ((client_status != 0 || server_status != 0)); then
        die "RPC benchmark case failed case=$label client_status=$client_status server_status=$server_status; logs=$run_dir"
    fi
}

run_case frame-16
run_case image-1k-1k
run_case image-1k-1m

if ((include_dynamic_20m)); then
    if ((dynamic_available)); then
        run_case image-1k-20m
    else
        printf 'RBT_RPC_BENCH_CAPABILITY case=image-1k-20m status=skipped reason=dynamic_rpc_capability_unavailable\n'
    fi
fi

printf 'RBT_RPC_BENCH_DONE run_id=%s log_dir=%s\n' "$run_id" "$run_dir"


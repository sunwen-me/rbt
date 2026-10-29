#!/usr/bin/env bash
set -euo pipefail

# Exercise the RPC data plane and its Composer lifecycle for long enough to
# expose ownership, descriptor-reuse, task, FD, thread, and memory leaks. The
# default run is the v0.6 acceptance soak; --smoke preserves every class of
# check with a shorter duration while retaining the ten healthy Composer
# restart requirement.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

die() {
    printf 'RBT_RPC_SOAK FAIL harness=%s logs=%s\n' "$*" "${run_dir:-unavailable}" >&2
    exit 1
}

usage() {
    cat <<'EOF'
usage: scripts/soak_rpc.sh [--smoke] [--duration-seconds N]

Runs mixed Rust/C++ traffic, Python asyncio deadline/cancellation/reuse
cycles, provider replacement, and healthy Composer recovery. The default
duration is RBT_RPC_SOAK_SECONDS or 300 seconds.
EOF
}

duration_seconds="${RBT_RPC_SOAK_SECONDS:-300}"
composer_restarts="${RBT_RPC_SOAK_COMPOSER_RESTARTS:-10}"
provider_replacements="${RBT_RPC_SOAK_PROVIDER_REPLACEMENTS:-3}"
while (($# > 0)); do
    case "$1" in
        --smoke)
            duration_seconds="${RBT_RPC_SOAK_SMOKE_SECONDS:-15}"
            # Smoke is short, but it still exercises every class at least
            # once and performs all ten healthy Composer reconstructions.
            composer_restarts=10
            provider_replacements=1
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
        *) die "unknown option: $1" ;;
    esac
done
[[ "${duration_seconds}" =~ ^[1-9][0-9]*$ ]] || die "duration must be a positive integer"
[[ "${composer_restarts}" =~ ^[1-9][0-9]*$ ]] \
    || die "Composer restart count must be a positive integer"
[[ "${provider_replacements}" =~ ^[1-9][0-9]*$ ]] \
    || die "provider replacement count must be a positive integer"
((composer_restarts >= 10)) || die "at least ten healthy Composer restarts are required"

for command_name in awk cargo cmake date find jq mktemp rg sleep; do
    command -v "${command_name}" >/dev/null 2>&1 \
        || die "required command not found: ${command_name}"
done

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
toolchain_bin="${toolchain_root}/bin"
cpp_prefix="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"
"${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${cpp_prefix}"
cpp_build="${RBT_RPC_SOAK_CPP_BUILD:-${repo_root}/target/rpc-interop-cpp}"
python_bin="${RBT_RPC_SOAK_PYTHON:-${repo_root}/target/python-verify-venv/bin/python}"
if [[ -z "${RBT_RPC_SOAK_PYTHON:-}" && ! -x "${python_bin}" ]]; then
    if [[ -x "/home/sw/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3" ]]; then
        python_bin="/home/sw/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3"
    else
        python_bin="$(command -v python3 || true)"
    fi
fi
composer_port="${RBT_RPC_SOAK_PORT:-17663}"
composer_addr="127.0.0.1:${composer_port}"
wait_seconds="${RBT_RPC_SOAK_STEP_TIMEOUT_SECONDS:-25}"

[[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-gcc" ]] \
    || die "Phase4 C compiler is missing"
[[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-g++" ]] \
    || die "Phase4 C++ compiler is missing"
[[ -f "${cpp_prefix}/lib/cmake/iceoryx2-cxx/iceoryx2-cxxConfig.cmake" ]] \
    || die "iceoryx2 C++ package is missing"
[[ -x "${python_bin}" ]] || die "Python verification interpreter is missing: ${python_bin}"
[[ "${composer_port}" =~ ^[0-9]+$ ]] || die "RBT_RPC_SOAK_PORT must be numeric"

export PATH="${toolchain_bin}:${PATH}"
export CC="${toolchain_bin}/x86_64-conda-linux-gnu-gcc"
export CXX="${toolchain_bin}/x86_64-conda-linux-gnu-g++"
export AR="${toolchain_bin}/x86_64-conda-linux-gnu-ar"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
export CMAKE_PREFIX_PATH="${cpp_prefix}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"
export LD_LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export PYTHONPATH="${repo_root}/python${PYTHONPATH:+:${PYTHONPATH}}"

if [[ "${RBT_SKIP_BUILD:-0}" != 1 ]]; then
    cargo build --locked -p composer -p rbt-cli -p rpc-onboarding -p rbt-rpc-interop
    cmake -S sdk/cpp -B "${cpp_build}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_CXX_COMPILER="${CXX}" \
        -DCMAKE_PREFIX_PATH="${cpp_prefix}"
    cmake --build "${cpp_build}" --target rpc_interop --parallel 2
fi

composer="${RBT_RPC_SOAK_COMPOSER_BINARY:-${repo_root}/target/debug/composer}"
rbt_cli="${RBT_RPC_SOAK_CLI_BINARY:-${repo_root}/target/debug/rbt}"
component="${RBT_RPC_SOAK_COMPONENT_BINARY:-${repo_root}/target/debug/rpc-onboarding}"
rust_interop="${RBT_RPC_SOAK_RUST_BINARY:-${repo_root}/target/debug/rbt-rpc-interop}"
cpp_interop="${RBT_RPC_SOAK_CPP_BINARY:-${cpp_build}/rpc_interop}"
python_probe="${repo_root}/python/tests/rpc_soak_probe.py"
for required in "${composer}" "${rbt_cli}" "${component}" "${rust_interop}" "${cpp_interop}"; do
    [[ -x "${required}" ]] || die "required executable is missing: ${required}"
done
[[ -f "${python_probe}" ]] || die "required Python probe is missing: ${python_probe}"
"${python_bin}" -c \
    'import rbt; assert hasattr(rbt._rbt, "_RpcAsyncCall"); assert hasattr(rbt.RpcClient, "call_async")' \
    || die "fresh native Python dynamic/async RPC extension is not installed"

log_root="${RBT_RPC_SOAK_LOG_DIR:-${repo_root}/target/rpc-soak-logs}"
mkdir -p "${log_root}"
run_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="${log_root}/${run_id}"
mkdir -p "${run_dir}"
resource_log="${run_dir}/resources.tsv"
printf 'checkpoint\trole\tpid\trss_kib\tfds\tthreads\n' >"${resource_log}"
resource_summary="${run_dir}/resources-summary.tsv"
printf 'role\tinitial_rss_kib\tmax_rss_kib\tfinal_rss_kib\tinitial_fds\tmax_fds\tfinal_fds\tinitial_threads\tmax_threads\tfinal_threads\n' \
    >"${resource_summary}"

declare -a child_pids=()
last_pid=''
composer_pid=''
provider_pid=''
client_pid=''
python_pid=''
interop_server_pid=''
interop_client_pid=''
composer_label='composer-0'
provider_label='provider-0'

cleanup() {
    local pid deadline
    for pid in "${child_pids[@]:-}"; do
        [[ -n "${pid}" ]] || continue
        if kill -0 "${pid}" 2>/dev/null; then
            kill -TERM "${pid}" 2>/dev/null || true
        fi
    done
    deadline=$((SECONDS + 2))
    while ((SECONDS < deadline)); do
        local live=0
        for pid in "${child_pids[@]:-}"; do
            [[ -n "${pid}" ]] || continue
            if kill -0 "${pid}" 2>/dev/null; then
                live=1
                break
            fi
        done
        ((live == 0)) && break
        sleep 0.05
    done
    for pid in "${child_pids[@]:-}"; do
        [[ -n "${pid}" ]] || continue
        if kill -0 "${pid}" 2>/dev/null; then
            kill -KILL "${pid}" 2>/dev/null || true
        fi
        wait "${pid}" 2>/dev/null || true
    done
}
trap cleanup EXIT
trap 'exit 130' INT TERM

start_process() {
    local label="$1"
    shift
    "$@" >"${run_dir}/${label}.log" 2>&1 &
    last_pid="$!"
    child_pids+=("${last_pid}")
}

forget_pid() {
    local forgotten="$1"
    local -a remaining=()
    local pid
    for pid in "${child_pids[@]:-}"; do
        [[ "${pid}" == "${forgotten}" ]] || remaining+=("${pid}")
    done
    child_pids=("${remaining[@]}")
}

kill_and_wait() {
    local signal="$1"
    local pid="$2"
    local deadline
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
        kill "-${signal}" "${pid}" 2>/dev/null || true
    fi
    if [[ -n "${pid}" ]]; then
        deadline=$((SECONDS + wait_seconds))
        while kill -0 "${pid}" 2>/dev/null && ((SECONDS < deadline)); do
            sleep 0.05
        done
        if kill -0 "${pid}" 2>/dev/null; then
            kill -KILL "${pid}" 2>/dev/null || true
        fi
        wait "${pid}" 2>/dev/null || true
        forget_pid "${pid}"
    fi
}

wait_log() {
    local file="$1"
    local marker="$2"
    local pid="$3"
    local description="$4"
    local deadline=$((SECONDS + wait_seconds))
    while ((SECONDS < deadline)); do
        if rg -Fq "${marker}" "${file}" 2>/dev/null; then
            return 0
        fi
        record_resources "watch"
        if ! kill -0 "${pid}" 2>/dev/null; then
            cat "${file}" >&2 || true
            die "${description} exited before marker ${marker}"
        fi
        sleep 0.05
    done
    cat "${file}" >&2 || true
    die "timed out waiting for ${description}: ${marker}"
}

inspect_ps() {
    local output="$1"
    RBT_COMPOSER_ADDR="${composer_addr}" "${rbt_cli}" ps --json >"${output}" 2>/dev/null
}

inspect_graph() {
    local output="$1"
    RBT_COMPOSER_ADDR="${composer_addr}" "${rbt_cli}" graph --json >"${output}" 2>/dev/null
}

inspect_rpc() {
    local output="$1"
    RBT_COMPOSER_ADDR="${composer_addr}" "${rbt_cli}" rpc info EchoRpc --json \
        >"${output}" 2>/dev/null
}

wait_query() {
    local description="$1"
    local query="$2"
    local deadline=$((SECONDS + wait_seconds))
    local current="${run_dir}/current.json"
    while ((SECONDS < deadline)); do
        if inspect_ps "${current}" && jq -e "${query}" "${current}" >/dev/null 2>&1; then
            return 0
        fi
        record_resources "watch"
        sleep 0.1
    done
    [[ -s "${current}" ]] && jq . "${current}" >&2 || true
    die "timed out waiting for ${description}"
}

wait_active_graph() {
    wait_query 'active RPC provider/client graph' '
        .phase == "running" and
        ([.components[] | select(
            (.component_id == "rpc-provider" or .component_id == "rpc-client") and
            .state == "ACTIVE"
        )] | length == 2)'
}

assert_invariants() {
    local label="$1"
    local ps_file="${run_dir}/${label}.ps.json"
    local graph_file="${run_dir}/${label}.graph.json"
    local rpc_file="${run_dir}/${label}.rpc.json"
    inspect_ps "${ps_file}" || die "ps failed at ${label}"
    inspect_graph "${graph_file}" || die "graph failed at ${label}"
    inspect_rpc "${rpc_file}" || die "rpc info failed at ${label}"
    jq -e '
        .phase == "running" and
        (.process_conflicts | length == 0) and
        ([.components[].component_id] | length) ==
            ([.components[].component_id] | unique | length) and
        ([.components[] | select(.state == "FAILED" or .failure_reason != null)] | length == 0) and
        ([.components[] | select(.pending != null)] | length == 0)
    ' "${ps_file}" >/dev/null || die "process invariant failed at ${label}"
    jq -e '
        ([.nodes[] | select(.kind == "capability" and .status == "active" and .resolved == true)] | length == 1) and
        ([.nodes[] | select(.kind == "requirement" and .resolved == true)] | length == 1) and
        ([.edges[] | select(.kind == "provides" and .resolved == true)] | length == 1) and
        ([.edges[] | select(.kind == "binds" and .resolved == true)] | length == 1)
    ' "${graph_file}" >/dev/null || die "RPC graph invariant failed at ${label}"
    jq -e '
        . as $root |
        all($root.nodes[] | select(.kind == "capability" and .status == "active" and .resolved == true);
            . as $cap |
            any($root.nodes[];
                .kind == "component" and
                .component_id == $cap.provider_component and
                .state == "active" and
                .provider_process == $cap.provider_process and
                .activation == $cap.activation
            ) and
            any($root.edges[];
                .kind == "provides" and .resolved == true and
                .source == ("component:" + $cap.provider_component) and
                .target == ("capability:" + $cap.capability_instance)
            )) and
        all($root.nodes[] | select(.kind == "requirement" and .resolved == true);
            . as $requirement |
            $requirement.capability_instance != null and
            $requirement.provider_component != null and
            any($root.nodes[];
                .kind == "capability" and .status == "active" and .resolved == true and
                .capability_instance == $requirement.capability_instance and
                .provider_component == $requirement.provider_component
            ) and
            any($root.edges[];
                .kind == "binds" and .resolved == true and
                .source == $requirement.id and
                .target == ("capability:" + $requirement.capability_instance)
            ))
    ' "${graph_file}" >/dev/null || die "stale RPC capability or binding at ${label}"
    jq -e '
        .transport_kind == "rpc" and
        .capability_type == "EchoRpc" and
        .physical_endpoint == ("rbt/cap/" + .capability_instance + "/rpc") and
        .provider_component == "rpc-provider" and
        .request_type.header_size == 16 and .response_type.header_size == 16 and
        .readiness == "rpc-sidecar-v1"
    ' "${rpc_file}" >/dev/null || die "RPC inspection invariant failed at ${label}"
}

normalise_identity() {
    local ps_file="$1"
    local rpc_file="$2"
    jq -S --slurpfile rpc "${rpc_file}" '
        [
            .components[] |
            select(.component_id == "rpc-provider" or .component_id == "rpc-client") |
            {component_id, process_instance_id, activation_id, state}
        ] as $components |
        {components: $components, rpc: {
            capability_instance: $rpc[0].capability_instance,
            provider_process: $rpc[0].provider_process,
            activation: $rpc[0].activation,
            physical_endpoint: $rpc[0].physical_endpoint
        }}
    ' "${ps_file}"
}

record_resource() {
    local checkpoint="$1"
    local role="$2"
    local pid="$3"
    [[ -n "${pid}" && -r "/proc/${pid}/status" ]] || return 0
    local rss fds threads
    if ! rss="$(awk '/^VmRSS:/ {print $2}' "/proc/${pid}/status" 2>/dev/null)"; then
        return 0
    fi
    if ! fds="$(find "/proc/${pid}/fd" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l 2>/dev/null)"; then
        return 0
    fi
    if ! threads="$(find "/proc/${pid}/task" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l 2>/dev/null)"; then
        return 0
    fi
    [[ -n "${rss}" && "${fds}" -gt 0 && "${threads}" -gt 0 ]] || return 0
    printf '%s\t%s\t%s\t%s\t%s\t%s\n' \
        "${checkpoint}" "${role}" "${pid}" "${rss}" "${fds}" "${threads}" \
        >>"${resource_log}"
}

record_resources() {
    local checkpoint="$1"
    record_resource "${checkpoint}" composer "${composer_pid}"
    record_resource "${checkpoint}" provider "${provider_pid}"
    record_resource "${checkpoint}" client "${client_pid}"
    record_resource "${checkpoint}" python "${python_pid}"
    record_resource "${checkpoint}" cpp-interop "${interop_server_pid}"
    record_resource "${checkpoint}" rust-interop "${interop_client_pid}"
}

wait_checked() {
    local pid="$1"
    local label="$2"
    local timeout_seconds="$3"
    local deadline=$((SECONDS + timeout_seconds))
    while kill -0 "${pid}" 2>/dev/null; do
        if ((SECONDS >= deadline)); then
            die "${label} did not finish within ${timeout_seconds}s"
        fi
        sleep 0.1
    done
    set +e
    wait "${pid}"
    local status=$?
    set -e
    ((status == 0)) || {
        cat "${run_dir}/${label}.log" >&2 || true
        die "${label} exited with status ${status}"
    }
    forget_pid "${pid}"
}

monotonic_ms() {
    "${python_bin}" -c 'import time; print(time.monotonic_ns() // 1_000_000)'
}

client_call_marker() {
    # The onboarding client emits a structured sequence marker for every
    # completed call.  Keep the legacy tracing phrase as a compatibility
    # fallback for binaries built before the explicit marker was added.
    rg -c --no-filename \
        'RPC client received zero-copy response|RBT_RPC_CLIENT_(CALL|RESPONSE)|event=(call_success|rpc_call|response)|call_count=[0-9]+|rpc_call_count=[0-9]+|role=rpc-client[^[:space:]]*.*sequence=' \
        "${run_dir}/client.log" 2>/dev/null || true
}

wait_client_call_progress() {
    local before="$1"
    local description="$2"
    local deadline=$((SECONDS + wait_seconds))
    local after
    while ((SECONDS < deadline)); do
        after="$(client_call_marker)"
        [[ "${after}" =~ ^[0-9]+$ ]] || after=0
        if ((after > before)); then
            printf '%s\n' "${after}"
            return 0
        fi
        record_resources "watch"
        sleep 0.05
    done
    die "client call marker did not progress during ${description} (before=${before})"
}

client_prepare_count() {
    local value
    value="$(rg -o --no-filename 'prepare_count=[0-9]+' "${run_dir}/client.log" \
        2>/dev/null | tail -n 1 | cut -d= -f2 || true)"
    printf '%s\n' "${value:-0}"
}

client_activation_count() {
    local value
    value="$(rg -o --no-filename 'activation_count=[0-9]+' "${run_dir}/client.log" \
        2>/dev/null | tail -n 1 | cut -d= -f2 || true)"
    printf '%s\n' "${value:-0}"
}

assert_component_identity() {
    local before_ps="$1"
    local after_ps="$2"
    jq -e --slurpfile before "${before_ps}" '
        ([.components[] | select(.component_id == "rpc-client")][0]) as $after |
        ($before[0].components[] | select(.component_id == "rpc-client")) as $old |
        $after.process_instance_id == $old.process_instance_id
    ' "${after_ps}" >/dev/null \
        || die "persistent client process instance changed"
}

assert_final_rpc_idle() {
    local output="$1"
    jq -e '
        .metrics_status == "available" and
        (.server_pressure != null) and
        (.server_pressure_freshness != null) and
        (.server_pressure_freshness.fresh == true) and
        (.server_pressure.server != null) and
        (.server_pressure.server.requests_current == 0) and
        (.client_observations | length > 0) and
        all(.client_observations[];
            .fresh == true and
            .pressure.client != null and
            .pressure.client.in_flight_current == 0 and
            .pressure.client.response_guards_current == 0
        )
    ' "${output}" >/dev/null \
        || return 1
}

printf 'RBT_RPC_SOAK RUN duration_seconds=%s composer_restarts=%s provider_replacements=%s address=%s logs=%s\n' \
    "${duration_seconds}" "${composer_restarts}" "${provider_replacements}" \
    "${composer_addr}" "${run_dir}"

start_process "${composer_label}" env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info "${composer}"
composer_pid="${last_pid}"
wait_query 'Composer inspection endpoint' '.phase == "recovering" or .phase == "running"'
start_process "${provider_label}" env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
    "${component}" provider
provider_pid="${last_pid}"
start_process client env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
    "${component}" client
client_pid="${last_pid}"
wait_active_graph
wait_log "${run_dir}/${provider_label}.log" 'event=activate' "${provider_pid}" provider
wait_log "${run_dir}/client.log" 'event=activate' "${client_pid}" client
assert_invariants initial
[[ "$(client_prepare_count)" == 1 ]] || die "client did not report prepare_count=1 at startup"
[[ "$(client_activation_count)" == 1 ]] || die "client did not report activation_count=1 at startup"

interop_calls=$((duration_seconds * 50 + 200))
((interop_calls <= 1000000)) || interop_calls=1000000
# Spread the long-lived lane over ~95% of the soak.  A fixed per-call sleep
# made its length depend on per-call cost (~3 ms in debug builds), so it could
# outlive the soak and miss the finish window.  Slow hosts run behind schedule
# without extra sleeps; the 90% liveness check below still applies.
interop_target_ms=$((duration_seconds * 950))
interop_service="rbt/rpc/soak/${run_id}/fixed"
# Keep the long-lived lane in the requested direction: a Rust client drives a
# C++ server. The pace leaves the processes alive for most of the soak while
# still providing thousands of checksum-validated calls in smoke mode.
start_process interop-server env RBT_RPC_INTEROP_QUIET=1 \
    "${cpp_interop}" server "${interop_service}" "${interop_calls}"
interop_server_pid="${last_pid}"
wait_log "${run_dir}/interop-server.log" 'RBT_RPC_INTEROP READY' \
    "${interop_server_pid}" 'C++ interop server'
start_process interop-client env RBT_RPC_INTEROP_QUIET=1 \
    RBT_RPC_INTEROP_TARGET_MS="${interop_target_ms}" \
    "${rust_interop}" client "${interop_service}" "${interop_calls}"
interop_client_pid="${last_pid}"
python_duration=$((duration_seconds + 2))
start_process python env RBT_RPC_SOAK_SECONDS="${python_duration}" \
    RBT_RPC_SOAK_SERVICE="rbt/rpc/soak/${run_id}/python" \
    "${python_bin}" "${python_probe}" --duration-seconds "${python_duration}"
python_pid="${last_pid}"
sleep 1
for running in "${interop_server_pid}" "${interop_client_pid}" "${python_pid}"; do
    kill -0 "${running}" 2>/dev/null || die "data-plane soak process exited during startup"
done
record_resources baseline

current_capability="$(jq -r .capability_instance "${run_dir}/initial.rpc.json")"
current_endpoint="$(jq -r .physical_endpoint "${run_dir}/initial.rpc.json")"
current_provider_process="$(jq -r .provider_process "${run_dir}/initial.rpc.json")"

soak_started_seconds="${SECONDS}"
restart_interval=$((duration_seconds / (composer_restarts + 1)))
((restart_interval > 0)) || restart_interval=1
replacement_interval=$((duration_seconds / (provider_replacements + 1)))
((replacement_interval > 0)) || replacement_interval=1
next_restart_at="${restart_interval}"
next_replacement_at="${replacement_interval}"
restart=0
replacement=0

healthy_composer_restart() {
    local number="$1"
    local before_calls down_calls after_calls before_prepare before_activation
    before_calls="$(client_call_marker)"
    [[ "${before_calls}" =~ ^[0-9]+$ ]] || before_calls=0
    ((before_calls > 0)) \
        || die "client log has no machine-readable call marker before Composer restart ${number}"
    before_prepare="$(client_prepare_count)"
    before_activation="$(client_activation_count)"
    inspect_ps "${run_dir}/restart-${number}-before.ps.json" \
        || die "ps failed before Composer restart ${number}"
    inspect_rpc "${run_dir}/restart-${number}-before.rpc.json" \
        || die "rpc info failed before Composer restart ${number}"
    normalise_identity "${run_dir}/restart-${number}-before.ps.json" \
        "${run_dir}/restart-${number}-before.rpc.json" \
        >"${run_dir}/restart-${number}-before.identity.json"

    kill_and_wait KILL "${composer_pid}"
    # Provider and client data plane work is independent of Composer. Prove a
    # real response while the control plane is down before reconnecting it.
    down_calls="$(client_call_marker)"
    [[ "${down_calls}" =~ ^[0-9]+$ ]] || down_calls=0
    after_calls="$(wait_client_call_progress "${down_calls}" \
        "Composer-only outage restart ${number}")"
    composer_label="composer-${number}"
    start_process "${composer_label}" env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
        "${composer}"
    composer_pid="${last_pid}"
    wait_active_graph
    assert_invariants "restart-${number}-after"
    normalise_identity "${run_dir}/restart-${number}-after.ps.json" \
        "${run_dir}/restart-${number}-after.rpc.json" \
        >"${run_dir}/restart-${number}-after.identity.json"
    cmp -s "${run_dir}/restart-${number}-before.identity.json" \
        "${run_dir}/restart-${number}-after.identity.json" \
        || die "healthy Composer recovery changed runtime identity at restart ${number}"
    assert_component_identity "${run_dir}/restart-${number}-before.ps.json" \
        "${run_dir}/restart-${number}-after.ps.json"
    rg -Fq 'reconciliation: 0 deactivate, 0 activate' \
        "${run_dir}/${composer_label}.log" \
        || die "Composer restart ${number} was not 0 deactivate/0 activate"
    ! rg -Fq 'sending Activate' "${run_dir}/${composer_label}.log" \
        || die "healthy Composer restart ${number} sent Activate"
    ! rg -Fq 'sending Deactivate' "${run_dir}/${composer_label}.log" \
        || die "healthy Composer restart ${number} sent Deactivate"
    [[ "$(client_prepare_count)" == "${before_prepare}" ]] \
        || die "client prepare count changed during healthy Composer restart ${number}"
    [[ "$(client_activation_count)" == "${before_activation}" ]] \
        || die "client activation count changed during healthy Composer restart ${number}"
    record_resources "composer-restart-${number}"
    printf 'RBT_RPC_SOAK composer_restart=%s deactivate=0 activate=0 identity=preserved data_plane_traffic_while_down=true calls_before=%s calls_after=%s\n' \
        "${number}" "${before_calls}" "${after_calls}"
}

provider_replacement() {
    local number="$1"
    local combined_outage="$2"
    local old_capability old_endpoint old_provider_pid old_provider_process
    local before_calls blocked_calls first_success_calls blocked_started blocked_ms replacement_started first_success_ms
    local expected_client_activations new_capability new_endpoint new_provider_process
    old_capability="${current_capability}"
    old_endpoint="${current_endpoint}"
    old_provider_process="${current_provider_process}"
    old_provider_pid="${provider_pid}"
    before_calls="$(client_call_marker)"
    [[ "${before_calls}" =~ ^[0-9]+$ ]] || before_calls=0
    inspect_ps "${run_dir}/replacement-${number}-before.ps.json" \
        || die "ps failed before provider replacement ${number}"
    blocked_started="$(monotonic_ms)"
    kill_and_wait KILL "${provider_pid}"
    if [[ "${combined_outage}" == 1 ]]; then
        kill_and_wait KILL "${composer_pid}"
        composer_label="composer-outage-${number}"
        start_process "${composer_label}" env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
            "${composer}"
        composer_pid="${last_pid}"
        wait_query 'client BLOCKED after Composer and provider outage' '
            .phase == "running" and
            ([.components[] | select(.component_id == "rpc-client" and .state == "BLOCKED")] | length == 1)'
    else
        wait_query 'client blocked after provider loss' '
            [.components[] | select(.component_id == "rpc-client" and .state == "BLOCKED")] | length == 1'
    fi
    blocked_ms=$(( $(monotonic_ms) - blocked_started ))
    blocked_calls="$(client_call_marker)"
    [[ "${blocked_calls}" =~ ^[0-9]+$ ]] || blocked_calls=0

    provider_label="provider-${number}"
    replacement_started="$(monotonic_ms)"
    start_process "${provider_label}" env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info \
        "${component}" provider
    provider_pid="${last_pid}"
    wait_active_graph
    wait_log "${run_dir}/${provider_label}.log" 'event=activate' "${provider_pid}" provider
    expected_client_activations=$((number + 1))
    wait_log "${run_dir}/client.log" \
        "activation_count=${expected_client_activations}" "${client_pid}" client
    first_success_calls="$(wait_client_call_progress "${blocked_calls}" \
        "provider replacement ${number}")"
    first_success_ms=$(( $(monotonic_ms) - replacement_started ))
    assert_invariants "replacement-${number}"
    new_capability="$(jq -r .capability_instance "${run_dir}/replacement-${number}.rpc.json")"
    new_endpoint="$(jq -r .physical_endpoint "${run_dir}/replacement-${number}.rpc.json")"
    new_provider_process="$(jq -r .provider_process "${run_dir}/replacement-${number}.rpc.json")"
    [[ "${new_capability}" != "${old_capability}" ]] \
        || die "provider replacement ${number} reused capability instance"
    [[ "${new_endpoint}" != "${old_endpoint}" ]] \
        || die "provider replacement ${number} reused physical RPC endpoint"
    [[ "${new_provider_process}" != "${old_provider_process}" ]] \
        || die "provider replacement ${number} reused provider process identity"
    current_capability="${new_capability}"
    current_endpoint="${new_endpoint}"
    current_provider_process="${new_provider_process}"
    [[ ! -e "/proc/${old_provider_pid}" ]] \
        || die "old provider process ${old_provider_pid} remained after replacement"
    assert_component_identity "${run_dir}/replacement-${number}-before.ps.json" \
        "${run_dir}/replacement-${number}.ps.json"
    [[ "$(client_prepare_count)" == 1 ]] \
        || die "persistent client prepared more than once"
    recovery_ms=$(( $(monotonic_ms) - blocked_started ))
    record_resources "provider-replacement-${number}"
    printf 'RBT_RPC_SOAK provider_replacement=%s combined_outage=%s old_capability=%s new_capability=%s blocked_ms=%s first_success_ms=%s recovery_ms=%s calls_before=%s calls_blocked=%s calls_first_success=%s client_prepare_count=1 client_activation_count=%s\n' \
        "${number}" "${combined_outage}" "${old_capability}" "${new_capability}" \
        "${blocked_ms}" "${first_success_ms}" "${recovery_ms}" \
        "${before_calls}" "${blocked_calls}" "${first_success_calls}" \
        "${expected_client_activations}"
}

while ((SECONDS - soak_started_seconds < duration_seconds || restart < composer_restarts || replacement < provider_replacements)); do
    elapsed=$((SECONDS - soak_started_seconds))
    if ((restart < composer_restarts && elapsed >= next_restart_at)); then
        restart=$((restart + 1))
        healthy_composer_restart "${restart}"
        next_restart_at=$((restart_interval * (restart + 1)))
        continue
    fi
    if ((replacement < provider_replacements && elapsed >= next_replacement_at)); then
        replacement=$((replacement + 1))
        if ((replacement == 1)); then
            provider_replacement "${replacement}" 1
        else
            provider_replacement "${replacement}" 0
        fi
        next_replacement_at=$((replacement_interval * (replacement + 1)))
        continue
    fi
    record_resources "steady-${elapsed}"
    kill -0 "${client_pid}" 2>/dev/null || die "persistent Composer client exited"
    kill -0 "${provider_pid}" 2>/dev/null || die "current provider exited"
    if ((elapsed * 10 < duration_seconds * 9)); then
        kill -0 "${interop_client_pid}" 2>/dev/null \
            || die "Rust interop client exited before most of the soak interval"
        kill -0 "${interop_server_pid}" 2>/dev/null \
            || die "C++ interop server exited before most of the soak interval"
    fi
    sleep 1
done
((restart == composer_restarts)) \
    || die "only ${restart}/${composer_restarts} healthy Composer restarts completed"
((replacement == provider_replacements)) \
    || die "only ${replacement}/${provider_replacements} provider replacements completed"

wait_checked "${python_pid}" python "$((wait_seconds + 5))"
wait_checked "${interop_client_pid}" interop-client "$((wait_seconds + 15))"
wait_checked "${interop_server_pid}" interop-server "$((wait_seconds + 5))"
rg -Fq 'RBT_RPC_SOAK_PYTHON PASS' "${run_dir}/python.log" \
    || die "Python asyncio probe did not report PASS"
rg -q 'responses=[1-9][0-9]* .*deadlines=[1-9][0-9]* .*cancellations=[1-9][0-9]* .*pending=0 .*client_registrations=0 .*server_registrations=0 .*warnings=0' \
    "${run_dir}/python.log" \
    || die "Python probe did not prove responses, deadlines, cancellation, registrations, and warning cleanup"
awk '
    BEGIN {
        required[1] = "client_registrations"
        required[2] = "server_registrations"
        required[3] = "in_flight_current"
        required[4] = "response_guards_current"
        required[5] = "requests_current"
    }
    /^RBT_RPC_SOAK_PYTHON PROGRESS / {
        progress++
        for (field_idx = 1; field_idx <= 5; field_idx++) {
            delete fields[required[field_idx]]
        }
        for (field_idx = 3; field_idx <= NF; field_idx++) {
            if (split($field_idx, pair, "=") != 2) {
                failed = 1
                exit 1
            }
            fields[pair[1]] = pair[2]
        }
        for (field_idx = 1; field_idx <= 5; field_idx++) {
            key = required[field_idx]
            if (!(key in fields) || fields[key] !~ /^[0-9]+$/ || fields[key] + 0 != 0) {
                failed = 1
                exit 1
            }
        }
    }
    END {
        if (progress == 0) failed = 1
        if (failed != 0) exit 1
    }
' "${run_dir}/python.log" \
    || die "Python probe did not report idle RPC metrics for every cycle"
rg -q 'RBT_RPC_SOAK_PYTHON PASS .*client_registrations=0 .*server_registrations=0 .*in_flight_current=0 .*response_guards_current=0 .*requests_current=0 .*rss_initial_kib=' \
    "${run_dir}/python.log" \
    || die "Python probe did not report final pre-close idle RPC metrics"
rg -q 'threads_initial=[0-9]+ .*threads_final=[0-9]+' "${run_dir}/python.log" \
    || die "Python probe did not report native thread cleanup evidence"
rg -Fq 'RBT_RPC_INTEROP PASS' "${run_dir}/interop-client.log" \
    || die "C++ interop client did not report PASS"
rg -Fq 'RBT_RPC_INTEROP PASS' "${run_dir}/interop-server.log" \
    || die "C++ interop server did not report PASS"
"${python_bin}" "${repo_root}/scripts/check_rpc_interop_idle.py" \
    --log "${run_dir}/interop-client.log" --language rust --role client \
    --service "${interop_service}" --calls "${interop_calls}" \
    || die "Rust interop client did not report valid final idle metrics"
"${python_bin}" "${repo_root}/scripts/check_rpc_interop_idle.py" \
    --log "${run_dir}/interop-server.log" --language cpp --role server \
    --service "${interop_service}" --calls "${interop_calls}" \
    || die "C++ interop server did not report valid final idle metrics"

idle_deadline=$((SECONDS + wait_seconds))
while true; do
    if inspect_rpc "${run_dir}/final.rpc.json" \
        && assert_final_rpc_idle "${run_dir}/final.rpc.json" 2>/dev/null; then
        break
    fi
    ((SECONDS < idle_deadline)) || die "RPC current ownership gauges did not drain"
    sleep 0.1
done
assert_invariants final
inspect_rpc "${run_dir}/final.rpc.json" \
    || die "final RPC inspection failed after topology assertion"
assert_final_rpc_idle "${run_dir}/final.rpc.json" \
    || die "final RPC metrics were not fresh, non-empty, and idle"
record_resources final

if rg -ni 'panicked|task was destroyed|unclosed.*(task|transport)|RBT_RPC_(INTEROP|SOAK) FAIL' \
    "${run_dir}"/*.log >"${run_dir}/unexpected.log" 2>/dev/null; then
    cat "${run_dir}/unexpected.log" >&2
    die "unexpected process failure or asyncio shutdown warning"
fi

resource_growth_violations="$(awk -v summary="${resource_summary}" -F '\t' '
    NR == 1 { next }
    {
        # Replacement provider PIDs are one logical role. Aggregate them so
        # the bound is checked across the complete provider lifetime rather
        # than resetting the baseline for every new PID.
        key = $2
        if (!(key in first_rss)) {
            first_rss[key] = $4; first_fd[key] = $5; first_threads[key] = $6
        }
        if ($4 > max_rss[key]) max_rss[key] = $4
        if ($5 > max_fd[key]) max_fd[key] = $5
        if ($6 > max_threads[key]) max_threads[key] = $6
        last_rss[key] = $4; last_fd[key] = $5; last_threads[key] = $6
    }
    END {
        violations = 0
        for (key in first_rss) {
            if (max_rss[key] - first_rss[key] > 65536 ||
                max_fd[key] - first_fd[key] > 64 ||
                max_threads[key] - first_threads[key] > 32) {
                print key, first_rss[key], max_rss[key], first_fd[key], max_fd[key],
                    first_threads[key], max_threads[key] > "/dev/stderr"
                violations++
            }
            printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", \
                key, first_rss[key], max_rss[key], last_rss[key], first_fd[key], \
                max_fd[key], last_fd[key], first_threads[key], max_threads[key], \
                last_threads[key] >> summary
        }
        print violations
    }
' "${resource_log}")"
((resource_growth_violations == 0)) \
    || die "resource growth exceeded +64 MiB/+64 FD/+32 threads"

kill_and_wait TERM "${client_pid}"
kill_and_wait TERM "${provider_pid}"
kill_and_wait TERM "${composer_pid}"
for pid in "${client_pid}" "${provider_pid}" "${composer_pid}" \
    "${python_pid}" "${interop_client_pid}" "${interop_server_pid}"; do
    [[ ! -e "/proc/${pid}" ]] || die "tracked child ${pid} was not reaped"
done
(( ${#child_pids[@]} == 0 )) || die "owned child PID registry was not fully reaped"

printf 'RBT_RPC_SOAK RESULT duration_seconds=%s composer_restarts=%s provider_replacements=%s python_pending=0 rpc_in_flight_current=0 rpc_response_guards_current=0 rpc_requests_current=0 resource_growth_violations=0 orphan_processes=0\n' \
    "${duration_seconds}" "${composer_restarts}" "${provider_replacements}"
printf 'RBT_RPC_SOAK PASS duration_seconds=%s composer_restarts=%s provider_replacements=%s\n' \
    "${duration_seconds}" "${composer_restarts}" "${provider_replacements}" \
    >"${run_dir}/PASS"
printf 'RBT_RPC_SOAK PASS logs=%s resources=%s resource_summary=%s\n' \
    "${run_dir}" "${resource_log}" "${resource_summary}"

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
usage: scripts/soak_v01.sh [--smoke] [--duration-seconds N] [--cycles N]

Runs a deterministic fault/recovery sequence. The default duration is 300
seconds; --smoke runs one complete cycle. RBT_SOAK_PORT and RBT_SOAK_LOG_DIR
select the control port and retained evidence directory.
EOF
}

duration_seconds="${RBT_SOAK_DURATION_SECONDS:-300}"
max_cycles=0
while (($# > 0)); do
    case "$1" in
        --smoke)
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
[[ "${duration_seconds}" =~ ^[1-9][0-9]*$ ]] || die "duration must be positive"
[[ "${max_cycles}" =~ ^[0-9]+$ ]] || die "cycles must be non-negative"

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
cpp_prefix="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"
"${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${cpp_prefix}"
cpp_build_dir="${RBT_V01_CPP_BUILD_DIR:-${repo_root}/target/v01-cpp-debug}"
composer_port="${RBT_SOAK_PORT:-17649}"
composer_addr="127.0.0.1:${composer_port}"
wait_seconds="${RBT_SOAK_STEP_TIMEOUT_SECONDS:-20}"

[[ "${composer_port}" =~ ^[0-9]+$ ]] || die "RBT_SOAK_PORT must be numeric"
[[ -x "${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc" ]] || die "Phase4 compiler toolchain is missing"
[[ -f "${cpp_prefix}/lib/cmake/iceoryx2-cxx/iceoryx2-cxxConfig.cmake" ]] || die "iceoryx2 C++ package is missing"
command -v cargo >/dev/null 2>&1 || die "cargo is required"
command -v cmake >/dev/null 2>&1 || die "cmake is required"
command -v jq >/dev/null 2>&1 || die "jq is required"

export PATH="${toolchain_root}/bin${PATH:+:${PATH}}"
export CC="${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc"
export CXX="${toolchain_root}/bin/x86_64-conda-linux-gnu-g++"
export AR="${toolchain_root}/bin/x86_64-conda-linux-gnu-ar"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
export LD_LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"
export CMAKE_PREFIX_PATH="${cpp_prefix}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"

if [[ "${RBT_SKIP_BUILD:-0}" != 1 ]]; then
    cargo build -p composer -p rbt-cli -p camera-a -p camera-b -p viewer
    cmake -S sdk/cpp -B "${cpp_build_dir}" -G Ninja \
        -DCMAKE_BUILD_TYPE=Debug \
        -DCMAKE_CXX_COMPILER="${CXX}" \
        -DCMAKE_PREFIX_PATH="${cpp_prefix}"
    cmake --build "${cpp_build_dir}" --target detector_component --parallel 2
fi

composer="${repo_root}/target/debug/composer"
camera_a="${repo_root}/target/debug/camera-a"
camera_b="${repo_root}/target/debug/camera-b"
viewer="${repo_root}/target/debug/viewer"
detector="${cpp_build_dir}/detector_component"
for binary in "${composer}" "${camera_a}" "${camera_b}" "${viewer}" "${detector}"; do
    [[ -x "${binary}" ]] || die "required binary is missing: ${binary}"
done

log_root="${RBT_SOAK_LOG_DIR:-${repo_root}/target/v01-logs}"
mkdir -p "${log_root}"
run_dir="$(mktemp -d "${log_root}/soak.XXXXXX")"
resource_log="${run_dir}/resources.tsv"
printf 'cycle\tcheckpoint\trole\tpid\trss_kib\tfds\tthreads\n' >"${resource_log}"

declare -a child_pids=()
last_pid=''
composer_pid=''
camera_pid=''
detector_pid=''
viewer_pid=''
viewer_log_label='viewer-0'
camera_role='camera-a'
orphan_capabilities=0
stale_bindings=0
duplicate_components=0
unexpected_failed=0
recovery_activate=0
recovery_deactivate=0

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

kill_and_wait() {
    local signal="$1"
    local pid="$2"
    if [[ -n "${pid}" ]] && kill -0 "${pid}" 2>/dev/null; then
        kill "-${signal}" "${pid}"
    fi
    if [[ -n "${pid}" ]]; then
        set +e
        wait "${pid}" 2>/dev/null
        set -e
    fi
}

inspect_json() {
    local output="$1"
    RBT_COMPOSER_ADDR="${composer_addr}" "${repo_root}/target/debug/rbt" ps --json >"${output}" 2>/dev/null
}

inspect_graph_json() {
    local output="$1"
    RBT_COMPOSER_ADDR="${composer_addr}" "${repo_root}/target/debug/rbt" graph --json >"${output}" 2>/dev/null
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

state_query() {
    local component="$1"
    local state="$2"
    case "${state}" in
        Active)
            printf '[.components[] | select(.component_id == "%s" and .state == "ACTIVE")] | length == 1' "${component}"
            ;;
        Blocked)
            printf '[.components[] | select(.component_id == "%s" and .state == "BLOCKED" and .block_reason != null)] | length == 1' "${component}"
            ;;
        Missing)
            printf '[.components[] | select(.component_id == "%s")] | length == 0' "${component}"
            ;;
        *)
            die "unsupported state query: ${state}"
            ;;
    esac
}

wait_state() {
    wait_for_query "$1 $2" "$(state_query "$1" "$2")"
}

record_resource() {
    local cycle="$1"
    local checkpoint="$2"
    local role="$3"
    local pid="$4"
    [[ -r "/proc/${pid}/status" ]] || return 0
    local rss_kib fds threads
    rss_kib="$(awk '/^VmRSS:/ {print $2}' "/proc/${pid}/status")"
    fds="$(find "/proc/${pid}/fd" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)"
    threads="$(find "/proc/${pid}/task" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "${cycle}" "${checkpoint}" "${role}" "${pid}" "${rss_kib:-0}" "${fds}" "${threads}" >>"${resource_log}"
}

record_all_resources() {
    local cycle="$1"
    local checkpoint="$2"
    record_resource "${cycle}" "${checkpoint}" composer "${composer_pid}"
    record_resource "${cycle}" "${checkpoint}" "${camera_role}" "${camera_pid}"
    record_resource "${cycle}" "${checkpoint}" detector "${detector_pid}"
    record_resource "${cycle}" "${checkpoint}" viewer "${viewer_pid}"
}

assert_invariants() {
    local label="$1"
    local snapshot="${run_dir}/${label}.json"
    local graph="${run_dir}/${label}.graph.json"
    inspect_json "${snapshot}" || die "could not inspect invariants at ${label}"
    inspect_graph_json "${graph}" || die "could not inspect graph invariants at ${label}"

    if ! jq -e '([.components[].component_id] | length) == ([.components[].component_id] | unique | length)' "${snapshot}" >/dev/null; then
        duplicate_components=$((duplicate_components + 1))
        die "duplicate authoritative ComponentId at ${label}"
    fi
    if ! jq -e '.process_conflicts | length == 0' "${snapshot}" >/dev/null; then
        duplicate_components=$((duplicate_components + 1))
        die "live process conflict at ${label}"
    fi
    if ! jq -e '
        . as $root |
        all($root.nodes[] | select(
                .kind == "capability" and
                .status == "active" and
                .resolved == true
            );
            . as $cap |
            any($root.nodes[];
                .kind == "component" and
                .component_id == $cap.provider_component and
                .state == "active" and
                .provider_process == $cap.provider_process and
                .activation == $cap.activation
            ) and
            any($root.edges[];
                .kind == "provides" and
                .resolved == true and
                .source == ("component:" + $cap.provider_component) and
                .target == ("capability:" + $cap.capability_instance)
            ))' "${graph}" >/dev/null; then
        orphan_capabilities=$((orphan_capabilities + 1))
        die "orphan capability at ${label}"
    fi
    if ! jq -e '
        . as $root |
        all($root.nodes[] | select(.kind == "component" and .state == "active");
            . as $component |
            all($root.nodes[] | select(
                    .kind == "requirement" and
                    .component_id == $component.component_id
                );
                . as $requirement |
                $requirement.resolved == true and
                $requirement.capability_instance != null and
                $requirement.provider_component != null and
                any($root.nodes[];
                    .kind == "capability" and
                    .status == "active" and
                    .resolved == true and
                    .capability_instance == $requirement.capability_instance and
                    .provider_component == $requirement.provider_component
                ) and
                any($root.edges[];
                    .kind == "binds" and
                    .resolved == true and
                    .source == $requirement.id and
                    .target == ("capability:" + $requirement.capability_instance)
                )
            ))' "${graph}" >/dev/null; then
        stale_bindings=$((stale_bindings + 1))
        die "stale or missing active binding at ${label}"
    fi
    if ! jq -e '[.components[] | select(.state == "FAILED" or .failure_reason != null)] | length == 0' "${snapshot}" >/dev/null; then
        unexpected_failed=$((unexpected_failed + 1))
        die "unexpected FAILED component at ${label}"
    fi
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

printf 'RBT_SOAK_RUN duration_seconds=%s max_cycles=%s address=%s logs=%s\n' \
    "${duration_seconds}" "${max_cycles}" "${composer_addr}" "${run_dir}"
start_process composer-0 env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info "${composer}"
composer_pid="${last_pid}"
wait_for_query 'composer inspection endpoint' '.phase == "recovering" or .phase == "running"'
start_process camera-a-0 env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info "${camera_a}"
camera_pid="${last_pid}"
wait_state camera-a Active
start_process detector-0 env RBT_COMPOSER_ADDR="${composer_addr}" "${detector}"
detector_pid="${last_pid}"
wait_state detector Active
start_process viewer-0 env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info "${viewer}"
viewer_pid="${last_pid}"
wait_state viewer Active
assert_invariants initial

started_at="${SECONDS}"
cycle=0
while true; do
    cycle=$((cycle + 1))
    record_all_resources "${cycle}" start

    old_camera_role="${camera_role}"
    kill_and_wait KILL "${camera_pid}"
    wait_state detector Blocked
    wait_state viewer Blocked
    if [[ "${old_camera_role}" == camera-a ]]; then
        camera_role=camera-b
        camera_binary="${camera_b}"
    else
        camera_role=camera-a
        camera_binary="${camera_a}"
    fi
    start_process "${camera_role}-${cycle}" env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info "${camera_binary}"
    camera_pid="${last_pid}"
    wait_state "${camera_role}" Active
    wait_state detector Active
    wait_state viewer Active
    assert_invariants "cycle-${cycle}-camera-replacement"
    record_all_resources "${cycle}" camera-replacement

    kill_and_wait KILL "${detector_pid}"
    wait_state detector Missing
    wait_state viewer Blocked
    start_process "detector-${cycle}" env RBT_COMPOSER_ADDR="${composer_addr}" "${detector}"
    detector_pid="${last_pid}"
    wait_state detector Active
    wait_state viewer Active
    assert_invariants "cycle-${cycle}-detector-restart"
    record_all_resources "${cycle}" detector-restart

    inspect_json "${run_dir}/cycle-${cycle}-before-composer.json"
    inspect_graph_json "${run_dir}/cycle-${cycle}-before-composer.graph.json"
    normalise_runtime_identity "${run_dir}/cycle-${cycle}-before-composer.json" \
        "${run_dir}/cycle-${cycle}-before-composer.graph.json" \
        >"${run_dir}/cycle-${cycle}-before-composer.identity.json"
    viewer_lines_before="$(grep -Fc 'VIEW component=' "${run_dir}/${viewer_log_label}.log" 2>/dev/null || true)"
    kill_and_wait KILL "${composer_pid}"
    data_deadline=$((SECONDS + wait_seconds))
    viewer_lines_after="${viewer_lines_before}"
    while ((SECONDS < data_deadline)); do
        viewer_lines_after="$(grep -Fc 'VIEW component=' "${run_dir}/${viewer_log_label}.log" 2>/dev/null || true)"
        if ((viewer_lines_after > viewer_lines_before)); then
            break
        fi
        sleep 0.1
    done
    ((viewer_lines_after > viewer_lines_before)) \
        || die "data did not continue while composer was down in cycle ${cycle}"
    start_process "composer-${cycle}" env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info "${composer}"
    composer_pid="${last_pid}"
    wait_for_query "composer recovery cycle ${cycle}" \
        '.phase == "running" and ([.components[] | select(.state == "ACTIVE")] | length == 3)'
    inspect_json "${run_dir}/cycle-${cycle}-after-composer.json"
    inspect_graph_json "${run_dir}/cycle-${cycle}-after-composer.graph.json"
    normalise_runtime_identity "${run_dir}/cycle-${cycle}-after-composer.json" \
        "${run_dir}/cycle-${cycle}-after-composer.graph.json" \
        >"${run_dir}/cycle-${cycle}-after-composer.identity.json"
    cmp -s "${run_dir}/cycle-${cycle}-before-composer.identity.json" \
        "${run_dir}/cycle-${cycle}-after-composer.identity.json" \
        || die "composer recovery changed healthy runtime identity in cycle ${cycle}"
    if ! grep -Fq 'reconciliation: 0 deactivate, 0 activate' "${run_dir}/composer-${cycle}.log"; then
        die "composer recovery was not 0 deactivate/0 activate in cycle ${cycle}"
    fi
    recovery_activate=$((recovery_activate + $(grep -Fc 'sending Activate' "${run_dir}/composer-${cycle}.log" || true)))
    recovery_deactivate=$((recovery_deactivate + $(grep -Fc 'sending Deactivate' "${run_dir}/composer-${cycle}.log" || true)))
    assert_invariants "cycle-${cycle}-composer-recovery"
    record_all_resources "${cycle}" composer-recovery

    kill_and_wait KILL "${viewer_pid}"
    wait_state viewer Missing
    start_process "viewer-${cycle}" env RBT_COMPOSER_ADDR="${composer_addr}" RUST_LOG=info "${viewer}"
    viewer_pid="${last_pid}"
    viewer_log_label="viewer-${cycle}"
    wait_state viewer Active
    assert_invariants "cycle-${cycle}-viewer-restart"
    record_all_resources "${cycle}" viewer-restart

    printf 'RBT_SOAK_CYCLE cycle=%s elapsed_seconds=%s camera=%s invariants=pass\n' \
        "${cycle}" "$((SECONDS - started_at))" "${camera_role}"
    if ((max_cycles > 0 && cycle >= max_cycles)); then
        break
    fi
    if ((SECONDS - started_at >= duration_seconds)); then
        break
    fi
done

resource_samples="$(($(wc -l <"${resource_log}") - 1))"
resource_growth_violations="$(awk -F '\t' '
    NR == 1 { next }
    {
        key = $3 ":" $4
        if (!(key in first_rss)) {
            first_rss[key] = $5; first_fd[key] = $6; first_threads[key] = $7
        }
        last_rss[key] = $5; last_fd[key] = $6; last_threads[key] = $7
    }
    END {
        violations = 0
        for (key in first_rss) {
            if (last_rss[key] - first_rss[key] > 65536 ||
                last_fd[key] - first_fd[key] > 64 ||
                last_threads[key] - first_threads[key] > 32) {
                violations++
            }
        }
        print violations
    }' "${resource_log}")"

((recovery_activate == 0)) || die "healthy recoveries emitted ${recovery_activate} Activate commands"
((recovery_deactivate == 0)) || die "healthy recoveries emitted ${recovery_deactivate} Deactivate commands"
((resource_growth_violations == 0)) || die "resource observations show obvious growth; see ${resource_log}"

printf 'RBT_SOAK_RESULT duration_seconds=%s cycles=%s\n' "$((SECONDS - started_at))" "${cycle}"
printf 'RBT_SOAK_RESULT orphan_capabilities=%s stale_bindings=%s duplicate_components=%s unexpected_failed=%s\n' \
    "${orphan_capabilities}" "${stale_bindings}" "${duplicate_components}" "${unexpected_failed}"
printf 'RBT_SOAK_RESULT recovery_deactivate=%s recovery_activate=%s\n' \
    "${recovery_deactivate}" "${recovery_activate}"
printf 'RBT_SOAK_RESULT resource_samples=%s obvious_unbounded_growth=%s resource_log=%s\n' \
    "${resource_samples}" "${resource_growth_violations}" "${resource_log}"
printf 'PASS: v0.1 deterministic fault soak; logs=%s\n' "${run_dir}"

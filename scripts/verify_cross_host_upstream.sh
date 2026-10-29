#!/usr/bin/env bash
set -euo pipefail

# Reproducible, isolated capability verifier for the upstream iceoryx2 Zenoh
# link tunnel. The probe compares the pristine v0.10.0 release independently
# of production, whose unified rbt fork is pinned and fingerprint-verified.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

usage() {
    cat <<'USAGE'
usage: scripts/verify_cross_host_upstream.sh --quick|--full

--quick  Build/check the pinned release and run transport, lifecycle,
         partition, allow-list, reactive, and basic Sidecar gates.
--full   Add churn, reconnect/resource checks, high-rate Sidecar, final-ready
         overload evidence, Image/PointCloud payloads, pressure/progress, and
         schema/fingerprint mismatch checks.

Environment overrides:
  RBT_UPSTREAM_RELEASE_SOURCE  release checkout (default target/iceoryx2-v0.10.0)
  RBT_UPSTREAM_MAIN_SOURCE     main checkout (default target/iceoryx2-upstream-main)
  RBT_PHASE4_TOOLCHAIN         Phase4 compiler root
  RBT_CROSS_HOST_UPSTREAM_LOG_ROOT  retained evidence root
USAGE
}

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

if (($# != 1)); then
    usage >&2
    exit 2
fi
case "$1" in
    --quick) mode=quick ;;
    --full) mode=full ;;
    --help|-h)
        usage
        exit 0
        ;;
    *)
        usage >&2
        exit 2
        ;;
esac

require_command() {
    command -v "$1" >/dev/null 2>&1 || die "required command not found: $1"
}

for command_name in awk basename cargo date find git grep ip mkdir mount nsenter ps seq sha256sum \
    sed sleep tail tee unshare wc; do
    require_command "${command_name}"
done

release_source="${RBT_UPSTREAM_RELEASE_SOURCE:-${repo_root}/target/iceoryx2-v0.10.0}"
main_source="${RBT_UPSTREAM_MAIN_SOURCE:-${repo_root}/target/iceoryx2-upstream-main}"
toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
toolchain_bin="${toolchain_root}/bin"
log_root="${RBT_CROSS_HOST_UPSTREAM_LOG_ROOT:-${repo_root}/target/cross-host-upstream-verify-logs}"
probe_manifest="${repo_root}/experiments/cross-host-upstream/Cargo.toml"
probe_target="${repo_root}/experiments/cross-host-upstream/target/debug/cross-host-upstream-probe"
zenoh_config_a="${repo_root}/experiments/cross-host-upstream/zenoh-hosta.toml"
zenoh_config_b="${repo_root}/experiments/cross-host-upstream/zenoh-hostb.toml"
release_tunnel="${release_source}/integrations/zenoh/target/debug/iox2-link-tunnel-zenoh"
release_service="${release_source}/target/debug/iox2-service"
main_tunnel="${main_source}/integrations/zenoh/target/debug/iox2-link-tunnel-zenoh"

expected_release_commit=135d09dd8b29f321f1725920d434864c4e512378
expected_main_commit=d32f60beedeef90ad480a3c7926c7211dd170512

[[ -e "${release_source}/.git" ]] || die "release checkout missing: ${release_source}"
[[ -e "${main_source}/.git" ]] || die "main checkout missing: ${main_source}"
[[ -f "${probe_manifest}" ]] || die "standalone probe manifest missing: ${probe_manifest}"
[[ -f "${zenoh_config_a}" ]] || die "Host A Zenoh config missing: ${zenoh_config_a}"
[[ -f "${zenoh_config_b}" ]] || die "Host B Zenoh config missing: ${zenoh_config_b}"
grep -Fqx 'mode = "peer"' "${zenoh_config_a}" \
    || die "Host A Zenoh config is not peer mode"
grep -Fqx 'mode = "peer"' "${zenoh_config_b}" \
    || die "Host B Zenoh config is not peer mode"
grep -Fqx 'enabled = false' "${zenoh_config_a}" \
    || die "Host A multicast scouting is not disabled"
grep -Fqx 'enabled = false' "${zenoh_config_b}" \
    || die "Host B multicast scouting is not disabled"
grep -Fq 'tcp/10.77.62.1:7447' "${zenoh_config_a}" \
    || die "Host A Zenoh listener endpoint is missing"
grep -Fq 'tcp/10.77.62.1:7447' "${zenoh_config_b}" \
    || die "Host B Zenoh connect endpoint is missing"
[[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-gcc" ]] \
    || die "Phase4 GCC missing under ${toolchain_bin}"

release_commit="$(git -C "${release_source}" rev-parse HEAD)"
main_commit="$(git -C "${main_source}" rev-parse HEAD)"
[[ "${release_commit}" == "${expected_release_commit}" ]] \
    || die "release checkout is ${release_commit}, expected ${expected_release_commit}"
[[ "${main_commit}" == "${expected_main_commit}" ]] \
    || die "main checkout is ${main_commit}, expected ${expected_main_commit}"

# Check the production workspace's unified fork pin independently of the
# isolated upstream release used by this experiment.
grep -Fqx 'iceoryx2 = { version = "=0.10.0", path = "third_party/iceoryx2-rbt/source/iceoryx2" }' <(grep '^iceoryx2 = ' Cargo.toml) \
    || die 'production Cargo.toml no longer directly pins the maintained iceoryx2 source'
grep -Fqx 'iceoryx2 = { path = "third_party/iceoryx2-rbt/source/iceoryx2" }' \
    <(grep '^iceoryx2 = { path' Cargo.toml) \
    || die 'production Cargo.toml no longer patches iceoryx2 to the unified rbt source'
grep -Fqx 'fork_version = "0.10.0-rbt.2"' third_party/iceoryx2-rbt/UPSTREAM.toml \
    || die 'production fork metadata no longer identifies 0.10.0-rbt.2'
grep -Fqx 'base_commit = "135d09dd8b29f321f1725920d434864c4e512378"' \
    third_party/iceoryx2-rbt/UPSTREAM.toml \
    || die 'production fork metadata no longer identifies the pinned v0.10.0 base'
awk '
    $0 == "[[package]]" { in_iox2 = 0 }
    $0 == "name = \"iceoryx2\"" { count++; in_iox2 = 1; next }
    in_iox2 && $0 == "version = \"0.10.0\"" { version_count++ }
    in_iox2 && $0 ~ /^source = / { source_count++ }
    END { exit !(count == 1 && version_count == 1 && source_count == 0) }
' Cargo.lock \
    || die 'production Cargo.lock must contain only one path-sourced iceoryx2 package at 0.10.0'
production_files=(
    Cargo.toml
    Cargo.lock
    crates/rbt-types/src/lib.rs
    crates/rbt-iox2/src/lib.rs
    third_party/iceoryx2-rbt/UPSTREAM.toml
    third_party/iceoryx2-rbt/SOURCE_DIFF_SHA256
    third_party/iceoryx2-rbt/patches/series
    third_party/iceoryx2-rbt/patches/SHA256SUMS
    third_party/iceoryx2-rbt/patches/*.patch
)
for production_file in "${production_files[@]}"; do
    [[ -f "${production_file}" ]] || die "production audit file missing: ${production_file}"
done

export PATH="${toolchain_bin}:${PATH}"
export CC="${toolchain_bin}/x86_64-conda-linux-gnu-gcc"
export CXX="${toolchain_bin}/x86_64-conda-linux-gnu-g++"
export AR="${toolchain_bin}/x86_64-conda-linux-gnu-ar"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"

mkdir -p "${log_root}"
run_id="upstream-${mode}-$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="${log_root}/${run_id}"
mkdir -p "${run_dir}"

run_logged() {
    local label="$1"
    shift
    {
        printf '+'
        printf ' %q' "$@"
        printf '\n'
    } | tee "${run_dir}/${label}.command.log"
    if ! "$@" >"${run_dir}/${label}.log" 2>&1; then
        sed -n '1,260p' "${run_dir}/${label}.log" >&2
        die "${label} failed; see ${run_dir}/${label}.log"
    fi
}

run_optional_logged() {
    local label="$1"
    shift
    {
        printf '+'
        printf ' %q' "$@"
        printf '\n'
    } | tee "${run_dir}/${label}.command.log"
    "$@" >"${run_dir}/${label}.log" 2>&1 || {
        printf 'XFAIL %s command_status=%s\n' "${label}" "$?" \
            | tee -a "${run_dir}/${label}.log"
    }
}

run_logged production-iox2-source-prepare ./scripts/bootstrap_iceoryx2_rbt.sh --source-only
run_logged production-iox2-source-fingerprint ./scripts/bootstrap_iceoryx2_rbt.sh --verify
production_source_diff="$(awk 'NR == 1 { print $1 }' \
    third_party/iceoryx2-rbt/SOURCE_DIFF_SHA256)"
production_hash_before="$(sha256sum "${production_files[@]}")"

printf 'CROSS_HOST_UPSTREAM_VERIFY run_id=%s mode=%s logs=%s\n' \
    "${run_id}" "${mode}" "${run_dir}" | tee "${run_dir}/environment.log"
printf 'release_source=%s\nrelease_commit=%s\nmain_source=%s\nmain_commit=%s\n' \
    "${release_source}" "${release_commit}" "${main_source}" "${main_commit}" \
    | tee -a "${run_dir}/environment.log"
printf 'production_hash_before=%s\n' "${production_hash_before}" \
    | tee -a "${run_dir}/environment.log"
printf 'production_iceoryx2_pin=0.10.0 fork=0.10.0-rbt.2 base_commit=135d09dd8b29f321f1725920d434864c4e512378 source_diff_sha256=%s\n' \
    "${production_source_diff}" | tee -a "${run_dir}/environment.log"

# Keep the isolated probe and upstream builds locked and separate from the
# production fork. Runtime comparison uses the exact v0.10.0 release binary.
run_logged probe-fmt cargo fmt --manifest-path "${probe_manifest}" -- --check
run_logged probe-test cargo test --manifest-path "${probe_manifest}" --locked
run_logged probe-clippy cargo clippy --manifest-path "${probe_manifest}" --locked \
    --all-targets -- -D warnings
run_logged probe-build cargo build --manifest-path "${probe_manifest}" --locked

if [[ ! -x "${release_tunnel}" ]]; then
    run_logged release-tunnel-build cargo build --manifest-path \
        "${release_source}/integrations/zenoh/Cargo.toml" --locked \
        --bin iox2-link-tunnel-zenoh
fi
if [[ ! -x "${release_service}" ]]; then
    run_logged release-service-build cargo build --manifest-path \
        "${release_source}/Cargo.toml" --locked -p iceoryx2-cli --bin iox2-service
fi

# Main has no role in the runtime comparison, but compile its exact audited
# checkout when the reusable binary is absent. This catches a stale checkout
# without replacing the release executable used by the experiment.
if [[ ! -x "${main_tunnel}" ]]; then
    run_logged main-tunnel-build cargo build --manifest-path \
        "${main_source}/integrations/zenoh/Cargo.toml" --locked \
        --bin iox2-link-tunnel-zenoh
fi
[[ -x "${probe_target}" ]] || die "probe binary missing after build: ${probe_target}"
[[ -x "${release_tunnel}" ]] || die "release tunnel binary missing: ${release_tunnel}"
[[ -x "${release_service}" ]] || die "release service binary missing: ${release_service}"

# The inner script runs in an unshared user/mount/net/pid namespace. Its only
# externally visible state is the retained log directory. It deliberately
# keeps the two namespace hosts on a veth so every tested message crosses the
# tunnel; private tmpfs /tmp and /dev/shm prevent local IPC shortcuts.
set +e
unshare --user --map-root-user --mount --net --pid --fork --mount-proc \
    bash -s -- "${mode}" "${run_dir}" "${release_tunnel}" "${probe_target}" \
    "${release_service}" "${zenoh_config_a}" "${zenoh_config_b}" <<'NAMESPACE_VERIFY'
set -euo pipefail

mode="$1"
run_dir="$2"
tunnel="$3"
probe="$4"
iox2_service="$5"
zenoh_config_a="$6"
zenoh_config_b="$7"

managed_pids=()
track_pid() {
    managed_pids+=("$1")
}

children_of() {
    local pid="$1"
    local children=''
    [[ -r "/proc/${pid}/task/${pid}/children" ]] || return 0
    children="$(<"/proc/${pid}/task/${pid}/children")"
    printf '%s\n' ${children:-}
}

# A background `ip netns exec`/`nsenter` shell can outlive the wrapper that
# launched it. Walk descendants before terminating the parent, then repeat
# with SIGKILL so the verifier never leaks tunnel/probe processes.
stop_pid() {
    local pid="$1"
    local child
    [[ "${pid}" =~ ^[0-9]+$ ]] || return 0
    [[ -d "/proc/${pid}" ]] || return 0
    while read -r child; do
        [[ -n "${child}" ]] || continue
        stop_pid "${child}"
    done < <(children_of "${pid}")
    kill -TERM "${pid}" 2>/dev/null || true
    sleep 0.03
    kill -KILL "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
}

cleanup() {
    local index
    set +e
    [[ -e /sys/class/net/vetha ]] && ip link set vetha down 2>/dev/null || true
    [[ -e /sys/class/net/vethb ]] && ip link set vethb down 2>/dev/null || true
    for ((index=${#managed_pids[@]} - 1; index >= 0; index--)); do
        stop_pid "${managed_pids[index]}"
    done
    ip netns del hosta 2>/dev/null || true
    ip netns del hostb 2>/dev/null || true
}
trap cleanup EXIT INT TERM

mount --make-rprivate /
mkdir -p /run/netns
mount -t tmpfs tmpfs /run/netns
ip netns add hosta
ip netns add hostb
ip link add vetha type veth peer name vethb
ip link set vetha netns hosta
ip link set vethb netns hostb
ip -n hosta link set lo up
ip -n hostb link set lo up
ip -n hosta addr add 10.77.62.1/24 dev vetha
ip -n hostb addr add 10.77.62.2/24 dev vethb
ip -n hosta link set vetha mtu 1500 up
ip -n hostb link set vethb mtu 1500 up

ip netns exec hosta unshare --mount --ipc --uts bash -c '
    set -e
    mount --make-rprivate /
    mount -t tmpfs tmpfs /dev/shm
    mount -t tmpfs tmpfs /tmp
    mkdir -p /tmp/home /tmp/config /tmp/cache /tmp/log
    hostname upstream-hosta
    exec sleep 86400
' >"${run_dir}/hosta-anchor.log" 2>&1 &
hosta_anchor=$!
track_pid "${hosta_anchor}"

ip netns exec hostb unshare --mount --ipc --uts bash -c '
    set -e
    mount --make-rprivate /
    mount -t tmpfs tmpfs /dev/shm
    mount -t tmpfs tmpfs /tmp
    mkdir -p /tmp/home /tmp/config /tmp/cache /tmp/log
    hostname upstream-hostb
    exec sleep 86400
' >"${run_dir}/hostb-anchor.log" 2>&1 &
hostb_anchor=$!
track_pid "${hostb_anchor}"
sleep 0.5

host_a() {
    nsenter --target "${hosta_anchor}" --mount --ipc --uts --net \
        --preserve-credentials -- env HOME=/tmp/home XDG_CONFIG_HOME=/tmp/config \
        XDG_CACHE_HOME=/tmp/cache "$@"
}
host_b() {
    nsenter --target "${hostb_anchor}" --mount --ipc --uts --net \
        --preserve-credentials -- env HOME=/tmp/home XDG_CONFIG_HOME=/tmp/config \
        XDG_CACHE_HOME=/tmp/cache "$@"
}

start_tunnel_a() {
    local label="$1"
    shift
    host_a "${tunnel}" --zenoh-config "${zenoh_config_a}" "$@" \
        >"${run_dir}/${label}-a.log" 2>&1 &
    tunnel_a=$!
    track_pid "${tunnel_a}"
}
start_tunnel_b() {
    local label="$1"
    shift
    host_b "${tunnel}" --zenoh-config "${zenoh_config_b}" "$@" \
        >"${run_dir}/${label}-b.log" 2>&1 &
    tunnel_b=$!
    track_pid "${tunnel_b}"
}
start_tunnels() {
    local label="$1"
    shift
    start_tunnel_a "${label}" "$@"
    sleep 0.5
    start_tunnel_b "${label}" "$@"
    sleep 1
}

process_alive() {
    [[ -d "/proc/$1" ]] && kill -0 "$1" 2>/dev/null
}

wait_for_line() {
    local file="$1"
    local pattern="$2"
    local seconds="$3"
    local attempt
    for attempt in $(seq 1 $((seconds * 20))); do
        grep -Eq "${pattern}" "${file}" 2>/dev/null && return 0
        sleep 0.05
    done
    return 1
}

run_probe_pair() {
    local label="$1"
    shift
    local -a subscriber_args=()
    local -a publisher_args=()
    while (($# > 0)) && [[ "$1" != -- ]]; do
        subscriber_args+=("$1")
        shift
    done
    [[ "${1:-}" == -- ]] || {
        printf 'missing pair separator for %s\n' "${label}" >&2
        return 2
    }
    shift
    publisher_args=("$@")

    host_b "${probe}" "${subscriber_args[@]}" \
        >"${run_dir}/${label}-subscriber.log" 2>&1 &
    local subscriber_pid=$!
    track_pid "${subscriber_pid}"
    sleep 0.15
    host_a "${probe}" "${publisher_args[@]}" \
        >"${run_dir}/${label}-publisher.log" 2>&1 &
    local publisher_pid=$!
    track_pid "${publisher_pid}"
    local publisher_status=0
    local subscriber_status=0
    wait "${publisher_pid}" || publisher_status=$?
    wait "${subscriber_pid}" || subscriber_status=$?
    if ((publisher_status != 0 || subscriber_status != 0)); then
        sed -n '1,180p' "${run_dir}/${label}-publisher.log" >&2 || true
        sed -n '1,180p' "${run_dir}/${label}-subscriber.log" >&2 || true
        printf 'FAIL %s publisher_status=%s subscriber_status=%s\n' \
            "${label}" "${publisher_status}" "${subscriber_status}" \
            | tee -a "${run_dir}/gates.log" >&2
        return 1
    fi
    if ! grep -q '^SUMMARY ' "${run_dir}/${label}-publisher.log" \
        || ! grep -q '^SUMMARY ' "${run_dir}/${label}-subscriber.log"; then
        sed -n '1,180p' "${run_dir}/${label}-publisher.log" >&2 || true
        sed -n '1,180p' "${run_dir}/${label}-subscriber.log" >&2 || true
        printf 'FAIL %s missing-summary publisher_status=%s subscriber_status=%s\n' \
            "${label}" "${publisher_status}" "${subscriber_status}" \
            | tee -a "${run_dir}/gates.log" >&2
        return 1
    fi
    printf 'PASS %s\n' "${label}" | tee -a "${run_dir}/gates.log"
}

run_preexisting_reactive_pair() {
    local label="$1"
    local check_withdrawal="$2"
    shift 2
    local -a subscriber_args=()
    local -a publisher_args=()
    while (($# > 0)) && [[ "$1" != -- ]]; do
        subscriber_args+=("$1")
        shift
    done
    [[ "${1:-}" == -- ]] || {
        printf 'missing pair separator for %s\n' "${label}" >&2
        return 2
    }
    shift
    publisher_args=("$@")
    local publisher_service=''
    local publisher_arg_index
    for ((publisher_arg_index = 0; publisher_arg_index < ${#publisher_args[@]}; publisher_arg_index++)); do
        if [[ "${publisher_args[publisher_arg_index]}" == --service \
            && $((publisher_arg_index + 1)) -lt ${#publisher_args[@]} ]]; then
            publisher_service="${publisher_args[publisher_arg_index + 1]}"
            break
        fi
    done

    # The provider must exist before either reactive tunnel starts.  This is
    # intentionally different from run_probe_pair: it checks startup import
    # of an already-existing local service without a poll wake source.
    stop_pid "${tunnel_a:-}"
    stop_pid "${tunnel_b:-}"
    host_a "${probe}" "${publisher_args[@]}" \
        >"${run_dir}/${label}-publisher.log" 2>&1 &
    local publisher_pid=$!
    track_pid "${publisher_pid}"
    local ready_status=0
    wait_for_line "${run_dir}/${label}-publisher.log" '^READY ' 10 \
        || ready_status=$?

    start_tunnel_a "${label}" --reactive
    sleep 0.5
    start_tunnel_b "${label}" --reactive
    sleep 1
    host_b "${probe}" "${subscriber_args[@]}" \
        >"${run_dir}/${label}-subscriber.log" 2>&1 &
    local subscriber_pid=$!
    track_pid "${subscriber_pid}"
    wait_for_line "${run_dir}/${label}-subscriber.log" '^READY ' 30 \
        || ready_status=$?

    local publisher_status=0
    local subscriber_status=0
    wait "${subscriber_pid}" || subscriber_status=$?
    wait "${publisher_pid}" || publisher_status=$?
    local summary_status=0
    grep -q '^SUMMARY ' "${run_dir}/${label}-publisher.log" \
        || summary_status=1
    grep -q '^SUMMARY ' "${run_dir}/${label}-subscriber.log" \
        || summary_status=1
    if ((ready_status == 0 && publisher_status == 0 && subscriber_status == 0 \
        && summary_status == 0)); then
        printf 'PASS %s preexisting-reactive-data\n' "${label}" \
            | tee -a "${run_dir}/gates.log"
    else
        printf 'XFAIL %s preexisting-reactive-data publisher_status=%s subscriber_status=%s ready_status=%s summary_status=%s\n' \
            "${label}" "${publisher_status}" "${subscriber_status}" \
            "${ready_status}" "${summary_status}" \
            | tee -a "${run_dir}/gates.log"
    fi

    if [[ "${check_withdrawal}" == yes && ${publisher_status} -eq 0 \
        && -n "${publisher_service}" ]]; then
        sleep 3
        if wait_service_state "${publisher_service}" absent 1 \
            "${run_dir}/${label}-after-provider.json"; then
            printf 'PASS pure-reactive-local-withdrawal-wake label=%s\n' "${label}" \
                | tee -a "${run_dir}/gates.log"
        else
            printf 'XFAIL pure-reactive-local-withdrawal-wake label=%s expected-source-limitation\n' \
                "${label}" | tee -a "${run_dir}/gates.log"
        fi
    fi
    stop_pid "${tunnel_a}"
    stop_pid "${tunnel_b}"
}

run_probe_command() {
    local label="$1"
    shift
    if ! "$@" >"${run_dir}/${label}.log" 2>&1; then
        sed -n '1,220p' "${run_dir}/${label}.log" >&2 || true
        die "${label} failed; see ${run_dir}/${label}.log"
    fi
}

run_observed_churn() {
    local label="$1"
    local kind="$2"
    local prefix="$3"
    local cycles="$4"
    local hold_ms="$5"
    local gap_ms="$6"
    local probe_timeout_ms="$7"
    local observe_timeout_ms="$8"
    local observer_log="${run_dir}/${label}-observer.log"
    local churn_log="${run_dir}/${label}.log"
    local observer_pid churn_pid observer_status churn_status

    host_b "${probe}" service-observe --prefix "${prefix}" \
        --timeout-ms "${observe_timeout_ms}" --poll-ms 20 \
        >"${observer_log}" 2>&1 &
    observer_pid=$!
    track_pid "${observer_pid}"
    wait_for_line "${observer_log}" '^READY ' 10
    if [[ "${kind}" == same ]]; then
        host_a "${probe}" churn-same --service "${prefix}" --cycles "${cycles}" \
            --hold-ms "${hold_ms}" --gap-ms "${gap_ms}" \
            --timeout-ms "${probe_timeout_ms}" >"${churn_log}" 2>&1 &
    else
        host_a "${probe}" churn-unique --prefix "${prefix}" --cycles "${cycles}" \
            --hold-ms "${hold_ms}" --gap-ms "${gap_ms}" \
            --timeout-ms "${probe_timeout_ms}" >"${churn_log}" 2>&1 &
    fi
    churn_pid=$!
    track_pid "${churn_pid}"
    sample_tunnel_peak "${label}" "${churn_pid}" "${tunnel_a}" "${tunnel_b}"
    local sampler_pid="${resource_sampler_pid}"
    churn_status=0
    wait "${churn_pid}" || churn_status=$?
    local sampler_status=0
    wait "${sampler_pid}" || sampler_status=$?
    observer_status=0
    wait "${observer_pid}" || observer_status=$?
    if ((churn_status != 0 || observer_status != 0 || sampler_status != 0)); then
        sed -n '1,220p' "${churn_log}" >&2 || true
        sed -n '1,220p' "${observer_log}" >&2 || true
        sed -n '1,120p' "${run_dir}/${label}-resource-sampler.log" >&2 || true
        die "${label} churn/observer failed churn_status=${churn_status} observer_status=${observer_status} sampler_status=${sampler_status}"
    fi
    grep -Eq "^SUMMARY role=churn kind=${kind} .*requested_cycles=${cycles} .*created=${cycles} .*dropped=${cycles} .*errors=0 " \
        "${churn_log}" \
        || die "${label} churn metrics were not exact"
    grep -q "^OBSERVE prefix=${prefix} transition=present " "${observer_log}" \
        || die "${label} observer never saw a remote present transition"
    local expected_transitions=$((cycles * 2 + 1))
    grep -Eq "^SUMMARY role=observer kind=service-observe prefix=${prefix} .* transitions=${expected_transitions} final_present=false " \
        "${observer_log}" \
        || die "${label} observer did not see every present/absent lifecycle; expected transitions=${expected_transitions}"
    wait_service_state "${prefix}" absent 30 "${run_dir}/${label}-final.json" \
        || die "${label} remote prefix remained visible after final drop"
    printf 'PASS %s observed-created=%s observed-dropped=%s\n' \
        "${label}" "${cycles}" "${cycles}" | tee -a "${run_dir}/gates.log"
}

sidecar_field() {
    local log_file="$1"
    local field="$2"
    sed -n "s/^SUMMARY role=subscriber kind=sidecar .* ${field}=\\([^ ]*\\).*$/\\1/p" \
        "${log_file}" | tail -n 1
}

check_sidecar_pair() {
    local label="$1"
    local expected="$2"
    local subscriber_log="${run_dir}/${label}-subscriber.log"
    local publisher_log="${run_dir}/${label}-publisher.log"
    local samples remaining final_observed payload_ok payload_errors
    samples="$(sidecar_field "${subscriber_log}" samples)"
    remaining="$(sidecar_field "${subscriber_log}" remaining_after_last_wake)"
    final_observed="$(sidecar_field "${subscriber_log}" final_observed)"
    payload_ok="$(sed -n 's/^SUMMARY role=publisher kind=sidecar .* payload_ok=\([^ ]*\).*$/\1/p' \
        "${publisher_log}" | tail -n 1)"
    payload_errors="$(sed -n 's/^SUMMARY role=publisher kind=sidecar .* payload_errors=\([^ ]*\).*$/\1/p' \
        "${publisher_log}" | tail -n 1)"
    [[ -n "${samples}" && -n "${payload_ok}" && -n "${payload_errors}" ]] \
        || die "${label} missing parseable Sidecar summary"
    printf 'SIDECAR_RESULT label=%s requested=%s samples=%s payload_ok=%s payload_errors=%s remaining_after_last_wake=%s final_observed=%s\n' \
        "${label}" "${expected}" "${samples}" "${payload_ok}" "${payload_errors}" \
        "${remaining:-unknown}" "${final_observed:-unknown}" \
        | tee -a "${run_dir}/gates.log"
    if [[ "${samples}" == "${expected}" && "${payload_ok}" == "${expected}" \
        && "${payload_errors}" == 0 && "${remaining}" == 0 \
        && "${final_observed}" == 0 ]]; then
        printf 'PASS %s sidecar-progress\n' "${label}" | tee -a "${run_dir}/gates.log"
        return 0
    fi
    printf 'XFAIL %s sidecar-progress-bounded-queue-or-final-strand\n' "${label}" \
        | tee -a "${run_dir}/gates.log"
    return 1
}

run_expected_schema_mismatch() {
    local service="$1"
    local subscriber_log="${run_dir}/schema-mismatch-subscriber.log"
    local publisher_log="${run_dir}/schema-mismatch-publisher.log"
    set +e
    host_b "${probe}" image-sub --service "${service}" --count 1 \
        --timeout-ms 4000 >"${subscriber_log}" 2>&1 &
    local subscriber_pid=$!
    track_pid "${subscriber_pid}"
    sleep 0.15
    host_a "${probe}" bytes-pub --service "${service}" --payload-bytes 1024 \
        --count 1 --timeout-ms 4000 >"${publisher_log}" 2>&1 &
    local publisher_pid=$!
    track_pid "${publisher_pid}"
    local subscriber_status=0
    local publisher_status=0
    wait "${subscriber_pid}" || subscriber_status=$?
    wait "${publisher_pid}" || publisher_status=$?
    set -e
    if ((subscriber_status != 0 || publisher_status != 0)); then
        printf 'PASS schema-fingerprint-mismatch subscriber_status=%s publisher_status=%s\n' \
            "${subscriber_status}" "${publisher_status}" | tee -a "${run_dir}/gates.log"
    else
        printf 'XFAIL schema-fingerprint-mismatch unexpectedly-opened-both\n' \
            | tee -a "${run_dir}/gates.log"
    fi
}

service_visible() {
    local service="$1"
    local output="$2"
    host_b "${iox2_service}" list -f JSON >"${output}" 2>&1 || true
    grep -Fq "${service}" "${output}"
}

wait_service_state() {
    local service="$1"
    local expected="$2"
    local timeout_seconds="$3"
    local output="$4"
    local attempt
    for attempt in $(seq 1 $((timeout_seconds * 20))); do
        if service_visible "${service}" "${output}"; then
            [[ "${expected}" == present ]] && return 0
        elif [[ "${expected}" == absent ]]; then
            return 0
        fi
        sleep 0.05
    done
    return 1
}

snapshot_tunnel() {
    local label="$1"
    local pid="$2"
    local rss=0 fds=0 tasks=0 cpu_ticks=0
    local current child rss_one fds_one tasks_one cpu_one
    local -a pending=("${pid}")
    local pending_index=0
    declare -A seen=()
    while ((pending_index < ${#pending[@]})); do
        current="${pending[pending_index]}"
        pending_index=$((pending_index + 1))
        [[ "${current}" =~ ^[0-9]+$ ]] || continue
        [[ -d "/proc/${current}" ]] || continue
        [[ "${seen[${current}]+yes}" == yes ]] && continue
        seen["${current}"]=yes
        rss_one="$(awk '/^VmRSS:/{print $2}' "/proc/${current}/status" 2>/dev/null || true)"
        fds_one="$(find "/proc/${current}/fd" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)"
        tasks_one="$(find "/proc/${current}/task" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)"
        cpu_one="$(awk '{print $14 + $15}' "/proc/${current}/stat" 2>/dev/null || true)"
        rss=$((rss + ${rss_one:-0}))
        fds=$((fds + fds_one))
        tasks=$((tasks + tasks_one))
        cpu_ticks=$((cpu_ticks + ${cpu_one:-0}))
        while read -r child; do
            [[ -n "${child}" ]] && pending+=("${child}")
        done < <(children_of "${current}")
    done
    printf 'RESOURCE_SNAPSHOT label=%s pid=%s tree_pids=%s rss_kib=%s fds=%s tasks=%s cpu_ticks=%s\n' \
        "${label}" "${pid}" "${#pending[@]}" "${rss}" "${fds}" "${tasks}" "${cpu_ticks}" \
        | tee -a "${run_dir}/resources.log"
}

resource_values() {
    local pid="$1"
    local rss=0 fds=0 tasks=0
    local current child rss_one fds_one tasks_one
    local -a pending=("${pid}")
    local pending_index=0
    declare -A seen=()
    while ((pending_index < ${#pending[@]})); do
        current="${pending[pending_index]}"
        pending_index=$((pending_index + 1))
        [[ "${current}" =~ ^[0-9]+$ ]] || continue
        [[ -d "/proc/${current}" ]] || continue
        [[ "${seen[${current}]+yes}" == yes ]] && continue
        seen["${current}"]=yes
        rss_one="$(awk '/^VmRSS:/{print $2}' "/proc/${current}/status" 2>/dev/null || true)"
        fds_one="$(find "/proc/${current}/fd" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)"
        tasks_one="$(find "/proc/${current}/task" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)"
        rss=$((rss + ${rss_one:-0}))
        fds=$((fds + fds_one))
        tasks=$((tasks + tasks_one))
        while read -r child; do
            [[ -n "${child}" ]] && pending+=("${child}")
        done < <(children_of "${current}")
    done
    printf '%s %s %s\n' "${rss}" "${fds}" "${tasks}"
}

sample_tunnel_peak() {
    local label="$1"
    local watched_pid="$2"
    local tunnel_pid_a="$3"
    local tunnel_pid_b="$4"
    local sampler_log="${run_dir}/${label}-resource-sampler.log"
    (
        local max_rss_a=0 max_rss_b=0 max_fds_a=0 max_fds_b=0
        local max_tasks_a=0 max_tasks_b=0 sample_count=0
        local rss_a fds_a tasks_a rss_b fds_b tasks_b
        while process_alive "${watched_pid}" || ((sample_count == 0)); do
            read -r rss_a fds_a tasks_a < <(resource_values "${tunnel_pid_a}")
            read -r rss_b fds_b tasks_b < <(resource_values "${tunnel_pid_b}")
            ((rss_a > max_rss_a)) && max_rss_a="${rss_a}"
            ((rss_b > max_rss_b)) && max_rss_b="${rss_b}"
            ((fds_a > max_fds_a)) && max_fds_a="${fds_a}"
            ((fds_b > max_fds_b)) && max_fds_b="${fds_b}"
            ((tasks_a > max_tasks_a)) && max_tasks_a="${tasks_a}"
            ((tasks_b > max_tasks_b)) && max_tasks_b="${tasks_b}"
            sample_count=$((sample_count + 1))
            process_alive "${watched_pid}" || break
            sleep 1
        done
        printf 'RESOURCE_PEAK label=%s samples=%s a_rss_kib=%s a_fds=%s a_tasks=%s b_rss_kib=%s b_fds=%s b_tasks=%s\n' \
            "${label}" "${sample_count}" "${max_rss_a}" "${max_fds_a}" \
            "${max_tasks_a}" "${max_rss_b}" "${max_fds_b}" "${max_tasks_b}" \
            | tee -a "${run_dir}/resources.log"
    ) >"${sampler_log}" 2>&1 &
    resource_sampler_pid=$!
    track_pid "${resource_sampler_pid}"
}

run_sensor_partition() {
    local label="$1"
    local service="$2"
    local subscriber_log="${run_dir}/${label}-subscriber.log"
    local publisher_log="${run_dir}/${label}-publisher.log"
    host_b "${probe}" sensor-sub --service "${service}" --count 360 \
        --timeout-ms 30000 >"${subscriber_log}" 2>&1 &
    local sensor_subscriber_pid=$!
    track_pid "${sensor_subscriber_pid}"
    sleep 0.15
    host_a "${probe}" sensor-pub --service "${service}" --count 360 \
        --pace-ms 33 --timeout-ms 30000 >"${publisher_log}" 2>&1 &
    local sensor_publisher_pid=$!
    track_pid "${sensor_publisher_pid}"

    local ready_status=0
    wait_for_line "${subscriber_log}" '^READY ' 10 || ready_status=1
    wait_for_line "${publisher_log}" '^READY ' 10 || ready_status=1
    sleep 2
    local down_epoch_ms up_epoch_ms
    down_epoch_ms="$(( $(date +%s%N) / 1000000 ))"
    ip -n hosta link set vetha down
    sleep 5
    up_epoch_ms="$(( $(date +%s%N) / 1000000 ))"
    ip -n hosta link set vetha up

    local subscriber_status=0 publisher_status=0
    wait "${sensor_subscriber_pid}" || subscriber_status=$?
    wait "${sensor_publisher_pid}" || publisher_status=$?

    local last_pre='' last_sent='' first_post=''
    last_pre="$(awk -v cutoff="${down_epoch_ms}" '
        function value(prefix,    i, field) {
            for (i = 1; i <= NF; i++) {
                field = $i
                if (field ~ ("^" prefix "=")) {
                    sub("^" prefix "=", "", field)
                    return field
                }
            }
            return ""
        }
        /^SENSOR_SAMPLE / {
            sequence = value("sequence")
            timestamp = value("realtime_ms")
            if (sequence ~ /^[0-9]+$/ && timestamp ~ /^[0-9]+$/ \
                && timestamp < cutoff \
                && (!found || timestamp >= best_timestamp)) {
                found = 1
                best_sequence = sequence
                best_timestamp = timestamp
            }
        }
        END { if (found) print best_sequence "|" best_timestamp }
    ' "${subscriber_log}")"
    last_sent="$(awk -v cutoff="${up_epoch_ms}" '
        function value(prefix,    i, field) {
            for (i = 1; i <= NF; i++) {
                field = $i
                if (field ~ ("^" prefix "=")) {
                    sub("^" prefix "=", "", field)
                    return field
                }
            }
            return ""
        }
        /^SENSOR_SEND / {
            sequence = value("sequence")
            timestamp = value("realtime_ms")
            if (sequence ~ /^[0-9]+$/ && timestamp ~ /^[0-9]+$/ \
                && timestamp < cutoff \
                && (!found || timestamp >= best_timestamp)) {
                found = 1
                best_sequence = sequence
                best_timestamp = timestamp
            }
        }
        END { if (found) print best_sequence "|" best_timestamp }
    ' "${publisher_log}")"
    first_post="$(awk -v cutoff="${up_epoch_ms}" '
        function value(prefix,    i, field) {
            for (i = 1; i <= NF; i++) {
                field = $i
                if (field ~ ("^" prefix "=")) {
                    sub("^" prefix "=", "", field)
                    return field
                }
            }
            return ""
        }
        /^SENSOR_SAMPLE / {
            sequence = value("sequence")
            timestamp = value("realtime_ms")
            if (sequence ~ /^[0-9]+$/ && timestamp ~ /^[0-9]+$/ \
                && timestamp >= cutoff \
                && (!found || timestamp < best_timestamp)) {
                found = 1
                best_sequence = sequence
                best_timestamp = timestamp
            }
        }
        END { if (found) print best_sequence "|" best_timestamp }
    ' "${subscriber_log}")"

    local last_pre_sequence='' last_pre_realtime_ms=''
    local last_sent_sequence='' last_sent_realtime_ms=''
    local first_post_sequence='' first_post_realtime_ms=''
    if [[ -n "${last_pre}" ]]; then
        IFS='|' read -r last_pre_sequence last_pre_realtime_ms <<<"${last_pre}"
    fi
    if [[ -n "${last_sent}" ]]; then
        IFS='|' read -r last_sent_sequence last_sent_realtime_ms <<<"${last_sent}"
    fi
    if [[ -n "${first_post}" ]]; then
        IFS='|' read -r first_post_sequence first_post_realtime_ms <<<"${first_post}"
    fi
    local stale_replay_count=''
    if [[ "${last_sent_sequence}" =~ ^[0-9]+$ ]]; then
        stale_replay_count="$(awk -v cutoff="${up_epoch_ms}" -v sent="${last_sent_sequence}" '
            function value(prefix,    i, field) {
                for (i = 1; i <= NF; i++) {
                    field = $i
                    if (field ~ ("^" prefix "=")) {
                        sub("^" prefix "=", "", field)
                        return field
                    }
                }
                return ""
            }
            /^SENSOR_SAMPLE / {
                sequence = value("sequence")
                timestamp = value("realtime_ms")
                if (sequence ~ /^[0-9]+$/ && timestamp ~ /^[0-9]+$/ \
                    && timestamp >= cutoff && sequence < sent) {
                    stale += 1
                }
            }
            END { print stale + 0 }
        ' "${subscriber_log}")"
    fi

    local monotonic_status=FAIL
    local publisher_summary_ok=0 subscriber_summary_ok=0
    if grep -Eq '^SUMMARY .*kind=sensor .*sent=360([[:space:]]|$)' \
        "${publisher_log}"; then
        publisher_summary_ok=1
    fi
    if grep -Eq '^SUMMARY .*kind=sensor .*last=359([[:space:]]|$)' \
        "${subscriber_log}"; then
        subscriber_summary_ok=1
    fi
    if ((publisher_summary_ok == 1 && subscriber_summary_ok == 1)); then
        monotonic_status=PASS
    fi
    local parse_status=PASS recovery_gap=''
    if [[ ! "${last_pre_sequence}" =~ ^[0-9]+$ \
        || ! "${last_pre_realtime_ms}" =~ ^[0-9]+$ \
        || ! "${last_sent_sequence}" =~ ^[0-9]+$ \
        || ! "${last_sent_realtime_ms}" =~ ^[0-9]+$ \
        || ! "${first_post_sequence}" =~ ^[0-9]+$ \
        || ! "${first_post_realtime_ms}" =~ ^[0-9]+$ \
        || ! "${stale_replay_count}" =~ ^[0-9]+$ ]]; then
        parse_status=FAIL
    else
        recovery_gap=$((first_post_sequence - last_pre_sequence))
    fi
    local monotonic_sequence_status=FAIL
    if [[ "${parse_status}" == PASS && "${monotonic_status}" == PASS \
        && "${first_post_sequence}" -gt "${last_pre_sequence}" \
        && "${stale_replay_count}" -le 4 \
        && "${ready_status}" -eq 0 && "${subscriber_status}" -eq 0 \
        && "${publisher_status}" -eq 0 ]]; then
        monotonic_sequence_status=PASS
    fi
    printf 'SENSOR_RECOVERY label=%s service=%s down_epoch_ms=%s up_epoch_ms=%s last_pre_sequence=%s last_pre_realtime_ms=%s last_sent_before_up_sequence=%s last_sent_before_up_realtime_ms=%s first_post_sequence=%s first_post_realtime_ms=%s stale_replay_count=%s recovery_gap=%s monotonic=%s probe_status=subscriber:%s,publisher:%s parse_status=%s\n' \
        "${label}" "${service}" "${down_epoch_ms}" "${up_epoch_ms}" \
        "${last_pre_sequence:-unknown}" "${last_pre_realtime_ms:-unknown}" \
        "${last_sent_sequence:-unknown}" "${last_sent_realtime_ms:-unknown}" \
        "${first_post_sequence:-unknown}" "${first_post_realtime_ms:-unknown}" \
        "${stale_replay_count:-unknown}" "${recovery_gap:-unknown}" \
        "${monotonic_sequence_status}" "${subscriber_status}" "${publisher_status}" \
        "${parse_status}" | tee -a "${run_dir}/gates.log"
    if [[ "${monotonic_sequence_status}" == PASS ]]; then
        printf 'PASS %s\n' "${label}" \
            | tee -a "${run_dir}/gates.log"
    else
        printf 'XFAIL %s sensor-partition-backlog parse-order-or-stale-replay-limit\n' "${label}" \
            | tee -a "${run_dir}/gates.log"
    fi
}

start_tunnels baseline --poll 20
run_probe_pair plain-u64 \
    fixed-sub --service rbt/cross-host-upstream/verify/u64 --count 8 --timeout-ms 20000 -- \
    fixed-pub --service rbt/cross-host-upstream/verify/u64 --count 8 --timeout-ms 20000
run_probe_pair plain-bytes-low-rate \
    bytes-sub --service rbt/cross-host-upstream/verify/bytes-low-rate \
        --payload-bytes 1024 --count 3 --timeout-ms 20000 -- \
    bytes-pub --service rbt/cross-host-upstream/verify/bytes-low-rate \
        --payload-bytes 1024 --count 3 --timeout-ms 20000
run_probe_pair plain-event \
    event-listen --service rbt/cross-host-upstream/verify/event --count 7 --timeout-ms 20000 -- \
    event-notify --service rbt/cross-host-upstream/verify/event --count 7 --timeout-ms 20000

selective_prefix="rbt/cross-host-upstream/verify/selective-${mode}"
selective_service_a="${selective_prefix}/a"
selective_service_b="${selective_prefix}/b"
selective_service_c="${selective_prefix}/c"
host_a "${probe}" provider-hold --service "${selective_service_a}" \
    --duration-ms 120000 --timeout-ms 20000 >"${run_dir}/selective-a-provider.log" 2>&1 &
selective_provider_a=$!
track_pid "${selective_provider_a}"
host_a "${probe}" provider-hold --service "${selective_service_b}" \
    --duration-ms 120000 --timeout-ms 20000 >"${run_dir}/selective-b-provider.log" 2>&1 &
selective_provider_b=$!
track_pid "${selective_provider_b}"
host_a "${probe}" provider-hold --service "${selective_service_c}" \
    --duration-ms 120000 --timeout-ms 20000 >"${run_dir}/selective-c-provider.log" 2>&1 &
selective_provider_c=$!
track_pid "${selective_provider_c}"
wait_for_line "${run_dir}/selective-a-provider.log" '^READY ' 10
wait_for_line "${run_dir}/selective-b-provider.log" '^READY ' 10
wait_for_line "${run_dir}/selective-c-provider.log" '^READY ' 10
wait_service_state "${selective_service_a}" present 15 "${run_dir}/selective-a-present.json" \
    || die 'selective-removal provider A was not visible'
wait_service_state "${selective_service_b}" present 15 "${run_dir}/selective-b-present.json" \
    || die 'selective-removal provider B was not visible'
wait_service_state "${selective_service_c}" present 15 "${run_dir}/selective-c-present.json" \
    || die 'selective-removal provider C was not visible'
stop_pid "${selective_provider_b}"
wait_service_state "${selective_service_b}" absent 15 "${run_dir}/selective-b-absent.json" \
    || die 'selective-removal provider B mirror remained visible'
wait_service_state "${selective_service_a}" present 5 "${run_dir}/selective-a-retained.json" \
    || die 'selective-removal provider A mirror was lost'
wait_service_state "${selective_service_c}" present 5 "${run_dir}/selective-c-retained.json" \
    || die 'selective-removal provider C mirror was lost'
printf 'PASS selective-removal removed=%s retained=%s,%s\n' \
    "${selective_service_b}" "${selective_service_a}" "${selective_service_c}" \
    | tee -a "${run_dir}/gates.log"
stop_pid "${selective_provider_a}"
stop_pid "${selective_provider_c}"

# Short same-name churn is part of the quick gate. A 50ms hold and gap are
# longer than the 20ms poll interval, so the provider is observable while the
# final absence still tests withdrawal cleanup.
quick_churn_prefix="rbt/cross-host-upstream/verify/churn/quick"
run_observed_churn churn-quick same "${quick_churn_prefix}" 5 50 50 10000 10000

# Provider withdrawal and remote-tunnel withdrawal are checked from the
# observer host. The provider command owns the local service until killed;
# tunnel A is then restarted while the provider remains alive.
lifecycle_service="rbt/cross-host-upstream/verify/lifecycle-${mode}"
host_a "${probe}" provider-hold --service "${lifecycle_service}" \
    --duration-ms 120000 --timeout-ms 20000 >"${run_dir}/lifecycle-provider.log" 2>&1 &
lifecycle_provider=$!
track_pid "${lifecycle_provider}"
wait_for_line "${run_dir}/lifecycle-provider.log" '^READY ' 10
wait_service_state "${lifecycle_service}" present 15 "${run_dir}/lifecycle-present.json"
provider_withdraw_started_ns="$(date +%s%N)"
stop_pid "${lifecycle_provider}"
wait_service_state "${lifecycle_service}" absent 15 "${run_dir}/lifecycle-after-provider.json" \
    || die 'remote service remained visible after provider withdrawal'
provider_withdraw_finished_ns="$(date +%s%N)"
printf 'LIFECYCLE_TIMING kind=provider-withdrawal elapsed_ms=%s\n' \
    "$(((provider_withdraw_finished_ns - provider_withdraw_started_ns) / 1000000))" \
    | tee -a "${run_dir}/gates.log"
printf 'PASS lifecycle-provider-withdrawal\n' | tee -a "${run_dir}/gates.log"

host_a "${probe}" provider-hold --service "${lifecycle_service}" \
    --duration-ms 120000 --timeout-ms 20000 >"${run_dir}/lifecycle-provider-restart.log" 2>&1 &
lifecycle_provider=$!
track_pid "${lifecycle_provider}"
wait_for_line "${run_dir}/lifecycle-provider-restart.log" '^READY ' 10
wait_service_state "${lifecycle_service}" present 15 "${run_dir}/lifecycle-represent.json"
gateway_withdraw_started_ns="$(date +%s%N)"
stop_pid "${tunnel_a}"
wait_service_state "${lifecycle_service}" absent 15 "${run_dir}/lifecycle-after-tunnel.json" \
    || die 'remote service remained visible after tunnel/session loss'
gateway_withdraw_finished_ns="$(date +%s%N)"
printf 'LIFECYCLE_TIMING kind=tunnel-loss-withdrawal elapsed_ms=%s\n' \
    "$(((gateway_withdraw_finished_ns - gateway_withdraw_started_ns) / 1000000))" \
    | tee -a "${run_dir}/gates.log"
start_tunnel_a baseline-restart --poll 20
sleep 1
wait_service_state "${lifecycle_service}" present 20 "${run_dir}/lifecycle-after-restart.json" \
    || die 'remote service did not recover after tunnel restart'
printf 'PASS lifecycle-tunnel-loss-restart\n' | tee -a "${run_dir}/gates.log"

stop_pid "${tunnel_b}"
wait_service_state "${lifecycle_service}" absent 15 "${run_dir}/lifecycle-after-hostb-tunnel.json" \
    || die 'remote service remained visible after Host B tunnel stop'
start_tunnel_b lifecycle-hostb-restart --poll 20
sleep 1
wait_service_state "${lifecycle_service}" present 20 "${run_dir}/lifecycle-after-hostb-restart.json" \
    || die 'remote service did not recover after Host B tunnel restart'
printf 'PASS lifecycle-hostb-tunnel-restart\n' | tee -a "${run_dir}/gates.log"

# Partition and recovery use a provider that remains alive throughout the
# link flap. Polled discovery makes the expected withdrawal observable.
partition_service="rbt/cross-host-upstream/verify/partition-${mode}"
host_a "${probe}" provider-hold --service "${partition_service}" \
    --duration-ms 120000 --timeout-ms 20000 >"${run_dir}/partition-provider.log" 2>&1 &
partition_provider=$!
track_pid "${partition_provider}"
wait_for_line "${run_dir}/partition-provider.log" '^READY ' 10
wait_service_state "${partition_service}" present 15 "${run_dir}/partition-present.json"
partition_down_started_ns="$(date +%s%N)"
ip -n hosta link set vetha down
wait_service_state "${partition_service}" absent 15 "${run_dir}/partition-down.json" \
    || die 'remote service remained visible during network partition'
partition_down_finished_ns="$(date +%s%N)"
printf 'LIFECYCLE_TIMING kind=network-partition-withdrawal elapsed_ms=%s\n' \
    "$(((partition_down_finished_ns - partition_down_started_ns) / 1000000))" \
    | tee -a "${run_dir}/gates.log"
partition_recovery_started_ns="$(date +%s%N)"
ip -n hosta link set vetha up
wait_service_state "${partition_service}" present 20 "${run_dir}/partition-up.json" \
    || die 'remote service did not recover after network partition'
partition_recovery_finished_ns="$(date +%s%N)"
printf 'LIFECYCLE_TIMING kind=network-partition-recovery elapsed_ms=%s\n' \
    "$(((partition_recovery_finished_ns - partition_recovery_started_ns) / 1000000))" \
    | tee -a "${run_dir}/gates.log"
printf 'PASS network-partition-withdrawal-recovery\n' | tee -a "${run_dir}/gates.log"

# The allow-list is exercised in isolation, then the baseline tunnel is
# restored for the remaining gates. Rules are exact and repeated for the
# payload and readiness names; no wildcard may accidentally export private
# services.
stop_pid "${tunnel_a}"
stop_pid "${tunnel_b}"
allow_payload_service="rbt/cross-host-upstream/allow/exact-payload-${mode}"
allow_ready_service="${allow_payload_service}/ready"
allow_private_internal="rbt/cross-host-upstream/private/internal-${mode}"
allow_private_cancellation="rbt/cross-host-upstream/private/cancellation-${mode}"
allow_private_test="rbt/cross-host-upstream/private/test-${mode}"
start_tunnels allowlist-exact --allow "${allow_payload_service}" \
    --allow "${allow_ready_service}" --poll 20
host_a "${probe}" provider-hold --service "${allow_payload_service}" \
    --duration-ms 60000 --timeout-ms 20000 >"${run_dir}/allow-payload-provider.log" 2>&1 &
allow_payload_provider=$!
track_pid "${allow_payload_provider}"
host_a "${probe}" event-notify --service "${allow_ready_service}" --count 1 \
    --timeout-ms 20000 >"${run_dir}/allow-ready-provider.log" 2>&1 &
allow_ready_provider=$!
track_pid "${allow_ready_provider}"
host_a "${probe}" provider-hold --service "${allow_private_internal}" \
    --duration-ms 60000 --timeout-ms 20000 >"${run_dir}/private-internal-provider.log" 2>&1 &
allow_private_internal_provider=$!
track_pid "${allow_private_internal_provider}"
host_a "${probe}" provider-hold --service "${allow_private_cancellation}" \
    --duration-ms 60000 --timeout-ms 20000 >"${run_dir}/private-cancellation-provider.log" 2>&1 &
allow_private_cancellation_provider=$!
track_pid "${allow_private_cancellation_provider}"
host_a "${probe}" provider-hold --service "${allow_private_test}" \
    --duration-ms 60000 --timeout-ms 20000 >"${run_dir}/private-test-provider.log" 2>&1 &
allow_private_test_provider=$!
track_pid "${allow_private_test_provider}"
wait_for_line "${run_dir}/allow-payload-provider.log" '^READY ' 10
wait_for_line "${run_dir}/allow-ready-provider.log" '^READY ' 10
wait_for_line "${run_dir}/private-internal-provider.log" '^READY ' 10
wait_for_line "${run_dir}/private-cancellation-provider.log" '^READY ' 10
wait_for_line "${run_dir}/private-test-provider.log" '^READY ' 10
wait_service_state "${allow_payload_service}" present 15 "${run_dir}/allow-payload-present.json" \
    || die 'allow-listed payload service was not exported'
wait_service_state "${allow_ready_service}" present 15 "${run_dir}/allow-ready-present.json" \
    || die 'allow-listed readiness service was not exported'
for private_service in "${allow_private_internal}" "${allow_private_cancellation}" "${allow_private_test}"; do
    if wait_service_state "${private_service}" present 3 \
        "${run_dir}/$(basename "${private_service}")-private-present.json"; then
        die "non-allow-listed private service was exported: ${private_service}"
    fi
done
printf 'PASS allowlist-exact-payload-and-ready private_absent=3\n' \
    | tee -a "${run_dir}/gates.log"

# Keep both local providers alive while replacing the tunnel pair. Allowing
# only the payload demonstrates the observable half-filter: its mirror returns
# while the separately named readiness service remains absent.
stop_pid "${tunnel_a}"
stop_pid "${tunnel_b}"
start_tunnels allowlist-payload-only --allow "${allow_payload_service}" --poll 20
wait_service_state "${allow_payload_service}" present 15 \
    "${run_dir}/allow-payload-only-present.json" \
    || die 'payload-only allow-list did not export payload service'
if wait_service_state "${allow_ready_service}" present 3 \
    "${run_dir}/allow-ready-only-present.json"; then
    die 'payload-only allow-list exported readiness service'
fi
printf 'PASS allowlist-half-filter payload_present=1 ready_absent=1\n' \
    | tee -a "${run_dir}/gates.log"
stop_pid "${allow_payload_provider}"
stop_pid "${allow_ready_provider}"
stop_pid "${allow_private_internal_provider}"
stop_pid "${allow_private_cancellation_provider}"
stop_pid "${allow_private_test_provider}"
stop_pid "${tunnel_a}"
stop_pid "${tunnel_b}"
start_tunnels baseline-restored --poll 20

# A tunnel with no poll and no local listener has no local-discovery wake
# source. It must remain bounded (we stop it ourselves) and is recorded as a
# known limitation rather than treated as a transport failure.
stop_pid "${tunnel_a}"
stop_pid "${tunnel_b}"
start_tunnel_a pure-reactive --reactive
sleep 0.5
start_tunnel_b pure-reactive --reactive
sleep 1
if process_alive "${tunnel_a}" && process_alive "${tunnel_b}"; then
    snapshot_tunnel pure-reactive-idle-start-a "${tunnel_a}"
    snapshot_tunnel pure-reactive-idle-start-b "${tunnel_b}"
    sleep 10
    snapshot_tunnel pure-reactive-idle-after-10s-a "${tunnel_a}"
    snapshot_tunnel pure-reactive-idle-after-10s-b "${tunnel_b}"
    reactive_service="rbt/cross-host-upstream/verify/reactive/idle-${mode}"
    host_a "${probe}" provider-hold --service "${reactive_service}" \
        --duration-ms 10000 --timeout-ms 5000 >"${run_dir}/reactive-provider.log" 2>&1 &
    reactive_provider=$!
    track_pid "${reactive_provider}"
    wait_for_line "${run_dir}/reactive-provider.log" '^READY ' 5
    if wait_service_state "${reactive_service}" present 3 "${run_dir}/reactive-present.json"; then
        printf 'PASS pure-reactive-local-discovery-wake\n' | tee -a "${run_dir}/gates.log"
    else
        printf 'XFAIL pure-reactive-idle expected-no-local-discovery-wake bounded=1\n' \
            | tee -a "${run_dir}/gates.log"
    fi
    stop_pid "${reactive_provider}"
else
    die 'pure-reactive tunnel exited unexpectedly while idle'
fi
stop_pid "${tunnel_a}"
stop_pid "${tunnel_b}"
run_preexisting_reactive_pair preexisting-reactive-fixed-u64 yes \
    fixed-sub --service "rbt/cross-host-upstream/verify/reactive/preexisting-u64-${mode}" \
        --count 8 --timeout-ms 30000 -- \
    fixed-pub --service "rbt/cross-host-upstream/verify/reactive/preexisting-u64-${mode}" \
        --count 8 --timeout-ms 30000
stop_pid "${tunnel_a}"
stop_pid "${tunnel_b}"
start_tunnels hybrid-reactive --reactive --poll 1000
run_probe_pair sidecar-spurious-wake \
        sidecar-sub --service rbt/cross-host-upstream/verify/sidecar/spurious-wake \
        --count 1 --payload-bytes 64 --ready-prefill 1 \
        --ready-prefill-gap-ms 2000 --timeout-ms 20000 -- \
    sidecar-pub --service rbt/cross-host-upstream/verify/sidecar/spurious-wake \
        --count 1 --payload-bytes 64 --ready-prefill 1 \
        --ready-prefill-gap-ms 2000 --pace-ms 30 --timeout-ms 20000
spurious_wake_samples="$(sidecar_field \
    "${run_dir}/sidecar-spurious-wake-subscriber.log" samples)"
spurious_wakes="$(sidecar_field \
    "${run_dir}/sidecar-spurious-wake-subscriber.log" spurious_wakes)"
if [[ ! "${spurious_wake_samples}" =~ ^[0-9]+$ \
    || "${spurious_wake_samples}" -ne 1 \
    || ! "${spurious_wakes}" =~ ^[0-9]+$ \
    || "${spurious_wakes}" -lt 1 ]]; then
    die "sidecar-spurious-wake expected samples=1 and spurious_wakes>=1; samples=${spurious_wake_samples:-unknown} spurious_wakes=${spurious_wakes:-unknown}"
fi
printf 'PASS sidecar-spurious-wake samples=%s spurious_wakes=%s\n' \
    "${spurious_wake_samples}" "${spurious_wakes}" \
    | tee -a "${run_dir}/gates.log"
run_probe_pair hybrid-reactive-sidecar \
        sidecar-sub --service rbt/cross-host-upstream/verify/sidecar/basic \
        --count 20 --payload-bytes 64 --timeout-ms 30000 -- \
    sidecar-pub --service rbt/cross-host-upstream/verify/sidecar/basic \
        --count 20 --payload-bytes 64 --pace-ms 30 --timeout-ms 30000
set +e
check_sidecar_pair hybrid-reactive-sidecar 20
hybrid_sidecar_status=$?
set -e
if ((hybrid_sidecar_status == 0)); then
    printf 'PASS hybrid-reactive-low-poll\n' | tee -a "${run_dir}/gates.log"
else
    printf 'XFAIL hybrid-reactive-sidecar-progress-loss-observed status=%s\n' \
        "${hybrid_sidecar_status}" | tee -a "${run_dir}/gates.log"
    printf 'XFAIL hybrid-reactive-low-poll-sidecar-progress\n' \
        | tee -a "${run_dir}/gates.log"
fi

if [[ "${mode}" == full ]]; then
    stop_pid "${tunnel_a}"
    stop_pid "${tunnel_b}"
    run_preexisting_reactive_pair preexisting-reactive-image-1080p no \
        image-sub --service rbt/cross-host-upstream/verify/reactive/preexisting-image-${mode} \
            --layout 1920x1080-rgb8 --count 3 --timeout-ms 30000 -- \
        image-pub --service rbt/cross-host-upstream/verify/reactive/preexisting-image-${mode} \
            --layout 1920x1080-rgb8 --count 3 --pace-ms 30 --timeout-ms 30000
    stop_pid "${tunnel_a}"
    stop_pid "${tunnel_b}"

    # Full churn uses a fast polling tunnel and deliberately observable
    # lifetimes. Restart both directions first so the pure-reactive/hybrid
    # experiments cannot leave an older tunnel attached to the namespace.
    stop_pid "${tunnel_a}"
    stop_pid "${tunnel_b}"
    start_tunnels full-churn --poll 20
    snapshot_tunnel full-churn-before-a "${tunnel_a}"
    snapshot_tunnel full-churn-before-b "${tunnel_b}"

    run_observed_churn churn-same same rbt/cross-host-upstream/churn/same 100 50 50 60000 30000
    run_observed_churn churn-unique unique rbt/cross-host-upstream/churn/unique 1000 50 50 150000 130000

    snapshot_tunnel full-churn-after-a "${tunnel_a}"
    snapshot_tunnel full-churn-after-b "${tunnel_b}"
    for _cycle in $(seq 1 10); do
        ip -n hosta link set vetha down
        sleep 0.05
        ip -n hosta link set vetha up
        sleep 0.05
    done
    sleep 2
    snapshot_tunnel reconnect-after-a "${tunnel_a}"
    snapshot_tunnel reconnect-after-b "${tunnel_b}"
    printf 'PASS reconnect-resource-snapshots cycles=10\n' | tee -a "${run_dir}/gates.log"

    run_sensor_partition sensor-partition-backlog \
        rbt/cross-host-upstream/verify/sensor/partition-${mode}

    run_probe_pair sidecar-10000 \
        sidecar-sub --service rbt/cross-host-upstream/verify/sidecar/ten-k \
            --count 10000 --payload-bytes 64 --timeout-ms 30000 -- \
        sidecar-pub --service rbt/cross-host-upstream/verify/sidecar/ten-k \
            --count 10000 --payload-bytes 64 --timeout-ms 30000
    set +e
    check_sidecar_pair sidecar-10000 10000
    high_rate_status=$?
    set -e
    if ((high_rate_status != 0)); then
        printf 'XFAIL sidecar-10000-progress-loss-observed\n' \
            | tee -a "${run_dir}/gates.log"
    fi

    # The final-ready trap intentionally withholds the last Event activation
    # for a paced 20 MiB payload. With ready-prefill=0, the first readiness
    # wake is deterministic; the final payload arrives after that drain and
    # exposes whether progress depends on a missing final wake. A pass is
    # useful evidence; the known bounded-queue failure is retained as XFAIL.
    set +e
    run_probe_pair sidecar-final-trap \
        sidecar-sub --service rbt/cross-host-upstream/verify/sidecar/final-trap \
            --count 2 --payload-bytes 20971520 --ready-prefill 0 \
            --timeout-ms 30000 -- \
        sidecar-pub --service rbt/cross-host-upstream/verify/sidecar/final-trap \
            --count 2 --payload-bytes 20971520 --ready-prefill 0 \
            --pace-ms 100 --suppress-final-ready --timeout-ms 30000
    trap_status=$?
    set -e
    if ((trap_status != 0)); then
        printf 'XFAIL sidecar-final-ready-overload-trap command-status=%s\n' \
            "${trap_status}" | tee -a "${run_dir}/gates.log"
    else
        set +e
        check_sidecar_pair sidecar-final-trap 2
        trap_summary_status=$?
        set -e
        if ((trap_summary_status == 0)); then
            printf 'PASS sidecar-final-ready-overload-trap\n' \
                | tee -a "${run_dir}/gates.log"
        else
            printf 'XFAIL sidecar-final-ready-overload-trap bounded-queue-progress-lost\n' \
                | tee -a "${run_dir}/gates.log"
        fi
    fi

    run_probe_pair image-1080p \
        image-sub --service rbt/cross-host-upstream/verify/image-1080p \
            --layout 1920x1080-rgb8 --count 3 --timeout-ms 90000 -- \
        image-pub --service rbt/cross-host-upstream/verify/image-1080p \
            --layout 1920x1080-rgb8 --count 3 --pace-ms 30 --timeout-ms 90000
    run_probe_pair pointcloud-20m \
        pointcloud-sub --service rbt/cross-host-upstream/verify/pointcloud-20m \
            --points 1310720 --count 1 --timeout-ms 120000 -- \
        pointcloud-pub --service rbt/cross-host-upstream/verify/pointcloud-20m \
            --points 1310720 --count 1 --pace-ms 0 --timeout-ms 120000

    # Slow subscriber/fast publisher records latest-progress behavior. This
    # is a diagnostic gate: successful completion with selected checks is
    # evidence of bounded progress, not a lossless QoS claim.
    run_probe_pair sidecar-latest-progress \
        sidecar-sub --service rbt/cross-host-upstream/verify/sidecar/latest \
            --count 200 --payload-bytes 1024 --timeout-ms 30000 -- \
        sidecar-pub --service rbt/cross-host-upstream/verify/sidecar/latest \
            --count 200 --payload-bytes 1024 --pace-ms 1 --timeout-ms 30000
    set +e
    check_sidecar_pair sidecar-latest-progress 200
    latest_progress_status=$?
    set -e
    if ((latest_progress_status != 0)); then
        printf 'XFAIL sidecar-latest-progress-bounded-queue\n' \
            | tee -a "${run_dir}/gates.log"
    fi

    run_expected_schema_mismatch rbt/cross-host-upstream/verify/schema-mismatch
fi

printf 'SKIP thin-proxy-composer reason=G6-reactive-local-discovery-and-G7-bounded-progress-gates-not-proven\n' \
    | tee -a "${run_dir}/gates.log"
printf 'CROSS_HOST_NAMESPACE_RESULT mode=%s transport=PASS known_limitations_logged=1\n' \
    "${mode}" | tee -a "${run_dir}/gates.log"
NAMESPACE_VERIFY
namespace_status=$?
set -e

if ((namespace_status != 0)); then
    die "namespace verification failed with exit ${namespace_status}; see ${run_dir}"
fi

run_logged production-iox2-source-post-check ./scripts/bootstrap_iceoryx2_rbt.sh --source-only
production_hash_after="$(sha256sum "${production_files[@]}")"
[[ "${production_hash_before}" == "${production_hash_after}" ]] \
    || die 'production manifests, ABI source, or unified fork fingerprint changed during upstream verification'

leftovers="$(ps -eo pid=,args= \
    | grep -E 'iox2-link-tunnel-zenoh|cross-host-upstream-probe' \
    | grep -v grep || true)"
if [[ -n "${leftovers}" ]]; then
    printf '%s\n' "${leftovers}" >"${run_dir}/leftovers.log"
    die "leftover upstream verification processes detected; see ${run_dir}/leftovers.log"
fi

printf 'CROSS_HOST_UPSTREAM_VERIFY_RESULT mode=%s execution=PASS critical_gates=PARTIAL product_unchanged=PASS logs=%s\n' \
    "${mode}" "${run_dir}" | tee "${run_dir}/result.log"

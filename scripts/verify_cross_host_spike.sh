#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

usage() {
    printf '%s\n' \
        'usage: scripts/verify_cross_host_spike.sh --quick|--full' \
        '' \
        '--quick  Run the minimum two-network-stack functional gate and reproduce the liveness blocker.' \
        '--full   Add all payload shapes, impairment, stress, pressure, and performance diagnostics.'
}

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

# usage: scripts/verify_cross_host_spike.sh --quick|--full
#
# --quick  Reproduce the minimum two-network-stack transport gate, one
#          disconnect/reconnect, ABI rejection, and the known remote-liveness
#          blocker.
# --full   Add all payload shapes, a 20 MiB PointCloud, controlled netem,
#          reconnect stress, a slow Sensor diagnostic, and the local/cross-host
#          performance matrix. Productization-only work is intentionally not
#          run after the remote-liveness hard gate fails.
#
# Environment overrides:
#   RBT_PHASE4_TOOLCHAIN       compiler/toolchain root
#   RBT_IOX2_093_SOURCE        exact iceoryx2 v0.9.3 checkout
#   RBT_CROSS_HOST_LOG_ROOT    retained evidence root

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

for command_name in awk cargo date find git ip nsenter ps rg sed seq tc tee unshare wc; do
    require_command "${command_name}"
done

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
iox2_source="${RBT_IOX2_093_SOURCE:-${repo_root}/target/iceoryx2-v0.9.3}"
log_root="${RBT_CROSS_HOST_LOG_ROOT:-${repo_root}/target/cross-host-verify-logs}"
toolchain_bin="${toolchain_root}/bin"

[[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-gcc" ]] \
    || die "Phase4 C compiler is missing under ${toolchain_bin}"
[[ -f "${iox2_source}/integrations/Cargo.lock" ]] \
    || die "exact iceoryx2 integration checkout is missing: ${iox2_source}"
[[ "$(git -C "${iox2_source}" rev-parse HEAD)" == \
    6ef7c55176a8487ae7ca53c2e0649d5df9a1b3b4 ]] \
    || die "iceoryx2 checkout is not the audited v0.9.3 commit"

export PATH="${toolchain_bin}:${PATH}"
export CC="${toolchain_bin}/x86_64-conda-linux-gnu-gcc"
export CXX="${toolchain_bin}/x86_64-conda-linux-gnu-g++"
export AR="${toolchain_bin}/x86_64-conda-linux-gnu-ar"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"

mkdir -p "${log_root}"
run_id="cross-host-${mode}-$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="${log_root}/${run_id}"
mkdir -p "${run_dir}"

run_logged() {
    local label="$1"
    shift
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    if ! "$@" >"${run_dir}/${label}.log" 2>&1; then
        sed -n '1,240p' "${run_dir}/${label}.log" >&2
        die "${label} failed; see ${run_dir}/${label}.log"
    fi
}

printf 'CROSS_HOST_VERIFY run_id=%s mode=%s logs=%s\n' \
    "${run_id}" "${mode}" "${run_dir}" | tee "${run_dir}/environment.log"
printf 'CROSS_HOST_VERIFY iox2_commit=%s kernel=%s\n' \
    "$(git -C "${iox2_source}" rev-parse HEAD)" "$(uname -srmo)" \
    | tee -a "${run_dir}/environment.log"

run_logged probe-fmt cargo fmt \
    --manifest-path experiments/cross-host/Cargo.toml -- --check
run_logged probe-test cargo test \
    --manifest-path experiments/cross-host/Cargo.toml --locked
run_logged probe-clippy cargo clippy \
    --manifest-path experiments/cross-host/Cargo.toml --locked \
    --all-targets -- -D warnings
run_logged probe-build cargo build \
    --manifest-path experiments/cross-host/Cargo.toml --locked
run_logged rbt-tools-build cargo build --locked \
    -p image-publisher -p image-consumer -p rbt-bench
run_logged tunnel-build cargo build \
    --manifest-path "${iox2_source}/integrations/Cargo.toml" --locked \
    -p iceoryx2-integrations-zenoh-tunnel-cli
run_logged iox2-service-build cargo build \
    --manifest-path "${iox2_source}/Cargo.toml" --locked \
    -p iceoryx2-cli --bin iox2-service

probe="${repo_root}/experiments/cross-host/target/debug/cross-host-probe"
tunnel="${iox2_source}/integrations/target/debug/iox2-tunnel-zenoh"
iox2_service="${iox2_source}/target/debug/iox2-service"
image_publisher="${repo_root}/target/debug/image-publisher"
image_consumer="${repo_root}/target/debug/image-consumer"
rbt_bench="${repo_root}/target/debug/rbt-bench"

run_logged abi-mismatch "${probe}" abi-mismatch \
    --service rbt/cross-host/verify/abi-mismatch
rg -q 'result=PASS.*AbiMismatch.*endpoints_opened=0' \
    "${run_dir}/abi-mismatch.log" \
    || die "ABI mismatch probe did not reject before endpoint creation"

# The outer namespace grants a scoped CAP_NET_ADMIN without changing host
# networking. The nested hosts share only a veth link; /tmp and /dev/shm are
# private per host so local iceoryx2 IPC cannot bypass the network path.
set +e
unshare --user --map-root-user --mount --net --pid --fork --mount-proc \
    bash -s -- "${mode}" "${run_dir}" "${tunnel}" "${probe}" \
    "${iox2_service}" "${image_publisher}" "${image_consumer}" \
    "${rbt_bench}" <<'NAMESPACE_TESTS'
set -euo pipefail

mode="$1"
run_dir="$2"
tunnel="$3"
probe="$4"
iox2_service="$5"
image_publisher="$6"
image_consumer="$7"
rbt_bench="$8"

managed_pids=()
track_pid() {
    managed_pids+=("$1")
}

stop_pid() {
    local pid="$1"
    local child
    local children=""
    if [[ -r "/proc/${pid}/task/${pid}/children" ]]; then
        children="$(<"/proc/${pid}/task/${pid}/children")"
    fi
    for child in ${children}; do
        stop_pid "${child}"
    done
    kill "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
}

cleanup() {
    local index
    set +e
    for ((index=${#managed_pids[@]} - 1; index >= 0; index--)); do
        stop_pid "${managed_pids[index]}"
    done
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
ip -n hosta addr add 10.77.42.1/24 dev vetha
ip -n hostb addr add 10.77.42.2/24 dev vethb
ip -n hosta link set vetha mtu 1500 up
ip -n hostb link set vethb mtu 1500 up

ip netns exec hosta unshare --mount --ipc --uts bash -c '
    set -e
    mount --make-rprivate /
    mount -t tmpfs tmpfs /dev/shm
    mount -t tmpfs tmpfs /tmp
    hostname hosta
    exec sleep 86400
' >"${run_dir}/hosta-anchor.log" 2>&1 &
hosta_anchor=$!
track_pid "${hosta_anchor}"

ip netns exec hostb unshare --mount --ipc --uts bash -c '
    set -e
    mount --make-rprivate /
    mount -t tmpfs tmpfs /dev/shm
    mount -t tmpfs tmpfs /tmp
    hostname hostb
    exec sleep 86400
' >"${run_dir}/hostb-anchor.log" 2>&1 &
hostb_anchor=$!
track_pid "${hostb_anchor}"
sleep 0.5

host_a() {
    nsenter --target "${hosta_anchor}" --mount --ipc --uts --net \
        --preserve-credentials -- "$@"
}
host_b() {
    nsenter --target "${hostb_anchor}" --mount --ipc --uts --net \
        --preserve-credentials -- "$@"
}

host_a "${tunnel}" --poll 1 >"${run_dir}/tunnel-a.log" 2>&1 &
tunnel_a=$!
track_pid "${tunnel_a}"
host_b "${tunnel}" --poll 1 >"${run_dir}/tunnel-b.log" 2>&1 &
tunnel_b=$!
track_pid "${tunnel_b}"
sleep 2

leaf_pid() {
    local pid="$1"
    local children=""
    while [[ -r "/proc/${pid}/task/${pid}/children" ]]; do
        children="$(<"/proc/${pid}/task/${pid}/children")"
        [[ -n "${children}" ]] || break
        # Each background host wrapper has one foreground nsenter child.
        pid="${children%% *}"
    done
    printf '%s\n' "${pid}"
}
tunnel_a_process="$(leaf_pid "${tunnel_a}")"
tunnel_b_process="$(leaf_pid "${tunnel_b}")"

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
    sleep 0.2
    host_a "${probe}" "${publisher_args[@]}" \
        >"${run_dir}/${label}-publisher.log" 2>&1 &
    local publisher_pid=$!
    track_pid "${publisher_pid}"

    local status=0
    wait "${publisher_pid}" || status=$?
    wait "${subscriber_pid}" || status=$?
    if ((status != 0)); then
        sed -n '1,200p' "${run_dir}/${label}-publisher.log" >&2
        sed -n '1,200p' "${run_dir}/${label}-subscriber.log" >&2
        return "${status}"
    fi
    rg -q '^SUMMARY ' "${run_dir}/${label}-publisher.log"
    rg -q '^SUMMARY ' "${run_dir}/${label}-subscriber.log"
    printf 'PASS %s\n' "${label}"
}

run_probe_pair plain-u64 \
    fixed-sub --service rbt/cross-host/verify/u64 --count 8 --timeout-ms 20000 -- \
    fixed-pub --service rbt/cross-host/verify/u64 --count 8 --timeout-ms 20000
run_probe_pair plain-event \
    event-listen --service rbt/cross-host/verify/event --count 7 --timeout-ms 20000 -- \
    event-notify --service rbt/cross-host/verify/event --count 7 --timeout-ms 20000
run_probe_pair dynamic-1k \
    bytes-sub --service rbt/cross-host/verify/bytes-1k --payload-bytes 1024 \
        --count 3 --timeout-ms 20000 -- \
    bytes-pub --service rbt/cross-host/verify/bytes-1k --payload-bytes 1024 \
        --count 3 --timeout-ms 20000
run_probe_pair image-1080p \
    image-sub --service rbt/cross-host/verify/image-1080p \
        --layout 1920x1080-rgb8 --count 100 --timeout-ms 60000 -- \
    image-pub --service rbt/cross-host/verify/image-1080p \
        --layout 1920x1080-rgb8 --count 100 --pace-ms 10 --timeout-ms 60000
run_probe_pair pointcloud-100000 \
    pointcloud-sub --service rbt/cross-host/verify/pointcloud-100000 \
        --points 100000 --count 1 --timeout-ms 30000 -- \
    pointcloud-pub --service rbt/cross-host/verify/pointcloud-100000 \
        --points 100000 --count 1 --pace-ms 0 --timeout-ms 30000

if [[ "${mode}" == full ]]; then
    run_probe_pair dynamic-empty \
        bytes-sub --service rbt/cross-host/verify/bytes-empty --payload-bytes 0 \
            --count 3 --timeout-ms 20000 -- \
        bytes-pub --service rbt/cross-host/verify/bytes-empty --payload-bytes 0 \
            --count 3 --timeout-ms 20000
    run_probe_pair dynamic-1m \
        bytes-sub --service rbt/cross-host/verify/bytes-1m --payload-bytes 1048576 \
            --count 3 --timeout-ms 30000 -- \
        bytes-pub --service rbt/cross-host/verify/bytes-1m --payload-bytes 1048576 \
            --count 3 --timeout-ms 30000
    run_probe_pair image-padded \
        image-sub --service rbt/cross-host/verify/image-padded \
            --layout 640x480-stride2048 --count 1 --timeout-ms 30000 -- \
        image-pub --service rbt/cross-host/verify/image-padded \
            --layout 640x480-stride2048 --count 1 --pace-ms 0 --timeout-ms 30000
    for points in 0 1 100; do
        run_probe_pair "pointcloud-${points}" \
            pointcloud-sub --service "rbt/cross-host/verify/pointcloud-${points}" \
                --points "${points}" --count 1 --timeout-ms 30000 -- \
            pointcloud-pub --service "rbt/cross-host/verify/pointcloud-${points}" \
                --points "${points}" --count 1 --pace-ms 0 --timeout-ms 30000
    done
    run_probe_pair pointcloud-20m \
        pointcloud-sub --service rbt/cross-host/verify/pointcloud-20m \
            --points 1310720 --count 3 --timeout-ms 60000 -- \
        pointcloud-pub --service rbt/cross-host/verify/pointcloud-20m \
            --points 1310720 --count 3 --pace-ms 700 --timeout-ms 60000

    netem_service=rbt/cross-host/verify/netem-image
    host_b "${rbt_bench}" subscriber --service "${netem_service}" \
        --profile sensor --message image --payload 1048576 --count 3 \
        --timeout-ms 60000 >"${run_dir}/netem-subscriber.log" 2>&1 &
    netem_subscriber=$!
    track_pid "${netem_subscriber}"
    sleep 0.2
    host_a "${rbt_bench}" publisher --service "${netem_service}" \
        --profile sensor --message image --payload 1048576 --count 3 \
        --interval-us 200000 --startup-delay-ms 3000 --timeout-ms 60000 \
        >"${run_dir}/netem-publisher.log" 2>&1 &
    netem_publisher=$!
    track_pid "${netem_publisher}"
    netem_ready=0
    for _attempt in $(seq 1 100); do
        if rg -q '^RBT_BENCH_READY role=subscriber' \
            "${run_dir}/netem-subscriber.log" 2>/dev/null; then
            netem_ready=1
            break
        fi
        sleep 0.05
    done
    if ((netem_ready == 1)) && tc -n hosta qdisc add dev vetha root netem \
        delay 20ms 5ms loss 1% rate 100mbit 2>"${run_dir}/netem-error.log"; then
        netem_status=0
        wait "${netem_publisher}" || netem_status=$?
        wait "${netem_subscriber}" || netem_status=$?
        tc -n hosta qdisc del dev vetha root
        ((netem_status == 0)) || {
            sed -n '1,200p' "${run_dir}/netem-publisher.log" >&2
            sed -n '1,200p' "${run_dir}/netem-subscriber.log" >&2
            exit "${netem_status}"
        }
        if rg -q '^RBT_BENCH_RESULT role=subscriber.*received=3' \
            "${run_dir}/netem-subscriber.log"; then
            printf 'PASS netem-established-stream received=3/3 delay=20ms jitter=5ms loss=1%% rate=100mbit\n'
        else
            netem_received="$(sed -n 's/^RBT_BENCH_RESULT role=subscriber.* received=\([0-9]*\) .*/\1/p' \
                "${run_dir}/netem-subscriber.log")"
            [[ -n "${netem_received}" ]]
            printf 'OBSERVED_LOSS netem-established-stream received=%s/3 delay=20ms jitter=5ms loss=1%% rate=100mbit\n' \
                "${netem_received}" | tee "${run_dir}/netem-summary.log"
        fi
    else
        stop_pid "${netem_publisher}"
        stop_pid "${netem_subscriber}"
        printf 'NOT_RUN netem kernel qdisc unavailable; see %s\n' \
            "${run_dir}/netem-error.log"
    fi

    read -r tunnel_a_threads_before tunnel_a_fds_before tunnel_a_rss_before < <(
        printf '%s %s %s\n' \
            "$(find "/proc/${tunnel_a_process}/task" -mindepth 1 -maxdepth 1 | wc -l)" \
            "$(find "/proc/${tunnel_a_process}/fd" -mindepth 1 -maxdepth 1 | wc -l)" \
            "$(sed -n 's/^VmRSS:[[:space:]]*\([0-9]*\).*/\1/p' "/proc/${tunnel_a_process}/status")"
    )
    for _cycle in $(seq 1 50); do
        ip -n hosta link set vetha down
        sleep 0.05
        ip -n hosta link set vetha up
        sleep 0.05
    done
    sleep 2
    run_probe_pair reconnect-stress-proof \
        fixed-sub --service rbt/cross-host/verify/reconnect-stress \
            --count 8 --timeout-ms 30000 -- \
        fixed-pub --service rbt/cross-host/verify/reconnect-stress \
            --count 8 --timeout-ms 30000
    printf 'RECONNECT_STRESS cycles=50 tunnel_a_threads_before=%s tunnel_a_fds_before=%s tunnel_a_rss_kib_before=%s tunnel_a_threads_after=%s tunnel_a_fds_after=%s tunnel_a_rss_kib_after=%s\n' \
        "${tunnel_a_threads_before}" "${tunnel_a_fds_before}" \
        "${tunnel_a_rss_before}" \
        "$(find "/proc/${tunnel_a_process}/task" -mindepth 1 -maxdepth 1 | wc -l)" \
        "$(find "/proc/${tunnel_a_process}/fd" -mindepth 1 -maxdepth 1 | wc -l)" \
        "$(sed -n 's/^VmRSS:[[:space:]]*\([0-9]*\).*/\1/p' "/proc/${tunnel_a_process}/status")" \
        | tee "${run_dir}/reconnect-stress-resources.log"
fi

# Existing rbt Sensor/SidecarV1 path under a five-second outage. The consumer
# uses WaitContext and accepts sequence gaps while still checking every header,
# payload length, selected byte, and checksum.
disconnect_service=rbt/cap/00000000-0000-0000-0000-00000000c051/stream
host_b env \
    RBT_IMAGE_SERVICE="${disconnect_service}" \
    RBT_IMAGE_FRAMES=30 \
    RBT_IMAGE_TIMEOUT_SECS=45 \
    "${image_consumer}" >"${run_dir}/disconnect-consumer.log" 2>&1 &
disconnect_consumer=$!
track_pid "${disconnect_consumer}"
sleep 0.2
host_a env \
    RBT_IMAGE_SERVICE="${disconnect_service}" \
    RBT_IMAGE_FRAMES=220 \
    RBT_IMAGE_RATE_HZ=10 \
    RBT_IMAGE_START_DELAY_MS=1000 \
    "${image_publisher}" >"${run_dir}/disconnect-publisher.log" 2>&1 &
disconnect_publisher=$!
track_pid "${disconnect_publisher}"
sleep 2
disconnect_started_ns="$(date +%s%N)"
ip -n hosta link set vetha down
sleep 5
ip -n hosta link set vetha up
reconnect_started_ns="$(date +%s%N)"
disconnect_status=0
wait "${disconnect_consumer}" || disconnect_status=$?
wait "${disconnect_publisher}" || disconnect_status=$?
((disconnect_status == 0)) || {
    sed -n '1,240p' "${run_dir}/disconnect-consumer.log" >&2
    sed -n '1,240p' "${run_dir}/disconnect-publisher.log" >&2
    exit "${disconnect_status}"
}
rg -q 'IMAGE_CONSUMER received=30' "${run_dir}/disconnect-consumer.log"
rg -q 'sequence_gaps=[1-9][0-9]*' "${run_dir}/disconnect-consumer.log"
first_after_reconnect="$(awk '
    /^IMAGE_CONSUMER seq=/ && / gap=[1-9][0-9]*$/ {
        sub(/^IMAGE_CONSUMER seq=/, "")
        sub(/ .*/, "")
        print
        exit
    }
' "${run_dir}/disconnect-consumer.log")"
[[ -n "${first_after_reconnect}" ]]
printf 'PASS disconnect-reconnect outage_ns=%s first_after_reconnect_sequence=%s consumer=WaitContext\n' \
    "$((reconnect_started_ns - disconnect_started_ns))" \
    "${first_after_reconnect}" \
    | tee "${run_dir}/disconnect-summary.log"

if [[ "${mode}" == full ]]; then
    # Slow Sensor diagnostic: completion with increasing sequences and gaps is
    # evidence of bounded recent-data behavior, not identical local/network QoS.
    slow_service=rbt/cap/00000000-0000-0000-0000-00000000c052/stream
    host_b env RBT_IMAGE_SERVICE="${slow_service}" RBT_IMAGE_FRAMES=20 \
        RBT_IMAGE_TIMEOUT_SECS=45 RBT_TEST_CONSUMER_RATE_HZ=5 \
        "${image_consumer}" >"${run_dir}/slow-consumer.log" 2>&1 &
    slow_consumer=$!
    track_pid "${slow_consumer}"
    sleep 0.2
    host_a env RBT_IMAGE_SERVICE="${slow_service}" RBT_IMAGE_FRAMES=180 \
        RBT_IMAGE_RATE_HZ=30 RBT_IMAGE_START_DELAY_MS=1000 \
        "${image_publisher}" >"${run_dir}/slow-publisher.log" 2>&1 &
    slow_publisher=$!
    track_pid "${slow_publisher}"
    slow_status=0
    wait "${slow_consumer}" || slow_status=$?
    wait "${slow_publisher}" || slow_status=$?
    ((slow_status == 0)) || exit "${slow_status}"
    rg -q 'IMAGE_CONSUMER received=20' "${run_dir}/slow-consumer.log"
    rg -q 'sequence_gaps=[1-9][0-9]*' "${run_dir}/slow-consumer.log"
    printf 'PASS slow-sensor bounded-recent-data\n'

    run_benchmark_pair() {
        local label="$1"
        local message="$2"
        local payload="$3"
        local count="$4"
        local interval_us="$5"
        local profile="${6:-sensor}"
        local service="rbt/cross-host/verify/bench-${label}"
        local tunnel_a_ticks_before
        local tunnel_b_ticks_before
        tunnel_a_ticks_before="$(awk '{ print $14 + $15 }' "/proc/${tunnel_a_process}/stat")"
        tunnel_b_ticks_before="$(awk '{ print $14 + $15 }' "/proc/${tunnel_b_process}/stat")"
        host_b "${rbt_bench}" subscriber --service "${service}" \
            --profile "${profile}" --message "${message}" --payload "${payload}" \
            --count "${count}" --timeout-ms 90000 \
            >"${run_dir}/bench-${label}-subscriber.log" 2>&1 &
        local subscriber_pid=$!
        track_pid "${subscriber_pid}"
        sleep 0.2
        host_a "${rbt_bench}" publisher --service "${service}" \
            --profile "${profile}" --message "${message}" --payload "${payload}" \
            --count "${count}" --interval-us "${interval_us}" \
            --startup-delay-ms 1000 --timeout-ms 90000 \
            >"${run_dir}/bench-${label}-publisher.log" 2>&1 &
        local publisher_pid=$!
        track_pid "${publisher_pid}"
        local status=0
        wait "${publisher_pid}" || status=$?
        wait "${subscriber_pid}" || status=$?
        ((status == 0)) || return "${status}"
        rg -q '^RBT_BENCH_RESULT role=publisher' \
            "${run_dir}/bench-${label}-publisher.log"
        rg -q '^RBT_BENCH_RESULT role=subscriber' \
            "${run_dir}/bench-${label}-subscriber.log"
        local tunnel_a_ticks_after
        local tunnel_b_ticks_after
        tunnel_a_ticks_after="$(awk '{ print $14 + $15 }' "/proc/${tunnel_a_process}/stat")"
        tunnel_b_ticks_after="$(awk '{ print $14 + $15 }' "/proc/${tunnel_b_process}/stat")"
        printf 'BENCH_GATEWAY_RESOURCES label=%s a_cpu_ticks=%s b_cpu_ticks=%s a_rss_kib=%s b_rss_kib=%s a_threads=%s b_threads=%s a_fds=%s b_fds=%s\n' \
            "${label}" \
            "$((tunnel_a_ticks_after - tunnel_a_ticks_before))" \
            "$((tunnel_b_ticks_after - tunnel_b_ticks_before))" \
            "$(sed -n 's/^VmRSS:[[:space:]]*\([0-9]*\).*/\1/p' "/proc/${tunnel_a_process}/status")" \
            "$(sed -n 's/^VmRSS:[[:space:]]*\([0-9]*\).*/\1/p' "/proc/${tunnel_b_process}/status")" \
            "$(find "/proc/${tunnel_a_process}/task" -mindepth 1 -maxdepth 1 | wc -l)" \
            "$(find "/proc/${tunnel_b_process}/task" -mindepth 1 -maxdepth 1 | wc -l)" \
            "$(find "/proc/${tunnel_a_process}/fd" -mindepth 1 -maxdepth 1 | wc -l)" \
            "$(find "/proc/${tunnel_b_process}/fd" -mindepth 1 -maxdepth 1 | wc -l)" \
            | tee -a "${run_dir}/bench-gateway-resources.log"
        printf 'PASS benchmark-%s\n' "${label}"
    }
    run_benchmark_pair 64b image 64 10000 0
    run_benchmark_pair 1k image 1024 5000 0
    run_benchmark_pair 1m image 1048576 200 10000
    run_benchmark_pair image-6220800 image 6220800 100 100000
    run_benchmark_pair pointcloud-20m pointcloud 20971520 30 400000
    run_benchmark_pair control-diagnostic control-command 16 20 10000 control
fi

# Hard architectural gate: neither provider disappearance nor remote tunnel
# disappearance may leave an apparently alive mirrored service on Host B.
lifecycle_service=rbt/cap/00000000-0000-0000-0000-00000000c053/stream
host_a "${probe}" image-pub --service "${lifecycle_service}" \
    --layout 1920x1080-rgb8 --count 400 --pace-ms 100 --timeout-ms 20000 \
    >"${run_dir}/lifecycle-provider.log" 2>&1 &
lifecycle_provider=$!
track_pid "${lifecycle_provider}"

visible=0
for _attempt in $(seq 1 100); do
    host_b "${iox2_service}" list -f JSON \
        >"${run_dir}/lifecycle-before.json" 2>&1 || true
    if rg -Fq "${lifecycle_service}" "${run_dir}/lifecycle-before.json"; then
        visible=1
        break
    fi
    sleep 0.1
done
((visible == 1)) || {
    printf 'remote lifecycle service never became visible\n' >&2
    exit 1
}

stop_pid "${lifecycle_provider}"
sleep 2
host_b "${iox2_service}" list -f JSON \
    >"${run_dir}/lifecycle-after-provider.json" 2>&1
host_b "${iox2_service}" details -f JSON "${lifecycle_service}" \
    >"${run_dir}/lifecycle-after-provider-details.json" 2>&1
stop_pid "${tunnel_a}"
sleep 5
host_b "${iox2_service}" list -f JSON \
    >"${run_dir}/lifecycle-after-gateway.json" 2>&1
host_b "${iox2_service}" details -f JSON "${lifecycle_service}" \
    >"${run_dir}/lifecycle-after-gateway-details.json" 2>&1

rg -Fq "${lifecycle_service}" "${run_dir}/lifecycle-after-provider.json"
rg -Fq "${lifecycle_service}" "${run_dir}/lifecycle-after-gateway.json"
rg -q '"state": "Alive"' "${run_dir}/lifecycle-after-gateway-details.json"
rg -q '"executable": "iox2-tunnel-zenoh"' \
    "${run_dir}/lifecycle-after-gateway-details.json"
printf 'EXPECTED_BLOCKER remote_liveness=FAIL mirrored_service_after_provider=present mirrored_service_after_remote_gateway=present local_alive_node=hostb_tunnel\n' \
    | tee "${run_dir}/lifecycle-summary.log"

printf 'NOT_RUN gateway-restart-loop reason=remote-liveness-hard-gate\n'
printf 'NOT_RUN composer-a-b-failure reason=no-health-primitive-for-proxy\n'
printf 'NOT_RUN remote-proxy-lifecycle reason=no-health-primitive-for-revocation\n'
printf 'NOT_RUN mixed-python-cpp-mcap reason=productization-stopped-at-hard-gate\n'
printf 'NOT_RUN 300-second-soak reason=productization-stopped-at-hard-gate\n'
printf 'CROSS_HOST_NAMESPACE_RESULT mode=%s functional_transport=PASS product_gate=FAIL recommendation=D\n' \
    "${mode}"
NAMESPACE_TESTS
namespace_status=$?
set -e

if ((namespace_status != 0)); then
    die "namespace verification failed with exit ${namespace_status}; see ${run_dir}"
fi

leftovers="$(
    ps -eo pid=,args= \
        | rg 'iox2-tunnel-zenoh|cross-host-probe|image-(publisher|consumer)' \
        | rg -v 'rg ' || true
)"
if [[ -n "${leftovers}" ]]; then
    printf '%s\n' "${leftovers}" >"${run_dir}/leftovers.log"
    die "leftover spike processes detected; see ${run_dir}/leftovers.log"
fi

printf 'CROSS_HOST_VERIFY_RESULT mode=%s evidence=%s functional=PASS product_gate=FAIL recommendation=D cleanup=PASS\n' \
    "${mode}" "${run_dir}"

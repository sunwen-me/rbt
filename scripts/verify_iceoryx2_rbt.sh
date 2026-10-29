#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"
mode="${1:---quick}"
case "${mode}" in
    --quick|--full) ;;
    --help|-h)
        printf '%s\n' 'usage: scripts/verify_iceoryx2_rbt.sh --quick|--full' \
            '--quick  Patch reproducibility, two-stack final-frame/QoS/reactive/lifecycle acceptance.' \
            '--full   Add all edge payloads, slow Sensor, reconnect and resource diagnostics.'
        exit 0 ;;
    *) printf 'unknown mode: %s\n' "${mode}" >&2; exit 2 ;;
esac
(($# <= 1)) || exit 2
# The standalone probe has a deliberate path dependency, not a floating crate.
# Refuse a mixed-source acceptance when bootstrap's optional source override is used.
[[ "${RBT_IOX2_FORK_SOURCE:-${repo_root}/third_party/iceoryx2-rbt/source}" == "${repo_root}/third_party/iceoryx2-rbt/source" ]] \
    || { printf 'verifier requires the default fork source path used by its probe\n' >&2; exit 2; }
for executable in cargo git ip mount nsenter unshare timeout awk rg sha256sum sort xargs; do
    command -v "${executable}" >/dev/null || { printf 'missing: %s\n' "${executable}" >&2; exit 1; }
done

toolchain="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
export PATH="${toolchain}/bin:${PATH}"
export CC="${toolchain}/bin/x86_64-conda-linux-gnu-gcc"
export CXX="${toolchain}/bin/x86_64-conda-linux-gnu-g++"
export AR="${toolchain}/bin/x86_64-conda-linux-gnu-ar"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
[[ -x "${CC}" ]] || { printf 'missing compiler: %s\n' "${CC}" >&2; exit 1; }

log_root="${RBT_IOX2_FORK_LOG_ROOT:-${repo_root}/target/iceoryx2-rbt-verify-logs}"
mkdir -p "${log_root}"
run_dir="$(mktemp -d "${log_root}/${mode#--}.XXXXXXXX")"
production_fingerprint() {
    {
        printf '%s\0' Cargo.toml Cargo.lock
        rg --files --null crates sdk/cpp python third_party/iceoryx2
    } | sort -z | xargs -0 sha256sum | sha256sum
}
production_before="$(production_fingerprint)"
printf '%s\n' "${production_before}" >"${run_dir}/production-before.sha256"
acceptance_inputs() {
    sha256sum scripts/bootstrap_iceoryx2_rbt.sh scripts/verify_iceoryx2_rbt.sh \
        experiments/iceoryx2-rbt/Cargo.toml experiments/iceoryx2-rbt/Cargo.lock \
        experiments/iceoryx2-rbt/src/*.rs third_party/iceoryx2-rbt/SOURCE_DIFF_SHA256 \
        experiments/iceoryx2-rbt/negative-control/Cargo.toml \
        experiments/iceoryx2-rbt/negative-control/Cargo.lock scripts/verify_iceoryx2_rpc_negative.sh \
        third_party/iceoryx2-rbt/patches/series third_party/iceoryx2-rbt/patches/SHA256SUMS \
        third_party/iceoryx2-rbt/patches/*.patch
}
inputs_before="$(acceptance_inputs)"
printf '%s\n' "${inputs_before}" >"${run_dir}/acceptance-inputs.sha256"
finish() {
    local status=$?
    local production_after
    production_after="$(production_fingerprint)"
    printf '%s\n' "${production_after}" >"${run_dir}/production-after.sha256"
    if [[ "${production_before}" != "${production_after}" ]]; then
        printf 'FAIL production fingerprint changed\n' >&2
        exit 1
    fi
    if [[ "${inputs_before}" != "$(acceptance_inputs)" ]]; then
        printf 'FAIL acceptance inputs changed during the run; rerun a frozen snapshot\n' >&2
        exit 1
    fi
    printf 'ICEORYX2_RBT_VERIFY status=%s mode=%s evidence=%s baseline=0.10.0-rbt.2 production_inputs_frozen=PASS\n' \
        "${status}" "${mode}" "${run_dir}" | tee "${run_dir}/result.log"
}
trap finish EXIT

run_logged() {
    local label="$1"
    shift
    printf 'CHECK %s\n' "${label}"
    if ! "$@" >"${run_dir}/${label}.log" 2>&1; then
        tail -80 "${run_dir}/${label}.log" >&2
        return 1
    fi
}

run_logged fork-reproduce scripts/bootstrap_iceoryx2_rbt.sh --verify
run_logged fork-source scripts/bootstrap_iceoryx2_rbt.sh --source-only
production_build="${repo_root}/target/iceoryx2-unified-build/workspace"
run_logged production-types-compose cargo test --locked --target-dir "${production_build}" -p rbt-types -p rbt-compose
run_logged production-rpc-correlation cargo test --locked --target-dir "${production_build}" \
    -p rbt-iox2 --test rpc_resources stale -- --test-threads=1
run_logged production-rpc-uninit-identity cargo test --locked --target-dir "${production_build}" \
    -p rbt-iox2 --test rpc_native_identity
run_logged fork-build scripts/bootstrap_iceoryx2_rbt.sh --build
fork_source="${RBT_IOX2_FORK_SOURCE:-${repo_root}/third_party/iceoryx2-rbt/source}"
build_root="${RBT_IOX2_FORK_BUILD:-${repo_root}/target/iceoryx2-rbt-build}"
run_logged fork-rpc-id-exhaustion cargo test --manifest-path "${fork_source}/Cargo.toml" \
    --target-dir "${build_root}/core" --locked -p iceoryx2 --lib request_id_tests
run_logged fork-ffi-storage cargo test --manifest-path "${fork_source}/Cargo.toml" \
    --target-dir "${build_root}/core" --locked -p iceoryx2-ffi-c --lib api::response_mut::test_generated::assert_storage_size
run_logged fork-link-tests cargo test --manifest-path "${fork_source}/Cargo.toml" \
    --target-dir "${build_root}/core" --locked -p iceoryx2-link -p iceoryx2-link-backend \
    -p iceoryx2-link-tunnel --lib
run_logged fork-no-std cargo check --manifest-path "${fork_source}/Cargo.toml" \
    --target-dir "${build_root}/core" --locked -p iceoryx2-link -p iceoryx2-link-backend \
    -p iceoryx2-link-tunnel --no-default-features --lib
run_logged fork-carrier-tests cargo test --manifest-path "${fork_source}/integrations/zenoh/Cargo.toml" \
    --target-dir "${build_root}/zenoh" --locked -p iceoryx2-integrations-zenoh-link-carrier --lib
run_logged fork-cli-tests cargo test --manifest-path "${fork_source}/integrations/zenoh/Cargo.toml" \
    --target-dir "${build_root}/zenoh" --locked -p iceoryx2-integrations-zenoh-link-tunnel-cli --bin iox2-link-tunnel-zenoh
if [[ "${mode}" == --full ]]; then
    run_logged rpc-upstream-negative-control scripts/verify_iceoryx2_rpc_negative.sh
    run_logged fork-conformance cargo test --manifest-path "${fork_source}/integrations/zenoh/Cargo.toml" \
        --target-dir "${build_root}/zenoh" --locked -p iceoryx2-integrations-zenoh-link-carrier --test main
fi
probe_manifest="${repo_root}/experiments/iceoryx2-rbt/Cargo.toml"
probe_target="${repo_root}/experiments/iceoryx2-rbt/target"
run_logged probe-fmt cargo fmt --manifest-path "${probe_manifest}" -- --check
run_logged probe-test cargo test --manifest-path "${probe_manifest}" --target-dir "${probe_target}" --locked
run_logged probe-clippy cargo clippy --manifest-path "${probe_manifest}" --target-dir "${probe_target}" --locked --all-targets -- -D warnings
run_logged probe-build cargo build --manifest-path "${probe_manifest}" --target-dir "${probe_target}" --locked
native_identity="path+file://${fork_source}/iceoryx2#0.10.0"
[[ "$(cargo pkgid --locked --offline -p iceoryx2)" == "${native_identity}" ]]
[[ "$(cargo pkgid --manifest-path "${probe_manifest}" --locked --offline -p iceoryx2)" == "${native_identity}" ]]
printf 'IDENTITY production_and_probe=%s fork=0.10.0-rbt.2\n' "${native_identity}" \
    | tee "${run_dir}/native-identity.log"

tunnel="${build_root}/zenoh/debug/iox2-link-tunnel-zenoh"
service_cli="${build_root}/core/debug/iox2-service"
probe="${probe_target}/debug/rbt-iceoryx2-fork-probe"
config_a="${repo_root}/experiments/cross-host-upstream/zenoh-hosta.toml"
config_b="${repo_root}/experiments/cross-host-upstream/zenoh-hostb.toml"

timeout --signal=TERM --kill-after=5 600 \
    unshare --user --map-root-user --mount --net --pid --fork --mount-proc \
    bash -s -- "${mode}" "${run_dir}" "${tunnel}" "${service_cli}" "${probe}" \
    "${config_a}" "${config_b}" <<'NAMESPACE' | tee "${run_dir}/namespace-console.log"
set -euo pipefail
mode="$1"; run_dir="$2"; tunnel="$3"; service_cli="$4"; probe="$5"; config_a="$6"; config_b="$7"
pids=()
stop_pid() {
    local pid="$1"
    kill -TERM "${pid}" 2>/dev/null || true
    for _attempt in $(seq 1 40); do
        kill -0 "${pid}" 2>/dev/null || break
        [[ "$(awk '{print $3}' "/proc/${pid}/stat" 2>/dev/null || true)" == Z ]] && break
        sleep 0.05
    done
    kill -KILL "${pid}" 2>/dev/null || true
    wait "${pid}" 2>/dev/null || true
}
cleanup() {
    local status=$?
    local index
    trap - EXIT INT TERM
    for ((index=${#pids[@]}-1; index>=0; index--)); do stop_pid "${pids[index]}"; done
    for pid in "${pids[@]}"; do
        if kill -0 "${pid}" 2>/dev/null; then
            printf 'FAIL leftover pid=%s\n' "${pid}" >&2
            status=1
        fi
    done
    printf 'CLEANUP status=%s tracked=%s\n' "${status}" "${#pids[@]}" >"${run_dir}/cleanup.log"
    exit "${status}"
}
trap cleanup EXIT
trap 'exit 130' INT TERM
mount --make-rprivate /
mkdir -p /run/netns
mount -t tmpfs tmpfs /run/netns
ip netns add hosta
ip netns add hostb
ip link add vetha type veth peer name vethb
ip link set vetha netns hosta
ip link set vethb netns hostb
ip -n hosta addr add 10.77.62.1/24 dev vetha
ip -n hostb addr add 10.77.62.2/24 dev vethb
ip -n hosta link set lo up
ip -n hostb link set lo up
ip -n hosta link set vetha mtu 1500 up
ip -n hostb link set vethb mtu 1500 up
ip netns exec hosta unshare --mount --ipc --uts bash -c '
    set -e; mount --make-rprivate /; mount -t tmpfs tmpfs /tmp; mount -t tmpfs tmpfs /dev/shm; hostname hosta; exec sleep 86400
' >"${run_dir}/anchor-a.log" 2>&1 & anchor_a=$!; pids+=("${anchor_a}")
ip netns exec hostb unshare --mount --ipc --uts bash -c '
    set -e; mount --make-rprivate /; mount -t tmpfs tmpfs /tmp; mount -t tmpfs tmpfs /dev/shm; hostname hostb; exec sleep 86400
' >"${run_dir}/anchor-b.log" 2>&1 & anchor_b=$!; pids+=("${anchor_b}")
sleep 0.3

spawn_a() {
    local label="$1"; shift
    nsenter -t "${anchor_a}" -m -i -u -n --preserve-credentials -- "$@" \
        >"${run_dir}/${label}.log" 2>&1 & spawn_pid=$!; pids+=("${spawn_pid}")
}
spawn_b() {
    local label="$1"; shift
    nsenter -t "${anchor_b}" -m -i -u -n --preserve-credentials -- "$@" \
        >"${run_dir}/${label}.log" 2>&1 & spawn_pid=$!; pids+=("${spawn_pid}")
}
wait_text() {
    local label="$1"; local text="$2"; local pid="$3"
    for _attempt in $(seq 1 500); do
        if rg -q -- "${text}" "${run_dir}/${label}.log"; then return 0; fi
        if ! kill -0 "${pid}" 2>/dev/null; then break; fi
        sleep 0.05
    done
    tail -50 "${run_dir}/${label}.log" >&2
    printf 'FAIL waiting for %s: %s\n' "${label}" "${text}" >&2
    return 1
}
wait_ok() {
    local pid="$1"; local label="$2"
    if ! wait "${pid}"; then
        tail -70 "${run_dir}/${label}.log" >&2
        return 1
    fi
}
assert_running() {
    local pid="$1"; local label="$2"; local state
    kill -0 "${pid}" 2>/dev/null || { printf 'FAIL %s is not alive\n' "${label}" >&2; return 1; }
    state="$(awk '{print $3}' "/proc/${pid}/stat")"
    [[ "${state}" != Z && "${state}" != X ]] \
        || { printf 'FAIL %s has process state %s\n' "${label}" "${state}" >&2; return 1; }
}
sigkill_provider() {
    local pid="$1"; local label="$2"; local status=0
    assert_running "${pid}" "${label}"
    kill -KILL "${pid}"
    wait "${pid}" 2>/dev/null || status=$?
    [[ "${status}" == 137 ]] \
        || { printf 'FAIL %s did not terminate by SIGKILL: status=%s\n' "${label}" "${status}" >&2; return 1; }
    printf 'SIGKILL label=%s wait_status=%s\n' "${label}" "${status}" | tee -a "${run_dir}/signals.log"
}

streams=(preexisting final image cloud padded points0 points1 points100 points100000 slow control lifecycle schema sdk)
tunnel_arguments=(--reactive --queue-capacity 1 --queue-overflow drop-oldest)
for stream in "${streams[@]}"; do tunnel_arguments+=(--rbt-stream "rbt/fork/${stream}"); done

# A service which exists before the local tunnel must converge in pure reactive mode.
spawn_a preexisting-pub "${probe}" publish --service rbt/fork/preexisting --kind bytes \
    --elements 64 --count 1 --start-delay-ms 5000 --hold-ms 2000
preexisting_pub="${spawn_pid}"
wait_text preexisting-pub '^READY role=publisher' "${preexisting_pub}"
spawn_a tunnel-a "${tunnel}" --zenoh-config "${config_a}" "${tunnel_arguments[@]}"
tunnel_a="${spawn_pid}"
spawn_b tunnel-b "${tunnel}" --zenoh-config "${config_b}" "${tunnel_arguments[@]}"
tunnel_b="${spawn_pid}"
spawn_b preexisting-sub "${probe}" consume --service rbt/fork/preexisting --kind bytes \
    --elements 64 --count 1 --expect-all --timeout-ms 15000
preexisting_sub="${spawn_pid}"
wait_ok "${preexisting_sub}" preexisting-sub
wait_ok "${preexisting_pub}" preexisting-pub
printf 'PASS reactive-preexisting-local-service\n'

run_pair() {
    local label="$1"; local kind="$2"; local count="$3"; local elements="$4"; local period="$5"
    shift 5
    spawn_b "${label}-sub" "${probe}" consume --service "rbt/fork/${label}" --kind "${kind}" \
        --count "${count}" --elements "${elements}" --timeout-ms 60000 --expect-all "$@"
    local sub="${spawn_pid}"
    spawn_a "${label}-pub" "${probe}" publish --service "rbt/fork/${label}" --kind "${kind}" \
        --count "${count}" --elements "${elements}" --period-ms "${period}" --start-delay-ms 2000 --hold-ms 1000 "$@"
    local pub="${spawn_pid}"
    wait_ok "${sub}" "${label}-sub"
    wait_ok "${pub}" "${label}-pub"
    rg -q 'strategy=WaitSet payload_polling=false initial_drain=0' "${run_dir}/${label}-sub.log"
    printf 'PASS %s native-qos-and-waitset\n' "${label}"
}

# No readiness frames cross the network for these configured pairs. A single
# final image must wake the receiver after local SHM insertion, with no later
# sample, synthetic initial drain, or payload polling to rescue it.
run_pair final image 1 0 0
run_pair image image 100 0 200
run_pair cloud points 3 1310720 700
run_pair control bytes 20 16 100 --control

# The production SDK, not just the native comparison probe, must receive a
# final Image through its existing SliceStream/WaitContext implementation.
spawn_b sdk-sub "${probe}" sdk-consume --service rbt/fork/sdk
sdk_sub="${spawn_pid}"
spawn_a sdk-pub "${probe}" sdk-publish --service rbt/fork/sdk
sdk_pub="${spawn_pid}"
wait_ok "${sdk_sub}" sdk-sub
wait_ok "${sdk_pub}" sdk-pub
rg -q 'sdk=true.*strategy=WaitContext.*payload_polling=false.*initial_drain=0' "${run_dir}/sdk-sub.log"
printf 'PASS production-SliceStream-and-WaitContext-final-frame\n'

# Explicit selection must not mirror an unrelated private service.
spawn_a private-pub "${probe}" publish --service private/not-exported --kind bytes --elements 64 \
    --count 0 --start-delay-ms 0 --hold-ms 3000
private_pub="${spawn_pid}"
sleep 0.5
nsenter -t "${anchor_b}" -m -i -u -n --preserve-credentials -- \
    "${service_cli}" list -f JSON >"${run_dir}/private-list.json" 2>&1
if rg -q 'private/not-exported' "${run_dir}/private-list.json"; then exit 1; fi
wait_ok "${private_pub}" private-pub
printf 'PASS explicit-stream-selection\n'

spawn_a schema-provider "${probe}" publish --service rbt/fork/schema --kind image \
    --count 0 --start-delay-ms 0 --hold-ms 20000
schema_provider="${spawn_pid}"
spawn_b schema-rejection "${probe}" reject-mismatch --service rbt/fork/schema
schema_rejection="${spawn_pid}"
wait_ok "${schema_rejection}" schema-rejection
rg -q 'error=IncompatibleTypes sample_endpoints_opened=0' "${run_dir}/schema-rejection.log"
stop_pid "${schema_provider}"
printf 'PASS typed-schema-mismatch-before-sample-endpoints\n'

# A real dependent uses the unchanged Composer runtime. The proxy observes
# native mirror publisher metadata and revokes only its own local capability.
spawn_a lifecycle-provider "${probe}" publish --service rbt/fork/lifecycle --kind image \
    --count 0 --start-delay-ms 0 --hold-ms 120000
provider="${spawn_pid}"
spawn_b lifecycle "${probe}" lifecycle --service rbt/fork/lifecycle --timeout-ms 60000 --expect-recovery
lifecycle="${spawn_pid}"
wait_text lifecycle 'LIFECYCLE phase=active ' "${lifecycle}"
sigkill_provider "${provider}" lifecycle-provider
wait_text lifecycle 'LIFECYCLE phase=blocked ' "${lifecycle}"
spawn_a lifecycle-replacement "${probe}" publish --service rbt/fork/lifecycle --kind image \
    --count 0 --start-delay-ms 0 --hold-ms 120000
replacement="${spawn_pid}"
wait_text lifecycle 'LIFECYCLE phase=reactivated ' "${lifecycle}"
sigkill_provider "${replacement}" lifecycle-replacement
wait_ok "${lifecycle}" lifecycle
rg -q 'result=PASS.*prepare_count=1 process_preserved=true' "${run_dir}/lifecycle.log"
printf 'PASS provider-sigkill-replacement-and-second-loss real-composer dependent-BLOCKED prepare-once\n'

if [[ "${mode}" == --full ]]; then
    run_pair padded padded 1 0 0
    run_pair points0 points 1 0 0
    run_pair points1 points 1 1 0
    run_pair points100 points 1 100 0
    run_pair points100000 points 1 100000 0
    spawn_b slow-sub "${probe}" consume --service rbt/fork/slow --kind bytes \
        --elements 1024 --count 300 --slow-ms 50 --timeout-ms 30000
    slow_sub="${spawn_pid}"
    spawn_a slow-pub "${probe}" publish --service rbt/fork/slow --kind bytes \
        --elements 1024 --count 300 --period-ms 5 --hold-ms 2000
    slow_pub="${spawn_pid}"
    wait_ok "${slow_pub}" slow-pub
    wait_ok "${slow_sub}" slow-sub
    rg -q 'last=299 ' "${run_dir}/slow-sub.log"
    printf 'PASS slow-Sensor-converges-to-final-frame\n'

    for _cycle in $(seq 1 10); do
        ip -n hosta link set vetha down
        sleep 0.05
        ip -n hosta link set vetha up
        sleep 0.05
    done
    sleep 2
    run_pair final image 1 0 0
    printf 'PASS reconnect-ten-cycles\n'

    # Interface flaps may be absorbed by TCP. Also exercise an actual tunnel
    # process replacement rather than equating every flap with a new session.
    stop_pid "${tunnel_a}"
    spawn_a tunnel-a-restarted "${tunnel}" --zenoh-config "${config_a}" "${tunnel_arguments[@]}"
    tunnel_a="${spawn_pid}"
    run_pair final image 1 0 0
    printf 'PASS tunnel-process-restart\n'
fi

# An idle pure-reactive tunnel must not spin on filesystem reads/own ports.
resources() {
    local side="$1"; local pid="$2"; local phase="$3"
    local descriptors=(/proc/"${pid}"/fd/*)
    printf 'RESOURCE side=%s phase=%s fds=%s ' "${side}" "${phase}" "${#descriptors[@]}"
    awk '/^Threads:/ {printf "threads=%s ", $2} /^VmRSS:/ {printf "rss_kib=%s ", $2} END {print ""}' "/proc/${pid}/status"
}
sleep 1
assert_running "${tunnel_a}" tunnel-a
assert_running "${tunnel_b}" tunnel-b
resources a "${tunnel_a}" before | tee "${run_dir}/resources.log"
resources b "${tunnel_b}" before | tee -a "${run_dir}/resources.log"
a_before="$(awk '{print $14+$15}' "/proc/${tunnel_a}/stat")"
b_before="$(awk '{print $14+$15}' "/proc/${tunnel_b}/stat")"
sleep 10
assert_running "${tunnel_a}" tunnel-a
assert_running "${tunnel_b}" tunnel-b
a_after="$(awk '{print $14+$15}' "/proc/${tunnel_a}/stat")"
b_after="$(awk '{print $14+$15}' "/proc/${tunnel_b}/stat")"
resources a "${tunnel_a}" after | tee -a "${run_dir}/resources.log"
resources b "${tunnel_b}" after | tee -a "${run_dir}/resources.log"
printf 'IDLE a_ticks=%s b_ticks=%s interval_s=10\n' "$((a_after-a_before))" "$((b_after-b_before))" \
    | tee "${run_dir}/idle.log"
((a_after-a_before <= 10 && b_after-b_before <= 10)) || { printf 'FAIL idle busy loop\n' >&2; exit 1; }
printf 'PASS reactive-idle\n'
printf 'ICEORYX2_RBT_NAMESPACE result=PASS mode=%s\n' "${mode}" | tee "${run_dir}/namespace-result.log"
NAMESPACE

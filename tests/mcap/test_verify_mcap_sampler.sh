#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
verifier="${repo_root}/scripts/verify_mcap.sh"
tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/rbt-mcap-sampler-test.XXXXXX")"
live_pid=''
failure_pid=''
race_pid=''
exit_race_pid=''

cleanup() {
    local pid
    for pid in "${live_pid}" "${failure_pid}" "${race_pid}" "${exit_race_pid}"; do
        [[ -n "${pid}" ]] || continue
        kill -KILL "${pid}" 2>/dev/null || true
        wait "${pid}" 2>/dev/null || true
    done
    rm -rf -- "${tmp_root}"
}
trap cleanup EXIT

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

extract_function() {
    local name="$1"
    awk -v name="${name}" '
        $0 ~ "^" name "\\(\\) \\{" { capturing=1 }
        capturing { print }
        capturing && /^}$/ { exit }
    ' "${verifier}"
}

for function_name in parse_proc_stat_exit_state proc_stat_exit_state proc_status_fields sample_process process_is_running; do
    definition="$(extract_function "${function_name}")"
    [[ -n "${definition}" ]] || fail "could not extract ${function_name} from verifier"
    eval "${definition}"
done
original_proc_stat_exit_state="$(declare -f proc_stat_exit_state)"

resources="${tmp_root}/live.tsv"
sleep 30 &
live_pid=$!
sample_process "${live_pid}" "${resources}" live
[[ -s "${resources}" ]] || fail "live process did not produce a resource row"
IFS=$'\t' read -r sampled_at label rss fds threads <"${resources}"
[[ "${label}" == live ]] || fail "unexpected resource label: ${label}"
[[ "${sampled_at}" =~ ^[0-9]+$ ]] || fail "invalid sample timestamp: ${sampled_at}"
[[ "${rss}" =~ ^[0-9]+$ ]] && ((rss > 0)) || fail "invalid RSS sample: ${rss}"
[[ "${fds}" =~ ^[0-9]+$ ]] && ((fds > 0)) || fail "invalid FD sample: ${fds}"
[[ "${threads}" =~ ^[0-9]+$ ]] && ((threads > 0)) || fail "invalid thread sample: ${threads}"
process_is_running "${live_pid}" || fail "live child was reported stopped"

comm_stat="${live_pid} (worker ) with spaces (and parens)) S 1 2 3 4 5 2"
if parse_proc_stat_exit_state "${live_pid}" "${comm_stat}"; then
    :
else
    status=$?
    fail "comm with spaces/parens was not parsed as a stable live process (status ${status})"
fi
exit_stat="${live_pid} (worker ) with spaces (and parens)) R 1 2 3 4 5 4"
if parse_proc_stat_exit_state "${live_pid}" "${exit_stat}"; then
    fail "PF_EXITING process stat was reported live"
else
    status=$?
    [[ "${status}" == 1 ]] || fail "PF_EXITING process stat returned ${status}, expected 1"
fi
if parse_proc_stat_exit_state "${live_pid}" "${live_pid} malformed" 2>"${tmp_root}/malformed-stat.log"; then
    fail "malformed process stat was accepted"
else
    status=$?
fi
[[ "${status}" == 2 ]] || fail "malformed process stat returned ${status}, expected 2"
rg -q 'malformed process stat' "${tmp_root}/malformed-stat.log" \
    || fail "malformed process stat lacked a diagnostic"

awk() {
    printf 'S\t\t\n'
}
incomplete_resources="${tmp_root}/incomplete-live.tsv"
incomplete_log="${tmp_root}/incomplete-live.log"
if sample_process "${live_pid}" "${incomplete_resources}" incomplete-live 2>"${incomplete_log}"; then
    fail "incomplete metrics for a stable live process were accepted"
else
    status=$?
fi
unset -f awk
[[ "${status}" == 2 ]] || fail "stable live incomplete metrics returned ${status}, expected 2"
rg -q 'incomplete process metrics for live pid' "${incomplete_log}" \
    || fail "stable live incomplete metrics lacked a diagnostic"
[[ ! -s "${incomplete_resources}" ]] || fail "stable live incomplete metrics produced a resource row"

awk() {
    printf 'R\t\t\n'
}
proc_stat_exit_state() {
    parse_proc_stat_exit_state "$1" "$1 (worker ) with spaces (and parens)) R 1 2 3 4 5 4"
}
exitflag_resources="${tmp_root}/incomplete-exiting.tsv"
sample_process "${live_pid}" "${exitflag_resources}" incomplete-exiting
unset -f awk proc_stat_exit_state
eval "${original_proc_stat_exit_state}"
[[ ! -s "${exitflag_resources}" ]] || fail "PF_EXITING process with incomplete metrics produced a resource row"

kill -TERM "${live_pid}" 2>/dev/null || true
wait "${live_pid}" 2>/dev/null || true
live_pid=''

true &
local_exited_pid=$!
wait "${local_exited_pid}"
exited_resources="${tmp_root}/exited.tsv"
sample_process "${local_exited_pid}" "${exited_resources}" exited
[[ ! -s "${exited_resources}" ]] || fail "already-exited child produced a resource row"
if process_is_running "${local_exited_pid}"; then
    fail "already-exited child was reported running"
else
    status=$?
    [[ "${status}" == 1 ]] || fail "exited child check returned ${status}, expected 1"
fi

sleep 30 &
race_pid=$!
race_find_marker="${tmp_root}/race-find-called"
find() {
    [[ "$1" == "/proc/${race_pid}/fd" ]] \
        || fail "sampler enumerated an unexpected FD path: $1"
    : >"${race_find_marker}"
    kill -TERM "${race_pid}" 2>/dev/null || true
    local attempt state status_path="/proc/${race_pid}/status"
    for ((attempt = 0; attempt < 100; ++attempt)); do
        if [[ ! -e "${status_path}" ]]; then
            return 1
        fi
        state="$(awk '/^State:/ { print $2; found=1; exit } END { if (!found) exit 1 }' \
            "${status_path}" 2>/dev/null)" || state=''
        case "${state}" in
            Z|X|x) return 1 ;;
        esac
        sleep 0.01
    done
    return 1
}
race_resources="${tmp_root}/race.tsv"
sample_process "${race_pid}" "${race_resources}" race
unset -f find
[[ -f "${race_find_marker}" ]] || fail "race regression did not reach FD enumeration"
[[ ! -s "${race_resources}" ]] || fail "vanished process produced a partial resource row"
wait "${race_pid}" 2>/dev/null || true
race_pid=''

sleep 30 &
exit_race_pid=$!
awk() {
    local status_path="${!#}"
    local captured state attempt observation_status
    if [[ "${status_path}" != "/proc/${exit_race_pid}/status" ]]; then
        command awk "$@"
        return $?
    fi
    captured="$(command awk "$@")" || return $?
    IFS=$'\t' read -r state _ _ <<<"${captured}"
    [[ "${state}" == R || "${state}" == S ]] \
        || fail "exit transition regression captured unexpected state: ${state}"
    kill -TERM "${exit_race_pid}" 2>/dev/null || return 2
    for ((attempt = 0; attempt < 100; ++attempt)); do
        if proc_stat_exit_state "${exit_race_pid}"; then
            sleep 0.01
        else
            observation_status=$?
            ((observation_status == 1)) && break
            return "${observation_status}"
        fi
    done
    ((observation_status == 1)) || return 2
    printf '%s\t\t\n' "${state}"
}
exit_race_resources="${tmp_root}/exit-after-status.tsv"
sample_process "${exit_race_pid}" "${exit_race_resources}" exit-after-status
unset -f awk
[[ ! -s "${exit_race_resources}" ]] \
    || fail "process that exited after status capture produced a resource row"
wait "${exit_race_pid}" 2>/dev/null || true
exit_race_pid=''

sleep 30 &
failure_pid=$!
find() { return 1; }
failure_log="${tmp_root}/collector-error.log"
if sample_process "${failure_pid}" "${tmp_root}/collector-error.tsv" collector-error 2>"${failure_log}"; then
    fail "live FD collector failure was accepted"
else
    status=$?
fi
unset -f find
[[ "${status}" == 2 ]] || fail "live FD collector failure returned ${status}, expected 2"
rg -q 'failed to enumerate file descriptors for live pid' "${failure_log}" \
    || fail "live collector failure lacked a diagnostic"

awk() { return 2; }
if process_is_running "${failure_pid}" 2>"${tmp_root}/status-error.log"; then
    fail "live process status read failure was accepted"
else
    status=$?
fi
unset -f awk
[[ "${status}" == 2 ]] || fail "live status read failure returned ${status}, expected 2"
rg -q 'failed to read process status for pid' "${tmp_root}/status-error.log" \
    || fail "live status read failure lacked a diagnostic"
kill -TERM "${failure_pid}" 2>/dev/null || true
wait "${failure_pid}" 2>/dev/null || true
failure_pid=''

printf 'MCAP sampler regressions passed\n'

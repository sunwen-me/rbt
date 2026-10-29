#!/usr/bin/env bash
set -euo pipefail

# Bounded v0.6 developer-experience gate.  It deliberately creates the test
# project outside the checkout so discovery/path handling and stale generated
# output are exercised independently of the current working directory. The
# checkout is passed explicitly as the development SDK; --full also runs the
# fresh-install boundary.

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

# All nested Cargo/Python verification must be reproducible from the already
# provisioned checkout.  Preserve an explicit caller override, but default
# every verifier launched from this gate to offline operation.
export CARGO_NET_OFFLINE="${CARGO_NET_OFFLINE:-true}"
export PIP_NO_INDEX="${PIP_NO_INDEX:-1}"
export PIP_DISABLE_PIP_VERSION_CHECK=1

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
usage: scripts/verify_dx.sh [--quick|--full]

--quick  build the CLI and exercise the clean-project first-ten-minutes flow
--full   quick plus broken-project diagnostics and deduplicated inherited
         release gates (including one representative pressure soak)

Environment overrides:
  RBT_DX_LOG_DIR              directory for retained logs
  RBT_DX_RUST_SDK             SDK root used when scaffolding from a checkout
  RBT_DX_COMPOSER_ADDR        isolated Composer address (default 127.0.0.1:17547)
  RBT_DX_SKIP_RUNTIME=1       skip the live launcher portion explicitly
  RBT_DX_SKIP_INHERITED=1     skip inherited gates in --full explicitly

Nested Cargo/Python activity defaults to offline mode:
  CARGO_NET_OFFLINE=true, PIP_NO_INDEX=1, PIP_DISABLE_PIP_VERSION_CHECK=1
EOF
}

mode=quick
case "${1:-}" in
    '') ;;
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
[[ $# -le 1 ]] || die "unexpected extra arguments"

for command_name in cargo rustc python3 bash mktemp find ps readlink setsid awk; do
    command -v "${command_name}" >/dev/null 2>&1 \
        || die "required command not found: ${command_name}"
done

# The checked development environment may ship its C compiler in the bundled
# Phase 4 toolchain.  Reuse the same explicit setup as the release verifiers.
toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
cpp_prefix="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"
if [[ -x "${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc" ]]; then
    export PATH="${toolchain_root}/bin:${PATH}"
    export CC="${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc"
    export CXX="${toolchain_root}/bin/x86_64-conda-linux-gnu-g++"
    export AR="${toolchain_root}/bin/x86_64-conda-linux-gnu-ar"
    export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
    export LD_LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
    export LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"
fi

log_root="${RBT_DX_LOG_DIR:-${repo_root}/target/dx-logs}"
run_id="dx-${mode}-$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="${log_root}/${run_id}"
mkdir -p "${run_dir}"
tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/rbt dx.XXXXXX")"

run_pid=''
run_pgid=''
declare -a external_pids=()
declare -A external_exes=()
declare -A external_labels=()

real_exe() {
    readlink -f -- "$1" 2>/dev/null || true
}

pid_exe() {
    local pid="$1"
    real_exe "/proc/${pid}/exe"
}

pid_matches_exe() {
    local pid="$1"
    local expected="$2"
    [[ -e "/proc/${pid}/exe" ]] || return 1
    [[ "$(pid_exe "${pid}")" == "$(real_exe "${expected}")" ]]
}

forget_external_pid() {
    local target="$1"
    local -a kept=()
    local pid
    for pid in "${external_pids[@]}"; do
        [[ "${pid}" == "${target}" ]] || kept+=("${pid}")
    done
    external_pids=("${kept[@]}")
    unset 'external_exes[$target]' 'external_labels[$target]'
}

stop_exact_pid() {
    local label="$1"
    local pid="$2"
    local expected="$3"
    local signal_name="${4:-TERM}"
    if ! kill -0 "${pid}" 2>/dev/null; then
        return 0
    fi
    if ! pid_matches_exe "${pid}" "${expected}"; then
        printf 'WARN: refusing to signal %s pid=%s because /proc/exe is not %s\n' \
            "${label}" "${pid}" "${expected}" >&2
        return 1
    fi
    kill -"${signal_name}" "${pid}" 2>/dev/null || true
}

stop_external_pid() {
    local pid="$1"
    local label="${external_labels[${pid}]:-external-${pid}}"
    local expected="${external_exes[${pid}]:-}"
    if [[ -n "${expected}" ]] && kill -0 "${pid}" 2>/dev/null; then
        stop_exact_pid "${label}" "${pid}" "${expected}" TERM \
            || die "refused to stop ${label} pid=${pid}"
    fi
    set +e
    wait "${pid}"
    local status=$?
    set -e
    forget_external_pid "${pid}"
    printf 'stopped %s pid=%s status=%s\n' "${label}" "${pid}" "${status}"
}

cleanup() {
    local pid
    # These are exact PIDs with an exact /proc/exe check.  No name-based kill
    # is used, so cleanup cannot target an unrelated hello-source process.
    for pid in "${external_pids[@]}"; do
        if [[ -n "${external_exes[${pid}]:-}" ]] && kill -0 "${pid}" 2>/dev/null; then
            stop_exact_pid "${external_labels[${pid}]:-external-${pid}}" \
                "${pid}" "${external_exes[${pid}]}" TERM || true
        fi
    done
    for pid in "${external_pids[@]}"; do
        wait "${pid}" 2>/dev/null || true
    done
    external_pids=()

    if [[ -n "${run_pid}" ]] && kill -0 "${run_pid}" 2>/dev/null; then
        if pid_matches_exe "${run_pid}" "${rbt_binary:-}"; then
            kill -INT "${run_pid}" 2>/dev/null || true
            for _ in {1..50}; do
                kill -0 "${run_pid}" 2>/dev/null || break
                sleep 0.1
            done
            if kill -0 "${run_pid}" 2>/dev/null; then
                # The process group was created by this exact launcher PID.
                [[ -n "${run_pgid}" ]] && kill -TERM -- "-${run_pgid}" 2>/dev/null || true
            fi
        fi
        wait "${run_pid}" 2>/dev/null || true
    fi
    if [[ -d "${tmp_root}" ]]; then
        rm -rf -- "${tmp_root}"
    fi
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
    local log_file="${run_dir}/${label}.log"
    print_command "$@"
    set +e
    "$@" >"${log_file}" 2>&1
    local status=$?
    set -e
    cat "${log_file}"
    ((status == 0)) || die "${label} failed with exit ${status}; see ${log_file}"
}

run_capture() {
    local label="$1"
    local output_file="$2"
    shift 2
    print_command "$@"
    set +e
    "$@" >"${output_file}" 2>"${run_dir}/${label}.stderr.log"
    local status=$?
    set -e
    cat "${output_file}"
    if ((status != 0)); then
        cat "${run_dir}/${label}.stderr.log" >&2
        die "${label} failed with exit ${status}; see ${run_dir}/${label}.stderr.log"
    fi
}

expect_failure() {
    local label="$1"
    shift
    local log_file="${run_dir}/${label}.log"
    print_command "$@"
    set +e
    "$@" >"${log_file}" 2>&1
    local status=$?
    set -e
    cat "${log_file}"
    ((status != 0)) || die "${label} unexpectedly succeeded; see ${log_file}"
    printf 'PASS: %s rejected invalid input (exit=%s)\n' "${label}" "${status}"
}

assert_json() {
    local label="$1"
    local json_file="$2"
    python3 - "${label}" "${json_file}" <<'PY'
import json
import pathlib
import sys

label, filename = sys.argv[1:]
try:
    value = json.loads(pathlib.Path(filename).read_text())
except Exception as exc:
    raise SystemExit(f"{label}: invalid JSON: {exc}")
if value is None:
    raise SystemExit(f"{label}: JSON value must not be null")
print(f"PASS: {label} is valid JSON")
PY
}

assert_version_verbose() {
    local label="$1"
    local text_file="$2"
    python3 - "${label}" "${text_file}" <<'PY'
import pathlib
import sys

label, filename = sys.argv[1:]
lines = [line.strip() for line in pathlib.Path(filename).read_text().splitlines() if line.strip()]
expected = {
    "rbt": "0.8.0",
    "SDK": "0.8.0",
    "IDL language": "1",
    "message descriptor": "1",
    "control protocol": "2",
    "Composer snapshot": "4",
    "MCAP file encoding": "1",
    "iceoryx2": "0.10.0-rbt.2",
}
if len(lines) != len(expected):
    raise SystemExit(f"{label}: expected exactly {len(expected)} non-empty version lines, got {len(lines)}")
observed = {}
for line in lines:
    if line.startswith("rbt "):
        key, value = "rbt", line[4:]
    else:
        key, separator, value = line.rpartition(" ")
        if not separator:
            raise SystemExit(f"{label}: malformed version line: {line!r}")
    if key in observed:
        raise SystemExit(f"{label}: duplicate version axis {key!r}")
    observed[key] = value
if observed != expected:
    raise SystemExit(f"{label}: frozen axes mismatch: expected {expected!r}, got {observed!r}")
print(f"PASS: {label} reports all frozen v0.6 axes exactly")
PY
}

assert_doctor_projection() {
    local label="$1"
    local json_file="$2"
    python3 - "${label}" "${json_file}" <<'PY'
import json
import pathlib
import sys

label, filename = sys.argv[1:]
value = json.loads(pathlib.Path(filename).read_text())
if not isinstance(value, dict):
    raise SystemExit(f"{label}: doctor report must be an object")
if value.get("platform") != "linux-x86_64":
    raise SystemExit(f"{label}: expected platform linux-x86_64")
if value.get("success") is not True:
    raise SystemExit(f"{label}: doctor success must be true")
checks = value.get("checks")
if not isinstance(checks, list) or not checks:
    raise SystemExit(f"{label}: checks must be a non-empty list")
required = {
    "platform",
    "rbt_cli",
    "composer_tool",
    "rbt-idl_tool",
    "rust_sdk",
    "rust_toolchain",
    "shm",
    "tmp",
}
observed = {}
for check in checks:
    if not isinstance(check, dict) or not isinstance(check.get("check_id"), str):
        raise SystemExit(f"{label}: every check must have a string check_id")
    check_id = check["check_id"]
    status = check.get("status")
    if status not in {"PASS", "WARN", "FAIL", "SKIP"}:
        raise SystemExit(f"{label}: check {check_id} has invalid status {status!r}")
    if not isinstance(check.get("summary"), str):
        raise SystemExit(f"{label}: check {check_id} has no string summary")
    if status == "FAIL":
        raise SystemExit(f"{label}: doctor contains FAIL check {check_id}")
    if check_id in observed:
        raise SystemExit(f"{label}: duplicate check_id {check_id}")
    observed[check_id] = status
missing = sorted(required - observed.keys())
if missing:
    raise SystemExit(f"{label}: missing required checks: {', '.join(missing)}")
not_pass = sorted(check_id for check_id in required if observed[check_id] != "PASS")
if not_pass:
    raise SystemExit(f"{label}: required checks not PASS: {', '.join(not_pass)}")
print(f"PASS: {label} has typed successful doctor checks on linux-x86_64")
PY
}

assert_check_projection() {
    local label="$1"
    local json_file="$2"
    python3 - "${label}" "${json_file}" <<'PY'
import json
import pathlib
import sys

label, filename = sys.argv[1:]
value = json.loads(pathlib.Path(filename).read_text())
if not isinstance(value, dict):
    raise SystemExit(f"{label}: check report must be an object")
if value.get("valid") is not True:
    raise SystemExit(f"{label}: check report valid must be true")
for field in ("project_root", "manifest"):
    if not isinstance(value.get(field), str) or not value[field]:
        raise SystemExit(f"{label}: {field} must be a non-empty string")
diagnostics = value.get("diagnostics")
if not isinstance(diagnostics, list):
    raise SystemExit(f"{label}: diagnostics must be a list")
for diagnostic in diagnostics:
    if not isinstance(diagnostic, dict):
        raise SystemExit(f"{label}: every diagnostic must be an object")
    if not isinstance(diagnostic.get("check_id"), str) or not diagnostic["check_id"]:
        raise SystemExit(f"{label}: diagnostic check_id must be a non-empty string")
    if diagnostic.get("status") not in {"PASS", "WARN", "FAIL", "SKIP"}:
        raise SystemExit(
            f"{label}: diagnostic {diagnostic.get('check_id')} has invalid status"
        )
    if not isinstance(diagnostic.get("summary"), str):
        raise SystemExit(f"{label}: diagnostic summary must be a string")
    if diagnostic.get("status") == "FAIL":
        raise SystemExit(f"{label}: diagnostic {diagnostic['check_id']} is FAIL")
print(f"PASS: {label} has a typed valid project report with no FAIL diagnostics")
PY
}

assert_ps_projection() {
    local label="$1"
    local json_file="$2"
    python3 - "${label}" "${json_file}" <<'PY'
import json
import pathlib
import sys

label, filename = sys.argv[1:]
value = json.loads(pathlib.Path(filename).read_text())
if not isinstance(value, dict) or not isinstance(value.get("components"), list):
    raise SystemExit(f"{label}: expected ps object with components")
if not isinstance(value.get("process_conflicts"), list):
    raise SystemExit(f"{label}: missing process_conflicts list")
print(f"PASS: {label} has structured process state")
PY
}

assert_graph_projection() {
    local label="$1"
    local json_file="$2"
    python3 - "${label}" "${json_file}" <<'PY'
import json
import pathlib
import sys

label, filename = sys.argv[1:]
value = json.loads(pathlib.Path(filename).read_text())
if not isinstance(value, dict):
    raise SystemExit(f"{label}: expected graph object")
for field in ("nodes", "edges", "freshness"):
    if field not in value:
        raise SystemExit(f"{label}: missing {field}")
if not isinstance(value["nodes"], list) or not isinstance(value["edges"], list):
    raise SystemExit(f"{label}: graph nodes/edges must be lists")
if not isinstance(value["freshness"], dict) or not value["freshness"].get("live_composer_response"):
    raise SystemExit(f"{label}: graph freshness is not a live Composer response")
print(f"PASS: {label} has typed graph/freshness fields")
PY
}

assert_why_active() {
    local label="$1"
    local json_file="$2"
    local expected_component="${3:-hello-sink}"
    python3 - "${label}" "${json_file}" "${expected_component}" <<'PY'
import json
import pathlib
import sys

label, filename, expected_component = sys.argv[1:]
value = json.loads(pathlib.Path(filename).read_text())
if value.get("component") != expected_component or value.get("component_state") != "active":
    raise SystemExit(f"{label}: {expected_component} is not active")
if not isinstance(value.get("requirements"), list):
    raise SystemExit(f"{label}: missing requirement list")
print(f"PASS: {label} reports active {expected_component}")
PY
}

assert_why_blocked() {
    local label="$1"
    local json_file="$2"
    python3 - "${label}" "${json_file}" <<'PY'
import json
import pathlib
import sys

label, filename = sys.argv[1:]
value = json.loads(pathlib.Path(filename).read_text())
if value.get("component") != "hello-sink" or value.get("component_state") != "blocked":
    raise SystemExit(f"{label}: hello-sink is not BLOCKED")
if "requirement_unresolved" not in value.get("reason_codes", []):
    raise SystemExit(f"{label}: why did not report requirement_unresolved")
requirements = value.get("requirements", [])
if not any(
    item.get("capability_type") == "Hello" and item.get("resolved") is False
    for item in requirements
):
    raise SystemExit(f"{label}: no unresolved Hello requirement")
print(f"PASS: {label} reports an unresolved Hello requirement")
PY
}

assert_capability_projection() {
    local label="$1"
    local json_file="$2"
    local capability_type="$3"
    python3 - "${label}" "${json_file}" "${capability_type}" <<'PY'
import json
import pathlib
import sys

label, filename, capability_type = sys.argv[1:]
value = json.loads(pathlib.Path(filename).read_text())
if not isinstance(value, list):
    raise SystemExit(f"{label}: capability list must be a JSON list")
matches = [item for item in value if item.get("capability_type") == capability_type]
if not matches:
    raise SystemExit(f"{label}: no logical {capability_type} capability")
if any(not item.get("provider_component") for item in matches):
    raise SystemExit(f"{label}: capability lacks provider identity")
print(f"PASS: {label} contains logical {capability_type} capability")
PY
}

assert_stream_projection() {
    local label="$1"
    local json_file="$2"
    local capability_type="$3"
    python3 - "${label}" "${json_file}" "${capability_type}" <<'PY'
import json
import pathlib
import sys

label, filename, capability_type = sys.argv[1:]
value = json.loads(pathlib.Path(filename).read_text())
if not isinstance(value, dict) or value.get("capability_type") != capability_type:
    raise SystemExit(f"{label}: stream selector did not resolve logical {capability_type}")
for field in ("capability_instance", "provider_component", "physical_endpoint", "message_type"):
    if field not in value:
        raise SystemExit(f"{label}: missing {field}")
print(f"PASS: {label} resolved logical {capability_type} selector")
PY
}

assert_pressure_projection() {
    local label="$1"
    local json_file="$2"
    local capability_type="$3"
    python3 - "${label}" "${json_file}" "${capability_type}" <<'PY'
import json
import pathlib
import sys

label, filename, capability_type = sys.argv[1:]
value = json.loads(pathlib.Path(filename).read_text())
if not isinstance(value, dict) or value.get("capability_type") != capability_type:
    raise SystemExit(f"{label}: pressure selector did not resolve logical {capability_type}")
for field in ("capability_instance", "provider_component", "physical_endpoint", "snapshot_freshness"):
    if field not in value:
        raise SystemExit(f"{label}: missing {field}")
if "publisher" not in value or not isinstance(value.get("subscriber_observations"), list):
    raise SystemExit(f"{label}: missing publisher/subscriber pressure projection")
print(f"PASS: {label} resolved logical {capability_type} pressure selector")
PY
}

assert_record_summary() {
    local label="$1"
    local json_file="$2"
    python3 - "${label}" "${json_file}" <<'PY'
import json
import pathlib
import sys

label, filename = sys.argv[1:]
value = json.loads(pathlib.Path(filename).read_text())
if value.get("complete") is not True or value.get("messages_recorded", 0) <= 0:
    raise SystemExit(f"{label}: recording was not complete and non-empty")
if len(value.get("channels", [])) != 1 or value.get("file_size_bytes", 0) <= 0:
    raise SystemExit(f"{label}: expected one non-empty recorded channel")
print(f"PASS: {label} is a complete short recording")
PY
}

assert_mcap_info() {
    local label="$1"
    local json_file="$2"
    python3 - "${label}" "${json_file}" <<'PY'
import json
import pathlib
import sys

label, filename = sys.argv[1:]
value = json.loads(pathlib.Path(filename).read_text())
if value.get("message_count", 0) <= 0 or not value.get("channels"):
    raise SystemExit(f"{label}: mcap info did not report recorded messages/channels")
print(f"PASS: {label} reports recording metadata")
PY
}

assert_play_summary() {
    local label="$1"
    local json_file="$2"
    python3 - "${label}" "${json_file}" <<'PY'
import json
import pathlib
import sys

label, filename = sys.argv[1:]
value = json.loads(pathlib.Path(filename).read_text())
if value.get("messages_published", 0) <= 0:
    raise SystemExit(f"{label}: replay published no messages")
print(f"PASS: {label} replayed recorded messages")
PY
}

wait_components_active() {
    local output="$1"
    local stderr_file="${run_dir}/$(basename "${output%.json}").stderr.log"
    local attempt
    for ((attempt = 0; attempt < 180; ++attempt)); do
        if RBT_COMPOSER_ADDR="${RBT_COMPOSER_ADDR}" "${rbt_cmd[@]}" ps --json \
            >"${output}" 2>"${stderr_file}"; then
            if python3 - "${output}" <<'PY'
import json
import pathlib
import sys

value = json.loads(pathlib.Path(sys.argv[1]).read_text())
expected = {"hello-source", "hello-sink", "camera", "detector"}
components = value.get("components", [])
states = {item.get("component_id"): item.get("state") for item in components}
if expected.issubset(states) and all(states[name] == "ACTIVE" for name in expected):
    raise SystemExit(0)
raise SystemExit(1)
PY
            then
                printf 'PASS: hello-source/hello-sink/camera/detector are all ACTIVE\n'
                return 0
            fi
        fi
        if [[ -n "${run_pid}" ]] && ! kill -0 "${run_pid}" 2>/dev/null; then
            cat "${stderr_file}" >&2 || true
            die "rbt run exited while waiting for all configured components"
        fi
        sleep 0.2
    done
    cat "${output}" >&2 || true
    cat "${stderr_file}" >&2 || true
    die "timed out waiting structurally for all configured components to become ACTIVE"
}

wait_component_state() {
    local output="$1"
    local component="$2"
    local expected_state="$3"
    local attempt
    for ((attempt = 0; attempt < 180; ++attempt)); do
        if RBT_COMPOSER_ADDR="${RBT_COMPOSER_ADDR}" "${rbt_cmd[@]}" ps --json \
            >"${output}" 2>"${output%.json}.stderr.log"; then
            if python3 - "${output}" "${component}" "${expected_state}" <<'PY'
import json
import pathlib
import sys

value = json.loads(pathlib.Path(sys.argv[1]).read_text())
component, expected = sys.argv[2:]
for item in value.get("components", []):
    if item.get("component_id") == component and item.get("state") == expected:
        raise SystemExit(0)
raise SystemExit(1)
PY
            then
                printf 'PASS: %s reached %s\n' "${component}" "${expected_state}"
                return 0
            fi
        fi
        sleep 0.2
    done
    cat "${output}" >&2 || true
    die "timed out waiting for ${component} to become ${expected_state}"
}

find_direct_child_by_exe() {
    local parent="$1"
    local expected="$2"
    python3 - "${parent}" "${expected}" <<'PY'
import os
import pathlib
import sys

parent = int(sys.argv[1])
expected = os.path.realpath(sys.argv[2])
matches = []
for entry in pathlib.Path("/proc").iterdir():
    if not entry.name.isdigit():
        continue
    try:
        status = (entry / "status").read_text()
        ppid = int(next(line.split(":", 1)[1] for line in status.splitlines() if line.startswith("PPid:")))
        exe = os.path.realpath(os.readlink(entry / "exe"))
    except (FileNotFoundError, PermissionError, OSError, StopIteration, ValueError):
        continue
    if ppid == parent and exe == expected:
        matches.append(int(entry.name))
if matches:
    print(min(matches))
PY
}

wait_direct_child_by_exe() {
    local parent="$1"
    local expected="$2"
    local attempt
    local pid
    for ((attempt = 0; attempt < 100; ++attempt)); do
        pid="$(find_direct_child_by_exe "${parent}" "${expected}")"
        if [[ -n "${pid}" ]]; then
            printf '%s\n' "${pid}"
            return 0
        fi
        sleep 0.1
    done
    die "launcher pid ${parent} did not own child executable ${expected}"
}

wait_started_pid() {
    local label="$1"
    local log_file="$2"
    local component="$3"
    local attempt
    local pid
    for ((attempt = 0; attempt < 100; ++attempt)); do
        pid="$(python3 - "${log_file}" "${component}" <<'PY'
import pathlib
import re
import sys

filename, component = sys.argv[1:]
pattern = re.compile(rf"^\[rbt\] started {re.escape(component)} pid=(\d+)$")
for line in pathlib.Path(filename).read_text(errors="replace").splitlines():
    match = pattern.fullmatch(line.strip())
    if match:
        print(match.group(1))
        break
PY
        )"
        if [[ -n "${pid}" ]]; then
            printf '%s\n' "${pid}"
            return 0
        fi
        sleep 0.1
    done
    cat "${log_file}" >&2 || true
    die "launcher log ${log_file} did not report started ${component} pid"
}

wait_pid_gone() {
    local label="$1"
    local pid="$2"
    local attempt
    for ((attempt = 0; attempt < 150; ++attempt)); do
        [[ ! -e "/proc/${pid}" ]] && return 0
        sleep 0.1
    done
    local state
    state="$(awk '/^State:/ {print $2; exit}' "/proc/${pid}/status" 2>/dev/null || true)"
    die "${label} pid=${pid} did not exit (state=${state:-unknown})"
}

pid_process_group() {
    local pid="$1"
    python3 - "${pid}" <<'PY'
import pathlib
import sys

pid = int(sys.argv[1])
try:
    stat = pathlib.Path(f"/proc/{pid}/stat").read_text()
    fields = stat.rsplit(")", 1)[1].split()
    print(fields[2])
except (FileNotFoundError, OSError, IndexError, ValueError) as exc:
    raise SystemExit(f"cannot read process group for pid {pid}: {exc}")
PY
}

pid_state() {
    local pid="$1"
    awk '{print $3}' "/proc/${pid}/stat" 2>/dev/null || true
}

count_process_fds() {
    local pid="$1"
    python3 - "${pid}" <<'PY'
import pathlib
import sys

pid = int(sys.argv[1])
try:
    print(sum(1 for entry in pathlib.Path(f"/proc/{pid}/fd").iterdir()))
except (FileNotFoundError, OSError) as exc:
    raise SystemExit(f"cannot count file descriptors for pid {pid}: {exc}")
PY
}

json_revision() {
    local label="$1"
    local json_file="$2"
    python3 - "${label}" "${json_file}" <<'PY'
import json
import pathlib
import sys

label, filename = sys.argv[1:]
value = json.loads(pathlib.Path(filename).read_text())
revision = value.get("revision") if isinstance(value, dict) else None
if isinstance(revision, bool) or not isinstance(revision, int):
    raise SystemExit(f"{label}: expected integer Composer revision")
print(revision)
PY
}

start_external() {
    local label="$1"
    local expected_exe="$2"
    shift 2
    local log_file="${run_dir}/${label}.log"
    print_command "$@"
    setsid -- "$@" >"${log_file}" 2>&1 &
    local pid=$!
    external_pids+=("${pid}")
    external_exes["${pid}"]="${expected_exe}"
    external_labels["${pid}"]="${label}"
    local attempt
    for ((attempt = 0; attempt < 50; ++attempt)); do
        if pid_matches_exe "${pid}" "${expected_exe}"; then
            last_external_pid="${pid}"
            printf '%s pid=%s log=%s\n' "${label}" "${pid}" "${log_file}"
            return 0
        fi
        kill -0 "${pid}" 2>/dev/null || break
        sleep 0.1
    done
    cat "${log_file}" >&2 || true
    die "${label} did not become the expected executable ${expected_exe}"
}

audit_exact_pids() {
    local output="$1"
    shift
    python3 - "${output}" "$@" <<'PY'
import os
import pathlib
import sys

output = pathlib.Path(sys.argv[1])
specs = sys.argv[2:]
failed = False
lines = []
for spec in specs:
    label, pid_text, expected = spec.split("|", 2)
    pid = int(pid_text)
    proc = pathlib.Path("/proc") / str(pid)
    if not proc.exists():
        lines.append(f"{label} pid={pid} gone")
        continue
    try:
        status = (proc / "status").read_text()
        state = next(line.split(":", 1)[1].strip().split()[0] for line in status.splitlines() if line.startswith("State:"))
        ppid = next(line.split(":", 1)[1].strip() for line in status.splitlines() if line.startswith("PPid:"))
        exe = os.path.realpath(os.readlink(proc / "exe"))
    except (FileNotFoundError, PermissionError, OSError, StopIteration):
        lines.append(f"{label} pid={pid} disappeared during audit")
        continue
    lines.append(f"{label} pid={pid} state={state} ppid={ppid} exe={exe}")
    # Any surviving exact PID is a cleanup failure. This includes a normal
    # live process, an unreaped zombie, and a reused PID with another exe.
    failed = True
if failed:
    output.write_text("\n".join(lines) + "\n")
    raise SystemExit(1)
output.write_text("\n".join(lines) + "\n")
print("PASS: exact PID/process audit found no live or zombie owned child")
PY
    cat "${output}"
}

audit_process_groups() {
    local output="$1"
    shift
    python3 - "${output}" "$@" <<'PY'
import os
import pathlib
import sys

output = pathlib.Path(sys.argv[1])
specs = sys.argv[2:]
groups = {}
for spec in specs:
    label, pgid_text = spec.split("|", 1)
    groups[int(pgid_text)] = label
violations = []
for entry in pathlib.Path("/proc").iterdir():
    if not entry.name.isdigit():
        continue
    try:
        stat = (entry / "stat").read_text()
        fields = stat.rsplit(")", 1)[1].split()
        state = fields[0]
        ppid = int(fields[1])
        pgrp = int(fields[2])
        if pgrp not in groups:
            continue
        try:
            exe = os.path.realpath(os.readlink(entry / "exe"))
        except (FileNotFoundError, PermissionError, OSError):
            exe = "<unavailable>"
        violations.append(
            f"{groups[pgrp]} pgid={pgrp} member_pid={entry.name} "
            f"state={state} ppid={ppid} exe={exe}"
        )
    except (FileNotFoundError, PermissionError, OSError, IndexError, ValueError):
        # Processes can disappear while /proc is being sampled.  A surviving
        # entry is reported; a racing entry is not evidence of a leak.
        continue
if violations:
    output.write_text("\n".join(violations) + "\n")
    raise SystemExit(1)
output.write_text("no members or zombies remain in exact process groups\n")
print("PASS: exact process-group audit found no members or zombies")
PY
    cat "${output}"
}

run_broken_diagnostics() {
    local broken_root="${tmp_root}/broken projects"
    mkdir -p "${broken_root}"

    pushd "${broken_root}" >/dev/null
    run_logged broken-new-bad-idl "${rbt_cmd[@]}" new bad-idl
    local bad_project="${broken_root}/bad-idl"
    local bad_file="${bad_project}/types/src/bad.rbt"
    python3 - "${bad_file}" >"${run_dir}/broken-write-bad-idl.log" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
path.write_text("package broken.bad;\nabi 1;\n\nmessage Bad {\n    value: floatt;\n}\n")
PY
    cat "${run_dir}/broken-write-bad-idl.log"
    pushd "${bad_project}" >/dev/null
    expect_failure broken-bad-idl "${rbt_cmd[@]}" idl check
    grep -Eq 'bad\.rbt|floatt|line|column' "${run_dir}/broken-bad-idl.log" \
        || die "bad IDL diagnostics omitted source/action context"
    popd >/dev/null

    run_logged broken-new-missing-component "${rbt_cmd[@]}" new missing-component
    local missing_project="${broken_root}/missing-component"
    python3 - "${missing_project}/rbt.toml" >"${run_dir}/broken-write-missing-component.log" <<'PY'
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text()
text = text.replace('package = "hello-source"', 'package = "missing-source"', 1)
path.write_text(text)
PY
    cat "${run_dir}/broken-write-missing-component.log"
    pushd "${missing_project}" >/dev/null
    expect_failure broken-missing-component "${rbt_cmd[@]}" check
    grep -Eqi 'missing|hello-source|component' "${run_dir}/broken-missing-component.log" \
        || die "missing component diagnostics omitted component context"
    popd >/dev/null

    run_logged broken-new-duplicate-name "${rbt_cmd[@]}" new duplicate-name
    local duplicate_project="${broken_root}/duplicate-name"
    pushd "${duplicate_project}" >/dev/null
    expect_failure broken-duplicate-name "${rbt_cmd[@]}" new component hello-source
    grep -Eqi 'already declared|duplicate|hello-source' "${run_dir}/broken-duplicate-name.log" \
        || die "duplicate-name diagnostics omitted the duplicate identity"
    popd >/dev/null
    popd >/dev/null
    printf 'PASS: --full broken-project diagnostics lane completed\n'
}

run_full_restart_hammer() {
    local project_root="$1"
    local launcher_log="${run_dir}/full-restart-hammer.log"
    local active_before="${run_dir}/full-restart-hammer-active-before.json"
    local active_after="${run_dir}/full-restart-hammer-active-after.json"
    local query_errors="${run_dir}/full-restart-hammer-query-errors.log"
    local second_pid
    local second_pgid
    local second_status
    local second_composer_pid
    local second_source_pid
    local second_sink_pid
    local second_camera_pid
    local second_detector_pid
    local second_composer_pgid
    local second_source_pgid
    local second_sink_pgid
    local second_camera_pgid
    local second_detector_pgid
    local revision_before
    local revision_after
    local fds_before
    local fds_after
    local state

    pushd "${project_root}" >/dev/null
    setsid "${rbt_cmd[@]}" run >"${launcher_log}" 2>&1 &
    second_pid="$!"
    run_pid="${second_pid}"
    second_pgid="$(pid_process_group "${second_pid}")"
    run_pgid="${second_pgid}"
    printf 'full restart launcher pid=%s pgid=%s log=%s\n' \
        "${second_pid}" "${second_pgid}" "${launcher_log}"

    wait_components_active "${active_before}"
    assert_json full-restart-active-before "${active_before}"
    assert_ps_projection full-restart-active-before "${active_before}"
    second_composer_pid="$(wait_direct_child_by_exe "${second_pid}" "${composer_exe}")"
    second_source_pid="$(wait_direct_child_by_exe "${second_pid}" "${hello_source_exe}")"
    second_sink_pid="$(wait_direct_child_by_exe "${second_pid}" "${hello_sink_exe}")"
    second_camera_pid="$(wait_direct_child_by_exe "${second_pid}" "${camera_exe}")"
    second_detector_pid="$(wait_direct_child_by_exe "${second_pid}" "${detector_exe}")"
    second_composer_pgid="$(pid_process_group "${second_composer_pid}")"
    second_source_pgid="$(pid_process_group "${second_source_pid}")"
    second_sink_pgid="$(pid_process_group "${second_sink_pid}")"
    second_camera_pgid="$(pid_process_group "${second_camera_pid}")"
    second_detector_pgid="$(pid_process_group "${second_detector_pid}")"
    revision_before="$(json_revision full-restart-revision-before "${active_before}")"
    fds_before="$(count_process_fds "${second_composer_pid}")"
    printf 'full restart baseline revision=%s composer_fds=%s\n' \
        "${revision_before}" "${fds_before}" \
        | tee "${run_dir}/full-restart-hammer-baseline.txt"

    : >"${query_errors}"
    local query_index
    for ((query_index = 1; query_index <= 250; ++query_index)); do
        RBT_COMPOSER_ADDR="${RBT_COMPOSER_ADDR}" "${rbt_cmd[@]}" ps --json \
            >/dev/null 2>>"${query_errors}" \
            || die "full restart ps hammer failed at iteration ${query_index}"
    done
    for ((query_index = 1; query_index <= 250; ++query_index)); do
        RBT_COMPOSER_ADDR="${RBT_COMPOSER_ADDR}" "${rbt_cmd[@]}" graph --json \
            >/dev/null 2>>"${query_errors}" \
            || die "full restart graph hammer failed at iteration ${query_index}"
    done
    for ((query_index = 1; query_index <= 250; ++query_index)); do
        RBT_COMPOSER_ADDR="${RBT_COMPOSER_ADDR}" "${rbt_cmd[@]}" why hello-sink --json \
            >/dev/null 2>>"${query_errors}" \
            || die "full restart why hammer failed at iteration ${query_index}"
    done
    for ((query_index = 1; query_index <= 250; ++query_index)); do
        RBT_COMPOSER_ADDR="${RBT_COMPOSER_ADDR}" "${rbt_cmd[@]}" stream pressure Hello --json \
            >/dev/null 2>>"${query_errors}" \
            || die "full restart stream-pressure hammer failed at iteration ${query_index}"
    done
    printf 'PASS: full restart CLI hammer completed 1000 bounded JSON queries\n' \
        | tee "${run_dir}/full-restart-hammer-result.txt"

    wait_components_active "${active_after}"
    assert_json full-restart-active-after "${active_after}"
    assert_ps_projection full-restart-active-after "${active_after}"
    revision_after="$(json_revision full-restart-revision-after "${active_after}")"
    fds_after="$(count_process_fds "${second_composer_pid}")"
    [[ "${revision_after}" == "${revision_before}" ]] \
        || die "Composer revision changed during CLI hammer (${revision_before} -> ${revision_after})"
    if ((fds_after > fds_before + 2)); then
        die "Composer FD count grew by more than 2 during CLI hammer (${fds_before} -> ${fds_after})"
    fi
    printf 'PASS: full restart composition revision unchanged; Composer FDs %s -> %s\n' \
        "${fds_before}" "${fds_after}" | tee "${run_dir}/full-restart-hammer-resources.txt"

    stop_exact_pid full-restart-launcher "${second_pid}" "${rbt_binary}" TERM \
        || die "could not signal exact full-restart launcher"
    local attempt
    for ((attempt = 0; attempt < 120; ++attempt)); do
        state="$(pid_state "${second_pid}")"
        [[ -z "${state}" || "${state}" == Z ]] && break
        sleep 0.1
    done
    state="$(pid_state "${second_pid}")"
    if [[ -n "${state}" && "${state}" != Z ]]; then
        cat "${launcher_log}" >&2 || true
        die "full-restart launcher did not exit within bounded SIGTERM timeout"
    fi
    set +e
    wait "${second_pid}"
    second_status=$?
    set -e
    ((second_status == 0)) || die "full-restart launcher exited with ${second_status} after SIGTERM"
    run_pid=''
    run_pgid=''
    printf 'PASS: full restart launcher exited successfully after SIGTERM\n'

    audit_exact_pids "${run_dir}/full-restart-pid-audit.txt" \
        "launcher|${second_pid}|${rbt_binary}" \
        "composer|${second_composer_pid}|${composer_exe}" \
        "hello-source|${second_source_pid}|${hello_source_exe}" \
        "hello-sink|${second_sink_pid}|${hello_sink_exe}" \
        "camera|${second_camera_pid}|${camera_exe}" \
        "detector|${second_detector_pid}|${detector_exe}"
    audit_process_groups "${run_dir}/full-restart-process-group-audit.txt" \
        "launcher|${second_pgid}" \
        "composer|${second_composer_pgid}" \
        "hello-source|${second_source_pgid}" \
        "hello-sink|${second_sink_pgid}" \
        "camera|${second_camera_pgid}" \
        "detector|${second_detector_pgid}"
    popd >/dev/null
}

mutate_sink_to_stuck_wrapper() {
    local manifest="$1"
    local sink_exe="$2"
    python3 - "${manifest}" "${sink_exe}" <<'PY'
import json
import pathlib
import shlex
import sys

manifest, sink_exe = map(pathlib.Path, sys.argv[1:])
lines = manifest.read_text().splitlines()
in_component = False
target = False
changed = False
wrapper = [
    "/bin/sh",
    "-c",
    f"trap '' TERM; {shlex.quote(str(sink_exe))}; while :; do sleep 1; done",
]
replacement = "command = " + json.dumps(wrapper)
updated = []
for line in lines:
    stripped = line.strip()
    if stripped == "[[component]]":
        in_component = True
        target = False
    if in_component and stripped == 'name = "hello-sink"':
        target = True
    if target and stripped == 'package = "hello-sink"':
        updated.append(replacement)
        changed = True
    else:
        updated.append(line)
if not changed:
    raise SystemExit(f"could not find hello-sink package entry in {manifest}")
manifest.write_text("\n".join(updated) + "\n")
PY
    printf 'PASS: hello-sink manifest command replaced with exact TERM-resistant wrapper\n'
}

run_full_forced_shutdown() {
    local project_root="$1"
    local manifest="${project_root}/rbt.toml"
    local third_log="${run_dir}/full-forced-shutdown.log"
    local third_pid
    local third_pgid
    local third_status
    local third_composer_pid
    local third_source_pid
    local third_sink_pid
    local third_camera_pid
    local third_detector_pid
    local third_composer_pgid
    local third_source_pgid
    local third_sink_pgid
    local third_camera_pgid
    local third_detector_pgid
    local state

    mutate_sink_to_stuck_wrapper "${manifest}" "${hello_sink_exe}"
    pushd "${project_root}" >/dev/null
    run_logged full-forced-shutdown-check "${rbt_cmd[@]}" check
    setsid "${rbt_cmd[@]}" run >"${third_log}" 2>&1 &
    third_pid="$!"
    run_pid="${third_pid}"
    third_pgid="$(pid_process_group "${third_pid}")"
    run_pgid="${third_pgid}"
    printf 'full forced-shutdown launcher pid=%s pgid=%s log=%s\n' \
        "${third_pid}" "${third_pgid}" "${third_log}"
    wait_components_active "${run_dir}/full-forced-shutdown-active.json"

    # The launcher owns exact PIDs emitted by its own structured start log.
    # This intentionally does not use pgrep or process-name matching.
    third_composer_pid="$(wait_started_pid full-forced-shutdown "${third_log}" composer)"
    third_source_pid="$(wait_started_pid full-forced-shutdown "${third_log}" hello-source)"
    third_sink_pid="$(wait_started_pid full-forced-shutdown "${third_log}" hello-sink)"
    third_camera_pid="$(wait_started_pid full-forced-shutdown "${third_log}" camera)"
    third_detector_pid="$(wait_started_pid full-forced-shutdown "${third_log}" detector)"
    pid_matches_exe "${third_sink_pid}" /bin/sh \
        || die "started hello-sink PID ${third_sink_pid} is not the expected wrapper shell"
    third_composer_pgid="$(pid_process_group "${third_composer_pid}")"
    third_source_pgid="$(pid_process_group "${third_source_pid}")"
    third_sink_pgid="$(pid_process_group "${third_sink_pid}")"
    third_camera_pgid="$(pid_process_group "${third_camera_pid}")"
    third_detector_pgid="$(pid_process_group "${third_detector_pid}")"
    printf 'full forced-shutdown started PIDs composer=%s hello-source=%s hello-sink-wrapper=%s camera=%s detector=%s\n' \
        "${third_composer_pid}" "${third_source_pid}" "${third_sink_pid}" \
        "${third_camera_pid}" "${third_detector_pid}" \
        | tee "${run_dir}/full-forced-shutdown-started-pids.txt"

    stop_exact_pid full-forced-shutdown-launcher "${third_pid}" "${rbt_binary}" INT \
        || die "could not send first SIGINT to exact forced-shutdown launcher"
    sleep 0.5
    state="$(pid_state "${third_pid}")"
    [[ "${state}" != Z && -n "${state}" ]] \
        || die "forced-shutdown launcher exited after first SIGINT despite stuck wrapper"
    printf 'PASS: first SIGINT left launcher alive behind TERM-resistant wrapper\n'

    stop_exact_pid full-forced-shutdown-launcher "${third_pid}" "${rbt_binary}" INT \
        || die "could not send second SIGINT to exact forced-shutdown launcher"
    local attempt
    for ((attempt = 0; attempt < 120; ++attempt)); do
        state="$(pid_state "${third_pid}")"
        [[ -z "${state}" || "${state}" == Z ]] && break
        sleep 0.1
    done
    state="$(pid_state "${third_pid}")"
    if [[ -n "${state}" && "${state}" != Z ]]; then
        cat "${third_log}" >&2 || true
        die "forced-shutdown launcher did not exit within bounded second-SIGINT timeout"
    fi
    set +e
    wait "${third_pid}"
    third_status=$?
    set -e
    ((third_status != 0)) || die "forced-shutdown launcher unexpectedly exited successfully"
    python3 - "${third_log}" <<'PY'
import pathlib
import sys

log = pathlib.Path(sys.argv[1]).read_text(errors="replace")
if "forcing shutdown" not in log:
    raise SystemExit("forced-shutdown launcher log omitted forcing shutdown")
PY
    run_pid=''
    run_pgid=''
    printf 'PASS: second SIGINT forced bounded nonzero launcher shutdown\n'

    audit_exact_pids "${run_dir}/full-forced-shutdown-pid-audit.txt" \
        "launcher|${third_pid}|${rbt_binary}" \
        "composer|${third_composer_pid}|${composer_exe}" \
        "hello-source|${third_source_pid}|${hello_source_exe}" \
        "hello-sink-wrapper|${third_sink_pid}|/bin/sh" \
        "camera|${third_camera_pid}|${camera_exe}" \
        "detector|${third_detector_pid}|${detector_exe}"
    audit_process_groups "${run_dir}/full-forced-shutdown-process-group-audit.txt" \
        "launcher|${third_pgid}" \
        "composer|${third_composer_pgid}" \
        "hello-source|${third_source_pgid}" \
        "hello-sink-wrapper|${third_sink_pgid}" \
        "camera|${third_camera_pgid}" \
        "detector|${third_detector_pgid}"
    popd >/dev/null
}

printf 'RBT_DX_VERIFY mode=%s logs=%s\n' "${mode}" "${run_dir}"

[[ -f "${cpp_prefix}/lib/cmake/iceoryx2-cxx/iceoryx2-cxxConfig.cmake" ]] \
    || die "iceoryx2 C++ package config is missing under ${cpp_prefix}"
[[ -f "${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" ]] \
    || die "iceoryx2 C++ provenance checker is missing"
run_logged cpp-prefix-provenance bash \
    "${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${cpp_prefix}"

run_logged shell-syntax bash -n scripts/verify_dx.sh
# Keep the first build explicitly offline and include all three independently
# shipped onboarding tools.
run_logged cli-idl-composer-build cargo build --offline -p rbt-cli -p rbt-idl -p composer
rbt_binary="${RBT_BIN:-${repo_root}/target/debug/rbt}"
[[ -x "${rbt_binary}" ]] || die "rbt binary is missing at ${rbt_binary}; set RBT_BIN or build rbt-cli"
composer_binary="${repo_root}/target/debug/composer"
[[ -x "${composer_binary}" ]] || die "Composer binary is missing at ${composer_binary}"
rbt_cmd=("${rbt_binary}")
run_logged version "${rbt_cmd[@]}" --version
run_logged version-verbose "${rbt_cmd[@]}" version --verbose
assert_version_verbose version-verbose "${run_dir}/version-verbose.log"

export RBT_RUST_SDK="${RBT_DX_RUST_SDK:-${repo_root}}"
export RBT_COMPOSER_ADDR="${RBT_DX_COMPOSER_ADDR:-127.0.0.1:17547}"
export PATH="${repo_root}/target/debug:${PATH}"

outside="${tmp_root}/first ten minutes"
mkdir -p "${outside}"
pushd "${outside}" >/dev/null
run_capture doctor-json "${run_dir}/doctor.json" "${rbt_cmd[@]}" doctor --json
assert_json doctor "${run_dir}/doctor.json"
assert_doctor_projection doctor "${run_dir}/doctor.json"
run_logged doctor-human "${rbt_cmd[@]}" doctor
run_logged new-project "${rbt_cmd[@]}" new hello-robot
project_root="${outside}/hello-robot"
[[ -f "${project_root}/rbt.toml" ]] || die "new project did not create rbt.toml"
set +e
"${rbt_cmd[@]}" new hello-robot >"${run_dir}/new-duplicate.log" 2>&1
duplicate_status=$?
set -e
((duplicate_status != 0)) || die "rbt new overwrote an existing project"
printf 'PASS: duplicate project creation was rejected\n'
popd >/dev/null

pushd "${project_root}" >/dev/null
run_logged check-initial "${rbt_cmd[@]}" check
run_logged idl-check-initial "${rbt_cmd[@]}" idl check
run_logged idl-fmt-check-before "${rbt_cmd[@]}" idl fmt --check
run_logged new-message "${rbt_cmd[@]}" new message Detection
run_logged new-camera "${rbt_cmd[@]}" new component camera
run_logged new-detector "${rbt_cmd[@]}" new component detector
run_logged idl-fmt "${rbt_cmd[@]}" idl fmt
run_logged idl-fmt-check-after "${rbt_cmd[@]}" idl fmt --check
run_logged idl-check "${rbt_cmd[@]}" idl check
run_logged idl-generate "${rbt_cmd[@]}" idl generate
run_logged check-generated "${rbt_cmd[@]}" check
run_logged project-build cargo build --offline --workspace

# Discovery must work from a component source directory, not only from root.
component_source="$(find components -type d -name src -print -quit)"
[[ -n "${component_source}" ]] || die "generated project has no component source directory"
pushd "${component_source}" >/dev/null
run_logged check-from-subdir "${rbt_cmd[@]}" check
popd >/dev/null

check_json="${run_dir}/check.json"
run_capture check-json "${check_json}" "${rbt_cmd[@]}" check --json
assert_json check "${check_json}"
assert_check_projection check "${check_json}"
run_logged doctor-project-human "${rbt_cmd[@]}" doctor
run_capture doctor-project-json "${run_dir}/doctor-project.json" "${rbt_cmd[@]}" doctor --json
assert_json doctor-project "${run_dir}/doctor-project.json"
assert_doctor_projection doctor-project "${run_dir}/doctor-project.json"

if [[ "${RBT_DX_SKIP_RUNTIME:-0}" == 1 ]]; then
    printf 'SKIP: live rbt run was explicitly disabled by RBT_DX_SKIP_RUNTIME=1\n'
else
    run_log="${run_dir}/rbt-run.log"
    # rbt run owns Composer and all four configured components.  Keep its PID
    # and process group so the final shutdown path is exact and auditable.
    setsid "${rbt_cmd[@]}" run >"${run_log}" 2>&1 &
    run_pid="$!"
    run_pgid="$(pid_process_group "${run_pid}")"
    printf 'rbt run pid=%s log=%s\n' "${run_pid}" "${run_log}"

    ps_json="${run_dir}/ps-active.json"
    wait_components_active "${ps_json}"
    assert_json ps-active "${ps_json}"
    assert_ps_projection ps-active "${ps_json}"
    run_logged ps-human "${rbt_cmd[@]}" ps

    graph_json="${run_dir}/graph-active.json"
    run_capture graph-active-json "${graph_json}" "${rbt_cmd[@]}" graph --json
    assert_json graph-active "${graph_json}"
    assert_graph_projection graph-active "${graph_json}"
    run_logged graph-active-human "${rbt_cmd[@]}" graph

    why_active_json="${run_dir}/why-active.json"
    run_capture why-active-json "${why_active_json}" "${rbt_cmd[@]}" why hello-sink --json
    assert_json why-active "${why_active_json}"
    assert_why_active why-active "${why_active_json}"
    run_logged why-active-human "${rbt_cmd[@]}" why hello-sink
    why_detector_json="${run_dir}/why-detector.json"
    run_capture why-detector-json "${why_detector_json}" "${rbt_cmd[@]}" why detector --json
    assert_json why-detector "${why_detector_json}"
    assert_why_active why-detector "${why_detector_json}" detector
    run_logged why-detector-human "${rbt_cmd[@]}" why detector

    capability_json="${run_dir}/capability-hello.json"
    run_capture capability-hello-json "${capability_json}" "${rbt_cmd[@]}" capability list --json
    assert_json capability-hello "${capability_json}"
    assert_capability_projection capability-hello "${capability_json}" Hello
    run_logged capability-human "${rbt_cmd[@]}" capability list

    # The onboarding selector is deliberately logical.  It never copies the
    # physical endpoint printed in verbose/JSON diagnostics into a command.
    stream_selector=Hello
    stream_info_json="${run_dir}/stream-info-hello.json"
    run_capture stream-info-hello-json "${stream_info_json}" "${rbt_cmd[@]}" stream info "${stream_selector}" --json
    assert_json stream-info-hello "${stream_info_json}"
    assert_stream_projection stream-info-hello "${stream_info_json}" Hello
    run_logged stream-info-human "${rbt_cmd[@]}" stream info "${stream_selector}"
    stream_pressure_json="${run_dir}/stream-pressure-hello.json"
    run_capture stream-pressure-hello-json "${stream_pressure_json}" "${rbt_cmd[@]}" stream pressure "${stream_selector}" --json
    assert_json stream-pressure-hello "${stream_pressure_json}"
    assert_pressure_projection stream-pressure-hello "${stream_pressure_json}" Hello
    run_logged stream-pressure-human "${rbt_cmd[@]}" stream pressure "${stream_selector}"

    composer_exe="${repo_root}/target/debug/composer"
    hello_source_exe="${project_root}/target/debug/hello-source"
    hello_sink_exe="${project_root}/target/debug/hello-sink"
    camera_exe="${project_root}/target/debug/camera"
    detector_exe="${project_root}/target/debug/detector"
    launcher_composer_pid="$(wait_direct_child_by_exe "${run_pid}" "${composer_exe}")"
    launcher_source_pid="$(wait_direct_child_by_exe "${run_pid}" "${hello_source_exe}")"
    launcher_sink_pid="$(wait_direct_child_by_exe "${run_pid}" "${hello_sink_exe}")"
    launcher_camera_pid="$(wait_direct_child_by_exe "${run_pid}" "${camera_exe}")"
    launcher_detector_pid="$(wait_direct_child_by_exe "${run_pid}" "${detector_exe}")"
    printf 'launcher-owned composer=%s hello-source=%s hello-sink=%s camera=%s detector=%s\n' \
        "${launcher_composer_pid}" "${launcher_source_pid}" "${launcher_sink_pid}" \
        "${launcher_camera_pid}" "${launcher_detector_pid}" \
        | tee "${run_dir}/launcher-pids.txt"
    launcher_composer_pgid="$(pid_process_group "${launcher_composer_pid}")"
    launcher_source_pgid="$(pid_process_group "${launcher_source_pid}")"
    launcher_sink_pgid="$(pid_process_group "${launcher_sink_pid}")"
    launcher_camera_pgid="$(pid_process_group "${launcher_camera_pid}")"
    launcher_detector_pgid="$(pid_process_group "${launcher_detector_pid}")"

    # Locate and stop only the launcher's direct hello-source child.  The
    # lookup above used both /proc/PPid and /proc/exe; no process-name match is
    # involved.  The launcher reaps this child while Composer retains the
    # source's last active binding for diagnostics.
    stop_exact_pid launcher-owned-hello-source "${launcher_source_pid}" "${hello_source_exe}" TERM \
        || die "could not stop the exact launcher-owned hello-source child"
    wait_pid_gone launcher-owned-hello-source "${launcher_source_pid}"

    blocked_ps_json="${run_dir}/ps-blocked.json"
    wait_component_state "${blocked_ps_json}" hello-sink BLOCKED
    assert_ps_projection ps-blocked "${blocked_ps_json}"
    run_logged ps-blocked-human "${rbt_cmd[@]}" ps
    graph_blocked_json="${run_dir}/graph-blocked.json"
    run_capture graph-blocked-json "${graph_blocked_json}" "${rbt_cmd[@]}" graph --json
    assert_json graph-blocked "${graph_blocked_json}"
    assert_graph_projection graph-blocked "${graph_blocked_json}"
    run_logged graph-blocked-human "${rbt_cmd[@]}" graph

    why_blocked_json="${run_dir}/why-blocked.json"
    local_why_ready=0
    for _ in {1..100}; do
        if "${rbt_cmd[@]}" why hello-sink --json >"${why_blocked_json}" \
            2>"${run_dir}/why-blocked.stderr.log" \
            && assert_why_blocked why-blocked "${why_blocked_json}"; then
            local_why_ready=1
            break
        fi
        sleep 0.1
    done
    ((local_why_ready == 1)) || {
        cat "${why_blocked_json}" >&2 || true
        cat "${run_dir}/why-blocked.stderr.log" >&2 || true
        die "why did not report an unresolved Hello requirement after provider loss"
    }
    run_logged why-blocked-human "${rbt_cmd[@]}" why hello-sink

    # Reconnect the exact built source binary as an external, explicitly
    # tracked process.  Composer should install a fresh capability and the
    # existing sink should return to ACTIVE without restarting the launcher.
    start_external hello-source-restarted "${hello_source_exe}" \
        env RBT_COMPOSER_ADDR="${RBT_COMPOSER_ADDR}" "${hello_source_exe}"
    restarted_source_pid="${last_external_pid}"
    restarted_source_pgid="$(pid_process_group "${restarted_source_pid}")"
    restarted_ps_json="${run_dir}/ps-restarted.json"
    wait_components_active "${restarted_ps_json}"
    assert_json ps-restarted "${restarted_ps_json}"
    run_logged ps-restarted-human "${rbt_cmd[@]}" ps
    why_restarted_json="${run_dir}/why-restarted.json"
    run_capture why-restarted-json "${why_restarted_json}" "${rbt_cmd[@]}" why hello-sink --json
    assert_json why-restarted "${why_restarted_json}"
    assert_why_active why-restarted "${why_restarted_json}"

    # The generated Hello source deliberately uses the frozen Image ABI, so
    # the same first graph completes MCAP without an internal endpoint or a
    # repository-only source fixture.
    recording_file="${run_dir}/onboarding.mcap"
    recording_json="${run_dir}/record.json"
    run_capture record "${recording_json}" "${rbt_cmd[@]}" record \
        --output "${recording_file}" --capability Hello --duration 0.8
    assert_record_summary record "${recording_json}"
    [[ -s "${recording_file}" ]] || die "record did not create a non-empty MCAP file"
    run_logged mcap-info-human "${rbt_cmd[@]}" mcap info "${recording_file}"
    mcap_info_json="${run_dir}/mcap-info.json"
    run_capture mcap-info-json "${mcap_info_json}" "${rbt_cmd[@]}" mcap info "${recording_file}" --json
    assert_mcap_info mcap-info "${mcap_info_json}"
    play_json="${run_dir}/play.json"
    run_capture play "${play_json}" "${rbt_cmd[@]}" play "${recording_file}" --rate 0
    assert_play_summary play "${play_json}"

    # Stop only the exact externally tracked processes before the launcher
    # audit.  The launcher itself still owns the configured four children.
    restarted_source_pid_for_audit="${restarted_source_pid}"
    stop_external_pid "${restarted_source_pid}"
    restarted_source_pid=''

    launcher_pid_before_shutdown="${run_pid}"
    kill -INT "${run_pid}" 2>/dev/null || true
    for _ in {1..120}; do
        kill -0 "${run_pid}" 2>/dev/null || break
        sleep 0.1
    done
    if kill -0 "${run_pid}" 2>/dev/null; then
        cat "${run_log}" >&2
        die "rbt run did not stop after SIGINT"
    fi
    wait "${run_pid}" 2>/dev/null || true
    run_pid=''
    launcher_run_pgid_for_audit="${run_pgid}"
    run_pgid=''
    printf 'PASS: graceful launcher shutdown completed\n'

    audit_exact_pids "${run_dir}/process-audit.txt" \
        "launcher|${launcher_pid_before_shutdown}|${rbt_binary}" \
        "composer|${launcher_composer_pid}|${composer_exe}" \
        "hello-source|${launcher_source_pid}|${hello_source_exe}" \
        "hello-sink|${launcher_sink_pid}|${hello_sink_exe}" \
        "camera|${launcher_camera_pid}|${camera_exe}" \
        "detector|${launcher_detector_pid}|${detector_exe}" \
        "restarted-hello-source|${restarted_source_pid_for_audit}|${hello_source_exe}"
    audit_process_groups "${run_dir}/process-group-audit.txt" \
        "launcher|${launcher_run_pgid_for_audit}" \
        "composer|${launcher_composer_pgid}" \
        "hello-source|${launcher_source_pgid}" \
        "hello-sink|${launcher_sink_pgid}" \
        "camera|${launcher_camera_pgid}" \
        "detector|${launcher_detector_pgid}" \
        "restarted-hello-source|${restarted_source_pgid}"
fi
popd >/dev/null

if [[ "${mode}" == full ]]; then
    if [[ "${RBT_DX_SKIP_RUNTIME:-0}" == 1 ]]; then
        printf 'SKIP: full restart/forced-shutdown lanes explicitly disabled by RBT_DX_SKIP_RUNTIME=1\n'
    else
        run_full_restart_hammer "${project_root}"
        run_full_forced_shutdown "${project_root}"
    fi
    run_broken_diagnostics
fi

if [[ "${mode}" == full && "${RBT_DX_SKIP_INHERITED:-0}" != 1 ]]; then
    # The pressure full gate is the deduplicated inherited release
    # composition: v0.1 runtime/interop/install/sanitizers, WaitSet, MCAP,
    # Python wheel/component/asyncio lanes, provider replacement, and one
    # representative 300-second pressure soak.  Its explicit deduplication
    # flag keeps long MCAP/component/asyncio soaks out of this parent gate.
    # Inherited gates choose isolated addresses; do not pass the DX runtime address.
    run_logged inherited-release-full env -u RBT_COMPOSER_ADDR \
        RBT_PRESSURE_TUNING_DEDUP_LONG_SOAKS=1 \
        ./scripts/verify_pressure_tuning.sh --full
else
    if [[ "${mode}" == full ]]; then
        printf 'SKIP: inherited gates explicitly disabled by RBT_DX_SKIP_INHERITED=1\n'
    fi
fi

printf 'PASS: rbt DX %s verification completed; logs=%s\n' "${mode}" "${run_dir}"

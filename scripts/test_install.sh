#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

die() {
    printf 'ERROR: %s\n' "$*" >&2
    [[ -z "${work_root:-}" || ! -d "${work_root}" ]] || \
        printf 'Retained install-smoke logs: %s\n' "${work_root}" >&2
    exit 1
}

print_command() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
}

for command_name in bash cargo cmake find mktemp python3 readlink rg setsid sha256sum tar; do
    command -v "${command_name}" >/dev/null 2>&1 \
        || die "required command not found: ${command_name}"
done

# The install acceptance lane must not access the network, including when the
# generated project's default build is run by `rbt run`.
export CARGO_NET_OFFLINE=true

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
cpp_prefix="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"
bash "${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${cpp_prefix}"
work_root="$(mktemp -d "${TMPDIR:-/tmp}/rbt-install-smoke.XXXXXX")"
log_root="${work_root}/logs"
install_prefix="${work_root}/prefix"
rust_consumer="${work_root}/rust-consumer"
cpp_consumer="${work_root}/cpp-consumer"
installed_home="${work_root}/home"
installed_projects="${work_root}/installed-projects"
mkdir -p "${log_root}"

launcher_pid=''
launcher_pgid=''
launcher_reaped=0
launcher_log=''
declare -A child_pid=()
declare -A child_pgid=()
declare -A child_sid=()
declare -A child_exe=()

real_exe() {
    readlink -f -- "$1" 2>/dev/null || true
}

proc_metadata() {
    local pid="$1"
    python3 - "${pid}" <<'PY'
import os
import pathlib
import sys

pid = sys.argv[1]
proc = pathlib.Path("/proc") / pid
try:
    stat = (proc / "stat").read_text()
    close = stat.rfind(")")
    fields = stat[close + 2 :].split()
    state, ppid, pgid, sid = fields[0:4]
except (FileNotFoundError, PermissionError, OSError, IndexError, ValueError):
    raise SystemExit(1)
try:
    exe = os.path.realpath(os.readlink(proc / "exe"))
except (FileNotFoundError, PermissionError, OSError):
    exe = ""
print("|".join((state, ppid, pgid, sid, exe)))
PY
}

proc_state() {
    local metadata
    metadata="$(proc_metadata "$1" 2>/dev/null || true)"
    [[ -n "${metadata}" ]] || return 1
    printf '%s\n' "${metadata%%|*}"
}

run_logged() {
    local label="$1"
    shift
    local log_file="${log_root}/${label}.log"
    print_command "$@"
    set +e
    "$@" >"${log_file}" 2>&1
    local status=$?
    set -e
    cat "${log_file}"
    if ((status != 0)); then
        cat "${log_file}" >&2
        die "${label} failed (exit=${status}); see ${log_file}"
    fi
}

run_capture() {
    local label="$1"
    local output_file="$2"
    shift 2
    local stderr_file="${log_root}/${label}.stderr.log"
    print_command "$@"
    set +e
    "$@" >"${output_file}" 2>"${stderr_file}"
    local status=$?
    set -e
    cat "${output_file}"
    if ((status != 0)); then
        cat "${stderr_file}" >&2
        die "${label} failed (exit=${status}); see ${stderr_file}"
    fi
}

assert_installed_docs() {
    local docs_root="${install_prefix}/share/rbt"
    local artifact source relative
    for artifact in README.md LICENSE-APACHE LICENSE-MIT THIRD_PARTY_NOTICES.md; do
        [[ -f "${docs_root}/${artifact}" ]] \
            || die "installed documentation/license is missing: ${docs_root}/${artifact}"
    done
    [[ -d "${docs_root}/docs" ]] \
        || die "installed docs directory is missing: ${docs_root}/docs"
    while IFS= read -r -d '' source; do
        relative="${source#docs/}"
        [[ -f "${docs_root}/docs/${relative}" ]] \
            || die "installed docs file is missing: ${docs_root}/docs/${relative}"
    done < <(find docs -type f -print0)
    printf 'PASS: installed README, licenses, notices, and complete docs tree exist\n'
}

assert_installed_rpc_artifacts() {
    local rust_root="${install_prefix}/share/rbt/rust"
    local artifact
    for artifact in \
        "${install_prefix}/include/rbt/rpc.hpp" \
        "${install_prefix}/include/rbt/demo.hpp" \
        "${install_prefix}/lib/librbt_runtime.a" \
        "${install_prefix}/lib/cmake/rbt/rbtConfig.cmake" \
        "${rust_root}/crates/rbt-iox2/src/rpc.rs" \
        "${rust_root}/generated/rust/demo.rs" \
        "${rust_root}/generated/cpp/rbt/demo.hpp" \
        "${rust_root}/generated/descriptors/demo.json" \
        "${install_prefix}/share/rbt/docs/rpc.md" \
        "${install_prefix}/share/rbt/docs/rpc-iox2-notes.md"; do
        [[ -f "${artifact}" ]] || die "installed RPC artifact is missing: ${artifact}"
    done
    printf 'PASS: installed RPC API, canonical generated types/descriptors, runtime, and docs exist\n'
}

assert_installed_iceoryx2_source() {
    local rust_root="${install_prefix}/share/rbt/rust"
    local fork_root="${rust_root}/third_party/iceoryx2-rbt"
    local native_source="${fork_root}/source"
    local git_metadata
    [[ -f "${rust_root}/Cargo.lock" ]] \
        || die "installed Rust SDK lockfile is missing"
    [[ -f "${rust_root}/Cargo.toml" ]] \
        || die "installed Rust SDK workspace manifest is missing"
    [[ -f "${rust_root}/Cargo.toml" ]] \
        && rg -Fq -- 'iceoryx2 = { path = "third_party/iceoryx2-rbt/source/iceoryx2" }' \
            "${rust_root}/Cargo.toml" \
        || die "installed Rust SDK does not patch iceoryx2 to its bundled fork"
    [[ -f "${native_source}/iceoryx2/Cargo.toml" \
        && -f "${fork_root}/UPSTREAM.toml" \
        && -f "${fork_root}/SOURCE_DIFF_SHA256" \
        && -f "${fork_root}/patches/series" \
        && -f "${native_source}/LICENSE-APACHE" \
        && -f "${native_source}/LICENSE-MIT" ]] \
        || die "installed patched iceoryx2 source, metadata, patch series, or licenses are incomplete"
    git_metadata="$(find "${fork_root}" -name .git -print -quit)"
    [[ -z "${git_metadata}" ]] \
        || die "installed native source unexpectedly contains Git metadata: ${git_metadata}"
    [[ -z "$(find "${native_source}" -type d -name target -print -quit)" ]] \
        || die "installed native source contains build-target files"
    printf 'PASS: installed Rust workspace carries patched iceoryx2 source without Git/build artifacts\n'
}

assert_no_source_checkout() {
    local label="$1"
    local root="$2"
    local matches="${log_root}/${label}-source-root.log"
    set +e
    rg --hidden --fixed-strings --glob '!target/**' "${repo_root}" "${root}" \
        >"${matches}" 2>&1
    local status=$?
    set -e
    if ((status == 0)); then
        cat "${matches}" >&2
        die "${label} generated files refer to the source repository; see ${matches}"
    fi
    ((status == 1)) || die "could not scan ${label} for source-checkout references; see ${matches}"
    printf 'PASS: %s generated files contain no source-checkout path\n' "${label}"
}

assert_doctor_json() {
    local label="$1"
    local json_file="$2"
    python3 - "${label}" "${json_file}" <<'PY'
import json
import pathlib
import sys

label, filename = sys.argv[1:]
try:
    report = json.loads(pathlib.Path(filename).read_text())
except Exception as exc:
    raise SystemExit(f"{label}: invalid JSON: {exc}")
if not isinstance(report, dict):
    raise SystemExit(f"{label}: expected a JSON object")
if report.get("platform") != "linux-x86_64":
    raise SystemExit(f"{label}: unexpected platform: {report.get('platform')!r}")
if report.get("success") is not True:
    raise SystemExit(f"{label}: success is not true")
checks = report.get("checks")
if not isinstance(checks, list) or not checks:
    raise SystemExit(f"{label}: checks must be a non-empty list")
for check in checks:
    if not isinstance(check, dict) or not isinstance(check.get("check_id"), str):
        raise SystemExit(f"{label}: malformed diagnostic entry")
    if check.get("status") not in ("PASS", "WARN", "SKIP"):
        raise SystemExit(f"{label}: unexpected diagnostic status: {check.get('status')!r}")
required = ("platform", "rbt_cli", "composer_tool", "rbt-idl_tool", "rust_sdk", "rust_toolchain", "shm", "tmp")
by_id = {check["check_id"]: check for check in checks}
missing = [check_id for check_id in required if check_id not in by_id]
if missing:
    raise SystemExit(f"{label}: missing required checks: {missing}")
not_pass = [check_id for check_id in required if by_id[check_id].get("status") != "PASS"]
if not_pass:
    details = {check_id: by_id[check_id].get("status") for check_id in not_pass}
    raise SystemExit(f"{label}: required checks are not PASS: {details}")
print(f"PASS: {label} has a successful structured doctor report")
PY
}

assert_ps_active() {
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
if not isinstance(value, dict) or not isinstance(value.get("components"), list):
    raise SystemExit(f"{label}: expected ps object with components")
states = dict((item.get("component_id"), item.get("state")) for item in value["components"] if isinstance(item, dict))
expected = ("hello-source", "hello-sink")
if any(name not in states or states[name] != "ACTIVE" for name in expected):
    raise SystemExit(f"{label}: expected hello-source and hello-sink ACTIVE, got {states}")
if not isinstance(value.get("process_conflicts"), list):
    raise SystemExit(f"{label}: missing process_conflicts list")
print(f"PASS: {label} reports hello-source and hello-sink ACTIVE")
PY
}

choose_composer_address() {
    python3 <<'PY'
import socket

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
try:
    sock.bind(("127.0.0.1", 0))
    print(f"127.0.0.1:{sock.getsockname()[1]}")
finally:
    sock.close()
PY
}

wait_for_pid_exit() {
    local pid="$1"
    local attempts="${2:-150}"
    local state
    for ((attempt = 0; attempt < attempts; ++attempt)); do
        [[ ! -e "/proc/${pid}" ]] && return 0
        state="$(proc_state "${pid}" 2>/dev/null || true)"
        [[ "${state}" == Z ]] && return 0
        sleep 0.1
    done
    return 1
}

terminate_group_if_exact() {
    local label="$1"
    local pid="$2"
    local pgid="$3"
    local sid="$4"
    local expected="$5"
    local signal_name="${6:-TERM}"
    local metadata state ppid current_pgid current_sid actual
    [[ -n "${expected}" ]] || return 1
    metadata="$(proc_metadata "${pid}" 2>/dev/null || true)"
    [[ -n "${metadata}" ]] || return 0
    IFS='|' read -r state ppid current_pgid current_sid actual <<<"${metadata}"
    if [[ "${actual}" != "$(real_exe "${expected}")" \
        || "${current_pgid}" != "${pgid}" || "${current_sid}" != "${sid}" ]]; then
        printf 'WARN: refusing to signal %s pid=%s; /proc identity/group changed\n' \
            "${label}" "${pid}" >&2
        return 1
    fi
    kill -"${signal_name}" -- "-${pgid}" 2>/dev/null || true
}

parse_started_children() {
    [[ -n "${launcher_log}" && -f "${launcher_log}" ]] || return 0
    local name pid
    while IFS='|' read -r name pid; do
        [[ -n "${name}" && -n "${pid}" ]] || continue
        case "${name}" in
            composer)
                child_pid[composer]="${pid}"
                child_exe[composer]="${install_prefix}/bin/composer"
                ;;
            hello-source)
                child_pid[hello-source]="${pid}"
                child_exe[hello-source]="${installed_project}/target/debug/hello-source"
                ;;
            hello-sink)
                child_pid[hello-sink]="${pid}"
                child_exe[hello-sink]="${installed_project}/target/debug/hello-sink"
                ;;
        esac
    done < <(python3 - "${launcher_log}" <<'PY'
import pathlib
import re
import sys

pattern = re.compile(r"^\[rbt\] started ([^ ]+) pid=([0-9]+)\s*$")
seen = set()
for line in pathlib.Path(sys.argv[1]).read_text(errors="replace").splitlines():
    match = pattern.match(line)
    if match and match.group(1) not in seen:
        seen.add(match.group(1))
        print(f"{match.group(1)}|{match.group(2)}")
PY
    )
}

record_process_metadata() {
    local label="$1"
    local pid="$2"
    local expected="$3"
    local metadata state ppid pgid sid actual
    metadata="$(proc_metadata "${pid}" 2>/dev/null || true)"
    [[ -n "${metadata}" ]] || die "${label} pid=${pid} disappeared before /proc audit"
    IFS='|' read -r state ppid pgid sid actual <<<"${metadata}"
    [[ "${state}" != Z ]] || die "${label} pid=${pid} is already a zombie"
    [[ "${actual}" == "$(real_exe "${expected}")" ]] \
        || die "${label} pid=${pid} executable is ${actual}, expected $(real_exe "${expected}")"
    [[ "${pgid}" == "${pid}" && "${sid}" == "${pid}" ]] \
        || die "${label} pid=${pid} is not an isolated process group/session (pgid=${pgid}, sid=${sid})"
    printf '%s|%s|%s|%s|%s|%s\n' "${label}" "${pid}" "${ppid}" "${pgid}" "${sid}" "${actual}" \
        >>"${log_root}/process-groups.before"
}

require_started_children() {
    parse_started_children
    local name metadata state ppid pgid sid actual
    for name in composer hello-source hello-sink; do
        [[ -n "${child_pid[${name}]:-}" ]] \
            || die "installed rbt run log did not identify ${name} with '[rbt] started ${name} pid=N'"
        record_process_metadata "${name}" "${child_pid[${name}]}" "${child_exe[${name}]}"
        metadata="$(proc_metadata "${child_pid[${name}]}" 2>/dev/null || true)"
        IFS='|' read -r state ppid pgid sid actual <<<"${metadata}"
        child_pgid[${name}]="${pgid}"
        child_sid[${name}]="${sid}"
    done
}

audit_no_processes_or_groups() {
    local audit_file="$1"
    shift
    python3 - "${audit_file}" "$@" <<'PY'
import os
import pathlib
import sys

audit_file = pathlib.Path(sys.argv[1])
records = []
for spec in sys.argv[2:]:
    label, pid_text, pgid_text, sid_text = spec.split("|", 3)
    records.append((label, int(pid_text), int(pgid_text), int(sid_text)))

def stat_info(pid):
    proc = pathlib.Path("/proc") / str(pid)
    try:
        stat = (proc / "stat").read_text()
        close = stat.rfind(")")
        fields = stat[close + 2 :].split()
        try:
            exe = os.path.realpath(os.readlink(proc / "exe"))
        except (FileNotFoundError, PermissionError, OSError):
            exe = ""
        return fields[0], int(fields[2]), int(fields[3]), exe
    except (FileNotFoundError, PermissionError, OSError, IndexError, ValueError):
        return None

lines = []
failed = False
for label, pid, pgid, sid in records:
    info = stat_info(pid)
    if info is not None:
        state, current_pgid, current_sid, exe = info
        lines.append(f"{label} pid={pid} state={state} pgid={current_pgid} sid={current_sid} exe={exe}")
        failed = True
    else:
        lines.append(f"{label} pid={pid} gone")
    members = []
    for entry in pathlib.Path("/proc").iterdir():
        if entry.name.isdigit():
            info = stat_info(int(entry.name))
            if info is not None and info[1] == pgid:
                members.append((int(entry.name), info[0], info[3]))
    if members:
        failed = True
        lines.append(f"{label} process-group pgid={pgid} still has members: {members}")
    else:
        lines.append(f"{label} process-group pgid={pgid} gone")

audit_file.write_text("\n".join(lines) + "\n")
print(audit_file.read_text(), end="")
if failed:
    raise SystemExit(1)
print("PASS: /proc audit found no live or zombie launcher/child process or process-group member")
PY
}

cleanup() {
    local status=$?
    trap - EXIT
    set +e

    if [[ -n "${launcher_pid}" && "${launcher_reaped}" != 1 ]]; then
        parse_started_children
        local metadata state ppid pgid sid actual
        metadata="$(proc_metadata "${launcher_pid}" 2>/dev/null || true)"
        if [[ -n "${metadata}" ]]; then
            IFS='|' read -r state ppid pgid sid actual <<<"${metadata}"
            if [[ -n "${rbt_binary:-}" && "${actual}" == "$(real_exe "${rbt_binary}")" ]]; then
                launcher_pgid="${pgid}"
                terminate_group_if_exact launcher "${launcher_pid}" "${pgid}" "${sid}" \
                    "${rbt_binary}" TERM || true
                wait_for_pid_exit "${launcher_pid}" 100 || \
                    terminate_group_if_exact launcher "${launcher_pid}" "${pgid}" "${sid}" \
                        "${rbt_binary}" KILL || true
            fi
        fi
        wait "${launcher_pid}" 2>/dev/null || true
        launcher_reaped=1
    fi

    # If the launcher failed before reaping its children, signal only the
    # exact child sessions captured from its [rbt] started lines.
    local name child
    for name in composer hello-source hello-sink; do
        child="${child_pid[${name}]:-}"
        [[ -n "${child}" ]] || continue
        if [[ -z "${child_pgid[${name}]:-}" || -z "${child_sid[${name}]:-}" ]]; then
            metadata="$(proc_metadata "${child}" 2>/dev/null || true)"
            if [[ -n "${metadata}" ]]; then
                IFS='|' read -r state ppid pgid sid actual <<<"${metadata}"
                if [[ -n "${child_exe[${name}]:-}" \
                    && "${actual}" == "$(real_exe "${child_exe[${name}]}")" ]]; then
                    child_pgid[${name}]="${pgid}"
                    child_sid[${name}]="${sid}"
                fi
            fi
        fi
        if [[ -n "${child_pgid[${name}]:-}" && -n "${child_sid[${name}]:-}" ]]; then
            terminate_group_if_exact "${name}" "${child}" "${child_pgid[${name}]}" \
                "${child_sid[${name}]}" "${child_exe[${name}]}" TERM || true
        fi
    done
    for name in composer hello-source hello-sink; do
        child="${child_pid[${name}]:-}"
        [[ -n "${child}" ]] || continue
        if ! wait_for_pid_exit "${child}" 100; then
            metadata="$(proc_metadata "${child}" 2>/dev/null || true)"
            if [[ -n "${metadata}" ]]; then
                IFS='|' read -r state ppid pgid sid actual <<<"${metadata}"
                if [[ -n "${child_exe[${name}]:-}" \
                    && "${actual}" == "$(real_exe "${child_exe[${name}]}")" ]]; then
                    terminate_group_if_exact "${name}" "${child}" "${pgid}" "${sid}" \
                        "${child_exe[${name}]}" KILL || true
                fi
            fi
            wait_for_pid_exit "${child}" 50 || true
        fi
    done

    if ((status == 0)); then
        rm -rf -- "${work_root}"
    else
        printf 'Install-smoke failed; logs retained at %s\n' "${work_root}" >&2
    fi
    exit "${status}"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

install_args=(./scripts/install_v01.sh --prefix "${install_prefix}")
if [[ -n "${RBT_INSTALL_PYTHON_WHEEL:-}" ]]; then
    install_args+=(--python-wheel "${RBT_INSTALL_PYTHON_WHEEL}")
fi
run_logged install "${install_args[@]}"

for binary in rbt rbt-idl composer; do
    [[ -x "${install_prefix}/bin/${binary}" ]] \
        || die "installed binary is missing: ${binary}"
done
assert_installed_docs
assert_installed_rpc_artifacts
assert_installed_iceoryx2_source

if [[ -n "${RBT_INSTALL_PYTHON_WHEEL:-}" ]]; then
    installed_wheel="${install_prefix}/share/rbt/python/$(basename "${RBT_INSTALL_PYTHON_WHEEL}")"
    [[ -f "${installed_wheel}" ]] \
        || die "installed Python wheel is missing: ${installed_wheel}"
    printf 'PASS: installed Python wheel staged at %s\n' "${installed_wheel}"
fi

mkdir -p "${rust_consumer}/src" "${cpp_consumer}"
cp tests/consumers/rust/Cargo.toml.in "${rust_consumer}/Cargo.toml"
cp tests/consumers/rust/src/main.rs "${rust_consumer}/src/main.rs"
escaped_sdk="$(printf '%s' "${install_prefix}/share/rbt/rust/crates/rbt" | sed 's/[&|]/\\&/g')"
sed -i "s|@RBT_RUST_SDK@|${escaped_sdk}|g" "${rust_consumer}/Cargo.toml"
if grep -Fq "${repo_root}" "${rust_consumer}/Cargo.toml"; then
    die "fresh Rust consumer refers to the source repository"
fi

if [[ -x "${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc" ]]; then
    export PATH="${toolchain_root}/bin${PATH:+:${PATH}}"
    export CC="${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc"
    export CXX="${toolchain_root}/bin/x86_64-conda-linux-gnu-g++"
    export AR="${toolchain_root}/bin/x86_64-conda-linux-gnu-ar"
    export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
    export LD_LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
    export LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"
else
    command -v cc >/dev/null 2>&1 || die "a C compiler is required"
    command -v c++ >/dev/null 2>&1 || die "a C++ compiler is required"
    export CC="${CC:-$(command -v cc)}"
    export CXX="${CXX:-$(command -v c++)}"
    export LD_LIBRARY_PATH="${cpp_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
    export LIBRARY_PATH="${cpp_prefix}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"
fi

run_logged installed-rust-sdk-build cargo build \
    --manifest-path "${install_prefix}/share/rbt/rust/Cargo.toml" \
    --target-dir "${work_root}/installed-rust-sdk-target" \
    --offline --locked --package rbt-iox2 --verbose
rg -Fq -- "${install_prefix}/share/rbt/rust/third_party/iceoryx2-rbt/source/iceoryx2" \
    "${log_root}/installed-rust-sdk-build.log" \
    || die "installed Rust SDK did not build against its bundled iceoryx2 fork"

run_logged rust-consumer cargo run --manifest-path "${rust_consumer}/Cargo.toml" --offline
installed_native_id="$(cargo pkgid --manifest-path "${rust_consumer}/Cargo.toml" --offline -p iceoryx2)"
[[ "${installed_native_id}" == "path+file://${install_prefix}/share/rbt/rust/third_party/iceoryx2-rbt/source/iceoryx2#0.10.0" ]] \
    || die "external Rust consumer resolved a different native source: ${installed_native_id}"
printf 'PASS: external Rust consumer directly resolves the bundled native source\n'
rg -Fq -- 'RBT_RPC_INSTALL_SMOKE_OK rust' "${log_root}/rust-consumer.log" \
    || die "Rust consumer did not execute its installed fixed RPC call"

cp tests/consumers/cpp/CMakeLists.txt tests/consumers/cpp/main.cpp "${cpp_consumer}/"
run_logged cpp-consumer-configure cmake -S "${cpp_consumer}" -B "${cpp_consumer}/build" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER="${CXX}" \
    -DCMAKE_PREFIX_PATH="${install_prefix};${cpp_prefix}"
run_logged cpp-consumer-build cmake --build "${cpp_consumer}/build" --parallel 2
run_logged cpp-consumer-run "${cpp_consumer}/build/rbt-installed-cpp-consumer"
rg -Fq -- 'RBT_RPC_INSTALL_SMOKE_OK cpp' "${log_root}/cpp-consumer-run.log" \
    || die "C++ consumer did not execute its installed fixed RPC call"

# Use an empty HOME and no SDK override for the fresh-install lane.  Existing
# Cargo/rustup caches are retained explicitly when present so the offline
# build can use the preloaded toolchain without reading user configuration.
original_home="${HOME:-}"
if [[ -z "${CARGO_HOME:-}" && -n "${original_home}" && -d "${original_home}/.cargo" ]]; then
    export CARGO_HOME="${original_home}/.cargo"
fi
if [[ -z "${RUSTUP_HOME:-}" && -n "${original_home}" && -d "${original_home}/.rustup" ]]; then
    export RUSTUP_HOME="${original_home}/.rustup"
fi
mkdir -p "${installed_home}" "${installed_projects}"
export HOME="${installed_home}"
unset RBT_RUST_SDK
export PATH="${install_prefix}/bin:${PATH}"
export RBT_COMPOSER_ADDR="$(choose_composer_address)"
[[ "${RBT_COMPOSER_ADDR}" =~ ^127\.0\.0\.1:[0-9]+$ ]] \
    || die "isolated Composer address is not loopback: ${RBT_COMPOSER_ADDR}"

rbt_cmd=("${install_prefix}/bin/rbt")
rbt_binary="${rbt_cmd[0]}"

pushd "${installed_projects}" >/dev/null
run_logged installed-doctor "${rbt_cmd[@]}" doctor
run_capture installed-doctor-json "${log_root}/installed-doctor.json" "${rbt_cmd[@]}" doctor --json
assert_doctor_json installed-doctor "${log_root}/installed-doctor.json"
run_logged installed-new "${rbt_cmd[@]}" new installed-hello
installed_project="${installed_projects}/installed-hello"
[[ -f "${installed_project}/rbt.toml" ]] \
    || die "installed rbt new did not create rbt.toml"
assert_no_source_checkout installed-new "${installed_project}"
popd >/dev/null

pushd "${installed_project}" >/dev/null
run_logged installed-check "${rbt_cmd[@]}" check
run_logged installed-idl-check "${rbt_cmd[@]}" idl check
run_logged installed-idl-fmt-check "${rbt_cmd[@]}" idl fmt --check
run_logged installed-idl-generate "${rbt_cmd[@]}" idl generate
run_logged installed-cargo-build cargo build --workspace --offline
assert_no_source_checkout installed-generated "${installed_project}"

launcher_log="${log_root}/installed-rbt-run.log"
setsid -- "${rbt_cmd[@]}" run >"${launcher_log}" 2>&1 &
launcher_pid="$!"
printf 'installed rbt run launcher pid=%s address=%s log=%s\n' \
    "${launcher_pid}" "${RBT_COMPOSER_ADDR}" "${launcher_log}"

launcher_ready=0
for ((attempt = 0; attempt < 100; ++attempt)); do
    metadata="$(proc_metadata "${launcher_pid}" 2>/dev/null || true)"
    if [[ -n "${metadata}" ]]; then
        IFS='|' read -r launcher_state launcher_ppid launcher_pgid launcher_sid launcher_exe <<<"${metadata}"
        if [[ "${launcher_exe}" == "$(real_exe "${rbt_binary}")" ]]; then
            launcher_ready=1
            break
        fi
    fi
    sleep 0.1
done
((launcher_ready == 1)) || {
    cat "${launcher_log}" >&2 || true
    die "installed rbt run launcher did not become the installed rbt executable"
}
[[ "${launcher_pgid}" == "${launcher_pid}" && "${launcher_sid}" == "${launcher_pid}" ]] \
    || die "launcher is not an isolated process group/session (pgid=${launcher_pgid}, sid=${launcher_sid})"
printf '%s|%s|%s|%s|%s|%s\n' "launcher" "${launcher_pid}" "${launcher_ppid}" \
    "${launcher_pgid}" "${launcher_sid}" "${launcher_exe}" \
    >"${log_root}/process-groups.before"

ps_json="${log_root}/installed-ps-active.json"
active=0
for ((attempt = 0; attempt < 180; ++attempt)); do
    if "${rbt_cmd[@]}" ps --json >"${ps_json}" 2>"${log_root}/installed-ps-active.stderr.log" \
        && assert_ps_active installed-ps-active "${ps_json}"; then
        active=1
        break
    fi
    if ! [[ -e "/proc/${launcher_pid}" ]]; then
        cat "${launcher_log}" >&2 || true
        die "installed rbt run exited before components became ACTIVE"
    fi
    sleep 0.2
done
((active == 1)) || {
    cat "${ps_json}" >&2 || true
    cat "${log_root}/installed-ps-active.stderr.log" >&2 || true
    cat "${launcher_log}" >&2 || true
    die "timed out waiting for installed hello-source and hello-sink to become ACTIVE"
}
require_started_children

launcher_before_pid="${launcher_pid}"
launcher_before_pgid="${launcher_pgid}"
launcher_before_sid="${launcher_sid}"
kill -TERM "${launcher_pid}" 2>/dev/null \
    || die "could not send SIGTERM to the exact installed rbt launcher pid=${launcher_pid}"
if ! wait_for_pid_exit "${launcher_pid}" 150; then
    cat "${launcher_log}" >&2 || true
    terminate_group_if_exact launcher "${launcher_pid}" "${launcher_pgid}" "${launcher_sid}" \
        "${rbt_binary}" KILL || true
    wait_for_pid_exit "${launcher_pid}" 50 || die "installed rbt launcher did not exit within bounded shutdown"
fi
set +e
wait "${launcher_pid}"
launcher_status=$?
set -e
launcher_reaped=1
launcher_pid=''
((launcher_status == 0)) || {
    cat "${launcher_log}" >&2 || true
    die "installed rbt run did not exit cleanly after SIGTERM (exit=${launcher_status})"
}
printf 'PASS: installed rbt launcher stopped cleanly after SIGTERM\n'

audit_specs=(
    "launcher|${launcher_before_pid}|${launcher_before_pgid}|${launcher_before_sid}"
    "composer|${child_pid[composer]}|${child_pgid[composer]}|${child_sid[composer]}"
    "hello-source|${child_pid[hello-source]}|${child_pgid[hello-source]}|${child_sid[hello-source]}"
    "hello-sink|${child_pid[hello-sink]}|${child_pgid[hello-sink]}|${child_sid[hello-sink]}"
)
audit_no_processes_or_groups "${log_root}/process-audit.txt" "${audit_specs[@]}"
popd >/dev/null

printf 'PASS: installed Rust/C++ consumers and fresh installed-project/runtime acceptance completed\n'

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
usage: scripts/verify_python_publisher.sh [--quick|--full]

--quick  Run the Python verifier's focused publisher and receive gates plus
         the publisher timing sanity check.
--full   Quick plus the verifier's complete publisher, sanitizer, wheel,
         pressure, and existing regression gates.

Environment overrides:
  RBT_PYTHON_PUBLISHER_LOG_DIR  retained wrapper evidence root
  RBT_PYTHON_LOG_DIR            base verifier evidence root (optional)
  RBT_PYTHON_VERIFY_VENV        reusable verifier virtualenv
EOF
}

mode=quick
if (($# > 1)); then
    usage >&2
    exit 2
fi
if (($# == 1)); then
    case "$1" in
        --quick) mode=quick ;;
        --full) mode=full ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
fi

command -v date >/dev/null 2>&1 || die "required command not found: date"
command -v python3 >/dev/null 2>&1 || die "required command not found: python3"

log_root="${RBT_PYTHON_PUBLISHER_LOG_DIR:-${repo_root}/target/python-publisher-verify-logs}"
run_id="python-publisher-${mode}-$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="${log_root}/${run_id}"
mkdir -p "${run_dir}"

run_logged() {
    local label="$1"
    shift
    local log_file="${run_dir}/${label}.log"
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    set +e
    "$@" >"${log_file}" 2>&1
    local status=$?
    set -e
    cat "${log_file}"
    ((status == 0)) || die "${label} failed with exit ${status}; see ${log_file}"
}

base_log_dir="${RBT_PYTHON_LOG_DIR:-${run_dir}/base}"
run_logged base-python-verifier env \
    RBT_PYTHON_LOG_DIR="${base_log_dir}" \
    "${repo_root}/scripts/verify_python.sh" "--${mode}"

verify_venv="${RBT_PYTHON_VERIFY_VENV:-${repo_root}/target/python-verify-venv}"
verify_python="${verify_venv}/bin/python"
[[ -x "${verify_python}" ]] || die "Python verifier interpreter is missing: ${verify_python}"
run_logged publisher-benchmark "${verify_python}" \
    "${repo_root}/scripts/benchmark_python_publisher.py"

printf 'PASS: Python publisher %s verification completed; logs=%s\n' \
    "${mode}" "${run_dir}"

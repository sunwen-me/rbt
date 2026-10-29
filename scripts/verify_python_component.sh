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
usage: scripts/verify_python_component.sh [--quick|--full]

--quick  Python Component API, wheel, mixed graph, lifecycle/failure/recovery,
         workspace tests, strict clippy, and existing Python RX/TX quick gates.
--full   Quick semantics plus fresh-wheel 100-cycle/five-minute soak and all
         inherited Rust/C++/Python/WaitSet/pressure/v0.1/sanitizer gates.

Environment overrides:
  RBT_PYTHON                 bootstrap CPython 3.10+
  RBT_PHASE4_TOOLCHAIN       compiler/toolchain root
  RBT_IOX2_PREFIX            iceoryx2 C++ install prefix
  RBT_PYTHON_COMPONENT_LOG_DIR
  RBT_PYTHON_COMPONENT_SOAK_SECONDS (default 300 in --full)
  RBT_PYTHON_COMPONENT_SKIP_INHERITED=1
      skip the base Python gate when a parent verifier schedules it separately
EOF
}

mode=quick
if (($# > 1)); then
    usage >&2
    exit 2
fi

skip_inherited="${RBT_PYTHON_COMPONENT_SKIP_INHERITED:-0}"
[[ "${skip_inherited}" =~ ^[01]$ ]] \
    || die "RBT_PYTHON_COMPONENT_SKIP_INHERITED must be 0 or 1"
if (($# == 1)); then
    case "$1" in
        --quick) mode=quick ;;
        --full) mode=full ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
fi

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
toolchain_bin="${toolchain_root}/bin"
cpp_prefix="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"
"${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${cpp_prefix}"
default_python="/home/sw/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3"
[[ -x "${default_python}" ]] || default_python="$(command -v python3 || true)"
bootstrap_python="${RBT_PYTHON:-${default_python}}"
verify_venv="${RBT_PYTHON_VERIFY_VENV:-${repo_root}/target/python-verify-venv}"
log_root="${RBT_PYTHON_COMPONENT_VERIFY_LOG_DIR:-${repo_root}/target/python-component-verify-logs}"
mkdir -p "${log_root}"
run_id="python-component-${mode}-$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="${log_root}/${run_id}"
mkdir -p "${run_dir}"
tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/rbt-python-component-verify.XXXXXX")"
trap 'rm -rf -- "${tmp_root}"' EXIT

for command_name in cargo rustc cmake ctest rg diff unzip readelf strings ldd; do
    command -v "${command_name}" >/dev/null 2>&1 \
        || die "required command not found: ${command_name}"
done
[[ -x "${bootstrap_python}" ]] || die "CPython is missing: ${bootstrap_python}"
[[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-gcc" ]] || die "C compiler missing"
[[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-g++" ]] || die "C++ compiler missing"
[[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-ar" ]] || die "archiver missing"
[[ -f "${cpp_prefix}/lib/cmake/iceoryx2-cxx/iceoryx2-cxxConfig.cmake" ]] \
    || die "iceoryx2 C++ package is missing"

export PATH="${toolchain_bin}${PATH:+:${PATH}}"
export CC="${toolchain_bin}/x86_64-conda-linux-gnu-gcc"
export CXX="${toolchain_bin}/x86_64-conda-linux-gnu-g++"
export AR="${toolchain_bin}/x86_64-conda-linux-gnu-ar"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
export LD_LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"
export CMAKE_PREFIX_PATH="${cpp_prefix}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"

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
    ((status == 0)) || die "${label} failed with ${status}; see ${log_file}"
}

printf 'RBT_PYTHON_COMPONENT_VERIFY mode=%s logs=%s\n' "${mode}" "${run_dir}"

run_logged rust-fmt cargo fmt --all -- --check
run_logged rust-workspace-test cargo test --workspace
run_logged rust-strict-clippy cargo clippy --workspace --all-targets --all-features -- -D warnings

if rg -n \
    'PyThreadState_SetAsyncExc|daemon[[:space:]]*=[[:space:]]*True|Python-side resolver|Python-side Composer' \
    crates/rbt-python/src/component_runtime.rs python/rbt/__init__.py \
    >"${run_dir}/forbidden-runtime-source.log"; then
    cat "${run_dir}/forbidden-runtime-source.log" >&2
    die "forbidden Python runtime mechanism found"
fi
printf 'RBT_PYTHON_COMPONENT_FORBIDDEN_SOURCE matches=0\n' \
    | tee "${run_dir}/forbidden-runtime-source.log"

if rg -n \
    'PyBytes|PyByteArray|to_vec\(|pickle|mpsc.*payload|serde_json.*payload|\.tobytes\(' \
    crates/rbt-python/src/component_runtime.rs python/rbt/__init__.py \
    examples/python/component_detector.py >"${run_dir}/payload-copy-source.log"; then
    cat "${run_dir}/payload-copy-source.log" >&2
    die "payload-copy primitive found in Component lifecycle path"
fi
printf 'RBT_PYTHON_COMPONENT_PAYLOAD_DTO_AUDIT matches=0\n' \
    | tee "${run_dir}/payload-copy-source.log"

if [[ "${skip_inherited}" == 1 ]]; then
    printf 'SKIP: inherited base Python gate is scheduled by the parent composition\n' \
        | tee "${run_dir}/inherited-python.log"
elif [[ "${mode}" == full ]]; then
    run_logged inherited-python ./scripts/verify_python.sh --full
else
    run_logged inherited-python ./scripts/verify_python.sh --quick
fi

verify_python="${verify_venv}/bin/python"
[[ -x "${verify_python}" ]] || die "Python verifier venv was not created"
host="$(rustc -vV | sed -n 's/^host: //p')"
rust_lld="$(rustc --print sysroot)/lib/rustlib/${host}/bin/rust-lld"
gcc_libdir="$(dirname "$(${CC} -print-file-name=libgcc.a)")"
wheel_dir="${run_dir}/wheel"
mkdir -p "${wheel_dir}"
unset CONDA_PREFIX CONDA_DEFAULT_ENV
export VIRTUAL_ENV="${verify_venv}"
export PYO3_PYTHON="${verify_python}"
run_logged component-wheel-build bash -c \
    'cd "$1" && "$2" -m maturin build --release --interpreter "$2" \
        --compatibility off --out "$3" -- "${@:4}"' _ \
    "${repo_root}/python" "${verify_python}" "${wheel_dir}" \
    -C "linker=${rust_lld}" -C linker-flavor=ld.lld \
    -C "link-arg=-L${toolchain_root}/lib" -C "link-arg=-L${gcc_libdir}" \
    -C link-arg=-L/usr/lib/x86_64-linux-gnu -C link-arg=-L/lib/x86_64-linux-gnu
wheel_path="$(find "${wheel_dir}" -maxdepth 1 -type f -name 'rbt-*.whl' -print -quit)"
[[ -n "${wheel_path}" ]] || die "Component wheel build produced no wheel"

wheel_extract="${tmp_root}/wheel-extract"
mkdir -p "${wheel_extract}"
run_logged component-wheel-extract unzip -q "${wheel_path}" -d "${wheel_extract}"
native_extension="$(find "${wheel_extract}" -type f -name '*.so' -print -quit)"
[[ -n "${native_extension}" ]] || die "wheel contains no extension"
readelf -d "${native_extension}" >"${run_dir}/wheel-dynamic.log"
! rg -n 'RPATH|RUNPATH' "${run_dir}/wheel-dynamic.log" \
    || die "wheel extension has an RPATH/RUNPATH"
! strings "${native_extension}" | rg -F "${repo_root}" \
    >"${run_dir}/wheel-repository-paths.log" \
    || die "wheel extension embeds the repository path"
env -u LD_LIBRARY_PATH -u LIBRARY_PATH ldd "${native_extension}" \
    >"${run_dir}/wheel-dependencies.log"
! rg -n 'not found' "${run_dir}/wheel-dependencies.log" \
    || die "wheel extension has unresolved dependencies"

fresh_venv="${tmp_root}/fresh-venv"
run_logged fresh-venv "${bootstrap_python}" -m venv --system-site-packages "${fresh_venv}"
run_logged fresh-wheel-preinstall env -u PYTHONPATH -u LD_LIBRARY_PATH -u LIBRARY_PATH \
    "${fresh_venv}/bin/python" -c \
    'import importlib.util, numpy; assert importlib.util.find_spec("rbt") is None; print("NumPy", numpy.__version__)'
run_logged fresh-wheel-install env -u PYTHONPATH -u LD_LIBRARY_PATH -u LIBRARY_PATH \
    "${fresh_venv}/bin/python" -m pip install --disable-pip-version-check --no-deps "${wheel_path}"
run_logged fresh-wheel-pip-check env -u PYTHONPATH -u LD_LIBRARY_PATH -u LIBRARY_PATH \
    "${fresh_venv}/bin/python" -m pip check
run_logged fresh-wheel-import env -u PYTHONPATH -u LD_LIBRARY_PATH -u LIBRARY_PATH \
    "${fresh_venv}/bin/python" -c \
    'import rbt; assert rbt.__version__ == "0.8.0"; assert hasattr(rbt, "Component")'

outside="${tmp_root}/outside-checkout"
mkdir -p "${outside}"
cp "${repo_root}/examples/python/component_detector.py" "${outside}/component_detector.py"
cp "${repo_root}/python/tests/component_runtime_probe.py" "${outside}/component_runtime_probe.py"

harness_args=(--quick)
[[ "${mode}" == full ]] && harness_args=(--full)
# Workspace tests build test harnesses, not the executable fixtures below.
# Rebuild them so an old native baseline cannot be mixed with the fresh wheel.
run_logged fresh-wheel-component env -u PYTHONPATH \
    RBT_SKIP_BUILD=0 \
    RBT_PYTHON_COMPONENT_PYTHON="${fresh_venv}/bin/python" \
    RBT_PYTHON_COMPONENT_EXAMPLE="${outside}/component_detector.py" \
    RBT_PYTHON_COMPONENT_PROBE="${outside}/component_runtime_probe.py" \
    RBT_PYTHON_COMPONENT_LOG_DIR="${run_dir}/fresh-wheel-runtime" \
    RBT_PYTHON_COMPONENT_SOAK_SECONDS="${RBT_PYTHON_COMPONENT_SOAK_SECONDS:-300}" \
    ./scripts/test_python_component_runtime.sh "${harness_args[@]}"

unsafe_log="${run_dir}/unsafe-audit.log"
python3 "${repo_root}/scripts/audit_unsafe.py" --label RBT_PYTHON_COMPONENT_UNSAFE 2>&1 \
    | tee "${unsafe_log}" \
    || die "production unsafe audit failed; see ${unsafe_log}"

printf 'PASS: Python Component %s verification; wheel=%s python=%s logs=%s\n' \
    "${mode}" "$(basename "${wheel_path}")" \
    "$("${fresh_venv}/bin/python" -c 'import platform; print(platform.python_version())')" \
    "${run_dir}"

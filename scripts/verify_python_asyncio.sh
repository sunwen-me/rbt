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
usage: scripts/verify_python_asyncio.sh [--quick|--full]

--quick  Fresh CPython 3.12 wheel, asyncio unit tests, source audits, the
         inherited synchronous Component gate, and focused asyncio runtime
         cancellation/timeout/sibling checks.
--full   Quick plus the inherited full/ASan gates, 100-cycle/soak runtime,
         idle CPU, readiness latency, and reverse sibling cancellation.

Environment overrides:
  RBT_PYTHON_ASYNCIO_BOOTSTRAP  fresh CPython 3.12 used to build the wheel
  RBT_PYTHON                   compatibility alias for the bootstrap Python
  RBT_PHASE4_TOOLCHAIN         compiler/toolchain root
  RBT_IOX2_PREFIX              iceoryx2 C++ install prefix
  RBT_PYTHON_ASYNCIO_LOG_DIR   retained verifier logs
  RBT_PYTHON_ASYNCIO_TOOL_PYTHON
      existing CPython 3.12 environment with maturin/pytest for an offline
      composed run; the runtime wheel is still installed in a fresh venv
  RBT_PYTHON_ASYNCIO_SOAK_SECONDS (default 300 in --full)
  RBT_PYTHON_ASYNCIO_IDLE_SECONDS (default 10 in --full)
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

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
toolchain_bin="${toolchain_root}/bin"
cpp_prefix="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"
"${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${cpp_prefix}"
cpp_build="${RBT_PYTHON_ASYNCIO_CPP_BUILD:-${repo_root}/target/python-cpp-build}"
default_python="/home/sw/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3"
if [[ ! -x "${default_python}" ]]; then
    default_python="$(command -v python3 || true)"
fi
bootstrap_python="${RBT_PYTHON_ASYNCIO_BOOTSTRAP:-${RBT_PYTHON:-${default_python}}}"
tool_python="${RBT_PYTHON_ASYNCIO_TOOL_PYTHON:-}"
log_root="${RBT_PYTHON_ASYNCIO_LOG_DIR:-${repo_root}/target/python-asyncio-verify-logs}"
soak_seconds="${RBT_PYTHON_ASYNCIO_SOAK_SECONDS:-300}"
idle_seconds="${RBT_PYTHON_ASYNCIO_IDLE_SECONDS:-10}"
inherited_venv="${RBT_PYTHON_ASYNCIO_INHERITED_VENV:-${repo_root}/target/python-asyncio-inherited-venv}"
mkdir -p "${log_root}"
run_id="python-asyncio-${mode}-$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="${log_root}/${run_id}"
mkdir -p "${run_dir}"
tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/rbt-python-asyncio-verify.XXXXXX")"
trap 'rm -rf -- "${tmp_root}"' EXIT

[[ "${soak_seconds}" =~ ^[1-9][0-9]*$ ]] || die "soak duration must be positive"
[[ "${idle_seconds}" =~ ^[1-9][0-9]*$ ]] || die "idle duration must be positive"
for command_name in cargo rustc cmake ctest rg awk diff find sed sort wc unzip readelf strings ldd jq timeout getconf; do
    command -v "${command_name}" >/dev/null 2>&1 \
        || die "required command not found: ${command_name}"
done
[[ -x "${bootstrap_python}" ]] || die "CPython interpreter not found: ${bootstrap_python}"
if [[ -n "${tool_python}" ]]; then
    [[ -x "${tool_python}" ]] || die "asyncio tool Python is missing: ${tool_python}"
fi
[[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-gcc" ]] \
    || die "Phase4 C compiler is missing"
[[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-g++" ]] \
    || die "Phase4 C++ compiler is missing"
[[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-ar" ]] \
    || die "Phase4 archiver is missing"
[[ -f "${cpp_prefix}/lib/cmake/iceoryx2-cxx/iceoryx2-cxxConfig.cmake" ]] \
    || die "iceoryx2 C++ package config is missing under ${cpp_prefix}"

python_version="$("${bootstrap_python}" -c \
    'import platform, struct, sys; assert platform.python_implementation() == "CPython"; assert sys.version_info[:2] == (3, 12), sys.version; assert struct.calcsize("P") == 8 and sys.byteorder == "little"; print(platform.python_version())')"
printf 'RBT_PYTHON_ASYNCIO_BOOTSTRAP python=%s path=%s\n' \
    "${python_version}" "${bootstrap_python}"

rust_host="$(rustc -vV | sed -n 's/^host: //p')"
rust_lld="$(rustc --print sysroot)/lib/rustlib/${rust_host}/bin/rust-lld"
[[ -x "${rust_lld}" ]] || die "rust-lld is missing: ${rust_lld}"
export PATH="${toolchain_bin}${PATH:+:${PATH}}"
export CC="${toolchain_bin}/x86_64-conda-linux-gnu-gcc"
export CXX="${toolchain_bin}/x86_64-conda-linux-gnu-g++"
export AR="${toolchain_bin}/x86_64-conda-linux-gnu-ar"
phase4_gcc_lib="$("${CC}" -print-file-name=libgcc.a)"
[[ -f "${phase4_gcc_lib}" ]] || die "Phase4 libgcc.a is missing: ${phase4_gcc_lib}"
phase4_gcc_libdir="${phase4_gcc_lib%/*}"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
export LD_LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"
export CMAKE_PREFIX_PATH="${cpp_prefix}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"
export PYTHONASYNCIODEBUG="${PYTHONASYNCIODEBUG:-1}"

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

printf 'RBT_PYTHON_ASYNCIO_VERIFY mode=%s logs=%s\n' "${mode}" "${run_dir}"

async_runtime_source="${repo_root}/crates/rbt-python/src/async_runtime.rs"
component_runtime_source="${repo_root}/crates/rbt-python/src/component_runtime.rs"
python_source="${repo_root}/python/rbt/__init__.py"
async_detector_source="${repo_root}/examples/python/async_component_detector.py"
async_probe_source="${repo_root}/python/tests/async_component_runtime_probe.py"
for source_file in \
    "${async_runtime_source}" "${component_runtime_source}" "${python_source}" \
    "${async_detector_source}" "${async_probe_source}"; do
    [[ -f "${source_file}" ]] || die "required asyncio source is missing: ${source_file}"
done

run_logged async-python-compile "${bootstrap_python}" -c \
    'import pathlib, sys; [compile(path.read_text(encoding="utf-8"), str(path), "exec") for path in map(pathlib.Path, sys.argv[1:])]' \
    "${async_detector_source}" "${async_probe_source}" "${python_source}"

# Restrict the Python audit to receive_async().  The rest of __init__.py
# legitimately serializes descriptors and accepts bytes-like metadata.
async_receive_source="${tmp_root}/receive-async-source.py"
awk '
    /^[[:space:]]*async def _receive_async[[:space:]]*\(/ { capture = 1 }
    capture { print }
    capture && /^[[:space:]]*class ImageSubscriber[[:space:]]*\(/ { exit }
' "${python_source}" >"${async_receive_source}"
[[ -s "${async_receive_source}" ]] || die "could not isolate receive_async source"

payload_pattern='asyncio[.]Queue[[:space:]]*[(]|queue[.]put[[:space:]]*[(]|bytes[[:space:]]*[(][[:space:]]*(payload|sample)|bytearray[[:space:]]*[(][[:space:]]*(payload|sample)|pickle|json[[:space:]_-]*payload|mpsc[^[:cntrl:]]*(sample|payload)|to_vec[[:space:]]*[(]|Vec[[:space:]]*<[[:space:]]*[^>]*(sample|payload)|np[.](array|copy)[[:space:]]*[(]|[.]tobytes[[:space:]]*[(]|Py(Bytes|ByteArray)|memcpy|std::copy|std::vector'
payload_audit_log="${run_dir}/async-payload-audit.log"
if rg -n -i "${payload_pattern}" \
    "${async_runtime_source}" "${async_receive_source}" "${async_detector_source}" \
    >"${payload_audit_log}"; then
    cat "${payload_audit_log}" >&2
    die "asyncio path contains a payload queue/copy primitive"
fi
printf 'RBT_PYTHON_ASYNCIO_PAYLOAD_AUDIT forbidden_matches=0\n' \
    | tee "${payload_audit_log}"

thread_audit_log="${run_dir}/async-thread-audit.log"
if rg -n -i \
    'asyncio[.]to_thread|run_in_executor|ThreadPoolExecutor|threading[.]Thread|spawn_blocking|std::thread::spawn[[:space:]]*[(]|thread::spawn[[:space:]]*[(]' \
    "${async_receive_source}" "${async_detector_source}" "${async_probe_source}" \
    >"${thread_audit_log}"; then
    cat "${thread_audit_log}" >&2
    die "asyncio path creates a thread per await"
fi
printf 'RBT_PYTHON_ASYNCIO_PER_AWAIT_THREAD_AUDIT forbidden_matches=0 managed_native_threads=2\n' \
    | tee "${thread_audit_log}"

busy_audit_log="${run_dir}/async-busy-poll-audit.log"
if rg -n -i \
    'asyncio[.]sleep|time[.]sleep|thread::sleep|std::thread::sleep|recv_timeout|try_iter|wait_receive[[:space:]]*[(]|busy[ _-]*poll' \
    "${async_runtime_source}" "${async_receive_source}" \
    >"${busy_audit_log}"; then
    cat "${busy_audit_log}" >&2
    die "async receive path contains sleeping or busy polling"
fi
printf 'RBT_PYTHON_ASYNCIO_BUSY_POLL_AUDIT forbidden_matches=0\n' \
    | tee "${busy_audit_log}"

forceful_audit_log="${run_dir}/async-forceful-interruption-audit.log"
if rg -n -i \
    'PyThreadState_SetAsyncExc|pthread_(cancel|kill)|PyErr_SetInterrupt|Py_Finalize|daemon[[:space:]]*=[[:space:]]*True|Thread[.]kill|thread[.]kill|TerminateThread' \
    "${async_runtime_source}" "${component_runtime_source}" "${async_receive_source}" \
    "${async_detector_source}" "${async_probe_source}" >"${forceful_audit_log}"; then
    cat "${forceful_audit_log}" >&2
    die "asyncio path contains forceful interruption"
fi
printf 'RBT_PYTHON_ASYNCIO_FORCEFUL_INTERRUPTION_AUDIT forbidden_matches=0\n' \
    | tee "${forceful_audit_log}"

if [[ "${RBT_PYTHON_ASYNCIO_SKIP_INHERITED:-0}" == 1 ]]; then
    printf 'SKIP: inherited Python Component gate explicitly disabled\n'
else
    inherited_args=(--quick)
    [[ "${mode}" == full ]] && inherited_args=(--full)
    run_logged inherited-python-component env \
        RBT_PYTHON="${bootstrap_python}" \
        RBT_PYTHON_VERIFY_VENV="${inherited_venv}" \
        RBT_PYTHON_COMPONENT_VERIFY_LOG_DIR="${run_dir}/inherited-component" \
        RBT_PYTHON_COMPONENT_SOAK_SECONDS="${soak_seconds}" \
        ./scripts/verify_python_component.sh "${inherited_args[@]}"
fi

# Build and install a wheel in an otherwise unused environment.  The
# bootstrap interpreter is required to be CPython 3.12 above, so this cannot
# silently exercise a cached extension built for another Python ABI.
build_venv="${tmp_root}/wheel-build-venv"
if [[ -n "${tool_python}" ]]; then
    build_python="${tool_python}"
    run_logged fresh-wheel-build-tools "${build_python}" -c \
        'import importlib.metadata, maturin, numpy, pytest; print("maturin", importlib.metadata.version("maturin"), "NumPy", numpy.__version__, "pytest", pytest.__version__)'
else
    build_python="${build_venv}/bin/python"
    run_logged fresh-wheel-build-venv "${bootstrap_python}" -m venv "${build_venv}"
    run_logged fresh-wheel-build-tools "${build_python}" -m pip install \
        --disable-pip-version-check 'maturin>=1.7,<2' 'numpy>=1.23' 'pytest>=8,<10' \
        'pytest-timeout>=2,<3'
fi
wheel_dir="${run_dir}/wheel"
mkdir -p "${wheel_dir}"
unset CONDA_PREFIX CONDA_DEFAULT_ENV
export VIRTUAL_ENV="$(cd "$(dirname "${build_python}")/.." && pwd)"
export PYO3_PYTHON="${build_python}"
rust_link_args=(
    -C "linker=${rust_lld}"
    -C linker-flavor=ld.lld
    -C "link-arg=-L${toolchain_root}/lib"
    -C "link-arg=-L${phase4_gcc_libdir}"
    -C link-arg=-L/usr/lib/x86_64-linux-gnu
    -C link-arg=-L/lib/x86_64-linux-gnu
)
run_logged fresh-wheel-build bash -c \
    'cd "$1" && "$2" -m maturin build --release --interpreter "$2" \
        --compatibility off --out "$3" -- "${@:4}"' _ \
    "${repo_root}/python" "${build_python}" "${wheel_dir}" "${rust_link_args[@]}"
wheel_path="$(find "${wheel_dir}" -maxdepth 1 -type f -name 'rbt-*.whl' -print -quit)"
[[ -n "${wheel_path}" ]] || die "fresh CPython 3.12 wheel build produced no wheel"

wheel_extract="${tmp_root}/wheel-extract"
mkdir -p "${wheel_extract}"
run_logged fresh-wheel-extract unzip -q "${wheel_path}" -d "${wheel_extract}"
native_extension="$(find "${wheel_extract}" -type f -name '*.so' -print -quit)"
[[ -n "${native_extension}" ]] || die "fresh wheel contains no native extension"
readelf -d "${native_extension}" >"${run_dir}/wheel-dynamic.log"
! rg -n 'RPATH|RUNPATH' "${run_dir}/wheel-dynamic.log" \
    || die "fresh wheel extension has an RPATH/RUNPATH"
! strings "${native_extension}" | rg -F "${repo_root}" \
    >"${run_dir}/wheel-repository-paths.log" \
    || die "fresh wheel extension embeds the repository path"
env -u LD_LIBRARY_PATH -u LIBRARY_PATH ldd "${native_extension}" \
    >"${run_dir}/wheel-dependencies.log"
! rg -n 'not found' "${run_dir}/wheel-dependencies.log" \
    || die "fresh wheel extension has unresolved dependencies"

wheel_venv="${tmp_root}/wheel-runtime-venv"
wheel_python="${wheel_venv}/bin/python"
run_logged fresh-wheel-runtime-venv "${bootstrap_python}" -m venv \
    --system-site-packages "${wheel_venv}"
run_logged fresh-wheel-preinstall env -u PYTHONPATH -u LD_LIBRARY_PATH -u LIBRARY_PATH \
    "${wheel_python}" -c \
    'import importlib.util, numpy; assert importlib.util.find_spec("rbt") is None; print("NumPy", numpy.__version__)'
run_logged fresh-wheel-install env -u PYTHONPATH -u LD_LIBRARY_PATH -u LIBRARY_PATH \
    "${wheel_python}" -m pip install --disable-pip-version-check --no-deps \
    "${wheel_path}"
run_logged fresh-wheel-pip-check env -u PYTHONPATH -u LD_LIBRARY_PATH -u LIBRARY_PATH \
    "${wheel_python}" -m pip check
run_logged fresh-wheel-import env -u PYTHONPATH -u LD_LIBRARY_PATH -u LIBRARY_PATH \
    "${wheel_python}" -c \
    'import asyncio, platform, rbt, struct, sys; assert platform.python_implementation() == "CPython"; assert sys.version_info[:2] == (3, 12); assert struct.calcsize("P") == 8 and sys.byteorder == "little"; assert hasattr(rbt, "AsyncTaskHandle"); print("asyncio", asyncio.__name__, "rbt", rbt.__version__)'
wheel_site="$(${wheel_python} -c 'import sysconfig; print(sysconfig.get_paths()["purelib"])')"
run_logged async-unit-tests env -u LD_LIBRARY_PATH -u LIBRARY_PATH \
    PYTHONPATH="${wheel_site}" \
    PYTHONASYNCIODEBUG="${PYTHONASYNCIODEBUG}" \
    "${build_python}" -m pytest -q --import-mode=importlib \
    "${repo_root}/python/tests/test_component_api.py"

outside="${tmp_root}/outside-checkout"
mkdir -p "${outside}"
cp "${async_detector_source}" "${outside}/async_component_detector.py"
cp "${async_probe_source}" "${outside}/async_component_runtime_probe.py"
runtime_args=(--quick)
[[ "${mode}" == full ]] && runtime_args=(--full)
run_logged async-runtime env -u PYTHONPATH -u LD_LIBRARY_PATH -u LIBRARY_PATH \
    RBT_PYTHON_ASYNCIO_PYTHON="${wheel_python}" \
    RBT_PYTHON_ASYNCIO_EXAMPLE="${outside}/async_component_detector.py" \
    RBT_PYTHON_ASYNCIO_PROBE="${outside}/async_component_runtime_probe.py" \
    RBT_PYTHON_ASYNCIO_CPP_BUILD="${cpp_build}" \
    RBT_PYTHON_ASYNCIO_LOG_DIR="${run_dir}/async-runtime" \
    RBT_PYTHON_ASYNCIO_PROCESS_CWD="${outside}" \
    RBT_PYTHON_ASYNCIO_SOAK_SECONDS="${soak_seconds}" \
    RBT_PYTHON_ASYNCIO_IDLE_SECONDS="${idle_seconds}" \
    PYTHONASYNCIODEBUG="${PYTHONASYNCIODEBUG}" \
    ./scripts/test_python_asyncio_runtime.sh "${runtime_args[@]}"

printf 'PASS: Python asyncio %s verification; wheel=%s python=%s logs=%s\n' \
    "${mode}" "$(basename "${wheel_path}")" "${python_version}" "${run_dir}"

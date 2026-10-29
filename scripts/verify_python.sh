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
usage: scripts/verify_python.sh [--quick|--full]

--quick  Build the extension/C++ fixture and run focused Python zero-copy gates.
--full   Quick plus pressure, GC/resource, signal, wheel/fresh-venv, and all
         existing v0.1/pressure/WaitSet regression gates.

Environment overrides:
  RBT_PYTHON                 CPython 3.10+ interpreter used to create test venvs
  RBT_PYTHON_VERIFY_VENV     reusable verifier virtualenv
  RBT_PHASE4_TOOLCHAIN       compiler/toolchain root
  RBT_IOX2_PREFIX            iceoryx2 C++ install prefix
  RBT_PYTHON_LOG_DIR         retained verifier evidence root
  RBT_PYTHON_SKIP_SANITIZER=1
                             omit the isolated native-extension ASan gate only
                             when the compiler/runtime cannot support it
  RBT_PYTHON_SKIP_REGRESSION=1
                             omit the existing full regression only when explicit
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
cpp_build="${repo_root}/target/python-cpp-build"
log_root="${RBT_PYTHON_LOG_DIR:-${repo_root}/target/python-verify-logs}"
default_python="/home/sw/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3"
if [[ ! -x "${default_python}" ]]; then
    default_python="$(command -v python3 || true)"
fi
bootstrap_python="${RBT_PYTHON:-${default_python}}"
verify_venv="${RBT_PYTHON_VERIFY_VENV:-${repo_root}/target/python-verify-venv}"

for command_name in cargo rustc cmake ctest diff rg date unzip readelf strings ldd timeout; do
    command -v "${command_name}" >/dev/null 2>&1 || die "required command not found: ${command_name}"
done
[[ -x "${bootstrap_python}" ]] || die "CPython interpreter not found: ${bootstrap_python}"
[[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-gcc" ]] || die "Phase4 C compiler is missing"
[[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-g++" ]] || die "Phase4 C++ compiler is missing"
[[ -x "${toolchain_bin}/x86_64-conda-linux-gnu-ar" ]] || die "Phase4 archiver is missing"
[[ -f "${cpp_prefix}/lib/cmake/iceoryx2-cxx/iceoryx2-cxxConfig.cmake" ]] \
    || die "iceoryx2 C++ package config is missing under ${cpp_prefix}"

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

mkdir -p "${log_root}"
run_id="python-${mode}-$(date -u +%Y%m%dT%H%M%SZ)-$$"
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

if [[ ! -x "${verify_venv}/bin/python" ]]; then
    run_logged python-create-venv "${bootstrap_python}" -m venv "${verify_venv}"
fi
verify_python="${verify_venv}/bin/python"
run_logged python-tools "${verify_python}" -m pip install --disable-pip-version-check \
    'maturin>=1.7,<2' 'numpy>=1.23' 'pytest>=8,<10' 'pytest-timeout>=2,<3'
run_logged python-environment "${verify_python}" -c \
    'import platform, struct, sys, numpy; assert platform.python_implementation() == "CPython"; assert sys.version_info >= (3, 10); assert struct.calcsize("P") == 8 and sys.byteorder == "little"; print(sys.version); print("NumPy", numpy.__version__, platform.machine())'

run_logged rust-fmt cargo fmt --all -- --check
run_logged rust-check cargo check --workspace --all-targets
run_logged rust-tests cargo test -p rbt-python -p rbt-idl -p rbt-types -p rbt-iox2
run_logged rust-clippy cargo clippy --workspace --all-targets --all-features -- -D warnings
run_logged rust-fixture-image cargo build --release \
    -p rbt-python --bin image_fixture --bin readiness_deadline_fixture
run_logged rust-fixture-interop cargo build --release -p phase4-interop
run_logged rust-fixture-pressure cargo build --release -p rbt-iox2 --bin mixed_pressure

run_logged cpp-configure cmake \
    -S "${repo_root}/sdk/cpp" \
    -B "${cpp_build}" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER="${CXX}" \
    -DCMAKE_PREFIX_PATH="${cpp_prefix}"
run_logged cpp-build cmake --build "${cpp_build}" --parallel 2
run_logged cpp-ctest ctest --test-dir "${cpp_build}" --output-on-failure

for required_binary in \
    "${repo_root}/target/release/image_fixture" \
    "${repo_root}/target/release/readiness_deadline_fixture" \
    "${repo_root}/target/release/phase4-interop" \
    "${repo_root}/target/release/mixed_pressure" \
    "${cpp_build}/interop_cpp"; do
    [[ -x "${required_binary}" ]] || die "required current verifier binary is missing: ${required_binary}"
done

generation_one="${run_dir}/python-generation-one"
generation_two="${run_dir}/python-generation-two"
mkdir -p \
    "${generation_one}/rust" "${generation_one}/cpp" \
    "${generation_one}/descriptors" "${generation_one}/python" \
    "${generation_two}/rust" "${generation_two}/cpp" \
    "${generation_two}/descriptors" "${generation_two}/python"
run_logged idl-python-generate-one cargo run --quiet -p rbt-idl -- generate \
    types/sensor.rbt types/control.rbt types/demo.rbt types/tf.rbt \
    --rust "${generation_one}/rust" --cpp "${generation_one}/cpp" \
    --descriptor "${generation_one}/descriptors" --python "${generation_one}/python"
run_logged idl-python-generate-two cargo run --quiet -p rbt-idl -- generate \
    types/sensor.rbt types/control.rbt types/demo.rbt types/tf.rbt \
    --rust "${generation_two}/rust" --cpp "${generation_two}/cpp" \
    --descriptor "${generation_two}/descriptors" --python "${generation_two}/python"
run_logged idl-python-deterministic diff -ru -- "${generation_one}" "${generation_two}"
for generated_file in sensor.py control.py demo.py tf.py; do
    run_logged "idl-python-checked-in-${generated_file%.py}" diff -u -- \
        "${generation_one}/python/${generated_file}" \
        "${repo_root}/python/rbt/_generated/${generated_file}"
done

if rg -n \
    'np\.array\(|np\.copy\(|\.tobytes\(|PyBytes|PyByteArray|to_vec\(|memcpy|std::copy|std::vector' \
    crates/rbt-python/src/lib.rs python/rbt >"${run_dir}/zero-copy-source-audit.log"; then
    cat "${run_dir}/zero-copy-source-audit.log" >&2
    die "forbidden payload-copy primitive found in production Python data paths"
fi
printf 'RBT_PYTHON_ZERO_COPY_SOURCE_AUDIT forbidden_matches=0\n' \
    | tee "${run_dir}/zero-copy-source-audit.log"

unset CONDA_PREFIX CONDA_DEFAULT_ENV
export VIRTUAL_ENV="${verify_venv}"
export PATH="${verify_venv}/bin:${PATH}"
export PYO3_PYTHON="${verify_python}"
rust_link_args=(
    -C "linker=${rust_lld}"
    -C linker-flavor=ld.lld
    -C "link-arg=-L${toolchain_root}/lib"
    -C "link-arg=-L${phase4_gcc_libdir}"
    -C link-arg=-L/usr/lib/x86_64-linux-gnu
    -C link-arg=-L/lib/x86_64-linux-gnu
)
run_logged python-extension-build bash -c \
    'cd "$1" && "$2" -m maturin develop --release -- "${@:3}"' _ \
    "${repo_root}/python" "${verify_python}" "${rust_link_args[@]}"
export RBT_CPP_INTEROP="${cpp_build}/interop_cpp"
export RBT_PHASE4_INTEROP="${repo_root}/target/release/phase4-interop"
export RBT_PYTHON_IMAGE_FIXTURE="${repo_root}/target/release/image_fixture"
export RBT_READINESS_DEADLINE_FIXTURE="${repo_root}/target/release/readiness_deadline_fixture"

publisher_test_paths=()
for publisher_test in \
    "${repo_root}/python/tests/test_publishers.py" \
    "${repo_root}/python/tests/test_publisher_ownership.py" \
    "${repo_root}/python/tests/test_publisher_interop.py"; do
    if [[ -f "${publisher_test}" ]]; then
        publisher_test_paths+=("${publisher_test}")
    fi
done
if ((${#publisher_test_paths[@]} > 0)); then
    run_logged python-pytest-publisher-collection "${verify_python}" -m pytest -q \
        --collect-only -m 'not full' "${publisher_test_paths[@]}"
else
    printf 'SKIP: no dedicated Python publisher test module is present\n'
fi
run_logged python-pytest-quick "${verify_python}" -m pytest -q \
    -m 'not full' "${repo_root}/python/tests"

unsafe_matches="${run_dir}/python-unsafe-locations.log"
rg -n '\bunsafe\b' crates/rbt-python/src/lib.rs >"${unsafe_matches}"
unsafe_count="$(wc -l <"${unsafe_matches}")"
unsafe_count="${unsafe_count//[[:space:]]/}"
printf 'RBT_PYTHON_UNSAFE_AUDIT legacy_non_python=31 python_old=12 python_new=%s legacy_total=49 tf_generated=24 total=73 delta_python=%s\n' \
    "${unsafe_count}" "$((unsafe_count - 12))" \
    | tee "${run_dir}/unsafe-audit.log"
cat "${unsafe_matches}" | tee -a "${run_dir}/unsafe-audit.log"
[[ "${unsafe_count}" == 18 ]] \
    || die "Python binding unsafe source-location count is ${unsafe_count}, expected 18"

if [[ "${mode}" == full ]]; then
    run_logged python-pytest-full "${verify_python}" -m pytest -q -s \
        "${repo_root}/python/tests"

    if [[ "${RBT_PYTHON_SKIP_SANITIZER:-0}" == 1 ]]; then
        printf 'SKIP: Python native-extension ASan explicitly disabled by RBT_PYTHON_SKIP_SANITIZER=1\n'
    else
        asan_runtime="$("${CC}" -print-file-name=libasan.so)"
        [[ -f "${asan_runtime}" ]] || die "AddressSanitizer runtime is missing: ${asan_runtime}"
        asan_wheel_dir="${run_dir}/asan-wheel"
        asan_target_dir="${repo_root}/target/python-asan-target"
        mkdir -p "${asan_wheel_dir}"
        run_logged python-asan-wheel-build env RUSTC_BOOTSTRAP=1 bash -c \
            'cd "$1" && "$2" -m maturin build --release --target-dir "$3" \
                --interpreter "$2" --compatibility off --out "$4" -- \
                -Z sanitizer=address "${@:5}"' _ \
            "${repo_root}/python" "${verify_python}" "${asan_target_dir}" \
            "${asan_wheel_dir}" "${rust_link_args[@]}"
        asan_wheel_path="$(find "${asan_wheel_dir}" -maxdepth 1 -type f -name 'rbt-*.whl' -print -quit)"
        [[ -n "${asan_wheel_path}" ]] || die "ASan wheel build produced no rbt wheel"
        asan_extract_dir="$(mktemp -d "${TMPDIR:-/tmp}/rbt-python-asan.XXXXXX")"
        run_logged python-asan-wheel-extract unzip -q "${asan_wheel_path}" -d "${asan_extract_dir}"
        asan_publisher_tests=()
        for publisher_test in \
            "${repo_root}/python/tests/test_publishers.py" \
            "${repo_root}/python/tests/test_publisher_ownership.py" \
            "${repo_root}/python/tests/test_time_stream.py"; do
            if [[ -f "${publisher_test}" ]]; then
                asan_publisher_tests+=("${publisher_test}")
            fi
        done
        ((${#asan_publisher_tests[@]} > 0)) \
            || die "no Python publisher ownership test module is present for ASan"
        run_logged python-asan-ownership env \
            ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:abort_on_error=1 \
            LD_PRELOAD="${asan_runtime}" \
            PYTHONPATH="${asan_extract_dir}:${repo_root}/python/tests" \
            "${verify_python}" -m pytest -q --rootdir=/tmp --import-mode=importlib \
            "${repo_root}/python/tests/test_image.py::test_derived_view_retains_native_borrow_then_release_recovers" \
            "${repo_root}/python/tests/test_image.py::test_memoryview_retains_native_borrow" \
            "${repo_root}/python/tests/test_image.py::test_multiple_arrays_each_retain_the_native_sample_owner" \
            "${repo_root}/python/tests/test_image.py::test_last_array_reference_can_be_dropped_on_another_python_thread" \
            "${repo_root}/python/tests/test_pointcloud.py::test_cpp_pointcloud_zero_copy_structured_dtype" \
            "${asan_publisher_tests[@]}" \
            "${repo_root}/python/tests/test_validation.py"
    fi

    wheel_dir="${run_dir}/wheel"
    mkdir -p "${wheel_dir}"
    run_logged python-wheel-build bash -c \
        'cd "$1" && "$2" -m maturin build --release --interpreter "$2" \
            --compatibility off --out "$3" -- "${@:4}"' _ \
        "${repo_root}/python" "${verify_python}" "${wheel_dir}" "${rust_link_args[@]}"
    wheel_path="$(find "${wheel_dir}" -maxdepth 1 -type f -name 'rbt-*.whl' -print -quit)"
    [[ -n "${wheel_path}" ]] || die "wheel build produced no rbt wheel"

    wheel_audit_dir="$(mktemp -d "${TMPDIR:-/tmp}/rbt-python-wheel-audit.XXXXXX")"
    run_logged wheel-audit-extract unzip -q "${wheel_path}" -d "${wheel_audit_dir}"
    if unzip -Z1 "${wheel_path}" | rg -n '(^|/)(__pycache__/|.*\.pyc$)' \
        >"${run_dir}/wheel-bytecode.log"; then
        cat "${run_dir}/wheel-bytecode.log" >&2
        die "wheel contains Python bytecode or __pycache__ entries"
    fi
    native_extension="$(find "${wheel_audit_dir}" -type f -name '*.so' -print -quit)"
    [[ -n "${native_extension}" ]] || die "wheel contains no native extension"
    readelf -d "${native_extension}" >"${run_dir}/wheel-dynamic.log"
    if rg -n 'RPATH|RUNPATH' "${run_dir}/wheel-dynamic.log" \
        >"${run_dir}/wheel-rpath.log"; then
        cat "${run_dir}/wheel-rpath.log" >&2
        die "wheel native extension contains an unexpected RPATH/RUNPATH"
    fi
    if strings "${native_extension}" | rg -n -F "${repo_root}" \
        >"${run_dir}/wheel-repo-strings.log"; then
        cat "${run_dir}/wheel-repo-strings.log" >&2
        die "wheel native extension contains an absolute repository path"
    fi
    run_logged wheel-native-dependencies env -u LD_LIBRARY_PATH -u LIBRARY_PATH \
        ldd "${native_extension}"
    if rg -n 'not found' "${run_dir}/wheel-native-dependencies.log" \
        >"${run_dir}/wheel-unresolved-dependencies.log" \
        || rg -n -F "${repo_root}" "${run_dir}/wheel-native-dependencies.log" \
            >"${run_dir}/wheel-unresolved-dependencies.log"; then
        cat "${run_dir}/wheel-unresolved-dependencies.log" >&2
        die "wheel has an unresolved or repository-local native dependency"
    fi
    printf 'PASS: wheel audit extension=%s bytecode=0 repo_paths=0\n' \
        "${native_extension}" | tee "${run_dir}/wheel-audit.log"

    # The wheel smoke must exercise only the installed artifact.  In
    # particular, do not inherit a caller's source-tree PYTHONPATH into any
    # of the fresh-venv checks below.
    unset PYTHONPATH
    smoke_root="$(mktemp -d "${TMPDIR:-/tmp}/rbt-python-smoke.XXXXXX")"
    smoke_venv="${smoke_root}/venv"
    # The bundled bootstrap runtime already provides NumPy. Inherit only its
    # site packages so the fresh-venv wheel smoke is reproducible without a
    # package-index download; rbt itself must still come solely from wheel_path.
    run_logged wheel-smoke-create-venv "${bootstrap_python}" -m venv \
        --system-site-packages "${smoke_venv}"
    run_logged wheel-smoke-preinstall env -u LD_LIBRARY_PATH -u LIBRARY_PATH \
        "${smoke_venv}/bin/python" -c \
        'import importlib.util, numpy; assert importlib.util.find_spec("rbt") is None; print("NumPy", numpy.__version__)'
    run_logged wheel-smoke-install env -u LD_LIBRARY_PATH -u LIBRARY_PATH \
        "${smoke_venv}/bin/python" -m pip install \
        --disable-pip-version-check --no-deps "${wheel_path}"
    run_logged wheel-smoke-pip-check env -u LD_LIBRARY_PATH -u LIBRARY_PATH \
        "${smoke_venv}/bin/python" -m pip check
    run_logged wheel-smoke-import env -u LD_LIBRARY_PATH -u LIBRARY_PATH \
        "${smoke_venv}/bin/python" -c 'import rbt; print(rbt.__version__)'
    rpc_interop_bin="${RBT_RPC_INTEROP_RUST_BIN:-${repo_root}/target/debug/rbt-rpc-interop}"
    if [[ ! -x "${rpc_interop_bin}" && -x "${repo_root}/target/release/rbt-rpc-interop" ]]; then
        rpc_interop_bin="${repo_root}/target/release/rbt-rpc-interop"
    fi
    [[ -x "${rpc_interop_bin}" ]] \
        || die "prebuilt Rust RPC interop binary is missing: ${rpc_interop_bin}"
    run_logged wheel-smoke-rpc env -u LD_LIBRARY_PATH -u LIBRARY_PATH -u PYTHONPATH \
        "${smoke_venv}/bin/python" "${repo_root}/python/tests/wheel_rpc_probe.py" \
        --repo-root "${repo_root}" --rust-bin "${rpc_interop_bin}" --run-id "${run_id}"

    smoke_service="rbt/python/wheel-smoke-${run_id}"
    "${repo_root}/target/release/image_fixture" \
        "${smoke_service}" 64 4 192 1 0 >"${run_dir}/wheel-smoke-publisher.log" 2>&1 &
    publisher_pid=$!
    sleep 0.2
    run_logged wheel-smoke-receive env -u LD_LIBRARY_PATH -u LIBRARY_PATH \
        "${smoke_venv}/bin/python" - "${smoke_service}" <<'PY'
import sys
import rbt

subscriber = rbt.ImageSubscriber.open(rbt.StreamDescriptor.image(sys.argv[1]))
sample = subscriber.receive(timeout=5)
assert sample is not None
image = sample.numpy()
assert image.shape == (4, 64, 3)
assert image.__array_interface__["data"][0] == sample._debug_payload_address()
assert not image.flags.owndata and not image.flags.writeable
print("fresh-wheel receive", image.shape, image.strides, rbt.__version__)
PY
    wait "${publisher_pid}" || die "fresh-wheel publisher failed; see ${run_dir}/wheel-smoke-publisher.log"

    fresh_publisher_service="rbt/python/wheel-smoke-publisher-${run_id}"
    fresh_publisher_gate="${smoke_root}/publisher-go"
    fresh_publisher_log="${run_dir}/wheel-smoke-installed-publisher.log"
    fresh_native_log="${run_dir}/wheel-smoke-native-subscriber.log"
    fresh_publisher_pid=''
    fresh_native_pid=''
    cleanup_fresh_publisher() {
        if [[ -n "${fresh_publisher_pid}" ]] && kill -0 "${fresh_publisher_pid}" 2>/dev/null; then
            kill -TERM "${fresh_publisher_pid}" 2>/dev/null || true
            wait "${fresh_publisher_pid}" 2>/dev/null || true
        fi
        if [[ -n "${fresh_native_pid}" ]] && kill -0 "${fresh_native_pid}" 2>/dev/null; then
            kill -TERM "${fresh_native_pid}" 2>/dev/null || true
            wait "${fresh_native_pid}" 2>/dev/null || true
        fi
    }
    trap cleanup_fresh_publisher EXIT

    env -u LD_LIBRARY_PATH -u LIBRARY_PATH \
        "${smoke_venv}/bin/python" - \
        "${fresh_publisher_service}" "${fresh_publisher_gate}" \
        >"${fresh_publisher_log}" 2>&1 <<'PY' &
import gc
import pathlib
import sys
import time

import rbt

service = sys.argv[1]
gate = pathlib.Path(sys.argv[2])
publisher = rbt.ImagePublisher.open(rbt.StreamDescriptor.image(service))
print("FRESH_WHEEL_PUBLISHER_READY", service, flush=True)
deadline = time.monotonic() + 15
while not gate.exists():
    if time.monotonic() >= deadline:
        raise TimeoutError("native subscriber did not become ready")
    time.sleep(0.01)
loan = publisher.loan(64, 4, rbt.PixelFormat.RGB8, sequence=0, timestamp_ns=0)
image = loan.numpy()
assert image.shape == (4, 64, 3)
assert image.__array_interface__["data"][0] == loan._debug_payload_address()
assert image.flags.writeable and not image.flags.owndata
flat = image.reshape(-1)
for offset in range(256):
    flat[offset::256] = offset
del flat, image
gc.collect()
loan.send()
print("fresh-wheel publish", service, "payload=768", flush=True)
PY
    fresh_publisher_pid="$!"
    for ((attempt = 0; attempt < 150; attempt++)); do
        if rg -Fq -- 'FRESH_WHEEL_PUBLISHER_READY' "${fresh_publisher_log}" 2>/dev/null; then
            break
        fi
        if ! kill -0 "${fresh_publisher_pid}" 2>/dev/null; then
            wait "${fresh_publisher_pid}" 2>/dev/null || true
            cat "${fresh_publisher_log}"
            die "fresh-wheel installed publisher exited before readiness"
        fi
        sleep 0.1
    done
    rg -Fq -- 'FRESH_WHEEL_PUBLISHER_READY' "${fresh_publisher_log}" \
        || die "timed out waiting for fresh-wheel installed publisher; see ${fresh_publisher_log}"

    printf '+ timeout --signal=TERM --kill-after=5s 30s %q image-subscriber %q 64 4 192 1\n' \
        "${RBT_PHASE4_INTEROP}" "${fresh_publisher_service}"
    timeout --signal=TERM --kill-after=5s 30s \
        "${RBT_PHASE4_INTEROP}" image-subscriber \
        "${fresh_publisher_service}" 64 4 192 1 >"${fresh_native_log}" 2>&1 &
    fresh_native_pid="$!"
    for ((attempt = 0; attempt < 150; attempt++)); do
        if rg -Fq -- 'IMAGE_SUBSCRIBER ready' "${fresh_native_log}" 2>/dev/null; then
            break
        fi
        if ! kill -0 "${fresh_native_pid}" 2>/dev/null; then
            wait "${fresh_native_pid}" 2>/dev/null || true
            cat "${fresh_native_log}"
            die "fresh-wheel native subscriber exited before readiness"
        fi
        sleep 0.1
    done
    rg -Fq -- 'IMAGE_SUBSCRIBER ready' "${fresh_native_log}" \
        || die "timed out waiting for fresh-wheel native subscriber; see ${fresh_native_log}"

    : >"${fresh_publisher_gate}"

    set +e
    wait "${fresh_publisher_pid}"
    fresh_publisher_status=$?
    set -e
    fresh_publisher_pid=''
    cat "${fresh_publisher_log}"
    ((fresh_publisher_status == 0)) \
        || die "fresh-wheel installed publisher failed with exit ${fresh_publisher_status}; see ${fresh_publisher_log}"

    set +e
    wait "${fresh_native_pid}"
    fresh_native_status=$?
    set -e
    fresh_native_pid=''
    cat "${fresh_native_log}"
    ((fresh_native_status == 0)) \
        || die "fresh-wheel native subscriber failed with exit ${fresh_native_status}; see ${fresh_native_log}"
    trap - EXIT

    if [[ "${RBT_PYTHON_SKIP_REGRESSION:-0}" == 1 ]]; then
        printf 'SKIP: existing regression explicitly disabled by RBT_PYTHON_SKIP_REGRESSION=1\n'
    else
        run_logged existing-regression ./scripts/verify_waitset.sh --full
    fi
fi

printf 'PASS: rbt Python %s verification completed; logs=%s\n' "${mode}" "${run_dir}"

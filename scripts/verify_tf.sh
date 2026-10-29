#!/usr/bin/env bash
set -euo pipefail

invocation_dir="${PWD}"
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

die() {
    printf 'RBT_TF_VERIFY FAIL %s\n' "$*" >&2
    [[ -z "${run_dir:-}" || ! -d "${run_dir}" ]] || printf 'RBT_TF_VERIFY logs=%s\n' "${run_dir}" >&2
    exit 1
}

usage() {
    cat <<'USAGE'
usage: scripts/verify_tf.sh [--quick|--full|--hardware] [--device PATH --config PATH --duration SEC]

--quick     TF math, buffer, transport, CLI, fresh Python wheel, native provenance,
            frozen ABI/unsafe audits, and inherited workspace verification
--full      quick plus cross-language native TF transport, external packages,
            Debug/Release/sanitizer builds, provider/recovery stress, and a 300s soak
--hardware  run the strict physical TF camera probe for the selected device/config

Hardware mode requires an explicit --device PATH, --config PATH, and --duration of
at least 300 seconds. The selected device is never inferred from the host.
USAGE
}

mode=quick
mode_seen=0
device_path=''
config_path=''
duration_seconds=300
device_seen=0
config_seen=0
duration_seen=0
while (($#)); do
    case "$1" in
        --quick|--full|--hardware)
            ((mode_seen == 0)) || die 'select exactly one mode'
            mode="${1#--}"
            mode_seen=1
            shift
            ;;
        --device)
            ((device_seen == 0)) || die 'duplicate --device option'
            (($# >= 2)) || die '--device requires a path'
            device_path="$2"
            device_seen=1
            shift 2
            ;;
        --config)
            ((config_seen == 0)) || die 'duplicate --config option'
            (($# >= 2)) || die '--config requires a path'
            config_path="$2"
            config_seen=1
            shift 2
            ;;
        --duration)
            ((duration_seen == 0)) || die 'duplicate --duration option'
            (($# >= 2)) || die '--duration requires seconds'
            duration_seconds="$2"
            duration_seen=1
            shift 2
            ;;
        --help|-h)
            (($# == 1)) || die '--help cannot be combined with other options'
            usage
            exit 0
            ;;
        *)
            usage >&2
            die "unknown argument: $1"
            ;;
    esac
done

if [[ "${mode}" == hardware ]]; then
    [[ "${device_seen}" == 1 ]] || die '--hardware requires an explicit --device PATH'
    [[ "${config_seen}" == 1 ]] || die '--hardware requires an explicit --config PATH'
    [[ "${device_path}" == /* ]] || die '--device must be an absolute path'
    [[ "${duration_seconds}" =~ ^[0-9]+$ ]] || die '--duration must be an integer number of seconds'
    ((duration_seconds >= 300)) || die '--hardware duration must be at least 300 seconds'
    [[ "${config_path}" == /* ]] || config_path="${invocation_dir}/${config_path}"
    [[ -f "${config_path}" ]] || die "selected TF config does not exist: ${config_path}"
else
    [[ "${device_seen}" == 0 && "${config_seen}" == 0 && "${duration_seen}" == 0 ]] \
        || die '--device, --config, and --duration are only valid with --hardware'
fi

for command_name in awk bash cargo cmake ctest date env find git mktemp python3 rg sed sha256sum tee timeout; do
    command -v "${command_name}" >/dev/null 2>&1 || die "required command not found: ${command_name}"
done

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
toolchain_bin="${toolchain_root}/bin"
cpp_prefix="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"
cc_path="${toolchain_bin}/x86_64-conda-linux-gnu-gcc"
cxx_path="${toolchain_bin}/x86_64-conda-linux-gnu-g++"
ar_path="${toolchain_bin}/x86_64-conda-linux-gnu-ar"
[[ -x "${cc_path}" ]] || die "pinned C compiler is missing: ${cc_path}"
[[ -x "${cxx_path}" ]] || die "pinned C++ compiler is missing: ${cxx_path}"
[[ -x "${ar_path}" ]] || die "pinned archiver is missing: ${ar_path}"
[[ -f "${cpp_prefix}/lib/cmake/iceoryx2-cxx/iceoryx2-cxxConfig.cmake" ]] \
    || die "maintained iceoryx2 C++ package config is missing under ${cpp_prefix}"
export PATH="${toolchain_bin}:${PATH}"
export CC="${cc_path}"
export CXX="${cxx_path}"
export AR="${ar_path}"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
export LD_LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
export LIBRARY_PATH="${toolchain_root}/lib:${cpp_prefix}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"
export CMAKE_PREFIX_PATH="${cpp_prefix}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"

python="${RBT_TF_VERIFY_PYTHON:-${RBT_TIME_PYTHON:-${repo_root}/target/python-verify-venv/bin/python}}"
[[ -x "${python}" ]] || die "fresh Python verification interpreter is missing: ${python}"
python_version="$("${python}" -c 'import sys; print(".".join(map(str, sys.version_info[:2])))')"
[[ "${python_version}" == 3.12 ]] || die "expected current CPython 3.12, found ${python_version}: ${python}"

log_root="${RBT_TF_LOG_DIR:-${repo_root}/target/tf-verify-logs}"
mkdir -p "${log_root}"
log_root="$(cd "${log_root}" && pwd)"
run_id="tf-${mode}-$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="${log_root}/${run_id}"
mkdir -p "${run_dir}"
timeout_seconds="${RBT_TF_STEP_TIMEOUT_SECONDS:-21600}"
[[ "${timeout_seconds}" =~ ^[1-9][0-9]*$ ]] || die 'RBT_TF_STEP_TIMEOUT_SECONDS must be a positive integer'
manifest="${run_dir}/manifest.tsv"
: >"${manifest}"

export RBT_TIME_PYTHON="${python}"
export RBT_RPC_VERIFY_PYTHON="${python}"
export RBT_TF_VERIFY_PYTHON="${python}"
export RBT_RPC_VERIFY_CPP_BUILD_DIR="${repo_root}/target/tf-verify-rpc-cpp-debug"
export RBT_TIME_LOG_DIR="${run_dir}/inherited-time"
export RBT_RPC_VERIFY_LOG_DIR="${run_dir}/inherited-rpc"
export RBT_TF_INTEROP_PYTHON="${python}"

printf 'RBT_TF_VERIFY mode=%s run_id=%s logs=%s toolchain=%s cpp_prefix=%s python=%s\n' \
    "${mode}" "${run_id}" "${run_dir}" "${toolchain_root}" "${cpp_prefix}" "${python}" \
    | tee "${run_dir}/environment.log"

run_logged() {
    local label="$1"
    shift
    local log_file="${run_dir}/${label}.log"
    local command_file="${run_dir}/${label}.command.log"
    {
        printf '+'
        printf ' %q' timeout --kill-after=30 "${timeout_seconds}s" "$@"
        printf '\n'
    } | tee "${command_file}"
    set +e
    timeout --kill-after=30 "${timeout_seconds}s" "$@" >"${log_file}" 2>&1
    local status=$?
    set -e
    cat "${log_file}"
    printf 'RBT_TF_STEP label=%s result=%s exit=%s log=%s\n' \
        "${label}" "$([[ ${status} == 0 ]] && printf PASS || printf FAIL)" "${status}" "${log_file}" \
        | tee -a "${command_file}"
    printf '%s\t%s\t%s\n' "${label}" "${status}" "${log_file}" >>"${manifest}"
    ((status == 0)) || die "${label} failed with exit ${status}; see ${log_file}"
}

todo_gate() {
    local label="$1"
    local path="$2"
    printf 'RBT_TF_STEP label=%s result=NOTRUN reason=TODO_missing_gate path=%s\n' \
        "${label}" "${path}" | tee "${run_dir}/${label}.log" "${run_dir}/${label}.command.log"
    printf '%s\tNOTRUN\tTODO_missing_gate\t%s\n' "${label}" "${path}" >>"${manifest}"
    die "TODO gate is not implemented: ${path}"
}

require_gate() {
    local path="$1"
    local label="$2"
    [[ -f "${path}" ]] || todo_gate "${label}" "${path}"
}

version_audit() {
    local cargo_version python_version cmake_version
    cargo_version="$(sed -n '/^\[workspace\.package\]/,/^\[/s/^version = "\([^"]*\)"/\1/p' Cargo.toml | head -n1)"
    python_version="$(sed -n 's/^version = "\([^"]*\)"/\1/p' python/pyproject.toml | head -n1)"
    cmake_version="$(sed -n 's/^project(rbt-cpp-sdk VERSION \([^ ]*\) LANGUAGES CXX).*/\1/p' sdk/cpp/CMakeLists.txt | head -n1)"
    [[ -n "${cargo_version}" && "${cargo_version}" == "${python_version}" \
        && "${cargo_version}" == "${cmake_version}" ]] \
        || die "product versions disagree: Cargo=${cargo_version:-missing}, Python=${python_version:-missing}, C++=${cmake_version:-missing}"
    [[ "${cargo_version}" == 0.8.0 ]] \
        || die "release verification requires 0.8.0 (found ${cargo_version})"
    printf 'RBT_TF_VERSION_AUDIT product=%s status=pass\n' "${cargo_version}" \
        | tee "${run_dir}/version-audit.log"
    printf 'version-audit\tPASS\tproduct=%s\n' "${cargo_version}" >>"${manifest}"
}

native_build_and_tests() {
    local cpp_build="${RBT_TF_CPP_BUILD_DIR:-${repo_root}/target/tf-verify-cpp-debug}"
    run_logged native-prefix-provenance "${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${cpp_prefix}"
    run_logged rust-tf-tests cargo test --locked -p rbt-tf
    run_logged rust-tf-transport-tests cargo test --locked -p rbt-tf-transport
    run_logged rust-tf-tools-tests cargo test --locked -p rbt-tf-tools --bin rbt-tf-tools
    run_logged cpp-tf-configure cmake -S "${repo_root}/sdk/cpp" -B "${cpp_build}" \
        -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER="${CXX}" \
        -DCMAKE_PREFIX_PATH="${cpp_prefix}"
    run_logged cpp-tf-build cmake --build "${cpp_build}" \
        --target tf_math_tests tf_tests tf_transport_tests tf_component_tests --parallel 2
    cpp_tf_ctest cpp-tf-ctest "${cpp_build}"
}

cpp_tf_ctest() {
    local label="$1"
    local build_dir="$2"
    local sanitizer="${3:-}"
    local junit="${run_dir}/${label}.junit.xml"
    local expected_tests='tf_math_tests,tf_tests,tf_transport_tests,tf_component_tests'
    local -a ctest_command=(ctest --test-dir "${build_dir}" --output-on-failure
        --no-tests=error --output-junit "${junit}"
        -R '^(tf_math_tests|tf_tests|tf_transport_tests|tf_component_tests)$')
    case "${sanitizer}" in
        address)
            run_logged "${label}" env ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 "${ctest_command[@]}"
            ;;
        undefined)
            run_logged "${label}" env UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 "${ctest_command[@]}"
            ;;
        '')
            run_logged "${label}" "${ctest_command[@]}"
            ;;
        *)
            die "unsupported C++ TF sanitizer completeness contract: ${sanitizer}"
            ;;
    esac
    run_logged "${label}-completeness-contract" python3 -c '
import pathlib
import sys
import xml.etree.ElementTree as ET

path = pathlib.Path(sys.argv[1])
expected = set(sys.argv[2].split(","))
root = ET.parse(path).getroot()
suites = list(root.iter("testsuite")) or ([root] if root.tag == "testsuite" else [])
cases = [case for suite in suites for case in suite.iter("testcase")]
names = [case.attrib.get("name", "") for case in cases]
skipped = sum(int(suite.attrib.get("skipped", 0)) for suite in suites)
skipped += sum(1 for case in cases if case.find("skipped") is not None)
failures = sum(int(suite.attrib.get("failures", 0)) for suite in suites)
failures += sum(1 for case in cases if case.find("failure") is not None or case.find("error") is not None)
assert len(cases) == len(expected) and set(names) == expected, {"tests": names, "expected": sorted(expected)}
assert skipped == 0, {"skipped": skipped, "tests": names}
assert failures == 0, {"failures": failures, "tests": names}
print("C++ TF tests PASS", {"tests": sorted(names), "skipped": skipped})
' "${junit}" "${expected_tests}"
}

fresh_python_tests() {
    local quick_python="${run_dir}/quick-python-verify"
    local wheel_dir="${run_dir}/quick-wheel"
    local test_dir="${run_dir}/python-tests"
    require_gate "${repo_root}/python/tests/test_tf_transport.py" python-tf-transport-tests
    require_gate "${repo_root}/python/tests/test_tf_component.py" python-tf-component-tests
    require_gate "${repo_root}/tests/tf/external/python/test_installed_wheel.py" python-installed-wheel-origin-smoke
    mkdir -p "${wheel_dir}" "${test_dir}" "${run_dir}/python-cwd"
    local maturin_version=1.15.0
    run_logged pinned-maturin-version "${python}" -m maturin --version
    run_logged pinned-maturin-contract python3 -c \
        'import pathlib,sys; observed=pathlib.Path(sys.argv[1]).read_text(); expected=f"maturin {sys.argv[2]}"; assert expected in observed, f"expected pinned {expected}, found {observed.strip()}"; print("pinned maturin PASS",expected)' \
        "${run_dir}/pinned-maturin-version.log" "${maturin_version}"
    run_logged quick-python-wheel env -u CONDA_PREFIX \
        VIRTUAL_ENV="$(dirname "$(dirname "${python}")")" PYO3_PYTHON="${python}" \
        CARGO_TARGET_DIR="${run_dir}/quick-python-cargo-target" \
        bash -c 'cd "$1" && exec "$2" -m maturin build --release --locked --out "$3"' \
        _ "${repo_root}/python" "${python}" "${wheel_dir}"
    local wheel
    run_logged quick-python-wheel-selection python3 -c \
        'import pathlib,sys; wheels=list(pathlib.Path(sys.argv[1]).glob("rbt-*.whl")); assert len(wheels)==1, f"expected one fresh wheel, found {len(wheels)}"; print(wheels[0].resolve())' \
        "${wheel_dir}"
    wheel="$(cat "${run_dir}/quick-python-wheel-selection.log")"
    run_logged quick-python-venv "${python}" -m venv --system-site-packages "${quick_python}"
    run_logged quick-python-wheel-install "${quick_python}/bin/python" -m pip install \
        --no-deps --force-reinstall "${wheel}"
    run_logged quick-python-wheel-site-path "${quick_python}/bin/python" -c \
        'import sysconfig; print(sysconfig.get_paths()["purelib"])'
    local quick_python_site
    quick_python_site="$(cat "${run_dir}/quick-python-wheel-site-path.log")"
    [[ -d "${quick_python_site}" ]] || die "wheel site-packages directory is missing: ${quick_python_site}"
    cp "${repo_root}/python/tests/test_tf_math.py" "${repo_root}/python/tests/test_tf.py" \
        "${repo_root}/python/tests/test_tf_transport.py" \
        "${repo_root}/python/tests/test_tf_component.py" "${test_dir}/"
    local python_junit="${run_dir}/python-tf-tests.xml"
    run_logged python-tf-focused-tests env -u PYTHONPATH -u PYTHONHOME -u CONDA_PREFIX \
        PYTHONNOUSERSITE=1 PYTHONPATH="${quick_python_site}" RBT_TF_EXPECTED_VENV="${quick_python}" \
        RBT_TF_SOURCE_ROOT="${repo_root}" RBT_TF_COMPONENT_REPOSITORY="${repo_root}" \
        RBT_TF_COMPONENT_OVERLAY="${quick_python_site}" \
        bash -c \
        'cd "$1" && exec "$2" -m pytest --import-mode=importlib -q --junitxml "$3" "$4" "$5" "$6" "$7" "$8"' \
        _ "${run_dir}/python-cwd" "${python}" \
        "${python_junit}" "${test_dir}/test_tf_math.py" "${test_dir}/test_tf.py" \
        "${test_dir}/test_tf_transport.py" "${test_dir}/test_tf_component.py" \
        "${repo_root}/tests/tf/external/python/test_installed_wheel.py"
    run_logged python-tf-no-skips-contract python3 -c '
import pathlib
import sys
import xml.etree.ElementTree as ET

root = ET.parse(pathlib.Path(sys.argv[1])).getroot()
suites = list(root.iter("testsuite")) or [root]
cases = [case for suite in suites for case in suite.iter("testcase")]
modules = {case.attrib.get("classname", "").split(".")[-1] for case in cases}
case_names = {case.attrib.get("name", "") for case in cases}
expected = {"test_tf_math", "test_tf", "test_tf_transport", "test_tf_component"}
skipped = sum(int(suite.attrib.get("skipped", 0)) for suite in suites)
skipped += sum(1 for case in cases if case.find("skipped") is not None)
assert expected <= modules, {"modules": sorted(modules), "expected": sorted(expected)}
assert any(name.startswith("test_installed_wheel_") for name in case_names), case_names
assert skipped == 0, {"skipped": skipped, "modules": sorted(modules)}
print("Python TF focused tests PASS", {"modules": sorted(modules), "skipped": skipped})
' "${python_junit}"
    printf 'fresh_python_wheel=%s\n' "${wheel}" | tee -a "${run_dir}/environment.log"
    export RBT_TF_INTEROP_PYTHON="${quick_python}/bin/python"
}

cli_json_tests() {
    run_logged cli-tf-json-integration cargo test --locked -p rbt-cli --test tf_cli
}

strict_audits() {
    run_logged tf-unsafe-audit python3 "${repo_root}/scripts/audit_unsafe.py" --json
    run_logged tf-unsafe-audit-contract python3 -c \
        'import json,pathlib,sys; report=json.loads(pathlib.Path(sys.argv[1]).read_text()); assert report["legacy"] == 49, report; assert report["tf_generated"] == 24, report; assert report["total"] == 73, report; print("TF unsafe markers PASS legacy=49 tf_generated=24 total=73")' \
        "${run_dir}/tf-unsafe-audit.log"
    run_logged frozen-hash-test cargo test --locked -p rbt-types --lib \
        tests::built_in_layout_hashes_are_fixed -- --exact
}

run_unittest_contract() {
    local label="$1"
    local test_file="$2"
    local expected_count="$3"
    run_logged "${label}" python3 -c '
import importlib.util
import pathlib
import sys
import unittest

path = pathlib.Path(sys.argv[1]).resolve()
expected_count = int(sys.argv[2])
spec = importlib.util.spec_from_file_location("tf_gate_test_suite", path)
assert spec is not None and spec.loader is not None, path
module = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = module
spec.loader.exec_module(module)
result = unittest.TextTestRunner(verbosity=2).run(
    unittest.defaultTestLoader.loadTestsFromModule(module)
)
assert result.testsRun == expected_count, {"expected": expected_count, "ran": result.testsRun}
assert not result.skipped, result.skipped
assert result.wasSuccessful(), {"failures": result.failures, "errors": result.errors}
print("TF script unit tests PASS", {"file": str(path), "tests": result.testsRun})
' "${test_file}" "${expected_count}"
}

script_unit_tests() {
    run_unittest_contract tf-unsafe-audit-unit-tests \
        "${repo_root}/tests/tf/audit_unsafe_test.py" 9
    run_unittest_contract tf-hardware-jsonl-unit-tests \
        "${repo_root}/tests/tf/test_hardware_gate_jsonl.py" 4
    run_unittest_contract tf-interop-harness-unit-tests \
        "${repo_root}/tests/tf/test_interop_harness.py" 9
    run_unittest_contract tf-process-pressure-unit-tests \
        "${repo_root}/tests/tf/test_process_pressure.py" 12
    run_logged tf-hardware-software-fixtures "${python}" \
        "${repo_root}/scripts/test_tf_hardware.py" --self-test softwareonly
}

tf_math_fixture() {
    require_gate "${repo_root}/scripts/test_tf_interop.py" tf-cross-language-math
    require_gate "${repo_root}/tests/fixtures/tf/semantics.json" tf-math-fixture
    local wheel_python="${RBT_TF_INTEROP_PYTHON:-${python}}"
    run_logged tf-math-cross-language env -u PYTHONPATH -u PYTHONHOME -u CONDA_PREFIX \
        RBT_TF_INTEROP_PYTHON="${wheel_python}" "${wheel_python}" \
        "${repo_root}/scripts/test_tf_interop.py" \
        --math-only --python "${wheel_python}" \
        --cpp-build "${RBT_TF_INTEROP_CPP_BUILD:-${repo_root}/target/tf-verify-cpp-debug}" \
        --rust-probe "${RBT_TF_INTEROP_RUST_PROBE:-${repo_root}/target/debug/rbt-tf-tools}" \
        --log-dir "${run_dir}/math-interop"
}

quick() {
    version_audit
    native_build_and_tests
    fresh_python_tests
    cli_json_tests
    strict_audits
    script_unit_tests
    tf_math_fixture
    run_logged inherited-time-quick env \
        RBT_TIME_LOG_DIR="${run_dir}/inherited-time" \
        RBT_RPC_VERIFY_LOG_DIR="${run_dir}/inherited-rpc" \
        "${repo_root}/scripts/verify_time.sh" --quick
}

cpp_matrix() {
    local build_type key build_dir sanitizer flags
    for build_type in Debug Release; do
        key="${build_type,,}"
        build_dir="${repo_root}/target/tf-verify-cpp-${key}"
        run_logged "cpp-${key}-configure" cmake -S "${repo_root}/sdk/cpp" -B "${build_dir}" \
            -G Ninja -DCMAKE_BUILD_TYPE="${build_type}" -DCMAKE_CXX_COMPILER="${CXX}" \
            -DCMAKE_PREFIX_PATH="${cpp_prefix}"
        run_logged "cpp-${key}-build" cmake --build "${build_dir}" \
            --target tf_math_tests tf_tests tf_transport_tests tf_component_tests --parallel 2
        cpp_tf_ctest "cpp-${key}-ctest" "${build_dir}"
    done
    for sanitizer in address undefined; do
        flags="-fsanitize=${sanitizer} -fno-omit-frame-pointer"
        build_dir="${repo_root}/target/tf-verify-cpp-${sanitizer}"
        run_logged "cpp-${sanitizer}-configure" cmake -S "${repo_root}/sdk/cpp" -B "${build_dir}" \
            -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER="${CXX}" \
            -DCMAKE_PREFIX_PATH="${cpp_prefix}" "-DCMAKE_CXX_FLAGS=${flags}" \
            "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=${sanitizer}"
        run_logged "cpp-${sanitizer}-build" cmake --build "${build_dir}" \
            --target tf_math_tests tf_tests tf_transport_tests tf_component_tests --parallel 2
        cpp_tf_ctest "cpp-${sanitizer}-ctest" "${build_dir}" "${sanitizer}"
    done
}

full() {
    local soak_seconds="${RBT_TF_SOAK_SECONDS:-300}"
    [[ "${soak_seconds}" =~ ^[0-9]+$ ]] || die 'RBT_TF_SOAK_SECONDS must be an integer'
    ((soak_seconds >= 300)) || die 'full TF software soak must be at least 300 seconds'
    require_gate "${repo_root}/python/tests/test_tf_transport.py" python-tf-transport-tests
    require_gate "${repo_root}/python/tests/test_tf_component.py" python-tf-component-tests
    require_gate "${repo_root}/scripts/test_tf_interop.py" tf-native-interop
    require_gate "${repo_root}/tests/fixtures/tf/semantics.json" tf-math-fixture
    require_gate "${repo_root}/scripts/tf_process_probe.py" tf-process-probe
    require_gate "${repo_root}/scripts/benchmark_tf.sh" tf-benchmark
    require_gate "${repo_root}/scripts/soak_tf.sh" tf-process-soak
    require_gate "${repo_root}/scripts/test_tf_packages.sh" tf-package-consumers
    version_audit
    run_logged native-prefix-provenance "${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${cpp_prefix}"
    run_logged rust-tf-tests cargo test --locked -p rbt-tf
    run_logged rust-tf-transport-tests cargo test --locked -p rbt-tf-transport
    run_logged rust-tf-tools-tests cargo test --locked -p rbt-tf-tools --bin rbt-tf-tools
    fresh_python_tests
    cli_json_tests
    strict_audits
    script_unit_tests
    # Parent verify_rpc --full schedules the full Python and WaitSet coverage.
    run_logged inherited-time-full env \
        RBT_PYTHON_SKIP_REGRESSION=1 \
        RBT_PYTHON_COMPONENT_SKIP_INHERITED=1 \
        RBT_TIME_LOG_DIR="${run_dir}/inherited-time" \
        RBT_RPC_VERIFY_LOG_DIR="${run_dir}/inherited-rpc" \
        "${repo_root}/scripts/verify_time.sh" --full
    run_logged rust-tf-100-provider-replacement cargo test --locked -p rbt-tf --test buffer \
        activation_replacement_rejects_late_samples_and_bounds_history -- --exact
    run_logged rust-tf-concurrent-ingest-lookup cargo test --locked -p rbt-tf --test buffer \
        concurrent_ingest_and_lookup_share_one_safe_buffer -- --exact
    run_logged rust-tf-static-recovery cargo test --locked -p rbt-tf-transport --test native_transport \
        periodic_snapshot_refresh_recovers_when_the_final_update_is_not_drained -- --exact
    cpp_matrix
    require_gate "${repo_root}/scripts/test_tf_interop.py" tf-native-interop
    require_gate "${repo_root}/tests/fixtures/tf/semantics.json" tf-math-fixture
    run_logged tf-six-pair-native-interop "${python}" "${repo_root}/scripts/test_tf_interop.py" \
        --python "${RBT_TF_INTEROP_PYTHON:-${python}}" \
        --cpp-build "${repo_root}/target/tf-verify-cpp-debug" \
        --rust-probe "${repo_root}/target/debug/rbt-tf-tools" --log-dir "${run_dir}/interop"
    run_logged tf-external-packages env \
        RBT_TF_PACKAGE_LOG_DIR="${run_dir}/packages" \
        RBT_TF_PACKAGE_PREFIX="${run_dir}/packages/sdk" \
        "${repo_root}/scripts/test_tf_packages.sh" --python "${python}"
    require_gate "${repo_root}/scripts/tf_process_probe.py" tf-process-probe
    require_gate "${repo_root}/scripts/benchmark_tf.sh" tf-benchmark
    require_gate "${repo_root}/scripts/soak_tf.sh" tf-process-soak
    run_logged tf-benchmark env RBT_TF_BENCH_LOG_DIR="${run_dir}/benchmark" \
        CARGO_NET_OFFLINE=true \
        "${repo_root}/scripts/benchmark_tf.sh"
    run_logged tf-process-provider-replacements env CARGO_NET_OFFLINE=true \
        "${repo_root}/scripts/soak_tf.sh" --replacement-cycles 100 --consumers 2
    run_logged tf-process-healthy-composer-restarts env CARGO_NET_OFFLINE=true \
        "${repo_root}/scripts/soak_tf.sh" --restart-count 10 --consumers 2
    run_logged tf-process-combined-outage env CARGO_NET_OFFLINE=true \
        "${repo_root}/scripts/soak_tf.sh" --combined-outage --duration "${soak_seconds}" --consumers 2
    run_logged tf-mixed-soak env CARGO_NET_OFFLINE=true \
        "${repo_root}/scripts/soak_tf.sh" --soak --duration "${soak_seconds}" --consumers 2
}

hardware() {
    if [[ ! -c "${device_path}" ]]; then
        {
            printf 'RBT_TF_HARDWARE status=NOTRUN reason=selected_device_unavailable path=%s\n' "${device_path}"
            if [[ -e "${device_path}" ]]; then
                printf 'device_type=not_character_device\n'
            else
                printf 'device_type=missing\n'
            fi
        } | tee "${run_dir}/hardware-notrun.log"
        printf 'hardware-probe\tNOTRUN\tselected_device_unavailable\t%s\n' \
            "${device_path}" >>"${manifest}"
        die 'selected TF device is unavailable; no hardware pass was recorded'
    fi
    require_gate "${repo_root}/scripts/test_tf_hardware.py" tf-physical-hardware-probe
    local hardware_timeout=$((duration_seconds + 180))
    timeout_seconds="${hardware_timeout}"
    run_logged strict-physical-tf-hardware "${python}" "${repo_root}/scripts/test_tf_hardware.py" \
        --device "${device_path}" --config "${config_path}" --duration "${duration_seconds}"
}

case "${mode}" in
    quick) quick ;;
    full) full ;;
    hardware) hardware ;;
esac

printf 'PASS: TF %s verification completed; logs=%s\n' "${mode}" "${run_dir}"

#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

die() {
    printf 'RBT_TF_PACKAGES FAIL %s\n' "$*" >&2
    [[ -z "${run_dir:-}" || ! -d "${run_dir}" ]] || printf 'RBT_TF_PACKAGES logs=%s\n' "${run_dir}" >&2
    exit 1
}

usage() {
    cat <<'USAGE'
usage: scripts/test_tf_packages.sh [--python PATH] [--prefix PATH] [--log-dir PATH]

Builds a fresh Python wheel and checks TF math/buffer APIs from that installed
wheel, then installs the current Rust/C++ SDK and builds external TF consumers.
The Python interpreter must be the fresh CPython 3.12 native verification env.
USAGE
}

python="${RBT_TF_VERIFY_PYTHON:-${RBT_TIME_PYTHON:-${repo_root}/target/python-verify-venv/bin/python}}"
prefix="${RBT_TF_PACKAGE_PREFIX:-}"
log_root="${RBT_TF_PACKAGE_LOG_DIR:-${repo_root}/target/tf-verify-logs}"
python_seen=0
prefix_seen=0
log_seen=0
while (($#)); do
    case "$1" in
        --python)
            ((python_seen == 0)) || die 'duplicate --python option'
            (($# >= 2)) || die '--python requires a path'
            python="$2"
            python_seen=1
            shift 2
            ;;
        --prefix)
            ((prefix_seen == 0)) || die 'duplicate --prefix option'
            (($# >= 2)) || die '--prefix requires a path'
            prefix="$2"
            prefix_seen=1
            shift 2
            ;;
        --log-dir)
            ((log_seen == 0)) || die 'duplicate --log-dir option'
            (($# >= 2)) || die '--log-dir requires a path'
            log_root="$2"
            log_seen=1
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

for command_name in cargo cmake date find git mktemp python3 rg sha256sum tee timeout; do
    command -v "${command_name}" >/dev/null 2>&1 || die "required command not found: ${command_name}"
done
product_cargo_version="$(sed -n '/^\[workspace\.package\]/,/^\[/s/^version = "\([^"]*\)"/\1/p' Cargo.toml | head -n1)"
product_python_version="$(sed -n 's/^version = "\([^"]*\)"/\1/p' python/pyproject.toml | head -n1)"
product_cpp_version="$(sed -n 's/^project(rbt-cpp-sdk VERSION \([^ ]*\) LANGUAGES CXX).*/\1/p' sdk/cpp/CMakeLists.txt | head -n1)"
[[ -x "${python}" ]] || die "Python verification interpreter is missing: ${python}"
python_version="$("${python}" -c 'import sys; print(".".join(map(str, sys.version_info[:2])))')"
[[ "${python_version}" == 3.12 ]] || die "expected fresh CPython 3.12, found ${python_version}: ${python}"

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

mkdir -p "${log_root}"
log_root="$(cd "${log_root}" && pwd)"
run_id="tf-packages-$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="${log_root}/${run_id}"
mkdir -p "${run_dir}/wheel" "${run_dir}/external/rust/src" "${run_dir}/external/cpp/build" \
    "${run_dir}/external/python-cwd"
timeout_seconds="${RBT_TF_PACKAGE_STEP_TIMEOUT_SECONDS:-7200}"
[[ "${timeout_seconds}" =~ ^[1-9][0-9]*$ ]] || die 'RBT_TF_PACKAGE_STEP_TIMEOUT_SECONDS must be a positive integer'
manifest="${run_dir}/manifest.tsv"
: >"${manifest}"
printf 'RBT_TF_PACKAGES run_id=%s logs=%s python=%s cpp_prefix=%s\n' \
    "${run_id}" "${run_dir}" "${python}" "${cpp_prefix}" | tee "${run_dir}/environment.log"
printf 'version-audit\tPASS\tproduct=%s\n' "${product_cargo_version}" >>"${run_dir}/manifest.tsv"

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
    printf 'RBT_TF_PACKAGES_STEP label=%s result=%s exit=%s log=%s\n' \
        "${label}" "$([[ ${status} == 0 ]] && printf PASS || printf FAIL)" "${status}" "${log_file}" \
        | tee -a "${command_file}"
    printf '%s\t%s\t%s\n' "${label}" "${status}" "${log_file}" >>"${manifest}"
    ((status == 0)) || die "${label} failed with exit ${status}; see ${log_file}"
}

require_file() {
    [[ -f "$1" ]] || die "required package-gate input is missing: $1"
}

todo_gate() {
    local label="$1"
    local path="$2"
    printf 'RBT_TF_PACKAGES_STEP label=%s result=NOTRUN reason=TODO_missing_gate path=%s\n' \
        "${label}" "${path}" | tee "${run_dir}/${label}.log" "${run_dir}/${label}.command.log"
    printf '%s\tNOTRUN\tTODO_missing_gate\t%s\n' "${label}" "${path}" >>"${manifest}"
    die "TODO gate is not implemented: ${path}"
}

[[ "${product_cargo_version}" == "${product_python_version}" \
    && "${product_cargo_version}" == "${product_cpp_version}" ]] \
    || die "product versions disagree: Cargo=${product_cargo_version:-missing}, Python=${product_python_version:-missing}, C++=${product_cpp_version:-missing}"
[[ "${product_cargo_version}" == 0.8.0 ]] \
    || die "release verification requires 0.8.0 (found ${product_cargo_version})"
[[ -f "${repo_root}/scripts/test_tf_interop.py" ]] \
    || todo_gate tf-native-interop "${repo_root}/scripts/test_tf_interop.py"
[[ -f "${repo_root}/tests/fixtures/tf/semantics.json" ]] \
    || todo_gate tf-math-fixture "${repo_root}/tests/fixtures/tf/semantics.json"
[[ -f "${repo_root}/python/tests/test_tf_transport.py" ]] \
    || todo_gate python-tf-transport-tests "${repo_root}/python/tests/test_tf_transport.py"
[[ -f "${repo_root}/python/tests/test_tf_component.py" ]] \
    || todo_gate python-tf-component-tests "${repo_root}/python/tests/test_tf_component.py"
printf 'version-audit\tPASS\tproduct=%s\n' "${product_cargo_version}" >>"${manifest}"

wheel_version=1.15.0
run_logged maturin-version "${python}" -m maturin --version
run_logged maturin-pin-contract python3 -c \
    'import pathlib,sys; observed=pathlib.Path(sys.argv[1]).read_text(); expected=f"maturin {sys.argv[2]}"; assert expected in observed, f"expected pinned {expected}, found {observed.strip()}"; print("pinned maturin PASS",expected)' \
    "${run_dir}/maturin-version.log" "${wheel_version}"
run_logged fresh-python-wheel env -u CONDA_PREFIX \
    VIRTUAL_ENV="$(dirname "$(dirname "${python}")")" PYO3_PYTHON="${python}" \
    CARGO_TARGET_DIR="${run_dir}/wheel-cargo-target" \
    bash -c 'cd "$1" && exec "$2" -m maturin build --release --locked --out "$3"' \
    _ "${repo_root}/python" "${python}" "${run_dir}/wheel"
run_logged fresh-wheel-selection python3 -c \
    'import pathlib,sys; wheels=list(pathlib.Path(sys.argv[1]).glob("rbt-*.whl")); assert len(wheels)==1, f"expected exactly one fresh Python wheel, found {len(wheels)}"; print(wheels[0].resolve())' \
    "${run_dir}/wheel"
wheel="$(cat "${run_dir}/fresh-wheel-selection.log")"
printf 'fresh_wheel=%s\n' "${wheel}" | tee -a "${run_dir}/environment.log"

python_venv="${run_dir}/external/python-venv"
run_logged clean-python-venv "${python}" -m venv --system-site-packages "${python_venv}"
run_logged python-wheel-install "${python_venv}/bin/python" -m pip install --no-deps --force-reinstall "${wheel}"
run_logged python-wheel-site-path "${python_venv}/bin/python" -c \
    'import sysconfig; print(sysconfig.get_paths()["purelib"])'
wheel_python_site="$(cat "${run_dir}/python-wheel-site-path.log")"
[[ -d "${wheel_python_site}" ]] || die "wheel site-packages directory is missing: ${wheel_python_site}"
require_file "${repo_root}/tests/tf/external/python/test_installed_wheel.py"
require_file "${repo_root}/python/tests/test_tf_math.py"
require_file "${repo_root}/python/tests/test_tf.py"
require_file "${repo_root}/python/tests/test_tf_transport.py"
require_file "${repo_root}/python/tests/test_tf_component.py"
python_junit="${run_dir}/external/python-tf-tests.xml"
run_logged installed-python-tf-tests env -u PYTHONPATH -u PYTHONHOME -u CONDA_PREFIX \
    PYTHONNOUSERSITE=1 PYTHONPATH="${wheel_python_site}" RBT_TF_EXPECTED_VENV="${python_venv}" \
    RBT_TF_SOURCE_ROOT="${repo_root}" RBT_TF_COMPONENT_REPOSITORY="${repo_root}" \
    RBT_TF_COMPONENT_OVERLAY="${wheel_python_site}" \
    bash -c 'cd "$1" && exec "$2" -m pytest --import-mode=importlib -q --junitxml "$3" "$4" "$5" "$6" "$7" "$8"' \
    _ "${run_dir}/external/python-cwd" "${python}" \
    "${python_junit}" \
    "${repo_root}/tests/tf/external/python/test_installed_wheel.py" \
    "${repo_root}/python/tests/test_tf_math.py" "${repo_root}/python/tests/test_tf.py" \
    "${repo_root}/python/tests/test_tf_transport.py" \
    "${repo_root}/python/tests/test_tf_component.py"
run_logged installed-python-no-skips-contract "${python}" -c \
    'import sys,xml.etree.ElementTree as E; root=E.parse(sys.argv[1]).getroot(); suites=list(root.iter("testsuite")) or [root]; cases=[case for suite in suites for case in suite.iter("testcase")]; modules={case.attrib.get("classname", "").split(".")[-1] for case in cases}; names={case.attrib.get("name", "") for case in cases}; expected={"test_tf_math","test_tf","test_tf_transport","test_tf_component"}; skipped=sum(int(s.attrib.get("skipped",0)) for s in suites)+sum(1 for case in cases if case.find("skipped") is not None); assert expected <= modules, {"modules":sorted(modules),"expected":sorted(expected)}; assert any(name.startswith("test_installed_wheel_") for name in names), names; assert skipped==0, {"skipped":skipped,"modules":sorted(modules)}; print("installed wheel TF tests PASS",{"modules":sorted(modules),"skipped":skipped})' \
    "${python_junit}"

sdk_prefix="${prefix:-${run_dir}/external/sdk}"
case "${sdk_prefix}" in
    /|"${HOME}") die "refusing unsafe SDK install prefix: ${sdk_prefix}" ;;
esac
if [[ -e "${sdk_prefix}" && "${sdk_prefix}" != "${run_dir}/external/sdk" ]]; then
    die "explicit --prefix must not already exist: ${sdk_prefix}"
fi
run_logged sdk-install env \
    RBT_INSTALL_CARGO_TARGET_DIR="${run_dir}/sdk-cargo-target" \
    RBT_INSTALL_CPP_BUILD_DIR="${run_dir}/sdk-cpp-release" \
    "${repo_root}/scripts/install_v01.sh" --prefix "${sdk_prefix}" --python-wheel "${wheel}"
installed_rust_sdk="${sdk_prefix}/share/rbt/rust"
require_file "${installed_rust_sdk}/crates/rbt/Cargo.toml"
require_file "${installed_rust_sdk}/crates/rbt-tf/Cargo.toml"
require_file "${installed_rust_sdk}/crates/rbt-tf-transport/Cargo.toml"
require_file "${installed_rust_sdk}/crates/rbt-types/Cargo.toml"
require_file "${installed_rust_sdk}/Cargo.lock"

rust_project="${run_dir}/external/rust"
rust_manifest="${rust_project}/Cargo.toml"
if [[ "${installed_rust_sdk}" == *'"'* || "${installed_rust_sdk}" == *$'\n'* ]]; then
    die 'installed Rust SDK path cannot be represented safely in an external Cargo manifest'
fi
sed "s|@RBT_INSTALLED_RUST_SDK@|${installed_rust_sdk}|g" \
    "${repo_root}/tests/tf/external/rust/Cargo.toml.in" >"${rust_manifest}"
cp -f "${installed_rust_sdk}/Cargo.lock" "${rust_project}/Cargo.lock"
cp -f "${repo_root}/tests/tf/external/rust/src/main.rs" "${rust_project}/src/main.rs"
run_logged external-rust-consumer env CARGO_TARGET_DIR="${run_dir}/external/rust-target" \
    cargo run --offline --manifest-path "${rust_manifest}"
run_logged external-rust-metadata env CARGO_TARGET_DIR="${run_dir}/external/rust-target" \
    cargo metadata --offline --manifest-path "${rust_manifest}" --format-version 1
run_logged installed-rust-sdk-origin-contract "${python}" -c \
    'import json,pathlib,sys; data=json.loads(pathlib.Path(sys.argv[1]).read_text()); root=pathlib.Path(sys.argv[2]).resolve(); expected={"rbt","rbt-tf","rbt-tf-transport","rbt-types"}; found={p["name"]: pathlib.Path(p["manifest_path"]).resolve() for p in data["packages"] if p["name"] in expected}; assert expected <= found.keys(), f"missing installed packages: {expected-found.keys()}"; assert all(root in path.parents for path in found.values()), f"Cargo resolved outside installed SDK: {found}"; print("external Cargo dependencies use installed SDK:", ", ".join(f"{name}={path}" for name,path in sorted(found.items())))' \
    "${run_dir}/external-rust-metadata.log" "${installed_rust_sdk}"

require_file "${sdk_prefix}/lib/cmake/rbt/rbtConfig.cmake"
require_file "${sdk_prefix}/include/rbt/transforms.hpp"
require_file "${sdk_prefix}/include/rbt/tf_math.hpp"
require_file "${sdk_prefix}/include/rbt/tf_transport.hpp"
require_file "${repo_root}/tests/tf/external/cpp/CMakeLists.txt"
require_file "${repo_root}/tests/tf/external/cpp/main.cpp"
run_logged external-cpp-configure cmake -S "${repo_root}/tests/tf/external/cpp" \
    -B "${run_dir}/external/cpp/build" -G Ninja \
    -DCMAKE_CXX_COMPILER="${CXX}" \
    -DCMAKE_PREFIX_PATH="${sdk_prefix};${cpp_prefix}" \
    -Drbt_DIR="${sdk_prefix}/lib/cmake/rbt" \
    -DRBT_EXPECTED_PREFIX="${sdk_prefix}" \
    -DRBT_EXPECTED_VERSION="${product_cargo_version}"
run_logged external-cpp-build cmake --build "${run_dir}/external/cpp/build" --parallel 2
run_logged external-cpp-consumer "${run_dir}/external/cpp/build/tf_external_consumer"

interop_script="${repo_root}/scripts/test_tf_interop.py"
run_logged installed-wheel-six-pair-interop env -u PYTHONPATH -u PYTHONHOME -u CONDA_PREFIX \
    RBT_TF_INTEROP_PYTHON="${python_venv}/bin/python" \
    "${python}" "${interop_script}" \
    --python "${python_venv}/bin/python" \
    --cpp-build "${RBT_TF_PACKAGE_INTEROP_CPP_BUILD:-${repo_root}/target/tf-packages-cpp-debug}" \
    --rust-probe "${RBT_TF_PACKAGE_INTEROP_RUST_PROBE:-${repo_root}/target/debug/rbt-tf-tools}" \
    --log-dir "${run_dir}/external/wheel-interop"

printf 'PASS: TF package consumers completed; logs=%s\n' "${run_dir}"

#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

usage() {
    printf 'usage: scripts/install_v01.sh [--prefix PATH] [--python-wheel PATH] [--debug]\n'
}

prefix="${RBT_INSTALL_PREFIX:-${repo_root}/target/v01-install}"
python_wheel="${RBT_PYTHON_WHEEL:-}"
build_type=Release
while (($# > 0)); do
    case "$1" in
        --prefix)
            (($# >= 2)) || die "--prefix requires a path"
            prefix="$2"
            shift 2
            ;;
        --python-wheel)
            (($# >= 2)) || die "--python-wheel requires a path"
            python_wheel="$2"
            shift 2
            ;;
        --debug)
            build_type=Debug
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1"
            ;;
    esac
done

[[ -n "${prefix}" ]] || die "install prefix must not be empty"
case "${prefix}" in
    /|"${HOME}") die "refusing unsafe install prefix: ${prefix}" ;;
esac
RBT_IOX2_FORK_SOURCE="${repo_root}/third_party/iceoryx2-rbt/source" \
    bash "${repo_root}/scripts/bootstrap_iceoryx2_rbt.sh" --source-only
prefix="$(mkdir -p "${prefix}" && cd "${prefix}" && pwd)"

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
cpp_prefix="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"
bash "${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${cpp_prefix}"

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
export CMAKE_PREFIX_PATH="${cpp_prefix}${CMAKE_PREFIX_PATH:+:${CMAKE_PREFIX_PATH}}"

command -v cargo >/dev/null 2>&1 || die "cargo is required"
command -v cmake >/dev/null 2>&1 || die "cmake is required"

cargo_install_target="${RBT_INSTALL_CARGO_TARGET_DIR:-${repo_root}/target/v01-install-cargo-target}"
for package_path in crates/rbt-cli crates/rbt-idl examples/rust/composer; do
    CARGO_TARGET_DIR="${cargo_install_target}" cargo install \
        --offline \
        --locked \
        --force \
        --root "${prefix}" \
        --path "${package_path}"
done

rust_sdk="${prefix}/share/rbt/rust"
mkdir -p "${rust_sdk}/crates" "${rust_sdk}/generated"
cp -f packaging/rust-sdk-workspace.toml "${rust_sdk}/Cargo.toml"
cp -f Cargo.lock "${rust_sdk}/"
cp -a crates/. "${rust_sdk}/crates/"
cp -a generated/. "${rust_sdk}/generated/"

fork_root="${repo_root}/third_party/iceoryx2-rbt"
installed_fork_root="${rust_sdk}/third_party/iceoryx2-rbt"
mkdir -p "${rust_sdk}/third_party"
declare -a fork_archive_args=(
    --exclude-vcs
    --exclude=source/target
)
while IFS= read -r -d '' target_dir; do
    relative_target="${target_dir#"${fork_root}/"}"
    fork_archive_args+=("--exclude=${relative_target}")
done < <(find "${fork_root}/source" -type d -name target -print0)
fork_archive_paths=(source UPSTREAM.toml SOURCE_DIFF_SHA256 patches)
if [[ -e "${installed_fork_root}" ]]; then
    [[ -d "${installed_fork_root}" ]] \
        || die "installed native source path is not a directory: ${installed_fork_root}"
    if ! tar -C "${fork_root}" "${fork_archive_args[@]}" -cf - "${fork_archive_paths[@]}" \
        | tar -C "${installed_fork_root}" --compare --file=-; then
        die "installed native source bundle differs from the canonical fork; preserved: ${installed_fork_root}"
    fi
else
    mkdir -p "${installed_fork_root}"
    tar -C "${fork_root}" "${fork_archive_args[@]}" -cf - "${fork_archive_paths[@]}" \
        | tar -C "${installed_fork_root}" --extract --file=-
fi

docs_root="${prefix}/share/rbt"
mkdir -p "${docs_root}/docs"
cp -f README.md LICENSE-APACHE LICENSE-MIT THIRD_PARTY_NOTICES.md "${docs_root}/"
cp -a docs/. "${docs_root}/docs/"
if [[ -n "${python_wheel}" ]]; then
    [[ -f "${python_wheel}" ]] || die "Python wheel is missing: ${python_wheel}"
    mkdir -p "${docs_root}/python"
    cp -f "${python_wheel}" "${docs_root}/python/"
fi

cpp_build_dir="${RBT_INSTALL_CPP_BUILD_DIR:-${repo_root}/target/v01-install-cpp-${build_type,,}}"
cmake -S "${repo_root}/sdk/cpp" -B "${cpp_build_dir}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${build_type}" \
    -DCMAKE_CXX_COMPILER="${CXX}" \
    -DCMAKE_PREFIX_PATH="${cpp_prefix}"
cmake --build "${cpp_build_dir}" --target rbt-cpp --parallel 2
cmake --install "${cpp_build_dir}" --prefix "${prefix}"

printf 'RBT_INSTALL_COMPLETE prefix=%s\n' "${prefix}"
printf '  binaries=%s/bin/{rbt,rbt-idl,composer}\n' "${prefix}"
printf '  rust_sdk=%s\n' "${rust_sdk}"
printf '  cpp_package=%s/lib/cmake/rbt/rbtConfig.cmake\n' "${prefix}"
if [[ -n "${python_wheel}" ]]; then
    printf '  python_wheel=%s/share/rbt/python/%s\n' "${prefix}" "$(basename "${python_wheel}")"
fi

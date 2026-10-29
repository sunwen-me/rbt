#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fork_root="${repo_root}/third_party/iceoryx2-rbt"
source_dir="${RBT_IOX2_FORK_SOURCE:-${fork_root}/source}"
build_dir="${RBT_IOX2_CPP_BUILD_DIR:-${RBT_IOX2_BUILD_DIR:-${repo_root}/target/iox2-rbt-0.10.0-cpp-build}}"
install_dir="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

read_upstream_value() {
    awk -F '"' -v key="$1" \
        '$1 ~ "^[[:space:]]*" key "[[:space:]]*=" { print $2; exit }' \
        "${fork_root}/UPSTREAM.toml"
}

printf 'Validating the canonical patched iceoryx2 source before CMake configuration\n'
RBT_IOX2_FORK_SOURCE="${source_dir}" \
    bash "${repo_root}/scripts/bootstrap_iceoryx2_rbt.sh" --source-only
source_dir="$(cd "${source_dir}" && pwd -P)"

command -v cmake >/dev/null 2>&1 || die 'cmake is required'
command -v sha256sum >/dev/null 2>&1 || die 'sha256sum is required'
[[ -x "${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}/bin/x86_64-conda-linux-gnu-gcc" ]] \
    || die "missing Phase4 Conda toolchain: ${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
export PATH="${toolchain_root}/bin${PATH:+:${PATH}}"
export CC="${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc"
export CXX="${toolchain_root}/bin/x86_64-conda-linux-gnu-g++"
export AR="${toolchain_root}/bin/x86_64-conda-linux-gnu-ar"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"

expected_build_source="${source_dir}"
if [[ -e "${build_dir}" ]]; then
    [[ -d "${build_dir}" ]] || die "C++ build path is not a directory: ${build_dir}"
    if [[ -f "${build_dir}/CMakeCache.txt" ]]; then
        cached_source="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${build_dir}/CMakeCache.txt")"
        cached_prefix="$(sed -n 's/^CMAKE_INSTALL_PREFIX:PATH=//p' "${build_dir}/CMakeCache.txt")"
        [[ "${cached_source}" == "${expected_build_source}" ]] \
            || die "C++ build directory points at another source: ${cached_source}; preserved"
        [[ "${cached_prefix}" == "${install_dir}" ]] \
            || die "C++ build directory has another install prefix: ${cached_prefix}; preserved"
    elif [[ -n "$(find "${build_dir}" -mindepth 1 -print -quit)" ]]; then
        die "C++ build directory is non-empty and has no CMake cache: ${build_dir}; preserved"
    fi
fi

if [[ -e "${install_dir}" ]]; then
    [[ -d "${install_dir}" ]] || die "install prefix is not a directory: ${install_dir}"
    if [[ -n "$(find "${install_dir}" -mindepth 1 -print -quit)" ]]; then
        bash "${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${install_dir}"
    fi
fi

fork_version="$(read_upstream_value fork_version)"
upstream_commit="$(read_upstream_value base_commit)"
[[ -n "${upstream_commit}" ]] || upstream_commit="$(read_upstream_value upstream_commit)"
source_diff_sha256="$(awk 'NR == 1 { print $1; exit }' "${fork_root}/SOURCE_DIFF_SHA256")"
patch_series_sha256="$(cd "${fork_root}/patches" && sha256sum series SHA256SUMS | sha256sum | awk '{print $1}')"
[[ "${fork_version}" == '0.10.0-rbt.2' ]] \
    || die "unexpected native fork version in UPSTREAM.toml: ${fork_version}"
[[ "${upstream_commit}" =~ ^[[:xdigit:]]{40}$ ]] \
    || die 'UPSTREAM.toml does not contain a valid upstream commit'
[[ "${source_diff_sha256}" =~ ^[[:xdigit:]]{64}$ ]] \
    || die 'SOURCE_DIFF_SHA256 does not contain a valid SHA-256 fingerprint'
[[ "${patch_series_sha256}" =~ ^[[:xdigit:]]{64}$ ]] \
    || die 'could not compute the canonical patch series SHA-256'

cmake_args=(
    -S "${source_dir}"
    -B "${build_dir}"
    -DCMAKE_BUILD_TYPE=Release
    -DBUILD_TESTING=OFF
    -DBUILD_EXAMPLES=OFF
    -DCMAKE_C_COMPILER="${CC}"
    -DCMAKE_CXX_COMPILER="${CXX}"
    -DCMAKE_INSTALL_PREFIX="${install_dir}"
)
if [[ ! -f "${build_dir}/CMakeCache.txt" ]] && command -v ninja >/dev/null 2>&1; then
    cmake_args+=(-G Ninja)
fi

cmake "${cmake_args[@]}"
cmake --build "${build_dir}" --parallel
cmake --install "${build_dir}"

provenance_dir="${install_dir}/share/rbt"
provenance_file="${provenance_dir}/iceoryx2-provenance.toml"
mkdir -p "${provenance_dir}"
if [[ -L "${provenance_file}" ]]; then
    die "refusing to replace provenance symlink: ${provenance_file}"
fi
temporary_provenance="$(mktemp "${provenance_dir}/.iceoryx2-provenance.XXXXXX")"
printf 'fork_version = "%s"\nupstream_commit = "%s"\nsource_diff_sha256 = "%s"\npatch_series_sha256 = "%s"\n' \
    "${fork_version}" "${upstream_commit}" "${source_diff_sha256}" "${patch_series_sha256}" \
    >"${temporary_provenance}"
if [[ -e "${provenance_file}" ]]; then
    cmp -s "${temporary_provenance}" "${provenance_file}" \
        || die "existing provenance differs from canonical source: ${provenance_file}; preserved"
else
    ln "${temporary_provenance}" "${provenance_file}" \
        || die "could not create provenance stamp without replacing existing data: ${provenance_file}"
fi
rm -- "${temporary_provenance}"

bash "${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${install_dir}"
printf 'iceoryx2 C++ package %s installed at %s\n' '0.10.0' "${install_dir}"

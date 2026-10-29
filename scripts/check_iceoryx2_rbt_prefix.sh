#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fork_root="${repo_root}/third_party/iceoryx2-rbt"

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

read_toml_value() {
    local file="$1"
    local key="$2"
    awk -F '"' -v key="${key}" \
        '$1 ~ "^[[:space:]]*" key "[[:space:]]*=" { print $2; exit }' "${file}"
}

if (($# > 1)); then
    die 'usage: scripts/check_iceoryx2_rbt_prefix.sh [PREFIX]'
fi
prefix="${1:-${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}}"
[[ -n "${prefix}" && -d "${prefix}" ]] || die "native prefix is missing: ${prefix}"
prefix="$(cd "${prefix}" && pwd -P)"

upstream_file="${fork_root}/UPSTREAM.toml"
diff_file="${fork_root}/SOURCE_DIFF_SHA256"
patch_dir="${fork_root}/patches"
[[ -f "${upstream_file}" && -f "${diff_file}" ]] \
    || die 'canonical iceoryx2 fork metadata is incomplete'
[[ -f "${patch_dir}/series" && -f "${patch_dir}/SHA256SUMS" ]] \
    || die 'canonical iceoryx2 patch series metadata is incomplete'
(cd "${patch_dir}" && sha256sum --check --status SHA256SUMS) \
    || die 'canonical iceoryx2 patch checksum validation failed'

fork_version="$(read_toml_value "${upstream_file}" fork_version)"
upstream_commit="$(read_toml_value "${upstream_file}" base_commit)"
[[ -n "${upstream_commit}" ]] \
    || upstream_commit="$(read_toml_value "${upstream_file}" upstream_commit)"
source_diff_sha256="$(awk 'NR == 1 { print $1; exit }' "${diff_file}")"
# Bind both the ordered series and its checked-in patch content checksums.
patch_series_sha256="$(cd "${patch_dir}" && sha256sum series SHA256SUMS | sha256sum | awk '{print $1}')"

[[ "${fork_version}" == '0.10.0-rbt.2' ]] \
    || die "unsupported native fork version in UPSTREAM.toml: ${fork_version}"
[[ "${upstream_commit}" =~ ^[[:xdigit:]]{40}$ ]] \
    || die 'UPSTREAM.toml does not contain a valid upstream commit'
[[ "${source_diff_sha256}" =~ ^[[:xdigit:]]{64}$ ]] \
    || die 'SOURCE_DIFF_SHA256 does not contain a valid SHA-256 fingerprint'
[[ "${patch_series_sha256}" =~ ^[[:xdigit:]]{64}$ ]] \
    || die 'could not compute the canonical patch series SHA-256'

version_file="$(find "${prefix}" -type f \
    -path '*/cmake/iceoryx2-cxx/iceoryx2-cxxConfigVersion.cmake' -print -quit)"
[[ -n "${version_file}" ]] || die "iceoryx2-cxx CMake package is missing under ${prefix}"
config_file="$(dirname "${version_file}")/iceoryx2-cxxConfig.cmake"
[[ -f "${config_file}" ]] || die "iceoryx2-cxx package config is missing: ${config_file}"
package_version="$(awk -F '"' \
    '$1 ~ /^[[:space:]]*set\(PACKAGE_VERSION[[:space:]]*$/ { print $2; exit }' \
    "${version_file}")"
[[ "${package_version}" == '0.10.0' ]] \
    || die "iceoryx2-cxx package version must be exactly 0.10.0 (found ${package_version:-unknown})"

stamp="${prefix}/share/rbt/iceoryx2-provenance.toml"
[[ -f "${stamp}" && ! -L "${stamp}" ]] \
    || die "canonical iceoryx2 provenance stamp is missing: ${stamp}"
installed_fork_version="$(read_toml_value "${stamp}" fork_version)"
installed_upstream_commit="$(read_toml_value "${stamp}" upstream_commit)"
installed_source_diff_sha256="$(read_toml_value "${stamp}" source_diff_sha256)"
installed_patch_series_sha256="$(read_toml_value "${stamp}" patch_series_sha256)"
[[ "${installed_fork_version}" == "${fork_version}" ]] \
    || die "native prefix fork version mismatch: expected ${fork_version}, found ${installed_fork_version:-missing}"
[[ "${installed_upstream_commit}" == "${upstream_commit}" ]] \
    || die "native prefix upstream commit mismatch: expected ${upstream_commit}, found ${installed_upstream_commit:-missing}"
[[ "${installed_source_diff_sha256}" == "${source_diff_sha256}" ]] \
    || die "native prefix source fingerprint mismatch: expected ${source_diff_sha256}, found ${installed_source_diff_sha256:-missing}"
[[ "${installed_patch_series_sha256}" == "${patch_series_sha256}" ]] \
    || die "native prefix patch series mismatch: expected ${patch_series_sha256}, found ${installed_patch_series_sha256:-missing}"

printf 'ICEORYX2_RBT_PREFIX PASS version=%s fork=%s prefix=%s\n' \
    "${package_version}" "${fork_version}" "${prefix}"

#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fork_root="${repo_root}/third_party/iceoryx2-rbt"
source_dir="${RBT_IOX2_FORK_SOURCE:-${fork_root}/source}"
build_root="${RBT_IOX2_FORK_BUILD:-${repo_root}/target/iceoryx2-rbt-build}"
base_commit=135d09dd8b29f321f1725920d434864c4e512378
mode="${1:---source-only}"

die() { printf 'ERROR: %s\n' "$*" >&2; exit 1; }
case "${mode}" in
    --source-only|--build|--verify) ;;
    --help|-h)
        printf '%s\n' \
            'usage: scripts/bootstrap_iceoryx2_rbt.sh [--source-only|--build|--verify]' \
            '--source-only  Prepare the pinned upstream commit plus the audited patch series.' \
            '--build        Prepare and build the fork tunnel and service inspection CLI.' \
            '--verify       Verify the patch series in a fresh temporary checkout.'
        exit 0
        ;;
    *) die "unknown mode: ${mode}" ;;
esac
(($# <= 1)) || die 'expected at most one mode'

for executable in git sha256sum awk cmp mktemp; do
    command -v "${executable}" >/dev/null || die "missing command: ${executable}"
done
[[ -f "${fork_root}/patches/series" ]] || die 'fork patch series missing'
[[ -f "${fork_root}/patches/SHA256SUMS" ]] || die 'fork patch checksums missing'
[[ -f "${fork_root}/SOURCE_DIFF_SHA256" ]] || die 'fork source fingerprint missing'
(cd "${fork_root}/patches" && sha256sum --check --status SHA256SUMS) \
    || die 'fork patch checksum mismatch'

declare -a patches=()
while IFS= read -r patch_name || [[ -n "${patch_name}" ]]; do
    [[ -z "${patch_name}" || "${patch_name}" == \#* ]] && continue
    [[ "${patch_name}" =~ ^[0-9]{4}-[a-z0-9-]+\.patch$ ]] \
        || die "invalid patch name in series: ${patch_name}"
    [[ -f "${fork_root}/patches/${patch_name}" ]] || die "missing patch: ${patch_name}"
    patches+=("${fork_root}/patches/${patch_name}")
done < "${fork_root}/patches/series"
((${#patches[@]} > 0)) || die 'empty fork patch series'

pristine_source="${RBT_IOX2_FORK_UPSTREAM:-${repo_root}/target/iceoryx2-v0.10.0}"
clone_origin=https://github.com/eclipse-iceoryx/iceoryx2.git
if [[ -e "${pristine_source}/.git" ]] \
    && [[ "$(git -C "${pristine_source}" rev-parse HEAD)" == "${base_commit}" ]]; then
    clone_origin="${pristine_source}"
fi

temporary_root=""
cleanup() {
    if [[ -n "${temporary_root}" && -d "${temporary_root}" ]]; then
        # Only the exact mktemp directory created by this invocation is removed.
        rm -rf -- "${temporary_root}"
    fi
}
trap cleanup EXIT
if [[ "${mode}" == --verify ]]; then
    temporary_root="$(mktemp -d "${TMPDIR:-/tmp}/rbt-iox2-verify.XXXXXXXX")"
    source_dir="${temporary_root}/source"
fi

if [[ ! -e "${source_dir}" ]]; then
    mkdir -p "$(dirname "${source_dir}")"
    git clone --quiet --no-hardlinks --no-checkout "${clone_origin}" "${source_dir}"
    git -C "${source_dir}" switch --quiet --create codex/iceoryx2-rbt-0.10.0 "${base_commit}"
    for patch_path in "${patches[@]}"; do
        git -C "${source_dir}" apply --index --whitespace=error "${patch_path}"
    done
fi

if [[ ! -e "${source_dir}/.git" ]]; then
    command -v python3 >/dev/null || die 'Python 3 is required to verify exported source'
    python3 "${repo_root}/scripts/check_iceoryx2_rbt_export.py" \
        "${source_dir}" "${fork_root}/EXPORT_MANIFEST.json" \
        || die 'exported native source differs from the audited source; preserved'
else
    [[ "$(git -C "${source_dir}" rev-parse HEAD)" == "${base_commit}" ]] \
        || die 'fork HEAD differs from the pinned upstream commit; source was preserved'
    [[ -z "$(git -C "${source_dir}" ls-files --others --exclude-standard)" ]] \
        || die 'fork has untracked source files; source was preserved'
    expected_diff="$(awk 'NR == 1 { print $1 }' "${fork_root}/SOURCE_DIFF_SHA256")"
    actual_diff="$(git -C "${source_dir}" diff --binary HEAD | sha256sum | awk '{print $1}')"
    [[ "${actual_diff}" == "${expected_diff}" ]] \
        || die 'fork differs from the audited patch series; local edits were preserved'
    printf 'ICEORYX2_RBT_SOURCE PASS base=%s patches=%s diff=%s source=%s\n' \
        "${base_commit}" "${#patches[@]}" "${actual_diff}" "${source_dir}"
fi

if [[ "${mode}" == --build ]]; then
    toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
    [[ -x "${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc" ]] \
        || die "missing Phase4 toolchain: ${toolchain_root}"
    export PATH="${toolchain_root}/bin:${PATH}"
    export CC="${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc"
    export CXX="${toolchain_root}/bin/x86_64-conda-linux-gnu-g++"
    export AR="${toolchain_root}/bin/x86_64-conda-linux-gnu-ar"
    export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
    cargo build --locked --manifest-path "${source_dir}/integrations/zenoh/Cargo.toml" \
        --target-dir "${build_root}/zenoh" --bin iox2-link-tunnel-zenoh
    cargo build --locked --manifest-path "${source_dir}/Cargo.toml" \
        --target-dir "${build_root}/core" -p iceoryx2-cli --bin iox2-service
    printf 'ICEORYX2_RBT_BUILD PASS tunnel=%s service=%s\n' \
        "${build_root}/zenoh/debug/iox2-link-tunnel-zenoh" \
        "${build_root}/core/debug/iox2-service"
fi

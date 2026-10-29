#!/usr/bin/env bash
# Verifies that third_party/iceoryx2/iceoryx2 is exactly the crates.io
# iceoryx2 0.9.3 crate plus patches/0001-*.patch, and that the C/C++ patch set
# still applies to the pinned upstream commit.
set -euo pipefail

readonly CRATE_SHA256="e226c6041866b416e8c384d69a54a8578cb26619725253351e3e17ea5f32627f"
readonly IOX2_COMMIT="6ef7c55176a8487ae7ca53c2e0649d5df9a1b3b4"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "${here}/../.." && pwd)"
crate="${RBT_IOX2_CRATE:-$(ls "${CARGO_HOME:-$HOME/.cargo}"/registry/cache/*/iceoryx2-0.9.3.crate 2>/dev/null | head -n1)}"
[[ -f "${crate}" ]] || { echo "iceoryx2-0.9.3.crate not in cargo cache; run cargo fetch" >&2; exit 1; }
[[ "$(sha256sum "${crate}" | cut -d' ' -f1)" == "${CRATE_SHA256}" ]] \
    || { echo "iceoryx2-0.9.3.crate checksum mismatch" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "${work}"' EXIT
tar -xzf "${crate}" -C "${work}"
mv "${work}/iceoryx2-0.9.3" "${work}/iceoryx2"
(cd "${work}" && patch -p1 -s < "${here}/patches/0001-iceoryx2-stale-response-correlation.patch")
diff -r "${work}/iceoryx2" "${here}/iceoryx2" \
    || { echo "vendored iceoryx2 differs from crate 0.9.3 + patches" >&2; exit 1; }

source_dir="${RBT_IOX2_SOURCE_DIR:-${repo_root}/target/iceoryx2-v0.9.3}"
if [[ -d "${source_dir}/.git" ]]; then
    git clone --quiet --shared --no-checkout "${source_dir}" "${work}/src"
    git -C "${work}/src" checkout --quiet "${IOX2_COMMIT}"
    for p in "${here}"/patches/*.patch; do
        git -C "${work}/src" apply --check "${p}"
    done
fi

echo "iceoryx2 vendor OK: crate 0.9.3 (${CRATE_SHA256:0:12}) + $(ls "${here}"/patches/*.patch | wc -l) patches"

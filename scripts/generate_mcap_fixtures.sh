#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo_root"

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
if [[ -x "${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc" ]]; then
    export PATH="${toolchain_root}/bin${PATH:+:${PATH}}"
    export CC="${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc"
    export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
    export LD_LIBRARY_PATH="${toolchain_root}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
fi

cargo run --offline --quiet -p rbt-mcap --bin rbt-mcap-fixtures -- \
    generate tests/fixtures/mcap

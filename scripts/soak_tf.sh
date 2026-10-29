#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="${repo_root}/target/phase4-toolchain/bin:${PATH}"
export CC="${repo_root}/target/phase4-toolchain/bin/x86_64-conda-linux-gnu-gcc"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
export LD_LIBRARY_PATH="${repo_root}/target/phase4-toolchain/lib:${LD_LIBRARY_PATH:-}"

cd "${repo_root}"
python3 scripts/tf_process_probe.py "$@"

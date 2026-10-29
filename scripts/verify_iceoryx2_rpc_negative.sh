#!/usr/bin/env bash
set -euo pipefail
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
base=135d09dd8b29f321f1725920d434864c4e512378
pristine="${repo_root}/target/iceoryx2-v0.10.0"
if [[ ! -e "${pristine}" ]]; then
    "${repo_root}/scripts/bootstrap_iceoryx2_rbt.sh" --source-only
    git clone --quiet --no-hardlinks --no-checkout \
        "${repo_root}/third_party/iceoryx2-rbt/source" "${pristine}"
    git -C "${pristine}" switch --quiet --detach "${base}"
fi
[[ "$(git -C "${pristine}" rev-parse HEAD)" == "${base}" ]]
[[ -z "$(git -C "${pristine}" status --porcelain)" ]]
log_dir="${repo_root}/target/iceoryx2-unified-logs"
mkdir -p "${log_dir}"
log="${log_dir}/rpc-negative-control.log"
status=0
cargo test --manifest-path "${repo_root}/experiments/iceoryx2-rbt/negative-control/Cargo.toml" \
    --locked --offline --target-dir "${repo_root}/target/iceoryx2-unified-build/negative" \
    --test rpc_native_identity >"${log}" 2>&1 || status=$?
[[ "${status}" == 101 ]] || { tail -60 "${log}"; exit 1; }
rg -Fq 'test result: FAILED. 0 passed; 2 failed;' "${log}"
rg -Fq 'assertion failed: !old_active.is_connected()' "${log}"
printf 'RPC_NEGATIVE_CONTROL PASS upstream=%s expected_failures=2 evidence=%s\n' "${base}" "${log}"

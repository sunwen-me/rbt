#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
composer_port="${RBT_RPC_COMPOSER_PORT:-17447}"
composer_addr="127.0.0.1:${composer_port}"
toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
demo_delay_ms="${RBT_RPC_DEMO_DELAY_MS:-}"
run_dir="$(mktemp -d /tmp/rbt-rpc.XXXXXX)"
children=()

if [[ ! "${composer_port}" =~ ^[0-9]+$ ]]; then
    printf 'RBT_RPC_COMPOSER_PORT must be numeric\n' >&2
    exit 2
fi
if [[ -n "${demo_delay_ms}" ]]; then
    [[ "${demo_delay_ms}" =~ ^[0-9]+$ ]] || {
        printf 'RBT_RPC_DEMO_DELAY_MS must be numeric in 0..=5000\n' >&2
        exit 2
    }
    if (( ${#demo_delay_ms} > 4 )) || (( 10#${demo_delay_ms} > 5000 )); then
        printf 'RBT_RPC_DEMO_DELAY_MS must be numeric in 0..=5000\n' >&2
        exit 2
    fi
fi

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    for pid in "${children[@]:-}"; do
        if kill -0 "${pid}" 2>/dev/null; then
            kill -TERM "${pid}" 2>/dev/null || true
        fi
    done
    for pid in "${children[@]:-}"; do
        wait "${pid}" 2>/dev/null || true
    done
    printf 'RPC demo logs: %s\n' "${run_dir}"
    exit "${status}"
}
trap cleanup EXIT INT TERM

# The repository's CI/runtime images may not provide a system C linker. Use
# the pinned workspace toolchain when it is present, while still allowing the
# script to run with a normal host toolchain elsewhere.
if [[ -x "${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc" \
    && -x "${toolchain_root}/bin/x86_64-conda-linux-gnu-g++" ]]; then
    export PATH="${toolchain_root}/bin${PATH:+:${PATH}}"
    export CC="${toolchain_root}/bin/x86_64-conda-linux-gnu-gcc"
    export CXX="${toolchain_root}/bin/x86_64-conda-linux-gnu-g++"
    export AR="${toolchain_root}/bin/x86_64-conda-linux-gnu-ar"
    export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
    export LD_LIBRARY_PATH="${toolchain_root}/lib${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
    export LIBRARY_PATH="${toolchain_root}/lib${LIBRARY_PATH:+:${LIBRARY_PATH}}"
fi

if [[ "${RBT_RPC_SKIP_BUILD:-0}" == 1 ]]; then
    [[ -x "${repo_root}/target/debug/composer" ]] \
        && [[ -x "${repo_root}/target/debug/rpc-onboarding" ]] \
        || {
            printf 'RBT_RPC_SKIP_BUILD=1 requires target/debug/composer and target/debug/rpc-onboarding\n' >&2
            exit 2
        }
else
    printf 'Building Composer and the fixed RPC onboarding example...\n'
    (cd "${repo_root}" && cargo build -p composer -p rpc-onboarding)
fi

start() {
    local name="$1"
    shift
    printf '+'
    printf ' %q' "$@"
    printf '  > %s/%s.log\n' "${run_dir}" "${name}"
    env RBT_COMPOSER_ADDR="${composer_addr}" "$@" >"${run_dir}/${name}.log" 2>&1 &
    children+=("$!")
}

start composer "${repo_root}/target/debug/composer"
composer_ready=0
for _ in $(seq 1 100); do
    if (exec 3<>"/dev/tcp/127.0.0.1/${composer_port}") 2>/dev/null; then
        composer_ready=1
        exec 3>&-
        break
    fi
    sleep 0.1
done
if ((composer_ready == 0)); then
    printf 'Composer did not listen on %s; inspect %s/composer.log\n' \
        "${composer_addr}" "${run_dir}" >&2
    exit 1
fi

provider_env=()
if [[ -n "${demo_delay_ms}" ]]; then
    provider_env=(env "RBT_RPC_DEMO_DELAY_MS=${demo_delay_ms}")
fi
start provider "${provider_env[@]}" "${repo_root}/target/debug/rpc-onboarding" provider
start client "${repo_root}/target/debug/rpc-onboarding" client

printf 'RPC demo is running at %s; press Ctrl-C to stop all three processes.\n' "${composer_addr}"
set +e
wait -n "${children[@]}"
status=$?
set -e
exit "${status}"

#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$repo_root"

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
usage: scripts/verify_pressure_tuning.sh [--quick|--full]

--quick  Format, focused Rust pressure/resource tests, C++ CTest, Python
         pressure/resource tests, layout/hash invariants, and unsafe=73 audit.
--full   Quick plus existing cross-language, WaitSet/MCAP, sanitizer,
         fresh-wheel/external-consumer, activation-cycle, 300-second soak,
         and five-size pressure benchmark gates.

The verifier delegates substantive work to existing repository scripts.
Use RBT_PRESSURE_TUNING_LOG_DIR to retain this run's command logs.
RBT_PRESSURE_TUNING_DEDUP_LONG_SOAKS=1  in --full, use --quick for the
                                       MCAP, Python component, and Python
                                       asyncio inherited lanes; keep the
                                       standalone pressure soak and matrix.
EOF
}

mode=quick
if (($# > 1)); then
    usage >&2
    exit 2
fi
if (($# == 1)); then
    case "$1" in
        --quick) mode=quick ;;
        --full) mode=full ;;
        --help|-h) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
fi

dedup_long_soaks="${RBT_PRESSURE_TUNING_DEDUP_LONG_SOAKS:-0}"
[[ "${dedup_long_soaks}" =~ ^[01]$ ]] \
    || die "RBT_PRESSURE_TUNING_DEDUP_LONG_SOAKS must be 0 or 1"

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-$repo_root/target/phase4-toolchain}"
toolchain_bin="$toolchain_root/bin"
cpp_prefix="${RBT_IOX2_PREFIX:-$repo_root/target/iox2-rbt-0.10.0-cpp-install}"
"${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${cpp_prefix}"
if [[ -d "$toolchain_bin" ]]; then
    export PATH="$toolchain_bin:$PATH"
fi

for command_name in cargo rustc cmake ctest ninja rg awk find wc tr date; do
    command -v "$command_name" >/dev/null 2>&1 \
        || die "required command not found: $command_name"
done

log_root="${RBT_PRESSURE_TUNING_LOG_DIR:-$repo_root/target/pressure-tuning-verify-logs}"
run_id="pressure-tuning-$mode-$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="$log_root/$run_id"
mkdir -p "$run_dir"

if [[ -d "$toolchain_bin" ]]; then
    if [[ -x "$toolchain_bin/x86_64-conda-linux-gnu-gcc" ]]; then
        export CC="$toolchain_bin/x86_64-conda-linux-gnu-gcc"
        export CXX="$toolchain_bin/x86_64-conda-linux-gnu-g++"
        export AR="$toolchain_bin/x86_64-conda-linux-gnu-ar"
        export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="$CC"
        export LD_LIBRARY_PATH="$toolchain_root/lib:$cpp_prefix/lib:${LD_LIBRARY_PATH:-}"
        export LIBRARY_PATH="$toolchain_root/lib:$cpp_prefix/lib:${LIBRARY_PATH:-}"
    fi
fi
if [[ -n "${CMAKE_PREFIX_PATH:-}" ]]; then
    export CMAKE_PREFIX_PATH="$cpp_prefix:$CMAKE_PREFIX_PATH"
else
    export CMAKE_PREFIX_PATH="$cpp_prefix"
fi

run_logged() {
    local label="$1"
    shift
    local log_file="$run_dir/$label.log"
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    set +e
    "$@" >"$log_file" 2>&1
    local status=$?
    set -e
    cat "$log_file"
    ((status == 0)) || die "$label failed with exit $status; see $log_file"
}

unsafe_audit() {
    python3 "$repo_root/scripts/audit_unsafe.py" --label RBT_PRESSURE_TUNING_UNSAFE 2>&1 \
        | tee "$run_dir/unsafe-audit.log" \
        || die "production unsafe audit failed; see $run_dir/unsafe-audit.log"
}

layout_hash_gate() {
    run_logged layout-hash-rust-tests cargo test -p rbt-types --lib
    run_logged layout-hash-iox2-tests cargo test -p rbt-iox2 --test phase3_data_plane -- --test-threads=1
    rg -Fq '943fa4008cf0f55f89a281b051c69a6f' crates/rbt-types/src/lib.rs \
        || die "Image layout hash assertion is missing"
    rg -Fq 'd9da2bb29bb2cf217c40f1a56d1fb755' crates/rbt-types/src/lib.rs \
        || die "PointCloud layout hash assertion is missing"
    rg -Fq '9381cf15842a48ac0964111f0f45ef72' crates/rbt-types/src/lib.rs \
        || die "ControlCommand layout hash assertion is missing"
    printf 'PASS: payload layout hashes remain frozen\n'
}

focused_rust() {
    run_logged rust-format cargo fmt --all -- --check
    run_logged rust-check cargo check --workspace --all-targets
    run_logged rust-types-tests cargo test -p rbt-types --lib
    run_logged rust-core-tests cargo test -p rbt-core --lib
    run_logged rust-iox2-pressure cargo test -p rbt-iox2 --test pressure_semantics -- --test-threads=1
    run_logged rust-iox2-fault cargo test -p rbt-iox2 --test pressure_fault -- --test-threads=1
    run_logged rust-compose-pressure cargo test -p rbt-compose --lib
    run_logged rust-cli-pressure cargo test -p rbt-cli --lib
}

focused_cpp() {
    [[ -f "$cpp_prefix/lib/cmake/iceoryx2-cxx/iceoryx2-cxxConfig.cmake" ]] \
        || die "iceoryx2 C++ package config is missing under $cpp_prefix"
    if [[ -z "${CXX:-}" ]]; then
        export CXX=c++
    fi
    local build_dir="${RBT_PRESSURE_TUNING_CPP_BUILD_DIR:-$repo_root/target/pressure-tuning-cpp}"
    run_logged cpp-configure cmake -S "$repo_root/sdk/cpp" -B "$build_dir" \
        -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER="$CXX" \
        -DCMAKE_PREFIX_PATH="$cpp_prefix"
    run_logged cpp-build cmake --build "$build_dir" --parallel
    run_logged cpp-ctest ctest --test-dir "$build_dir" --output-on-failure
}

focused_python() {
    run_logged python-pressure-resource ./scripts/verify_python.sh --quick
}

printf 'RBT_PRESSURE_TUNING_VERIFY mode=%s logs=%s toolchain=%s cpp_prefix=%s\n' \
    "$mode" "$run_dir" "$toolchain_root" "$cpp_prefix"

if [[ "$mode" == full && "$dedup_long_soaks" == 1 ]]; then
    printf 'RBT_PRESSURE_TUNING_DEDUP_LONG_SOAKS=1 composition: v01=full waitset=full mcap=quick python-publisher=full python-component=quick python-asyncio=quick mixed-pressure=kept pressure-soak-300s=kept benchmark-matrix=kept\n'
else
    printf 'RBT_PRESSURE_TUNING_DEDUP_LONG_SOAKS=%s composition: standalone inherited lane modes unchanged\n' \
        "$dedup_long_soaks"
fi

focused_rust
focused_cpp
focused_python
layout_hash_gate
unsafe_audit

if [[ "$mode" == full ]]; then
    inherited_mcap_mode=full
    inherited_python_component_mode=full
    inherited_python_asyncio_mode=full
    if [[ "$dedup_long_soaks" == 1 ]]; then
        inherited_mcap_mode=quick
        inherited_python_component_mode=quick
        inherited_python_asyncio_mode=quick
    fi
    run_logged inherited-v01 ./scripts/verify_v01.sh --full
    run_logged inherited-waitset env RBT_WAITSET_SKIP_V01=1 \
        ./scripts/verify_waitset.sh --full
    run_logged inherited-mcap env RBT_MCAP_SKIP_INHERITED=1 \
        ./scripts/verify_mcap.sh "--${inherited_mcap_mode}"
    run_logged inherited-python-publisher env RBT_PYTHON_SKIP_REGRESSION=1 \
        ./scripts/verify_python_publisher.sh --full
    run_logged inherited-python-component env \
        RBT_PYTHON_COMPONENT_SKIP_INHERITED=1 \
        ./scripts/verify_python_component.sh "--${inherited_python_component_mode}"
    run_logged inherited-python-asyncio env \
        RBT_PYTHON_ASYNCIO_SKIP_INHERITED=1 \
        RBT_PYTHON_ASYNCIO_TOOL_PYTHON="$repo_root/target/python-verify-venv/bin/python" \
        ./scripts/verify_python_asyncio.sh "--${inherited_python_asyncio_mode}"
    run_logged mixed-language-pressure ./scripts/test_mixed_pressure.sh
    run_logged pressure-live-binaries cargo build --release \
        -p image-publisher -p image-consumer
    run_logged pressure-soak-300s ./scripts/soak_pressure.sh
    run_logged pressure-benchmark-matrix ./scripts/benchmark_pressure.sh
fi

if command -v shellcheck >/dev/null 2>&1; then
    run_logged shellcheck \
        shellcheck scripts/verify_pressure_tuning.sh scripts/benchmark_pressure.sh
else
    printf 'SKIP: shellcheck is not installed\n' | tee "$run_dir/shellcheck.log"
fi

printf 'PASS: pressure tuning %s verification completed; logs=%s\n' \
    "$mode" "$run_dir"

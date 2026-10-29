#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${repo_root}"

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
usage: scripts/verify_v01.sh [--quick|--full]

--quick  Rust quality gates, deterministic IDL, CLI smoke, C++ Debug/Release CTest
--full   quick plus Phase4 interop, mixed runtime, install, soak, sanitizer, benchmark (default)

Environment overrides:
  RBT_PHASE4_TOOLCHAIN, RBT_IOX2_PREFIX, RBT_V01_LOG_DIR
  RBT_V01_SKIP_SANITIZER=1    explicitly omit the ASan/UBSan gate
  RBT_V01_SOAK_FULL=1         run the soak's default/configured duration, not --smoke
EOF
}

mode=full
if (($# > 1)); then
    usage >&2
    exit 2
fi
if (($# == 1)); then
    case "$1" in
        --quick) mode=quick ;;
        --full) mode=full ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            usage >&2
            exit 2
            ;;
    esac
fi

for command_name in cargo rustc cmake ctest diff cmp; do
    command -v "${command_name}" >/dev/null 2>&1 || die "required command not found: ${command_name}"
done

toolchain_root="${RBT_PHASE4_TOOLCHAIN:-${repo_root}/target/phase4-toolchain}"
cpp_prefix="${RBT_IOX2_PREFIX:-${repo_root}/target/iox2-rbt-0.10.0-cpp-install}"
"${repo_root}/scripts/check_iceoryx2_rbt_prefix.sh" "${cpp_prefix}"
[[ -f "${cpp_prefix}/lib/cmake/iceoryx2-cxx/iceoryx2-cxxConfig.cmake" ]] \
    || die "maintained iceoryx2 0.10.0-rbt.2 C++ package is missing; run scripts/bootstrap_iceoryx2_cpp.sh"

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

log_root="${RBT_V01_LOG_DIR:-${repo_root}/target/v01-logs}"
mkdir -p "${log_root}"
run_id="verify-${mode}-$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="${log_root}/${run_id}"
mkdir -p "${run_dir}"
tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/rbt-verify-v01.XXXXXX")"

cleanup() {
    rm -rf -- "${tmp_root}"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

run_logged() {
    local label="$1"
    shift
    local log_file="${run_dir}/${label}.log"
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    set +e
    "$@" >"${log_file}" 2>&1
    local status=$?
    set -e
    cat "${log_file}"
    ((status == 0)) || die "${label} failed with exit ${status}; see ${log_file}"
}

compare_tree() {
    local label="$1"
    local left="$2"
    local right="$3"
    local diff_file="${run_dir}/${label}.diff"
    if ! diff -ru -- "${left}" "${right}" >"${diff_file}" 2>&1; then
        cat "${diff_file}" >&2
        die "${label} differs; see ${diff_file}"
    fi
    while IFS= read -r -d '' file; do
        local relative="${file#"${left}"/}"
        cmp -s -- "${file}" "${right}/${relative}" \
            || die "${label} byte comparison failed for ${relative}"
    done < <(find "${left}" -type f -print0)
}

generate_set() {
    local destination="$1"
    mkdir -p "${destination}/rust" "${destination}/cpp" "${destination}/descriptors"
    cargo run --quiet -p rbt-idl -- generate \
        types/sensor.rbt types/control.rbt types/demo.rbt types/tf.rbt \
        --rust "${destination}/rust" \
        --cpp "${destination}/cpp" \
        --descriptor "${destination}/descriptors"
}

generate_mismatch() {
    local destination="$1"
    mkdir -p "${destination}/rust" "${destination}/cpp" "${destination}/descriptors"
    cargo run --quiet -p rbt-idl -- generate \
        types/fixtures/image_mismatch.rbt \
        --rust "${destination}/rust" \
        --cpp "${destination}/cpp" \
        --descriptor "${destination}/descriptors"
}

printf 'RBT_VERIFY_V01 mode=%s logs=%s\n' "${mode}" "${run_dir}"
printf 'RBT_VERIFY_ENV rust=%s cmake=%s cxx=%s iox2=%s\n' \
    "$(rustc --version)" "$(cmake --version | sed -n '1p')" \
    "$(${CXX} --version | sed -n '1p')" "${cpp_prefix}"

run_logged cargo-fmt cargo fmt --all -- --check
run_logged cargo-test cargo test --workspace
run_logged cargo-clippy cargo clippy --workspace --all-targets --all-features -- -D warnings
run_logged cli-help cargo run --quiet -p rbt-cli -- --help

generation_one="${tmp_root}/generation-one"
generation_two="${tmp_root}/generation-two"
mismatch_one="${tmp_root}/mismatch-one"
mismatch_two="${tmp_root}/mismatch-two"
run_logged idl-generate-one generate_set "${generation_one}"
run_logged idl-generate-two generate_set "${generation_two}"
run_logged idl-mismatch-one generate_mismatch "${mismatch_one}"
run_logged idl-mismatch-two generate_mismatch "${mismatch_two}"
compare_tree generated-rust-twice "${generation_one}/rust" "${generation_two}/rust"
compare_tree generated-cpp-twice "${generation_one}/cpp" "${generation_two}/cpp"
compare_tree generated-descriptors-twice "${generation_one}/descriptors" "${generation_two}/descriptors"
compare_tree generated-rust-checked-in "${generation_one}/rust" "${repo_root}/generated/rust"
compare_tree generated-cpp-checked-in "${generation_one}/cpp" "${repo_root}/generated/cpp"
compare_tree generated-descriptors-checked-in "${generation_one}/descriptors" "${repo_root}/generated/descriptors"
compare_tree mismatch-rust-twice "${mismatch_one}/rust" "${mismatch_two}/rust"
compare_tree mismatch-cpp-twice "${mismatch_one}/cpp" "${mismatch_two}/cpp"
compare_tree mismatch-descriptors-twice "${mismatch_one}/descriptors" "${mismatch_two}/descriptors"
compare_tree mismatch-rust-checked-in "${mismatch_one}/rust" "${repo_root}/generated/mismatch/rust"
compare_tree mismatch-cpp-checked-in "${mismatch_one}/cpp" "${repo_root}/generated/mismatch/cpp"
compare_tree mismatch-descriptors-checked-in "${mismatch_one}/descriptors" "${repo_root}/generated/mismatch/descriptors"
printf 'PASS: deterministic IDL outputs match the checked-in trees\n'

for build_type in Debug Release; do
    build_key="$(printf '%s' "${build_type}" | tr '[:upper:]' '[:lower:]')"
    build_dir="${repo_root}/target/v01-verify-cpp-${build_key}"
    run_logged "cpp-${build_key}-configure" cmake \
        -S "${repo_root}/sdk/cpp" \
        -B "${build_dir}" \
        -G Ninja \
        -DCMAKE_BUILD_TYPE="${build_type}" \
        -DCMAKE_CXX_COMPILER="${CXX}" \
        -DCMAKE_PREFIX_PATH="${cpp_prefix}"
    run_logged "cpp-${build_key}-build" cmake --build "${build_dir}" --parallel 2
    run_logged "cpp-${build_key}-ctest" ctest --test-dir "${build_dir}" --output-on-failure
done

if [[ "${mode}" == full ]]; then
    run_logged phase4-regression ./scripts/verify_phase4.sh
    run_logged mixed-runtime ./scripts/test_mixed_runtime.sh
    run_logged installed-consumers ./scripts/test_install.sh
    if [[ "${RBT_V01_SOAK_FULL:-0}" == 1 ]]; then
        run_logged soak ./scripts/soak_v01.sh
    else
        run_logged soak ./scripts/soak_v01.sh --smoke
    fi

    if [[ "${RBT_V01_SKIP_SANITIZER:-0}" == 1 ]]; then
        printf 'SKIP: C++ ASan/UBSan explicitly disabled by RBT_V01_SKIP_SANITIZER=1\n'
    else
        sanitize_dir="${repo_root}/target/v01-verify-cpp-sanitize"
        run_logged sanitizer-configure cmake \
            -S "${repo_root}/sdk/cpp" \
            -B "${sanitize_dir}" \
            -G Ninja \
            -DCMAKE_BUILD_TYPE=Debug \
            -DCMAKE_CXX_COMPILER="${CXX}" \
            -DCMAKE_PREFIX_PATH="${cpp_prefix}" \
            -DCMAKE_CXX_FLAGS=-fsanitize=address,undefined\ -fno-omit-frame-pointer \
            -DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined
        run_logged sanitizer-build cmake --build "${sanitize_dir}" --parallel 2
        run_logged sanitizer-ctest env \
            ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
            UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            ctest --test-dir "${sanitize_dir}" --output-on-failure
    fi

    run_logged benchmark-smoke ./scripts/benchmark_v01.sh --smoke
fi

printf 'PASS: rbt v0.1 %s verification completed; logs=%s\n' "${mode}" "${run_dir}"

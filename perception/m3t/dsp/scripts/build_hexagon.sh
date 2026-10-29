#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: build_hexagon.sh --output-dir DIR [--phase compile|link|all]

Environment:
  QAIC          QAIC executable (default: qaic)
  QAIC_REGENERATE  Run QAIC instead of using pinned generated sources (0/1)
  HEXAGON_CC    Hexagon C compiler (default: clang)
  HEXAGON_CXX   Hexagon C++ compiler (default: clang++)
  HEXAGON_LINK  Qualcomm ELD linker (default: hexagon-link)
  HEXAGON_MCPU  Hexagon CPU target (default: hexagonv68)
  HEXAGON_CPPFLAGS, HEXAGON_CFLAGS
  HOST_CC       Host C compiler for the FastRPC AP stub (default: cc)
  HOST_CPPFLAGS, HOST_CFLAGS, HOST_LDFLAGS
  FASTRPC_LIBS  FastRPC host libraries/link flags (default: -lcdsprpc)
  READELF       ELF inspection utility (default: readelf)

The compile phase uses checked-in pinned QAIC sources by default, compiles the
DSP and AP stub objects, and writes them under DIR. Set QAIC_REGENERATE=1 to
regenerate the interface with QAIC. The link phase links the DSP objects with
Qualcomm ELD and checks its mandatory Hexagon ABI dynamic tag. Run compile on
the target build host and link on an x86_64 host when ELD is only available
there; copy the complete output directory between those hosts.
EOF
}

fail() {
    printf 'build_hexagon.sh: %s\n' "$*" >&2
    exit 2
}

phase=all
output_dir=
while (($#)); do
    case "$1" in
        --phase)
            (($# >= 2)) || fail "--phase needs a value"
            phase=$2
            shift 2
            ;;
        --output-dir)
            (($# >= 2)) || fail "--output-dir needs a value"
            output_dir=$2
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            fail "unknown argument: $1"
            ;;
    esac
done

[[ -n "$output_dir" ]] || fail "--output-dir is required"
case "$phase" in
    compile|link|all) ;;
    *) fail "invalid phase: $phase" ;;
esac

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
dsp_dir=$(cd -- "$script_dir/.." && pwd -P)
output_dir=$(realpath -m -- "$output_dir")
case "$output_dir/" in
    "$dsp_dir/"*) fail "output directory must be outside $dsp_dir" ;;
esac
mkdir -p -- "$output_dir"

qaic=${QAIC:-qaic}
qaic_regenerate=${QAIC_REGENERATE:-0}
hexagon_cc=${HEXAGON_CC:-clang}
hexagon_cxx=${HEXAGON_CXX:-clang++}
hexagon_link=${HEXAGON_LINK:-hexagon-link}
hexagon_mcpu=${HEXAGON_MCPU:-hexagonv68}
host_cc=${HOST_CC:-cc}
readelf_tool=${READELF:-readelf}
read -r -a hexagon_cppflags <<<"${HEXAGON_CPPFLAGS:-}"
read -r -a hexagon_cflags <<<"${HEXAGON_CFLAGS:-}"
read -r -a host_cppflags <<<"${HOST_CPPFLAGS:-}"
read -r -a host_cflags <<<"${HOST_CFLAGS:--O2}"
read -r -a host_ldflags <<<"${HOST_LDFLAGS:-}"
read -r -a fastrpc_libs <<<"${FASTRPC_LIBS:--lcdsprpc}"

case "$qaic_regenerate" in
    0|1) ;;
    *) fail "QAIC_REGENERATE must be 0 or 1" ;;
esac

run() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
    "$@"
}

compile_phase() {
    local qaic_dir="$output_dir/qaic"
    local object_dir="$output_dir/obj"
    mkdir -p -- "$qaic_dir" "$object_dir"

    if [[ "$qaic_regenerate" == 1 ]]; then
        run "$qaic" -st -I "$dsp_dir/include/fastrpc" -o "$qaic_dir" \
            "$dsp_dir/rbt_m3t_orb.idl"
    else
        local generated_dir="$dsp_dir/generated/qaic"
        local generated
        for generated in rbt_m3t_orb.h rbt_m3t_orb_skel.c \
                         rbt_m3t_orb_stub.c; do
            [[ -f "$generated_dir/$generated" ]] ||
                fail "missing pinned QAIC output: $generated_dir/$generated"
            cp -- "$generated_dir/$generated" "$qaic_dir/$generated"
        done
    fi

    local hexagon_target=(--target=hexagon-unknown-none-elf
                          "-mcpu=$hexagon_mcpu" -O3 -G0 -ffreestanding -fPIC
                          -fno-builtin -fvisibility=hidden)
    local dsp_includes=("-I$dsp_dir/include" "-I$qaic_dir"
                        "-I$dsp_dir/include/fastrpc")
    local cxx_flags=(-std=c++17 -fno-exceptions -fno-rtti
                     -fno-threadsafe-statics -fno-use-cxa-atexit
                     -fno-fast-math -ffp-contract=off)

    local source
    for source in orb_extract orb_match orb_dispatch orb_rpc_server; do
        run "$hexagon_cxx" "${hexagon_target[@]}" "${hexagon_cppflags[@]}" \
            "${hexagon_cflags[@]}" "${cxx_flags[@]}" \
            "${dsp_includes[@]}" -c "$dsp_dir/src/$source.cpp" \
            -o "$object_dir/$source.o"
    done

    run "$hexagon_cc" "${hexagon_target[@]}" "${hexagon_cppflags[@]}" \
        "${hexagon_cflags[@]}" "${dsp_includes[@]}" \
        -c "$dsp_dir/src/orb_rpc_memory.c" \
        -o "$object_dir/orb_rpc_memory.o"
    run "$hexagon_cc" "${hexagon_target[@]}" "${hexagon_cppflags[@]}" \
        "${hexagon_cflags[@]}" "${dsp_includes[@]}" \
        '-D__QAIC_SKEL_EXPORT=__attribute__((visibility("default")))' \
        -c "$qaic_dir/rbt_m3t_orb_skel.c" \
        -o "$object_dir/rbt_m3t_orb_skel.o"
    run "$hexagon_cc" "--target=hexagon-unknown-none-elf" \
        "-mcpu=$hexagon_mcpu" -G0 -c \
        "$dsp_dir/third_party/compiler-rt/hexagon/sfdiv_opt.S" \
        -o "$object_dir/compiler_rt_sfdiv_opt.o"
    run "$hexagon_cc" "--target=hexagon-unknown-none-elf" \
        "-mcpu=$hexagon_mcpu" -G0 -c \
        "$dsp_dir/third_party/compiler-rt/hexagon/udivsi3.S" \
        -o "$object_dir/compiler_rt_udivsi3.o"

    run "$host_cc" "${host_cppflags[@]}" "${host_cflags[@]}" -fPIC \
        "-I$qaic_dir" "-I$dsp_dir/include/fastrpc" \
        -c "$qaic_dir/rbt_m3t_orb_stub.c" \
        -o "$object_dir/rbt_m3t_orb_stub.o"
    run "$host_cc" -std=c11 "${host_cppflags[@]}" -O2 -Wall -Wextra \
        -Wpedantic -Werror -fPIC "-I$dsp_dir/include/fastrpc" \
        -c "$dsp_dir/src/orb_rpc_host.c" \
        -o "$object_dir/orb_rpc_host.o"
    run "$host_cc" -shared -Wl,-z,defs \
        -Wl,-soname,librbt_m3t_orb_stub.so "${host_ldflags[@]}" \
        -o "$output_dir/librbt_m3t_orb_stub.so" \
        "$object_dir/rbt_m3t_orb_stub.o" "$object_dir/orb_rpc_host.o" \
        "${fastrpc_libs[@]}"
}

link_phase() {
    local object_dir="$output_dir/obj"
    local objects=(
        "$object_dir/rbt_m3t_orb_skel.o"
        "$object_dir/orb_rpc_server.o"
        "$object_dir/orb_dispatch.o"
        "$object_dir/orb_extract.o"
        "$object_dir/orb_match.o"
        "$object_dir/orb_rpc_memory.o"
        "$object_dir/compiler_rt_sfdiv_opt.o"
        "$object_dir/compiler_rt_udivsi3.o"
    )
    local object
    for object in "${objects[@]}"; do
        [[ -f "$object" ]] || fail "missing link input: $object"
    done

    run "$hexagon_link" --shared -Bsymbolic -z max-page-size=4096 \
        --hash-style=sysv --no-undefined -o \
        "$output_dir/librbt_m3t_orb_skel.so" "${objects[@]}"

    local header dynamic undefined loads skel_export
    header=$("$readelf_tool" -h "$output_dir/librbt_m3t_orb_skel.so")
    dynamic=$("$readelf_tool" -d "$output_dir/librbt_m3t_orb_skel.so")
    undefined=$("$readelf_tool" -Ws "$output_dir/librbt_m3t_orb_skel.so" |
               awk '$7 == "UND" && $8 != "" { print $8 }')
    skel_export=$("$readelf_tool" --dyn-syms -W \
                  "$output_dir/librbt_m3t_orb_skel.so" |
                  awk '$4 == "FUNC" && $5 == "GLOBAL" && $6 == "DEFAULT" &&
                       $8 == "rbt_m3t_orb_skel_handle_invoke" { print $8 }')
    loads=$("$readelf_tool" -l "$output_dir/librbt_m3t_orb_skel.so" |
            awk '$1 == "LOAD" { count += 1 } END { print count + 0 }')

    grep -q 'Class:.*ELF32' <<<"$header" || fail "output is not ELF32"
    grep -q 'Machine:.*QUALCOMM DSP6 Processor' <<<"$header" ||
        fail "output is not a Hexagon DSP ELF"
    grep -Eq '0x70000001 .* 0x3([[:space:]]|$)' <<<"$dynamic" ||
        fail "output lacks DT_HEXAGON_VER=3"
    [[ "$skel_export" == rbt_m3t_orb_skel_handle_invoke ]] ||
        fail "output lacks exported dynamic skeleton entry point"
    [[ -z "$undefined" ]] || fail "unresolved DSP symbols: $undefined"
    [[ "$loads" == 2 ]] || fail "expected two PT_LOAD segments, got $loads"
    ! grep -q '(NEEDED)' <<<"$dynamic" ||
        fail "unexpected dynamic library dependency in the DSP module"

    printf '%s\n' "$header" "$dynamic"
    printf 'Exported skeleton entry: %s\n' "$skel_export"
    printf 'ELF checks passed: ELF32 DSP6, DT_HEXAGON_VER=3, two PT_LOAD, no undefined symbols/dependencies.\n'
}

case "$phase" in
    compile)
        compile_phase
        ;;
    link)
        link_phase
        ;;
    all)
        compile_phase
        link_phase
        ;;
esac

#!/usr/bin/env bash
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
build_dir="${root}/build-arm64"
prefix="${root}/install-arm64"
rbt_prefix="${RBT_PREFIX:-}"
jobs="${RBT_BUILD_JOBS:-2}"
tests=OFF
orbbec=ON
m3t=ON
only=both
hexagon_orb=OFF
dsp_skel=""
dsp_stub=""
hexagon_orb_requested=OFF
dsp_skel_requested=OFF
dsp_stub_requested=OFF

usage() {
    cat <<'HELP'
Usage: ./build.sh --rbt-prefix /path/to/ARM64/rbt-sdk [OPTIONS]
  --prefix DIR       New/local installation prefix (default: ./install-arm64)
  --build-dir DIR    CMake build directory (default: ./build-arm64)
  --jobs N           Parallel compilation jobs (default: 2)
  --tests            Build and run the focused module CTest checks
  --only orbbec|m3t  Build only one module (default: both)
  --hexagon-orb      Enable the optional Hexagon ORB backend (requires both DSP files)
  --dsp-skel FILE    Hexagon DSP skeleton shared library
  --dsp-stub FILE    Native ARM64 FastRPC stub shared library
Uses the installed ARM64 RBT 0.8.0 C++ SDK. Installs no system packages or udev rules.
HELP
}
while (($#)); do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --tests) tests=ON; shift ;;
        --hexagon-orb) hexagon_orb=ON; hexagon_orb_requested=ON; shift ;;
        --rbt-prefix|--prefix|--build-dir|--jobs|--only|--dsp-skel|--dsp-stub)
            (($# >= 2)) || { printf '%s needs a value\n' "$1" >&2; exit 2; }
            case "$1" in
                --rbt-prefix) rbt_prefix="$2" ;;
                --prefix) prefix="$2" ;;
                --build-dir) build_dir="$2" ;;
                --jobs) jobs="$2" ;;
                --dsp-skel) dsp_skel="$2"; dsp_skel_requested=ON ;;
                --dsp-stub) dsp_stub="$2"; dsp_stub_requested=ON ;;
                --only)
                    case "$2" in
                        orbbec) orbbec=ON; m3t=OFF; only=orbbec ;;
                        m3t) orbbec=OFF; m3t=ON; only=m3t ;;
                        *) printf 'Unknown module: %s\n' "$2" >&2; exit 2 ;;
                    esac ;;
            esac
            shift 2 ;;
        *) printf 'Unknown option: %s\n' "$1" >&2; exit 2 ;;
    esac
done
if [[ "$only" == orbbec && ( "$hexagon_orb_requested" == ON || \
      "$dsp_skel_requested" == ON || "$dsp_stub_requested" == ON ) ]]; then
    printf 'Hexagon ORB options require the M3T module; they cannot be used with --only orbbec.\n' >&2
    exit 2
fi
if [[ "$hexagon_orb" == ON ]]; then
    [[ -f "$dsp_skel" && -r "$dsp_skel" ]] || {
        printf '%s\n' '--hexagon-orb requires --dsp-skel pointing to a readable regular file.' >&2; exit 2;
    }
    [[ -f "$dsp_stub" && -r "$dsp_stub" ]] || {
        printf '%s\n' '--hexagon-orb requires --dsp-stub pointing to a readable regular file.' >&2; exit 2;
    }
elif [[ "$dsp_skel_requested" == ON || "$dsp_stub_requested" == ON ]]; then
    printf '%s\n' '--dsp-skel and --dsp-stub require --hexagon-orb.' >&2
    exit 2
fi
[[ "$(uname -s)" == Linux ]] || { printf 'Linux is required\n' >&2; exit 2; }
case "$(uname -m)" in
    aarch64|arm64) ;;
    *) printf 'This source bundle includes the ARM64 Orbbec SDK; compile it on Linux ARM64.\n' >&2; exit 2 ;;
esac
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { printf 'jobs must be positive\n' >&2; exit 2; }
[[ -n "$rbt_prefix" && -d "$rbt_prefix" ]] || {
    printf 'Pass --rbt-prefix pointing to the installed ARM64 RBT 0.8.0 C++ SDK.\n' >&2; exit 2;
}
for tool in cmake python3; do
    command -v "$tool" >/dev/null || { printf 'Missing tool: %s\n' "$tool" >&2; exit 2; }
done
python3 "${root}/verify_source.py"
if [[ -f "${rbt_prefix}/env.sh" ]]; then
    # Existing SDK environment is selected by the caller, never fetched/replaced.
    source "${rbt_prefix}/env.sh"
fi
cmake -S "$root" -B "$build_dir" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_PREFIX_PATH="$rbt_prefix" \
    -DBUILD_TESTING="$tests" \
    -DRBT_ADDONS_ORBBEC="$orbbec" -DRBT_ADDONS_M3T="$m3t" \
    -DRBT_M3T_ENABLE_HEXAGON_ORB="$hexagon_orb" \
    -DRBT_M3T_DSP_SKEL="$dsp_skel" -DRBT_M3T_DSP_STUB="$dsp_stub"
cmake --build "$build_dir" --parallel "$jobs"
if [[ "$tests" == ON ]]; then
    ctest --test-dir "$build_dir" --output-on-failure
fi
cmake --install "$build_dir"
printf '\nInstalled. In each terminal, source the RBT SDK environment, then:\n  source "%s/env.sh"\n' "$prefix"

# Hexagon build

[`scripts/build_hexagon.sh`](scripts/build_hexagon.sh) builds the DSP module
and ARM FastRPC AP stub from the checked-in QAIC interface, and checks the
final DSP ELF. The checked-in generated interface is used by default; this
normal build does not require QAIC or Haskell. Set `QAIC_REGENERATE=1` only
when intentionally regenerating the interface with QAIC. The script writes
generated build files and outputs to the required
`--output-dir`, which must be outside this source directory.

The build requires a Hexagon C/C++ compiler, the Qualcomm ELD Hexagon linker,
a host C compiler, and the host FastRPC library. QAIC is needed only when
regenerating the checked-in interface. No compiler, linker, SDK runtime, or
FastRPC binary is vendored here. See
[`PROVENANCE.md`](PROVENANCE.md) for the pinned public inputs and tested
toolchain details.

When the Hexagon compiler and ELD run on the same build host:

```bash
HEXAGON_CC=/opt/hexagon-sdk/bin/hexagon-clang \
HEXAGON_CXX=/opt/hexagon-sdk/bin/hexagon-clang++ \
HEXAGON_LINK=/opt/eld/bin/hexagon-link \
HEXAGON_MCPU=hexagonv68 \
HOST_CC=aarch64-linux-gnu-gcc \
FASTRPC_LIBS='-L/opt/aidlux/lib -lcdsprpc' \
perception/m3t/dsp/scripts/build_hexagon.sh \
  --phase all --output-dir "$PWD/target/m3t-dsp-hexagon"
```

If ELD is available only on an x86_64 host, run the compile phase on the
Hexagon build host, copy the complete output directory to the ELD host, then
run the link phase there:

```bash
HEXAGON_CC=/opt/hexagon-sdk/bin/hexagon-clang \
HEXAGON_CXX=/opt/hexagon-sdk/bin/hexagon-clang++ \
HEXAGON_MCPU=hexagonv68 HOST_CC=aarch64-linux-gnu-gcc \
FASTRPC_LIBS='-L/opt/aidlux/lib -lcdsprpc' \
perception/m3t/dsp/scripts/build_hexagon.sh \
  --phase compile --output-dir "$PWD/target/m3t-dsp-hexagon"

# Copy target/m3t-dsp-hexagon to the x86_64 ELD build host first.
HEXAGON_LINK=/opt/eld/bin/hexagon-link \
perception/m3t/dsp/scripts/build_hexagon.sh \
  --phase link --output-dir "$PWD/target/m3t-dsp-hexagon"
```

The compile phase targets `hexagon-unknown-none-elf` and uses
`-O3 -G0 -ffreestanding -fPIC -fno-builtin -fvisibility=hidden`. C++ sources
use C++17, disable exceptions, RTTI, fast-math, and FP contraction, and omit
thread-safe statics. The phase also compiles the two pinned compiler-rt
Hexagon helpers required by this core. `HEXAGON_CPPFLAGS` and `HEXAGON_CFLAGS`
pass SDK-specific include paths and options to the DSP compiler. The AP stub
is linked against `libcdsprpc`; set `HOST_CC`, `HOST_CPPFLAGS`, `HOST_CFLAGS`,
`HOST_LDFLAGS`, and `FASTRPC_LIBS` for the target ARM64 toolchain and SDK.

The tested standalone Clang 15 build used the compile phase on AidLux and the
ELD link phase on the local x86_64 build host. The exact compiler paths,
`HEXAGON_CPPFLAGS`, commands, ELF identity, and artifact hashes are recorded
in the repository-local artifact directory
`target/m3t-hexagon-bootstrap-20261023/` (not included in the source package).
This standalone compiler run used minimal target headers because that compiler
package had no Hexagon C library sysroot; a normal SDK build should use the
matching SDK compiler wrapper and headers.

The link phase uses Qualcomm ELD with
`--shared -Bsymbolic -z max-page-size=4096 --hash-style=sysv --no-undefined`.
It checks for ELF32 DSP6,
`DT_HEXAGON_VER=3`, two load segments, no unresolved symbols, and no dynamic
library dependencies. It also checks that `rbt_m3t_orb_skel_handle_invoke` is
an exported `GLOBAL DEFAULT FUNC` in `.dynsym`; the compile command explicitly
overrides QAIC's empty `__QAIC_SKEL_EXPORT` macro for the generated skeleton
source. The output module is `librbt_m3t_orb_skel.so`; the AP stub is
`librbt_m3t_orb_stub.so`.

For device validation, point the M3T device test at the absolute stub and
module paths:

```bash
RBT_M3T_DSP_AUDIT=1 \
build/m3t-dsp/rbt_m3t_dsp_device_test \
  /abs/path/librbt_m3t_orb_stub.so /abs/path/librbt_m3t_orb_skel.so
```

The host facade requests unsigned-module loading through the process-local
FastRPC session control before opening the CDSP handle. It prepends the module
directory to `ADSP_LIBRARY_PATH` for the session, forms the loader URI from the
canonical module basename, and restores the previous environment value after
the session. See the device output and limitations in [`README.md`](README.md).

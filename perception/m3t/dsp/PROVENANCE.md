# DSP build dependency provenance

The DSP module is built from the checked-in RBT sources and IDL. The build
script does not download or install a toolchain. It consumes QAIC, a Hexagon
compiler, Qualcomm ELD, and the host FastRPC development library supplied by
the build environment.

## QAIC

- Repository: <https://github.com/qualcomm/QAIC>
- Source commit: `a66a0019fb488abae955bbc9b3a9de3dc15962d0`
- Source archive SHA-256: `7f033a57adee248962a4317b8b4367060bdef1e7fb50fac403e7e21e2552f5af`
- Tested generator: QAIC `01.00.51`
- The byte-for-byte generated header, skeleton, and AP stub are checked in at
  [`generated/qaic`](generated/qaic/); their SHA-256 values are recorded in
  [`generated/qaic/README.md`](generated/qaic/README.md). Builds use these
  outputs by default. `QAIC_REGENERATE=1` requests regeneration.
- The IDL dependency `include/fastrpc/remote.idl` came from this commit and
  retains its `BSD-3-Clause-Clear` notice. Its SHA-256 is
  `beea202eb5ee1f0adca7af04850511620172898a6e0cd03f227931aba7f22caf`.
- QAIC license and notice: [`licenses/qaic-LICENSE.txt`](licenses/qaic-LICENSE.txt)
  and [`licenses/qaic-NOTICE`](licenses/qaic-NOTICE).

## FastRPC public headers

- Repository: <https://github.com/quic/fastrpc>
- Source commit: `75e71493a8fbe84ae84db153b6f9cefca8f9e19a`
- Copied public headers: `remote.h`, `AEEStdErr.h`, `AEEStdDef.h`,
  `HAP_farf.h`, and `HAP_debug.h` under [`include/fastrpc`](include/fastrpc).
- Each copied header retains its upstream `BSD-3-Clause` identifier.
- The complete upstream license text from this commit is included as
  [`licenses/fastrpc-LICENSE.txt`](licenses/fastrpc-LICENSE.txt), SHA-256
  `683e80f5436e6b0761139e626d2c764870ea0ed19b14916ef4c82772b99c4d44`.
- SHA-256 values:

  | File | SHA-256 |
  | --- | --- |
  | `AEEStdDef.h` | `319a834050e0a53eaecaef0c54d84dab3133ea340de5f68d9f213a7d5463ed3d` |
  | `AEEStdErr.h` | `e25d3f48cfff870394c2af5a18b21c7b050c1370594f3193b5ad7d8033a3ff57` |
  | `HAP_debug.h` | `68103f23d79da9149588d07fb1397e3508730e8b0f71e4a5a1ffaa5b3baf4c43` |
  | `HAP_farf.h` | `08656b697776a6a25b34d57e2bb908ddba499972c202e103c21ebebbbf47e07a` |
  | `remote.h` | `f9d0690cfd8953697d6fc3163c2be448a7b5b801148808c9b1b360857aeabc44` |

## LLVM compiler-rt helpers

LLVM 15.0.7 Clang emitted `__hexagon_divsf3` and `__hexagon_udivsi3` for the
ORB core. The build includes only the corresponding upstream Hexagon helpers:
`third_party/compiler-rt/hexagon/sfdiv_opt.S` and `udivsi3.S` from
<https://github.com/llvm/llvm-project/tree/llvmorg-15.0.7/compiler-rt/lib/builtins/hexagon>.
Both files retain their `Apache-2.0 WITH LLVM-exception` identifiers and are
covered by [`licenses/compiler-rt-LICENSE.TXT`](licenses/compiler-rt-LICENSE.TXT).

| File | SHA-256 |
| --- | --- |
| `sfdiv_opt.S` | `cd1737c23f46377052bdee4ea9b3f6c14dea57c6c65ea63c062f69d123c7300d` |
| `udivsi3.S` | `67947228c07e08e7802eac9f8756a9bf352b6729f0445dc06b54021ae3c710a3` |

## Hexagon compiler and linker

The checked device build used Ubuntu Clang `15.0.7` extracted from Jammy ARM64
packages into the task scratch directory, without a system install. The DSP
objects were compiled for `hexagonv68`; the receiving CDSP is V73. The actual
V73 load and device-oracle results below establish compatibility for this
board and this toolchain output. They do not establish compatibility across
other Hexagon firmware versions.

The extracted package inputs came from the Ubuntu Jammy Ports archive
[`https://ports.ubuntu.com/ubuntu-ports`](https://ports.ubuntu.com/ubuntu-ports):
`clang-15`, `libclang-common-15-dev`,
`libclang-cpp15`, and `lld-15`, each version
`1:15.0.7-0ubuntu0.22.04.3`, architecture `arm64`. Their SHA-256 values are:

| Package | SHA-256 |
| --- | --- |
| `clang-15` | `9cc0e975b41e1c09beed4db94146f37ef543851d2bf02a53161a279e9470ab4d` |
| `libclang-common-15-dev` | `37a06aced0633d549ab3c1d6f79eb1fbd4323959b64a8a7de9fc06768e8144cd` |
| `libclang-cpp15` | `d4f3c9fb55854f1a7ed308b893f06e0fef3f58163121318fbf65a61a241a1462` |
| `lld-15` | `bf6c9915952f5f6c8618653f9b3f4d6531382486866e9bfb7c3be7a60b7dd182` |

Clang also resolved `libLLVM-15.so.1` from the existing AidLux system runtime;
that library was not installed or copied by this task. The extracted Clang
package set therefore records the tested compiler inputs, not a standalone
redistributable SDK. The tested raw Clang had no Hexagon C library sysroot, so
the compile reproduction used the task-local minimal declarations under
`target/m3t-hexagon-bootstrap-20261023/freestanding-include/`. A regular
Qualcomm SDK build should use its matching compiler wrapper and SDK headers;
the build script accepts these via its compiler and `HEXAGON_CPPFLAGS`
environment settings.

The shared object was linked with Qualcomm ELD `24.0` from the official
[ELD nightly release](https://github.com/qualcomm/eld/releases/tag/nightly),
asset ID `644698108`, SHA-256
`edaac2c1637631f1e18c9b54b4e2b5b488bfac7329639d6167151f4bc084e2d6`. The
toolchain archive is retained under the task's `target/` directory, not in
this source tree. ELD emitted the required `DT_HEXAGON_VER` dynamic tag with
value `3`; the preceding LLD 15 output did not contain that ABI tag and was
rejected by the CDSP loader. The build script also requires the QAIC skeleton
callback `rbt_m3t_orb_skel_handle_invoke` to be a `GLOBAL DEFAULT FUNC` in
`.dynsym`, because the CDSP loader resolves that function dynamically.

## Device evidence

The ARM64 AP stub was compiled with Ubuntu GCC `11.4.0` and linked to the
device's existing `libcdsprpc.so`. Its exported QAIC methods and
`rbt_m3t_orb_prepare_cdsp` were verified. The prepare helper uses the public
`remote_session_control(DSPRPC_CONTROL_UNSIGNED_MODULE)` request for CDSP
domain 3 before opening the module.

The custom QAIC add probe returned `42` over CDSP. The production module
passed `rbt_m3t_dsp_device_test` on the V73 board: seven extraction cases, four
batch-match cases, 600 returned neighbors for a 300-by-300 batch, one FastRPC
call per nonempty operation, and zero reported failures. The device oracle
also verified exact keypoint-field and descriptor equality against the
portable CPU core for its cases. The checked final artifacts are in
`target/m3t-hexagon-bootstrap-20261023/final-repro-output/`:

| Artifact | SHA-256 |
| --- | --- |
| `librbt_m3t_orb_skel.so` | `10d2e3ecfebacb6b578d665b4e7d1118094fe2fcdd105d2e532019b2c82349d5` |
| `librbt_m3t_orb_stub.so` | `d229c3f5e0e63d6d3e3bcb619c675d48c527b01a4806a6b16d580d3a4ab65b4e` |

The module was built for V68 and loaded on the V73 CDSP. The final ARM64 M3T
DSP-enabled build passed 7/7 CTest, installed both libraries and all five
DSP license files, and passed the 20-batch extended DSP software fixture with
SIGHUP reinitialization. The source-relative build, install, and profile
records are in `target/aidlux-m3t-0.3.0-dsp-validation/final/`; the standalone
device oracle output is in
`target/m3t-hexagon-bootstrap-20261023/evidence/production-device-test.stdout`.
These are repository-local validation artifacts, not bundled SDK/toolchain
files. The generated QAIC header and sources remain checked in under
`generated/qaic/` for source builds.

# Local release bundles

This guide describes how to build and inspect the Linux x86_64 artifacts for
RBT 0.8.0 from a provisioned checkout. The release packager writes files to a
local output directory; it does not publish to GitHub, PyPI, or another
registry.

## Artifacts and supported platform

The package command produces three install/source archives and three metadata
files:

| Artifact | Contents |
| --- | --- |
| `rbt-sdk-0.8.0-linux-x86_64.tar.gz` | Relocatable Rust and C++ SDK, the RBT CLI tools, the maintained iceoryx2 0.10.0-rbt.2 native prefix, required `libgcc_s` and `libstdc++` runtimes, licenses, docs, examples, Rust SDK sources, and `env.sh` |
| `rbt-0.8.0-source.tar.gz` | Curated source release with maintained and reconstructed source, native patches, and the historical vendor source used by negative-control verification |
| `rbt-0.8.0-cp312-cp312-manylinux_2_28_x86_64.whl` | CPython 3.12 Linux x86_64 Python SDK wheel, normalized for the manylinux 2.28 baseline |
| `release.json` | Product and compatibility version axes and artifact metadata |
| `SHA256SUMS` | Checksums for the release artifacts and manifests |
| `verification.json` | Results from the package command's focused artifact checks |

The SDK uses relative ELF runtime paths, so its native libraries are resolved
from the installed SDK tree after extraction. The wheel is normalized separately:
its build-tree RPATH is removed and its license inventory is repaired. The
packager keeps the supplied wheel unchanged.

The artifacts target Linux x86_64 with glibc. The wheel targets CPython 3.12
and the manylinux 2.28 policy. The SDK manifest records the minimum glibc
version detected from the shipped binaries, but that value is not a guarantee
for every distribution at or above that version. ARM64 and other platforms are
not verified by this release process.

RBT and middleware version axes are independent:

| Axis | Value |
| --- | --- |
| Product / SDK | `0.8.0` |
| IDL language / message descriptor | `1` / `1` |
| Composer control protocol / snapshot | `2` / `4` |
| MCAP file encoding | `1` (`rbt-canonical-v1`) |
| Maintained native source | `0.10.0-rbt.2` |

## Prepare a release builder

Run the packager on Linux x86_64 after provisioning the build inputs. It needs
the maintained native C++ prefix and Phase 4 toolchain used by the project,
along with Rust/Cargo, CMake, `tar`, and the packaging tools `patchelf`,
`auditwheel`, and `bwrap`. The build uses a CPython 3.12 wheel builder with
`maturin` 1.15. A Python 3.11 or newer interpreter is needed for the packager;
use `RBT_RELEASE_PYTHON` to select it when it is not on `PATH`.

The packager also searches `target/release-tools-venv/bin`. Provision its
Python tools with:

```bash
python3 -m venv target/release-tools-venv
target/release-tools-venv/bin/pip install patchelf==0.19.1.0 auditwheel==6.8.2 ninja==1.13.2
```

Provision the Phase 4 compiler toolchain and maintained native prefix using
the [development guide](development.md). The package command does not fetch
missing tools or dependencies. Cargo install steps use the local cache in
offline mode, so provision the locked Rust dependencies first. The shipped
Rust SDK source does not include every registry crate; an application built
with that SDK in a network-isolated environment also needs its dependency cache
preloaded.

From the repository root, build the release set with the defaults:

```bash
./scripts/package_release.sh
```

By default, artifacts go to `dist/rbt-0.8.0`. Paths can be overridden when
using another provisioned environment or output location:

```bash
./scripts/package_release.sh \
  --output-dir /tmp/rbt-release \
  --work-dir /tmp/rbt-release-work \
  --python target/python-verify-venv/bin/python \
  --native-prefix target/iox2-rbt-0.10.0-cpp-install \
  --toolchain target/phase4-toolchain
```

The `--python` option selects the CPython 3.12 wheel builder; its default is
`target/python-verify-venv/bin/python`. Use `--python-wheel PATH` to supply a
wheel that is already built. The packager also accepts `--native-prefix PATH`
and `--toolchain PATH`; they default to the project's standard provisioned
prefix and Phase 4 toolchain. `--work-dir PATH` selects scratch build space.
The output directory is a local staging area for review and installation.

## Inspect the package results

The packager's focused checks verify the source archive contents and audit,
checksums, ELF runtime paths, and imports from a private wheel environment. It
extracts the SDK outside the checkout and builds Rust and C++ consumers against
that extracted SDK. For build checks it masks the original source tree and
native prefix while keeping the Phase 4 compiler available; runtime and import
checks run with the checkout hidden in a bubblewrap environment.

These checks establish artifact usability on the builder host. They use its
kernel and glibc and do not qualify the packages on another distribution.
They do not repeat the full runtime or TF soak suite, and they do not change
any physical-hardware result. See the relevant [testing guide](testing.md) and
[v0.8 TF report](tf-report.md) for runtime and hardware acceptance status.

Check the staged files locally before distributing them:

```bash
cd dist/rbt-0.8.0
sha256sum -c SHA256SUMS
```

The script performs these checks when it stages the output; `verification.json`
records their results. Review the manifests and retain them with the artifacts.

## Use the SDK archive

Extract the archive to a stable location and source its environment file in
each shell that uses the SDK:

```bash
mkdir -p "$HOME/opt/rbt"
tar -xzf rbt-sdk-0.8.0-linux-x86_64.tar.gz -C "$HOME/opt/rbt"
source "$HOME/opt/rbt/rbt-sdk-0.8.0-linux-x86_64/env.sh"
rbt version --verbose
rbt doctor
```

`env.sh` adds the bundled tools and Rust SDK to the current environment. It
derives paths from its own location, so the extracted directory can be moved
as a unit. Start a project with the normal CLI flow:

```bash
rbt new hello-robot
cd hello-robot
rbt check
rbt run
```

For a C++ application, configure CMake with the extracted SDK directory in
`CMAKE_PREFIX_PATH`:

```bash
SDK_ROOT="$HOME/opt/rbt/rbt-sdk-0.8.0-linux-x86_64"
cmake -S my-app -B build -DCMAKE_PREFIX_PATH="$SDK_ROOT"
```

Rust applications built from the included Rust SDK use the registry
dependencies available to Cargo; offline builds need those dependencies
cached locally.

## Install the Python wheel

The wheel requires CPython 3.12 and NumPy 1.23 or newer. Install it in a
virtual environment with a compatible Python:

```bash
python3.12 -m venv .venv
source .venv/bin/activate
python -m pip install ./rbt-0.8.0-cp312-cp312-manylinux_2_28_x86_64.whl
```

This is the Python data SDK wheel. The native RBT command-line tools and Rust
or C++ SDK are supplied in the SDK archive.

## Source release contents

The source archive is built from an explicit file allowlist. It includes the
maintained RBT source, reconstructed iceoryx2 source and patches, and the
historical vendor source used by negative-control verification. It omits Git metadata,
build directories, secrets, and generated package outputs. Use the release
archive for source review or a clean source build; a full development checkout
also contains local history and build evidence that are not part of the
distribution.

The Git-free native export is verified by a frozen file manifest whose SHA-256
is pinned in `scripts/check_iceoryx2_rbt_export.py`. The normal Git checkout
continues to use its pinned upstream commit and audited binary patch diff.
`bootstrap_iceoryx2_rbt.sh --source-only` accepts either verified representation.

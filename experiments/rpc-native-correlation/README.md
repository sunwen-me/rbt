# Native RPC correlation fix experiment

This is an isolated regression harness and proposed native patch for exact
iceoryx2 0.9.3. The main rbt workspace and installed C++/Python dependencies
have **not** been switched to the patch.

The baseline is upstream commit
`6ef7c55176a8487ae7ca53c2e0649d5df9a1b3b4`. On the unmodified registry crate,
the original six-test harness passed four tests and failed the stale-client
response and 100-cycle client-slot-reuse tests at the first reuse. The raw
rbt reproducer also failed independently. Strengthened assertions now also
check connection state before and after an old request is dropped.

The proposed patch:

- resolves retained server requests by their existing native client origin;
- retains that identity on response loans and validates it after refreshing
  connections under the native shared-state lock;
- suppresses delivery to a retired client or a different request channel;
- stops allocating request IDs before they wrap;
- explicitly declares the native crate's parent workspace so the standalone
  rbt integration harness can resolve both workspaces without ambiguity.

There is no new payload field, wire header, serializer, retry, or custom
transport. The native public C++/FFI integration and final release gates still
need verification before production adoption. Dropping a stale response
remains a successful no-op in this experiment, consistent with native
fire-and-forget behavior; `Ok(())` is not a delivery acknowledgement.

## Reproduce

Run from the rbt repository root. Use the pinned local C/C++ toolchain if the
host does not provide a linker:

```bash
export CC="$PWD/target/phase4-toolchain/bin/x86_64-conda-linux-gnu-gcc"
export CXX="$PWD/target/phase4-toolchain/bin/x86_64-conda-linux-gnu-g++"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="$CC"
```

The unpatched run is expected to fail its correlation assertions:

```bash
CARGO_TARGET_DIR="$PWD/target/rpc-native-correlation-tests" \
cargo test --manifest-path experiments/rpc-native-correlation/Cargo.toml \
  --test native_correlation -- --test-threads=1
```

Prepare a separate checkout of the pinned source, then apply the saved patch.
The target path below must not already contain unrelated changes:

```bash
git clone --shared --no-hardlinks target/iceoryx2-v0.9.3 \
  target/iceoryx2-rpc-correlation-fix
git -C target/iceoryx2-rpc-correlation-fix apply \
  ../../experiments/rpc-native-correlation/iceoryx2-0.9.3-correlation.patch
```

The current session already prepared that patched checkout. The Cargo patch
below applies only to this standalone command; it does not edit the root
production manifest or lockfile:

```bash
CARGO_TARGET_DIR="$PWD/target/rpc-native-correlation-patched" \
cargo test --manifest-path experiments/rpc-native-correlation/Cargo.toml \
  --config "patch.crates-io.iceoryx2.path=\"$PWD/target/iceoryx2-rpc-correlation-fix/iceoryx2\"" \
  --test native_correlation -- --test-threads=1
```

The same harness imports rbt's actual fixed, dynamic, stress and resource
test sources as Cargo test targets `rbt-fixed`, `rbt-dynamic`, `rbt-stress`
and `rbt-resources`. It does not copy or weaken their assertions.

## Retained evidence

- `target/rpc-blocker-evidence/unpatched.log`: original baseline failures.
- `target/rpc-blocker-evidence/patched.log`: six native tests passed, including
  100 slot-reuse cycles, before the strengthened connection-state assertions.
- `target/rpc-blocker-evidence/patched-rbt-regression.log`: strengthened native
  assertions 6/6, existing Rust fixed 15/15, dynamic 4/4 and stress 4/4 passed
  (29 total), exit 0. This does not establish C++/FFI, Python or release acceptance.

The overall v0.6 acceptance status is recorded in
[`docs/rpc-report.md`](../../docs/rpc-report.md).

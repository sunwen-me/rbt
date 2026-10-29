# Unified maintained iceoryx2 baseline

Status: unified production dependency integration accepted on the tested Linux
x86_64 environment. RPC/FFI and network repairs share one maintained source;
this is not acceptance of a production remote-RPC or Network Gateway product.

## Source and production wiring

The active native dependency is `0.10.0-rbt.2`, upstream v0.10.0 commit
`135d09dd8b29f321f1725920d434864c4e512378` plus **one six-patch series**.
The complete source-diff SHA-256 is
`b0e90730fee476487b0c47dba0a272b0d0df9c5480629f8ac64849b8882d91d1`.
The reconstructed canonical source is `third_party/iceoryx2-rbt/source` on
local branch `codex/iceoryx2-rbt-0.10.0`. No remote repository or push was made.

The four network patches and two RPC/FFI patches are applied to that single
source, not to two runtime versions. Root Rust, the Python native extension,
the C++ native build, installed Rust SDK, and `rbt new` generated projects all
resolve to it. Cargo package versions remain 0.10.0; the maintained build name,
direct path dependency, checked patch series and source fingerprint identify
the fork.
The historical 0.9.3 vendor tree is no longer a production dependency.

The source is a reconstructible ignored checkout; its durable definition is
`UPSTREAM.toml`, `patches/series`, `patches/SHA256SUMS` and
`SOURCE_DIFF_SHA256`. Bootstrap verifies a fresh patch replay and refuses
unknown source drift. Rust 1.89+ is required by the new native baseline.

## Combined repairs

1. Original PublishSubscribe/Event settings cross the tunnel; descriptors are
   validated and fork keys use `iox2-rbt/v1`. Complete settings participate in
   channel identity, so differing QoS is isolated rather than negotiated.
2. Destination readiness follows successful local SHM ingress. Paired source
   Events wake the source tunnel but are not independently forwarded. Multiple
   stream pairs, pending readiness, and consumer-held mirror recovery are
   supported without payload polling.
3. Carrier frame queues have configurable bounded capacity, drop-oldest or
   drop-newest, and counters. This does not bound all network byte buffers or
   guarantee lossless Control.
4. Linux inotify/pidfd/native WaitSet drives reactive discovery, process exits,
   local readiness and startup convergence.
5. RPC response routing resolves the original client identity and checks the
   copied request ID, not just a reusable connection slot. Identity survives
   fixed, dynamic-slice and FlatBuffer uninitialized/initialized response paths.
   Stale request destruction does not close a replacement client's channel.
   Request IDs stop at exhaustion before allocating a chunk or channel.
6. C response storage is 264 bytes, validated by native FFI layout checks; the
   old 0.9.3 storage patch was ported, not applied unchanged to a different ABI.

The production Rust and C++ event adapters consume counted 0.10 activations as
advisory readiness. Counts never become payload counts. Payload drain budgets,
cancellation, native correlation and loan/borrow ownership remain enforced.
Existing fixed and slice RPC APIs remain supported. No rbt message layout,
layout hash, readiness version, Composer protocol or snapshot version changed.

## Packaging and mixed-version rejection

C++ uses fresh `target/iox2-rbt-0.10.0-cpp-build` and
`target/iox2-rbt-0.10.0-cpp-install` directories. Its provenance stamp binds the
fork version, upstream commit, source fingerprint and ordered patch series.
All active verifier defaults use this prefix. Both in-tree and installed CMake
packages require C, BB-C++ and C++ packages at exactly 0.10.0, all from the
same prefix, and check imported native include targets as well as provenance.
This closes the observed case where CMake retained old C/BB cache entries while
finding a new C++ package. Missing or mismatched provenance is rejected;
official unpatched 0.10 is not accepted merely because its version matches.

Installed Rust SDKs contain the patched full native source, patch metadata and
upstream licenses, without `.git` or build-target directories. Their manifest
and root workspace use a direct native path dependency, not only a root Cargo
patch (which does not propagate to external consumers). Generated projects
select the same source directly for their types crate and add a root patch.
The installed workspace excludes the nested native workspace so its inherited
package metadata is resolved from the correct Cargo root. External-consumer
tests assert the resolved native package ID, not just successful compilation.
Installation validates the exact canonical source fingerprint before creating
the prefix or building/copying artifacts, regardless of ambient source-path
overrides. Project creation rejects SDKs missing either bundled native manifest.
Fresh-wheel Component verification rebuilds executable fixtures explicitly;
workspace tests alone can leave an old Camera executable beside a new wheel.

Rebuild and restart every process in an old graph before switching. This does
not establish native 0.9/0.10 SHM interoperability. See
[compatibility policy](compatibility.md) for existing external project updates.

## Verification evidence

Retained acceptance runs from 2026-10-16 (repository-relative paths):

- Network quick: `target/iceoryx2-rbt-verify-logs/quick.PpcajUUh/result.log`,
  status 0. Network full:
  `target/iceoryx2-rbt-verify-logs/full.rf7pid2B/result.log`, status 0. Each
  verified source/patch replay, native identity, and unchanged production and
  acceptance-input fingerprints during that run.
- Full network coverage includes 100 deterministic 1080p Images, padded Images,
  PointCloud counts 0/1/100/100000 and three approximately 20 MiB clouds,
  slow-Sensor convergence to frame 299, ten isolated veth reconnect cycles,
  tunnel-process restart, schema rejection, filtering and provider replacement.
  Both tunnel processes remained reactive during ten idle seconds: 0 and 1 CPU
  ticks, respectively. This is the maintained-fork gate, not completion of the
  older feasibility spike's larger impairment/performance/300-second matrix.
- The real production `SliceStream<Image> + WaitContext` path passed across
  both network stacks. The consumer ignores its synthetic initial pass and
  validates one complete 6,220,800-byte Image after actual readiness. Production
  and probe native package IDs both resolve to the canonical source path.
- Core request-ID exhaustion passed; the native FFI storage suite passed all
  60 checks. The full gate also reran the specific response-storage check.
  Fork link/carrier/CLI tests and the 71-test conformance suite passed.
- Two new raw-native fixed/slice uninitialized-response regressions pass on the
  maintained source and fail at stale-client identity checks on pristine
  upstream 0.10 (negative control). Evidence:
  `target/iceoryx2-unified-logs/rpc-native-identity-final.log` and
  `target/iceoryx2-unified-logs/rpc-negative-control-result.log`.
- C++ Debug and Release: seven CTest groups passed in each configuration.
  Evidence: `target/iceoryx2-unified-logs/cpp-tests.log` and
  `target/iceoryx2-unified-logs/cpp-release-tests.log`. Reconfiguring an actual
  old CMake cache selects all three new packages; a deliberately pre-imported
  old native target is rejected (`cpp-old-cache-reconfigure.log` and
  `cpp-imported-target-negative.log` in the same directory).
- RPC quick passed, including fixed/dynamic/fault/stress tests, release
  resource tests with 100,000 calls, current C++ and Python native RPC, frozen
  layout hashes, workspace tests and strict Clippy. Final retained run:
  `target/rpc-verify-logs/rpc-quick-20261016T130820Z-2729984` (exit 0).
  The final workspace run passed 260 tests across 56 result groups, with one
  debug-only ignored long test; that same 100,000-call test was executed and
  passed in the release resource lane (five passed, zero ignored).
- Rust/C++/Python RPC interop passed ten directions, two dynamic cases, three
  asyncio cases and both 4096-call burst directions:
  `target/iceoryx2-unified-logs/rpc-interop-retry.log`. Under concurrent build
  load the original 15-second outer watchdog expired near the end of one
  burst. The passing rerun used the supported 60-second outer watchdog;
  workload, per-call three-second deadline and idle-resource assertions were
  unchanged.

- Python RX/TX/ownership: 114 passed (15 full-only tests deselected), including
  local NumPy zero-copy checks. Evidence:
  `target/python-verify-logs/python-quick-20261016T130258Z-2692505`.
  Fresh-wheel synchronous Component mixed-graph/lifecycle/failure/recovery
  verification passed, with model loading once and production unsafe count
  unchanged at 49:
  `target/python-asyncio-verify-logs/python-asyncio-quick-20261016T130156Z-2689157/inherited-component/python-component-quick-20261016T130156Z-2689272`.
- Fresh CPython 3.12 asyncio wheel: 25 API tests and the runtime quick gate
  passed, including zero-copy receive, timeout, task/provider cancellation,
  sibling cleanup and no unexpected pending-task warnings. Evidence:
  `target/python-asyncio-verify-logs/python-asyncio-quick-20261016T130721Z-2718634`.
  This composed rerun reused the separately passed inherited gate via
  `RBT_PYTHON_ASYNCIO_SKIP_INHERITED=1` and selected an existing build-tools
  environment with `RBT_PYTHON_ASYNCIO_TOOL_PYTHON`. The earlier outer run
  reached passed Component verification but could not download build tools
  under `PIP_NO_INDEX=1`. The successful asyncio run still built a new wheel
  and installed it into an unused runtime venv outside the checkout.
- Guarded installation passed: installed SDK compilation, standalone external
  Rust/C++ RPC consumers, exact bundled-native package ID, generated project
  build, graph activation and complete process cleanup. Evidence:
  `target/iceoryx2-unified-logs/install-smoke-guarded.log` and
  `/tmp/rbt-install-smoke.pv0Z1n/logs`. The partial-SDK unit regression also
  passed; it rejects either missing native manifest before creating a project.

The network environment is two Linux network namespaces with private SHM,
mount/IPC/UTS stacks and MTU 1500, not two physical hosts. The lifecycle probe
uses the real production Composer/dependent with a local health-metadata proxy;
it is not a distributed Composer or a productized cross-host capability proxy.

## Reproduction

```bash
./scripts/bootstrap_iceoryx2_rbt.sh --source-only
./scripts/bootstrap_iceoryx2_rbt.sh --verify
./scripts/bootstrap_iceoryx2_cpp.sh
./scripts/verify_rpc.sh --quick
./scripts/verify_iceoryx2_rbt.sh --full
RBT_RPC_INTEROP_WATCHDOG_SECONDS=60 ./scripts/test_rpc_interop.sh
./scripts/verify_python_asyncio.sh --quick
./scripts/test_install.sh
```

The unified patches fix local RPC and network PublishSubscribe/Event on the
same baseline. They do **not** add remote RPC. Physical-host deployment,
long-duration network impairment/freshness soaks and a productized cross-host
integration remain distinct acceptance tasks, not inferred passes here.

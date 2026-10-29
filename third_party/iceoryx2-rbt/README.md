# Unified maintained iceoryx2 baseline

This directory contains the reproducible source delta for rbt's maintained
iceoryx2 production baseline, `codex/iceoryx2-rbt-0.10.0`. RPC correlation,
C FFI storage, and network fixes are one six-patch source series. The build is
identified as `0.10.0-rbt.2`. Its immutable upstream base is release `v0.10.0`, commit
`135d09dd8b29f321f1725920d434864c4e512378`.

`UPSTREAM.toml`, the ordered `patches/series`, patch checksums, and the complete
source-diff fingerprint define the build. The working Git checkout defaults to
`third_party/iceoryx2-rbt/source`; it is reconstructed from this tracked patch set.
The bootstrap refuses to overwrite a checkout whose source differs from the
recorded delta. It preserves local work on failure.

## Build and verify

```bash
./scripts/bootstrap_iceoryx2_rbt.sh --source-only
./scripts/bootstrap_iceoryx2_rbt.sh --verify
./scripts/bootstrap_iceoryx2_rbt.sh --build
./scripts/bootstrap_iceoryx2_cpp.sh
./scripts/verify_iceoryx2_rbt.sh --quick
./scripts/verify_iceoryx2_rbt.sh --full
```

`--verify` applies the patch series to a fresh temporary checkout and checks its
exact source fingerprint. `--build` builds the Zenoh link tunnel and native
service inspection CLI. The verifiers retain logs below
`target/iceoryx2-rbt-verify-logs` and use two distinct Linux network/IPC stacks
with private SHM. Production dependencies and ABI source fingerprints are
checked before and after the run.

When the pristine pinned checkout already exists at `target/iceoryx2-v0.10.0`,
bootstrap can clone it locally. Otherwise it obtains the upstream repository.
Builds require Rust 1.89+ and use the repository's Phase4 C toolchain and locked
Cargo workspaces. The reconstructed source is ignored by Git; retain the patch
series and metadata, and bootstrap before a fresh source build.

## Contract changes

1. Service descriptors include original PublishSubscribe and Event settings.
   Imports validate descriptor name, pattern, and hash and retain the advertised
   settings. Conflicting offers are refused. The fork uses `iox2-rbt/v1` Zenoh
   keys so stock v0.10 descriptors cannot be mistaken for this wire format.
   The channel fingerprint includes all settings: independently created native
   services with the same name but different QoS do not exchange samples.
   There is no implicit compatible-QoS negotiation in this first version.
2. Explicit payload/readiness pairs generate readiness locally after each
   successful inbound SHM publication. The paired Event's source notifications
   wake its local tunnel but are not separately transported. A missing readiness
   bridge retains pending readiness until discovery opens it. A local notifier
   error remains observable and never causes payload/Control retry.
3. The carrier has configurable bounded frame queues with `drop-newest` and
   `drop-oldest` policies and aggregate drop counters. Capacity bounds frame
   count, not a total byte budget. These policies do not imply lossless Control.
4. Linux pure reactive execution observes local node/service-tag changes with
   inotify, process exits with pidfds, and local Event listeners with the native
   WaitSet. Startup performs discovery before waiting. Native PublishSubscribe
   sources without any notification source still require explicit polling.
5. A consumer-held imported service retains its import identity across remote
   withdrawal/recovery. Keeping its local factory alive cannot promote it into
   an export. A genuinely recreated local native service has a new creation id.
6. RPC responses carry the original client and request identity through fixed,
   slice and FlatBuffer response loans. Connection-slot reuse cannot route a
   stale response to a replacement client. Request IDs stop at exhaustion
   rather than wrapping; the C FFI response storage is updated with native
   layout checks. These are local RequestResponse fixes, not remote RPC support.

Example for both sides, with a separately configured Zenoh peer endpoint:

```bash
iox2-link-tunnel-zenoh --reactive --zenoh-config host.toml \
  --rbt-stream rbt/cap/00000000-0000-0000-0000-000000000001/stream \
  --queue-capacity 1 --queue-overflow drop-oldest
```

`--rbt-stream` is repeatable, selects both the payload and `/ready` service, and
restricts export/import to selected streams plus any explicit `--allow`
patterns. Pair configuration is required on both tunnels. Local SidecarV1 uses
Event ID 0; no rbt message layout or descriptor hash changes.

## Production identity and acceptance boundary

The root Rust workspace, Python native extension, C++ bootstrap, installed Rust
SDK, and generated projects use this same maintained source. Cargo package
versions remain 0.10.0; the fork build identifier and source fingerprint
disambiguate the maintained baseline. C++ uses a fresh versioned build/install
directory and a provenance stamp, checked by
`scripts/check_iceoryx2_rbt_prefix.sh`; merely finding official 0.10 libraries
does not satisfy that check.

The old 0.9.3 vendor tree remains only for historical comparison/reproduction.
It is not the active production dependency. This upgrade does not promise
0.9/0.10 SHM interoperability: all processes in a local graph must rebuild and
use the unified native baseline. Frozen rbt message descriptors do not certify
the native middleware's internal ABI across versions.

New fixes must include a regression and remain separate, reviewable patches.
An upstream update is a deliberate base change with patch replay, local/native
regressions, two-stack failure tests, ABI checks, and then SDK acceptance.
Keep upstream copyright and license notices when redistributing the source.

The old network-only milestone is historical. Current unified acceptance is
recorded in [`docs/iceoryx2-unified-report.md`](../../docs/iceoryx2-unified-report.md).

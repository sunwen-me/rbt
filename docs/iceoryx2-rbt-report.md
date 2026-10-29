# Maintained iceoryx2 network branch: first milestone

> Historical network-only milestone (`0.10.0-rbt.1`). The active production
> baseline now merges RPC/FFI and network fixes as `0.10.0-rbt.2`; see
> [unified acceptance](iceoryx2-unified-report.md). Statements below about
> production remaining on 0.9.3 describe the earlier milestone only.

## Scope and source identity

This is an isolated maintained network candidate, not a production dependency
upgrade or a completed rbt Network Gateway. The production Rust/C++/Python
stack remains pinned to iceoryx2 0.9.3 with the existing RPC correlation and C
FFI patches. No custom TCP/UDP framing, retransmission, distributed Composer,
or new network message ABI was introduced.

The maintained build is `0.10.0-rbt.1`, based on upstream `v0.10.0`, commit
`135d09dd8b29f321f1725920d434864c4e512378`. Its local dependency branch is
`rbt/network-0.10.0` in `target/iceoryx2-rbt-v0.10.0`. The immutable original
checkout `target/iceoryx2-v0.10.0` remains unchanged. No remote fork or push
was created. The surrounding repository has no initial commit, so no parent
history was invented; the reviewed source diff is against the dependency's
real upstream HEAD.

The durable source is the ordered four-patch series, SHA-256 checksums and
complete source-diff fingerprint under `third_party/iceoryx2-rbt`, rather than
an unrecoverable modification under the ignored build directory. The final
source-diff SHA-256 is:

```text
1a9febcdcff2334b7a35d86ec4450826193e752b99bea665986e49cbb407014f
```

## Implemented contracts

| Patch | Contract |
| --- | --- |
| 0001 | Carry original PublishSubscribe/Event settings; validate descriptor name, pattern and service hash; reject conflicting imports; isolate fork wire keys under `iox2-rbt/v1`. |
| 0002 | Reconstruct destination-local readiness after successful inbound SHM publication; support multiple explicit stream pairs; preserve consumer-held mirror import identity across withdrawal/recovery. |
| 0003 | Bound each carrier frame inbox; configure capacity and drop-newest/drop-oldest; expose aggregate accepted/popped/drop counters. |
| 0004 | Linux inotify/pidfd/native WaitSet reactive execution, startup discovery, repeatable `--rbt-stream`, queue flags and fork version identification. |

Full settings participate in the channel fingerprint. Independently created
same-name native services with differing settings remain separate channels;
this candidate does not negotiate compatible QoS. The corresponding upstream
conformance test was changed to assert that deliberate isolation over a
one-second observation interval, retaining positive same-settings transport
tests. Nothing is marked ignored or expected-failure to pass the gate.

For an explicitly configured stream pair, source-local readiness still wakes
the source tunnel. That Event is not separately forwarded over the network.
After inbound payload insertion succeeds, the destination tunnel notifies its
local `/ready` Event with ID 0. Missing readiness bridges retain pending state;
local notifier failures remain observable and do not retry payloads or Control.
This removes the independent network-readiness delivery race, not the existing
source-local send/notify failure boundary.

Queues are bounded by frame count, not an end-to-end byte or RSS budget.
Zenoh/TCP/kernel buffers still exist. Dropping policies do not imply lossless
Control, and network crossings copy/serialize; there is no network zero-copy
claim. Production payload layouts and descriptor hashes were not changed.

## Verification method

The public verifier replays all patches in a fresh checkout, checks the exact
source fingerprint, builds locked workspaces, and runs:

- Link/backend/tunnel unit tests: 37 + 37 + 14.
- Zenoh carrier unit tests: 18; Linux CLI tests: 9.
- Full carrier conformance: 71, including both local and IPC variants.
- No-default-features library checks; isolated probe formatting, tests and
  Clippy with warnings denied.
- Unchanged production type/Composer tests: 68; stale RPC correlation tests: 2;
  exact production vendor reconstruction.

The CLI FD-cleanup test runs in its own exact-test child process so concurrent
test descriptors cannot corrupt its before/after counts. Startup, pending
numeric-node directories before Alive registration, service tags, idle wakes,
SIGKILL pidfds and descriptor teardown have focused regressions.

Network acceptance uses two Linux network namespaces with separate network,
mount, IPC and UTS stacks, private `/tmp` and `/dev/shm`, a veth pair with MTU
1500, and explicit Zenoh TCP peers. These are not two physical machines. Both
tunnels use `--reactive`, queue capacity 1 and drop-oldest, without `--poll`.
Consumer payload reads occur only after native WaitSet readiness; initial
payload drain is zero. Startup retries discover service metadata only.

The native probe checks exact rbt Image/PointCloud layouts and every payload
element. It does not use the existing production v0.9 Stream/WaitContext
wrappers. The lifecycle prototype connects a native v0.10 publisher-count
health observer to a local capability proxy over the existing Composer
protocol. The Composer and dependent are real unchanged production runtimes;
the observer polls only health metadata at 50 ms, never payloads or tunnel
progress. The dependent's activation binds Camera but does not open a v0.10
data endpoint. This is control-plane feasibility, not an SDK interoperability
or completed proxy product claim.

## Final retained results

The first maintained-network milestone is accepted. Both frozen-snapshot
public runs completed with `status=0` and `production_unchanged=PASS`:

- quick: `target/iceoryx2-rbt-verify-logs/quick.Xeb1qWAY`;
- full: `target/iceoryx2-rbt-verify-logs/full.ZtwgI0Gr`.

Each directory retains the outer `result.log`, namespace console, cleanup,
production and acceptance-input fingerprints, and individual check logs.
The full run confirms all 115 fork unit tests, 71 conformance tests and 70
focused stable regressions passed; these are not a claim about every test in
the repository. Both final runs use the source-diff fingerprint above.

| Gate | Retained result |
| --- | --- |
| Patch replay and builds | All four patches replay on a fresh pinned checkout with identical source fingerprint; CLI reports `0.10.0-rbt.1`. |
| Preexisting service | Pure-reactive startup discovers a provider created before the tunnel. |
| Last-frame readiness | One 6,220,800-byte Image is received after native WaitSet readiness, with zero initial drain and no payload polling or separately forwarded network readiness. Quick `final-sub.log` retains this standalone case. |
| Image | 100/100 1920×1080 RGB8 frames, final sequence 99, exact headers and every byte; 100 wakes, zero spurious wakes. Publication is paced at 200 ms after filling each frame, not a throughput benchmark. |
| Padded Image | 640×480 RGB8, stride 2048, 983,040 bytes, padding retained and validated. |
| PointCloud | N=0/1/100/100000 and three 1,310,720-point frames (20 MiB each), every float bit and header validated. |
| Control-profile diagnostic | 20/20 small Image-shaped byte samples with depth 4 and safe overflow disabled. This is not an actual ControlCommand or lossless-network-Control certification. |
| Filtering/schema | Unselected private service not mirrored; fixed-u64 open against the Image mirror fails with `IncompatibleTypes` before sample endpoints are created. |
| Real local lifecycle | Active → Blocked → Active → Blocked after two provider SIGKILLs and a replacement. Both signal waits are 137; same dependent process, prepare=1, activations=2, replacement proxy identity differs, stale Camera capability absent after each loss. |
| Slow Sensor | 300 publications at a 5 ms interval plus fill time, consumer delay 50 ms: 32 samples received, strictly increasing, final sequence 299, no payload polling. This demonstrates bounded recent-data behavior for this case, not sustained-impairment freshness. |
| Recovery | Final-frame delivery after ten 50 ms veth down/up cycles, and separately after an actual source tunnel process replacement. Fast flaps alone are not counted as ten distinct TCP sessions. |
| Idle | Ten seconds after the full run: source/destination CPU ticks 0/1; FD counts unchanged at 29/25. Threads settle from 9/8 to 7/6; RSS changes from 34,820/85,140 to 34,848/85,124 KiB. These are observations, not a memory ceiling or soak result. |
| Teardown | Namespace cleanup status 0, all tracked processes gone; production and acceptance-input hashes unchanged. |

Earlier development attempts are not final acceptance: one found the
concurrent-FD-count test defect, another was invalidated by editing a running
shell harness, and the first conformance pass exposed two old differing-QoS
expectations. The frozen quick/full runs above supersede those attempts.

## Reproduction and next migration gates

```bash
./scripts/bootstrap_iceoryx2_rbt.sh --source-only
./scripts/bootstrap_iceoryx2_rbt.sh --verify
./scripts/bootstrap_iceoryx2_rbt.sh --build
./scripts/verify_iceoryx2_rbt.sh --quick
./scripts/verify_iceoryx2_rbt.sh --full
```

Bootstrap never overwrites source drift. It can clone the pristine pinned
local source, or obtain upstream when unavailable. Verifiers preserve logs,
hash the production manifests and files under `crates/`, `sdk/cpp/`,
`python/`, and `third_party/iceoryx2/` before and after,
reject acceptance-input changes during a run, and clean only the processes
and namespaces created by that run. Existing local build toolchain and Cargo
dependencies are prerequisites. See the maintenance README for CLI examples.

Before production migration, port and validate the stable 0.9.3 RPC fixes on
v0.10 (the existing patches do not apply directly), adapt the production
Stream/WaitContext path, and accept C ABI/C++/Python ownership and lifecycle
tests. Physical-host testing, sustained impairment/freshness/resource soaks,
and a productized local proxy remain separate gates. This milestone does not
silently certify those gates from the isolated native tests.

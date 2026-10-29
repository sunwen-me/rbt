# iceoryx2 upstream v0.10.0 cross-host report

> Historical production baseline: production-version statements below describe
> the workspace snapshot audited in September 2026, before unification.
> Production now uses maintained iceoryx2 `0.10.0-rbt.2`; the isolated
> official upstream `v0.10.0` results and their unresolved product gates remain
> scoped to that experiment. See the [unified baseline report](iceoryx2-unified-report.md).

## Verdict and evidence boundary

The v0.10.0 Zenoh link tunnel is partially fixed for generic cross-host
transport. The strict full run's retained namespace gate ended with
`mode=full transport=PASS known_limitations_logged=1`, while the pure-reactive
local-lifecycle and bounded Sidecar progress contracts remain failed. Its
terminal-only outer result line was not retained in the run directory, so this
report does not present one as captured evidence. All full-run processes
exited, no leftover log was produced, and the production hashes were rechecked
unchanged. Imported remote-only services also use receiver defaults instead of
preserving the exporting service's per-service settings. This is a generic
transport result, not production rbt compatibility.

The latest quick run retained the exact outer result
`execution=PASS critical_gates=PARTIAL product_unchanged=PASS` and included
the explicit spurious-wake safety gate.

Evidence roots:

- quick: `target/cross-host-upstream-verify-logs/upstream-quick-20260922T104504Z-28629`;
- full: `target/cross-host-upstream-verify-logs/upstream-full-20260922T055557Z-67857`.

The strict full run's environment metadata names the then-local main checkout
`671932de95ceddf5803e95f8884b889d28158a3c`; that checkout was not its runtime
candidate. The final live ref refresh moved the isolated main audit to
`d32f60beedeef90ad480a3c7926c7211dd170512`. The latest quick metadata and the
focused 71-test/build logs use the refreshed SHA, whose intervening changes do
not touch link, gateway, Zenoh source, or their tests.

The full namespace run is authoritative for the final-trap, churn, resource,
and Sensor values below; the quick run is authoritative for the added
spurious-wake gate and its captured outer postconditions. The probe source is
isolated under `experiments/cross-host-upstream/**`; the production 0.9.3
baseline in this report is the pre-unification snapshot.

## Version and source matrix

| Subject | Exact value | Evidence |
|---|---|---|
| Upstream repository | `https://github.com/eclipse-iceoryx/iceoryx2` | official Eclipse iceoryx2 repository; live refs rechecked 2026-09-22 |
| Production workspace at audit time | rbt workspace `0.3.0`, `iceoryx2 = "=0.9.3"` | `Cargo.toml:12-27`; production lockfile |
| Audited production upstream | tag `v0.9.3`, commit `6ef7c55176a8487ae7ca53c2e0649d5df9a1b3b4` | `target/iceoryx2-v0.9.3` |
| Latest official upstream | tag `v0.10.0`, commit `135d09dd8b29f321f1725920d434864c4e512378`, workspace/package `0.10.0`, Rust 1.89 | `target/iceoryx2-v0.10.0/Cargo.toml:126-172` |
| Current upstream main | commit `d32f60beedeef90ad480a3c7926c7211dd170512`, workspace/package `0.10.999`, Rust 1.89 | `target/iceoryx2-upstream-main/Cargo.toml:126-172` |
| Generic gateway packages | `iceoryx2-gateway` / `iceoryx2-link-gateway`: `0.10.0` in the release, `0.10.999` on main; no Zenoh backend | release/main package manifests; the shipped Zenoh path is the link tunnel |
| Zenoh integration | Zenoh `1.9.0` in the v0.10 release and main source trees | `target/iceoryx2-v0.10.0/integrations/zenoh/Cargo.toml:22-66` |
| Isolated probe | edition 2024, Rust 1.89, exact `iceoryx2 = "=0.10.0"`, own `Cargo.lock`, `#![forbid(unsafe_code)]` | `experiments/cross-host-upstream/Cargo.toml:1-13`; `src/lib.rs:1` |

The release checkout is dated 2026-09-18 and main 2026-09-22. The relevant
link and Zenoh source files are source-identical between those checkouts; the
post-release main changes through this SHA are package/version metadata and an
unrelated FlatBuffers publish-subscribe benchmark. The v0.10.0 release is the
runtime candidate. Current main received focused source tests/build coverage
only (71 tests/build checks, retained under
`target/cross-host-upstream-main-logs/upstream-main-20260922T-current`), not a
second namespace verifier run.

### Version capability matrix

The cells deliberately use `PASS`, `FAIL`, `NOT AVAILABLE`, or `NOT TESTED`.
`NOT TESTED` is not an inferred pass.

| Capability | Production 0.9.3 | Official v0.10.0 | Current main | Evidence/meaning |
|---|---|---|---|---|
| Fixed `u64`, Event, dynamic bytes | PASS | PASS | NOT TESTED | The accepted 0.9.3 spike passed; the v0.10 full runtime probe passed; main is source-identical but had no second namespace run. |
| 1080p Image and 20 MiB PointCloud | PASS | PASS | NOT TESTED | The accepted 0.9.3 spike and the v0.10 full runtime probe passed with checksums/layouts. |
| Remote provider withdrawal | NOT AVAILABLE | PASS | NOT TESTED | v0.9.3 has no equivalent modeled removal; full mirror absence was 108 ms. |
| Gateway/tunnel/network usable transition | NOT AVAILABLE | PASS | NOT TESTED | v0.10 runtime transitions passed; public cause/provider observability is limited. |
| Generic mirror lifecycle/replacement | NOT AVAILABLE | PASS | NOT TESTED | Selective removal and both gateway restart paths passed. |
| Logical cleanup under churn | FAIL | PASS | NOT TESTED | The accepted 0.9.3 spike retained unique services; v0.10 observed exact 100/1,000 lifecycle convergence and final absence. |
| Name filtering | NOT AVAILABLE | PASS | NOT TESTED | Exact payload/readiness and private-service cases passed. |
| Pure local reactive discovery | NOT AVAILABLE | FAIL | NOT TESTED | v0.9.3 reactive mode is unavailable; v0.10 pure reactive missed local create/drop. |
| Bounded Sidecar progress | FAIL | FAIL | NOT TESTED | The accepted 0.9.3 spike observed payload/readiness divergence; v0.10 still lacks a cross-channel progress guarantee and the modeled final-ready loss stranded one sample. |
| Controllable/lossless carrier queue | NOT AVAILABLE | FAIL | NOT TESTED | v0.10 carrier inbox is fixed at 64 and drops on full. |
| Fingerprint mismatch isolation | NOT AVAILABLE | PASS | NOT TESTED | Matching wire types and deliberate mismatch isolation passed. |
| Per-service QoS/settings preservation | NOT AVAILABLE | FAIL | NOT TESTED | Remote-only mirrors use receiver defaults; unchanged rbt explicit settings remain a blocker. |

## Topology and limitation

The verifier used one Linux machine with two nested network namespaces,
`hosta` and `hostb`, each with private mount, IPC, UTS, and `/tmp`/`/dev/shm`
state. A veth pair carried tested traffic: `10.77.62.1/24` to
`10.77.62.2/24`, MTU 1500 (`scripts/verify_cross_host_upstream.sh:268-300`).
Zenoh used peer mode with multicast scouting disabled and a configured TCP
listener/connect endpoint. This is two isolated Linux network/IPC stacks on one
physical host, not two physical hosts; it does not establish a WAN, physical
LAN, clock-synchronization, or failure-domain claim.
If a future upstream release clears the semantic gates, full acceptance must
then use two independent physical Linux hosts with real NICs and Ethernet/LAN;
this milestone makes no such performance or deployment claim.

## G1-G9 gate table

| Gate | Status | Runtime interpretation |
|---|---|---|
| G1 remote provider withdrawal | PASS | The provider withdrawal became mirror absence in 108 ms in the authoritative full run. |
| G2 gateway/network loss usable transition | PASS with observability caveat | Tunnel-loss withdrawal was 106 ms; network withdrawal was 9,983 ms and recovery 747 ms. Mirror absence was usable, but no public provider/health/classification API exists. |
| G3 local mirror lifecycle | PASS for generic services; settings blocker remains | Selective three-service removal, gateway A and B restarts, and replacement passed. Remote-only mirrors use receiver defaults, so unchanged rbt Sensor/Control settings are not preserved. |
| G4 churn logical cleanup | PASS | Same-name 100/100 and unique-name 1000/1000 completed with final absence and settled resources; the observer recorded exactly 2,001 unique transitions. |
| G5 filtering | PASS | Exact payload and `${service}/ready` filtering passed; three private services were absent, and payload-only filtering made readiness absent and observable. |
| G6 pure reactive local discovery | FAIL | A local create/drop did not wake a tunnel running only with `--reactive`; no poll means no local listing cycle. |
| G7 bounded Sidecar progress | FAIL | High-rate cases show severe wake/sample collapse but are not alone decisive because queue depth 1 may coalesce/drop old samples. The final trap models an independently lost last readiness and is decisive about the consequence: one valid payload remained queued with no future wake. |
| G8 carrier queue/backpressure | PARTIAL | The carrier inbox is fixed at 64, not configurable, and uses drop-on-full behavior; no controllable queue policy is exposed. |
| G9 compatible wire types and mismatch isolation | PASS with settings caveat | Matching wire types passed and fixed-vs-dynamic mismatch was rejected before the Image subscriber opened. This is not schema migration and does not repair settings loss. |

## Liveness timeline and Sensor partition

| Timeline event | Observed result |
|---|---|
| Provider held and killed | The remote mirror was present, then absent after 108 ms. |
| Tunnel A killed and restarted | Tunnel-loss withdrawal was 106 ms; the mirror returned after the scripted settle. |
| Host B tunnel restarted | The provider remained alive and the mirror returned after restart. |
| Network partition down/up | Withdrawal was 9,983 ms and recovery 747 ms; the down listing was empty and the up listing showed the service again within bounded waits. |
| Pure `--reactive`, no poll | The local service did not produce a discovery wake; this is G6. |
| Hybrid `--reactive --poll 1000` | Remote/data activity woke the tunnel and opened the Sidecar, but the subscriber saw only one of twenty samples. Polling is a workaround, not a progress contract. |

Provider-only timing can be stated as the required lifecycle sequence:

```text
T0  provider, both tunnels, and Host B mirror healthy
T1  kill provider P only; tunnel A remains alive
T2  link discovery no longer lists P locally (not separately timestamped)
T3  Zenoh liveliness Delete/removal propagates (not separately timestamped)
T4  Host B Service::list reports the mirror absent, T4-T1 = 108 ms
```

Tunnel death uses the same final observable transition and took 106 ms.
Partition withdrawal/recovery took 9,983/747 ms. During the partition the
original Host A provider stayed alive and its local health was not invalidated.

The public observer did not expose a publisher/provider count, an explicit
removal event, a provider-health object, or a cause classification. The usable
input was mirror presence/absence from official `Service::list`, not payload
silence. Internally, provider withdrawal becomes a Zenoh liveliness `Delete`;
the roughly ten-second partition decision is owned by Zenoh session/liveliness
behavior, not by an rbt timeout or heartbeat.

The dedicated Sensor partition sequence recorded an epoch delta of 5,019 ms,
last pre-partition sequence 40, last sent before rejoin 191, first post-rejoin
sequence 103, stale replay count 2, recovery gap 63, receiver 210/360,
two gap events, 150 estimated missing, final sequence 359, and monotonic
`PASS`. This is separate from the service-listing observer and the Sidecar
backlog diagnostic.

## Churn and resource observations

The official `Service::list` observer was used only for presence/absence and
did not drive Sidecar progress. Publisher counts were `unknown`.

| Run | Requested | Created/dropped | Observer result | Final state |
|---|---:|---:|---|---|
| Quick same-name | 5 | 5 / 5 | 11 transitions | absent |
| Full same-name | 100 | 100 / 100 | 201 transitions | absent |
| Full unique-name | 1000 | 1000 / 1000 | 2,001 transitions | absent |

The full unique observer used 50 ms hold/gap and captured the exact 2,001
appearance/absence transitions. Resource samples were:

| Tunnel | Before RSS/FD/tasks | Peak RSS/FD/tasks | Settled after RSS/FD/tasks | Reconnect RSS/FD/tasks |
|---|---:|---:|---:|---:|
| Host A | 36,068 KiB / 28 / 10 | 42,724 KiB / 40 / 8 | 42,692 KiB / 28 / 8 | 42,692 KiB / 28 / 8 |
| Host B | 36,292 KiB / 24 / 9 | 42,988 KiB / 34 / 7 | 42,980 KiB / 24 / 7 | 42,980 KiB / 24 / 7 |

The quick-churn peak was Host A 40,796 KiB / 28 FD / 10 tasks and Host B
40,260 KiB / 32 FD / 9 tasks. The exact logical counts, final absence, source
reconciliation, and settled snapshots support G4; these are diagnostics, not a
public leak counter or proof of per-service reclamation. Ten tunnel reconnect
cycles also completed without an orphan mirror or resource-count increase.

## Filtering and reactive execution

The v0.10 CLI accepts repeatable wildcard `--allow` patterns and applies them
to local exports and remote mirrors (`target/iceoryx2-v0.10.0/integrations/zenoh/link-tunnel-cli/src/cli.rs:33-42`; `target/iceoryx2-v0.10.0/iceoryx2-link/link/src/link.rs:80-85`). The exact allowlist exposed both the payload service and its `/ready` Event. Three private services (`private/test`, `private/internal`, and `private/cancellation`) were absent. The payload-only case kept payload observable while readiness was absent; the startup-only/partial filter was also recorded. Matching is case-sensitive, with `*` and `?` semantics (`target/iceoryx2-v0.10.0/iceoryx2-link/backend/src/allow_list.rs:16-20,39-43,53-69`).

Remote offer/data changes signal the backend wake, while local service listing
occurs only inside `Link::discover()` (`target/iceoryx2-v0.10.0/integrations/zenoh/link-carrier/src/offers.rs:160-199`; `target/iceoryx2-v0.10.0/integrations/zenoh/link-carrier/src/inbox.rs:19-25,60-70`; `target/iceoryx2-v0.10.0/iceoryx2-link/link/src/link.rs:98-135`). In the ten-second idle measurement, CPU ticks stayed quiet at A `2 -> 2` and B `2 -> 2`. Local-after-wait and preexisting fixed/Image cases failed discovery/data without poll, including the startup-only preexisting-service import. The hybrid poll path made remote activity observable but did not repair Sidecar progress.

| Reactive property | Result |
|---|---|
| Polling timer required | Yes for local lifecycle discovery; no for backend remote-offer/data wake |
| Idle CPU | Quiet over ten seconds: A `2 -> 2`, B `2 -> 2` ticks |
| Tunnel tasks | A `10 -> 8`, B `9 -> 7` during the idle observation |
| Data wake | Remote carrier frames wake the link |
| Discovery wake | Remote offers wake; local create/preexisting discovery does not |
| Removal wake | Remote liveliness removal wakes/reconciles; local removal has no dedicated wake |

## Sidecar progress evidence

The probe uses a separate `${service}/ready` Event and an event-driven
subscriber. It drains payload only after a readiness wake; direct receive after
the event-driven phase is diagnostic only. Payload length is configurable,
including 20 MiB, and the publisher supports readiness prefill and final-ready
suppression (`experiments/cross-host-upstream/src/lib.rs:1544-1648,1721-1810`).

| Case | Payload | Publisher result | Subscriber event-driven result |
|---|---:|---|---|
| Basic | 64 B | `payload_ok=20/20`, `readiness_ok=20` | `wakes/drains=1/1`, `samples=1/20`, `spurious_wakes=0`, `remaining_after_last_wake=0` |
| High-rate | 64 B | `payload_ok=10000/10000`, `readiness_ok=10000` | `wakes/drains=2/2`, `samples=2/10000`, `spurious_wakes=0`, `remaining_after_last_wake=0` |
| Final-ready trap | 20 MiB | `payload_ok=2/2`, `readiness_ok=1`, `suppress_final_ready=true` | `wakes/drains=1/1`, `samples=1/2`, `spurious_wakes=0`, `remaining_after_last_wake=1`, `final_observed=0` |
| Latest/backlog diagnostic | 1024 B | `payload_ok=200/200`, `readiness_ok=200` | `wakes/drains=13/13`, `samples=13/200`, `spurious_wakes=0`, `remaining_after_last_wake=0` |
| Readiness-prefill gap | 64 B | `payload_ok=1/1`, `ready_prefill=1`, `readiness_ok=2` | `wakes/drains=2/2`, `samples=1/1`, `spurious_wakes=1`, `remaining_after_last_wake=0` |

The final trap models the consequence of an independently lost final
readiness: the large final payload remained after the last wake, and the
separate fixed 64-entry Event inbox has source-level drop-on-full behavior.
All five publisher summaries reported `payload_errors=0` and
`readiness_errors=0`; the final trap intentionally omitted, rather than
attempted and failed, its last readiness notification.
The high-rate wake count by itself is not a failure because readiness may
coalesce; the missing cross-channel progress guarantee plus this final state is
the G7 failure. The separate quick readiness-prefill gap also passed
the spurious-wake safety check: `PASS sidecar-spurious-wake samples=1
spurious_wakes=1`. One readiness wake was spurious, and the single payload was
still received event-driven after a two-second payload gap. This proves
`wake -> no payload` is safe; it does not change G7 because the final trap still
strands one payload.

## Payload and layout results

| Payload | Result | Retained evidence |
|---|---|---|
| Fixed `u64` | PASS, 8/8 exact sequence 0..7, checksum 28 | `plain-u64-publisher.log`, `plain-u64-subscriber.log` |
| Plain Event | PASS, 7/7 IDs 1..7, checksum 28 | `plain-event-publisher.log`, `plain-event-subscriber.log` |
| Dynamic `[u8]` | PASS, 3/3 at 1,024 bytes, checksum 391,680 | `plain-bytes-low-rate-publisher.log`, `plain-bytes-low-rate-subscriber.log` |
| 1080p Image | PASS, 3/3; 1920x1080 RGB8, 6,220,800 bytes, checksum 2,379,456,000 | `image-1080p-*.log` |
| 20 MiB PointCloud | PASS, 1/1; 1,310,720 `PointXYZI` elements, 20,971,520 bytes, checksum 12,562,007,357,776,199,680 | `pointcloud-20m-*.log` |

The isolated probe defines local `repr(C)` Image and PointCloud header/element
types with the exact rbt names/layouts, derives `ZeroCopySend`, and includes
checksum/layout/parser unit tests. Runtime transport proof and source/layout
tests are distinct claims.

## Schema mismatch and imported settings

The same-name mismatch started a `[u8]` publisher with a 1,024-byte payload and
an Image subscriber. The publisher completed while the Image subscriber
reported `PublishSubscribeOpenError::DoesNotExist`; the gate logged
`subscriber_status=1 publisher_status=0`. Fingerprinted offers and channels
therefore isolate incompatible types (`target/iceoryx2-v0.10.0/integrations/zenoh/link-carrier/src/keys.rs:19-55`; `.../src/fingerprint.rs:62-96`). This is mismatch isolation, not schema migration.

The upstream fingerprint does not replace rbt's authoritative compatibility
tuple: namespace, name, `abi_major`, `layout_hash`, and `TransportFeatures`.
No compatibility transform or schema-evolution mechanism was added.

The v0.10 `ServiceDescriptor` contains name, hash, and types and explicitly
allows equal descriptors to exchange data regardless of settings
(`target/iceoryx2-v0.10.0/iceoryx2-link/backend/src/service_description/descriptor.rs:19-37`). Descriptor conversion omits `ServiceDescription.settings`, and the resolver creates a remote-only mirror with local defaults (`target/iceoryx2-v0.10.0/iceoryx2-link/backend/src/service_description/mod.rs:66-80`; `target/iceoryx2-v0.10.0/iceoryx2-link/tunnel/src/resolver.rs:36-43,81-92`). The upstream conformance test asserts those local mirror defaults (`target/iceoryx2-v0.10.0/iceoryx2-link/conformance-tests/src/tunnel/publish_subscribe.rs:36-70`).

Production rbt binds QoS to `StreamProfile`: Sensor uses queue depth 1, history
0, and safe overflow, while Control has a separate profile
(`crates/rbt-iox2/src/lib.rs:90-111,240-241`). Production subscribers reapply
those settings before `.open()` (`crates/rbt-iox2/src/lib.rs:429-460,784-815`).
The probe intentionally omits service settings on imported subscriber/listener
opens (`experiments/cross-host-upstream/src/lib.rs:858-881,1235-1253,1403-1422,1652-1693`). A typed mirror can therefore be visible while an unchanged rbt subscriber insisting on explicit Sensor/Control settings fails to open. This is the central G3 and G9 compatibility blocker.

## Queue model and loss boundary

```text
local iceoryx2 publisher
  -> local tunnel bridge
  -> Zenoh Reliable publisher
  -> remote Zenoh subscriber
  -> flume::bounded(64) carrier Inbox
       try_send; full arrivals are dropped with a warning
  -> remote tunnel bridge
  -> imported iceoryx2 service
  -> rbt subscriber
```

The carrier capacity is `CAPACITY: usize = 64`; its subscriber is declared
`Reliable`, but the local Inbox is `flume::bounded(64)` and `try_send` drops on
full (`target/iceoryx2-v0.10.0/integrations/zenoh/link-carrier/src/channel.rs:20-27,69-89`; `.../src/inbox.rs:19-31,60-70`). PublishSubscribe and Event have separate channel/bridge paths; propagation orders samples before notifications but is not transactional (`target/iceoryx2-v0.10.0/iceoryx2-link/link/src/bridge/mod.rs:190-223`). The capacity is a source constant, not a CLI setting. The probe's local Sidecar queue depth 1/safe-overflow settings do not change the independent carrier inbox.

At overload, carrier enqueue is nonblocking: it neither blocks the Zenoh
callback nor backpressures the sender, and it does not retry internally. A
full inbox emits a warning and drops the frame; a successful local
`Notifier::notify` therefore provides no typed remote-delivery error. Separate
payload/Event queues may drop independently.

No custom queue, serialization, heartbeat, lease, retransmission, network
socket, or custom cross-host protocol was added.

## Implementation audit

| API/type | Semantics exercised | Zenoh/CLI/library exposure | Tests/evidence |
|---|---|---|---|
| Fixed `u64`, Event, `[u8]` | Fixed samples, separate Event notifications, dynamic byte length/checksum | Probe commands use official iceoryx2 0.10 ports; link tunnel carries upstream Frames | Full fixed/Event/bytes runtime PASS; parser/checksum tests |
| `ImageHeader`/`PointCloudHeader`/`PointXYZI` | Local `repr(C)` exact names/layouts and dynamic slices | Same upstream PublishSubscribe path; no production type dependency | Layout/checksum unit tests; full Image and 20 MiB PointCloud PASS |
| `provider-hold`, `service-observe` | Bounded provider lifetime and listing-only presence transitions | Official `Service::list`; observer never drives Sidecar progress | Provider, tunnel, network, replacement, and churn evidence |
| Sidecar payload plus `/ready` | Readiness-driven drain, prefill, configurable bytes, final suppression | Separate PublishSubscribe and Event bridges | Basic/high/final/latest summaries; G7 remains FAIL |
| `churn-same`, `churn-unique` | Repeated create/drop and final absence | Same upstream service APIs; no protocol overlay | Exact 100 and 1000 counts, 2,001 transitions, resource snapshots |
| Allowlist and fingerprints | Name filtering and mismatch isolation | Official `--allow`, fingerprinted upstream keys | Three private services absent; mismatch gate PASS |
| Reactive/partition observation | Remote/data wake, local idle limitation, Sensor sequence gaps | Official `--reactive`/poll and link listing | Idle CPU quiet; G6 FAIL; Sensor monotonic PASS |

## Thin proxy viability

No thin proxy or Composer outage choreography was built. The verifier records
`SKIP thin-proxy-composer` because G6 and G7 are not proven. The public
`Link::bridges()` surface reports whether a service hash is bridged but does not
provide provider identity, health, queue state, or a reason for a missing sample
(`target/iceoryx2-v0.10.0/iceoryx2-link/link/src/link.rs:212-215`;
`.../src/bridge/mod.rs:180-188`). A proxy would add a lifecycle/queue contract
and would still need explicit settings mapping, so it is not justified by this
evidence.
Any eventual proxy must also allocate a local `CapabilityInstanceId` distinct
from the remote identity retained only as provenance; this milestone adds no
identity plumbing.

## Production baseline, layout, unsafe boundary, and checks at audit time

Unchanged production hashes from the full verifier:

| File | SHA-256 |
|---|---|
| `Cargo.toml` | `048e18c818da430d6fdcfc708a719404012c0cd307fbc2d6ecf75921c8dbf632` |
| `Cargo.lock` | `425f2842746bb287117b9d799f14098b0483f6211089cbf97f5f8b0d53dfea30` |
| `crates/rbt-types/src/lib.rs` | `46fe9267c1881bfbfee8c835138f081d58e0fb4a6ac44422b3188a2d52ff69e8` |
| `crates/rbt-iox2/src/lib.rs` | `68f15f03a797e91e931a5d8424b3db0259d78a0443220acb895e29ce182f3010` |

Frozen layout hashes remain Image `943fa4008cf0f55f89a281b051c69a6f`,
PointCloud `d9da2bb29bb2cf217c40f1a56d1fb755`, and ControlCommand
`9381cf15842a48ac0964111f0f45ef72`. The production Rust unsafe audit remains
`49 -> 49`, delta 0 when applying the accepted production-runtime audit
boundary (excluding generator source and test-only sources); the isolated
probe forbids unsafe code.

The current isolated probe checks passed in the latest quick run; the same core
checks also passed in the strict full run before the spurious-wake control was
added:

```text
cargo fmt --manifest-path experiments/cross-host-upstream/Cargo.toml -- --check
cargo test --manifest-path experiments/cross-host-upstream/Cargo.toml --locked
# 4 unit tests passed; 0 failed
cargo clippy --manifest-path experiments/cross-host-upstream/Cargo.toml --locked --all-targets -- -D warnings
cargo build --manifest-path experiments/cross-host-upstream/Cargo.toml --locked
```

Production `cargo test --workspace --locked` also passed with the Phase4 linker;
the retained log is `target/cross-host-upstream-production-test-phase4.log`.
Production fmt/clippy passed as previously recorded. No production upgrade,
custom transport, public API change, generated layout change, or hash change
was made.

Production files changed: **none**. Milestone-owned files are the standalone
`experiments/cross-host-upstream/` workspace and Zenoh configs,
`scripts/verify_cross_host_upstream.sh`,
`docs/cross-host-upstream-notes.md`, and this report.

## Final acceptance checklist

1. **What exact upstream commit was tested?** Runtime: released v0.10.0 at `135d09dd8b29f321f1725920d434864c4e512378`. Current-main source/build comparison: `d32f60beedeef90ad480a3c7926c7211dd170512`.
2. **Is there a newer official release than 0.9.3?** Yes. v0.10.0 is the latest official tag and GitHub marks it pre-release.
3. **Can remote provider death be observed on Host B?** Yes. Provider-only death became mirror absence in 108 ms while tunnel A stayed alive.
4. **Does the mirrored service actually disappear?** Yes. Official `Service::list` reported it absent.
5. **Does gateway death become observable remotely?** Yes. Tunnel-A death became mirror absence in 106 ms, without a public cause classification.
6. **Does network partition become observable?** Yes. Mirror withdrawal took 9,983 ms and recovery 747 ms.
7. **Is the signal authoritative enough to revoke a proxy capability?** Yes for generic service availability: it is Zenoh-liveliness-backed mirror absence, not payload silence or an rbt heartbeat. It is not a complete production proxy contract because provider identity/cause, settings, G6, G7, and G8 remain unresolved.
8. **Does provider replacement converge correctly?** Yes. Same-name removal/reappearance and tunnel A/B restart paths returned one current mirror with no observed duplicate.
9. **Does repeated churn leave stale mirrors?** No. The 100 same-name and 1,000 unique-name runs ended absent with exact 201/2,001 transitions.
10. **Does repeated churn leave stale relays?** No logically permanent relay was observed, and source reconciliation removes unseen bridges; upstream exposes no public relay-count API, so this is logical evidence rather than a direct counter.
11. **Does resource use settle after churn?** Yes operationally: FD/tasks returned to stable values and RSS plateaued after allocator growth; byte-perfect RSS return was not required.
12. **Can services be explicitly allowlisted?** Yes. Repeatable startup-time `--allow` filters passed.
13. **Can private services remain local?** Yes. All three private services were absent remotely.
14. **Does reactive Gateway mode actually work?** No, not for the complete contract. Remote offer/data wakes work, but pure reactive local lifecycle discovery does not.
15. **Is idle behavior event-driven?** Yes. Pure-reactive idle CPU ticks stayed A `2 -> 2` and B `2 -> 2` over ten seconds.
16. **Does discovery work in reactive mode?** Remote-offer discovery does; local service creation and even a preexisting local service did not progress without maintenance polling, so G6 is FAIL.
17. **Does withdrawal work in reactive mode?** Remote offer withdrawal wakes and reconciles; purely local withdrawal lacks its own wake, so the complete reactive lifecycle contract fails.
18. **Does Event progress remain reliable under overload?** No sufficient cross-channel progress guarantee exists. The high-rate run's two wakes for 10,000 notifications are not alone a failure because wakes may coalesce, but the separate fixed Event inbox may drop on full and has no coupling to payload progress.
19. **Can a final payload become stranded with no wake?** Yes. When the independently lost final readiness was modeled by suppression, the diagnostic left one 20 MiB payload queued after the last wake and no future message.
20. **Is SidecarV1 sufficient without payload polling?** No. The event-driven receiver can remain asleep with payload available.
21. **Are queues/buffers bounded or controllable?** Partially: the carrier ingress is bounded at 64, but fixed, not operationally configurable, and drops on full.
22. **Is stale Sensor backlog bounded after reconnect?** Yes in this diagnostic: only two stale frames crossed after recovery, the first recovered sequence jumped from 40 to 103, and delivery remained monotonic to 359.
23. **Does service-description mismatch prevent accidental exchange?** Yes. Fingerprinted channels isolated the incompatible same-name types.
24. **Does existing dynamic payload transport still work?** Yes. Dynamic `[u8]` passed 3/3 at 1,024 bytes.
25. **Does 1080p Image still work?** Yes, 3/3 at 6,220,800 bytes.
26. **Does 20 MiB PointCloud still work?** Yes, 1/1 at 20,971,520 bytes.
27. **Is a thin rbt proxy now semantically viable?** No. G6, G7, G8, settings preservation, and public identity/health detail remain insufficient.
28. **Does this capability exist in a released version?** The partial fixes do exist in official v0.10.0; the complete G1-G9 capability set does not.
29. **Was production rbt still pinned to 0.9.3 at the time of this audit?** Yes. The audit snapshot's production manifests/lockfile and hashes were unchanged.
30. **What did the report recommend as the next step at the time?** Continue waiting on upstream semantics. Do not start production migration and do not build a custom transport.

Recommendation:
C — PARTIALLY FIXED
At audit time, the smallest justified next milestone was to retest a released
Zenoh integration only after it added local lifecycle wake, a Sidecar-safe
progress contract, controllable queue policy, and preservation or explicit
mapping of per-service settings. Until then, the report recommended keeping
the then-current 0.9.3 production baseline and not building a custom transport.

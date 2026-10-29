# iceoryx2 upstream cross-host Zenoh source audit

> Historical production baseline: references to production `0.9.3` below
> describe the pre-unification source and runtime snapshot. Production now uses
> maintained iceoryx2 `0.10.0-rbt.2`; the isolated upstream `v0.10.0`
> findings remain candidate evidence, not production acceptance. See the
> [unified baseline report](iceoryx2-unified-report.md).

## Scope and evidence labels

This note audits the exact local iceoryx2 checkouts used for the isolated
upstream experiment. Paths below are repository-relative to `/home/sw/rbt`
unless they begin with `/home/sw/rbt` explicitly.

- **Source-backed** means the behavior is visible in the cited source.
- **Runtime-backed** means a retained experiment log demonstrated it.
- **Source-only** means the cited source or upstream conformance tests establish
  a capability, but the retained two-network-stack run did not exercise that
  exact product contract.

This note combines the implementation/source audit with the retained upstream
runtime evidence. The pre-unification production 0.9.3 runtime evidence is
retained in `docs/cross-host-iox2-notes.md`; it must not be silently reused as
v0.10.0 evidence. Runtime status is summarized from the quick and full runs
below.

## Exact upstream artifacts

At the audit date (2026-09-22), the latest release tag is `v0.10.0`, commit
`135d09dd8b29f321f1725920d434864c4e512378`, dated 2026-09-18. The GitHub
release is marked pre-release. The current upstream checkout is commit
`d32f60beedeef90ad480a3c7926c7211dd170512`, dated 2026-09-22, and reports
workspace version `0.10.999`. The relevant link and Zenoh source files are
identical between these two checkouts; the diff is package/version metadata and
unrelated benchmark/workspace metadata. The exact manifests are:

- v0.10.0: `target/iceoryx2-v0.10.0/Cargo.toml:126-176` and
  `target/iceoryx2-v0.10.0/integrations/zenoh/Cargo.toml:22-66`;
- main: `target/iceoryx2-upstream-main/Cargo.toml:126-176` and
  `target/iceoryx2-upstream-main/integrations/zenoh/Cargo.toml:22-66`.

Both require Rust 1.89 and the Zenoh integration uses Zenoh 1.9.0. The release
notes explicitly list remote-service removal, remote-host association,
allowlists, reactive execution, the event API/delivery changes, and
description fingerprints (`target/iceoryx2-v0.10.0/doc/release-notes/iceoryx2-v0.10.0.md:16-34`).

At the time of this audit, production used `iceoryx2 = "=0.9.3"` in
`Cargo.toml:19-27`; its audited checkout was tag `v0.9.3`, commit
`6ef7c55176a8487ae7ca53c2e0649d5df9a1b3b4`. Its Zenoh artifacts were the
`iceoryx2-services` tunnel plus `iox2-tunnel-zenoh`; the v0.10 artifacts are
`iceoryx2-link`, `iceoryx2-link-tunnel`,
`iceoryx2-integrations-zenoh-link-carrier`, and binary
`iox2-link-tunnel-zenoh`. The upstream release notes record this rename and
architecture transition (`target/iceoryx2-v0.10.0/doc/release-notes/iceoryx2-v0.10.0.md:184-207`).

## The actual Zenoh executable, and the gateway distinction

The Zenoh command is `iox2 link tunnel zenoh`, described as a tunnel between
iceoryx2 systems (`target/iceoryx2-v0.10.0/integrations/zenoh/link-tunnel-cli/src/cli.rs:15-22`).
It constructs `Link<ipc::Service, Tunnel<ZenohCarrier>>`, optionally applies
the link filter, and runs `discover()` then `propagate()` (`target/iceoryx2-v0.10.0/integrations/zenoh/link-tunnel-cli/src/tunnel.rs:23-69`).

The new generic `iceoryx2-gateway` package is a trait/type layer for gateway
backends (`target/iceoryx2-v0.10.0/iceoryx2-gateway/gateway/Cargo.toml:1-29`),
and `iceoryx2-link-gateway` adapts a middleware through an adapter; its Event
relay is explicitly unsupported (`target/iceoryx2-v0.10.0/iceoryx2-link/gateway/src/gateway.rs:28-33,49-70`).
The shipped gateway CLI is the ROS 2 integration and constructs a
`Ros2Backend`, not a Zenoh backend (`target/iceoryx2-v0.10.0/integrations/ros2/gateway-cli/src/main.rs:32-44`).
There is no new `iceoryx2-gateway` Zenoh backend or Zenoh gateway CLI in
v0.10.0/main. Conflating that generic/ROS 2 gateway work with the Zenoh link
tunnel would overstate the upstream capability.

## Candidate capability matrix

| Capability | Production 0.9.3 | v0.10.0 / current main source candidate | Surface, tests, and exact caveat |
|---|---|---|---|
| Remote service removal / peer liveness | No modeled remote removal. The old discovery query only sends service details and processes them; the old tunnel keeps persistent port/relay maps (`target/iceoryx2-v0.9.3/integrations/zenoh/tunnel-backend/src/discovery.rs:107-200`; `target/iceoryx2-v0.9.3/iceoryx2-services/tunnel/src/tunnel.rs:201-282`). | **Implemented in the source path.** A service offer is a Zenoh liveliness token; `Delete` removes the peer, advances generation, and wakes the link (`target/iceoryx2-v0.10.0/integrations/zenoh/link-carrier/src/offers.rs:75-91,160-200`). Unseen remote descriptions are pruned and bridge tables retain only the current epoch (`target/iceoryx2-v0.10.0/iceoryx2-link/link/src/discovery_state.rs:373-393`; `target/iceoryx2-v0.10.0/iceoryx2-link/link/src/bridge/mod.rs:132-178`). | Conformance includes withdrawn-offer and withdrawn-service cases (`target/iceoryx2-v0.10.0/iceoryx2-link/conformance-tests/src/carrier/discovery.rs:118-153`; `.../src/tunnel/discovery.rs:158-255`) and Zenoh instantiates carrier/link discovery tests (`target/iceoryx2-v0.10.0/integrations/zenoh/link-carrier/tests/zenoh_carrier_tests.rs:166-205`). They are not physical-host acceptance. Runtime-backed G1 PASS: provider withdrawal became mirror absence in 108 ms. Runtime-backed G2 PASS with observability caveat: tunnel/network loss became usable absence/recovery, but the public API has no provider/health/classification surface. |
| Provider/remote-host association | The old backend trait lists `StaticConfig`-like services without a remote identity (`target/iceoryx2-v0.9.3/iceoryx2-services/tunnel-backend/src/traits/backend.rs:85-121`). | **Partial source support.** `Offer` carries `peer` plus descriptor and `OfferId` carries `peer` plus service hash (`target/iceoryx2-v0.10.0/iceoryx2-link/carrier/src/offer.rs:13-22`; `.../tunnel/src/offer_id.rs:13-35`). Offers are grouped by `(hash,fingerprint)` and may have multiple peers (`target/iceoryx2-v0.10.0/integrations/zenoh/link-carrier/src/offers.rs:46-72,119-131`). | The internal resolver receives peer-keyed remote IDs, but it imports the first unanimous descriptor and does not retain a public provider object (`target/iceoryx2-v0.10.0/iceoryx2-link/tunnel/src/resolver.rs:53-93`). Public `Link::bridges()` exposes only `contains`/`empty` (`target/iceoryx2-v0.10.0/iceoryx2-link/link/src/link.rs:212-215`; `.../bridge/mod.rs:180-188`). No public per-provider health/counter/association API or CLI health report is present. Runtime-backed G2 PASS with observability caveat: `service-observe` reports usable presence/absence, not provider identity or health. |
| Service allowlist | No name/namespace allowlist in the old tunnel CLI; its options are config path, discovery service, poll, and reactive (`target/iceoryx2-v0.9.3/integrations/zenoh/tunnel-cli/src/cli.rs:26-55`). | **Implemented.** CLI `--allow` is repeatable; `with_filter` applies the predicate to both local exports and remote mirrors (`target/iceoryx2-v0.10.0/integrations/zenoh/link-tunnel-cli/src/cli.rs:33-42`; `.../src/tunnel.rs:53-60`; `target/iceoryx2-v0.10.0/iceoryx2-link/link/src/link.rs:80-85,161-175`). | Matching is case-sensitive; `*` spans zero or more characters and `?` exactly one (`target/iceoryx2-v0.10.0/iceoryx2-link/backend/src/allow_list.rs:16-20,39-43,53-69`). Unit coverage exists. Runtime-backed G5 PASS: payload and `/ready` were allowed, three private services were absent, and payload-only made readiness absent and observable. |
| Event delivery | The old tunnel has a separate Event relay and separate propagation loop; old CLI reactive mode is `unimplemented!` (`target/iceoryx2-v0.9.3/iceoryx2-services/tunnel/src/tunnel.rs:247-273`; `target/iceoryx2-v0.9.3/integrations/zenoh/tunnel-cli/src/main.rs:59-83`). | The release includes the new event API/delivery behavior, and link propagation drains PublishSubscribe before Event (`target/iceoryx2-v0.10.0/iceoryx2-link/link/src/bridge/mod.rs:190-223`; local Notifier API at `target/iceoryx2-v0.10.0/iceoryx2/src/port/notifier.rs:101-127,515-585`). | Zenoh still carries payload and Event frames on separate channels; each is independently bounded and delivered by separate bridges (`target/iceoryx2-v0.10.0/iceoryx2-link/link/src/bridge/publish_subscribe.rs:67-85`; `.../src/bridge/event.rs:67-88`). The source does not establish an atomic payload/Event transaction or a cross-channel progress guarantee. Runtime-backed: plain Event is PASS, but Sidecar progress is not a transaction or a universal progress guarantee. |
| Reactive execution | CLI advertises `--reactive` but immediately panics; `--poll` conflicts with it (`target/iceoryx2-v0.9.3/integrations/zenoh/tunnel-cli/src/cli.rs:42-55`; `.../src/main.rs:61-66`). | **Implemented for remote/data wake sources.** Zenoh `OfferTracker` wakes on offer changes and `Inbox` wakes on sample arrival (`target/iceoryx2-v0.10.0/integrations/zenoh/link-carrier/src/offers.rs:160-165,194-200`; `.../src/inbox.rs:19-25,60-70`). CLI supports `--reactive`, optional `--listener`, and optional `--poll` (`target/iceoryx2-v0.10.0/integrations/zenoh/link-tunnel-cli/src/cli.rs:44-67`; `.../src/wake.rs:34-68`). | Pure reactive mode does not wake for a local service create/drop: local listing is read only inside `Link::discover()` (`target/iceoryx2-v0.10.0/iceoryx2-link/link/src/link.rs:98-135`), while the CLI waitset invokes `spin()` only after a selected wake (`target/iceoryx2-v0.10.0/integrations/zenoh/link-tunnel-cli/src/main.rs:57-75`). A low-rate maintenance `--poll` can cover this limitation. Runtime-backed G6 FAIL: pure reactive local discovery did not wake; a 1000 ms maintenance poll made the hybrid transport observable, but did not repair Sidecar progress. |
| Description fingerprint / schema isolation | Old Zenoh keys use only service hash; no description fingerprint (`target/iceoryx2-v0.9.3/integrations/zenoh/tunnel-backend/src/keys.rs:15-33`). | **Implemented.** Offers use `iox2/v1/offer/{service_hash}/{fingerprint}/{peer_id}` and data uses `iox2/v1/channel/{service_hash}/{fingerprint}` (`target/iceoryx2-v0.10.0/integrations/zenoh/link-carrier/src/keys.rs:19-55`). Descriptors are postcard-encoded and SHA-1 fingerprinted; received bytes must hash to the key fingerprint before decoding (`target/iceoryx2-v0.10.0/integrations/zenoh/link-carrier/src/fingerprint.rs:62-96`). | Differing types are warned/refused or isolated by resolver/channel selection (`target/iceoryx2-v0.10.0/iceoryx2-link/tunnel/src/resolver.rs:69-92`). This protects against accidental cross-type delivery; it is not a schema migration or application compatibility protocol. Runtime-backed G9 PASS for matching wire types and deliberate mismatch isolation, with the imported-settings caveat below. |
| Queue/backpressure and loss | Old tunnel/Zenoh FIFOs and local queues had no tunnel-level end-to-end delivery contract; prior runtime pressure evidence is documented separately. | Zenoh publisher is `Reliable`, but the carrier subscriber's local `Inbox` is `flume::bounded(64)` (`target/iceoryx2-v0.10.0/integrations/zenoh/link-carrier/src/channel.rs:20-27,69-89`). `try_send` drops arrivals with a warning when full (`.../src/inbox.rs:53-70`). `receive()` pops one queued frame per call (`.../src/channel.rs:96-115`). | Payload and Event each have their own 64-frame ingress queue, and the tunnel drains them in separate bridges. Reliable transport does not convert a full application-side ingress inbox into lossless or transactional semantics. Runtime-backed G8 PARTIAL: the fixed carrier capacity is not CLI-configurable and drop-on-full is observable in Sidecar diagnostics. |
| Public observability | No remote provider/liveness API. | `Link::bridges()` only reports whether a service hash is bridged; `with_monitoring()` emits per-bridge moved counts at trace level (`target/iceoryx2-v0.10.0/iceoryx2-link/link/src/link.rs:87-90,212-215`; `target/iceoryx2-v0.10.0/iceoryx2-link/link/src/bridge/table.rs:134-165`). | Internal `OfferId` has peer identity, but no public mapping from a mirror/bridge to its provider, no remote-health state, and no CLI command exposing this association. A thin proxy must therefore use application-level metadata and maintenance rather than infer remote health from mirror existence. |

## Wire path and exact semantics

The v0.10 Zenoh carrier uses a liveliness token and queryable per offered
service. A peer subscribes to `iox2/v1/offer/*/*/*`; a `Put` records an offer,
queries its descriptor, and a `Delete` withdraws it. The descriptor is not a
free-form application protocol: it is the upstream `ServiceDescriptor` encoded
with postcard and keyed by its SHA-1 fingerprint. Data and notifications are
opaque upstream `Frame` bytes on the fingerprinted channel key
(`target/iceoryx2-v0.10.0/integrations/zenoh/link-carrier/src/carrier.rs:136-210`; `.../src/keys.rs:25-55`; `.../src/fingerprint.rs:62-96`).
The experiment adds no custom cross-host protocol and does not change these
upstream keys or wire types.

The relevant data path is:

```text
local iceoryx2 ports
  -> Link::propagate()
  -> Zenoh Reliable publisher
  -> remote Zenoh subscriber
  -> 64-item flume Inbox (try_send; drops when full)
  -> remote Link bridge
  -> remote iceoryx2 ports
```

`Bridges::propagate()` processes PublishSubscribe before Event, but those are
still separate channels and separate local queues (`target/iceoryx2-v0.10.0/iceoryx2-link/link/src/bridge/mod.rs:190-223`). No source guarantee couples “latest payload” to “ready Event”, and no source guarantee says a bounded inbox eventually drains under a producer that outruns the tunnel.

## Reactive and liveness boundary

The reactive wake is a backend wake, not a local service lifecycle watcher.
Remote offer changes and arriving channel samples signal it, but local service
creation/removal is only observed when `Link::discover()` lists local services
(`target/iceoryx2-v0.10.0/iceoryx2-link/link/src/link.rs:113-135`; `target/iceoryx2-v0.10.0/integrations/zenoh/link-carrier/src/offers.rs:160-165`; `.../src/inbox.rs:60-70`). With `--reactive` and no `--poll`, the CLI can therefore sleep indefinitely after a purely local change. A documented low-frequency poll is a viable maintenance workaround, but it is not pure-reactive local lifecycle notification.

Remote withdrawal is better modeled than in 0.9.3: Zenoh session/liveliness
deletion removes the peer's offer, generation changes, `Link::discover()`
resolves the service again, and `Bridges::reconcile()` closes bridges not seen
in the current epoch. This is source-backed; timing, behavior through a network
partition, and whether cleanup meets the product SLA are runtime questions.

## Security and trust boundary

The Zenoh tunnel README says the joined systems and network are trusted and
labels the link a prototype intended for development experimentation
(`target/iceoryx2-v0.10.0/integrations/zenoh/link-tunnel-cli/README.md:3-13`).
The CLI accepts an external Zenoh config or uses `zenoh::Config::default()`
(`target/iceoryx2-v0.10.0/integrations/zenoh/link-tunnel-cli/src/tunnel.rs:72-85`).
The integration enables authentication/TLS-related Zenoh features but supplies
no credentials or certificates itself (`target/iceoryx2-v0.10.0/integrations/zenoh/Cargo.toml:51-66`). Consequently, authentication, authorization, TLS, peer discovery mode, and transport isolation are deployment-configured rather than guarantees of the tunnel. The default configuration must be treated as trusted-network development behavior, not a production security boundary.

## Production isolation and test boundary

The experiment is a standalone workspace with its own lockfile and an exact
`iceoryx2 = "=0.10.0"` dependency
(`experiments/cross-host-upstream/Cargo.toml:1-13`). It was not a member of the
production workspace at audit time, when that workspace used 0.9.3. No
production crate, public API, generated hash, or custom wire format was changed
by this source audit. The two-stack verifier and its retained logs are the
proper place for runtime evidence; upstream unit/conformance tests are useful
regression evidence but do not prove two physical hosts or a product-level
liveness/Sidecar contract. The isolated probe checks completed with
`cargo fmt --check`, `cargo test --locked` (four unit tests),
`cargo clippy --locked --all-targets -- -D warnings`, and `cargo build --locked`.
Production `cargo test --workspace --locked` also passed with the Phase4 linker
(`target/cross-host-upstream-production-test-phase4.log`); production fmt and
clippy passed as previously recorded. Current main had focused 71-test/build
coverage at the exact SHA above
(`target/cross-host-upstream-main-logs/upstream-main-20260922T-current`), not a
second namespace runtime acceptance.

## Runtime gate summary

The retained evidence roots are:

- quick: `target/cross-host-upstream-verify-logs/upstream-quick-20260922T104504Z-28629`;
- full: `target/cross-host-upstream-verify-logs/upstream-full-20260922T055557Z-67857`.

The strict full run's metadata captured the then-local main checkout at
`671932de95ceddf5803e95f8884b889d28158a3c`; main was metadata-only in that
release-runtime run. The final live ref refresh advanced the isolated main
checkout to `d32f60beedeef90ad480a3c7926c7211dd170512`. The latest quick metadata
and the focused 71-test/build evidence use that current SHA; no link, gateway,
Zenoh source, or test file changed between these main revisions.

The latest quick verifier retained its exact outer result:
`execution=PASS`, `critical_gates=PARTIAL`, and
`product_unchanged=PASS`. The strict full run's retained `gates.log` ends with
`CROSS_HOST_NAMESPACE_RESULT mode=full transport=PASS
known_limitations_logged=1`; its terminal-only outer result line was not
retained in the run directory. All full-run processes exited, no leftover log
was produced, and the production hashes were rechecked unchanged. The gate
status is:

| Gate | Runtime result | Evidence |
|---|---|---|
| G1 remote provider withdrawal | PASS | provider mirror absence was observed in 108 ms in the full run |
| G2 gateway/network loss usable transition | PASS with observability caveat | tunnel loss was 106 ms; network withdrawal was 9,983 ms and recovery 747 ms; no public provider/health/classification API exists |
| G3 local mirror lifecycle | PASS for generic services; settings blocker remains | selective removal, gateway A/B restart, and replacement passed; receiver-default settings do not preserve unchanged rbt Sensor/Control settings |
| G4 churn logical cleanup | PASS | same-name 100/100 and unique-name 1000/1000 passed; exact unique observer result was 2,001 transitions and final absence |
| G5 filtering | PASS | payload and `/ready` were allowed, three private services were absent, and payload-only made readiness absent and observable |
| G6 pure local reactive discovery | FAIL | local create/drop did not wake a pure `--reactive` tunnel without `--poll` |
| G7 bounded Sidecar progress | FAIL | high-rate diagnostics show severe wake/sample collapse; decisively, the final-trap diagnostic leaves one valid payload queued after the last readiness wake |
| G8 carrier queue/backpressure | PARTIAL | carrier inbox is fixed at 64, not configurable, and drops on full |
| G9 compatible wire types and mismatch isolation | PASS with settings caveat | matching types passed and fixed-vs-dynamic mismatch was rejected before Image subscriber open |

The full Sensor partition sequence recorded down/up epoch delta 5,019 ms,
`last_pre_sequence=40`, `last_sent_before_up_sequence=191`,
`first_post_sequence=103`, `stale_replay_count=2`, `recovery_gap=63`, receiver
210/360, two gap events, 150 estimated missing, final 359, and monotonic `PASS`.
The full resource log recorded before/peak/settled/reconnect RSS/FD/tasks of
Host A `36068/28/10`, `42724/40/8`, `42692/28/8`, `42692/28/8` and Host B
`36292/24/9`, `42988/34/7`, `42980/24/7`, `42980/24/7`. The unique observer
used 50 ms hold/gap and captured exact 2,001 transitions; these are logical
cleanup/resource diagnostics, not a public leak counter.

The full Sidecar summaries were basic `1/20`, high-rate `2/10000`, final trap
`1/2` with one remaining after the last wake, and latest `13/200`. The latest
quick run added `PASS sidecar-spurious-wake samples=1 spurious_wakes=1`: the
readiness-prefill gap had `samples=1/1` after a two-second payload gap, with
event-driven delivery intact. The separate
dynamic bytes runtime path passed 3/3 at 1,024 bytes. Image and 20 MiB
PointCloud also passed in the full run. Pure-reactive idle CPU stayed A `2 -> 2`
and B `2 -> 2` over ten seconds; local-after-wait and preexisting fixed/Image
cases failed discovery/data without poll, including the startup-only
preexisting-service import, while hybrid poll was only a workaround.

## Imported settings finding

The key source fact is explicit. v0.10's `ServiceDescriptor` identifies a
service by name, hash, and types only, and says services with the same
descriptor exchange data “whatever their settings”
(`target/iceoryx2-v0.10.0/iceoryx2-link/backend/src/service_description/descriptor.rs:19-37`).
The descriptor conversion drops `ServiceDescription.settings`, while the
resolver imports a remote-only service with `ServiceDescription::with_default_settings`
from its local `Config`
(`target/iceoryx2-v0.10.0/iceoryx2-link/tunnel/src/resolver.rs:36-43,81-92`;
`target/iceoryx2-v0.10.0/iceoryx2-link/backend/src/service_description/mod.rs:66-80`).
The upstream conformance test names this behavior: a mirror has the local
default settings (`target/iceoryx2-v0.10.0/iceoryx2-link/conformance-tests/src/tunnel/publish_subscribe.rs:36-70`).

Production rbt intentionally binds QoS to `StreamProfile`: Sensor uses queue
depth 1, history 0, and safe overflow, while Control uses its separate profile
(`crates/rbt-iox2/src/lib.rs:90-111,240-241`). Its subscriber constructors
reapply those settings before `.open()` for fixed and slice streams
(`crates/rbt-iox2/src/lib.rs:429-460,784-815`). The upstream probe therefore
removes service settings from imported subscriber/listener opens and uses
receiver endpoint defaults (`experiments/cross-host-upstream/src/lib.rs:858-881,1235-1253,1403-1422,1652-1693`).
Runtime evidence showed typed mirrors can be visible while an unchanged rbt
subscriber that insists on its explicit Sensor/Control settings fails to open.
Generic G3 lifecycle remains a pass; settings preservation is a separate
production compatibility blocker and is the caveat attached to G9.

# iceoryx2 v0.9.3 cross-host Zenoh tunnel audit

## Scope and audit basis

This note audits the exact local checkout:

`/home/sw/rbt/target/iceoryx2-v0.9.3`

The checkout was verified at tag `v0.9.3`, commit
`6ef7c55176a8487ae7ca53c2e0649d5df9a1b3b4`. No `main` branch behavior is
included. Source citations below are paths in that checkout unless another
absolute path is shown.

The labels mean:

- **Source-proved**: directly visible in the pinned source.
- **Runtime-verified**: observed in the spike runtime evidence.
- **Inference**: the consequence of the source, called out as such.
- **Unknown/pending**: not established by this audit.

This is the exact-version source audit supporting the final A/B/C/D report in
`docs/cross-host-iox2-report.md`. Dynamic payload, pressure, impairment,
performance, SidecarV1, and remote-disappearance evidence is summarized below.

## Exact artifacts and versions

The integration workspace is declared at `integrations/Cargo.toml:1-6` and
contains these relevant crates:

| Artifact | Exact source/package | Evidence |
|---|---|---|
| Generic tunnel library | `iceoryx2-services/tunnel`, `iceoryx2-services/tunnel-backend` | `iceoryx2-services/tunnel/src/lib.rs:13-35`; `iceoryx2-services/tunnel-backend/src/traits/backend.rs:20-26` |
| Zenoh backend crate | `iceoryx2-integrations-zenoh-tunnel-backend`, package version `0.9.3` | `integrations/zenoh/tunnel-backend/Cargo.toml:1-24`; `integrations/Cargo.lock:1511-1525` |
| Zenoh CLI crate | `iceoryx2-integrations-zenoh-tunnel-cli`, package version `0.9.3` | `integrations/zenoh/tunnel-cli/Cargo.toml:1-29`; `integrations/Cargo.lock:1526-1541` |
| Binary | `iox2-tunnel-zenoh` | `integrations/zenoh/tunnel-cli/Cargo.toml:14-17` |

The manifest declares `zenoh = { version = "1.7.2", features = ["default", "unstable"] }`, i.e. the caret
requirement `^1.7.2`; the locked package is Zenoh `1.9.0` at
`integrations/Cargo.lock:4728-4732`. The lockfile is therefore the version
used by this audit, not the manifest lower bound.

## Scope of the tunnel

**Source-proved:** iceoryx2 itself has four messaging-pattern variants:
RequestResponse, PublishSubscribe, Event, and Blackboard
(`iceoryx2/src/service/static_config/messaging_pattern.rs:36-52`). The tunnel's
`on_discovery` match creates ports and relays only for PublishSubscribe and
Event; all other patterns take the unsupported branch
(`iceoryx2-services/tunnel/src/tunnel.rs:285-325`). The tunnel library's own
documentation says the same and says it does not spawn threads
(`iceoryx2-services/tunnel/src/lib.rs:13-35`).

Discovery and propagation are explicit operations. `discover()` performs local
and backend discovery, and `propagate()` drains/forwards PublishSubscribe and
Event ports (`iceoryx2-services/tunnel/src/tunnel.rs:201-273`). The generic
backend trait exposes separate PublishSubscribe and Event relay factories, not
RequestResponse or Blackboard relays
(`iceoryx2-services/tunnel-backend/src/traits/relay/factory.rs:71-184`).

The library `Config` contains only an optional local discovery-service name:
`iceoryx2-services/tunnel/src/tunnel.rs:118-121`. The Zenoh CLI adds a Zenoh
config path, discovery-service name, poll interval, and reactive flag
(`integrations/zenoh/tunnel-cli/src/cli.rs:17-55`). There is no tunnel-level
service allowlist, key-expression filter, payload filter, or QoS filter in
these configuration surfaces.

## Service naming and discovery

### Local discovery

With no `discovery_service` configured, `Tunnel::create` uses the local
`DiscoveryTracker`; with one configured it opens the named discovery service
(`iceoryx2-services/tunnel/src/tunnel.rs:133-189`). The named subscriber
processes only `DiscoveryEvent::Added`; other events, including removal, are
not processed (`iceoryx2-services/tunnel/src/discovery/subscriber.rs:102-129`).

The tracker calls `tracker.sync()` but binds the removed set to `_removed` and
processes only `added` (`iceoryx2-services/tunnel/src/discovery/tracker.rs:69-100`).
This is source-proved: local removal is ignored by the tunnel discovery layer.

### Zenoh keys and identity

The Zenoh backend uses these exact key expressions
(`integrations/zenoh/tunnel-backend/src/keys.rs:13-33`):

```text
iox2/service_details/*                 discovery query
iox2/service_details/{service_hash}    serialized StaticConfig
iox2/publish_subscribe/{service_hash}  PublishSubscribe bytes
iox2/event/{service_hash}              Event IDs
```

The service hash is derived from the messaging pattern and service name
(`iceoryx2/src/service/service_hash.rs:35-48`); the announced JSON is the
serialized `StaticConfig` (`integrations/zenoh/tunnel-backend/src/discovery.rs:107-129`).
The discovery object declares a remote-only querier, queries immediately at
creation, drains replies on each `discover()`, and issues another query for the
next call (`integrations/zenoh/tunnel-backend/src/discovery.rs:70-104;
154-200`). Thus, the current implementation repeatedly resends all visible
service details; the source itself marks this as non-optimal.

For each discovered supported service, the tunnel inserts a local port and a
backend relay into maps keyed by `ServiceHash`, then announces the service
(`iceoryx2-services/tunnel/src/tunnel.rs:88-116; 329-415`). Discovery is
deduplicated by hash (`iceoryx2-services/tunnel/src/tunnel.rs:299-302`).

### Disappearance and liveness

**Source-proved:** there is no removal branch in the local discovery handlers,
no removal event in the Zenoh discovery protocol here, and no timeout/TTL/
keepalive state in the tunnel discovery code. `tunneled_services()` is the
union of the two persistent port maps (`iceoryx2-services/tunnel/src/tunnel.rs:247-282`).
Zenoh announcements also create a remote-only background queryable for future
hosts (`integrations/zenoh/tunnel-backend/src/discovery.rs:131-149`).

**Inference:** once discovered, the tunnel retains its ports and relays, and
the background queryable has no tunnel-level removal/liveness representation.
If a remote service, process, or queryable disappears, a later discovery pass
does not remove the existing local port/relay or report service disappearance.
Resource cleanup therefore relies on local object/process teardown;
disappearance is not a modeled data-plane state. The runtime lifecycle test
below confirmed that Host B retained its local mirrors after the remote
provider and remote tunnel were both killed.

## Data representation and wire behavior

### PublishSubscribe, dynamic slices, and custom headers

The backend relay types are explicitly a custom header plus a dynamic payload
slice:

```rust
Header = CustomHeaderMarker
Payload = [CustomPayloadMarker]
```

(`iceoryx2-services/tunnel-backend/src/types/publish_subscribe.rs:13-29`).
The conformance source defines a custom `repr(C)`/`ZeroCopySend` header and
tests both structured payloads and `[u8]` slice payloads, including a
configured maximum slice length (`iceoryx2-services/tunnel-conformance-tests/src/publish_subscribe_propagation.rs:34-47; 166-195;
257-305`). Those tests establish the in-process/conformance shape; they do
not by themselves establish physical two-host acceptance.

The Zenoh PublishSubscribe relay:

1. Reads the user-header size from the static type details.
2. Appends the raw user-header bytes followed by the raw payload bytes to one
   Zenoh `ZBytes` value.
3. On receive, splits the received bytes at that static user-header size,
   loans an iceoryx2 payload of exactly the received payload-byte length, and
   copies the header and payload bytes into it.

This is visible at
`integrations/zenoh/tunnel-backend/src/relays/publish_subscribe.rs:141-179; 181-250`.
The core port converts received payload bytes to dynamic-slice element count
using the static payload type size
(`iceoryx2-services/tunnel/src/ports/publish_subscribe.rs:134-194`).
`MessageTypeDetails` defines Dynamic as a slice of a compatible element type
and carries size/alignment/type details
(`iceoryx2/src/service/static_config/message_type_details.rs:24-50; 56-172`).

**Implications:** this path is byte copying, not a portable serialization
format. Header layout, element size/alignment, endianness, and type agreement
must match on both sides. There is no explicit short-frame guard before the
header slice in the receive path; a malformed frame shorter than the static
header size is a source-level panic risk rather than a typed receive error.
That last statement is an inference from the indexing at
`integrations/zenoh/tunnel-backend/src/relays/publish_subscribe.rs:200-204`, not a runtime result.

The local PublishSubscribe port also drops samples published by the gateway's
own node ID to avoid loopback (`iceoryx2-services/tunnel/src/ports/publish_subscribe.rs:196-243`,
especially `:224-226`). This is an implementation loop filter, not a
user-configurable service filter.

### Event

The Event relay publishes `event_id.as_value().to_ne_bytes()` and accepts only
a payload of native `usize` length, reading it with an unaligned native
`usize` load (`integrations/zenoh/tunnel-backend/src/relays/event.rs:132-182`).
This is not a cross-architecture wire format; same-architecture Linux is the
only runtime evidence in this spike.

On the local side, pending backend IDs are accumulated in a `BTreeSet`, so
duplicate pending IDs are emitted once and in sorted order. The source has a
TODO explicitly noting that backend event ordering is not preserved
(`iceoryx2-services/tunnel/src/ports/event.rs:154-195`).

## Polling, execution, threads, and shutdown

The CLI selects `zenoh::Config::from_file` when `--zenoh-config` is supplied,
otherwise `zenoh::Config::default()` (`integrations/zenoh/tunnel-cli/src/main.rs:31-57`).
In non-reactive mode it uses `cli.poll.unwrap_or(100)`, attaches a WaitSet
interval, and on each tick calls `tunnel.discover()` followed by
`tunnel.propagate()` (`integrations/zenoh/tunnel-cli/src/main.rs:59-84`).
The documented default is therefore 100 ms (`integrations/zenoh/tunnel-cli/src/cli.rs:42-48`).

`--reactive` is not implemented: the branch calls
`unimplemented!("Reactive mode is not yet supported.")` at
`integrations/zenoh/tunnel-cli/src/main.rs:61-66`, with the panic observed at
line 63 in the spike invocation. The `--reactive` and `--poll` flags conflict.

The tunnel library itself has no thread-spawn path
(`iceoryx2-services/tunnel/src/lib.rs:24-35`). That does not mean the overall
process has one thread: Zenoh and other dependencies may own execution
resources. The idle-process measurements below observed four threads.

No explicit tunnel `shutdown`, `close`, or removal API was found in the tunnel
or Zenoh integration sources. The CLI remains in `wait_and_process`; normal
Rust object/session destruction is the apparent teardown mechanism. Explicit
remote shutdown notification and graceful service withdrawal are therefore
unknown/not provided by this source surface.

## Queue and buffering path

The source-audited path, subsequently exercised by pressure measurements, is:

```text
local app publisher/notifier
        |
        v
iceoryx2 local subscriber/listener
        |
        v   Tunnel::propagate() (poll-driven)
Zenoh reliable publisher  --->  remote Zenoh subscriber, FifoChannel(10)
                                      |
                                      v
                         remote tunnel relay receive/ingest
                                      |
                                      v
                         remote iceoryx2 publisher/notifier
```

For both PublishSubscribe and Event, the Zenoh subscriber is a remote-only
`FifoChannel::new(10)` and the publisher is remote-only and Reliable
(`integrations/zenoh/tunnel-backend/src/relays/publish_subscribe.rs:101-122;
integrations/zenoh/tunnel-backend/src/relays/event.rs:93-113`). Receives use
nonblocking `try_recv`; sends call `put(writer).wait()` or `put(event_id).wait()`
(same source paths). The
relay TODO says the handler type/properties should become configurable.

The default iceoryx2 PublishSubscribe settings are max subscribers 8, max
publishers 2, max nodes 20, history 0, subscriber buffer 2, subscriber
borrowed samples 2, publisher loaned samples 2, safe overflow enabled,
`RetryUntilDelivered` backpressure, expired-connection buffer 128, and static
allocation (`iceoryx2/src/config.rs:271-347`). Event defaults are 16 listeners,
16 notifiers, 36 nodes, maximum event ID 255, and no deadline or notifier
lifecycle events (`iceoryx2/src/config.rs:351-392`). These are defaults, not
a measured capacity guarantee for a cross-host path.

The discovery reply handler is also FIFO-based, but its capacity is not set by
the tunnel source (`integrations/zenoh/tunnel-backend/src/discovery.rs:13-22;
70-75`). The source does not provide a tunnel-level drop, retry, or
backpressure policy for a full remote FIFO. Runtime pressure tests observed
bounded recent-data behavior at the rbt Sensor queues, but also high-rate
small-message loss and local Event `FailedToDeliverSignal` warnings. Those
observations do not turn the combined lower-layer queues into a documented
tunnel-level contract.

## Filtering and configuration

There is no tunnel filter configuration. `Tunnel::Config` has only
`discovery_service`, and the CLI has only Zenoh config path, discovery service,
poll, and reactive options (`iceoryx2-services/tunnel/src/tunnel.rs:118-121`;
`integrations/zenoh/tunnel-cli/src/cli.rs:17-55`). The discovery wildcard and
the per-service hash keys are fixed in `integrations/zenoh/tunnel-backend/src/keys.rs:13-33`.
Duplicate hashes and unsupported messaging patterns are filtered internally;
there is no name/namespace allowlist or payload predicate.

`Locality::Remote` on publishers, subscribers, queriers, and queryables limits
the tunnel endpoints to remote Zenoh traffic; it is not an application-level
service filter. A complete Zenoh config file can still affect transport-level
behavior because the CLI passes it directly to `zenoh::open`
(`integrations/zenoh/tunnel-cli/src/main.rs:42-46`; `integrations/zenoh/tunnel-backend/src/backend.rs:58-74`).

## Error model

The generic tunnel has separate typed creation, discovery, and propagation
errors, including node/service/backend, port/relay, announcement, payload,
ingestion, and event errors
(`iceoryx2-services/tunnel/src/tunnel.rs:36-86`). The Zenoh backend adds typed
session/discovery, query, serialization, queryable, publisher/subscriber, and
payload/event errors (`integrations/zenoh/tunnel-backend/src/backend.rs:24-90`;
`integrations/zenoh/tunnel-backend/src/discovery.rs:26-68`;
`integrations/zenoh/tunnel-backend/src/relays/publish_subscribe.rs:35-75`;
`integrations/zenoh/tunnel-backend/src/relays/event.rs:27-67`).

At startup, tunnel construction errors are returned through the CLI's failure
handling. During each polling tick, `discover()` and `propagate()` errors are
logged as warnings and discarded so the loop continues
(`integrations/zenoh/tunnel-cli/src/main.rs:71-79`). Malformed discovery JSON
is warned and skipped (`integrations/zenoh/tunnel-backend/src/discovery.rs:154-177`).
Malformed Event frame length maps to `InvalidEvent`; the short PublishSubscribe
header case has the indexing risk described above.

## Security capabilities and defaults

The integration enables Zenoh `default` and `unstable` features and does not
configure authentication, authorization, or TLS in its own source
(`integrations/Cargo.toml:40-45`). It accepts a full external Zenoh config via
`--zenoh-config`; without that option it calls `zenoh::Config::default()`
(`integrations/zenoh/tunnel-cli/src/main.rs:42-46`). In the locked Zenoh
1.9.0 source, `default()` is equivalent to peer mode
(`/home/sw/.cargo/registry/src/index.crates.io-1949cf8c6b5b557f/zenoh-config-1.9.0/src/lib.rs:434-444`;
default mode constant at `/home/sw/.cargo/registry/src/index.crates.io-1949cf8c6b5b557f/zenoh-config-1.9.0/src/defaults.rs:29-48`).

The locked config exposes TLS certificate/key and mTLS fields
(`/home/sw/.cargo/registry/src/index.crates.io-1949cf8c6b5b557f/zenoh-config-1.9.0/src/lib.rs:794-818`)
and user/password or public-key authentication fields
(`/home/sw/.cargo/registry/src/index.crates.io-1949cf8c6b5b557f/zenoh-config-1.9.0/src/lib.rs:859-878`),
but this tunnel does not set them. Consequently, the source-proved default
for this integration is “Zenoh default peer config, with no tunnel-supplied
credentials/certificates”; the effective security posture of a deployment
using a config file is **configuration-dependent** and was not inferred.

## Verified runtime evidence

The following are runtime observations for this exact spike. They are not
claims that the upstream suite is a two-physical-host acceptance test.

### Build and upstream backend suite

- **Exact build:** `iox2-tunnel-zenoh` was built from the pinned checkout's
  `integrations` workspace using the repository's existing phase4 toolchain
  (`/home/sw/rbt/target/phase4-toolchain`). The repository documents that
  phase4 toolchain selection in `/home/sw/rbt/README.md:33-34` and
  `/home/sw/rbt/docs/testing.md:10-13`. The resulting binary is
  `/home/sw/rbt/target/iceoryx2-v0.9.3/integrations/target/debug/iox2-tunnel-zenoh`.
- **Upstream Zenoh backend suite:** **30 passed, 0 failed**. It includes
  PublishSubscribe/Event and slice coverage. The test wiring is at
  `integrations/zenoh/tunnel-backend/tests/main.rs:13-18`, with the
  PublishSubscribe and Event conformance modules in
  `integrations/zenoh/tunnel-backend/tests/publish_subscribe_propagation_tests.rs:13-34`
  and the corresponding Event test module. This suite uses logical/in-process
  conformance hosts and is **not** a two-host acceptance test.

### CLI behavior

- `--reactive` **panics** at
  `integrations/zenoh/tunnel-cli/src/main.rs:63` because the branch is
  `unimplemented!`.
- Polling defaults to **100 ms**, from
  `integrations/zenoh/tunnel-cli/src/main.rs:64-69` and
  `integrations/zenoh/tunnel-cli/src/cli.rs:42-48`.

### Two-network-stack namespace run

- Two Linux network namespaces were joined only by a veth pair. Each host
  process group had private tmpfs mounts at `/tmp` and `/dev/shm`.
- The setup was performed by a top-level unprivileged user+mount+network
  namespace despite the host having no effective `CAP_NET_ADMIN`; it created
  two nested named netns and the veth pair successfully.
- This is a valid two-network-stack namespace test, but it is **namespace-based,
  not physical hosts**.
- Plain PublishSubscribe delivered exact samples **1..8**. The observed logs
  are `/home/sw/rbt/target/cross-host-plain.a3dskt/hosta-publisher.log:5-14`
  and `/home/sw/rbt/target/cross-host-plain.a3dskt/hostb-subscriber.log:5-14`.
- Plain Event delivered exact IDs **1..7**. The observed logs are
  `/home/sw/rbt/target/cross-host-event.Gj4ZhA/hosta-notifier.log:5-12`
  and `/home/sw/rbt/target/cross-host-event.Gj4ZhA/hostb-listener.log:5-13`.
- The rebuilt production examples transported **100/100** existing rbt Image
  samples at 10 Hz. Each sample retained the existing Image hash
  `943fa4008cf0f55f89a281b051c69a6f`, its 6,220,800-byte RGB8 payload, and
  checksum `793152000`. Both
  `rbt/cap/00000000-0000-0000-0000-00000000f100/stream` and its
  `/ready` Event were discovered and ingested. The unchanged Host B consumer
  bound through `SliceStream<Image>`, used its existing `WaitContext`, and
  reported zero sequence gaps. Evidence is under
  `/home/sw/rbt/target/cross-host-rbt-image-rebuilt.AoNvd1`.
- In the temporary-disconnect run, the link was down for 5 seconds while the
  10 Hz publisher continued. Host B resumed at sequence 100 after sequence 39,
  reported one gap of 61 samples, and then received contiguous current data
  through sequence 200. No stale replay was observed. Evidence is under
  `/home/sw/rbt/target/cross-host-reconnect-image.Zfw2FP`.
- Service disappearance does not propagate. Five seconds after killing the
  Host A provider, and another five seconds after killing Host A's tunnel,
  Host B still listed the payload and `/ready` services. The payload service
  still showed one alive node: Host B's own `iox2-tunnel-zenoh` process. The
  before/after JSON is under
  `/home/sw/rbt/target/cross-host-lifecycle.3zKanF`. This runtime result matches
  the source audit: a mirrored service's existence is not evidence that its
  remote provider or peer remains healthy.
- The isolated probe subsequently passed plain dynamic `[u8]` (including zero
  and 1 MiB), a padded 640x480 Image, PointCloud N=0/1/100/100000, and three
  exact 20 MiB PointCloud samples across the same two-stack topology. The
  retained full run is
  `/home/sw/rbt/target/cross-host-verify-logs/cross-host-full-20260921T144542Z-29420`.
- An established 1 MiB stream passed one netem run with 20 ms delay, 5 ms
  jitter, 1% loss, and a 100 Mbit/s rate. A repeated run delivered 2/3 before
  timeout, showing that impairment results are not identical to local SHM.
- A 30 Hz/5 Hz Sensor test completed with increasing recent samples and gaps;
  it did not grow or replay a stale application queue. Fifty rapid link cycles
  were followed by an exact 8/8 u64 transfer.
- The full test intentionally created many unique services. Because the tunnel
  retains every discovered service, its live totals grew to seven threads,
  about 89 MiB/364 FDs on A and 228 MiB/473 FDs on B before process exit.
  These cumulative totals include payload, readiness, cancellation, benchmark,
  and lifecycle services and are not per-stream steady-state costs.
- Host platform: Linux kernel `7.0.0-31-generic`, Ubuntu `26.04`, x86_64.
  Physical NIC: `wlp0s20f3`, MTU **1500**, live 286.7 Mbit/s RX/TX Wi-Fi
  negotiation when recorded. Test veth pair MTU **1500**; the Wi-Fi link was
  not part of the test path.
- The initial namespace runs used a 10 ms poll setting; the final automated
  verifier used 1 ms. Both are separate from the CLI's 100 ms default.

### Idle resource measurements

Measured in an isolated namespace for the idle tunnel process:

| Poll setting | Threads | FDs | RSS | Measurement window | CPU ticks | `CLK_TCK` | Approx. CPU |
|---|---:|---:|---:|---:|---:|---:|---:|
| Default 100 ms | 4 | 20 | 32,084 KiB | 10 s | 4 | 100 | ~0.4% of one core |
| 10 ms | 4 | 20 | 31,876 KiB | 3 s | 9 | 100 | ~3% of one core |

These are idle-process observations, not throughput or saturation limits.

## Test boundary and unresolved product requirements

The evidence proves same-architecture PublishSubscribe/Event and all requested
dynamic payload shapes across two isolated Linux network stacks, the existing
rbt Image plus SidecarV1/WaitSet path, bounded recent-data recovery, and the
absence of usable remote-service withdrawal/liveness in the 0.9.3 tunnel.

It does not prove physical-LAN behavior, heterogeneous architectures,
production Zenoh security configuration, a transactional relationship between
payload and Event, or a way to map stale mirrors to rbt `BLOCKED`/`ACTIVE`.
The last item is a source-proved and runtime-confirmed productization blocker,
not merely an unexecuted test.

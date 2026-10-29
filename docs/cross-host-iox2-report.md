# iceoryx2 0.9.3 cross-host feasibility report

> Historical native baseline: this report's source audit and runtime results
> cover upstream `0.9.3` and predate production unification. Production now
> uses maintained iceoryx2 `0.10.0-rbt.2`; see the
> [unified baseline report](iceoryx2-unified-report.md).

## Decision

The existing iceoryx2 0.9.3 Zenoh tunnel can move the current rbt byte
representation, dynamic slices, and SidecarV1 Event across two isolated Linux
network stacks. The existing Host B `WaitContext` consumes those streams
without payload polling. That is not sufficient for rbt product integration.

The decisive missing primitive is **remote service/provider liveness with
withdrawal**. After the Host A provider and then Host A tunnel were killed,
Host B indefinitely retained both the payload and `/ready` mirrors. Their only
reported alive node was Host B's own tunnel. The 0.9.3 source has no removal
message, TTL, peer-health surface, or retained-relay eviction that a thin rbt
proxy could observe. A proxy therefore could not truthfully revoke its local
capability and drive dependents to `BLOCKED`.

There are two additional material limitations:

- PublishSubscribe payload and Event readiness use independent relays and are
  not transactional. Under high-rate small-message load, the source publisher
  logged thousands of `FailedToDeliverSignal` readiness failures while payload
  sends continued. SidecarV1 remains safe against a spurious wake, but this
  tunnel cannot guarantee a wake for every payload transition.
- The tunnel exports every supported local service, never removes a discovered
  relay, and exposes no tunnel-level queue/filter policy. Resource use rose as
  this test deliberately created unique services.

No custom network transport was implemented. No production rbt API, QoS,
layout, lifecycle, Python, C++, or MCAP behavior was changed.

## Scope and stop point

The spike followed the required order through exact-version source audit,
plain PublishSubscribe/Event, all requested payload shapes, existing rbt
Stream/SidecarV1/WaitSet, pressure, impairment, reconnect, liveness, schema
rejection, and performance diagnostics.

Productization-only work stopped at the liveness/Sidecar hard gate. Therefore
the remote proxy, Composer A/B failure choreography, mixed Python/C++/MCAP
graph, gateway-restart product loop, and 300-second product soak are explicitly
**NOT RUN**, rather than being simulated with polling or a new protocol.

## Exact environment

| Item | Tested value |
|---|---|
| iceoryx2 | tag `v0.9.3`, commit `6ef7c55176a8487ae7ca53c2e0649d5df9a1b3b4` |
| tunnel crates | `iceoryx2-services-tunnel`/backend and `iceoryx2-integrations-zenoh-tunnel-{backend,cli}` from that commit; packages report 0.9.3 |
| tunnel binary | `iox2-tunnel-zenoh`, debug build, `--poll 1` in automated functional runs |
| Zenoh | manifest requirement `^1.7.2`; locked package `1.9.0` |
| OS | Ubuntu 26.04 (Resolute Raccoon), Linux `7.0.0-31-generic`, x86_64 |
| CPU / RAM | Intel Core Ultra 5 125H; 31,744,776 KiB RAM |
| host topology | one Linux machine; two nested network namespaces, **not two physical hosts** |
| IPC isolation | private mount/IPC namespaces with separate tmpfs `/tmp` and `/dev/shm` per simulated host |
| test link | one veth pair, `10.77.42.1/24` to `10.77.42.2/24`, MTU 1500 |
| physical NIC | `wlp0s20f3`, Wi-Fi, MAC `c8:6e:08:01:d3:26`, MTU 1500; live negotiated RX/TX 286.7 Mbit/s when recorded |
| physical-network use | none; packets crossed the veth between distinct network stacks, not the Wi-Fi NIC |
| discovery in test | default Zenoh peer configuration and multicast scouting over the veth |
| clock | both namespaces shared one kernel clock; no PTP/NTP cross-host claim |

The default Zenoh configuration supports peer/router/listener/connect and
multicast/gossip configuration. The tested default used peer mode and
multicast discovery. The CLI accepts a full `--zenoh-config`, so deterministic
explicit peers are possible at the Zenoh layer, but were not a separate
acceptance run.

## Exact 0.9.3 API inventory

The detailed source audit is in `docs/cross-host-iox2-notes.md`. The relevant
inventory is:

| Capability | iceoryx2 0.9.3 tunnel result |
|---|---|
| PublishSubscribe | supported |
| Event | supported |
| RequestResponse / Blackboard | not supported by tunnel |
| dynamic slice payload | supported as copied raw user-header and payload bytes |
| service-name preservation | yes; Zenoh keys use pattern+name service hash; local mirror retains original name |
| service filtering | none in tunnel/CLI; every supported discovered service is exported |
| discovery | local tracker or named discovery service; Zenoh query/queryable announcements |
| removal / remote liveness | absent |
| default execution | polling; 100 ms default, configurable in milliseconds |
| reactive mode | exposed by CLI but panics as unimplemented |
| tunnel-library threads | none; caller drives `discover`/`propagate` |
| observed CLI threads | four idle in the isolated measurement; seven in the full service-heavy run |
| shutdown | process/object teardown; no remote withdrawal protocol |
| error behavior | typed create/discovery/propagation errors; CLI logs and continues after loop errors |

PublishSubscribe is not schema serialization. It copies native user-header and
payload bytes. Event IDs are native-endian `usize`. This spike therefore proves
same-architecture Linux compatibility only; it does not establish mixed
endianness or mixed-word-size compatibility.

## Frozen production baseline

At the time of this audit, the workspace was pinned to `iceoryx2 = "=0.9.3"`.
The existing hashes were observed unchanged:

| Type | Layout hash |
|---|---|
| Image | `943fa4008cf0f55f89a281b051c69a6f` |
| PointCloud | `d9da2bb29bb2cf217c40f1a56d1fb755` |
| ControlCommand | `9381cf15842a48ac0964111f0f45ef72` |

`cargo test --workspace --locked` passed before the isolated spike was added.
The isolated probe has `#![forbid(unsafe_code)]`, four unit tests, clean fmt,
and clean clippy with warnings denied. Production unsafe remains at its frozen
baseline; no existing ownership/buffer code was edited.

## Message compatibility

Every PASS below crossed the veth between the two private IPC environments.
The probe checked exact headers, lengths/counts, checksums, and selected values;
PointCloud also checked first and last points.

| Shape | Result | Evidence summary |
|---|---|---|
| plain `u64` | PASS | 8/8, exact sequence 0..7, checksum 28 |
| Event | PASS | 7/7 IDs 1..7, checksum 28 |
| dynamic `[u8]`, empty | PASS | 3/3, length 0 |
| dynamic `[u8]`, 1 KiB | PASS | 3/3, exact pattern/checksum/selected bytes |
| dynamic `[u8]`, 1 MiB | PASS | 3/3, exact pattern/checksum/selected bytes |
| Image 1920x1080 RGB8 | PASS | 100/100; 6,220,800 bytes; exact header, checksum 793,152,000, selected bytes |
| padded Image 640x480, stride 2048 | PASS | 1/1; 983,040 bytes; stride and padding preserved, checksum 125,337,600 |
| PointCloud N=0 | PASS | 1/1, zero-byte dynamic slice |
| PointCloud N=1 | PASS | 1/1, 16 bytes |
| PointCloud N=100 | PASS | 1/1, 1,600 bytes |
| PointCloud N=100000 | PASS | 1/1, 1,600,000 bytes |
| PointCloud 20 MiB | PASS | 3/3 at 1,310,720 points and 20,971,520 bytes in the exact-shape run |
| deliberate Image/PointCloud mismatch | PASS (rejected) | typed `AbiMismatch`; zero endpoints opened |
| ControlCommand diagnostic | PASS, experimental only | 20/20 in order, no observed duplicate; disconnect ambiguity not accepted/tested |

Single-sample tests for edge shapes intentionally separate representation
compatibility from Sensor pressure. At faster pacing, queue=1 legitimately
replaced intermediate N=0/N=100000/padded samples; that is pressure behavior,
not byte corruption.

## SidecarV1 and WaitSet

The exact services retained their names:

```text
rbt/cap/00000000-0000-0000-0000-00000000f100/stream
rbt/cap/00000000-0000-0000-0000-00000000f100/stream/ready
```

No tunnel prefix was added. Because identity is only pattern+name hash and the
tunnel mirrors globally without filtering, same-name conflicts are possible
and there is no explicit export/import boundary.

| Requirement | Result |
|---|---|
| payload PublishSubscribe tunneled | PASS |
| `/ready` Event tunneled | PASS |
| existing Host B `WaitContext` wakes | PASS |
| payload polling/artificial receive sleep | none |
| synthetic readiness | none beyond existing initial-drain semantics |
| 100-frame Image spurious wakeups | 0 |
| PointCloud exact-shape spurious wakeups | 0 |
| wake with no payload observed in functional run | no |
| payload without a successful local readiness notify possible | **yes; observed `FailedToDeliverSignal` under overload** |
| transactional payload+Event guarantee | **absent** |

The two relays are independent. The Event implementation additionally
deduplicates pending equal IDs in a `BTreeSet`, sorts them, and explicitly does
not preserve backend event order. SidecarV1's receiver behavior remains safe
when a wake is spurious, but the wake protocol is not reliable enough to be a
universal cross-host progress guarantee under overload/disruption.

## Pressure, impairment, and recovery

The Sensor profile remained unchanged: queue 1, history 0, safe overflow,
maximum one borrowed sample, and two publisher loans. A 30 Hz publisher with a
5 Hz consumer completed while receiving 20 increasing samples with 19 gap
events and 24 estimated dropped ordinals. The publisher did not block
indefinitely. This supports bounded recent-data behavior; it does not prove
that every hidden network queue has local Sensor semantics.

An established 1 MiB stream was exercised with netem delay 20 ms, jitter 5 ms,
1% packet loss, and 100 Mbit/s rate. One repetition delivered 3/3; another
delivered 2/3 before the finite timeout. This is acceptable loss for Sensor,
but demonstrates that impairment outcomes are not an exact copy of local SHM
behavior.

For the five-second disconnect test, the final automated run received 30
validated frames with one sequence gap, estimated 72 missing ordinals, and
first resumed at sequence 81. Earlier 220-frame evidence resumed at sequence
100 after sequence 39 (one gap of 61) and then stayed contiguous through 200.
No old-frame replay was observed. Reconnect-to-first-data was approximately one
second in that earlier timestamped run; the finite script records the exact
5.013 s link-down interval but does not claim a synchronized physical-host
latency.

Fifty rapid down/up cycles were followed by an exact 8/8 u64 transfer. Tunnel
A stayed at seven threads. Across that interval plus creation of the proof
service, FDs changed 212 to 220 and RSS 74,676 to 75,456 KiB. Since 0.9.3 never
removes discovered services, the added service itself confounds attribution;
this is not evidence of clean service-churn reclamation.

## Mandatory queue model

```text
A rbt publisher
  -> A local iceoryx2 subscriber queue owned by tunnel
  -> poll-driven tunnel copy
  -> Zenoh reliable transport/session queues
  -> B Zenoh remote-only FIFO(10)
  -> B tunnel copy into local iceoryx2 publisher
  -> B rbt subscriber queue

A readiness notifier
  -> bounded local Event/Unix-datagram delivery
  -> independent Zenoh Event reliable path and FIFO(10)
  -> B readiness notifier/listener
  -> existing WaitContext
```

| Layer | Bound/configuration | Policy/observation |
|---|---|---|
| A rbt Sensor to tunnel | service queue 1, history 0, safe overflow | bounded recent-data replacement |
| A local Event delivery | kernel/local Event resources; not a tunnel knob | nonblocking notify can report `FailedToDeliverSignal` |
| tunnel scheduler | poll interval; 100 ms default, 1 ms tested | drains on each `propagate`; reactive mode unavailable |
| Zenoh transport internals | Zenoh-config dependent; not surfaced as a tunnel queue contract | reliable `put(...).wait()`; exact aggregate bound/drop policy not established by tunnel API |
| remote PS FIFO | fixed 10 | reliable, remote-only, nonblocking drain; not CLI configurable |
| remote Event FIFO | fixed 10 | independent from payload, reliable, remote-only; not CLI configurable |
| B tunnel to rbt Sensor | queue 1, history 0, safe overflow | bounded recent-data replacement |
| retained service/relay maps | **no eviction/removal bound** | each unique discovered service remains until tunnel process exits |

No custom unbounded queue was added by the spike. The retained-service maps
are nevertheless unbounded with respect to service-name churn. In the full
run, which intentionally created many unique payload, readiness, cancellation,
benchmark, and lifecycle services, tunnel A ended near 89 MiB RSS/364 FDs and
tunnel B near 228 MiB RSS/473 FDs. The process cleaned up on exit, but live
reclamation was not observed.

## Service disappearance and composition blocker

Observed sequence:

```text
Host A provider alive
  -> Host B payload and /ready mirrors visible

kill Host A provider; wait 2-5 s
  -> both mirrors still visible on B
  -> payload mirror reports B tunnel as one Alive node

kill actual Host A tunnel child; wait 5 s
  -> both mirrors still visible on B
  -> payload mirror still reports B tunnel as one Alive node
```

There is no finite disconnect-detection latency at the service model: **no
withdrawal was observed**, and the source contains no mechanism that would
eventually produce one. A mirrored service's existence cannot satisfy an rbt
Requirement health check.

The thin-proxy hypothesis therefore fails on 0.9.3. A proxy could create a new
local identity, but it has no authoritative input for revocation:

```text
remote provider/tunnel/network loss
  -X-> observable remote-health transition
  -X-> revoke local proxy capability
  -X-> dependent QUIESCING -> BLOCKED
```

No proxy was built. Consequently provider replacement, new local proxy
`CapabilityInstanceId`, prepare-count preservation, and Composer A/B outage
behavior are NOT RUN. If a future tunnel provides health, the identity rule
remains: remote physical identity is provenance only; a Host B proxy receives
its own distinct local capability identity.

## Failure classification

| Class | What this spike can observe |
|---|---|
| local provider failure | provider process exit locally; **not propagated as remote withdrawal** |
| local gateway failure | local tunnel process exit/OS status; remote peer gets no service withdrawal |
| network unreachable | data stops; no service-health transition |
| remote gateway failure | data stops; stale local mirrors remain |
| remote provider failure | data stops; stale local mirrors remain |
| schema incompatibility | typed `AbiMismatch` before endpoint creation in rbt preflight |
| Sidecar readiness failure | local notify warning/error and/or missing wake; independent from payload relay |
| transport overload | Sensor gaps, notify failures, high small-message drop rate, resource growth |

This is exactly why collapsing these states into one `NetworkError` would not
repair composition semantics.

## Performance observations

These are debug-build feasibility measurements on one physical machine with
two network namespaces. They are not physical-LAN results or marketing claims.
The latency field is generated from `SystemTime` carried in the sample. Both
namespaces shared one host clock, so it is useful diagnostically here, but it
is not a validated monotonic or synchronized cross-host one-way measurement.

### Local versus tunnel

| Payload | Local pub/recv Hz | Tunnel pub/recv Hz | Tunnel receive throughput | Tunnel received | Tunnel p50/p90/p99/max us |
|---|---:|---:|---:|---:|---:|
| 64 B | 182,013 / 129,833 | 5,620 / 337 | 0.022 MB/s | 600/10,000 | 1,915 / 2,935 / 3,329 / 3,918 |
| 1 KiB | 84,427 / 83,303 | 5,086 / 221 | 0.226 MB/s | 218/5,000 | 2,977 / 4,518 / 5,352 / 11,450 |
| 1 MiB | 126.1 / 126.7 | 74.2 / 74.6 | 78.2 MB/s | 200/200 | 7,404 / 10,669 / 12,143 / 12,740 |
| 6.22 MiB Image | 21.1 / 21.3 | 6.57 / 6.64 | 41.3 MB/s | 100/100 | 12,617 / 17,200 / 26,684 / 28,494 |
| 20 MiB PointCloud | 54.6 / 56.5 | 1.93 / 1.99 | 41.8 MB/s | 30/30 | 23,814 / 27,921 / 33,245 / 33,245 |

High-rate 64 B and 1 KiB runs converged to recent data but dropped most
ordinals, as Sensor permits. Their publisher logs contained 2,197 and 2,393
`FailedToDeliverSignal` readiness warnings respectively. Large paced messages
were stable around 41-42 MB/s in this setup.

### Process resources

For the cross-host matrix above, app process CPU time/RSS were:

| Payload | A publisher CPU s / RSS | B subscriber CPU s / RSS |
|---|---:|---:|
| 64 B | 0.287 / 8.50 MB | 0.218 / 9.45 MB |
| 1 KiB | 0.244 / 8.50 MB | 0.024 / 9.27 MB |
| 1 MiB | 1.782 / 10.60 MB | 0.033 / 9.36 MB |
| 6.22 MiB | 5.511 / 14.73 MB | 0.027 / 9.36 MB |
| 20 MiB | 0.636 / 29.47 MB | 0.038 / 9.38 MB |

In the final full run, gateway A/B CPU deltas at `CLK_TCK=100` were 1.83/1.79
s (64 B), 1.68/1.63 s (1 KiB), 5.65/5.56 s (1 MiB), 18.02/18.22 s
(6.22 MiB), and 15.30/15.47 s (20 MiB). These include multithreaded Zenoh
work and each case's finite setup/wait window. RSS/FD totals are cumulative
because all prior services remain retained, so they must not be attributed to
one payload size.

Idle tunnel measurements were:

| Poll | Threads | FDs | RSS | CPU over window |
|---|---:|---:|---:|---:|
| default 100 ms | 4 | 20 | 32,084 KiB | 4 ticks / 10 s (~0.4% one core) |
| 10 ms | 4 | 20 | 31,876 KiB | 9 ticks / 3 s (~3% one core) |

## Security and deployment boundary

The tunnel itself adds no authentication, authorization, certificate handling,
or service allowlist. It passes an optional config file to Zenoh. Locked Zenoh
1.9.0 exposes TLS/mTLS, username/password, and public-key configuration fields,
but none were configured or tested here. Therefore the tested setup provides
no evidenced authenticated or encrypted deployment boundary.

The default also mirrors every PublishSubscribe/Event service. An eventual
robot deployment needs explicit export/import selection, deterministic peers,
credentials, observability, and service lifecycle. Those cannot be honestly
described as “thin deployment configuration” on the tested tunnel.

## Source-code and regression audit

The isolated probe contains no TCP/UDP/QUIC framing, retransmission, network
discovery, schema negotiation, distributed registry, custom payload serializer,
or payload polling loop. It calls existing iceoryx2 and rbt APIs only. The
network boundary copies bytes inside the upstream Zenoh backend; local rbt SHM
semantics remain unchanged and no network zero-copy claim is made.

The reusable commands are:

```bash
./scripts/verify_cross_host_spike.sh --quick
./scripts/verify_cross_host_spike.sh --full
```

Both modes build exact locked artifacts, create isolated hosts and veth, run
finite tests, retain evidence under `target/cross-host-verify-logs`, tear down
all processes/namespaces, and check for leftover spike processes. Quick and
full both finish with `functional=PASS product_gate=FAIL recommendation=D`.

## Product recommendation

Recommendation:
D

Smallest next milestone: do not implement an rbt network protocol. Run an
isolated upstream-capability comparison (without upgrading production rbt)
that must demonstrate all of the following before a thin proxy is
reconsidered:

1. explicit remote peer/provider liveness with timely service withdrawal;
2. bounded relay/service cleanup under churn and restart;
3. explicit service export/import filtering;
4. a documented bounded queue/backpressure model;
5. either coupled payload/readiness delivery or a supported progress primitive
   that preserves event-driven WaitSet operation under loss and overload.

Only if a released iceoryx2 version proves these primitives should a separate
migration-risk report and thin local proxy prototype be opened.

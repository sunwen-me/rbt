# v0.6 typed local RPC — acceptance report

> Historical native baseline: the acceptance and source reproduction below
> cover the former `0.9.3` candidate and rbt patches in use at report time
> (with unmodified-upstream cases identified separately). Production now uses
> maintained iceoryx2 `0.10.0-rbt.2`; see the
> [unified baseline report](iceoryx2-unified-report.md).

## Decision / status

**Functional acceptance: ACCEPTED** on the tested iceoryx2 0.9.3 baseline with
rbt-maintained patches. The final 2026-10-16 quick and full gates both exited
0, and the repaired 300-second RPC soak passed. The final checklist below is
149 PASS and 1 PASS* (#138), with no FAIL, INSUFFICIENT, or PENDING items. This
is acceptance of the tested candidate, not a claim that unmodified upstream
0.9.3 is safe or that a release has been published.

The earlier pre-repair tally—145 passing items (including #138 PASS*), 4 FAIL,
and 1 INSUFFICIENT—is retained as historical review only. It records the C++
move-assignment defect and the then-missing response-guard soak assertion; both
were repaired and reverified below. The tested package was iceoryx2 0.9.3 with
rbt-maintained patches, not unmodified upstream 0.9.3 (#138).

On 2026-10-15 a regression disproved the correlation invariant of the
unmodified dependency. A server retains a request after its client disappears.
A fresh client reuses the native connection slot and starts its own request
counter at zero. The old response is delivered through the reused slot and
accepted by the fresh pending request (acceptance items 15, 16, 19; section
229 hard stop).

Decision (user, 2026-10-15): fix the native defect, ship rbt on a patched
iceoryx2, and upstream the fix. rbt therefore no longer uses unmodified
upstream iceoryx2:

- Rust and Python: `third_party/iceoryx2/iceoryx2` (crates.io 0.9.3 crate +
  `patches/0001`) via `[patch.crates-io]`; at report time `Cargo.toml` pinned
  `=0.9.3`.
- C/C++: `scripts/bootstrap_iceoryx2_cpp.sh` applies `patches/*.patch` to
  commit `6ef7c55`. Patch 0002 grows the C FFI `iox2_response_mut_storage_t`
  from 88 to 112 bytes, so the C/C++ libraries must be rebuilt.
- `third_party/iceoryx2/verify_vendor.sh` proves the vendored crate equals
  the published crate (sha256 `e226c604…`) plus the patch; `verify_rpc.sh`
  source audit runs it.
- Upstream port with conformance tests is prepared for eclipse-iceoryx/iceoryx2
  main (not yet submitted).

The earlier 2026-10-16 current-source quick gate is
[`rpc-quick-20261016T013743Z-1823563`](../target/rpc-verify-logs/rpc-quick-20261016T013743Z-1823563/)
(exit 0). Its source-hash check, vendor audit, Rust RPC groups, Python
focused/fresh-extension tests, C++
RPC suite, CLI diagnostics, frozen hashes, format/clippy checks, and workspace
suite all passed. Workspace coverage was 257 passed / 1 ignored. The quick
gate is not a rerun of the full gate and did not exercise the separate
live-destination reproducer; the targeted repair evidence is recorded below.

Fixed during adoption (unrelated to the native defect): an exhausted per-call
sidecar listener budget was reported as `Listener(ExceedsMaxSupportedListeners)`
instead of `ClientCapacityReached` in Rust and C++.

2026-10-15 historical full gate
([`rpc-full-20261015T173357Z-1376936`](../target/rpc-verify-logs/rpc-full-20261015T173357Z-1376936/),
exit 0): source/vendor/hash/unsafe audits; Rust RPC groups, 1000 timeout and
1000 cancellation cycles, process-death cases, 100-cycle replacement and
`rpc_resources` 5/5; workspace 257 passed / 1 ignored across 55 suites; C++ Debug, Release,
ASan and UBSan 7/7 each; Python sync/async against a fresh 0.6.0 wheel;
validated benchmark matrix; 10-direction interop including two dynamic,
three asyncio and 4096-call burst lanes; 300-second soak with ten healthy
Composer restarts and three provider replacements; installed external
consumers; inherited Phase4, DX, v0.1, pressure, WaitSet, MCAP, Python and
Python asyncio gates. A passing verifier run does not establish the live
destination move-assignment cases, which were not in that gate.

Gate and harness defects fixed to reach that run, none in the RPC data path:
stale `rbt graph` edge assertions (v0.6 prints the interaction kind); ANSI
colour codes in redirected component logs (Python runtime and Rust examples
now enable colour only on a terminal); `rg -E` misuse in `soak_rpc.sh`; the
soak interop lane now spreads calls over the soak duration instead of a
fixed per-call sleep; a Python test called the `pressure` property.

Node-directory leak (fixed 2026-10-16): iceoryx2 cannot remove a node's
directory while one of that node's ports still exists, and logs "Unable to
remove node resources". rbt leaked one empty `/tmp/iceoryx2/nodes` entry per
endpoint whenever a port outlived its node: C++ RPC types declared the node
holder after native ports, and Rust/C++ stream samples held their port but
not the node, so a sample outliving its publisher/subscriber (common with
retained NumPy views) leaked. Every node creation scans that directory;
after a day of gates the WaitSet 100-cycle test slowed from 0.9 s to over
8 s. Endpoints now share the node with their samples and C++ members are
ordered so ports are destroyed first. Regressions: Rust
`rbt-iox2/tests/node_cleanup.rs` and C++ `pressure_tests`
(`test_sample_outliving_endpoint_releases_node`); both fail on the old
code. SIGKILLed processes are still cleaned by iceoryx2's dead-node
cleanup. `/dev/shm/test_prefix_*` files come only from running upstream
iceoryx2 conformance tests.

The checklist's original tally of 145 passing items (including #138 PASS*), 4
FAIL and 1 INSUFFICIENT is retained above as a historical summary. The single
150-row table below now records the
final post-repair disposition. It distinguishes the historical failure on
unmodified upstream from the patched production source and records the final
evidence for the tested candidate.

## Exact iceoryx2 0.9.3 audit and historical reproduction

Source baseline: tag `v0.9.3`, commit
`6ef7c55176a8487ae7ca53c2e0649d5df9a1b3b4`. At report time, production Cargo
pinned version `=0.9.3` through the rbt-maintained local patch. The failure
below was captured against the unmodified upstream source before patching; it
does not characterize the patched 0.9.3 candidate, which passes the retained
regression below.

The original upstream CONTINUE verdict was invalidated by the retained tests
in [`rpc_resources.rs`](../crates/rbt-iox2/tests/rpc_resources.rs), which
failed against unmodified 0.9.3 and pass against the patched source. Detailed
source analysis is in [`rpc-iox2-notes.md`](rpc-iox2-notes.md).

The command below is retained from the historical unpatched worktree. Running
it from today's repository resolves the rbt local patch and is not the same
reproduction.

```bash
CC="$PWD/target/phase4-toolchain/bin/x86_64-conda-linux-gnu-gcc" \
CXX="$PWD/target/phase4-toolchain/bin/x86_64-conda-linux-gnu-g++" \
CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="$PWD/target/phase4-toolchain/bin/x86_64-conda-linux-gnu-gcc" \
cargo test -p rbt-iox2 --test rpc_resources --locked \
  native_stale_response_cannot_cross_client_connection_reuse \
  -- --exact --nocapture --test-threads=1
```

Historical result on unmodified 0.9.3: **1 failed, exit 101**. The fresh
native pending request received `(43690, 3735928559)`, or
`(0xaaaa, 0xdead_beef)`, from the old client's request. The wrapper regression
independently expected `(0xbbbb, 0xcafe_babe)` and received the same old
marker. A separate supplementary patched-source request-ID boundary probe
passed against the same source bytes recorded in
`native-boundary-source-identity.log`; it was run alongside, but is not part
of, `verify_rpc.sh --quick`. A direct vendored `--locked --offline` unit-test
attempt did not run because the local Cargo index lacked
`iceoryx2-bb-testing`; that failed attempt is not counted as a test result.

Retained root-run evidence:

- [Raw native failure](../target/rpc-blocker-evidence/native-stale-client-reopen-root.log)
- [rbt wrapper failure](../target/rpc-blocker-evidence/stale-client-reopen-root.log)

The raw test uses only native iceoryx2 APIs. It has no Composer, rbt endpoint,
sidecar, terminal arbiter, pressure tracker, or application correlation layer.

```text
old client: send request (native request ID 0, channel 0)
server:     retain old ActiveRequest
old client: drop PendingResponse and client endpoint
new client: create, send first request (ID 0, channel 0)
server:     receive new request; connection slot now refers to new client
old request: send old response through cached slot
new pending: request ID matches 0 -> old payload accepted  [FAIL]
```

The minimum missing protection is validation of the originating native client
identity at response delivery and at retained-request channel cleanup. Native
`ActiveRequest::origin()` already retains that identity. A complete correction
must also prevent request-ID wrap from reusing a live correlation identity.
Changing wrapper timeout text, ignoring test payload markers, adding retries,
or checking only `is_connected()` before the native send is not sufficient.

## Architecture and request/response ABI

The implemented architecture uses native RequestResponse for loans, buffers,
correlation and data delivery. Composer resolves requirements, owns lifecycle
truth, and reports low-rate metadata; it is outside the Q/R payload path.
Physical services are named `rbt/cap/<CapabilityInstanceId>/rpc`.

Q and R are ordinary canonical rbt messages. Request/response descriptors are
both checked before opening transport. Runtime metadata and advisory sidecars
are not part of user layouts or their hashes. Frame/Detection are 16-byte
fixed messages; Image/PointCloud retain 32-byte canonical user headers.

RPC v0.6 is local-host only. Network work remains paused. v0.6 MCAP records
Streams, not RPC interactions. State, schema evolution, generic JSON RPC,
reflection, custom transports and load balancing are outside this milestone.

## Zero-copy ownership and correlation

```text
Rust request:  RpcRequestLoan -> send consumes loan -> native pending
Rust response: RpcRequest -> loan consumes request -> RpcResponseLoan
               -> send/drop -> received RpcResponseGuard -> final drop
C++ request/response: move-only RAII, corresponding native ownership transfers
Python writable: native loan <- retained lease root <- ndarray/derived aliases
                 all writable aliases must end before send
Python received: native read-only guard <- lease root <- ndarray/derived views
async wait: endpoint bridge -> bounded registration -> Future notification
            native owner remains authoritative for the returned sample
```

Same-process Python tests compare the native owner pointer with the NumPy
view pointer, and check `OWNDATA=False` and received `WRITEABLE=False`.
Cross-process virtual addresses need not match. Cross-language evidence uses
native loan/borrow ownership, source inspection, exact headers and bytes, and
checksums; it does not assert equal virtual addresses across processes.

The native payload path does not serialize Q/R or transport them through an
intermediate `Vec`. Metadata JSON and benchmark/test fixture construction do
not constitute a payload transport. Native zero-copy ownership alone does
not prove correlation: unmodified upstream 0.9.3 failed the retained
slot-reuse regression. The rbt patch adds originating-client validation and
prevents request-ID wrap; current targeted patched-source correlation tests
pass, while the package remains explicitly distinct from an unmodified
upstream release.

## Deadline, cancellation and response race

```text
Loaned --send--> Waiting
                  |-- response CAS ----------> Completed
                  |-- monotonic deadline CAS -> TimedOut
                  |-- local cancellation CAS -> Cancelled
                  `-- activation expiry CAS -> Expired
```

The rbt terminal atomic accepts one winner among response, deadline,
cancellation and expiry. Initial/final native probes allow an already visible
response to compete with the deadline. That arbitration prevents two local
terminal outcomes; it cannot distinguish a wrongly correlated native response.

An RPC timeout means the caller no longer accepted a response before its
deadline; it does not prove that the server did not observe or execute the
request. rbt does not automatically retry RPC calls. Client cancellation
abandons the local wait; it is not a distributed cancellation guarantee for
server-side work. Framework success does not make a physical side effect
transactional or establish exactly-once execution.

## Provider replacement and failure taxonomy

Managed provider disappearance uses the existing Requirement lifecycle:
QUIESCING -> BLOCKED -> ACTIVATING -> ACTIVE. Preparation is process-lifetime
work. A replacement provider receives a fresh capability instance and physical
endpoint; old activation endpoints are expired before tasks are joined.
The 100-cycle in-process replacement test passes; the process soak separately
covers three provider replacements. Fresh-client/same-server slot reuse is
covered by the patched native and wrapper correlation regressions.

| Error | Could request have executed? | Automatic retry | Client reuse | Lifecycle |
| --- | --- | --- | --- | --- |
| AbiMismatch / invalid config/length | No new send | None | Correct input/descriptor | No automatic transition |
| ClientCapacityReached | New send rejected | None | After capacity release | None by itself |
| ServerCapacityReached | Delivery rejected | None | Endpoint permitting | None by itself |
| ServerUnavailable | Do not infer execution from a generic unavailable report | None | Requires valid server/binding | Managed provider loss drives normal Requirement handling |
| DeadlineExceeded | Yes/unknown | None | Same valid endpoint, within tested lifetime | None by itself |
| Cancelled | Yes/unknown | None | Same valid endpoint, within tested lifetime | None by itself |
| ActivationExpired | Yes/unknown for an already sent call | None | Open next activation's binding | Endpoint authority ended |
| LoanPoolExhausted / BorrowLimitReached | Depends on operation stage | None | After relevant guard release | None by itself |
| ResponseCapacityReached / TransportFailure | Yes/unknown after send | None | Inspect operation and endpoint | Only normal supervisor policy if propagated |

Application errors belong in R. Critical task failures retain the existing
supervisor policy; RPC does not introduce another executor or lifecycle.

## Pressure / resource model and waitability

```text
client loan pool (max_request_loans)
  -> active pending requests (max_in_flight per client)
  -> native per-client server request path (same active-request bound)
  -> server ActiveRequest / unary response loan (max_response_loans per request)
  -> response buffer (response_buffer_size per pending channel/server)
  -> response borrow (max_borrowed_responses per pending response)
```

Defaults: in-flight 4, clients 16, request loans 2, response buffer 1,
borrowed responses 1, response loans 1, slice ceilings 1 element per direction.
One native server; static allocation; no request/response safe overflow;
fire-and-forget disabled; `DiscardData` backpressure. Limits are not profiles.
There is no unbounded request history, pending map or hidden task per request.

`loan_response(self)` consumes unary request ownership, so multiple concurrent
response loans for the same request cannot be forced through this wrapper.
Likewise, the native response borrow ceiling is per pending request, not a
global cap across different completed calls. Unreachable native exhaustion
cases must be recorded as not exposed rather than faked with a new knob.

Request/response readiness and local expiry use payload-free advisory Event
sidecars. Native WaitSets sleep between notifications with a 100 ms safety
probe; initial probes close setup races. Spurious/coalesced/lost notifications
do not carry correlation authority. See the exact audit for buffer/connection
cleanup caveats. A measured immediate process-death SLA is not claimed.

## Rust, C++, Python APIs and asyncio

Rust and C++ expose direct native fixed/slice loans, receive, response loans,
deadline waits and expiry handles. Python exposes sync and asyncio client and
server calls and Image dynamic shapes, reusing retained lease roots. Each
Python endpoint has a native bridge servicing bounded registrations, rather
than a native thread or event loop per call. Sync native waits release the GIL.

Response-guard current/high-water telemetry was included in the earlier quick
gate. Rust formatting/clippy, Python focused and fresh-extension tests, and
the C++ RPC suite passed there. The pre-repair five-case C++ live-destination
move-assignment audit reproduced skipped destination cleanup/accounting for
`PendingRpcCall`, `RpcRequest`, and `RpcResponseLoan`; it established stale rbt
counters, not a payload memory leak or shared-memory corruption. See the
2026-10-16 repair evidence below for the targeted regression result and the
completed full C++/sanitizer revalidation.

## Interoperability, observability / CLI

The full run passed ten interop directions, including two dynamic directions,
three asyncio directions, and 4096-call burst lanes with 16 outstanding calls
and nonce/checksum validation. See
`rpc-cross-language-interop.log` in the full-run evidence directory.

RPC graph/capability/why projections include transport kind, Q/R descriptors,
physical identities and retained provider evidence. `rbt rpc info --json`
reports bounded resources and low-rate identity-scoped metrics; fresh zero,
stale telemetry and unavailable telemetry are distinct. The CLI unit assertion
ran one test in the latest full run. No per-call Composer payload event exists.

## Performance, 300-second soak, process/resource cleanup

The 2026-10-16 F benchmark matrix completed and validated nonce, result and
checksum before reporting monotonic round-trip p50/p90/p99/max, calls/s, CPU
and RSS for 16B/16B, 1KiB/1KiB, 1KiB/1MiB and 1KiB/20MiB cases. No
cross-framework or one-way latency claim is made.

The 2026-10-15 full run completed a 300-second soak, but its harness did not
assert final response-guard counts across participants; checklist item #116
was therefore INSUFFICIENT in the pre-repair review. The post-repair 300-second
soak passed on 2026-10-16; see S below. That run had ten healthy Composer
restarts with zero deactivate/activate transitions and three provider
replacements, including one combined outage. Its pre-close live gauge snapshots
recorded zero pending calls, inflight calls, response guards, requests and
Python registrations. The managed RPC snapshot contained fresh nonempty client
observations and a fresh server observation: client inflight and response
guards were zero, and server requests were zero. The soak Rust-client/C++-server
lane completed 15,200 calls; both participants emitted and asserted idle
snapshots before endpoint close, which the parent harness validated after they
exited. Python completed 1,006 cycles, 2,012 responses, 1,006 deadlines and
1,006 cancellations; pre-close gauges showed zero pending calls, client/server
registrations, inflight calls, response guards and requests. Its separate final
pending-task/warning audit reported warnings=0. Cleanup reported no orphan
processes or resource-growth violations. Resource maxima were bounded (FD
delta 27, thread delta one, RSS growth about 5.1 MiB). The separate process-level
shutdown diagnostic passed four cases: client SIGINT/SIGTERM
with a pending call and provider SIGINT/SIGTERM blocked in receive, all with
graceful exit in 15–16 ms and no forced kill.

## Version/hash/unsafe audit and gate evidence

| Axis | Development value at report time |
| --- | --- |
| rbt / SDK | 0.6.0; functionally accepted on the patched 0.9.3 baseline (not a published release) |
| IDL language / descriptor | 1 / 1 |
| Control protocol / Composer snapshot | 2 / 4 |
| MCAP encoding | 1 / rbt-canonical-v1 |
| iceoryx2 dependency | upstream version 0.9.3 plus rbt patch 0001 (Rust) and patch 0002 (C FFI storage) |
| Production unsafe source locations | report-time quick audit: expected 49, observed 49, delta 0 (rbt-owned production source only) |

Frozen Image `943fa4008cf0f55f89a281b051c69a6f`, PointCloud
`d9da2bb29bb2cf217c40f1a56d1fb755`, ControlCommand
`9381cf15842a48ac0964111f0f45ef72`, Frame
`5d2c102a797d5b0ab64b4db8844297c0`, and Detection
`d8e60d2ca65e6b5b74f0981d34884a4d` passed the retained hash audit.

The earlier interrupted run
[`rpc-full-20261015T061829Z-24776`](../target/rpc-verify-logs/rpc-full-20261015T061829Z-24776/)
is historical partial evidence only. The 2026-10-15 full and earlier quick
runs above supersede that partial run as historical gate evidence. They predate
the 2026-10-16 repairs and do not provide final revalidation; see below.

## 2026-10-16 final verification and acceptance

The live-destination move-assignment repair and soak assertions are implemented
and verified. The final quick run
[`rpc-quick-20261016T070025Z-2297727`](../target/rpc-verify-logs/rpc-quick-20261016T070025Z-2297727/)
and full run
[`rpc-full-20261016T022204Z-1877923`](../target/rpc-verify-logs/rpc-full-20261016T022204Z-1877923/)
both exited 0. The full gate completed its inherited Phase4, install/external
consumer, DX, v0.1, pressure, WaitSet, MCAP, Python, and asyncio chains as well
as RPC-specific lanes. Both runs use the frozen source manifest
`target/rpc-fix-20261016-source-sha256.txt`; their copied manifests and source
checks agree.

Shell syntax checks (`bash -n`) passed. Optional ShellCheck was unavailable
(`Q/shellcheck.log`); its lint check is not counted as a passing test.

- The C++ RAII move-assignment change covers six affected classes in
  `sdk/cpp/include/rbt/rpc.hpp`; displaced destinations are cleaned up once,
  all native members move before the displaced temporary is destroyed, and
  self-move is guarded. The corrected five-case reproducer at
  `target/rpc-acceptance-audit/move_assign_fixed_regression.cpp` passed
  (`fixed-run.log`, exit 0). Its copy corrected only two setup preconditions
  so the initial counts matched normal expectations; the original historical
  source and log remain untouched. Permanent C++ tests cover the five
  accounting cases, request-loan capacity, self-move, and endpoint
  replacement/expiry.
- The final quick gate passed 26 focused Python tests, 15 idle-validator tests,
  and the workspace suite (257 passed / 1 ignored). It also ran all eight new
  C++ RPC regressions. The full gate passed C++ Debug, Release, AddressSanitizer
  and UndefinedBehaviorSanitizer CTest suites (7/7 each), plus the 15-test RPC
  interop idle validator.
- F passed the full Python sync and asyncio verification and the RPC benchmark
  matrix, with nonce, result and checksum validation. Evidence is in
  `F/python-sync-full.log`, `F/python-async-full.log`, and
  `F/rpc-benchmark.log`.
- Ten-direction Rust/C++/Python interoperability passed, including 4096-call
  bursts per lane. In this standalone interop run, all 20 participants emitted
  and asserted their own idle snapshots before endpoint
  close. After those participants exited, the parent harness emitted
  `IDLE_VALIDATED` after validating positive call counts, service identity,
  timestamps/activity and zero gauges for all 20 roles. Evidence is in
  [`F/rpc-cross-language-interop.log`](../target/rpc-verify-logs/rpc-full-20261016T022204Z-1877923/rpc-cross-language-interop.log)
  and `target/rpc-interop-logs/20261016T031205Z-2044101/`.
- The full 300-second soak passed in
  [`S`](../target/rpc-soak-logs/20261016T031227Z-2044429/)
  (`PASS`, also reported by `F/rpc-300s-soak.log`). It completed ten healthy
  Composer restarts with 0 deactivate / 0 activate transitions, three
  provider replacements including one combined outage, and the pre-close
  zero-gauge/idle assertions described above. This is distinct from the
  standalone 20-process interop run: S's interop lane was one Rust client and
  one C++ server, both emitting and asserting IDLE snapshots before endpoint
  close after 15,200 calls each.
  The Python probe finished
  1,006 cycles with 2,012 responses, 1,006 deadlines and 1,006 cancellations;
  its pre-close gauges showed zero pending calls, registrations, inflight calls,
  response guards and requests. Its separate final pending-task/warning audit
  reported warnings=0. Cleanup found no orphan processes. `S/final.rpc.json`
  contains fresh, nonempty managed-client observations with client
  inflight/response guards zero and a fresh server observation with requests
  zero.
- The full gate also passed inherited Phase4, installed/external-consumer, DX,
  v0.1, pressure (332 seconds, five cycles), WaitSet, MCAP, Python and asyncio
  verification. The final asyncio run lasted 304 seconds with ten restarts and
  reported zero unexpected warnings. The final checklist records the repaired
  statuses for #67, #68, #70, #94 and #116.

## Acceptance checklist

The question text in the Question column is copied verbatim from section 227 of the requested acceptance specification. PASS means evidence met the item at its stated scope; PASS* means the tested dependency is iceoryx2 0.9.3 with rbt patches, not unmodified upstream. The pre-repair FAIL and INSUFFICIENT results are preserved in the historical summary above.

Evidence roots:

- Q = final quick gate: `target/rpc-verify-logs/rpc-quick-20261016T070025Z-2297727/` (exit 0; frozen source manifest and source check pass).
- F = final full gate: `target/rpc-verify-logs/rpc-full-20261016T022204Z-1877923/` (exit 0; frozen source manifest and source check pass).
- S = final RPC 300-second soak: `target/rpc-soak-logs/20261016T031227Z-2044429/` (PASS; ten Composer restarts and three provider replacements).
- N = supplemental patched-source request-ID boundary evidence, run alongside but not inside `verify_rpc.sh --quick`: `target/rpc-verify-logs/rpc-quick-20261016T013743Z-1823563/native-request-id-boundary-matched-source.log` and `target/rpc-verify-logs/rpc-quick-20261016T013743Z-1823563/native-boundary-source-identity.log`. This remains supplemental evidence from the earlier quick path; the same native source bytes were reconfirmed in the final gates.
- M = C++ live-destination move-assignment audit directory `target/rpc-acceptance-audit/`. `root-move-assignment-run.log` is the untouched HISTORICAL failing reproduction; `move_assign_fixed_regression.cpp` and `fixed-run.log` are the corrected reproducer and passing post-repair result.
- D = process-shutdown diagnostic directory `target/rpc-acceptance-audit/run-20261016-094914-1835022/`; `summary.txt` records four passing process-level SIGINT/SIGTERM checks.

Final tally: **149 PASS + 1 PASS* (#138), 0 FAIL, 0 INSUFFICIENT, 0 PENDING.** Historical pre-repair tally: **145 passing items (including #138 PASS*), 4 FAIL (#67, #68, #70, #94), 1 INSUFFICIENT (#116).** The four C++ failures shared the live-destination move-assignment defect; #116 was the missing soak assertion. The acceptance scope is the tested rbt-maintained iceoryx2 0.9.3 candidate, not unmodified upstream or a published release.

| # | Status | Question | Evidence |
| ---: | :---: | --- | --- |
| 1 | PASS | Does exact iceoryx2 0.9.3 RequestResponse behavior have a source audit? | Q/source-audit.log; docs/rpc-iox2-notes.md audits the exact v0.9.3 tag and patched delta. |
| 2 | PASS | Does rbt use native RequestResponse rather than building a custom protocol? | crates/rbt-iox2/src/rpc.rs:443-480 opens native RequestResponse; Q/source-audit.log. |
| 3 | PASS | Is Composer absent from request/response payload path? | docs/rpc.md:24-30 places Composer outside the Q/R request/response path. |
| 4 | PASS | Are Q and R ordinary canonical rbt message types? | crates/rbt-iox2/src/rpc.rs:443-480 uses canonical generic Q/R types; rpc-iox2-notes.md distinguishes native headers. |
| 5 | PASS | Is Q ABI validated before endpoint open? | crates/rbt-iox2/tests/rpc_fixed.rs::preflight_rejects_request_and_response_mismatch_before_transport; Q/rust-rpc-fixed.log. |
| 6 | PASS | Is R ABI validated before endpoint open? | crates/rbt-iox2/tests/rpc_fixed.rs::preflight_rejects_request_and_response_mismatch_before_transport; Q/rust-rpc-fixed.log. |
| 7 | PASS | Are existing layout hashes unchanged? | Q/hash-audit.log and Q/frozen-hash-test.log verify the five frozen message hashes. |
| 8 | PASS | Is RPC metadata excluded from user payload layout? | third_party/iceoryx2/iceoryx2/src/service/header/request_response.rs:22-97 keeps internal correlation headers separate from user Q/R. |
| 9 | PASS | Does fixed-size request use loaned SHM? | crates/rbt-iox2/src/rpc.rs:780-799 loans native request storage; Q/rust-rpc-fixed.log exercises fixed native loan roundtrip. |
| 10 | PASS | Does fixed-size response use loaned SHM? | crates/rbt-iox2/src/rpc.rs:2209-2228 loans native response storage; Q/rust-rpc-fixed.log. |
| 11 | PASS | Are dynamic requests supported if upstream supports them? | crates/rbt-iox2/tests/rpc_dynamic.rs slice-request cases; Q/rust-rpc-dynamic.log and Q/python-rpc-fresh-extension.log. |
| 12 | PASS | Are dynamic responses supported if upstream supports them? | crates/rbt-iox2/tests/rpc_dynamic.rs slice-response cases; Q/rust-rpc-dynamic.log and Q/python-rpc-fresh-extension.log. |
| 13 | PASS | Is there no payload serialization? | Q/source-audit.log and crates/rbt-iox2/src/rpc.rs:803-839 show native loans with no Q/R serializer. |
| 14 | PASS | Is there no intermediate Vec payload transport? | Q/source-audit.log and crates/rbt-iox2/src/rpc.rs:803-839 show native slice loans without an intermediate payload Vec. |
| 15 | PASS | Is request correlation correct? | Q/rust-rpc-resources.log patched regressions stale_server_request_cannot_satisfy_reopened_clients_first_pending_call and native_stale_response_cannot_cross_client_connection_reuse. |
| 16 | PASS | Can response A never complete request B? | Q/rust-rpc-resources.log runs both retained stale-response regressions on patched source; third_party/iceoryx2/iceoryx2/src/response_mut.rs:337-366 validates origin identity. |
| 17 | PASS | Are concurrent calls supported within a bound? | crates/rbt-iox2/tests/rpc_fixed.rs:345-415 verifies eight out-of-order responses; Q/rust-rpc-fixed.log. |
| 18 | PASS | Is pending-call state strictly bounded? | crates/rbt-iox2/src/rpc.rs:301-317 validates resource limits; :1130-1151 bounds listeners; Q/rust-rpc-resources.log. |
| 19 | PASS | Is request identity safe against wrap/reuse? | third_party/iceoryx2/iceoryx2/src/port/client.rs:166-195,640-648 rejects wrap; N supplementary max-boundary probe. |
| 20 | PASS | Does timeout use monotonic time? | crates/rbt-iox2/src/rpc.rs:1349 uses Instant; Q/rust-rpc-fault.log deadline cases. |
| 21 | PASS | Does DeadlineExceeded terminate the local wait? | crates/rbt-iox2/src/rpc.rs:1372-1382 terminates at the deadline; Q/rust-rpc-fault.log. |
| 22 | PASS | Is timeout documented as execution-ambiguous? | docs/rpc.md:45-57 documents execution ambiguity after timeout. |
| 23 | PASS | Does rbt perform no automatic retry? | docs/rpc.md:45-57 states no automatic retry; crates/rbt-iox2/src/rpc.rs:1031-1048 returns without retry. |
| 24 | PASS | Is a late response after timeout ignored safely? | crates/rbt-iox2/tests/rpc_fixed.rs::timeout_late_response_does_not_poison_next_call; F/rust-rpc-timeout-1000.log. |
| 25 | PASS | Does the next call work after timeout? | crates/rbt-iox2/tests/rpc_fixed.rs::timeout_late_response_does_not_poison_next_call; F/rust-rpc-timeout-1000.log. |
| 26 | PASS | Is timeout/response race linearized exactly once? | crates/rbt-iox2/src/rpc.rs:492-517 terminal CAS; Q/rust-rpc-fixed.log race tests and F 1000-timeout run. |
| 27 | PASS | Does client cancellation terminate the local wait? | crates/rbt-iox2/src/rpc.rs:1599-1615 cancellation handle; rpc_fixed.rs::cancellation_is_typed_and_next_call_works. |
| 28 | PASS | Is cancellation explicitly not claimed as server cancellation? | docs/rpc.md:51-57 says local cancellation does not cancel server-side work. |
| 29 | PASS | Is a late response after cancellation safe? | crates/rbt-iox2/src/rpc.rs:1543-1560 resolves terminal-state losers; patched native response identity is checked before delivery. |
| 30 | PASS | Does the next call work after cancellation? | crates/rbt-iox2/tests/rpc_fixed.rs::cancellation_is_typed_and_next_call_works; F/rust-rpc-cancel-1000.log. |
| 31 | PASS | Does activation cancellation wake pending calls? | crates/rbt-iox2/src/rpc.rs:520-534 activation expiry; rpc_fixed activation-cancellation test in Q/rust-rpc-fixed.log. |
| 32 | PASS | Does process shutdown wake pending calls? | D/summary.txt: four client-pending/provider-blocked SIGINT/SIGTERM scenarios exited gracefully in 15-16 ms. |
| 33 | PASS | Is server receive event-driven? | crates/rbt-iox2/src/rpc.rs:2002-2099 attaches request readiness to native WaitSet; Q/source-audit.log. |
| 34 | PASS | Is client response wait event-driven? | crates/rbt-iox2/src/rpc.rs:1346-1504 waits on native WaitSet response readiness; Q/source-audit.log. |
| 35 | PASS | Is there no busy payload polling? | crates/rbt-iox2/src/rpc.rs:62,1404-1422 uses a bounded 100 ms native safety probe; no busy-loop payload polling. |
| 36 | PASS | Is there no thread per call? | crates/rbt-iox2/src/rpc.rs:1346-1533 performs synchronous inline waits; no thread is spawned per call. |
| 37 | PASS | If a readiness sidecar exists, is its protocol explicitly defined? | crates/rbt-types/src/lib.rs:1093-1117 defines RpcSidecarV1 and its readiness identities. |
| 38 | PASS | Are spurious wakes safe? | crates/rbt-iox2/src/rpc.rs:1458-1475 tolerates empty wakes; Q workspace-test.log WaitSet empty-wake cases. |
| 39 | PASS | Can a valid response ever remain stranded forever? | No: crates/rbt-iox2/src/rpc.rs:1404-1406,1485-1497 performs the finite native 100 ms safety probe after advisory wakes. |
| 40 | PASS | Are drains/fairness bounded? | crates/rbt-iox2/src/wait.rs:37-44,820-854 bounds event drains; Q/workspace-test.log WaitSet fairness test. |
| 41 | PASS | Is client capacity explicit? | crates/rbt-types/src/lib.rs:1187-1224 defines explicit RPC resource fields and limits. |
| 42 | PASS | Is server capacity explicit where upstream exposes it? | crates/rbt-iox2/src/rpc.rs:470-480,1861-1866 maps the exposed native server limits; Q/rust-rpc-resources.log. |
| 43 | PASS | Is overload bounded? | crates/rbt-iox2/src/rpc.rs:470-480 disables overflow; slow_server_reports_explicit_client_capacity_and_recovers passes in Q/rust-rpc-stress.log. |
| 44 | PASS | Is overload failure typed? | crates/rbt-iox2/src/rpc.rs:1143-1146 returns typed capacity failure; Q/rust-rpc-resources.log. |
| 45 | PASS | Is there no blind overload retry? | crates/rbt-iox2/src/rpc.rs:691-693 selects DiscardData; :1031-1048 returns overload errors without retry. |
| 46 | PASS | Do multiple clients route responses correctly? | crates/rbt-iox2/tests/rpc_fixed.rs::sixteen_clients_route_markers_and_isolate_cancellation; Q/rust-rpc-fixed.log. |
| 47 | PASS | Do out-of-order responses correlate correctly? | crates/rbt-iox2/tests/rpc_fixed.rs:345-415 verifies out-of-order correlation. |
| 48 | PASS | Is there no hidden unbounded task spawn per request? | crates/rbt-iox2/src/rpc.rs:1007-1099,1346-1533 uses bounded synchronous paths without per-request task spawn. |
| 49 | PASS | Does dropping an unsent request loan release capacity? | crates/rbt-iox2/tests/rpc_fixed.rs:104-123 verifies dropped unsent loan releases capacity. |
| 50 | PASS | Does dropping an unanswered server request release server resources? | crates/rbt-iox2/tests/rpc_fixed.rs:444-495 verifies unanswered request drop releases server resources. |
| 51 | PASS | Does duplicate response ownership become impossible or typed-rejected? | crates/rbt-iox2/src/rpc.rs:2157-2170,2209-2228,2359-2365 transfers response ownership linearly. |
| 52 | PASS | Can one timeout avoid poisoning the RpcClient? | crates/rbt-iox2/tests/rpc_stress.rs::thousand_timeout_late_response_reuse_cycles_have_no_in_flight_calls; F log. |
| 53 | PASS | Can one cancellation avoid poisoning the RpcClient? | crates/rbt-iox2/tests/rpc_stress.rs::thousand_cancellation_races_have_one_terminal_result_and_recover; F log. |
| 54 | PASS | Does provider disappearance drive normal Requirement lifecycle semantics? | crates/rbt-compose/src/component.rs:1082-1121 deactivates the dependent component through normal lifecycle handling; F replacement test. |
| 55 | PASS | Does missing provider mean BLOCKED rather than FAILED? | crates/rbt-compose/src/component.rs:1111-1121 records unresolved required providers as BLOCKED. |
| 56 | PASS | Does client prepare remain once across provider replacement? | crates/rbt-compose/src/component.rs:1908-1918,2136-2139; F replacement-100 test keeps client prepare count at one. |
| 57 | PASS | Does provider replacement create a fresh CapabilityInstanceId? | crates/rbt-compose/src/component.rs:2062-2065; F replacement stress observes 101 distinct CapabilityInstanceIds. |
| 58 | PASS | Does provider replacement create a fresh physical endpoint? | crates/rbt-compose/src/component.rs:1956-1958,2136-2137; F replacement stress observes 101 endpoint activations. |
| 59 | PASS | Can old server response never satisfy new activation calls? | crates/rbt-compose/src/component.rs:2050-2058 expires old activation calls; :2118-2128 permits the replacement call. |
| 60 | PASS | Does 100-cycle provider replacement pass? | F/rust-rpc-replacement-100.log passes the in-process 100-cycle test; S separately covers three process-soak replacements. |
| 61 | PASS | Does server crash during a call terminate eventually? | crates/rbt-iox2/tests/rpc_fault.rs:275-364 covers server SIGKILL during an RPC call. |
| 62 | PASS | Does client crash reclaim upstream resources eventually? | crates/rbt-iox2/tests/rpc_fault.rs:375-457 verifies client SIGKILL, app release of the held server request, then recovery. |
| 63 | PASS | Does healthy Composer outage leave RPC traffic running? | S restart records plus crates/rbt-compose/src/component.rs:2226-2294 show RPC traffic continues while Composer is down. |
| 64 | PASS | Does healthy Composer restart produce 0 deactivate / 0 activate? | scripts/soak_rpc.sh:581-599 asserts stable identities and zero lifecycle churn; S contains all ten restart records. |
| 65 | PASS | Does Composer+provider outage reconstruct reality correctly? | S combined-outage record plus scripts/soak_rpc.sh:621-660 verifies BLOCKED then provider rebind. |
| 66 | PASS | Are RPC metrics keyed to physical capability identity? | crates/rbt-iox2/src/rpc_pressure.rs:95-117 keys metrics by physical identity; composer.rs:1714-1776 joins current identity. |
| 67 | PASS | Is inflight_current exact? | M/fixed-run.log reports calls_started=2, calls_terminal=2, in_flight_current=0 after live-destination assignment; permanent regression tests in sdk/cpp/tests/rpc_tests.cpp:289-478 run at :809-816. |
| 68 | PASS | Is inflight_high_water correct? | M/fixed-run.log reports max_in_flight=2, two fresh calls, high-water=2 and current=0; permanent C++ regressions run in F. |
| 69 | PASS | Are timeout counts exact? | crates/rbt-iox2/tests/rpc_stress.rs:122-134 asserts exact timeout counts over 1000 cycles. |
| 70 | PASS | Are cancellation counts exact? | M/fixed-run.log reports two calls terminal, one cancelled, and in_flight_current=0 after overwriting the cancelled live destination; permanent C++ regressions run in F. |
| 71 | PASS | Are capacity failures observable? | crates/rbt-iox2/tests/rpc_stress.rs:282-300 checks typed capacity failure and its counter. |
| 72 | PASS | Are metrics low-frequency snapshots only? | crates/rbt-compose/src/component.rs:781-782,860-862 publishes aggregate metrics on a one-second tick. |
| 73 | PASS | Is there no per-call Composer control event? | crates/rbt-compose/src/component.rs:921-964 separates snapshot publication from per-call local metric updates. |
| 74 | PASS | Does `rbt graph` represent RPC as RPC rather than topic? | crates/rbt-cli/src/lib.rs:540-568,649-715 represents RPC transport edges; S/final.graph.json records the graph. |
| 75 | PASS | Does `rbt why` explain missing RPC requirements? | crates/rbt-cli/src/lib.rs:1239-1285,1426-1437,1632-1688 explains typed last-active/provider evidence; an initially unresolved generic requirement has no transport until bound. |
| 76 | PASS | Does capability listing expose transport kind? | crates/rbt-cli/src/lib.rs:1794-1833 exposes RPC transport kind and Q/R descriptors. |
| 77 | PASS | Is RPC diagnostic JSON structured? | Q/cli-rpc-human-json.log and crates/rbt-cli/src/lib.rs:3263-3295, `tests::rpc_info_reports_typed_metrics_without_fabricating_unavailable_zeroes`, verify structured output and preserve unavailable as distinct from zero. |
| 78 | PASS | Does stale/unavailable telemetry remain distinguishable from zero? | crates/rbt-compose/src/composer.rs:2323-2403 keeps stale telemetry distinct from unavailable; CLI tests around 3263-3285 distinguish null from zero. |
| 79 | PASS | Does Rust sync RPC work? | Q/rust-rpc-fixed.log: 15 fixed RPC tests pass; crates/rbt-iox2/tests/rpc_fixed.rs. |
| 80 | PASS | Does C++ native RPC work? | F/cpp-debug-ctest.log and C++ sanitizer/Release suites pass 7/7; Q C++ RPC suite uses patched 112-byte FFI response storage. |
| 81 | PASS | Does Python sync RPC work? | Q/python-rpc-focused.log: sync tests pass in the 26-test focused sync/native/async/dynamic run. |
| 82 | PASS | Does Python async RPC work? | Q/python-rpc-focused.log: async tests pass in the same 26-test focused run; F/inherited-python-asyncio.log covers the fresh wheel. |
| 83 | PASS | Does Python sync wait release the GIL? | python/tests/test_rpc_native.py:155-195 verifies GIL progress; crates/rbt-python/src/rpc.rs:2373-2375 detaches blocking wait. |
| 84 | PASS | Does asyncio avoid polling? | crates/rbt-python/src/rpc.rs:1178-1205 waits through native WaitSet with a 100 ms safety interval; no asyncio.sleep polling coroutine. |
| 85 | PASS | Does asyncio avoid one thread per call? | crates/rbt-python/src/rpc.rs:986-1004 uses one bridge; python/tests/test_rpc_async.py:191-224 checks four calls without per-call OS threads. |
| 86 | PASS | Does `Task.cancel()` clean up registrations? | python/tests/test_rpc_async.py:191-224 runs 100 cancellations and verifies registrations return to zero. |
| 87 | PASS | Does `asyncio.wait_for()` behave correctly? | python/tests/test_rpc_async.py:129-149 covers wait_for deadline, cleanup, and reuse. |
| 88 | PASS | Do async cancellation races have one terminal outcome? | crates/rbt-python/src/rpc.rs:777-788 terminal CAS and :882-907 cancel handling; python/tests/test_rpc_async.py:92-125 checks cancellation followed by late-response cleanup/reuse, not a simultaneous stress race. |
| 89 | PASS | Does Python server receive_async cancel cleanly on activation loss? | python/tests/test_rpc_async.py:156-187 verifies receive_async cancellation on activation expiry. |
| 90 | PASS | Do Python dynamic received views remain zero-copy if supported? | python/tests/test_rpc_dynamic.py:140-197,362-410 checks Image view pointers, OWNDATA=False, and received WRITEABLE=False. |
| 91 | PASS | Do Python writable RPC loans preserve alias safety? | python/tests/test_rpc_dynamic.py:203-240 confirms a derived writable alias prevents send; python/tests/test_rpc_native.py:63-95 verifies the retained writable alias guard. |
| 92 | PASS | Do derived NumPy views preserve native sample lifetime? | python/tests/test_rpc_dynamic.py:167-196 keeps the native guard at one while derived views live and returns it to zero after release. |
| 93 | PASS | Does cross-thread final drop remain safe? | python/tests/test_rpc_dynamic.py:183-196,228-240 verifies cross-thread final drop and guard release. |
| 94 | PASS | Does C++ move-only ownership remain correct? | Six RAII move assignments in sdk/cpp/include/rbt/rpc.hpp:1531,1951,2199,2474,2613,2751 use safe displacement; eight C++ RPC regressions (rpc_tests.cpp:289-478, called at :809-816) and F Debug/Release/ASan/UBSan CTest each pass 7/7. M/fixed-run.log covers all five accounting cases. |
| 95 | PASS | Do C++ ASan tests pass? | F/cpp-address-ctest.log passes 7/7 with the address sanitizer build. |
| 96 | PASS | Do C++ UBSan tests pass? | F/cpp-undefined-ctest.log passes 7/7 with the undefined-behavior sanitizer build. |
| 97 | PASS | Does Rust client → C++ server pass? | F/rpc-cross-language-interop.log CASE_PASS rust-client-cpp-server; nonce/result checks pass. |
| 98 | PASS | Does C++ client → Rust server pass? | F/rpc-cross-language-interop.log CASE_PASS cpp-client-rust-server; nonce/result checks pass. |
| 99 | PASS | Does Rust client → Python server pass? | F/rpc-cross-language-interop.log CASE_PASS rust-client-python-server; nonce/result checks pass. |
| 100 | PASS | Does Python client → Rust server pass? | F/rpc-cross-language-interop.log CASE_PASS python-client-rust-server; nonce/result checks pass. |
| 101 | PASS | Does Python async client → C++ server pass? | F/rpc-cross-language-interop.log CASE_PASS python-async-client-cpp-server; nonce/result checks pass. |
| 102 | PASS | Does C++ client → Python async server pass? | F/rpc-cross-language-interop.log CASE_PASS cpp-client-python-async-server; nonce/result checks pass. |
| 103 | PASS | Does high-concurrency nonce validation pass? | F/rpc-cross-language-interop.log: Rust/C++ burst lanes each validate 4096 calls with 16 outstanding; implementations at crates/rbt-rpc-interop/src/main.rs:370-504 and sdk/cpp/tools/rpc_interop.cpp:362-503. |
| 104 | PASS | Do 1000 deadline/late-response cycles pass? | F/rust-rpc-timeout-1000.log: thousand_timeout_late_response_reuse_cycles_have_no_in_flight_calls passes. |
| 105 | PASS | Do cancellation-storm tests return pending registrations to zero? | S/python.log records 1,006 cancellations with client/server registrations zero; python/tests/rpc_soak_probe.py:287-318,383-384 asserts pre-close gauges; Q Python asyncio 100-cycle cancellation suite passes. |
| 106 | PASS | Does slow-server pressure remain bounded? | Q/rust-rpc-stress.log test slow_server_reports_explicit_client_capacity_and_recovers passes. |
| 107 | PASS | Does repeated timeout avoid response-queue leakage? | F/rust-rpc-timeout-1000.log verifies 1000 timeout/late-response cycles and subsequent reuse with zero inflight. |
| 108 | PASS | Does repeated cancellation avoid response-queue leakage? | F/rust-rpc-cancel-1000.log verifies 1000 cancellation races and subsequent reuse with zero inflight. |
| 109 | PASS | Does large dynamic RPC pass if supported? | python/tests/test_rpc_dynamic.py:300-348 exercises 1 MiB/20 MiB shapes; Q dynamic suite and PointCloud tests pass. |
| 110 | PASS | Does the benchmark validate payload/result correctness? | crates/rbt-rpc-bench/src/main.rs:146-219,285-359,491-530 validates nonce/result/checksum; F benchmark records 23 validated responses per case. |
| 111 | PASS | Does the benchmark report round-trip latency rather than fake one-way latency? | crates/rbt-rpc-bench/src/main.rs:166-171,309-359,499-516 reports monotonic client round-trip percentiles. |
| 112 | PASS | Is there no cross-framework benchmark claim? | scripts/benchmark_rpc.sh:16-19 defines no cross-framework comparison. |
| 113 | PASS | Does 300-second RPC soak pass? | F/rpc-300s-soak.log reports PASS for 300 seconds, ten Composer restarts and three provider replacements; S/python.log, S/interop-client.log, S/interop-server.log, S/final.rpc.json and S/resources-summary.tsv hold the soak observations. Pre-close response-guard assertions are covered at #116. |
| 114 | PASS | Are final pending calls zero? | S/python.log and S/final.rpc.json record zero pending/inflight calls; python/tests/rpc_soak_probe.py:287-318,383-384 checks fresh pre-close gauges. |
| 115 | PASS | Are final request guards zero? | S/final.rpc.json has a fresh server observation with requests_current=0; S/python.log and S/interop-server.log also record pre-close requests_current=0. |
| 116 | PASS | Are final response guards zero? | S/python.log and S/interop-client.log record pre-close response_guards_current=0; fresh nonempty client observations in S/final.rpc.json also show zero. The Rust-client/C++-server soak lane asserted pre-close idle gauges, and F's 20 standalone interop participants each asserted actual snapshots before close; parent IDLE_VALIDATED markers follow participant exit. |
| 117 | PASS | Are FDs stable? | S/resources-summary.tsv: observed maximum FD delta is 27 against the +64 limit. |
| 118 | PASS | Are threads stable? | S/resources-summary.tsv: observed maximum thread delta is one against the +32 limit. |
| 119 | PASS | Is RSS free of obvious unbounded growth? | S/resources-summary.tsv: observed maximum client RSS growth is about 5.1 MiB against the +64 MiB limit. |
| 120 | PASS | Are there no orphan RPC capabilities? | crates/rbt-compose/src/component.rs:2170-2220 verifies an empty registry and released pressure sources after shutdown; S/final.graph.json has one expected active capability and one valid binding, not an empty graph. |
| 121 | PASS | Are there no stale RPC bindings? | scripts/soak_rpc.sh:292-328 checks bindings refer to the fresh capability; S replacement snapshots show rotated identities. |
| 122 | PASS | Are there no orphan processes? | scripts/soak_rpc.sh:831-838 reaps tracked process IDs; S final reports zero orphan processes (not an OS-wide process enumeration). |
| 123 | PASS | Are there no pending asyncio task warnings? | python/tests/rpc_soak_probe.py:321-325,383-445 checks pending asyncio tasks and performs final cleanup; S/python.log records 1,006 cycles and the separate final audit reports warnings=0. Pre-close live gauges are recorded separately from that final audit. |
| 124 | PASS | Do existing Stream Sensor semantics remain unchanged? | F/inherited-v01.log passes; inherited pressure run retains Sensor stream checks. |
| 125 | PASS | Do existing Control semantics remain unchanged? | F/inherited-v01.log control tests and Q frozen hashes preserve Control behavior. |
| 126 | PASS | Do WaitSet gates remain green? | F/inherited-waitset.log reports PASS. |
| 127 | PASS | Do pressure v0.4 gates remain green? | F/inherited-pressure.log reports PASS including its own 300-second soak. |
| 128 | PASS | Do DX v0.5 gates remain green? | F/inherited-dx.log reports PASS; covered sibling suites are skipped rather than rerun. |
| 129 | PASS | Do MCAP gates remain green? | F/inherited-mcap.log reports PASS including its own soak. |
| 130 | PASS | Does fresh Python wheel pass? | F/inherited-python.log and inherited-python-asyncio.log pass with an isolated fresh CPython 3.12 wheel. |
| 131 | PASS | Does external Rust consumer pass? | F/packaging-external-consumers.log confirms installed Rust consumer result (12). |
| 132 | PASS | Does external C++ consumer pass? | F/packaging-external-consumers.log confirms installed C++ consumer result (32). |
| 133 | PASS | Does packaging include RPC public APIs? | scripts/test_install.sh:138-150,525-535 checks installed public Rust/C++ RPC APIs and external compilation. |
| 134 | PASS | Is MCAP encoding unchanged? | crates/rbt-types/src/lib.rs:36 keeps MCAP encoding at 1. |
| 135 | PASS | Is IDL language unchanged unless absolutely required? | crates/rbt-idl/src/lib.rs:23 keeps IDL language at 1. |
| 136 | PASS | Is descriptor format unchanged unless absolutely required? | crates/rbt-idl/src/lib.rs:25 keeps descriptor format at 1. |
| 137 | PASS | Is control protocol still v2 unless absolutely required? | crates/rbt-compose/src/protocol.rs:13-21 keeps Control protocol at 2 and snapshot at 4. |
| 138 | PASS* | Is iceoryx2 still exactly 0.9.3? | PASS*: Cargo.toml:29,48-52 pins 0.9.3 and applies rbt patches; Q/iox2-vendor-audit.log checks published-source identity plus patch. |
| 139 | PASS | Is network work still paused? | docs/rpc.md:32-33 limits RPC to the local host; network work remains paused. |
| 140 | PASS | Was State<T> not implemented? | Q/source-audit.log and crates/rbt-iox2/src/rpc.rs show the implemented call/endpoint API; no State<T> RPC primitive was introduced. |
| 141 | PASS | Was schema evolution not implemented? | IDL and descriptor source audit show no schema-evolution change. |
| 142 | PASS | Was no generic JSON RPC/reflection system added? | docs/rpc.md and crates/rbt-types/src/lib.rs contain no generic JSON RPC or reflection invocation API. |
| 143 | PASS | Was no custom transport protocol added? | docs/rpc.md and crates/rbt-iox2/src/rpc.rs use native RequestResponse, not a custom payload transport. |
| 144 | PASS | Was no automatic retry added? | docs/rpc.md:45-57 and rpc.rs:1031-1048 show no automatic retry. |
| 145 | PASS | Was no exactly-once claim made? | docs/rpc.md:45-57 disclaims exactly-once execution. |
| 146 | PASS | Is production unsafe still 49? | Q/unsafe-audit.log and unsafe-production-matches.log count 49 rbt-owned production source locations; vendored/dependency unsafe is outside this count. |
| 147 | PASS | Are all frozen layout hashes unchanged? | Q/hash-audit.log checks all five frozen hashes; Q/frozen-hash-test.log passes. |
| 148 | PASS | Can a user understand whether a failure means unavailable, timeout, cancel, or capacity? | docs/rpc.md:117-128 documents typed unavailable, timeout, cancellation, and capacity errors. |
| 149 | PASS | Can a client safely continue after a timed-out or cancelled call? | Q/rust-rpc-stress.log includes both 1000-cycle timeout/late-response and cancellation/reuse tests; each finishes with no in-flight calls. |
| 150 | PASS | Does Rpc<Q,R> now feel like a native rbt primitive rather than a side subsystem? | crates/rbt-compose/src/component.rs:319-345 integrates Rpc into ActivationCtx and normal Requirement lifecycle; CLI RPC projections are covered. |


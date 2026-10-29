# iceoryx2 v0.9.3 RequestResponse audit for typed local RPC

## Verdict

**Unmodified upstream iceoryx2 0.9.3 is blocked by a native correlation
defect.** A server retaining a request from a removed client can send a late
response to a new client occupying the same connection slot. Both clients
start with native request ID/channel zero, and the new pending call accepts
the old payload. This was reproduced using raw iceoryx2 APIs without rbt
wrappers, sidecars, metrics, or Composer. The retained regression is historical
evidence against unmodified source; the current rbt production dependency is
version 0.9.3 with a local patch that validates client identity and stops
request-ID wrap. That patch is not an upstream 0.9.3 release.

The retained regressions are in
[`rpc_resources.rs`](../crates/rbt-iox2/tests/rpc_resources.rs):
`stale_server_request_cannot_satisfy_reopened_clients_first_pending_call` and
`native_stale_response_cannot_cross_client_connection_reuse`. They failed on
the unmodified registry 0.9.3 implementation. The native test returned old marker
`(0xaaaa, 0xdead_beef)` to a fresh client's call that expects a different
response. The rbt-maintained patch is
`third_party/iceoryx2/patches/0001-iceoryx2-stale-response-correlation.patch`;
patch 0002 grows the C FFI response-loan storage to match the patched Rust
layout. Current patched-source correlation regressions passed in the quick
gate. A supplementary request-ID boundary probe passed against the same
patched source during that run, but was not part of `verify_rpc.sh --quick`.
The exact patched source identity is recorded in its logs; a direct vendored
`--locked --offline` crate test could
not start because a Cargo index entry was uncached. See [`rpc-report.md`](rpc-report.md)
for the evidence. Do not describe this patched build as unmodified upstream.

The native API otherwise provides fixed/dynamic loans, user headers, and
bounded resources. It does not provide a native waitable RequestResponse
endpoint; advisory Event sidecars address waiting, but cannot repair native
correlation. The patched candidate and any accepted implementation must keep
these invariants:

1. The rbt request identifier is not put into `Q` or `R`; iceoryx2's internal
   RequestHeader/ResponseHeader correlation is used.
2. Every readiness source is established before its corresponding RPC endpoint
   is published or used, and every wake is treated as advisory rather than as
   a lossless message or a payload count.
3. Each receive path has a finite native WaitSet deadline/interval safety
   probe, so a lost or coalesced sidecar notification cannot leave a request
   or response permanently unwoken.
4. All native buffers, active requests, listeners, and deferred cleanup
   capacities are explicitly bounded. There is no retry-until-delivered RPC
   policy, unbounded queue, hidden executor, or network transport.

The hard-stop rule still applies to unmodified upstream 0.9.3: do not use it
for this RPC contract without the identity and non-wrapping fixes. The rbt
candidate carries those fixes and has functional acceptance on the tested
patched 0.9.3 baseline. A 2026-10-16 pre-repair audit also found
live-destination C++ move-assignment accounting defects and a missing final
response-guard soak assertion; both were repaired and reverified. The final
quick and full gates exited 0, all four C++ configurations passed CTest 7/7,
and the post-repair 300-second soak passed with response-guard assertions.
These product checks are separate from native payload correlation. The source
analysis below describes the unmodified baseline; patch behavior is called out
where it changes it. This acceptance does not mean unmodified upstream is safe
or that a release has been published.

## 2026-10-16 final repair verification

The patched-source correlation checks and post-repair quick gate passed. The
C++ live-destination reproducer passes all five cases; permanent C++ tests cover
those cases plus request-loan capacity, self-move, and endpoint
replacement/expiry. The final quick gate is
[`rpc-quick-20261016T070025Z-2297727`](../target/rpc-verify-logs/rpc-quick-20261016T070025Z-2297727/)
(exit 0); the final full gate is
[`rpc-full-20261016T022204Z-1877923`](../target/rpc-verify-logs/rpc-full-20261016T022204Z-1877923/)
(exit 0). Both match the frozen source manifest and pass its source check. The
full gate passed C++ Debug, Release, AddressSanitizer and UndefinedBehaviorSanitizer
CTest suites (7/7 each), the 15-test idle validator, the full Python sync and
asyncio suites, and the RPC benchmark matrix.

The standalone full-gate interoperability run passed ten Rust/C++/Python directions,
including 4096-call bursts. Each of the 20 participating processes emitted and
asserted its own idle snapshot before endpoint close; after those participants
exited, the parent harness emitted
`IDLE_VALIDATED` after validating the snapshots for all 20 roles. This is
separate from the S soak interop lane, which was one Rust client and one C++
server; both emitted and asserted idle snapshots before endpoint close after
15,200 calls each. S, the final RPC soak, passed the actual 300-second run
with ten healthy Composer restarts (zero deactivate/activate transitions),
three provider replacements including one combined outage, zero resource-growth
violations, and no orphan processes. Its pre-close checks captured fresh
managed client/server metrics; the client response guards and inflight calls
and server requests were zero. The Python probe completed 1,006 cycles, 2,012
responses, 1,006 deadlines and 1,006 cancellations; pre-close gauges showed
pending calls, registrations, inflight calls, response guards and requests at
zero. The separate final pending-task/warning audit reported warnings=0.
Resource maxima were FD delta 27, thread delta one and RSS growth about 5.1 MiB.
The final soak artifacts are in
[`S`](../target/rpc-soak-logs/20261016T031227Z-2044429/).

The full gate also passed inherited Phase4, installed/external-consumer, DX,
v0.1, pressure (332 seconds, five cycles), WaitSet, MCAP, Python and asyncio
chains. The final asyncio run lasted 304 seconds with ten restarts and reported
zero unexpected warnings. The final 150-item checklist in `rpc-report.md` is
149 PASS and 1 PASS* (#138), with zero FAIL, INSUFFICIENT or PENDING. The
pre-repair tally of 145 passing items (including #138 PASS*), 4 FAIL and
1 INSUFFICIENT, along with the unmodified-upstream native failure, remains
historical evidence; the tested rbt-maintained patched
candidate is accepted, but the upstream port has not been submitted and no
published release is claimed.

## Audit basis and exact version

This note audits the base source checkout at
`/home/sw/rbt/target/iceoryx2-v0.9.3`, tag `v0.9.3`, commit
`6ef7c55176a8487ae7ca53c2e0649d5df9a1b3b4`. The workspace pins
`iceoryx2 = "=0.9.3"` (`Cargo.toml:29`; `Cargo.lock:467-490`) and applies
`third_party/iceoryx2/patches/0001` through `[patch.crates-io]`. The C/C++
bootstrap applies the correlation patch and FFI storage patch 0002 to the
matching baseline. No behavior from another upstream version is assumed.

The line references below are source evidence from that checkout. They are
not links to an upstream `main` branch and should be rechecked if the pinned
source is replaced.

## Native correlation and data layout

The Rust port is generic over four user-visible type parameters:
`RequestPayload`, `RequestHeader`, `ResponsePayload`, and `ResponseHeader`
(`iceoryx2/src/port/client.rs:315-330`; `iceoryx2/src/port/server.rs:223-270`).
The user payloads and user headers are separate from iceoryx2's internal
headers.

The native request header is `#[repr(C)]` and contains `node_id`, `client_id`,
`channel_id`, `request_id`, and `number_of_elements`
(`iceoryx2/src/service/header/request_response.rs:22-59`). The native response
header is also `#[repr(C)]` and contains `node_id`, `server_id`, `request_id`,
and `number_of_elements` (`.../service/header/request_response.rs:61-97`).
These headers are allocated in the iceoryx2 chunk before the independent user
header and payload; they are not fields in `Q` or `R`.

On the client, `next_request_id()` uses the native `RequestId` domain and
wraps modulo `RequestId::max_value()` (`iceoryx2/src/port/client.rs:608-617`).
The request send path first checks the configured active-request limit, updates
connections, marks the response channel with the `(channel_id, request_id)`,
and then delivers the request (`.../port/client.rs:214-249`). The client uses
one request channel but allocates separate response channels so one response
cannot fill another request's response buffer
(`.../port/client.rs:470-497`).

The server copies the native request `request_id` and `channel_id` into the
`ActiveRequest` (`iceoryx2/src/port/server.rs:594-622`). A response loan writes
the native response header with that request id, plus the server id and element
count (`iceoryx2/src/active_request.rs:322-367`). Sending the response routes
the native chunk to the originating client connection and channel
(`iceoryx2/src/response_mut.rs:333-350`). However, the unmodified path retains
only a numeric connection slot: `Sender::update_connection` replaces a slot
when its client port identity changes, while a previously received
`ActiveRequest` or loaned `ResponseMut` still points at that slot. The
unmodified source does not revalidate the originating client identity when
the old response is delivered. This breaks correlation across client
replacement even without a request-counter wrap.

`PendingResponse::receive()` drains native response chunks and rejects a
response whose native `request_id` does not match its request, releasing the
discarded chunk before trying the next one
(`iceoryx2/src/pending_response.rs:262-271; 274-340`). The dynamic-slice
implementation applies the same request-id filter
(`.../pending_response.rs:343-417`). Filtering protects distinct native
request IDs on a retained client; it does not distinguish two clients whose
counters both start at zero. Native request IDs also wrap modulo `2^62` in the
unmodified baseline; the ID filter alone does not prove safety across a
complete wrap. Patch 0001 checks that the retained request's originating
client still maps to the same live connection before sending or cleaning its
response channel, and makes request-ID allocation stop at the maximum rather
than wrap. The matched-source boundary test passed in a supplementary probe
run alongside, but not inside, `verify_rpc.sh --quick`. This is evidence for
the rbt patch, not for upstream 0.9.3.

## Fixed payloads, slices, and user headers

The exact Rust API supports all four payload-shape combinations needed by the
transport-neutral descriptor:

- fixed request and fixed response: typed `loan_uninit`, `loan`, `send_copy`,
  and `ActiveRequest::loan_uninit`/`send_copy`
  (`iceoryx2/src/port/client.rs:646-837`; `iceoryx2/src/active_request.rs:286-448`);
- dynamic request slices: `request_response::<[T], R>()`,
  `Client::loan_slice`/`loan_slice_uninit`, and the native element count in
  `RequestHeader` (`iceoryx2/src/port/client.rs:847-1057`;
  `iceoryx2/src/port/server.rs:694-809`);
- dynamic response slices: `request_response::<Q, [T]>()`,
  `ActiveRequest::loan_slice`/`loan_slice_uninit`, and
  `PendingResponse::receive` using the native response element count
  (`iceoryx2/src/active_request.rs:453-500`;
  `iceoryx2/src/pending_response.rs:343-417`); and
- custom payload marker support for byte-oriented integration code through the
  documented unsafe `receive_custom_payload` APIs
  (`iceoryx2/src/port/server.rs:825-880`;
  `iceoryx2/src/pending_response.rs:419-478`).

Request and response user headers are first-class independent types. The
builder changes their types with `request_user_header` and
`response_user_header` (`iceoryx2/src/service/builder/request_response.rs:342-360`).
The typed Rust APIs expose the headers on `RequestMut`, `PendingResponse`,
`ActiveRequest`, and `Response`; for example, request accessors are in
`iceoryx2/src/request_mut.rs:198-212` and pending-response accessors are in
`iceoryx2/src/pending_response.rs:227-243`.

The C++ wrapper has parity for this surface. `iox2/client.hpp:25-105`
exposes typed fixed and slice loans/sends; `iox2/server.hpp:23-66` exposes
typed receive and `has_requests`; `iox2/pending_response.hpp:150-228` exposes
response receive and user-header/payload access; and
`iox2/active_request.hpp:24-111` exposes response loans, sends, connection
state, and request headers. C++ service-builder overloads for
`request_user_header` and `response_user_header` are in
`iox2/service_builder_request_response.hpp:146-154, 215-237`. The conformance
tests exercise fixed, slice, and user-header request/response paths in
`iceoryx2-cxx/tests/src/service_request_response_tests.cpp:280-344,
991-1032, 1051-1091`.

## Resource controls and safe RPC configuration

The exact upstream `Config::defaults.request_response` type is spelled
`RequestResonse` in 0.9.3 (`iceoryx2/src/config.rs:394-400`). Its fields are
the resource/QoS controls that must be considered by the rbt bounded
descriptor:

| Native field | Meaning | 0.9.3 default |
|---|---|---:|
| `max_active_requests_per_client` | server-side active requests per client; also request buffer size | 4 |
| `max_response_buffer_size` | response samples per server connection on a pending response | 2 |
| `max_servers` | maximum server ports | 2 |
| `max_clients` | maximum client ports | 8 |
| `max_nodes` | maximum nodes opening the service | 20 |
| `max_borrowed_responses_per_pending_response` | client response borrows per pending response | 2 |
| `max_loaned_requests` | client request loans in parallel | 2 |
| `server_max_loaned_responses_per_request` | server response loans per active request | 2 |
| `client_expired_connection_buffer` | deferred client-side cleanup slots | 128 |
| `server_expired_connection_buffer` | deferred server-side cleanup slots | 128 |

The fields and their documentation are at `iceoryx2/src/config.rs:401-470`;
the default values are at `.../config.rs:473-494`. The defaults also enable
safe overflow for both directions, use `RetryUntilDelivered` backpressure on
both directions, and enable fire-and-forget requests
(`.../config.rs:476-492`). These defaults are not the rbt RPC contract.

The RPC service must explicitly request bounded, non-overwriting behavior:

- `enable_safe_overflow_for_requests(false)`;
- `enable_safe_overflow_for_responses(false)`;
- `client_backpressure_strategy(BackpressureStrategy::DiscardData)` and
  `server_backpressure_strategy(BackpressureStrategy::DiscardData)` on the
  respective port builders; and
- `enable_fire_and_forget_requests(false)` so a request whose client has
  abandoned its response is not delivered to the server as a work item with
  no possible result.

The request/response builder exposes the service-level settings and their
open-time compatibility checks at
`iceoryx2/src/service/builder/request_response.rs:387-466, 544-629`.
Client and server port builders expose their per-port backpressure and loan
settings at `iceoryx2/src/service/port_factory/client.rs:231-289` and
`.../server.rs:236-307`. `max_servers` must be one for unary RPC so a request
does not produce multiple responses; the descriptor's `max_in_flight` must be
no greater than `max_active_requests_per_client`.

The milestone's bounded defaults are therefore a wrapper policy layered over
these exact native knobs, for example `max_in_flight=4`, `max_clients=16`,
`max_request_loans=2`, `response_buffer_size=1`,
`max_borrowed_responses=1`, and `max_response_loans=1`. The upstream defaults
are recorded above so the wrapper can reject an incompatible existing service
instead of silently inheriting a lossy or unbounded semantic. The wrapper
must also account for the native event service capacity (`max_listeners=16`,
`max_notifiers=16`, `max_nodes=36`, event-id maximum 255 by default), from
`iceoryx2/src/config.rs:354-392`, when sizing the sidecar endpoints.

## Ownership, cancellation, late responses, and disappearance

`PendingResponse` owns the request sample and its response channel. Its `Drop`
decrements the active-request counter and closes the channel
(`iceoryx2/src/pending_response.rs:142-178, 191-197`). The native channel close
is the cleanup operation for timeout and cancellation: the rbt wrapper can
drop the pending response after atomically winning the terminal state, and a
response sent after that point is not deliverable to that pending response.
The received `Response` itself releases its borrowed chunk on drop
(`iceoryx2/src/response.rs:84-95`).

`ActiveRequest::Drop` releases the request chunk and closes the corresponding
response channel (`iceoryx2/src/active_request.rs:174-207`). Its
`is_connected()` becomes false when the client drops the pending response
(`.../active_request.rs:224-237`). Conversely,
`PendingResponse::is_connected()` becomes false after all corresponding server
active requests close (`.../pending_response.rs:212-225`). The upstream
conformance tests directly cover this relationship:
`iceoryx2/conformance-tests/src/pending_response.rs:63-105` and
`.../active_request.rs:63-133`.

With `enable_fire_and_forget_requests(false)`, the server filters a request
whose client connection has disappeared before returning an `ActiveRequest`
(`iceoryx2/src/port/server.rs:653-691`, including the
`!active_request.is_connected()` check). With fire-and-forget enabled, the
server may receive such a request with an invalid response connection; that
mode is unsuitable for unary RPC. The corresponding conformance cases are
`iceoryx2/conformance-tests/src/service_request_response.rs:385-457`.

Connection disappearance is safe but cleanup is deliberately bounded and can
be deferred. On receiver removal, iceoryx2 retains a disconnected connection
while it has queued data or outstanding borrows, placing it in the configured
expired-connection buffer (`iceoryx2/src/port/details/receiver.rs:256-324`).
If the bounded buffer is full, data without borrows may be discarded; removing
a connection that still has borrows is treated as unsafe and can fatal-panic
(`.../receiver.rs:275-317`). The sender side reclaims returned samples during
the next connection/update cycle (`iceoryx2/src/port/details/sender.rs:491-510,
522-533`). The client/server constructors add active connection capacity to
the configured expired-buffer capacity (`iceoryx2/src/port/client.rs:500-524`;
`iceoryx2/src/port/server.rs:364-404`).

This is the required caveat: dropping a timed-out RPC closes its native
correlation channel immediately, but physical chunk reclamation may be
deferred until the next connection/update cycle and is bounded by the
expired-connection capacities. The rbt resource profile must keep those
capacities sufficient for its maximum in-flight/disconnected population and
must not describe the sidecar as lossless cleanup. Late responses are ignored
or reclaimed by the native channel/response path; they are never retried or
rebound to a new logical call.

## Native waitability audit

The 0.9.3 `WaitSet` attaches an object implementing
`SynchronousMultiplexing` (`iceoryx2/src/waitset.rs:648-697`). The exposed
RequestResponse `Client` and `Server` ports have no such implementation and
their receive/has-response operations are non-blocking calls over internal
zero-copy connections (`iceoryx2/src/port/client.rs:208-270`,
`.../port/server.rs:548-583`). In contrast, Event `Listener` explicitly
implements both `FileDescriptorBased` and `SynchronousMultiplexing`
(`iceoryx2/src/port/listener.rs:139-154`). Therefore an RPC client or server
cannot attach its native RequestResponse port directly to a WaitSet in this
version.

This is a wakeup limitation, not a data-path limitation. It rules out using a
RequestResponse port itself as the only blocking primitive, but it does not
block an Event sidecar with a bounded safety probe.

## Required dual Event sidecar contract

Use a distinct sidecar protocol, `RpcSidecarV1`, rather than reusing the
stream sidecar identity. For an RPC service name `S`, the physical endpoints
are:

```text
S                         iceoryx2 RequestResponse<Q, R>
S/request-ready           Event: server-side request readiness
S/response-ready          Event: client-side response readiness
```

The event IDs are fixed protocol constants (for example, one readiness event
per sidecar); they are not request IDs, response IDs, sequence numbers, or
payload counts. A notification says only that the associated native port may
be worth checking. It can coalesce multiple requests/responses, wake with no
payload, or be absent; correctness must come from bounded native drains and
the interval/deadline probe.

The required construction ordering is:

```text
server: create request-ready Listener + response-ready Notifier
        -> create/open RequestResponse server
        -> publish/accept server activation

client: create request-ready Notifier + response-ready Listener
        -> create/open RequestResponse client
        -> issue a request
```

The listener must be registered before the corresponding notifier can send a
notification. This is the same ordering requirement visible in the upstream
Event implementation: the listener creates its underlying event concept and
only then adds its listener ID to dynamic configuration
(`iceoryx2/src/port/listener.rs:185-253`); a notifier refreshes listener
connections before notifying and only adds its notifier ID after its channels
exist (`iceoryx2/src/port/notifier.rs:368-448`). The rbt wrapper must create
and retain both sidecar ports before exposing the RPC endpoint to application
code, and must perform an initial nonblocking native drain after setup to close
the final construction race.

The dual direction is intentional. A server cannot infer that a request is
ready from an Event it owns unless the client has a request notifier, and a
client cannot await a response from a server-owned event unless it has a
response listener. A per-pending-call response listener may be used when
bounded by the configured client/in-flight capacity; a shared listener is also
valid if it preserves bounded fairness and does not serialize unrelated calls.

The cancellation/expiry wake is a separate bounded Event sidecar owned by the
activation (or an equivalent activation-local listener/notifier pair). It
wakes the blocked wait; it does not carry the cancellation reason or any
payload. A terminal-state atomic on each pending call must decide exactly once
between response, deadline, cancellation, and activation expiry.

## Advisory/coalesced event semantics and safety probe

iceoryx2 Event `Notifier` broadcasts to currently connected listeners and
returns the number successfully triggered. Disconnected listeners are removed;
other notification failures are warned and do not become a lossless queue
(`iceoryx2/src/port/notifier.rs:504-534`). The event CAL can report
`FailedToDeliverSignal` when its underlying signal/socket cannot enqueue a
notification (`iceoryx2-cal/src/event/mod.rs:30-36`; Unix datagram behavior at
`iceoryx2-cal/src/event/unix_datagram_socket.rs:182-200`). Event notifications
therefore must be treated as advisory. They are not a durable wake log and
must never be used as a request/response acknowledgment.

Listener APIs provide nonblocking `try_wait_one`/`try_wait_all` and timed or
blocking waits (`iceoryx2/src/port/listener.rs:266-330`). Spurious wakeups are
explicitly documented for `blocking_wait_one` (`.../listener.rs:323-330`), and
an event can wake a waiter even though the RequestResponse queue is empty.
The wait handler must drain only a finite number of native items per wake and
then return to the WaitSet so cancellation, other RPC calls, and fairness are
bounded. It must also tolerate a stale listener wake after a port has closed.

Every RPC wait loop must attach a finite native WaitSet interval or deadline
as a safety probe. `WaitSet::attach_interval` supplies a timer attachment
(`iceoryx2/src/waitset.rs:699-711`), and
`wait_and_process_once_with_timeout` waits until a notification or the
provided bounded timeout (`.../waitset.rs:830-906`). On each sidecar wake,
interval tick, or initial pass, the handler performs a bounded nonblocking
drain of the native `Server::receive()` or `PendingResponse::has_response()` /
`receive()` operation and checks connection/terminal state. The final deadline
check is monotonic and linearized against a queued native response; a response
already visible at the deadline may win, otherwise timeout wins and the native
pending response is dropped.

The interval is a bounded safety probe, not a payload transport, polling queue,
or busy spin. The thread sleeps inside the native WaitSet between readiness
events and the finite probe ticks; it never loops continuously with
`try_wait_*` or copies `Q`/`R` through an intermediate queue. The probe exists
only because exact 0.9.3 does not make RequestResponse itself waitable and
because Event notification is advisory.

## Server-side receive and unary response implications

`Server::receive()` is nonblocking and returns an `ActiveRequest` only after
updating client connections (`iceoryx2/src/port/server.rs:548-583,
653-691`). It may be driven by the request-ready listener and bounded safety
probe. The active request can be handed to a bounded application worker, but
the wrapper must not create an unbounded task per request.

Native iceoryx2 RequestResponse supports a response stream: one
`ActiveRequest` can loan and send multiple `ResponseMut`s while connected
(`iceoryx2/src/active_request.rs:197-237, 322-405`). The rbt unary API must
constrain that surface. A response wrapper should consume or otherwise
linearly own the `ActiveRequest` until one response is sent, then close/drop
it; there is no exactly-once guarantee across crashes, but the API can prevent
two successful rbt responses for one local call. If the client cancels first,
`ActiveRequest::is_connected()` and the native send path determine whether the
response can still be delivered; the wrapper reports the terminal state
according to its atomic race policy.

## C++ parity and cross-language constraint

The C++ API is a binding over the same Rust/FFI RequestResponse semantics, not
a second transport. Its `PendingResponse` and `ActiveRequest` are move-only
RAII handles (`iox2/pending_response.hpp:24-111`;
`iox2/active_request.hpp:24-111`); their destructors map to the native drop
cleanup. The C++ service builder exposes the same fixed/slice payload and
user-header type choices, and C++ tests cover response routing, disconnection,
slice reallocation, and user headers in
`iceoryx2-cxx/tests/src/service_request_response_tests.cpp`.

The C++ binding does not add native RequestResponse WaitSet support: the
waitable source remains the Event `Listener`/file descriptor surface. The
sidecar names, creation ordering, bounded probe, event caveats, and no-payload-
transport rule must therefore be identical for Rust and C++.

The pre-repair C++ wrappers had a separate move-assignment accounting defect.
Assigning into a live `PendingRpcCall`, `RpcRequest`, or `RpcResponseLoan`
skipped rbt cleanup/accounting performed by the destination destructor. The
five-case reproducer observed stale `in_flight_current`, an inflated high-water
counter, a missing `calls_cancelled` increment, and stale request-current
counters. The repair's five-case reproducer passed: terminal calls and
abandoned requests were fully accounted, high-water matched the configured
limit, and cancellation incremented exactly once. All four final-gate C++
configurations passed CTest 7/7. The original reproduction is retained as
historical evidence; it does not establish a payload-memory leak or
shared-memory corruption.

## Audit conclusion

The unmodified pinned source does not meet the correlation requirement. The
rbt patch makes the correlation and request-ID boundary checks pass on the
current 0.9.3-derived candidate. The tested patched candidate has functional
acceptance: the final full and quick gates exited 0, the C++ move-assignment
repair passed all four C++ configurations, and the response-guard assertion
passed in the 300-second soak. This verdict is specific to the rbt-maintained
0.9.3 patches, not stock upstream or a published release. The bounded
local-only design is:

```text
bounded Q loan -> native RequestResponse correlation -> bounded R loan
                       ^                         ^
                 request-ready              response-ready
                       \_____ advisory Event sidecars _____/
                                      + finite WaitSet probe
```

The sidecars solve wakeup only. The rbt source patch supplies client-origin
validation and non-wrapping request IDs over the 0.9.3 baseline. The
implementation must preserve the bounded deferred-cleanup caveat above,
explicitly override unsafe upstream RequestResponse defaults, and keep all
timeout/cancel paths on native drop/connection semantics. Functional
acceptance applies to the tested rbt-maintained 0.9.3 candidate; it is not a
claim that unmodified upstream passed or that a release has been published.
See the itemized report for the final checklist and historical pre-repair
results.

## Current unified source identity (2026-10-16)

The production workspace now resolves the `iceoryx2` Cargo package at version
`0.10.0` through `[patch.crates-io]` to
`third_party/iceoryx2-rbt/source/iceoryx2`. The fork build identifier is
`0.10.0-rbt.2`; its upstream base is v0.10.0 commit
`135d09dd8b29f321f1725920d434864c4e512378`. The recorded unified source-diff
fingerprint in `third_party/iceoryx2-rbt/SOURCE_DIFF_SHA256` is
`b0e90730fee476487b0c47dba0a272b0d0df9c5480629f8ac64849b8882d91d1`.
`scripts/bootstrap_iceoryx2_rbt.sh --source-only` checks the production
checkout, and `--verify` reapplies the patch series and checks that exact
fingerprint.

All v0.9.3-specific source analysis, reproductions, and gate results above are
historical evidence for the 0.9.3-derived source identified in those sections.
They do not establish behavior or verification results for this unified
v0.10.0 source; no historical result is being carried forward as new evidence.

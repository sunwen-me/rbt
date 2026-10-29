# Typed local RPC

See the [documentation index](README.md) for current SDK versions and related
feature guides.

Typed local RPC was introduced in v0.6. This guide describes the current rbt
SDK release, 0.8.0, on the maintained iceoryx2 0.10.0-rbt.2 baseline. The
current frozen protocol and snapshot version axes are listed in the
[documentation index](README.md).

The Rust onboarding example in
[`examples/08-rpc`](../examples/08-rpc/) demonstrates a bounded fixed
request/response endpoint. Its provider accepts `Frame` requests and returns
`Detection` responses through `ActivationCtx::rpc_server`; its client resolves
the `EchoRpc` requirement with `ActivationCtx::require_rpc`.

Run it with three terminals from the repository root:

```bash
cargo run -p composer
cargo run -p rpc-onboarding -- provider
cargo run -p rpc-onboarding -- client
```

For a one-command smoke/demo run, use the supervised launcher (Ctrl-C stops
Composer and both roles):

```bash
./examples/08-rpc/run.sh
```

## Data and control planes

Composer resolves the logical requirement and publishes low-rate lifecycle and
capability metadata.  It is outside the payload path: the `Frame` request is
loaned from the native request pool, and the `Detection` response is loaned by
the provider and returned as a native response guard.  No request or response
bytes are serialized through Composer.

This endpoint is local-only shared-memory IPC.  The descriptor and payload
contract are not a network wire protocol and do not imply remote-host RPC.

The RPC sidecar only supplies advisory readiness wakes.  Native iceoryx2
request/response correlation is authoritative: the native pending response
accepts only the response for its own request.  Sidecar event IDs are not
request IDs, sequence numbers, or payload counts, and application payloads do
not need a correlation field.

## Bounded calls and lifecycle

`RpcRequestLoan` and `RpcResponseLoan` are move-only ownership guards.  A
request is sent once; a response is sent once.  The example waits with a
bounded 250 ms monotonic deadline.  `DeadlineExceeded` closes that logical
pending call, but it does not prove whether the provider already executed the
request.  Therefore the example explicitly does not retry automatically.  A
caller that needs retry must define an idempotency policy outside this fixed
transport example.

An RPC timeout means the caller no longer accepted a response before its
deadline; it does not prove that the server did not observe or execute the
request. rbt does not automatically retry RPC calls. Client cancellation
abandons the local wait; it is not a distributed cancellation guarantee for
server-side work. A successful framework response also does not make a
physical-world side effect transactional: application success/failure belongs
in the ordinary response message `R`.

The pending call's `RpcCallCancelHandle` can cancel a call from another task.
When an activation is withdrawn, its `EffectScope` expires the RPC endpoint
before joining its tasks, so blocked waits finish with `ActivationExpired` (or
with `Cancelled` if cancellation won the terminal-state race).  Provider
replacement follows the same lifecycle rule: the replacement receives a new
`CapabilityInstanceId` and physical endpoint.  The old activation's pending
call and response cannot be rebound to that new provider; the next client
activation resolves and opens the new binding.

## Inspection and metrics

RPC resource limits are fixed when the endpoint opens (`max_in_flight`, loan
ceilings, response buffer, and borrow limits).  `rbt rpc info <capability>` is
an inspection view, not a data-plane dependency.  Runtime RPC pressure metrics
are low-rate snapshots collected with lifecycle reporting; they are not a
per-call event stream.  An unavailable metrics snapshot is different from an
available metric whose value is zero.  Do not interpret an unavailable value as
evidence that no calls, loans, or failures occurred.

MCAP recording and playback remain stream-only.  They can inspect and replay
supported stream capabilities, but they do not capture or replay RPC calls.

## Fixed and dynamic shapes

The native Rust and C++ APIs support all four descriptor shapes without a
payload serializer or transport-owned `Vec`: fixed → fixed, fixed → slice,
slice → fixed, and slice → slice. A slice loan takes an explicit element count
and validates it against the descriptor's bounded resource ceiling before the
native loan is created. `Image` and `PointCloud` retain their canonical
32-byte headers; their elements remain in the native shared-memory slice.

Python exposes the same shape matrix through `RpcDescriptor.fixed`,
`fixed_slice`, `slice_fixed`, and `slice`. Slice request and response loans
take `element_count`; `Image` headers are set with `set_image_header`, and
received images are viewed with `as_image`. `payload_view()` and NumPy views
borrow the native guard directly. A writable derived view keeps its lease root
alive and therefore prevents sending the loan until every alias is released,
including when the final alias is dropped by another Python thread.

`RpcClient.call_async` and `RpcServer.receive_async` share one native WaitSet
bridge thread per endpoint, not one thread per call. Task cancellation invokes
the native terminal arbiter, unregisters the pending operation, and leaves the
same client reusable. Native deadlines and cancellation have the same
late-response rule as synchronous calls: a response that loses the terminal
CAS is harmless and cannot satisfy a later request.

## Capacity and errors

The defaults are `max_in_flight=4` per client, `max_clients=16` per physical
server, `max_request_loans=2`, `response_buffer_size=1`,
`max_borrowed_responses=1`, `max_response_loans=1`, and a one-element ceiling
for each slice direction. Set explicit slice ceilings for an Image or
PointCloud endpoint. The native service has one server, static allocation,
safe overflow disabled in both directions, fire-and-forget disabled, and
`DiscardData` backpressure. A full path returns an error; it does not silently
overwrite an older request or response. Applications explicitly receive and
respond, and choose their own bounded worker policy.

| Result | Meaning and recovery |
| --- | --- |
| `AbiMismatch` | Request or response metadata differs; preflight rejects before opening native endpoints. |
| `ServerUnavailable` | No server connection accepted the request; managed provider loss also drives the existing Requirement lifecycle. |
| `DeadlineExceeded` | This call stopped accepting a response by its monotonic deadline. The same valid client can call again. |
| `Cancelled` | Local call abandonment won the terminal race. The same valid client can call again. |
| `ActivationExpired` | Endpoint authority ended. Resolve/open the next activation's fresh binding. |
| `ClientCapacityReached` | This client's configured active-request ceiling is reached. Release existing calls before a later application-directed attempt. |
| `ServerCapacityReached` / `ResponseCapacityReached` | The native bounded delivery path did not accept this request/response. |
| `LoanPoolExhausted` / `BorrowLimitReached` | A native loan or borrow bound applies. Release the relevant guards. No hidden copy bypasses the limit. |
| `InvalidSliceLength` | The requested element count exceeds the advertised ceiling. |
| `TransportFailure` | A native operation failed outside the categories above; inspect its operation/detail and endpoint lifecycle. |

An individual call error does not itself change component state. Propagating
an error from a critical task retains the normal supervisor contract:
activation failure, capability withdrawal, and dependent components becoming
BLOCKED. Background task policy and deterministic provider resolution are
unchanged. There is no hidden server executor or load balancer.

## Metric definitions

Client metrics are scoped to one native client and, for managed endpoints,
its client activation and bound `CapabilityInstanceId`. Server metrics are
scoped to one physical server capability. Replacement creates fresh counters;
old guards retain only their old counter state. Cumulative counters saturate,
and current gauges decrement when their owning native wrapper is released.
Snapshots read atomics individually: each counter is exact for its event, but
a snapshot during concurrent work is not a transaction across all fields.

| Client field | Counted event |
| --- | --- |
| `calls_attempted` | Request send attempted, including pre-start failures. |
| `calls_started` | Native send established a server-connected pending call. |
| `calls_completed` | Response won terminal arbitration. |
| `calls_failed` | A pre-start failure or an otherwise failed/abandoned pending call. Deadlines, cancellation and expiry have separate counters. |
| `calls_timed_out`, `calls_cancelled`, `calls_activation_expired` | The corresponding terminal outcome; each started call contributes at most once. Expiry before send is also observable. |
| `in_flight_current`, `in_flight_high_water` | Currently started, nonterminal calls and their observed peak. A retained completed response is no longer in flight. |
| `capacity_rejections`, `server_capacity_rejections`, `server_unavailable` | Explicit pre-start client bound, native request-path rejection, and absent server connection. |
| `request_loan_failures`, `response_receive_failures` | Native request loan and response receive errors. |
| `call_duration_count`, `call_duration_total_ns`, `call_duration_max_ns` | Bounded monotonic aggregates from started call to terminal completion/drop; no call history is retained. |

| Server field | Counted event |
| --- | --- |
| `requests_received` | Native request returned to the application. |
| `requests_current`, `requests_high_water` | Live request ownership, including ownership moved into an unsent response loan, and its peak. |
| `responses_attempted` | Application attempted to obtain a response loan. |
| `responses_sent`, `responses_failed` | Native response send succeeded or a response operation failed. |
| `responses_abandoned` | Request/response ownership dropped without a successful response. |
| `request_borrow_failures`, `response_loan_failures` | Native receive/borrow and response-loan errors. |

These are ownership and transport counters, not proof of business execution.
There is no separate active-response-guard telemetry field; retained response
sample lifetime is checked through guard/lease ownership and final teardown.

The Python lifecycle helpers are covered in the
[Python Component runtime](python-component.md). The current [Time and clocks](time.md)
and [spatial transforms](tf.md) guides describe identified timestamps and the
TF static snapshot RPC pair.

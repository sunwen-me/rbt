# Fixed RPC onboarding (v0.6)

This is the smallest Rust request/response graph.  It uses a fixed `Frame`
request and a fixed `Detection` response so the payload stays in native
iceoryx2 shared memory from the client loan through the provider response.

Build the example once:

```bash
cargo check -p rpc-onboarding
```

For a one-command smoke/demo run, let the example build and supervise Composer
and both roles (Ctrl-C terminates all three children):

```bash
./examples/08-rpc/run.sh
```

For manual inspection, start Composer in one terminal, then start each role in
its own terminal.  The same package has one command for each role:

```bash
cargo run -p composer
cargo run -p rpc-onboarding -- provider
cargo run -p rpc-onboarding -- client
```

The provider calls `ActivationCtx::rpc_server::<EchoRpc, Frame, Detection>`.
Each activation gets a fresh `CapabilityInstanceId`, opens its bounded native
endpoint, and publishes the capability only after that endpoint is ready.  The
client declares the `echo` requirement and calls
`ActivationCtx::require_rpc::<EchoRpc, Frame, Detection>`; Composer resolves
the binding, but it never sees a request or response payload.

The client fills a `RpcRequestLoan` in place, sends it, and receives a borrowed
response guard.  The wait has a 250 ms monotonic deadline.  A
`DeadlineExceeded` result means the remote operation may or may not have
executed, so this example logs “no automatic retry”, advances to a new
sequence, and does not silently issue a duplicate request.  Activation
cancellation uses the pending call's cancellation handle; activation teardown
or provider replacement also wakes the call and is reported as `Cancelled` or
`ActivationExpired`.

For a deterministic slow-call demo, delay the provider's first request by more
than the fixed 250 ms deadline:

```bash
RBT_RPC_DEMO_DELAY_MS=1000 ./examples/08-rpc/run.sh
```

`RBT_RPC_DEMO_DELAY_MS` is optional and bounded to `0..=5000` ms.  It delays
only sequence `1`, so the client logs one `DeadlineExceeded` with an unknown
remote outcome, then continues with sequence `2` as a distinct call.  A late
response send for that selected demo call may fail after the client deadline;
the provider logs that expected one-call condition and remains available for
the next call.

To exercise replacement, stop the provider and observe the dependent client
become `BLOCKED` while its requirement is unresolved.  Start the provider
again: the new activation publishes a fresh physical capability instance,
Composer reactivates the client, and the client opens a new endpoint.  A call
belonging to the old activation is never rebound to the replacement; only new,
distinct sequences continue after reactivation.

This is a local-only shared-memory RPC example; it is not a network RPC or a
remote-host wire protocol.  The existing MCAP workflow is stream-only: it does
not record or replay RPC request/response calls.

See [`docs/rpc.md`](../../docs/rpc.md) for the fixed RPC contract,
correlation, timeout, lifecycle, and metrics notes.

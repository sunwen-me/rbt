# Runtime pressure semantics

This document records the implementation-level basis for rbt's stream pressure
contract. Production uses maintained iceoryx2 `0.10.0-rbt.2` from
[`third_party/iceoryx2-rbt/source`](../third_party/iceoryx2-rbt/source), as
described in the [unified baseline report](iceoryx2-unified-report.md).
Statements under “iceoryx2 behavior” are checked against that source;
statements under “rbt policy” are choices made by rbt. Timings and counts under
“observed” are test results, not portable iceoryx2 service-level guarantees.
The former 0.9.3 source audit remains available in
[runtime-pressure-notes.md](runtime-pressure-notes.md) and is historical.

## Vocabulary

- **Loan** is a mutable publisher allocation before it is sent or dropped.
- **Borrow** is a subscriber's guarded reference to a received allocation.
- **Retention** is the time a Loan, Subscriber Queue entry, history entry, or
  Borrow keeps an allocation referenced.
- **Subscriber Queue** is the bounded unread-offset queue for one
  publisher-to-subscriber connection.
- **Loan Pool** is a publisher's data segment. It is not a Subscriber Queue.
- **Backpressure** is the publisher-side response when a non-overflowing
  Subscriber Queue cannot accept a delivery.
- **Drop** is replacement of an unread delivery for one subscriber, or
  abandonment of an unsent Loan.
- **Stall** means a connected subscriber does not call `receive()`.
- **Pressure** is bounded resource saturation or sample loss caused by Loans,
  Subscriber Queues, Borrows, or failed delivery.

Queue full, Loan Pool exhausted, and Borrow limit reached are distinct states
and have distinct typed errors.

## Maintained iceoryx2 0.10.0-rbt.2 behavior

The implementation creates an independent submission queue and completion
queue for every publisher/subscriber connection. Sending iterates the
connections and increments the allocation reference count for every accepted
delivery. The returned success value is the number of accepting connections.
The factory dynamic configuration exposes only aggregate connected subscriber
count; it does not expose public per-subscriber queue occupancy or active
Borrow count. See [connection construction](../third_party/iceoryx2-rbt/source/iceoryx2/src/port/details/sender.rs#L80-L137),
[fanout and delivery results](../third_party/iceoryx2-rbt/source/iceoryx2/src/port/details/sender.rs#L226-L352),
and the [rbt aggregate subscriber observation](../crates/rbt-iox2/src/lib.rs#L441-L450).

With safe overflow, a full connection queue replaces its oldest unread offset.
That replacement is local to the lagging subscriber. An already borrowed
sample has left the queue and cannot be overwritten. The public send result
does not contain the replacement count, so rbt reports generic transport Drop
counts as unknown. A test subscriber may separately estimate Drops from
sequence gaps. The maintained queue returns the replaced offset internally,
and the native sender releases its reference without exposing an overflow
count in the result ([queue push](../third_party/iceoryx2-rbt/source/iceoryx2-cal/src/zero_copy_connection/common.rs#L675-L737),
[sender handling](../third_party/iceoryx2-rbt/source/iceoryx2/src/port/details/sender.rs#L342-L352)).

`receive()` checks the per-subscriber Borrow limit before taking another
offset. At the limit it returns `ReceiveError::ExceedsMaxBorrows`. Dropping the
received guard returns the offset through the completion queue. A later
publisher operation reclaims that reference ([native receive/release](../third_party/iceoryx2-rbt/source/iceoryx2-cal/src/zero_copy_connection/common.rs#L982-L1039),
[rbt receive error mapping](../crates/rbt-iox2/src/lib.rs#L375-L379)).

`loan()` checks the publisher's maximum parallel Loans synchronously. At the
limit it returns `LoanError::ExceedsMaxLoans` without blocking. Dropping an
unsent Loan immediately returns it. `OutOfMemory` is also classified as Loan
Pool exhaustion because the caller has no allocation to use; other Loan
errors remain transport errors ([native allocation](../third_party/iceoryx2-rbt/source/iceoryx2/src/port/details/sender.rs#L472-L509),
[rbt error mapping](../crates/rbt-iox2/src/lib.rs#L366-L373)).

Without a preallocation override, the publisher's worst-case sample count is:

```text
max_subscribers * (subscriber_max_buffer_size + subscriber_max_borrowed_samples)
+ history_size
+ publisher_max_loaned_samples
```

This is why retention within configured limits does not consume the separate
publisher Loan allowance, while still keeping bounded shared-memory
allocations referenced. The formula is implemented in the maintained
[publish/subscribe static configuration](../third_party/iceoryx2-rbt/source/iceoryx2/src/service/static_config/publish_subscribe.rs#L78-L90).

For non-overflow delivery, the sender visits every connection even after one
receiver rejects the offset. `DiscardDataAndFail` records an error for that
receiver and fanout continues. The final send returns the stored error and no
recipient count. Therefore a failure can coexist with successful deliveries to
other subscribers ([fanout result handling](../third_party/iceoryx2-rbt/source/iceoryx2/src/port/details/sender.rs#L402-L444),
[full-queue action](../third_party/iceoryx2-rbt/source/iceoryx2-cal/src/zero_copy_connection/common.rs#L739-L823)).

When a subscriber disappears, sender connection removal reacquires its used
offsets and releases their references. A subscriber can retain an expired
publisher connection while unread or borrowed samples still depend on its data
mapping. These are source-backed mechanisms; cleanup latency is only reported
from bounded process tests ([sender cleanup](../third_party/iceoryx2-rbt/source/iceoryx2/src/port/details/sender.rs#L522-L567),
[deferred receiver cleanup](../third_party/iceoryx2-rbt/source/iceoryx2/src/port/details/receiver.rs#L260-L315)).

## rbt policy

The existing profile semantics are unchanged and the resource values are now
set explicitly in Rust and C++:

| Profile | Subscriber Queue | history | safe overflow | max Loans | max Borrowed | publisher response |
| --- | ---: | ---: | --- | ---: | ---: | --- |
| Sensor | 1 | 0 | true | 2 | 1 | replace oldest unread per lagging subscriber |
| Control | 4 | 0 | false | 2 | 1 | `DiscardDataAndFail` and typed `QueueFull` |

`StreamResourceConfig` exposes only `max_loans` and `max_borrowed` without
inventing a new profile. Zero is rejected rather than relying on transport
clamping. Queue depth, history, and safe-overflow remain frozen profile policy;
they are not tuning knobs. Current profile values are defined by
[`StreamSemanticConfig`](../crates/rbt-core/src/lib.rs#L25-L48), resource
defaults by [`StreamResourceConfig`](../crates/rbt-types/src/lib.rs#L327-L369),
and endpoint configuration by the [rbt stream wrapper](../crates/rbt-iox2/src/lib.rs#L433-L467).

Rust and C++ wrappers count Loans in use, Loan failures, send attempts,
successful sends, send failures, aggregate connected subscribers, received
samples, active Borrows, and Borrow-limit failures. Counters are atomics on the
local wrapper path. Snapshot serialization happens only on inspection or the
component runtime's at-most-once-per-second changed-state publication.

Generic wrappers do not understand message sequence fields, so
`estimated_dropped_samples` is absent rather than zero. The Control
backpressure callback may attach the offending iceoryx2 receiver-port id to a
`QueueFull` event. That opaque 32-hex transport id is not a subscriber
process/component identity; queue occupancy and per-subscriber transport Drop
counts remain unknown.

Pressure is keyed by the physical service name. Composer accepts a provider
observation only when both its `ProcessInstanceId` and service match the active
capability descriptor. Since the service contains `CapabilityInstanceId`, a
replacement receives a new identity and old counters cannot attach to it.

## Historical observed deterministic cases

The `pressure_semantics` suite uses fixed queue/Loan/Borrow limits and bounded
watchdogs. These results are from the accepted rbt v0.4 report, which used
iceoryx2 0.9.3 on the named verification host. They are preserved as historical
measurements; they do not describe current-baseline performance. The pressure
policy was retested on the unified baseline, while these timings and counts
remain host-specific. See the [v0.4 pressure report](pressure-tuning-report.md).

- In two final full-HD RGB8 runs, the Sensor publisher completed all 301 sends
  with four connected subscribers. Fast and 33 ms readers each received 301
  with no gaps; the 200 ms reader received 69–72 and estimated 229–232 Drops;
  the stalled reader called receive zero times.
- Two retained 20 MiB PointCloud samples reached the configured Borrow limit.
  A fast peer received all five sends, the next holder receive returned
  `BorrowLimitReached`, publisher Loan failures stayed zero, and releasing one
  Borrow made the newest queued sample receivable in the same millisecond. The
  final strict-soak run processed the five large samples at 41.747 samples/s; this
  is an observed correctness rate, not a peak claim.
- With `max_loaned_samples=2`, two concurrent Loans succeeded, the third failed
  promptly with `LoanPoolExhausted(ExceedsMaxLoans)`, and dropping one restored
  loanability.
- In Control, queues for A and B were drained while C retained sequences 0–3.
  Sending sequence 4 returned `QueueFull`; A and B received sequence 4 and C
  did not. This proves non-transactional multicast.
- In the latest bounded process-fault run, subscriber SIGKILL recovery was
  observed in 6 ms and fresh-publisher recovery after killing a process with
  two unsent Loans was observed in 256 ms. These are host observations, not
  timing guarantees.

The process-fault and cross-language test logs are the authority for observed
cleanup latency and parity; they must never be promoted into a hard timing SLA.

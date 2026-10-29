# Runtime pressure source notes

> Historical native baseline: the source-backed findings below audit former
> upstream `0.9.3` only. Production now uses maintained iceoryx2
> `0.10.0-rbt.2`; see the [unified baseline report](iceoryx2-unified-report.md).

Status: historical native source audit for the multi-subscriber pressure
milestone. All dependency findings labelled **source-backed** cover former
upstream `0.9.3` only. The rbt pressure policy is unchanged and was retested on
the unified `0.10.0-rbt.2` baseline. Results labelled **observed** come only
from the named bounded rbt tests.

This 0.9.3 source audit is not a promise about another iceoryx2 release. The
user contract belongs in `docs/pressure.md`.

## Vocabulary

- **Loan**: a mutable sample allocation owned by a Publisher before it is sent
  or dropped.
- **Borrow**: a Subscriber's guarded reference to a received sample.
- **Retention**: time for which a Loan, a Subscriber Queue entry, history, or a
  Borrow keeps an allocation referenced.
- **Subscriber Queue**: the unread offsets in one publisher-to-subscriber
  connection. It is not the Loan Pool and it does not include active Borrows.
- **Loan Pool**: one Publisher's preallocated data segment from which Loans are
  allocated. It is not a Subscriber Queue.
- **Backpressure**: the Publisher-side response when a non-overflowing
  Subscriber Queue cannot accept another offset.
- **Drop**: removal of an unread delivery for one subscriber, or abandonment of
  an unsent Loan. A Drop is not necessarily a send failure.
- **Stall**: a connected Subscriber that does not call `receive()`.
- **Pressure**: bounded resource saturation or sample loss caused by Loans,
  Subscriber Queues, Borrows, or delivery failure.

`queue full`, `pool exhausted`, and `borrow limit reached` are different
conditions and must remain different diagnostics.

## iceoryx2 0.9.3 behavior (source-backed)

### Configuration and allocation

- The global iceoryx2 publish/subscribe defaults are 8 maximum subscribers, 2
  maximum publishers, subscriber maximum buffer size 2, subscriber maximum
  borrowed samples 2, publisher maximum loaned samples 2, history 0, and safe
  overflow enabled. Source: `iceoryx2-0.9.3/src/config.rs`,
  `PublishSubscribe::default`.
- A publisher's local `max_loaned_samples` defaults from
  `publisher_max_loaned_samples` and can be overridden on the publisher
  builder. Source:
  `iceoryx2-0.9.3/src/service/port_factory/publisher.rs`,
  `PortFactoryPublisher::new` and `max_loaned_samples`.
- Each Publisher owns a data segment. Without a preallocation override,
  iceoryx2 preallocates

  ```text
  max_subscribers * (subscriber_max_buffer_size
                     + subscriber_max_borrowed_samples)
  + history_size
  + publisher_max_loaned_samples
  ```

  samples per publisher data segment. Source:
  `iceoryx2-0.9.3/src/service/static_config/publish_subscribe.rs`,
  `required_amount_of_samples_per_data_segment`, and
  `iceoryx2-0.9.3/src/port/publisher.rs`, `Publisher::new`.
- Therefore a normally preallocated pool reserves distinct capacity for every
  supported subscriber's maximum Queue and Borrow retention plus publisher
  Loans. A held Borrow consumes a pool reference, but within configured bounds
  it does not steal the publisher's configured Loan allowance.

### Fanout and queue ownership

- A Publisher maintains one zero-copy connection per Subscriber. Each
  connection owns its own submission queue and completion queue. Source:
  `iceoryx2-0.9.3/src/port/details/sender.rs`, `Connection::new`, and
  `iceoryx2-cal-0.9.3/src/zero_copy_connection/common.rs`, `Channel`.
- `send()` refreshes connections and iterates through all connection slots. A
  successful delivery increments the sample allocation's reference count once
  for that receiver. Source: `Sender::deliver_offset` and
  `Sender::deliver_offset_to_connection_impl`.
- The success value returned by `send()` is the number of Subscriber
  connections that accepted the offset. Zero is a successful send to no
  currently connected subscriber.
- iceoryx2 exposes the service's current aggregate number of subscribers via
  `PortFactory::dynamic_config().number_of_subscribers()`. It does not expose a
  public per-subscriber Queue occupancy or current Borrow count through the
  rbt wrappers.

### Safe overflow

- With safe overflow enabled, pushing into a full per-connection submission
  queue returns the oldest unread offset. The Publisher releases that old
  offset's reference and the new offset remains queued. Source:
  `ZeroCopySender::try_send` and
  `Sender::deliver_offset_to_connection_impl`.
- Overflow is per Subscriber Queue. A slow or stalled Sensor subscriber cannot
  make another subscriber's queue overflow.
- A Borrow has already been removed from the submission queue. Safe overflow
  can replace an unread Queue entry but cannot overwrite an actively borrowed
  sample.
- iceoryx2 does not return the overflow count in the public `send()` result.
  Exact transport Drops are therefore not directly observable through the
  current API. rbt may only label a Drop count as estimated when it is derived
  from subscriber sequence gaps.

### Borrow and release

- `receive()` first enforces the receiver's per-channel maximum Borrow count.
  At the limit it returns `ReceiveError::ExceedsMaxBorrows`; this is not a
  Queue-full or Loan-Pool error. Source:
  `iceoryx2-cal-0.9.3/src/zero_copy_connection/common.rs`,
  `ZeroCopyReceiver::receive`, and
  `iceoryx2-0.9.3/src/port/details/receiver.rs`, `Receiver::receive`.
- A successful receive pops one offset from that connection's Subscriber Queue
  and increments its local Borrow count.
- Dropping `iceoryx2::sample::Sample` returns the offset through the
  connection's completion queue. The Publisher reclaims returned offsets on a
  later loan or send path and releases the allocation reference. Source:
  `iceoryx2-0.9.3/src/sample.rs`, `Drop for Sample`, receiver
  `release_offset`, and sender `retrieve_returned_samples`.
- Dropping an unsent `SampleMut` immediately returns the Loan and decrements
  the publisher's parallel-loan counter. Source:
  `iceoryx2-0.9.3/src/sample_mut.rs`, `Drop for SampleMut`, and sender
  `return_loaned_sample`.

### Loan exhaustion

- A loan attempt fails synchronously with `LoanError::ExceedsMaxLoans` when the
  publisher already holds `max_loaned_samples` unsent Loans. This path does not
  block. Allocation can independently fail with `LoanError::OutOfMemory`.
  Source: `iceoryx2-0.9.3/src/port/details/sender.rs`, `Sender::allocate`.
- rbt explicitly configures two maximum parallel Loans. Rust maps
  `ExceedsMaxLoans` and `OutOfMemory` to `LoanPoolExhausted` while preserving
  the native source error; C++ exposes the matching
  `StreamError::LoanPoolExhausted` classification.

### Control backpressure and partial delivery

- rbt Control uses safe overflow false, Publisher backpressure strategy
  `DiscardData`, and a handler that returns `DiscardDataAndFail` immediately.
- On a full receiver queue, that action discards the offset for the offending
  receiver, returns `SendError::UnableToDeliver` for that receiver, and the
  outer Publisher loop continues delivering to remaining receivers. Source:
  `iceoryx2-0.9.3/src/port/mod.rs`, `BackpressureAction`, and sender
  `deliver_offset`/`deliver_offset_to_connection_impl`.
- After visiting every connection, the outer send returns the stored error if
  any receiver failed. The successful-recipient count is then not available to
  the caller.
- Consequently Control send is not transactional multicast. A QueueFull result
  means at least one connected delivery failed; other subscribers can already
  have accepted the same sample. It does not mean zero deliveries.
- `delivery` here means accepted by a Subscriber's transport queue. It never
  means that the subscriber application or an actuator processed the command.

### Disconnect and crash cleanup

- When a Publisher observes that a Subscriber connection has disappeared, it
  reacquires every offset still tracked by that connection and releases the
  corresponding sample references. Source:
  `iceoryx2-0.9.3/src/port/details/sender.rs`, `remove_connection`.
- A Subscriber retains an expired Publisher connection while that connection
  still has unread data or active Borrows, so the mapped data remains usable
  until those samples are released, subject to the bounded expired-connection
  buffer. Source: `iceoryx2-0.9.3/src/port/details/receiver.rs`,
  `prepare_connection_removal`.
- Port resources include a final-created port tag specifically so another
  process can clean them after a crash. Source:
  `iceoryx2-0.9.3/src/port/publisher.rs`, `Publisher::new`.
- The bounded time until dead-node cleanup becomes observable remains an
  empirical rbt test result, not a source-backed SLA.

## Current rbt policy

The following settings are explicitly applied by both Rust and C++ wrappers:

| Profile | Subscriber Queue | history | safe overflow | max Borrowed | max Loans | publisher strategy |
| --- | ---: | ---: | --- | ---: | ---: | --- |
| Sensor | 1 | 0 | true | 1 | 2 | DiscardData |
| Control | 4 | 0 | false | 1 | 2 | DiscardData + immediate DiscardDataAndFail handler |

`StreamResourceConfig` makes only max_loans and max_borrowed explicit in Rust
and C++; zero values are rejected. Queue depth, history, and safe-overflow are
derived from StreamProfile and are not caller-tunable. The ordinary
constructors use the table above.
`LoanPoolExhausted`, `BorrowLimitReached`, `QueueFull`, and
`UnableToDeliver` are separate public classifications in both SDKs.

Each live wrapper exposes a point-in-time `pressure()` snapshot and contributes
to a weak process-local registry without extending the endpoint lifetime. The
publisher counts current Loans, Loan failures, send attempts/successes/failures,
and aggregate connected subscribers. The subscriber counts receives, current
Borrows, and Borrow failures. Unknown per-subscriber queue occupancy and
transport Drops remain absent rather than fabricated as zero.

## Verified empirical observations

Every case below has a timeout and is exercised by `pressure_semantics`,
`pressure_fault`, `test_mixed_pressure.sh`, or strict `soak_pressure.sh`:

- **A/B:** four independent Sensor connections proved that fast subscribers
  receive every measured sequence while slow readers acquire sequence gaps and
  a never-receiving subscriber does not stop the publisher.
- **C/D:** retained Borrows were exercised with 64 B data, 6.22 MiB Image, and
  bounded 20 MiB PointCloud payloads. The configured Borrow limit is typed,
  retention does not consume the publisher's separate Loan allowance, and the
  resource observations remain bounded rather than being presented as a leak
  proof.
- **E:** two concurrent Loans succeeded; the third returned
  `LoanPoolExhausted(ExceedsMaxLoans)` promptly; dropping one Loan restored
  allocation.
- **F:** two draining Control subscribers received sequence 4 while a third
  full queue caused the same send to return `QueueFull`. Control multicast is
  therefore explicitly non-transactional.
- **G/H:** SIGKILL of a subscriber holding transport state and of a publisher
  with outstanding state both recovered within the tests' bounded watchdogs.
- Rust-publisher/C++-subscriber and C++-publisher/Rust-subscriber fanout passed,
  including a slow or stalled peer.
- Provider replacement and Composer SIGKILL/restart passed while slow and
  stalled subscribers remained connected. The capability/service identity
  rotated on replacement, counters restarted for the new capability, and a
  healthy Composer restart reconciled with `0 deactivate, 0 activate` while
  the new counters continued increasing.

Exact host-specific counts and the policy contract are recorded in
[`runtime-pressure.md`](runtime-pressure.md); they are evidence, not a cleanup
or throughput SLA.

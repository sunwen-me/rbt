# Stream pressure guide

Stream pressure is bounded behavior, not an automatically growing backlog.
The current native implementation uses maintained iceoryx2 `0.10.0-rbt.2`;
see [runtime-pressure.md](runtime-pressure.md) for source-backed details and
historical comparison notes.
Sensor and Control deliberately make different tradeoffs:

| Profile | Queue | History | Safe overflow | max_loans | max_borrowed |
| --- | ---: | ---: | --- | ---: | ---: |
| Sensor | 1 | 0 | true | 2 | 1 |
| Control | 4 | 0 | false | 2 | 1 |

Queue depth, history, and safe-overflow are frozen StreamProfile semantics.
Only max_loans and max_borrowed are explicit resource settings, introduced in
v0.4. They are advertised in StreamDescriptor metadata and do not change a
payload ABI.

A Sensor queue may replace an unread sample for one slow subscriber, so a
stalled Sensor reader sees gaps while fast subscribers continue. Control does
not overwrite an unread command. QueueFull means at least one delivery failed;
other subscribers may already have received the same sample. Control is not
transactional multicast.

A held receive guard is an active Borrow. At max_borrowed, another receive
returns BorrowLimitReached; releasing the guard restores capacity. An unsent
publisher loan consumes one max_loans slot and loan() returns LoanPoolExhausted
synchronously when that ceiling is reached. Neither operation blocks or retries
automatically. QueueFull, UnableToDeliver, ReadinessNotify, LoanPoolExhausted,
and BorrowLimitReached remain distinct outcomes.

Every endpoint exposes a pressure snapshot with exact wrapper-owned current
loans/borrows, high-water values, lifetime totals, payload byte observations,
send/receive failures, borrow durations, and the oldest current borrow age.
Cumulative counters use saturating arithmetic and do not reset automatically.
The metric lifetime is one physical CapabilityInstanceId; provider replacement
starts a new metric owner.

Generic wrappers cannot see exact per-subscriber queue occupancy or every
iceoryx2 Sensor replacement. They report unknown/null rather than zero unless a
message-aware sequence-gap estimate is available. MCAP recorder drops remain
recording-layer metrics.

## Inspection

For an active capability:

~~~bash
rbt stream info <capability-instance-uuid>
rbt stream pressure <capability-instance-uuid>
rbt stream pressure <capability-instance-uuid> --json
~~~

The CLI separates profile semantics from resources, shows snapshot age/status,
and uses conservative reason codes. A missing report is unavailable, not zero.
Within an available endpoint snapshot, a counter value of zero means no
matching events have been counted for that endpoint. Optional `null` values
are field-specific: `oldest_borrow_age_ns` is null when there is no active
borrow, while `estimated_dropped_samples` is null when no sequence-gap
estimate is available. Pressure snapshots are low-rate telemetry (at most
about once per second);
they do not change composition revisions or enter the payload path. Composer
outage does not stop local accounting.

See [pressure-tuning.md](pressure-tuning.md) for the Rust/C++/Python APIs,
metric contract, ownership diagrams, tuning workflow, and memory estimates.
See [pressure-tuning-report.md](pressure-tuning-report.md) for the evidence
register and pending acceptance gates.


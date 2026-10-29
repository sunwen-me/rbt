# Python zero-copy publisher report

## Status and evidence boundary

This report records the implemented and accepted v0.2 Python writable-loan
design. The canonical `./scripts/verify_python_publisher.sh --full` run passed
on 2026-09-12. Its retained wrapper evidence is
`target/python-publisher-verify-logs/python-publisher-full-20260911T234433Z-15383`;
the inner Python evidence is under
`base/python-full-20260911T234433Z-15389`.

The tested environment is CPython 3.12.14, NumPy 2.5.3, Linux x86_64, and
little-endian. The package version remains `0.2.0`. Package metadata permits
CPython 3.10+, but the evidence here does not claim validation of every
permitted interpreter or platform.

The public addition is limited to local Image and PointCloud publishers. The
binding still uses the existing Rust/PyO3 data path and readiness-only
SidecarV1. There is no payload, IDL, QoS, WaitSet, or component-lifecycle
change in this publisher work. It does not add a Python Component runtime,
asyncio integration, callback thread, or network transport.

## Public contract

`ImagePublisher.open(descriptor, *, maximum_payload_bytes=6_220_800)` loans an
Image payload with `loan(width, height, encoding, *, stride=None, sequence=0,
timestamp_ns=0)`. RGB8/BGR8 views have shape `(height, width, 3)` and GRAY8
views have shape `(height, width)`. `payload_view()` exposes all
`stride * height` bytes, including row padding. Empty dimensions are valid;
for example, `loan(0, 4, PixelFormat.GRAY8)` has shape `(4, 0)` and an empty
payload with the default stride.

`PointCloudPublisher.open(descriptor, *, maximum_points=100_000)` loans
`loan(num_points, *, sequence=0, timestamp_ns=0, dense=True)`. Its NumPy view
has shape `(N,)` and the generated canonical `POINT_XYZI_DTYPE`: four
little-endian `float32` fields `x`, `y`, `z`, and `intensity`, at offsets 0,
4, 8, and 12, with itemsize 16. The header uses `width=N`, `height=1`.

The usage rule is intentionally visible in the public documentation:

```python
publisher = rbt.ImagePublisher.open(rbt.StreamDescriptor.image(service))
loan = publisher.loan(1920, 1080, rbt.PixelFormat.RGB8, sequence=17)
image = loan.numpy()                 # writable, non-owning, zero-copy
image[0, 0] = (255, 0, 0)
del image                             # delete every writable alias
loan.send()                           # the LoanedSample is the authority
del loan
```

Derived arrays, slices, reshapes, transposes, and memoryviews all count as
aliases. An explicit `image.copy()` creates independent storage, but it does
not make `send()` safe while the original view remains live.
`np.ascontiguousarray(image)` may return an alias when the loan is already
contiguous, so it must not be assumed to escape the loan. The complete
examples, including raw padded payload access and the PointCloud API, are in
[`python.md`](python.md).

## State machine and ownership invariant

Each loan has one `Arc<Mutex<LoanState>>`. In its active state it owns the
initialized `LoanedSliceSample` by value, plus cached payload address/length,
metadata, and a live lease-root count. Each `numpy()` or `payload_view()`
creates one retained native lease. The returned NumPy object retains that
lease as its buffer base; normal NumPy/memoryview base chains retain it through
derived aliases.

```text
publisher.loan()
      |
      v
   Active -- numpy()/payload_view() --> Mapped (lease count > 0)
      ^                                      |
      |       drop every writable alias   ---+
      |                                      v
      +------------------------------- Active (lease count == 0)
                                             |
                                             | send(): atomically consume
                                             v
                                          Consumed
```

The mutex is the race invariant: mapping creation and the send transition are
linearized under the same lock. If a mapping wins first, `send()` raises
`LoanStillMappedError` and leaves the loan active. If a send wins first, the
state becomes `Consumed` before native delivery, and a concurrent mapping
attempt fails with `LoanConsumedError`. A lease is dropped on any Python
thread, so the final alias can safely unblock a later send from another
thread.

Arrays and memoryviews retain the allocation's memory lifetime, but only the
`LoanedImageSample` or `LoanedPointCloudSample` retains send authority. Keep
the publisher alive until all loans that may be sent have had their send
attempt. The public contract does not promise sending after the publisher is
deleted. There is no destructor auto-send and no `discard()` method. Dropping
an unsent loan after all of its aliases are gone safely returns the allocation
to the pool and does not deliver a sample. Call `gc.collect()` after deleting
the last alias and loan when a test or bounded producer needs deterministic
reclamation; collection never auto-sends.

`send()` is one-shot. After the mapping check succeeds, success and every
native send result leave the loan consumed, including `QueueFullError`,
`UnableToDeliverError`, and `ReadinessNotifyError`. Retrying or remapping then
raises `LoanConsumedError`. A `ReadinessNotifyError` is deliberately not
treated as a delivery rollback: the payload may already have been received
when the following readiness notification fails.

## Why the lease is separate from `Py_buffer`

The first private proof of concept counted active CPython `Py_buffer` exports.
That model was empirically insufficient on the tested CPython 3.12.14 / NumPy
2.5.3 combination: `np.ndarray(buffer=exporter)` called `__getbuffer__` and
then `__releasebuffer__` while the writable ndarray was still alive. The
export count therefore reached zero prematurely, allowing a simulated send
while the array could still write.

The production mapping instead creates a dedicated retained lease before
constructing NumPy. The lease itself is the ndarray/memoryview base and its
Rust `Drop` decrements the count. `__getbuffer__` only fills a transient
`Py_buffer` with the cached address and transfers a strong lease reference to
`view.obj`; transient buffer callbacks do not define the mapping lifetime.
This is why derived aliases and memoryviews continue to block `send()` until
the final root is gone.

## Initialization and safety

The publisher uses `SlicePublisher::loan_slice`, which initializes every
element with its `Default` value before Python receives a pointer. Image bytes
and PointXYZI elements are therefore initialized; no `MaybeUninit` payload is
exposed and dropping an unsent loan cannot leak partially initialized data.
After metadata and the payload pointer are cached, mapped operations do not
dereference the Rust loan. The active sample remains the sole native owner
until the mutex changes the state to `Consumed` or the unsent state is dropped.

## Zero-copy and interoperability evidence

The focused publisher tests compare the NumPy data pointer with the native
loan pointer in the same Python process and assert `OWNDATA=False` and
`WRITEABLE=True` for loaned arrays. Received arrays retain their existing
contract: `OWNDATA=False` and `WRITEABLE=False`.

| Sample | Shape / payload | Pointer | `OWNDATA` | `WRITEABLE` |
| --- | --- | --- | --- | --- |
| Loaned Image, RGB8 1920x1080 | `(1080, 1920, 3)` / 6,220,800 B | equal | false | true |
| Loaned padded Image, RGB8 640x480 stride 2048 | `(480, 640, 3)` / 983,040 B | equal | false | true |
| Loaned PointCloud, N=0, 1, 100, 100000 | `(N,)` / `N * 16` B | equal | false | true |
| Received Image or PointCloud | generated view | equal | false | false |

Five focused interop tests use real native consumers: Python Image to Rust,
Python Image to C++, padded Python Image to Rust, and Python PointCloud to
both Rust and C++. The Image path sends ten 1920x1080 frames and validates
canonical byte patterns, padding, sequence values, and checksums. The
PointCloud path validates N=0, 1, 100, and 100,000, the generated dtype, first
and last values, alignment, and checksums. No cross-process virtual-address
equality is assumed; pointer equality is a local NumPy/native-loan proof and
the native consumers provide the independent data-plane proof.

## Readiness failure and pressure behavior

The readiness failure test uses a real deadline-configured sidecar fixture.
The publisher's payload is received by a subscriber, then the sidecar
notifier reports a missed deadline. Python observes `ReadinessNotifyError`,
and the loan is already `Consumed`; a second `send()` or `numpy()` raises
`LoanConsumedError`. This is a real post-payload readiness failure, not a
synthetic Python error injected after a successful send.

Sensor publishers retain the existing queue=1, history=0, safe-overflow
policy. The publisher loan pool has a maximum of two outstanding loans, and a
third loan raises `LoanPoolExhaustedError`. The full pressure gate exercises a
Python publisher at about 30 Hz with a Rust fast consumer, a C++ slow
consumer, and a C++ stalled consumer. It checks that the fast path continues,
slow consumers observe bounded drops, and the stalled peer does not block the
publisher indefinitely.

## Focused checks and resource coverage

The currently known focused evidence is:

- 28 focused core/receive tests covering the existing read-only and publisher
  integration surface;
- 5 native interop tests described above;
- 14 ownership tests covering pointer/flags, derived aliases, copy escape,
  pool recovery, unsent drop, and cross-thread final release;
- the C++ fixture's 6/6 CTest suite;
- a timing sanity run separating loan, mapping, user fill, and send. The
  accepted full run measured mapping medians of approximately 6.9 microseconds
  (64 B), 23.2 microseconds (6.22 MiB), and 23.7 microseconds (20 MiB); these
  are run-dependent observations, not a throughput guarantee.

The full-marked ownership checks also cover a 2,000-iteration mapping/send
race, 5,000 send/drop cycles with periodic garbage collection, interpreter
shutdown with outstanding unsent views, and prompt SIGTERM with an outstanding
view. The verification harness records RSS, file-descriptor, and thread
counts; these checks are resource-growth guards, not a proof of leak absence.

## Unsafe audit and packaging contract

The production unsafe-location audit is explicit:

```text
old total: 43 = 31 non-Python + 12 Python read-only buffer sites
new total: 49 = 31 non-Python + 18 Python sites
delta:     +6
```

The six new sites are only the writable Py_buffer boundary: the writable
buffer-fill helper and its field-write block, plus the Image and PointCloud
lease `__getbuffer__` callbacks and their calls into that helper. They validate
the CPython view, install the lease owner in `view.obj`, expose the cached
initialized pointer, and mark the view writable. There is no unsafe lifetime
erasure, manual sample free, ownerless pointer, or copy-on-send path.

The private `_debug_payload_address()` diagnostic returns an integer solely so
tests can compare the NumPy data pointer with the native loan address. It does
not construct or authorize a buffer; every array and memoryview created by the
supported API still retains a lease. As with NumPy's own pointer introspection,
deliberately rebuilding a pointer through `ctypes` is outside the supported
ownership contract.

The fresh-wheel gate builds and installs a wheel into a virtual environment
outside the checkout. It checks version `0.2.0`, no `.pyc`/`__pycache__`, no
repository path strings, no `RPATH`/`RUNPATH`, and no unresolved repository
native dependencies with `LD_LIBRARY_PATH` and `LIBRARY_PATH` unset. It then
connects the installed Python publisher to a real native Rust Image consumer.

## Verification commands and remaining scope

The implemented entry points are:

```bash
./scripts/verify_python_publisher.sh --quick
./scripts/verify_python_publisher.sh --full
target/python-verify-venv/bin/python scripts/benchmark_python_publisher.py
```

The quick/full wrapper includes Rust formatting/checks/tests/Clippy, a Release
C++ fixture check, deterministic IDL checks, source audits, Python ownership
and interop tests, and (in full mode) pressure, ASan, shutdown, fresh-wheel,
and existing regression gates. The nested full WaitSet/v0.1 regression adds
workspace-wide Rust tests and Debug/Release C++ and sanitizer builds. The
accepted run completed with 56 Python tests and 36 ASan ownership tests
passing, 6/6 CTest cases passing in each required build, deterministic
generated output, no forbidden production payload-copy primitive, and the
documented 49-location unsafe audit. Nested evidence is retained at
`target/waitset-verify-logs/waitset-full-20260911T234626Z-18441` and
`target/v01-logs/verify-full-20260911T234704Z-21777`.

The Component runtime was outside this publisher milestone and is delivered by
the subsequent synchronous adapter documented in
[`python-component.md`](python-component.md). The asyncio adapter, MCAP, ROS 2
bridge, and network gateway remain deferred.

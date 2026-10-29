# Python Data SDK

The rbt Python package is version `0.8.0`. It provides a CPython/NumPy data
SDK for local Image and PointCloud
streams. Subscribers construct NumPy arrays directly over received iceoryx2
shared-memory payloads. Publishers loan initialized shared-memory payloads and
expose writable NumPy views without a copy. Every writable alias must be gone
before a publisher loan is sent. The current package also exposes synchronous
and asyncio Component APIs, typed local RPC, `rbt.time`, `rbt.time_stream`, and
the `rbt.tf` transform APIs. See the [developer docs index](README.md) and the
guides for [components](python-component.md),
[asyncio](python-asyncio.md), [RPC](rpc.md), [time](time.md), and [TF](tf.md).

## Install

Build a wheel with CPython 3.10 or newer and install it into a virtual
environment:

```bash
python -m build --wheel python
python -m venv /tmp/rbt-python
/tmp/rbt-python/bin/pip install python/dist/rbt-0.8.0-*.whl
/tmp/rbt-python/bin/python -c 'import rbt; print(rbt.__version__)'
```

The wheel contains the Rust/PyO3 extension and its Rust iceoryx2 data path; it
does not need `LD_LIBRARY_PATH` pointing at this repository. NumPy is a normal
Python dependency and is intentionally not bundled into the wheel.

The tested platform is CPython 3.12.14, NumPy 2.5.3, Linux x86_64,
little-endian. The package metadata permits CPython 3.10+, but this milestone
does not claim validation of every permitted minor version. PyPy, GraalPy,
free-threaded CPython, subinterpreters, big-endian systems, and non-Linux
systems are not supported by this release.

## Publish an Image

`ImagePublisher.open()` requires an Image `StreamDescriptor` advertising
`sidecar-v1`. The default maximum payload is 6,220,800 bytes (1920x1080
RGB8). `loan()` checks dimensions, encoding, stride, and arithmetic before
asking iceoryx2 for an initialized loan.

```python
import rbt

descriptor = rbt.StreamDescriptor.image("rbt/camera/image")
publisher = rbt.ImagePublisher.open(descriptor)

# Keep both publisher and loan alive until the send attempt completes.
loan = publisher.loan(
    1920,
    1080,
    rbt.PixelFormat.RGB8,
    sequence=17,
    timestamp_ns=123_000_000,
)
image = loan.numpy()                 # writable, non-owning, zero-copy
image[0, 0] = (255, 0, 0)

# Delete every writable alias before send. `loan` is the send authority.
del image
loan.send()
del loan
```

RGB8 and BGR8 map to `uint8` shape `(height, width, 3)` and strides
`(stride, 3, 1)`. GRAY8 maps to shape `(height, width)` and strides
`(stride, 1)`. A padded row stays padded; the NumPy view uses the supplied
stride and does not make the image contiguous by copying it.

The raw payload view includes padding and is useful when a producer needs to
initialize or inspect every byte. It is writable and counts as a live alias
until all arrays and memoryviews derived from it are released:

```python
import numpy as np

loan = publisher.loan(640, 480, rbt.PixelFormat.RGB8, stride=2048)
image = loan.numpy()  # shape=(480, 640, 3), strides=(2048, 3, 1)
raw_view = loan.payload_view()
raw = np.frombuffer(raw_view, dtype=np.uint8)
raw[:] = 0  # includes row padding; this is user code, not copy-on-send

# `raw.base` retains raw_view, so both names are released explicitly.
del raw, raw_view, image
loan.send()
del loan
```

An image with no visible pixels is valid. With the default stride, this creates
an empty payload and an empty writable view:

```python
loan = publisher.loan(0, 4, rbt.PixelFormat.GRAY8)
empty = loan.numpy()
assert empty.shape == (4, 0)
del empty
loan.send()
del loan
```

## Writable aliases and `send()`

`numpy()` and `payload_view()` each create one retained native lease. NumPy
derived arrays, slices, reshapes, transposes, and memoryviews retain that
lease through their ordinary base chain. Therefore a send attempt with any
live writable alias raises `LoanStillMappedError` and leaves the loan active:

```python
loan = publisher.loan(4, 3, rbt.PixelFormat.RGB8)
root = loan.numpy()
crop = root[:, :2, :]
transposed = crop.transpose(1, 0, 2)

try:
    loan.send()
except rbt.LoanStillMappedError:
    pass

# Drop the original and every derived alias, not just `root`.
del root, crop, transposed
loan.send()
del loan
```

An explicit copy is an escape hatch for retaining data after the loan is
sent. The copy is independent; it is not a live writable alias of the loan:

```python
loan = publisher.loan(4, 3, rbt.PixelFormat.RGB8)
image = loan.numpy()
owned = image.copy()       # explicit user-requested copy
del image                  # release the shared-memory alias
loan.send()
assert owned.flags.owndata
del owned, loan
```

Only `LoanedImageSample` has send authority. A NumPy array or memoryview can
keep the initialized shared-memory allocation alive after the sample wrapper
is dropped, but it cannot send it. Keep the `ImagePublisher` alive while any
loan may be sent; the public contract does not promise sending after the
publisher has been deleted. There is no implicit send, destructor auto-send,
or `discard()` method. Dropping an unsent loan after its aliases are gone
returns the allocation safely to the loan pool and does not deliver a sample.
When deterministic cleanup matters, drop the last alias and loan and call
`gc.collect()`; garbage collection never sends the loan implicitly.

`LoanedImageSample` and `LoanedPointCloudSample` handles are intentionally not
copyable or pickleable. The default Sensor loan capacity is two outstanding
loans; exhaustion raises `LoanPoolExhaustedError`.

`send()` is a one-shot consuming operation. Once the mapping check succeeds,
the loan changes to consumed before the native send call. A successful send
and every native send error—including `QueueFullError`,
`UnableToDeliverError`, and `ReadinessNotifyError`—leave it consumed. Do not
retry or remap after any such error; subsequent operations raise
`LoanConsumedError`. A `ReadinessNotifyError` means the payload may already
have reached a subscriber even though the following readiness notification
failed.

## Publish a PointCloud

PointCloud loans use the generated canonical `rbt.POINT_XYZI_DTYPE`: four
little-endian `float32` fields named `x`, `y`, `z`, and `intensity`, at offsets
0, 4, 8, and 12, with itemsize 16. The view shape is `(N,)`; `width=N`,
`height=1`, and `dense` is supplied in the header. The default maximum is
100,000 points.

```python
import rbt

descriptor = rbt.StreamDescriptor.point_cloud("rbt/lidar/points")
publisher = rbt.PointCloudPublisher.open(descriptor, maximum_points=100_000)
loan = publisher.loan(100_000, sequence=9, dense=True)
points = loan.numpy()
assert points.dtype == rbt.POINT_XYZI_DTYPE
points["x"] = 1.0
points["y"] = 2.0
points["z"] = 3.0
points["intensity"] = 0.5
del points
loan.send()
del loan
```

`N=0`, `N=1`, `N=100`, and `N=100_000` are covered by the interop tests.
`payload_nbytes` is exactly `N * 16`; the binding checks alignment before
constructing the structured view. The same explicit alias and one-shot send
rules apply to PointCloud loans.

## Receive an Image

The stream descriptor can come from JSON/CLI inspection or be constructed
explicitly when the service name is known:

```python
import rbt

descriptor = rbt.StreamDescriptor.image("rbt/camera/image")
sub = rbt.ImageSubscriber.open(descriptor)

sample = sub.receive()
assert sample is not None
image = sample.numpy()

print(sample.sequence, image.shape, image.dtype)
```

RGB8 and BGR8 map to `uint8` shape `(height, width, 3)` and strides
`(header.stride, 3, 1)`. GRAY8 maps to shape `(height, width)` and strides
`(header.stride, 1)`. Padded rows remain padded; rbt does not make the array
contiguous by copying it. Corrupt or overflowing dimensions, strides, and
payload bounds are rejected before NumPy sees the pointer.

`try_receive()` is nonblocking. `receive(timeout=seconds)` returns `None` on a
deadline. `receive()` without a timeout blocks until data or a Python signal.

## Receive a PointCloud

```python
import rbt

sub = rbt.PointCloudSubscriber.open(
    rbt.StreamDescriptor.point_cloud("rbt/lidar/points")
)
sample = sub.receive()
assert sample is not None
points = sample.numpy()

centroid = (
    points["x"].mean(),
    points["y"].mean(),
    points["z"].mean(),
)
```

The returned shape is `(N,)`. Its structured dtype is generated from the
canonical `.rbt` IDL metadata: four little-endian `float32` fields named `x`,
`y`, `z`, and `intensity`, at offsets 0, 4, 8, and 12 with itemsize 16. The
binding checks element count, byte length, and payload alignment before array
construction. This is a local shared-memory ABI, not a network format.

## Ownership and read-only receive contract

The core rule is:

> A NumPy view owns the received shared-memory sample for as long as the view
> or any derived view can still access that memory.

The ownership graph is one-way:

```text
ndarray / derived ndarray
        -> native Python buffer owner
        -> Rust ReceivedSliceSample
        -> iceoryx2 received-sample guard
        -> shared-memory borrow
```

For example:

```python
sample = sub.receive()
image = sample.numpy()

del sample

# Still valid: ndarray retains the sample owner.
process(image)

del image
# The underlying SHM borrow can now be released.
```

Derived views preserve NumPy's base chain, so a crop remains valid after both
the original array and sample wrapper are gone. Repeated `numpy()` calls create
independent lightweight views; each retains the same native owner. There is no
`release()` or `close()` method that could invalidate an exported array and no
sample-to-array reference cycle.

Keeping NumPy arrays alive keeps shared-memory samples borrowed. The frozen
Sensor profile permits one concurrent borrow. Retaining an array while trying
to receive another queued sample therefore raises
`BorrowLimitReachedError`. Delete all arrays/derived views (and use
`gc.collect()` when diagnosing ownership) to permit the borrow to return.

For long-term storage, make the copy explicitly and then release the view:

```python
owned = image.copy()
del image
```

Both Image and PointCloud arrays are backed by a read-only buffer exporter.
`array.flags.writeable` is false, assignment fails, and ordinary NumPy APIs
cannot set the flag back to true. The threat model does not attempt to stop
deliberate mutation through `ctypes`, a hostile native extension, or raw
address manipulation.

## Waiting, signals, and pressure

Blocking receive attaches the stream's readiness-only SidecarV1 listener to
the existing rbt `WaitContext`. Readiness never carries payload. Synthetic
initial drain, stable token handling, spurious readiness, and the global
bounded drain limit of 64 remain native WaitSet semantics.

PyO3 releases the GIL for every native wait, so other Python threads continue
to execute. An unbounded Python call uses at most 0.5-second native WaitSet
deadline quanta solely to return to CPython for `PyErr_CheckSignals`; it never
loops on `try_receive()` or sleeps between payload polls. This bounds observed
`KeyboardInterrupt` latency while leaving payload receive event-driven.

`subscriber.pressure` exposes received, currently borrowed, high-water,
borrow-limit, completed-duration, and oldest-borrow-age counters from the rbt
subscriber wrapper. `publisher.pressure` additionally exposes exact loan
ownership/high-water, payload-byte, send, queue, delivery, and readiness
counters. Slow or stalled Python Sensor subscribers use the same queue=1,
history=0, safe-overflow semantics as Rust and C++; no Python-only queue or
counter exists. Resource tuning is limited to `max_loans` and
`max_borrowed`; see [pressure tuning](pressure-tuning.md).

## Descriptors and errors

`StreamDescriptor.from_json()` treats a missing pre-v0.2 `transport` field as
`readiness=none`. New WaitSet consumers require SidecarV1 and raise
`MissingTransportFeatureError` before creating any listener, payload service,
node, or endpoint. Message identity, ABI major, shape, sizes/alignments, and
layout hash are likewise checked before transport open; a mismatch raises
`AbiMismatchError`.

Other typed errors include `BorrowLimitReachedError`, `WaitError`,
`InvalidPayloadError`, `InvalidImageLayoutError`,
`InvalidPayloadAlignmentError`, and `UnsupportedPixelFormatError`. Timeout/no
sample is the only receive outcome represented by `None`.

## Scope

The v0.8.0 Python package exposes descriptors, message metadata, Image and
PointCloud publishers and subscribers, writable loan leases, received sample
owners, pressure inspection, typed errors, event-driven receive, typed local
RPC, clocks and timestamp mappings in `rbt.time`, stamped byte streams in
`rbt.time_stream`, and transform math, buffers, and transport bindings in
`rbt.tf`. `Component`, `ActivationContext`, `TaskContext`, and
`run_component` adapt the existing Rust runtime; Python does not implement a
separate resolver, registry, snapshot reconciler, scheduler, or lifecycle
state machine. The public declarations are in `python/rbt/__init__.pyi` and
`python/rbt/tf.pyi`. See [Python Component runtime](python-component.md).

The package also has an asyncio Component adapter and `receive_async`; see
[Python asyncio runtime](python-asyncio.md). RPC exposes synchronous and
asyncio clients/servers; see [typed local RPC](rpc.md). Time and TF APIs are
described in [real-hardware time](time.md) and [transform frames and lookups](tf.md).
Physical-camera hardware acceptance remains NOT RUN, and generic TF MCAP
recording/replay remains unverified; see the [TF acceptance report](tf-report.md).
The synchronous buffer ownership,
ABI, pressure, and no-implicit-copy rules on this page are unchanged in async
code. There is no payload callback model.

The promise is narrowly: rbt local IPC payloads are exposed to NumPy without an
intermediate binding copy, and a publisher's loan is the memory owner until it
is sent or dropped. It is not a claim of camera-DMA, GPU, network, or
end-to-end hardware zero-copy. `image.copy()` explicitly creates independent
storage. `np.ascontiguousarray(image)` may return an alias when the loan is
already contiguous and therefore must not be assumed to copy.

## Verification

```bash
./scripts/verify_python.sh --quick
./scripts/verify_python.sh --full
```

Quick mode builds Rust/C++ fixtures and the extension, verifies deterministic
Python IDL metadata, audits the receive and publisher sources for forbidden
copy primitives, and runs Image, PointCloud, lifetime, read-only, writable
lease, descriptor, GIL, and preflight tests. Full mode adds native
fast/slow/stalled pressure peers, GC/resources, cross-thread races, signal and
interpreter shutdown, a fresh wheel/venv publisher connection, an isolated
ASan-instrumented ownership run, and the existing full v0.1/pressure/WaitSet
regression suite. The dedicated wrapper commands are:

```bash
./scripts/verify_python_publisher.sh --quick
./scripts/verify_python_publisher.sh --full

./scripts/verify_python_component.sh --quick
./scripts/verify_python_component.sh --full
```

The wrapper also runs `scripts/benchmark_python_publisher.py`, which times loan,
mapping, user fill, and send separately for 64-byte, 6,220,800-byte, and
20-MiB payloads and checks that mapping does not scale like a payload copy.

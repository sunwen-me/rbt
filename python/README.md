# rbt Python Data SDK

The rbt Python package is version `0.8.0`. It provides zero-copy NumPy access
to local iceoryx2 shared-memory Image and PointCloud streams. Received views
are read-only;
publisher loans are writable and must be explicitly sent.

The current SDK also provides synchronous and asyncio Component APIs, typed
local RPC, clocks and timestamps in `rbt.time`, stamped byte streams in
`rbt.time_stream`, and transform APIs in `rbt.tf`.

See the [developer docs index](../docs/README.md), [Python SDK guide](../docs/python.md),
[Component runtime](../docs/python-component.md),
[asyncio runtime](../docs/python-asyncio.md), [RPC](../docs/rpc.md),
[time](../docs/time.md), [TF](../docs/tf.md), and
[pressure tuning](../docs/pressure-tuning.md).

Stream resource ceilings are advertised separately from the payload ABI. The
defaults allow two outstanding publisher loans and one borrowed subscriber
sample; applications can override max_loans and max_borrowed when opening a
publisher (public subscribers select a local max_borrowed):

```python
descriptor = rbt.StreamDescriptor.image("rbt/camera/image")
publisher = rbt.ImagePublisher.open(
    descriptor, max_loans=4, max_borrowed=2
)
subscriber = rbt.ImageSubscriber.open(descriptor, max_borrowed=2)
assert publisher.resources.max_loans == 4
```

```python
import rbt

subscriber = rbt.ImageSubscriber.open(
    rbt.StreamDescriptor.image("rbt/camera/image")
)
sample = subscriber.receive()
image = sample.numpy()
```

```python
publisher = rbt.ImagePublisher.open(
    rbt.StreamDescriptor.image("rbt/camera/image")
)
loan = publisher.loan(1920, 1080, rbt.PixelFormat.RGB8)
image = loan.numpy()
image[...] = 0
del image
loan.send()
```

`sample.numpy()` does not copy the payload. The read-only NumPy array retains
the native received-sample owner, so it and any derived view remain valid after
the `sample` wrapper is deleted. Keeping arrays alive also keeps native shared
memory borrowed and can raise `BorrowLimitReachedError`; copy explicitly with
`image.copy()` when long-term owned storage is desired.

`publisher.pressure()` and `subscriber.pressure()` return immutable typed
snapshots. They report resource settings, exact ownership/high-water counters,
loan/receive attempts, payload and delivery failures, monotonic borrow
durations, and the age of the oldest outstanding borrow without changing
ownership. Derived NumPy views remain accounted as borrowed until the final
Python alias is released; see [pressure tuning](../docs/pressure-tuning.md).

## Components

Subclass `rbt.Component`, implement synchronous `activate(ctx)`, and call
`rbt.run_component(component)`. The native runtime owns lifecycle, cancellation,
and managed worker threads. `run_component` is synchronous; the same package
also provides a separately documented asyncio Component adapter and
`receive_async` without changing buffer ownership.

```python
class Detector(rbt.Component):
    component_id = "detector"
    requirements = {"camera": "Camera"}

    def prepare(self, config):
        self.model = load_model(config)

    def activate(self, ctx):
        camera = ctx.require("camera")
        output = ctx.provide_point_cloud("Detections", maximum_points=4)
        ctx.spawn_critical(
            "inference", lambda task: run_inference(task, camera, output)
        )


rbt.run_component(Detector(), config={"model": "small"})
```

Native rbt waits release the GIL and wake on activation cancellation. Arbitrary
Python and third-party native code is cooperatively cancelled and is never
forcibly interrupted.

The tested platform is CPython 3.12 on little-endian Linux x86_64. The package
is typed via `py.typed`, `rbt/__init__.pyi`, and `rbt/tf.pyi`. Physical-camera
hardware acceptance remains NOT RUN, and generic TF MCAP recording/replay
remains unverified. See the linked guides for the complete ownership, waiting,
lifecycle, and compatibility rules.

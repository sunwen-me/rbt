# rbt v0.2 Python Data SDK report

## Result and scope

The v0.2 milestone is complete. The production package is a receive-only
CPython/NumPy data SDK for local Image and PointCloud streams. Its
`sample.numpy()` path creates a read-only view over the currently borrowed
iceoryx2 shared-memory payload without an intermediate payload copy, and the
NumPy ownership chain keeps that borrow alive exactly as long as it can still
be accessed.

This milestone did not add a Python publisher, Component, Capability,
Requirement, EffectScope, callback thread, asyncio integration, scheduler, or
remote transport.

The verified environment was CPython 3.12.14, NumPy 2.5.3, Linux x86_64,
little-endian. Package metadata permits CPython 3.10+, but this report does not
claim that every permitted Python or NumPy version was tested. PyPy, GraalPy,
free-threaded CPython, subinterpreters, big-endian hosts, and non-Linux hosts
are outside the supported v0.2 matrix.

## Binding architecture and decision

Chosen: PyO3 over the Rust data SDK.

```text
Python rbt package
        |
        v
PyO3 extension rbt._rbt
        |
        v
rbt-iox2 Rust data SDK
        |
        v
iceoryx2 ipc_threadsafe service
```

The Rust `ReceivedSliceSample<M>` owns the upstream iceoryx2 received sample
by value. It has no lifetime tied to a borrowed Subscriber, so the Python
owner needs no lifetime transmute, fake `'static`, self-reference, or leak.
The sample is `Send` but intentionally not `Sync`; the PyO3 owner stores it in
a `Mutex`, providing the synchronization required by a cross-thread Python
object without changing the underlying ownership rule. The cross-thread
lifetime test passed: a NumPy array was handed from one Python thread to
another, consumed there, dropped there, and the next receive recovered.

Rejected alternative: pybind11 over the C++ SDK. The C++ sample is move-only
RAII, but iceoryx2's installed C++ `sample.hpp` explicitly states that Sample
is not `Send` and must not be moved to another thread. A Python object's final
reference may be dropped by any Python thread. Making that path safe would
therefore require an owner-thread deferred-release mechanism, which would be
less direct and less maintainable than the Rust owned-sample path. No second
production binding or feasibility-spike code remains.

The decision priority was memory safety, exact ownership, zero-copy behavior,
binding thinness, then maintainability; it was not based on language
preference.

## Ownership model

The invariant, present in the implementation and user documentation, is:

> A NumPy view owns the received shared-memory sample for as long as the view
> or any derived view can still access that memory.

The implemented one-way ownership graph is:

```text
ndarray / derived ndarray / memoryview
        |
        | owns through the CPython buffer base chain
        v
native PyO3 received-sample owner
        |
        | owns by value
        v
Rust ReceivedSliceSample<M>
        |
        | owns
        v
iceoryx2 received-sample guard
        |
        v
shared-memory borrow
```

The native owner exports a read-only CPython buffer. `Py_buffer.obj` holds a
strong reference to that exact owner; NumPy constructs the ndarray with the
owner as its buffer. Derived arrays retain NumPy's ordinary base chain. The
sample does not cache or own the ndarray, so there is no owner cycle. Repeated
`numpy()` calls create independent lightweight views that retain the same
native owner. There is deliberately no `release()` or `close()` API capable of
invalidating an outstanding view.

## Readiness compatibility and versioning

Rust, C++, Composer DTOs, the CLI, and Python now use the same transport
metadata:

```text
TransportFeatures.readiness = none | sidecar-v1
```

New stream constructors advertise `sidecar-v1`. Every fixed and slice
publisher/subscriber performs common preflight before creating a node,
listener, payload service, or endpoint. ABI identity is checked first, then a
WaitSet consumer requires SidecarV1. A missing or incompatible feature raises
the typed `MissingTransportFeature` / `MissingTransportFeatureError` instead
of opening a transport that could sleep forever. `rbt stream info` exposes the
advertised readiness value.

Backward compatibility is intentionally explicit: a legacy descriptor with
no `transport` field parses as `readiness=none`, and a new WaitSet consumer
rejects it before transport open. Current descriptors round-trip the additive
field through Rust/C++ control DTOs and Composer snapshots. Healthy Composer
restart still reconciles with `0 deactivate, 0 activate`.

Version decision:

| Axis | v0.2 value | Change in this milestone |
| --- | ---: | --- |
| SDK | `0.2.0` | Advanced to v0.2 |
| IDL language | `1` | None |
| Message descriptor format | `1` | None |
| Control protocol | `2` | None |
| Composer snapshot | `2` | None |

The descriptor's JSON/control representation gained an additive transport
field, but the already-current control and snapshot versions were retained:
their decoders already support additive metadata and the regression matrix
verified the round-trip. The transport feature is not payload ABI and is not
an input to `MessageTypeDescriptor.layout_hash`. Frozen hashes remained:

| Message | Layout hash |
| --- | --- |
| Image | `943fa4008cf0f55f89a281b051c69a6f` |
| PointCloud | `d9da2bb29bb2cf217c40f1a56d1fb755` |
| ControlCommand | `9381cf15842a48ac0964111f0f45ef72` |

`ReadinessNotify` retains its prior meaning: payload send may already have
succeeded when the following readiness notification fails. This milestone
hardens deployment compatibility; it does not make readiness carry payload or
redesign sidecar delivery.

## Zero-copy evidence

Pointer equality below is within the Python subscriber process: the native
received-payload pointer equals the NumPy data pointer. Cross-process virtual
addresses are neither compared nor required to match.

| Producer / sample | Payload bytes | Native payload pointer | NumPy pointer | `OWNDATA` | `WRITEABLE` |
| --- | ---: | --- | --- | --- | --- |
| Rust Image 1920x1080 RGB8 | 6,220,800 | `0x76e445c360d8` | `0x76e445c360d8` | false | false |
| C++ PointCloud N=0 | 0 | `0x76e44a3870d8` | `0x76e44a3870d8` | false | false |
| C++ PointCloud N=1 | 16 | `0x76e44a3870d8` | `0x76e44a3870d8` | false | false |
| C++ PointCloud N=100 | 1,600 | `0x76e44a3870d8` | `0x76e44a3870d8` | false | false |
| C++ PointCloud N=100000 | 1,600,000 | `0x76e44a3870d8` | `0x76e44a3870d8` | false | false |

The addresses are evidence from the definitive full run and are naturally
run-specific. The source audit found zero forbidden payload-copy constructs
in the binding path. There is no `bytes`, `bytearray`, `Vec`, `std::vector`,
serializer, `memcpy`, `tobytes`, `np.array`, or `np.copy` payload stage.
Header-to-Python scalar conversion is intentionally outside the payload-copy
claim.

Both native owners refuse writable buffer requests. Image and PointCloud
assignment fail, and `array.flags.writeable = True` raises rather than
re-enabling writes. Deliberate mutation through `ctypes`, a hostile native
extension, or raw address manipulation is outside the threat model.

## Image interoperability

The independent Rust publisher sent ten 1920x1080 RGB8 frames. All ten were
received and checked.

| Property | Result |
| --- | --- |
| Shape | `(1080, 1920, 3)` |
| dtype | `uint8` |
| Strides | `(5760, 3, 1)` |
| Payload | 6,220,800 bytes |
| Checksum per frame | 793,152,000 |
| Frame count | 10 |
| Selected pixels / sequence | Passed for every frame |
| Pointer identity | Passed |
| `OWNDATA` / `WRITEABLE` | false / false |

RGB8 and BGR8 use `(height, width, 3)` and GRAY8 uses `(height, width)`.
Validation covers all three formats plus zero dimensions, unknown encodings,
short payloads, invalid strides, and checked-arithmetic overflow before NumPy
receives the pointer.

The padded case used 640x480 RGB8 with row stride 2048:

| Property | Result |
| --- | --- |
| Shape | `(480, 640, 3)` |
| Strides | `(2048, 3, 1)` |
| Visible-pixel checksum | 113,571,840 |
| Pointer identity | Passed |
| C contiguous | false, as required |

Padding is represented by strides and is never removed with a copy.

## PointCloud interoperability

The independent C++ publisher sent four PointCloud samples. The dtype came
from deterministic `.rbt` IDL-generated Python metadata, not a second
handwritten ABI definition:

```text
names     = x, y, z, intensity
formats   = <f4, <f4, <f4, <f4
offsets   = 0, 4, 8, 12
itemsize  = 16
shape     = (N,)
alignment = 4
```

| N | Payload bytes | Checksum | First/last values | Pointer / read-only |
| ---: | ---: | ---: | --- | --- |
| 0 | 0 | 0 | Empty case passed | Passed |
| 1 | 16 | 66,571,993,088 | Passed | Passed |
| 100 | 1,600 | 3,386,543,084,142,592 | Passed | Passed |
| 100,000 | 1,600,000 | 3,677,129,335,508,477,952 | Passed | Passed |

The binding validates `width * height == N`, exact byte length, checked
arithmetic, and pointer alignment before array construction. The structured
dtype is little-endian because this is the frozen local SHM ABI, not a network
serialization format.

## Lifetime and borrow-pressure evidence

The following real ownership transitions passed:

1. Delete the Python sample and collect garbage while the ndarray remains:
   its checksum and selected values remain valid.
2. Keep only a derived crop: it remains valid and the next native receive
   returns `BorrowLimitReached` under Sensor `max_borrowed=1`.
3. Delete the last crop and collect: receive succeeds again.
4. Create two arrays from one sample, then delete the sample and one array:
   the remaining array stays valid and retains the borrow. Deleting the last
   array restores receive.
5. A raw read-only `memoryview` likewise retains the borrow until it is
   dropped.
6. Drop the final ndarray reference on a different Python thread: no crash or
   leak occurs and receive recovers.
7. Exit an interpreter while a sample, ndarray, and derived view remain alive:
   no crash, double free, or hang occurs.

The observed concise result was
`sample_deleted=true derived_view_retained=true borrow_limit_reached=true
recovery=true`. This is native SHM borrow pressure, not a Python-only flag or
counter. Five hundred sequential ownership/GC cycles had resource snapshots
`(RSS KiB, fd, threads) = (56060, 6, 18)` before and `(56064, 6, 18)` after,
with zero borrow failures and no obvious unbounded growth.

## WaitSet, GIL, and signals

Blocking receive follows the existing readiness-only path:

```text
payload SHM send
        |
        v
SidecarV1 readiness notification (no payload)
        |
        v
native rbt WaitContext / iceoryx2 WaitSet
        |
        v
subscriber.receive()
        |
        v
owned sample guard -> Python owner -> NumPy view
```

Synthetic initial drain, stable tokens, spurious readiness handling,
readiness-only events, cancellation wake-before-join, and the global bounded
drain of 64 remain the native implementation. The Python wrapper does not
poll `try_receive()` for payload and has no sleep-based receive loop.

PyO3 releases the GIL during every native WaitSet call. A bounded native
deadline uses a cancellation-sidecar timer, not payload polling. An unbounded
Python receive uses 0.5-second native deadline quanta only to return to CPython
and call its normal signal check.

Measured full-run evidence:

| Check | Result |
| --- | --- |
| 10-second no-data wait | 10.001873 s wall, 0.039186 s process CPU |
| Idle resources | `(56084 KiB, 20 fd, 18 threads)` before and after |
| Idle stats | 20 native WaitSet calls, 20 wakeups, 0 payload samples |
| GIL worker progress | 32,425,642 increments during a 1-second blocked receive |
| SIGINT | `KeyboardInterrupt` path returned in 0.510627 s |
| SIGTERM | Blocked process terminated within the 2-second test bound; no hang |

## Pressure and isolation

Python uses the frozen Sensor policy: queue 1, history 0, safe overflow, one
concurrent borrow, and two publisher loans. It adds no Python-side queue or
buffer pool.

| Scenario | Observed result |
| --- | --- |
| Slow Python consumer | Sequences `[0, 17, 35]`; gaps, bounded backlog, publisher continued |
| Stalled Python consumer | Publisher completed 40 frames with no send errors; Rust fast peer received at least 20; Python received 0 |
| Retained ndarray | Publisher completed 40 frames; Rust fast peer received at least 20; Python saw native borrow failure, then recovered after drop |

The broader 48-second pressure graph also passed Sensor fanout, Control
partial delivery, loan and borrow recovery, publisher/subscriber SIGKILL,
slow/stalled start-stop, provider replacement, Composer restart, capability
identity rotation, and resource-growth checks. It reported zero orphan
capabilities, zero stale bindings, and `obvious_unbounded_growth=0`.

## Packaging

The package exposes `import rbt`, version `0.2.0`, `py.typed`, and a basic
stub for the public receive-only API. Maturin builds the sole native module as
`rbt._rbt`; NumPy remains a normal Python dependency.

Definitive wheel:

```text
rbt-0.2.0-cp312-cp312-linux_x86_64.whl
```

The wheel contains no `.pyc`/cache entries, repository path strings,
`RPATH`, or `RUNPATH`. With `LD_LIBRARY_PATH` and `LIBRARY_PATH` unset, its
native dependencies resolve only to system `libgcc_s`, `libc`, and the ELF
loader; there is no dynamic `librbt_runtime` or iceoryx2 dependency. A fresh
virtual environment outside the repository installed the wheel, imported
`rbt` as 0.2.0, and received a real Rust-published Image with shape
`(4, 64, 3)` and strides `(192, 3, 1)`.

The release verifier links the selected PyO3 extension with rust-lld because
the bundled conda GCC injects its own absolute toolchain RPATH. Rust
dependencies still use the selected GNU toolchain; the resulting wheel audit
proves that no build-tree path leaks into the artifact.

## Unsafe and native ownership audit

The production Rust unsafe count changed from 31 to 43: old 31, new 43,
delta +12. All 12 locations are in the single PyO3 read-only buffer-export
boundary in `crates/rbt-python/src/lib.rs`:

- fill a CPython-owned `Py_buffer`;
- transfer one strong owner reference into `Py_buffer.obj`;
- expose the immutable SHM pointer while that owner is alive;
- release the optional format allocation paired with that buffer export;
- implement the two Image and PointCloud `__getbuffer__` / `__releasebuffer__`
  pairs.

The crate forbids unsafe operations outside explicit unsafe blocks, and each
location carries its local SAFETY and ownership rationale. No unsafe lifetime
erasure, self-reference, leaked Subscriber, manual sample free, or ownerless
NumPy pointer exists. The native extension ownership subset passed 18 tests
under an isolated Rust ASan build. Leak detection is disabled for that CPython
ASan process because interpreter allocations are not a usable SHM-ownership
oracle; exact borrow-failure/recovery and GC/resource tests prove release. The
existing C++ ASan/UBSan/leak suite remained enabled and passed.

## IDL and Python API

`rbt-idl generate` gained an optional `--python` output containing only
deterministic message descriptors and dtype specifications. Two independent
generations were byte-identical and matched the checked-in sensor, control,
and demo artifacts. Existing Rust and C++ generated artifacts remained
unchanged except for intended readiness transport use outside message layout.

The public Python scope is `StreamDescriptor`, message metadata, Image and
PointCloud subscribers/samples, pressure and WaitSet statistics, raw
read-only payload views, and typed errors. Timeout/no sample is represented by
`None`; ABI, transport, wait, payload, alignment, format, and borrow errors are
typed exceptions.

## Quality gates

The definitive command was:

```bash
./scripts/verify_python.sh --full
```

It passed without skipped inherited gates:

- Cargo format, workspace tests, strict all-target/all-feature Clippy, and
  Rust docs/tests;
- CMake Debug and Release builds with warnings as errors, 6/6 CTest in each;
- deterministic Rust/C++/descriptor/Python IDL generation and checked-in
  comparisons;
- 32 full Python tests in 38.61 seconds;
- 18 ASan-instrumented native ownership tests in 1.99 seconds;
- source zero-copy audit with `forbidden_matches=0`;
- fresh wheel install/import/external producer receive and ELF dependency
  audits;
- full v0.1/Phase 1-4 regression, external installed Rust/C++ consumers,
  mixed-language runtime/replacement/restart, fault soak, and benchmark smoke;
- existing C++ ASan/UBSan/leak suite, 6/6 CTest;
- full WaitSet correctness, fairness, cancellation, lifecycle, 100 activation
  cycles, benchmark matrix, and 45-second idle-resource regression;
- full pressure smoke and graph regression.

Evidence directories:

```text
target/python-verify-logs/python-full-20260911T134556Z-229210
target/v01-logs/verify-full-20260911T134828Z-234388
target/waitset-verify-logs/waitset-full-20260911T134722Z-231334
target/waitset-bench-logs/waitset-20260911T135230Z-248710
target/waitset-idle-logs/waitset-idle-20260911T135338Z-253598
target/pressure-logs/pressure.8hK9Bj
```

After adding the padded-image checksum assertion to the evidence line, its
targeted multiprocess regression passed again; the retained output is
`target/python-verify-logs/post-full-padded-checksum.log`.

## Behavioral changes

| Area | Change |
| --- | --- |
| Payload ABI and layout hashes | None |
| Sensor and Control QoS | None |
| Lifecycle / activation semantics | None |
| WaitSet semantics and bounded drain | None |
| Readiness notification delivery meaning | None |
| Transport compatibility | Explicit SidecarV1 advertisement and preflight |
| SDK/API surface | v0.2 receive-only Python package and Python IDL metadata |

## Deferred work at the time of this report

This receive-only milestone intentionally deferred the following work:

- Python publisher writable-loan lifetime;
- Python Component runtime;
- MCAP;
- ROS2 bridge;
- network gateway;
- schema evolution;
- advanced loan-pool tuning.

The publisher ownership work and synchronous Component adapter were completed
in later milestones. Their current contracts are documented in
[`python.md`](python.md) and [`python-component.md`](python-component.md);
asyncio, MCAP, ROS2, the network gateway, schema evolution, and advanced pool
tuning remain deferred.

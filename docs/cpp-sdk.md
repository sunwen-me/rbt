# C++ SDK

The C++ SDK is version `0.8.0`: a linked C++20 component runtime plus a typed
iceoryx2 data-plane API. It implements the same preparation, activation,
ownership, control-protocol, snapshot, reconnect, and provider-replacement
semantics as the Rust runtime. It uses the maintained native iceoryx2 fork
`0.10.0-rbt.2`.
See the [developer docs index](README.md) for the guide map and current
version axes.

## Public surface

| Header | Purpose |
| --- | --- |
| `rbt/runtime.hpp` | `Component`, `ActivationContext`, `ComponentRuntime`, owning control DTOs and codec |
| `rbt/effect_scope.hpp` | critical/background tasks, `TaskEvent`, cooperative stop, LIFO cleanup |
| `rbt/stream.hpp` | fixed/slice publishers, subscribers, loan and receive guards |
| `rbt/rpc.hpp` | typed, bounded local request/response endpoints |
| `rbt/time.hpp` | clock identities, timestamps, mappings, and checked arithmetic |
| `rbt/time_stream.hpp` | timing metadata in ordinary byte-slice streams |
| `rbt/tf_math.hpp` | transform math and frame identifiers |
| `rbt/transforms.hpp` | bounded local transform history and lookup |
| `rbt/tf_transport.hpp` | native dynamic and static TF endpoints and wire transport |
| `rbt/tf_component.hpp` | activation-owned TF provider/subscriber helpers |
| `rbt/tf_wire.hpp` | TF wire-record validation and codecs |
| `rbt/types.hpp` | descriptors, profiles, result/error types |
| `rbt/version.hpp` | SDK 0.8.0, IDL language 1, descriptor 1, control protocol 2, snapshot 4 |
| `rbt/demo.hpp`, `rbt/sensor.hpp`, `rbt/control.hpp`, `rbt/tf.hpp` | generated message structs and descriptors; `tf.hpp` is distinct from the native TF helper headers |

All SDK types are in `namespace rbt`; generated schema types are in the schema
namespace, for example `rbt::demo::Frame` and `rbt::sensor::ImageHeader`.
Headers do not add generated names to the global namespace.

## Component contract

```cpp
class Detector final : public rbt::Component {
public:
    rbt::Result<void> prepare() override;
    rbt::ComponentId id() const override;
    std::vector<rbt::RequirementSpec> requirements() const override;
    rbt::Result<void> activate(rbt::ActivationContext& context) override;
};
```

`prepare()` runs exactly once per process and owns long-lived resources such
as a model. `requirements()` declares stable name/type pairs. Each accepted
`activate()` receives a fresh activation id and complete resolved bindings.
Use `context.require<Tag>(name)` for an input and
`context.provide<Tag>(capability_id, descriptor)` for an activation-owned
output.

For an RPC capability, `ActivationContext::provide_rpc<Tag>(capability_id,
descriptor)` stages its descriptor for the activation. Include `<rbt/rpc.hpp>`
for the native RPC endpoints. `<rbt/tf_component.hpp>` provides native C++
`rbt::tf::provide_dynamic_tf`, `provide_static_tf`, `require_dynamic_tf`, and
`require_static_tf` helpers that scope their endpoints to the activation. The
C++ TF helpers use the C++ transport directly and do not call through Rust FFI;
see [`tf.md`](tf.md) for the cross-language API and verification boundaries.

`ComponentRuntime` owns the component, generates a process UUID, connects with
bounded `100/200/500/1000 ms` backoff, sends `HELLO` plus complete snapshots,
applies only current identity/newer revision commands, and preserves a healthy
activation while Composer is unavailable. `stop()` requests a clean shutdown;
the destructor is the synchronous safety net.

The complete detector used in the real mixed-language test is
`examples/cpp/detector/main.cpp`. The same tree contains buildable Camera and
Viewer components. The detector's core activation shape is:

```cpp
auto camera = context.require<Camera>("camera");
auto subscriber = camera.slice_subscriber<rbt::sensor::ImageHeader, std::uint8_t>();

const auto capability = rbt::generate_uuid();
auto descriptor = rbt::OwnedStreamDescriptor::from(
    rbt::StreamDescriptor("rbt/cap/" + capability + "/stream",
                          rbt::demo::DETECTION_DESCRIPTOR,
                          rbt::StreamProfile::Sensor));
auto publisher = rbt::FixedPublisher<rbt::demo::Detection>::create(
    descriptor.borrowed(), rbt::demo::DETECTION_DESCRIPTOR);
auto provided = context.provide<Detections>(capability, descriptor);

context.spawn_critical("inference", [subscriber = std::move(subscriber).value(),
                                      publisher = std::move(publisher).value()]
                                     (std::stop_token stop) mutable {
    // Receive while the guard lives, copy/compute, then loan and send output.
    return rbt::TaskOutcome::completed();
});
```

Check every returned `Result`/handle in application code; the abbreviated
snippet only highlights ownership boundaries.

## EffectScope and task policy

An activation owns one `EffectScope`.

- `spawn_critical` is for work whose unexpected exit invalidates the
  activation. Error, exception, panic-equivalent, or premature completion
  emits an identity-bearing `TaskEvent`; runtime teardown withdraws every
  activation-owned capability and reports `FAILED`.
- `spawn_background` is for auxiliary work such as metrics. Its failure is
  observable but does not invalidate the activation.
- Cancellation during requested teardown is not reported as an unexpected
  task failure.
- Tasks use `std::jthread` and `std::stop_token`, are never detached, are joined
  before owned resources disappear, and cleanup callbacks run once in strict
  LIFO order.

`TaskEvent` includes process instance, activation id, task name,
critical/background classification, and outcome. Stale events from an old
activation are ignored.

## Data-plane ownership

Publishers use `open_or_create`; subscribers use `open`. Both validate the
complete generated descriptor before opening transport. A mismatch returns
`StreamError::AbiMismatch` with no endpoint opened.

`StreamResourceConfig` exposes only max_loans and max_borrowed; queue depth 1
for Sensor or 4 for Control, history 0, and safe-overflow are derived from the
profile. Resource-aware constructors reject zero or incompatible values. Public
pressure classifications match Rust:
`QueueFull`, `UnableToDeliver`, `LoanPoolExhausted`, and
`BorrowLimitReached` remain distinct from generic `Transport` failures.

Fixed and slice publishers/subscribers expose `pressure()`. Publisher
snapshots count loan attempts/successes/current/high-water/failures, payload
bytes, send attempts/successes/failures, queue/delivery and readiness failures,
and aggregate connected subscribers. Subscriber snapshots count receives,
current/high-water Borrows, limit failures, completed monotonic durations, and
oldest current borrow age. `local_pressure_snapshots()` feeds the low-rate
component snapshot path without extending endpoint lifetimes.
For Control `QueueFull`, iceoryx2's backpressure callback also supplies the
offending receiver port id; C++ records its 32-character lower-case hex form
as optional transport diagnostics. It is not a process/component identity.
Unknown queue occupancy and safe-overflow Drop totals remain absent.

`LoanedSample` and `LoanedSliceSample` are move-only owners. `send()` consumes
the native loan, while destruction releases an unsent loan. `ReceivedSample`
and `ReceivedSliceSample` keep the transport borrow alive. A returned
`std::span` is valid only while its receive guard lives; copy it before the
guard is destroyed if longer retention is needed.

Owning endpoints are non-copyable and move-only. Their destruction order keeps
the endpoint/factory/node and backpressure handler valid for the lifetime
required by iceoryx2.

## Build in the repository

```bash
./scripts/bootstrap_iceoryx2_cpp.sh
cmake -S sdk/cpp -B target/rbt-cpp-debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$PWD/target/iox2-rbt-0.10.0-cpp-install"
cmake --build target/rbt-cpp-debug --parallel
ctest --test-dir target/rbt-cpp-debug --output-on-failure
```

The SDK build uses `-Wall -Wextra -Wpedantic -Werror`. Its unit tests cover
descriptors, stream compilation/lifetime, EffectScope, and runtime/control
parity. The same build also produces `camera_component`,
`detector_component`, and `viewer_component` when generated demo headers are
present.

## Install and consume

```bash
./scripts/install_v01.sh --prefix "$PWD/target/rbt-install"
```

This installs:

```text
include/rbt/
lib/librbt_runtime.a
lib/cmake/rbt/rbtConfig.cmake
lib/cmake/rbt/rbtTargets.cmake
share/licenses/rbt/
```

A fresh application uses the standalone CMake file included in each C++
example:

```cmake
find_package(rbt CONFIG REQUIRED)
add_executable(my_component main.cpp)
target_compile_features(my_component PRIVATE cxx_std_20)
target_link_libraries(my_component PRIVATE rbt::runtime rbt::iox2)
```

Configure with both the rbt and pinned iceoryx2 prefixes in
`CMAKE_PREFIX_PATH`. `scripts/test_install.sh` proves this from a temporary
project outside the source tree. Generate application types explicitly with
`rbt-idl generate` before building; CMake does not run the IDL generator during
configure.

## Runtime parity evidence

`scripts/test_mixed_runtime.sh` runs Rust Camera → C++ Detector → Rust Viewer,
replaces the provider without restarting the detector, verifies its model was
prepared once, kills/restarts Composer with unchanged runtime identities and
`0 deactivate, 0 activate`, then injects a C++ critical-task failure and checks
capability withdrawal and downstream blocking. With
`RBT_MIXED_ENABLE_PRESSURE=1`, the same graph keeps a slow C++ Borrower and a
never-receiving Rust subscriber active through provider replacement and
Composer restart, then verifies capability-scoped counter reset/continuity.
`scripts/test_mixed_pressure.sh` separately exercises both publisher-language
directions.

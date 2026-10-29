# Examples and learning path

These v0.8 examples are grouped by the concept they exercise.  The
canonical first run is the generated Rust project in
[the quickstart](../docs/quickstart.md); the existing examples below are
reference implementations and regression fixtures.  Every row has one build
or verification command, an observable result, and a cleanup rule.

| Lesson | Reference | Run/build | What to observe | Cleanup |
| --- | --- | --- | --- | --- |
| 1. Hello stream | `examples/rust/camera`, `examples/rust/viewer`, generated `hello-source`/`hello-sink` | `rbt new hello-robot && cd hello-robot && rbt run` | Both components become `ACTIVE`; the sink prints sequence/value samples | Ctrl-C `rbt run` |
| 2. Camera → detector → viewer | `examples/rust/camera`, `examples/rust/detector`, `examples/rust/viewer` | `./scripts/test_mixed_runtime.sh` | A synthetic Camera feeds a detector and viewer with no hardware | The script reaps all children on exit |
| 3. Provider replacement | `scripts/test_mixed_runtime.sh` | `./scripts/test_mixed_runtime.sh` | A killed provider makes the dependent component `BLOCKED`; a replacement returns it to `ACTIVE` | The script uses isolated control addresses and traps |
| 4. Python zero-copy | `examples/python/image_stats.py`, `examples/python/pointcloud_stats.py` | `./scripts/verify_python.sh --quick` | Borrowed NumPy views remain read-only and retain their receive guard | The verifier closes subscribers and removes its temp prefix |
| 5. Python asyncio | `examples/python/async_component_detector.py` | `./scripts/verify_python_asyncio.sh --quick` | Async receive/cancellation uses the existing runtime and releases the GIL | The verifier terminates its graph and venv processes |
| 6. Pressure diagnosis | `examples/advanced/image-consumer`, `docs/pressure.md` | `./scripts/verify_pressure_tuning.sh --quick` | `BorrowLimitReached`, `LoanPoolExhausted`, and delivery pressure remain typed and visible | The pressure harness bounds held samples and reaps children |
| 7. MCAP record/replay | `examples/advanced/mcap-source`, `docs/mcap.md` | `./scripts/verify_mcap.sh --quick` | `record`, `mcap info`, and `play` preserve canonical schemas and report drops/errors | The verifier removes only its temporary recordings |
| 8. Fixed local RPC | `examples/08-rpc`, `docs/rpc.md` | `cargo run -p composer` plus `cargo run -p rpc-onboarding -- provider` / `-- client` | Loaned `Frame` requests receive correlated `Detection` responses with bounded timeout and activation cancellation | Stop the three foreground processes; restart the provider to exercise replacement |

| 9. Identified time and stamped payloads | [Time guide](../docs/time.md), [physical camera example](09-time-camera/README.md) | `cargo run -p rbt-cli -- time status --json` | Local host clock identity and availability are explicit; the linked camera path carries capture metadata with the payload | The status command exits; camera cleanup is described in its guide |
| 10. Local static transforms | [Local TF example](advanced/tf-local/README.md), [TF guide](../docs/tf.md) | `cargo run -p rbt-tf-local -- --config docs/tf-config-example.toml` | Explicit fixture extrinsics map a point from `camera_optical` to `base_link` and back at an identified timestamp | The local software example exits and opens no hardware |

Physical camera/TF work uses the separate [hardware example](10-tf-camera/README.md)
with an explicitly selected device and supplied extrinsics. The fixture config
is not measured calibration. Physical-camera and physical dynamic-TF acceptance
remain **NOT RUN** in the v0.8 TF report.

The [documentation index](../docs/README.md) and
[framework development guide](../docs/development.md) explain the current SDK
and how to select a focused check for an example change.

For an individual example, read its source and the linked verifier before
changing it.  The normal application path uses semantic requirements and
bindings; it does not ask users to hand-write physical iceoryx2 service names.
The scripts are bounded tests as well as runnable demonstrations, so their
logs show the expected lifecycle and failure transitions without requiring a
five-terminal manual setup.

## Interop references

- [Rust image → C++ consumer](interop/rust-image-cpp-consumer/README.md)
- [C++ PointCloud → Rust consumer](interop/cpp-pointcloud-rust-consumer/README.md)
- [Phase 4 runtime harness](interop/phase4-runtime/README.md) (run by the
  phase verification script)

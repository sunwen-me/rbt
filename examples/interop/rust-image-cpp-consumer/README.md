# Rust Image publisher to C++ consumer

This checked interop direction is driven by `scripts/verify_phase4.sh`. The
Rust process implementation is in `../phase4-runtime`; the C++ consumer is in
`sdk/cpp/src/interop_cpp.cpp`. Keeping the process harness shared avoids two
copies of the same ABI probe while this directory provides the public example
entrypoint requested by the v0.1 layout.

Run it from the repository root:

```bash
./scripts/verify_phase4.sh
```

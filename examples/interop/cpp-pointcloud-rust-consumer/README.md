# C++ PointCloud publisher to Rust consumer

This checked interop direction is driven by `scripts/verify_phase4.sh`. The
C++ publisher is in `sdk/cpp/src/interop_cpp.cpp`; the Rust process
implementation is in `../phase4-runtime`. The verifier compares generated
layouts first and then moves real samples through iceoryx2.

Run it from the repository root:

```bash
./scripts/verify_phase4.sh
```

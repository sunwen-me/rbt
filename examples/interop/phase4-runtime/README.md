# Phase 4 runtime harness

This crate is the shared executable harness used by the cross-language ABI and
transport checks. It is an implementation reference, not the first onboarding
path. Run it through the bounded verifier from the repository root:

```bash
./scripts/verify_phase4.sh
```

The verifier builds the pinned iceoryx2 0.9.3 C++ side, compares deterministic
IDL output, moves real Image and PointCloud samples across the Rust/C++
boundary, and cleans up every child process on exit. See
[the ABI documentation](../../../docs/abi.md) for the durable contract.

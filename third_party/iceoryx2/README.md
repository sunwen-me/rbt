# iceoryx2 0.9.3 + rbt patches

This is the historical 0.9.3 baseline retained for defect reproduction. Active
production now uses the unified [maintained 0.10 baseline](../iceoryx2-rbt/README.md),
including ports of these fixes alongside the network changes. The source here
is **not** unmodified upstream and is no longer the production dependency.

## Defect

A server that retains an `ActiveRequest` (or a loaned `ResponseMut`) from a
client that has since disconnected keeps the numeric connection slot index.
When a new client reuses that slot, its request ids also restart at 0, so the
stale response is delivered into the new client's channel and accepted by its
first pending response. `ActiveRequest` drop, `is_connected` and
`has_disconnect_hint` likewise act on the new client's channel. Separately,
client request ids wrapped modulo `RequestId::max_value()`.

## Patches

- `patches/0001-iceoryx2-stale-response-correlation.patch` (`iceoryx2` crate):
  re-resolve the connection by the request's origin client id under the shared
  state lock; `ResponseMut` keeps the client id and request id and drops the
  response if the channel no longer belongs to that request; request id
  allocation stops instead of wrapping (`LoanError::InternalFailure`).
  A stale `send()` still returns `Ok(())` (fire-and-forget).
- `patches/0002-iceoryx2-ffi-c-response-mut-storage.patch` (C FFI):
  `iox2_response_mut_storage_t` grows from 88 to 112 bytes. This changes the
  C ABI, so the C/C++ libraries and headers must be rebuilt from the patched
  source (`scripts/bootstrap_iceoryx2_cpp.sh` does this).

No wire header, payload layout, serializer, retry or transport changes.

## Layout

- `iceoryx2/`: crates.io `iceoryx2-0.9.3.crate`
  (sha256 `e226c604…627f`) with patch 0001 applied. Used by Rust and the
  Python extension through `[patch.crates-io]` in the root `Cargo.toml`.
- C/C++: `scripts/bootstrap_iceoryx2_cpp.sh` applies all patches to a copy of
  upstream commit `6ef7c55176a8487ae7ca53c2e0649d5df9a1b3b4`.

Run `third_party/iceoryx2/verify_vendor.sh` to check that the vendored crate
is exactly the published crate plus the patch.

## Upstream

The fix has been ported to upstream `main` together with conformance tests;
drop these patches once an iceoryx2 release containing it is adopted.

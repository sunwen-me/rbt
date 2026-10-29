"""Leave a publisher loan and writable aliases live through interpreter exit."""

from __future__ import annotations

import signal
import sys

import rbt


service = sys.argv[1]
blocking = len(sys.argv) > 2 and sys.argv[2] == "block"
if len(sys.argv) > 2 and not blocking:
    raise SystemExit(f"unexpected mode: {sys.argv[2]!r}")
descriptor = rbt.StreamDescriptor.image(service)
publisher = rbt.ImagePublisher.open(descriptor, maximum_payload_bytes=256)
loan = publisher.loan(8, 4, rbt.PixelFormat.RGB8, sequence=17)
image = loan.numpy()
image[...] = 0x5A
reshaped = image.reshape(16, 6)
transposed = reshaped.transpose()
payload = memoryview(transposed)

# The native loan and the writable aliases are intentionally not released.
# CPython must tear them down in a safe order while the process exits.
del loan
print(
    "PUBLISHER_EXIT_PROBE",
    f"service={service}",
    f"shape={image.shape}",
    f"payload_nbytes={image.nbytes}",
    f"blocking={blocking}",
    flush=True,
)
if blocking:
    signal.pause()

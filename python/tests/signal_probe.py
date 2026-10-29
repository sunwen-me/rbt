from __future__ import annotations

import sys

import rbt

subscriber = rbt.ImageSubscriber.open(rbt.StreamDescriptor.image(sys.argv[1]))
try:
    print("BLOCKING", flush=True)
    subscriber.receive()
except KeyboardInterrupt:
    print("KEYBOARD_INTERRUPT", flush=True)
    raise SystemExit(0)
raise SystemExit("receive returned without a sample or signal")

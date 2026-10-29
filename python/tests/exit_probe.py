from __future__ import annotations

import sys

import rbt

subscriber = rbt.ImageSubscriber.open(rbt.StreamDescriptor.image(sys.argv[1]))
sample = subscriber.receive(timeout=5)
assert sample is not None
image = sample.numpy()
crop = image[:1, :1]
assert crop.size == 3
# Intentionally retain sample, ndarray, and derived view through interpreter exit.

#!/usr/bin/env python3
"""Print metadata and a checksum for one borrowed Image sample."""

import argparse

import numpy as np

import rbt

parser = argparse.ArgumentParser()
parser.add_argument("service")
args = parser.parse_args()

subscriber = rbt.ImageSubscriber.open(rbt.StreamDescriptor.image(args.service))
sample = subscriber.receive()
assert sample is not None
image = sample.numpy()
print(
    f"sequence={sample.sequence} shape={image.shape} strides={image.strides} "
    f"checksum={int(image.sum(dtype=np.uint64))} writeable={image.flags.writeable}"
)

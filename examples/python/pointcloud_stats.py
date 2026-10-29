#!/usr/bin/env python3
"""Print the centroid of one borrowed PointCloud sample."""

import argparse

import rbt

parser = argparse.ArgumentParser()
parser.add_argument("service")
args = parser.parse_args()

subscriber = rbt.PointCloudSubscriber.open(
    rbt.StreamDescriptor.point_cloud(args.service)
)
sample = subscriber.receive()
assert sample is not None
points = sample.numpy()
if len(points):
    centroid = tuple(float(points[axis].mean()) for axis in ("x", "y", "z"))
else:
    centroid = (0.0, 0.0, 0.0)
print(
    f"sequence={sample.sequence} points={len(points)} centroid={centroid} "
    f"writeable={points.flags.writeable}"
)

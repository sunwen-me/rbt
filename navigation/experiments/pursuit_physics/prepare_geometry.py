#!/usr/bin/env python3
"""Cover the experiment's assumed caster trail without changing robot defaults."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path

import yaml


def convex_hull(points):
    points = sorted(set(points))

    def cross(o, a, b):
        return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0])

    lower, upper = [], []
    for p in points:
        while len(lower) >= 2 and cross(lower[-2], lower[-1], p) <= 0:
            lower.pop()
        lower.append(p)
    for p in reversed(points):
        while len(upper) >= 2 and cross(upper[-2], upper[-1], p) <= 0:
            upper.pop()
        upper.append(p)
    return lower[:-1] + upper[:-1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    geometry = yaml.safe_load(args.source.read_text())
    original = [tuple(p) for p in geometry["footprint"]]
    trail, caster_radius, sides = 0.02, 0.026, 16
    # A circumscribed polygon contains the full circle, including between vertices.
    radius = (trail + caster_radius) / math.cos(math.pi / sides)
    points = list(original)
    for x, y in [(-0.301, 0.20175), (-0.301, -0.20175)]:
        points.extend((x + radius * math.cos(2 * math.pi * i / sides),
                       y + radius * math.sin(2 * math.pi * i / sides))
                      for i in range(sides))
    geometry["footprint"] = [list(p) for p in convex_hull(points)]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        "# Experiment only: source chassis + full sweep of assumed 20 mm caster trail.\n"
        "# The production robot geometry is unchanged.\n"
        + yaml.safe_dump(geometry, sort_keys=False)
    )
    args.output.with_suffix(".provenance.json").write_text(json.dumps({
        "source": str(args.source.resolve()),
        "source_sha256": hashlib.sha256(args.source.read_bytes()).hexdigest(),
        "output_sha256": hashlib.sha256(args.output.read_bytes()).hexdigest(),
        "assumed_caster_trail_m": trail,
        "rounded_caster_radius_m": caster_radius,
        "caster_pivots_xy_m": [[-0.301, 0.20175], [-0.301, -0.20175]],
        "circle_polygon_sides": sides,
        "circumscribed_radius_m": radius,
        "footprint_vertices": len(geometry["footprint"]),
        "note": "Conservative collision envelope for the experimental contact assumption; not a robot measurement.",
    }, indent=2) + "\n")


if __name__ == "__main__":
    main()

# Vector Pursuit geometry source record

The experimental Vector Pursuit geometry adapts the core `calcTurningRadius`
calculation from `blackcoffeerobotics/vector_pursuit_controller`, pinned to
commit `0a8b8ee32b2ffcc25826be61fd289d2a0e72efbc`, in
`src/vector_pursuit_controller.cpp` (lines 269–303). The source carries
Copyright (c) 2020 Shrijit Singh and Samsung Research America, and Copyright
(c) 2024 Black Coffee Robotics. The source is licensed under Apache-2.0; a
copy is included as `LICENSE-2.0.txt`.

RBT uses the upstream radius calculation for curvature, with finite-value
handling at the singular denominator and a configurable minimum radius. This
controller combines that geometry with RBT's existing path segmentation,
speed regulation, dynamic-window or acceleration limits, and collision and
measured-motion checks. It is a Vector Pursuit geometry experiment with RBT
common regulators, not an exact port of the upstream plugin.

The optional `vector_pursuit_use_heading_from_path` switch follows the upstream
heading-source choice: stored/interpolated path pose yaw when true, selected XY
segment tangent yaw when false. RBT retains its existing arc-length lookahead
and preserves the original path yaw for segmentation and final goal alignment.

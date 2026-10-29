# Native state lattice planner port

This planner is a native C++ subset derived from Nav2 Smac Lattice at commit
`426682ff88405a99617294f5cc8401a61aa5fa50`. It reads the official differential
drive control set in `config/lattice/diff-5cm-0_5m.json`: 16 non-uniform heading
bins and 112 motion primitives at 5 cm resolution.

The search state uses integer lattice offsets from the caller's actual start
position and a discrete body heading. Primitive coordinates are expressed in
the map grid frame, then rotated by the map origin yaw into world coordinates.
The map resolution must match the control set resolution. The continuous start
position remains the anchor; the planner never snaps or slides it to a cell
center. It may enqueue several starting headings after checking each initial
in-place rotation with the footprint checker.

Each expansion uses the control set's sampled poses. Forward primitives retain
their declared headings. Reverse expansion selects primitives from the
opposite heading and adds π to every sampled body yaw, preserving the physical
vehicle orientation. The state key tracks the last turn side and travel
direction for transition costs. Backtrace emits the primitive samples in order,
including repeated positions where the vehicle rotates in place. The local
exact-goal connector checks rotate, straight travel, and final rotation in that
order; when reverse is enabled, it also checks the corresponding reverse
straight segment. Every pose and neighboring sample sweep is checked against
the oriented polygon footprint.

The planner uses a local A* loop with Euclidean distance as its lower-bound
heuristic. It does not include Nav2's OMPL Dubins/Reeds-Shepp analytic
expansion, obstacle heuristic lookup, search cache, or smoother. The exact-goal
connector is a small local replacement for that analytic expansion and does
not claim equivalent coverage. The RBT map has binary occupancy plus an ESDF,
so the local traversal cost uses clearance near obstacles rather than Nav2
inflation-layer costs. The result is not the complete Nav2 Smac planner package
or a drop-in behavior equivalent.

When a popped node is within goal tolerance, the planner checks the available
forward and reverse exact-goal connectors from that node and chooses the lower
cost valid connection. It backtraces immediately on the first node with a valid
connector, matching Nav2 A*'s successful analytic-expansion termination. This
does not claim a globally minimum-cost path; the Euclidean heuristic does not
account for heading changes.

The official 5 cm primitive samples are discretized waypoints rather than exact
continuous wheel commands. An independent midpoint-heading check across the
112-file control set measured a maximum lateral chord residual of 1.0471% of a
sample interval (0.5222 mm on a 49.87 mm chord). The planner test allows 1.1%
plus 0.05 mm for this source-table quantization; it also rejects lateral goal
connectors and checks the resulting footprint path with a separate polygon
versus occupied-cell test.

Timeout and cancellation are checked throughout the search. Reaching the
configured expansion cap reports `PlanningTimeout`; an exhausted search with
no checked exact-goal connector reports `NoPath`. Paths are returned in the
caller's map frame and retain the exact requested endpoint pose.

The upstream source provenance and file hashes are recorded in
`../licenses/nav2-lattice/README.md`. The adapted source is distributed under the
Apache License 2.0 in that directory.

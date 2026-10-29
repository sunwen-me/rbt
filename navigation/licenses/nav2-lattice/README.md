# Nav2 Smac Lattice source record

The native state lattice planner is derived from the differential-drive
NodeLattice expansion and A* search model in Nav2 Smac Planner. The pinned
upstream revision is `426682ff88405a99617294f5cc8401a61aa5fa50` (Apache-2.0).
The reference checkout is cached at `target/nav2-lattice-reference`.
The relevant upstream source notices are Copyright (c) 2021, Samsung Research
America (NodeLattice) and Copyright (c) 2020, Samsung Research America and
Applied Electric Vehicles Pty Ltd (A*). Those notices are retained on the
adapted implementation alongside the RBT contribution notice.

SHA-256 hashes of the inspected upstream files:

| Upstream file at the pinned revision | SHA-256 |
| --- | --- |
| `nav2_smac_planner/src/node_lattice.cpp` | `0e7431d8e65b20a8376c69647b34bf93937982632e7e8182df829e8b2c737836` |
| `nav2_smac_planner/src/a_star.cpp` | `5e33a489ff3eb77ff607326d7552798a1155023b7d82b669988b61d786fa9126` |

The official generated control set retained at
`navigation/config/lattice/diff-5cm-0_5m.json` has SHA-256
`105b37351d15e340cc2921fa8878c8ea727a65cd7e89c39c977be0654be3b2a9`.
It contains 16 headings and 112 `diff` primitives at 5 cm resolution with a
0.5 m configured turning radius.

The implementation is an adaptation of the algorithm and control-set format,
not a copy of the full Nav2 plugin. See `navigation/docs/lattice-port.md` for
the supported behavior and omitted Nav2 components. The Apache-2.0 license
text is included as `LICENSE-2.0.txt`.

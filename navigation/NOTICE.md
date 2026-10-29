# Upstream and third-party notices

Original RBT portions use RBT's MIT OR Apache-2.0 licensing. The Nav2-derived
portions described below are licensed under Apache-2.0.

The occupancy → binary → signed-distance map design and grid A* structure were
adapted after inspecting [ROSE Navigation](https://github.com/hyheiyue/rose_navigation),
commit `c979eb8e36d1621b31a04884af66ee3e3f5e8afd` (2026-10-21).
Copyright (c) 2026 hyheiyue. Its MIT notice is preserved in
`licenses/ROSE-MIT.txt` and applies to the adapted portions in `src/grid_map.cpp`
and `src/a_star.cpp`.

The RPP controller and analytic DWPP velocity solver are adapted from
[Nav2](https://github.com/ros-navigation/navigation2), commit
`cea9ed9d2e55b4e084be705a495a721368021f19`. Applicable notices include
Copyright (c) 2020 Shrijit Singh, Copyright (c) 2020 and 2022 Samsung Research
America, and Copyright (c) 2025 Fumiya Ohnishi. Adapted files preserve their
Apache-2.0 copyright/license headers. The license text and upstream package
license policy are included as `licenses/Nav2-APACHE.txt` and
`licenses/Nav2-LICENSE-POLICY.txt`; RPP's package.xml declares Apache-2.0.
No Nav2 ROS runtime, message implementation, plugin loader or costmap library
is bundled. See `docs/nav2-port.md` for the exact adaptation boundary.

The native `FeasiblePathHandler` follows the ordered path constraint handling
at the same RPP reference revision. Its upstream attribution includes
Copyright (c) 2022 Samsung Research America, Alexey Budyakov; Copyright (c) 2023
Dexory; and Copyright (c) 2023 Open Navigation LLC. The adapted source is
Apache-2.0; ROS TF, parameters and plugin lifecycle code are not included.

The experimental Vector Pursuit geometry calculation is adapted from
[blackcoffeerobotics/vector_pursuit_controller](https://github.com/blackcoffeerobotics/vector_pursuit_controller)
at commit `0a8b8ee32b2ffcc25826be61fd289d2a0e72efbc`. Applicable notices include
Copyright (c) 2020 Shrijit Singh and Samsung Research America, and Copyright
(c) 2024 Black Coffee Robotics. Apache-2.0 text and the adaptation boundary
are recorded in `licenses/vector-pursuit/`. RBT shares its own regulated speed,
ordered path handling and polygon collision checks across the experiments;
the upstream ROS 2 plugin runtime and speed policy are not bundled.

The State Lattice search structure is adapted from Nav2's `nav2_smac_planner`
at fixed Jazzy commit
[`426682ff88405a99617294f5cc8401a61aa5fa50`](https://github.com/ros-navigation/navigation2/tree/426682ff88405a99617294f5cc8401a61aa5fa50/nav2_smac_planner),
whose [pinned package.xml](https://github.com/ros-navigation/navigation2/blob/426682ff88405a99617294f5cc8401a61aa5fa50/nav2_smac_planner/package.xml)
declares Apache-2.0. The relevant NodeLattice source is
[node_lattice.cpp at that commit](https://github.com/ros-navigation/navigation2/blob/426682ff88405a99617294f5cc8401a61aa5fa50/nav2_smac_planner/src/node_lattice.cpp).
The copied upstream package license policy is preserved at
`licenses/nav2-lattice/LICENSE`, with the Apache-2.0 text at
`licenses/nav2-lattice/LICENSE-2.0.txt`; adapted source keeps its license
header. Source attribution and inspected-file hashes are recorded in
`licenses/nav2-lattice/README.md`; the native implementation and its exact
adaptation boundary are documented in `docs/lattice-port.md`. It does not
bundle Nav2 ROS runtime, OMPL analytic expansion, pluginlib, or costmap code.

No ROSE vendored third-party source is included. In particular, the uncertain
provenance `3rdparty/lbfgs.hpp`, the ankerl container, liblzf, Willow Garage's
angles header, small_gicp, and trajectory/controller/localization sources are
not copied or linked by this package. The exact separable Euclidean distance
transform is newly implemented here using the standard lower-envelope
algorithm; it is not ROSE's two-pass chamfer approximation.

Native targets consume the already licensed RBT SDK and maintained iceoryx2
build. The RBT notice index and iceoryx2's own MIT/Apache texts and NOTICE are
also preserved in `licenses/`. Redistributing the native SDK/binaries must
retain the SDK distribution's complete transitive dependency notices as well;
this extension does not replace those notices. The pure navigation library
uses only C++20 and the system threading library.

The optional, independently implemented `map_io` component reads the common
YAML + PGM/PNG occupancy-map format. Format conventions were checked against
Nav2's public map I/O implementation; no ROS or Nav2 map I/O source was copied.
This component links external yaml-cpp, libpng and zlib libraries. Distributing
those libraries must retain their respective upstream licenses; they are not
part of the dependency-free navigation core or message-only interface.

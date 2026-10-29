# Prior-map localization acceptance on AidLux

This standalone wrapper exercises the existing `PriorMapLocalization` and
vendored KISS/FastVGICP pipeline in the MID360 source tree. It does not edit or
replace vendor source, the original `build-navigation` tree, or the reviewed
configuration. The wrapper build is intentionally separate.

The test loads the reviewed YAML with Super-LIO's existing `Parameters` and
`load_parameters` functions. It first retains the YAML's `kiss_matcher.enable:
false` value and verifies that eight paced startup frames cannot initialize
localization. It then enables KISS only in memory, loads the reviewed 92,678
point prior map, crops a deterministic feature region, transforms those real
map points by a known 20 degree yaw and translation `(1.1, -0.7, 0.25)`, and
submits eight synthetic startup frames. The actual callback must recover the
known `map->odom` transform within 5 cm and 1 degree, set `navigationReady()`,
and age out after 2.2 seconds without another accepted registration. The YAML's
`initial_max_fitness: 0.01` and `max_fitness: 0.1` remain unchanged.

This is software-path acceptance with synthetic scans derived from the real
map. It does not validate the physical LiDAR frame, map calibration, motors,
robot motion, or an actual device session.

On AidLux, use a clean build directory and source the existing SDK environment:

```sh
cd /home/aidlux/mid360_lio_rbt
source scripts/env.sh
cmake -S /home/aidlux/rbt-navigation-20261027/navigation/integrations/mid360_lio/prior_map_acceptance \
  -B /home/aidlux/rbt-navigation-20261027/build-prior-localization \
  -DMID360_SOURCE_DIR=/home/aidlux/mid360_lio_rbt \
  -DRBT_NAVIGATION_SOURCE_DIR=/home/aidlux/rbt-navigation-20261027/navigation \
  -DGTSAM_DIR=/opt/ros/humble/lib/cmake/GTSAM \
  -DBUILD_TESTING=ON
cmake --build /home/aidlux/rbt-navigation-20261027/build-prior-localization --parallel 2
ctest --test-dir /home/aidlux/rbt-navigation-20261027/build-prior-localization \
  --output-on-failure
```

The CMake cache enables the existing `LIO_WITH_KISS_MATCHER`,
`LIO_WITH_FAST_GICP`, and `LIO_WITH_RBT_NAVIGATION` options. If the pinned
vendored KISS dependencies need fetching, retain the configure log and fetched
source checkout under the new build directory. Run the nine pre-existing
MID/LIO native tests in the same isolated build after it configures and builds:

```sh
ctest --test-dir /home/aidlux/rbt-navigation-20261027/build-prior-localization \
  --output-on-failure -E '^prior_map_acceptance$'
```

Save the configure, build, acceptance, and native CTest output under
`/home/aidlux/rbt-navigation-20261027/target/user-map-readiness/`; save the
structured acceptance result under
`/home/aidlux/rbt-navigation-20261027/output/navigation-lio/readiness/localization/`.

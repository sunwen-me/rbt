#pragma once

#include <rbt_navigation/map/grid_map.hpp>

#include <cstddef>
#include <filesystem>

namespace rbt::navigation {

// Loads a ROS-style occupancy-map YAML file and its P2/P5 PGM or PNG image.
// Image rows are flipped into the grid's lower-left, row-major convention.
// PNG color pixels use rounded Rec. 601 luminance. In trinary and scale mode,
// transparent pixels are unknown; raw mode uses gray values 0..100 directly
// and ignores both negate and alpha.
[[nodiscard]] OccupancyGrid loadOccupancyMap(
    const std::filesystem::path& yaml_file,
    std::size_t max_cells = 1'000'000);

} // namespace rbt::navigation

#pragma once

#include <rbt_navigation/geometry/footprint.hpp>

#include <string>

namespace rbt::navigation {

// Loads the base_footprint-local polygon and drive-to-base transform. Navigation
// poses are expressed in map at the drive axle.
[[nodiscard]] RobotGeometry loadRobotGeometry(const std::string& path);

} // namespace rbt::navigation

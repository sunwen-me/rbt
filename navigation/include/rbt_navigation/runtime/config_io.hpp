#pragma once

#include <rbt_navigation/docking/docking_manager.hpp>
#include <rbt_navigation/geometry/footprint.hpp>

#include <string>

namespace rbt::navigation {

[[nodiscard]] RobotGeometry loadRobotGeometry(const std::string& path);
[[nodiscard]] DockConfig loadDockConfig(const std::string& path);

} // namespace rbt::navigation

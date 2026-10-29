// Copyright (c) 2024 Black Coffee Robotics
// Copyright (c) 2026 RBT contributors
// Licensed under the Apache License, Version 2.0. See licenses/vector-pursuit.

#include <rbt_navigation/controller/vector_pursuit.hpp>

namespace rbt::navigation {
namespace {

[[nodiscard]] ControllerConfig vectorPursuitConfig(ControllerConfig config) noexcept {
    config.geometry = PursuitGeometry::VectorPursuit;
    return config;
}

} // namespace

VectorPursuit::VectorPursuit(ControllerConfig config)
    : controller_(vectorPursuitConfig(config)) {}

ControllerResult VectorPursuit::computeCommand(const RobotState& state,
                                               const Path2D& path,
                                               const ControllerContext& context) {
    return controller_.computeCommand(state, path, context);
}

} // namespace rbt::navigation

#pragma once

#include <rbt_navigation/safety/velocity_guard.hpp>
#include <rbt_navigation/simulation/simulated_base.hpp>

namespace rbt::navigation {

using SafeBase = VelocityGuard;
using SafeBaseConfig = VelocityGuardConfig;

} // namespace rbt::navigation

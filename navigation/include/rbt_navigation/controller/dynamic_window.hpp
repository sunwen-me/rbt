#pragma once
#include <rbt_navigation/core/types.hpp>

namespace rbt::navigation {
struct DynamicWindowBounds {
    double min_linear{}, max_linear{}, min_angular{}, max_angular{};
};
struct DynamicWindowLimits {
    double max_linear{0.6}, max_angular{1.5};
    double linear_acceleration{0.8}, linear_deceleration{0.8};
    double angular_acceleration{2.5}, angular_deceleration{2.5};
    bool allow_reverse{false};
};
// Pure C++ adaptation of Nav2 DWPP (Fumiya Ohnishi, Apache-2.0).
// All invalid/nonfinite inputs and empty windows throw invalid_argument.
DynamicWindowBounds computeDynamicWindow(Twist2D previous,const DynamicWindowLimits& limits,double dt);
// A requested speed below the reachable minimum collapses to the reachable
// deceleration boundary; regulation must never violate acceleration limits.
DynamicWindowBounds regulateDynamicWindow(DynamicWindowBounds window,double regulated_linear);
// Closest point to w=curvature*v; sign must be +1 or -1, and ties prefer faster
// motion in that direction.
Twist2D selectDynamicWindowVelocity(const DynamicWindowBounds& window,double curvature,double sign);
} // namespace rbt::navigation

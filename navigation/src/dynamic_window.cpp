// Copyright (c) 2025 Fumiya Ohnishi
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Adapted from Nav2 Regulated Pure Pursuit DWPP functions at commit
// cea9ed9d2e55b4e084be705a495a721368021f19. This implementation uses RBT's
// pure C++ domain types and has no ROS dependencies.

#include <rbt_navigation/controller/dynamic_window.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace rbt::navigation {
namespace {

constexpr double kVelocitySignEpsilon = 1e-3;
constexpr double kCurvatureEpsilon = 1e-3;
constexpr long double kCornerTieTolerance = 1e-3L;

void validateWindow(const DynamicWindowBounds& window) {
    if (!std::isfinite(window.min_linear) || !std::isfinite(window.max_linear) ||
        !std::isfinite(window.min_angular) || !std::isfinite(window.max_angular)) {
        throw std::invalid_argument("dynamic window bounds must be finite");
    }
    if (window.min_linear > window.max_linear || window.min_angular > window.max_angular) {
        throw std::invalid_argument("dynamic window must not be empty");
    }
}

std::pair<double, double> computeDimensionWindow(
    double current, double absolute_min, double absolute_max,
    double acceleration, double deceleration, double dt) {
    long double candidate_min{};
    long double candidate_max{};
    const long double velocity = current;
    const long double accel_delta = static_cast<long double>(acceleration) * dt;
    const long double decel_delta = static_cast<long double>(deceleration) * dt;

    if (current > kVelocitySignEpsilon) {
        candidate_min = velocity - decel_delta;
        candidate_max = velocity + accel_delta;
    } else if (current < -kVelocitySignEpsilon) {
        candidate_min = velocity - accel_delta;
        candidate_max = velocity + decel_delta;
    } else {
        candidate_min = velocity - accel_delta;
        candidate_max = velocity + accel_delta;
    }

    // Deceleration limits are public positive magnitudes. Applying them toward
    // zero gives the negative step above for positive motion and the positive
    // step above for negative motion.
    const long double clipped_min = std::max(candidate_min, static_cast<long double>(absolute_min));
    const long double clipped_max = std::min(candidate_max, static_cast<long double>(absolute_max));
    if (clipped_min > clipped_max) {
        throw std::invalid_argument("reachable velocities do not intersect absolute limits");
    }
    return {static_cast<double>(clipped_min), static_cast<double>(clipped_max)};
}

}  // namespace

DynamicWindowBounds computeDynamicWindow(
    Twist2D previous, const DynamicWindowLimits& limits, double dt) {
    if (!finite(previous)) {
        throw std::invalid_argument("previous command must be finite");
    }
    if (!std::isfinite(dt) || dt <= 0.0 || dt > 1.0) {
        throw std::invalid_argument("dt must be finite and in (0, 1]");
    }
    if (!std::isfinite(limits.max_linear) || !std::isfinite(limits.max_angular) ||
        !std::isfinite(limits.linear_acceleration) ||
        !std::isfinite(limits.linear_deceleration) ||
        !std::isfinite(limits.angular_acceleration) ||
        !std::isfinite(limits.angular_deceleration)) {
        throw std::invalid_argument("dynamic window limits must be finite");
    }
    if (limits.max_linear < 0.0 || limits.max_angular < 0.0 ||
        limits.linear_acceleration < 0.0 || limits.linear_deceleration < 0.0 ||
        limits.angular_acceleration < 0.0 || limits.angular_deceleration < 0.0) {
        throw std::invalid_argument("velocity and acceleration limits must be nonnegative");
    }
    if (!limits.allow_reverse && previous.linear_x < 0.0) {
        throw std::invalid_argument("negative previous linear command requires reverse mode");
    }

    const double absolute_min_linear = limits.allow_reverse ? -limits.max_linear : 0.0;
    const auto [min_linear, max_linear] = computeDimensionWindow(
        previous.linear_x, absolute_min_linear, limits.max_linear,
        limits.linear_acceleration, limits.linear_deceleration, dt);
    const auto [min_angular, max_angular] = computeDimensionWindow(
        previous.angular_z, -limits.max_angular, limits.max_angular,
        limits.angular_acceleration, limits.angular_deceleration, dt);

    return {min_linear, max_linear, min_angular, max_angular};
}

DynamicWindowBounds regulateDynamicWindow(DynamicWindowBounds window, double regulated_linear) {
    validateWindow(window);
    if (!std::isfinite(regulated_linear)) {
        throw std::invalid_argument("regulated linear velocity must be finite");
    }

    const double regulation_min = std::min(0.0, regulated_linear);
    const double regulation_max = std::max(0.0, regulated_linear);
    window.min_linear = std::max(window.min_linear, regulation_min);
    window.max_linear = std::min(window.max_linear, regulation_max);

    if (window.min_linear > window.max_linear) {
        // The requested speed is unreachable. Keep the closest edge of the
        // reachable window, which is its deceleration boundary.
        if (window.min_linear > regulation_max) {
            window.max_linear = window.min_linear;
        } else {
            window.min_linear = window.max_linear;
        }
    }
    return window;
}

Twist2D selectDynamicWindowVelocity(
    const DynamicWindowBounds& window, double curvature, double sign) {
    validateWindow(window);
    if (!std::isfinite(curvature) || (sign != 1.0 && sign != -1.0)) {
        throw std::invalid_argument("curvature must be finite and direction sign must be +1 or -1");
    }

    if (std::abs(curvature) < kCurvatureEpsilon) {
        const double linear = sign >= 0.0 ? window.max_linear : window.min_linear;
        double angular{};
        if (window.min_angular <= 0.0 && 0.0 <= window.max_angular) {
            angular = 0.0;
        } else if (std::abs(window.min_angular) <= std::abs(window.max_angular)) {
            angular = window.min_angular;
        } else {
            angular = window.max_angular;
        }
        return {linear, angular};
    }

    const long double k = curvature;
    const long double angular_at_min = static_cast<long double>(window.min_angular) / k;
    const long double angular_at_max = static_cast<long double>(window.max_angular) / k;
    const long double intersection_min = std::max(
        static_cast<long double>(window.min_linear),
        std::min(angular_at_min, angular_at_max));
    const long double intersection_max = std::min(
        static_cast<long double>(window.max_linear),
        std::max(angular_at_min, angular_at_max));

    if (intersection_min <= intersection_max) {
        const long double selected_linear = sign >= 0.0 ? intersection_max : intersection_min;
        const long double selected_angular = std::clamp(
            k * selected_linear,
            static_cast<long double>(window.min_angular),
            static_cast<long double>(window.max_angular));
        return {static_cast<double>(selected_linear), static_cast<double>(selected_angular)};
    }

    const std::array<Twist2D, 4> corners{{
        {window.min_linear, window.min_angular},
        {window.min_linear, window.max_angular},
        {window.max_linear, window.min_angular},
        {window.max_linear, window.max_angular},
    }};
    const long double denominator = std::hypot(k, 1.0L);
    const long double direction = sign >= 0.0 ? 1.0L : -1.0L;
    long double closest_distance = std::numeric_limits<long double>::infinity();
    Twist2D closest{};

    for (const Twist2D& corner : corners) {
        const long double residual =
            k * static_cast<long double>(corner.linear_x) - corner.angular_z;
        const long double distance = std::abs(residual) / denominator;
        if (distance < closest_distance ||
            (std::abs(distance - closest_distance) <= kCornerTieTolerance &&
             direction * corner.linear_x > direction * closest.linear_x)) {
            closest_distance = distance;
            closest = corner;
        }
    }
    return closest;
}

}  // namespace rbt::navigation

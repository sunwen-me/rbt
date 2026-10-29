// Copyright (c) 2020 Shrijit Singh
// Copyright (c) 2020 Samsung Research America
// Copyright (c) 2022 Samsung Research America
// Copyright (c) 2024 Black Coffee Robotics
// Copyright (c) 2026 RBT contributors
//
// This file adapts the Nav2 Regulated Pure Pursuit controller and regulation
// heuristics to the RBT navigation interfaces.
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

#include <rbt_navigation/controller/dynamic_window.hpp>
#include <rbt_navigation/controller/pure_pursuit.hpp>
#include <rbt_navigation/geometry/footprint.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace rbt::navigation {
namespace {

constexpr double kGeometryEpsilon = 1e-12;
constexpr double kNearZeroVelocity = 1e-3;
constexpr std::size_t kMaximumStoppingSamples = 100'000U;

struct Projection {
    double x{};
    double y{};
    double distance_squared{std::numeric_limits<double>::infinity()};
    double along_path{};
    double tangent_yaw{};
};

struct Point2D {
    double x{};
    double y{};
};

[[nodiscard]] bool finiteConfig(const ControllerConfig& config) noexcept {
    return std::isfinite(config.max_linear) && config.max_linear > 0.0
        && std::isfinite(config.max_angular) && config.max_angular > 0.0
        && std::isfinite(config.max_acceleration) && config.max_acceleration > 0.0
        && std::isfinite(config.max_angular_acceleration) && config.max_angular_acceleration > 0.0
        && std::isfinite(config.max_deceleration) && config.max_deceleration > 0.0
        && std::isfinite(config.max_angular_deceleration) && config.max_angular_deceleration > 0.0
        && std::isfinite(config.min_lookahead) && config.min_lookahead > 0.0
        && std::isfinite(config.max_lookahead) && config.max_lookahead >= config.min_lookahead
        && std::isfinite(config.lookahead_time) && config.lookahead_time > 0.0
        && std::isfinite(config.position_tolerance) && config.position_tolerance > 0.0
        && std::isfinite(config.yaw_tolerance) && config.yaw_tolerance > 0.0
        && std::isfinite(config.max_path_deviation) && config.max_path_deviation > 0.0
        && std::isfinite(config.regulated_min_radius) && config.regulated_min_radius > 0.0
        && std::isfinite(config.regulated_min_speed) && config.regulated_min_speed >= 0.0
        && config.regulated_min_speed <= config.max_linear
        && std::isfinite(config.cost_scaling_distance) && config.cost_scaling_distance > 0.0
        && std::isfinite(config.cost_scaling_gain) && config.cost_scaling_gain >= 0.0
        && std::isfinite(config.approach_scaling_distance) && config.approach_scaling_distance > 0.0
        && std::isfinite(config.min_approach_speed) && config.min_approach_speed >= 0.0
        && config.min_approach_speed <= config.max_linear
        && std::isfinite(config.rotate_to_heading_angle) && config.rotate_to_heading_angle > 0.0
        && config.rotate_to_heading_angle <= std::numbers::pi
        && std::isfinite(config.rotate_to_heading_speed) && config.rotate_to_heading_speed > 0.0
        && config.rotate_to_heading_speed <= config.max_angular
        && std::isfinite(config.collision_horizon) && config.collision_horizon > 0.0
        && std::isfinite(config.reaction_time) && config.reaction_time > 0.0
        && std::isfinite(config.curvature_lookahead) && config.curvature_lookahead > 0.0
        && std::isfinite(config.minimum_path_rotation) && config.minimum_path_rotation >= 0.0
        && config.minimum_path_rotation <= std::numbers::pi
        && (config.geometry == PursuitGeometry::PurePursuit
            || config.geometry == PursuitGeometry::VectorPursuit)
        && std::isfinite(config.vector_pursuit_k) && config.vector_pursuit_k > 0.0
        && std::isfinite(config.vector_pursuit_min_radius)
        && config.vector_pursuit_min_radius > 0.0
        && std::isfinite(1.0 / config.vector_pursuit_min_radius);
}

[[nodiscard]] bool validInputs(const RobotState& state,
                               const Path2D& path,
                               const ControllerContext& context,
                               const ControllerConfig& config) noexcept {
    if (!finiteConfig(config) || !finite(state.pose) || !finite(state.velocity)
        || !finite(context.previous_command) || !std::isfinite(context.dt) || context.dt <= 0.0
        || context.dt > 1.0 || !std::isfinite(context.robot_radius) || context.robot_radius < 0.0
        || context.now_ns < 0 || context.max_input_age_ns <= 0 || context.max_map_age_ns <= 0
        || std::abs(context.previous_command.linear_x) > config.max_linear
        || std::abs(context.previous_command.angular_z) > config.max_angular
        || !std::isfinite(config.max_acceleration * context.dt)
        || !std::isfinite(config.max_deceleration * context.dt)
        || !std::isfinite(config.max_angular_acceleration * context.dt)
        || !std::isfinite(config.max_angular_deceleration * context.dt)) {
        return false;
    }
    if (!config.allow_reverse && context.previous_command.linear_x < 0.0) {
        return false;
    }

    double accumulated_length = 0.0;
    for (std::size_t index = 0; index < path.poses.size(); ++index) {
        const auto& pose = path.poses[index];
        if (!finite(pose)) {
            return false;
        }
        const double relative_x = pose.x - state.pose.x;
        const double relative_y = pose.y - state.pose.y;
        if (!std::isfinite(relative_x) || !std::isfinite(relative_y)
            || !std::isfinite(relative_x * relative_x + relative_y * relative_y)) {
            return false;
        }
        if (index + 1U < path.poses.size()) {
            const double dx = path.poses[index + 1U].x - pose.x;
            const double dy = path.poses[index + 1U].y - pose.y;
            const double segment_squared = dx * dx + dy * dy;
            const double segment_length = std::hypot(dx, dy);
            if (!std::isfinite(dx) || !std::isfinite(dy) || !std::isfinite(segment_squared)
                || !std::isfinite(segment_length)
                || !std::isfinite(accumulated_length + segment_length)) {
                return false;
            }
            accumulated_length += segment_length;
        }
    }
    return true;
}

[[nodiscard]] Projection nearestProjection(const Pose2D& robot,
                                           const Path2D& path,
                                           double& total_length) noexcept {
    Projection best;
    total_length = 0.0;
    if (path.poses.size() == 1U) {
        const auto& point = path.poses.front();
        best.x = point.x;
        best.y = point.y;
        best.distance_squared = (robot.x - point.x) * (robot.x - point.x)
            + (robot.y - point.y) * (robot.y - point.y);
        best.tangent_yaw = point.yaw;
        return best;
    }

    double accumulated = 0.0;
    for (std::size_t index = 0; index + 1U < path.poses.size(); ++index) {
        const auto& start = path.poses[index];
        const auto& end = path.poses[index + 1U];
        const double dx = end.x - start.x;
        const double dy = end.y - start.y;
        const double length_squared = dx * dx + dy * dy;
        const double length = std::sqrt(length_squared);
        if (length <= kGeometryEpsilon) {
            continue;
        }
        const double fraction = std::clamp(((robot.x - start.x) * dx + (robot.y - start.y) * dy)
                                               / length_squared,
                                           0.0,
                                           1.0);
        const double x = start.x + fraction * dx;
        const double y = start.y + fraction * dy;
        const double error_x = robot.x - x;
        const double error_y = robot.y - y;
        const double distance_squared = error_x * error_x + error_y * error_y;
        if (distance_squared < best.distance_squared) {
            best.x = x;
            best.y = y;
            best.distance_squared = distance_squared;
            best.along_path = accumulated + fraction * length;
            best.tangent_yaw = std::atan2(dy, dx);
        }
        accumulated += length;
    }
    total_length = accumulated;
    if (!std::isfinite(best.distance_squared)) {
        const auto& point = path.poses.back();
        best.x = point.x;
        best.y = point.y;
        best.distance_squared = (robot.x - point.x) * (robot.x - point.x)
            + (robot.y - point.y) * (robot.y - point.y);
        best.tangent_yaw = point.yaw;
    }
    return best;
}

// Nav2 getLookAheadPoint semantics: measure cumulative path distance from the
// nearest projection, interpolate within the segment containing that distance,
// and return the last pose when the requested lookahead extends beyond goal.
[[nodiscard]] Point2D pointAtDistance(const Path2D& path, double along_path) noexcept {
    if (path.poses.size() == 1U || along_path <= 0.0) {
        return {path.poses.front().x, path.poses.front().y};
    }
    double accumulated = 0.0;
    for (std::size_t index = 0; index + 1U < path.poses.size(); ++index) {
        const auto& start = path.poses[index];
        const auto& end = path.poses[index + 1U];
        const double dx = end.x - start.x;
        const double dy = end.y - start.y;
        const double length = std::hypot(dx, dy);
        if (length <= kGeometryEpsilon) {
            continue;
        }
        if (along_path <= accumulated + length) {
            const double fraction = std::clamp((along_path - accumulated) / length, 0.0, 1.0);
            return {start.x + fraction * dx, start.y + fraction * dy};
        }
        accumulated += length;
    }
    const auto& goal = path.poses.back();
    return {goal.x, goal.y};
}

[[nodiscard]] Pose2D poseAtDistance(const Path2D& path,
                                    double along_path,
                                    bool use_heading_from_path = true) noexcept {
    if (path.poses.size() == 1U) {
        return path.poses.front();
    }
    if (along_path <= 0.0) {
        if (use_heading_from_path) {
            return path.poses.front();
        }
        for (std::size_t index = 0; index + 1U < path.poses.size(); ++index) {
            const auto& start = path.poses[index];
            const auto& end = path.poses[index + 1U];
            const double dx = end.x - start.x;
            const double dy = end.y - start.y;
            if (std::hypot(dx, dy) > kGeometryEpsilon) {
                return {path.poses.front().x, path.poses.front().y, std::atan2(dy, dx)};
            }
        }
        return path.poses.front();
    }
    double accumulated = 0.0;
    for (std::size_t index = 0; index + 1U < path.poses.size(); ++index) {
        const auto& start = path.poses[index];
        const auto& end = path.poses[index + 1U];
        const double dx = end.x - start.x;
        const double dy = end.y - start.y;
        const double length = std::hypot(dx, dy);
        if (length <= kGeometryEpsilon) {
            continue;
        }
        if (along_path <= accumulated + length) {
            const double fraction = std::clamp((along_path - accumulated) / length, 0.0, 1.0);
            double yaw = 0.0;
            if (use_heading_from_path) {
                const double yaw_delta = normalizeAngle(end.yaw - start.yaw);
                yaw = normalizeAngle(start.yaw + fraction * yaw_delta);
            } else {
                yaw = std::atan2(dy, dx);
            }
            return {start.x + fraction * dx,
                    start.y + fraction * dy,
                    yaw};
        }
        accumulated += length;
    }
    if (!use_heading_from_path) {
        for (std::size_t index = path.poses.size() - 1U; index > 0U; --index) {
            const auto& start = path.poses[index - 1U];
            const auto& end = path.poses[index];
            const double dx = end.x - start.x;
            const double dy = end.y - start.y;
            if (std::hypot(dx, dy) > kGeometryEpsilon) {
                return {path.poses.back().x, path.poses.back().y, std::atan2(dy, dx)};
            }
        }
    }
    return path.poses.back();
}

[[nodiscard]] Point2D toBaseLink(const Pose2D& pose, Point2D map_point) noexcept {
    const double dx = map_point.x - pose.x;
    const double dy = map_point.y - pose.y;
    const double cosine = std::cos(pose.yaw);
    const double sine = std::sin(pose.yaw);
    return {cosine * dx + sine * dy, -sine * dx + cosine * dy};
}

[[nodiscard]] Pose2D targetPoseInDriveAxleFrame(const Pose2D& robot_pose,
                                                const Pose2D& target_pose) noexcept {
    const Point2D position = toBaseLink(robot_pose, {target_pose.x, target_pose.y});
    return {position.x, position.y, normalizeAngle(target_pose.yaw - robot_pose.yaw)};
}

[[nodiscard]] double calculateCurvature(Point2D lookahead_point) noexcept {
    const double distance_squared = lookahead_point.x * lookahead_point.x
        + lookahead_point.y * lookahead_point.y;
    // Match Nav2's degeneracy threshold (carrot distance squared > 0.001).
    return distance_squared > 0.001 ? 2.0 * lookahead_point.y / distance_squared : 0.0;
}

// Adapted from blackcoffeerobotics/vector_pursuit_controller.cpp at commit
// 0a8b8ee32b2ffcc25826be61fd289d2a0e72efbc. Compute curvature directly so a
// singular denominator represents an infinite radius (zero curvature) without
// producing a non-finite command.
[[nodiscard]] double calculateVectorPursuitCurvature(const Pose2D& target,
                                                      bool allow_reverse,
                                                      const ControllerConfig& config) noexcept {
    if (!finite(target)) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    const double target_angle = normalizeAngle(target.yaw);
    if (!allow_reverse && target.x < 0.0) {
        return std::copysign(1.0 / config.vector_pursuit_min_radius, target.y);
    }
    if (std::abs(target.y) <= 1e-6) {
        return 0.0;
    }

    const long double x = target.x;
    const long double y = target.y;
    const long double distance_squared = x * x + y * y;
    const long double phi1 = std::atan2(2.0L * y * y - distance_squared, 2.0L * x * y);
    const long double term2 = distance_squared / (2.0L * y);
    const long double phi2 = std::atan2(term2, 0.0L);
    constexpr long double two_pi = 2.0L * std::numbers::pi_v<long double>;
    long double phi = std::fmod(phi1 - phi2, two_pi);
    if (phi < 0.0L) {
        phi += two_pi;
    }
    phi = std::max(phi, 1e-9L);

    const long double k = config.vector_pursuit_k;
    const long double numerator = k * phi;
    const long double denominator = (k - 1.0L) * phi + target_angle;
    if (!std::isfinite(distance_squared) || !std::isfinite(term2)
        || !std::isfinite(phi) || !std::isfinite(numerator)
        || !std::isfinite(denominator)) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    if (denominator == 0.0L) {
        return 0.0;
    }

    const long double radius = std::abs((numerator / denominator) * term2);
    const long double bounded_radius = std::max(radius,
                                                static_cast<long double>(config.vector_pursuit_min_radius));
    if (std::isnan(bounded_radius) || bounded_radius <= 0.0L) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const long double curvature = std::copysign(1.0L / bounded_radius, y);
    const double result = static_cast<double>(curvature);
    return std::isfinite(result) ? result : std::numeric_limits<double>::quiet_NaN();
}

[[nodiscard]] double curvatureSpeed(double raw_speed, double curvature, double minimum_radius) noexcept {
    if (std::abs(curvature) <= kGeometryEpsilon) {
        return raw_speed;
    }
    const double radius = std::abs(1.0 / curvature);
    if (radius >= minimum_radius) {
        return raw_speed;
    }
    return raw_speed * std::clamp(radius / minimum_radius, 0.0, 1.0);
}

[[nodiscard]] double costSpeed(double raw_speed,
                               double clearance,
                               const ControllerConfig& config) noexcept {
    if (clearance >= config.cost_scaling_distance) {
        return raw_speed;
    }
    const double scale = std::clamp(config.cost_scaling_gain * clearance / config.cost_scaling_distance,
                                    0.0,
                                    1.0);
    return raw_speed * scale;
}

[[nodiscard]] double approachSpeed(double raw_speed,
                                   double remaining_path_distance,
                                   double goal_distance,
                                   const ControllerConfig& config) noexcept {
    if (remaining_path_distance >= config.approach_scaling_distance) {
        return raw_speed;
    }
    const double scale = std::clamp(goal_distance / config.approach_scaling_distance, 0.0, 1.0);
    const double scaled_speed = std::max(config.min_approach_speed, raw_speed * scale);
    return std::min(raw_speed, scaled_speed);
}

[[nodiscard]] double moveToward(double current, double target, double max_delta) noexcept {
    const double delta = std::clamp(target - current, -max_delta, max_delta);
    return current + delta;
}

[[nodiscard]] Twist2D applyLimits(const Twist2D& desired,
                                 const Twist2D& previous,
                                 const ControllerConfig& config,
                                 double dt) noexcept {
    const auto axisLimit = [dt](double target,
                                double current,
                                double acceleration,
                                double deceleration,
                                double maximum) {
        const bool same_direction = target * current >= 0.0;
        const bool increasing_speed = same_direction && std::abs(target) > std::abs(current);
        const double delta = (increasing_speed ? acceleration : deceleration) * dt;
        return std::clamp(moveToward(current, target, delta), -maximum, maximum);
    };
    return {
        axisLimit(desired.linear_x, previous.linear_x,
                  config.max_acceleration, config.max_deceleration, config.max_linear),
        axisLimit(desired.angular_z, previous.angular_z,
                  config.max_angular_acceleration, config.max_angular_deceleration, config.max_angular),
    };
}

[[nodiscard]] Pose2D integrateArc(const Pose2D& start,
                                  const Twist2D& velocity,
                                  double signed_distance) noexcept {
    Pose2D predicted = start;
    const double speed = velocity.linear_x;
    if (std::abs(speed) <= kGeometryEpsilon) {
        return predicted;
    }
    const double curvature = velocity.angular_z / speed;
    const double yaw_delta = curvature * signed_distance;
    if (std::abs(curvature) <= kGeometryEpsilon) {
        predicted.x += signed_distance * std::cos(start.yaw);
        predicted.y += signed_distance * std::sin(start.yaw);
    } else {
        const double next_yaw = start.yaw + yaw_delta;
        predicted.x += (std::sin(next_yaw) - std::sin(start.yaw)) / curvature;
        predicted.y -= (std::cos(next_yaw) - std::cos(start.yaw)) / curvature;
        predicted.yaw = normalizeAngle(next_yaw);
    }
    return predicted;
}

[[nodiscard]] Pose2D integrateCommand(const Pose2D& start,
                                      const Twist2D& velocity,
                                      double duration) noexcept {
    Pose2D predicted = start;
    if (std::abs(velocity.angular_z) <= kGeometryEpsilon) {
        predicted.x += velocity.linear_x * duration * std::cos(start.yaw);
        predicted.y += velocity.linear_x * duration * std::sin(start.yaw);
        return predicted;
    }

    const double next_yaw = start.yaw + velocity.angular_z * duration;
    predicted.x += velocity.linear_x / velocity.angular_z
        * (std::sin(next_yaw) - std::sin(start.yaw));
    predicted.y -= velocity.linear_x / velocity.angular_z
        * (std::cos(next_yaw) - std::cos(start.yaw));
    predicted.yaw = normalizeAngle(next_yaw);
    return predicted;
}

[[nodiscard]] bool arcIsFree(const Pose2D& start,
                            const Twist2D& velocity,
                            double distance_to_check,
                            const ControllerContext& context) noexcept {
    const auto& map = *context.map.grid;
    const double spacing = map.resolution() * 0.25;
    if (!std::isfinite(distance_to_check) || distance_to_check < 0.0
        || !std::isfinite(spacing) || spacing <= 0.0
        || !map.isFree(start.x, start.y, context.robot_radius)) {
        return false;
    }
    if (distance_to_check <= kGeometryEpsilon) {
        return true;
    }
    const double raw_steps = std::ceil(distance_to_check / spacing);
    if (!std::isfinite(raw_steps) || raw_steps > 100000.0) {
        return false;
    }
    const auto steps = static_cast<std::size_t>(raw_steps);
    const double direction = velocity.linear_x < 0.0 ? -1.0 : 1.0;
    Pose2D predicted = start;
    for (std::size_t index = 1U; index <= steps; ++index) {
        const double traveled = std::min(distance_to_check, static_cast<double>(index) * spacing);
        predicted = integrateArc(start, velocity, direction * traveled);
        if (!finite(predicted) || !map.isFree(predicted.x, predicted.y, context.robot_radius)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool commandedArcIsFree(const Pose2D& start,
                                      const Twist2D& velocity,
                                      double carrot_distance,
                                      const ControllerConfig& config,
                                      const ControllerContext& context) noexcept {
    const auto& map = *context.map.grid;
    const double spacing = map.resolution() * 0.25;
    if (!std::isfinite(carrot_distance) || carrot_distance < 0.0
        || !std::isfinite(spacing) || spacing <= 0.0
        || !map.isFree(start.x, start.y, context.robot_radius)) {
        return false;
    }
    const double speed = std::abs(velocity.linear_x);
    if (speed <= kGeometryEpsilon || carrot_distance <= kGeometryEpsilon) {
        return true;
    }
    const double distance_to_check = speed * config.collision_horizon;
    if (!std::isfinite(distance_to_check)) {
        return false;
    }
    const double raw_steps = std::ceil(distance_to_check / spacing);
    if (!std::isfinite(raw_steps) || raw_steps > 100000.0) {
        return false;
    }

    const double direction = velocity.linear_x < 0.0 ? -1.0 : 1.0;
    double traveled = 0.0;
    const auto radialDistance = [&start](const Pose2D& pose) {
        return std::hypot(pose.x - start.x, pose.y - start.y);
    };
    const auto isFreeAt = [&](double signed_distance) {
        const Pose2D sample = integrateArc(start, velocity, signed_distance);
        return finite(sample) && map.isFree(sample.x, sample.y, context.robot_radius);
    };

    const auto steps = static_cast<std::size_t>(raw_steps);
    for (std::size_t index = 0U; index < steps; ++index) {
        const double step_distance = std::min(spacing, distance_to_check - traveled);
        const double next_traveled = traveled + step_distance;
        const Pose2D next = integrateArc(start, velocity, direction * next_traveled);
        if (!finite(next)) {
            return false;
        }
        if (radialDistance(next) > carrot_distance) {
            // Nav2 limits collision checking by Euclidean carrot displacement,
            // not by the travelled arc length. Find and check the last point
            // still inside that radial horizon before stopping the prediction.
            double low = traveled;
            double high = next_traveled;
            for (int iteration = 0; iteration < 32; ++iteration) {
                const double middle = (low + high) * 0.5;
                const Pose2D sample = integrateArc(start, velocity, direction * middle);
                if (radialDistance(sample) <= carrot_distance) {
                    low = middle;
                } else {
                    high = middle;
                }
            }
            return isFreeAt(direction * low);
        }
        if (!map.isFree(next.x, next.y, context.robot_radius)) {
            return false;
        }
        traveled = next_traveled;
    }
    return true;
}

[[nodiscard]] bool commandMotionIsFree(const Pose2D& start,
                                       const Twist2D& velocity,
                                       double carrot_distance,
                                       const ControllerConfig& config,
                                       const ControllerContext& context) {
    if (context.footprint) {
        return context.footprint->commandFree(*context.map.grid, start, velocity,
                                              config.collision_horizon, carrot_distance);
    }
    return commandedArcIsFree(start, velocity, carrot_distance, config, context);
}

[[nodiscard]] bool measuredStoppingArcIsFree(const Pose2D& start,
                                             const Twist2D& velocity,
                                             const ControllerConfig& config,
                                             const ControllerContext& context) noexcept {
    const double speed = std::abs(velocity.linear_x);
    const double reaction_distance = speed * std::max(config.reaction_time, context.dt);
    const double braking_distance = speed * speed / (2.0 * config.max_deceleration);
    const double stopping_distance = reaction_distance + braking_distance;
    return arcIsFree(start, velocity, stopping_distance, context);
}

[[nodiscard]] bool measuredStoppingMotionIsFree(const Pose2D& start,
                                                const Twist2D& velocity,
                                                const ControllerConfig& config,
                                                const ControllerContext& context) {
    if (!context.footprint) {
        return measuredStoppingArcIsFree(start, velocity, config, context);
    }

    const auto& map = *context.map.grid;
    const auto& checker = *context.footprint;
    if (!checker.poseFree(map, start)) return false;

    Pose2D predicted = start;
    const double reaction_time = std::max(config.reaction_time, context.dt);
    if ((std::abs(velocity.linear_x) > kGeometryEpsilon ||
         std::abs(velocity.angular_z) > kGeometryEpsilon) &&
        !checker.commandFree(map, predicted, velocity, reaction_time)) {
        return false;
    }
    predicted = integrateCommand(predicted, velocity, reaction_time);
    if (!finite(predicted)) return false;

    Twist2D braking_velocity = velocity;
    std::size_t sample_count = 0U;
    while (std::abs(braking_velocity.linear_x) > kGeometryEpsilon ||
           std::abs(braking_velocity.angular_z) > kGeometryEpsilon) {
        if (++sample_count > kMaximumStoppingSamples) return false;

        double stop_time = 0.0;
        if (std::abs(braking_velocity.linear_x) > kGeometryEpsilon) {
            stop_time = std::max(stop_time,
                std::abs(braking_velocity.linear_x) / config.max_deceleration);
        }
        if (std::abs(braking_velocity.angular_z) > kGeometryEpsilon) {
            stop_time = std::max(stop_time,
                std::abs(braking_velocity.angular_z) / config.max_angular_deceleration);
        }
        const double step_time = std::min({context.dt, 0.02, stop_time});
        if (!std::isfinite(step_time) || step_time <= 0.0) return false;

        const Twist2D next_velocity{
            moveToward(braking_velocity.linear_x, 0.0,
                       config.max_deceleration * step_time),
            moveToward(braking_velocity.angular_z, 0.0,
                       config.max_angular_deceleration * step_time),
        };
        const Twist2D average_velocity{
            0.5 * (braking_velocity.linear_x + next_velocity.linear_x),
            0.5 * (braking_velocity.angular_z + next_velocity.angular_z),
        };
        const Pose2D next_pose = integrateCommand(predicted, average_velocity, step_time);
        if (!finite(next_pose) || !checker.motionFree(map, predicted, next_pose)) return false;
        predicted = next_pose;
        braking_velocity = next_velocity;
    }
    return true;
}

[[nodiscard]] ControllerResult failure(ErrorCode error) noexcept {
    return ControllerResult{Twist2D{}, error, false};
}

[[nodiscard]] Twist2D dynamicWindowCommand(const Twist2D& previous,
                                           double regulated_linear,
                                           double curvature,
                                           double sign,
                                           const ControllerConfig& config,
                                           double dt) {
    const DynamicWindowLimits limits{
        config.max_linear,
        config.max_angular,
        config.max_acceleration,
        config.max_deceleration,
        config.max_angular_acceleration,
        config.max_angular_deceleration,
        config.allow_reverse,
    };
    DynamicWindowBounds window = computeDynamicWindow(previous, limits, dt);
    window = regulateDynamicWindow(window, regulated_linear);
    return selectDynamicWindowVelocity(window, curvature, sign);
}

} // namespace

RegulatedPurePursuit::RegulatedPurePursuit(ControllerConfig config)
    : config_(config),
      feasible_path_handler_(config.enforce_path_inversion,
                             config.enforce_path_rotation,
                             config.minimum_path_rotation) {
    if (!finiteConfig(config_)) {
        throw std::invalid_argument("regulated pure pursuit controller configuration is invalid");
    }
}

ControllerResult RegulatedPurePursuit::computeCommand(const RobotState& state,
                                                       const Path2D& path,
                                                       const ControllerContext& context) {
    if (!validInputs(state, path, context, config_)) {
        return failure(ErrorCode::InvalidInput);
    }
    if (path.poses.empty()) {
        return failure(ErrorCode::EmptyPath);
    }
    if (!state.localized) {
        return failure(ErrorCode::LocalizationUnavailable);
    }
    if (!fresh(state.tf_stamp_ns, context.now_ns, context.max_input_age_ns)) {
        return failure(ErrorCode::StaleTf);
    }
    if (!fresh(state.odom_stamp_ns, context.now_ns, context.max_input_age_ns)) {
        return failure(ErrorCode::StaleOdometry);
    }
    if (!context.map.valid() || !context.map.grid) {
        return failure(ErrorCode::MapUnavailable);
    }
    if (!fresh(context.map.stamp_ns, context.now_ns, context.max_map_age_ns)
        || context.map.version != path.map_version) {
        return failure(ErrorCode::StaleMap);
    }
    const double map_resolution = context.map.grid->resolution();
    if (!std::isfinite(map_resolution) || map_resolution <= 0.0
        || context.map.grid->width() == 0U || context.map.grid->height() == 0U) {
        return failure(ErrorCode::MapUnavailable);
    }
    const bool current_pose_is_free = context.footprint
        ? context.footprint->poseFree(*context.map.grid, state.pose)
        : context.map.grid->isFree(state.pose.x, state.pose.y, context.robot_radius);
    if (!current_pose_is_free) {
        return failure(ErrorCode::Collision);
    }

    const Path2D* tracking_path = &path;
    bool final_segment = true;
    FeasiblePathSegment feasible_segment;
    if (config_.use_feasible_path_handler) {
        feasible_segment = feasible_path_handler_.update(
            path, state.pose, context.map.version, context.map.grid.get());
        tracking_path = &feasible_segment.path;
        final_segment = feasible_segment.final_segment;
        if (tracking_path->poses.empty()) {
            return failure(ErrorCode::EmptyPath);
        }
    }

    double total_length = 0.0;
    const Projection nearest = nearestProjection(state.pose, *tracking_path, total_length);
    const double deviation = std::sqrt(nearest.distance_squared);
    if (!std::isfinite(deviation) || deviation > config_.max_path_deviation) {
        return failure(ErrorCode::PathDeviation);
    }

    const Pose2D& goal = tracking_path->poses.back();
    const double goal_distance = std::hypot(goal.x - state.pose.x, goal.y - state.pose.y);
    const double goal_yaw_error = normalizeAngle(goal.yaw - state.pose.yaw);
    if (!std::isfinite(goal_distance) || !std::isfinite(goal_yaw_error)) {
        return failure(ErrorCode::InvalidInput);
    }
    if (goal_distance <= config_.position_tolerance
        && std::abs(goal_yaw_error) <= config_.yaw_tolerance
        && std::abs(state.velocity.linear_x) <= kNearZeroVelocity
        && std::abs(state.velocity.angular_z) <= kNearZeroVelocity
        && std::abs(context.previous_command.linear_x) <= kNearZeroVelocity
        && std::abs(context.previous_command.angular_z) <= kNearZeroVelocity) {
        if (!final_segment) {
            feasible_path_handler_.advance();
            return ControllerResult{Twist2D{}, ErrorCode::None, false};
        }
        return ControllerResult{Twist2D{}, ErrorCode::None, true};
    }

    if (!measuredStoppingMotionIsFree(state.pose, state.velocity, config_, context)) {
        return failure(ErrorCode::Collision);
    }

    const double remaining_path_distance = total_length > kGeometryEpsilon
        ? std::max(0.0, total_length - nearest.along_path)
        : 0.0;
    const double lookahead_distance = std::clamp(std::abs(state.velocity.linear_x) * config_.lookahead_time,
                                                 config_.min_lookahead,
                                                 config_.max_lookahead);
    const Point2D carrot_map = pointAtDistance(*tracking_path, nearest.along_path + lookahead_distance);
    const Point2D carrot = toBaseLink(state.pose, carrot_map);
    Pose2D vector_carrot{};
    if (config_.geometry == PursuitGeometry::VectorPursuit) {
        vector_carrot = targetPoseInDriveAxleFrame(
            state.pose, poseAtDistance(*tracking_path,
                                       nearest.along_path + lookahead_distance,
                                       config_.vector_pursuit_use_heading_from_path));
    }
    const double carrot_distance = std::hypot(carrot.x, carrot.y);
    if (!std::isfinite(carrot.x) || !std::isfinite(carrot.y) || !std::isfinite(carrot_distance)) {
        return failure(ErrorCode::InvalidInput);
    }
    if (config_.geometry == PursuitGeometry::VectorPursuit && !finite(vector_carrot)) {
        return failure(ErrorCode::InvalidInput);
    }

    const double lookahead_curvature = config_.geometry == PursuitGeometry::VectorPursuit
        ? calculateVectorPursuitCurvature(vector_carrot, config_.allow_reverse, config_)
        : calculateCurvature(carrot);
    double regulation_curvature = lookahead_curvature;
    Point2D rotation_carrot = carrot;
    Pose2D vector_rotation_carrot = vector_carrot;
    if (config_.use_fixed_curvature_lookahead) {
        const double curvature_distance = nearest.along_path + config_.curvature_lookahead;
        const Point2D curvature_map = pointAtDistance(*tracking_path, curvature_distance);
        rotation_carrot = toBaseLink(state.pose, curvature_map);
        if (!std::isfinite(rotation_carrot.x) || !std::isfinite(rotation_carrot.y)) {
            return failure(ErrorCode::InvalidInput);
        }
        if (config_.geometry == PursuitGeometry::VectorPursuit) {
            vector_rotation_carrot = targetPoseInDriveAxleFrame(
                state.pose, poseAtDistance(*tracking_path, curvature_distance,
                                           config_.vector_pursuit_use_heading_from_path));
            if (!finite(vector_rotation_carrot)) {
                return failure(ErrorCode::InvalidInput);
            }
        }
        regulation_curvature = config_.geometry == PursuitGeometry::VectorPursuit
            ? calculateVectorPursuitCurvature(vector_rotation_carrot, config_.allow_reverse, config_)
            : calculateCurvature(rotation_carrot);
    }

    double direction = 1.0;
    if (config_.allow_reverse && carrot.x < 0.0) {
        direction = -1.0;
    }
    double angle_to_path = std::atan2(rotation_carrot.y, rotation_carrot.x);
    if (direction < 0.0) {
        angle_to_path = normalizeAngle(angle_to_path + std::numbers::pi);
    }

    if (goal_distance <= config_.position_tolerance) {
        Twist2D desired{};
        const bool still_translating = std::abs(state.velocity.linear_x) > kNearZeroVelocity
            || std::abs(context.previous_command.linear_x) > kNearZeroVelocity;
        if (!still_translating && std::abs(goal_yaw_error) > config_.yaw_tolerance) {
            const double stopping_speed = std::sqrt(2.0 * config_.max_angular_deceleration
                                                    * std::abs(goal_yaw_error));
            const double magnitude = std::min(config_.rotate_to_heading_speed, stopping_speed);
            desired.angular_z = std::copysign(magnitude, goal_yaw_error);
        }
        const Twist2D command = applyLimits(desired, context.previous_command, config_, context.dt);
        if (!finite(command)
            || !commandMotionIsFree(state.pose, command, carrot_distance, config_, context)) {
            return failure(ErrorCode::Collision);
        }
        return ControllerResult{command, ErrorCode::None, false};
    }

    if (std::abs(angle_to_path) > config_.rotate_to_heading_angle) {
        const double stopping_speed = std::sqrt(2.0 * config_.max_angular_deceleration
                                                * std::abs(angle_to_path));
        const double magnitude = std::min(config_.rotate_to_heading_speed, stopping_speed);
        const Twist2D desired{0.0, std::copysign(magnitude, angle_to_path)};
        const Twist2D command = applyLimits(desired, context.previous_command, config_, context.dt);
        if (!finite(command)
            || !commandMotionIsFree(state.pose, command, carrot_distance, config_, context)) {
            return failure(ErrorCode::Collision);
        }
        return ControllerResult{command, ErrorCode::None, false};
    }

    const double clearance = context.map.grid->getDistanceToObstacle(state.pose.x, state.pose.y);
    if (!std::isfinite(clearance) || clearance < 0.0) {
        return failure(ErrorCode::MapUnavailable);
    }
    double regulated_speed = std::min(curvatureSpeed(config_.max_linear,
                                                     regulation_curvature,
                                                     config_.regulated_min_radius),
                                      costSpeed(config_.max_linear, clearance, config_));
    regulated_speed = std::max(regulated_speed, config_.regulated_min_speed);
    regulated_speed = approachSpeed(regulated_speed,
                                    remaining_path_distance,
                                    goal_distance,
                                    config_);
    const double braking_distance = std::max(0.0, goal_distance - 0.5 * config_.position_tolerance);
    const double braking_speed = std::sqrt(2.0 * config_.max_deceleration * braking_distance);
    regulated_speed = std::min(regulated_speed, braking_speed);
    const double regulated_linear = direction * regulated_speed;
    if (!std::isfinite(regulated_linear) || !std::isfinite(lookahead_curvature)
        || !std::isfinite(regulation_curvature)) {
        return failure(ErrorCode::InvalidInput);
    }

    Twist2D command{};
    if (config_.use_dynamic_window) {
        command = dynamicWindowCommand(context.previous_command,
                                       regulated_linear,
                                       regulation_curvature,
                                       direction,
                                       config_,
                                       context.dt);
    } else {
        const Twist2D desired{regulated_linear, regulated_linear * regulation_curvature};
        command = applyLimits(desired, context.previous_command, config_, context.dt);
    }
    if (!finite(command)
        || !commandMotionIsFree(state.pose, command, carrot_distance, config_, context)) {
        return failure(ErrorCode::Collision);
    }
    return ControllerResult{command, ErrorCode::None, false};
}

} // namespace rbt::navigation

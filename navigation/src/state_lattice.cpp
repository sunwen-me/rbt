// Copyright (c) 2021, Samsung Research America
// Copyright (c) 2020, Samsung Research America
// Copyright (c) 2020, Applied Electric Vehicles Pty Ltd
// Copyright (c) 2026, RBT contributors
//
// The search structure is a native C++ adaptation of the Nav2 Smac Lattice
// planner's NodeLattice expansion model. See licenses/nav2-lattice/README.md.
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

#include <rbt_navigation/planner/state_lattice.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numbers>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace rbt::navigation {
namespace {

constexpr double kEndpointTolerance = 1e-5;
constexpr double kAngleTolerance = 1e-8;

struct LatticeKey {
    std::int64_t x{};
    std::int64_t y{};
    std::uint32_t heading{};
    std::int8_t turn{};
    bool reverse{};

    friend bool operator==(const LatticeKey&, const LatticeKey&) = default;
};

struct LatticeKeyHash {
    std::size_t operator()(const LatticeKey& key) const noexcept {
        std::size_t value = std::hash<std::int64_t>{}(key.x);
        const auto mix = [&value](std::size_t part) {
            value ^= part + static_cast<std::size_t>(0x9e3779b9U) +
                     (value << 6U) + (value >> 2U);
        };
        mix(std::hash<std::int64_t>{}(key.y));
        mix(std::hash<std::uint32_t>{}(key.heading));
        mix(std::hash<std::int8_t>{}(key.turn));
        mix(std::hash<bool>{}(key.reverse));
        return value;
    }
};

struct Node {
    LatticeKey key;
    double cost{};
    std::size_t parent{std::numeric_limits<std::size_t>::max()};
    std::size_t primitive{std::numeric_limits<std::size_t>::max()};
    bool reverse{};
    double seed_yaw{};
};

struct OpenEntry {
    std::size_t node{};
    double cost{};
    double estimate{};
    double heuristic{};
};

struct OpenEntryLater {
    bool operator()(const OpenEntry& a, const OpenEntry& b) const noexcept {
        if (a.estimate == b.estimate) return a.heuristic > b.heuristic;
        return a.estimate > b.estimate;
    }
};

struct PrimitiveProfile {
    double angular_change{};
    bool curved{};
    std::int8_t turn{1};
};

[[nodiscard]] double angleDistance(double a, double b) noexcept {
    return std::abs(normalizeAngle(normalizeAngle(a) - normalizeAngle(b)));
}

[[nodiscard]] PrimitiveProfile profileOf(const LatticePrimitive& primitive) noexcept {
    PrimitiveProfile profile;
    if (primitive.poses.empty()) return profile;
    for (std::size_t i = 1; i < primitive.poses.size(); ++i) {
        profile.angular_change += normalizeAngle(
            normalizeAngle(primitive.poses[i].yaw) -
            normalizeAngle(primitive.poses[i - 1U].yaw));
    }
    profile.curved = std::abs(profile.angular_change) > 0.001;
    profile.turn = profile.angular_change < -0.001 ? -1 : 1;
    return profile;
}

[[nodiscard]] bool primitiveEndpointValid(const LatticeControlSet& controls,
                                          const LatticePrimitive& primitive) {
    if (primitive.poses.empty()) return false;
    const Pose2D& endpoint = primitive.poses.back();
    const double cells_x = endpoint.x / controls.resolution;
    const double cells_y = endpoint.y / controls.resolution;
    return std::isfinite(cells_x) && std::isfinite(cells_y) &&
           std::abs(cells_x - std::round(cells_x)) <= kEndpointTolerance &&
           std::abs(cells_y - std::round(cells_y)) <= kEndpointTolerance &&
           angleDistance(endpoint.yaw, controls.headings[primitive.end_angle]) <=
               kEndpointTolerance;
}

[[nodiscard]] std::int64_t endpointCells(double coordinate, double resolution) {
    const double cells = std::round(coordinate / resolution);
    if (!std::isfinite(cells) ||
        cells < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        cells > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("lattice endpoint is outside the supported range");
    }
    return static_cast<std::int64_t>(cells);
}

[[nodiscard]] std::uint32_t oppositeHeading(std::uint32_t heading,
                                           std::size_t count) noexcept {
    return static_cast<std::uint32_t>((heading + count / 2U) % count);
}

[[nodiscard]] std::uint32_t nearestHeading(const LatticeControlSet& controls,
                                           double yaw) noexcept {
    std::uint32_t best = 0U;
    double best_error = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < controls.headings.size(); ++i) {
        const double error = angleDistance(controls.headings[i], yaw);
        if (error < best_error) {
            best = static_cast<std::uint32_t>(i);
            best_error = error;
        }
    }
    return best;
}

[[nodiscard]] Pose2D worldPose(const Pose2D& anchor, double map_yaw,
                               double local_x, double local_y,
                               double local_yaw) noexcept {
    const double c = std::cos(map_yaw);
    const double s = std::sin(map_yaw);
    return {anchor.x + c * local_x - s * local_y,
            anchor.y + s * local_x + c * local_y,
            normalizeAngle(map_yaw + local_yaw)};
}

[[nodiscard]] double localRisk(const GridMap& map, const Pose2D& pose,
                               double influence_distance) noexcept {
    const double clearance = map.getDistanceToObstacle(pose.x, pose.y);
    if (!std::isfinite(clearance) || clearance >= influence_distance) return 0.0;
    return std::clamp((influence_distance - clearance) / influence_distance,
                      0.0, 1.0);
}

[[nodiscard]] double rotationCost(double radians,
                                  const StateLatticeConfig& config,
                                  double nominal_bin_width) noexcept {
    if (radians <= kAngleTolerance) return 0.0;
    return config.rotation_penalty * radians / nominal_bin_width;
}

[[nodiscard]] bool samePose(const Pose2D& a, const Pose2D& b) noexcept {
    return a.x == b.x && a.y == b.y &&
           angleDistance(a.yaw, b.yaw) <= kAngleTolerance;
}

void appendDistinct(std::vector<Pose2D>& path, const Pose2D& pose) {
    if (path.empty() || !samePose(path.back(), pose)) path.push_back(pose);
}

} // namespace

StateLatticePlanner::StateLatticePlanner(
    LatticeControlSet controls,
    std::shared_ptr<const FootprintCollisionChecker> collision_checker,
    StateLatticeConfig config)
    : controls_(std::move(controls)),
      collision_checker_(std::move(collision_checker)), config_(config) {
    if (!collision_checker_ || !std::isfinite(controls_.resolution) ||
        controls_.resolution <= 0.0 ||
        !std::isfinite(controls_.min_turning_radius) ||
        controls_.min_turning_radius <= 0.0 || controls_.headings.size() < 2U ||
        controls_.headings.size() % 2U != 0U || controls_.primitives.empty() ||
        config_.timeout <= std::chrono::nanoseconds::zero() ||
        !std::isfinite(config_.planning_clearance) || config_.planning_clearance < 0.0 ||
        !std::isfinite(config_.rotation_penalty) || config_.rotation_penalty <= 0.0 ||
        !std::isfinite(config_.reverse_penalty) || config_.reverse_penalty < 1.0 ||
        !std::isfinite(config_.change_penalty) || config_.change_penalty < 0.0 ||
        !std::isfinite(config_.non_straight_penalty) ||
        config_.non_straight_penalty < 1.0 ||
        !std::isfinite(config_.cost_penalty) || config_.cost_penalty < 0.0 ||
        !std::isfinite(config_.goal_tolerance) || config_.goal_tolerance < 0.0 ||
        config_.max_iterations == 0U) {
        throw std::invalid_argument("state lattice controls or configuration are invalid");
    }
    for (std::size_t i = 0; i < controls_.headings.size(); ++i) {
        if (!std::isfinite(controls_.headings[i]) ||
            (i > 0U && controls_.headings[i] <= controls_.headings[i - 1U])) {
            throw std::invalid_argument("state lattice headings must be finite and ordered");
        }
        const std::size_t opposite = (i + controls_.headings.size() / 2U) %
                                     controls_.headings.size();
        if (angleDistance(controls_.headings[i],
                          controls_.headings[opposite] - std::numbers::pi) > 1e-6) {
            throw std::invalid_argument("state lattice headings must have opposite bins");
        }
    }
    std::vector<std::uint8_t> has_primitives(controls_.headings.size(), 0U);
    for (const LatticePrimitive& primitive : controls_.primitives) {
        if (primitive.start_angle >= controls_.headings.size() ||
            primitive.end_angle >= controls_.headings.size() ||
            !std::isfinite(primitive.trajectory_length) ||
            primitive.trajectory_length < 0.0 ||
            !primitiveEndpointValid(controls_, primitive) ||
            std::any_of(primitive.poses.begin(), primitive.poses.end(),
                        [](const Pose2D& pose) { return !finite(pose); })) {
            throw std::invalid_argument("state lattice primitive is invalid");
        }
        has_primitives[primitive.start_angle] = 1U;
    }
    if (std::any_of(has_primitives.begin(), has_primitives.end(),
                    [](std::uint8_t present) { return present == 0U; })) {
        throw std::invalid_argument("state lattice requires primitives for every heading");
    }
}

PlannerResult StateLatticePlanner::plan(const Pose2D& start, const Pose2D& goal,
                                       const MapSnapshot& map,
                                       std::stop_token stop) {
    using Clock = std::chrono::steady_clock;
    PlannerResult result;
    if (map.valid()) result.path.map_version = map.version;
    if (!map.valid()) {
        result.error = ErrorCode::MapUnavailable;
        return result;
    }
    if (!finite(start) || !finite(goal)) {
        result.error = ErrorCode::InvalidInput;
        return result;
    }
    const double resolution_tolerance = std::max(1e-9, controls_.resolution * 1e-6);
    if (std::abs(map.grid->resolution() - controls_.resolution) >
        resolution_tolerance) {
        result.error = ErrorCode::InvalidInput;
        return result;
    }

    const auto started = Clock::now();
    const auto interrupted = [&]() {
        if (stop.stop_requested()) {
            result.error = ErrorCode::Canceled;
            return true;
        }
        if (Clock::now() - started >= config_.timeout) {
            result.error = ErrorCode::PlanningTimeout;
            return true;
        }
        return false;
    };
    if (interrupted()) return result;

    const auto start_cell = map.grid->worldToGrid(start.x, start.y);
    if (!start_cell) {
        result.error = ErrorCode::StartOutside;
        return result;
    }
    if (!map.grid->worldToGrid(goal.x, goal.y)) {
        result.error = ErrorCode::GoalOutside;
        return result;
    }
    if (!collision_checker_->poseFree(*map.grid, start,
                                     config_.planning_clearance)) {
        result.error = ErrorCode::StartOccupied;
        return result;
    }
    if (!collision_checker_->poseFree(*map.grid, goal,
                                     config_.planning_clearance)) {
        result.error = ErrorCode::GoalOccupied;
        return result;
    }
    if (interrupted()) return result;

    const std::size_t heading_count = controls_.headings.size();
    const Pose2D& map_origin = map.grid->origin();
    const double map_yaw = map_origin.yaw;
    const double resolution = controls_.resolution;
    const double nominal_bin_width = 2.0 * std::numbers::pi /
                                     static_cast<double>(heading_count);
    const double risk_distance = std::max(map.grid->resolution() * 2.0, resolution);
    const auto poseAt = [&](std::int64_t x, std::int64_t y,
                            std::uint32_t heading, bool /*reverse*/) {
        const double local_x = static_cast<double>(x) * resolution;
        const double local_y = static_cast<double>(y) * resolution;
        const double local_yaw = controls_.headings[heading];
        return worldPose(start, map_yaw, local_x, local_y, local_yaw);
    };

    std::vector<Node> nodes;
    std::unordered_map<LatticeKey, std::size_t, LatticeKeyHash> best_node;
    std::priority_queue<OpenEntry, std::vector<OpenEntry>, OpenEntryLater> open;
    const auto heuristic = [&](const Pose2D& pose) {
        return distance(pose, goal) / resolution;
    };
    const auto pushNode = [&](Node node) {
        const auto existing = best_node.find(node.key);
        if (existing != best_node.end() &&
            nodes[existing->second].cost <= node.cost) {
            return;
        }
        const std::size_t index = nodes.size();
        nodes.push_back(std::move(node));
        best_node[nodes.back().key] = index;
        const Pose2D pose = poseAt(nodes.back().key.x, nodes.back().key.y,
                                   nodes.back().key.heading,
                                   nodes.back().key.reverse);
        const double h = heuristic(pose);
        open.push({index, nodes.back().cost, nodes.back().cost + h, h});
    };

    // The continuous input pose is the spatial origin of the search. Each
    // collision-free heading seed is reached by a checked in-place rotation.
    const std::uint32_t closest_heading = nearestHeading(
        controls_, normalizeAngle(start.yaw - map_yaw));
    for (std::size_t offset = 0; offset < heading_count; ++offset) {
        if (interrupted()) return result;
        const std::uint32_t heading = static_cast<std::uint32_t>(
            (closest_heading + offset) % heading_count);
        const double target_yaw = normalizeAngle(map_yaw + controls_.headings[heading]);
        const Pose2D seed_pose{start.x, start.y, target_yaw};
        if (!collision_checker_->motionFree(*map.grid, start, seed_pose,
                                            config_.planning_clearance)) {
            continue;
        }
        const double initial_rotation = angleDistance(start.yaw, target_yaw);
        const double initial_cost = rotationCost(initial_rotation, config_,
                                                 nominal_bin_width);
        pushNode({{0, 0, heading, 0, false}, initial_cost,
                  std::numeric_limits<std::size_t>::max(),
                  std::numeric_limits<std::size_t>::max(), false, target_yaw});
    }
    if (open.empty()) {
        result.error = ErrorCode::StartOccupied;
        return result;
    }
    std::vector<std::vector<std::size_t>> primitives_by_heading(heading_count);
    for (std::size_t i = 0; i < controls_.primitives.size(); ++i) {
        primitives_by_heading[controls_.primitives[i].start_angle].push_back(i);
    }
    std::vector<PrimitiveProfile> profiles;
    profiles.reserve(controls_.primitives.size());
    for (const LatticePrimitive& primitive : controls_.primitives) {
        profiles.push_back(profileOf(primitive));
    }

    std::size_t best_goal_node = std::numeric_limits<std::size_t>::max();
    double best_goal_approach_yaw = 0.0;
    std::vector<std::uint8_t> expanded(nodes.size(), 0U);

    const auto validateGoalConnection = [&](std::size_t node_index,
                                            bool reverse_motion,
                                            double& connection_cost,
                                            double& approach_yaw) {
        const Node& node = nodes[node_index];
        const Pose2D current = poseAt(node.key.x, node.key.y,
                                      node.key.heading, node.key.reverse);
        const double dx = goal.x - current.x;
        const double dy = goal.y - current.y;
        const double length = std::hypot(dx, dy);
        approach_yaw = current.yaw;
        if (length > kAngleTolerance) {
            approach_yaw = std::atan2(dy, dx);
            if (reverse_motion) approach_yaw = normalizeAngle(approach_yaw + std::numbers::pi);
        }
        const Pose2D aligned{current.x, current.y, approach_yaw};
        const Pose2D translated{goal.x, goal.y, approach_yaw};
        if (!collision_checker_->motionFree(*map.grid, current, aligned,
                                            config_.planning_clearance) ||
            !collision_checker_->motionFree(*map.grid, aligned, translated,
                                            config_.planning_clearance) ||
            !collision_checker_->motionFree(*map.grid, translated, goal,
                                            config_.planning_clearance)) {
            return false;
        }
        const double initial_turn = angleDistance(current.yaw, approach_yaw);
        const double final_turn = angleDistance(approach_yaw, goal.yaw);
        connection_cost = length / resolution *
                (reverse_motion && length > kAngleTolerance
                     ? config_.reverse_penalty : 1.0) +
            rotationCost(initial_turn, config_, nominal_bin_width) +
            rotationCost(final_turn, config_, nominal_bin_width);
        return true;
    };

    while (!open.empty()) {
        if (interrupted()) return result;
        if (result.expanded >= config_.max_iterations) {
            result.error = ErrorCode::PlanningTimeout;
            return result;
        }
        const OpenEntry entry = open.top();
        open.pop();
        if (entry.node >= nodes.size()) continue;
        const Node current_node = nodes[entry.node];
        const auto current_best = best_node.find(current_node.key);
        if (current_best == best_node.end() || current_best->second != entry.node ||
            entry.cost != current_node.cost) {
            continue;
        }
        if (entry.node < expanded.size() && expanded[entry.node] != 0U) continue;
        if (entry.node >= expanded.size()) expanded.resize(nodes.size(), 0U);
        expanded[entry.node] = 1U;

        const Pose2D current_pose = poseAt(current_node.key.x, current_node.key.y,
                                           current_node.key.heading,
                                           current_node.key.reverse);
        ++result.expanded;

        if (distance(current_pose, goal) <= config_.goal_tolerance) {
            const std::size_t direction_count = config_.allow_reverse_expansion ? 2U : 1U;
            double best_connection_cost = std::numeric_limits<double>::infinity();
            double best_connection_yaw = current_pose.yaw;
            for (std::size_t direction = 0; direction < direction_count; ++direction) {
                if (interrupted()) return result;
                double connection_cost = 0.0;
                double approach_yaw = 0.0;
                const bool reverse_motion = direction != 0U;
                if (validateGoalConnection(entry.node, reverse_motion,
                                           connection_cost, approach_yaw) &&
                    connection_cost < best_connection_cost) {
                    best_connection_cost = connection_cost;
                    best_connection_yaw = approach_yaw;
                }
            }
            if (interrupted()) return result;
            if (std::isfinite(best_connection_cost)) {
                // Nav2's A* accepts a valid analytic expansion from the
                // current node immediately. Do the same for this local
                // rotate/straight/rotate connector instead of continuing to
                // expand merely to prove global cost optimality.
                best_goal_node = entry.node;
                best_goal_approach_yaw = best_connection_yaw;
                break;
            }
        }

        const std::size_t directions = config_.allow_reverse_expansion ? 2U : 1U;
        for (std::size_t direction = 0; direction < directions; ++direction) {
            if (interrupted()) return result;
            const bool reverse = direction != 0U;
            const std::uint32_t source_heading = reverse
                ? oppositeHeading(current_node.key.heading, heading_count)
                : current_node.key.heading;
            for (const std::size_t primitive_index :
                 primitives_by_heading[source_heading]) {
                if (interrupted()) return result;
                const LatticePrimitive& primitive = controls_.primitives[primitive_index];
                const std::int64_t end_dx = endpointCells(
                    primitive.poses.back().x, resolution);
                const std::int64_t end_dy = endpointCells(
                    primitive.poses.back().y, resolution);
                if ((end_dx > 0 && current_node.key.x >
                                     std::numeric_limits<std::int64_t>::max() - end_dx) ||
                    (end_dx < 0 && current_node.key.x <
                                     std::numeric_limits<std::int64_t>::min() - end_dx) ||
                    (end_dy > 0 && current_node.key.y >
                                     std::numeric_limits<std::int64_t>::max() - end_dy) ||
                    (end_dy < 0 && current_node.key.y <
                                     std::numeric_limits<std::int64_t>::min() - end_dy)) {
                    continue;
                }
                const std::int64_t next_x = current_node.key.x + end_dx;
                const std::int64_t next_y = current_node.key.y + end_dy;
                const std::uint32_t next_heading = reverse
                    ? oppositeHeading(primitive.end_angle, heading_count)
                    : primitive.end_angle;
                const PrimitiveProfile& profile = profiles[primitive_index];
                const Pose2D endpoint = poseAt(next_x, next_y, next_heading, reverse);
                if (!map.grid->worldToGrid(endpoint.x, endpoint.y)) continue;

                Pose2D previous = current_pose;
                double maximum_risk = localRisk(*map.grid, previous, risk_distance);
                bool clear = true;
                for (const Pose2D& primitive_pose : primitive.poses) {
                    const double local_x = static_cast<double>(current_node.key.x) * resolution +
                                           primitive_pose.x;
                    const double local_y = static_cast<double>(current_node.key.y) * resolution +
                                           primitive_pose.y;
                    double local_yaw = primitive_pose.yaw;
                    if (reverse) local_yaw = normalizeAngle(local_yaw + std::numbers::pi);
                    Pose2D sample = worldPose(start, map_yaw, local_x, local_y, local_yaw);
                    if (!collision_checker_->motionFree(*map.grid, previous, sample,
                                                        config_.planning_clearance)) {
                        clear = false;
                        break;
                    }
                    maximum_risk = std::max(maximum_risk,
                                            localRisk(*map.grid, sample, risk_distance));
                    previous = sample;
                }
                if (!clear) continue;
                // Snap the validated primitive endpoint to its declared bin;
                // the loader guarantees that the angular mismatch is tiny.
                if (!collision_checker_->motionFree(*map.grid, previous, endpoint,
                                                    config_.planning_clearance)) {
                    continue;
                }
                maximum_risk = std::max(maximum_risk,
                                        localRisk(*map.grid, endpoint, risk_distance));

                double transition_cost = 0.0;
                if (primitive.trajectory_length < 1e-4) {
                    transition_cost = config_.rotation_penalty;
                } else {
                    double multiplier = 1.0 + config_.cost_penalty * maximum_risk;
                    if (profile.curved) {
                        multiplier *= config_.non_straight_penalty;
                        if (current_node.key.turn != 0 &&
                            current_node.key.turn != profile.turn) {
                            multiplier += config_.change_penalty;
                        }
                    }
                    if (reverse) multiplier *= config_.reverse_penalty;
                    transition_cost = primitive.trajectory_length / resolution * multiplier;
                }
                if (!std::isfinite(transition_cost) || transition_cost <= 0.0) continue;

                LatticeKey next_key{next_x, next_y, next_heading,
                                    primitive.trajectory_length < 1e-4
                                        ? current_node.key.turn : profile.turn,
                                    reverse};
                pushNode({next_key, current_node.cost + transition_cost,
                          entry.node, primitive_index, reverse, 0.0});
            }
        }
        if (expanded.size() < nodes.size()) expanded.resize(nodes.size(), 0U);
    }

    if (best_goal_node == std::numeric_limits<std::size_t>::max()) {
        result.error = ErrorCode::NoPath;
        return result;
    }

    std::vector<std::size_t> chain;
    for (std::size_t node = best_goal_node;
         node != std::numeric_limits<std::size_t>::max();
         node = nodes[node].parent) {
        chain.push_back(node);
        if (chain.size() > nodes.size()) {
            result.error = ErrorCode::PlannerFailure;
            return result;
        }
    }
    std::reverse(chain.begin(), chain.end());
    result.path.poses.push_back(start);
    if (!chain.empty()) {
        const Node& seed = nodes[chain.front()];
        const Pose2D seed_pose{start.x, start.y, seed.seed_yaw};
        appendDistinct(result.path.poses, seed_pose);
    }
    for (std::size_t chain_index = 1; chain_index < chain.size(); ++chain_index) {
        if (interrupted()) {
            result.path.poses.clear();
            return result;
        }
        const Node& node = nodes[chain[chain_index]];
        const Node& parent = nodes[chain[chain_index - 1U]];
        const LatticePrimitive& primitive = controls_.primitives[node.primitive];
        for (const Pose2D& primitive_pose : primitive.poses) {
            double local_yaw = primitive_pose.yaw;
            if (node.reverse) local_yaw = normalizeAngle(local_yaw + std::numbers::pi);
            appendDistinct(result.path.poses,
                           worldPose(start, map_yaw,
                                     static_cast<double>(parent.key.x) * resolution +
                                         primitive_pose.x,
                                     static_cast<double>(parent.key.y) * resolution +
                                         primitive_pose.y,
                                     local_yaw));
        }
        appendDistinct(result.path.poses,
                       poseAt(node.key.x, node.key.y, node.key.heading,
                              node.key.reverse));
    }

    const Pose2D current = poseAt(nodes[best_goal_node].key.x,
                                  nodes[best_goal_node].key.y,
                                  nodes[best_goal_node].key.heading,
                                  nodes[best_goal_node].key.reverse);
    if (distance(current, goal) > kAngleTolerance) {
        appendDistinct(result.path.poses,
                       {current.x, current.y, best_goal_approach_yaw});
        appendDistinct(result.path.poses,
                       {goal.x, goal.y, best_goal_approach_yaw});
    }
    appendDistinct(result.path.poses, goal);
    if (result.path.poses.empty()) {
        result.error = ErrorCode::PlannerFailure;
        return result;
    }
    result.error = ErrorCode::None;
    return result;
}

} // namespace rbt::navigation

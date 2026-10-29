// Copyright (c) 2022 Samsung Research America, @artofnothingness Alexey Budyakov
// Copyright (c) 2023 Dexory
// Copyright (c) 2023 Open Navigation LLC
// Copyright (c) 2026 RBT contributors
//
// Native adaptation of Nav2 FeasiblePathHandler's ordered path constraints.
// SPDX-License-Identifier: Apache-2.0
// See licenses/Nav2-APACHE.txt and docs/nav2-port.md.

#include <rbt_navigation/controller/feasible_path_handler.hpp>
#include <rbt_navigation/map/grid_map.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace rbt::navigation {
namespace {

constexpr double kTranslationEpsilon = 1e-4;

[[nodiscard]] bool samePath(const Path2D& lhs, const Path2D& rhs) noexcept {
    if (lhs.map_version != rhs.map_version || lhs.goal_id != rhs.goal_id ||
        lhs.poses.size() != rhs.poses.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.poses.size(); ++index) {
        const Pose2D& left = lhs.poses[index];
        const Pose2D& right = rhs.poses[index];
        if (left.x != right.x || left.y != right.y || left.yaw != right.yaw) {
            return false;
        }
    }
    return true;
}

} // namespace

FeasiblePathHandler::FeasiblePathHandler(bool enforce_path_inversion,
                                         bool enforce_path_rotation,
                                         double minimum_path_rotation)
    : enforce_path_inversion_(enforce_path_inversion),
      enforce_path_rotation_(enforce_path_rotation),
      minimum_path_rotation_(minimum_path_rotation) {
    if (!std::isfinite(minimum_path_rotation_) || minimum_path_rotation_ < 0.0) {
        throw std::invalid_argument("minimum path rotation must be finite and nonnegative");
    }
}

void FeasiblePathHandler::reset(const Path2D& path, std::uint64_t map_version,
                                const GridMap* map_identity) {
    path_ = path;
    map_version_ = map_version;
    map_identity_ = map_identity;
    cursor_ = 0U;
    progress_index_ = 0U;
    current_boundary_ = path_.poses.empty() ? 0U : path_.poses.size() - 1U;
    current_final_segment_ = true;
    initialized_ = true;
}

std::size_t FeasiblePathHandler::firstConstraint(std::size_t begin) const noexcept {
    if (path_.poses.empty()) return 0U;

    const std::size_t last = path_.poses.size() - 1U;
    if (begin >= last) return last;

    std::size_t boundary = last;
    if (enforce_path_inversion_) {
        bool have_previous_translation = false;
        double previous_dx = 0.0;
        double previous_dy = 0.0;
        for (std::size_t index = begin; index < last; ++index) {
            const Pose2D& start = path_.poses[index];
            const Pose2D& end = path_.poses[index + 1U];
            const double dx = end.x - start.x;
            const double dy = end.y - start.y;
            if (std::hypot(dx, dy) <= kTranslationEpsilon) continue;

            if (have_previous_translation && previous_dx * dx + previous_dy * dy < 0.0) {
                // The cusp is the first pose of this reversed segment. Any
                // zero-length rotation immediately before it stays in the
                // preceding segment.
                boundary = std::min(boundary, index);
                break;
            }
            previous_dx = dx;
            previous_dy = dy;
            have_previous_translation = true;
        }
    }

    if (enforce_path_rotation_ && minimum_path_rotation_ > 0.0) {
        std::size_t index = begin;
        while (index < last) {
            const Pose2D& start = path_.poses[index];
            const Pose2D& end = path_.poses[index + 1U];
            if (std::hypot(end.x - start.x, end.y - start.y) > kTranslationEpsilon) {
                ++index;
                continue;
            }

            double accumulated_rotation = 0.0;
            std::size_t rotation_end = index;
            bool rotation_constraint_found = false;
            for (; rotation_end < last; ++rotation_end) {
                const Pose2D& rotation_start_pose = path_.poses[rotation_end];
                const Pose2D& rotation_end_pose = path_.poses[rotation_end + 1U];
                if (std::hypot(rotation_end_pose.x - rotation_start_pose.x,
                               rotation_end_pose.y - rotation_start_pose.y) >
                    kTranslationEpsilon) {
                    break;
                }
                accumulated_rotation += std::abs(
                    normalizeAngle(rotation_end_pose.yaw - rotation_start_pose.yaw));
                if (accumulated_rotation > minimum_path_rotation_) {
                    boundary = std::min(boundary, rotation_end + 1U);
                    rotation_constraint_found = true;
                    break;
                }
            }
            if (rotation_constraint_found) break;
            index = rotation_end + 1U;
        }
    }

    return boundary;
}

FeasiblePathSegment FeasiblePathHandler::update(const Path2D& path,
                                                const Pose2D& robot_pose,
                                                std::uint64_t map_version,
                                                const GridMap* map_identity) {
    if (!initialized_ || map_version_ != map_version || map_identity_ != map_identity ||
        !samePath(path_, path)) {
        reset(path, map_version, map_identity);
    }

    FeasiblePathSegment result;
    result.path.map_version = path_.map_version;
    result.path.goal_id = path_.goal_id;
    if (path_.poses.empty()) return result;

    current_boundary_ = firstConstraint(cursor_);
    current_final_segment_ = current_boundary_ == path_.poses.size() - 1U;
    progress_index_ = std::clamp(progress_index_, cursor_, current_boundary_);

    std::size_t closest_index = progress_index_;
    double closest_distance_squared = std::numeric_limits<double>::infinity();
    for (std::size_t index = progress_index_; index <= current_boundary_; ++index) {
        const Pose2D& pose = path_.poses[index];
        const double dx = robot_pose.x - pose.x;
        const double dy = robot_pose.y - pose.y;
        const double distance_squared = dx * dx + dy * dy;
        if (distance_squared < closest_distance_squared) {
            closest_distance_squared = distance_squared;
            closest_index = index;
        }
    }
    progress_index_ = std::max(progress_index_, closest_index);

    const std::size_t start_index = progress_index_ > cursor_
        ? progress_index_ - 1U
        : progress_index_;
    result.path.poses.assign(path_.poses.begin() + static_cast<std::ptrdiff_t>(start_index),
                             path_.poses.begin() + static_cast<std::ptrdiff_t>(current_boundary_ + 1U));
    result.final_segment = current_final_segment_;
    return result;
}

void FeasiblePathHandler::advance() {
    if (!initialized_ || current_final_segment_) return;
    cursor_ = current_boundary_;
    progress_index_ = cursor_;
}

} // namespace rbt::navigation

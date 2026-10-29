#include <rbt_navigation/planner/a_star.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <queue>
#include <stdexcept>
#include <vector>

namespace rbt::navigation {
namespace {

struct OpenNode {
    int index{};
    double cost{};
    double estimate{};
    double heuristic{};
};

// The grid-search structure is adapted from ROSE Navigation by hyheiyue
// (Copyright (c) 2026, MIT License), with an admissible heuristic and checked edges.
struct OpenNodeLater {
    bool operator()(const OpenNode& a, const OpenNode& b) const noexcept {
        if (a.estimate == b.estimate) return a.heuristic > b.heuristic;
        return a.estimate > b.estimate;
    }
};

double octileDistance(int x, int y, int goal_x, int goal_y, double resolution) noexcept {
    const int dx = std::abs(x - goal_x);
    const int dy = std::abs(y - goal_y);
    const int diagonal = std::min(dx, dy);
    const int straight = std::max(dx, dy) - diagonal;
    return resolution * (static_cast<double>(straight) +
                         static_cast<double>(diagonal) * std::numbers::sqrt2);
}

bool samePosition(const Pose2D& a, const Pose2D& b) noexcept {
    return a.x == b.x && a.y == b.y;
}

} // namespace

AStarPlanner::AStarPlanner(PlannerConfig config) : config_(config) {
    if (!std::isfinite(config_.robot_radius) || config_.robot_radius < 0.0 ||
        config_.timeout <= std::chrono::nanoseconds::zero()) {
        throw std::invalid_argument("planner radius and timeout must be valid");
    }
}

PlannerResult AStarPlanner::plan(const Pose2D& start, const Pose2D& goal,
                                 const MapSnapshot& map, std::stop_token stop) {
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
    const auto goal_cell = map.grid->worldToGrid(goal.x, goal.y);
    if (!goal_cell) {
        result.error = ErrorCode::GoalOutside;
        return result;
    }
    if (map.grid->isOccupied(start.x, start.y) ||
        !map.grid->isFree(start.x, start.y, config_.robot_radius)) {
        result.error = ErrorCode::StartOccupied;
        return result;
    }
    if (map.grid->isOccupied(goal.x, goal.y) ||
        !map.grid->isFree(goal.x, goal.y, config_.robot_radius)) {
        result.error = ErrorCode::GoalOccupied;
        return result;
    }

    const std::size_t width = map.grid->width();
    const std::size_t height = map.grid->height();
    const std::size_t count = width * height;
    const int start_x = start_cell->first;
    const int start_y = start_cell->second;
    const int goal_x = goal_cell->first;
    const int goal_y = goal_cell->second;
    const int start_index = static_cast<int>(static_cast<std::size_t>(start_y) * width +
                                             static_cast<std::size_t>(start_x));
    const int goal_index = static_cast<int>(static_cast<std::size_t>(goal_y) * width +
                                            static_cast<std::size_t>(goal_x));
    if (start_index == goal_index) {
        if (interrupted()) return result;
        if (map.grid->segmentFree(start, goal, config_.robot_radius)) {
            if (interrupted()) return result;
            result.path.poses.push_back(start);
            if (!samePosition(start, goal) || start.yaw != goal.yaw) {
                result.path.poses.push_back(goal);
            }
            return result;
        }
    }
    const auto indexOf = [width](int x, int y) {
        return static_cast<int>(static_cast<std::size_t>(y) * width +
                                static_cast<std::size_t>(x));
    };
    const auto xOf = [width](int index) {
        return static_cast<int>(static_cast<std::size_t>(index) % width);
    };
    const auto yOf = [width](int index) {
        return static_cast<int>(static_cast<std::size_t>(index) / width);
    };

    std::vector<std::int8_t> safety(count, -1);
    const auto cellIsSafe = [&](int x, int y) {
        const int index = indexOf(x, y);
        auto& cached = safety[static_cast<std::size_t>(index)];
        if (cached < 0) {
            const Pose2D center = map.grid->gridToWorld(x, y);
            cached = static_cast<std::int8_t>(map.grid->isFree(
                center.x, center.y, config_.robot_radius));
        }
        return cached != 0;
    };

    if (interrupted()) return result;
    if (!cellIsSafe(start_x, start_y)) {
        result.error = ErrorCode::StartOccupied;
        return result;
    }
    if (!cellIsSafe(goal_x, goal_y)) {
        result.error = ErrorCode::GoalOccupied;
        return result;
    }
    const Pose2D start_center = map.grid->gridToWorld(start_x, start_y);
    const Pose2D goal_center = map.grid->gridToWorld(goal_x, goal_y);
    if (!map.grid->segmentFree(start, start_center, config_.robot_radius)) {
        result.error = ErrorCode::StartOccupied;
        return result;
    }
    if (!map.grid->segmentFree(goal_center, goal, config_.robot_radius)) {
        result.error = ErrorCode::GoalOccupied;
        return result;
    }
    if (interrupted()) return result;

    const auto makePath = [&](const std::vector<int>& cells) {
        std::vector<Pose2D> points;
        points.reserve(cells.size() + 2);
        points.push_back(start);
        for (const int index : cells) {
            const Pose2D center = map.grid->gridToWorld(xOf(index), yOf(index));
            if (!samePosition(points.back(), center)) points.push_back(center);
        }
        if (!samePosition(points.back(), goal) || points.back().yaw != goal.yaw) {
            points.push_back(goal);
        } else {
            points.back() = goal;
        }

        for (std::size_t i = 1; i + 1 < points.size(); ++i) {
            const double dx = points[i + 1].x - points[i].x;
            const double dy = points[i + 1].y - points[i].y;
            if (dx != 0.0 || dy != 0.0) points[i].yaw = std::atan2(dy, dx);
        }
        points.front() = start;
        if (points.size() > 1) points.back() = goal;
        result.path.poses = std::move(points);
    };

    if (interrupted()) return result;
    std::vector<double> costs(count, std::numeric_limits<double>::infinity());
    std::vector<int> parents(count, -1);
    std::vector<std::uint8_t> closed(count, 0);
    std::priority_queue<OpenNode, std::vector<OpenNode>, OpenNodeLater> open;
    const double resolution = map.grid->resolution();
    const double initial_h = octileDistance(start_x, start_y, goal_x, goal_y, resolution);
    costs[static_cast<std::size_t>(start_index)] = 0.0;
    open.push({start_index, 0.0, initial_h, initial_h});

    constexpr std::array<int, 8> dx{1, -1, 0, 0, 1, 1, -1, -1};
    constexpr std::array<int, 8> dy{0, 0, 1, -1, 1, -1, 1, -1};
    bool found = false;
    while (!open.empty()) {
        if (interrupted()) return result;
        const OpenNode current = open.top();
        open.pop();
        const std::size_t current_index = static_cast<std::size_t>(current.index);
        if (current.cost > costs[current_index] || closed[current_index] != 0) continue;
        if (current.index == goal_index) {
            found = true;
            break;
        }
        closed[current_index] = 1;
        ++result.expanded;

        const int x = xOf(current.index);
        const int y = yOf(current.index);
        const Pose2D current_center = map.grid->gridToWorld(x, y);
        for (std::size_t direction = 0; direction < dx.size(); ++direction) {
            const int next_x = x + dx[direction];
            const int next_y = y + dy[direction];
            if (next_x < 0 || next_y < 0 ||
                static_cast<std::size_t>(next_x) >= width ||
                static_cast<std::size_t>(next_y) >= height) {
                continue;
            }
            const bool diagonal = dx[direction] != 0 && dy[direction] != 0;
            if (diagonal && (!cellIsSafe(x + dx[direction], y) ||
                             !cellIsSafe(x, y + dy[direction]))) {
                continue;
            }
            if (!cellIsSafe(next_x, next_y)) continue;
            const int next_index = indexOf(next_x, next_y);
            const std::size_t next_slot = static_cast<std::size_t>(next_index);
            if (closed[next_slot] != 0) continue;

            const Pose2D next_center = map.grid->gridToWorld(next_x, next_y);
            if (!map.grid->segmentFree(current_center, next_center, config_.robot_radius)) continue;
            const double edge_cost = resolution * (diagonal ? std::numbers::sqrt2 : 1.0);
            const double next_cost = current.cost + edge_cost;
            if (next_cost >= costs[next_slot]) continue;

            costs[next_slot] = next_cost;
            parents[next_slot] = current.index;
            const double heuristic = octileDistance(next_x, next_y, goal_x, goal_y, resolution);
            open.push({next_index, next_cost, next_cost + heuristic, heuristic});
        }
    }

    if (!found) {
        if (interrupted()) return result;
        result.error = ErrorCode::NoPath;
        return result;
    }
    if (interrupted()) return result;

    std::vector<int> reverse_cells;
    reverse_cells.reserve(static_cast<std::size_t>(std::abs(goal_x - start_x) +
                                                   std::abs(goal_y - start_y)) + 1);
    int index = goal_index;
    while (index >= 0) {
        reverse_cells.push_back(index);
        if (index == start_index) break;
        index = parents[static_cast<std::size_t>(index)];
    }
    if (reverse_cells.empty() || reverse_cells.back() != start_index) {
        result.error = ErrorCode::NoPath;
        return result;
    }
    std::reverse(reverse_cells.begin(), reverse_cells.end());
    makePath(reverse_cells);
    if (interrupted()) {
        result.path.poses.clear();
        return result;
    }
    return result;
}

} // namespace rbt::navigation

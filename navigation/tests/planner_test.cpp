#include <rbt_navigation/planner/a_star.hpp>

#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>

using namespace rbt::navigation;

namespace {

OccupancyGrid makeGrid(std::size_t width, std::size_t height) {
    OccupancyGrid grid;
    grid.width = width;
    grid.height = height;
    grid.resolution = 1.0;
    grid.cells.assign(width * height, 0);
    return grid;
}

MapSnapshot makeSnapshot(OccupancyGrid grid, std::uint64_t version = 7) {
    return {std::make_shared<GridMap>(std::move(grid)), version, 0};
}

double pathLength(const Path2D& path) {
    double length = 0.0;
    for (std::size_t i = 1; i < path.poses.size(); ++i) {
        length += distance(path.poses[i - 1], path.poses[i]);
    }
    return length;
}

template <typename F>
void mustThrow(F&& function) {
    bool threw = false;
    try {
        function();
    } catch (const std::exception&) {
        threw = true;
    }
    assert(threw);
}

} // namespace

int main() {
    {
        AStarPlanner planner({0.0, std::chrono::seconds(1)});
        auto map = makeSnapshot(makeGrid(10, 10), 19);
        const Pose2D start{1.5, 1.5, 0.3};
        const Pose2D goal{6.5, 5.5, -0.4};
        const PlannerResult result = planner.plan(start, goal, map);
        assert(result.success());
        assert(result.path.map_version == 19);
        assert(result.path.poses.front().x == start.x && result.path.poses.front().yaw == start.yaw);
        assert(result.path.poses.back().x == goal.x && result.path.poses.back().yaw == goal.yaw);
        assert(std::abs(pathLength(result.path) - (4.0 * std::numbers::sqrt2 + 1.0)) < 1e-10);
        for (std::size_t i = 1; i < result.path.poses.size(); ++i) {
            assert(map.grid->segmentFree(result.path.poses[i - 1], result.path.poses[i], 0.0));
        }
    }
    {
        auto grid = makeGrid(9, 7);
        grid.cells[3 * grid.width + 4] = 100;
        auto map = makeSnapshot(std::move(grid));
        AStarPlanner planner({0.0, std::chrono::seconds(1)});
        const auto result = planner.plan({2.5, 3.5, 0.0}, {6.5, 3.5, 0.0}, map);
        assert(result.success());
        assert(pathLength(result.path) > 4.0);
        for (std::size_t i = 1; i < result.path.poses.size(); ++i) {
            assert(map.grid->segmentFree(result.path.poses[i - 1], result.path.poses[i], 0.0));
        }
    }
    {
        auto grid = makeGrid(3, 3);
        grid.cells[1] = 100; // (1, 0) blocks the direct diagonal from (0, 0) to (1, 1).
        auto map = makeSnapshot(std::move(grid));
        AStarPlanner planner({0.0, std::chrono::seconds(1)});
        const auto result = planner.plan({0.5, 0.5, 0.0}, {1.5, 1.5, 0.0}, map);
        assert(result.success());
        assert(result.path.poses.size() >= 3);
        bool passed_through_open_cardinal = false;
        for (const auto& pose : result.path.poses) {
            if (std::abs(pose.x - 0.5) < 1e-12 && std::abs(pose.y - 1.5) < 1e-12) {
                passed_through_open_cardinal = true;
            }
        }
        assert(passed_through_open_cardinal);
    }
    {
        auto grid = makeGrid(9, 7);
        for (std::size_t y = 0; y < grid.height; ++y) grid.cells[y * grid.width + 4] = 100;
        auto map = makeSnapshot(std::move(grid));
        AStarPlanner planner({0.0, std::chrono::seconds(1)});
        const auto result = planner.plan({2.5, 3.5, 0.0}, {6.5, 3.5, 0.0}, map);
        assert(result.error == ErrorCode::NoPath);
        assert(result.path.poses.empty());
    }
    {
        auto grid = makeGrid(5, 5);
        grid.cells[1 * grid.width + 2] = 100;
        auto map = makeSnapshot(std::move(grid));
        AStarPlanner planner({0.6, std::chrono::seconds(1)});
        const auto result = planner.plan({1.5, 1.5, 0.0}, {3.5, 3.5, 0.0}, map);
        assert(result.error == ErrorCode::StartOccupied);
    }
    {
        auto map = makeSnapshot(makeGrid(4, 4), 23);
        AStarPlanner planner({0.0, std::chrono::seconds(1)});
        const Pose2D start{1.5, 1.5, 0.25};
        const Pose2D goal{1.5, 1.5, 1.25};
        const auto result = planner.plan(start, goal, map);
        assert(result.success());
        assert(result.path.map_version == 23);
        assert(result.path.poses.size() == 2);
        assert(result.path.poses.front().yaw == start.yaw);
        assert(result.path.poses.back().yaw == goal.yaw);
    }
    {
        auto map = makeSnapshot(makeGrid(3, 3));
        AStarPlanner planner({0.0, std::chrono::seconds(1)});
        assert(planner.plan({-0.1, 0.5, 0.0}, {1.5, 1.5, 0.0}, map).error ==
               ErrorCode::StartOutside);
        assert(planner.plan({0.5, 0.5, 0.0}, {3.0, 1.5, 0.0}, map).error ==
               ErrorCode::GoalOutside);
        auto occupied_grid = makeGrid(3, 3);
        occupied_grid.cells[0] = 100;
        auto occupied_map = makeSnapshot(std::move(occupied_grid));
        assert(planner.plan({0.5, 0.5, 0.0}, {1.5, 1.5, 0.0}, occupied_map).error ==
               ErrorCode::StartOccupied);
        assert(planner.plan({1.5, 1.5, 0.0}, {0.5, 0.5, 0.0}, occupied_map).error ==
               ErrorCode::GoalOccupied);
        assert(planner.plan({0.5, 0.5, 0.0}, {1.5, 1.5, 0.0}, {}).error ==
               ErrorCode::MapUnavailable);
        assert(planner.plan({0.5, 0.5, std::numeric_limits<double>::quiet_NaN()},
                            {1.5, 1.5, 0.0}, map).error == ErrorCode::InvalidInput);
    }
    {
        auto map = makeSnapshot(makeGrid(40, 40));
        AStarPlanner planner({0.0, std::chrono::seconds(1)});
        std::stop_source source;
        source.request_stop();
        assert(planner.plan({1.5, 1.5, 0.0}, {38.5, 38.5, 0.0}, map,
                            source.get_token()).error == ErrorCode::Canceled);

        AStarPlanner short_timeout({0.0, std::chrono::nanoseconds(1)});
        assert(short_timeout.plan({1.5, 1.5, 0.0}, {38.5, 38.5, 0.0}, map).error ==
               ErrorCode::PlanningTimeout);
    }
    {
        mustThrow([] { AStarPlanner invalid({-0.1, std::chrono::seconds(1)}); });
        mustThrow([] { AStarPlanner invalid({0.0, std::chrono::nanoseconds::zero()}); });
    }
    std::cout << "A* shortest path, collision checks, endpoint errors, timeout and cancellation: PASS\n";
}

#include <rbt_navigation/geometry/footprint.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <utility>
#include <vector>

using namespace rbt::navigation;

namespace {

OccupancyGrid makeGrid(std::size_t width, std::size_t height,
                       double resolution = 0.1) {
    OccupancyGrid grid;
    grid.width = width;
    grid.height = height;
    grid.resolution = resolution;
    grid.cells.assign(width * height, 0);
    return grid;
}

std::size_t cellIndex(const OccupancyGrid& grid, std::size_t x,
                      std::size_t y) {
    return y * grid.width + x;
}

bool near(double actual, double expected, double tolerance = 1e-10) {
    return std::abs(actual - expected) <= tolerance;
}

template <typename F>
void mustThrow(F&& function) {
    bool threw = false;
    try {
        function();
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
}

} // namespace

int main() {
    {
        const FootprintCollisionChecker checker(
            {{-0.2, -0.1}, {0.2, -0.1}, {0.2, 0.1}, {-0.2, 0.1}});
        GridMap clear_map(makeGrid(80, 80));
        assert(checker.poseFree(clear_map, {4.0, 4.0, 0.0}));

        auto occupied = makeGrid(80, 80);
        occupied.cells[cellIndex(occupied, 40, 40)] = 100;
        GridMap occupied_map(std::move(occupied));
        assert(!checker.poseFree(occupied_map, {4.0, 4.0, 0.0}));
    }
    {
        const FootprintCollisionChecker checker(
            {{-1.0, -1.0}, {1.0, -1.0}, {1.0, 1.0}, {-1.0, 1.0}});
        auto grid = makeGrid(10, 10, 1.0);
        grid.cells[cellIndex(grid, 5, 5)] = 100;
        GridMap map(std::move(grid));
        // The occupied cell lies strictly inside the polygon, away from every edge.
        assert(!checker.poseFree(map, {5.0, 5.0, 0.0}));
    }
    {
        const FootprintCollisionChecker checker(
            {{-0.2, -0.03}, {0.2, -0.03}, {0.2, 0.03}, {-0.2, 0.03}});
        auto grid = makeGrid(40, 40);
        grid.origin = {10.0, -4.0, std::numbers::pi / 2.0};
        grid.cells[cellIndex(grid, 22, 20)] = 100;
        GridMap map(std::move(grid));
        const Pose2D footprint_center{10.0 - 2.05, -4.0 + 2.05,
                                      std::numbers::pi / 2.0};
        assert(!checker.poseFree(map, footprint_center));
    }
    {
        const FootprintCollisionChecker checker(
            {{-0.5, -0.5}, {0.5, -0.5}, {0.5, 0.5}, {-0.5, 0.5}});
        GridMap map(makeGrid(10, 10, 1.0));
        assert(!checker.poseFree(map, {0.5, 5.0, 0.0}));
        assert(checker.poseFree(map, {0.6, 5.0, 0.0}));

        auto unknown = makeGrid(10, 10, 1.0);
        unknown.cells[cellIndex(unknown, 5, 5)] = -1;
        GridMap unknown_map(std::move(unknown));
        assert(!checker.poseFree(unknown_map, {5.5, 5.5, 0.0}));
    }
    {
        const FootprintCollisionChecker checker(
            {{-0.2, -0.2}, {0.2, -0.2}, {0.2, 0.2}, {-0.2, 0.2}});
        auto grid = makeGrid(10, 10, 1.0);
        grid.cells[cellIndex(grid, 5, 5)] = 100;
        GridMap map(std::move(grid));
        assert(checker.poseFree(map, {4.5, 5.5, 0.0}));
        assert(checker.poseFree(map, {4.5, 5.5, 0.0}, 0.29));
        assert(!checker.poseFree(map, {4.5, 5.5, 0.0}, 0.31));
    }
    {
        const FootprintCollisionChecker checker(
            {{-1.0, -0.15}, {0.4, -0.15}, {0.4, 0.15}, {-1.0, 0.15}});
        auto grid = makeGrid(60, 60);
        grid.cells[cellIndex(grid, 20, 29)] = 100;
        GridMap map(std::move(grid));
        const Pose2D start{3.0, 3.0, -std::numbers::pi / 2.0};
        const Pose2D finish{3.0, 3.0, std::numbers::pi / 2.0};
        assert(checker.poseFree(map, start));
        assert(checker.poseFree(map, finish));
        assert(!checker.motionFree(map, start, finish));
    }
    {
        RobotGeometry geometry;
        geometry.drive_to_base = {-0.196, 0.0, 0.0};
        const Pose2D drive_pose{2.0, 2.0, std::numbers::pi / 2.0};
        const Pose2D base_pose = driveToBase(drive_pose, geometry);
        assert(near(base_pose.x, 2.0));
        assert(near(base_pose.y, 1.804));
        const Pose2D round_trip = baseToDrive(base_pose, geometry);
        assert(near(round_trip.x, drive_pose.x));
        assert(near(round_trip.y, drive_pose.y));
        assert(near(normalizeAngle(round_trip.yaw - drive_pose.yaw), 0.0));

        const Pose2D turned_base = driveToBase({2.0, 2.0, 0.0}, geometry);
        const Pose2D rotated_base = driveToBase(
            {2.0, 2.0, std::numbers::pi / 2.0}, geometry);
        assert(near(std::hypot(turned_base.x - 2.0, turned_base.y - 2.0),
                    0.196));
        assert(near(std::hypot(rotated_base.x - 2.0, rotated_base.y - 2.0),
                    0.196));
        assert(near(std::hypot(rotated_base.x - turned_base.x,
                              rotated_base.y - turned_base.y),
                    std::sqrt(2.0) * 0.196));
    }
    {
        const FootprintCollisionChecker checker(
            {{-0.05, -0.05}, {0.05, -0.05}, {0.05, 0.05}, {-0.05, 0.05}});
        GridMap clear_map(makeGrid(100, 100));
        const Pose2D start{3.0, 3.0, 0.0};
        const Twist2D arc{1.0, 1.0};
        const double duration = std::numbers::pi / 2.0;
        const Pose2D expected_end{4.0, 4.0, std::numbers::pi / 2.0};
        const Pose2D actual_end{start.x + std::sin(duration),
                               start.y + 1.0 - std::cos(duration), duration};
        assert(near(actual_end.x, expected_end.x));
        assert(near(actual_end.y, expected_end.y));
        assert(checker.commandFree(clear_map, start, arc, duration));

        auto occupied = makeGrid(100, 100);
        occupied.cells[cellIndex(occupied, 40, 40)] = 100;
        GridMap occupied_map(std::move(occupied));
        assert(!checker.commandFree(occupied_map, start, arc, duration));
    }
    {
        const FootprintCollisionChecker checker(
            {{-1.0, -0.15}, {0.4, -0.15}, {0.4, 0.15}, {-1.0, 0.15}});
        auto grid = makeGrid(100, 100);
        grid.cells[cellIndex(grid, 43, 42)] = 100;
        GridMap map(std::move(grid));
        assert(!checker.commandFree(map, {5.0, 5.0, 0.0}, {0.0, 1.0}, 1.0,
                                    0.0));
    }
    {
        mustThrow([] { FootprintCollisionChecker invalid({{0.0, 0.0}, {1.0, 0.0}}); });
        const FootprintCollisionChecker checker(
            {{-0.1, -0.1}, {0.1, -0.1}, {0.1, 0.1}, {-0.1, 0.1}});
        GridMap map(makeGrid(10, 10, 1.0));
        assert(!checker.poseFree(map, {1.0, 1.0, 0.0}, -0.01));
        assert(!checker.commandFree(map, {1.0, 1.0, 0.0}, {0.0, 0.0}, -1.0));
        assert(near(checker.radius(), std::hypot(0.1, 0.1)));
        assert(checker.footprint().size() == 4U);
    }

    std::cout << "PASS polygon footprint collision and swept-motion checks\n";
    return 0;
}

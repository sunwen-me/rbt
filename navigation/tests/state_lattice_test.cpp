#include <rbt_navigation/planner/state_lattice.hpp>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <stop_token>
#include <utility>
#include <vector>

using namespace rbt::navigation;

namespace {

constexpr double kResolution = 0.05;
const std::vector<FootprintPoint> kTestFootprint{
    {-0.045, -0.035}, {0.045, -0.035}, {0.045, 0.035}, {-0.045, 0.035}};

OccupancyGrid makeGrid(std::size_t width, std::size_t height,
                       double resolution = kResolution) {
    OccupancyGrid grid;
    grid.width = width;
    grid.height = height;
    grid.resolution = resolution;
    grid.cells.assign(width * height, 0);
    return grid;
}

MapSnapshot makeSnapshot(OccupancyGrid grid, std::uint64_t version = 17) {
    return {std::make_shared<GridMap>(std::move(grid)), version, 0};
}

bool near(double actual, double expected, double tolerance = 1e-8) {
    return std::abs(actual - expected) <= tolerance;
}

Pose2D localToWorld(const Pose2D& origin, double x, double y,
                    double yaw = 0.0) {
    const double c = std::cos(origin.yaw);
    const double s = std::sin(origin.yaw);
    return {origin.x + c * x - s * y,
            origin.y + s * x + c * y,
            normalizeAngle(origin.yaw + yaw)};
}

// Check the local differential-drive constraint independently of the planner:
// the displacement of one constant-curvature sample interval lies along its
// midpoint body heading. In-place rotation intervals have no translation.
void assertDifferentialDrivePath(const Path2D& path) {
    assert(path.poses.size() >= 2U);
    for (std::size_t i = 1; i < path.poses.size(); ++i) {
        const Pose2D& a = path.poses[i - 1U];
        const Pose2D& b = path.poses[i];
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const double yaw_delta = normalizeAngle(b.yaw - a.yaw);
        const double length = std::hypot(dx, dy);
        if (length < 1e-9) continue;
        const double middle_yaw = normalizeAngle(a.yaw + yaw_delta * 0.5);
        const double lateral = -std::sin(middle_yaw) * dx +
                               std::cos(middle_yaw) * dy;
        // The checked-in Nav2 control set quantizes its samples: independently
        // measured maximum midpoint slip is 1.0471% of one sample chord.
        assert(std::abs(lateral) <= std::max(5e-5, length * 0.011));
    }
}

void assertExactGoalConnectorHasNoSidewaysSegment(const Path2D& path) {
    for (std::size_t i = path.poses.size(); i > 1U; --i) {
        const Pose2D& a = path.poses[i - 2U];
        const Pose2D& b = path.poses[i - 1U];
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        if (std::hypot(dx, dy) < 1e-8) continue;
        const double yaw_delta = normalizeAngle(b.yaw - a.yaw);
        const double middle_yaw = normalizeAngle(a.yaw + yaw_delta * 0.5);
        const double lateral = -std::sin(middle_yaw) * dx +
                               std::cos(middle_yaw) * dy;
        assert(std::abs(lateral) <= 1e-6);
        return;
    }
}

struct Cell {
    double x{};
    double y{};
};

std::vector<Cell> occupiedCells(const GridMap& map) {
    std::vector<Cell> cells;
    for (std::size_t y = 0; y < map.height(); ++y) {
        for (std::size_t x = 0; x < map.width(); ++x) {
            if (map.cellOccupied(static_cast<int>(x), static_cast<int>(y))) {
                const Pose2D center = map.gridToWorld(static_cast<int>(x),
                                                      static_cast<int>(y));
                cells.push_back({center.x, center.y});
            }
        }
    }
    return cells;
}

bool obbOverlapsCell(const Pose2D& pose,
                     const std::vector<FootprintPoint>& footprint,
                     const Cell& cell, double resolution, double padding) {
    std::vector<Cell> polygon;
    const double c = std::cos(pose.yaw);
    const double s = std::sin(pose.yaw);
    for (const FootprintPoint& point : footprint) {
        polygon.push_back({pose.x + c * point.x - s * point.y,
                           pose.y + s * point.x + c * point.y});
    }
    const double half = resolution * 0.5 + padding;
    const Cell square[] = {{cell.x - half, cell.y - half},
                           {cell.x + half, cell.y - half},
                           {cell.x + half, cell.y + half},
                           {cell.x - half, cell.y + half}};
    const auto separated = [&](double axis_x, double axis_y) {
        double polygon_min = std::numeric_limits<double>::infinity();
        double polygon_max = -std::numeric_limits<double>::infinity();
        double square_min = std::numeric_limits<double>::infinity();
        double square_max = -std::numeric_limits<double>::infinity();
        for (const Cell& point : polygon) {
            const double projection = point.x * axis_x + point.y * axis_y;
            polygon_min = std::min(polygon_min, projection);
            polygon_max = std::max(polygon_max, projection);
        }
        for (const Cell& point : square) {
            const double projection = point.x * axis_x + point.y * axis_y;
            square_min = std::min(square_min, projection);
            square_max = std::max(square_max, projection);
        }
        return polygon_max < square_min || square_max < polygon_min;
    };
    if (separated(1.0, 0.0) || separated(0.0, 1.0)) return false;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Cell& a = polygon[i];
        const Cell& b = polygon[(i + 1U) % polygon.size()];
        const double edge_x = b.x - a.x;
        const double edge_y = b.y - a.y;
        if (separated(-edge_y, edge_x)) return false;
    }
    return true;
}

bool independentlyPoseFree(const Pose2D& pose, const GridMap& map,
                            const std::vector<FootprintPoint>& footprint,
                            const std::vector<Cell>& occupied,
                            double padding = 0.0) {
    const double c = std::cos(pose.yaw);
    const double s = std::sin(pose.yaw);
    for (const FootprintPoint& point : footprint) {
        const double x = pose.x + c * point.x - s * point.y;
        const double y = pose.y + s * point.x + c * point.y;
        if (!map.worldToGrid(x, y)) return false;
    }
    for (const Cell& cell : occupied) {
        if (obbOverlapsCell(pose, footprint, cell, map.resolution(), padding)) {
            return false;
        }
    }
    return true;
}

Pose2D interpolate(const Pose2D& a, const Pose2D& b, double t) {
    const double delta = normalizeAngle(b.yaw - a.yaw);
    return {(1.0 - t) * a.x + t * b.x,
            (1.0 - t) * a.y + t * b.y,
            normalizeAngle(a.yaw + t * delta)};
}

void assertIndependentCollisionFree(const Path2D& path, const GridMap& map,
                                    const std::vector<FootprintPoint>& footprint) {
    const std::vector<Cell> occupied = occupiedCells(map);
    double radius = 0.0;
    for (const FootprintPoint& point : footprint) {
        radius = std::max(radius, std::hypot(point.x, point.y));
    }
    for (std::size_t i = 0; i < path.poses.size(); ++i) {
        if (i == 0U) {
            assert(independentlyPoseFree(path.poses[i], map, footprint, occupied));
            continue;
        }
        const Pose2D& a = path.poses[i - 1U];
        const Pose2D& b = path.poses[i];
        const double movement = distance(a, b) + radius *
            std::abs(normalizeAngle(b.yaw - a.yaw));
        const std::size_t steps = std::max<std::size_t>(
            1U, static_cast<std::size_t>(std::ceil(movement / 0.0025)));
        for (std::size_t step = 1; step <= steps; ++step) {
            const Pose2D sample = interpolate(
                a, b, static_cast<double>(step) / static_cast<double>(steps));
            assert(independentlyPoseFree(sample, map, footprint, occupied, 0.00125));
        }
    }
}

std::filesystem::path controlPath() {
    return std::filesystem::path(__FILE__).parent_path().parent_path() /
           "config/lattice/diff-5cm-0_5m.json";
}

} // namespace

int main() {
    LatticeControlSet controls = loadLatticeControlSet(controlPath().string());
    assert(controls.headings.size() == 16U);
    assert(controls.primitives.size() == 112U);
    assert(near(controls.resolution, 0.05));

    auto checker = std::make_shared<FootprintCollisionChecker>(kTestFootprint);
    {
        StateLatticeConfig config;
        config.timeout = std::chrono::seconds(5);
        config.goal_tolerance = 0.18;
        StateLatticePlanner planner(controls, checker, config);
        const MapSnapshot map = makeSnapshot(makeGrid(120, 120));
        const Pose2D start{2.5, 2.5, 0.0};
        const Pose2D goal{3.15, 2.9, 0.35};
        const PlannerResult result = planner.plan(start, goal, map);
        assert(result.success());
        assert(result.path.map_version == map.version);
        assert(near(result.path.poses.front().x, start.x));
        assert(near(result.path.poses.front().y, start.y));
        assert(near(result.path.poses.front().yaw, start.yaw));
        assert(near(result.path.poses.back().x, goal.x));
        assert(near(result.path.poses.back().y, goal.y));
        assert(near(result.path.poses.back().yaw, goal.yaw));
        assertDifferentialDrivePath(result.path);
        assertIndependentCollisionFree(result.path, *map.grid, kTestFootprint);
        assertExactGoalConnectorHasNoSidewaysSegment(result.path);
    }
    {
        StateLatticeConfig config;
        config.timeout = std::chrono::seconds(5);
        config.goal_tolerance = 0.12;
        StateLatticePlanner planner(controls, checker, config);
        OccupancyGrid grid = makeGrid(120, 120);
        grid.cells[50U * grid.width + 52U] = 100;
        const MapSnapshot map = makeSnapshot(std::move(grid));
        const Pose2D start{1.5, 2.5, 0.0};
        const Pose2D goal{3.0, 2.5, 0.0};
        const PlannerResult result = planner.plan(start, goal, map);
        assert(result.success());
        assertDifferentialDrivePath(result.path);
        assertIndependentCollisionFree(result.path, *map.grid, kTestFootprint);
        assertExactGoalConnectorHasNoSidewaysSegment(result.path);
    }
    {
        StateLatticeConfig config;
        config.timeout = std::chrono::seconds(5);
        config.goal_tolerance = 0.1;
        StateLatticePlanner planner(controls, checker, config);
        const MapSnapshot map = makeSnapshot(makeGrid(100, 100));
        const Pose2D start{2.5234, 2.5171, 0.0};
        const Pose2D goal{3.1234, 2.5171, 0.0};
        const PlannerResult result = planner.plan(start, goal, map);
        assert(result.success());
        assert(result.path.poses.front().x == start.x);
        assert(result.path.poses.front().y == start.y);
        assert(result.path.poses.front().yaw == start.yaw);
        assertDifferentialDrivePath(result.path);
    }
    {
        StateLatticeConfig config;
        config.timeout = std::chrono::seconds(2);
        config.goal_tolerance = 0.05;
        StateLatticePlanner planner(controls, checker, config);
        const MapSnapshot map = makeSnapshot(makeGrid(80, 80));
        const Pose2D start{2.0, 2.0, 0.0};
        const Pose2D goal{2.0, 2.0, std::numbers::pi / 2.0};
        const PlannerResult result = planner.plan(start, goal, map);
        assert(result.success());
        assert(result.path.poses.size() >= 2U);
        assert(near(result.path.poses.front().x, result.path.poses.back().x));
        assert(near(result.path.poses.front().y, result.path.poses.back().y));
        assert(near(result.path.poses.back().yaw, goal.yaw));
        bool kept_same_position_rotation = false;
        for (std::size_t i = 1; i < result.path.poses.size(); ++i) {
            if (near(result.path.poses[i - 1U].x, result.path.poses[i].x) &&
                near(result.path.poses[i - 1U].y, result.path.poses[i].y) &&
                std::abs(normalizeAngle(result.path.poses[i].yaw -
                                        result.path.poses[i - 1U].yaw)) > 1e-4) {
                kept_same_position_rotation = true;
            }
        }
        assert(kept_same_position_rotation);
    }
    {
        StateLatticeConfig config;
        config.timeout = std::chrono::seconds(2);
        config.goal_tolerance = 0.1;
        config.max_iterations = 1U;
        StateLatticePlanner planner(controls, checker, config);
        const MapSnapshot map = makeSnapshot(makeGrid(80, 80));
        const Pose2D start{2.0, 2.0, 0.0};
        const Pose2D goal{2.05, 2.0, 0.0};
        const PlannerResult result = planner.plan(start, goal, map);
        assert(result.success());
        assert(result.expanded == 1U);
        assert(near(result.path.poses.front().x, start.x));
        assert(near(result.path.poses.back().x, goal.x));
        assert(near(result.path.poses.back().yaw, goal.yaw));
        assertDifferentialDrivePath(result.path);
    }
    {
        StateLatticeConfig config;
        config.timeout = std::chrono::seconds(5);
        config.goal_tolerance = 0.1;
        config.allow_reverse_expansion = true;
        StateLatticePlanner planner(controls, checker, config);
        const MapSnapshot map = makeSnapshot(makeGrid(120, 120));
        const Pose2D start{3.0, 3.0, 0.0};
        const Pose2D goal{2.0, 3.0, 0.0};
        const PlannerResult result = planner.plan(start, goal, map);
        assert(result.success());
        bool backed_while_facing_forward = false;
        for (std::size_t i = 1; i < result.path.poses.size(); ++i) {
            const Pose2D& a = result.path.poses[i - 1U];
            const Pose2D& b = result.path.poses[i];
            const double dx = b.x - a.x;
            const double dy = b.y - a.y;
            const double yaw = normalizeAngle(a.yaw +
                normalizeAngle(b.yaw - a.yaw) * 0.5);
            if (dx * std::cos(yaw) + dy * std::sin(yaw) < -0.02 &&
                std::abs(normalizeAngle(yaw)) < 0.15) {
                backed_while_facing_forward = true;
            }
        }
        assert(backed_while_facing_forward);
        assertDifferentialDrivePath(result.path);
    }
    {
        StateLatticeConfig config;
        config.timeout = std::chrono::seconds(5);
        config.goal_tolerance = 0.1;
        config.allow_reverse_expansion = false;
        StateLatticePlanner planner(controls, checker, config);
        const MapSnapshot map = makeSnapshot(makeGrid(120, 120));
        const Pose2D start{3.0, 3.0, 0.0};
        const Pose2D goal{2.0, 3.0, 0.0};
        const PlannerResult result = planner.plan(start, goal, map);
        assert(result.success());
        for (std::size_t i = 1; i < result.path.poses.size(); ++i) {
            const Pose2D& a = result.path.poses[i - 1U];
            const Pose2D& b = result.path.poses[i];
            const double dx = b.x - a.x;
            const double dy = b.y - a.y;
            const double yaw = normalizeAngle(a.yaw +
                normalizeAngle(b.yaw - a.yaw) * 0.5);
            if (std::hypot(dx, dy) > 0.01) {
                assert(dx * std::cos(yaw) + dy * std::sin(yaw) >= -1e-4);
            }
        }
        assertDifferentialDrivePath(result.path);
    }
    {
        StateLatticeConfig config;
        config.timeout = std::chrono::seconds(5);
        config.goal_tolerance = 0.15;
        StateLatticePlanner planner(controls, checker, config);
        OccupancyGrid grid = makeGrid(100, 100);
        grid.origin = {10.0, -4.0, 0.7};
        const MapSnapshot map = makeSnapshot(std::move(grid));
        const Pose2D start = localToWorld(map.grid->origin(), 2.0, 2.0);
        const Pose2D goal = localToWorld(map.grid->origin(), 2.6, 2.0);
        const PlannerResult result = planner.plan(start, goal, map);
        assert(result.success());
        assert(near(result.path.poses.back().x, goal.x));
        assert(near(result.path.poses.back().y, goal.y));
        assertDifferentialDrivePath(result.path);
    }
    {
        OccupancyGrid grid = makeGrid(120, 60);
        std::fill(grid.cells.begin(), grid.cells.end(), 100);
        for (std::size_t y = 27; y <= 32; ++y) {
            for (std::size_t x = 0; x < grid.width; ++x) {
                grid.cells[y * grid.width + x] = 0;
            }
        }
        const MapSnapshot map = makeSnapshot(std::move(grid));
        const std::vector<FootprintPoint> long_body{
            {-0.32, -0.05}, {0.32, -0.05}, {0.32, 0.05}, {-0.32, 0.05}};
        auto long_checker = std::make_shared<FootprintCollisionChecker>(long_body);
        const Pose2D start{3.0, 1.5, 0.0};
        const Pose2D goal{3.0, 1.5, std::numbers::pi};
        const std::vector<Cell> occupied = occupiedCells(*map.grid);
        assert(independentlyPoseFree(start, *map.grid, long_body, occupied));
        assert(independentlyPoseFree(goal, *map.grid, long_body, occupied));
        assert(!independentlyPoseFree({3.0, 1.5, std::numbers::pi / 2.0},
                                      *map.grid, long_body, occupied));
        assert(!long_checker->motionFree(*map.grid, start, goal));
        StateLatticeConfig config;
        config.timeout = std::chrono::seconds(5);
        config.goal_tolerance = 0.05;
        StateLatticePlanner planner(controls, long_checker, config);
        const PlannerResult result = planner.plan(start, goal, map);
        assert(result.error == ErrorCode::NoPath);
    }
    {
        StateLatticeConfig config;
        config.timeout = std::chrono::seconds(2);
        config.goal_tolerance = 0.1;
        const MapSnapshot map = makeSnapshot(makeGrid(40, 40));
        StateLatticePlanner planner(controls, checker, config);
        std::stop_source source;
        source.request_stop();
        assert(planner.plan({1.0, 1.0, 0.0}, {1.5, 1.0, 0.0}, map,
                            source.get_token()).error == ErrorCode::Canceled);

        config.timeout = std::chrono::nanoseconds(1);
        StateLatticePlanner short_timeout(controls, checker, config);
        assert(short_timeout.plan({1.0, 1.0, 0.0}, {1.5, 1.0, 0.0}, map).error ==
               ErrorCode::PlanningTimeout);

        const MapSnapshot wrong_resolution = makeSnapshot(makeGrid(40, 40, 0.1));
        assert(planner.plan({1.0, 1.0, 0.0}, {1.5, 1.0, 0.0},
                            wrong_resolution).error == ErrorCode::InvalidInput);
    }

    std::cout << "official diff lattice, differential paths, reverse, goal connection, "
                 "rotated map, swept tail collision and cancellation: PASS\n";
}

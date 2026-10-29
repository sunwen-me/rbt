#include <rbt_navigation/map/grid_map.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <limits>
#include <numbers>
#include <stdexcept>

using namespace rbt::navigation;

namespace {

OccupancyGrid makeGrid(std::size_t width, std::size_t height, double resolution = 1.0) {
    OccupancyGrid grid;
    grid.width = width;
    grid.height = height;
    grid.resolution = resolution;
    grid.cells.assign(width * height, 0);
    return grid;
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
        auto grid = makeGrid(4, 3, 0.5);
        grid.origin = {2.0, -1.0, std::numbers::pi / 2.0};
        GridMap map(std::move(grid));
        const Pose2D center = map.gridToWorld(1, 2);
        const auto cell = map.worldToGrid(center.x, center.y);
        assert(cell && cell->first == 1 && cell->second == 2);
        assert(std::abs(center.x - 0.75) < 1e-12);
        assert(std::abs(center.y + 0.25) < 1e-12);
    }
    {
        GridMap map(makeGrid(4, 3));
        assert((map.worldToGrid(0.0, 0.0) == std::pair<int, int>{0, 0}));
        assert((map.worldToGrid(3.999, 2.999) == std::pair<int, int>{3, 2}));
        assert(!map.worldToGrid(4.0, 1.0));
        assert(!map.worldToGrid(-0.001, 1.0));
        assert(!map.worldToGrid(std::numeric_limits<double>::quiet_NaN(), 0.0));
        assert(!map.isFree(4.0, 1.0, 0.0));
        assert(map.isFree(2.5, 1.5, 0.0));
    }
    {
        auto grid = makeGrid(3, 2);
        grid.cells[1] = -1;
        GridMap map(std::move(grid));
        assert(map.cellOccupied(1, 0));
        assert(map.isOccupied(1.5, 0.5));
        assert(!map.isFree(1.5, 0.5, 0.0));
        assert(!map.isFree(0.5, 0.5, -0.1));
        assert(map.isOccupied(-0.01, 0.5));
        assert(map.getDistanceToObstacle(-0.01, 0.5) == 0.0);
        assert(map.cellOccupied(-1, 0));
    }
    {
        auto grid = makeGrid(7, 7, 0.5);
        grid.cells[3 * grid.width + 3] = 100;
        GridMap map(std::move(grid));
        assert(std::abs(map.signedDistance(3, 3) + 0.5) < 1e-12);
        assert(std::abs(map.signedDistance(4, 3) - 0.5) < 1e-12);
        assert(std::abs(map.signedDistance(5, 4) - std::sqrt(5.0) * 0.5) < 1e-12);
        assert(map.getDistanceToObstacle(3.5 * 0.5, 3.5 * 0.5) == 0.0);
    }
    {
        std::uint64_t random_state = 0x6a09e667f3bcc909ULL;
        const auto random = [&random_state]() {
            random_state ^= random_state << 13;
            random_state ^= random_state >> 7;
            random_state ^= random_state << 17;
            return random_state;
        };
        const double yaws[] = {-2.1, -0.45, 0.0, 0.73, 1.8};
        for (int trial = 0; trial < 48; ++trial) {
            const std::size_t width = 2 + static_cast<std::size_t>(random() % 7);
            const std::size_t height = 2 + static_cast<std::size_t>(random() % 7);
            const double resolution = static_cast<double>(2 + random() % 11) / 10.0;
            auto grid = makeGrid(width, height, resolution);
            grid.occupied_threshold = static_cast<std::int8_t>(1 + random() % 100);
            grid.origin = {static_cast<double>(static_cast<int>(random() % 13) - 6) * 0.4,
                           static_cast<double>(static_cast<int>(random() % 13) - 6) * 0.3,
                           yaws[random() % std::size(yaws)]};
            for (auto& value : grid.cells) {
                const std::uint64_t sample = random() % 8;
                value = sample == 0 ? -1 :
                        sample < 4 ? 0 :
                        static_cast<std::int8_t>(random() % 101);
            }
            grid.cells.front() = 0;
            grid.cells.back() = 100;
            GridMap map(std::move(grid));

            for (std::size_t y = 0; y < height; ++y) {
                for (std::size_t x = 0; x < width; ++x) {
                    const bool occupied = map.cellOccupied(static_cast<int>(x), static_cast<int>(y));
                    double nearest_opposite = std::numeric_limits<double>::infinity();
                    for (std::size_t other_y = 0; other_y < height; ++other_y) {
                        for (std::size_t other_x = 0; other_x < width; ++other_x) {
                            if (map.cellOccupied(static_cast<int>(other_x),
                                                 static_cast<int>(other_y)) == occupied) {
                                continue;
                            }
                            const double dx = static_cast<double>(x) - static_cast<double>(other_x);
                            const double dy = static_cast<double>(y) - static_cast<double>(other_y);
                            nearest_opposite = std::min(nearest_opposite,
                                                        std::hypot(dx, dy) * resolution);
                        }
                    }
                    const double expected = occupied ? -nearest_opposite : nearest_opposite;
                    const double actual = map.signedDistance(static_cast<int>(x),
                                                             static_cast<int>(y));
                    if (std::isinf(expected)) {
                        assert(std::isinf(actual) && std::signbit(actual) == std::signbit(expected));
                    } else {
                        assert(std::abs(actual - expected) <=
                               1e-12 * std::max(1.0, std::abs(expected)));
                    }
                }
            }

            const Pose2D origin = map.gridToWorld(0, 0);
            const double c = std::cos(origin.yaw);
            const double s = std::sin(origin.yaw);
            const double map_width = static_cast<double>(width) * resolution;
            const double map_height = static_cast<double>(height) * resolution;
            // Recover the actual grid origin from the first cell center.
            const double origin_x = origin.x - c * resolution * 0.5 + s * resolution * 0.5;
            const double origin_y = origin.y - s * resolution * 0.5 - c * resolution * 0.5;
            for (int sample = 0; sample < 32; ++sample) {
                const double fraction_x = (static_cast<double>(random() % 1'000'000) + 0.5) /
                                          1'000'000.0;
                const double fraction_y = (static_cast<double>(random() % 1'000'000) + 0.5) /
                                          1'000'000.0;
                const double local_x = fraction_x * map_width;
                const double local_y = fraction_y * map_height;
                const double world_x = origin_x + c * local_x - s * local_y;
                const double world_y = origin_y + s * local_x + c * local_y;
                const auto cell = map.worldToGrid(world_x, world_y);
                assert(cell);
                if (map.cellOccupied(cell->first, cell->second)) continue;

                double exact_clearance = std::min(
                    std::min(local_x, map_width - local_x),
                    std::min(local_y, map_height - local_y));
                for (std::size_t y = 0; y < height; ++y) {
                    for (std::size_t x = 0; x < width; ++x) {
                        if (!map.cellOccupied(static_cast<int>(x), static_cast<int>(y))) continue;
                        const double left = static_cast<double>(x) * resolution;
                        const double right = left + resolution;
                        const double bottom = static_cast<double>(y) * resolution;
                        const double top = bottom + resolution;
                        const double dx = std::max({left - local_x, 0.0, local_x - right});
                        const double dy = std::max({bottom - local_y, 0.0, local_y - top});
                        exact_clearance = std::min(exact_clearance, std::hypot(dx, dy));
                    }
                }
                const double clearance = map.getDistanceToObstacle(world_x, world_y);
                const double tolerance = 1e-10 * std::max(1.0, exact_clearance);
                assert(clearance >= 0.0);
                assert(clearance <= exact_clearance + tolerance);
            }
        }
    }
    {
        auto grid = makeGrid(10, 10);
        grid.cells[4 * grid.width + 4] = 100;
        GridMap map(std::move(grid));
        const double half_diagonal = std::sqrt(0.5);
        assert(std::abs(map.getDistanceToObstacle(6.5, 4.5) - (2.0 - half_diagonal)) < 1e-12);
        assert(std::abs(map.getDistanceToObstacle(6.9, 4.5) - (2.0 - half_diagonal - 0.4)) < 1e-12);
    }
    {
        GridMap map(makeGrid(3, 3));
        assert(std::abs(map.getDistanceToObstacle(0.5, 1.5) - 0.5) < 1e-12);
        assert(map.isFree(0.5, 1.5, 0.49));
        assert(!map.isFree(0.5, 1.5, 0.51));
        assert(map.segmentFree({0.5, 1.5, 0.0}, {2.5, 1.5, 0.0}, 0.25));

        auto blocked = makeGrid(3, 3);
        blocked.cells[1 * blocked.width + 1] = 100;
        GridMap obstacle_map(std::move(blocked));
        assert(!obstacle_map.segmentFree({0.5, 0.5, 0.0}, {2.5, 2.5, 0.0}, 0.0));
    }
    {
        auto bad_dimensions = makeGrid(0, 1);
        mustThrow([&] { GridMap map(std::move(bad_dimensions)); });

        auto bad_count = makeGrid(2, 2);
        bad_count.cells.pop_back();
        mustThrow([&] { GridMap map(std::move(bad_count)); });

        auto bad_value = makeGrid(1, 1);
        bad_value.cells[0] = 101;
        mustThrow([&] { GridMap map(std::move(bad_value)); });

        auto bad_threshold = makeGrid(1, 1);
        bad_threshold.occupied_threshold = 101;
        mustThrow([&] { GridMap map(std::move(bad_threshold)); });

        auto too_large = makeGrid(4'000'001, 1);
        mustThrow([&] { GridMap map(std::move(too_large)); });

        auto bad_geometry = makeGrid(1, 1);
        bad_geometry.resolution = std::numeric_limits<double>::infinity();
        mustThrow([&] { GridMap map(std::move(bad_geometry)); });

        auto overflowing_bounds = makeGrid(1, 1);
        overflowing_bounds.resolution = std::numeric_limits<double>::max();
        mustThrow([&] { GridMap map(std::move(overflowing_bounds)); });
    }
    std::cout << "occupancy conversion, rotated geometry, exact EDT and conservative clearance: PASS\n";
}

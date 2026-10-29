#include <rbt_navigation/map/obstacle_layer.hpp>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <vector>

using namespace rbt::navigation;

namespace {

OccupancyGrid makeGrid(std::size_t width, std::size_t height,
                       double resolution = 1.0) {
    OccupancyGrid grid;
    grid.width = width;
    grid.height = height;
    grid.resolution = resolution;
    grid.cells.assign(width * height, 0);
    return grid;
}

ObstaclePoint cellCenter(const OccupancyGrid& grid, int x, int y, double z) {
    const double local_x = (static_cast<double>(x) + 0.5) * grid.resolution;
    const double local_y = (static_cast<double>(y) + 0.5) * grid.resolution;
    const double cosine = std::cos(grid.origin.yaw);
    const double sine = std::sin(grid.origin.yaw);
    return {grid.origin.x + cosine * local_x - sine * local_y,
            grid.origin.y + sine * local_x + cosine * local_y, z};
}

Point3 sensorAtCell(const OccupancyGrid& grid, int x, int y, double z) {
    const ObstaclePoint center = cellCenter(grid, x, y, z);
    return {center.x, center.y, center.z};
}

template <typename Function>
void mustThrow(Function&& function) {
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
        // A return just outside the default 18 cm body must remain visible.
        const auto grid = makeGrid(20, 20, 0.05);
        ObstacleLayer layer(grid);
        const std::vector<ObstaclePoint> near_body{{0.725, 0.525, 0.5}};
        assert(layer.integrate(near_body, {0.525, 0.525, 0.5}, {0.525, 0.525, 0}, 0));
        assert(layer.combinedGrid().cells[10 * grid.width + 14] == 100);
    }
    {
        const auto grid = makeGrid(8, 3);
        ObstacleLayer layer(grid);
        const Point3 sensor{0.5, 1.5, 1.5};
        const Pose2D base{0.5, 1.5, 0.0};
        const std::vector<ObstaclePoint> valid{{3.5, 1.5, 1.5}};
        assert(layer.integrate(valid, sensor, base, 0.5));
        assert(layer.combinedGrid().cells[1 * grid.width + 3] == 100);

        const double nan = std::numeric_limits<double>::quiet_NaN();
        const std::vector<ObstaclePoint> filtered{
            {4.5, 1.5, 0.55},  // ground
            {4.5, 1.5, 2.1},   // above the height band
            {0.5, 1.5, 1.5},   // inside the robot body
            {7.5, 1.5, 1.5},   // beyond max_range
            {nan, 1.5, 1.5}};  // nonfinite endpoint
        ObstacleLayerConfig config;
        config.max_range = 4.0;
        ObstacleLayer filtered_layer(grid, config);
        assert(filtered_layer.integrate(valid, sensor, base, 0.5));
        assert(!filtered_layer.integrate(filtered, sensor, base, 0.5));
        assert(filtered_layer.combinedGrid().cells[1 * grid.width + 3] == 100);
        assert(filtered_layer.combinedGrid().cells[1 * grid.width + 4] == 0);

        // A height-invalid sensor may contribute a valid hit, but no free ray.
        ObstacleLayer sensor_height_layer(grid);
        const std::vector<ObstaclePoint> old_hit{{2.5, 1.5, 1.0}};
        assert(sensor_height_layer.integrate(old_hit, {0.5, 1.5, 1.0},
                                            {0.5, 1.5, 0.0}, 0.0));
        const std::vector<ObstaclePoint> high_sensor_hit{{5.5, 1.5, 1.0}};
        assert(sensor_height_layer.integrate(high_sensor_hit, {0.5, 1.5, 2.0},
                                             {0.5, 1.5, 0.0}, 0.0));
        assert(sensor_height_layer.combinedGrid().cells[1 * grid.width + 2] == 100);
        assert(sensor_height_layer.combinedGrid().cells[1 * grid.width + 5] == 100);
    }
    {
        auto grid = makeGrid(8, 3);
        grid.cells[1 * grid.width + 2] = 80;
        grid.cells[1 * grid.width + 3] = -1;
        const auto static_values = grid.cells;
        ObstacleLayer layer(grid);
        const std::vector<ObstaclePoint> points{
            {2.5, 1.5, 1.0},  // static occupied endpoint
            {3.5, 1.5, 1.0},  // unknown endpoint
            {6.5, 1.5, 1.0}};
        assert(layer.integrate(points, {0.5, 1.5, 1.0},
                              {0.5, 1.5, 0.0}, 0.0));
        assert(layer.combinedGrid().cells[1 * grid.width + 2] == 80);
        assert(layer.combinedGrid().cells[1 * grid.width + 3] == -1);
        assert(layer.combinedGrid().cells[1 * grid.width + 6] == 100);
        assert(layer.combinedGrid().cells[1 * grid.width + 2] == static_values[1 * grid.width + 2]);
        assert(layer.combinedGrid().cells[1 * grid.width + 3] == static_values[1 * grid.width + 3]);
    }
    {
        auto grid = makeGrid(5, 3, 0.5);
        grid.origin = {10.0, -2.0, std::numbers::pi / 2.0};
        ObstacleLayer layer(grid);
        const ObstaclePoint hit = cellCenter(grid, 4, 1, 1.0);
        const Point3 sensor = sensorAtCell(grid, 0, 1, 1.0);
        const Pose2D base{sensor.x, sensor.y, 0.0};
        const std::vector<ObstaclePoint> points{hit};
        assert(layer.integrate(points, sensor, base, 0.0));
        assert(layer.combinedGrid().cells[1 * grid.width + 4] == 100);
        assert(layer.combinedGrid().cells[1 * grid.width] == 0);
    }
    {
        const auto grid = makeGrid(8, 3);
        ObstacleLayer layer(grid);
        const Point3 sensor{0.5, 1.5, 1.0};
        const Pose2D base{0.5, 1.5, 0.0};
        const std::vector<ObstaclePoint> first_hit{{3.5, 1.5, 1.0}};
        assert(layer.integrate(first_hit, sensor, base, 0.0));
        assert(layer.combinedGrid().cells[1 * grid.width + 3] == 100);
        assert(!layer.integrate({}, sensor, base, 0.0));
        assert(layer.combinedGrid().cells[1 * grid.width + 3] == 100);

        const std::vector<ObstaclePoint> clearing_ray{{6.5, 1.5, 1.0}};
        assert(layer.integrate(clearing_ray, sensor, base, 0.0));
        assert(layer.combinedGrid().cells[1 * grid.width + 3] == 0);
        assert(layer.combinedGrid().cells[1 * grid.width + 6] == 100);
    }
    {
        const auto grid = makeGrid(8, 3);
        ObstacleLayer layer(grid);
        const std::vector<ObstaclePoint> seeded_hit{{4.5, 1.5, 1.0}};
        assert(layer.integrate(seeded_hit, {7.5, 1.5, 1.0},
                               {7.5, 1.5, 0.0}, 0.0));
        assert(layer.combinedGrid().cells[1 * grid.width + 4] == 100);

        // The nearer return at x=2 blocks both rays from clearing the old hit
        // at x=4, while both current endpoints remain occupied.
        const std::vector<ObstaclePoint> returns{
            {2.5, 1.5, 1.0}, {6.5, 1.5, 1.0}};
        assert(layer.integrate(returns, {0.5, 1.5, 1.0},
                               {0.5, 1.5, 0.0}, 0.0));
        assert(layer.combinedGrid().cells[1 * grid.width + 2] == 100);
        assert(layer.combinedGrid().cells[1 * grid.width + 4] == 100);
        assert(layer.combinedGrid().cells[1 * grid.width + 6] == 100);
    }
    {
        auto grid = makeGrid(8, 3);
        grid.cells[1 * grid.width + 3] = 80;
        ObstacleLayer layer(grid);
        const std::vector<ObstaclePoint> seeded_hit{{5.5, 1.5, 1.0}};
        assert(layer.integrate(seeded_hit, {7.5, 1.5, 1.0},
                               {7.5, 1.5, 0.0}, 0.0));
        assert(layer.combinedGrid().cells[1 * grid.width + 5] == 100);

        const std::vector<ObstaclePoint> farther_return{{6.5, 1.5, 1.0}};
        assert(layer.integrate(farther_return, {0.5, 1.5, 1.0},
                               {0.5, 1.5, 0.0}, 0.0));
        assert(layer.combinedGrid().cells[1 * grid.width + 3] == 80);
        assert(layer.combinedGrid().cells[1 * grid.width + 5] == 100);
        assert(layer.combinedGrid().cells[1 * grid.width + 6] == 100);
    }
    {
        auto bad_config = ObstacleLayerConfig{};
        bad_config.min_height = -0.1;
        mustThrow([&] { ObstacleLayer layer(makeGrid(1, 1), bad_config); });
        bad_config = {};
        bad_config.max_points = 200'001;
        mustThrow([&] { ObstacleLayer layer(makeGrid(1, 1), bad_config); });

        auto bad_grid = makeGrid(2, 2);
        bad_grid.cells.pop_back();
        mustThrow([&] { ObstacleLayer layer(std::move(bad_grid)); });
        mustThrow([&] { ObstacleLayer layer(makeGrid(1001, 1000)); });

        const auto grid = makeGrid(4, 2);
        ObstacleLayerConfig config;
        config.max_points = 2;
        ObstacleLayer layer(grid, config);
        const std::vector<ObstaclePoint> too_many(3, {2.5, 0.5, 1.0});
        mustThrow([&] {
            (void)layer.integrate(too_many, {0.5, 0.5, 1.0},
                                 {0.5, 0.5, 0.0}, 0.0);
        });
        mustThrow([&] {
            (void)layer.integrate({}, {0.5, 0.5, 1.0},
                                 {std::numeric_limits<double>::quiet_NaN(), 0.5, 0.0}, 0.0);
        });
        mustThrow([&] {
            (void)layer.integrate({}, {0.5, 0.5,
                                      std::numeric_limits<double>::infinity()},
                                 {0.5, 0.5, 0.0}, 0.0);
        });
        assert(layer.combinedGrid().cells == grid.cells);
    }
    std::cout << "standalone obstacle fusion, persistence, conservative clearing and validation: PASS\n";
}

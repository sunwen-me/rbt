#pragma once

#include <rbt_navigation/map/grid_map.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rbt::navigation {

struct Point3 {
    double x{}, y{}, z{};
};

struct ObstaclePoint {
    double x{}, y{}, z{};
};

struct ObstacleLayerConfig {
    double min_height{0.10};
    double max_height{1.5};
    // Match the default navigation footprint; this mask must never extend
    // beyond the measured robot body and hide nearby external obstacles.
    double self_radius{0.18};
    double max_range{10.0};
    std::size_t max_points{200'000};
};

class ObstacleLayer {
public:
    explicit ObstacleLayer(OccupancyGrid static_map,
                           ObstacleLayerConfig config = {});

    // Returns true when the combined occupancy grid changes. Invalid scan
    // geometry or a scan above max_points throws std::invalid_argument.
    [[nodiscard]] bool integrate(std::span<const ObstaclePoint> points,
                                 const Point3& sensor_origin,
                                 const Pose2D& base_pose,
                                 double base_z);

    [[nodiscard]] const OccupancyGrid& combinedGrid() const noexcept {
        return combined_grid_;
    }

private:
    OccupancyGrid static_grid_;
    OccupancyGrid combined_grid_;
    ObstacleLayerConfig config_;
    std::vector<std::uint8_t> dynamic_occupied_;
};

} // namespace rbt::navigation

#include <rbt_navigation/map/obstacle_layer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace rbt::navigation {
namespace {

constexpr std::size_t kMaximumCells = 1'000'000;
constexpr std::size_t kMaximumPoints = 200'000;

struct GridPoint {
    double x{};
    double y{};
};

struct Ray {
    GridPoint endpoint;
};

void validateConfig(const ObstacleLayerConfig& config) {
    if (!std::isfinite(config.min_height) || !std::isfinite(config.max_height) ||
        config.min_height < 0.0 || config.max_height <= config.min_height ||
        !std::isfinite(config.self_radius) || config.self_radius < 0.0 ||
        !std::isfinite(config.max_range) || config.max_range <= 0.0 ||
        config.max_points == 0 || config.max_points > kMaximumPoints) {
        throw std::invalid_argument("obstacle layer configuration is invalid");
    }
}

void validateGrid(const OccupancyGrid& grid) {
    if (grid.width == 0 || grid.height == 0 ||
        grid.width > kMaximumCells / grid.height ||
        grid.width * grid.height > kMaximumCells) {
        throw std::invalid_argument("static occupancy grid dimensions are invalid or exceed one million cells");
    }
    if (!std::isfinite(grid.resolution) || grid.resolution <= 0.0 ||
        !std::isfinite(grid.resolution * 0.25) || grid.resolution * 0.25 == 0.0 ||
        !finite(grid.origin) || grid.occupied_threshold < 0 ||
        grid.occupied_threshold > 100 ||
        grid.cells.size() != grid.width * grid.height) {
        throw std::invalid_argument("static occupancy grid geometry or cell count is invalid");
    }
    if (std::any_of(grid.cells.begin(), grid.cells.end(), [](std::int8_t cell) {
            return cell < -1 || cell > 100;
        })) {
        throw std::invalid_argument("static occupancy grid values must be -1 or in [0, 100]");
    }

    const double map_width = static_cast<double>(grid.width) * grid.resolution;
    const double map_height = static_cast<double>(grid.height) * grid.resolution;
    if (!std::isfinite(map_width) || !std::isfinite(map_height) ||
        !std::isfinite(std::hypot(map_width, map_height))) {
        throw std::invalid_argument("static occupancy grid metric bounds are not finite");
    }
    const double cosine = std::cos(grid.origin.yaw);
    const double sine = std::sin(grid.origin.yaw);
    const double corner_x[] = {
        grid.origin.x, grid.origin.x + cosine * map_width,
        grid.origin.x - sine * map_height,
        grid.origin.x + cosine * map_width - sine * map_height};
    const double corner_y[] = {
        grid.origin.y, grid.origin.y + sine * map_width,
        grid.origin.y + cosine * map_height,
        grid.origin.y + sine * map_width + cosine * map_height};
    for (std::size_t i = 0; i < 4; ++i) {
        if (!std::isfinite(corner_x[i]) || !std::isfinite(corner_y[i])) {
            throw std::invalid_argument("static occupancy grid world bounds are not finite");
        }
    }
}

bool worldToGridCoordinates(const OccupancyGrid& grid, double x, double y,
                            GridPoint& result) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    const double dx = x - grid.origin.x;
    const double dy = y - grid.origin.y;
    const double cosine = std::cos(grid.origin.yaw);
    const double sine = std::sin(grid.origin.yaw);
    const double local_x = cosine * dx + sine * dy;
    const double local_y = -sine * dx + cosine * dy;
    result.x = local_x / grid.resolution;
    result.y = local_y / grid.resolution;
    return std::isfinite(result.x) && std::isfinite(result.y);
}

std::optional<std::size_t> cellAt(const OccupancyGrid& grid,
                                  const GridPoint& point) noexcept {
    if (point.x < 0.0 || point.y < 0.0 ||
        point.x >= static_cast<double>(grid.width) ||
        point.y >= static_cast<double>(grid.height)) {
        return std::nullopt;
    }
    const auto x = static_cast<std::size_t>(std::floor(point.x));
    const auto y = static_cast<std::size_t>(std::floor(point.y));
    return y * grid.width + x;
}

bool staticFree(const OccupancyGrid& grid, std::size_t index) noexcept {
    return grid.cells[index] >= 0 && grid.cells[index] < grid.occupied_threshold;
}

bool inHeightBand(double height, const ObstacleLayerConfig& config) noexcept {
    return std::isfinite(height) && height >= config.min_height &&
           height <= config.max_height;
}

struct CellGroup {
    std::array<std::size_t, 4> cells{};
    std::size_t count{};
};

void addAxisCells(long double coordinate,
                  std::array<long long, 2>& cells, std::size_t& count) noexcept {
    const long double nearest = std::round(coordinate);
    const long double tolerance = 16.0L * std::numeric_limits<double>::epsilon() *
                                  std::max(1.0L, std::abs(coordinate));
    if (std::abs(coordinate - nearest) <= tolerance) {
        const auto boundary = static_cast<long long>(nearest);
        cells[count++] = boundary - 1;
        cells[count++] = boundary;
    } else {
        cells[count++] = static_cast<long long>(std::floor(coordinate));
    }
    if (count == 2 && cells[0] == cells[1]) count = 1;
    if (count > 2) count = 2;
}

CellGroup touchedCells(long double x, long double y,
                      std::size_t width, std::size_t height) noexcept {
    CellGroup group;
    std::array<long long, 2> x_cells{};
    std::array<long long, 2> y_cells{};
    std::size_t x_count = 0;
    std::size_t y_count = 0;
    addAxisCells(x, x_cells, x_count);
    addAxisCells(y, y_cells, y_count);
    for (std::size_t yi = 0; yi < y_count; ++yi) {
        for (std::size_t xi = 0; xi < x_count; ++xi) {
            const long long cell_x = x_cells[xi];
            const long long cell_y = y_cells[yi];
            if (cell_x < 0 || cell_y < 0 ||
                cell_x >= static_cast<long long>(width) ||
                cell_y >= static_cast<long long>(height)) {
                continue;
            }
            const auto index = static_cast<std::size_t>(cell_y) * width +
                               static_cast<std::size_t>(cell_x);
            if (std::find(group.cells.begin(), group.cells.begin() +
                          static_cast<std::ptrdiff_t>(group.count), index) ==
                group.cells.begin() + static_cast<std::ptrdiff_t>(group.count)) {
                group.cells[group.count++] = index;
            }
        }
    }
    return group;
}

// Clips a segment in grid coordinates, then emits the cells touched by each
// interval and boundary crossing. Corner crossings include both side cells.
template <typename Visitor>
void traceSupercover(const GridPoint& start, const GridPoint& end,
                     std::size_t width, std::size_t height,
                     Visitor&& visit) {
    const long double x0 = static_cast<long double>(start.x);
    const long double y0 = static_cast<long double>(start.y);
    const long double dx = static_cast<long double>(end.x) - x0;
    const long double dy = static_cast<long double>(end.y) - y0;
    if (!std::isfinite(dx) || !std::isfinite(dy)) return;

    long double first = 0.0L;
    long double last = 1.0L;
    const auto clipAxis = [&first, &last](long double origin, long double delta,
                                          long double maximum) {
        if (delta == 0.0L) return origin >= 0.0L && origin <= maximum;
        long double enter = -origin / delta;
        long double leave = (maximum - origin) / delta;
        if (enter > leave) std::swap(enter, leave);
        first = std::max(first, enter);
        last = std::min(last, leave);
        return first <= last;
    };
    if (!clipAxis(x0, dx, static_cast<long double>(width)) ||
        !clipAxis(y0, dy, static_cast<long double>(height)) ||
        last < 0.0L || first > 1.0L) {
        return;
    }
    first = std::clamp(first, 0.0L, 1.0L);
    last = std::clamp(last, 0.0L, 1.0L);
    if (first > last) return;

    const long double clipped_x0 = std::clamp(x0 + first * dx, 0.0L,
                                               static_cast<long double>(width));
    const long double clipped_y0 = std::clamp(y0 + first * dy, 0.0L,
                                               static_cast<long double>(height));
    const long double clipped_x1 = std::clamp(x0 + last * dx, 0.0L,
                                               static_cast<long double>(width));
    const long double clipped_y1 = std::clamp(y0 + last * dy, 0.0L,
                                               static_cast<long double>(height));
    const long double clipped_dx = clipped_x1 - clipped_x0;
    const long double clipped_dy = clipped_y1 - clipped_y0;
    const int step_x = (clipped_dx > 0.0L) - (clipped_dx < 0.0L);
    const int step_y = (clipped_dy > 0.0L) - (clipped_dy < 0.0L);

    long double boundary_x = 0.0L;
    long double boundary_y = 0.0L;
    long double crossing_x = std::numeric_limits<long double>::infinity();
    long double crossing_y = std::numeric_limits<long double>::infinity();
    if (step_x > 0) {
        boundary_x = std::floor(clipped_x0) + 1.0L;
        crossing_x = (boundary_x - clipped_x0) / clipped_dx;
    } else if (step_x < 0) {
        boundary_x = std::ceil(clipped_x0) - 1.0L;
        crossing_x = (boundary_x - clipped_x0) / clipped_dx;
    }
    if (step_y > 0) {
        boundary_y = std::floor(clipped_y0) + 1.0L;
        crossing_y = (boundary_y - clipped_y0) / clipped_dy;
    } else if (step_y < 0) {
        boundary_y = std::ceil(clipped_y0) - 1.0L;
        crossing_y = (boundary_y - clipped_y0) / clipped_dy;
    }

    const auto emit = [&visit, width, height](long double x, long double y) {
        const CellGroup group = touchedCells(x, y, width, height);
        return group.count == 0 || visit(group);
    };
    if (!emit(clipped_x0, clipped_y0)) return;

    long double current = 0.0L;
    const std::size_t maximum_crossings = width + height + 2;
    for (std::size_t crossing_count = 0;
         crossing_count < maximum_crossings; ++crossing_count) {
        const long double next = std::min(crossing_x, crossing_y);
        if (!std::isfinite(next) || next > 1.0L) break;
        if (next > current) {
            const long double midpoint = (current + next) * 0.5L;
            if (!emit(clipped_x0 + midpoint * clipped_dx,
                      clipped_y0 + midpoint * clipped_dy)) {
                return;
            }
        }

        const long double tie_tolerance = 32.0L *
            std::numeric_limits<long double>::epsilon() *
            std::max(1.0L, std::abs(next));
        const bool cross_x = crossing_x <= next + tie_tolerance;
        const bool cross_y = crossing_y <= next + tie_tolerance;
        const long double event_x = cross_x ? boundary_x :
            clipped_x0 + next * clipped_dx;
        const long double event_y = cross_y ? boundary_y :
            clipped_y0 + next * clipped_dy;
        if (!emit(event_x, event_y)) return;
        current = next;
        if (cross_x) {
            boundary_x += static_cast<long double>(step_x);
            crossing_x = (boundary_x - clipped_x0) / clipped_dx;
        }
        if (cross_y) {
            boundary_y += static_cast<long double>(step_y);
            crossing_y = (boundary_y - clipped_y0) / clipped_dy;
        }
    }

    if (current < 1.0L) {
        const long double midpoint = (current + 1.0L) * 0.5L;
        if (!emit(clipped_x0 + midpoint * clipped_dx,
                  clipped_y0 + midpoint * clipped_dy)) {
            return;
        }
    }
    (void)emit(clipped_x1, clipped_y1);
}

} // namespace

ObstacleLayer::ObstacleLayer(OccupancyGrid static_map,
                             ObstacleLayerConfig config)
    : static_grid_(std::move(static_map)), config_(config) {
    validateConfig(config_);
    validateGrid(static_grid_);
    combined_grid_ = static_grid_;
    dynamic_occupied_.assign(static_grid_.cells.size(), 0);
}

bool ObstacleLayer::integrate(std::span<const ObstaclePoint> points,
                              const Point3& sensor_origin,
                              const Pose2D& base_pose,
                              double base_z) {
    if (points.size() > config_.max_points) {
        throw std::invalid_argument("obstacle scan exceeds configured point bound");
    }
    if (!std::isfinite(sensor_origin.x) || !std::isfinite(sensor_origin.y) ||
        !std::isfinite(sensor_origin.z) || !finite(base_pose) ||
        !std::isfinite(base_z)) {
        throw std::invalid_argument("obstacle scan pose or sensor origin is invalid");
    }

    const double sensor_height = sensor_origin.z - base_z;
    const bool sensor_height_valid = inHeightBand(sensor_height, config_);
    GridPoint sensor_grid;
    const bool sensor_grid_valid = worldToGridCoordinates(
        static_grid_, sensor_origin.x, sensor_origin.y, sensor_grid);

    std::vector<std::optional<std::size_t>> endpoint_cells;
    std::vector<Ray> valid_rays;
    endpoint_cells.reserve(points.size());
    if (sensor_height_valid) valid_rays.reserve(points.size());

    for (const ObstaclePoint& point : points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
            !std::isfinite(point.z)) {
            continue;
        }
        const double endpoint_height = point.z - base_z;
        if (!inHeightBand(endpoint_height, config_)) continue;

        const double dx = point.x - sensor_origin.x;
        const double dy = point.y - sensor_origin.y;
        const double dz = point.z - sensor_origin.z;
        const double range = std::hypot(std::hypot(dx, dy), dz);
        if (!std::isfinite(range) || range > config_.max_range) continue;

        const double body_distance = std::hypot(point.x - base_pose.x,
                                                point.y - base_pose.y);
        if (!std::isfinite(body_distance) || body_distance <= config_.self_radius) {
            continue;
        }

        GridPoint endpoint_grid;
        const bool endpoint_grid_valid = worldToGridCoordinates(
            static_grid_, point.x, point.y, endpoint_grid);
        const auto endpoint_cell = endpoint_grid_valid
            ? cellAt(static_grid_, endpoint_grid) : std::nullopt;
        endpoint_cells.push_back(endpoint_cell);
        if (sensor_height_valid && sensor_grid_valid && endpoint_grid_valid) {
            valid_rays.push_back({endpoint_grid});
        }
    }

    const std::size_t cell_count = static_grid_.cells.size();
    std::vector<std::uint8_t> scan_hits(cell_count, 0);
    bool changed = false;

    // Record every accepted return before tracing any ray so an earlier ray
    // cannot erase a hit elsewhere in this same scan.
    for (const auto& endpoint_cell : endpoint_cells) {
        if (!endpoint_cell) continue;
        const std::size_t index = *endpoint_cell;
        scan_hits[index] = 1;
        if (staticFree(static_grid_, index) && dynamic_occupied_[index] == 0) {
            dynamic_occupied_[index] = 1;
            combined_grid_.cells[index] = 100;
            changed = true;
        }
    }

    if (sensor_height_valid && sensor_grid_valid) {
        for (const Ray& ray : valid_rays) {
            traceSupercover(sensor_grid, ray.endpoint, static_grid_.width,
                            static_grid_.height, [&](const CellGroup& group) {
                for (std::size_t i = 0; i < group.count; ++i) {
                    const std::size_t index = group.cells[i];
                    if (scan_hits[index] != 0 || !staticFree(static_grid_, index)) {
                        return false;
                    }
                }
                for (std::size_t i = 0; i < group.count; ++i) {
                    const std::size_t index = group.cells[i];
                    if (staticFree(static_grid_, index) &&
                        dynamic_occupied_[index] != 0) {
                        dynamic_occupied_[index] = 0;
                        combined_grid_.cells[index] = static_grid_.cells[index];
                        changed = true;
                    }
                }
                return true;
            });
        }
    }
    return changed;
}

} // namespace rbt::navigation

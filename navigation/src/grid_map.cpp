#include <rbt_navigation/map/grid_map.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace rbt::navigation {
namespace {

constexpr std::size_t kMaximumCells = 4'000'000;
constexpr double kInfinity = std::numeric_limits<double>::infinity();

// The occupancy-to-binary-to-signed ESDF flow follows ROSE Navigation by
// hyheiyue (Copyright (c) 2026, MIT License); this uses an exact EDT.
bool worldToLocal(const OccupancyGrid& grid, double x, double y,
                  double& local_x, double& local_y) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    const double dx = x - grid.origin.x;
    const double dy = y - grid.origin.y;
    const double c = std::cos(grid.origin.yaw);
    const double s = std::sin(grid.origin.yaw);
    local_x = c * dx + s * dy;
    local_y = -s * dx + c * dy;
    return std::isfinite(local_x) && std::isfinite(local_y);
}

void squaredDistanceTransform1D(const double* input, std::size_t length,
                                double* output, std::vector<std::size_t>& sites,
                                std::vector<double>& boundaries) {
    int last = -1;
    for (std::size_t q = 0; q < length; ++q) {
        if (!std::isfinite(input[q])) continue;

        double intersection = -kInfinity;
        while (last >= 0) {
            const std::size_t p = sites[static_cast<std::size_t>(last)];
            const double qd = static_cast<double>(q);
            const double pd = static_cast<double>(p);
            intersection = ((input[q] + qd * qd) - (input[p] + pd * pd)) /
                           (2.0 * (qd - pd));
            if (intersection > boundaries[static_cast<std::size_t>(last)]) break;
            --last;
        }

        ++last;
        const auto slot = static_cast<std::size_t>(last);
        sites[slot] = q;
        boundaries[slot] = last == 0 ? -kInfinity : intersection;
        boundaries[slot + 1] = kInfinity;
    }

    if (last < 0) {
        std::fill(output, output + length, kInfinity);
        return;
    }

    int site = 0;
    for (std::size_t q = 0; q < length; ++q) {
        while (boundaries[static_cast<std::size_t>(site + 1)] <
               static_cast<double>(q)) {
            ++site;
        }
        const double delta = static_cast<double>(q) -
                             static_cast<double>(sites[static_cast<std::size_t>(site)]);
        output[q] = delta * delta + input[sites[static_cast<std::size_t>(site)]];
    }
}

std::vector<double> squaredDistanceTransform(const std::vector<std::uint8_t>& cells,
                                             std::size_t width, std::size_t height,
                                             bool source_value) {
    const std::size_t count = width * height;
    std::vector<double> row_pass(count, kInfinity);
    std::vector<double> result(count, kInfinity);
    const std::size_t maximum_axis = std::max(width, height);
    std::vector<double> input(maximum_axis);
    std::vector<double> output(maximum_axis);
    std::vector<std::size_t> sites(maximum_axis);
    std::vector<double> boundaries(maximum_axis + 1);

    for (std::size_t y = 0; y < height; ++y) {
        const std::size_t row = y * width;
        for (std::size_t x = 0; x < width; ++x) {
            input[x] = (cells[row + x] != 0) == source_value ? 0.0 : kInfinity;
        }
        squaredDistanceTransform1D(input.data(), width, output.data(), sites, boundaries);
        std::copy_n(output.data(), width, row_pass.data() + row);
    }

    for (std::size_t x = 0; x < width; ++x) {
        for (std::size_t y = 0; y < height; ++y) input[y] = row_pass[y * width + x];
        squaredDistanceTransform1D(input.data(), height, output.data(), sites, boundaries);
        for (std::size_t y = 0; y < height; ++y) result[y * width + x] = output[y];
    }
    return result;
}

} // namespace

GridMap::GridMap(OccupancyGrid grid) : grid_(std::move(grid)) {
    if (grid_.width == 0 || grid_.height == 0 ||
        grid_.width > kMaximumCells / grid_.height ||
        grid_.width * grid_.height > kMaximumCells) {
        throw std::invalid_argument("occupancy grid dimensions are invalid or exceed 4 million cells");
    }
    if (!std::isfinite(grid_.resolution) || grid_.resolution <= 0.0 ||
        !std::isfinite(grid_.resolution * 0.25) || grid_.resolution * 0.25 == 0.0 ||
        !finite(grid_.origin) || grid_.occupied_threshold < 0 ||
        grid_.occupied_threshold > 100) {
        throw std::invalid_argument("occupancy grid geometry or threshold is invalid");
    }
    const std::size_t count = grid_.width * grid_.height;
    if (grid_.cells.size() != count) {
        throw std::invalid_argument("occupancy grid cell count does not match dimensions");
    }
    if (std::any_of(grid_.cells.begin(), grid_.cells.end(), [](std::int8_t cell) {
            return cell < -1 || cell > 100;
        })) {
        throw std::invalid_argument("occupancy grid values must be -1 or in [0, 100]");
    }

    const double map_width = static_cast<double>(grid_.width) * grid_.resolution;
    const double map_height = static_cast<double>(grid_.height) * grid_.resolution;
    if (!std::isfinite(map_width) || !std::isfinite(map_height) ||
        !std::isfinite(std::hypot(map_width, map_height))) {
        throw std::invalid_argument("occupancy grid metric bounds are not finite");
    }
    const double c = std::cos(grid_.origin.yaw);
    const double s = std::sin(grid_.origin.yaw);
    const double corner_x[] = {
        grid_.origin.x, grid_.origin.x + c * map_width,
        grid_.origin.x - s * map_height,
        grid_.origin.x + c * map_width - s * map_height};
    const double corner_y[] = {
        grid_.origin.y, grid_.origin.y + s * map_width,
        grid_.origin.y + c * map_height,
        grid_.origin.y + s * map_width + c * map_height};
    for (std::size_t i = 0; i < 4; ++i) {
        if (!std::isfinite(corner_x[i]) || !std::isfinite(corner_y[i])) {
            throw std::invalid_argument("occupancy grid world bounds are not finite");
        }
    }

    occupied_.resize(count);
    for (std::size_t i = 0; i < count; ++i) {
        occupied_[i] = static_cast<std::uint8_t>(
            grid_.cells[i] < 0 || grid_.cells[i] >= grid_.occupied_threshold);
    }
    distance_to_occupied_ = squaredDistanceTransform(occupied_, grid_.width, grid_.height, true);
    distance_to_free_ = squaredDistanceTransform(occupied_, grid_.width, grid_.height, false);
    for (double& distance : distance_to_occupied_) {
        distance = std::sqrt(distance) * grid_.resolution;
    }
    for (double& distance : distance_to_free_) {
        distance = std::sqrt(distance) * grid_.resolution;
    }
}

std::optional<std::pair<int, int>> GridMap::worldToGrid(double x, double y) const {
    double local_x = 0.0;
    double local_y = 0.0;
    if (!worldToLocal(grid_, x, y, local_x, local_y)) return std::nullopt;
    const double map_width = static_cast<double>(grid_.width) * grid_.resolution;
    const double map_height = static_cast<double>(grid_.height) * grid_.resolution;
    if (local_x < 0.0 || local_y < 0.0 || local_x >= map_width || local_y >= map_height) {
        return std::nullopt;
    }
    const auto cell_x = static_cast<std::size_t>(std::floor(local_x / grid_.resolution));
    const auto cell_y = static_cast<std::size_t>(std::floor(local_y / grid_.resolution));
    if (cell_x >= grid_.width || cell_y >= grid_.height ||
        cell_x > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        cell_y > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    return std::pair<int, int>{static_cast<int>(cell_x), static_cast<int>(cell_y)};
}

Pose2D GridMap::gridToWorld(int x, int y) const {
    if (x < 0 || y < 0 || static_cast<std::size_t>(x) >= grid_.width ||
        static_cast<std::size_t>(y) >= grid_.height) {
        throw std::out_of_range("grid cell is outside the occupancy grid");
    }
    const double local_x = (static_cast<double>(x) + 0.5) * grid_.resolution;
    const double local_y = (static_cast<double>(y) + 0.5) * grid_.resolution;
    const double c = std::cos(grid_.origin.yaw);
    const double s = std::sin(grid_.origin.yaw);
    return {grid_.origin.x + c * local_x - s * local_y,
            grid_.origin.y + s * local_x + c * local_y,
            grid_.origin.yaw};
}

bool GridMap::cellOccupied(int x, int y) const {
    if (x < 0 || y < 0 || static_cast<std::size_t>(x) >= grid_.width ||
        static_cast<std::size_t>(y) >= grid_.height) {
        return true;
    }
    return occupied_[static_cast<std::size_t>(y) * grid_.width + static_cast<std::size_t>(x)] != 0;
}

bool GridMap::isOccupied(double x, double y) const {
    const auto cell = worldToGrid(x, y);
    return !cell || cellOccupied(cell->first, cell->second);
}

double GridMap::getDistanceToObstacle(double x, double y) const {
    const auto cell = worldToGrid(x, y);
    if (!cell || cellOccupied(cell->first, cell->second)) return 0.0;

    double local_x = 0.0;
    double local_y = 0.0;
    if (!worldToLocal(grid_, x, y, local_x, local_y)) return 0.0;
    const double center_x = (static_cast<double>(cell->first) + 0.5) * grid_.resolution;
    const double center_y = (static_cast<double>(cell->second) + 0.5) * grid_.resolution;
    const double offset = std::hypot(local_x - center_x, local_y - center_y);
    const std::size_t index = static_cast<std::size_t>(cell->second) * grid_.width +
                              static_cast<std::size_t>(cell->first);
    const double center_distance = distance_to_occupied_[index];
    const double half_diagonal = std::hypot(grid_.resolution * 0.5, grid_.resolution * 0.5);
    const double obstacle_clearance = center_distance - half_diagonal - offset;
    const double map_width = static_cast<double>(grid_.width) * grid_.resolution;
    const double map_height = static_cast<double>(grid_.height) * grid_.resolution;
    const double boundary_clearance = std::min(
        std::min(local_x, map_width - local_x), std::min(local_y, map_height - local_y));
    return std::max(0.0, std::min(boundary_clearance, obstacle_clearance));
}

double GridMap::signedDistance(int x, int y) const {
    if (x < 0 || y < 0 || static_cast<std::size_t>(x) >= grid_.width ||
        static_cast<std::size_t>(y) >= grid_.height) {
        throw std::out_of_range("grid cell is outside the occupancy grid");
    }
    const std::size_t index = static_cast<std::size_t>(y) * grid_.width +
                              static_cast<std::size_t>(x);
    const auto& distances = occupied_[index] != 0 ? distance_to_free_ : distance_to_occupied_;
    const double distance = distances[index];
    return occupied_[index] != 0 ? -distance : distance;
}

bool GridMap::isFree(double x, double y, double radius) const {
    if (!std::isfinite(radius) || radius < 0.0 || isOccupied(x, y)) return false;
    return getDistanceToObstacle(x, y) >= radius;
}

bool GridMap::segmentFree(const Pose2D& a, const Pose2D& b, double radius) const {
    if (!finite(a) || !finite(b) || !std::isfinite(radius) || radius < 0.0 ||
        !worldToGrid(a.x, a.y) || !worldToGrid(b.x, b.y)) {
        return false;
    }
    double ax = 0.0;
    double ay = 0.0;
    double bx = 0.0;
    double by = 0.0;
    if (!worldToLocal(grid_, a.x, a.y, ax, ay) || !worldToLocal(grid_, b.x, b.y, bx, by)) {
        return false;
    }
    const double length = std::hypot(bx - ax, by - ay);
    const double spacing = grid_.resolution * 0.25;
    const double step_count = std::max(1.0, std::ceil(length / spacing));
    if (!std::isfinite(step_count) || step_count > 20'000'000.0) return false;
    const auto steps = static_cast<std::size_t>(step_count);
    for (std::size_t i = 0; i <= steps; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(steps);
        const double x = (1.0 - t) * a.x + t * b.x;
        const double y = (1.0 - t) * a.y + t * b.y;
        if (!isFree(x, y, radius)) return false;
    }
    return true;
}

} // namespace rbt::navigation

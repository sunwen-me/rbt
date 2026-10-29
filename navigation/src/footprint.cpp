#include <rbt_navigation/geometry/footprint.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace rbt::navigation {
namespace {

constexpr double kMaximumSamples = 100'000.0;

struct Point {
    double x{};
    double y{};
};

struct Bounds {
    double min_x{std::numeric_limits<double>::infinity()};
    double min_y{std::numeric_limits<double>::infinity()};
    double max_x{-std::numeric_limits<double>::infinity()};
    double max_y{-std::numeric_limits<double>::infinity()};
};

[[nodiscard]] long double orient(const Point& a, const Point& b,
                                 const Point& c) noexcept {
    return (static_cast<long double>(b.x) - a.x) *
               (static_cast<long double>(c.y) - a.y) -
           (static_cast<long double>(b.y) - a.y) *
               (static_cast<long double>(c.x) - a.x);
}

[[nodiscard]] long double orientationTolerance(const Point& a, const Point& b,
                                               const Point& c) noexcept {
    const long double scale = std::max(
        {1.0L, std::abs(static_cast<long double>(a.x)),
         std::abs(static_cast<long double>(a.y)),
         std::abs(static_cast<long double>(b.x)),
         std::abs(static_cast<long double>(b.y)),
         std::abs(static_cast<long double>(c.x)),
         std::abs(static_cast<long double>(c.y))});
    return 32.0L * std::numeric_limits<long double>::epsilon() * scale * scale;
}

[[nodiscard]] int orientationSign(const Point& a, const Point& b,
                                  const Point& c) noexcept {
    const long double value = orient(a, b, c);
    const long double tolerance = orientationTolerance(a, b, c);
    if (value > tolerance) return 1;
    if (value < -tolerance) return -1;
    return 0;
}

[[nodiscard]] bool pointOnSegment(const Point& point, const Point& a,
                                  const Point& b) noexcept {
    if (orientationSign(a, b, point) != 0) return false;
    const double tolerance = static_cast<double>(orientationTolerance(a, b, point));
    return point.x >= std::min(a.x, b.x) - tolerance &&
           point.x <= std::max(a.x, b.x) + tolerance &&
           point.y >= std::min(a.y, b.y) - tolerance &&
           point.y <= std::max(a.y, b.y) + tolerance;
}

[[nodiscard]] bool segmentsIntersect(const Point& a, const Point& b,
                                     const Point& c, const Point& d) noexcept {
    const int ab_c = orientationSign(a, b, c);
    const int ab_d = orientationSign(a, b, d);
    const int cd_a = orientationSign(c, d, a);
    const int cd_b = orientationSign(c, d, b);
    if (ab_c * ab_d < 0 && cd_a * cd_b < 0) return true;
    return (ab_c == 0 && pointOnSegment(c, a, b)) ||
           (ab_d == 0 && pointOnSegment(d, a, b)) ||
           (cd_a == 0 && pointOnSegment(a, c, d)) ||
           (cd_b == 0 && pointOnSegment(b, c, d));
}

[[nodiscard]] double pointSegmentDistanceSquared(const Point& point,
                                                 const Point& a,
                                                 const Point& b) noexcept {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double length_squared = dx * dx + dy * dy;
    if (length_squared == 0.0) {
        const double px = point.x - a.x;
        const double py = point.y - a.y;
        return px * px + py * py;
    }
    const double projection = std::clamp(
        ((point.x - a.x) * dx + (point.y - a.y) * dy) / length_squared,
        0.0, 1.0);
    const double px = point.x - (a.x + projection * dx);
    const double py = point.y - (a.y + projection * dy);
    return px * px + py * py;
}

[[nodiscard]] double segmentDistanceSquared(const Point& a, const Point& b,
                                            const Point& c,
                                            const Point& d) noexcept {
    if (segmentsIntersect(a, b, c, d)) return 0.0;
    return std::min({pointSegmentDistanceSquared(a, c, d),
                     pointSegmentDistanceSquared(b, c, d),
                     pointSegmentDistanceSquared(c, a, b),
                     pointSegmentDistanceSquared(d, a, b)});
}

[[nodiscard]] bool pointInPolygon(const Point& point,
                                  const std::vector<Point>& polygon) noexcept {
    bool inside = false;
    for (std::size_t i = 0, previous = polygon.size() - 1U;
         i < polygon.size(); previous = i++) {
        const Point& a = polygon[previous];
        const Point& b = polygon[i];
        if (pointOnSegment(point, a, b)) return true;
        if ((a.y > point.y) != (b.y > point.y)) {
            const long double crossing_x =
                static_cast<long double>(a.x) +
                (static_cast<long double>(point.y) - a.y) *
                    (static_cast<long double>(b.x) - a.x) /
                    (static_cast<long double>(b.y) - a.y);
            if (static_cast<long double>(point.x) < crossing_x) inside = !inside;
        }
    }
    return inside;
}

[[nodiscard]] bool pointInRectangle(const Point& point, double min_x,
                                    double min_y, double max_x,
                                    double max_y) noexcept {
    return point.x >= min_x && point.x <= max_x &&
           point.y >= min_y && point.y <= max_y;
}

[[nodiscard]] bool polygonIntersectsRectangle(const std::vector<Point>& polygon,
                                              double min_x, double min_y,
                                              double max_x,
                                              double max_y) noexcept {
    const Point rectangle[] = {{min_x, min_y}, {max_x, min_y},
                               {max_x, max_y}, {min_x, max_y}};
    for (const Point& point : polygon) {
        if (pointInRectangle(point, min_x, min_y, max_x, max_y)) return true;
    }
    for (const Point& corner : rectangle) {
        if (pointInPolygon(corner, polygon)) return true;
    }
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Point& a = polygon[i];
        const Point& b = polygon[(i + 1U) % polygon.size()];
        for (std::size_t edge = 0; edge < 4U; ++edge) {
            if (segmentsIntersect(a, b, rectangle[edge],
                                  rectangle[(edge + 1U) % 4U])) {
                return true;
            }
        }
    }
    return false;
}

[[nodiscard]] double polygonRectangleDistanceSquared(
    const std::vector<Point>& polygon, double min_x, double min_y,
    double max_x, double max_y) noexcept {
    if (polygonIntersectsRectangle(polygon, min_x, min_y, max_x, max_y)) {
        return 0.0;
    }

    const Point rectangle[] = {{min_x, min_y}, {max_x, min_y},
                               {max_x, max_y}, {min_x, max_y}};
    double minimum = std::numeric_limits<double>::infinity();
    for (const Point& point : polygon) {
        const double closest_x = std::clamp(point.x, min_x, max_x);
        const double closest_y = std::clamp(point.y, min_y, max_y);
        const double dx = point.x - closest_x;
        const double dy = point.y - closest_y;
        minimum = std::min(minimum, dx * dx + dy * dy);
    }
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Point& a = polygon[i];
        const Point& b = polygon[(i + 1U) % polygon.size()];
        for (std::size_t edge = 0; edge < 4U; ++edge) {
            minimum = std::min(
                minimum,
                segmentDistanceSquared(a, b, rectangle[edge],
                                       rectangle[(edge + 1U) % 4U]));
        }
    }
    return minimum;
}

[[nodiscard]] Bounds boundsOf(const std::vector<Point>& polygon) noexcept {
    Bounds bounds;
    for (const Point& point : polygon) {
        bounds.min_x = std::min(bounds.min_x, point.x);
        bounds.min_y = std::min(bounds.min_y, point.y);
        bounds.max_x = std::max(bounds.max_x, point.x);
        bounds.max_y = std::max(bounds.max_y, point.y);
    }
    return bounds;
}

[[nodiscard]] bool validSimplePolygon(const std::vector<FootprintPoint>& polygon) {
    if (polygon.size() < 3U) return false;
    for (const FootprintPoint& point : polygon) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) return false;
    }

    std::vector<Point> points;
    points.reserve(polygon.size());
    for (const FootprintPoint& point : polygon) points.push_back({point.x, point.y});

    long double twice_area = 0.0L;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const Point& a = points[i];
        const Point& b = points[(i + 1U) % points.size()];
        if (a.x == b.x && a.y == b.y) return false;
        twice_area += static_cast<long double>(a.x) * b.y -
                      static_cast<long double>(a.y) * b.x;
    }
    if (!std::isfinite(twice_area) || twice_area == 0.0L) return false;

    for (std::size_t i = 0; i < points.size(); ++i) {
        const std::size_t next_i = (i + 1U) % points.size();
        for (std::size_t j = i + 1U; j < points.size(); ++j) {
            const std::size_t next_j = (j + 1U) % points.size();
            if (i == j || next_i == j || next_j == i) continue;
            if (segmentsIntersect(points[i], points[next_i], points[j],
                                  points[next_j])) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool validPadding(double padding) noexcept {
    return std::isfinite(padding) && padding >= 0.0;
}

[[nodiscard]] Pose2D interpolatePose(const Pose2D& a, const Pose2D& b,
                                     double t, double yaw_delta) noexcept {
    const double yaw = normalizeAngle(
        normalizeAngle(a.yaw) + t * yaw_delta);
    return {(1.0 - t) * a.x + t * b.x,
            (1.0 - t) * a.y + t * b.y, yaw};
}

[[nodiscard]] Pose2D integrateCommand(const Pose2D& start,
                                      const Twist2D& command,
                                      double time) noexcept {
    const double angular_distance = command.angular_z * time;
    if (!std::isfinite(angular_distance)) {
        return {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0};
    }
    const double half_angle = angular_distance * 0.5;
    const double sinc = half_angle == 0.0
                            ? 1.0
                            : std::sin(half_angle) / half_angle;
    const double center_distance = command.linear_x * time * sinc;
    const double middle_yaw = normalizeAngle(
        normalizeAngle(start.yaw) + half_angle);
    return {start.x + center_distance * std::cos(middle_yaw),
            start.y + center_distance * std::sin(middle_yaw),
            normalizeAngle(normalizeAngle(start.yaw) + angular_distance)};
}

[[nodiscard]] double commandHorizon(const Twist2D& command, double duration,
                                    double max_reference_distance) noexcept {
    if (!std::isfinite(max_reference_distance) || command.linear_x == 0.0) {
        return duration;
    }
    if (max_reference_distance == 0.0) return 0.0;

    const long double speed = std::abs(static_cast<long double>(command.linear_x));
    const long double angular_speed =
        std::abs(static_cast<long double>(command.angular_z));
    long double crossing_time = std::numeric_limits<long double>::infinity();
    if (angular_speed == 0.0L) {
        crossing_time = static_cast<long double>(max_reference_distance) / speed;
    } else {
        const long double ratio =
            static_cast<long double>(max_reference_distance) * angular_speed /
            (2.0L * speed);
        if (ratio < 1.0L) {
            crossing_time = 2.0L * std::asin(ratio) / angular_speed;
        }
    }
    if (crossing_time < static_cast<long double>(duration)) {
        return static_cast<double>(crossing_time);
    }
    return duration;
}

[[nodiscard]] bool sampleCount(double total_displacement, double spacing,
                               std::size_t& count) noexcept {
    if (!std::isfinite(total_displacement) || total_displacement < 0.0 ||
        !std::isfinite(spacing) || spacing <= 0.0) {
        return false;
    }
    if (total_displacement == 0.0) {
        count = 0U;
        return true;
    }
    const double raw_count = std::ceil(total_displacement / spacing);
    if (!std::isfinite(raw_count) || raw_count > kMaximumSamples) return false;
    count = std::max<std::size_t>(1U, static_cast<std::size_t>(raw_count));
    return true;
}

} // namespace

Pose2D composePose(const Pose2D& a, const Pose2D& b) noexcept {
    const double yaw = normalizeAngle(a.yaw) + normalizeAngle(b.yaw);
    const double c = std::cos(normalizeAngle(a.yaw));
    const double s = std::sin(normalizeAngle(a.yaw));
    return {a.x + c * b.x - s * b.y,
            a.y + s * b.x + c * b.y,
            normalizeAngle(yaw)};
}

Pose2D inversePose(const Pose2D& pose) noexcept {
    const double yaw = normalizeAngle(pose.yaw);
    const double c = std::cos(yaw);
    const double s = std::sin(yaw);
    return {-c * pose.x - s * pose.y,
            s * pose.x - c * pose.y,
            normalizeAngle(-yaw)};
}

Pose2D baseToDrive(const Pose2D& base_pose,
                   const RobotGeometry& geometry) noexcept {
    return composePose(base_pose, inversePose(geometry.drive_to_base));
}

Pose2D driveToBase(const Pose2D& drive_pose,
                   const RobotGeometry& geometry) noexcept {
    return composePose(drive_pose, geometry.drive_to_base);
}

FootprintCollisionChecker::FootprintCollisionChecker(
    std::vector<FootprintPoint> polygon)
    : footprint_(std::move(polygon)) {
    if (!validSimplePolygon(footprint_)) {
        throw std::invalid_argument("footprint must be a finite simple polygon with nonzero area");
    }
    for (const FootprintPoint& point : footprint_) {
        radius_ = std::max(radius_, std::hypot(point.x, point.y));
    }
    if (!std::isfinite(radius_)) {
        throw std::invalid_argument("footprint radius must be finite");
    }
}

bool FootprintCollisionChecker::poseFree(const GridMap& map,
                                         const Pose2D& drive_pose,
                                         double padding) const {
    if (!finite(drive_pose) || !validPadding(padding)) return false;

    // The existing ESDF query is a conservative distance to occupied cell area
    // and map boundaries. A free circumscribed disk proves the polygon free;
    // near obstacles we still check the filled, asymmetric polygon below.
    if (map.getDistanceToObstacle(drive_pose.x,drive_pose.y)>radius_+padding)
        return true;

    const Pose2D local_pose = composePose(inversePose(map.origin()), drive_pose);
    if (!finite(local_pose)) return false;
    const double c = std::cos(local_pose.yaw);
    const double s = std::sin(local_pose.yaw);
    std::vector<Point> local_polygon;
    local_polygon.reserve(footprint_.size());
    for (const FootprintPoint& vertex : footprint_) {
        const Point point{local_pose.x + c * vertex.x - s * vertex.y,
                          local_pose.y + s * vertex.x + c * vertex.y};
        if (!std::isfinite(point.x) || !std::isfinite(point.y)) return false;
        local_polygon.push_back(point);
    }

    const Bounds bounds = boundsOf(local_polygon);
    const double resolution = map.resolution();
    const double map_width = static_cast<double>(map.width()) * resolution;
    const double map_height = static_cast<double>(map.height()) * resolution;
    if (!std::isfinite(map_width) || !std::isfinite(map_height) ||
        bounds.min_x - padding <= 0.0 || bounds.min_y - padding <= 0.0 ||
        bounds.max_x + padding >= map_width ||
        bounds.max_y + padding >= map_height) {
        return false;
    }

    const double min_cell_x = std::ceil((bounds.min_x - padding) / resolution) - 1.0;
    const double min_cell_y = std::ceil((bounds.min_y - padding) / resolution) - 1.0;
    const double max_cell_x = std::floor((bounds.max_x + padding) / resolution);
    const double max_cell_y = std::floor((bounds.max_y + padding) / resolution);
    const int first_x = std::max(0, static_cast<int>(min_cell_x));
    const int first_y = std::max(0, static_cast<int>(min_cell_y));
    const int last_x = std::min(static_cast<int>(map.width()) - 1,
                                static_cast<int>(max_cell_x));
    const int last_y = std::min(static_cast<int>(map.height()) - 1,
                                static_cast<int>(max_cell_y));
    const double padding_squared = padding * padding;

    for (int y = first_y; y <= last_y; ++y) {
        for (int x = first_x; x <= last_x; ++x) {
            if (!map.cellOccupied(x, y)) continue;
            const double min_x = static_cast<double>(x) * resolution;
            const double min_y = static_cast<double>(y) * resolution;
            const double max_x = min_x + resolution;
            const double max_y = min_y + resolution;
            if (polygonRectangleDistanceSquared(local_polygon, min_x, min_y,
                                                max_x, max_y) <= padding_squared) {
                return false;
            }
        }
    }
    return true;
}

bool FootprintCollisionChecker::motionFree(const GridMap& map, const Pose2D& a,
                                           const Pose2D& b,
                                           double padding) const {
    if (!finite(a) || !finite(b) || !validPadding(padding)) return false;
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double translation = std::hypot(dx, dy);
    const double yaw_delta = normalizeAngle(
        normalizeAngle(b.yaw) - normalizeAngle(a.yaw));
    const double angular_displacement = radius_ * std::abs(yaw_delta);
    const double total_displacement = translation + angular_displacement;
    std::size_t steps = 0U;
    if (!sampleCount(total_displacement, map.resolution() * 0.25, steps)) return false;
    if (steps == 0U) return poseFree(map, a, padding);

    const double interval_displacement = total_displacement / static_cast<double>(steps);
    const double checked_padding = padding + interval_displacement * 0.5;
    if (!std::isfinite(checked_padding)) return false;
    for (std::size_t i = 0; i <= steps; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(steps);
        const Pose2D pose = interpolatePose(a, b, t, yaw_delta);
        if (!finite(pose) || !poseFree(map, pose, checked_padding)) return false;
    }
    return true;
}

bool FootprintCollisionChecker::commandFree(
    const GridMap& map, const Pose2D& start, const Twist2D& command,
    double duration, double max_reference_distance, double padding) const {
    if (!finite(start) || !finite(command) || !std::isfinite(duration) ||
        duration < 0.0 || std::isnan(max_reference_distance) ||
        max_reference_distance < 0.0 || !validPadding(padding)) {
        return false;
    }

    const double horizon = commandHorizon(command, duration,
                                          max_reference_distance);
    if (!std::isfinite(horizon) || horizon < 0.0) return false;
    const double point_speed_bound = std::abs(command.linear_x) +
                                     radius_ * std::abs(command.angular_z);
    const double total_displacement = point_speed_bound * horizon;
    std::size_t steps = 0U;
    if (!sampleCount(total_displacement, map.resolution() * 0.25, steps)) return false;
    if (steps == 0U) return poseFree(map, start, padding);

    const double interval_displacement = total_displacement / static_cast<double>(steps);
    const double checked_padding = padding + interval_displacement * 0.5;
    if (!std::isfinite(checked_padding)) return false;
    for (std::size_t i = 0; i <= steps; ++i) {
        const double time = horizon * static_cast<double>(i) /
                            static_cast<double>(steps);
        const Pose2D pose = integrateCommand(start, command, time);
        if (!finite(pose) || !poseFree(map, pose, checked_padding)) return false;
    }
    return true;
}

} // namespace rbt::navigation

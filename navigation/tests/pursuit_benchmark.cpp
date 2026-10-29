#include <rbt_navigation/controller/pure_pursuit.hpp>
#include <rbt_navigation/geometry/geometry_io.hpp>
#include <rbt_navigation/planner/state_lattice.hpp>
#include <rbt_navigation/simulation/simulated_base.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace rbt::navigation;
namespace fs = std::filesystem;

namespace {

constexpr double kResolution = 0.05;
constexpr double kDt = 0.02;
constexpr TimePoint kDtNs = 20'000'000;
constexpr std::size_t kMaxSteps = 6000U;
constexpr double kSampleSpacing = 0.0025;
constexpr double kRearZoneX = -0.196;
constexpr double kGoalPositionTolerance = 0.08;
constexpr double kGoalYawTolerance = 0.06;

struct Point {
    double x{};
    double y{};
};

using Polygon = std::vector<Point>;

struct Scenario {
    std::string name;
    std::string path_source;
    Pose2D start;
    OccupancyGrid grid;
    Path2D path;
    bool allow_reverse{};
};

struct ControllerVariant {
    std::string name;
    PursuitGeometry geometry{PursuitGeometry::PurePursuit};
    bool dynamic_window{};
};

struct MetricAccumulator {
    std::size_t tracking_samples{};
    double position_square_sum{};
    double position_max{};
    double yaw_square_sum{};
    double yaw_max{};
    std::size_t linear_increment_exceedances{};
    std::size_t angular_increment_exceedances{};
    double command_linear_square_sum{};
    double command_angular_square_sum{};
    std::size_t command_actual_samples{};
    double full_clearance{std::numeric_limits<double>::infinity()};
    double rear_clearance{std::numeric_limits<double>::infinity()};
    bool collision{};
};

struct RunResult {
    std::string id;
    std::string scenario;
    std::string source;
    std::string controller;
    std::string dynamics;
    std::string start_variant;
    std::string outcome{"error"};
    std::string fixture_status{"valid"};
    std::string error_message;
    std::uint32_t error_code{};
    std::size_t steps{};
    double task_seconds{};
    Pose2D initial_pose;
    Pose2D goal_pose;
    Pose2D final_pose;
    double final_position_error{std::numeric_limits<double>::infinity()};
    double final_yaw_error{std::numeric_limits<double>::infinity()};
    bool production_path_poses_free{};
    bool production_path_segments_free{};
    MetricAccumulator metrics;
    fs::path directory;
    fs::path path_csv;
    fs::path map_csv;
    fs::path telemetry_csv;
    fs::path result_json;
};

struct BvhCell {
    int x{};
    int y{};
};

struct BvhNode {
    double min_x{};
    double min_y{};
    double max_x{};
    double max_y{};
    std::size_t begin{};
    std::size_t end{};
    int left{-1};
    int right{-1};
};

[[noreturn]] void invalid(const std::string& reason) {
    throw std::runtime_error(reason);
}

double angleDelta(double from, double to) noexcept {
    return normalizeAngle(to - from);
}

double distance2(Point a, Point b) noexcept {
    const double dx = a.x - b.x;
    const double dy = a.y - b.y;
    return dx * dx + dy * dy;
}

double pointSegmentDistanceSquared(Point point, Point a, Point b) noexcept {
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double denominator = dx * dx + dy * dy;
    if (denominator <= 1e-24) return distance2(point, a);
    const double t = std::clamp(((point.x - a.x) * dx + (point.y - a.y) * dy) /
                                    denominator,
                                0.0, 1.0);
    return distance2(point, {a.x + t * dx, a.y + t * dy});
}

bool pointInPolygon(Point point, const Polygon& polygon) noexcept {
    bool inside = false;
    for (std::size_t i = 0, j = polygon.size() - 1U; i < polygon.size(); j = i++) {
        const Point& a = polygon[i];
        const Point& b = polygon[j];
        const bool crosses = ((a.y > point.y) != (b.y > point.y)) &&
            (point.x < (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x);
        if (crosses) inside = !inside;
    }
    return inside;
}

Polygon transformPolygon(const std::vector<FootprintPoint>& footprint,
                         const Pose2D& pose) {
    Polygon result;
    result.reserve(footprint.size());
    const double c = std::cos(pose.yaw);
    const double s = std::sin(pose.yaw);
    for (const FootprintPoint& point : footprint) {
        result.push_back({pose.x + c * point.x - s * point.y,
                          pose.y + s * point.x + c * point.y});
    }
    return result;
}

Polygon clipRearZone(const std::vector<FootprintPoint>& footprint,
                     const Pose2D& pose) {
    Polygon local;
    local.reserve(footprint.size() + 2U);
    for (const FootprintPoint& point : footprint) local.push_back({point.x, point.y});
    Polygon clipped;
    for (std::size_t i = 0; i < local.size(); ++i) {
        const Point a = local[i];
        const Point b = local[(i + 1U) % local.size()];
        const bool a_inside = a.x <= kRearZoneX;
        const bool b_inside = b.x <= kRearZoneX;
        if (a_inside) clipped.push_back(a);
        if (a_inside != b_inside) {
            const double t = (kRearZoneX - a.x) / (b.x - a.x);
            clipped.push_back({kRearZoneX, a.y + t * (b.y - a.y)});
        }
    }
    const double c = std::cos(pose.yaw);
    const double s = std::sin(pose.yaw);
    for (Point& point : clipped) {
        point = {pose.x + c * point.x - s * point.y,
                 pose.y + s * point.x + c * point.y};
    }
    return clipped;
}

bool separatedOnAxis(const Polygon& polygon, const std::array<Point, 4>& rect,
                     Point axis) noexcept {
    double polygon_min = polygon.front().x * axis.x + polygon.front().y * axis.y;
    double polygon_max = polygon_min;
    for (std::size_t i = 1; i < polygon.size(); ++i) {
        const double projection = polygon[i].x * axis.x + polygon[i].y * axis.y;
        polygon_min = std::min(polygon_min, projection);
        polygon_max = std::max(polygon_max, projection);
    }
    double rect_min = rect.front().x * axis.x + rect.front().y * axis.y;
    double rect_max = rect_min;
    for (std::size_t i = 1; i < rect.size(); ++i) {
        const double projection = rect[i].x * axis.x + rect[i].y * axis.y;
        rect_min = std::min(rect_min, projection);
        rect_max = std::max(rect_max, projection);
    }
    return polygon_max < rect_min - 1e-12 || rect_max < polygon_min - 1e-12;
}

bool polygonIntersectsRect(const Polygon& polygon, double min_x, double min_y,
                           double max_x, double max_y) noexcept {
    if (polygon.size() < 3U) return false;
    const std::array<Point, 4> rectangle{{
        {min_x, min_y}, {max_x, min_y}, {max_x, max_y}, {min_x, max_y}}};
    if (separatedOnAxis(polygon, rectangle, {1.0, 0.0}) ||
        separatedOnAxis(polygon, rectangle, {0.0, 1.0})) return false;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Point a = polygon[i];
        const Point b = polygon[(i + 1U) % polygon.size()];
        if (separatedOnAxis(polygon, rectangle, {-(b.y - a.y), b.x - a.x})) return false;
    }
    return true;
}

double polygonRectDistance(const Polygon& polygon, double min_x, double min_y,
                           double max_x, double max_y) noexcept {
    if (polygon.size() < 3U) return std::numeric_limits<double>::infinity();
    if (polygonIntersectsRect(polygon, min_x, min_y, max_x, max_y)) return 0.0;
    const std::array<Point, 4> rectangle{{
        {min_x, min_y}, {max_x, min_y}, {max_x, max_y}, {min_x, max_y}}};
    double best_squared = std::numeric_limits<double>::infinity();
    for (Point point : polygon) {
        const double x = std::clamp(point.x, min_x, max_x);
        const double y = std::clamp(point.y, min_y, max_y);
        best_squared = std::min(best_squared, distance2(point, {x, y}));
    }
    for (Point point : rectangle) {
        for (std::size_t i = 0; i < polygon.size(); ++i) {
            best_squared = std::min(best_squared,
                pointSegmentDistanceSquared(point, polygon[i], polygon[(i + 1U) % polygon.size()]));
        }
    }
    return std::sqrt(best_squared);
}

double bboxDistanceSquared(const Polygon& polygon, const BvhNode& node) noexcept {
    double min_x = polygon.front().x;
    double max_x = min_x;
    double min_y = polygon.front().y;
    double max_y = min_y;
    for (Point point : polygon) {
        min_x = std::min(min_x, point.x);
        max_x = std::max(max_x, point.x);
        min_y = std::min(min_y, point.y);
        max_y = std::max(max_y, point.y);
    }
    const double dx = max_x < node.min_x ? node.min_x - max_x
                     : (node.max_x < min_x ? min_x - node.max_x : 0.0);
    const double dy = max_y < node.min_y ? node.min_y - max_y
                     : (node.max_y < min_y ? min_y - node.max_y : 0.0);
    return dx * dx + dy * dy;
}

class IndependentClearance {
public:
    explicit IndependentClearance(const OccupancyGrid& grid) : grid_(grid) {
        for (std::size_t y = 0; y < grid.height; ++y) {
            for (std::size_t x = 0; x < grid.width; ++x) {
                if (grid.cells[y * grid.width + x] < grid.occupied_threshold) continue;
                occupied_.push_back({static_cast<int>(x), static_cast<int>(y)});
            }
        }
        if (!occupied_.empty()) build(0U, occupied_.size());
    }

    [[nodiscard]] double clearance(const Polygon& polygon) const {
        if (polygon.size() < 3U) return 0.0;
        double min_x = polygon.front().x;
        double max_x = min_x;
        double min_y = polygon.front().y;
        double max_y = min_y;
        for (Point point : polygon) {
            min_x = std::min(min_x, point.x);
            max_x = std::max(max_x, point.x);
            min_y = std::min(min_y, point.y);
            max_y = std::max(max_y, point.y);
        }
        const double map_max_x = grid_.width * grid_.resolution;
        const double map_max_y = grid_.height * grid_.resolution;
        double best = std::min({min_x, min_y, map_max_x - max_x, map_max_y - max_y});
        if (best <= 0.0) return 0.0;
        if (nodes_.empty()) return best;
        double best_squared = best * best;
        query(0, polygon, best_squared, best);
        return std::max(0.0, best);
    }

private:
    int build(std::size_t begin, std::size_t end) {
        BvhNode node;
        node.begin = begin;
        node.end = end;
        node.min_x = std::numeric_limits<double>::infinity();
        node.min_y = std::numeric_limits<double>::infinity();
        node.max_x = -std::numeric_limits<double>::infinity();
        node.max_y = -std::numeric_limits<double>::infinity();
        for (std::size_t i = begin; i < end; ++i) {
            const double x = occupied_[i].x * grid_.resolution;
            const double y = occupied_[i].y * grid_.resolution;
            node.min_x = std::min(node.min_x, x);
            node.min_y = std::min(node.min_y, y);
            node.max_x = std::max(node.max_x, x + grid_.resolution);
            node.max_y = std::max(node.max_y, y + grid_.resolution);
        }
        const int index = static_cast<int>(nodes_.size());
        nodes_.push_back(node);
        if (end - begin > 8U) {
            const bool split_x = node.max_x - node.min_x >= node.max_y - node.min_y;
            const std::size_t middle = begin + (end - begin) / 2U;
            std::nth_element(occupied_.begin() + static_cast<std::ptrdiff_t>(begin),
                             occupied_.begin() + static_cast<std::ptrdiff_t>(middle),
                             occupied_.begin() + static_cast<std::ptrdiff_t>(end),
                             [split_x](BvhCell a, BvhCell b) {
                                 return split_x ? a.x < b.x : a.y < b.y;
                             });
            const int left = build(begin, middle);
            const int right = build(middle, end);
            nodes_[static_cast<std::size_t>(index)].left = left;
            nodes_[static_cast<std::size_t>(index)].right = right;
        }
        return index;
    }

    void query(int node_index, const Polygon& polygon, double& best_squared,
               double& best) const {
        const BvhNode& node = nodes_[static_cast<std::size_t>(node_index)];
        if (bboxDistanceSquared(polygon, node) > best_squared) return;
        if (node.left < 0) {
            for (std::size_t i = node.begin; i < node.end; ++i) {
                const BvhCell cell = occupied_[i];
                const double x = cell.x * grid_.resolution;
                const double y = cell.y * grid_.resolution;
                const double distance = polygonRectDistance(
                    polygon, x, y, x + grid_.resolution, y + grid_.resolution);
                const double squared = distance * distance;
                if (squared < best_squared) {
                    best_squared = squared;
                    best = distance;
                    if (best <= 0.0) return;
                }
            }
            return;
        }
        const BvhNode& left = nodes_[static_cast<std::size_t>(node.left)];
        const BvhNode& right = nodes_[static_cast<std::size_t>(node.right)];
        if (bboxDistanceSquared(polygon, left) <= bboxDistanceSquared(polygon, right)) {
            query(node.left, polygon, best_squared, best);
            if (best <= 0.0) return;
            query(node.right, polygon, best_squared, best);
        } else {
            query(node.right, polygon, best_squared, best);
            if (best <= 0.0) return;
            query(node.left, polygon, best_squared, best);
        }
    }

    const OccupancyGrid& grid_;
    std::vector<BvhCell> occupied_;
    std::vector<BvhNode> nodes_;
};

std::vector<std::int8_t> freeCells(std::size_t width, std::size_t height) {
    return std::vector<std::int8_t>(width * height, 0);
}

Scenario makeOpenScenario(std::string name, Pose2D start) {
    Scenario scenario;
    scenario.name = std::move(name);
    scenario.start = start;
    scenario.grid.width = 240U;
    scenario.grid.height = 240U;
    scenario.grid.resolution = kResolution;
    scenario.grid.cells = freeCells(scenario.grid.width, scenario.grid.height);
    return scenario;
}

const LatticePrimitive& shortestTranslation(const LatticeControlSet& controls,
                                             std::uint32_t from,
                                             std::uint32_t to) {
    const LatticePrimitive* best = nullptr;
    for (const LatticePrimitive& primitive : controls.primitives) {
        if (primitive.start_angle != from || primitive.end_angle != to ||
            primitive.trajectory_length <= 1e-4 || primitive.poses.empty()) continue;
        const Pose2D& end = primitive.poses.back();
        if (std::hypot(end.x, end.y) <= 0.10) continue;
        if (best == nullptr || primitive.trajectory_length < best->trajectory_length ||
            (primitive.trajectory_length == best->trajectory_length &&
             primitive.trajectory_id < best->trajectory_id)) best = &primitive;
    }
    if (best == nullptr) {
        invalid("control set has no translating primitive for heading pair " +
                std::to_string(from) + "->" + std::to_string(to));
    }
    return *best;
}

const LatticePrimitive& shortestStraight(const LatticeControlSet& controls,
                                         std::uint32_t heading) {
    const LatticePrimitive* best = nullptr;
    for (const LatticePrimitive& primitive : controls.primitives) {
        if (primitive.start_angle != heading || primitive.end_angle != heading ||
            primitive.trajectory_length <= 1e-4 || primitive.poses.empty()) continue;
        double yaw_change = 0.0;
        for (std::size_t i = 1; i < primitive.poses.size(); ++i) {
            yaw_change += std::abs(angleDelta(primitive.poses[i - 1U].yaw,
                                             primitive.poses[i].yaw));
        }
        const Pose2D& end = primitive.poses.back();
        if (yaw_change > 1e-8 || std::hypot(end.x, end.y) <= 0.10) continue;
        if (best == nullptr || primitive.trajectory_length < best->trajectory_length ||
            (primitive.trajectory_length == best->trajectory_length &&
             primitive.trajectory_id < best->trajectory_id)) best = &primitive;
    }
    if (best == nullptr) invalid("control set has no translating straight primitive for heading " +
                                 std::to_string(heading));
    return *best;
}

void appendPrimitive(Path2D& path, Pose2D& anchor,
                     const LatticePrimitive& primitive,
                     const LatticeControlSet& controls) {
    for (const Pose2D& local : primitive.poses) {
        path.poses.push_back({anchor.x + local.x, anchor.y + local.y, local.yaw});
    }
    const Pose2D& end = primitive.poses.back();
    anchor = {anchor.x + end.x, anchor.y + end.y,
              normalizeAngle(controls.headings[primitive.end_angle])};
}

void appendStraightDistance(Path2D& path, Pose2D& anchor,
                            const LatticeControlSet& controls,
                            std::uint32_t heading, double distance_m) {
    const LatticePrimitive& straight = shortestStraight(controls, heading);
    const std::size_t repetitions = static_cast<std::size_t>(
        std::ceil(distance_m / straight.trajectory_length));
    for (std::size_t i = 0; i < repetitions; ++i) appendPrimitive(path, anchor, straight, controls);
}

Scenario makeSCurve(const LatticeControlSet& controls) {
    Scenario scenario = makeOpenScenario("S_curve", {3.0, 3.0, 0.0});
    scenario.path_source = "constructed_official_primitives";
    Pose2D anchor = scenario.start;
    scenario.path.poses.push_back(anchor);
    appendStraightDistance(scenario.path, anchor, controls, 0U, 0.6);
    constexpr std::array<std::uint32_t, 9> headings{{0U, 1U, 2U, 1U, 0U, 15U, 14U, 15U, 0U}};
    for (std::size_t i = 1; i < headings.size(); ++i) {
        appendPrimitive(scenario.path, anchor,
                        shortestTranslation(controls, headings[i - 1U], headings[i]), controls);
    }
    appendStraightDistance(scenario.path, anchor, controls, 0U, 0.75);
    scenario.path.map_version = 1U;
    return scenario;
}

void appendLinear(Path2D& path, Pose2D& pose, double dx, double dy,
                  double length, std::size_t segments) {
    const Pose2D origin = pose;
    for (std::size_t i = 1; i <= segments; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(segments);
        path.poses.push_back({origin.x + t * dx, origin.y + t * dy, origin.yaw});
    }
    pose = {origin.x + dx, origin.y + dy, origin.yaw};
    (void)length;
}

double pointPolygonDistance(Point point, const Polygon& polygon) noexcept {
    if (polygon.size() < 3U) return std::numeric_limits<double>::infinity();
    if (pointInPolygon(point, polygon)) return 0.0;
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        best = std::min(best, pointSegmentDistanceSquared(
            point, polygon[i], polygon[(i + 1U) % polygon.size()]));
    }
    return std::sqrt(best);
}

void carveSweptCorridor(Scenario& scenario,
                        const std::vector<FootprintPoint>& footprint,
                        double margin) {
    scenario.grid.cells.assign(scenario.grid.width * scenario.grid.height,
                               scenario.grid.occupied_threshold);
    std::vector<Polygon> swept;
    const double radius = [&] {
        double result = 0.0;
        for (const FootprintPoint& point : footprint)
            result = std::max(result, std::hypot(point.x, point.y));
        return result;
    }();
    for (std::size_t i = 0; i < scenario.path.poses.size(); ++i) {
        const Pose2D& pose = scenario.path.poses[i];
        swept.push_back(transformPolygon(footprint, pose));
        if (i == 0U) continue;
        const Pose2D& prior = scenario.path.poses[i - 1U];
        const double distance = std::hypot(pose.x - prior.x, pose.y - prior.y);
        const double yaw_change = std::abs(angleDelta(prior.yaw, pose.yaw));
        const std::size_t samples = std::max<std::size_t>(1U, static_cast<std::size_t>(
            std::ceil((distance + radius * yaw_change) / 0.01)));
        for (std::size_t j = 1U; j < samples; ++j) {
            const double t = static_cast<double>(j) / static_cast<double>(samples);
            swept.push_back(transformPolygon(footprint, {
                prior.x + t * (pose.x - prior.x), prior.y + t * (pose.y - prior.y),
                normalizeAngle(prior.yaw + t * angleDelta(prior.yaw, pose.yaw))}));
        }
    }
    const double cell_half_diagonal = scenario.grid.resolution * std::numbers::sqrt2 / 2.0;
    for (std::size_t y = 0; y < scenario.grid.height; ++y) {
        for (std::size_t x = 0; x < scenario.grid.width; ++x) {
            const Point center{(static_cast<double>(x) + 0.5) * scenario.grid.resolution,
                               (static_cast<double>(y) + 0.5) * scenario.grid.resolution};
            bool in_corridor = false;
            for (const Polygon& polygon : swept) {
                if (pointPolygonDistance(center, polygon) <= margin + cell_half_diagonal) {
                    in_corridor = true;
                    break;
                }
            }
            if (in_corridor) scenario.grid.cells[y * scenario.grid.width + x] = 0;
        }
    }
}

Scenario makeNarrowTurn(const LatticeControlSet& controls,
                        const RobotGeometry& geometry) {
    Scenario scenario = makeOpenScenario("narrow_turn", {4.0, 4.0, 0.0});
    scenario.path_source = "constructed_official_primitives";
    Pose2D anchor = scenario.start;
    scenario.path.poses.push_back(anchor);
    appendStraightDistance(scenario.path, anchor, controls, 0U, 0.6);
    constexpr std::array<std::uint32_t, 5> headings{{0U, 1U, 2U, 3U, 4U}};
    for (std::size_t i = 1; i < headings.size(); ++i) {
        appendPrimitive(scenario.path, anchor,
                        shortestTranslation(controls, headings[i - 1U], headings[i]), controls);
    }
    appendStraightDistance(scenario.path, anchor, controls, 4U, 0.75);
    scenario.path.map_version = 1U;
    carveSweptCorridor(scenario, geometry.footprint, 0.08);
    return scenario;
}

Scenario makeReverseCusp() {
    Scenario scenario = makeOpenScenario("reverse_cusp", {4.0, 4.0, 0.0});
    scenario.path_source = "constructed_kinematic_segments";
    scenario.allow_reverse = true;
    Pose2D pose = scenario.start;
    scenario.path.poses.push_back(pose);
    appendLinear(scenario.path, pose, 1.0, 0.0, 1.0, 20U);
    appendLinear(scenario.path, pose, -1.5, 0.0, 1.5, 30U);
    scenario.path.map_version = 1U;
    scenario.grid.cells.assign(scenario.grid.width * scenario.grid.height,
                               scenario.grid.occupied_threshold);
    const std::size_t first_x = static_cast<std::size_t>(std::floor(2.3 / kResolution));
    const std::size_t end_x = static_cast<std::size_t>(std::ceil(5.8 / kResolution));
    const std::size_t first_y = static_cast<std::size_t>(std::floor(3.7 / kResolution));
    const std::size_t end_y = static_cast<std::size_t>(std::ceil(4.3 / kResolution));
    for (std::size_t y = first_y; y < end_y; ++y)
        for (std::size_t x = first_x; x < end_x; ++x)
            scenario.grid.cells[y * scenario.grid.width + x] = 0;
    return scenario;
}

Scenario makeRotationTranslate() {
    Scenario scenario = makeOpenScenario("rotation_translate", {4.0, 4.0, 0.0});
    scenario.path_source = "constructed_kinematic_segments";
    scenario.path.poses = {scenario.start, {4.0, 4.0, std::numbers::pi / 2.0}};
    Pose2D pose{4.0, 4.0, std::numbers::pi / 2.0};
    appendLinear(scenario.path, pose, 0.0, 1.5, 1.5, 30U);
    scenario.path.map_version = 1U;
    return scenario;
}

Scenario makeTailSweep(const LatticeControlSet& controls,
                       const RobotGeometry& geometry,
                       std::size_t obstacle_cell_x) {
    Scenario scenario = makeOpenScenario("tail_sweep", {6.0, 6.0, 0.0});
    scenario.path_source = "planner_search";
    scenario.grid.width = 240U;
    scenario.grid.height = 240U;
    scenario.grid.cells.assign(scenario.grid.width * scenario.grid.height, 0);
    scenario.grid.cells[120U * scenario.grid.width + obstacle_cell_x] = 100;
    const auto map = std::make_shared<GridMap>(scenario.grid);
    auto checker = std::make_shared<const FootprintCollisionChecker>(geometry.footprint);
    StateLatticeConfig planner_config;
    planner_config.timeout = std::chrono::seconds(4);
    planner_config.allow_reverse_expansion = false;
    StateLatticePlanner planner(controls, checker, planner_config);
    const Pose2D goal{6.0, 7.0, std::numbers::pi / 2.0};
    const PlannerResult result = planner.plan(scenario.start, goal, {map, 1U, 0});
    if (!result.success()) {
        invalid("tail_sweep fixture planning failed with error " +
                std::to_string(static_cast<std::uint32_t>(result.error)));
    }
    scenario.path = result.path;
    scenario.path_source = "planner_search";
    return scenario;
}

std::vector<Pose2D> makeRunPath(const Scenario& scenario, const Pose2D& start) {
    std::vector<Pose2D> result = scenario.path.poses;
    if (result.empty()) return result;
    // The fixed map path remains unchanged; the perturbed run starts beside it.
    (void)start;
    return result;
}

Pose2D perturbedStart(const Pose2D& start) noexcept {
    const double c = std::cos(start.yaw);
    const double s = std::sin(start.yaw);
    return {start.x - s * 0.03, start.y + c * 0.03,
            normalizeAngle(start.yaw + 0.05)};
}

struct Projection {
    Pose2D desired;
    double position_error{};
};

Projection projectPath(const std::vector<Pose2D>& path, const Pose2D& actual) {
    if (path.empty()) invalid("cannot project onto an empty path");
    Projection best{path.back(), distance(actual, path.back())};
    bool found = false;
    for (std::size_t i = 1; i < path.size(); ++i) {
        const Pose2D& a = path[i - 1U];
        const Pose2D& b = path[i];
        const double dx = b.x - a.x;
        const double dy = b.y - a.y;
        const double length_squared = dx * dx + dy * dy;
        if (length_squared <= 1e-16) continue;
        const double t = std::clamp(((actual.x - a.x) * dx +
                                     (actual.y - a.y) * dy) / length_squared,
                                    0.0, 1.0);
        const Pose2D desired{a.x + t * dx, a.y + t * dy,
            normalizeAngle(a.yaw + t * angleDelta(a.yaw, b.yaw))};
        const double error = std::hypot(actual.x - desired.x, actual.y - desired.y);
        if (!found || error < best.position_error - 1e-12) {
            best = {desired, error};
            found = true;
        }
    }
    return best;
}

struct PathCheck {
    bool poses_free{true};
    bool segments_free{true};
};

PathCheck checkProductionPath(const Scenario& scenario,
                             const FootprintCollisionChecker& checker,
                              const GridMap& map,
                              const std::vector<Pose2D>& path) {
    PathCheck result;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (!checker.poseFree(map, path[i])) {
            result.poses_free = false;
        }
        if (i > 0U && !checker.motionFree(map, path[i - 1U], path[i])) {
            result.segments_free = false;
        }
    }
    (void)scenario;
    return result;
}

double footprintRadius(const std::vector<FootprintPoint>& footprint) noexcept {
    double result = 0.0;
    for (const FootprintPoint& point : footprint)
        result = std::max(result, std::hypot(point.x, point.y));
    return result;
}

void sampleSweep(const Pose2D& from, const Pose2D& to,
                 const std::vector<FootprintPoint>& footprint,
                 const IndependentClearance& independent,
                 MetricAccumulator& metrics) {
    const double translation = distance(from, to);
    const double yaw_change = std::abs(angleDelta(from.yaw, to.yaw));
    const double swept = translation + footprintRadius(footprint) * yaw_change;
    const std::size_t samples = std::max<std::size_t>(1U,
        static_cast<std::size_t>(std::ceil(swept / kSampleSpacing)));
    for (std::size_t i = 0; i <= samples; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(samples);
        const Pose2D pose{from.x + t * (to.x - from.x),
                          from.y + t * (to.y - from.y),
                          normalizeAngle(from.yaw + t * angleDelta(from.yaw, to.yaw))};
        const double full = independent.clearance(transformPolygon(footprint, pose));
        const double rear = independent.clearance(clipRearZone(footprint, pose));
        metrics.full_clearance = std::min(metrics.full_clearance, full);
        metrics.rear_clearance = std::min(metrics.rear_clearance, rear);
        if (full <= 1e-12) metrics.collision = true;
    }
}

double moveToward(double current, double target, double max_delta) noexcept {
    if (target > current) return std::min(target, current + max_delta);
    return std::max(target, current - max_delta);
}

Twist2D applyDynamics(const std::string& dynamics, const Twist2D& command,
                      std::deque<Twist2D>& delay, Twist2D& lagged) {
    if (dynamics == "ideal") return command;
    const Twist2D delayed = delay.front();
    delay.pop_front();
    delay.push_back(command);
    const double alpha = 1.0 - std::exp(-kDt / 0.12);
    const double filtered_linear = lagged.linear_x +
                                   alpha * (delayed.linear_x - lagged.linear_x);
    const double filtered_angular = lagged.angular_z +
                                    alpha * (delayed.angular_z - lagged.angular_z);
    lagged.linear_x = moveToward(lagged.linear_x, filtered_linear, 0.8 * kDt);
    lagged.angular_z = moveToward(lagged.angular_z, filtered_angular, 2.5 * kDt);
    return lagged;
}

std::string jsonEscape(std::string_view value) {
    std::ostringstream output;
    for (unsigned char c : value) {
        switch (c) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default: if (c >= 0x20U) output << static_cast<char>(c); break;
        }
    }
    return output.str();
}

void writeNumber(std::ostream& output, double value) {
    if (std::isfinite(value)) output << value;
    else output << "null";
}

std::string controllerGeometryName(PursuitGeometry geometry) {
    return geometry == PursuitGeometry::VectorPursuit ? "VectorPursuit" : "PurePursuit";
}

struct TelemetryRow {
    std::size_t step{};
    double time_seconds{};
    std::string phase;
    Pose2D actual;
    Pose2D desired;
    double position_error{};
    double yaw_error{};
    Twist2D command;
    Twist2D measured;
    double full_clearance{};
    double rear_clearance{};
    bool collision{};
};

std::string errorName(ErrorCode error) {
    switch (error) {
    case ErrorCode::None: return "None";
    case ErrorCode::InvalidInput: return "InvalidInput";
    case ErrorCode::MapUnavailable: return "MapUnavailable";
    case ErrorCode::StaleMap: return "StaleMap";
    case ErrorCode::LocalizationUnavailable: return "LocalizationUnavailable";
    case ErrorCode::StaleTf: return "StaleTf";
    case ErrorCode::StaleOdometry: return "StaleOdometry";
    case ErrorCode::StartOutside: return "StartOutside";
    case ErrorCode::GoalOutside: return "GoalOutside";
    case ErrorCode::StartOccupied: return "StartOccupied";
    case ErrorCode::GoalOccupied: return "GoalOccupied";
    case ErrorCode::NoPath: return "NoPath";
    case ErrorCode::PlanningTimeout: return "PlanningTimeout";
    case ErrorCode::Canceled: return "Canceled";
    case ErrorCode::EmptyPath: return "EmptyPath";
    case ErrorCode::PathDeviation: return "PathDeviation";
    case ErrorCode::Collision: return "Collision";
    case ErrorCode::ControllerFailure: return "ControllerFailure";
    case ErrorCode::PlannerFailure: return "PlannerFailure";
    case ErrorCode::NavigationTimeout: return "NavigationTimeout";
    case ErrorCode::GoalConflict: return "GoalConflict";
    }
    return "Unknown";
}

void writeMapCsv(const fs::path& file, const OccupancyGrid& grid) {
    std::ofstream out(file, std::ios::trunc);
    if (!out) invalid("cannot create " + file.string());
    out << "width,height,resolution_m,origin_x,origin_y,origin_yaw,cell_x,cell_y,"
           "world_x,world_y,occupancy\n" << std::setprecision(17);
    for (std::size_t y = 0; y < grid.height; ++y) {
        for (std::size_t x = 0; x < grid.width; ++x) {
            out << grid.width << ',' << grid.height << ',' << grid.resolution << ','
                << grid.origin.x << ',' << grid.origin.y << ',' << grid.origin.yaw << ','
                << x << ',' << y << ',' << x * grid.resolution << ','
                << y * grid.resolution << ','
                << static_cast<int>(grid.cells[y * grid.width + x]) << '\n';
        }
    }
    out.flush();
    if (!out) invalid("failed writing " + file.string());
}

void writePathCsv(const fs::path& file, const Scenario& scenario,
                  const std::vector<Pose2D>& path,
                  const FootprintCollisionChecker& checker,
                  const GridMap& map) {
    std::ofstream out(file, std::ios::trunc);
    if (!out) invalid("cannot create " + file.string());
    out << "index,x,y,body_yaw,production_pose_free,production_segment_free\n"
        << std::setprecision(17);
    for (std::size_t i = 0; i < path.size(); ++i) {
        const bool pose_free = checker.poseFree(map, path[i]);
        const bool segment_free = i == 0U || checker.motionFree(map, path[i - 1U], path[i]);
        out << i << ',' << path[i].x << ',' << path[i].y << ',' << path[i].yaw << ','
            << (pose_free ? 1 : 0) << ',' << (segment_free ? 1 : 0) << '\n';
    }
    out.flush();
    if (!out) invalid("failed writing " + file.string());
    (void)scenario;
}

void writeTelemetryCsv(const fs::path& file, const std::vector<TelemetryRow>& rows) {
    std::ofstream out(file, std::ios::trunc);
    if (!out) invalid("cannot create " + file.string());
    out << "step,sim_time_seconds,phase,actual_x,actual_y,actual_yaw,desired_x,desired_y,"
           "desired_yaw,position_error_m,yaw_error_rad,command_linear_x,command_angular_z,"
           "actual_linear_x,actual_angular_z,full_footprint_clearance_m,"
           "rear_zone_clearance_m,collision\n" << std::setprecision(17);
    for (const TelemetryRow& row : rows) {
        out << row.step << ',' << row.time_seconds << ',' << row.phase << ','
            << row.actual.x << ',' << row.actual.y << ',' << row.actual.yaw << ','
            << row.desired.x << ',' << row.desired.y << ',' << row.desired.yaw << ','
            << row.position_error << ',' << row.yaw_error << ','
            << row.command.linear_x << ',' << row.command.angular_z << ','
            << row.measured.linear_x << ',' << row.measured.angular_z << ','
            << row.full_clearance << ',' << row.rear_clearance << ','
            << (row.collision ? 1 : 0) << '\n';
    }
    out.flush();
    if (!out) invalid("failed writing " + file.string());
}

void writeRunJson(const fs::path& file, const RunResult& run,
                  const ControllerVariant& variant, const ControllerConfig& config,
                  const RobotGeometry& geometry, const std::string& path_projection_note) {
    std::ofstream out(file, std::ios::trunc);
    if (!out) invalid("cannot create " + file.string());
    const MetricAccumulator& m = run.metrics;
    out << std::setprecision(17)
        << "{\n  \"schema_version\": 1,\n"
        << "  \"id\": \"" << jsonEscape(run.id) << "\",\n"
        << "  \"scenario\": \"" << jsonEscape(run.scenario) << "\",\n"
        << "  \"path_source\": \"" << jsonEscape(run.source) << "\",\n"
        << "  \"controller\": \"" << jsonEscape(run.controller) << "\",\n"
        << "  \"controller_geometry\": \"" << controllerGeometryName(variant.geometry) << "\",\n"
        << "  \"use_dynamic_window\": " << (variant.dynamic_window ? "true" : "false") << ",\n"
        << "  \"dynamics\": \"" << jsonEscape(run.dynamics) << "\",\n"
        << "  \"start_variant\": \"" << jsonEscape(run.start_variant) << "\",\n"
        << "  \"allow_reverse\": " << (config.allow_reverse ? "true" : "false") << ",\n"
        << "  \"outcome\": \"" << run.outcome << "\",\n"
        << "  \"fixture_status\": \"" << run.fixture_status << "\",\n"
        << "  \"error_code\": " << run.error_code << ",\n"
        << "  \"error_name\": \"" << errorName(static_cast<ErrorCode>(run.error_code)) << "\",\n"
        << "  \"error_message\": \"" << jsonEscape(run.error_message) << "\",\n"
        << "  \"task_seconds\": " << run.task_seconds << ",\n"
        << "  \"steps\": " << run.steps << ",\n"
        << "  \"tracking_samples_moving\": " << m.tracking_samples << ",\n"
        << "  \"tracking_projection_rule\": \"" << jsonEscape(path_projection_note) << "\",\n"
        << "  \"position_rms_m\": ";
    writeNumber(out, m.tracking_samples ? std::sqrt(m.position_square_sum / m.tracking_samples)
                                        : std::numeric_limits<double>::quiet_NaN());
    out << ",\n  \"position_max_m\": "; writeNumber(out, m.tracking_samples ? m.position_max : std::numeric_limits<double>::quiet_NaN());
    out << ",\n  \"body_yaw_rms_rad\": "; writeNumber(out, m.tracking_samples ? std::sqrt(m.yaw_square_sum / m.tracking_samples) : std::numeric_limits<double>::quiet_NaN());
    out << ",\n  \"body_yaw_max_rad\": "; writeNumber(out, m.tracking_samples ? m.yaw_max : std::numeric_limits<double>::quiet_NaN());
    out << ",\n  \"command_actual_linear_rms_mps\": "; writeNumber(out, m.command_actual_samples ? std::sqrt(m.command_linear_square_sum / m.command_actual_samples) : std::numeric_limits<double>::quiet_NaN());
    out << ",\n  \"command_actual_angular_rms_radps\": "; writeNumber(out, m.command_actual_samples ? std::sqrt(m.command_angular_square_sum / m.command_actual_samples) : std::numeric_limits<double>::quiet_NaN());
    out << ",\n  \"command_linear_increment_exceedances\": " << m.linear_increment_exceedances
        << ",\n  \"command_angular_increment_exceedances\": " << m.angular_increment_exceedances
        << ",\n  \"full_footprint_min_clearance_m\": "; writeNumber(out, m.full_clearance);
    out << ",\n  \"rear_zone_min_clearance_m\": "; writeNumber(out, m.rear_clearance);
    out << ",\n  \"independent_collision\": " << (m.collision ? "true" : "false")
        << ",\n  \"production_path_poses_free\": " << (run.production_path_poses_free ? "true" : "false")
        << ",\n  \"production_path_segments_free\": " << (run.production_path_segments_free ? "true" : "false")
        << ",\n  \"initial_pose\": [" << run.initial_pose.x << ',' << run.initial_pose.y << ',' << run.initial_pose.yaw << ']'
        << ",\n  \"goal_pose\": [" << run.goal_pose.x << ',' << run.goal_pose.y << ',' << run.goal_pose.yaw << ']'
        << ",\n  \"final_pose\": [" << run.final_pose.x << ',' << run.final_pose.y << ',' << run.final_pose.yaw << ']'
        << ",\n  \"final_position_error_m\": "; writeNumber(out, run.final_position_error);
    out << ",\n  \"final_yaw_error_rad\": "; writeNumber(out, run.final_yaw_error);
    out << ",\n  \"experiment_config\": {\n"
        << "    \"dt_seconds\": " << kDt << ",\n"
        << "    \"max_steps\": " << kMaxSteps << ",\n"
        << "    \"max_linear_mps\": " << config.max_linear << ",\n"
        << "    \"max_angular_radps\": " << config.max_angular << ",\n"
        << "    \"max_acceleration_mps2\": " << config.max_acceleration << ",\n"
        << "    \"max_deceleration_mps2\": " << config.max_deceleration << ",\n"
        << "    \"max_angular_acceleration_radps2\": " << config.max_angular_acceleration << ",\n"
        << "    \"max_angular_deceleration_radps2\": " << config.max_angular_deceleration << ",\n"
        << "    \"min_lookahead_m\": " << config.min_lookahead << ",\n"
        << "    \"max_lookahead_m\": " << config.max_lookahead << ",\n"
        << "    \"lookahead_time_s\": " << config.lookahead_time << ",\n"
        << "    \"position_tolerance_m\": " << config.position_tolerance << ",\n"
        << "    \"yaw_tolerance_rad\": " << config.yaw_tolerance << ",\n"
        << "    \"max_path_deviation_m\": " << config.max_path_deviation << ",\n"
        << "    \"regulated_min_radius_m\": " << config.regulated_min_radius << ",\n"
        << "    \"regulated_min_speed_mps\": " << config.regulated_min_speed << ",\n"
        << "    \"cost_scaling_distance_m\": " << config.cost_scaling_distance << ",\n"
        << "    \"cost_scaling_gain\": " << config.cost_scaling_gain << ",\n"
        << "    \"approach_scaling_distance_m\": " << config.approach_scaling_distance << ",\n"
        << "    \"min_approach_speed_mps\": " << config.min_approach_speed << ",\n"
        << "    \"rotate_to_heading_angle_rad\": " << config.rotate_to_heading_angle << ",\n"
        << "    \"rotate_to_heading_speed_radps\": " << config.rotate_to_heading_speed << ",\n"
        << "    \"collision_horizon_s\": " << config.collision_horizon << ",\n"
        << "    \"vector_pursuit_k\": " << config.vector_pursuit_k << ",\n"
        << "    \"vector_pursuit_min_radius_m\": " << config.vector_pursuit_min_radius << ",\n"
        << "    \"curvature_lookahead_m\": " << config.curvature_lookahead << ",\n"
        << "    \"use_fixed_curvature_lookahead\": " << (config.use_fixed_curvature_lookahead ? "true" : "false") << ",\n"
        << "    \"use_dynamic_window\": " << (config.use_dynamic_window ? "true" : "false") << ",\n"
        << "    \"allow_reverse\": " << (config.allow_reverse ? "true" : "false") << ",\n"
        << "    \"path_handler_enabled\": " << (config.use_feasible_path_handler ? "true" : "false") << ",\n"
        << "    \"enforce_path_inversion\": " << (config.enforce_path_inversion ? "true" : "false") << ",\n"
        << "    \"enforce_path_rotation\": " << (config.enforce_path_rotation ? "true" : "false") << ",\n"
        << "    \"minimum_path_rotation_rad\": " << config.minimum_path_rotation << ",\n"
        << "    \"measured_stopping_reaction_time_s\": " << config.reaction_time << ",\n"
        << "    \"footprint_vertices\": [";
    for (std::size_t i = 0; i < geometry.footprint.size(); ++i) {
        if (i != 0U) out << ',';
        out << '[' << geometry.footprint[i].x << ',' << geometry.footprint[i].y << ']';
    }
    out << "],\n    \"drive_to_base\": [" << geometry.drive_to_base.x << ','
        << geometry.drive_to_base.y << ',' << geometry.drive_to_base.yaw << "]\n  },\n"
        << "  \"artifacts\": {\n"
        << "    \"path_csv\": \"path.csv\",\n"
        << "    \"map_csv\": \"map.csv\",\n"
        << "    \"telemetry_csv\": \"telemetry.csv\"\n  }\n}\n";
    out.flush();
    if (!out) invalid("failed writing " + file.string());
}

RunResult runOne(const Scenario& scenario, const ControllerVariant& variant,
                 const std::string& dynamics, const std::string& start_variant,
                 const RobotGeometry& geometry,
                 const std::shared_ptr<const GridMap>& map,
                 const FootprintCollisionChecker& checker,
                 const IndependentClearance& independent,
                 const fs::path& output_directory) {
    RunResult run;
    run.id = scenario.name + "__" + variant.name + "__" + dynamics + "__" + start_variant;
    run.scenario = scenario.name;
    run.source = scenario.path_source;
    run.controller = variant.name;
    run.dynamics = dynamics;
    run.start_variant = start_variant;
    run.goal_pose = scenario.path.poses.back();
    run.initial_pose = start_variant == "perturbed" ? perturbedStart(scenario.start) : scenario.start;
    run.final_pose = run.initial_pose;
    run.directory = output_directory / scenario.name / dynamics / start_variant / variant.name;
    run.path_csv = run.directory / "path.csv";
    run.map_csv = run.directory / "map.csv";
    run.telemetry_csv = run.directory / "telemetry.csv";
    run.result_json = run.directory / "result.json";
    fs::create_directories(run.directory);

    const std::vector<Pose2D> fixed_path = makeRunPath(scenario, run.initial_pose);
    const PathCheck path_check = checkProductionPath(scenario, checker, *map, fixed_path);
    run.production_path_poses_free = path_check.poses_free;
    run.production_path_segments_free = path_check.segments_free;
    writePathCsv(run.path_csv, scenario, fixed_path, checker, *map);
    writeMapCsv(run.map_csv, scenario.grid);

    std::vector<TelemetryRow> telemetry;
    const Polygon initial_polygon = transformPolygon(geometry.footprint, run.initial_pose);
    if (!path_check.poses_free || !path_check.segments_free) {
        run.fixture_status = "invalid_shared_path";
        run.outcome = "error";
        run.error_code = static_cast<std::uint32_t>(ErrorCode::InvalidInput);
        run.error_message = "invalid_fixture: fixed path failed production pose/motionFree checks";
    } else if (independent.clearance(initial_polygon) <= 0.0 ||
        !checker.poseFree(*map, run.initial_pose)) {
        run.fixture_status = "invalid_start_polygon_not_free";
        run.outcome = "error";
        run.error_code = static_cast<std::uint32_t>(ErrorCode::InvalidInput);
        run.error_message = "invalid_fixture: start polygon collides with obstacle or map edge";
    } else {
        ControllerConfig config;
        config.max_linear = 0.2;
        config.max_angular = 0.5;
        config.max_acceleration = 0.8;
        config.max_deceleration = 0.8;
        config.max_angular_acceleration = 2.5;
        config.max_angular_deceleration = 2.5;
        config.position_tolerance = kGoalPositionTolerance;
        config.yaw_tolerance = kGoalYawTolerance;
        config.rotate_to_heading_speed = config.max_angular;
        config.use_dynamic_window = variant.dynamic_window;
        config.geometry = variant.geometry;
        config.allow_reverse = scenario.allow_reverse;
        config.use_feasible_path_handler = true;
        config.vector_pursuit_k = 5.0;
        config.vector_pursuit_min_radius = 0.10;
        config.reaction_time = 0.25;
        RegulatedPurePursuit controller(config);
        SimulatedBaseAdapter base(run.initial_pose);
        Twist2D previous_command{};
        Twist2D lagged_actual{};
        std::deque<Twist2D> delay(5U);
        const auto footprint_checker = std::make_shared<const FootprintCollisionChecker>(geometry.footprint);
        const TimePoint start_ns = 1;
        MetricAccumulator& metrics = run.metrics;
        sampleSweep(run.initial_pose, run.initial_pose, geometry.footprint, independent, metrics);
        for (std::size_t step = 0; step < kMaxSteps; ++step) {
            const TimePoint now = start_ns + static_cast<TimePoint>(step) * kDtNs;
            const Pose2D before = base.pose();
            const Twist2D measured_before = base.velocity();
            const RobotState state{before, measured_before, now, now, true};
            const MapSnapshot snapshot{map, 1U, now};
            ControllerContext context{now, kDt, previous_command, snapshot,
                footprintRadius(geometry.footprint), 250'000'000, 2 * second,
                footprint_checker};
            const ControllerResult result = controller.computeCommand(state, scenario.path, context);
            run.steps = step;
            run.final_pose = before;
            run.task_seconds = static_cast<double>(step) * kDt;
            run.final_position_error = distance(before, run.goal_pose);
            run.final_yaw_error = std::abs(angleDelta(before.yaw, run.goal_pose.yaw));
            if (result.error != ErrorCode::None) {
                run.outcome = "error";
                run.error_code = static_cast<std::uint32_t>(result.error);
                run.error_message = "controller returned " + errorName(result.error);
                break;
            }
            if (!finite(result.command)) {
                run.outcome = "error";
                run.error_code = static_cast<std::uint32_t>(ErrorCode::ControllerFailure);
                run.error_message = "controller returned a non-finite command";
                break;
            }
            if (result.reached_goal) {
                run.outcome = "success";
                run.steps = step;
                run.task_seconds = static_cast<double>(step) * kDt;
                telemetry.push_back({step, run.task_seconds,
                    std::abs(measured_before.linear_x) > 0.01 ? "MOVING" : "STOPPED",
                    before, fixed_path.back(), run.final_position_error, run.final_yaw_error,
                    result.command, measured_before,
                    independent.clearance(transformPolygon(geometry.footprint, before)),
                    independent.clearance(clipRearZone(geometry.footprint, before)),
                    metrics.collision});
                break;
            }

            if (step > 0U) {
                const double linear_limit = (result.command.linear_x >= previous_command.linear_x
                    ? config.max_acceleration : config.max_deceleration) * kDt;
                const double angular_limit = (result.command.angular_z >= previous_command.angular_z
                    ? config.max_angular_acceleration : config.max_angular_deceleration) * kDt;
                if (std::abs(result.command.linear_x - previous_command.linear_x) > linear_limit + 1e-9)
                    ++metrics.linear_increment_exceedances;
                if (std::abs(result.command.angular_z - previous_command.angular_z) > angular_limit + 1e-9)
                    ++metrics.angular_increment_exceedances;
            }
            const Twist2D actual_command = applyDynamics(dynamics, result.command, delay, lagged_actual);
            if (!finite(actual_command)) {
                run.outcome = "error";
                run.error_code = static_cast<std::uint32_t>(ErrorCode::ControllerFailure);
                run.error_message = "actuator wrapper produced a non-finite velocity";
                break;
            }
            base.apply(actual_command, kDt);
            const Pose2D after = base.pose();
            const Twist2D measured = base.velocity();
            sampleSweep(before, after, geometry.footprint, independent, metrics);
            const Projection projection = projectPath(fixed_path, after);
            const double yaw_error = angleDelta(projection.desired.yaw, after.yaw);
            if (std::abs(measured.linear_x) > 0.01) {
                ++metrics.tracking_samples;
                metrics.position_square_sum += projection.position_error * projection.position_error;
                metrics.position_max = std::max(metrics.position_max, projection.position_error);
                metrics.yaw_square_sum += yaw_error * yaw_error;
                metrics.yaw_max = std::max(metrics.yaw_max, std::abs(yaw_error));
            }
            metrics.command_linear_square_sum +=
                (result.command.linear_x - measured.linear_x) *
                (result.command.linear_x - measured.linear_x);
            metrics.command_angular_square_sum +=
                (result.command.angular_z - measured.angular_z) *
                (result.command.angular_z - measured.angular_z);
            ++metrics.command_actual_samples;
            run.final_pose = after;
            run.steps = step + 1U;
            run.task_seconds = static_cast<double>(step + 1U) * kDt;
            run.final_position_error = distance(after, run.goal_pose);
            run.final_yaw_error = std::abs(angleDelta(after.yaw, run.goal_pose.yaw));
            const double full = independent.clearance(transformPolygon(geometry.footprint, after));
            const double rear = independent.clearance(clipRearZone(geometry.footprint, after));
            telemetry.push_back({step + 1U, run.task_seconds,
                std::abs(measured.linear_x) > 0.01 ? "MOVING" : "STOPPED", after,
                projection.desired, projection.position_error, yaw_error, result.command,
                measured, full, rear, metrics.collision});
            previous_command = result.command;
            if (metrics.collision) {
                run.outcome = "error";
                run.error_code = static_cast<std::uint32_t>(ErrorCode::Collision);
                run.error_message = "independent filled-footprint sweep detected collision";
                break;
            }
            if (step + 1U == kMaxSteps) {
                run.outcome = "timeout";
                run.error_code = static_cast<std::uint32_t>(ErrorCode::NavigationTimeout);
                run.error_message = "120 second virtual task budget exceeded";
            }
        }
        if (run.outcome.empty()) run.outcome = "error";
        if (run.error_message.empty() && run.outcome == "error")
            run.error_message = "controller run stopped without a terminal outcome";
        if (run.error_message.empty() && run.outcome == "timeout")
            run.error_message = "virtual task timeout";
        writeTelemetryCsv(run.telemetry_csv, telemetry);
        writeRunJson(run.result_json, run, variant, config, geometry,
            "nearest nonzero-length segment by Euclidean projection; strict improvement only, so ties keep the first; body yaw interpolated on shortest arc; samples counted only when abs(actual linear velocity)>0.01 m/s");
    }
    if (run.fixture_status != "valid") {
        ControllerConfig config;
        config.max_linear = 0.2; config.max_angular = 0.5;
        config.max_acceleration = 0.8; config.max_deceleration = 0.8;
        config.max_angular_acceleration = 2.5; config.max_angular_deceleration = 2.5;
        config.position_tolerance = kGoalPositionTolerance;
        config.yaw_tolerance = kGoalYawTolerance;
        config.rotate_to_heading_speed = config.max_angular;
        config.use_dynamic_window = variant.dynamic_window;
        config.geometry = variant.geometry;
        config.allow_reverse = scenario.allow_reverse;
        config.use_feasible_path_handler = true;
        config.vector_pursuit_k = 5.0;
        config.vector_pursuit_min_radius = 0.10;
        config.reaction_time = 0.25;
        writeTelemetryCsv(run.telemetry_csv, telemetry);
        writeRunJson(run.result_json, run, variant, config, geometry,
            "nearest nonzero-length segment by Euclidean projection; strict improvement only, so ties keep the first; body yaw interpolated on shortest arc; samples counted only when abs(actual linear velocity)>0.01 m/s");
    }
    return run;
}

void writeSummary(const fs::path& file, const std::vector<RunResult>& runs,
                  const fs::path& geometry_file, const fs::path& lattice_file,
                  const RobotGeometry& geometry,
                  const LatticeControlSet& controls, bool smoke,
                  std::size_t tail_obstacle_cell_x) {
    std::ofstream out(file, std::ios::trunc);
    if (!out) invalid("cannot create summary.json");
    out << std::setprecision(17)
        << "{\n  \"schema_version\": 1,\n"
        << "  \"experiment\": \"offline_controller_ab\",\n"
        << "  \"mode\": \"offlinecontroller_experiment\",\n"
        << "  \"smoke\": " << (smoke ? "true" : "false") << ",\n"
        << "  \"geometry_file\": \"" << jsonEscape(fs::absolute(geometry_file).string()) << "\",\n"
        << "  \"lattice_file\": \"" << jsonEscape(fs::absolute(lattice_file).string()) << "\",\n"
        << "  \"geometry\": {\"frame\":\"base_footprint\",\"drive_to_base\":["
        << geometry.drive_to_base.x << ',' << geometry.drive_to_base.y << ','
        << geometry.drive_to_base.yaw << "],\"footprint_vertices\":[";
    for (std::size_t i = 0; i < geometry.footprint.size(); ++i) {
        if (i != 0U) out << ',';
        out << '[' << geometry.footprint[i].x << ',' << geometry.footprint[i].y << ']';
    }
    out << "]},\n"
        << "  \"lattice\": {\"resolution_m\":" << controls.resolution
        << ",\"turning_radius_m\":" << controls.min_turning_radius
        << ",\"headings_rad\":[";
    for (std::size_t i = 0; i < controls.headings.size(); ++i) {
        if (i != 0U) out << ',';
        out << controls.headings[i];
    }
    out << "]},\n"
        << "  \"fixture_parameters\": {\"tail_sweep_obstacle_cell_x\":"
        << tail_obstacle_cell_x << ",\"tail_sweep_obstacle_cell_y\":120},\n"
        << "  \"path_projection_rule\": \"nearest nonzero-length segment by Euclidean projection; strict improvement only, so ties keep the first; body yaw interpolated on shortest arc; moving-only when abs(actual linear velocity)>0.01 m/s\",\n"
        << "  \"dynamics_models\": {\n"
        << "    \"ideal\": \"actual twist equals command immediately\",\n"
        << "    \"lagged\": \"assumed sensitivity model: 5 control-step command delay (0.10 s), first-order tau=0.12 s, actual acceleration caps 0.8 m/s^2 and 2.5 rad/s^2\"\n  },\n"
        << "  \"common_controller_config\": {\n"
        << "    \"max_linear_mps\": 0.2, \"max_angular_radps\": 0.5,\n"
        << "    \"max_acceleration_mps2\": 0.8, \"max_deceleration_mps2\": 0.8,\n"
        << "    \"max_angular_acceleration_radps2\": 2.5, \"max_angular_deceleration_radps2\": 2.5,\n"
        << "    \"position_tolerance_m\": 0.08, \"yaw_tolerance_rad\": 0.06,\n"
        << "    \"vector_pursuit_k\": 5.0, \"vector_pursuit_min_radius_m\": 0.1,\n"
        << "    \"reaction_time_s\": 0.25, \"path_handler_enabled\": true,\n"
        << "    \"goal_condition\": \"position <= 0.08 m; body yaw <= 0.06 rad; abs(actual linear), abs(actual angular), abs(previous command linear), and abs(previous command angular) <= 0.001\",\n"
        << "    \"dynamic_window_variant_mapping\": {\"rpp\":false,\"rpp_dwpp\":true,\"vp\":false,\"vp_window\":true}\n  },\n"
        << "  \"run_count\": " << runs.size() << ",\n"
        << "  \"runs\": [\n";
    for (std::size_t i = 0; i < runs.size(); ++i) {
        const RunResult& run = runs[i];
        const MetricAccumulator& m = run.metrics;
        out << "    {\"id\":\"" << jsonEscape(run.id) << "\","
            << "\"scenario\":\"" << jsonEscape(run.scenario) << "\","
            << "\"path_source\":\"" << jsonEscape(run.source) << "\","
            << "\"controller\":\"" << jsonEscape(run.controller) << "\","
            << "\"controller_geometry\":\"" << (run.controller == "vp" || run.controller == "vp_window" ? "VectorPursuit" : "PurePursuit") << "\","
            << "\"use_dynamic_window\":" << ((run.controller == "rpp_dwpp" || run.controller == "vp_window") ? "true" : "false") << ','
            << "\"dynamics\":\"" << jsonEscape(run.dynamics) << "\","
            << "\"start_variant\":\"" << jsonEscape(run.start_variant) << "\","
            << "\"initial_pose\":[" << run.initial_pose.x << ',' << run.initial_pose.y << ',' << run.initial_pose.yaw << "],"
            << "\"goal_pose\":[" << run.goal_pose.x << ',' << run.goal_pose.y << ',' << run.goal_pose.yaw << "],"
            << "\"final_pose\":[" << run.final_pose.x << ',' << run.final_pose.y << ',' << run.final_pose.yaw << "],"
            << "\"outcome\":\"" << run.outcome << "\","
            << "\"fixture_status\":\"" << run.fixture_status << "\","
            << "\"error_code\":" << run.error_code << ','
            << "\"error_message\":\"" << jsonEscape(run.error_message) << "\","
            << "\"task_seconds\":" << run.task_seconds << ','
            << "\"steps\":" << run.steps << ','
            << "\"tracking_samples_moving\":" << m.tracking_samples << ','
            << "\"position_rms_m\":";
        writeNumber(out, m.tracking_samples ? std::sqrt(m.position_square_sum / m.tracking_samples)
                                            : std::numeric_limits<double>::quiet_NaN());
        out << ",\"position_max_m\":"; writeNumber(out, m.tracking_samples ? m.position_max : std::numeric_limits<double>::quiet_NaN());
        out << ",\"body_yaw_rms_rad\":"; writeNumber(out, m.tracking_samples ? std::sqrt(m.yaw_square_sum / m.tracking_samples) : std::numeric_limits<double>::quiet_NaN());
        out << ",\"body_yaw_max_rad\":"; writeNumber(out, m.tracking_samples ? m.yaw_max : std::numeric_limits<double>::quiet_NaN());
        out << ",\"command_actual_linear_rms_mps\":"; writeNumber(out, m.command_actual_samples ? std::sqrt(m.command_linear_square_sum / m.command_actual_samples) : std::numeric_limits<double>::quiet_NaN());
        out << ",\"command_actual_angular_rms_radps\":"; writeNumber(out, m.command_actual_samples ? std::sqrt(m.command_angular_square_sum / m.command_actual_samples) : std::numeric_limits<double>::quiet_NaN());
        out << ",\"command_linear_increment_exceedances\":" << m.linear_increment_exceedances
            << ",\"command_angular_increment_exceedances\":" << m.angular_increment_exceedances
            << ",\"full_footprint_min_clearance_m\":"; writeNumber(out, m.full_clearance);
        out << ",\"rear_zone_min_clearance_m\":"; writeNumber(out, m.rear_clearance);
        out << ",\"independent_collision\":" << (m.collision ? "true" : "false")
            << ",\"production_path_poses_free\":" << (run.production_path_poses_free ? "true" : "false")
            << ",\"production_path_segments_free\":" << (run.production_path_segments_free ? "true" : "false")
            << ",\"final_position_error_m\":"; writeNumber(out, run.final_position_error);
        out << ",\"final_yaw_error_rad\":"; writeNumber(out, run.final_yaw_error);
        out << ",\"artifacts\":{\"path_csv\":\"" << jsonEscape(fs::relative(run.path_csv, file.parent_path()).generic_string())
            << "\",\"map_csv\":\"" << jsonEscape(fs::relative(run.map_csv, file.parent_path()).generic_string())
            << "\",\"telemetry_csv\":\"" << jsonEscape(fs::relative(run.telemetry_csv, file.parent_path()).generic_string())
            << "\",\"result_json\":\"" << jsonEscape(fs::relative(run.result_json, file.parent_path()).generic_string()) << "\"}}"
            << (i + 1U == runs.size() ? "\n" : ",\n");
    }
    out << "  ]\n}\n";
    out.flush();
    if (!out) invalid("failed writing summary.json");
}

struct Arguments {
    fs::path output;
    fs::path geometry;
    fs::path lattice;
    bool smoke{};
    std::size_t tail_obstacle_cell_x{112U};
};

Arguments parseArguments(int argc, char** argv) {
    Arguments args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option(argv[i]);
        if (option == "--smoke") {
            args.smoke = true;
            continue;
        }
        if (option != "--output" && option != "--geometry" && option != "--lattice"
            && option != "--tail-obstacle-cell-x")
            invalid("unknown argument: " + std::string(option));
        if (i + 1 >= argc) invalid("missing value for " + std::string(option));
        const std::string_view value(argv[++i]);
        if (option == "--tail-obstacle-cell-x") {
            std::size_t cell_x{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), cell_x);
            if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
                || cell_x > 239U) {
                invalid("--tail-obstacle-cell-x must be an integer in [0, 239]");
            }
            args.tail_obstacle_cell_x = cell_x;
        } else {
            const fs::path path(value);
            if (option == "--output") args.output = path;
            else if (option == "--geometry") args.geometry = path;
            else args.lattice = path;
        }
    }
    if (args.output.empty() || args.geometry.empty() || args.lattice.empty()) {
        invalid("usage: rbt_navigation_pursuit_benchmark --output DIR --geometry YAML --lattice JSON"
                " [--smoke] [--tail-obstacle-cell-x N]");
    }
    if (!fs::is_regular_file(args.geometry)) invalid("geometry file does not exist: " + args.geometry.string());
    if (!fs::is_regular_file(args.lattice)) invalid("lattice file does not exist: " + args.lattice.string());
    return args;
}

void validateArtifacts(const fs::path& output, const std::vector<RunResult>& runs) {
    if (!fs::is_regular_file(output / "summary.json")) invalid("summary.json artifact is missing");
    for (const RunResult& run : runs) {
        if (!fs::is_regular_file(run.path_csv) || !fs::is_regular_file(run.map_csv) ||
            !fs::is_regular_file(run.telemetry_csv) || !fs::is_regular_file(run.result_json)) {
            invalid("run artifact is missing for " + run.id);
        }
    }
}

} // namespace

int main(int argc, char** argv) try {
    const Arguments args = parseArguments(argc, argv);
    fs::create_directories(args.output);
    if (fs::exists(args.output / "summary.json"))
        invalid("refusing to overwrite existing summary.json in " + args.output.string());
    const RobotGeometry geometry = loadRobotGeometry(args.geometry.string());
    const LatticeControlSet controls = loadLatticeControlSet(args.lattice.string());
    const std::vector<ControllerVariant> variants{
        {"rpp", PursuitGeometry::PurePursuit, false},
        {"rpp_dwpp", PursuitGeometry::PurePursuit, true},
        {"vp", PursuitGeometry::VectorPursuit, false},
        {"vp_window", PursuitGeometry::VectorPursuit, true}};
    std::vector<Scenario> scenarios;
    scenarios.push_back(makeSCurve(controls));
    scenarios.push_back(makeNarrowTurn(controls, geometry));
    scenarios.push_back(makeReverseCusp());
    scenarios.push_back(makeRotationTranslate());
    scenarios.push_back(makeTailSweep(controls, geometry, args.tail_obstacle_cell_x));
    if (args.smoke) scenarios.resize(1U);
    const std::vector<std::string> dynamics = args.smoke
        ? std::vector<std::string>{"ideal"}
        : std::vector<std::string>{"ideal", "lagged"};
    const std::vector<std::string> starts = args.smoke
        ? std::vector<std::string>{"aligned"}
        : std::vector<std::string>{"aligned", "perturbed"};
    std::vector<RunResult> runs;
    for (const Scenario& scenario : scenarios) {
        const auto map = std::make_shared<const GridMap>(scenario.grid);
        const FootprintCollisionChecker checker(geometry.footprint);
        const auto shared_checker = std::make_shared<const FootprintCollisionChecker>(geometry.footprint);
        (void)shared_checker;
        const IndependentClearance independent(scenario.grid);
        for (const std::string& model : dynamics) {
            for (const std::string& start : starts) {
                for (const ControllerVariant& variant : variants) {
                    runs.push_back(runOne(scenario, variant, model, start, geometry,
                                          map, checker, independent, args.output));
                    const RunResult& run = runs.back();
                    std::cout << run.id << ": " << run.outcome
                              << " at " << std::fixed << std::setprecision(2)
                              << run.task_seconds << " simulated seconds\n";
                }
            }
        }
    }
    writeSummary(args.output / "summary.json", runs, args.geometry, args.lattice,
                 geometry, controls, args.smoke, args.tail_obstacle_cell_x);
    validateArtifacts(args.output, runs);
    std::cout << "wrote " << runs.size() << " pursuit benchmark runs to "
              << fs::absolute(args.output).string() << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << "pursuit benchmark: " << error.what() << '\n';
    return 1;
}

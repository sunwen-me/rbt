#include <rbt_navigation/geometry/geometry_io.hpp>
#include <rbt_navigation/map/grid_map.hpp>
#include <rbt_navigation/runtime/native.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace rbt::navigation;
namespace native = rbt::navigation::native;
namespace wire = rbt::navwire;
namespace fs = std::filesystem;

namespace {

constexpr TimePoint kSecond = 1'000'000'000;
constexpr std::uint32_t kIdle = static_cast<std::uint32_t>(NavigationState::Idle);
constexpr std::uint32_t kSucceeded = static_cast<std::uint32_t>(NavigationState::Succeeded);
constexpr std::uint32_t kFailed = static_cast<std::uint32_t>(NavigationState::Failed);
constexpr double kResolution = 0.05;
constexpr double kSampleSpacing = 0.0025;
constexpr double kPositionTolerance = 0.08;
constexpr double kYawTolerance = 0.06;

struct Scenario {
    std::string name;
    std::size_t width{};
    std::size_t height{};
    Pose2D start_drive;
    Pose2D goal_base;
    bool allow_reverse{};
    std::vector<std::int8_t> cells;
};

struct TelemetryRow {
    double seconds{};
    Pose2D drive;
    Pose2D base_link;
    double linear_x{};
    double angular_z{};
    std::uint32_t nav_state{};
    std::uint32_t nav_error{};
    std::uint32_t base_watchdog{};
    std::uint32_t guard_watchdog{};
};

struct Summary {
    std::string scenario;
    bool passed{};
    std::string error;
    std::uint64_t map_version{};
    std::uint64_t base_frames{};
    std::uint64_t guard_frames{};
    std::uint64_t command_frames{};
    std::uint64_t path_points{};
    std::uint64_t path_messages_checked{};
    std::uint64_t reverse_path_segments{};
    std::uint64_t negative_base_samples{};
    std::uint64_t negative_final_commands{};
    std::uint64_t moving_final_commands{};
    std::uint32_t nav_state{kIdle};
    std::uint32_t nav_error{};
    Pose2D goal_base;
    Pose2D final_drive;
    Pose2D final_base_link;
    double final_position_error{std::numeric_limits<double>::infinity()};
    double final_drive_position_error{std::numeric_limits<double>::infinity()};
    double final_yaw_error{std::numeric_limits<double>::infinity()};
    double maximum_abs_angular_command{};
    double maximum_path_yaw_change{};
    bool have_final_pose{};
    bool have_path{};
    bool planned_poses_free{true};
    bool planned_segments_free{true};
    bool actual_poses_free{true};
    bool actual_segments_free{true};
    bool path_turn_while_moving{};
    std::optional<double> planned_first_turn_translation;
    std::optional<double> actual_first_turn_translation;
    bool actual_turn_checked{};
    bool direct_turn_start_free{};
    bool direct_turn_end_free{};
    bool direct_turn_sweep_free{};
    std::vector<Pose2D> path_drive;
    std::vector<TelemetryRow> telemetry;
};

void validatePath(const Scenario& scenario, const RobotGeometry& geometry,
                  Summary& summary);

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

double angleDelta(double from, double to) noexcept {
    return normalizeAngle(to - from);
}

std::vector<std::int8_t> makeCells(const std::string& scenario,
                                  std::size_t width, std::size_t height) {
    std::vector<std::int8_t> cells(width * height, 0);
    if (scenario == "reverse") {
        // Leave a 0.60 m straight corridor. The 0.05 m grid is the same
        // resolution as the official differential-drive motion primitives.
        constexpr std::size_t corridor_first_row = 74U;
        constexpr std::size_t corridor_end_row = 86U;
        for (std::size_t y = 0; y < height; ++y) {
            if (y >= corridor_first_row && y < corridor_end_row) continue;
            std::fill_n(cells.begin() + static_cast<std::ptrdiff_t>(y * width),
                        width, static_cast<std::int8_t>(100));
        }
    } else if (scenario == "tail_sweep") {
        // This cell clears the footprint at yaw 0 and pi/2, but intersects
        // the rear sweep between those orientations at the start position.
        constexpr std::size_t obstacle_x = 112U;
        constexpr std::size_t obstacle_y = 120U;
        cells[obstacle_y * width + obstacle_x] = 100;
    }
    return cells;
}

Scenario makeScenario(const std::string& name) {
    Scenario result;
    result.name = name;
    if (name == "turn") {
        result.width = 160U;
        result.height = 160U;
        result.start_drive = {2.0, 2.0, 0.0};
        result.goal_base = {4.0, 4.0, std::numbers::pi / 2.0};
        result.allow_reverse = false;
    } else if (name == "reverse") {
        result.width = 160U;
        result.height = 160U;
        result.start_drive = {5.5, 4.0, 0.0};
        // The requested goal remains a base_link pose. Converting it with the
        // -0.196 m drive-to-base offset yields a drive-axle goal at x=2.5 m.
        result.goal_base = {2.304, 4.0, 0.0};
        result.allow_reverse = true;
    } else if (name == "tail_sweep") {
        result.width = 240U;
        result.height = 240U;
        result.start_drive = {6.0, 6.0, 0.0};
        // Drive-axle goal is (6, 7, pi/2); this is its base_link pose.
        result.goal_base = {6.0, 6.804, std::numbers::pi / 2.0};
        result.allow_reverse = false;
    } else {
        fail("unknown lattice IPC scenario: " + name);
    }
    result.cells = makeCells(name, result.width, result.height);
    return result;
}

struct Point {
    double x{};
    double y{};
};

using Polygon = std::vector<Point>;

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

double projection(const Point& point, const Point& axis) noexcept {
    return point.x * axis.x + point.y * axis.y;
}

bool convexPolygon(const Polygon& polygon) noexcept {
    if (polygon.size() < 3U) return false;
    int sign = 0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Point& a = polygon[i];
        const Point& b = polygon[(i + 1U) % polygon.size()];
        const Point& c = polygon[(i + 2U) % polygon.size()];
        const double cross = (b.x - a.x) * (c.y - b.y) -
                             (b.y - a.y) * (c.x - b.x);
        if (std::abs(cross) <= 1e-12) continue;
        const int current_sign = cross > 0.0 ? 1 : -1;
        if (sign != 0 && current_sign != sign) return false;
        sign = current_sign;
    }
    return sign != 0;
}

bool intersectsOccupiedCell(const Polygon& polygon, double min_x, double min_y,
                            double max_x, double max_y) noexcept {
    const std::array<Point, 4> rectangle{{
        {min_x, min_y}, {max_x, min_y}, {max_x, max_y}, {min_x, max_y}}};
    const auto separated = [&](const Point& axis) {
        double polygon_min = projection(polygon.front(), axis);
        double polygon_max = polygon_min;
        for (std::size_t i = 1; i < polygon.size(); ++i) {
            const double value = projection(polygon[i], axis);
            polygon_min = std::min(polygon_min, value);
            polygon_max = std::max(polygon_max, value);
        }
        double rectangle_min = projection(rectangle.front(), axis);
        double rectangle_max = rectangle_min;
        for (std::size_t i = 1; i < rectangle.size(); ++i) {
            const double value = projection(rectangle[i], axis);
            rectangle_min = std::min(rectangle_min, value);
            rectangle_max = std::max(rectangle_max, value);
        }
        return polygon_max < rectangle_min - 1e-12 ||
               rectangle_max < polygon_min - 1e-12;
    };

    if (separated({1.0, 0.0}) || separated({0.0, 1.0})) return false;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const Point& a = polygon[i];
        const Point& b = polygon[(i + 1U) % polygon.size()];
        if (separated({-(b.y - a.y), b.x - a.x})) return false;
    }
    return true;
}

bool referencePoseFree(const Scenario& scenario,
                       const std::vector<FootprintPoint>& footprint,
                       const Pose2D& pose) {
    if (!finite(pose)) return false;
    const Polygon polygon = transformPolygon(footprint, pose);
    if (!convexPolygon(polygon)) fail("reference SAT requires a convex footprint");
    double min_x = polygon.front().x;
    double min_y = polygon.front().y;
    double max_x = min_x;
    double max_y = min_y;
    for (const Point& point : polygon) {
        min_x = std::min(min_x, point.x);
        min_y = std::min(min_y, point.y);
        max_x = std::max(max_x, point.x);
        max_y = std::max(max_y, point.y);
    }
    const double map_width = static_cast<double>(scenario.width) * kResolution;
    const double map_height = static_cast<double>(scenario.height) * kResolution;
    if (min_x < 0.0 || min_y < 0.0 || max_x > map_width || max_y > map_height) {
        return false;
    }

    const std::size_t first_x = static_cast<std::size_t>(std::floor(min_x / kResolution));
    const std::size_t first_y = static_cast<std::size_t>(std::floor(min_y / kResolution));
    const std::size_t last_x = std::min(
        scenario.width - 1U,
        static_cast<std::size_t>(std::floor(max_x / kResolution)));
    const std::size_t last_y = std::min(
        scenario.height - 1U,
        static_cast<std::size_t>(std::floor(max_y / kResolution)));
    for (std::size_t y = first_y; y <= last_y; ++y) {
        for (std::size_t x = first_x; x <= last_x; ++x) {
            if (scenario.cells[y * scenario.width + x] <= 0) continue;
            const double cell_min_x = static_cast<double>(x) * kResolution;
            const double cell_min_y = static_cast<double>(y) * kResolution;
            if (intersectsOccupiedCell(polygon, cell_min_x, cell_min_y,
                                       cell_min_x + kResolution,
                                       cell_min_y + kResolution)) {
                return false;
            }
        }
    }
    return true;
}

double footprintRadius(const std::vector<FootprintPoint>& footprint) noexcept {
    double radius = 0.0;
    for (const FootprintPoint& point : footprint) {
        radius = std::max(radius, std::hypot(point.x, point.y));
    }
    return radius;
}

bool referenceMotionFree(const Scenario& scenario,
                         const std::vector<FootprintPoint>& footprint,
                         const Pose2D& from, const Pose2D& to) {
    if (!finite(from) || !finite(to)) return false;
    const double distance = std::hypot(to.x - from.x, to.y - from.y);
    const double yaw_delta = angleDelta(from.yaw, to.yaw);
    const double swept = distance + footprintRadius(footprint) * std::abs(yaw_delta);
    const std::size_t steps = std::max<std::size_t>(
        1U, static_cast<std::size_t>(std::ceil(swept / kSampleSpacing)));
    for (std::size_t i = 0; i <= steps; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(steps);
        const Pose2D pose{
            (1.0 - t) * from.x + t * to.x,
            (1.0 - t) * from.y + t * to.y,
            normalizeAngle(from.yaw + t * yaw_delta)};
        if (!referencePoseFree(scenario, footprint, pose)) return false;
    }
    return true;
}

void publishMap(rbt::SlicePublisher<wire::MapHeader, std::int8_t>& publisher,
                const Scenario& scenario, const rbt::time::HostClock& clock) {
    auto sample = native::take(publisher.loan_slice(scenario.cells.size()));
    auto& header = sample.header();
    header = {};
    header.stamp = native::encodeStamp(native::now(clock));
    header.version = 1U;
    header.width = static_cast<std::uint32_t>(scenario.width);
    header.height = static_cast<std::uint32_t>(scenario.height);
    header.resolution = kResolution;
    std::copy_n("map", 3, header.frame);
    std::copy(scenario.cells.begin(), scenario.cells.end(), sample.payload().begin());
    if (!sample.send()) fail("occupancy map send failed");
}

void publishMapTransform(rbt::tf::DynamicTfPublisher& publisher,
                         const rbt::time::HostClock& clock) {
    publisher.publish({rbt::tf::FrameId("map"), rbt::tf::FrameId("odom"),
                       native::now(clock), 0U, 1U,
                       rbt::tf::Transform3(rbt::tf::Vec3(0.0, 0.0, 0.0),
                                           rbt::tf::Quaternion(0.0, 0.0, 0.0, 1.0))});
}

void publishBaseTransform(rbt::tf::DynamicTfPublisher& publisher,
                          const wire::BaseState& state,
                          const RobotGeometry& geometry,
                          const rbt::time::HostClock& clock,
                          std::uint64_t& last_sequence) {
    const auto current = native::now(clock);
    const auto stamp_ns = native::decodeStamp(state.stamp, current.clock);
    if (state.sequence == 0U || state.sequence <= last_sequence) {
        fail("base-state TF sequence did not increase");
    }
    if (!fresh(stamp_ns, current.ns, 250'000'000)) {
        fail("base-state TF stamp was stale or future-dated");
    }
    const Pose2D drive_pose{state.x, state.y, state.yaw};
    const Pose2D base_pose = driveToBase(drive_pose, geometry);
    if (!finite(base_pose)) fail("base-link transform pose was not finite");
    last_sequence = state.sequence;
    const rbt::time::Timestamp original_stamp{current.clock, stamp_ns};
    const rbt::tf::Transform3 transform(
        rbt::tf::Vec3(base_pose.x, base_pose.y, 0.0),
        rbt::tf::Quaternion(0.0, 0.0, std::sin(base_pose.yaw / 2.0),
                            std::cos(base_pose.yaw / 2.0)));
    try {
        publisher.publish({rbt::tf::FrameId("odom"), rbt::tf::FrameId("base_link"),
                           original_stamp, 0U, 1U, transform});
    } catch (const rbt::tf::TfTransportError& error) {
        if (error.stream_error() != rbt::StreamError::QueueFull) throw;
    }
}

std::string frameName(const std::uint8_t* bytes, std::size_t size) {
    const auto end = std::find(bytes, bytes + size, static_cast<std::uint8_t>(0));
    return std::string(reinterpret_cast<const char*>(bytes),
                       static_cast<std::size_t>(end - bytes));
}

void updatePath(const wire::PathHeader& header,
                const std::span<const wire::PathPoint>& points,
                std::uint64_t goal_id, const Scenario& scenario,
                const RobotGeometry& geometry, Summary& summary) {
    if (header.goal_id != goal_id || points.empty()) return;
    if (frameName(header.frame, std::size(header.frame)) != "map") {
        fail("planned path did not use the map frame");
    }
    std::vector<Pose2D> path;
    path.reserve(points.size());
    for (const wire::PathPoint& point : points) {
        const Pose2D base_pose{point.x, point.y, point.yaw};
        if (!finite(base_pose)) fail("planned path contained a non-finite pose");
        path.push_back(baseToDrive(base_pose, geometry));
    }
    summary.path_drive = std::move(path);
    summary.have_path = true;
    validatePath(scenario, geometry, summary);
}

void validatePath(const Scenario& scenario, const RobotGeometry& geometry,
                  Summary& summary) {
    if (summary.path_drive.empty()) fail("successful goal published no path");
    ++summary.path_messages_checked;
    summary.path_points = static_cast<std::uint64_t>(summary.path_drive.size());
    bool poses_free = true;
    bool segments_free = true;
    summary.path_turn_while_moving = false;
    summary.planned_first_turn_translation.reset();
    summary.maximum_path_yaw_change = 0.0;
    std::uint64_t reverse_path_segments = 0U;
    for (std::size_t i = 0; i < summary.path_drive.size(); ++i) {
        const Pose2D& pose = summary.path_drive[i];
        if (!referencePoseFree(scenario, geometry.footprint, pose)) {
            poses_free = false;
        }
        if (i == 0U) continue;
        const Pose2D& previous = summary.path_drive[i - 1U];
        const double distance = std::hypot(pose.x - previous.x, pose.y - previous.y);
        const double yaw_change = std::abs(angleDelta(previous.yaw, pose.yaw));
        summary.maximum_path_yaw_change = std::max(summary.maximum_path_yaw_change,
                                                   yaw_change);
        if (distance > 0.03 && yaw_change > 0.02) {
            summary.path_turn_while_moving = true;
        }
        if (!referenceMotionFree(scenario, geometry.footprint, previous, pose)) {
            segments_free = false;
        }
        if (summary.scenario == "reverse" && yaw_change < 0.12) {
            const double forward_progress =
                (pose.x - previous.x) * std::cos(previous.yaw) +
                (pose.y - previous.y) * std::sin(previous.yaw);
            if (forward_progress < -0.02) ++reverse_path_segments;
        }
    }
    summary.planned_poses_free = summary.planned_poses_free && poses_free;
    summary.planned_segments_free = summary.planned_segments_free && segments_free;
    summary.reverse_path_segments = reverse_path_segments;

    const Pose2D start = scenario.start_drive;
    for (const Pose2D& pose : summary.path_drive) {
        if (std::abs(angleDelta(start.yaw, pose.yaw)) >= 0.35) {
            summary.planned_first_turn_translation =
                std::hypot(pose.x - start.x, pose.y - start.y);
            break;
        }
    }
}

std::string jsonEscape(std::string_view value) {
    std::ostringstream escaped;
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') escaped << '\\';
        if (character >= 0x20U) escaped << static_cast<char>(character);
    }
    return escaped.str();
}

void writeResult(const fs::path& output_directory, const Scenario& scenario,
                 const RobotGeometry& geometry, Summary& summary) {
    fs::create_directories(output_directory);
    const fs::path path_file = output_directory / "path.csv";
    std::ofstream path_output(path_file, std::ios::out | std::ios::trunc);
    if (!path_output) throw std::runtime_error("cannot create path.csv");
    path_output << "index,drive_x,drive_y,drive_yaw,pose_free,segment_free\n"
                << std::setprecision(17);
    for (std::size_t i = 0; i < summary.path_drive.size(); ++i) {
        const Pose2D& pose = summary.path_drive[i];
        const bool pose_free = referencePoseFree(scenario, geometry.footprint, pose);
        const bool segment_free = i == 0U || referenceMotionFree(
            scenario, geometry.footprint, summary.path_drive[i - 1U], pose);
        path_output << i << ',' << pose.x << ',' << pose.y << ',' << pose.yaw << ','
                    << (pose_free ? 1 : 0) << ',' << (segment_free ? 1 : 0) << '\n';
    }
    path_output.flush();
    if (!path_output) throw std::runtime_error("failed writing path.csv");

    const fs::path telemetry_file = output_directory / "telemetry.csv";
    std::ofstream telemetry_output(telemetry_file, std::ios::out | std::ios::trunc);
    if (!telemetry_output) throw std::runtime_error("cannot create telemetry.csv");
    telemetry_output << "relative_stamp_seconds,drive_x,drive_y,drive_yaw,base_link_x,"
                        "base_link_y,base_link_yaw,linear_x,angular_z,nav_state,nav_error,"
                        "base_watchdog,guard_watchdog\n" << std::setprecision(17);
    for (const TelemetryRow& row : summary.telemetry) {
        telemetry_output << row.seconds << ',' << row.drive.x << ',' << row.drive.y << ','
                         << row.drive.yaw << ',' << row.base_link.x << ',' << row.base_link.y
                         << ',' << row.base_link.yaw << ',' << row.linear_x << ','
                         << row.angular_z << ',' << row.nav_state << ',' << row.nav_error << ','
                         << row.base_watchdog << ',' << row.guard_watchdog << '\n';
    }
    telemetry_output.flush();
    if (!telemetry_output) throw std::runtime_error("failed writing telemetry.csv");

    const fs::path result_file = output_directory / "result.json";
    std::ofstream result_output(result_file, std::ios::out | std::ios::trunc);
    if (!result_output) throw std::runtime_error("cannot create result.json");
    result_output << std::setprecision(17)
        << "{\n  \"passed\": " << (summary.passed ? "true" : "false")
        << ",\n  \"scenario\": \"" << jsonEscape(summary.scenario) << "\""
        << ",\n  \"error\": \"" << jsonEscape(summary.error) << "\""
        << ",\n  \"map_version\": " << summary.map_version
        << ",\n  \"nav_state\": " << summary.nav_state
        << ",\n  \"nav_error\": " << summary.nav_error
        << ",\n  \"base_frames\": " << summary.base_frames
        << ",\n  \"guard_frames\": " << summary.guard_frames
        << ",\n  \"final_command_frames\": " << summary.command_frames
        << ",\n  \"path_points\": " << summary.path_points
        << ",\n  \"path_messages_checked\": " << summary.path_messages_checked
        << ",\n  \"planned_poses_free\": " << (summary.planned_poses_free ? "true" : "false")
        << ",\n  \"planned_segments_free\": " << (summary.planned_segments_free ? "true" : "false")
        << ",\n  \"actual_poses_free\": " << (summary.actual_poses_free ? "true" : "false")
        << ",\n  \"actual_segments_free\": " << (summary.actual_segments_free ? "true" : "false")
        << ",\n  \"reverse_path_segments\": " << summary.reverse_path_segments
        << ",\n  \"negative_base_samples\": " << summary.negative_base_samples
        << ",\n  \"negative_final_commands\": " << summary.negative_final_commands
        << ",\n  \"moving_final_commands\": " << summary.moving_final_commands
        << ",\n  \"maximum_abs_angular_command\": " << summary.maximum_abs_angular_command
        << ",\n  \"maximum_path_yaw_change\": " << summary.maximum_path_yaw_change
        << ",\n  \"path_turn_while_moving\": " << (summary.path_turn_while_moving ? "true" : "false")
        << ",\n  \"planned_first_turn_translation\": ";
    if (summary.planned_first_turn_translation) {
        result_output << *summary.planned_first_turn_translation;
    } else {
        result_output << "null";
    }
    result_output << ",\n  \"actual_first_turn_translation\": ";
    if (summary.actual_first_turn_translation) {
        result_output << *summary.actual_first_turn_translation;
    } else {
        result_output << "null";
    }
    result_output
        << ",\n  \"direct_turn_start_free\": "
        << (summary.direct_turn_start_free ? "true" : "false")
        << ",\n  \"direct_turn_end_free\": "
        << (summary.direct_turn_end_free ? "true" : "false")
        << ",\n  \"direct_turn_sweep_free\": "
        << (summary.direct_turn_sweep_free ? "true" : "false")
        << ",\n  \"goal_base\": [" << scenario.goal_base.x << ',' << scenario.goal_base.y
        << ',' << scenario.goal_base.yaw << ']'
        << ",\n  \"final_base_link\": [" << summary.final_base_link.x << ','
        << summary.final_base_link.y << ',' << summary.final_base_link.yaw << ']'
        << ",\n  \"final_drive\": [" << summary.final_drive.x << ','
        << summary.final_drive.y << ',' << summary.final_drive.yaw << ']'
        << ",\n  \"final_position_error\": ";
    if (std::isfinite(summary.final_position_error)) result_output << summary.final_position_error;
    else result_output << "null";
    result_output << ",\n  \"final_drive_position_error\": ";
    if (std::isfinite(summary.final_drive_position_error)) {
        result_output << summary.final_drive_position_error;
    } else {
        result_output << "null";
    }
    result_output << ",\n  \"final_yaw_error\": ";
    if (std::isfinite(summary.final_yaw_error)) result_output << summary.final_yaw_error;
    else result_output << "null";
    result_output
        << ",\n  \"geometry_drive_to_base\": [" << geometry.drive_to_base.x << ','
        << geometry.drive_to_base.y << ',' << geometry.drive_to_base.yaw << "]\n}\n";
    result_output.flush();
    if (!result_output) throw std::runtime_error("failed writing result.json");
}

void runScenario(const Scenario& scenario, const RobotGeometry& geometry,
                 const std::string& prefix, const std::string& map_id,
                 const std::string& odom_id, const fs::path& output_directory,
                 Summary& summary) {
    summary.goal_base = scenario.goal_base;
    if (scenario.name == "tail_sweep") {
        const Pose2D same_position_goal{scenario.start_drive.x, scenario.start_drive.y,
                                        scenario.goal_base.yaw};
        summary.direct_turn_start_free = referencePoseFree(
            scenario, geometry.footprint, scenario.start_drive);
        summary.direct_turn_end_free = referencePoseFree(
            scenario, geometry.footprint, same_position_goal);
        summary.direct_turn_sweep_free = referenceMotionFree(
            scenario, geometry.footprint, scenario.start_drive, same_position_goal);
        if (!summary.direct_turn_start_free || !summary.direct_turn_end_free
            || summary.direct_turn_sweep_free) {
            fail("tail-sweep fixture does not have free endpoints and a blocked in-place turn");
        }
    }

    auto clock = native::hostClock();
    const auto cells = scenario.cells;
    auto map_publisher = native::take(
        rbt::SlicePublisher<wire::MapHeader, std::int8_t>::create(
            native::stream(prefix, "map", wire::OCCUPANCY_MAP_DESCRIPTOR),
            wire::OCCUPANCY_MAP_DESCRIPTOR, cells.size()));
    auto map_tf = rbt::tf::DynamicTfPublisher::open(map_id);
    auto base_tf = rbt::tf::DynamicTfPublisher::open(odom_id);
    auto goals = native::publisher<wire::GoalCommand>(prefix, "goal");
    auto base_subscriber = native::subscriber<wire::BaseState>(prefix, "base_state");
    auto status_subscriber = native::subscriber<wire::NavigationStatus>(prefix, "status");
    auto guard_subscriber = native::subscriberAt<wire::GuardState>(prefix + "/guard_state");
    const std::string cmd_vel_topic = "/" + prefix + "/cmd_vel";
    if (cmd_vel_topic == "/cmd_vel") fail("fixture refuses to use the physical cmd_vel topic");
    auto final_subscriber = native::subscriberAt<wire::VelocityCommand>(cmd_vel_topic);
    auto path_subscriber = native::take(
        rbt::SliceSubscriber<wire::PathHeader, wire::PathPoint>::create(
            native::stream(prefix, "path", wire::PLANNED_PATH_DESCRIPTOR),
            wire::PLANNED_PATH_DESCRIPTOR));

    std::uint64_t last_base_tf_sequence = 0;
    std::uint64_t last_guard_sequence = 0;
    std::uint64_t last_command_sequence = 0;
    std::uint64_t last_base_sequence = 0;
    std::optional<Pose2D> previous_drive_pose{scenario.start_drive};
    std::optional<std::chrono::steady_clock::time_point> goal_started;
    std::optional<std::chrono::steady_clock::time_point> success_seen;
    std::uint32_t latest_guard_watchdog = 0U;
    auto next_map = std::chrono::steady_clock::time_point::min();
    auto next_goal = std::chrono::steady_clock::time_point::min();
    const auto started = std::chrono::steady_clock::now();
    const auto start_stamp_ns = native::now(clock).ns;
    const auto fixture_deadline = started + std::chrono::seconds(75);
    bool goal_sent = false;

    std::cout << "lattice native fixture ready: scenario=" << scenario.name
              << " prefix=" << prefix << " goal_base=(" << scenario.goal_base.x << ','
              << scenario.goal_base.y << ',' << scenario.goal_base.yaw << ')' << std::endl;

    while (!native::interrupted) {
        const auto wall_now = std::chrono::steady_clock::now();
        if (wall_now >= fixture_deadline) fail("fixture exceeded its 75 second wall-clock budget");
        const auto current = native::now(clock);
        if (next_map == std::chrono::steady_clock::time_point::min()
            || wall_now >= next_map) {
            publishMap(map_publisher, scenario, clock);
            next_map = wall_now + std::chrono::seconds(1);
        }
        publishMapTransform(map_tf, clock);

        native::drain(base_subscriber, [&](const wire::BaseState& state) {
            const auto receipt = native::now(clock);
            const auto stamp_ns = native::decodeStamp(state.stamp, receipt.clock);
            if (state.sequence == 0U || state.sequence <= last_base_sequence) {
                fail("base-state sequence did not increase");
            }
            if (!fresh(stamp_ns, receipt.ns, 250'000'000)) {
                fail("base-state sample was stale or future-dated");
            }
            const Pose2D drive_pose{state.x, state.y, state.yaw};
            if (!finite(drive_pose) || !std::isfinite(state.linear_x)
                || !std::isfinite(state.angular_z)) {
                fail("base-state telemetry was non-finite");
            }
            last_base_sequence = state.sequence;
            ++summary.base_frames;
            if (!referencePoseFree(scenario, geometry.footprint, drive_pose)) {
                summary.actual_poses_free = false;
            }
            if (previous_drive_pose && !referenceMotionFree(
                    scenario, geometry.footprint, *previous_drive_pose, drive_pose)) {
                summary.actual_segments_free = false;
            }
            if (state.linear_x < -0.02) ++summary.negative_base_samples;
            const Pose2D base_pose = driveToBase(drive_pose, geometry);
            if (!summary.actual_turn_checked
                && std::abs(angleDelta(scenario.start_drive.yaw, drive_pose.yaw)) >= 0.35) {
                summary.actual_turn_checked = true;
                summary.actual_first_turn_translation =
                    std::hypot(drive_pose.x - scenario.start_drive.x,
                               drive_pose.y - scenario.start_drive.y);
            }
            const double relative_seconds =
                static_cast<double>(stamp_ns - start_stamp_ns) / static_cast<double>(kSecond);
            summary.telemetry.push_back({relative_seconds, drive_pose, base_pose,
                state.linear_x, state.angular_z, summary.nav_state, summary.nav_error,
                state.watchdog_expired, latest_guard_watchdog});
            previous_drive_pose = drive_pose;
            summary.final_drive = drive_pose;
            summary.final_base_link = base_pose;
            summary.have_final_pose = true;
            publishBaseTransform(base_tf, state, geometry, clock, last_base_tf_sequence);
        });

        native::drain(status_subscriber, [&](const wire::NavigationStatus& status) {
            (void)native::decodeStamp(status.stamp, current.clock);
            summary.nav_state = status.state;
            summary.nav_error = status.error;
            summary.map_version = status.map_version;
        });
        native::drain(guard_subscriber, [&](const wire::GuardState& state) {
            (void)native::decodeStamp(state.stamp, current.clock);
            if (state.sequence == 0U || state.sequence <= last_guard_sequence) {
                fail("guard-state sequence did not increase");
            }
            if (state.allowed_source != 1U || state.output_source != 100U) {
                fail("guard reported unexpected command sources");
            }
            last_guard_sequence = state.sequence;
            ++summary.guard_frames;
            latest_guard_watchdog = state.watchdog_expired;
        });
        native::drain(final_subscriber, [&](const wire::VelocityCommand& command) {
            const auto stamp_ns = native::decodeStamp(command.stamp, current.clock);
            if (command.source_id != 100U || command.sequence == 0U
                || command.sequence <= last_command_sequence) {
                fail("final command source or sequence was invalid");
            }
            if (command.valid_until_ns <= stamp_ns
                || command.valid_until_ns - stamp_ns > 100'000'000) {
                fail("guard final command validity was not within 100 ms");
            }
            if (!std::isfinite(command.linear_x) || !std::isfinite(command.angular_z)) {
                fail("final command was non-finite");
            }
            last_command_sequence = command.sequence;
            ++summary.command_frames;
            if (command.valid_until_ns <= current.ns) return;
            if (std::abs(command.linear_x) + std::abs(command.angular_z) > 0.05) {
                ++summary.moving_final_commands;
            }
            if (command.linear_x < -0.02) ++summary.negative_final_commands;
            summary.maximum_abs_angular_command = std::max(
                summary.maximum_abs_angular_command, std::abs(command.angular_z));
        });
        for (std::size_t i = 0; i < rbt::MAX_DRAIN_PER_SOURCE; ++i) {
            auto received = native::take(path_subscriber.receive());
            if (!received) break;
            updatePath(received->header(), received->payload(), 1U,
                       scenario, geometry, summary);
        }

        const bool ready = summary.base_frames > 0U && summary.guard_frames > 0U
            && summary.map_version == 1U
            && wall_now - started >= std::chrono::milliseconds(750);
        if (ready && !goal_sent && wall_now >= next_goal) {
            const auto stamp = native::now(clock);
            native::publish(goals, wire::GoalCommand{
                native::encodeStamp(stamp), 1U, 0U, 0U,
                scenario.goal_base.x, scenario.goal_base.y, scenario.goal_base.yaw,
                60 * kSecond});
            goal_sent = true;
            goal_started = wall_now;
            next_goal = wall_now + std::chrono::milliseconds(100);
        } else if (goal_sent && !success_seen && summary.nav_state == kIdle
                   && wall_now >= next_goal) {
            // Repeat until acknowledged; this matches the existing native
            // fixture's protection against an IPC subscriber startup race.
            const auto stamp = native::now(clock);
            native::publish(goals, wire::GoalCommand{
                native::encodeStamp(stamp), 1U, 0U, 0U,
                scenario.goal_base.x, scenario.goal_base.y, scenario.goal_base.yaw,
                60 * kSecond});
            next_goal = wall_now + std::chrono::milliseconds(100);
        }

        if (goal_started && wall_now - *goal_started > std::chrono::seconds(60)) {
            fail("navigation exceeded the 60 second goal budget; state="
                 + std::to_string(summary.nav_state) + " error="
                 + std::to_string(summary.nav_error));
        }
        if (summary.nav_state == kFailed) {
            fail("navigation failed with error=" + std::to_string(summary.nav_error));
        }
        if (summary.nav_state == kSucceeded) {
            if (!success_seen) success_seen = wall_now;
            if (wall_now - *success_seen >= std::chrono::milliseconds(250)) break;
        }

        std::this_thread::sleep_until(wall_now + std::chrono::milliseconds(20));
    }
    if (native::interrupted) fail("fixture interrupted before goal success");
    if (summary.nav_state != kSucceeded) fail("navigation did not report success");
    if (!summary.have_final_pose) fail("no final simulated base pose was observed");
    if (summary.guard_frames == 0U || summary.command_frames == 0U
        || summary.moving_final_commands == 0U) {
        fail("navigation did not produce guarded, moving final commands");
    }
    if (!summary.have_path || summary.path_drive.empty()) {
        fail("successful goal published no path");
    }
    if (!summary.planned_poses_free || !summary.planned_segments_free) {
        fail("independent SAT found a colliding planned pose or segment");
    }
    if (!summary.actual_poses_free || !summary.actual_segments_free) {
        fail("independent SAT found a colliding actual base pose or segment");
    }
    summary.final_position_error = std::hypot(
        summary.final_base_link.x - scenario.goal_base.x,
        summary.final_base_link.y - scenario.goal_base.y);
    const Pose2D goal_drive = baseToDrive(scenario.goal_base, geometry);
    summary.final_drive_position_error = std::hypot(
        summary.final_drive.x - goal_drive.x, summary.final_drive.y - goal_drive.y);
    summary.final_yaw_error = std::abs(angleDelta(
        scenario.goal_base.yaw, summary.final_base_link.yaw));
    // These are the existing controller pose tolerances; this fixture does not
    // add another navigation convergence criterion.
    if (summary.final_drive_position_error > kPositionTolerance + 1e-6
        || summary.final_yaw_error > kYawTolerance + 1e-6) {
        fail("successful navigation ended outside the existing drive-axis RPP tolerances");
    }

    if (scenario.name == "turn") {
        if (!summary.path_turn_while_moving || summary.maximum_abs_angular_command < 0.05) {
            fail("turn scenario did not execute a curved/turning path");
        }
    } else if (scenario.name == "reverse") {
        if (summary.reverse_path_segments == 0U || summary.negative_base_samples == 0U
            || summary.negative_final_commands == 0U) {
            fail("reverse corridor scenario did not plan and execute reverse motion");
        }
    }
    summary.passed = true;
    writeResult(output_directory, scenario, geometry, summary);
    std::cout << "PASS lattice native " << scenario.name
              << ": path_points=" << summary.path_points
              << " base_frames=" << summary.base_frames
              << " reverse_samples=" << summary.negative_base_samples
              << " final_base_error=" << summary.final_position_error
              << " final_drive_error=" << summary.final_drive_position_error << ','
              << summary.final_yaw_error << '\n';
}

int run(int argc, char** argv) {
    native::Options options(argc, argv);
    native::installSignals();
    const std::string scenario_name = options.get("scenario", "turn");
    const Scenario scenario = makeScenario(scenario_name);
    const std::string prefix = options.get("prefix");
    const std::string map_id = options.get("tf-map");
    const std::string odom_id = options.get("tf-odom");
    const fs::path output_directory(options.get("output"));
    const std::string geometry_path = options.get("robot-geometry");
    if (prefix.empty() || map_id.empty() || odom_id.empty() || map_id == odom_id
        || output_directory.empty() || geometry_path.empty()) {
        fail("fixture requires --prefix, distinct TF IDs, --output, and --robot-geometry");
    }
    const RobotGeometry geometry = loadRobotGeometry(geometry_path);
    if (std::abs(geometry.drive_to_base.x + 0.196) > 1e-9
        || std::abs(geometry.drive_to_base.y) > 1e-9
        || std::abs(normalizeAngle(geometry.drive_to_base.yaw)) > 1e-9) {
        fail("fixture expects drive_to_base [-0.196, 0, 0]");
    }
    Summary summary;
    summary.scenario = scenario_name;
    try {
        runScenario(scenario, geometry, prefix, map_id, odom_id, output_directory, summary);
    } catch (const std::exception& error) {
        summary.error = error.what();
        try {
            writeResult(output_directory, scenario, geometry, summary);
        } catch (const std::exception& output_error) {
            std::cerr << "fixture output error: " << output_error.what() << '\n';
        }
        std::cerr << "FAIL lattice native " << scenario_name << ": " << summary.error << '\n';
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) try {
    return run(argc, argv);
} catch (const std::exception& error) {
    std::cerr << "lattice native fixture: " << error.what() << '\n';
    return 1;
}

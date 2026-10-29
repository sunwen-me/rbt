#pragma once

#include <cmath>
#include <cstdint>
#include <memory>
#include <numbers>
#include <stop_token>
#include <vector>

namespace rbt::navigation {

// Nanoseconds in one explicitly selected monotonic clock. The native adapter
// validates the full RBT HostClock identity before converting wire timestamps.
// Simulators inject a separate clock and never mix it with native timestamps.
using TimePoint = std::int64_t;
inline constexpr TimePoint second = 1'000'000'000;
inline bool fresh(TimePoint stamp, TimePoint now, TimePoint max_age) noexcept {
    return stamp >= 0 && now >= stamp && max_age > 0 && now - stamp <= max_age;
}
inline double normalizeAngle(double angle) noexcept {
    return std::remainder(angle, 2.0 * std::numbers::pi);
}

// map: fixed global planning frame; odom: continuous local frame;
// base_link: robot body, +x forward, +y left, +yaw counterclockwise.
struct Pose2D { double x{}, y{}, yaw{}; }; // metres / radians in map
struct Twist2D { double linear_x{}, angular_z{}; }; // body forward / yaw rate
inline bool finite(const Pose2D& p) noexcept {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.yaw);
}
inline bool finite(const Twist2D& v) noexcept {
    return std::isfinite(v.linear_x) && std::isfinite(v.angular_z);
}
inline double distance(const Pose2D& a, const Pose2D& b) noexcept {
    return std::hypot(a.x - b.x, a.y - b.y);
}
struct RobotState {
    Pose2D pose;
    Twist2D velocity;
    TimePoint tf_stamp_ns{-1}, odom_stamp_ns{-1};
    bool localized{false};
};
struct Path2D {
    std::vector<Pose2D> poses;
    std::uint64_t map_version{}, goal_id{};
};
struct NavigationGoal {
    std::uint64_t id{}; // nonzero, stable across retransmission
    Pose2D pose;
    TimePoint timeout_ns{120 * second};
};
enum class NavigationState : std::uint32_t {
    Idle, Planning, Controlling, Replanning, Succeeded, Canceled, Failed
};
enum class ErrorCode : std::uint32_t {
    None, InvalidInput, MapUnavailable, StaleMap, LocalizationUnavailable,
    StaleTf, StaleOdometry, StartOutside, GoalOutside, StartOccupied,
    GoalOccupied, NoPath, PlanningTimeout, Canceled, EmptyPath,
    PathDeviation, Collision, ControllerFailure, PlannerFailure,
    NavigationTimeout, GoalConflict
};
struct NavigationStatus {
    NavigationState state{NavigationState::Idle};
    ErrorCode error{ErrorCode::None};
    std::uint64_t goal_id{}, map_version{};
};
class GridMap;
class FootprintCollisionChecker;
struct MapSnapshot {
    std::shared_ptr<const GridMap> grid;
    std::uint64_t version{};
    TimePoint stamp_ns{-1};
    [[nodiscard]] bool valid() const noexcept { return grid && version != 0; }
};
struct PlannerResult {
    ErrorCode error{ErrorCode::None};
    Path2D path;
    std::uint64_t expanded{};
    [[nodiscard]] bool success() const noexcept {
        return error == ErrorCode::None && !path.poses.empty();
    }
};
struct ControllerContext {
    TimePoint now_ns{};
    double dt{0.02};
    Twist2D previous_command;
    MapSnapshot map;
    double robot_radius{0.18};
    TimePoint max_input_age_ns{250'000'000};
    TimePoint max_map_age_ns{2 * second};
    std::shared_ptr<const FootprintCollisionChecker> footprint{};
};
struct ControllerResult {
    Twist2D command;
    ErrorCode error{ErrorCode::None};
    bool reached_goal{false};
};

} // namespace rbt::navigation

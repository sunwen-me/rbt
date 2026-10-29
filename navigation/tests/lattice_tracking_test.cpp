#include <rbt_navigation/controller/feasible_path_handler.hpp>
#include <rbt_navigation/controller/pure_pursuit.hpp>
#include <rbt_navigation/geometry/footprint.hpp>
#include <rbt_navigation/map/grid_map.hpp>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

using namespace rbt::navigation;

namespace {

constexpr double kDt = 0.02;
constexpr std::uint64_t kMapVersion = 9U;
constexpr TimePoint kStartTime = 10 * second;

bool expect(bool condition, std::string_view description) {
    if (!condition) std::cerr << "FAIL: " << description << '\n';
    return condition;
}

std::shared_ptr<const GridMap> makeMap(
    std::vector<std::int8_t> cells = {}, std::size_t width = 300U,
    std::size_t height = 300U, double resolution = 0.05) {
    if (cells.empty()) cells.assign(width * height, 0);
    OccupancyGrid grid;
    grid.width = width;
    grid.height = height;
    grid.resolution = resolution;
    grid.cells = std::move(cells);
    return std::make_shared<const GridMap>(std::move(grid));
}

Path2D makePath(std::vector<Pose2D> poses, std::uint64_t goal_id = 17U) {
    Path2D path;
    path.poses = std::move(poses);
    path.map_version = kMapVersion;
    path.goal_id = goal_id;
    return path;
}

Pose2D integrate(Pose2D pose, Twist2D command) {
    const double next_yaw = pose.yaw + command.angular_z * kDt;
    if (std::abs(command.angular_z) < 1e-12) {
        pose.x += command.linear_x * kDt * std::cos(pose.yaw);
        pose.y += command.linear_x * kDt * std::sin(pose.yaw);
    } else {
        pose.x += command.linear_x / command.angular_z *
                  (std::sin(next_yaw) - std::sin(pose.yaw));
        pose.y -= command.linear_x / command.angular_z *
                  (std::cos(next_yaw) - std::cos(pose.yaw));
    }
    pose.yaw = normalizeAngle(next_yaw);
    return pose;
}

ControllerResult commandAt(RegulatedPurePursuit& controller, const Path2D& path,
                           Pose2D pose, Twist2D previous,
                           const std::shared_ptr<const GridMap>& map,
                           TimePoint now,
                           std::shared_ptr<const FootprintCollisionChecker> footprint = {},
                           double robot_radius = 0.01,
                           std::optional<Twist2D> measured_velocity = std::nullopt) {
    RobotState state{pose, measured_velocity.value_or(previous), now, now, true};
    ControllerContext context;
    context.now_ns = now;
    context.dt = kDt;
    context.previous_command = previous;
    context.map = MapSnapshot{map, kMapVersion, now};
    context.robot_radius = robot_radius;
    context.footprint = std::move(footprint);
    return controller.computeCommand(state, path, context);
}

bool rotationSegmentAdvancesBeforeTranslation(
    const std::shared_ptr<const GridMap>& map) {
    ControllerConfig config;
    config.use_feasible_path_handler = true;
    RegulatedPurePursuit controller(config);
    const Pose2D start{5.0, 5.0, 0.0};
    const Pose2D rotated{5.0, 5.0, std::numbers::pi / 2.0};
    const Path2D path = makePath({start, rotated, {5.0, 8.0, std::numbers::pi / 2.0}});

    Pose2D pose = start;
    Twist2D previous{};
    bool saw_segment_stop = false;
    bool saw_translation = false;
    bool ok = true;
    for (int step = 0; step < 1200; ++step) {
        const TimePoint now = kStartTime + static_cast<TimePoint>(step) * 20'000'000;
        const ControllerResult result = commandAt(controller, path, pose, previous, map, now);
        if (result.error != ErrorCode::None || result.reached_goal) {
            std::cerr << "rotation segment failed at step " << step << " with error "
                      << static_cast<unsigned>(result.error) << '\n';
            return false;
        }
        if (step == 0) {
            ok &= expect(std::abs(result.command.linear_x) < 1e-12 &&
                             result.command.angular_z > 0.0,
                         "same-position yaw change starts with an in-place rotation");
        }
        if (saw_segment_stop && result.command.linear_x > 1e-3) {
            saw_translation = true;
            ok &= expect(std::abs(normalizeAngle(pose.yaw - rotated.yaw)) <=
                             config.yaw_tolerance,
                         "translation begins only after the rotation segment is aligned");
            break;
        }
        if (std::hypot(pose.x - rotated.x, pose.y - rotated.y) <= config.position_tolerance &&
            std::abs(normalizeAngle(pose.yaw - rotated.yaw)) <= config.yaw_tolerance &&
            std::abs(previous.linear_x) < 1e-12 &&
            std::abs(previous.angular_z) < 1e-12 &&
            std::abs(result.command.linear_x) < 1e-12 &&
            std::abs(result.command.angular_z) < 1e-12) {
            saw_segment_stop = true;
        }
        pose = integrate(pose, result.command);
        previous = result.command;
    }
    return ok && expect(saw_segment_stop && saw_translation,
                        "completed rotation advances as a zero, nonterminal segment");
}

bool denseOppositeRotationsRemainOrdered(
    const std::shared_ptr<const GridMap>& map) {
    ControllerConfig config;
    config.use_feasible_path_handler = true;
    RegulatedPurePursuit controller(config);
    const Pose2D start{5.0, 5.0, 0.0};
    std::vector<Pose2D> poses{start};
    constexpr int steps_per_quarter_turn = 16;
    for (int step = 1; step <= steps_per_quarter_turn; ++step) {
        poses.push_back({start.x, start.y,
                         (std::numbers::pi / 2.0) * step / steps_per_quarter_turn});
    }
    for (int step = steps_per_quarter_turn - 1; step >= 0; --step) {
        poses.push_back({start.x, start.y,
                         (std::numbers::pi / 2.0) * step / steps_per_quarter_turn});
    }
    const Pose2D goal{8.0, 5.0, 0.0};
    poses.push_back(goal);
    const Path2D path = makePath(std::move(poses));

    Pose2D pose = start;
    Twist2D previous{};
    bool saw_positive_rotation = false;
    bool saw_negative_rotation = false;
    bool saw_forward = false;
    for (int step = 0; step < 2400; ++step) {
        const TimePoint now = kStartTime + static_cast<TimePoint>(step) * 20'000'000;
        const ControllerResult result = commandAt(controller, path, pose, previous, map, now);
        if (result.error != ErrorCode::None) {
            std::cerr << "opposite-rotation route failed at step " << step
                      << " with error " << static_cast<unsigned>(result.error) << '\n';
            return false;
        }
        if (result.command.angular_z > 1e-3) saw_positive_rotation = true;
        if (result.command.angular_z < -1e-3 &&
            pose.yaw > std::numbers::pi / 4.0) {
            saw_negative_rotation = true;
        }
        if (result.command.linear_x > 1e-3) {
            saw_forward = true;
            if (!saw_positive_rotation || !saw_negative_rotation ||
                std::abs(normalizeAngle(pose.yaw)) > config.yaw_tolerance) {
                return expect(false,
                              "forward motion waits for both rotations to return to the requested heading");
            }
        }
        if (result.reached_goal) {
            return expect(saw_positive_rotation && saw_negative_rotation && saw_forward,
                          "dense opposite rotations are both tracked before the final translation");
        }
        pose = integrate(pose, result.command);
        previous = result.command;
    }
    return expect(false, "dense opposite-rotation route closes at its final goal");
}

bool forwardReverseCuspHasCorrectProgressAndTerminal(
    const std::shared_ptr<const GridMap>& map) {
    ControllerConfig config;
    config.use_feasible_path_handler = true;
    config.allow_reverse = true;
    RegulatedPurePursuit controller(config);
    const Pose2D start{4.0, 5.0, 0.0};
    const Pose2D cusp{6.0, 5.0, 0.0};
    const Pose2D goal{4.0, 5.0, 0.0};
    const Path2D path = makePath({start, cusp, goal});

    Pose2D pose = start;
    Twist2D previous{};
    bool saw_cusp_stop = false;
    bool saw_reverse = false;
    for (int step = 0; step < 2400; ++step) {
        const TimePoint now = kStartTime + static_cast<TimePoint>(step) * 20'000'000;
        const ControllerResult result = commandAt(controller, path, pose, previous, map, now);
        if (result.error != ErrorCode::None) {
            std::cerr << "cusp route failed at step " << step << " with error "
                      << static_cast<unsigned>(result.error) << '\n';
            return false;
        }
        if (result.command.linear_x < -1e-3) {
            saw_reverse = true;
            if (!saw_cusp_stop) {
                return expect(false, "reverse motion starts only after the forward cusp is stopped");
            }
        }
        if (std::hypot(pose.x - cusp.x, pose.y - cusp.y) <= config.position_tolerance &&
            std::abs(previous.linear_x) < 1e-12 &&
            std::abs(previous.angular_z) < 1e-12 &&
            std::abs(result.command.linear_x) < 1e-12 &&
            std::abs(result.command.angular_z) < 1e-12) {
            saw_cusp_stop = true;
            if (result.reached_goal) {
                return expect(false, "a cusp segment cannot report the navigation goal reached");
            }
        }
        if (result.reached_goal) {
            return expect(saw_cusp_stop && saw_reverse &&
                              std::hypot(pose.x - goal.x, pose.y - goal.y) <=
                                  config.position_tolerance &&
                              std::abs(normalizeAngle(pose.yaw - goal.yaw)) <=
                                  config.yaw_tolerance,
                          "only the final reverse segment reports the reached goal");
        }
        pose = integrate(pose, result.command);
        previous = result.command;
    }
    return expect(false, "forward then reverse route closes at its terminal goal");
}

bool pathAndMapReplacementResetProgress(
    const std::shared_ptr<const GridMap>& first_map,
    const std::shared_ptr<const GridMap>& replacement_map) {
    FeasiblePathHandler handler;
    Path2D original = makePath({{2.0, 2.0, 0.0}, {3.0, 2.0, 0.0},
                                {4.0, 2.0, 0.0}}, 55U);
    const auto advanced = handler.update(original, {3.9, 2.0, 0.0},
                                         kMapVersion, first_map.get());
    bool ok = expect(advanced.path.poses.size() == 2U &&
                         advanced.path.poses.front().x == 3.0 &&
                         advanced.path.poses.back().x == 4.0,
                     "handler retains one projection anchor behind monotonic progress");

    Path2D changed_path = makePath({{2.0, 2.0, 0.0}, {2.5, 2.0, 0.0},
                                    {3.0, 2.0, 0.0}}, 55U);
    const auto path_reset = handler.update(changed_path, {2.0, 2.0, 0.0},
                                           kMapVersion, first_map.get());
    ok &= expect(!path_reset.path.poses.empty() &&
                     path_reset.path.poses.front().x == 2.0,
                 "changed path contents reset progress even with the same goal id");

    (void)handler.update(changed_path, {2.9, 2.0, 0.0},
                         kMapVersion, first_map.get());
    const auto map_reset = handler.update(changed_path, {2.0, 2.0, 0.0},
                                          kMapVersion, replacement_map.get());
    ok &= expect(!map_reset.path.poses.empty() &&
                     map_reset.path.poses.front().x == 2.0,
                 "replacing the map resets progress for the same goal and path");
    return ok;
}

bool polygonChecksInteriorObstacle() {
    std::vector<std::int8_t> cells(300U * 300U, 0);
    cells[120U * 300U + 126U] = 100;
    const auto map = makeMap(std::move(cells));
    const auto footprint = std::make_shared<const FootprintCollisionChecker>(
        std::vector<FootprintPoint>{{-0.4, -0.1}, {0.4, -0.1},
                                    {0.4, 0.1}, {-0.4, 0.1}});
    ControllerConfig config;
    RegulatedPurePursuit controller(config);
    const Pose2D start{6.0, 6.0, 0.0};
    const ControllerResult result = commandAt(controller,
        makePath({start, {8.0, 6.0, 0.0}}), start, {}, map, kStartTime,
        footprint, 0.01);
    return expect(result.error == ErrorCode::Collision &&
                      result.command.linear_x == 0.0 && result.command.angular_z == 0.0,
                  "polygon footprint detects an obstacle while its reference point is clear");
}

bool polygonChecksPureRotationSweep() {
    std::vector<std::int8_t> cells(300U * 300U, 0);
    cells[126U * 300U + 126U] = 100;
    const auto map = makeMap(std::move(cells));
    const auto footprint = std::make_shared<const FootprintCollisionChecker>(
        std::vector<FootprintPoint>{{-0.5, -0.05}, {0.5, -0.05},
                                    {0.5, 0.05}, {-0.5, 0.05}});
    ControllerConfig config;
    config.max_angular_acceleration = 100.0;
    RegulatedPurePursuit controller(config);
    const Pose2D start{6.0, 6.0, 0.0};
    const Path2D path = makePath({start, {6.0, 6.0, std::numbers::pi / 2.0}});
    const ControllerResult result = commandAt(controller, path, start, {}, map,
                                               kStartTime, footprint, 0.01);
    return expect(result.error == ErrorCode::Collision &&
                      result.command.linear_x == 0.0 && result.command.angular_z == 0.0,
                  "polygon footprint checks the full pure-rotation sweep");
}

bool measuredAngularOnlyStoppingChecksRotationSweep() {
    constexpr std::size_t map_size = 750U;
    constexpr double resolution = 0.02;
    std::vector<std::int8_t> cells(map_size * map_size, 0);
    // A 0.25 s reaction followed by 2.5 rad/s^2 braking stops near 0.45 rad;
    // this cell lies in the footprint at that endpoint and outside it initially.
    cells[310U * map_size + 321U] = 100;
    const auto map = makeMap(std::move(cells), map_size, map_size, resolution);
    const auto footprint = std::make_shared<const FootprintCollisionChecker>(
        std::vector<FootprintPoint>{{-0.5, -0.05}, {0.5, -0.05},
                                    {0.5, 0.05}, {-0.5, 0.05}});
    RegulatedPurePursuit controller;
    const Pose2D start{6.0, 6.0, 0.0};
    const ControllerResult result = commandAt(
        controller, makePath({start}), start, {}, map, kStartTime,
        footprint, 0.01, Twist2D{0.0, 1.0});
    const bool zero_command_is_free = footprint->commandFree(
        *map, start, {}, 1.0, 0.0);
    return expect(zero_command_is_free && result.error == ErrorCode::Collision &&
                      result.command.linear_x == 0.0 && result.command.angular_z == 0.0,
                  "measured angular-only stopping sweep collides although the final zero command is free");
}

bool measuredMixedStoppingChecksDecelerationEndpoint() {
    constexpr std::size_t map_size = 750U;
    constexpr double resolution = 0.02;
    std::vector<std::int8_t> cells(map_size * map_size, 0);
    cells[304U * map_size + 318U] = 100;
    const auto map = makeMap(std::move(cells), map_size, map_size, resolution);
    const auto footprint = std::make_shared<const FootprintCollisionChecker>(
        std::vector<FootprintPoint>{{-0.03, -0.03}, {0.03, -0.03},
                                    {0.03, 0.03}, {-0.03, 0.03}});
    RegulatedPurePursuit controller;
    const Pose2D start{6.0, 6.0, 0.0};
    const Twist2D measured{0.6, 0.8};
    // The .25 s reaction and independent linear/angular deceleration put the
    // stopping pose near (6.364, 6.080, 0.328), where this cell is occupied.
    const ControllerResult result = commandAt(
        controller, makePath({start}), start, {}, map, kStartTime,
        footprint, 0.01, measured);
    const bool zero_command_is_free = footprint->commandFree(
        *map, start, {}, 1.0, 0.0);
    return expect(zero_command_is_free && result.error == ErrorCode::Collision &&
                      result.command.linear_x == 0.0 && result.command.angular_z == 0.0,
                  "mixed measured v/w stopping endpoint collides despite a free zero final command");
}

bool polygonControllerClosesAnUnobstructedRoute(
    const std::shared_ptr<const GridMap>& map) {
    const auto footprint = std::make_shared<const FootprintCollisionChecker>(
        std::vector<FootprintPoint>{{-0.25, -0.14}, {0.25, -0.14},
                                    {0.25, 0.14}, {-0.25, 0.14}});
    ControllerConfig config;
    config.use_feasible_path_handler = true;
    RegulatedPurePursuit controller(config);
    const Pose2D start{4.0, 5.0, 0.0};
    const Pose2D goal{6.0, 5.0, 0.0};
    const Path2D path = makePath({start, goal});
    Pose2D pose = start;
    Twist2D previous{};
    for (int step = 0; step < 1500; ++step) {
        const TimePoint now = kStartTime + static_cast<TimePoint>(step) * 20'000'000;
        const ControllerResult result = commandAt(controller, path, pose, previous,
                                                  map, now, footprint, 0.01);
        if (result.error != ErrorCode::None) {
            std::cerr << "polygon closed loop failed at step " << step
                      << " with error " << static_cast<unsigned>(result.error) << '\n';
            return false;
        }
        if (result.reached_goal) {
            return expect(std::hypot(pose.x - goal.x, pose.y - goal.y) <=
                              config.position_tolerance &&
                              std::abs(normalizeAngle(pose.yaw - goal.yaw)) <=
                                  config.yaw_tolerance,
                          "polygon tracking reaches the final pose");
        }
        pose = integrate(pose, result.command);
        previous = result.command;
    }
    return expect(false, "polygon tracking reaches the goal on a free map");
}

} // namespace

int main() {
    const auto free_map = makeMap();
    std::vector<std::int8_t> replacement_cells(300U * 300U, 0);
    replacement_cells[150U * 300U + 150U] = 100;
    const auto replacement_map = makeMap(std::move(replacement_cells));
    bool ok = true;
    ok &= rotationSegmentAdvancesBeforeTranslation(free_map);
    ok &= denseOppositeRotationsRemainOrdered(free_map);
    ok &= forwardReverseCuspHasCorrectProgressAndTerminal(free_map);
    ok &= pathAndMapReplacementResetProgress(free_map, replacement_map);
    ok &= polygonChecksInteriorObstacle();
    ok &= polygonChecksPureRotationSweep();
    ok &= measuredAngularOnlyStoppingChecksRotationSweep();
    ok &= measuredMixedStoppingChecksDecelerationEndpoint();
    ok &= polygonControllerClosesAnUnobstructedRoute(free_map);
    if (!ok) return 1;
    std::cout << "lattice tracking, feasible segments and polygon collision checks: PASS\n";
    return 0;
}

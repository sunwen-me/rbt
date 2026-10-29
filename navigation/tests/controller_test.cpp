#include <rbt_navigation/controller/pure_pursuit.hpp>
#include <rbt_navigation/planner/a_star.hpp>
#include <rbt_navigation/safety/safe_base.hpp>

#include <cmath>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace rbt::navigation;

namespace {

constexpr double kDt = 0.02;
constexpr std::uint64_t kMapVersion = 7U;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

std::shared_ptr<const GridMap> makeMap(std::size_t width = 200U,
                                      std::size_t height = 200U,
                                      double resolution = 0.1,
                                      std::vector<std::int8_t> cells = {}) {
    if (cells.empty()) {
        cells.assign(width * height, 0);
    }
    OccupancyGrid grid;
    grid.width = width;
    grid.height = height;
    grid.resolution = resolution;
    grid.origin = Pose2D{};
    grid.cells = std::move(cells);
    return std::make_shared<const GridMap>(std::move(grid));
}

ControllerContext makeContext(const std::shared_ptr<const GridMap>& map,
                              TimePoint now = 5 * second) {
    ControllerContext context;
    context.now_ns = now;
    context.dt = kDt;
    context.map = MapSnapshot{map, kMapVersion, now};
    context.robot_radius = 0.08;
    return context;
}

RobotState makeState(Pose2D pose, TimePoint now = 5 * second) {
    RobotState state;
    state.pose = pose;
    state.tf_stamp_ns = now;
    state.odom_stamp_ns = now;
    state.localized = true;
    return state;
}

Path2D makePath(std::vector<Pose2D> poses) {
    Path2D path;
    path.poses = std::move(poses);
    path.map_version = kMapVersion;
    return path;
}

Pose2D integrate(Pose2D pose, Twist2D command, double dt) {
    const double next_yaw = pose.yaw + command.angular_z * dt;
    if (std::abs(command.angular_z) < 1e-12) {
        pose.x += command.linear_x * dt * std::cos(pose.yaw);
        pose.y += command.linear_x * dt * std::sin(pose.yaw);
    } else {
        pose.x += command.linear_x / command.angular_z * (std::sin(next_yaw) - std::sin(pose.yaw));
        pose.y -= command.linear_x / command.angular_z * (std::cos(next_yaw) - std::cos(pose.yaw));
    }
    pose.yaw = normalizeAngle(next_yaw);
    return pose;
}

bool simulateToGoal(const Path2D& path, Pose2D initial_pose, const std::shared_ptr<const GridMap>& map) {
    DiffDrivePurePursuit controller;
    Twist2D previous{};
    constexpr TimePoint start_time = 10 * second;
    bool reached = false;
    for (int step = 0; step < 1500; ++step) {
        const TimePoint now = start_time + static_cast<TimePoint>(step) * 20'000'000;
        RobotState state = makeState(initial_pose, now);
        state.velocity = previous;
        ControllerContext context = makeContext(map, now);
        context.previous_command = previous;
        const ControllerResult result = controller.computeCommand(state, path, context);
        if (result.error != ErrorCode::None) {
            std::cerr << "simulation failed with controller error " << static_cast<unsigned>(result.error)
                      << " at step " << step << " pose (" << initial_pose.x << ", " << initial_pose.y
                      << ", " << initial_pose.yaw << ") measured (" << state.velocity.linear_x << ", "
                      << state.velocity.angular_z << ") previous (" << previous.linear_x << ", "
                      << previous.angular_z << ")\n";
            return false;
        }
        if (result.reached_goal) {
            reached = true;
            break;
        }
        initial_pose = integrate(initial_pose, result.command, kDt);
        previous = result.command;
    }
    return reached;
}

bool simulateWallRoute() {
    OccupancyGrid grid;
    grid.width = 80U;
    grid.height = 60U;
    grid.resolution = 0.1;
    grid.cells.assign(grid.width * grid.height, 0);
    for (std::size_t y = 0; y < 25U; ++y) {
        for (std::size_t x = 30U; x < 32U; ++x) {
            grid.cells[y * grid.width + x] = 100;
        }
    }
    const auto map = std::make_shared<const GridMap>(std::move(grid));
    const Pose2D start{1.0, 1.0, 0.0};
    const Pose2D goal{5.0, 1.0, 0.0};
    constexpr TimePoint start_time = 10 * second;
    MapSnapshot snapshot{map, kMapVersion, start_time};
    AStarPlanner planner(PlannerConfig{0.28, std::chrono::milliseconds(500)});
    const PlannerResult planned = planner.plan(start, goal, snapshot);
    if (!planned.success()) {
        std::cerr << "wall route planning failed\n";
        return false;
    }
    for (std::size_t index = 1U; index < planned.path.poses.size(); ++index) {
        if (!map->segmentFree(planned.path.poses[index - 1U], planned.path.poses[index], 0.28)) {
            std::cerr << "wall route planner produced a segment inside the configured planning clearance\n";
            return false;
        }
    }

    SafeBaseConfig safe_config;
    safe_config.max_linear = 0.7;
    safe_config.max_angular = 3.0;
    safe_config.max_acceleration = 1.0;
    safe_config.max_angular_acceleration = 3.0;
    SafeBase safe_base(safe_config);
    SimulatedBaseAdapter base(start);
    DiffDrivePurePursuit controller;
    Twist2D previous_command{};
    std::uint64_t sequence = 0U;

    for (int step = 0; step < 2000; ++step) {
        const TimePoint now = start_time + static_cast<TimePoint>(step) * 20'000'000;
        snapshot.stamp_ns = now;
        const Pose2D pose = base.pose();
        RobotState state{pose, base.velocity(), now, now, true};
        ControllerContext context{now, kDt, previous_command, snapshot, 0.18, 250'000'000, 2 * second};
        const ControllerResult result = controller.computeCommand(state, planned.path, context);
        if (result.error != ErrorCode::None) {
            std::cerr << "wall route controller failed with error "
                      << static_cast<unsigned>(result.error) << " at step " << step
                      << " pose (" << pose.x << ", " << pose.y << ", " << pose.yaw << ") measured ("
                      << state.velocity.linear_x << ", " << state.velocity.angular_z << ") previous ("
                      << previous_command.linear_x << ", " << previous_command.angular_z << ")\n";
            return false;
        }
        if (result.reached_goal) {
            return true;
        }

        const VelocityCommand command{1U, ++sequence, now, now + 200'000'000, result.command};
        if (safe_base.accept(command, now) != CommandRejection::None) {
            std::cerr << "wall route command was rejected by SafeBase at step " << step << '\n';
            return false;
        }
        previous_command = result.command;
        const Pose2D previous_pose = base.pose();
        base.apply(safe_base.step(now, kDt), kDt);
        if (!map->isFree(base.pose().x, base.pose().y, 0.18)
            || !map->segmentFree(previous_pose, base.pose(), 0.18)) {
            std::cerr << "wall route simulation collided at step " << step << '\n';
            return false;
        }
    }
    std::cerr << "wall route simulation did not reach its goal\n";
    return false;
}

bool wallCornerFailsClosedWithoutPlanningClearance() {
    OccupancyGrid grid;
    grid.width = 80U;
    grid.height = 60U;
    grid.resolution = 0.1;
    grid.cells.assign(grid.width * grid.height, 0);
    for (std::size_t y = 0; y < 25U; ++y) {
        for (std::size_t x = 30U; x < 32U; ++x) {
            grid.cells[y * grid.width + x] = 100;
        }
    }
    const auto map = std::make_shared<const GridMap>(std::move(grid));
    constexpr TimePoint now = 20 * second;
    const Pose2D start{1.0, 1.0, 0.0};
    const Pose2D goal{5.0, 1.0, 0.0};
    const MapSnapshot snapshot{map, kMapVersion, now};
    AStarPlanner planner(PlannerConfig{0.18, std::chrono::milliseconds(500)});
    const PlannerResult planned = planner.plan(start, goal, snapshot);
    if (!planned.success()) {
        return false;
    }

    // This state is the tight upper-corner approach on the original shortest
    // path. The physical footprint is clear, but the RPP command arc reaches
    // the corner; the controller must return Collision and a zero command.
    RobotState state{{2.37086, 2.22711, 0.842879}, {0.594964, -0.140675}, now, now, true};
    ControllerContext context{now,
                              kDt,
                              state.velocity,
                              snapshot,
                              0.18,
                              250'000'000,
                              2 * second};
    RegulatedPurePursuit controller;
    const ControllerResult result = controller.computeCommand(state, planned.path, context);
    return map->isFree(state.pose.x, state.pose.y, 0.18)
        && result.error == ErrorCode::Collision
        && result.command.linear_x == 0.0 && result.command.angular_z == 0.0;
}

} // namespace

int main() {
    const auto free_map = makeMap();
    DiffDrivePurePursuit controller;
    bool ok = true;

    {
        const RobotState state = makeState({2.0, 2.0, 0.0});
        const Path2D path = makePath({{2.0, 2.0, 0.0}, {7.0, 2.0, 0.0}});
        const ControllerResult result = controller.computeCommand(state, path, makeContext(free_map));
        ok &= expect(result.error == ErrorCode::None && result.command.linear_x > 0.0
                         && std::abs(result.command.angular_z) < 1e-10,
                     "straight path produces forward, straight motion");
    }

    {
        ControllerConfig config;
        config.min_lookahead = 0.8;
        config.max_lookahead = 0.8;
        DiffDrivePurePursuit turn_controller(config);
        const RobotState state = makeState({2.4, 2.0, 0.0});
        const Path2D path = makePath({{2.0, 2.0, 0.0}, {2.5, 2.0, 0.0}, {2.5, 4.0, std::numbers::pi / 2.0}});
        const ControllerResult result = turn_controller.computeCommand(state, path, makeContext(free_map));
        ok &= expect(result.error == ErrorCode::None && result.command.angular_z > 0.0,
                     "left turn produces positive angular velocity");
    }

    {
        ControllerConfig curvature_config;
        curvature_config.use_dynamic_window = false;
        curvature_config.min_lookahead = 0.6;
        curvature_config.max_lookahead = 0.6;
        curvature_config.rotate_to_heading_angle = 1.2;
        curvature_config.max_acceleration = 20.0;
        curvature_config.max_deceleration = 20.0;
        curvature_config.max_angular_acceleration = 20.0;
        curvature_config.max_angular_deceleration = 20.0;
        DiffDrivePurePursuit curvature_controller(curvature_config);
        const Pose2D pose{5.0, 5.0, 0.0};
        const Path2D curved = makePath({pose, {5.2, 5.0, 0.0}, {5.2, 7.0, std::numbers::pi / 2.0}});
        const ControllerResult curved_result = curvature_controller.computeCommand(makeState(pose),
                                                                                    curved,
                                                                                    makeContext(free_map));
        ok &= expect(curved_result.error == ErrorCode::None
                         && std::abs(curved_result.command.linear_x - (0.6 * (0.25 / 0.9))) < 1e-6
                         && curved_result.command.angular_z > 0.0,
                     "curvature regulation slows a tight turn while preserving its steering direction");

        ControllerConfig cost_config = curvature_config;
        cost_config.min_lookahead = 0.3;
        cost_config.max_lookahead = 0.3;
        cost_config.rotate_to_heading_angle = 0.785;
        cost_config.cost_scaling_distance = 0.6;
        cost_config.cost_scaling_gain = 1.0;
        cost_config.regulated_min_speed = 0.01;
        DiffDrivePurePursuit cost_controller(cost_config);
        std::vector<std::int8_t> cells(200U * 200U, 0);
        cells[20U * 200U + 23U] = 100;
        const auto near_obstacle_map = makeMap(200U, 200U, 0.1, std::move(cells));
        const Pose2D cost_pose{2.0, 2.0, 0.0};
        const Path2D straight = makePath({cost_pose, {8.0, 2.0, 0.0}});
        ControllerContext cost_context = makeContext(near_obstacle_map);
        cost_context.robot_radius = 0.05;
        const ControllerResult cost_limited = cost_controller.computeCommand(makeState(cost_pose),
                                                                             straight,
                                                                             cost_context);
        const ControllerResult unregulated = cost_controller.computeCommand(makeState(cost_pose),
                                                                            straight,
                                                                            makeContext(free_map));
        ok &= expect(cost_limited.error == ErrorCode::None && unregulated.error == ErrorCode::None
                         && cost_limited.command.linear_x > 0.0
                         && cost_limited.command.linear_x < unregulated.command.linear_x,
                     "ESDF clearance scales speed near an obstacle");

        ControllerConfig approach_config = cost_config;
        approach_config.cost_scaling_gain = 0.0;
        approach_config.approach_scaling_distance = 0.6;
        approach_config.min_approach_speed = 0.03;
        DiffDrivePurePursuit approach_controller(approach_config);
        const Path2D short_path = makePath({cost_pose, {2.4, 2.0, 0.0}});
        const ControllerResult approach = approach_controller.computeCommand(makeState(cost_pose),
                                                                             short_path,
                                                                             makeContext(free_map));
        const ControllerResult cruise = approach_controller.computeCommand(makeState(cost_pose),
                                                                           straight,
                                                                           makeContext(free_map));
        ok &= expect(approach.error == ErrorCode::None && cruise.error == ErrorCode::None
                         && std::abs(approach.command.linear_x - 0.4) < 1e-6
                         && approach.command.linear_x < cruise.command.linear_x,
                     "approach scaling reduces speed as remaining path length falls below its threshold");

        ControllerConfig dynamic_config = curvature_config;
        dynamic_config.regulated_min_radius = 0.2;
        dynamic_config.max_acceleration = 0.8;
        dynamic_config.max_deceleration = 0.8;
        dynamic_config.max_angular_acceleration = 2.5;
        dynamic_config.max_angular_deceleration = 2.5;
        dynamic_config.use_dynamic_window = true;
        DiffDrivePurePursuit dynamic_controller(dynamic_config);
        ControllerConfig slew_config = dynamic_config;
        slew_config.use_dynamic_window = false;
        DiffDrivePurePursuit slew_controller(slew_config);
        ControllerContext dynamic_context = makeContext(free_map);
        dynamic_context.previous_command.linear_x = 0.2;
        const ControllerResult dynamic_command = dynamic_controller.computeCommand(makeState(pose), curved, dynamic_context);
        const ControllerResult slew_command = slew_controller.computeCommand(makeState(pose), curved, dynamic_context);
        if (dynamic_command.error != ErrorCode::None || slew_command.error != ErrorCode::None
            || dynamic_command.command.linear_x >= slew_command.command.linear_x) {
            std::cerr << "DWPP probe dynamic error=" << static_cast<unsigned>(dynamic_command.error)
                      << " cmd=(" << dynamic_command.command.linear_x << ", " << dynamic_command.command.angular_z
                      << ") slew error=" << static_cast<unsigned>(slew_command.error)
                      << " cmd=(" << slew_command.command.linear_x << ", " << slew_command.command.angular_z << ")\n";
        }
        ok &= expect(dynamic_command.error == ErrorCode::None && slew_command.error == ErrorCode::None
                         && dynamic_command.command.linear_x < slew_command.command.linear_x
                         && std::abs(dynamic_command.command.linear_x - 0.2)
                             <= dynamic_config.max_acceleration * dynamic_context.dt + 1e-12,
                     "DWPP selects a feasible velocity on the curvature line while RPP slew mode remains available");
    }

    {
        const auto map = makeMap(200U, 200U, 0.1);
        const Pose2D pose{10.0, 10.0, 0.4};
        const Path2D path = makePath({pose, {10.0 + 5.0 * std::cos(pose.yaw),
                                             10.0 + 5.0 * std::sin(pose.yaw),
                                             pose.yaw}});
        const ControllerResult original = controller.computeCommand(makeState(pose), path, makeContext(map));
        const double rotation = 1.13;
        const auto rotate = [rotation](Pose2D point) {
            point.yaw = normalizeAngle(point.yaw + rotation);
            return Pose2D{10.0 + std::cos(rotation) * (point.x - 10.0) - std::sin(rotation) * (point.y - 10.0),
                          10.0 + std::sin(rotation) * (point.x - 10.0) + std::cos(rotation) * (point.y - 10.0),
                          point.yaw};
        };
        const Path2D rotated_path = makePath({rotate(path.poses[0]), rotate(path.poses[1])});
        const ControllerResult rotated = controller.computeCommand(makeState(rotate(pose)), rotated_path, makeContext(map));
        ok &= expect(original.error == ErrorCode::None && rotated.error == ErrorCode::None
                         && std::abs(original.command.linear_x - rotated.command.linear_x) < 1e-9
                         && std::abs(original.command.angular_z - rotated.command.angular_z) < 1e-9,
                     "command is invariant to a common global-frame yaw rotation");
    }

    {
        const Pose2D goal{4.0, 4.0, std::numbers::pi / 2.0};
        const Path2D path = makePath({goal});
        const ControllerResult alignment = controller.computeCommand(makeState({goal.x, goal.y, 0.0}),
                                                                      path,
                                                                      makeContext(free_map));
        const ControllerResult reached = controller.computeCommand(makeState(goal), path, makeContext(free_map));
        ok &= expect(alignment.error == ErrorCode::None && !alignment.reached_goal
                         && std::abs(alignment.command.linear_x) < 1e-12 && alignment.command.angular_z > 0.0
                         && reached.error == ErrorCode::None && reached.reached_goal
                         && std::abs(reached.command.linear_x) < 1e-12
                         && std::abs(reached.command.angular_z) < 1e-12,
                     "final yaw is aligned in place and completion is reported");

        std::vector<std::int8_t> occupied_cells(200U * 200U, 0);
        occupied_cells[40U * 200U + 40U] = 100;
        const auto occupied_goal_map = makeMap(200U, 200U, 0.1, std::move(occupied_cells));
        const ControllerResult occupied_goal = controller.computeCommand(makeState(goal),
                                                                          path,
                                                                          makeContext(occupied_goal_map));
        ok &= expect(occupied_goal.error == ErrorCode::Collision && !occupied_goal.reached_goal
                         && occupied_goal.command.linear_x == 0.0 && occupied_goal.command.angular_z == 0.0,
                     "a robot at an occupied goal cannot be reported as reached");
    }

    {
        ControllerConfig config;
        config.max_linear = 0.5;
        config.max_acceleration = 0.2;
        DiffDrivePurePursuit limited(config);
        const Pose2D pose{2.0, 2.0, 0.0};
        const Path2D path = makePath({pose, {8.0, 2.0, 0.0}});
        ControllerContext context = makeContext(free_map);
        context.previous_command = {0.49, 0.0};
        const ControllerResult result = limited.computeCommand(makeState(pose), path, context);
        ok &= expect(result.error == ErrorCode::None
                         && std::abs(result.command.linear_x) <= config.max_linear
                         && std::abs(result.command.angular_z) <= config.max_angular
                         && std::abs(result.command.linear_x - context.previous_command.linear_x)
                             <= config.max_acceleration * context.dt + 1e-12,
                     "speed and acceleration limits constrain the command");
    }

    {
        const Pose2D pose{2.0, 2.0, 0.0};
        const ControllerResult empty = controller.computeCommand(makeState(pose), makePath({}), makeContext(free_map));
        ok &= expect(empty.error == ErrorCode::EmptyPath && empty.command.linear_x == 0.0
                         && empty.command.angular_z == 0.0,
                     "empty path fails closed with zero command");

        Path2D path = makePath({pose, {5.0, 2.0, 0.0}});
        RobotState stale_tf = makeState(pose);
        stale_tf.tf_stamp_ns -= 2 * second;
        const ControllerResult tf_result = controller.computeCommand(stale_tf, path, makeContext(free_map));
        RobotState future_tf = makeState(pose);
        future_tf.tf_stamp_ns += 1;
        const ControllerResult future_tf_result = controller.computeCommand(future_tf, path, makeContext(free_map));
        RobotState stale_odom = makeState(pose);
        stale_odom.odom_stamp_ns -= 2 * second;
        const ControllerResult stale_odom_result = controller.computeCommand(stale_odom, path, makeContext(free_map));
        RobotState future_odom = makeState(pose);
        future_odom.odom_stamp_ns += 1;
        const ControllerResult odom_result = controller.computeCommand(future_odom, path, makeContext(free_map));
        RobotState unlocalized = makeState(pose);
        unlocalized.localized = false;
        const ControllerResult localization_result = controller.computeCommand(unlocalized, path, makeContext(free_map));
        ok &= expect(tf_result.error == ErrorCode::StaleTf && tf_result.command.linear_x == 0.0
                         && future_tf_result.error == ErrorCode::StaleTf && future_tf_result.command.linear_x == 0.0
                         && stale_odom_result.error == ErrorCode::StaleOdometry
                         && stale_odom_result.command.linear_x == 0.0
                         && odom_result.error == ErrorCode::StaleOdometry && odom_result.command.linear_x == 0.0
                         && localization_result.error == ErrorCode::LocalizationUnavailable
                         && localization_result.command.linear_x == 0.0,
                     "stale or future TF/odometry and missing localization fail closed");

        RobotState non_finite_state = makeState(pose);
        non_finite_state.pose.y = std::numeric_limits<double>::quiet_NaN();
        ControllerConfig invalid_config;
        invalid_config.max_linear = std::numeric_limits<double>::infinity();
        bool invalid_config_rejected = false;
        try {
            DiffDrivePurePursuit invalid_controller(invalid_config);
            (void)invalid_controller;
        } catch (const std::invalid_argument&) {
            invalid_config_rejected = true;
        }
        ControllerConfig invalid_reaction_time;
        invalid_reaction_time.reaction_time = 0.0;
        bool invalid_reaction_time_rejected = false;
        try {
            DiffDrivePurePursuit invalid_controller(invalid_reaction_time);
            (void)invalid_controller;
        } catch (const std::invalid_argument&) {
            invalid_reaction_time_rejected = true;
        }
        Path2D non_finite_path = makePath({pose, {5.0, 2.0, 0.0}});
        non_finite_path.poses.back().yaw = std::numeric_limits<double>::quiet_NaN();
        const ControllerResult invalid_state_result = controller.computeCommand(non_finite_state,
                                                                                 path,
                                                                                 makeContext(free_map));
        const ControllerResult invalid_path_result = controller.computeCommand(makeState(pose),
                                                                                 non_finite_path,
                                                                                 makeContext(free_map));
        const ControllerResult deviation = controller.computeCommand(makeState({5.0, 5.0, 0.0}),
                                                                       path,
                                                                       makeContext(free_map));
        ok &= expect(invalid_state_result.error == ErrorCode::InvalidInput
                         && invalid_config_rejected && invalid_reaction_time_rejected
                         && invalid_path_result.error == ErrorCode::InvalidInput
                         && deviation.error == ErrorCode::PathDeviation,
                     "non-finite state, path and config plus excessive path deviation are rejected");

        ControllerContext stale_map = makeContext(free_map);
        stale_map.map.stamp_ns -= 3 * second;
        ControllerContext future_map = makeContext(free_map);
        future_map.map.stamp_ns += 1;
        ControllerContext mismatched_map = makeContext(free_map);
        mismatched_map.map.version += 1U;
        const ControllerResult stale_map_result = controller.computeCommand(makeState(pose), path, stale_map);
        const ControllerResult future_map_result = controller.computeCommand(makeState(pose), path, future_map);
        const ControllerResult mismatched_map_result = controller.computeCommand(makeState(pose), path, mismatched_map);
        ok &= expect(stale_map_result.error == ErrorCode::StaleMap && stale_map_result.command.linear_x == 0.0
                         && future_map_result.error == ErrorCode::StaleMap && future_map_result.command.linear_x == 0.0
                         && mismatched_map_result.error == ErrorCode::StaleMap
                         && mismatched_map_result.command.linear_x == 0.0,
                     "stale or version-mismatched maps fail closed");
    }

    {
        std::vector<std::int8_t> cells(200U * 200U, 0);
        cells[20U * 200U + 22U] = 100;
        const auto obstacle_map = makeMap(200U, 200U, 0.1, std::move(cells));
        ControllerConfig config;
        config.max_acceleration = 10.0;
        config.max_angular_acceleration = 10.0;
        DiffDrivePurePursuit collision_controller(config);
        RobotState state = makeState({2.0, 2.0, 0.0});
        state.velocity.linear_x = 0.6;
        ControllerContext context = makeContext(obstacle_map);
        context.robot_radius = 0.05;
        context.previous_command.linear_x = 0.6;
        const Path2D path = makePath({{2.0, 2.0, 0.0}, {8.0, 2.0, 0.0}});
        const ControllerResult result = collision_controller.computeCommand(state, path, context);
        ok &= expect(result.error == ErrorCode::Collision && result.command.linear_x == 0.0
                         && result.command.angular_z == 0.0,
                     "predicted footprint collision fails closed with no slew command");

        ControllerConfig cautious_config;
        cautious_config.max_acceleration = 0.2;
        DiffDrivePurePursuit cautious_controller(cautious_config);
        ControllerContext small_command_context = makeContext(obstacle_map);
        small_command_context.robot_radius = 0.05;
        RobotState fast_state = makeState({2.0, 2.0, 0.0});
        fast_state.velocity.linear_x = 0.6;
        const ControllerResult measured_velocity_result = cautious_controller.computeCommand(fast_state,
                                                                                              path,
                                                                                              small_command_context);
        ok &= expect(measured_velocity_result.error == ErrorCode::Collision
                         && measured_velocity_result.command.linear_x == 0.0,
                     "measured speed is collision-checked while the new command is small");
    }

    {
        const Pose2D pose{5.0, 5.0, 0.0};
        const Path2D behind = makePath({pose, {3.0, 5.0, 0.0}});
        const ControllerResult forward_only = controller.computeCommand(makeState(pose), behind, makeContext(free_map));
        ControllerConfig reverse_config;
        reverse_config.allow_reverse = true;
        DiffDrivePurePursuit reverse_controller(reverse_config);
        const ControllerResult reverse = reverse_controller.computeCommand(makeState(pose), behind, makeContext(free_map));
        ok &= expect(forward_only.error == ErrorCode::None && forward_only.command.linear_x == 0.0
                         && forward_only.command.angular_z > 0.0
                         && reverse.error == ErrorCode::None && reverse.command.linear_x < 0.0,
                     "reverse is opt-in; forward-only mode rotates to face a path behind");

        ControllerContext reverse_previous_context = makeContext(free_map);
        reverse_previous_context.previous_command.linear_x = -0.1;
        const Path2D ahead = makePath({pose, {7.0, 5.0, 0.0}});
        const ControllerResult reverse_previous = controller.computeCommand(makeState(pose),
                                                                              ahead,
                                                                              reverse_previous_context);
        ok &= expect(reverse_previous.error == ErrorCode::InvalidInput
                         && reverse_previous.command.linear_x == 0.0
                         && reverse_previous.command.angular_z == 0.0,
                     "forward-only mode rejects a negative previous command");
    }

    {
        const Path2D straight = makePath({{2.0, 2.0, 0.0}, {5.0, 2.0, 0.0}});
        const Path2D turn = makePath({{2.0, 2.0, 0.0}, {4.0, 2.0, 0.0}, {4.0, 4.0, std::numbers::pi / 2.0}});
        ok &= expect(simulateToGoal(straight, {2.0, 2.0, 0.0}, free_map),
                     "kinematic simulation reaches a straight-path goal");
        ok &= expect(simulateToGoal(turn, {2.0, 2.0, 0.0}, free_map),
                     "kinematic simulation follows a turn and aligns at the goal");
        ok &= expect(simulateWallRoute(),
                     "controller and SafeBase navigate the exact wall with 0.10 m planning clearance and 0.18 m physical footprint");
        ok &= expect(wallCornerFailsClosedWithoutPlanningClearance(),
                     "the no-clearance wall corner returns Collision with a zero command");
    }

    if (!ok) {
        return 1;
    }
    std::cout << "pure pursuit control, validation, collision prediction and simulation: PASS\n";
    return 0;
}

#include <rbt_navigation/controller/pure_pursuit.hpp>
#include <rbt_navigation/controller/vector_pursuit.hpp>
#include <rbt_navigation/geometry/footprint.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

using namespace rbt::navigation;

namespace {

constexpr std::uint64_t kMapVersion = 7U;
constexpr TimePoint kNow = 10 * second;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

std::shared_ptr<const GridMap> makeMap(std::vector<std::int8_t> cells = {}) {
    OccupancyGrid grid;
    grid.width = 200U;
    grid.height = 200U;
    grid.resolution = 0.1;
    if (cells.empty()) {
        cells.assign(grid.width * grid.height, 0);
    }
    grid.cells = std::move(cells);
    return std::make_shared<const GridMap>(std::move(grid));
}

RobotState makeState(Pose2D pose) {
    return RobotState{pose, {}, kNow, kNow, true};
}

Path2D makePath(std::vector<Pose2D> poses) {
    Path2D path;
    path.poses = std::move(poses);
    path.map_version = kMapVersion;
    return path;
}

ControllerContext makeContext(const std::shared_ptr<const GridMap>& map) {
    ControllerContext context;
    context.now_ns = kNow;
    context.dt = 0.02;
    context.map = MapSnapshot{map, kMapVersion, kNow};
    context.robot_radius = 0.0;
    return context;
}

ControllerConfig vectorConfig(double lookahead, bool allow_reverse = false) {
    ControllerConfig config;
    config.geometry = PursuitGeometry::VectorPursuit;
    config.use_dynamic_window = false;
    config.allow_reverse = allow_reverse;
    config.min_lookahead = lookahead;
    config.max_lookahead = lookahead;
    config.max_angular = 100.0;
    config.rotate_to_heading_speed = 10.0;
    config.rotate_to_heading_angle = std::numbers::pi;
    config.max_acceleration = 1000.0;
    config.max_deceleration = 1000.0;
    config.max_angular_acceleration = 1000.0;
    config.max_angular_deceleration = 1000.0;
    config.min_approach_speed = 0.0;
    return config;
}

double upstreamOracle(double x, double y, double yaw, double k,
                      double minimum_radius, bool allow_reverse) {
    const double target_angle = normalizeAngle(yaw);
    const double distance = std::hypot(x, y);
    if (!allow_reverse && x < 0.0) {
        return std::copysign(1.0 / minimum_radius, y);
    }
    if (std::abs(y) <= 1e-6) return 0.0;

    const double distance_squared = distance * distance;
    const double phi1 = std::atan2(2.0 * y * y - distance_squared, 2.0 * x * y);
    const double term2 = distance_squared / (2.0 * y);
    const double phi2 = std::atan2(term2, 0.0);
    const double two_pi = 2.0 * std::numbers::pi;
    double phi = std::fmod(phi1 - phi2, two_pi);
    if (phi < 0.0) phi += two_pi;
    phi = std::max(phi, 1e-9);

    const double denominator = (k - 1.0) * phi + target_angle;
    if (denominator == 0.0) return 0.0;
    const double term1 = k * phi / denominator;
    const double radius = std::max(std::abs(term1 * term2), minimum_radius);
    return std::copysign(1.0 / radius, y);
}

Pose2D targetInRobotFrame(const Pose2D& robot, const Pose2D& target) {
    const double dx = target.x - robot.x;
    const double dy = target.y - robot.y;
    const double cosine = std::cos(robot.yaw);
    const double sine = std::sin(robot.yaw);
    return {cosine * dx + sine * dy,
            -sine * dx + cosine * dy,
            normalizeAngle(target.yaw - robot.yaw)};
}

Pose2D poseAtDistanceForOracle(const Path2D& path, double distance) {
    if (distance <= 0.0) return path.poses.front();
    double accumulated = 0.0;
    for (std::size_t index = 0U; index + 1U < path.poses.size(); ++index) {
        const Pose2D& start = path.poses[index];
        const Pose2D& end = path.poses[index + 1U];
        const double length = std::hypot(end.x - start.x, end.y - start.y);
        if (length == 0.0) continue;
        if (distance <= accumulated + length) {
            const double fraction = (distance - accumulated) / length;
            const double yaw_delta = normalizeAngle(end.yaw - start.yaw);
            return {start.x + fraction * (end.x - start.x),
                    start.y + fraction * (end.y - start.y),
                    normalizeAngle(start.yaw + fraction * yaw_delta)};
        }
        accumulated += length;
    }
    return path.poses.back();
}

bool matchesOracle(ILocalController& controller,
                   const Pose2D& robot,
                   const Pose2D& target,
                   double lookahead,
                   bool allow_reverse,
                   const std::shared_ptr<const GridMap>& map,
                   std::string_view message) {
    const Path2D path = makePath({robot, target});
    const ControllerResult result = controller.computeCommand(
        makeState(robot), path, makeContext(map));
    const Pose2D local_target = targetInRobotFrame(robot, target);
    const double expected = upstreamOracle(local_target.x, local_target.y,
                                           local_target.yaw, 5.0, 0.10,
                                           allow_reverse);
    return expect(result.error == ErrorCode::None && finite(result.command)
                      && std::abs(result.command.linear_x) > 1e-5
                      && std::abs(std::hypot(local_target.x, local_target.y) - lookahead) < 1e-10
                      && std::abs(result.command.angular_z / result.command.linear_x - expected) < 1e-7,
                  message);
}

bool runRotationCusp(ILocalController& controller,
                     const std::shared_ptr<const GridMap>& map) {
    constexpr double turn_yaw = 0.2;
    const double forward_x = 0.5 * std::cos(turn_yaw);
    const double forward_y = 0.5 * std::sin(turn_yaw);
    const Path2D path = makePath({{10.0, 10.0, 0.0},
                                  {10.0, 10.0, turn_yaw},
                                  {10.0 + forward_x, 10.0 + forward_y, turn_yaw},
                                  {10.0, 10.0, turn_yaw}});

    ControllerContext context = makeContext(map);
    RobotState state = makeState(path.poses.front());
    const ControllerResult rotate = controller.computeCommand(state, path, context);
    if (!expect(rotate.error == ErrorCode::None && rotate.command.angular_z > 0.0
                    && rotate.command.linear_x == 0.0,
                "path rotation produces an in-place turn")) {
        return false;
    }

    state = makeState(path.poses[1]);
    context = makeContext(map);
    const ControllerResult advance_after_rotation = controller.computeCommand(state, path, context);
    if (!expect(advance_after_rotation.error == ErrorCode::None
                    && advance_after_rotation.command.linear_x == 0.0
                    && advance_after_rotation.command.angular_z == 0.0,
                "controller advances past the completed path rotation")) {
        return false;
    }

    const ControllerResult forward = controller.computeCommand(state, path, context);
    if (!expect(forward.error == ErrorCode::None && forward.command.linear_x > 0.0,
                "controller tracks the forward segment before the cusp")) {
        return false;
    }

    state = makeState(path.poses[2]);
    context = makeContext(map);
    const ControllerResult advance_at_cusp = controller.computeCommand(state, path, context);
    if (!expect(advance_at_cusp.error == ErrorCode::None
                    && advance_at_cusp.command.linear_x == 0.0,
                "controller stops at the path cusp before changing direction")) {
        return false;
    }

    const ControllerResult reverse = controller.computeCommand(state, path, context);
    return expect(reverse.error == ErrorCode::None && reverse.command.linear_x < 0.0,
                  "controller tracks the reverse segment after the cusp");
}

} // namespace

int main() {
    bool ok = true;
    const auto map = makeMap();

    {
        const Pose2D robot{10.0, 10.0, 0.0};
        const double lookahead = std::hypot(0.5, 0.3);
        ControllerConfig config = vectorConfig(lookahead);
        RegulatedPurePursuit controller(config);
        ok &= matchesOracle(controller, robot, {10.5, 10.3, 0.25}, lookahead,
                            false, map, "normal target curvature matches the upstream oracle");
    }

    {
        const Pose2D robot{10.0, 10.0, 0.0};
        const double lookahead = std::hypot(0.5, 0.3);
        ControllerConfig config = vectorConfig(lookahead, true);
        VectorPursuit controller(config);
        ok &= matchesOracle(controller, robot, {9.5, 10.3, 0.0}, lookahead,
                            true, map, "reverse target curvature uses physical body yaw");
    }

    {
        const Pose2D robot{10.0, 10.0, 0.0};
        const double lookahead = std::hypot(0.5, 0.3);
        ControllerConfig first_config = vectorConfig(lookahead);
        ControllerConfig second_config = first_config;
        ControllerConfig explicit_on_config = first_config;
        explicit_on_config.vector_pursuit_use_heading_from_path = true;
        RegulatedPurePursuit first(first_config);
        RegulatedPurePursuit second(second_config);
        RegulatedPurePursuit explicit_on(explicit_on_config);
        const Path2D first_path = makePath({robot, {10.5, 10.3, 0.0}});
        const Path2D second_path = makePath({robot, {10.5, 10.3, 0.7}});
        const ControllerResult first_result = first.computeCommand(makeState(robot), first_path,
                                                                     makeContext(map));
        const ControllerResult second_result = second.computeCommand(makeState(robot), second_path,
                                                                       makeContext(map));
        const ControllerResult explicit_on_result = explicit_on.computeCommand(
            makeState(robot), second_path, makeContext(map));
        const double first_curvature = first_result.command.angular_z / first_result.command.linear_x;
        const double second_curvature = second_result.command.angular_z / second_result.command.linear_x;
        ok &= expect(ControllerConfig{}.vector_pursuit_use_heading_from_path
                         && first_result.error == ErrorCode::None
                         && second_result.error == ErrorCode::None
                         && explicit_on_result.error == ErrorCode::None
                         && std::isfinite(first_curvature) && std::isfinite(second_curvature)
                         && std::abs(first_curvature - second_curvature) > 1e-3
                         && second_result.command.linear_x == explicit_on_result.command.linear_x
                         && second_result.command.angular_z == explicit_on_result.command.angular_z,
                     "path orientation defaults on and target yaw changes vector pursuit curvature");
    }

    {
        const Pose2D robot{10.0, 10.0, 0.0};
        const double lookahead = 0.3;
        ControllerConfig config = vectorConfig(lookahead);
        config.vector_pursuit_use_heading_from_path = false;
        RegulatedPurePursuit controller(config);
        const Path2D first_yaw_path = makePath({robot, {10.0, 10.0, 0.3},
                                                {10.5, 10.3, 0.7}});
        const Path2D second_yaw_path = makePath({robot, {10.0, 10.0, -1.1},
                                                 {10.5, 10.3, -0.5}});
        const ControllerResult first_yaw = controller.computeCommand(
            makeState(robot), first_yaw_path, makeContext(map));
        const ControllerResult second_yaw = controller.computeCommand(
            makeState(robot), second_yaw_path, makeContext(map));
        const double tangent_yaw = std::atan2(0.3, 0.5);
        const double fraction = lookahead / std::hypot(0.5, 0.3);
        const double expected = upstreamOracle(fraction * 0.5, fraction * 0.3, tangent_yaw,
                                               config.vector_pursuit_k,
                                               config.vector_pursuit_min_radius, false);
        const double first_curvature = first_yaw.command.angular_z / first_yaw.command.linear_x;
        const double second_curvature = second_yaw.command.angular_z / second_yaw.command.linear_x;
        ok &= expect(first_yaw.error == ErrorCode::None && second_yaw.error == ErrorCode::None
                         && std::isfinite(first_curvature) && std::isfinite(second_curvature)
                         && std::abs(first_curvature - expected) < 1e-7
                         && std::abs(second_curvature - expected) < 1e-7,
                     "disabled path heading uses the selected XY segment tangent");
    }

    {
        const Pose2D robot{10.0, 10.0, 0.0};
        ControllerConfig config = vectorConfig(0.3);
        config.vector_pursuit_use_heading_from_path = false;
        RegulatedPurePursuit controller(config);
        const Path2D path = makePath({robot, {10.2, 10.1, -0.8}, {10.2, 10.1, 1.2}});
        const ControllerResult result = controller.computeCommand(makeState(robot), path,
                                                                    makeContext(map));
        const double observed = result.command.angular_z / result.command.linear_x;
        const double tangent_yaw = std::atan2(0.1, 0.2);
        const double expected = upstreamOracle(0.2, 0.1, tangent_yaw, config.vector_pursuit_k,
                                               config.vector_pursuit_min_radius, false);
        ok &= expect(result.error == ErrorCode::None && finite(result.command)
                         && result.command.linear_x > 0.0
                         && std::abs(observed - expected) < 1e-7,
                     "lookahead beyond the path uses the last nonzero XY segment tangent");
    }

    {
        const Pose2D robot{10.0, 10.0, 0.0};
        const double lookahead = 0.6;
        ControllerConfig config = vectorConfig(lookahead);
        RegulatedPurePursuit controller(config);
        const ControllerResult result = controller.computeCommand(
            makeState(robot), makePath({robot, {10.6, 10.0, 0.0}}), makeContext(map));
        ok &= expect(result.error == ErrorCode::None && finite(result.command)
                         && std::abs(result.command.angular_z) < 1e-10,
                     "zero lateral offset has finite zero curvature");
    }

    {
        const Pose2D robot{10.0, 10.0, 0.0};
        const double lookahead = 0.6;
        ControllerConfig config = vectorConfig(lookahead);
        RegulatedPurePursuit controller(config);
        const ControllerResult result = controller.computeCommand(
            makeState(robot), makePath({robot, {9.4, 10.0, 0.0}}), makeContext(map));
        const double observed = result.command.angular_z / result.command.linear_x;
        const double expected = upstreamOracle(-lookahead, 0.0, 0.0,
                                               config.vector_pursuit_k,
                                               config.vector_pursuit_min_radius,
                                               false);
        ok &= expect(result.error == ErrorCode::None && finite(result.command)
                         && result.command.linear_x > 0.0 && result.command.angular_z > 0.0
                         && std::abs(observed - expected) < 1e-7,
                     "behind target with reverse disabled uses the minimum radius at zero lateral offset");
    }

    {
        const Pose2D robot{10.0, 10.0, 0.0};
        const double lookahead = std::hypot(0.5, 0.3);
        ControllerConfig config = vectorConfig(lookahead);
        config.vector_pursuit_k = 1.0;
        RegulatedPurePursuit controller(config);
        const Path2D path = makePath({robot, {10.5, 10.3, 1e-12}});
        const ControllerResult result = controller.computeCommand(makeState(robot), path,
                                                                    makeContext(map));
        const double observed = result.command.linear_x == 0.0
            ? std::numeric_limits<double>::infinity()
            : result.command.angular_z / result.command.linear_x;
        ok &= expect(result.error == ErrorCode::None && finite(result.command)
                         && std::isfinite(observed) && std::abs(observed) < 1e-9,
                     "near-singular radius stays finite and produces near-zero curvature");
    }

    {
        const Pose2D robot{10.0, 10.0, 0.0};
        const double lookahead = std::hypot(0.5, 0.5);
        ControllerConfig config = vectorConfig(lookahead);
        config.vector_pursuit_min_radius = 1.0;
        RegulatedPurePursuit controller(config);
        const ControllerResult result = controller.computeCommand(
            makeState(robot), makePath({robot, {10.5, 10.5, 0.0}}), makeContext(map));
        const double observed = result.command.angular_z / result.command.linear_x;
        ok &= expect(result.error == ErrorCode::None && std::isfinite(observed)
                         && std::abs(observed - 1.0) < 1e-7,
                     "minimum radius caps vector pursuit curvature");
    }

    {
        const double start_yaw = 179.0 * std::numbers::pi / 180.0;
        const Pose2D robot{10.0, 10.0, start_yaw};
        const Pose2D end{10.6, 10.2, -start_yaw};
        const double segment_length = std::hypot(end.x - robot.x, end.y - robot.y);
        const double lookahead = 0.5 * segment_length;
        ControllerConfig config = vectorConfig(lookahead, true);
        RegulatedPurePursuit controller(config);
        const Pose2D interpolated = poseAtDistanceForOracle(makePath({robot, end}), lookahead);
        const Pose2D local = targetInRobotFrame(robot, interpolated);
        const ControllerResult result = controller.computeCommand(
            makeState(robot), makePath({robot, end}), makeContext(map));
        const double expected = upstreamOracle(local.x, local.y, local.yaw, 5.0, 0.10, true);
        const double observed = result.command.angular_z / result.command.linear_x;
        ok &= expect(std::abs(local.yaw - std::numbers::pi / 180.0) < 1e-10
                         && result.error == ErrorCode::None && finite(result.command)
                         && std::abs(observed - expected) < 1e-7,
                     "pose yaw interpolation follows the shortest arc through wraparound");
    }

    {
        const Pose2D robot{10.0, 10.0, 0.1};
        const Path2D path = makePath({robot,
                                      {10.4, 10.2, 0.4},
                                      {10.8, 10.5, 0.9}});
        const double ordinary_lookahead = 0.3;
        const double fixed_lookahead = 0.7;
        ControllerConfig config = vectorConfig(ordinary_lookahead);
        config.use_fixed_curvature_lookahead = true;
        config.curvature_lookahead = fixed_lookahead;
        RegulatedPurePursuit controller(config);
        const Pose2D target = poseAtDistanceForOracle(path, fixed_lookahead);
        const Pose2D local = targetInRobotFrame(robot, target);
        const double expected = upstreamOracle(local.x, local.y, local.yaw, 5.0, 0.10, false);
        const ControllerResult result = controller.computeCommand(makeState(robot), path,
                                                                    makeContext(map));
        const double observed = result.command.angular_z / result.command.linear_x;
        ok &= expect(result.error == ErrorCode::None && finite(result.command)
                         && std::abs(observed - expected) < 1e-7,
                     "fixed curvature lookahead uses its interpolated target pose");
    }

    {
        ControllerConfig config = vectorConfig(0.3);
        config.vector_pursuit_k = 0.0;
        bool rejected = false;
        try {
            RegulatedPurePursuit invalid(config);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        ok &= expect(rejected, "nonpositive vector pursuit gain is rejected");

        config = vectorConfig(0.3);
        config.vector_pursuit_min_radius = std::numeric_limits<double>::quiet_NaN();
        rejected = false;
        try {
            VectorPursuit invalid(config);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        ok &= expect(rejected, "nonfinite vector pursuit minimum radius is rejected");
    }

    {
        ControllerConfig config = vectorConfig(0.4);
        RegulatedPurePursuit controller(config);
        const Pose2D robot{10.0, 10.0, 0.0};
        const ControllerResult invalid_target = controller.computeCommand(
            makeState(robot), makePath({robot, {10.4, 10.0,
                                               std::numeric_limits<double>::infinity()}}),
            makeContext(map));
        ok &= expect(invalid_target.error == ErrorCode::InvalidInput
                         && invalid_target.command.linear_x == 0.0
                         && invalid_target.command.angular_z == 0.0,
                     "nonfinite target pose is rejected with a zero command");
    }

    {
        const auto check_rotation_cusp = [&](bool use_heading_from_path) {
            ControllerConfig direct_config = vectorConfig(0.3, true);
            direct_config.use_feasible_path_handler = true;
            direct_config.minimum_path_rotation = 0.1;
            direct_config.vector_pursuit_use_heading_from_path = use_heading_from_path;
            RegulatedPurePursuit direct(direct_config);
            ok &= runRotationCusp(direct, map);

            ControllerConfig wrapper_config = direct_config;
            wrapper_config.geometry = PursuitGeometry::PurePursuit;
            VectorPursuit wrapper(wrapper_config);
            ok &= runRotationCusp(wrapper, map);
        };
        check_rotation_cusp(true);
        check_rotation_cusp(false);
    }

    {
        ControllerConfig config = vectorConfig(0.3);
        config.use_feasible_path_handler = true;
        config.vector_pursuit_use_heading_from_path = false;
        RegulatedPurePursuit controller(config);
        const Pose2D robot{10.5, 10.0, 0.0};
        const Path2D path = makePath({{10.0, 10.0, 0.0}, {10.5, 10.0, 1.0}});
        const ControllerResult result = controller.computeCommand(makeState(robot), path,
                                                                    makeContext(map));
        ok &= expect(result.error == ErrorCode::None && result.command.linear_x == 0.0
                         && result.command.angular_z > 0.0,
                     "final goal yaw still commands rotation when path heading is disabled");
    }

    {
        auto cells = std::vector<std::int8_t>(200U * 200U, 0);
        cells[52U * 200U + 54U] = 100;
        const auto obstacle_map = makeMap(std::move(cells));
        ControllerConfig config = vectorConfig(0.3);
        VectorPursuit controller(config);
        const Pose2D start{5.0, 5.0, 0.0};
        const Path2D rotation = makePath({start, {5.0, 5.0, 0.6}});
        ControllerContext context = makeContext(obstacle_map);
        context.footprint = std::make_shared<const FootprintCollisionChecker>(
            std::vector<FootprintPoint>{{-0.5, -0.1}, {0.5, -0.1},
                                        {0.5, 0.1}, {-0.5, 0.1}});
        const ControllerResult result = controller.computeCommand(makeState(start), rotation,
                                                                    context);
        ok &= expect(result.error == ErrorCode::Collision
                         && result.command.linear_x == 0.0
                         && result.command.angular_z == 0.0,
                     "polygon collision checks reject an unsafe pure rotation");
    }

    if (!ok) return 1;
    std::cout << "PASS vector pursuit geometry and shared controller checks\n";
    return 0;
}

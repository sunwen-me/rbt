#include <rbt_navigation/controller/pure_pursuit.hpp>
#include <rbt_navigation/planner/a_star.hpp>
#include <rbt_navigation/runtime/navigation_manager.hpp>
#include <rbt_navigation/safety/safe_base.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

using namespace rbt::navigation;

namespace {

constexpr double kDt = 0.02;
constexpr TimePoint kStepNs = 20'000'000;
constexpr TimePoint kCommandLifetimeNs = 200'000'000;
constexpr double kRobotRadius = 0.18;
constexpr double kGlobalPlanningClearance = 0.10;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

bool nearZero(const Twist2D& velocity, double tolerance = 1e-9) {
    return std::abs(velocity.linear_x) <= tolerance
        && std::abs(velocity.angular_z) <= tolerance;
}

double pathLength(const Path2D& path) {
    double length = 0.0;
    for (std::size_t index = 0; index + 1U < path.poses.size(); ++index) {
        length += distance(path.poses[index], path.poses[index + 1U]);
    }
    return length;
}

std::shared_ptr<GridMap> makeMap(bool with_wall = false) {
    OccupancyGrid grid;
    grid.width = 80;
    grid.height = 60;
    grid.resolution = 0.1;
    grid.origin = {0.0, 0.0, 0.0};
    grid.cells.assign(grid.width * grid.height, 0);

    if (with_wall) {
        // A vertical barrier forces the route above the direct start-to-goal line.
        // The 1.7 m opening is wide enough for the configured circular footprint.
        for (std::size_t y = 5; y < 45; ++y) {
            if (y >= 22 && y < 39) {
                continue;
            }
            grid.cells[y * grid.width + 30] = 100;
        }
    }
    return std::make_shared<GridMap>(std::move(grid));
}

RuntimeConfig runtimeConfig(TimePoint planning_timeout = 3 * second) {
    RuntimeConfig config;
    config.robot_radius = kRobotRadius;
    config.max_input_age_ns = 250'000'000;
    config.max_map_age_ns = 2 * second;
    config.planning_timeout_ns = planning_timeout;
    config.max_replans = 5;
    return config;
}

std::unique_ptr<IGlobalPlanner> makeSimulationPlanner(const RuntimeConfig& config) {
    return std::make_unique<AStarPlanner>(
        PlannerConfig{config.robot_radius + kGlobalPlanningClearance});
}

class Simulation {
public:
    explicit Simulation(std::shared_ptr<GridMap> grid, Pose2D initial = {1.0, 1.0, 1.0},
                        RuntimeConfig config = runtimeConfig())
        : grid_(std::move(grid)), map_{grid_, 1, kStepNs}, base_(initial),
          manager_(makeSimulationPlanner(config),
                   std::make_unique<DiffDrivePurePursuit>(), config),
          now_(kStepNs) {
        require(grid_ != nullptr, "simulation requires a map");
        state_ = {initial, {}, now_, now_, true};
        manager_.setMap(map_);
    }

    bool setGoal(std::uint64_t id, Pose2D pose, TimePoint timeout = 120 * second) {
        return manager_.setGoal({id, pose, timeout}, state_, now_);
    }

    Twist2D step() {
        map_.stamp_ns = now_;
        manager_.setMap(map_);
        last_navigation_command_ = manager_.controlStep(state_, now_, kDt);

        auto status = manager_.status();
        if (status.state == NavigationState::Planning || status.state == NavigationState::Replanning) {
            // The virtual clock advances only at 50 Hz; this short yield lets the
            // real planning thread run without allowing virtual timeout to race it.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            status = manager_.status();
        }
        (void)status;
        observePath();

        const VelocityCommand command{1, ++sequence_, now_, now_ + kCommandLifetimeNs,
                                      last_navigation_command_};
        require(managerBase_.accept(command, now_) == CommandRejection::None,
                "SafeBase rejected a fresh navigation command");
        const Twist2D safe_velocity = managerBase_.step(now_, kDt);
        const Pose2D previous_pose = base_.pose();
        base_.apply(safe_velocity, kDt);
        const Pose2D pose = base_.pose();
        require(grid_->isFree(pose.x, pose.y, kRobotRadius),
                describePose("robot footprint entered an occupied or unsafe cell", pose));
        require(grid_->segmentFree(previous_pose, pose, kRobotRadius),
                "robot footprint crossed an occupied or unsafe cell between 50 Hz states");

        minimum_clearance_ = std::min(minimum_clearance_, grid_->getDistanceToObstacle(pose.x, pose.y));
        now_ += kStepNs;
        state_ = {pose, base_.velocity(), now_, now_, localized_};
        return last_navigation_command_;
    }

    void setLocalized(bool localized) {
        localized_ = localized;
        state_.localized = localized;
    }
    [[nodiscard]] TimePoint now() const { return now_; }
    [[nodiscard]] const RobotState& state() const { return state_; }
    [[nodiscard]] const Pose2D& pose() const { return state_.pose; }
    [[nodiscard]] const Twist2D& velocity() const { return state_.velocity; }
    [[nodiscard]] NavigationStatus status() const { return manager_.status(); }
    [[nodiscard]] double longestObservedPath() const { return longest_observed_path_; }
    [[nodiscard]] std::size_t pathSamples() const { return path_samples_; }
    [[nodiscard]] double minimumClearance() const { return minimum_clearance_; }
    [[nodiscard]] const Twist2D& lastNavigationCommand() const { return last_navigation_command_; }
    [[nodiscard]] NavigationManager& manager() { return manager_; }
    [[nodiscard]] const MapSnapshot& map() const { return map_; }

private:
    static std::string describePose(const std::string& message, const Pose2D& pose) {
        std::ostringstream output;
        output << message << " at (" << pose.x << ", " << pose.y << ")";
        return output.str();
    }

    void observePath() {
        const Path2D current = manager_.path();
        if (current.poses.empty()) {
            return;
        }
        ++path_samples_;
        longest_observed_path_ = std::max(longest_observed_path_, pathLength(current));
        for (std::size_t index = 0; index + 1U < current.poses.size(); ++index) {
            require(grid_->segmentFree(current.poses[index], current.poses[index + 1U], kRobotRadius),
                    "planner returned a path segment that violates the robot footprint clearance");
        }
    }

    std::shared_ptr<GridMap> grid_;
    MapSnapshot map_;
    SimulatedBaseAdapter base_;
    SafeBase managerBase_;
    NavigationManager manager_;
    TimePoint now_;
    RobotState state_;
    std::uint64_t sequence_{};
    bool localized_{true};
    Twist2D last_navigation_command_{};
    double longest_observed_path_{};
    double minimum_clearance_{std::numeric_limits<double>::infinity()};
    std::size_t path_samples_{};
};

void runToTerminal(Simulation& simulation, std::size_t max_steps, const std::string& scenario) {
    for (std::size_t step = 0; step < max_steps; ++step) {
        simulation.step();
        const auto status = simulation.status();
        if (status.state == NavigationState::Succeeded || status.state == NavigationState::Failed
            || status.state == NavigationState::Canceled) {
            return;
        }
    }
    throw std::runtime_error(scenario + " did not reach a terminal navigation state within its step limit");
}

void waitForMotion(Simulation& simulation, const std::string& scenario) {
    for (std::size_t step = 0; step < 2'000; ++step) {
        simulation.step();
        const auto status = simulation.status();
        require(status.state != NavigationState::Failed,
                scenario + " failed before the simulated base began moving");
        if (std::abs(simulation.velocity().linear_x) > 0.1) {
            return;
        }
    }
    throw std::runtime_error(scenario + " did not reach a moving state");
}

struct SlowPlannerState {
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};
};

class CancellableSlowPlanner final : public IGlobalPlanner {
public:
    explicit CancellableSlowPlanner(std::shared_ptr<SlowPlannerState> state) : state_(std::move(state)) {}

    PlannerResult plan(const Pose2D&, const Pose2D&, const MapSnapshot&, std::stop_token stop) override {
        state_->started.store(true, std::memory_order_release);
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        state_->stopped.store(true, std::memory_order_release);
        return {ErrorCode::Canceled, {}, 0};
    }

private:
    std::shared_ptr<SlowPlannerState> state_;
};

void testOpenMapNavigation() {
    Simulation simulation(makeMap());
    require(simulation.setGoal(1, {5.0, 3.0, 1.0}), "open-map goal was rejected");
    runToTerminal(simulation, 6'000, "open-map navigation");
    const auto status = simulation.status();
    require(status.state == NavigationState::Succeeded,
            "open-map navigation did not succeed (error=" + std::to_string(static_cast<unsigned>(status.error)) + ")");
    require(distance(simulation.pose(), {5.0, 3.0, 1.0}) < 0.12,
            "open-map robot stopped too far from the requested goal");

    std::cout << "PASS open map: pose=(" << simulation.pose().x << ", " << simulation.pose().y
              << "), steps=" << (simulation.now() / kStepNs - 1) << '\n';
}

void testObstacleDetour() {
    Simulation simulation(makeMap(true));
    require(simulation.setGoal(2, {5.0, 3.0, 1.0}), "obstacle-map goal was rejected");
    runToTerminal(simulation, 6'000, "obstacle detour");
    const auto status = simulation.status();
    require(status.state == NavigationState::Succeeded,
            "obstacle detour did not succeed (error=" + std::to_string(static_cast<unsigned>(status.error)) + ")");
    require(simulation.pathSamples() > 0, "obstacle detour never exposed a planned path");
    require(simulation.longestObservedPath() > std::hypot(4.0, 2.0) + 0.3,
            "obstacle route did not detour around the wall opening");
    require(simulation.minimumClearance() >= kRobotRadius,
            "obstacle route violated footprint clearance during simulation");
    require(distance(simulation.pose(), {5.0, 3.0, 1.0}) < 0.12,
            "obstacle-map robot stopped too far from the requested goal");

    std::cout << "PASS obstacle detour: path=" << simulation.longestObservedPath()
              << " m, planning radius=" << (kRobotRadius + kGlobalPlanningClearance)
              << " m, minimum physical clearance=" << simulation.minimumClearance() << " m\n";
}

void testInvalidPlannerEndpoints() {
    AStarPlanner planner;
    const auto open_map = makeMap();
    const MapSnapshot snapshot{open_map, 1, kStepNs};
    const auto outside_start = planner.plan({-0.1, 1.0, 0.0}, {5.0, 3.0, 0.0}, snapshot);
    const auto outside_goal = planner.plan({1.0, 1.0, 0.0}, {8.1, 3.0, 0.0}, snapshot);
    require(outside_start.error == ErrorCode::StartOutside,
            "planner did not reject a start outside map bounds");
    require(outside_goal.error == ErrorCode::GoalOutside,
            "planner did not reject a goal outside map bounds");

    const auto wall_map = makeMap(true);
    const MapSnapshot wall_snapshot{wall_map, 2, kStepNs};
    const auto occupied_start = planner.plan({3.05, 1.0, 0.0}, {5.0, 3.0, 0.0}, wall_snapshot);
    const auto occupied_goal = planner.plan({1.0, 1.0, 0.0}, {3.05, 1.0, 0.0}, wall_snapshot);
    require(occupied_start.error == ErrorCode::StartOccupied,
            "planner did not reject a start inside the wall");
    require(occupied_goal.error == ErrorCode::GoalOccupied,
            "planner did not reject a goal inside the wall");
    std::cout << "PASS invalid planner endpoints: outside and occupied starts/goals rejected\n";
}

void testCancellationStopsBase() {
    Simulation simulation(makeMap());
    require(simulation.setGoal(3, {5.0, 3.0, 1.0}), "cancel scenario goal was rejected");
    waitForMotion(simulation, "cancel scenario");
    const double speed_before_cancel = std::abs(simulation.velocity().linear_x);
    require(simulation.manager().cancel(3), "active goal cancellation was rejected");
    simulation.step();
    require(simulation.status().state == NavigationState::Canceled,
            "cancel did not put NavigationManager in the canceled state");
    require(nearZero(simulation.lastNavigationCommand()),
            "canceled NavigationManager emitted a nonzero command");
    require(std::abs(simulation.velocity().linear_x) < speed_before_cancel,
            "SafeBase did not begin controlled deceleration after cancellation");

    for (std::size_t step = 0; step < 100 && !nearZero(simulation.velocity()); ++step) {
        simulation.step();
        require(nearZero(simulation.lastNavigationCommand()),
                "canceled NavigationManager emitted a later nonzero command");
    }
    require(nearZero(simulation.velocity()), "SafeBase did not stop after cancellation");
    std::cout << "PASS cancellation stop: speed before cancel=" << speed_before_cancel << " m/s\n";
}

void testLocalizationInterruptionStopsBase() {
    Simulation simulation(makeMap());
    require(simulation.setGoal(4, {5.0, 3.0, 1.0}), "localization scenario goal was rejected");
    waitForMotion(simulation, "localization scenario");
    simulation.setLocalized(false);
    const Twist2D first_command_after_loss = simulation.step();
    require(nearZero(first_command_after_loss),
            "localization loss produced a nonzero navigation command");
    require(simulation.status().state == NavigationState::Failed
                && simulation.status().error == ErrorCode::LocalizationUnavailable,
            "localization loss did not fail navigation with LocalizationUnavailable");

    for (std::size_t step = 0; step < 100 && !nearZero(simulation.velocity()); ++step) {
        simulation.step();
        require(nearZero(simulation.lastNavigationCommand()),
                "navigation emitted a nonzero command after localization loss");
    }
    require(nearZero(simulation.velocity()), "SafeBase did not stop after localization loss");
    std::cout << "PASS localization interruption: no command and base stopped\n";
}

void testCancellablePlannerTimeoutIsNonblocking() {
    auto grid = makeMap();
    auto slow_state = std::make_shared<SlowPlannerState>();
    NavigationManager manager(std::make_unique<CancellableSlowPlanner>(slow_state),
                              std::make_unique<DiffDrivePurePursuit>(), runtimeConfig(100'000'000));
    MapSnapshot map{grid, 1, kStepNs};
    RobotState state{{1.0, 1.0, 1.0}, {}, kStepNs, kStepNs, true};
    manager.setMap(map);
    require(manager.setGoal({5, {5.0, 3.0, 1.0}, 5 * second}, state, kStepNs),
            "slow-planner goal was rejected");

    const auto tick_start = std::chrono::steady_clock::now();
    (void)manager.controlStep(state, kStepNs, kDt);
    const auto tick_elapsed = std::chrono::steady_clock::now() - tick_start;
    require(tick_elapsed < std::chrono::milliseconds(50),
            "controlStep blocked while an injected planner was running");

    for (std::size_t retry = 0; retry < 250 && !slow_state->started.load(std::memory_order_acquire); ++retry) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(slow_state->started.load(std::memory_order_acquire),
            "background worker never entered the injected slow planner");

    const TimePoint timeout_now = kStepNs + 100'000'000;
    state.tf_stamp_ns = timeout_now;
    state.odom_stamp_ns = timeout_now;
    map.stamp_ns = timeout_now;
    manager.setMap(map);
    const auto timeout_tick_start = std::chrono::steady_clock::now();
    (void)manager.controlStep(state, timeout_now, kDt);
    const auto timeout_tick_elapsed = std::chrono::steady_clock::now() - timeout_tick_start;
    const auto status = manager.status();
    require(timeout_tick_elapsed < std::chrono::milliseconds(50),
            "planning-timeout tick blocked waiting for planner cancellation");
    require(status.state == NavigationState::Failed && status.error == ErrorCode::PlanningTimeout,
            "manager did not apply its virtual planning timeout independently of planner completion");

    for (std::size_t retry = 0; retry < 250 && !slow_state->stopped.load(std::memory_order_acquire); ++retry) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    require(slow_state->stopped.load(std::memory_order_acquire),
            "planning timeout did not cancel the injected planner");
    std::cout << "PASS cancellable planner timeout: control ticks=" << tick_elapsed.count() / 1'000'000
              << ", timeout tick=" << timeout_tick_elapsed.count() / 1'000'000 << " ms\n";
}

void testIndependentBaseWatchdog() {
    SafeBaseConfig config;
    config.watchdog_ns = 250'000'000;
    SafeBase safe_base(config);
    SimulatedBaseAdapter base({1.0, 1.0, 0.0});
    constexpr TimePoint first_sent_at = kStepNs;
    const VelocityCommand first_command{1, 1, first_sent_at,
                                        first_sent_at + config.watchdog_ns, {0.5, 0.0}};
    require(safe_base.accept(first_command, first_sent_at) == CommandRejection::None,
            "SafeBase rejected the last command before simulated navigation-process loss");
    base.apply(safe_base.step(first_sent_at, kDt), kDt);

    // The safety process has no elapsed interval on its first clock sample.
    // Publish the next 50 Hz command before simulating the navigation outage.
    constexpr TimePoint last_sent_at = first_sent_at + kStepNs;
    const VelocityCommand last_command{1, 2, last_sent_at,
                                       last_sent_at + config.watchdog_ns, {0.5, 0.0}};
    require(safe_base.accept(last_command, last_sent_at) == CommandRejection::None,
            "SafeBase rejected the final fresh command before simulated navigation-process loss");
    base.apply(safe_base.step(last_sent_at, kDt), kDt);
    require(base.velocity().linear_x > 0.0, "watchdog scenario did not establish motion");

    const TimePoint silence_deadline = last_sent_at + config.watchdog_ns;
    require(safe_base.watchdogExpired(silence_deadline),
            "base watchdog did not expire when navigation stopped publishing");
    const Twist2D stopped = safe_base.step(silence_deadline, kDt);
    base.apply(stopped, kDt);
    require(nearZero(stopped) && nearZero(base.velocity()),
            "independent base watchdog did not command immediate zero");
    std::cout << "PASS independent base watchdog: zero at " << config.watchdog_ns / 1'000'000
              << " ms without navigation publication\n";
}

} // namespace

int main() {
    try {
        testInvalidPlannerEndpoints();
        testOpenMapNavigation();
        testObstacleDetour();
        testCancellationStopsBase();
        testLocalizationInterruptionStopsBase();
        testCancellablePlannerTimeoutIsNonblocking();
        testIndependentBaseWatchdog();
    } catch (const std::exception& error) {
        std::cerr << "FAIL simulation: " << error.what() << '\n';
        return 1;
    }
    return 0;
}

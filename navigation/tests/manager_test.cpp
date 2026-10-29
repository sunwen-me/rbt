#include <rbt_navigation/runtime/navigation_manager.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

using namespace rbt::navigation;
using namespace std::chrono_literals;

namespace {

void check(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <class Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = 1s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return predicate();
}

MapSnapshot makeMap(std::uint64_t version, TimePoint stamp) {
    OccupancyGrid grid;
    grid.width = 20;
    grid.height = 20;
    grid.resolution = 0.25;
    grid.cells.assign(grid.width * grid.height, 0);
    return {std::make_shared<GridMap>(std::move(grid)), version, stamp};
}

RobotState makeState(TimePoint stamp) {
    RobotState state;
    state.pose = {1.0, 1.0, 0.0};
    state.localized = true;
    state.tf_stamp_ns = stamp;
    state.odom_stamp_ns = stamp;
    return state;
}

NavigationGoal makeGoal(std::uint64_t id, TimePoint timeout = 10 * second) {
    return {id, {2.0, 2.0, 0.0}, timeout};
}

PlannerResult successfulPlan(const Pose2D& start, const Pose2D& goal) {
    PlannerResult result;
    result.path.poses = {start, goal};
    return result;
}

class FunctionPlanner final : public IGlobalPlanner {
public:
    using Function = std::function<PlannerResult(const Pose2D&, const Pose2D&,
                                                 const MapSnapshot&, std::stop_token)>;
    explicit FunctionPlanner(Function function) : function_(std::move(function)) {}

    PlannerResult plan(const Pose2D& start, const Pose2D& goal, const MapSnapshot& map,
                       std::stop_token stop) override {
        calls.fetch_add(1, std::memory_order_relaxed);
        return function_(start, goal, map, stop);
    }

    std::atomic<int> calls{0};

private:
    Function function_;
};

class FunctionController final : public ILocalController {
public:
    using Function = std::function<ControllerResult(const RobotState&, const Path2D&,
                                                    const ControllerContext&)>;
    explicit FunctionController(Function function) : function_(std::move(function)) {}

    ControllerResult computeCommand(const RobotState& state, const Path2D& path,
                                    const ControllerContext& context) override {
        return function_(state, path, context);
    }

private:
    Function function_;
};

class Gate {
public:
    void enterAndWait() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        cv_.notify_all();
        cv_.wait(lock, [this] { return released_; });
    }

    bool waitUntilEntered() {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, 1s, [this] { return entered_; });
    }

    void release() {
        std::lock_guard lock(mutex_);
        released_ = true;
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool entered_{};
    bool released_{};
};

void testAsyncSuccess() {
    const TimePoint now = 10 * second;
    const auto caller = std::this_thread::get_id();
    std::atomic<bool> planned_off_caller{false};
    auto planner = std::make_unique<FunctionPlanner>(
        [&](const Pose2D& start, const Pose2D& goal, const MapSnapshot&, std::stop_token) {
            planned_off_caller.store(std::this_thread::get_id() != caller, std::memory_order_relaxed);
            return successfulPlan(start, goal);
        });
    auto* planner_view = planner.get();
    auto controller = std::make_unique<FunctionController>(
        [](const RobotState&, const Path2D&, const ControllerContext&) {
            return ControllerResult{{}, ErrorCode::None, true};
        });
    NavigationManager manager(std::move(planner), std::move(controller));
    manager.setMap(makeMap(1, now));
    const auto state = makeState(now);
    check(manager.setGoal(makeGoal(1), state, now), "valid goal should be accepted");
    check(waitFor([&] { return manager.status().state == NavigationState::Controlling; }),
          "async plan should become available");
    const auto path = manager.path();
    check(path.poses.size() == 2 && path.goal_id == 1 && path.map_version == 1,
          "accepted path should carry the active goal and map versions");
    check(planned_off_caller.load(std::memory_order_relaxed) && planner_view->calls == 1,
          "planner should run once on the isolated worker");
    check(manager.controlStep(state, now + 10'000'000, 0.02).linear_x == 0.0,
          "reaching the goal should emit a zero command");
    check(manager.status().state == NavigationState::Succeeded,
          "controller completion should mark the goal succeeded");
}

void testPlanningTimeoutDoesNotBlockControl() {
    const TimePoint now = 10 * second;
    std::atomic<bool> started{false};
    std::atomic<bool> observed_stop{false};
    auto planner = std::make_unique<FunctionPlanner>(
        [&](const Pose2D&, const Pose2D&, const MapSnapshot&, std::stop_token stop) {
            started.store(true, std::memory_order_relaxed);
            while (!stop.stop_requested()) {
                std::this_thread::sleep_for(1ms);
            }
            observed_stop.store(true, std::memory_order_relaxed);
            return PlannerResult{ErrorCode::Canceled, {}, 0};
        });
    auto controller = std::make_unique<FunctionController>(
        [](const RobotState&, const Path2D&, const ControllerContext&) { return ControllerResult{}; });
    RuntimeConfig config;
    config.planning_timeout_ns = 50'000'000;
    NavigationManager manager(std::move(planner), std::move(controller), config);
    manager.setMap(makeMap(1, now));
    const auto state = makeState(now);
    check(manager.setGoal(makeGoal(2), state, now), "slow-planner goal should be accepted");
    check(waitFor([&] { return started.load(std::memory_order_relaxed); }),
          "slow planner should start before the timeout tick");

    const auto before = std::chrono::steady_clock::now();
    const auto command = manager.controlStep(state, now + config.planning_timeout_ns, 0.02);
    const auto elapsed = std::chrono::steady_clock::now() - before;
    check(command.linear_x == 0.0 && elapsed < 100ms,
          "controlStep should return zero promptly when planning times out");
    check(manager.status().state == NavigationState::Failed
              && manager.status().error == ErrorCode::PlanningTimeout,
          "manager should enforce its own planning timeout");
    check(waitFor([&] { return observed_stop.load(std::memory_order_relaxed); }),
          "timed-out planner should receive cancellation");
}

void testPlannerWallClockTimeoutWithoutControlTicks() {
    const TimePoint now = 10 * second;
    std::atomic<bool> finished{false};
    auto planner = std::make_unique<FunctionPlanner>(
        [&](const Pose2D& start, const Pose2D& goal, const MapSnapshot&, std::stop_token) {
            std::this_thread::sleep_for(60ms); // Deliberately ignore cancellation.
            finished.store(true, std::memory_order_relaxed);
            return successfulPlan(start, goal);
        });
    auto controller = std::make_unique<FunctionController>(
        [](const RobotState&, const Path2D&, const ControllerContext&) { return ControllerResult{}; });
    RuntimeConfig config;
    config.planning_timeout_ns = 20'000'000;
    NavigationManager manager(std::move(planner), std::move(controller), config);
    manager.setMap(makeMap(1, now));
    check(manager.setGoal(makeGoal(13), makeState(now), now), "wall-timeout goal should be accepted");
    check(waitFor([&] { return finished.load(std::memory_order_relaxed); }),
          "slow planner should return without a control tick");
    check(waitFor([&] { return manager.status().state == NavigationState::Failed; }),
          "planner result should be checked after the bounded delay");
    check(manager.status().error == ErrorCode::PlanningTimeout && manager.path().poses.empty(),
          "wall-clock planning timeout should reject a late successful plan");
}

void testPlannerException() {
    const TimePoint now = 10 * second;
    auto planner = std::make_unique<FunctionPlanner>(
        [](const Pose2D&, const Pose2D&, const MapSnapshot&, std::stop_token) -> PlannerResult {
            throw std::runtime_error("planner error");
        });
    auto controller = std::make_unique<FunctionController>(
        [](const RobotState&, const Path2D&, const ControllerContext&) { return ControllerResult{}; });
    NavigationManager manager(std::move(planner), std::move(controller));
    manager.setMap(makeMap(1, now));
    check(manager.setGoal(makeGoal(3), makeState(now), now), "exception-test goal should be accepted");
    check(waitFor([&] { return manager.status().state == NavigationState::Failed; }),
          "planner exception should fail the active goal");
    check(manager.status().error == ErrorCode::PlannerFailure && manager.path().poses.empty(),
          "planner exception should clear the path and report PlannerFailure");
}

void testCancellationDiscardsLateResult() {
    const TimePoint now = 10 * second;
    Gate gate;
    std::atomic<bool> finished{false};
    auto planner = std::make_unique<FunctionPlanner>(
        [&](const Pose2D& start, const Pose2D& goal, const MapSnapshot&, std::stop_token) {
            gate.enterAndWait();
            finished.store(true, std::memory_order_relaxed);
            return successfulPlan(start, goal);
        });
    auto controller = std::make_unique<FunctionController>(
        [](const RobotState&, const Path2D&, const ControllerContext&) { return ControllerResult{}; });
    NavigationManager manager(std::move(planner), std::move(controller));
    manager.setMap(makeMap(1, now));
    check(manager.setGoal(makeGoal(4), makeState(now), now), "cancel-test goal should be accepted");
    check(gate.waitUntilEntered(), "planner should enter the blocking test section");
    check(manager.cancel(4), "matching goal should cancel");
    check(manager.status().state == NavigationState::Canceled && manager.path().poses.empty(),
          "cancel should clear path and mark the goal canceled");
    gate.release();
    check(waitFor([&] { return finished.load(std::memory_order_relaxed); }),
          "blocked planner should be released");
    std::this_thread::sleep_for(5ms);
    check(manager.status().state == NavigationState::Canceled && manager.path().poses.empty(),
          "late planner result must not revive a canceled goal");
}

void testDuplicateConflictAndPreemption() {
    const TimePoint now = 10 * second;
    Gate first_gate;
    std::atomic<int> second_goal_calls{0};
    auto planner = std::make_unique<FunctionPlanner>(
        [&](const Pose2D& start, const Pose2D& goal, const MapSnapshot&, std::stop_token) {
            if (goal.x == 2.0) {
                first_gate.enterAndWait();
            } else {
                second_goal_calls.fetch_add(1, std::memory_order_relaxed);
            }
            return successfulPlan(start, goal);
        });
    auto controller = std::make_unique<FunctionController>(
        [](const RobotState&, const Path2D&, const ControllerContext&) { return ControllerResult{}; });
    NavigationManager manager(std::move(planner), std::move(controller));
    manager.setMap(makeMap(1, now));
    const auto state = makeState(now);
    const auto first_goal = makeGoal(5);
    check(manager.setGoal(first_goal, state, now), "first goal should be accepted");
    check(first_gate.waitUntilEntered(), "first planner call should start");
    check(manager.setGoal(first_goal, state, now), "identical goal retry should be idempotent");
    auto conflict = first_goal;
    conflict.pose.x += 0.1;
    check(!manager.setGoal(conflict, state, now), "same id with changed content should be rejected");
    check(manager.status().goal_id == first_goal.id
              && manager.status().state == NavigationState::Planning,
          "conflict should preserve the active goal");

    const auto next_goal = makeGoal(6);
    auto preempting_goal = next_goal;
    preempting_goal.pose.x = 3.0;
    check(manager.setGoal(preempting_goal, state, now), "new goal id should preempt the old goal");
    check(manager.status().goal_id == preempting_goal.id && manager.path().poses.empty(),
          "preemption should clear the old path immediately");
    first_gate.release();
    check(waitFor([&] { return manager.status().state == NavigationState::Controlling; }),
          "new goal should be planned after the old planner returns");
    check(second_goal_calls.load(std::memory_order_relaxed) == 1
              && manager.path().goal_id == preempting_goal.id,
          "late first-goal result should not replace the preempting goal path");
    const auto current_status = manager.status();
    const auto current_path = manager.path();
    check(!manager.setGoal(first_goal, state, now), "older preempted goal id should be rejected");
    check(manager.status().goal_id == current_status.goal_id
              && manager.status().state == current_status.state
              && manager.path().goal_id == current_path.goal_id,
          "replaying an older goal should preserve the current goal and path");
}

void testMapRefreshUsesFreshState() {
    const TimePoint now = 10 * second;
    auto planner = std::make_unique<FunctionPlanner>(
        [](const Pose2D& start, const Pose2D& goal, const MapSnapshot&, std::stop_token) {
            return successfulPlan(start, goal);
        });
    auto controller = std::make_unique<FunctionController>(
        [](const RobotState&, const Path2D&, const ControllerContext&) { return ControllerResult{}; });
    NavigationManager manager(std::move(planner), std::move(controller));
    manager.setMap(makeMap(1, now));
    const auto state = makeState(now);
    check(manager.setGoal(makeGoal(7), state, now), "map-refresh goal should be accepted");
    check(waitFor([&] { return manager.status().state == NavigationState::Controlling; }),
          "initial map should produce a path");

    manager.setMap(makeMap(2, now + 10'000'000));
    check(manager.status().state == NavigationState::Replanning && manager.path().poses.empty(),
          "new map version should invalidate the old path immediately");
    check(manager.controlStep(makeState(now + 10'000'000), now + 10'000'000, 0.02).linear_x == 0.0,
          "map refresh should return zero while it queues a fresh-state plan");
    check(waitFor([&] { return manager.status().state == NavigationState::Controlling; }),
          "fresh map and state should produce a replacement path");
    check(manager.path().map_version == 2, "replacement path should use the new map version");
}

void testControllerReplanBudget() {
    const TimePoint now = 10 * second;
    std::atomic<int> calls{0};
    auto planner = std::make_unique<FunctionPlanner>(
        [](const Pose2D& start, const Pose2D& goal, const MapSnapshot&, std::stop_token) {
            return successfulPlan(start, goal);
        });
    auto controller = std::make_unique<FunctionController>(
        [&](const RobotState&, const Path2D&, const ControllerContext&) {
            calls.fetch_add(1, std::memory_order_relaxed);
            return ControllerResult{{}, ErrorCode::PathDeviation, false};
        });
    RuntimeConfig config;
    config.max_replans = 1;
    NavigationManager manager(std::move(planner), std::move(controller), config);
    manager.setMap(makeMap(1, now));
    const auto state = makeState(now);
    check(manager.setGoal(makeGoal(8), state, now), "replan-test goal should be accepted");
    check(waitFor([&] { return manager.status().state == NavigationState::Controlling; }),
          "initial path should become available");
    check(manager.controlStep(state, now + 10'000'000, 0.02).linear_x == 0.0
              && manager.status().state == NavigationState::Replanning,
          "first path deviation should request a zero-command replan");
    check(waitFor([&] { return manager.status().state == NavigationState::Controlling; }),
          "bounded replan should produce a replacement path");
    manager.controlStep(state, now + 20'000'000, 0.02);
    check(manager.status().state == NavigationState::Failed
              && manager.status().error == ErrorCode::PathDeviation && calls == 2,
          "exhausted replan budget should fail with the controller error");
}

void testControllerExceptionAndInvalidStep() {
    const TimePoint now = 10 * second;
    auto makePlanner = [] {
        return std::make_unique<FunctionPlanner>(
            [](const Pose2D& start, const Pose2D& goal, const MapSnapshot&, std::stop_token) {
                return successfulPlan(start, goal);
            });
    };
    {
        auto controller = std::make_unique<FunctionController>(
            [](const RobotState&, const Path2D&, const ControllerContext&) -> ControllerResult {
                throw std::runtime_error("controller error");
            });
        NavigationManager manager(makePlanner(), std::move(controller));
        manager.setMap(makeMap(1, now));
        const auto state = makeState(now);
        check(manager.setGoal(makeGoal(11), state, now), "controller-exception goal should be accepted");
        check(waitFor([&] { return manager.status().state == NavigationState::Controlling; }),
              "controller-exception path should become available");
        const auto command = manager.controlStep(state, now + 10'000'000, 0.02);
        check(command.linear_x == 0.0 && command.angular_z == 0.0
                  && manager.status().state == NavigationState::Failed
                  && manager.status().error == ErrorCode::ControllerFailure,
              "controller exception should stop with ControllerFailure");
    }
    {
        auto controller = std::make_unique<FunctionController>(
            [](const RobotState&, const Path2D&, const ControllerContext&) { return ControllerResult{}; });
        NavigationManager manager(makePlanner(), std::move(controller));
        manager.setMap(makeMap(1, now));
        const auto state = makeState(now);
        check(manager.setGoal(makeGoal(12), state, now), "invalid-step goal should be accepted");
        const auto command = manager.controlStep(state, now + 10'000'000,
                                                std::numeric_limits<double>::quiet_NaN());
        check(command.linear_x == 0.0 && manager.status().state == NavigationState::Failed
                  && manager.status().error == ErrorCode::InvalidInput,
              "invalid control dt should fail safely with a zero command");
    }
}

void testInvalidConfigRejectedBeforeWorkerStart() {
    auto planner = std::make_unique<FunctionPlanner>(
        [](const Pose2D&, const Pose2D&, const MapSnapshot&, std::stop_token) {
            return PlannerResult{};
        });
    auto controller = std::make_unique<FunctionController>(
        [](const RobotState&, const Path2D&, const ControllerContext&) { return ControllerResult{}; });
    RuntimeConfig config;
    config.planning_timeout_ns = 0;
    bool rejected = false;
    try {
        NavigationManager manager(std::move(planner), std::move(controller), config);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "invalid configuration should be rejected before worker startup");
}

void testInvalidGoalRetriesAreRejected() {
    const TimePoint now = 10 * second;
    auto planner = std::make_unique<FunctionPlanner>(
        [](const Pose2D& start, const Pose2D& goal, const MapSnapshot&, std::stop_token) {
            return successfulPlan(start, goal);
        });
    auto controller = std::make_unique<FunctionController>(
        [](const RobotState&, const Path2D&, const ControllerContext&) { return ControllerResult{}; });
    NavigationManager manager(std::move(planner), std::move(controller));
    manager.setMap(makeMap(1, now));
    const auto state = makeState(now);

    const auto zero_id_goal = makeGoal(0);
    check(!manager.setGoal(zero_id_goal, state, now), "zero goal id should be rejected");
    check(!manager.setGoal(zero_id_goal, state, now), "repeated zero goal id should remain rejected");
    check(manager.status().state == NavigationState::Failed
              && manager.status().error == ErrorCode::InvalidInput,
          "invalid goal submissions should retain InvalidInput status");

    const auto valid_goal = makeGoal(14);
    check(manager.setGoal(valid_goal, state, now), "valid goal should replace invalid input");
    check(!manager.setGoal(valid_goal, state, -1), "negative-time duplicate should be rejected");
    const auto after = manager.status();
    check(after.goal_id == valid_goal.id && after.error == ErrorCode::None
              && (after.state == NavigationState::Planning
                  || after.state == NavigationState::Controlling),
          "invalid duplicate must preserve the active goal state");
}

void testStaleLocalizationAndGoalTimeout() {
    const TimePoint now = 10 * second;
    auto makeNoopPlanner = [] {
        return std::make_unique<FunctionPlanner>(
            [](const Pose2D& start, const Pose2D& goal, const MapSnapshot&, std::stop_token) {
                return successfulPlan(start, goal);
            });
    };
    auto makeNoopController = [] {
        return std::make_unique<FunctionController>(
            [](const RobotState&, const Path2D&, const ControllerContext&) { return ControllerResult{}; });
    };
    {
        NavigationManager manager(makeNoopPlanner(), makeNoopController());
        manager.setMap(makeMap(1, now));
        auto state = makeState(now);
        state.localized = false;
        check(!manager.setGoal(makeGoal(9), state, now), "unlocalized goal input should be rejected");
        check(manager.status().state == NavigationState::Failed
                  && manager.status().error == ErrorCode::LocalizationUnavailable,
              "unlocalized initial input should fail with its specific error");
    }
    {
        NavigationManager manager(makeNoopPlanner(), makeNoopController());
        manager.setMap(makeMap(1, now));
        const auto state = makeState(now);
        check(manager.setGoal(makeGoal(10, 5'000'000), state, now), "short-timeout goal should be accepted");
        manager.controlStep(state, now + 5'000'000, 0.02);
        check(manager.status().state == NavigationState::Failed
                  && manager.status().error == ErrorCode::NavigationTimeout,
              "navigation timeout should end the goal");
    }
}

} // namespace

int main() {
    try {
        testAsyncSuccess();
        testPlanningTimeoutDoesNotBlockControl();
        testPlannerWallClockTimeoutWithoutControlTicks();
        testPlannerException();
        testCancellationDiscardsLateResult();
        testDuplicateConflictAndPreemption();
        testMapRefreshUsesFreshState();
        testControllerReplanBudget();
        testControllerExceptionAndInvalidStep();
        testInvalidConfigRejectedBeforeWorkerStart();
        testInvalidGoalRetriesAreRejected();
        testStaleLocalizationAndGoalTimeout();
    } catch (const std::exception& error) {
        std::cerr << "navigation manager test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "asynchronous navigation manager lifecycle: PASS\n";
    return 0;
}

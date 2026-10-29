#include <rbt_navigation/runtime/navigation_manager.hpp>

#include <chrono>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace rbt::navigation {
namespace {

constexpr double max_control_dt_seconds = 0.5;

bool sameGoal(const NavigationGoal& lhs, const NavigationGoal& rhs) noexcept {
    return lhs.id == rhs.id && lhs.pose.x == rhs.pose.x && lhs.pose.y == rhs.pose.y
        && lhs.pose.yaw == rhs.pose.yaw && lhs.timeout_ns == rhs.timeout_ns;
}

bool terminal(NavigationState state) noexcept {
    return state == NavigationState::Succeeded || state == NavigationState::Canceled
        || state == NavigationState::Failed;
}

RuntimeConfig validatedConfig(RuntimeConfig config, const IGlobalPlanner* planner,
                              const ILocalController* controller) {
    if (!planner || !controller) {
        throw std::invalid_argument("NavigationManager requires a planner and controller");
    }
    if (!std::isfinite(config.robot_radius) || config.robot_radius < 0.0
        || config.max_input_age_ns <= 0 || config.max_map_age_ns <= 0
        || config.planning_timeout_ns <= 0) {
        throw std::invalid_argument("NavigationManager configuration is invalid");
    }
    return config;
}

} // namespace

NavigationManager::NavigationManager(std::unique_ptr<IGlobalPlanner> planner,
                                     std::unique_ptr<ILocalController> controller,
                                     RuntimeConfig config)
    : planner_(std::move(planner)), controller_(std::move(controller)),
      config_(validatedConfig(config, planner_.get(), controller_.get())),
      planning_thread_([this](std::stop_token stop) { planningLoop(stop); }) {}

NavigationManager::~NavigationManager() {
    {
        std::lock_guard lock(mutex_);
        ++generation_;
        pending_.reset();
        plan_stop_.request_stop();
    }
    planning_thread_.request_stop();
    cv_.notify_all();
    // jthread joins here. Injected planners must cooperate with stop for prompt shutdown.
}

void NavigationManager::setMap(MapSnapshot map) {
    std::lock_guard lock(mutex_);
    const auto previous_version = map_.version;
    map_ = std::move(map);
    if (previous_version == map_.version) {
        return;
    }

    status_.map_version = map_.version;
    if (!activeLocked()) {
        return;
    }

    plan_stop_.request_stop();
    ++generation_;
    pending_.reset();
    path_ = {};
    previous_command_ = {};
    planning_started_ns_ = -1; // controlStep queues a fresh-state plan after this map change.
    status_.state = NavigationState::Replanning;
    status_.error = ErrorCode::None;
    cv_.notify_all();
}

bool NavigationManager::setGoal(NavigationGoal goal, const RobotState& state, TimePoint now) {
    std::lock_guard lock(mutex_);

    const bool goal_valid = goal.id != 0 && finite(goal.pose) && goal.timeout_ns > 0 && now >= 0;
    if (!goal_valid) {
        if (!activeLocked() && !goal_) {
            status_.goal_id = goal.id;
            status_.map_version = map_.version;
            failLocked(ErrorCode::InvalidInput);
        }
        return false;
    }

    if (goal_ && goal.id == goal_->id) {
        return sameGoal(goal, *goal_);
    }
    if (goal_ && goal.id < goal_->id) {
        return false;
    }

    plan_stop_.request_stop();
    ++generation_;
    pending_.reset();
    path_ = {};
    previous_command_ = {};
    goal_ = goal;
    goal_started_ns_ = now;
    latest_now_ns_ = now;
    latest_state_ = state;
    planning_started_ns_ = -1;
    replans_ = 0;
    status_ = {NavigationState::Planning, ErrorCode::None, goal.id, map_.version};

    if (!inputsValidLocked(state, now)) {
        failLocked(status_.error);
        return false;
    }

    queuePlanLocked(state, now, false);
    return true;
}

bool NavigationManager::cancel(std::uint64_t goal_id) {
    std::lock_guard lock(mutex_);
    if (!goal_ || (goal_id != 0 && goal_id != goal_->id)) {
        return false;
    }
    if (status_.state == NavigationState::Canceled) {
        return true;
    }
    if (!activeLocked()) {
        return false;
    }

    plan_stop_.request_stop();
    ++generation_;
    pending_.reset();
    path_ = {};
    previous_command_ = {};
    status_.state = NavigationState::Canceled;
    status_.error = ErrorCode::Canceled;
    cv_.notify_all();
    return true;
}

Twist2D NavigationManager::controlStep(const RobotState& state, TimePoint now, double dt) {
    std::lock_guard lock(mutex_);
    if (terminal(status_.state)) {
        return {};
    }
    if (!activeLocked()) {
        return {};
    }

    if (now < 0 || now < latest_now_ns_ || !std::isfinite(dt) || dt <= 0.0
        || dt > max_control_dt_seconds) {
        failLocked(ErrorCode::InvalidInput);
        return {};
    }
    latest_now_ns_ = now;
    latest_state_ = state;

    if (now < goal_started_ns_ || now - goal_started_ns_ >= goal_->timeout_ns) {
        failLocked(ErrorCode::NavigationTimeout);
        return {};
    }
    if (!inputsValidLocked(state, now)) {
        failLocked(status_.error);
        return {};
    }

    if (status_.state == NavigationState::Planning || status_.state == NavigationState::Replanning) {
        if (planning_started_ns_ < 0) {
            queuePlanLocked(state, now, true);
        }
        if (now < planning_started_ns_
            || now - planning_started_ns_ >= config_.planning_timeout_ns) {
            failLocked(ErrorCode::PlanningTimeout);
        }
        return {};
    }

    if (path_.poses.empty()) {
        failLocked(ErrorCode::EmptyPath);
        return {};
    }
    if (path_.map_version != map_.version) {
        path_ = {};
        previous_command_ = {};
        status_.state = NavigationState::Replanning;
        status_.error = ErrorCode::None;
        planning_started_ns_ = -1;
        queuePlanLocked(state, now, true);
        return {};
    }

    ControllerResult result;
    try {
        const ControllerContext context{now, dt, previous_command_, map_, config_.robot_radius,
                                        config_.max_input_age_ns, config_.max_map_age_ns, config_.footprint};
        result = controller_->computeCommand(state, path_, context);
    } catch (...) {
        failLocked(ErrorCode::ControllerFailure);
        return {};
    }

    if (result.error == ErrorCode::PathDeviation || result.error == ErrorCode::Collision) {
        previous_command_ = {};
        path_ = {};
        if (replans_ >= config_.max_replans) {
            failLocked(result.error);
            return {};
        }
        ++replans_;
        status_.state = NavigationState::Replanning;
        status_.error = ErrorCode::None;
        planning_started_ns_ = -1;
        queuePlanLocked(state, now, true);
        return {};
    }
    if (result.error != ErrorCode::None) {
        failLocked(result.error);
        return {};
    }
    if (!finite(result.command)) {
        failLocked(ErrorCode::ControllerFailure);
        return {};
    }
    if (result.reached_goal) {
        path_ = {};
        previous_command_ = {};
        status_.state = NavigationState::Succeeded;
        status_.error = ErrorCode::None;
        return {};
    }

    previous_command_ = result.command;
    return result.command;
}

NavigationStatus NavigationManager::status() const {
    std::lock_guard lock(mutex_);
    return status_;
}

Path2D NavigationManager::path() const {
    std::lock_guard lock(mutex_);
    return path_;
}

void NavigationManager::planningLoop(std::stop_token stop) {
    while (!stop.stop_requested()) {
        Request request;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, stop, [this] { return pending_.has_value(); });
            if (stop.stop_requested()) {
                return;
            }
            if (!pending_) {
                continue;
            }
            request = std::move(*pending_);
            pending_.reset();
        }

        PlannerResult result;
        const auto planning_wall_started = std::chrono::steady_clock::now();
        try {
            result = planner_->plan(request.start, request.goal.pose, request.map, request.stop);
        } catch (...) {
            result.error = ErrorCode::PlannerFailure;
        }
        const auto planning_wall_elapsed = std::chrono::steady_clock::now() - planning_wall_started;

        std::lock_guard lock(mutex_);
        if (stop.stop_requested() || request.stop.stop_requested()
            || request.generation != generation_ || !activeLocked()
            || !goal_ || status_.goal_id != request.goal.id
            || map_.version != request.map.version) {
            continue;
        }
        if (planning_wall_elapsed >= std::chrono::nanoseconds(config_.planning_timeout_ns)) {
            failLocked(ErrorCode::PlanningTimeout);
            continue;
        }
        if (latest_now_ns_ < planning_started_ns_
            || latest_now_ns_ - planning_started_ns_ >= config_.planning_timeout_ns) {
            failLocked(ErrorCode::PlanningTimeout);
            continue;
        }
        if (result.error != ErrorCode::None) {
            failLocked(result.error);
            continue;
        }
        if (result.path.poses.empty()) {
            failLocked(ErrorCode::EmptyPath);
            continue;
        }

        result.path.goal_id = request.goal.id;
        result.path.map_version = request.map.version;
        path_ = std::move(result.path);
        previous_command_ = {};
        status_.state = NavigationState::Controlling;
        status_.error = ErrorCode::None;
        status_.goal_id = request.goal.id;
        status_.map_version = request.map.version;
    }
}

void NavigationManager::queuePlanLocked(const RobotState& state, TimePoint now, bool replan) {
    if (!goal_) {
        failLocked(ErrorCode::InvalidInput);
        return;
    }
    plan_stop_.request_stop();
    plan_stop_ = std::stop_source{};
    ++generation_;
    Request request;
    request.generation = generation_;
    request.goal = *goal_;
    request.start = state.pose;
    request.map = map_;
    request.stop = plan_stop_.get_token();
    pending_ = std::move(request);
    latest_state_ = state;
    planning_started_ns_ = now;
    previous_command_ = {};
    status_.state = replan ? NavigationState::Replanning : NavigationState::Planning;
    status_.error = ErrorCode::None;
    status_.goal_id = goal_->id;
    status_.map_version = map_.version;
    cv_.notify_one();
}

void NavigationManager::failLocked(ErrorCode error) {
    plan_stop_.request_stop();
    ++generation_;
    pending_.reset();
    path_ = {};
    previous_command_ = {};
    status_.state = NavigationState::Failed;
    status_.error = error;
    if (goal_) {
        status_.goal_id = goal_->id;
    }
    status_.map_version = map_.version;
    cv_.notify_all();
}

bool NavigationManager::inputsValidLocked(const RobotState& state, TimePoint now) {
    ErrorCode error = ErrorCode::None;
    if (!map_.valid()) {
        error = ErrorCode::MapUnavailable;
    } else if (!fresh(map_.stamp_ns, now, config_.max_map_age_ns)) {
        error = ErrorCode::StaleMap;
    } else if (!state.localized) {
        error = ErrorCode::LocalizationUnavailable;
    } else if (!finite(state.pose) || !finite(state.velocity)) {
        error = ErrorCode::InvalidInput;
    } else if (!fresh(state.tf_stamp_ns, now, config_.max_input_age_ns)) {
        error = ErrorCode::StaleTf;
    } else if (!fresh(state.odom_stamp_ns, now, config_.max_input_age_ns)) {
        error = ErrorCode::StaleOdometry;
    }
    if (error != ErrorCode::None) {
        status_.error = error;
        return false;
    }
    return true;
}

bool NavigationManager::activeLocked() const {
    return status_.state == NavigationState::Planning || status_.state == NavigationState::Controlling
        || status_.state == NavigationState::Replanning;
}

} // namespace rbt::navigation

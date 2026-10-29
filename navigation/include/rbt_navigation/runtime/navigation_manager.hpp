#pragma once
#include <rbt_navigation/controller/pure_pursuit.hpp>
#include <rbt_navigation/planner/a_star.hpp>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace rbt::navigation {
struct RuntimeConfig {
    double robot_radius{0.18};
    TimePoint max_input_age_ns{250'000'000}, max_map_age_ns{2 * second};
    TimePoint planning_timeout_ns{second};
    std::size_t max_replans{3};
    std::shared_ptr<const FootprintCollisionChecker> footprint{};
};
class NavigationManager {
public:
    NavigationManager(std::unique_ptr<IGlobalPlanner> planner,
                      std::unique_ptr<ILocalController> controller,
                      RuntimeConfig config = {});
    ~NavigationManager();
    NavigationManager(const NavigationManager&) = delete;
    NavigationManager& operator=(const NavigationManager&) = delete;
    void setMap(MapSnapshot map);
    // Goal ids increase within this manager; id=0 is reserved for cancel-all.
    // Same id+content is idempotent; conflicting or older ids are rejected.
    // A newer id preempts, cancels search and invalidates all old path/results.
    bool setGoal(NavigationGoal goal,const RobotState& state,TimePoint now);
    bool cancel(std::uint64_t goal_id); // id=0 cancels current goal
    Twist2D controlStep(const RobotState& state,TimePoint now,double dt);
    [[nodiscard]] NavigationStatus status() const;
    [[nodiscard]] Path2D path() const;
private:
    struct Request {
        std::uint64_t generation{};
        NavigationGoal goal;
        Pose2D start;
        MapSnapshot map;
        std::stop_token stop;
    };
    void planningLoop(std::stop_token stop);
    void queuePlanLocked(const RobotState& state,TimePoint now,bool replan);
    void failLocked(ErrorCode error);
    bool inputsValidLocked(const RobotState& state,TimePoint now);
    bool activeLocked() const;
    std::unique_ptr<IGlobalPlanner> planner_;
    std::unique_ptr<ILocalController> controller_;
    RuntimeConfig config_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    NavigationStatus status_;
    MapSnapshot map_;
    Path2D path_;
    std::optional<NavigationGoal> goal_;
    std::optional<Request> pending_;
    RobotState latest_state_;
    Twist2D previous_command_;
    TimePoint goal_started_ns_{}, planning_started_ns_{}, latest_now_ns_{};
    std::uint64_t generation_{};
    std::size_t replans_{};
    std::stop_source plan_stop_;
    std::jthread planning_thread_; // declared last; joined before state destruction
};
} // namespace rbt::navigation

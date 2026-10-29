#pragma once
#include <rbt_navigation/map/grid_map.hpp>
#include <chrono>

namespace rbt::navigation {
struct PlannerConfig {
    double robot_radius{0.18};
    std::chrono::nanoseconds timeout{std::chrono::milliseconds(500)};
};
class IGlobalPlanner {
public:
    virtual ~IGlobalPlanner() = default;
    virtual PlannerResult plan(const Pose2D& start,const Pose2D& goal,
                               const MapSnapshot& map,std::stop_token stop = {}) = 0;
};
class AStarPlanner final : public IGlobalPlanner {
public:
    explicit AStarPlanner(PlannerConfig config = {});
    PlannerResult plan(const Pose2D& start,const Pose2D& goal,
                       const MapSnapshot& map,std::stop_token stop = {}) override;
private:
    PlannerConfig config_;
};
} // namespace rbt::navigation

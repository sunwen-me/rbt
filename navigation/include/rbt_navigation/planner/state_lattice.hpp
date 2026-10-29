#pragma once

#include <rbt_navigation/geometry/footprint.hpp>
#include <rbt_navigation/planner/a_star.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rbt::navigation {

struct LatticePrimitive {
    std::uint32_t trajectory_id{};
    std::uint32_t start_angle{};
    std::uint32_t end_angle{};
    double trajectory_length{};
    std::vector<Pose2D> poses;
};

struct LatticeControlSet {
    double resolution{};
    double min_turning_radius{};
    std::vector<double> headings;
    std::vector<LatticePrimitive> primitives;
};

[[nodiscard]] LatticeControlSet loadLatticeControlSet(const std::string& path);

struct StateLatticeConfig {
    std::chrono::nanoseconds timeout{std::chrono::milliseconds(500)};
    double planning_clearance{0.0};
    double rotation_penalty{5.0};
    double reverse_penalty{2.0};
    double change_penalty{0.05};
    double non_straight_penalty{1.05};
    double cost_penalty{2.0};
    bool allow_reverse_expansion{false};
    double goal_tolerance{0.25};
    std::size_t max_iterations{1'000'000};
};

class StateLatticePlanner final : public IGlobalPlanner {
public:
    StateLatticePlanner(
        LatticeControlSet controls,
        std::shared_ptr<const FootprintCollisionChecker> collision_checker,
        StateLatticeConfig config = {});

    [[nodiscard]] PlannerResult plan(
        const Pose2D& start, const Pose2D& goal, const MapSnapshot& map,
        std::stop_token stop = {}) override;

private:
    LatticeControlSet controls_;
    std::shared_ptr<const FootprintCollisionChecker> collision_checker_;
    StateLatticeConfig config_;
};

} // namespace rbt::navigation

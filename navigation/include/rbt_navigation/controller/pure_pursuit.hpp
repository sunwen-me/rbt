#pragma once
#include <rbt_navigation/controller/feasible_path_handler.hpp>
#include <rbt_navigation/map/grid_map.hpp>

namespace rbt::navigation {
enum class PursuitGeometry { PurePursuit, VectorPursuit };

struct ControllerConfig {
    double max_linear{0.6}, max_angular{1.5};
    double max_acceleration{0.8}, max_angular_acceleration{2.5};
    double max_deceleration{0.8}, max_angular_deceleration{2.5}; // positive magnitudes
    double min_lookahead{0.3}, max_lookahead{0.8}, lookahead_time{1.5};
    double position_tolerance{0.08}, yaw_tolerance{0.06};
    double max_path_deviation{0.8};
    double regulated_min_radius{0.9}, regulated_min_speed{0.05};
    double cost_scaling_distance{0.6}, cost_scaling_gain{1.0};
    double approach_scaling_distance{0.6}, min_approach_speed{0.03};
    double rotate_to_heading_angle{0.785}, rotate_to_heading_speed{1.2};
    double collision_horizon{1.0}; // Nav2 horizon, capped at carrot
    double reaction_time{0.25}; // separate measured-motion stopping check
    double curvature_lookahead{0.6};
    bool use_fixed_curvature_lookahead{false};
    bool use_dynamic_window{true};
    bool allow_reverse{false};
    bool use_feasible_path_handler{false};
    bool enforce_path_inversion{true};
    bool enforce_path_rotation{true};
    double minimum_path_rotation{0.01};
    PursuitGeometry geometry{PursuitGeometry::PurePursuit};
    double vector_pursuit_k{5.0};
    double vector_pursuit_min_radius{0.10}; // metres
    bool vector_pursuit_use_heading_from_path{true};
};
class ILocalController {
public:
    virtual ~ILocalController() = default;
    virtual ControllerResult computeCommand(const RobotState& state,const Path2D& path,
                                            const ControllerContext& context) = 0;
};
class RegulatedPurePursuit final : public ILocalController {
public:
    explicit RegulatedPurePursuit(ControllerConfig config = {});
    ControllerResult computeCommand(const RobotState& state,const Path2D& path,
                                    const ControllerContext& context) override;
private:
    ControllerConfig config_;
    FeasiblePathHandler feasible_path_handler_;
};
// Compatibility for the initial navigation API; the implementation is RPP.
using DiffDrivePurePursuit = RegulatedPurePursuit;
} // namespace rbt::navigation

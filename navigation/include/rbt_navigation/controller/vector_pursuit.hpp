#pragma once

#include <rbt_navigation/controller/pure_pursuit.hpp>

namespace rbt::navigation {

// Experimental wrapper that selects Vector Pursuit geometry while sharing
// RBT's regulated speed, collision, and command-limit behavior.
class VectorPursuit final : public ILocalController {
public:
    explicit VectorPursuit(ControllerConfig config = {});
    ControllerResult computeCommand(const RobotState& state, const Path2D& path,
                                    const ControllerContext& context) override;

private:
    RegulatedPurePursuit controller_;
};

} // namespace rbt::navigation

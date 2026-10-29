#pragma once

#include <rbt_navigation/core/types.hpp>

namespace rbt::navigation {

// Legacy adapter interface retained for simulation/source compatibility only.
// Real chassis drivers live in the independent rbt_base_controller package.
class SafeBaseInterface {
public:
    virtual ~SafeBaseInterface() = default;
    virtual void apply(Twist2D velocity, double dt) = 0;
};

class SimulatedBaseAdapter final : public SafeBaseInterface {
public:
    explicit SimulatedBaseAdapter(Pose2D initial = {}) : pose_(initial) {}
    void apply(Twist2D velocity, double dt) override;
    [[nodiscard]] Pose2D pose() const noexcept { return pose_; }
    [[nodiscard]] Twist2D velocity() const noexcept { return velocity_; }

private:
    Pose2D pose_;
    Twist2D velocity_;
};

} // namespace rbt::navigation

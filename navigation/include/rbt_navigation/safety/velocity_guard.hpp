#pragma once

#include <rbt_navigation/core/types.hpp>

namespace rbt::navigation {

struct VelocityCommand {
    std::uint64_t source_id{}, sequence{};
    TimePoint timestamp_ns{}, valid_until_ns{};
    Twist2D velocity;
};

struct VelocityGuardConfig {
    std::uint64_t allowed_source{1}; // explicit single configured authority
    double max_linear{0.7}, max_angular{1.8};
    double max_acceleration{1.0}, max_angular_acceleration{3.0};
    TimePoint watchdog_ns{250'000'000};
};

enum class CommandRejection {
    None, WrongSource, Replay, InvalidTime, NonFinite, EmergencyStop
};

class VelocityGuard {
public:
    explicit VelocityGuard(VelocityGuardConfig config = {});
    CommandRejection accept(const VelocityCommand& command, TimePoint now);
    // Independent process calls this even with no navigation input. Expiry,
    // invalid time and emergency stop return immediate zero; explicit zero
    // command (cancel) uses the configured controlled deceleration.
    Twist2D step(TimePoint now, double dt);
    void setEmergencyStop(bool active);
    [[nodiscard]] bool emergencyStop() const noexcept { return estop_; }
    [[nodiscard]] bool watchdogExpired(TimePoint now) const noexcept;
    // Exact expiry from the active input command; -1 after invalidation.
    [[nodiscard]] TimePoint commandValidUntil() const noexcept { return valid_until_ns_; }

private:
    VelocityGuardConfig config_;
    Twist2D target_, output_;
    std::uint64_t last_sequence_{};
    TimePoint last_received_ns_{-1}, valid_until_ns_{-1}, last_step_ns_{-1};
    bool estop_{false};
};

} // namespace rbt::navigation

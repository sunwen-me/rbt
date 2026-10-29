#include <rbt_navigation/safety/velocity_guard.hpp>
#include <rbt_navigation/simulation/simulated_base.hpp>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace rbt::navigation {
namespace {

constexpr double max_step_seconds = 0.2;

bool validConfig(const VelocityGuardConfig& config) noexcept {
    return config.allowed_source != 0 &&
           std::isfinite(config.max_linear) && config.max_linear > 0.0 &&
           std::isfinite(config.max_angular) && config.max_angular > 0.0 &&
           std::isfinite(config.max_acceleration) && config.max_acceleration > 0.0 &&
           std::isfinite(config.max_angular_acceleration) &&
           config.max_angular_acceleration > 0.0 && config.watchdog_ns > 0;
}

double moveToward(double current, double target, double maximum_delta) noexcept {
    if (current == target) return target;
    if (!std::isfinite(maximum_delta) || maximum_delta < 0.0) return target;

    if (target > current) {
        const double difference = target - current;
        return maximum_delta >= difference ? target : current + maximum_delta;
    }
    const double difference = current - target;
    return maximum_delta >= difference ? target : current - maximum_delta;
}

bool validStep(double dt) noexcept {
    return std::isfinite(dt) && dt > 0.0 && dt <= max_step_seconds;
}

} // namespace

VelocityGuard::VelocityGuard(VelocityGuardConfig config) : config_(config) {
    if (!validConfig(config_)) {
        throw std::invalid_argument("VelocityGuard requires finite positive limits and a nonzero source");
    }
}

CommandRejection VelocityGuard::accept(const VelocityCommand& command, TimePoint now) {
    if (estop_) return CommandRejection::EmergencyStop;
    if (command.source_id != config_.allowed_source) return CommandRejection::WrongSource;
    if (command.sequence == 0 || command.sequence <= last_sequence_) {
        return CommandRejection::Replay;
    }
    if (!finite(command.velocity)) return CommandRejection::NonFinite;

    if (now < 0 || command.timestamp_ns < 0 || command.timestamp_ns > now ||
        command.valid_until_ns <= now || command.valid_until_ns < command.timestamp_ns ||
        command.valid_until_ns - command.timestamp_ns > config_.watchdog_ns ||
        (last_received_ns_ >= 0 && now < last_received_ns_) ||
        (last_step_ns_ >= 0 && now < last_step_ns_)) {
        return CommandRejection::InvalidTime;
    }

    target_.linear_x = std::clamp(command.velocity.linear_x, -config_.max_linear, config_.max_linear);
    target_.angular_z = std::clamp(command.velocity.angular_z, -config_.max_angular, config_.max_angular);
    last_sequence_ = command.sequence;
    last_received_ns_ = now;
    valid_until_ns_ = command.valid_until_ns;
    return CommandRejection::None;
}

bool VelocityGuard::watchdogExpired(TimePoint now) const noexcept {
    if (now < 0 || last_received_ns_ < 0 || now < last_received_ns_) return true;
    return now - last_received_ns_ >= config_.watchdog_ns;
}

Twist2D VelocityGuard::step(TimePoint now, double dt) {
    const bool time_regressed = now >= 0 && last_step_ns_ >= 0 && now < last_step_ns_;
    if (now < 0 || time_regressed || !validStep(dt)) {
        target_ = {};
        output_ = {};
        last_received_ns_ = -1;
        valid_until_ns_ = -1;
        if (now >= 0) last_step_ns_ = now;
        return output_;
    }

    const TimePoint time_anchor = last_step_ns_ >= 0 ? last_step_ns_ : last_received_ns_;
    const double elapsed = time_anchor < 0
                               ? 0.0
                               : static_cast<double>(now - time_anchor) /
                                     static_cast<double>(second);
    const double effective_dt = std::min(dt, elapsed);
    last_step_ns_ = now;

    if (estop_ || watchdogExpired(now) || valid_until_ns_ < 0 || now >= valid_until_ns_) {
        target_ = {};
        output_ = {};
        last_received_ns_ = -1;
        valid_until_ns_ = -1;
        return output_;
    }

    output_.linear_x = moveToward(output_.linear_x, target_.linear_x,
                                  config_.max_acceleration * effective_dt);
    output_.angular_z = moveToward(output_.angular_z, target_.angular_z,
                                   config_.max_angular_acceleration * effective_dt);
    return output_;
}

void VelocityGuard::setEmergencyStop(bool active) {
    if (!active) {
        estop_ = false;
        return;
    }

    estop_ = true;
    target_ = {};
    output_ = {};
    last_received_ns_ = -1;
    valid_until_ns_ = -1;
}

void SimulatedBaseAdapter::apply(Twist2D velocity, double dt) {
    if (!finite(pose_) || !finite(velocity) || !validStep(dt)) {
        throw std::invalid_argument("simulated base requires finite state and velocity with a bounded positive dt");
    }

    const double delta_yaw = velocity.angular_z * dt;
    if (!std::isfinite(delta_yaw)) {
        throw std::invalid_argument("simulated base yaw increment is not finite");
    }

    const double half_delta = delta_yaw * 0.5;
    double scale;
    if (std::abs(half_delta) < 1e-6) {
        const double square = half_delta * half_delta;
        scale = 1.0 - square / 6.0 + square * square / 120.0;
    } else {
        scale = std::sin(half_delta) / half_delta;
    }

    const double travel = velocity.linear_x * dt * scale;
    const double middle_yaw = pose_.yaw + half_delta;
    Pose2D next{
        pose_.x + travel * std::cos(middle_yaw),
        pose_.y + travel * std::sin(middle_yaw),
        normalizeAngle(pose_.yaw + delta_yaw),
    };
    if (!finite(next)) {
        throw std::invalid_argument("simulated base integration produced a non-finite pose");
    }

    pose_ = next;
    velocity_ = velocity;
}

} // namespace rbt::navigation

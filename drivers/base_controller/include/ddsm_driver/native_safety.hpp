#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include "rbt/hardware.hpp"
#include "rbt/navwire.hpp"
#include "rbt/wait.hpp"
#include "rbt_hardware/native.hpp"

namespace ddsm_driver
{

enum class CommandRejection
{
  NONE,
  CLOCK,
  SOURCE,
  SEQUENCE,
  TIME,
  TTL,
  NONFINITE,
  LIMIT,
  ESTOP,
};

struct CommandSnapshot
{
  double linear_x{0.0};
  double angular_z{0.0};
  bool watchdog_expired{true};
  bool emergency_stop{false};
};

struct NativeSafetyConfig
{
  std::uint64_t command_source{100};
  std::uint64_t estop_source{2};
  std::int64_t command_timeout_ns{600'000'000};
  std::int64_t maximum_ttl_ns{250'000'000};
  std::int64_t maximum_age_ns{600'000'000};
  double maximum_linear{0.7};
  double maximum_angular{1.8};
};

class NativeCommandGate
{
public:
  explicit NativeCommandGate(NativeSafetyConfig config = {}) : config_(config)
  {
    if (config_.command_source == 0 || config_.estop_source == 0 ||
        config_.command_source == config_.estop_source ||
        config_.command_timeout_ns <= 0 || config_.maximum_ttl_ns <= 0 ||
        config_.maximum_age_ns <= 0 || !std::isfinite(config_.maximum_linear) ||
        !std::isfinite(config_.maximum_angular) || config_.maximum_linear <= 0.0 ||
        config_.maximum_angular <= 0.0) {
      throw std::invalid_argument("invalid native command gate configuration");
    }
  }

  CommandRejection accept(const rbt::navwire::VelocityCommand & command,
                          const rbt::time::Timestamp & now)
  {
    std::int64_t stamp_ns = 0;
    try {
      stamp_ns = rbt::hardware::native::decodeStamp(command.stamp, now.clock);
    } catch (const std::invalid_argument &) {
      return reject(CommandRejection::CLOCK);
    }

    if (command.source_id != config_.command_source) {
      return reject(CommandRejection::SOURCE);
    }
    if (command.sequence == 0 || command.sequence <= command_sequence_) {
      return reject(CommandRejection::SEQUENCE);
    }
    if (estop_active_) {
      return reject(CommandRejection::ESTOP);
    }
    if (!rbt::hardware::native::fresh(stamp_ns, now.ns, config_.maximum_age_ns) ||
        command.valid_until_ns <= now.ns) {
      return reject(CommandRejection::TIME);
    }
    const std::int64_t ttl_ns = command.valid_until_ns - stamp_ns;
    if (ttl_ns <= 0 || ttl_ns > config_.maximum_ttl_ns) {
      return reject(CommandRejection::TTL);
    }
    if (!std::isfinite(command.linear_x) || !std::isfinite(command.angular_z)) {
      return reject(CommandRejection::NONFINITE);
    }
    if (std::abs(command.linear_x) > config_.maximum_linear ||
        std::abs(command.angular_z) > config_.maximum_angular) {
      return reject(CommandRejection::LIMIT);
    }

    command_sequence_ = command.sequence;
    const std::int64_t watchdog_deadline =
      now.ns > std::numeric_limits<std::int64_t>::max() - config_.command_timeout_ns ?
      std::numeric_limits<std::int64_t>::max() : now.ns + config_.command_timeout_ns;
    command_deadline_ns_ = std::min(command.valid_until_ns, watchdog_deadline);
    linear_x_ = command.linear_x;
    angular_z_ = command.angular_z;
    has_command_ = true;
    return CommandRejection::NONE;
  }

  CommandRejection accept(const rbt::navwire::EmergencyStop & estop,
                          const rbt::time::Timestamp & now)
  {
    std::int64_t stamp_ns = 0;
    try {
      stamp_ns = rbt::hardware::native::decodeStamp(estop.stamp, now.clock);
    } catch (const std::invalid_argument &) {
      return reject(CommandRejection::CLOCK);
    }

    if (estop.source_id != config_.estop_source) {
      return reject(CommandRejection::SOURCE);
    }
    if (estop.sequence == 0 || estop.sequence <= estop_sequence_) {
      return reject(CommandRejection::SEQUENCE);
    }
    if (estop.reserved != 0 || estop.active > 1) {
      return reject(CommandRejection::TIME);
    }
    if (!rbt::hardware::native::fresh(stamp_ns, now.ns, config_.maximum_age_ns)) {
      return reject(CommandRejection::TIME);
    }

    const bool was_active = estop_active_;
    estop_sequence_ = estop.sequence;
    estop_active_ = estop.active != 0;
    if (estop_active_ || was_active) {
      clearCommand();
    }
    return CommandRejection::NONE;
  }

  CommandSnapshot tick(std::int64_t now_ns)
  {
    if (has_command_ && now_ns >= command_deadline_ns_) {
      clearCommand();
    }
    if (estop_active_ || !has_command_) {
      return {0.0, 0.0, !estop_active_ && !has_command_, estop_active_};
    }
    return {linear_x_, angular_z_, false, false};
  }

  void clearCommand() noexcept
  {
    has_command_ = false;
    linear_x_ = 0.0;
    angular_z_ = 0.0;
    command_deadline_ns_ = 0;
  }

  void rejectExternal() noexcept { ++rejected_commands_; }
  [[nodiscard]] std::uint64_t rejectedCommands() const noexcept { return rejected_commands_; }
  [[nodiscard]] std::uint64_t commandSequence() const noexcept { return command_sequence_; }
  [[nodiscard]] std::uint64_t estopSequence() const noexcept { return estop_sequence_; }
  [[nodiscard]] bool emergencyStop() const noexcept { return estop_active_; }

private:
  CommandRejection reject(CommandRejection reason) noexcept
  {
    ++rejected_commands_;
    return reason;
  }

  NativeSafetyConfig config_;
  std::uint64_t command_sequence_{0};
  std::uint64_t estop_sequence_{0};
  std::uint64_t rejected_commands_{0};
  std::int64_t command_deadline_ns_{0};
  double linear_x_{0.0};
  double angular_z_{0.0};
  bool has_command_{false};
  bool estop_active_{false};
};

class IoFaultLatch
{
public:
  IoFaultLatch(int failure_threshold, int recovery_cycles)
  : failure_threshold_(failure_threshold), recovery_cycles_(recovery_cycles)
  {
    if (failure_threshold_ <= 0 || recovery_cycles_ <= 0) {
      throw std::invalid_argument("I/O failure and recovery thresholds must be positive");
    }
  }

  // Returns true when the fault latch changes state.
  bool observe(bool healthy, bool motor_error)
  {
    if (!healthy) {
      if (consecutive_failures_ < failure_threshold_) {
        ++consecutive_failures_;
      }
      recovery_count_ = 0;
      if ((motor_error || consecutive_failures_ >= failure_threshold_) && !active_) {
        active_ = true;
        return true;
      }
      return false;
    }

    consecutive_failures_ = 0;
    if (!active_) {
      return false;
    }
    ++recovery_count_;
    if (recovery_count_ >= recovery_cycles_) {
      active_ = false;
      recovery_count_ = 0;
      return true;
    }
    return false;
  }

  [[nodiscard]] bool active() const noexcept { return active_; }
  [[nodiscard]] int consecutiveFailures() const noexcept { return consecutive_failures_; }

private:
  int failure_threshold_;
  int recovery_cycles_;
  int consecutive_failures_{0};
  int recovery_count_{0};
  bool active_{false};
};

}  // namespace ddsm_driver

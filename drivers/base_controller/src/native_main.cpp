#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "ddsm_driver/ddsm_control.hpp"
#include "ddsm_driver/ddsm_motor.hpp"
#include "ddsm_driver/diff_drive_kinematics.hpp"
#include "ddsm_driver/native_config.hpp"
#include "ddsm_driver/native_safety.hpp"
#include "ddsm_driver/odometry.hpp"
#include "ddsm_driver/pid_controller.hpp"
#include "ddsm_driver/serial_comm.hpp"
#include "rbt_hardware/native.hpp"
#include "rbt/navwire.hpp"

namespace ddsm_driver
{
namespace
{

using SteadyClock = std::chrono::steady_clock;

struct WheelTelemetry
{
  rbt::hardware::WheelOdometry wheel{};
  rbt::navwire::Odom2D odom{};
};

struct SharedState
{
  explicit SharedState(NativeSafetyConfig safety) : command_gate(safety) {}

  mutable std::mutex mutex;
  NativeCommandGate command_gate;
  std::optional<WheelTelemetry> latest_telemetry;
  double measured_angular_z{0.0};
  std::int64_t imu_stamp_ns{0};
  std::int64_t feedback_stamp_ns{0};
  std::uint64_t io_failures{0};
  int left_motor_command{0};
  int right_motor_command{0};
  bool has_imu{false};
  bool io_fault{false};
  bool feedback_seen{false};
  bool watchdog_expired{true};
  bool io_thread_failed{false};
};

std::int64_t secondsToNs(double seconds)
{
  return static_cast<std::int64_t>(seconds * 1e9);
}

class NativeBaseDriver
{
public:
  NativeBaseDriver(const DriverConfig & config, const NativeOptions & options,
                   rbt::time::HostClock & clock)
  : config_(config), clock_(clock),
    state_(NativeSafetyConfig{
      options.command_source,
      options.estop_source,
      secondsToNs(config.command_timeout),
      250'000'000,
      600'000'000,
      options.maximum_linear,
      options.maximum_angular})
  {
  }

  NativeBaseDriver(const NativeBaseDriver &) = delete;
  NativeBaseDriver & operator=(const NativeBaseDriver &) = delete;

  ~NativeBaseDriver() { stop(); }

  void start()
  {
    std::promise<void> initialized;
    auto ready = initialized.get_future();
    io_thread_ = std::jthread(
      [this, initialized = std::move(initialized)](std::stop_token stop) mutable {
        ioLoop(stop, std::move(initialized));
      });
    ready.get();
  }

  void stop() noexcept
  {
    if (io_thread_.joinable()) {
      io_thread_.request_stop();
      io_thread_.join();
    }
  }

  bool acceptCommand(const rbt::navwire::VelocityCommand & command)
  {
    const auto now = rbt::hardware::native::now(clock_);
    std::lock_guard<std::mutex> lock(state_.mutex);
    if (state_.io_fault) {
      state_.command_gate.rejectExternal();
      return false;
    }
    const auto result = state_.command_gate.accept(command, now);
    return result == CommandRejection::NONE;
  }

  bool acceptEstop(const rbt::navwire::EmergencyStop & estop)
  {
    const auto now = rbt::hardware::native::now(clock_);
    std::lock_guard<std::mutex> lock(state_.mutex);
    return state_.command_gate.accept(estop, now) == CommandRejection::NONE;
  }

  void acceptImu(const rbt::hardware::Imu & imu)
  {
    const auto now = rbt::hardware::native::now(clock_);
    try {
      const auto stamp_ns = rbt::hardware::native::decodeStamp(imu.stamp, now.clock);
      if (!rbt::hardware::native::fresh(stamp_ns, now.ns, secondsToNs(config_.imu_timeout)) ||
          imu.angular_velocity_covariance[0] == -1.0 ||
          !std::isfinite(imu.angular_velocity_z)) {
        return;
      }
      std::lock_guard<std::mutex> lock(state_.mutex);
      state_.measured_angular_z = imu.angular_velocity_z;
      state_.imu_stamp_ns = stamp_ns;
      state_.has_imu = true;
    } catch (const std::invalid_argument &) {
      // Samples from another clock epoch cannot contribute to feedback control.
    }
  }

  rbt::hardware::BaseStatus status(const rbt::time::Timestamp & now,
                                  std::uint64_t sequence) const
  {
    rbt::hardware::BaseStatus result{};
    result.stamp = rbt::hardware::native::encodeStamp(now);
    result.sequence = sequence;
    std::lock_guard<std::mutex> lock(state_.mutex);
    result.rejected_commands = state_.command_gate.rejectedCommands();
    result.io_failures = state_.io_failures;
    result.watchdog_expired = state_.watchdog_expired ? 1U : 0U;
    result.emergency_stop = state_.command_gate.emergencyStop() ? 1U : 0U;
    result.io_fault = state_.io_fault ? 1U : 0U;
    result.feedback_fresh = state_.feedback_seen &&
        rbt::hardware::native::fresh(
          state_.feedback_stamp_ns, now.ns, secondsToNs(config_.feedback_timeout)) ? 1U : 0U;
    result.left_motor_command = state_.left_motor_command;
    result.right_motor_command = state_.right_motor_command;
    return result;
  }

  std::optional<WheelTelemetry> takeTelemetry()
  {
    std::lock_guard<std::mutex> lock(state_.mutex);
    auto result = std::move(state_.latest_telemetry);
    state_.latest_telemetry.reset();
    return result;
  }

  bool threadFailed() const
  {
    std::lock_guard<std::mutex> lock(state_.mutex);
    return state_.io_thread_failed;
  }

private:
  void ioLoop(std::stop_token stop, std::promise<void> initialized)
  {
    bool reported_ready = false;
    std::unique_ptr<SerialComm> serial;
    std::unique_ptr<MotorTransport> motor;
    try {
      serial = std::make_unique<SerialComm>(config_.serial_port, config_.baudrate);
      if (!serial->open()) {
        throw std::runtime_error("failed to open serial port: " + config_.serial_port);
      }
      if (config_.protocol == "rs485") {
        motor = std::make_unique<Rs485MotorTransport>(*serial);
      } else {
        motor = std::make_unique<HatBinaryMotorTransport>(*serial);
      }
      if (!motor->configure(config_.motor_type, config_.heartbeat_ms,
                            config_.left_motor_id, config_.right_motor_id)) {
        throw std::runtime_error("failed to configure DDSM motor transport");
      }
      if (!motor->stopPair(config_.left_motor_id, config_.right_motor_id)) {
        throw std::runtime_error("could not confirm startup motor stop");
      }
      initialized.set_value();
      reported_ready = true;

      DiffDriveKinematics kinematics(config_.wheel_separation, config_.wheel_radius);
      Odometry odometry(kinematics);
      PidController angular_pid(
        config_.pid_kp, config_.pid_ki, config_.pid_kd,
        -config_.pid_output_limit, config_.pid_output_limit);
      IoFaultLatch fault_latch(
        config_.max_consecutive_io_failures, config_.io_recovery_cycles);
      const auto period = std::chrono::duration_cast<SteadyClock::duration>(
        std::chrono::duration<double>(1.0 / config_.control_rate));
      auto next_cycle = SteadyClock::now();
      std::int64_t previous_control_ns = 0;
      std::int64_t previous_odom_ns = 0;
      std::uint64_t telemetry_sequence = static_cast<std::uint64_t>(
        rbt::hardware::native::now(clock_).ns);

      while (!stop.stop_requested()) {
        next_cycle += period;
        const auto cycle_now = rbt::hardware::native::now(clock_);
        CommandSnapshot command;
        double measured_angular_z = 0.0;
        bool imu_fresh = false;
        bool io_fault = false;
        bool feedback_fresh = false;
        {
          std::lock_guard<std::mutex> lock(state_.mutex);
          command = state_.command_gate.tick(cycle_now.ns);
          io_fault = state_.io_fault;
          feedback_fresh = state_.feedback_seen && rbt::hardware::native::fresh(
            state_.feedback_stamp_ns, cycle_now.ns, secondsToNs(config_.feedback_timeout));
          const auto age = cycle_now.ns - state_.imu_stamp_ns;
          imu_fresh = state_.has_imu && age >= 0 && age <= secondsToNs(config_.imu_timeout);
          measured_angular_z = imu_fresh ? state_.measured_angular_z : 0.0;
          state_.watchdog_expired = command.watchdog_expired;
        }
        if (io_fault || !feedback_fresh) {
          command.linear_x = 0.0;
          command.angular_z = 0.0;
        }

        double control_dt = previous_control_ns == 0 ? 1.0 / config_.control_rate :
          static_cast<double>(cycle_now.ns - previous_control_ns) * 1e-9;
        if (!std::isfinite(control_dt) || control_dt <= 0.0) {
          control_dt = 1.0 / config_.control_rate;
        }
        previous_control_ns = cycle_now.ns;

        const MotorCommands commands = computeMotorCommands(
          kinematics, angular_pid, config_.motor_type,
          command.linear_x, command.angular_z, measured_angular_z,
          control_dt, config_.max_motor_cmd, imu_fresh);
        int left_command = commands.left_cmd * config_.left_motor_sign;
        int right_command = commands.right_cmd * config_.right_motor_sign;

        const auto request_stamp = rbt::hardware::native::now(clock_);
        CommandSnapshot final_command;
        bool final_io_fault = false;
        bool final_feedback_fresh = false;
        {
          std::lock_guard<std::mutex> lock(state_.mutex);
          final_command = state_.command_gate.tick(request_stamp.ns);
          final_io_fault = state_.io_fault;
          final_feedback_fresh = state_.feedback_seen && rbt::hardware::native::fresh(
            state_.feedback_stamp_ns, request_stamp.ns,
            secondsToNs(config_.feedback_timeout));
          state_.watchdog_expired = final_command.watchdog_expired;
        }
        const bool final_zero = std::abs(final_command.linear_x) <= 1e-6 &&
          std::abs(final_command.angular_z) <= 1e-6;
        if (final_command.watchdog_expired || final_command.emergency_stop || final_zero ||
            final_io_fault || !final_feedback_fresh) {
          left_command = 0;
          right_command = 0;
          angular_pid.reset();
        }
        {
          std::lock_guard<std::mutex> lock(state_.mutex);
          state_.left_motor_command = left_command;
          state_.right_motor_command = right_command;
        }

        const MotorFeedbackPair feedback = motor->exchangePair(
          config_.left_motor_id, left_command,
          config_.right_motor_id, right_command);
        const auto received_stamp = rbt::hardware::native::now(clock_);
        const auto response_ns = received_stamp.ns - request_stamp.ns;
        const MotorFeedback & left = feedback.left;
        const MotorFeedback & right = feedback.right;
        const bool motor_error = feedback.bridge_error != 0 ||
          (left.valid && left.error != 0) ||
          (right.valid && right.error != 0);
        const bool healthy = feedback.valid && left.valid && right.valid &&
          left.id == config_.left_motor_id && right.id == config_.right_motor_id &&
          feedback.bridge_error == 0 && left.error == 0 && right.error == 0 && response_ns >= 0 &&
          response_ns <= secondsToNs(config_.feedback_timeout);
        const bool fault_changed = fault_latch.observe(healthy, motor_error);
        if (fault_changed) {
          std::lock_guard<std::mutex> lock(state_.mutex);
          state_.io_fault = fault_latch.active();
          state_.command_gate.clearCommand();
        }

        {
          std::lock_guard<std::mutex> lock(state_.mutex);
          state_.io_fault = fault_latch.active();
          if (!healthy) {
            ++state_.io_failures;
            previous_odom_ns = 0;
          } else {
            state_.feedback_seen = true;
            state_.feedback_stamp_ns = received_stamp.ns;
          }
        }

        if (healthy) {
          const double speed_scale = config_.motor_type == 210 ? 0.1 : 1.0;
          const double left_rpm = static_cast<double>(left.speed * config_.left_motor_sign) * speed_scale;
          const double right_rpm = static_cast<double>(right.speed * config_.right_motor_sign) * speed_scale;
          double odom_dt = 0.0;
          if (previous_odom_ns != 0 && received_stamp.ns > previous_odom_ns &&
              received_stamp.ns - previous_odom_ns <= secondsToNs(config_.feedback_timeout)) {
            odom_dt = static_cast<double>(received_stamp.ns - previous_odom_ns) * 1e-9;
          }
          previous_odom_ns = received_stamp.ns;
          odometry.update(left_rpm, right_rpm, odom_dt);
          const OdometryData data = odometry.getOdometry();
          const Velocity measured_velocity = kinematics.wheelSpeedsToVelocity(left_rpm, right_rpm);

          WheelTelemetry telemetry;
          telemetry.wheel.stamp = rbt::hardware::native::encodeStamp(received_stamp);
          telemetry.wheel.sequence = ++telemetry_sequence;
          rbt::hardware::native::frame(telemetry.wheel.frame, config_.odom_frame);
          rbt::hardware::native::frame(telemetry.wheel.child_frame, config_.base_frame);
          telemetry.wheel.x = data.x;
          telemetry.wheel.y = data.y;
          telemetry.wheel.yaw = data.theta;
          telemetry.wheel.linear_x = measured_velocity.linear_x;
          telemetry.wheel.angular_z = measured_velocity.angular_z;
          telemetry.wheel.left_rpm = left_rpm;
          telemetry.wheel.right_rpm = right_rpm;
          telemetry.odom.stamp = rbt::hardware::native::encodeStamp<rbt::navwire::Stamp>(received_stamp);
          telemetry.odom.sequence = telemetry_sequence;
          telemetry.odom.linear_x = measured_velocity.linear_x;
          telemetry.odom.angular_z = measured_velocity.angular_z;
          std::lock_guard<std::mutex> lock(state_.mutex);
          state_.latest_telemetry = telemetry;
        }

        std::this_thread::sleep_until(next_cycle);
      }

      if (motor && serial && serial->isOpen()) {
        if (!motor->stopPair(config_.left_motor_id, config_.right_motor_id)) {
          std::cerr << "warning: could not confirm DDSM stop during shutdown\n";
        }
        serial->close();
      }
    } catch (...) {
      if (!reported_ready) {
        initialized.set_exception(std::current_exception());
      } else {
        try {
          throw;
        } catch (const std::exception & error) {
          std::cerr << "DDSM I/O thread failed: " << error.what() << '\n';
        } catch (...) {
          std::cerr << "DDSM I/O thread failed with an unknown exception\n";
        }
        std::lock_guard<std::mutex> lock(state_.mutex);
        state_.io_fault = true;
        state_.io_thread_failed = true;
        state_.command_gate.clearCommand();
      }
      if (motor && serial && serial->isOpen()) {
        (void)motor->stopPair(config_.left_motor_id, config_.right_motor_id);
        serial->close();
      }
    }
  }

  const DriverConfig & config_;
  rbt::time::HostClock & clock_;
  SharedState state_;
  std::jthread io_thread_;
};

void logInputError(const char * input, const std::exception & error)
{
  static auto last_log = SteadyClock::time_point::min();
  const auto now = SteadyClock::now();
  if (last_log != SteadyClock::time_point::min() &&
      now - last_log < std::chrono::seconds(1)) {
    return;
  }
  std::cerr << input << " input: " << error.what() << '\n';
  last_log = now;
}

}  // namespace
}  // namespace ddsm_driver

int main(int argc, char ** argv)
{
  using namespace ddsm_driver;
  try {
    const NativeOptions options = parseOptions(argc, argv);
    if (options.show_help) {
      std::cout << "Usage: rbt_base_controller [--config FILE] [--serial-port PATH] "
        "[--prefix PREFIX] [--cmd-vel-topic TOPIC] [--imu-topic TOPIC] "
        "[--estop-topic TOPIC] [--command-source ID] [--estop-source ID] "
        "[--max-linear-velocity M/S] [--max-angular-velocity RAD/S] [--duration SECONDS]\n";
      return 0;
    }
    const DriverConfig config = loadDriverConfig(options);
    auto clock = rbt::hardware::native::hostClock();
    rbt::hardware::native::installSignals();

    NativeBaseDriver driver(config, options, clock);
    driver.start();

    auto command_input = std::make_unique<rbt::hardware::native::Input<rbt::navwire::VelocityCommand>>(
      options.command_topic);
    auto estop_input = std::make_unique<rbt::hardware::native::Input<rbt::navwire::EmergencyStop>>(
      options.estop_topic);
    auto imu_input = std::make_unique<rbt::hardware::native::Input<rbt::hardware::Imu>>(
      config.imu_topic);
    auto wheel_publisher = rbt::hardware::native::publisher<rbt::hardware::WheelOdometry>(
      options.prefix + "/wheel_odom");
    auto odom_publisher = rbt::hardware::native::publisher<rbt::navwire::Odom2D>(
      options.prefix + "/odom");
    auto status_publisher = rbt::hardware::native::publisher<rbt::hardware::BaseStatus>(
      options.prefix + "/base_status");

    std::cout << "rbt_base_controller ready\n" << std::flush;

    const auto start = SteadyClock::now();
    const auto duration = std::chrono::duration<double>(options.duration_seconds);
    auto next_tick = start;
    std::uint64_t status_sequence = static_cast<std::uint64_t>(
      rbt::hardware::native::now(clock).ns);
    while (!rbt::hardware::native::interrupted) {
      if (options.duration_seconds > 0.0 && SteadyClock::now() - start >= duration) {
        break;
      }
      next_tick += std::chrono::milliseconds(10);
      try {
        command_input->poll([&driver](const rbt::navwire::VelocityCommand & command) {
          if (!driver.acceptCommand(command)) {
            // Rejected counts are surfaced on BaseStatus.
          }
        });
      } catch (const std::exception & error) {
        logInputError("command", error);
      }
      try {
        estop_input->poll([&driver](const rbt::navwire::EmergencyStop & estop) {
          if (!driver.acceptEstop(estop)) {
            // Rejected counts are surfaced on BaseStatus.
          }
        });
      } catch (const std::exception & error) {
        logInputError("estop", error);
      }
      try {
        imu_input->poll([&driver](const rbt::hardware::Imu & imu) { driver.acceptImu(imu); });
      } catch (const std::exception & error) {
        logInputError("IMU", error);
      }

      const auto now = rbt::hardware::native::now(clock);
      const auto status = driver.status(now, ++status_sequence);
      try {
        rbt::hardware::native::publish(status_publisher, status);
      } catch (const std::exception & error) {
        logInputError("base_status publish", error);
      }
      if (auto telemetry = driver.takeTelemetry()) {
        try {
          rbt::hardware::native::publish(wheel_publisher, telemetry->wheel);
        } catch (const std::exception & error) {
          logInputError("wheel_odom publish", error);
        }
        try {
          rbt::hardware::native::publish(odom_publisher, telemetry->odom);
        } catch (const std::exception & error) {
          logInputError("odom publish", error);
        }
      }
      if (driver.threadFailed()) {
        throw std::runtime_error("DDSM I/O thread stopped unexpectedly");
      }
      std::this_thread::sleep_until(next_tick);
    }
    driver.stop();
    return 0;
  } catch (const std::exception & error) {
    std::cerr << "rbt_base_controller: " << error.what() << '\n';
    return 1;
  }
}

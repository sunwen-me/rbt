#pragma once

#include <rbt_dm_imu/parser.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace rbt_dm_imu {

struct Config {
    std::string serial_port = "/dev/dm_imu";
    std::uint32_t baud = 921600U;
    std::uint32_t publish_rate_hz = 200U;
    std::string frame = "imu_link";
    std::string topic = "/imu/data";
    GyroUnit gyro_unit = GyroUnit::RadiansPerSecond;
    AccelerationUnit acceleration_unit = AccelerationUnit::MetersPerSecondSquared;
    ProtocolOptions protocol;
    bool configure_device = false;
    bool save_parameters = false;
    std::chrono::milliseconds reconnect_initial {100};
    std::chrono::milliseconds reconnect_maximum {5000};
    std::optional<std::chrono::duration<double>> duration;
};

class PublishRateGate {
  public:
    using Clock = std::chrono::steady_clock;

    PublishRateGate(std::uint32_t rate_hz, Clock::time_point phase_start);
    [[nodiscard]] bool due(Clock::time_point now);

  private:
    Clock::duration period_;
    Clock::time_point next_deadline_;
};

[[nodiscard]] Config load_config(const std::optional<std::string>& path);
void validate_config(const Config& config);
[[nodiscard]] GyroUnit parse_gyro_unit(const std::string& value);
[[nodiscard]] AccelerationUnit parse_acceleration_unit(const std::string& value);

} // namespace rbt_dm_imu

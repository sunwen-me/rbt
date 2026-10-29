#pragma once

#include <cstdint>
#include <string>

namespace ddsm_driver
{

struct NativeOptions
{
  std::string config_file;
  std::string serial_port;
  std::string prefix{"rbt/navigation/demo"};
  std::string command_topic{"/cmd_vel"};
  std::string imu_topic;
  std::string estop_topic;
  std::uint64_t command_source{100};
  std::uint64_t estop_source{2};
  double maximum_linear{0.7};
  double maximum_angular{1.8};
  double duration_seconds{0.0};
  bool show_help{false};
};

struct DriverConfig
{
  std::string serial_port{"/dev/ddsm"};
  std::uint32_t baudrate{460800};
  std::string protocol{"hat_binary"};
  int motor_type{115};
  int left_motor_id{1};
  int right_motor_id{2};
  int heartbeat_ms{1000};
  double wheel_radius{0.05};
  double wheel_separation{0.395};
  double control_rate{100.0};
  double command_timeout{0.6};
  double feedback_timeout{0.05};
  double imu_timeout{0.5};
  int max_consecutive_io_failures{5};
  int io_recovery_cycles{5};
  int left_motor_sign{1};
  int right_motor_sign{-1};
  double pid_kp{1.0};
  double pid_ki{0.0};
  double pid_kd{0.0};
  double pid_output_limit{0.5};
  int max_motor_cmd{330};
  std::string imu_topic{"/imu/data"};
  std::string odom_frame{"odom"};
  std::string base_frame{"base_footprint"};
};

NativeOptions parseOptions(int argc, char ** argv);
DriverConfig loadDriverConfig(const NativeOptions & options);

}  // namespace ddsm_driver

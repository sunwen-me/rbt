#include "ddsm_driver/native_config.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace ddsm_driver
{
namespace
{

std::string requireValue(int & index, int argc, char ** argv, const char * flag)
{
  if (index + 1 >= argc) {
    throw std::invalid_argument(std::string("missing value for ") + flag);
  }
  return argv[++index];
}

double parseDouble(const std::string & text, const char * name)
{
  std::size_t consumed = 0;
  const double value = std::stod(text, &consumed);
  if (consumed != text.size() || !std::isfinite(value)) {
    throw std::invalid_argument(std::string("invalid value for ") + name);
  }
  return value;
}

std::uint64_t parseSource(const std::string & text, const char * name)
{
  if (text.empty() || text.front() == '-') {
    throw std::invalid_argument(std::string("invalid nonzero source id for ") + name);
  }
  std::size_t consumed = 0;
  const auto value = std::stoull(text, &consumed, 0);
  if (consumed != text.size() || value == 0) {
    throw std::invalid_argument(std::string("invalid nonzero source id for ") + name);
  }
  return value;
}

template<typename T>
void readIfPresent(const YAML::Node & node, const char * key, T & value)
{
  if (node[key]) {
    value = node[key].as<T>();
  }
}

int defaultMotorLimit(int motor_type)
{
  return motor_type == 210 ? 2100 : 330;
}

void validate(const DriverConfig & config, const NativeOptions & options)
{
  if (config.serial_port.empty()) {
    throw std::invalid_argument("serial_port must not be empty");
  }
  if (config.motor_type != 115 && config.motor_type != 210) {
    throw std::invalid_argument("motor_type must be 115 or 210");
  }
  if (config.protocol != "hat_binary" && config.protocol != "rs485") {
    throw std::invalid_argument("protocol must be 'hat_binary' or 'rs485'");
  }
  if (config.protocol == "rs485" && config.motor_type != 115) {
    throw std::invalid_argument("protocol='rs485' currently supports motor_type=115 only");
  }
  const std::uint32_t expected_baudrate = config.protocol == "rs485" ? 115200U : 460800U;
  if (config.baudrate != expected_baudrate) {
    throw std::invalid_argument("baudrate must be 460800 for hat_binary or 115200 for rs485");
  }
  if (config.left_motor_id < 1 || config.left_motor_id > 253 ||
      config.right_motor_id < 1 || config.right_motor_id > 253 ||
      config.left_motor_id == config.right_motor_id) {
    throw std::invalid_argument("motor IDs must be distinct values in the range 1..253");
  }
  if (config.heartbeat_ms < -1 || config.heartbeat_ms > 65534) {
    throw std::invalid_argument("heartbeat_ms must be -1 or in the range 0..65534");
  }
  if (!std::isfinite(config.wheel_radius) || !std::isfinite(config.wheel_separation) ||
      config.wheel_radius <= 0.0 || config.wheel_separation <= 0.0) {
    throw std::invalid_argument("wheel radius and separation must be positive and finite");
  }
  if (!std::isfinite(config.control_rate) || config.control_rate <= 0.0 ||
      config.control_rate > 200.0 ||
      (config.protocol == "hat_binary" && config.control_rate > 100.0)) {
    throw std::invalid_argument("control_rate exceeds the supported protocol rate");
  }
  if (!std::isfinite(config.command_timeout) || config.command_timeout <= 0.0 ||
      !std::isfinite(config.feedback_timeout) || config.feedback_timeout <= 0.0 ||
      !std::isfinite(config.imu_timeout) || config.imu_timeout <= 0.0) {
    throw std::invalid_argument("command, feedback, and IMU timeouts must be positive");
  }
  if (config.command_timeout > 60.0 || config.feedback_timeout > 60.0 ||
      config.imu_timeout > 60.0) {
    throw std::invalid_argument("timeouts must not exceed 60 seconds");
  }
  if (config.max_consecutive_io_failures < 1 || config.io_recovery_cycles < 1) {
    throw std::invalid_argument("I/O failure and recovery cycles must be positive");
  }
  if ((config.left_motor_sign != -1 && config.left_motor_sign != 1) ||
      (config.right_motor_sign != -1 && config.right_motor_sign != 1)) {
    throw std::invalid_argument("motor signs must be -1 or 1");
  }
  if (!std::isfinite(config.pid_kp) || !std::isfinite(config.pid_ki) ||
      !std::isfinite(config.pid_kd) || !std::isfinite(config.pid_output_limit) ||
      config.pid_kp < 0.0 || config.pid_ki < 0.0 || config.pid_kd < 0.0 ||
      config.pid_output_limit <= 0.0) {
    throw std::invalid_argument("PID values must be finite and nonnegative with a positive limit");
  }
  if (config.max_motor_cmd <= 0 || config.max_motor_cmd > defaultMotorLimit(config.motor_type)) {
    throw std::invalid_argument("max_motor_cmd exceeds the selected motor protocol range");
  }
  if (!std::isfinite(options.maximum_linear) || options.maximum_linear <= 0.0 ||
      !std::isfinite(options.maximum_angular) || options.maximum_angular <= 0.0) {
    throw std::invalid_argument("velocity bounds must be positive and finite");
  }
  if (options.command_source == 0 || options.estop_source == 0 ||
      options.command_source == options.estop_source) {
    throw std::invalid_argument("command and estop sources must be distinct nonzero IDs");
  }
  if (options.prefix.empty() || options.command_topic.empty() || config.imu_topic.empty()) {
    throw std::invalid_argument("prefix, command topic, and IMU topic must not be empty");
  }
  if (config.odom_frame.empty() || config.odom_frame.size() >= 64 ||
      config.base_frame.empty() || config.base_frame.size() >= 64) {
    throw std::invalid_argument("odometry frame names must fit the native telemetry fields");
  }
  if (options.duration_seconds < 0.0 || !std::isfinite(options.duration_seconds)) {
    throw std::invalid_argument("duration must be nonnegative and finite");
  }
}

std::string resolveDefaultConfig()
{
  const std::vector<std::filesystem::path> candidates{
    "config/ddsm_robot_current.yaml",
    "drivers/base_controller/config/ddsm_robot_current.yaml",
#ifdef RBT_BASE_CONTROLLER_SOURCE_CONFIG
    RBT_BASE_CONTROLLER_SOURCE_CONFIG,
#endif
  };
  for (const auto & candidate : candidates) {
    if (std::filesystem::is_regular_file(candidate)) {
      return candidate.string();
    }
  }
  throw std::runtime_error("could not locate config/ddsm_robot_current.yaml; pass --config FILE");
}

}  // namespace

NativeOptions parseOptions(int argc, char ** argv)
{
  NativeOptions options;
  for (int index = 1; index < argc; ++index) {
    const std::string_view arg(argv[index]);
    if (arg == "--help" || arg == "-h") {
      options.show_help = true;
    } else if (arg == "--config") {
      options.config_file = requireValue(index, argc, argv, "--config");
    } else if (arg == "--serial-port") {
      options.serial_port = requireValue(index, argc, argv, "--serial-port");
    } else if (arg == "--prefix") {
      options.prefix = requireValue(index, argc, argv, "--prefix");
    } else if (arg == "--cmd-vel-topic") {
      options.command_topic = requireValue(index, argc, argv, "--cmd-vel-topic");
    } else if (arg == "--imu-topic") {
      options.imu_topic = requireValue(index, argc, argv, "--imu-topic");
    } else if (arg == "--estop-topic") {
      options.estop_topic = requireValue(index, argc, argv, "--estop-topic");
    } else if (arg == "--command-source" || arg == "--guard-source") {
      const char * flag = arg == "--command-source" ? "--command-source" : "--guard-source";
      options.command_source = parseSource(requireValue(index, argc, argv, flag), "command-source");
    } else if (arg == "--estop-source") {
      options.estop_source = parseSource(requireValue(index, argc, argv, "--estop-source"), "estop-source");
    } else if (arg == "--max-linear-velocity") {
      options.maximum_linear = parseDouble(requireValue(index, argc, argv, "--max-linear-velocity"), "max-linear-velocity");
    } else if (arg == "--max-angular-velocity") {
      options.maximum_angular = parseDouble(requireValue(index, argc, argv, "--max-angular-velocity"), "max-angular-velocity");
    } else if (arg == "--duration") {
      options.duration_seconds = parseDouble(requireValue(index, argc, argv, "--duration"), "duration");
    } else {
      throw std::invalid_argument("unknown argument: " + std::string(arg));
    }
  }
  if (options.show_help) {
    return options;
  }
  if (options.estop_topic.empty()) {
    options.estop_topic = options.prefix + "/estop";
  }
  return options;
}

DriverConfig loadDriverConfig(const NativeOptions & options)
{
  DriverConfig config;
  const std::string path = options.config_file.empty() ? resolveDefaultConfig() : options.config_file;
  const YAML::Node root = YAML::LoadFile(path);
  const YAML::Node parameters = root["ddsm_driver_node"]["ros__parameters"];
  if (!parameters || !parameters.IsMap()) {
    throw std::invalid_argument("config must contain ddsm_driver_node.ros__parameters");
  }

  readIfPresent(parameters, "serial_port", config.serial_port);
  std::int64_t baudrate = config.baudrate;
  readIfPresent(parameters, "baudrate", baudrate);
  if (baudrate <= 0 || baudrate > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("baudrate is outside its supported range");
  }
  config.baudrate = static_cast<std::uint32_t>(baudrate);
  readIfPresent(parameters, "protocol", config.protocol);
  readIfPresent(parameters, "motor_type", config.motor_type);
  readIfPresent(parameters, "left_motor_id", config.left_motor_id);
  readIfPresent(parameters, "right_motor_id", config.right_motor_id);
  readIfPresent(parameters, "heartbeat_ms", config.heartbeat_ms);
  readIfPresent(parameters, "wheel_radius", config.wheel_radius);
  readIfPresent(parameters, "wheel_separation", config.wheel_separation);
  readIfPresent(parameters, "control_rate", config.control_rate);
  readIfPresent(parameters, "cmd_vel_timeout", config.command_timeout);
  readIfPresent(parameters, "feedback_timeout", config.feedback_timeout);
  readIfPresent(parameters, "imu_timeout", config.imu_timeout);
  readIfPresent(parameters, "max_consecutive_io_failures", config.max_consecutive_io_failures);
  readIfPresent(parameters, "io_recovery_cycles", config.io_recovery_cycles);
  readIfPresent(parameters, "left_motor_sign", config.left_motor_sign);
  readIfPresent(parameters, "right_motor_sign", config.right_motor_sign);
  readIfPresent(parameters, "pid_kp", config.pid_kp);
  readIfPresent(parameters, "pid_ki", config.pid_ki);
  readIfPresent(parameters, "pid_kd", config.pid_kd);
  readIfPresent(parameters, "pid_output_limit", config.pid_output_limit);
  readIfPresent(parameters, "max_motor_cmd", config.max_motor_cmd);
  readIfPresent(parameters, "imu_topic", config.imu_topic);
  readIfPresent(parameters, "odom_frame", config.odom_frame);
  readIfPresent(parameters, "base_frame", config.base_frame);

  bool use_imu_orientation = false;
  readIfPresent(parameters, "use_imu_orientation", use_imu_orientation);
  if (use_imu_orientation) {
    throw std::invalid_argument("use_imu_orientation=true is unsupported by the native driver");
  }
  if (config.max_motor_cmd == 0) {
    config.max_motor_cmd = defaultMotorLimit(config.motor_type);
  }
  if (!options.serial_port.empty()) {
    config.serial_port = options.serial_port;
  }
  if (!options.imu_topic.empty()) {
    config.imu_topic = options.imu_topic;
  }
  validate(config, options);
  return config;
}

}  // namespace ddsm_driver

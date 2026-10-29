#include "ddsm_driver/ddsm_control.hpp"

#include <algorithm>
#include <cmath>

#include "ddsm_driver/ddsm_protocol.hpp"

namespace ddsm_driver
{
MotorCommands computeMotorCommands(
  const DiffDriveKinematics & kinematics,
  PidController & angular_pid,
  int motor_type,
  double target_linear_x,
  double target_angular_z,
  double measured_angular_z,
  double dt,
  int max_motor_cmd,
  bool use_angular_feedback)
{
  if (!std::isfinite(target_linear_x) || !std::isfinite(target_angular_z) ||
    !std::isfinite(measured_angular_z) || !std::isfinite(dt))
  {
    angular_pid.reset();
    return MotorCommands{};
  }

  if (std::abs(target_linear_x) <= 1e-6 && std::abs(target_angular_z) <= 1e-6) {
    angular_pid.reset();
    return MotorCommands{};
  }

  double effective_angular_z = target_angular_z;
  if (use_angular_feedback) {
    effective_angular_z += angular_pid.compute(target_angular_z, measured_angular_z, dt);
  } else {
    angular_pid.reset();
  }

  const WheelSpeeds wheel_speeds =
    kinematics.velocityToWheelSpeeds(target_linear_x, effective_angular_z);

  MotorCommands commands;
  commands.left_cmd = rpmToMotorCommand(motor_type, wheel_speeds.left_rpm);
  commands.right_cmd = rpmToMotorCommand(motor_type, wheel_speeds.right_rpm);
  if (max_motor_cmd > 0) {
    commands.left_cmd = std::clamp(commands.left_cmd, -max_motor_cmd, max_motor_cmd);
    commands.right_cmd = std::clamp(commands.right_cmd, -max_motor_cmd, max_motor_cmd);
  }
  return commands;
}

}  // namespace ddsm_driver

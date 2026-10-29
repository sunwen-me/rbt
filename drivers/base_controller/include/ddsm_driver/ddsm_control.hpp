#ifndef DDSM_DRIVER__DDSM_CONTROL_HPP_
#define DDSM_DRIVER__DDSM_CONTROL_HPP_

#include "ddsm_driver/diff_drive_kinematics.hpp"
#include "ddsm_driver/pid_controller.hpp"

namespace ddsm_driver
{

struct MotorCommands
{
  int left_cmd{0};
  int right_cmd{0};
};

MotorCommands computeMotorCommands(
  const DiffDriveKinematics & kinematics,
  PidController & angular_pid,
  int motor_type,
  double target_linear_x,
  double target_angular_z,
  double measured_angular_z,
  double dt,
  int max_motor_cmd = 0,
  bool use_angular_feedback = true);

}  // namespace ddsm_driver

#endif  // DDSM_DRIVER__DDSM_CONTROL_HPP_

#include "ddsm_driver/diff_drive_kinematics.hpp"

namespace ddsm_driver
{

DiffDriveKinematics::DiffDriveKinematics(double wheel_separation, double wheel_radius)
: wheel_separation_(wheel_separation), wheel_radius_(wheel_radius)
{
}

WheelSpeeds DiffDriveKinematics::velocityToWheelSpeeds(double linear_x, double angular_z) const
{
  WheelSpeeds speeds;

  double v_left = linear_x - angular_z * wheel_separation_ / 2.0;
  double v_right = linear_x + angular_z * wheel_separation_ / 2.0;

  speeds.left_rpm = v_left / (2.0 * M_PI * wheel_radius_) * 60.0;
  speeds.right_rpm = v_right / (2.0 * M_PI * wheel_radius_) * 60.0;

  return speeds;
}

Velocity DiffDriveKinematics::wheelSpeedsToVelocity(double left_rpm, double right_rpm) const
{
  Velocity vel;

  double v_left = left_rpm * (2.0 * M_PI * wheel_radius_) / 60.0;
  double v_right = right_rpm * (2.0 * M_PI * wheel_radius_) / 60.0;

  vel.linear_x = (v_right + v_left) / 2.0;
  vel.angular_z = (v_right - v_left) / wheel_separation_;

  return vel;
}

}  // namespace ddsm_driver

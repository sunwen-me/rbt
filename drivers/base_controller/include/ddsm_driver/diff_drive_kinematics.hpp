#ifndef DDSM_DRIVER__DIFF_DRIVE_KINEMATICS_HPP_
#define DDSM_DRIVER__DIFF_DRIVE_KINEMATICS_HPP_

#include <cmath>

namespace ddsm_driver
{

struct WheelSpeeds
{
  double left_rpm{0.0};
  double right_rpm{0.0};
};

struct Velocity
{
  double linear_x{0.0};
  double angular_z{0.0};
};

class DiffDriveKinematics
{
public:
  DiffDriveKinematics(double wheel_separation, double wheel_radius);

  WheelSpeeds velocityToWheelSpeeds(double linear_x, double angular_z) const;
  Velocity wheelSpeedsToVelocity(double left_rpm, double right_rpm) const;

  double getWheelSeparation() const { return wheel_separation_; }
  double getWheelRadius() const { return wheel_radius_; }

private:
  double wheel_separation_;
  double wheel_radius_;
};

}  // namespace ddsm_driver

#endif  // DDSM_DRIVER__DIFF_DRIVE_KINEMATICS_HPP_

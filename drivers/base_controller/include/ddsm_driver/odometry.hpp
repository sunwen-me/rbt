#ifndef DDSM_DRIVER__ODOMETRY_HPP_
#define DDSM_DRIVER__ODOMETRY_HPP_

#include <cmath>
#include <mutex>

#include "ddsm_driver/diff_drive_kinematics.hpp"

namespace ddsm_driver
{

struct OdometryData
{
  double x{0.0};
  double y{0.0};
  double theta{0.0};
  double vx{0.0};
  double vy{0.0};
  double vth{0.0};
};

class Odometry
{
public:
  explicit Odometry(const DiffDriveKinematics & kinematics);

  void update(double left_rpm, double right_rpm, double dt);
  OdometryData getOdometry() const;
  void setImuYaw(double yaw);
  void reset();

private:
  static double normalizeAngle(double angle);

  const DiffDriveKinematics & kinematics_;

  mutable std::mutex mutex_;
  OdometryData data_;
  double imu_yaw_{0.0};
  bool imu_yaw_initialized_{false};
};

}  // namespace ddsm_driver

#endif  // DDSM_DRIVER__ODOMETRY_HPP_

#include "ddsm_driver/odometry.hpp"

namespace ddsm_driver
{

Odometry::Odometry(const DiffDriveKinematics & kinematics)
: kinematics_(kinematics)
{
}

void Odometry::update(double left_rpm, double right_rpm, double dt)
{
  if (dt <= 0.0) {
    return;
  }

  Velocity vel = kinematics_.wheelSpeedsToVelocity(left_rpm, right_rpm);

  std::lock_guard<std::mutex> lock(mutex_);

  data_.vx = vel.linear_x;
  data_.vy = 0.0;
  data_.vth = vel.angular_z;

  double theta = imu_yaw_initialized_ ? imu_yaw_ : data_.theta;

  double dx = data_.vx * std::cos(theta) * dt;
  double dy = data_.vx * std::sin(theta) * dt;

  data_.x += dx;
  data_.y += dy;

  if (!imu_yaw_initialized_) {
    data_.theta += data_.vth * dt;
    data_.theta = normalizeAngle(data_.theta);
  } else {
    data_.theta = imu_yaw_;
  }
}

OdometryData Odometry::getOdometry() const
{
  std::lock_guard<std::mutex> lock(mutex_);
  return data_;
}

void Odometry::setImuYaw(double yaw)
{
  std::lock_guard<std::mutex> lock(mutex_);
  imu_yaw_ = yaw;
  imu_yaw_initialized_ = true;
  data_.theta = yaw;
}

void Odometry::reset()
{
  std::lock_guard<std::mutex> lock(mutex_);
  data_ = OdometryData{};
  imu_yaw_initialized_ = false;
}

double Odometry::normalizeAngle(double angle)
{
  angle = std::fmod(angle + M_PI, 2.0 * M_PI);
  if (angle < 0.0) {
    angle += 2.0 * M_PI;
  }
  return angle - M_PI;
}

}  // namespace ddsm_driver

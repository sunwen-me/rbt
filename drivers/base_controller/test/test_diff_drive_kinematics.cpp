#include <cmath>
#include <iostream>

#include "ddsm_driver/diff_drive_kinematics.hpp"
#include "test_common.hpp"

using namespace ddsm_driver;

constexpr double EPS = 1e-6;

bool approx_equal(double a, double b, double eps = EPS)
{
  return std::abs(a - b) < eps;
}

void test_velocity_to_wheel_speeds()
{
  // Test with default parameters (wheel_radius=0.05, wheel_separation=0.3)
  DiffDriveKinematics kin(0.3, 0.05);

  // Pure forward motion: both wheels should have same speed
  auto speeds = kin.velocityToWheelSpeeds(0.1, 0.0);
  DDSM_CHECK(approx_equal(speeds.left_rpm, speeds.right_rpm));
  // v = 0.1 m/s, circumference = 2*pi*0.05 = 0.314159...
  // rpm = 0.1 / 0.314159 * 60 = 19.0986...
  DDSM_CHECK(approx_equal(speeds.left_rpm, 19.0986, 0.01));

  // Pure rotation: wheels should have opposite speeds
  speeds = kin.velocityToWheelSpeeds(0.0, 1.0);
  DDSM_CHECK(approx_equal(speeds.left_rpm, -speeds.right_rpm));

  // Combined motion
  speeds = kin.velocityToWheelSpeeds(0.1, 0.5);
  DDSM_CHECK(speeds.right_rpm > speeds.left_rpm);  // Turning left

  std::cout << "  PASS: test_velocity_to_wheel_speeds\n";
}

void test_wheel_speeds_to_velocity()
{
  DiffDriveKinematics kin(0.3, 0.05);

  // Equal speeds -> pure forward
  auto vel = kin.wheelSpeedsToVelocity(100.0, 100.0);
  DDSM_CHECK(approx_equal(vel.angular_z, 0.0));
  DDSM_CHECK(vel.linear_x > 0.0);

  // Opposite speeds -> pure rotation
  vel = kin.wheelSpeedsToVelocity(-100.0, 100.0);
  DDSM_CHECK(approx_equal(vel.linear_x, 0.0));
  DDSM_CHECK(vel.angular_z > 0.0);

  std::cout << "  PASS: test_wheel_speeds_to_velocity\n";
}

void test_roundtrip_conversion()
{
  DiffDriveKinematics kin(0.3, 0.05);

  // Convert velocity -> wheel speeds -> velocity should be identity
  double linear_x = 0.15;
  double angular_z = 0.8;

  auto speeds = kin.velocityToWheelSpeeds(linear_x, angular_z);
  auto vel = kin.wheelSpeedsToVelocity(speeds.left_rpm, speeds.right_rpm);

  DDSM_CHECK(approx_equal(vel.linear_x, linear_x, 0.001));
  DDSM_CHECK(approx_equal(vel.angular_z, angular_z, 0.001));

  std::cout << "  PASS: test_roundtrip_conversion\n";
}

void test_zero_velocity()
{
  DiffDriveKinematics kin(0.3, 0.05);

  auto speeds = kin.velocityToWheelSpeeds(0.0, 0.0);
  DDSM_CHECK(approx_equal(speeds.left_rpm, 0.0));
  DDSM_CHECK(approx_equal(speeds.right_rpm, 0.0));

  auto vel = kin.wheelSpeedsToVelocity(0.0, 0.0);
  DDSM_CHECK(approx_equal(vel.linear_x, 0.0));
  DDSM_CHECK(approx_equal(vel.angular_z, 0.0));

  std::cout << "  PASS: test_zero_velocity\n";
}

int main()
{
  std::cout << "Running DiffDriveKinematics tests...\n";
  test_velocity_to_wheel_speeds();
  test_wheel_speeds_to_velocity();
  test_roundtrip_conversion();
  test_zero_velocity();
  std::cout << "All tests passed!\n";
  return 0;
}

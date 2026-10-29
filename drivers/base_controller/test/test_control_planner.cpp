#include <iostream>

#include "ddsm_driver/ddsm_control.hpp"
#include "ddsm_driver/diff_drive_kinematics.hpp"
#include "ddsm_driver/pid_controller.hpp"
#include "test_common.hpp"

using namespace ddsm_driver;

void test_closed_loop_bias_creates_asymmetric_wheel_commands()
{
  DiffDriveKinematics kinematics(0.3, 0.05);
  PidController pid(1.0, 0.0, 0.0, -100.0, 100.0);

  const int motor_type = 115;
  const double target_linear_x = 0.1;
  const double target_angular_z = 0.5;
  const double dt = 0.1;

  const MotorCommands baseline = computeMotorCommands(
    kinematics, pid, motor_type, target_linear_x, target_angular_z, target_angular_z, dt);

  PidController pid_biased(1.0, 0.0, 0.0, -100.0, 100.0);
  const MotorCommands biased = computeMotorCommands(
    kinematics, pid_biased, motor_type, target_linear_x, target_angular_z, target_angular_z - 0.2, dt);

  DDSM_CHECK(biased.left_cmd != baseline.left_cmd || biased.right_cmd != baseline.right_cmd);
}

void test_closed_loop_matches_open_loop_when_imu_matches_target()
{
  DiffDriveKinematics kinematics(0.3, 0.05);
  PidController pid(1.0, 0.0, 0.0, -100.0, 100.0);

  const int motor_type = 115;
  const double target_linear_x = 0.1;
  const double target_angular_z = 0.4;
  const double dt = 0.1;

  const MotorCommands commands = computeMotorCommands(
    kinematics, pid, motor_type, target_linear_x, target_angular_z, target_angular_z, dt);

  DDSM_CHECK(commands.left_cmd == 8);
  DDSM_CHECK(commands.right_cmd == 31);
}

void test_zero_command_stays_zero_even_with_imu_bias()
{
  DiffDriveKinematics kinematics(0.3, 0.05);
  PidController pid(1.0, 0.0, 0.0, -100.0, 100.0);

  const MotorCommands commands = computeMotorCommands(
    kinematics, pid, 115, 0.0, 0.0, 0.05, 0.1);

  DDSM_CHECK(commands.left_cmd == 0);
  DDSM_CHECK(commands.right_cmd == 0);
}

void test_motor_commands_are_limited()
{
  DiffDriveKinematics kinematics(0.3, 0.05);
  PidController pid(1.0, 0.0, 0.0, -100.0, 100.0);

  const MotorCommands commands = computeMotorCommands(
    kinematics, pid, 115, 10.0, 20.0, 0.0, 0.1, 120);

  DDSM_CHECK(commands.left_cmd >= -120);
  DDSM_CHECK(commands.left_cmd <= 120);
  DDSM_CHECK(commands.right_cmd >= -120);
  DDSM_CHECK(commands.right_cmd <= 120);
}

void test_open_loop_resets_pid_state()
{
  DiffDriveKinematics kinematics(0.3, 0.05);
  PidController pid(0.0, 10.0, 0.0, -100.0, 100.0);

  (void)computeMotorCommands(
    kinematics, pid, 115, 0.1, 0.5, 0.0, 0.1, 330, true);
  const MotorCommands open_loop = computeMotorCommands(
    kinematics, pid, 115, 0.1, 0.5, 0.0, 0.1, 330, false);

  PidController fresh_pid(0.0, 10.0, 0.0, -100.0, 100.0);
  const MotorCommands expected = computeMotorCommands(
    kinematics, fresh_pid, 115, 0.1, 0.5, 0.0, 0.1, 330, false);
  DDSM_CHECK(open_loop.left_cmd == expected.left_cmd);
  DDSM_CHECK(open_loop.right_cmd == expected.right_cmd);
}

int main()
{
  test_closed_loop_bias_creates_asymmetric_wheel_commands();
  test_closed_loop_matches_open_loop_when_imu_matches_target();
  test_zero_command_stays_zero_even_with_imu_bias();
  test_motor_commands_are_limited();
  test_open_loop_resets_pid_state();
  std::cout << "All control planner tests passed\n";
  return 0;
}

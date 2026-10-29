#include <cmath>
#include <iostream>

#include "ddsm_driver/pid_controller.hpp"
#include "test_common.hpp"

using namespace ddsm_driver;

constexpr double EPS = 1e-6;

bool approx_equal(double a, double b, double eps = EPS)
{
  return std::abs(a - b) < eps;
}

void test_proportional_only()
{
  PidController pid(2.0, 0.0, 0.0, -100.0, 100.0);

  // Error = 1.0, dt = 0.1
  // Output = kp * error = 2.0 * 1.0 = 2.0
  double output = pid.compute(1.0, 0.0, 0.1);
  DDSM_CHECK(approx_equal(output, 2.0));

  // Error = -0.5
  // Output = 2.0 * (-0.5) = -1.0
  output = pid.compute(-0.5, 0.0, 0.1);
  DDSM_CHECK(approx_equal(output, -1.0));

  std::cout << "  PASS: test_proportional_only\n";
}

void test_integral_accumulation()
{
  PidController pid(0.0, 1.0, 0.0, -100.0, 100.0);

  // After first call: integral = 1.0 * 0.1 = 0.1, output = 0.1
  double output = pid.compute(1.0, 0.0, 0.1);
  DDSM_CHECK(approx_equal(output, 0.1));

  // After second call: integral = 0.1 + 1.0 * 0.1 = 0.2, output = 0.2
  output = pid.compute(1.0, 0.0, 0.1);
  DDSM_CHECK(approx_equal(output, 0.2));

  std::cout << "  PASS: test_integral_accumulation\n";
}

void test_derivative()
{
  PidController pid(0.0, 0.0, 1.0, -100.0, 100.0);

  // First call: no previous error, so derivative = 0
  double output = pid.compute(1.0, 0.0, 0.1);
  DDSM_CHECK(approx_equal(output, 0.0));

  // Second call: error changed from 1.0 to 2.0, derivative = (2.0 - 1.0) / 0.1 = 10.0
  output = pid.compute(2.0, 0.0, 0.1);
  DDSM_CHECK(approx_equal(output, 10.0));

  std::cout << "  PASS: test_derivative\n";
}

void test_output_clamping()
{
  PidController pid(100.0, 0.0, 0.0, -50.0, 50.0);

  // Error = 1.0, kp = 100, output = 100, but clamped to 50
  double output = pid.compute(1.0, 0.0, 0.1);
  DDSM_CHECK(approx_equal(output, 50.0));

  // Error = -1.0, output = -100, clamped to -50
  output = pid.compute(-1.0, 0.0, 0.1);
  DDSM_CHECK(approx_equal(output, -50.0));

  std::cout << "  PASS: test_output_clamping\n";
}

void test_reset()
{
  PidController pid(0.0, 1.0, 0.0, -100.0, 100.0);

  // Accumulate some integral
  pid.compute(1.0, 0.0, 0.1);
  pid.compute(1.0, 0.0, 0.1);

  // Reset and check that integral is cleared
  pid.reset();
  double output = pid.compute(1.0, 0.0, 0.1);
  DDSM_CHECK(approx_equal(output, 0.1));  // Only one step of integration

  std::cout << "  PASS: test_reset\n";
}

void test_zero_dt()
{
  PidController pid(1.0, 1.0, 1.0, -100.0, 100.0);

  // Zero dt should return 0
  double output = pid.compute(1.0, 0.0, 0.0);
  DDSM_CHECK(approx_equal(output, 0.0));

  std::cout << "  PASS: test_zero_dt\n";
}

int main()
{
  std::cout << "Running PidController tests...\n";
  test_proportional_only();
  test_integral_accumulation();
  test_derivative();
  test_output_clamping();
  test_reset();
  test_zero_dt();
  std::cout << "All tests passed!\n";
  return 0;
}

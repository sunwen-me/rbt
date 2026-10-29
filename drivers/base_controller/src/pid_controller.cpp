#include "ddsm_driver/pid_controller.hpp"

#include <algorithm>

namespace ddsm_driver
{

PidController::PidController(double kp, double ki, double kd, double output_min, double output_max)
: gains_{kp, ki, kd},
  output_min_(output_min),
  output_max_(output_max),
  integral_min_(output_min),
  integral_max_(output_max)
{
}

double PidController::compute(double setpoint, double measurement, double dt)
{
  if (dt <= 0.0) {
    return 0.0;
  }

  double error = setpoint - measurement;

  // Proportional
  double p_term = gains_.kp * error;

  // Integral with clamping (anti-windup)
  integral_ += error * dt;
  integral_ = std::clamp(integral_, integral_min_, integral_max_);
  double i_term = gains_.ki * integral_;

  // Derivative (on error)
  double d_term = 0.0;
  if (has_prev_error_) {
    d_term = gains_.kd * (error - prev_error_) / dt;
  }
  prev_error_ = error;
  has_prev_error_ = true;

  // Output with clamping
  double output = std::clamp(p_term + i_term + d_term, output_min_, output_max_);

  return output;
}

void PidController::reset()
{
  integral_ = 0.0;
  prev_error_ = 0.0;
  has_prev_error_ = false;
}

void PidController::setGains(double kp, double ki, double kd)
{
  gains_ = {kp, ki, kd};
}

void PidController::setOutputLimits(double min, double max)
{
  output_min_ = min;
  output_max_ = max;
}

void PidController::setIntegralLimits(double min, double max)
{
  integral_min_ = min;
  integral_max_ = max;
}

}  // namespace ddsm_driver

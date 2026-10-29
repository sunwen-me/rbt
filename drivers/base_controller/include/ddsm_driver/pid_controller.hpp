#ifndef DDSM_DRIVER__PID_CONTROLLER_HPP_
#define DDSM_DRIVER__PID_CONTROLLER_HPP_

namespace ddsm_driver
{

struct PidGains
{
  double kp{0.0};
  double ki{0.0};
  double kd{0.0};
};

class PidController
{
public:
  PidController(double kp, double ki, double kd, double output_min, double output_max);

  double compute(double setpoint, double measurement, double dt);
  void reset();

  void setGains(double kp, double ki, double kd);
  void setOutputLimits(double min, double max);
  void setIntegralLimits(double min, double max);

  PidGains getGains() const { return gains_; }

private:
  PidGains gains_;
  double output_min_;
  double output_max_;
  double integral_min_;
  double integral_max_;

  double integral_{0.0};
  double prev_error_{0.0};
  bool has_prev_error_{false};
};

}  // namespace ddsm_driver

#endif  // DDSM_DRIVER__PID_CONTROLLER_HPP_

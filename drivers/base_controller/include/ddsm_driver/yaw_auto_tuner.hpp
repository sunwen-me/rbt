#ifndef DDSM_DRIVER__YAW_AUTO_TUNER_HPP_
#define DDSM_DRIVER__YAW_AUTO_TUNER_HPP_

#include <string>
#include <vector>

namespace ddsm_driver
{

struct YawResponseSample
{
  double time{0.0};
  double angular_velocity{0.0};
};

struct YawAutoTuneConfig
{
  double test_angular_velocity{0.35};
  double precheck_duration{1.0};
  double pulse_duration{2.5};
  double rest_duration{1.0};
  double validation_duration{2.0};
  double max_duration{30.0};
  double stationary_threshold{0.08};
  double min_plant_gain{0.2};
  double max_plant_gain{2.0};
  double lambda_factor{2.0};
  double min_integral_time{1.0};
  double max_normalized_rmse{0.35};
  double max_overshoot_ratio{1.5};
  double min_kp{0.05};
  double max_kp{1.5};
  double min_ki{0.0};
  double max_ki{0.5};
  double min_output_limit{0.3};
  double max_output_limit{1.0};
};

struct YawTuneResult
{
  bool valid{false};
  double kp{0.0};
  double ki{0.0};
  double kd{0.0};
  double output_limit{0.0};
  double plant_gain{0.0};
  double time_constant{0.0};
  double dead_time{0.0};
  double normalized_rmse{0.0};
  double overshoot_ratio{0.0};
  std::string message;
};

enum class YawAutoTuneState
{
  IDLE,
  PRECHECK,
  IDENTIFY_POSITIVE,
  REST_AFTER_POSITIVE,
  IDENTIFY_NEGATIVE,
  REST_AFTER_NEGATIVE,
  VALIDATE_POSITIVE,
  REST_AFTER_VALIDATION_POSITIVE,
  VALIDATE_NEGATIVE,
  FINAL_STOP,
  SUCCEEDED,
  FAILED,
  CANCELLED,
};

struct YawAutoTuneUpdate
{
  bool active{false};
  bool use_pid{false};
  bool apply_candidate{false};
  bool finished{false};
  bool succeeded{false};
  double angular_command{0.0};
  YawTuneResult result;
  std::string status;
};

class YawAutoTuner
{
public:
  bool start(double now_seconds, const YawAutoTuneConfig & config, std::string & reason);
  YawAutoTuneUpdate update(
    double now_seconds,
    double measured_angular_velocity,
    bool imu_fresh,
    bool motor_feedback_fresh,
    bool io_fault);
  YawAutoTuneUpdate cancel();

  bool active() const;
  bool lastRunSucceeded() const;
  YawAutoTuneState state() const;
  const YawTuneResult & result() const;
  std::string status() const;

  static YawTuneResult identify(
    const std::vector<YawResponseSample> & positive,
    const std::vector<YawResponseSample> & negative,
    const YawAutoTuneConfig & config);

private:
  void transition(YawAutoTuneState next, double now_seconds);
  YawAutoTuneUpdate fail(const std::string & reason);
  YawAutoTuneUpdate makeUpdate() const;
  bool validateConfig(const YawAutoTuneConfig & config, std::string & reason) const;
  bool validateCandidate();

  YawAutoTuneConfig config_;
  YawAutoTuneState state_{YawAutoTuneState::IDLE};
  YawTuneResult result_;
  double run_start_time_{0.0};
  double state_start_time_{0.0};
  double baseline_sum_{0.0};
  std::size_t baseline_count_{0};
  double baseline_{0.0};
  bool active_{false};
  bool last_run_succeeded_{false};
  bool candidate_event_pending_{false};
  bool finish_event_pending_{false};
  std::vector<YawResponseSample> positive_samples_;
  std::vector<YawResponseSample> negative_samples_;
  std::vector<YawResponseSample> validation_positive_samples_;
  std::vector<YawResponseSample> validation_negative_samples_;
};

const char * toString(YawAutoTuneState state);

}  // namespace ddsm_driver

#endif  // DDSM_DRIVER__YAW_AUTO_TUNER_HPP_

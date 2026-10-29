#include "ddsm_driver/yaw_auto_tuner.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <sstream>

namespace ddsm_driver
{
namespace
{

struct StepModel
{
  bool valid{false};
  double gain{0.0};
  double time_constant{0.0};
  double dead_time{0.0};
  std::string message;
};

double meanTail(const std::vector<YawResponseSample> & samples, double fraction)
{
  const std::size_t start = static_cast<std::size_t>(
    static_cast<double>(samples.size()) * std::clamp(1.0 - fraction, 0.0, 1.0));
  double sum = 0.0;
  for (std::size_t i = std::min(start, samples.size() - 1); i < samples.size(); ++i) {
    sum += samples[i].angular_velocity;
  }
  return sum / static_cast<double>(samples.size() - std::min(start, samples.size() - 1));
}

double firstCrossingTime(
  const std::vector<YawResponseSample> & samples,
  double target,
  double fraction)
{
  const double threshold = std::abs(target) * fraction;
  const double direction = target >= 0.0 ? 1.0 : -1.0;
  for (const auto & sample : samples) {
    if (direction * sample.angular_velocity >= threshold) {
      return sample.time;
    }
  }
  return std::numeric_limits<double>::quiet_NaN();
}

StepModel identifyStep(
  const std::vector<YawResponseSample> & samples,
  double command,
  const YawAutoTuneConfig & config)
{
  StepModel model;
  if (samples.size() < 20 || !std::isfinite(command) || std::abs(command) < 1e-6) {
    model.message = "not enough response samples";
    return model;
  }

  const double steady = meanTail(samples, 0.25);
  model.gain = steady / command;
  if (!std::isfinite(model.gain) ||
    model.gain < config.min_plant_gain ||
    model.gain > config.max_plant_gain)
  {
    model.message = "plant gain is outside the safe range or has the wrong sign";
    return model;
  }

  const double dead_crossing = firstCrossingTime(samples, steady, 0.05);
  const double tau_crossing = firstCrossingTime(samples, steady, 0.632);
  if (!std::isfinite(dead_crossing) || !std::isfinite(tau_crossing) ||
    tau_crossing <= dead_crossing)
  {
    model.message = "could not identify response delay and time constant";
    return model;
  }

  model.dead_time = std::max(0.0, dead_crossing);
  model.time_constant = tau_crossing - dead_crossing;
  if (model.time_constant < 0.01 || model.time_constant > config.pulse_duration) {
    model.message = "identified time constant is outside the safe range";
    return model;
  }

  model.valid = true;
  return model;
}

struct ValidationMetrics
{
  bool valid{false};
  double normalized_rmse{0.0};
  double overshoot_ratio{0.0};
};

ValidationMetrics validationMetrics(
  const std::vector<YawResponseSample> & samples,
  double command)
{
  ValidationMetrics metrics;
  if (samples.size() < 20 || std::abs(command) < 1e-6) {
    return metrics;
  }

  const std::size_t start = samples.size() / 2;
  double squared_error_sum = 0.0;
  double peak = 0.0;
  for (std::size_t i = start; i < samples.size(); ++i) {
    const double measured = samples[i].angular_velocity;
    const double error = command - measured;
    squared_error_sum += error * error;
    peak = std::max(peak, std::abs(measured));
  }

  const double count = static_cast<double>(samples.size() - start);
  metrics.normalized_rmse =
    std::sqrt(squared_error_sum / count) / std::abs(command);
  metrics.overshoot_ratio = peak / std::abs(command);
  metrics.valid =
    std::isfinite(metrics.normalized_rmse) && std::isfinite(metrics.overshoot_ratio);
  return metrics;
}

}  // namespace

bool YawAutoTuner::start(
  double now_seconds,
  const YawAutoTuneConfig & config,
  std::string & reason)
{
  if (active_) {
    reason = "yaw auto-tune is already active";
    return false;
  }
  if (!validateConfig(config, reason)) {
    return false;
  }

  config_ = config;
  result_ = YawTuneResult{};
  baseline_sum_ = 0.0;
  baseline_count_ = 0;
  baseline_ = 0.0;
  positive_samples_.clear();
  negative_samples_.clear();
  validation_positive_samples_.clear();
  validation_negative_samples_.clear();
  positive_samples_.reserve(512);
  negative_samples_.reserve(512);
  validation_positive_samples_.reserve(512);
  validation_negative_samples_.reserve(512);
  run_start_time_ = now_seconds;
  state_start_time_ = now_seconds;
  active_ = true;
  last_run_succeeded_ = false;
  candidate_event_pending_ = false;
  finish_event_pending_ = false;
  state_ = YawAutoTuneState::PRECHECK;
  reason = "yaw auto-tune started";
  return true;
}

YawAutoTuneUpdate YawAutoTuner::update(
  double now_seconds,
  double measured_angular_velocity,
  bool imu_fresh,
  bool motor_feedback_fresh,
  bool io_fault)
{
  if (!active_) {
    auto output = makeUpdate();
    if (finish_event_pending_) {
      output.finished = true;
      output.succeeded = last_run_succeeded_;
      finish_event_pending_ = false;
    }
    return output;
  }

  if (!std::isfinite(now_seconds) || !std::isfinite(measured_angular_velocity)) {
    return fail("received non-finite timing or IMU data");
  }
  if (io_fault) {
    return fail("motor I/O fault became active");
  }
  if (!imu_fresh) {
    return fail("IMU feedback timed out");
  }
  if (!motor_feedback_fresh) {
    return fail("motor feedback timed out");
  }
  if (now_seconds - run_start_time_ > config_.max_duration) {
    return fail("auto-tune exceeded its maximum duration");
  }

  const double elapsed = now_seconds - state_start_time_;
  const double corrected_measurement = measured_angular_velocity - baseline_;

  switch (state_) {
    case YawAutoTuneState::PRECHECK:
      if (std::abs(measured_angular_velocity) > config_.stationary_threshold) {
        baseline_sum_ = 0.0;
        baseline_count_ = 0;
        state_start_time_ = now_seconds;
      } else {
        baseline_sum_ += measured_angular_velocity;
        ++baseline_count_;
        if (elapsed >= config_.precheck_duration && baseline_count_ >= 20) {
          baseline_ = baseline_sum_ / static_cast<double>(baseline_count_);
          transition(YawAutoTuneState::IDENTIFY_POSITIVE, now_seconds);
        }
      }
      break;

    case YawAutoTuneState::IDENTIFY_POSITIVE:
      positive_samples_.push_back({elapsed, corrected_measurement});
      if (elapsed >= config_.pulse_duration) {
        transition(YawAutoTuneState::REST_AFTER_POSITIVE, now_seconds);
      }
      break;

    case YawAutoTuneState::REST_AFTER_POSITIVE:
      if (elapsed >= config_.rest_duration) {
        transition(YawAutoTuneState::IDENTIFY_NEGATIVE, now_seconds);
      }
      break;

    case YawAutoTuneState::IDENTIFY_NEGATIVE:
      negative_samples_.push_back({elapsed, corrected_measurement});
      if (elapsed >= config_.pulse_duration) {
        transition(YawAutoTuneState::REST_AFTER_NEGATIVE, now_seconds);
      }
      break;

    case YawAutoTuneState::REST_AFTER_NEGATIVE:
      if (elapsed >= config_.rest_duration) {
        result_ = identify(positive_samples_, negative_samples_, config_);
        if (!result_.valid) {
          return fail(result_.message);
        }
        candidate_event_pending_ = true;
        transition(YawAutoTuneState::VALIDATE_POSITIVE, now_seconds);
      }
      break;

    case YawAutoTuneState::VALIDATE_POSITIVE:
      validation_positive_samples_.push_back({elapsed, corrected_measurement});
      if (elapsed >= config_.validation_duration) {
        transition(YawAutoTuneState::REST_AFTER_VALIDATION_POSITIVE, now_seconds);
      }
      break;

    case YawAutoTuneState::REST_AFTER_VALIDATION_POSITIVE:
      if (elapsed >= config_.rest_duration) {
        transition(YawAutoTuneState::VALIDATE_NEGATIVE, now_seconds);
      }
      break;

    case YawAutoTuneState::VALIDATE_NEGATIVE:
      validation_negative_samples_.push_back({elapsed, corrected_measurement});
      if (elapsed >= config_.validation_duration) {
        transition(YawAutoTuneState::FINAL_STOP, now_seconds);
      }
      break;

    case YawAutoTuneState::FINAL_STOP:
      if (elapsed >= config_.rest_duration) {
        if (!validateCandidate()) {
          return fail(result_.message);
        }
        active_ = false;
        last_run_succeeded_ = true;
        finish_event_pending_ = true;
        state_ = YawAutoTuneState::SUCCEEDED;
      }
      break;

    default:
      break;
  }

  auto output = makeUpdate();
  if (candidate_event_pending_) {
    output.apply_candidate = true;
    candidate_event_pending_ = false;
  }
  if (!active_ && finish_event_pending_) {
    output.finished = true;
    output.succeeded = last_run_succeeded_;
    finish_event_pending_ = false;
  }
  return output;
}

YawAutoTuneUpdate YawAutoTuner::cancel()
{
  if (!active_) {
    return makeUpdate();
  }
  active_ = false;
  last_run_succeeded_ = false;
  candidate_event_pending_ = false;
  finish_event_pending_ = false;
  state_ = YawAutoTuneState::CANCELLED;
  result_.valid = false;
  result_.message = "yaw auto-tune cancelled";
  auto output = makeUpdate();
  output.finished = true;
  output.succeeded = false;
  return output;
}

bool YawAutoTuner::active() const
{
  return active_;
}

bool YawAutoTuner::lastRunSucceeded() const
{
  return last_run_succeeded_;
}

YawAutoTuneState YawAutoTuner::state() const
{
  return state_;
}

const YawTuneResult & YawAutoTuner::result() const
{
  return result_;
}

std::string YawAutoTuner::status() const
{
  std::ostringstream stream;
  stream << "state=" << toString(state_);
  if (!result_.message.empty()) {
    stream << " message=\"" << result_.message << "\"";
  }
  if (result_.valid) {
    stream << " kp=" << result_.kp
           << " ki=" << result_.ki
           << " kd=" << result_.kd
           << " output_limit=" << result_.output_limit
           << " gain=" << result_.plant_gain
           << " tau=" << result_.time_constant
           << " delay=" << result_.dead_time
           << " nrmse=" << result_.normalized_rmse
           << " overshoot=" << result_.overshoot_ratio;
  }
  return stream.str();
}

YawTuneResult YawAutoTuner::identify(
  const std::vector<YawResponseSample> & positive,
  const std::vector<YawResponseSample> & negative,
  const YawAutoTuneConfig & config)
{
  YawTuneResult result;
  const StepModel positive_model =
    identifyStep(positive, config.test_angular_velocity, config);
  if (!positive_model.valid) {
    result.message = "positive step: " + positive_model.message;
    return result;
  }

  const StepModel negative_model =
    identifyStep(negative, -config.test_angular_velocity, config);
  if (!negative_model.valid) {
    result.message = "negative step: " + negative_model.message;
    return result;
  }

  result.plant_gain = 0.5 * (positive_model.gain + negative_model.gain);
  result.time_constant =
    0.5 * (positive_model.time_constant + negative_model.time_constant);
  result.dead_time = 0.5 * (positive_model.dead_time + negative_model.dead_time);

  const double lambda = std::max(
    config.min_integral_time,
    config.lambda_factor * result.time_constant);
  const double raw_kp =
    result.time_constant /
    (result.plant_gain * std::max(0.01, lambda + result.dead_time));
  const double integral_time = std::max(
    config.min_integral_time,
    4.0 * result.time_constant);

  result.kp = std::clamp(raw_kp, config.min_kp, config.max_kp);
  result.ki = std::clamp(result.kp / integral_time, config.min_ki, config.max_ki);
  result.kd = 0.0;
  result.output_limit = std::clamp(
    2.0 * config.test_angular_velocity,
    config.min_output_limit,
    config.max_output_limit);
  result.valid =
    std::isfinite(result.kp) && std::isfinite(result.ki) &&
    std::isfinite(result.output_limit);
  result.message = result.valid ? "candidate PI gains identified" : "identified gains are invalid";
  return result;
}

void YawAutoTuner::transition(YawAutoTuneState next, double now_seconds)
{
  state_ = next;
  state_start_time_ = now_seconds;
}

YawAutoTuneUpdate YawAutoTuner::fail(const std::string & reason)
{
  active_ = false;
  last_run_succeeded_ = false;
  candidate_event_pending_ = false;
  finish_event_pending_ = false;
  state_ = YawAutoTuneState::FAILED;
  result_.valid = false;
  result_.message = reason;
  auto output = makeUpdate();
  output.finished = true;
  output.succeeded = false;
  return output;
}

YawAutoTuneUpdate YawAutoTuner::makeUpdate() const
{
  YawAutoTuneUpdate output;
  output.active = active_;
  output.result = result_;
  output.status = status();

  switch (state_) {
    case YawAutoTuneState::IDENTIFY_POSITIVE:
      output.angular_command = config_.test_angular_velocity;
      break;
    case YawAutoTuneState::IDENTIFY_NEGATIVE:
      output.angular_command = -config_.test_angular_velocity;
      break;
    case YawAutoTuneState::VALIDATE_POSITIVE:
      output.angular_command = config_.test_angular_velocity;
      output.use_pid = true;
      break;
    case YawAutoTuneState::VALIDATE_NEGATIVE:
      output.angular_command = -config_.test_angular_velocity;
      output.use_pid = true;
      break;
    default:
      output.angular_command = 0.0;
      break;
  }
  return output;
}

bool YawAutoTuner::validateConfig(
  const YawAutoTuneConfig & config,
  std::string & reason) const
{
  const double values[] = {
    config.test_angular_velocity,
    config.precheck_duration,
    config.pulse_duration,
    config.rest_duration,
    config.validation_duration,
    config.max_duration,
    config.stationary_threshold,
    config.min_plant_gain,
    config.max_plant_gain,
    config.lambda_factor,
    config.min_integral_time,
    config.max_normalized_rmse,
    config.max_overshoot_ratio,
    config.min_kp,
    config.max_kp,
    config.min_ki,
    config.max_ki,
    config.min_output_limit,
    config.max_output_limit,
  };
  for (const double value : values) {
    if (!std::isfinite(value)) {
      reason = "auto-tune parameters must be finite";
      return false;
    }
  }

  if (config.test_angular_velocity <= 0.0 || config.test_angular_velocity > 1.0 ||
    config.precheck_duration < 0.5 || config.pulse_duration < 1.0 ||
    config.rest_duration < 0.5 || config.validation_duration < 1.0 ||
    config.max_duration < 10.0 || config.stationary_threshold <= 0.0 ||
    config.min_plant_gain <= 0.0 || config.max_plant_gain <= config.min_plant_gain ||
    config.lambda_factor < 1.0 || config.min_integral_time <= 0.0 ||
    config.max_normalized_rmse <= 0.0 || config.max_overshoot_ratio <= 1.0 ||
    config.min_kp < 0.0 || config.max_kp <= config.min_kp ||
    config.min_ki < 0.0 || config.max_ki < config.min_ki ||
    config.min_output_limit <= 0.0 ||
    config.max_output_limit < config.min_output_limit)
  {
    reason = "auto-tune parameters are outside their safe ranges";
    return false;
  }
  return true;
}

bool YawAutoTuner::validateCandidate()
{
  const ValidationMetrics positive = validationMetrics(
    validation_positive_samples_, config_.test_angular_velocity);
  const ValidationMetrics negative = validationMetrics(
    validation_negative_samples_, -config_.test_angular_velocity);
  if (!positive.valid || !negative.valid) {
    result_.valid = false;
    result_.message = "not enough validation samples";
    return false;
  }

  result_.normalized_rmse =
    std::max(positive.normalized_rmse, negative.normalized_rmse);
  result_.overshoot_ratio =
    std::max(positive.overshoot_ratio, negative.overshoot_ratio);
  if (result_.normalized_rmse > config_.max_normalized_rmse) {
    result_.valid = false;
    result_.message = "candidate failed tracking-error validation";
    return false;
  }
  if (result_.overshoot_ratio > config_.max_overshoot_ratio) {
    result_.valid = false;
    result_.message = "candidate failed overshoot validation";
    return false;
  }

  result_.valid = true;
  result_.message = "candidate PI gains passed positive and negative validation";
  return true;
}

const char * toString(YawAutoTuneState state)
{
  switch (state) {
    case YawAutoTuneState::IDLE:
      return "idle";
    case YawAutoTuneState::PRECHECK:
      return "precheck";
    case YawAutoTuneState::IDENTIFY_POSITIVE:
      return "identify_positive";
    case YawAutoTuneState::REST_AFTER_POSITIVE:
      return "rest_after_positive";
    case YawAutoTuneState::IDENTIFY_NEGATIVE:
      return "identify_negative";
    case YawAutoTuneState::REST_AFTER_NEGATIVE:
      return "rest_after_negative";
    case YawAutoTuneState::VALIDATE_POSITIVE:
      return "validate_positive";
    case YawAutoTuneState::REST_AFTER_VALIDATION_POSITIVE:
      return "rest_after_validation_positive";
    case YawAutoTuneState::VALIDATE_NEGATIVE:
      return "validate_negative";
    case YawAutoTuneState::FINAL_STOP:
      return "final_stop";
    case YawAutoTuneState::SUCCEEDED:
      return "succeeded";
    case YawAutoTuneState::FAILED:
      return "failed";
    case YawAutoTuneState::CANCELLED:
      return "cancelled";
  }
  return "unknown";
}

}  // namespace ddsm_driver

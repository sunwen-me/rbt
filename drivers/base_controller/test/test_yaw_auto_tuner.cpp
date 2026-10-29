#include <cmath>
#include <iostream>
#include <vector>

#include "ddsm_driver/yaw_auto_tuner.hpp"
#include "test_common.hpp"

using ddsm_driver::YawAutoTuneConfig;
using ddsm_driver::YawAutoTuner;
using ddsm_driver::YawResponseSample;

std::vector<YawResponseSample> makeStep(
  double command,
  double gain,
  double time_constant,
  double dead_time,
  double duration,
  double dt)
{
  std::vector<YawResponseSample> samples;
  for (double time = 0.0; time <= duration; time += dt) {
    double response = 0.0;
    if (time >= dead_time) {
      response =
        gain * command * (1.0 - std::exp(-(time - dead_time) / time_constant));
    }
    samples.push_back({time, response});
  }
  return samples;
}

void test_identifies_conservative_pi()
{
  YawAutoTuneConfig config;
  config.test_angular_velocity = 0.35;
  config.pulse_duration = 3.0;

  const auto positive = makeStep(0.35, 0.9, 0.25, 0.08, 3.0, 0.01);
  const auto negative = makeStep(-0.35, 0.9, 0.25, 0.08, 3.0, 0.01);
  const auto result = YawAutoTuner::identify(positive, negative, config);

  DDSM_CHECK(result.valid);
  DDSM_CHECK(result.plant_gain > 0.8 && result.plant_gain < 1.0);
  DDSM_CHECK(result.time_constant > 0.15 && result.time_constant < 0.35);
  DDSM_CHECK(result.dead_time >= 0.0 && result.dead_time < 0.2);
  DDSM_CHECK(result.kp >= config.min_kp && result.kp <= config.max_kp);
  DDSM_CHECK(result.ki >= config.min_ki && result.ki <= config.max_ki);
  DDSM_CHECK(result.kd == 0.0);
  DDSM_CHECK(result.output_limit >= config.min_output_limit);
  DDSM_CHECK(result.output_limit <= config.max_output_limit);
}

void test_rejects_wrong_feedback_sign()
{
  YawAutoTuneConfig config;
  config.pulse_duration = 3.0;

  const auto positive = makeStep(0.35, -0.8, 0.25, 0.05, 3.0, 0.01);
  const auto negative = makeStep(-0.35, -0.8, 0.25, 0.05, 3.0, 0.01);
  const auto result = YawAutoTuner::identify(positive, negative, config);

  DDSM_CHECK(!result.valid);
}

void test_rejects_missing_imu_immediately()
{
  YawAutoTuner tuner;
  YawAutoTuneConfig config;
  std::string reason;
  DDSM_CHECK(tuner.start(10.0, config, reason));

  const auto update = tuner.update(10.01, 0.0, false, true, false);
  DDSM_CHECK(update.finished);
  DDSM_CHECK(!update.succeeded);
  DDSM_CHECK(!update.active);
  DDSM_CHECK(tuner.state() == ddsm_driver::YawAutoTuneState::FAILED);
}

void test_cancel_stops_active_tune()
{
  YawAutoTuner tuner;
  YawAutoTuneConfig config;
  std::string reason;
  DDSM_CHECK(tuner.start(20.0, config, reason));

  const auto update = tuner.cancel();
  DDSM_CHECK(update.finished);
  DDSM_CHECK(!update.succeeded);
  DDSM_CHECK(!update.active);
  DDSM_CHECK(update.angular_command == 0.0);
  DDSM_CHECK(tuner.state() == ddsm_driver::YawAutoTuneState::CANCELLED);
}

int main()
{
  std::cout << "Running YawAutoTuner tests...\n";
  test_identifies_conservative_pi();
  test_rejects_wrong_feedback_sign();
  test_rejects_missing_imu_immediately();
  test_cancel_stops_active_tune();
  std::cout << "All tests passed!\n";
  return 0;
}

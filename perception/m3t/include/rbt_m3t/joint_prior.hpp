#pragma once

#include <rbt_m3t/joint_prior_types.hpp>
#include <rbt_m3t/model.hpp>
#include <rbt/time_stream.hpp>

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace rbt_m3t {

struct JointStateSample {
    rbt::time::SensorTiming timing;
    std::uint64_t sequence;
    std::string controller_id, driver_session;
    // Canonical source arm order: left, right. Canonical source joint order:
    // shoulder_pan, shoulder_lift, elbow_flex, wrist_flex, wrist_roll, gripper.
    std::array<std::array<double, 6>, 2> values;
};

// Copies and validates the existing SO101 StampedBytes application payload.
// The borrowed native sample can be released immediately after this call.
[[nodiscard]] JointStateSample copy_joint_state_sample(
    std::uint64_t sequence, std::span<const std::uint8_t> payload);

void validate_joint_prior_config(const JointPriorConfig& config,
                                 const RobotModel& model,
                                 const std::vector<ArmSeed>& arms);

class JointPriorBuffer {
  public:
    JointPriorBuffer(JointPriorConfig config, const RobotModel& model,
                     const std::vector<ArmSeed>& arms);
    void push(JointStateSample sample);
    // Latest causal sample in the configured time basis, bounded by age.
    // Missing capture clocks, stale samples and clock mismatch yield no factor.
    [[nodiscard]] JointPriorReport select(const rbt::time::SensorTiming& image) const;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    JointPriorConfig config_;
    RobotModel model_;
    std::deque<JointStateSample> samples_;
    std::vector<std::string> retired_sessions_;
};

} // namespace rbt_m3t

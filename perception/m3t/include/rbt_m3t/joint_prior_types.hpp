#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace rbt_m3t {

// An already associated measurement in the model's bounded joint coordinate.
// This is a scalar Gaussian factor, not a replacement for the visual estimate.
struct JointAnglePrior {
    std::string arm, joint;
    double angle_rad {};
    double standard_deviation_rad {};
};

struct JointPriorReport {
    bool enabled { false };
    std::string status { "disabled" }, time_basis;
    std::string controller_id, driver_session;
    std::uint64_t sequence {};
    std::optional<std::uint64_t> age_ns;
    // Receipt association does not establish exposure-time synchronization.
    bool capture_time_associated { false };
    std::vector<std::string> rejected_mappings;
    std::vector<JointAnglePrior> priors;
};

struct JointPriorMapping {
    std::string arm, joint, source_arm, source_joint;
    // Source protocol: joints 1-5 are degrees; gripper is percent.
    // Every scale/offset is explicit. No VLA or model-convention offsets.
    double scale_rad_per_unit {}, offset_rad {}, standard_deviation_rad {};
};

struct JointPriorConfig {
    bool enabled { false }, required { false }, initialize_from_measurement { true };
    std::string service_name, controller_id;
    // "capture" requires matching capture clocks. "received" is explicit
    // best effort on the shared host receipt clock; there is no fallback.
    std::string time_basis { "capture" };
    std::uint64_t maximum_age_ns { 50'000'000U };
    std::size_t capacity { 128U };
    std::vector<JointPriorMapping> mappings;
};

} // namespace rbt_m3t

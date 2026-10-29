#include <rbt_m3t/joint_prior.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace rbt_m3t {
namespace {

constexpr std::size_t kMaximumPayloadBytes = 64U * 1024U;
constexpr std::size_t kMaximumIdentifierBytes = 128U;
constexpr std::size_t kMaximumCapacity = 512U;
constexpr std::size_t kMaximumMappings = 64U;
constexpr std::size_t kRetiredSessionLimit = 8U;
constexpr int kMaximumJsonDepth = 32;
constexpr std::array<const char*, 6> kSourceJoints {
    "shoulder_pan", "shoulder_lift", "elbow_flex", "wrist_flex", "wrist_roll", "gripper"
};

[[noreturn]] void invalid(const std::string& message) {
    throw std::invalid_argument(message);
}

using Json = nlohmann::json;

[[nodiscard]] std::string read_string(const Json& node, const char* field) {
    if (!node.is_string()) {
        invalid(std::string(field) + " must be a string");
    }
    return node.get<std::string>();
}

[[nodiscard]] double read_finite_number(const Json& node, const char* field) {
    if (!node.is_number()) {
        invalid(std::string(field) + " must be a number");
    }
    const auto value = node.get<double>();
    if (!std::isfinite(value)) {
        invalid(std::string(field) + " must be finite");
    }
    return value;
}

[[nodiscard]] std::uint64_t read_sequence(const Json& node) {
    if (node.is_number_unsigned()) {
        return node.get<std::uint64_t>();
    }
    if (node.is_number_integer()) {
        const auto value = node.get<std::int64_t>();
        if (value >= 0) {
            return static_cast<std::uint64_t>(value);
        }
    }
    invalid("sequence must be a non-negative uint64 JSON integer");
}

void validate_identifier(const std::string& value, const char* field) {
    if (value.empty() || value.size() > kMaximumIdentifierBytes) {
        invalid(std::string(field) + " must be non-empty and at most 128 bytes");
    }
}

void validate_keys(const Json& node,
                   std::initializer_list<const char*> allowed,
                   const char* description) {
    if (!node.is_object()) {
        invalid(std::string(description) + " must be an object");
    }
    for (auto field = node.begin(); field != node.end(); ++field) {
        const auto known = std::find_if(allowed.begin(), allowed.end(), [&field](const char* key) {
            return field.key() == key;
        });
        if (known == allowed.end()) {
            invalid(std::string(description) + " contains an unknown field: " + field.key());
        }
    }
}

[[nodiscard]] bool known_source_joint(const std::string& joint) {
    return std::any_of(kSourceJoints.begin(), kSourceJoints.end(), [&joint](const char* candidate) {
        return joint == candidate;
    });
}

[[nodiscard]] std::size_t source_arm_index(const std::string& arm) {
    if (arm == "left") {
        return 0U;
    }
    if (arm == "right") {
        return 1U;
    }
    invalid("source_arm must be left or right");
}

[[nodiscard]] std::size_t source_joint_index(const std::string& joint) {
    for (std::size_t index = 0U; index < kSourceJoints.size(); ++index) {
        if (joint == kSourceJoints[index]) {
            return index;
        }
    }
    invalid("source_joint must be a canonical SO101 joint name");
}

[[nodiscard]] std::string session_key(const std::string& controller,
                                      const std::string& session) {
    return std::to_string(controller.size()) + ":" + controller + session;
}

[[nodiscard]] std::uint64_t timestamp_distance(std::int64_t later,
                                              std::int64_t earlier) noexcept {
    return static_cast<std::uint64_t>(later) - static_cast<std::uint64_t>(earlier);
}

[[nodiscard]] bool is_valid_timing(const rbt::time::SensorTiming& timing) noexcept {
    return timing.received.clock.is_well_formed()
           && (!timing.capture.has_value() || timing.capture->clock.is_well_formed())
           && (!timing.capture_uncertainty_ns.has_value() || timing.capture.has_value());
}

[[nodiscard]] bool same_capture_clock(const rbt::time::SensorTiming& lhs,
                                      const rbt::time::SensorTiming& rhs) noexcept {
    if (lhs.capture.has_value() != rhs.capture.has_value()) {
        return false;
    }
    return !lhs.capture.has_value() || lhs.capture->clock == rhs.capture->clock;
}

[[nodiscard]] bool clock_identity_changed(const rbt::time::SensorTiming& lhs,
                                          const rbt::time::SensorTiming& rhs) noexcept {
    return lhs.received.clock != rhs.received.clock || !same_capture_clock(lhs, rhs);
}

[[nodiscard]] const rbt::time::Timestamp* timestamp_for(
    const JointStateSample& sample, const std::string& time_basis) noexcept {
    if (time_basis == "capture") {
        return sample.timing.capture.has_value() ? &*sample.timing.capture : nullptr;
    }
    return &sample.timing.received;
}

} // namespace

JointStateSample copy_joint_state_sample(std::uint64_t sequence,
                                         std::span<const std::uint8_t> payload) {
    if (payload.size() > kMaximumPayloadBytes) {
        invalid("SO101 StampedBytes payload exceeds 64 KiB");
    }
    const auto stamped = rbt::time::decode_stamped_bytes(payload);
    const auto* decoded = std::get_if<rbt::time::DecodedStampedBytes>(&stamped);
    if (decoded == nullptr) {
        invalid("SO101 StampedBytes timing prefix is invalid");
    }
    if (decoded->bytes.empty()) {
        invalid("SO101 state JSON payload is empty");
    }

    const std::string json(reinterpret_cast<const char*>(decoded->bytes.data()),
                           decoded->bytes.size());
    if (json.find('\0') != std::string::npos) {
        invalid("SO101 state JSON may not contain NUL bytes");
    }

    std::vector<std::set<std::string>> object_keys;
    const auto callback = [&object_keys](int depth, Json::parse_event_t event, Json& parsed) {
        if (depth > kMaximumJsonDepth) {
            invalid("SO101 state JSON exceeds the maximum nesting depth");
        }
        switch (event) {
            case Json::parse_event_t::object_start:
                if (object_keys.size() >= static_cast<std::size_t>(kMaximumJsonDepth)) {
                    invalid("SO101 state JSON exceeds the maximum nesting depth");
                }
                object_keys.emplace_back();
                break;
            case Json::parse_event_t::object_end:
                if (object_keys.empty()) {
                    invalid("SO101 state JSON object nesting is invalid");
                }
                object_keys.pop_back();
                break;
            case Json::parse_event_t::key: {
                if (object_keys.empty()
                    || !object_keys.back().insert(parsed.get<std::string>()).second) {
                    invalid("SO101 state JSON contains a duplicate object key");
                }
                break;
            }
            case Json::parse_event_t::array_start:
            case Json::parse_event_t::array_end:
            case Json::parse_event_t::value:
                break;
        }
        return true;
    };

    try {
        const auto root = Json::parse(json, callback, true, false);
        validate_keys(root,
                      { "protocol", "kind", "units", "sequence", "controller_id",
                        "driver_session", "arms", "fault", "capture_time_known", "mode",
                        "monotonic_ns", "read_intervals", "last_command_sequence",
                        "last_command_status" },
                      "SO101 state JSON");

        if (read_string(root.at("protocol"), "protocol") != "rbt.so101.dual.v1"
            || read_string(root.at("kind"), "kind") != "state"
            || read_string(root.at("units"), "units") != "joint_deg_gripper_percent") {
            invalid("SO101 state protocol, kind, or units do not match");
        }
        const auto json_sequence = read_sequence(root.at("sequence"));
        if (json_sequence != sequence) {
            invalid("SO101 JSON sequence does not match the StampedBytes header");
        }

        const auto controller_id = read_string(root.at("controller_id"), "controller_id");
        const auto driver_session = read_string(root.at("driver_session"), "driver_session");
        validate_identifier(controller_id, "controller_id");
        validate_identifier(driver_session, "driver_session");

        if (root.contains("fault") && !root.at("fault").is_null()) {
            invalid("SO101 state fault must be null");
        }

        const auto& arms = root.at("arms");
        validate_keys(arms, { "left", "right" }, "SO101 arms");
        std::array<std::array<double, 6>, 2> values {};
        for (std::size_t arm_index = 0U; arm_index < 2U; ++arm_index) {
            const auto& arm_node = arms.at(arm_index == 0U ? "left" : "right");
            if (!arm_node.is_array() || arm_node.size() != 6U) {
                invalid("each SO101 arm must contain exactly six joint values");
            }
            for (std::size_t joint_index = 0U; joint_index < 6U; ++joint_index) {
                values[arm_index][joint_index] = read_finite_number(
                    arm_node[joint_index], "SO101 joint value");
            }
        }

        return JointStateSample { decoded->timing, sequence, controller_id,
                                  driver_session, values };
    } catch (const Json::exception& error) {
        invalid(std::string("could not parse SO101 state JSON: ") + error.what());
    }
}

void validate_joint_prior_config(const JointPriorConfig& config,
                                 const RobotModel& model,
                                 const std::vector<ArmSeed>& arms) {
    if (config.time_basis != "capture" && config.time_basis != "received") {
        invalid("joint prior time_basis must be capture or received");
    }
    if (config.capacity == 0U || config.capacity > kMaximumCapacity) {
        invalid("joint prior capacity must be between 1 and 512");
    }
    if (config.maximum_age_ns == 0U || config.maximum_age_ns > 1'000'000'000U) {
        invalid("joint prior maximum_age_ns must be between 1 and 1000000000");
    }
    if (config.mappings.size() > kMaximumMappings
        || (config.enabled && config.mappings.empty())) {
        invalid("enabled joint priors require between 1 and 64 mappings");
    }
    if (config.required && !config.enabled) {
        invalid("joint priors cannot be required while disabled");
    }
    if (!config.controller_id.empty()) {
        validate_identifier(config.controller_id, "configured controller_id");
    }
    if (config.enabled) {
        if (config.service_name.empty() || config.service_name.size() > 240U) {
            invalid("enabled joint priors require a service_name of at most 240 bytes");
        }
        if (config.controller_id.empty()) {
            invalid("enabled joint priors require a controller_id");
        }
    } else if (!config.service_name.empty() && config.service_name.size() > 240U) {
        invalid("joint prior service_name must be at most 240 bytes");
    }

    std::set<std::string> known_arms;
    for (const auto& arm : arms) {
        validate_identifier(arm.name, "configured arm name");
        if (!known_arms.insert(arm.name).second) {
            invalid("configured arm names must be unique");
        }
    }

    std::set<std::string> joint_names;
    for (const auto& joint : model.joints) {
        if (!joint_names.insert(joint.name).second) {
            invalid("robot model joint names must be unique for joint priors");
        }
    }

    std::set<std::pair<std::string, std::string>> targets;
    for (const auto& mapping : config.mappings) {
        if (known_arms.find(mapping.arm) == known_arms.end()) {
            invalid("joint prior mapping arm must name a configured arm");
        }
        const auto target = std::find_if(model.joints.begin(), model.joints.end(),
                                         [&mapping](const ModelJoint& joint) {
                                             return joint.name == mapping.joint;
                                         });
        if (target == model.joints.end() || target->type != "revolute") {
            invalid("joint prior mapping must target a known revolute model joint");
        }
        if (!targets.emplace(mapping.arm, mapping.joint).second) {
            invalid("joint prior target arm/joint mappings must be unique");
        }
        (void)source_arm_index(mapping.source_arm);
        if (!known_source_joint(mapping.source_joint)) {
            invalid("source_joint must be a canonical SO101 joint name");
        }
        if (!std::isfinite(mapping.scale_rad_per_unit)
            || mapping.scale_rad_per_unit == 0.0
            || !std::isfinite(mapping.offset_rad)
            || !std::isfinite(mapping.standard_deviation_rad)
            || mapping.standard_deviation_rad <= 0.0) {
            invalid("joint prior scale, offset, and standard deviation must be finite, with nonzero scale and positive deviation");
        }

        const auto sigma_float = static_cast<float>(mapping.standard_deviation_rad);
        const auto precision = 1.0 / (static_cast<double>(sigma_float)
                                      * static_cast<double>(sigma_float));
        const auto precision_float = static_cast<float>(precision);
        const auto joint_span = target->upper - target->lower;
        const auto worst_weighted_residual = std::max(1.0, joint_span) * precision;
        if (!std::isfinite(sigma_float) || sigma_float <= 0.0F
            || !std::isfinite(precision) || precision <= 0.0
            || !std::isfinite(precision_float) || precision_float <= 0.0F
            || precision > static_cast<double>(std::numeric_limits<float>::max())
            || !std::isfinite(joint_span) || joint_span < 0.0
            || !std::isfinite(worst_weighted_residual)
            || worst_weighted_residual
                   > static_cast<double>(std::numeric_limits<float>::max())) {
            invalid("joint prior uncertainty is not numerically representable by the optimizer");
        }
    }
}

JointPriorBuffer::JointPriorBuffer(JointPriorConfig config, const RobotModel& model,
                                   const std::vector<ArmSeed>& arms)
    : config_{std::move(config)}, model_{model} {
    validate_joint_prior_config(config_, model_, arms);
}

void JointPriorBuffer::push(JointStateSample sample) {
    validate_identifier(sample.controller_id, "controller_id");
    validate_identifier(sample.driver_session, "driver_session");
    if (!config_.controller_id.empty() && sample.controller_id != config_.controller_id) {
        invalid("SO101 sample controller_id does not match the configured controller");
    }
    if (!is_valid_timing(sample.timing)) {
        invalid("SO101 state timing is invalid");
    }
    for (const auto& arm : sample.values) {
        if (!std::all_of(arm.begin(), arm.end(), [](double value) {
                return std::isfinite(value);
            })) {
            invalid("SO101 state values must be finite");
        }
    }

    const auto incoming_session = session_key(sample.controller_id, sample.driver_session);
    if (std::find(retired_sessions_.begin(), retired_sessions_.end(), incoming_session)
        != retired_sessions_.end()) {
        invalid("SO101 sample belongs to a retired driver session");
    }

    bool new_session = samples_.empty();
    bool clock_changed = false;
    if (!samples_.empty()) {
        const auto& previous = samples_.back();
        const auto previous_session = session_key(previous.controller_id,
                                                  previous.driver_session);
        new_session = incoming_session != previous_session;
        if (sample.timing.received.clock == previous.timing.received.clock
            && sample.timing.received.ns <= previous.timing.received.ns) {
            invalid("SO101 received timestamps must increase within a clock identity");
        }
        if (sample.timing.capture.has_value() && previous.timing.capture.has_value()
            && sample.timing.capture->clock == previous.timing.capture->clock
            && sample.timing.capture->ns <= previous.timing.capture->ns) {
            invalid("SO101 capture timestamps must increase within a clock identity");
        }
        if (!new_session) {
            if (sample.sequence <= previous.sequence) {
                invalid("SO101 sequence must increase within a driver session");
            }
            clock_changed = clock_identity_changed(sample.timing, previous.timing);
        }
    }

    if (new_session && !samples_.empty()) {
        const auto& previous = samples_.back();
        retired_sessions_.push_back(session_key(previous.controller_id,
                                                previous.driver_session));
        if (retired_sessions_.size() > kRetiredSessionLimit) {
            retired_sessions_.erase(retired_sessions_.begin());
        }
        samples_.clear();
    } else if (clock_changed) {
        samples_.clear();
    }

    samples_.push_back(std::move(sample));
    while (samples_.size() > config_.capacity) {
        samples_.pop_front();
    }
}

JointPriorReport JointPriorBuffer::select(const rbt::time::SensorTiming& image) const {
    JointPriorReport report;
    report.enabled = config_.enabled;
    report.time_basis = config_.time_basis;
    if (!config_.enabled) {
        report.status = "disabled";
        return report;
    }
    if (samples_.empty()) {
        report.status = "missing";
        return report;
    }

    const auto set_sample_diagnostics = [&report](const JointStateSample& sample) {
        report.controller_id = sample.controller_id;
        report.driver_session = sample.driver_session;
        report.sequence = sample.sequence;
    };
    set_sample_diagnostics(samples_.back());

    const rbt::time::Timestamp* image_time = nullptr;
    bool clock_matches = true;
    if (config_.time_basis == "capture") {
        if (!image.capture.has_value()) {
            clock_matches = false;
        } else {
            image_time = &*image.capture;
            for (const auto& sample : samples_) {
                if (sample.timing.capture.has_value()) {
                    if (sample.timing.capture->clock != image.capture->clock) {
                        clock_matches = false;
                        break;
                    }
                } else {
                    clock_matches = false;
                    break;
                }
            }
        }
    } else {
        image_time = &image.received;
        if (image.received.clock.kind() != rbt::time::ClockKind::HostMonotonic) {
            clock_matches = false;
        }
        for (const auto& sample : samples_) {
            if (sample.timing.received.clock.kind() != rbt::time::ClockKind::HostMonotonic
                || sample.timing.received.clock != image.received.clock) {
                clock_matches = false;
                break;
            }
        }
    }
    if (!clock_matches || image_time == nullptr) {
        report.status = "clock_mismatch";
        return report;
    }

    const JointStateSample* selected = nullptr;
    const rbt::time::Timestamp* selected_time = nullptr;
    for (const auto& sample : samples_) {
        const auto* sample_time = timestamp_for(sample, config_.time_basis);
        if (sample_time == nullptr) {
            continue;
        }
        if (sample_time->ns <= image_time->ns) {
            if (selected_time == nullptr || sample_time->ns > selected_time->ns) {
                selected = &sample;
                selected_time = sample_time;
            }
        }
    }
    if (selected == nullptr || selected_time == nullptr) {
        report.status = "missing";
        return report;
    }

    set_sample_diagnostics(*selected);
    const auto age_ns = timestamp_distance(image_time->ns, selected_time->ns);
    report.age_ns = age_ns;
    report.capture_time_associated = config_.time_basis == "capture";
    if (age_ns > config_.maximum_age_ns) {
        report.status = "stale";
        return report;
    }
    if (!config_.controller_id.empty() && selected->controller_id != config_.controller_id) {
        report.status = "missing";
        return report;
    }

    for (const auto& mapping : config_.mappings) {
        const auto source_arm = source_arm_index(mapping.source_arm);
        const auto source_joint = source_joint_index(mapping.source_joint);
        const auto angle = selected->values[source_arm][source_joint]
                           * mapping.scale_rad_per_unit + mapping.offset_rad;
        const auto joint = std::find_if(model_.joints.begin(), model_.joints.end(),
                                        [&mapping](const ModelJoint& candidate) {
                                            return candidate.name == mapping.joint;
                                        });
        const auto float_angle = static_cast<float>(angle);
        if (!std::isfinite(angle) || joint == model_.joints.end()
            || angle < joint->lower || angle > joint->upper
            || !std::isfinite(float_angle)) {
            report.rejected_mappings.push_back(mapping.arm + "/" + mapping.joint
                                               + ": mapped value outside URDF limits");
            continue;
        }
        report.priors.push_back(JointAnglePrior { mapping.arm, mapping.joint, angle,
                                                  mapping.standard_deviation_rad });
    }
    report.status = report.priors.empty() ? "no_valid_mappings" : "applied";
    return report;
}

std::size_t JointPriorBuffer::size() const noexcept {
    return samples_.size();
}

} // namespace rbt_m3t

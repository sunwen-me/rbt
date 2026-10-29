#include <rbt_m3t/joint_prior.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const std::string& description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        ++failures;
    }
}

template <typename Function>
void expect_invalid(Function&& function, const std::string& description) {
    try {
        std::forward<Function>(function)();
    } catch (const std::invalid_argument&) {
        return;
    } catch (const std::exception& error) {
        check(false, description + " (threw the wrong exception: " + error.what() + ")");
        return;
    }
    check(false, description + " (did not throw)");
}

[[nodiscard]] rbt::time::ClockIdentity clock(rbt::time::ClockKind kind,
                                             std::uint8_t source,
                                             std::uint64_t generation = 0U) {
    std::array<std::uint8_t, 48> bytes {};
    bytes[0] = source;
    return rbt::time::ClockIdentity::create(
               kind, rbt::time::ClockSourceId { bytes }, generation)
        .value();
}

[[nodiscard]] rbt::time::Timestamp timestamp(const rbt::time::ClockIdentity& identity,
                                              std::int64_t ns) {
    return rbt::time::Timestamp::create(identity, ns).value();
}

[[nodiscard]] rbt::time::SensorTiming timing(
    std::optional<rbt::time::Timestamp> capture,
    rbt::time::Timestamp received) {
    return rbt::time::SensorTiming::create(std::move(capture), std::move(received), {}, {})
        .value();
}

[[nodiscard]] std::string state_json(std::uint64_t sequence,
                                     const std::string& fault = "null",
                                     const std::string& values =
                                         "[-8.967032967032967,-98.15384615384616,"
                                         "91.42857142857143,-7.912087912087912,"
                                         "-158.37362637362637,0.0]") {
    return "{\"protocol\":\"rbt.so101.dual.v1\",\"kind\":\"state\","
           "\"units\":\"joint_deg_gripper_percent\",\"sequence\":"
           + std::to_string(sequence)
           + ",\"controller_id\":\"so101-dual-1\","
             "\"driver_session\":\"629d8369fd424af7ae176d207e0378d0\","
             "\"fault\":"
           + fault
           + ",\"capture_time_known\":false,\"mode\":\"readonly\","
             "\"monotonic_ns\":174530530369429,\"read_intervals\":{"
             "\"left\":{\"start_ns\":174530526718363,\"end_ns\":174530528319213},"
             "\"right\":{\"start_ns\":174530528540354,\"end_ns\":174530530213130}},"
             "\"last_command_sequence\":0,\"last_command_status\":\"no_command\","
             "\"arms\":{\"left\":"
           + values
           + ",\"right\":[1,2,3,4,5,6]}}";
}

[[nodiscard]] std::string real_state_json(std::uint64_t sequence) {
    auto json = state_json(sequence);
    const auto right_at = json.find(",\"right\":[1,2,3,4,5,6]");
    json.replace(right_at, std::string(",\"right\":[1,2,3,4,5,6]").size(),
                 ",\"right\":[-17.45054945054945,-39.38461538461539,"
                 "93.0989010989011,-46.72527472527472,-53.582417582417584,"
                 "0.5532503457814661]");
    return json;
}

[[nodiscard]] std::vector<std::uint8_t> stamped_payload(
    const std::string& json, const rbt::time::SensorTiming& sample_timing) {
    std::vector<std::uint8_t> payload(rbt::time::kStampedBytesPrefixBytes + json.size());
    const auto prefix = std::span<std::uint8_t>(payload).first(
        rbt::time::kStampedBytesPrefixBytes);
    check(rbt::time::write_timing_prefix(prefix, sample_timing)
              == rbt::time::PrefixCodecError::None,
          "timing prefix fixture should encode");
    std::memcpy(payload.data() + rbt::time::kStampedBytesPrefixBytes,
                json.data(), json.size());
    return payload;
}

[[nodiscard]] rbt_m3t::RobotModel model() {
    rbt_m3t::RobotModel result;
    result.name = "joint prior fixture";
    result.root_link = "base";
    result.joints = {
        { "q1", "revolute", "base", "link1", Eigen::Affine3f::Identity(),
          Eigen::Vector3f::UnitZ(), -1.0, 1.0 },
        { "q2", "revolute", "base", "link2", Eigen::Affine3f::Identity(),
          Eigen::Vector3f::UnitZ(), -10.0, 10.0 },
        { "q5", "revolute", "base", "link5", Eigen::Affine3f::Identity(),
          Eigen::Vector3f::UnitZ(), -10.0, 10.0 },
        { "gripper_joint", "revolute", "base", "gripper", Eigen::Affine3f::Identity(),
          Eigen::Vector3f::UnitZ(), -10.0, 10.0 },
        { "fixed", "fixed", "base", "fixed_link", Eigen::Affine3f::Identity(),
          Eigen::Vector3f::Zero(), 0.0, 0.0 },
    };
    return result;
}

[[nodiscard]] std::vector<rbt_m3t::ArmSeed> arms() {
    rbt_m3t::ArmSeed left;
    left.name = "left_arm";
    rbt_m3t::ArmSeed right;
    right.name = "right_arm";
    return { std::move(left), std::move(right) };
}

[[nodiscard]] rbt_m3t::JointPriorMapping mapping(
    std::string arm, std::string joint, std::string source_arm,
    std::string source_joint, double scale = 0.01, double offset = 0.0,
    double sigma = 0.05) {
    return { std::move(arm), std::move(joint), std::move(source_arm),
             std::move(source_joint), scale, offset, sigma };
}

[[nodiscard]] rbt_m3t::JointPriorConfig config(
    std::string basis = "capture", std::uint64_t maximum_age = 500U,
    std::size_t capacity = 4U) {
    rbt_m3t::JointPriorConfig result;
    result.enabled = true;
    result.service_name = "joint-state";
    result.controller_id = "controller-1";
    result.time_basis = std::move(basis);
    result.maximum_age_ns = maximum_age;
    result.capacity = capacity;
    result.mappings = { mapping("left_arm", "q2", "left", "shoulder_pan") };
    return result;
}

[[nodiscard]] rbt_m3t::JointStateSample sample(
    std::uint64_t sequence, std::string session, std::int64_t receipt_ns,
    std::optional<std::int64_t> capture_ns, std::uint8_t received_source = 1U,
    std::uint64_t received_generation = 0U, std::uint8_t capture_source = 2U,
    std::uint64_t capture_generation = 0U, double left_shoulder_pan = 10.0) {
    const auto received_clock = clock(rbt::time::ClockKind::HostMonotonic,
                                      received_source, received_generation);
    std::optional<rbt::time::Timestamp> capture;
    if (capture_ns.has_value()) {
        capture = timestamp(clock(rbt::time::ClockKind::Device, capture_source,
                                  capture_generation), *capture_ns);
    }
    std::array<std::array<double, 6>, 2> values {};
    values[0] = { left_shoulder_pan, 20.0, 30.0, 40.0, 50.0, 60.0 };
    values[1] = { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0 };
    return { timing(std::move(capture), timestamp(received_clock, receipt_ns)), sequence,
             "controller-1", std::move(session), values };
}

void decoder_copies_and_validates_the_native_sample() {
    const auto host = clock(rbt::time::ClockKind::HostMonotonic, 1U);
    const auto source_timing = timing({}, timestamp(host, 1234));
    auto payload = stamped_payload(real_state_json(3U), source_timing);
    const auto copied = rbt_m3t::copy_joint_state_sample(3U, payload);
    payload.clear();
    check(copied.timing.capture == std::nullopt,
          "state decoder preserves unknown native capture time");
    check(copied.timing.received.clock == host,
          "state decoder copies the validated native receive timestamp");
    check(copied.controller_id == "so101-dual-1"
              && copied.driver_session == "629d8369fd424af7ae176d207e0378d0",
          "state decoder accepts the real controller and driver session fields");
    check(copied.values[0][0] == -8.967032967032967
              && std::abs(copied.values[1][5] - 0.5532503457814661) < 1.0e-12,
          "state decoder copies both canonical arm arrays");

    auto rejects_json = [&](std::uint64_t sequence, const std::string& json,
                            const std::string& description) {
        const auto bytes = stamped_payload(json, source_timing);
        expect_invalid([&] {
            (void)rbt_m3t::copy_joint_state_sample(sequence, bytes);
        }, description);
    };
    auto without_root_scalar = [](std::string json, const std::string& field) {
        const auto property = "\"" + field + "\":";
        const auto position = json.find(property);
        if (position == std::string::npos) {
            return json;
        }
        const auto value_end = json.find(',', position);
        if (position == 1U) {
            json.erase(position, value_end - position + 1U);
        } else {
            json.erase(position - 1U, value_end - position + 1U);
        }
        return json;
    };
    auto wrong_units = state_json(4U);
    const auto units_at = wrong_units.find("joint_deg_gripper_percent");
    wrong_units.replace(units_at, std::string("joint_deg_gripper_percent").size(), "radians");
    rejects_json(4U, wrong_units, "wrong SO101 units are rejected");

    auto wrong_protocol = state_json(4U);
    const auto protocol_at = wrong_protocol.find("rbt.so101.dual.v1");
    wrong_protocol.replace(protocol_at, std::string("rbt.so101.dual.v1").size(), "other.v1");
    rejects_json(4U, wrong_protocol, "wrong SO101 protocol is rejected");

    rejects_json(5U, state_json(4U), "JSON/header sequence mismatch is rejected");
    rejects_json(4U, state_json(4U, "true"),
                 "a non-null fault is rejected");
    rejects_json(4U, state_json(4U, "null", "[1,2,3,4,5,1e9999]"),
                 "non-finite joint values are rejected");
    rejects_json(4U, state_json(4U, "null", "[1,2,3,4,5,true]"),
                 "boolean joint values are rejected as non-numbers");

    for (const auto* field : { "protocol", "sequence", "controller_id", "driver_session" }) {
        rejects_json(4U, without_root_scalar(state_json(4U), field),
                     std::string("missing required ") + field + " is rejected");
    }
    auto missing_arms = state_json(4U);
    const auto arms_property = missing_arms.find(",\"arms\":");
    missing_arms.erase(arms_property, missing_arms.rfind('}') - arms_property);
    rejects_json(4U, missing_arms, "missing required arms object is rejected");

    auto missing_left_arm = state_json(4U);
    const auto arms_object = missing_left_arm.find("\"arms\":{");
    const auto left_property = missing_left_arm.find("\"left\":", arms_object);
    const auto right_property = missing_left_arm.find(",\"right\":", left_property);
    missing_left_arm.erase(left_property, right_property - left_property + 1U);
    rejects_json(4U, missing_left_arm, "missing left arm is rejected");

    auto missing_right_arm = state_json(4U);
    const auto right_arm_property = missing_right_arm.find(",\"right\":",
                                                           missing_right_arm.find("\"arms\":{") );
    const auto arms_end = missing_right_arm.find("}}", right_arm_property);
    missing_right_arm.erase(right_arm_property, arms_end - right_arm_property);
    rejects_json(4U, missing_right_arm, "missing right arm is rejected");

    auto wrong_protocol_type = state_json(4U);
    const auto protocol_value = wrong_protocol_type.find("\"protocol\":\"rbt.so101.dual.v1\"");
    wrong_protocol_type.replace(protocol_value,
                                std::string("\"protocol\":\"rbt.so101.dual.v1\"").size(),
                                "\"protocol\":true");
    rejects_json(4U, wrong_protocol_type, "boolean protocol field is rejected");

    auto wrong_controller_type = state_json(4U);
    const auto controller_value = wrong_controller_type.find(
        "\"controller_id\":\"so101-dual-1\"");
    wrong_controller_type.replace(controller_value,
                                  std::string("\"controller_id\":\"so101-dual-1\"").size(),
                                  "\"controller_id\":false");
    rejects_json(4U, wrong_controller_type, "boolean controller id is rejected");

    const auto maximum_sequence = 9'007'199'254'740'991ULL;
    const auto maximum_payload = stamped_payload(state_json(maximum_sequence), source_timing);
    check(rbt_m3t::copy_joint_state_sample(maximum_sequence, maximum_payload).sequence
              == maximum_sequence,
          "maximum exact safe JSON sequence is accepted");

    constexpr std::uint64_t beyond_double_integer = 9'007'199'254'740'993ULL;
    const auto beyond_double_payload = stamped_payload(
        state_json(beyond_double_integer), source_timing);
    check(rbt_m3t::copy_joint_state_sample(beyond_double_integer, beyond_double_payload)
                  .sequence == beyond_double_integer,
          "uint64 JSON sequence above the exact double range is preserved exactly");

    constexpr auto maximum_uint64 = std::numeric_limits<std::uint64_t>::max();
    const auto maximum_uint64_payload = stamped_payload(state_json(maximum_uint64), source_timing);
    check(rbt_m3t::copy_joint_state_sample(maximum_uint64, maximum_uint64_payload).sequence
              == maximum_uint64,
          "maximum uint64 JSON sequence is accepted exactly");

    auto overflow_sequence = state_json(4U);
    const auto sequence_at = overflow_sequence.find("\"sequence\":4");
    overflow_sequence.replace(sequence_at, std::string("\"sequence\":4").size(),
                              "\"sequence\":18446744073709551616");
    rejects_json(4U, overflow_sequence, "uint64 sequence overflow is rejected");

    auto fractional_sequence = state_json(4U);
    const auto fractional_at = fractional_sequence.find("\"sequence\":4");
    fractional_sequence.replace(fractional_at, std::string("\"sequence\":4").size(),
                                "\"sequence\":4.0");
    rejects_json(4U, fractional_sequence, "floating-point sequence is rejected");

    auto exponent_sequence = state_json(4U);
    const auto exponent_at = exponent_sequence.find("\"sequence\":4");
    exponent_sequence.replace(exponent_at, std::string("\"sequence\":4").size(),
                              "\"sequence\":4e0");
    rejects_json(4U, exponent_sequence, "exponent-form sequence is rejected");

    auto duplicate_root_key = state_json(4U);
    duplicate_root_key.insert(1U, "\"kind\":\"state\",");
    rejects_json(4U, duplicate_root_key, "duplicate root JSON keys are rejected");

    auto duplicate_nested_key = state_json(4U);
    const auto left_at = duplicate_nested_key.find("\"arms\":{\"left\":");
    duplicate_nested_key.replace(left_at, std::string("\"arms\":{\"left\":").size(),
                                  "\"arms\":{\"left\":[1,2,3,4,5,6],\"left\":");
    rejects_json(4U, duplicate_nested_key, "duplicate nested JSON keys are rejected");

    std::string deeply_nested(40U, '[');
    deeply_nested += "0";
    deeply_nested.append(40U, ']');
    rejects_json(4U, deeply_nested, "JSON nesting beyond the configured limit is rejected");
    rejects_json(4U, "{", "malformed JSON is rejected");
    rejects_json(4U, state_json(4U) + " trailing", "trailing JSON content is rejected");

    std::vector<std::uint8_t> oversized(64U * 1024U + 1U);
    expect_invalid([&] {
        (void)rbt_m3t::copy_joint_state_sample(0U, oversized);
    }, "payload above 64 KiB is rejected");
}

void selection_is_causal_and_accepts_the_exact_age_boundary() {
    auto configured = config("capture", 500U);
    rbt_m3t::JointPriorBuffer buffer(configured, model(), arms());
    buffer.push(sample(1U, "session-1", 100U, 1000));

    const auto device = clock(rbt::time::ClockKind::Device, 2U);
    const auto receipt = clock(rbt::time::ClockKind::HostMonotonic, 1U);
    const auto exact = buffer.select(timing(timestamp(device, 1500), timestamp(receipt, 9999)));
    check(exact.status == "applied" && exact.age_ns == 500U,
          "capture selection accepts a sample exactly at maximum age");
    const auto stale = buffer.select(timing(timestamp(device, 1501), timestamp(receipt, 10000)));
    check(stale.status == "stale" && stale.age_ns == 501U,
          "capture selection reports a sample beyond maximum age as stale");
    const auto future = buffer.select(timing(timestamp(device, 999), timestamp(receipt, 10001)));
    check(future.status == "missing" && future.priors.empty(),
          "future measurements are not selected");
}

void selection_requires_the_configured_clock_basis_without_fallback() {
    const auto host = clock(rbt::time::ClockKind::HostMonotonic, 1U);
    const auto device = clock(rbt::time::ClockKind::Device, 2U);

    rbt_m3t::JointPriorBuffer capture_buffer(config("capture"), model(), arms());
    capture_buffer.push(sample(1U, "session-1", 100U, std::nullopt));
    const auto capture_result = capture_buffer.select(
        timing(timestamp(device, 1000), timestamp(host, 200)));
    check(capture_result.status == "clock_mismatch" && capture_result.priors.empty(),
          "capture mode does not fall back to the receive clock");

    rbt_m3t::JointPriorBuffer received_buffer(config("received"), model(), arms());
    received_buffer.push(sample(1U, "session-1", 100U, std::nullopt));
    const auto received_result = received_buffer.select(
        timing(timestamp(device, 1000), timestamp(host, 200)));
    check(received_result.status == "applied" && !received_result.capture_time_associated,
          "explicit receive mode associates only on matching HostMonotonic clocks");
    check(received_result.time_basis == "received",
          "report records the selected receive-time basis");

    const auto other_host = clock(rbt::time::ClockKind::HostMonotonic, 3U);
    const auto mismatch = received_buffer.select(
        timing(timestamp(device, 1000), timestamp(other_host, 200)));
    check(mismatch.status == "clock_mismatch" && mismatch.priors.empty(),
          "received mode rejects a different host clock identity");

    rbt_m3t::JointPriorBuffer capture_clock_buffer(config("capture"), model(), arms());
    capture_clock_buffer.push(sample(1U, "session-1", 100U, 1000));
    const auto generation_mismatch = capture_clock_buffer.select(
        timing(timestamp(clock(rbt::time::ClockKind::Device, 2U, 1U), 1000),
               timestamp(host, 200)));
    check(generation_mismatch.status == "clock_mismatch",
          "capture mode rejects a different device clock generation");
}

void bounded_buffer_retires_sessions_and_flushes_clock_generations() {
    auto bounded = config("received", 1000U, 2U);
    rbt_m3t::JointPriorBuffer capacity_buffer(bounded, model(), arms());
    capacity_buffer.push(sample(1U, "session-1", 10U, std::nullopt));
    capacity_buffer.push(sample(2U, "session-1", 20U, std::nullopt));
    capacity_buffer.push(sample(3U, "session-1", 30U, std::nullopt));
    check(capacity_buffer.size() == 2U, "joint state buffer stays within configured capacity");

    constexpr auto maximum_sequence = std::numeric_limits<std::uint64_t>::max();
    rbt_m3t::JointPriorBuffer full_uint64_sequence_buffer(bounded, model(), arms());
    full_uint64_sequence_buffer.push(sample(maximum_sequence, "uint64-session", 100U,
                                            std::nullopt));
    const auto host_clock = clock(rbt::time::ClockKind::HostMonotonic, 1U);
    const auto maximum_sequence_report = full_uint64_sequence_buffer.select(
        timing({}, timestamp(host_clock, 200)));
    check(maximum_sequence_report.status == "applied"
              && maximum_sequence_report.sequence == maximum_sequence,
          "buffer preserves the complete uint64 sequence range");

    rbt_m3t::JointPriorBuffer session_buffer(bounded, model(), arms());
    session_buffer.push(sample(8U, "old-session", 10U, std::nullopt));
    session_buffer.push(sample(1U, "new-session", 20U, std::nullopt));
    check(session_buffer.size() == 1U, "new driver session clears old samples and resets sequence");
    expect_invalid([&] {
        session_buffer.push(sample(9U, "old-session", 30U, std::nullopt));
    }, "late samples from a retired session cannot reactivate it");
    check(session_buffer.size() == 1U, "rejected retired session leaves active samples intact");

    rbt_m3t::JointPriorBuffer stale_session_buffer(bounded, model(), arms());
    stale_session_buffer.push(sample(1U, "active-session", 100U, std::nullopt));
    expect_invalid([&] {
        stale_session_buffer.push(sample(1U, "stale-session", 99U, std::nullopt));
    }, "new session with older timestamp on the same clock is rejected");
    stale_session_buffer.push(sample(2U, "active-session", 101U, std::nullopt));
    check(stale_session_buffer.size() == 2U,
          "rejected stale session does not replace the active history");

    rbt_m3t::JointPriorBuffer retired_rollover_buffer(bounded, model(), arms());
    retired_rollover_buffer.push(sample(1U, "session-0", 100U, std::nullopt));
    for (std::uint64_t session = 1U; session <= 9U; ++session) {
        retired_rollover_buffer.push(sample(1U, "session-" + std::to_string(session),
                                            100U + static_cast<std::int64_t>(session),
                                            std::nullopt));
    }
    expect_invalid([&] {
        retired_rollover_buffer.push(sample(2U, "session-0", 108U, std::nullopt));
    }, "old session remains rejected by clock order after retired-session eviction");
    check(retired_rollover_buffer.size() == 1U,
          "retired-session rollover cannot replace the active history with stale data");

    rbt_m3t::JointPriorBuffer controller_buffer(bounded, model(), arms());
    controller_buffer.push(sample(1U, "active-session", 10U, std::nullopt));
    auto wrong_controller = sample(1U, "wrong-controller-session", 20U, std::nullopt);
    wrong_controller.controller_id = "another-controller";
    expect_invalid([&] {
        controller_buffer.push(std::move(wrong_controller));
    }, "sample from an unexpected controller is rejected");
    controller_buffer.push(sample(2U, "active-session", 20U, std::nullopt));
    check(controller_buffer.size() == 2U,
          "wrong controller sample cannot evict the configured controller history");

    rbt_m3t::JointPriorBuffer generation_buffer(bounded, model(), arms());
    generation_buffer.push(sample(1U, "session-1", 10U, std::nullopt, 1U, 0U));
    generation_buffer.push(sample(2U, "session-1", 1U, std::nullopt, 1U, 1U));
    check(generation_buffer.size() == 1U,
          "receive clock generation change flushes old buffered samples");
}

void mappings_are_explicit_and_rejected_per_joint() {
    auto configured = config("received", 1000U);
    configured.mappings = {
        mapping("left_arm", "q1", "left", "shoulder_pan", 1.0),
        mapping("right_arm", "q2", "right", "elbow_flex", 0.01),
        mapping("left_arm", "q5", "left", "wrist_roll", 0.02, 0.1),
        mapping("left_arm", "gripper_joint", "left", "gripper", 0.01, -0.5),
    };
    rbt_m3t::JointPriorBuffer buffer(configured, model(), arms());
    buffer.push(sample(1U, "session-1", 100U, std::nullopt));
    const auto host = clock(rbt::time::ClockKind::HostMonotonic, 1U);
    const auto report = buffer.select(timing({}, timestamp(host, 200)));
    check(report.status == "applied" && report.priors.size() == 3U,
          "one out-of-range mapping is omitted while other mappings remain");
    check(report.rejected_mappings.size() == 1U
              && report.rejected_mappings.front().find("left_arm/q1") != std::string::npos,
          "rejected mapping diagnostics identify only the out-of-range joint");

    const auto q5 = std::find_if(report.priors.begin(), report.priors.end(), [](const auto& prior) {
        return prior.joint == "q5";
    });
    const auto gripper = std::find_if(report.priors.begin(), report.priors.end(),
                                      [](const auto& prior) {
                                          return prior.joint == "gripper_joint";
                                      });
    check(q5 != report.priors.end() && std::abs(q5->angle_rad - 1.1) < 1.0e-12,
          "q5 receives only its explicitly configured affine conversion");
    check(gripper != report.priors.end()
              && std::abs(gripper->angle_rad - 0.1) < 1.0e-12,
          "gripper percent is used only through its explicitly configured affine conversion");

    const auto pi = std::acos(-1.0);
    auto boundary_model = model();
    const auto boundary_joint = std::find_if(
        boundary_model.joints.begin(), boundary_model.joints.end(), [](const auto& joint) {
            return joint.name == "q1";
        });
    boundary_joint->lower = -pi;
    boundary_joint->upper = pi;
    auto boundary_config = config("received", 1000U);
    boundary_config.mappings = {
        mapping("left_arm", "q1", "left", "shoulder_pan", 1.0),
    };
    rbt_m3t::JointPriorBuffer boundary_buffer(boundary_config, boundary_model, arms());
    boundary_buffer.push(sample(1U, "pi-boundary", 100U, std::nullopt,
                                1U, 0U, 2U, 0U, pi));
    const auto boundary_report = boundary_buffer.select(timing(
        {}, timestamp(clock(rbt::time::ClockKind::HostMonotonic, 1U), 200)));
    check(boundary_report.priors.size() == 1U
              && boundary_report.priors.front().angle_rad == pi,
          "exact double URDF boundary is accepted when float conversion rounds outward");

    auto partial = config("received", 1000U);
    partial.mappings = { mapping("left_arm", "q5", "left", "wrist_roll", 0.02) };
    rbt_m3t::JointPriorBuffer partial_buffer(partial, model(), arms());
    partial_buffer.push(sample(1U, "session-1", 100U, std::nullopt));
    const auto partial_report = partial_buffer.select(timing({}, timestamp(host, 200)));
    check(partial_report.priors.size() == 1U && partial_report.priors.front().joint == "q5",
          "unconfigured joints, including gripper, produce no implicit priors");
}

void configuration_validation_rejects_unsafe_mappings() {
    auto invalid_config = config();
    invalid_config.capacity = 513U;
    expect_invalid([&] {
        rbt_m3t::JointPriorBuffer buffer(invalid_config, model(), arms());
        (void)buffer;
    }, "capacity above 512 is rejected");

    invalid_config = config();
    invalid_config.mappings.front().scale_rad_per_unit = 0.0;
    expect_invalid([&] {
        rbt_m3t::JointPriorBuffer buffer(invalid_config, model(), arms());
        (void)buffer;
    }, "zero mapping scale is rejected");

    invalid_config = config();
    invalid_config.mappings.front().joint = "fixed";
    expect_invalid([&] {
        rbt_m3t::JointPriorBuffer buffer(invalid_config, model(), arms());
        (void)buffer;
    }, "fixed model joints are rejected as prior targets");

    invalid_config = config();
    invalid_config.service_name.clear();
    expect_invalid([&] {
        rbt_m3t::JointPriorBuffer buffer(invalid_config, model(), arms());
        (void)buffer;
    }, "enabled joint priors require a service name");

    invalid_config = config();
    invalid_config.controller_id.clear();
    expect_invalid([&] {
        rbt_m3t::JointPriorBuffer buffer(invalid_config, model(), arms());
        (void)buffer;
    }, "enabled joint priors require a controller id");

    invalid_config = config();
    invalid_config.service_name.assign(241U, 's');
    expect_invalid([&] {
        rbt_m3t::JointPriorBuffer buffer(invalid_config, model(), arms());
        (void)buffer;
    }, "service name above 240 bytes is rejected");

    invalid_config = config();
    invalid_config.controller_id.assign(129U, 'c');
    expect_invalid([&] {
        rbt_m3t::JointPriorBuffer buffer(invalid_config, model(), arms());
        (void)buffer;
    }, "controller id above 128 bytes is rejected");

    invalid_config = config();
    invalid_config.maximum_age_ns = 0U;
    expect_invalid([&] {
        rbt_m3t::JointPriorBuffer buffer(invalid_config, model(), arms());
        (void)buffer;
    }, "zero maximum age is rejected");

    invalid_config = config();
    invalid_config.maximum_age_ns = 1'000'000'001U;
    expect_invalid([&] {
        rbt_m3t::JointPriorBuffer buffer(invalid_config, model(), arms());
        (void)buffer;
    }, "maximum age above one second is rejected");

    invalid_config = config();
    invalid_config.mappings.front().standard_deviation_rad = 1.0e-30;
    expect_invalid([&] {
        rbt_m3t::JointPriorBuffer buffer(invalid_config, model(), arms());
        (void)buffer;
    }, "prior precision that overflows float is rejected");

    invalid_config = config();
    invalid_config.mappings.front().standard_deviation_rad = 1.0e30;
    expect_invalid([&] {
        rbt_m3t::JointPriorBuffer buffer(invalid_config, model(), arms());
        (void)buffer;
    }, "prior precision that underflows float is rejected");

    invalid_config = config();
    invalid_config.mappings.front().standard_deviation_rad = 1.0e-19;
    expect_invalid([&] {
        rbt_m3t::JointPriorBuffer buffer(invalid_config, model(), arms());
        (void)buffer;
    }, "worst joint-span weighted residual above float range is rejected");

    rbt_m3t::JointPriorBuffer disabled_buffer(rbt_m3t::JointPriorConfig {}, model(), arms());
    const auto disabled = disabled_buffer.select(timing(
        timestamp(clock(rbt::time::ClockKind::Device, 2U), 1),
        timestamp(clock(rbt::time::ClockKind::HostMonotonic, 1U), 1)));
    check(disabled.status == "disabled",
          "default disabled config remains valid without service or mappings");
}

} // namespace

int main() {
    decoder_copies_and_validates_the_native_sample();
    selection_is_causal_and_accepts_the_exact_age_boundary();
    selection_requires_the_configured_clock_basis_without_fallback();
    bounded_buffer_retires_sessions_and_flushes_clock_generations();
    mappings_are_explicit_and_rejected_per_joint();
    configuration_validation_rejects_unsafe_mappings();
    if (failures != 0) {
        std::cerr << failures << " joint prior check(s) failed\n";
        return 1;
    }
    std::cout << "All joint prior checks passed\n";
    return 0;
}

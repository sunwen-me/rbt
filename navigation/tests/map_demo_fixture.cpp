#include <rbt_navigation/map/grid_map.hpp>
#include <rbt_navigation/map/map_io.hpp>
#include <rbt_navigation/runtime/native.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using namespace rbt::navigation;
namespace native = rbt::navigation::native;
namespace wire = rbt::navwire;
namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t idle_state = static_cast<std::uint32_t>(NavigationState::Idle);
constexpr std::uint32_t replanning_state = static_cast<std::uint32_t>(NavigationState::Replanning);
constexpr std::uint32_t succeeded_state = static_cast<std::uint32_t>(NavigationState::Succeeded);
constexpr std::uint32_t canceled_state = static_cast<std::uint32_t>(NavigationState::Canceled);
constexpr std::uint32_t failed_state = static_cast<std::uint32_t>(NavigationState::Failed);
constexpr TimePoint maximum_input_age_ns = 250'000'000;
constexpr double robot_radius = 0.18;

struct Arguments {
    fs::path map_file;
    std::string prefix;
    std::string map_tf_id;
    std::string odom_tf_id;
    double goal_x{};
    double goal_y{};
    double goal_yaw{};
    double goal_timeout_seconds{90.0};
    fs::path output_file;
};

struct Summary {
    bool passed{};
    std::string error;
    std::uint64_t map_version{};
    std::uint64_t base_state_frames{};
    std::uint64_t guard_state_frames{};
    std::uint64_t command_frames{};
    std::uint64_t status_frames{};
    std::uint64_t path_publications{};
    std::uint64_t path_point_count{};
    std::uint64_t max_path_point_count{};
    std::uint64_t nav_replanning_count{};
    std::uint64_t map_tf_publish_drops{};
    std::uint64_t base_tf_publish_drops{};
    std::uint64_t watchdog_faults_after_warmup{};
    std::uint64_t estop_faults_after_warmup{};
    std::uint64_t base_zero_frames_after_success{};
    std::uint64_t final_zero_frames_after_success{};
    double executed_path_length_m{};
    double min_conservative_clearance_m{std::numeric_limits<double>::infinity()};
    double elapsed_seconds{};
    double goal_elapsed_seconds{};
    bool moving_final_command_seen{};
    bool moving_base_state_seen{};
    bool all_base_footprints_free{true};
    bool all_base_segments_free{true};
    bool all_path_footprints_free{true};
    bool all_path_segments_free{true};
    std::optional<Pose2D> first_base_pose;
    std::optional<Pose2D> last_base_pose;
    std::optional<Pose2D> first_path_pose;
    std::optional<Pose2D> last_path_pose;
};

struct Observed {
    wire::BaseState base{};
    wire::GuardState guard{};
    wire::NavigationStatus status{};
    std::uint64_t last_base_sequence{};
    std::uint64_t last_guard_sequence{};
    std::uint64_t last_command_sequence{};
    TimePoint last_status_stamp{-1};
    std::optional<std::uint32_t> last_status_state;
    std::optional<Pose2D> previous_base_pose;
    std::uint64_t base_zero_streak{};
    std::uint64_t final_zero_streak{};
    bool have_base{};
    bool have_guard{};
    bool goal_acknowledged{};
    bool goal_succeeded{};
    bool path_seen{};
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

std::string normalizePrefix(std::string prefix) {
    while (!prefix.empty() && prefix.front() == '/') prefix.erase(prefix.begin());
    while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
    if (prefix.empty() || prefix.find("//") != std::string::npos
        || prefix == "." || prefix == ".." || prefix.find("/../") != std::string::npos
        || prefix.starts_with("../") || prefix.ends_with("/..")) {
        fail("--prefix must be a nonempty topic prefix without empty or parent components");
    }
    return prefix;
}

double parseFiniteDouble(const std::string& value, const std::string& key) {
    std::size_t consumed = 0;
    double parsed{};
    try {
        parsed = std::stod(value, &consumed);
    } catch (const std::exception&) {
        fail("invalid number for --" + key + ": " + value);
    }
    if (consumed != value.size() || !std::isfinite(parsed)) {
        fail("invalid finite number for --" + key + ": " + value);
    }
    return parsed;
}

Arguments parseArguments(int argc, char** argv) {
    const std::map<std::string, bool, std::less<>> allowed{
        {"map", true}, {"prefix", true}, {"tf-map", true}, {"tf-odom", true},
        {"goal-x", true}, {"goal-y", true}, {"goal-yaw", true},
        {"goal-timeout", true}, {"output", true}};
    std::map<std::string, std::string, std::less<>> values;
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (!argument.starts_with("--") || argument.size() <= 2) {
            fail("unexpected positional argument: " + argument);
        }
        const auto equal = argument.find('=');
        const std::string key = argument.substr(2, equal == std::string::npos
            ? std::string::npos : equal - 2);
        if (!allowed.contains(key)) fail("unknown option: --" + key);

        std::string value;
        if (equal != std::string::npos) {
            value = argument.substr(equal + 1);
        } else {
            if (index + 1 >= argc || std::string_view(argv[index + 1]).starts_with("--")) {
                fail("missing value for --" + key);
            }
            value = argv[++index];
        }
        if (value.empty()) fail("empty value for --" + key);
        if (!values.emplace(key, std::move(value)).second) fail("duplicate option: --" + key);
    }

    for (const auto* key : {"map", "prefix", "tf-map", "tf-odom", "goal-x", "goal-y",
                            "goal-yaw", "output"}) {
        if (!values.contains(key)) fail("missing required option: --" + std::string(key));
    }

    Arguments result;
    result.map_file = values.at("map");
    result.prefix = normalizePrefix(values.at("prefix"));
    result.map_tf_id = values.at("tf-map");
    result.odom_tf_id = values.at("tf-odom");
    result.goal_x = parseFiniteDouble(values.at("goal-x"), "goal-x");
    result.goal_y = parseFiniteDouble(values.at("goal-y"), "goal-y");
    result.goal_yaw = parseFiniteDouble(values.at("goal-yaw"), "goal-yaw");
    if (const auto found = values.find("goal-timeout"); found != values.end()) {
        result.goal_timeout_seconds = parseFiniteDouble(found->second, "goal-timeout");
    }
    result.output_file = values.at("output");
    if (result.map_tf_id == result.odom_tf_id) fail("--tf-map and --tf-odom must be distinct UUIDs");
    if (!(result.goal_timeout_seconds > 0.0 && result.goal_timeout_seconds <= 86'400.0)) {
        fail("--goal-timeout must be greater than 0 and at most 86400 seconds");
    }
    if (result.map_file.empty() || result.output_file.empty()) fail("map and output paths must not be empty");
    return result;
}

void validatePaths(const Arguments& args) {
    if (!fs::is_regular_file(args.map_file)) {
        fail("--map is not a readable regular file: " + args.map_file.string());
    }
    std::error_code equivalent_error;
    if (fs::exists(args.output_file)
        && fs::equivalent(args.map_file, args.output_file, equivalent_error)
        && !equivalent_error) {
        fail("--output must not overwrite the occupancy-map YAML");
    }
}

bool finite(const wire::BaseState& state) {
    return std::isfinite(state.x) && std::isfinite(state.y) && std::isfinite(state.yaw)
        && std::isfinite(state.linear_x) && std::isfinite(state.angular_z);
}

bool finite(const wire::PathPoint& point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.yaw);
}

bool nearZero(const wire::BaseState& state) {
    return std::abs(state.linear_x) < 0.02 && std::abs(state.angular_z) < 0.03;
}

bool moving(const wire::BaseState& state) {
    return std::abs(state.linear_x) + std::abs(state.angular_z) > 0.05;
}

bool moving(const wire::VelocityCommand& command) {
    return std::abs(command.linear_x) + std::abs(command.angular_z) > 0.05;
}

bool zero(const wire::VelocityCommand& command) {
    return std::abs(command.linear_x) <= 1e-6 && std::abs(command.angular_z) <= 1e-6;
}

bool fixedFrameIsMap(const std::uint8_t* frame) {
    return std::memcmp(frame, "map\0", 4) == 0
        && std::all_of(frame + 4, frame + 64, [](std::uint8_t byte) { return byte == 0; });
}

void publishMapTransform(rbt::tf::DynamicTfPublisher& publisher,
                         const rbt::time::HostClock& clock,
                         Summary& summary) {
    const auto stamp = native::now(clock);
    try {
        publisher.publish({rbt::tf::FrameId("map"), rbt::tf::FrameId("odom"), stamp,
                           0U, 1U,
                           rbt::tf::Transform3(rbt::tf::Vec3(0.0, 0.0, 0.0),
                                               rbt::tf::Quaternion(0.0, 0.0, 0.0, 1.0))});
    } catch (const rbt::tf::TfTransportError& error) {
        if (!error.stream_error() || *error.stream_error() != rbt::StreamError::QueueFull) throw;
        ++summary.map_tf_publish_drops;
    }
}

void publishBaseTransform(rbt::tf::DynamicTfPublisher& publisher,
                          const wire::BaseState& state,
                          const rbt::time::Timestamp& current,
                          TimePoint stamp_ns,
                          Summary& summary) {
    const Pose2D pose{state.x, state.y, state.yaw};
    const auto transform = rbt::tf::Transform3(
        rbt::tf::Vec3(pose.x, pose.y, 0.0),
        rbt::tf::Quaternion(0.0, 0.0, std::sin(pose.yaw / 2.0), std::cos(pose.yaw / 2.0)));
    const rbt::time::Timestamp original_stamp{current.clock, stamp_ns};
    try {
        publisher.publish({rbt::tf::FrameId("odom"), rbt::tf::FrameId("base_link"),
                           original_stamp, 0U, 1U, transform});
    } catch (const rbt::tf::TfTransportError& error) {
        if (!error.stream_error() || *error.stream_error() != rbt::StreamError::QueueFull) throw;
        ++summary.base_tf_publish_drops;
    }
}

void updateBase(const wire::BaseState& state,
                const rbt::time::HostClock& clock,
                const GridMap& grid,
                rbt::tf::DynamicTfPublisher& base_tf,
                Observed& observed,
                Summary& summary,
                bool warmup_complete) {
    const auto current = native::now(clock);
    const auto stamp_ns = native::decodeStamp(state.stamp, clock.monotonic_identity());
    if (state.sequence == 0 || state.sequence <= observed.last_base_sequence) {
        fail("base state sequence did not increase");
    }
    if (!fresh(stamp_ns, current.ns, maximum_input_age_ns)) {
        fail("base state stamp was stale or future-dated");
    }
    if (!finite(state)) fail("base state contained nonfinite pose or velocity");
    if (state.watchdog_expired > 1U || state.emergency_stop > 1U) {
        fail("base state contained invalid safety flags");
    }
    observed.last_base_sequence = state.sequence;
    observed.base = state;
    observed.have_base = true;
    ++summary.base_state_frames;
    if (warmup_complete) {
        if (state.watchdog_expired != 0U) {
            ++summary.watchdog_faults_after_warmup;
            fail("base watchdog fault after warmup");
        }
        if (state.emergency_stop != 0U) {
            ++summary.estop_faults_after_warmup;
            fail("base emergency stop fault after warmup");
        }
    }

    const Pose2D pose{state.x, state.y, state.yaw};
    if (!grid.isFree(pose.x, pose.y, robot_radius)) {
        summary.all_base_footprints_free = false;
        fail("base pose footprint was not collision-free in the YAML map");
    }
    const double clearance = grid.getDistanceToObstacle(pose.x, pose.y) - robot_radius;
    if (!std::isfinite(clearance)) fail("base pose had nonfinite conservative clearance");
    summary.min_conservative_clearance_m = std::min(summary.min_conservative_clearance_m, clearance);
    if (observed.previous_base_pose) {
        const double distance_m = distance(*observed.previous_base_pose, pose);
        if (!std::isfinite(distance_m)) fail("base executed path length became nonfinite");
        summary.executed_path_length_m += distance_m;
        if (!grid.segmentFree(*observed.previous_base_pose, pose, robot_radius)) {
            summary.all_base_segments_free = false;
            fail("consecutive base poses crossed an unsafe map segment");
        }
    } else {
        summary.first_base_pose = pose;
    }
    observed.previous_base_pose = pose;
    summary.last_base_pose = pose;

    if (moving(state)) summary.moving_base_state_seen = true;
    if (observed.goal_succeeded) {
        if (nearZero(state)) {
            ++observed.base_zero_streak;
        } else {
            observed.base_zero_streak = 0;
        }
        summary.base_zero_frames_after_success = observed.base_zero_streak;
    }

    publishBaseTransform(base_tf, state, current, stamp_ns, summary);
}

void updateGuard(const wire::GuardState& state,
                 const rbt::time::HostClock& clock,
                 Observed& observed,
                 Summary& summary,
                 bool warmup_complete) {
    const auto current = native::now(clock);
    const auto stamp_ns = native::decodeStamp(state.stamp, clock.monotonic_identity());
    if (state.sequence == 0 || state.sequence <= observed.last_guard_sequence) {
        fail("guard state sequence did not increase");
    }
    if (!fresh(stamp_ns, current.ns, maximum_input_age_ns)) {
        fail("guard state stamp was stale or future-dated");
    }
    if (state.watchdog_expired > 1U || state.emergency_stop > 1U) {
        fail("guard state contained invalid safety flags");
    }
    if (state.allowed_source != 1U || state.output_source != 100U) {
        fail("guard state reported unexpected command authority");
    }
    observed.last_guard_sequence = state.sequence;
    observed.guard = state;
    observed.have_guard = true;
    ++summary.guard_state_frames;
    if (warmup_complete) {
        if (state.watchdog_expired != 0U) {
            ++summary.watchdog_faults_after_warmup;
            fail("guard watchdog fault after warmup");
        }
        if (state.emergency_stop != 0U) {
            ++summary.estop_faults_after_warmup;
            fail("guard emergency stop fault after warmup");
        }
    }
}

void updateStatus(const wire::NavigationStatus& status,
                  const rbt::time::HostClock& clock,
                  Observed& observed,
                  Summary& summary) {
    const auto current = native::now(clock);
    const auto stamp_ns = native::decodeStamp(status.stamp, clock.monotonic_identity());
    if (!fresh(stamp_ns, current.ns, 2 * second) || stamp_ns <= observed.last_status_stamp) {
        fail("navigation status stamp was stale, future-dated, or did not advance");
    }
    if (status.state > failed_state) fail("navigation status used an unknown state");
    observed.last_status_stamp = stamp_ns;
    if (observed.last_status_state && status.state == replanning_state
        && *observed.last_status_state != replanning_state) {
        ++summary.nav_replanning_count;
    }
    observed.last_status_state = status.state;
    observed.status = status;
    ++summary.status_frames;
    summary.map_version = std::max(summary.map_version, status.map_version);

    if (status.goal_id == 1U && status.state != idle_state) observed.goal_acknowledged = true;
    if (status.goal_id == 1U && status.state == failed_state) {
        fail("navigation failed with error=" + std::to_string(status.error));
    }
    if (status.goal_id == 1U && status.state == canceled_state) {
        fail("navigation canceled goal 1 with error=" + std::to_string(status.error));
    }
    if (status.goal_id == 1U && status.state == succeeded_state) observed.goal_succeeded = true;
}

void updateCommand(const wire::VelocityCommand& command,
                   const rbt::time::HostClock& clock,
                   Observed& observed,
                   Summary& summary,
                   bool warmup_complete) {
    const auto current = native::now(clock);
    const auto stamp_ns = native::decodeStamp(command.stamp, clock.monotonic_identity());
    if (!fresh(stamp_ns, current.ns, maximum_input_age_ns)) {
        fail("final cmd_vel stamp was stale or future-dated");
    }
    if (command.sequence == 0 || command.sequence <= observed.last_command_sequence) {
        fail("final cmd_vel sequence did not increase");
    }
    if (command.source_id != 100U) fail("final cmd_vel source was not guard source 100");
    if (!std::isfinite(command.linear_x) || !std::isfinite(command.angular_z)
        || std::abs(command.linear_x) > 0.7 + 1e-9
        || std::abs(command.angular_z) > 1.8 + 1e-9) {
        fail("final cmd_vel was nonfinite or exceeded speed bounds");
    }
    if (command.valid_until_ns <= stamp_ns
        || command.valid_until_ns - stamp_ns > 100'000'000) {
        fail("final cmd_vel TTL was not positive and at most 100 ms");
    }
    if (command.valid_until_ns <= current.ns) fail("final cmd_vel command was already expired");
    observed.last_command_sequence = command.sequence;
    ++summary.command_frames;
    if (moving(command)) summary.moving_final_command_seen = true;
    if (warmup_complete && observed.goal_succeeded) {
        if (zero(command)) {
            ++observed.final_zero_streak;
        } else {
            observed.final_zero_streak = 0;
        }
        summary.final_zero_frames_after_success = observed.final_zero_streak;
    }
}

void updatePaths(rbt::SliceSubscriber<wire::PathHeader, wire::PathPoint>& subscriber,
                 const GridMap& grid,
                 const rbt::time::HostClock& clock,
                 Summary& summary,
                 Observed& observed) {
    for (std::size_t count = 0; count < rbt::MAX_DRAIN_PER_SOURCE; ++count) {
        auto received = native::take(subscriber.receive());
        if (!received) break;
        const auto& header = received->header();
        if (header.goal_id != 1U) continue;
        const auto current = native::now(clock);
        const auto stamp_ns = native::decodeStamp(header.stamp, clock.monotonic_identity());
        if (!fresh(stamp_ns, current.ns, 2 * second)) fail("goal path stamp was stale or future-dated");
        if (!fixedFrameIsMap(header.frame)) fail("goal path frame was not map");
        const auto points = received->payload();
        if (points.empty()) continue;
        if (points.size() > native::max_path_points) fail("goal path exceeded native point limit");
        if (header.map_version == 0U) fail("goal path had map version zero");

        ++summary.path_publications;
        summary.path_point_count += static_cast<std::uint64_t>(points.size());
        summary.max_path_point_count = std::max(summary.max_path_point_count,
                                                static_cast<std::uint64_t>(points.size()));
        observed.path_seen = true;
        std::optional<Pose2D> previous;
        for (const auto& point : points) {
            if (!finite(point)) fail("goal path contained nonfinite coordinates");
            const Pose2D pose{point.x, point.y, point.yaw};
            if (!grid.isFree(pose.x, pose.y, robot_radius)) {
                summary.all_path_footprints_free = false;
                fail("A* path pose footprint was not collision-free in the YAML map");
            }
            const double clearance = grid.getDistanceToObstacle(pose.x, pose.y) - robot_radius;
            if (!std::isfinite(clearance)) fail("path pose had nonfinite conservative clearance");
            summary.min_conservative_clearance_m =
                std::min(summary.min_conservative_clearance_m, clearance);
            if (previous && !grid.segmentFree(*previous, pose, robot_radius)) {
                summary.all_path_segments_free = false;
                fail("A* path crossed an unsafe map segment");
            }
            if (!summary.first_path_pose) summary.first_path_pose = pose;
            summary.last_path_pose = pose;
            previous = pose;
        }
    }
}

std::string jsonEscape(std::string_view value) {
    std::ostringstream escaped;
    for (const unsigned char character : value) {
        switch (character) {
        case '"': escaped << "\\\""; break;
        case '\\': escaped << "\\\\"; break;
        case '\b': escaped << "\\b"; break;
        case '\f': escaped << "\\f"; break;
        case '\n': escaped << "\\n"; break;
        case '\r': escaped << "\\r"; break;
        case '\t': escaped << "\\t"; break;
        default:
            if (character < 0x20U) {
                escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<unsigned>(character) << std::dec;
            } else {
                escaped << character;
            }
        }
    }
    return escaped.str();
}

void writePose(std::ostream& output, const std::optional<Pose2D>& pose) {
    if (!pose) {
        output << "null";
        return;
    }
    output << "{\"x\":" << pose->x << ",\"y\":" << pose->y
           << ",\"yaw\":" << pose->yaw << '}';
}

void writeSummary(const fs::path& output_file, const Arguments& args, const Summary& summary) {
    const auto parent = output_file.parent_path();
    if (!parent.empty()) fs::create_directories(parent);
    std::ofstream output(output_file, std::ios::out | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot open summary output: " + output_file.string());
    output << std::setprecision(17)
           << "{\n  \"passed\": " << (summary.passed ? "true" : "false")
           << ",\n  \"error\": \"" << jsonEscape(summary.error) << "\""
           << ",\n  \"map_yaml\": \"" << jsonEscape(args.map_file.string()) << "\""
           << ",\n  \"prefix\": \"" << jsonEscape(args.prefix) << "\""
           << ",\n  \"goal\": {\"x\": " << args.goal_x << ", \"y\": " << args.goal_y
           << ", \"yaw\": " << args.goal_yaw << ", \"timeout_seconds\": "
           << args.goal_timeout_seconds << '}'
           << ",\n  \"map_version\": " << summary.map_version
           << ",\n  \"tf_epoch\": 1"
           << ",\n  \"path_publications\": " << summary.path_publications
           << ",\n  \"path_point_count\": " << summary.path_point_count
           << ",\n  \"max_path_point_count\": " << summary.max_path_point_count
           << ",\n  \"nav_replanning_count\": " << summary.nav_replanning_count
           << ",\n  \"base_state_frames\": " << summary.base_state_frames
           << ",\n  \"guard_state_frames\": " << summary.guard_state_frames
           << ",\n  \"final_cmd_vel_frames\": " << summary.command_frames
           << ",\n  \"status_frames\": " << summary.status_frames
           << ",\n  \"tf_publish_drops\": {\"map_to_odom\": " << summary.map_tf_publish_drops
           << ", \"odom_to_base_link\": " << summary.base_tf_publish_drops << '}'
           << ",\n  \"executed_path_length_m\": " << summary.executed_path_length_m
           << ",\n  \"min_conservative_clearance_m\": ";
    if (std::isfinite(summary.min_conservative_clearance_m)) {
        output << summary.min_conservative_clearance_m;
    } else {
        output << "null";
    }
    output << ",\n  \"first_base_pose\": ";
    writePose(output, summary.first_base_pose);
    output << ",\n  \"last_base_pose\": ";
    writePose(output, summary.last_base_pose);
    output << ",\n  \"first_path_pose\": ";
    writePose(output, summary.first_path_pose);
    output << ",\n  \"last_path_pose\": ";
    writePose(output, summary.last_path_pose);
    output << ",\n  \"moving_final_command_seen\": "
           << (summary.moving_final_command_seen ? "true" : "false")
           << ",\n  \"moving_base_state_seen\": "
           << (summary.moving_base_state_seen ? "true" : "false")
           << ",\n  \"all_base_footprints_free\": "
           << (summary.all_base_footprints_free ? "true" : "false")
           << ",\n  \"all_base_segments_free\": "
           << (summary.all_base_segments_free ? "true" : "false")
           << ",\n  \"all_path_footprints_free\": "
           << (summary.all_path_footprints_free ? "true" : "false")
           << ",\n  \"all_path_segments_free\": "
           << (summary.all_path_segments_free ? "true" : "false")
           << ",\n  \"watchdog_faults_after_warmup\": " << summary.watchdog_faults_after_warmup
           << ",\n  \"estop_faults_after_warmup\": " << summary.estop_faults_after_warmup
           << ",\n  \"base_zero_frames_after_success\": " << summary.base_zero_frames_after_success
           << ",\n  \"final_zero_frames_after_success\": " << summary.final_zero_frames_after_success
           << ",\n  \"elapsed_seconds\": " << summary.elapsed_seconds
           << ",\n  \"goal_elapsed_seconds\": " << summary.goal_elapsed_seconds << "\n}\n";
    output.flush();
    if (!output) throw std::runtime_error("failed writing summary output: " + output_file.string());
}

void execute(const Arguments& args, Summary& summary, std::chrono::steady_clock::time_point started) {
    const auto occupancy = loadOccupancyMap(args.map_file);
    const GridMap grid(occupancy);
    auto clock = native::hostClock();
    auto map_tf = rbt::tf::DynamicTfPublisher::open(args.map_tf_id);
    auto base_tf = rbt::tf::DynamicTfPublisher::open(args.odom_tf_id);
    auto goals = native::publisher<wire::GoalCommand>(args.prefix, "goal");
    auto base_subscriber = native::subscriber<wire::BaseState>(args.prefix, "base_state");
    auto status_subscriber = native::subscriber<wire::NavigationStatus>(args.prefix, "status");
    auto guard_subscriber = native::subscriberAt<wire::GuardState>(args.prefix + "/guard_state");
    const auto final_topic = "/" + args.prefix + "/cmd_vel";
    auto command_subscriber = native::subscriberAt<wire::VelocityCommand>(final_topic);
    auto path_subscriber = native::take(rbt::SliceSubscriber<wire::PathHeader, wire::PathPoint>::create(
        native::stream(args.prefix, "path", wire::PLANNED_PATH_DESCRIPTOR),
        wire::PLANNED_PATH_DESCRIPTOR));

    std::cout << "map demo fixture ready: prefix=" << args.prefix
              << " final_cmd_vel_topic=" << final_topic << std::endl;

    Observed observed;
    const auto startup_deadline = started + std::chrono::seconds(8);
    const auto wall_budget = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(args.goal_timeout_seconds + 10.0));
    const auto wall_deadline = started + wall_budget;
    const auto timeout_ns = static_cast<TimePoint>(std::llround(
        args.goal_timeout_seconds * static_cast<double>(second)));
    bool warmup_complete = false;
    auto goal_started = std::chrono::steady_clock::time_point{};
    auto next_goal_send = std::chrono::steady_clock::time_point::min();
    auto next_map_tf = std::chrono::steady_clock::time_point::min();

    while (true) {
        const auto loop_started = std::chrono::steady_clock::now();
        if (loop_started >= wall_deadline) fail("fixture wall-clock budget expired");
        if (!warmup_complete && loop_started >= startup_deadline) {
            fail("runtime did not provide a mapped base and guard state within 8 seconds");
        }

        if (loop_started >= next_map_tf) {
            publishMapTransform(map_tf, clock, summary);
            next_map_tf = loop_started + std::chrono::milliseconds(8);
        }

        native::drain(base_subscriber, [&](const wire::BaseState& state) {
            updateBase(state, clock, grid, base_tf, observed, summary, warmup_complete);
        });
        native::drain(status_subscriber, [&](const wire::NavigationStatus& status) {
            updateStatus(status, clock, observed, summary);
        });
        native::drain(guard_subscriber, [&](const wire::GuardState& state) {
            updateGuard(state, clock, observed, summary, warmup_complete);
        });
        native::drain(command_subscriber, [&](const wire::VelocityCommand& command) {
            updateCommand(command, clock, observed, summary, warmup_complete);
        });
        updatePaths(path_subscriber, grid, clock, summary, observed);

        const auto now_wall = std::chrono::steady_clock::now();
        const bool map_ready = observed.status.map_version != 0U;
        if (!warmup_complete && now_wall - started >= std::chrono::seconds(1)
            && map_ready && observed.have_base && observed.have_guard) {
            warmup_complete = true;
            summary.map_version = observed.status.map_version;
            goal_started = now_wall;
            next_goal_send = now_wall;
        }

        if (warmup_complete && !observed.goal_acknowledged && now_wall >= next_goal_send) {
            const auto current = native::now(clock);
            native::publish(goals, wire::GoalCommand{native::encodeStamp(current), 1U, 0U, 0U,
                args.goal_x, args.goal_y, args.goal_yaw, timeout_ns});
            next_goal_send = now_wall + std::chrono::milliseconds(100);
        }

        if (observed.goal_succeeded && observed.base_zero_streak >= 5U
            && observed.final_zero_streak >= 3U) {
            if (!summary.moving_final_command_seen || !summary.moving_base_state_seen) {
                fail("successful goal did not include observed movement in base state and final cmd_vel");
            }
            if (!observed.have_base || !nearZero(observed.base)) {
                fail("final base state was not stopped after navigation success");
            }
            if (std::hypot(observed.base.x - args.goal_x, observed.base.y - args.goal_y) >= 0.12
                || std::abs(normalizeAngle(observed.base.yaw - args.goal_yaw)) >= 0.16) {
                fail("successful goal ended outside the requested pose tolerance");
            }
            if (observed.path_seen && summary.path_point_count != 0U) {
                summary.passed = true;
                summary.goal_elapsed_seconds = std::chrono::duration<double>(now_wall - goal_started).count();
                break;
            }
        }

        if (native::interrupted) fail("fixture interrupted before goal completion");
        std::this_thread::sleep_until(loop_started + std::chrono::milliseconds(5));
    }

    summary.elapsed_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();
}

void printMetrics(const Summary& summary) {
    std::cout << std::setprecision(4)
              << "base_frames=" << summary.base_state_frames
              << " guard_frames=" << summary.guard_state_frames
              << " final_cmd_vel_frames=" << summary.command_frames
              << " status_frames=" << summary.status_frames
              << " path_publications=" << summary.path_publications
              << " path_points=" << summary.path_point_count
              << " executed_path_length_m=" << summary.executed_path_length_m
              << " min_conservative_clearance_m=";
    if (std::isfinite(summary.min_conservative_clearance_m)) {
        std::cout << summary.min_conservative_clearance_m;
    } else {
        std::cout << "n/a";
    }
    std::cout << " elapsed_seconds=" << summary.elapsed_seconds
              << " goal_elapsed_seconds=" << summary.goal_elapsed_seconds
              << " tf_publish_drops=" << (summary.map_tf_publish_drops + summary.base_tf_publish_drops)
              << " nav_replanning_count=" << summary.nav_replanning_count << '\n';
}

} // namespace

int main(int argc, char** argv) {
    Summary summary;
    Arguments args;
    bool have_args = false;
    const auto started = std::chrono::steady_clock::now();
    try {
        args = parseArguments(argc, argv);
        have_args = true;
        validatePaths(args);
        native::installSignals();
        execute(args, summary, started);
        summary.elapsed_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        if (!summary.passed) fail("fixture ended without satisfying success conditions");
        writeSummary(args.output_file, args, summary);
        printMetrics(summary);
        std::cout << "PASS map demo fixture\n";
        return 0;
    } catch (const std::exception& error) {
        summary.passed = false;
        summary.error = error.what();
        summary.elapsed_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        std::cerr << "FAIL map demo fixture: " << summary.error << '\n';
        printMetrics(summary);
        if (have_args) {
            try {
                writeSummary(args.output_file, args, summary);
            } catch (const std::exception& output_error) {
                std::cerr << "could not write failure summary: " << output_error.what() << '\n';
            }
        }
        return 1;
    }
}

#include <rbt_navigation/runtime/native.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace rbt::navigation;
namespace native = rbt::navigation::native;
namespace wire = rbt::navwire;
namespace fs = std::filesystem;

namespace {
constexpr TimePoint nanoseconds_per_second = 1'000'000'000;
constexpr std::uint32_t maximum_navigation_state = 6;

bool retryable(rbt::StreamError error) {
    return error == rbt::StreamError::ReadinessOpen || error == rbt::StreamError::Transport;
}

template <typename Header, typename Element>
bool openSlice(std::optional<rbt::SliceSubscriber<Header, Element>>& subscriber,
               const std::string& prefix, std::string_view suffix,
               const rbt::MessageTypeDescriptor& descriptor, const char* name) {
    if (subscriber) return true;
    auto result = rbt::SliceSubscriber<Header, Element>::create(
        native::stream(prefix, suffix, descriptor), descriptor);
    if (!result) {
        if (retryable(result.error())) return false;
        throw std::runtime_error(std::string(name) + " subscriber open: "
                                 + std::string(rbt::to_string(result.error())));
    }
    subscriber.emplace(std::move(result).value());
    return true;
}

bool finite(const wire::PathPoint& point) {
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.yaw);
}

bool finite(const wire::BaseState& state) {
    return std::isfinite(state.x) && std::isfinite(state.y) && std::isfinite(state.yaw)
        && std::isfinite(state.linear_x) && std::isfinite(state.angular_z);
}

void writeMap(const fs::path& destination, const wire::MapHeader& header,
              std::span<const std::int8_t> cells) {
    if (header.width == 0 || header.height == 0
        || header.width > native::max_cells / header.height
        || cells.size() != static_cast<std::size_t>(header.width) * header.height
        || !std::isfinite(header.resolution) || header.resolution <= 0
        || !std::isfinite(header.origin_x) || !std::isfinite(header.origin_y)
        || !std::isfinite(header.origin_yaw)) {
        throw std::invalid_argument("invalid occupancy map header or payload");
    }

    auto temporary = destination;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::out | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create map.json temporary output");
    output << std::setprecision(17)
           << "{\"stamp_source_id\":[";
    for (std::size_t index = 0; index < std::size(header.stamp.source_id); ++index) {
        if (index != 0) output << ',';
        output << static_cast<unsigned>(header.stamp.source_id[index]);
    }
    output << "],\"stamp_generation\":" << header.stamp.generation
           << ",\"stamp_ns\":" << header.stamp.nanoseconds
           << ",\"stamp_kind\":" << header.stamp.kind
           << ",\"stamp_reserved\":" << header.stamp.reserved
           << ",\"version\":" << header.version
           << ",\"width\":" << header.width
           << ",\"height\":" << header.height
           << ",\"resolution\":" << header.resolution
           << ",\"origin_x\":" << header.origin_x
           << ",\"origin_y\":" << header.origin_y
           << ",\"origin_yaw\":" << header.origin_yaw
           << ",\"frame_bytes\":[";
    for (std::size_t index = 0; index < std::size(header.frame); ++index) {
        if (index != 0) output << ',';
        output << static_cast<unsigned>(header.frame[index]);
    }
    output << "],\"cells\":[";
    for (std::size_t index = 0; index < cells.size(); ++index) {
        if (index != 0) output << ',';
        output << static_cast<int>(cells[index]);
    }
    output << "]}\n";
    output.flush();
    if (!output) throw std::runtime_error("failed writing map.json");
    output.close();
    std::error_code error;
    fs::rename(temporary, destination, error);
    if (error) throw std::runtime_error("cannot publish map.json: " + error.message());
}
} // namespace

int main(int argc, char** argv) try {
    native::Options options(argc, argv);
    native::installSignals();
    const auto prefix = options.get("prefix", "rbt/navigation/demo");
    const fs::path output_directory(options.get("output", "navigation-telemetry"));
    const double duration_seconds = options.number("duration", 30.0);
    if (prefix.empty() || output_directory.empty() || !std::isfinite(duration_seconds)
        || duration_seconds < 0 || duration_seconds > 86'400) {
        throw std::invalid_argument("invalid telemetry recorder prefix, output directory, or duration");
    }
    const auto command_topic = options.get("cmd-vel-topic", "/" + prefix + "/cmd_vel");
    if (command_topic.empty()) throw std::invalid_argument("--cmd-vel-topic must not be empty");

    fs::create_directories(output_directory);
    const auto telemetry_path = output_directory / "telemetry.csv";
    const auto path_path = output_directory / "path.csv";
    const auto map_path = output_directory / "map.json";
    std::error_code remove_error;
    fs::remove(map_path, remove_error);
    if (remove_error) throw std::runtime_error("cannot clear previous map.json: " + remove_error.message());
    auto map_temporary = map_path;
    map_temporary += ".tmp";
    fs::remove(map_temporary, remove_error);
    if (remove_error) throw std::runtime_error("cannot clear previous map.json.tmp: " + remove_error.message());

    std::ofstream telemetry(telemetry_path, std::ios::out | std::ios::trunc);
    std::ofstream path_output(path_path, std::ios::out | std::ios::trunc);
    if (!telemetry || !path_output) throw std::runtime_error("cannot create telemetry outputs");
    telemetry << "relative_stamp_seconds,x,y,yaw,linear_x,angular_z,basewatchdog,baseestop,"
                 "guardwatchdog,guardestop,nav_state,nav_error,goal_id\n";
    path_output << "relative_stamp_seconds,x,y,yaw\n";
    telemetry.flush();
    path_output.flush();
    if (!telemetry || !path_output) throw std::runtime_error("cannot initialize telemetry outputs");

    auto clock = native::hostClock();
    native::FixedInput<wire::BaseState> base_input(prefix, "base_state");
    native::FixedInput<wire::GuardState> guard_input(prefix, "guard_state");
    native::FixedInput<wire::NavigationStatus> status_input(prefix, "status");
    native::FixedInput<wire::VelocityCommand> command_input(command_topic);
    std::optional<rbt::SliceSubscriber<wire::PathHeader, wire::PathPoint>> path_input;
    std::optional<rbt::SliceSubscriber<wire::MapHeader, std::int8_t>> map_input;

    std::optional<TimePoint> first_base_stamp;
    TimePoint latest_status_stamp = -1;
    std::uint64_t previous_base_sequence = 0;
    std::uint64_t previous_guard_sequence = 0;
    std::uint64_t base_samples = 0;
    std::uint64_t guard_samples = 0;
    std::uint64_t status_samples = 0;
    std::uint64_t command_samples = 0;
    std::uint64_t path_points = 0;
    std::uint32_t guard_watchdog = 0;
    std::uint32_t guard_estop = 0;
    std::int64_t nav_state = -1;
    std::int64_t nav_error = -1;
    std::uint64_t goal_id = 0;
    bool path_captured = false;
    bool path_seen = false;
    bool map_captured = false;
    std::optional<TimePoint> first_path_stamp;
    std::vector<wire::PathPoint> first_path_points;
    const auto started = std::chrono::steady_clock::now();

    std::cout << "telemetry recorder ready: prefix=" << prefix
              << " output=" << output_directory.string() << std::endl;

    while (!native::interrupted) {
        const auto loop_started = std::chrono::steady_clock::now();
        if (duration_seconds > 0
            && std::chrono::duration<double>(loop_started - started).count() >= duration_seconds) {
            break;
        }

        guard_input.poll([&](const wire::GuardState& state) {
            (void)native::decodeStamp(state.stamp, clock.monotonic_identity());
            if (state.sequence == 0 || state.sequence <= previous_guard_sequence) return;
            previous_guard_sequence = state.sequence;
            guard_watchdog = state.watchdog_expired;
            guard_estop = state.emergency_stop;
            ++guard_samples;
        });

        status_input.poll([&](const wire::NavigationStatus& status) {
            const auto stamp = native::decodeStamp(status.stamp, clock.monotonic_identity());
            if (stamp < latest_status_stamp || status.state > maximum_navigation_state) return;
            latest_status_stamp = stamp;
            nav_state = status.state;
            nav_error = status.error;
            goal_id = status.goal_id;
            ++status_samples;
        });

        command_input.poll([&](const wire::VelocityCommand&) { ++command_samples; });

        base_input.poll([&](const wire::BaseState& state) {
            const auto stamp = native::decodeStamp(state.stamp, clock.monotonic_identity());
            if (state.sequence == 0 || state.sequence <= previous_base_sequence || !finite(state)) return;
            previous_base_sequence = state.sequence;
            if (!first_base_stamp) first_base_stamp = stamp;
            const double relative_seconds = static_cast<double>(stamp - *first_base_stamp)
                / static_cast<double>(nanoseconds_per_second);
            telemetry << std::setprecision(17) << relative_seconds << ',' << state.x << ',' << state.y
                      << ',' << state.yaw << ',' << state.linear_x << ',' << state.angular_z << ','
                      << state.watchdog_expired << ',' << state.emergency_stop << ','
                      << guard_watchdog << ',' << guard_estop << ',' << nav_state << ',' << nav_error
                      << ',' << goal_id << '\n';
            telemetry.flush();
            if (!telemetry) throw std::runtime_error("failed writing telemetry.csv");
            ++base_samples;
        });

        if (!path_seen && openSlice(path_input, prefix, "path", wire::PLANNED_PATH_DESCRIPTOR, "path")) {
            for (std::size_t count = 0; count < rbt::MAX_DRAIN_PER_SOURCE; ++count) {
                auto received = native::take(path_input->receive());
                if (!received) break;
                const auto& header = received->header();
                const auto points = received->payload();
                if (header.goal_id != 1 || points.empty()) continue;
                if (points.size() > native::max_path_points)
                    throw std::invalid_argument("path exceeds native maximum size");
                for (const auto& point : points) {
                    if (!finite(point)) throw std::invalid_argument("path contains nonfinite coordinates");
                }
                first_path_stamp = native::decodeStamp(header.stamp, clock.monotonic_identity());
                first_path_points.assign(points.begin(), points.end());
                path_points = points.size();
                path_seen = true;
                break;
            }
        }

        if (path_seen && !path_captured && first_base_stamp) {
            const double path_time = static_cast<double>(*first_path_stamp - *first_base_stamp)
                / static_cast<double>(nanoseconds_per_second);
            for (const auto& point : first_path_points) {
                path_output << std::setprecision(17) << path_time << ',' << point.x << ','
                            << point.y << ',' << point.yaw << '\n';
            }
            path_output.flush();
            if (!path_output) throw std::runtime_error("failed writing path.csv");
            path_captured = true;
            first_path_points.clear();
        }

        if (!map_captured && openSlice(map_input, prefix, "map", wire::OCCUPANCY_MAP_DESCRIPTOR, "map")) {
            for (std::size_t count = 0; count < rbt::MAX_DRAIN_PER_SOURCE; ++count) {
                auto received = native::take(map_input->receive());
                if (!received) break;
                writeMap(map_path, received->header(), received->payload());
                map_captured = true;
                break;
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    telemetry.flush();
    path_output.flush();
    std::cout << "telemetry recorder stopped: base_samples=" << base_samples
              << " guard_samples=" << guard_samples << " status_samples=" << status_samples
              << " command_samples=" << command_samples << " path_points=" << path_points
              << " map=" << (map_captured ? 1 : 0) << std::endl;
    if (base_samples == 0 || !path_captured || !map_captured) {
        std::cerr << "telemetry recorder incomplete: base, goal-1 path, and map samples are required\n";
        return 1;
    }
    return 0;
} catch (const std::exception& error) {
    std::cerr << "telemetry recorder failed: " << error.what() << '\n';
    return 1;
}

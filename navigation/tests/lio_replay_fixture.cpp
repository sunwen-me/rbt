#include <rbt/runtime.hpp>
#include <rbt_navigation/runtime/lio_contract.hpp>
#include <rbt_navigation/runtime/native.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace rbt::navigation;
namespace native = rbt::navigation::native;
namespace lio = rbt::navigation::lio;
namespace wire = rbt::navwire;
namespace fs = std::filesystem;

namespace {

constexpr std::uint32_t idle_state = static_cast<std::uint32_t>(NavigationState::Idle);
constexpr std::uint32_t succeeded_state = static_cast<std::uint32_t>(NavigationState::Succeeded);
constexpr std::uint32_t failed_state = static_cast<std::uint32_t>(NavigationState::Failed);
constexpr std::uint32_t localization_unavailable =
    static_cast<std::uint32_t>(ErrorCode::LocalizationUnavailable);
constexpr std::uint32_t stale_tf = static_cast<std::uint32_t>(ErrorCode::StaleTf);
constexpr std::uint32_t stale_odometry = static_cast<std::uint32_t>(ErrorCode::StaleOdometry);

std::string argument(int argc, char** argv, const std::string& name,
                     std::string fallback = {}) {
    const std::string key = "--" + name;
    for (int index = 1; index < argc; ++index) {
        if (argv[index] == key) {
            if (index + 1 >= argc) throw std::invalid_argument("missing value for " + key);
            return argv[index + 1];
        }
    }
    return fallback;
}

bool nearZero(double linear, double angular) {
    return std::abs(linear) < 0.02 && std::abs(angular) < 0.03;
}

bool moving(double linear, double angular) {
    return std::abs(linear) + std::abs(angular) > 0.08;
}

double distanceToSegment(double px, double py, double x0, double y0,
                         double x1, double y1) {
    const double dx = x1 - x0;
    const double dy = y1 - y0;
    const double length_squared = dx * dx + dy * dy;
    const double projection = length_squared > 0.0
        ? std::clamp(((px - x0) * dx + (py - y0) * dy) / length_squared, 0.0, 1.0)
        : 0.0;
    return std::hypot(px - (x0 + projection * dx), py - (y0 + projection * dy));
}

bool segmentAvoidsCell(double x0, double y0, double x1, double y1,
                       double cell_x, double cell_y) {
    const double safety_radius = 0.18 + 0.1 / std::sqrt(2.0);
    return distanceToSegment(cell_x, cell_y, x0, y0, x1, y1) > safety_radius;
}

bool retryable(rbt::StreamError error) {
    return error == rbt::StreamError::ReadinessOpen || error == rbt::StreamError::Transport;
}

void writeText(const fs::path& path, const std::string& value) {
    if (path.empty()) return;
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    fs::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::out | std::ios::trunc);
        if (!output) throw std::runtime_error("cannot write " + temporary.string());
        output << value << '\n';
        output.flush();
        if (!output) throw std::runtime_error("failed writing " + temporary.string());
    }
    std::error_code error;
    fs::rename(temporary, path, error);
    if (error) throw std::runtime_error("cannot publish " + path.string() + ": " + error.message());
}

std::string readPhase(const fs::path& path) {
    std::ifstream input(path);
    std::string phase;
    if (input >> phase) return phase;
    return "valid";
}

std::vector<std::int8_t> staticCells() {
    constexpr std::size_t width = 80;
    constexpr std::size_t height = 60;
    std::vector<std::int8_t> cells(width * height, 0);
    // A permanent wall gives the fusion check a cell that a point return must
    // never clear. It is outside the test goal's route.
    cells[40U * width + 60U] = 100;
    cells[40U * width + 61U] = 100;
    cells[40U * width + 62U] = 100;
    return cells;
}

void publishStaticMap(rbt::SlicePublisher<wire::MapHeader, std::int8_t>& publisher,
                      const std::vector<std::int8_t>& cells,
                      const rbt::time::HostClock& clock) {
    auto sample = native::take(publisher.loan_slice(cells.size()));
    auto& header = sample.header();
    header = {};
    header.stamp = native::encodeStamp(native::now(clock));
    header.version = 1;
    header.width = 80;
    header.height = 60;
    header.resolution = 0.1;
    std::copy_n("map", 3, header.frame);
    std::copy(cells.begin(), cells.end(), sample.payload().begin());
    if (!sample.send()) throw std::runtime_error("static map publication failed");
}

class ReplayProvider final : public rbt::Component {
  public:
    ReplayProvider(std::string provider_id, std::string scenario,
                   std::string base_prefix,
                   std::string static_prefix,
                   fs::path control_file)
        : provider_id_(std::move(provider_id)), scenario_(std::move(scenario)),
          base_prefix_(std::move(base_prefix)),
          static_prefix_(std::move(static_prefix)),
          control_file_(std::move(control_file)) {}

    rbt::Result<void> prepare() override { return {}; }

    rbt::ComponentId id() const override { return provider_id_; }

    std::vector<rbt::RequirementSpec> requirements() const override { return {}; }

    rbt::Result<void> activate(rbt::ActivationContext& context) override {
        const auto capability_id = rbt::generate_uuid();
        const auto descriptor = rbt::OwnedStreamDescriptor::from(rbt::StreamDescriptor(
            "rbt/cap/" + capability_id + "/stream", wire::LIO_NAVIGATION_FRAME_DESCRIPTOR,
            rbt::StreamProfile::Sensor));
        auto frame_publisher = rbt::SlicePublisher<wire::LioFrameHeader, wire::LioPoint>::create(
            descriptor.borrowed(), wire::LIO_NAVIGATION_FRAME_DESCRIPTOR, 8U);
        if (!frame_publisher) return rbt::Result<void>(frame_publisher.error());
        auto map_publisher = rbt::SlicePublisher<wire::MapHeader, std::int8_t>::create(
            {static_prefix_ + "/map", wire::OCCUPANCY_MAP_DESCRIPTOR, rbt::StreamProfile::Sensor},
            wire::OCCUPANCY_MAP_DESCRIPTOR, 80U * 60U);
        if (!map_publisher) return rbt::Result<void>(map_publisher.error());

        const auto provided = context.provide<lio::LioNavigationFrameV1>(capability_id, descriptor);
        if (!provided) return rbt::Result<void>::failure(provided.error());
        std::cout << "LIO_SOURCE_ACTIVATED provider=" << provider_id_
                  << " capability=" << capability_id << std::endl;

        const auto scenario = scenario_;
        const auto base_prefix = base_prefix_;
        const auto control_file = control_file_;
        auto frame_endpoint = std::move(frame_publisher).value();
        auto map_endpoint = std::move(map_publisher).value();
        (void)context.spawn_critical(
            "publish-lio-replay",
            [scenario, base_prefix, control_file,
             frame_endpoint = std::move(frame_endpoint),
             map_endpoint = std::move(map_endpoint)](std::stop_token stop) mutable {
                auto clock = native::hostClock();
                native::FixedInput<wire::BaseState> base_input(base_prefix, "base_state");
                const auto cells = staticCells();
                std::optional<wire::BaseState> base_state;
                auto last_map = std::chrono::steady_clock::time_point::min();
                std::uint64_t sequence = 0;
                const auto started = std::chrono::steady_clock::now();

                while (!stop.stop_requested()) {
                    const auto wall_now = std::chrono::steady_clock::now();
                    const auto current = native::now(clock);
                    base_input.poll([&](const wire::BaseState& value) {
                        try {
                            const auto stamp = native::decodeStamp(value.stamp, current.clock);
                            if (value.sequence != 0 && fresh(stamp, current.ns, 250'000'000)
                                && std::isfinite(value.x) && std::isfinite(value.y)
                                && std::isfinite(value.yaw) && std::isfinite(value.linear_x)
                                && std::isfinite(value.angular_z)) {
                                base_state = value;
                            }
                        } catch (const std::exception&) {
                            // The next fresh feedback sample replaces this one.
                        }
                    });

                    if (last_map == std::chrono::steady_clock::time_point::min()
                        || wall_now - last_map >= std::chrono::milliseconds(500)) {
                        publishStaticMap(map_endpoint, cells, clock);
                        last_map = wall_now;
                    }

                    const std::string phase = readPhase(control_file);
                    wire::LioFrameHeader header{};
                    header.stamp = native::encodeStamp(current);
                    header.sequence = ++sequence;
                    header.uncertainty_ns = 100'000;
                    header.flags = 1U;
                    std::copy_n("odom", 4, header.world_frame);
                    std::copy_n("base_footprint", 14, header.base_frame);
                    const double x = base_state ? base_state->x : 1.0;
                    const double y = base_state ? base_state->y : 1.0;
                    const double yaw = base_state ? base_state->yaw : 0.0;
                    header.odom_base_position[0] = x;
                    header.odom_base_position[1] = y;
                    header.odom_base_position[2] = 0.0;
                    header.odom_base_quaternion_xyzw[2] = std::sin(yaw / 2.0);
                    header.odom_base_quaternion_xyzw[3] = std::cos(yaw / 2.0);
                    header.map_odom_quaternion_xyzw[3] = 1.0;
                    header.sensor_origin[0] = x;
                    header.sensor_origin[1] = y;
                    header.sensor_origin[2] = 0.5;
                    if (base_state) {
                        header.linear_velocity[0] = base_state->linear_x;
                        header.angular_velocity[2] = base_state->angular_z;
                    }

                    if (phase == "invalid") {
                        if (scenario == "invalid_localization") {
                            header.flags = 0U;
                        } else if (scenario == "stale") {
                            header.stamp.nanoseconds -= second;
                        } else if (scenario == "future") {
                            header.stamp.nanoseconds += second;
                        } else if (scenario == "foreign_clock") {
                            header.stamp.source_id[0] ^= 0x80U;
                        }
                    }

                    auto sample = native::take(frame_endpoint.loan_slice(1U));
                    auto& sample_header = sample.header();
                    sample_header = header;
                    if (phase == "obstacle") {
                        sample.payload()[0] = wire::LioPoint{3.05F, 1.05F, 0.8F, 1.0F};
                    } else if (phase == "clear") {
                        // This higher return passes through the earlier hit's cell.
                        sample.payload()[0] = wire::LioPoint{4.05F, 1.05F, 1.4F, 1.0F};
                    } else {
                        // A small ground-level return keeps ordinary frames nonempty
                        // without adding a dynamic obstacle.
                        sample.payload()[0] = wire::LioPoint{1.05F, 1.05F, 0.0F, 1.0F};
                    }
                    if (!sample.send()) return rbt::TaskOutcome::error("LIO frame send failed");

                    if (std::chrono::steady_clock::now() - started > std::chrono::seconds(30)) {
                        return rbt::TaskOutcome::error("LIO replay provider exceeded its test window");
                    }
                    std::this_thread::sleep_until(wall_now + std::chrono::milliseconds(50));
                }
                return rbt::TaskOutcome::completed();
            });
        return {};
    }

  private:
    std::string provider_id_;
    std::string scenario_;
    std::string base_prefix_;
    std::string static_prefix_;
    fs::path control_file_;
};

struct Evidence {
    std::string scenario;
    std::string phase = "valid";
    std::uint64_t status_frames{};
    std::uint64_t base_frames{};
    std::uint64_t command_frames{};
    std::uint64_t fresh_odom_after_fault{};
    std::uint64_t zero_frames_after_fault{};
    std::uint64_t base_zero_frames_after_fault{};
    std::int64_t zero_latency_ms = -1;
    std::int64_t base_zero_latency_ms = -1;
    std::uint32_t nav_state = idle_state;
    std::uint32_t nav_error{};
    std::uint64_t map_version{};
    double last_base_x{}, last_base_y{}, last_base_yaw{};
    int obstacle_cell = -1;
    int clear_cell = -1;
    int return_cell = -1;
    int static_wall_cell = -1;
    bool goal_sent{};
    bool goal_acknowledged{};
    bool moving_seen{};
    bool moving_before_fault{};
    bool obstacle_seen{};
    bool safe_replan{};
    bool clear_seen{};
    bool static_wall_seen{};
    bool static_wall_preserved = true;
    bool avoidance_goal_succeeded{};
    bool avoidance_goal_reached{};
    bool avoidance_obstacle_clear = true;
    bool avoidance_static_clear = true;
    bool obstacle_persisted_at_goal{};
    bool nav_failed_after_fault{};
    bool no_fresh_odom_after_fault{};
    bool final_zero_after_fault{};
    bool base_zero_after_fault{};
    bool passed{};
    std::string failure;
};

void writeEvidence(const fs::path& path, const Evidence& value) {
    if (path.empty()) return;
    std::ofstream output(path.string() + ".tmp", std::ios::out | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write evidence file");
    output << "{\"scenario\":\"" << value.scenario << "\",\"phase\":\"" << value.phase
           << "\",\"status_frames\":" << value.status_frames
           << ",\"base_frames\":" << value.base_frames
           << ",\"command_frames\":" << value.command_frames
           << ",\"fresh_odom_after_fault\":" << value.fresh_odom_after_fault
           << ",\"zero_frames_after_fault\":" << value.zero_frames_after_fault
           << ",\"base_zero_frames_after_fault\":" << value.base_zero_frames_after_fault
           << ",\"zero_latency_ms\":" << value.zero_latency_ms
           << ",\"base_zero_latency_ms\":" << value.base_zero_latency_ms
           << ",\"nav_state\":" << value.nav_state << ",\"nav_error\":" << value.nav_error
           << ",\"map_version\":" << value.map_version
           << ",\"last_base_x\":" << value.last_base_x
           << ",\"last_base_y\":" << value.last_base_y
           << ",\"last_base_yaw\":" << value.last_base_yaw
           << ",\"obstacle_cell\":" << value.obstacle_cell
           << ",\"clear_cell\":" << value.clear_cell
           << ",\"return_cell\":" << value.return_cell
           << ",\"static_wall_cell\":" << value.static_wall_cell
           << ",\"goal_sent\":" << (value.goal_sent ? "true" : "false")
           << ",\"goal_acknowledged\":" << (value.goal_acknowledged ? "true" : "false")
           << ",\"moving_seen\":" << (value.moving_seen ? "true" : "false")
           << ",\"moving_before_fault\":" << (value.moving_before_fault ? "true" : "false")
           << ",\"obstacle_seen\":" << (value.obstacle_seen ? "true" : "false")
           << ",\"safe_replan\":" << (value.safe_replan ? "true" : "false")
           << ",\"clear_seen\":" << (value.clear_seen ? "true" : "false")
           << ",\"static_wall_seen\":" << (value.static_wall_seen ? "true" : "false")
           << ",\"static_wall_preserved\":" << (value.static_wall_preserved ? "true" : "false")
           << ",\"avoidance_goal_succeeded\":" << (value.avoidance_goal_succeeded ? "true" : "false")
           << ",\"avoidance_goal_reached\":" << (value.avoidance_goal_reached ? "true" : "false")
           << ",\"avoidance_obstacle_clear\":" << (value.avoidance_obstacle_clear ? "true" : "false")
           << ",\"avoidance_static_clear\":" << (value.avoidance_static_clear ? "true" : "false")
           << ",\"obstacle_persisted_at_goal\":" << (value.obstacle_persisted_at_goal ? "true" : "false")
           << ",\"nav_failed_after_fault\":" << (value.nav_failed_after_fault ? "true" : "false")
           << ",\"no_fresh_odom_after_fault\":" << (value.no_fresh_odom_after_fault ? "true" : "false")
           << ",\"final_zero_after_fault\":" << (value.final_zero_after_fault ? "true" : "false")
           << ",\"base_zero_after_fault\":" << (value.base_zero_after_fault ? "true" : "false")
           << ",\"passed\":" << (value.passed ? "true" : "false")
           << ",\"failure\":\"" << value.failure << "\"}\n";
    output.flush();
    if (!output) throw std::runtime_error("failed writing evidence file");
    output.close();
    std::error_code error;
    fs::rename(path.string() + ".tmp", path, error);
    if (error) throw std::runtime_error("cannot publish evidence file: " + error.message());
}

template <typename Header, typename Element>
bool openSlice(std::optional<rbt::SliceSubscriber<Header, Element>>& subscriber,
               const std::string& prefix, const char* suffix,
               const rbt::MessageTypeDescriptor& descriptor) {
    if (subscriber) return true;
    auto result = rbt::SliceSubscriber<Header, Element>::create(
        native::stream(prefix, suffix, descriptor), descriptor);
    if (!result) {
        if (retryable(result.error())) return false;
        throw std::runtime_error(std::string("fixture slice input: ")
                                 + std::string(rbt::to_string(result.error())));
    }
    subscriber.emplace(std::move(result).value());
    return true;
}

int cellAt(const wire::MapHeader& header, std::span<const std::int8_t> cells,
           double x, double y) {
    if (header.width == 0 || header.height == 0
        || static_cast<std::size_t>(header.width) * header.height != cells.size()
        || !std::isfinite(header.resolution) || header.resolution <= 0.0) return -1;
    const double dx = x - header.origin_x;
    const double dy = y - header.origin_y;
    const double cosine = std::cos(header.origin_yaw);
    const double sine = std::sin(header.origin_yaw);
    const double gx = (cosine * dx + sine * dy) / header.resolution;
    const double gy = (-sine * dx + cosine * dy) / header.resolution;
    if (gx < 0.0 || gy < 0.0 || gx >= header.width || gy >= header.height) return -1;
    return cells[static_cast<std::size_t>(std::floor(gy)) * header.width
                 + static_cast<std::size_t>(std::floor(gx))];
}

bool pathClearsObstacle(const std::vector<wire::PathPoint>& points) {
    constexpr double obstacle_x = 3.05;
    constexpr double obstacle_y = 1.05;
    constexpr double clearance = 0.26;
    if (points.empty() || std::any_of(points.begin(), points.end(), [](const wire::PathPoint& point) {
            return !std::isfinite(point.x) || !std::isfinite(point.y);
        })) return false;
    if (points.size() == 1U) {
        return std::hypot(points.front().x - obstacle_x, points.front().y - obstacle_y) >= clearance;
    }
    for (std::size_t index = 1; index < points.size(); ++index) {
        if (distanceToSegment(obstacle_x, obstacle_y,
                              points[index - 1].x, points[index - 1].y,
                              points[index].x, points[index].y) < clearance) return false;
    }
    return true;
}

int runMonitor(const std::string& scenario, const std::string& prefix,
               const std::string& base_prefix,
               const fs::path& control_file, const fs::path& evidence_file) {
    auto clock = native::hostClock();
    auto goals = native::publisher<wire::GoalCommand>(prefix, "goal");
    native::FixedInput<wire::BaseState> base_input(base_prefix, "base_state");
    native::FixedInput<wire::NavigationStatus> status_input(prefix, "status");
    native::FixedInput<wire::Odom2D> odom_input(prefix, "odom");
    native::FixedInput<wire::VelocityCommand> command_input("/" + prefix + "/cmd_vel");
    std::optional<rbt::SliceSubscriber<wire::MapHeader, std::int8_t>> map_input;
    std::optional<rbt::SliceSubscriber<wire::PathHeader, wire::PathPoint>> path_input;

    Evidence evidence;
    evidence.scenario = scenario;
    std::ofstream base_trace(evidence_file.parent_path() / "base-trajectory.csv");
    base_trace << "stamp_ns,x,y,yaw,v,omega\n";
    std::ofstream command_trace(evidence_file.parent_path() / "commands.csv");
    command_trace << "stamp_ns,v,omega\n";
    std::optional<wire::BaseState> base_state;
    std::optional<std::array<double, 2>> previous_base_pose;
    wire::NavigationStatus status{};
    std::uint64_t previous_base_sequence = 0;
    std::uint64_t previous_command_sequence = 0;
    TimePoint obstacle_stamp_ns = -1;
    std::vector<wire::PathPoint> latest_path;
    bool path_received_after_obstacle = false;
    std::int64_t fault_started_ns = -1;
    auto fault_started_wall = std::chrono::steady_clock::time_point::min();
    auto first_moving_at = std::chrono::steady_clock::time_point::min();
    auto next_goal = std::chrono::steady_clock::time_point::min();
    const auto started = std::chrono::steady_clock::now();
    const auto avoidance_scenario = scenario == "avoidance";
    const auto deadline = started + (avoidance_scenario
        ? std::chrono::seconds(20) : std::chrono::seconds(10));
    const auto goal_timeout_ns = avoidance_scenario ? 30 * second : 20 * second;
    const auto dynamic_scenario = scenario == "obstacle" || avoidance_scenario;
    const auto invalid_scenario = scenario == "invalid_localization" || scenario == "stale"
        || scenario == "future" || scenario == "foreign_clock";
    const auto crash_scenario = scenario == "source_crash" || scenario == "bridge_crash";
    if (!dynamic_scenario && !invalid_scenario && !crash_scenario) {
        throw std::invalid_argument("unknown LIO monitor scenario: " + scenario);
    }
    std::cout << "LIO_MONITOR_READY scenario=" << scenario << " prefix=" << prefix << std::endl;

    while (std::chrono::steady_clock::now() < deadline) {
        const auto wall_now = std::chrono::steady_clock::now();
        const auto current = native::now(clock);
        base_input.poll([&](const wire::BaseState& sample) {
            try {
                const auto stamp = native::decodeStamp(sample.stamp, current.clock);
                if (sample.sequence != 0 && sample.sequence > previous_base_sequence
                    && fresh(stamp, current.ns, 500'000'000)) {
                    previous_base_sequence = sample.sequence;
                    base_state = sample;
                    evidence.last_base_x = sample.x;
                    evidence.last_base_y = sample.y;
                    evidence.last_base_yaw = sample.yaw;
                    base_trace << stamp << ',' << sample.x << ',' << sample.y << ','
                               << sample.yaw << ',' << sample.linear_x << ',' << sample.angular_z << '\n';
                    ++evidence.base_frames;
                    if (avoidance_scenario && evidence.obstacle_seen) {
                        if (!std::isfinite(sample.x) || !std::isfinite(sample.y)) {
                            evidence.avoidance_obstacle_clear = false;
                            evidence.avoidance_static_clear = false;
                        } else if (previous_base_pose) {
                            evidence.avoidance_obstacle_clear = evidence.avoidance_obstacle_clear
                                && segmentAvoidsCell((*previous_base_pose)[0], (*previous_base_pose)[1],
                                    sample.x, sample.y, 3.05, 1.05);
                            for (std::size_t wall_cell = 0; wall_cell < 3; ++wall_cell) {
                                evidence.avoidance_static_clear = evidence.avoidance_static_clear
                                    && segmentAvoidsCell((*previous_base_pose)[0], (*previous_base_pose)[1],
                                        sample.x, sample.y, 6.05 + 0.1 * wall_cell, 4.05);
                            }
                        }
                    }
                    previous_base_pose = std::array<double, 2>{sample.x, sample.y};
                    if (fault_started_ns >= 0 && stamp > fault_started_ns) {
                        if (nearZero(sample.linear_x, sample.angular_z)) {
                            ++evidence.base_zero_frames_after_fault;
                            if (evidence.base_zero_latency_ms < 0) {
                                evidence.base_zero_latency_ms = static_cast<std::int64_t>(
                                    std::chrono::duration_cast<std::chrono::milliseconds>(
                                        wall_now - fault_started_wall).count());
                            }
                        } else {
                            evidence.base_zero_frames_after_fault = 0;
                            evidence.base_zero_latency_ms = -1;
                        }
                    }
                }
            } catch (const std::exception&) {
                // A malformed sample does not replace the last valid one.
            }
        });
        status_input.poll([&](const wire::NavigationStatus& sample) {
            try {
                (void)native::decodeStamp(sample.stamp, current.clock);
                status = sample;
                evidence.nav_state = sample.state;
                evidence.nav_error = sample.error;
                evidence.map_version = sample.map_version;
                ++evidence.status_frames;
            } catch (const std::exception&) {
                // Ignore foreign-clock diagnostics.
            }
        });
        odom_input.poll([&](const wire::Odom2D& odom) {
            try {
                const auto stamp = native::decodeStamp(odom.stamp, current.clock);
                if (fault_started_ns >= 0 && stamp > fault_started_ns + 750'000'000) {
                    ++evidence.fresh_odom_after_fault;
                }
            } catch (const std::exception&) {
                // A clock-invalid odometry sample is not fresh input.
            }
        });
        command_input.poll([&](const wire::VelocityCommand& command) {
            try {
                const auto stamp = native::decodeStamp(command.stamp, current.clock);
                if (command.sequence == 0 || command.sequence <= previous_command_sequence
                    || !std::isfinite(command.linear_x) || !std::isfinite(command.angular_z)) return;
                previous_command_sequence = command.sequence;
                ++evidence.command_frames;
                command_trace << stamp << ',' << command.linear_x << ',' << command.angular_z << '\n';
                const bool is_moving = moving(command.linear_x, command.angular_z);
                if (is_moving) {
                    evidence.moving_seen = true;
                    if (fault_started_ns < 0) evidence.moving_before_fault = true;
                }
                if (fault_started_ns >= 0 && stamp > fault_started_ns) {
                    if (nearZero(command.linear_x, command.angular_z)) {
                        ++evidence.zero_frames_after_fault;
                        if (evidence.zero_latency_ms < 0) {
                            evidence.zero_latency_ms = static_cast<std::int64_t>(
                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                    wall_now - fault_started_wall).count());
                        }
                    } else {
                        evidence.zero_frames_after_fault = 0;
                        evidence.zero_latency_ms = -1;
                    }
                }
            } catch (const std::exception&) {
                // Do not let a malformed command satisfy the motion or stop checks.
            }
        });

        if (openSlice(map_input, prefix, "map", wire::OCCUPANCY_MAP_DESCRIPTOR)) {
            for (std::size_t count = 0; count < rbt::MAX_DRAIN_PER_SOURCE; ++count) {
                auto received = native::take(map_input->receive());
                if (!received) break;
                const auto& header = received->header();
                const auto cells = received->payload();
                evidence.obstacle_cell = cellAt(header, cells, 3.05, 1.05);
                evidence.clear_cell = evidence.obstacle_cell;
                evidence.return_cell = cellAt(header, cells, 4.05, 1.05);
                evidence.static_wall_cell = cellAt(header, cells, 6.05, 4.05);
                if (evidence.obstacle_cell == 100) {
                    evidence.obstacle_seen = true;
                    if (fault_started_ns < 0) {
                        fault_started_ns = current.ns;
                        fault_started_wall = wall_now;
                        obstacle_stamp_ns = native::decodeStamp(header.stamp, current.clock);
                    }
                }
                if (evidence.static_wall_cell == 100) {
                    evidence.static_wall_seen = true;
                } else if (evidence.static_wall_seen) {
                    evidence.static_wall_preserved = false;
                }
                for (std::size_t wall_cell = 0; wall_cell < 3; ++wall_cell) {
                    const int wall_value = cellAt(header, cells, 6.05 + 0.1 * wall_cell, 4.05);
                    if (wall_value == 100) {
                        evidence.static_wall_seen = true;
                    } else if (evidence.static_wall_seen) {
                        evidence.static_wall_preserved = false;
                    }
                }
                if (avoidance_scenario && evidence.avoidance_goal_succeeded
                    && evidence.obstacle_cell == 100) {
                    evidence.obstacle_persisted_at_goal = true;
                }
                if (dynamic_scenario && evidence.obstacle_seen
                    && readPhase(control_file) == "clear"
                    && evidence.obstacle_cell == 0 && evidence.return_cell == 100
                    && evidence.static_wall_cell == 100 && evidence.static_wall_seen) {
                    evidence.clear_seen = true;
                }
            }
        }

        if (openSlice(path_input, prefix, "path", wire::PLANNED_PATH_DESCRIPTOR)) {
            for (std::size_t count = 0; count < rbt::MAX_DRAIN_PER_SOURCE; ++count) {
                auto received = native::take(path_input->receive());
                if (!received) break;
                if (received->header().goal_id != 1) continue;
                const auto points = received->payload();
                latest_path.assign(points.begin(), points.end());
                if (evidence.obstacle_seen) {
                    const auto path_stamp = native::decodeStamp(received->header().stamp, current.clock);
                    if (obstacle_stamp_ns < 0 || path_stamp > obstacle_stamp_ns) {
                        path_received_after_obstacle = true;
                    }
                }
            }
        }

        if (evidence.map_version != 0 && base_state && !evidence.goal_sent) {
            native::publish(goals, wire::GoalCommand{native::encodeStamp(current), 1U, 0U, 0U,
                5.5, 1.0, 0.0, goal_timeout_ns});
            evidence.goal_sent = true;
            next_goal = wall_now + std::chrono::milliseconds(100);
        } else if (evidence.goal_sent && status.goal_id != 1U && wall_now >= next_goal) {
            native::publish(goals, wire::GoalCommand{native::encodeStamp(current), 1U, 0U, 0U,
                5.5, 1.0, 0.0, goal_timeout_ns});
            next_goal = wall_now + std::chrono::milliseconds(100);
        }
        evidence.goal_acknowledged = status.goal_id == 1U && status.state != idle_state;

        if (evidence.moving_seen && first_moving_at == std::chrono::steady_clock::time_point::min()) {
            first_moving_at = wall_now;
        }
        if (dynamic_scenario && evidence.goal_acknowledged && evidence.moving_seen
            && first_moving_at != std::chrono::steady_clock::time_point::min()
            && wall_now - first_moving_at >= std::chrono::milliseconds(500)
            && readPhase(control_file) != "obstacle" && !evidence.obstacle_seen) {
            writeText(control_file, "obstacle");
            evidence.phase = "obstacle";
        }

        if (dynamic_scenario && evidence.obstacle_seen) {
            const bool stopped = evidence.zero_frames_after_fault >= 3;
            const bool path_safe = path_received_after_obstacle && pathClearsObstacle(latest_path);
            if (stopped || path_safe) evidence.safe_replan = true;
            if (!avoidance_scenario && evidence.safe_replan && readPhase(control_file) != "clear") {
                writeText(control_file, "clear");
                evidence.phase = "clear";
            }
            if (!avoidance_scenario && evidence.clear_seen && evidence.static_wall_preserved) {
                evidence.passed = true;
                break;
            }
        }

        if (avoidance_scenario && evidence.goal_acknowledged) {
            evidence.avoidance_goal_succeeded = status.goal_id == 1U
                && status.state == succeeded_state;
            evidence.avoidance_goal_reached = evidence.avoidance_goal_succeeded && base_state
                && std::hypot(base_state->x - 5.5, base_state->y - 1.0) <= 0.25;
            if (evidence.avoidance_goal_succeeded && evidence.obstacle_cell == 100) {
                evidence.obstacle_persisted_at_goal = true;
            }
            if (evidence.safe_replan && evidence.avoidance_goal_reached
                && evidence.avoidance_obstacle_clear && evidence.avoidance_static_clear
                && evidence.obstacle_persisted_at_goal && evidence.static_wall_seen
                && evidence.static_wall_preserved) {
                evidence.passed = true;
                break;
            }
        }

        if (invalid_scenario && evidence.goal_acknowledged && evidence.moving_seen
            && readPhase(control_file) != "invalid") {
            writeText(control_file, "invalid");
            evidence.phase = "invalid";
            fault_started_ns = current.ns;
            fault_started_wall = wall_now;
            first_moving_at = wall_now;
        }
        if (crash_scenario) {
            const auto phase = readPhase(control_file);
            const auto expected_phase = scenario == "source_crash" ? "source_crash" : "bridge_crash";
            if (phase == expected_phase && fault_started_ns < 0) {
                fault_started_ns = current.ns;
                fault_started_wall = wall_now;
                evidence.phase = phase;
            }
        }

        if (fault_started_ns >= 0 && evidence.goal_acknowledged) {
            const bool localization_error = status.error == localization_unavailable
                || status.error == stale_tf || status.error == stale_odometry;
            evidence.nav_failed_after_fault = status.state == failed_state && localization_error;
            const bool grace_elapsed = wall_now - fault_started_wall >= std::chrono::milliseconds(750);
            evidence.no_fresh_odom_after_fault = grace_elapsed
                && evidence.fresh_odom_after_fault == 0;
            evidence.final_zero_after_fault = evidence.zero_frames_after_fault >= 3;
            evidence.base_zero_after_fault = evidence.base_zero_frames_after_fault >= 3;
            if (evidence.nav_failed_after_fault && evidence.no_fresh_odom_after_fault
                && evidence.final_zero_after_fault
                && evidence.base_zero_after_fault) {
                evidence.passed = true;
                break;
            }
        }

        evidence.phase = readPhase(control_file);
        writeEvidence(evidence_file, evidence);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    if (!evidence.passed) {
        if (std::chrono::steady_clock::now() >= deadline) {
            evidence.failure = avoidance_scenario
                ? "avoidance scenario exceeded the 20-second monitor deadline"
                : "scenario exceeded the 10-second monitor deadline";
        } else if (!evidence.goal_sent) {
            evidence.failure = "navigation map/base did not become ready for a goal";
        } else if (dynamic_scenario && !evidence.obstacle_seen) {
            evidence.failure = "fused map did not show the injected obstacle cell as occupied";
        } else if (dynamic_scenario && !evidence.safe_replan) {
            evidence.failure = "navigation did not stop or publish a path clear of the obstacle";
        } else if (avoidance_scenario && !evidence.avoidance_goal_succeeded) {
            evidence.failure = "navigation did not complete the goal while the obstacle remained";
        } else if (avoidance_scenario && !evidence.avoidance_goal_reached) {
            evidence.failure = "simulated base did not reach the requested avoidance goal";
        } else if (avoidance_scenario && !evidence.avoidance_obstacle_clear) {
            evidence.failure = "simulated base swept within the obstacle footprint clearance";
        } else if (avoidance_scenario && !evidence.avoidance_static_clear) {
            evidence.failure = "simulated base swept within a static wall footprint clearance";
        } else if (avoidance_scenario && !evidence.obstacle_persisted_at_goal) {
            evidence.failure = "dynamic obstacle did not remain occupied through goal completion";
        } else if (avoidance_scenario && !evidence.static_wall_preserved) {
            evidence.failure = "fused map did not preserve every static wall cell";
        } else if (dynamic_scenario && !evidence.clear_seen) {
            evidence.failure = "higher return did not clear the prior hit while preserving the static wall";
        } else if (fault_started_ns < 0) {
            evidence.failure = "fault was not injected after confirmed motion";
        } else {
            evidence.failure = "navigation did not stop consuming fresh odometry and fail closed after the fault";
        }
    }
    writeEvidence(evidence_file, evidence);
    if (!evidence.passed) {
        std::cerr << "FAIL lio replay " << scenario << ": " << evidence.failure << std::endl;
        return 1;
    }
    std::cout << "PASS lio replay " << scenario << std::endl;
    return 0;
}

} // namespace

int main(int argc, char** argv) try {
    const auto role = argument(argc, argv, "role", "provider");
    const auto scenario = argument(argc, argv, "scenario", "obstacle");
    const auto navigation_prefix = argument(argc, argv, "prefix");
    const auto base_prefix = argument(argc, argv, "base-prefix");
    const auto static_prefix = argument(argc, argv, "static-prefix");
    const auto provider_id = argument(argc, argv, "provider-id", "super-lio");
    const fs::path control_file(argument(argc, argv, "control-file"));
    const fs::path evidence_file(argument(argc, argv, "evidence-file"));
    if (role == "monitor") {
        if (navigation_prefix.empty() || base_prefix.empty() || control_file.empty() || evidence_file.empty()) {
            throw std::invalid_argument("monitor requires --prefix, --base-prefix, --control-file, and --evidence-file");
        }
        return runMonitor(scenario, navigation_prefix, base_prefix, control_file, evidence_file);
    }
    if (role != "provider" || base_prefix.empty() || static_prefix.empty()) {
        throw std::invalid_argument("provider requires --base-prefix and --static-prefix");
    }
    const char* composer_address = std::getenv("RBT_COMPOSER_ADDR");
    if (composer_address == nullptr || *composer_address == '\0') {
        throw std::invalid_argument("RBT_COMPOSER_ADDR must identify this test's Composer");
    }
    rbt::RuntimeOptions options;
    options.composer_address = composer_address;
    rbt::ComponentRuntime runtime(std::make_unique<ReplayProvider>(
        provider_id, scenario, base_prefix, static_prefix, control_file),
        std::move(options));
    return runtime.run();
} catch (const std::exception& error) {
    std::cerr << "LIO replay fixture: " << error.what() << '\n';
    return 1;
}

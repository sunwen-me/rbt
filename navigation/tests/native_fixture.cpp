#include <rbt_navigation/map/grid_map.hpp>
#include <rbt_navigation/runtime/native.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/types.h>

using namespace rbt::navigation;
namespace native = rbt::navigation::native;
namespace wire = rbt::navwire;

namespace {

constexpr std::uint32_t idle_state = static_cast<std::uint32_t>(NavigationState::Idle);
constexpr std::uint32_t succeeded_state = static_cast<std::uint32_t>(NavigationState::Succeeded);
constexpr std::uint32_t canceled_state = static_cast<std::uint32_t>(NavigationState::Canceled);
constexpr std::uint32_t failed_state = static_cast<std::uint32_t>(NavigationState::Failed);
constexpr std::uint32_t localization_unavailable =
    static_cast<std::uint32_t>(ErrorCode::LocalizationUnavailable);
constexpr std::uint32_t stale_tf = static_cast<std::uint32_t>(ErrorCode::StaleTf);
constexpr std::uint32_t map_unavailable = static_cast<std::uint32_t>(ErrorCode::MapUnavailable);
constexpr std::uint32_t stale_map = static_cast<std::uint32_t>(ErrorCode::StaleMap);

struct FixtureState {
    wire::BaseState base{};
    wire::NavigationStatus status{};
    wire::GuardState guard{};
    wire::VelocityCommand final_command{};
    bool have_base{};
    bool have_guard{};
    std::uint64_t base_state_frames{};
    std::uint64_t status_count{};
    std::uint64_t guard_state_frames{};
    std::uint64_t final_command_frames{};
    std::uint64_t valid_final_command_frames{};
    std::uint64_t injected_expired_final_frames{};
    std::uint64_t guard_moving_frames{};
    std::uint64_t guard_stopped_frames{};
    std::uint64_t last_guard_sequence{};
    std::uint64_t last_final_sequence{};
    std::uint64_t last_guard_stamp_ns{};
    std::uint64_t last_final_stamp_ns{};
    bool guard_moving_seen{};
    bool guard_moving_before_fault{};
    std::uint64_t path_points{};
    bool path_seen{};
    bool path_above_wall{};
    bool path_footprint_free{true};
    bool path_segments_free{true};
    bool all_footprints_free{true};
    bool all_pose_segments_free{true};
    Pose2D previous_base_pose{};
    bool have_previous_base_pose{};
};

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

bool nearZero(const wire::BaseState& state) {
    return std::abs(state.linear_x) < 0.02 && std::abs(state.angular_z) < 0.03;
}

bool isMoving(const wire::BaseState& state) {
    return std::abs(state.linear_x) + std::abs(state.angular_z) > 0.05;
}

bool isMoving(const wire::VelocityCommand& command) {
    return std::abs(command.linear_x) + std::abs(command.angular_z) > 0.05;
}

std::vector<std::int8_t> makeMap(const std::string& scenario) {
    constexpr std::size_t width = 80;
    constexpr std::size_t height = 60;
    std::vector<std::int8_t> cells(width * height, 0);
    if (scenario == "obstacle") {
        // A wall at x=3 m leaves a traversable gap above y=2.5 m.
        for (std::size_t y = 0; y < 25; ++y) {
            for (std::size_t x = 30; x < 32; ++x) {
                cells[y * width + x] = 100;
            }
        }
    }
    return cells;
}

void publishMap(rbt::SlicePublisher<wire::MapHeader, std::int8_t>& publisher,
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
    if (!sample.send()) fail("occupancy map send failed");
}

void publishMapTransform(rbt::tf::DynamicTfPublisher& publisher,
                          const rbt::time::HostClock& clock) {
    const auto stamp = native::now(clock);
    publisher.publish({rbt::tf::FrameId("map"), rbt::tf::FrameId("odom"), stamp,
                       0U, 1U,
                       rbt::tf::Transform3(rbt::tf::Vec3(0.0, 0.0, 0.0),
                                           rbt::tf::Quaternion(0.0, 0.0, 0.0, 1.0))});
}

void publishBaseTransform(rbt::tf::DynamicTfPublisher& publisher,
                          const wire::BaseState& state,
                          const rbt::time::HostClock& clock,
                          std::uint64_t& last_sequence,
                          std::uint64_t& publication_drops,
                          std::chrono::steady_clock::time_point& last_drop_log) {
    const auto current = native::now(clock);
    const auto stamp_ns = native::decodeStamp(state.stamp, current.clock);
    if (!state.sequence || state.sequence <= last_sequence) {
        fail("base state TF sequence did not increase");
    }
    if (!fresh(stamp_ns, current.ns, 250'000'000)) {
        fail("base state TF stamp was stale or future-dated");
    }
    const Pose2D pose{state.x, state.y, state.yaw};
    if (!finite(pose)) fail("base state TF pose was not finite");
    last_sequence = state.sequence;
    const rbt::time::Timestamp original_stamp{current.clock, stamp_ns};
    const rbt::tf::Transform3 transform(rbt::tf::Vec3(pose.x, pose.y, 0.0),
        rbt::tf::Quaternion(0.0, 0.0, std::sin(pose.yaw / 2.0), std::cos(pose.yaw / 2.0)));
    try {
        publisher.publish({rbt::tf::FrameId("odom"), rbt::tf::FrameId("base_link"),
                           original_stamp, 0U, 1U, transform});
    } catch (const rbt::tf::TfTransportError& error) {
        if (error.stream_error() != rbt::StreamError::QueueFull) throw;
        ++publication_drops;
        const auto wall_now = std::chrono::steady_clock::now();
        if (last_drop_log == std::chrono::steady_clock::time_point::min()
            || wall_now - last_drop_log >= std::chrono::seconds(1)) {
            std::cerr << "native fixture: dropped odom->base_link TF publication: " << error.what()
                      << "; tf_publish_drops=" << publication_drops << '\n';
            last_drop_log = wall_now;
        }
    }
}

void drainInputs(rbt::FixedSubscriber<wire::BaseState>& base_subscriber,
                 rbt::FixedSubscriber<wire::NavigationStatus>& status_subscriber,
                 rbt::SliceSubscriber<wire::PathHeader, wire::PathPoint>& path_subscriber,
                 const GridMap& grid,
                 rbt::tf::DynamicTfPublisher& base_tf,
                 const rbt::time::HostClock& clock,
                 std::uint64_t& last_base_tf_sequence,
                 std::uint64_t& base_tf_publish_drops,
                 std::chrono::steady_clock::time_point& last_base_tf_drop_log,
                 FixtureState& observed) {
    native::drain(base_subscriber, [&](const wire::BaseState& state) {
        observed.base = state;
        observed.have_base = true;
        ++observed.base_state_frames;
        if (!grid.isFree(state.x, state.y, 0.18)) observed.all_footprints_free = false;
        const Pose2D pose{state.x, state.y, state.yaw};
        if (observed.have_previous_base_pose
            && !grid.segmentFree(observed.previous_base_pose, pose, 0.18)) {
            observed.all_pose_segments_free = false;
        }
        observed.previous_base_pose = pose;
        observed.have_previous_base_pose = true;
        publishBaseTransform(base_tf, state, clock, last_base_tf_sequence, base_tf_publish_drops,
                             last_base_tf_drop_log);
    });
    native::drain(status_subscriber, [&](const wire::NavigationStatus& status) {
        observed.status = status;
        ++observed.status_count;
    });
    for (std::size_t i = 0; i < rbt::MAX_DRAIN_PER_SOURCE; ++i) {
        auto received = native::take(path_subscriber.receive());
        if (!received) break;
        if (received->header().goal_id != 1U) continue;
        const auto points = received->payload();
        if (points.empty()) continue;
        observed.path_seen = true;
        observed.path_points = std::max(observed.path_points,
                                        static_cast<std::uint64_t>(points.size()));
        std::optional<Pose2D> previous_point;
        for (const auto& point : points) {
            if (point.y > 2.5) observed.path_above_wall = true;
            if (!grid.isFree(point.x, point.y, 0.18)) observed.path_footprint_free = false;
            const Pose2D pose{point.x, point.y, point.yaw};
            if (previous_point && !grid.segmentFree(*previous_point, pose, 0.18)) {
                observed.path_segments_free = false;
            }
            previous_point = pose;
        }
    }
}

int run(int argc, char** argv) {
    native::Options options(argc, argv);
    const auto scenario = options.get("scenario", "success");
    const auto prefix = options.get("prefix");
    const auto map_id = options.get("tf-map");
    const auto odom_id = options.get("tf-odom");
    const auto runtime_pid_value = options.integer("runtime-pid", 0);
    const auto guard_pid_value = options.integer("guard-pid", 0);
    const auto map_pid_value = options.integer("map-pid", 0);
    const auto external_map_value = options.integer("external-map", 0);
    const auto cmd_vel_topic = options.get("cmd-vel-topic", "/cmd_vel");
    const auto desired_topic = options.get("desired-topic", prefix + "/desired_cmd_vel");
    if (scenario != "success" && scenario != "obstacle" && scenario != "cancel"
        && scenario != "dropout" && scenario != "watchdog" && scenario != "estop"
        && scenario != "guard_crash" && scenario != "expired" && scenario != "authority"
        && scenario != "cmd_vel" && scenario != "base_expired" && scenario != "map_dropout") {
        fail("unknown native fixture scenario: " + scenario);
    }
    const bool external_map = external_map_value == 1U;
    if (external_map_value > 1U || (external_map && scenario != "obstacle"
        && scenario != "map_dropout") || (scenario == "map_dropout" && !external_map)) {
        fail("external map mode is only valid for obstacle and map_dropout scenarios");
    }
    if (prefix.empty() || map_id.empty() || odom_id.empty() || map_id == odom_id) {
        fail("fixture requires a prefix and distinct map/odom TF UUIDs");
    }
    const bool runtime_pid_required = scenario == "watchdog" || scenario == "expired"
        || scenario == "authority" || scenario == "map_dropout";
    const bool guard_must_be_killed = scenario == "guard_crash" || scenario == "base_expired";
    if ((runtime_pid_required && runtime_pid_value <= 1)
        || ((guard_must_be_killed || scenario == "map_dropout") && guard_pid_value <= 1)
        || (runtime_pid_value != 0U && runtime_pid_value <= 1U)
        || (guard_pid_value != 0U && guard_pid_value <= 1U)
        || (external_map && map_pid_value <= 1U)
        || (!external_map && map_pid_value != 0U)
        || runtime_pid_value > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())
        || guard_pid_value > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())
        || map_pid_value > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
        fail("native fixture requires valid supplied process IDs for its fault scenario");
    }
    const auto runtime_pid = static_cast<pid_t>(runtime_pid_value);
    const auto guard_pid = static_cast<pid_t>(guard_pid_value);
    const auto map_pid = static_cast<pid_t>(map_pid_value);

    auto clock = native::hostClock();
    const auto attack_sequence_base = static_cast<std::uint64_t>(native::now(clock).ns)
        + 60ULL * static_cast<std::uint64_t>(second);
    const auto cells = makeMap(scenario == "map_dropout" ? "obstacle" : scenario);
    GridMap grid(OccupancyGrid{80, 60, 0.1, {0.0, 0.0, 0.0}, cells, 50});
    std::optional<rbt::SlicePublisher<wire::MapHeader, std::int8_t>> map_publisher;
    if (!external_map) {
        map_publisher.emplace(native::take(
            rbt::SlicePublisher<wire::MapHeader, std::int8_t>::create(
                native::stream(prefix, "map", wire::OCCUPANCY_MAP_DESCRIPTOR),
                wire::OCCUPANCY_MAP_DESCRIPTOR, cells.size())));
    }
    auto map_tf = rbt::tf::DynamicTfPublisher::open(map_id);
    auto base_tf = rbt::tf::DynamicTfPublisher::open(odom_id);
    rbt::tf::TransformBuffer watchdog_buffer;
    std::optional<rbt::tf::DynamicTfSubscriber> watchdog_congestion;
    auto goals = native::publisher<wire::GoalCommand>(prefix, "goal");
    auto emergency = native::publisher<wire::EmergencyStop>(prefix, "estop");
    auto base_subscriber = native::subscriber<wire::BaseState>(prefix, "base_state");
    auto status_subscriber = native::subscriber<wire::NavigationStatus>(prefix, "status");
    auto guard_subscriber = native::subscriberAt<wire::GuardState>(prefix + "/guard_state");
    auto final_subscriber = native::subscriberAt<wire::VelocityCommand>(cmd_vel_topic);
    std::optional<rbt::FixedPublisher<wire::VelocityCommand>> desired_attack;
    std::optional<rbt::FixedPublisher<wire::VelocityCommand>> final_attack;
    if (scenario == "expired" || scenario == "authority") {
        desired_attack.emplace(native::publisherAt<wire::VelocityCommand>(desired_topic));
    }
    if (scenario == "base_expired") {
        final_attack.emplace(native::publisherAt<wire::VelocityCommand>(cmd_vel_topic));
    }
    auto path_subscriber = native::take(rbt::SliceSubscriber<wire::PathHeader, wire::PathPoint>::create(
        native::stream(prefix, "path", wire::PLANNED_PATH_DESCRIPTOR),
        wire::PLANNED_PATH_DESCRIPTOR));

    FixtureState observed;
    std::uint64_t last_base_tf_sequence = 0;
    std::uint64_t base_tf_publish_drops = 0;
    auto last_base_tf_drop_log = std::chrono::steady_clock::time_point::min();
    const auto warmup_started = std::chrono::steady_clock::now();
    const auto test_deadline = scenario == "obstacle" ? std::chrono::seconds(24)
                                                       : std::chrono::seconds(14);
    auto next_goal = std::chrono::steady_clock::time_point::min();
    auto next_cancel = std::chrono::steady_clock::time_point::min();
    auto last_map = std::chrono::steady_clock::time_point::min();
    auto goal_started = std::chrono::steady_clock::time_point::min();
    auto trigger_started = std::chrono::steady_clock::time_point::min();
    bool goal_started_flag = false;
    bool goal_acknowledged = false;
    bool goal_sent = false;
    bool cancel_sent = false;
    bool estop_sent = false;
    bool map_tf_enabled = true;
    std::int64_t estop_stamp_ns = -1;
    std::uint64_t previous_base_sequence = 0;
    std::uint64_t base_frames_at_runtime_kill = 0;
    std::uint64_t base_sequence_at_runtime_kill = 0;
    std::uint64_t base_frames_at_guard_kill = 0;
    std::uint64_t base_sequence_at_guard_kill = 0;
    std::uint64_t base_frames_at_map_kill = 0;
    std::uint64_t base_sequence_at_map_kill = 0;
    std::uint64_t base_rejections_at_guard_kill = 0;
    std::uint64_t guard_rejections_at_fault = 0;
    std::uint64_t guard_sequence_at_runtime_kill = 0;
    std::uint64_t guard_sequence_at_estop = 0;
    unsigned stopped_frames = 0;
    unsigned guard_zero_frames_after_runtime_kill = 0;
    unsigned guard_zero_frames_after_estop = 0;
    unsigned base_zero_frames_after_map_kill = 0;
    unsigned guard_zero_frames_after_map_kill = 0;
    std::uint64_t statuses_after_estop = 0;
    std::uint64_t last_status_stamp_after_estop = 0;
    std::uint64_t statuses_after_guard_kill = 0;
    std::uint64_t last_status_stamp_after_guard_kill = 0;
    std::uint64_t statuses_after_map_kill = 0;
    std::uint64_t last_status_stamp_after_map_kill = 0;
    bool runtime_killed = false;
    bool guard_killed = false;
    bool map_server_killed = false;
    bool authority_attack_started = false;
    bool authority_attack_done = false;
    std::uint64_t authority_rejections_at_start = 0;
    // Stay above the live monotonic command sequence so these probes exercise
    // authority/expiry policy instead of being discarded as old replays.
    std::uint64_t authority_attack_sequence = attack_sequence_base;
    std::uint64_t expired_attack_sequence = attack_sequence_base + 1'000'000'000ULL;
    const std::uint64_t final_attack_sequence = attack_sequence_base + 2'000'000'000ULL;
    unsigned expired_attack_sends = 0;
    unsigned final_attack_sends = 0;
    bool terminal_observed = false;
    std::int64_t guard_kill_stamp_ns = -1;
    std::int64_t map_kill_stamp_ns = -1;
    std::uint64_t guard_sequence_at_map_kill = 0;
    std::uint64_t final_sequence_at_map_kill = 0;
    std::int64_t last_status_time = 0;
    auto next_attack = std::chrono::steady_clock::time_point::min();

    while (true) {
        const auto now_wall = std::chrono::steady_clock::now();
        const auto deadline = now_wall + std::chrono::milliseconds(20);
        const auto current = native::now(clock);

        if (!external_map && (last_map == std::chrono::steady_clock::time_point::min()
            || now_wall - last_map >= std::chrono::seconds(1))) {
            publishMap(*map_publisher, cells, clock);
            last_map = now_wall;
        }
        if (map_tf_enabled) publishMapTransform(map_tf, clock);

        drainInputs(base_subscriber, status_subscriber, path_subscriber, grid, base_tf,
                    clock, last_base_tf_sequence, base_tf_publish_drops,
                    last_base_tf_drop_log, observed);
        native::drain(guard_subscriber, [&](const wire::GuardState& state) {
            const auto stamp_ns = native::decodeStamp(state.stamp, current.clock);
            if (state.sequence == 0U || state.sequence <= observed.guard.sequence) {
                fail("guard diagnostic sequence did not increase");
            }
            if (state.allowed_source != 1U || state.output_source != 100U) {
                fail("guard diagnostic reported unexpected command authority");
            }
            observed.guard = state;
            observed.have_guard = true;
            ++observed.guard_state_frames;
            observed.last_guard_stamp_ns = static_cast<std::uint64_t>(stamp_ns);
        });
        native::drain(final_subscriber, [&](const wire::VelocityCommand& command) {
            const auto stamp_ns = native::decodeStamp(command.stamp, current.clock);
            ++observed.final_command_frames;
            if (command.source_id != 100U) {
                fail("final cmd_vel command did not come from the guard");
            }
            if (command.valid_until_ns <= stamp_ns
                || command.valid_until_ns - stamp_ns > 100'000'000) {
                fail("guard final cmd_vel validity interval was not within 100 ms");
            }
            if (scenario == "base_expired" && command.sequence >= final_attack_sequence
                && command.valid_until_ns <= current.ns) {
                ++observed.injected_expired_final_frames;
                return;
            }
            if (command.sequence == 0U || command.sequence <= observed.last_final_sequence) {
                fail("guard final cmd_vel sequence did not increase");
            }
            observed.last_final_sequence = command.sequence;
            observed.last_final_stamp_ns = static_cast<std::uint64_t>(stamp_ns);
            if (!std::isfinite(command.linear_x) || !std::isfinite(command.angular_z)
                || std::abs(command.linear_x) > 0.7 + 1e-9
                || std::abs(command.angular_z) > 1.8 + 1e-9) {
                fail("guard final cmd_vel exceeded finite speed bounds");
            }
            const bool guard_was_killed = guard_killed && guard_kill_stamp_ns >= 0;
            if (guard_was_killed && stamp_ns > guard_kill_stamp_ns) {
                fail("guard published a final cmd_vel command after it was killed");
            }
            if (command.valid_until_ns <= current.ns) {
                return;
            }
            observed.final_command = command;
            ++observed.valid_final_command_frames;
            const bool moving = isMoving(command);
            if (moving) {
                ++observed.guard_moving_frames;
                observed.guard_stopped_frames = 0;
                if (runtime_killed) guard_zero_frames_after_runtime_kill = 0;
                if (estop_sent) guard_zero_frames_after_estop = 0;
                if (map_server_killed) guard_zero_frames_after_map_kill = 0;
                observed.guard_moving_seen = true;
                const bool fault_started = cancel_sent || !map_tf_enabled || runtime_killed
                    || guard_killed || map_server_killed || estop_sent || authority_attack_started;
                if (!fault_started) observed.guard_moving_before_fault = true;
            } else {
                ++observed.guard_stopped_frames;
                if (runtime_killed && command.sequence > guard_sequence_at_runtime_kill) {
                    ++guard_zero_frames_after_runtime_kill;
                }
                if (estop_sent && command.sequence > guard_sequence_at_estop) {
                    ++guard_zero_frames_after_estop;
                }
                if (map_server_killed && command.sequence > final_sequence_at_map_kill
                    && stamp_ns > map_kill_stamp_ns) {
                    ++guard_zero_frames_after_map_kill;
                }
            }
        });
        if (observed.status.stamp.nanoseconds > last_status_time) {
            last_status_time = native::decodeStamp(observed.status.stamp, current.clock);
            if (estop_stamp_ns >= 0 && last_status_time > estop_stamp_ns) {
                ++statuses_after_estop;
                last_status_stamp_after_estop = static_cast<std::uint64_t>(last_status_time);
            }
            if (guard_killed && guard_kill_stamp_ns >= 0
                && last_status_time > guard_kill_stamp_ns
                && static_cast<std::uint64_t>(last_status_time) > last_status_stamp_after_guard_kill) {
                ++statuses_after_guard_kill;
                last_status_stamp_after_guard_kill = static_cast<std::uint64_t>(last_status_time);
            }
            if (map_server_killed && map_kill_stamp_ns >= 0
                && last_status_time > map_kill_stamp_ns
                && static_cast<std::uint64_t>(last_status_time) > last_status_stamp_after_map_kill) {
                ++statuses_after_map_kill;
                last_status_stamp_after_map_kill = static_cast<std::uint64_t>(last_status_time);
            }
        }
        if (observed.have_base && observed.base.sequence != previous_base_sequence) {
            previous_base_sequence = observed.base.sequence;
            const bool stopped = nearZero(observed.base);
            const bool base_watchdog_required = scenario == "guard_crash" || scenario == "base_expired";
            if (goal_started_flag && stopped
                && (scenario != "estop" || observed.base.emergency_stop != 0U)
                && (!base_watchdog_required || observed.base.watchdog_expired != 0U)) {
                ++stopped_frames;
            } else {
                stopped_frames = 0;
            }
            if (map_server_killed) {
                const auto base_stamp_ns = native::decodeStamp(observed.base.stamp, current.clock);
                if (observed.base.sequence > base_sequence_at_map_kill && stopped
                    && base_stamp_ns > map_kill_stamp_ns) {
                    ++base_zero_frames_after_map_kill;
                } else {
                    base_zero_frames_after_map_kill = 0;
                }
            }
        }

        if (!goal_started_flag) {
            const bool map_ready = external_map ? observed.status.map_version != 0U
                                                : observed.status.map_version == 1U;
            const bool warm = now_wall - warmup_started >= std::chrono::seconds(1)
                && observed.have_base && observed.have_guard && map_ready;
            if (warm) {
                goal_started_flag = true;
                goal_started = now_wall;
                next_goal = now_wall;
            } else if (now_wall - warmup_started > std::chrono::seconds(6)) {
                fail("runtime did not receive fixture map and base state during startup");
            }
        }

        if (goal_started_flag && !goal_acknowledged) {
            if (!goal_sent || now_wall >= next_goal) {
                native::publish(goals, wire::GoalCommand{native::encodeStamp(current), 1U, 0U, 0U,
                    (scenario == "success" || scenario == "cmd_vel") ? 2.0 : 5.0,
                    1.0, (scenario == "success" || scenario == "cmd_vel") ? 0.3 : 0.0,
                    30 * second});
                goal_sent = true;
                next_goal = now_wall + std::chrono::milliseconds(100);
            }
            if (observed.status.goal_id == 1U && observed.status.state != idle_state) {
                goal_acknowledged = true;
            }
        }

        if (goal_started_flag && now_wall - goal_started > test_deadline) {
            fail("scenario timed out; state=" + std::to_string(observed.status.state)
                 + " error=" + std::to_string(observed.status.error));
        }
        if (goal_acknowledged && observed.status.goal_id == 1U
            && observed.status.state == failed_state && scenario != "dropout"
            && scenario != "map_dropout") {
            const auto elapsed = std::chrono::duration<double>(now_wall - goal_started).count();
            fail("navigation failed unexpectedly with error=" + std::to_string(observed.status.error)
                 + " elapsed_s=" + std::to_string(elapsed)
                 + " base=(" + std::to_string(observed.base.x) + ","
                 + std::to_string(observed.base.y) + "," + std::to_string(observed.base.yaw) + ")"
                 + " velocity=(" + std::to_string(observed.base.linear_x) + ","
                 + std::to_string(observed.base.angular_z) + ") path_points="
                 + std::to_string(observed.path_points) + " path_above_wall="
                 + std::to_string(observed.path_above_wall) + " footprints_free="
                 + std::to_string(observed.all_footprints_free) + " path_footprint_free="
                 + std::to_string(observed.path_footprint_free) + " pose_segments_free="
                 + std::to_string(observed.all_pose_segments_free) + " path_segments_free="
                 + std::to_string(observed.path_segments_free));
        }

        if (goal_acknowledged && (scenario == "success" || scenario == "cmd_vel")
            && observed.status.goal_id == 1U
            && observed.status.state == succeeded_state) {
            terminal_observed = true;
            if (std::hypot(observed.base.x - 2.0, observed.base.y - 1.0) >= 0.12
                || std::abs(normalizeAngle(observed.base.yaw - 0.3)) >= 0.16) {
                fail("successful goal ended outside the requested pose tolerance");
            }
            if (!observed.path_seen || observed.path_points == 0U) fail("successful goal published no path");
            if (stopped_frames >= 5U && observed.guard_stopped_frames >= 2U) break;
        } else if (goal_acknowledged && scenario == "obstacle") {
            if (!observed.all_footprints_free || !observed.path_footprint_free
                || !observed.all_pose_segments_free || !observed.path_segments_free) {
                fail("obstacle scenario violated GridMap footprint clearance");
            }
            if (observed.status.goal_id == 1U && observed.status.state == succeeded_state) {
                terminal_observed = true;
                if (std::hypot(observed.base.x - 5.0, observed.base.y - 1.0) >= 0.12) {
                    fail("obstacle goal ended outside the requested position tolerance");
                }
                if (!observed.path_seen || !observed.path_above_wall) {
                    fail("obstacle route did not publish a path through the upper gap");
                }
                if (stopped_frames >= 5U && observed.guard_stopped_frames >= 2U) break;
            }
        } else if (goal_acknowledged && scenario == "cancel") {
            if (!cancel_sent && observed.have_base && isMoving(observed.base)
                && observed.guard_moving_before_fault) {
                const auto stamp = native::now(clock);
                native::publish(goals, wire::GoalCommand{native::encodeStamp(stamp), 1U, 1U, 0U,
                    0.0, 0.0, 0.0, 0});
                cancel_sent = true;
                trigger_started = now_wall;
                next_cancel = now_wall + std::chrono::milliseconds(100);
            }
            if (cancel_sent && observed.status.goal_id == 1U && observed.status.state == canceled_state) {
                terminal_observed = true;
                if (now_wall - trigger_started > std::chrono::seconds(1)
                    && (stopped_frames < 5U || observed.guard_stopped_frames < 2U)) {
                    fail("base did not stop within one second after cancellation");
                }
                if (stopped_frames >= 5U && observed.guard_stopped_frames >= 2U) break;
            } else if (cancel_sent && now_wall >= next_cancel) {
                native::publish(goals, wire::GoalCommand{native::encodeStamp(current), 1U, 1U, 0U,
                    0.0, 0.0, 0.0, 0});
                next_cancel = now_wall + std::chrono::milliseconds(100);
            }
        } else if (goal_acknowledged && scenario == "dropout") {
            if (!map_tf_enabled) {
                if (observed.status.goal_id == 1U && observed.status.state == failed_state
                    && (observed.status.error == localization_unavailable || observed.status.error == stale_tf)) {
                    terminal_observed = true;
                    if (now_wall - trigger_started > std::chrono::seconds(1)
                        && (stopped_frames < 5U || observed.guard_stopped_frames < 2U)) {
                        fail("base did not stop within one second after localization loss");
                    }
                    if (stopped_frames >= 5U && observed.guard_stopped_frames >= 2U) break;
                }
            } else if (observed.have_base && isMoving(observed.base)
                && observed.guard_moving_before_fault) {
                map_tf_enabled = false;
                trigger_started = now_wall;
            }
        } else if (goal_acknowledged && scenario == "map_dropout") {
            if (!map_server_killed && observed.have_base && isMoving(observed.base)
                && observed.guard_moving_before_fault) {
                const auto fault_stamp = native::now(clock);
                if (::kill(map_pid, SIGKILL) != 0) fail("could not SIGKILL supplied map server");
                map_kill_stamp_ns = fault_stamp.ns;
                map_server_killed = true;
                trigger_started = std::chrono::steady_clock::now();
                base_frames_at_map_kill = observed.base_state_frames;
                base_sequence_at_map_kill = observed.base.sequence;
                guard_sequence_at_map_kill = observed.guard.sequence;
                final_sequence_at_map_kill = observed.last_final_sequence;
                stopped_frames = 0;
                base_zero_frames_after_map_kill = 0;
                guard_zero_frames_after_map_kill = 0;
                std::cerr << "native fixture: map_dropout fault_start_ns=" << map_kill_stamp_ns
                          << " map_server_pid=" << map_pid << '\n';
            }
            if (map_server_killed) {
                const bool map_failure = observed.status.goal_id == 1U
                    && observed.status.state == failed_state
                    && (observed.status.error == stale_map || observed.status.error == map_unavailable);
                const bool fresh_base_zeros = observed.have_base
                    && observed.base_state_frames >= base_frames_at_map_kill + 5U
                    && observed.base.sequence > base_sequence_at_map_kill
                    && native::decodeStamp(observed.base.stamp, current.clock) > map_kill_stamp_ns
                    && base_zero_frames_after_map_kill >= 5U;
                const bool fresh_guard_zeros = guard_zero_frames_after_map_kill >= 2U
                    && observed.last_final_stamp_ns > static_cast<std::uint64_t>(map_kill_stamp_ns)
                    && observed.guard.sequence > guard_sequence_at_map_kill
                    && observed.last_guard_stamp_ns > static_cast<std::uint64_t>(map_kill_stamp_ns);
                const bool fresh_status = statuses_after_map_kill >= 2U
                    && last_status_stamp_after_map_kill > static_cast<std::uint64_t>(map_kill_stamp_ns);
                const bool live_watchdogs = observed.have_guard && observed.have_base
                    && observed.guard.watchdog_expired == 0U
                    && observed.base.watchdog_expired == 0U;
                if (map_failure && fresh_base_zeros && fresh_guard_zeros && fresh_status
                    && live_watchdogs && ::kill(runtime_pid, 0) == 0 && ::kill(guard_pid, 0) == 0
                    && std::chrono::steady_clock::now() - trigger_started
                        >= std::chrono::milliseconds(200)) {
                    terminal_observed = true;
                    break;
                }
                if (std::chrono::steady_clock::now() - trigger_started > std::chrono::seconds(3)) {
                    fail("map server dropout did not produce fresh navigation failure and zero output"
                         " within 3 seconds: state=" + std::to_string(observed.status.state)
                         + " error=" + std::to_string(observed.status.error)
                         + " base_zero_frames=" + std::to_string(base_zero_frames_after_map_kill)
                         + " guard_zero_frames=" + std::to_string(guard_zero_frames_after_map_kill)
                         + " statuses=" + std::to_string(statuses_after_map_kill)
                         + " base_watchdog=" + std::to_string(observed.base.watchdog_expired)
                         + " guard_watchdog=" + std::to_string(observed.guard.watchdog_expired));
                }
            }
        } else if (goal_acknowledged
                   && (scenario == "watchdog" || scenario == "expired")) {
            if (!runtime_killed && observed.have_base && isMoving(observed.base)
                && observed.guard_moving_before_fault && observed.have_guard) {
                guard_rejections_at_fault = observed.guard.rejected_commands;
                if (::kill(runtime_pid, SIGKILL) != 0) fail("could not SIGKILL navigation runtime");
                guard_sequence_at_runtime_kill = observed.last_final_sequence;
                runtime_killed = true;
                map_tf_enabled = false;
                trigger_started = now_wall;
                base_frames_at_runtime_kill = observed.base_state_frames;
                base_sequence_at_runtime_kill = observed.base.sequence;
                watchdog_congestion.emplace(rbt::tf::DynamicTfSubscriber::open(
                    odom_id, "watchdog_congestion", watchdog_buffer));
                next_attack = now_wall;
            }
            if (scenario == "expired" && runtime_killed
                && now_wall - trigger_started <= std::chrono::milliseconds(500)
                && now_wall >= next_attack) {
                auto expired = wire::VelocityCommand{};
                expired.stamp = native::encodeStamp(current);
                expired.stamp.nanoseconds = std::max<std::int64_t>(0, current.ns - 200'000'000);
                expired.source_id = 1U;
                expired.sequence = expired_attack_sequence++;
                if (expired.sequence <= static_cast<std::uint64_t>(current.ns)) {
                    fail("expired raw probe sequence fell below the monotonic replay watermark");
                }
                expired.valid_until_ns = expired.stamp.nanoseconds + 80'000'000;
                expired.linear_x = 999.0;
                expired.angular_z = 999.0;
                native::publish(*desired_attack, expired);
                ++expired_attack_sends;
                next_attack = now_wall + std::chrono::milliseconds(10);
            }
            const bool fresh_base_after_kill = runtime_killed && observed.have_base
                && observed.base_state_frames >= base_frames_at_runtime_kill + 5U
                && observed.base.sequence > base_sequence_at_runtime_kill;
            const bool watchdog_stopped = fresh_base_after_kill && observed.have_guard
                && observed.guard.watchdog_expired != 0U && nearZero(observed.base)
                && stopped_frames >= 5U
                && observed.guard_stopped_frames >= 2U
                && guard_zero_frames_after_runtime_kill >= 2U
                && (scenario != "expired" || (expired_attack_sends >= 10U
                    && observed.guard.rejected_commands > guard_rejections_at_fault))
                && now_wall - trigger_started >= std::chrono::milliseconds(200);
            if (watchdog_stopped) {
                terminal_observed = true;
                break;
            }
            if (runtime_killed && now_wall - trigger_started > std::chrono::milliseconds(500)) {
                fail("guard watchdog did not stop within 500 ms: guard_watchdog="
                     + std::to_string(observed.guard.watchdog_expired)
                     + " guard_zero_after_kill=" + std::to_string(guard_zero_frames_after_runtime_kill)
                     + " base_stopped_frames=" + std::to_string(stopped_frames)
                     + " expired_probes=" + std::to_string(expired_attack_sends)
                     + " guard_rejected_delta="
                     + std::to_string(observed.guard.rejected_commands - guard_rejections_at_fault));
            }
        } else if (goal_acknowledged && scenario == "guard_crash") {
            if (!guard_killed && observed.have_base && isMoving(observed.base)
                && observed.guard_moving_before_fault) {
                if (::kill(guard_pid, SIGKILL) != 0) fail("could not SIGKILL velocity guard");
                guard_killed = true;
                guard_kill_stamp_ns = native::now(clock).ns;
                trigger_started = now_wall;
                base_frames_at_guard_kill = observed.base_state_frames;
                base_sequence_at_guard_kill = observed.base.sequence;
            }
            const bool fresh_base_after_guard_kill = guard_killed && observed.have_base
                && observed.base_state_frames >= base_frames_at_guard_kill + 5U
                && observed.base.sequence > base_sequence_at_guard_kill;
            const bool base_watchdog_stopped = fresh_base_after_guard_kill
                && observed.base.watchdog_expired != 0U && nearZero(observed.base)
                && stopped_frames >= 5U && statuses_after_guard_kill >= 2U
                && now_wall - trigger_started >= std::chrono::milliseconds(200);
            if (base_watchdog_stopped) {
                terminal_observed = true;
                break;
            }
            if (guard_killed && now_wall - trigger_started > std::chrono::milliseconds(500)) {
                fail("base watchdog or live runtime status did not survive guard crash within 500 ms");
            }
        } else if (goal_acknowledged && scenario == "base_expired") {
            if (!guard_killed && observed.have_base && isMoving(observed.base)
                && observed.guard_moving_before_fault) {
                if (::kill(guard_pid, SIGKILL) != 0) fail("could not SIGKILL velocity guard");
                guard_killed = true;
                guard_kill_stamp_ns = native::now(clock).ns;
                trigger_started = now_wall;
                base_frames_at_guard_kill = observed.base_state_frames;
                base_sequence_at_guard_kill = observed.base.sequence;
                base_rejections_at_guard_kill = observed.base.rejected_commands;
                next_attack = now_wall;
            }
            if (guard_killed && now_wall - trigger_started <= std::chrono::milliseconds(500)
                && now_wall >= next_attack) {
                auto expired = wire::VelocityCommand{};
                expired.stamp = native::encodeStamp(current);
                expired.stamp.nanoseconds = std::max<std::int64_t>(0, current.ns - 200'000'000);
                expired.source_id = 100U;
                expired.sequence = final_attack_sequence + final_attack_sends++;
                if (expired.sequence <= static_cast<std::uint64_t>(current.ns)) {
                    fail("expired final probe sequence fell below the monotonic replay watermark");
                }
                expired.valid_until_ns = expired.stamp.nanoseconds + 80'000'000;
                expired.linear_x = 999.0;
                expired.angular_z = 999.0;
                native::publish(*final_attack, expired);
                next_attack = now_wall + std::chrono::milliseconds(10);
            }
            const bool fresh_base_after_guard_kill = guard_killed && observed.have_base
                && observed.base_state_frames >= base_frames_at_guard_kill + 5U
                && observed.base.sequence > base_sequence_at_guard_kill;
            const bool base_watchdog_stopped = fresh_base_after_guard_kill
                && observed.base.watchdog_expired != 0U && nearZero(observed.base)
                && stopped_frames >= 5U && final_attack_sends >= 10U
                && observed.injected_expired_final_frames > 0U
                && observed.base.rejected_commands > base_rejections_at_guard_kill
                && now_wall - trigger_started >= std::chrono::milliseconds(200);
            if (base_watchdog_stopped) {
                terminal_observed = true;
                break;
            }
            if (guard_killed && now_wall - trigger_started > std::chrono::milliseconds(500)) {
                fail("base watchdog did not stop within 500 ms: base_watchdog="
                     + std::to_string(observed.base.watchdog_expired)
                     + " base_stopped_frames=" + std::to_string(stopped_frames)
                     + " final_probes=" + std::to_string(final_attack_sends)
                     + " observed_expired_final="
                     + std::to_string(observed.injected_expired_final_frames)
                     + " base_rejected_delta="
                     + std::to_string(observed.base.rejected_commands - base_rejections_at_guard_kill));
            }
        } else if (goal_acknowledged && scenario == "authority") {
            if (!authority_attack_started && observed.have_base && isMoving(observed.base)
                && observed.guard_moving_before_fault && observed.have_guard) {
                authority_attack_started = true;
                authority_rejections_at_start = observed.guard.rejected_commands;
                trigger_started = now_wall;
                next_attack = now_wall;
            }
            if (authority_attack_started && !authority_attack_done) {
                if (now_wall - trigger_started < std::chrono::milliseconds(300)
                    && now_wall >= next_attack) {
                    auto unauthorized = wire::VelocityCommand{};
                    unauthorized.stamp = native::encodeStamp(current);
                    unauthorized.source_id = 99U;
                    unauthorized.sequence = authority_attack_sequence++;
                    if (unauthorized.sequence <= static_cast<std::uint64_t>(current.ns)) {
                        fail("authority probe sequence fell below the monotonic replay watermark");
                    }
                    unauthorized.valid_until_ns = current.ns + 80'000'000;
                    unauthorized.linear_x = 999.0;
                    unauthorized.angular_z = 999.0;
                    native::publish(*desired_attack, unauthorized);
                    next_attack = now_wall + std::chrono::milliseconds(10);
                } else if (now_wall - trigger_started >= std::chrono::milliseconds(300)) {
                    if (observed.guard.rejected_commands <= authority_rejections_at_start) {
                        fail("guard did not count wrong-source desired commands as rejected");
                    }
                    if (::kill(runtime_pid, 0) != 0) {
                        fail("navigation runtime exited during authority attack");
                    }
                    authority_attack_done = true;
                    const auto stamp = native::now(clock);
                    native::publish(goals, wire::GoalCommand{native::encodeStamp(stamp), 1U, 1U,
                        0U, 0.0, 0.0, 0.0, 0});
                    cancel_sent = true;
                    trigger_started = now_wall;
                    next_cancel = now_wall + std::chrono::milliseconds(100);
                }
            }
            if (cancel_sent && observed.status.goal_id == 1U
                && observed.status.state == canceled_state) {
                terminal_observed = true;
                if (now_wall - trigger_started > std::chrono::seconds(1)
                    && (stopped_frames < 5U || observed.guard_stopped_frames < 2U)) {
                    fail("base did not stop within one second after authority attack cancellation");
                }
                if (stopped_frames >= 5U && observed.guard_stopped_frames >= 2U) break;
            } else if (cancel_sent && now_wall >= next_cancel) {
                native::publish(goals, wire::GoalCommand{native::encodeStamp(current), 1U, 1U,
                    0U, 0.0, 0.0, 0.0, 0});
                next_cancel = now_wall + std::chrono::milliseconds(100);
            }
        } else if (goal_acknowledged && scenario == "estop") {
            if (!estop_sent && observed.have_base && isMoving(observed.base)
                && observed.guard_moving_before_fault) {
                const auto stamp = native::now(clock);
                estop_stamp_ns = stamp.ns;
                native::publish(emergency, wire::EmergencyStop{native::encodeStamp(stamp), 2U, 1U, 1U, 0U});
                guard_sequence_at_estop = observed.last_final_sequence;
                estop_sent = true;
                trigger_started = now_wall;
            }
            if (estop_sent && observed.have_base && observed.base.emergency_stop != 0U
                && nearZero(observed.base) && observed.have_guard
                && observed.guard.emergency_stop != 0U && observed.guard_stopped_frames >= 2U
                && guard_zero_frames_after_estop >= 2U
                && statuses_after_estop >= 2U) {
                terminal_observed = true;
                break;
            }
            if (estop_sent && now_wall - trigger_started > std::chrono::seconds(2)
                && (statuses_after_estop < 2U || !observed.have_guard
                    || observed.guard.emergency_stop == 0U || observed.guard_stopped_frames < 2U
                    || guard_zero_frames_after_estop < 2U)) {
                fail("emergency stop did not produce guarded zero output and continued status");
            }
        }

        std::this_thread::sleep_until(deadline);
    }

    if (!goal_acknowledged || !terminal_observed) fail("scenario ended without its required terminal evidence");
    if (!observed.guard_moving_seen || !observed.guard_moving_before_fault) {
        fail("scenario did not observe moving final cmd_vel from the guard before its terminal/fault event");
    }
    if (scenario != "guard_crash" && scenario != "base_expired"
        && observed.guard_stopped_frames < 2U) {
        fail("scenario did not observe fresh terminal zero commands from the live guard");
    }
    if ((scenario == "obstacle" || scenario == "map_dropout")
        && (!observed.all_footprints_free || !observed.path_footprint_free
        || !observed.all_pose_segments_free || !observed.path_segments_free)) {
        fail("obstacle scenario violated GridMap footprint clearance");
    }
    if ((scenario == "watchdog" || scenario == "expired") && !runtime_killed) {
        fail("runtime-fault scenario did not kill the supplied runtime process");
    }
    if ((scenario == "guard_crash" || scenario == "base_expired") && !guard_killed) {
        fail("guard-fault scenario did not kill the supplied guard process");
    }
    if (scenario == "map_dropout" && (!map_server_killed
        || statuses_after_map_kill < 2U || last_status_stamp_after_map_kill <= 0U
        || guard_zero_frames_after_map_kill < 2U || base_zero_frames_after_map_kill < 5U
        || observed.guard.watchdog_expired != 0U || observed.base.watchdog_expired != 0U
        || observed.last_final_stamp_ns <= static_cast<std::uint64_t>(map_kill_stamp_ns)
        || observed.last_guard_stamp_ns <= static_cast<std::uint64_t>(map_kill_stamp_ns)
        || last_status_stamp_after_map_kill <= static_cast<std::uint64_t>(map_kill_stamp_ns)
        || ::kill(runtime_pid, 0) != 0 || ::kill(guard_pid, 0) != 0)) {
        fail("map server dropout did not preserve fresh live runtime, guard, and base evidence");
    }
    if (scenario == "guard_crash" && statuses_after_guard_kill < 2U) {
        fail("navigation runtime did not keep publishing fresh statuses after guard crash");
    }
    if (scenario == "base_expired" && observed.injected_expired_final_frames == 0U) {
        fail("expired final cmd_vel injections were not recognized separately");
    }
    if (scenario == "estop" && (statuses_after_estop < 2U || last_status_stamp_after_estop <= 0U)) {
        fail("runtime did not keep publishing status after emergency stop");
    }

    std::cout << "PASS native " << scenario << ": state=" << observed.status.state
              << " error=" << observed.status.error << " path_points=" << observed.path_points
              << " guard_cmds=" << observed.valid_final_command_frames
              << " guard_rejected=" << observed.guard.rejected_commands
              << " guard_watchdog=" << observed.guard.watchdog_expired
              << " base_watchdog=" << observed.base.watchdog_expired
              << " base_rejected=" << observed.base.rejected_commands
              << " expired_raw_probes=" << expired_attack_sends
              << " expired_final_probes=" << final_attack_sends;
    if (scenario == "map_dropout") {
        std::cout << " map_server_killed=" << map_server_killed
                  << " map_zero_base_frames=" << base_zero_frames_after_map_kill
                  << " map_zero_guard_frames=" << guard_zero_frames_after_map_kill
                  << " statuses_after_map_kill=" << statuses_after_map_kill;
    }
    if (trigger_started != std::chrono::steady_clock::time_point::min()) {
        std::cout << " fault_elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - trigger_started).count();
    }
    if (observed.have_base) {
        std::cout << " pose=(" << observed.base.x << ',' << observed.base.y << ',' << observed.base.yaw
                  << ") velocity=(" << observed.base.linear_x << ',' << observed.base.angular_z << ')';
    }
    std::cout << '\n';
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "FAIL native fixture: " << error.what() << '\n';
        return 1;
    }
}

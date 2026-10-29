#include <rbt_hardware/native.hpp>
#include <rbt/navwire.hpp>
#include <rbt/tf_transport.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <signal.h>
#include <string>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <vector>

namespace hw = rbt::hardware;
namespace wire = rbt::navwire;
namespace native = rbt::hardware::native;
using SteadyClock = std::chrono::steady_clock;
constexpr std::int64_t second = 1'000'000'000;

namespace {

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

void require(bool condition, const std::string& message) {
    if (!condition) fail(message);
}

bool nearZero(double value, double tolerance = 0.01) {
    return std::isfinite(value) && std::abs(value) <= tolerance;
}

bool statusStopped(const hw::BaseStatus& status) {
    return status.left_motor_command == 0 && status.right_motor_command == 0;
}

std::string frameName(const std::uint8_t* value, std::size_t size) {
    const auto end = std::find(value, value + size, 0);
    return {reinterpret_cast<const char*>(value), static_cast<std::size_t>(end - value)};
}

void validateFrame(const std::uint8_t* value, std::size_t size,
                   const std::string& expected, const char* field) {
    require(frameName(value, size) == expected, std::string("unexpected ") + field + " frame");
    const auto end = value + expected.size();
    require(std::all_of(end, value + size, [](std::uint8_t byte) { return byte == 0; }),
            std::string("nonzero padding in ") + field + " frame");
}

struct Options {
    std::string scenario;
    std::string prefix;
    std::string desired_topic;
    std::string cmd_topic;
    std::string imu_topic;
    std::string phase_file;
    std::string map_tf_id;
    std::string odom_tf_id;
    std::uint64_t guard_pid{};
    std::uint64_t runtime_pid{};

    Options(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--help") {
                throw std::invalid_argument(
                    "--scenario pipeline|navigation|navigation_crash --prefix NAME --phase-file PATH "
                    "[--guard-pid PID] [--runtime-pid PID] [--tf-map UUID --tf-odom UUID]");
            }
            if (i + 1 >= argc) throw std::invalid_argument("missing value for " + key);
            const std::string value = argv[++i];
            if (key == "--scenario") scenario = value;
            else if (key == "--prefix") prefix = value;
            else if (key == "--phase-file") phase_file = value;
            else if (key == "--tf-map") map_tf_id = value;
            else if (key == "--tf-odom") odom_tf_id = value;
            else if (key == "--guard-pid") guard_pid = std::stoull(value);
            else if (key == "--runtime-pid") runtime_pid = std::stoull(value);
            else throw std::invalid_argument("unknown option: " + key);
        }
        if (scenario != "pipeline" && scenario != "navigation" && scenario != "navigation_crash")
            throw std::invalid_argument("unknown hardware IPC scenario: " + scenario);
        if (prefix.empty() || phase_file.empty())
            throw std::invalid_argument("scenario requires a prefix and phase file");
        desired_topic = prefix + "/desired_cmd_vel";
        cmd_topic = "/" + prefix + "/cmd_vel";
        imu_topic = "/" + prefix + "/imu";
        if (scenario == "pipeline" && guard_pid <= 1)
            throw std::invalid_argument("pipeline requires the supplied guard child PID");
        if (scenario == "navigation_crash" && runtime_pid <= 1)
            throw std::invalid_argument("navigation_crash requires the supplied runtime child PID");
        if ((scenario == "navigation" || scenario == "navigation_crash")
            && (map_tf_id.empty() || odom_tf_id.empty()
            || map_tf_id == odom_tf_id))
            throw std::invalid_argument("navigation requires distinct TF UUIDs");
    }
};

class Fixture {
public:
    explicit Fixture(Options options)
        : options_(std::move(options)), clock_(native::hostClock()),
          raw_(native::publisher<wire::VelocityCommand>(options_.desired_topic)),
          estop_(native::publisher<wire::EmergencyStop>(options_.prefix + "/estop")),
          final_probe_(options_.scenario == "pipeline"
              ? std::optional<rbt::FixedPublisher<wire::VelocityCommand>>(
                    native::publisher<wire::VelocityCommand>(options_.cmd_topic))
              : std::nullopt),
          base_input_(options_.prefix + "/base_status"),
          wheel_input_(options_.prefix + "/wheel_odom"),
          odom_input_(options_.prefix + "/odom"),
          imu_input_(options_.imu_topic),
          guard_input_(options_.prefix + "/guard_state"),
          nav_status_input_(options_.prefix + "/status") {
        std::ofstream clear(options_.phase_file, std::ios::out | std::ios::trunc);
        if (!clear) fail("cannot initialize phase file");
    }

    void run() {
        std::cout << "hardware IPC fixture ready: scenario=" << options_.scenario
                  << " prefix=" << options_.prefix << std::endl;
        if (options_.scenario == "pipeline") runPipeline();
        else runNavigation();
    }

private:
    struct OdometryObservation {
        std::int64_t stamp{};
        double linear_x{};
        double angular_z{};
    };

    Options options_;
    rbt::time::HostClock clock_;
    rbt::FixedPublisher<wire::VelocityCommand> raw_;
    rbt::FixedPublisher<wire::EmergencyStop> estop_;
    std::optional<rbt::FixedPublisher<wire::VelocityCommand>> final_probe_;
    native::Input<hw::BaseStatus> base_input_;
    native::Input<hw::WheelOdometry> wheel_input_;
    native::Input<wire::Odom2D> odom_input_;
    native::Input<hw::Imu> imu_input_;
    native::Input<wire::GuardState> guard_input_;
    native::Input<wire::NavigationStatus> nav_status_input_;
    std::optional<hw::BaseStatus> base_;
    std::optional<hw::WheelOdometry> wheel_;
    std::optional<hw::Imu> imu_;
    std::optional<wire::GuardState> guard_;
    std::optional<wire::NavigationStatus> nav_status_;
    std::uint64_t base_frames_{};
    std::uint64_t wheel_frames_{};
    std::uint64_t imu_frames_{};
    std::uint64_t guard_frames_{};
    std::uint64_t nav_status_frames_{};
    std::uint64_t raw_sequence_{};
    std::uint64_t direct_sequence_{};
    std::uint64_t estop_sequence_{};
    std::uint64_t previous_base_sequence_{};
    std::uint64_t previous_wheel_sequence_{};
    std::uint64_t previous_odom_sequence_{};
    std::uint64_t previous_imu_sequence_{};
    std::uint64_t previous_guard_sequence_{};
    unsigned stopped_status_run_{};
    unsigned stopped_wheel_run_{};
    unsigned stopped_status_after_guard_kill_{};
    unsigned stopped_wheel_after_guard_kill_{};
    unsigned stopped_status_after_runtime_kill_{};
    unsigned stopped_wheel_after_runtime_kill_{};
    unsigned watchdog_guard_frames_after_runtime_kill_{};
    std::uint64_t raw_sends_after_guard_kill_{};
    bool raw_enabled_{};
    bool direct_enabled_{};
    double direct_linear_{0.05};
    double direct_angular_{0.2};
    SteadyClock::time_point next_command_{SteadyClock::time_point::min()};
    SteadyClock::time_point next_direct_{SteadyClock::time_point::min()};
    SteadyClock::time_point next_map_{SteadyClock::time_point::min()};
    std::uint64_t odom_pairs_{};
    std::unordered_map<std::uint64_t, OdometryObservation> wheel_observations_;
    std::unordered_map<std::uint64_t, OdometryObservation> odom_observations_;

    void phase(const std::string& name) const {
        std::ofstream output(options_.phase_file, std::ios::out | std::ios::app);
        if (!output) fail("cannot append phase file");
        const auto timestamp = now();
        output << name << ' ' << timestamp.ns << '\n';
        output.flush();
        if (!output) fail("cannot flush phase file");
        std::cout << "PHASE " << name << ' ' << timestamp.ns << std::endl;
    }

    rbt::time::Timestamp now() const { return native::now(clock_); }

    template<class T>
    void publishCommand(rbt::FixedPublisher<T>& publisher, const T& command) {
        native::publish(publisher, command);
    }

    void sendRaw() {
        const auto stamp = now();
        if (raw_sequence_ == std::numeric_limits<std::uint64_t>::max())
            fail("raw command sequence exhausted");
        wire::VelocityCommand command{};
        command.stamp = native::encodeStamp<wire::Stamp>(stamp);
        command.source_id = 1;
        command.sequence = ++raw_sequence_;
        command.valid_until_ns = stamp.ns + 100'000'000;
        command.linear_x = 0.05;
        command.angular_z = 0.2;
        publishCommand(raw_, command);
    }

    void sendDirect(double linear, double angular, std::int64_t valid_for_ns = 250'000'000) {
        if (!final_probe_) fail("final probe publisher missing");
        const auto stamp = now();
        wire::VelocityCommand command{};
        command.stamp = native::encodeStamp<wire::Stamp>(stamp);
        command.source_id = 100;
        command.sequence = ++direct_sequence_;
        command.valid_until_ns = stamp.ns + valid_for_ns;
        command.linear_x = linear;
        command.angular_z = angular;
        publishCommand(*final_probe_, command);
    }

    void sendEmergency(bool active) {
        const auto stamp = now();
        wire::EmergencyStop command{};
        command.stamp = native::encodeStamp<wire::Stamp>(stamp);
        command.source_id = 2;
        command.sequence = ++estop_sequence_;
        command.active = active ? 1U : 0U;
        publishCommand(estop_, command);
    }

    void validateBase(const hw::BaseStatus& status, const rbt::time::Timestamp& current) {
        const auto stamp = native::decodeStamp(status.stamp, current.clock);
        require(status.sequence != 0 && status.sequence > previous_base_sequence_,
                "BaseStatus sequence did not increase");
        require(native::fresh(stamp, current.ns, 250'000'000), "BaseStatus stamp was stale/future-dated");
        previous_base_sequence_ = status.sequence;
        ++base_frames_;
        base_ = status;
        if (statusStopped(status)) ++stopped_status_run_;
        else stopped_status_run_ = 0;
        if (guard_kill_started_) {
            if (statusStopped(status) && status.watchdog_expired != 0U)
                ++stopped_status_after_guard_kill_;
            else
                stopped_status_after_guard_kill_ = 0;
        }
        if (runtime_kill_started_) {
            if (statusStopped(status) && status.watchdog_expired == 0U)
                ++stopped_status_after_runtime_kill_;
            else
                stopped_status_after_runtime_kill_ = 0;
        }
    }

    void validateWheel(const hw::WheelOdometry& wheel, const rbt::time::Timestamp& current,
                       const std::function<void(const hw::WheelOdometry&,
                                                const rbt::time::Timestamp&)>& callback) {
        const auto stamp = native::decodeStamp(wheel.stamp, current.clock);
        require(wheel.sequence != 0 && wheel.sequence > previous_wheel_sequence_,
                "WheelOdometry sequence did not increase");
        require(native::fresh(stamp, current.ns, 250'000'000),
                "WheelOdometry stamp was stale/future-dated");
        require(std::isfinite(wheel.x) && std::isfinite(wheel.y) && std::isfinite(wheel.yaw)
                && std::isfinite(wheel.linear_x) && std::isfinite(wheel.angular_z)
                && std::isfinite(wheel.left_rpm) && std::isfinite(wheel.right_rpm),
                "WheelOdometry contained nonfinite values");
        validateFrame(wheel.frame, std::size(wheel.frame), "odom", "odometry");
        validateFrame(wheel.child_frame, std::size(wheel.child_frame), "base_footprint", "odometry child");
        previous_wheel_sequence_ = wheel.sequence;
        ++wheel_frames_;
        wheel_ = wheel;
        wheel_observations_[wheel.sequence] = {stamp, wheel.linear_x, wheel.angular_z};
        if (nearZero(wheel.left_rpm, 0.5) && nearZero(wheel.right_rpm, 0.5)) ++stopped_wheel_run_;
        else stopped_wheel_run_ = 0;
        if (guard_kill_started_) {
            if (nearZero(wheel.left_rpm, 0.5) && nearZero(wheel.right_rpm, 0.5))
                ++stopped_wheel_after_guard_kill_;
            else
                stopped_wheel_after_guard_kill_ = 0;
        }
        if (runtime_kill_started_) {
            if (nearZero(wheel.left_rpm, 0.5) && nearZero(wheel.right_rpm, 0.5))
                ++stopped_wheel_after_runtime_kill_;
            else
                stopped_wheel_after_runtime_kill_ = 0;
        }
        if (callback) callback(wheel, current);
    }

    void validateNavigationOdometry(const wire::Odom2D& odom,
                                   const rbt::time::Timestamp& current) {
        const auto stamp = native::decodeStamp(odom.stamp, current.clock);
        require(odom.sequence != 0 && odom.sequence > previous_odom_sequence_,
                "Odom2D sequence did not increase");
        require(native::fresh(stamp, current.ns, 250'000'000), "Odom2D stamp was stale/future-dated");
        require(std::isfinite(odom.linear_x) && std::isfinite(odom.angular_z),
                "Odom2D contained nonfinite values");
        previous_odom_sequence_ = odom.sequence;
        odom_observations_[odom.sequence] = {stamp, odom.linear_x, odom.angular_z};
    }

    void verifyOdometryPairs() {
        for (auto iterator = odom_observations_.begin(); iterator != odom_observations_.end();) {
            const auto wheel = wheel_observations_.find(iterator->first);
            if (wheel == wheel_observations_.end()) {
                ++iterator;
                continue;
            }
            require(wheel->second.stamp == iterator->second.stamp,
                    "Odom2D did not preserve its WheelOdometry source timestamp");
            require(std::abs(wheel->second.linear_x - iterator->second.linear_x) < 1e-12
                    && std::abs(wheel->second.angular_z - iterator->second.angular_z) < 1e-12,
                    "Odom2D velocity did not match its WheelOdometry sequence");
            ++odom_pairs_;
            wheel_observations_.erase(wheel);
            iterator = odom_observations_.erase(iterator);
        }
        while (wheel_observations_.size() > 256U) wheel_observations_.erase(wheel_observations_.begin());
        while (odom_observations_.size() > 256U) odom_observations_.erase(odom_observations_.begin());
    }

    void validateImu(const hw::Imu& imu, const rbt::time::Timestamp& current) {
        const auto stamp = native::decodeStamp(imu.stamp, current.clock);
        require(imu.sequence != 0 && imu.sequence > previous_imu_sequence_,
                "IMU sequence did not increase");
        require(native::fresh(stamp, current.ns, 250'000'000), "IMU stamp was stale/future-dated");
        validateFrame(imu.frame, std::size(imu.frame), "imu_link", "IMU");
        const std::array<double, 10> values{
            imu.orientation_x, imu.orientation_y, imu.orientation_z, imu.orientation_w,
            imu.angular_velocity_x, imu.angular_velocity_y, imu.angular_velocity_z,
            imu.linear_acceleration_x, imu.linear_acceleration_y, imu.linear_acceleration_z};
        require(std::all_of(values.begin(), values.end(), [](double value) { return std::isfinite(value); }),
                "IMU contained nonfinite SI values");
        const double qnorm = std::sqrt(imu.orientation_x * imu.orientation_x
            + imu.orientation_y * imu.orientation_y + imu.orientation_z * imu.orientation_z
            + imu.orientation_w * imu.orientation_w);
        require(std::abs(qnorm - 1.0) < 1e-6, "IMU orientation quaternion was not normalized");
        require(nearZero(imu.orientation_x, 1e-6) && nearZero(imu.orientation_y, 1e-6)
                && nearZero(imu.orientation_z, 1e-6) && std::abs(imu.orientation_w - 1.0) < 1e-6,
                "IMU zero-Euler sample did not produce an identity quaternion");
        const double expected_gyro_z = options_.scenario == "pipeline" ? 0.2 : 0.0;
        require(std::abs(imu.angular_velocity_z - expected_gyro_z) < 1e-4,
                "IMU angular velocity was not published in rad/s");
        require(std::abs(imu.linear_acceleration_z - 9.80665) < 1e-3,
                "IMU acceleration was not published in m/s^2");
        for (const auto* covariance : {imu.orientation_covariance, imu.angular_velocity_covariance,
                                       imu.linear_acceleration_covariance}) {
            require(std::all_of(covariance, covariance + 9,
                                [](double value) { return std::isfinite(value); }),
                    "IMU covariance contained a nonfinite value");
            require(std::all_of(covariance, covariance + 9,
                                [](double value) { return value == 0.0; }),
                    "IMU covariance did not remain zero without reported uncertainty");
        }
        previous_imu_sequence_ = imu.sequence;
        ++imu_frames_;
        imu_ = imu;
    }

    void pump(const std::function<void(const hw::WheelOdometry&,
                                       const rbt::time::Timestamp&)>& wheel_callback = {}) {
        base_input_.poll([&](const hw::BaseStatus& value) { validateBase(value, now()); });
        wheel_input_.poll([&](const hw::WheelOdometry& value) {
            validateWheel(value, now(), wheel_callback);
        });
        odom_input_.poll([&](const wire::Odom2D& value) {
            validateNavigationOdometry(value, now());
        });
        verifyOdometryPairs();
        imu_input_.poll([&](const hw::Imu& value) { validateImu(value, now()); });
        guard_input_.poll([&](const wire::GuardState& value) {
            const auto current = now();
            const auto stamp = native::decodeStamp(value.stamp, current.clock);
            require(value.sequence != 0 && value.sequence > previous_guard_sequence_,
                    "GuardState sequence did not increase");
            require(native::fresh(stamp, current.ns, 250'000'000),
                    "GuardState stamp was stale/future-dated");
            previous_guard_sequence_ = value.sequence;
            ++guard_frames_;
            if (runtime_kill_started_) {
                if (value.watchdog_expired != 0U)
                    ++watchdog_guard_frames_after_runtime_kill_;
                else
                    watchdog_guard_frames_after_runtime_kill_ = 0;
            }
            guard_ = value;
        });
        nav_status_input_.poll([&](const wire::NavigationStatus& value) {
            const auto current = now();
            const auto stamp = native::decodeStamp(value.stamp, current.clock);
            require(value.stamp.reserved == 0, "invalid NavigationStatus stamp");
            require(native::fresh(stamp, current.ns, 500'000'000),
                    "NavigationStatus stamp was stale/future-dated");
            ++nav_status_frames_;
            nav_status_ = value;
        });
        const auto wall_now = SteadyClock::now();
        if (raw_enabled_ && wall_now >= next_command_) {
            sendRaw();
            next_command_ = wall_now + std::chrono::milliseconds(20);
            if (guard_kill_started_) ++raw_sends_after_guard_kill_;
        }
        if (direct_enabled_ && wall_now >= next_direct_) {
            sendDirect(direct_linear_, direct_angular_);
            next_direct_ = wall_now + std::chrono::milliseconds(20);
        }
    }

    template<class Predicate>
    void waitFor(std::chrono::milliseconds timeout, const std::string& message,
                 Predicate predicate,
                 const std::function<void(const hw::WheelOdometry&,
                                          const rbt::time::Timestamp&)>& callback = {}) {
        const auto deadline = SteadyClock::now() + timeout;
        while (SteadyClock::now() < deadline) {
            pump(callback);
            if (predicate()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        pump(callback);
        if (!predicate()) fail(message);
    }

    void waitHealthy(std::chrono::milliseconds timeout) {
        waitFor(timeout, "hardware processes did not publish fresh status, wheel feedback, and IMU",
            [&] {
                return base_frames_ >= 5 && wheel_frames_ >= 5 && imu_frames_ >= 2
                    && base_ && wheel_ && imu_ && base_->feedback_fresh != 0U;
            });
    }

    void runInvalidImuWindow() {
        phase("imu_invalid_only_prepare");
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        pump();
        const auto start_sequence = previous_imu_sequence_;
        const auto start_frames = imu_frames_;
        phase("imu_invalid_only");
        const auto deadline = SteadyClock::now() + std::chrono::milliseconds(300);
        while (SteadyClock::now() < deadline) {
            pump();
            if (previous_imu_sequence_ != start_sequence || imu_frames_ != start_frames)
                fail("driver published a stale IMU sample while only noisy/CRC-bad frames arrived");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        phase("imu_valid_resume");
        waitFor(std::chrono::milliseconds(800), "IMU did not resume after valid frames returned",
                [&] { return previous_imu_sequence_ > start_sequence; });
    }

    void requireZeroFrames(unsigned count, const std::string& context) const {
        require(stopped_status_run_ >= count, context + ": too few consecutive zero BaseStatus frames");
        require(stopped_wheel_run_ >= count, context + ": too few consecutive zero WheelOdometry frames");
        require(base_ && base_->watchdog_expired == 0U,
                context + ": a live guard zero was incorrectly reported as base watchdog expiry");
    }

    void runPipeline() {
        waitHealthy(std::chrono::seconds(6));
        raw_enabled_ = true;
        waitFor(std::chrono::seconds(3), "pipeline never commanded nonzero wheel motion", [&] {
            return base_ && wheel_ && !statusStopped(*base_)
                && (!nearZero(wheel_->left_rpm, 0.5) || !nearZero(wheel_->right_rpm, 0.5));
        });
        phase("driving");
        runInvalidImuWindow();

        phase("raw_stop");
        raw_enabled_ = false;
        stopped_status_run_ = 0;
        stopped_wheel_run_ = 0;
        waitFor(std::chrono::seconds(2), "live guard did not stop after raw command stream ended", [&] {
            return guard_ && guard_->watchdog_expired != 0U && base_ && base_->watchdog_expired == 0U
                && statusStopped(*base_) && wheel_ && nearZero(wheel_->left_rpm, 0.5)
                && nearZero(wheel_->right_rpm, 0.5) && stopped_status_run_ >= 5
                && stopped_wheel_run_ >= 5;
        });
        requireZeroFrames(5, "raw stream watchdog");

        phase("raw_resume");
        raw_enabled_ = true;
        waitFor(std::chrono::seconds(2), "live guard did not resume after fresh raw commands", [&] {
            return base_ && !statusStopped(*base_) && wheel_
                && (!nearZero(wheel_->left_rpm, 0.5) || !nearZero(wheel_->right_rpm, 0.5));
        });

        phase("guard_crash");
        const auto guard_kill_time = SteadyClock::now();
        guard_kill_started_ = true;
        if (::kill(static_cast<pid_t>(options_.guard_pid), SIGKILL) != 0)
            fail("could not kill the supplied velocity-guard child PID");
        stopped_status_after_guard_kill_ = 0;
        stopped_wheel_after_guard_kill_ = 0;
        const auto crash_deadline = guard_kill_time + std::chrono::milliseconds(500);
        bool stopped_before_deadline = false;
        while (SteadyClock::now() < crash_deadline) {
            pump();
            if (base_ && base_->watchdog_expired != 0U && statusStopped(*base_)
                && wheel_ && nearZero(wheel_->left_rpm, 0.5) && nearZero(wheel_->right_rpm, 0.5)
                && stopped_status_after_guard_kill_ >= 5 && stopped_wheel_after_guard_kill_ >= 5
                && SteadyClock::now() - guard_kill_time >= std::chrono::milliseconds(200)
                && SteadyClock::now() <= crash_deadline) {
                stopped_before_deadline = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        require(stopped_before_deadline,
                "base watchdog did not stop the wheels within 500 ms after guard death");
        require(base_ && base_->watchdog_expired != 0U && statusStopped(*base_),
                "base did not report its watchdog after guard death");
        require(stopped_status_after_guard_kill_ >= 5 && stopped_wheel_after_guard_kill_ >= 5,
                "guard crash did not produce five fresh zero hardware telemetry frames");
        require(raw_sends_after_guard_kill_ >= 5,
                "raw navigation stream stopped when the guard process died");

        phase("invalid_probe");
        const auto rejected_before = base_->rejected_commands;
        auto current = now();
        const auto high_sequence = static_cast<std::uint64_t>(current.ns) + 30ULL * second;
        wire::VelocityCommand wrong_source{};
        wrong_source.stamp = native::encodeStamp<wire::Stamp>(current);
        wrong_source.source_id = 99;
        wrong_source.sequence = high_sequence;
        wrong_source.valid_until_ns = current.ns + 100'000'000;
        wrong_source.linear_x = 0.4;
        publishCommand(*final_probe_, wrong_source);
        waitFor(std::chrono::milliseconds(300), "base did not reject wrong-source final command", [&] {
            return base_ && base_->rejected_commands >= rejected_before + 1;
        });
        current = now();
        wire::VelocityCommand expired{};
        expired.stamp = native::encodeStamp<wire::Stamp>(current);
        expired.source_id = 100;
        expired.sequence = high_sequence + 1;
        expired.valid_until_ns = current.ns - 1;
        expired.linear_x = 0.4;
        publishCommand(*final_probe_, expired);
        waitFor(std::chrono::milliseconds(300), "base did not reject expired source-100 final command", [&] {
            return base_ && base_->rejected_commands >= rejected_before + 2;
        });
        require(base_ && statusStopped(*base_) && base_->watchdog_expired != 0U,
                "invalid final command probes renewed base motion");

        direct_sequence_ = high_sequence + 2;
        phase("rearm_source100");
        direct_enabled_ = true;
        waitFor(std::chrono::seconds(1), "valid source-100 command did not re-arm the base", [&] {
            return base_ && !statusStopped(*base_) && base_->watchdog_expired == 0U
                && wheel_ && (!nearZero(wheel_->left_rpm, 0.5) || !nearZero(wheel_->right_rpm, 0.5));
        });

        phase("estop");
        direct_enabled_ = false;
        sendEmergency(true);
        waitFor(std::chrono::milliseconds(600), "emergency stop did not latch and zero both motors", [&] {
            return base_ && base_->emergency_stop != 0U && statusStopped(*base_)
                && wheel_ && nearZero(wheel_->left_rpm, 0.5) && nearZero(wheel_->right_rpm, 0.5);
        });
        require(base_->watchdog_expired == 0U, "base watchdog obscured the emergency-stop test");

        phase("estop_release");
        stopped_status_run_ = 0;
        stopped_wheel_run_ = 0;
        sendEmergency(false);
        waitFor(std::chrono::milliseconds(250), "emergency-stop release did not clear its latch", [&] {
            return base_ && base_->emergency_stop == 0U && statusStopped(*base_)
                && wheel_ && nearZero(wheel_->left_rpm, 0.5) && nearZero(wheel_->right_rpm, 0.5)
                && stopped_status_run_ >= 5 && stopped_wheel_run_ >= 5;
        });

        phase("rearm_after_release");
        direct_enabled_ = true;
        direct_sequence_ += 1;
        waitFor(std::chrono::seconds(1), "base did not resume with a fresh command after estop release", [&] {
            return base_ && !statusStopped(*base_) && base_->emergency_stop == 0U
                && wheel_ && (!nearZero(wheel_->left_rpm, 0.5) || !nearZero(wheel_->right_rpm, 0.5));
        });

        phase("shutdown_stop");
        direct_enabled_ = false;
        sendDirect(0.0, 0.0);
        waitFor(std::chrono::milliseconds(800), "base did not stop on a fresh zero command", [&] {
            return base_ && statusStopped(*base_) && wheel_
                && nearZero(wheel_->left_rpm, 0.5) && nearZero(wheel_->right_rpm, 0.5)
                && stopped_status_run_ >= 5 && stopped_wheel_run_ >= 5;
        });
        require(base_frames_ >= 20 && wheel_frames_ >= 20 && imu_frames_ >= 20 && guard_frames_ >= 10,
                "pipeline did not observe enough fresh telemetry from each real process");
        require(odom_pairs_ >= 20,
                "base Odom2D did not preserve WheelOdometry sequence and source timestamp");
        phase("pipeline_pass");
        std::cout << "PASS hardware pipeline: base_frames=" << base_frames_
                  << " wheel_frames=" << wheel_frames_ << " imu_frames=" << imu_frames_
                  << " guard_frames=" << guard_frames_ << " rejected=" << base_->rejected_commands
                  << std::endl;
    }

    void publishMap(rbt::SlicePublisher<wire::MapHeader, std::int8_t>& publisher,
                    const std::vector<std::int8_t>& cells) {
        const auto stamp = now();
        auto sample = native::take(publisher.loan_slice(cells.size()));
        auto& header = sample.header();
        header = {};
        header.stamp = native::encodeStamp<wire::Stamp>(stamp);
        header.version = 1;
        header.width = 80;
        header.height = 80;
        header.resolution = 0.1;
        header.origin_x = -2.0;
        header.origin_y = -2.0;
        header.origin_yaw = 0.0;
        native::frame(header.frame, "map");
        std::copy(cells.begin(), cells.end(), sample.payload().begin());
        if (!sample.send()) fail("navigation fixture occupancy map send failed");
    }

    void publishMapTransform(rbt::tf::DynamicTfPublisher& publisher,
                             std::uint64_t& publication_drops,
                             SteadyClock::time_point& last_drop_log) {
        const auto stamp = now();
        const rbt::tf::Transform3 transform(rbt::tf::Vec3(0.0, 0.0, 0.0),
                                             rbt::tf::Quaternion(0.0, 0.0, 0.0, 1.0));
        try {
            publisher.publish({rbt::tf::FrameId("map"), rbt::tf::FrameId("odom"), stamp,
                               0U, 1U, transform});
        } catch (const rbt::tf::TfTransportError& error) {
            if (error.stream_error() != rbt::StreamError::QueueFull) throw;
            ++publication_drops;
            const auto wall_now = SteadyClock::now();
            if (last_drop_log == SteadyClock::time_point::min()
                || wall_now - last_drop_log >= std::chrono::seconds(1)) {
                std::cerr << "hardware IPC fixture: dropped map->odom TF publication: "
                          << error.what() << "; tf_publish_drops=" << publication_drops << '\n';
                last_drop_log = wall_now;
            }
        }
    }

    void publishOdometryTransform(rbt::tf::DynamicTfPublisher& publisher,
                                  const hw::WheelOdometry& wheel,
                                  const rbt::time::Timestamp& current,
                                  std::uint64_t& publication_drops,
                                  SteadyClock::time_point& last_drop_log) {
        const auto stamp_ns = native::decodeStamp(wheel.stamp, current.clock);
        const rbt::time::Timestamp original_stamp{current.clock, stamp_ns};
        const rbt::tf::Transform3 transform(
            rbt::tf::Vec3(wheel.x, wheel.y, 0.0),
            rbt::tf::Quaternion(0.0, 0.0, std::sin(wheel.yaw / 2.0), std::cos(wheel.yaw / 2.0)));
        try {
            publisher.publish({rbt::tf::FrameId("odom"), rbt::tf::FrameId("base_link"),
                               original_stamp, 0U, 1U, transform});
        } catch (const rbt::tf::TfTransportError& error) {
            if (error.stream_error() != rbt::StreamError::QueueFull) throw;
            ++publication_drops;
            const auto wall_now = SteadyClock::now();
            if (last_drop_log == SteadyClock::time_point::min()
                || wall_now - last_drop_log >= std::chrono::seconds(1)) {
                std::cerr << "hardware IPC fixture: dropped odom->base_link TF publication: "
                          << error.what() << "; tf_publish_drops=" << publication_drops << '\n';
                last_drop_log = wall_now;
            }
        }
    }

    void publishGoal(rbt::FixedPublisher<wire::GoalCommand>& publisher) {
        const auto stamp = now();
        wire::GoalCommand goal{};
        goal.stamp = native::encodeStamp<wire::Stamp>(stamp);
        goal.goal_id = 1;
        goal.operation = 0;
        goal.x = 1.0;
        goal.y = 0.0;
        goal.yaw = 0.0;
        goal.timeout_ns = 20 * second;
        publishCommand(publisher, goal);
    }

    void runNavigation() {
        auto map_publisher = native::take(rbt::SlicePublisher<wire::MapHeader, std::int8_t>::create(
            {options_.prefix + "/map", wire::OCCUPANCY_MAP_DESCRIPTOR, rbt::StreamProfile::Sensor},
            wire::OCCUPANCY_MAP_DESCRIPTOR, 80U * 80U));
        auto map_tf = rbt::tf::DynamicTfPublisher::open(options_.map_tf_id);
        auto odom_tf = rbt::tf::DynamicTfPublisher::open(options_.odom_tf_id);
        auto goal_publisher = native::publisher<wire::GoalCommand>(options_.prefix + "/goal");
        std::vector<std::int8_t> cells(80U * 80U, 0);
        std::uint64_t map_tf_drops = 0;
        std::uint64_t odom_tf_drops = 0;
        auto last_map_tf_drop_log = SteadyClock::time_point::min();
        auto last_odom_tf_drop_log = SteadyClock::time_point::min();
        const auto wheel_callback = [&](const hw::WheelOdometry& wheel,
                                        const rbt::time::Timestamp& current) {
            publishOdometryTransform(odom_tf, wheel, current,
                                     odom_tf_drops, last_odom_tf_drop_log);
        };
        next_map_ = SteadyClock::time_point::min();
        const auto warmup_deadline = SteadyClock::now() + std::chrono::seconds(8);
        const auto localization_ready_after = SteadyClock::now() + std::chrono::milliseconds(500);
        bool goal_sent = false;
        bool motion_seen = false;
        bool runtime_crash_started = false;
        SteadyClock::time_point runtime_kill_time{};
        const auto test_deadline = SteadyClock::now() + std::chrono::seconds(24);
        while (SteadyClock::now() < test_deadline) {
            pump(wheel_callback);
            const auto wall_now = SteadyClock::now();
            if (wall_now >= next_map_) {
                publishMap(map_publisher, cells);
                next_map_ = wall_now + std::chrono::milliseconds(100);
            }
            publishMapTransform(map_tf, map_tf_drops, last_map_tf_drop_log);
            if (!goal_sent && base_frames_ >= 5 && wheel_frames_ >= 5 && imu_frames_ >= 2
                && odom_pairs_ >= 5
                && nav_status_frames_ >= 3 && base_ && base_->feedback_fresh != 0U
                && wall_now >= localization_ready_after) {
                phase("navigation_goal");
                publishGoal(goal_publisher);
                goal_sent = true;
            }
            if (goal_sent && nav_status_ && nav_status_->state == 6U)
                fail("navigation runtime returned Failed status error="
                     + std::to_string(nav_status_->error));
            if (goal_sent && base_ && wheel_ && !statusStopped(*base_)
                && (!nearZero(wheel_->left_rpm, 0.5) || !nearZero(wheel_->right_rpm, 0.5))) {
                motion_seen = true;
            }
            if (options_.scenario == "navigation_crash" && motion_seen && !runtime_crash_started) {
                phase("navigation_runtime_crash");
                runtime_kill_started_ = true;
                runtime_crash_started = true;
                runtime_kill_time = SteadyClock::now();
                if (::kill(static_cast<pid_t>(options_.runtime_pid), SIGKILL) != 0)
                    fail("could not kill the supplied navigation-runtime child PID");
                stopped_status_after_runtime_kill_ = 0;
                stopped_wheel_after_runtime_kill_ = 0;
                watchdog_guard_frames_after_runtime_kill_ = 0;
            }
            if (runtime_crash_started) {
                const auto elapsed = SteadyClock::now() - runtime_kill_time;
                const bool stopped_with_live_guard = base_ && wheel_ && guard_
                    && base_->watchdog_expired == 0U && statusStopped(*base_)
                    && guard_->watchdog_expired != 0U
                    && nearZero(wheel_->left_rpm, 0.5) && nearZero(wheel_->right_rpm, 0.5)
                    && stopped_status_after_runtime_kill_ >= 5
                    && stopped_wheel_after_runtime_kill_ >= 5
                    && watchdog_guard_frames_after_runtime_kill_ >= 5;
                if (stopped_with_live_guard && elapsed >= std::chrono::milliseconds(200)) {
                    require(motion_seen, "navigation did not move before the runtime crash");
                    phase("navigation_crash_pass");
                    std::cout << "PASS hardware navigation crash: zero_base_frames="
                              << stopped_status_after_runtime_kill_
                              << " zero_wheel_frames=" << stopped_wheel_after_runtime_kill_
                              << " watchdog_guard_frames=" << watchdog_guard_frames_after_runtime_kill_
                              << " guard_frames=" << guard_frames_
                              << " map_tf_drops=" << map_tf_drops
                              << " odom_tf_drops=" << odom_tf_drops << std::endl;
                    return;
                }
                if (elapsed > std::chrono::milliseconds(500))
                    fail("live guard/base did not stop after runtime death within 500 ms");
            }
            if (goal_sent && nav_status_ && nav_status_->state == 4U
                && options_.scenario == "navigation"
                && stopped_status_run_ >= 5 && stopped_wheel_run_ >= 5) {
                require(std::abs(wheel_->x - 1.0) < 0.12 && std::abs(wheel_->y - 0.0) < 0.12,
                        "wheel odometry final pose did not reach the requested map goal");
                require(nav_status_->goal_id == 1U && nav_status_->map_version == 1U,
                        "navigation success did not correspond to the requested goal and map");
                require(odom_pairs_ >= 20,
                        "navigation odometry did not retain WheelOdometry source sequence and timestamp");
                require(wheel_frames_ >= 20 && imu_frames_ >= 20,
                        "navigation did not retain fresh hardware feedback through completion");
                phase("navigation_pass");
                std::cout << "PASS hardware navigation: base_frames=" << base_frames_
                          << " wheel_frames=" << wheel_frames_ << " imu_frames=" << imu_frames_
                          << " nav_state=" << nav_status_->state << " goal_id=" << nav_status_->goal_id
                          << " pose=(" << wheel_->x << ',' << wheel_->y << ")" << std::endl;
                return;
            }
            if (!goal_sent && wall_now >= warmup_deadline)
                fail("navigation runtime did not become ready while actual hardware telemetry was flowing");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        fail("real hardware navigation pipeline exceeded its 24-second deadline");
    }

    bool guard_kill_started_{};
    bool runtime_kill_started_{};
};

} // namespace

int main(int argc, char** argv) {
    try {
        Fixture fixture(Options(argc, argv));
        fixture.run();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL hardware IPC fixture: " << error.what() << '\n';
        return 1;
    }
}

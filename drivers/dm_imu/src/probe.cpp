#include <rbt_hardware/native.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

namespace {

using Imu = rbt::hardware::Imu;
using SteadyClock = std::chrono::steady_clock;

static_assert(std::atomic<bool>::is_always_lock_free);
std::atomic<bool> interrupted { false };

void signal_handler(int) noexcept {
    interrupted.store(true, std::memory_order_relaxed);
}

void install_signal_handlers() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
}

constexpr std::int64_t kMaximumAgeNs = 500'000'000;
constexpr double kQuaternionNormTolerance = 0.05;

struct Options {
    std::string topic = "/imu/data";
    double duration_seconds = 5.0;
    std::string mode = "poll";
};

struct LatestSample {
    bool received = false;
    double gyro[3] {};
    double acceleration[3] {};
    double quaternion_norm = 0.0;
    bool quaternion_norm_available = false;
    std::array<std::uint8_t, sizeof(Imu::frame)> frame {};
    std::optional<std::size_t> frame_length;
};

struct Summary {
    std::uint64_t received = 0;
    std::uint64_t valid = 0;
    std::uint64_t invalid = 0;
    double rate_hz = 0.0;
    std::optional<double> latest_age_ms;
    LatestSample latest;
};

[[nodiscard]] std::string usage() {
    return "Usage: rbt_dm_imu_probe [--topic TOPIC] [--duration SEC] [--mode poll|event]\n"
           "  --topic TOPIC       Native rbt.hardware::Imu topic (default /imu/data)\n"
           "  --duration SEC      Observation duration in positive seconds (default 5)\n"
           "  --mode MODE         Receive using poll or event wait (default poll)\n"
           "  --help, -h          Show this help\n";
}

[[nodiscard]] std::string_view require_value(int& index, int argc, char** argv,
                                             std::string_view option) {
    if (index + 1 >= argc) {
        throw std::invalid_argument(std::string(option) + " requires a value");
    }
    ++index;
    const std::string_view value(argv[index]);
    if (value.empty() || value.starts_with("--")) {
        throw std::invalid_argument(std::string(option) + " requires a value");
    }
    return value;
}

[[nodiscard]] double parse_duration(std::string_view text) {
    double value = 0.0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value,
                                        std::chars_format::general);
    if (text.empty() || parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size()
        || !std::isfinite(value) || value <= 0.0) {
        throw std::invalid_argument("--duration must be a finite positive number of seconds");
    }
    return value;
}

[[nodiscard]] Options parse_arguments(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            std::cout << usage();
            std::exit(0);
        }
        if (argument == "--topic") {
            options.topic = require_value(index, argc, argv, argument);
        } else if (argument == "--duration") {
            options.duration_seconds = parse_duration(require_value(index, argc, argv, argument));
        } else if (argument == "--mode") {
            options.mode = require_value(index, argc, argv, argument);
            if (options.mode != "poll" && options.mode != "event") {
                throw std::invalid_argument("--mode must be poll or event");
            }
        } else {
            throw std::invalid_argument("unknown option: " + std::string(argument));
        }
    }
    return options;
}

[[nodiscard]] bool finite_vector_and_quaternion(const Imu& message) {
    const std::array values {
        message.orientation_x, message.orientation_y, message.orientation_z, message.orientation_w,
        message.angular_velocity_x, message.angular_velocity_y, message.angular_velocity_z,
        message.linear_acceleration_x, message.linear_acceleration_y, message.linear_acceleration_z,
    };
    return std::all_of(values.begin(), values.end(), [](double value) { return std::isfinite(value); });
}

[[nodiscard]] bool valid_frame(const Imu& message, std::size_t& length) {
    const auto* end = std::find(std::begin(message.frame), std::end(message.frame), std::uint8_t {});
    if (end == std::end(message.frame)) {
        return false;
    }
    length = static_cast<std::size_t>(end - std::begin(message.frame));
    return length > 0U;
}

[[nodiscard]] double quaternion_norm(const Imu& message) {
    return std::hypot(std::hypot(message.orientation_x, message.orientation_y),
                      std::hypot(message.orientation_z, message.orientation_w));
}

[[nodiscard]] bool duration_elapsed(SteadyClock::time_point started, double duration_seconds) {
    return std::chrono::duration<double>(SteadyClock::now() - started).count() >= duration_seconds;
}

void observe(const Imu& message, const rbt::time::HostClock& clock,
             std::optional<std::uint64_t>& previous_sequence, Summary& summary) {
    ++summary.received;
    summary.latest.received = true;
    summary.latest.gyro[0] = message.angular_velocity_x;
    summary.latest.gyro[1] = message.angular_velocity_y;
    summary.latest.gyro[2] = message.angular_velocity_z;
    summary.latest.acceleration[0] = message.linear_acceleration_x;
    summary.latest.acceleration[1] = message.linear_acceleration_y;
    summary.latest.acceleration[2] = message.linear_acceleration_z;
    summary.latest.quaternion_norm_available = finite_vector_and_quaternion(message);
    if (summary.latest.quaternion_norm_available) {
        summary.latest.quaternion_norm = quaternion_norm(message);
    }
    std::copy(std::begin(message.frame), std::end(message.frame), summary.latest.frame.begin());
    std::size_t frame_length = 0U;
    summary.latest.frame_length = valid_frame(message, frame_length)
                                      ? std::optional(frame_length)
                                      : std::nullopt;

    const auto sequence_increases = message.sequence != 0U
                                   && (!previous_sequence.has_value()
                                       || message.sequence > *previous_sequence);
    previous_sequence = message.sequence;

    bool timestamp_fresh = false;
    try {
        const auto stamp_ns = rbt::hardware::native::decodeStamp(message.stamp,
                                                                 clock.monotonic_identity());
        const auto current_ns = rbt::hardware::native::now(clock).ns;
        summary.latest_age_ms = static_cast<double>(current_ns - stamp_ns) / 1'000'000.0;
        timestamp_fresh = rbt::hardware::native::fresh(stamp_ns, current_ns, kMaximumAgeNs);
    } catch (const std::invalid_argument&) {
        summary.latest_age_ms.reset();
    } catch (...) {
        ++summary.invalid;
        throw;
    }

    const auto norm = summary.latest.quaternion_norm_available
                          ? summary.latest.quaternion_norm
                          : std::numeric_limits<double>::quiet_NaN();
    const auto norm_valid = std::isfinite(norm)
                            && std::abs(norm - 1.0) <= kQuaternionNormTolerance;
    const auto all_fields_finite = finite_vector_and_quaternion(message);
    const auto frame_is_valid = summary.latest.frame_length.has_value();
    if (all_fields_finite && norm_valid && frame_is_valid && timestamp_fresh && sequence_increases) {
        ++summary.valid;
    } else {
        ++summary.invalid;
    }
}

void print_number(double value) {
    if (std::isfinite(value)) {
        std::cout << std::setprecision(17) << value;
    } else {
        std::cout << "null";
    }
}

void print_vector(const double (&values)[3]) {
    std::cout << '[';
    for (std::size_t index = 0U; index < 3U; ++index) {
        if (index != 0U) {
            std::cout << ',';
        }
        print_number(values[index]);
    }
    std::cout << ']';
}

void print_frame(const LatestSample& latest) {
    if (!latest.frame_length.has_value()) {
        std::cout << "null";
        return;
    }
    constexpr char hex[] = "0123456789abcdef";
    std::cout << '"';
    for (std::size_t index = 0U; index < *latest.frame_length; ++index) {
        const auto value = latest.frame[index];
        if (value == '"' || value == '\\') {
            std::cout << '\\' << static_cast<char>(value);
        } else if (value >= 0x20U && value < 0x7fU) {
            std::cout << static_cast<char>(value);
        } else {
            std::cout << "\\u00" << hex[(value >> 4U) & 0x0fU] << hex[value & 0x0fU];
        }
    }
    std::cout << '"';
}

void print_summary(const Options& options, const Summary& summary) {
    std::cout << "{\"topic\":\"";
    for (const char value : options.topic) {
        if (value == '"' || value == '\\') {
            std::cout << '\\' << value;
        } else if (static_cast<unsigned char>(value) >= 0x20U
                   && static_cast<unsigned char>(value) < 0x7fU) {
            std::cout << value;
        } else {
            constexpr char hex[] = "0123456789abcdef";
            const auto byte = static_cast<unsigned char>(value);
            std::cout << "\\u00" << hex[(byte >> 4U) & 0x0fU] << hex[byte & 0x0fU];
        }
    }
    std::cout << "\",\"duration_s\":";
    print_number(options.duration_seconds);
    std::cout << ",\"received\":" << summary.received
              << ",\"valid\":" << summary.valid
              << ",\"invalid\":" << summary.invalid
              << ",\"rate_hz\":";
    print_number(summary.rate_hz);
    std::cout << ",\"latest_age_ms\":";
    if (summary.latest_age_ms.has_value()) {
        print_number(*summary.latest_age_ms);
    } else {
        std::cout << "null";
    }
    std::cout << ",\"last_gyro_rad_s\":";
    if (summary.latest.received) {
        print_vector(summary.latest.gyro);
    } else {
        std::cout << "null";
    }
    std::cout << ",\"last_accel_m_s2\":";
    if (summary.latest.received) {
        print_vector(summary.latest.acceleration);
    } else {
        std::cout << "null";
    }
    std::cout << ",\"quaternion_norm\":";
    if (summary.latest.quaternion_norm_available) {
        print_number(summary.latest.quaternion_norm);
    } else {
        std::cout << "null";
    }
    std::cout << ",\"frame\":";
    print_frame(summary.latest);
    std::cout << "}\n";
}

int run(const Options& options) {
    install_signal_handlers();
    Summary summary;
    std::optional<rbt::time::HostClock> clock;
    std::optional<std::uint64_t> previous_sequence;
    std::optional<SteadyClock::time_point> first_received;
    std::optional<SteadyClock::time_point> last_received;
    std::int64_t latest_stamp_ns = 0;
    bool latest_stamp_valid = false;
    std::string runtime_error;
    const auto started = SteadyClock::now();
    try {
        clock.emplace(rbt::hardware::native::hostClock());
        const auto process_sample = [&](const Imu& message, SteadyClock::time_point receive_time) {
            if (!first_received.has_value()) {
                first_received = receive_time;
            }
            last_received = receive_time;
            try {
                latest_stamp_ns = rbt::hardware::native::decodeStamp(
                    message.stamp, clock->monotonic_identity());
                latest_stamp_valid = true;
            } catch (const std::invalid_argument&) {
                latest_stamp_valid = false;
            }
            observe(message, *clock, previous_sequence, summary);
        };

        if (options.mode == "poll") {
            rbt::hardware::native::Input<Imu> input(options.topic);
            auto next_poll = started;
            constexpr auto poll_period = std::chrono::milliseconds(1);
            while (!interrupted.load(std::memory_order_relaxed)
                   && !duration_elapsed(started, options.duration_seconds)) {
                const auto poll_time = SteadyClock::now();
                input.poll([&](const Imu& message) { process_sample(message, poll_time); });
                next_poll += poll_period;
                const auto poll_finished = SteadyClock::now();
                if (next_poll < poll_finished) {
                    next_poll = poll_finished;
                }
                std::this_thread::sleep_until(next_poll);
            }
        } else {
            const auto& descriptor = rbt::FixedMessageBinding<Imu>::descriptor();
            std::optional<rbt::FixedSubscriber<Imu>> subscriber;
            while (!interrupted.load(std::memory_order_relaxed)
                   && !duration_elapsed(started, options.duration_seconds)) {
                auto result = rbt::FixedSubscriber<Imu>::create(
                    { options.topic, descriptor, rbt::StreamProfile::Sensor }, descriptor);
                if (result) {
                    subscriber.emplace(std::move(result).value());
                    break;
                }
                if (result.error() != rbt::StreamError::ReadinessOpen
                    && result.error() != rbt::StreamError::Transport) {
                    throw std::runtime_error(std::string("RBT input: ") + rbt::to_string(result.error()));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }

            if (subscriber.has_value()
                && !interrupted.load(std::memory_order_relaxed)
                && !duration_elapsed(started, options.duration_seconds)) {
                auto context_result = rbt::WaitContext::create(rbt::generate_uuid(), 1U);
                if (!context_result) {
                    throw std::runtime_error(std::string("wait context: ")
                                             + rbt::to_string(context_result.error()));
                }
                auto wait_context = std::move(context_result).value();
                std::stop_source stop_source;
                std::jthread monitor([&](std::stop_token monitor_stop) {
                    while (!monitor_stop.stop_requested()) {
                        if (interrupted.load(std::memory_order_relaxed)
                            || duration_elapsed(started, options.duration_seconds)) {
                            stop_source.request_stop();
                            return;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    }
                });

                std::optional<std::string> receive_error;
                const auto token = rbt::WaitToken::new_token(1U);
                const auto wait_result = wait_context.run(
                    stop_source.get_token(),
                    { subscriber->event_source(token) },
                    [&](rbt::ReadyEvent event, std::size_t budget) {
                        if (event.is_cancellation() || stop_source.stop_requested()) {
                            return rbt::WaitProgress::continue_waiting();
                        }

                        std::size_t drained = 0U;
                        for (std::size_t index = 0U; index < budget; ++index) {
                            if (stop_source.stop_requested()) {
                                break;
                            }
                            auto received = subscriber->receive();
                            if (!received) {
                                receive_error = std::string("RBT input: ")
                                                + rbt::to_string(received.error());
                                return rbt::WaitProgress::stop();
                            }
                            auto sample = std::move(received).value();
                            if (!sample.has_value()) {
                                break;
                            }
                            process_sample(sample->payload(), SteadyClock::now());
                            ++drained;
                        }
                        return drained == 0U ? rbt::WaitProgress::empty()
                                             : rbt::WaitProgress::samples(drained);
                    });
                monitor.request_stop();
                if (!wait_result) {
                    throw std::runtime_error(std::string("wait failed: ")
                                             + rbt::to_string(wait_result.error()));
                }
                if (receive_error.has_value()) {
                    throw std::runtime_error(std::move(*receive_error));
                }
            }
        }
    } catch (const std::exception& error) {
        runtime_error = error.what();
    }

    if (first_received.has_value() && last_received.has_value() && *last_received > *first_received) {
        const auto interval = std::chrono::duration<double>(*last_received - *first_received).count();
        summary.rate_hz = static_cast<double>(summary.received - 1U) / interval;
    }
    if (clock.has_value() && latest_stamp_valid) {
        try {
            const auto current_ns = rbt::hardware::native::now(*clock).ns;
            summary.latest_age_ms = static_cast<double>(current_ns - latest_stamp_ns) / 1'000'000.0;
        } catch (const std::exception& error) {
            if (runtime_error.empty()) {
                runtime_error = error.what();
            }
        }
    }

    print_summary(options, summary);
    if (!runtime_error.empty()) {
        std::cerr << "rbt_dm_imu_probe: " << runtime_error << '\n';
    }
    const auto latest_is_fresh = summary.latest_age_ms.has_value()
                                 && *summary.latest_age_ms >= 0.0
                                 && *summary.latest_age_ms <= 500.0;
    return summary.valid >= 5U && summary.invalid == 0U && latest_is_fresh
                   && runtime_error.empty()
               ? 0
               : 1;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    try {
        options = parse_arguments(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "rbt_dm_imu_probe: " << error.what() << '\n' << usage();
        return 2;
    }
    return run(options);
}

#include <rbt_dm_imu/config.hpp>
#include <rbt_dm_imu/parser.hpp>
#include <rbt_dm_imu/serial_port.hpp>

#include <rbt/wait.hpp>
#include <rbt_hardware/native.hpp>
#include <rbt/effect_scope.hpp>
#include <rbt/runtime.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

volatile std::sig_atomic_t interrupted = 0;

extern "C" void handle_signal(int) {
    interrupted = 1;
}

void install_signals() {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
}

[[nodiscard]] bool should_stop() {
    return interrupted != 0;
}

[[nodiscard]] std::string usage() {
    return "Usage: rbt_dm_imu [--config FILE] [--serial-port PATH] [--topic TOPIC]\n"
           "                    [--duration SEC] [--gyro-unit rad_s|deg_s]\n"
           "                    [--accel-unit m_s2|g] [--configure-device] [--save-parameters]\n";
}

[[nodiscard]] double parse_duration(std::string_view text) {
    if (text.empty()) {
        throw std::invalid_argument("--duration requires a positive number of seconds");
    }
    double value = 0.0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value,
                                        std::chars_format::general);
    if (parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size() || !std::isfinite(value)
        || value <= 0.0 || value > 86400.0) {
        throw std::invalid_argument("--duration must be greater than zero and no more than 86400 seconds");
    }
    return value;
}

struct Overrides {
    std::optional<std::string> config_path;
    std::optional<std::string> serial_port;
    std::optional<std::string> topic;
    std::optional<std::string> gyro_unit;
    std::optional<std::string> acceleration_unit;
    std::optional<double> duration_seconds;
    bool configure_device = false;
    bool save_parameters = false;
};

[[nodiscard]] Overrides parse_arguments(int argc, char** argv) {
    Overrides overrides;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            std::cout << usage();
            std::exit(0);
        }
        const auto require_value = [&]() -> std::string_view {
            if (index + 1 >= argc) {
                throw std::invalid_argument(std::string(argument) + " requires a value");
            }
            ++index;
            const std::string_view value(argv[index]);
            if (value.empty() || value.starts_with("--")) {
                throw std::invalid_argument(std::string(argument) + " requires a value");
            }
            return value;
        };
        if (argument == "--config") {
            overrides.config_path = require_value();
        } else if (argument == "--serial-port") {
            overrides.serial_port = require_value();
        } else if (argument == "--topic") {
            overrides.topic = require_value();
        } else if (argument == "--duration") {
            overrides.duration_seconds = parse_duration(require_value());
        } else if (argument == "--gyro-unit") {
            overrides.gyro_unit = require_value();
        } else if (argument == "--accel-unit") {
            overrides.acceleration_unit = require_value();
        } else if (argument == "--configure-device") {
            overrides.configure_device = true;
        } else if (argument == "--save-parameters") {
            overrides.save_parameters = true;
        } else {
            throw std::invalid_argument("unknown option: " + std::string(argument));
        }
    }
    return overrides;
}

[[nodiscard]] rbt_dm_imu::Config get_config(const Overrides& overrides) {
    auto config = rbt_dm_imu::load_config(overrides.config_path);
    if (overrides.serial_port.has_value()) {
        config.serial_port = *overrides.serial_port;
    }
    if (overrides.topic.has_value()) {
        config.topic = *overrides.topic;
    }
    if (overrides.gyro_unit.has_value()) {
        config.gyro_unit = rbt_dm_imu::parse_gyro_unit(*overrides.gyro_unit);
    }
    if (overrides.acceleration_unit.has_value()) {
        config.acceleration_unit = rbt_dm_imu::parse_acceleration_unit(*overrides.acceleration_unit);
    }
    if (overrides.duration_seconds.has_value()) {
        config.duration = std::chrono::duration<double>(*overrides.duration_seconds);
    }
    config.configure_device = config.configure_device || overrides.configure_device || overrides.save_parameters;
    config.save_parameters = config.save_parameters || overrides.save_parameters;
    rbt_dm_imu::validate_config(config);
    return config;
}

void wait_interruptibly(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (!should_stop() && std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        std::this_thread::sleep_for(std::min(remaining, std::chrono::milliseconds(20)));
    }
}

[[nodiscard]] bool send_configuration(rbt_dm_imu::SerialPort& port, const rbt_dm_imu::Config& config) {
    using Command = std::array<std::uint8_t, 5>;
    constexpr Command enter {0xaaU, 0x06U, 0x01U, 0x0dU, 0U};
    constexpr Command accel {0xaaU, 0x01U, 0x14U, 0x0dU, 0U};
    constexpr Command gyro {0xaaU, 0x01U, 0x15U, 0x0dU, 0U};
    constexpr Command euler {0xaaU, 0x01U, 0x16U, 0x0dU, 0U};
    constexpr Command quaternion_off {0xaaU, 0x01U, 0x07U, 0x0dU, 0U};
    constexpr Command output_1000_hz {0xaaU, 0x02U, 0x01U, 0x00U, 0x0dU};
    constexpr Command save {0xaaU, 0x03U, 0x01U, 0x0dU, 0U};
    constexpr Command exit {0xaaU, 0x06U, 0x00U, 0x0dU, 0U};
    const auto send = [&](const Command& command, std::size_t size) {
        for (unsigned repeat = 0U; repeat < 5U && !should_stop(); ++repeat) {
            if (!port.write_all(std::span<const std::uint8_t>(command.data(), size), should_stop)) {
                return false;
            }
            if (repeat < 4U) {
                wait_interruptibly(std::chrono::milliseconds(10));
            }
        }
        return !should_stop();
    };

    if (!send(enter, 4U) || !send(accel, 4U) || !send(gyro, 4U) || !send(euler, 4U)
        || !send(quaternion_off, 4U) || !send(output_1000_hz, 5U)
        || (config.save_parameters && !send(save, 4U)) || !send(exit, 4U)) {
        return false;
    }
    wait_interruptibly(std::chrono::milliseconds(100));
    return !should_stop();
}

void publish_sample(rbt::FixedPublisher<rbt::hardware::Imu>& publisher,
                    const rbt::time::HostClock& clock,
                    const rbt_dm_imu::Config& config,
                    const rbt_dm_imu::RawImuSample& raw,
                    std::uint64_t sequence) {
    const auto sample = rbt_dm_imu::to_si(raw, config.gyro_unit, config.acceleration_unit);
    rbt::hardware::Imu message {};
    message.stamp = rbt::hardware::native::encodeStamp(rbt::hardware::native::now(clock));
    message.sequence = sequence;
    rbt::hardware::native::frame(message.frame, config.frame);
    message.orientation_x = sample.orientation_xyzw[0];
    message.orientation_y = sample.orientation_xyzw[1];
    message.orientation_z = sample.orientation_xyzw[2];
    message.orientation_w = sample.orientation_xyzw[3];
    message.angular_velocity_x = sample.angular_velocity_rad_s[0];
    message.angular_velocity_y = sample.angular_velocity_rad_s[1];
    message.angular_velocity_z = sample.angular_velocity_rad_s[2];
    message.linear_acceleration_x = sample.linear_acceleration_m_s2[0];
    message.linear_acceleration_y = sample.linear_acceleration_m_s2[1];
    message.linear_acceleration_z = sample.linear_acceleration_m_s2[2];
    // Covariances remain zero because the protocol provides no uncertainty values.
    rbt::hardware::native::publish(publisher, message);
}

int run(const rbt_dm_imu::Config& config) {
    install_signals();
    rbt_dm_imu::SerialPort port;
    rbt::EffectScope scope(rbt::generate_uuid(), 1U);
    auto host_clock = rbt::hardware::native::hostClock();
    auto publisher = rbt::hardware::native::publisher<rbt::hardware::Imu>(config.topic);
    scope.defer_sync([&port] { port.close(); });
    rbt_dm_imu::Parser parser(config.protocol);

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = config.duration.has_value()
                              ? std::optional(started + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                                          *config.duration))
                              : std::nullopt;
    rbt_dm_imu::PublishRateGate publish_rate(config.publish_rate_hz, started);
    auto reconnect_delay = config.reconnect_initial;
    const auto sequence_seed_ns = rbt::hardware::native::now(host_clock).ns;
    std::uint64_t sequence = sequence_seed_ns > 0
                                 ? static_cast<std::uint64_t>(sequence_seed_ns)
                                 : 0U;
    std::array<std::uint8_t, 512U> bytes {};

    while (!should_stop() && (!deadline.has_value() || std::chrono::steady_clock::now() < *deadline)) {
        if (!port.is_open()) {
            if (!port.open(config.serial_port, config.baud)) {
                std::cerr << "Unable to open " << config.serial_port << "; retrying in "
                          << reconnect_delay.count() << " ms\n";
                wait_interruptibly(reconnect_delay);
                reconnect_delay = std::min(reconnect_delay * 2, config.reconnect_maximum);
                continue;
            }
            parser.reset();
            if (config.configure_device && !send_configuration(port, config)) {
                std::cerr << "IMU startup configuration failed; reconnecting\n";
                port.close();
                parser.reset();
                wait_interruptibly(reconnect_delay);
                reconnect_delay = std::min(reconnect_delay * 2, config.reconnect_maximum);
                continue;
            }
            reconnect_delay = config.reconnect_initial;
            std::cerr << "Connected to " << config.serial_port << " at " << config.baud << " baud\n";
        }

        const auto ready = port.wait_readable(20);
        if (ready == rbt_dm_imu::PollResult::Interrupted) {
            continue;
        }
        if (ready == rbt_dm_imu::PollResult::Disconnected) {
            std::cerr << "IMU serial connection closed; reconnecting\n";
            port.close();
            parser.reset();
            wait_interruptibly(reconnect_delay);
            reconnect_delay = std::min(reconnect_delay * 2, config.reconnect_maximum);
            continue;
        }
        if (ready == rbt_dm_imu::PollResult::Timeout) {
            continue;
        }

        const auto count = port.read(bytes);
        if (count < 0) {
            std::cerr << "IMU serial read failed; reconnecting\n";
            port.close();
            parser.reset();
            wait_interruptibly(reconnect_delay);
            reconnect_delay = std::min(reconnect_delay * 2, config.reconnect_maximum);
            continue;
        }
        if (count == 0) {
            continue;
        }
        const auto samples = parser.feed(std::span<const std::uint8_t>(bytes.data(),
                                                                       static_cast<std::size_t>(count)));
        if (!samples.empty() && publish_rate.due(std::chrono::steady_clock::now())) {
            const auto next_sequence = sequence + 1U;
            try {
                publish_sample(publisher, host_clock, config, samples.back(), next_sequence);
                sequence = next_sequence;
            } catch (const std::exception& error) {
                std::cerr << "IMU publication failed: " << error.what() << '\n';
            }
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto overrides = parse_arguments(argc, argv);
        return run(get_config(overrides));
    } catch (const std::exception& error) {
        std::cerr << "rbt_dm_imu: " << error.what() << '\n';
        return 1;
    }
}

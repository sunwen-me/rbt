#include <rbt_dm_imu/config.hpp>

#include <yaml-cpp/yaml.h>

#include <cmath>
#include <stdexcept>
#include <string_view>

namespace rbt_dm_imu {
namespace {

[[noreturn]] void invalid(std::string_view message) {
    throw std::invalid_argument(std::string(message));
}

[[nodiscard]] std::uint8_t parse_byte(const YAML::Node& node, std::string_view name) {
    const auto value = node.as<unsigned>();
    if (value > 255U) {
        invalid(std::string(name) + " values must be between 0 and 255");
    }
    return static_cast<std::uint8_t>(value);
}

[[nodiscard]] std::optional<BlockTag> parse_tag(const YAML::Node& node) {
    if (!node || node.IsNull()) {
        return std::nullopt;
    }
    if (!node.IsSequence() || node.size() != 4U) {
        invalid("protocol.expected_tags entries must be null or four byte values");
    }
    BlockTag result;
    for (std::size_t index = 0U; index < result.bytes.size(); ++index) {
        result.bytes[index] = parse_byte(node[index], "protocol.expected_tags");
    }
    return result;
}

[[nodiscard]] std::optional<std::uint8_t> parse_terminator(const YAML::Node& node) {
    if (!node || node.IsNull()) {
        return std::nullopt;
    }
    return parse_byte(node, "protocol.expected_terminators");
}

} // namespace

PublishRateGate::PublishRateGate(std::uint32_t rate_hz, Clock::time_point phase_start)
    : period_(Clock::duration::zero()), next_deadline_(phase_start) {
    if (rate_hz < 1U || rate_hz > 1000U) {
        invalid("publish_rate_hz must be between 1 and 1000");
    }
    period_ = std::chrono::duration_cast<Clock::duration>(std::chrono::seconds(1)) / rate_hz;
}

bool PublishRateGate::due(Clock::time_point now) {
    if (now < next_deadline_) {
        return false;
    }
    const auto elapsed = now - next_deadline_;
    const auto missed_periods = elapsed / period_;
    next_deadline_ += period_ * (missed_periods + 1);
    return true;
}

Config load_config(const std::optional<std::string>& path) {
    Config config;
    if (!path.has_value()) {
        validate_config(config);
        return config;
    }

    const auto yaml = YAML::LoadFile(*path);
    if (const auto serial = yaml["serial_port"]) {
        config.serial_port = serial.as<std::string>();
    }
    if (const auto baud = yaml["baud"]) {
        config.baud = baud.as<std::uint32_t>();
    }
    if (const auto publish_rate = yaml["publish_rate_hz"]) {
        config.publish_rate_hz = publish_rate.as<std::uint32_t>();
    }
    if (const auto frame = yaml["frame"]) {
        config.frame = frame.as<std::string>();
    }
    if (const auto topic = yaml["topic"]) {
        config.topic = topic.as<std::string>();
    }
    if (const auto units = yaml["source_units"]) {
        if (const auto gyro = units["gyro"]) {
            config.gyro_unit = parse_gyro_unit(gyro.as<std::string>());
        }
        if (const auto acceleration = units["acceleration"]) {
            config.acceleration_unit = parse_acceleration_unit(acceleration.as<std::string>());
        }
    }
    if (const auto protocol = yaml["protocol"]) {
        if (const auto tags = protocol["expected_tags"]) {
            if (!tags.IsSequence() || tags.size() != 3U) {
                invalid("protocol.expected_tags must contain three entries");
            }
            for (std::size_t index = 0U; index < 3U; ++index) {
                config.protocol.expected_tags[index] = parse_tag(tags[index]);
            }
        }
        if (const auto terminators = protocol["expected_terminators"]) {
            if (!terminators.IsSequence() || terminators.size() != 3U) {
                invalid("protocol.expected_terminators must contain three entries");
            }
            for (std::size_t index = 0U; index < 3U; ++index) {
                config.protocol.expected_terminators[index] = parse_terminator(terminators[index]);
            }
        }
    }
    if (const auto startup = yaml["startup"]) {
        if (const auto configure = startup["configure_device"]) {
            config.configure_device = configure.as<bool>();
        }
        if (const auto save = startup["save_parameters"]) {
            config.save_parameters = save.as<bool>();
        }
    }
    if (const auto reconnect = yaml["reconnect"]) {
        if (const auto initial = reconnect["initial_delay_ms"]) {
            config.reconnect_initial = std::chrono::milliseconds(initial.as<std::uint32_t>());
        }
        if (const auto maximum = reconnect["maximum_delay_ms"]) {
            config.reconnect_maximum = std::chrono::milliseconds(maximum.as<std::uint32_t>());
        }
    }

    validate_config(config);
    return config;
}

void validate_config(const Config& config) {
    if (config.serial_port.empty()) {
        invalid("serial_port cannot be empty");
    }
    if (config.baud != 921600U) {
        invalid("only the protocol's documented 921600 baud setting is supported");
    }
    if (config.publish_rate_hz < 1U || config.publish_rate_hz > 1000U) {
        invalid("publish_rate_hz must be between 1 and 1000");
    }
    if (config.frame.empty() || config.frame.size() >= 64U) {
        invalid("frame must contain between 1 and 63 bytes");
    }
    if (config.topic.empty()) {
        invalid("topic cannot be empty");
    }
    if (config.save_parameters && !config.configure_device) {
        invalid("save_parameters requires configure_device");
    }
    if (config.reconnect_initial.count() < 20
        || config.reconnect_maximum < config.reconnect_initial
        || config.reconnect_maximum > std::chrono::seconds(60)) {
        invalid("reconnect delays must satisfy 20 ms <= initial <= maximum <= 60000 ms");
    }
    if (config.duration.has_value()
        && (!std::isfinite(config.duration->count()) || config.duration->count() <= 0.0
            || config.duration->count() > 86400.0)) {
        invalid("duration must be greater than zero and no more than 86400 seconds");
    }
    constexpr BlockTag anchor {{0x55U, 0xaaU, 0x01U, 0x01U}};
    if (!config.protocol.expected_tags[0].has_value()
        || config.protocol.expected_tags[0]->bytes != anchor.bytes) {
        invalid("the first record tag is fixed to the established 55 aa 01 01 anchor");
    }
}

GyroUnit parse_gyro_unit(const std::string& value) {
    if (value == "rad_s") {
        return GyroUnit::RadiansPerSecond;
    }
    if (value == "deg_s") {
        return GyroUnit::DegreesPerSecond;
    }
    invalid("gyro unit must be rad_s or deg_s");
}

AccelerationUnit parse_acceleration_unit(const std::string& value) {
    if (value == "m_s2") {
        return AccelerationUnit::MetersPerSecondSquared;
    }
    if (value == "g") {
        return AccelerationUnit::StandardGravity;
    }
    invalid("acceleration unit must be m_s2 or g");
}

} // namespace rbt_dm_imu

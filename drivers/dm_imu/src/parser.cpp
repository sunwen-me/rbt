#include <rbt_dm_imu/parser.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace rbt_dm_imu {
namespace {

constexpr double kDegreesToRadians = 0.017453292519943295769236907684886;
constexpr double kStandardGravity = 9.80665;

[[nodiscard]] float read_float_le(const std::uint8_t* bytes) noexcept {
    const std::uint32_t bits = static_cast<std::uint32_t>(bytes[0])
                               | (static_cast<std::uint32_t>(bytes[1]) << 8U)
                               | (static_cast<std::uint32_t>(bytes[2]) << 16U)
                               | (static_cast<std::uint32_t>(bytes[3]) << 24U);
    return std::bit_cast<float>(bits);
}

[[nodiscard]] std::uint16_t read_u16_le(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>(bytes[0])
           | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[1]) << 8U);
}

[[nodiscard]] bool finite(const std::array<double, 3>& values) noexcept {
    return std::all_of(values.begin(), values.end(), [](double value) { return std::isfinite(value); });
}

} // namespace

Parser::Parser(ProtocolOptions options) : options_(std::move(options)) {}

std::uint16_t crc16(std::span<const std::uint8_t> bytes) noexcept {
    std::uint16_t crc = 0xffffU;
    for (const auto byte : bytes) {
        const auto index = static_cast<std::uint8_t>((crc >> 8U) ^ byte);
        auto contribution = static_cast<std::uint16_t>(static_cast<std::uint16_t>(index) << 8U);
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            contribution = (contribution & 0x8000U) != 0U
                               ? static_cast<std::uint16_t>((contribution << 1U) ^ 0x1021U)
                               : static_cast<std::uint16_t>(contribution << 1U);
        }
        // The device reference shifts the running CRC once per byte; its table
        // entry supplies the usual 0x1021 contribution for the high-byte index.
        crc = static_cast<std::uint16_t>((crc << 1U) ^ contribution);
    }
    return crc;
}

std::vector<RawImuSample> Parser::feed(std::span<const std::uint8_t> bytes) {
    std::vector<RawImuSample> output;
    output.reserve(bytes.size() / frame_size + 1U);
    for (const auto byte : bytes) {
        if (buffered_ == buffer_.size()) {
            discard_prefix(1U);
        }
        buffer_[buffered_++] = byte;
        process(output);
    }
    return output;
}

void Parser::reset() noexcept {
    buffered_ = 0U;
}

std::uint64_t Parser::rejected_frames() const noexcept {
    return rejected_frames_;
}

bool Parser::is_anchor_at(std::size_t offset) const noexcept {
    constexpr std::array<std::uint8_t, 4> anchor {0x55U, 0xaaU, 0x01U, 0x01U};
    return buffered_ >= offset + anchor.size()
           && std::equal(anchor.begin(), anchor.end(), buffer_.begin() + static_cast<std::ptrdiff_t>(offset));
}

bool Parser::decode(RawImuSample& sample) const noexcept {
    for (std::size_t record = 0U; record < 3U; ++record) {
        const auto offset = record * record_size;
        if (options_.expected_tags[record].has_value()
            && !std::equal(options_.expected_tags[record]->bytes.begin(),
                           options_.expected_tags[record]->bytes.end(), buffer_.begin() + offset)) {
            return false;
        }
        if (options_.expected_terminators[record].has_value()
            && buffer_[offset + 18U] != *options_.expected_terminators[record]) {
            return false;
        }
        const auto actual_crc = read_u16_le(buffer_.data() + offset + 16U);
        if (actual_crc != crc16(std::span<const std::uint8_t>(buffer_.data() + offset, 16U))) {
            return false;
        }
    }

    std::array<float, 9> values {};
    for (std::size_t record = 0U; record < 3U; ++record) {
        const auto offset = record * record_size + 4U;
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            values[record * 3U + axis] = read_float_le(buffer_.data() + offset + axis * sizeof(float));
            if (!std::isfinite(values[record * 3U + axis])) {
                return false;
            }
        }
    }

    sample.acceleration = {values[0], values[1], values[2]};
    sample.angular_velocity = {values[3], values[4], values[5]};
    sample.euler_degrees = {values[6], values[7], values[8]};
    return true;
}

void Parser::discard_prefix(std::size_t count) noexcept {
    count = std::min(count, buffered_);
    if (count < buffered_) {
        std::move(buffer_.begin() + static_cast<std::ptrdiff_t>(count),
                  buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_), buffer_.begin());
    }
    buffered_ -= count;
}

void Parser::process(std::vector<RawImuSample>& output) {
    while (true) {
        std::size_t anchor = 0U;
        while (anchor + 4U <= buffered_ && !is_anchor_at(anchor)) {
            ++anchor;
        }
        if (anchor + 4U > buffered_) {
            // Retain only a possible partial anchor so noise cannot grow the buffer.
            const auto keep = std::min<std::size_t>(3U, buffered_);
            discard_prefix(buffered_ - keep);
            return;
        }
        if (anchor != 0U) {
            discard_prefix(anchor);
        }
        if (buffered_ < frame_size) {
            return;
        }

        RawImuSample sample;
        if (decode(sample)) {
            output.push_back(sample);
            discard_prefix(frame_size);
        } else {
            ++rejected_frames_;
            // Search every byte of a rejected candidate for the next known anchor.
            discard_prefix(1U);
        }
    }
}

ImuSample to_si(const RawImuSample& sample, GyroUnit gyro_unit, AccelerationUnit acceleration_unit) {
    ImuSample result;
    const auto gyro_scale = gyro_unit == GyroUnit::DegreesPerSecond ? kDegreesToRadians : 1.0;
    const auto acceleration_scale = acceleration_unit == AccelerationUnit::StandardGravity ? kStandardGravity : 1.0;
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
        result.angular_velocity_rad_s[axis] = sample.angular_velocity[axis] * gyro_scale;
        result.linear_acceleration_m_s2[axis] = sample.acceleration[axis] * acceleration_scale;
    }

    const auto roll = sample.euler_degrees[0] * kDegreesToRadians;
    const auto pitch = sample.euler_degrees[1] * kDegreesToRadians;
    const auto yaw = sample.euler_degrees[2] * kDegreesToRadians;
    const auto cr = std::cos(roll * 0.5);
    const auto sr = std::sin(roll * 0.5);
    const auto cp = std::cos(pitch * 0.5);
    const auto sp = std::sin(pitch * 0.5);
    const auto cy = std::cos(yaw * 0.5);
    const auto sy = std::sin(yaw * 0.5);
    result.orientation_xyzw = {
        sr * cp * cy - cr * sp * sy,
        cr * sp * cy + sr * cp * sy,
        cr * cp * sy - sr * sp * cy,
        cr * cp * cy + sr * sp * sy,
    };
    const auto norm = std::sqrt(result.orientation_xyzw[0] * result.orientation_xyzw[0]
                                + result.orientation_xyzw[1] * result.orientation_xyzw[1]
                                + result.orientation_xyzw[2] * result.orientation_xyzw[2]
                                + result.orientation_xyzw[3] * result.orientation_xyzw[3]);
    if (!std::isfinite(norm) || norm == 0.0 || !finite(result.angular_velocity_rad_s)
        || !finite(result.linear_acceleration_m_s2)) {
        throw std::invalid_argument("IMU conversion produced a non-finite sample");
    }
    for (auto& value : result.orientation_xyzw) {
        value /= norm;
    }
    return result;
}

} // namespace rbt_dm_imu

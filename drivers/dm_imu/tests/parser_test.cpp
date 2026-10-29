#include <rbt_dm_imu/parser.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace {

using rbt_dm_imu::Parser;
using Frame = std::array<std::uint8_t, Parser::frame_size>;

constexpr Frame captured_device_frame {
    0x55U, 0xaaU, 0x01U, 0x01U, 0xa4U, 0x6cU, 0x0dU, 0xbeU, 0xdaU, 0x91U, 0x1dU, 0xbeU,
    0x47U, 0xf7U, 0x1fU, 0x41U, 0x73U, 0x25U, 0x0aU,
    0x55U, 0xaaU, 0x01U, 0x02U, 0x2aU, 0xc9U, 0x05U, 0x3bU, 0xf7U, 0xb4U, 0xa5U, 0xbbU,
    0x78U, 0x8cU, 0x42U, 0xbbU, 0xebU, 0x44U, 0x0aU,
    0x55U, 0xaaU, 0x01U, 0x03U, 0xceU, 0x44U, 0x6fU, 0xbfU, 0x7bU, 0x29U, 0x3aU, 0x3fU,
    0xf4U, 0xe0U, 0xb9U, 0xbeU, 0xb7U, 0xe3U, 0x0aU,
};

void write_float(std::uint8_t* bytes, float value) {
    const auto bits = std::bit_cast<std::uint32_t>(value);
    bytes[0] = static_cast<std::uint8_t>(bits & 0xffU);
    bytes[1] = static_cast<std::uint8_t>((bits >> 8U) & 0xffU);
    bytes[2] = static_cast<std::uint8_t>((bits >> 16U) & 0xffU);
    bytes[3] = static_cast<std::uint8_t>((bits >> 24U) & 0xffU);
}

void finish_record(std::uint8_t* bytes) {
    const auto crc = rbt_dm_imu::crc16(std::span<const std::uint8_t>(bytes, 16U));
    bytes[16] = static_cast<std::uint8_t>(crc & 0xffU);
    bytes[17] = static_cast<std::uint8_t>((crc >> 8U) & 0xffU);
}

[[nodiscard]] std::uint16_t standard_crc16(std::span<const std::uint8_t> bytes) {
    std::uint16_t crc = 0xffffU;
    for (const auto byte : bytes) {
        crc ^= static_cast<std::uint16_t>(byte) << 8U;
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 0x8000U) != 0U
                      ? static_cast<std::uint16_t>((crc << 1U) ^ 0x1021U)
                      : static_cast<std::uint16_t>(crc << 1U);
        }
    }
    return crc;
}

void finish_record_standard(std::uint8_t* bytes) {
    const auto crc = standard_crc16(std::span<const std::uint8_t>(bytes, 16U));
    bytes[16] = static_cast<std::uint8_t>(crc & 0xffU);
    bytes[17] = static_cast<std::uint8_t>((crc >> 8U) & 0xffU);
}

[[nodiscard]] Frame make_frame(std::array<float, 3> acceleration = {1.0F, 2.0F, 3.0F},
                               std::array<float, 3> gyro = {0.1F, 0.2F, 0.3F},
                               std::array<float, 3> euler = {0.0F, 0.0F, 90.0F}) {
    Frame frame {};
    constexpr std::array<std::array<std::uint8_t, 4>, 3> tags {{
        {{0x55U, 0xaaU, 0x01U, 0x01U}},
        {{0x11U, 0x22U, 0x33U, 0x44U}},
        {{0x66U, 0x77U, 0x88U, 0x99U}},
    }};
    const std::array<std::array<float, 3>, 3> values {acceleration, gyro, euler};
    for (std::size_t record = 0U; record < 3U; ++record) {
        auto* bytes = frame.data() + record * Parser::record_size;
        std::copy(tags[record].begin(), tags[record].end(), bytes);
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            write_float(bytes + 4U + axis * sizeof(float), values[record][axis]);
        }
        bytes[18] = static_cast<std::uint8_t>(0xe0U + record);
        finish_record(bytes);
    }
    return frame;
}

void test_crc16() {
    constexpr std::array<std::uint8_t, 9> text {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    assert(rbt_dm_imu::crc16(text) == 0x5f16U);
    assert(standard_crc16(text) == 0x29b1U);
}

void test_captured_frame_crc_and_decode() {
    constexpr std::array<std::uint16_t, 3> expected_crcs {0x2573U, 0x44ebU, 0xe3b7U};
    for (std::size_t record = 0U; record < expected_crcs.size(); ++record) {
        const auto offset = record * Parser::record_size;
        assert(rbt_dm_imu::crc16(std::span<const std::uint8_t>(captured_device_frame.data() + offset, 16U))
               == expected_crcs[record]);
    }

    Parser parser;
    constexpr std::array<std::uint8_t, 4> noise {0x00U, 0x55U, 0x00U, 0x7fU};
    assert(parser.feed(noise).empty());
    assert(parser.feed(std::span<const std::uint8_t>(captured_device_frame.data(), 13U)).empty());
    assert(parser.feed(std::span<const std::uint8_t>(captured_device_frame.data() + 13U, 21U)).empty());
    const auto decoded = parser.feed(std::span<const std::uint8_t>(captured_device_frame.data() + 34U,
                                                                   captured_device_frame.size() - 34U));
    assert(decoded.size() == 1U);
    constexpr std::array<double, 3> acceleration {
        -0.1381097435951233, -0.15387669205665588, 9.997870445251465,
    };
    constexpr std::array<double, 3> gyro {
        0.002041409257799387, -0.005056973081082106, -0.002968577668070793,
    };
    constexpr std::array<double, 3> euler {
        -0.9346436262130737, 0.7271954417228699, -0.3630443811416626,
    };
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
        assert(std::abs(decoded[0].acceleration[axis] - acceleration[axis]) < 1e-7);
        assert(std::abs(decoded[0].angular_velocity[axis] - gyro[axis]) < 1e-9);
        assert(std::abs(decoded[0].euler_degrees[axis] - euler[axis]) < 1e-7);
    }
}

void test_damaged_capture_and_standard_crc_are_rejected() {
    for (std::size_t record = 0U; record < 3U; ++record) {
        auto damaged = captured_device_frame;
        damaged[record * Parser::record_size + 4U] ^= 0x01U;
        Parser parser;
        assert(parser.feed(damaged).empty());
        assert(parser.rejected_frames() == 1U);
    }

    auto standard_crc_frame = make_frame();
    for (std::size_t record = 0U; record < 3U; ++record) {
        finish_record_standard(standard_crc_frame.data() + record * Parser::record_size);
    }
    Parser parser;
    assert(parser.feed(standard_crc_frame).empty());
    assert(parser.rejected_frames() == 1U);
}

void test_fragmented_and_coalesced_frames() {
    Parser fragmented;
    const auto frame = make_frame();
    assert(fragmented.feed(std::span<const std::uint8_t>(frame.data(), 7U)).empty());
    assert(fragmented.feed(std::span<const std::uint8_t>(frame.data() + 7U, 20U)).empty());
    const auto last = fragmented.feed(std::span<const std::uint8_t>(frame.data() + 27U, frame.size() - 27U));
    assert(last.size() == 1U);
    assert(last[0].acceleration[0] == 1.0);
    assert(last[0].angular_velocity[2] > 0.29 && last[0].angular_velocity[2] < 0.31);
    assert(last[0].euler_degrees[2] == 90.0);

    Parser coalesced;
    std::vector<std::uint8_t> stream;
    for (unsigned index = 0U; index < 3U; ++index) {
        const auto next = make_frame();
        stream.insert(stream.end(), next.begin(), next.end());
    }
    assert(coalesced.feed(stream).size() == 3U);
}

void test_noise_prefix_and_recovery() {
    Parser parser;
    const auto frame = make_frame();
    std::vector<std::uint8_t> stream {0x00U, 0x55U, 0x00U, 0xaaU, 0x01U, 0x01U};
    auto wrong_prefix = frame;
    wrong_prefix[0] = 0x54U;
    stream.insert(stream.end(), wrong_prefix.begin(), wrong_prefix.end());
    stream.insert(stream.end(), frame.begin(), frame.end());
    const auto decoded = parser.feed(stream);
    assert(decoded.size() == 1U);
    assert(parser.rejected_frames() == 0U);
    parser.reset();
    assert(parser.feed(frame).size() == 1U);
}

void test_every_crc_must_pass() {
    for (std::size_t bad_record = 0U; bad_record < 3U; ++bad_record) {
        Parser parser;
        auto damaged = make_frame();
        damaged[bad_record * Parser::record_size + 16U] ^= 0x01U;
        std::vector<std::uint8_t> stream(damaged.begin(), damaged.end());
        const auto good = make_frame();
        stream.insert(stream.end(), good.begin(), good.end());
        const auto decoded = parser.feed(stream);
        assert(decoded.size() == 1U);
        assert(parser.rejected_frames() == 1U);
    }
}

void test_non_finite_and_optional_protocol_checks() {
    Parser parser;
    auto not_finite = make_frame({std::numeric_limits<float>::quiet_NaN(), 2.0F, 3.0F});
    std::vector<std::uint8_t> stream(not_finite.begin(), not_finite.end());
    const auto good = make_frame();
    stream.insert(stream.end(), good.begin(), good.end());
    assert(parser.feed(stream).size() == 1U);
    assert(parser.rejected_frames() == 1U);

    rbt_dm_imu::ProtocolOptions options;
    options.expected_tags[1] = rbt_dm_imu::BlockTag {{0x11U, 0x22U, 0x33U, 0x44U}};
    options.expected_tags[2] = rbt_dm_imu::BlockTag {{0x66U, 0x77U, 0x88U, 0x99U}};
    options.expected_terminators[0] = 0xe0U;
    options.expected_terminators[1] = 0xe1U;
    options.expected_terminators[2] = 0xe2U;

    auto wrong_tag = good;
    wrong_tag[Parser::record_size] ^= 0x01U;
    finish_record(wrong_tag.data() + Parser::record_size);
    Parser tag_checked(options);
    std::vector<std::uint8_t> tag_stream(wrong_tag.begin(), wrong_tag.end());
    tag_stream.insert(tag_stream.end(), good.begin(), good.end());
    assert(tag_checked.feed(tag_stream).size() == 1U);
    assert(tag_checked.rejected_frames() == 1U);

    auto wrong_terminator = good;
    wrong_terminator[Parser::record_size + 18U] ^= 0x01U;
    Parser terminator_checked(options);
    std::vector<std::uint8_t> terminator_stream(wrong_terminator.begin(), wrong_terminator.end());
    terminator_stream.insert(terminator_stream.end(), good.begin(), good.end());
    assert(terminator_checked.feed(terminator_stream).size() == 1U);
    assert(terminator_checked.rejected_frames() == 1U);
}

void test_si_conversion_and_euler_quaternion() {
    rbt_dm_imu::RawImuSample raw;
    raw.acceleration = {1.0, 0.0, -1.0};
    raw.angular_velocity = {180.0, 0.0, -90.0};
    raw.euler_degrees = {0.0, 0.0, 90.0};
    const auto sample = rbt_dm_imu::to_si(raw,
                                          rbt_dm_imu::GyroUnit::DegreesPerSecond,
                                          rbt_dm_imu::AccelerationUnit::StandardGravity);
    constexpr double half_root = 0.7071067811865475244;
    assert(std::abs(sample.angular_velocity_rad_s[0] - 3.14159265358979323846) < 1e-12);
    assert(std::abs(sample.angular_velocity_rad_s[2] + 1.57079632679489661923) < 1e-12);
    assert(std::abs(sample.linear_acceleration_m_s2[0] - 9.80665) < 1e-12);
    assert(std::abs(sample.linear_acceleration_m_s2[2] + 9.80665) < 1e-12);
    assert(std::abs(sample.orientation_xyzw[0]) < 1e-12);
    assert(std::abs(sample.orientation_xyzw[1]) < 1e-12);
    assert(std::abs(sample.orientation_xyzw[2] - half_root) < 1e-12);
    assert(std::abs(sample.orientation_xyzw[3] - half_root) < 1e-12);
    const auto norm = std::sqrt(sample.orientation_xyzw[0] * sample.orientation_xyzw[0]
                                + sample.orientation_xyzw[1] * sample.orientation_xyzw[1]
                                + sample.orientation_xyzw[2] * sample.orientation_xyzw[2]
                                + sample.orientation_xyzw[3] * sample.orientation_xyzw[3]);
    assert(std::abs(norm - 1.0) < 1e-12);
}

} // namespace

int main() {
    test_crc16();
    test_captured_frame_crc_and_decode();
    test_damaged_capture_and_standard_crc_are_rejected();
    test_fragmented_and_coalesced_frames();
    test_noise_prefix_and_recovery();
    test_every_crc_must_pass();
    test_non_finite_and_optional_protocol_checks();
    test_si_conversion_and_euler_quaternion();
    return 0;
}

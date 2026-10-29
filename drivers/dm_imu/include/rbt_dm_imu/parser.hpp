#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace rbt_dm_imu {

enum class GyroUnit {
    RadiansPerSecond,
    DegreesPerSecond,
};

enum class AccelerationUnit {
    MetersPerSecondSquared,
    StandardGravity,
};

struct BlockTag {
    std::array<std::uint8_t, 4> bytes {};
};

struct ProtocolOptions {
    std::array<std::optional<BlockTag>, 3> expected_tags {
        BlockTag {{0x55U, 0xaaU, 0x01U, 0x01U}},
        std::nullopt,
        std::nullopt,
    };
    std::array<std::optional<std::uint8_t>, 3> expected_terminators {};
};

struct RawImuSample {
    std::array<double, 3> acceleration {};
    std::array<double, 3> angular_velocity {};
    std::array<double, 3> euler_degrees {};
};

struct ImuSample {
    std::array<double, 4> orientation_xyzw {};
    std::array<double, 3> angular_velocity_rad_s {};
    std::array<double, 3> linear_acceleration_m_s2 {};
};

class Parser {
public:
    static constexpr std::size_t record_size = 19U;
    static constexpr std::size_t frame_size = record_size * 3U;

    explicit Parser(ProtocolOptions options = {});

    [[nodiscard]] std::vector<RawImuSample> feed(std::span<const std::uint8_t> bytes);
    void reset() noexcept;
    [[nodiscard]] std::uint64_t rejected_frames() const noexcept;

private:
    [[nodiscard]] bool is_anchor_at(std::size_t offset) const noexcept;
    [[nodiscard]] bool decode(RawImuSample& sample) const noexcept;
    void discard_prefix(std::size_t count) noexcept;
    void process(std::vector<RawImuSample>& output);

    ProtocolOptions options_;
    std::array<std::uint8_t, frame_size> buffer_ {};
    std::size_t buffered_ = 0U;
    std::uint64_t rejected_frames_ = 0U;
};

[[nodiscard]] ImuSample to_si(const RawImuSample& sample,
                              GyroUnit gyro_unit,
                              AccelerationUnit acceleration_unit);
[[nodiscard]] std::uint16_t crc16(std::span<const std::uint8_t> bytes) noexcept;

} // namespace rbt_dm_imu

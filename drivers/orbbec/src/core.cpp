#include <rbt_orbbec/driver.hpp>

#include <rbt/tf_wire.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace rbt_orbbec {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic { 'R', 'B', 'T', 'O', 'R', 'B', '0', '1' };
constexpr std::uint32_t kMaximumDimension = 4096U;
constexpr std::uint32_t kMaximumFps = 240U;
constexpr std::uint32_t kMaximumTimeoutMs = 60000U;
constexpr std::size_t kMaximumPayloadBytes = kMaximumFrameBytes - kFrameHeaderBytes;
constexpr double kRotationTolerance = 1.0e-6;

[[noreturn]] void invalid(std::string_view message) {
    throw std::invalid_argument(std::string(message));
}

[[nodiscard]] bool is_kind(Kind kind) noexcept {
    switch (kind) {
    case Kind::Color:
    case Kind::Depth:
    case Kind::LeftIr:
    case Kind::RightIr:
    case Kind::Points:
    case Kind::Accel:
    case Kind::Gyro:
        return true;
    }
    return false;
}

[[nodiscard]] bool is_format(Format format) noexcept {
    switch (format) {
    case Format::Rgb8:
    case Format::Mono8:
    case Format::Mono16:
    case Format::Depth16:
    case Format::Xyz32f:
    case Format::Accel32f:
    case Format::Gyro32f:
        return true;
    }
    return false;
}

[[nodiscard]] bool format_matches(Kind kind, Format format) noexcept {
    switch (kind) {
    case Kind::Color:
        return format == Format::Rgb8;
    case Kind::Depth:
        return format == Format::Depth16;
    case Kind::LeftIr:
    case Kind::RightIr:
        return format == Format::Mono8 || format == Format::Mono16;
    case Kind::Points:
        return format == Format::Xyz32f;
    case Kind::Accel:
        return format == Format::Accel32f;
    case Kind::Gyro:
        return format == Format::Gyro32f;
    }
    return false;
}

[[nodiscard]] std::size_t bytes_per_pixel(Format format) {
    switch (format) {
    case Format::Rgb8:
        return 3U;
    case Format::Mono8:
        return 1U;
    case Format::Mono16:
    case Format::Depth16:
        return 2U;
    case Format::Xyz32f:
        return 12U;
    case Format::Accel32f:
    case Format::Gyro32f:
        return 12U;
    }
    invalid("unknown frame format");
}

[[nodiscard]] std::size_t checked_product(std::size_t lhs, std::size_t rhs) {
    if (rhs != 0U && lhs > std::numeric_limits<std::size_t>::max() / rhs) {
        invalid("frame byte size overflows");
    }
    return lhs * rhs;
}

[[nodiscard]] std::uint32_t read_u32_le(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4U) {
        invalid("truncated frame header");
    }
    return static_cast<std::uint32_t>(bytes[offset])
           | (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U)
           | (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U)
           | (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

[[nodiscard]] std::uint64_t read_u64_le(std::span<const std::uint8_t> bytes, std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 8U) {
        invalid("truncated frame header");
    }
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

void write_u32_le(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
    }
}

void write_u64_le(std::span<std::uint8_t> bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0U; index < 8U; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
    }
}

[[nodiscard]] std::uint32_t enum_u32(Kind value) noexcept {
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] std::uint32_t enum_u32(Format value) noexcept {
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] bool reserved_zero(std::span<const std::uint8_t> bytes,
                                 std::size_t offset,
                                 std::size_t count) noexcept {
    return std::all_of(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                       bytes.begin() + static_cast<std::ptrdiff_t>(offset + count),
                       [](std::uint8_t byte) { return byte == 0U; });
}

[[nodiscard]] double matrix_determinant(const std::array<double, 9>& matrix) noexcept {
    return matrix[0] * (matrix[4] * matrix[8] - matrix[5] * matrix[7])
           - matrix[1] * (matrix[3] * matrix[8] - matrix[5] * matrix[6])
           + matrix[2] * (matrix[3] * matrix[7] - matrix[4] * matrix[6]);
}

[[nodiscard]] std::string_view frame_suffix(Kind kind) noexcept {
    switch (kind) {
    case Kind::Color:
        return "color_optical";
    case Kind::Depth:
    case Kind::Points:
        return "depth_optical";
    case Kind::LeftIr:
        return "left_ir_optical";
    case Kind::RightIr:
        return "right_ir_optical";
    case Kind::Accel:
        return "accel";
    case Kind::Gyro:
        return "gyro";
    }
    return {};
}

void validate_finite_imu_sample(std::span<const std::uint8_t> data) {
    for (std::size_t offset = 0U; offset < data.size(); offset += sizeof(float)) {
        float sample = 0.0F;
        std::memcpy(&sample, data.data() + offset, sizeof(sample));
        if (!std::isfinite(sample)) {
            invalid("IMU samples must contain only finite floats");
        }
    }
}

} // namespace

const char* kind_name(Kind kind) {
    switch (kind) {
    case Kind::Color:
        return "color";
    case Kind::Depth:
        return "depth";
    case Kind::LeftIr:
        return "left_ir";
    case Kind::RightIr:
        return "right_ir";
    case Kind::Points:
        return "points";
    case Kind::Accel:
        return "accel";
    case Kind::Gyro:
        return "gyro";
    }
    return "unknown";
}

const char* format_name(Format format) {
    switch (format) {
    case Format::Rgb8:
        return "rgb8";
    case Format::Mono8:
        return "mono8";
    case Format::Mono16:
        return "mono16";
    case Format::Depth16:
        return "depth16";
    case Format::Xyz32f:
        return "xyz32f";
    case Format::Accel32f:
        return "accel32f";
    case Format::Gyro32f:
        return "gyro32f";
    }
    return "unknown";
}

std::string frame_id(std::string_view prefix, Kind kind) {
    if (!is_kind(kind)) {
        invalid("unknown frame kind");
    }
    const auto suffix = frame_suffix(kind);
    if (prefix.size() > 63U - 1U - suffix.size()) {
        invalid("frame prefix is too long for a canonical TF frame id");
    }
    std::string result(prefix);
    result.push_back('/');
    result.append(suffix);
    return std::string(rbt::tf::FrameId(result).str());
}

std::vector<Kind> enabled_kinds(const Options& options) {
    std::vector<Kind> result;
    if (options.color) {
        result.push_back(Kind::Color);
    }
    if (options.depth) {
        result.push_back(Kind::Depth);
    }
    if (options.left_ir) {
        result.push_back(Kind::LeftIr);
    }
    if (options.right_ir) {
        result.push_back(Kind::RightIr);
    }
    if (options.points) {
        result.push_back(Kind::Points);
    }
    if (options.imu) {
        result.push_back(Kind::Accel);
        result.push_back(Kind::Gyro);
    }
    return result;
}

void validate_options(const Options& options) {
    validate_processing_options(options);
    if (options.serial.empty() || options.serial.size() > 63U
        || options.serial.find('\0') != std::string::npos) {
        invalid("an explicit device serial of at most 63 non-NUL bytes is required");
    }
    if (options.component_id.empty() || options.component_id.find('\0') != std::string::npos) {
        invalid("component id must be nonempty and contain no NUL bytes");
    }
    if (options.width == 0U || options.width > kMaximumDimension || options.height == 0U
        || options.height > kMaximumDimension) {
        invalid("video dimensions must be between 1 and 4096");
    }
    if (options.fps == 0U || options.fps > kMaximumFps) {
        invalid("frame rate must be between 1 and 240");
    }
    if (options.frame_timeout_ms == 0U || options.frame_timeout_ms > kMaximumTimeoutMs) {
        invalid("frame timeout must be between 1 and 60000 milliseconds");
    }
    if (!options.color && !options.depth && !options.left_ir && !options.right_ir) {
        invalid("at least one video stream must be enabled");
    }
    if (options.points && !options.depth) {
        invalid("points require depth to be enabled");
    }

    const auto pixels = checked_product(options.width, options.height);
    std::size_t largest_payload = 0U;
    if (options.color) {
        largest_payload = std::max(largest_payload, checked_product(pixels, 3U));
    }
    if (options.depth || options.left_ir || options.right_ir) {
        largest_payload = std::max(largest_payload, checked_product(pixels, 2U));
    }
    if (options.points) {
        largest_payload = std::max(largest_payload, checked_product(pixels, 12U));
    }
    if (largest_payload > kMaximumPayloadBytes) {
        invalid("configured stream exceeds the 32 MiB frame limit");
    }

    for (const auto kind : enabled_kinds(options)) {
        (void)frame_id(options.frame_prefix, kind);
    }
}

void validate_frame(const Frame& frame) {
    if (!is_kind(frame.kind) || !is_format(frame.format) || !format_matches(frame.kind, frame.format)) {
        invalid("frame kind and format do not match");
    }
    if (frame.width == 0U || frame.width > kMaximumDimension || frame.height == 0U
        || frame.height > kMaximumDimension) {
        invalid("frame dimensions must be between 1 and 4096");
    }
    if (!std::isfinite(frame.unit_scale)) {
        invalid("frame unit scale must be finite");
    }
    if (frame.kind == Kind::Depth) {
        if (frame.unit_scale <= 0.0) {
            invalid("depth unit scale must be positive");
        }
    } else if (frame.unit_scale != 1.0) {
        invalid("non-depth frame unit scale must be 1.0");
    }

    const auto minimum_row_bytes = checked_product(frame.width, bytes_per_pixel(frame.format));
    std::size_t expected_bytes = 0U;
    if (frame.kind == Kind::Points) {
        if (frame.stride != minimum_row_bytes) {
            invalid("point frame stride must exactly match XYZ row size");
        }
        expected_bytes = checked_product(minimum_row_bytes, frame.height);
    } else if (frame.kind == Kind::Accel || frame.kind == Kind::Gyro) {
        if (frame.width != 1U || frame.height != 1U || frame.stride != 12U) {
            invalid("IMU frames must contain one three-float sample");
        }
        expected_bytes = 12U;
    } else {
        if (frame.stride < minimum_row_bytes) {
            invalid("frame stride is shorter than a row");
        }
        expected_bytes = checked_product(frame.stride, frame.height);
    }
    if (expected_bytes > kMaximumPayloadBytes) {
        invalid("frame exceeds the 32 MiB frame limit");
    }
    if (frame.data.size() != expected_bytes) {
        invalid("frame payload size does not match its dimensions and stride");
    }
    if (frame.kind == Kind::Accel || frame.kind == Kind::Gyro) {
        validate_finite_imu_sample(frame.data);
    }
}

void encode_frame(std::span<std::uint8_t> destination,
                  const Frame& frame,
                  std::string_view frame_name) {
    validate_frame(frame);
    const auto name = rbt::tf::FrameId(frame_name);
    const auto total_bytes = kFrameHeaderBytes + frame.data.size();
    if (total_bytes > kMaximumFrameBytes || destination.size() != total_bytes) {
        invalid("destination size must exactly match the bounded frame size");
    }

    std::fill(destination.begin(), destination.begin() + static_cast<std::ptrdiff_t>(kFrameHeaderBytes), 0U);
    std::copy(kMagic.begin(), kMagic.end(), destination.begin());
    write_u32_le(destination, 8U, enum_u32(frame.kind));
    write_u32_le(destination, 12U, enum_u32(frame.format));
    write_u32_le(destination, 16U, frame.width);
    write_u32_le(destination, 20U, frame.height);
    write_u32_le(destination, 24U, frame.stride);
    write_u64_le(destination, 32U, frame.index);
    write_u64_le(destination, 40U, std::bit_cast<std::uint64_t>(frame.unit_scale));
    write_u64_le(destination, 48U, static_cast<std::uint64_t>(frame.data.size()));
    const auto frame_bytes = name.to_bytes();
    std::copy(frame_bytes.begin(), frame_bytes.end(), destination.begin() + 64);
    std::memmove(destination.data() + kFrameHeaderBytes, frame.data.data(), frame.data.size());
}

DecodedFrame decode_frame(std::span<const std::uint8_t> application) {
    if (application.size() < kFrameHeaderBytes || application.size() > kMaximumFrameBytes) {
        invalid("frame envelope size is outside the supported bounds");
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), application.begin())) {
        invalid("invalid frame magic");
    }
    if (!reserved_zero(application, 28U, 4U) || !reserved_zero(application, 56U, 8U)) {
        invalid("reserved frame header bytes must be zero");
    }

    const auto raw_kind = read_u32_le(application, 8U);
    const auto raw_format = read_u32_le(application, 12U);
    const auto kind = static_cast<Kind>(raw_kind);
    const auto format = static_cast<Format>(raw_format);
    const auto width = read_u32_le(application, 16U);
    const auto height = read_u32_le(application, 20U);
    const auto stride = read_u32_le(application, 24U);
    const auto index = read_u64_le(application, 32U);
    const auto unit_scale = std::bit_cast<double>(read_u64_le(application, 40U));
    const auto data_size_u64 = read_u64_le(application, 48U);
    if (data_size_u64 > static_cast<std::uint64_t>(kMaximumPayloadBytes)) {
        invalid("frame payload exceeds the 32 MiB frame limit");
    }
    const auto data_size = static_cast<std::size_t>(data_size_u64);
    if (data_size != application.size() - kFrameHeaderBytes) {
        invalid("frame payload length does not match the envelope");
    }

    rbt::tf::FrameId::Bytes frame_bytes {};
    std::copy_n(application.begin() + 64, frame_bytes.size(), frame_bytes.begin());
    const auto name = rbt::tf::FrameId::from_bytes(frame_bytes);
    const auto data = application.subspan(kFrameHeaderBytes, data_size);
    const Frame frame { kind, format, width, height, stride, index, 0U, unit_scale, data, {} };
    validate_frame(frame);
    return DecodedFrame { kind, format, width, height, stride, index, unit_scale,
                          std::string(name.str()), data };
}

rbt::tf::Transform3 transform_from_extrinsic(const Extrinsic& extrinsic) {
    const auto same_frame_kind = [](Kind kind) {
        return kind == Kind::Points ? Kind::Depth : kind;
    };
    if (!is_kind(extrinsic.parent) || !is_kind(extrinsic.child)
        || same_frame_kind(extrinsic.parent) == same_frame_kind(extrinsic.child)) {
        invalid("extrinsic must connect two distinct known frames");
    }
    for (const auto value : extrinsic.rotation) {
        if (!std::isfinite(value)) {
            invalid("extrinsic rotation must be finite");
        }
    }
    for (const auto value : extrinsic.translation_m) {
        if (!std::isfinite(value)) {
            invalid("extrinsic translation must be finite");
        }
    }

    const auto& r = extrinsic.rotation;
    for (std::size_t row = 0U; row < 3U; ++row) {
        for (std::size_t other = 0U; other < 3U; ++other) {
            double dot = 0.0;
            for (std::size_t column = 0U; column < 3U; ++column) {
                dot += r[row * 3U + column] * r[other * 3U + column];
            }
            const auto expected = row == other ? 1.0 : 0.0;
            if (std::abs(dot - expected) > kRotationTolerance) {
                invalid("extrinsic rotation is not orthonormal");
            }
        }
    }
    const auto determinant = matrix_determinant(r);
    if (!std::isfinite(determinant) || determinant <= 0.0
        || std::abs(determinant - 1.0) > 3.0 * kRotationTolerance) {
        invalid("extrinsic rotation must be a proper rotation");
    }

    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double w = 0.0;
    const auto trace = r[0] + r[4] + r[8];
    if (trace > 0.0) {
        const auto s = std::sqrt(trace + 1.0) * 2.0;
        w = 0.25 * s;
        x = (r[7] - r[5]) / s;
        y = (r[2] - r[6]) / s;
        z = (r[3] - r[1]) / s;
    } else if (r[0] > r[4] && r[0] > r[8]) {
        const auto s = std::sqrt(1.0 + r[0] - r[4] - r[8]) * 2.0;
        w = (r[7] - r[5]) / s;
        x = 0.25 * s;
        y = (r[1] + r[3]) / s;
        z = (r[2] + r[6]) / s;
    } else if (r[4] > r[8]) {
        const auto s = std::sqrt(1.0 + r[4] - r[0] - r[8]) * 2.0;
        w = (r[2] - r[6]) / s;
        x = (r[1] + r[3]) / s;
        y = 0.25 * s;
        z = (r[5] + r[7]) / s;
    } else {
        const auto s = std::sqrt(1.0 + r[8] - r[0] - r[4]) * 2.0;
        w = (r[3] - r[1]) / s;
        x = (r[2] + r[6]) / s;
        y = (r[5] + r[7]) / s;
        z = 0.25 * s;
    }
    const auto norm = std::hypot(std::hypot(x, y), std::hypot(z, w));
    if (!std::isfinite(norm) || norm == 0.0) {
        invalid("extrinsic rotation could not be converted to a quaternion");
    }
    const rbt::tf::Quaternion quaternion(x / norm, y / norm, z / norm, w / norm);
    const rbt::tf::Vec3 translation(extrinsic.translation_m[0],
                                    extrinsic.translation_m[1],
                                    extrinsic.translation_m[2]);
    return rbt::tf::Transform3(translation, quaternion);
}

std::array<std::uint8_t, 16> parse_uuid(std::string_view text) {
    const auto parsed = rbt::tf::detail::parse_uuid(text);
    if (!parsed) {
        invalid("UUID must use canonical 8-4-4-4-12 hexadecimal form");
    }
    return *parsed;
}

std::string json_quote(std::string_view text) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.reserve(text.size() + 2U);
    result.push_back('"');
    for (const auto character : text) {
        const auto byte = static_cast<unsigned char>(character);
        switch (byte) {
        case '"':
            result.append("\\\"");
            break;
        case '\\':
            result.append("\\\\");
            break;
        case '\b':
            result.append("\\b");
            break;
        case '\f':
            result.append("\\f");
            break;
        case '\n':
            result.append("\\n");
            break;
        case '\r':
            result.append("\\r");
            break;
        case '\t':
            result.append("\\t");
            break;
        default:
            if (byte < 0x20U) {
                result.append("\\u00");
                result.push_back(kHex[(byte >> 4U) & 0x0FU]);
                result.push_back(kHex[byte & 0x0FU]);
            } else {
                result.push_back(character);
            }
            break;
        }
    }
    result.push_back('"');
    return result;
}

} // namespace rbt_orbbec

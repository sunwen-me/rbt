#ifndef RBT_CPP_TIME_STREAM_HPP
#define RBT_CPP_TIME_STREAM_HPP

#include <rbt/stream.hpp>
#include <rbt/time.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

namespace rbt::time {

inline constexpr std::size_t kStampedBytesPrefixBytes = 192U;
inline constexpr char STAMPED_BYTES_SIGNATURE[] =
    "header(sequence:u64@0);element:u8@0;"
    "prefix(RBTTIME1@0,flags:u8@8,reserved:zero[7]@9,capture:timestamp72@16,"
    "received:timestamp72@88,capture_uncertainty_ns:u64@160,"
    "received_uncertainty_ns:u64@168,reserved:zero[16]@176);"
    "timestamp72(kind:u8@0,reserved:zero[7]@1,source_id:u8[48]@8,"
    "generation:u64@56,nanoseconds:i64@64);application:u8[]@192";

inline constexpr MessageTypeDescriptor STAMPED_BYTES_DESCRIPTOR {
    "rbt.time",
    "StampedBytes",
    1U,
    MessageShape::Slice,
    8U,
    8U,
    1U,
    1U,
    { 0x97, 0xE9, 0xA9, 0x24, 0x99, 0x2D, 0xED, 0xFE,
      0x83, 0x19, 0x61, 0x97, 0xCC, 0xCB, 0x6B, 0x35 },
    STAMPED_BYTES_SIGNATURE,
};

} // namespace rbt::time

namespace rbt {
template <>
struct SliceMessageBinding<std::uint64_t, std::uint8_t> {
    static constexpr const MessageTypeDescriptor& descriptor() noexcept {
        return time::STAMPED_BYTES_DESCRIPTOR;
    }
};
} // namespace rbt

namespace rbt::time {

using StampedBytesPublisher = SlicePublisher<std::uint64_t, std::uint8_t>;
using StampedBytesSubscriber = SliceSubscriber<std::uint64_t, std::uint8_t>;

[[nodiscard]] inline rbt::Result<StampedBytesPublisher> create_stamped_bytes_publisher(
    const StreamDescriptor& stream,
    std::size_t maximum_elements) {
    return StampedBytesPublisher::create(stream, STAMPED_BYTES_DESCRIPTOR, maximum_elements);
}

[[nodiscard]] inline rbt::Result<StampedBytesSubscriber> create_stamped_bytes_subscriber(
    const StreamDescriptor& stream) {
    return StampedBytesSubscriber::create(stream, STAMPED_BYTES_DESCRIPTOR);
}

enum class PrefixCodecError : std::uint8_t {
    None,
    DestinationTooSmall,
    SourceTooSmall,
    InvalidTiming,
    InvalidMagic,
    InvalidFlags,
    NonzeroReserved,
    InvalidCapture,
    InvalidReceived,
    InvalidUncertainty,
};

[[nodiscard]] constexpr const char* to_string(PrefixCodecError error) noexcept {
    switch (error) {
    case PrefixCodecError::None:
        return "None";
    case PrefixCodecError::DestinationTooSmall:
        return "DestinationTooSmall";
    case PrefixCodecError::SourceTooSmall:
        return "SourceTooSmall";
    case PrefixCodecError::InvalidTiming:
        return "InvalidTiming";
    case PrefixCodecError::InvalidMagic:
        return "InvalidMagic";
    case PrefixCodecError::InvalidFlags:
        return "InvalidFlags";
    case PrefixCodecError::NonzeroReserved:
        return "NonzeroReserved";
    case PrefixCodecError::InvalidCapture:
        return "InvalidCapture";
    case PrefixCodecError::InvalidReceived:
        return "InvalidReceived";
    case PrefixCodecError::InvalidUncertainty:
        return "InvalidUncertainty";
    }
    return "InvalidTiming";
}

struct DecodedStampedBytes {
    SensorTiming timing;
    std::span<const std::uint8_t> bytes;
};

using StampedBytesDecodeResult = std::variant<DecodedStampedBytes, PrefixCodecError>;

namespace detail {

inline constexpr std::array<std::uint8_t, 8> kStampedBytesMagic {
    'R', 'B', 'T', 'T', 'I', 'M', 'E', '1'
};
inline constexpr std::uint8_t kCapturePresent = 1U << 0U;
inline constexpr std::uint8_t kCaptureUncertaintyKnown = 1U << 1U;
inline constexpr std::uint8_t kReceivedUncertaintyKnown = 1U << 2U;
inline constexpr std::uint8_t kKnownFlags =
    kCapturePresent | kCaptureUncertaintyKnown | kReceivedUncertaintyKnown;
inline constexpr std::size_t kStampBytes = 72U;
inline constexpr std::size_t kCaptureOffset = 16U;
inline constexpr std::size_t kReceivedOffset = 88U;
inline constexpr std::size_t kCaptureUncertaintyOffset = 160U;
inline constexpr std::size_t kReceivedUncertaintyOffset = 168U;

[[nodiscard]] inline std::uint64_t load_u64_le(std::span<const std::uint8_t> bytes,
                                                std::size_t offset) noexcept {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

inline void store_u64_le(std::span<std::uint8_t> bytes,
                         std::size_t offset,
                         std::uint64_t value) noexcept {
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
    }
}

[[nodiscard]] inline std::optional<std::uint8_t> wire_kind(ClockKind kind) noexcept {
    switch (kind) {
    case ClockKind::HostMonotonic:
        return 1U;
    case ClockKind::SystemRealtime:
        return 2U;
    case ClockKind::Device:
        return 3U;
    }
    return {};
}

[[nodiscard]] inline std::optional<ClockKind> clock_kind(std::uint8_t kind) noexcept {
    switch (kind) {
    case 1U:
        return ClockKind::HostMonotonic;
    case 2U:
        return ClockKind::SystemRealtime;
    case 3U:
        return ClockKind::Device;
    default:
        return {};
    }
}

[[nodiscard]] inline bool all_zero(std::span<const std::uint8_t> bytes) noexcept {
    for (const auto byte : bytes) {
        if (byte != 0U) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool write_stamp(std::span<std::uint8_t> bytes,
                                       std::size_t offset,
                                       const Timestamp& timestamp) noexcept {
    if (bytes.size() < offset + kStampBytes || !timestamp.clock.is_well_formed()) {
        return false;
    }
    const auto kind = wire_kind(timestamp.clock.kind());
    if (!kind.has_value()) {
        return false;
    }
    std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
              bytes.begin() + static_cast<std::ptrdiff_t>(offset + kStampBytes),
              0U);
    bytes[offset] = *kind;
    const auto& source = timestamp.clock.source_id().bytes();
    std::copy(source.begin(), source.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset + 8U));
    store_u64_le(bytes, offset + 56U, timestamp.clock.generation());
    store_u64_le(bytes, offset + 64U, std::bit_cast<std::uint64_t>(timestamp.ns));
    return true;
}

[[nodiscard]] inline std::optional<Timestamp> read_stamp(std::span<const std::uint8_t> bytes,
                                                          std::size_t offset) {
    if (bytes.size() < offset + kStampBytes || !all_zero(bytes.subspan(offset + 1U, 7U))) {
        return {};
    }
    const auto kind = clock_kind(bytes[offset]);
    if (!kind.has_value()) {
        return {};
    }
    std::array<std::uint8_t, 48> source_bytes {};
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset + 8U),
                source_bytes.size(),
                source_bytes.begin());
    auto identity = ClockIdentity::create(
        *kind, ClockSourceId(std::move(source_bytes)), load_u64_le(bytes, offset + 56U));
    if (!identity) {
        return {};
    }
    return Timestamp { std::move(identity).value(),
                       std::bit_cast<std::int64_t>(load_u64_le(bytes, offset + 64U)) };
}

} // namespace detail

[[nodiscard]] inline PrefixCodecError write_timing_prefix(std::span<std::uint8_t> destination,
                                                           const SensorTiming& timing) noexcept {
    if (destination.size() < kStampedBytesPrefixBytes) {
        return PrefixCodecError::DestinationTooSmall;
    }
    if (!timing.received.clock.is_well_formed()
        || (timing.capture.has_value() && !timing.capture->clock.is_well_formed())
        || (timing.capture_uncertainty_ns.has_value() && !timing.capture.has_value())) {
        return PrefixCodecError::InvalidTiming;
    }

    auto prefix = destination.first(kStampedBytesPrefixBytes);
    std::fill(prefix.begin(), prefix.end(), 0U);
    std::copy(detail::kStampedBytesMagic.begin(),
              detail::kStampedBytesMagic.end(),
              prefix.begin());
    std::uint8_t flags = 0U;
    if (timing.capture.has_value()) {
        flags |= detail::kCapturePresent;
        if (!detail::write_stamp(prefix, detail::kCaptureOffset, *timing.capture)) {
            return PrefixCodecError::InvalidTiming;
        }
    }
    if (timing.capture_uncertainty_ns.has_value()) {
        flags |= detail::kCaptureUncertaintyKnown;
        detail::store_u64_le(prefix,
                             detail::kCaptureUncertaintyOffset,
                             *timing.capture_uncertainty_ns);
    }
    if (timing.received_uncertainty_ns.has_value()) {
        flags |= detail::kReceivedUncertaintyKnown;
        detail::store_u64_le(prefix,
                             detail::kReceivedUncertaintyOffset,
                             *timing.received_uncertainty_ns);
    }
    prefix[8U] = flags;
    if (!detail::write_stamp(prefix, detail::kReceivedOffset, timing.received)) {
        return PrefixCodecError::InvalidTiming;
    }
    return PrefixCodecError::None;
}

[[nodiscard]] inline StampedBytesDecodeResult decode_stamped_bytes(
    std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kStampedBytesPrefixBytes) {
        return PrefixCodecError::SourceTooSmall;
    }
    const auto prefix = bytes.first(kStampedBytesPrefixBytes);
    if (!std::equal(detail::kStampedBytesMagic.begin(),
                    detail::kStampedBytesMagic.end(),
                    prefix.begin())) {
        return PrefixCodecError::InvalidMagic;
    }
    const auto flags = prefix[8U];
    if ((flags & static_cast<std::uint8_t>(~detail::kKnownFlags)) != 0U) {
        return PrefixCodecError::InvalidFlags;
    }
    if (!detail::all_zero(prefix.subspan(9U, 7U))
        || !detail::all_zero(prefix.subspan(176U, 16U))) {
        return PrefixCodecError::NonzeroReserved;
    }

    const bool capture_present = (flags & detail::kCapturePresent) != 0U;
    const bool capture_uncertainty_known = (flags & detail::kCaptureUncertaintyKnown) != 0U;
    const bool received_uncertainty_known = (flags & detail::kReceivedUncertaintyKnown) != 0U;
    if ((!capture_present && capture_uncertainty_known)
        || (!capture_uncertainty_known
            && detail::load_u64_le(prefix, detail::kCaptureUncertaintyOffset) != 0U)
        || (!received_uncertainty_known
            && detail::load_u64_le(prefix, detail::kReceivedUncertaintyOffset) != 0U)) {
        return PrefixCodecError::InvalidUncertainty;
    }

    std::optional<Timestamp> capture;
    if (capture_present) {
        capture = detail::read_stamp(prefix, detail::kCaptureOffset);
        if (!capture.has_value()) {
            return PrefixCodecError::InvalidCapture;
        }
    } else if (!detail::all_zero(prefix.subspan(detail::kCaptureOffset, detail::kStampBytes))) {
        return PrefixCodecError::InvalidCapture;
    }
    auto received = detail::read_stamp(prefix, detail::kReceivedOffset);
    if (!received.has_value()) {
        return PrefixCodecError::InvalidReceived;
    }

    auto timing = SensorTiming::create(
        std::move(capture),
        std::move(*received),
        capture_uncertainty_known
            ? std::optional<std::uint64_t>(detail::load_u64_le(prefix, detail::kCaptureUncertaintyOffset))
            : std::optional<std::uint64_t> {},
        received_uncertainty_known
            ? std::optional<std::uint64_t>(detail::load_u64_le(prefix, detail::kReceivedUncertaintyOffset))
            : std::optional<std::uint64_t> {});
    if (!timing) {
        return capture_present ? PrefixCodecError::InvalidCapture : PrefixCodecError::InvalidReceived;
    }
    return DecodedStampedBytes { std::move(timing).value(), bytes.subspan(kStampedBytesPrefixBytes) };
}

static_assert(std::is_standard_layout_v<std::uint64_t>);
static_assert(std::is_trivially_copyable_v<std::uint64_t>);

} // namespace rbt::time

#endif

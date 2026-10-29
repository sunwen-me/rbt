#ifndef RBT_CPP_TIME_HPP
#define RBT_CPP_TIME_HPP

#include <array>
#include <atomic>
#include <chrono>
#include <compare>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#if defined(__linux__)
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#endif

namespace rbt::time {

/// Stable time API errors. The spelling returned by to_string is part of the
/// cross-language contract.
enum class TimeError : std::uint8_t {
    ClockDomainMismatch,
    ClockSourceMismatch,
    ClockGenerationMismatch,
    UnknownClockDomain,
    ClockMappingUnavailable,
    ClockMappingExpired,
    ClockMappingInvalid,
    MappingInvalidated,
    TimestampOutOfRange,
    TimestampOverflow,
    InvalidTimestamp,
    InvalidConversionRate,
    UnknownUncertainty,
    TimeSourceReset,
    ClockUnavailable,
    InvalidCalibrationEvidence,
    NegativeDuration,
    InvalidClockSourceId,
    InvalidClockIdentity,
};

[[nodiscard]] constexpr const char* to_string(TimeError error) noexcept {
    switch (error) {
    case TimeError::ClockDomainMismatch:
        return "ClockDomainMismatch";
    case TimeError::ClockSourceMismatch:
        return "ClockSourceMismatch";
    case TimeError::ClockGenerationMismatch:
        return "ClockGenerationMismatch";
    case TimeError::UnknownClockDomain:
        return "UnknownClockDomain";
    case TimeError::InvalidClockSourceId:
        return "InvalidClockSourceId";
    case TimeError::InvalidClockIdentity:
        return "InvalidClockIdentity";
    case TimeError::ClockMappingUnavailable:
        return "ClockMappingUnavailable";
    case TimeError::ClockMappingExpired:
        return "ClockMappingExpired";
    case TimeError::ClockMappingInvalid:
        return "ClockMappingInvalid";
    case TimeError::MappingInvalidated:
        return "MappingInvalidated";
    case TimeError::TimestampOutOfRange:
        return "TimestampOutOfRange";
    case TimeError::TimestampOverflow:
        return "TimestampOverflow";
    case TimeError::InvalidTimestamp:
        return "InvalidTimestamp";
    case TimeError::InvalidConversionRate:
        return "InvalidConversionRate";
    case TimeError::UnknownUncertainty:
        return "UnknownUncertainty";
    case TimeError::TimeSourceReset:
        return "TimeSourceReset";
    case TimeError::ClockUnavailable:
        return "ClockUnavailable";
    case TimeError::InvalidCalibrationEvidence:
        return "InvalidCalibrationEvidence";
    case TimeError::NegativeDuration:
        return "NegativeDuration";
    }
    return "ClockUnavailable";
}

/// A local result type keeps the time API independent of the runtime's
/// transport-specific Result<T>.
template <typename T>
class Result {
  public:
    Result(T value)
        : value_(std::move(value)) {
    }

    Result(TimeError error)
        : value_(error) {
    }

    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;
    Result(Result&&) noexcept = default;
    Result& operator=(Result&&) noexcept = default;

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(value_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    T& value() & {
        return std::get<T>(value_);
    }

    const T& value() const& {
        return std::get<T>(value_);
    }

    T&& value() && {
        return std::get<T>(std::move(value_));
    }

    [[nodiscard]] TimeError error() const {
        return std::get<TimeError>(value_);
    }

  private:
    std::variant<T, TimeError> value_;
};

template <>
class Result<void> {
  public:
    Result() noexcept = default;

    Result(TimeError error) noexcept
        : error_(error) {
    }

    [[nodiscard]] bool has_value() const noexcept {
        return !error_.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    void value() const noexcept {
    }

    [[nodiscard]] TimeError error() const noexcept {
        return error_.value_or(TimeError::ClockUnavailable);
    }

  private:
    std::optional<TimeError> error_;
};

enum class ClockKind : std::uint8_t {
    HostMonotonic,
    SystemRealtime,
    Device,
};

/// The source identity is exactly 48 bytes on the wire. It is serialized as
/// 96 lower-case hexadecimal characters without separators.
class ClockSourceId {
  public:
    ClockSourceId() = default;

    explicit ClockSourceId(std::array<std::uint8_t, 48> bytes)
        : bytes_(std::move(bytes)) {
    }

    [[nodiscard]] const std::array<std::uint8_t, 48>& bytes() const noexcept {
        return bytes_;
    }

    [[nodiscard]] bool is_nonzero() const noexcept {
        for (const auto byte : bytes_) {
            if (byte != 0U) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::string to_hex() const {
        static constexpr char digits[] = "0123456789abcdef";
        std::string output;
        output.reserve(96U);
        for (const auto byte : bytes_) {
            output.push_back(digits[(byte >> 4U) & 0x0FU]);
            output.push_back(digits[byte & 0x0FU]);
        }
        return output;
    }

    [[nodiscard]] static Result<ClockSourceId> from_hex(std::string_view hex) {
        if (hex.size() != 96U) {
            return TimeError::InvalidClockSourceId;
        }
        std::array<std::uint8_t, 48> bytes {};
        const auto decode = [](char value) -> int {
            if (value >= '0' && value <= '9') {
                return value - '0';
            }
            if (value >= 'a' && value <= 'f') {
                return value - 'a' + 10;
            }
            return -1;
        };
        for (std::size_t index = 0U; index < bytes.size(); ++index) {
            const auto high = decode(hex[index * 2U]);
            const auto low = decode(hex[index * 2U + 1U]);
            if (high < 0 || low < 0) {
                return TimeError::InvalidClockSourceId;
            }
            bytes[index] = static_cast<std::uint8_t>((high << 4) | low);
        }
        ClockSourceId result(std::move(bytes));
        if (!result.is_nonzero()) {
            return TimeError::InvalidClockSourceId;
        }
        return result;
    }

    [[nodiscard]] friend bool operator==(const ClockSourceId&, const ClockSourceId&) = default;

  private:
    std::array<std::uint8_t, 48> bytes_ {};
};

class ClockIdentity {
  public:
    ClockIdentity() = default;

    [[nodiscard]] static Result<ClockIdentity> create(ClockKind kind,
                                                       ClockSourceId source_id,
                                                       std::uint64_t generation = 0U) {
        ClockIdentity identity(kind, std::move(source_id), generation);
        if (!identity.is_well_formed()) {
            return TimeError::InvalidClockIdentity;
        }
        return identity;
    }

    [[nodiscard]] ClockKind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] const ClockSourceId& source_id() const noexcept {
        return source_id_;
    }

    [[nodiscard]] std::uint64_t generation() const noexcept {
        return generation_;
    }

    [[nodiscard]] Result<void> checked_compatible(const ClockIdentity& other) const noexcept {
        if (!is_well_formed() || !other.is_well_formed()) {
            return TimeError::InvalidClockIdentity;
        }
        if (kind_ != other.kind_) {
            return TimeError::ClockDomainMismatch;
        }
        if (source_id_ != other.source_id_) {
            return TimeError::ClockSourceMismatch;
        }
        if (generation_ != other.generation_) {
            return TimeError::ClockGenerationMismatch;
        }
        return {};
    }

    [[nodiscard]] bool is_well_formed() const noexcept {
        const auto& bytes = source_id_.bytes();
        if (!source_id_.is_nonzero()) {
            return false;
        }
        switch (kind_) {
        case ClockKind::HostMonotonic:
            {
                std::uint64_t offset_nanoseconds = 0U;
                for (std::size_t index = 0U; index < 8U; ++index) {
                    offset_nanoseconds |= static_cast<std::uint64_t>(bytes[40U + index]) << (index * 8U);
                }
                if (offset_nanoseconds >= 1000000000ULL) {
                    return false;
                }
            }
            for (std::size_t index = 0U; index < 16U; ++index) {
                if (bytes[index] != 0U) {
                    return true;
                }
            }
            return false;
        case ClockKind::SystemRealtime:
        case ClockKind::Device:
            for (std::size_t index = 16U; index < bytes.size(); ++index) {
                if (bytes[index] != 0U) {
                    return false;
                }
            }
            for (std::size_t index = 0U; index < 16U; ++index) {
                if (bytes[index] != 0U) {
                    return true;
                }
            }
            return false;
        }
        return false;
    }

    [[nodiscard]] friend bool operator==(const ClockIdentity&, const ClockIdentity&) = default;

  private:
    ClockIdentity(ClockKind kind, ClockSourceId source_id, std::uint64_t generation)
        : kind_(kind)
        , source_id_(std::move(source_id))
        , generation_(generation) {
    }

    ClockKind kind_ { static_cast<ClockKind>(0xFFU) };
    ClockSourceId source_id_ {};
    std::uint64_t generation_ { 0U };
};

struct Timestamp {
    ClockIdentity clock;
    std::int64_t ns { 0 };

    [[nodiscard]] static Result<Timestamp> create(ClockIdentity clock, std::int64_t nanoseconds) {
        if (!clock.is_well_formed()) {
            return TimeError::InvalidClockIdentity;
        }
        return Timestamp { std::move(clock), nanoseconds };
    }
};

namespace detail {

inline constexpr std::uint64_t kNsPerSecond = 1000000000ULL;

[[nodiscard]] inline Result<void> validate_timestamp(const Timestamp& timestamp) noexcept {
    if (!timestamp.clock.is_well_formed()) {
        return TimeError::InvalidTimestamp;
    }
    return {};
}

[[nodiscard]] inline Result<void> compare_clock_identity(const ClockIdentity& left,
                                                         const ClockIdentity& right) noexcept {
    if (!left.is_well_formed() || !right.is_well_formed()) {
        return TimeError::InvalidClockIdentity;
    }
    if (left.kind() != right.kind()) {
        return TimeError::ClockDomainMismatch;
    }
    if (left.source_id() != right.source_id()) {
        return TimeError::ClockSourceMismatch;
    }
    if (left.generation() != right.generation()) {
        return TimeError::ClockGenerationMismatch;
    }
    return {};
}

[[nodiscard]] inline bool uuid_is_nonzero(const std::array<std::uint8_t, 16>& uuid) noexcept {
    for (const auto byte : uuid) {
        if (byte != 0U) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline int hex_digit(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] inline bool parse_uuid(std::string_view text, std::array<std::uint8_t, 16>& uuid) noexcept {
    if (text.size() != 36U) {
        return false;
    }
    constexpr std::array<std::size_t, 4> hyphen_positions { 8U, 13U, 18U, 23U };
    std::size_t byte_index = 0U;
    for (std::size_t index = 0U; index < text.size();) {
        if (index == hyphen_positions[0] || index == hyphen_positions[1]
            || index == hyphen_positions[2] || index == hyphen_positions[3]) {
            if (text[index] != '-') {
                return false;
            }
            ++index;
            continue;
        }
        if (byte_index >= uuid.size() || index + 1U >= text.size()) {
            return false;
        }
        const auto high = hex_digit(text[index]);
        const auto low = hex_digit(text[index + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        uuid[byte_index++] = static_cast<std::uint8_t>((high << 4) | low);
        index += 2U;
    }
    return byte_index == uuid.size() && uuid_is_nonzero(uuid);
}

inline void store_u64_le(std::array<std::uint8_t, 48>& bytes, std::size_t offset, std::uint64_t value) noexcept {
    for (std::size_t index = 0U; index < 8U; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
    }
}

inline void store_i64_le(std::array<std::uint8_t, 48>& bytes, std::size_t offset, std::int64_t value) noexcept {
    store_u64_le(bytes, offset, static_cast<std::uint64_t>(value));
}

[[nodiscard]] inline bool all_zero(const std::array<std::uint8_t, 48>& bytes,
                                   std::size_t begin,
                                   std::size_t end) noexcept {
    for (std::size_t index = begin; index < end; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

#if defined(__SIZEOF_INT128__)
__extension__ typedef __int128 int128_t;
__extension__ typedef unsigned __int128 uint128_t;
#else
#error "rbt::time requires a compiler with 128-bit integer support"
#endif

#if defined(__linux__)
[[nodiscard]] inline bool same_namespace(const struct stat& left, const struct stat& right) noexcept {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

[[nodiscard]] inline bool read_namespace_stat(struct stat& result) noexcept {
    return ::stat("/proc/thread-self/ns/time", &result) == 0;
}

[[nodiscard]] inline bool parse_time_namespace_inode(std::string_view name,
                                                     std::uint64_t& inode) noexcept {
    constexpr std::string_view prefix = "time:[";
    if (name.size() < prefix.size() + 2U || !name.starts_with(prefix) || name.back() != ']') {
        return false;
    }

    const auto digits = name.substr(prefix.size(), name.size() - prefix.size() - 1U);
    if (digits.empty() || digits.front() == '0') {
        return false;
    }
    std::uint64_t parsed = 0U;
    for (const auto digit : digits) {
        if (digit < '0' || digit > '9') {
            return false;
        }
        const auto value = static_cast<std::uint64_t>(digit - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - value) / 10U) {
            return false;
        }
        parsed = parsed * 10U + value;
    }
    inode = parsed;
    return true;
}

[[nodiscard]] inline bool read_namespace_inode(std::uint64_t& inode) noexcept {
    char name[64] {};
    const auto length = ::readlink("/proc/thread-self/ns/time", name, sizeof(name));
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(name)) {
        return false;
    }
    return parse_time_namespace_inode(std::string_view(name, static_cast<std::size_t>(length)), inode);
}

[[nodiscard]] inline bool parse_monotonic_offset(std::int64_t& seconds,
                                                 std::int64_t& nanoseconds) {
    const auto thread_id = ::syscall(SYS_gettid);
    if (thread_id <= 0) {
        return false;
    }
    char path[64] {};
    const auto path_size = std::snprintf(path, sizeof(path), "/proc/%ld/timens_offsets", thread_id);
    if (path_size < 0 || static_cast<std::size_t>(path_size) >= sizeof(path)) {
        return false;
    }
    std::ifstream input(path);
    if (!input) {
        return false;
    }
    std::string line;
    bool found_monotonic = false;
    while (std::getline(input, line)) {
        std::istringstream parser(line);
        std::string clock_name;
        std::int64_t parsed_seconds = 0;
        std::int64_t parsed_nanoseconds = 0;
        std::string trailing;
        if (!(parser >> clock_name)) {
            continue;
        }
        if (clock_name != "monotonic") {
            continue;
        }
        if (found_monotonic) {
            return false;
        }
        if (!(parser >> parsed_seconds >> parsed_nanoseconds) || (parser >> trailing)
            || parsed_nanoseconds < 0
            || parsed_nanoseconds >= static_cast<std::int64_t>(kNsPerSecond)) {
            return false;
        }
        seconds = parsed_seconds;
        nanoseconds = parsed_nanoseconds;
        found_monotonic = true;
    }
    return found_monotonic;
}

[[nodiscard]] inline Result<void> read_boot_uuid(std::array<std::uint8_t, 16>& uuid) {
    std::ifstream input("/proc/sys/kernel/random/boot_id");
    std::string value;
    if (!input || !std::getline(input, value)) {
        return TimeError::ClockUnavailable;
    }
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r')) {
        value.pop_back();
    }
    if (!parse_uuid(value, uuid)) {
        return TimeError::UnknownClockDomain;
    }
    return {};
}

[[nodiscard]] inline Result<std::int64_t> timespec_to_ns(const struct timespec& value) noexcept {
    if (value.tv_nsec < 0 || value.tv_nsec >= static_cast<decltype(value.tv_nsec)>(kNsPerSecond)) {
        return TimeError::ClockUnavailable;
    }
    const auto total = static_cast<int128_t>(value.tv_sec) * static_cast<int128_t>(kNsPerSecond)
                       + static_cast<int128_t>(value.tv_nsec);
    if (total < static_cast<int128_t>(std::numeric_limits<std::int64_t>::min())
        || total > static_cast<int128_t>(std::numeric_limits<std::int64_t>::max())) {
        return TimeError::TimestampOutOfRange;
    }
    return static_cast<std::int64_t>(total);
}

[[nodiscard]] inline Result<std::int64_t> read_clock_ns(clockid_t clock_id) noexcept {
    struct timespec value {};
    if (::clock_gettime(clock_id, &value) != 0) {
        return TimeError::ClockUnavailable;
    }
    return timespec_to_ns(value);
}
#endif

} // namespace detail

[[nodiscard]] inline Result<std::strong_ordering> checked_compare(const Timestamp& left,
                                                                   const Timestamp& right) noexcept {
    if (auto valid = detail::validate_timestamp(left); !valid) {
        return valid.error();
    }
    if (auto valid = detail::validate_timestamp(right); !valid) {
        return valid.error();
    }
    if (auto compatible = detail::compare_clock_identity(left.clock, right.clock); !compatible) {
        return compatible.error();
    }
    if (left.ns < right.ns) {
        return std::strong_ordering::less;
    }
    if (left.ns > right.ns) {
        return std::strong_ordering::greater;
    }
    return std::strong_ordering::equal;
}

[[nodiscard]] inline Result<std::int64_t> checked_difference(const Timestamp& left,
                                                             const Timestamp& right) noexcept {
    if (auto order = checked_compare(left, right); !order) {
        return order.error();
    }
    const auto difference = static_cast<detail::int128_t>(left.ns) - static_cast<detail::int128_t>(right.ns);
    if (difference < static_cast<detail::int128_t>(std::numeric_limits<std::int64_t>::min())
        || difference > static_cast<detail::int128_t>(std::numeric_limits<std::int64_t>::max())) {
        return TimeError::TimestampOverflow;
    }
    return static_cast<std::int64_t>(difference);
}

[[nodiscard]] inline Result<std::chrono::nanoseconds> checked_duration_since(const Timestamp& later,
                                                                              const Timestamp& earlier) noexcept {
    auto difference = checked_difference(later, earlier);
    if (!difference) {
        return difference.error();
    }
    if (difference.value() < 0) {
        return TimeError::NegativeDuration;
    }
    return std::chrono::nanoseconds(difference.value());
}

[[nodiscard]] inline Result<Timestamp> checked_add_nanoseconds(const Timestamp& timestamp,
                                                                std::int64_t nanoseconds) noexcept {
    if (auto valid = detail::validate_timestamp(timestamp); !valid) {
        return valid.error();
    }
    const auto total = static_cast<detail::int128_t>(timestamp.ns) + static_cast<detail::int128_t>(nanoseconds);
    if (total < static_cast<detail::int128_t>(std::numeric_limits<std::int64_t>::min())
        || total > static_cast<detail::int128_t>(std::numeric_limits<std::int64_t>::max())) {
        return TimeError::TimestampOverflow;
    }
    return Timestamp { timestamp.clock, static_cast<std::int64_t>(total) };
}

struct ClockDiscontinuity {
    ClockIdentity previous;
    ClockIdentity current;
};

class DeviceClock {
  public:
    DeviceClock(const DeviceClock&) = delete;
    DeviceClock& operator=(const DeviceClock&) = delete;
    DeviceClock(DeviceClock&& other) noexcept
        : source_id_(std::move(other.source_id_))
        , generation_(other.generation_.load(std::memory_order_relaxed)) {
    }
    DeviceClock& operator=(DeviceClock&& other) noexcept {
        if (this != &other) {
            source_id_ = std::move(other.source_id_);
            generation_.store(other.generation_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }

    [[nodiscard]] static Result<DeviceClock> create(const std::array<std::uint8_t, 16>& uuid,
                                                     std::uint64_t generation = 0U) {
        if (!detail::uuid_is_nonzero(uuid)) {
            return TimeError::InvalidClockSourceId;
        }
        std::array<std::uint8_t, 48> bytes {};
        for (std::size_t index = 0U; index < uuid.size(); ++index) {
            bytes[index] = uuid[index];
        }
        return DeviceClock(ClockSourceId(std::move(bytes)), generation);
    }

    [[nodiscard]] Result<ClockIdentity> identity() const {
        return ClockIdentity::create(ClockKind::Device, source_id_, generation_.load(std::memory_order_acquire));
    }

    [[nodiscard]] Result<Timestamp> stamp(std::int64_t nanoseconds) const {
        auto current_identity = identity();
        if (!current_identity) {
            return current_identity.error();
        }
        return Timestamp { std::move(current_identity).value(), nanoseconds };
    }

    [[nodiscard]] Result<ClockDiscontinuity> reset() {
        auto generation = generation_.load(std::memory_order_acquire);
        while (true) {
            if (generation == std::numeric_limits<std::uint64_t>::max()) {
                return TimeError::TimestampOverflow;
            }
            auto previous = ClockIdentity::create(ClockKind::Device, source_id_, generation);
            if (!previous) {
                return previous.error();
            }
            if (generation_.compare_exchange_weak(generation,
                                                  generation + 1U,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                auto current = ClockIdentity::create(ClockKind::Device, source_id_, generation + 1U);
                if (!current) {
                    return current.error();
                }
                return ClockDiscontinuity { std::move(previous).value(), std::move(current).value() };
            }
        }
    }

  private:
    DeviceClock(ClockSourceId source_id, std::uint64_t generation)
        : source_id_(std::move(source_id))
        , generation_(generation) {
    }

    ClockSourceId source_id_;
    std::atomic<std::uint64_t> generation_ { 0U };
};

class HostClock {
  public:
    HostClock(const HostClock&) = delete;
    HostClock& operator=(const HostClock&) = delete;
    HostClock(HostClock&& other) noexcept
        : monotonic_identity_(std::move(other.monotonic_identity_))
        , realtime_source_id_(std::move(other.realtime_source_id_))
        , namespace_device_(other.namespace_device_)
        , namespace_inode_(other.namespace_inode_)
        , realtime_generation_(other.realtime_generation_.load(std::memory_order_relaxed)) {
    }
    HostClock& operator=(HostClock&& other) noexcept {
        if (this != &other) {
            monotonic_identity_ = std::move(other.monotonic_identity_);
            realtime_source_id_ = std::move(other.realtime_source_id_);
            namespace_device_ = other.namespace_device_;
            namespace_inode_ = other.namespace_inode_;
            realtime_generation_.store(other.realtime_generation_.load(std::memory_order_relaxed),
                                       std::memory_order_relaxed);
        }
        return *this;
    }

    [[nodiscard]] static Result<HostClock> create() {
#if defined(__linux__)
        std::array<std::uint8_t, 16> boot_uuid {};
        if (auto boot_read = detail::read_boot_uuid(boot_uuid); !boot_read) {
            return boot_read.error();
        }
        struct stat before {};
        struct stat after {};
        if (!detail::read_namespace_stat(before)) {
            return TimeError::ClockUnavailable;
        }
        std::int64_t offset_seconds = 0;
        std::int64_t offset_nanoseconds = 0;
        if (!detail::parse_monotonic_offset(offset_seconds, offset_nanoseconds)) {
            return TimeError::UnknownClockDomain;
        }
        if (!detail::read_namespace_stat(after)) {
            return TimeError::ClockUnavailable;
        }
        if (!detail::same_namespace(before, after)) {
            return TimeError::TimeSourceReset;
        }

        std::array<std::uint8_t, 48> monotonic_bytes {};
        for (std::size_t index = 0U; index < boot_uuid.size(); ++index) {
            monotonic_bytes[index] = boot_uuid[index];
        }
        detail::store_u64_le(monotonic_bytes, 16U, static_cast<std::uint64_t>(before.st_dev));
        detail::store_u64_le(monotonic_bytes, 24U, static_cast<std::uint64_t>(before.st_ino));
        detail::store_i64_le(monotonic_bytes, 32U, offset_seconds);
        detail::store_i64_le(monotonic_bytes, 40U, offset_nanoseconds);
        std::array<std::uint8_t, 48> realtime_bytes {};
        for (std::size_t index = 0U; index < boot_uuid.size(); ++index) {
            realtime_bytes[index] = boot_uuid[index];
        }
        auto monotonic_identity = ClockIdentity::create(
            ClockKind::HostMonotonic, ClockSourceId(std::move(monotonic_bytes)), 0U);
        if (!monotonic_identity) {
            return monotonic_identity.error();
        }
        return HostClock(std::move(monotonic_identity).value(),
                         ClockSourceId(std::move(realtime_bytes)),
                         static_cast<std::uint64_t>(before.st_dev),
                         static_cast<std::uint64_t>(before.st_ino));
#else
        return TimeError::ClockUnavailable;
#endif
    }

    [[nodiscard]] const ClockIdentity& monotonic_identity() const noexcept {
        return monotonic_identity_;
    }

    [[nodiscard]] Result<ClockIdentity> realtime_identity() const {
        return ClockIdentity::create(ClockKind::SystemRealtime,
                                     realtime_source_id_,
                                     realtime_generation_.load(std::memory_order_acquire));
    }

    [[nodiscard]] Result<Timestamp> now_monotonic() const noexcept {
#if defined(__linux__)
        std::uint64_t before_inode = 0U;
        std::uint64_t after_inode = 0U;
        if (!detail::read_namespace_inode(before_inode)) {
            return TimeError::ClockUnavailable;
        }
        if (before_inode != namespace_inode_) {
            return TimeError::TimeSourceReset;
        }
        auto nanoseconds = detail::read_clock_ns(CLOCK_MONOTONIC);
        if (!nanoseconds) {
            return nanoseconds.error();
        }
        if (!detail::read_namespace_inode(after_inode)) {
            return TimeError::ClockUnavailable;
        }
        if (after_inode != namespace_inode_ || after_inode != before_inode) {
            return TimeError::TimeSourceReset;
        }
        return Timestamp { monotonic_identity_, nanoseconds.value() };
#else
        return TimeError::ClockUnavailable;
#endif
    }

    [[nodiscard]] Result<Timestamp> now_realtime() const noexcept {
#if defined(__linux__)
        const auto generation_before = realtime_generation_.load(std::memory_order_acquire);
        auto nanoseconds = detail::read_clock_ns(CLOCK_REALTIME);
        if (!nanoseconds) {
            return nanoseconds.error();
        }
        const auto generation_after = realtime_generation_.load(std::memory_order_acquire);
        if (generation_before != generation_after) {
            return TimeError::TimeSourceReset;
        }
        auto identity = ClockIdentity::create(ClockKind::SystemRealtime, realtime_source_id_, generation_after);
        if (!identity) {
            return identity.error();
        }
        return Timestamp { std::move(identity).value(), nanoseconds.value() };
#else
        return TimeError::ClockUnavailable;
#endif
    }

    /// Call only when an external observation confirms a realtime step. This
    /// does not claim to detect every wall-clock correction automatically.
    [[nodiscard]] Result<ClockDiscontinuity> confirm_realtime_discontinuity() noexcept {
        auto previous = realtime_identity();
        if (!previous) {
            return previous.error();
        }
        auto generation = realtime_generation_.load(std::memory_order_acquire);
        while (true) {
            if (previous.value().generation() != generation) {
                previous = realtime_identity();
                if (!previous) {
                    return previous.error();
                }
                generation = realtime_generation_.load(std::memory_order_acquire);
                continue;
            }
            if (generation == std::numeric_limits<std::uint64_t>::max()) {
                return TimeError::TimestampOverflow;
            }
            if (realtime_generation_.compare_exchange_weak(generation,
                                                           generation + 1U,
                                                           std::memory_order_acq_rel,
                                                           std::memory_order_acquire)) {
                auto current = realtime_identity();
                if (!current) {
                    return current.error();
                }
                return ClockDiscontinuity { std::move(previous).value(), std::move(current).value() };
            }
        }
    }

  private:
    HostClock(ClockIdentity monotonic_identity,
              ClockSourceId realtime_source_id,
              std::uint64_t namespace_device,
              std::uint64_t namespace_inode)
        : monotonic_identity_(std::move(monotonic_identity))
        , realtime_source_id_(std::move(realtime_source_id))
        , namespace_device_(namespace_device)
        , namespace_inode_(namespace_inode) {
    }

    ClockIdentity monotonic_identity_;
    ClockSourceId realtime_source_id_;
    std::uint64_t namespace_device_ { 0U };
    std::uint64_t namespace_inode_ { 0U };
    mutable std::atomic<std::uint64_t> realtime_generation_ { 0U };
};

class TimestampState {
  public:
    enum class Kind : std::uint8_t {
        Known,
        Unknown,
        Unmapped,
    };

    [[nodiscard]] static Result<TimestampState> known(Timestamp timestamp) {
        if (auto valid = detail::validate_timestamp(timestamp); !valid) {
            return valid.error();
        }
        return TimestampState(Kind::Known, std::move(timestamp));
    }

    [[nodiscard]] static TimestampState unknown() noexcept {
        return TimestampState(Kind::Unknown, {});
    }

    [[nodiscard]] static Result<TimestampState> unmapped(Timestamp timestamp) {
        if (auto valid = detail::validate_timestamp(timestamp); !valid) {
            return valid.error();
        }
        return TimestampState(Kind::Unmapped, std::move(timestamp));
    }

    /// Legacy numeric timestamps carry no trustworthy source or epoch.
    [[nodiscard]] static TimestampState from_legacy_timestamp_ns(std::uint64_t) noexcept {
        return unknown();
    }

    [[nodiscard]] Kind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] const std::optional<Timestamp>& timestamp() const noexcept {
        return timestamp_;
    }

  private:
    TimestampState(Kind kind, std::optional<Timestamp> timestamp)
        : kind_(kind)
        , timestamp_(std::move(timestamp)) {
    }

    Kind kind_ { Kind::Unknown };
    std::optional<Timestamp> timestamp_;
};

struct SensorTiming {
    std::optional<Timestamp> capture;
    Timestamp received;
    std::optional<std::uint64_t> capture_uncertainty_ns;
    std::optional<std::uint64_t> received_uncertainty_ns;

    [[nodiscard]] static Result<SensorTiming> create(std::optional<Timestamp> capture,
                                                      Timestamp received,
                                                      std::optional<std::uint64_t> capture_uncertainty_ns,
                                                      std::optional<std::uint64_t> received_uncertainty_ns) {
        if (auto valid = detail::validate_timestamp(received); !valid) {
            return valid.error();
        }
        if (capture.has_value()) {
            if (auto valid = detail::validate_timestamp(*capture); !valid) {
                return valid.error();
            }
        }
        if (!capture.has_value() && capture_uncertainty_ns.has_value()) {
            return TimeError::InvalidTimestamp;
        }
        return SensorTiming { std::move(capture),
                              std::move(received),
                              capture_uncertainty_ns,
                              received_uncertainty_ns };
    }

    [[nodiscard]] static Result<SensorTiming> legacy(Timestamp received,
                                                    std::optional<std::uint64_t> received_uncertainty_ns = {}) {
        return create({}, std::move(received), {}, received_uncertainty_ns);
    }
};

struct ClockMappingCalibration {
    ClockIdentity source;
    ClockIdentity target;
    std::int64_t source_anchor_ns { 0 };
    std::int64_t target_anchor_ns { 0 };
    std::uint64_t rate_numerator { 1U };
    std::uint64_t rate_denominator { 1U };
    std::int64_t valid_from_ns { 0 };
    std::int64_t valid_until_ns { 0 };
    std::array<std::uint8_t, 16> calibration_id {};
    Timestamp calibrated_at;
    Timestamp expires_at;
    std::optional<std::uint64_t> uncertainty_ns;
};

struct ConvertedTimestamp {
    Timestamp timestamp;
    std::uint64_t uncertainty_ns { 0U };
};

class ClockMapping {
  public:
    ClockMapping(const ClockMapping&) = delete;
    ClockMapping& operator=(const ClockMapping&) = delete;
    ClockMapping(ClockMapping&& other) noexcept
        : calibration_(std::move(other.calibration_))
        , invalidated_(other.invalidated_.load(std::memory_order_relaxed)) {
    }
    ClockMapping& operator=(ClockMapping&& other) noexcept {
        if (this != &other) {
            calibration_ = std::move(other.calibration_);
            invalidated_.store(other.invalidated_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        }
        return *this;
    }

    [[nodiscard]] static Result<ClockMapping> create(ClockMappingCalibration calibration) {
        if (!calibration.source.is_well_formed() || !calibration.target.is_well_formed()) {
            return TimeError::InvalidClockIdentity;
        }
        if (calibration.rate_numerator == 0U || calibration.rate_denominator == 0U) {
            return TimeError::InvalidConversionRate;
        }
        if (calibration.source == calibration.target
            && (calibration.source_anchor_ns != calibration.target_anchor_ns
                || calibration.rate_numerator != calibration.rate_denominator)) {
            return TimeError::ClockMappingInvalid;
        }
        if (calibration.valid_from_ns > calibration.valid_until_ns) {
            return TimeError::ClockMappingInvalid;
        }
        if (!calibration.uncertainty_ns.has_value()) {
            return TimeError::UnknownUncertainty;
        }
        bool has_calibration_id = false;
        for (const auto byte : calibration.calibration_id) {
            has_calibration_id = has_calibration_id || byte != 0U;
        }
        if (!has_calibration_id || !calibration.calibrated_at.clock.is_well_formed()
            || !calibration.expires_at.clock.is_well_formed()) {
            return TimeError::InvalidCalibrationEvidence;
        }
        if (calibration.calibrated_at.clock.kind() != ClockKind::HostMonotonic
            || calibration.expires_at.clock.kind() != ClockKind::HostMonotonic) {
            return TimeError::InvalidCalibrationEvidence;
        }
        if (auto valid = detail::validate_timestamp(calibration.calibrated_at); !valid) {
            return TimeError::InvalidCalibrationEvidence;
        }
        if (auto valid = detail::validate_timestamp(calibration.expires_at); !valid) {
            return TimeError::InvalidCalibrationEvidence;
        }
        if (auto same_clock = detail::compare_clock_identity(calibration.calibrated_at.clock,
                                                            calibration.expires_at.clock);
            !same_clock) {
            return TimeError::InvalidCalibrationEvidence;
        }
        if (calibration.calibrated_at.ns >= calibration.expires_at.ns) {
            return TimeError::InvalidCalibrationEvidence;
        }
        return ClockMapping(std::move(calibration));
    }

    [[nodiscard]] const ClockMappingCalibration& calibration() const noexcept {
        return calibration_;
    }

    [[nodiscard]] bool invalidate(const ClockDiscontinuity& discontinuity) noexcept {
        if (discontinuity.previous == calibration_.source || discontinuity.current == calibration_.source
            || discontinuity.previous == calibration_.target || discontinuity.current == calibration_.target) {
            invalidated_.store(true, std::memory_order_release);
            return true;
        }
        return false;
    }

    [[nodiscard]] Result<ConvertedTimestamp> convert(const Timestamp& input,
                                                     std::optional<std::uint64_t> input_uncertainty_ns,
                                                     const ClockIdentity& requested_target,
                                                     const Timestamp& now) const noexcept {
        if (invalidated_.load(std::memory_order_acquire)) {
            return TimeError::MappingInvalidated;
        }
        if (auto valid = detail::validate_timestamp(input); !valid) {
            return valid.error();
        }
        if (auto valid = detail::validate_timestamp(now); !valid) {
            return valid.error();
        }
        if (auto source_match = detail::compare_clock_identity(input.clock, calibration_.source);
            !source_match) {
            return source_match.error();
        }
        if (auto target_match = detail::compare_clock_identity(requested_target, calibration_.target);
            !target_match) {
            return target_match.error();
        }
        if (auto now_clock = detail::compare_clock_identity(now.clock, calibration_.calibrated_at.clock);
            !now_clock) {
            return now_clock.error();
        }
        if (now.ns < calibration_.calibrated_at.ns) {
            return TimeError::ClockMappingUnavailable;
        }
        if (now.ns >= calibration_.expires_at.ns) {
            return TimeError::ClockMappingExpired;
        }
        if (input.ns < calibration_.valid_from_ns || input.ns > calibration_.valid_until_ns) {
            return TimeError::TimestampOutOfRange;
        }
        if (!input_uncertainty_ns.has_value() || !calibration_.uncertainty_ns.has_value()) {
            return TimeError::UnknownUncertainty;
        }

        const auto source_delta = static_cast<detail::int128_t>(input.ns)
                                  - static_cast<detail::int128_t>(calibration_.source_anchor_ns);
        detail::int128_t scaled_delta = 0;
        if (__builtin_mul_overflow(source_delta,
                                   static_cast<detail::int128_t>(calibration_.rate_numerator),
                                   &scaled_delta)) {
            return TimeError::TimestampOverflow;
        }
        const auto denominator = static_cast<detail::int128_t>(calibration_.rate_denominator);
        const auto quotient = scaled_delta / denominator;
        const auto remainder = scaled_delta % denominator;
        detail::int128_t mapped = 0;
        if (__builtin_add_overflow(static_cast<detail::int128_t>(calibration_.target_anchor_ns),
                                   quotient,
                                   &mapped)
            || mapped < static_cast<detail::int128_t>(std::numeric_limits<std::int64_t>::min())
            || mapped > static_cast<detail::int128_t>(std::numeric_limits<std::int64_t>::max())) {
            return TimeError::TimestampOverflow;
        }

        const auto uncertainty_product = static_cast<detail::uint128_t>(*input_uncertainty_ns)
                                         * static_cast<detail::uint128_t>(calibration_.rate_numerator);
        auto scaled_uncertainty = uncertainty_product / static_cast<detail::uint128_t>(calibration_.rate_denominator);
        if (uncertainty_product % static_cast<detail::uint128_t>(calibration_.rate_denominator) != 0U) {
            ++scaled_uncertainty;
        }
        if (scaled_uncertainty > static_cast<detail::uint128_t>(std::numeric_limits<std::uint64_t>::max())) {
            return TimeError::TimestampOverflow;
        }
        auto output_uncertainty = static_cast<std::uint64_t>(scaled_uncertainty);
        if (output_uncertainty > std::numeric_limits<std::uint64_t>::max() - *calibration_.uncertainty_ns) {
            return TimeError::TimestampOverflow;
        }
        output_uncertainty += *calibration_.uncertainty_ns;
        if (remainder != 0) {
            if (output_uncertainty == std::numeric_limits<std::uint64_t>::max()) {
                return TimeError::TimestampOverflow;
            }
            ++output_uncertainty;
        }
        return ConvertedTimestamp { Timestamp { calibration_.target, static_cast<std::int64_t>(mapped) },
                                    output_uncertainty };
    }

  private:
    explicit ClockMapping(ClockMappingCalibration calibration)
        : calibration_(std::move(calibration)) {
    }

    ClockMappingCalibration calibration_;
    mutable std::atomic<bool> invalidated_ { false };
};

} // namespace rbt::time

#endif // RBT_CPP_TIME_HPP

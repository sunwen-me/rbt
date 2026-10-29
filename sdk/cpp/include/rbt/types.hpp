#ifndef RBT_CPP_TYPES_HPP
#define RBT_CPP_TYPES_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace rbt {

/// Whether a stream carries one fixed-size value or a header followed by a slice.
enum class MessageShape : std::uint8_t {
    Fixed,
    Slice,
};

/// The 128-bit (truncated) layout identity used by the local ABI contract.
using LayoutHash = std::array<std::uint8_t, 16>;

/// Formats a layout hash as exactly 32 lower-case hexadecimal characters.
inline std::string lowercase_hex(const LayoutHash& hash) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(hash.size() * 2U);
    for (const auto byte : hash) {
        result.push_back(digits[(byte >> 4U) & 0x0FU]);
        result.push_back(digits[byte & 0x0FU]);
    }
    return result;
}

/// Alias matching the terminology used in the Rust descriptor implementation.
inline std::string layout_hash_hex(const LayoutHash& hash) {
    return lowercase_hex(hash);
}

/// Complete metadata for one fixed or header-plus-slice message ABI.
///
/// The string views intentionally refer to generated/static metadata.  A
/// descriptor does not own or serialize a payload; it is only used for
/// control-plane preflight and diagnostics.
struct MessageTypeDescriptor {
    std::string_view package;
    std::string_view name;
    std::uint16_t abi_major { 0U };
    MessageShape shape { MessageShape::Fixed };
    std::uint32_t header_size { 0U };
    std::uint32_t header_align { 0U };
    std::uint32_t element_size { 0U };
    std::uint32_t element_align { 0U };
    LayoutHash layout_hash {};
    std::string_view fields;

    constexpr MessageTypeDescriptor() = default;

    constexpr MessageTypeDescriptor(std::string_view package_name,
                                    std::string_view type_name,
                                    std::uint16_t abi,
                                    MessageShape message_shape,
                                    std::uint32_t message_header_size,
                                    std::uint32_t message_header_align,
                                    std::uint32_t message_element_size,
                                    std::uint32_t message_element_align,
                                    LayoutHash hash,
                                    std::string_view field_signature = {})
        : package(package_name)
        , name(type_name)
        , abi_major(abi)
        , shape(message_shape)
        , header_size(message_header_size)
        , header_align(message_header_align)
        , element_size(message_element_size)
        , element_align(message_element_align)
        , layout_hash(hash)
        , fields(field_signature) {
    }

    constexpr MessageTypeDescriptor(std::string_view package_name,
                                    std::string_view type_name,
                                    std::uint16_t abi,
                                    MessageShape message_shape,
                                    std::string_view field_signature,
                                    std::uint32_t message_header_size,
                                    std::uint32_t message_header_align,
                                    std::uint32_t message_element_size,
                                    std::uint32_t message_element_align,
                                    LayoutHash hash)
        : MessageTypeDescriptor(package_name,
                                type_name,
                                abi,
                                message_shape,
                                message_header_size,
                                message_header_align,
                                message_element_size,
                                message_element_align,
                                hash,
                                field_signature) {
    }

    [[nodiscard]] constexpr bool is_fixed() const noexcept {
        return shape == MessageShape::Fixed;
    }

    [[nodiscard]] constexpr bool is_slice() const noexcept {
        return shape == MessageShape::Slice;
    }

    [[nodiscard]] std::string qualified_name() const {
        return std::string(package) + "/" + std::string(name) + "@" + std::to_string(abi_major);
    }

    [[nodiscard]] constexpr std::string_view package_name() const noexcept {
        return package;
    }

    [[nodiscard]] constexpr std::string_view namespace_name() const noexcept {
        return package;
    }
};

/// Generated fixed-message bindings associate a payload type with its ABI.
/// The primary template is intentionally incomplete: arbitrary C++ types may
/// not be used as stream payloads without generated metadata.
template <typename T>
struct FixedMessageBinding;

/// Generated slice-message bindings associate a header/element pair with its
/// ABI.  The primary template is intentionally incomplete for the same
/// reason as [`FixedMessageBinding`].
template <typename Header, typename Element>
struct SliceMessageBinding;

/// Equality includes all ABI metadata, including the canonical field signature.
[[nodiscard]] constexpr bool operator==(const MessageTypeDescriptor& lhs,
                                        const MessageTypeDescriptor& rhs) noexcept {
    return lhs.package == rhs.package && lhs.name == rhs.name && lhs.abi_major == rhs.abi_major
        && lhs.shape == rhs.shape && lhs.header_size == rhs.header_size && lhs.header_align == rhs.header_align
        && lhs.element_size == rhs.element_size && lhs.element_align == rhs.element_align
        && lhs.layout_hash == rhs.layout_hash && lhs.fields == rhs.fields;
}

[[nodiscard]] constexpr bool operator!=(const MessageTypeDescriptor& lhs,
                                        const MessageTypeDescriptor& rhs) noexcept {
    return !(lhs == rhs);
}

/// A data-plane policy with the same two profiles as the Rust SDK.
enum class StreamProfile : std::uint8_t {
    Sensor,
    Control,
};

/// Shared-memory capacity ceilings advertised by a stream provider.
///
/// Resource sizing is transport metadata.  It is deliberately separate from
/// the stream profile and from MessageTypeDescriptor so changing these values
/// never changes a payload layout hash.  The defaults preserve the original
/// rbt stream behavior.
struct StreamResourceConfig {
    /// Maximum concurrent loans held by one publisher.
    std::uint64_t max_loans { 2U };
    /// Maximum concurrent borrows advertised for one subscriber.
    std::uint64_t max_borrowed { 1U };

    constexpr StreamResourceConfig() = default;

    constexpr StreamResourceConfig(std::uint64_t loans,
                                   std::uint64_t borrowed) noexcept
        : max_loans(loans)
        , max_borrowed(borrowed) {
    }

    [[nodiscard]] static constexpr StreamResourceConfig defaults() noexcept {
        return StreamResourceConfig { 2U, 1U };
    }

    friend constexpr bool operator==(const StreamResourceConfig&, const StreamResourceConfig&) = default;
};

/// Frozen queue/history/overflow semantics derived from StreamProfile.
///
/// Applications may tune transport resource ceilings, but queue depth,
/// history, and overwrite behavior remain the profile contract.
struct StreamSemanticConfig {
    /// Maximum unread samples in one subscriber queue.
    std::uint64_t queue_depth { 0U };
    /// Samples retained for subscribers connecting after publication.
    std::uint64_t history_size { 0U };
    /// Whether a full queue replaces its oldest unread sample.
    bool safe_overflow { false };

    constexpr StreamSemanticConfig() = default;

    constexpr StreamSemanticConfig(std::uint64_t depth,
                                   std::uint64_t history,
                                   bool overflow) noexcept
        : queue_depth(depth)
        , history_size(history)
        , safe_overflow(overflow) {
    }

    [[nodiscard]] static constexpr StreamSemanticConfig for_profile(StreamProfile profile) noexcept {
        return StreamSemanticConfig { profile == StreamProfile::Sensor ? 1U : 4U,
                                      0U,
                                      profile == StreamProfile::Sensor };
    }

    friend constexpr bool operator==(const StreamSemanticConfig&, const StreamSemanticConfig&) = default;
};

/// Readiness protocol advertised by a stream provider. This is transport
/// metadata and never contributes to a MessageTypeDescriptor layout hash.
enum class ReadinessProtocol : std::uint8_t {
    None,
    SidecarV1,
};

[[nodiscard]] constexpr const char* to_string(ReadinessProtocol readiness) noexcept {
    switch (readiness) {
    case ReadinessProtocol::None:
        return "none";
    case ReadinessProtocol::SidecarV1:
        return "sidecar-v1";
    }
    return "none";
}

/// Transport/deployment capabilities advertised with a stream descriptor.
struct TransportFeatures {
    ReadinessProtocol readiness { ReadinessProtocol::None };

    [[nodiscard]] static constexpr TransportFeatures sidecar_v1() noexcept {
        return TransportFeatures { ReadinessProtocol::SidecarV1 };
    }

    friend constexpr bool operator==(const TransportFeatures&, const TransportFeatures&) = default;
};

/// A service name, message ABI descriptor, and stream policy advertised by a provider.
struct StreamDescriptor {
    std::string service_name;
    MessageTypeDescriptor message_type;
    StreamProfile profile;
    TransportFeatures transport;
    StreamResourceConfig resources;

    StreamDescriptor(std::string_view service,
                     MessageTypeDescriptor type,
                     StreamProfile stream_profile)
        : StreamDescriptor(service,
                           type,
                           stream_profile,
                           TransportFeatures::sidecar_v1(),
                           StreamResourceConfig::defaults()) {
    }

    StreamDescriptor(std::string_view service,
                     MessageTypeDescriptor type,
                     StreamProfile stream_profile,
                     TransportFeatures transport_features)
        : StreamDescriptor(service,
                           type,
                           stream_profile,
                           transport_features,
                           StreamResourceConfig::defaults()) {
    }

    StreamDescriptor(std::string_view service,
                     MessageTypeDescriptor type,
                     StreamProfile stream_profile,
                     StreamResourceConfig resource_config)
        : StreamDescriptor(service,
                           type,
                           stream_profile,
                           TransportFeatures::sidecar_v1(),
                           resource_config) {
    }

    StreamDescriptor(std::string_view service,
                     MessageTypeDescriptor type,
                     StreamProfile stream_profile,
                     TransportFeatures transport_features,
                     StreamResourceConfig resource_config)
        : service_name(service)
        , message_type(type)
        , profile(stream_profile)
        , transport(transport_features)
        , resources(resource_config) {
    }

    StreamDescriptor(std::string_view service,
                     MessageTypeDescriptor type,
                     StreamProfile stream_profile,
                     StreamResourceConfig resource_config,
                     TransportFeatures transport_features)
        : StreamDescriptor(service,
                           type,
                           stream_profile,
                           transport_features,
                           resource_config) {
    }

    [[nodiscard]] const MessageTypeDescriptor& type_descriptor() const noexcept {
        return message_type;
    }
};

[[nodiscard]] inline bool operator==(const StreamDescriptor& lhs, const StreamDescriptor& rhs) {
    return lhs.service_name == rhs.service_name && lhs.message_type == rhs.message_type && lhs.profile == rhs.profile
        && lhs.transport == rhs.transport && lhs.resources == rhs.resources;
}

[[nodiscard]] inline bool operator!=(const StreamDescriptor& lhs, const StreamDescriptor& rhs) {
    return !(lhs == rhs);
}

/// Compares two complete message descriptors, not only their short names.
[[nodiscard]] constexpr bool descriptors_equal(const MessageTypeDescriptor& expected,
                                               const MessageTypeDescriptor& actual) noexcept {
    return expected == actual;
}

/// Returns the configured subscriber queue depth for a profile.
[[nodiscard]] constexpr std::uint64_t queue_depth(StreamProfile profile) noexcept {
    return StreamSemanticConfig::for_profile(profile).queue_depth;
}

/// Returns whether a profile replaces an unread sample when its queue is full.
[[nodiscard]] constexpr bool safe_overflow(StreamProfile profile) noexcept {
    return StreamSemanticConfig::for_profile(profile).safe_overflow;
}

inline constexpr std::uint64_t kHistorySize = 0U;
inline constexpr std::uint64_t kMaxBorrowedSamples = 1U;
inline constexpr std::uint64_t kMaxLoanedSamples = 2U;

/// Backwards-compatible spelling for resource sizing.
///
/// Queue depth, history, and overflow are profile semantics and are not part
/// of this compatibility name.  Use `StreamResourceConfig` for new code.
using StreamLimits [[deprecated("use StreamResourceConfig")]] = StreamResourceConfig;

/// Backwards-compatible spelling for the semantic portion of a pressure
/// snapshot.  Resource ceilings live in `StreamPressure::resources`.
using StreamPressureConfig [[deprecated("use StreamSemanticConfig")]] = StreamSemanticConfig;

/// A typed pressure transition observed by a local stream wrapper.
enum class PressureEvent : std::uint8_t {
    QueueFull,
    UnableToDeliver,
    LoanPoolExhausted,
    BorrowLimitReached,
};

[[nodiscard]] constexpr const char* to_string(PressureEvent event) noexcept {
    switch (event) {
    case PressureEvent::QueueFull:
        return "QueueFull";
    case PressureEvent::UnableToDeliver:
        return "UnableToDeliver";
    case PressureEvent::LoanPoolExhausted:
        return "LoanPoolExhausted";
    case PressureEvent::BorrowLimitReached:
        return "BorrowLimitReached";
    }
    return "UnableToDeliver";
}

/// Publisher-side counters observed by the transport wrapper.
struct PublisherPressure {
    /// Calls that attempted to obtain a publisher loan.
    std::uint64_t loan_attempts { 0U };
    /// Loans successfully obtained from the transport.
    std::uint64_t loan_successes { 0U };
    /// Loans currently held by application code and not yet sent or dropped.
    std::uint64_t loans_in_use { 0U };
    /// Maximum simultaneous loans observed.
    std::uint64_t loans_high_water { 0U };
    /// Loan attempts rejected by the configured loan limit or allocation pool.
    std::uint64_t loan_failures { 0U };
    /// Bytes represented by the most recent successful loan.
    std::uint64_t payload_bytes_last { 0U };
    /// Largest payload size observed in a successful loan.
    std::uint64_t payload_bytes_max_seen { 0U };
    /// Cumulative bytes represented by successful loans.
    std::uint64_t total_loaned { 0U };
    /// Calls that attempted to send a previously loaned sample.
    std::uint64_t send_attempts { 0U };
    /// Send calls that completed without a transport error.
    std::uint64_t send_successes { 0U };
    /// Send calls that returned a transport error.
    std::uint64_t send_failures { 0U };
    /// Send failures caused by at least one full subscriber queue.
    std::uint64_t queue_full { 0U };
    /// Send failures not classified as a full queue.
    std::uint64_t unable_to_deliver { 0U };
    /// Readiness notifications that failed after a payload send attempt.
    std::uint64_t readiness_notify_failures { 0U };
    /// Last aggregate subscriber count observed from iceoryx2.
    std::uint64_t subscriber_count { 0U };
    /// Generic wrappers cannot observe safe-overflow counts.
    std::optional<std::uint64_t> estimated_dropped_samples;

    friend constexpr bool operator==(const PublisherPressure&, const PublisherPressure&) = default;
};

/// Subscriber-side counters self-reported by one local stream wrapper.
struct SubscriberPressure {
    /// Calls that attempted to receive a sample.
    std::uint64_t receive_attempts { 0U };
    /// Samples successfully received by this wrapper.
    std::uint64_t received_samples { 0U };
    /// Samples currently retained as active borrows by application code.
    std::uint64_t borrowed_samples { 0U };
    /// Maximum simultaneous borrows observed.
    std::uint64_t borrowed_high_water { 0U };
    /// Receive attempts rejected at the configured borrow limit.
    std::uint64_t borrow_failures { 0U };
    /// Number of completed borrow lifetimes with a measured duration.
    std::uint64_t borrow_duration_count { 0U };
    /// Cumulative duration of completed borrows in nanoseconds.
    std::uint64_t borrow_duration_total_ns { 0U };
    /// Longest completed borrow duration in nanoseconds.
    std::uint64_t borrow_duration_max_ns { 0U };
    /// Age in nanoseconds of the oldest currently active borrow, if any.
    std::optional<std::uint64_t> oldest_borrow_age_ns;
    /// Generic wrappers cannot infer sequence gaps.
    std::optional<std::uint64_t> estimated_dropped_samples;

    friend constexpr bool operator==(const SubscriberPressure&, const SubscriberPressure&) = default;
};

/// A point-in-time pressure snapshot for one local publisher or subscriber.
struct StreamPressure {
    std::string service_name;
    StreamProfile profile { StreamProfile::Sensor };
    StreamSemanticConfig semantic;
    StreamResourceConfig resources;
    std::optional<PublisherPressure> publisher;
    std::optional<SubscriberPressure> subscriber;
    std::optional<PressureEvent> last_pressure_event;
    std::uint64_t snapshot_timestamp_unix_ms { 0U };

    /// Transport receiver port identity associated with QueueFull, when supplied.
    std::optional<std::string> last_pressure_subscriber;

    friend bool operator==(const StreamPressure& lhs, const StreamPressure& rhs) {
        return lhs.service_name == rhs.service_name && lhs.profile == rhs.profile
            && lhs.semantic == rhs.semantic && lhs.resources == rhs.resources
            && lhs.publisher == rhs.publisher && lhs.subscriber == rhs.subscriber
            && lhs.last_pressure_event == rhs.last_pressure_event
            && lhs.last_pressure_subscriber == rhs.last_pressure_subscriber
            && lhs.snapshot_timestamp_unix_ms == rhs.snapshot_timestamp_unix_ms;
    }

    friend bool operator!=(const StreamPressure& lhs, const StreamPressure& rhs) {
        return !(lhs == rhs);
    }
};

/// Returns snapshots for live C++ stream wrappers in this process.
///
/// The definition is provided by `rbt/stream.hpp`, where the transport-aware
/// registry lives.  Keeping this declaration here makes the public pressure
/// types available alongside the rest of the descriptor API.
[[nodiscard]] std::vector<StreamPressure> local_pressure_snapshots();

/// Publicly observable stream failures.
enum class StreamError : std::uint8_t {
    AbiMismatch,
    MissingTransportFeature,
    InvalidPayloadLength,
    InvalidStreamLimit,
    InvalidResourceConfig,
    IncompatibleResourceConfig,
    QueueFull,
    UnableToDeliver,
    LoanPoolExhausted,
    BorrowLimitReached,
    Transport,
    /// The readiness sidecar could not be opened or created.
    ReadinessOpenOrCreate,
    /// An existing readiness sidecar could not be opened.
    ReadinessOpen,
    /// The readiness sidecar notifier could not be created.
    ReadinessNotifier,
    /// The readiness sidecar listener could not be created.
    ReadinessListener,
    /// A completed payload send could not notify the readiness sidecar.
    ReadinessNotify,
};

[[nodiscard]] constexpr const char* to_string(StreamError error) noexcept {
    switch (error) {
    case StreamError::AbiMismatch:
        return "AbiMismatch";
    case StreamError::MissingTransportFeature:
        return "MissingTransportFeature";
    case StreamError::InvalidPayloadLength:
        return "InvalidPayloadLength";
    case StreamError::InvalidStreamLimit:
        return "InvalidStreamLimit";
    case StreamError::InvalidResourceConfig:
        return "InvalidResourceConfig";
    case StreamError::IncompatibleResourceConfig:
        return "IncompatibleResourceConfig";
    case StreamError::QueueFull:
        return "QueueFull";
    case StreamError::UnableToDeliver:
        return "UnableToDeliver";
    case StreamError::LoanPoolExhausted:
        return "LoanPoolExhausted";
    case StreamError::BorrowLimitReached:
        return "BorrowLimitReached";
    case StreamError::Transport:
        return "Transport";
    case StreamError::ReadinessOpenOrCreate:
        return "ReadinessOpenOrCreate";
    case StreamError::ReadinessOpen:
        return "ReadinessOpen";
    case StreamError::ReadinessNotifier:
        return "ReadinessNotifier";
    case StreamError::ReadinessListener:
        return "ReadinessListener";
    case StreamError::ReadinessNotify:
        return "ReadinessNotify";
    }
    return "Transport";
}

/// A small C++20 result replacement for APIs that cannot use std::expected.
template <typename T>
class Result {
  public:
    Result(T value)
        : value_(std::move(value)) {
    }

    Result(StreamError error)
        : value_(error) {
    }

    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;
    Result(Result&&) noexcept = default;
    Result& operator=(Result&&) noexcept = default;
    ~Result() = default;

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(value_);
    }

    [[nodiscard]] bool is_ok() const noexcept {
        return has_value();
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

    [[nodiscard]] StreamError error() const {
        return std::get<StreamError>(value_);
    }

  private:
    std::variant<T, StreamError> value_;
};

/// Preflight a provider/consumer binding before any transport object is opened.
[[nodiscard]] inline std::optional<StreamError> preflight(const StreamDescriptor& stream,
                                                          const MessageTypeDescriptor& expected) {
    if (!descriptors_equal(expected, stream.message_type)) {
        return StreamError::AbiMismatch;
    }
    if (stream.transport.readiness != ReadinessProtocol::SidecarV1) {
        return StreamError::MissingTransportFeature;
    }
    return std::nullopt;
}

/// Variant that also validates the policy selected for the endpoint.
[[nodiscard]] inline std::optional<StreamError> preflight(const StreamDescriptor& stream,
                                                          const MessageTypeDescriptor& expected,
                                                          StreamProfile expected_profile) {
    if (stream.profile != expected_profile) {
        return StreamError::AbiMismatch;
    }
    return preflight(stream, expected);
}

} // namespace rbt

#endif // RBT_CPP_TYPES_HPP

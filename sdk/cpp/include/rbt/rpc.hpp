#ifndef RBT_CPP_RPC_HPP
#define RBT_CPP_RPC_HPP

// Typed, bounded local request/response RPC for maintained iceoryx2 0.10.0.
//
// Request/response payloads stay in the native iceoryx2 loans.  The event
// services below are coalesced readiness sidecars only: activation ids and
// counts carry no request id, response id, or payload bytes.  A positive
// activation count prompts one native readiness dispatch; it never maps to a
// payload count.  Native ActiveRequest and PendingResponse handles preserve
// request/client correlation.

#include <iox2/iceoryx2.hpp>

#include <rbt/types.hpp>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace rbt {

enum class RpcReadinessProtocol : std::uint8_t {
    None,
    RpcSidecarV1,
};

[[nodiscard]] constexpr const char* to_string(RpcReadinessProtocol value) noexcept {
    return value == RpcReadinessProtocol::RpcSidecarV1 ? "rpc-sidecar-v1" : "none";
}

struct RpcTransportFeatures {
    RpcReadinessProtocol readiness { RpcReadinessProtocol::RpcSidecarV1 };

    [[nodiscard]] static constexpr RpcTransportFeatures rpc_sidecar_v1() noexcept {
        return RpcTransportFeatures { RpcReadinessProtocol::RpcSidecarV1 };
    }

    friend constexpr bool operator==(const RpcTransportFeatures& lhs,
                                     const RpcTransportFeatures& rhs) noexcept {
        return lhs.readiness == rhs.readiness;
    }
};

struct RpcResourceConfig {
    std::size_t max_in_flight { 4U };
    std::size_t max_clients { 16U };
    std::size_t max_request_loans { 2U };
    std::size_t response_buffer_size { 1U };
    std::size_t max_borrowed_responses { 1U };
    std::size_t max_response_loans { 1U };
    std::size_t max_request_slice_len { 1U };
    std::size_t max_response_slice_len { 1U };

    [[nodiscard]] static constexpr RpcResourceConfig defaults() noexcept {
        return RpcResourceConfig {};
    }

    [[nodiscard]] constexpr RpcResourceConfig with_max_in_flight(
        std::size_t value) const noexcept {
        auto copy = *this;
        copy.max_in_flight = value;
        return copy;
    }
    [[nodiscard]] constexpr RpcResourceConfig with_max_clients(
        std::size_t value) const noexcept {
        auto copy = *this;
        copy.max_clients = value;
        return copy;
    }
    [[nodiscard]] constexpr RpcResourceConfig with_max_request_loans(
        std::size_t value) const noexcept {
        auto copy = *this;
        copy.max_request_loans = value;
        return copy;
    }
    [[nodiscard]] constexpr RpcResourceConfig with_response_buffer_size(
        std::size_t value) const noexcept {
        auto copy = *this;
        copy.response_buffer_size = value;
        return copy;
    }
    [[nodiscard]] constexpr RpcResourceConfig with_max_borrowed_responses(
        std::size_t value) const noexcept {
        auto copy = *this;
        copy.max_borrowed_responses = value;
        return copy;
    }
    [[nodiscard]] constexpr RpcResourceConfig with_max_response_loans(
        std::size_t value) const noexcept {
        auto copy = *this;
        copy.max_response_loans = value;
        return copy;
    }
    [[nodiscard]] constexpr RpcResourceConfig with_max_request_slice_len(
        std::size_t value) const noexcept {
        auto copy = *this;
        copy.max_request_slice_len = value;
        return copy;
    }
    [[nodiscard]] constexpr RpcResourceConfig with_max_response_slice_len(
        std::size_t value) const noexcept {
        auto copy = *this;
        copy.max_response_slice_len = value;
        return copy;
    }

    friend constexpr bool operator==(const RpcResourceConfig& lhs,
                                     const RpcResourceConfig& rhs) noexcept {
        return lhs.max_in_flight == rhs.max_in_flight && lhs.max_clients == rhs.max_clients
            && lhs.max_request_loans == rhs.max_request_loans
            && lhs.response_buffer_size == rhs.response_buffer_size
            && lhs.max_borrowed_responses == rhs.max_borrowed_responses
            && lhs.max_response_loans == rhs.max_response_loans
            && lhs.max_request_slice_len == rhs.max_request_slice_len
            && lhs.max_response_slice_len == rhs.max_response_slice_len;
    }
};

/// Aggregate client-side RPC counters.  These counters are intentionally
/// payload-free and are sampled only when a component snapshot is built.
struct RpcClientMetrics {
    std::uint64_t calls_attempted { 0U };
    std::uint64_t calls_started { 0U };
    std::uint64_t calls_completed { 0U };
    std::uint64_t calls_failed { 0U };
    std::uint64_t calls_timed_out { 0U };
    std::uint64_t calls_cancelled { 0U };
    std::uint64_t calls_activation_expired { 0U };
    std::uint64_t in_flight_current { 0U };
    std::uint64_t in_flight_high_water { 0U };
    std::uint64_t response_guards_current { 0U };
    std::uint64_t response_guards_high_water { 0U };
    std::uint64_t capacity_rejections { 0U };
    std::uint64_t server_capacity_rejections { 0U };
    std::uint64_t server_unavailable { 0U };
    std::uint64_t request_loan_failures { 0U };
    std::uint64_t response_receive_failures { 0U };
    std::uint64_t call_duration_count { 0U };
    std::uint64_t call_duration_total_ns { 0U };
    std::uint64_t call_duration_max_ns { 0U };

    friend bool operator==(const RpcClientMetrics&, const RpcClientMetrics&) = default;
};

/// Aggregate server-side RPC counters.  No payload or request identifier is
/// retained in these counters.
struct RpcServerMetrics {
    std::uint64_t requests_received { 0U };
    std::uint64_t requests_current { 0U };
    std::uint64_t requests_high_water { 0U };
    std::uint64_t responses_attempted { 0U };
    std::uint64_t responses_sent { 0U };
    std::uint64_t responses_failed { 0U };
    std::uint64_t responses_abandoned { 0U };
    std::uint64_t request_borrow_failures { 0U };
    std::uint64_t response_loan_failures { 0U };

    friend bool operator==(const RpcServerMetrics&, const RpcServerMetrics&) = default;
};

/// Low-rate process-local RPC pressure snapshot used by the component
/// control plane.  Exactly one of `client` and `server` is populated for a
/// live endpoint wrapper.
struct RpcPressure {
    std::string service_name;
    std::optional<std::string> capability_instance;
    RpcResourceConfig resources {};
    std::optional<std::uint64_t> client_activation;
    std::optional<RpcClientMetrics> client;
    std::optional<RpcServerMetrics> server;
    std::uint64_t snapshot_timestamp_unix_ms { 0U };

    [[nodiscard]] bool is_client() const noexcept {
        return client.has_value() && !server.has_value();
    }
    [[nodiscard]] bool is_server() const noexcept {
        return server.has_value() && !client.has_value();
    }
    [[nodiscard]] bool has_exactly_one_side() const noexcept {
        return client.has_value() != server.has_value();
    }

    friend bool operator==(const RpcPressure&, const RpcPressure&) = default;
};

/// Weak source interface for runtime snapshot collection.
class RpcPressureSnapshotSource {
  public:
    virtual ~RpcPressureSnapshotSource() = default;
    [[nodiscard]] virtual RpcPressure pressure_snapshot() const = 0;
};

namespace detail {

inline std::mutex& rpc_pressure_registry_mutex() {
    static std::mutex mutex;
    return mutex;
}

inline std::vector<std::weak_ptr<RpcPressureSnapshotSource>>& rpc_pressure_registry() {
    static std::vector<std::weak_ptr<RpcPressureSnapshotSource>> registry;
    return registry;
}

inline std::uint64_t rpc_unix_timestamp_ms() noexcept {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

} // namespace detail

template <typename T>
void register_rpc_pressure_source(const std::shared_ptr<T>& source) {
    static_assert(std::is_base_of<RpcPressureSnapshotSource, T>::value,
                  "RPC pressure source must implement RpcPressureSnapshotSource");
    std::lock_guard<std::mutex> lock(detail::rpc_pressure_registry_mutex());
    std::shared_ptr<RpcPressureSnapshotSource> erased = source;
    detail::rpc_pressure_registry().emplace_back(std::move(erased));
}

[[nodiscard]] inline std::vector<RpcPressure> local_rpc_pressure_snapshots() {
    std::vector<RpcPressure> snapshots;
    std::lock_guard<std::mutex> lock(detail::rpc_pressure_registry_mutex());
    auto& registry = detail::rpc_pressure_registry();
    registry.erase(
        std::remove_if(registry.begin(),
                       registry.end(),
                       [&snapshots](const std::weak_ptr<RpcPressureSnapshotSource>& weak) {
                           auto source = weak.lock();
                           if (!source) {
                               return true;
                           }
                           snapshots.push_back(source->pressure_snapshot());
                           return false;
                       }),
        registry.end());
    return snapshots;
}

struct RpcDescriptor {
    std::string service_name;
    MessageTypeDescriptor request_type;
    MessageTypeDescriptor response_type;
    RpcTransportFeatures transport {};
    RpcResourceConfig resources {};

    RpcDescriptor() = default;

    RpcDescriptor(std::string_view service,
                  MessageTypeDescriptor request,
                  MessageTypeDescriptor response,
                  RpcTransportFeatures features = RpcTransportFeatures::rpc_sidecar_v1(),
                  RpcResourceConfig limits = RpcResourceConfig {})
        : service_name(service)
        , request_type(request)
        , response_type(response)
        , transport(features)
        , resources(limits) {
    }

    [[nodiscard]] static RpcDescriptor new_with_transport(std::string_view service,
                                                           MessageTypeDescriptor request,
                                                           MessageTypeDescriptor response,
                                                           RpcTransportFeatures features) {
        return RpcDescriptor(service, request, response, features, RpcResourceConfig {});
    }

    [[nodiscard]] static RpcDescriptor new_with_resources(std::string_view service,
                                                           MessageTypeDescriptor request,
                                                           MessageTypeDescriptor response,
                                                           RpcResourceConfig limits) {
        return RpcDescriptor(service,
                             request,
                             response,
                             RpcTransportFeatures::rpc_sidecar_v1(),
                             limits);
    }

    [[nodiscard]] static RpcDescriptor new_with_transport_and_resources(
        std::string_view service,
        MessageTypeDescriptor request,
        MessageTypeDescriptor response,
        RpcTransportFeatures features,
        RpcResourceConfig limits) {
        return RpcDescriptor(service, request, response, features, limits);
    }

    template <typename Request, typename Response>
    [[nodiscard]] static RpcDescriptor for_fixed(std::string_view service);

    template <typename RequestHeader,
              typename RequestElement,
              typename ResponseHeader,
              typename ResponseElement>
    [[nodiscard]] static RpcDescriptor for_slices(std::string_view service);

    template <typename Request,
              typename ResponseHeader,
              typename ResponseElement>
    [[nodiscard]] static RpcDescriptor for_fixed_slice(std::string_view service);

    template <typename RequestHeader,
              typename RequestElement,
              typename Response>
    [[nodiscard]] static RpcDescriptor for_slice_fixed(std::string_view service);

    template <typename RequestHeader,
              typename RequestElement,
              typename ResponseHeader,
              typename ResponseElement>
    [[nodiscard]] static RpcDescriptor for_slice(std::string_view service);

    template <typename Request, typename Response>
    [[nodiscard]] static RpcDescriptor for_fixed_with_resources(
        std::string_view service,
        RpcResourceConfig resources);

    template <typename Request,
              typename ResponseHeader,
              typename ResponseElement>
    [[nodiscard]] static RpcDescriptor for_fixed_slice_with_resources(
        std::string_view service,
        RpcResourceConfig resources);

    template <typename RequestHeader,
              typename RequestElement,
              typename Response>
    [[nodiscard]] static RpcDescriptor for_slice_fixed_with_resources(
        std::string_view service,
        RpcResourceConfig resources);

    template <typename RequestHeader,
              typename RequestElement,
              typename ResponseHeader,
              typename ResponseElement>
    [[nodiscard]] static RpcDescriptor for_slice_with_resources(
        std::string_view service,
        RpcResourceConfig resources);

    friend bool operator==(const RpcDescriptor& lhs, const RpcDescriptor& rhs) {
        return lhs.service_name == rhs.service_name && lhs.request_type == rhs.request_type
            && lhs.response_type == rhs.response_type && lhs.transport == rhs.transport
            && lhs.resources == rhs.resources;
    }
};

enum class RpcError : std::uint8_t {
    AbiMismatch,
    MissingTransportFeature,
    InvalidResourceConfig,
    ResourceOverflow,
    ServiceName,
    Node,
    EventOpenOrCreate,
    EventOpen,
    Listener,
    Notifier,
    RequestResponseOpenOrCreate,
    RequestResponseOpen,
    ClientCreate,
    ServerCreate,
    ClientCapacityReached,
    ServerCapacityReached,
    ResponseCapacityReached,
    ServerUnavailable,
    LoanPoolExhausted,
    BorrowLimitReached,
    DeadlineExceeded,
    Cancelled,
    ActivationExpired,
    TransportFailure,
    InvalidPayloadLength,
};

[[nodiscard]] constexpr const char* to_string(RpcError error) noexcept {
    switch (error) {
    case RpcError::AbiMismatch:
        return "AbiMismatch";
    case RpcError::MissingTransportFeature:
        return "MissingTransportFeature";
    case RpcError::InvalidResourceConfig:
        return "InvalidResourceConfig";
    case RpcError::ResourceOverflow:
        return "ResourceOverflow";
    case RpcError::ServiceName:
        return "ServiceName";
    case RpcError::Node:
        return "Node";
    case RpcError::EventOpenOrCreate:
        return "EventOpenOrCreate";
    case RpcError::EventOpen:
        return "EventOpen";
    case RpcError::Listener:
        return "Listener";
    case RpcError::Notifier:
        return "Notifier";
    case RpcError::RequestResponseOpenOrCreate:
        return "RequestResponseOpenOrCreate";
    case RpcError::RequestResponseOpen:
        return "RequestResponseOpen";
    case RpcError::ClientCreate:
        return "ClientCreate";
    case RpcError::ServerCreate:
        return "ServerCreate";
    case RpcError::ClientCapacityReached:
        return "ClientCapacityReached";
    case RpcError::ServerCapacityReached:
        return "ServerCapacityReached";
    case RpcError::ResponseCapacityReached:
        return "ResponseCapacityReached";
    case RpcError::ServerUnavailable:
        return "ServerUnavailable";
    case RpcError::LoanPoolExhausted:
        return "LoanPoolExhausted";
    case RpcError::BorrowLimitReached:
        return "BorrowLimitReached";
    case RpcError::DeadlineExceeded:
        return "DeadlineExceeded";
    case RpcError::Cancelled:
        return "Cancelled";
    case RpcError::ActivationExpired:
        return "ActivationExpired";
    case RpcError::TransportFailure:
        return "TransportFailure";
    case RpcError::InvalidPayloadLength:
        return "InvalidPayloadLength";
    }
    return "TransportFailure";
}

template <typename T>
class RpcResult {
  public:
    explicit RpcResult(T value)
        : value_(std::move(value)) {
    }
    explicit RpcResult(RpcError error)
        : value_(error) {
    }

    RpcResult(const RpcResult&) = delete;
    RpcResult& operator=(const RpcResult&) = delete;
    RpcResult(RpcResult&&) noexcept = default;
    RpcResult& operator=(RpcResult&&) noexcept = default;

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
    [[nodiscard]] RpcError error() const noexcept {
        return std::get<RpcError>(value_);
    }

  private:
    std::variant<T, RpcError> value_;
};

template <>
class RpcResult<void> {
  public:
    RpcResult() noexcept = default;
    explicit RpcResult(RpcError error) noexcept
        : error_(error) {
    }
    [[nodiscard]] bool has_value() const noexcept {
        return !error_.has_value();
    }
    [[nodiscard]] bool is_ok() const noexcept {
        return has_value();
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }
    void value() const noexcept {
    }
    [[nodiscard]] RpcError error() const noexcept {
        return error_.value_or(RpcError::TransportFailure);
    }

  private:
    std::optional<RpcError> error_;
};

using RpcResultVoid = RpcResult<void>;

namespace detail {

inline constexpr iox2::ServiceType RpcIpcService = iox2::ServiceType::Ipc;
using NativeNode = iox2::Node<RpcIpcService>;

template <typename Q, typename R, typename QH = void, typename RH = void>
using NativeClient = iox2::Client<RpcIpcService, Q, QH, R, RH>;
template <typename Q, typename R, typename QH = void, typename RH = void>
using NativeServer = iox2::Server<RpcIpcService, Q, QH, R, RH>;
template <typename Q, typename R, typename QH = void, typename RH = void>
using NativeRequest = iox2::RequestMut<RpcIpcService, Q, QH, R, RH>;
template <typename Q, typename R, typename QH = void, typename RH = void>
using NativePending = iox2::PendingResponse<RpcIpcService, Q, QH, R, RH>;
template <typename R, typename RH = void>
using NativeResponse = iox2::Response<RpcIpcService, R, RH>;
template <typename Q, typename R, typename QH = void, typename RH = void>
using NativeActive = iox2::ActiveRequest<RpcIpcService, Q, QH, R, RH>;
template <typename R, typename RH = void>
using NativeResponseLoan = iox2::ResponseMut<RpcIpcService, R, RH>;
template <typename Q, typename R, typename QH = void, typename RH = void>
using NativeFactory =
    iox2::PortFactoryRequestResponse<RpcIpcService, Q, QH, R, RH>;
using NativeListener = iox2::Listener<RpcIpcService>;
using NativeNotifier = iox2::Notifier<RpcIpcService>;
using NativeEventFactory = iox2::PortFactoryEvent<RpcIpcService>;

inline RpcResult<bool> try_consume_activations(NativeListener& listener) {
    bool activated = false;
    // Drain the available batch, but reduce it to one readiness indication.
    auto result = listener.try_wait([&](iox2::EventActivation activation) {
        activated = activated || activation.count() > 0U;
    });
    if (!result.has_value()) {
        return RpcResult<bool>(RpcError::TransportFailure);
    }
    return RpcResult<bool>(activated || result.value() > 0U);
}

inline constexpr std::size_t kRpcEventId = 1U;
inline constexpr std::chrono::milliseconds kRpcWaitInterval { 100 };
inline constexpr const char* kRequestReadySuffix = "/request-ready";
inline constexpr const char* kResponseReadySuffix = "/response-ready";
inline constexpr const char* kClientCancelSuffix = "/client-cancel";
inline constexpr const char* kServerCancelSuffix = "/server-cancel";

struct RpcResourceBounds {
    std::size_t response_listener_capacity { 0U };
    std::size_t event_max_nodes { 0U };
};

inline RpcResult<RpcResourceBounds> validate_rpc_resources(const RpcResourceConfig& resources) {
    const std::pair<const char*, std::size_t> values[] = {
        { "max_in_flight", resources.max_in_flight },
        { "max_clients", resources.max_clients },
        { "max_request_loans", resources.max_request_loans },
        { "response_buffer_size", resources.response_buffer_size },
        { "max_borrowed_responses", resources.max_borrowed_responses },
        { "max_response_loans", resources.max_response_loans },
        { "max_request_slice_len", resources.max_request_slice_len },
        { "max_response_slice_len", resources.max_response_slice_len },
    };
    for (const auto& value : values) {
        if (value.second == 0U) {
            return RpcResult<RpcResourceBounds>(RpcError::InvalidResourceConfig);
        }
    }
    if (resources.max_clients > std::numeric_limits<std::size_t>::max()
                                   / resources.max_in_flight) {
        return RpcResult<RpcResourceBounds>(RpcError::ResourceOverflow);
    }
    const auto response_listener_capacity =
        resources.max_clients * resources.max_in_flight;
    if (resources.max_clients == std::numeric_limits<std::size_t>::max()) {
        return RpcResult<RpcResourceBounds>(RpcError::ResourceOverflow);
    }
    return RpcResult<RpcResourceBounds>(
        RpcResourceBounds { response_listener_capacity, resources.max_clients + 1U });
}

inline RpcResult<iox2::ServiceName> make_service_name(std::string_view value) {
    std::string owned(value);
    auto result = iox2::ServiceName::create(owned.c_str());
    if (!result.has_value()) {
        return RpcResult<iox2::ServiceName>(RpcError::ServiceName);
    }
    return RpcResult<iox2::ServiceName>(std::move(result).value());
}

inline RpcResult<iox2::ServiceName> make_sidecar_name(const RpcDescriptor& descriptor,
                                                      const char* suffix) {
    return make_service_name(descriptor.service_name + suffix);
}

template <typename Builder>
auto configure_rr_builder(Builder builder, const RpcResourceConfig& resources) {
    std::move(builder).enable_safe_overflow_for_requests(false);
    std::move(builder).enable_safe_overflow_for_responses(false);
    std::move(builder).enable_fire_and_forget_requests(false);
    std::move(builder).max_active_requests_per_client(
        static_cast<std::uint64_t>(resources.max_in_flight));
    std::move(builder).max_loaned_requests(
        static_cast<std::uint64_t>(resources.max_request_loans));
    std::move(builder).max_response_buffer_size(
        static_cast<std::uint64_t>(resources.response_buffer_size));
    std::move(builder).max_borrowed_responses_per_pending_response(
        static_cast<std::uint64_t>(resources.max_borrowed_responses));
    std::move(builder).max_servers(1U);
    std::move(builder).max_clients(
        static_cast<std::uint64_t>(resources.max_clients));
    return builder;
}

template <typename Q, typename R, typename QH, typename RH>
auto make_rr_builder(NativeNode& node,
                     const iox2::ServiceName& name,
                     const RpcResourceConfig& resources) {
    auto builder = node.service_builder(name).template request_response<Q, R>();
    if constexpr (!std::is_same<QH, void>::value) {
        auto typed = std::move(builder).template request_user_header<QH>();
        if constexpr (!std::is_same<RH, void>::value) {
            auto typed_both = std::move(typed).template response_user_header<RH>();
            return configure_rr_builder(std::move(typed_both), resources);
        } else {
            return configure_rr_builder(std::move(typed), resources);
        }
    } else if constexpr (!std::is_same<RH, void>::value) {
        auto typed = std::move(builder).template response_user_header<RH>();
        return configure_rr_builder(std::move(typed), resources);
    } else {
        return configure_rr_builder(std::move(builder), resources);
    }
}

template <typename Q, typename R, typename QH, typename RH>
RpcResult<NativeFactory<Q, R, QH, RH>> open_rr(
    NativeNode& node,
    const iox2::ServiceName& name,
    const RpcResourceConfig& resources,
    bool create_if_missing) {
    auto builder = make_rr_builder<Q, R, QH, RH>(node, name, resources);
    if (create_if_missing) {
        auto result = std::move(builder).open_or_create();
        if (!result.has_value()) {
            return RpcResult<NativeFactory<Q, R, QH, RH>>(
                RpcError::RequestResponseOpenOrCreate);
        }
        return RpcResult<NativeFactory<Q, R, QH, RH>>(
            std::move(result).value());
    }
    auto result = std::move(builder).open();
    if (!result.has_value()) {
        return RpcResult<NativeFactory<Q, R, QH, RH>>(
            RpcError::RequestResponseOpen);
    }
    return RpcResult<NativeFactory<Q, R, QH, RH>>(std::move(result).value());
}

template <typename Q, typename R, typename QH, typename RH>
RpcResult<NativeClient<Q, R, QH, RH>> create_native_client(
    NativeFactory<Q, R, QH, RH>& factory,
    const RpcResourceConfig& resources) {
    auto builder = factory.client_builder();
    auto result = [&]() {
        if constexpr (iox2::bb::IsSlice<Q>::VALUE) {
            return std::move(builder)
                .initial_max_slice_len(
                    static_cast<std::uint64_t>(resources.max_request_slice_len))
                .backpressure_strategy(iox2::BackpressureStrategy::DiscardData)
                .create();
        } else {
            return std::move(builder)
                .backpressure_strategy(iox2::BackpressureStrategy::DiscardData)
                .create();
        }
    }();
    if (!result.has_value()) {
        return RpcResult<NativeClient<Q, R, QH, RH>>(RpcError::ClientCreate);
    }
    return RpcResult<NativeClient<Q, R, QH, RH>>(std::move(result).value());
}

template <typename Q, typename R, typename QH, typename RH>
RpcResult<NativeServer<Q, R, QH, RH>> create_native_server(
    NativeFactory<Q, R, QH, RH>& factory,
    const RpcResourceConfig& resources) {
    auto builder = factory.server_builder();
    auto result = [&]() {
        if constexpr (iox2::bb::IsSlice<R>::VALUE) {
            return std::move(builder)
                .initial_max_slice_len(
                    static_cast<std::uint64_t>(resources.max_response_slice_len))
                .backpressure_strategy(iox2::BackpressureStrategy::DiscardData)
                .max_loaned_responses_per_request(
                    static_cast<std::uint64_t>(resources.max_response_loans))
                .create();
        } else {
            return std::move(builder)
                .backpressure_strategy(iox2::BackpressureStrategy::DiscardData)
                .max_loaned_responses_per_request(
                    static_cast<std::uint64_t>(resources.max_response_loans))
                .create();
        }
    }();
    if (!result.has_value()) {
        return RpcResult<NativeServer<Q, R, QH, RH>>(RpcError::ServerCreate);
    }
    return RpcResult<NativeServer<Q, R, QH, RH>>(std::move(result).value());
}

template <typename FactoryBuilder>
RpcResult<NativeEventFactory> open_event(FactoryBuilder&& builder,
                                          std::size_t max_listeners,
                                          std::size_t max_notifiers,
                                          std::size_t max_nodes,
                                          bool create_if_missing) {
    auto event_builder = std::forward<FactoryBuilder>(builder).event();
    std::move(event_builder).max_listeners(
        static_cast<std::uint64_t>(max_listeners));
    std::move(event_builder).max_notifiers(
        static_cast<std::uint64_t>(max_notifiers));
    std::move(event_builder).max_nodes(static_cast<std::uint64_t>(max_nodes));
    std::move(event_builder).event_id_max_value(
        static_cast<std::uint64_t>(kRpcEventId));
    if (create_if_missing) {
        auto result = std::move(event_builder).open_or_create();
        if (!result.has_value()) {
            return RpcResult<NativeEventFactory>(RpcError::EventOpenOrCreate);
        }
        return RpcResult<NativeEventFactory>(std::move(result).value());
    }
    auto result = std::move(event_builder).open();
    if (!result.has_value()) {
        return RpcResult<NativeEventFactory>(RpcError::EventOpen);
    }
    return RpcResult<NativeEventFactory>(std::move(result).value());
}

inline RpcResult<NativeListener> create_listener(NativeEventFactory& factory) {
    auto result = factory.listener_builder().create();
    if (!result.has_value()) {
        return RpcResult<NativeListener>(RpcError::Listener);
    }
    return RpcResult<NativeListener>(std::move(result).value());
}

// Per-call sidecar listener capacity is exactly max_clients * max_in_flight and
// each client is natively bounded to max_in_flight active requests, so an
// exhausted listener budget means this client's calls are all in flight.
inline RpcResult<NativeListener> create_call_listener(NativeEventFactory& factory) {
    auto result = factory.listener_builder().create();
    if (!result.has_value()) {
        if (result.error() == iox2::ListenerCreateError::ExceedsMaxSupportedListeners) {
            return RpcResult<NativeListener>(RpcError::ClientCapacityReached);
        }
        return RpcResult<NativeListener>(RpcError::Listener);
    }
    return RpcResult<NativeListener>(std::move(result).value());
}

inline RpcResult<std::shared_ptr<NativeNotifier>> create_notifier(
    NativeEventFactory& factory) {
    auto builder = factory.notifier_builder();
    auto result = std::move(builder)
                      .default_event_id(iox2::EventId(kRpcEventId))
                      .create();
    if (!result.has_value()) {
        return RpcResult<std::shared_ptr<NativeNotifier>>(RpcError::Notifier);
    }
    return RpcResult<std::shared_ptr<NativeNotifier>>(
        std::make_shared<NativeNotifier>(std::move(result).value()));
}

inline void notify(const std::shared_ptr<NativeNotifier>& notifier) noexcept {
    if (notifier != nullptr) {
        auto result = notifier->notify();
        (void)result;
    }
}

template <typename Rep, typename Period>
std::chrono::steady_clock::duration to_steady_duration(
    const std::chrono::duration<Rep, Period>& duration) {
    if (duration <= std::chrono::duration<Rep, Period>::zero()) {
        return std::chrono::steady_clock::duration::zero();
    }
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        duration);
}

inline RpcError map_rpc_loan_error(iox2::LoanError error) noexcept {
    switch (error) {
    case iox2::LoanError::OutOfMemory:
    case iox2::LoanError::ExceedsMaxLoanedSamples:
        return RpcError::LoanPoolExhausted;
    default:
        return RpcError::TransportFailure;
    }
}

inline RpcError map_rpc_receive_error(iox2::ReceiveError error) noexcept {
    return error == iox2::ReceiveError::ExceedsMaxBorrows
               ? RpcError::BorrowLimitReached
               : RpcError::TransportFailure;
}

inline RpcError map_request_send_error(iox2::RequestSendError error) noexcept {
    switch (error) {
    case iox2::RequestSendError::ExceedsMaxActiveRequests:
        return RpcError::ClientCapacityReached;
    case iox2::RequestSendError::UnableToDeliver:
        return RpcError::ServerCapacityReached;
    case iox2::RequestSendError::ConnectionBrokenSinceSenderNoLongerExists:
    case iox2::RequestSendError::ConnectionError:
        return RpcError::ServerUnavailable;
    case iox2::RequestSendError::LoanErrorOutOfMemory:
    case iox2::RequestSendError::LoanErrorExceedsMaxLoans:
        return RpcError::LoanPoolExhausted;
    default:
        return RpcError::TransportFailure;
    }
}

inline RpcError map_response_send_error(iox2::SendError error) noexcept {
    switch (error) {
    case iox2::SendError::UnableToDeliver:
        return RpcError::ResponseCapacityReached;
    case iox2::SendError::ConnectionBrokenSinceSenderNoLongerExists:
    case iox2::SendError::ConnectionError:
        return RpcError::ServerUnavailable;
    case iox2::SendError::LoanErrorOutOfMemory:
    case iox2::SendError::LoanErrorExceedsMaxLoans:
        return RpcError::LoanPoolExhausted;
    default:
        return RpcError::TransportFailure;
    }
}

inline std::uint64_t saturating_increment(std::atomic<std::uint64_t>& value) noexcept {
    auto current = value.load(std::memory_order_relaxed);
    while (current != std::numeric_limits<std::uint64_t>::max()
           && !value.compare_exchange_weak(current,
                                            current + 1U,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
    }
    return current == std::numeric_limits<std::uint64_t>::max() ? current : current + 1U;
}

inline void rpc_saturating_add(std::atomic<std::uint64_t>& value,
                               std::uint64_t amount) noexcept {
    auto current = value.load(std::memory_order_relaxed);
    for (;;) {
        const auto next = amount > std::numeric_limits<std::uint64_t>::max() - current
                              ? std::numeric_limits<std::uint64_t>::max()
                              : current + amount;
        if (value.compare_exchange_weak(current,
                                         next,
                                         std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
            return;
        }
    }
}

inline void rpc_saturating_max(std::atomic<std::uint64_t>& value,
                               std::uint64_t candidate) noexcept {
    auto current = value.load(std::memory_order_relaxed);
    while (current < candidate
           && !value.compare_exchange_weak(current,
                                            candidate,
                                            std::memory_order_relaxed,
                                            std::memory_order_relaxed)) {
    }
}

struct ClientMetricsState final : RpcPressureSnapshotSource {
    std::string service_name;
    std::optional<std::string> capability_instance;
    RpcResourceConfig resources;
    std::optional<std::uint64_t> activation;
    std::atomic<std::uint64_t> calls_attempted { 0U };
    std::atomic<std::uint64_t> calls_started { 0U };
    std::atomic<std::uint64_t> calls_completed { 0U };
    std::atomic<std::uint64_t> calls_failed { 0U };
    std::atomic<std::uint64_t> calls_timed_out { 0U };
    std::atomic<std::uint64_t> calls_cancelled { 0U };
    std::atomic<std::uint64_t> calls_activation_expired { 0U };
    std::atomic<std::uint64_t> in_flight_current { 0U };
    std::atomic<std::uint64_t> in_flight_high_water { 0U };
    std::atomic<std::uint64_t> response_guards_current { 0U };
    std::atomic<std::uint64_t> response_guards_high_water { 0U };
    std::atomic<std::uint64_t> capacity_rejections { 0U };
    std::atomic<std::uint64_t> server_capacity_rejections { 0U };
    std::atomic<std::uint64_t> server_unavailable { 0U };
    std::atomic<std::uint64_t> request_loan_failures { 0U };
    std::atomic<std::uint64_t> response_receive_failures { 0U };
    std::atomic<std::uint64_t> call_duration_count { 0U };
    std::atomic<std::uint64_t> call_duration_total_ns { 0U };
    std::atomic<std::uint64_t> call_duration_max_ns { 0U };

    ClientMetricsState(std::string service,
                       RpcResourceConfig limits,
                       std::optional<std::string> capability,
                       std::optional<std::uint64_t> activation_id)
        : service_name(std::move(service))
        , capability_instance(std::move(capability))
        , resources(limits)
        , activation(activation_id) {
    }

    void attempted() noexcept { saturating_increment(calls_attempted); }
    void started() noexcept {
        saturating_increment(calls_started);
        const auto current = saturating_increment(in_flight_current);
        rpc_saturating_max(in_flight_high_water, current);
    }
    void response_guard_created() noexcept {
        const auto current = saturating_increment(response_guards_current);
        rpc_saturating_max(response_guards_high_water, current);
    }
    void response_guard_released() noexcept {
        auto current = response_guards_current.load(std::memory_order_relaxed);
        while (current != 0U
               && !response_guards_current.compare_exchange_weak(
                   current,
                   current - 1U,
                   std::memory_order_relaxed,
                   std::memory_order_relaxed)) {
        }
    }
    void request_loan_failed() noexcept { saturating_increment(request_loan_failures); }
    void response_receive_failed() noexcept { saturating_increment(response_receive_failures); }
    void capacity_rejected() noexcept { saturating_increment(capacity_rejections); }
    void server_capacity_rejected() noexcept { saturating_increment(server_capacity_rejections); }
    void server_unavailable_failed() noexcept { saturating_increment(server_unavailable); }

    void complete(std::chrono::steady_clock::duration duration) noexcept {
        finish_in_flight(duration);
        saturating_increment(calls_completed);
    }

    void finished(RpcError terminal,
                  std::chrono::steady_clock::duration duration) noexcept {
        finish_in_flight(duration);
        switch (terminal) {
        case RpcError::Cancelled:
            saturating_increment(calls_cancelled);
            break;
        case RpcError::ActivationExpired:
            saturating_increment(calls_activation_expired);
            break;
        case RpcError::DeadlineExceeded:
            saturating_increment(calls_timed_out);
            break;
        default:
            saturating_increment(calls_failed);
            break;
        }
    }

  private:
    void finish_in_flight(std::chrono::steady_clock::duration duration) noexcept {
        auto current = in_flight_current.load(std::memory_order_relaxed);
        while (current != 0U
               && !in_flight_current.compare_exchange_weak(current,
                                                            current - 1U,
                                                            std::memory_order_relaxed,
                                                            std::memory_order_relaxed)) {
        }
        const auto duration_ns = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
        saturating_increment(call_duration_count);
        rpc_saturating_add(call_duration_total_ns, duration_ns);
        rpc_saturating_max(call_duration_max_ns, duration_ns);
    }

  public:
    [[nodiscard]] RpcPressure pressure_snapshot() const override {
        RpcPressure result;
        result.service_name = service_name;
        result.capability_instance = capability_instance;
        result.resources = resources;
        result.client_activation = activation;
        result.snapshot_timestamp_unix_ms = rpc_unix_timestamp_ms();
        RpcClientMetrics metrics;
        metrics.calls_attempted = calls_attempted.load(std::memory_order_relaxed);
        metrics.calls_started = calls_started.load(std::memory_order_relaxed);
        metrics.calls_completed = calls_completed.load(std::memory_order_relaxed);
        metrics.calls_failed = calls_failed.load(std::memory_order_relaxed);
        metrics.calls_timed_out = calls_timed_out.load(std::memory_order_relaxed);
        metrics.calls_cancelled = calls_cancelled.load(std::memory_order_relaxed);
        metrics.calls_activation_expired = calls_activation_expired.load(std::memory_order_relaxed);
        metrics.in_flight_current = in_flight_current.load(std::memory_order_relaxed);
        metrics.in_flight_high_water = in_flight_high_water.load(std::memory_order_relaxed);
        metrics.response_guards_current =
            response_guards_current.load(std::memory_order_relaxed);
        metrics.response_guards_high_water =
            response_guards_high_water.load(std::memory_order_relaxed);
        metrics.capacity_rejections = capacity_rejections.load(std::memory_order_relaxed);
        metrics.server_capacity_rejections = server_capacity_rejections.load(std::memory_order_relaxed);
        metrics.server_unavailable = server_unavailable.load(std::memory_order_relaxed);
        metrics.request_loan_failures = request_loan_failures.load(std::memory_order_relaxed);
        metrics.response_receive_failures = response_receive_failures.load(std::memory_order_relaxed);
        metrics.call_duration_count = call_duration_count.load(std::memory_order_relaxed);
        metrics.call_duration_total_ns = call_duration_total_ns.load(std::memory_order_relaxed);
        metrics.call_duration_max_ns = call_duration_max_ns.load(std::memory_order_relaxed);
        result.client = metrics;
        return result;
    }
};

struct ServerMetricsState final : RpcPressureSnapshotSource {
    std::string service_name;
    std::optional<std::string> capability_instance;
    RpcResourceConfig resources;
    std::atomic<std::uint64_t> requests_received { 0U };
    std::atomic<std::uint64_t> requests_current { 0U };
    std::atomic<std::uint64_t> requests_high_water { 0U };
    std::atomic<std::uint64_t> responses_attempted { 0U };
    std::atomic<std::uint64_t> responses_sent { 0U };
    std::atomic<std::uint64_t> responses_failed { 0U };
    std::atomic<std::uint64_t> responses_abandoned { 0U };
    std::atomic<std::uint64_t> request_borrow_failures { 0U };
    std::atomic<std::uint64_t> response_loan_failures { 0U };

    ServerMetricsState(std::string service,
                       RpcResourceConfig limits,
                       std::optional<std::string> capability)
        : service_name(std::move(service))
        , capability_instance(std::move(capability))
        , resources(limits) {
    }

    void request_received() noexcept {
        saturating_increment(requests_received);
        const auto current = saturating_increment(requests_current);
        rpc_saturating_max(requests_high_water, current);
    }
    void request_released() noexcept {
        auto current = requests_current.load(std::memory_order_relaxed);
        while (current != 0U
               && !requests_current.compare_exchange_weak(current,
                                                           current - 1U,
                                                           std::memory_order_relaxed,
                                                           std::memory_order_relaxed)) {
        }
    }
    [[nodiscard]] RpcPressure pressure_snapshot() const override {
        RpcPressure result;
        result.service_name = service_name;
        result.capability_instance = capability_instance;
        result.resources = resources;
        result.snapshot_timestamp_unix_ms = rpc_unix_timestamp_ms();
        RpcServerMetrics metrics;
        metrics.requests_received = requests_received.load(std::memory_order_relaxed);
        metrics.requests_current = requests_current.load(std::memory_order_relaxed);
        metrics.requests_high_water = requests_high_water.load(std::memory_order_relaxed);
        metrics.responses_attempted = responses_attempted.load(std::memory_order_relaxed);
        metrics.responses_sent = responses_sent.load(std::memory_order_relaxed);
        metrics.responses_failed = responses_failed.load(std::memory_order_relaxed);
        metrics.responses_abandoned = responses_abandoned.load(std::memory_order_relaxed);
        metrics.request_borrow_failures = request_borrow_failures.load(std::memory_order_relaxed);
        metrics.response_loan_failures = response_loan_failures.load(std::memory_order_relaxed);
        result.server = metrics;
        return result;
    }
};

struct ClientState {
    // Declare the node before every port so C++ reverse destruction order
    // drops ports first; a node destroyed while its ports still exist cannot
    // remove its node directory and leaves it behind (see WaitWakeState).
    std::shared_ptr<NativeNode> node;
    std::atomic<bool> expired { false };
    std::shared_ptr<NativeNotifier> notifier;

    void expire() noexcept {
        expired.store(true, std::memory_order_release);
        notify(notifier);
    }
};

struct ServerState {
    // Declare the node before every port so C++ reverse destruction order
    // drops ports first; a node destroyed while its ports still exist cannot
    // remove its node directory and leaves it behind (see WaitWakeState).
    std::shared_ptr<NativeNode> node;
    std::atomic<bool> expired { false };
    std::shared_ptr<NativeNotifier> notifier;

    void expire() noexcept {
        expired.store(true, std::memory_order_release);
        notify(notifier);
    }
};

template <typename Q, typename R, typename QH, typename RH>
struct ClientInner {
    // Owns the node; declared first so it outlives the native client and
    // notifier below.
    std::shared_ptr<ClientState> state;
    std::mutex native_mutex;
    NativeClient<Q, R, QH, RH> native;
    RpcDescriptor descriptor;
    iox2::ServiceName response_ready_name;
    iox2::ServiceName client_cancel_name;
    std::size_t response_listener_capacity { 0U };
    std::size_t event_max_nodes { 0U };
    std::shared_ptr<NativeNotifier> request_notifier;
    std::shared_ptr<ClientMetricsState> metrics;

    ClientInner(NativeClient<Q, R, QH, RH>&& endpoint,
                RpcDescriptor endpoint_descriptor,
                iox2::ServiceName response_name,
                iox2::ServiceName cancel_name,
                std::size_t listener_capacity,
                std::size_t nodes,
                std::shared_ptr<NativeNotifier> request_ready_notifier,
                std::shared_ptr<ClientMetricsState> client_metrics,
                std::shared_ptr<ClientState> client_state)
        : state(std::move(client_state))
        , native(std::move(endpoint))
        , descriptor(std::move(endpoint_descriptor))
        , response_ready_name(std::move(response_name))
        , client_cancel_name(std::move(cancel_name))
        , response_listener_capacity(listener_capacity)
        , event_max_nodes(nodes)
        , request_notifier(std::move(request_ready_notifier))
        , metrics(std::move(client_metrics)) {
    }
};

} // namespace detail

namespace detail {

template <typename T, typename = void>
struct has_fixed_binding : std::false_type {
};

template <typename T>
struct has_fixed_binding<T,
                         std::void_t<decltype(FixedMessageBinding<T>::descriptor())>>
    : std::true_type {
};

template <typename T>
struct is_slice_payload : std::false_type {
};

template <typename T>
struct is_slice_payload<iox2::bb::Slice<T>> : std::true_type {
    using element_type = T;
};

template <typename Payload, typename UserHeader>
const MessageTypeDescriptor& descriptor_for() {
    if constexpr (is_slice_payload<Payload>::value) {
        return SliceMessageBinding<UserHeader,
                                   typename is_slice_payload<Payload>::element_type>::descriptor();
    } else {
        static_assert(has_fixed_binding<Payload>::value,
                      "RPC fixed payload needs a generated FixedMessageBinding");
        return FixedMessageBinding<Payload>::descriptor();
    }
}

} // namespace detail

template <typename Request, typename Response>
RpcDescriptor RpcDescriptor::for_fixed(std::string_view service) {
    return RpcDescriptor(service,
                         detail::descriptor_for<Request, void>(),
                         detail::descriptor_for<Response, void>());
}

template <typename RequestHeader,
          typename RequestElement,
          typename ResponseHeader,
          typename ResponseElement>
RpcDescriptor RpcDescriptor::for_slices(std::string_view service) {
    return RpcDescriptor(service,
                         SliceMessageBinding<RequestHeader, RequestElement>::descriptor(),
                         SliceMessageBinding<ResponseHeader, ResponseElement>::descriptor());
}

template <typename Request, typename ResponseHeader, typename ResponseElement>
RpcDescriptor RpcDescriptor::for_fixed_slice(std::string_view service) {
    return RpcDescriptor(service,
                         detail::descriptor_for<Request, void>(),
                         SliceMessageBinding<ResponseHeader, ResponseElement>::descriptor());
}

template <typename RequestHeader, typename RequestElement, typename Response>
RpcDescriptor RpcDescriptor::for_slice_fixed(std::string_view service) {
    return RpcDescriptor(
        service,
        SliceMessageBinding<RequestHeader, RequestElement>::descriptor(),
        detail::descriptor_for<Response, void>());
}

template <typename RequestHeader,
          typename RequestElement,
          typename ResponseHeader,
          typename ResponseElement>
RpcDescriptor RpcDescriptor::for_slice(std::string_view service) {
    return for_slices<RequestHeader, RequestElement, ResponseHeader, ResponseElement>(service);
}

template <typename Request, typename Response>
RpcDescriptor RpcDescriptor::for_fixed_with_resources(std::string_view service,
                                                       RpcResourceConfig resources) {
    auto descriptor = for_fixed<Request, Response>(service);
    descriptor.resources = resources;
    return descriptor;
}

template <typename Request, typename ResponseHeader, typename ResponseElement>
RpcDescriptor RpcDescriptor::for_fixed_slice_with_resources(
    std::string_view service,
    RpcResourceConfig resources) {
    auto descriptor = for_fixed_slice<Request, ResponseHeader, ResponseElement>(service);
    descriptor.resources = resources;
    return descriptor;
}

template <typename RequestHeader, typename RequestElement, typename Response>
RpcDescriptor RpcDescriptor::for_slice_fixed_with_resources(
    std::string_view service,
    RpcResourceConfig resources) {
    auto descriptor = for_slice_fixed<RequestHeader, RequestElement, Response>(service);
    descriptor.resources = resources;
    return descriptor;
}

template <typename RequestHeader,
          typename RequestElement,
          typename ResponseHeader,
          typename ResponseElement>
RpcDescriptor RpcDescriptor::for_slice_with_resources(
    std::string_view service,
    RpcResourceConfig resources) {
    auto descriptor = for_slice<RequestHeader, RequestElement, ResponseHeader, ResponseElement>(
        service);
    descriptor.resources = resources;
    return descriptor;
}

template <typename RequestPayload,
          typename ResponsePayload,
          typename RequestHeader = void,
          typename ResponseHeader = void>
RpcResult<void> preflight_rpc(const RpcDescriptor& descriptor) {
    if (detail::descriptor_for<RequestPayload, RequestHeader>() !=
        descriptor.request_type) {
        return RpcResult<void>(RpcError::AbiMismatch);
    }
    if (detail::descriptor_for<ResponsePayload, ResponseHeader>() !=
        descriptor.response_type) {
        return RpcResult<void>(RpcError::AbiMismatch);
    }
    if (descriptor.transport.readiness != RpcReadinessProtocol::RpcSidecarV1) {
        return RpcResult<void>(RpcError::MissingTransportFeature);
    }
    auto resources = detail::validate_rpc_resources(descriptor.resources);
    if (!resources.has_value()) {
        return RpcResult<void>(resources.error());
    }
    return RpcResult<void>();
}

template <typename RequestPayload,
          typename ResponsePayload,
          typename RequestHeader = void,
          typename ResponseHeader = void>
class RpcResponseGuard;

template <typename RequestPayload,
          typename ResponsePayload,
          typename RequestHeader = void,
          typename ResponseHeader = void>
class PendingRpcCall;

template <typename RequestPayload,
          typename ResponsePayload,
          typename RequestHeader = void,
          typename ResponseHeader = void>
class RpcCallCancelHandle;

template <typename RequestPayload,
          typename ResponsePayload,
          typename RequestHeader = void,
          typename ResponseHeader = void>
class RpcRequestLoan;

template <typename RequestPayload,
          typename ResponsePayload,
          typename RequestHeader = void,
          typename ResponseHeader = void>
class RpcClient;

template <typename RequestPayload,
          typename ResponsePayload,
          typename RequestHeader = void,
          typename ResponseHeader = void>
class RpcRequest;

template <typename RequestPayload,
          typename ResponsePayload,
          typename RequestHeader = void,
          typename ResponseHeader = void>
class RpcResponseLoan;

template <typename RequestPayload,
          typename ResponsePayload,
          typename RequestHeader = void,
          typename ResponseHeader = void>
class RpcServer;

class RpcClientWakeHandle {
  public:
    RpcClientWakeHandle() = default;

    RpcResult<void> wake() const noexcept {
        if (state_ == nullptr) {
            return RpcResult<void>(RpcError::TransportFailure);
        }
        state_->expire();
        return RpcResult<void>();
    }
    RpcResult<void> expire() const noexcept {
        return wake();
    }
    [[nodiscard]] bool is_expired() const noexcept {
        return state_ != nullptr
            && state_->expired.load(std::memory_order_acquire);
    }

  private:
    explicit RpcClientWakeHandle(std::shared_ptr<detail::ClientState> state)
        : state_(std::move(state)) {
    }
    std::shared_ptr<detail::ClientState> state_;

    template <typename, typename, typename, typename>
    friend class RpcClient;
};

class RpcServerWakeHandle {
  public:
    RpcServerWakeHandle() = default;

    RpcResult<void> wake() const noexcept {
        if (state_ == nullptr) {
            return RpcResult<void>(RpcError::TransportFailure);
        }
        state_->expire();
        return RpcResult<void>();
    }
    RpcResult<void> expire() const noexcept {
        return wake();
    }
    [[nodiscard]] bool is_expired() const noexcept {
        return state_ != nullptr
            && state_->expired.load(std::memory_order_acquire);
    }

  private:
    explicit RpcServerWakeHandle(std::shared_ptr<detail::ServerState> state)
        : state_(std::move(state)) {
    }
    std::shared_ptr<detail::ServerState> state_;

    template <typename, typename, typename, typename>
    friend class RpcServer;
};

template <typename RequestPayload,
          typename ResponsePayload,
          typename RequestHeader,
          typename ResponseHeader>
class RpcResponseGuard {
    using Native = detail::NativeResponse<ResponsePayload, ResponseHeader>;

  public:
    RpcResponseGuard(const RpcResponseGuard&) = delete;
    RpcResponseGuard& operator=(const RpcResponseGuard&) = delete;
    RpcResponseGuard(RpcResponseGuard&& other) noexcept
        : state_(std::move(other.state_))
        , response_(std::move(other.response_))
        , metrics_(std::move(other.metrics_)) {
    }
    RpcResponseGuard& operator=(RpcResponseGuard&& other) noexcept {
        if (this != &other) {
            release();
            state_ = std::move(other.state_);
            response_ = std::move(other.response_);
            metrics_ = std::move(other.metrics_);
        }
        return *this;
    }
    ~RpcResponseGuard() {
        release();
    }

    template <typename P = ResponsePayload,
              typename std::enable_if<!detail::is_slice_payload<P>::value, int>::type = 0>
    [[nodiscard]] const P& payload() const {
        return response_->template payload<P>();
    }

    template <typename P = ResponsePayload,
              typename std::enable_if<detail::is_slice_payload<P>::value, int>::type = 0>
    [[nodiscard]] auto payload() const {
        return response_->template payload<P>();
    }

    template <typename P = ResponsePayload,
              typename std::enable_if<!detail::is_slice_payload<P>::value, int>::type = 0>
    [[nodiscard]] const P& value() const {
        return response_->template payload<P>();
    }

    template <typename P = ResponsePayload,
              typename std::enable_if<detail::is_slice_payload<P>::value, int>::type = 0>
    [[nodiscard]] auto slice() const {
        return response_->template payload<P>();
    }

    template <typename H = ResponseHeader,
              typename std::enable_if<!std::is_same<H, void>::value, int>::type = 0>
    [[nodiscard]] const H& user_header() const {
        return response_->template user_header<H>();
    }

    template <typename P = ResponsePayload,
              typename std::enable_if<!detail::is_slice_payload<P>::value, int>::type = 0>
    const P& operator*() const {
        return payload();
    }
    template <typename P = ResponsePayload,
              typename std::enable_if<!detail::is_slice_payload<P>::value, int>::type = 0>
    const P* operator->() const {
        return std::addressof(payload());
    }

  private:
    explicit RpcResponseGuard(Native&& response,
                              std::shared_ptr<detail::ClientState> state,
                              std::shared_ptr<detail::ClientMetricsState> metrics)
        : state_(std::move(state))
        , response_(std::in_place, std::move(response))
        , metrics_(std::move(metrics)) {
        if (metrics_ != nullptr) {
            metrics_->response_guard_created();
        }
    }

    void release() noexcept {
        response_.reset();
        state_.reset();
        if (metrics_ != nullptr) {
            metrics_->response_guard_released();
            metrics_.reset();
        }
    }

    // Node owner first: members are destroyed in reverse order, so the node
    // outlives the native objects below.
    std::shared_ptr<detail::ClientState> state_;
    std::optional<Native> response_;
    std::shared_ptr<detail::ClientMetricsState> metrics_;

    template <typename, typename, typename, typename>
    friend class PendingRpcCall;
};

template <typename RequestPayload,
          typename ResponsePayload,
          typename RequestHeader,
          typename ResponseHeader>
class RpcCallCancelHandle {
  public:
    RpcResult<void> cancel() const noexcept {
        if (state_ == nullptr || client_state_ == nullptr) {
            return RpcResult<void>(RpcError::TransportFailure);
        }
        auto expected = expected_open_;
        state_->compare_exchange_strong(expected,
                                        terminal_cancelled_,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire);
        detail::notify(client_state_->notifier);
        return RpcResult<void>();
    }

    [[nodiscard]] bool is_cancelled() const noexcept {
        return state_ != nullptr
            && state_->load(std::memory_order_acquire) == terminal_cancelled_;
    }

  private:
    static constexpr std::uint8_t expected_open_ = 0U;
    static constexpr std::uint8_t terminal_cancelled_ = 2U;

    RpcCallCancelHandle(std::shared_ptr<std::atomic<std::uint8_t>> state,
                        std::shared_ptr<detail::ClientState> client_state)
        : state_(std::move(state))
        , client_state_(std::move(client_state)) {
    }

    std::shared_ptr<std::atomic<std::uint8_t>> state_;
    std::shared_ptr<detail::ClientState> client_state_;

    template <typename, typename, typename, typename>
    friend class PendingRpcCall;
};

template <typename RequestPayload,
          typename ResponsePayload,
          typename RequestHeader,
          typename ResponseHeader>
class PendingRpcCall {
    using NativePending =
        detail::NativePending<RequestPayload, ResponsePayload, RequestHeader, ResponseHeader>;
    using NativeResponse = detail::NativeResponse<ResponsePayload, ResponseHeader>;

  public:
    PendingRpcCall(const PendingRpcCall&) = delete;
    PendingRpcCall& operator=(const PendingRpcCall&) = delete;
    PendingRpcCall(PendingRpcCall&&) noexcept = default;
    PendingRpcCall& operator=(PendingRpcCall&& other) noexcept {
        if (this != &other) {
            // Keep the old native handles with their node owner until normal cleanup runs.
            PendingRpcCall displaced(std::move(*this));
            client_state_ = std::move(other.client_state_);
            pending_ = std::move(other.pending_);
            pre_received_ = std::move(other.pre_received_);
            response_listener_ = std::move(other.response_listener_);
            cancellation_listener_ = std::move(other.cancellation_listener_);
            metrics_ = std::move(other.metrics_);
            started_at_ = std::move(other.started_at_);
            state_ = std::move(other.state_);
            metrics_recorded_ = std::move(other.metrics_recorded_);
            (void)displaced;
        }
        return *this;
    }
    ~PendingRpcCall() {
        if (state_ != nullptr && metrics_ != nullptr
            && !metrics_recorded_->load(std::memory_order_acquire)) {
            record_terminal(state_->load(std::memory_order_acquire) == 2U
                                ? RpcError::Cancelled
                                : state_->load(std::memory_order_acquire) == 3U
                                      ? RpcError::ActivationExpired
                                      : state_->load(std::memory_order_acquire) == 4U
                                            ? RpcError::DeadlineExceeded
                                            : RpcError::TransportFailure);
        }
    }

    [[nodiscard]] RpcCallCancelHandle<RequestPayload,
                                      ResponsePayload,
                                      RequestHeader,
                                      ResponseHeader>
    cancel_handle() const {
        return RpcCallCancelHandle<RequestPayload,
                                   ResponsePayload,
                                   RequestHeader,
                                   ResponseHeader>(
            state_, client_state_);
    }

    [[nodiscard]] bool is_cancelled() const noexcept {
        return state_ != nullptr && state_->load(std::memory_order_acquire) == 2U;
    }

    RpcResult<std::optional<
        RpcResponseGuard<RequestPayload, ResponsePayload, RequestHeader, ResponseHeader>>>
    try_wait() {
        if (client_state_->expired.load(std::memory_order_acquire)
            && claim(3U)) {
            record_terminal(RpcError::ActivationExpired);
            pending_.reset();
            return RpcResult<std::optional<Guard>>(RpcError::ActivationExpired);
        }
        if (state_->load(std::memory_order_acquire) != 0U) {
            record_terminal(terminal_error());
            return RpcResult<std::optional<Guard>>(terminal_error());
        }
        if (pre_received_.has_value()) {
            auto response = std::move(*pre_received_);
            pre_received_.reset();
            return finish_response(std::move(response));
        }
        auto response = poll_response();
        if (!response.has_value()) {
            return RpcResult<std::optional<Guard>>(response.error());
        }
        if (!response.value().has_value()) {
            return RpcResult<std::optional<Guard>>(std::optional<Guard> {});
        }
        return finish_response(std::move(response).value().value());
    }

    template <typename Rep, typename Period>
    RpcResult<RpcResponseGuard<RequestPayload,
                               ResponsePayload,
                               RequestHeader,
                               ResponseHeader>>
    wait(const std::chrono::duration<Rep, Period>& timeout) {
        using Guard = RpcResponseGuard<RequestPayload,
                                       ResponsePayload,
                                       RequestHeader,
                                       ResponseHeader>;
        const auto started = std::chrono::steady_clock::now();
        const auto duration = detail::to_steady_duration(timeout);

        for (;;) {
            if (client_state_->expired.load(std::memory_order_acquire)
                && claim(3U)) {
                record_terminal(RpcError::ActivationExpired);
                pending_.reset();
                return RpcResult<Guard>(RpcError::ActivationExpired);
            }
            if (state_->load(std::memory_order_acquire) != 0U) {
                record_terminal(terminal_error());
                pending_.reset();
                return RpcResult<Guard>(terminal_error());
            }
            if (pre_received_.has_value()) {
                auto response = std::move(*pre_received_);
                pre_received_.reset();
                auto result = finish_response(std::move(response));
                if (result.has_value()) {
                    return RpcResult<Guard>(std::move(result).value().value());
                }
                return RpcResult<Guard>(result.error());
            }

            auto immediate = poll_response();
            if (!immediate.has_value()) {
                return RpcResult<Guard>(immediate.error());
            }
            if (immediate.value().has_value()) {
                auto result = finish_response(
                    std::move(immediate).value().value());
                if (result.has_value()) {
                    return RpcResult<Guard>(std::move(result).value().value());
                }
                return RpcResult<Guard>(result.error());
            }

            const auto elapsed = std::chrono::steady_clock::now() - started;
            const auto remaining = elapsed >= duration
                                       ? std::chrono::steady_clock::duration::zero()
                                       : duration - elapsed;
            if (remaining == std::chrono::steady_clock::duration::zero()) {
                auto final_probe = poll_response();
                if (!final_probe.has_value()) {
                    return RpcResult<Guard>(final_probe.error());
                }
                if (final_probe.value().has_value()) {
                    auto result = finish_response(
                        std::move(final_probe).value().value());
                    if (result.has_value()) {
                        return RpcResult<Guard>(std::move(result).value().value());
                    }
                    return RpcResult<Guard>(result.error());
                }
                if (claim(4U)) {
                    record_terminal(RpcError::DeadlineExceeded);
                    pending_.reset();
                    return RpcResult<Guard>(RpcError::DeadlineExceeded);
                }
                pending_.reset();
                record_terminal(terminal_error());
                return RpcResult<Guard>(terminal_error());
            }

            enum class Outcome : std::uint8_t {
                Continue,
                Cancelled,
                Expired,
            };
            Outcome outcome = Outcome::Continue;
            std::optional<NativeResponse> received;
            std::optional<RpcError> callback_error;

            auto waitset_result = iox2::WaitSetBuilder()
                                      .signal_handling_mode(
                                          iox2::SignalHandlingMode::Disabled)
                                      .create<detail::RpcIpcService>();
            if (!waitset_result.has_value()) {
                return RpcResult<Guard>(RpcError::TransportFailure);
            }
            auto waitset = std::move(waitset_result).value();
            auto response_guard =
                waitset.attach_notification(response_listener_);
            auto cancellation_guard =
                waitset.attach_notification(cancellation_listener_);
            auto interval_guard = waitset.attach_interval(
                iox2::bb::Duration::from_millis(
                    static_cast<std::uint64_t>(
                        detail::kRpcWaitInterval.count())));
            if (!response_guard.has_value() || !cancellation_guard.has_value()
                || !interval_guard.has_value()) {
                return RpcResult<Guard>(RpcError::TransportFailure);
            }

            auto response_attachment = std::move(response_guard).value();
            auto cancellation_attachment = std::move(cancellation_guard).value();
            auto interval_attachment = std::move(interval_guard).value();

            auto response_probe = detail::try_consume_activations(response_listener_);
            auto cancellation_probe = detail::try_consume_activations(cancellation_listener_);
            if (!response_probe.has_value() || !cancellation_probe.has_value()) {
                return RpcResult<Guard>(RpcError::TransportFailure);
            }
            if (client_state_->expired.load(std::memory_order_acquire)) {
                outcome = Outcome::Expired;
            } else if (state_->load(std::memory_order_acquire) == 2U) {
                outcome = Outcome::Cancelled;
            } else {
                auto probe = poll_response();
                if (!probe.has_value()) {
                    return RpcResult<Guard>(probe.error());
                }
                if (probe.value().has_value()) {
                    received.emplace(std::move(probe).value().value());
                }
            }

            if (!received.has_value() && outcome == Outcome::Continue) {
                auto callback = [&](iox2::WaitSetAttachmentId<detail::RpcIpcService> id) {
                    if (id.has_event_from(cancellation_attachment)) {
                        auto event = detail::try_consume_activations(cancellation_listener_);
                        if (!event.has_value()) {
                            callback_error = RpcError::TransportFailure;
                            return iox2::CallbackProgression::Stop;
                        }
                        if (client_state_->expired.load(std::memory_order_acquire)) {
                            (void)claim(3U);
                            outcome = Outcome::Expired;
                            return iox2::CallbackProgression::Stop;
                        }
                        if (state_->load(std::memory_order_acquire) == 2U) {
                            outcome = Outcome::Cancelled;
                            return iox2::CallbackProgression::Stop;
                        }
                        // A different pending call may have raised this
                        // shared sidecar; this call's atomic state is the
                        // cancellation authority.
                        return iox2::CallbackProgression::Continue;
                    }
                    if (id.has_event_from(response_attachment)) {
                        auto event = detail::try_consume_activations(response_listener_);
                        if (!event.has_value()) {
                            callback_error = RpcError::TransportFailure;
                            return iox2::CallbackProgression::Stop;
                        }
                        if (!event.value()) {
                            return iox2::CallbackProgression::Continue;
                        }
                        auto response = poll_response();
                        if (!response.has_value()) {
                            callback_error = response.error();
                            return iox2::CallbackProgression::Stop;
                        }
                        if (response.value().has_value()) {
                            received.emplace(std::move(response).value().value());
                            return iox2::CallbackProgression::Stop;
                        }
                        return iox2::CallbackProgression::Continue;
                    }
                    if (id.has_event_from(interval_attachment)) {
                        auto response = poll_response();
                        if (!response.has_value()) {
                            callback_error = response.error();
                            return iox2::CallbackProgression::Stop;
                        }
                        if (response.value().has_value()) {
                            received.emplace(std::move(response).value().value());
                            return iox2::CallbackProgression::Stop;
                        }
                    }
                    return iox2::CallbackProgression::Continue;
                };
                iox2::bb::StaticFunction<
                    iox2::CallbackProgression(
                        iox2::WaitSetAttachmentId<detail::RpcIpcService>)>
                    static_callback(callback);
                auto wait_result =
                    waitset.wait_and_process_once_with_timeout(
                        static_callback,
                        iox2::bb::Duration::from_nanos(
                            static_cast<std::uint64_t>(
                                std::chrono::duration_cast<std::chrono::nanoseconds>(
                                    remaining)
                                    .count())));
                if (!wait_result.has_value()) {
                    return RpcResult<Guard>(RpcError::TransportFailure);
                }
            }

            if (callback_error.has_value()) {
                return RpcResult<Guard>(*callback_error);
            }
            if (received.has_value()) {
                auto result = finish_response(std::move(*received));
                if (result.has_value()) {
                    return RpcResult<Guard>(std::move(result).value().value());
                }
                return RpcResult<Guard>(result.error());
            }
            if (outcome == Outcome::Expired) {
                record_terminal(RpcError::ActivationExpired);
                pending_.reset();
                return RpcResult<Guard>(RpcError::ActivationExpired);
            }
            if (outcome == Outcome::Cancelled) {
                record_terminal(RpcError::Cancelled);
                pending_.reset();
                return RpcResult<Guard>(RpcError::Cancelled);
            }
        }
    }

  private:
    using Guard = RpcResponseGuard<RequestPayload,
                                   ResponsePayload,
                                   RequestHeader,
                                   ResponseHeader>;

    PendingRpcCall(
        detail::NativePending<RequestPayload,
                              ResponsePayload,
                              RequestHeader,
                              ResponseHeader>&& pending,
        std::optional<NativeResponse>&& pre_received,
        detail::NativeListener&& response_listener,
        detail::NativeListener&& cancellation_listener,
        std::shared_ptr<detail::ClientState> client_state,
        std::shared_ptr<detail::ClientMetricsState> metrics)
        : client_state_(std::move(client_state))
        , pending_(std::in_place, std::move(pending))
        , pre_received_(std::move(pre_received))
        , response_listener_(std::move(response_listener))
        , cancellation_listener_(std::move(cancellation_listener))
        , metrics_(std::move(metrics))
        , started_at_(std::chrono::steady_clock::now())
        , state_(std::make_shared<std::atomic<std::uint8_t>>(0U))
        , metrics_recorded_(std::make_shared<std::atomic<bool>>(false)) {
    }

    [[nodiscard]] bool claim(std::uint8_t terminal) const noexcept {
        std::uint8_t expected = 0U;
        return state_->compare_exchange_strong(expected,
                                               terminal,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire);
    }

    [[nodiscard]] RpcError terminal_error() const noexcept {
        switch (state_->load(std::memory_order_acquire)) {
        case 2U:
            return RpcError::Cancelled;
        case 3U:
            return RpcError::ActivationExpired;
        case 4U:
            return RpcError::DeadlineExceeded;
        default:
            return RpcError::TransportFailure;
        }
    }

    RpcResult<std::optional<NativeResponse>> poll_response() {
        // This PendingResponse is the request identity.  Sidecar notifications
        // only wake this call; they never select or count native responses.
        if (!pending_.has_value()) {
            return RpcResult<std::optional<NativeResponse>>(RpcError::TransportFailure);
        }
        auto response = pending_->receive();
        if (!response.has_value()) {
            return RpcResult<std::optional<NativeResponse>>(
                detail::map_rpc_receive_error(response.error()));
        }
        auto optional_response = std::move(response).value();
        if (!optional_response.has_value()) {
            return RpcResult<std::optional<NativeResponse>>(
                std::optional<NativeResponse> {});
        }
        return RpcResult<std::optional<NativeResponse>>(
            std::optional<NativeResponse>(std::move(optional_response).value()));
    }

    RpcResult<std::optional<Guard>> finish_response(NativeResponse&& response) {
        if (!claim(1U)) {
            return RpcResult<std::optional<Guard>>(terminal_error());
        }
        pending_.reset();
        record_complete();
        return RpcResult<std::optional<Guard>>(
            std::optional<Guard>(Guard(std::move(response), client_state_, metrics_)));
    }

    // Node owner first: members are destroyed in reverse order, so the node
    // outlives the native objects below.
    std::shared_ptr<detail::ClientState> client_state_;
    std::optional<NativePending> pending_;
    std::optional<NativeResponse> pre_received_;
    detail::NativeListener response_listener_;
    detail::NativeListener cancellation_listener_;
    std::shared_ptr<detail::ClientMetricsState> metrics_;
    std::chrono::steady_clock::time_point started_at_;
    std::shared_ptr<std::atomic<std::uint8_t>> state_;
    std::shared_ptr<std::atomic<bool>> metrics_recorded_;

    void record_complete() noexcept {
        bool expected = false;
        if (metrics_ != nullptr
            && metrics_recorded_->compare_exchange_strong(expected,
                                                          true,
                                                          std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
            metrics_->complete(std::chrono::steady_clock::now() - started_at_);
        }
    }

    void record_terminal(RpcError error) noexcept {
        bool expected = false;
        if (metrics_ != nullptr
            && metrics_recorded_->compare_exchange_strong(expected,
                                                          true,
                                                          std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
            metrics_->finished(error, std::chrono::steady_clock::now() - started_at_);
        }
    }

    template <typename, typename, typename, typename>
    friend class RpcRequestLoan;
};

template <typename RequestPayload,
          typename ResponsePayload,
          typename RequestHeader,
          typename ResponseHeader>
class RpcRequestLoan {
    using NativeRequest =
        detail::NativeRequest<RequestPayload,
                              ResponsePayload,
                              RequestHeader,
                              ResponseHeader>;

  public:
    RpcRequestLoan(const RpcRequestLoan&) = delete;
    RpcRequestLoan& operator=(const RpcRequestLoan&) = delete;
    RpcRequestLoan(RpcRequestLoan&&) noexcept = default;
    RpcRequestLoan& operator=(RpcRequestLoan&& other) noexcept {
        if (this != &other) {
            RpcRequestLoan displaced(std::move(*this));
            client_ = std::move(other.client_);
            request_ = std::move(other.request_);
            (void)displaced;
        }
        return *this;
    }

    template <typename P = RequestPayload,
              typename std::enable_if<!detail::is_slice_payload<P>::value, int>::type = 0>
    [[nodiscard]] P& payload() {
        return request_->template payload_mut<P>();
    }

    template <typename P = RequestPayload,
              typename std::enable_if<!detail::is_slice_payload<P>::value, int>::type = 0>
    [[nodiscard]] const P& payload() const {
        return request_->template payload<P>();
    }

    template <typename P = RequestPayload,
              typename std::enable_if<detail::is_slice_payload<P>::value, int>::type = 0>
    [[nodiscard]] auto payload() {
        return request_->template payload_mut<P>();
    }

    template <typename P = RequestPayload,
              typename std::enable_if<detail::is_slice_payload<P>::value, int>::type = 0>
    [[nodiscard]] auto payload() const {
        return request_->template payload<P>();
    }

    template <typename H = RequestHeader,
              typename std::enable_if<!std::is_same<H, void>::value, int>::type = 0>
    [[nodiscard]] H& user_header() {
        return request_->template user_header_mut<H>();
    }

    template <typename H = RequestHeader,
              typename std::enable_if<!std::is_same<H, void>::value, int>::type = 0>
    [[nodiscard]] const H& user_header() const {
        return request_->template user_header<H>();
    }

    RpcResult<PendingRpcCall<RequestPayload,
                             ResponsePayload,
                             RequestHeader,
                             ResponseHeader>>
    send() && {
        using Pending = PendingRpcCall<RequestPayload,
                                       ResponsePayload,
                                       RequestHeader,
                                       ResponseHeader>;
        if (client_->metrics != nullptr) {
            client_->metrics->attempted();
        }
        if (client_->state->expired.load(std::memory_order_acquire)) {
            if (client_->metrics != nullptr) {
                client_->metrics->finished(RpcError::ActivationExpired,
                                           std::chrono::steady_clock::duration::zero());
            }
            return RpcResult<Pending>(RpcError::ActivationExpired);
        }

        auto response_factory = detail::open_event(
            client_->state->node->service_builder(client_->response_ready_name),
            client_->response_listener_capacity,
            1U,
            client_->event_max_nodes,
            false);
        if (!response_factory.has_value()) {
            if (client_->metrics != nullptr) {
                client_->metrics->finished(response_factory.error(),
                                           std::chrono::steady_clock::duration::zero());
            }
            return RpcResult<Pending>(response_factory.error());
        }
        auto response_listener_result =
            detail::create_call_listener(response_factory.value());
        if (!response_listener_result.has_value()) {
            if (client_->metrics != nullptr) {
                // Matches the native ExceedsMaxActiveRequests path below.
                if (response_listener_result.error() == RpcError::ClientCapacityReached) {
                    client_->metrics->capacity_rejected();
                } else {
                    client_->metrics->finished(response_listener_result.error(),
                                               std::chrono::steady_clock::duration::zero());
                }
            }
            return RpcResult<Pending>(response_listener_result.error());
        }

        auto cancellation_factory = detail::open_event(
            client_->state->node->service_builder(client_->client_cancel_name),
            client_->response_listener_capacity,
            1U,
            client_->event_max_nodes,
            false);
        if (!cancellation_factory.has_value()) {
            if (client_->metrics != nullptr) {
                client_->metrics->finished(cancellation_factory.error(),
                                           std::chrono::steady_clock::duration::zero());
            }
            return RpcResult<Pending>(cancellation_factory.error());
        }
        auto cancellation_listener_result =
            detail::create_call_listener(cancellation_factory.value());
        if (!cancellation_listener_result.has_value()) {
            if (client_->metrics != nullptr) {
                // Matches the native ExceedsMaxActiveRequests path below.
                if (cancellation_listener_result.error() == RpcError::ClientCapacityReached) {
                    client_->metrics->capacity_rejected();
                } else {
                    client_->metrics->finished(cancellation_listener_result.error(),
                                               std::chrono::steady_clock::duration::zero());
                }
            }
            return RpcResult<Pending>(cancellation_listener_result.error());
        }

        if (client_->state->expired.load(std::memory_order_acquire)) {
            if (client_->metrics != nullptr) {
                client_->metrics->finished(RpcError::ActivationExpired,
                                           std::chrono::steady_clock::duration::zero());
            }
            return RpcResult<Pending>(RpcError::ActivationExpired);
        }
        if (!request_.has_value()) {
            if (client_->metrics != nullptr) {
                client_->metrics->finished(RpcError::TransportFailure,
                                           std::chrono::steady_clock::duration::zero());
            }
            return RpcResult<Pending>(RpcError::TransportFailure);
        }

        auto request = std::move(*request_);
        request_.reset();
        RpcResult<detail::NativePending<RequestPayload,
                                        ResponsePayload,
                                        RequestHeader,
                                        ResponseHeader>>
            pending_result(RpcError::TransportFailure);
        {
            std::lock_guard<std::mutex> lock(client_->native_mutex);
            auto pending = iox2::send(std::move(request));
            if (!pending.has_value()) {
                if (client_->metrics != nullptr) {
                    const auto error = detail::map_request_send_error(pending.error());
                    if (error == RpcError::ClientCapacityReached) {
                        client_->metrics->capacity_rejected();
                    } else if (error == RpcError::ServerCapacityReached) {
                        client_->metrics->server_capacity_rejected();
                    } else {
                        client_->metrics->finished(error,
                                                   std::chrono::steady_clock::duration::zero());
                    }
                }
                return RpcResult<Pending>(
                    detail::map_request_send_error(pending.error()));
            }
            pending_result = RpcResult<detail::NativePending<RequestPayload,
                                                             ResponsePayload,
                                                             RequestHeader,
                                                             ResponseHeader>>(
                std::move(pending).value());
        }

        auto pending = std::move(pending_result).value();
        if (pending.number_of_server_connections() == 0U) {
            if (client_->metrics != nullptr) {
                client_->metrics->server_unavailable_failed();
            }
            return RpcResult<Pending>(RpcError::ServerUnavailable);
        }
        auto pre_received_result = pending.receive();
        if (!pre_received_result.has_value()) {
            if (client_->metrics != nullptr) {
                client_->metrics->response_receive_failed();
                client_->metrics->finished(detail::map_rpc_receive_error(pre_received_result.error()),
                                           std::chrono::steady_clock::duration::zero());
            }
            return RpcResult<Pending>(
                detail::map_rpc_receive_error(pre_received_result.error()));
        }
        std::optional<detail::NativeResponse<ResponsePayload, ResponseHeader>>
            pre_received;
        auto optional_response = std::move(pre_received_result).value();
        if (optional_response.has_value()) {
            pre_received.emplace(std::move(optional_response).value());
        }
        detail::notify(client_->request_notifier);
        if (client_->metrics != nullptr) {
            client_->metrics->started();
        }
        return RpcResult<Pending>(
            Pending(std::move(pending),
                    std::move(pre_received),
                    std::move(response_listener_result).value(),
                    std::move(cancellation_listener_result).value(),
                    client_->state,
                    client_->metrics));
    }

  private:
    RpcRequestLoan(NativeRequest&& request,
                   std::shared_ptr<detail::ClientInner<RequestPayload,
                                                       ResponsePayload,
                                                       RequestHeader,
                                                       ResponseHeader>>
                       client)
        : client_(std::move(client))
        , request_(std::in_place, std::move(request)) {
    }

    // Node owner first: members are destroyed in reverse order, so the node
    // outlives the native objects below.
    std::shared_ptr<detail::ClientInner<RequestPayload,
                                        ResponsePayload,
                                        RequestHeader,
                                        ResponseHeader>>
        client_;
    std::optional<NativeRequest> request_;

    template <typename, typename, typename, typename>
    friend class RpcClient;
};

template <typename RequestPayload,
          typename ResponsePayload,
          typename RequestHeader,
          typename ResponseHeader>
class RpcClient {
    using Inner = detail::ClientInner<RequestPayload,
                                      ResponsePayload,
                                      RequestHeader,
                                      ResponseHeader>;
    using Native = detail::NativeClient<RequestPayload,
                                        ResponsePayload,
                                        RequestHeader,
                                        ResponseHeader>;

  public:
    RpcClient() = default;
    RpcClient(const RpcClient&) = delete;
    RpcClient& operator=(const RpcClient&) = delete;
    RpcClient(RpcClient&&) noexcept = default;
    RpcClient& operator=(RpcClient&& other) noexcept {
        if (this != &other) {
            RpcClient displaced(std::move(*this));
            inner_ = std::move(other.inner_);
            (void)displaced;
        }
        return *this;
    }
    ~RpcClient() {
        if (inner_ != nullptr) {
            inner_->state->expire();
        }
    }

    static RpcResult<RpcClient> open(const RpcDescriptor& descriptor) {
        return open_impl(descriptor);
    }

    static RpcResult<RpcClient> connect(const RpcDescriptor& descriptor) {
        return open(descriptor);
    }

    static RpcResult<RpcClient> open_for_activation(
        const RpcDescriptor& descriptor,
        std::string capability_instance,
        std::uint64_t activation) {
        return open_impl(descriptor,
                         std::optional<std::string>(std::move(capability_instance)),
                         std::optional<std::uint64_t>(activation));
    }

    [[nodiscard]] RpcClientWakeHandle wake_handle() const {
        return RpcClientWakeHandle(inner_ == nullptr ? nullptr : inner_->state);
    }

    RpcResult<void> expire() const noexcept {
        if (inner_ == nullptr) {
            return RpcResult<void>(RpcError::TransportFailure);
        }
        inner_->state->expire();
        return RpcResult<void>();
    }

    RpcResult<void> shutdown() const noexcept {
        return expire();
    }

    template <typename P = RequestPayload,
              typename std::enable_if<!detail::is_slice_payload<P>::value, int>::type = 0>
    RpcResult<RpcRequestLoan<RequestPayload,
                             ResponsePayload,
                             RequestHeader,
                             ResponseHeader>>
    loan_request() {
        using Loan = RpcRequestLoan<RequestPayload,
                                    ResponsePayload,
                                    RequestHeader,
                                    ResponseHeader>;
        if (inner_ == nullptr) {
            return RpcResult<Loan>(RpcError::TransportFailure);
        }
        if (inner_->state->expired.load(std::memory_order_acquire)) {
            return RpcResult<Loan>(RpcError::ActivationExpired);
        }
        std::lock_guard<std::mutex> lock(inner_->native_mutex);
        auto result = inner_->native.loan();
        if (!result.has_value()) {
            return RpcResult<Loan>(detail::map_rpc_loan_error(result.error()));
        }
        return RpcResult<Loan>(
            Loan(std::move(result).value(), inner_));
    }

    template <typename P = RequestPayload,
              typename std::enable_if<detail::is_slice_payload<P>::value, int>::type = 0>
    RpcResult<RpcRequestLoan<RequestPayload,
                             ResponsePayload,
                             RequestHeader,
                             ResponseHeader>>
    loan_request_slice(std::size_t length) {
        using Loan = RpcRequestLoan<RequestPayload,
                                    ResponsePayload,
                                    RequestHeader,
                                    ResponseHeader>;
        if (inner_ == nullptr) {
            return RpcResult<Loan>(RpcError::TransportFailure);
        }
        if (length > inner_->descriptor.resources.max_request_slice_len) {
            return RpcResult<Loan>(RpcError::InvalidPayloadLength);
        }
        if (inner_->state->expired.load(std::memory_order_acquire)) {
            return RpcResult<Loan>(RpcError::ActivationExpired);
        }
        std::lock_guard<std::mutex> lock(inner_->native_mutex);
        auto result = inner_->native.loan_slice(
            static_cast<std::uint64_t>(length));
        if (!result.has_value()) {
            return RpcResult<Loan>(detail::map_rpc_loan_error(result.error()));
        }
        return RpcResult<Loan>(
            Loan(std::move(result).value(), inner_));
    }

    template <typename Rep, typename Period,
              typename P = RequestPayload,
              typename std::enable_if<!detail::is_slice_payload<P>::value, int>::type = 0>
    RpcResult<RpcResponseGuard<RequestPayload,
                               ResponsePayload,
                               RequestHeader,
                               ResponseHeader>>
    call_copy(const RequestPayload& request,
              const std::chrono::duration<Rep, Period>& timeout) {
        auto loan = loan_request();
        using Guard = RpcResponseGuard<RequestPayload,
                                       ResponsePayload,
                                       RequestHeader,
                                       ResponseHeader>;
        if (!loan.has_value()) {
            return RpcResult<Guard>(loan.error());
        }
        auto request_loan = std::move(loan).value();
        request_loan.payload() = request;
        auto pending = std::move(request_loan).send();
        if (!pending.has_value()) {
            return RpcResult<Guard>(pending.error());
        }
        return std::move(pending).value().wait(timeout);
    }

    template <typename Rep, typename Period,
              typename P = RequestPayload,
              typename std::enable_if<!detail::is_slice_payload<P>::value, int>::type = 0>
    RpcResult<RpcResponseGuard<RequestPayload,
                               ResponsePayload,
                               RequestHeader,
                               ResponseHeader>>
    call(const RequestPayload& request,
         const std::chrono::duration<Rep, Period>& timeout) {
        return call_copy(request, timeout);
    }

  private:
    static RpcResult<RpcClient> open_impl(
        const RpcDescriptor& descriptor,
        std::optional<std::string> capability_instance = std::nullopt,
        std::optional<std::uint64_t> activation = std::nullopt) {
        auto preflight =
            preflight_rpc<RequestPayload, ResponsePayload, RequestHeader, ResponseHeader>(
                descriptor);
        if (!preflight.has_value()) {
            return RpcResult<RpcClient>(preflight.error());
        }
        auto bounds = detail::validate_rpc_resources(descriptor.resources);
        if (!bounds.has_value()) {
            return RpcResult<RpcClient>(bounds.error());
        }
        auto name = detail::make_service_name(descriptor.service_name);
        auto response_name =
            detail::make_sidecar_name(descriptor, detail::kResponseReadySuffix);
        auto cancel_name =
            detail::make_sidecar_name(descriptor, detail::kClientCancelSuffix);
        auto request_name =
            detail::make_sidecar_name(descriptor, detail::kRequestReadySuffix);
        if (!name.has_value() || !response_name.has_value()
            || !cancel_name.has_value() || !request_name.has_value()) {
            return RpcResult<RpcClient>(RpcError::ServiceName);
        }

        auto node_result = iox2::NodeBuilder()
                                .signal_handling_mode(
                                    iox2::SignalHandlingMode::Disabled)
                                .create<detail::RpcIpcService>();
        if (!node_result.has_value()) {
            return RpcResult<RpcClient>(RpcError::Node);
        }
        auto node = std::move(node_result).value();
        auto request_event = detail::open_event(
            node.service_builder(request_name.value()),
            1U,
            descriptor.resources.max_clients,
            bounds.value().event_max_nodes,
            false);
        if (!request_event.has_value()) {
            return RpcResult<RpcClient>(request_event.error());
        }
        auto request_notifier = detail::create_notifier(request_event.value());
        if (!request_notifier.has_value()) {
            return RpcResult<RpcClient>(request_notifier.error());
        }
        auto response_event = detail::open_event(
            node.service_builder(response_name.value()),
            bounds.value().response_listener_capacity,
            1U,
            bounds.value().event_max_nodes,
            false);
        if (!response_event.has_value()) {
            return RpcResult<RpcClient>(response_event.error());
        }
        auto cancellation_event = detail::open_event(
            node.service_builder(cancel_name.value()),
            bounds.value().response_listener_capacity,
            descriptor.resources.max_clients,
            bounds.value().event_max_nodes,
            true);
        if (!cancellation_event.has_value()) {
            return RpcResult<RpcClient>(cancellation_event.error());
        }
        auto cancellation_notifier =
            detail::create_notifier(cancellation_event.value());
        if (!cancellation_notifier.has_value()) {
            return RpcResult<RpcClient>(cancellation_notifier.error());
        }

        auto factory = detail::open_rr<RequestPayload,
                                       ResponsePayload,
                                       RequestHeader,
                                       ResponseHeader>(
            node, name.value(), descriptor.resources, false);
        if (!factory.has_value()) {
            return RpcResult<RpcClient>(factory.error());
        }
        auto native = detail::create_native_client(factory.value(), descriptor.resources);
        if (!native.has_value()) {
            return RpcResult<RpcClient>(native.error());
        }

        auto state = std::make_shared<detail::ClientState>();
        state->node =
            std::make_shared<detail::NativeNode>(std::move(node));
        state->notifier = std::move(cancellation_notifier).value();
        auto metrics = std::make_shared<detail::ClientMetricsState>(
            descriptor.service_name,
            descriptor.resources,
            std::move(capability_instance),
            activation);
        register_rpc_pressure_source(metrics);
        auto inner = std::make_shared<Inner>(
            std::move(native).value(),
            descriptor,
            std::move(response_name).value(),
            std::move(cancel_name).value(),
            bounds.value().response_listener_capacity,
            bounds.value().event_max_nodes,
            std::move(request_notifier).value(),
            std::move(metrics),
            state);
        RpcClient client;
        client.inner_ = std::move(inner);
        return RpcResult<RpcClient>(std::move(client));
    }

    explicit RpcClient(std::shared_ptr<Inner> inner)
        : inner_(std::move(inner)) {
    }

    std::shared_ptr<Inner> inner_;

    template <typename, typename, typename, typename>
    friend class RpcRequestLoan;
};

template <typename RequestPayload,
          typename ResponsePayload,
          typename RequestHeader,
          typename ResponseHeader>
class RpcRequest {
    using Native = detail::NativeActive<RequestPayload,
                                        ResponsePayload,
                                        RequestHeader,
                                        ResponseHeader>;

  public:
    RpcRequest(const RpcRequest&) = delete;
    RpcRequest& operator=(const RpcRequest&) = delete;
    RpcRequest(RpcRequest&&) noexcept = default;
    RpcRequest& operator=(RpcRequest&& other) noexcept {
        if (this != &other) {
            RpcRequest displaced(std::move(*this));
            server_state_ = std::move(other.server_state_);
            request_ = std::move(other.request_);
            response_notifier_ = std::move(other.response_notifier_);
            metrics_ = std::move(other.metrics_);
            max_response_slice_len_ = other.max_response_slice_len_;
            (void)displaced;
        }
        return *this;
    }
    ~RpcRequest() {
        if (request_.has_value() && metrics_ != nullptr) {
            metrics_->request_released();
            detail::saturating_increment(metrics_->responses_abandoned);
        }
    }

    template <typename P = RequestPayload>
    [[nodiscard]] auto payload() const {
        return request_->template payload<P>();
    }

    template <typename H = RequestHeader,
              typename std::enable_if<!std::is_same<H, void>::value, int>::type = 0>
    [[nodiscard]] const H& user_header() const {
        return request_->template user_header<H>();
    }

    RpcResult<RpcResponseLoan<RequestPayload,
                              ResponsePayload,
                              RequestHeader,
                              ResponseHeader>>
    loan_response() {
        return loan_response_impl(max_response_slice_len_);
    }

    template <typename P = ResponsePayload,
              typename std::enable_if<detail::is_slice_payload<P>::value, int>::type = 0>
    RpcResult<RpcResponseLoan<RequestPayload,
                              ResponsePayload,
                              RequestHeader,
                              ResponseHeader>>
    loan_response_slice(std::size_t length) {
        if (length > max_response_slice_len_) {
            return RpcResult<RpcResponseLoan<RequestPayload,
                                             ResponsePayload,
                                             RequestHeader,
                                             ResponseHeader>>(
                RpcError::InvalidPayloadLength);
        }
        return loan_response_impl(length);
    }

  private:
    RpcResult<RpcResponseLoan<RequestPayload,
                              ResponsePayload,
                              RequestHeader,
                              ResponseHeader>>
    loan_response_impl(std::size_t response_slice_len) {
        using Loan = RpcResponseLoan<RequestPayload,
                                     ResponsePayload,
                                     RequestHeader,
                                     ResponseHeader>;
        if (server_state_->expired.load(std::memory_order_acquire)) {
            return RpcResult<Loan>(RpcError::ActivationExpired);
        }
        if (!request_.has_value()) {
            return RpcResult<Loan>(RpcError::TransportFailure);
        }
        if (metrics_ != nullptr) {
            detail::saturating_increment(metrics_->responses_attempted);
        }
        auto request = std::move(*request_);
        request_.reset();
        auto response = [&]() {
            if constexpr (detail::is_slice_payload<ResponsePayload>::value) {
                return request.template loan_slice(
                    static_cast<std::uint64_t>(response_slice_len));
            } else {
                return request.loan();
            }
        }();
        if (!response.has_value()) {
            if (metrics_ != nullptr) {
                metrics_->request_released();
                detail::saturating_increment(metrics_->response_loan_failures);
            }
            return RpcResult<Loan>(detail::map_rpc_loan_error(response.error()));
        }
        return RpcResult<Loan>(
            Loan(std::move(request),
                 std::move(response).value(),
                 response_notifier_,
                 server_state_,
                 metrics_));
    }

    RpcRequest(Native&& request,
               std::shared_ptr<detail::NativeNotifier> response_notifier,
               std::shared_ptr<detail::ServerState> server_state,
               std::shared_ptr<detail::ServerMetricsState> metrics,
               std::size_t max_response_slice_len)
        : server_state_(std::move(server_state))
        , request_(std::in_place, std::move(request))
        , response_notifier_(std::move(response_notifier))
        , metrics_(std::move(metrics))
        , max_response_slice_len_(max_response_slice_len) {
    }

    // Node owner first: members are destroyed in reverse order, so the node
    // outlives the native objects below.
    std::shared_ptr<detail::ServerState> server_state_;
    std::optional<Native> request_;
    std::shared_ptr<detail::NativeNotifier> response_notifier_;
    std::shared_ptr<detail::ServerMetricsState> metrics_;
    std::size_t max_response_slice_len_ { 1U };

    template <typename, typename, typename, typename>
    friend class RpcServer;
};

template <typename RequestPayload,
          typename ResponsePayload,
          typename RequestHeader,
          typename ResponseHeader>
class RpcResponseLoan {
    using NativeActive = detail::NativeActive<RequestPayload,
                                              ResponsePayload,
                                              RequestHeader,
                                              ResponseHeader>;
    using NativeResponse = detail::NativeResponseLoan<ResponsePayload,
                                                       ResponseHeader>;

  public:
    RpcResponseLoan(const RpcResponseLoan&) = delete;
    RpcResponseLoan& operator=(const RpcResponseLoan&) = delete;
    RpcResponseLoan(RpcResponseLoan&&) noexcept = default;
    RpcResponseLoan& operator=(RpcResponseLoan&& other) noexcept {
        if (this != &other) {
            RpcResponseLoan displaced(std::move(*this));
            server_state_ = std::move(other.server_state_);
            request_ = std::move(other.request_);
            response_ = std::move(other.response_);
            response_notifier_ = std::move(other.response_notifier_);
            metrics_ = std::move(other.metrics_);
            (void)displaced;
        }
        return *this;
    }
    ~RpcResponseLoan() {
        if (request_.has_value() && metrics_ != nullptr) {
            metrics_->request_released();
            detail::saturating_increment(metrics_->responses_abandoned);
        }
    }

    template <typename P = ResponsePayload,
              typename std::enable_if<!detail::is_slice_payload<P>::value, int>::type = 0>
    [[nodiscard]] P& payload() {
        return response_->template payload_mut<P>();
    }

    template <typename P = ResponsePayload,
              typename std::enable_if<detail::is_slice_payload<P>::value, int>::type = 0>
    [[nodiscard]] auto payload() {
        return response_->template payload_mut<P>();
    }

    template <typename P = ResponsePayload,
              typename std::enable_if<!detail::is_slice_payload<P>::value, int>::type = 0>
    [[nodiscard]] const P& payload() const {
        return response_->template payload<P>();
    }

    template <typename P = ResponsePayload,
              typename std::enable_if<detail::is_slice_payload<P>::value, int>::type = 0>
    [[nodiscard]] auto payload() const {
        return response_->template payload<P>();
    }

    template <typename H = ResponseHeader,
              typename std::enable_if<!std::is_same<H, void>::value, int>::type = 0>
    [[nodiscard]] H& user_header() {
        return response_->template user_header_mut<H>();
    }

    RpcResult<void> send() {
        if (server_state_->expired.load(std::memory_order_acquire)) {
            return RpcResult<void>(RpcError::ActivationExpired);
        }
        if (!response_.has_value()) {
            return RpcResult<void>(RpcError::TransportFailure);
        }
        auto response = std::move(*response_);
        response_.reset();
        auto result = iox2::send(std::move(response));
        request_.reset();
        if (!result.has_value()) {
            if (metrics_ != nullptr) {
                detail::saturating_increment(metrics_->responses_failed);
                metrics_->request_released();
            }
            return RpcResult<void>(detail::map_response_send_error(result.error()));
        }
        if (metrics_ != nullptr) {
            detail::saturating_increment(metrics_->responses_sent);
            metrics_->request_released();
        }
        detail::notify(response_notifier_);
        return RpcResult<void>();
    }

  private:
    RpcResponseLoan(
        NativeActive&& request,
        NativeResponse&& response,
        std::shared_ptr<detail::NativeNotifier> response_notifier,
        std::shared_ptr<detail::ServerState> server_state,
        std::shared_ptr<detail::ServerMetricsState> metrics)
        : server_state_(std::move(server_state))
        , request_(std::in_place, std::move(request))
        , response_(std::in_place, std::move(response))
        , response_notifier_(std::move(response_notifier))
        , metrics_(std::move(metrics)) {
    }

    // Node owner first: members are destroyed in reverse order, so the node
    // outlives the native objects below.
    std::shared_ptr<detail::ServerState> server_state_;
    std::optional<NativeActive> request_;
    std::optional<NativeResponse> response_;
    std::shared_ptr<detail::NativeNotifier> response_notifier_;
    std::shared_ptr<detail::ServerMetricsState> metrics_;

    template <typename, typename, typename, typename>
    friend class RpcRequest;
};

template <typename RequestPayload,
          typename ResponsePayload,
          typename RequestHeader,
          typename ResponseHeader>
class RpcServer {
    using Native = detail::NativeServer<RequestPayload,
                                        ResponsePayload,
                                        RequestHeader,
                                        ResponseHeader>;
    struct Impl {
        std::shared_ptr<detail::ServerState> state;
        std::shared_ptr<detail::ServerMetricsState> metrics;
        Native native;
        detail::NativeListener request_listener;
        detail::NativeListener cancellation_listener;
        std::shared_ptr<detail::NativeNotifier> response_notifier;

        Impl(Native&& endpoint,
             detail::NativeListener&& request_ready,
             detail::NativeListener&& cancellation,
             std::shared_ptr<detail::NativeNotifier> response_ready,
             std::shared_ptr<detail::ServerState> server_state,
             std::shared_ptr<detail::ServerMetricsState> server_metrics)
            : state(std::move(server_state))
            , metrics(std::move(server_metrics))
            , native(std::move(endpoint))
            , request_listener(std::move(request_ready))
            , cancellation_listener(std::move(cancellation))
            , response_notifier(std::move(response_ready)) {
        }
    };

  public:
    RpcServer() = default;
    RpcServer(const RpcServer&) = delete;
    RpcServer& operator=(const RpcServer&) = delete;
    RpcServer(RpcServer&&) noexcept = default;
    RpcServer& operator=(RpcServer&& other) noexcept {
        if (this != &other) {
            RpcServer displaced(std::move(*this));
            impl_ = std::move(other.impl_);
            (void)displaced;
        }
        return *this;
    }
    ~RpcServer() {
        if (impl_ != nullptr) {
            impl_->state->expire();
        }
    }

    static RpcResult<RpcServer> open_or_create(const RpcDescriptor& descriptor) {
        return open_impl(descriptor, true);
    }
    static RpcResult<RpcServer> open_or_create_for_capability(
        const RpcDescriptor& descriptor,
        std::string capability_instance) {
        return open_impl(descriptor,
                         true,
                         std::optional<std::string>(std::move(capability_instance)));
    }
    static RpcResult<RpcServer> create(const RpcDescriptor& descriptor) {
        return open_or_create(descriptor);
    }
    static RpcResult<RpcServer> open(const RpcDescriptor& descriptor) {
        return open_or_create(descriptor);
    }

    [[nodiscard]] RpcServerWakeHandle wake_handle() const {
        return RpcServerWakeHandle(impl_ == nullptr ? nullptr : impl_->state);
    }

    RpcResult<void> expire() const noexcept {
        if (impl_ == nullptr) {
            return RpcResult<void>(RpcError::TransportFailure);
        }
        impl_->state->expire();
        return RpcResult<void>();
    }
    RpcResult<void> shutdown() const noexcept {
        return expire();
    }

    RpcResult<std::optional<
        RpcRequest<RequestPayload, ResponsePayload, RequestHeader, ResponseHeader>>>
    try_receive() {
        using Request =
            RpcRequest<RequestPayload, ResponsePayload, RequestHeader, ResponseHeader>;
        if (impl_ == nullptr) {
            return RpcResult<std::optional<Request>>(RpcError::TransportFailure);
        }
        if (impl_->state->expired.load(std::memory_order_acquire)) {
            return RpcResult<std::optional<Request>>(
                RpcError::ActivationExpired);
        }
        auto result = impl_->native.receive();
        if (!result.has_value()) {
            if (impl_->metrics != nullptr) {
                detail::saturating_increment(impl_->metrics->request_borrow_failures);
            }
            return RpcResult<std::optional<Request>>(
                                detail::map_rpc_receive_error(result.error()));
        }
        auto optional_request = std::move(result).value();
        if (!optional_request.has_value()) {
            return RpcResult<std::optional<Request>>(std::optional<Request> {});
        }
        if (impl_->metrics != nullptr) {
            impl_->metrics->request_received();
        }
        return RpcResult<std::optional<Request>>(
            std::optional<Request>(Request(
                std::move(optional_request).value(),
                impl_->response_notifier,
                impl_->state,
                impl_->metrics,
                impl_->metrics == nullptr ? 1U : impl_->metrics->resources.max_response_slice_len)));
    }

    template <typename Rep, typename Period>
    RpcResult<std::optional<
        RpcRequest<RequestPayload, ResponsePayload, RequestHeader, ResponseHeader>>>
    receive_timeout(const std::chrono::duration<Rep, Period>& timeout) {
        using Request =
            RpcRequest<RequestPayload, ResponsePayload, RequestHeader, ResponseHeader>;
        const auto started = std::chrono::steady_clock::now();
        const auto duration = detail::to_steady_duration(timeout);
        for (;;) {
            auto request = try_receive();
            if (!request.has_value()) {
                return RpcResult<std::optional<Request>>(request.error());
            }
            if (request.value().has_value()) {
                return request;
            }
            const auto elapsed = std::chrono::steady_clock::now() - started;
            const auto remaining = elapsed >= duration
                                       ? std::chrono::steady_clock::duration::zero()
                                       : duration - elapsed;
            if (remaining == std::chrono::steady_clock::duration::zero()) {
                auto final_probe = try_receive();
                if (!final_probe.has_value()) {
                    return RpcResult<std::optional<Request>>(final_probe.error());
                }
                if (final_probe.value().has_value()) {
                    return final_probe;
                }
                return RpcResult<std::optional<Request>>(std::optional<Request> {});
            }

            auto waitset_result = iox2::WaitSetBuilder()
                                      .signal_handling_mode(
                                          iox2::SignalHandlingMode::Disabled)
                                      .create<detail::RpcIpcService>();
            if (!waitset_result.has_value()) {
                return RpcResult<std::optional<Request>>(
                    RpcError::TransportFailure);
            }
            auto waitset = std::move(waitset_result).value();
            auto request_guard =
                waitset.attach_notification(impl_->request_listener);
            auto cancellation_guard =
                waitset.attach_notification(impl_->cancellation_listener);
            auto interval_guard = waitset.attach_interval(
                iox2::bb::Duration::from_millis(
                    static_cast<std::uint64_t>(
                        detail::kRpcWaitInterval.count())));
            if (!request_guard.has_value() || !cancellation_guard.has_value()
                || !interval_guard.has_value()) {
                return RpcResult<std::optional<Request>>(
                    RpcError::TransportFailure);
            }
            auto request_attachment = std::move(request_guard).value();
            auto cancellation_attachment =
                std::move(cancellation_guard).value();
            auto interval_attachment = std::move(interval_guard).value();

            bool request_ready = false;
            bool cancelled = impl_->state->expired.load(
                std::memory_order_acquire);
            std::optional<RpcError> callback_error;
            auto callback = [&](iox2::WaitSetAttachmentId<detail::RpcIpcService> id) {
                if (id.has_event_from(cancellation_attachment)) {
                    auto event = detail::try_consume_activations(impl_->cancellation_listener);
                    if (!event.has_value()) {
                        callback_error = RpcError::TransportFailure;
                        return iox2::CallbackProgression::Stop;
                    }
                    if (impl_->state->expired.load(std::memory_order_acquire)) {
                        cancelled = true;
                        return iox2::CallbackProgression::Stop;
                    }
                    // The native state is authoritative; the sidecar only
                    // wakes this receive loop.
                    return iox2::CallbackProgression::Continue;
                }
                if (id.has_event_from(request_attachment)) {
                    auto event = detail::try_consume_activations(impl_->request_listener);
                    if (!event.has_value()) {
                        callback_error = RpcError::TransportFailure;
                        return iox2::CallbackProgression::Stop;
                    }
                    if (!event.value()) {
                        return iox2::CallbackProgression::Continue;
                    }
                    request_ready = true;
                    return iox2::CallbackProgression::Stop;
                }
                if (id.has_event_from(interval_attachment)) {
                    request_ready = true;
                    return iox2::CallbackProgression::Stop;
                }
                return iox2::CallbackProgression::Continue;
            };
            iox2::bb::StaticFunction<
                iox2::CallbackProgression(
                    iox2::WaitSetAttachmentId<detail::RpcIpcService>)>
                static_callback(callback);
            auto wait_result =
                waitset.wait_and_process_once_with_timeout(
                    static_callback,
                    iox2::bb::Duration::from_nanos(
                        static_cast<std::uint64_t>(
                            std::chrono::duration_cast<std::chrono::nanoseconds>(
                                remaining)
                                .count())));
            if (!wait_result.has_value()) {
                return RpcResult<std::optional<Request>>(
                    RpcError::TransportFailure);
            }
            if (callback_error.has_value()) {
                return RpcResult<std::optional<Request>>(*callback_error);
            }
            if (cancelled) {
                return RpcResult<std::optional<Request>>(
                    RpcError::ActivationExpired);
            }
            if (request_ready) {
                auto native_request = try_receive();
                if (!native_request.has_value()) {
                    return native_request;
                }
                if (native_request.value().has_value()) {
                    return native_request;
                }
            }
        }
    }

    RpcResult<RpcRequest<RequestPayload, ResponsePayload, RequestHeader, ResponseHeader>>
    receive() {
        for (;;) {
            auto result = receive_timeout(std::chrono::seconds(1));
            if (!result.has_value()) {
                return RpcResult<RpcRequest<RequestPayload,
                                            ResponsePayload,
                                            RequestHeader,
                                            ResponseHeader>>(result.error());
            }
            if (result.value().has_value()) {
                return RpcResult<RpcRequest<RequestPayload,
                                            ResponsePayload,
                                            RequestHeader,
                                            ResponseHeader>>(
                    std::move(result).value().value());
            }
        }
    }

  private:
    static RpcResult<RpcServer> open_impl(const RpcDescriptor& descriptor,
                                          bool create_if_missing,
                                          std::optional<std::string> capability_instance =
                                              std::nullopt) {
        auto preflight =
            preflight_rpc<RequestPayload, ResponsePayload, RequestHeader, ResponseHeader>(
                descriptor);
        if (!preflight.has_value()) {
            return RpcResult<RpcServer>(preflight.error());
        }
        auto bounds = detail::validate_rpc_resources(descriptor.resources);
        if (!bounds.has_value()) {
            return RpcResult<RpcServer>(bounds.error());
        }
        auto name = detail::make_service_name(descriptor.service_name);
        auto request_name =
            detail::make_sidecar_name(descriptor, detail::kRequestReadySuffix);
        auto response_name =
            detail::make_sidecar_name(descriptor, detail::kResponseReadySuffix);
        auto cancel_name =
            detail::make_sidecar_name(descriptor, detail::kServerCancelSuffix);
        if (!name.has_value() || !request_name.has_value()
            || !response_name.has_value() || !cancel_name.has_value()) {
            return RpcResult<RpcServer>(RpcError::ServiceName);
        }
        auto node_result = iox2::NodeBuilder()
                                .signal_handling_mode(
                                    iox2::SignalHandlingMode::Disabled)
                                .create<detail::RpcIpcService>();
        if (!node_result.has_value()) {
            return RpcResult<RpcServer>(RpcError::Node);
        }
        auto node = std::move(node_result).value();
        auto request_event = detail::open_event(
            node.service_builder(request_name.value()),
            1U,
            descriptor.resources.max_clients,
            bounds.value().event_max_nodes,
            true);
        if (!request_event.has_value()) {
            return RpcResult<RpcServer>(request_event.error());
        }
        auto request_listener = detail::create_listener(request_event.value());
        if (!request_listener.has_value()) {
            return RpcResult<RpcServer>(request_listener.error());
        }
        auto response_event = detail::open_event(
            node.service_builder(response_name.value()),
            bounds.value().response_listener_capacity,
            1U,
            bounds.value().event_max_nodes,
            true);
        if (!response_event.has_value()) {
            return RpcResult<RpcServer>(response_event.error());
        }
        auto response_notifier = detail::create_notifier(response_event.value());
        if (!response_notifier.has_value()) {
            return RpcResult<RpcServer>(response_notifier.error());
        }
        auto cancel_event = detail::open_event(
            node.service_builder(cancel_name.value()),
            1U,
            1U,
            bounds.value().event_max_nodes,
            true);
        if (!cancel_event.has_value()) {
            return RpcResult<RpcServer>(cancel_event.error());
        }
        auto cancellation_listener = detail::create_listener(cancel_event.value());
        if (!cancellation_listener.has_value()) {
            return RpcResult<RpcServer>(cancellation_listener.error());
        }
        auto cancellation_notifier = detail::create_notifier(cancel_event.value());
        if (!cancellation_notifier.has_value()) {
            return RpcResult<RpcServer>(cancellation_notifier.error());
        }
        auto factory = detail::open_rr<RequestPayload,
                                       ResponsePayload,
                                       RequestHeader,
                                       ResponseHeader>(
            node, name.value(), descriptor.resources, create_if_missing);
        if (!factory.has_value()) {
            return RpcResult<RpcServer>(factory.error());
        }
        auto native = detail::create_native_server(factory.value(),
                                                   descriptor.resources);
        if (!native.has_value()) {
            return RpcResult<RpcServer>(native.error());
        }
        auto state = std::make_shared<detail::ServerState>();
        state->node =
            std::make_shared<detail::NativeNode>(std::move(node));
        state->notifier = std::move(cancellation_notifier).value();
        auto metrics = std::make_shared<detail::ServerMetricsState>(
            descriptor.service_name,
            descriptor.resources,
            std::move(capability_instance));
        register_rpc_pressure_source(metrics);
        RpcServer server;
        server.impl_ = std::make_unique<Impl>(
            std::move(native).value(),
            std::move(request_listener).value(),
            std::move(cancellation_listener).value(),
            std::move(response_notifier).value(),
            state,
            std::move(metrics));
        return RpcResult<RpcServer>(std::move(server));
    }

    std::unique_ptr<Impl> impl_;
};

template <typename RequestPayload, typename ResponsePayload>
struct Rpc {
    [[nodiscard]] static RpcDescriptor descriptor(std::string_view service) {
        return RpcDescriptor::for_fixed<RequestPayload, ResponsePayload>(service);
    }

    [[nodiscard]] static RpcResult<
        RpcClient<RequestPayload, ResponsePayload>>
    client(const RpcDescriptor& endpoint) {
        return RpcClient<RequestPayload, ResponsePayload>::open(endpoint);
    }

    [[nodiscard]] static RpcResult<
        RpcServer<RequestPayload, ResponsePayload>>
    server(const RpcDescriptor& endpoint) {
        return RpcServer<RequestPayload, ResponsePayload>::open_or_create(endpoint);
    }
};

} // namespace rbt

#endif // RBT_CPP_RPC_HPP

#ifndef RBT_CPP_TF_TRANSPORT_HPP
#define RBT_CPP_TF_TRANSPORT_HPP

#include <iox2/iceoryx2.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rbt/rpc.hpp>
#include <rbt/stream.hpp>
#include <rbt/tf_wire.hpp>
#include <rbt/wait.hpp>

namespace rbt::tf {

/// Endpoint-construction and transport failures retain their native error
/// category. Transform validation failures continue to use TfError directly.
enum class TfTransportErrorKind : std::uint8_t {
    InvalidCapability,
    StaticCapabilityPairMismatch,
    InactiveEndpoint,
    Stream,
    Rpc,
};

class TfTransportError final : public std::runtime_error {
  public:
    explicit TfTransportError(TfTransportErrorKind kind)
        : std::runtime_error(message(kind)), kind_(kind) {
    }

    explicit TfTransportError(rbt::StreamError error)
        : std::runtime_error(rbt::to_string(error))
        , kind_(TfTransportErrorKind::Stream)
        , stream_error_(error) {
    }

    explicit TfTransportError(rbt::RpcError error)
        : std::runtime_error(rbt::to_string(error))
        , kind_(TfTransportErrorKind::Rpc)
        , rpc_error_(error) {
    }

    [[nodiscard]] TfTransportErrorKind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] std::optional<rbt::StreamError> stream_error() const noexcept {
        return stream_error_;
    }

    [[nodiscard]] std::optional<rbt::RpcError> rpc_error() const noexcept {
        return rpc_error_;
    }

  private:
    [[nodiscard]] static constexpr const char* message(TfTransportErrorKind kind) noexcept {
        switch (kind) {
        case TfTransportErrorKind::InvalidCapability:
            return "InvalidCapability";
        case TfTransportErrorKind::StaticCapabilityPairMismatch:
            return "StaticCapabilityPairMismatch";
        case TfTransportErrorKind::InactiveEndpoint:
            return "InactiveEndpoint";
        case TfTransportErrorKind::Stream:
            return "Stream";
        case TfTransportErrorKind::Rpc:
            return "Rpc";
        }
        return "InvalidCapability";
    }

    TfTransportErrorKind kind_;
    std::optional<rbt::StreamError> stream_error_;
    std::optional<rbt::RpcError> rpc_error_;
};

namespace transport_detail {

inline constexpr std::size_t kMaximumStaticTransforms = 4096U;

[[nodiscard]] inline std::array<std::uint8_t, 16> capability_bytes(std::string_view id) {
    const auto parsed = detail::parse_uuid(id);
    if (!parsed || !detail::nonzero(*parsed)) {
        throw TfTransportError(TfTransportErrorKind::InvalidCapability);
    }
    return *parsed;
}

inline void validate_capability_id(std::string_view id) {
    (void)capability_bytes(id);
}

[[nodiscard]] inline std::string endpoint_service(std::string_view capability,
                                                  std::string_view kind) {
    validate_capability_id(capability);
    return "rbt/cap/" + std::string(capability) + "/" + std::string(kind);
}

inline void validate_static_pair(std::string_view authority_id,
                                 std::string_view update_stream_id,
                                 std::string_view snapshot_rpc_id) {
    validate_capability_id(authority_id);
    validate_capability_id(update_stream_id);
    validate_capability_id(snapshot_rpc_id);
    // The static authority is the update-stream capability. Snapshot RPC is
    // separately identified so the two services cannot be paired implicitly.
    if (authority_id != update_stream_id || authority_id == snapshot_rpc_id) {
        throw TfTransportError(TfTransportErrorKind::StaticCapabilityPairMismatch);
    }
}

inline void throw_if_stream_error(rbt::StreamError error) {
    throw TfTransportError(error);
}

inline void throw_if_rpc_error(rbt::RpcError error) {
    throw TfTransportError(error);
}

[[nodiscard]] inline rbt::StreamDescriptor dynamic_stream_descriptor(
    std::string_view capability_id,
    rbt::StreamResourceConfig resources = rbt::StreamResourceConfig::defaults()) {
    const auto service = endpoint_service(capability_id, "stream");
    return rbt::StreamDescriptor(service,
                                 DYNAMIC_TRANSFORM_DESCRIPTOR,
                                 rbt::StreamProfile::Control,
                                 resources);
}

[[nodiscard]] inline rbt::StreamDescriptor static_update_stream_descriptor(
    std::string_view capability_id,
    rbt::StreamResourceConfig resources = rbt::StreamResourceConfig::defaults()) {
    const auto service = endpoint_service(capability_id, "stream");
    return rbt::StreamDescriptor(service,
                                 STATIC_UPDATE_DESCRIPTOR,
                                 rbt::StreamProfile::Control,
                                 resources);
}

[[nodiscard]] inline rbt::RpcDescriptor static_snapshot_rpc_descriptor(
    std::string_view capability_id,
    std::size_t max_transforms,
    rbt::RpcResourceConfig resources = rbt::RpcResourceConfig::defaults()) {
    if (max_transforms == 0U || max_transforms > kMaximumStaticTransforms
        || max_transforms > std::numeric_limits<std::uint32_t>::max()) {
        throw TfError(TfErrorCode::InvalidConfig);
    }
    resources = resources.with_max_response_slice_len(max_transforms);
    const auto service = endpoint_service(capability_id, "rpc");
    return rbt::RpcDescriptor::for_fixed_slice_with_resources<StaticSnapshotRequest,
                                                               StaticHeader,
                                                               StaticTransform>(service, resources);
}

[[nodiscard]] inline StaticSnapshotRequest snapshot_request(
    std::string_view authority_id,
    std::size_t max_transforms) {
    if (max_transforms > std::numeric_limits<std::uint32_t>::max()) {
        throw TfError(TfErrorCode::SnapshotTooLarge);
    }
    StaticSnapshotRequest request {};
    const auto bytes = capability_bytes(authority_id);
    std::copy(bytes.begin(), bytes.end(), std::begin(request.authority));
    request.max_transforms = static_cast<std::uint32_t>(max_transforms);
    return request;
}

inline void validate_static_set(const StaticSet& set, std::size_t maximum) {
    TfResourceConfig resources;
    resources.max_providers = 1U;
    resources.max_total_edges = maximum;
    resources.max_samples_per_edge = 1U;
    resources.max_total_samples = 1U;
    resources.max_static_transforms_per_provider = maximum;
    resources.max_pending_static_updates = 1U;
    TransformBuffer validator(resources);
    const auto authority = validator.bind_provider("tf_transport_validation",
                                                   "00000000-0000-0000-0000-000000000001");
    validator.apply_static_snapshot(authority, set);
}

inline void validate_static_transition(const StaticSet& current, const StaticSet& next) {
    if (current.revision == std::numeric_limits<std::uint64_t>::max()
        || next.revision != current.revision + 1U
        || next.effective_from.ns < current.effective_from.ns) {
        throw TfError(TfErrorCode::SnapshotRevisionGap);
    }
    if (current.effective_from.clock != next.effective_from.clock) {
        if (current.effective_from.clock.kind() != next.effective_from.clock.kind()
            || current.effective_from.clock.source_id() != next.effective_from.clock.source_id()) {
            throw TfError(rbt::time::TimeError::ClockDomainMismatch);
        }
        throw TfError(rbt::time::TimeError::ClockGenerationMismatch);
    }
}

[[nodiscard]] inline std::vector<StaticTransform> encode_static_set(const StaticSet& set) {
    std::vector<StaticTransform> encoded;
    encoded.reserve(set.transforms.size());
    for (const auto& edge : set.transforms) {
        encoded.push_back(encode_static_edge(edge));
    }
    return encoded;
}

} // namespace transport_detail

[[nodiscard]] inline rbt::StreamDescriptor dynamic_tf_stream_descriptor(
    std::string_view capability_id,
    rbt::StreamResourceConfig resources = rbt::StreamResourceConfig::defaults()) {
    return transport_detail::dynamic_stream_descriptor(capability_id, resources);
}

[[nodiscard]] inline rbt::StreamDescriptor static_tf_update_stream_descriptor(
    std::string_view capability_id,
    rbt::StreamResourceConfig resources = rbt::StreamResourceConfig::defaults()) {
    return transport_detail::static_update_stream_descriptor(capability_id, resources);
}

[[nodiscard]] inline rbt::RpcDescriptor static_tf_snapshot_rpc_descriptor(
    std::string_view capability_id,
    std::size_t max_transforms,
    rbt::RpcResourceConfig resources = rbt::RpcResourceConfig::defaults()) {
    return transport_detail::static_snapshot_rpc_descriptor(capability_id,
                                                             max_transforms,
                                                             resources);
}

using StaticSnapshotPayload = iox2::bb::Slice<StaticTransform>;
using StaticSnapshotClient = rbt::RpcClient<StaticSnapshotRequest,
                                             StaticSnapshotPayload,
                                             void,
                                             StaticHeader>;
using StaticSnapshotServer = rbt::RpcServer<StaticSnapshotRequest,
                                             StaticSnapshotPayload,
                                             void,
                                             StaticHeader>;

/// Publishes one fixed native dynamic-transform record per sample.
class DynamicTfPublisher {
  public:
    DynamicTfPublisher(const DynamicTfPublisher&) = delete;
    DynamicTfPublisher& operator=(const DynamicTfPublisher&) = delete;
    DynamicTfPublisher(DynamicTfPublisher&&) noexcept = default;
    DynamicTfPublisher& operator=(DynamicTfPublisher&&) noexcept = default;

    [[nodiscard]] static DynamicTfPublisher open(
        const rbt::CapabilityInstanceId& authority_id,
        rbt::StreamResourceConfig resources = rbt::StreamResourceConfig::defaults()) {
        auto descriptor = transport_detail::dynamic_stream_descriptor(authority_id, resources);
        auto publisher = rbt::FixedPublisher<DynamicTransform>::create(
            descriptor, DYNAMIC_TRANSFORM_DESCRIPTOR, resources);
        if (!publisher) {
            transport_detail::throw_if_stream_error(publisher.error());
        }
        return DynamicTfPublisher(authority_id, std::move(publisher).value());
    }

    void publish(const DynamicRecord& record) {
        if (!publisher_) {
            throw TfTransportError(TfTransportErrorKind::InactiveEndpoint);
        }
        const auto wire = encode_dynamic(authority_id_, record);
        auto loan = publisher_->loan();
        if (!loan) {
            transport_detail::throw_if_stream_error(loan.error());
        }
        auto sample = std::move(loan).value();
        sample.payload() = wire;
        auto sent = sample.send();
        if (!sent) {
            transport_detail::throw_if_stream_error(sent.error());
        }
    }

    [[nodiscard]] rbt::StreamPressure pressure() const {
        return publisher_ ? publisher_->pressure() : rbt::StreamPressure {};
    }

    void close() noexcept {
        publisher_.reset();
    }

  private:
    DynamicTfPublisher(rbt::CapabilityInstanceId authority_id,
                       rbt::FixedPublisher<DynamicTransform>&& publisher)
        : authority_id_(std::move(authority_id))
        , publisher_(std::in_place, std::move(publisher)) {
    }

    rbt::CapabilityInstanceId authority_id_;
    std::optional<rbt::FixedPublisher<DynamicTransform>> publisher_;
};

/// Copies bounded native stream records into the local TransformBuffer.
class DynamicTfSubscriber {
  public:
    DynamicTfSubscriber(const DynamicTfSubscriber&) = delete;
    DynamicTfSubscriber& operator=(const DynamicTfSubscriber&) = delete;
    DynamicTfSubscriber(DynamicTfSubscriber&& other) noexcept
        : authority_id_(std::move(other.authority_id_))
        , subscriber_(std::move(other.subscriber_))
        , buffer_(std::move(other.buffer_))
        , authority_(std::move(other.authority_)) {
        other.buffer_.reset();
        other.authority_.reset();
    }
    DynamicTfSubscriber& operator=(DynamicTfSubscriber&& other) noexcept {
        if (this != &other) {
            close();
            authority_id_ = std::move(other.authority_id_);
            subscriber_ = std::move(other.subscriber_);
            if (other.buffer_) {
                buffer_.emplace(std::move(*other.buffer_));
            }
            authority_ = std::move(other.authority_);
            other.buffer_.reset();
            other.authority_.reset();
        }
        return *this;
    }
    ~DynamicTfSubscriber() {
        close();
    }

    [[nodiscard]] static DynamicTfSubscriber open(
        const rbt::CapabilityInstanceId& authority_id,
        std::string_view slot,
        TransformBuffer& buffer,
        rbt::StreamResourceConfig resources = rbt::StreamResourceConfig::defaults()) {
        auto descriptor = transport_detail::dynamic_stream_descriptor(authority_id, resources);
        auto subscriber = rbt::FixedSubscriber<DynamicTransform>::create(
            descriptor, DYNAMIC_TRANSFORM_DESCRIPTOR, resources);
        if (!subscriber) {
            transport_detail::throw_if_stream_error(subscriber.error());
        }
        TransformBuffer owned_buffer = buffer;
        auto authority = owned_buffer.bind_provider(slot, authority_id);
        return DynamicTfSubscriber(authority_id,
                                   std::move(subscriber).value(),
                                   std::move(owned_buffer),
                                   std::move(authority));
    }

    [[nodiscard]] std::size_t drain(std::size_t maximum = rbt::MAX_DRAIN_PER_SOURCE) {
        if (!subscriber_ || !buffer_ || !authority_) {
            throw TfTransportError(TfTransportErrorKind::InactiveEndpoint);
        }
        if (maximum > rbt::MAX_DRAIN_PER_SOURCE) {
            throw TfError(TfErrorCode::InvalidConfig);
        }
        std::size_t drained = 0U;
        while (drained < maximum) {
            auto received = subscriber_->receive();
            if (!received) {
                transport_detail::throw_if_stream_error(received.error());
            }
            auto optional_sample = std::move(received).value();
            if (!optional_sample) {
                break;
            }
            DynamicTransform wire {};
            {
                auto sample = std::move(*optional_sample);
                wire = sample.payload();
            }
            auto record = decode_dynamic(authority_id_, wire);
            buffer_->ingest_dynamic(*authority_, std::move(record));
            ++drained;
        }
        return drained;
    }

    [[nodiscard]] rbt::EventSource event_source(rbt::WaitToken token) const& {
        if (!subscriber_) {
            throw TfTransportError(TfTransportErrorKind::InactiveEndpoint);
        }
        return subscriber_->event_source(token);
    }

    [[nodiscard]] rbt::StreamPressure pressure() const {
        return subscriber_ ? subscriber_->pressure() : rbt::StreamPressure {};
    }

    [[nodiscard]] const Authority& authority() const {
        if (!authority_) {
            throw TfTransportError(TfTransportErrorKind::InactiveEndpoint);
        }
        return *authority_;
    }

    void close() noexcept {
        subscriber_.reset();
        if (buffer_ && authority_) {
            try {
                buffer_->unbind_provider(*authority_);
            } catch (...) {
                // Destruction and explicit close are non-throwing; a stale
                // local binding means another owner already removed it.
            }
        }
        authority_.reset();
        buffer_.reset();
    }

  private:
    DynamicTfSubscriber(rbt::CapabilityInstanceId authority_id,
                        rbt::FixedSubscriber<DynamicTransform>&& subscriber,
                        TransformBuffer&& buffer,
                        Authority authority)
        : authority_id_(std::move(authority_id))
        , subscriber_(std::in_place, std::move(subscriber))
        , buffer_(std::in_place, std::move(buffer))
        , authority_(std::make_unique<Authority>(std::move(authority))) {
    }

    rbt::CapabilityInstanceId authority_id_;
    std::optional<rbt::FixedSubscriber<DynamicTransform>> subscriber_;
    std::optional<TransformBuffer> buffer_;
    std::unique_ptr<Authority> authority_;
};

/// Serves complete static snapshots and publishes atomic full-set updates.
class StaticTfProvider {
  public:
    StaticTfProvider(const StaticTfProvider&) = delete;
    StaticTfProvider& operator=(const StaticTfProvider&) = delete;
    StaticTfProvider(StaticTfProvider&&) noexcept = default;
    StaticTfProvider& operator=(StaticTfProvider&&) noexcept = default;

    [[nodiscard]] static StaticTfProvider open(
        const rbt::CapabilityInstanceId& authority_id,
        const rbt::CapabilityInstanceId& update_stream_id,
        const rbt::CapabilityInstanceId& snapshot_rpc_id,
        std::size_t max_transforms = 64U,
        rbt::StreamResourceConfig stream_resources = rbt::StreamResourceConfig::defaults(),
        rbt::RpcResourceConfig rpc_resources = rbt::RpcResourceConfig::defaults()) {
        transport_detail::validate_static_pair(authority_id, update_stream_id, snapshot_rpc_id);
        if (max_transforms == 0U || max_transforms > transport_detail::kMaximumStaticTransforms) {
            throw TfError(TfErrorCode::InvalidConfig);
        }
        auto update_descriptor = transport_detail::static_update_stream_descriptor(
            update_stream_id, stream_resources);
        auto update_publisher = rbt::SlicePublisher<StaticUpdateHeader, StaticTransform>::create(
            update_descriptor, STATIC_UPDATE_DESCRIPTOR, max_transforms, stream_resources);
        if (!update_publisher) {
            transport_detail::throw_if_stream_error(update_publisher.error());
        }
        auto rpc_descriptor = transport_detail::static_snapshot_rpc_descriptor(
            snapshot_rpc_id, max_transforms, rpc_resources);
        auto rpc_server = StaticSnapshotServer::open_or_create(rpc_descriptor);
        if (!rpc_server) {
            transport_detail::throw_if_rpc_error(rpc_server.error());
        }
        return StaticTfProvider(authority_id,
                                update_stream_id,
                                snapshot_rpc_id,
                                max_transforms,
                                std::move(update_publisher).value(),
                                std::move(rpc_server).value());
    }

    void replace(StaticSet set) {
        if (!update_publisher_) {
            throw TfTransportError(TfTransportErrorKind::InactiveEndpoint);
        }
        transport_detail::validate_static_set(set, max_transforms_);
        if (current_set_) {
            if (set == *current_set_) {
                return;
            }
            transport_detail::validate_static_transition(*current_set_, set);
        }
        auto encoded = transport_detail::encode_static_set(set);
        auto header = static_update_header(authority_id_, set);
        auto loan = update_publisher_->loan_slice(encoded.size());
        if (!loan) {
            transport_detail::throw_if_stream_error(loan.error());
        }
        auto sample = std::move(loan).value();
        sample.header() = header;
        std::copy(encoded.begin(), encoded.end(), sample.payload().begin());
        auto sent = sample.send();
        if (!sent) {
            transport_detail::throw_if_stream_error(sent.error());
        }
        current_set_ = std::move(set);
    }

    /// Services at most `maximum` native requests; callers may poll from their
    /// existing event loop without an endpoint-owned background thread.
    [[nodiscard]] std::size_t serve_requests(std::size_t maximum = rbt::MAX_DRAIN_PER_SOURCE) {
        if (!rpc_server_) {
            throw TfTransportError(TfTransportErrorKind::InactiveEndpoint);
        }
        if (maximum > rbt::MAX_DRAIN_PER_SOURCE) {
            throw TfError(TfErrorCode::InvalidConfig);
        }
        std::size_t served = 0U;
        while (served < maximum) {
            auto received = rpc_server_->try_receive();
            if (!received) {
                transport_detail::throw_if_rpc_error(received.error());
            }
            auto optional_request = std::move(received).value();
            if (!optional_request) {
                break;
            }
            auto request = std::move(*optional_request);
            const auto& payload = request.payload();
            const bool reserved_zero = std::all_of(std::begin(payload.reserved),
                                                   std::end(payload.reserved),
                                                   [](std::uint8_t byte) { return byte == 0U; });
            bool expected_authority = false;
            try {
                validate_wire_authority(authority_id_, payload.authority);
                expected_authority = true;
            } catch (const TfError&) {
                expected_authority = false;
            }

            const bool available = current_set_.has_value() && expected_authority && reserved_zero;
            const bool too_large = available
                                   && payload.max_transforms < current_set_->transforms.size();
            const std::uint32_t status = available
                                             ? (too_large ? kStaticStatusSnapshotTooLarge
                                                          : kStaticStatusSuccess)
                                             : kStaticStatusProviderUnavailable;
            const std::size_t count = status == kStaticStatusSuccess
                                          ? current_set_->transforms.size()
                                          : 0U;
            auto response = request.loan_response_slice(count);
            if (!response) {
                transport_detail::throw_if_rpc_error(response.error());
            }
            auto response_loan = std::move(response).value();
            auto& header = response_loan.user_header();
            std::memset(&header, 0, sizeof(header));
            const auto authority_bytes = transport_detail::capability_bytes(authority_id_);
            std::copy(authority_bytes.begin(), authority_bytes.end(), std::begin(header.authority));
            header.status = status;
            header.count = static_cast<std::uint32_t>(count);
            if (current_set_) {
                header.revision = current_set_->revision;
                header.effective_from = encode_timestamp(current_set_->effective_from,
                                                          current_set_->effective_uncertainty_ns);
            }
            if (count != 0U) {
                auto output = response_loan.payload();
                for (std::size_t index = 0U; index < count; ++index) {
                    output[index] = encode_static_edge(current_set_->transforms[index]);
                }
            }
            auto sent = response_loan.send();
            if (!sent) {
                transport_detail::throw_if_rpc_error(sent.error());
            }
            ++served;
        }
        return served;
    }

    [[nodiscard]] std::optional<std::uint64_t> current_revision() const noexcept {
        return current_set_ ? std::optional<std::uint64_t>(current_set_->revision) : std::nullopt;
    }

    [[nodiscard]] rbt::StreamPressure update_stream_pressure() const {
        return update_publisher_ ? update_publisher_->pressure() : rbt::StreamPressure {};
    }

    void close() noexcept {
        if (rpc_server_) {
            (void)rpc_server_->shutdown();
            rpc_server_.reset();
        }
        update_publisher_.reset();
        current_set_.reset();
    }

  private:
    StaticTfProvider(rbt::CapabilityInstanceId authority_id,
                     rbt::CapabilityInstanceId update_stream_id,
                     rbt::CapabilityInstanceId snapshot_rpc_id,
                     std::size_t max_transforms,
                     rbt::SlicePublisher<StaticUpdateHeader, StaticTransform>&& update_publisher,
                     StaticSnapshotServer&& rpc_server)
        : authority_id_(std::move(authority_id))
        , update_stream_id_(std::move(update_stream_id))
        , snapshot_rpc_id_(std::move(snapshot_rpc_id))
        , max_transforms_(max_transforms)
        , update_publisher_(std::in_place, std::move(update_publisher))
        , rpc_server_(std::in_place, std::move(rpc_server)) {
    }

    rbt::CapabilityInstanceId authority_id_;
    rbt::CapabilityInstanceId update_stream_id_;
    rbt::CapabilityInstanceId snapshot_rpc_id_;
    std::size_t max_transforms_ { 0U };
    std::optional<rbt::SlicePublisher<StaticUpdateHeader, StaticTransform>> update_publisher_;
    std::optional<StaticSnapshotServer> rpc_server_;
    std::optional<StaticSet> current_set_;
};

/// Receives full static updates and obtains recovery snapshots from the paired
/// RPC endpoint. Each loan is copied and released before local graph mutation.
class StaticTfClient {
  public:
    StaticTfClient(const StaticTfClient&) = delete;
    StaticTfClient& operator=(const StaticTfClient&) = delete;
    StaticTfClient(StaticTfClient&& other) noexcept
        : authority_id_(std::move(other.authority_id_))
        , update_stream_id_(std::move(other.update_stream_id_))
        , snapshot_rpc_id_(std::move(other.snapshot_rpc_id_))
        , max_transforms_(other.max_transforms_)
        , update_subscriber_(std::move(other.update_subscriber_))
        , rpc_client_(std::move(other.rpc_client_))
        , buffer_(std::move(other.buffer_))
        , authority_(std::move(other.authority_)) {
        other.buffer_.reset();
        other.authority_.reset();
    }
    StaticTfClient& operator=(StaticTfClient&& other) noexcept {
        if (this != &other) {
            close();
            authority_id_ = std::move(other.authority_id_);
            update_stream_id_ = std::move(other.update_stream_id_);
            snapshot_rpc_id_ = std::move(other.snapshot_rpc_id_);
            max_transforms_ = other.max_transforms_;
            update_subscriber_ = std::move(other.update_subscriber_);
            rpc_client_ = std::move(other.rpc_client_);
            if (other.buffer_) {
                buffer_.emplace(std::move(*other.buffer_));
            }
            authority_ = std::move(other.authority_);
            other.buffer_.reset();
            other.authority_.reset();
        }
        return *this;
    }
    ~StaticTfClient() {
        close();
    }

    [[nodiscard]] static StaticTfClient open(
        const rbt::CapabilityInstanceId& authority_id,
        const rbt::CapabilityInstanceId& update_stream_id,
        const rbt::CapabilityInstanceId& snapshot_rpc_id,
        std::string_view slot,
        TransformBuffer& buffer,
        std::size_t max_transforms = 64U,
        rbt::StreamResourceConfig stream_resources = rbt::StreamResourceConfig::defaults(),
        rbt::RpcResourceConfig rpc_resources = rbt::RpcResourceConfig::defaults()) {
        transport_detail::validate_static_pair(authority_id, update_stream_id, snapshot_rpc_id);
        if (max_transforms == 0U || max_transforms > transport_detail::kMaximumStaticTransforms) {
            throw TfError(TfErrorCode::InvalidConfig);
        }
        // The update listener is established before connecting to the snapshot
        // RPC so no update can race ahead of the local queue.
        auto update_descriptor = transport_detail::static_update_stream_descriptor(
            update_stream_id, stream_resources);
        auto update_subscriber = rbt::SliceSubscriber<StaticUpdateHeader, StaticTransform>::create(
            update_descriptor, STATIC_UPDATE_DESCRIPTOR, stream_resources);
        if (!update_subscriber) {
            transport_detail::throw_if_stream_error(update_subscriber.error());
        }
        auto rpc_descriptor = transport_detail::static_snapshot_rpc_descriptor(
            snapshot_rpc_id, max_transforms, rpc_resources);
        auto rpc_client = StaticSnapshotClient::open(rpc_descriptor);
        if (!rpc_client) {
            transport_detail::throw_if_rpc_error(rpc_client.error());
        }
        TransformBuffer owned_buffer = buffer;
        auto authority = owned_buffer.bind_provider(slot, authority_id);
        return StaticTfClient(authority_id,
                              update_stream_id,
                              snapshot_rpc_id,
                              max_transforms,
                              std::move(update_subscriber).value(),
                              std::move(rpc_client).value(),
                              std::move(owned_buffer),
                              std::move(authority));
    }

    void refresh_snapshot(std::chrono::nanoseconds timeout,
                          std::size_t requested_max_transforms = 0U) {
        ensure_active();
        const std::size_t requested = requested_max_transforms == 0U
                                          ? max_transforms_
                                          : requested_max_transforms;
        if (requested > max_transforms_) {
            throw TfError(TfErrorCode::SnapshotTooLarge);
        }
        try {
            const auto request = transport_detail::snapshot_request(authority_id_, requested);
            auto result = rpc_client_->call_copy(request, timeout);
            if (!result) {
                transport_detail::throw_if_rpc_error(result.error());
            }
            StaticSet snapshot;
            {
                auto response = std::move(result).value();
                const auto payload = response.payload();
                const auto elements = payload.number_of_elements();
                if (elements > max_transforms_) {
                    throw TfError(TfErrorCode::SnapshotTooLarge);
                }
                const std::span<const StaticTransform> wire(payload.data(),
                                                            static_cast<std::size_t>(elements));
                snapshot = decode_static_snapshot(authority_id_,
                                                  response.user_header(),
                                                  wire,
                                                  max_transforms_);
            }
            // The RPC response and update stream race by design. Release the RPC
            // guard above, stage or apply every already-queued complete update,
            // then let the buffer reconcile revisions atomically with this set.
            (void)drain_updates_bounded(rbt::MAX_DRAIN_PER_SOURCE);
            buffer_->apply_static_snapshot(*authority_, std::move(snapshot));
        } catch (...) {
            mark_snapshot_stale_noexcept();
            throw;
        }
    }

    [[nodiscard]] std::size_t drain_updates(
        std::size_t maximum = rbt::MAX_DRAIN_PER_SOURCE,
        std::chrono::nanoseconds resync_timeout = std::chrono::seconds(1)) {
        ensure_active();
        if (maximum > rbt::MAX_DRAIN_PER_SOURCE) {
            throw TfError(TfErrorCode::InvalidConfig);
        }
        const auto result = drain_updates_bounded(maximum);
        if (result.revision_gap) {
            (void)refresh_if_needed(resync_timeout);
        }
        return result.drained;
    }

    [[nodiscard]] bool refresh_if_needed(std::chrono::nanoseconds timeout) {
        ensure_active();
        if (!buffer_->needs_resync(*authority_)) {
            return false;
        }
        refresh_snapshot(timeout);
        return true;
    }

    [[nodiscard]] rbt::EventSource event_source(rbt::WaitToken token) const& {
        if (!update_subscriber_) {
            throw TfTransportError(TfTransportErrorKind::InactiveEndpoint);
        }
        return update_subscriber_->event_source(token);
    }

    [[nodiscard]] rbt::StreamPressure update_stream_pressure() const {
        return update_subscriber_ ? update_subscriber_->pressure() : rbt::StreamPressure {};
    }

    [[nodiscard]] const Authority& authority() const {
        if (!authority_) {
            throw TfTransportError(TfTransportErrorKind::InactiveEndpoint);
        }
        return *authority_;
    }

    void close() noexcept {
        if (rpc_client_) {
            (void)rpc_client_->shutdown();
            rpc_client_.reset();
        }
        update_subscriber_.reset();
        if (buffer_ && authority_) {
            try {
                buffer_->unbind_provider(*authority_);
            } catch (...) {
                // Explicit close is idempotent after another owner removes it.
            }
        }
        authority_.reset();
        buffer_.reset();
    }

  private:
    struct UpdateDrain {
        std::size_t drained { 0U };
        bool revision_gap { false };
    };

    void mark_snapshot_stale_noexcept() noexcept {
        if (!buffer_ || !authority_) {
            return;
        }
        try {
            buffer_->mark_snapshot_stale(*authority_);
        } catch (...) {
        }
    }

    [[nodiscard]] UpdateDrain drain_updates_bounded(std::size_t maximum) {
        UpdateDrain result;
        while (result.drained < maximum) {
            auto received = [&] {
                try {
                    return update_subscriber_->receive();
                } catch (...) {
                    mark_snapshot_stale_noexcept();
                    throw;
                }
            }();
            if (!received) {
                mark_snapshot_stale_noexcept();
                transport_detail::throw_if_stream_error(received.error());
            }
            auto optional_sample = std::move(received).value();
            if (!optional_sample) {
                break;
            }
            try {
                StaticUpdateHeader header {};
                std::vector<StaticTransform> wire_edges;
                {
                    auto sample = std::move(*optional_sample);
                    header = sample.header();
                    const auto payload = sample.payload();
                    if (payload.size() > max_transforms_) {
                        throw TfError(TfErrorCode::SnapshotTooLarge);
                    }
                    wire_edges.assign(payload.begin(), payload.end());
                }
                ++result.drained;
                auto update = decode_static_update(authority_id_,
                                                   header,
                                                   std::span<const StaticTransform>(wire_edges),
                                                   max_transforms_);
                buffer_->apply_static_update(*authority_, std::move(update));
            } catch (const TfError& error) {
                mark_snapshot_stale_noexcept();
                if (error.code() != TfErrorCode::SnapshotRevisionGap) {
                    throw;
                }
                result.revision_gap = true;
            } catch (...) {
                mark_snapshot_stale_noexcept();
                throw;
            }
        }
        return result;
    }

    StaticTfClient(rbt::CapabilityInstanceId authority_id,
                   rbt::CapabilityInstanceId update_stream_id,
                   rbt::CapabilityInstanceId snapshot_rpc_id,
                   std::size_t max_transforms,
                   rbt::SliceSubscriber<StaticUpdateHeader, StaticTransform>&& update_subscriber,
                   StaticSnapshotClient&& rpc_client,
                   TransformBuffer&& buffer,
                   Authority authority)
        : authority_id_(std::move(authority_id))
        , update_stream_id_(std::move(update_stream_id))
        , snapshot_rpc_id_(std::move(snapshot_rpc_id))
        , max_transforms_(max_transforms)
        , update_subscriber_(std::in_place, std::move(update_subscriber))
        , rpc_client_(std::in_place, std::move(rpc_client))
        , buffer_(std::in_place, std::move(buffer))
        , authority_(std::make_unique<Authority>(std::move(authority))) {
    }

    void ensure_active() const {
        if (!update_subscriber_ || !rpc_client_ || !buffer_ || !authority_) {
            throw TfTransportError(TfTransportErrorKind::InactiveEndpoint);
        }
    }

    rbt::CapabilityInstanceId authority_id_;
    rbt::CapabilityInstanceId update_stream_id_;
    rbt::CapabilityInstanceId snapshot_rpc_id_;
    std::size_t max_transforms_ { 0U };
    std::optional<rbt::SliceSubscriber<StaticUpdateHeader, StaticTransform>> update_subscriber_;
    std::optional<StaticSnapshotClient> rpc_client_;
    std::optional<TransformBuffer> buffer_;
    std::unique_ptr<Authority> authority_;
};

} // namespace rbt::tf

#endif // RBT_CPP_TF_TRANSPORT_HPP

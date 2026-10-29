#ifndef RBT_CPP_TF_COMPONENT_HPP
#define RBT_CPP_TF_COMPONENT_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <rbt/runtime.hpp>
#include <rbt/tf_transport.hpp>

namespace rbt::tf {

namespace component_detail {

[[noreturn]] inline void invalid_capability() {
    throw TfTransportError(TfTransportErrorKind::InvalidCapability);
}

template <typename Capability>
[[nodiscard]] inline const CapabilityDescriptor& validate_handle(
    const rbt::CapabilityHandle<Capability>& handle) {
    if (!handle) {
        invalid_capability();
    }
    const auto& descriptor = handle.binding().descriptor;
    if (handle.provider() != descriptor.provider_component) {
        invalid_capability();
    }
    return descriptor;
}

template <typename Capability>
[[nodiscard]] inline const OwnedStreamDescriptor& validate_stream_handle(
    const rbt::CapabilityHandle<Capability>& handle,
    const rbt::StreamDescriptor& expected) {
    const auto& binding = validate_handle(handle);
    const auto* actual = handle.try_stream();
    if (actual == nullptr) {
        invalid_capability();
    }
    const auto wire = OwnedStreamDescriptor::from(expected);
    if (actual->service_name != wire.service_name
        || !actual->message_type.wire_equal(wire.message_type)
        || actual->profile != wire.profile || actual->transport != wire.transport
        || actual->resources != wire.resources
        || binding.capability_instance.empty()) {
        invalid_capability();
    }
    return *actual;
}

template <typename Capability>
[[nodiscard]] inline const OwnedRpcDescriptor& validate_rpc_handle(
    const rbt::CapabilityHandle<Capability>& handle,
    const rbt::RpcDescriptor& expected) {
    const auto& binding = validate_handle(handle);
    const auto* actual = handle.try_rpc();
    if (actual == nullptr) {
        invalid_capability();
    }
    const auto wire = OwnedRpcDescriptor::from(expected);
    if (actual->service_name != wire.service_name
        || !actual->request_type.wire_equal(wire.request_type)
        || !actual->response_type.wire_equal(wire.response_type)
        || actual->transport != wire.transport || actual->resources != wire.resources
        || binding.capability_instance.empty()) {
        invalid_capability();
    }
    return *actual;
}

template <typename Endpoint>
inline void close_on_activation_shutdown(rbt::ActivationContext& context,
                                         const std::shared_ptr<Endpoint>& endpoint) {
    if (context.scope().is_shutdown()) {
        throw TfTransportError(TfTransportErrorKind::InactiveEndpoint);
    }
    context.scope().defer_sync([endpoint] { endpoint->close(); });
}

inline void validate_same_provider(const rbt::CapabilityDescriptor& first,
                                   const rbt::CapabilityDescriptor& second) {
    if (first.provider_component != second.provider_component
        || first.provider_process != second.provider_process
        || first.activation != second.activation) {
        throw TfTransportError(TfTransportErrorKind::StaticCapabilityPairMismatch);
    }
}

} // namespace component_detail

/// An activation-owned dynamic publisher paired with its advertised capability.
template <typename Capability>
class ScopedDynamicTfPublisher {
  public:
    ScopedDynamicTfPublisher(const ScopedDynamicTfPublisher&) = delete;
    ScopedDynamicTfPublisher& operator=(const ScopedDynamicTfPublisher&) = delete;
    ScopedDynamicTfPublisher(ScopedDynamicTfPublisher&&) noexcept = default;
    ScopedDynamicTfPublisher& operator=(ScopedDynamicTfPublisher&&) noexcept = default;

    [[nodiscard]] const rbt::CapabilityHandle<Capability>& capability() const noexcept {
        return capability_;
    }

    void publish(const DynamicRecord& record) {
        active_endpoint().publish(record);
    }

    [[nodiscard]] rbt::StreamPressure pressure() const {
        return active_endpoint().pressure();
    }

    void close() noexcept {
        if (endpoint_) {
            endpoint_->close();
        }
    }

  private:
    [[nodiscard]] DynamicTfPublisher& active_endpoint() const {
        if (!endpoint_) {
            throw TfTransportError(TfTransportErrorKind::InactiveEndpoint);
        }
        return *endpoint_;
    }

    ScopedDynamicTfPublisher(rbt::CapabilityHandle<Capability> capability,
                             std::shared_ptr<DynamicTfPublisher> endpoint)
        : capability_(std::move(capability)), endpoint_(std::move(endpoint)) {
    }

    rbt::CapabilityHandle<Capability> capability_;
    std::shared_ptr<DynamicTfPublisher> endpoint_;

    template <typename T>
    friend ScopedDynamicTfPublisher<T> provide_dynamic_tf(
        rbt::ActivationContext&, rbt::StreamResourceConfig);
};

/// Opens and validates the native publisher before staging its capability.
template <typename Capability>
[[nodiscard]] inline ScopedDynamicTfPublisher<Capability> provide_dynamic_tf(
    rbt::ActivationContext& context,
    rbt::StreamResourceConfig resources = rbt::StreamResourceConfig::defaults()) {
    const auto capability_id = rbt::generate_uuid();
    auto endpoint = std::make_shared<DynamicTfPublisher>(
        DynamicTfPublisher::open(capability_id, resources));
    component_detail::close_on_activation_shutdown(context, endpoint);
    const auto descriptor = OwnedStreamDescriptor::from(
        dynamic_tf_stream_descriptor(capability_id, resources));
    auto capability = context.provide<Capability>(capability_id, descriptor);
    return ScopedDynamicTfPublisher<Capability>(std::move(capability), std::move(endpoint));
}

/// An activation-owned pair of static-update and snapshot-RPC publications.
template <typename UpdateCapability, typename SnapshotCapability>
class ScopedStaticTfProvider {
  public:
    ScopedStaticTfProvider(const ScopedStaticTfProvider&) = delete;
    ScopedStaticTfProvider& operator=(const ScopedStaticTfProvider&) = delete;
    ScopedStaticTfProvider(ScopedStaticTfProvider&&) noexcept = default;
    ScopedStaticTfProvider& operator=(ScopedStaticTfProvider&&) noexcept = default;

    [[nodiscard]] const rbt::CapabilityHandle<UpdateCapability>& update_capability() const noexcept {
        return update_capability_;
    }

    [[nodiscard]] const rbt::CapabilityHandle<SnapshotCapability>& snapshot_capability() const noexcept {
        return snapshot_capability_;
    }

    void replace(StaticSet set) {
        active_endpoint().replace(std::move(set));
    }

    [[nodiscard]] std::size_t serve_requests(std::size_t maximum = rbt::MAX_DRAIN_PER_SOURCE) {
        return active_endpoint().serve_requests(maximum);
    }

    [[nodiscard]] std::optional<std::uint64_t> current_revision() const noexcept {
        return endpoint_ ? endpoint_->current_revision() : std::nullopt;
    }

    void close() noexcept {
        if (endpoint_) {
            endpoint_->close();
        }
    }

  private:
    [[nodiscard]] StaticTfProvider& active_endpoint() const {
        if (!endpoint_) {
            throw TfTransportError(TfTransportErrorKind::InactiveEndpoint);
        }
        return *endpoint_;
    }

    ScopedStaticTfProvider(rbt::CapabilityHandle<UpdateCapability> update_capability,
                           rbt::CapabilityHandle<SnapshotCapability> snapshot_capability,
                           std::shared_ptr<StaticTfProvider> endpoint)
        : update_capability_(std::move(update_capability))
        , snapshot_capability_(std::move(snapshot_capability))
        , endpoint_(std::move(endpoint)) {
    }

    rbt::CapabilityHandle<UpdateCapability> update_capability_;
    rbt::CapabilityHandle<SnapshotCapability> snapshot_capability_;
    std::shared_ptr<StaticTfProvider> endpoint_;

    template <typename Update, typename Snapshot>
    friend ScopedStaticTfProvider<Update, Snapshot> provide_static_tf(
        rbt::ActivationContext&,
        std::size_t,
        rbt::StreamResourceConfig,
        rbt::RpcResourceConfig);
};

/// Opens both native endpoints before publishing their independent roles.
template <typename UpdateCapability, typename SnapshotCapability>
[[nodiscard]] inline ScopedStaticTfProvider<UpdateCapability, SnapshotCapability> provide_static_tf(
    rbt::ActivationContext& context,
    std::size_t max_transforms = 64U,
    rbt::StreamResourceConfig stream_resources = rbt::StreamResourceConfig::defaults(),
    rbt::RpcResourceConfig rpc_resources = rbt::RpcResourceConfig::defaults()) {
    const auto update_id = rbt::generate_uuid();
    auto snapshot_rpc_id = rbt::generate_uuid();
    while (snapshot_rpc_id == update_id) {
        snapshot_rpc_id = rbt::generate_uuid();
    }
    auto endpoint = std::make_shared<StaticTfProvider>(StaticTfProvider::open(
        update_id, update_id, snapshot_rpc_id, max_transforms, stream_resources, rpc_resources));
    component_detail::close_on_activation_shutdown(context, endpoint);

    const auto update_descriptor = OwnedStreamDescriptor::from(
        static_tf_update_stream_descriptor(update_id, stream_resources));
    const auto snapshot_descriptor = OwnedRpcDescriptor::from(
        static_tf_snapshot_rpc_descriptor(snapshot_rpc_id, max_transforms, rpc_resources));
    auto update_capability = context.provide<UpdateCapability>(update_id, update_descriptor);
    auto snapshot_capability = context.provide_rpc<SnapshotCapability>(snapshot_rpc_id,
                                                                        snapshot_descriptor);
    return ScopedStaticTfProvider<UpdateCapability, SnapshotCapability>(
        std::move(update_capability), std::move(snapshot_capability), std::move(endpoint));
}

/// An activation-owned dynamic subscriber that copies samples into its buffer.
template <typename Capability>
class ScopedDynamicTfSubscriber {
  public:
    ScopedDynamicTfSubscriber(const ScopedDynamicTfSubscriber&) = delete;
    ScopedDynamicTfSubscriber& operator=(const ScopedDynamicTfSubscriber&) = delete;
    ScopedDynamicTfSubscriber(ScopedDynamicTfSubscriber&&) noexcept = default;
    ScopedDynamicTfSubscriber& operator=(ScopedDynamicTfSubscriber&&) noexcept = default;

    [[nodiscard]] const rbt::CapabilityHandle<Capability>& capability() const noexcept {
        return capability_;
    }

    [[nodiscard]] std::size_t drain(std::size_t maximum = rbt::MAX_DRAIN_PER_SOURCE) {
        return active_endpoint().drain(maximum);
    }

    [[nodiscard]] rbt::EventSource event_source(rbt::WaitToken token) const& {
        return active_endpoint().event_source(token);
    }

    [[nodiscard]] const Authority& authority() const {
        return active_endpoint().authority();
    }

    void close() noexcept {
        if (endpoint_) {
            endpoint_->close();
        }
    }

  private:
    [[nodiscard]] DynamicTfSubscriber& active_endpoint() const {
        if (!endpoint_) {
            throw TfTransportError(TfTransportErrorKind::InactiveEndpoint);
        }
        return *endpoint_;
    }

    ScopedDynamicTfSubscriber(rbt::CapabilityHandle<Capability> capability,
                              std::shared_ptr<DynamicTfSubscriber> endpoint)
        : capability_(std::move(capability)), endpoint_(std::move(endpoint)) {
    }

    rbt::CapabilityHandle<Capability> capability_;
    std::shared_ptr<DynamicTfSubscriber> endpoint_;

    template <typename T>
    friend ScopedDynamicTfSubscriber<T> require_dynamic_tf(
        rbt::ActivationContext&,
        std::string_view,
        std::string_view,
        TransformBuffer&,
        rbt::StreamResourceConfig);
};

/// Resolves a stream role, checks its physical ID and generated ABI, then binds it.
/// The endpoint owns a buffer copy that shares the supplied buffer's graph state.
template <typename Capability>
[[nodiscard]] inline ScopedDynamicTfSubscriber<Capability> require_dynamic_tf(
    rbt::ActivationContext& context,
    std::string_view requirement_name,
    std::string_view slot,
    TransformBuffer& buffer,
    rbt::StreamResourceConfig resources = rbt::StreamResourceConfig::defaults()) {
    const auto capability = context.require<Capability>(requirement_name);
    const auto& descriptor = component_detail::validate_handle(capability);
    const auto expected = dynamic_tf_stream_descriptor(descriptor.capability_instance, resources);
    (void)component_detail::validate_stream_handle(capability, expected);
    auto endpoint = std::make_shared<DynamicTfSubscriber>(DynamicTfSubscriber::open(
        descriptor.capability_instance, slot, buffer, resources));
    component_detail::close_on_activation_shutdown(context, endpoint);
    return ScopedDynamicTfSubscriber<Capability>(capability, std::move(endpoint));
}

/// An activation-owned static update subscriber and paired snapshot client.
template <typename UpdateCapability, typename SnapshotCapability>
class ScopedStaticTfClient {
  public:
    ScopedStaticTfClient(const ScopedStaticTfClient&) = delete;
    ScopedStaticTfClient& operator=(const ScopedStaticTfClient&) = delete;
    ScopedStaticTfClient(ScopedStaticTfClient&&) noexcept = default;
    ScopedStaticTfClient& operator=(ScopedStaticTfClient&&) noexcept = default;

    [[nodiscard]] const rbt::CapabilityHandle<UpdateCapability>& update_capability() const noexcept {
        return update_capability_;
    }

    [[nodiscard]] const rbt::CapabilityHandle<SnapshotCapability>& snapshot_capability() const noexcept {
        return snapshot_capability_;
    }

    void refresh_snapshot(std::chrono::nanoseconds timeout,
                          std::size_t requested_max_transforms = 0U) {
        active_endpoint().refresh_snapshot(timeout, requested_max_transforms);
    }

    [[nodiscard]] std::size_t drain_updates(
        std::size_t maximum = rbt::MAX_DRAIN_PER_SOURCE,
        std::chrono::nanoseconds resync_timeout = std::chrono::seconds(1)) {
        return active_endpoint().drain_updates(maximum, resync_timeout);
    }

    [[nodiscard]] bool refresh_if_needed(std::chrono::nanoseconds timeout) {
        return active_endpoint().refresh_if_needed(timeout);
    }

    [[nodiscard]] rbt::EventSource event_source(rbt::WaitToken token) const& {
        return active_endpoint().event_source(token);
    }

    [[nodiscard]] const Authority& authority() const {
        return active_endpoint().authority();
    }

    void close() noexcept {
        if (endpoint_) {
            endpoint_->close();
        }
    }

  private:
    [[nodiscard]] StaticTfClient& active_endpoint() const {
        if (!endpoint_) {
            throw TfTransportError(TfTransportErrorKind::InactiveEndpoint);
        }
        return *endpoint_;
    }

    ScopedStaticTfClient(rbt::CapabilityHandle<UpdateCapability> update_capability,
                         rbt::CapabilityHandle<SnapshotCapability> snapshot_capability,
                         std::shared_ptr<StaticTfClient> endpoint)
        : update_capability_(std::move(update_capability))
        , snapshot_capability_(std::move(snapshot_capability))
        , endpoint_(std::move(endpoint)) {
    }

    rbt::CapabilityHandle<UpdateCapability> update_capability_;
    rbt::CapabilityHandle<SnapshotCapability> snapshot_capability_;
    std::shared_ptr<StaticTfClient> endpoint_;

    template <typename Update, typename Snapshot>
    friend ScopedStaticTfClient<Update, Snapshot> require_static_tf(
        rbt::ActivationContext&,
        std::string_view,
        std::string_view,
        std::string_view,
        TransformBuffer&,
        std::size_t,
        rbt::StreamResourceConfig,
        rbt::RpcResourceConfig);
};

/// Resolves and verifies both static roles before binding the update authority.
/// The endpoint owns a buffer copy that shares the supplied buffer's graph state.
template <typename UpdateCapability, typename SnapshotCapability>
[[nodiscard]] inline ScopedStaticTfClient<UpdateCapability, SnapshotCapability> require_static_tf(
    rbt::ActivationContext& context,
    std::string_view update_requirement,
    std::string_view snapshot_requirement,
    std::string_view slot,
    TransformBuffer& buffer,
    std::size_t max_transforms = 64U,
    rbt::StreamResourceConfig stream_resources = rbt::StreamResourceConfig::defaults(),
    rbt::RpcResourceConfig rpc_resources = rbt::RpcResourceConfig::defaults()) {
    const auto update = context.require<UpdateCapability>(update_requirement);
    const auto snapshot = context.require<SnapshotCapability>(snapshot_requirement);
    const auto& update_descriptor = component_detail::validate_handle(update);
    const auto& snapshot_descriptor = component_detail::validate_handle(snapshot);
    component_detail::validate_same_provider(update_descriptor, snapshot_descriptor);
    const auto expected_update = static_tf_update_stream_descriptor(
        update_descriptor.capability_instance, stream_resources);
    const auto expected_snapshot = static_tf_snapshot_rpc_descriptor(
        snapshot_descriptor.capability_instance, max_transforms, rpc_resources);
    (void)component_detail::validate_stream_handle(update, expected_update);
    (void)component_detail::validate_rpc_handle(snapshot, expected_snapshot);
    auto endpoint = std::make_shared<StaticTfClient>(StaticTfClient::open(
        update_descriptor.capability_instance,
        update_descriptor.capability_instance,
        snapshot_descriptor.capability_instance,
        slot,
        buffer,
        max_transforms,
        stream_resources,
        rpc_resources));
    component_detail::close_on_activation_shutdown(context, endpoint);
    return ScopedStaticTfClient<UpdateCapability, SnapshotCapability>(
        update, snapshot, std::move(endpoint));
}

} // namespace rbt::tf

#endif // RBT_CPP_TF_COMPONENT_HPP

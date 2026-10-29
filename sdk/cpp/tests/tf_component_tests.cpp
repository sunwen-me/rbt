#ifdef NDEBUG
#undef NDEBUG
#endif

#include <rbt/tf_component.hpp>

#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct DynamicCapability {
    static constexpr std::string_view TYPE_NAME { "TfDynamic" };
};

struct StaticUpdateCapability {
    static constexpr std::string_view TYPE_NAME { "TfStaticUpdate" };
};

struct StaticSnapshotCapability {
    static constexpr std::string_view TYPE_NAME { "TfStaticSnapshot" };
};

rbt::time::ClockIdentity test_clock() {
    std::array<std::uint8_t, 48U> source_bytes {};
    source_bytes[0] = 0x54U;
    auto result = rbt::time::ClockIdentity::create(
        rbt::time::ClockKind::HostMonotonic,
        rbt::time::ClockSourceId(std::move(source_bytes)),
        0U);
    assert(result);
    return std::move(result).value();
}

void dynamic_helpers_bind_copy_and_expire_endpoints() {
    const auto clock = test_clock();
    rbt::EffectScope provider_scope("dynamic-provider-process", 11U);
    rbt::ActivationContext provider_context("dynamic_provider",
                                            "dynamic-provider-process",
                                            11U,
                                            {},
                                            provider_scope);
    auto publisher = rbt::tf::provide_dynamic_tf<DynamicCapability>(provider_context);
    const auto& provided_stream = publisher.capability().stream();
    assert(provided_stream.service_name
           == "rbt/cap/" + publisher.capability().capability_instance() + "/stream");

    auto binding = publisher.capability().binding();
    binding.requirement_name = "dynamic";
    rbt::EffectScope subscriber_scope("dynamic-consumer-process", 17U);
    rbt::ActivationContext subscriber_context("dynamic_consumer",
                                              "dynamic-consumer-process",
                                              17U,
                                              { binding },
                                              subscriber_scope);
    rbt::tf::TransformBuffer buffer;
    auto subscriber = rbt::tf::require_dynamic_tf<DynamicCapability>(
        subscriber_context, "dynamic", "camera/base", buffer);
    const auto old_authority = subscriber.authority();
    const auto event_source = subscriber.event_source(rbt::WaitToken::new_token(71U));
    assert(event_source.token() == rbt::WaitToken::new_token(71U));

    publisher.publish(rbt::tf::DynamicRecord {
        rbt::tf::FrameId("map"),
        rbt::tf::FrameId("camera/base"),
        rbt::time::Timestamp { clock, 10 },
        1U,
        0U,
        rbt::tf::Transform3::identity(),
    });
    assert(subscriber.drain() == 1U);
    assert(buffer.pressure_snapshot().stored_samples == 1U);

    provider_scope.shutdown();
    bool publisher_expired = false;
    try {
        publisher.publish(rbt::tf::DynamicRecord {
            rbt::tf::FrameId("map"),
            rbt::tf::FrameId("camera/base"),
            rbt::time::Timestamp { clock, 20 },
            1U,
            0U,
            rbt::tf::Transform3::identity(),
        });
    } catch (const rbt::tf::TfTransportError& error) {
        publisher_expired = error.kind() == rbt::tf::TfTransportErrorKind::InactiveEndpoint;
    }
    assert(publisher_expired);

    subscriber_scope.shutdown();
    bool subscriber_expired = false;
    try {
        (void)subscriber.drain();
    } catch (const rbt::tf::TfTransportError& error) {
        subscriber_expired = error.kind() == rbt::tf::TfTransportErrorKind::InactiveEndpoint;
    }
    assert(subscriber_expired);
    bool stale_wait_token_rejected = false;
    try {
        (void)subscriber.event_source(event_source.token());
    } catch (const rbt::tf::TfTransportError& error) {
        stale_wait_token_rejected = error.kind() == rbt::tf::TfTransportErrorKind::InactiveEndpoint;
    }
    assert(stale_wait_token_rejected);

    bool stale_authority_rejected = false;
    try {
        buffer.ingest_dynamic(old_authority,
                              rbt::tf::DynamicRecord {
                                  rbt::tf::FrameId("map"),
                                  rbt::tf::FrameId("camera/base"),
                                  rbt::time::Timestamp { clock, 20 },
                                  1U,
                                  0U,
                                  rbt::tf::Transform3::identity(),
                              });
    } catch (const rbt::tf::TfError& error) {
        stale_authority_rejected = error.code() == rbt::tf::TfErrorCode::ProviderUnavailable;
    }
    assert(stale_authority_rejected);
}

void dynamic_endpoint_owns_buffer_state_until_scope_shutdown() {
    const auto clock = test_clock();
    rbt::EffectScope provider_scope("owned-buffer-dynamic-provider", 37U);
    rbt::ActivationContext provider_context("owned_buffer_dynamic_provider",
                                            "owned-buffer-dynamic-provider",
                                            37U,
                                            {},
                                            provider_scope);
    auto publisher = rbt::tf::provide_dynamic_tf<DynamicCapability>(provider_context);
    auto binding = publisher.capability().binding();
    binding.requirement_name = "dynamic";

    rbt::EffectScope subscriber_scope("owned-buffer-dynamic-subscriber", 41U);
    rbt::ActivationContext subscriber_context("owned_buffer_dynamic_subscriber",
                                              "owned-buffer-dynamic-subscriber",
                                              41U,
                                              { binding },
                                              subscriber_scope);
    std::optional<rbt::tf::TransformBuffer> observer;
    std::optional<rbt::tf::Authority> old_authority;
    std::optional<rbt::tf::ScopedDynamicTfSubscriber<DynamicCapability>> subscriber;
    const auto record = [&clock](std::int64_t ns) {
        return rbt::tf::DynamicRecord {
            rbt::tf::FrameId("map"),
            rbt::tf::FrameId("camera/base"),
            rbt::time::Timestamp { clock, ns },
            1U,
            0U,
            rbt::tf::Transform3::identity(),
        };
    };

    {
        rbt::tf::TransformBuffer caller_buffer;
        observer.emplace(caller_buffer);
        subscriber.emplace(rbt::tf::require_dynamic_tf<DynamicCapability>(
            subscriber_context, "dynamic", "camera/base", caller_buffer));
        old_authority = subscriber->authority();
        publisher.publish(record(10));
        assert(subscriber->drain() == 1U);
    }

    // The caller's buffer object is gone, but the endpoint's owned handle still
    // reaches the shared state and can ingest another native sample.
    publisher.publish(record(20));
    assert(subscriber->drain() == 1U);
    assert(observer->pressure_snapshot().stored_samples == 2U);
    subscriber_scope.shutdown();
    assert(observer->pressure_snapshot().providers.empty());

    rbt::tf::LookupOptions options;
    options.timestamp_uncertainty_ns = 1U;
    bool unknown_after_shutdown = false;
    try {
        (void)observer->lookup_at("map", "camera/base", rbt::time::Timestamp { clock, 20 }, options);
    } catch (const rbt::tf::TfError& error) {
        unknown_after_shutdown = error.code() == rbt::tf::TfErrorCode::UnknownFrame;
    }
    assert(unknown_after_shutdown);

    bool stale_authority_rejected = false;
    try {
        observer->ingest_dynamic(*old_authority, record(30));
    } catch (const rbt::tf::TfError& error) {
        stale_authority_rejected = error.code() == rbt::tf::TfErrorCode::ProviderUnavailable;
    }
    assert(stale_authority_rejected);
}

void static_helpers_validate_pair_snapshot_and_shutdown() {
    const auto clock = test_clock();
    rbt::EffectScope provider_scope("static-provider-process", 23U);
    rbt::ActivationContext provider_context("static_provider",
                                            "static-provider-process",
                                            23U,
                                            {},
                                            provider_scope);
    auto provider = rbt::tf::provide_static_tf<StaticUpdateCapability, StaticSnapshotCapability>(
        provider_context, 8U);
    const auto& update_descriptor = provider.update_capability().stream();
    const auto& snapshot_descriptor = provider.snapshot_capability().rpc();
    assert(update_descriptor.service_name
           == "rbt/cap/" + provider.update_capability().capability_instance() + "/stream");
    assert(snapshot_descriptor.service_name
           == "rbt/cap/" + provider.snapshot_capability().capability_instance() + "/rpc");
    assert(provider.update_capability().capability_instance()
           != provider.snapshot_capability().capability_instance());

    auto update_binding = provider.update_capability().binding();
    update_binding.requirement_name = "static_update";
    auto snapshot_binding = provider.snapshot_capability().binding();
    snapshot_binding.requirement_name = "snapshot";
    const auto pair_mismatch_rejected = [&](rbt::ResolvedBinding bad_snapshot) {
        rbt::EffectScope mismatch_scope("static-mismatch-process", 29U);
        rbt::ActivationContext mismatch_context("static_consumer",
                                                "static-mismatch-process",
                                                29U,
                                                { update_binding, std::move(bad_snapshot) },
                                                mismatch_scope);
        rbt::tf::TransformBuffer mismatch_buffer;
        try {
            (void)rbt::tf::require_static_tf<StaticUpdateCapability, StaticSnapshotCapability>(
                mismatch_context,
                "static_update",
                "snapshot",
                "camera",
                mismatch_buffer,
                8U);
        } catch (const rbt::tf::TfTransportError& error) {
            return error.kind() == rbt::tf::TfTransportErrorKind::StaticCapabilityPairMismatch;
        }
        return false;
    };
    auto mismatched_snapshot = snapshot_binding;
    ++mismatched_snapshot.descriptor.activation;
    assert(pair_mismatch_rejected(mismatched_snapshot));
    mismatched_snapshot = snapshot_binding;
    mismatched_snapshot.descriptor.provider_process = "other-provider-process";
    assert(pair_mismatch_rejected(mismatched_snapshot));
    mismatched_snapshot = snapshot_binding;
    mismatched_snapshot.provider = "other_provider";
    mismatched_snapshot.descriptor.provider_component = "other_provider";
    assert(pair_mismatch_rejected(mismatched_snapshot));

    rbt::EffectScope client_scope("static-consumer-process", 31U);
    rbt::ActivationContext client_context("static_consumer",
                                          "static-consumer-process",
                                          31U,
                                          { update_binding, snapshot_binding },
                                          client_scope);
    rbt::tf::TransformBuffer buffer;
    auto client = rbt::tf::require_static_tf<StaticUpdateCapability, StaticSnapshotCapability>(
        client_context, "static_update", "snapshot", "camera", buffer, 8U);
    const auto old_authority = client.authority();

    provider.replace(rbt::tf::StaticSet {
        1U,
        rbt::time::Timestamp { clock, 0 },
        1U,
        { rbt::tf::StaticEdge {
            rbt::tf::FrameId("map"),
            rbt::tf::FrameId("camera"),
            rbt::tf::Transform3(rbt::tf::Vec3(2.0, 0.0, 0.0), rbt::tf::Quaternion::identity()),
        } },
    });
    std::jthread server([&provider](std::stop_token stop) {
        while (!stop.stop_requested()) {
            (void)provider.serve_requests(1U);
            std::this_thread::sleep_for(1ms);
        }
    });
    client.refresh_snapshot(1s);
    server.request_stop();
    server.join();
    assert(buffer.pressure_snapshot().providers.size() == 1U);
    assert(buffer.pressure_snapshot().providers.front().static_revision == 1U);

    rbt::tf::LookupOptions options;
    options.timestamp_uncertainty_ns = 1U;
    const auto lookup = buffer.lookup_at("map", "camera", rbt::time::Timestamp { clock, 10 }, options);
    const auto transformed = lookup.apply_point(rbt::tf::Vec3(0.0, 0.0, 0.0));
    assert(std::abs(transformed.x() - 2.0) < 1e-12);

    client_scope.shutdown();
    bool client_expired = false;
    try {
        (void)client.refresh_if_needed(1ms);
    } catch (const rbt::tf::TfTransportError& error) {
        client_expired = error.kind() == rbt::tf::TfTransportErrorKind::InactiveEndpoint;
    }
    assert(client_expired);
    bool stale_authority_rejected = false;
    try {
        buffer.apply_static_snapshot(old_authority, rbt::tf::StaticSet {
            2U,
            rbt::time::Timestamp { clock, 10 },
            1U,
            {},
        });
    } catch (const rbt::tf::TfError& error) {
        stale_authority_rejected = error.code() == rbt::tf::TfErrorCode::ProviderUnavailable;
    }
    assert(stale_authority_rejected);

    provider_scope.shutdown();
    bool provider_expired = false;
    try {
        (void)provider.serve_requests(1U);
    } catch (const rbt::tf::TfTransportError& error) {
        provider_expired = error.kind() == rbt::tf::TfTransportErrorKind::InactiveEndpoint;
    }
    assert(provider_expired);
}

void static_endpoint_owns_buffer_state_until_explicit_close() {
    const auto clock = test_clock();
    rbt::EffectScope provider_scope("owned-buffer-static-provider", 43U);
    rbt::ActivationContext provider_context("owned_buffer_static_provider",
                                            "owned-buffer-static-provider",
                                            43U,
                                            {},
                                            provider_scope);
    auto provider = rbt::tf::provide_static_tf<StaticUpdateCapability, StaticSnapshotCapability>(
        provider_context, 8U);
    auto update_binding = provider.update_capability().binding();
    update_binding.requirement_name = "static_update";
    auto snapshot_binding = provider.snapshot_capability().binding();
    snapshot_binding.requirement_name = "snapshot";

    rbt::EffectScope client_scope("owned-buffer-static-client", 47U);
    rbt::ActivationContext client_context("owned_buffer_static_client",
                                          "owned-buffer-static-client",
                                          47U,
                                          { update_binding, snapshot_binding },
                                          client_scope);
    std::optional<rbt::tf::TransformBuffer> observer;
    std::optional<rbt::tf::Authority> old_authority;
    std::optional<rbt::tf::ScopedStaticTfClient<StaticUpdateCapability, StaticSnapshotCapability>> client;
    {
        rbt::tf::TransformBuffer caller_buffer;
        observer.emplace(caller_buffer);
        client.emplace(rbt::tf::require_static_tf<StaticUpdateCapability, StaticSnapshotCapability>(
            client_context, "static_update", "snapshot", "camera", caller_buffer, 8U));
        old_authority = client->authority();

        provider.replace(rbt::tf::StaticSet {
            1U,
            rbt::time::Timestamp { clock, 0 },
            1U,
            { rbt::tf::StaticEdge {
                rbt::tf::FrameId("map"),
                rbt::tf::FrameId("camera"),
                rbt::tf::Transform3(rbt::tf::Vec3(2.0, 0.0, 0.0),
                                    rbt::tf::Quaternion::identity()),
            } },
        });
        std::jthread server([&provider](std::stop_token stop) {
            while (!stop.stop_requested()) {
                (void)provider.serve_requests(1U);
                std::this_thread::sleep_for(1ms);
            }
        });
        client->refresh_snapshot(1s);
        server.request_stop();
        server.join();
    }

    provider.replace(rbt::tf::StaticSet {
        2U,
        rbt::time::Timestamp { clock, 0 },
        1U,
        { rbt::tf::StaticEdge {
            rbt::tf::FrameId("map"),
            rbt::tf::FrameId("camera"),
            rbt::tf::Transform3(rbt::tf::Vec3(4.0, 0.0, 0.0),
                                rbt::tf::Quaternion::identity()),
        } },
    });
    assert(client->drain_updates() == 1U);
    assert(observer->pressure_snapshot().providers.size() == 1U);
    assert(observer->pressure_snapshot().providers.front().static_revision == 2U);
    rbt::tf::LookupOptions options;
    options.timestamp_uncertainty_ns = 1U;
    const auto lookup = observer->lookup_at("map", "camera", rbt::time::Timestamp { clock, 10 }, options);
    assert(std::abs(lookup.transform.translation().x() - 4.0) < 1e-12);

    // Explicit close through the retained scoped client safely unbinds the
    // endpoint-owned buffer copy after the original object has been destroyed.
    client->close();
    assert(observer->pressure_snapshot().providers.empty());
    bool unknown_after_close = false;
    try {
        (void)observer->lookup_at("map", "camera", rbt::time::Timestamp { clock, 10 }, options);
    } catch (const rbt::tf::TfError& error) {
        unknown_after_close = error.code() == rbt::tf::TfErrorCode::UnknownFrame;
    }
    assert(unknown_after_close);

    bool stale_authority_rejected = false;
    try {
        observer->apply_static_snapshot(*old_authority, rbt::tf::StaticSet {
            3U,
            rbt::time::Timestamp { clock, 10 },
            1U,
            {},
        });
    } catch (const rbt::tf::TfError& error) {
        stale_authority_rejected = error.code() == rbt::tf::TfErrorCode::ProviderUnavailable;
    }
    assert(stale_authority_rejected);
    client_scope.shutdown();
}

} // namespace

int main() {
    dynamic_helpers_bind_copy_and_expire_endpoints();
    dynamic_endpoint_owns_buffer_state_until_scope_shutdown();
    static_helpers_validate_pair_snapshot_and_shutdown();
    static_endpoint_owns_buffer_state_until_explicit_close();
    return 0;
}

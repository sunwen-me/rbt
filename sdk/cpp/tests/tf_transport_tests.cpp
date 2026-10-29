#ifdef NDEBUG
#undef NDEBUG
#endif

#include <rbt/tf_transport.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using rbt::CapabilityInstanceId;
using rbt::tf::DynamicRecord;
using rbt::tf::FrameId;
using rbt::tf::LookupOptions;
using rbt::tf::Quaternion;
using rbt::tf::StaticEdge;
using rbt::tf::StaticSet;
using rbt::tf::StaticTransform;
using rbt::tf::StaticUpdateHeader;
using rbt::tf::TfError;
using rbt::tf::TfErrorCode;
using rbt::tf::TfResourceConfig;
using rbt::tf::Transform3;
using rbt::tf::TransformBuffer;
using rbt::tf::Vec3;
using rbt::time::ClockSourceId;
using rbt::time::ClockIdentity;
using rbt::time::ClockKind;
using rbt::time::Timestamp;

using StaticUpdatePublisher = rbt::SlicePublisher<StaticUpdateHeader, StaticTransform>;

void require(bool condition) {
    assert(condition);
}

[[nodiscard]] CapabilityInstanceId uuid(std::uint16_t suffix) {
    static constexpr char digits[] = "0123456789abcdef";
    CapabilityInstanceId result = "00000000-0000-0000-0000-000000000000";
    for (std::size_t index = 0U; index < 4U; ++index) {
        result[35U - index] = digits[suffix & 0x0FU];
        suffix = static_cast<std::uint16_t>(suffix >> 4U);
    }
    return result;
}

[[nodiscard]] ClockIdentity clock_identity() {
    std::array<std::uint8_t, 48U> bytes {};
    std::fill_n(bytes.begin(), 16U, 42U);
    auto result = ClockIdentity::create(ClockKind::Device,
                                        ClockSourceId(std::move(bytes)),
                                        7U);
    require(result.has_value());
    return std::move(result).value();
}

[[nodiscard]] Transform3 translation(double x) {
    return Transform3(Vec3(x, 0.0, 0.0), Quaternion::identity());
}

[[nodiscard]] StaticSet static_set(std::uint64_t revision,
                                   const ClockIdentity& clock) {
    return StaticSet { revision,
                       Timestamp { clock, 0 },
                       0U,
                       { StaticEdge { FrameId("base_link"), FrameId("camera_link"), translation(1.0) },
                         StaticEdge { FrameId("camera_link"),
                                      FrameId("camera_optical"),
                                      Transform3::identity() } } };
}

[[nodiscard]] std::array<DynamicRecord, 2U> dynamic_set(const ClockIdentity& clock) {
    return { DynamicRecord { FrameId("odom"),
                             FrameId("base_link"),
                             Timestamp { clock, 100'000'000 },
                             0U,
                             3U,
                             translation(0.0) },
             DynamicRecord { FrameId("odom"),
                             FrameId("base_link"),
                             Timestamp { clock, 200'000'000 },
                             0U,
                             3U,
                             Transform3(Vec3(2.0, 0.0, 0.0),
                                        Quaternion(0.0,
                                                   0.0,
                                                   std::sin(std::numbers::pi / 4.0),
                                                   std::cos(std::numbers::pi / 4.0))) } };
}

template <typename Callable>
void expect_tf_error(TfErrorCode code, Callable&& callable) {
    bool threw = false;
    try {
        std::forward<Callable>(callable)();
    } catch (const TfError& error) {
        threw = true;
        require(error.code() == code);
    }
    require(threw);
}

template <typename Callable>
void expect_transport_error(rbt::tf::TfTransportErrorKind kind, Callable&& callable) {
    bool threw = false;
    try {
        std::forward<Callable>(callable)();
    } catch (const rbt::tf::TfTransportError& error) {
        threw = true;
        require(error.kind() == kind);
    }
    require(threw);
}

template <typename Callable>
void serve_one_request(rbt::tf::StaticTfProvider& provider, Callable&& call) {
    std::atomic<bool> finished { false };
    std::thread server([&] {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!finished.load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < deadline) {
            if (provider.serve_requests(1U) != 0U) {
                return;
            }
            std::this_thread::sleep_for(1ms);
        }
    });
    try {
        std::forward<Callable>(call)();
    } catch (...) {
        finished.store(true, std::memory_order_release);
        server.join();
        throw;
    }
    finished.store(true, std::memory_order_release);
    server.join();
}

std::uint64_t snapshot_requests_received(const CapabilityInstanceId& snapshot_id) {
    const auto service = "rbt/cap/" + snapshot_id + "/rpc";
    for (const auto& pressure : rbt::local_rpc_pressure_snapshots()) {
        if (pressure.service_name == service && pressure.server) {
            return pressure.server->requests_received;
        }
    }
    std::abort();
}

void publish_static_update(StaticUpdatePublisher& publisher,
                           const StaticUpdateHeader& header,
                           std::span<const StaticTransform> edges) {
    auto loan_result = publisher.loan_slice(edges.size());
    require(loan_result.has_value());
    auto loan = std::move(loan_result).value();
    loan.header() = header;
    const auto payload = loan.payload();
    std::copy(edges.begin(), edges.end(), payload.begin());
    require(loan.send().has_value());
}

void static_snapshot_update_gap_resync_and_rpc_bounds() {
    const auto clock = clock_identity();
    const auto static_id = uuid(100U);
    const auto snapshot_id = uuid(101U);
    const auto dynamic_id = uuid(102U);
    auto provider = rbt::tf::StaticTfProvider::open(static_id, static_id, snapshot_id, 8U);
    provider.replace(static_set(10U, clock));

    // Join after the initial static update. The RPC snapshot gives a complete
    // current set, and dynamic data is received from the next native samples.
    TfResourceConfig buffer_resources;
    buffer_resources.resync_interval = 10ms;
    TransformBuffer buffer(buffer_resources);
    auto static_client = rbt::tf::StaticTfClient::open(static_id,
                                                       static_id,
                                                       snapshot_id,
                                                       "static",
                                                       buffer,
                                                       8U);
    auto dynamic_publisher = rbt::tf::DynamicTfPublisher::open(dynamic_id);
    auto dynamic_subscriber = rbt::tf::DynamicTfSubscriber::open(dynamic_id,
                                                                  "dynamic",
                                                                  buffer);
    serve_one_request(provider, [&] { static_client.refresh_snapshot(2s); });
    require(buffer.pressure_snapshot().providers.size() == 2U);

    provider.replace(static_set(11U, clock));
    for (const auto& record : dynamic_set(clock)) {
        dynamic_publisher.publish(record);
    }
    require(static_client.drain_updates() == 1U);
    require(dynamic_subscriber.drain() == 2U);
    const auto revision_11 = buffer.pressure_snapshot();
    require(revision_11.providers[0].static_revision == std::optional<std::uint64_t>(11U)
            || revision_11.providers[1].static_revision == std::optional<std::uint64_t>(11U));

    expect_tf_error(TfErrorCode::SnapshotTooLarge, [&] {
        serve_one_request(provider, [&] { static_client.refresh_snapshot(2s, 1U); });
    });
    const auto failed_snapshot = buffer.pressure_snapshot();
    const auto failed_static = std::find_if(
        failed_snapshot.providers.begin(),
        failed_snapshot.providers.end(),
        [](const auto& observation) {
            return observation.static_revision == std::optional<std::uint64_t>(11U);
        });
    require(failed_static != failed_snapshot.providers.end() && failed_static->static_stale);
    LookupOptions stale_options {};
    stale_options.timestamp_uncertainty_ns = 0U;
    expect_tf_error(TfErrorCode::SnapshotStale, [&] {
        (void)buffer.lookup_at("base_link", "camera_optical", Timestamp { clock, 0 }, stale_options);
    });
    bool failed_snapshot_client_pressure_seen = false;
    for (const auto& pressure : rbt::local_rpc_pressure_snapshots()) {
        if (pressure.service_name == "rbt/cap/" + snapshot_id + "/rpc" && pressure.client) {
            failed_snapshot_client_pressure_seen = true;
            require(pressure.client->response_guards_current == 0U);
            require(pressure.client->in_flight_current == 0U);
        }
    }
    require(failed_snapshot_client_pressure_seen);

    // The next native revision triggers a snapshot resync and restores the complete set.
    std::this_thread::sleep_for(12ms);
    provider.replace(static_set(12U, clock));
    serve_one_request(provider, [&] { require(static_client.drain_updates() == 1U); });
    const auto recovered = buffer.pressure_snapshot();
    const auto recovered_static = std::find_if(
        recovered.providers.begin(),
        recovered.providers.end(),
        [](const auto& observation) {
            return observation.static_revision == std::optional<std::uint64_t>(12U);
        });
    require(recovered_static != recovered.providers.end() && !recovered_static->static_stale);

    const auto dynamic_pressure = dynamic_subscriber.pressure();
    const auto static_pressure = static_client.update_stream_pressure();
    require(dynamic_pressure.subscriber && dynamic_pressure.subscriber->borrowed_samples == 0U);
    require(static_pressure.subscriber && static_pressure.subscriber->borrowed_samples == 0U);
    bool final_snapshot_client_pressure_seen = false;
    for (const auto& pressure : rbt::local_rpc_pressure_snapshots()) {
        if (pressure.service_name == "rbt/cap/" + snapshot_id + "/rpc" && pressure.client) {
            final_snapshot_client_pressure_seen = true;
            require(pressure.client->response_guards_current == 0U);
            require(pressure.client->in_flight_current == 0U);
        }
    }
    require(final_snapshot_client_pressure_seen);
}

void static_snapshot_resync_obeys_interval() {
    const auto clock = clock_identity();
    const auto static_id = uuid(120U);
    const auto snapshot_id = uuid(121U);
    TfResourceConfig buffer_resources;
    buffer_resources.resync_interval = 10ms;
    TransformBuffer buffer(buffer_resources);
    auto provider = rbt::tf::StaticTfProvider::open(static_id, static_id, snapshot_id, 8U);
    provider.replace(static_set(10U, clock));
    auto client = rbt::tf::StaticTfClient::open(static_id,
                                                static_id,
                                                snapshot_id,
                                                "resync_interval",
                                                buffer,
                                                8U);
    serve_one_request(provider, [&] { client.refresh_snapshot(2s); });
    const auto initial_requests = snapshot_requests_received(snapshot_id);
    require(initial_requests == 1U);

    buffer.mark_snapshot_stale(client.authority());
    provider.replace(static_set(11U, clock));
    require(client.drain_updates() == 1U);
    provider.replace(static_set(12U, clock));
    require(client.drain_updates() == 1U);
    require(snapshot_requests_received(snapshot_id) == initial_requests);
    LookupOptions options {};
    options.timestamp_uncertainty_ns = 0U;
    expect_tf_error(TfErrorCode::SnapshotStale, [&] {
        (void)buffer.lookup_at("base_link", "camera_optical", Timestamp { clock, 0 }, options);
    });

    std::this_thread::sleep_for(12ms);
    provider.replace(static_set(13U, clock));
    serve_one_request(provider, [&] { require(client.drain_updates() == 1U); });
    const auto after_due_resync = snapshot_requests_received(snapshot_id);
    require(after_due_resync == initial_requests + 1U);
    auto pressure = buffer.pressure_snapshot();
    require(pressure.providers.size() == 1U);
    require(pressure.providers[0].static_revision == std::optional<std::uint64_t>(13U));
    require(buffer.lookup_at("base_link",
                             "camera_optical",
                             Timestamp { clock, 0 },
                             options)
                .transform.translation()
                .x()
            == 1.0);

    buffer.mark_snapshot_stale(client.authority());
    provider.replace(static_set(14U, clock));
    require(client.drain_updates() == 1U);
    require(snapshot_requests_received(snapshot_id) == after_due_resync);
}

void wrong_authority_is_rejected_after_releasing_sample_guard() {
    const auto expected_id = uuid(110U);
    const auto wrong_id = uuid(111U);
    const auto descriptor = rbt::tf::dynamic_tf_stream_descriptor(expected_id);
    auto publisher_result = rbt::FixedPublisher<rbt::tf::DynamicTransform>::create(
        descriptor, rbt::tf::DYNAMIC_TRANSFORM_DESCRIPTOR);
    require(publisher_result.has_value());
    auto publisher = std::move(publisher_result).value();
    TransformBuffer buffer;
    auto subscriber = rbt::tf::DynamicTfSubscriber::open(expected_id, "expected", buffer);
    const auto clock = clock_identity();
    const auto record = dynamic_set(clock)[0];
    const auto wire = rbt::tf::encode_dynamic(wrong_id, record);
    auto loan_result = publisher.loan();
    require(loan_result.has_value());
    auto loan = std::move(loan_result).value();
    loan.payload() = wire;
    require(loan.send().has_value());
    expect_tf_error(TfErrorCode::ConflictingAuthority, [&] { (void)subscriber.drain(); });
    const auto pressure = subscriber.pressure();
    require(pressure.subscriber && pressure.subscriber->borrowed_samples == 0U);
}

void malformed_static_updates_fail_closed_and_resync() {
    const auto clock = clock_identity();
    const auto static_id = uuid(115U);
    const auto snapshot_id = uuid(116U);
    auto provider = rbt::tf::StaticTfProvider::open(static_id, static_id, snapshot_id, 8U);
    provider.replace(static_set(10U, clock));

    TfResourceConfig buffer_resources;
    buffer_resources.resync_interval = 10ms;
    TransformBuffer buffer(buffer_resources);
    auto client = rbt::tf::StaticTfClient::open(static_id,
                                                static_id,
                                                snapshot_id,
                                                "malformed_static",
                                                buffer,
                                                8U);
    serve_one_request(provider, [&] { client.refresh_snapshot(2s); });
    auto pressure = buffer.pressure_snapshot();
    require(pressure.providers.size() == 1U);
    require(pressure.providers[0].static_revision == std::optional<std::uint64_t>(10U));

    const auto update_descriptor = rbt::tf::static_tf_update_stream_descriptor(static_id);
    auto publisher_result = StaticUpdatePublisher::create(
        update_descriptor, rbt::tf::STATIC_UPDATE_DESCRIPTOR, 16U);
    require(publisher_result.has_value());
    auto publisher = std::move(publisher_result).value();

    LookupOptions options {};
    options.timestamp_uncertainty_ns = 0U;
    const auto require_failed_closed = [&](TfErrorCode code,
                                           const StaticUpdateHeader& header,
                                           std::span<const StaticTransform> edges) {
        publish_static_update(publisher, header, edges);
        expect_tf_error(code, [&] { (void)client.drain_updates(); });
        const auto update_pressure = client.update_stream_pressure();
        require(update_pressure.subscriber
                && update_pressure.subscriber->borrowed_samples == 0U);
        expect_tf_error(TfErrorCode::SnapshotStale, [&] {
            (void)buffer.lookup_at("base_link", "camera_optical", Timestamp { clock, 0 }, options);
        });
    };
    const auto recover = [&](std::uint64_t revision) {
        std::this_thread::sleep_for(12ms);
        provider.replace(static_set(revision, clock));
        serve_one_request(provider, [&] { require(client.drain_updates() == 1U); });
        const auto recovered = buffer.pressure_snapshot();
        require(recovered.providers.size() == 1U);
        require(recovered.providers[0].static_revision
                == std::optional<std::uint64_t>(revision));
        require(buffer.lookup_at("base_link",
                                 "camera_optical",
                                 Timestamp { clock, 0 },
                                 options)
                    .transform.translation()
                    .x()
                == 1.0);
    };

    const auto revision_11 = static_set(11U, clock);
    auto malformed_quaternion = rbt::tf::transport_detail::encode_static_set(revision_11);
    malformed_quaternion.front().transform.rotation = rbt::tf::TfQuaternion { 0.0, 0.0, 0.0, 0.0 };
    const auto header_11 = rbt::tf::static_update_header(static_id, revision_11);
    require_failed_closed(TfErrorCode::InvalidQuaternion,
                          header_11,
                          std::span<const StaticTransform>(malformed_quaternion));
    recover(11U);

    const auto revision_12 = static_set(12U, clock);
    auto count_mismatch = rbt::tf::transport_detail::encode_static_set(revision_12);
    auto incomplete_header = rbt::tf::static_update_header(static_id, revision_12);
    ++incomplete_header.count;
    require_failed_closed(TfErrorCode::InvalidTransform,
                          incomplete_header,
                          std::span<const StaticTransform>(count_mismatch));
    recover(12U);

    const auto revision_13 = static_set(13U, clock);
    const auto valid_edges = rbt::tf::transport_detail::encode_static_set(revision_13);
    std::vector<StaticTransform> oversized_payload(9U, valid_edges.front());
    const auto header_13 = rbt::tf::static_update_header(static_id, revision_13);
    require_failed_closed(TfErrorCode::SnapshotTooLarge,
                          header_13,
                          std::span<const StaticTransform>(oversized_payload));
    recover(13U);
}

void static_pair_is_explicit_and_bounded() {
    TransformBuffer buffer;
    expect_transport_error(rbt::tf::TfTransportErrorKind::StaticCapabilityPairMismatch, [&] {
        (void)rbt::tf::StaticTfClient::open(uuid(120U),
                                            uuid(121U),
                                            uuid(122U),
                                            "static",
                                            buffer);
    });
}

void static_snapshot_descriptor_preserves_rpc_resources() {
    const auto default_resources = rbt::RpcResourceConfig::defaults();
    const auto default_descriptor = rbt::tf::static_tf_snapshot_rpc_descriptor(
        uuid(130U), 64U);
    require(default_descriptor.resources
            == default_resources.with_max_response_slice_len(64U));

    const auto custom_resources = default_resources.with_max_in_flight(2U)
                                      .with_max_clients(3U)
                                      .with_max_request_loans(4U)
                                      .with_response_buffer_size(5U)
                                      .with_max_borrowed_responses(6U)
                                      .with_max_response_loans(7U)
                                      .with_max_request_slice_len(8U)
                                      .with_max_response_slice_len(9U);
    const auto custom_descriptor = rbt::tf::static_tf_snapshot_rpc_descriptor(
        uuid(131U), 64U, custom_resources);
    require(custom_descriptor.resources
            == custom_resources.with_max_response_slice_len(64U));
}

void run_all() {
    static_snapshot_resync_obeys_interval();
    static_snapshot_update_gap_resync_and_rpc_bounds();
    wrong_authority_is_rejected_after_releasing_sample_guard();
    malformed_static_updates_fail_closed_and_resync();
    static_pair_is_explicit_and_bounded();
    static_snapshot_descriptor_preserves_rpc_resources();
}

} // namespace

int main() {
    run_all();
}

#include <rbt/runtime.hpp>

#include <rbt/demo.hpp>
#include <rbt/tf.hpp>

#include <chrono>
#include <cstdlib>
#include <memory>
#include <thread>

namespace {

using namespace std::chrono_literals;

void require(bool condition) {
    if (!condition) {
        std::abort();
    }
}

rbt::OwnedMessageTypeDescriptor frame_type() {
    return {
        "rbt.demo",
        "Frame",
        1U,
        rbt::MessageShape::Fixed,
        16U,
        8U,
        0U,
        0U,
        "5d2c102a797d5b0ab64b4db8844297c0",
        "sequence:u64@0;value:u64@8;padding:none",
    };
}

rbt::OwnedStreamDescriptor frame_stream() {
    return {
        "rbt/cap/11111111-1111-4111-8111-111111111111/stream",
        frame_type(),
        rbt::StreamProfile::Sensor,
        rbt::TransportFeatures::sidecar_v1(),
        rbt::StreamResourceConfig::defaults(),
    };
}

struct StaticSnapshotCapability {
    static constexpr std::string_view TYPE_NAME { "TfStaticSnapshot" };
};

std::string static_snapshot_server_message(std::string_view endpoints) {
    return "{\"type\":\"activate\",\"revision\":{\"epoch\":\"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee\",\"sequence\":3},\"bindings\":["
        "{\"requirement_name\":\"snapshot\",\"capability_type\":\"TfStaticSnapshot\",\"provider\":\"tf_provider\",\"descriptor\":{"
        "\"capability_instance\":\"33333333-3333-4333-8333-333333333333\",\"provider_component\":\"tf_provider\",\"provider_process\":\"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa\",\"activation\":9"
        + std::string(endpoints) + "}}]}";
}

std::string static_snapshot_rpc_json() {
    return R"(,"rpc":{"service_name":"rbt/cap/33333333-3333-4333-8333-333333333333/rpc","request_type":{"namespace":"rbt.tf","name":"StaticSnapshotRequest","abi_major":1,"shape":"fixed","header_size":24,"header_align":4,"element_size":0,"element_align":0,"layout_hash":"85b655e27004d7d38eb10d720e035d7b"},"response_type":{"namespace":"rbt.tf","name":"StaticSnapshot","abi_major":1,"shape":"slice","header_size":112,"header_align":8,"element_size":184,"element_align":8,"layout_hash":"cd373f3a4bdf5d680aec5d208da98e82"},"transport":{"readiness":"rpc-sidecar-v1"},"resources":{"max_in_flight":4,"max_clients":16,"max_request_loans":2,"response_buffer_size":1,"max_borrowed_responses":1,"max_response_loans":1,"max_request_slice_len":1,"max_response_slice_len":1}})";
}

rbt::ResolvedBinding frame_binding() {
    return {
        "camera",
        "Camera",
        "camera-a",
        {
            "11111111-1111-4111-8111-111111111111",
            "camera-a",
            "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
            7U,
            frame_stream(),
        },
    };
}

void codec_round_trip_matches_rust_shape() {
    const auto hello = rbt::ClientMessage::hello("camera", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    require(rbt::ControlCodec::encode(hello)
            == "{\"type\":\"hello\",\"component_id\":\"camera\",\"process_instance\":\"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa\",\"control_protocol_version\":2,\"snapshot_version\":4}");

    const auto binding = frame_binding();
    rbt::ComponentSnapshot snapshot;
    snapshot.component_id = "detector";
    snapshot.process_instance = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
    snapshot.state = rbt::ComponentState::active();
    snapshot.activation = rbt::ActivationSnapshot { 7U, { binding } };
    snapshot.requirements = { { { "camera", "Camera" }, binding } };
    const auto detection_stream = rbt::OwnedStreamDescriptor {
        "rbt/cap/22222222-2222-4222-8222-222222222222/stream",
        {
            "rbt.demo",
            "Detection",
            1U,
            rbt::MessageShape::Fixed,
            16U,
            8U,
            0U,
            0U,
            "d8e60d2ca65e6b5b74f0981d34884a4d",
            "sequence:u64@0;value:u64@8;padding:none",
        },
        rbt::StreamProfile::Sensor,
        rbt::TransportFeatures::sidecar_v1(),
        rbt::StreamResourceConfig::defaults(),
    };
    const rbt::CapabilityDescriptor output_descriptor {
        "22222222-2222-4222-8222-222222222222",
        "detector",
        snapshot.process_instance,
        7U,
        detection_stream,
    };
    snapshot.provided_capabilities = { {
        "Detections",
        output_descriptor.capability_instance,
        output_descriptor,
    } };
    rbt::StreamPressure pressure;
    pressure.service_name = detection_stream.service_name;
    pressure.profile = rbt::StreamProfile::Sensor;
    pressure.semantic = rbt::StreamSemanticConfig { 1U, 0U, true };
    pressure.resources = rbt::StreamResourceConfig { 2U, 1U };
    pressure.publisher = rbt::PublisherPressure {};
    pressure.publisher->loan_attempts = 1U;
    pressure.publisher->loan_successes = 1U;
    pressure.publisher->loans_in_use = 3U;
    pressure.publisher->loans_high_water = 3U;
    pressure.publisher->loan_failures = 1U;
    pressure.publisher->payload_bytes_last = 4U;
    pressure.publisher->payload_bytes_max_seen = 4U;
    pressure.publisher->total_loaned = 4U;
    pressure.publisher->send_attempts = 1U;
    pressure.publisher->send_successes = 1U;
    pressure.publisher->queue_full = 1U;
    pressure.last_pressure_event = rbt::PressureEvent::QueueFull;
    pressure.last_pressure_subscriber = "00112233445566778899aabbccddeeff";
    snapshot.stream_pressures = { pressure };
    rbt::RpcPressure rpc_pressure;
    rpc_pressure.service_name = "rbt/rpc/add";
    rpc_pressure.capability_instance = "33333333-3333-4333-8333-333333333333";
    rpc_pressure.client_activation = 7U;
    rpc_pressure.client = rbt::RpcClientMetrics {};
    rpc_pressure.client->calls_attempted = 4U;
    rpc_pressure.client->calls_completed = 3U;
    snapshot.rpc_pressures = { rpc_pressure };

    const auto encoded = rbt::ControlCodec::encode_snapshot(snapshot);
    require(encoded.find("\"snapshot_version\":" + std::to_string(rbt::COMPOSER_SNAPSHOT_VERSION)) == 1U);
    require(encoded.find("\"fields\"") == std::string::npos);
    require(encoded.find("\"config\"") == std::string::npos);
    require(encoded.find("\"stream\":") != std::string::npos);
    require(encoded.find("\"rpc\":") == std::string::npos);
    rbt::ComponentSnapshot decoded;
    std::string error;
    require(rbt::ControlCodec::decode_snapshot(encoded, decoded, &error));
    require(decoded.component_id == snapshot.component_id);
    require(decoded.process_instance == snapshot.process_instance);
    require(decoded.activation.has_value());
    require(snapshot.activation.has_value());
    require(decoded.activation->activation_id == snapshot.activation->activation_id);
    require(decoded.activation->bindings.size() == snapshot.activation->bindings.size());
    require(decoded.activation->bindings.front().descriptor.stream.message_type.wire_equal(
        snapshot.activation->bindings.front().descriptor.stream.message_type));
    require(decoded.activation->bindings.front().descriptor.stream.transport
            == snapshot.activation->bindings.front().descriptor.stream.transport);
    require(decoded.requirements.size() == snapshot.requirements.size());
    require(decoded.requirements.front().requirement == snapshot.requirements.front().requirement);
    require(decoded.requirements.front().binding.has_value());
    require(snapshot.requirements.front().binding.has_value());
    require(decoded.requirements.front().binding->descriptor.stream.message_type.wire_equal(
        snapshot.requirements.front().binding->descriptor.stream.message_type));
    require(decoded.provided_capabilities.size() == snapshot.provided_capabilities.size());
    require(decoded.provided_capabilities.front().descriptor.stream.message_type.wire_equal(
        snapshot.provided_capabilities.front().descriptor.stream.message_type));
    require(decoded.stream_pressures == snapshot.stream_pressures);
    require(decoded.rpc_pressures == snapshot.rpc_pressures);
}

void rpc_capabilities_round_trip_and_bind_without_stream_fallback() {
    const rbt::RpcDescriptor native_rpc {
        "rbt/cap/33333333-3333-4333-8333-333333333333/rpc",
        rbt::tf::STATIC_SNAPSHOT_REQUEST_DESCRIPTOR,
        rbt::tf::STATIC_SNAPSHOT_DESCRIPTOR,
        rbt::RpcTransportFeatures::rpc_sidecar_v1(),
        rbt::RpcResourceConfig::defaults(),
    };
    const auto expected_rpc = rbt::OwnedRpcDescriptor::from(native_rpc);

    rbt::ServerMessage activation;
    std::string error;
    const auto fixture = static_snapshot_server_message(static_snapshot_rpc_json());
    require(rbt::ControlCodec::decode_server(fixture, activation, &error));
    require(activation.kind == rbt::ServerMessage::Kind::Activate);
    require(activation.bindings.size() == 1U);
    const auto descriptor = activation.bindings.front().descriptor;
    require(descriptor.try_stream() == nullptr);
    require(descriptor.try_rpc() != nullptr);
    require(descriptor.try_rpc()->service_name == expected_rpc.service_name);
    require(descriptor.try_rpc()->request_type.wire_equal(expected_rpc.request_type));
    require(descriptor.try_rpc()->response_type.wire_equal(expected_rpc.response_type));
    require(descriptor.try_rpc()->transport == expected_rpc.transport);
    require(descriptor.try_rpc()->resources == expected_rpc.resources);
    const auto borrowed_rpc = descriptor.try_rpc()->borrowed();
    require(borrowed_rpc.service_name == expected_rpc.service_name);
    require(borrowed_rpc.request_type.package == "rbt.tf");
    require(borrowed_rpc.response_type.shape == rbt::MessageShape::Slice);

    rbt::ComponentSnapshot snapshot;
    snapshot.component_id = "tf_consumer";
    snapshot.process_instance = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
    snapshot.state = rbt::ComponentState::active();
    snapshot.provided_capabilities.push_back({ "TfStaticSnapshot",
                                               descriptor.capability_instance,
                                               descriptor });
    const auto encoded_snapshot = rbt::ControlCodec::encode_snapshot(snapshot);
    require(encoded_snapshot.find("\"rpc\":") != std::string::npos);
    require(encoded_snapshot.find("\"stream\":") == std::string::npos);
    rbt::ComponentSnapshot decoded_snapshot;
    require(rbt::ControlCodec::decode_snapshot(encoded_snapshot, decoded_snapshot, &error));
    require(decoded_snapshot.provided_capabilities.size() == 1U);
    const auto& decoded_descriptor = decoded_snapshot.provided_capabilities.front().descriptor;
    require(decoded_descriptor.try_rpc() != nullptr);
    require(decoded_descriptor.try_rpc()->request_type.wire_equal(expected_rpc.request_type));

    require(!rbt::ControlCodec::decode_server(
        static_snapshot_server_message(static_snapshot_rpc_json() + ",\"stream\":{}"), activation, &error));
    require(!rbt::ControlCodec::decode_server(static_snapshot_server_message(""), activation, &error));

    auto alias_fixture = static_snapshot_rpc_json();
    const auto request_key = alias_fixture.find("request_type");
    require(request_key != std::string::npos);
    alias_fixture.replace(request_key, std::string("request_type").size(), "request");
    const auto response_key = alias_fixture.find("response_type");
    require(response_key != std::string::npos);
    alias_fixture.replace(response_key, std::string("response_type").size(), "response");
    require(rbt::ControlCodec::decode_server(
        static_snapshot_server_message(alias_fixture), activation, &error));

    const auto binding = activation.bindings.front();
    rbt::EffectScope scope("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", 9U);
    rbt::ActivationContext context("tf_consumer",
                                   "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
                                   9U,
                                   { binding },
                                   scope);
    const auto required = context.require<StaticSnapshotCapability>("snapshot");
    require(required.valid());
    require(required.try_rpc() != nullptr);
    require(required.try_stream() == nullptr);
    require(required.capability_instance() == descriptor.capability_instance);
    require(required.rpc().service_name == expected_rpc.service_name);
    bool stream_access_rejected = false;
    try {
        (void)required.stream();
    } catch (const std::logic_error&) {
        stream_access_rejected = true;
    }
    require(stream_access_rejected);
    auto wrong_transport = required.subscriber<rbt::tf::DynamicTransform>();
    require(!wrong_transport && wrong_transport.error() == rbt::StreamError::AbiMismatch);

    const auto provided = context.provide_rpc<StaticSnapshotCapability>(
        "44444444-4444-4444-8444-444444444444", expected_rpc);
    require(provided.valid());
    require(provided.try_rpc() != nullptr);
    require(provided.try_stream() == nullptr);
    require(context.capabilities().size() == 1U);
    require(context.capabilities().front().descriptor.capability_instance
            == "44444444-4444-4444-8444-444444444444");
    require(context.capabilities().front().descriptor.try_rpc() != nullptr);
}

void server_messages_are_owned_and_parsed() {
    const std::string activate =
        "{\"type\":\"activate\",\"revision\":{\"epoch\":\"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee\",\"sequence\":3},\"bindings\":["
        "{\"requirement_name\":\"camera\",\"capability_type\":\"Camera\",\"provider\":\"camera-a\",\"descriptor\":{\"capability_instance\":\"11111111-1111-4111-8111-111111111111\",\"provider_component\":\"camera-a\",\"provider_process\":\"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa\",\"activation\":7,\"stream\":{\"service_name\":\"rbt/cap/11111111-1111-4111-8111-111111111111/stream\",\"message_type\":{\"namespace\":\"rbt.demo\",\"name\":\"Frame\",\"abi_major\":1,\"shape\":\"fixed\",\"header_size\":16,\"header_align\":8,\"element_size\":0,\"element_align\":0,\"layout_hash\":\"5d2c102a797d5b0ab64b4db8844297c0\"},\"profile\":\"sensor\"}}}] }";
    rbt::ServerMessage message;
    std::string error;
    require(rbt::ControlCodec::decode_server(activate, message, &error));
    require(message.kind == rbt::ServerMessage::Kind::Activate);
    require(message.revision.sequence == 3U);
    require(message.bindings.size() == 1U);
    require(message.bindings.front().descriptor.stream.message_type.namespace_name == "rbt.demo");
    require(message.bindings.front().descriptor.stream.message_type.local_fields.empty());
    require(message.bindings.front().descriptor.stream.transport.readiness == rbt::ReadinessProtocol::None);

    const std::string deactivate =
        "{\"type\":\"deactivate\",\"revision\":{\"epoch\":\"eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee\",\"sequence\":4},\"expected_process_instance\":\"bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb\",\"expected_activation\":7,\"reason\":\"provider disappeared\"}";
    require(rbt::ControlCodec::decode_server(deactivate, message, &error));
    require(message.kind == rbt::ServerMessage::Kind::Deactivate);
    require(message.expected_activation == 7U);
    require(message.reason == "provider disappeared");

    const std::string mismatch =
        "{\"type\":\"protocol_mismatch\",\"expected_control_protocol_version\":2,\"expected_snapshot_version\":1,\"received_control_protocol_version\":99,\"received_snapshot_version\":null,\"reason\":\"unsupported version\"}";
    require(rbt::ControlCodec::decode_server(mismatch, message, &error));
    require(message.kind == rbt::ServerMessage::Kind::ProtocolMismatch);
    require(message.received_control_protocol_version == 99U);
    require(!message.received_snapshot_version.has_value());
}

class CountingComponent final : public rbt::Component {
  public:
    rbt::Result<void> prepare() override {
        ++prepare_count;
        return {};
    }

    rbt::ComponentId id() const override {
        return "counting";
    }

    std::vector<rbt::RequirementSpec> requirements() const override {
        return { { "camera", "Camera" } };
    }

    rbt::Result<void> activate(rbt::ActivationContext& context) override {
        ++activate_count;
        output_activation = context.activation_id();
        return {};
    }

    std::uint64_t prepare_count { 0U };
    std::uint64_t activate_count { 0U };
    rbt::ActivationId output_activation { 0U };
};

template <typename Predicate>
void wait_until(Predicate&& predicate) {
    for (std::size_t attempt = 0U; attempt < 100U; ++attempt) {
        if (predicate()) {
            return;
        }
        std::this_thread::sleep_for(10ms);
    }
    require(false);
}

void runtime_lifecycle_and_stale_event_rules() {
    auto component = std::make_unique<CountingComponent>();
    auto* component_ptr = component.get();
    rbt::RuntimeOptions options;
    options.composer_address = "127.0.0.1:1";
    options.reconnect_backoff = { 1ms };
    rbt::ComponentRuntime runtime(std::move(component), options);
    std::thread runner([&runtime] { require(runtime.run() == 0); });
    wait_until([&runtime] { return !runtime.snapshot().component_id.empty(); });
    require(component_ptr->prepare_count == 1U);

    const auto binding = frame_binding();
    rbt::ServerMessage activate;
    activate.kind = rbt::ServerMessage::Kind::Activate;
    activate.revision = { "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee", 1U };
    activate.bindings = { binding };
    require(runtime.apply_server_message(activate));
    require(runtime.state().is(rbt::ComponentStateKind::Active));

    rbt::TaskEvent stale {
        runtime.snapshot().process_instance,
        0U,
        "old",
        rbt::TaskKind::Critical,
        rbt::TaskOutcome::error("stale"),
    };
    runtime.apply_task_event(stale);
    require(runtime.state().is(rbt::ComponentStateKind::Active));

    rbt::ServerMessage deactivate;
    deactivate.kind = rbt::ServerMessage::Kind::Deactivate;
    deactivate.revision = { "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee", 2U };
    deactivate.expected_process_instance = runtime.snapshot().process_instance;
    deactivate.expected_activation = 1U;
    deactivate.reason = "replacement";
    require(runtime.apply_server_message(deactivate));
    require(runtime.state().is(rbt::ComponentStateKind::Blocked));

    activate.revision.sequence = 3U;
    require(runtime.apply_server_message(activate));
    require(runtime.state().is(rbt::ComponentStateKind::Active));
    require(component_ptr->prepare_count == 1U);
    require(component_ptr->activate_count == 2U);

    rbt::TaskEvent background {
        runtime.snapshot().process_instance,
        2U,
        "metrics",
        rbt::TaskKind::Background,
        rbt::TaskOutcome::error("background failure"),
    };
    runtime.apply_task_event(background);
    require(runtime.state().is(rbt::ComponentStateKind::Active));

    rbt::TaskEvent critical {
        runtime.snapshot().process_instance,
        2U,
        "inference",
        rbt::TaskKind::Critical,
        rbt::TaskOutcome::error("critical failure"),
    };
    runtime.apply_task_event(critical);
    require(runtime.state().is(rbt::ComponentStateKind::Failed));

    rbt::ServerMessage mismatch;
    mismatch.kind = rbt::ServerMessage::Kind::ProtocolMismatch;
    mismatch.expected_control_protocol_version = 2U;
    mismatch.expected_snapshot_version = 1U;
    mismatch.received_control_protocol_version = 9U;
    mismatch.reason = "unsupported";
    require(runtime.apply_server_message(mismatch));
    require(runtime.last_protocol_mismatch().has_value());

    runtime.stop();
    runner.join();
}

} // namespace

int main() {
    codec_round_trip_matches_rust_shape();
    rpc_capabilities_round_trip_and_bind_without_stream_fallback();
    server_messages_are_owned_and_parsed();
    runtime_lifecycle_and_stale_event_rules();
    return 0;
}

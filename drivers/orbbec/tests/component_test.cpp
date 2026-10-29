// SOFTWARE_FIXTURE: test backend only. The camera executable never links it.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <rbt_orbbec/component.hpp>
#include <rbt/orbbec.hpp>
#include <rbt/tf_component.hpp>
#include <rbt/time_stream.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

namespace {
using namespace rbt_orbbec;
using namespace std::chrono_literals;
struct FixtureState {
    std::atomic<int> stopped { 0 };
    std::atomic<bool> regress { false }, omit_depth { false };
    std::atomic<int> imu_fault { 0 }, video_delay_ms { 5 };
};
auto fixture = std::make_shared<FixtureState>();

class FixtureDevice final : public CaptureDevice {
  public:
    explicit FixtureDevice(const Options& options) : state_(fixture) {
        calibration_.device = { "SOFTWARE_FIXTURE", options.serial, "fixture", "memory", 0 };
        calibration_.videos = {
            { Kind::Color, 2, 2, 30, Format::Rgb8, 100, 100, 1, 1, 0, {} },
            { Kind::Depth, 2, 2, 30, Format::Depth16, 120, 110, 1, 1, 0, {} }
        };
        auto& depth = calibration_.videos[1];
        if (!options.depth_filters.empty() && !options.align_depth_to_color) {
            depth.width = depth.height = 1;
            depth.fx = 60;
            depth.fy = 55;
            depth.cx = depth.cy = .5;
        }
        if (options.align_depth_to_color) {
            depth.fx = depth.fy = 100;
            depth.cx = depth.cy = 1;
        }
        calibration_.extrinsics.push_back({ Kind::Depth, Kind::Color,
            { 0, -1, 0, 1, 0, 0, 0, 0, 1 }, { .1, .2, .3 } });
    }
    ~FixtureDevice() override { stop(); }
    const Calibration& calibration() const override { return calibration_; }
    std::vector<Frame> poll(std::chrono::milliseconds) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(state_->video_delay_ms.load()));
        ++index_;
        std::vector<Frame> frames;
        for (const auto kind : { Kind::Color, Kind::Depth }) {
            if (kind == Kind::Depth && state_->omit_depth.load()) continue;
            const auto width = kind == Kind::Depth ? calibration_.videos[1].width : 2U;
            const auto height = kind == Kind::Depth ? calibration_.videos[1].height : 2U;
            auto data = std::make_shared<std::vector<std::uint8_t>>(
                static_cast<std::size_t>(width) * height * (kind == Kind::Depth ? 2U : 3U), 0);
            if (kind == Kind::Depth) { (*data)[0] = 0xe8; (*data)[1] = 3; }
            frames.push_back({ kind, kind == Kind::Depth ? Format::Depth16 : Format::Rgb8,
                width, height, width * (kind == Kind::Depth ? 2U : 3U), index_,
                state_->regress.load() && index_ >= 4 ? 1 : index_ * 1000,
                kind == Kind::Depth ? .001 : 1.0, *data, data });
        }
        return frames;
    }
    std::vector<Frame> poll_imu(std::chrono::milliseconds) override {
        std::this_thread::sleep_for(5ms);
        const auto index = ++imu_index_;
        const auto fault = state_->imu_fault.load();
        if (fault == 4) throw std::overflow_error("fixture IMU callback queue overflow");
        std::vector<Frame> frames;
        for (auto kind : { Kind::Accel, Kind::Gyro }) {
            if (fault == 3 && kind == Kind::Gyro) continue;
            auto data = std::make_shared<std::array<float, 3>>(std::array<float, 3>{1, 2, 3});
            const auto bytes = std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(data->data()), sizeof(*data));
            frames.push_back({kind, kind == Kind::Accel ? Format::Accel32f : Format::Gyro32f,
                1, 1, 12, index + (fault == 1 && index >= 4 ? 1 : 0),
                fault == 2 && index >= 4 ? 1 : index * 5000, 1.0, bytes, data});
        }
        return frames;
    }
    void stop() noexcept override { if (!closed_) { closed_ = true; ++state_->stopped; } }
    std::int32_t get_control(std::uint32_t property) override {
        if (property == 8) throw UnsupportedControl("fixture unsupported control");
        if (property == 3) throw std::runtime_error("fixture SDK error");
        if (property == 4) std::this_thread::sleep_for(60ms);
        return value_;
    }
    void set_control(std::uint32_t, std::int32_t value) override {
        if (value < 0) throw std::out_of_range("fixture range");
        value_ = value;
    }
  private:
    Calibration calibration_;
    std::shared_ptr<FixtureState> state_;
    std::uint64_t index_ { 0 }, imu_index_ { 0 };
    std::int32_t value_ { 1 };
    bool closed_ { false };
};

const rbt::ProvidedCapability& find(const rbt::ActivationContext& context, std::string_view type) {
    for (const auto& capability : context.capabilities())
        if (capability.capability_type == type) return capability;
    throw std::runtime_error("missing fixture capability");
}

rbt::time::ClockIdentity receive_depth(rbt::time::StampedBytesSubscriber& subscriber) {
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto received = subscriber.receive();
        assert(received);
        auto sample = std::move(received).value();
        if (!sample) { std::this_thread::sleep_for(2ms); continue; }
        const auto timed = rbt::time::decode_stamped_bytes(sample->payload());
        const auto* decoded = std::get_if<rbt::time::DecodedStampedBytes>(&timed);
        assert(decoded && decoded->timing.capture && !decoded->timing.capture_uncertainty_ns);
        assert(decoded->timing.capture->clock.kind() == rbt::time::ClockKind::Device);
        const auto frame = decode_frame(decoded->bytes);
        assert(frame.kind == Kind::Depth && frame.width == 2 && frame.height == 2);
        assert(frame.frame_id == "orbbec335/depth_optical" && frame.unit_scale == .001);
        assert(frame.data[0] == 0xe8 && frame.data[1] == 3);
        return decoded->timing.capture->clock;
    }
    throw std::runtime_error("fixture received no depth frame");
}

void integration() {
    fixture = std::make_shared<FixtureState>();
    Options options;
    options.serial = "SOFTWARE_FIXTURE";
    options.width = options.height = 2;
    auto camera = make_camera_component(options);
    assert(camera->prepare());
    const auto process = rbt::generate_uuid();
    rbt::EffectScope scope(process, 1);
    rbt::ActivationContext context(options.component_id, process, 1, {}, scope);
    assert(camera->activate(context));
    assert(context.capabilities().size() == 6);
    const auto depth = find(context, DepthCapability::TYPE_NAME).descriptor;
    auto opened = rbt::time::create_stamped_bytes_subscriber(depth.stream.borrowed());
    assert(opened);
    auto subscriber = std::move(opened).value();
    const auto clock = receive_depth(subscriber);

    using InfoClient = rbt::RpcClient<rbt::orbbec::CameraInfoRequest, rbt::orbbec::CameraInfoResponse>;
    auto info = InfoClient::open(find(context, CameraInfoCapability::TYPE_NAME).descriptor.rpc->borrowed());
    assert(info);
    auto response = info.value().call({ 2, 0 }, 1s);
    assert(response && response.value().payload().status == 0 && response.value().payload().width == 2);
    response = info.value().call({ 2, 1 }, 1s);
    assert(response && response.value().payload().status == 2);

    using ControlClient = rbt::RpcClient<rbt::orbbec::ControlRequest, rbt::orbbec::ControlResponse>;
    auto control = ControlClient::open(find(context, ControlCapability::TYPE_NAME).descriptor.rpc->borrowed());
    assert(control);
    for (const auto request : { rbt::orbbec::ControlRequest { 0, 99, 0, 0 }, { 1, 2, -1, 0 },
                                { 0, 8, 0, 0 }, { 0, 3, 0, 0 }, { 1, 2, 7, 0 }, { 0, 2, 0, 0 } }) {
        auto reply = control.value().call(request, 1s);
        assert(reply);
        const auto expected = request.property == 99 || request.value < 0 ? 2U
            : request.property == 8 ? 1U : request.property == 3 ? 3U : 0U;
        assert(reply.value().payload().status == expected);
        if (expected == 0) assert(reply.value().payload().value == 7);
    }
    auto timed_out = control.value().call({ 0, 4, 0, 0 }, 20ms);
    assert(!timed_out && timed_out.error() == rbt::RpcError::DeadlineExceeded);
    std::this_thread::sleep_for(80ms);
    (void)receive_depth(subscriber); // Rejected RPC operations did not stop capture.

    std::vector<rbt::ResolvedBinding> bindings;
    for (const auto& pair : { std::pair { "updates", StaticUpdatesCapability::TYPE_NAME },
                              std::pair { "snapshot", StaticSnapshotCapability::TYPE_NAME } }) {
        const auto& capability = find(context, pair.second);
        bindings.push_back({ pair.first, pair.second, options.component_id, capability.descriptor });
    }
    rbt::EffectScope client_scope(rbt::generate_uuid(), 2);
    rbt::ActivationContext client_context("fixture-client", client_scope.process_instance(), 2, bindings, client_scope);
    rbt::tf::LookupPolicy policy;
    policy.allow_unknown_uncertainty = true;
    rbt::tf::TransformBuffer buffer({}, policy);
    auto tf = rbt::tf::require_static_tf<StaticUpdatesCapability, StaticSnapshotCapability>(
        client_context, "updates", "snapshot", "fixture", buffer);
    tf.refresh_snapshot(1s);
    const auto found = buffer.lookup_at("orbbec335/depth_optical", "orbbec335/color_optical",
        { clock, 1'000'000'000 }, { {}, {}, nullptr });
    const auto point = found.transform.apply_point(rbt::tf::Vec3(1, 0, 0));
    assert(std::abs(point.x() - .1) < 1e-9 && std::abs(point.y() - 1.2) < 1e-9);
    assert(!found.timing_uncertainty_ns);
    client_scope.shutdown();
    const auto before = std::chrono::steady_clock::now();
    scope.shutdown();
    assert(std::chrono::steady_clock::now() - before < 500ms && fixture->stopped == 1);

    rbt::EffectScope replacement_scope(process, 3);
    rbt::ActivationContext replacement(options.component_id, process, 3, {}, replacement_scope);
    assert(camera->activate(replacement));
    const auto& next_depth = find(replacement, DepthCapability::TYPE_NAME).descriptor;
    assert(depth.capability_instance != next_depth.capability_instance);
    auto next = rbt::time::create_stamped_bytes_subscriber(next_depth.stream.borrowed());
    assert(next && receive_depth(next.value()) != clock);
    replacement_scope.shutdown();
    assert(fixture->stopped == 2);
}

void imu_integration() {
    fixture = std::make_shared<FixtureState>();
    fixture->video_delay_ms = 80;
    Options options;
    options.serial = "SOFTWARE_FIXTURE";
    options.width = options.height = 2;
    options.imu = true;
    auto camera = make_camera_component(options);
    rbt::EffectScope scope(rbt::generate_uuid(), 1);
    rbt::ActivationContext context(options.component_id, scope.process_instance(), 1, {}, scope);
    assert(camera->activate(context));
    const auto& depth = find(context, DepthCapability::TYPE_NAME).descriptor.stream;
    auto depth_sub = rbt::time::create_stamped_bytes_subscriber(depth.borrowed());
    assert(depth_sub);
    const auto video_clock = receive_depth(depth_sub.value());
    const auto& imu = find(context, AccelCapability::TYPE_NAME).descriptor.stream;
    assert(imu.profile == rbt::StreamProfile::Control);
    assert(depth.profile == rbt::StreamProfile::Sensor);
    auto opened = rbt::time::create_stamped_bytes_subscriber(imu.borrowed());
    assert(opened);
    auto sub = std::move(opened).value();
    std::optional<std::uint64_t> sequence, index;
    unsigned count = 0;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (count < 30 && std::chrono::steady_clock::now() < deadline) {
        auto result = sub.receive();
        assert(result);
        auto sample = std::move(result).value();
        if (!sample) { std::this_thread::sleep_for(1ms); continue; }
        auto timed = rbt::time::decode_stamped_bytes(sample->payload());
        const auto& decoded = std::get<rbt::time::DecodedStampedBytes>(timed);
        const auto frame = decode_frame(decoded.bytes);
        assert(frame.kind == Kind::Accel && decoded.timing.capture);
        assert(decoded.timing.capture->clock == video_clock);
        if (sequence && sample->header() == *sequence) { assert(frame.index == *index); continue; }
        if (sequence) assert(sample->header() == *sequence + 1 && frame.index == *index + 1);
        sequence = sample->header(); index = frame.index;
        ++count;
    }
    assert(count == 30 && scope.events().empty());
    scope.shutdown();
    assert(fixture->stopped == 1);
}

void imu_retry() {
    fixture = std::make_shared<FixtureState>();
    Options options;
    options.serial = "SOFTWARE_FIXTURE";
    options.width = options.height = 2;
    options.imu = true;
    auto camera = make_camera_component(options);
    rbt::EffectScope scope(rbt::generate_uuid(), 1);
    rbt::ActivationContext context(options.component_id, scope.process_instance(), 1, {}, scope);
    assert(camera->activate(context));
    const auto& descriptor = find(context, AccelCapability::TYPE_NAME).descriptor.stream;
    auto fast_result = rbt::time::create_stamped_bytes_subscriber(descriptor.borrowed());
    auto slow_result = rbt::time::create_stamped_bytes_subscriber(descriptor.borrowed());
    assert(fast_result && slow_result);
    auto slow = std::move(slow_result).value();
    std::atomic<unsigned> duplicates{0}, unique{0};
    std::jthread reader([sub = std::move(fast_result).value(), &duplicates, &unique](std::stop_token stop) mutable {
        std::optional<std::uint64_t> previous;
        std::vector<std::uint8_t> last;
        while (!stop.stop_requested()) {
            auto result = sub.receive(); assert(result);
            auto sample = std::move(result).value();
            if (!sample) { std::this_thread::sleep_for(1ms); continue; }
            const auto bytes = sample->payload();
            if (previous && sample->header() == *previous) {
                assert(std::equal(bytes.begin(), bytes.end(), last.begin(), last.end()));
                ++duplicates;
            } else {
                if (previous) assert(sample->header() == *previous + 1);
                previous = sample->header();
                last.assign(bytes.begin(), bytes.end());
                ++unique;
            }
        }
    });
    // Four queued samples fill in 20 ms. Another receiver remains fast, so
    // retrying after the slow receiver recovers must produce identical copies.
    std::this_thread::sleep_for(45ms);
    std::optional<std::uint64_t> previous;
    unsigned count = 0;
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (count < 25 && std::chrono::steady_clock::now() < deadline) {
        auto result = slow.receive(); assert(result);
        auto sample = std::move(result).value();
        if (!sample) { std::this_thread::sleep_for(1ms); continue; }
        if (previous && sample->header() == *previous) continue;
        if (previous) assert(sample->header() == *previous + 1);
        previous = sample->header();
        ++count;
    }
    assert(count == 25 && duplicates > 0 && unique >= 20 && scope.events().empty());
    scope.shutdown();
    reader.request_stop();
    reader.join();
    assert(fixture->stopped == 1);
}

void imu_failure(int fault, const std::string& expected, bool slow_subscriber = false) {
    fixture = std::make_shared<FixtureState>();
    fixture->imu_fault = fault;
    Options options;
    options.serial = "SOFTWARE_FIXTURE";
    options.width = options.height = 2;
    options.imu = true;
    options.frame_timeout_ms = 60;
    auto camera = make_camera_component(options);
    rbt::EffectScope scope(rbt::generate_uuid(), 1);
    rbt::ActivationContext context(options.component_id, scope.process_instance(), 1, {}, scope);
    assert(camera->activate(context));
    std::optional<rbt::time::StampedBytesSubscriber> stalled;
    if (slow_subscriber) {
        auto opened = rbt::time::create_stamped_bytes_subscriber(
            find(context, AccelCapability::TYPE_NAME).descriptor.stream.borrowed());
        assert(opened);
        stalled.emplace(std::move(opened).value());
    }
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (scope.events().empty() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(2ms);
    const auto events = scope.events();
    assert(!events.empty() && events[0].is_critical() && events[0].outcome.is_error());
    if (events[0].outcome.message.find(expected) == std::string::npos)
        std::cerr << "unexpected IMU failure: " << events[0].outcome.message << std::endl;
    assert(events[0].outcome.message.find(expected) != std::string::npos);
    const auto before = std::chrono::steady_clock::now();
    scope.shutdown();
    assert(std::chrono::steady_clock::now() - before < 500ms && fixture->stopped == 1);
}

void failure(bool regression) {
    fixture = std::make_shared<FixtureState>();
    fixture->regress = regression;
    fixture->omit_depth = !regression;
    Options options;
    options.serial = "SOFTWARE_FIXTURE";
    options.width = options.height = 2;
    options.frame_timeout_ms = 60;
    auto camera = make_camera_component(options);
    rbt::EffectScope scope(rbt::generate_uuid(), 1);
    rbt::ActivationContext context(options.component_id, scope.process_instance(), 1, {}, scope);
    assert(camera->activate(context));
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (scope.events().empty() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(5ms);
    const auto events = scope.events();
    assert(events.size() == 1 && events[0].is_critical() && events[0].outcome.is_error());
    assert(events[0].outcome.message.find(regression ? "regressed" : "depth") != std::string::npos);
    scope.shutdown();
    assert(fixture->stopped == 1);
}

void processed_geometry(bool aligned) {
    fixture = std::make_shared<FixtureState>();
    Options options;
    options.serial = "SOFTWARE_FIXTURE";
    options.width = options.height = 2;
    options.align_depth_to_color = aligned;
    options.depth_filters.push_back({ "DecimationFilter", { { "decimate", 2.0 } } });
    auto camera = make_camera_component(options);
    assert(camera->prepare());
    rbt::EffectScope scope(rbt::generate_uuid(), 1);
    rbt::ActivationContext context(options.component_id, scope.process_instance(), 1, {}, scope);
    assert(camera->activate(context));
    auto opened = rbt::time::create_stamped_bytes_subscriber(
        find(context, DepthCapability::TYPE_NAME).descriptor.stream.borrowed());
    assert(opened);
    auto subscriber = std::move(opened).value();
    bool observed = false;
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline && !observed) {
        auto received = subscriber.receive();
        assert(received);
        auto sample = std::move(received).value();
        if (!sample) { std::this_thread::sleep_for(2ms); continue; }
        const auto timed = rbt::time::decode_stamped_bytes(sample->payload());
        const auto* decoded = std::get_if<rbt::time::DecodedStampedBytes>(&timed);
        assert(decoded && decoded->timing.capture);
        const auto frame = decode_frame(decoded->bytes);
        assert(frame.frame_id == (aligned ? "orbbec335/color_optical" : "orbbec335/depth_optical"));
        assert(frame.width == (aligned ? 2U : 1U) && frame.height == frame.width);
        assert(frame.stride == frame.width * 2U && frame.data.size() == frame.width * frame.height * 2U);
        assert(frame.unit_scale == .001);
        observed = true;
    }
    assert(observed);
    using InfoClient = rbt::RpcClient<rbt::orbbec::CameraInfoRequest, rbt::orbbec::CameraInfoResponse>;
    auto info = InfoClient::open(find(context, CameraInfoCapability::TYPE_NAME).descriptor.rpc->borrowed());
    assert(info);
    auto response = info.value().call({ 2, 0 }, 1s);
    assert(response);
    const auto& payload = response.value().payload();
    assert(payload.status == 0 && payload.width == (aligned ? 2U : 1U) && payload.height == payload.width);
    assert(payload.fx == (aligned ? 100 : 60) && payload.fy == (aligned ? 100 : 55));
    assert(payload.cx == (aligned ? 1.0 : .5) && payload.cy == payload.cx);
    assert(std::string(reinterpret_cast<const char*>(payload.frame_id))
        == (aligned ? "orbbec335/color_optical" : "orbbec335/depth_optical"));
    scope.shutdown();
    assert(fixture->stopped == 1);
}
} // namespace

namespace rbt_orbbec {
std::unique_ptr<CaptureDevice> open_device(const Options& options) { return std::make_unique<FixtureDevice>(options); }
std::string sdk_version() { return "SOFTWARE_FIXTURE"; }
} // namespace rbt_orbbec

int main(int argc, char**) {
    if (argc > 1) {
        Options options;
        options.serial = "SOFTWARE_FIXTURE";
        options.width = options.height = 2;
        rbt::RuntimeOptions runtime_options;
        if (const auto* address = std::getenv("RBT_COMPOSER_ADDR")) runtime_options.composer_address = address;
        rbt::ComponentRuntime runtime(make_camera_component(options), runtime_options);
        return runtime.run();
    }
    integration();
    processed_geometry(false);
    processed_geometry(true);
    failure(true);
    failure(false);
    imu_integration();
    imu_retry();
    imu_failure(1, "not contiguous");
    imu_failure(2, "regressed");
    imu_failure(3, "gyro");
    imu_failure(4, "callback queue overflow");
    imu_failure(0, "QueueFull", true);
    std::cout << "SOFTWARE_FIXTURE PASS native_stream RPC TF shutdown replacement regression per_stream_timeout\n";
}

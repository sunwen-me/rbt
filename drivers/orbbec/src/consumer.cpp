#include <rbt_orbbec/component.hpp>
#include <rbt/time_stream.hpp>
#include <rbt/tf_component.hpp>
#include <rbt/wait.hpp>

#include <algorithm>
#include <atomic>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
using namespace rbt_orbbec;
using namespace std::chrono_literals;
struct ConsumerOptions {
    Kind kind { Kind::Depth };
    std::string provider { "orbbec335" }, target;
    std::uint64_t count { 30 }, timeout_seconds { 15 };
    bool allow_unknown { false };
};
struct Completion {
    std::atomic<bool> done { false };
    std::atomic<int> code { 2 };
    void finish(int result) { code.store(result); done.store(true); }
};

Kind parse_kind(std::string_view text) {
    for (const auto kind : { Kind::Color, Kind::Depth, Kind::LeftIr, Kind::RightIr,
                             Kind::Points, Kind::Accel, Kind::Gyro }) {
        if (text == kind_name(kind)) return kind;
    }
    if (text == "left-ir") return Kind::LeftIr;
    if (text == "right-ir") return Kind::RightIr;
    throw std::invalid_argument("unknown stream");
}
std::string capability_type(Kind kind) {
    switch (kind) {
    case Kind::Color: return ColorCapability::TYPE_NAME;
    case Kind::Depth: return DepthCapability::TYPE_NAME;
    case Kind::LeftIr: return LeftIrCapability::TYPE_NAME;
    case Kind::RightIr: return RightIrCapability::TYPE_NAME;
    case Kind::Points: return PointsCapability::TYPE_NAME;
    case Kind::Accel: return AccelCapability::TYPE_NAME;
    case Kind::Gyro: return GyroCapability::TYPE_NAME;
    }
    throw std::invalid_argument("unknown stream");
}

class Consumer final : public rbt::Component {
  public:
    Consumer(ConsumerOptions options, std::shared_ptr<Completion> completion)
        : options_(std::move(options)), completion_(std::move(completion)) {}
    rbt::Result<void> prepare() override { return {}; }
    rbt::ComponentId id() const override { return "orbbec-consumer"; }
    std::vector<rbt::RequirementSpec> requirements() const override {
        std::vector<rbt::RequirementSpec> result { { "frames", capability_type(options_.kind) } };
        if (!options_.target.empty()) {
            result.push_back({ "tf_updates", StaticUpdatesCapability::TYPE_NAME });
            result.push_back({ "tf_snapshot", StaticSnapshotCapability::TYPE_NAME });
        }
        return result;
    }
    rbt::Result<void> activate(rbt::ActivationContext& context) override {
        try {
            // The framework resolves semantic requirements. Verify the expected
            // provider before opening its actual physical stream descriptor.
            auto frames = context.require<void>("frames");
            if (!frames || frames.provider() != options_.provider)
                throw std::runtime_error("resolved provider differs from --provider");
            auto opened = frames.slice_subscriber<std::uint64_t, std::uint8_t>();
            if (!opened) throw std::runtime_error("cannot open resolved camera stream");
            rbt::tf::LookupPolicy policy;
            policy.allow_unknown_uncertainty = options_.allow_unknown;
            rbt::tf::TransformBuffer buffer({}, policy);
            using TfClient = rbt::tf::ScopedStaticTfClient<StaticUpdatesCapability, StaticSnapshotCapability>;
            std::optional<TfClient> tf;
            if (!options_.target.empty()) {
                const auto updates = context.require<StaticUpdatesCapability>("tf_updates");
                const auto& a = frames.binding().descriptor;
                const auto& b = updates.binding().descriptor;
                if (a.provider_component != b.provider_component || a.provider_process != b.provider_process
                    || a.activation != b.activation)
                    throw std::runtime_error("camera stream and TF authority belong to different activations");
                tf.emplace(rbt::tf::require_static_tf<StaticUpdatesCapability, StaticSnapshotCapability>(
                    context, "tf_updates", "tf_snapshot", "orbbec-camera", buffer));
            }
            const auto options = options_;
            const auto completion = completion_;
            const auto process = context.process_instance();
            const auto activation = context.activation_id();
            const auto task = context.spawn_critical("consume-camera",
                [subscriber = std::move(opened).value(), tf = std::move(tf), buffer,
                 options, completion, process, activation](std::stop_token stop) mutable {
                    try {
                        if (tf) tf->refresh_snapshot(1s);
                        auto next_tf_refresh = std::chrono::steady_clock::now() + 2s;
                        auto waiting = rbt::WaitContext::create(process, activation);
                        if (!waiting) throw std::runtime_error("cannot create activation wait context");
                        auto wait = std::move(waiting).value();
                        std::uint64_t count = 0;
                        std::optional<std::uint64_t> last_sequence, last_device_index;
                        std::optional<rbt::time::Timestamp> last_capture;
                        std::array<std::uint8_t, rbt::time::kStampedBytesPrefixBytes + kFrameHeaderBytes + 12> last_imu_payload {};
                        std::vector<rbt::EventSource> sources;
                        sources.push_back(subscriber.event_source(rbt::WaitToken::new_token(1)));
                        if (tf) sources.push_back(tf->event_source(rbt::WaitToken::new_token(2)));
                        std::exception_ptr handler_error;
                        const auto result = wait.run(stop, std::move(sources),
                            [&](rbt::ReadyEvent event, std::size_t budget) {
                                try {
                                    if (event.is_cancellation() || stop.stop_requested())
                                        return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitProgress::continue_waiting());
                                    if (tf) {
                                        (void)tf->drain_updates(8, 100ms);
                                        // Static snapshots have a five-second freshness
                                        // limit even when the factory extrinsics stay fixed.
                                        if (std::chrono::steady_clock::now() >= next_tf_refresh) {
                                            tf->refresh_snapshot(100ms);
                                            next_tf_refresh = std::chrono::steady_clock::now() + 2s;
                                        }
                                    }
                                    std::size_t drained = 0;
                                    for (; drained < budget && !stop.stop_requested(); ++drained) {
                                        auto received = subscriber.receive();
                                        if (!received) throw std::runtime_error("camera receive failed");
                                        auto sample = std::move(received).value();
                                        if (!sample) break;
                                        auto decoded = rbt::time::decode_stamped_bytes(sample->payload());
                                        const auto* timed = std::get_if<rbt::time::DecodedStampedBytes>(&decoded);
                                        if (!timed) throw std::runtime_error("invalid camera timing prefix");
                                        const auto frame = decode_frame(timed->bytes);
                                        if (frame.kind != options.kind) throw std::runtime_error("camera stream kind mismatch");
                                        if (options.kind == Kind::Accel || options.kind == Kind::Gyro) {
                                            const auto payload = sample->payload();
                                            if (payload.size() != last_imu_payload.size())
                                                throw std::runtime_error("unexpected IMU payload size");
                                            if (last_sequence && sample->header() == *last_sequence) {
                                                if (!std::equal(payload.begin(), payload.end(), last_imu_payload.begin()))
                                                    throw std::runtime_error("conflicting IMU retransmission");
                                                continue;
                                            }
                                            if (!timed->timing.capture || timed->timing.capture->clock.kind() != rbt::time::ClockKind::Device)
                                                throw std::runtime_error("IMU capture timestamp/device clock is unavailable");
                                            if (last_sequence && (sample->header() <= *last_sequence || sample->header() - *last_sequence != 1))
                                                throw std::runtime_error("IMU RBT sequence is not contiguous");
                                            if (last_device_index && (frame.index <= *last_device_index || frame.index - *last_device_index != 1))
                                                throw std::runtime_error("IMU SDK frame index is not contiguous");
                                            if (last_capture && (timed->timing.capture->clock != last_capture->clock
                                                || timed->timing.capture->ns < last_capture->ns))
                                                throw std::runtime_error("IMU capture clock changed or timestamp regressed");
                                            last_sequence = sample->header();
                                            last_device_index = frame.index;
                                            last_capture = timed->timing.capture;
                                            std::copy(payload.begin(), payload.end(), last_imu_payload.begin());
                                        }
                                        std::string tf_fields;
                                        if (tf) {
                                            if (!timed->timing.capture) throw std::runtime_error("camera capture time is unavailable");
                                            const auto found = buffer.lookup_at(options.target, frame.frame_id,
                                                *timed->timing.capture,
                                                { timed->timing.capture_uncertainty_ns, {}, nullptr });
                                            const auto& t = found.transform.translation();
                                            tf_fields = ",\"tf_target\":" + json_quote(options.target)
                                                + ",\"translation_m\":[" + std::to_string(t.x()) + ","
                                                + std::to_string(t.y()) + "," + std::to_string(t.z()) + "]";
                                        }
                                        std::cout << "{\"event\":\"frame\",\"stream\":" << json_quote(kind_name(frame.kind))
                                                  << ",\"sequence\":" << sample->header() << ",\"device_index\":" << frame.index
                                                  << ",\"frame_id\":" << json_quote(frame.frame_id)
                                                  << ",\"format\":" << json_quote(format_name(frame.format))
                                                  << ",\"width\":" << frame.width << ",\"height\":" << frame.height
                                                  << ",\"unit_scale\":" << frame.unit_scale
                                                  << ",\"capture_ns\":";
                                        if (timed->timing.capture) std::cout << timed->timing.capture->ns;
                                        else std::cout << "null";
                                        std::cout << ",\"capture_uncertainty_ns\":null" << tf_fields << "}" << std::endl;
                                        // All pixel spans die before their native sample guard.
                                        if (++count >= options.count) {
                                            completion->finish(0);
                                            return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitProgress::stop());
                                        }
                                    }
                                    return rbt::WaitResult<rbt::WaitProgress>(drained ? rbt::WaitProgress::samples(drained)
                                                                                     : rbt::WaitProgress::empty());
                                } catch (...) {
                                    handler_error = std::current_exception();
                                    return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitError::Handler);
                                }
                            });
                        if (stop.stop_requested()) return rbt::TaskOutcome::completed();
                        if (handler_error) std::rethrow_exception(handler_error);
                        if (!result) throw std::runtime_error(std::string("camera wait failed: ") + rbt::to_string(result.error()));
                        return rbt::TaskOutcome::completed();
                    } catch (const std::exception& error) {
                        if (stop.stop_requested()) return rbt::TaskOutcome::completed();
                        std::cerr << "orbbec consumer: " << error.what() << std::endl;
                        completion->finish(1);
                        return rbt::TaskOutcome::error(error.what());
                    }
                });
            (void)task;
            return {};
        } catch (const std::exception& error) {
            completion_->finish(1);
            return rbt::Result<void>::failure(error.what());
        }
    }
  private:
    ConsumerOptions options_;
    std::shared_ptr<Completion> completion_;
};

std::uint64_t positive(std::string_view text) {
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || !result || result > 86400)
        throw std::invalid_argument("count/timeout must be in 1..86400");
    return result;
}
} // namespace

int main(int argc, char** argv) {
    try {
        ConsumerOptions options;
        for (int i = 1; i < argc; ++i) {
            const std::string argument(argv[i]);
            if (argument == "--help") {
                std::cout << "rbt-orbbec-consumer [--provider orbbec335] [--stream depth] [--count 30]"
                             " [--timeout-sec 15] [--tf-target FRAME] [--allow-unknown-uncertainty]\n";
                return 0;
            }
            if (argument == "--allow-unknown-uncertainty") { options.allow_unknown = true; continue; }
            if (++i >= argc) throw std::invalid_argument(argument + " requires a value");
            const std::string value(argv[i]);
            if (argument == "--provider") options.provider = value;
            else if (argument == "--stream") options.kind = parse_kind(value);
            else if (argument == "--count") options.count = positive(value);
            else if (argument == "--timeout-sec") options.timeout_seconds = positive(value);
            else if (argument == "--tf-target") { (void)rbt::tf::FrameId(value); options.target = value; }
            else throw std::invalid_argument("unknown argument: " + argument);
        }
        auto completion = std::make_shared<Completion>();
        rbt::RuntimeOptions runtime_options;
        if (const auto* address = std::getenv("RBT_COMPOSER_ADDR")) runtime_options.composer_address = address;
        rbt::ComponentRuntime runtime(std::make_unique<Consumer>(options, completion), runtime_options);
        std::jthread watchdog([&](std::stop_token stop) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(options.timeout_seconds);
            while (!stop.stop_requested() && !completion->done.load() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(20ms);
            if (!stop.stop_requested()) { if (!completion->done.load()) completion->finish(2); runtime.stop(); }
        });
        const int result = runtime.run();
        watchdog.request_stop();
        return completion->done.load() ? completion->code.load() : result;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

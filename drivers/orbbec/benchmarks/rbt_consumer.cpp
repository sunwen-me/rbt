#include "common.hpp"

#include <rbt/time_stream.hpp>
#include <rbt/wait.hpp>
#include <rbt_orbbec/component.hpp>

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace std::chrono_literals;
using namespace rbt_orbbec;

constexpr std::int64_t kNanosecondsPerSecond = 1000000000LL;
constexpr std::int64_t kMaximumWindowNs = 120LL * kNanosecondsPerSecond;
constexpr std::int64_t kMaximumWarmupNs = 30LL * kNanosecondsPerSecond;
constexpr std::size_t kColorBytes = 640U * 480U * 3U;
constexpr std::size_t kDepthBytes = 640U * 480U * 2U;

struct ConsumerOptions {
    std::int64_t start_ns { 0 };
    std::int64_t end_ns { 0 };
    std::filesystem::path output;
};

struct Completion {
    std::atomic<bool> done { false };
    std::atomic<int> code { 0 };

    void finish(int result) {
        code.store(result, std::memory_order_relaxed);
        done.store(true, std::memory_order_release);
    }
};

std::int64_t parse_ns(std::string_view text, const char* option) {
    std::int64_t result = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc() || parsed.ptr != text.data() + text.size() || result <= 0) {
        throw std::invalid_argument(std::string(option) + " must be a positive integer nanosecond timestamp");
    }
    return result;
}

void validate_frame(const DecodedFrame& frame, Kind kind) {
    const auto expected_format = kind == Kind::Color ? Format::Rgb8 : Format::Depth16;
    const auto expected_stride = kind == Kind::Color ? 640U * 3U : 640U * 2U;
    const auto expected_size = kind == Kind::Color ? kColorBytes : kDepthBytes;
    if (frame.kind != kind || frame.format != expected_format
        || frame.width != 640U || frame.height != 480U
        || frame.stride != expected_stride || frame.data.size() != expected_size) {
        throw std::runtime_error(std::string(kind_name(kind))
                                 + " frame does not match the required 640x480 benchmark profile");
    }
}

class Consumer final : public rbt::Component {
  public:
    Consumer(ConsumerOptions options,
             std::shared_ptr<Completion> completion,
             std::shared_ptr<bench::Stats> color_stats,
             std::shared_ptr<bench::Stats> depth_stats)
        : options_(std::move(options))
        , completion_(std::move(completion))
        , color_stats_(std::move(color_stats))
        , depth_stats_(std::move(depth_stats)) {}

    rbt::Result<void> prepare() override { return {}; }
    rbt::ComponentId id() const override { return "orbbec-rbt-benchmark-consumer"; }

    std::vector<rbt::RequirementSpec> requirements() const override {
        return {
            { "color", ColorCapability::TYPE_NAME },
            { "depth", DepthCapability::TYPE_NAME },
        };
    }

    rbt::Result<void> activate(rbt::ActivationContext& context) override {
        try {
            auto color_capability = context.require<void>("color");
            auto depth_capability = context.require<void>("depth");
            const auto& color_binding = color_capability.binding();
            const auto& depth_binding = depth_capability.binding();
            if (color_binding.provider != depth_binding.provider
                || color_binding.descriptor.provider_process != depth_binding.descriptor.provider_process
                || color_binding.descriptor.activation != depth_binding.descriptor.activation) {
                throw std::runtime_error("color and depth requirements resolved to different camera activations");
            }

            auto opened_color = color_capability.slice_subscriber<std::uint64_t, std::uint8_t>();
            auto opened_depth = depth_capability.slice_subscriber<std::uint64_t, std::uint8_t>();
            if (!opened_color || !opened_depth) {
                throw std::runtime_error("cannot open resolved color and depth subscriptions");
            }

            auto color_subscriber = std::move(opened_color).value();
            auto depth_subscriber = std::move(opened_depth).value();
            auto completion = completion_;
            auto color_stats = color_stats_;
            auto depth_stats = depth_stats_;
            const auto options = options_;
            const auto process = context.process_instance();
            const auto activation = context.activation_id();

            (void)context.spawn_critical(
                "consume-rbt-benchmark-frames",
                [color_subscriber = std::move(color_subscriber),
                 depth_subscriber = std::move(depth_subscriber),
                 completion = std::move(completion),
                 color_stats = std::move(color_stats),
                 depth_stats = std::move(depth_stats), options, process, activation](std::stop_token stop) mutable {
                    try {
                        auto waiting = rbt::WaitContext::create(process, activation);
                        if (!waiting) throw std::runtime_error("cannot create activation wait context");
                        auto wait = std::move(waiting).value();
                        const auto color_token = rbt::WaitToken::new_token(1U);
                        const auto depth_token = rbt::WaitToken::new_token(2U);
                        std::vector<rbt::EventSource> sources;
                        sources.push_back(color_subscriber.event_source(color_token));
                        sources.push_back(depth_subscriber.event_source(depth_token));

                        const auto drain = [&](auto& subscriber,
                                               Kind kind,
                                               bench::Stats& stats,
                                               std::size_t budget) {
                            std::size_t drained = 0U;
                            for (; drained < budget && !stop.stop_requested(); ++drained) {
                                auto received = subscriber.receive();
                                if (!received) throw std::runtime_error("RBT camera receive failed");
                                auto sample = std::move(received).value();
                                if (!sample) break;

                                // This timestamp is deliberately taken before decoding the RBT frame.
                                const auto receive_ns = bench::now_ns();
                                const auto payload = sample->payload();
                                const auto decoded = rbt::time::decode_stamped_bytes(payload);
                                const auto* timed = std::get_if<rbt::time::DecodedStampedBytes>(&decoded);
                                if (timed == nullptr) throw std::runtime_error("invalid RBT sensor timing prefix");
                                if (timed->timing.received.clock.kind() != rbt::time::ClockKind::HostMonotonic) {
                                    throw std::runtime_error("SensorTiming.received is not on the host monotonic clock");
                                }

                                const auto frame = decode_frame(timed->bytes);
                                validate_frame(frame, kind);
                                if (receive_ns >= options.end_ns) break;
                                if (receive_ns < options.start_ns) {
                                    stats.mark_ready();
                                    continue;
                                }
                                stats.add(receive_ns,
                                          timed->timing.received.ns,
                                          frame.index,
                                          frame.data.data(),
                                          frame.data.size());
                            }
                            return drained;
                        };

                        const auto waited = wait.run(
                            stop,
                            sources,
                            [&](rbt::ReadyEvent event, std::size_t budget) {
                                if (event.is_cancellation() || stop.stop_requested()) {
                                    return rbt::WaitProgress::stop();
                                }
                                std::size_t drained = 0U;
                                if (event.source_token() == color_token) {
                                    drained = drain(color_subscriber, Kind::Color, *color_stats, budget);
                                } else if (event.source_token() == depth_token) {
                                    drained = drain(depth_subscriber, Kind::Depth, *depth_stats, budget);
                                } else {
                                    throw std::runtime_error("unexpected RBT benchmark event source");
                                }
                                if (bench::now_ns() >= options.end_ns) {
                                    completion->finish(0);
                                    return rbt::WaitProgress::stop_after_samples(drained);
                                }
                                return drained != 0U ? rbt::WaitProgress::samples(drained)
                                                     : rbt::WaitProgress::empty();
                            });
                        if (!waited && !stop.stop_requested()) {
                            throw std::runtime_error("RBT benchmark wait failed");
                        }
                        return rbt::TaskOutcome::completed();
                    } catch (const std::exception& error) {
                        std::cerr << "rbt consumer: " << error.what() << '\n';
                        completion->finish(1);
                        return rbt::TaskOutcome::error(error.what());
                    }
                });
            return {};
        } catch (const std::exception& error) {
            completion_->finish(1);
            return rbt::Result<void>::failure(error.what());
        }
    }

  private:
    ConsumerOptions options_;
    std::shared_ptr<Completion> completion_;
    std::shared_ptr<bench::Stats> color_stats_;
    std::shared_ptr<bench::Stats> depth_stats_;
};

ConsumerOptions parse_options(int argc, char** argv) {
    ConsumerOptions options;
    bool have_start = false, have_end = false, have_output = false;
    for (int index = 1; index < argc; ++index) {
        const std::string argument(argv[index]);
        if (argument == "--help") {
            std::cout << "rbt-orbbec-bench-consumer --start-ns INTEGER --end-ns INTEGER --output ABS.json\n";
            std::exit(0);
        }
        if (++index >= argc) throw std::invalid_argument(argument + " requires a value");
        const std::string value(argv[index]);
        if (argument == "--start-ns") {
            options.start_ns = parse_ns(value, "--start-ns");
            have_start = true;
        } else if (argument == "--end-ns") {
            options.end_ns = parse_ns(value, "--end-ns");
            have_end = true;
        } else if (argument == "--output") {
            options.output = value;
            have_output = true;
        } else {
            throw std::invalid_argument("unknown argument: " + argument);
        }
    }
    if (!have_start || !have_end || !have_output) {
        throw std::invalid_argument("--start-ns, --end-ns, and --output are required");
    }
    if (!options.output.is_absolute()) throw std::invalid_argument("--output must be an absolute path");
    if (options.end_ns <= options.start_ns || options.end_ns - options.start_ns > kMaximumWindowNs) {
        throw std::invalid_argument("measurement interval must be in (0, 120] seconds");
    }
    const auto now = bench::now_ns();
    if (options.start_ns > now && options.start_ns - now > kMaximumWarmupNs) {
        throw std::invalid_argument("--start-ns is more than 30 seconds in the future");
    }
    if (options.end_ns <= now) throw std::invalid_argument("measurement end is already in the past");
    return options;
}

void write_summary(const ConsumerOptions& options,
                   const bench::Stats& color,
                   const bench::Stats& depth) {
    std::ofstream output(options.output, std::ios::out | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot open output summary path");
    output << "{\"measurement\":{\"start_ns\":" << options.start_ns
           << ",\"end_ns\":" << options.end_ns
           << ",\"interval_seconds\":" << std::fixed << std::setprecision(9)
           << static_cast<double>(options.end_ns - options.start_ns) / 1.0e9
           << "},\"streams\":{\"color\":" << color.json(options.start_ns, options.end_ns)
           << ",\"depth\":" << depth.json(options.start_ns, options.end_ns) << "}}\n";
    output.flush();
    if (!output) throw std::runtime_error("failed writing output summary");
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        auto completion = std::make_shared<Completion>();
        auto color_stats = std::make_shared<bench::Stats>();
        auto depth_stats = std::make_shared<bench::Stats>();
        rbt::RuntimeOptions runtime_options;
        if (const auto* address = std::getenv("RBT_COMPOSER_ADDR")) runtime_options.composer_address = address;
        rbt::ComponentRuntime runtime(
            std::make_unique<Consumer>(options, completion, color_stats, depth_stats), runtime_options);

        std::jthread deadline([&](std::stop_token stop) {
            try {
                while (!stop.stop_requested() && !completion->done.load(std::memory_order_acquire)) {
                    if (bench::now_ns() >= options.end_ns) {
                        runtime.stop();
                        return;
                    }
                    std::this_thread::sleep_for(2ms);
                }
                if (!stop.stop_requested()) runtime.stop();
            } catch (const std::exception& error) {
                std::cerr << "rbt consumer: " << error.what() << '\n';
                completion->finish(1);
                runtime.stop();
            }
        });

        const auto runtime_result = runtime.run();
        deadline.request_stop();
        deadline.join();

        if (completion->code.load(std::memory_order_acquire) != 0) return 1;
        if (runtime_result != 0) return runtime_result;
        if (bench::now_ns() < options.end_ns) {
            throw std::runtime_error("managed runtime exited before the measurement window ended");
        }
        write_summary(options, *color_stats, *depth_stats);
        if (color_stats->count() == 0U || depth_stats->count() == 0U) return 1;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "rbt consumer: " << error.what() << '\n';
        return 1;
    }
}

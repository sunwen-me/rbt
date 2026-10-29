// SOFTWARE_FIXTURE consumer: resolve the native Composer capability and
// validate each delivered StampedBytes timing envelope.
#include <rbt_m3t/app_config.hpp>
#include <rbt/orbbec.hpp>
#include <rbt/runtime.hpp>
#include <rbt/time_stream.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

namespace {

using namespace std::chrono_literals;

constexpr std::string_view kFixtureClockPrefix = "534f4654574152455f46495854555245";

struct Options {
    std::uint64_t count { 20U };
    std::uint64_t timeout_seconds { 80U };
    std::string provider { "m3t-so101" };
};

struct Completion {
    std::atomic<bool> done { false };
    std::atomic<int> result { 2 };

    void finish(int code) {
        result.store(code, std::memory_order_release);
        done.store(true, std::memory_order_release);
    }
};

struct Observation {
    std::uint64_t count {};
    std::optional<std::uint64_t> previous_sequence;
    std::optional<std::int64_t> previous_capture_ns;
    std::optional<rbt::time::ClockIdentity> clock;
};

[[nodiscard]] std::uint64_t parse_unsigned(std::string_view text,
                                           std::string_view option,
                                           std::uint64_t maximum) {
    std::uint64_t value = 0U;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc {} || parsed.ptr != text.data() + text.size() || value == 0U
        || value > maximum) {
        throw std::invalid_argument(std::string(option) + " must be a positive bounded integer");
    }
    return value;
}

[[nodiscard]] Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: rbt_m3t_tracking_output_consumer_test [--count N] "
                         "[--timeout-sec N] [--provider ID]\n";
            std::exit(0);
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument(std::string(argument) + " requires a value");
        }
        const std::string_view value(argv[++index]);
        if (argument == "--count") {
            options.count = parse_unsigned(value, argument, 1'000'000U);
        } else if (argument == "--timeout-sec") {
            options.timeout_seconds = parse_unsigned(value, argument, 600U);
        } else if (argument == "--provider") {
            options.provider = value;
        } else {
            throw std::invalid_argument("unknown consumer option: " + std::string(argument));
        }
    }
    if (options.provider.empty()) {
        throw std::invalid_argument("--provider must not be empty");
    }
    return options;
}

[[nodiscard]] bool is_fixture_clock(std::string_view source) {
    return source.size() == 96U && source.starts_with(kFixtureClockPrefix)
           && std::all_of(source.begin() + static_cast<std::ptrdiff_t>(kFixtureClockPrefix.size()),
                          source.end(), [](char byte) { return byte == '0'; });
}

class TrackingOutputConsumer final : public rbt::Component {
  public:
    TrackingOutputConsumer(Options options, std::shared_ptr<Completion> completion)
        : options_{std::move(options)}, completion_{std::move(completion)} {}

    rbt::Result<void> prepare() override { return {}; }
    rbt::ComponentId id() const override { return "m3t-output-verifier"; }
    std::vector<rbt::RequirementSpec> requirements() const override {
        return { { "tracking", rbt_m3t::TrackingCapability::TYPE_NAME } };
    }

    rbt::Result<void> activate(rbt::ActivationContext& context) override {
        auto tracking = context.require<rbt_m3t::TrackingCapability>("tracking");
        if (!tracking) return rbt::Result<void>::failure(tracking.error());
        if (tracking.provider() != options_.provider) {
            return rbt::Result<void>::failure("resolved M3T output provider does not match --provider");
        }
        auto subscriber = tracking.slice_subscriber<std::uint64_t, std::uint8_t>();
        if (!subscriber) return rbt::Result<void>(subscriber.error());

        const auto provider = tracking.provider();
        const auto target_count = options_.count;
        const auto completion = completion_;
        std::cout << "{\"event\":\"consumer_ready\",\"mode\":\"SOFTWARE_FIXTURE\","
                     "\"provider\":" << rbt_orbbec::json_quote(provider)
                  << ",\"count\":" << target_count << "}" << std::endl;
        (void)context.spawn_critical("m3t-tracking-output-verifier",
            [subscriber = std::move(subscriber).value(), provider, target_count, completion]
            (std::stop_token stop) mutable -> rbt::TaskOutcome {
                Observation observation;
                try {
                    while (!stop.stop_requested()) {
                        auto received = subscriber.receive();
                        if (!received) {
                            if (stop.stop_requested()) return rbt::TaskOutcome::completed();
                            throw std::runtime_error("tracking output receive failed: "
                                + std::string(rbt::to_string(received.error())));
                        }
                        auto maybe_sample = std::move(received).value();
                        if (!maybe_sample) {
                            std::this_thread::sleep_for(1ms);
                            continue;
                        }
                        auto sample = std::move(*maybe_sample);
                        const auto sequence = sample.header();
                        const auto decoded_result = rbt::time::decode_stamped_bytes(sample.payload());
                        const auto* decoded = std::get_if<rbt::time::DecodedStampedBytes>(&decoded_result);
                        if (decoded == nullptr || !decoded->timing.capture.has_value()
                            || decoded->bytes.empty()) {
                            throw std::runtime_error("tracking output has invalid StampedBytes timing or payload");
                        }
                        const auto& timing = decoded->timing;
                        if (timing.capture->clock.kind() != rbt::time::ClockKind::Device
                            || timing.received.clock.kind() != rbt::time::ClockKind::HostMonotonic
                            || timing.capture_uncertainty_ns.has_value()
                            || timing.received_uncertainty_ns.has_value()
                            || timing.capture->ns <= 0 || timing.received.ns <= 0
                            || !is_fixture_clock(timing.capture->clock.source_id().to_hex())
                            || timing.capture->clock.generation() != 0U) {
                            throw std::runtime_error("tracking output clock or unknown-uncertainty contract failed");
                        }
                        if (observation.clock.has_value()
                            && *observation.clock != timing.capture->clock) {
                            throw std::runtime_error("synthetic device clock identity changed during a fixture run");
                        }
                        if (observation.previous_sequence.has_value()
                            && sequence <= *observation.previous_sequence) {
                            throw std::runtime_error("tracking output sequence did not increase");
                        }
                        if (observation.previous_capture_ns.has_value()
                            && timing.capture->ns <= *observation.previous_capture_ns) {
                            throw std::runtime_error("tracking output capture timestamp did not increase");
                        }

                        const auto source = timing.capture->clock.source_id().to_hex();
                        const std::string estimate(
                            reinterpret_cast<const char*>(decoded->bytes.data()), decoded->bytes.size());
                        std::cout << "{\"event\":\"tracking_sample\","
                                     "\"mode\":\"SOFTWARE_FIXTURE\",\"hardware\":\"NOT RUN\","
                                     "\"provider\":" << rbt_orbbec::json_quote(provider)
                                  << ",\"sequence\":" << sequence
                                  << ",\"capture_ns\":" << timing.capture->ns
                                  << ",\"received_ns\":" << timing.received.ns
                                  << ",\"capture_clock_kind\":\"Device\""
                                     ",\"received_clock_kind\":\"HostMonotonic\""
                                     ",\"capture_clock_source\":"
                                  << rbt_orbbec::json_quote(source)
                                  << ",\"capture_clock_generation\":"
                                  << timing.capture->clock.generation()
                                  << ",\"capture_uncertainty_ns\":null"
                                     ",\"received_uncertainty_ns\":null"
                                     ",\"estimate\":" << estimate << "}" << std::endl;

                        observation.previous_sequence = sequence;
                        observation.previous_capture_ns = timing.capture->ns;
                        observation.clock = timing.capture->clock;
                        ++observation.count;
                        if (observation.count < target_count) continue;

                        std::cout << "{\"event\":\"tracking_samples_consumed\","
                                     "\"mode\":\"SOFTWARE_FIXTURE\",\"hardware\":\"NOT RUN\","
                                     "\"provider\":" << rbt_orbbec::json_quote(provider)
                                  << ",\"count\":" << observation.count << "}" << std::endl;
                        completion->finish(0);
                        return rbt::TaskOutcome::completed();
                    }
                    return rbt::TaskOutcome::completed();
                } catch (const std::exception& error) {
                    if (stop.stop_requested()) return rbt::TaskOutcome::completed();
                    std::cerr << "tracking output consumer: " << error.what() << std::endl;
                    completion->finish(1);
                    return rbt::TaskOutcome::error(error.what());
                }
            });
        return {};
    }

  private:
    Options options_;
    std::shared_ptr<Completion> completion_;
};

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        auto completion = std::make_shared<Completion>();
        rbt::RuntimeOptions runtime_options;
        if (const char* address = std::getenv("RBT_COMPOSER_ADDR")) {
            runtime_options.composer_address = address;
        }
        rbt::ComponentRuntime runtime(
            std::make_unique<TrackingOutputConsumer>(options, completion), std::move(runtime_options));
        std::jthread watchdog([&](std::stop_token stop) {
            const auto deadline = std::chrono::steady_clock::now()
                                  + std::chrono::seconds(options.timeout_seconds);
            while (!stop.stop_requested() && !completion->done.load(std::memory_order_acquire)) {
                if (runtime.state().kind == rbt::ComponentStateKind::Failed) {
                    completion->finish(1);
                    break;
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    std::cerr << "tracking output consumer timed out waiting for estimate batches\n";
                    completion->finish(2);
                    break;
                }
                std::this_thread::sleep_for(20ms);
            }
            if (!stop.stop_requested()) runtime.stop();
        });
        const int runtime_result = runtime.run();
        watchdog.request_stop();
        return completion->done.load(std::memory_order_acquire)
                   ? completion->result.load(std::memory_order_acquire)
                   : runtime_result;
    } catch (const std::exception& error) {
        std::cerr << "rbt_m3t_tracking_output_consumer_test: " << error.what() << '\n';
        return 2;
    }
}

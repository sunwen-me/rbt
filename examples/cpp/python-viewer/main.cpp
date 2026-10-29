#include <rbt/runtime.hpp>

#include <rbt/sensor.hpp>
#include <rbt/wait.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

struct Detections {
    inline static constexpr char TYPE_NAME[] = "Detections";
};

rbt::sensor::PointXYZI expected_point(std::size_t index) {
    const float value = static_cast<float>(index);
    return rbt::sensor::PointXYZI { value, value * 2.0F, -value, static_cast<float>(index % 31U) };
}

std::uint64_t point_checksum(const rbt::sensor::PointXYZI& point) {
    std::uint64_t checksum = std::bit_cast<std::uint32_t>(point.x);
    checksum = checksum * 31U + std::bit_cast<std::uint32_t>(point.y);
    checksum = checksum * 31U + std::bit_cast<std::uint32_t>(point.z);
    return checksum * 31U + std::bit_cast<std::uint32_t>(point.intensity);
}

bool points_equal(const rbt::sensor::PointXYZI& actual,
                  const rbt::sensor::PointXYZI& expected) {
    return actual.x == expected.x && actual.y == expected.y && actual.z == expected.z
        && actual.intensity == expected.intensity;
}

class ViewerComponent final : public rbt::Component {
  public:
    rbt::Result<void> prepare() override {
        return {};
    }

    rbt::ComponentId id() const override {
        return "viewer";
    }

    std::vector<rbt::RequirementSpec> requirements() const override {
        return { { "detections", "Detections" } };
    }

    rbt::Result<void> activate(rbt::ActivationContext& context) override {
        auto detections = context.require<Detections>("detections");
        if (!detections) {
            return rbt::Result<void>::failure(detections.error());
        }
        auto subscriber = detections.slice_subscriber<rbt::sensor::PointCloudHeader,
                                                       rbt::sensor::PointXYZI>();
        if (!subscriber) {
            return rbt::Result<void>(subscriber.error());
        }

        const auto component = context.component();
        const auto process = context.process_instance();
        const auto activation = context.activation_id();
        const auto input_capability = detections.capability_instance();
        const auto provider = detections.provider();
        std::cout << "PYTHON_VIEWER ACTIVATED component=" << component
                  << " process_instance=" << process
                  << " activation=" << activation
                  << " input_capability_instance=" << input_capability
                  << " provider=" << provider << std::endl;

        const auto task = context.spawn_critical(
            "receive",
            [subscriber = std::move(subscriber).value(),
             component,
             process,
             activation,
             input_capability,
             provider](std::stop_token stop) mutable -> rbt::TaskOutcome {
                auto wait_context_result = rbt::WaitContext::create(process, activation);
                if (!wait_context_result) {
                    return rbt::TaskOutcome::error(
                        "python viewer failed to create wait context: "
                        + std::string(rbt::to_string(wait_context_result.error())));
                }
                auto wait_context = std::move(wait_context_result).value();
                std::optional<std::string> task_error;
                std::optional<std::uint64_t> previous_sequence;
                std::uint64_t samples = 0U;

                const auto wait_result = wait_context.run(
                    stop,
                    { subscriber.event_source(rbt::WaitToken::new_token(1U)) },
                    [&](rbt::ReadyEvent event, std::size_t budget) {
                        if (event.is_cancellation() || stop.stop_requested()) {
                            return rbt::WaitResult<rbt::WaitProgress>(
                                rbt::WaitProgress::continue_waiting());
                        }

                        std::size_t drained = 0U;
                        for (std::size_t index = 0U; index < budget; ++index) {
                            if (stop.stop_requested()) {
                                break;
                            }

                            auto received = subscriber.receive();
                            if (!received) {
                                task_error = "python viewer failed to receive PointCloud";
                                return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitError::Handler);
                            }
                            auto guarded = std::move(received).value();
                            if (!guarded.has_value()) {
                                break;
                            }

                            auto sample = std::move(guarded).value();
                            const auto& header = sample.header();
                            const auto payload = sample.payload();
                            const bool reserved_zero = std::all_of(
                                std::begin(header.reserved), std::end(header.reserved), [](std::uint8_t byte) {
                                    return byte == 0U;
                                });
                            if (header.timestamp_ns != header.sequence * 1'000'000U
                                || header.width != payload.size() || header.height != 1U
                                || header.dense != 1U || !reserved_zero) {
                                task_error = "python viewer received an invalid PointCloud header";
                                return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitError::Handler);
                            }
                            if (previous_sequence.has_value() && header.sequence <= *previous_sequence) {
                                task_error = "python viewer received a non-increasing PointCloud sequence";
                                return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitError::Handler);
                            }

                            std::uint64_t checksum = 0U;
                            for (std::size_t point_index = 0U; point_index < payload.size(); ++point_index) {
                                const auto expected = expected_point(point_index);
                                if (!points_equal(payload[point_index], expected)) {
                                    task_error = "python viewer received a non-deterministic PointCloud sample";
                                    return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitError::Handler);
                                }
                                checksum += point_checksum(payload[point_index]);
                            }

                            previous_sequence = header.sequence;
                            ++samples;
                            ++drained;
                            std::cout << "PYTHON_VIEWER SAMPLE component=" << component
                                      << " process_instance=" << process
                                      << " activation=" << activation
                                      << " input_capability_instance=" << input_capability
                                      << " provider=" << provider
                                      << " sequence=" << header.sequence
                                      << " checksum=" << checksum
                                      << " points=" << payload.size() << std::endl;
                        }

                        return rbt::WaitResult<rbt::WaitProgress>(
                            drained == 0U ? rbt::WaitProgress::empty()
                                          : rbt::WaitProgress::samples(drained));
                    });

                if (!wait_result) {
                    if (task_error.has_value()) {
                        return rbt::TaskOutcome::error(std::move(*task_error));
                    }
                    return rbt::TaskOutcome::error(
                        "python viewer wait failed: "
                        + std::string(rbt::to_string(wait_result.error())));
                }
                if (wait_result.value() == rbt::WaitExit::Cancelled
                    || wait_result.value() == rbt::WaitExit::Stopped) {
                    std::cout << "PYTHON_VIEWER COMPLETE component=" << component
                              << " process_instance=" << process
                              << " activation=" << activation
                              << " input_capability_instance=" << input_capability
                              << " provider=" << provider
                              << " samples=" << samples << std::endl;
                    return rbt::TaskOutcome::completed();
                }
                return rbt::TaskOutcome::error("python viewer wait exited unexpectedly");
            });
        (void)task;
        return {};
    }
};

} // namespace

int main() {
    rbt::RuntimeOptions options;
    if (const char* address = std::getenv("RBT_COMPOSER_ADDR"); address != nullptr && *address != '\0') {
        options.composer_address = address;
    }
    rbt::ComponentRuntime runtime(std::make_unique<ViewerComponent>(), std::move(options));
    return runtime.run();
}

#include <rbt/runtime.hpp>

#include <rbt/demo.hpp>
#include <rbt/sensor.hpp>
#include <rbt/wait.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace {

struct Camera {
    inline static constexpr char TYPE_NAME[] = "Camera";
};

struct Detections {
    inline static constexpr char TYPE_NAME[] = "Detections";
};

[[nodiscard]] std::optional<std::uint64_t> environment_u64(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    try {
        const std::string text(value);
        std::size_t consumed = 0U;
        const auto parsed = std::stoull(text, &consumed, 10);
        if (consumed != text.size()) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(parsed);
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] bool environment_enabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' && std::string(value) != "0";
}

class Detector final : public rbt::Component {
  public:
    rbt::Result<void> prepare() override {
        model_id_ = 1U;
        fail_after_ = environment_u64("RBT_TEST_FAIL_AFTER");
        exit_after_ = environment_u64("RBT_TEST_EXIT_AFTER");
        pause_ms_ = environment_u64("RBT_TEST_PAUSE_MS");
        trace_frames_ = environment_enabled("RBT_TEST_TRACE_FRAMES");
        std::cout << "MODEL LOADED id=" << model_id_ << std::endl;
        return {};
    }

    rbt::ComponentId id() const override {
        return "detector";
    }

    std::vector<rbt::RequirementSpec> requirements() const override {
        return { { "camera", "Camera" } };
    }

    rbt::Result<void> activate(rbt::ActivationContext& context) override {
        auto camera = context.require<Camera>("camera");
        if (!camera) {
            return rbt::Result<void>::failure(camera.error());
        }
        auto subscriber = camera.slice_subscriber<rbt::sensor::ImageHeader, std::uint8_t>();
        if (!subscriber) {
            return rbt::Result<void>(subscriber.error());
        }

        const auto output_capability = rbt::generate_uuid();
        const rbt::OwnedStreamDescriptor output_stream = rbt::OwnedStreamDescriptor::from(
            rbt::StreamDescriptor("rbt/cap/" + output_capability + "/stream",
                                  rbt::demo::DETECTION_DESCRIPTOR,
                                  rbt::StreamProfile::Sensor));
        auto publisher = rbt::FixedPublisher<rbt::demo::Detection>::create(
            output_stream.borrowed(), rbt::demo::DETECTION_DESCRIPTOR);
        if (!publisher) {
            return rbt::Result<void>(publisher.error());
        }
        const auto provided = context.provide<Detections>(output_capability, output_stream);
        if (!provided) {
            return rbt::Result<void>::failure("failed to register Detections capability");
        }

        std::cout << "DETECTOR ACTIVATED component=" << context.component()
                  << " process_instance=" << context.process_instance()
                  << " activation=" << context.activation_id()
                  << " capability_instance=" << provided.capability_instance()
                  << " input_capability_instance=" << camera.capability_instance()
                  << " model_id=" << model_id_ << std::endl;

        const auto component = context.component();
        const auto process = context.process_instance();
        const auto activation = context.activation_id();
        const auto model_id = model_id_;
        const auto fail_after = fail_after_;
        const auto exit_after = exit_after_;
        const auto pause_ms = pause_ms_;
        const auto trace_frames = trace_frames_;
        const auto input_capability = camera.capability_instance();
        const auto output_instance = provided.capability_instance();
        const auto task = context.spawn_critical(
            "inference",
            [subscriber = std::move(subscriber).value(),
             publisher = std::move(publisher).value(),
             component,
             process,
             activation,
             input_capability,
             output_instance,
             model_id,
             fail_after,
             exit_after,
             pause_ms,
             trace_frames](std::stop_token stop) mutable -> rbt::TaskOutcome {
                std::uint64_t processed = 0U;
                if (exit_after == 0U && exit_after.has_value()) {
                    std::exit(1);
                }

                auto context_result = rbt::WaitContext::create(process, activation);
                if (!context_result) {
                    return rbt::TaskOutcome::error(
                        "detector failed to create wait context: "
                        + std::string(rbt::to_string(context_result.error())));
                }
                auto context = std::move(context_result).value();
                std::optional<std::string> task_error;
                const auto token = rbt::WaitToken::new_token(1U);
                const auto wait_result = context.run(
                    stop,
                    { subscriber.event_source(token) },
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

                            auto image = subscriber.receive();
                            if (!image) {
                                task_error = "detector failed to receive image";
                                return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitError::Handler);
                            }
                            auto received_image = std::move(image).value();
                            if (!received_image.has_value()) {
                                break;
                            }
                            if (pause_ms.has_value() && *pause_ms > 0U) {
                                const auto deadline = std::chrono::steady_clock::now()
                                    + std::chrono::milliseconds(*pause_ms);
                                while (!stop.stop_requested()
                                       && std::chrono::steady_clock::now() < deadline) {
                                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                                }
                                if (stop.stop_requested()) {
                                    break;
                                }
                            }
                            const auto sequence = received_image->header().sequence;
                            const auto payload = received_image->payload();
                            const auto value = payload.empty()
                                                    ? 0U
                                                    : static_cast<std::uint64_t>(payload.front()) * 10U;
                            received_image.reset();

                            auto detection = publisher.loan();
                            if (!detection) {
                                task_error = "detector failed to loan detection";
                                return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitError::Handler);
                            }
                            detection.value().payload().sequence = sequence;
                            detection.value().payload().value = value;
                            if (!detection.value().send()) {
                                task_error = "detector failed to publish detection";
                                return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitError::Handler);
                            }
                            ++processed;
                            ++drained;
                            if (trace_frames) {
                                std::cout << "DETECTOR PROCESS component=" << component
                                          << " process_instance=" << process
                                          << " activation=" << activation
                                          << " capability_instance=" << output_instance
                                          << " input_capability_instance=" << input_capability
                                          << " model_id=" << model_id
                                          << " sequence=" << sequence
                                          << " value=" << value << std::endl;
                            }
                            if (exit_after.has_value() && processed >= *exit_after) {
                                std::exit(1);
                            }
                            if (fail_after.has_value() && processed >= *fail_after) {
                                task_error = "test failure after processing "
                                             + std::to_string(processed) + " frames";
                                return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitError::Handler);
                            }
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
                        "detector wait failed: "
                        + std::string(rbt::to_string(wait_result.error())));
                }
                if (wait_result.value() == rbt::WaitExit::Cancelled
                    || wait_result.value() == rbt::WaitExit::Stopped) {
                    return rbt::TaskOutcome::completed();
                }
                return rbt::TaskOutcome::error("detector wait failed: unknown exit");
            });
        (void)task;
        return {};
    }

  private:
    std::uint64_t model_id_ { 0U };
    std::optional<std::uint64_t> fail_after_;
    std::optional<std::uint64_t> exit_after_;
    std::optional<std::uint64_t> pause_ms_;
    bool trace_frames_ { false };
};

} // namespace

int main() {
    rbt::RuntimeOptions options;
    if (const char* address = std::getenv("RBT_COMPOSER_ADDR"); address != nullptr && *address != '\0') {
        options.composer_address = address;
    }
    rbt::ComponentRuntime runtime(std::make_unique<Detector>(), std::move(options));
    return runtime.run();
}

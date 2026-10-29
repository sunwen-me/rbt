#include <rbt/runtime.hpp>

#include <rbt/demo.hpp>
#include <rbt/wait.hpp>

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>

namespace {

struct Detections {
    inline static constexpr char TYPE_NAME[] = "Detections";
};

class ViewerComponent final : public rbt::Component {
  public:
    rbt::Result<void> prepare() override {
        return {};
    }

    rbt::ComponentId id() const override {
        return "cpp-viewer";
    }

    std::vector<rbt::RequirementSpec> requirements() const override {
        return { { "detections", "Detections" } };
    }

    rbt::Result<void> activate(rbt::ActivationContext& context) override {
        auto detections = context.require<Detections>("detections");
        if (!detections) {
            return rbt::Result<void>::failure(detections.error());
        }
        auto subscriber = detections.subscriber<rbt::demo::Detection>();
        if (!subscriber) {
            return rbt::Result<void>(subscriber.error());
        }

        std::cout << "VIEWER ACTIVATED component=" << context.component()
                  << " process_instance=" << context.process_instance()
                  << " activation=" << context.activation_id()
                  << " capability_instance=" << detections.capability_instance()
                  << " provider=" << detections.provider() << std::endl;

        const auto process = context.process_instance();
        const auto activation = context.activation_id();
        const auto task = context.spawn_critical(
            "receive",
            [subscriber = std::move(subscriber).value(), process, activation](std::stop_token stop) mutable {
                auto context_result = rbt::WaitContext::create(process, activation);
                if (!context_result) {
                    return rbt::TaskOutcome::error(
                        "viewer failed to create wait context: "
                        + std::string(rbt::to_string(context_result.error())));
                }
                auto wait_context = std::move(context_result).value();
                std::optional<std::string> task_error;
                auto next_report = std::chrono::steady_clock::now();
                const auto token = rbt::WaitToken::new_token(1U);
                const auto wait_result = wait_context.run(
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

                            auto sample = subscriber.receive();
                            if (!sample) {
                                task_error = "viewer failed to receive detection";
                                return rbt::WaitResult<rbt::WaitProgress>(rbt::WaitError::Handler);
                            }
                            auto received_sample = std::move(sample).value();
                            if (!received_sample.has_value()) {
                                break;
                            }
                            const auto& detection = received_sample->payload();
                            const auto now = std::chrono::steady_clock::now();
                            if (now >= next_report) {
                                std::cout << "VIEW detection sequence=" << detection.sequence
                                          << " value=" << detection.value << std::endl;
                                next_report = now + std::chrono::seconds(1);
                            }
                            ++drained;
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
                        "viewer wait failed: "
                        + std::string(rbt::to_string(wait_result.error())));
                }
                if (wait_result.value() == rbt::WaitExit::Cancelled
                    || wait_result.value() == rbt::WaitExit::Stopped) {
                    return rbt::TaskOutcome::completed();
                }
                return rbt::TaskOutcome::error("viewer wait failed: unknown exit");
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

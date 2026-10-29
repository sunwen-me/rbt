#include <rbt/runtime.hpp>

#include <rbt/sensor.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>

namespace {

struct Camera {
    inline static constexpr char TYPE_NAME[] = "Camera";
};

class CameraComponent final : public rbt::Component {
  public:
    rbt::Result<void> prepare() override {
        return {};
    }

    rbt::ComponentId id() const override {
        return "cpp-camera";
    }

    std::vector<rbt::RequirementSpec> requirements() const override {
        return {};
    }

    rbt::Result<void> activate(rbt::ActivationContext& context) override {
        const auto capability = rbt::generate_uuid();
        constexpr std::size_t kImageBytes = 64U;
        const auto descriptor = rbt::OwnedStreamDescriptor::from(
            rbt::StreamDescriptor("rbt/cap/" + capability + "/stream",
                                  rbt::sensor::IMAGE_DESCRIPTOR,
                                  rbt::StreamProfile::Sensor));
        auto publisher = rbt::SlicePublisher<rbt::sensor::ImageHeader, std::uint8_t>::create(
            descriptor.borrowed(), rbt::sensor::IMAGE_DESCRIPTOR, kImageBytes);
        if (!publisher) {
            return rbt::Result<void>(publisher.error());
        }
        const auto provided = context.provide<Camera>(capability, descriptor);
        if (!provided) {
            return rbt::Result<void>::failure("failed to register Camera capability");
        }

        std::cout << "CAMERA ACTIVATED component=" << context.component()
                  << " process_instance=" << context.process_instance()
                  << " activation=" << context.activation_id()
                  << " capability_instance=" << capability << std::endl;

        const auto task = context.spawn_critical(
            "publish",
            [publisher = std::move(publisher).value(), kImageBytes](std::stop_token stop) mutable {
                std::uint64_t sequence = 1U;
                while (!stop.stop_requested()) {
                    auto sample = publisher.loan_slice(kImageBytes);
                    if (!sample) {
                        return rbt::TaskOutcome::error("camera failed to loan image");
                    }
                    auto& header = sample.value().header();
                    header.sequence = sequence;
                    header.timestamp_ns = sequence * 100'000'000U;
                    header.width = static_cast<std::uint32_t>(kImageBytes);
                    header.height = 1U;
                    header.stride = static_cast<std::uint32_t>(kImageBytes);
                    header.encoding = rbt::sensor::GRAY8;
                    std::fill(sample.value().payload().begin(), sample.value().payload().end(), 0xC3U);
                    if (!sample.value().send()) {
                        return rbt::TaskOutcome::error("camera failed to publish image");
                    }
                    ++sequence;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                return rbt::TaskOutcome::completed();
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
    rbt::ComponentRuntime runtime(std::make_unique<CameraComponent>(), std::move(options));
    return runtime.run();
}

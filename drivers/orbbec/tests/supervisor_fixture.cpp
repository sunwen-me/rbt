// SOFTWARE_FIXTURE: this executable uses only an explicit state file and never opens hardware.
#include <rbt_orbbec/component.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace rbt_orbbec {
namespace {

using namespace std::chrono_literals;
constexpr std::string_view kFixtureSerial = "SOFTWARE_FIXTURE";
constexpr std::string_view kStateEnvironment = "RBT_ORBBEC_SOFTWARE_FIXTURE_STATE";
constexpr std::string_view kGenerationEnvironment = "RBT_ORBBEC_SOFTWARE_FIXTURE_GENERATION";

[[nodiscard]] std::string state_file() {
    const char* value = std::getenv(kStateEnvironment.data());
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error("SOFTWARE_FIXTURE requires RBT_ORBBEC_SOFTWARE_FIXTURE_STATE");
    }
    return value;
}

[[nodiscard]] std::string generation_file() {
    const char* value = std::getenv(kGenerationEnvironment.data());
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error("SOFTWARE_FIXTURE requires RBT_ORBBEC_SOFTWARE_FIXTURE_GENERATION");
    }
    return value;
}

[[nodiscard]] std::uint64_t fixture_disconnect_generation(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot read SOFTWARE_FIXTURE generation file");
    }
    std::string line;
    std::uint64_t target_generation = 0U;
    bool found_target = false;
    while (std::getline(input, line)) {
        constexpr std::string_view target_prefix = "target=";
        if (!std::string_view(line).starts_with(target_prefix)) {
            continue;
        }
        const std::string_view value(line.data() + target_prefix.size(), line.size() - target_prefix.size());
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), target_generation);
        if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size()) {
            throw std::runtime_error("SOFTWARE_FIXTURE target generation must be an unsigned integer");
        }
        found_target = true;
        break;
    }
    if (!found_target) {
        throw std::runtime_error("SOFTWARE_FIXTURE generation file must contain target=<integer>");
    }
    return target_generation;
}

[[nodiscard]] bool fixture_present(const std::string& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("cannot read SOFTWARE_FIXTURE state file");
    }
    std::string value;
    std::getline(input, value);
    if (value == "present") {
        return true;
    }
    if (value == "absent") {
        return false;
    }
    throw std::runtime_error("SOFTWARE_FIXTURE state file must contain 'present' or 'absent'");
}

class FixtureDevice final : public CaptureDevice {
  public:
    FixtureDevice(Options options, std::string state_path)
        : options_(std::move(options)), state_path_(std::move(state_path)) {
        calibration_.device = { "SOFTWARE_FIXTURE", options_.serial, "fixture", "state-file", 0U };
        if (options_.color) {
            calibration_.videos.push_back({ Kind::Color, 2U, 2U, 30U, Format::Rgb8,
                                            100.0, 100.0, 1.0, 1.0, 0U, {} });
        }
        if (options_.depth) {
            calibration_.videos.push_back({ Kind::Depth, 2U, 2U, 30U, Format::Depth16,
                                            100.0, 100.0, 1.0, 1.0, 0U, {} });
            if (options_.color) {
                calibration_.extrinsics.push_back({ Kind::Depth, Kind::Color,
                    { 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 }, { 0.1, 0.2, 0.3 } });
            }
        }
    }

    ~FixtureDevice() override {
        stop();
    }

    [[nodiscard]] const Calibration& calibration() const override {
        return calibration_;
    }

    std::vector<Frame> poll(std::chrono::milliseconds timeout) override {
        if (timeout > 0ms) {
            std::this_thread::sleep_for(std::min(timeout, 10ms));
        }
        if (stopped_.load(std::memory_order_acquire) || !fixture_present(state_path_)) {
            return {};
        }

        const auto index = ++video_index_;
        std::vector<Frame> frames;
        if (options_.color) {
            auto data = std::make_shared<std::vector<std::uint8_t>>(12U, 0U);
            frames.push_back({ Kind::Color, Format::Rgb8, 2U, 2U, 6U, index, index * 33333U,
                               1.0, *data, data });
        }
        if (options_.depth) {
            auto data = std::make_shared<std::vector<std::uint8_t>>(8U, 0U);
            (*data)[0] = 0xe8U;
            (*data)[1] = 0x03U;
            frames.push_back({ Kind::Depth, Format::Depth16, 2U, 2U, 4U, index, index * 33333U,
                               0.001, *data, data });
        }
        return frames;
    }

    std::vector<Frame> poll_imu(std::chrono::milliseconds timeout) override {
        if (timeout > 0ms) {
            std::this_thread::sleep_for(std::min(timeout, 5ms));
        }
        if (stopped_.load(std::memory_order_acquire) || !options_.imu || !fixture_present(state_path_)) {
            return {};
        }
        const auto index = ++imu_index_;
        std::vector<Frame> frames;
        for (const auto kind : { Kind::Accel, Kind::Gyro }) {
            auto data = std::make_shared<std::array<float, 3>>(std::array<float, 3> { 0.0F, 0.0F, 1.0F });
            const auto bytes = std::span<const std::uint8_t>(
                reinterpret_cast<const std::uint8_t*>(data->data()), sizeof(*data));
            frames.push_back({ kind, kind == Kind::Accel ? Format::Accel32f : Format::Gyro32f,
                               1U, 1U, 12U, index, index * 5000U, 1.0, bytes, data });
        }
        return frames;
    }

    void stop() noexcept override {
        stopped_.store(true, std::memory_order_release);
    }

    std::int32_t get_control(std::uint32_t) override {
        return 0;
    }

    void set_control(std::uint32_t, std::int32_t) override {
    }

  private:
    Options options_;
    std::string state_path_;
    Calibration calibration_;
    std::uint64_t video_index_ { 0U };
    std::uint64_t imu_index_ { 0U };
    std::atomic<bool> stopped_ { false };
};

class FixtureDeviceMonitor final : public DeviceMonitor {
  public:
    FixtureDeviceMonitor(std::string state_path, std::string generation_path)
        : state_path_(std::move(state_path)), generation_path_(std::move(generation_path)) {
    }

    [[nodiscard]] bool connected() override {
        return fixture_present(state_path_);
    }

    [[nodiscard]] std::uint64_t disconnect_generation() const noexcept override {
        try {
            return fixture_disconnect_generation(generation_path_);
        } catch (...) {
            return 0U;
        }
    }

    void wait_for_change(std::chrono::milliseconds timeout) override {
        if (timeout > 0ms) {
            std::this_thread::sleep_for(std::min(timeout, 20ms));
        }
    }

  private:
    std::string state_path_;
    std::string generation_path_;
};

[[nodiscard]] std::uint32_t parse_u32(std::string_view value, std::string_view name) {
    std::uint32_t result = 0U;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc {} || parsed.ptr != value.data() + value.size()) {
        throw std::invalid_argument(std::string(name) + " requires an unsigned integer");
    }
    return result;
}

} // namespace

std::unique_ptr<CaptureDevice> open_device(const Options& options) {
    if (options.serial != kFixtureSerial) {
        throw std::invalid_argument("SOFTWARE_FIXTURE accepts only the SOFTWARE_FIXTURE serial");
    }
    const auto path = state_file();
    if (!fixture_present(path)) {
        throw std::runtime_error("SOFTWARE_FIXTURE device is absent");
    }
    if (options.width != 2U || options.height != 2U || options.fps != 30U
        || options.left_ir || options.right_ir || options.points || options.imu) {
        throw std::invalid_argument("SOFTWARE_FIXTURE supports only color/depth 2x2@30 profiles");
    }
    return std::make_unique<FixtureDevice>(options, path);
}

std::unique_ptr<DeviceMonitor> watch_device(const std::string& serial) {
    if (serial != kFixtureSerial) {
        throw std::invalid_argument("SOFTWARE_FIXTURE monitor accepts only the SOFTWARE_FIXTURE serial");
    }
    return std::make_unique<FixtureDeviceMonitor>(state_file(), generation_file());
}

} // namespace rbt_orbbec

int main(int argc, char** argv) {
    using namespace rbt_orbbec;
    try {
        Options options;
        options.serial = "SOFTWARE_FIXTURE";
        options.width = 2U;
        options.height = 2U;
        options.reconnect_delay_ms = 200U;
        options.frame_timeout_ms = 1200U;
        for (int index = 1; index < argc; ++index) {
            const std::string_view argument(argv[index]);
            if (argument == "--no-reconnect") {
                options.reconnect = false;
                continue;
            }
            if (index + 1 >= argc) {
                throw std::invalid_argument(std::string(argument) + " requires a value");
            }
            const std::string_view value(argv[++index]);
            if (argument == "--duration-sec") {
                options.duration_seconds = parse_u32(value, argument);
            } else if (argument == "--reconnect-delay-ms") {
                options.reconnect_delay_ms = parse_u32(value, argument);
            } else if (argument == "--frame-timeout-ms") {
                options.frame_timeout_ms = parse_u32(value, argument);
            } else if (argument == "--width") {
                options.width = parse_u32(value, argument);
            } else if (argument == "--height") {
                options.height = parse_u32(value, argument);
            } else if (argument == "--component-id") {
                options.component_id = value;
            } else {
                throw std::invalid_argument("unknown SOFTWARE_FIXTURE option: " + std::string(argument));
            }
        }
        std::cerr << "{\"mode\":\"SOFTWARE_FIXTURE\",\"hardware\":\"NOT RUN\"}" << std::endl;
        return run_camera_supervisor(std::move(options));
    } catch (const std::exception& error) {
        std::cerr << "SOFTWARE_FIXTURE: " << error.what() << std::endl;
        return 2;
    }
}

#include <rbt/navwire.hpp>
#include <rbt/runtime.hpp>
#include <rbt/time.hpp>
#include <rbt_navigation/runtime/lio_bridge.hpp>
#include <rbt_navigation/runtime/lio_contract.hpp>
#include <rbt_navigation/runtime/native.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace wire = rbt::navwire;
namespace lio = rbt::navigation::lio;
namespace native = rbt::navigation::native;

struct Options {
    std::string expect;
    double duration_seconds{};
    std::string output;
};

struct ProbeResult {
    mutable std::mutex mutex;
    std::mutex observation_gate;
    std::atomic<bool> activation_ready{false};
    std::uint64_t frames{};
    std::uint64_t unlocalized_frames{};
    std::uint64_t localized_valid_frames{};
    std::uint64_t last_sequence{};
    std::int64_t last_stamp_ns{-1};
    std::uint64_t maximum_uncertainty_ns{};
    bool has_localized_diagnostics{};
    std::array<double, 4> last_localized_map_base{};
    std::array<double, 3> last_map_odom_translation{};
    std::array<double, 4> last_map_odom_quaternion_xyzw{};
    double last_sensor_map_z_minus_base_z{};
    bool observation_end_set{};
    std::chrono::steady_clock::time_point observation_end{};
    bool has_last_valid_reception{};
    std::chrono::steady_clock::time_point last_valid_reception{};
    std::int64_t ending_valid_frame_age_ns{-1};
    bool activated{};
    std::string provider;
    std::string error;

    void fail(std::string message) {
        std::lock_guard lock(mutex);
        if (error.empty()) error = std::move(message);
    }

    void set_observation_end(std::chrono::steady_clock::time_point end) {
        std::lock_guard lock(mutex);
        observation_end = end;
        observation_end_set = true;
    }

    void finish_observation(std::chrono::steady_clock::time_point end) {
        std::lock_guard observation_lock(observation_gate);
        std::lock_guard lock(mutex);
        ending_valid_frame_age_ns = -1;
        if (has_last_valid_reception && last_valid_reception <= end) {
            ending_valid_frame_age_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                end - last_valid_reception).count();
        }
    }
};

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view key(argv[index]);
        if (index + 1 >= argc) throw std::invalid_argument("missing option value");
        const std::string_view value(argv[++index]);
        if (key == "--expect") options.expect = value;
        else if (key == "--duration-sec") {
            char* end = nullptr;
            options.duration_seconds = std::strtod(value.data(), &end);
            if (end != value.data() + value.size() || !std::isfinite(options.duration_seconds)
                || options.duration_seconds < 0.1 || options.duration_seconds > 25.0) {
                throw std::invalid_argument("--duration-sec must be in [0.1, 25]");
            }
        } else if (key == "--output") options.output = value;
        else throw std::invalid_argument("unknown option: " + std::string(key));
    }
    if (options.expect != "frames" && options.expect != "none" && options.expect != "localized") {
        throw std::invalid_argument("--expect must be frames, localized, or none");
    }
    if (options.duration_seconds == 0.0 || options.output.empty()) {
        throw std::invalid_argument("--expect, --duration-sec, and --output are required");
    }
    return options;
}

std::string frame_name(const std::uint8_t (&bytes)[64]) {
    const auto end = std::find(std::begin(bytes), std::end(bytes), std::uint8_t{0});
    if (end == std::end(bytes)
        || !std::all_of(end, std::end(bytes), [](std::uint8_t value) { return value == 0U; })) {
        throw std::runtime_error("frame name is not canonically NUL-padded");
    }
    return {reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(end - std::begin(bytes))};
}

template <typename Range>
bool finite_values(const Range& values) {
    return std::all_of(std::begin(values), std::end(values),
                       [](double value) { return std::isfinite(value); });
}

void validate_frame(const wire::LioFrameHeader& header,
                    std::span<const wire::LioPoint> points,
                    const rbt::time::HostClock& clock,
                    std::uint64_t previous_sequence,
                    std::int64_t previous_stamp_ns,
                    bool synthetic_contract = true) {
    if (header.reserved != 0U || header.flags != 0U) {
        throw std::runtime_error("frame flags are not fail-closed and unlocalized");
    }
    if (header.sequence == 0U || header.sequence <= previous_sequence) {
        throw std::runtime_error("frame sequence is zero, duplicate, or regressed");
    }
    if (header.stamp.kind != 1U || header.uncertainty_ns > 250'000U) {
        throw std::runtime_error("frame stamp kind or uncertainty is outside the contract");
    }
    const auto now = native::now(clock);
    const auto stamp_ns = native::decodeStamp(header.stamp, now.clock);
    if (stamp_ns != header.stamp.nanoseconds || stamp_ns > now.ns
        || now.ns - stamp_ns > 250'000'000LL) {
        throw std::runtime_error("frame stamp identity or source epoch age is invalid");
    }
    if (previous_stamp_ns >= 0 && stamp_ns <= previous_stamp_ns) {
        throw std::runtime_error("frame source epoch is duplicate or regressed");
    }
    const auto world_frame = frame_name(header.world_frame);
    const auto base_frame = frame_name(header.base_frame);
    if (world_frame != "odom"
        || (synthetic_contract ? base_frame != "base_footprint"
                               : (base_frame != "base_footprint" && base_frame != "base_link"))) {
        throw std::runtime_error(synthetic_contract
            ? "frame names differ from odom/base_footprint"
            : "frame names differ from odom/base_footprint or odom/base_link");
    }
    if (points.empty() || points.size() > 200'000U) {
        throw std::runtime_error("frame point count is outside 1..200000");
    }
    if (!finite_values(header.odom_base_position)
        || !finite_values(header.odom_base_quaternion_xyzw)
        || !finite_values(header.map_odom_position)
        || !finite_values(header.map_odom_quaternion_xyzw)
        || !finite_values(header.sensor_origin)
        || !finite_values(header.linear_velocity)
        || !finite_values(header.angular_velocity)) {
        throw std::runtime_error("frame header contains nonfinite geometry or twist");
    }
    double map_norm = 0.0;
    for (const auto value : header.map_odom_position) map_norm += value * value;
    for (const auto value : header.map_odom_quaternion_xyzw) map_norm += value * value;
    if (map_norm != 0.0) throw std::runtime_error("unlocalized frame contains a map-to-odom transform");
    double odom_quaternion_norm = 0.0;
    for (const auto value : header.odom_base_quaternion_xyzw) {
        odom_quaternion_norm += value * value;
    }
    if (std::abs(odom_quaternion_norm - 1.0) > 0.001) {
        throw std::runtime_error("odom-to-base quaternion is not normalized");
    }
    if (synthetic_contract) {
        const double sensor_to_base_z = header.sensor_origin[2] - header.odom_base_position[2];
        if (std::abs(sensor_to_base_z - 0.735) > 0.02) {
            throw std::runtime_error("sensor-to-base vertical offset is "
                                     + std::to_string(sensor_to_base_z)
                                     + " m; expected 0.735 m within 0.02 m");
        }
    }
    for (const auto& point : points) {
        if (!std::isfinite(point.x) || !std::isfinite(point.y)
            || !std::isfinite(point.z) || !std::isfinite(point.intensity)) {
            throw std::runtime_error("frame contains a nonfinite XYZI point");
        }
    }
}

std::string json_escape(std::string_view value) {
    std::string escaped;
    for (const char byte : value) {
        switch (byte) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (static_cast<unsigned char>(byte) < 0x20U) escaped += '?';
            else escaped += byte;
        }
    }
    return escaped;
}

std::string json_number(double value) {
    if (!std::isfinite(value)) return "null";
    std::ostringstream stream;
    stream << std::setprecision(17) << value;
    return stream.str();
}

class SourceProbe final : public rbt::Component {
public:
    SourceProbe(Options options, std::shared_ptr<ProbeResult> result)
        : options_(std::move(options)), result_(std::move(result)) {}

    rbt::Result<void> prepare() override { return {}; }
    rbt::ComponentId id() const override { return "lio-source-probe"; }
    std::vector<rbt::RequirementSpec> requirements() const override {
        return {{"lio", lio::LioNavigationFrameV1::TYPE_NAME}};
    }

    rbt::Result<void> activate(rbt::ActivationContext& context) override {
        auto capability = context.require<lio::LioNavigationFrameV1>("lio");
        if (!capability) return rbt::Result<void>::failure(capability.error());
        if (capability.provider() != "super-lio") {
            return rbt::Result<void>::failure("LIO navigation-frame provider is not super-lio");
        }
        auto opened = capability.slice_subscriber<wire::LioFrameHeader, wire::LioPoint>();
        if (!opened) {
            return rbt::Result<void>::failure("LIO navigation-frame subscriber open failed: "
                                              + std::string(rbt::to_string(opened.error())));
        }
        auto clock_result = rbt::time::HostClock::create();
        if (!clock_result) return rbt::Result<void>::failure("HostClock unavailable for probe");
        auto clock = std::make_shared<rbt::time::HostClock>(std::move(clock_result).value());
        auto result = result_;
        const auto expect_frames = options_.expect == "frames";
        const auto expect_localized = options_.expect == "localized";
        auto subscriber = std::move(opened).value();
        const auto task = context.spawn_critical(
            "lio-frame-probe",
            [subscriber = std::move(subscriber), clock, result, expect_frames,
             expect_localized](std::stop_token stop) mutable {
                std::uint64_t previous_sequence = 0U;
                std::int64_t previous_stamp_ns = -1;
                bool localized_seen = false;
                while (!stop.stop_requested()) {
                    std::unique_lock observation_lock(result->observation_gate);
                    auto received = subscriber.receive();
                    if (!received) {
                        const auto error = "LIO navigation-frame receive failed: "
                            + std::string(rbt::to_string(received.error()));
                        result->fail(error);
                        return rbt::TaskOutcome::error(error);
                    }
                    if (received.value()) {
                        const auto received_at = std::chrono::steady_clock::now();
                        auto sample = std::move(received).value();
                        const auto& header = sample->header();
                        const auto points = sample->payload();
                        {
                            std::lock_guard lock(result->mutex);
                            ++result->frames;
                            result->last_sequence = header.sequence;
                            result->maximum_uncertainty_ns =
                                std::max(result->maximum_uncertainty_ns, header.uncertainty_ns);
                        }
                        if (!expect_frames) {
                            if (!expect_localized) {
                                const std::string error = "unexpected normalized frame in none mode";
                                result->fail(error);
                                return rbt::TaskOutcome::error(error);
                            }
                            try {
                                constexpr std::uint32_t map_odom_valid_flag = 1U;
                                if (header.reserved != 0U || (header.flags & ~map_odom_valid_flag) != 0U) {
                                    throw std::runtime_error("localized frame flags or reserved field are invalid");
                                }
                                if (header.flags == 0U) {
                                    validate_frame(header, points, *clock,
                                                   previous_sequence, previous_stamp_ns, false);
                                    {
                                        std::lock_guard lock(result->mutex);
                                        ++result->unlocalized_frames;
                                    }
                                    if (localized_seen) {
                                        throw std::runtime_error("unlocalized frame received after localization became valid");
                                    }
                                    previous_sequence = header.sequence;
                                    previous_stamp_ns = header.stamp.nanoseconds;
                                    continue;
                                }

                                const auto projected = lio::validateAndProject(
                                    header, points, native::now(*clock), lio::ProjectionConfig{},
                                    previous_sequence, previous_stamp_ns);
                                previous_sequence = projected.sequence;
                                previous_stamp_ns = projected.stamp.ns;
                                localized_seen = true;
                                const auto map_odom_translation =
                                    projected.map_to_odom.transform.translation();
                                const auto map_odom_rotation =
                                    projected.map_to_odom.transform.rotation();
                                const std::array<double, 4> map_base{
                                    projected.map_base_link.x, projected.map_base_link.y,
                                    projected.map_base_z, projected.map_base_link.yaw};
                                const std::array<double, 3> map_odom_position{
                                    map_odom_translation.x(), map_odom_translation.y(),
                                    map_odom_translation.z()};
                                const std::array<double, 4> map_odom_quaternion{
                                    map_odom_rotation.x(), map_odom_rotation.y(),
                                    map_odom_rotation.z(), map_odom_rotation.w()};
                                bool within_observation = false;
                                {
                                    std::lock_guard lock(result->mutex);
                                    within_observation = !result->observation_end_set
                                        || received_at <= result->observation_end;
                                }
                                if (within_observation) {
                                    std::lock_guard lock(result->mutex);
                                    ++result->localized_valid_frames;
                                    result->last_stamp_ns = projected.stamp.ns;
                                    result->last_localized_map_base = map_base;
                                    result->last_map_odom_translation = map_odom_position;
                                    result->last_map_odom_quaternion_xyzw = map_odom_quaternion;
                                    result->last_sensor_map_z_minus_base_z =
                                        projected.sensor_origin_map.z() - projected.map_base_z;
                                    result->has_localized_diagnostics = true;
                                    result->last_valid_reception = received_at;
                                    result->has_last_valid_reception = true;
                                }
                            } catch (const std::exception& error) {
                                result->fail(error.what());
                                return rbt::TaskOutcome::error(error.what());
                            }
                            continue;
                        }
                        try {
                            validate_frame(header, points, *clock,
                                           previous_sequence, previous_stamp_ns);
                            previous_sequence = header.sequence;
                            previous_stamp_ns = header.stamp.nanoseconds;
                            std::lock_guard lock(result->mutex);
                            result->last_stamp_ns = previous_stamp_ns;
                            if (!result->observation_end_set
                                || received_at <= result->observation_end) {
                                result->last_valid_reception = received_at;
                                result->has_last_valid_reception = true;
                            }
                        } catch (const std::exception& error) {
                            result->fail(error.what());
                            return rbt::TaskOutcome::error(error.what());
                        }
                    } else {
                        observation_lock.unlock();
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    }
                }
                return rbt::TaskOutcome::completed();
            });
        if (!task) return rbt::Result<void>::failure("failed to spawn LIO source probe task");
        {
            std::lock_guard lock(result_->mutex);
            result_->activated = true;
            result_->provider = capability.provider();
        }
        result_->activation_ready.store(true, std::memory_order_release);
        std::cout << "LIO_SOURCE_PROBE_READY provider=super-lio" << std::endl;
        return {};
    }

private:
    Options options_;
    std::shared_ptr<ProbeResult> result_;
};

bool write_result(const std::string& output, const Options& options,
                  const ProbeResult& result, int runtime_status) {
    std::uint64_t frames = 0U, last_sequence = 0U, maximum_uncertainty_ns = 0U;
    std::uint64_t unlocalized_frames = 0U, localized_valid_frames = 0U;
    std::int64_t last_stamp_ns = -1;
    std::int64_t ending_valid_frame_age_ns = -1;
    bool has_localized_diagnostics = false;
    std::array<double, 4> last_localized_map_base{};
    std::array<double, 3> last_map_odom_translation{};
    std::array<double, 4> last_map_odom_quaternion_xyzw{};
    double last_sensor_map_z_minus_base_z = 0.0;
    bool activated = false;
    std::string provider, error;
    {
        std::lock_guard lock(result.mutex);
        frames = result.frames;
        unlocalized_frames = result.unlocalized_frames;
        localized_valid_frames = result.localized_valid_frames;
        last_sequence = result.last_sequence;
        last_stamp_ns = result.last_stamp_ns;
        maximum_uncertainty_ns = result.maximum_uncertainty_ns;
        ending_valid_frame_age_ns = result.ending_valid_frame_age_ns;
        has_localized_diagnostics = result.has_localized_diagnostics;
        last_localized_map_base = result.last_localized_map_base;
        last_map_odom_translation = result.last_map_odom_translation;
        last_map_odom_quaternion_xyzw = result.last_map_odom_quaternion_xyzw;
        last_sensor_map_z_minus_base_z = result.last_sensor_map_z_minus_base_z;
        activated = result.activated;
        provider = result.provider;
        error = result.error;
    }
    if (!activated && error.empty()) error = "probe never activated against super-lio";
    const bool ending_frame_fresh = ending_valid_frame_age_ns >= 0
        && ending_valid_frame_age_ns <= 250'000'000LL;
    const bool expectation_met = options.expect == "frames"
        ? frames >= 5U && ending_frame_fresh
        : options.expect == "localized"
            ? localized_valid_frames >= 5U && ending_frame_fresh
            : frames == 0U;
    if (!expectation_met && error.empty()) {
        if (options.expect == "frames" && frames < 5U) {
            error = "fewer than five valid normalized frames received";
        } else if (options.expect == "frames") {
            error = "no valid normalized frame within 250 ms of observation end";
        } else if (options.expect == "localized" && localized_valid_frames < 5U) {
            error = "fewer than five valid localized frames received";
        } else if (options.expect == "localized") {
            error = "no valid localized frame within 250 ms of observation end";
        } else {
            error = "normalized frames were received in none mode";
        }
    }
    const bool passed = runtime_status == 0 && activated && error.empty() && expectation_met;
    const std::string json = "{\n"
        "  \"passed\": " + std::string(passed ? "true" : "false") + ",\n"
        "  \"expect\": \"" + json_escape(options.expect) + "\",\n"
        "  \"duration_sec\": " + std::to_string(options.duration_seconds) + ",\n"
        "  \"provider\": \"" + json_escape(provider) + "\",\n"
        "  \"activated\": " + std::string(activated ? "true" : "false") + ",\n"
        "  \"frames\": " + std::to_string(frames) + ",\n"
        "  \"unlocalized_frames\": " + std::to_string(unlocalized_frames) + ",\n"
        "  \"localized_valid_frames\": " + std::to_string(localized_valid_frames) + ",\n"
        "  \"last_sequence\": " + std::to_string(last_sequence) + ",\n"
        "  \"last_stamp_ns\": " + std::to_string(last_stamp_ns) + ",\n"
        "  \"maximum_uncertainty_ns\": " + std::to_string(maximum_uncertainty_ns) + ",\n"
        "  \"ending_valid_frame_age_ns\": " + std::to_string(ending_valid_frame_age_ns) + ",\n"
        "  \"last_localized_map_base_link\": "
        + (has_localized_diagnostics
            ? "{\"x\":" + json_number(last_localized_map_base[0])
                + ",\"y\":" + json_number(last_localized_map_base[1])
                + ",\"z\":" + json_number(last_localized_map_base[2])
                + ",\"yaw\":" + json_number(last_localized_map_base[3]) + "}"
            : "null") + ",\n"
        "  \"last_map_from_odom\": "
        + (has_localized_diagnostics
            ? "{\"translation\":{\"x\":" + json_number(last_map_odom_translation[0])
                + ",\"y\":" + json_number(last_map_odom_translation[1])
                + ",\"z\":" + json_number(last_map_odom_translation[2])
                + "},\"quaternion_xyzw\":{\"x\":"
                + json_number(last_map_odom_quaternion_xyzw[0])
                + ",\"y\":" + json_number(last_map_odom_quaternion_xyzw[1])
                + ",\"z\":" + json_number(last_map_odom_quaternion_xyzw[2])
                + ",\"w\":" + json_number(last_map_odom_quaternion_xyzw[3]) + "}}"
            : "null") + ",\n"
        "  \"last_sensor_map_z_minus_base_z\": "
        + (has_localized_diagnostics
            ? json_number(last_sensor_map_z_minus_base_z)
            : "null") + ",\n"
        "  \"runtime_status\": " + std::to_string(runtime_status) + ",\n"
        "  \"error\": \"" + json_escape(error) + "\"\n}\n";
    if (output == "-") {
        std::cout << json;
    } else {
        std::ofstream stream(output, std::ios::out | std::ios::trunc);
        if (!stream) {
            std::cerr << "cannot open probe output: " << output << '\n';
            return false;
        }
        stream << json;
        stream.flush();
        if (!stream) {
            std::cerr << "failed to write probe output: " << output << '\n';
            return false;
        }
    }
    std::cout << "LIO_SOURCE_PROBE_RESULT frames=" << frames
              << " passed=" << (passed ? "true" : "false") << std::endl;
    if (!error.empty()) std::cerr << "LIO_SOURCE_PROBE_ERROR " << error << std::endl;
    return passed;
}
} // namespace

int main(int argc, char** argv) {
    std::shared_ptr<ProbeResult> result = std::make_shared<ProbeResult>();
    try {
        auto options = parse_options(argc, argv);
        const auto* composer = std::getenv("RBT_COMPOSER_ADDR");
        if (composer == nullptr || *composer == '\0') {
            throw std::invalid_argument("RBT_COMPOSER_ADDR must identify the private Composer");
        }
        rbt::RuntimeOptions runtime_options;
        runtime_options.composer_address = composer;
        rbt::ComponentRuntime runtime(std::make_unique<SourceProbe>(options, result),
                                      std::move(runtime_options));
        std::jthread deadline([&runtime, result, duration = options.duration_seconds](std::stop_token stop) {
            const auto activation_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (!stop.stop_requested() && !result->activation_ready.load(std::memory_order_acquire)
                   && std::chrono::steady_clock::now() < activation_deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            const auto end = std::chrono::steady_clock::now()
                + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(duration));
            result->set_observation_end(end);
            while (!stop.stop_requested() && std::chrono::steady_clock::now() < end) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (!stop.stop_requested()) {
                result->finish_observation(end);
                runtime.stop();
            }
        });
        const int status = runtime.run();
        deadline.request_stop();
        deadline.join();
        return write_result(options.output, options, *result, status) ? 0 : 1;
    } catch (const std::exception& error) {
        result->fail(error.what());
        std::cerr << "LIO source probe: " << error.what() << '\n';
        return 1;
    }
}

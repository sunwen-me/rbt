#include <rbt_orbbec/component.hpp>

#include <rbt/orbbec.hpp>
#include <rbt/tf_component.hpp>
#include <rbt/time_stream.hpp>
#include <rbt/transforms.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <syncstream>
#include <thread>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rbt_orbbec {
namespace {

using CameraInfoServer = rbt::RpcServer<rbt::orbbec::CameraInfoRequest,
                                        rbt::orbbec::CameraInfoResponse>;
using ControlServer = rbt::RpcServer<rbt::orbbec::ControlRequest,
                                     rbt::orbbec::ControlResponse>;
using FramePublisher = rbt::time::StampedBytesPublisher;

constexpr std::uint32_t kCameraInfoOk = 0U;
constexpr std::uint32_t kCameraInfoUnsupported = 1U;
constexpr std::uint32_t kCameraInfoInvalid = 2U;
constexpr std::uint32_t kCameraInfoSdkError = 3U;
constexpr std::uint32_t kControlOk = 0U;
constexpr std::uint32_t kControlUnsupported = 1U;
constexpr std::uint32_t kControlInvalid = 2U;
constexpr std::uint32_t kControlSdkError = 3U;
constexpr auto kRpcDrainPerIteration = 8U;
constexpr auto kPollInterval = std::chrono::milliseconds(100);
constexpr auto kPressureReportInterval = std::chrono::seconds(5);

struct PublisherBinding {
    Kind kind { Kind::Depth };
    rbt::StreamProfile profile { rbt::StreamProfile::Sensor };
    std::string service_name;
    std::string frame_name;
    std::size_t maximum_sample_bytes { 0U };
    std::uint64_t sequence { 1U };
    FramePublisher publisher;
};

struct PublicationState {
    std::vector<PublisherBinding> publishers;
    std::map<Kind, std::uint64_t> last_indexes;
    std::map<Kind, std::uint64_t> last_capture_ns;
    std::map<Kind, std::chrono::steady_clock::time_point> last_stream_frames;
    std::uint64_t dropped_samples { 0U };
    std::uint64_t loan_failures { 0U };
};

[[nodiscard]] bool is_imu_kind(Kind kind) noexcept {
    return kind == Kind::Accel || kind == Kind::Gyro;
}

[[nodiscard]] const char* profile_name(rbt::StreamProfile profile) noexcept {
    return profile == rbt::StreamProfile::Control ? "control" : "sensor";
}

[[nodiscard]] const VideoCalibration* find_video(const Calibration& calibration, Kind kind) {
    const auto found = std::find_if(calibration.videos.begin(), calibration.videos.end(),
                                    [kind](const VideoCalibration& video) {
                                        return video.kind == kind;
                                    });
    return found == calibration.videos.end() ? nullptr : &*found;
}

[[nodiscard]] std::size_t bytes_per_pixel(Format format) {
    switch (format) {
    case Format::Rgb8:
        return 3U;
    case Format::Mono8:
        return 1U;
    case Format::Mono16:
    case Format::Depth16:
        return 2U;
    case Format::Xyz32f:
    case Format::Accel32f:
    case Format::Gyro32f:
        return 12U;
    }
    throw std::runtime_error("camera calibration contains an unknown pixel format");
}

[[nodiscard]] std::size_t checked_product(std::size_t lhs, std::size_t rhs) {
    if (rhs != 0U && lhs > std::numeric_limits<std::size_t>::max() / rhs) {
        throw std::runtime_error("camera stream capacity overflows");
    }
    return lhs * rhs;
}

[[nodiscard]] std::size_t maximum_sample_bytes(Kind kind, const Calibration& calibration) {
    std::size_t data_bytes = 12U;
    if (kind == Kind::Accel || kind == Kind::Gyro) {
        data_bytes = 12U;
    } else {
        const auto calibrated_kind = kind == Kind::Points ? Kind::Depth : kind;
        const auto* video = find_video(calibration, calibrated_kind);
        if (video == nullptr || video->width == 0U || video->height == 0U || video->fps == 0U) {
            throw std::runtime_error(std::string("missing physical profile for ") + kind_name(kind));
        }
        const auto bytes_per_pixel_for_kind = kind == Kind::Points ? 12U : bytes_per_pixel(video->format);
        const auto pixels = checked_product(video->width, video->height);
        data_bytes = checked_product(pixels, bytes_per_pixel_for_kind);
    }

    const auto application_bytes = kFrameHeaderBytes + data_bytes;
    if (application_bytes > kMaximumFrameBytes) {
        throw std::runtime_error(std::string("physical profile exceeds the frame limit for ")
                                 + kind_name(kind));
    }
    return rbt::time::kStampedBytesPrefixBytes + application_bytes;
}

[[nodiscard]] std::string endpoint_name(std::string_view capability, std::string_view endpoint) {
    return "rbt/cap/" + std::string(capability) + "/" + std::string(endpoint);
}

void provide_stream_capability(rbt::ActivationContext& context,
                               const std::string& capability,
                               const rbt::StreamDescriptor& descriptor,
                               Kind kind) {
    switch (kind) {
    case Kind::Color:
        (void)context.provide<ColorCapability>(capability, descriptor);
        return;
    case Kind::Depth:
        (void)context.provide<DepthCapability>(capability, descriptor);
        return;
    case Kind::LeftIr:
        (void)context.provide<LeftIrCapability>(capability, descriptor);
        return;
    case Kind::RightIr:
        (void)context.provide<RightIrCapability>(capability, descriptor);
        return;
    case Kind::Points:
        (void)context.provide<PointsCapability>(capability, descriptor);
        return;
    case Kind::Accel:
        (void)context.provide<AccelCapability>(capability, descriptor);
        return;
    case Kind::Gyro:
        (void)context.provide<GyroCapability>(capability, descriptor);
        return;
    }
    throw std::runtime_error("unknown stream kind while advertising a capability");
}

void saturating_increment(std::uint64_t& value) noexcept {
    if (value != std::numeric_limits<std::uint64_t>::max()) {
        ++value;
    }
}

void copy_text(std::string_view source, std::uint8_t* destination, std::size_t capacity) {
    if (capacity == 0U || source.size() >= capacity) {
        throw std::length_error("RPC text field is too small");
    }
    std::copy(source.begin(), source.end(), destination);
}

void set_control_message(rbt::orbbec::ControlResponse& response, std::string_view message) noexcept {
    const auto count = std::min(message.size(), sizeof(response.message) - 1U);
    std::copy_n(message.begin(), count, response.message);
}

[[nodiscard]] rbt::orbbec::CameraInfoResponse camera_info_response(
    const rbt::orbbec::CameraInfoRequest& request,
    const Options& options,
    const Calibration& calibration) {
    rbt::orbbec::CameraInfoResponse response {};
    response.stream = request.stream;
    if (request.reserved != 0U || request.stream < static_cast<std::uint32_t>(Kind::Color)
        || request.stream > static_cast<std::uint32_t>(Kind::Gyro)) {
        response.status = kCameraInfoInvalid;
        return response;
    }

    const auto kind = static_cast<Kind>(request.stream);
    try {
        copy_text(calibration.device.serial, response.serial, sizeof(response.serial));
    } catch (const std::length_error&) {
        response.status = kCameraInfoSdkError;
        return response;
    }
    try {
        const auto name = output_frame_id(options, kind);
        copy_text(name, response.frame_id, sizeof(response.frame_id));
    } catch (const std::length_error&) {
        response.status = kCameraInfoInvalid;
        return response;
    } catch (const std::exception&) {
        response.status = kCameraInfoInvalid;
        return response;
    }

    const auto* video = find_video(calibration, kind);
    if (video == nullptr) {
        response.status = kCameraInfoUnsupported;
        return response;
    }

    response.status = kCameraInfoOk;
    response.width = video->width;
    response.height = video->height;
    response.fps = video->fps;
    response.format = static_cast<std::uint32_t>(video->format);
    response.fx = video->fx;
    response.fy = video->fy;
    response.cx = video->cx;
    response.cy = video->cy;
    response.distortion_model = video->distortion_model;
    std::copy(video->distortion.begin(), video->distortion.end(), response.distortion);
    return response;
}

void publish_pressure_report(const std::vector<PublisherBinding>& publishers,
                             std::uint64_t dropped_samples,
                             std::uint64_t loan_failures) {
    std::osyncstream output(std::cout);
    output << "{\"type\":\"orbbec_pressure\",\"loan_failures\":" << loan_failures
           << ",\"dropped_samples\":" << dropped_samples << ",\"streams\":[";
    for (std::size_t index = 0U; index < publishers.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        const auto pressure = publishers[index].publisher.pressure();
        output << "{\"kind\":" << json_quote(kind_name(publishers[index].kind)) << ",\"loan_failures\":";
        if (pressure.publisher.has_value()) {
            output << pressure.publisher->loan_failures << ",\"send_failures\":"
                   << pressure.publisher->send_failures << ",\"estimated_dropped_samples\":";
            if (pressure.publisher->estimated_dropped_samples.has_value()) {
                output << *pressure.publisher->estimated_dropped_samples;
            } else {
                output << "null";
            }
        } else {
            output << "0,\"send_failures\":0,\"estimated_dropped_samples\":null";
        }
        output << '}';
    }
    output << "]}" << std::endl;
}

void publish_frame(PublicationState& state,
                   const rbt::time::DeviceClock& device_clock,
                   const rbt::time::HostClock& host_clock,
                   const Frame& frame,
                   bool require_imu_contiguity,
                   std::stop_token stop = {}) {
    validate_frame(frame);
    const auto previous_index = state.last_indexes.find(frame.kind);
    if (previous_index != state.last_indexes.end()) {
        if (require_imu_contiguity) {
            const auto expected = previous_index->second == std::numeric_limits<std::uint64_t>::max()
                ? std::optional<std::uint64_t> {}
                : std::optional<std::uint64_t> { previous_index->second + 1U };
            if (!expected.has_value() || frame.index != *expected) {
                throw std::runtime_error(std::string("IMU source frame index is not contiguous for ")
                                         + kind_name(frame.kind) + ": expected "
                                         + (expected.has_value() ? std::to_string(*expected) : "no value after overflow")
                                         + ", got " + std::to_string(frame.index));
            }
        } else if (frame.index <= previous_index->second) {
            saturating_increment(state.dropped_samples);
            return;
        }
    }

    if (require_imu_contiguity && frame.capture_us == 0U) {
        throw std::runtime_error("IMU capture timestamp is unavailable");
    }
    std::optional<rbt::time::Timestamp> capture;
    if (frame.capture_us != 0U) {
        constexpr auto maximum_microseconds =
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / 1000U;
        if (frame.capture_us > maximum_microseconds) {
            throw std::runtime_error("device capture timestamp overflows nanoseconds");
        }
        const auto capture_ns = frame.capture_us * 1000U;
        const auto previous_capture = state.last_capture_ns.find(frame.kind);
        if (previous_capture != state.last_capture_ns.end() && capture_ns < previous_capture->second) {
            throw std::runtime_error(std::string("device timestamp regressed for ") + kind_name(frame.kind)
                                     + ": previous " + std::to_string(previous_capture->second)
                                     + " ns, got " + std::to_string(capture_ns) + " ns");
        }
        state.last_capture_ns[frame.kind] = capture_ns;
        auto stamped = device_clock.stamp(static_cast<std::int64_t>(capture_ns));
        if (!stamped) throw std::runtime_error("failed to stamp device capture time");
        capture = std::move(stamped).value();
    }
    state.last_indexes[frame.kind] = frame.index;

    const auto publisher = std::find_if(
        state.publishers.begin(), state.publishers.end(), [&frame](const PublisherBinding& item) {
            return item.kind == frame.kind;
        });
    if (publisher == state.publishers.end()) {
        throw std::runtime_error(std::string("SDK returned disabled stream ") + kind_name(frame.kind));
    }
    state.last_stream_frames[frame.kind] = std::chrono::steady_clock::now();
    if (frame.data.size() > kMaximumFrameBytes - kFrameHeaderBytes) {
        throw std::runtime_error("SDK frame payload exceeds the RBT frame limit");
    }
    const auto application_bytes = kFrameHeaderBytes + frame.data.size();
    const auto sample_bytes = rbt::time::kStampedBytesPrefixBytes + application_bytes;
    if (sample_bytes > publisher->maximum_sample_bytes) {
        throw std::runtime_error(std::string("SDK frame exceeds calibrated publisher capacity for ")
                                 + kind_name(frame.kind));
    }

    auto received = host_clock.now_monotonic();
    if (!received) {
        throw std::runtime_error(std::string("failed to read monotonic host time: ")
                                 + rbt::time::to_string(received.error()));
    }
    auto timing = rbt::time::SensorTiming::create(
        std::move(capture), std::move(received).value(), std::nullopt, std::nullopt);
    if (!timing) {
        throw std::runtime_error("failed to construct frame timing metadata");
    }

    const auto send_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (true) {
        if (stop.stop_requested()) return;
        auto loan_result = publisher->publisher.loan_slice(sample_bytes);
        if (!loan_result) {
            if (loan_result.error() == rbt::StreamError::LoanPoolExhausted) {
                saturating_increment(state.loan_failures);
                if (require_imu_contiguity)
                    throw std::runtime_error(std::string("IMU publisher loan pool exhausted for ") + kind_name(frame.kind));
                saturating_increment(state.dropped_samples);
                return;
            }
            throw std::runtime_error(std::string("failed to loan ") + kind_name(frame.kind)
                                     + " sample: " + rbt::to_string(loan_result.error()));
        }
        auto sample = std::move(loan_result).value();
        sample.header() = publisher->sequence;
        auto bytes = sample.payload();
        const auto prefix_result = rbt::time::write_timing_prefix(
            bytes.first(rbt::time::kStampedBytesPrefixBytes), timing.value());
        if (prefix_result != rbt::time::PrefixCodecError::None)
            throw std::runtime_error(std::string("failed to encode frame timing: ") + rbt::time::to_string(prefix_result));
        encode_frame(bytes.subspan(rbt::time::kStampedBytesPrefixBytes), frame, publisher->frame_name);
        auto sent = sample.send();
        if (sent) break;
        // A multi-subscriber send may deliver to some receivers before another
        // reports QueueFull. Keep identical bytes/sequence on bounded retries;
        // IMU consumers suppress only byte-identical copies of the last sample.
        if (require_imu_contiguity && sent.error() == rbt::StreamError::QueueFull
            && std::chrono::steady_clock::now() < send_deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        throw std::runtime_error(std::string("failed to publish ")
                                 + (require_imu_contiguity ? "IMU " : "") + kind_name(frame.kind)
                                 + " sample: " + rbt::to_string(sent.error())
                                 + (require_imu_contiguity && sent.error() == rbt::StreamError::QueueFull
                                        ? " (100 ms retry deadline exceeded)" : ""));
    }
    if (publisher->sequence == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("published sample sequence overflow");
    }
    ++publisher->sequence;
}

[[nodiscard]] std::string rpc_error_message(std::string_view service, rbt::RpcError error) {
    return std::string(service) + " RPC failed: " + rbt::to_string(error);
}

[[nodiscard]] bool is_cancelled_request_error(rbt::RpcError error) noexcept {
    return error == rbt::RpcError::Cancelled || error == rbt::RpcError::DeadlineExceeded;
}

void print_activation_report(const rbt::ActivationContext& context,
                             const Calibration& calibration,
                             std::string_view device_clock_source,
                             const std::vector<PublisherBinding>& publishers) {
    std::cout << "{\"event\":\"activation\",\"component_id\":"
              << json_quote(context.component())
              << ",\"process_instance\":" << json_quote(context.process_instance())
              << ",\"activation_id\":" << context.activation_id()
              << ",\"serial\":" << json_quote(calibration.device.serial)
              << ",\"sdk_clock_source\":" << json_quote(device_clock_source)
              << ",\"streams\":[";
    for (std::size_t index = 0U; index < publishers.size(); ++index) {
        if (index != 0U) {
            std::cout << ',';
        }
        std::cout << "{\"kind\":" << json_quote(kind_name(publishers[index].kind))
                  << ",\"service\":" << json_quote(publishers[index].service_name)
                  << ",\"profile\":" << json_quote(profile_name(publishers[index].profile)) << '}';
    }
    std::cout << "]}" << std::endl;
}

class CameraComponent final : public rbt::Component {
  public:
    explicit CameraComponent(Options options)
        : options_(std::move(options)) {
    }

    rbt::Result<void> prepare() override {
        try {
            validate_options(options_);
            return {};
        } catch (const std::exception& error) {
            return rbt::Result<void>::failure(error.what());
        }
    }

    rbt::ComponentId id() const override {
        return options_.component_id;
    }

    std::vector<rbt::RequirementSpec> requirements() const override {
        return {};
    }

    rbt::Result<void> activate(rbt::ActivationContext& context) override {
        try {
            auto opened_device = open_device(options_);
            if (!opened_device) {
                return rbt::Result<void>::failure("Orbbec SDK returned no capture device");
            }
            auto device = std::shared_ptr<CaptureDevice>(std::move(opened_device));
            context.scope().defer_sync([device] { device->stop(); });

            const auto calibration = device->calibration();
            const auto device_clock_source = rbt::generate_uuid();
            auto device_clock_result = rbt::time::DeviceClock::create(parse_uuid(device_clock_source));
            if (!device_clock_result) {
                return rbt::Result<void>::failure("failed to create activation device clock");
            }
            auto device_clock = std::move(device_clock_result).value();
            auto host_clock_result = rbt::time::HostClock::create();
            if (!host_clock_result) {
                return rbt::Result<void>::failure(std::string("failed to create monotonic host clock: ")
                                                  + rbt::time::to_string(host_clock_result.error()));
            }
            auto host_clock = std::move(host_clock_result).value();

            std::vector<PublisherBinding> publishers;
            for (const auto kind : enabled_kinds(options_)) {
                const auto capability = rbt::generate_uuid();
                const auto descriptor = rbt::StreamDescriptor(
                    endpoint_name(capability, "stream"),
                    rbt::time::STAMPED_BYTES_DESCRIPTOR,
                    is_imu_kind(kind) ? rbt::StreamProfile::Control : rbt::StreamProfile::Sensor);
                const auto capacity = maximum_sample_bytes(kind, calibration);
                auto publisher = rbt::time::create_stamped_bytes_publisher(descriptor, capacity);
                if (!publisher) {
                    return rbt::Result<void>::failure(std::string("failed to open ") + kind_name(kind)
                                                      + " publisher: " + rbt::to_string(publisher.error()));
                }
                provide_stream_capability(context, capability, descriptor, kind);
                publishers.push_back(PublisherBinding {
                    kind,
                    descriptor.profile,
                    descriptor.service_name,
                    output_frame_id(options_, kind),
                    capacity,
                    1U,
                    std::move(publisher).value(),
                });
            }

            const auto camera_info_id = rbt::generate_uuid();
            const auto camera_info_descriptor = rbt::RpcDescriptor::for_fixed<
                rbt::orbbec::CameraInfoRequest,
                rbt::orbbec::CameraInfoResponse>(endpoint_name(camera_info_id, "rpc"));
            auto camera_info_server_result = CameraInfoServer::open_or_create_for_capability(
                camera_info_descriptor, camera_info_id);
            if (!camera_info_server_result) {
                return rbt::Result<void>::failure(
                    rpc_error_message("CameraInfo", camera_info_server_result.error()));
            }
            (void)context.provide_rpc<CameraInfoCapability>(
                camera_info_id, rbt::OwnedRpcDescriptor::from(camera_info_descriptor));

            const auto control_id = rbt::generate_uuid();
            const auto control_descriptor = rbt::RpcDescriptor::for_fixed<
                rbt::orbbec::ControlRequest,
                rbt::orbbec::ControlResponse>(endpoint_name(control_id, "rpc"));
            auto control_server_result = ControlServer::open_or_create_for_capability(
                control_descriptor, control_id);
            if (!control_server_result) {
                return rbt::Result<void>::failure(rpc_error_message("Control", control_server_result.error()));
            }
            (void)context.provide_rpc<ControlCapability>(
                control_id, rbt::OwnedRpcDescriptor::from(control_descriptor));

            auto static_provider = rbt::tf::provide_static_tf<StaticUpdatesCapability,
                                                             StaticSnapshotCapability>(
                context, 64U);
            auto static_stamp_result = device_clock.stamp(0);
            if (!static_stamp_result) {
                return rbt::Result<void>::failure("failed to stamp static transform calibration");
            }
            rbt::tf::StaticSet static_set;
            static_set.revision = 1U;
            static_set.effective_from = std::move(static_stamp_result).value();
            static_set.effective_uncertainty_ns = std::nullopt;
            static_set.transforms.reserve(calibration.extrinsics.size());
            for (const auto& extrinsic : calibration.extrinsics) {
                static_set.transforms.push_back(rbt::tf::StaticEdge {
                    rbt::tf::FrameId(frame_id(options_.frame_prefix, extrinsic.parent)),
                    rbt::tf::FrameId(frame_id(options_.frame_prefix, extrinsic.child)),
                    transform_from_extrinsic(extrinsic),
                });
            }
            static_provider.replace(std::move(static_set));
            print_activation_report(context, calibration, device_clock_source, publishers);

            auto shared_device_clock = std::make_shared<rbt::time::DeviceClock>(std::move(device_clock));
            auto shared_host_clock = std::make_shared<rbt::time::HostClock>(std::move(host_clock));
            PublicationState video_state, imu_state;
            for (auto& publisher : publishers) {
                auto& state = is_imu_kind(publisher.kind) ? imu_state : video_state;
                state.publishers.push_back(std::move(publisher));
            }
            if (options_.imu) {
                const auto timeout = std::chrono::milliseconds(options_.frame_timeout_ms);
                (void)context.spawn_critical("orbbec-imu",
                    [device, device_clock = shared_device_clock, host_clock = shared_host_clock,
                     state = std::move(imu_state), timeout](std::stop_token stop) mutable {
                        try {
                            const auto started = std::chrono::steady_clock::now();
                            auto next_report = started + kPressureReportInterval;
                            for (const auto& publisher : state.publishers)
                                state.last_stream_frames.emplace(publisher.kind, started);
                            while (!stop.stop_requested()) {
                                const auto frames = device->poll_imu(kPollInterval);
                                for (const auto& frame : frames) {
                                    if (stop.stop_requested()) break;
                                    if (!is_imu_kind(frame.kind))
                                        throw std::runtime_error("non-IMU frame on independent IMU path");
                                    publish_frame(state, *device_clock, *host_clock, frame, true, stop);
                                }
                                const auto now = std::chrono::steady_clock::now();
                                for (const auto& [kind, last] : state.last_stream_frames)
                                    if (now - last >= timeout)
                                        throw std::runtime_error(std::string("no frame for requested IMU stream ")
                                                                 + kind_name(kind) + " before frame timeout");
                                if (now >= next_report) {
                                    publish_pressure_report(state.publishers, state.dropped_samples, state.loan_failures);
                                    next_report = now + kPressureReportInterval;
                                }
                            }
                            publish_pressure_report(state.publishers, state.dropped_samples, state.loan_failures);
                            return rbt::TaskOutcome::completed();
                        } catch (const std::exception& error) {
                            publish_pressure_report(state.publishers, state.dropped_samples, state.loan_failures);
                            return rbt::TaskOutcome::error(error.what());
                        } catch (...) {
                            return rbt::TaskOutcome::error("unknown error in Orbbec IMU loop");
                        }
                    });
            }

            auto camera_info_server = std::move(camera_info_server_result).value();
            auto control_server = std::move(control_server_result).value();
            auto frame_options = options_;
            auto task = context.spawn_critical(
                "orbbec-capture",
                [device, calibration,
                 frame_options = std::move(frame_options),
                 device_clock = shared_device_clock,
                 host_clock = shared_host_clock,
                 state = std::move(video_state),
                 camera_info_server = std::move(camera_info_server),
                 control_server = std::move(control_server),
                 static_provider = std::move(static_provider)](std::stop_token stop) mutable {
                    auto& publishers = state.publishers;
                    auto& dropped_samples = state.dropped_samples;
                    auto& loan_failures = state.loan_failures;
                    try {
                        const auto loop_started = std::chrono::steady_clock::now();
                        auto next_pressure_report = loop_started + kPressureReportInterval;
                        auto& last_stream_frames = state.last_stream_frames;
                        for (const auto& publisher : publishers) {
                            last_stream_frames.emplace(publisher.kind, loop_started);
                        }

                        while (!stop.stop_requested()) {
                            auto frames = device->poll(kPollInterval);
                            for (const auto& frame : frames) {
                                if (stop.stop_requested()) break;
                                if (is_imu_kind(frame.kind))
                                    throw std::runtime_error("IMU frame on video capture path");
                                publish_frame(state, *device_clock, *host_clock, frame, false);
                            }

                            for (std::size_t served = 0U; served < kRpcDrainPerIteration; ++served) {
                                auto incoming = camera_info_server.try_receive();
                                if (!incoming) {
                                    throw std::runtime_error(rpc_error_message("CameraInfo", incoming.error()));
                                }
                                auto request = std::move(incoming).value();
                                if (!request.has_value()) {
                                    break;
                                }
                                auto active = std::move(*request);
                                const auto response_payload = camera_info_response(
                                    active.payload(), frame_options, calibration);
                                auto response_result = active.loan_response();
                                if (!response_result) {
                                    if (is_cancelled_request_error(response_result.error())) {
                                        continue;
                                    }
                                    throw std::runtime_error(rpc_error_message("CameraInfo", response_result.error()));
                                }
                                auto response = std::move(response_result).value();
                                response.payload() = response_payload;
                                auto sent = response.send();
                                if (!sent) {
                                    if (is_cancelled_request_error(sent.error())) {
                                        continue;
                                    }
                                    throw std::runtime_error(rpc_error_message("CameraInfo", sent.error()));
                                }
                            }

                            for (std::size_t served = 0U; served < kRpcDrainPerIteration; ++served) {
                                auto incoming = control_server.try_receive();
                                if (!incoming) {
                                    throw std::runtime_error(rpc_error_message("Control", incoming.error()));
                                }
                                auto request = std::move(incoming).value();
                                if (!request.has_value()) {
                                    break;
                                }
                                auto active = std::move(*request);
                                const auto request_payload = active.payload();
                                rbt::orbbec::ControlResponse response_payload {};
                                if (request_payload.reserved != 0U || request_payload.operation > 1U
                                    || request_payload.property < kFirstControl
                                    || request_payload.property > kLastControl) {
                                    response_payload.status = kControlInvalid;
                                    set_control_message(response_payload, "invalid operation, property, or reserved field");
                                } else {
                                    try {
                                        if (request_payload.operation == 0U) {
                                            response_payload.value = device->get_control(request_payload.property);
                                        } else {
                                            device->set_control(request_payload.property, request_payload.value);
                                            response_payload.value = request_payload.value;
                                        }
                                        response_payload.status = kControlOk;
                                        set_control_message(response_payload, "ok");
                                    } catch (const UnsupportedControl& error) {
                                        response_payload.status = kControlUnsupported;
                                        set_control_message(response_payload, error.what());
                                    } catch (const std::invalid_argument& error) {
                                        response_payload.status = kControlInvalid;
                                        set_control_message(response_payload, error.what());
                                    } catch (const std::out_of_range& error) {
                                        response_payload.status = kControlInvalid;
                                        set_control_message(response_payload, error.what());
                                    } catch (const std::exception& error) {
                                        response_payload.status = kControlSdkError;
                                        set_control_message(response_payload, error.what());
                                    } catch (...) {
                                        response_payload.status = kControlSdkError;
                                        set_control_message(response_payload, "unknown SDK error");
                                    }
                                }
                                auto response_result = active.loan_response();
                                if (!response_result) {
                                    if (is_cancelled_request_error(response_result.error())) {
                                        continue;
                                    }
                                    throw std::runtime_error(rpc_error_message("Control", response_result.error()));
                                }
                                auto response = std::move(response_result).value();
                                response.payload() = response_payload;
                                auto sent = response.send();
                                if (!sent) {
                                    if (is_cancelled_request_error(sent.error())) {
                                        continue;
                                    }
                                    throw std::runtime_error(rpc_error_message("Control", sent.error()));
                                }
                            }

                            try {
                                (void)static_provider.serve_requests(kRpcDrainPerIteration);
                            } catch (const rbt::tf::TfTransportError& error) {
                                const auto rpc_error = error.rpc_error();
                                if (!rpc_error.has_value() || !is_cancelled_request_error(*rpc_error)) {
                                    throw;
                                }
                            }
                            const auto now = std::chrono::steady_clock::now();
                            for (const auto& [kind, last_frame] : last_stream_frames) {
                                if (now - last_frame >= std::chrono::milliseconds(frame_options.frame_timeout_ms)) {
                                    throw std::runtime_error(std::string("no frame for requested stream ")
                                                             + kind_name(kind) + " before frame timeout");
                                }
                            }
                            if (now >= next_pressure_report) {
                                publish_pressure_report(publishers, dropped_samples, loan_failures);
                                next_pressure_report = now + kPressureReportInterval;
                            }
                        }

                        publish_pressure_report(publishers, dropped_samples, loan_failures);
                        return rbt::TaskOutcome::completed();
                    } catch (const std::exception& error) {
                        publish_pressure_report(publishers, dropped_samples, loan_failures);
                        return rbt::TaskOutcome::error(error.what());
                    } catch (...) {
                        publish_pressure_report(publishers, dropped_samples, loan_failures);
                        return rbt::TaskOutcome::error("unknown error in Orbbec capture loop");
                    }
                });
            (void)task;
            return {};
        } catch (const std::exception& error) {
            return rbt::Result<void>::failure(error.what());
        } catch (...) {
            return rbt::Result<void>::failure("unknown error while activating Orbbec camera");
        }
    }

  private:
    Options options_;
};

} // namespace

std::unique_ptr<rbt::Component> make_camera_component(Options options) {
    return std::make_unique<CameraComponent>(std::move(options));
}

} // namespace rbt_orbbec

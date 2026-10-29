// SOFTWARE_FIXTURE: native Composer source for the M3T pipeline. No camera SDK
// or physical device is opened by this executable.
#include <rbt_m3t/app_config.hpp>
#include <rbt_orbbec/component.hpp>
#include <rbt/orbbec.hpp>
#include <rbt/runtime.hpp>
#include <rbt/time_stream.hpp>

#include <m3t/basic_depth_renderer.h>
#include <m3t/normal_renderer.h>
#include <m3t/renderer_geometry.h>
#include <m3t/body.h>

#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;
using CameraInfoServer = rbt::RpcServer<rbt::orbbec::CameraInfoRequest,
                                        rbt::orbbec::CameraInfoResponse>;
using namespace rbt_orbbec;

constexpr int kWidth = 256;
constexpr int kHeight = 256;
constexpr int kFramesPerSecond = 30;
constexpr std::int64_t kFramePeriodNs = 1'000'000'000LL / kFramesPerSecond;
constexpr std::string_view kSerial = "SOFTWARE_FIXTURE";
constexpr std::string_view kFrameId = "fixture/color_optical";
constexpr std::string_view kComponentId = "orbbec335";
constexpr std::size_t kMaximumSampleBytes = rbt::time::kStampedBytesPrefixBytes
                                             + kFrameHeaderBytes
                                             + static_cast<std::size_t>(kWidth * kHeight * 3);
constexpr std::size_t kMaximumJointPriorSampleBytes = rbt::time::kStampedBytesPrefixBytes
                                                       + 64U * 1024U;

struct Options {
    fs::path config;
    std::optional<fs::path> joint_prior_stop_file;
    std::string component_id { kComponentId };
};

[[nodiscard]] Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: rbt_m3t_rgbd_fixture --config FILE [--component-id ID] "
                         "[--joint-prior-stop-file FILE]\n";
            std::exit(0);
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument(std::string(argument) + " requires a value");
        }
        const std::string value(argv[++index]);
        if (argument == "--config") {
            options.config = fs::absolute(value);
        } else if (argument == "--component-id") {
            options.component_id = value;
        } else if (argument == "--joint-prior-stop-file") {
            options.joint_prior_stop_file = fs::absolute(value);
        } else {
            throw std::invalid_argument("unknown fixture option: " + std::string(argument));
        }
    }
    if (options.config.empty() || options.component_id.empty()) {
        throw std::invalid_argument("--config and a nonempty component id are required");
    }
    return options;
}

void copy_text(std::string_view source, std::uint8_t* destination, std::size_t capacity) {
    if (source.empty() || source.size() >= capacity) {
        throw std::length_error("SOFTWARE_FIXTURE camera metadata field is too long");
    }
    std::copy(source.begin(), source.end(), destination);
}

[[nodiscard]] rbt::orbbec::CameraInfoResponse camera_info(
    const rbt::orbbec::CameraInfoRequest& request) {
    rbt::orbbec::CameraInfoResponse response {};
    response.stream = request.stream;
    if (request.reserved != 0U || (request.stream != static_cast<std::uint32_t>(Kind::Color)
                                   && request.stream != static_cast<std::uint32_t>(Kind::Depth))) {
        response.status = 2U;
        return response;
    }
    response.status = 0U;
    response.width = kWidth;
    response.height = kHeight;
    response.fps = kFramesPerSecond;
    response.format = static_cast<std::uint32_t>(request.stream == static_cast<std::uint32_t>(Kind::Color)
                                                     ? Format::Rgb8 : Format::Depth16);
    response.fx = 220.0;
    response.fy = 220.0;
    response.cx = (kWidth - 1) / 2.0;
    response.cy = (kHeight - 1) / 2.0;
    response.distortion_model = 0U;
    copy_text(kFrameId, response.frame_id, sizeof(response.frame_id));
    copy_text(kSerial, response.serial, sizeof(response.serial));
    return response;
}

[[nodiscard]] std::string endpoint_name(std::string_view capability,
                                        std::string_view endpoint) {
    return "rbt/cap/" + std::string(capability) + "/" + std::string(endpoint);
}

struct StreamPublisher {
    Kind kind;
    std::string service;
    std::string capability;
    rbt::time::StampedBytesPublisher publisher;
    std::uint64_t sequence { 1U };
};

void publish(StreamPublisher& binding,
              const rbt::time::Timestamp& capture,
              const rbt::time::Timestamp& received,
              std::uint64_t device_index,
              std::span<const std::uint8_t> pixels,
              std::uint32_t stride) {
    const auto kind = binding.kind;
    const auto format = kind == Kind::Color ? Format::Rgb8 : Format::Depth16;
    const auto scale = kind == Kind::Color ? 1.0 : 0.001;
    const auto sample_bytes = rbt::time::kStampedBytesPrefixBytes + kFrameHeaderBytes + pixels.size();
    auto loan_result = binding.publisher.loan_slice(sample_bytes);
    if (!loan_result) {
        throw std::runtime_error(std::string("fixture cannot loan ") + kind_name(kind)
                                 + " sample: " + rbt::to_string(loan_result.error()));
    }
    auto sample = std::move(loan_result).value();
    sample.header() = binding.sequence;
    auto bytes = sample.payload();
    const auto timing = rbt::time::SensorTiming::create(capture, received, std::nullopt, std::nullopt);
    if (!timing || rbt::time::write_timing_prefix(
                       bytes.first(rbt::time::kStampedBytesPrefixBytes), timing.value())
                       != rbt::time::PrefixCodecError::None) {
        throw std::runtime_error("fixture cannot encode frame timing");
    }
    const Frame frame { kind, format, static_cast<std::uint32_t>(kWidth),
                        static_cast<std::uint32_t>(kHeight), stride, device_index,
                        static_cast<std::uint64_t>(capture.ns / 1000), scale, pixels, {} };
    encode_frame(bytes.subspan(rbt::time::kStampedBytesPrefixBytes), frame, kFrameId);
    auto sent = sample.send();
    if (!sent) {
        throw std::runtime_error(std::string("fixture cannot publish ") + kind_name(kind)
                                 + " sample: " + rbt::to_string(sent.error()));
    }
    if (binding.sequence == std::numeric_limits<std::uint64_t>::max()) {
        throw std::runtime_error("fixture stream sequence overflow");
    }
    ++binding.sequence;
}

[[nodiscard]] std::size_t source_arm_index(std::string_view arm) {
    if (arm == "left") return 0U;
    if (arm == "right") return 1U;
    throw std::invalid_argument("fixture joint-prior mapping source_arm must be left or right");
}

[[nodiscard]] std::size_t source_joint_index(std::string_view joint) {
    constexpr std::array<std::string_view, 6> joints {
        "shoulder_pan", "shoulder_lift", "elbow_flex", "wrist_flex", "wrist_roll", "gripper"
    };
    const auto found = std::find(joints.begin(), joints.end(), joint);
    if (found == joints.end()) {
        throw std::invalid_argument("fixture joint-prior mapping source_joint is not canonical");
    }
    return static_cast<std::size_t>(found - joints.begin());
}

[[nodiscard]] std::array<std::array<double, 6>, 2> source_joint_values(
    const rbt_m3t::TrackingConfig& config, std::span<const double> truth_angles) {
    std::array<std::array<double, 6>, 2> values {};
    for (const auto& mapping : config.joint_prior.mappings) {
        const auto arm = std::find_if(config.arms.begin(), config.arms.end(), [&mapping](const auto& candidate) {
            return candidate.name == mapping.arm;
        });
        if (arm == config.arms.end() || mapping.joint != "shoulder") {
            throw std::invalid_argument("fixture joint-prior mappings must target a configured shoulder");
        }
        const auto arm_index = static_cast<std::size_t>(arm - config.arms.begin());
        values[source_arm_index(mapping.source_arm)][source_joint_index(mapping.source_joint)] =
            (truth_angles[arm_index] - mapping.offset_rad) / mapping.scale_rad_per_unit;
    }
    return values;
}

void publish_joint_state(rbt::time::StampedBytesPublisher& publisher,
                         std::uint64_t& sequence,
                         const rbt_m3t::TrackingConfig& config,
                         std::string_view driver_session,
                         std::span<const double> truth_angles,
                         const rbt::time::Timestamp& capture,
                         const rbt::time::Timestamp& received) {
    if (sequence > 9'007'199'254'740'991U) {
        throw std::runtime_error("fixture joint-prior sequence exceeds the exact safe integer range");
    }
    const auto values = source_joint_values(config, truth_angles);
    std::ostringstream json;
    json << std::setprecision(17)
         << "{\"protocol\":\"rbt.so101.dual.v1\",\"kind\":\"state\","
            "\"units\":\"joint_deg_gripper_percent\",\"sequence\":" << sequence
         << ",\"controller_id\":" << rbt_orbbec::json_quote(config.joint_prior.controller_id)
         << ",\"driver_session\":" << rbt_orbbec::json_quote(driver_session)
         << ",\"fault\":null,\"arms\":{\"left\":[";
    for (std::size_t joint = 0U; joint < values[0].size(); ++joint) {
        if (joint != 0U) json << ',';
        json << values[0][joint];
    }
    json << "],\"right\":[";
    for (std::size_t joint = 0U; joint < values[1].size(); ++joint) {
        if (joint != 0U) json << ',';
        json << values[1][joint];
    }
    json << "]}}";
    const auto serialized = json.str();
    const auto sample_bytes = rbt::time::kStampedBytesPrefixBytes + serialized.size();
    auto loan_result = publisher.loan_slice(sample_bytes);
    if (!loan_result) {
        throw std::runtime_error(std::string("fixture cannot loan joint-prior sample: ")
                                 + rbt::to_string(loan_result.error()));
    }
    auto sample = std::move(loan_result).value();
    sample.header() = sequence;
    auto bytes = sample.payload();
    const auto timing = rbt::time::SensorTiming::create(capture, received, std::nullopt, std::nullopt);
    if (!timing || rbt::time::write_timing_prefix(
                       bytes.first(rbt::time::kStampedBytesPrefixBytes), timing.value())
                       != rbt::time::PrefixCodecError::None) {
        throw std::runtime_error("fixture cannot encode joint-prior timing");
    }
    std::memcpy(bytes.data() + rbt::time::kStampedBytesPrefixBytes,
                serialized.data(), serialized.size());
    auto sent = sample.send();
    if (!sent) {
        throw std::runtime_error(std::string("fixture cannot publish joint-prior sample: ")
                                 + rbt::to_string(sent.error()));
    }
    ++sequence;
}

[[nodiscard]] std::array<std::uint8_t, 16> synthetic_clock_id() {
    // A fixed, visibly synthetic source identity. It is never derived from hardware.
    constexpr std::array<std::uint8_t, 16> id {
        'S', 'O', 'F', 'T', 'W', 'A', 'R', 'E', '_', 'F', 'I', 'X', 'T', 'U', 'R', 'E'
    };
    return id;
}

[[nodiscard]] Eigen::Affine3f to_eigen(const m3t::Transform3fA& pose) {
    Eigen::Affine3f result = Eigen::Affine3f::Identity();
    result.matrix() = pose.matrix();
    return result;
}

[[nodiscard]] std::string body_name(const std::string& arm, const std::string& link) {
    return arm + "/" + link + "/fixture_body";
}

struct FixtureRenderer {
    std::shared_ptr<m3t::RendererGeometry> geometry;
    std::vector<std::shared_ptr<m3t::Body>> bodies;
    std::vector<std::vector<std::shared_ptr<m3t::Body>>> arm_bodies;
    std::unique_ptr<m3t::FullNormalRenderer> normal;
    std::unique_ptr<m3t::FullBasicDepthRenderer> depth;

    explicit FixtureRenderer(const rbt_m3t::TrackingConfig& config)
        : geometry{std::make_shared<m3t::RendererGeometry>("rbt_m3t_fixture_geometry")} {
        const m3t::Intrinsics intrinsics { 220.0F, 220.0F, (kWidth - 1) / 2.0F,
                                           (kHeight - 1) / 2.0F, kWidth, kHeight };
        arm_bodies.resize(config.arms.size());
        for (std::size_t arm_index = 0U; arm_index < config.arms.size(); ++arm_index) {
            const auto& arm = config.arms[arm_index];
            for (const auto& link : config.model.links) {
                if (link.mesh.empty()) {
                    continue;
                }
                auto body = std::make_shared<m3t::Body>(body_name(arm.name, link.name), link.mesh,
                    1.0F, true, false, m3t::Transform3fA::Identity());
                if (!body->SetUp() || !geometry->AddBody(body)) {
                    throw std::runtime_error("fixture cannot load renderer body " + link.name);
                }
                bodies.push_back(body);
                arm_bodies[arm_index].push_back(std::move(body));
            }
        }
        if (bodies.empty() || !geometry->SetUp()) {
            throw std::runtime_error("fixture cannot set up the M3T OpenGL renderer geometry");
        }
        const m3t::Transform3fA camera_pose = m3t::Transform3fA::Identity();
        normal = std::make_unique<m3t::FullNormalRenderer>(
            "rbt_m3t_fixture_normals", geometry, camera_pose, intrinsics, 0.1F, 2.0F);
        depth = std::make_unique<m3t::FullBasicDepthRenderer>(
            "rbt_m3t_fixture_depth", geometry, camera_pose, intrinsics, 0.1F, 2.0F);
        if (!normal->SetUp() || !depth->SetUp()) {
            throw std::runtime_error("fixture cannot set up M3T original renderers");
        }
    }

    [[nodiscard]] std::vector<double> truth_joint_angles(
        const rbt_m3t::TrackingConfig& config, std::uint64_t frame_index) const {
        const auto phase = static_cast<double>(frame_index % 100U) / 100.0;
        const auto triangle = phase < 0.5 ? -1.0 + 4.0 * phase : 3.0 - 4.0 * phase;
        std::vector<double> result;
        result.reserve(config.arms.size());
        for (std::size_t arm_index = 0U; arm_index < config.arms.size(); ++arm_index) {
            // The bounded triangular motion keeps a frozen-seed result detectable
            // even when renderer or optimizer setup takes several seconds.
            const auto seed_angle = config.arms[arm_index].joint_angles.at("shoulder");
            result.push_back(seed_angle + (arm_index == 0U
                ? 0.15 + 0.22 * triangle : -0.15 - 0.22 * triangle));
        }
        return result;
    }

    void set_truth_poses(const rbt_m3t::TrackingConfig& config,
                         std::span<const double> truth_angles) {
        if (truth_angles.size() != config.arms.size()) {
            throw std::invalid_argument("fixture truth-angle count does not match configured arms");
        }
        for (std::size_t arm_index = 0U; arm_index < config.arms.size(); ++arm_index) {
            auto truth = config.arms[arm_index];
            truth.joint_angles["shoulder"] = truth_angles[arm_index];
            const auto poses = rbt_m3t::forward_kinematics(config.model, truth);
            for (std::size_t link_index = 0U; link_index < poses.size(); ++link_index) {
                const auto found = std::find_if(config.model.links.begin(), config.model.links.end(),
                    [&poses, &link_index](const rbt_m3t::ModelLink& link) {
                        return link.name == poses[link_index].link;
                    });
                if (found == config.model.links.end() || found->mesh.empty()) {
                    continue;
                }
                const auto mesh_index = static_cast<std::size_t>(found - config.model.links.begin());
                auto& body = arm_bodies[arm_index].at(mesh_index);
                body->set_body2world_pose(to_eigen(poses[link_index].in_camera));
            }
        }
    }

    void render(cv::Mat& rgb, cv::Mat& depth_u16) {
        if (!normal->StartRendering() || !normal->FetchNormalImage()
            || !depth->StartRendering() || !depth->FetchDepthImage()) {
            throw std::runtime_error("M3T original renderer failed to produce fixture pixels");
        }
        cv::cvtColor(normal->normal_image(), rgb, cv::COLOR_BGRA2RGB);
        depth_u16 = cv::Mat::zeros(kHeight, kWidth, CV_16UC1);
        for (int row = 0; row < kHeight; ++row) {
            auto* output = depth_u16.ptr<std::uint16_t>(row);
            for (int column = 0; column < kWidth; ++column) {
                const auto depth_m = depth->Depth(cv::Point { column, row });
                if (std::isfinite(depth_m) && depth_m > 0.0F) {
                    const auto millimetres = std::lround(static_cast<double>(depth_m) * 1000.0);
                    if (millimetres > 0L && millimetres < 65535L) {
                        output[column] = static_cast<std::uint16_t>(millimetres);
                    }
                }
            }
        }
    }
};

class FixtureComponent final : public rbt::Component {
  public:
    explicit FixtureComponent(Options options) : options_{std::move(options)} {}

    rbt::Result<void> prepare() override {
        try {
            config_ = std::make_shared<rbt_m3t::TrackingConfig>(
                rbt_m3t::load_tracking_config(options_.config));
            if (config_->serial != kSerial || config_->coordinate_frame != kFrameId
                || config_->arms.size() != 2U || config_->model.joints.size() != 1U
                || config_->model.joints.front().name != "shoulder") {
                return rbt::Result<void>::failure("tracking config is not the SOFTWARE_FIXTURE model");
            }
            return {};
        } catch (const std::exception& error) {
            return rbt::Result<void>::failure(error.what());
        }
    }

    rbt::ComponentId id() const override { return options_.component_id; }
    std::vector<rbt::RequirementSpec> requirements() const override { return {}; }

    rbt::Result<void> activate(rbt::ActivationContext& context) override {
        try {
            std::vector<StreamPublisher> publishers;
            for (const auto kind : { Kind::Color, Kind::Depth }) {
                const auto capability = rbt::generate_uuid();
                const auto descriptor = rbt::StreamDescriptor(
                    endpoint_name(capability, "stream"), rbt::time::STAMPED_BYTES_DESCRIPTOR,
                    rbt::StreamProfile::Sensor);
                auto publisher = rbt::time::create_stamped_bytes_publisher(descriptor, kMaximumSampleBytes);
                if (!publisher) {
                    return rbt::Result<void>::failure(std::string("fixture cannot create ")
                        + kind_name(kind) + " stream: " + rbt::to_string(publisher.error()));
                }
                if (kind == Kind::Color) {
                    (void)context.provide<ColorCapability>(capability, descriptor);
                } else {
                    (void)context.provide<DepthCapability>(capability, descriptor);
                }
                publishers.push_back({ kind, descriptor.service_name, capability,
                                       std::move(publisher).value(), 1U });
            }

            std::optional<rbt::time::StampedBytesPublisher> joint_prior_publisher;
            std::string driver_session;
            if (config_->joint_prior.enabled) {
                const auto descriptor = rbt::StreamDescriptor(
                    config_->joint_prior.service_name, rbt::time::STAMPED_BYTES_DESCRIPTOR,
                    rbt::StreamProfile::Sensor);
                auto publisher = rbt::time::create_stamped_bytes_publisher(
                    descriptor, kMaximumJointPriorSampleBytes);
                if (!publisher) {
                    return rbt::Result<void>::failure(std::string("fixture cannot create joint-prior stream: ")
                        + rbt::to_string(publisher.error()));
                }
                joint_prior_publisher.emplace(std::move(publisher).value());
                driver_session = rbt::generate_uuid();
            }

            const auto info_id = rbt::generate_uuid();
            const auto info_descriptor = rbt::RpcDescriptor::for_fixed<
                rbt::orbbec::CameraInfoRequest, rbt::orbbec::CameraInfoResponse>(
                    endpoint_name(info_id, "rpc"));
            auto info_result = CameraInfoServer::open_or_create_for_capability(info_descriptor, info_id);
            if (!info_result) {
                return rbt::Result<void>::failure("fixture cannot create CameraInfo RPC server");
            }
            (void)context.provide_rpc<CameraInfoCapability>(
                info_id, rbt::OwnedRpcDescriptor::from(info_descriptor));

            std::cout << "{\"event\":\"activation\",\"mode\":\"SOFTWARE_FIXTURE\","
                         "\"hardware\":\"NOT RUN\",\"component_id\":"
                      << json_quote(options_.component_id) << ",\"serial\":" << json_quote(kSerial)
                      << ",\"clock_source\":\"synthetic_device_clock\",\"streams\":[";
            for (std::size_t index = 0U; index < publishers.size(); ++index) {
                if (index != 0U) std::cout << ',';
                std::cout << "{\"kind\":" << json_quote(kind_name(publishers[index].kind))
                          << ",\"service\":" << json_quote(publishers[index].service) << '}';
            }
            std::cout << "]}" << std::endl;

            auto config = config_;
            const auto joint_prior_stop_file = options_.joint_prior_stop_file;
            auto info_server = std::move(info_result).value();
            (void)context.spawn_critical("m3t-rgbd-software-fixture",
                [config, publishers = std::move(publishers),
                 joint_prior_publisher = std::move(joint_prior_publisher),
                 driver_session = std::move(driver_session), joint_prior_stop_file,
                 info_server = std::move(info_server)]
                (std::stop_token stop) mutable -> rbt::TaskOutcome {
                    try {
                        FixtureRenderer renderer(*config);
                        std::array<std::uint8_t, 16> id = synthetic_clock_id();
                        auto device_clock_result = rbt::time::DeviceClock::create(id);
                        auto host_clock_result = rbt::time::HostClock::create();
                        if (!device_clock_result || !host_clock_result) {
                            throw std::runtime_error("fixture cannot create synthetic clocks");
                        }
                        auto device_clock = std::move(device_clock_result).value();
                        auto host_clock = std::move(host_clock_result).value();
                        const auto identity = device_clock.identity();
                        if (!identity) throw std::runtime_error("fixture clock identity is invalid");
                        std::cout << "{\"event\":\"fixture_ready\",\"mode\":\"SOFTWARE_FIXTURE\","
                                     "\"hardware\":\"NOT RUN\",\"clock_source\":"
                                  << json_quote(identity.value().source_id().to_hex())
                                  << ",\"generation\":0}" << std::endl;
                        auto next_frame = std::chrono::steady_clock::now();
                        std::uint64_t frame_index = 1U;
                        std::uint64_t joint_prior_sequence = 1U;
                        while (!stop.stop_requested()) {
                            const auto truth_angles = renderer.truth_joint_angles(*config, frame_index - 1U);
                            renderer.set_truth_poses(*config, truth_angles);
                            cv::Mat color_rgb, depth_u16;
                            renderer.render(color_rgb, depth_u16);
                            const auto capture = device_clock.stamp(
                                static_cast<std::int64_t>(frame_index) * kFramePeriodNs);
                            const auto received = host_clock.now_monotonic();
                            if (!capture || !received) {
                                throw std::runtime_error("fixture cannot stamp a synthetic RGB-D pair");
                            }
                            std::error_code stop_file_error;
                            const bool stop_joint_prior = joint_prior_stop_file
                                && fs::exists(*joint_prior_stop_file, stop_file_error);
                            if (stop_file_error) {
                                throw std::runtime_error("fixture cannot inspect joint-prior stop file: "
                                                         + stop_file_error.message());
                            }
                            if (joint_prior_publisher && !stop_joint_prior) {
                                publish_joint_state(*joint_prior_publisher, joint_prior_sequence, *config,
                                    driver_session, truth_angles, capture.value(), received.value());
                            }
                            for (auto& binding : publishers) {
                                const auto& image = binding.kind == Kind::Color ? color_rgb : depth_u16;
                                const auto pixel_bytes = image.total() * image.elemSize();
                                const auto pixels = std::span<const std::uint8_t>(image.data, pixel_bytes);
                                publish(binding, capture.value(), received.value(), frame_index, pixels,
                                    static_cast<std::uint32_t>(image.step));
                            }
                            for (std::size_t served = 0U; served < 8U; ++served) {
                                auto incoming = info_server.try_receive();
                                if (!incoming) {
                                    if (stop.stop_requested()) return rbt::TaskOutcome::completed();
                                    throw std::runtime_error("fixture CameraInfo request receive failed");
                                }
                                auto request = std::move(incoming).value();
                                if (!request) break;
                                auto active = std::move(*request);
                                const auto response_payload = camera_info(active.payload());
                                auto response_result = active.loan_response();
                                if (!response_result) {
                                    if (stop.stop_requested()) return rbt::TaskOutcome::completed();
                                    if (response_result.error() == rbt::RpcError::Cancelled
                                        || response_result.error() == rbt::RpcError::DeadlineExceeded) {
                                        continue;
                                    }
                                    throw std::runtime_error("fixture CameraInfo response loan failed");
                                }
                                auto response = std::move(response_result).value();
                                response.payload() = response_payload;
                                auto sent = response.send();
                                if (!sent && !stop.stop_requested()
                                    && sent.error() != rbt::RpcError::Cancelled
                                    && sent.error() != rbt::RpcError::DeadlineExceeded) {
                                    throw std::runtime_error("fixture CameraInfo response send failed");
                                }
                            }
                            ++frame_index;
                            next_frame += 33ms;
                            std::this_thread::sleep_until(next_frame);
                        }
                        return rbt::TaskOutcome::completed();
                    } catch (const std::exception& error) {
                        if (stop.stop_requested()) return rbt::TaskOutcome::completed();
                        return rbt::TaskOutcome::error(error.what());
                    }
                });
            return {};
        } catch (const std::exception& error) {
            return rbt::Result<void>::failure(error.what());
        }
    }

  private:
    Options options_;
    std::shared_ptr<rbt_m3t::TrackingConfig> config_;
};

} // namespace

int main(int argc, char** argv) {
    try {
        auto options = parse_options(argc, argv);
        rbt::RuntimeOptions runtime_options;
        if (const char* address = std::getenv("RBT_COMPOSER_ADDR")) {
            runtime_options.composer_address = address;
        }
        rbt::ComponentRuntime runtime(std::make_unique<FixtureComponent>(std::move(options)),
                                      std::move(runtime_options));
        return runtime.run();
    } catch (const std::exception& error) {
        std::cerr << "rbt_m3t_rgbd_fixture: " << error.what() << '\n';
        return 2;
    }
}

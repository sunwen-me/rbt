#include <rbt_orbbec/driver.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;
using rbt_orbbec::Calibration;
using rbt_orbbec::Frame;
using rbt_orbbec::Kind;
using rbt_orbbec::Options;
using rbt_orbbec::VideoCalibration;

constexpr std::uint32_t kWidth = 640U;
constexpr std::uint32_t kHeight = 480U;
constexpr std::uint32_t kDecimatedWidth = 320U;
constexpr std::uint32_t kDecimatedHeight = 240U;
constexpr double kMinimumDepthM = 0.2;
constexpr double kMaximumDepthM = 4.0;
constexpr auto kFrameDeadline = 10s;

struct Arguments {
    std::string serial;
    std::string mode;
    std::uint64_t count { 30U };
};

struct DeviceGuard {
    std::unique_ptr<rbt_orbbec::CaptureDevice> device;
    ~DeviceGuard() {
        if (device) {
            device->stop();
        }
    }
};

struct StreamTimeline {
    std::uint64_t count { 0U };
    std::uint64_t first_index { 0U };
    std::uint64_t last_index { 0U };
    std::uint64_t first_capture_ns { 0U };
    std::uint64_t last_capture_ns { 0U };
};

struct FrameEvidence {
    StreamTimeline depth;
    StreamTimeline points;
    double depth_unit_scale { 0.0 };
    std::uint64_t nonzero_depth_values { 0U };
    double minimum_nonzero_depth_m { std::numeric_limits<double>::infinity() };
    double maximum_nonzero_depth_m { 0.0 };
    std::uint64_t finite_point_values { 0U };
    std::uint64_t zero_invalid_points { 0U };
};

[[nodiscard]] std::uint64_t parse_positive(std::string_view text, std::string_view option) {
    if (text.empty()) {
        throw std::invalid_argument(std::string(option) + " requires a positive decimal integer");
    }
    std::uint64_t value = 0U;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            throw std::invalid_argument(std::string(option) + " requires a positive decimal integer");
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            throw std::out_of_range(std::string(option) + " is too large");
        }
        value = value * 10U + digit;
    }
    if (value == 0U || value > 20000U) {
        throw std::out_of_range(std::string(option) + " must be in 1..20000");
    }
    return value;
}

[[nodiscard]] Arguments parse_arguments(int argc, char** argv) {
    Arguments result;
    bool serial_set = false;
    bool mode_set = false;
    bool count_set = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--help" || option == "-h") {
            std::cout << "Usage: rbt_orbbec_hardware_processing_probe --serial SN "
                         "--mode align|decimate|both [--count N]\n";
            std::exit(0);
        }
        if (option != "--serial" && option != "--mode" && option != "--count") {
            throw std::invalid_argument("unknown option: " + std::string(option));
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument(std::string(option) + " requires a value");
        }
        const std::string_view value(argv[++index]);
        if (value.empty() || (value.size() >= 2U && value[0] == '-' && value[1] == '-')) {
            throw std::invalid_argument(std::string(option) + " requires a value");
        }
        if (option == "--serial") {
            if (serial_set) {
                throw std::invalid_argument("duplicate --serial option");
            }
            result.serial = value;
            serial_set = true;
        } else if (option == "--mode") {
            if (mode_set) {
                throw std::invalid_argument("duplicate --mode option");
            }
            result.mode = value;
            mode_set = true;
        } else {
            if (count_set) {
                throw std::invalid_argument("duplicate --count option");
            }
            result.count = parse_positive(value, option);
            count_set = true;
        }
    }
    const auto first_serial_character = result.serial.find_first_not_of(" \t\r\n");
    if (!serial_set || result.serial.empty() || first_serial_character != 0U
        || result.serial.find_last_not_of(" \t\r\n") != result.serial.size() - 1U) {
        throw std::invalid_argument("an explicit non-empty --serial SN is required");
    }
    if (!mode_set || (result.mode != "align" && result.mode != "decimate" && result.mode != "both")) {
        throw std::invalid_argument("--mode must be align, decimate, or both");
    }
    return result;
}

[[nodiscard]] const VideoCalibration& video(const Calibration& calibration, Kind kind) {
    const auto found = std::find_if(calibration.videos.begin(), calibration.videos.end(),
                                    [kind](const VideoCalibration& candidate) {
                                        return candidate.kind == kind;
                                    });
    if (found == calibration.videos.end()) {
        throw std::runtime_error(std::string("calibration omitted ") + rbt_orbbec::kind_name(kind));
    }
    return *found;
}

void validate_intrinsics(const VideoCalibration& calibration, std::string_view label) {
    const std::array<double, 4> values { calibration.fx, calibration.fy, calibration.cx, calibration.cy };
    if (!std::all_of(values.begin(), values.end(), [](double value) { return std::isfinite(value); })
        || calibration.fx <= 0.0 || calibration.fy <= 0.0
        || !std::all_of(calibration.distortion.begin(), calibration.distortion.end(),
                        [](double value) { return std::isfinite(value); })) {
        throw std::runtime_error(std::string(label) + " calibration contains non-finite or invalid intrinsics");
    }
}

[[nodiscard]] bool sdk_close(double lhs, double rhs) noexcept {
    constexpr double absolute_tolerance = 1e-4;
    constexpr double relative_tolerance = 1e-6;
    return std::abs(lhs - rhs) <= absolute_tolerance + relative_tolerance * std::max(std::abs(lhs), std::abs(rhs));
}

[[nodiscard]] std::string calibration_json(const VideoCalibration& calibration);

void validate_alignment(const VideoCalibration& depth, const VideoCalibration& color) {
    if (depth.width != color.width || depth.height != color.height) {
        throw std::runtime_error("aligned depth calibration does not use the color image dimensions; depth="
                                 + calibration_json(depth) + "; color=" + calibration_json(color));
    }
    if (depth.distortion_model != color.distortion_model || !sdk_close(depth.fx, color.fx)
        || !sdk_close(depth.fy, color.fy) || !sdk_close(depth.cx, color.cx) || !sdk_close(depth.cy, color.cy)) {
        throw std::runtime_error("aligned depth intrinsics do not match the SDK color calibration; depth="
                                 + calibration_json(depth) + "; color=" + calibration_json(color));
    }
    for (std::size_t index = 0U; index < depth.distortion.size(); ++index) {
        if (!sdk_close(depth.distortion[index], color.distortion[index])) {
            throw std::runtime_error("aligned depth distortion does not match the SDK color calibration; depth="
                                     + calibration_json(depth) + "; color=" + calibration_json(color));
        }
    }
}

[[nodiscard]] std::string calibration_json(const VideoCalibration& calibration) {
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<double>::max_digits10)
           << "{\"width\":" << calibration.width
           << ",\"height\":" << calibration.height
           << ",\"fps\":" << calibration.fps
           << ",\"fx\":" << calibration.fx
           << ",\"fy\":" << calibration.fy
           << ",\"cx\":" << calibration.cx
           << ",\"cy\":" << calibration.cy
           << ",\"distortion_model\":" << calibration.distortion_model
           << ",\"distortion\":[";
    for (std::size_t index = 0U; index < calibration.distortion.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        output << calibration.distortion[index];
    }
    output << "]}";
    return output.str();
}

void validate_timeline(const Frame& frame, StreamTimeline& timeline) {
    if (frame.index == 0U || frame.capture_us == 0U
        || frame.capture_us > std::numeric_limits<std::uint64_t>::max() / 1000U) {
        throw std::runtime_error(std::string(rbt_orbbec::kind_name(frame.kind))
                                 + " frame has a missing index or capture timestamp");
    }
    const auto capture_ns = frame.capture_us * 1000U;
    if (timeline.count != 0U
        && (frame.index < timeline.last_index || capture_ns < timeline.last_capture_ns)) {
        throw std::runtime_error(std::string(rbt_orbbec::kind_name(frame.kind))
                                 + " frame index or capture timestamp regressed");
    }
    if (timeline.count == 0U) {
        timeline.first_index = frame.index;
        timeline.first_capture_ns = capture_ns;
    }
    timeline.last_index = frame.index;
    timeline.last_capture_ns = capture_ns;
    ++timeline.count;
}

void inspect_depth(const Frame& frame, const VideoCalibration& calibration, bool aligned, FrameEvidence& evidence) {
    if (frame.format != rbt_orbbec::Format::Depth16 || frame.width != calibration.width
        || frame.height != calibration.height || frame.width == 0U || frame.height == 0U
        || frame.stride < frame.width * 2U || frame.unit_scale <= 0.0 || !std::isfinite(frame.unit_scale)) {
        throw std::runtime_error("depth frame format, dimensions, stride, or unit scale is invalid");
    }
    const auto required_bytes = static_cast<std::uint64_t>(frame.stride) * frame.height;
    if (required_bytes > frame.data.size()) {
        throw std::runtime_error("depth frame payload is shorter than its advertised dimensions");
    }
    evidence.depth_unit_scale = frame.unit_scale;
    for (std::uint32_t row = 0U; row < frame.height; ++row) {
        const auto row_offset = static_cast<std::size_t>(row) * frame.stride;
        for (std::uint32_t column = 0U; column < frame.width; ++column) {
            const auto offset = row_offset + static_cast<std::size_t>(column) * 2U;
            const auto value = static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(frame.data[offset])
                | static_cast<std::uint16_t>(static_cast<std::uint16_t>(frame.data[offset + 1U]) << 8U));
            if (value == std::uint16_t { 0U }) {
                continue;
            }
            const double depth_m = static_cast<double>(value) * frame.unit_scale;
            // Thresholding precedes D2C. D2C rotates/translates Z into the color
            // camera, so the source-camera Z interval is not an output bound.
            if (!std::isfinite(depth_m) || (!aligned && (depth_m < kMinimumDepthM - 1e-3
                || depth_m > kMaximumDepthM + 1e-3))) {
                throw std::runtime_error("nonzero depth value is outside the 0.2..4.0 metre filter range");
            }
            ++evidence.nonzero_depth_values;
            evidence.minimum_nonzero_depth_m = std::min(evidence.minimum_nonzero_depth_m, depth_m);
            evidence.maximum_nonzero_depth_m = std::max(evidence.maximum_nonzero_depth_m, depth_m);
        }
    }
}

void inspect_points(const Frame& frame, const Frame& depth, FrameEvidence& evidence) {
    const auto expected_bytes = static_cast<std::uint64_t>(frame.width) * frame.height * 3U * sizeof(float);
    if (frame.format != rbt_orbbec::Format::Xyz32f || frame.width != depth.width || frame.height != depth.height
        || frame.stride != frame.width * 3U * sizeof(float) || frame.data.size() != expected_bytes
        || frame.unit_scale != 1.0 || frame.index != depth.index || frame.capture_us != depth.capture_us) {
        throw std::runtime_error("point frame dimensions, format, stride, or metre scale do not match depth");
    }
    for (std::size_t offset = 0U; offset < frame.data.size(); offset += 3U * sizeof(float)) {
        std::array<float, 3> point {};
        std::memcpy(point.data(), frame.data.data() + offset, sizeof(point));
        if (!std::all_of(point.begin(), point.end(), [](float value) { return std::isfinite(value); })) {
            throw std::runtime_error("point frame contains a non-finite coordinate");
        }
        if (point[0] == 0.0F && point[1] == 0.0F && point[2] == 0.0F) {
            ++evidence.zero_invalid_points;
            continue;
        }
        const auto pixel = offset / (3U * sizeof(float));
        const auto depth_offset = (pixel / depth.width) * depth.stride + (pixel % depth.width) * 2U;
        const auto depth_value = static_cast<std::uint16_t>(depth.data[depth_offset])
            | (static_cast<std::uint16_t>(depth.data[depth_offset + 1U]) << 8U);
        const auto expected_z_m = static_cast<double>(depth_value) * depth.unit_scale;
        if (point[2] <= 0.0F || std::abs(point[2] - expected_z_m) > 1e-5 * std::max(1.0, expected_z_m)) {
            throw std::runtime_error("point Z in metres does not match its processed depth pixel");
        }
        ++evidence.finite_point_values;
    }
}

[[nodiscard]] Options make_options(const Arguments& arguments) {
    Options options;
    options.serial = arguments.serial;
    options.width = kWidth;
    options.height = kHeight;
    options.fps = 30U;
    options.points = true;
    options.align_depth_to_color = arguments.mode == "align" || arguments.mode == "both";
    if (arguments.mode == "decimate" || arguments.mode == "both") {
        options.depth_filters.push_back(rbt_orbbec::parse_depth_filter("DecimationFilter:decimate=2"));
    }
    options.depth_filters.push_back(rbt_orbbec::parse_depth_filter("ThresholdFilter:min=200,max=4000"));
    return options;
}

void validate_calibrations(const Calibration& calibration, const Arguments& arguments,
                           const VideoCalibration*& depth_out, const VideoCalibration*& color_out) {
    color_out = &video(calibration, Kind::Color);
    depth_out = &video(calibration, Kind::Depth);
    validate_intrinsics(*color_out, "color");
    validate_intrinsics(*depth_out, "depth");
    if (calibration.device.serial != arguments.serial) {
        throw std::runtime_error("opened device serial differs from the requested serial");
    }
    const bool aligned = arguments.mode == "align" || arguments.mode == "both";
    const auto expected_width = aligned ? kWidth
        : (arguments.mode == "decimate" ? kDecimatedWidth : kWidth);
    const auto expected_height = aligned ? kHeight
        : (arguments.mode == "decimate" ? kDecimatedHeight : kHeight);
    if ((*depth_out).width != expected_width || (*depth_out).height != expected_height) {
        throw std::runtime_error("depth calibration dimensions do not match the requested processing mode");
    }
    if (color_out->width != kWidth || color_out->height != kHeight) {
        throw std::runtime_error("color calibration is not 640x480");
    }
    if (aligned) {
        validate_alignment(*depth_out, *color_out);
    }
}

[[nodiscard]] std::string evidence_json(const FrameEvidence& evidence) {
    std::ostringstream output;
    output << std::setprecision(std::numeric_limits<double>::max_digits10)
           << "{\"depth\":{\"count\":" << evidence.depth.count
           << ",\"first_index\":" << evidence.depth.first_index
           << ",\"last_index\":" << evidence.depth.last_index
           << ",\"first_capture_ns\":" << evidence.depth.first_capture_ns
           << ",\"last_capture_ns\":" << evidence.depth.last_capture_ns
           << ",\"unit_scale_m\":" << evidence.depth_unit_scale
           << ",\"nonzero_values\":" << evidence.nonzero_depth_values
           << ",\"minimum_nonzero_m\":";
    if (evidence.nonzero_depth_values == 0U) {
        output << "null";
    } else {
        output << evidence.minimum_nonzero_depth_m;
    }
    output << ",\"maximum_nonzero_m\":";
    if (evidence.nonzero_depth_values == 0U) {
        output << "null";
    } else {
        output << evidence.maximum_nonzero_depth_m;
    }
    output << "},\"points\":{\"count\":" << evidence.points.count
           << ",\"first_index\":" << evidence.points.first_index
           << ",\"last_index\":" << evidence.points.last_index
           << ",\"first_capture_ns\":" << evidence.points.first_capture_ns
           << ",\"last_capture_ns\":" << evidence.points.last_capture_ns
           << ",\"unit_scale_m\":1.0,\"finite_nonzero_points\":" << evidence.finite_point_values
           << ",\"zero_invalid_points\":" << evidence.zero_invalid_points << "}}";
    return output.str();
}

[[nodiscard]] int run_probe(const Arguments& arguments) {
    const auto options = make_options(arguments);
    DeviceGuard guard { rbt_orbbec::open_device(options) };
    if (!guard.device) {
        throw std::runtime_error("SDK returned no capture device");
    }

    const auto& calibration = guard.device->calibration();
    const VideoCalibration* depth = nullptr;
    const VideoCalibration* color = nullptr;
    validate_calibrations(calibration, arguments, depth, color);

    FrameEvidence evidence;
    std::map<Kind, std::pair<std::uint64_t, std::uint64_t>> last_timestamps;
    const auto deadline = std::chrono::steady_clock::now() + kFrameDeadline;
    while (evidence.depth.count < arguments.count || evidence.points.count < arguments.count) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            throw std::runtime_error("depth and points did not both reach the requested count within 10 seconds");
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        const auto wait = std::min(std::chrono::milliseconds(100),
                                   std::max(std::chrono::milliseconds(1), remaining));
        const auto batch = guard.device->poll(wait);
        const auto batch_depth = std::find_if(batch.begin(), batch.end(), [](const Frame& frame) {
            return frame.kind == Kind::Depth;
        });
        for (const auto& frame : batch) {
            if (frame.kind != Kind::Depth && frame.kind != Kind::Points) {
                continue;
            }
            auto& timeline = frame.kind == Kind::Depth ? evidence.depth : evidence.points;
            if (timeline.count >= arguments.count) {
                continue;
            }
            const auto previous = last_timestamps.find(frame.kind);
            if (previous != last_timestamps.end()
                && (frame.index < previous->second.first || frame.capture_us < previous->second.second)) {
                throw std::runtime_error(std::string(rbt_orbbec::kind_name(frame.kind))
                                         + " source frame index or capture timestamp regressed");
            }
            last_timestamps[frame.kind] = { frame.index, frame.capture_us };
            validate_timeline(frame, timeline);
            if (frame.kind == Kind::Depth) {
                inspect_depth(frame, *depth, options.align_depth_to_color, evidence);
            } else {
                if (batch_depth == batch.end()) {
                    throw std::runtime_error("points arrived without their processed depth frame");
                }
                inspect_points(frame, *batch_depth, evidence);
            }
        }
    }
    if (evidence.nonzero_depth_values == 0U || evidence.finite_point_values == 0U) {
        throw std::runtime_error("hardware frames contained no valid nonzero depth or points");
    }

    guard.device->stop();
    std::cout << std::setprecision(std::numeric_limits<double>::max_digits10)
              << "{\"status\":\"PASS\",\"mode\":\"REAL_HARDWARE\",\"serial\":"
              << rbt_orbbec::json_quote(arguments.serial)
              << ",\"processing_mode\":" << rbt_orbbec::json_quote(arguments.mode)
              << ",\"requested_count\":" << arguments.count
              << ",\"align_depth_to_color\":"
              << ((arguments.mode == "align" || arguments.mode == "both") ? "true" : "false")
              << ",\"calibration\":{\"color\":" << calibration_json(*color)
              << ",\"depth\":" << calibration_json(*depth) << "},\"frames\":"
              << evidence_json(evidence) << "}" << std::endl;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string requested_serial;
    std::string requested_mode;
    for (int index = 1; index + 1 < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--serial") {
            requested_serial = argv[index + 1];
        } else if (option == "--mode") {
            requested_mode = argv[index + 1];
        }
    }
    try {
        const auto arguments = parse_arguments(argc, argv);
        return run_probe(arguments);
    } catch (const std::exception& error) {
        std::cerr << "hardware processing probe: " << error.what() << '\n';
        std::cout << "{\"status\":\"FAIL\",\"mode\":\"REAL_HARDWARE\",\"serial\":"
                  << rbt_orbbec::json_quote(requested_serial)
                  << ",\"processing_mode\":" << rbt_orbbec::json_quote(requested_mode)
                  << ",\"error\":" << rbt_orbbec::json_quote(error.what()) << "}" << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "hardware processing probe: unexpected failure\n";
        std::cout << "{\"status\":\"FAIL\",\"mode\":\"REAL_HARDWARE\",\"error\":"
                  << rbt_orbbec::json_quote("unexpected failure") << "}" << std::endl;
        return 1;
    }
}

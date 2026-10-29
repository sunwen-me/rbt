#include <rbt_m3t/frame_source.hpp>

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using rbt::time::ClockIdentity;
using rbt::time::ClockKind;
using rbt::time::ClockSourceId;
using rbt::time::SensorTiming;
using rbt::time::Timestamp;
using rbt_m3t::ImageSample;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::ostringstream message;
        message << "line " << line << ": check failed: " << expression;
        throw std::runtime_error(message.str());
    }
}

template <typename Exception, typename Function>
void check_throws(Function&& function, const char* expression, int line) {
    try {
        function();
    } catch (const Exception&) {
        return;
    }
    check(false, expression, line);
}

#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)
#define CHECK_EQ(lhs, rhs) CHECK((lhs) == (rhs))
#define CHECK_THROW(statement, exception) \
    check_throws<exception>([&] { statement; }, #statement, __LINE__)
#define CHECK_NO_THROW(expression) static_cast<void>(expression)

[[nodiscard]] ClockIdentity clock_identity(std::uint8_t seed,
                                           std::uint64_t generation = 0U,
                                           ClockKind kind = ClockKind::Device) {
    std::array<std::uint8_t, 48> bytes {};
    bytes[0] = seed;
    if (kind == ClockKind::HostMonotonic) {
        bytes[8] = 1U;
    }
    auto result = ClockIdentity::create(kind, ClockSourceId(std::move(bytes)), generation);
    CHECK(result.has_value());
    return std::move(result).value();
}

[[nodiscard]] SensorTiming timing(const ClockIdentity& clock,
                                 std::int64_t nanoseconds,
                                 std::optional<std::uint64_t> capture_uncertainty = {}) {
    auto capture = Timestamp::create(clock, nanoseconds);
    auto received = Timestamp::create(clock, nanoseconds + 1'000);
    CHECK(capture.has_value());
    CHECK(received.has_value());
    auto result = SensorTiming::create(std::move(capture).value(),
                                       std::move(received).value(),
                                       capture_uncertainty,
                                       {});
    CHECK(result.has_value());
    return std::move(result).value();
}

[[nodiscard]] std::vector<std::uint8_t> stamped_payload(
    const ClockIdentity& clock,
    std::int64_t nanoseconds,
    rbt_orbbec::Kind kind,
    std::uint32_t width,
    std::uint32_t height,
    std::uint32_t stride,
    std::uint64_t index,
    double unit_scale,
    std::span<const std::uint8_t> pixels,
    std::string_view frame_id = "cam_color_optical") {
    const auto envelope_size = rbt_orbbec::kFrameHeaderBytes + pixels.size();
    std::vector<std::uint8_t> payload(rbt::time::kStampedBytesPrefixBytes + envelope_size);
    auto app = std::span<std::uint8_t>(payload).subspan(rbt::time::kStampedBytesPrefixBytes);
    const auto format = kind == rbt_orbbec::Kind::Color
                            ? rbt_orbbec::Format::Rgb8
                            : rbt_orbbec::Format::Depth16;
    const rbt_orbbec::Frame frame { kind, format, width, height, stride, index, 0U,
                                    unit_scale, pixels, {} };
    rbt_orbbec::encode_frame(app, frame, frame_id);
    const auto prefix_error = rbt::time::write_timing_prefix(
        std::span<std::uint8_t>(payload).first(rbt::time::kStampedBytesPrefixBytes),
        timing(clock, nanoseconds));
    CHECK_EQ(prefix_error, rbt::time::PrefixCodecError::None);
    return payload;
}

[[nodiscard]] ImageSample sample(rbt_orbbec::Kind kind,
                                 std::int64_t nanoseconds,
                                 std::uint64_t index,
                                 std::uint8_t clock_seed = 1U,
                                 std::uint64_t generation = 0U,
                                 std::string frame_id = "cam_color_optical") {
    const bool is_color = kind == rbt_orbbec::Kind::Color;
    cv::Mat pixels(3, 3, is_color ? CV_8UC3 : CV_16UC1, cv::Scalar::all(0));
    return ImageSample { kind, timing(clock_identity(clock_seed, generation), nanoseconds),
                         index, index, std::move(frame_id), is_color ? 1.0 : 0.002,
                         std::move(pixels) };
}

[[nodiscard]] rbt::orbbec::CameraInfoResponse camera_info(rbt_orbbec::Kind kind) {
    rbt::orbbec::CameraInfoResponse result {};
    const bool is_color = kind == rbt_orbbec::Kind::Color;
    result.status = 0U;
    result.stream = static_cast<std::uint32_t>(kind);
    result.width = 3U;
    result.height = 3U;
    result.format = static_cast<std::uint32_t>(
        is_color ? rbt_orbbec::Format::Rgb8 : rbt_orbbec::Format::Depth16);
    result.fx = 4.0;
    result.fy = 4.0;
    result.cx = 1.0;
    result.cy = 1.0;
    result.distortion_model = 0U;
    constexpr char serial[] = "SN-123";
    constexpr char frame[] = "cam_color_optical";
    std::copy(std::begin(serial), std::end(serial), result.serial);
    std::copy(std::begin(frame), std::end(frame), result.frame_id);
    return result;
}

[[nodiscard]] rbt_m3t::RgbdPair rgbd_pair() {
    auto color = sample(rbt_orbbec::Kind::Color, 10'000, 1U);
    auto depth = sample(rbt_orbbec::Kind::Depth, 10'005, 2U);
    color.pixels.at<cv::Vec3b>(1, 1) = cv::Vec3b { 10U, 20U, 30U };
    depth.pixels.at<std::uint16_t>(1, 1) = 0U;
    depth.pixels.at<std::uint16_t>(1, 0) = 777U;
    return { std::move(color), std::move(depth), 5U };
}

void copies_strided_pixels_and_owns_them_after_payload_changes() {
    const auto clock = clock_identity(4U);
    const std::array<std::uint8_t, 16> rgb {
        1U, 2U, 3U, 4U, 5U, 6U, 0xEEU, 0xEEU,
        7U, 8U, 9U, 10U, 11U, 12U, 0xDDU, 0xDDU,
    };
    auto payload = stamped_payload(clock, 500, rbt_orbbec::Kind::Color,
                                   2U, 2U, 8U, 11U, 1.0, rgb);
    auto copied = rbt_m3t::copy_image_sample(7U, payload);
    std::fill(payload.begin(), payload.end(), 0U);

    CHECK_EQ(copied.sequence, 7U);
    CHECK_EQ(copied.device_index, 11U);
    CHECK_EQ(copied.pixels.type(), CV_8UC3);
    CHECK_EQ(copied.pixels.at<cv::Vec3b>(0, 0), (cv::Vec3b { 1U, 2U, 3U }));
    CHECK_EQ(copied.pixels.at<cv::Vec3b>(0, 1), (cv::Vec3b { 4U, 5U, 6U }));
    CHECK_EQ(copied.pixels.at<cv::Vec3b>(1, 0), (cv::Vec3b { 7U, 8U, 9U }));
    CHECK_EQ(copied.pixels.at<cv::Vec3b>(1, 1), (cv::Vec3b { 10U, 11U, 12U }));
}

void requires_device_capture_timestamp() {
    const std::array<std::uint8_t, 3> rgb { 1U, 2U, 3U };
    auto payload = stamped_payload(clock_identity(8U, 0U, ClockKind::HostMonotonic),
                                   100, rbt_orbbec::Kind::Color, 1U, 1U, 3U,
                                   0U, 1.0, rgb);
    CHECK_THROW((void)rbt_m3t::copy_image_sample(0U, payload), std::invalid_argument);
    const std::array<std::uint8_t, 8> malformed {};
    CHECK_THROW((void)rbt_m3t::copy_image_sample(0U, malformed), std::invalid_argument);
}

void copies_depth_rows_using_stride_and_retains_metres_per_unit() {
    const auto clock = clock_identity(5U);
    const std::array<std::uint8_t, 12> depth {
        0x34U, 0x12U, 0x00U, 0x00U, 0xEEU, 0xEEU,
        0xCDU, 0xABU, 0x02U, 0x01U, 0xDDU, 0xDDU,
    };
    const auto payload = stamped_payload(clock, 600, rbt_orbbec::Kind::Depth,
                                         2U, 2U, 6U, 12U, 0.003, depth);
    const auto copied = rbt_m3t::copy_image_sample(8U, payload);
    CHECK_EQ(copied.pixels.type(), CV_16UC1);
    CHECK_EQ(copied.pixels.at<std::uint16_t>(0, 0), 0x1234U);
    CHECK_EQ(copied.pixels.at<std::uint16_t>(0, 1), 0U);
    CHECK_EQ(copied.pixels.at<std::uint16_t>(1, 0), 0xABCDU);
    CHECK_EQ(copied.pixels.at<std::uint16_t>(1, 1), 0x0102U);
    CHECK_EQ(copied.unit_scale, 0.003);
}

void pairs_greedily_drop_skewed_frames_and_bound_queues() {
    rbt_m3t::PairBuffer skew_buffer(20U, 4U);
    skew_buffer.push(sample(rbt_orbbec::Kind::Color, 100, 0U));
    skew_buffer.push(sample(rbt_orbbec::Kind::Depth, 130, 0U));
    CHECK(!skew_buffer.take().has_value());
    CHECK_EQ(skew_buffer.dropped(), 1U);
    skew_buffer.push(sample(rbt_orbbec::Kind::Color, 135, 1U));
    auto pair = skew_buffer.take();
    CHECK(pair.has_value());
    CHECK_EQ(pair->skew_ns, 5U);
    CHECK_EQ(pair->color.device_index, 1U);
    CHECK_EQ(pair->depth.device_index, 0U);

    rbt_m3t::PairBuffer bounded_buffer(20U, 2U);
    bounded_buffer.push(sample(rbt_orbbec::Kind::Color, 201, 0U));
    bounded_buffer.push(sample(rbt_orbbec::Kind::Color, 202, 1U));
    bounded_buffer.push(sample(rbt_orbbec::Kind::Color, 203, 2U));
    CHECK_EQ(bounded_buffer.dropped(), 1U);
    bounded_buffer.push(sample(rbt_orbbec::Kind::Depth, 203, 0U));
    auto bounded_pair = bounded_buffer.take();
    CHECK(bounded_pair.has_value());
    CHECK_EQ(bounded_pair->color.device_index, 1U);
}

void pair_buffer_rejects_invalid_limits_and_zero_capture_time() {
    CHECK_THROW((void)rbt_m3t::PairBuffer(20U, 0U), std::invalid_argument);
    CHECK_THROW((void)rbt_m3t::PairBuffer(20U, 17U), std::invalid_argument);
    CHECK_THROW((void)rbt_m3t::PairBuffer(100'000'001U, 4U), std::invalid_argument);

    rbt_m3t::PairBuffer exact_buffer(0U, 16U);
    exact_buffer.push(sample(rbt_orbbec::Kind::Color, 250, 0U));
    exact_buffer.push(sample(rbt_orbbec::Kind::Depth, 250, 0U));
    const auto exact_pair = exact_buffer.take();
    CHECK(exact_pair.has_value());
    CHECK_EQ(exact_pair->skew_ns, 0U);

    rbt_m3t::PairBuffer buffer;
    CHECK_THROW(buffer.push(sample(rbt_orbbec::Kind::Color, 0, 0U)), std::invalid_argument);
}

void reject_mixed_clocks_generations_and_nonmonotonic_streams() {
    rbt_m3t::PairBuffer buffer;
    buffer.push(sample(rbt_orbbec::Kind::Color, 10, 5U));
    CHECK_THROW(buffer.push(sample(rbt_orbbec::Kind::Depth, 11, 0U, 2U)),
                std::invalid_argument);
    CHECK_THROW(buffer.push(sample(rbt_orbbec::Kind::Color, 11, 6U, 1U, 1U)),
                std::invalid_argument);
    CHECK_THROW(buffer.push(sample(rbt_orbbec::Kind::Color, 9, 6U)),
                std::invalid_argument);
    CHECK_THROW(buffer.push(sample(rbt_orbbec::Kind::Color, 11, 5U)),
                std::invalid_argument);
}

void validate_calibration_and_distortion_models() {
    const auto color = camera_info(rbt_orbbec::Kind::Color);
    const auto depth = camera_info(rbt_orbbec::Kind::Depth);
    CHECK_NO_THROW((rbt_m3t::Rectifier(color, depth)));
    auto brown3_color = color;
    auto brown3_depth = depth;
    brown3_color.distortion_model = 3U;
    brown3_depth.distortion_model = 3U;
    CHECK_NO_THROW((rbt_m3t::Rectifier(brown3_color, brown3_depth)));

    auto invalid_info = color;
    invalid_info.status = 1U;
    CHECK_THROW((rbt_m3t::Rectifier(invalid_info, depth)), std::invalid_argument);
    invalid_info = color;
    invalid_info.format = static_cast<std::uint32_t>(rbt_orbbec::Format::Mono8);
    CHECK_THROW((rbt_m3t::Rectifier(invalid_info, depth)), std::invalid_argument);
    invalid_info = depth;
    invalid_info.width += 1U;
    CHECK_THROW((rbt_m3t::Rectifier(color, invalid_info)), std::invalid_argument);
    invalid_info = depth;
    invalid_info.fx += 0.5;
    CHECK_THROW((rbt_m3t::Rectifier(color, invalid_info)), std::invalid_argument);
    invalid_info = depth;
    invalid_info.frame_id[0] = 'x';
    CHECK_THROW((rbt_m3t::Rectifier(color, invalid_info)), std::invalid_argument);
    invalid_info = depth;
    invalid_info.serial[0] = 'x';
    CHECK_THROW((rbt_m3t::Rectifier(color, invalid_info)), std::invalid_argument);
    invalid_info = color;
    invalid_info.distortion_model = 2U;
    auto paired_depth = depth;
    paired_depth.distortion_model = 2U;
    CHECK_THROW((rbt_m3t::Rectifier(invalid_info, paired_depth)), std::invalid_argument);

    invalid_info = color;
    invalid_info.fx = 1.0e-300;
    invalid_info.fy = 1.0e-300;
    paired_depth = depth;
    paired_depth.fx = invalid_info.fx;
    paired_depth.fy = invalid_info.fy;
    CHECK_THROW((rbt_m3t::Rectifier(invalid_info, paired_depth)), std::invalid_argument);
}

void rectify_once_preserving_scale_timestamps_and_unknown_uncertainty() {
    auto color_info = camera_info(rbt_orbbec::Kind::Color);
    auto depth_info = camera_info(rbt_orbbec::Kind::Depth);
    color_info.width = depth_info.width = 5U;
    color_info.height = depth_info.height = 5U;
    color_info.fx = depth_info.fx = 2.0;
    color_info.fy = depth_info.fy = 2.0;
    color_info.cx = depth_info.cx = 2.0;
    color_info.cy = depth_info.cy = 2.0;
    color_info.distortion_model = 4U;
    depth_info.distortion_model = 4U;
    const std::array<double, 8> sdk_distortion { 0.1, 0.02, 0.01, 0.03,
                                                  0.04, 0.05, 0.03, -0.02 };
    std::copy(sdk_distortion.begin(), sdk_distortion.end(), color_info.distortion);
    std::copy(sdk_distortion.begin(), sdk_distortion.end(), depth_info.distortion);
    rbt_m3t::Rectifier rectifier(color_info, depth_info);
    auto pair = rgbd_pair();
    pair.color.pixels = cv::Mat(5, 5, CV_8UC3);
    pair.depth.pixels = cv::Mat(5, 5, CV_16UC1);
    for (int row = 0; row < 5; ++row) {
        for (int column = 0; column < 5; ++column) {
            pair.color.pixels.at<cv::Vec3b>(row, column) = cv::Vec3b {
                static_cast<std::uint8_t>(row * 40 + column * 10),
                static_cast<std::uint8_t>(row * 20 + column * 5),
                static_cast<std::uint8_t>(row * 10 + column * 2),
            };
            pair.depth.pixels.at<std::uint16_t>(row, column) =
                static_cast<std::uint16_t>(100 + row * 5 + column);
        }
    }
    pair.depth.pixels.at<std::uint16_t>(2, 2) = 0U;
    auto rectified = rectifier.apply(pair);

    auto underflow_scale_pair = pair;
    underflow_scale_pair.depth.unit_scale = std::numeric_limits<double>::denorm_min();
    CHECK_THROW((void)rectifier.apply(underflow_scale_pair), std::invalid_argument);

    const cv::Mat camera_matrix = (cv::Mat_<double>(3, 3)
        << 2.0, 0.0, 2.0,
           0.0, 2.0, 2.0,
           0.0, 0.0, 1.0);
    const cv::Mat opencv_distortion = (cv::Mat_<double>(1, 8)
        << 0.1, 0.02, 0.03, -0.02, 0.01, 0.03, 0.04, 0.05);
    cv::Mat map_x;
    cv::Mat map_y;
    cv::initUndistortRectifyMap(camera_matrix, opencv_distortion,
                                cv::Mat::eye(3, 3, CV_64F), camera_matrix,
                                cv::Size(5, 5), CV_32FC1, map_x, map_y);
    cv::Mat expected_rgb;
    cv::Mat expected_depth;
    cv::Mat expected_bgr;
    cv::remap(pair.color.pixels, expected_rgb, map_x, map_y, cv::INTER_LINEAR,
              cv::BORDER_CONSTANT, cv::Scalar::all(0));
    cv::remap(pair.depth.pixels, expected_depth, map_x, map_y, cv::INTER_NEAREST,
              cv::BORDER_CONSTANT, cv::Scalar::all(0));
    cv::cvtColor(expected_rgb, expected_bgr, cv::COLOR_RGB2BGR);

    CHECK_EQ(rectified.bgr.type(), CV_8UC3);
    CHECK_EQ(rectified.depth_u16.type(), CV_16UC1);
    CHECK_EQ(rectified.bgr.size(), (cv::Size { 5, 5 }));
    CHECK_EQ(cv::norm(expected_bgr, rectified.bgr, cv::NORM_INF), 0.0);
    CHECK_EQ(cv::norm(expected_depth, rectified.depth_u16, cv::NORM_INF), 0.0);
    CHECK_EQ(rectified.depth_u16.at<std::uint16_t>(2, 2), 0U);
    CHECK_EQ(rectified.depth_scale, 0.002);
    CHECK(!rectified.timing.capture_uncertainty_ns.has_value());
    CHECK(rectified.timing.capture.has_value());
    CHECK_EQ(rectified.timing.capture->ns, 10'005);
    CHECK_EQ(rectified.color_capture.ns, 10'000);
    CHECK_EQ(rectified.frame_id, "cam_color_optical");
}

void both_cameras_read_the_caller_selected_shared_pair() {
    auto images = std::make_shared<rbt_m3t::CameraImages>();
    auto pair = rgbd_pair();
    rbt_m3t::Rectifier rectifier(camera_info(rbt_orbbec::Kind::Color),
                                 camera_info(rbt_orbbec::Kind::Depth));
    images->current = rectifier.apply(pair);
    rbt_m3t::RbtColorCamera color("rgb", images);
    rbt_m3t::RbtDepthCamera depth("z", images);

    CHECK(color.SetUp());
    CHECK(depth.SetUp());
    CHECK_EQ(color.image().at<cv::Vec3b>(1, 1), (cv::Vec3b { 30U, 20U, 10U }));
    CHECK_EQ(depth.image().at<std::uint16_t>(1, 0), 777U);
    CHECK(std::abs(depth.depth_scale() - 0.002F) < 1.0e-6F);
    CHECK(std::abs(color.intrinsics().fu - depth.intrinsics().fu) < 1.0e-6F);
    CHECK(color.camera2world_pose().matrix().isIdentity());
    CHECK(depth.camera2world_pose().matrix().isIdentity());

    const Eigen::Affine3f first_camera_pose =
        Eigen::Translation3f { 0.4F, -0.2F, 0.7F }
        * Eigen::AngleAxisf { 0.3F, Eigen::Vector3f::UnitY() };
    images->camera_in_world = first_camera_pose;
    CHECK(color.UpdateImage(false));
    CHECK(depth.UpdateImage(false));
    CHECK(color.camera2world_pose().matrix().isApprox(first_camera_pose.matrix(), 1.0e-6F));
    CHECK(depth.camera2world_pose().matrix().isApprox(first_camera_pose.matrix(), 1.0e-6F));

    Eigen::Affine3f invalid_camera_pose = Eigen::Affine3f::Identity();
    invalid_camera_pose.linear()(0, 0) = 2.0F;
    images->camera_in_world = invalid_camera_pose;
    CHECK(!color.UpdateImage(false));
    CHECK(!depth.UpdateImage(false));
    CHECK(!color.SetUp());
    CHECK(!depth.SetUp());
    CHECK(color.camera2world_pose().matrix().isApprox(first_camera_pose.matrix(), 1.0e-6F));
    CHECK(depth.camera2world_pose().matrix().isApprox(first_camera_pose.matrix(), 1.0e-6F));
    images->camera_in_world = first_camera_pose;

    images->current->depth_scale = 0.003;
    CHECK(!depth.UpdateImage(false));
    CHECK(depth.SetUp());
    CHECK(std::abs(depth.depth_scale() - 0.003F) < 1.0e-6F);

    images->current->intrinsics.fu += 1.0F;
    CHECK(!color.UpdateImage(false));
    CHECK(!depth.UpdateImage(false));
    CHECK(color.SetUp());
    CHECK(depth.SetUp());
    CHECK(color.camera2world_pose().matrix().isApprox(first_camera_pose.matrix(), 1.0e-6F));
    CHECK(depth.camera2world_pose().matrix().isApprox(first_camera_pose.matrix(), 1.0e-6F));
    CHECK_EQ(color.intrinsics().fu, images->current->intrinsics.fu);
    CHECK_EQ(depth.intrinsics().fu, images->current->intrinsics.fu);
}

} // namespace

int main() {
    const std::array tests {
        copies_strided_pixels_and_owns_them_after_payload_changes,
        requires_device_capture_timestamp,
        copies_depth_rows_using_stride_and_retains_metres_per_unit,
        pairs_greedily_drop_skewed_frames_and_bound_queues,
        pair_buffer_rejects_invalid_limits_and_zero_capture_time,
        reject_mixed_clocks_generations_and_nonmonotonic_streams,
        validate_calibration_and_distortion_models,
        rectify_once_preserving_scale_timestamps_and_unknown_uncertainty,
        both_cameras_read_the_caller_selected_shared_pair,
    };
    for (const auto test : tests) {
        try {
            test();
        } catch (const std::exception& error) {
            std::cerr << "frame_source_test failed: " << error.what() << '\n';
            return 1;
        }
    }
    return 0;
}

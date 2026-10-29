#include <rbt_m3t/frame_source.hpp>

#include <opencv2/calib3d.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace rbt_m3t {
namespace {

[[noreturn]] void invalid(const char* message) {
    throw std::invalid_argument(message);
}

[[nodiscard]] std::uint64_t timestamp_distance(std::int64_t lhs,
                                               std::int64_t rhs) noexcept {
    if (lhs >= rhs) {
        return static_cast<std::uint64_t>(lhs) - static_cast<std::uint64_t>(rhs);
    }
    return static_cast<std::uint64_t>(rhs) - static_cast<std::uint64_t>(lhs);
}

[[nodiscard]] std::int64_t capture_ns(const ImageSample& sample) {
    if (!sample.timing.capture.has_value() || sample.timing.capture->ns <= 0) {
        invalid("M3T image samples require a positive capture timestamp");
    }
    return sample.timing.capture->ns;
}

[[nodiscard]] bool m3t_depth_scale(double value) noexcept {
    if (!std::isfinite(value) || value <= 0.0) {
        return false;
    }
    const auto scale = static_cast<float>(value);
    return std::isfinite(scale) && scale > 0.0F;
}

[[nodiscard]] bool same_intrinsics(const m3t::Intrinsics& lhs,
                                   const m3t::Intrinsics& rhs) noexcept {
    return lhs.fu == rhs.fu && lhs.fv == rhs.fv && lhs.ppu == rhs.ppu
           && lhs.ppv == rhs.ppv && lhs.width == rhs.width && lhs.height == rhs.height;
}

[[nodiscard]] bool valid_intrinsics(const m3t::Intrinsics& intrinsics,
                                    cv::Size image_size) noexcept {
    return std::isfinite(intrinsics.fu) && intrinsics.fu > 0.0F
           && std::isfinite(intrinsics.fv) && intrinsics.fv > 0.0F
           && std::isfinite(intrinsics.ppu) && std::isfinite(intrinsics.ppv)
           && intrinsics.width == image_size.width && intrinsics.height == image_size.height;
}

[[nodiscard]] bool valid_camera_pose(const Eigen::Affine3f& pose) noexcept {
    constexpr float tolerance = 1.0e-4F;
    const auto matrix = pose.matrix();
    if (!matrix.array().isFinite().all()
        || (matrix.row(3) - Eigen::RowVector4f { 0.0F, 0.0F, 0.0F, 1.0F })
                   .cwiseAbs()
                   .maxCoeff()
               > tolerance) {
        return false;
    }
    const Eigen::Matrix3f rotation = matrix.topLeftCorner<3, 3>();
    return (rotation.transpose() * rotation - Eigen::Matrix3f::Identity())
                   .cwiseAbs()
                   .maxCoeff()
               <= tolerance
           && std::abs(rotation.determinant() - 1.0F) <= tolerance;
}

template <typename Camera>
void set_camera_world_pose(Camera& camera, const Eigen::Affine3f& pose) {
    m3t::Transform3fA transform = m3t::Transform3fA::Identity();
    transform.matrix() = pose.matrix();
    camera.set_camera2world_pose(transform);
}

[[nodiscard]] std::size_t bytes_per_pixel(rbt_orbbec::Format format) {
    switch (format) {
    case rbt_orbbec::Format::Rgb8:
        return 3U;
    case rbt_orbbec::Format::Depth16:
        return 2U;
    default:
        invalid("M3T accepts only RGB8 color and U16 depth frames");
    }
}

template <std::size_t N>
[[nodiscard]] std::string fixed_text(const std::uint8_t (&bytes)[N],
                                    const char* field) {
    const auto terminator = std::find(std::begin(bytes), std::end(bytes), 0U);
    if (terminator == std::begin(bytes) || terminator == std::end(bytes)) {
        throw std::invalid_argument(std::string("invalid empty or unterminated camera ") + field);
    }
    if (std::any_of(terminator, std::end(bytes), [](std::uint8_t byte) { return byte != 0U; })) {
        throw std::invalid_argument(std::string("camera ") + field + " is not zero padded");
    }
    return std::string(reinterpret_cast<const char*>(bytes),
                       static_cast<std::size_t>(terminator - std::begin(bytes)));
}

struct CalibrationInfo {
    m3t::Intrinsics intrinsics;
    cv::Mat camera_matrix;
    cv::Mat distortion;
    std::string serial;
    std::string frame_id;
};

[[nodiscard]] CalibrationInfo validate_calibration(
    const rbt::orbbec::CameraInfoResponse& color,
    const rbt::orbbec::CameraInfoResponse& depth) {
    using Format = rbt_orbbec::Format;
    if (color.status != 0U || depth.status != 0U) {
        invalid("M3T camera calibration status must be successful");
    }
    if (color.stream != static_cast<std::uint32_t>(rbt_orbbec::Kind::Color)
        || depth.stream != static_cast<std::uint32_t>(rbt_orbbec::Kind::Depth)
        || color.format != static_cast<std::uint32_t>(Format::Rgb8)
        || depth.format != static_cast<std::uint32_t>(Format::Depth16)) {
        invalid("M3T requires color RGB8 and depth U16 camera calibration");
    }
    if (color.width == 0U || color.height == 0U || color.width > 4096U
        || color.height > 4096U || color.width != depth.width
        || color.height != depth.height
        || color.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max())
        || color.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        invalid("M3T color and depth calibration dimensions must match");
    }

    const auto color_serial = fixed_text(color.serial, "serial");
    const auto depth_serial = fixed_text(depth.serial, "serial");
    const auto color_frame = fixed_text(color.frame_id, "frame id");
    const auto depth_frame = fixed_text(depth.frame_id, "frame id");
    if (color_serial != depth_serial || color_frame != depth_frame) {
        invalid("M3T color and depth must share a serial and optical frame");
    }

    if (color.fx != depth.fx || color.fy != depth.fy || color.cx != depth.cx
        || color.cy != depth.cy || color.distortion_model != depth.distortion_model
        || !std::equal(std::begin(color.distortion), std::end(color.distortion),
                       std::begin(depth.distortion))) {
        invalid("M3T color and depth calibration must have identical intrinsics and distortion");
    }
    if (!std::isfinite(color.fx) || color.fx <= 0.0 || !std::isfinite(color.fy)
        || color.fy <= 0.0 || !std::isfinite(color.cx) || !std::isfinite(color.cy)
        || !std::all_of(std::begin(color.distortion), std::end(color.distortion),
                        [](double value) { return std::isfinite(value); })) {
        invalid("M3T calibration values must be finite with positive focal lengths");
    }
    const auto float_representable = [](double value) {
        return std::abs(value) <= static_cast<double>(std::numeric_limits<float>::max())
               && std::isfinite(static_cast<float>(value));
    };
    if (!float_representable(color.fx) || static_cast<float>(color.fx) <= 0.0F
        || !float_representable(color.fy) || static_cast<float>(color.fy) <= 0.0F
        || !float_representable(color.cx) || !float_representable(color.cy)) {
        invalid("M3T camera intrinsics must fit the native float representation");
    }

    cv::Mat distortion;
    switch (color.distortion_model) {
    case 0U: // OB_DISTORTION_NONE
        distortion = cv::Mat::zeros(1, 5, CV_64F);
        break;
    case 3U: // OB_DISTORTION_BROWN_CONRADY
        distortion = (cv::Mat_<double>(1, 5) << color.distortion[0], color.distortion[1],
                      color.distortion[6], color.distortion[7], color.distortion[2]);
        break;
    case 4U: // OB_DISTORTION_BROWN_CONRADY_K6
        distortion = (cv::Mat_<double>(1, 8) << color.distortion[0], color.distortion[1],
                      color.distortion[6], color.distortion[7], color.distortion[2],
                      color.distortion[3], color.distortion[4], color.distortion[5]);
        break;
    default:
        invalid("M3T does not support this native camera distortion model");
    }

    cv::Mat camera_matrix = (cv::Mat_<double>(3, 3)
        << color.fx, 0.0, color.cx,
           0.0, color.fy, color.cy,
           0.0, 0.0, 1.0);
    return CalibrationInfo {
        m3t::Intrinsics { static_cast<float>(color.fx), static_cast<float>(color.fy),
                          static_cast<float>(color.cx), static_cast<float>(color.cy),
                          static_cast<int>(color.width), static_cast<int>(color.height) },
        std::move(camera_matrix), std::move(distortion), color_serial, color_frame
    };
}

[[nodiscard]] bool valid_pair_image(const ImageSample& sample,
                                    rbt_orbbec::Kind kind,
                                    int type) noexcept {
    return sample.kind == kind && !sample.pixels.empty() && sample.pixels.type() == type
           && sample.timing.capture.has_value()
           && sample.timing.capture->clock.kind() == rbt::time::ClockKind::Device;
}

void copy_rows(cv::Mat& destination,
               std::span<const std::uint8_t> source,
               std::size_t stride,
               std::size_t row_bytes) {
    for (int row = 0; row < destination.rows; ++row) {
        std::memcpy(destination.ptr(row),
                    source.data() + static_cast<std::size_t>(row) * stride,
                    row_bytes);
    }
}

} // namespace

ImageSample copy_image_sample(std::uint64_t sequence,
                              std::span<const std::uint8_t> payload) {
    const auto stamped = rbt::time::decode_stamped_bytes(payload);
    const auto* decoded_timing = std::get_if<rbt::time::DecodedStampedBytes>(&stamped);
    if (decoded_timing == nullptr || !decoded_timing->timing.capture.has_value()
        || decoded_timing->timing.capture->clock.kind() != rbt::time::ClockKind::Device
        || decoded_timing->timing.capture->ns <= 0) {
        invalid("M3T requires a valid Device capture timestamp");
    }

    const auto frame = rbt_orbbec::decode_frame(decoded_timing->bytes);
    if (frame.kind != rbt_orbbec::Kind::Color && frame.kind != rbt_orbbec::Kind::Depth) {
        invalid("M3T accepts only color and depth frames");
    }
    const auto expected_format = frame.kind == rbt_orbbec::Kind::Color
                                     ? rbt_orbbec::Format::Rgb8
                                     : rbt_orbbec::Format::Depth16;
    if (frame.format != expected_format) {
        invalid("M3T frame kind and format do not match RGB-D input");
    }

    const auto pixel_size = bytes_per_pixel(frame.format);
    const auto row_bytes = static_cast<std::size_t>(frame.width) * pixel_size;
    const auto type = frame.kind == rbt_orbbec::Kind::Color ? CV_8UC3 : CV_16UC1;
    cv::Mat pixels(static_cast<int>(frame.height), static_cast<int>(frame.width), type);
    copy_rows(pixels, frame.data, frame.stride, row_bytes);

    return ImageSample { frame.kind, decoded_timing->timing, sequence, frame.index,
                         frame.frame_id, frame.unit_scale, std::move(pixels) };
}

PairBuffer::PairBuffer(std::uint64_t maximum_skew_ns, std::size_t capacity)
    : maximum_skew_ns_{maximum_skew_ns}, capacity_{capacity} {
    if (capacity_ == 0U || capacity_ > 16U) {
        invalid("RGB-D pair buffer capacity must be between one and sixteen");
    }
    if (maximum_skew_ns_ > 100'000'000U) {
        invalid("RGB-D maximum skew must not exceed 100 milliseconds");
    }
}

void PairBuffer::push(ImageSample sample) {
    if (sample.kind != rbt_orbbec::Kind::Color && sample.kind != rbt_orbbec::Kind::Depth) {
        invalid("RGB-D pair buffer accepts only color and depth samples");
    }
    const auto* capture = sample.timing.capture.has_value() ? &*sample.timing.capture : nullptr;
    if (capture == nullptr || capture->clock.kind() != rbt::time::ClockKind::Device) {
        invalid("RGB-D pair buffer requires Device capture timestamps");
    }
    if (capture->ns <= 0) {
        invalid("RGB-D capture timestamps must be positive");
    }
    if (sample.kind == rbt_orbbec::Kind::Color && sample.unit_scale != 1.0) {
        invalid("RGB color samples must use unit scale one");
    }
    if ((sample.kind == rbt_orbbec::Kind::Color
             && !valid_pair_image(sample, rbt_orbbec::Kind::Color, CV_8UC3))
        || (sample.kind == rbt_orbbec::Kind::Depth
            && (!valid_pair_image(sample, rbt_orbbec::Kind::Depth, CV_16UC1)
                || !std::isfinite(sample.unit_scale) || sample.unit_scale <= 0.0))) {
        invalid("RGB-D pair buffer received an invalid image sample");
    }

    if (clock_.has_value() && *clock_ != capture->clock) {
        invalid("RGB-D samples cannot mix clock sources or generations");
    }
    const bool is_color = sample.kind == rbt_orbbec::Kind::Color;
    const auto last_ns = is_color ? last_color_ns_ : last_depth_ns_;
    const auto last_index = is_color ? last_color_index_ : last_depth_index_;
    if ((last_ns.has_value() && capture->ns <= *last_ns)
        || (last_index.has_value() && sample.device_index <= *last_index)) {
        invalid("RGB-D source indices and capture timestamps must increase per stream");
    }

    clock_ = capture->clock;
    if (is_color) {
        last_color_ns_ = capture->ns;
        last_color_index_ = sample.device_index;
        color_.push_back(std::move(sample));
        if (color_.size() > capacity_) {
            color_.pop_front();
            ++dropped_;
        }
    } else {
        last_depth_ns_ = capture->ns;
        last_depth_index_ = sample.device_index;
        depth_.push_back(std::move(sample));
        if (depth_.size() > capacity_) {
            depth_.pop_front();
            ++dropped_;
        }
    }
}

std::optional<RgbdPair> PairBuffer::take() {
    while (!color_.empty() && !depth_.empty()) {
        const auto color_ns = capture_ns(color_.front());
        const auto depth_ns = capture_ns(depth_.front());
        const auto skew = timestamp_distance(color_ns, depth_ns);
        if (skew <= maximum_skew_ns_) {
            RgbdPair pair { std::move(color_.front()), std::move(depth_.front()), skew };
            color_.pop_front();
            depth_.pop_front();
            return pair;
        }
        if (color_ns < depth_ns) {
            color_.pop_front();
        } else {
            depth_.pop_front();
        }
        ++dropped_;
    }
    return {};
}

std::uint64_t PairBuffer::dropped() const noexcept {
    return dropped_;
}

Rectifier::Rectifier(const rbt::orbbec::CameraInfoResponse& color,
                     const rbt::orbbec::CameraInfoResponse& depth) {
    const auto calibration = validate_calibration(color, depth);
    intrinsics_ = calibration.intrinsics;
    frame_id_ = calibration.frame_id;
    const auto size = cv::Size { intrinsics_.width, intrinsics_.height };
    cv::initUndistortRectifyMap(calibration.camera_matrix,
                                calibration.distortion,
                                cv::Mat::eye(3, 3, CV_64F),
                                calibration.camera_matrix,
                                size,
                                CV_32FC1,
                                map_x_,
                                map_y_);
    if (map_x_.size() != size || map_y_.size() != size
        || map_x_.type() != CV_32FC1 || map_y_.type() != CV_32FC1
        || !cv::checkRange(map_x_) || !cv::checkRange(map_y_)) {
        invalid("M3T calibration produced invalid rectification maps");
    }
}

RectifiedPair Rectifier::apply(const RgbdPair& pair) const {
    if (!valid_pair_image(pair.color, rbt_orbbec::Kind::Color, CV_8UC3)
        || !valid_pair_image(pair.depth, rbt_orbbec::Kind::Depth, CV_16UC1)
        || !m3t_depth_scale(pair.depth.unit_scale)) {
        invalid("rectifier received an invalid RGB-D pair");
    }
    const auto& color_capture = *pair.color.timing.capture;
    const auto& depth_capture = *pair.depth.timing.capture;
    if (color_capture.ns <= 0 || depth_capture.ns <= 0
        || color_capture.clock != depth_capture.clock
        || color_capture.clock.kind() != rbt::time::ClockKind::Device
        || timestamp_distance(color_capture.ns, depth_capture.ns) != pair.skew_ns) {
        invalid("rectifier pair timestamps or source clock are inconsistent");
    }
    const cv::Size expected_size { intrinsics_.width, intrinsics_.height };
    if (pair.color.pixels.size() != expected_size || pair.depth.pixels.size() != expected_size
        || pair.color.frame_id != frame_id_ || pair.depth.frame_id != frame_id_
        || pair.color.unit_scale != 1.0) {
        invalid("rectifier pair does not match the shared aligned camera calibration");
    }

    cv::Mat rectified_rgb;
    cv::Mat rectified_depth;
    cv::remap(pair.color.pixels, rectified_rgb, map_x_, map_y_, cv::INTER_LINEAR,
              cv::BORDER_CONSTANT, cv::Scalar::all(0));
    cv::remap(pair.depth.pixels, rectified_depth, map_x_, map_y_, cv::INTER_NEAREST,
              cv::BORDER_CONSTANT, cv::Scalar::all(0));
    cv::Mat bgr;
    cv::cvtColor(rectified_rgb, bgr, cv::COLOR_RGB2BGR);

    return RectifiedPair { std::move(bgr), std::move(rectified_depth), intrinsics_,
                           pair.depth.unit_scale, pair.depth.timing, color_capture,
                           frame_id_, pair.skew_ns };
}

RbtColorCamera::RbtColorCamera(std::string name, std::shared_ptr<CameraImages> images)
    : ColorCamera{name}, images_{std::move(images)} {
}

bool RbtColorCamera::SetUp() {
    if (!images_ || !valid_camera_pose(images_->camera_in_world)) {
        return false;
    }
    set_up_ = false;
    if (!images_->current || images_->current->bgr.empty()
        || images_->current->bgr.type() != CV_8UC3
        || !valid_intrinsics(images_->current->intrinsics, images_->current->bgr.size())) {
        return false;
    }
    set_camera_world_pose(*this, images_->camera_in_world);
    intrinsics_ = images_->current->intrinsics;
    SaveMetaDataIfDesired();
    set_up_ = true;
    return UpdateImage(true);
}

bool RbtColorCamera::UpdateImage(bool) {
    if (!images_ || !valid_camera_pose(images_->camera_in_world)) {
        return false;
    }
    if (!set_up_ || !images_->current || images_->current->bgr.empty()
        || images_->current->bgr.type() != CV_8UC3
        || !valid_intrinsics(images_->current->intrinsics, images_->current->bgr.size())
        || !same_intrinsics(images_->current->intrinsics, intrinsics_)) {
        return false;
    }
    set_camera_world_pose(*this, images_->camera_in_world);
    image_ = images_->current->bgr;
    SaveImageIfDesired();
    return true;
}

RbtDepthCamera::RbtDepthCamera(std::string name, std::shared_ptr<CameraImages> images)
    : DepthCamera{name}, images_{std::move(images)} {
}

bool RbtDepthCamera::SetUp() {
    if (!images_ || !valid_camera_pose(images_->camera_in_world)) {
        return false;
    }
    set_up_ = false;
    if (!images_->current || images_->current->depth_u16.empty()
        || images_->current->depth_u16.type() != CV_16UC1
        || !valid_intrinsics(images_->current->intrinsics, images_->current->depth_u16.size())
        || !m3t_depth_scale(images_->current->depth_scale)) {
        return false;
    }
    set_camera_world_pose(*this, images_->camera_in_world);
    intrinsics_ = images_->current->intrinsics;
    depth_scale_ = static_cast<float>(images_->current->depth_scale);
    SaveMetaDataIfDesired();
    set_up_ = true;
    return UpdateImage(true);
}

bool RbtDepthCamera::UpdateImage(bool) {
    if (!images_ || !valid_camera_pose(images_->camera_in_world)) {
        return false;
    }
    if (!set_up_ || !images_->current || images_->current->depth_u16.empty()
        || images_->current->depth_u16.type() != CV_16UC1
        || !valid_intrinsics(images_->current->intrinsics, images_->current->depth_u16.size())
        || !same_intrinsics(images_->current->intrinsics, intrinsics_)
        || !m3t_depth_scale(images_->current->depth_scale)
        || static_cast<float>(images_->current->depth_scale) != depth_scale_) {
        return false;
    }
    set_camera_world_pose(*this, images_->camera_in_world);
    image_ = images_->current->depth_u16;
    SaveImageIfDesired();
    return true;
}

} // namespace rbt_m3t

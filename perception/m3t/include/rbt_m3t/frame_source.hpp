#pragma once

#include <rbt/time_stream.hpp>
#include <rbt_orbbec/driver.hpp>
#include <rbt/orbbec.hpp>
#include <m3t/camera.h>
#include <Eigen/Geometry>
#include <opencv2/core.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace rbt_m3t {

struct ImageSample {
    rbt_orbbec::Kind kind;
    rbt::time::SensorTiming timing;
    std::uint64_t sequence;
    std::uint64_t device_index;
    std::string frame_id;
    double unit_scale;
    // Owns pixels independently of the native sample guard. RGB8 / little-endian U16.
    cv::Mat pixels;
};

[[nodiscard]] ImageSample copy_image_sample(std::uint64_t sequence,
                                           std::span<const std::uint8_t> payload);

struct RgbdPair {
    ImageSample color;
    ImageSample depth;
    std::uint64_t skew_ns;
};

class PairBuffer {
  public:
    explicit PairBuffer(std::uint64_t maximum_skew_ns = 20'000'000U,
                        std::size_t capacity = 4U);
    void push(ImageSample sample);
    [[nodiscard]] std::optional<RgbdPair> take();
    [[nodiscard]] std::uint64_t dropped() const noexcept;

  private:
    std::uint64_t maximum_skew_ns_;
    std::size_t capacity_;
    std::deque<ImageSample> color_, depth_;
    std::optional<rbt::time::ClockIdentity> clock_;
    std::optional<std::int64_t> last_color_ns_, last_depth_ns_;
    std::optional<std::uint64_t> last_color_index_, last_depth_index_;
    std::uint64_t dropped_ { 0U };
};

struct RectifiedPair {
    cv::Mat bgr;
    cv::Mat depth_u16;
    m3t::Intrinsics intrinsics;
    double depth_scale;
    rbt::time::SensorTiming timing; // Depth capture timestamp is the pose timestamp.
    rbt::time::Timestamp color_capture;
    std::string frame_id;
    std::uint64_t skew_ns;
};

class Rectifier {
  public:
    // Requires the driver D2C path: same raw RGB pixel grid, frame and calibration.
    Rectifier(const rbt::orbbec::CameraInfoResponse& color,
              const rbt::orbbec::CameraInfoResponse& depth);
    [[nodiscard]] RectifiedPair apply(const RgbdPair& pair) const;

  private:
    cv::Mat map_x_, map_y_;
    m3t::Intrinsics intrinsics_;
    std::string frame_id_;
};

// The caller advances one shared immutable pair before Tracker::UpdateCameras.
// Neither camera independently receives/advances a frame.
struct CameraImages {
    std::optional<RectifiedPair> current;
    Eigen::Affine3f camera_in_world { Eigen::Affine3f::Identity() };
};

class RbtColorCamera final : public m3t::ColorCamera {
  public:
    RbtColorCamera(std::string name, std::shared_ptr<CameraImages> images);
    bool SetUp() override;
    bool UpdateImage(bool synchronized) override;
  private:
    std::shared_ptr<CameraImages> images_;
};

class RbtDepthCamera final : public m3t::DepthCamera {
  public:
    RbtDepthCamera(std::string name, std::shared_ptr<CameraImages> images);
    bool SetUp() override;
    bool UpdateImage(bool synchronized) override;
  private:
    std::shared_ptr<CameraImages> images_;
};

} // namespace rbt_m3t

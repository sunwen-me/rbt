#pragma once

#include <rbt/tf_math.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rbt_orbbec {

enum class Kind : std::uint32_t { Color = 1, Depth = 2, LeftIr = 3, RightIr = 4, Points = 5, Accel = 6, Gyro = 7 };
enum class Format : std::uint32_t { Rgb8 = 1, Mono8 = 2, Mono16 = 3, Depth16 = 4, Xyz32f = 5, Accel32f = 6, Gyro32f = 7 };

struct FilterParameter {
    std::string name;
    double value { 0.0 };
};

struct DepthFilterConfig {
    std::string name;
    std::vector<FilterParameter> parameters;
};

struct Options {
    std::string serial;
    std::string component_id { "orbbec335" };
    std::string frame_prefix { "orbbec335" };
    std::uint32_t width { 640 }, height { 480 }, fps { 30 };
    bool color { true }, depth { true }, left_ir { false }, right_ir { false };
    bool points { false }, imu { false };
    // Software D2C runs after the configured depth-filter chain. Aligned depth
    // and XYZ use color_optical, with output calibration obtained from the SDK.
    bool align_depth_to_color { false };
    std::vector<DepthFilterConfig> depth_filters;
    bool reconnect { true };
    std::uint32_t reconnect_delay_ms { 1000 };
    std::uint32_t frame_timeout_ms { 5000 };
    std::uint64_t duration_seconds { 0 };
};

struct DeviceInfo {
    std::string name, serial, firmware, connection;
    std::uint32_t product_id { 0 };
};

struct VideoCalibration {
    Kind kind { Kind::Depth };
    std::uint32_t width { 0 }, height { 0 }, fps { 0 };
    Format format { Format::Depth16 };
    double fx { 0 }, fy { 0 }, cx { 0 }, cy { 0 };
    // Orbbec OBCameraDistortionModel numeric value; SDK coefficient order:
    // k1, k2, k3, k4, k5, k6, p1, p2. No unadvertised rectification.
    std::uint32_t distortion_model { 0 };
    std::array<double, 8> distortion {};
};

struct Extrinsic {
    Kind parent { Kind::Depth }, child { Kind::Color };
    // T_parent<-child, row-major rotation; translation in metres.
    std::array<double, 9> rotation { 1, 0, 0, 0, 1, 0, 0, 0, 1 };
    std::array<double, 3> translation_m {};
};

struct Calibration {
    DeviceInfo device;
    std::vector<VideoCalibration> videos;
    std::vector<Extrinsic> extrinsics;
};

struct Frame {
    Kind kind { Kind::Depth };
    Format format { Format::Depth16 };
    std::uint32_t width { 0 }, height { 0 }, stride { 0 };
    std::uint64_t index { 0 }, capture_us { 0 };
    // Depth: metres per integer unit; RGB/IR/XYZ/IMU: 1.0.
    // Points are metres, acceleration m/s^2, gyro rad/s.
    double unit_scale { 1.0 };
    std::span<const std::uint8_t> data;
    // Keeps the SDK frame/storage alive until the native RBT loan is filled.
    std::shared_ptr<const void> owner;
};

class CaptureDevice {
  public:
    virtual ~CaptureDevice() = default;
    [[nodiscard]] virtual const Calibration& calibration() const = 0;
    // Timeout is bounded; empty means no frame. No unbounded callback queues.
    virtual std::vector<Frame> poll(std::chrono::milliseconds timeout) = 0;
    // Independent ordered IMU path. The first call starts SDK IMU sensors,
    // after activation endpoints exist. Bounded waits/batches; overflow fails.
    virtual std::vector<Frame> poll_imu(std::chrono::milliseconds timeout) = 0;
    virtual void stop() noexcept = 0;
    // Property IDs are the driver's explicit whitelist below, never raw SDK IDs.
    virtual std::int32_t get_control(std::uint32_t property) = 0;
    virtual void set_control(std::uint32_t property, std::int32_t value) = 0;
};

// 1 color auto exposure(bool), 2 color exposure, 3 color gain,
// 4 depth auto exposure(bool), 5 depth exposure, 6 depth gain,
// 7 color auto white balance(bool), 8 color white balance.
inline constexpr std::uint32_t kFirstControl = 1, kLastControl = 8;
class UnsupportedControl : public std::runtime_error {
  public:
    using std::runtime_error::runtime_error;
};
[[nodiscard]] std::vector<DeviceInfo> list_devices();
[[nodiscard]] std::unique_ptr<CaptureDevice> open_device(const Options& options);
[[nodiscard]] std::string sdk_version();

class DeviceMonitor {
  public:
    virtual ~DeviceMonitor() = default;
    // Presence is checked for the explicit serial only; it never selects a
    // different attached device. SDK callbacks record removal and wake waits.
    [[nodiscard]] virtual bool connected() = 0;
    virtual void wait_for_change(std::chrono::milliseconds timeout) = 0;
    // A target removal must remain observable even if USB is reinserted before
    // the next presence query. Other devices must not advance this counter.
    [[nodiscard]] virtual std::uint64_t disconnect_generation() const noexcept { return 0U; }
};
[[nodiscard]] std::unique_ptr<DeviceMonitor> watch_device(const std::string& serial);
[[nodiscard]] std::string describe_filters(const std::string& serial);
// Ordered CLI syntax: FilterName[:parameter=value,parameter=value].
[[nodiscard]] DepthFilterConfig parse_depth_filter(std::string_view text);
void validate_processing_options(const Options& options);
[[nodiscard]] std::string output_frame_id(const Options& options, Kind kind);

// StampedBytes application contract RBTORB01, little endian, 128-byte header.
// 0 magic[8]; 8 kind:u32; 12 format:u32; 16 width:u32; 20 height:u32;
// 24 stride:u32; 28 reserved[4]; 32 device frame index:u64; 40 scale:f64;
// 48 data size:u64; 56 reserved[8]; 64 canonical TF FrameId[64]; 128 data[].
inline constexpr std::size_t kFrameHeaderBytes = 128;
inline constexpr std::size_t kMaximumFrameBytes = 32 * 1024 * 1024;
struct DecodedFrame {
    Kind kind;
    Format format;
    std::uint32_t width, height, stride;
    std::uint64_t index;
    double unit_scale;
    std::string frame_id;
    std::span<const std::uint8_t> data;
};

[[nodiscard]] const char* kind_name(Kind kind);
[[nodiscard]] const char* format_name(Format format);
[[nodiscard]] std::string frame_id(std::string_view prefix, Kind kind);
[[nodiscard]] std::vector<Kind> enabled_kinds(const Options& options);
void validate_options(const Options& options);
void validate_frame(const Frame& frame);
void encode_frame(std::span<std::uint8_t> destination, const Frame& frame, std::string_view frame_name);
[[nodiscard]] DecodedFrame decode_frame(std::span<const std::uint8_t> application);
[[nodiscard]] rbt::tf::Transform3 transform_from_extrinsic(const Extrinsic& extrinsic);
[[nodiscard]] std::array<std::uint8_t, 16> parse_uuid(std::string_view text);
[[nodiscard]] std::string json_quote(std::string_view text);

} // namespace rbt_orbbec

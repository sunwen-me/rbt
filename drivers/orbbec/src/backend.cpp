#include <rbt_orbbec/driver.hpp>
#include <rbt_orbbec/detail/filter_validation.hpp>
#include <rbt_orbbec/detail/imu_queue.hpp>

#include <libobsensor/ObSensor.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rbt_orbbec {
[[nodiscard]] std::string describe_profiles(const std::string& serial);
namespace {

constexpr int kOrbbecVendorId = 0x2BC5;
constexpr int kGemini335ProductId = 0x0800;
constexpr std::string_view kGemini335Name = "Gemini 335";
constexpr std::string_view kOrbbecGemini335Name = "Orbbec Gemini 335";
constexpr std::size_t kMaximumPayloadBytes = kMaximumFrameBytes - kFrameHeaderBytes;

[[nodiscard]] bool is_gemini335_name(std::string_view name) noexcept {
    return name == kGemini335Name || name == kOrbbecGemini335Name;
}

void require_supported_sdk_version() {
    const auto version = sdk_version();
    if (version != "2.10.6") {
        throw std::runtime_error("this driver requires OrbbecSDK 2.10.6, found " + version);
    }
}

[[nodiscard]] std::string sdk_string(const char* value) {
    return value == nullptr ? std::string {} : std::string(value);
}

struct DeviceEntry {
    std::uint32_t index { 0 };
    int vendor_id { 0 };
    int product_id { 0 };
    std::string name;
    std::string serial;
    std::string connection;
};

[[nodiscard]] DeviceEntry device_entry(const ob::DeviceList& devices, std::uint32_t index) {
    return DeviceEntry {
        index,
        devices.getVid(index),
        devices.getPid(index),
        sdk_string(devices.getName(index)),
        sdk_string(devices.getSerialNumber(index)),
        sdk_string(devices.getConnectionType(index)),
    };
}

[[nodiscard]] DeviceEntry require_serial(const ob::DeviceList& devices, std::string_view serial) {
    if (serial.empty()) {
        throw std::invalid_argument("an explicit device serial is required");
    }

    std::optional<DeviceEntry> selected;
    bool has_unreadable_serial = false;
    const auto count = devices.getCount();
    for (std::uint32_t index = 0; index < count; ++index) {
        auto entry = device_entry(devices, index);
        has_unreadable_serial = has_unreadable_serial || entry.serial.empty();
        if (entry.serial != serial) {
            continue;
        }
        if (selected) {
            throw std::runtime_error("multiple Orbbec devices have the requested serial");
        }
        selected = std::move(entry);
    }
    if (!selected) {
        if (has_unreadable_serial) {
            throw std::runtime_error(
                "no Orbbec device has the requested serial, and one or more detected devices have an empty serial; "
                "check read/write access to the matching /dev/bus/usb node and Orbbec SDK logs, then rerun "
                "'rbt-orbbec-camera list'");
        }
        throw std::runtime_error("no Orbbec device has the requested serial");
    }
    return *selected;
}

void require_gemini335(const DeviceEntry& entry) {
    if (entry.vendor_id != kOrbbecVendorId || entry.product_id != kGemini335ProductId
        || !is_gemini335_name(entry.name)) {
        std::ostringstream message;
        message << "serial " << entry.serial << " identifies unsupported Orbbec model '" << entry.name
                << "' (VID 0x" << std::hex << entry.vendor_id << ", PID 0x" << entry.product_id
                << "); this driver requires the base Gemini 335 (VID 0x2BC5, PID 0x0800, name 'Gemini 335' "
                   "or 'Orbbec Gemini 335')";
        throw std::invalid_argument(message.str());
    }
}

[[nodiscard]] DeviceInfo read_device_info(const DeviceEntry& entry, const std::shared_ptr<ob::Device>& device) {
    auto sdk_info = device->getDeviceInfo();
    const auto name = sdk_string(sdk_info->getName());
    const auto serial = sdk_string(sdk_info->getSerialNumber());
    if (sdk_info->getVid() != kOrbbecVendorId || sdk_info->getPid() != kGemini335ProductId
        || !is_gemini335_name(name) || serial != entry.serial) {
        throw std::runtime_error("selected device metadata changed while opening the Gemini 335");
    }
    return DeviceInfo {
        name,
        serial,
        sdk_string(sdk_info->getFirmwareVersion()),
        sdk_string(sdk_info->getConnectionType()),
        static_cast<std::uint32_t>(sdk_info->getPid()),
    };
}

[[nodiscard]] std::shared_ptr<ob::Sensor> find_sensor(const std::shared_ptr<ob::SensorList>& sensors,
                                                       OBSensorType type) {
    if (!sensors) {
        return {};
    }
    for (std::uint32_t index = 0; index < sensors->getCount(); ++index) {
        if (sensors->getSensorType(index) == type) {
            return sensors->getSensor(index);
        }
    }
    return {};
}

struct VideoEncoding {
    Format output_format;
    std::optional<OBConvertFormat> conversion;
    int priority { 0 };
};

[[nodiscard]] std::optional<VideoEncoding> video_encoding(Kind kind, OBFormat format) {
    switch (kind) {
    case Kind::Color:
        switch (format) {
        case OB_FORMAT_RGB:
            return VideoEncoding { Format::Rgb8, std::nullopt, 0 };
        case OB_FORMAT_MJPG:
            return VideoEncoding { Format::Rgb8, FORMAT_MJPG_TO_RGB, 1 };
        case OB_FORMAT_YUYV:
            return VideoEncoding { Format::Rgb8, FORMAT_YUYV_TO_RGB, 2 };
        case OB_FORMAT_UYVY:
            return VideoEncoding { Format::Rgb8, FORMAT_UYVY_TO_RGB, 3 };
        case OB_FORMAT_NV12:
            return VideoEncoding { Format::Rgb8, FORMAT_NV12_TO_RGB, 4 };
        case OB_FORMAT_NV21:
            return VideoEncoding { Format::Rgb8, FORMAT_NV21_TO_RGB, 5 };
        case OB_FORMAT_I420:
            return VideoEncoding { Format::Rgb8, FORMAT_I420_TO_RGB, 6 };
        case OB_FORMAT_BGR:
            return VideoEncoding { Format::Rgb8, FORMAT_BGR_TO_RGB, 7 };
        case OB_FORMAT_RGBA:
            return VideoEncoding { Format::Rgb8, FORMAT_RGBA_TO_RGB, 8 };
        default:
            return std::nullopt;
        }
    case Kind::Depth:
        if (format == OB_FORMAT_Y16 || format == OB_FORMAT_Z16) {
            return VideoEncoding { Format::Depth16, std::nullopt, 0 };
        }
        return std::nullopt;
    case Kind::LeftIr:
    case Kind::RightIr:
        if (format == OB_FORMAT_Y16) {
            return VideoEncoding { Format::Mono16, std::nullopt, 0 };
        }
        if (format == OB_FORMAT_Y8) {
            return VideoEncoding { Format::Mono8, std::nullopt, 1 };
        }
        return std::nullopt;
    case Kind::Points:
    case Kind::Accel:
    case Kind::Gyro:
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] std::string_view sdk_format_name(OBFormat format) noexcept {
    switch (format) {
    case OB_FORMAT_YUYV:
        return "YUYV";
    case OB_FORMAT_YUY2:
        return "YUY2";
    case OB_FORMAT_UYVY:
        return "UYVY";
    case OB_FORMAT_NV12:
        return "NV12";
    case OB_FORMAT_NV21:
        return "NV21";
    case OB_FORMAT_MJPG:
        return "MJPG";
    case OB_FORMAT_Y16:
        return "Y16";
    case OB_FORMAT_Y8:
        return "Y8";
    case OB_FORMAT_Z16:
        return "Z16";
    case OB_FORMAT_I420:
        return "I420";
    case OB_FORMAT_RGB:
        return "RGB";
    case OB_FORMAT_BGR:
        return "BGR";
    case OB_FORMAT_RGBA:
        return "RGBA";
    default:
        return "UNKNOWN";
    }
}

struct VideoRequest {
    Kind kind;
    OBSensorType sensor_type;
    OBStreamType stream_type;
};

[[nodiscard]] VideoRequest video_request(Kind kind) {
    switch (kind) {
    case Kind::Color:
        return { kind, OB_SENSOR_COLOR, OB_STREAM_COLOR };
    case Kind::Depth:
        return { kind, OB_SENSOR_DEPTH, OB_STREAM_DEPTH };
    case Kind::LeftIr:
        return { kind, OB_SENSOR_IR_LEFT, OB_STREAM_IR_LEFT };
    case Kind::RightIr:
        return { kind, OB_SENSOR_IR_RIGHT, OB_STREAM_IR_RIGHT };
    case Kind::Points:
    case Kind::Accel:
    case Kind::Gyro:
        break;
    }
    throw std::invalid_argument("requested kind is not a video stream");
}

struct ResolvedVideo {
    Kind kind;
    std::shared_ptr<ob::VideoStreamProfile> profile;
    Format output_format;
    std::optional<OBConvertFormat> conversion;
};

[[nodiscard]] std::shared_ptr<ob::StreamProfileList> profile_list_for(
    const std::shared_ptr<ob::SensorList>& sensors, OBSensorType sensor_type) {
    auto sensor = find_sensor(sensors, sensor_type);
    if (!sensor) {
        return {};
    }
    return sensor->getStreamProfileList();
}

[[nodiscard]] ResolvedVideo resolve_video(const std::shared_ptr<ob::SensorList>& sensors,
                                           Kind kind,
                                           std::uint32_t width,
                                           std::uint32_t height,
                                           std::uint32_t fps) {
    const auto request = video_request(kind);
    auto profiles = profile_list_for(sensors, request.sensor_type);
    std::optional<ResolvedVideo> selected;
    int best_priority = std::numeric_limits<int>::max();

    if (profiles) {
        for (std::uint32_t index = 0; index < profiles->getCount(); ++index) {
            auto base = profiles->getProfile(index);
            if (base->getType() != request.stream_type) {
                continue;
            }
            auto profile = base->as<ob::VideoStreamProfile>();
            if (profile->getWidth() != width || profile->getHeight() != height || profile->getFps() != fps) {
                continue;
            }
            auto encoding = video_encoding(kind, profile->getFormat());
            if (!encoding || encoding->priority >= best_priority) {
                continue;
            }
            best_priority = encoding->priority;
            selected = ResolvedVideo { kind, std::move(profile), encoding->output_format, encoding->conversion };
        }
    }

    if (!selected) {
        std::ostringstream message;
        message << "unsupported Gemini 335 profile tuple for " << kind_name(kind) << ": " << width << 'x' << height
                << '@' << fps;
        throw std::invalid_argument(message.str());
    }
    return std::move(*selected);
}

[[nodiscard]] std::shared_ptr<ob::StreamProfile> first_imu_profile(
    const std::shared_ptr<ob::SensorList>& sensors, OBSensorType sensor_type, OBStreamType stream_type) {
    auto profiles = profile_list_for(sensors, sensor_type);
    if (!profiles) {
        return {};
    }
    for (std::uint32_t index = 0; index < profiles->getCount(); ++index) {
        auto profile = profiles->getProfile(index);
        if (profile->getType() == stream_type) {
            return profile;
        }
    }
    return {};
}

[[nodiscard]] VideoCalibration video_calibration(Kind kind,
                                                 const std::shared_ptr<ob::VideoStreamProfile>& profile,
                                                 Format output_format) {
    const auto intrinsic = profile->getIntrinsic();
    const auto distortion = profile->getDistortion();
    const auto width = profile->getWidth();
    const auto height = profile->getHeight();
    const std::array<double, 8> coefficients { distortion.k1, distortion.k2, distortion.k3, distortion.k4,
                                                distortion.k5, distortion.k6, distortion.p1, distortion.p2 };
    if (intrinsic.width <= 0 || intrinsic.height <= 0
        || static_cast<std::uint32_t>(intrinsic.width) != width
        || static_cast<std::uint32_t>(intrinsic.height) != height || !std::isfinite(intrinsic.fx)
        || intrinsic.fx <= 0.0F || !std::isfinite(intrinsic.fy) || intrinsic.fy <= 0.0F
        || !std::isfinite(intrinsic.cx) || !std::isfinite(intrinsic.cy)
        || !std::all_of(coefficients.begin(), coefficients.end(), [](double value) { return std::isfinite(value); })) {
        std::ostringstream message;
        message << "SDK returned invalid calibration for " << kind_name(kind) << ' ' << width << 'x'
                << height;
        throw std::runtime_error(message.str());
    }
    VideoCalibration result;
    result.kind = kind;
    result.width = width;
    result.height = height;
    result.fps = profile->getFps();
    result.format = output_format;
    result.fx = intrinsic.fx;
    result.fy = intrinsic.fy;
    result.cx = intrinsic.cx;
    result.cy = intrinsic.cy;
    result.distortion_model = static_cast<std::uint32_t>(distortion.model);
    result.distortion = coefficients;
    return result;
}

[[nodiscard]] VideoCalibration video_calibration(const ResolvedVideo& resolved) {
    return video_calibration(resolved.kind, resolved.profile, resolved.output_format);
}

[[nodiscard]] bool same_video_calibration(const VideoCalibration& lhs, const VideoCalibration& rhs) noexcept {
    return lhs.kind == rhs.kind && lhs.width == rhs.width && lhs.height == rhs.height && lhs.fps == rhs.fps
           && lhs.format == rhs.format && lhs.fx == rhs.fx && lhs.fy == rhs.fy && lhs.cx == rhs.cx
           && lhs.cy == rhs.cy && lhs.distortion_model == rhs.distortion_model && lhs.distortion == rhs.distortion;
}

[[nodiscard]] std::optional<Extrinsic> calibrated_extrinsic(
    const std::shared_ptr<ob::StreamProfile>& source,
    const std::shared_ptr<ob::StreamProfile>& depth_profile,
    Kind child) {
    try {
        const auto sdk_extrinsic = source->getExtrinsicTo(depth_profile);
        Extrinsic result;
        result.parent = Kind::Depth;
        result.child = child;
        for (std::size_t index = 0; index < result.rotation.size(); ++index) {
            result.rotation[index] = sdk_extrinsic.rot[index];
            if (!std::isfinite(result.rotation[index])) {
                throw std::runtime_error("SDK returned a non-finite camera rotation");
            }
        }
        for (std::size_t index = 0; index < result.translation_m.size(); ++index) {
            result.translation_m[index] = static_cast<double>(sdk_extrinsic.trans[index]) / 1000.0;
            if (!std::isfinite(result.translation_m[index])) {
                throw std::runtime_error("SDK returned a non-finite camera translation");
            }
        }
        return result;
    } catch (const ob::Error& error) {
        if (error.getStatus() == OB_ERROR_INVALID_PARAMETER
            || error.getStatus() == OB_ERROR_UNSUPPORTED_OPERATION
            || error.getExceptionType() == OB_EXCEPTION_TYPE_UNSUPPORTED_OPERATION) {
            return std::nullopt;
        }
        throw;
    }
}

[[nodiscard]] std::uint32_t checked_stride(const ob::VideoFrame& frame, std::size_t bytes_per_pixel) {
    const auto width = frame.getWidth();
    const auto height = frame.getHeight();
    const auto size = static_cast<std::size_t>(frame.getDataSize());
    if (width == 0 || height == 0 || size == 0 || size > kMaximumPayloadBytes || size % height != 0) {
        throw std::runtime_error("SDK returned invalid or oversized video frame storage");
    }
    const auto stride = size / height;
    const auto minimum = static_cast<std::size_t>(width) * bytes_per_pixel;
    if (stride < minimum || stride > std::numeric_limits<std::uint32_t>::max()) {
        throw std::runtime_error("SDK returned an invalid video frame stride");
    }
    return static_cast<std::uint32_t>(stride);
}

[[nodiscard]] std::shared_ptr<const void> frame_owner(const std::shared_ptr<ob::Frame>& frame) {
    return std::static_pointer_cast<const void>(frame);
}

[[nodiscard]] Frame make_video_frame(Kind kind,
                                     Format format,
                                     const std::shared_ptr<ob::VideoFrame>& sdk_frame,
                                     std::size_t bytes_per_pixel,
                                     double unit_scale = 1.0) {
    auto* data = sdk_frame->getData();
    const auto size = static_cast<std::size_t>(sdk_frame->getDataSize());
    if (data == nullptr || size == 0 || size > kMaximumPayloadBytes) {
        throw std::runtime_error("SDK returned invalid or oversized video frame data");
    }
    Frame result;
    result.kind = kind;
    result.format = format;
    result.width = sdk_frame->getWidth();
    result.height = sdk_frame->getHeight();
    result.stride = checked_stride(*sdk_frame, bytes_per_pixel);
    result.index = sdk_frame->getIndex();
    result.capture_us = sdk_frame->getTimeStampUs();
    result.unit_scale = unit_scale;
    result.data = std::span<const std::uint8_t>(data, size);
    result.owner = frame_owner(sdk_frame);
    return result;
}

[[nodiscard]] Frame make_imu_frame(Kind kind,
                                   std::uint64_t index,
                                   std::uint64_t capture_us,
                                   float x,
                                   float y,
                                   float z) {
    auto values = std::make_shared<std::array<float, 3>>(std::array<float, 3> { x, y, z });
    Frame result;
    result.kind = kind;
    result.format = kind == Kind::Accel ? Format::Accel32f : Format::Gyro32f;
    result.width = 1;
    result.height = 1;
    result.stride = sizeof(*values);
    result.index = index;
    result.capture_us = capture_us;
    result.unit_scale = 1.0;
    result.data = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(values->data()), sizeof(*values));
    result.owner = std::static_pointer_cast<const void>(values);
    return result;
}

[[nodiscard]] detail::ImuSample read_imu_sample(const std::shared_ptr<ob::Frame>& frame, Kind kind) {
    if (!frame) {
        throw std::runtime_error("Orbbec IMU callback received an empty frame");
    }

    detail::ImuSample sample;
    sample.kind = kind;
    sample.index = frame->getIndex();
    sample.capture_us = frame->getTimeStampUs();
    if (kind == Kind::Accel) {
        if (!frame->is<ob::AccelFrame>()) {
            throw std::runtime_error("Orbbec accelerometer callback received a malformed frame");
        }
        ob_error* error = nullptr;
        const auto value = ob_accel_frame_get_value(frame->getImpl(), &error);
        ob::Error::handle(&error);
        sample.xyz = { value.x, value.y, value.z };
    } else if (kind == Kind::Gyro) {
        if (!frame->is<ob::GyroFrame>()) {
            throw std::runtime_error("Orbbec gyroscope callback received a malformed frame");
        }
        ob_error* error = nullptr;
        const auto value = ob_gyro_frame_get_value(frame->getImpl(), &error);
        ob::Error::handle(&error);
        sample.xyz = { value.x, value.y, value.z };
    } else {
        throw std::runtime_error("Orbbec IMU callback received an invalid sample kind");
    }
    return sample;
}

[[nodiscard]] ob::Sensor::FrameCallback make_imu_callback(const std::shared_ptr<detail::ImuQueue>& queue,
                                                           Kind kind) {
    return [queue, kind](std::shared_ptr<ob::Frame> frame) noexcept {
        try {
            queue->push(read_imu_sample(frame, kind));
        } catch (...) {
            try {
                queue->fail(std::current_exception());
            } catch (...) {
            }
        }
    };
}

struct ControlSpec {
    OBPropertyID id;
    bool boolean;
    std::string_view name;
};

[[nodiscard]] ControlSpec control_spec(std::uint32_t property) {
    switch (property) {
    case 1:
        return { OB_PROP_COLOR_AUTO_EXPOSURE_BOOL, true, "color auto exposure" };
    case 2:
        return { OB_PROP_COLOR_EXPOSURE_INT, false, "color exposure" };
    case 3:
        return { OB_PROP_COLOR_GAIN_INT, false, "color gain" };
    case 4:
        return { OB_PROP_DEPTH_AUTO_EXPOSURE_BOOL, true, "depth auto exposure" };
    case 5:
        return { OB_PROP_DEPTH_EXPOSURE_INT, false, "depth exposure" };
    case 6:
        return { OB_PROP_DEPTH_GAIN_INT, false, "depth gain" };
    case 7:
        return { OB_PROP_COLOR_AUTO_WHITE_BALANCE_BOOL, true, "color auto white balance" };
    case 8:
        return { OB_PROP_COLOR_WHITE_BALANCE_INT, false, "color white balance" };
    default:
        throw std::invalid_argument("control property must be in the driver whitelist 1..8");
    }
}

void require_control_permission(const std::shared_ptr<ob::Device>& device,
                                const ControlSpec& spec,
                                OBPermissionType permission) {
    if (!device->isPropertySupported(spec.id, permission)) {
        throw UnsupportedControl(std::string(spec.name) + " is unsupported by this Gemini 335");
    }
}

void append_video_profile_json(std::ostringstream& output,
                               bool& first,
                               const std::shared_ptr<ob::SensorList>& sensors,
                               Kind kind,
                               std::string_view kind_text,
                               OBSensorType sensor_type,
                               OBStreamType stream_type) {
    auto profiles = profile_list_for(sensors, sensor_type);
    if (!profiles) {
        return;
    }
    std::vector<std::string> entries;
    for (std::uint32_t index = 0; index < profiles->getCount(); ++index) {
        auto base = profiles->getProfile(index);
        if (base->getType() != stream_type) {
            continue;
        }
        auto profile = base->as<ob::VideoStreamProfile>();
        auto encoding = video_encoding(kind, profile->getFormat());
        if (!encoding) {
            continue;
        }
        std::ostringstream item;
        item << "{\"kind\":" << json_quote(kind_text)
             << ",\"width\":" << profile->getWidth()
             << ",\"height\":" << profile->getHeight()
             << ",\"fps\":" << profile->getFps()
             << ",\"format\":" << json_quote(sdk_format_name(profile->getFormat()))
             << ",\"output_format\":" << json_quote(format_name(encoding->output_format));
        if (encoding->conversion) {
            item << ",\"conversion\":\"sdk\"";
        }
        item << '}';
        entries.push_back(item.str());
    }
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
    for (const auto& entry : entries) {
        if (!first) {
            output << ',';
        }
        first = false;
        output << entry;
    }
}

[[nodiscard]] bool has_imu_profile(const std::shared_ptr<ob::SensorList>& sensors,
                                   OBSensorType sensor_type,
                                   OBStreamType stream_type) {
    return static_cast<bool>(first_imu_profile(sensors, sensor_type, stream_type));
}

[[nodiscard]] detail::FilterValueType filter_value_type(OBFilterConfigValueType type) noexcept {
    switch (type) {
    case OB_FILTER_CONFIG_VALUE_TYPE_INT:
        return detail::FilterValueType::Integer;
    case OB_FILTER_CONFIG_VALUE_TYPE_FLOAT:
        return detail::FilterValueType::Floating;
    case OB_FILTER_CONFIG_VALUE_TYPE_BOOLEAN:
        return detail::FilterValueType::Boolean;
    default:
        return detail::FilterValueType::Unsupported;
    }
}

[[nodiscard]] std::string_view filter_value_type_name(detail::FilterValueType type) noexcept {
    switch (type) {
    case detail::FilterValueType::Integer:
        return "int";
    case detail::FilterValueType::Floating:
        return "float";
    case detail::FilterValueType::Boolean:
        return "bool";
    case detail::FilterValueType::Unsupported:
        return "invalid";
    }
    return "invalid";
}

[[nodiscard]] std::vector<detail::FilterParameterSchema> filter_parameter_schemas(
    const std::shared_ptr<ob::Filter>& filter) {
    const auto sdk_schema = filter->getConfigSchemaVec();
    std::vector<detail::FilterParameterSchema> result;
    result.reserve(sdk_schema.size());
    for (const auto& item : sdk_schema) {
        result.push_back(detail::FilterParameterSchema {
            sdk_string(item.name), filter_value_type(item.type), item.min, item.max, item.step,
        });
    }
    return result;
}

struct RecommendedDepthFilter {
    std::string name;
    std::shared_ptr<ob::Filter> filter;
    std::vector<detail::FilterParameterSchema> schema;
};

[[nodiscard]] std::vector<RecommendedDepthFilter> recommended_depth_filters(
    const std::shared_ptr<ob::Sensor>& sensor) {
    std::vector<RecommendedDepthFilter> result;
    for (auto& filter : sensor->createRecommendedFilters()) {
        auto schemas = filter_parameter_schemas(filter);
        result.push_back(RecommendedDepthFilter { filter->getName(), std::move(filter), std::move(schemas) });
    }
    return result;
}

[[nodiscard]] std::string json_number(double value) {
    if (!std::isfinite(value)) {
        throw std::runtime_error("SDK returned a non-finite depth filter schema value");
    }
    std::ostringstream result;
    result << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return result.str();
}

void append_filter_json(std::ostringstream& output, const RecommendedDepthFilter& recommended, bool& first) {
    if (!first) {
        output << ',';
    }
    first = false;
    output << "{\"name\":" << json_quote(recommended.name)
           << ",\"default_enabled\":" << (recommended.filter->isEnabled() ? "true" : "false")
           << ",\"parameters\":[";
    const auto sdk_schema = recommended.filter->getConfigSchemaVec();
    for (std::size_t index = 0; index < sdk_schema.size(); ++index) {
        const auto& item = sdk_schema[index];
        if (index != 0) {
            output << ',';
        }
        const auto name = sdk_string(item.name);
        const auto value_type = filter_value_type(item.type);
        const auto current = recommended.filter->getConfigValue(name);
        output << "{\"name\":" << json_quote(name)
               << ",\"type\":" << json_quote(filter_value_type_name(value_type))
               << ",\"min\":" << json_number(item.min)
               << ",\"max\":" << json_number(item.max)
               << ",\"step\":" << json_number(item.step)
               << ",\"default\":" << json_number(item.def)
               << ",\"current\":" << json_number(current)
               << ",\"description\":" << json_quote(sdk_string(item.desc)) << '}';
    }
    output << "]}";
}

class OrbbecCaptureDevice final : public CaptureDevice {
  public:
    OrbbecCaptureDevice(std::shared_ptr<ob::Context> context,
                       std::shared_ptr<ob::Device> device,
                       DeviceInfo info,
                       const Options& options)
        : context_(std::move(context)), device_(std::move(device)), options_(options) {
        calibration_.device = std::move(info);
        auto sensors = device_->getSensorList();

        if (options_.color) {
            color_ = resolve_video(sensors, Kind::Color, options_.width, options_.height, options_.fps);
            calibration_.videos.push_back(video_calibration(*color_));
        }
        if (options_.depth) {
            depth_ = resolve_video(sensors, Kind::Depth, options_.width, options_.height, options_.fps);
            depth_output_calibration_ = video_calibration(*depth_);
            calibration_.videos.push_back(*depth_output_calibration_);
        }
        if (options_.left_ir) {
            left_ir_ = resolve_video(sensors, Kind::LeftIr, options_.width, options_.height, options_.fps);
            calibration_.videos.push_back(video_calibration(*left_ir_));
        }
        if (options_.right_ir) {
            right_ir_ = resolve_video(sensors, Kind::RightIr, options_.width, options_.height, options_.fps);
            calibration_.videos.push_back(video_calibration(*right_ir_));
        }

        if (!options_.depth_filters.empty()) {
            configure_depth_filters(find_sensor(sensors, OB_SENSOR_DEPTH));
        }
        if (options_.align_depth_to_color) {
            if (!depth_ || !color_) {
                throw std::invalid_argument("software depth-to-color alignment requires depth and color streams");
            }
            align_ = std::make_shared<ob::Align>(OB_STREAM_COLOR);
            align_->setAlignToStreamProfile(color_->profile);
            // Project onto raw RGB pixels, which retain the color lens distortion.
            align_->setConfigValue("TargetDistortion", 1.0);
            align_->setMatchTargetResolution(true);
        }

        if (options_.imu) {
            accel_sensor_ = find_sensor(sensors, OB_SENSOR_ACCEL);
            gyro_sensor_ = find_sensor(sensors, OB_SENSOR_GYRO);
            if (!accel_sensor_ || !gyro_sensor_) {
                throw std::invalid_argument("Gemini 335 does not expose both accelerometer and gyroscope profiles");
            }
            auto accel_profiles = accel_sensor_->getStreamProfileList();
            auto gyro_profiles = gyro_sensor_->getStreamProfileList();
            if (!accel_profiles || !gyro_profiles) {
                throw std::invalid_argument("Gemini 335 does not expose both accelerometer and gyroscope profiles");
            }
            accel_profile_ = accel_profiles->getAccelStreamProfile(
                OB_ACCEL_FULL_SCALE_RANGE_ANY, OB_SAMPLE_RATE_200_HZ);
            gyro_profile_ = gyro_profiles->getGyroStreamProfile(
                OB_GYRO_FULL_SCALE_RANGE_ANY, OB_SAMPLE_RATE_200_HZ);
        }

        if (depth_) {
            const auto depth_base = std::static_pointer_cast<ob::StreamProfile>(depth_->profile);
            const auto append_extrinsic = [this, &depth_base](const auto& video, Kind kind) {
                if (!video || kind == Kind::Depth) {
                    return;
                }
                auto source = std::static_pointer_cast<ob::StreamProfile>(video->profile);
                if (auto extrinsic = calibrated_extrinsic(source, depth_base, kind)) {
                    calibration_.extrinsics.push_back(*extrinsic);
                }
            };
            append_extrinsic(color_, Kind::Color);
            append_extrinsic(left_ir_, Kind::LeftIr);
            append_extrinsic(right_ir_, Kind::RightIr);
            if (accel_profile_) {
                if (auto extrinsic = calibrated_extrinsic(accel_profile_, depth_base, Kind::Accel)) {
                    calibration_.extrinsics.push_back(*extrinsic);
                }
            }
            if (gyro_profile_) {
                if (auto extrinsic = calibrated_extrinsic(gyro_profile_, depth_base, Kind::Gyro)) {
                    calibration_.extrinsics.push_back(*extrinsic);
                }
            }
        }

        if (options_.points) {
            point_cloud_ = std::make_shared<ob::PointCloudFilter>();
            point_cloud_->setCreatePointFormat(OB_FORMAT_POINT);
            point_cloud_->setCoordinateSystem(OB_RIGHT_HAND_COORDINATE_SYSTEM);
            point_cloud_->setCoordinateDataScaled(1.0F);
            point_cloud_->setConfigValue("outputZeroPoint", 1.0);
        }
        if (options_.color && color_->conversion) {
            color_converter_ = std::make_shared<ob::FormatConvertFilter>();
            color_converter_->setFormatConvertType(*color_->conversion);
        }

        auto config = std::make_shared<ob::Config>();
        if (color_) {
            config->enableStream(std::static_pointer_cast<ob::StreamProfile>(color_->profile));
        }
        if (depth_) {
            config->enableStream(std::static_pointer_cast<ob::StreamProfile>(depth_->profile));
        }
        if (left_ir_) {
            config->enableStream(std::static_pointer_cast<ob::StreamProfile>(left_ir_->profile));
        }
        if (right_ir_) {
            config->enableStream(std::static_pointer_cast<ob::StreamProfile>(right_ir_->profile));
        }
        config->setFrameAggregateOutputMode(OB_FRAME_AGGREGATE_OUTPUT_ANY_SITUATION);

        pipeline_ = std::make_shared<ob::Pipeline>(device_);
        start_attempted_ = true;
        try {
            pipeline_->start(config);
            started_ = true;
            if (align_) {
                pipeline_->enableFrameSync();
            }
            if (processing_enabled()) {
                prime_processed_depth();
            }
        } catch (...) {
            try {
                pipeline_->stop();
            } catch (...) {
            }
            stopped_ = true;
            throw;
        }
    }

    ~OrbbecCaptureDevice() override {
        stop();
    }

    [[nodiscard]] const Calibration& calibration() const override {
        return calibration_;
    }

    std::vector<Frame> poll(std::chrono::milliseconds timeout) override {
        std::lock_guard poll_guard(poll_mutex_);
        if (timeout.count() <= 0) {
            return {};
        }
        if (pending_batch_available_) {
            pending_batch_available_ = false;
            return std::move(pending_batch_);
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        auto frameset = wait_for_frameset(deadline);
        return frameset ? process_frameset(frameset) : std::vector<Frame> {};
    }

    std::vector<Frame> poll_imu(std::chrono::milliseconds timeout) override {
        if (!options_.imu) {
            return {};
        }
        start_imu_sensors();
        const auto batch = imu_queue_->wait_and_drain(timeout);
        std::vector<Frame> result;
        result.reserve(batch.size);
        for (std::size_t index = 0; index < batch.size; ++index) {
            const auto& sample = batch.samples[index];
            result.push_back(make_imu_frame(sample.kind, sample.index, sample.capture_us,
                                            sample.xyz[0], sample.xyz[1], sample.xyz[2]));
        }
        return result;
    }

    void stop() noexcept override {
        try {
            imu_queue_->close();
        } catch (...) {
        }
        {
            std::lock_guard imu_guard(imu_lifecycle_mutex_);
            stop_imu_sensors_locked();
        }
        std::lock_guard pipeline_guard(pipeline_mutex_);
        if (stopped_) {
            return;
        }
        stopped_ = true;
        if (start_attempted_ && pipeline_) {
            try {
                pipeline_->stop();
            } catch (...) {
            }
        }
        started_ = false;
    }

    std::int32_t get_control(std::uint32_t property) override {
        const auto spec = control_spec(property);
        require_control_permission(device_, spec, OB_PERMISSION_READ);
        if (spec.boolean) {
            const auto range = device_->getBoolPropertyRange(spec.id);
            const auto value = device_->getBoolProperty(spec.id);
            if (value < range.min || value > range.max) {
                throw std::runtime_error(std::string(spec.name) + " returned a value outside its SDK range");
            }
            return value ? 1 : 0;
        }
        const auto range = device_->getIntPropertyRange(spec.id);
        const auto value = device_->getIntProperty(spec.id);
        if (value < range.min || value > range.max) {
            throw std::runtime_error(std::string(spec.name) + " returned a value outside its SDK range");
        }
        return value;
    }

    void set_control(std::uint32_t property, std::int32_t value) override {
        const auto spec = control_spec(property);
        if (spec.boolean && value != 0 && value != 1) {
            throw std::out_of_range(std::string(spec.name) + " accepts only 0 or 1");
        }
        require_control_permission(device_, spec, OB_PERMISSION_READ_WRITE);
        if (spec.boolean) {
            const auto range = device_->getBoolPropertyRange(spec.id);
            if (value < static_cast<std::int32_t>(range.min) || value > static_cast<std::int32_t>(range.max)) {
                throw std::out_of_range(std::string(spec.name) + " is outside its SDK property range");
            }
            device_->setBoolProperty(spec.id, value != 0);
            return;
        }
        const auto range = device_->getIntPropertyRange(spec.id);
        const auto offset = static_cast<std::int64_t>(value) - static_cast<std::int64_t>(range.min);
        if (value < range.min || value > range.max || (range.step > 0 && offset % range.step != 0)) {
            throw std::out_of_range(std::string(spec.name) + " is outside its SDK property range");
        }
        device_->setIntProperty(spec.id, value);
    }

  private:
    void configure_depth_filters(const std::shared_ptr<ob::Sensor>& depth_sensor) {
        if (!depth_sensor) {
            throw std::invalid_argument("Gemini 335 does not expose a depth sensor for the requested filters");
        }
        auto recommended = recommended_depth_filters(depth_sensor);
        std::map<std::string, std::size_t, std::less<>> indices;
        for (std::size_t index = 0; index < recommended.size(); ++index) {
            if (!indices.emplace(recommended[index].name, index).second) {
                throw std::runtime_error("SDK returned duplicate recommended depth filter names");
            }
        }

        std::vector<std::size_t> selected;
        selected.reserve(options_.depth_filters.size());
        for (const auto& requested : options_.depth_filters) {
            const auto found = indices.find(requested.name);
            if (found == indices.end()) {
                throw std::invalid_argument("unsupported recommended depth filter '" + requested.name + "'");
            }
            const auto& entry = recommended[found->second];
            detail::validate_filter_parameters(requested.parameters, entry.schema);
            selected.push_back(found->second);
        }

        for (auto& entry : recommended) {
            entry.filter->enable(false);
        }
        depth_filter_chain_.reserve(selected.size());
        for (std::size_t request_index = 0; request_index < selected.size(); ++request_index) {
            const auto& request = options_.depth_filters[request_index];
            auto& entry = recommended[selected[request_index]];
            for (const auto& parameter : request.parameters) {
                entry.filter->setConfigValue(parameter.name, parameter.value);
            }
            entry.filter->enable(true);
            depth_filter_chain_.push_back(entry.filter);
        }
    }

    [[nodiscard]] bool processing_enabled() const noexcept {
        return align_ || !depth_filter_chain_.empty();
    }

    [[nodiscard]] std::shared_ptr<ob::FrameSet> wait_for_frameset(
        std::chrono::steady_clock::time_point deadline) {
        while (std::chrono::steady_clock::now() < deadline) {
            std::lock_guard pipeline_guard(pipeline_mutex_);
            if (stopped_ || !pipeline_) {
                return {};
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return {};
            }
            auto remaining = deadline - now;
            auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
            if (remaining_ms < remaining) {
                ++remaining_ms;
            }
            const auto wait_ms = static_cast<std::uint32_t>(std::min<std::int64_t>(
                100, std::max<std::int64_t>(1, remaining_ms.count())));
            if (auto frameset = pipeline_->waitForFrameset(wait_ms)) {
                return frameset;
            }
        }
        return {};
    }

    void prime_processed_depth() {
        const auto timeout = std::chrono::milliseconds(options_.frame_timeout_ms);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            auto frameset = wait_for_frameset(deadline);
            if (!frameset) {
                break;
            }
            pending_batch_ = process_frameset(frameset, true);
            const auto has_depth = std::any_of(pending_batch_.begin(), pending_batch_.end(), [](const Frame& frame) {
                return frame.kind == Kind::Depth;
            });
            if (has_depth) {
                pending_batch_available_ = true;
                return;
            }
        }
        throw std::runtime_error("timed out waiting for the first processed Gemini 335 depth frame");
    }

    void validate_processed_depth_metadata(const std::shared_ptr<ob::DepthFrame>& output) {
        if (!output) {
            throw std::runtime_error("SDK depth processing returned an empty frame");
        }
        if (output->getTimeStampUs() == 0) {
            throw std::runtime_error("SDK depth processing lost its capture timestamp");
        }
        const auto scale = output->getValueScale();
        if (!std::isfinite(scale) || scale <= 0.0F) {
            throw std::runtime_error("SDK depth processing returned an invalid depth value scale");
        }
    }

    [[nodiscard]] std::shared_ptr<ob::DepthFrame> process_depth(
        const std::shared_ptr<ob::DepthFrame>& source_depth,
        const std::shared_ptr<ob::VideoFrame>& raw_color) {
        if (align_ && !raw_color) {
            return {};
        }
        auto processed = source_depth;
        for (const auto& filter : depth_filter_chain_) {
            auto output = filter->process(processed);
            if (!output) {
                return {};
            }
            if (!output->is<ob::DepthFrame>()) {
                throw std::runtime_error("recommended depth filter returned a non-depth frame");
            }
            processed = output->as<ob::DepthFrame>();
            validate_processed_depth_metadata(processed);
        }
        if (align_) {
            auto input = ob::FrameFactory::createFrameSet();
            input->pushFrame(processed);
            input->pushFrame(raw_color);
            auto aligned = align_->process(input);
            if (!aligned) {
                return {};
            }
            if (!aligned->is<ob::FrameSet>()) {
                throw std::runtime_error("SDK color alignment returned a non-frameset output");
            }
            auto aligned_frameset = aligned->as<ob::FrameSet>();
            auto aligned_depth = aligned_frameset->getFrame(OB_FRAME_DEPTH);
            if (!aligned_depth) {
                return {};
            }
            if (!aligned_depth->is<ob::DepthFrame>()) {
                throw std::runtime_error("SDK color alignment returned a non-depth frame");
            }
            processed = aligned_depth->as<ob::DepthFrame>();
            validate_processed_depth_metadata(processed);
        }
        return processed;
    }

    void record_depth_calibration(const std::shared_ptr<ob::DepthFrame>& frame, bool establishing) {
        validate_processed_depth_metadata(frame);
        auto stream_profile = frame->getStreamProfile();
        if (!stream_profile || !stream_profile->is<ob::VideoStreamProfile>()) {
            throw std::runtime_error("SDK processed depth frame has no video stream profile");
        }
        auto profile = stream_profile->as<ob::VideoStreamProfile>();
        if (profile->getType() != OB_STREAM_DEPTH
            || (profile->getFormat() != OB_FORMAT_Y16 && profile->getFormat() != OB_FORMAT_Z16)
            || profile->getFormat() != frame->getFormat()) {
            throw std::runtime_error("SDK processed depth frame has an unsupported stream profile");
        }
        const auto current = video_calibration(Kind::Depth, profile, Format::Depth16);
        if (align_ && (!color_ || current.width != color_->profile->getWidth()
                       || current.height != color_->profile->getHeight())) {
            throw std::runtime_error("SDK aligned depth dimensions do not match the selected color profile");
        }
        if (establishing) {
            depth_output_calibration_ = current;
            const auto existing = std::find_if(calibration_.videos.begin(), calibration_.videos.end(), [](const auto& video) {
                return video.kind == Kind::Depth;
            });
            if (existing == calibration_.videos.end()) {
                throw std::runtime_error("depth calibration is missing during processed output activation");
            }
            *existing = current;
        } else if (!depth_output_calibration_ || !same_video_calibration(current, *depth_output_calibration_)) {
            throw std::runtime_error("SDK processed depth profile changed after activation");
        }
    }

    [[nodiscard]] std::vector<Frame> process_frameset(const std::shared_ptr<ob::FrameSet>& frameset,
                                                      bool establishing = false) {
        std::vector<Frame> result;
        std::shared_ptr<ob::VideoFrame> raw_color;
        if (color_) {
            if (auto raw = frameset->getFrame(OB_FRAME_COLOR)) {
                raw_color = raw->as<ob::VideoFrame>();
                auto video = raw_color;
                if (color_converter_) {
                    auto converted = color_converter_->process(video);
                    if (!converted) {
                        throw std::runtime_error("SDK color conversion returned no frame");
                    }
                    video = converted->as<ob::VideoFrame>();
                }
                if (video->getFormat() != OB_FORMAT_RGB
                    || video->getWidth() != options_.width || video->getHeight() != options_.height) {
                    throw std::runtime_error("SDK color frame does not match the selected RGB profile");
                }
                result.push_back(make_video_frame(Kind::Color, Format::Rgb8, video, 3));
            }
        }

        if (depth_) {
            if (auto raw = frameset->getFrame(OB_FRAME_DEPTH)) {
                auto source = raw->as<ob::DepthFrame>();
                if ((source->getFormat() != OB_FORMAT_Y16 && source->getFormat() != OB_FORMAT_Z16)
                    || source->getWidth() != depth_->profile->getWidth()
                    || source->getHeight() != depth_->profile->getHeight()) {
                    throw std::runtime_error("SDK depth frame does not match the selected source profile");
                }
                auto video = process_depth(source, raw_color);
                if (video) {
                    record_depth_calibration(video, establishing);
                    const auto scale = static_cast<double>(video->getValueScale()) / 1000.0;
                    if (!std::isfinite(scale) || scale <= 0.0) {
                        throw std::runtime_error("SDK returned an invalid processed depth value scale");
                    }
                    result.push_back(make_video_frame(Kind::Depth, Format::Depth16, video, 2, scale));
                    if (options_.points) {
                        append_points(result, video);
                    }
                }
            }
        }
        if (left_ir_) {
            append_ir(result, frameset, OB_FRAME_IR_LEFT, Kind::LeftIr, *left_ir_);
        }
        if (right_ir_) {
            append_ir(result, frameset, OB_FRAME_IR_RIGHT, Kind::RightIr, *right_ir_);
        }
        return result;
    }

    void start_imu_sensors() {
        std::lock_guard imu_guard(imu_lifecycle_mutex_);
        if (imu_start_attempted_ || imu_queue_->closed()) {
            return;
        }
        imu_start_attempted_ = true;
        try {
            accel_start_attempted_ = true;
            accel_sensor_->start(accel_profile_, make_imu_callback(imu_queue_, Kind::Accel));
            gyro_start_attempted_ = true;
            gyro_sensor_->start(gyro_profile_, make_imu_callback(imu_queue_, Kind::Gyro));
        } catch (...) {
            const auto original_error = std::current_exception();
            try {
                imu_queue_->fail(original_error);
            } catch (...) {
            }
            try {
                imu_queue_->close();
            } catch (...) {
            }
            stop_imu_sensors_locked();
            std::rethrow_exception(original_error);
        }
    }

    void stop_imu_sensors_locked() noexcept {
        if (accel_start_attempted_) {
            accel_start_attempted_ = false;
            try {
                accel_sensor_->stop();
            } catch (...) {
            }
        }
        if (gyro_start_attempted_) {
            gyro_start_attempted_ = false;
            try {
                gyro_sensor_->stop();
            } catch (...) {
            }
        }
    }

    void append_ir(std::vector<Frame>& result,
                   const std::shared_ptr<ob::FrameSet>& frameset,
                   OBFrameType frame_type,
                   Kind kind,
                   const ResolvedVideo& selected) {
        auto raw = frameset->getFrame(frame_type);
        if (!raw) {
            return;
        }
        auto video = raw->as<ob::VideoFrame>();
        const auto sdk_format = video->getFormat();
        const auto expected_format = selected.output_format == Format::Mono8 ? OB_FORMAT_Y8 : OB_FORMAT_Y16;
        if (sdk_format != expected_format || video->getWidth() != options_.width || video->getHeight() != options_.height) {
            throw std::runtime_error("SDK IR frame does not match the selected Y8/Y16 profile");
        }
        const auto bytes_per_pixel = selected.output_format == Format::Mono8 ? 1U : 2U;
        result.push_back(make_video_frame(kind, selected.output_format, video, bytes_per_pixel));
    }

    void append_points(std::vector<Frame>& result, const std::shared_ptr<ob::DepthFrame>& depth_frame) {
        auto generated = point_cloud_->process(depth_frame);
        if (!generated) {
            return;
        }
        auto points = generated->as<ob::PointsFrame>();
        const auto width = points->getWidth();
        const auto height = points->getHeight();
        const auto data_size = static_cast<std::size_t>(points->getDataSize());
        if (width == 0 || height == 0
            || static_cast<std::size_t>(width) > kMaximumPayloadBytes / (3U * sizeof(float))) {
            throw std::runtime_error("SDK point cloud frame is malformed or exceeds the frame limit");
        }
        const auto row_size = static_cast<std::size_t>(width) * 3U * sizeof(float);
        if (height > kMaximumPayloadBytes / row_size) {
            throw std::runtime_error("SDK point cloud frame is malformed or exceeds the frame limit");
        }
        const auto expected_size = row_size * height;
        if (points->getFormat() != OB_FORMAT_POINT || width != depth_frame->getWidth()
            || height != depth_frame->getHeight()
            || data_size != expected_size || data_size > kMaximumPayloadBytes) {
            throw std::runtime_error("SDK point cloud frame is malformed or exceeds the frame limit");
        }
        if (points->getIndex() != depth_frame->getIndex()) {
            throw std::runtime_error("SDK point cloud frame index does not match the processed depth frame");
        }
        if (points->getTimeStampUs() == 0 || points->getTimeStampUs() != depth_frame->getTimeStampUs()) {
            throw std::runtime_error("SDK point cloud timestamp does not match the processed depth frame");
        }
        auto* source = points->getData();
        if (source == nullptr) {
            throw std::runtime_error("SDK point cloud frame has no data");
        }
        const auto value_scale_m = static_cast<double>(points->getCoordinateValueScale()) / 1000.0;
        if (!std::isfinite(value_scale_m) || value_scale_m <= 0.0) {
            throw std::runtime_error("SDK returned an invalid point cloud coordinate scale");
        }
        auto values = std::make_shared<std::vector<float>>(data_size / sizeof(float));
        std::memcpy(values->data(), source, data_size);
        for (auto& coordinate : *values) {
            coordinate = static_cast<float>(static_cast<double>(coordinate) * value_scale_m);
        }
        Frame frame;
        frame.kind = Kind::Points;
        frame.format = Format::Xyz32f;
        frame.width = width;
        frame.height = height;
        frame.stride = width * 3U * static_cast<std::uint32_t>(sizeof(float));
        frame.index = points->getIndex();
        frame.capture_us = points->getTimeStampUs();
        frame.unit_scale = 1.0;
        frame.data = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(values->data()), data_size);
        frame.owner = std::static_pointer_cast<const void>(values);
        result.push_back(std::move(frame));
    }

    std::shared_ptr<ob::Context> context_;
    std::shared_ptr<ob::Device> device_;
    Options options_;
    Calibration calibration_;
    std::optional<ResolvedVideo> color_;
    std::optional<ResolvedVideo> depth_;
    std::optional<VideoCalibration> depth_output_calibration_;
    std::optional<ResolvedVideo> left_ir_;
    std::optional<ResolvedVideo> right_ir_;
    std::shared_ptr<ob::Sensor> accel_sensor_;
    std::shared_ptr<ob::Sensor> gyro_sensor_;
    std::shared_ptr<ob::StreamProfile> accel_profile_;
    std::shared_ptr<ob::StreamProfile> gyro_profile_;
    std::shared_ptr<ob::PointCloudFilter> point_cloud_;
    std::shared_ptr<ob::FormatConvertFilter> color_converter_;
    std::shared_ptr<ob::Align> align_;
    std::vector<std::shared_ptr<ob::Filter>> depth_filter_chain_;
    std::shared_ptr<ob::Pipeline> pipeline_;
    std::shared_ptr<detail::ImuQueue> imu_queue_ { std::make_shared<detail::ImuQueue>() };
    std::vector<Frame> pending_batch_;
    std::mutex pipeline_mutex_;
    std::mutex poll_mutex_;
    std::mutex imu_lifecycle_mutex_;
    bool imu_start_attempted_ { false };
    bool accel_start_attempted_ { false };
    bool gyro_start_attempted_ { false };
    bool start_attempted_ { false };
    bool started_ { false };
    bool stopped_ { false };
    bool pending_batch_available_ { false };
};

} // namespace

std::vector<DeviceInfo> list_devices() {
    auto context = std::make_shared<ob::Context>();
    auto devices = context->queryDeviceList();
    std::vector<DeviceInfo> result;
    result.reserve(devices->getCount());
    for (std::uint32_t index = 0; index < devices->getCount(); ++index) {
        const auto entry = device_entry(*devices, index);
        result.push_back(DeviceInfo {
            entry.name,
            entry.serial,
            {}, // Firmware is only available from an opened SDK DeviceInfo object.
            entry.connection,
            static_cast<std::uint32_t>(entry.product_id),
        });
    }
    return result;
}

std::unique_ptr<CaptureDevice> open_device(const Options& options) {
    require_supported_sdk_version();
    validate_options(options);
    auto context = std::make_shared<ob::Context>();
    auto devices = context->queryDeviceList();
    const auto entry = require_serial(*devices, options.serial);
    require_gemini335(entry);
    auto device = devices->getDevice(entry.index);
    auto info = read_device_info(entry, device);
    return std::make_unique<OrbbecCaptureDevice>(std::move(context), std::move(device), std::move(info), options);
}

std::string sdk_version() {
    const auto packed_version = ob::Version::getVersion();
    std::ostringstream text;
    text << (packed_version / 10000) << '.' << ((packed_version / 100) % 100) << '.' << (packed_version % 100);
    return text.str();
}

std::string describe_profiles(const std::string& serial) {
    require_supported_sdk_version();
    auto context = std::make_shared<ob::Context>();
    auto devices = context->queryDeviceList();
    const auto entry = require_serial(*devices, serial);
    require_gemini335(entry);
    auto device = devices->getDevice(entry.index);
    const auto info = read_device_info(entry, device);
    auto sensors = device->getSensorList();

    std::ostringstream result;
    result << "{\"serial\":" << json_quote(info.serial)
           << ",\"name\":" << json_quote(info.name)
           << ",\"profiles\":[";
    bool first = true;
    append_video_profile_json(result, first, sensors, Kind::Color, "color", OB_SENSOR_COLOR, OB_STREAM_COLOR);
    append_video_profile_json(result, first, sensors, Kind::Depth, "depth", OB_SENSOR_DEPTH, OB_STREAM_DEPTH);
    append_video_profile_json(result, first, sensors, Kind::LeftIr, "left_ir", OB_SENSOR_IR_LEFT, OB_STREAM_IR_LEFT);
    append_video_profile_json(result, first, sensors, Kind::RightIr, "right_ir", OB_SENSOR_IR_RIGHT, OB_STREAM_IR_RIGHT);
    result << "],\"imu\":{\"accel\":"
           << (has_imu_profile(sensors, OB_SENSOR_ACCEL, OB_STREAM_ACCEL) ? "true" : "false")
           << ",\"gyro\":"
           << (has_imu_profile(sensors, OB_SENSOR_GYRO, OB_STREAM_GYRO) ? "true" : "false")
           << "}}";
    return result.str();
}

std::string describe_filters(const std::string& serial) {
    require_supported_sdk_version();
    auto context = std::make_shared<ob::Context>();
    auto devices = context->queryDeviceList();
    const auto entry = require_serial(*devices, serial);
    require_gemini335(entry);
    auto device = devices->getDevice(entry.index);
    const auto info = read_device_info(entry, device);
    auto sensor = find_sensor(device->getSensorList(), OB_SENSOR_DEPTH);
    if (!sensor) {
        throw std::runtime_error("selected Gemini 335 does not expose a depth sensor");
    }
    const auto filters = recommended_depth_filters(sensor);

    std::ostringstream result;
    result << "{\"serial\":" << json_quote(info.serial)
           << ",\"name\":" << json_quote(info.name)
           << ",\"filters\":[";
    bool first = true;
    for (const auto& filter : filters) {
        append_filter_json(result, filter, first);
    }
    result << "]}";
    return result.str();
}

} // namespace rbt_orbbec

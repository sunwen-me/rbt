#include <rbt/time.hpp>
#include <rbt/time_stream.hpp>
#include <rbt/tf_math.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <optional>
#include <poll.h>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

constexpr std::size_t kLegacyApplicationPrefixBytes = 48U;
constexpr std::size_t kFrameApplicationPrefixBytes = 112U;
constexpr std::size_t kMaximumV4l2Buffers = 4U;
constexpr std::uint32_t kRequestedWidth = 640U;
constexpr std::uint32_t kRequestedHeight = 480U;

struct Options {
    std::string device;
    std::string service;
    std::uint64_t duration_seconds { 0U };
    std::optional<std::uint64_t> restart_after_seconds;
    std::optional<std::string> frame_id;
};

[[nodiscard]] std::uint64_t parse_positive_seconds(std::string_view value,
                                                    std::string_view option) {
    std::uint64_t parsed = 0U;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc {} || result.ptr != value.data() + value.size() || parsed == 0U
        || parsed > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 1'000'000'000LL)) {
        throw std::runtime_error(std::string(option) + " must be a positive number of seconds");
    }
    return parsed;
}

[[nodiscard]] Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if ((argument == "--device" || argument == "--service" || argument == "--duration-sec"
             || argument == "--frame-id"
             || argument == "--restart-after-sec")
            && index + 1 >= argc) {
            throw std::runtime_error(std::string(argument) + " requires a value");
        }
        if (argument == "--device") {
            options.device = argv[++index];
        } else if (argument == "--service") {
            options.service = argv[++index];
        } else if (argument == "--duration-sec") {
            options.duration_seconds = parse_positive_seconds(argv[++index], argument);
        } else if (argument == "--frame-id") {
            options.frame_id = argv[++index];
            (void)rbt::tf::FrameId(*options.frame_id);
        } else if (argument == "--restart-after-sec") {
            options.restart_after_seconds = parse_positive_seconds(argv[++index], argument);
        } else if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: rbt-time-camera --device PATH --service NAME --duration-sec N"
                         " [--restart-after-sec N] [--frame-id NAME]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + std::string(argument));
        }
    }
    if (options.device.empty() || options.service.empty() || options.duration_seconds == 0U) {
        throw std::runtime_error("--device, --service, and --duration-sec are required");
    }
    if (options.restart_after_seconds.has_value()
        && *options.restart_after_seconds >= options.duration_seconds) {
        throw std::runtime_error("--restart-after-sec must be less than --duration-sec");
    }
    return options;
}

[[nodiscard]] int ioctl_retry(int descriptor, unsigned long request, void* argument) noexcept {
    int result = 0;
    do {
        result = ::ioctl(descriptor, request, argument);
    } while (result < 0 && errno == EINTR);
    return result;
}

[[nodiscard]] std::runtime_error system_error(std::string_view action) {
    return std::runtime_error(std::string(action) + ": " + std::strerror(errno));
}

[[nodiscard]] std::string c_string(const std::uint8_t* value, std::size_t capacity) {
    const auto* characters = reinterpret_cast<const char*>(value);
    const auto length = ::strnlen(characters, capacity);
    return std::string(characters, length);
}

[[nodiscard]] std::string json_quote(std::string_view value) {
    std::string output;
    output.reserve(value.size() + 2U);
    output.push_back('"');
    for (const auto character : value) {
        switch (character) {
        case '"':
            output.append("\\\"");
            break;
        case '\\':
            output.append("\\\\");
            break;
        case '\n':
            output.append("\\n");
            break;
        case '\r':
            output.append("\\r");
            break;
        case '\t':
            output.append("\\t");
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                constexpr char digits[] = "0123456789abcdef";
                output.append("\\u00");
                output.push_back(digits[(static_cast<unsigned char>(character) >> 4U) & 0x0FU]);
                output.push_back(digits[static_cast<unsigned char>(character) & 0x0FU]);
            } else {
                output.push_back(character);
            }
        }
    }
    output.push_back('"');
    return output;
}

[[nodiscard]] std::string fourcc_string(std::uint32_t fourcc) {
    std::string output(4U, '\0');
    for (std::size_t index = 0U; index < output.size(); ++index) {
        const auto value = static_cast<unsigned char>((fourcc >> (index * 8U)) & 0xFFU);
        output[index] = value >= 0x20U && value <= 0x7EU ? static_cast<char>(value) : '?';
    }
    return output;
}

[[nodiscard]] std::optional<std::int64_t> timestamp_ns(const timeval& timestamp) noexcept {
    constexpr std::uint64_t kNanosPerSecond = 1'000'000'000ULL;
    constexpr std::uint64_t kNanosPerMicrosecond = 1'000ULL;
    if (timestamp.tv_sec < 0 || timestamp.tv_usec < 0 || timestamp.tv_usec >= 1'000'000) {
        return {};
    }
    const auto seconds = static_cast<std::uint64_t>(timestamp.tv_sec);
    const auto maximum_ns = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (seconds > maximum_ns / kNanosPerSecond) {
        return {};
    }
    const auto whole_seconds_ns = seconds * kNanosPerSecond;
    const auto fractional_ns = static_cast<std::uint64_t>(timestamp.tv_usec) * kNanosPerMicrosecond;
    if (fractional_ns > maximum_ns - whole_seconds_ns) {
        return {};
    }
    return static_cast<std::int64_t>(whole_seconds_ns + fractional_ns);
}

class InvalidTimestamp final : public std::runtime_error {
  public:
    InvalidTimestamp(const timeval& timestamp, std::uint32_t sequence, std::uint32_t flags)
        : std::runtime_error(
              "InvalidTimestamp: sequence=" + std::to_string(sequence)
              + " flags=" + std::to_string(flags)
              + " raw_tv_sec=" + std::to_string(static_cast<long long>(timestamp.tv_sec))
              + " raw_tv_usec=" + std::to_string(static_cast<long long>(timestamp.tv_usec))) {}
};

[[nodiscard]] std::optional<std::int64_t> checked_subtract(std::int64_t left,
                                                           std::int64_t right) noexcept {
    if ((right > 0 && left < std::numeric_limits<std::int64_t>::min() + right)
        || (right < 0 && left > std::numeric_limits<std::int64_t>::max() + right)) {
        return {};
    }
    return left - right;
}

struct NonnegativeLatencyStats {
    std::uint64_t count { 0U };
    std::int64_t sum_ns { 0 };
    bool mean_known { true };
    std::optional<std::int64_t> minimum_ns;
    std::optional<std::int64_t> maximum_ns;

    void add(std::int64_t latency_ns) noexcept {
        ++count;
        if (mean_known) {
            if (latency_ns > std::numeric_limits<std::int64_t>::max() - sum_ns) {
                mean_known = false;
            } else {
                sum_ns += latency_ns;
            }
        }
        minimum_ns = minimum_ns.has_value() ? std::min(*minimum_ns, latency_ns) : latency_ns;
        maximum_ns = maximum_ns.has_value() ? std::max(*maximum_ns, latency_ns) : latency_ns;
    }

    [[nodiscard]] std::optional<std::uint64_t> mean_ns() const noexcept {
        if (count == 0U || !mean_known) {
            return {};
        }
        return static_cast<std::uint64_t>(sum_ns) / count;
    }

    [[nodiscard]] std::string_view mean_status() const noexcept {
        if (!mean_known) {
            return "sum_overflow";
        }
        return count == 0U ? "no_valid_samples" : "known";
    }
};

struct Mapping {
    void* address { MAP_FAILED };
    std::size_t length { 0U };
};

struct CapturedFrame {
    std::uint32_t index { 0U };
    std::uint32_t sequence { 0U };
    std::uint32_t flags { 0U };
    std::uint32_t bytes_used { 0U };
    std::int64_t raw_timestamp_ns { 0 };
    std::span<const std::uint8_t> bytes;
};

constexpr bool is_sequence_discontinuity(std::uint32_t previous, std::uint32_t current) {
    const auto delta = static_cast<std::uint32_t>(current - previous);
    return delta == 0U || delta >= 0x80000000U;
}

// The 627 -> 0 case was observed on the selected physical UVC camera.
static_assert(is_sequence_discontinuity(627U, 0U));
static_assert(is_sequence_discontinuity(10U, 10U));
static_assert(!is_sequence_discontinuity(std::numeric_limits<std::uint32_t>::max(), 0U));
static_assert(!is_sequence_discontinuity(0U, 1U));

class V4l2Capture {
  public:
    explicit V4l2Capture(std::string device)
        : device_(std::move(device)) {
        open_and_start();
    }

    V4l2Capture(const V4l2Capture&) = delete;
    V4l2Capture& operator=(const V4l2Capture&) = delete;

    ~V4l2Capture() {
        close_session();
    }

    void reopen() {
        close_session();
        confirm_sequence_discontinuity();
        open_and_start();
    }

    // A V4L2 sequence discontinuity is evidence that the driver sequence
    // association ended. It does not prove a physical reset or a host-clock step.
    void confirm_sequence_discontinuity() {
        if (session_generation_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::runtime_error("driver-session generation overflow");
        }
        ++session_generation_;
    }

    [[nodiscard]] const v4l2_capability& capability() const noexcept {
        return capability_;
    }

    [[nodiscard]] const v4l2_pix_format& format() const noexcept {
        return format_;
    }

    [[nodiscard]] std::optional<std::pair<std::uint32_t, std::uint32_t>> frame_interval() const noexcept {
        return frame_interval_;
    }

    [[nodiscard]] std::uint64_t session_generation() const noexcept {
        return session_generation_;
    }

    [[nodiscard]] int poll_frame(int timeout_ms, CapturedFrame& frame) {
        pollfd descriptor { fd_, POLLIN | POLLPRI, 0 };
        int result = 0;
        do {
            result = ::poll(&descriptor, 1U, timeout_ms);
        } while (result < 0 && errno == EINTR);
        if (result < 0) {
            throw system_error("poll V4L2 frame");
        }
        if (result == 0) {
            return 0;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            throw std::runtime_error("V4L2 poll reported a device error");
        }
        v4l2_buffer buffer {};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        if (ioctl_retry(fd_, VIDIOC_DQBUF, &buffer) < 0) {
            if (errno == EAGAIN) {
                return 0;
            }
            throw system_error("dequeue V4L2 frame");
        }
        if (buffer.index >= mappings_.size() || buffer.bytesused > mappings_[buffer.index].length) {
            throw std::runtime_error("V4L2 returned an invalid buffer index or byte count");
        }
        frame.index = buffer.index;
        frame.sequence = buffer.sequence;
        frame.flags = buffer.flags;
        frame.bytes_used = buffer.bytesused;
        const auto raw_timestamp_ns = timestamp_ns(buffer.timestamp);
        if (!raw_timestamp_ns.has_value()) {
            throw InvalidTimestamp(buffer.timestamp, buffer.sequence, buffer.flags);
        }
        frame.raw_timestamp_ns = *raw_timestamp_ns;
        frame.bytes = std::span<const std::uint8_t>(
            static_cast<const std::uint8_t*>(mappings_[buffer.index].address), buffer.bytesused);
        return 1;
    }

    void requeue(std::uint32_t index) {
        v4l2_buffer buffer {};
        buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = index;
        if (ioctl_retry(fd_, VIDIOC_QBUF, &buffer) < 0) {
            throw system_error("requeue V4L2 frame");
        }
    }

  private:
    void open_and_start() {
        fd_ = ::open(device_.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) {
            throw system_error("open " + device_);
        }
        try {
            if (ioctl_retry(fd_, VIDIOC_QUERYCAP, &capability_) < 0) {
                throw system_error("query V4L2 capabilities");
            }
            const auto caps = (capability_.capabilities & V4L2_CAP_DEVICE_CAPS) != 0U
                ? capability_.device_caps
                : capability_.capabilities;
            if ((caps & V4L2_CAP_VIDEO_CAPTURE) == 0U || (caps & V4L2_CAP_STREAMING) == 0U) {
                throw std::runtime_error("device must support single-plane V4L2 video capture and streaming");
            }

            v4l2_format requested {};
            requested.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            requested.fmt.pix.width = kRequestedWidth;
            requested.fmt.pix.height = kRequestedHeight;
            requested.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
            requested.fmt.pix.field = V4L2_FIELD_ANY;
            if (ioctl_retry(fd_, VIDIOC_S_FMT, &requested) < 0) {
                throw system_error("request YUYV 640x480");
            }
            v4l2_format actual {};
            actual.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (ioctl_retry(fd_, VIDIOC_G_FMT, &actual) < 0) {
                throw system_error("read negotiated V4L2 format");
            }
            format_ = actual.fmt.pix;
            if (format_.pixelformat != V4L2_PIX_FMT_YUYV || format_.width == 0U || format_.height == 0U
                || format_.bytesperline == 0U || format_.sizeimage == 0U) {
                throw std::runtime_error("device did not negotiate a valid YUYV image format");
            }

            v4l2_streamparm parameters {};
            parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (ioctl_retry(fd_, VIDIOC_G_PARM, &parameters) == 0
                && parameters.parm.capture.timeperframe.numerator != 0U
                && parameters.parm.capture.timeperframe.denominator != 0U) {
                frame_interval_ = std::pair { parameters.parm.capture.timeperframe.numerator,
                                              parameters.parm.capture.timeperframe.denominator };
            }

            v4l2_requestbuffers request {};
            request.count = static_cast<std::uint32_t>(kMaximumV4l2Buffers);
            request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            request.memory = V4L2_MEMORY_MMAP;
            if (ioctl_retry(fd_, VIDIOC_REQBUFS, &request) < 0) {
                throw system_error("request V4L2 mmap buffers");
            }
            if (request.count < 2U) {
                throw std::runtime_error("V4L2 driver supplied fewer than two mmap buffers");
            }
            mappings_.reserve(request.count);
            for (std::uint32_t index = 0U; index < request.count; ++index) {
                v4l2_buffer buffer {};
                buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                buffer.memory = V4L2_MEMORY_MMAP;
                buffer.index = index;
                if (ioctl_retry(fd_, VIDIOC_QUERYBUF, &buffer) < 0) {
                    throw system_error("query V4L2 mmap buffer");
                }
                void* address = ::mmap(nullptr,
                                       buffer.length,
                                       PROT_READ | PROT_WRITE,
                                       MAP_SHARED,
                                       fd_,
                                       static_cast<off_t>(buffer.m.offset));
                if (address == MAP_FAILED) {
                    throw system_error("map V4L2 buffer");
                }
                mappings_.push_back(Mapping { address, buffer.length });
                if (ioctl_retry(fd_, VIDIOC_QBUF, &buffer) < 0) {
                    throw system_error("queue V4L2 buffer");
                }
            }
            v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            if (ioctl_retry(fd_, VIDIOC_STREAMON, &type) < 0) {
                throw system_error("start V4L2 stream");
            }
            streaming_ = true;
        } catch (...) {
            close_session();
            throw;
        }
    }

    void close_session() noexcept {
        if (fd_ >= 0 && streaming_) {
            auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            (void)ioctl_retry(fd_, VIDIOC_STREAMOFF, &type);
            streaming_ = false;
        }
        for (auto& mapping : mappings_) {
            if (mapping.address != MAP_FAILED) {
                (void)::munmap(mapping.address, mapping.length);
                mapping.address = MAP_FAILED;
            }
        }
        mappings_.clear();
        if (fd_ >= 0) {
            v4l2_requestbuffers request {};
            request.count = 0U;
            request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            request.memory = V4L2_MEMORY_MMAP;
            (void)ioctl_retry(fd_, VIDIOC_REQBUFS, &request);
            (void)::close(fd_);
            fd_ = -1;
        }
        frame_interval_.reset();
    }

    std::string device_;
    int fd_ { -1 };
    bool streaming_ { false };
    std::vector<Mapping> mappings_;
    v4l2_capability capability_ {};
    v4l2_pix_format format_ {};
    std::optional<std::pair<std::uint32_t, std::uint32_t>> frame_interval_;
    std::uint64_t session_generation_ { 0U };
};

[[nodiscard]] std::string timestamp_type(std::uint32_t flags) {
    switch (flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) {
    case V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC:
        return "monotonic";
    case V4L2_BUF_FLAG_TIMESTAMP_COPY:
        return "copy";
    case V4L2_BUF_FLAG_TIMESTAMP_UNKNOWN:
        return "unknown";
    default:
        return "unrecognized";
    }
}

[[nodiscard]] std::string timestamp_source(std::uint32_t flags) {
    switch (flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK) {
    case V4L2_BUF_FLAG_TSTAMP_SRC_SOE:
        return "start_of_exposure";
    case V4L2_BUF_FLAG_TSTAMP_SRC_EOF:
        return "end_of_frame";
    default:
        return "unrecognized";
    }
}

[[nodiscard]] bool zero_monotonic_offset(const rbt::time::HostClock& host_clock) noexcept {
    const auto& bytes = host_clock.monotonic_identity().source_id().bytes();
    return std::all_of(bytes.begin() + 32, bytes.end(), [](std::uint8_t byte) { return byte == 0U; });
}

void store_u32_le(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) noexcept {
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
    }
}

void store_i64_le(std::span<std::uint8_t> bytes, std::size_t offset, std::int64_t value) noexcept {
    const auto raw = std::bit_cast<std::uint64_t>(value);
    for (std::size_t index = 0U; index < sizeof(raw); ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>((raw >> (index * 8U)) & 0xFFU);
    }
}

void store_u64_le(std::span<std::uint8_t> bytes, std::size_t offset, std::uint64_t value) noexcept {
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
    }
}

[[nodiscard]] std::uint64_t fnv1a64(std::span<const std::uint8_t> bytes) noexcept {
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (const auto byte : bytes) {
        hash ^= byte;
        hash *= 1'099'511'628'211ULL;
    }
    return hash;
}

int run(const Options& options) {
    auto host_result = rbt::time::HostClock::create();
    if (!host_result) {
        throw std::runtime_error(std::string("cannot establish the host monotonic clock: ")
                                 + rbt::time::to_string(host_result.error()));
    }
    auto host_clock = std::move(host_result).value();
    const bool monotonic_offset_is_zero = zero_monotonic_offset(host_clock);
    const auto host_clock_source_id = host_clock.monotonic_identity().source_id().to_hex();

    V4l2Capture camera(options.device);
    const auto& capability = camera.capability();
    const auto& format = camera.format();
    const auto application_prefix_bytes = options.frame_id.has_value()
        ? kFrameApplicationPrefixBytes
        : kLegacyApplicationPrefixBytes;
    const auto overhead = rbt::time::kStampedBytesPrefixBytes + application_prefix_bytes;
    if (format.sizeimage > std::numeric_limits<std::size_t>::max() - overhead) {
        throw std::runtime_error("negotiated V4L2 image size overflows the stamped sample size");
    }
    const std::size_t maximum_elements = overhead + static_cast<std::size_t>(format.sizeimage);
    const rbt::StreamDescriptor stream(
        options.service, rbt::time::STAMPED_BYTES_DESCRIPTOR, rbt::StreamProfile::Sensor);
    auto publisher_result = rbt::time::create_stamped_bytes_publisher(stream, maximum_elements);
    if (!publisher_result) {
        throw std::runtime_error("create StampedBytes publisher failed: "
                                 + std::string(rbt::to_string(publisher_result.error())));
    }
    auto publisher = std::move(publisher_result).value();

    const auto frame_rate = camera.frame_interval();
    std::cout << "{\"event\":\"device\",\"device\":" << json_quote(options.device)
              << ",\"service\":" << json_quote(options.service)
              << ",\"application_format\":" << json_quote(options.frame_id ? "RBTVCAM2" : "RBTVCAM1")
              << ",\"frame_id\":" << (options.frame_id ? json_quote(*options.frame_id) : "null")
              << ",\"driver\":" << json_quote(c_string(capability.driver, sizeof(capability.driver)))
              << ",\"card\":" << json_quote(c_string(capability.card, sizeof(capability.card)))
              << ",\"bus_info\":" << json_quote(c_string(capability.bus_info, sizeof(capability.bus_info)))
              << ",\"kernel_version\":" << capability.version
              << ",\"capabilities\":" << capability.capabilities
              << ",\"device_capabilities\":" << capability.device_caps
              << ",\"width\":" << format.width << ",\"height\":" << format.height
              << ",\"stride\":" << format.bytesperline
              << ",\"fourcc\":" << json_quote(fourcc_string(format.pixelformat))
              << ",\"sizeimage\":" << format.sizeimage << ",\"fps_numerator\":"
              << (frame_rate.has_value() ? frame_rate->first : 0U) << ",\"fps_denominator\":"
              << (frame_rate.has_value() ? frame_rate->second : 0U)
              << ",\"host_monotonic_offset_zero\":"
              << (monotonic_offset_is_zero ? "true" : "false")
              << ",\"host_clock_source_id\":"
              << json_quote(host_clock_source_id)
              << ",\"capture_policy\":\"V4L2 MONOTONIC+SOE and zero monotonic time-namespace offset only\"}\n"
              << std::flush;

    const auto start = std::chrono::steady_clock::now();
    const auto requested_duration = std::chrono::seconds(options.duration_seconds);
    if (requested_duration > std::chrono::steady_clock::time_point::max() - start) {
        throw std::runtime_error("--duration-sec exceeds the steady-clock deadline range");
    }
    const auto deadline = start + requested_duration;
    const auto restart_at = options.restart_after_seconds.has_value()
        ? std::optional<std::chrono::steady_clock::time_point>(
              start + std::chrono::seconds(*options.restart_after_seconds))
        : std::nullopt;
    bool restarted = false;
    std::uint64_t frames_dequeued = 0U;
    std::uint64_t published = 0U;
    std::uint64_t capture_known = 0U;
    std::uint64_t driver_to_receive_negative_count = 0U;
    std::uint64_t driver_to_receive_arithmetic_invalid_count = 0U;
    NonnegativeLatencyStats driver_to_receive_stats;
    NonnegativeLatencyStats capture_to_receive_stats;
    std::uint64_t capture_to_receive_invalid_count = 0U;
    std::uint64_t driver_sequence_gap_count = 0U;
    std::uint64_t driver_sequence_duplicate_count = 0U;
    std::uint64_t driver_sequence_out_of_order_count = 0U;
    std::uint64_t driver_timestamp_forward_step_count = 0U;
    std::uint64_t driver_timestamp_backwards_count = 0U;
    std::uint64_t driver_timestamp_equal_count = 0U;
    std::optional<std::uint32_t> previous_driver_sequence;
    std::optional<std::int64_t> previous_driver_timestamp_ns;
    std::optional<std::uint32_t> previous_driver_timestamp_flags;

    while (std::chrono::steady_clock::now() < deadline) {
        const auto loop_now = std::chrono::steady_clock::now();
        if (restart_at.has_value() && !restarted && loop_now >= *restart_at) {
            camera.reopen();
            restarted = true;
            previous_driver_sequence.reset();
            previous_driver_timestamp_ns.reset();
            previous_driver_timestamp_flags.reset();
            std::cout << "{\"event\":\"driver_session_reopened\",\"session_generation\":"
                      << camera.session_generation()
                      << ",\"prior_device_timestamp_association\":\"invalidated\","
                         "\"host_clock_generation_changed\":false}\n"
                      << std::flush;
        }

        CapturedFrame frame;
        if (camera.poll_frame(250, frame) == 0) {
            continue;
        }
        ++frames_dequeued;
        if (previous_driver_sequence.has_value()) {
            const auto delta = static_cast<std::uint32_t>(frame.sequence - *previous_driver_sequence);
            if (delta == 0U) {
                ++driver_sequence_duplicate_count;
            } else if (delta < 0x80000000U) {
                driver_sequence_gap_count += static_cast<std::uint64_t>(delta - 1U);
            } else {
                ++driver_sequence_out_of_order_count;
            }
            if (is_sequence_discontinuity(*previous_driver_sequence, frame.sequence)) {
                camera.confirm_sequence_discontinuity();
                std::cout << "{\"event\":\"driver_sequence_discontinuity\",\"previous_sequence\":"
                          << *previous_driver_sequence << ",\"current_sequence\":" << frame.sequence
                          << ",\"session_generation\":" << camera.session_generation()
                          << ",\"cause\":\"unconfirmed\",\"host_clock_generation_changed\":false,"
                             "\"prior_device_timestamp_association\":\"invalidated\"}\n"
                          << std::flush;
                previous_driver_timestamp_ns.reset();
                previous_driver_timestamp_flags.reset();
            }
        }
        previous_driver_sequence = frame.sequence;

        const auto driver_timestamp_flags = frame.flags
            & (V4L2_BUF_FLAG_TIMESTAMP_MASK | V4L2_BUF_FLAG_TSTAMP_SRC_MASK);
        if (previous_driver_timestamp_ns.has_value()
            && previous_driver_timestamp_flags == driver_timestamp_flags) {
            if (frame.raw_timestamp_ns > *previous_driver_timestamp_ns) {
                ++driver_timestamp_forward_step_count;
            } else if (frame.raw_timestamp_ns < *previous_driver_timestamp_ns) {
                ++driver_timestamp_backwards_count;
            } else {
                ++driver_timestamp_equal_count;
            }
        }
        previous_driver_timestamp_ns = frame.raw_timestamp_ns;
        previous_driver_timestamp_flags = driver_timestamp_flags;

        const auto received_result = host_clock.now_monotonic();
        if (!received_result) {
            throw std::runtime_error(std::string("read host monotonic receive time failed: ")
                                     + rbt::time::to_string(received_result.error()));
        }
        auto received = std::move(received_result).value();
        const auto v4l2_type = frame.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK;
        const auto v4l2_source = frame.flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK;
        const bool monotonic_timestamp = v4l2_type == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
        const bool start_of_exposure = v4l2_source == V4L2_BUF_FLAG_TSTAMP_SRC_SOE;
        const bool comparable_driver_event = monotonic_timestamp && monotonic_offset_is_zero;
        const bool capture_present = comparable_driver_event && start_of_exposure;
        std::optional<rbt::time::Timestamp> capture;
        if (capture_present) {
            auto timestamp_result = rbt::time::Timestamp::create(
                host_clock.monotonic_identity(), frame.raw_timestamp_ns);
            if (!timestamp_result) {
                throw std::runtime_error("construct V4L2 capture stamp failed: "
                                         + std::string(rbt::time::to_string(timestamp_result.error())));
            }
            capture = std::move(timestamp_result).value();
            ++capture_known;
        }
        auto timing_result = rbt::time::SensorTiming::create(
            capture,
            received,
            {},
            {});
        if (!timing_result) {
            throw std::runtime_error("construct stamped sensor timing failed: "
                                     + std::string(rbt::time::to_string(timing_result.error())));
        }

        if (frame.bytes.size() > format.sizeimage) {
            throw std::runtime_error("V4L2 frame exceeds the negotiated sizeimage");
        }
        const auto sample_elements = rbt::time::kStampedBytesPrefixBytes + application_prefix_bytes
            + frame.bytes.size();
        auto loan_result = publisher.loan_slice(sample_elements);
        if (!loan_result) {
            throw std::runtime_error("loan StampedBytes sample failed: "
                                     + std::string(rbt::to_string(loan_result.error())));
        }
        auto loan = std::move(loan_result).value();
        loan.header() = frame.sequence;
        auto payload = loan.payload();
        if (rbt::time::write_timing_prefix(payload, timing_result.value())
            != rbt::time::PrefixCodecError::None) {
            throw std::runtime_error("encode StampedBytes timing prefix failed");
        }
        auto application = payload.subspan(rbt::time::kStampedBytesPrefixBytes, application_prefix_bytes);
        constexpr std::array<std::uint8_t, 8> kCameraV1Magic { 'R', 'B', 'T', 'V', 'C', 'A', 'M', '1' };
        constexpr std::array<std::uint8_t, 8> kCameraV2Magic { 'R', 'B', 'T', 'V', 'C', 'A', 'M', '2' };
        const auto& camera_magic = options.frame_id ? kCameraV2Magic : kCameraV1Magic;
        std::copy(camera_magic.begin(), camera_magic.end(), application.begin());
        store_u32_le(application, 8U, format.width);
        store_u32_le(application, 12U, format.height);
        store_u32_le(application, 16U, format.bytesperline);
        store_u32_le(application, 20U, format.pixelformat);
        store_i64_le(application, 24U, frame.raw_timestamp_ns);
        store_u32_le(application, 32U, frame.flags);
        store_u32_le(application, 36U, 0U);
        store_u64_le(application, 40U, camera.session_generation());
        if (options.frame_id) {
            const auto frame_id = rbt::tf::FrameId(*options.frame_id).to_bytes();
            std::copy(frame_id.begin(), frame_id.end(), application.begin() + 48);
        }
        std::copy(frame.bytes.begin(),
                  frame.bytes.end(),
                  payload.begin()
                      + static_cast<std::ptrdiff_t>(rbt::time::kStampedBytesPrefixBytes
                                                    + application_prefix_bytes));

        auto sent = loan.send();
        if (!sent) {
            throw std::runtime_error("send StampedBytes sample failed: "
                                     + std::string(rbt::to_string(sent.error())));
        }

        std::optional<std::int64_t> driver_to_receive_ns;
        std::string driver_to_receive_status = "not_comparable_clock_basis";
        if (comparable_driver_event) {
            if (received.ns < frame.raw_timestamp_ns) {
                ++driver_to_receive_negative_count;
                driver_to_receive_status = "negative_latency_rejected";
            } else {
                driver_to_receive_ns = checked_subtract(received.ns, frame.raw_timestamp_ns);
            }
            if (driver_to_receive_ns.has_value()) {
                driver_to_receive_status = "nonnegative_comparable_latency";
                driver_to_receive_stats.add(*driver_to_receive_ns);
            } else if (driver_to_receive_status != "negative_latency_rejected") {
                ++driver_to_receive_arithmetic_invalid_count;
                driver_to_receive_status = "latency_subtraction_overflow";
            }
        }
        std::optional<std::int64_t> capture_to_receive_ns;
        if (capture_present) {
            if (driver_to_receive_ns.has_value()) {
                capture_to_receive_ns = *driver_to_receive_ns;
                capture_to_receive_stats.add(*capture_to_receive_ns);
            } else {
                ++capture_to_receive_invalid_count;
            }
        }
        const std::string capture_status = capture_present
            ? "start_of_exposure_host_monotonic"
            : (!monotonic_timestamp
                   ? "unknown_or_non_monotonic_driver_clock"
                   : (v4l2_source == V4L2_BUF_FLAG_TSTAMP_SRC_EOF
                          ? "driver_event_is_end_of_frame"
                          : (!start_of_exposure
                                 ? "driver_timestamp_source_unrecognized"
                                 : "monotonic_namespace_offset_not_proven_zero")));
        std::cout << "{\"event\":\"frame_published\",\"sequence\":" << frame.sequence
                  << ",\"bytes\":" << frame.bytes_used << ",\"raw_driver_timestamp_ns\":"
                  << frame.raw_timestamp_ns << ",\"driver_timestamp_valid\":true"
                  << ",\"driver_timestamp_type\":" << json_quote(timestamp_type(frame.flags))
                  << ",\"driver_timestamp_source\":" << json_quote(timestamp_source(frame.flags))
                  << ",\"driver_flags\":" << frame.flags << ",\"capture_status\":"
                  << json_quote(capture_status) << ",\"capture_present\":"
                  << (capture_present ? "true" : "false") << ",\"receive_ns\":" << received.ns
                  << ",\"receive_clock_source_id\":"
                  << json_quote(host_clock_source_id)
                  << ",\"receive_uncertainty_ns\":null,\"driver_to_receive_ns\":";
        if (driver_to_receive_ns.has_value()) {
            std::cout << *driver_to_receive_ns;
        } else {
            std::cout << "null";
        }
        std::cout << ",\"driver_to_receive_status\":" << json_quote(driver_to_receive_status)
                  << ",\"capture_to_receive_ns\":";
        if (capture_to_receive_ns.has_value()) {
            std::cout << *capture_to_receive_ns;
        } else {
            std::cout << "null";
        }
        std::cout << ",\"driver_session_generation\":" << camera.session_generation();
        if (options.frame_id) {
            std::cout << ",\"frame_id\":" << json_quote(*options.frame_id)
                      << ",\"capture_ns\":";
            if (capture.has_value()) {
                std::cout << capture->ns;
            } else {
                std::cout << "null";
            }
            std::cout << ",\"sample_checksum_fnv1a64\":" << fnv1a64(frame.bytes);
        }
        std::cout << "}\n" << std::flush;
        ++published;
        camera.requeue(frame.index);
    }

    const auto actual_elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
    std::cout << "{\"event\":\"producer_summary\",\"actual_elapsed_ms\":" << actual_elapsed_ms
              << ",\"frames_dequeued\":" << frames_dequeued << ",\"published\":" << published
              << ",\"capture_known\":" << capture_known
              << ",\"capture_unknown\":" << (published - capture_known)
              << ",\"driver_to_receive_count\":" << driver_to_receive_stats.count
              << ",\"driver_to_receive_negative_count\":" << driver_to_receive_negative_count
              << ",\"driver_to_receive_arithmetic_invalid_count\":"
              << driver_to_receive_arithmetic_invalid_count
              << ",\"driver_to_receive_min_ns\":";
    if (driver_to_receive_stats.minimum_ns.has_value()) {
        std::cout << *driver_to_receive_stats.minimum_ns;
    } else {
        std::cout << "null";
    }
    std::cout << ",\"driver_to_receive_max_ns\":";
    if (driver_to_receive_stats.maximum_ns.has_value()) {
        std::cout << *driver_to_receive_stats.maximum_ns;
    } else {
        std::cout << "null";
    }
    std::cout << ",\"driver_to_receive_mean_ns\":";
    const auto driver_to_receive_mean_ns = driver_to_receive_stats.mean_ns();
    if (driver_to_receive_mean_ns.has_value()) {
        std::cout << *driver_to_receive_mean_ns;
    } else {
        std::cout << "null";
    }
    std::cout << ",\"driver_to_receive_mean_status\":"
              << json_quote(driver_to_receive_stats.mean_status())
              << ",\"capture_to_receive_count\":" << capture_to_receive_stats.count
              << ",\"capture_to_receive_invalid_count\":" << capture_to_receive_invalid_count
              << ",\"capture_to_receive_min_ns\":";
    if (capture_to_receive_stats.minimum_ns.has_value()) {
        std::cout << *capture_to_receive_stats.minimum_ns;
    } else {
        std::cout << "null";
    }
    std::cout << ",\"capture_to_receive_max_ns\":";
    if (capture_to_receive_stats.maximum_ns.has_value()) {
        std::cout << *capture_to_receive_stats.maximum_ns;
    } else {
        std::cout << "null";
    }
    std::cout << ",\"capture_to_receive_mean_ns\":";
    const auto capture_to_receive_mean_ns = capture_to_receive_stats.mean_ns();
    if (capture_to_receive_mean_ns.has_value()) {
        std::cout << *capture_to_receive_mean_ns;
    } else {
        std::cout << "null";
    }
    std::cout << ",\"capture_to_receive_mean_status\":"
              << json_quote(capture_to_receive_stats.mean_status())
              << ",\"driver_sequence_gap_count\":" << driver_sequence_gap_count
              << ",\"driver_sequence_duplicate_count\":" << driver_sequence_duplicate_count
              << ",\"driver_sequence_out_of_order_count\":" << driver_sequence_out_of_order_count
              << ",\"driver_timestamp_forward_step_count\":" << driver_timestamp_forward_step_count
              << ",\"driver_timestamp_backwards_count\":" << driver_timestamp_backwards_count
              << ",\"driver_timestamp_equal_count\":" << driver_timestamp_equal_count
              << ",\"driver_session_generation\":" << camera.session_generation() << "}\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_options(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "rbt-time-camera: " << error.what() << '\n';
        return 1;
    }
}

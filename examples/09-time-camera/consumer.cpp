#include <rbt/time.hpp>
#include <rbt/time_stream.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#include <linux/videodev2.h>

namespace {

constexpr std::size_t kApplicationPrefixBytes = 48U;
constexpr std::uint32_t kYuyvFourcc = V4L2_PIX_FMT_YUYV;

using ReceivedCameraSample = rbt::ReceivedSliceSample<std::uint64_t, std::uint8_t>;
static_assert(std::is_same_v<decltype(std::declval<const ReceivedCameraSample&>().payload()),
                             std::span<const std::uint8_t>>);

struct Options {
    std::string service;
    std::uint64_t duration_seconds { 0U };
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
        if ((argument == "--service" || argument == "--duration-sec") && index + 1 >= argc) {
            throw std::runtime_error(std::string(argument) + " requires a value");
        }
        if (argument == "--service") {
            options.service = argv[++index];
        } else if (argument == "--duration-sec") {
            options.duration_seconds = parse_positive_seconds(argv[++index], argument);
        } else if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: rbt-time-camera-consumer --service NAME --duration-sec N\n";
            std::exit(0);
        } else {
            throw std::runtime_error("unknown argument: " + std::string(argument));
        }
    }
    if (options.service.empty() || options.duration_seconds == 0U) {
        throw std::runtime_error("--service and --duration-sec are required");
    }
    return options;
}

[[nodiscard]] std::uint32_t load_u32_le(std::span<const std::uint8_t> bytes,
                                        std::size_t offset) noexcept {
    std::uint32_t value = 0U;
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint64_t load_u64_le(std::span<const std::uint8_t> bytes,
                                        std::size_t offset) noexcept {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < sizeof(value); ++index) {
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::int64_t load_i64_le(std::span<const std::uint8_t> bytes,
                                       std::size_t offset) noexcept {
    return std::bit_cast<std::int64_t>(load_u64_le(bytes, offset));
}

[[nodiscard]] bool has_magic(std::span<const std::uint8_t> bytes) noexcept {
    constexpr std::array<std::uint8_t, 8> expected { 'R', 'B', 'T', 'V', 'C', 'A', 'M', '1' };
    return bytes.size() >= expected.size()
        && std::equal(expected.begin(), expected.end(), bytes.begin());
}

[[nodiscard]] std::uint64_t fnv1a64(std::span<const std::uint8_t> bytes) noexcept {
    constexpr std::uint64_t kOffsetBasis = 14695981039346656037ULL;
    constexpr std::uint64_t kPrime = 1099511628211ULL;
    auto checksum = kOffsetBasis;
    for (const auto byte : bytes) {
        checksum ^= byte;
        checksum *= kPrime;
    }
    return checksum;
}

[[nodiscard]] std::string hex_u64(std::uint64_t value) {
    std::array<char, 16> digits {};
    constexpr char alphabet[] = "0123456789abcdef";
    for (std::size_t index = 0U; index < digits.size(); ++index) {
        const auto shift = (digits.size() - index - 1U) * 4U;
        digits[index] = alphabet[(value >> shift) & 0x0FU];
    }
    return std::string(digits.begin(), digits.end());
}

[[nodiscard]] const rbt::SubscriberPressure* subscriber_pressure(const rbt::StreamPressure& pressure) noexcept {
    return pressure.subscriber.has_value() ? &*pressure.subscriber : nullptr;
}

[[nodiscard]] std::size_t active_stream_count() {
    return rbt::local_pressure_snapshots().size();
}

void print_pressure_state(std::string_view name, const rbt::StreamPressure& pressure) {
    const auto* subscriber = subscriber_pressure(pressure);
    std::cout << "\"" << name << "\":{\"borrowed_samples\":"
              << (subscriber != nullptr ? subscriber->borrowed_samples : 0U)
              << ",\"borrowed_high_water\":"
              << (subscriber != nullptr ? subscriber->borrowed_high_water : 0U)
              << ",\"borrow_failures\":" << (subscriber != nullptr ? subscriber->borrow_failures : 0U)
              << "}";
}

int run(const Options& options) {
    auto host_result = rbt::time::HostClock::create();
    if (!host_result) {
        throw std::runtime_error(std::string("cannot establish the host monotonic clock: ")
                                 + rbt::time::to_string(host_result.error()));
    }
    auto host_clock = std::move(host_result).value();
    const auto resources_before = active_stream_count();
    const rbt::StreamDescriptor stream(
        options.service, rbt::time::STAMPED_BYTES_DESCRIPTOR, rbt::StreamProfile::Sensor);
    auto subscriber_result = rbt::time::create_stamped_bytes_subscriber(stream);
    if (!subscriber_result) {
        throw std::runtime_error("create StampedBytes subscriber failed: "
                                 + std::string(rbt::to_string(subscriber_result.error())));
    }
    std::optional<rbt::time::StampedBytesSubscriber> subscriber(
        std::move(subscriber_result).value());
    const auto resources_started = active_stream_count();
    const auto pressure_before = subscriber->pressure();
    std::cout << "{\"event\":\"consumer_ready\",\"service\":\"" << options.service
              << "\",\"clock_source_id\":\""
              << host_clock.monotonic_identity().source_id().to_hex()
              << "\",\"subscriber_buffer_size\":" << subscriber->buffer_size() << "}\n"
              << std::flush;

    const auto start = std::chrono::steady_clock::now();
    const auto requested_duration = std::chrono::seconds(options.duration_seconds);
    if (requested_duration > std::chrono::steady_clock::time_point::max() - start) {
        throw std::runtime_error("--duration-sec exceeds the steady-clock deadline range");
    }
    const auto deadline = start + requested_duration;
    std::uint64_t received_count = 0U;
    std::uint64_t dropped_count = 0U;
    std::uint64_t duplicate_count = 0U;
    std::uint64_t out_of_order_count = 0U;
    std::uint64_t capture_known = 0U;
    std::uint64_t capture_unknown = 0U;
    std::set<std::uint64_t> session_generations;
    std::optional<std::uint64_t> previous_sequence;
    std::optional<std::uint64_t> previous_generation;

    while (std::chrono::steady_clock::now() < deadline) {
        auto result = subscriber->receive();
        if (!result) {
            throw std::runtime_error("receive StampedBytes sample failed: "
                                     + std::string(rbt::to_string(result.error())));
        }
        auto maybe_sample = std::move(result).value();
        if (!maybe_sample.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        auto sample = std::move(*maybe_sample);
        const auto sequence = sample.header();
        const auto transport_bytes = sample.payload();
        auto decoded_result = rbt::time::decode_stamped_bytes(transport_bytes);
        if (const auto* error = std::get_if<rbt::time::PrefixCodecError>(&decoded_result)) {
            throw std::runtime_error(std::string("decode StampedBytes prefix failed: ")
                                     + rbt::time::to_string(*error));
        }
        const auto& decoded = std::get<rbt::time::DecodedStampedBytes>(decoded_result);
        if (decoded.bytes.size() < kApplicationPrefixBytes || !has_magic(decoded.bytes)) {
            throw std::runtime_error("invalid camera application prefix");
        }
        const auto app = decoded.bytes.first(kApplicationPrefixBytes);
        const auto width = load_u32_le(app, 8U);
        const auto height = load_u32_le(app, 12U);
        const auto stride = load_u32_le(app, 16U);
        const auto pixel_format = load_u32_le(app, 20U);
        const auto raw_driver_ns = load_i64_le(app, 24U);
        const auto driver_flags = load_u32_le(app, 32U);
        const auto reserved = load_u32_le(app, 36U);
        const auto session_generation = load_u64_le(app, 40U);
        const auto raw_bytes = decoded.bytes.subspan(kApplicationPrefixBytes);
        if (width == 0U || height == 0U
            || static_cast<std::uint64_t>(stride) < static_cast<std::uint64_t>(width) * 2U
            || pixel_format != kYuyvFourcc
            || reserved != 0U || sequence > std::numeric_limits<std::uint32_t>::max()) {
            throw std::runtime_error("camera metadata contains invalid dimensions, format, or reserved bytes");
        }
        const auto minimum_frame_bytes = static_cast<std::uint64_t>(height - 1U) * stride
            + static_cast<std::uint64_t>(width) * 2U;
        const auto maximum_frame_bytes = static_cast<std::uint64_t>(height) * stride;
        if (raw_bytes.size() < minimum_frame_bytes || raw_bytes.size() > maximum_frame_bytes) {
            throw std::runtime_error("camera pixel payload does not match the YUYV dimensions and stride");
        }
        if (decoded.timing.received.clock.kind() != rbt::time::ClockKind::HostMonotonic
            || decoded.timing.received.clock != host_clock.monotonic_identity()) {
            throw std::runtime_error("received stamp source does not match this host monotonic clock");
        }
        if (decoded.timing.capture.has_value()) {
            if (decoded.timing.capture->clock.kind() != rbt::time::ClockKind::HostMonotonic
                || decoded.timing.capture->clock != decoded.timing.received.clock
                || decoded.timing.capture->ns != raw_driver_ns
                || (driver_flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) != V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC
                || (driver_flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK) != V4L2_BUF_FLAG_TSTAMP_SRC_SOE) {
                throw std::runtime_error("capture stamp source does not match its V4L2 exposure metadata");
            }
            ++capture_known;
        } else {
            ++capture_unknown;
        }

        if (previous_generation.has_value() && *previous_generation == session_generation
            && previous_sequence.has_value()) {
            const auto previous = static_cast<std::uint32_t>(*previous_sequence);
            const auto current = static_cast<std::uint32_t>(sequence);
            const auto delta = static_cast<std::uint32_t>(current - previous);
            if (delta == 0U) {
                ++duplicate_count;
            } else if (delta < 0x80000000U) {
                dropped_count += static_cast<std::uint64_t>(delta - 1U);
            } else {
                ++out_of_order_count;
            }
        } else {
            previous_sequence.reset();
        }
        previous_sequence = sequence;
        previous_generation = session_generation;
        if (!session_generations.contains(session_generation) && session_generations.size() >= 8U) {
            throw std::runtime_error("more than 8 driver-session generations observed");
        }
        session_generations.insert(session_generation);

        std::cout << "{\"event\":\"frame_checked\",\"sequence\":" << sequence
                  << ",\"driver_session_generation\":" << session_generation
                  << ",\"width\":" << width << ",\"height\":" << height
                  << ",\"stride\":" << stride << ",\"fourcc\":\"YUYV\""
                  << ",\"driver_timestamp_ns\":" << raw_driver_ns
                  << ",\"driver_flags\":" << driver_flags
                  << ",\"capture_present\":" << (decoded.timing.capture.has_value() ? "true" : "false")
                  << ",\"received_source_id\":\""
                  << decoded.timing.received.clock.source_id().to_hex()
                  << "\",\"pixel_bytes\":" << raw_bytes.size()
                  << ",\"fnv1a64\":\"" << hex_u64(fnv1a64(raw_bytes)) << "\"}\n"
                  << std::flush;
        ++received_count;
    }

    const auto pressure_after = subscriber->pressure();
    const auto resources_active_at_end = active_stream_count();
    subscriber.reset();
    const auto resources_after_cleanup = active_stream_count();
    if (received_count == 0U) {
        throw std::runtime_error("no StampedBytes camera frames were received during the requested interval");
    }
    std::cout << "{\"event\":\"consumer_summary\",\"received\":" << received_count
              << ",\"actual_elapsed_ms\":"
              << std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start).count()
              << ",\"estimated_sequence_drops\":" << dropped_count
              << ",\"duplicate_sequences\":" << duplicate_count
              << ",\"out_of_order_sequences\":" << out_of_order_count
              << ",\"capture_known\":" << capture_known
              << ",\"capture_unknown\":" << capture_unknown
              << ",\"driver_session_generations\":[";
    bool first_generation = true;
    for (const auto generation : session_generations) {
        if (!first_generation) {
            std::cout << ',';
        }
        std::cout << generation;
        first_generation = false;
    }
    std::cout << "],\"resource_wrappers_before\":" << resources_before
              << ",\"resource_wrappers_during\":" << resources_started
              << ",\"resource_wrappers_at_end\":" << resources_active_at_end
              << ",\"resource_wrappers_after_cleanup\":" << resources_after_cleanup << ',';
    print_pressure_state("pressure_before_receive", pressure_before);
    std::cout << ',';
    print_pressure_state("pressure_after_receive", pressure_after);
    std::cout << "}\n" << std::flush;
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_options(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "rbt-time-camera-consumer: " << error.what() << '\n';
        return 1;
    }
}

#include "rbt/time_stream.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using rbt::time::ClockIdentity;
using rbt::time::ClockKind;
using rbt::time::ClockMapping;
using rbt::time::ClockMappingCalibration;
using rbt::time::ClockSourceId;
using rbt::time::DeviceClock;
using rbt::time::HostClock;
using rbt::time::SensorTiming;
using rbt::time::TimeError;
using rbt::time::Timestamp;
using rbt::time::TimestampState;

constexpr std::uint64_t kInteropSequence = 4242U;

[[nodiscard]] rbt::time::Result<SensorTiming> interop_timing() {
    std::array<std::uint8_t, 48> source_bytes {};
    std::fill_n(source_bytes.begin(), 16, 0x44U);
    auto clock_result = ClockIdentity::create(
        ClockKind::Device, ClockSourceId(std::move(source_bytes)), 7U);
    if (!clock_result) {
        return clock_result.error();
    }
    const auto clock = std::move(clock_result).value();
    return SensorTiming::create(Timestamp { clock, 123456789 },
                                Timestamp { clock, 987654321 },
                                2U,
                                1U);
}

[[nodiscard]] std::uint64_t fnv1a64(std::span<const std::uint8_t> bytes) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto byte : bytes) {
        hash = (hash ^ byte) * 1099511628211ULL;
    }
    return hash;
}

[[nodiscard]] bool validate_application(std::span<const std::uint8_t> bytes) {
    if (bytes.size() != 32U || fnv1a64(bytes) != 0xE6CB594C1A148AC5ULL) {
        return false;
    }
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        if (bytes[index] != static_cast<std::uint8_t>(index)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool validate_timing(const SensorTiming& timing) {
    if (!timing.capture.has_value()) {
        return false;
    }
    const auto expected = interop_timing();
    if (!expected) {
        return false;
    }
    const auto& capture = *timing.capture;
    const auto& receipt = timing.received;
    const auto& expected_capture = *expected.value().capture;
    return capture.clock == expected_capture.clock && capture.ns == 123456789
        && receipt.clock == expected_capture.clock && receipt.ns == 987654321
        && timing.capture_uncertainty_ns == 2U
        && timing.received_uncertainty_ns == 1U;
}

[[nodiscard]] int stream_publish(std::string_view service) {
    const rbt::StreamDescriptor descriptor {
        service, rbt::time::STAMPED_BYTES_DESCRIPTOR, rbt::StreamProfile::Sensor
    };
    auto publisher_result =
        rbt::time::create_stamped_bytes_publisher(descriptor, rbt::time::kStampedBytesPrefixBytes + 32U);
    if (!publisher_result) {
        std::cerr << "cannot create StampedBytes publisher\n";
        return 1;
    }
    auto publisher = std::move(publisher_result).value();
    std::cout << "RBT_TIME_STREAM READY_TO_PUBLISH\n" << std::flush;
    std::this_thread::sleep_for(std::chrono::seconds(5));

    auto timing_result = interop_timing();
    if (!timing_result) {
        std::cerr << "cannot construct interop timing\n";
        return 1;
    }
    const auto timing = std::move(timing_result).value();
    std::vector<std::uint8_t> payload(rbt::time::kStampedBytesPrefixBytes + 32U);
    const auto codec = rbt::time::write_timing_prefix(
        std::span<std::uint8_t>(payload.data(), rbt::time::kStampedBytesPrefixBytes), timing);
    if (codec != rbt::time::PrefixCodecError::None) {
        std::cerr << "cannot encode timing prefix: " << rbt::time::to_string(codec) << '\n';
        return 1;
    }
    for (std::size_t index = 0U; index < 32U; ++index) {
        payload[rbt::time::kStampedBytesPrefixBytes + index] = static_cast<std::uint8_t>(index);
    }
    auto loan_result = publisher.loan_slice(payload.size());
    if (!loan_result) {
        std::cerr << "cannot loan StampedBytes sample\n";
        return 1;
    }
    auto loan = std::move(loan_result).value();
    loan.header() = kInteropSequence;
    std::copy(payload.begin(), payload.end(), loan.payload().begin());
    auto sent = loan.send();
    if (!sent) {
        std::cerr << "cannot send StampedBytes sample\n";
        return 1;
    }
    std::cout << "{\"status\":\"sent\",\"sequence\":" << kInteropSequence
              << ",\"payload_len\":32}\n";
    return 0;
}

[[nodiscard]] int stream_consume(std::string_view service) {
    const rbt::StreamDescriptor descriptor {
        service, rbt::time::STAMPED_BYTES_DESCRIPTOR, rbt::StreamProfile::Sensor
    };
    auto subscriber_result = rbt::time::create_stamped_bytes_subscriber(descriptor);
    if (!subscriber_result) {
        std::cerr << "cannot create StampedBytes subscriber\n";
        return 1;
    }
    auto subscriber = std::move(subscriber_result).value();
    std::cout << "RBT_TIME_STREAM READY_TO_RECEIVE\n" << std::flush;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        auto receive_result = subscriber.receive();
        if (!receive_result) {
            std::cerr << "cannot receive StampedBytes sample\n";
            return 1;
        }
        auto maybe_sample = std::move(receive_result).value();
        if (!maybe_sample.has_value()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        auto sample = std::move(*maybe_sample);
        if (sample.header() != kInteropSequence) {
            std::cerr << "received sequence differs from fixture\n";
            return 1;
        }
        auto decoded = rbt::time::decode_stamped_bytes(sample.payload());
        if (!std::holds_alternative<rbt::time::DecodedStampedBytes>(decoded)) {
            std::cerr << "received timing prefix is invalid\n";
            return 1;
        }
        const auto& value = std::get<rbt::time::DecodedStampedBytes>(decoded);
        if (!validate_timing(value.timing) || !validate_application(value.bytes)
            || fnv1a64(value.bytes) != 0xE6CB594C1A148AC5ULL) {
            std::cerr << "received StampedBytes fields differ from fixture\n";
            return 1;
        }
        std::cout << "{\"status\":\"received\",\"sequence\":" << sample.header()
                  << ",\"payload_len\":" << value.bytes.size()
                  << ",\"fnv1a64\":\"";
        std::cout << std::hex << std::nouppercase << fnv1a64(value.bytes) << std::dec;
        std::cout << "\",\"capture_ns\":123456789,\"received_ns\":987654321"
                     ",\"capture_uncertainty_ns\":2,\"received_uncertainty_ns\":1}\n";
        return 0;
    }
    std::cerr << "timed out waiting for a StampedBytes sample\n";
    return 1;
}

[[nodiscard]] const char* kind_name(ClockKind kind) noexcept {
    switch (kind) {
    case ClockKind::HostMonotonic:
        return "host_monotonic";
    case ClockKind::SystemRealtime:
        return "system_realtime";
    case ClockKind::Device:
        return "device";
    }
    return "unknown";
}

template <typename T>
[[nodiscard]] const char* result_error(const rbt::time::Result<T>& result) noexcept {
    return result ? "UNEXPECTED_OK" : rbt::time::to_string(result.error());
}

[[nodiscard]] rbt::time::Result<ClockIdentity> identity(ClockKind kind,
                                                        std::uint8_t first_byte,
                                                        std::uint64_t generation = 0U) {
    std::array<std::uint8_t, 48> bytes {};
    for (std::size_t index = 0U; index < 16U; ++index) {
        bytes[index] = first_byte;
    }
    return ClockIdentity::create(kind, ClockSourceId(std::move(bytes)), generation);
}

[[nodiscard]] ClockIdentity take_identity(rbt::time::Result<ClockIdentity> result) {
    if (!result) {
        std::cerr << "identity: " << rbt::time::to_string(result.error()) << '\n';
        std::exit(1);
    }
    return std::move(result).value();
}

[[nodiscard]] ClockIdentity calibration_host_identity() {
    std::array<std::uint8_t, 48> bytes {};
    for (std::size_t index = 0U; index < 16U; ++index) {
        bytes[index] = 0x44U;
    }
    bytes[16] = 1U;
    bytes[24] = 2U;
    auto result = ClockIdentity::create(ClockKind::HostMonotonic, ClockSourceId(std::move(bytes)), 0U);
    if (!result) {
        std::cerr << "host calibration identity: " << rbt::time::to_string(result.error()) << '\n';
        std::exit(1);
    }
    return std::move(result).value();
}

[[nodiscard]] rbt::time::Result<ClockMapping> make_mapping(const ClockIdentity& source,
                                                            const ClockIdentity& target,
                                                            const ClockIdentity& host,
                                                            std::uint64_t numerator,
                                                            std::uint64_t denominator,
                                                            std::uint64_t uncertainty,
                                                            std::int64_t source_anchor = 100,
                                                            std::int64_t target_anchor = 1000) {
    ClockMappingCalibration calibration {};
    calibration.source = source;
    calibration.target = target;
    calibration.source_anchor_ns = source_anchor;
    calibration.target_anchor_ns = target_anchor;
    calibration.rate_numerator = numerator;
    calibration.rate_denominator = denominator;
    calibration.valid_from_ns = 0;
    calibration.valid_until_ns = 10000;
    calibration.calibration_id.fill(1U);
    calibration.calibrated_at = Timestamp { host, 10 };
    calibration.expires_at = Timestamp { host, 200 };
    calibration.uncertainty_ns = uncertainty;
    return ClockMapping::create(std::move(calibration));
}

[[nodiscard]] bool semantics() {
    std::array<std::uint8_t, 16> device_uuid {};
    device_uuid.fill(0x11U);
    auto device_result = DeviceClock::create(device_uuid);
    if (!device_result) {
        std::cerr << "device clock: " << rbt::time::to_string(device_result.error()) << '\n';
        return false;
    }
    auto device = std::move(device_result).value();
    auto device_stamp_result = device.stamp(103);
    if (!device_stamp_result) {
        std::cerr << "device stamp: " << rbt::time::to_string(device_stamp_result.error()) << '\n';
        return false;
    }
    const Timestamp device_stamp = std::move(device_stamp_result).value();

    std::array<std::uint8_t, 16> other_uuid {};
    other_uuid.fill(0x22U);
    auto other_clock_result = DeviceClock::create(other_uuid);
    if (!other_clock_result) {
        std::cerr << "other device clock: " << rbt::time::to_string(other_clock_result.error()) << '\n';
        return false;
    }
    auto other_clock = std::move(other_clock_result).value();
    auto other_stamp_result = other_clock.stamp(103);
    if (!other_stamp_result) {
        std::cerr << "other device stamp: " << rbt::time::to_string(other_stamp_result.error()) << '\n';
        return false;
    }
    const Timestamp other_stamp = std::move(other_stamp_result).value();

    const auto host_identity = calibration_host_identity();
    const auto realtime = take_identity(identity(ClockKind::SystemRealtime, 0x33U));
    const auto arithmetic_identity = take_identity(identity(ClockKind::Device, 0x11U));
    const Timestamp arithmetic_left { arithmetic_identity, 7 };
    const Timestamp arithmetic_right { arithmetic_identity, 4 };
    const auto difference = rbt::time::checked_difference(arithmetic_left, arithmetic_right);
    const auto duration = rbt::time::checked_duration_since(arithmetic_left, arithmetic_right);
    const auto add_result = rbt::time::checked_add_nanoseconds(arithmetic_right, -2);
    const auto add_overflow = rbt::time::checked_add_nanoseconds(
        Timestamp { arithmetic_identity, std::numeric_limits<std::int64_t>::max() }, 1);
    const auto negative_duration = rbt::time::checked_duration_since(
        Timestamp { arithmetic_identity, 10 }, Timestamp { arithmetic_identity, 11 });
    if (!difference || !duration || !add_result) {
        std::cerr << "arithmetic probe did not produce expected values\n";
        return false;
    }

    auto base_mapping_result = make_mapping(device_stamp.clock, realtime, host_identity, 3, 2, 5);
    if (!base_mapping_result) {
        std::cerr << "mapping: " << rbt::time::to_string(base_mapping_result.error()) << '\n';
        return false;
    }
    auto base_mapping = std::move(base_mapping_result).value();
    const auto converted = base_mapping.convert(device_stamp,
                                                2U,
                                                realtime,
                                                Timestamp { host_identity, 11 });
    const auto expiry = base_mapping.convert(device_stamp,
                                             2U,
                                             realtime,
                                             Timestamp { host_identity, 200 });
    const auto unknown_uncertainty = base_mapping.convert(device_stamp,
                                                          std::nullopt,
                                                          realtime,
                                                          Timestamp { host_identity, 11 });

    auto reset_result = device.reset();
    if (!reset_result) {
        std::cerr << "device reset: " << rbt::time::to_string(reset_result.error()) << '\n';
        return false;
    }
    auto reset_event = std::move(reset_result).value();
    auto reset_stamp_result = device.stamp(103);
    if (!reset_stamp_result) {
        std::cerr << "reset stamp: " << rbt::time::to_string(reset_stamp_result.error()) << '\n';
        return false;
    }
    const auto reset_old_stamp = rbt::time::checked_compare(device_stamp, reset_stamp_result.value());

    auto invalidated_mapping_result = make_mapping(device_stamp.clock, realtime, host_identity, 1, 1, 0);
    if (!invalidated_mapping_result) {
        std::cerr << "invalidated mapping: " << rbt::time::to_string(invalidated_mapping_result.error()) << '\n';
        return false;
    }
    auto invalidated_mapping = std::move(invalidated_mapping_result).value();
    const bool invalidated = invalidated_mapping.invalidate(reset_event);
    const auto invalidated_error = invalidated_mapping.convert(device_stamp,
                                                              0U,
                                                              realtime,
                                                              Timestamp { host_identity, 11 });

    auto overflow_mapping_result = make_mapping(device_stamp.clock, realtime, host_identity, 2, 1, 1);
    if (!overflow_mapping_result) {
        std::cerr << "overflow mapping: " << rbt::time::to_string(overflow_mapping_result.error()) << '\n';
        return false;
    }
    auto overflow_mapping = std::move(overflow_mapping_result).value();
    const auto uncertainty_overflow = overflow_mapping.convert(device_stamp,
                                                               std::numeric_limits<std::uint64_t>::max(),
                                                               realtime,
                                                               Timestamp { host_identity, 11 });
    auto maximum_uncertainty_mapping_result = make_mapping(device_stamp.clock,
                                                           realtime,
                                                           host_identity,
                                                           std::numeric_limits<std::uint64_t>::max(),
                                                           std::numeric_limits<std::uint64_t>::max(),
                                                           0);
    if (!maximum_uncertainty_mapping_result) {
        std::cerr << "maximum uncertainty mapping: "
                  << rbt::time::to_string(maximum_uncertainty_mapping_result.error()) << '\n';
        return false;
    }
    auto maximum_uncertainty_mapping = std::move(maximum_uncertainty_mapping_result).value();
    const auto maximum_uncertainty = maximum_uncertainty_mapping.convert(
        device_stamp,
        std::numeric_limits<std::uint64_t>::max(),
        realtime,
        Timestamp { host_identity, 11 });

    const auto self_mapping_offset = make_mapping(device_stamp.clock,
                                                  device_stamp.clock,
                                                  host_identity,
                                                  1U,
                                                  1U,
                                                  0U,
                                                  100,
                                                  101);
    const auto self_mapping_rate = make_mapping(device_stamp.clock,
                                                device_stamp.clock,
                                                host_identity,
                                                2U,
                                                1U,
                                                0U,
                                                100,
                                                100);
    auto self_mapping_identity_result = make_mapping(device_stamp.clock,
                                                     device_stamp.clock,
                                                     host_identity,
                                                     2U,
                                                     2U,
                                                     0U,
                                                     100,
                                                     100);
    if (!self_mapping_identity_result) {
        std::cerr << "identity self-mapping: "
                  << rbt::time::to_string(self_mapping_identity_result.error()) << '\n';
        return false;
    }
    auto self_mapping_identity = std::move(self_mapping_identity_result).value().convert(
        Timestamp { device_stamp.clock, 103 },
        0U,
        device_stamp.clock,
        Timestamp { host_identity, 11 });

    auto capture_timing = SensorTiming::create(std::optional<Timestamp> { device_stamp },
                                               Timestamp { host_identity, 400 },
                                               2U,
                                               1U);
    auto legacy_timing = SensorTiming::legacy(Timestamp { host_identity, 401 });
    if (!capture_timing || !legacy_timing || !converted) {
        std::cerr << "sensor timing or mapping probe failed\n";
        return false;
    }
    if (!maximum_uncertainty) {
        std::cerr << "maximum uncertainty conversion: " << rbt::time::to_string(maximum_uncertainty.error()) << '\n';
        return false;
    }
    if (!self_mapping_identity) {
        std::cerr << "identity self-mapping conversion: "
                  << rbt::time::to_string(self_mapping_identity.error()) << '\n';
        return false;
    }
    const char* legacy_state = legacy_timing.value().capture.has_value() ? "known" : "unknown";

    std::cout << "{\"cases\":{";
    std::cout << "\"arithmetic\":{";
    std::cout << "\"difference_ns\":" << difference.value() << ',';
    std::cout << "\"duration_ns\":" << duration.value().count() << ',';
    std::cout << "\"add_result_ns\":" << add_result.value().ns << ',';
    std::cout << "\"add_overflow\":\"" << result_error(add_overflow) << "\",";
    std::cout << "\"negative_duration\":\"" << result_error(negative_duration) << "\"},";
    std::cout << "\"identity\":{";
    std::cout << "\"source_mismatch\":\"" << result_error(rbt::time::checked_compare(device_stamp, other_stamp)) << "\",";
    std::cout << "\"generation_mismatch\":\"" << result_error(rbt::time::checked_compare(device_stamp, reset_stamp_result.value())) << "\",";
    std::cout << "\"domain_mismatch\":\"" << result_error(rbt::time::checked_compare(device_stamp, Timestamp { realtime, 103 })) << "\"},";
    std::cout << "\"device_reset\":{";
    std::cout << "\"generation\":" << device.identity().value().generation() << ',';
    std::cout << "\"old_stamp_rejected\":\"" << result_error(reset_old_stamp) << "\"},";
    std::cout << "\"mapping\":{";
    std::cout << "\"timestamp_ns\":" << converted.value().timestamp.ns << ',';
    std::cout << "\"uncertainty_ns\":" << converted.value().uncertainty_ns << ',';
    std::cout << "\"expiry\":\"" << result_error(expiry) << "\",";
    std::cout << "\"unknown_uncertainty\":\"" << result_error(unknown_uncertainty) << "\",";
    std::cout << "\"invalidated\":\""
              << (invalidated && !invalidated_error ? rbt::time::to_string(invalidated_error.error()) : "INVALIDATION_FAILED")
              << "\",";
    std::cout << "\"uncertainty_overflow\":\"" << result_error(uncertainty_overflow) << "\",";
    std::cout << "\"uncertainty_u64_max\":" << maximum_uncertainty.value().uncertainty_ns << ',';
    std::cout << "\"self_mapping_offset\":\"" << result_error(self_mapping_offset) << "\",";
    std::cout << "\"self_mapping_rate\":\"" << result_error(self_mapping_rate) << "\",";
    std::cout << "\"self_mapping_identity\":" << self_mapping_identity.value().timestamp.ns << "},";
    std::cout << "\"capture_separation\":{";
    std::cout << "\"capture_kind\":\"" << kind_name(capture_timing.value().capture->clock.kind()) << "\",";
    std::cout << "\"received_kind\":\"" << kind_name(capture_timing.value().received.clock.kind()) << "\",";
    std::cout << "\"legacy_state\":\"" << legacy_state << "\"}},";

    auto live_clock_result = HostClock::create();
    if (!live_clock_result) {
        std::cout << "\"host_monotonic\":{\"available\":false,\"error\":\""
                  << rbt::time::to_string(live_clock_result.error()) << "\"}}\n";
        return true;
    }
    auto live_clock = std::move(live_clock_result).value();
    const auto now = live_clock.now_monotonic();
    if (!now) {
        std::cout << "\"host_monotonic\":{\"available\":false,\"error\":\""
                  << rbt::time::to_string(now.error()) << "\"}}\n";
        return true;
    }
    std::cout << "\"host_monotonic\":{";
    std::cout << "\"available\":true,";
    std::cout << "\"kind\":\"" << kind_name(now.value().clock.kind()) << "\",";
    std::cout << "\"source_id\":\"" << now.value().clock.source_id().to_hex() << "\",";
    std::cout << "\"generation\":" << now.value().clock.generation() << ',';
    std::cout << "\"nanoseconds\":" << now.value().ns << "}}\n";
    return true;
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "semantics") {
        return semantics() ? 0 : 1;
    }
    if (argc == 3 && std::string_view(argv[1]) == "stream-publish") {
        return stream_publish(argv[2]);
    }
    if (argc == 3 && std::string_view(argv[1]) == "stream-consume") {
        return stream_consume(argv[2]);
    }
    std::cerr << "usage: rbt-time-probe semantics | stream-publish SERVICE | stream-consume SERVICE\n";
    return 2;
}

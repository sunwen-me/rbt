#include <rbt/time.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

using rbt::time::ClockDiscontinuity;
using rbt::time::ClockIdentity;
using rbt::time::ClockKind;
using rbt::time::ClockMapping;
using rbt::time::ClockMappingCalibration;
using rbt::time::ClockSourceId;
using rbt::time::DeviceClock;
using rbt::time::HostClock;
using rbt::time::Result;
using rbt::time::SensorTiming;
using rbt::time::TimeError;
using rbt::time::Timestamp;
using rbt::time::TimestampState;

template <typename T>
bool is_error(const Result<T>& result, TimeError expected) {
    return !result && result.error() == expected;
}

ClockIdentity make_identity(ClockKind kind, std::uint8_t seed, std::uint64_t generation = 0U) {
    std::array<std::uint8_t, 48> bytes {};
    for (std::size_t index = 0U; index < 16U; ++index) {
        bytes[index] = static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(index));
    }
    auto result = ClockIdentity::create(kind, ClockSourceId(std::move(bytes)), generation);
    if (!result) {
        std::terminate();
    }
    return std::move(result).value();
}

ClockMappingCalibration calibration_for(const ClockIdentity& source,
                                        const ClockIdentity& target,
                                        const ClockIdentity& calibration_clock,
                                        std::int64_t source_anchor = 0,
                                        std::int64_t target_anchor = 0,
                                        std::uint64_t numerator = 1U,
                                        std::uint64_t denominator = 1U,
                                        std::int64_t valid_from = -100,
                                        std::int64_t valid_until = 100,
                                        std::optional<std::uint64_t> uncertainty = 1U) {
    ClockMappingCalibration calibration;
    calibration.source = source;
    calibration.target = target;
    calibration.source_anchor_ns = source_anchor;
    calibration.target_anchor_ns = target_anchor;
    calibration.rate_numerator = numerator;
    calibration.rate_denominator = denominator;
    calibration.valid_from_ns = valid_from;
    calibration.valid_until_ns = valid_until;
    calibration.calibration_id[0] = 0xA5U;
    calibration.calibrated_at = Timestamp { calibration_clock, 100 };
    calibration.expires_at = Timestamp { calibration_clock, 200 };
    calibration.uncertainty_ns = uncertainty;
    return calibration;
}

bool test_source_id_and_comparison() {
    std::array<std::uint8_t, 16> uuid {};
    uuid[0] = 0x12U;
    uuid[15] = 0xFEU;
    auto device_result = DeviceClock::create(uuid);
    if (!device_result) {
        return false;
    }
    auto device = std::move(device_result).value();
    auto device_identity = device.identity();
    if (!device_identity) {
        return false;
    }
    const auto known_state = TimestampState::known(Timestamp { device_identity.value(), 7 });
    const auto unknown_state = TimestampState::from_legacy_timestamp_ns(123U);
    const auto unmapped_state = TimestampState::unmapped(Timestamp { device_identity.value(), 9 });
    if (!known_state || !unmapped_state
        || known_state.value().kind() != TimestampState::Kind::Known
        || known_state.value().timestamp()->ns != 7
        || unknown_state.kind() != TimestampState::Kind::Unknown || unknown_state.timestamp().has_value()
        || unmapped_state.value().kind() != TimestampState::Kind::Unmapped
        || unmapped_state.value().timestamp()->ns != 9) {
        return false;
    }
    const auto source_id = device_identity.value().source_id();
    const auto encoded = source_id.to_hex();
    if (encoded.size() != 96U || encoded.substr(0U, 4U) != "1200" || encoded.substr(32U) != std::string(64U, '0')) {
        return false;
    }
    auto decoded = ClockSourceId::from_hex(encoded);
    if (!decoded || decoded.value() != source_id) {
        return false;
    }
    if (!is_error(ClockSourceId::from_hex(std::string(96U, '0')), TimeError::InvalidClockSourceId)
        || !is_error(ClockSourceId::from_hex(std::string(96U, 'A')), TimeError::InvalidClockSourceId)) {
        return false;
    }
    std::array<std::uint8_t, 48> invalid_identity_bytes {};
    if (!is_error(ClockIdentity::create(ClockKind::Device, ClockSourceId(invalid_identity_bytes)),
                  TimeError::InvalidClockIdentity)) {
        return false;
    }
    std::array<std::uint8_t, 48> invalid_offset_bytes {};
    invalid_offset_bytes[0] = 0x01U;
    invalid_offset_bytes[40] = 0x00U;
    invalid_offset_bytes[41] = 0xCAU;
    invalid_offset_bytes[42] = 0x9AU;
    invalid_offset_bytes[43] = 0x3BU;
    if (!is_error(ClockIdentity::create(ClockKind::HostMonotonic, ClockSourceId(invalid_offset_bytes)),
                  TimeError::InvalidClockIdentity)) {
        return false;
    }

    const auto same_clock = device_identity.value();
    const auto another_source = make_identity(ClockKind::Device, 0x30U);
    const auto other_domain = make_identity(ClockKind::SystemRealtime, 0x30U);
    auto other_generation_result = ClockIdentity::create(ClockKind::Device, source_id, 1U);
    if (!other_generation_result) {
        return false;
    }
    const auto other_generation = std::move(other_generation_result).value();
    const Timestamp first { same_clock, 10 };
    const Timestamp second { same_clock, 30 };
    const Timestamp wrong_source { another_source, 30 };
    const Timestamp wrong_domain { other_domain, 30 };
    const Timestamp wrong_generation { other_generation, 30 };
    if (rbt::time::checked_difference(second, first).value() != 20
        || rbt::time::checked_duration_since(second, first).value() != std::chrono::nanoseconds(20)) {
        return false;
    }
    if (!is_error(rbt::time::checked_compare(first, wrong_domain), TimeError::ClockDomainMismatch)
        || !is_error(rbt::time::checked_compare(first, wrong_source), TimeError::ClockSourceMismatch)
        || !is_error(rbt::time::checked_compare(first, wrong_generation), TimeError::ClockGenerationMismatch)
        || !is_error(rbt::time::checked_duration_since(first, second), TimeError::NegativeDuration)) {
        return false;
    }
    if (!rbt::time::checked_add_nanoseconds(second, 5).has_value()
        || rbt::time::checked_add_nanoseconds(second, 5).value().ns != 35) {
        return false;
    }
    const Timestamp maximum { same_clock, std::numeric_limits<std::int64_t>::max() };
    const Timestamp minimum { same_clock, std::numeric_limits<std::int64_t>::min() };
    if (!is_error(rbt::time::checked_difference(maximum, minimum), TimeError::TimestampOverflow)
        || !is_error(rbt::time::checked_add_nanoseconds(maximum, 1), TimeError::TimestampOverflow)) {
        return false;
    }
    return true;
}

bool test_device_reset_and_sensor_timing() {
    std::array<std::uint8_t, 16> uuid {};
    uuid[0] = 1U;
    auto device_result = DeviceClock::create(uuid);
    if (!device_result) {
        return false;
    }
    auto device = std::move(device_result).value();
    auto before = device.stamp(5);
    auto reset = device.reset();
    auto after = device.stamp(6);
    if (!before || !reset || !after || reset.value().previous.generation() != 0U
        || reset.value().current.generation() != 1U
        || !is_error(rbt::time::checked_compare(before.value(), after.value()), TimeError::ClockGenerationMismatch)) {
        return false;
    }

    auto overflow_device = DeviceClock::create(uuid, std::numeric_limits<std::uint64_t>::max());
    if (!overflow_device || !is_error(overflow_device.value().reset(), TimeError::TimestampOverflow)) {
        return false;
    }

    const auto received_clock = make_identity(ClockKind::HostMonotonic, 0x40U);
    auto legacy = SensorTiming::legacy(Timestamp { received_clock, 1234 }, 7U);
    if (!legacy || legacy.value().capture.has_value()
        || legacy.value().received.ns != 1234 || legacy.value().received_uncertainty_ns != 7U) {
        return false;
    }
    auto capture_without_time = SensorTiming::create({}, Timestamp { received_clock, 1 }, 0U, {});
    auto with_capture = SensorTiming::create(Timestamp { received_clock, 10 },
                                             Timestamp { received_clock, 11 },
                                             2U,
                                             1U);
    return is_error(capture_without_time, TimeError::InvalidTimestamp)
           && with_capture && with_capture.value().capture.has_value()
           && with_capture.value().capture_uncertainty_ns == 2U;
}

bool test_mapping_conversion_and_validation() {
    const auto source = make_identity(ClockKind::Device, 0x11U);
    const auto target = make_identity(ClockKind::SystemRealtime, 0x21U);
    const auto calibration_clock = make_identity(ClockKind::HostMonotonic, 0x31U);
    auto mapping_result = ClockMapping::create(
        calibration_for(source, target, calibration_clock, 0, 1000, 2U, 3U, -4, 4, 1U));
    if (!mapping_result) {
        return false;
    }
    auto fixture_calibration = calibration_for(source, target, calibration_clock, 100, 1000, 3U, 2U, 0, 200, 4U);
    fixture_calibration.calibrated_at.ns = 10;
    fixture_calibration.expires_at.ns = 20;
    auto fixture_result = ClockMapping::create(std::move(fixture_calibration));
    if (!fixture_result) {
        return false;
    }
    auto fixture = std::move(fixture_result).value();
    const Timestamp fixture_now { calibration_clock, 19 };
    auto rounded_positive = fixture.convert(Timestamp { source, 101 }, 2U, target, fixture_now);
    auto rounded_negative = fixture.convert(Timestamp { source, 99 }, 2U, target, fixture_now);
    auto exact = fixture.convert(Timestamp { source, 100 }, 2U, target, fixture_now);
    if (!rounded_positive || rounded_positive.value().timestamp.ns != 1001
        || rounded_positive.value().uncertainty_ns != 8U
        || !rounded_negative || rounded_negative.value().timestamp.ns != 999
        || rounded_negative.value().uncertainty_ns != 8U
        || !exact || exact.value().timestamp.ns != 1000 || exact.value().uncertainty_ns != 7U) {
        return false;
    }
    auto mapping = std::move(mapping_result).value();
    const Timestamp now { calibration_clock, 150 };
    auto positive = mapping.convert(Timestamp { source, 4 }, 2U, target, now);
    auto negative = mapping.convert(Timestamp { source, -4 }, 2U, target, now);
    if (!positive || positive.value().timestamp.ns != 1002 || positive.value().uncertainty_ns != 4U
        || !negative || negative.value().timestamp.ns != 998 || negative.value().uncertainty_ns != 4U) {
        return false;
    }
    if (!is_error(mapping.convert(Timestamp { source, 5 }, 2U, target, now), TimeError::TimestampOutOfRange)
        || !is_error(mapping.convert(Timestamp { source, 0 }, {}, target, now), TimeError::UnknownUncertainty)
        || !is_error(mapping.convert(Timestamp { source, 0 }, 2U, target, Timestamp { calibration_clock, 200 }),
                     TimeError::ClockMappingExpired)
        || !is_error(mapping.convert(Timestamp { source, 0 }, 2U, target, Timestamp { calibration_clock, 99 }),
                     TimeError::ClockMappingUnavailable)
        || !is_error(mapping.convert(Timestamp { source, 0 }, 2U, target,
                                    Timestamp { make_identity(ClockKind::SystemRealtime, 0x77U), 150 }),
                     TimeError::ClockDomainMismatch)) {
        return false;
    }

    const auto different_source = make_identity(ClockKind::Device, 0x51U);
    const auto changed_source_generation = make_identity(ClockKind::Device, 0x11U, 1U);
    const auto different_target = make_identity(ClockKind::SystemRealtime, 0x61U);
    const auto different_target_kind = make_identity(ClockKind::HostMonotonic, 0x21U);
    auto different_target_generation_result = ClockIdentity::create(ClockKind::SystemRealtime,
                                                                    target.source_id(),
                                                                    target.generation() + 1U);
    if (!different_target_generation_result) {
        return false;
    }
    const auto different_target_generation = std::move(different_target_generation_result).value();
    if (!is_error(mapping.convert(Timestamp { different_source, 0 }, 2U, target, now),
                  TimeError::ClockSourceMismatch)
        || !is_error(mapping.convert(Timestamp { changed_source_generation, 0 }, 2U, target, now),
                     TimeError::ClockGenerationMismatch)
        || !is_error(mapping.convert(Timestamp { source, 0 }, 2U, different_target, now),
                     TimeError::ClockSourceMismatch)
        || !is_error(mapping.convert(Timestamp { source, 0 }, 2U, different_target_kind, now),
                     TimeError::ClockDomainMismatch)
        || !is_error(mapping.convert(Timestamp { source, 0 }, 2U, different_target_generation, now),
                     TimeError::ClockGenerationMismatch)) {
        return false;
    }

    auto zero_rate = calibration_for(source, target, calibration_clock);
    zero_rate.rate_numerator = 0U;
    if (!is_error(ClockMapping::create(std::move(zero_rate)), TimeError::InvalidConversionRate)) {
        return false;
    }
    auto invalid_interval = calibration_for(source, target, calibration_clock);
    invalid_interval.valid_from_ns = 2;
    invalid_interval.valid_until_ns = 1;
    if (!is_error(ClockMapping::create(std::move(invalid_interval)), TimeError::ClockMappingInvalid)) {
        return false;
    }
    auto unknown_mapping_uncertainty = calibration_for(source, target, calibration_clock, 0, 0, 1U, 1U, -5, 5, {});
    if (!is_error(ClockMapping::create(std::move(unknown_mapping_uncertainty)), TimeError::UnknownUncertainty)) {
        return false;
    }
    auto no_evidence = calibration_for(source, target, calibration_clock);
    no_evidence.calibration_id = {};
    if (!is_error(ClockMapping::create(std::move(no_evidence)), TimeError::InvalidCalibrationEvidence)) {
        return false;
    }
    return true;
}

bool test_identity_mapping_calibration_invariant() {
    const auto clock = make_identity(ClockKind::Device, 0x70U);
    const auto calibration_clock = make_identity(ClockKind::HostMonotonic, 0x71U);

    auto offset_calibration = calibration_for(clock, clock, calibration_clock, 5, 6, 1U, 1U, -10, 10, 0U);
    if (!is_error(ClockMapping::create(std::move(offset_calibration)), TimeError::ClockMappingInvalid)) {
        return false;
    }

    auto rate_calibration = calibration_for(clock, clock, calibration_clock, 5, 5, 3U, 2U, -10, 10, 0U);
    if (!is_error(ClockMapping::create(std::move(rate_calibration)), TimeError::ClockMappingInvalid)) {
        return false;
    }

    auto equivalent_fraction = calibration_for(clock, clock, calibration_clock, 5, 5, 2U, 2U, -10, 10, 0U);
    auto mapping_result = ClockMapping::create(std::move(equivalent_fraction));
    if (!mapping_result) {
        return false;
    }
    auto mapping = std::move(mapping_result).value();
    auto converted = mapping.convert(Timestamp { clock, 8 }, 0U, clock, Timestamp { calibration_clock, 150 });
    return converted && converted.value().timestamp.clock == clock && converted.value().timestamp.ns == 8
           && converted.value().uncertainty_ns == 0U;
}

bool test_mapping_overflow_and_invalidation() {
    const auto source = make_identity(ClockKind::Device, 0x01U);
    const auto target = make_identity(ClockKind::SystemRealtime, 0x02U);
    const auto mono = make_identity(ClockKind::HostMonotonic, 0x03U);

    auto i128_overflow = calibration_for(source,
                                         target,
                                         mono,
                                         std::numeric_limits<std::int64_t>::min(),
                                         0,
                                         std::numeric_limits<std::uint64_t>::max(),
                                         1U,
                                         std::numeric_limits<std::int64_t>::min(),
                                         std::numeric_limits<std::int64_t>::max());
    auto i128_mapping_result = ClockMapping::create(std::move(i128_overflow));
    if (!i128_mapping_result) {
        return false;
    }
    auto i128_mapping = std::move(i128_mapping_result).value();
    if (!is_error(i128_mapping.convert(Timestamp { source, std::numeric_limits<std::int64_t>::max() },
                                       0U,
                                       target,
                                       Timestamp { mono, 150 }),
                     TimeError::TimestampOverflow)) {
        return false;
    }

    auto i64_overflow = calibration_for(source,
                                        target,
                                        mono,
                                        0,
                                        std::numeric_limits<std::int64_t>::max(),
                                        1U,
                                        1U,
                                        0,
                                        2);
    auto i64_mapping_result = ClockMapping::create(std::move(i64_overflow));
    if (!i64_mapping_result) {
        return false;
    }
    auto i64_mapping = std::move(i64_mapping_result).value();
    if (!is_error(i64_mapping.convert(Timestamp { source, 1 }, 0U, target, Timestamp { mono, 150 }),
                  TimeError::TimestampOverflow)) {
        return false;
    }

    auto uncertainty_overflow = calibration_for(source,
                                                target,
                                                mono,
                                                0,
                                                0,
                                                std::numeric_limits<std::uint64_t>::max(),
                                                1U,
                                                0,
                                                2,
                                                0U);
    auto uncertainty_mapping_result = ClockMapping::create(std::move(uncertainty_overflow));
    if (!uncertainty_mapping_result) {
        return false;
    }
    auto uncertainty_mapping = std::move(uncertainty_mapping_result).value();
    if (!is_error(uncertainty_mapping.convert(Timestamp { source, 1 },
                                              std::numeric_limits<std::uint64_t>::max(),
                                              target,
                                              Timestamp { mono, 150 }),
                     TimeError::TimestampOverflow)) {
        return false;
    }
    auto unit_rate = calibration_for(source, target, mono, 0, 0,
                                     std::numeric_limits<std::uint64_t>::max(),
                                     std::numeric_limits<std::uint64_t>::max(),
                                     0,
                                     2,
                                     0U);
    auto unit_rate_mapping_result = ClockMapping::create(std::move(unit_rate));
    if (!unit_rate_mapping_result) {
        return false;
    }
    auto unit_rate_mapping = std::move(unit_rate_mapping_result).value();
    auto exact_uncertainty = unit_rate_mapping.convert(Timestamp { source, 0 },
                                                       std::numeric_limits<std::uint64_t>::max(),
                                                       target,
                                                       Timestamp { mono, 150 });
    if (!exact_uncertainty || exact_uncertainty.value().uncertainty_ns != std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }

    std::array<std::uint8_t, 16> uuid {};
    uuid[0] = 0x77U;
    auto device_result = DeviceClock::create(uuid);
    if (!device_result) {
        return false;
    }
    auto device = std::move(device_result).value();
    auto before_reset = device.identity();
    if (!before_reset) {
        return false;
    }
    auto stale_mapping_result = ClockMapping::create(
        calibration_for(before_reset.value(), target, mono, 0, 0, 1U, 1U, -5, 5));
    if (!stale_mapping_result) {
        return false;
    }
    auto stale_mapping = std::move(stale_mapping_result).value();
    auto unrelated_event = ClockDiscontinuity { make_identity(ClockKind::Device, 0x18U),
                                                make_identity(ClockKind::Device, 0x18U, 1U) };
    if (stale_mapping.invalidate(unrelated_event)) {
        return false;
    }
    auto reset = device.reset();
    if (!reset || !stale_mapping.invalidate(reset.value())
        || !is_error(stale_mapping.convert(Timestamp { before_reset.value(), 0 },
                                           0U,
                                           target,
                                           Timestamp { mono, 150 }),
                     TimeError::MappingInvalidated)) {
        return false;
    }
    auto target_generation = ClockIdentity::create(ClockKind::SystemRealtime, target.source_id(), 1U);
    auto target_mapping_result = ClockMapping::create(calibration_for(source, target, mono, 0, 0, 1U, 1U, -5, 5));
    if (!target_generation || !target_mapping_result) {
        return false;
    }
    auto target_mapping = std::move(target_mapping_result).value();
    const ClockDiscontinuity target_reset { target, std::move(target_generation).value() };
    return target_mapping.invalidate(target_reset)
           && is_error(target_mapping.convert(Timestamp { source, 0 },
                                              0U,
                                              target,
                                              Timestamp { mono, 150 }),
                       TimeError::MappingInvalidated);
}

bool decode_uuid(std::string_view text, std::array<std::uint8_t, 16>& uuid) {
    if (text.size() != 36U) {
        return false;
    }
    std::size_t output = 0U;
    for (std::size_t index = 0U; index < text.size();) {
        if (text[index] == '-') {
            ++index;
            continue;
        }
        const auto digit = [](char value) -> int {
            if (value >= '0' && value <= '9') {
                return value - '0';
            }
            if (value >= 'a' && value <= 'f') {
                return value - 'a' + 10;
            }
            return -1;
        };
        if (index + 1U >= text.size() || output >= uuid.size()) {
            return false;
        }
        const auto high = digit(text[index]);
        const auto low = digit(text[index + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        uuid[output++] = static_cast<std::uint8_t>((high << 4) | low);
        index += 2U;
    }
    return output == uuid.size();
}

void put_le(std::array<std::uint8_t, 48>& bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0U; index < 8U; ++index) {
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
    }
}

std::string current_thread_offsets_path() {
    const auto thread_id = ::syscall(SYS_gettid);
    return "/proc/" + std::to_string(thread_id) + "/timens_offsets";
}

bool test_observed_realtime_step_keeps_steady_elapsed() {
    const auto realtime_before_clock = make_identity(ClockKind::SystemRealtime, 0x33U, 0U);
    const auto realtime_after_clock = make_identity(ClockKind::SystemRealtime, 0x33U, 1U);
    const auto monotonic_clock = make_identity(ClockKind::HostMonotonic, 0x44U, 0U);
    const auto device_clock = make_identity(ClockKind::Device, 0x55U, 0U);
    auto mapping_result = ClockMapping::create(
        calibration_for(realtime_before_clock, device_clock, monotonic_clock, 0, 0, 1U, 1U,
                        std::numeric_limits<std::int64_t>::min(),
                        std::numeric_limits<std::int64_t>::max()));
    if (!mapping_result) {
        return false;
    }
    auto mapping = std::move(mapping_result).value();
    const Timestamp realtime_before { realtime_before_clock, 500 };
    const Timestamp realtime_after { realtime_after_clock, 100 };
    const Timestamp monotonic_before { monotonic_clock, 150 };
    const Timestamp monotonic_after { monotonic_clock, 151 };
    const ClockDiscontinuity confirmed_step { realtime_before_clock, realtime_after_clock };
    return is_error(rbt::time::checked_compare(realtime_before, realtime_after),
                    TimeError::ClockGenerationMismatch)
           && mapping.invalidate(confirmed_step)
           && is_error(mapping.convert(realtime_before, 0U, device_clock, monotonic_after),
                       TimeError::MappingInvalidated)
           && rbt::time::checked_duration_since(monotonic_after, monotonic_before).value()
                  == std::chrono::nanoseconds(1);
}

bool test_host_clock_basis_and_realtime_discontinuity() {
    std::ifstream boot_file("/proc/sys/kernel/random/boot_id");
    std::string boot_text;
    struct stat time_namespace {};
    std::ifstream offsets_file(current_thread_offsets_path());
    if (!boot_file || !std::getline(boot_file, boot_text)
        || ::stat("/proc/thread-self/ns/time", &time_namespace) != 0) {
        return false;
    }
    std::array<std::uint8_t, 16> boot_uuid {};
    if (!decode_uuid(boot_text, boot_uuid)) {
        return false;
    }
    std::int64_t offset_seconds = 0;
    std::int64_t offset_nanoseconds = 0;
    std::string line;
    bool found_monotonic_offset = false;
    while (offsets_file && std::getline(offsets_file, line)) {
        std::istringstream parser(line);
        std::string clock_name;
        if (!(parser >> clock_name) || clock_name != "monotonic") {
            continue;
        }
        if (!(parser >> offset_seconds >> offset_nanoseconds)) {
            return false;
        }
        found_monotonic_offset = true;
        break;
    }
    auto host_result = HostClock::create();
    if (!offsets_file || !found_monotonic_offset) {
        return is_error(host_result, TimeError::UnknownClockDomain)
               && test_observed_realtime_step_keeps_steady_elapsed();
    }
    if (!host_result) {
        return false;
    }
    auto host = std::move(host_result).value();
    auto mono_before = host.now_monotonic();
    auto realtime_before = host.now_realtime();
    if (!mono_before || !realtime_before || mono_before.value().clock.kind() != ClockKind::HostMonotonic
        || realtime_before.value().clock.kind() != ClockKind::SystemRealtime
        || realtime_before.value().ns < 1000000000000000000LL) {
        return false;
    }
    std::array<std::uint8_t, 48> expected_monotonic {};
    std::array<std::uint8_t, 48> expected_realtime {};
    for (std::size_t index = 0U; index < boot_uuid.size(); ++index) {
        expected_monotonic[index] = boot_uuid[index];
        expected_realtime[index] = boot_uuid[index];
    }
    put_le(expected_monotonic, 16U, static_cast<std::uint64_t>(time_namespace.st_dev));
    put_le(expected_monotonic, 24U, static_cast<std::uint64_t>(time_namespace.st_ino));
    put_le(expected_monotonic, 32U, static_cast<std::uint64_t>(offset_seconds));
    put_le(expected_monotonic, 40U, static_cast<std::uint64_t>(offset_nanoseconds));
    if (mono_before.value().clock.source_id().bytes() != expected_monotonic
        || realtime_before.value().clock.source_id().bytes() != expected_realtime
        || mono_before.value().clock.generation() != 0U || realtime_before.value().clock.generation() != 0U) {
        return false;
    }

    auto source = host.realtime_identity();
    if (!source) {
        return false;
    }
    auto mapping_result = ClockMapping::create(
        calibration_for(source.value(), make_identity(ClockKind::Device, 0x60U),
                        mono_before.value().clock, 0, 0, 1U, 1U,
                        std::numeric_limits<std::int64_t>::min(),
                        std::numeric_limits<std::int64_t>::max()));
    if (!mapping_result) {
        return false;
    }
    auto mapping = std::move(mapping_result).value();
    auto confirmed_step = host.confirm_realtime_discontinuity();
    auto realtime_after = host.now_realtime();
    auto mono_after = host.now_monotonic();
    if (!confirmed_step || !realtime_after || !mono_after
        || confirmed_step.value().previous.generation() != 0U
        || confirmed_step.value().current.generation() != 1U
        || realtime_after.value().clock.generation() != 1U
        || !mapping.invalidate(confirmed_step.value())
        || !is_error(mapping.convert(realtime_before.value(), 0U,
                                     make_identity(ClockKind::Device, 0x60U), mono_after.value()),
                     TimeError::MappingInvalidated)) {
        return false;
    }
    auto elapsed = rbt::time::checked_duration_since(mono_after.value(), mono_before.value());
    return elapsed.has_value() && test_observed_realtime_step_keeps_steady_elapsed();
}

} // namespace

int main() {
    if (!test_source_id_and_comparison()) {
        std::cerr << "time_tests: source identity or checked comparison failed\n";
        return 1;
    }
    if (!test_device_reset_and_sensor_timing()) {
        std::cerr << "time_tests: device reset or sensor timing failed\n";
        return 2;
    }
    if (!test_mapping_conversion_and_validation() || !test_identity_mapping_calibration_invariant()) {
        std::cerr << "time_tests: mapping conversion or validation failed\n";
        return 3;
    }
    if (!test_mapping_overflow_and_invalidation()) {
        std::cerr << "time_tests: mapping overflow or invalidation failed\n";
        return 4;
    }
    if (!test_host_clock_basis_and_realtime_discontinuity()) {
        std::cerr << "time_tests: Linux host clock identity or realtime discontinuity failed\n";
        return 5;
    }
    return 0;
}

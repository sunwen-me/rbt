#ifndef RBT_NAVIGATION_RUNTIME_LIO_RECEIPT_CLOCK_HPP
#define RBT_NAVIGATION_RUNTIME_LIO_RECEIPT_CLOCK_HPP

#include <rbt/time.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace rbt::navigation::runtime {

/// Correlates MID360 mode-0 host receipt timestamps (CLOCK_REALTIME) with this
/// process's HostMonotonic clock. This is a receipt-epoch correlation only:
/// UDP transport, kernel queueing, sensor acquisition, and hardware latency
/// are unknown and are not represented by the returned uncertainty.
///
/// The bound assumes Linux CLOCK_REALTIME is slewed by at most 500 ppm between
/// observations. Offset changes beyond that rate plus 5 us measurement jitter
/// latch the clock as failed. An unobserved realtime step cannot be detected.
class LioReceiptClock {
  public:
    using Timestamp = rbt::time::Timestamp;
    using ConvertedTimestamp = rbt::time::ConvertedTimestamp;
    using TimeError = rbt::time::TimeError;
    using Result = rbt::time::Result<ConvertedTimestamp>;
    using Status = rbt::time::Result<void>;

    LioReceiptClock()
        : host_clock_(create_host_clock())
        , monotonic_identity_(host_clock_.monotonic_identity())
        , realtime_identity_(create_realtime_identity(host_clock_)) {
        auto initial_sample = sample();
        if (!initial_sample) {
            throw std::runtime_error(std::string("LioReceiptClock initial correlation failed: ")
                                     + rbt::time::to_string(initial_sample.error()));
        }
    }

    LioReceiptClock(const LioReceiptClock&) = delete;
    LioReceiptClock& operator=(const LioReceiptClock&) = delete;
    LioReceiptClock(LioReceiptClock&&) = delete;
    LioReceiptClock& operator=(LioReceiptClock&&) = delete;

    [[nodiscard]] const rbt::time::ClockIdentity& monotonic_identity() const noexcept {
        return monotonic_identity_;
    }

    [[nodiscard]] const rbt::time::ClockIdentity& realtime_identity() const noexcept {
        return realtime_identity_;
    }

    /// Reads monotonic, realtime, monotonic and refreshes the bounded mapping.
    /// An overwide read is rejected without refreshing the last valid anchor;
    /// a later valid read must still satisfy the original gap and step checks.
    [[nodiscard]] Status sample() noexcept {
        if (failed_) {
            return TimeError::MappingInvalidated;
        }
        std::optional<Timestamp> best_before;
        std::optional<Timestamp> best_realtime;
        std::optional<Timestamp> best_after;
        auto best_bracket = std::numeric_limits<std::uint64_t>::max();
        for (std::uint8_t attempt = 0U; attempt < 3U; ++attempt) {
            auto before = host_clock_.now_monotonic();
            auto realtime = host_clock_.now_realtime();
            auto after = host_clock_.now_monotonic();
            if (!before || !realtime || !after) {
                failed_ = true;
                if (!before) return before.error();
                if (!realtime) return realtime.error();
                return after.error();
            }
            if (after.value().ns < before.value().ns) {
                failed_ = true;
                return TimeError::TimeSourceReset;
            }
            const auto bracket = static_cast<std::uint64_t>(after.value().ns - before.value().ns);
            if (bracket < best_bracket) {
                best_before = before.value();
                best_realtime = realtime.value();
                best_after = after.value();
                best_bracket = bracket;
            }
            if (bracket <= kMaxBracketNs) {
                break;
            }
        }
        if (!best_before || !best_realtime || !best_after || best_bracket > kMaxBracketNs) {
            return TimeError::UnknownUncertainty;
        }
        return observe(*best_before, *best_realtime, *best_after);
    }

    /// Deterministic observation seam. The realtime read is bracketed by the
    /// two monotonic reads; the mapping anchor uses their midpoint.
    [[nodiscard]] Status observe(const Timestamp& before,
                                 const Timestamp& realtime,
                                 const Timestamp& after) noexcept {
        if (failed_) {
            return TimeError::MappingInvalidated;
        }
        if (!before.clock.is_well_formed() || !realtime.clock.is_well_formed()
            || !after.clock.is_well_formed()) {
            return TimeError::InvalidTimestamp;
        }
        if (before.clock != monotonic_identity_ || after.clock != monotonic_identity_) {
            return TimeError::ClockSourceMismatch;
        }
        if (realtime.clock != realtime_identity_) {
            return TimeError::ClockSourceMismatch;
        }
        if (before.ns < 0 || realtime.ns < 0 || after.ns < 0 || after.ns < before.ns) {
            failed_ = true;
            return TimeError::TimeSourceReset;
        }

        const auto bracket = static_cast<std::uint64_t>(after.ns - before.ns);
        if (bracket > kMaxBracketNs) {
            return TimeError::UnknownUncertainty;
        }
        const auto midpoint = before.ns + (after.ns - before.ns) / 2;
        const auto bracket_half_ns = (bracket + 1U) / 2U;

        if (has_observation_) {
            if (before.ns < last_after_ns_ || midpoint < last_midpoint_ns_
                || realtime.ns < last_realtime_ns_) {
                failed_ = true;
                return TimeError::TimeSourceReset;
            }
            const auto elapsed_mono = static_cast<std::uint64_t>(midpoint - last_midpoint_ns_);
            if (elapsed_mono > kMaxSampleGapNs) {
                failed_ = true;
                return TimeError::ClockMappingUnavailable;
            }
            const auto elapsed_real = static_cast<std::uint64_t>(realtime.ns - last_realtime_ns_);
            const auto offset_change = static_cast<std::int64_t>(elapsed_real)
                                       - static_cast<std::int64_t>(elapsed_mono);
            const auto magnitude = static_cast<std::uint64_t>(
                offset_change < 0 ? -offset_change : offset_change);
            const auto slew_bound = (elapsed_mono * kMaxSlewPpm + kPpmDenominator - 1U)
                                    / kPpmDenominator;
            const auto allowed_change = slew_bound + kOffsetJitterNs
                                        + last_bracket_half_ns_ + bracket_half_ns;
            if (magnitude > allowed_change) {
                failed_ = true;
                return TimeError::TimeSourceReset;
            }
        }

        if (!has_observation_) {
            first_realtime_ns_ = realtime.ns;
            has_observation_ = true;
        }
        const auto calibration_uncertainty = bracket_half_ns + kMaxDriftUncertaintyNs + kOffsetJitterNs;
        if (calibration_uncertainty + kSourceDoubleUncertaintyNs > kMaxTotalUncertaintyNs) {
            failed_ = true;
            return TimeError::UnknownUncertainty;
        }
        if (calibration_generation_ == std::numeric_limits<std::uint64_t>::max()) {
            failed_ = true;
            return TimeError::TimestampOverflow;
        }

        if (after.ns > std::numeric_limits<std::int64_t>::max()
                           - static_cast<std::int64_t>(kMappingLifetimeNs)) {
            failed_ = true;
            return TimeError::TimestampOverflow;
        }
        const auto expires_at_ns = after.ns + static_cast<std::int64_t>(kMappingLifetimeNs);
        rbt::time::ClockMappingCalibration calibration;
        calibration.source = realtime_identity_;
        calibration.target = monotonic_identity_;
        calibration.source_anchor_ns = realtime.ns;
        calibration.target_anchor_ns = midpoint;
        calibration.rate_numerator = 1U;
        calibration.rate_denominator = 1U;
        calibration.valid_from_ns = first_realtime_ns_;
        calibration.valid_until_ns = realtime.ns;
        calibration.calibration_id = next_calibration_id();
        calibration.calibrated_at = Timestamp { monotonic_identity_, midpoint };
        calibration.expires_at = Timestamp { monotonic_identity_, expires_at_ns };
        calibration.uncertainty_ns = calibration_uncertainty;
        auto mapping = rbt::time::ClockMapping::create(std::move(calibration));
        if (!mapping) {
            failed_ = true;
            return mapping.error();
        }

        mapping_.emplace(std::move(mapping).value());
        latest_realtime_ns_ = realtime.ns;
        last_realtime_ns_ = realtime.ns;
        last_midpoint_ns_ = midpoint;
        last_after_ns_ = after.ns;
        last_bracket_half_ns_ = bracket_half_ns;
        ++calibration_generation_;
        return {};
    }

    /// Converts only mode 0, whose timestamp is host CLOCK_REALTIME at UDP
    /// receipt. This overload refreshes the correlation before conversion.
    [[nodiscard]] Result convert(std::uint64_t realtime_ns, std::uint8_t mid360_time_type) noexcept {
        if (mid360_time_type != kHostRealtimeReceiptMode) {
            return TimeError::ClockMappingUnavailable;
        }
        auto sampled = sample();
        if (!sampled) {
            return sampled.error();
        }
        auto now = host_clock_.now_monotonic();
        if (!now) {
            failed_ = true;
            return now.error();
        }
        return convert(realtime_ns, mid360_time_type, now.value());
    }

    /// Deterministic conversion overload for callers that already have a
    /// current HostMonotonic timestamp.
    [[nodiscard]] Result convert(std::uint64_t realtime_ns,
                                 std::uint8_t mid360_time_type,
                                 const Timestamp& now) noexcept {
        if (mid360_time_type != kHostRealtimeReceiptMode) {
            return TimeError::ClockMappingUnavailable;
        }
        if (failed_) {
            return TimeError::MappingInvalidated;
        }
        if (!mapping_.has_value()) {
            return TimeError::ClockMappingUnavailable;
        }
        if (realtime_ns > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return TimeError::TimestampOverflow;
        }
        const auto event_ns = static_cast<std::int64_t>(realtime_ns);
        if (event_ns < first_realtime_ns_ || event_ns > latest_realtime_ns_) {
            return TimeError::TimestampOutOfRange;
        }
        const auto age_ns = static_cast<std::uint64_t>(latest_realtime_ns_ - event_ns);
        if (age_ns > kMaxEventAgeNs) {
            return TimeError::TimestampOutOfRange;
        }
        if (!now.clock.is_well_formed()) {
            return TimeError::InvalidTimestamp;
        }
        auto converted = mapping_->convert(Timestamp { realtime_identity_, event_ns },
                                           kSourceDoubleUncertaintyNs,
                                           monotonic_identity_,
                                           now);
        if (!converted) {
            return converted.error();
        }
        if (now.clock != monotonic_identity_) {
            return TimeError::ClockSourceMismatch;
        }
        if (now.ns < last_midpoint_ns_) {
            return TimeError::ClockMappingUnavailable;
        }
        if (static_cast<std::uint64_t>(now.ns - last_midpoint_ns_) > kMaxSampleGapNs) {
            failed_ = true;
            return TimeError::ClockMappingUnavailable;
        }
        if (converted.value().uncertainty_ns > kMaxTotalUncertaintyNs) {
            return TimeError::UnknownUncertainty;
        }
        if (has_converted_ && converted.value().timestamp.ns < last_converted_ns_) {
            return TimeError::TimestampOutOfRange;
        }
        last_converted_ns_ = converted.value().timestamp.ns;
        has_converted_ = true;
        return std::move(converted).value();
    }

  private:
    static constexpr std::uint8_t kHostRealtimeReceiptMode = 0U;
    static constexpr std::uint64_t kMaxBracketNs = 200'000U;
    static constexpr std::uint64_t kMaxSampleGapNs = 200'000'000U;
    static constexpr std::uint64_t kMaxEventAgeNs = 250'000'000U;
    static constexpr std::uint64_t kMappingLifetimeNs = 300'000'000U;
    static constexpr std::uint64_t kMaxSlewPpm = 500U;
    static constexpr std::uint64_t kPpmDenominator = 1'000'000U;
    static constexpr std::uint64_t kOffsetJitterNs = 5'000U;
    static constexpr std::uint64_t kMaxDriftUncertaintyNs = 125'000U;
    static constexpr std::uint64_t kSourceDoubleUncertaintyNs = 512U;
    static constexpr std::uint64_t kMaxTotalUncertaintyNs = 250'000U;

    static rbt::time::HostClock create_host_clock() {
        auto clock = rbt::time::HostClock::create();
        if (!clock) {
            throw std::runtime_error("RBT HostClock unavailable for LioReceiptClock");
        }
        return std::move(clock).value();
    }

    static rbt::time::ClockIdentity create_realtime_identity(const rbt::time::HostClock& clock) {
        auto identity = clock.realtime_identity();
        if (!identity) {
            throw std::runtime_error("RBT realtime identity unavailable for LioReceiptClock");
        }
        return std::move(identity).value();
    }

    [[nodiscard]] std::array<std::uint8_t, 16> next_calibration_id() const noexcept {
        std::array<std::uint8_t, 16> id {};
        auto generation = calibration_generation_ + 1U;
        for (std::size_t index = 0U; index < sizeof(generation); ++index) {
            id[index] = static_cast<std::uint8_t>((generation >> (index * 8U)) & 0xFFU);
        }
        id.back() = 0xA5U;
        return id;
    }

    rbt::time::HostClock host_clock_;
    rbt::time::ClockIdentity monotonic_identity_;
    rbt::time::ClockIdentity realtime_identity_;
    std::optional<rbt::time::ClockMapping> mapping_;
    std::int64_t first_realtime_ns_ { 0 };
    std::int64_t latest_realtime_ns_ { 0 };
    std::int64_t last_realtime_ns_ { 0 };
    std::int64_t last_midpoint_ns_ { 0 };
    std::int64_t last_after_ns_ { 0 };
    std::uint64_t last_bracket_half_ns_ { 0U };
    std::int64_t last_converted_ns_ { 0 };
    std::uint64_t calibration_generation_ { 0U };
    bool has_observation_ { false };
    bool has_converted_ { false };
    bool failed_ { false };
};

} // namespace rbt::navigation::runtime

#endif // RBT_NAVIGATION_RUNTIME_LIO_RECEIPT_CLOCK_HPP

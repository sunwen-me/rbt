#include <rbt_navigation/runtime/lio_receipt_clock.hpp>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <utility>

namespace {

using rbt::navigation::runtime::LioReceiptClock;
using rbt::time::ClockIdentity;
using rbt::time::ClockKind;
using rbt::time::ClockSourceId;
using rbt::time::HostClock;
using rbt::time::TimeError;
using rbt::time::Timestamp;

constexpr std::int64_t kMs = 1'000'000;
constexpr std::int64_t kSecond = 1'000'000'000;

ClockIdentity make_identity(ClockKind kind, std::uint8_t seed) {
    std::array<std::uint8_t, 48> bytes {};
    for (std::size_t index = 0U; index < 16U; ++index) {
        bytes[index] = static_cast<std::uint8_t>(seed + index);
    }
    auto identity = ClockIdentity::create(kind, ClockSourceId(std::move(bytes)));
    if (!identity) std::terminate();
    return std::move(identity).value();
}

HostClock make_host_clock() {
    auto clock = HostClock::create();
    if (!clock) std::terminate();
    return std::move(clock).value();
}

struct Fixture {
    LioReceiptClock receipt;
    HostClock reference_clock;
    std::int64_t base_mono_ns;
    std::int64_t base_real_ns;

    Fixture()
        : receipt()
        , reference_clock(make_host_clock()) {
        auto mono = reference_clock.now_monotonic();
        auto real = reference_clock.now_realtime();
        if (!mono || !real) std::terminate();
        base_mono_ns = mono.value().ns + 20 * kMs;
        base_real_ns = real.value().ns + 20 * kMs;
    }

    LioReceiptClock::Status observe(std::int64_t mono_delta_ns,
                                    std::int64_t real_delta_ns,
                                    std::int64_t bracket_ns = 20'000) {
        const auto before_ns = base_mono_ns + mono_delta_ns;
        return receipt.observe(Timestamp { receipt.monotonic_identity(), before_ns },
                               Timestamp { receipt.realtime_identity(), base_real_ns + real_delta_ns },
                               Timestamp { receipt.monotonic_identity(), before_ns + bracket_ns });
    }

    Timestamp now_after(std::int64_t mono_delta_ns,
                        std::int64_t bracket_ns = 20'000,
                        std::int64_t extra_ns = kMs) const {
        return Timestamp { receipt.monotonic_identity(),
                           base_mono_ns + mono_delta_ns + bracket_ns + extra_ns };
    }
};

bool test_old_epoch_and_uncertainty() {
    Fixture fixture;
    if (!fixture.observe(0, 0) || !fixture.observe(100 * kMs, 100 * kMs)) return false;
    const auto event_ns = static_cast<std::uint64_t>(fixture.base_real_ns + 50 * kMs);
    auto converted = fixture.receipt.convert(event_ns, 0U, fixture.now_after(100 * kMs));
    if (!converted) return false;
    const auto expected_ns = fixture.base_mono_ns + 50 * kMs + 10'000;
    if (converted.value().timestamp.ns != expected_ns
        || converted.value().timestamp.clock != fixture.receipt.monotonic_identity()
        || converted.value().uncertainty_ns != 140'512U) {
        return false;
    }

    auto warmup_event = fixture.receipt.convert(static_cast<std::uint64_t>(fixture.base_real_ns - 1),
                                               0U,
                                               fixture.now_after(100 * kMs));
    auto future_event = fixture.receipt.convert(static_cast<std::uint64_t>(fixture.base_real_ns + 100 * kMs + 1),
                                                0U,
                                                fixture.now_after(100 * kMs));
    auto old_mode = fixture.receipt.convert(event_ns, 1U, fixture.now_after(100 * kMs));
    auto device_mode = fixture.receipt.convert(event_ns, 2U, fixture.now_after(100 * kMs));
    auto out_of_order = fixture.receipt.convert(static_cast<std::uint64_t>(fixture.base_real_ns + 49 * kMs),
                                                0U,
                                                fixture.now_after(100 * kMs));
    return !warmup_event && warmup_event.error() == TimeError::TimestampOutOfRange
           && !future_event && future_event.error() == TimeError::TimestampOutOfRange
           && !old_mode && !device_mode && !out_of_order
           && out_of_order.error() == TimeError::TimestampOutOfRange;
}

bool test_age_window_and_expiry() {
    Fixture fixture;
    if (!fixture.observe(0, 0) || !fixture.observe(150 * kMs, 150 * kMs)
        || !fixture.observe(250 * kMs, 250 * kMs)) {
        return false;
    }
    auto maximum_age = fixture.receipt.convert(static_cast<std::uint64_t>(fixture.base_real_ns),
                                               0U,
                                               fixture.now_after(250 * kMs));
    if (!maximum_age || maximum_age.value().uncertainty_ns > 250'000U) return false;
    auto stale = fixture.receipt.convert(static_cast<std::uint64_t>(fixture.base_real_ns - 1),
                                         0U,
                                         fixture.now_after(250 * kMs));
    if (stale || stale.error() != TimeError::TimestampOutOfRange
        || !fixture.observe(300 * kMs, 300 * kMs)) {
        return false;
    }
    auto too_old = fixture.receipt.convert(static_cast<std::uint64_t>(fixture.base_real_ns + 49 * kMs + 999'999),
                                           0U,
                                           fixture.now_after(300 * kMs));
    if (too_old || too_old.error() != TimeError::TimestampOutOfRange) return false;

    auto expired = fixture.receipt.convert(static_cast<std::uint64_t>(fixture.base_real_ns + 300 * kMs),
                                           0U,
                                           fixture.now_after(300 * kMs, 20'000, 301 * kMs));
    return !expired && expired.error() == TimeError::ClockMappingExpired;
}

bool test_identity_rejection() {
    Fixture fixture;
    const auto before_ns = fixture.base_mono_ns;
    auto wrong_source = fixture.receipt.observe(
        Timestamp { fixture.receipt.monotonic_identity(), before_ns },
        Timestamp { make_identity(ClockKind::SystemRealtime, 0x41U), fixture.base_real_ns },
        Timestamp { fixture.receipt.monotonic_identity(), before_ns + 20'000 });
    if (wrong_source || wrong_source.error() != TimeError::ClockSourceMismatch) return false;
    auto wrong_monotonic = fixture.receipt.observe(
        Timestamp { make_identity(ClockKind::HostMonotonic, 0x61U), before_ns },
        Timestamp { fixture.receipt.realtime_identity(), fixture.base_real_ns },
        Timestamp { fixture.receipt.monotonic_identity(), before_ns + 20'000 });
    if (wrong_monotonic || wrong_monotonic.error() != TimeError::ClockSourceMismatch) return false;
    if (!fixture.observe(0, 0) || !fixture.observe(50 * kMs, 50 * kMs)) return false;

    auto wrong_target = fixture.receipt.convert(static_cast<std::uint64_t>(fixture.base_real_ns),
                                                0U,
                                                Timestamp { make_identity(ClockKind::HostMonotonic, 0x81U),
                                                            fixture.base_mono_ns + 51 * kMs });
    return !wrong_target && wrong_target.error() == TimeError::ClockSourceMismatch;
}

bool test_wall_step_latches_and_slew_is_bounded() {
    Fixture stepped;
    if (!stepped.observe(0, 0)) return false;
    auto step = stepped.observe(100 * kMs, 100 * kMs + 100'000);
    if (step || step.error() != TimeError::TimeSourceReset) return false;
    auto after_step = stepped.observe(150 * kMs, 150 * kMs);
    auto converted_after_step = stepped.receipt.convert(
        static_cast<std::uint64_t>(stepped.base_real_ns),
        0U,
        stepped.now_after(150 * kMs));
    if (after_step || converted_after_step) return false;

    Fixture slewed;
    if (!slewed.observe(0, 0) || !slewed.observe(100 * kMs, 100 * kMs + 40'000)
        || !slewed.observe(200 * kMs, 200 * kMs + 80'000)) {
        return false;
    }
    auto converted = slewed.receipt.convert(
        static_cast<std::uint64_t>(slewed.base_real_ns + 150 * kMs + 60'000),
        0U,
        slewed.now_after(200 * kMs));
    return converted && converted.value().uncertainty_ns <= 250'000U;
}

bool test_bracket_position_variation() {
    Fixture fixture;
    if (!fixture.observe(0, 0) || !fixture.observe(kMs, kMs + 20'000)) return false;
    auto converted = fixture.receipt.convert(static_cast<std::uint64_t>(fixture.base_real_ns + kMs + 20'000),
                                             0U,
                                             fixture.now_after(kMs));
    return converted && converted.value().uncertainty_ns <= 250'000U;
}

bool test_maximum_bracket_uncertainty() {
    Fixture fixture;
    if (!fixture.observe(0, 0, 200'000) || !fixture.observe(100 * kMs, 100 * kMs, 200'000)) {
        return false;
    }
    auto converted = fixture.receipt.convert(static_cast<std::uint64_t>(fixture.base_real_ns + 100 * kMs),
                                             0U,
                                             fixture.now_after(100 * kMs, 200'000));
    if (!converted || converted.value().uncertainty_ns != 230'512U
        || converted.value().uncertainty_ns > 250'000U) {
        return false;
    }

    Fixture over_limit;
    auto rejected = over_limit.observe(0, 0, 200'001);
    return !rejected && rejected.error() == TimeError::UnknownUncertainty;
}

bool test_overwide_observation_recovery() {
    Fixture fixture;
    if (!fixture.observe(0, 0)) return false;
    auto overwide = fixture.observe(50 * kMs, 50 * kMs, 200'001);
    if (overwide || overwide.error() != TimeError::UnknownUncertainty) return false;

    auto retained = fixture.receipt.convert(static_cast<std::uint64_t>(fixture.base_real_ns),
                                            0U,
                                            fixture.now_after(0));
    if (!retained || retained.value().timestamp.ns != fixture.base_mono_ns + 10'000) return false;
    if (!fixture.observe(100 * kMs, 100 * kMs)) return false;
    auto recovered = fixture.receipt.convert(static_cast<std::uint64_t>(fixture.base_real_ns + 100 * kMs),
                                             0U,
                                             fixture.now_after(100 * kMs));
    return recovered && recovered.value().timestamp.ns == fixture.base_mono_ns + 100 * kMs + 10'000;
}

bool test_overwide_does_not_refresh_sample_gap() {
    Fixture fixture;
    if (!fixture.observe(0, 0)) return false;
    auto overwide = fixture.observe(150 * kMs, 150 * kMs, 200'001);
    if (overwide || overwide.error() != TimeError::UnknownUncertainty) return false;
    auto too_late = fixture.observe(201 * kMs, 201 * kMs);
    return !too_late && too_late.error() == TimeError::ClockMappingUnavailable;
}

bool test_wall_step_after_overwide_observation() {
    Fixture fixture;
    if (!fixture.observe(0, 0)) return false;
    auto overwide = fixture.observe(50 * kMs, 50 * kMs + 100'000, 200'001);
    if (overwide || overwide.error() != TimeError::UnknownUncertainty) return false;
    auto stepped = fixture.observe(100 * kMs, 100 * kMs + 100'000);
    return !stepped && stepped.error() == TimeError::TimeSourceReset;
}

bool test_sample_gap_and_source_overflow() {
    Fixture fixture;
    if (!fixture.observe(0, 0)) return false;
    auto gap = fixture.observe(201 * kMs, 201 * kMs);
    if (gap || gap.error() != TimeError::ClockMappingUnavailable) return false;
    auto converted = fixture.receipt.convert(std::numeric_limits<std::uint64_t>::max(), 0U);
    return !converted;
}

} // namespace

int main() {
    if (!test_old_epoch_and_uncertainty()) {
        std::cerr << "receipt clock event epoch or uncertainty failed\n";
        return 1;
    }
    if (!test_age_window_and_expiry()) {
        std::cerr << "receipt clock freshness bounds failed\n";
        return 2;
    }
    if (!test_identity_rejection()) {
        std::cerr << "receipt clock identity checks failed\n";
        return 3;
    }
    if (!test_wall_step_latches_and_slew_is_bounded()) {
        std::cerr << "receipt clock offset tracking failed\n";
        return 4;
    }
    if (!test_bracket_position_variation()) {
        std::cerr << "receipt clock bracket position tolerance failed\n";
        return 5;
    }
    if (!test_maximum_bracket_uncertainty()) {
        std::cerr << "receipt clock maximum bracket uncertainty failed\n";
        return 6;
    }
    if (!test_overwide_observation_recovery()) {
        std::cerr << "receipt clock overwide observation recovery failed\n";
        return 7;
    }
    if (!test_overwide_does_not_refresh_sample_gap()) {
        std::cerr << "receipt clock overwide sample gap tracking failed\n";
        return 8;
    }
    if (!test_wall_step_after_overwide_observation()) {
        std::cerr << "receipt clock wall-step tracking after overwide observation failed\n";
        return 9;
    }
    if (!test_sample_gap_and_source_overflow()) {
        std::cerr << "receipt clock observation gap or overflow failed\n";
        return 10;
    }
    std::cout << "MID360 mode-0 receipt clock mapping: PASS\n";
}

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <rbt/tf_wire.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using rbt::CapabilityInstanceId;
using rbt::tf::Authority;
using rbt::tf::DynamicRecord;
using rbt::tf::FrameId;
using rbt::tf::LookupOptions;
using rbt::tf::LookupPolicy;
using rbt::tf::Quaternion;
using rbt::tf::StaticEdge;
using rbt::tf::StaticSet;
using rbt::tf::TfError;
using rbt::tf::TfErrorCode;
using rbt::tf::TfResourceConfig;
using rbt::tf::Transform3;
using rbt::tf::TransformBuffer;
using rbt::tf::Vec3;
using rbt::time::ClockIdentity;
using rbt::time::ClockKind;
using rbt::time::ClockMapping;
using rbt::time::ClockMappingCalibration;
using rbt::time::ClockSourceId;
using rbt::time::TimeError;
using rbt::time::Timestamp;

ClockIdentity clock_identity(ClockKind kind, std::uint8_t seed, std::uint64_t generation = 0U) {
    std::array<std::uint8_t, 48U> bytes {};
    for (std::size_t index = 0U; index < 16U; ++index) {
        bytes[index] = static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(index));
    }
    auto result = ClockIdentity::create(kind, ClockSourceId(std::move(bytes)), generation);
    assert(result);
    return std::move(result).value();
}

CapabilityInstanceId uuid(std::uint16_t suffix) {
    static constexpr char digits[] = "0123456789abcdef";
    CapabilityInstanceId result = "00000000-0000-0000-0000-000000000000";
    for (std::size_t index = 0U; index < 4U; ++index) {
        result[35U - index] = digits[suffix & 0x0FU];
        suffix = static_cast<std::uint16_t>(suffix >> 4U);
    }
    assert(suffix == 0U);
    return result;
}

Transform3 translated(double x, double y = 0.0, double z = 0.0) {
    return Transform3(Vec3(x, y, z), Quaternion::identity());
}

DynamicRecord dynamic_record(std::string_view parent,
                             std::string_view child,
                             const ClockIdentity& clock,
                             std::int64_t ns,
                             Transform3 transform,
                             std::uint64_t epoch = 0U,
                             std::optional<std::uint64_t> uncertainty = 1U) {
    return DynamicRecord { FrameId(parent), FrameId(child), Timestamp { clock, ns }, uncertainty, epoch, transform };
}

StaticSet static_set(std::uint64_t revision,
                     const ClockIdentity& clock,
                     std::int64_t effective_ns,
                     std::vector<StaticEdge> edges) {
    return StaticSet { revision, Timestamp { clock, effective_ns }, 1U, std::move(edges) };
}

ClockMapping mapping(const ClockIdentity& source,
                     const ClockIdentity& target,
                     const ClockIdentity& calibration_clock,
                     std::int64_t expires_at = 500) {
    ClockMappingCalibration calibration;
    calibration.source = source;
    calibration.target = target;
    calibration.source_anchor_ns = 0;
    calibration.target_anchor_ns = 0;
    calibration.rate_numerator = 1U;
    calibration.rate_denominator = 1U;
    calibration.valid_from_ns = -10'000;
    calibration.valid_until_ns = 10'000;
    calibration.calibration_id[0] = 1U;
    calibration.calibrated_at = Timestamp { calibration_clock, 10 };
    calibration.expires_at = Timestamp { calibration_clock, expires_at };
    calibration.uncertainty_ns = 2U;
    auto result = ClockMapping::create(std::move(calibration));
    assert(result);
    return std::move(result).value();
}

LookupOptions options(std::optional<std::uint64_t> uncertainty = 1U) {
    LookupOptions result;
    result.timestamp_uncertainty_ns = uncertainty;
    return result;
}

template <typename Callable>
void expect_error(TfErrorCode code,
                  Callable&& callable,
                  std::optional<TimeError> time_error = {}) {
    bool threw = false;
    try {
        std::forward<Callable>(callable)();
    } catch (const TfError& error) {
        threw = true;
        assert(error.code() == code);
        assert(error.time_error() == time_error);
    }
    assert(threw);
}

void test_graph_direction_lca_conflicts_and_recovery() {
    const auto clock = clock_identity(ClockKind::HostMonotonic, 1U);
    TransformBuffer buffer;
    const auto first = buffer.bind_provider("vision", uuid(1U));
    buffer.ingest_dynamic(first, dynamic_record("world", "base", clock, 100, translated(1.0)));
    buffer.ingest_dynamic(first, dynamic_record("base", "camera", clock, 100, translated(2.0)));

    const auto whole = buffer.lookup_at("world", "camera", Timestamp { clock, 100 }, options());
    assert(whole.transform.translation().x() == 3.0);
    const auto reverse = buffer.lookup_at("camera", "world", Timestamp { clock, 100 }, options());
    assert(reverse.transform.translation().x() == -3.0);
    // The source is the LCA here; its parent edge must not be evaluated.
    const auto reverse_one_edge = buffer.lookup_at("camera", "base", Timestamp { clock, 100 }, options());
    assert(reverse_one_edge.transform.translation().x() == -2.0);
    assert(reverse_one_edge.sources.size() == 1U);

    expect_error(TfErrorCode::CycleDetected, [&] {
        buffer.ingest_dynamic(first, dynamic_record("camera", "world", clock, 110, translated(0.0)));
    });

    const auto competing_parent = buffer.bind_provider("external", uuid(2U));
    expect_error(TfErrorCode::ConflictingParent, [&] {
        buffer.ingest_dynamic(competing_parent,
                              dynamic_record("other_base", "camera", clock, 100, translated(0.0)));
    });
    expect_error(TfErrorCode::ConflictingParent, [&] {
        (void)buffer.lookup_at("world", "camera", Timestamp { clock, 100 }, options());
    });
    buffer.unbind_provider(competing_parent);
    assert(buffer.lookup_at("world", "camera", Timestamp { clock, 100 }, options())
               .transform.translation().x()
           == 3.0);

    const auto competing_authority = buffer.bind_provider("external", uuid(3U));
    expect_error(TfErrorCode::ConflictingAuthority, [&] {
        buffer.ingest_dynamic(competing_authority,
                              dynamic_record("base", "camera", clock, 100, translated(0.0)));
    });
    expect_error(TfErrorCode::ConflictingAuthority, [&] {
        (void)buffer.lookup_at("world", "camera", Timestamp { clock, 100 }, options());
    });
    buffer.unbind_provider(competing_authority);
    assert(buffer.lookup_at("world", "camera", Timestamp { clock, 100 }, options())
               .transform.translation().x()
           == 3.0);
    expect_error(TfErrorCode::ProviderUnavailable, [&] { buffer.unbind_provider(competing_authority); });
}

void test_interpolation_extrapolation_gap_epoch_and_duplicate_rules() {
    const auto clock = clock_identity(ClockKind::HostMonotonic, 2U);
    LookupPolicy policy;
    policy.max_interpolation_gap = 100ns;
    TransformBuffer buffer({}, policy);
    const auto authority = buffer.bind_provider("motion", uuid(4U));
    buffer.ingest_dynamic(authority, dynamic_record("world", "tool", clock, 100, translated(1.0)));
    buffer.ingest_dynamic(authority, dynamic_record("world", "tool", clock, 200, translated(3.0)));
    // An exact duplicate is idempotent.
    buffer.ingest_dynamic(authority, dynamic_record("world", "tool", clock, 200, translated(3.0)));
    expect_error(TfErrorCode::OutOfOrderSample, [&] {
        buffer.ingest_dynamic(authority, dynamic_record("world", "tool", clock, 200, translated(4.0)));
    });

    const auto midpoint = buffer.lookup_at("world", "tool", Timestamp { clock, 150 }, options());
    assert(midpoint.transform.translation().x() == 2.0);
    assert(midpoint.sources.size() == 1U);
    assert(midpoint.sources.front().bracket_start.ns == 100);
    assert(midpoint.sources.front().bracket_end.ns == 200);
    expect_error(TfErrorCode::PastExtrapolation, [&] {
        (void)buffer.lookup_at("world", "tool", Timestamp { clock, 99 }, options());
    });
    expect_error(TfErrorCode::FutureExtrapolation, [&] {
        (void)buffer.lookup_at("world", "tool", Timestamp { clock, 201 }, options());
    });

    buffer.ingest_dynamic(authority, dynamic_record("world", "tool", clock, 400, translated(4.0), 0U));
    expect_error(TfErrorCode::InterpolationGapTooLarge, [&] {
        (void)buffer.lookup_at("world", "tool", Timestamp { clock, 300 }, options());
    });
    buffer.ingest_dynamic(authority, dynamic_record("world", "tool", clock, 500, translated(5.0), 1U));
    expect_error(TfErrorCode::TransformEpochMismatch, [&] {
        (void)buffer.lookup_at("world", "tool", Timestamp { clock, 450 }, options());
    });
    expect_error(TfErrorCode::TransformEpochMismatch, [&] {
        buffer.ingest_dynamic(authority, dynamic_record("world", "tool", clock, 600, translated(6.0), 0U));
    });
    assert(buffer.lookup_at("world", "tool", Timestamp { clock, 500 }, options())
               .transform.translation().x()
           == 5.0);
}

void test_clock_mapping_query_direction_and_errors() {
    const auto query_clock = clock_identity(ClockKind::Device, 10U);
    const auto history_clock = clock_identity(ClockKind::HostMonotonic, 20U);
    const auto calibration_clock = clock_identity(ClockKind::HostMonotonic, 30U);
    TransformBuffer buffer;
    const auto authority = buffer.bind_provider("camera_tf", uuid(5U));
    buffer.ingest_dynamic(authority,
                          dynamic_record("base", "camera", history_clock, 100, translated(1.0)));
    buffer.ingest_dynamic(authority,
                          dynamic_record("base", "camera", history_clock, 200, translated(3.0)));

    auto valid_mapping = mapping(query_clock, history_clock, calibration_clock);
    std::array<const ClockMapping*, 1U> mappings { &valid_mapping };
    const Timestamp now { calibration_clock, 100 };
    auto lookup_options = options(1U);
    lookup_options.mappings = std::span<const ClockMapping* const>(mappings);
    lookup_options.now = &now;
    const auto result = buffer.lookup_at("base", "camera", Timestamp { query_clock, 150 }, lookup_options);
    assert(result.transform.translation().x() == 2.0);
    assert(result.requested_timestamp.clock == query_clock);
    assert(result.requested_timestamp.ns == 150);
    assert(result.sources.front().timestamp.clock == history_clock);
    assert(result.sources.front().timestamp.ns == 150);

    auto expired_mapping = mapping(query_clock, history_clock, calibration_clock, 200);
    std::array<const ClockMapping*, 1U> expired_mappings { &expired_mapping };
    const Timestamp expired_now { calibration_clock, 200 };
    auto expired_options = options(1U);
    expired_options.mappings = std::span<const ClockMapping* const>(expired_mappings);
    expired_options.now = &expired_now;
    expect_error(TfErrorCode::ClockMappingExpired,
                 [&] {
                     (void)buffer.lookup_at("base", "camera", Timestamp { query_clock, 150 }, expired_options);
                 },
                 TimeError::ClockMappingExpired);

    const auto wrong_generation = clock_identity(ClockKind::HostMonotonic, 20U, 1U);
    auto generation_mapping = mapping(query_clock, wrong_generation, calibration_clock);
    std::array<const ClockMapping*, 1U> generation_mappings { &generation_mapping };
    auto generation_options = options(1U);
    generation_options.mappings = std::span<const ClockMapping* const>(generation_mappings);
    generation_options.now = &now;
    expect_error(TfErrorCode::ClockGenerationMismatch,
                 [&] {
                     (void)buffer.lookup_at("base", "camera", Timestamp { query_clock, 150 }, generation_options);
                 },
                 TimeError::ClockGenerationMismatch);

    const auto wrong_domain = clock_identity(ClockKind::SystemRealtime, 40U);
    auto domain_mapping = mapping(query_clock, wrong_domain, calibration_clock);
    std::array<const ClockMapping*, 1U> domain_mappings { &domain_mapping };
    auto domain_options = options(1U);
    domain_options.mappings = std::span<const ClockMapping* const>(domain_mappings);
    domain_options.now = &now;
    expect_error(TfErrorCode::ClockMappingUnavailable,
                 [&] {
                     (void)buffer.lookup_at("base", "camera", Timestamp { query_clock, 150 }, domain_options);
                 },
                 TimeError::ClockMappingUnavailable);

    const auto device_a = clock_identity(ClockKind::Device, 41U);
    const auto device_b = clock_identity(ClockKind::Device, 42U);
    const auto device_c = clock_identity(ClockKind::Device, 43U);
    const auto device_d = clock_identity(ClockKind::Device, 44U);
    TransformBuffer unrelated_mapping_buffer;
    const auto unrelated_authority = unrelated_mapping_buffer.bind_provider("device_b_history", uuid(21U));
    unrelated_mapping_buffer.ingest_dynamic(
        unrelated_authority,
        dynamic_record("base", "camera", device_b, 100, translated(1.0)));
    const auto unrelated_mapping_c = mapping(device_a, device_c, calibration_clock);
    const auto unrelated_mapping_d = mapping(device_a, device_d, calibration_clock);
    std::array<const ClockMapping*, 2U> unrelated_mappings { &unrelated_mapping_c, &unrelated_mapping_d };
    std::array<const ClockMapping*, 2U> reversed_unrelated_mappings { &unrelated_mapping_d,
                                                                      &unrelated_mapping_c };
    auto unrelated_options = options(1U);
    unrelated_options.mappings = std::span<const ClockMapping* const>(unrelated_mappings);
    unrelated_options.now = &now;
    expect_error(TfErrorCode::ClockMappingUnavailable,
                 [&] {
                     (void)unrelated_mapping_buffer.lookup_at(
                         "base", "camera", Timestamp { device_a, 100 }, unrelated_options);
                 },
                 TimeError::ClockMappingUnavailable);
    unrelated_options.mappings = std::span<const ClockMapping* const>(reversed_unrelated_mappings);
    expect_error(TfErrorCode::ClockMappingUnavailable,
                 [&] {
                     (void)unrelated_mapping_buffer.lookup_at(
                         "base", "camera", Timestamp { device_a, 100 }, unrelated_options);
                 },
                 TimeError::ClockMappingUnavailable);

    const auto old_query_clock = clock_identity(ClockKind::Device, 10U, 1U);
    auto old_source_mapping = mapping(old_query_clock, history_clock, calibration_clock);
    std::array<const ClockMapping*, 1U> old_source_mappings { &old_source_mapping };
    auto old_source_options = options(1U);
    old_source_options.mappings = std::span<const ClockMapping* const>(old_source_mappings);
    old_source_options.now = &now;
    expect_error(TfErrorCode::ClockGenerationMismatch,
                 [&] {
                     (void)buffer.lookup_at("base", "camera", Timestamp { query_clock, 150 }, old_source_options);
                 },
                 TimeError::ClockGenerationMismatch);

    auto no_mapping_options = options(1U);
    expect_error(TfErrorCode::ClockMappingUnavailable,
                 [&] {
                     (void)buffer.lookup_at("base", "camera", Timestamp { query_clock, 150 }, no_mapping_options);
                 },
                 TimeError::ClockMappingUnavailable);
    expect_error(TfErrorCode::UnknownUncertainty,
                 [&] {
                     (void)buffer.lookup_at("base", "camera", Timestamp { query_clock, 150 }, options({}));
                 },
                 TimeError::UnknownUncertainty);
}

void test_unknown_disconnected_and_static_clock_mapping() {
    const auto query_clock = clock_identity(ClockKind::Device, 21U);
    const auto edge_clock = clock_identity(ClockKind::HostMonotonic, 22U);
    const auto calibration_clock = clock_identity(ClockKind::HostMonotonic, 23U);
    TransformBuffer buffer;
    expect_error(TfErrorCode::UnknownFrame, [&] {
        (void)buffer.lookup_at("world", "camera", Timestamp { edge_clock, 100 }, options());
    });
    const auto authority = buffer.bind_provider("static_map", uuid(14U));
    buffer.apply_static_snapshot(
        authority,
        static_set(1U,
                   edge_clock,
                   100,
                   { StaticEdge { FrameId("world"), FrameId("camera"), translated(2.0) } }));
    const auto query_to_edge = mapping(query_clock, edge_clock, calibration_clock);
    std::array<const ClockMapping*, 1U> mappings { &query_to_edge };
    const Timestamp now { calibration_clock, 100 };
    auto mapped_options = options(1U);
    mapped_options.mappings = std::span<const ClockMapping* const>(mappings);
    mapped_options.now = &now;
    const auto result = buffer.lookup_at("world", "camera", Timestamp { query_clock, 150 }, mapped_options);
    assert(result.transform.translation().x() == 2.0);
    assert(result.sources.front().timestamp.clock == edge_clock);
    assert(result.sources.front().timestamp.ns == 150);
    expect_error(TfErrorCode::PastExtrapolation, [&] {
        (void)buffer.lookup_at("world", "camera", Timestamp { query_clock, 50 }, mapped_options);
    });

    const auto disconnected = buffer.bind_provider("other_static", uuid(15U));
    buffer.apply_static_snapshot(
        disconnected,
        static_set(1U,
                   edge_clock,
                   0,
                   { StaticEdge { FrameId("left"), FrameId("right"), translated(0.0) } }));
    expect_error(TfErrorCode::DisconnectedFrames, [&] {
        (void)buffer.lookup_at("world", "right", Timestamp { edge_clock, 150 }, options());
    });
}

void test_static_revision_races_gaps_and_effective_time() {
    const auto clock = clock_identity(ClockKind::HostMonotonic, 50U);
    TransformBuffer buffer;
    const auto authority = buffer.bind_provider("static_provider", uuid(6U));
    const auto ten = static_set(10U, clock, 100, { StaticEdge { FrameId("world"), FrameId("camera"), translated(10.0) } });
    const auto eleven = static_set(11U, clock, 110, { StaticEdge { FrameId("world"), FrameId("camera"), translated(11.0) } });
    buffer.apply_static_update(authority, eleven);
    buffer.apply_static_snapshot(authority, ten);
    const auto merged = buffer.lookup_at("world", "camera", Timestamp { clock, 120 }, options());
    assert(merged.transform.translation().x() == 11.0);
    assert(merged.sources.front().static_revision == 11U);

    // A late revision 10 snapshot is ignored once revision 11 is current.
    buffer.apply_static_snapshot(authority, ten);
    assert(buffer.lookup_at("world", "camera", Timestamp { clock, 120 }, options())
               .transform.translation().x()
           == 11.0);
    expect_error(TfErrorCode::PastExtrapolation, [&] {
        (void)buffer.lookup_at("world", "camera", Timestamp { clock, 109 }, options());
    });

    TransformBuffer gap_buffer;
    const auto gap_authority = gap_buffer.bind_provider("gap_provider", uuid(7U));
    gap_buffer.apply_static_snapshot(gap_authority, ten);
    const auto twelve = static_set(12U, clock, 120, { StaticEdge { FrameId("world"), FrameId("camera"), translated(12.0) } });
    expect_error(TfErrorCode::SnapshotRevisionGap, [&] {
        gap_buffer.apply_static_update(gap_authority, twelve);
    });
    expect_error(TfErrorCode::SnapshotStale, [&] {
        (void)gap_buffer.lookup_at("world", "camera", Timestamp { clock, 130 }, options());
    });
    expect_error(TfErrorCode::SnapshotRevisionGap, [&] {
        gap_buffer.apply_static_snapshot(gap_authority, ten);
    });
    gap_buffer.apply_static_snapshot(gap_authority, twelve);
    assert(gap_buffer.lookup_at("world", "camera", Timestamp { clock, 130 }, options())
               .transform.translation().x()
           == 12.0);
}

void test_static_effective_time_chronology_clock_identity_and_rebind() {
    const auto clock = clock_identity(ClockKind::HostMonotonic, 70U);
    TransformBuffer buffer;
    const auto authority = buffer.bind_provider("chronology", uuid(26U));
    buffer.apply_static_snapshot(
        authority,
        static_set(1U, clock, 100, { StaticEdge { FrameId("base"), FrameId("camera"), translated(1.0) } }));

    expect_error(TfErrorCode::SnapshotRevisionGap, [&] {
        buffer.apply_static_update(
            authority,
            static_set(2U, clock, 99, { StaticEdge { FrameId("base"), FrameId("camera"), translated(2.0) } }));
    });
    expect_error(TfErrorCode::SnapshotStale, [&] {
        (void)buffer.lookup_at("base", "camera", Timestamp { clock, 101 }, options());
    });
    assert(buffer.pressure_snapshot().providers.front().static_revision == 1U);

    // A valid authoritative snapshot recovers; a later backdated snapshot
    // marks the provider stale without publishing the invalid revision.
    buffer.apply_static_snapshot(
        authority,
        static_set(2U, clock, 110, { StaticEdge { FrameId("base"), FrameId("camera"), translated(2.0) } }));
    expect_error(TfErrorCode::SnapshotRevisionGap, [&] {
        buffer.apply_static_snapshot(
            authority,
            static_set(5U, clock, 109, { StaticEdge { FrameId("base"), FrameId("camera"), translated(5.0) } }));
    });
    const auto stale = buffer.pressure_snapshot();
    assert(stale.providers.front().static_revision == 2U);
    assert(stale.providers.front().static_stale);

    const auto device_clock = clock_identity(ClockKind::Device, 71U);
    const auto next_generation = clock_identity(ClockKind::Device, 71U, 1U);
    const auto other_domain = clock_identity(ClockKind::HostMonotonic, 72U);
    for (const auto& [replacement_clock, expected_code, expected_time_error] : {
             std::tuple { next_generation, TfErrorCode::ClockGenerationMismatch, TimeError::ClockGenerationMismatch },
             std::tuple { other_domain, TfErrorCode::ClockDomainMismatch, TimeError::ClockDomainMismatch },
         }) {
        TransformBuffer clock_buffer;
        const auto clock_authority = clock_buffer.bind_provider("clock_change", uuid(27U));
        clock_buffer.apply_static_snapshot(
            clock_authority,
            static_set(1U, device_clock, 100, { StaticEdge { FrameId("base"), FrameId("camera"), translated(1.0) } }));
        expect_error(expected_code, [&] {
            clock_buffer.apply_static_update(
                clock_authority,
                static_set(2U,
                           replacement_clock,
                           110,
                           { StaticEdge { FrameId("base"), FrameId("camera"), translated(2.0) } }));
        }, expected_time_error);
        expect_error(TfErrorCode::SnapshotStale, [&] {
            (void)clock_buffer.lookup_at("base", "camera", Timestamp { device_clock, 120 }, options());
        });
    }

    TransformBuffer staged_buffer;
    const auto staged_authority = staged_buffer.bind_provider("staged", uuid(28U));
    staged_buffer.apply_static_update(
        staged_authority,
        static_set(11U, clock, 90, { StaticEdge { FrameId("base"), FrameId("camera"), translated(11.0) } }));
    expect_error(TfErrorCode::SnapshotRevisionGap, [&] {
        staged_buffer.apply_static_snapshot(
            staged_authority,
            static_set(10U, clock, 100, { StaticEdge { FrameId("base"), FrameId("camera"), translated(10.0) } }));
    });
    const auto staged_pressure = staged_buffer.pressure_snapshot();
    assert(!staged_pressure.providers.front().static_revision.has_value());
    assert(staged_pressure.providers.front().static_stale);

    const auto replacement = buffer.bind_provider("chronology", uuid(29U));
    const auto new_clock = clock_identity(ClockKind::Device, 73U);
    buffer.apply_static_snapshot(
        replacement,
        static_set(1U, new_clock, 0, { StaticEdge { FrameId("base"), FrameId("camera"), translated(7.0) } }));
    assert(buffer.lookup_at("base", "camera", Timestamp { new_clock, 1 }, options())
               .transform.translation().x()
           == 7.0);
    expect_error(TfErrorCode::ProviderUnavailable, [&] {
        buffer.apply_static_snapshot(
            authority,
            static_set(6U, clock, 111, { StaticEdge { FrameId("base"), FrameId("camera"), translated(6.0) } }));
    });
}

void test_resource_caps_provider_replacement_and_eviction() {
    TfResourceConfig invalid;
    invalid.max_total_edges = 0U;
    expect_error(TfErrorCode::InvalidConfig, [&] { invalid.validate(); });
    invalid = {};
    invalid.max_providers = 256U;
    invalid.max_pending_static_updates = 4096U;
    invalid.max_static_transforms_per_provider = 128U;
    expect_error(TfErrorCode::InvalidConfig, [&] { invalid.validate(); });

    TfResourceConfig one_provider_config;
    one_provider_config.max_providers = 1U;
    TransformBuffer one_provider(one_provider_config);
    (void)one_provider.bind_provider("one", uuid(8U));
    expect_error(TfErrorCode::CapacityExceeded, [&] {
        (void)one_provider.bind_provider("two", uuid(9U));
    });

    TransformBuffer replacements;
    auto current = replacements.bind_provider("rotating", uuid(100U));
    const auto stale = current;
    for (std::uint16_t index = 101U; index < 201U; ++index) {
        current = replacements.bind_provider("rotating", uuid(index));
    }
    const auto clock = clock_identity(ClockKind::HostMonotonic, 60U);
    expect_error(TfErrorCode::ProviderUnavailable, [&] {
        replacements.ingest_dynamic(stale, dynamic_record("world", "sensor", clock, 1, translated(0.0)));
    });
    expect_error(TfErrorCode::ProviderUnavailable, [&] { replacements.unbind_provider(stale); });
    expect_error(TfErrorCode::ConflictingAuthority, [&] {
        (void)replacements.bind_provider("another", current.physical());
    });

    TfResourceConfig bounded;
    bounded.max_samples_per_edge = 2U;
    bounded.max_total_samples = 2U;
    TransformBuffer evicting(bounded);
    const auto authority = evicting.bind_provider("bounded", uuid(10U));
    evicting.ingest_dynamic(authority, dynamic_record("root", "old", clock, 100, translated(0.0)));
    evicting.ingest_dynamic(authority, dynamic_record("root", "old", clock, 200, translated(1.0)));
    evicting.ingest_dynamic(authority, dynamic_record("root", "new", clock, 100, translated(2.0)));
    const auto pressure = evicting.pressure_snapshot();
    assert(pressure.stored_samples == 2U);
    assert(pressure.evicted_samples == 1U);
    expect_error(TfErrorCode::PastExtrapolation, [&] {
        (void)evicting.lookup_at("root", "old", Timestamp { clock, 100 }, options());
    });
}

void test_tf_diagnostic_counters_are_categorized_and_reset_on_rebind() {
    const auto clock = clock_identity(ClockKind::HostMonotonic, 66U);
    const auto foreign_clock = clock_identity(ClockKind::Device, 67U);
    TransformBuffer buffer;
    const auto authority = buffer.bind_provider("metrics", uuid(36U));

    expect_error(TfErrorCode::UnknownFrame, [&] {
        (void)buffer.lookup_at("missing_parent", "missing_child", Timestamp { clock, 100 }, options());
    });
    auto pressure = buffer.pressure_snapshot();
    assert(pressure.lookup_attempts == 1U);
    assert(pressure.lookup_failures == 1U);
    assert(pressure.missing_transform_count == 1U);
    assert(pressure.missing_frame_count == 1U);
    auto provider = std::find_if(pressure.providers.begin(), pressure.providers.end(),
                                 [&authority](const auto& observation) {
                                     return observation.authority == authority;
                                 });
    assert(provider != pressure.providers.end());
    assert(provider->buffer_observed_lookup_attempts == 1U);
    assert(provider->buffer_observed_lookup_failures == 1U);
    assert(provider->missing_transform_count == 1U);
    assert(provider->missing_frame_count == 1U);

    buffer.ingest_dynamic(authority, dynamic_record("base", "sensor", clock, 100, translated(1.0)));
    expect_error(TfErrorCode::UnknownUncertainty, [&] {
        (void)buffer.lookup_at("base", "sensor", Timestamp { clock, 100 }, options({}));
    }, TimeError::UnknownUncertainty);
    expect_error(TfErrorCode::ClockMappingUnavailable, [&] {
        (void)buffer.lookup_at("base", "sensor", Timestamp { foreign_clock, 100 }, options());
    }, TimeError::ClockMappingUnavailable);

    pressure = buffer.pressure_snapshot();
    assert(pressure.missing_transform_count == 1U);
    assert(pressure.missing_frame_count == 1U);
    assert(pressure.clock_error_count == 2U);
    assert(pressure.clock_mapping_failure_count == 1U);
    provider = std::find_if(pressure.providers.begin(), pressure.providers.end(),
                            [&authority](const auto& observation) {
                                return observation.authority == authority;
                            });
    assert(provider != pressure.providers.end());
    assert(provider->missing_transform_count == 1U);
    assert(provider->missing_frame_count == 1U);
    assert(provider->clock_error_count == 2U);
    assert(provider->clock_mapping_failure_count == 1U);

    const auto authority_peer = buffer.bind_provider("metrics_peer", uuid(37U));
    expect_error(TfErrorCode::ConflictingAuthority, [&] {
        buffer.ingest_dynamic(authority_peer,
                              dynamic_record("base", "sensor", clock, 100, translated(2.0)));
    });
    expect_error(TfErrorCode::ConflictingAuthority, [&] {
        (void)buffer.lookup_at("base", "sensor", Timestamp { clock, 100 }, options());
    });

    const auto parent_conflict = buffer.bind_provider("metrics_parent", uuid(38U));
    expect_error(TfErrorCode::ConflictingParent, [&] {
        buffer.ingest_dynamic(parent_conflict,
                              dynamic_record("other_base", "sensor", clock, 100, translated(3.0)));
    });
    pressure = buffer.pressure_snapshot();
    assert(pressure.conflict_count == 3U);
    assert(pressure.authority_conflict_count == 2U);
    const auto first_provider = std::find_if(pressure.providers.begin(), pressure.providers.end(),
                                             [&authority](const auto& observation) {
                                                 return observation.authority == authority;
                                             });
    const auto peer_provider = std::find_if(pressure.providers.begin(), pressure.providers.end(),
                                             [&authority_peer](const auto& observation) {
                                                 return observation.authority == authority_peer;
                                             });
    const auto parent_provider = std::find_if(pressure.providers.begin(), pressure.providers.end(),
                                               [&parent_conflict](const auto& observation) {
                                                   return observation.authority == parent_conflict;
                                               });
    assert(first_provider != pressure.providers.end());
    assert(first_provider->conflict_count == 1U);
    assert(first_provider->authority_conflict_count == 1U);
    assert(peer_provider != pressure.providers.end());
    assert(peer_provider->conflict_count == 2U);
    assert(peer_provider->authority_conflict_count == 2U);
    assert(parent_provider != pressure.providers.end());
    assert(parent_provider->conflict_count == 1U);
    assert(parent_provider->authority_conflict_count == 0U);

    const auto replacement = buffer.bind_provider("metrics", uuid(39U));
    pressure = buffer.pressure_snapshot();
    assert(pressure.lookup_attempts == 4U);
    assert(pressure.lookup_failures == 4U);
    assert(pressure.missing_transform_count == 1U);
    assert(pressure.missing_frame_count == 1U);
    assert(pressure.clock_error_count == 2U);
    assert(pressure.clock_mapping_failure_count == 1U);
    assert(pressure.conflict_count == 3U);
    assert(pressure.authority_conflict_count == 2U);
    provider = std::find_if(pressure.providers.begin(), pressure.providers.end(),
                            [&replacement](const auto& observation) {
                                return observation.authority == replacement;
                            });
    assert(provider != pressure.providers.end());
    assert(provider->buffer_observed_lookup_attempts == 0U);
    assert(provider->buffer_observed_lookup_failures == 0U);
    assert(provider->missing_transform_count == 0U);
    assert(provider->missing_frame_count == 0U);
    assert(provider->clock_error_count == 0U);
    assert(provider->clock_mapping_failure_count == 0U);
    assert(provider->conflict_count == 0U);
    assert(provider->authority_conflict_count == 0U);
}

void test_global_sample_eviction_retains_ordering_state() {
    const auto clock = clock_identity(ClockKind::HostMonotonic, 62U);
    TfResourceConfig bounded;
    bounded.max_samples_per_edge = 1U;
    bounded.max_total_samples = 1U;
    TransformBuffer buffer(bounded);
    const auto authority = buffer.bind_provider("bounded_order", uuid(22U));
    const auto evicted = dynamic_record("root", "old", clock, 100, translated(1.0));
    buffer.ingest_dynamic(authority, evicted);
    buffer.ingest_dynamic(authority, dynamic_record("root", "new", clock, 200, translated(2.0)));

    auto pressure = buffer.pressure_snapshot();
    assert(pressure.stored_samples == 1U);
    assert(pressure.evicted_samples == 1U);
    assert(pressure.providers.front().dynamic_update_age_ns.has_value());
    assert(pressure.providers.front().dynamic_fresh);

    buffer.ingest_dynamic(authority, evicted);
    pressure = buffer.pressure_snapshot();
    assert(pressure.stored_samples == 1U);
    assert(pressure.evicted_samples == 1U);

    expect_error(TfErrorCode::OutOfOrderSample, [&] {
        buffer.ingest_dynamic(authority, dynamic_record("root", "old", clock, 100, translated(9.0)));
    });
    expect_error(TfErrorCode::OutOfOrderSample, [&] {
        buffer.ingest_dynamic(authority, dynamic_record("root", "old", clock, 99, translated(0.0)));
    });
}

std::string deep_frame_name(std::size_t index) {
    auto number = std::to_string(index);
    std::string result = "frame";
    result.append(4U - number.size(), '0');
    result += number;
    return result;
}

void test_deep_static_chain_and_cycle_validation_is_iterative() {
    constexpr std::size_t node_count = 4096U;
    TfResourceConfig resources;
    resources.max_providers = 1U;
    resources.max_total_edges = node_count;
    resources.max_static_transforms_per_provider = node_count;
    resources.max_pending_static_updates = 1U;
    TransformBuffer buffer(resources);
    const auto authority = buffer.bind_provider("deep_chain", uuid(23U));
    const auto clock = clock_identity(ClockKind::HostMonotonic, 63U);

    std::vector<StaticEdge> chain;
    chain.reserve(node_count - 1U);
    for (std::size_t index = 0U; index + 1U < node_count; ++index) {
        chain.push_back(StaticEdge { FrameId(deep_frame_name(index + 1U)),
                                     FrameId(deep_frame_name(index)),
                                     translated(0.0) });
    }
    buffer.apply_static_snapshot(authority, static_set(1U, clock, 0, chain));

    chain.push_back(StaticEdge { FrameId(deep_frame_name(0U)),
                                 FrameId(deep_frame_name(node_count - 1U)),
                                 translated(0.0) });
    expect_error(TfErrorCode::CycleDetected, [&] {
        buffer.apply_static_snapshot(authority, static_set(2U, clock, 0, std::move(chain)));
    });
}

void test_periodic_static_resync_keeps_healthy_snapshot_available() {
    TfResourceConfig resources;
    resources.resync_interval = 2ms;
    resources.snapshot_max_age = 5s;
    TransformBuffer buffer(resources);
    const auto authority = buffer.bind_provider("periodic_static", uuid(24U));
    const auto clock = clock_identity(ClockKind::HostMonotonic, 64U);
    buffer.apply_static_snapshot(
        authority,
        static_set(1U, clock, 0, { StaticEdge { FrameId("world"), FrameId("camera"), translated(2.0) } }));

    assert(!buffer.needs_resync(authority));
    assert(!buffer.needs_resync(authority));
    assert(buffer.lookup_at("world", "camera", Timestamp { clock, 1 }, options()).transform.translation().x()
           == 2.0);
    assert(!buffer.pressure_snapshot().providers.front().static_stale);

    std::this_thread::sleep_for(3ms);
    assert(buffer.needs_resync(authority));
    assert(!buffer.needs_resync(authority));
    assert(buffer.lookup_at("world", "camera", Timestamp { clock, 1 }, options()).transform.translation().x()
           == 2.0);
    assert(!buffer.pressure_snapshot().providers.front().static_stale);
}

void test_successful_snapshot_restarts_the_resync_cooldown() {
    TfResourceConfig resources;
    resources.resync_interval = 10ms;
    TransformBuffer buffer(resources);
    const auto authority = buffer.bind_provider("resync_cooldown", uuid(33U));
    const auto clock = clock_identity(ClockKind::HostMonotonic, 75U);

    assert(buffer.needs_resync(authority));
    std::this_thread::sleep_for(12ms);
    buffer.apply_static_snapshot(
        authority,
        static_set(1U, clock, 0, { StaticEdge { FrameId("base"), FrameId("camera"), translated(1.0) } }));

    assert(!buffer.needs_resync(authority));
    std::this_thread::sleep_for(12ms);
    assert(buffer.needs_resync(authority));
    assert(!buffer.needs_resync(authority));
}

void test_expired_snapshot_waits_for_minimum_resync_interval() {
    TfResourceConfig resources;
    resources.resync_interval = 20ms;
    resources.snapshot_max_age = 2ms;
    TransformBuffer buffer(resources);
    const auto authority = buffer.bind_provider("stale_static", uuid(25U));
    const auto clock = clock_identity(ClockKind::HostMonotonic, 65U);
    buffer.apply_static_snapshot(
        authority,
        static_set(1U, clock, 0, { StaticEdge { FrameId("world"), FrameId("camera"), translated(2.0) } }));

    assert(!buffer.needs_resync(authority));
    std::this_thread::sleep_for(5ms);
    assert(!buffer.needs_resync(authority));
    assert(buffer.pressure_snapshot().providers.front().static_stale);
    expect_error(TfErrorCode::SnapshotStale, [&] {
        (void)buffer.lookup_at("world", "camera", Timestamp { clock, 1 }, options());
    });

    std::this_thread::sleep_for(20ms);
    assert(buffer.needs_resync(authority));
    assert(!buffer.needs_resync(authority));
}

void test_precision_policy_and_unknown_uncertainty_opt_in() {
    const auto clock = clock_identity(ClockKind::HostMonotonic, 61U);
    LookupPolicy limited_precision;
    limited_precision.max_timestamp_uncertainty_ns = 5U;
    TransformBuffer strict({}, limited_precision);
    const auto dynamic = strict.bind_provider("precision", uuid(16U));
    strict.ingest_dynamic(dynamic,
                          dynamic_record("world", "sensor", clock, 100, translated(0.0), 0U, 10U));
    expect_error(TfErrorCode::TimestampUncertaintyExceeded, [&] {
        (void)strict.lookup_at("world", "sensor", Timestamp { clock, 100 }, options(1U));
    });

    LookupPolicy permissive;
    permissive.allow_unknown_uncertainty = true;
    TransformBuffer unknown_allowed({}, permissive);
    const auto static_authority = unknown_allowed.bind_provider("unknown_precision", uuid(17U));
    unknown_allowed.apply_static_snapshot(
        static_authority,
        StaticSet { 1U,
                    Timestamp { clock, 0 },
                    {},
                    { StaticEdge { FrameId("world"), FrameId("sensor"), translated(0.0) } } });
    const auto result = unknown_allowed.lookup_at("world", "sensor", Timestamp { clock, 1 }, options({}));
    assert(!result.timing_uncertainty_ns);
}

void test_best_effort_mixed_uncertainty_enforces_known_limit() {
    LookupPolicy policy;
    policy.max_timestamp_uncertainty_ns = 5U;
    policy.allow_unknown_uncertainty = true;
    const auto clock = clock_identity(ClockKind::HostMonotonic, 76U);
    const auto lookup_with_static_uncertainty = [&policy, &clock](std::uint64_t static_uncertainty_ns) {
        TransformBuffer buffer({}, policy);
        const auto static_authority = buffer.bind_provider("static_precision", uuid(34U));
        const auto dynamic_authority = buffer.bind_provider("dynamic_precision", uuid(35U));
        buffer.apply_static_snapshot(
            static_authority,
            StaticSet { 1U,
                        Timestamp { clock, 0 },
                        static_uncertainty_ns,
                        { StaticEdge { FrameId("world"), FrameId("base"), translated(2.0) } } });
        buffer.ingest_dynamic(
            dynamic_authority,
            dynamic_record("base", "camera", clock, 100, translated(3.0), 1U, std::nullopt));
        return buffer.lookup_at("world", "camera", Timestamp { clock, 100 }, options(0U));
    };

    expect_error(TfErrorCode::TimestampUncertaintyExceeded, [&] {
        (void)lookup_with_static_uncertainty(10U);
    });
    const auto within_limit = lookup_with_static_uncertainty(4U);
    assert(within_limit.transform.translation().x() == 5.0);
    assert(within_limit.sources.size() == 2U);
    assert(!within_limit.timing_uncertainty_ns);
}

void test_concurrent_static_snapshot_and_lookup() {
    const auto clock = clock_identity(ClockKind::HostMonotonic, 70U);
    TransformBuffer buffer;
    const auto authority = buffer.bind_provider("concurrent", uuid(11U));
    const auto snapshot = static_set(1U,
                                    clock,
                                    0,
                                    { StaticEdge { FrameId("world"), FrameId("camera"), translated(2.0) } });
    buffer.apply_static_snapshot(authority, snapshot);
    std::atomic<std::uint32_t> failures { 0U };
    std::vector<std::thread> readers;
    for (std::size_t index = 0U; index < 4U; ++index) {
        readers.emplace_back([&] {
            for (std::size_t count = 0U; count < 100U; ++count) {
                try {
                    const auto result = buffer.lookup_at("world", "camera", Timestamp { clock, 10 }, options());
                    if (result.transform.translation().x() != 2.0) {
                        failures.fetch_add(1U, std::memory_order_relaxed);
                    }
                } catch (...) {
                    failures.fetch_add(1U, std::memory_order_relaxed);
                }
            }
        });
    }
    std::thread writer([&] {
        for (std::size_t count = 0U; count < 100U; ++count) {
            try {
                buffer.apply_static_snapshot(authority, snapshot);
            } catch (...) {
                failures.fetch_add(1U, std::memory_order_relaxed);
            }
        }
    });
    for (auto& reader : readers) {
        reader.join();
    }
    writer.join();
    assert(failures.load(std::memory_order_relaxed) == 0U);
}

void test_concurrent_static_and_dynamic_mutations_keep_lookup_views_atomic() {
    constexpr std::uint64_t writes = 1000U;
    constexpr std::size_t reader_count = 3U;

    TfResourceConfig resources;
    resources.max_samples_per_edge = 2048U;
    resources.max_total_samples = 8192U;
    resources.max_dynamic_age = 60s;
    resources.snapshot_max_age = 60s;
    TransformBuffer buffer(resources);
    const auto static_authority = buffer.bind_provider("static_calibration", uuid(31U));
    const auto dynamic_authority = buffer.bind_provider("base_motion", uuid(32U));
    const auto clock = clock_identity(ClockKind::HostMonotonic, 74U);

    buffer.apply_static_snapshot(
        static_authority,
        static_set(1U,
                   clock,
                   0,
                   { StaticEdge { FrameId("base"), FrameId("mount"), translated(1.0) },
                     StaticEdge { FrameId("mount"), FrameId("camera"), translated(-1.0) } }));
    buffer.ingest_dynamic(dynamic_authority, dynamic_record("odom", "base", clock, 0, translated(5.0), 1U, 0U));

    std::barrier start(static_cast<std::ptrdiff_t>(reader_count + 3U));
    std::atomic<std::uint32_t> failures { 0U };
    std::thread static_writer([&] {
        start.arrive_and_wait();
        for (std::uint64_t revision = 2U; revision <= writes + 1U; ++revision) {
            const auto offset = static_cast<double>(revision);
            try {
                buffer.apply_static_snapshot(
                    static_authority,
                    static_set(revision,
                               clock,
                               0,
                               { StaticEdge { FrameId("base"), FrameId("mount"), translated(offset) },
                                 StaticEdge { FrameId("mount"), FrameId("camera"), translated(-offset) } }));
            } catch (...) {
                failures.fetch_add(1U, std::memory_order_relaxed);
            }
        }
    });
    std::thread dynamic_writer([&] {
        start.arrive_and_wait();
        for (std::uint64_t time = 1U; time <= writes; ++time) {
            try {
                buffer.ingest_dynamic(
                    dynamic_authority,
                    dynamic_record("odom", "base", clock, static_cast<std::int64_t>(time), translated(5.0), 1U, 0U));
            } catch (...) {
                failures.fetch_add(1U, std::memory_order_relaxed);
            }
        }
    });

    std::vector<std::thread> readers;
    readers.reserve(reader_count);
    for (std::size_t index = 0U; index < reader_count; ++index) {
        readers.emplace_back([&] {
            start.arrive_and_wait();
            for (std::uint64_t count = 0U; count < writes; ++count) {
                try {
                    const auto result = buffer.lookup_at("odom", "camera", Timestamp { clock, 0 }, options(0U));
                    bool consistent = result.transform.translation().x() == 5.0 && result.sources.size() == 3U;
                    std::vector<std::uint64_t> static_revisions;
                    std::size_t dynamic_sources = 0U;
                    for (const auto& source : result.sources) {
                        if (source.static_revision) {
                            consistent = consistent && source.authority == static_authority;
                            static_revisions.push_back(*source.static_revision);
                        } else {
                            consistent = consistent && source.authority == dynamic_authority
                                         && source.transform_epoch == std::optional<std::uint64_t>(1U);
                            ++dynamic_sources;
                        }
                    }
                    consistent = consistent && static_revisions.size() == 2U
                                 && static_revisions[0] == static_revisions[1] && dynamic_sources == 1U;
                    if (!consistent) {
                        failures.fetch_add(1U, std::memory_order_relaxed);
                    }
                } catch (...) {
                    failures.fetch_add(1U, std::memory_order_relaxed);
                }
            }
        });
    }

    start.arrive_and_wait();
    static_writer.join();
    dynamic_writer.join();
    for (auto& reader : readers) {
        reader.join();
    }

    assert(failures.load(std::memory_order_relaxed) == 0U);
    const auto final_result = buffer.lookup_at("odom", "camera", Timestamp { clock, 0 }, options(0U));
    assert(final_result.transform.translation().x() == 5.0);
    assert(final_result.sources.size() == 3U);
    const auto pressure = buffer.pressure_snapshot();
    assert(pressure.active_providers == 2U);
    const auto static_provider = std::find_if(pressure.providers.begin(), pressure.providers.end(),
                                              [&static_authority](const auto& provider) {
                                                  return provider.authority == static_authority;
                                              });
    const auto dynamic_provider = std::find_if(pressure.providers.begin(), pressure.providers.end(),
                                               [&dynamic_authority](const auto& provider) {
                                                   return provider.authority == dynamic_authority;
                                               });
    assert(static_provider != pressure.providers.end());
    assert(dynamic_provider != pressure.providers.end());
    assert(static_provider->static_revision == std::optional<std::uint64_t>(writes + 1U));
    assert(!static_provider->static_stale);
    assert(dynamic_provider->stored_samples == writes + 1U);
    assert(dynamic_provider->stored_samples <= 2048U);
    assert(pressure.stored_samples <= 8192U);
    assert(pressure.evicted_samples == 0U);
}

void test_wire_canonicality_and_layout_contract() {
    const auto device_clock = clock_identity(ClockKind::Device, 80U, 3U);
    const Timestamp stamp { device_clock, -123456 };
    auto wire_stamp = rbt::tf::encode_timestamp(stamp, 77U);
    auto decoded_stamp = rbt::tf::decode_timestamp(wire_stamp);
    assert(decoded_stamp.first.clock == device_clock);
    assert(decoded_stamp.first.ns == stamp.ns);
    assert(decoded_stamp.second == 77U);
    const auto unknown_precision = rbt::tf::decode_timestamp(rbt::tf::encode_timestamp(stamp, {}));
    assert(!unknown_precision.second);

    auto invalid_timestamp = wire_stamp;
    invalid_timestamp.flags = 0x80U;
    expect_error(TfErrorCode::InvalidTransform, [&] { (void)rbt::tf::decode_timestamp(invalid_timestamp); });
    invalid_timestamp = wire_stamp;
    invalid_timestamp.reserved[2] = 1U;
    expect_error(TfErrorCode::InvalidTransform, [&] { (void)rbt::tf::decode_timestamp(invalid_timestamp); });
    invalid_timestamp = wire_stamp;
    invalid_timestamp.kind = 0U;
    expect_error(TfErrorCode::InvalidTransform, [&] { (void)rbt::tf::decode_timestamp(invalid_timestamp); });
    invalid_timestamp = rbt::tf::encode_timestamp(stamp, {});
    invalid_timestamp.uncertainty_ns = 1U;
    expect_error(TfErrorCode::InvalidTransform, [&] { (void)rbt::tf::decode_timestamp(invalid_timestamp); });
    invalid_timestamp = wire_stamp;
    std::fill(std::begin(invalid_timestamp.source_id), std::end(invalid_timestamp.source_id), 0U);
    expect_error(TfErrorCode::Time, [&] { (void)rbt::tf::decode_timestamp(invalid_timestamp); },
                 TimeError::InvalidClockIdentity);

    const StaticEdge edge { FrameId("world"), FrameId("camera"), translated(1.0, 2.0, 3.0) };
    const auto wire_edge = rbt::tf::encode_static_edge(edge);
    assert(rbt::tf::decode_static_edge(wire_edge) == edge);
    auto invalid_edge = wire_edge;
    invalid_edge.parent.bytes[63] = static_cast<std::uint8_t>('x');
    expect_error(TfErrorCode::InvalidFrame, [&] { (void)rbt::tf::decode_static_edge(invalid_edge); });
    invalid_edge = wire_edge;
    invalid_edge.transform.rotation.w = 2.0;
    expect_error(TfErrorCode::InvalidQuaternion, [&] { (void)rbt::tf::decode_static_edge(invalid_edge); });

    const auto authority = uuid(12U);
    const auto record = dynamic_record("world", "camera", device_clock, -4, translated(4.0), 9U, 3U);
    const auto wire_dynamic = rbt::tf::encode_dynamic(authority, record);
    assert(rbt::tf::decode_dynamic(authority, wire_dynamic) == record);
    expect_error(TfErrorCode::ConflictingAuthority, [&] {
        (void)rbt::tf::decode_dynamic(uuid(13U), wire_dynamic);
    });
    auto invalid_dynamic = wire_dynamic;
    std::fill(std::begin(invalid_dynamic.authority), std::end(invalid_dynamic.authority), 0U);
    expect_error(TfErrorCode::InvalidTransform, [&] {
        (void)rbt::tf::decode_dynamic(authority, invalid_dynamic);
    });
    expect_error(TfErrorCode::InvalidTransform, [&] {
        (void)rbt::tf::encode_dynamic("00000000-0000-0000-0000-000000000000", record);
    });

    const auto set = static_set(77U, device_clock, -20, { edge });
    const auto header = rbt::tf::static_snapshot_header(authority, set);
    const std::array<rbt::tf::StaticTransform, 1U> payload { wire_edge };
    const auto decoded_set = rbt::tf::decode_static_snapshot(authority, header, payload, 4U);
    assert(decoded_set == set);
    auto bad_header = header;
    ++bad_header.count;
    expect_error(TfErrorCode::InvalidTransform, [&] {
        (void)rbt::tf::decode_static_snapshot(authority, bad_header, payload, 4U);
    });
    expect_error(TfErrorCode::SnapshotTooLarge, [&] {
        (void)rbt::tf::decode_static_snapshot(authority, header, payload, 0U);
    });
    bad_header = header;
    bad_header.status = rbt::tf::kStaticStatusSnapshotTooLarge;
    bad_header.count = 0U;
    expect_error(TfErrorCode::InvalidTransform, [&] {
        (void)rbt::tf::decode_static_snapshot(authority, bad_header, payload, 4U);
    });
    expect_error(TfErrorCode::SnapshotTooLarge, [&] {
        (void)rbt::tf::decode_static_snapshot(authority, bad_header, {}, 4U);
    });
    bad_header.status = rbt::tf::kStaticStatusProviderUnavailable;
    expect_error(TfErrorCode::ProviderUnavailable, [&] {
        (void)rbt::tf::decode_static_snapshot(authority, bad_header, {}, 4U);
    });
    const auto update_header = rbt::tf::static_update_header(authority, set);
    assert(rbt::tf::decode_static_update(authority, update_header, payload, 4U) == set);

    static_assert(sizeof(rbt::tf::TfFrameId) == 64U);
    static_assert(sizeof(rbt::tf::TfVec3) == 24U);
    static_assert(sizeof(rbt::tf::TfQuaternion) == 32U);
    static_assert(sizeof(rbt::tf::TfTransform3) == 56U);
    static_assert(sizeof(rbt::tf::TfTimestamp) == 80U);
    static_assert(sizeof(rbt::tf::StaticTransform) == 184U);
    static_assert(sizeof(rbt::tf::StaticHeader) == 112U);
    static_assert(sizeof(rbt::tf::StaticUpdateHeader) == 112U);
    static_assert(sizeof(rbt::tf::DynamicTransform) == 288U);
    static_assert(sizeof(rbt::tf::StaticSnapshotRequest) == 24U);
    assert(rbt::tf::DYNAMIC_TRANSFORM_DESCRIPTOR.layout_hash
           == (rbt::LayoutHash { 0xb1U, 0xa0U, 0x29U, 0x59U, 0x97U, 0x0fU, 0xa0U, 0xecU,
                                 0xd3U, 0x0bU, 0x62U, 0x19U, 0xabU, 0x2bU, 0xcbU, 0x56U }));
    assert(rbt::tf::STATIC_SNAPSHOT_REQUEST_DESCRIPTOR.layout_hash
           == (rbt::LayoutHash { 0x85U, 0xb6U, 0x55U, 0xe2U, 0x70U, 0x04U, 0xd7U, 0xd3U,
                                 0x8eU, 0xb1U, 0x0dU, 0x72U, 0x0eU, 0x03U, 0x5dU, 0x7bU }));
    assert(rbt::tf::STATIC_SNAPSHOT_DESCRIPTOR.layout_hash
           == (rbt::LayoutHash { 0xcdU, 0x37U, 0x3fU, 0x3aU, 0x4bU, 0xdfU, 0x5dU, 0x68U,
                                 0x0aU, 0xecU, 0x5dU, 0x20U, 0x8dU, 0xa9U, 0x8eU, 0x82U }));
    assert(rbt::tf::STATIC_UPDATE_DESCRIPTOR.layout_hash
           == (rbt::LayoutHash { 0x8eU, 0x4aU, 0xaeU, 0xb6U, 0xd5U, 0xc1U, 0xbdU, 0x7cU,
                                 0x04U, 0x0aU, 0x9fU, 0xf2U, 0x38U, 0x44U, 0x32U, 0xb8U }));
}

} // namespace

int main() {
    test_graph_direction_lca_conflicts_and_recovery();
    test_interpolation_extrapolation_gap_epoch_and_duplicate_rules();
    test_clock_mapping_query_direction_and_errors();
    test_unknown_disconnected_and_static_clock_mapping();
    test_static_revision_races_gaps_and_effective_time();
    test_static_effective_time_chronology_clock_identity_and_rebind();
    test_resource_caps_provider_replacement_and_eviction();
    test_tf_diagnostic_counters_are_categorized_and_reset_on_rebind();
    test_global_sample_eviction_retains_ordering_state();
    test_deep_static_chain_and_cycle_validation_is_iterative();
    test_periodic_static_resync_keeps_healthy_snapshot_available();
    test_successful_snapshot_restarts_the_resync_cooldown();
    test_expired_snapshot_waits_for_minimum_resync_interval();
    test_precision_policy_and_unknown_uncertainty_opt_in();
    test_best_effort_mixed_uncertainty_enforces_known_limit();
    test_concurrent_static_snapshot_and_lookup();
    test_concurrent_static_and_dynamic_mutations_keep_lookup_views_atomic();
    test_wire_canonicality_and_layout_contract();
    return 0;
}

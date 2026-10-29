#ifndef RBT_CPP_TRANSFORMS_HPP
#define RBT_CPP_TRANSFORMS_HPP

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <rbt/runtime.hpp>
#include <rbt/tf_math.hpp>
#include <rbt/time.hpp>

namespace rbt::tf {

using SteadyClock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

struct TfResourceConfig {
    std::size_t max_providers { 16U };
    std::size_t max_total_edges { 128U };
    std::size_t max_samples_per_edge { 256U };
    std::size_t max_total_samples { 8192U };
    std::size_t max_static_transforms_per_provider { 64U };
    std::size_t max_pending_static_updates { 8U };
    Nanoseconds max_history_duration { std::chrono::seconds(10) };
    Nanoseconds resync_interval { std::chrono::seconds(1) };
    Nanoseconds snapshot_max_age { std::chrono::seconds(5) };
    Nanoseconds max_dynamic_age { std::chrono::seconds(5) };

    void validate() const {
        constexpr std::size_t max_providers_hard = 256U;
        constexpr std::size_t max_edges_hard = 4096U;
        constexpr std::size_t max_samples_hard = 1'000'000U;
        constexpr std::size_t max_pending_hard = 4096U;
        constexpr std::size_t max_pending_static_records = 65'536U;
        if (max_providers == 0U || max_providers > max_providers_hard || max_total_edges == 0U
            || max_total_edges > max_edges_hard || max_samples_per_edge == 0U
            || max_samples_per_edge > max_samples_hard || max_total_samples == 0U
            || max_total_samples > max_samples_hard || max_samples_per_edge > max_total_samples
            || max_static_transforms_per_provider == 0U
            || max_static_transforms_per_provider > max_total_edges
            || max_pending_static_updates == 0U || max_pending_static_updates > max_pending_hard
            || max_history_duration.count() <= 0 || resync_interval.count() <= 0
            || snapshot_max_age.count() <= 0 || max_dynamic_age.count() <= 0) {
            throw TfError(TfErrorCode::InvalidConfig);
        }
        if (max_providers > max_pending_static_records / max_pending_static_updates
            || max_providers * max_pending_static_updates
                   > max_pending_static_records / max_static_transforms_per_provider) {
            throw TfError(TfErrorCode::InvalidConfig);
        }
    }
};

struct LookupPolicy {
    std::optional<std::uint64_t> max_timestamp_uncertainty_ns { 1'000'000U };
    Nanoseconds max_interpolation_gap { std::chrono::seconds(1) };
    bool allow_unknown_uncertainty { false };

    void validate() const {
        if (max_interpolation_gap.count() <= 0) {
            throw TfError(TfErrorCode::InvalidConfig);
        }
    }
};

class Authority {
  public:
    Authority(FrameId slot, CapabilityInstanceId physical)
        : slot_(std::move(slot)), physical_(std::move(physical)) {
    }

    [[nodiscard]] const FrameId& slot() const noexcept {
        return slot_;
    }

    [[nodiscard]] const CapabilityInstanceId& physical() const noexcept {
        return physical_;
    }

    [[nodiscard]] friend bool operator==(const Authority&, const Authority&) = default;

  private:
    FrameId slot_;
    CapabilityInstanceId physical_;
};

struct StaticEdge {
    FrameId parent;
    FrameId child;
    Transform3 transform;

    [[nodiscard]] friend bool operator==(const StaticEdge&, const StaticEdge&) = default;
};

struct DynamicRecord {
    FrameId parent;
    FrameId child;
    rbt::time::Timestamp stamp;
    std::optional<std::uint64_t> uncertainty_ns;
    std::uint64_t transform_epoch { 0U };
    Transform3 transform;

    [[nodiscard]] friend bool operator==(const DynamicRecord& lhs, const DynamicRecord& rhs) {
        return lhs.parent == rhs.parent && lhs.child == rhs.child && lhs.stamp.clock == rhs.stamp.clock
               && lhs.stamp.ns == rhs.stamp.ns && lhs.uncertainty_ns == rhs.uncertainty_ns
               && lhs.transform_epoch == rhs.transform_epoch && lhs.transform == rhs.transform;
    }
};

struct StaticSet {
    std::uint64_t revision { 0U };
    rbt::time::Timestamp effective_from;
    std::optional<std::uint64_t> effective_uncertainty_ns;
    std::vector<StaticEdge> transforms;

    [[nodiscard]] friend bool operator==(const StaticSet& lhs, const StaticSet& rhs) {
        return lhs.revision == rhs.revision && lhs.effective_from.clock == rhs.effective_from.clock
               && lhs.effective_from.ns == rhs.effective_from.ns
               && lhs.effective_uncertainty_ns == rhs.effective_uncertainty_ns
               && lhs.transforms == rhs.transforms;
    }
};

struct LookupOptions {
    std::optional<std::uint64_t> timestamp_uncertainty_ns;
    std::span<const rbt::time::ClockMapping* const> mappings {};
    const rbt::time::Timestamp* now { nullptr };
};

struct TransformSource {
    Authority authority;
    FrameId parent;
    FrameId child;
    rbt::time::Timestamp timestamp;
    rbt::time::Timestamp bracket_start;
    rbt::time::Timestamp bracket_end;
    std::optional<std::uint64_t> static_revision;
    std::optional<std::uint64_t> transform_epoch;
};

struct LookupResult {
    Transform3 transform { Transform3::identity() };
    rbt::time::Timestamp requested_timestamp;
    std::optional<std::uint64_t> timing_uncertainty_ns;
    std::vector<TransformSource> sources;

    [[nodiscard]] Vec3 apply_point(Vec3 point) const {
        return transform.apply_point(point);
    }

    [[nodiscard]] Vec3 apply_vector(Vec3 vector) const {
        return transform.apply_vector(vector);
    }

    [[nodiscard]] Pose3 apply_pose(Pose3 pose) const {
        return transform.apply_pose(pose);
    }
};

struct ProviderObservation {
    Authority authority;
    std::optional<std::uint64_t> static_revision;
    std::optional<std::uint64_t> highest_observed_revision;
    bool static_tracked { false };
    bool static_stale { false };
    std::optional<std::uint64_t> snapshot_age_ns;
    std::optional<bool> snapshot_fresh;
    std::optional<std::uint64_t> dynamic_update_age_ns;
    bool dynamic_fresh { false };
    std::size_t active_static_edges { 0U };
    std::size_t active_dynamic_edges { 0U };
    std::size_t stored_samples { 0U };
    std::uint64_t evicted_samples { 0U };
    std::uint64_t buffer_observed_lookup_attempts { 0U };
    std::uint64_t buffer_observed_lookup_successes { 0U };
    std::uint64_t buffer_observed_lookup_failures { 0U };
    std::uint64_t missing_transform_count { 0U };
    std::uint64_t extrapolation_count { 0U };
    std::uint64_t clock_error_count { 0U };
    std::uint64_t conflict_count { 0U };
    std::uint64_t resync_attempts { 0U };
    std::uint64_t resync_failures { 0U };
    std::uint64_t missing_frame_count { 0U };
    std::uint64_t clock_mapping_failure_count { 0U };
    std::uint64_t authority_conflict_count { 0U };
};

struct GraphEdgeObservation {
    FrameId parent;
    FrameId child;
    Authority authority;
    bool is_static { false };
    std::optional<std::uint64_t> revision;
    std::optional<std::uint64_t> transform_epoch;
    std::size_t stored_samples { 0U };
    std::size_t conflicting_claims { 0U };
};

struct TfPressureSnapshot {
    std::size_t active_providers { 0U };
    std::size_t active_frames { 0U };
    std::size_t static_edges { 0U };
    std::size_t dynamic_edges { 0U };
    std::size_t stored_samples { 0U };
    std::uint64_t evicted_samples { 0U };
    std::uint64_t lookup_attempts { 0U };
    std::uint64_t lookup_successes { 0U };
    std::uint64_t lookup_failures { 0U };
    std::uint64_t missing_transform_count { 0U };
    std::uint64_t extrapolation_count { 0U };
    std::uint64_t clock_error_count { 0U };
    std::uint64_t conflict_count { 0U };
    std::vector<ProviderObservation> providers;
    std::vector<GraphEdgeObservation> graph;
    std::uint64_t missing_frame_count { 0U };
    std::uint64_t clock_mapping_failure_count { 0U };
    std::uint64_t authority_conflict_count { 0U };
};

namespace detail {

struct FrameIdLess {
    [[nodiscard]] bool operator()(const FrameId& lhs, const FrameId& rhs) const noexcept {
        return lhs.str() < rhs.str();
    }
};

[[nodiscard]] inline bool same_timestamp(const rbt::time::Timestamp& lhs,
                                         const rbt::time::Timestamp& rhs) noexcept {
    return lhs.clock == rhs.clock && lhs.ns == rhs.ns;
}

[[nodiscard]] inline std::uint64_t saturating_add(std::uint64_t lhs, std::uint64_t rhs) noexcept {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return lhs + rhs;
}

inline void increment(std::uint64_t& value) noexcept {
    value = saturating_add(value, 1U);
}

[[nodiscard]] inline std::optional<std::array<std::uint8_t, 16>> parse_uuid(
    std::string_view value) noexcept {
    if (value.size() != 36U || value[8] != '-' || value[13] != '-' || value[18] != '-'
        || value[23] != '-') {
        return {};
    }
    const auto digit = [](char c) noexcept -> int {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        return -1;
    };
    std::array<std::uint8_t, 16> result {};
    std::size_t output = 0U;
    for (std::size_t index = 0U; index < value.size();) {
        if (index == 8U || index == 13U || index == 18U || index == 23U) {
            ++index;
            continue;
        }
        const auto high = digit(value[index]);
        const auto low = digit(value[index + 1U]);
        if (high < 0 || low < 0 || output >= result.size()) {
            return {};
        }
        result[output++] = static_cast<std::uint8_t>((high << 4) | low);
        index += 2U;
    }
    if (output != result.size()) {
        return {};
    }
    return result;
}

[[nodiscard]] inline bool nonzero(const std::array<std::uint8_t, 16>& value) noexcept {
    return std::any_of(value.begin(), value.end(), [](std::uint8_t byte) { return byte != 0U; });
}

[[nodiscard]] inline std::uint64_t elapsed_ns(SteadyClock::time_point now,
                                               SteadyClock::time_point then) noexcept {
    const auto elapsed = std::chrono::duration_cast<Nanoseconds>(now - then).count();
    return elapsed <= 0 ? 0U : static_cast<std::uint64_t>(elapsed);
}

[[nodiscard]] inline std::uint64_t duration_ns(Nanoseconds duration) noexcept {
    return duration.count() <= 0 ? 0U : static_cast<std::uint64_t>(duration.count());
}

[[nodiscard]] inline std::uint64_t unsigned_difference(std::int64_t later,
                                                       std::int64_t earlier) noexcept {
    return static_cast<std::uint64_t>(later) - static_cast<std::uint64_t>(earlier);
}

[[nodiscard]] inline bool same_clock_domain_and_source(const rbt::time::ClockIdentity& lhs,
                                                        const rbt::time::ClockIdentity& rhs) noexcept {
    return lhs.kind() == rhs.kind() && lhs.source_id() == rhs.source_id();
}

[[nodiscard]] inline bool has_cycle(const std::vector<std::pair<FrameId, FrameId>>& edges) {
    std::map<FrameId, std::vector<FrameId>, FrameIdLess> parents;
    for (const auto& [child, parent] : edges) {
        parents[child].push_back(parent);
    }
    std::map<FrameId, std::uint8_t, FrameIdLess> marks;
    std::vector<std::pair<FrameId, std::size_t>> stack;
    stack.reserve(parents.size() + 1U);
    for (const auto& [child, _] : parents) {
        if (marks[child] == 2U) {
            continue;
        }
        marks[child] = 1U;
        stack.emplace_back(child, 0U);
        while (!stack.empty()) {
            auto& [frame, next_parent] = stack.back();
            const auto found = parents.find(frame);
            if (found == parents.end() || next_parent == found->second.size()) {
                marks[frame] = 2U;
                stack.pop_back();
                continue;
            }
            const auto parent = found->second[next_parent++];
            const auto parent_mark = marks[parent];
            if (parent_mark == 1U) {
                return true;
            }
            if (parent_mark == 2U) {
                continue;
            }
            marks[parent] = 1U;
            stack.emplace_back(parent, 0U);
        }
    }
    return false;
}

} // namespace detail

/// A bounded, thread-safe local transform graph and dynamic history.
class TransformBuffer {
  public:
    explicit TransformBuffer(TfResourceConfig resources = {}, LookupPolicy policy = {})
        : resources_(std::move(resources)), policy_(std::move(policy)), state_(std::make_shared<State>()) {
        resources_.validate();
        policy_.validate();
    }

    [[nodiscard]] Authority bind_provider(std::string_view slot_name,
                                          CapabilityInstanceId physical_id) {
        const FrameId slot(slot_name);
        const auto parsed_id = detail::parse_uuid(physical_id);
        if (!parsed_id || !detail::nonzero(*parsed_id)) {
            throw TfError(TfErrorCode::InvalidConfig);
        }
        auto& state = *state_;
        std::lock_guard lock(state.mutex);
        const auto existing = state.providers.find(slot);
        if (existing != state.providers.end() && existing->second.authority.physical() == physical_id) {
            return existing->second.authority;
        }
        if (existing == state.providers.end() && state.providers.size() >= resources_.max_providers) {
            throw TfError(TfErrorCode::CapacityExceeded);
        }
        for (const auto& [other_slot, provider] : state.providers) {
            if (other_slot != slot && provider.authority.physical() == physical_id) {
                throw TfError(TfErrorCode::ConflictingAuthority);
            }
        }
        Authority authority(slot, std::move(physical_id));
        Provider provider(authority);
        if (existing == state.providers.end()) {
            state.providers.emplace(slot, std::move(provider));
        } else {
            existing->second = std::move(provider);
        }
        return authority;
    }

    void unbind_provider(const Authority& authority) {
        auto& state = *state_;
        std::lock_guard lock(state.mutex);
        (void)require_provider(state, authority);
        state.providers.erase(authority.slot());
    }

    void ingest_dynamic(const Authority& authority, DynamicRecord record) {
        if (!record.stamp.clock.is_well_formed()) {
            throw TfError(rbt::time::TimeError::InvalidTimestamp);
        }
        auto& state = *state_;
        std::lock_guard lock(state.mutex);
        auto provider_it = state.providers.find(authority.slot());
        if (provider_it == state.providers.end() || provider_it->second.authority != authority) {
            throw TfError(TfErrorCode::ProviderUnavailable);
        }
        auto& provider = provider_it->second;
        if (record.parent == record.child) {
            throw TfError(TfErrorCode::CycleDetected);
        }
        if (provider.static_set
            && std::any_of(provider.static_set->transforms.begin(),
                           provider.static_set->transforms.end(),
                           [&record](const StaticEdge& edge) { return edge.child == record.child; })) {
            throw TfError(TfErrorCode::ConflictingParent);
        }
        auto history_it = provider.dynamic.find(record.child);
        if (history_it != provider.dynamic.end()) {
            const auto& history = history_it->second;
            if (history.parent != record.parent) {
                throw TfError(TfErrorCode::ConflictingParent);
            }
            validate_dynamic_transition(history, record);
            if (record.transform_epoch == history.epoch && history.last_record) {
                const auto& previous = *history.last_record;
                if (detail::same_timestamp(record.stamp, previous.stamp)) {
                    if (record == previous) {
                        return;
                    }
                    throw TfError(TfErrorCode::OutOfOrderSample);
                }
                if (record.transform_epoch == history.epoch && record.stamp.clock == history.clock
                    && record.stamp.ns <= previous.stamp.ns) {
                    throw TfError(TfErrorCode::OutOfOrderSample);
                }
            }
        }

        const bool new_edge = history_it == provider.dynamic.end();
        if (new_edge) {
            if (edge_count(state) >= resources_.max_total_edges
                || owner_edge_count(provider) >= resources_.max_total_edges) {
                throw TfError(TfErrorCode::CapacityExceeded);
            }
            ensure_acyclic_with_extra(state, record.parent, record.child);
        }
        if (state.next_order == std::numeric_limits<std::uint64_t>::max()) {
            throw TfError(TfErrorCode::CapacityExceeded);
        }
        const auto order = state.next_order++;
        auto [inserted_it, inserted] = provider.dynamic.try_emplace(
            record.child,
            DynamicHistory { record.parent, record.stamp.clock, record.transform_epoch, {}, {}, {}, {} });
        (void)inserted;
        auto& history = inserted_it->second;
        std::uint64_t reset_evictions = 0U;
        if (record.transform_epoch > history.epoch || record.stamp.clock != history.clock) {
            reset_evictions = static_cast<std::uint64_t>(history.samples.size());
            history.samples.clear();
            history.boundary = record.stamp;
            history.epoch = record.transform_epoch;
            history.clock = record.stamp.clock;
        }
        const auto received_at = SteadyClock::now();
        history.last_record = record;
        history.last_received_at = received_at;
        history.samples.push_back(Sample { std::move(record), received_at, order });
        auto evicted = prune_history(history);
        evicted = detail::saturating_add(evicted, reset_evictions);
        if (evicted != 0U) {
            provider.metrics.evicted_samples = detail::saturating_add(provider.metrics.evicted_samples, evicted);
            state.metrics.evicted_samples = detail::saturating_add(state.metrics.evicted_samples, evicted);
        }
        enforce_global_sample_limit(state);
        const std::vector<FrameId> children { inserted_it->first };
        if (const auto conflict = conflict_in_set(state, children)) {
            count_conflict(state, provider, *conflict);
            throw TfError(*conflict);
        }
    }

    void apply_static_snapshot(const Authority& authority, StaticSet snapshot) {
        validate_static_set(snapshot);
        auto& state = *state_;
        std::lock_guard lock(state.mutex);
        auto provider_it = state.providers.find(authority.slot());
        if (provider_it == state.providers.end() || provider_it->second.authority != authority) {
            throw TfError(TfErrorCode::ProviderUnavailable);
        }
        auto& provider = provider_it->second;
        const auto current = provider.static_set;
        const auto highest_observed = provider.highest_observed_revision;
        const auto was_stale = provider.static_stale;
        if (current) {
            if (snapshot.revision < current->revision) {
                if (was_stale || (highest_observed && *highest_observed > current->revision)) {
                    provider.static_stale = true;
                    detail::increment(provider.metrics.resync_failures);
                    throw TfError(TfErrorCode::SnapshotRevisionGap);
                }
                return;
            }
            if (snapshot.revision == current->revision && !(snapshot == *current)) {
                provider.static_stale = true;
                detail::increment(provider.metrics.resync_failures);
                throw TfError(TfErrorCode::SnapshotRevisionGap);
            }
        }
        if (current && snapshot.revision > current->revision) {
            validate_static_chronology_or_stale(provider, *current, snapshot);
        }

        StaticSet merged = std::move(snapshot);
        auto remaining = provider.pending;
        for (auto it = remaining.begin(); it != remaining.end();) {
            if (it->first <= merged.revision) {
                it = remaining.erase(it);
            } else {
                ++it;
            }
        }
        for (;;) {
            if (merged.revision == std::numeric_limits<std::uint64_t>::max()) {
                break;
            }
            const auto next = remaining.find(merged.revision + 1U);
            if (next == remaining.end()) {
                break;
            }
            validate_static_chronology_or_stale(provider, merged, next->second);
            merged = std::move(next->second);
            remaining.erase(next);
        }
        const bool has_gap = !remaining.empty()
                             || (highest_observed && *highest_observed > merged.revision);
        if (edge_count_replacing_static(state, authority.slot(), merged.transforms.size())
            > resources_.max_total_edges) {
            throw TfError(TfErrorCode::CapacityExceeded);
        }
        for (const auto& [child, _] : provider.dynamic) {
            if (std::any_of(merged.transforms.begin(), merged.transforms.end(), [&child](const StaticEdge& edge) {
                    return edge.child == child;
                })) {
                throw TfError(TfErrorCode::ConflictingParent);
            }
        }
        ensure_acyclic_replacing_static(state, authority.slot(), merged.transforms);

        const auto received_at = SteadyClock::now();
        provider.static_set = merged;
        provider.static_stale = has_gap;
        provider.highest_observed_revision = provider.highest_observed_revision
                                                 ? std::max(*provider.highest_observed_revision, merged.revision)
                                                 : merged.revision;
        provider.pending.clear();
        provider.last_snapshot = received_at;
        if (has_gap) {
            detail::increment(provider.metrics.resync_failures);
        }
        std::vector<FrameId> children;
        children.reserve(merged.transforms.size());
        for (const auto& edge : merged.transforms) {
            children.push_back(edge.child);
        }
        if (const auto conflict = conflict_in_set(state, children)) {
            count_conflict(state, provider, *conflict);
            throw TfError(*conflict);
        }
        if (has_gap) {
            throw TfError(TfErrorCode::SnapshotRevisionGap);
        }
    }

    void apply_static_update(const Authority& authority, StaticSet update) {
        validate_static_set(update);
        auto& state = *state_;
        std::lock_guard lock(state.mutex);
        auto provider_it = state.providers.find(authority.slot());
        if (provider_it == state.providers.end() || provider_it->second.authority != authority) {
            throw TfError(TfErrorCode::ProviderUnavailable);
        }
        auto& provider = provider_it->second;
        if (!provider.static_set) {
            const auto previous = provider.pending.find(update.revision);
            if (previous != provider.pending.end()) {
                if (previous->second == update) {
                    return;
                }
                provider.static_stale = true;
                detail::increment(provider.metrics.resync_failures);
                throw TfError(TfErrorCode::SnapshotRevisionGap);
            }
            provider.highest_observed_revision = provider.highest_observed_revision
                                                     ? std::max(*provider.highest_observed_revision,
                                                                update.revision)
                                                     : update.revision;
            if (provider.pending.size() >= resources_.max_pending_static_updates) {
                provider.static_stale = true;
                provider.pending.clear();
                detail::increment(provider.metrics.resync_failures);
                throw TfError(TfErrorCode::SnapshotRevisionGap);
            }
            provider.pending.emplace(update.revision, std::move(update));
            return;
        }

        const auto current_revision = provider.static_set->revision;
        if (update.revision == current_revision) {
            if (*provider.static_set == update) {
                return;
            }
            provider.static_stale = true;
            detail::increment(provider.metrics.resync_failures);
            throw TfError(TfErrorCode::SnapshotRevisionGap);
        }
        if (update.revision > current_revision) {
            validate_static_chronology_or_stale(provider, *provider.static_set, update);
        }
        provider.highest_observed_revision = provider.highest_observed_revision
                                                 ? std::max(*provider.highest_observed_revision, update.revision)
                                                 : update.revision;
        if (provider.static_stale || current_revision == std::numeric_limits<std::uint64_t>::max()
            || update.revision != current_revision + 1U) {
            provider.static_stale = true;
            detail::increment(provider.metrics.resync_failures);
            throw TfError(TfErrorCode::SnapshotRevisionGap);
        }
        if (edge_count_replacing_static(state, authority.slot(), update.transforms.size())
            > resources_.max_total_edges) {
            throw TfError(TfErrorCode::CapacityExceeded);
        }
        for (const auto& [child, _] : provider.dynamic) {
            if (std::any_of(update.transforms.begin(), update.transforms.end(), [&child](const StaticEdge& edge) {
                    return edge.child == child;
                })) {
                throw TfError(TfErrorCode::ConflictingParent);
            }
        }
        ensure_acyclic_replacing_static(state, authority.slot(), update.transforms);
        std::vector<FrameId> children;
        children.reserve(update.transforms.size());
        for (const auto& edge : update.transforms) {
            children.push_back(edge.child);
        }
        provider.static_set = std::move(update);
        if (const auto conflict = conflict_in_set(state, children)) {
            count_conflict(state, provider, *conflict);
            throw TfError(*conflict);
        }
    }

    void mark_snapshot_stale(const Authority& authority) {
        auto& state = *state_;
        std::lock_guard lock(state.mutex);
        auto& provider = require_provider(state, authority);
        provider.static_stale = true;
    }

    [[nodiscard]] bool needs_resync(const Authority& authority) {
        auto& state = *state_;
        std::lock_guard lock(state.mutex);
        auto& provider = require_provider(state, authority);
        const auto now = SteadyClock::now();
        if (!provider.last_snapshot
            || now - *provider.last_snapshot >= resources_.snapshot_max_age) {
            provider.static_stale = true;
        }
        const auto resync_base = std::max(provider.last_resync, provider.last_snapshot);
        if (resync_base && now - *resync_base < resources_.resync_interval) {
            return false;
        }
        provider.last_resync = now;
        detail::increment(provider.metrics.resync_attempts);
        return true;
    }

    [[nodiscard]] LookupResult lookup_at(std::string_view target,
                                         std::string_view source,
                                         rbt::time::Timestamp stamp,
                                         LookupOptions options = {}) {
        const FrameId target_id(target);
        const FrameId source_id(source);
        if (!stamp.clock.is_well_formed()) {
            throw TfError(rbt::time::TimeError::InvalidTimestamp);
        }
        auto& state = *state_;
        std::lock_guard lock(state.mutex);
        detail::increment(state.metrics.lookup_attempts);
        for (auto& [_, provider] : state.providers) {
            detail::increment(provider.metrics.lookup_attempts);
        }
        try {
            auto result = lookup_locked(state, target_id, source_id, stamp, options);
            detail::increment(state.metrics.lookup_successes);
            for (auto& [_, provider] : state.providers) {
                detail::increment(provider.metrics.lookup_successes);
            }
            return result;
        } catch (const TfError& error) {
            detail::increment(state.metrics.lookup_failures);
            for (auto& [_, provider] : state.providers) {
                detail::increment(provider.metrics.lookup_failures);
            }
            const auto code = error.code();
            if (code == TfErrorCode::UnknownFrame) {
                detail::increment(state.metrics.missing_frame_count);
                for (auto& [_, provider] : state.providers) {
                    detail::increment(provider.metrics.missing_frame_count);
                }
            }
            if (code == TfErrorCode::UnknownFrame || code == TfErrorCode::DisconnectedFrames
                || code == TfErrorCode::MissingTransform || code == TfErrorCode::InsufficientHistory) {
                detail::increment(state.metrics.missing_transform_count);
                for (auto& [_, provider] : state.providers) {
                    detail::increment(provider.metrics.missing_transform_count);
                }
            } else if (code == TfErrorCode::PastExtrapolation || code == TfErrorCode::FutureExtrapolation
                       || code == TfErrorCode::InterpolationGapTooLarge) {
                detail::increment(state.metrics.extrapolation_count);
                for (auto& [_, provider] : state.providers) {
                    detail::increment(provider.metrics.extrapolation_count);
                }
            } else if (error.time_error()) {
                detail::increment(state.metrics.clock_error_count);
                for (auto& [_, provider] : state.providers) {
                    detail::increment(provider.metrics.clock_error_count);
                }
                if (is_clock_mapping_failure(*error.time_error())) {
                    detail::increment(state.metrics.clock_mapping_failure_count);
                    for (auto& [_, provider] : state.providers) {
                        detail::increment(provider.metrics.clock_mapping_failure_count);
                    }
                }
            } else if (code == TfErrorCode::ConflictingAuthority || code == TfErrorCode::ConflictingParent) {
                count_conflict(state, code);
            }
            throw;
        }
    }

    [[nodiscard]] TfPressureSnapshot pressure_snapshot() const {
        const auto& state = *state_;
        std::lock_guard lock(state.mutex);
        const auto now = SteadyClock::now();
        const auto claims = all_claims(state);
        std::set<FrameId, detail::FrameIdLess> frames;
        std::vector<ProviderObservation> observations;
        std::size_t total_samples = 0U;
        std::size_t static_edges = 0U;
        std::size_t dynamic_edges = 0U;
        observations.reserve(state.providers.size());
        for (const auto& [_, provider] : state.providers) {
            const auto static_count = provider.static_set ? provider.static_set->transforms.size() : 0U;
            const auto dynamic_count = provider.dynamic.size();
            std::size_t sample_count = 0U;
            for (const auto& [child, history] : provider.dynamic) {
                (void)child;
                sample_count += history.samples.size();
            }
            total_samples += sample_count;
            static_edges += static_count;
            dynamic_edges += dynamic_count;
            if (provider.static_set) {
                for (const auto& edge : provider.static_set->transforms) {
                    frames.insert(edge.parent);
                    frames.insert(edge.child);
                }
            }
            for (const auto& [child, history] : provider.dynamic) {
                frames.insert(child);
                frames.insert(history.parent);
            }
            std::optional<std::uint64_t> snapshot_age;
            if (provider.last_snapshot) {
                snapshot_age = detail::elapsed_ns(now, *provider.last_snapshot);
            }
            const bool static_tracked = provider.static_set.has_value()
                                        || provider.highest_observed_revision.has_value()
                                        || !provider.pending.empty();
            std::optional<bool> snapshot_fresh;
            if (static_tracked) {
                snapshot_fresh = snapshot_age && *snapshot_age < detail::duration_ns(resources_.snapshot_max_age);
            }
            std::optional<std::uint64_t> dynamic_age;
            for (const auto& [child, history] : provider.dynamic) {
                (void)child;
                if (history.last_received_at) {
                    const auto age = detail::elapsed_ns(now, *history.last_received_at);
                    dynamic_age = dynamic_age ? std::min(*dynamic_age, age) : age;
                }
            }
            const bool dynamic_fresh = dynamic_age && *dynamic_age <= detail::duration_ns(resources_.max_dynamic_age);
            const auto& metrics = provider.metrics;
            observations.push_back(ProviderObservation {
                provider.authority,
                provider.static_set ? std::optional<std::uint64_t>(provider.static_set->revision) : std::nullopt,
                provider.highest_observed_revision,
                static_tracked,
                static_tracked && (provider.static_stale || snapshot_fresh != std::optional<bool>(true)),
                snapshot_age,
                snapshot_fresh,
                dynamic_age,
                dynamic_fresh,
                static_count,
                dynamic_count,
                sample_count,
                metrics.evicted_samples,
                metrics.lookup_attempts,
                metrics.lookup_successes,
                metrics.lookup_failures,
                metrics.missing_transform_count,
                metrics.extrapolation_count,
                metrics.clock_error_count,
                metrics.conflict_count,
                metrics.resync_attempts,
                metrics.resync_failures,
                metrics.missing_frame_count,
                metrics.clock_mapping_failure_count,
                metrics.authority_conflict_count,
            });
        }
        std::vector<GraphEdgeObservation> graph;
        graph.reserve(claims.size());
        for (const auto& claim : claims) {
            std::optional<std::uint64_t> revision;
            std::optional<std::uint64_t> epoch;
            std::size_t samples = 0U;
            if (claim.is_static) {
                revision = claim.revision;
            } else {
                epoch = claim.epoch;
                const auto provider = state.providers.find(claim.authority.slot());
                if (provider != state.providers.end()) {
                    const auto history = provider->second.dynamic.find(claim.child);
                    if (history != provider->second.dynamic.end()) {
                        samples = history->second.samples.size();
                    }
                }
            }
            const auto same_child = static_cast<std::size_t>(std::count_if(
                claims.begin(), claims.end(), [&claim](const Claim& other) { return other.child == claim.child; }));
            graph.push_back(GraphEdgeObservation { claim.parent,
                                                   claim.child,
                                                   claim.authority,
                                                   claim.is_static,
                                                   revision,
                                                   epoch,
                                                   samples,
                                                   same_child == 0U ? 0U : same_child - 1U });
        }
        const auto& metrics = state.metrics;
        return TfPressureSnapshot { state.providers.size(),
                                     frames.size(),
                                     static_edges,
                                     dynamic_edges,
                                     total_samples,
                                     metrics.evicted_samples,
                                     metrics.lookup_attempts,
                                     metrics.lookup_successes,
                                     metrics.lookup_failures,
                                     metrics.missing_transform_count,
                                     metrics.extrapolation_count,
                                     metrics.clock_error_count,
                                     metrics.conflict_count,
                                     std::move(observations),
                                     std::move(graph),
                                     metrics.missing_frame_count,
                                     metrics.clock_mapping_failure_count,
                                     metrics.authority_conflict_count };
    }

  private:
    struct Metrics {
        std::uint64_t evicted_samples { 0U };
        std::uint64_t lookup_attempts { 0U };
        std::uint64_t lookup_successes { 0U };
        std::uint64_t lookup_failures { 0U };
        std::uint64_t missing_transform_count { 0U };
        std::uint64_t extrapolation_count { 0U };
        std::uint64_t clock_error_count { 0U };
        std::uint64_t conflict_count { 0U };
        std::uint64_t resync_attempts { 0U };
        std::uint64_t resync_failures { 0U };
        std::uint64_t missing_frame_count { 0U };
        std::uint64_t clock_mapping_failure_count { 0U };
        std::uint64_t authority_conflict_count { 0U };
    };

    struct Sample {
        DynamicRecord record;
        SteadyClock::time_point received_at;
        std::uint64_t order { 0U };
    };

    struct DynamicHistory {
        FrameId parent;
        rbt::time::ClockIdentity clock;
        std::uint64_t epoch { 0U };
        std::optional<rbt::time::Timestamp> boundary;
        std::optional<DynamicRecord> last_record;
        std::optional<SteadyClock::time_point> last_received_at;
        std::vector<Sample> samples;
    };

    struct Provider {
        explicit Provider(Authority value)
            : authority(std::move(value)) {
        }

        Authority authority;
        std::optional<StaticSet> static_set;
        bool static_stale { true };
        std::optional<std::uint64_t> highest_observed_revision;
        std::map<std::uint64_t, StaticSet> pending;
        std::optional<SteadyClock::time_point> last_snapshot;
        std::optional<SteadyClock::time_point> last_resync;
        std::map<FrameId, DynamicHistory, detail::FrameIdLess> dynamic;
        Metrics metrics;
    };

    struct StateMetrics {
        std::uint64_t evicted_samples { 0U };
        std::uint64_t lookup_attempts { 0U };
        std::uint64_t lookup_successes { 0U };
        std::uint64_t lookup_failures { 0U };
        std::uint64_t missing_transform_count { 0U };
        std::uint64_t extrapolation_count { 0U };
        std::uint64_t clock_error_count { 0U };
        std::uint64_t conflict_count { 0U };
        std::uint64_t missing_frame_count { 0U };
        std::uint64_t clock_mapping_failure_count { 0U };
        std::uint64_t authority_conflict_count { 0U };
    };

    struct Claim {
        Authority authority;
        FrameId parent;
        FrameId child;
        bool is_static { false };
        std::uint64_t revision { 0U };
        std::uint64_t epoch { 0U };
        StaticEdge edge;
    };

    struct State {
        mutable std::mutex mutex;
        std::map<FrameId, Provider, detail::FrameIdLess> providers;
        std::uint64_t next_order { 0U };
        StateMetrics metrics;
    };

    struct EdgeValue {
        FrameId parent;
        FrameId child;
        Transform3 transform { Transform3::identity() };
        std::optional<std::uint64_t> uncertainty_ns;
        TransformSource source;
    };

    const TfResourceConfig resources_;
    const LookupPolicy policy_;
    std::shared_ptr<State> state_;

    [[nodiscard]] Provider& require_provider(State& state, const Authority& authority) const {
        auto found = state.providers.find(authority.slot());
        if (found == state.providers.end() || found->second.authority != authority) {
            throw TfError(TfErrorCode::ProviderUnavailable);
        }
        return found->second;
    }

    [[nodiscard]] const Provider& require_provider(const State& state, const Authority& authority) const {
        const auto found = state.providers.find(authority.slot());
        if (found == state.providers.end() || found->second.authority != authority) {
            throw TfError(TfErrorCode::ProviderUnavailable);
        }
        return found->second;
    }

    void validate_static_set(const StaticSet& set) const {
        if (!set.effective_from.clock.is_well_formed()) {
            throw TfError(rbt::time::TimeError::InvalidTimestamp);
        }
        if (set.transforms.size() > resources_.max_static_transforms_per_provider) {
            throw TfError(TfErrorCode::SnapshotTooLarge);
        }
        std::set<FrameId, detail::FrameIdLess> children;
        std::vector<std::pair<FrameId, FrameId>> edges;
        edges.reserve(set.transforms.size());
        for (const auto& edge : set.transforms) {
            if (edge.parent == edge.child) {
                throw TfError(TfErrorCode::CycleDetected);
            }
            if (!children.insert(edge.child).second) {
                throw TfError(TfErrorCode::ConflictingParent);
            }
            edges.emplace_back(edge.child, edge.parent);
        }
        if (detail::has_cycle(edges)) {
            throw TfError(TfErrorCode::CycleDetected);
        }
    }

    static void validate_static_chronology_or_stale(Provider& provider,
                                                     const StaticSet& previous,
                                                     const StaticSet& next) {
        const auto order = rbt::time::checked_compare(previous.effective_from, next.effective_from);
        if (!order) {
            provider.static_stale = true;
            detail::increment(provider.metrics.resync_failures);
            throw TfError(order.error());
        }
        if (order.value() == std::strong_ordering::greater) {
            provider.static_stale = true;
            detail::increment(provider.metrics.resync_failures);
            throw TfError(TfErrorCode::SnapshotRevisionGap);
        }
    }

    [[nodiscard]] std::size_t owner_edge_count(const Provider& provider) const {
        return (provider.static_set ? provider.static_set->transforms.size() : 0U) + provider.dynamic.size();
    }

    [[nodiscard]] std::size_t edge_count(const State& state) const {
        std::size_t count = 0U;
        for (const auto& [_, provider] : state.providers) {
            count += owner_edge_count(provider);
        }
        return count;
    }

    [[nodiscard]] std::size_t edge_count_replacing_static(const State& state,
                                                          const FrameId& slot,
                                                          std::size_t new_size) const {
        const auto found = state.providers.find(slot);
        const auto old_size = found != state.providers.end() && found->second.static_set
                                  ? found->second.static_set->transforms.size()
                                  : 0U;
        return edge_count(state) - old_size + new_size;
    }

    [[nodiscard]] std::vector<Claim> all_claims(const State& state) const {
        std::vector<Claim> claims;
        claims.reserve(edge_count(state));
        for (const auto& [_, provider] : state.providers) {
            if (provider.static_set) {
                for (const auto& edge : provider.static_set->transforms) {
                    claims.push_back(Claim { provider.authority,
                                             edge.parent,
                                             edge.child,
                                             true,
                                             provider.static_set->revision,
                                             0U,
                                             edge });
                }
            }
            for (const auto& [child, history] : provider.dynamic) {
                claims.push_back(Claim { provider.authority,
                                         history.parent,
                                         child,
                                         false,
                                         0U,
                                         history.epoch,
                                         StaticEdge { history.parent, child, Transform3::identity() } });
            }
        }
        return claims;
    }

    void ensure_acyclic_with_extra(const State& state, const FrameId& parent, const FrameId& child) const {
        auto claims = all_claims(state);
        std::vector<std::pair<FrameId, FrameId>> edges;
        edges.reserve(claims.size() + 1U);
        for (const auto& claim : claims) {
            edges.emplace_back(claim.child, claim.parent);
        }
        edges.emplace_back(child, parent);
        if (detail::has_cycle(edges)) {
            throw TfError(TfErrorCode::CycleDetected);
        }
    }

    void ensure_acyclic_replacing_static(const State& state,
                                         const FrameId& slot,
                                         const std::vector<StaticEdge>& set) const {
        std::vector<std::pair<FrameId, FrameId>> edges;
        edges.reserve(edge_count(state) + set.size());
        for (const auto& [provider_slot, provider] : state.providers) {
            if (provider_slot != slot && provider.static_set) {
                for (const auto& edge : provider.static_set->transforms) {
                    edges.emplace_back(edge.child, edge.parent);
                }
            }
            for (const auto& [child, history] : provider.dynamic) {
                edges.emplace_back(child, history.parent);
            }
        }
        for (const auto& edge : set) {
            edges.emplace_back(edge.child, edge.parent);
        }
        if (detail::has_cycle(edges)) {
            throw TfError(TfErrorCode::CycleDetected);
        }
    }

    [[nodiscard]] std::optional<TfErrorCode> conflict_in_set(const State& state,
                                                              const std::vector<FrameId>& children) const {
        const auto claims = all_claims(state);
        for (const auto& child : children) {
            std::optional<FrameId> parent;
            std::optional<CapabilityInstanceId> physical;
            for (const auto& claim : claims) {
                if (claim.child != child) {
                    continue;
                }
                if (parent && *parent != claim.parent) {
                    return TfErrorCode::ConflictingParent;
                }
                parent = claim.parent;
                if (physical && *physical != claim.authority.physical()) {
                    return TfErrorCode::ConflictingAuthority;
                }
                physical = claim.authority.physical();
            }
        }
        return {};
    }

    [[nodiscard]] static bool is_clock_mapping_failure(rbt::time::TimeError error) noexcept {
        using rbt::time::TimeError;
        switch (error) {
        case TimeError::ClockDomainMismatch:
        case TimeError::ClockSourceMismatch:
        case TimeError::ClockGenerationMismatch:
        case TimeError::UnknownClockDomain:
        case TimeError::ClockMappingUnavailable:
        case TimeError::ClockMappingExpired:
        case TimeError::ClockMappingInvalid:
        case TimeError::MappingInvalidated:
            return true;
        default:
            return false;
        }
    }

    void count_conflict(State& state, TfErrorCode code) const {
        detail::increment(state.metrics.conflict_count);
        if (code == TfErrorCode::ConflictingAuthority) {
            detail::increment(state.metrics.authority_conflict_count);
        }
        for (auto& [_, provider] : state.providers) {
            detail::increment(provider.metrics.conflict_count);
            if (code == TfErrorCode::ConflictingAuthority) {
                detail::increment(provider.metrics.authority_conflict_count);
            }
        }
    }

    void count_conflict(State& state, Provider& provider, TfErrorCode code) const {
        detail::increment(state.metrics.conflict_count);
        detail::increment(provider.metrics.conflict_count);
        if (code == TfErrorCode::ConflictingAuthority) {
            detail::increment(state.metrics.authority_conflict_count);
            detail::increment(provider.metrics.authority_conflict_count);
        }
    }

    [[nodiscard]] std::uint64_t prune_history(DynamicHistory& history) const {
        const auto original = history.samples.size();
        while (history.samples.size() > resources_.max_samples_per_edge) {
            history.samples.erase(history.samples.begin());
        }
        if (!history.samples.empty()) {
            const auto newest = history.samples.back().record.stamp.ns;
            const auto maximum_age = detail::duration_ns(resources_.max_history_duration);
            while (!history.samples.empty()
                   && detail::unsigned_difference(newest, history.samples.front().record.stamp.ns) > maximum_age) {
                history.samples.erase(history.samples.begin());
            }
        }
        return static_cast<std::uint64_t>(original - history.samples.size());
    }

    [[nodiscard]] std::size_t total_samples(const State& state) const {
        std::size_t count = 0U;
        for (const auto& [_, provider] : state.providers) {
            for (const auto& [child, history] : provider.dynamic) {
                (void)child;
                count += history.samples.size();
            }
        }
        return count;
    }

    void enforce_global_sample_limit(State& state) const {
        while (total_samples(state) > resources_.max_total_samples) {
            auto oldest_provider = state.providers.end();
            auto oldest_sample = std::numeric_limits<std::uint64_t>::max();
            std::optional<FrameId> oldest_child;
            for (auto provider_it = state.providers.begin(); provider_it != state.providers.end(); ++provider_it) {
                for (const auto& [child, history] : provider_it->second.dynamic) {
                    if (!history.samples.empty() && history.samples.front().order < oldest_sample) {
                        oldest_sample = history.samples.front().order;
                        oldest_provider = provider_it;
                        oldest_child = child;
                    }
                }
            }
            if (!oldest_child) {
                break;
            }
            auto history = oldest_provider->second.dynamic.find(*oldest_child);
            history->second.samples.erase(history->second.samples.begin());
            detail::increment(oldest_provider->second.metrics.evicted_samples);
            detail::increment(state.metrics.evicted_samples);
        }
    }

    void validate_dynamic_transition(const DynamicHistory& history, const DynamicRecord& record) const {
        if (record.transform_epoch < history.epoch) {
            throw TfError(TfErrorCode::TransformEpochMismatch);
        }
        const auto& old_clock = history.clock;
        const auto& new_clock = record.stamp.clock;
        if (old_clock != new_clock) {
            if (old_clock.kind() != new_clock.kind()) {
                throw TfError(rbt::time::TimeError::ClockDomainMismatch);
            }
            if (old_clock.source_id() != new_clock.source_id()) {
                throw TfError(rbt::time::TimeError::ClockSourceMismatch);
            }
            if (new_clock.generation() < old_clock.generation()) {
                throw TfError(rbt::time::TimeError::ClockGenerationMismatch);
            }
        }
    }

    [[nodiscard]] std::vector<Claim> claims_for_child(const std::vector<Claim>& claims,
                                                       const FrameId& child) const {
        std::vector<Claim> selected;
        for (const auto& claim : claims) {
            if (claim.child == child) {
                selected.push_back(claim);
            }
        }
        return selected;
    }

    [[nodiscard]] const Claim* unique_claim(const std::vector<Claim>& claims,
                                             const FrameId& child,
                                             std::optional<TfErrorCode>& error) const {
        const Claim* selected = nullptr;
        std::optional<FrameId> parent;
        std::optional<CapabilityInstanceId> physical;
        for (const auto& claim : claims) {
            if (claim.child != child) {
                continue;
            }
            if (parent && *parent != claim.parent) {
                error = TfErrorCode::ConflictingParent;
                return nullptr;
            }
            parent = claim.parent;
            if (physical && *physical != claim.authority.physical()) {
                error = TfErrorCode::ConflictingAuthority;
                return nullptr;
            }
            physical = claim.authority.physical();
            if (selected == nullptr) {
                selected = &claim;
            }
        }
        return selected;
    }

    [[nodiscard]] LookupResult lookup_locked(const State& state,
                                              const FrameId& target,
                                              const FrameId& source,
                                              const rbt::time::Timestamp& stamp,
                                              LookupOptions options) const {
        validate_uncertainty(options.timestamp_uncertainty_ns);
        const auto claims = all_claims(state);
        std::set<FrameId, detail::FrameIdLess> known_frames;
        for (const auto& claim : claims) {
            known_frames.insert(claim.parent);
            known_frames.insert(claim.child);
        }
        if (!known_frames.contains(target) || !known_frames.contains(source)) {
            throw TfError(TfErrorCode::UnknownFrame);
        }
        if (target == source) {
            const auto precision = combined_uncertainty({ options.timestamp_uncertainty_ns });
            return LookupResult { Transform3::identity(), stamp, precision, {} };
        }

        std::set<FrameId, detail::FrameIdLess> source_nodes { source };
        std::vector<Claim> source_edges;
        auto current = source;
        std::optional<TfErrorCode> source_blocker;
        for (std::size_t depth = 0U; depth <= resources_.max_total_edges; ++depth) {
            std::optional<TfErrorCode> conflict;
            const auto* claim = unique_claim(claims, current, conflict);
            if (conflict) {
                source_blocker = conflict;
                break;
            }
            if (claim == nullptr) {
                break;
            }
            if (source_nodes.contains(claim->parent)) {
                throw TfError(TfErrorCode::CycleDetected);
            }
            source_edges.push_back(*claim);
            current = claim->parent;
            source_nodes.insert(current);
        }

        std::vector<Claim> target_edges;
        std::set<FrameId, detail::FrameIdLess> target_nodes { target };
        current = target;
        std::optional<FrameId> common;
        for (std::size_t depth = 0U; depth <= resources_.max_total_edges; ++depth) {
            if (source_nodes.contains(current)) {
                common = current;
                break;
            }
            std::optional<TfErrorCode> conflict;
            const auto* claim = unique_claim(claims, current, conflict);
            if (conflict) {
                throw TfError(*conflict);
            }
            if (claim == nullptr) {
                break;
            }
            if (target_nodes.contains(claim->parent)) {
                throw TfError(TfErrorCode::CycleDetected);
            }
            target_edges.push_back(*claim);
            current = claim->parent;
            target_nodes.insert(current);
        }
        if (!common) {
            if (source_blocker) {
                throw TfError(*source_blocker);
            }
            throw TfError(TfErrorCode::DisconnectedFrames);
        }

        auto source_transform = Transform3::identity();
        std::vector<TransformSource> source_sources;
        auto source_precision = options.timestamp_uncertainty_ns;
        current = source;
        for (const auto& claim : source_edges) {
            if (current == *common) {
                break;
            }
            const auto edge = resolve_claim(state, claim, stamp, options);
            source_transform = edge.transform.compose(source_transform);
            source_precision = combined_uncertainty({ source_precision, edge.uncertainty_ns });
            source_sources.push_back(edge.source);
            current = edge.parent;
            if (current == *common) {
                break;
            }
        }
        if (current != *common) {
            if (source_blocker) {
                throw TfError(*source_blocker);
            }
            throw TfError(TfErrorCode::DisconnectedFrames);
        }

        auto target_transform = Transform3::identity();
        std::vector<TransformSource> target_sources;
        auto target_precision = options.timestamp_uncertainty_ns;
        current = target;
        for (const auto& claim : target_edges) {
            const auto edge = resolve_claim(state, claim, stamp, options);
            target_transform = target_transform.compose(edge.transform.inverse());
            target_precision = combined_uncertainty({ target_precision, edge.uncertainty_ns });
            target_sources.push_back(edge.source);
            current = edge.parent;
            if (current == *common) {
                break;
            }
        }
        if (current != *common) {
            throw TfError(TfErrorCode::DisconnectedFrames);
        }
        const auto result_transform = target_transform.compose(source_transform);
        target_sources.insert(target_sources.end(), source_sources.begin(), source_sources.end());
        const auto precision = combined_uncertainty(
            { target_precision, source_precision, options.timestamp_uncertainty_ns });
        validate_uncertainty(precision);
        return LookupResult { result_transform, stamp, precision, std::move(target_sources) };
    }

    [[nodiscard]] EdgeValue resolve_claim(const State& state,
                                          const Claim& claim,
                                          const rbt::time::Timestamp& query,
                                          LookupOptions options) const {
        const auto& provider = require_provider(state, claim.authority);
        if (claim.is_static) {
            const auto now = SteadyClock::now();
            if (provider.static_stale || !provider.last_snapshot
                || now - *provider.last_snapshot >= resources_.snapshot_max_age || !provider.static_set) {
                throw TfError(TfErrorCode::SnapshotStale);
            }
            const auto& set = *provider.static_set;
            const auto [mapped_query, query_uncertainty] = map_query_to_edge(
                query, options.timestamp_uncertainty_ns, set.effective_from.clock, options);
            const auto compared = rbt::time::checked_compare(mapped_query, set.effective_from);
            if (!compared) {
                throw TfError(compared.error());
            }
            if (compared.value() == std::strong_ordering::less) {
                throw TfError(TfErrorCode::PastExtrapolation);
            }
            const auto precision = combined_uncertainty({ query_uncertainty,
                                                          set.effective_uncertainty_ns });
            return EdgeValue { claim.parent,
                               claim.child,
                               claim.edge.transform,
                               precision,
                               TransformSource { claim.authority,
                                                 claim.parent,
                                                 claim.child,
                                                 mapped_query,
                                                 set.effective_from,
                                                 set.effective_from,
                                                 claim.revision,
                                                 {} } };
        }
        const auto history_it = provider.dynamic.find(claim.child);
        if (history_it == provider.dynamic.end()) {
            throw TfError(TfErrorCode::MissingTransform);
        }
        const auto& history = history_it->second;
        const auto [mapped_query, query_uncertainty] = map_query_to_edge(
            query, options.timestamp_uncertainty_ns, history.clock, options);
        if (history.boundary && mapped_query.clock == history.boundary->clock
            && mapped_query.ns < history.boundary->ns) {
            throw TfError(TfErrorCode::TransformEpochMismatch);
        }
        return resolve_dynamic(claim, history, mapped_query, query_uncertainty);
    }

    [[nodiscard]] EdgeValue resolve_dynamic(const Claim& claim,
                                           const DynamicHistory& history,
                                           const rbt::time::Timestamp& mapped_query,
                                           std::optional<std::uint64_t> query_uncertainty) const {
        const auto now = SteadyClock::now();
        if (history.samples.empty()) {
            throw TfError(TfErrorCode::InsufficientHistory);
        }
        if (!history.last_received_at
            || now - *history.last_received_at > resources_.max_dynamic_age) {
            throw TfError(TfErrorCode::ProviderUnavailable);
        }
        struct Converted {
            const Sample* sample;
            rbt::time::Timestamp stamp;
            std::optional<std::uint64_t> uncertainty;
        };
        std::vector<Converted> samples;
        samples.reserve(history.samples.size());
        for (const auto& sample : history.samples) {
            samples.push_back(Converted { &sample, sample.record.stamp, sample.record.uncertainty_ns });
        }
        const auto exact = std::find_if(samples.begin(), samples.end(), [&mapped_query](const Converted& value) {
            return value.stamp.ns == mapped_query.ns;
        });
        if (exact != samples.end()) {
            const auto precision = combined_uncertainty({ query_uncertainty, exact->uncertainty });
            return EdgeValue { claim.parent,
                               claim.child,
                               exact->sample->record.transform,
                               precision,
                               TransformSource { claim.authority,
                                                 claim.parent,
                                                 claim.child,
                                                 mapped_query,
                                                 exact->sample->record.stamp,
                                                 exact->sample->record.stamp,
                                                 {},
                                                 exact->sample->record.transform_epoch } };
        }
        if (mapped_query.ns < samples.front().stamp.ns) {
            throw TfError(TfErrorCode::PastExtrapolation);
        }
        if (mapped_query.ns > samples.back().stamp.ns) {
            throw TfError(TfErrorCode::FutureExtrapolation);
        }
        auto after = std::adjacent_find(samples.begin(), samples.end(), [&mapped_query](const Converted& before_value,
                                                                                        const Converted& after_value) {
            return before_value.stamp.ns < mapped_query.ns && mapped_query.ns < after_value.stamp.ns;
        });
        if (after == samples.end()) {
            throw TfError(TfErrorCode::InsufficientHistory);
        }
        const auto before = after;
        ++after;
        const auto gap_result = rbt::time::checked_duration_since(after->stamp, before->stamp);
        if (!gap_result) {
            throw TfError(gap_result.error());
        }
        const auto gap = gap_result.value();
        if (gap > policy_.max_interpolation_gap) {
            throw TfError(TfErrorCode::InterpolationGapTooLarge);
        }
        const auto elapsed_result = rbt::time::checked_difference(mapped_query, before->stamp);
        if (!elapsed_result) {
            throw TfError(elapsed_result.error());
        }
        const auto amount = static_cast<double>(elapsed_result.value())
                            / static_cast<double>(gap.count());
        const auto transform = before->sample->record.transform.interpolate(after->sample->record.transform, amount);
        const auto precision = combined_uncertainty({ query_uncertainty,
                                                      before->uncertainty,
                                                      after->uncertainty });
        return EdgeValue { claim.parent,
                           claim.child,
                           transform,
                           precision,
                           TransformSource { claim.authority,
                                             claim.parent,
                                             claim.child,
                                             mapped_query,
                                             before->sample->record.stamp,
                                             after->sample->record.stamp,
                                             {},
                                             before->sample->record.transform_epoch } };
    }

    [[nodiscard]] std::pair<rbt::time::Timestamp, std::optional<std::uint64_t>> map_query_to_edge(
        const rbt::time::Timestamp& query,
        std::optional<std::uint64_t> query_uncertainty,
        const rbt::time::ClockIdentity& edge_clock,
        LookupOptions options) const {
        if (query.clock == edge_clock) {
            return { query, query_uncertainty };
        }
        const rbt::time::ClockMapping* mapping = nullptr;
        bool mapping_generation_mismatch = false;
        for (const auto* candidate : options.mappings) {
            if (candidate == nullptr) {
                continue;
            }
            const auto& calibration = candidate->calibration();
            if (calibration.source == query.clock && calibration.target == edge_clock) {
                mapping = candidate;
                break;
            }
            if (detail::same_clock_domain_and_source(calibration.source, query.clock)
                && detail::same_clock_domain_and_source(calibration.target, edge_clock)) {
                mapping_generation_mismatch = true;
            }
        }
        if (mapping == nullptr) {
            if (mapping_generation_mismatch
                || detail::same_clock_domain_and_source(query.clock, edge_clock)) {
                throw TfError(rbt::time::TimeError::ClockGenerationMismatch);
            }
            throw TfError(rbt::time::TimeError::ClockMappingUnavailable);
        }
        if (options.now == nullptr) {
            throw TfError(rbt::time::TimeError::ClockMappingUnavailable);
        }
        auto result = mapping->convert(query, query_uncertainty, edge_clock, *options.now);
        if (!result) {
            throw TfError(result.error());
        }
        return { result.value().timestamp, result.value().uncertainty_ns };
    }

    [[nodiscard]] std::optional<std::uint64_t> combined_uncertainty(
        std::initializer_list<std::optional<std::uint64_t>> values) const {
        std::optional<std::uint64_t> maximum;
        bool has_unknown = false;
        for (const auto& value : values) {
            if (!value) {
                has_unknown = true;
            } else {
                maximum = maximum ? std::max(*maximum, *value) : *value;
            }
        }
        if (!maximum) {
            if (policy_.allow_unknown_uncertainty) {
                return {};
            }
            throw TfError(rbt::time::TimeError::UnknownUncertainty);
        }
        if (has_unknown) {
            if (!policy_.allow_unknown_uncertainty) {
                throw TfError(rbt::time::TimeError::UnknownUncertainty);
            }
            validate_uncertainty(maximum);
            return {};
        }
        return maximum;
    }

    void validate_uncertainty(std::optional<std::uint64_t> uncertainty) const {
        if (!uncertainty && !policy_.allow_unknown_uncertainty) {
            throw TfError(rbt::time::TimeError::UnknownUncertainty);
        }
        if (policy_.max_timestamp_uncertainty_ns && uncertainty
            && *uncertainty > *policy_.max_timestamp_uncertainty_ns) {
            throw TfError(TfErrorCode::TimestampUncertaintyExceeded);
        }
    }
};

} // namespace rbt::tf

#endif

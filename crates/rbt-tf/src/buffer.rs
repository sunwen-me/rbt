//! Thread-safe, local TF graph and bounded dynamic history.

use std::{
    cmp::Ordering,
    collections::{BTreeMap, BTreeSet},
    sync::{Arc, Mutex, MutexGuard},
    time::{Duration, Instant},
};

use rbt_core::CapabilityInstanceId;
use rbt_time::{ClockIdentity, TimeError, Timestamp};

use crate::{
    Authority, DynamicRecord, FrameId, GraphEdgeObservation, LookupOptions, LookupPolicy,
    LookupResult, ProviderObservation, StaticEdge, StaticSet, TfError, TfPressureSnapshot,
    TfResourceConfig, TfResult, Transform3, TransformSource,
};

/// A bounded local transform graph with no transport or callback behavior.
#[derive(Clone)]
pub struct TransformBuffer {
    resources: TfResourceConfig,
    policy: LookupPolicy,
    state: Arc<Mutex<State>>,
}

#[derive(Default)]
struct State {
    providers: BTreeMap<FrameId, Provider>,
    next_order: u64,
    metrics: Metrics,
}

struct Provider {
    authority: Authority,
    static_set: Option<StaticSet>,
    static_stale: bool,
    highest_observed_revision: Option<u64>,
    pending: BTreeMap<u64, StaticSet>,
    accepted_static: BTreeMap<u64, StaticSet>,
    last_snapshot: Option<Instant>,
    last_resync: Option<Instant>,
    dynamic: BTreeMap<FrameId, DynamicHistory>,
    metrics: Metrics,
}

#[derive(Default)]
struct Metrics {
    evicted_samples: u64,
    lookup_attempts: u64,
    lookup_successes: u64,
    lookup_failures: u64,
    missing_transform_count: u64,
    extrapolation_count: u64,
    clock_error_count: u64,
    conflict_count: u64,
    resync_attempts: u64,
    resync_failures: u64,
    missing_frame_count: u64,
    clock_mapping_failure_count: u64,
    authority_conflict_count: u64,
}

#[derive(Clone)]
struct DynamicHistory {
    parent: FrameId,
    clock: ClockIdentity,
    epoch: u64,
    boundary: Option<Timestamp>,
    last_record: Option<DynamicRecord>,
    last_received_at: Option<Instant>,
    samples: Vec<StoredSample>,
}

#[derive(Clone)]
struct StoredSample {
    record: DynamicRecord,
    received_at: Instant,
    order: u64,
}

#[derive(Clone, Copy)]
enum ClaimKind {
    Static { edge: StaticEdge, revision: u64 },
    Dynamic,
}

#[derive(Clone, Copy)]
struct Claim {
    authority: Authority,
    parent: FrameId,
    child: FrameId,
    kind: ClaimKind,
}

#[derive(Clone)]
struct EdgeValue {
    parent: FrameId,
    transform: Transform3,
    uncertainty_ns: Option<u64>,
    source: TransformSource,
}

impl TransformBuffer {
    /// Creates a buffer after validating all allocation and timing limits.
    pub fn new(resources: TfResourceConfig, policy: LookupPolicy) -> TfResult<Self> {
        resources.validate()?;
        policy.validate()?;
        Ok(Self {
            resources,
            policy,
            state: Arc::new(Mutex::new(State::default())),
        })
    }

    /// Binds a logical provider slot to a physical capability instance.
    /// Rebinding removes the previous activation's entire local contribution.
    pub fn bind_provider(&self, slot: &str, id: CapabilityInstanceId) -> TfResult<Authority> {
        if id.as_uuid().is_nil() {
            return Err(TfError::InvalidConfig);
        }
        let slot = FrameId::new(slot)?;
        let mut state = self.lock()?;
        if let Some(provider) = state.providers.get(&slot) {
            if provider.authority.physical == id {
                return Ok(provider.authority);
            }
        } else if state.providers.len() >= self.resources.max_providers {
            return Err(TfError::CapacityExceeded);
        }
        if state
            .providers
            .values()
            .any(|provider| provider.authority.physical == id)
        {
            return Err(TfError::ConflictingAuthority);
        }

        state.providers.insert(
            slot,
            Provider {
                authority: Authority { slot, physical: id },
                static_set: None,
                static_stale: true,
                highest_observed_revision: None,
                pending: BTreeMap::new(),
                accepted_static: BTreeMap::new(),
                last_snapshot: None,
                last_resync: None,
                dynamic: BTreeMap::new(),
                metrics: Metrics::default(),
            },
        );
        Ok(Authority { slot, physical: id })
    }

    /// Removes all state for this exact current provider binding.
    pub fn unbind_provider(&self, authority: &Authority) -> TfResult<()> {
        let mut state = self.lock()?;
        self.require_provider(&state, authority)?;
        state.providers.remove(&authority.slot);
        Ok(())
    }

    /// Adds one strictly ordered dynamic sample to bounded edge history.
    pub fn ingest_dynamic(&self, authority: &Authority, record: DynamicRecord) -> TfResult<()> {
        let mut state = self.lock()?;
        self.require_provider(&state, authority)?;
        if record.parent == record.child {
            return Err(TfError::CycleDetected);
        }
        {
            let provider = state
                .providers
                .get(&authority.slot)
                .expect("checked provider");
            if provider
                .static_set
                .as_ref()
                .is_some_and(|set| set.transforms.iter().any(|edge| edge.child == record.child))
            {
                return Err(TfError::ConflictingParent);
            }
            if let Some(history) = provider.dynamic.get(&record.child) {
                if history.parent != record.parent {
                    return Err(TfError::ConflictingParent);
                }
                validate_dynamic_transition(history, &record)?;
                if record.transform_epoch == history.epoch
                    && let Some(previous) = history.last_record
                {
                    if record.stamp == previous.stamp {
                        if record == previous {
                            return Ok(());
                        }
                        return Err(TfError::OutOfOrderSample);
                    }
                    if record.stamp.clock() == &history.clock
                        && record.stamp.nanoseconds() <= previous.stamp.nanoseconds()
                    {
                        return Err(TfError::OutOfOrderSample);
                    }
                }
            }
        }

        let is_new_edge = !state.providers[&authority.slot]
            .dynamic
            .contains_key(&record.child);
        if is_new_edge {
            if self.edge_count(&state) >= self.resources.max_total_edges {
                return Err(TfError::CapacityExceeded);
            }
            if self.owner_edge_count(&state.providers[&authority.slot])
                >= self.resources.max_total_edges
            {
                return Err(TfError::CapacityExceeded);
            }
            self.ensure_acyclic_with_extra(&state, record.parent, record.child)?;
        }

        let now = Instant::now();
        let order = state.next_order;
        state.next_order = state
            .next_order
            .checked_add(1)
            .ok_or(TfError::CapacityExceeded)?;
        let history = state
            .providers
            .get_mut(&authority.slot)
            .expect("checked provider")
            .dynamic
            .entry(record.child)
            .or_insert_with(|| DynamicHistory {
                parent: record.parent,
                clock: *record.stamp.clock(),
                epoch: record.transform_epoch,
                boundary: None,
                last_record: None,
                last_received_at: None,
                samples: Vec::new(),
            });

        let mut reset_evictions = 0_u64;
        let reset =
            record.transform_epoch > history.epoch || record.stamp.clock() != &history.clock;
        if reset {
            reset_evictions = history.samples.len() as u64;
            history.samples.clear();
            history.samples.shrink_to_fit();
            history.boundary = Some(record.stamp);
            history.epoch = record.transform_epoch;
            history.clock = *record.stamp.clock();
        }
        history.last_record = Some(record);
        history.last_received_at = Some(now);
        history.samples.push(StoredSample {
            record,
            received_at: now,
            order,
        });
        let pruned = self.prune_history(history);
        if pruned != 0 || reset_evictions != 0 {
            let provider = state
                .providers
                .get_mut(&authority.slot)
                .expect("provider retained");
            let evicted = pruned.saturating_add(reset_evictions);
            provider.metrics.evicted_samples =
                provider.metrics.evicted_samples.saturating_add(evicted);
            state.metrics.evicted_samples = state.metrics.evicted_samples.saturating_add(evicted);
        }
        self.enforce_global_sample_limit(&mut state);
        if let Some(error) = self.conflict_in_set(&state, [record.child]) {
            state.metrics.conflict_count = state.metrics.conflict_count.saturating_add(1);
            if matches!(&error, TfError::ConflictingAuthority) {
                state.metrics.authority_conflict_count =
                    state.metrics.authority_conflict_count.saturating_add(1);
            }
            let metrics = &mut state.providers.get_mut(&authority.slot).unwrap().metrics;
            metrics.conflict_count = metrics.conflict_count.saturating_add(1);
            if matches!(&error, TfError::ConflictingAuthority) {
                metrics.authority_conflict_count =
                    metrics.authority_conflict_count.saturating_add(1);
            }
            return Err(error);
        }
        Ok(())
    }

    /// Atomically replaces one provider's static set with an authoritative snapshot.
    pub fn apply_static_snapshot(
        &self,
        authority: &Authority,
        mut snapshot: StaticSet,
    ) -> TfResult<()> {
        self.validate_static_set(&snapshot)?;
        snapshot.transforms = copy_bounded(&snapshot.transforms);
        let mut state = self.lock()?;
        self.require_provider(&state, authority)?;

        let current = state.providers[&authority.slot].static_set.clone();
        let pending = state.providers[&authority.slot].pending.clone();
        let highest_observed = state.providers[&authority.slot].highest_observed_revision;
        let was_stale = state.providers[&authority.slot].static_stale;
        if let Some(current) = &current {
            if snapshot.revision < current.revision {
                if was_stale || highest_observed.is_some_and(|highest| highest > current.revision) {
                    let provider = state.providers.get_mut(&authority.slot).unwrap();
                    provider.static_stale = true;
                    provider.metrics.resync_failures =
                        provider.metrics.resync_failures.saturating_add(1);
                    return Err(TfError::SnapshotRevisionGap);
                }
                // An older in-flight RPC response cannot replace or stale the current view.
                return Ok(());
            }
            if snapshot.revision == current.revision && snapshot != *current {
                let provider = state.providers.get_mut(&authority.slot).unwrap();
                provider.static_stale = true;
                provider.metrics.resync_failures =
                    provider.metrics.resync_failures.saturating_add(1);
                return Err(TfError::SnapshotRevisionGap);
            }
        }
        if let Some(current) = current
            .as_ref()
            .filter(|current| snapshot.revision > current.revision)
            && let Err(error) = validate_static_chronology(current, &snapshot)
        {
            mark_static_stale(state.providers.get_mut(&authority.slot).unwrap());
            return Err(error);
        }

        let mut merged = snapshot;
        let mut accepted_sets = vec![merged.clone()];
        let mut remaining = pending;
        remaining.retain(|revision, _| *revision > merged.revision);
        while let Some(next_revision) = merged.revision.checked_add(1) {
            let Some(next) = remaining.remove(&next_revision) else {
                break;
            };
            if let Err(error) = validate_static_chronology(&merged, &next) {
                mark_static_stale(state.providers.get_mut(&authority.slot).unwrap());
                return Err(error);
            }
            merged = next;
            accepted_sets.push(merged.clone());
        }
        let has_gap = !remaining.is_empty()
            || highest_observed.is_some_and(|highest| highest > merged.revision);
        if self.edge_count_replacing_static(&state, authority.slot, merged.transforms.len())
            > self.resources.max_total_edges
        {
            return Err(TfError::CapacityExceeded);
        }
        if state.providers[&authority.slot]
            .dynamic
            .keys()
            .any(|child| merged.transforms.iter().any(|edge| edge.child == *child))
        {
            return Err(TfError::ConflictingParent);
        }
        self.ensure_acyclic_replacing_static(&state, authority.slot, &merged.transforms)?;

        let received_at = Instant::now();
        let target = state.providers.get_mut(&authority.slot).unwrap();
        target.static_set = Some(merged.clone());
        target.static_stale = has_gap;
        target.highest_observed_revision = Some(
            target
                .highest_observed_revision
                .map_or(merged.revision, |revision| revision.max(merged.revision)),
        );
        target.pending.clear();
        for set in accepted_sets {
            target.accepted_static.insert(set.revision, set);
        }
        trim_revision_history(
            &mut target.accepted_static,
            self.resources.max_pending_static_updates,
        );
        target.last_snapshot = Some(received_at);
        if has_gap {
            target.metrics.resync_failures = target.metrics.resync_failures.saturating_add(1);
        }
        let conflict =
            self.conflict_in_set(&state, merged.transforms.iter().map(|edge| edge.child));
        if let Some(error) = conflict {
            state.metrics.conflict_count = state.metrics.conflict_count.saturating_add(1);
            if matches!(&error, TfError::ConflictingAuthority) {
                state.metrics.authority_conflict_count =
                    state.metrics.authority_conflict_count.saturating_add(1);
            }
            let metrics = &mut state.providers.get_mut(&authority.slot).unwrap().metrics;
            metrics.conflict_count = metrics.conflict_count.saturating_add(1);
            if matches!(&error, TfError::ConflictingAuthority) {
                metrics.authority_conflict_count =
                    metrics.authority_conflict_count.saturating_add(1);
            }
            return Err(error);
        }
        if has_gap {
            Err(TfError::SnapshotRevisionGap)
        } else {
            Ok(())
        }
    }

    /// Applies a complete static update, staging it until the first snapshot arrives.
    pub fn apply_static_update(
        &self,
        authority: &Authority,
        mut update: StaticSet,
    ) -> TfResult<()> {
        self.validate_static_set(&update)?;
        update.transforms = copy_bounded(&update.transforms);
        let mut state = self.lock()?;
        self.require_provider(&state, authority)?;
        let provider = &state.providers[&authority.slot];
        let current_revision = provider.static_set.as_ref().map(|set| set.revision);

        if provider.static_set.is_none() {
            if let Some(previous) = provider.pending.get(&update.revision) {
                if previous == &update {
                    return Ok(());
                }
                let target = state.providers.get_mut(&authority.slot).unwrap();
                target.static_stale = true;
                target.metrics.resync_failures = target.metrics.resync_failures.saturating_add(1);
                return Err(TfError::SnapshotRevisionGap);
            }
            let target = state.providers.get_mut(&authority.slot).unwrap();
            target.highest_observed_revision = Some(
                target
                    .highest_observed_revision
                    .map_or(update.revision, |revision| revision.max(update.revision)),
            );
            if target.pending.len() >= self.resources.max_pending_static_updates {
                target.static_stale = true;
                target.pending.clear();
                target.metrics.resync_failures = target.metrics.resync_failures.saturating_add(1);
                return Err(TfError::SnapshotRevisionGap);
            }
            target.pending.insert(update.revision, update);
            return Ok(());
        }

        let current_revision = current_revision.expect("snapshot checked");
        if update.revision < current_revision {
            let previous = state.providers[&authority.slot]
                .accepted_static
                .get(&update.revision);
            if previous == Some(&update) {
                return Ok(());
            }
            let target = state.providers.get_mut(&authority.slot).unwrap();
            target.static_stale = true;
            target.metrics.resync_failures = target.metrics.resync_failures.saturating_add(1);
            return Err(TfError::SnapshotRevisionGap);
        }
        if update.revision == current_revision {
            if state.providers[&authority.slot].static_set.as_ref() == Some(&update) {
                return Ok(());
            }
            let target = state.providers.get_mut(&authority.slot).unwrap();
            target.static_stale = true;
            target.metrics.resync_failures = target.metrics.resync_failures.saturating_add(1);
            return Err(TfError::SnapshotRevisionGap);
        }
        let current = state.providers[&authority.slot]
            .static_set
            .as_ref()
            .expect("snapshot checked");
        if let Err(error) = validate_static_chronology(current, &update) {
            mark_static_stale(state.providers.get_mut(&authority.slot).unwrap());
            return Err(error);
        }
        let target = state.providers.get_mut(&authority.slot).unwrap();
        target.highest_observed_revision = Some(
            target
                .highest_observed_revision
                .map_or(update.revision, |revision| revision.max(update.revision)),
        );
        if target.static_stale || update.revision != current_revision.saturating_add(1) {
            target.static_stale = true;
            target.metrics.resync_failures = target.metrics.resync_failures.saturating_add(1);
            return Err(TfError::SnapshotRevisionGap);
        }
        if self.edge_count_replacing_static(&state, authority.slot, update.transforms.len())
            > self.resources.max_total_edges
        {
            return Err(TfError::CapacityExceeded);
        }
        if state.providers[&authority.slot]
            .dynamic
            .keys()
            .any(|child| update.transforms.iter().any(|edge| edge.child == *child))
        {
            return Err(TfError::ConflictingParent);
        }
        self.ensure_acyclic_replacing_static(&state, authority.slot, &update.transforms)?;

        let children: Vec<_> = update.transforms.iter().map(|edge| edge.child).collect();
        let target = state.providers.get_mut(&authority.slot).unwrap();
        target.static_set = Some(update.clone());
        target.accepted_static.insert(update.revision, update);
        trim_revision_history(
            &mut target.accepted_static,
            self.resources.max_pending_static_updates,
        );
        let conflict = self.conflict_in_set(&state, children);
        if let Some(error) = conflict {
            state.metrics.conflict_count = state.metrics.conflict_count.saturating_add(1);
            if matches!(&error, TfError::ConflictingAuthority) {
                state.metrics.authority_conflict_count =
                    state.metrics.authority_conflict_count.saturating_add(1);
            }
            let target = state.providers.get_mut(&authority.slot).unwrap();
            target.metrics.conflict_count = target.metrics.conflict_count.saturating_add(1);
            if matches!(&error, TfError::ConflictingAuthority) {
                target.metrics.authority_conflict_count =
                    target.metrics.authority_conflict_count.saturating_add(1);
            }
            return Err(error);
        }
        Ok(())
    }

    /// Marks a provider's static state stale so its next bounded retry requests a snapshot.
    pub fn mark_snapshot_stale(&self, authority: &Authority) -> TfResult<()> {
        let mut state = self.lock()?;
        self.require_provider(&state, authority)?;
        state
            .providers
            .get_mut(&authority.slot)
            .unwrap()
            .static_stale = true;
        Ok(())
    }

    /// Returns whether snapshot verification is due, throttled by steady time.
    /// Healthy providers are verified before their maximum freshness age.
    pub fn needs_resync(&self, authority: &Authority) -> TfResult<bool> {
        let mut state = self.lock()?;
        self.require_provider(&state, authority)?;
        let now = Instant::now();
        let provider = state.providers.get_mut(&authority.slot).unwrap();
        if provider
            .last_snapshot
            .is_none_or(|received| now.duration_since(received) >= self.resources.snapshot_max_age)
        {
            provider.static_stale = true;
        }
        if provider
            .last_resync
            .max(provider.last_snapshot)
            .is_some_and(|last| now.duration_since(last) < self.resources.resync_interval)
        {
            return Ok(false);
        }
        provider.last_resync = Some(now);
        provider.metrics.resync_attempts = provider.metrics.resync_attempts.saturating_add(1);
        Ok(true)
    }

    /// Looks up `T_target<-source` at an identified timestamp with strict default precision.
    pub fn lookup_at(
        &self,
        target: &str,
        source: &str,
        stamp: Timestamp,
    ) -> TfResult<LookupResult> {
        self.lookup_at_with(target, source, stamp, LookupOptions::default())
    }

    /// Looks up `T_target<-source` using explicit precision and clock mappings.
    pub fn lookup_at_with(
        &self,
        target: &str,
        source: &str,
        stamp: Timestamp,
        options: LookupOptions<'_>,
    ) -> TfResult<LookupResult> {
        let target = FrameId::new(target)?;
        let source = FrameId::new(source)?;
        let mut state = self.lock()?;
        state.metrics.lookup_attempts = state.metrics.lookup_attempts.saturating_add(1);
        for provider in state.providers.values_mut() {
            provider.metrics.lookup_attempts = provider.metrics.lookup_attempts.saturating_add(1);
        }

        let result = self.lookup_locked(&state, target, source, stamp, options);
        match result {
            Ok(result) => {
                state.metrics.lookup_successes = state.metrics.lookup_successes.saturating_add(1);
                for provider in state.providers.values_mut() {
                    provider.metrics.lookup_successes =
                        provider.metrics.lookup_successes.saturating_add(1);
                }
                Ok(result)
            }
            Err(error) => {
                state.metrics.lookup_failures = state.metrics.lookup_failures.saturating_add(1);
                for provider in state.providers.values_mut() {
                    provider.metrics.lookup_failures =
                        provider.metrics.lookup_failures.saturating_add(1);
                }
                match error {
                    TfError::UnknownFrame => {
                        state.metrics.missing_transform_count =
                            state.metrics.missing_transform_count.saturating_add(1);
                        state.metrics.missing_frame_count =
                            state.metrics.missing_frame_count.saturating_add(1);
                        for provider in state.providers.values_mut() {
                            provider.metrics.missing_transform_count =
                                provider.metrics.missing_transform_count.saturating_add(1);
                            provider.metrics.missing_frame_count =
                                provider.metrics.missing_frame_count.saturating_add(1);
                        }
                    }
                    TfError::DisconnectedFrames
                    | TfError::MissingTransform
                    | TfError::InsufficientHistory => {
                        state.metrics.missing_transform_count =
                            state.metrics.missing_transform_count.saturating_add(1);
                        for provider in state.providers.values_mut() {
                            provider.metrics.missing_transform_count =
                                provider.metrics.missing_transform_count.saturating_add(1);
                        }
                    }
                    TfError::PastExtrapolation
                    | TfError::FutureExtrapolation
                    | TfError::InterpolationGapTooLarge => {
                        state.metrics.extrapolation_count =
                            state.metrics.extrapolation_count.saturating_add(1);
                        for provider in state.providers.values_mut() {
                            provider.metrics.extrapolation_count =
                                provider.metrics.extrapolation_count.saturating_add(1);
                        }
                    }
                    TfError::Time(
                        TimeError::ClockDomainMismatch
                        | TimeError::ClockSourceMismatch
                        | TimeError::ClockGenerationMismatch
                        | TimeError::UnknownClockDomain
                        | TimeError::ClockMappingUnavailable
                        | TimeError::ClockMappingExpired
                        | TimeError::ClockMappingInvalid
                        | TimeError::MappingInvalidated,
                    ) => {
                        state.metrics.clock_error_count =
                            state.metrics.clock_error_count.saturating_add(1);
                        state.metrics.clock_mapping_failure_count =
                            state.metrics.clock_mapping_failure_count.saturating_add(1);
                        for provider in state.providers.values_mut() {
                            provider.metrics.clock_error_count =
                                provider.metrics.clock_error_count.saturating_add(1);
                            provider.metrics.clock_mapping_failure_count = provider
                                .metrics
                                .clock_mapping_failure_count
                                .saturating_add(1);
                        }
                    }
                    TfError::Time(_) => {
                        state.metrics.clock_error_count =
                            state.metrics.clock_error_count.saturating_add(1);
                        for provider in state.providers.values_mut() {
                            provider.metrics.clock_error_count =
                                provider.metrics.clock_error_count.saturating_add(1);
                        }
                    }
                    TfError::ConflictingAuthority | TfError::ConflictingParent => {
                        state.metrics.conflict_count =
                            state.metrics.conflict_count.saturating_add(1);
                        if matches!(&error, TfError::ConflictingAuthority) {
                            state.metrics.authority_conflict_count =
                                state.metrics.authority_conflict_count.saturating_add(1);
                        }
                        for provider in state.providers.values_mut() {
                            provider.metrics.conflict_count =
                                provider.metrics.conflict_count.saturating_add(1);
                            if matches!(&error, TfError::ConflictingAuthority) {
                                provider.metrics.authority_conflict_count =
                                    provider.metrics.authority_conflict_count.saturating_add(1);
                            }
                        }
                    }
                    _ => {}
                }
                Err(error)
            }
        }
    }

    /// Returns bounded buffer, provider and graph observations.
    pub fn pressure_snapshot(&self) -> TfResult<TfPressureSnapshot> {
        let state = self.lock()?;
        let now = Instant::now();
        let claims = self.all_claims(&state);
        let mut frames = BTreeSet::new();
        let mut observations = Vec::with_capacity(state.providers.len());
        let mut total_samples = 0_usize;
        let mut static_edges = 0_usize;
        let mut dynamic_edges = 0_usize;
        for provider in state.providers.values() {
            let static_count = provider
                .static_set
                .as_ref()
                .map_or(0, |set| set.transforms.len());
            let dynamic_count = provider.dynamic.len();
            let sample_count: usize = provider
                .dynamic
                .values()
                .map(|history| history.samples.len())
                .sum();
            total_samples += sample_count;
            static_edges += static_count;
            dynamic_edges += dynamic_count;
            if let Some(set) = &provider.static_set {
                for edge in &set.transforms {
                    frames.insert(edge.parent);
                    frames.insert(edge.child);
                }
            }
            for (child, history) in &provider.dynamic {
                frames.insert(*child);
                frames.insert(history.parent);
            }
            let age = provider.last_snapshot.map(|at| elapsed_ns(now, at));
            let static_tracked = provider.static_set.is_some()
                || provider.highest_observed_revision.is_some()
                || !provider.pending.is_empty();
            let snapshot_fresh = if static_tracked {
                Some(age.is_some_and(|age| age < duration_ns(self.resources.snapshot_max_age)))
            } else {
                None
            };
            let dynamic_update_age_ns = provider
                .dynamic
                .values()
                .filter_map(|history| history.last_received_at)
                .map(|received_at| elapsed_ns(now, received_at))
                .min();
            let dynamic_fresh = dynamic_update_age_ns
                .is_some_and(|age| age <= duration_ns(self.resources.max_dynamic_age));
            observations.push(ProviderObservation {
                authority: provider.authority,
                static_revision: provider.static_set.as_ref().map(|set| set.revision),
                highest_observed_revision: provider.highest_observed_revision,
                static_tracked,
                static_stale: static_tracked
                    && (provider.static_stale || snapshot_fresh != Some(true)),
                snapshot_age_ns: age,
                snapshot_fresh,
                dynamic_update_age_ns,
                dynamic_fresh,
                active_static_edges: static_count,
                active_dynamic_edges: dynamic_count,
                stored_samples: sample_count,
                evicted_samples: provider.metrics.evicted_samples,
                buffer_observed_lookup_attempts: provider.metrics.lookup_attempts,
                buffer_observed_lookup_successes: provider.metrics.lookup_successes,
                buffer_observed_lookup_failures: provider.metrics.lookup_failures,
                missing_transform_count: provider.metrics.missing_transform_count,
                missing_frame_count: provider.metrics.missing_frame_count,
                extrapolation_count: provider.metrics.extrapolation_count,
                clock_error_count: provider.metrics.clock_error_count,
                clock_mapping_failure_count: provider.metrics.clock_mapping_failure_count,
                conflict_count: provider.metrics.conflict_count,
                authority_conflict_count: provider.metrics.authority_conflict_count,
                resync_attempts: provider.metrics.resync_attempts,
                resync_failures: provider.metrics.resync_failures,
            });
        }
        let graph = claims
            .iter()
            .map(|claim| {
                let (is_static, revision, epoch, stored_samples) = match claim.kind {
                    ClaimKind::Static { revision, .. } => (true, Some(revision), None, 0),
                    ClaimKind::Dynamic => {
                        let history =
                            &state.providers[&claim.authority.slot].dynamic[&claim.child()];
                        (false, None, Some(history.epoch), history.samples.len())
                    }
                };
                GraphEdgeObservation {
                    parent: claim.parent,
                    child: claim.child(),
                    authority: claim.authority,
                    is_static,
                    revision,
                    transform_epoch: epoch,
                    stored_samples,
                    conflicting_claims: claims
                        .iter()
                        .filter(|other| other.child() == claim.child())
                        .count()
                        .saturating_sub(1),
                }
            })
            .collect();
        let metrics = &state.metrics;
        Ok(TfPressureSnapshot {
            active_providers: state.providers.len(),
            active_frames: frames.len(),
            static_edges,
            dynamic_edges,
            stored_samples: total_samples,
            evicted_samples: metrics.evicted_samples,
            lookup_attempts: metrics.lookup_attempts,
            lookup_successes: metrics.lookup_successes,
            lookup_failures: metrics.lookup_failures,
            missing_transform_count: metrics.missing_transform_count,
            missing_frame_count: metrics.missing_frame_count,
            extrapolation_count: metrics.extrapolation_count,
            clock_error_count: metrics.clock_error_count,
            clock_mapping_failure_count: metrics.clock_mapping_failure_count,
            conflict_count: metrics.conflict_count,
            authority_conflict_count: metrics.authority_conflict_count,
            providers: observations,
            graph,
        })
    }

    fn lock(&self) -> TfResult<MutexGuard<'_, State>> {
        self.state.lock().map_err(|_| TfError::ProviderUnavailable)
    }

    fn require_provider<'a>(
        &self,
        state: &'a State,
        authority: &Authority,
    ) -> TfResult<&'a Provider> {
        state
            .providers
            .get(&authority.slot)
            .filter(|provider| provider.authority == *authority)
            .ok_or(TfError::ProviderUnavailable)
    }

    fn validate_static_set(&self, set: &StaticSet) -> TfResult<()> {
        if set.transforms.len() > self.resources.max_static_transforms_per_provider {
            return Err(TfError::SnapshotTooLarge);
        }
        let mut children = BTreeSet::new();
        let mut parents = Vec::with_capacity(set.transforms.len());
        for edge in &set.transforms {
            if edge.parent == edge.child {
                return Err(TfError::CycleDetected);
            }
            if !children.insert(edge.child) {
                return Err(TfError::ConflictingParent);
            }
            parents.push((edge.child, edge.parent));
        }
        if has_cycle(&parents) {
            return Err(TfError::CycleDetected);
        }
        Ok(())
    }

    fn owner_edge_count(&self, provider: &Provider) -> usize {
        provider
            .static_set
            .as_ref()
            .map_or(0, |set| set.transforms.len())
            + provider.dynamic.len()
    }

    fn edge_count(&self, state: &State) -> usize {
        state
            .providers
            .values()
            .map(|provider| self.owner_edge_count(provider))
            .sum()
    }

    fn edge_count_replacing_static(&self, state: &State, slot: FrameId, new_len: usize) -> usize {
        self.edge_count(state)
            - state.providers[&slot]
                .static_set
                .as_ref()
                .map_or(0, |set| set.transforms.len())
            + new_len
    }

    fn all_claims(&self, state: &State) -> Vec<Claim> {
        let mut claims = Vec::with_capacity(self.edge_count(state));
        for provider in state.providers.values() {
            if let Some(set) = &provider.static_set {
                for edge in &set.transforms {
                    claims.push(Claim {
                        authority: provider.authority,
                        parent: edge.parent,
                        child: edge.child,
                        kind: ClaimKind::Static {
                            edge: *edge,
                            revision: set.revision,
                        },
                    });
                }
            }
            for (child, history) in &provider.dynamic {
                claims.push(Claim {
                    authority: provider.authority,
                    parent: history.parent,
                    child: *child,
                    kind: ClaimKind::Dynamic,
                });
            }
        }
        claims
    }

    fn ensure_acyclic_with_extra(
        &self,
        state: &State,
        parent: FrameId,
        child: FrameId,
    ) -> TfResult<()> {
        let mut edges: Vec<_> = self
            .all_claims(state)
            .iter()
            .map(|claim| (claim.child(), claim.parent))
            .collect();
        edges.push((child, parent));
        if has_cycle(&edges) {
            Err(TfError::CycleDetected)
        } else {
            Ok(())
        }
    }

    fn ensure_acyclic_replacing_static(
        &self,
        state: &State,
        slot: FrameId,
        set: &[StaticEdge],
    ) -> TfResult<()> {
        let mut edges = Vec::with_capacity(self.edge_count(state) + set.len());
        for provider in state.providers.values() {
            if provider.authority.slot == slot {
                for (child, history) in &provider.dynamic {
                    edges.push((*child, history.parent));
                }
                continue;
            }
            if let Some(current) = &provider.static_set {
                edges.extend(
                    current
                        .transforms
                        .iter()
                        .map(|edge| (edge.child, edge.parent)),
                );
            }
            edges.extend(
                provider
                    .dynamic
                    .iter()
                    .map(|(child, history)| (*child, history.parent)),
            );
        }
        edges.extend(set.iter().map(|edge| (edge.child, edge.parent)));
        if has_cycle(&edges) {
            Err(TfError::CycleDetected)
        } else {
            Ok(())
        }
    }

    fn conflict_in_set<I>(&self, state: &State, children: I) -> Option<TfError>
    where
        I: IntoIterator<Item = FrameId>,
    {
        let claims = self.all_claims(state);
        for child in children {
            let same_child: Vec<_> = claims
                .iter()
                .filter(|claim| claim.child() == child)
                .collect();
            if same_child
                .iter()
                .map(|claim| claim.parent)
                .collect::<BTreeSet<_>>()
                .len()
                > 1
            {
                return Some(TfError::ConflictingParent);
            }
            if same_child
                .iter()
                .map(|claim| claim.authority.physical)
                .collect::<BTreeSet<_>>()
                .len()
                > 1
            {
                return Some(TfError::ConflictingAuthority);
            }
        }
        None
    }

    fn prune_history(&self, history: &mut DynamicHistory) -> u64 {
        let original_len = history.samples.len();
        while history.samples.len() > self.resources.max_samples_per_edge {
            history.samples.remove(0);
        }
        if let Some(latest) = history.samples.last() {
            let newest_ns = i128::from(latest.record.stamp.nanoseconds());
            let max_age = self.resources.max_history_duration.as_nanos();
            while history.samples.first().is_some_and(|sample| {
                u128::try_from(newest_ns - i128::from(sample.record.stamp.nanoseconds()))
                    .is_ok_and(|age| age > max_age)
            }) {
                history.samples.remove(0);
            }
        }
        if history.samples.capacity() > history.samples.len().saturating_mul(2) {
            history.samples.shrink_to_fit();
        }
        (original_len - history.samples.len()) as u64
    }

    fn enforce_global_sample_limit(&self, state: &mut State) {
        while state
            .providers
            .values()
            .flat_map(|provider| provider.dynamic.values())
            .map(|history| history.samples.len())
            .sum::<usize>()
            > self.resources.max_total_samples
        {
            let oldest = state
                .providers
                .iter()
                .flat_map(|(slot, provider)| {
                    provider.dynamic.iter().flat_map(move |(child, history)| {
                        history
                            .samples
                            .first()
                            .map(|sample| (*slot, *child, sample.order))
                    })
                })
                .min_by_key(|(_, _, order)| *order);
            let Some((slot, child, _)) = oldest else {
                break;
            };
            if let Some(history) = state
                .providers
                .get_mut(&slot)
                .and_then(|provider| provider.dynamic.get_mut(&child))
                && !history.samples.is_empty()
            {
                history.samples.remove(0);
                if history.samples.capacity() > history.samples.len().saturating_mul(2) {
                    history.samples.shrink_to_fit();
                }
                let provider = state.providers.get_mut(&slot).unwrap();
                provider.metrics.evicted_samples =
                    provider.metrics.evicted_samples.saturating_add(1);
                state.metrics.evicted_samples = state.metrics.evicted_samples.saturating_add(1);
            }
        }
    }

    fn lookup_locked(
        &self,
        state: &State,
        target: FrameId,
        source: FrameId,
        stamp: Timestamp,
        options: LookupOptions<'_>,
    ) -> TfResult<LookupResult> {
        self.validate_uncertainty(options.timestamp_uncertainty_ns)?;
        let claims = self.all_claims(state);
        let known_frames: BTreeSet<_> = claims
            .iter()
            .flat_map(|claim| [claim.parent, claim.child()])
            .collect();
        if !known_frames.contains(&target) || !known_frames.contains(&source) {
            return Err(TfError::UnknownFrame);
        }
        if target == source {
            let precision = self.combined_uncertainty([options.timestamp_uncertainty_ns])?;
            return Ok(LookupResult {
                transform: Transform3::identity(),
                requested_timestamp: stamp,
                timing_uncertainty_ns: precision,
                sources: Vec::new(),
            });
        }

        // Find the topology first, so a conflict or stale edge above the LCA
        // cannot poison a path that does not use that edge.
        let mut source_nodes = BTreeSet::new();
        source_nodes.insert(source);
        let mut source_edges = Vec::<Claim>::new();
        let mut current = source;
        let mut source_blocker = None;
        for _ in 0..=self.resources.max_total_edges {
            let claim = match self.unique_claim(&claims, current) {
                Ok(Some(claim)) => *claim,
                Ok(None) => break,
                Err(error) => {
                    source_blocker = Some(error);
                    break;
                }
            };
            if source_nodes.contains(&claim.parent) {
                return Err(TfError::CycleDetected);
            }
            source_edges.push(claim);
            current = claim.parent;
            source_nodes.insert(current);
        }

        let mut target_edges = Vec::<Claim>::new();
        let mut target_nodes = BTreeSet::new();
        target_nodes.insert(target);
        current = target;
        let mut common = None;
        for _ in 0..=self.resources.max_total_edges {
            if source_nodes.contains(&current) {
                common = Some(current);
                break;
            }
            let claim = match self.unique_claim(&claims, current) {
                Ok(Some(claim)) => *claim,
                Ok(None) => break,
                Err(error) => return Err(error),
            };
            if target_nodes.contains(&claim.parent) {
                return Err(TfError::CycleDetected);
            }
            target_edges.push(claim);
            current = claim.parent;
            target_nodes.insert(current);
        }
        let Some(common) = common else {
            return Err(source_blocker.unwrap_or(TfError::DisconnectedFrames));
        };

        let mut source_transform = Transform3::identity();
        let mut source_sources = Vec::new();
        let mut source_uncertainty = options.timestamp_uncertainty_ns;
        current = source;
        for claim in &source_edges {
            if current == common {
                break;
            }
            let edge = self.resolve_claim(state, claim, stamp, options)?;
            source_transform = edge.transform.compose(source_transform)?;
            source_uncertainty =
                self.combined_uncertainty([source_uncertainty, edge.uncertainty_ns])?;
            source_sources.push(edge.source);
            current = edge.parent;
            if current == common {
                break;
            }
        }
        if current != common {
            return Err(source_blocker.unwrap_or(TfError::DisconnectedFrames));
        }

        let mut target_transform = Transform3::identity();
        let mut target_sources = Vec::new();
        let mut target_uncertainty = options.timestamp_uncertainty_ns;
        current = target;
        for claim in &target_edges {
            if current == common {
                break;
            }
            let edge = self.resolve_claim(state, claim, stamp, options)?;
            // Each stored edge maps child into parent; this accumulates T_target<-current.
            target_transform = target_transform.compose(edge.transform.inverse()?)?;
            target_uncertainty =
                self.combined_uncertainty([target_uncertainty, edge.uncertainty_ns])?;
            target_sources.push(edge.source);
            current = edge.parent;
            if current == common {
                break;
            }
        }
        if current != common {
            return Err(TfError::DisconnectedFrames);
        }
        let transform = target_transform.compose(source_transform)?;
        target_sources.extend(source_sources);
        let precision = self.combined_uncertainty([
            target_uncertainty,
            source_uncertainty,
            options.timestamp_uncertainty_ns,
        ])?;
        self.validate_uncertainty(precision)?;
        Ok(LookupResult {
            transform,
            requested_timestamp: stamp,
            timing_uncertainty_ns: precision,
            sources: target_sources,
        })
    }

    fn unique_claim<'a>(&self, claims: &'a [Claim], child: FrameId) -> TfResult<Option<&'a Claim>> {
        let mut selected: Option<&Claim> = None;
        let mut parent: Option<FrameId> = None;
        let mut physical = BTreeSet::new();
        for claim in claims.iter().filter(|claim| claim.child() == child) {
            if parent.is_some_and(|existing| existing != claim.parent) {
                return Err(TfError::ConflictingParent);
            }
            parent = Some(claim.parent);
            physical.insert(claim.authority.physical);
            selected.get_or_insert(claim);
        }
        if physical.len() > 1 {
            return Err(TfError::ConflictingAuthority);
        }
        Ok(selected)
    }

    fn resolve_claim(
        &self,
        state: &State,
        claim: &Claim,
        query: Timestamp,
        options: LookupOptions<'_>,
    ) -> TfResult<EdgeValue> {
        match claim.kind {
            ClaimKind::Static { edge, revision } => {
                let provider = self.require_provider(state, &claim.authority)?;
                let now = Instant::now();
                if provider.static_stale
                    || provider.last_snapshot.is_none_or(|last| {
                        now.duration_since(last) >= self.resources.snapshot_max_age
                    })
                {
                    return Err(TfError::SnapshotStale);
                }
                let set = provider.static_set.as_ref().ok_or(TfError::SnapshotStale)?;
                let (mapped_query, query_uncertainty) = self.map_query_to_clock(
                    query,
                    options.timestamp_uncertainty_ns,
                    set.effective_from.clock(),
                    options,
                )?;
                if mapped_query.checked_compare(&set.effective_from)? == Ordering::Less {
                    return Err(TfError::PastExtrapolation);
                }
                let precision =
                    self.combined_uncertainty([query_uncertainty, set.effective_uncertainty_ns])?;
                Ok(EdgeValue {
                    parent: edge.parent,
                    transform: edge.transform,
                    uncertainty_ns: precision,
                    source: TransformSource {
                        authority: claim.authority,
                        parent: edge.parent,
                        child: edge.child,
                        timestamp: mapped_query,
                        bracket_start: set.effective_from,
                        bracket_end: set.effective_from,
                        static_revision: Some(revision),
                        transform_epoch: None,
                    },
                })
            }
            ClaimKind::Dynamic => {
                let provider = self.require_provider(state, &claim.authority)?;
                let history = provider
                    .dynamic
                    .get(&claim.child())
                    .ok_or(TfError::MissingTransform)?;
                self.resolve_dynamic(claim, history, query, options)
            }
        }
    }

    fn resolve_dynamic(
        &self,
        claim: &Claim,
        history: &DynamicHistory,
        query: Timestamp,
        options: LookupOptions<'_>,
    ) -> TfResult<EdgeValue> {
        let now = Instant::now();
        let newest = history.samples.last().ok_or(TfError::InsufficientHistory)?;
        if now.duration_since(newest.received_at) > self.resources.max_dynamic_age {
            return Err(TfError::ProviderUnavailable);
        }
        let (mapped_query, query_uncertainty) = self.map_query_to_clock(
            query,
            options.timestamp_uncertainty_ns,
            &history.clock,
            options,
        )?;
        if history
            .boundary
            .is_some_and(|boundary| mapped_query.nanoseconds() < boundary.nanoseconds())
        {
            return Err(TfError::TransformEpochMismatch);
        }
        let position = history
            .samples
            .iter()
            .position(|sample| sample.record.stamp.nanoseconds() == mapped_query.nanoseconds());
        if let Some(index) = position {
            let sample = &history.samples[index];
            let precision =
                self.combined_uncertainty([query_uncertainty, sample.record.uncertainty_ns])?;
            return Ok(EdgeValue {
                parent: claim.parent,
                transform: sample.record.transform,
                uncertainty_ns: precision,
                source: TransformSource {
                    authority: claim.authority,
                    parent: claim.parent,
                    child: claim.child(),
                    timestamp: mapped_query,
                    bracket_start: sample.record.stamp,
                    bracket_end: sample.record.stamp,
                    static_revision: None,
                    transform_epoch: Some(sample.record.transform_epoch),
                },
            });
        }
        let first = history
            .samples
            .first()
            .ok_or(TfError::InsufficientHistory)?;
        let last = history.samples.last().unwrap();
        if mapped_query.nanoseconds() < first.record.stamp.nanoseconds() {
            return Err(TfError::PastExtrapolation);
        }
        if mapped_query.nanoseconds() > last.record.stamp.nanoseconds() {
            return Err(TfError::FutureExtrapolation);
        }
        let (before, after) = history
            .samples
            .windows(2)
            .find(|pair| {
                pair[0].record.stamp.nanoseconds() < mapped_query.nanoseconds()
                    && mapped_query.nanoseconds() < pair[1].record.stamp.nanoseconds()
            })
            .map(|pair| (&pair[0], &pair[1]))
            .ok_or(TfError::InsufficientHistory)?;
        let gap = after
            .record
            .stamp
            .checked_duration_since(&before.record.stamp)?;
        if gap > self.policy.max_interpolation_gap {
            return Err(TfError::InterpolationGapTooLarge);
        }
        let elapsed = mapped_query.checked_difference(&before.record.stamp)?;
        let amount = elapsed as f64 / gap.as_nanos() as f64;
        let transform = before
            .record
            .transform
            .interpolate(after.record.transform, amount)?;
        let precision = self.combined_uncertainty([
            query_uncertainty,
            before.record.uncertainty_ns,
            after.record.uncertainty_ns,
        ])?;
        Ok(EdgeValue {
            parent: claim.parent,
            transform,
            uncertainty_ns: precision,
            source: TransformSource {
                authority: claim.authority,
                parent: claim.parent,
                child: claim.child(),
                timestamp: mapped_query,
                bracket_start: before.record.stamp,
                bracket_end: after.record.stamp,
                static_revision: None,
                transform_epoch: Some(before.record.transform_epoch),
            },
        })
    }

    fn map_query_to_clock(
        &self,
        query: Timestamp,
        uncertainty_ns: Option<u64>,
        target: &ClockIdentity,
        options: LookupOptions<'_>,
    ) -> TfResult<(Timestamp, Option<u64>)> {
        if query.clock() == target {
            return Ok((query, uncertainty_ns));
        }
        let mapping = options
            .mappings
            .iter()
            .copied()
            .find(|mapping| mapping.source() == query.clock() && mapping.target() == target)
            .or_else(|| {
                options.mappings.iter().copied().find(|mapping| {
                    mapping.source().kind() == query.clock().kind()
                        && mapping.source().source_id() == query.clock().source_id()
                        && mapping.target().kind() == target.kind()
                        && mapping.target().source_id() == target.source_id()
                })
            });
        let Some(mapping) = mapping else {
            if query.clock().kind() == target.kind()
                && query.clock().source_id() == target.source_id()
            {
                return Err(TimeError::ClockGenerationMismatch.into());
            }
            return Err(TimeError::ClockMappingUnavailable.into());
        };
        let now = options.now.ok_or(TimeError::ClockMappingUnavailable)?;
        let converted = mapping.convert(&query, uncertainty_ns, target, now)?;
        Ok((converted.timestamp, Some(converted.uncertainty_ns)))
    }

    fn combined_uncertainty<const N: usize>(
        &self,
        values: [Option<u64>; N],
    ) -> TfResult<Option<u64>> {
        // Report the maximum absolute timestamp uncertainty among the query
        // and used samples. This is not a relative alignment or spatial-error
        // bound. Any missing input stays unknown in explicit best effort.
        let Some(maximum) = values.iter().copied().flatten().max() else {
            if self.policy.allow_unknown_uncertainty {
                return Ok(None);
            }
            return Err(TimeError::UnknownUncertainty.into());
        };
        if values.iter().any(Option::is_none) {
            if !self.policy.allow_unknown_uncertainty {
                return Err(TimeError::UnknownUncertainty.into());
            }
            self.validate_uncertainty(Some(maximum))?;
            return Ok(None);
        }
        Ok(Some(maximum))
    }

    fn validate_uncertainty(&self, uncertainty: Option<u64>) -> TfResult<()> {
        if uncertainty.is_none() && !self.policy.allow_unknown_uncertainty {
            return Err(TimeError::UnknownUncertainty.into());
        }
        if let (Some(maximum), Some(actual)) =
            (self.policy.max_timestamp_uncertainty_ns, uncertainty)
            && actual > maximum
        {
            return Err(TfError::TimestampUncertaintyExceeded);
        }
        Ok(())
    }
}

impl Claim {
    fn child(&self) -> FrameId {
        self.child
    }
}

fn validate_static_chronology(previous: &StaticSet, next: &StaticSet) -> TfResult<()> {
    if previous
        .effective_from
        .checked_compare(&next.effective_from)?
        == Ordering::Greater
    {
        return Err(TfError::SnapshotRevisionGap);
    }
    Ok(())
}

fn mark_static_stale(provider: &mut Provider) {
    provider.static_stale = true;
    provider.metrics.resync_failures = provider.metrics.resync_failures.saturating_add(1);
}

fn validate_dynamic_transition(history: &DynamicHistory, record: &DynamicRecord) -> TfResult<()> {
    if record.transform_epoch < history.epoch {
        return Err(TfError::TransformEpochMismatch);
    }
    let old = history.clock;
    let new = *record.stamp.clock();
    if old != new {
        if old.kind() != new.kind() {
            return Err(TimeError::ClockDomainMismatch.into());
        }
        if old.source_id() != new.source_id() {
            return Err(TimeError::ClockSourceMismatch.into());
        }
        if new.generation() < old.generation() {
            return Err(TimeError::ClockGenerationMismatch.into());
        }
    }
    Ok(())
}

fn has_cycle(edges: &[(FrameId, FrameId)]) -> bool {
    let mut parent = BTreeMap::<FrameId, Vec<FrameId>>::new();
    for (child, ancestor) in edges {
        parent.entry(*child).or_default().push(*ancestor);
    }
    let mut colors = BTreeMap::<FrameId, u8>::new();
    let mut stack = Vec::<(FrameId, usize)>::new();
    for start in parent.keys().copied() {
        if colors.get(&start) == Some(&2) {
            continue;
        }
        colors.insert(start, 1);
        stack.push((start, 0));
        while !stack.is_empty() {
            let frame = stack.last().expect("stack checked").0;
            let next_parent = {
                let (node, next_index) = stack.last_mut().expect("stack checked");
                match parent.get(node) {
                    Some(ancestors) if *next_index < ancestors.len() => {
                        let ancestor = ancestors[*next_index];
                        *next_index += 1;
                        Some(ancestor)
                    }
                    _ => None,
                }
            };
            if let Some(ancestor) = next_parent {
                match colors.get(&ancestor).copied() {
                    Some(1) => return true,
                    Some(2) => {}
                    _ => {
                        colors.insert(ancestor, 1);
                        stack.push((ancestor, 0));
                    }
                }
            } else {
                colors.insert(frame, 2);
                stack.pop();
            }
        }
    }
    false
}

fn elapsed_ns(now: Instant, then: Instant) -> u64 {
    u64::try_from(now.duration_since(then).as_nanos()).unwrap_or(u64::MAX)
}

fn duration_ns(duration: Duration) -> u64 {
    u64::try_from(duration.as_nanos()).unwrap_or(u64::MAX)
}

fn copy_bounded(values: &[StaticEdge]) -> Vec<StaticEdge> {
    let mut copy = Vec::with_capacity(values.len());
    copy.extend_from_slice(values);
    copy
}

fn trim_revision_history(history: &mut BTreeMap<u64, StaticSet>, maximum: usize) {
    while history.len() > maximum {
        let Some(oldest) = history.keys().next().copied() else {
            break;
        };
        history.remove(&oldest);
    }
}

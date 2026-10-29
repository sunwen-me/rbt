//! Public value types and bounded resource configuration for the TF buffer.

use std::time::Duration;

use rbt_core::CapabilityInstanceId;
use rbt_time::Timestamp;
use serde::Serialize;

use crate::{FrameId, TfError, TfResult, Transform3};

/// Resource limits for a local transform buffer.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct TfResourceConfig {
    pub max_providers: usize,
    pub max_total_edges: usize,
    pub max_samples_per_edge: usize,
    pub max_total_samples: usize,
    pub max_static_transforms_per_provider: usize,
    pub max_pending_static_updates: usize,
    pub max_history_duration: Duration,
    pub resync_interval: Duration,
    pub snapshot_max_age: Duration,
    pub max_dynamic_age: Duration,
}

impl Default for TfResourceConfig {
    fn default() -> Self {
        Self {
            max_providers: 16,
            max_total_edges: 128,
            max_samples_per_edge: 256,
            max_total_samples: 8192,
            max_static_transforms_per_provider: 64,
            max_pending_static_updates: 8,
            max_history_duration: Duration::from_secs(10),
            resync_interval: Duration::from_secs(1),
            snapshot_max_age: Duration::from_secs(5),
            max_dynamic_age: Duration::from_secs(5),
        }
    }
}

impl TfResourceConfig {
    /// Validates nonzero limits and implementation hard caps.
    pub fn validate(&self) -> TfResult<()> {
        const MAX_PROVIDERS: usize = 256;
        const MAX_EDGES: usize = 4096;
        const MAX_SAMPLES: usize = 1_000_000;
        const MAX_PENDING: usize = 4096;
        const MAX_PENDING_STATIC_RECORDS: usize = 65_536;

        // Cap the largest per-provider collection of staged/accepted full sets.
        let bounded_static_records = self
            .max_providers
            .checked_mul(self.max_pending_static_updates)
            .and_then(|value| value.checked_mul(self.max_static_transforms_per_provider));

        if self.max_providers == 0
            || self.max_providers > MAX_PROVIDERS
            || self.max_total_edges == 0
            || self.max_total_edges > MAX_EDGES
            || self.max_samples_per_edge == 0
            || self.max_samples_per_edge > MAX_SAMPLES
            || self.max_total_samples == 0
            || self.max_total_samples > MAX_SAMPLES
            || self.max_samples_per_edge > self.max_total_samples
            || self.max_static_transforms_per_provider == 0
            || self.max_static_transforms_per_provider > self.max_total_edges
            || self.max_pending_static_updates == 0
            || self.max_pending_static_updates > MAX_PENDING
            || bounded_static_records.is_none_or(|count| count > MAX_PENDING_STATIC_RECORDS)
            || self.max_history_duration.is_zero()
            || self.resync_interval.is_zero()
            || self.snapshot_max_age.is_zero()
            || self.max_dynamic_age.is_zero()
        {
            return Err(TfError::InvalidConfig);
        }
        Ok(())
    }
}

/// Time precision and interpolation rules applied to lookups.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct LookupPolicy {
    pub max_timestamp_uncertainty_ns: Option<u64>,
    pub max_interpolation_gap: Duration,
    pub allow_unknown_uncertainty: bool,
}

impl Default for LookupPolicy {
    fn default() -> Self {
        Self {
            max_timestamp_uncertainty_ns: Some(1_000_000),
            max_interpolation_gap: Duration::from_secs(1),
            allow_unknown_uncertainty: false,
        }
    }
}

impl LookupPolicy {
    pub(crate) fn validate(&self) -> TfResult<()> {
        if self.max_interpolation_gap.is_zero() {
            return Err(TfError::InvalidConfig);
        }
        Ok(())
    }
}

/// A logical provider slot bound to one physical capability instance.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq, Serialize)]
pub struct Authority {
    pub(crate) slot: FrameId,
    pub(crate) physical: CapabilityInstanceId,
}

impl Authority {
    /// Returns the logical slot associated with this binding.
    pub const fn slot(&self) -> FrameId {
        self.slot
    }

    /// Returns the physical capability instance associated with this binding.
    pub const fn physical(&self) -> CapabilityInstanceId {
        self.physical
    }
}

/// One static `T_parent<-child` transform.
#[derive(Clone, Copy, Debug, PartialEq, Serialize)]
pub struct StaticEdge {
    pub parent: FrameId,
    pub child: FrameId,
    pub transform: Transform3,
}

/// One dynamic `T_parent<-child` sample.
#[derive(Clone, Copy, Debug, PartialEq, Serialize)]
pub struct DynamicRecord {
    pub parent: FrameId,
    pub child: FrameId,
    pub stamp: Timestamp,
    pub uncertainty_ns: Option<u64>,
    pub transform_epoch: u64,
    pub transform: Transform3,
}

/// A full static state at one authority-local revision.
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct StaticSet {
    pub revision: u64,
    pub effective_from: Timestamp,
    pub effective_uncertainty_ns: Option<u64>,
    pub transforms: Vec<StaticEdge>,
}

/// Additional data supplied to a lookup that may cross clock domains.
#[derive(Clone, Copy, Debug, Default)]
pub struct LookupOptions<'a> {
    pub timestamp_uncertainty_ns: Option<u64>,
    pub mappings: &'a [&'a rbt_time::ClockMapping],
    pub now: Option<&'a Timestamp>,
}

/// The source data used to resolve one graph edge.
#[derive(Clone, Copy, Debug, PartialEq, Serialize)]
pub struct TransformSource {
    pub authority: Authority,
    pub parent: FrameId,
    pub child: FrameId,
    pub timestamp: Timestamp,
    pub bracket_start: Timestamp,
    pub bracket_end: Timestamp,
    pub static_revision: Option<u64>,
    pub transform_epoch: Option<u64>,
}

/// A transform and the exact temporal provenance used by a lookup.
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct LookupResult {
    pub transform: Transform3,
    pub requested_timestamp: Timestamp,
    pub timing_uncertainty_ns: Option<u64>,
    pub sources: Vec<TransformSource>,
}

impl LookupResult {
    /// Applies this result to a point in the source frame.
    pub fn apply_point(&self, point: crate::Vec3) -> TfResult<crate::Vec3> {
        self.transform.apply_point(point)
    }

    /// Applies this result's rotation to a vector in the source frame.
    pub fn apply_vector(&self, vector: crate::Vec3) -> TfResult<crate::Vec3> {
        self.transform.apply_vector(vector)
    }

    /// Applies this result to a pose in the source frame.
    pub fn apply_pose(&self, pose: crate::Pose3) -> TfResult<crate::Pose3> {
        self.transform.apply_pose(pose)
    }
}

/// A bounded per-provider summary exposed by [`crate::TransformBuffer`].
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct ProviderObservation {
    pub authority: Authority,
    pub static_revision: Option<u64>,
    pub highest_observed_revision: Option<u64>,
    pub static_tracked: bool,
    pub static_stale: bool,
    pub snapshot_age_ns: Option<u64>,
    /// `None` means this provider has no static snapshot state.
    pub snapshot_fresh: Option<bool>,
    pub dynamic_update_age_ns: Option<u64>,
    /// False means no dynamic update is available or the most recent one is stale.
    pub dynamic_fresh: bool,
    pub active_static_edges: usize,
    pub active_dynamic_edges: usize,
    pub stored_samples: usize,
    pub evicted_samples: u64,
    /// Buffer lookups observed since this binding was created, not provider RPCs.
    pub buffer_observed_lookup_attempts: u64,
    pub buffer_observed_lookup_successes: u64,
    pub buffer_observed_lookup_failures: u64,
    pub missing_transform_count: u64,
    pub extrapolation_count: u64,
    pub clock_error_count: u64,
    pub conflict_count: u64,
    pub resync_attempts: u64,
    pub resync_failures: u64,
    /// Lookups that failed because at least one requested frame is unknown.
    pub missing_frame_count: u64,
    /// Clock errors caused by clock identity or mapping resolution failures.
    pub clock_mapping_failure_count: u64,
    /// Conflicts specifically caused by competing physical authorities.
    pub authority_conflict_count: u64,
}

/// One bounded graph-edge summary, including all retained competing claims.
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct GraphEdgeObservation {
    pub parent: FrameId,
    pub child: FrameId,
    pub authority: Authority,
    pub is_static: bool,
    pub revision: Option<u64>,
    pub transform_epoch: Option<u64>,
    pub stored_samples: usize,
    pub conflicting_claims: usize,
}

/// Local lookup pressure and graph state, with entries bounded by configuration.
#[derive(Clone, Debug, PartialEq, Serialize)]
pub struct TfPressureSnapshot {
    pub active_providers: usize,
    pub active_frames: usize,
    pub static_edges: usize,
    pub dynamic_edges: usize,
    pub stored_samples: usize,
    pub evicted_samples: u64,
    pub lookup_attempts: u64,
    pub lookup_successes: u64,
    pub lookup_failures: u64,
    pub missing_transform_count: u64,
    pub extrapolation_count: u64,
    pub clock_error_count: u64,
    pub conflict_count: u64,
    pub providers: Vec<ProviderObservation>,
    pub graph: Vec<GraphEdgeObservation>,
    /// Lookups that failed because at least one requested frame is unknown.
    pub missing_frame_count: u64,
    /// Clock errors caused by clock identity or mapping resolution failures.
    pub clock_mapping_failure_count: u64,
    /// Conflicts specifically caused by competing physical authorities.
    pub authority_conflict_count: u64,
}

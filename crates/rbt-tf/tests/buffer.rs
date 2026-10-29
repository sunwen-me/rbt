use std::{
    sync::{Arc, Barrier},
    thread,
    time::Duration,
};

use rbt_core::CapabilityInstanceId;
use rbt_tf::{
    Authority, DynamicRecord, FrameId, LookupOptions, LookupPolicy, Quaternion, StaticEdge,
    StaticSet, TfError, TfResourceConfig, Transform3, TransformBuffer, Vec3,
};
use rbt_time::{
    ClockIdentity, ClockKind, ClockMapping, ClockMappingCalibration, ClockSourceId, DeviceClock,
    TimeError, Timestamp,
};

fn frame(name: &str) -> FrameId {
    FrameId::new(name).expect("valid test frame")
}

fn device(seed: u8, generation: u64) -> DeviceClock {
    DeviceClock::new([seed; 16], generation).expect("valid test device clock")
}

fn host_identity(seed: u8) -> ClockIdentity {
    let mut bytes = [0_u8; 48];
    bytes[..16].fill(seed);
    ClockIdentity::new(
        ClockKind::HostMonotonic,
        ClockSourceId::from_bytes(bytes).expect("nonzero test source"),
        0,
    )
    .expect("valid host clock")
}

fn translate(x: f64) -> Transform3 {
    Transform3::new(
        Vec3::new(x, 0.0, 0.0).expect("finite translation"),
        Quaternion::identity(),
    )
}

fn authority(buffer: &TransformBuffer, slot: &str) -> Authority {
    buffer
        .bind_provider(slot, CapabilityInstanceId::new())
        .expect("provider binds")
}

fn edge(parent: &str, child: &str, x: f64) -> StaticEdge {
    StaticEdge {
        parent: frame(parent),
        child: frame(child),
        transform: translate(x),
    }
}

fn static_set(revision: u64, effective: Timestamp, transforms: Vec<StaticEdge>) -> StaticSet {
    StaticSet {
        revision,
        effective_from: effective,
        effective_uncertainty_ns: Some(0),
        transforms,
    }
}

fn options<'a>(uncertainty: Option<u64>) -> LookupOptions<'a> {
    LookupOptions {
        timestamp_uncertainty_ns: uncertainty,
        mappings: &[],
        now: None,
    }
}

fn query(
    buffer: &TransformBuffer,
    target: &str,
    source: &str,
    stamp: Timestamp,
) -> rbt_tf::LookupResult {
    buffer
        .lookup_at_with(target, source, stamp, options(Some(0)))
        .expect("lookup succeeds")
}

#[test]
fn chain_direction_composes_and_inverts_parent_from_child_edges() {
    let buffer =
        TransformBuffer::new(TfResourceConfig::default(), LookupPolicy::default()).unwrap();
    let owner = authority(&buffer, "robot");
    let clock = device(1, 0);
    buffer
        .apply_static_snapshot(
            &owner,
            static_set(
                1,
                clock.stamp(0),
                vec![edge("base", "arm", 1.0), edge("arm", "tool", 2.0)],
            ),
        )
        .unwrap();

    let forward = query(&buffer, "base", "tool", clock.stamp(1));
    assert_eq!(forward.apply_point(Vec3::zero()).unwrap().x(), 3.0);
    assert_eq!(forward.sources.len(), 2);

    let reverse = query(&buffer, "tool", "base", clock.stamp(1));
    assert_eq!(reverse.apply_point(Vec3::zero()).unwrap().x(), -3.0);
}

#[test]
fn intermediate_lca_paths_ignore_conflicted_or_stale_ancestors() {
    let buffer =
        TransformBuffer::new(TfResourceConfig::default(), LookupPolicy::default()).unwrap();
    let tree = authority(&buffer, "tree");
    let ancestor_a = authority(&buffer, "ancestor_a");
    let ancestor_b = authority(&buffer, "ancestor_b");
    let clock = device(15, 0);
    buffer
        .apply_static_snapshot(
            &tree,
            static_set(
                1,
                clock.stamp(0),
                vec![
                    edge("base", "arm", 1.0),
                    edge("arm", "tool", 2.0),
                    edge("arm", "camera", 3.0),
                ],
            ),
        )
        .unwrap();
    buffer
        .apply_static_snapshot(
            &ancestor_a,
            static_set(1, clock.stamp(0), vec![edge("world", "base", 4.0)]),
        )
        .unwrap();
    assert_eq!(
        buffer.apply_static_snapshot(
            &ancestor_b,
            static_set(1, clock.stamp(0), vec![edge("other", "base", 5.0)]),
        ),
        Err(TfError::ConflictingParent)
    );
    buffer.mark_snapshot_stale(&ancestor_a).unwrap();

    let frames = ["base", "arm", "tool", "camera"];
    for target in frames {
        for source in frames {
            query(&buffer, target, source, clock.stamp(1));
        }
    }
}

#[test]
fn cycle_is_rejected_atomically_and_conflicting_claims_fail_closed_until_removed() {
    let buffer =
        TransformBuffer::new(TfResourceConfig::default(), LookupPolicy::default()).unwrap();
    let first = authority(&buffer, "provider_a");
    let second = authority(&buffer, "provider_b");
    let clock = device(2, 0);
    buffer
        .apply_static_snapshot(
            &first,
            static_set(1, clock.stamp(0), vec![edge("world", "camera", 1.0)]),
        )
        .unwrap();

    assert_eq!(
        buffer.apply_static_update(
            &first,
            static_set(
                2,
                clock.stamp(0),
                vec![edge("camera", "world", 9.0), edge("world", "camera", 8.0)],
            ),
        ),
        Err(TfError::CycleDetected)
    );
    assert_eq!(
        query(&buffer, "world", "camera", clock.stamp(1))
            .transform
            .translation()
            .x(),
        1.0
    );

    assert_eq!(
        buffer.apply_static_snapshot(
            &second,
            static_set(1, clock.stamp(0), vec![edge("other", "camera", 4.0)])
        ),
        Err(TfError::ConflictingParent)
    );
    assert_eq!(
        buffer.lookup_at_with("world", "camera", clock.stamp(1), options(Some(0))),
        Err(TfError::ConflictingParent)
    );
    buffer.unbind_provider(&second).unwrap();
    assert_eq!(
        query(&buffer, "world", "camera", clock.stamp(1))
            .transform
            .translation()
            .x(),
        1.0
    );

    let third = authority(&buffer, "provider_c");
    assert_eq!(
        buffer.apply_static_snapshot(
            &third,
            static_set(1, clock.stamp(0), vec![edge("world", "camera", 3.0)])
        ),
        Err(TfError::ConflictingAuthority)
    );
    assert_eq!(
        buffer.lookup_at_with("world", "camera", clock.stamp(1), options(Some(0))),
        Err(TfError::ConflictingAuthority)
    );
    buffer.unbind_provider(&third).unwrap();
    assert_eq!(
        query(&buffer, "world", "camera", clock.stamp(1))
            .transform
            .translation()
            .x(),
        1.0
    );
}

#[test]
fn competing_dynamic_authorities_are_retained_as_blocking_claims() {
    let buffer =
        TransformBuffer::new(TfResourceConfig::default(), LookupPolicy::default()).unwrap();
    let first = authority(&buffer, "dynamic_a");
    let second = authority(&buffer, "dynamic_b");
    let clock = device(12, 0);
    let record = |parent: &str, x| DynamicRecord {
        parent: frame(parent),
        child: frame("camera"),
        stamp: clock.stamp(10),
        uncertainty_ns: Some(0),
        transform_epoch: 1,
        transform: translate(x),
    };
    buffer.ingest_dynamic(&first, record("world", 1.0)).unwrap();
    assert_eq!(
        buffer.ingest_dynamic(&second, record("world", 2.0)),
        Err(TfError::ConflictingAuthority)
    );
    assert_eq!(
        buffer.lookup_at_with("world", "camera", clock.stamp(10), options(Some(0))),
        Err(TfError::ConflictingAuthority)
    );
    buffer.unbind_provider(&second).unwrap();
    assert_eq!(
        query(&buffer, "world", "camera", clock.stamp(10))
            .transform
            .translation()
            .x(),
        1.0
    );
}

#[test]
fn missing_frame_count_is_narrower_than_missing_transform_count() {
    let resources = TfResourceConfig {
        max_samples_per_edge: 1,
        max_total_samples: 1,
        ..TfResourceConfig::default()
    };
    let buffer = TransformBuffer::new(resources, LookupPolicy::default()).unwrap();
    let clock = device(41, 0);
    let topology = authority(&buffer, "topology");

    assert_eq!(
        buffer.lookup_at_with("unknown_a", "unknown_b", clock.stamp(0), options(Some(0))),
        Err(TfError::UnknownFrame)
    );

    buffer
        .apply_static_snapshot(
            &topology,
            static_set(
                1,
                clock.stamp(0),
                vec![edge("root_a", "left", 1.0), edge("root_b", "right", 2.0)],
            ),
        )
        .unwrap();
    assert_eq!(
        buffer.lookup_at_with("left", "right", clock.stamp(1), options(Some(0))),
        Err(TfError::DisconnectedFrames)
    );

    let oldest = authority(&buffer, "oldest_history");
    let newest = authority(&buffer, "newest_history");
    for (owner, child, time) in [(&oldest, "old_tool", 10), (&newest, "new_tool", 20)] {
        buffer
            .ingest_dynamic(
                owner,
                DynamicRecord {
                    parent: frame("base"),
                    child: frame(child),
                    stamp: clock.stamp(time),
                    uncertainty_ns: Some(0),
                    transform_epoch: 1,
                    transform: translate(0.0),
                },
            )
            .unwrap();
    }
    assert_eq!(
        buffer.lookup_at_with("base", "old_tool", clock.stamp(10), options(Some(0))),
        Err(TfError::InsufficientHistory)
    );

    let pressure = buffer.pressure_snapshot().unwrap();
    assert_eq!(pressure.lookup_attempts, 3);
    assert_eq!(pressure.missing_transform_count, 3);
    assert_eq!(pressure.missing_frame_count, 1);
    let topology_metrics = pressure
        .providers
        .iter()
        .find(|provider| provider.authority == topology)
        .unwrap();
    assert_eq!(topology_metrics.missing_transform_count, 3);
    assert_eq!(topology_metrics.missing_frame_count, 1);
    assert!(
        pressure
            .providers
            .iter()
            .filter(|provider| provider.authority != topology)
            .all(|provider| provider.missing_transform_count == 1
                && provider.missing_frame_count == 0)
    );
}

#[test]
fn clock_mapping_failures_exclude_other_time_errors() {
    let resources = TfResourceConfig {
        max_history_duration: Duration::MAX,
        ..TfResourceConfig::default()
    };
    let buffer = TransformBuffer::new(resources, LookupPolicy::default()).unwrap();
    let owner = authority(&buffer, "clock_metrics");
    let clock = device(42, 0);
    buffer
        .apply_static_snapshot(
            &owner,
            static_set(1, clock.stamp(0), vec![edge("base", "camera", 1.0)]),
        )
        .unwrap();

    assert_eq!(
        buffer.lookup_at_with("base", "camera", device(43, 0).stamp(10), options(Some(0)),),
        Err(TfError::Time(TimeError::ClockMappingUnavailable))
    );
    assert_eq!(
        buffer.lookup_at_with("base", "camera", clock.stamp(10), options(None)),
        Err(TfError::Time(TimeError::UnknownUncertainty))
    );

    for nanoseconds in [i64::MIN, i64::MAX] {
        buffer
            .ingest_dynamic(
                &owner,
                DynamicRecord {
                    parent: frame("base"),
                    child: frame("moving"),
                    stamp: clock.stamp(nanoseconds),
                    uncertainty_ns: Some(0),
                    transform_epoch: 1,
                    transform: translate(0.0),
                },
            )
            .unwrap();
    }
    assert_eq!(
        buffer.lookup_at_with("base", "moving", clock.stamp(0), options(Some(0))),
        Err(TfError::Time(TimeError::TimestampOverflow))
    );

    let pressure = buffer.pressure_snapshot().unwrap();
    assert_eq!(pressure.clock_error_count, 3);
    assert_eq!(pressure.clock_mapping_failure_count, 1);
    assert_eq!(pressure.providers[0].clock_error_count, 3);
    assert_eq!(pressure.providers[0].clock_mapping_failure_count, 1);
}

#[test]
fn authority_conflicts_are_distinguished_from_parent_conflicts() {
    let clock = device(44, 0);
    let authority_buffer =
        TransformBuffer::new(TfResourceConfig::default(), LookupPolicy::default()).unwrap();
    let first = authority(&authority_buffer, "authority_a");
    let second = authority(&authority_buffer, "authority_b");
    let same_parent = |parent| DynamicRecord {
        parent: frame(parent),
        child: frame("camera"),
        stamp: clock.stamp(10),
        uncertainty_ns: Some(0),
        transform_epoch: 1,
        transform: translate(0.0),
    };
    authority_buffer
        .ingest_dynamic(&first, same_parent("world"))
        .unwrap();
    assert_eq!(
        authority_buffer.ingest_dynamic(&second, same_parent("world")),
        Err(TfError::ConflictingAuthority)
    );
    assert_eq!(
        authority_buffer.lookup_at_with("world", "camera", clock.stamp(10), options(Some(0)),),
        Err(TfError::ConflictingAuthority)
    );
    let pressure = authority_buffer.pressure_snapshot().unwrap();
    assert_eq!(pressure.conflict_count, 2);
    assert_eq!(pressure.authority_conflict_count, 2);
    assert!(pressure.providers.iter().all(|provider| {
        provider.conflict_count == 1 + u64::from(provider.authority == second)
            && provider.authority_conflict_count == 1 + u64::from(provider.authority == second)
    }));

    let parent_buffer =
        TransformBuffer::new(TfResourceConfig::default(), LookupPolicy::default()).unwrap();
    let first_parent = authority(&parent_buffer, "parent_a");
    let second_parent = authority(&parent_buffer, "parent_b");
    let conflicting_parent = |parent| DynamicRecord {
        parent: frame(parent),
        child: frame("camera"),
        stamp: clock.stamp(10),
        uncertainty_ns: Some(0),
        transform_epoch: 1,
        transform: translate(0.0),
    };
    parent_buffer
        .ingest_dynamic(&first_parent, conflicting_parent("world_a"))
        .unwrap();
    assert_eq!(
        parent_buffer.ingest_dynamic(&second_parent, conflicting_parent("world_b")),
        Err(TfError::ConflictingParent)
    );
    assert_eq!(
        parent_buffer.lookup_at_with("world_a", "camera", clock.stamp(10), options(Some(0)),),
        Err(TfError::ConflictingParent)
    );
    let pressure = parent_buffer.pressure_snapshot().unwrap();
    assert_eq!(pressure.conflict_count, 2);
    assert_eq!(pressure.authority_conflict_count, 0);
    assert!(
        pressure
            .providers
            .iter()
            .all(|provider| provider.authority_conflict_count == 0)
    );
}

#[test]
fn diagnostic_counters_reset_on_rebind_and_serialize() {
    let buffer =
        TransformBuffer::new(TfResourceConfig::default(), LookupPolicy::default()).unwrap();
    let owner = authority(&buffer, "rebound_metrics");
    let clock = device(45, 0);
    buffer
        .apply_static_snapshot(
            &owner,
            static_set(1, clock.stamp(0), vec![edge("base", "camera", 1.0)]),
        )
        .unwrap();
    assert_eq!(
        buffer.lookup_at_with("unknown_a", "unknown_b", clock.stamp(1), options(Some(0))),
        Err(TfError::UnknownFrame)
    );
    assert_eq!(
        buffer.lookup_at_with("base", "camera", device(46, 0).stamp(1), options(Some(0)),),
        Err(TfError::Time(TimeError::ClockMappingUnavailable))
    );

    let peer = authority(&buffer, "rebound_metrics_peer");
    assert_eq!(
        buffer.ingest_dynamic(
            &peer,
            DynamicRecord {
                parent: frame("base"),
                child: frame("camera"),
                stamp: clock.stamp(1),
                uncertainty_ns: Some(0),
                transform_epoch: 1,
                transform: translate(2.0),
            },
        ),
        Err(TfError::ConflictingAuthority)
    );
    assert_eq!(
        buffer.lookup_at_with("base", "camera", clock.stamp(1), options(Some(0))),
        Err(TfError::ConflictingAuthority)
    );

    let before = buffer.pressure_snapshot().unwrap();
    let old_provider = before
        .providers
        .iter()
        .find(|provider| provider.authority == owner)
        .unwrap();
    assert_eq!(old_provider.missing_frame_count, 1);
    assert_eq!(old_provider.clock_mapping_failure_count, 1);
    assert_eq!(old_provider.authority_conflict_count, 1);

    let replacement = buffer
        .bind_provider("rebound_metrics", CapabilityInstanceId::new())
        .unwrap();
    let after = buffer.pressure_snapshot().unwrap();
    assert_eq!(after.lookup_attempts, before.lookup_attempts);
    assert_eq!(after.missing_frame_count, before.missing_frame_count);
    assert_eq!(
        after.clock_mapping_failure_count,
        before.clock_mapping_failure_count
    );
    assert_eq!(
        after.authority_conflict_count,
        before.authority_conflict_count
    );
    let rebound = after
        .providers
        .iter()
        .find(|provider| provider.authority == replacement)
        .unwrap();
    assert_eq!(rebound.buffer_observed_lookup_attempts, 0);
    assert_eq!(rebound.missing_frame_count, 0);
    assert_eq!(rebound.clock_mapping_failure_count, 0);
    assert_eq!(rebound.authority_conflict_count, 0);

    let serialized = serde_json::to_value(&after).unwrap();
    assert_eq!(
        serialized["missing_frame_count"].as_u64(),
        Some(after.missing_frame_count)
    );
    assert_eq!(
        serialized["clock_mapping_failure_count"].as_u64(),
        Some(after.clock_mapping_failure_count)
    );
    assert_eq!(
        serialized["authority_conflict_count"].as_u64(),
        Some(after.authority_conflict_count)
    );
    let serialized_rebound = serialized["providers"]
        .as_array()
        .unwrap()
        .iter()
        .find(|provider| provider["authority"]["physical"] == replacement.physical().to_string())
        .unwrap();
    assert_eq!(serialized_rebound["missing_frame_count"].as_u64(), Some(0));
    assert_eq!(
        serialized_rebound["clock_mapping_failure_count"].as_u64(),
        Some(0)
    );
    assert_eq!(
        serialized_rebound["authority_conflict_count"].as_u64(),
        Some(0)
    );
}

#[test]
fn dynamic_interpolation_is_bounded_and_reports_extrapolation() {
    let resources = TfResourceConfig {
        max_history_duration: Duration::from_secs(3),
        ..TfResourceConfig::default()
    };
    let buffer = TransformBuffer::new(resources, LookupPolicy::default()).unwrap();
    let owner = authority(&buffer, "moving");
    let clock = device(3, 0);
    for (time, x) in [(0, 0.0), (1_000_000_000, 10.0)] {
        buffer
            .ingest_dynamic(
                &owner,
                DynamicRecord {
                    parent: frame("base"),
                    child: frame("tool"),
                    stamp: clock.stamp(time),
                    uncertainty_ns: Some(2),
                    transform_epoch: 7,
                    transform: translate(x),
                },
            )
            .unwrap();
    }
    let middle = query(&buffer, "base", "tool", clock.stamp(500_000_000));
    assert!((middle.transform.translation().x() - 5.0).abs() < 1.0e-9);
    assert_eq!(middle.timing_uncertainty_ns, Some(2));
    assert_eq!(
        buffer.lookup_at_with("base", "tool", clock.stamp(-1), options(Some(0))),
        Err(TfError::PastExtrapolation)
    );
    assert_eq!(
        buffer.lookup_at_with("base", "tool", clock.stamp(1_000_000_001), options(Some(0))),
        Err(TfError::FutureExtrapolation)
    );

    let strict_gap = LookupPolicy {
        max_interpolation_gap: Duration::from_millis(100),
        ..LookupPolicy::default()
    };
    let gap_buffer = TransformBuffer::new(TfResourceConfig::default(), strict_gap).unwrap();
    let gap_owner = authority(&gap_buffer, "moving");
    for (time, x) in [(0, 0.0), (1_000_000_000, 10.0)] {
        gap_buffer
            .ingest_dynamic(
                &gap_owner,
                DynamicRecord {
                    parent: frame("base"),
                    child: frame("tool"),
                    stamp: clock.stamp(time),
                    uncertainty_ns: Some(0),
                    transform_epoch: 1,
                    transform: translate(x),
                },
            )
            .unwrap();
    }
    assert_eq!(
        gap_buffer.lookup_at_with("base", "tool", clock.stamp(500_000_000), options(Some(0))),
        Err(TfError::InterpolationGapTooLarge)
    );
}

#[test]
fn dynamic_epoch_boundary_rejects_late_old_epoch_samples_and_queries() {
    let buffer =
        TransformBuffer::new(TfResourceConfig::default(), LookupPolicy::default()).unwrap();
    let owner = authority(&buffer, "moving");
    let clock = device(13, 0);
    for (time, epoch, x) in [(10, 1, 1.0), (20, 2, 2.0)] {
        buffer
            .ingest_dynamic(
                &owner,
                DynamicRecord {
                    parent: frame("base"),
                    child: frame("tool"),
                    stamp: clock.stamp(time),
                    uncertainty_ns: Some(0),
                    transform_epoch: epoch,
                    transform: translate(x),
                },
            )
            .unwrap();
    }
    assert_eq!(
        buffer.lookup_at_with("base", "tool", clock.stamp(10), options(Some(0))),
        Err(TfError::TransformEpochMismatch)
    );
    assert_eq!(
        query(&buffer, "base", "tool", clock.stamp(20))
            .transform
            .translation()
            .x(),
        2.0
    );
    assert_eq!(
        buffer.ingest_dynamic(
            &owner,
            DynamicRecord {
                parent: frame("base"),
                child: frame("tool"),
                stamp: clock.stamp(30),
                uncertainty_ns: Some(0),
                transform_epoch: 1,
                transform: translate(3.0),
            },
        ),
        Err(TfError::TransformEpochMismatch)
    );
}

#[test]
fn precision_clock_identity_and_mapping_expiry_are_explicit() {
    let buffer =
        TransformBuffer::new(TfResourceConfig::default(), LookupPolicy::default()).unwrap();
    let owner = authority(&buffer, "moving");
    let source_clock = device(4, 0);
    let history_clock = host_identity(16);
    for (time, x) in [(0, 0.0), (1000, 10.0)] {
        buffer
            .ingest_dynamic(
                &owner,
                DynamicRecord {
                    parent: frame("base"),
                    child: frame("tool"),
                    stamp: Timestamp::new(history_clock, time),
                    uncertainty_ns: Some(0),
                    transform_epoch: 1,
                    transform: translate(x),
                },
            )
            .unwrap();
    }

    assert_eq!(
        buffer.lookup_at("base", "tool", source_clock.stamp(100)),
        Err(TfError::Time(TimeError::UnknownUncertainty))
    );
    assert_eq!(
        buffer.lookup_at_with("base", "tool", device(5, 0).stamp(100), options(Some(0))),
        Err(TfError::Time(TimeError::ClockMappingUnavailable))
    );
    let host = host_identity(6);
    let mapping = ClockMapping::new(ClockMappingCalibration {
        source: source_clock.identity(),
        target: history_clock,
        source_anchor_ns: 0,
        target_anchor_ns: 0,
        rate_numerator: 1,
        rate_denominator: 1,
        // The query is in range, while both history samples are outside it.
        valid_from_ns: 50,
        valid_until_ns: 150,
        calibration_id: [7; 16],
        calibrated_at: Timestamp::new(host, 10),
        expires_at: Timestamp::new(host, 20),
        uncertainty_ns: Some(3),
    })
    .unwrap();
    let mappings = [&mapping];
    let now = Timestamp::new(host, 15);
    let mapped = buffer
        .lookup_at_with(
            "base",
            "tool",
            source_clock.stamp(100),
            LookupOptions {
                timestamp_uncertainty_ns: Some(0),
                mappings: &mappings,
                now: Some(&now),
            },
        )
        .unwrap();
    assert!((mapped.transform.translation().x() - 1.0).abs() < 1.0e-9);
    assert_eq!(mapped.timing_uncertainty_ns, Some(3));
    assert_eq!(mapped.sources[0].timestamp.clock(), &history_clock);

    assert_eq!(
        buffer.lookup_at_with(
            "base",
            "tool",
            device(4, 1).stamp(100),
            LookupOptions {
                timestamp_uncertainty_ns: Some(0),
                mappings: &mappings,
                now: Some(&now),
            },
        ),
        Err(TfError::Time(TimeError::ClockGenerationMismatch))
    );

    let expired_now = Timestamp::new(host, 20);
    assert_eq!(
        buffer.lookup_at_with(
            "base",
            "tool",
            source_clock.stamp(100),
            LookupOptions {
                timestamp_uncertainty_ns: Some(0),
                mappings: &mappings,
                now: Some(&expired_now),
            },
        ),
        Err(TfError::Time(TimeError::ClockMappingExpired))
    );
}

#[test]
fn snapshot_staging_gap_recovery_and_old_rpc_race_are_atomic() {
    let buffer =
        TransformBuffer::new(TfResourceConfig::default(), LookupPolicy::default()).unwrap();
    let owner = authority(&buffer, "static_provider");
    let clock = device(8, 0);
    let first = static_set(10, clock.stamp(0), vec![edge("base", "tool", 1.0)]);
    let second = static_set(11, clock.stamp(0), vec![edge("base", "tool", 2.0)]);
    buffer.apply_static_update(&owner, second.clone()).unwrap();
    buffer.apply_static_snapshot(&owner, first.clone()).unwrap();
    assert_eq!(
        query(&buffer, "base", "tool", clock.stamp(1))
            .transform
            .translation()
            .x(),
        2.0
    );
    buffer.apply_static_update(&owner, first.clone()).unwrap();

    // A late response from the earlier RPC is ignored and leaves revision 11 usable.
    buffer.apply_static_snapshot(&owner, first).unwrap();
    assert_eq!(
        query(&buffer, "base", "tool", clock.stamp(1))
            .transform
            .translation()
            .x(),
        2.0
    );
    assert_eq!(
        buffer.apply_static_update(
            &owner,
            static_set(10, clock.stamp(0), vec![edge("base", "tool", 99.0)]),
        ),
        Err(TfError::SnapshotRevisionGap)
    );

    assert_eq!(
        buffer.apply_static_update(
            &owner,
            static_set(13, clock.stamp(0), vec![edge("base", "tool", 3.0)])
        ),
        Err(TfError::SnapshotRevisionGap)
    );
    assert_eq!(
        buffer.lookup_at_with("base", "tool", clock.stamp(1), options(Some(0))),
        Err(TfError::SnapshotStale)
    );
    assert_eq!(
        buffer.apply_static_snapshot(
            &owner,
            static_set(12, clock.stamp(0), vec![edge("base", "tool", 9.0)])
        ),
        Err(TfError::SnapshotRevisionGap)
    );
    buffer
        .apply_static_snapshot(
            &owner,
            static_set(13, clock.stamp(0), vec![edge("base", "tool", 3.0)]),
        )
        .unwrap();
    assert_eq!(
        query(&buffer, "base", "tool", clock.stamp(1))
            .transform
            .translation()
            .x(),
        3.0
    );
}

#[test]
fn static_revisions_preserve_effective_time_chronology_per_authority() {
    let buffer =
        TransformBuffer::new(TfResourceConfig::default(), LookupPolicy::default()).unwrap();
    let owner = authority(&buffer, "chronology");
    let clock = host_identity(30);
    buffer
        .apply_static_snapshot(
            &owner,
            static_set(
                1,
                Timestamp::new(clock, 100),
                vec![edge("base", "camera", 1.0)],
            ),
        )
        .unwrap();

    assert_eq!(
        buffer.apply_static_update(
            &owner,
            static_set(
                2,
                Timestamp::new(clock, 99),
                vec![edge("base", "camera", 2.0)]
            ),
        ),
        Err(TfError::SnapshotRevisionGap)
    );
    assert_eq!(
        buffer.lookup_at_with(
            "base",
            "camera",
            Timestamp::new(clock, 101),
            options(Some(0)),
        ),
        Err(TfError::SnapshotStale)
    );
    assert_eq!(
        buffer.pressure_snapshot().unwrap().providers[0].static_revision,
        Some(1)
    );

    // A valid authoritative snapshot recovers the provider; a later backdated
    // snapshot fails closed without replacing the visible revision.
    buffer
        .apply_static_snapshot(
            &owner,
            static_set(
                2,
                Timestamp::new(clock, 110),
                vec![edge("base", "camera", 2.0)],
            ),
        )
        .unwrap();
    assert_eq!(
        buffer.apply_static_snapshot(
            &owner,
            static_set(
                5,
                Timestamp::new(clock, 109),
                vec![edge("base", "camera", 5.0)]
            ),
        ),
        Err(TfError::SnapshotRevisionGap)
    );
    let pressure = buffer.pressure_snapshot().unwrap();
    assert_eq!(pressure.providers[0].static_revision, Some(2));
    assert!(pressure.providers[0].static_stale);
    assert_eq!(
        buffer.lookup_at_with(
            "base",
            "camera",
            Timestamp::new(clock, 111),
            options(Some(0)),
        ),
        Err(TfError::SnapshotStale)
    );
}

#[test]
fn static_effective_time_clock_changes_preserve_clock_errors() {
    let original = device(31, 0);
    let later_generation = device(31, 1);
    let other_domain = host_identity(32);
    for (replacement_clock, expected) in [
        (
            later_generation.identity(),
            TimeError::ClockGenerationMismatch,
        ),
        (other_domain, TimeError::ClockDomainMismatch),
    ] {
        let buffer =
            TransformBuffer::new(TfResourceConfig::default(), LookupPolicy::default()).unwrap();
        let owner = authority(&buffer, "clock_change");
        buffer
            .apply_static_snapshot(
                &owner,
                static_set(1, original.stamp(100), vec![edge("base", "camera", 1.0)]),
            )
            .unwrap();
        assert_eq!(
            buffer.apply_static_update(
                &owner,
                static_set(
                    2,
                    Timestamp::new(replacement_clock, 110),
                    vec![edge("base", "camera", 2.0)],
                ),
            ),
            Err(TfError::Time(expected))
        );
        assert_eq!(
            buffer.lookup_at_with("base", "camera", original.stamp(120), options(Some(0)),),
            Err(TfError::SnapshotStale)
        );
    }
}

#[test]
fn staged_static_updates_validate_effective_time_before_publishing_snapshot() {
    let buffer =
        TransformBuffer::new(TfResourceConfig::default(), LookupPolicy::default()).unwrap();
    let owner = authority(&buffer, "staged_chronology");
    let clock = host_identity(33);
    buffer
        .apply_static_update(
            &owner,
            static_set(
                11,
                Timestamp::new(clock, 90),
                vec![edge("base", "camera", 11.0)],
            ),
        )
        .unwrap();

    assert_eq!(
        buffer.apply_static_snapshot(
            &owner,
            static_set(
                10,
                Timestamp::new(clock, 100),
                vec![edge("base", "camera", 10.0)]
            ),
        ),
        Err(TfError::SnapshotRevisionGap)
    );
    let pressure = buffer.pressure_snapshot().unwrap();
    let provider = &pressure.providers[0];
    assert_eq!(provider.static_revision, None);
    assert!(provider.static_stale);
}

#[test]
fn new_physical_static_authority_can_reset_revision_and_effective_time() {
    let buffer =
        TransformBuffer::new(TfResourceConfig::default(), LookupPolicy::default()).unwrap();
    let old_owner = authority(&buffer, "rebound_calibration");
    let old_clock = host_identity(34);
    buffer
        .apply_static_snapshot(
            &old_owner,
            static_set(
                10,
                Timestamp::new(old_clock, 100),
                vec![edge("base", "camera", 10.0)],
            ),
        )
        .unwrap();

    let new_owner = buffer
        .bind_provider("rebound_calibration", CapabilityInstanceId::new())
        .unwrap();
    let new_clock = device(35, 0);
    buffer
        .apply_static_snapshot(
            &new_owner,
            static_set(1, new_clock.stamp(0), vec![edge("base", "camera", 1.0)]),
        )
        .unwrap();
    assert_eq!(
        query(&buffer, "base", "camera", new_clock.stamp(1))
            .transform
            .translation()
            .x(),
        1.0
    );
    assert_eq!(
        buffer.apply_static_snapshot(
            &old_owner,
            static_set(
                11,
                Timestamp::new(old_clock, 110),
                vec![edge("base", "camera", 11.0)]
            ),
        ),
        Err(TfError::ProviderUnavailable)
    );
}

#[test]
fn unrelated_clock_mapping_target_is_unavailable() {
    let buffer =
        TransformBuffer::new(TfResourceConfig::default(), LookupPolicy::default()).unwrap();
    let owner = authority(&buffer, "mapped_history");
    let query_clock = device(36, 0);
    let edge_clock = device(37, 0);
    let unrelated_target = device(38, 0);
    let calibration_clock = host_identity(39);
    buffer
        .ingest_dynamic(
            &owner,
            DynamicRecord {
                parent: frame("base"),
                child: frame("camera"),
                stamp: edge_clock.stamp(100),
                uncertainty_ns: Some(0),
                transform_epoch: 0,
                transform: translate(1.0),
            },
        )
        .unwrap();
    let mapping = ClockMapping::new(ClockMappingCalibration {
        source: query_clock.identity(),
        target: unrelated_target.identity(),
        source_anchor_ns: 0,
        target_anchor_ns: 0,
        rate_numerator: 1,
        rate_denominator: 1,
        valid_from_ns: 0,
        valid_until_ns: 1000,
        calibration_id: [8; 16],
        calibrated_at: Timestamp::new(calibration_clock, 10),
        expires_at: Timestamp::new(calibration_clock, 20),
        uncertainty_ns: Some(0),
    })
    .unwrap();
    let mappings = [&mapping];
    let now = Timestamp::new(calibration_clock, 15);
    assert_eq!(
        buffer.lookup_at_with(
            "base",
            "camera",
            query_clock.stamp(100),
            LookupOptions {
                timestamp_uncertainty_ns: Some(0),
                mappings: &mappings,
                now: Some(&now),
            },
        ),
        Err(TfError::Time(TimeError::ClockMappingUnavailable))
    );
}

#[test]
fn static_snapshot_age_fails_closed_and_resync_is_throttled() {
    let resources = TfResourceConfig {
        snapshot_max_age: Duration::from_millis(2),
        resync_interval: Duration::from_millis(20),
        ..TfResourceConfig::default()
    };
    let buffer = TransformBuffer::new(resources, LookupPolicy::default()).unwrap();
    let owner = authority(&buffer, "static_provider");
    let clock = device(14, 0);
    let snapshot = static_set(1, clock.stamp(0), vec![edge("base", "tool", 1.0)]);
    buffer
        .apply_static_snapshot(&owner, snapshot.clone())
        .unwrap();
    thread::sleep(Duration::from_millis(5));
    assert_eq!(
        buffer.lookup_at_with("base", "tool", clock.stamp(1), options(Some(0))),
        Err(TfError::SnapshotStale)
    );
    // An explicitly shorter freshness age may expire before the configured
    // minimum verification interval. Stay unavailable rather than retry early.
    assert!(!buffer.needs_resync(&owner).unwrap());
    thread::sleep(Duration::from_millis(20));
    assert!(buffer.needs_resync(&owner).unwrap());
    assert!(!buffer.needs_resync(&owner).unwrap());
    buffer.apply_static_snapshot(&owner, snapshot).unwrap();
    assert_eq!(
        query(&buffer, "base", "tool", clock.stamp(1))
            .transform
            .translation()
            .x(),
        1.0
    );
}

#[test]
fn successful_snapshot_restarts_the_resync_cooldown() {
    let resources = TfResourceConfig {
        resync_interval: Duration::from_millis(10),
        ..TfResourceConfig::default()
    };
    let buffer = TransformBuffer::new(resources, LookupPolicy::default()).unwrap();
    let owner = authority(&buffer, "resync_cooldown");
    let clock = device(41, 0);

    assert!(buffer.needs_resync(&owner).unwrap());
    thread::sleep(Duration::from_millis(12));
    buffer
        .apply_static_snapshot(
            &owner,
            static_set(1, clock.stamp(0), vec![edge("base", "camera", 1.0)]),
        )
        .unwrap();

    assert!(!buffer.needs_resync(&owner).unwrap());
    thread::sleep(Duration::from_millis(12));
    assert!(buffer.needs_resync(&owner).unwrap());
    assert!(!buffer.needs_resync(&owner).unwrap());
}

#[test]
fn activation_replacement_rejects_late_samples_and_bounds_history() {
    let resources = TfResourceConfig {
        max_samples_per_edge: 3,
        max_total_samples: 4,
        ..TfResourceConfig::default()
    };
    let buffer = TransformBuffer::new(resources, LookupPolicy::default()).unwrap();
    let clock = device(9, 0);
    let mut old = authority(&buffer, "moving");
    for time in 0..10 {
        buffer
            .ingest_dynamic(
                &old,
                DynamicRecord {
                    parent: frame("base"),
                    child: frame("tool"),
                    stamp: clock.stamp(time),
                    uncertainty_ns: Some(0),
                    transform_epoch: 1,
                    transform: translate(time as f64),
                },
            )
            .unwrap();
    }
    let snapshot = buffer.pressure_snapshot().unwrap();
    assert_eq!(snapshot.stored_samples, 3);
    assert_eq!(snapshot.evicted_samples, 7);

    for _ in 0..100 {
        let replacement = buffer
            .bind_provider("moving", CapabilityInstanceId::new())
            .unwrap();
        assert_eq!(
            buffer.ingest_dynamic(
                &old,
                DynamicRecord {
                    parent: frame("base"),
                    child: frame("late"),
                    stamp: clock.stamp(100),
                    uncertainty_ns: Some(0),
                    transform_epoch: 2,
                    transform: translate(0.0),
                },
            ),
            Err(TfError::ProviderUnavailable)
        );
        old = replacement;
    }
    assert_eq!(buffer.pressure_snapshot().unwrap().stored_samples, 0);
}

#[test]
fn global_eviction_preserves_dynamic_ordering_and_latest_update_freshness() {
    let resources = TfResourceConfig {
        max_samples_per_edge: 1,
        max_total_samples: 1,
        ..TfResourceConfig::default()
    };
    let buffer = TransformBuffer::new(resources, LookupPolicy::default()).unwrap();
    let owner = authority(&buffer, "moving");
    let clock = device(18, 0);
    let record = |child: &str, time| DynamicRecord {
        parent: frame("base"),
        child: frame(child),
        stamp: clock.stamp(time),
        uncertainty_ns: Some(0),
        transform_epoch: 1,
        transform: translate(time as f64),
    };
    let first = record("tool", 10);
    buffer.ingest_dynamic(&owner, first).unwrap();
    buffer.ingest_dynamic(&owner, record("camera", 20)).unwrap();
    assert_eq!(buffer.pressure_snapshot().unwrap().stored_samples, 1);
    assert_eq!(buffer.ingest_dynamic(&owner, first), Ok(()));
    assert_eq!(
        buffer.ingest_dynamic(&owner, record("tool", 9)),
        Err(TfError::OutOfOrderSample)
    );
    let mut contradictory = first;
    contradictory.transform = translate(99.0);
    assert_eq!(
        buffer.ingest_dynamic(&owner, contradictory),
        Err(TfError::OutOfOrderSample)
    );
    let provider = buffer.pressure_snapshot().unwrap().providers.remove(0);
    assert!(provider.dynamic_update_age_ns.is_some());
    assert!(provider.dynamic_fresh);
}

#[test]
fn periodic_static_verification_is_due_before_a_healthy_view_expires() {
    let resources = TfResourceConfig {
        resync_interval: Duration::from_millis(2),
        ..TfResourceConfig::default()
    };
    let buffer = TransformBuffer::new(resources, LookupPolicy::default()).unwrap();
    let owner = authority(&buffer, "calibration");
    let clock = device(19, 0);
    buffer
        .apply_static_snapshot(
            &owner,
            static_set(1, clock.stamp(0), vec![edge("base", "camera", 1.0)]),
        )
        .unwrap();
    thread::sleep(Duration::from_millis(3));
    assert!(buffer.needs_resync(&owner).unwrap());
    assert!(!buffer.needs_resync(&owner).unwrap());
    assert_eq!(
        query(&buffer, "base", "camera", clock.stamp(1))
            .transform
            .translation()
            .x(),
        1.0
    );
    assert!(!buffer.pressure_snapshot().unwrap().providers[0].static_stale);
}

#[test]
fn concurrent_ingest_and_lookup_share_one_safe_buffer() {
    let buffer = Arc::new(
        TransformBuffer::new(TfResourceConfig::default(), LookupPolicy::default()).unwrap(),
    );
    let owner = authority(&buffer, "moving");
    let clock = device(10, 0);
    let ingest_buffer = Arc::clone(&buffer);
    let ingest_clock = clock.clone();
    let ingest = thread::spawn(move || {
        for time in 0..100 {
            ingest_buffer
                .ingest_dynamic(
                    &owner,
                    DynamicRecord {
                        parent: frame("base"),
                        child: frame("tool"),
                        stamp: ingest_clock.stamp(time),
                        uncertainty_ns: Some(0),
                        transform_epoch: 1,
                        transform: translate(time as f64),
                    },
                )
                .unwrap();
        }
    });
    let mut readers = Vec::new();
    for _ in 0..3 {
        let reader_buffer = Arc::clone(&buffer);
        let reader_clock = clock.clone();
        readers.push(thread::spawn(move || {
            for time in 0..100 {
                let result = reader_buffer.lookup_at_with(
                    "base",
                    "tool",
                    reader_clock.stamp(time),
                    options(Some(0)),
                );
                assert!(
                    result.is_ok()
                        || matches!(
                            result,
                            Err(TfError::UnknownFrame
                                | TfError::PastExtrapolation
                                | TfError::FutureExtrapolation)
                        )
                );
            }
        }));
    }
    ingest.join().unwrap();
    for reader in readers {
        reader.join().unwrap();
    }
    assert_eq!(
        query(&buffer, "base", "tool", clock.stamp(99))
            .transform
            .translation()
            .x(),
        99.0
    );
}

#[test]
fn concurrent_static_and_dynamic_writers_keep_lookup_views_atomic() {
    const WRITES: u64 = 1000;
    const READERS: usize = 3;

    let resources = TfResourceConfig {
        max_samples_per_edge: 2048,
        max_total_samples: 8192,
        max_dynamic_age: Duration::from_secs(60),
        snapshot_max_age: Duration::from_secs(60),
        ..TfResourceConfig::default()
    };
    let buffer = Arc::new(TransformBuffer::new(resources, LookupPolicy::default()).unwrap());
    let static_authority = authority(&buffer, "static_calibration");
    let dynamic_authority = authority(&buffer, "base_motion");
    let clock = device(40, 0);

    buffer
        .apply_static_snapshot(
            &static_authority,
            static_set(
                1,
                clock.stamp(0),
                vec![edge("base", "mount", 1.0), edge("mount", "camera", -1.0)],
            ),
        )
        .unwrap();
    buffer
        .ingest_dynamic(
            &dynamic_authority,
            DynamicRecord {
                parent: frame("odom"),
                child: frame("base"),
                stamp: clock.stamp(0),
                uncertainty_ns: Some(0),
                transform_epoch: 1,
                transform: translate(5.0),
            },
        )
        .unwrap();

    let start = Arc::new(Barrier::new(READERS + 3));
    let mut writers = Vec::new();

    let writer_buffer = Arc::clone(&buffer);
    let writer_start = Arc::clone(&start);
    let writer_clock = clock.clone();
    let writer_authority = static_authority;
    writers.push(thread::spawn(move || {
        writer_start.wait();
        for revision in 2..=WRITES + 1 {
            let offset = revision as f64;
            writer_buffer
                .apply_static_snapshot(
                    &writer_authority,
                    static_set(
                        revision,
                        writer_clock.stamp(0),
                        vec![
                            edge("base", "mount", offset),
                            edge("mount", "camera", -offset),
                        ],
                    ),
                )
                .unwrap();
        }
    }));

    let writer_buffer = Arc::clone(&buffer);
    let writer_start = Arc::clone(&start);
    let writer_clock = clock.clone();
    let writer_authority = dynamic_authority;
    writers.push(thread::spawn(move || {
        writer_start.wait();
        for time in 1..=WRITES {
            writer_buffer
                .ingest_dynamic(
                    &writer_authority,
                    DynamicRecord {
                        parent: frame("odom"),
                        child: frame("base"),
                        stamp: writer_clock.stamp(time as i64),
                        uncertainty_ns: Some(0),
                        transform_epoch: 1,
                        transform: translate(5.0),
                    },
                )
                .unwrap();
        }
    }));

    let mut readers = Vec::new();
    for _ in 0..READERS {
        let reader_buffer = Arc::clone(&buffer);
        let reader_start = Arc::clone(&start);
        let reader_clock = clock.clone();
        readers.push(thread::spawn(move || {
            reader_start.wait();
            for _ in 0..WRITES {
                let result = reader_buffer
                    .lookup_at_with("odom", "camera", reader_clock.stamp(0), options(Some(0)))
                    .expect("every concurrent lookup uses the initialized view");
                assert_eq!(result.transform.translation().x(), 5.0);
                assert_eq!(result.sources.len(), 3);

                let mut static_revisions = Vec::new();
                let mut dynamic_sources = 0;
                for source in &result.sources {
                    if let Some(revision) = source.static_revision {
                        assert_eq!(source.authority, static_authority);
                        static_revisions.push(revision);
                    } else {
                        assert_eq!(source.authority, dynamic_authority);
                        assert_eq!(source.transform_epoch, Some(1));
                        dynamic_sources += 1;
                    }
                }
                assert_eq!(static_revisions.len(), 2);
                assert_eq!(static_revisions[0], static_revisions[1]);
                assert_eq!(dynamic_sources, 1);
            }
        }));
    }

    start.wait();
    for writer in writers {
        writer.join().unwrap();
    }
    for reader in readers {
        reader.join().unwrap();
    }

    let final_result = query(&buffer, "odom", "camera", clock.stamp(0));
    assert_eq!(final_result.transform.translation().x(), 5.0);
    assert_eq!(final_result.sources.len(), 3);
    let pressure = buffer.pressure_snapshot().unwrap();
    let static_provider = pressure
        .providers
        .iter()
        .find(|provider| provider.authority == static_authority)
        .unwrap();
    let dynamic_provider = pressure
        .providers
        .iter()
        .find(|provider| provider.authority == dynamic_authority)
        .unwrap();
    assert_eq!(pressure.active_providers, 2);
    assert_eq!(static_provider.static_revision, Some(WRITES + 1));
    assert!(!static_provider.static_stale);
    assert_eq!(dynamic_provider.stored_samples, WRITES as usize + 1);
    assert!(dynamic_provider.stored_samples <= 2048);
    assert!(pressure.stored_samples <= 8192);
    assert_eq!(pressure.evicted_samples, 0);
}

#[test]
fn resources_bound_pending_static_payload_memory_and_lookup_precision() {
    let resources = TfResourceConfig {
        max_providers: 256,
        max_pending_static_updates: 4096,
        max_static_transforms_per_provider: 4096,
        max_total_edges: 4096,
        ..TfResourceConfig::default()
    };
    assert_eq!(resources.validate(), Err(TfError::InvalidConfig));

    let policy = LookupPolicy {
        max_timestamp_uncertainty_ns: Some(1),
        ..LookupPolicy::default()
    };
    let buffer = TransformBuffer::new(TfResourceConfig::default(), policy).unwrap();
    let owner = authority(&buffer, "precision");
    let clock = device(11, 0);
    buffer
        .apply_static_snapshot(
            &owner,
            static_set(1, clock.stamp(0), vec![edge("base", "tool", 0.0)]),
        )
        .unwrap();
    assert_eq!(
        buffer.lookup_at_with("base", "tool", clock.stamp(1), options(Some(2))),
        Err(TfError::TimestampUncertaintyExceeded)
    );
}

#[test]
fn best_effort_mixed_uncertainty_still_enforces_known_limit() {
    let policy = LookupPolicy {
        max_timestamp_uncertainty_ns: Some(5),
        allow_unknown_uncertainty: true,
        ..LookupPolicy::default()
    };
    let clock = device(42, 0);
    let lookup_with_static_uncertainty = |static_uncertainty_ns| {
        let buffer = TransformBuffer::new(TfResourceConfig::default(), policy.clone()).unwrap();
        let static_authority = authority(&buffer, "static_precision");
        let dynamic_authority = authority(&buffer, "dynamic_precision");
        buffer
            .apply_static_snapshot(
                &static_authority,
                StaticSet {
                    revision: 1,
                    effective_from: clock.stamp(0),
                    effective_uncertainty_ns: Some(static_uncertainty_ns),
                    transforms: vec![edge("world", "base", 2.0)],
                },
            )
            .unwrap();
        buffer
            .ingest_dynamic(
                &dynamic_authority,
                DynamicRecord {
                    parent: frame("base"),
                    child: frame("camera"),
                    stamp: clock.stamp(100),
                    uncertainty_ns: None,
                    transform_epoch: 1,
                    transform: translate(3.0),
                },
            )
            .unwrap();
        buffer.lookup_at_with("world", "camera", clock.stamp(100), options(Some(0)))
    };

    assert_eq!(
        lookup_with_static_uncertainty(10),
        Err(TfError::TimestampUncertaintyExceeded)
    );
    let within_limit = lookup_with_static_uncertainty(4).unwrap();
    assert_eq!(within_limit.transform.translation().x(), 5.0);
    assert_eq!(within_limit.sources.len(), 2);
    assert_eq!(within_limit.timing_uncertainty_ns, None);
}

#[test]
fn maximum_static_chain_cycle_check_is_iterative_and_bounded() {
    let resources = TfResourceConfig {
        max_providers: 1,
        max_total_edges: 4096,
        max_static_transforms_per_provider: 4096,
        ..TfResourceConfig::default()
    };
    let buffer = TransformBuffer::new(resources, LookupPolicy::default()).unwrap();
    let owner = authority(&buffer, "long_chain");
    let clock = device(17, 0);
    let chain: Vec<_> = (0..4096)
        .map(|index| {
            edge(
                &format!("frame_{}", index + 1),
                &format!("frame_{index}"),
                0.0,
            )
        })
        .collect();
    buffer
        .apply_static_snapshot(&owner, static_set(1, clock.stamp(0), chain.clone()))
        .unwrap();

    let mut cyclic = chain;
    cyclic[4095].parent = frame("frame_0");
    assert_eq!(
        buffer.apply_static_snapshot(&owner, static_set(2, clock.stamp(0), cyclic)),
        Err(TfError::CycleDetected)
    );
}

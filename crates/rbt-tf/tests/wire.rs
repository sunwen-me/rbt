use rbt_core::CapabilityInstanceId;
use rbt_tf::{
    DynamicRecord, FrameId, Quaternion, StaticEdge, StaticSet, TfError, Transform3, Vec3,
    decode_dynamic, decode_static_edge, decode_static_snapshot, decode_static_update,
    decode_timestamp, decode_transform, encode_dynamic, encode_static_edge, encode_timestamp,
    encode_transform, static_snapshot_header, static_update_header,
};
use rbt_time::{ClockIdentity, ClockKind, ClockSourceId, TimeError, Timestamp};
use rbt_types::{DynamicTransform, StaticTransform, TfTimestamp};

fn timestamp(kind: ClockKind, generation: u64, nanoseconds: i64) -> Timestamp {
    let mut source_bytes = [0; 48];
    source_bytes[0] = 1;
    if kind == ClockKind::HostMonotonic {
        source_bytes[40..].copy_from_slice(&123_i64.to_le_bytes());
    }
    let source = ClockSourceId::from_bytes(source_bytes).unwrap();
    let clock = ClockIdentity::new(kind, source, generation).unwrap();
    Timestamp::new(clock, nanoseconds)
}

fn transform() -> Transform3 {
    Transform3::new(
        Vec3::new(1.25, -2.5, 3.75).unwrap(),
        Quaternion::new(0.0, 0.0, 0.3826834323650898, 0.9238795325112867).unwrap(),
    )
}

fn edge() -> StaticEdge {
    StaticEdge {
        parent: FrameId::new("world").unwrap(),
        child: FrameId::new("robot/base").unwrap(),
        transform: transform(),
    }
}

fn static_set() -> StaticSet {
    StaticSet {
        revision: 12,
        effective_from: timestamp(ClockKind::SystemRealtime, 9, -120),
        effective_uncertainty_ns: Some(25),
        transforms: vec![edge()],
    }
}

fn dynamic_record() -> DynamicRecord {
    DynamicRecord {
        parent: FrameId::new("world").unwrap(),
        child: FrameId::new("robot/tool").unwrap(),
        stamp: timestamp(ClockKind::HostMonotonic, 5, 88_000),
        uncertainty_ns: Some(7),
        transform_epoch: 42,
        transform: transform(),
    }
}

#[test]
fn timestamp_roundtrips_all_clock_kinds_and_optional_uncertainty() {
    for (kind, wire_kind) in [
        (ClockKind::HostMonotonic, 1),
        (ClockKind::SystemRealtime, 2),
        (ClockKind::Device, 3),
    ] {
        let stamp = timestamp(kind, 17, -123_456);
        let wire = encode_timestamp(&stamp, Some(250));
        assert_eq!(wire.kind, wire_kind);
        assert_eq!(wire.flags, 1);
        assert_eq!(wire.uncertainty_ns, 250);
        assert_eq!(wire.reserved, [0; 6]);
        assert_eq!(decode_timestamp(&wire).unwrap(), (stamp, Some(250)));

        let unknown = encode_timestamp(&stamp, None);
        assert_eq!(unknown.flags, 0);
        assert_eq!(unknown.uncertainty_ns, 0);
        assert_eq!(decode_timestamp(&unknown).unwrap(), (stamp, None));
    }
}

#[test]
fn timestamp_rejects_noncanonical_flags_reserved_and_identity_fields() {
    let mut wire = encode_timestamp(&timestamp(ClockKind::Device, 1, 2), None);

    wire.flags = 2;
    assert_eq!(decode_timestamp(&wire), Err(TfError::InvalidTransform));
    wire.flags = 0;
    wire.reserved[4] = 1;
    assert_eq!(decode_timestamp(&wire), Err(TfError::InvalidTransform));
    wire.reserved = [0; 6];
    wire.uncertainty_ns = 1;
    assert_eq!(decode_timestamp(&wire), Err(TfError::InvalidTransform));
    wire.uncertainty_ns = 0;
    wire.kind = 4;
    assert_eq!(decode_timestamp(&wire), Err(TfError::InvalidTransform));

    wire = encode_timestamp(&timestamp(ClockKind::Device, 1, 2), None);
    wire.source_id = [0; 48];
    assert_eq!(
        decode_timestamp(&wire),
        Err(TfError::Time(TimeError::InvalidClockSourceId))
    );

    wire = encode_timestamp(&timestamp(ClockKind::HostMonotonic, 1, 2), None);
    wire.source_id[40..].copy_from_slice(&(-1_i64).to_le_bytes());
    assert_eq!(
        decode_timestamp(&wire),
        Err(TfError::Time(TimeError::InvalidClockIdentity))
    );
}

#[test]
fn transforms_and_static_edges_validate_wire_fields() {
    let original = transform();
    let wire = encode_transform(original);
    assert_eq!(decode_transform(&wire).unwrap(), original);

    let mut invalid = wire;
    invalid.translation.x = f64::INFINITY;
    assert_eq!(decode_transform(&invalid), Err(TfError::InvalidTransform));
    invalid = wire;
    invalid.rotation = rbt_types::TfQuaternion {
        x: 0.0,
        y: 0.0,
        z: 0.0,
        w: 0.0,
    };
    assert_eq!(decode_transform(&invalid), Err(TfError::InvalidQuaternion));

    let original_edge = edge();
    assert_eq!(
        decode_static_edge(&encode_static_edge(&original_edge)).unwrap(),
        original_edge
    );
    let mut invalid_edge = encode_static_edge(&original_edge);
    invalid_edge.parent.bytes[0] = b'/';
    assert_eq!(
        decode_static_edge(&invalid_edge),
        Err(TfError::InvalidFrame)
    );
    invalid_edge = encode_static_edge(&original_edge);
    invalid_edge.child.bytes[63] = b'x';
    assert_eq!(
        decode_static_edge(&invalid_edge),
        Err(TfError::InvalidFrame)
    );
}

#[test]
fn dynamic_record_roundtrips_and_checks_authority_time_frames_and_epoch() {
    let authority = CapabilityInstanceId::new();
    let record = dynamic_record();
    let wire = encode_dynamic(authority, &record);
    assert_eq!(decode_dynamic(authority, &wire).unwrap(), record);

    let mut invalid = wire;
    invalid.authority = [0; 16];
    assert_eq!(
        decode_dynamic(authority, &invalid),
        Err(TfError::InvalidTransform)
    );
    invalid = wire;
    invalid.authority[0] ^= 0xff;
    assert_eq!(
        decode_dynamic(authority, &invalid),
        Err(TfError::ConflictingAuthority)
    );
    invalid = wire;
    invalid.parent.bytes[0] = 0xff;
    assert_eq!(
        decode_dynamic(authority, &invalid),
        Err(TfError::InvalidFrame)
    );
    invalid = wire;
    invalid.stamp.flags = 0x80;
    assert_eq!(
        decode_dynamic(authority, &invalid),
        Err(TfError::InvalidTransform)
    );
    invalid = wire;
    invalid.transform.rotation.w = f64::NAN;
    assert_eq!(
        decode_dynamic(authority, &invalid),
        Err(TfError::InvalidQuaternion)
    );

    let mut changed_epoch = wire;
    changed_epoch.transform_epoch += 1;
    assert_eq!(
        decode_dynamic(authority, &changed_epoch)
            .unwrap()
            .transform_epoch,
        43
    );
}

#[test]
fn static_snapshot_and_update_headers_roundtrip_complete_sets() {
    let authority = CapabilityInstanceId::new();
    let set = static_set();
    let payload: Vec<StaticTransform> = set.transforms.iter().map(encode_static_edge).collect();

    let snapshot = static_snapshot_header(authority, &set);
    assert_eq!(snapshot.count, payload.len() as u32);
    assert_eq!(
        decode_static_snapshot(authority, &snapshot, &payload, 4).unwrap(),
        set
    );

    let update = static_update_header(authority, &set);
    assert_eq!(update.count, payload.len() as u32);
    assert_eq!(
        decode_static_update(authority, &update, &payload, 4).unwrap(),
        set
    );
}

#[test]
fn static_responses_reject_truncation_oversize_bad_status_and_bad_records() {
    let authority = CapabilityInstanceId::new();
    let set = static_set();
    let payload: Vec<StaticTransform> = set.transforms.iter().map(encode_static_edge).collect();
    let mut header = static_snapshot_header(authority, &set);

    assert_eq!(
        decode_static_snapshot(authority, &header, &payload, 0),
        Err(TfError::SnapshotTooLarge)
    );
    header.count += 1;
    assert_eq!(
        decode_static_snapshot(authority, &header, &payload, 4),
        Err(TfError::InvalidTransform)
    );
    header.count = 0;
    header.status = 1;
    assert_eq!(
        decode_static_snapshot(authority, &header, &[], 4),
        Err(TfError::SnapshotTooLarge)
    );
    header.status = 2;
    assert_eq!(
        decode_static_snapshot(authority, &header, &[], 4),
        Err(TfError::ProviderUnavailable)
    );
    header.status = 9;
    assert_eq!(
        decode_static_snapshot(authority, &header, &[], 4),
        Err(TfError::InvalidTransform)
    );
    header.status = 1;
    header.count = 1;
    assert_eq!(
        decode_static_snapshot(authority, &header, &payload, 4),
        Err(TfError::InvalidTransform)
    );

    header = static_snapshot_header(authority, &set);
    header.authority[0] ^= 1;
    assert_eq!(
        decode_static_snapshot(authority, &header, &payload, 4),
        Err(TfError::ConflictingAuthority)
    );

    header = static_snapshot_header(authority, &set);
    let mut invalid_payload = payload.clone();
    invalid_payload[0].transform.translation.y = f64::INFINITY;
    assert_eq!(
        decode_static_snapshot(authority, &header, &invalid_payload, 4),
        Err(TfError::InvalidTransform)
    );
}

#[test]
fn timestamp_unknown_precision_requires_zero_wire_value() {
    let wire = TfTimestamp {
        kind: 3,
        flags: 0,
        reserved: [0; 6],
        source_id: {
            let mut id = [0; 48];
            id[0] = 1;
            id
        },
        generation: 1,
        nanoseconds: 0,
        uncertainty_ns: 0,
    };
    assert_eq!(decode_timestamp(&wire).unwrap().1, None);

    let mut bad = wire;
    bad.uncertainty_ns = 1;
    assert_eq!(decode_timestamp(&bad), Err(TfError::InvalidTransform));
}

#[test]
fn dynamic_wire_uses_generated_fixed_type() {
    let _: DynamicTransform = encode_dynamic(CapabilityInstanceId::new(), &dynamic_record());
}

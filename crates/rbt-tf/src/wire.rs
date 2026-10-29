//! Checked conversion between local TF values and canonical generated records.

use rbt_core::CapabilityInstanceId;
use rbt_time::{ClockIdentity, ClockKind, ClockSourceId, Timestamp};
use rbt_types::{
    DynamicTransform, StaticHeader, StaticTransform, StaticUpdateHeader, TfQuaternion, TfTimestamp,
    TfTransform3, TfVec3,
};

use crate::{
    FrameId, TfError, TfResult, Transform3,
    model::{DynamicRecord, StaticEdge, StaticSet},
};

const TIMESTAMP_UNCERTAINTY_KNOWN: u8 = 1;
const STATUS_SUCCESS: u32 = 0;
const STATUS_SNAPSHOT_TOO_LARGE: u32 = 1;
const STATUS_PROVIDER_UNAVAILABLE: u32 = 2;

/// Encodes an identified timestamp and its optional precision bound.
pub fn encode_timestamp(timestamp: &Timestamp, uncertainty_ns: Option<u64>) -> TfTimestamp {
    let kind = match timestamp.clock().kind() {
        ClockKind::HostMonotonic => 1,
        ClockKind::SystemRealtime => 2,
        ClockKind::Device => 3,
    };
    TfTimestamp {
        kind,
        flags: u8::from(uncertainty_ns.is_some()) * TIMESTAMP_UNCERTAINTY_KNOWN,
        reserved: [0; 6],
        source_id: *timestamp.clock().source_id().as_bytes(),
        generation: timestamp.clock().generation(),
        nanoseconds: timestamp.nanoseconds(),
        uncertainty_ns: uncertainty_ns.unwrap_or(0),
    }
}

/// Decodes a timestamp, rejecting unknown flags and noncanonical fields.
pub fn decode_timestamp(wire: &TfTimestamp) -> TfResult<(Timestamp, Option<u64>)> {
    if wire.reserved.iter().any(|byte| *byte != 0) || wire.flags & !TIMESTAMP_UNCERTAINTY_KNOWN != 0
    {
        return Err(TfError::InvalidTransform);
    }

    let kind = match wire.kind {
        1 => ClockKind::HostMonotonic,
        2 => ClockKind::SystemRealtime,
        3 => ClockKind::Device,
        _ => return Err(TfError::InvalidTransform),
    };
    let uncertainty_ns = if wire.flags & TIMESTAMP_UNCERTAINTY_KNOWN != 0 {
        Some(wire.uncertainty_ns)
    } else {
        if wire.uncertainty_ns != 0 {
            return Err(TfError::InvalidTransform);
        }
        None
    };
    let source_id = ClockSourceId::from_bytes(wire.source_id)?;
    let clock = ClockIdentity::new(kind, source_id, wire.generation)?;
    Ok((Timestamp::new(clock, wire.nanoseconds), uncertainty_ns))
}

/// Encodes a validated transform in canonical xyzw order.
pub fn encode_transform(transform: Transform3) -> TfTransform3 {
    let (x, y, z) = transform.translation().xyz();
    let (qx, qy, qz, qw) = transform.rotation().xyzw();
    TfTransform3 {
        translation: TfVec3 { x, y, z },
        rotation: TfQuaternion {
            x: qx,
            y: qy,
            z: qz,
            w: qw,
        },
    }
}

/// Decodes and validates a transform record.
pub fn decode_transform(wire: &TfTransform3) -> TfResult<Transform3> {
    let translation = crate::Vec3::new(wire.translation.x, wire.translation.y, wire.translation.z)?;
    let rotation = crate::Quaternion::new(
        wire.rotation.x,
        wire.rotation.y,
        wire.rotation.z,
        wire.rotation.w,
    )?;
    Ok(Transform3::new(translation, rotation))
}

/// Encodes a static edge.
pub fn encode_static_edge(edge: &StaticEdge) -> StaticTransform {
    StaticTransform {
        parent: rbt_types::TfFrameId {
            bytes: edge.parent.to_bytes(),
        },
        child: rbt_types::TfFrameId {
            bytes: edge.child.to_bytes(),
        },
        transform: encode_transform(edge.transform),
    }
}

/// Decodes a static edge and validates both frame IDs and its transform.
pub fn decode_static_edge(wire: &StaticTransform) -> TfResult<StaticEdge> {
    Ok(StaticEdge {
        parent: FrameId::from_bytes(wire.parent.bytes)?,
        child: FrameId::from_bytes(wire.child.bytes)?,
        transform: decode_transform(&wire.transform)?,
    })
}

/// Encodes one dynamic sample together with its physical authority.
pub fn encode_dynamic(authority: CapabilityInstanceId, record: &DynamicRecord) -> DynamicTransform {
    DynamicTransform {
        authority: *authority.as_uuid().as_bytes(),
        parent: rbt_types::TfFrameId {
            bytes: record.parent.to_bytes(),
        },
        child: rbt_types::TfFrameId {
            bytes: record.child.to_bytes(),
        },
        stamp: encode_timestamp(&record.stamp, record.uncertainty_ns),
        transform: encode_transform(record.transform),
        transform_epoch: record.transform_epoch,
    }
}

/// Decodes one dynamic sample after checking its physical authority.
pub fn decode_dynamic(
    expected: CapabilityInstanceId,
    wire: &DynamicTransform,
) -> TfResult<DynamicRecord> {
    validate_authority(expected, &wire.authority)?;
    let (stamp, uncertainty_ns) = decode_timestamp(&wire.stamp)?;
    Ok(DynamicRecord {
        parent: FrameId::from_bytes(wire.parent.bytes)?,
        child: FrameId::from_bytes(wire.child.bytes)?,
        stamp,
        uncertainty_ns,
        transform_epoch: wire.transform_epoch,
        transform: decode_transform(&wire.transform)?,
    })
}

/// Builds a successful static snapshot response header.
pub fn static_snapshot_header(authority: CapabilityInstanceId, set: &StaticSet) -> StaticHeader {
    StaticHeader {
        authority: *authority.as_uuid().as_bytes(),
        revision: set.revision,
        status: STATUS_SUCCESS,
        count: wire_count(set.transforms.len()),
        effective_from: encode_timestamp(&set.effective_from, set.effective_uncertainty_ns),
    }
}

/// Builds a successful static update header.
pub fn static_update_header(
    authority: CapabilityInstanceId,
    set: &StaticSet,
) -> StaticUpdateHeader {
    StaticUpdateHeader {
        authority: *authority.as_uuid().as_bytes(),
        revision: set.revision,
        status: STATUS_SUCCESS,
        count: wire_count(set.transforms.len()),
        effective_from: encode_timestamp(&set.effective_from, set.effective_uncertainty_ns),
    }
}

/// Decodes a full static snapshot, checking authority, status, count and bounds.
pub fn decode_static_snapshot(
    expected: CapabilityInstanceId,
    header: &StaticHeader,
    payload: &[StaticTransform],
    max: usize,
) -> TfResult<StaticSet> {
    decode_static_payload(
        expected,
        StaticPayloadHeader {
            authority: &header.authority,
            revision: header.revision,
            status: header.status,
            count: header.count,
            effective_from: &header.effective_from,
        },
        payload,
        max,
    )
}

/// Decodes one complete static update, checking authority, status, count and bounds.
pub fn decode_static_update(
    expected: CapabilityInstanceId,
    header: &StaticUpdateHeader,
    payload: &[StaticTransform],
    max: usize,
) -> TfResult<StaticSet> {
    decode_static_payload(
        expected,
        StaticPayloadHeader {
            authority: &header.authority,
            revision: header.revision,
            status: header.status,
            count: header.count,
            effective_from: &header.effective_from,
        },
        payload,
        max,
    )
}

struct StaticPayloadHeader<'a> {
    authority: &'a [u8; 16],
    revision: u64,
    status: u32,
    count: u32,
    effective_from: &'a TfTimestamp,
}

fn decode_static_payload(
    expected: CapabilityInstanceId,
    header: StaticPayloadHeader<'_>,
    payload: &[StaticTransform],
    max: usize,
) -> TfResult<StaticSet> {
    validate_authority(expected, header.authority)?;

    if header.status != STATUS_SUCCESS {
        if header.count != 0 || !payload.is_empty() {
            return Err(TfError::InvalidTransform);
        }
        return match header.status {
            STATUS_SNAPSHOT_TOO_LARGE => Err(TfError::SnapshotTooLarge),
            STATUS_PROVIDER_UNAVAILABLE => Err(TfError::ProviderUnavailable),
            _ => Err(TfError::InvalidTransform),
        };
    }

    if payload.len() > max {
        return Err(TfError::SnapshotTooLarge);
    }
    if usize::try_from(header.count).ok() != Some(payload.len()) {
        return Err(TfError::InvalidTransform);
    }
    let (effective_from, effective_uncertainty_ns) = decode_timestamp(header.effective_from)?;

    let mut transforms = Vec::new();
    transforms
        .try_reserve_exact(payload.len())
        .map_err(|_| TfError::CapacityExceeded)?;
    for wire_edge in payload {
        transforms.push(decode_static_edge(wire_edge)?);
    }

    Ok(StaticSet {
        revision: header.revision,
        effective_from,
        effective_uncertainty_ns,
        transforms,
    })
}

fn validate_authority(expected: CapabilityInstanceId, authority: &[u8; 16]) -> TfResult<()> {
    let expected_bytes = expected.as_uuid().as_bytes();
    if expected.as_uuid().is_nil() || authority.iter().all(|byte| *byte == 0) {
        return Err(TfError::InvalidTransform);
    }
    if authority != expected_bytes {
        return Err(TfError::ConflictingAuthority);
    }
    Ok(())
}

fn wire_count(count: usize) -> u32 {
    u32::try_from(count).unwrap_or(u32::MAX)
}

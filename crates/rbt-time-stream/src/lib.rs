//! Safe stamped byte slices with an in-sample capture and receipt timing prefix.

use rbt_time::{ClockIdentity, ClockKind, ClockSourceId, SensorTiming, TimeError, Timestamp};
use rbt_types::{LayoutHash, MessageTypeDescriptor, SliceMessageAbi};
use thiserror::Error;

/// Size in bytes of the versioned timing prefix preceding application bytes.
pub const TIMING_PREFIX_LEN: usize = 192;

/// Complete ABI signature for the StampedBytes slice message.
pub const STAMPED_BYTES_SIGNATURE: &str = "header(sequence:u64@0);element:u8@0;prefix(RBTTIME1@0,flags:u8@8,reserved:zero[7]@9,capture:timestamp72@16,received:timestamp72@88,capture_uncertainty_ns:u64@160,received_uncertainty_ns:u64@168,reserved:zero[16]@176);timestamp72(kind:u8@0,reserved:zero[7]@1,source_id:u8[48]@8,generation:u64@56,nanoseconds:i64@64);application:u8[]@192";

/// Message marker for a sequence header followed by bytes containing one
/// timing prefix and an application payload.
#[derive(Clone, Copy, Debug, Default)]
pub struct StampedBytes;

impl StampedBytes {
    /// Returns the stable descriptor for this slice message ABI.
    pub fn descriptor() -> MessageTypeDescriptor {
        <Self as SliceMessageAbi>::descriptor()
    }

    /// Returns the stable truncated BLAKE3 layout hash.
    pub fn layout_hash() -> LayoutHash {
        Self::descriptor().layout_hash
    }
}

impl SliceMessageAbi for StampedBytes {
    type Header = u64;
    type Element = u8;

    const NAMESPACE: &'static str = "rbt.time";
    const NAME: &'static str = "StampedBytes";
    const ABI_MAJOR: u16 = 1;
    const SIGNATURE: &'static str = STAMPED_BYTES_SIGNATURE;
}

/// Errors returned by the fixed-width timing-prefix codec.
#[derive(Clone, Debug, Error, Eq, PartialEq)]
pub enum TimingPrefixError {
    /// The supplied slice is shorter than the required prefix or timestamp record.
    #[error("encoded value requires at least {expected} bytes, got {actual}")]
    InvalidLength { expected: usize, actual: usize },
    /// The fixed magic bytes do not identify RBTTIME1.
    #[error("timing prefix magic is invalid")]
    InvalidMagic,
    /// The flags contain unknown bits or an invalid capture/uncertainty combination.
    #[error("timing prefix flags are invalid")]
    InvalidFlags,
    /// One of the reserved regions contains a nonzero byte.
    #[error("timing prefix reserved bytes are nonzero")]
    NonzeroReserved,
    /// An absent capture timestamp did not use the all-zero encoding.
    #[error("missing capture timestamp must be encoded as 72 zero bytes")]
    InvalidMissingCapture,
    /// A timestamp kind byte is not defined by this format.
    #[error("timing prefix timestamp kind {0} is invalid")]
    InvalidKind(u8),
    /// A clock source or identity violates the core clock contract.
    #[error(transparent)]
    Time(#[from] TimeError),
}

/// Encodes one timing record into the exact 192-byte prefix buffer.
pub fn encode_timing_into(
    timing: &SensorTiming,
    output: &mut [u8],
) -> Result<(), TimingPrefixError> {
    if output.len() < TIMING_PREFIX_LEN {
        return Err(TimingPrefixError::InvalidLength {
            expected: TIMING_PREFIX_LEN,
            actual: output.len(),
        });
    }

    let output = &mut output[..TIMING_PREFIX_LEN];
    output.fill(0);
    output[..8].copy_from_slice(b"RBTTIME1");
    let mut flags = 0_u8;
    if timing.capture().is_some() {
        flags |= 1;
    }
    if timing.capture_uncertainty_ns().is_some() {
        flags |= 1 << 1;
    }
    if timing.received_uncertainty_ns().is_some() {
        flags |= 1 << 2;
    }
    output[8] = flags;

    if let Some(capture) = timing.capture() {
        encode_timestamp(capture, &mut output[16..88]);
    }
    encode_timestamp(timing.received(), &mut output[88..160]);
    if let Some(value) = timing.capture_uncertainty_ns() {
        output[160..168].copy_from_slice(&value.to_le_bytes());
    }
    if let Some(value) = timing.received_uncertainty_ns() {
        output[168..176].copy_from_slice(&value.to_le_bytes());
    }
    Ok(())
}

/// Decodes the 192-byte timing prefix at the start of a payload.
pub fn decode_timing(input: &[u8]) -> Result<SensorTiming, TimingPrefixError> {
    if input.len() < TIMING_PREFIX_LEN {
        return Err(TimingPrefixError::InvalidLength {
            expected: TIMING_PREFIX_LEN,
            actual: input.len(),
        });
    }
    let input = &input[..TIMING_PREFIX_LEN];
    if &input[..8] != b"RBTTIME1" {
        return Err(TimingPrefixError::InvalidMagic);
    }
    if !all_zero(&input[9..16]) || !all_zero(&input[176..192]) {
        return Err(TimingPrefixError::NonzeroReserved);
    }

    let flags = input[8];
    if flags & !0b111 != 0 || (flags & 0b010 != 0 && flags & 0b001 == 0) {
        return Err(TimingPrefixError::InvalidFlags);
    }
    let has_capture = flags & 0b001 != 0;
    if !has_capture && !all_zero(&input[16..88]) {
        return Err(TimingPrefixError::InvalidMissingCapture);
    }

    let capture_uncertainty = read_optional_u64(input, 0b010, 160, flags)?;
    let received_uncertainty = read_optional_u64(input, 0b100, 168, flags)?;
    let capture = if has_capture {
        Some(decode_timestamp(&input[16..88])?)
    } else {
        None
    };
    let received = decode_timestamp(&input[88..160])?;
    Ok(SensorTiming::new(
        capture,
        received,
        capture_uncertainty,
        received_uncertainty,
    )?)
}

/// Splits a transported payload into its timing record and application bytes.
pub fn split_payload(input: &[u8]) -> Result<(SensorTiming, &[u8]), TimingPrefixError> {
    let (prefix, application) =
        input
            .split_at_checked(TIMING_PREFIX_LEN)
            .ok_or(TimingPrefixError::InvalidLength {
                expected: TIMING_PREFIX_LEN,
                actual: input.len(),
            })?;
    Ok((decode_timing(prefix)?, application))
}

fn read_optional_u64(
    input: &[u8],
    flag: u8,
    offset: usize,
    flags: u8,
) -> Result<Option<u64>, TimingPrefixError> {
    let value = u64::from_le_bytes(input[offset..offset + 8].try_into().expect("fixed range"));
    if flags & flag != 0 {
        Ok(Some(value))
    } else if value == 0 {
        Ok(None)
    } else {
        Err(TimingPrefixError::InvalidFlags)
    }
}

fn encode_timestamp(timestamp: &Timestamp, output: &mut [u8]) {
    output.fill(0);
    output[0] = match timestamp.clock().kind() {
        ClockKind::HostMonotonic => 1,
        ClockKind::SystemRealtime => 2,
        ClockKind::Device => 3,
    };
    output[8..56].copy_from_slice(timestamp.clock().source_id().as_bytes());
    output[56..64].copy_from_slice(&timestamp.clock().generation().to_le_bytes());
    output[64..72].copy_from_slice(&timestamp.nanoseconds().to_le_bytes());
}

fn decode_timestamp(input: &[u8]) -> Result<Timestamp, TimingPrefixError> {
    if input.len() != 72 {
        return Err(TimingPrefixError::InvalidLength {
            expected: 72,
            actual: input.len(),
        });
    }
    if !all_zero(&input[1..8]) {
        return Err(TimingPrefixError::NonzeroReserved);
    }
    let kind = match input[0] {
        1 => ClockKind::HostMonotonic,
        2 => ClockKind::SystemRealtime,
        3 => ClockKind::Device,
        value => return Err(TimingPrefixError::InvalidKind(value)),
    };
    let source_bytes: [u8; 48] = input[8..56].try_into().expect("fixed range");
    let source_id = ClockSourceId::from_bytes(source_bytes)?;
    let generation = u64::from_le_bytes(input[56..64].try_into().expect("fixed range"));
    let nanoseconds = i64::from_le_bytes(input[64..72].try_into().expect("fixed range"));
    let clock = ClockIdentity::new(kind, source_id, generation)?;
    Ok(Timestamp::new(clock, nanoseconds))
}

fn all_zero(bytes: &[u8]) -> bool {
    bytes.iter().all(|byte| *byte == 0)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn monotonic(ns: i64) -> Timestamp {
        let mut source = [0_u8; 48];
        source[..16].copy_from_slice(&[1; 16]);
        source[16..24].copy_from_slice(&2_u64.to_le_bytes());
        source[24..32].copy_from_slice(&3_u64.to_le_bytes());
        source[32..40].copy_from_slice(&4_i64.to_le_bytes());
        source[40..48].copy_from_slice(&5_i64.to_le_bytes());
        Timestamp::new(
            ClockIdentity::new(
                ClockKind::HostMonotonic,
                ClockSourceId::from_bytes(source).unwrap(),
                9,
            )
            .unwrap(),
            ns,
        )
    }

    #[test]
    fn descriptor_has_the_frozen_identity_and_primitive_layout() {
        let descriptor = StampedBytes::descriptor();
        assert_eq!(descriptor.namespace, "rbt.time");
        assert_eq!(descriptor.name, "StampedBytes");
        assert_eq!(descriptor.abi_major, 1);
        assert_eq!(descriptor.header_size, 8);
        assert_eq!(descriptor.header_align, 8);
        assert_eq!(descriptor.element_size, 1);
        assert_eq!(descriptor.element_align, 1);
        assert_eq!(descriptor.layout_hash, StampedBytes::layout_hash());
        assert_eq!(
            descriptor.layout_hash.to_string(),
            "97e9a924992dedfe83196197cccb6b35"
        );
    }

    #[test]
    fn timing_round_trip_preserves_distinct_times_uncertainty_and_payload() {
        let timing =
            SensorTiming::new(Some(monotonic(123)), monotonic(456), Some(7), None).unwrap();
        let mut payload = vec![0_u8; TIMING_PREFIX_LEN + 4];
        payload[TIMING_PREFIX_LEN..].copy_from_slice(b"data");
        encode_timing_into(&timing, &mut payload).unwrap();
        assert_eq!(decode_timing(&payload).unwrap(), timing);
        let (decoded, application) = split_payload(&payload).unwrap();
        assert_eq!(decoded, timing);
        assert_eq!(application, b"data");
    }

    #[test]
    fn missing_capture_and_unknown_uncertainty_are_explicit() {
        let timing = SensorTiming::new(None, monotonic(456), None, None).unwrap();
        let mut prefix = [0_u8; TIMING_PREFIX_LEN];
        encode_timing_into(&timing, &mut prefix).unwrap();
        assert_eq!(prefix[8], 0);
        assert!(prefix[16..88].iter().all(|byte| *byte == 0));
        assert_eq!(decode_timing(&prefix).unwrap(), timing);
    }

    #[test]
    fn realtime_capture_and_device_receipt_keep_their_clock_kinds() {
        let realtime_source = ClockSourceId::from_bytes({
            let mut source = [0_u8; 48];
            source[..16].copy_from_slice(&[8; 16]);
            source
        })
        .unwrap();
        let device_source = ClockSourceId::from_bytes({
            let mut source = [0_u8; 48];
            source[..16].copy_from_slice(&[9; 16]);
            source
        })
        .unwrap();
        let timing = SensorTiming::new(
            Some(Timestamp::new(
                ClockIdentity::new(ClockKind::SystemRealtime, realtime_source, 2).unwrap(),
                -1,
            )),
            Timestamp::new(
                ClockIdentity::new(ClockKind::Device, device_source, 3).unwrap(),
                4,
            ),
            None,
            None,
        )
        .unwrap();
        let mut prefix = [0_u8; TIMING_PREFIX_LEN];
        encode_timing_into(&timing, &mut prefix).unwrap();
        assert_eq!(prefix[16], 2);
        assert_eq!(prefix[88], 3);
        assert_eq!(decode_timing(&prefix).unwrap(), timing);
    }

    #[test]
    fn malformed_magic_flags_reserved_and_absent_capture_are_rejected() {
        let timing = SensorTiming::new(None, monotonic(1), None, None).unwrap();
        let mut prefix = [0_u8; TIMING_PREFIX_LEN];
        encode_timing_into(&timing, &mut prefix).unwrap();

        prefix[0] ^= 1;
        assert_eq!(decode_timing(&prefix), Err(TimingPrefixError::InvalidMagic));
        encode_timing_into(&timing, &mut prefix).unwrap();
        prefix[8] = 0x80;
        assert_eq!(decode_timing(&prefix), Err(TimingPrefixError::InvalidFlags));
        encode_timing_into(&timing, &mut prefix).unwrap();
        prefix[9] = 1;
        assert_eq!(
            decode_timing(&prefix),
            Err(TimingPrefixError::NonzeroReserved)
        );
        encode_timing_into(&timing, &mut prefix).unwrap();
        prefix[16] = 1;
        assert_eq!(
            decode_timing(&prefix),
            Err(TimingPrefixError::InvalidMissingCapture)
        );
    }
}

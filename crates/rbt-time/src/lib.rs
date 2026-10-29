//! Clock identities and timestamps for robot data.
//!
//! A timestamp is meaningful only together with its clock identity. This
//! crate keeps that identity in the value and requires explicit, calibrated
//! mappings before timestamps from different clock domains can be compared.

#![forbid(unsafe_code)]

use std::{
    cmp::Ordering,
    sync::atomic::{AtomicBool, AtomicU64, Ordering as AtomicOrdering},
    time::Duration,
};

use serde::{Deserialize, Deserializer, Serialize, Serializer, de::Visitor};
use thiserror::Error;

const CLOCK_SOURCE_ID_BYTES: usize = 48;
const NANOS_PER_SECOND: i128 = 1_000_000_000;

/// Errors returned by clock, timestamp, and mapping operations.
#[derive(Clone, Debug, Eq, Error, PartialEq)]
pub enum TimeError {
    /// The values use different clock kinds.
    #[error("clock domains do not match")]
    ClockDomainMismatch,
    /// The values use different clock sources.
    #[error("clock sources do not match")]
    ClockSourceMismatch,
    /// The values use different clock generations.
    #[error("clock generations do not match")]
    ClockGenerationMismatch,
    /// A clock's source or epoch is unknown.
    #[error("clock domain is unknown")]
    UnknownClockDomain,
    /// No explicit mapping is available for this conversion.
    #[error("clock mapping is unavailable")]
    ClockMappingUnavailable,
    /// The mapping's calibration validity period has ended.
    #[error("clock mapping has expired")]
    ClockMappingExpired,
    /// Mapping calibration parameters are malformed or inconsistent.
    #[error("clock mapping is invalid")]
    ClockMappingInvalid,
    /// A clock discontinuity invalidated the mapping.
    #[error("clock mapping was invalidated by a clock discontinuity")]
    MappingInvalidated,
    /// A clock reading or converted timestamp is outside the timestamp range.
    #[error("timestamp is outside the supported range")]
    TimestampOutOfRange,
    /// Checked timestamp or uncertainty arithmetic overflowed.
    #[error("timestamp arithmetic overflowed")]
    TimestampOverflow,
    /// A timestamp or timing record violates its structural invariants.
    #[error("timestamp is invalid")]
    InvalidTimestamp,
    /// A clock conversion rate must have positive numerator and denominator.
    #[error("clock conversion rate must be positive")]
    InvalidConversionRate,
    /// A required uncertainty bound is unknown.
    #[error("timestamp uncertainty is unknown")]
    UnknownUncertainty,
    /// The host time namespace changed after the clock was constructed.
    #[error("the host time source was reset or changed")]
    TimeSourceReset,
    /// The operating system could not provide a required clock observation.
    #[error("the clock source is unavailable")]
    ClockUnavailable,
    /// Calibration evidence is absent or does not describe a valid interval.
    #[error("clock calibration evidence is invalid")]
    InvalidCalibrationEvidence,
    /// A duration cannot represent a negative timestamp difference.
    #[error("timestamp difference is negative")]
    NegativeDuration,
    /// A clock source identifier is zero or has an invalid encoding.
    #[error("clock source identifier is invalid")]
    InvalidClockSourceId,
    /// A source identifier does not match the supplied clock kind.
    #[error("clock identity is invalid")]
    InvalidClockIdentity,
}

/// The type of clock that produced a timestamp.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum ClockKind {
    /// Linux monotonic clock in one boot and time namespace.
    HostMonotonic,
    /// Linux realtime clock in one boot.
    SystemRealtime,
    /// A caller-defined device clock.
    Device,
}

/// A fixed-size identity for one clock source.
///
/// Serde represents this value as exactly 96 lowercase hexadecimal digits.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
pub struct ClockSourceId([u8; CLOCK_SOURCE_ID_BYTES]);

impl ClockSourceId {
    /// Constructs an identifier from its canonical 48 bytes.
    pub fn from_bytes(bytes: [u8; CLOCK_SOURCE_ID_BYTES]) -> Result<Self, TimeError> {
        if bytes.iter().all(|byte| *byte == 0) {
            return Err(TimeError::InvalidClockSourceId);
        }
        Ok(Self(bytes))
    }

    /// Constructs a device source identifier from a 16-byte device UUID.
    pub fn from_device_id(device_id: [u8; 16]) -> Result<Self, TimeError> {
        if device_id.iter().all(|byte| *byte == 0) {
            return Err(TimeError::InvalidClockSourceId);
        }
        let mut bytes = [0_u8; CLOCK_SOURCE_ID_BYTES];
        bytes[..16].copy_from_slice(&device_id);
        Ok(Self(bytes))
    }

    /// Constructs a device source identifier from a UUID when the device-id
    /// feature is enabled.
    #[cfg(feature = "device-id")]
    pub fn from_uuid(device_id: uuid::Uuid) -> Result<Self, TimeError> {
        Self::from_device_id(*device_id.as_bytes())
    }

    /// Returns the fixed 48-byte source identifier.
    pub const fn as_bytes(&self) -> &[u8; CLOCK_SOURCE_ID_BYTES] {
        &self.0
    }

    /// Encodes this identifier as exactly 96 lowercase hexadecimal digits.
    pub fn to_hex(self) -> String {
        let mut encoded = String::with_capacity(CLOCK_SOURCE_ID_BYTES * 2);
        for byte in self.0 {
            use std::fmt::Write as _;
            let _ = write!(encoded, "{byte:02x}");
        }
        encoded
    }

    /// Decodes exactly 96 lowercase hexadecimal digits.
    pub fn from_hex(encoded: &str) -> Result<Self, TimeError> {
        if encoded.len() != CLOCK_SOURCE_ID_BYTES * 2 {
            return Err(TimeError::InvalidClockSourceId);
        }
        let mut bytes = [0_u8; CLOCK_SOURCE_ID_BYTES];
        for (index, byte) in bytes.iter_mut().enumerate() {
            let high = decode_lower_hex(encoded.as_bytes()[index * 2])?;
            let low = decode_lower_hex(encoded.as_bytes()[index * 2 + 1])?;
            *byte = high << 4 | low;
        }
        Self::from_bytes(bytes)
    }
}

impl Serialize for ClockSourceId {
    fn serialize<S>(&self, serializer: S) -> Result<S::Ok, S::Error>
    where
        S: Serializer,
    {
        serializer.serialize_str(&(*self).to_hex())
    }
}

impl<'de> Deserialize<'de> for ClockSourceId {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        struct ClockSourceIdVisitor;

        impl Visitor<'_> for ClockSourceIdVisitor {
            type Value = ClockSourceId;

            fn expecting(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
                formatter.write_str("96 lowercase hexadecimal digits")
            }

            fn visit_str<E>(self, value: &str) -> Result<Self::Value, E>
            where
                E: serde::de::Error,
            {
                ClockSourceId::from_hex(value).map_err(E::custom)
            }
        }

        deserializer.deserialize_str(ClockSourceIdVisitor)
    }
}

/// The clock kind, source, and generation that give timestamps meaning.
#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq, Serialize)]
pub struct ClockIdentity {
    kind: ClockKind,
    source_id: ClockSourceId,
    generation: u64,
}

impl ClockIdentity {
    /// Creates a clock identity after validating its source encoding.
    pub fn new(
        kind: ClockKind,
        source_id: ClockSourceId,
        generation: u64,
    ) -> Result<Self, TimeError> {
        validate_source_for_kind(kind, source_id)?;
        Ok(Self {
            kind,
            source_id,
            generation,
        })
    }

    /// Returns this clock's kind.
    pub const fn kind(&self) -> ClockKind {
        self.kind
    }

    /// Returns this clock's source identifier.
    pub const fn source_id(&self) -> ClockSourceId {
        self.source_id
    }

    /// Returns this clock's generation.
    pub const fn generation(&self) -> u64 {
        self.generation
    }

    /// Checks that another identity has the same kind, source, and generation.
    pub fn checked_compatible(&self, other: &Self) -> Result<(), TimeError> {
        ensure_same_clock(self, other)
    }

    fn with_generation(&self, generation: u64) -> Self {
        Self {
            kind: self.kind,
            source_id: self.source_id,
            generation,
        }
    }
}

impl<'de> Deserialize<'de> for ClockIdentity {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        #[derive(Deserialize)]
        struct EncodedIdentity {
            kind: ClockKind,
            source_id: ClockSourceId,
            generation: u64,
        }

        let encoded = EncodedIdentity::deserialize(deserializer)?;
        Self::new(encoded.kind, encoded.source_id, encoded.generation)
            .map_err(serde::de::Error::custom)
    }
}

/// A signed nanosecond reading within one identified clock generation.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub struct Timestamp {
    clock: ClockIdentity,
    nanoseconds: i64,
}

impl Timestamp {
    /// Creates a timestamp in an already validated clock identity.
    pub const fn new(clock: ClockIdentity, nanoseconds: i64) -> Self {
        Self { clock, nanoseconds }
    }

    /// Returns the clock identity associated with this timestamp.
    pub const fn clock(&self) -> &ClockIdentity {
        &self.clock
    }

    /// Returns the signed nanosecond value.
    pub const fn nanoseconds(&self) -> i64 {
        self.nanoseconds
    }

    /// Compares timestamps only when their clock kind, source, and generation
    /// all match.
    pub fn checked_compare(&self, other: &Self) -> Result<Ordering, TimeError> {
        ensure_same_clock(&self.clock, &other.clock)?;
        Ok(self.nanoseconds.cmp(&other.nanoseconds))
    }

    /// Returns self minus other as a signed nanosecond count.
    pub fn checked_difference(&self, other: &Self) -> Result<i64, TimeError> {
        ensure_same_clock(&self.clock, &other.clock)?;
        self.nanoseconds
            .checked_sub(other.nanoseconds)
            .ok_or(TimeError::TimestampOverflow)
    }

    /// Returns the non-negative duration from earlier to this timestamp.
    pub fn checked_duration_since(&self, earlier: &Self) -> Result<Duration, TimeError> {
        let difference = self.checked_difference(earlier)?;
        let nanoseconds = u64::try_from(difference).map_err(|_| TimeError::NegativeDuration)?;
        Ok(Duration::from_nanos(nanoseconds))
    }

    /// Adds a signed nanosecond delta with overflow checking.
    pub fn checked_add_nanoseconds(&self, delta: i64) -> Result<Self, TimeError> {
        let nanoseconds = self
            .nanoseconds
            .checked_add(delta)
            .ok_or(TimeError::TimestampOverflow)?;
        Ok(Self::new(self.clock, nanoseconds))
    }
}

/// Capture and software-receipt timing for one sensor sample.
#[derive(Clone, Copy, Debug, Eq, PartialEq, Serialize)]
pub struct SensorTiming {
    capture: Option<Timestamp>,
    received: Timestamp,
    capture_uncertainty_ns: Option<u64>,
    received_uncertainty_ns: Option<u64>,
}

impl SensorTiming {
    /// Creates a sensor timing record. Missing capture time stays missing.
    pub fn new(
        capture: Option<Timestamp>,
        received: Timestamp,
        capture_uncertainty_ns: Option<u64>,
        received_uncertainty_ns: Option<u64>,
    ) -> Result<Self, TimeError> {
        if capture.is_none() && capture_uncertainty_ns.is_some() {
            return Err(TimeError::InvalidTimestamp);
        }
        Ok(Self {
            capture,
            received,
            capture_uncertainty_ns,
            received_uncertainty_ns,
        })
    }

    /// Returns the capture timestamp when the source supplied one.
    pub const fn capture(&self) -> Option<&Timestamp> {
        self.capture.as_ref()
    }

    /// Returns the software-receipt timestamp.
    pub const fn received(&self) -> &Timestamp {
        &self.received
    }

    /// Returns the known capture uncertainty bound.
    pub const fn capture_uncertainty_ns(&self) -> Option<u64> {
        self.capture_uncertainty_ns
    }

    /// Returns the known software-receipt uncertainty bound.
    pub const fn received_uncertainty_ns(&self) -> Option<u64> {
        self.received_uncertainty_ns
    }
}

impl<'de> Deserialize<'de> for SensorTiming {
    fn deserialize<D>(deserializer: D) -> Result<Self, D::Error>
    where
        D: Deserializer<'de>,
    {
        #[derive(Deserialize)]
        struct EncodedSensorTiming {
            capture: Option<Timestamp>,
            received: Timestamp,
            capture_uncertainty_ns: Option<u64>,
            received_uncertainty_ns: Option<u64>,
        }

        let encoded = EncodedSensorTiming::deserialize(deserializer)?;
        Self::new(
            encoded.capture,
            encoded.received,
            encoded.capture_uncertainty_ns,
            encoded.received_uncertainty_ns,
        )
        .map_err(serde::de::Error::custom)
    }
}

/// An explicit statement about whether a timestamp is available or mapped.
#[derive(Clone, Debug, Eq, PartialEq, Serialize, Deserialize)]
pub enum TimestampState {
    /// A timestamp has a known clock identity.
    Known(Timestamp),
    /// No trustworthy timestamp was supplied.
    Unknown,
    /// A timestamp exists, but no mapping is available to the requested clock.
    Unmapped(Timestamp),
}

impl TimestampState {
    /// Legacy numeric timestamp fields carry no source or epoch evidence.
    pub const fn from_legacy_timestamp_ns(_legacy_value: u64) -> Self {
        Self::Unknown
    }
}

/// A confirmed change to a clock's continuity.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ClockDiscontinuity {
    /// A device reset advanced its generation.
    DeviceReset {
        /// Identity before the reset.
        previous: ClockIdentity,
        /// Identity after the reset.
        current: ClockIdentity,
    },
    /// A confirmed system realtime step advanced its generation.
    RealtimeStep {
        /// Identity before the step.
        previous: ClockIdentity,
        /// Identity after the step.
        current: ClockIdentity,
    },
    /// A caller-confirmed external discontinuity in one clock identity.
    ExternalStep {
        /// Clock identity affected by the external step.
        clock: ClockIdentity,
    },
}

impl ClockDiscontinuity {
    fn affects(&self, identity: &ClockIdentity) -> bool {
        match self {
            Self::DeviceReset { previous, current } | Self::RealtimeStep { previous, current } => {
                identity == previous || identity == current
            }
            Self::ExternalStep { clock } => identity == clock,
        }
    }
}

/// A host clock bound to the current Linux boot and time namespace.
#[derive(Debug)]
pub struct HostClock {
    monotonic_identity: ClockIdentity,
    realtime_source_id: ClockSourceId,
    namespace_device: u64,
    namespace_inode: u64,
    realtime_generation: AtomicU64,
}

impl HostClock {
    /// Captures the current boot identity, time namespace, and monotonic offset.
    ///
    /// Construction fails if Linux cannot provide a known namespace offset
    /// basis; it never guesses a fallback identity.
    pub fn new() -> Result<Self, TimeError> {
        #[cfg(target_os = "linux")]
        {
            let boot_id = read_boot_id()?;
            let (namespace_device, namespace_inode) = current_time_namespace()?;
            let (offset_seconds, offset_nanoseconds) = read_monotonic_namespace_offset()?;
            if current_time_namespace()? != (namespace_device, namespace_inode) {
                return Err(TimeError::TimeSourceReset);
            }

            let mut monotonic_bytes = [0_u8; CLOCK_SOURCE_ID_BYTES];
            monotonic_bytes[..16].copy_from_slice(&boot_id);
            monotonic_bytes[16..24].copy_from_slice(&namespace_device.to_le_bytes());
            monotonic_bytes[24..32].copy_from_slice(&namespace_inode.to_le_bytes());
            monotonic_bytes[32..40].copy_from_slice(&offset_seconds.to_le_bytes());
            monotonic_bytes[40..48].copy_from_slice(&offset_nanoseconds.to_le_bytes());

            let mut realtime_bytes = [0_u8; CLOCK_SOURCE_ID_BYTES];
            realtime_bytes[..16].copy_from_slice(&boot_id);

            Ok(Self {
                monotonic_identity: ClockIdentity::new(
                    ClockKind::HostMonotonic,
                    ClockSourceId::from_bytes(monotonic_bytes)?,
                    0,
                )?,
                realtime_source_id: ClockSourceId::from_bytes(realtime_bytes)?,
                namespace_device,
                namespace_inode,
                realtime_generation: AtomicU64::new(0),
            })
        }
        #[cfg(not(target_os = "linux"))]
        {
            Err(TimeError::ClockUnavailable)
        }
    }

    /// Reads the Linux monotonic clock, checking the time namespace before and
    /// after the clock read.
    pub fn monotonic_now(&self) -> Result<Timestamp, TimeError> {
        #[cfg(target_os = "linux")]
        {
            let expected = (self.namespace_device, self.namespace_inode);
            if current_time_namespace()? != expected {
                return Err(TimeError::TimeSourceReset);
            }
            let reading = rustix::time::clock_gettime(rustix::time::ClockId::Monotonic);
            if current_time_namespace()? != expected {
                return Err(TimeError::TimeSourceReset);
            }
            let nanoseconds = timespec_to_nanoseconds(reading.tv_sec, reading.tv_nsec as i64)?;
            Ok(Timestamp::new(self.monotonic_identity, nanoseconds))
        }
        #[cfg(not(target_os = "linux"))]
        {
            Err(TimeError::ClockUnavailable)
        }
    }

    /// Reads the Linux realtime clock. Realtime steps are not detected
    /// automatically; callers must confirm a discontinuity explicitly.
    pub fn realtime_now(&self) -> Result<Timestamp, TimeError> {
        #[cfg(target_os = "linux")]
        {
            let generation = self.realtime_generation.load(AtomicOrdering::SeqCst);
            let reading = rustix::time::clock_gettime(rustix::time::ClockId::Realtime);
            if self.realtime_generation.load(AtomicOrdering::SeqCst) != generation {
                return Err(TimeError::TimeSourceReset);
            }
            let nanoseconds = timespec_to_nanoseconds(reading.tv_sec, reading.tv_nsec as i64)?;
            Ok(Timestamp::new(
                ClockIdentity {
                    kind: ClockKind::SystemRealtime,
                    source_id: self.realtime_source_id,
                    generation,
                },
                nanoseconds,
            ))
        }
        #[cfg(not(target_os = "linux"))]
        {
            Err(TimeError::ClockUnavailable)
        }
    }

    /// Confirms a realtime discontinuity and advances the realtime generation.
    pub fn confirm_realtime_discontinuity(&self) -> Result<ClockDiscontinuity, TimeError> {
        let previous_generation = self
            .realtime_generation
            .fetch_update(
                AtomicOrdering::SeqCst,
                AtomicOrdering::SeqCst,
                |generation| generation.checked_add(1),
            )
            .map_err(|_| TimeError::TimestampOverflow)?;
        let previous = ClockIdentity {
            kind: ClockKind::SystemRealtime,
            source_id: self.realtime_source_id,
            generation: previous_generation,
        };
        let current = previous.with_generation(previous_generation + 1);
        Ok(ClockDiscontinuity::RealtimeStep { previous, current })
    }

    /// Returns the current realtime clock identity.
    pub fn realtime_identity(&self) -> ClockIdentity {
        ClockIdentity {
            kind: ClockKind::SystemRealtime,
            source_id: self.realtime_source_id,
            generation: self.realtime_generation.load(AtomicOrdering::SeqCst),
        }
    }

    /// Returns the fixed host monotonic clock identity.
    pub fn monotonic_identity(&self) -> &ClockIdentity {
        &self.monotonic_identity
    }
}

/// An explicitly identified device clock whose reset advances its generation.
#[derive(Clone, Debug)]
pub struct DeviceClock {
    source_id: ClockSourceId,
    generation: u64,
}

impl DeviceClock {
    /// Creates a device clock with an explicit UUID-like 16-byte ID and
    /// generation.
    pub fn new(device_id: [u8; 16], generation: u64) -> Result<Self, TimeError> {
        let source_id = ClockSourceId::from_device_id(device_id)?;
        Self::from_source_id(source_id, generation)
    }

    /// Creates a device clock from a previously validated source identifier.
    pub fn from_source_id(source_id: ClockSourceId, generation: u64) -> Result<Self, TimeError> {
        validate_source_for_kind(ClockKind::Device, source_id)?;
        Ok(Self {
            source_id,
            generation,
        })
    }

    /// Creates a device clock from a UUID when the device-id feature is
    /// enabled.
    #[cfg(feature = "device-id")]
    pub fn from_uuid(device_id: uuid::Uuid, generation: u64) -> Result<Self, TimeError> {
        Self::from_source_id(ClockSourceId::from_uuid(device_id)?, generation)
    }

    /// Returns the current clock identity.
    pub fn identity(&self) -> ClockIdentity {
        ClockIdentity {
            kind: ClockKind::Device,
            source_id: self.source_id,
            generation: self.generation,
        }
    }

    /// Stamps a caller-supplied signed nanosecond reading.
    pub fn stamp(&self, nanoseconds: i64) -> Timestamp {
        Timestamp::new(self.identity(), nanoseconds)
    }

    /// Confirms a reset, increments the generation, and returns its event.
    pub fn reset(&mut self) -> Result<ClockDiscontinuity, TimeError> {
        let next_generation = self
            .generation
            .checked_add(1)
            .ok_or(TimeError::TimestampOverflow)?;
        let previous = self.identity();
        self.generation = next_generation;
        let current = self.identity();
        Ok(ClockDiscontinuity::DeviceReset { previous, current })
    }
}

/// Validated parameters for one explicit affine clock mapping.
#[derive(Clone, Copy, Debug, Serialize, Deserialize)]
pub struct ClockMappingCalibration {
    /// Full source clock identity accepted by this mapping.
    pub source: ClockIdentity,
    /// Full target clock identity produced by this mapping.
    pub target: ClockIdentity,
    /// Source timestamp at the calibration anchor.
    pub source_anchor_ns: i64,
    /// Target timestamp at the calibration anchor.
    pub target_anchor_ns: i64,
    /// Positive conversion rate numerator.
    pub rate_numerator: u64,
    /// Positive conversion rate denominator.
    pub rate_denominator: u64,
    /// Inclusive minimum nominal source timestamp accepted by this mapping.
    ///
    /// The interval bounds nominal timestamp values. The uncertainty bound is
    /// supplied separately and must be valid for nominal values in this range.
    pub valid_from_ns: i64,
    /// Inclusive maximum source timestamp accepted by this mapping.
    pub valid_until_ns: i64,
    /// Caller-supplied nonzero calibration evidence token.
    pub calibration_id: [u8; 16],
    /// Host monotonic time at which the calibration became valid.
    pub calibrated_at: Timestamp,
    /// Host monotonic time at which the calibration expires.
    pub expires_at: Timestamp,
    /// Known mapping uncertainty bound in nanoseconds.
    pub uncertainty_ns: Option<u64>,
}

/// A converted timestamp with its conservative uncertainty bound.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ConvertedTimestamp {
    /// Converted timestamp in the requested target clock.
    pub timestamp: Timestamp,
    /// Maximum uncertainty bound in nanoseconds.
    pub uncertainty_ns: u64,
}

/// One immutable affine clock mapping with explicit expiry and invalidation.
#[derive(Debug)]
pub struct ClockMapping {
    source: ClockIdentity,
    target: ClockIdentity,
    source_anchor_ns: i64,
    target_anchor_ns: i64,
    rate_numerator: u64,
    rate_denominator: u64,
    valid_from_ns: i64,
    valid_until_ns: i64,
    calibration_id: [u8; 16],
    calibrated_at: Timestamp,
    expires_at: Timestamp,
    uncertainty_ns: Option<u64>,
    invalidated: AtomicBool,
}

impl ClockMapping {
    /// Creates a mapping only when its clock identities, calibration interval,
    /// rate, evidence token, and uncertainty are valid.
    pub fn new(calibration: ClockMappingCalibration) -> Result<Self, TimeError> {
        if calibration.rate_numerator == 0 || calibration.rate_denominator == 0 {
            return Err(TimeError::InvalidConversionRate);
        }
        if calibration.valid_from_ns > calibration.valid_until_ns {
            return Err(TimeError::ClockMappingInvalid);
        }
        if calibration.source == calibration.target
            && (calibration.source_anchor_ns != calibration.target_anchor_ns
                || calibration.rate_numerator != calibration.rate_denominator)
        {
            return Err(TimeError::ClockMappingInvalid);
        }
        if calibration.calibration_id.iter().all(|byte| *byte == 0) {
            return Err(TimeError::InvalidCalibrationEvidence);
        }
        if calibration.uncertainty_ns.is_none() {
            return Err(TimeError::UnknownUncertainty);
        }
        if calibration.calibrated_at.clock.kind() != ClockKind::HostMonotonic
            || calibration.expires_at.clock.kind() != ClockKind::HostMonotonic
        {
            return Err(TimeError::InvalidCalibrationEvidence);
        }
        ensure_same_clock(
            calibration.calibrated_at.clock(),
            calibration.expires_at.clock(),
        )
        .map_err(|_| TimeError::InvalidCalibrationEvidence)?;
        if calibration.calibrated_at.nanoseconds() >= calibration.expires_at.nanoseconds() {
            return Err(TimeError::InvalidCalibrationEvidence);
        }

        Ok(Self {
            source: calibration.source,
            target: calibration.target,
            source_anchor_ns: calibration.source_anchor_ns,
            target_anchor_ns: calibration.target_anchor_ns,
            rate_numerator: calibration.rate_numerator,
            rate_denominator: calibration.rate_denominator,
            valid_from_ns: calibration.valid_from_ns,
            valid_until_ns: calibration.valid_until_ns,
            calibration_id: calibration.calibration_id,
            calibrated_at: calibration.calibrated_at,
            expires_at: calibration.expires_at,
            uncertainty_ns: calibration.uncertainty_ns,
            invalidated: AtomicBool::new(false),
        })
    }

    /// Returns the source identity accepted by the mapping.
    pub const fn source(&self) -> &ClockIdentity {
        &self.source
    }

    /// Returns the target identity produced by the mapping.
    pub const fn target(&self) -> &ClockIdentity {
        &self.target
    }

    /// Returns the calibration evidence token.
    pub const fn calibration_id(&self) -> &[u8; 16] {
        &self.calibration_id
    }

    /// Returns the mapping's uncertainty bound.
    pub const fn uncertainty_ns(&self) -> Option<u64> {
        self.uncertainty_ns
    }

    /// Marks this mapping invalid when a discontinuity affects either endpoint.
    /// Returns whether this event invalidated the mapping.
    pub fn invalidate(&self, discontinuity: &ClockDiscontinuity) -> bool {
        if discontinuity.affects(&self.source) || discontinuity.affects(&self.target) {
            self.invalidated.store(true, AtomicOrdering::Release);
            true
        } else {
            false
        }
    }

    /// Returns whether a discontinuity has invalidated this mapping.
    pub fn is_invalidated(&self) -> bool {
        self.invalidated.load(AtomicOrdering::Acquire)
    }

    /// Converts one source timestamp using this calibration and requested
    /// target identity.
    pub fn convert(
        &self,
        input: &Timestamp,
        input_uncertainty_ns: Option<u64>,
        requested_target: &ClockIdentity,
        now: &Timestamp,
    ) -> Result<ConvertedTimestamp, TimeError> {
        if self.is_invalidated() {
            return Err(TimeError::MappingInvalidated);
        }
        ensure_same_clock(&self.source, input.clock())?;
        ensure_same_clock(&self.target, requested_target)?;
        ensure_same_clock(self.calibrated_at.clock(), now.clock())?;
        if now.nanoseconds() < self.calibrated_at.nanoseconds() {
            return Err(TimeError::ClockMappingUnavailable);
        }
        if now.nanoseconds() >= self.expires_at.nanoseconds() {
            return Err(TimeError::ClockMappingExpired);
        }
        if input.nanoseconds() < self.valid_from_ns || input.nanoseconds() > self.valid_until_ns {
            return Err(TimeError::TimestampOutOfRange);
        }

        let input_uncertainty_ns = input_uncertainty_ns.ok_or(TimeError::UnknownUncertainty)?;
        let mapping_uncertainty_ns = self.uncertainty_ns.ok_or(TimeError::UnknownUncertainty)?;

        let delta = i128::from(input.nanoseconds())
            .checked_sub(i128::from(self.source_anchor_ns))
            .ok_or(TimeError::TimestampOverflow)?;
        let scaled = delta
            .checked_mul(i128::from(self.rate_numerator))
            .ok_or(TimeError::TimestampOverflow)?;
        let divisor = i128::from(self.rate_denominator);
        let converted_delta = scaled / divisor;
        let has_fractional_rounding = scaled % divisor != 0;
        let converted = i128::from(self.target_anchor_ns)
            .checked_add(converted_delta)
            .ok_or(TimeError::TimestampOverflow)?;
        let converted = i64::try_from(converted).map_err(|_| TimeError::TimestampOverflow)?;

        let scaled_uncertainty = u128::from(input_uncertainty_ns)
            .checked_mul(u128::from(self.rate_numerator))
            .ok_or(TimeError::TimestampOverflow)?;
        let uncertainty_divisor = u128::from(self.rate_denominator);
        let uncertainty_quotient = scaled_uncertainty / uncertainty_divisor;
        let uncertainty_remainder = scaled_uncertainty % uncertainty_divisor;
        let mut converted_uncertainty =
            u64::try_from(uncertainty_quotient).map_err(|_| TimeError::TimestampOverflow)?;
        if uncertainty_remainder != 0 {
            converted_uncertainty = converted_uncertainty
                .checked_add(1)
                .ok_or(TimeError::TimestampOverflow)?;
        }
        converted_uncertainty = converted_uncertainty
            .checked_add(mapping_uncertainty_ns)
            .ok_or(TimeError::TimestampOverflow)?;
        if has_fractional_rounding {
            converted_uncertainty = converted_uncertainty
                .checked_add(1)
                .ok_or(TimeError::TimestampOverflow)?;
        }

        Ok(ConvertedTimestamp {
            timestamp: Timestamp::new(self.target, converted),
            uncertainty_ns: converted_uncertainty,
        })
    }
}

/// Requires a mapping to be present, returning the typed unavailable error
/// when a caller has no calibration for the requested conversion.
pub fn require_mapping(mapping: Option<&ClockMapping>) -> Result<&ClockMapping, TimeError> {
    mapping.ok_or(TimeError::ClockMappingUnavailable)
}

fn ensure_same_clock(left: &ClockIdentity, right: &ClockIdentity) -> Result<(), TimeError> {
    if left.kind != right.kind {
        return Err(TimeError::ClockDomainMismatch);
    }
    if left.source_id != right.source_id {
        return Err(TimeError::ClockSourceMismatch);
    }
    if left.generation != right.generation {
        return Err(TimeError::ClockGenerationMismatch);
    }
    Ok(())
}

fn validate_source_for_kind(kind: ClockKind, source_id: ClockSourceId) -> Result<(), TimeError> {
    let bytes = source_id.as_bytes();
    let prefix_valid = bytes[..16].iter().any(|byte| *byte != 0);
    let tail_is_zero = bytes[16..].iter().all(|byte| *byte == 0);
    match kind {
        ClockKind::HostMonotonic => {
            let offset_nanoseconds = i64::from_le_bytes(
                bytes[40..48]
                    .try_into()
                    .map_err(|_| TimeError::InvalidClockIdentity)?,
            );
            if !prefix_valid || !(0..1_000_000_000).contains(&offset_nanoseconds) {
                return Err(TimeError::InvalidClockIdentity);
            }
        }
        ClockKind::SystemRealtime | ClockKind::Device => {
            if !prefix_valid || !tail_is_zero {
                return Err(TimeError::InvalidClockIdentity);
            }
        }
    }
    Ok(())
}

fn decode_lower_hex(byte: u8) -> Result<u8, TimeError> {
    match byte {
        b'0'..=b'9' => Ok(byte - b'0'),
        b'a'..=b'f' => Ok(byte - b'a' + 10),
        _ => Err(TimeError::InvalidClockSourceId),
    }
}

fn timespec_to_nanoseconds(seconds: i64, nanoseconds: i64) -> Result<i64, TimeError> {
    if !(0..1_000_000_000).contains(&nanoseconds) {
        return Err(TimeError::ClockUnavailable);
    }
    let value = i128::from(seconds)
        .checked_mul(NANOS_PER_SECOND)
        .and_then(|whole| whole.checked_add(i128::from(nanoseconds)))
        .ok_or(TimeError::TimestampOutOfRange)?;
    i64::try_from(value).map_err(|_| TimeError::TimestampOutOfRange)
}

#[cfg(target_os = "linux")]
fn read_boot_id() -> Result<[u8; 16], TimeError> {
    let boot_id = std::fs::read_to_string("/proc/sys/kernel/random/boot_id")
        .map_err(|_| TimeError::ClockUnavailable)?;
    parse_boot_id(&boot_id)
}

fn parse_boot_id(boot_id: &str) -> Result<[u8; 16], TimeError> {
    let encoded = boot_id.trim_end_matches(['\r', '\n']).as_bytes();
    if encoded.len() != 36 {
        return Err(TimeError::UnknownClockDomain);
    }

    let mut bytes = [0_u8; 16];
    let mut byte_index = 0;
    let mut high_nibble = None;
    for (index, encoded_byte) in encoded.iter().copied().enumerate() {
        if [8, 13, 18, 23].contains(&index) {
            if encoded_byte != b'-' {
                return Err(TimeError::UnknownClockDomain);
            }
            continue;
        }

        let nibble = decode_lower_hex(encoded_byte).map_err(|_| TimeError::UnknownClockDomain)?;
        if let Some(high) = high_nibble.take() {
            bytes[byte_index] = (high << 4) | nibble;
            byte_index += 1;
        } else {
            high_nibble = Some(nibble);
        }
    }
    if byte_index != bytes.len() || high_nibble.is_some() || bytes.iter().all(|byte| *byte == 0) {
        return Err(TimeError::UnknownClockDomain);
    }
    Ok(bytes)
}

#[cfg(target_os = "linux")]
fn current_time_namespace() -> Result<(u64, u64), TimeError> {
    use std::os::unix::fs::MetadataExt as _;

    let metadata =
        std::fs::metadata("/proc/thread-self/ns/time").map_err(|_| TimeError::ClockUnavailable)?;
    Ok((metadata.dev(), metadata.ino()))
}

#[cfg(target_os = "linux")]
fn read_monotonic_namespace_offset() -> Result<(i64, i64), TimeError> {
    let thread_path =
        std::fs::read_link("/proc/thread-self").map_err(|_| TimeError::ClockUnavailable)?;
    let thread_id = thread_path
        .file_name()
        .and_then(|name| name.to_str())
        .and_then(|name| name.parse::<u32>().ok())
        .filter(|thread_id| *thread_id != 0)
        .ok_or(TimeError::UnknownClockDomain)?;
    let contents = std::fs::read_to_string(format!("/proc/{thread_id}/timens_offsets"))
        .map_err(|_| TimeError::UnknownClockDomain)?;
    let mut monotonic_offset = None;
    for line in contents.lines() {
        let mut fields = line.split_ascii_whitespace();
        let Some(clock_name) = fields.next() else {
            continue;
        };
        if clock_name != "monotonic" {
            continue;
        }
        if monotonic_offset.is_some() {
            return Err(TimeError::UnknownClockDomain);
        }
        let seconds = fields
            .next()
            .and_then(|field| field.parse::<i64>().ok())
            .ok_or(TimeError::UnknownClockDomain)?;
        let nanoseconds = fields
            .next()
            .and_then(|field| field.parse::<i64>().ok())
            .ok_or(TimeError::UnknownClockDomain)?;
        if fields.next().is_some() || !(0..1_000_000_000).contains(&nanoseconds) {
            return Err(TimeError::UnknownClockDomain);
        }
        monotonic_offset = Some((seconds, nanoseconds));
    }
    monotonic_offset.ok_or(TimeError::UnknownClockDomain)
}

#[cfg(test)]
mod boot_id_tests {
    use super::parse_boot_id;
    use crate::TimeError;

    #[test]
    fn parses_canonical_boot_uuid() {
        assert_eq!(
            parse_boot_id("00112233-4455-6677-8899-aabbccddeeff\n"),
            Ok([
                0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd,
                0xee, 0xff,
            ])
        );
    }

    #[test]
    fn rejects_thirty_six_hyphens_without_panicking() {
        assert_eq!(
            parse_boot_id(&"-".repeat(36)),
            Err(TimeError::UnknownClockDomain)
        );
    }

    #[test]
    fn rejects_misplaced_or_extra_separators() {
        assert_eq!(
            parse_boot_id("0011223-34455-6677-8899-aabbccddeeff"),
            Err(TimeError::UnknownClockDomain)
        );
        assert_eq!(
            parse_boot_id("00112233-4455-6677-8899-aabbccddeeff-"),
            Err(TimeError::UnknownClockDomain)
        );
    }
}

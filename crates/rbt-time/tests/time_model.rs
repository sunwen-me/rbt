use std::cmp::Ordering;

use rbt_time::{
    ClockDiscontinuity, ClockIdentity, ClockKind, ClockMapping, ClockMappingCalibration,
    ClockSourceId, DeviceClock, HostClock, SensorTiming, TimeError, Timestamp, TimestampState,
    require_mapping,
};

fn device_clock(seed: u8, generation: u64) -> DeviceClock {
    DeviceClock::new([seed; 16], generation).expect("valid device ID")
}

fn realtime_identity(seed: u8, generation: u64) -> ClockIdentity {
    let mut bytes = [0_u8; 48];
    bytes[..16].fill(seed);
    ClockIdentity::new(
        ClockKind::SystemRealtime,
        ClockSourceId::from_bytes(bytes).expect("nonzero source"),
        generation,
    )
    .expect("valid realtime source")
}

fn host_identity(seed: u8, generation: u64) -> ClockIdentity {
    let mut bytes = [0_u8; 48];
    bytes[..16].fill(seed);
    ClockIdentity::new(
        ClockKind::HostMonotonic,
        ClockSourceId::from_bytes(bytes).expect("nonzero source"),
        generation,
    )
    .expect("valid monotonic source")
}

fn calibration() -> ClockMappingCalibration {
    ClockMappingCalibration {
        source: device_clock(1, 0).identity(),
        target: realtime_identity(2, 0),
        source_anchor_ns: 100,
        target_anchor_ns: 1_000,
        rate_numerator: 3,
        rate_denominator: 2,
        valid_from_ns: 0,
        valid_until_ns: 200,
        calibration_id: [3; 16],
        calibrated_at: Timestamp::new(host_identity(4, 0), 10),
        expires_at: Timestamp::new(host_identity(4, 0), 20),
        uncertainty_ns: Some(4),
    }
}

fn mapping() -> ClockMapping {
    ClockMapping::new(calibration()).expect("valid calibration")
}

fn convert(
    mapping: &ClockMapping,
    input_ns: i64,
    uncertainty: Option<u64>,
    now_ns: i64,
) -> rbt_time::ConvertedTimestamp {
    let calibration = calibration();
    mapping
        .convert(
            &Timestamp::new(calibration.source, input_ns),
            uncertainty,
            &calibration.target,
            &Timestamp::new(*calibration.calibrated_at.clock(), now_ns),
        )
        .expect("valid conversion")
}

#[test]
fn source_id_is_fixed_and_serializes_as_canonical_hex() {
    let source = ClockSourceId::from_device_id([0xab; 16]).expect("valid device ID");
    let encoded = serde_json::to_string(&source).expect("serialize source ID");
    assert_eq!(
        encoded,
        format!("\"{}{}\"", "ab".repeat(16), "00".repeat(32))
    );
    assert_eq!(ClockSourceId::from_hex(&source.to_hex()), Ok(source));
    assert_eq!(source.as_bytes().len(), 48);
    assert_eq!(
        ClockSourceId::from_bytes([0; 48]),
        Err(TimeError::InvalidClockSourceId)
    );
    assert!(ClockSourceId::from_hex(&"AB".repeat(48)).is_err());
}

#[test]
fn clock_mismatch_errors_follow_domain_source_generation_order() {
    let first = device_clock(1, 0).stamp(10);
    let different_domain = Timestamp::new(realtime_identity(1, 0), 10);
    assert_eq!(
        first.checked_compare(&different_domain),
        Err(TimeError::ClockDomainMismatch)
    );

    let different_source = device_clock(2, 0).stamp(10);
    assert_eq!(
        first.checked_compare(&different_source),
        Err(TimeError::ClockSourceMismatch)
    );

    let different_generation = device_clock(1, 1).stamp(10);
    assert_eq!(
        first.checked_compare(&different_generation),
        Err(TimeError::ClockGenerationMismatch)
    );
    assert_eq!(first.checked_compare(&first), Ok(Ordering::Equal));
}

#[test]
fn timestamp_arithmetic_checks_overflow_and_negative_duration() {
    let clock = device_clock(1, 0).identity();
    let low = Timestamp::new(clock, i64::MIN);
    let high = Timestamp::new(clock, i64::MAX);
    assert_eq!(
        high.checked_difference(&low),
        Err(TimeError::TimestampOverflow)
    );
    let start = Timestamp::new(clock, 0);
    let end = Timestamp::new(clock, 1);
    assert_eq!(
        start.checked_duration_since(&end),
        Err(TimeError::NegativeDuration)
    );
    assert_eq!(
        high.checked_add_nanoseconds(1),
        Err(TimeError::TimestampOverflow)
    );
}

#[test]
fn conversion_truncates_toward_zero_and_includes_rounding_uncertainty() {
    let mapping = mapping();
    let positive = convert(&mapping, 101, Some(2), 19);
    assert_eq!(positive.timestamp.nanoseconds(), 1_001);
    assert_eq!(positive.uncertainty_ns, 8);

    let negative = convert(&mapping, 99, Some(2), 19);
    assert_eq!(negative.timestamp.nanoseconds(), 999);
    assert_eq!(negative.uncertainty_ns, 8);

    let exact = convert(&mapping, 100, Some(2), 19);
    assert_eq!(exact.timestamp.nanoseconds(), 1_000);
    assert_eq!(exact.uncertainty_ns, 7);

    assert_eq!(
        mapping
            .convert(
                &Timestamp::new(*mapping.source(), 0),
                Some(1),
                mapping.target(),
                &Timestamp::new(host_identity(4, 0), 10),
            )
            .expect("inclusive lower endpoint")
            .timestamp
            .nanoseconds(),
        850
    );
    assert_eq!(
        mapping
            .convert(
                &Timestamp::new(*mapping.source(), 200),
                Some(1),
                mapping.target(),
                &Timestamp::new(host_identity(4, 0), 10),
            )
            .expect("inclusive upper endpoint")
            .timestamp
            .nanoseconds(),
        1_150
    );
}

#[test]
fn mapping_rejects_wrong_sources_targets_expiry_and_missing_uncertainty() {
    let mapping = mapping();
    let now = Timestamp::new(host_identity(4, 0), 19);
    let input = Timestamp::new(*mapping.source(), 100);

    assert_eq!(
        mapping.convert(
            &Timestamp::new(realtime_identity(2, 0), 100),
            Some(0),
            mapping.target(),
            &now,
        ),
        Err(TimeError::ClockDomainMismatch)
    );
    assert_eq!(
        mapping.convert(
            &Timestamp::new(device_clock(9, 0).identity(), 100),
            Some(0),
            mapping.target(),
            &now,
        ),
        Err(TimeError::ClockSourceMismatch)
    );
    assert_eq!(
        mapping.convert(
            &Timestamp::new(device_clock(1, 1).identity(), 100),
            Some(0),
            mapping.target(),
            &now,
        ),
        Err(TimeError::ClockGenerationMismatch)
    );
    assert_eq!(
        mapping.convert(&input, Some(0), &realtime_identity(2, 1), &now),
        Err(TimeError::ClockGenerationMismatch)
    );
    assert_eq!(
        mapping.convert(
            &input,
            Some(0),
            mapping.target(),
            &Timestamp::new(host_identity(4, 0), 20)
        ),
        Err(TimeError::ClockMappingExpired)
    );
    assert_eq!(
        mapping.convert(
            &input,
            Some(0),
            mapping.target(),
            &Timestamp::new(host_identity(4, 0), 9)
        ),
        Err(TimeError::ClockMappingUnavailable)
    );
    assert_eq!(
        mapping.convert(&input, None, mapping.target(), &now),
        Err(TimeError::UnknownUncertainty)
    );
    assert_eq!(
        mapping.convert(
            &Timestamp::new(*mapping.source(), 201),
            Some(0),
            mapping.target(),
            &now
        ),
        Err(TimeError::TimestampOutOfRange)
    );
    assert_eq!(
        require_mapping(None).map(|_| ()),
        Err(TimeError::ClockMappingUnavailable)
    );
}

#[test]
fn same_clock_mapping_must_be_identity() {
    let mut identity = calibration();
    identity.target = identity.source;
    identity.source_anchor_ns = 123;
    identity.target_anchor_ns = 123;
    identity.rate_numerator = 2;
    identity.rate_denominator = 2;
    assert!(ClockMapping::new(identity).is_ok());

    let mut offset = identity;
    offset.target_anchor_ns += 1;
    assert_eq!(
        ClockMapping::new(offset).err(),
        Some(TimeError::ClockMappingInvalid)
    );

    let mut rate_distortion = identity;
    rate_distortion.rate_numerator = 3;
    assert_eq!(
        ClockMapping::new(rate_distortion).err(),
        Some(TimeError::ClockMappingInvalid)
    );
}

#[test]
fn calibration_validation_and_conversion_detect_overflow() {
    let mut invalid_rate = calibration();
    invalid_rate.rate_numerator = 0;
    assert_eq!(
        ClockMapping::new(invalid_rate).err(),
        Some(TimeError::InvalidConversionRate)
    );

    let mut unknown_mapping_uncertainty = calibration();
    unknown_mapping_uncertainty.uncertainty_ns = None;
    assert_eq!(
        ClockMapping::new(unknown_mapping_uncertainty).err(),
        Some(TimeError::UnknownUncertainty)
    );

    let mut no_evidence = calibration();
    no_evidence.calibration_id = [0; 16];
    assert_eq!(
        ClockMapping::new(no_evidence).err(),
        Some(TimeError::InvalidCalibrationEvidence)
    );

    let mut uncertainty_overflow = calibration();
    uncertainty_overflow.rate_numerator = u64::MAX;
    uncertainty_overflow.rate_denominator = 1;
    uncertainty_overflow.valid_from_ns = 100;
    uncertainty_overflow.valid_until_ns = 100;
    let uncertainty_overflow = ClockMapping::new(uncertainty_overflow).expect("valid rate");
    assert_eq!(
        uncertainty_overflow.convert(
            &Timestamp::new(*uncertainty_overflow.source(), 100),
            Some(u64::MAX),
            uncertainty_overflow.target(),
            &Timestamp::new(host_identity(4, 0), 10),
        ),
        Err(TimeError::TimestampOverflow)
    );

    let mut full_width_uncertainty = calibration();
    full_width_uncertainty.rate_numerator = u64::MAX;
    full_width_uncertainty.rate_denominator = u64::MAX;
    full_width_uncertainty.valid_from_ns = 100;
    full_width_uncertainty.valid_until_ns = 100;
    full_width_uncertainty.uncertainty_ns = Some(0);
    let full_width_uncertainty = ClockMapping::new(full_width_uncertainty).expect("valid rate");
    assert_eq!(
        full_width_uncertainty
            .convert(
                &Timestamp::new(*full_width_uncertainty.source(), 100),
                Some(u64::MAX),
                full_width_uncertainty.target(),
                &Timestamp::new(host_identity(4, 0), 10),
            )
            .expect("full-width uncertainty conversion")
            .uncertainty_ns,
        u64::MAX
    );

    let mut timestamp_overflow = calibration();
    timestamp_overflow.rate_numerator = u64::MAX;
    timestamp_overflow.rate_denominator = 1;
    timestamp_overflow.source_anchor_ns = i64::MIN;
    timestamp_overflow.valid_from_ns = i64::MAX;
    timestamp_overflow.valid_until_ns = i64::MAX;
    let timestamp_overflow = ClockMapping::new(timestamp_overflow).expect("valid rate");
    assert_eq!(
        timestamp_overflow.convert(
            &Timestamp::new(*timestamp_overflow.source(), i64::MAX),
            Some(0),
            timestamp_overflow.target(),
            &Timestamp::new(host_identity(4, 0), 10),
        ),
        Err(TimeError::TimestampOverflow)
    );

    let mut output_range = calibration();
    output_range.rate_numerator = 1;
    output_range.rate_denominator = 1;
    output_range.source_anchor_ns = 0;
    output_range.target_anchor_ns = i64::MAX;
    output_range.valid_from_ns = 1;
    output_range.valid_until_ns = 1;
    let output_range = ClockMapping::new(output_range).expect("valid mapping");
    assert_eq!(
        output_range.convert(
            &Timestamp::new(*output_range.source(), 1),
            Some(0),
            output_range.target(),
            &Timestamp::new(host_identity(4, 0), 10),
        ),
        Err(TimeError::TimestampOverflow)
    );
}

#[test]
fn reset_and_realtime_step_events_invalidate_only_affected_mappings() {
    let mut source_clock = device_clock(1, 0);
    let mapping = mapping();
    let event = source_clock.reset().expect("generation can advance");
    assert_eq!(source_clock.identity().generation(), 1);
    assert!(mapping.invalidate(&event));
    assert_eq!(
        mapping.convert(
            &Timestamp::new(*mapping.source(), 100),
            Some(0),
            mapping.target(),
            &Timestamp::new(host_identity(4, 0), 11),
        ),
        Err(TimeError::MappingInvalidated)
    );

    let unrelated = ClockDiscontinuity::ExternalStep {
        clock: realtime_identity(8, 0),
    };
    assert!(
        !ClockMapping::new(calibration())
            .expect("valid calibration")
            .invalidate(&unrelated)
    );

    // These values are synthetic observations. The event models a caller-
    // confirmed realtime step without changing the machine clock.
    let realtime_mapping = ClockMapping::new(ClockMappingCalibration {
        source: device_clock(1, 0).identity(),
        target: realtime_identity(2, 0),
        source_anchor_ns: 0,
        target_anchor_ns: 0,
        rate_numerator: 1,
        rate_denominator: 1,
        valid_from_ns: 0,
        valid_until_ns: 100,
        calibration_id: [5; 16],
        calibrated_at: Timestamp::new(host_identity(4, 0), 10),
        expires_at: Timestamp::new(host_identity(4, 0), 20),
        uncertainty_ns: Some(0),
    })
    .expect("valid realtime mapping");
    let old_realtime = realtime_identity(2, 0);
    let new_realtime = realtime_identity(2, 1);
    let jump = ClockDiscontinuity::RealtimeStep {
        previous: old_realtime,
        current: new_realtime,
    };
    assert!(realtime_mapping.invalidate(&jump));
    assert_eq!(
        Timestamp::new(old_realtime, 500).checked_difference(&Timestamp::new(new_realtime, 1)),
        Err(TimeError::ClockGenerationMismatch)
    );
}

#[test]
fn missing_capture_is_preserved_and_legacy_numbers_stay_unknown() {
    let received = Timestamp::new(host_identity(4, 0), 123);
    let timing = SensorTiming::new(None, received, None, Some(4)).expect("valid timing");
    assert!(timing.capture().is_none());
    assert_eq!(timing.received(), &received);
    assert_eq!(timing.capture_uncertainty_ns(), None);
    assert_eq!(
        SensorTiming::new(None, received, Some(0), None),
        Err(TimeError::InvalidTimestamp)
    );
    let mut serialized = serde_json::to_value(timing).expect("serialize timing");
    serialized["capture_uncertainty_ns"] = serde_json::json!(0);
    assert!(serde_json::from_value::<SensorTiming>(serialized).is_err());
    assert_eq!(
        TimestampState::from_legacy_timestamp_ns(0),
        TimestampState::Unknown
    );
    assert_eq!(
        TimestampState::from_legacy_timestamp_ns(u64::MAX),
        TimestampState::Unknown
    );
}

#[test]
fn serialized_structures_preserve_invariants_and_calibration_data() {
    let calibration = calibration();
    let encoded = serde_json::to_string(&calibration).expect("serialize calibration");
    let decoded: ClockMappingCalibration =
        serde_json::from_str(&encoded).expect("deserialize calibration");
    assert_eq!(decoded.calibration_id, calibration.calibration_id);
    assert_eq!(decoded.source, calibration.source);
    assert!(ClockMapping::new(decoded).is_ok());

    let mut invalid_source = [0_u8; 48];
    invalid_source[0] = 1;
    invalid_source[47] = 1;
    let invalid_identity = serde_json::json!({
        "kind": "device",
        "source_id": ClockSourceId::from_bytes(invalid_source)
            .expect("nonzero source")
            .to_hex(),
        "generation": 0
    });
    assert!(serde_json::from_value::<ClockIdentity>(invalid_identity).is_err());
}

#[cfg(target_os = "linux")]
#[test]
fn host_clock_requires_a_known_time_namespace_basis() {
    match HostClock::new() {
        Ok(clock) => {
            let first = clock.monotonic_now().expect("monotonic clock");
            let second = clock.monotonic_now().expect("monotonic clock");
            assert_ne!(first.clock().kind(), ClockKind::SystemRealtime);
            assert!(matches!(
                first.checked_compare(&second),
                Ok(Ordering::Less | Ordering::Equal)
            ));
            let before = clock.realtime_now().expect("realtime clock");
            let event = clock
                .confirm_realtime_discontinuity()
                .expect("generation can advance");
            let after = clock.realtime_now().expect("realtime clock");
            assert!(matches!(event, ClockDiscontinuity::RealtimeStep { .. }));
            assert_eq!(
                before.checked_compare(&after),
                Err(TimeError::ClockGenerationMismatch)
            );
        }
        Err(TimeError::UnknownClockDomain) => {}
        Err(error) => panic!("unexpected host clock construction error: {error}"),
    }
}

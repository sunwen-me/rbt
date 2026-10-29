//! Local clock diagnostics. Optional input is a bounded, explicitly selected
//! observation file; it is neither a registry nor an authority on device sync.

use std::{collections::BTreeSet, fmt::Write as _, fs::File, io::Read, path::Path};

use rbt_time::{
    ClockIdentity, ClockKind, ClockMapping, ClockMappingCalibration, HostClock, Timestamp,
};
use serde::{Deserialize, Serialize};

use crate::{
    doctor::{DoctorDiagnostic, DoctorReport},
    project::DiagnosticStatus,
};

const MAX_SNAPSHOT_BYTES: u64 = 1_048_576;
const MAX_SOURCES: usize = 128;
const MAX_AGE_NS: i64 = 2_500_000_000;

/// Explicit local observation input. No sources are discovered or invented.
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SourceSnapshot {
    pub sources: Vec<SourceObservation>,
}

/// A source clock, a real host-monotonic observation, and optional explicit
/// calibration. A calibration token identifies supplied evidence; it does not
/// certify the trustworthiness of the producer that wrote this file.
#[derive(Debug, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct SourceObservation {
    pub name: String,
    pub clock: ClockIdentity,
    pub observed_at: Option<Timestamp>,
    pub last_timestamp: Option<Timestamp>,
    pub uncertainty_ns: Option<u64>,
    pub mapping: Option<ClockMappingCalibration>,
    #[serde(default)]
    pub invalidated: bool,
}

#[derive(Clone, Debug, Serialize)]
pub struct Uncertainty {
    pub status: &'static str,
    pub nanoseconds: Option<u64>,
}

impl Uncertainty {
    fn unknown() -> Self {
        Self {
            status: "unknown",
            nanoseconds: None,
        }
    }
    fn unavailable() -> Self {
        Self {
            status: "unavailable",
            nanoseconds: None,
        }
    }
    fn known(value: u64) -> Self {
        Self {
            status: "known",
            nanoseconds: Some(value),
        }
    }
}

#[derive(Clone, Debug, Serialize)]
pub struct Freshness {
    pub status: &'static str,
    pub age_ns: Option<i64>,
    pub observed_at: Option<Timestamp>,
}

#[derive(Clone, Debug, Serialize)]
pub struct TimeSourceStatus {
    pub name: String,
    pub source_id: String,
    pub clock_domain: ClockKind,
    pub generation: u64,
    pub mapping_status: &'static str,
    pub uncertainty: Uncertainty,
    pub timestamp_uncertainty: Uncertainty,
    pub freshness: Freshness,
    pub reason_code: String,
    pub nanoseconds: Option<i64>,
    pub calibration_id: Option<String>,
    pub valid_from_ns: Option<i64>,
    pub valid_until_ns: Option<i64>,
    pub expires_at: Option<Timestamp>,
}

#[derive(Clone, Debug, Serialize)]
pub struct HostClockStatus {
    pub availability: &'static str,
    pub timestamp: Option<Timestamp>,
    pub reason_code: String,
}

#[derive(Clone, Debug, Serialize)]
pub struct HostClocks {
    pub monotonic: HostClockStatus,
    pub realtime: HostClockStatus,
}

/// Preserves the v0.5 checks/status/success diagnostic shape, with additive
/// typed time fields. Zero uncertainty and missing uncertainty stay distinct.
#[derive(Debug, Serialize)]
pub struct TimeStatusReport {
    pub platform: String,
    pub checks: Vec<DoctorDiagnostic>,
    pub success: bool,
    pub host_clocks: HostClocks,
    pub sources: Vec<TimeSourceStatus>,
    pub source_observations: &'static str,
}

#[derive(Debug, thiserror::Error)]
pub enum TimeStatusError {
    #[error("cannot read time source snapshot: {0}")]
    Io(#[from] std::io::Error),
    #[error("invalid time source snapshot: {0}")]
    Json(#[from] serde_json::Error),
    #[error("invalid time source snapshot: {0}")]
    Invalid(String),
}

fn read_snapshot(path: &Path) -> Result<SourceSnapshot, TimeStatusError> {
    let mut bytes = Vec::new();
    File::open(path)?
        .take(MAX_SNAPSHOT_BYTES + 1)
        .read_to_end(&mut bytes)?;
    if bytes.len() as u64 > MAX_SNAPSHOT_BYTES {
        return Err(TimeStatusError::Invalid("larger than 1 MiB".to_owned()));
    }
    let snapshot: SourceSnapshot = serde_json::from_slice(&bytes)?;
    if snapshot.sources.len() > MAX_SOURCES {
        return Err(TimeStatusError::Invalid("more than 128 sources".to_owned()));
    }
    let mut names = BTreeSet::new();
    for source in &snapshot.sources {
        if source.name.is_empty()
            || source.name.len() > 256
            || source.name.chars().any(char::is_control)
            || !names.insert(&source.name)
        {
            return Err(TimeStatusError::Invalid("source names must be unique, nonempty and at most 256 bytes without control characters".to_owned()));
        }
    }
    Ok(snapshot)
}

fn host_status(result: Result<Timestamp, rbt_time::TimeError>) -> HostClockStatus {
    match result {
        Ok(timestamp) => HostClockStatus {
            availability: "available",
            timestamp: Some(timestamp),
            reason_code: "HostClockAvailable".to_owned(),
        },
        Err(error) => HostClockStatus {
            availability: "unavailable",
            timestamp: None,
            reason_code: format!("{error:?}"),
        },
    }
}

fn freshness(now: Option<Timestamp>, observed: Option<Timestamp>) -> (Freshness, Option<String>) {
    let (Some(now), Some(observed)) = (now, observed) else {
        return (
            Freshness {
                status: "unavailable",
                age_ns: None,
                observed_at: observed,
            },
            Some("ObservationUnavailable".to_owned()),
        );
    };
    match now.checked_difference(&observed) {
        Ok(age) if age >= 0 => (
            Freshness {
                status: if age <= MAX_AGE_NS {
                    "current"
                } else {
                    "stale"
                },
                age_ns: Some(age),
                observed_at: Some(observed),
            },
            (age > MAX_AGE_NS).then(|| "ObservationStale".to_owned()),
        ),
        Ok(_) => (
            Freshness {
                status: "unknown",
                age_ns: None,
                observed_at: Some(observed),
            },
            Some("ObservationInFuture".to_owned()),
        ),
        Err(error) => (
            Freshness {
                status: "unknown",
                age_ns: None,
                observed_at: Some(observed),
            },
            Some(format!("{error:?}")),
        ),
    }
}

fn source_status(observation: SourceObservation, now: Option<Timestamp>) -> TimeSourceStatus {
    let (freshness, freshness_reason) = freshness(now, observation.observed_at);
    let mut status = TimeSourceStatus {
        name: observation.name,
        source_id: observation.clock.source_id().to_hex(),
        clock_domain: observation.clock.kind(),
        generation: observation.clock.generation(),
        mapping_status: "unavailable",
        uncertainty: Uncertainty::unavailable(),
        timestamp_uncertainty: observation
            .uncertainty_ns
            .map_or_else(Uncertainty::unknown, Uncertainty::known),
        freshness,
        reason_code: "ClockMappingUnavailable".to_owned(),
        nanoseconds: observation.last_timestamp.map(|stamp| stamp.nanoseconds()),
        calibration_id: None,
        valid_from_ns: None,
        valid_until_ns: None,
        expires_at: None,
    };
    if observation.invalidated {
        status.mapping_status = "invalidated";
        status.reason_code = "MappingInvalidated".to_owned();
        return status;
    }
    if let Some(sample) = observation.last_timestamp
        && let Err(error) = sample.clock().checked_compatible(&observation.clock)
    {
        status.mapping_status = "invalid";
        status.reason_code = format!("{error:?}");
        return status;
    }
    if observation.mapping.is_none()
        && now.is_some_and(|host| host.clock().checked_compatible(&observation.clock).is_ok())
    {
        status.mapping_status = "not_required_for_same_clock";
        status.uncertainty = status.timestamp_uncertainty.clone();
        status.reason_code =
            freshness_reason.unwrap_or_else(|| "SameIdentityComparisonOnly".to_owned());
        return status;
    }
    if let Some(calibration) = observation.mapping {
        status.calibration_id = Some(
            calibration
                .calibration_id
                .iter()
                .map(|byte| format!("{byte:02x}"))
                .collect(),
        );
        status.valid_from_ns = Some(calibration.valid_from_ns);
        status.valid_until_ns = Some(calibration.valid_until_ns);
        status.expires_at = Some(calibration.expires_at);
        status.uncertainty = calibration
            .uncertainty_ns
            .map_or_else(Uncertainty::unknown, Uncertainty::known);
        let target = calibration.target;
        let result = ClockMapping::new(calibration).and_then(|mapping| {
            let sample = observation
                .last_timestamp
                .ok_or(rbt_time::TimeError::UnknownClockDomain)?;
            // A mismatched metadata identity is never a valid source report.
            sample.clock().checked_compatible(&observation.clock)?;
            mapping.convert(
                &sample,
                observation.uncertainty_ns,
                &target,
                &now.ok_or(rbt_time::TimeError::ClockUnavailable)?,
            )
        });
        match result {
            Ok(converted) => {
                status.mapping_status = "valid";
                status.uncertainty = Uncertainty::known(converted.uncertainty_ns);
                status.reason_code = "ExplicitCalibrationValidated".to_owned();
            }
            Err(error) => {
                status.mapping_status = match error {
                    rbt_time::TimeError::ClockMappingExpired => "expired",
                    rbt_time::TimeError::ClockMappingUnavailable
                    | rbt_time::TimeError::TimestampOutOfRange => "unavailable",
                    rbt_time::TimeError::UnknownClockDomain
                    | rbt_time::TimeError::UnknownUncertainty
                    | rbt_time::TimeError::ClockUnavailable => "unknown",
                    _ => "invalid",
                };
                status.reason_code = format!("{error:?}");
            }
        }
    }
    if let Some(reason) = freshness_reason
        && status.mapping_status == "valid"
    {
        status.mapping_status = "unknown";
        status.reason_code = reason;
    }
    status
}

pub fn run_time_status(path: Option<&Path>) -> Result<TimeStatusReport, TimeStatusError> {
    let snapshot = path.map(read_snapshot).transpose()?;
    let (monotonic, realtime) = match HostClock::new() {
        Ok(clock) => (
            host_status(clock.monotonic_now()),
            host_status(clock.realtime_now()),
        ),
        Err(error) => (host_status(Err(error.clone())), host_status(Err(error))),
    };
    let now = monotonic.timestamp;
    let checks = [
        ("time.host.monotonic", &monotonic),
        ("time.host.realtime", &realtime),
    ]
    .into_iter()
    .map(|(id, clock)| DoctorDiagnostic {
        check_id: id.to_owned(),
        status: if clock.timestamp.is_some() {
            DiagnosticStatus::Pass
        } else {
            DiagnosticStatus::Fail
        },
        summary: format!("{id}: {}", clock.availability),
        details: Some(clock.reason_code.clone()),
        suggested_action: None,
    })
    .collect();
    let doctor = DoctorReport::new(checks);
    let mut sources = Vec::new();
    for (name, stamp) in [
        ("host/monotonic", monotonic.timestamp),
        ("host/realtime", realtime.timestamp),
    ] {
        if let Some(stamp) = stamp {
            sources.push(TimeSourceStatus {
                name: name.to_owned(),
                source_id: stamp.clock().source_id().to_hex(),
                clock_domain: stamp.clock().kind(),
                generation: stamp.clock().generation(),
                mapping_status: "not_required_for_same_clock",
                uncertainty: Uncertainty::unknown(),
                timestamp_uncertainty: Uncertainty::unknown(),
                freshness: Freshness {
                    status: "current",
                    age_ns: Some(0),
                    observed_at: now,
                },
                reason_code: "SameIdentityComparisonOnly".to_owned(),
                nanoseconds: Some(stamp.nanoseconds()),
                calibration_id: None,
                valid_from_ns: None,
                valid_until_ns: None,
                expires_at: None,
            });
        }
    }
    if let Some(snapshot) = snapshot {
        sources.extend(
            snapshot
                .sources
                .into_iter()
                .map(|source| source_status(source, now)),
        );
    }
    Ok(TimeStatusReport {
        platform: doctor.platform,
        checks: doctor.checks,
        success: doctor.success,
        host_clocks: HostClocks {
            monotonic,
            realtime,
        },
        sources,
        source_observations: if path.is_some() {
            "explicit_local_snapshot"
        } else {
            "local_host_only"
        },
    })
}

pub fn format_time_status(report: &TimeStatusReport) -> String {
    let mut output = format!(
        "Host clocks:\n  monotonic    {}\n  realtime     {}\n\nSources ({}):\n",
        report.host_clocks.monotonic.availability,
        report.host_clocks.realtime.availability,
        report.source_observations
    );
    for source in &report.sources {
        let _ = writeln!(
            output,
            "  {}\n    domain       {:?}\n    source_id    {}\n    generation   {}\n    mapping      {}\n    uncertainty  {}{}\n    freshness    {}\n    reason_code  {}",
            source.name,
            source.clock_domain,
            source.source_id,
            source.generation,
            source.mapping_status,
            source.uncertainty.status,
            source
                .uncertainty
                .nanoseconds
                .map(|value| format!(" ({value} ns)"))
                .unwrap_or_default(),
            source.freshness.status,
            source.reason_code
        );
    }
    output
        .push_str("\nCapture time is unknown unless supplied by a verified acquisition source.\n");
    output
}

#[cfg(test)]
mod tests {
    use super::*;
    use rbt_time::ClockSourceId;

    fn identity(kind: ClockKind, id: u8, generation: u64) -> ClockIdentity {
        ClockIdentity::new(
            kind,
            ClockSourceId::from_device_id([id; 16]).unwrap(),
            generation,
        )
        .unwrap()
    }

    fn observation() -> (SourceObservation, Timestamp) {
        let source = identity(ClockKind::Device, 1, 1);
        let target = identity(ClockKind::HostMonotonic, 2, 0);
        let now = Timestamp::new(target, 5_000);
        (
            SourceObservation {
                name: "camera/front".to_owned(),
                clock: source,
                observed_at: Some(now),
                last_timestamp: Some(Timestamp::new(source, 100)),
                uncertainty_ns: Some(0),
                invalidated: false,
                mapping: Some(ClockMappingCalibration {
                    source,
                    target,
                    source_anchor_ns: 0,
                    target_anchor_ns: 1_000,
                    rate_numerator: 1,
                    rate_denominator: 1,
                    valid_from_ns: 0,
                    valid_until_ns: 1_000,
                    calibration_id: [1; 16],
                    calibrated_at: Timestamp::new(target, 0),
                    expires_at: Timestamp::new(target, 10_000_000_000),
                    uncertainty_ns: Some(0),
                }),
            },
            now,
        )
    }

    #[test]
    fn json_preserves_unknown_unavailable_and_known_zero() {
        assert_eq!(
            serde_json::to_value(Uncertainty::known(0)).unwrap()["nanoseconds"],
            0
        );
        assert!(serde_json::to_value(Uncertainty::unknown()).unwrap()["nanoseconds"].is_null());
        assert_ne!(
            serde_json::to_value(Uncertainty::unknown()).unwrap(),
            serde_json::to_value(Uncertainty::unavailable()).unwrap()
        );
    }

    #[test]
    fn actual_host_report_has_v05_diagnostic_shape_without_sync_claim() {
        let report = run_time_status(None).unwrap();
        let json = serde_json::to_value(&report).unwrap();
        assert!(json["checks"].is_array());
        assert!(json["success"].is_boolean());
        for source in json["sources"].as_array().unwrap() {
            for field in [
                "source_id",
                "clock_domain",
                "generation",
                "mapping_status",
                "uncertainty",
                "freshness",
                "reason_code",
            ] {
                assert!(source.get(field).is_some(), "missing {field}");
            }
            assert_eq!(source["mapping_status"], "not_required_for_same_clock");
            assert!(source["uncertainty"]["nanoseconds"].is_null());
        }
        assert!(!format_time_status(&report).contains("SYNCHRONIZED"));
    }

    #[test]
    fn explicit_source_diagnostics_distinguish_zero_unknown_expired_and_stale() {
        let (input, now) = observation();
        let valid = source_status(input, Some(now));
        assert_eq!(valid.mapping_status, "valid");
        assert_eq!(valid.uncertainty.nanoseconds, Some(0));

        let (mut input, now) = observation();
        input.mapping = None;
        let missing = source_status(input, Some(now));
        assert_eq!(missing.mapping_status, "unavailable");
        assert_eq!(missing.uncertainty.status, "unavailable");

        let (mut input, now) = observation();
        input.mapping.as_mut().unwrap().uncertainty_ns = None;
        let unknown = source_status(input, Some(now));
        assert_eq!(unknown.mapping_status, "unknown");
        assert_eq!(unknown.uncertainty.status, "unknown");

        let (mut input, now) = observation();
        input.mapping.as_mut().unwrap().expires_at = now;
        assert_eq!(source_status(input, Some(now)).mapping_status, "expired");

        let (mut input, now) = observation();
        input.observed_at = Some(Timestamp::new(
            *now.clock(),
            now.nanoseconds() - MAX_AGE_NS - 1,
        ));
        let stale = source_status(input, Some(now));
        assert_eq!(stale.mapping_status, "unknown");
        assert_eq!(stale.freshness.status, "stale");
        assert_eq!(stale.reason_code, "ObservationStale");
    }

    #[test]
    fn generation_metadata_and_observation_clock_are_checked() {
        let (mut input, now) = observation();
        input.clock = identity(ClockKind::Device, 1, 2);
        assert_eq!(
            source_status(input, Some(now)).reason_code,
            "ClockGenerationMismatch"
        );

        let (mut input, now) = observation();
        input.observed_at = Some(Timestamp::new(identity(ClockKind::HostMonotonic, 3, 0), 0));
        let incompatible = source_status(input, Some(now));
        assert_eq!(incompatible.mapping_status, "unknown");
        assert_eq!(incompatible.freshness.age_ns, None);
        assert_eq!(incompatible.reason_code, "ClockSourceMismatch");

        let (mut input, now) = observation();
        input.invalidated = true;
        assert_eq!(
            source_status(input, Some(now)).mapping_status,
            "invalidated"
        );
    }
}

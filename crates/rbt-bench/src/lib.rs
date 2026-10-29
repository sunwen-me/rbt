//! Small, repeatable process-level benchmark support for the v0.1 data plane.
//!
//! The binary deliberately keeps its command-line parser and percentile
//! calculation in this library so they can be tested without starting an
//! iceoryx2 node.

use std::{fmt, time::Duration};

use rbt_iox2::PointXYZI;

/// Machine-readable name of the receive-side payload validation method.
///
/// The benchmark checks every payload element against the deterministic
/// sequence pattern and computes an FNV-1a fingerprint in the same pass.  It
/// deliberately does not allocate a second buffer proportional to payload
/// size.
pub const FULL_LINEAR_FNV1A_VALIDATION: &str = "full_linear_fnv1a";

/// Initial value for the benchmark's deterministic 64-bit FNV-1a fingerprint.
pub const PAYLOAD_FINGERPRINT_OFFSET: u64 = 14_695_981_039_346_656_037;

/// Folds one validated sample fingerprint into a run fingerprint.
///
/// The sample fingerprint is already an FNV-1a result.  Feeding its stable
/// little-endian bytes through FNV-1a keeps the result compact while retaining
/// receive order (which makes dropped/reordered samples visible).
pub fn fold_payload_fingerprint(run: u64, sample: u64) -> u64 {
    sample.to_le_bytes().iter().fold(run, |fingerprint, byte| {
        (fingerprint ^ u64::from(*byte)).wrapping_mul(1_099_511_628_211)
    })
}

/// Validates every generated Image payload byte for one sequence ordinal.
///
/// The returned fingerprint is computed while checking the bytes.  A
/// corruption error identifies the first mismatching byte and does not hide a
/// bad sample behind a size/count-only result.
pub fn validate_image_payload(payload: &[u8], sequence: u64) -> Result<u64, String> {
    let mut fingerprint = PAYLOAD_FINGERPRINT_OFFSET;
    for (index, &actual) in payload.iter().enumerate() {
        let expected = sequence.wrapping_add(index as u64) as u8;
        if actual != expected {
            return Err(format!(
                "Image payload corruption at byte {index} for sequence {sequence}: expected {expected}, got {actual}"
            ));
        }
        fingerprint = (fingerprint ^ u64::from(actual)).wrapping_mul(1_099_511_628_211);
    }
    Ok(fingerprint)
}

/// Validates every generated PointCloud element for one sequence ordinal.
///
/// Floats are compared by their IEEE-754 bit representation, so signed zero
/// and any unexpected NaN/payload bit pattern are detected as corruption.
pub fn validate_pointcloud_payload(payload: &[PointXYZI], sequence: u64) -> Result<u64, String> {
    let mut fingerprint = PAYLOAD_FINGERPRINT_OFFSET;
    for (index, actual) in payload.iter().enumerate() {
        let expected = PointXYZI {
            x: index as f32,
            y: sequence as f32,
            z: -(index as f32),
            intensity: (index & 0xff) as f32,
        };
        for (field, actual_bits, expected_bits) in [
            ("x", actual.x.to_bits(), expected.x.to_bits()),
            ("y", actual.y.to_bits(), expected.y.to_bits()),
            ("z", actual.z.to_bits(), expected.z.to_bits()),
            (
                "intensity",
                actual.intensity.to_bits(),
                expected.intensity.to_bits(),
            ),
        ] {
            if actual_bits != expected_bits {
                return Err(format!(
                    "PointCloud payload corruption at element {index} field {field} for sequence {sequence}: expected 0x{expected_bits:08x}, got 0x{actual_bits:08x}"
                ));
            }
        }
        for value in [expected.x, expected.y, expected.z, expected.intensity] {
            for byte in value.to_bits().to_le_bytes() {
                fingerprint = (fingerprint ^ u64::from(byte)).wrapping_mul(1_099_511_628_211);
            }
        }
    }
    Ok(fingerprint)
}

/// The benchmark process role.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Role {
    /// Produce samples.
    Publisher,
    /// Consume samples and measure receive latency.
    Subscriber,
}

impl Role {
    /// Returns the stable command-line spelling.
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Publisher => "publisher",
            Self::Subscriber => "subscriber",
        }
    }
}

/// Behavior of a benchmark subscriber process.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum SubscriberMode {
    /// Receive samples as soon as they are available.
    Fast,
    /// Receive samples and wait for the configured delay between receives.
    Slow,
    /// Open the subscriber and retain its queue without receiving samples.
    Stalled,
}

impl SubscriberMode {
    /// Returns the stable command-line and metadata spelling.
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Fast => "fast",
            Self::Slow => "slow",
            Self::Stalled => "stalled",
        }
    }
}

impl fmt::Display for SubscriberMode {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.as_str())
    }
}

/// The data-plane stream profile under test.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Profile {
    /// Dynamic byte payload carried by the frozen `Image` slice ABI.
    Sensor,
    /// Fixed `ControlCommand` payload.
    Control,
}

impl Profile {
    /// Returns the stable command-line spelling.
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Sensor => "sensor",
            Self::Control => "control",
        }
    }

    /// Returns the fixed queue configuration used by `rbt-iox2`.
    pub const fn queue_config(self) -> &'static str {
        match self {
            Self::Sensor => "depth=1,safe_overflow=true,max_borrowed=1,backpressure=discard",
            Self::Control => {
                "depth=4,safe_overflow=false,max_borrowed=1,backpressure=discard_and_fail"
            }
        }
    }
}

impl fmt::Display for Profile {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.as_str())
    }
}

/// Generated message ABI carried by a benchmark case.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum MessageKind {
    /// `ImageHeader + u8[]`.
    Image,
    /// `PointCloudHeader + PointXYZI[]`.
    PointCloud,
    /// Fixed `ControlCommand`.
    ControlCommand,
}

impl MessageKind {
    /// Returns the stable command-line and metadata spelling.
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Image => "image",
            Self::PointCloud => "pointcloud",
            Self::ControlCommand => "control-command",
        }
    }
}

impl fmt::Display for MessageKind {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.as_str())
    }
}

/// Parsed arguments shared by publisher and subscriber processes.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct BenchConfig {
    /// Process role.
    pub role: Role,
    /// Caller-supplied iceoryx2 service name.
    pub service: String,
    /// Stream profile.
    pub profile: Profile,
    /// Generated message ABI used by this case.
    pub message: MessageKind,
    /// Dynamic payload size in bytes. Control requires its fixed ABI size.
    pub payload_bytes: usize,
    /// Number of measured samples.
    pub count: u64,
    /// Number of unmeasured warmup samples.
    pub warmup: u64,
    /// Expected number of subscribers, reported as benchmark metadata.
    pub subscriber_count: u32,
    /// Behavior of this process when it is a subscriber.
    pub subscriber_mode: SubscriberMode,
    /// Delay inserted after each received sample.
    pub receive_delay: Duration,
    /// Subscriber open/receive and Control QueueFull retry timeout.
    pub timeout: Duration,
    /// Delay after a publisher reports ready and before it sends warmup.
    pub startup_delay: Duration,
    /// Optional delay between sends.
    pub interval: Duration,
}

impl BenchConfig {
    /// Returns the total number of samples in one run.
    pub fn total_samples(&self) -> Result<u64, String> {
        self.warmup
            .checked_add(self.count)
            .ok_or_else(|| "warmup plus count overflows u64".to_owned())
    }
}

/// Returns command-line help text.
pub fn usage(program: &str) -> String {
    format!(
        "usage: {program} <publisher|subscriber> --service SERVICE --profile <sensor|control> \\
         [--message <image|pointcloud|control-command>] --payload BYTES --count N [options]\n\
         options:\n\
           --warmup N                 unmeasured samples (default: 0)\n\
           --subscriber-count N      method metadata (default: 1)\n\
           --subscriber-mode NAME    subscriber behavior: fast, slow, stalled (default: fast)\n\
           --receive-delay-us N      subscriber delay after each sample (default: 0)\n\
           --timeout-ms N             open/receive/retry timeout (default: 30000)\n\
           --startup-delay-ms N       publisher startup delay (default: 1000)\n\
           --interval-us N             delay between sends (default: 0)\n\
           -h, --help                 show this help"
    )
}

/// Parses benchmark arguments after the executable name.
pub fn parse_args<I, S>(args: I) -> Result<BenchConfig, String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let mut args = args.into_iter().map(Into::into).peekable();
    let role = match args.next().as_deref() {
        Some("publisher") => Role::Publisher,
        Some("subscriber") => Role::Subscriber,
        Some(other) => return Err(format!("unknown process role {other:?}")),
        None => return Err("missing process role".to_owned()),
    };

    let mut service = None;
    let mut profile = None;
    let mut message = None;
    let mut payload_bytes = None;
    let mut count = None;
    let mut warmup = 0_u64;
    let mut subscriber_count = 1_u32;
    let mut subscriber_mode = SubscriberMode::Fast;
    let mut receive_delay_us = 0_u64;
    let mut timeout_ms = 30_000_u64;
    let mut startup_delay_ms = 1_000_u64;
    let mut interval_us = 0_u64;

    while let Some(argument) = args.next() {
        let (option, inline_value) = argument
            .split_once('=')
            .map_or((argument.as_str(), None), |(name, value)| {
                (name, Some(value))
            });

        let value = |args: &mut std::iter::Peekable<_>, name: &str, inline: Option<&str>| {
            inline
                .map(str::to_owned)
                .or_else(|| args.next())
                .ok_or_else(|| format!("missing value for {name}"))
        };

        match option {
            "--service" => {
                let value = value(&mut args, option, inline_value)?;
                if value.trim().is_empty() {
                    return Err("--service must not be empty".to_owned());
                }
                service = Some(value);
            }
            "--profile" => {
                let value = value(&mut args, option, inline_value)?;
                profile = Some(match value.as_str() {
                    "sensor" => Profile::Sensor,
                    "control" => Profile::Control,
                    other => return Err(format!("invalid --profile value {other:?}")),
                });
            }
            "--message" => {
                let value = value(&mut args, option, inline_value)?;
                message = Some(match value.as_str() {
                    "image" => MessageKind::Image,
                    "pointcloud" => MessageKind::PointCloud,
                    "control-command" => MessageKind::ControlCommand,
                    other => return Err(format!("invalid --message value {other:?}")),
                });
            }
            "--payload" => {
                payload_bytes = Some(parse_usize(
                    &value(&mut args, option, inline_value)?,
                    option,
                )?);
            }
            "--count" => {
                count = Some(parse_positive_u64(
                    &value(&mut args, option, inline_value)?,
                    option,
                )?);
            }
            "--warmup" => {
                warmup = parse_u64(&value(&mut args, option, inline_value)?, option)?;
            }
            "--subscriber-count" => {
                subscriber_count =
                    parse_positive_u32(&value(&mut args, option, inline_value)?, option)?;
            }
            "--subscriber-mode" => {
                let value = value(&mut args, option, inline_value)?;
                subscriber_mode = match value.as_str() {
                    "fast" => SubscriberMode::Fast,
                    "slow" => SubscriberMode::Slow,
                    "stalled" => SubscriberMode::Stalled,
                    other => return Err(format!("invalid --subscriber-mode value {other:?}")),
                };
            }
            "--receive-delay-us" => {
                receive_delay_us = parse_u64(&value(&mut args, option, inline_value)?, option)?;
            }
            "--timeout-ms" => {
                timeout_ms = parse_positive_u64(&value(&mut args, option, inline_value)?, option)?;
            }
            "--startup-delay-ms" => {
                startup_delay_ms = parse_u64(&value(&mut args, option, inline_value)?, option)?;
            }
            "--interval-us" => {
                interval_us = parse_u64(&value(&mut args, option, inline_value)?, option)?;
            }
            "-h" | "--help" => return Err(usage("rbt-bench")),
            option if option.starts_with('-') => {
                return Err(format!("unknown option {option:?}"));
            }
            positional => return Err(format!("unexpected positional argument {positional:?}")),
        }
    }

    let service = service.ok_or_else(|| "missing required --service".to_owned())?;
    let profile = profile.ok_or_else(|| "missing required --profile".to_owned())?;
    let message = message.unwrap_or(match profile {
        Profile::Sensor => MessageKind::Image,
        Profile::Control => MessageKind::ControlCommand,
    });
    let payload_bytes = payload_bytes.ok_or_else(|| "missing required --payload".to_owned())?;
    let count = count.ok_or_else(|| "missing required --count".to_owned())?;

    if matches!(profile, Profile::Control) && message != MessageKind::ControlCommand {
        return Err("the control profile requires --message control-command".to_owned());
    }
    if matches!(profile, Profile::Sensor) && message == MessageKind::ControlCommand {
        return Err("the sensor profile requires --message image or pointcloud".to_owned());
    }
    if message == MessageKind::ControlCommand && payload_bytes != std::mem::size_of::<u64>() * 2 {
        return Err(format!(
            "--payload must be 16 for the fixed ControlCommand profile, got {payload_bytes}"
        ));
    }
    let point_size = std::mem::size_of::<rbt_iox2::PointXYZI>();
    if message == MessageKind::PointCloud && payload_bytes % point_size != 0 {
        return Err(format!(
            "--payload must be divisible by {point_size} for PointXYZI elements, got {payload_bytes}"
        ));
    }
    if payload_bytes == 0 {
        return Err("--payload must be greater than zero".to_owned());
    }
    let _ = warmup
        .checked_add(count)
        .ok_or_else(|| "warmup plus count overflows u64".to_owned())?;

    if role == Role::Publisher && subscriber_mode != SubscriberMode::Fast {
        return Err("--subscriber-mode is only valid for a subscriber".to_owned());
    }
    if role == Role::Publisher && receive_delay_us != 0 {
        return Err("--receive-delay-us is only valid for a subscriber".to_owned());
    }
    if subscriber_mode == SubscriberMode::Stalled && receive_delay_us != 0 {
        return Err("--receive-delay-us cannot be used with --subscriber-mode stalled".to_owned());
    }

    Ok(BenchConfig {
        role,
        service,
        profile,
        message,
        payload_bytes,
        count,
        warmup,
        subscriber_count,
        subscriber_mode,
        receive_delay: Duration::from_micros(receive_delay_us),
        timeout: Duration::from_millis(timeout_ms),
        startup_delay: Duration::from_millis(startup_delay_ms),
        interval: Duration::from_micros(interval_us),
    })
}

fn parse_u64(value: &str, option: &str) -> Result<u64, String> {
    value
        .parse::<u64>()
        .map_err(|error| format!("invalid {option} value {value:?}: {error}"))
}

fn parse_positive_u64(value: &str, option: &str) -> Result<u64, String> {
    let parsed = parse_u64(value, option)?;
    if parsed == 0 {
        return Err(format!("{option} must be greater than zero"));
    }
    Ok(parsed)
}

fn parse_positive_u32(value: &str, option: &str) -> Result<u32, String> {
    let parsed = value
        .parse::<u32>()
        .map_err(|error| format!("invalid {option} value {value:?}: {error}"))?;
    if parsed == 0 {
        return Err(format!("{option} must be greater than zero"));
    }
    Ok(parsed)
}

fn parse_usize(value: &str, option: &str) -> Result<usize, String> {
    value
        .parse::<usize>()
        .map_err(|error| format!("invalid {option} value {value:?}: {error}"))
}

/// Computes a nearest-rank percentile from an arbitrary-order sample set.
///
/// Percentiles are reported using the nearest-rank rule: rank is
/// `ceil(percentile / 100 * n)`, clamped to the first element for zero
/// percentiles.  The input is not modified.
pub fn percentile(values: &[u64], percentile: f64) -> Option<u64> {
    if values.is_empty() || !percentile.is_finite() || !(0.0..=100.0).contains(&percentile) {
        return None;
    }

    let mut sorted = values.to_vec();
    sorted.sort_unstable();
    let rank = ((percentile / 100.0) * sorted.len() as f64).ceil() as usize;
    let index = rank.saturating_sub(1).min(sorted.len() - 1);
    Some(sorted[index])
}

/// Process-level CPU and resident-memory observations.
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq)]
pub struct ProcessStats {
    /// CPU time in nanoseconds, if the platform exposes it.
    pub cpu_time_ns: Option<u64>,
    /// Current resident set size in bytes, if available.
    pub rss_bytes: Option<u64>,
}

/// Reads process CPU time and RSS without affecting the benchmark payload.
pub fn process_stats() -> ProcessStats {
    #[cfg(target_os = "linux")]
    {
        ProcessStats {
            cpu_time_ns: process_cpu_time_ns(),
            rss_bytes: process_rss_bytes(),
        }
    }
    #[cfg(not(target_os = "linux"))]
    {
        ProcessStats::default()
    }
}

/// Returns CPU seconds between two process observations.
pub fn cpu_seconds_between(start: ProcessStats, end: ProcessStats) -> Option<f64> {
    Some(end.cpu_time_ns?.saturating_sub(start.cpu_time_ns?) as f64 / 1_000_000_000.0)
}

#[cfg(target_os = "linux")]
fn process_cpu_time_ns() -> Option<u64> {
    let mut time = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    // SAFETY: `time` points to writable storage for one timespec and the
    // clock identifier is a constant supplied by libc.
    let result = unsafe { libc::clock_gettime(libc::CLOCK_PROCESS_CPUTIME_ID, &mut time) };
    if result != 0 || time.tv_sec < 0 || time.tv_nsec < 0 {
        return None;
    }
    let seconds = u128::try_from(time.tv_sec).ok()?;
    let nanos = u128::try_from(time.tv_nsec).ok()?;
    u64::try_from(seconds.checked_mul(1_000_000_000)?.checked_add(nanos)?).ok()
}

#[cfg(target_os = "linux")]
fn process_rss_bytes() -> Option<u64> {
    let resident_pages = std::fs::read_to_string("/proc/self/statm")
        .ok()?
        .split_whitespace()
        .nth(1)?
        .parse::<u64>()
        .ok()?;
    // SAFETY: `sysconf` has no pointer arguments and `_SC_PAGESIZE` is a
    // valid POSIX Linux configuration key.
    let page_size = unsafe { libc::sysconf(libc::_SC_PAGESIZE) };
    if page_size <= 0 {
        return None;
    }
    resident_pages.checked_mul(u64::try_from(page_size).ok()?)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn percentile_uses_nearest_rank_without_mutating_input() {
        let values = [4, 1, 3, 2];
        assert_eq!(percentile(&values, 0.0), Some(1));
        assert_eq!(percentile(&values, 50.0), Some(2));
        assert_eq!(percentile(&values, 90.0), Some(4));
        assert_eq!(percentile(&values, 99.0), Some(4));
        assert_eq!(values, [4, 1, 3, 2]);
    }

    #[test]
    fn percentile_rejects_empty_or_invalid_inputs() {
        assert_eq!(percentile(&[], 50.0), None);
        assert_eq!(percentile(&[1], -1.0), None);
        assert_eq!(percentile(&[1], 101.0), None);
        assert_eq!(percentile(&[1], f64::NAN), None);
    }

    #[test]
    fn parser_accepts_required_options_and_equals_syntax() {
        let config = parse_args([
            "publisher",
            "--service=rbt/bench/test",
            "--profile",
            "sensor",
            "--payload=64",
            "--count",
            "5",
            "--warmup",
            "2",
            "--subscriber-count",
            "1",
            "--timeout-ms=2000",
            "--startup-delay-ms",
            "10",
            "--interval-us",
            "3",
        ])
        .expect("valid arguments");
        assert_eq!(config.role, Role::Publisher);
        assert_eq!(config.profile, Profile::Sensor);
        assert_eq!(config.message, MessageKind::Image);
        assert_eq!(config.payload_bytes, 64);
        assert_eq!(config.count, 5);
        assert_eq!(config.warmup, 2);
        assert_eq!(config.timeout, Duration::from_secs(2));
        assert_eq!(config.startup_delay, Duration::from_millis(10));
        assert_eq!(config.interval, Duration::from_micros(3));
    }

    #[test]
    fn parser_rejects_invalid_control_payload_and_missing_arguments() {
        let missing = parse_args(["publisher", "--service", "name"])
            .expect_err("profile/payload/count are required");
        assert!(missing.contains("missing required --profile"));

        let invalid_control = parse_args([
            "subscriber",
            "--service",
            "name",
            "--profile",
            "control",
            "--payload",
            "8",
            "--count",
            "1",
        ])
        .expect_err("ControlCommand is fixed-size");
        assert!(invalid_control.contains("must be 16"));

        let misaligned_pointcloud = parse_args([
            "publisher",
            "--service=x",
            "--profile=sensor",
            "--message=pointcloud",
            "--payload=17",
            "--count=1",
        ])
        .expect_err("PointCloud payload is an integral number of points");
        assert!(misaligned_pointcloud.contains("divisible by 16"));
    }

    #[test]
    fn parser_accepts_explicit_pointcloud_message() {
        let config = parse_args([
            "subscriber",
            "--service=rbt/bench/test/pointcloud",
            "--profile=sensor",
            "--message=pointcloud",
            "--payload=32",
            "--count=2",
        ])
        .expect("valid PointCloud benchmark arguments");

        assert_eq!(config.profile, Profile::Sensor);
        assert_eq!(config.message, MessageKind::PointCloud);
        assert_eq!(config.payload_bytes, 32);
    }

    #[test]
    fn parser_accepts_stalled_subscriber_and_rejects_behavior_on_publisher() {
        let config = parse_args([
            "subscriber",
            "--service=rbt/bench/test/stalled",
            "--profile=sensor",
            "--message=image",
            "--payload=64",
            "--count=1",
            "--subscriber-mode=stalled",
        ])
        .expect("valid stalled subscriber arguments");
        assert_eq!(config.subscriber_mode, SubscriberMode::Stalled);
        assert_eq!(config.receive_delay, Duration::ZERO);

        let error = parse_args([
            "publisher",
            "--service=rbt/bench/test/publisher",
            "--profile=sensor",
            "--payload=64",
            "--count=1",
            "--subscriber-mode=stalled",
        ])
        .expect_err("subscriber behavior must not apply to publisher");
        assert!(error.contains("only valid for a subscriber"));
    }

    #[test]
    fn image_payload_validation_checks_every_byte_and_is_reproducible() {
        let sequence = 7_u64;
        let payload: Vec<u8> = (0..32)
            .map(|index| sequence.wrapping_add(index) as u8)
            .collect();

        let first = validate_image_payload(&payload, sequence).expect("valid Image pattern");
        let second = validate_image_payload(&payload, sequence).expect("same Image pattern");
        assert_eq!(first, second);
        assert_ne!(first, PAYLOAD_FINGERPRINT_OFFSET);
    }

    #[test]
    fn image_payload_validation_reports_corruption_without_size_change() {
        let sequence = 11_u64;
        let mut payload: Vec<u8> = (0..32)
            .map(|index| sequence.wrapping_add(index) as u8)
            .collect();
        payload[13] ^= 0x80;

        let error = validate_image_payload(&payload, sequence).expect_err("corrupt Image");
        assert!(error.contains("byte 13"));
        assert!(error.contains("sequence 11"));
    }

    #[test]
    fn pointcloud_payload_validation_checks_float_bits_and_corruption() {
        let sequence = 3_u64;
        let mut payload: Vec<PointXYZI> = (0..4)
            .map(|index| PointXYZI {
                x: index as f32,
                y: sequence as f32,
                z: -(index as f32),
                intensity: (index & 0xff) as f32,
            })
            .collect();

        let first =
            validate_pointcloud_payload(&payload, sequence).expect("valid PointCloud pattern");
        let second =
            validate_pointcloud_payload(&payload, sequence).expect("same PointCloud pattern");
        assert_eq!(first, second);

        payload[2].intensity += 1.0;
        let error = validate_pointcloud_payload(&payload, sequence)
            .expect_err("corrupt PointCloud element");
        assert!(error.contains("element 2 field intensity"));
    }
}

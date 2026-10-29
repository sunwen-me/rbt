//! Shared command-line, validation, and statistics support for the process
//! separated native RPC benchmark.
//!
//! The benchmark deliberately uses the canonical `Frame`, `Detection`, and
//! `Image` message types generated from the repository IDL.  The values below
//! describe the benchmark workload; they are not a second payload ABI.

use std::{fmt, time::Duration};

pub use rbt_bench::{ProcessStats, cpu_seconds_between, percentile, process_stats};

/// The process role in one benchmark pair.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Role {
    /// Owns the native RPC server and handles requests.
    Server,
    /// Owns the native RPC client and measures round trips.
    Client,
}

impl Role {
    /// Stable machine-readable spelling.
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Server => "server",
            Self::Client => "client",
        }
    }
}

impl fmt::Display for Role {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.as_str())
    }
}

/// One benchmark payload shape.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum Case {
    /// Canonical `Frame` request and `Detection` response (16 B each).
    Frame16,
    /// Canonical `Image` slice in both directions (1 KiB each).
    Image1K1K,
    /// Canonical `Image` slice with a 1 KiB request and 1 MiB response.
    Image1K1M,
    /// Optional canonical `Image` diagnostic case with a 20 MiB response.
    Image1K20M,
}

impl Case {
    /// Stable command-line and output spelling.
    pub const fn as_str(self) -> &'static str {
        match self {
            Self::Frame16 => "frame-16",
            Self::Image1K1K => "image-1k-1k",
            Self::Image1K1M => "image-1k-1m",
            Self::Image1K20M => "image-1k-20m",
        }
    }

    /// Request payload bytes carried by this case.
    pub const fn request_bytes(self) -> usize {
        match self {
            Self::Frame16 => 16,
            Self::Image1K1K | Self::Image1K1M | Self::Image1K20M => 1_024,
        }
    }

    /// Response payload bytes carried by this case.
    pub const fn response_bytes(self) -> usize {
        match self {
            Self::Frame16 => 16,
            Self::Image1K1K => 1_024,
            Self::Image1K1M => 1_048_576,
            Self::Image1K20M => 20_971_520,
        }
    }

    /// Returns whether this case uses the canonical `Image` slice ABI.
    pub const fn is_dynamic(self) -> bool {
        !matches!(self, Self::Frame16)
    }

    /// Parses one stable case spelling.
    pub fn parse(value: &str) -> Result<Self, String> {
        match value {
            "frame-16" => Ok(Self::Frame16),
            "image-1k-1k" => Ok(Self::Image1K1K),
            "image-1k-1m" => Ok(Self::Image1K1M),
            "image-1k-20m" => Ok(Self::Image1K20M),
            _ => Err(format!(
                "invalid --case {value:?}; expected frame-16, image-1k-1k, image-1k-1m, or image-1k-20m"
            )),
        }
    }
}

impl fmt::Display for Case {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(self.as_str())
    }
}

/// Parsed benchmark process configuration.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Config {
    /// Process role.
    pub role: Role,
    /// Native iceoryx2 RPC service name.
    pub service: String,
    /// Payload case.
    pub case: Case,
    /// Number of measured calls.
    pub count: u64,
    /// Number of validated but unmeasured warmup calls.
    pub warmup: u64,
    /// Per-call receive deadline.
    pub timeout: Duration,
}

impl Config {
    /// Total calls the server must receive.
    pub fn total_calls(&self) -> Result<u64, String> {
        self.count
            .checked_add(self.warmup)
            .ok_or_else(|| "warmup plus count overflows u64".to_owned())
    }
}

/// A parse result that keeps `--help` out of the error path.
#[derive(Clone, Debug, Eq, PartialEq)]
pub enum ParseOutcome {
    /// Print usage and exit successfully.
    Help,
    /// Run the selected benchmark process.
    Run(Config),
    /// Print capability information and exit successfully.
    Capabilities,
}

/// Command-line usage text.
pub fn usage(program: &str) -> String {
    format!(
        "usage: {program} <server|client> --service SERVICE --case CASE --count N [options]\n\
         cases: frame-16, image-1k-1k, image-1k-1m, image-1k-20m\n\
         options:\n\
           --warmup N       validated but unmeasured calls (default: 3)\n\
           --timeout-ms N   per-call deadline (default: 30000)\n\
           --help           show this help\n\
         command: capabilities  report compiled-in dynamic case support"
    )
}

/// Parses process arguments after the executable name.
pub fn parse_args<I, S>(args: I) -> Result<ParseOutcome, String>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let mut args = args.into_iter().map(Into::into);
    let first = args
        .next()
        .ok_or_else(|| "missing role; expected server or client".to_owned())?;
    if first == "capabilities" {
        if args.next().is_some() {
            return Err("capabilities does not accept options".to_owned());
        }
        return Ok(ParseOutcome::Capabilities);
    }
    if matches!(first.as_str(), "--help" | "-h") {
        return Ok(ParseOutcome::Help);
    }
    let role = match first.as_str() {
        "server" => Role::Server,
        "client" => Role::Client,
        _ => return Err(format!("invalid role {first:?}; expected server or client")),
    };

    let mut service = None;
    let mut case = None;
    let mut count = None;
    let mut warmup = 3;
    let mut timeout_ms = 30_000;
    while let Some(option) = args.next() {
        let value = match option.as_str() {
            "--help" | "-h" => return Ok(ParseOutcome::Help),
            "--service" => args
                .next()
                .ok_or_else(|| "--service requires a value".to_owned())?,
            "--case" => args
                .next()
                .ok_or_else(|| "--case requires a value".to_owned())?,
            "--count" => {
                let value = args
                    .next()
                    .ok_or_else(|| "--count requires a value".to_owned())?;
                count = Some(parse_positive_u64(&value, "--count")?);
                continue;
            }
            "--warmup" => {
                let value = args
                    .next()
                    .ok_or_else(|| "--warmup requires a value".to_owned())?;
                warmup = parse_nonnegative_u64(&value, "--warmup")?;
                continue;
            }
            "--timeout-ms" => {
                let value = args
                    .next()
                    .ok_or_else(|| "--timeout-ms requires a value".to_owned())?;
                timeout_ms = parse_positive_u64(&value, "--timeout-ms")?;
                continue;
            }
            option => return Err(format!("unknown option {option:?}")),
        };
        if value.is_empty() {
            return Err(format!("{option} must not be empty"));
        }
        match option.as_str() {
            "--service" => service = Some(value),
            "--case" => case = Some(Case::parse(&value)?),
            _ => unreachable!("all value-bearing options handled above"),
        }
    }

    let service = service.ok_or_else(|| "missing required --service".to_owned())?;
    let case = case.ok_or_else(|| "missing required --case".to_owned())?;
    let count = count.ok_or_else(|| "missing required --count".to_owned())?;
    let timeout = Duration::from_millis(timeout_ms);
    Ok(ParseOutcome::Run(Config {
        role,
        service,
        case,
        count,
        warmup,
        timeout,
    }))
}

fn parse_positive_u64(value: &str, option: &str) -> Result<u64, String> {
    let parsed = value
        .parse::<u64>()
        .map_err(|error| format!("invalid {option} value {value:?}: {error}"))?;
    if parsed == 0 {
        return Err(format!("{option} must be greater than zero"));
    }
    Ok(parsed)
}

fn parse_nonnegative_u64(value: &str, option: &str) -> Result<u64, String> {
    value
        .parse::<u64>()
        .map_err(|error| format!("invalid {option} value {value:?}: {error}"))
}

/// Constructs a stable per-call nonce.
pub const fn nonce_for(ordinal: u64) -> u64 {
    0x5250_4342_454e_4348u64.wrapping_add(ordinal)
}

/// Deterministic request value used by both processes.
pub const fn value_for(nonce: u64) -> u32 {
    (nonce as u32).rotate_left(13) ^ 0x9e37_79b9
}

/// Deterministic operation result used by both processes.
pub const fn result_for(nonce: u64, value: u32) -> u32 {
    value
        .wrapping_mul(0x045d_9f3b)
        .wrapping_add(nonce as u32)
        .rotate_left(7)
}

/// Deterministic byte pattern used to exercise and validate dynamic payloads.
pub const fn payload_byte(seed: u64, index: usize) -> u8 {
    seed.wrapping_add(index as u64).rotate_left(17) as u8
}

/// FNV-1a checksum over the nonce, scalar value, and complete payload.
pub fn checksum(nonce: u64, value: u32, payload: &[u8]) -> u32 {
    let mut result = 2_166_136_261u32;
    for byte in nonce.to_le_bytes() {
        result = (result ^ u32::from(byte)).wrapping_mul(16_777_619);
    }
    for byte in value.to_le_bytes() {
        result = (result ^ u32::from(byte)).wrapping_mul(16_777_619);
    }
    for &byte in payload {
        result = (result ^ u32::from(byte)).wrapping_mul(16_777_619);
    }
    result
}

/// Packs a result and checksum into one canonical 64-bit field.
pub const fn pack_result_checksum(result: u32, checksum: u32) -> u64 {
    (result as u64) | ((checksum as u64) << 32)
}

/// Unpacks the result and checksum from one canonical 64-bit field.
pub const fn unpack_result_checksum(value: u64) -> (u32, u32) {
    (value as u32, (value >> 32) as u32)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_client_configuration_and_defaults() {
        let parsed = parse_args([
            "client",
            "--service",
            "rbt/test",
            "--case",
            "image-1k-1m",
            "--count",
            "7",
        ])
        .expect("arguments should parse");
        assert_eq!(
            parsed,
            ParseOutcome::Run(Config {
                role: Role::Client,
                service: "rbt/test".to_owned(),
                case: Case::Image1K1M,
                count: 7,
                warmup: 3,
                timeout: Duration::from_secs(30),
            })
        );
    }

    #[test]
    fn rejects_missing_and_invalid_arguments() {
        assert!(parse_args(["server"]).is_err());
        assert!(parse_args(["server", "--service", "x", "--case", "frame-16"]).is_err());
        assert!(
            parse_args([
                "server",
                "--service",
                "x",
                "--case",
                "frame-16",
                "--count",
                "0"
            ])
            .is_err()
        );
        assert!(
            parse_args([
                "server",
                "--service",
                "x",
                "--case",
                "unknown",
                "--count",
                "1"
            ])
            .is_err()
        );
    }

    #[test]
    fn percentile_uses_nearest_rank_without_mutating_samples() {
        let samples = [30, 10, 20, 40];
        assert_eq!(percentile(&samples, 0.0), Some(10));
        assert_eq!(percentile(&samples, 50.0), Some(20));
        assert_eq!(percentile(&samples, 90.0), Some(40));
        assert_eq!(percentile(&samples, 99.0), Some(40));
        assert_eq!(samples, [30, 10, 20, 40]);
        assert_eq!(percentile(&[], 50.0), None);
        assert_eq!(percentile(&[1], 101.0), None);
    }

    #[test]
    fn checksum_and_payload_validation_are_deterministic() {
        let mut payload = [0u8; 8];
        for (index, byte) in payload.iter_mut().enumerate() {
            *byte = payload_byte(9, index);
        }
        let first = checksum(7, 11, &payload);
        assert_eq!(first, checksum(7, 11, &payload));
        payload[3] ^= 1;
        assert_ne!(first, checksum(7, 11, &payload));
        assert_eq!(unpack_result_checksum(pack_result_checksum(4, 5)), (4, 5));
    }
}

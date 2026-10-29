#![forbid(unsafe_code)]

use std::{
    collections::BTreeMap,
    error::Error,
    fmt,
    io::Write,
    mem::{align_of, size_of},
    str::FromStr,
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

use iceoryx2::prelude::{
    BackpressureStrategy, CallbackProgression, EventId, NodeBuilder, PortFactory, ServiceName,
    ZeroCopySend, ipc_threadsafe,
};
use iceoryx2::service::Service;

const DEFAULT_TIMEOUT_MS: u64 = 10_000;
const DEFAULT_COUNT: usize = 10;
const DEFAULT_SERVICE: &str = "rbt/cross-host-upstream/plain/fixed";
const DEFAULT_EVENT_SERVICE: &str = "rbt/cross-host-upstream/plain/event";
const DEFAULT_BYTES_SERVICE: &str = "rbt/cross-host-upstream/plain/bytes";
const DEFAULT_IMAGE_SERVICE: &str = "rbt/cross-host-upstream/rbt/image";
const DEFAULT_POINTCLOUD_SERVICE: &str = "rbt/cross-host-upstream/rbt/pointcloud";
const DEFAULT_SIDECAR_SERVICE: &str = "rbt/cross-host-upstream/sidecar/sensor";
const DEFAULT_SENSOR_SERVICE: &str = "rbt/cross-host-upstream/sensor";
const DEFAULT_CHURN_SERVICE: &str = "rbt/cross-host-upstream/churn/same";
const DEFAULT_CHURN_PREFIX: &str = "rbt/cross-host-upstream/churn/unique";
const DEFAULT_OBSERVE_PREFIX: &str = "rbt/cross-host-upstream";
const DEFAULT_IMAGE_PAYLOAD: usize = 1_920 * 1_080 * 3;
const DEFAULT_POINTCLOUD_POINTS: usize = 100_000;
const DEFAULT_SIDECAR_BYTES: usize = 8;
const DEFAULT_SIDECAR_FINAL_OBSERVE_MS: u64 = 250;
// A local endpoint can appear before a remote gateway/importer has attached.
// This fixed startup grace is orchestration only; it never drives payload
// progress and is deliberately independent of the receive/publish loops.
const REMOTE_ATTACH_GRACE: Duration = Duration::from_millis(750);
const REMOTE_INTER_SEND_PACE: Duration = Duration::from_millis(30);
// Keep finite providers alive briefly after their final send so an imported
// mirror can drain the last sample before the service is destroyed. This is
// teardown orchestration only; it emits no additional payload or readiness.
const REMOTE_FINAL_DRAIN_GRACE: Duration = Duration::from_millis(500);

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ProbeError(String);

impl ProbeError {
    fn new(message: impl Into<String>) -> Self {
        Self(message.into())
    }
}

impl fmt::Display for ProbeError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl Error for ProbeError {}

pub type ProbeResult<T> = Result<T, Box<dyn Error + Send + Sync>>;

fn boxed_error(message: impl Into<String>) -> Box<dyn Error + Send + Sync> {
    Box::new(ProbeError::new(message))
}

fn invalid<T>(message: impl Into<String>) -> ProbeResult<T> {
    Err(boxed_error(message))
}

// Local copies of the rbt.sensor fixed header and element layouts. The derive
// macro emits the shared-memory-safe implementation without this probe having
// to depend on the production 0.9.3 crates.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, Eq, PartialEq, ZeroCopySend)]
#[type_name("rbt.sensor/PixelFormat@1")]
pub struct PixelFormat(pub u32);

impl PixelFormat {
    const RGB8: Self = Self(1);
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, ZeroCopySend)]
#[type_name("rbt.sensor/ImageHeader@1")]
pub struct ImageHeader {
    pub sequence: u64,
    pub timestamp_ns: u64,
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub encoding: PixelFormat,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, ZeroCopySend)]
#[type_name("rbt.sensor/PointCloudHeader@1")]
pub struct PointCloudHeader {
    pub sequence: u64,
    pub timestamp_ns: u64,
    pub width: u32,
    pub height: u32,
    pub dense: u8,
    pub reserved: [u8; 7],
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Default, ZeroCopySend)]
#[type_name("rbt.sensor/PointXYZI@1")]
pub struct PointXYZI {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub intensity: f32,
}

const _: () = {
    assert!(size_of::<ImageHeader>() == 32);
    assert!(align_of::<ImageHeader>() == 8);
    assert!(size_of::<PointCloudHeader>() == 32);
    assert!(align_of::<PointCloudHeader>() == 8);
    assert!(size_of::<PointXYZI>() == 16);
    assert!(align_of::<PointXYZI>() == 4);
};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ImageLayout {
    Rgb1080p,
}

impl ImageLayout {
    pub const fn name(self) -> &'static str {
        "1920x1080-rgb8"
    }

    pub const fn dimensions(self) -> (u32, u32, u32) {
        (1_920, 1_080, 1_920 * 3)
    }

    pub const fn payload_bytes(self) -> usize {
        DEFAULT_IMAGE_PAYLOAD
    }
}

impl FromStr for ImageLayout {
    type Err = ProbeError;

    fn from_str(value: &str) -> Result<Self, Self::Err> {
        match value {
            "1920x1080-rgb8" | "1080p" => Ok(Self::Rgb1080p),
            _ => Err(ProbeError::new(format!(
                "invalid --layout {value:?}; use 1920x1080-rgb8"
            ))),
        }
    }
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct BasicConfig {
    pub service: String,
    pub count: usize,
    pub timeout_ms: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct BytesConfig {
    pub service: String,
    pub payload_bytes: usize,
    pub count: usize,
    pub timeout_ms: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ImageConfig {
    pub service: String,
    pub layout: ImageLayout,
    pub count: usize,
    pub timeout_ms: u64,
    pub pace_ms: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct PointCloudConfig {
    pub service: String,
    pub points: usize,
    pub count: usize,
    pub timeout_ms: u64,
    pub pace_ms: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SensorConfig {
    pub service: String,
    pub count: usize,
    pub timeout_ms: u64,
    pub pace_ms: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct HoldConfig {
    pub service: String,
    pub duration_ms: u64,
    pub timeout_ms: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct SidecarConfig {
    pub service: String,
    pub count: usize,
    pub payload_bytes: usize,
    pub ready_prefill: usize,
    pub ready_prefill_gap_ms: u64,
    pub timeout_ms: u64,
    pub pace_ms: u64,
    pub final_observe_ms: u64,
    pub suppress_final_ready: bool,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ChurnConfig {
    pub name: String,
    pub cycles: usize,
    pub hold_ms: u64,
    pub gap_ms: u64,
    pub timeout_ms: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ObserveConfig {
    pub prefix: String,
    pub timeout_ms: u64,
    pub poll_ms: u64,
}

#[derive(Clone, Debug, Eq, PartialEq)]
pub enum Command {
    Help,
    FixedPublisher(BasicConfig),
    FixedSubscriber(BasicConfig),
    EventNotifier(BasicConfig),
    EventListener(BasicConfig),
    BytesPublisher(BytesConfig),
    BytesSubscriber(BytesConfig),
    ImagePublisher(ImageConfig),
    ImageSubscriber(ImageConfig),
    PointCloudPublisher(PointCloudConfig),
    PointCloudSubscriber(PointCloudConfig),
    SensorPublisher(SensorConfig),
    SensorSubscriber(SensorConfig),
    ProviderHold(HoldConfig),
    SidecarPublisher(SidecarConfig),
    SidecarSubscriber(SidecarConfig),
    ChurnSame(ChurnConfig),
    ChurnUnique(ChurnConfig),
    ServiceObserve(ObserveConfig),
}

#[derive(Debug, Clone, Eq, PartialEq)]
pub struct ParseError(String);

impl ParseError {
    fn new(message: impl Into<String>) -> Self {
        Self(message.into())
    }
}

impl fmt::Display for ParseError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl Error for ParseError {}

fn parse_flags(
    args: &[String],
    allowed: &[&str],
    boolean_allowed: &[&str],
) -> Result<BTreeMap<String, String>, ParseError> {
    let mut values = BTreeMap::new();
    let mut index = 0;
    while index < args.len() {
        let token = &args[index];
        if token == "--help" {
            return Err(ParseError::new("help requested"));
        }
        let Some(stripped) = token.strip_prefix("--") else {
            return Err(ParseError::new(format!(
                "unexpected argument {token:?}; options must start with --"
            )));
        };
        let (key, inline_value) = match stripped.split_once('=') {
            Some((key, value)) => (key, Some(value.to_owned())),
            None => (stripped, None),
        };
        if !allowed.contains(&key) {
            return Err(ParseError::new(format!("unknown option --{key}")));
        }
        if values.contains_key(key) {
            return Err(ParseError::new(format!("duplicate option --{key}")));
        }
        let value = match inline_value {
            Some(value) if !value.is_empty() => value,
            Some(_) => return Err(ParseError::new(format!("option --{key} needs a value"))),
            None if boolean_allowed.contains(&key) => "true".to_owned(),
            None => {
                index += 1;
                let Some(value) = args.get(index) else {
                    return Err(ParseError::new(format!("option --{key} needs a value")));
                };
                if value.starts_with("--") {
                    return Err(ParseError::new(format!("option --{key} needs a value")));
                }
                value.clone()
            }
        };
        values.insert(key.to_owned(), value);
        index += 1;
    }
    Ok(values)
}

fn option_string(values: &BTreeMap<String, String>, key: &str, default: &str) -> String {
    values
        .get(key)
        .cloned()
        .unwrap_or_else(|| default.to_owned())
}

fn option_usize(
    values: &BTreeMap<String, String>,
    key: &str,
    default: usize,
) -> Result<usize, ParseError> {
    let value = option_string(values, key, &default.to_string());
    value
        .parse::<usize>()
        .map_err(|error| ParseError::new(format!("invalid --{key} value {value:?}: {error}")))
}

fn option_u64(
    values: &BTreeMap<String, String>,
    key: &str,
    default: u64,
) -> Result<u64, ParseError> {
    let value = option_string(values, key, &default.to_string());
    value
        .parse::<u64>()
        .map_err(|error| ParseError::new(format!("invalid --{key} value {value:?}: {error}")))
}

fn option_bool(
    values: &BTreeMap<String, String>,
    key: &str,
    default: bool,
) -> Result<bool, ParseError> {
    let value = option_string(values, key, if default { "true" } else { "false" });
    value
        .parse::<bool>()
        .map_err(|error| ParseError::new(format!("invalid --{key} value {value:?}: {error}")))
}

fn checked_service(values: &BTreeMap<String, String>, default: &str) -> Result<String, ParseError> {
    let service = option_string(values, "service", default);
    if service.is_empty() {
        return Err(ParseError::new("--service must not be empty"));
    }
    Ok(service)
}

fn common_config(
    values: &BTreeMap<String, String>,
    default_service: &str,
    default_count: usize,
) -> Result<BasicConfig, ParseError> {
    Ok(BasicConfig {
        service: checked_service(values, default_service)?,
        count: option_usize(values, "count", default_count)?,
        timeout_ms: option_u64(values, "timeout-ms", DEFAULT_TIMEOUT_MS)?,
    })
}

fn image_config(
    values: &BTreeMap<String, String>,
    default_service: &str,
) -> Result<ImageConfig, ParseError> {
    let layout = option_string(values, "layout", ImageLayout::Rgb1080p.name())
        .parse::<ImageLayout>()
        .map_err(|error| ParseError::new(error.to_string()))?;
    Ok(ImageConfig {
        service: checked_service(values, default_service)?,
        layout,
        count: option_usize(values, "count", 1)?,
        timeout_ms: option_u64(values, "timeout-ms", DEFAULT_TIMEOUT_MS)?,
        pace_ms: option_u64(values, "pace-ms", 5)?,
    })
}

fn pointcloud_config(
    values: &BTreeMap<String, String>,
    default_service: &str,
) -> Result<PointCloudConfig, ParseError> {
    Ok(PointCloudConfig {
        service: checked_service(values, default_service)?,
        points: option_usize(values, "points", DEFAULT_POINTCLOUD_POINTS)?,
        count: option_usize(values, "count", 1)?,
        timeout_ms: option_u64(values, "timeout-ms", DEFAULT_TIMEOUT_MS)?,
        pace_ms: option_u64(values, "pace-ms", 2)?,
    })
}

fn sensor_config(
    values: &BTreeMap<String, String>,
    default_service: &str,
) -> Result<SensorConfig, ParseError> {
    let count = option_usize(values, "count", DEFAULT_COUNT)?;
    if count == 0 {
        return Err(ParseError::new(
            "--count must be greater than zero for sensor",
        ));
    }
    Ok(SensorConfig {
        service: checked_service(values, default_service)?,
        count,
        timeout_ms: option_u64(values, "timeout-ms", DEFAULT_TIMEOUT_MS)?,
        pace_ms: option_u64(values, "pace-ms", 5)?,
    })
}

fn sidecar_config(
    values: &BTreeMap<String, String>,
    default_service: &str,
) -> Result<SidecarConfig, ParseError> {
    let payload_bytes = option_usize(values, "payload-bytes", DEFAULT_SIDECAR_BYTES)?;
    if payload_bytes < size_of::<u64>() {
        return Err(ParseError::new("--payload-bytes must be at least 8"));
    }
    Ok(SidecarConfig {
        service: checked_service(values, default_service)?,
        count: option_usize(values, "count", 10_000)?,
        payload_bytes,
        ready_prefill: option_usize(values, "ready-prefill", 0)?,
        ready_prefill_gap_ms: option_u64(values, "ready-prefill-gap-ms", 0)?,
        timeout_ms: option_u64(values, "timeout-ms", DEFAULT_TIMEOUT_MS)?,
        pace_ms: option_u64(values, "pace-ms", 0)?,
        final_observe_ms: option_u64(values, "final-observe-ms", DEFAULT_SIDECAR_FINAL_OBSERVE_MS)?,
        suppress_final_ready: option_bool(values, "suppress-final-ready", false)?,
    })
}

fn churn_config(
    values: &BTreeMap<String, String>,
    default_name: &str,
) -> Result<ChurnConfig, ParseError> {
    let key = if values.contains_key("prefix") {
        "prefix"
    } else {
        "service"
    };
    let name = option_string(values, key, default_name);
    if name.is_empty() {
        return Err(ParseError::new("--service/--prefix must not be empty"));
    }
    Ok(ChurnConfig {
        name,
        cycles: option_usize(values, "cycles", 10)?,
        hold_ms: option_u64(values, "hold-ms", 0)?,
        gap_ms: option_u64(values, "gap-ms", 0)?,
        timeout_ms: option_u64(values, "timeout-ms", DEFAULT_TIMEOUT_MS)?,
    })
}

/// Parses command-line arguments after the executable name.
pub fn parse_args<I, S>(args: I) -> Result<Command, ParseError>
where
    I: IntoIterator<Item = S>,
    S: Into<String>,
{
    let args: Vec<String> = args.into_iter().map(Into::into).collect();
    let Some(subcommand) = args.first() else {
        return Ok(Command::Help);
    };
    if subcommand == "--help" || subcommand == "help" {
        return Ok(Command::Help);
    }
    if args.iter().any(|argument| argument == "--help") {
        return Ok(Command::Help);
    }
    let options = &args[1..];
    let no_bool: &[&str] = &[];
    match subcommand.as_str() {
        "fixed-pub" | "fixed-publisher" => Ok(Command::FixedPublisher(common_config(
            &parse_flags(options, &["service", "count", "timeout-ms"], no_bool)?,
            DEFAULT_SERVICE,
            DEFAULT_COUNT,
        )?)),
        "fixed-sub" | "fixed-subscriber" => Ok(Command::FixedSubscriber(common_config(
            &parse_flags(options, &["service", "count", "timeout-ms"], no_bool)?,
            DEFAULT_SERVICE,
            DEFAULT_COUNT,
        )?)),
        "event-notify" | "event-notifier" => Ok(Command::EventNotifier(common_config(
            &parse_flags(options, &["service", "count", "timeout-ms"], no_bool)?,
            DEFAULT_EVENT_SERVICE,
            DEFAULT_COUNT,
        )?)),
        "event-listen" | "event-listener" => Ok(Command::EventListener(common_config(
            &parse_flags(options, &["service", "count", "timeout-ms"], no_bool)?,
            DEFAULT_EVENT_SERVICE,
            DEFAULT_COUNT,
        )?)),
        "bytes-pub" | "bytes-publisher" | "dynamic-pub" => {
            let values = parse_flags(
                options,
                &["service", "payload-bytes", "count", "timeout-ms"],
                no_bool,
            )?;
            Ok(Command::BytesPublisher(BytesConfig {
                service: checked_service(&values, DEFAULT_BYTES_SERVICE)?,
                payload_bytes: option_usize(&values, "payload-bytes", 1_024)?,
                count: option_usize(&values, "count", DEFAULT_COUNT)?,
                timeout_ms: option_u64(&values, "timeout-ms", DEFAULT_TIMEOUT_MS)?,
            }))
        }
        "bytes-sub" | "bytes-subscriber" | "dynamic-sub" => {
            let values = parse_flags(
                options,
                &["service", "payload-bytes", "count", "timeout-ms"],
                no_bool,
            )?;
            Ok(Command::BytesSubscriber(BytesConfig {
                service: checked_service(&values, DEFAULT_BYTES_SERVICE)?,
                payload_bytes: option_usize(&values, "payload-bytes", 1_024)?,
                count: option_usize(&values, "count", DEFAULT_COUNT)?,
                timeout_ms: option_u64(&values, "timeout-ms", DEFAULT_TIMEOUT_MS)?,
            }))
        }
        "image-pub" | "image-publisher" => Ok(Command::ImagePublisher(image_config(
            &parse_flags(
                options,
                &["service", "layout", "count", "timeout-ms", "pace-ms"],
                no_bool,
            )?,
            DEFAULT_IMAGE_SERVICE,
        )?)),
        "image-sub" | "image-subscriber" => Ok(Command::ImageSubscriber(image_config(
            &parse_flags(
                options,
                &["service", "layout", "count", "timeout-ms", "pace-ms"],
                no_bool,
            )?,
            DEFAULT_IMAGE_SERVICE,
        )?)),
        "pointcloud-pub" | "pointcloud-publisher" | "point-pub" => {
            Ok(Command::PointCloudPublisher(pointcloud_config(
                &parse_flags(
                    options,
                    &["service", "points", "count", "timeout-ms", "pace-ms"],
                    no_bool,
                )?,
                DEFAULT_POINTCLOUD_SERVICE,
            )?))
        }
        "pointcloud-sub" | "pointcloud-subscriber" | "point-sub" => {
            Ok(Command::PointCloudSubscriber(pointcloud_config(
                &parse_flags(
                    options,
                    &["service", "points", "count", "timeout-ms", "pace-ms"],
                    no_bool,
                )?,
                DEFAULT_POINTCLOUD_SERVICE,
            )?))
        }
        "sensor-pub" | "sensor-publisher" => Ok(Command::SensorPublisher(sensor_config(
            &parse_flags(
                options,
                &["service", "count", "timeout-ms", "pace-ms"],
                no_bool,
            )?,
            DEFAULT_SENSOR_SERVICE,
        )?)),
        "sensor-sub" | "sensor-subscriber" => Ok(Command::SensorSubscriber(sensor_config(
            &parse_flags(
                options,
                &["service", "count", "timeout-ms", "pace-ms"],
                no_bool,
            )?,
            DEFAULT_SENSOR_SERVICE,
        )?)),
        "provider-hold" | "hold" => {
            let values = parse_flags(options, &["service", "duration-ms", "timeout-ms"], no_bool)?;
            Ok(Command::ProviderHold(HoldConfig {
                service: checked_service(&values, DEFAULT_SERVICE)?,
                duration_ms: option_u64(&values, "duration-ms", 30_000)?,
                timeout_ms: option_u64(&values, "timeout-ms", DEFAULT_TIMEOUT_MS)?,
            }))
        }
        "sidecar-pub" | "sidecar-publisher" => Ok(Command::SidecarPublisher(sidecar_config(
            &parse_flags(
                options,
                &[
                    "service",
                    "count",
                    "payload-bytes",
                    "ready-prefill",
                    "ready-prefill-gap-ms",
                    "timeout-ms",
                    "pace-ms",
                    "final-observe-ms",
                    "suppress-final-ready",
                ],
                &["suppress-final-ready"],
            )?,
            DEFAULT_SIDECAR_SERVICE,
        )?)),
        "sidecar-sub" | "sidecar-subscriber" => Ok(Command::SidecarSubscriber(sidecar_config(
            &parse_flags(
                options,
                &[
                    "service",
                    "count",
                    "payload-bytes",
                    "ready-prefill",
                    "ready-prefill-gap-ms",
                    "timeout-ms",
                    "pace-ms",
                    "final-observe-ms",
                    "suppress-final-ready",
                ],
                &["suppress-final-ready"],
            )?,
            DEFAULT_SIDECAR_SERVICE,
        )?)),
        "churn-same" => Ok(Command::ChurnSame(churn_config(
            &parse_flags(
                options,
                &["service", "cycles", "hold-ms", "gap-ms", "timeout-ms"],
                no_bool,
            )?,
            DEFAULT_CHURN_SERVICE,
        )?)),
        "churn-unique" => Ok(Command::ChurnUnique(churn_config(
            &parse_flags(
                options,
                &["prefix", "cycles", "hold-ms", "gap-ms", "timeout-ms"],
                no_bool,
            )?,
            DEFAULT_CHURN_PREFIX,
        )?)),
        "service-observe" | "observe" => {
            let values = parse_flags(options, &["prefix", "timeout-ms", "poll-ms"], no_bool)?;
            let prefix = option_string(&values, "prefix", DEFAULT_OBSERVE_PREFIX);
            if prefix.is_empty() {
                return Err(ParseError::new("--prefix must not be empty"));
            }
            Ok(Command::ServiceObserve(ObserveConfig {
                prefix,
                timeout_ms: option_u64(&values, "timeout-ms", DEFAULT_TIMEOUT_MS)?,
                poll_ms: option_u64(&values, "poll-ms", 100)?,
            }))
        }
        other => Err(ParseError::new(format!(
            "unknown subcommand {other:?}; use --help"
        ))),
    }
}

/// Prints the command reference. Normal command output is intentionally a
/// single-line READY/SUMMARY record so shell verifiers can parse it safely.
pub fn print_help() {
    println!(
        "cross-host-upstream-probe: iceoryx2 =0.10.0 local IPC capability probe\n\
No network transport, custom protocol, heartbeat, lease, serialization, or retransmission is used.\n\
\n\
SUBCOMMANDS\n\
  fixed-pub / fixed-sub       fixed u64 publish-subscribe\n\
  event-notify / event-listen plain Event notifier/listener\n\
  bytes-pub / bytes-sub       dynamic [u8] publish-subscribe\n\
  image-pub / image-sub       1920x1080 Image-like dynamic slice with rbt.sensor/ImageHeader@1\n\
  pointcloud-pub / pointcloud-sub PointCloud-like dynamic slice; --points supports 20 MiB payloads\n\
  sensor-pub / sensor-sub     bounded Sensor-profile u64 stream with sequence-gap reporting\n\
  provider-hold               hold a u64 provider alive for --duration-ms\n\
  sidecar-pub / sidecar-sub   readiness Event plus event-driven dynamic payload drain\n\
  churn-same / churn-unique   create/drop one or many services repeatedly\n\
  service-observe             official Service::list observer for a name prefix\n\
\n\
COMMON OPTIONS\n\
  --service NAME --count N --timeout-ms N\n\
IMAGE/POINTCLOUD OPTIONS\n\
  --layout 1920x1080-rgb8 --points N --pace-ms N\n\
SENSOR OPTIONS\n\
  --pace-ms N\n\
SIDECAR OPTIONS\n\
  --payload-bytes N --ready-prefill N --ready-prefill-gap-ms N\n\
  --pace-ms N --final-observe-ms N\n\
  --suppress-final-ready     omit readiness for the final payload\n\
CHURN/OBSERVER OPTIONS\n\
  --cycles N --hold-ms N --gap-ms N --prefix NAME --poll-ms N"
    );
}

fn service_name(value: &str) -> ProbeResult<ServiceName> {
    value
        .try_into()
        .map_err(|error| boxed_error(format!("invalid service name {value:?}: {error}")))
}

fn deadline_after(timeout_ms: u64) -> ProbeResult<Instant> {
    Instant::now()
        .checked_add(Duration::from_millis(timeout_ms))
        .ok_or_else(|| boxed_error(format!("timeout {timeout_ms}ms is too large")))
}

fn sleep_until(deadline: Instant, maximum: Duration) {
    let remaining = deadline.saturating_duration_since(Instant::now());
    if !remaining.is_zero() {
        thread::sleep(remaining.min(maximum));
    }
}

fn emit(line: impl AsRef<str>) {
    println!("{}", line.as_ref());
    let _ = std::io::stdout().flush();
}

fn realtime_ms() -> ProbeResult<u128> {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|duration| duration.as_millis())
        .map_err(|error| boxed_error(format!("system clock is before Unix epoch: {error}")))
}

fn wait_for_at_least<F>(deadline: Instant, what: &str, mut count: F) -> ProbeResult<usize>
where
    F: FnMut() -> usize,
{
    loop {
        let current = count();
        if current > 0 {
            return Ok(current);
        }
        if Instant::now() >= deadline {
            return invalid(format!("timed out waiting for {what}"));
        }
        sleep_until(deadline, Duration::from_millis(2));
    }
}

fn timestamp_for(sequence: u64) -> u64 {
    sequence.saturating_mul(1_000_000)
}

fn stepped_checksum(start: u8, step: u8, length: usize) -> u64 {
    let complete_periods = length / 256;
    let remainder = length % 256;
    let mut checksum = (complete_periods as u64) * ((255 * 256) / 2);
    for index in 0..remainder {
        checksum = checksum.wrapping_add(u64::from(
            start.wrapping_add(step.wrapping_mul(index as u8)),
        ));
    }
    checksum
}

pub fn dynamic_byte(sequence: u64, index: usize) -> u8 {
    sequence
        .wrapping_mul(13)
        .wrapping_add((index as u64).wrapping_mul(17))
        .wrapping_add(7) as u8
}

pub fn dynamic_checksum(sequence: u64, payload_len: usize) -> u64 {
    stepped_checksum(
        sequence.wrapping_mul(13).wrapping_add(7) as u8,
        17,
        payload_len,
    )
}

pub fn image_byte(sequence: u64, index: usize) -> u8 {
    sequence.wrapping_add(index as u64) as u8
}

pub fn image_checksum(sequence: u64, payload_len: usize) -> u64 {
    stepped_checksum(sequence as u8, 1, payload_len)
}

pub fn point_at(index: usize) -> PointXYZI {
    let value = index as f32;
    PointXYZI {
        x: value,
        y: value * 2.0,
        z: -value,
        intensity: (index % 31) as f32,
    }
}

pub fn point_checksum(point: PointXYZI) -> u64 {
    u64::from(point.x.to_bits())
        .wrapping_mul(31)
        .wrapping_add(u64::from(point.y.to_bits()))
        .wrapping_mul(31)
        .wrapping_add(u64::from(point.z.to_bits()))
        .wrapping_mul(31)
        .wrapping_add(u64::from(point.intensity.to_bits()))
}

pub fn point_cloud_checksum(count: usize) -> u64 {
    (0..count).fold(0_u64, |checksum, index| {
        checksum.wrapping_add(point_checksum(point_at(index)))
    })
}

fn selected_expectations<F>(payload_len: usize, value_at: F) -> String
where
    F: Fn(usize) -> u8,
{
    if payload_len == 0 {
        return "none".to_owned();
    }
    [0, payload_len / 2, payload_len - 1]
        .into_iter()
        .map(|index| format!("{index}:{}", value_at(index)))
        .collect::<Vec<_>>()
        .join(",")
}

fn validate_selected_bytes<F>(payload: &[u8], value_at: F) -> ProbeResult<String>
where
    F: Fn(usize) -> u8,
{
    let selected = selected_expectations(payload.len(), &value_at);
    for index in [0, payload.len() / 2, payload.len().saturating_sub(1)] {
        if payload.is_empty() {
            break;
        }
        let expected = value_at(index);
        if payload[index] != expected {
            return invalid(format!(
                "selected byte mismatch index={index} expected={expected} actual={}",
                payload[index]
            ));
        }
    }
    Ok(selected)
}

fn sidecar_payload(sequence: u64, payload_len: usize) -> Vec<u8> {
    let mut payload = vec![0_u8; payload_len];
    payload[..size_of::<u64>()].copy_from_slice(&sequence.to_le_bytes());
    for (index, byte) in payload[size_of::<u64>()..].iter_mut().enumerate() {
        *byte = dynamic_byte(sequence, index + size_of::<u64>());
    }
    payload
}

fn sidecar_checksum(sequence: u64, payload_len: usize) -> u64 {
    sidecar_payload(sequence, payload_len)
        .into_iter()
        .fold(0_u64, |sum, byte| sum.wrapping_add(u64::from(byte)))
}

fn publish_fixed(config: BasicConfig) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let name = service_name(&config.service)?;
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let factory = node
        .service_builder(&name)
        .publish_subscribe::<u64>()
        .enable_safe_overflow(false)
        .subscriber_max_buffer_size(config.count.max(1))
        .history_size(0)
        .open_or_create()?;
    let publisher = factory
        .publisher_builder()
        .max_loaned_samples(2)
        .backpressure_strategy(BackpressureStrategy::DiscardData)
        .create()?;
    emit(format!(
        "READY role=publisher kind=plain-fixed service={} count={} payload_type=u64",
        config.service, config.count
    ));
    let connected = wait_for_at_least(deadline, "plain-fixed subscriber", || {
        factory.dynamic_config().number_of_subscribers()
    })?;
    thread::sleep(REMOTE_ATTACH_GRACE);
    let mut deliveries = 0usize;
    for sequence in 0..config.count {
        if Instant::now() >= deadline {
            return invalid(format!(
                "plain-fixed publish deadline at sequence={sequence}"
            ));
        }
        let recipients = publisher
            .loan_uninit()?
            .write_payload(sequence as u64)
            .send()?;
        if recipients == 0 {
            return invalid(format!(
                "plain-fixed sequence={sequence} was delivered to zero subscribers"
            ));
        }
        deliveries = deliveries.saturating_add(recipients);
        if sequence + 1 < config.count {
            thread::sleep(REMOTE_INTER_SEND_PACE);
        }
    }
    thread::sleep(REMOTE_FINAL_DRAIN_GRACE);
    emit(format!(
        "SUMMARY role=publisher kind=plain-fixed service={} count={} connected={} deliveries={} payload_type=u64",
        config.service, config.count, connected, deliveries
    ));
    Ok(())
}

fn open_fixed_subscriber(
    node: &iceoryx2::node::Node<ipc_threadsafe::Service>,
    name: &ServiceName,
    count: usize,
    deadline: Instant,
) -> ProbeResult<iceoryx2::port::subscriber::Subscriber<ipc_threadsafe::Service, u64, ()>> {
    loop {
        // Imported mirrors are created with receiver-side service defaults; omit explicit
        // service settings and bind only this local endpoint.
        match node.service_builder(name).publish_subscribe::<u64>().open() {
            Ok(factory) => {
                return Ok(factory
                    .subscriber_builder()
                    .buffer_size(count.clamp(1, 2))
                    .create()?);
            }
            Err(error) if Instant::now() < deadline => {
                let _ = error;
                sleep_until(deadline, Duration::from_millis(2));
            }
            Err(error) => {
                return invalid(format!("timed out opening plain-fixed service: {error}"));
            }
        }
    }
}

fn subscribe_fixed(config: BasicConfig) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let name = service_name(&config.service)?;
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let subscriber = open_fixed_subscriber(&node, &name, config.count, deadline)?;
    emit(format!(
        "READY role=subscriber kind=plain-fixed service={} count={} payload_type=u64",
        config.service, config.count
    ));
    let mut received = 0usize;
    let mut checksum = 0_u64;
    while received < config.count {
        if let Some(sample) = subscriber.receive()? {
            let actual = *sample.payload();
            let expected = received as u64;
            if actual != expected {
                return invalid(format!(
                    "plain-fixed sequence mismatch index={received} expected={expected} actual={actual}"
                ));
            }
            checksum = checksum.wrapping_add(actual);
            received += 1;
        } else if Instant::now() >= deadline {
            return invalid(format!(
                "timed out receiving plain-fixed samples received={} expected={}",
                received, config.count
            ));
        } else {
            sleep_until(deadline, Duration::from_millis(2));
        }
    }
    emit(format!(
        "SUMMARY role=subscriber kind=plain-fixed service={} count={} received={} checksum={} payload_type=u64",
        config.service, config.count, received, checksum
    ));
    Ok(())
}

fn publish_sensor(config: SensorConfig) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let name = service_name(&config.service)?;
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let factory = node
        .service_builder(&name)
        .publish_subscribe::<u64>()
        .enable_safe_overflow(true)
        .subscriber_max_buffer_size(1)
        .history_size(0)
        .open_or_create()?;
    let publisher = factory
        .publisher_builder()
        .max_loaned_samples(2)
        .backpressure_strategy(BackpressureStrategy::DiscardData)
        .create()?;
    emit(format!(
        "READY role=publisher kind=sensor service={} count={} pace_ms={} payload_type=u64",
        config.service, config.count, config.pace_ms
    ));
    let connected = wait_for_at_least(deadline, "sensor subscriber", || {
        factory.dynamic_config().number_of_subscribers()
    })?;
    thread::sleep(REMOTE_ATTACH_GRACE);
    let mut sent = 0usize;
    let mut deliveries = 0usize;
    for sequence in 0..config.count {
        if Instant::now() >= deadline {
            return invalid(format!("sensor publish deadline at sequence={sequence}"));
        }
        let sequence = u64::try_from(sequence)
            .map_err(|_| boxed_error("sensor sequence exceeds u64 range"))?;
        let recipients = publisher.loan_uninit()?.write_payload(sequence).send()?;
        sent = sent.saturating_add(1);
        deliveries = deliveries.saturating_add(recipients);
        emit(format!(
            "SENSOR_SEND sequence={sequence} realtime_ms={}",
            realtime_ms()?
        ));
        if config.pace_ms > 0 {
            thread::sleep(Duration::from_millis(config.pace_ms));
        }
    }
    thread::sleep(REMOTE_FINAL_DRAIN_GRACE);
    emit(format!(
        "SUMMARY role=publisher kind=sensor service={} count={} sent={} deliveries={}",
        config.service, config.count, sent, deliveries
    ));
    let _ = connected;
    Ok(())
}

fn open_sensor_subscriber(
    node: &iceoryx2::node::Node<ipc_threadsafe::Service>,
    name: &ServiceName,
    config: &SensorConfig,
    deadline: Instant,
) -> ProbeResult<iceoryx2::port::subscriber::Subscriber<ipc_threadsafe::Service, u64, ()>> {
    loop {
        // Imported mirrors are created with receiver-side service defaults; omit explicit
        // service settings and bind only this local endpoint.
        match node.service_builder(name).publish_subscribe::<u64>().open() {
            Ok(factory) => {
                return Ok(factory
                    .subscriber_builder()
                    .buffer_size(config.count.clamp(1, 2))
                    .create()?);
            }
            Err(error) if Instant::now() < deadline => {
                let _ = error;
                sleep_until(deadline, Duration::from_millis(2));
            }
            Err(error) => return invalid(format!("timed out opening sensor service: {error}")),
        }
    }
}

fn subscribe_sensor(config: SensorConfig) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let final_sequence = u64::try_from(config.count - 1)
        .map_err(|_| boxed_error("sensor final sequence exceeds u64 range"))?;
    let name = service_name(&config.service)?;
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let subscriber = open_sensor_subscriber(&node, &name, &config, deadline)?;
    emit(format!(
        "READY role=subscriber kind=sensor service={} count={} pace_ms={} payload_type=u64",
        config.service, config.count, config.pace_ms
    ));

    let mut received = 0usize;
    let mut gap_events = 0usize;
    let mut estimated_missing = 0u64;
    let mut first = None;
    let mut last = None;
    while last != Some(final_sequence) {
        if let Some(sample) = subscriber.receive()? {
            let sequence = *sample.payload();
            emit(format!(
                "SENSOR_SAMPLE sequence={sequence} realtime_ms={}",
                realtime_ms()?
            ));
            if let Some(previous) = last {
                if sequence <= previous {
                    return invalid(format!(
                        "sensor sequence is not strictly increasing previous={previous} actual={sequence}"
                    ));
                }
                if sequence > previous.saturating_add(1) {
                    gap_events = gap_events.saturating_add(1);
                    estimated_missing = estimated_missing
                        .saturating_add(sequence.saturating_sub(previous).saturating_sub(1));
                }
            } else if sequence > 0 {
                gap_events = gap_events.saturating_add(1);
                estimated_missing = sequence;
            }
            if sequence > final_sequence {
                return invalid(format!(
                    "sensor final sequence={final_sequence} never arrived; received out-of-range sequence={sequence}"
                ));
            }
            first = Some(first.unwrap_or(sequence));
            last = Some(sequence);
            received = received.saturating_add(1);
        } else if Instant::now() >= deadline {
            return invalid(format!(
                "timed out waiting for sensor final sequence={final_sequence} received={received}"
            ));
        } else {
            sleep_until(deadline, Duration::from_millis(2));
        }
    }
    emit(format!(
        "SUMMARY role=subscriber kind=sensor service={} count={} received={} gap_events={} estimated_missing={} first={} last={}",
        config.service,
        config.count,
        received,
        gap_events,
        estimated_missing,
        first.expect("sensor final sequence implies a first sample"),
        last.expect("sensor final sequence implies a last sample")
    ));
    Ok(())
}

fn notify_events(config: BasicConfig) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let name = service_name(&config.service)?;
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let factory = node
        .service_builder(&name)
        .event()
        .event_id_max_value(config.count.max(1))
        .open_or_create()?;
    let notifier = factory.notifier_builder().create()?;
    emit(format!(
        "READY role=notifier kind=plain-event service={} count={} payload_type=Event",
        config.service, config.count
    ));
    let connected = wait_for_at_least(deadline, "plain-event listener", || {
        factory.dynamic_config().number_of_listeners()
    })?;
    thread::sleep(REMOTE_ATTACH_GRACE);
    let mut notifications = 0usize;
    for sequence in 0..config.count {
        let listeners = notifier.notify_with_custom_event_id(EventId::new(sequence + 1))?;
        if listeners == 0 {
            return invalid(format!(
                "plain-event id={} reached zero listeners",
                sequence + 1
            ));
        }
        notifications = notifications.saturating_add(listeners);
    }
    thread::sleep(REMOTE_FINAL_DRAIN_GRACE);
    emit(format!(
        "SUMMARY role=notifier kind=plain-event service={} count={} connected={} notifications={} payload_type=Event",
        config.service, config.count, connected, notifications
    ));
    Ok(())
}

fn listen_events(config: BasicConfig) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let name = service_name(&config.service)?;
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let factory = loop {
        match node.service_builder(&name).event().open() {
            Ok(factory) => break factory,
            Err(error) if Instant::now() < deadline => {
                let _ = error;
                sleep_until(deadline, Duration::from_millis(2));
            }
            Err(error) => {
                return invalid(format!("timed out opening plain-event service: {error}"));
            }
        }
    };
    let listener = factory.listener_builder().create()?;
    emit(format!(
        "READY role=listener kind=plain-event service={} count={} payload_type=Event",
        config.service, config.count
    ));
    let mut received = 0usize;
    let mut checksum = 0_u64;
    while received < config.count {
        let timeout = deadline.saturating_duration_since(Instant::now());
        if timeout.is_zero() {
            return invalid(format!(
                "timed out receiving plain-event IDs received={} expected={}",
                received, config.count
            ));
        }
        let mut invalid_id = None;
        let notifications = listener.timed_wait(
            |activation| {
                let expected = received + 1;
                if activation.id.as_value() != expected {
                    invalid_id = Some((expected, activation.id.as_value()));
                }
                received = received.saturating_add(activation.count as usize);
                checksum = checksum.wrapping_add(
                    activation
                        .id
                        .as_value()
                        .saturating_mul(activation.count as usize) as u64,
                );
            },
            timeout,
        )?;
        if let Some((expected, actual)) = invalid_id {
            return invalid(format!(
                "plain-event ID mismatch expected={expected} actual={actual}"
            ));
        }
        if notifications == 0 {
            return invalid(format!(
                "timed out receiving plain-event IDs received={} expected={}",
                received, config.count
            ));
        }
    }
    emit(format!(
        "SUMMARY role=listener kind=plain-event service={} count={} received={} checksum={} payload_type=Event",
        config.service, config.count, received, checksum
    ));
    Ok(())
}

fn publish_bytes(config: BytesConfig) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let name = service_name(&config.service)?;
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let factory = node
        .service_builder(&name)
        .publish_subscribe::<[u8]>()
        .enable_safe_overflow(false)
        .subscriber_max_buffer_size(config.count.max(1))
        .history_size(0)
        .open_or_create()?;
    let publisher = factory
        .publisher_builder()
        .initial_max_slice_len(config.payload_bytes.max(1))
        .max_loaned_samples(2)
        .backpressure_strategy(BackpressureStrategy::DiscardData)
        .create()?;
    emit(format!(
        "READY role=publisher kind=plain-bytes service={} count={} payload_bytes={} payload_type=[u8]",
        config.service, config.count, config.payload_bytes
    ));
    let connected = wait_for_at_least(deadline, "plain-bytes subscriber", || {
        factory.dynamic_config().number_of_subscribers()
    })?;
    thread::sleep(REMOTE_ATTACH_GRACE);
    let mut deliveries = 0usize;
    let mut checksum = 0_u64;
    for sequence in 0..config.count {
        let sequence = sequence as u64;
        let sample = publisher
            .loan_slice_uninit(config.payload_bytes)?
            .write_from_fn(|index| dynamic_byte(sequence, index));
        let sample_checksum = dynamic_checksum(sequence, config.payload_bytes);
        let recipients = sample.send()?;
        if recipients == 0 {
            return invalid(format!(
                "plain-bytes sequence={sequence} reached zero subscribers"
            ));
        }
        checksum = checksum.wrapping_add(sample_checksum);
        deliveries = deliveries.saturating_add(recipients);
        if sequence + 1 < config.count as u64 {
            thread::sleep(REMOTE_INTER_SEND_PACE);
        }
    }
    thread::sleep(REMOTE_FINAL_DRAIN_GRACE);
    emit(format!(
        "SUMMARY role=publisher kind=plain-bytes service={} count={} connected={} payload_bytes={} checksum={} deliveries={} payload_type=[u8]",
        config.service, config.count, connected, config.payload_bytes, checksum, deliveries
    ));
    Ok(())
}

fn open_bytes_subscriber(
    node: &iceoryx2::node::Node<ipc_threadsafe::Service>,
    name: &ServiceName,
    config: &BytesConfig,
    deadline: Instant,
) -> ProbeResult<iceoryx2::port::subscriber::Subscriber<ipc_threadsafe::Service, [u8], ()>> {
    loop {
        match node
            .service_builder(name)
            .publish_subscribe::<[u8]>()
            .open()
        {
            Ok(factory) => {
                return Ok(factory
                    .subscriber_builder()
                    .buffer_size(config.count.clamp(1, 2))
                    .create()?);
            }
            Err(error) if Instant::now() < deadline => {
                let _ = error;
                sleep_until(deadline, Duration::from_millis(2));
            }
            Err(error) => {
                return invalid(format!("timed out opening plain-bytes service: {error}"));
            }
        }
    }
}

fn subscribe_bytes(config: BytesConfig) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let name = service_name(&config.service)?;
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let subscriber = open_bytes_subscriber(&node, &name, &config, deadline)?;
    emit(format!(
        "READY role=subscriber kind=plain-bytes service={} count={} payload_bytes={} payload_type=[u8]",
        config.service, config.count, config.payload_bytes
    ));
    let mut received = 0usize;
    let mut checksum = 0_u64;
    let mut selected = "none".to_owned();
    while received < config.count {
        if let Some(sample) = subscriber.receive()? {
            let payload = sample.payload();
            if payload.len() != config.payload_bytes {
                return invalid(format!(
                    "plain-bytes payload length mismatch sequence={} expected={} actual={}",
                    received,
                    config.payload_bytes,
                    payload.len()
                ));
            }
            let expected = dynamic_checksum(received as u64, payload.len());
            let actual = payload
                .iter()
                .fold(0_u64, |sum, byte| sum.wrapping_add(u64::from(*byte)));
            if expected != actual {
                return invalid(format!(
                    "plain-bytes checksum mismatch sequence={} expected={expected} actual={actual}",
                    received
                ));
            }
            selected =
                validate_selected_bytes(payload, |index| dynamic_byte(received as u64, index))?;
            checksum = checksum.wrapping_add(actual);
            received += 1;
        } else if Instant::now() >= deadline {
            return invalid(format!(
                "timed out receiving plain-bytes samples received={} expected={}",
                received, config.count
            ));
        } else {
            sleep_until(deadline, Duration::from_millis(2));
        }
    }
    emit(format!(
        "SUMMARY role=subscriber kind=plain-bytes service={} count={} received={} payload_bytes={} checksum={} selected={} payload_type=[u8]",
        config.service, config.count, received, config.payload_bytes, checksum, selected
    ));
    Ok(())
}

fn publish_image(config: ImageConfig) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let payload_bytes = config.layout.payload_bytes();
    let name = service_name(&config.service)?;
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let factory = node
        .service_builder(&name)
        .publish_subscribe::<[u8]>()
        .user_header::<ImageHeader>()
        .enable_safe_overflow(false)
        .subscriber_max_buffer_size(config.count.max(1))
        .history_size(0)
        .open_or_create()?;
    let publisher = factory
        .publisher_builder()
        .initial_max_slice_len(payload_bytes)
        .max_loaned_samples(2)
        .backpressure_strategy(BackpressureStrategy::DiscardData)
        .create()?;
    emit(format!(
        "READY role=publisher kind=rbt-image service={} count={} layout={} payload_bytes={} user_header_type=rbt.sensor/ImageHeader@1",
        config.service,
        config.count,
        config.layout.name(),
        payload_bytes
    ));
    let connected = wait_for_at_least(deadline, "Image subscriber", || {
        factory.dynamic_config().number_of_subscribers()
    })?;
    thread::sleep(REMOTE_ATTACH_GRACE);
    let (width, height, stride) = config.layout.dimensions();
    let mut deliveries = 0usize;
    let mut checksum = 0_u64;
    for sequence in 0..config.count {
        let sequence = sequence as u64;
        if Instant::now() >= deadline {
            return invalid(format!("Image publish deadline at sequence={sequence}"));
        }
        let mut sample = publisher.loan_slice_uninit(payload_bytes)?;
        *sample.user_header_mut() = ImageHeader {
            sequence,
            timestamp_ns: timestamp_for(sequence),
            width,
            height,
            stride,
            encoding: PixelFormat::RGB8,
        };
        let sample = sample.write_from_fn(|index| image_byte(sequence, index));
        let recipients = sample.send()?;
        if recipients == 0 {
            return invalid(format!(
                "Image sequence={sequence} reached zero subscribers"
            ));
        }
        checksum = checksum.wrapping_add(image_checksum(sequence, payload_bytes));
        deliveries = deliveries.saturating_add(recipients);
        if config.pace_ms > 0 {
            thread::sleep(Duration::from_millis(config.pace_ms));
        }
    }
    thread::sleep(REMOTE_FINAL_DRAIN_GRACE);
    emit(format!(
        "SUMMARY role=publisher kind=rbt-image service={} count={} connected={} layout={} payload_bytes={} checksum={} deliveries={} user_header_type=rbt.sensor/ImageHeader@1",
        config.service,
        config.count,
        connected,
        config.layout.name(),
        payload_bytes,
        checksum,
        deliveries
    ));
    Ok(())
}

fn open_image_subscriber(
    node: &iceoryx2::node::Node<ipc_threadsafe::Service>,
    name: &ServiceName,
    config: &ImageConfig,
    deadline: Instant,
) -> ProbeResult<iceoryx2::port::subscriber::Subscriber<ipc_threadsafe::Service, [u8], ImageHeader>>
{
    loop {
        match node
            .service_builder(name)
            .publish_subscribe::<[u8]>()
            .user_header::<ImageHeader>()
            .open()
        {
            Ok(factory) => {
                return Ok(factory
                    .subscriber_builder()
                    .buffer_size(config.count.clamp(1, 2))
                    .create()?);
            }
            Err(error) if Instant::now() < deadline => {
                let _ = error;
                sleep_until(deadline, Duration::from_millis(2));
            }
            Err(error) => return invalid(format!("timed out opening Image service: {error}")),
        }
    }
}

fn subscribe_image(config: ImageConfig) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let payload_bytes = config.layout.payload_bytes();
    let name = service_name(&config.service)?;
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let subscriber = open_image_subscriber(&node, &name, &config, deadline)?;
    emit(format!(
        "READY role=subscriber kind=rbt-image service={} count={} layout={} payload_bytes={} user_header_type=rbt.sensor/ImageHeader@1",
        config.service,
        config.count,
        config.layout.name(),
        payload_bytes
    ));
    let (width, height, stride) = config.layout.dimensions();
    let mut received = 0usize;
    let mut checksum = 0_u64;
    let mut selected = "none".to_owned();
    while received < config.count {
        if let Some(sample) = subscriber.receive()? {
            let sequence = received as u64;
            let header = sample.user_header();
            if header.sequence != sequence
                || header.timestamp_ns != timestamp_for(sequence)
                || header.width != width
                || header.height != height
                || header.stride != stride
                || header.encoding != PixelFormat::RGB8
            {
                return invalid(format!(
                    "Image header mismatch sequence={sequence} actual={header:?}"
                ));
            }
            let payload = sample.payload();
            if payload.len() != payload_bytes {
                return invalid(format!(
                    "Image payload length mismatch sequence={sequence} expected={payload_bytes} actual={}",
                    payload.len()
                ));
            }
            let expected = image_checksum(sequence, payload_bytes);
            let actual = payload
                .iter()
                .fold(0_u64, |sum, byte| sum.wrapping_add(u64::from(*byte)));
            if expected != actual {
                return invalid(format!(
                    "Image checksum mismatch sequence={sequence} expected={expected} actual={actual}"
                ));
            }
            selected = validate_selected_bytes(payload, |index| image_byte(sequence, index))?;
            checksum = checksum.wrapping_add(actual);
            received += 1;
        } else if Instant::now() >= deadline {
            return invalid(format!(
                "timed out receiving Images received={} expected={}",
                received, config.count
            ));
        } else {
            sleep_until(deadline, Duration::from_millis(2));
        }
    }
    emit(format!(
        "SUMMARY role=subscriber kind=rbt-image service={} count={} received={} layout={} payload_bytes={} checksum={} selected={} user_header_type=rbt.sensor/ImageHeader@1",
        config.service,
        config.count,
        received,
        config.layout.name(),
        payload_bytes,
        checksum,
        selected
    ));
    Ok(())
}

fn publish_pointcloud(config: PointCloudConfig) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let name = service_name(&config.service)?;
    let payload_bytes = config.points.saturating_mul(size_of::<PointXYZI>());
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let factory = node
        .service_builder(&name)
        .publish_subscribe::<[PointXYZI]>()
        .user_header::<PointCloudHeader>()
        .enable_safe_overflow(false)
        .subscriber_max_buffer_size(config.count.max(1))
        .history_size(0)
        .open_or_create()?;
    let publisher = factory
        .publisher_builder()
        .initial_max_slice_len(config.points.max(1))
        .max_loaned_samples(2)
        .backpressure_strategy(BackpressureStrategy::DiscardData)
        .create()?;
    emit(format!(
        "READY role=publisher kind=rbt-pointcloud service={} count={} points={} payload_bytes={} user_header_type=rbt.sensor/PointCloudHeader@1 element_type=rbt.sensor/PointXYZI@1",
        config.service, config.count, config.points, payload_bytes
    ));
    let connected = wait_for_at_least(deadline, "PointCloud subscriber", || {
        factory.dynamic_config().number_of_subscribers()
    })?;
    thread::sleep(REMOTE_ATTACH_GRACE);
    let mut deliveries = 0usize;
    let checksum = point_cloud_checksum(config.points);
    for sequence in 0..config.count {
        let sequence = sequence as u64;
        if Instant::now() >= deadline {
            return invalid(format!(
                "PointCloud publish deadline at sequence={sequence}"
            ));
        }
        let mut sample = publisher.loan_slice_uninit(config.points)?;
        *sample.user_header_mut() = PointCloudHeader {
            sequence,
            timestamp_ns: timestamp_for(sequence),
            width: u32::try_from(config.points)
                .map_err(|_| boxed_error("point count exceeds u32"))?,
            height: 1,
            dense: 1,
            reserved: [0; 7],
        };
        let sample = sample.write_from_fn(point_at);
        let recipients = sample.send()?;
        if recipients == 0 {
            return invalid(format!(
                "PointCloud sequence={sequence} reached zero subscribers"
            ));
        }
        deliveries = deliveries.saturating_add(recipients);
        if config.pace_ms > 0 {
            thread::sleep(Duration::from_millis(config.pace_ms));
        }
    }
    thread::sleep(REMOTE_FINAL_DRAIN_GRACE);
    emit(format!(
        "SUMMARY role=publisher kind=rbt-pointcloud service={} count={} connected={} points={} payload_bytes={} checksum={} deliveries={} user_header_type=rbt.sensor/PointCloudHeader@1 element_type=rbt.sensor/PointXYZI@1",
        config.service, config.count, connected, config.points, payload_bytes, checksum, deliveries
    ));
    Ok(())
}

fn open_pointcloud_subscriber(
    node: &iceoryx2::node::Node<ipc_threadsafe::Service>,
    name: &ServiceName,
    config: &PointCloudConfig,
    deadline: Instant,
) -> ProbeResult<
    iceoryx2::port::subscriber::Subscriber<ipc_threadsafe::Service, [PointXYZI], PointCloudHeader>,
> {
    loop {
        match node
            .service_builder(name)
            .publish_subscribe::<[PointXYZI]>()
            .user_header::<PointCloudHeader>()
            .open()
        {
            Ok(factory) => {
                return Ok(factory
                    .subscriber_builder()
                    .buffer_size(config.count.clamp(1, 2))
                    .create()?);
            }
            Err(error) if Instant::now() < deadline => {
                let _ = error;
                sleep_until(deadline, Duration::from_millis(2));
            }
            Err(error) => return invalid(format!("timed out opening PointCloud service: {error}")),
        }
    }
}

fn same_point(actual: &PointXYZI, expected: PointXYZI) -> bool {
    actual.x.to_bits() == expected.x.to_bits()
        && actual.y.to_bits() == expected.y.to_bits()
        && actual.z.to_bits() == expected.z.to_bits()
        && actual.intensity.to_bits() == expected.intensity.to_bits()
}

fn subscribe_pointcloud(config: PointCloudConfig) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let name = service_name(&config.service)?;
    let payload_bytes = config.points.saturating_mul(size_of::<PointXYZI>());
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let subscriber = open_pointcloud_subscriber(&node, &name, &config, deadline)?;
    emit(format!(
        "READY role=subscriber kind=rbt-pointcloud service={} count={} points={} payload_bytes={} user_header_type=rbt.sensor/PointCloudHeader@1 element_type=rbt.sensor/PointXYZI@1",
        config.service, config.count, config.points, payload_bytes
    ));
    let expected_checksum = point_cloud_checksum(config.points);
    let mut received = 0usize;
    let mut checksum = 0_u64;
    while received < config.count {
        if let Some(sample) = subscriber.receive()? {
            let sequence = received as u64;
            let header = sample.user_header();
            let expected_width =
                u32::try_from(config.points).map_err(|_| boxed_error("point count exceeds u32"))?;
            if header.sequence != sequence
                || header.timestamp_ns != timestamp_for(sequence)
                || header.width != expected_width
                || header.height != 1
                || header.dense != 1
                || header.reserved != [0; 7]
            {
                return invalid(format!(
                    "PointCloud header mismatch sequence={sequence} actual={header:?}"
                ));
            }
            let payload = sample.payload();
            if payload.len() != config.points {
                return invalid(format!(
                    "PointCloud count mismatch sequence={sequence} expected={} actual={}",
                    config.points,
                    payload.len()
                ));
            }
            if let Some(first) = payload.first()
                && !same_point(first, point_at(0))
            {
                return invalid(format!(
                    "PointCloud first point mismatch sequence={sequence} actual={first:?}"
                ));
            }
            if let Some(last) = payload.last()
                && !same_point(last, point_at(config.points.saturating_sub(1)))
            {
                return invalid(format!(
                    "PointCloud last point mismatch sequence={sequence} actual={last:?}"
                ));
            }
            let actual_checksum = payload
                .iter()
                .copied()
                .fold(0_u64, |sum, point| sum.wrapping_add(point_checksum(point)));
            if actual_checksum != expected_checksum {
                return invalid(format!(
                    "PointCloud checksum mismatch sequence={sequence} expected={expected_checksum} actual={actual_checksum}"
                ));
            }
            checksum = checksum.wrapping_add(actual_checksum);
            received += 1;
        } else if Instant::now() >= deadline {
            return invalid(format!(
                "timed out receiving PointClouds received={} expected={}",
                received, config.count
            ));
        } else {
            sleep_until(deadline, Duration::from_millis(2));
        }
    }
    emit(format!(
        "SUMMARY role=subscriber kind=rbt-pointcloud service={} count={} received={} points={} payload_bytes={} checksum={} user_header_type=rbt.sensor/PointCloudHeader@1 element_type=rbt.sensor/PointXYZI@1",
        config.service, config.count, received, config.points, payload_bytes, checksum
    ));
    Ok(())
}

fn provider_hold(config: HoldConfig) -> ProbeResult<()> {
    let _deadline = deadline_after(config.timeout_ms)?;
    let name = service_name(&config.service)?;
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let factory = node
        .service_builder(&name)
        .publish_subscribe::<u64>()
        .open_or_create()?;
    let _publisher = factory.publisher_builder().create()?;
    emit(format!(
        "READY role=provider kind=provider-hold service={} duration_ms={} payload_type=u64",
        config.service, config.duration_ms
    ));
    thread::sleep(Duration::from_millis(config.duration_ms));
    emit(format!(
        "SUMMARY role=provider kind=provider-hold service={} duration_ms={} held=true payload_type=u64",
        config.service, config.duration_ms
    ));
    Ok(())
}

fn readiness_service(value: &str) -> ProbeResult<ServiceName> {
    service_name(&format!("{value}/ready"))
}

fn sidecar_publisher(config: SidecarConfig) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let name = service_name(&config.service)?;
    let ready_name = readiness_service(&config.service)?;
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let factory = node
        .service_builder(&name)
        .publish_subscribe::<[u8]>()
        .enable_safe_overflow(true)
        .subscriber_max_buffer_size(1)
        .history_size(0)
        .open_or_create()?;
    let ready_factory = node
        .service_builder(&ready_name)
        .event()
        .event_id_max_value(1)
        .open_or_create()?;
    let publisher = factory
        .publisher_builder()
        .initial_max_slice_len(config.payload_bytes)
        .max_loaned_samples(2)
        .backpressure_strategy(BackpressureStrategy::DiscardData)
        .create()?;
    let notifier = ready_factory.notifier_builder().create()?;
    let connected = wait_for_at_least(deadline, "sidecar subscriber", || {
        factory.dynamic_config().number_of_subscribers()
    })?;
    let ready_connected = wait_for_at_least(deadline, "sidecar readiness listener", || {
        ready_factory.dynamic_config().number_of_listeners()
    })?;
    thread::sleep(REMOTE_ATTACH_GRACE);
    emit(format!(
        "READY role=publisher kind=sidecar service={} ready_service={}/ready count={} payload_bytes={} connected={} ready_connected={} payload_type=[u8]",
        config.service,
        config.service,
        config.count,
        config.payload_bytes,
        connected,
        ready_connected
    ));

    let mut prefill_ok = 0usize;
    let mut prefill_errors = 0usize;
    for _ in 0..config.ready_prefill {
        match notifier.notify_with_custom_event_id(EventId::new(1)) {
            Ok(_) => prefill_ok = prefill_ok.saturating_add(1),
            Err(_) => prefill_errors = prefill_errors.saturating_add(1),
        }
    }
    if config.ready_prefill_gap_ms > 0 {
        thread::sleep(Duration::from_millis(config.ready_prefill_gap_ms));
    }
    let mut payload_ok = 0usize;
    let mut payload_errors = 0usize;
    let mut payload_deliveries = 0usize;
    let mut payload_ready_ok = 0usize;
    let mut payload_ready_errors = 0usize;
    let mut checksum = 0_u64;
    for sequence in 0..config.count {
        if Instant::now() >= deadline {
            return invalid(format!("sidecar publish deadline at sequence={sequence}"));
        }
        let sequence_u64 = sequence as u64;
        let payload = sidecar_payload(sequence_u64, config.payload_bytes);
        match publisher
            .loan_slice_uninit(config.payload_bytes)?
            .write_from_fn(|index| payload[index])
            .send()
        {
            Ok(recipients) => {
                payload_ok = payload_ok.saturating_add(1);
                payload_deliveries = payload_deliveries.saturating_add(recipients);
                checksum =
                    checksum.wrapping_add(sidecar_checksum(sequence_u64, config.payload_bytes));
                let suppress = config.suppress_final_ready && sequence + 1 == config.count;
                if !suppress {
                    match notifier.notify_with_custom_event_id(EventId::new(1)) {
                        Ok(_) => payload_ready_ok = payload_ready_ok.saturating_add(1),
                        Err(_) => payload_ready_errors = payload_ready_errors.saturating_add(1),
                    }
                }
            }
            Err(_) => payload_errors = payload_errors.saturating_add(1),
        }
        if config.pace_ms > 0 {
            thread::sleep(Duration::from_millis(config.pace_ms));
        }
    }
    thread::sleep(REMOTE_FINAL_DRAIN_GRACE);
    emit(format!(
        "SUMMARY role=publisher kind=sidecar service={} ready_service={}/ready count={} payload_bytes={} payload_ok={} payload_errors={} payload_deliveries={} payload_checksum={} ready_prefill={} ready_prefill_gap_ms={} ready_prefill_ok={} ready_prefill_errors={} payload_ready_ok={} payload_ready_errors={} readiness_ok={} readiness_errors={} suppress_final_ready={} queue_depth=1 safe_overflow=true payload_type=[u8]",
        config.service,
        config.service,
        config.count,
        config.payload_bytes,
        payload_ok,
        payload_errors,
        payload_deliveries,
        checksum,
        config.ready_prefill,
        config.ready_prefill_gap_ms,
        prefill_ok,
        prefill_errors,
        payload_ready_ok,
        payload_ready_errors,
        prefill_ok.saturating_add(payload_ready_ok),
        prefill_errors.saturating_add(payload_ready_errors),
        config.suppress_final_ready
    ));
    Ok(())
}

fn open_sidecar_subscriber(
    node: &iceoryx2::node::Node<ipc_threadsafe::Service>,
    name: &ServiceName,
    _config: &SidecarConfig,
    deadline: Instant,
) -> ProbeResult<iceoryx2::port::subscriber::Subscriber<ipc_threadsafe::Service, [u8], ()>> {
    loop {
        match node
            .service_builder(name)
            .publish_subscribe::<[u8]>()
            .open()
        {
            Ok(factory) => {
                return Ok(factory.subscriber_builder().buffer_size(1).create()?);
            }
            Err(error) if Instant::now() < deadline => {
                let _ = error;
                sleep_until(deadline, Duration::from_millis(2));
            }
            Err(error) => return invalid(format!("timed out opening sidecar service: {error}")),
        }
    }
}

fn open_sidecar_listener(
    node: &iceoryx2::node::Node<ipc_threadsafe::Service>,
    name: &ServiceName,
    deadline: Instant,
) -> ProbeResult<iceoryx2::port::listener::Listener<ipc_threadsafe::Service>> {
    loop {
        match node.service_builder(name).event().open() {
            Ok(factory) => return Ok(factory.listener_builder().create()?),
            Err(error) if Instant::now() < deadline => {
                let _ = error;
                sleep_until(deadline, Duration::from_millis(2));
            }
            Err(error) => {
                return invalid(format!(
                    "timed out opening sidecar readiness service: {error}"
                ));
            }
        }
    }
}

fn validate_sidecar_payload(payload: &[u8], payload_bytes: usize) -> ProbeResult<(u64, u64)> {
    if payload.len() != payload_bytes {
        return invalid(format!(
            "sidecar payload length mismatch expected={payload_bytes} actual={}",
            payload.len()
        ));
    }
    let sequence = u64::from_le_bytes(
        payload[..size_of::<u64>()]
            .try_into()
            .map_err(|_| boxed_error("sidecar payload is shorter than sequence"))?,
    );
    let expected_checksum = sidecar_checksum(sequence, payload_bytes);
    let actual_checksum = payload
        .iter()
        .fold(0_u64, |sum, byte| sum.wrapping_add(u64::from(*byte)));
    if expected_checksum != actual_checksum {
        return invalid(format!(
            "sidecar checksum mismatch sequence={sequence} expected={expected_checksum} actual={actual_checksum}"
        ));
    }
    Ok((sequence, actual_checksum))
}

fn sidecar_subscriber(config: SidecarConfig) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let name = service_name(&config.service)?;
    let ready_name = service_name(&format!("{}/ready", config.service))?;
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let subscriber = open_sidecar_subscriber(&node, &name, &config, deadline)?;
    let listener = open_sidecar_listener(&node, &ready_name, deadline)?;
    emit(format!(
        "READY role=subscriber kind=sidecar service={} ready_service={}/ready count={} payload_bytes={} strategy=event-driven payload_type=[u8]",
        config.service, config.service, config.count, config.payload_bytes
    ));

    let mut wakes = 0usize;
    let mut notifications = 0u64;
    let mut drain_cycles = 0usize;
    let mut samples = 0usize;
    let mut spurious_wakes = 0usize;
    let mut checksum = 0_u64;
    let mut wrong_event = None;
    while samples < config.count && Instant::now() < deadline {
        let remaining = deadline.saturating_duration_since(Instant::now());
        let mut activation_count = 0_u64;
        let got = listener.timed_wait(
            |activation| {
                if activation.id.as_value() != 1 {
                    wrong_event = Some(activation.id.as_value());
                }
                activation_count = activation_count.saturating_add(activation.count);
            },
            remaining,
        )?;
        if got == 0 || activation_count == 0 {
            break;
        }
        wakes = wakes.saturating_add(1);
        notifications = notifications.saturating_add(activation_count);
        let mut drained = 0usize;
        drain_cycles = drain_cycles.saturating_add(1);
        while let Some(sample) = subscriber.receive()? {
            let (_, sample_checksum) =
                validate_sidecar_payload(sample.payload(), config.payload_bytes)?;
            checksum = checksum.wrapping_add(sample_checksum);
            samples = samples.saturating_add(1);
            drained = drained.saturating_add(1);
        }
        if drained == 0 {
            spurious_wakes = spurious_wakes.saturating_add(1);
        }
    }
    if let Some(event_id) = wrong_event {
        return invalid(format!(
            "sidecar readiness received unexpected event id={event_id}"
        ));
    }

    // This is intentionally after the event-driven phase. It is diagnostic
    // only and cannot make subscriber progress; it exposes a payload stranded
    // behind a dropped/coalesced final readiness notification.
    let mut remaining_after_last_wake = 0usize;
    while let Some(sample) = subscriber.receive()? {
        let (_, sample_checksum) =
            validate_sidecar_payload(sample.payload(), config.payload_bytes)?;
        checksum = checksum.wrapping_add(sample_checksum);
        remaining_after_last_wake = remaining_after_last_wake.saturating_add(1);
    }
    thread::sleep(Duration::from_millis(config.final_observe_ms));
    let mut final_observed = 0usize;
    while let Some(sample) = subscriber.receive()? {
        let (_, sample_checksum) =
            validate_sidecar_payload(sample.payload(), config.payload_bytes)?;
        checksum = checksum.wrapping_add(sample_checksum);
        final_observed = final_observed.saturating_add(1);
    }
    emit(format!(
        "SUMMARY role=subscriber kind=sidecar service={} ready_service={}/ready count={} payload_bytes={} wakes={} notifications={} drains={} drain_cycles={} samples={} spurious_wakes={} remaining_after_wake={} remaining_after_last_wake={} final_observed={} remaining_after_final_observation=0 checksum={} event_driven=true queue_depth=1 safe_overflow=true payload_type=[u8]",
        config.service,
        config.service,
        config.count,
        config.payload_bytes,
        wakes,
        notifications,
        drain_cycles,
        drain_cycles,
        samples,
        spurious_wakes,
        remaining_after_last_wake,
        remaining_after_last_wake,
        final_observed,
        checksum
    ));
    Ok(())
}

fn churn_once(name: &str, hold_ms: u64) -> ProbeResult<()> {
    let service = service_name(name)?;
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let factory = node
        .service_builder(&service)
        .publish_subscribe::<u64>()
        .subscriber_max_buffer_size(1)
        .open_or_create()?;
    let publisher = factory.publisher_builder().create()?;
    if hold_ms > 0 {
        thread::sleep(Duration::from_millis(hold_ms));
    }
    drop(publisher);
    drop(factory);
    drop(node);
    Ok(())
}

fn churn(config: ChurnConfig, unique: bool) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let mut created = 0usize;
    let mut dropped = 0usize;
    let mut errors = 0usize;
    for cycle in 0..config.cycles {
        if Instant::now() >= deadline {
            break;
        }
        let service = if unique {
            format!("{}/{}", config.name, cycle)
        } else {
            config.name.clone()
        };
        match churn_once(&service, config.hold_ms) {
            Ok(()) => {
                created = created.saturating_add(1);
                dropped = dropped.saturating_add(1);
            }
            Err(_) => errors = errors.saturating_add(1),
        }
        if config.gap_ms > 0 {
            sleep_until(deadline, Duration::from_millis(config.gap_ms));
        }
    }
    let kind = if unique { "unique" } else { "same" };
    emit(format!(
        "SUMMARY role=churn kind={kind} name={} requested_cycles={} created={} dropped={} errors={} hold_ms={} gap_ms={}",
        config.name, config.cycles, created, dropped, errors, config.hold_ms, config.gap_ms
    ));
    Ok(())
}

fn service_observe(config: ObserveConfig) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    emit(format!(
        "READY role=observer kind=service-observe prefix={} poll_ms={}",
        config.prefix, config.poll_ms
    ));
    let mut previous = None;
    let mut transitions = 0usize;
    let mut polls = 0usize;
    while Instant::now() < deadline {
        let mut present = false;
        let mut matches = 0usize;
        let mut nodes = 0usize;
        iceoryx2::prelude::ipc_threadsafe::Service::list(
            iceoryx2::config::Config::global_config(),
            |details| {
                let service_name = details.static_details.name().to_string();
                if service_name.starts_with(&config.prefix) {
                    present = true;
                    matches = matches.saturating_add(1);
                    nodes = nodes.saturating_add(
                        details
                            .dynamic_details
                            .as_ref()
                            .map_or(0, |dynamic| dynamic.nodes.len()),
                    );
                }
                CallbackProgression::Continue
            },
        )?;
        polls = polls.saturating_add(1);
        if previous != Some(present) {
            transitions = transitions.saturating_add(1);
            emit(format!(
                "OBSERVE prefix={} transition={} matches={} nodes={} publishers=unknown",
                config.prefix,
                if present { "present" } else { "absent" },
                matches,
                nodes
            ));
            previous = Some(present);
        }
        if config.poll_ms > 0 {
            sleep_until(deadline, Duration::from_millis(config.poll_ms));
        }
    }
    emit(format!(
        "SUMMARY role=observer kind=service-observe prefix={} polls={} transitions={} final_present={} publishers=unknown",
        config.prefix,
        polls,
        transitions,
        previous.unwrap_or(false)
    ));
    let _ = node;
    Ok(())
}

/// Runs one finite probe command.
pub fn run(command: Command) -> ProbeResult<()> {
    match command {
        Command::Help => {
            print_help();
            Ok(())
        }
        Command::FixedPublisher(config) => publish_fixed(config),
        Command::FixedSubscriber(config) => subscribe_fixed(config),
        Command::EventNotifier(config) => notify_events(config),
        Command::EventListener(config) => listen_events(config),
        Command::BytesPublisher(config) => publish_bytes(config),
        Command::BytesSubscriber(config) => subscribe_bytes(config),
        Command::ImagePublisher(config) => publish_image(config),
        Command::ImageSubscriber(config) => subscribe_image(config),
        Command::PointCloudPublisher(config) => publish_pointcloud(config),
        Command::PointCloudSubscriber(config) => subscribe_pointcloud(config),
        Command::SensorPublisher(config) => publish_sensor(config),
        Command::SensorSubscriber(config) => subscribe_sensor(config),
        Command::ProviderHold(config) => provider_hold(config),
        Command::SidecarPublisher(config) => sidecar_publisher(config),
        Command::SidecarSubscriber(config) => sidecar_subscriber(config),
        Command::ChurnSame(config) => churn(config, false),
        Command::ChurnUnique(config) => churn(config, true),
        Command::ServiceObserve(config) => service_observe(config),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parser_covers_sidecar_and_churn_options() {
        let command = parse_args([
            "sidecar-pub",
            "--service",
            "rbt/test/sidecar",
            "--count",
            "10000",
            "--payload-bytes",
            "20971520",
            "--ready-prefill",
            "7",
            "--ready-prefill-gap-ms",
            "1234",
            "--suppress-final-ready",
        ])
        .expect("sidecar parser");
        assert!(matches!(
            command,
            Command::SidecarPublisher(SidecarConfig {
                count: 10_000,
                payload_bytes: 20_971_520,
                ready_prefill: 7,
                ready_prefill_gap_ms: 1234,
                suppress_final_ready: true,
                ..
            })
        ));
        let command = parse_args([
            "sidecar-sub",
            "--service",
            "rbt/test/sidecar-defaults",
            "--payload-bytes",
            "8",
        ])
        .expect("sidecar default parser");
        assert!(matches!(
            command,
            Command::SidecarSubscriber(SidecarConfig {
                ready_prefill: 0,
                ready_prefill_gap_ms: 0,
                ..
            })
        ));
        let command = parse_args([
            "churn-unique",
            "--prefix",
            "rbt/test/churn",
            "--cycles",
            "4",
        ])
        .expect("churn parser");
        assert!(matches!(
            command,
            Command::ChurnUnique(ChurnConfig {
                name,
                cycles: 4,
                ..
            }) if name == "rbt/test/churn"
        ));
        let command = parse_args([
            "sensor-sub",
            "--service",
            "rbt/test/sensor",
            "--count",
            "17",
            "--timeout-ms",
            "2000",
            "--pace-ms",
            "3",
        ])
        .expect("sensor parser");
        assert!(matches!(
            command,
            Command::SensorSubscriber(SensorConfig {
                service,
                count: 17,
                timeout_ms: 2000,
                pace_ms: 3,
            }) if service == "rbt/test/sensor"
        ));
        assert!(parse_args(["sensor-pub", "--count", "0"]).is_err());
    }

    #[test]
    fn layout_sizes_match_rbt_sensor_abi() {
        assert_eq!(size_of::<ImageHeader>(), 32);
        assert_eq!(size_of::<PointCloudHeader>(), 32);
        assert_eq!(size_of::<PointXYZI>(), 16);
        assert_eq!(align_of::<ImageHeader>(), 8);
        assert_eq!(align_of::<PointCloudHeader>(), 8);
        assert_eq!(align_of::<PointXYZI>(), 4);
    }

    #[test]
    fn deterministic_checksums_match_iteration() {
        for sequence in [0, 1, 255, 1_000] {
            for length in [0, 1, 255, 256, 1_001] {
                let dynamic = (0..length).fold(0_u64, |sum, index| {
                    sum.wrapping_add(u64::from(dynamic_byte(sequence, index)))
                });
                let image = (0..length).fold(0_u64, |sum, index| {
                    sum.wrapping_add(u64::from(image_byte(sequence, index)))
                });
                assert_eq!(dynamic_checksum(sequence, length), dynamic);
                assert_eq!(image_checksum(sequence, length), image);
            }
        }
    }

    #[test]
    fn point_checksum_matches_iteration() {
        for count in [0, 1, 100, 10_000] {
            let iterated = (0..count).fold(0_u64, |sum, index| {
                sum.wrapping_add(point_checksum(point_at(index)))
            });
            assert_eq!(point_cloud_checksum(count), iterated);
        }
    }
}

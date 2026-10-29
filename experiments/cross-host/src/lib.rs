#![forbid(unsafe_code)]

use std::{
    collections::BTreeMap,
    error::Error,
    fmt,
    io::Write,
    str::FromStr,
    sync::{Arc, Condvar, Mutex},
    thread,
    time::{Duration, Instant},
};

use iceoryx2::prelude::{
    BackpressureStrategy, EventId, NodeBuilder, PortFactory, ServiceName, ipc_threadsafe,
};
use rbt_iox2::{
    ActivationId, Image, PixelFormat, PointCloud, PointXYZI, ProcessInstanceId, ReadyEvent,
    SliceStream, StreamDescriptor, StreamError, StreamProfile, WaitContext, WaitExit, WaitProgress,
    WaitStatsSnapshot, WaitToken, preflight_stream,
};

const DEFAULT_FIXED_SERVICE: &str = "rbt/cross-host/plain/fixed";
const DEFAULT_EVENT_SERVICE: &str = "rbt/cross-host/plain/event";
const DEFAULT_BYTES_SERVICE: &str = "rbt/cross-host/plain/bytes";
const DEFAULT_IMAGE_SERVICE: &str = "rbt/cross-host/rbt/image";
const DEFAULT_POINTCLOUD_SERVICE: &str = "rbt/cross-host/rbt/pointcloud";
const DEFAULT_ABI_SERVICE: &str = "rbt/cross-host/abi-preflight";

const DEFAULT_TIMEOUT_MS: u64 = 10_000;
const DEFAULT_PLAIN_COUNT: usize = 10;
const DEFAULT_IMAGE_COUNT: usize = 3;
const DEFAULT_POINTCLOUD_COUNT: usize = 1;
const DEFAULT_POINTCLOUD_POINTS: usize = 100_000;
const DEFAULT_IMAGE_PACE_MS: u64 = 5;
const DEFAULT_POINTCLOUD_PACE_MS: u64 = 2;
const REMOTE_ATTACH_GRACE: Duration = Duration::from_millis(250);

/// A small error type used to keep parser and validation errors single-line.
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

/// The two fixed image layouts supported by the probe.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ImageLayout {
    Rgb1080p,
    Stride640,
}

impl ImageLayout {
    pub const fn name(self) -> &'static str {
        match self {
            Self::Rgb1080p => "1920x1080-rgb8",
            Self::Stride640 => "640x480-stride2048",
        }
    }

    pub const fn dimensions(self) -> (u32, u32, u32) {
        match self {
            Self::Rgb1080p => (1_920, 1_080, 1_920 * 3),
            Self::Stride640 => (640, 480, 2_048),
        }
    }

    pub fn payload_bytes(self) -> ProbeResult<usize> {
        let (_, height, stride) = self.dimensions();
        usize::try_from(stride)
            .ok()
            .and_then(|stride| {
                usize::try_from(height)
                    .ok()
                    .and_then(|height| stride.checked_mul(height))
            })
            .ok_or_else(|| {
                boxed_error(format!("image payload length overflow for {}", self.name()))
            })
    }
}

impl FromStr for ImageLayout {
    type Err = ProbeError;

    fn from_str(value: &str) -> Result<Self, Self::Err> {
        match value {
            "1920x1080-rgb8" | "1080p" => Ok(Self::Rgb1080p),
            "640x480-stride2048" | "vga-stride2048" => Ok(Self::Stride640),
            _ => Err(ProbeError::new(format!(
                "invalid --layout {value:?}; use 1920x1080-rgb8 or 640x480-stride2048"
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
    AbiMismatch { service: String },
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

fn parse_flags(args: &[String], allowed: &[&str]) -> Result<BTreeMap<String, String>, ParseError> {
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

fn common_config(
    values: &BTreeMap<String, String>,
    default_service: &str,
    default_count: usize,
) -> Result<BasicConfig, ParseError> {
    let service = option_string(values, "service", default_service);
    if service.is_empty() {
        return Err(ParseError::new("--service must not be empty"));
    }
    Ok(BasicConfig {
        service,
        count: option_usize(values, "count", default_count)?,
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
    match subcommand.as_str() {
        "fixed-pub" | "fixed-publisher" => Ok(Command::FixedPublisher(common_config(
            &parse_flags(options, &["service", "count", "timeout-ms"])?,
            DEFAULT_FIXED_SERVICE,
            DEFAULT_PLAIN_COUNT,
        )?)),
        "fixed-sub" | "fixed-subscriber" => Ok(Command::FixedSubscriber(common_config(
            &parse_flags(options, &["service", "count", "timeout-ms"])?,
            DEFAULT_FIXED_SERVICE,
            DEFAULT_PLAIN_COUNT,
        )?)),
        "event-notify" | "event-notifier" => Ok(Command::EventNotifier(common_config(
            &parse_flags(options, &["service", "count", "timeout-ms"])?,
            DEFAULT_EVENT_SERVICE,
            DEFAULT_PLAIN_COUNT,
        )?)),
        "event-listen" | "event-listener" => Ok(Command::EventListener(common_config(
            &parse_flags(options, &["service", "count", "timeout-ms"])?,
            DEFAULT_EVENT_SERVICE,
            DEFAULT_PLAIN_COUNT,
        )?)),
        "bytes-pub" | "bytes-publisher" | "dynamic-pub" => {
            let values = parse_flags(
                options,
                &["service", "payload-bytes", "count", "timeout-ms"],
            )?;
            let service = option_string(&values, "service", DEFAULT_BYTES_SERVICE);
            if service.is_empty() {
                return Err(ParseError::new("--service must not be empty"));
            }
            Ok(Command::BytesPublisher(BytesConfig {
                service,
                payload_bytes: option_usize(&values, "payload-bytes", 1_024)?,
                count: option_usize(&values, "count", DEFAULT_PLAIN_COUNT)?,
                timeout_ms: option_u64(&values, "timeout-ms", DEFAULT_TIMEOUT_MS)?,
            }))
        }
        "bytes-sub" | "bytes-subscriber" | "dynamic-sub" => {
            let values = parse_flags(
                options,
                &["service", "payload-bytes", "count", "timeout-ms"],
            )?;
            let service = option_string(&values, "service", DEFAULT_BYTES_SERVICE);
            if service.is_empty() {
                return Err(ParseError::new("--service must not be empty"));
            }
            Ok(Command::BytesSubscriber(BytesConfig {
                service,
                payload_bytes: option_usize(&values, "payload-bytes", 1_024)?,
                count: option_usize(&values, "count", DEFAULT_PLAIN_COUNT)?,
                timeout_ms: option_u64(&values, "timeout-ms", DEFAULT_TIMEOUT_MS)?,
            }))
        }
        "image-pub" | "image-publisher" => {
            let values = parse_flags(
                options,
                &["service", "layout", "count", "timeout-ms", "pace-ms"],
            )?;
            Ok(Command::ImagePublisher(image_config(
                &values,
                DEFAULT_IMAGE_SERVICE,
            )?))
        }
        "image-sub" | "image-subscriber" => {
            let values = parse_flags(
                options,
                &["service", "layout", "count", "timeout-ms", "pace-ms"],
            )?;
            Ok(Command::ImageSubscriber(image_config(
                &values,
                DEFAULT_IMAGE_SERVICE,
            )?))
        }
        "pointcloud-pub" | "pointcloud-publisher" | "point-pub" => {
            let values = parse_flags(
                options,
                &["service", "points", "count", "timeout-ms", "pace-ms"],
            )?;
            Ok(Command::PointCloudPublisher(pointcloud_config(
                &values,
                DEFAULT_POINTCLOUD_SERVICE,
            )?))
        }
        "pointcloud-sub" | "pointcloud-subscriber" | "point-sub" => {
            let values = parse_flags(
                options,
                &["service", "points", "count", "timeout-ms", "pace-ms"],
            )?;
            Ok(Command::PointCloudSubscriber(pointcloud_config(
                &values,
                DEFAULT_POINTCLOUD_SERVICE,
            )?))
        }
        "abi-mismatch" | "abi-preflight" => {
            let values = parse_flags(options, &["service"])?;
            let service = option_string(&values, "service", DEFAULT_ABI_SERVICE);
            if service.is_empty() {
                return Err(ParseError::new("--service must not be empty"));
            }
            Ok(Command::AbiMismatch { service })
        }
        other => Err(ParseError::new(format!(
            "unknown subcommand {other:?}; use --help"
        ))),
    }
}

fn image_config(
    values: &BTreeMap<String, String>,
    default_service: &str,
) -> Result<ImageConfig, ParseError> {
    let service = option_string(values, "service", default_service);
    if service.is_empty() {
        return Err(ParseError::new("--service must not be empty"));
    }
    let layout_value = option_string(values, "layout", ImageLayout::Rgb1080p.name());
    let layout = layout_value
        .parse::<ImageLayout>()
        .map_err(|error| ParseError::new(error.to_string()))?;
    Ok(ImageConfig {
        service,
        layout,
        count: option_usize(values, "count", DEFAULT_IMAGE_COUNT)?,
        timeout_ms: option_u64(values, "timeout-ms", DEFAULT_TIMEOUT_MS)?,
        pace_ms: option_u64(values, "pace-ms", DEFAULT_IMAGE_PACE_MS)?,
    })
}

fn pointcloud_config(
    values: &BTreeMap<String, String>,
    default_service: &str,
) -> Result<PointCloudConfig, ParseError> {
    let service = option_string(values, "service", default_service);
    if service.is_empty() {
        return Err(ParseError::new("--service must not be empty"));
    }
    Ok(PointCloudConfig {
        service,
        points: option_usize(values, "points", DEFAULT_POINTCLOUD_POINTS)?,
        count: option_usize(values, "count", DEFAULT_POINTCLOUD_COUNT)?,
        timeout_ms: option_u64(values, "timeout-ms", DEFAULT_TIMEOUT_MS)?,
        pace_ms: option_u64(values, "pace-ms", DEFAULT_POINTCLOUD_PACE_MS)?,
    })
}

/// Prints the complete command and option reference.
pub fn print_help() {
    println!(
        "cross-host-probe: finite local iceoryx2 0.9.3 feasibility probe\n\
This binary uses local iceoryx2 IPC only; it does not implement network transport.\n\
Every normal result is one KEY=VALUE line and preserves the exact --service text.\n\
\n\
SUBCOMMANDS\n\
  fixed-pub       plain fixed u64 publisher (sequence values 0..count-1)\n\
  fixed-sub       plain fixed u64 subscriber with exact-sequence validation\n\
  event-notify    plain Event notifier (event IDs 1..count)\n\
  event-listen    plain Event listener with exact event-ID validation\n\
  bytes-pub       plain dynamic [u8] publisher\n\
  bytes-sub       plain dynamic [u8] subscriber with pattern/checksum checks\n\
  image-pub       rbt SliceStream<Image> publisher\n\
  image-sub       rbt SliceStream<Image> subscriber using WaitContext/EventSource\n\
  pointcloud-pub  rbt SliceStream<PointCloud> publisher\n\
  pointcloud-sub  rbt SliceStream<PointCloud> subscriber using WaitContext/EventSource\n\
  abi-mismatch    typed ABI mismatch preflight; opens zero endpoints\n\
\n\
COMMON OPTIONS\n\
  --service NAME       exact iceoryx2 service name\n\
  --count N             number of fixed/events/byte/image/cloud samples\n\
  --timeout-ms N        finite open/receive/publish timeout (default 10000)\n\
\n\
BYTES OPTIONS\n\
  --payload-bytes N     dynamic [u8] length (default 1024; zero is allowed)\n\
\n\
IMAGE OPTIONS\n\
  --layout NAME         1920x1080-rgb8 (default) or 640x480-stride2048\n\
  --pace-ms N           publisher inter-frame pacing (default 5)\n\
\n\
POINTCLOUD OPTIONS\n\
  --points N             PointXYZI elements; supports 0, 1, 100, 100000 and arbitrary N\n\
  --pace-ms N           publisher inter-frame pacing (default 2)\n\
\n\
EXAMPLES\n\
  cross-host-probe fixed-sub --service rbt/probe/fixed --count 10\n\
  cross-host-probe image-pub --service rbt/probe/image --layout 640x480-stride2048\n\
  cross-host-probe pointcloud-sub --service rbt/probe/cloud --points 100000\n\
  cross-host-probe abi-mismatch --service rbt/probe/abi\n"
    );
}

/// Deterministic byte pattern for the plain dynamic [u8] probe.
pub fn dynamic_byte(sequence: u64, index: usize) -> u8 {
    sequence
        .wrapping_mul(13)
        .wrapping_add((index as u64).wrapping_mul(17))
        .wrapping_add(7) as u8
}

/// Deterministic checksum for the dynamic byte pattern.
pub fn dynamic_checksum(sequence: u64, payload_len: usize) -> u64 {
    stepped_checksum(
        sequence.wrapping_mul(13).wrapping_add(7) as u8,
        17,
        payload_len,
    )
}

/// Deterministic byte pattern for Image payloads.
pub fn image_byte(sequence: u64, index: usize) -> u8 {
    sequence.wrapping_add(index as u64) as u8
}

/// Deterministic checksum for the Image byte pattern.
pub fn image_checksum(sequence: u64, payload_len: usize) -> u64 {
    stepped_checksum(sequence as u8, 1, payload_len)
}

fn stepped_checksum(start: u8, step: u8, length: usize) -> u64 {
    const PERIOD_SUM: u64 = (255 * 256) / 2;
    let complete_periods = length / 256;
    let remainder = length % 256;
    let mut checksum = (complete_periods as u64).wrapping_mul(PERIOD_SUM);
    for index in 0..remainder {
        checksum = checksum.wrapping_add(u64::from(
            start.wrapping_add(step.wrapping_mul(index as u8)),
        ));
    }
    checksum
}

/// Deterministic PointXYZI payload element.
pub fn point_at(index: usize) -> PointXYZI {
    let value = index as f32;
    PointXYZI {
        x: value,
        y: value * 2.0,
        z: -value,
        intensity: (index % 31) as f32,
    }
}

/// Stable bitwise checksum contribution for one PointXYZI value.
pub fn point_checksum(point: PointXYZI) -> u64 {
    u64::from(point.x.to_bits())
        .wrapping_mul(31)
        .wrapping_add(u64::from(point.y.to_bits()))
        .wrapping_mul(31)
        .wrapping_add(u64::from(point.z.to_bits()))
        .wrapping_mul(31)
        .wrapping_add(u64::from(point.intensity.to_bits()))
}

/// Stable checksum for the first count deterministic points.
pub fn point_cloud_checksum(count: usize) -> u64 {
    (0..count).fold(0_u64, |checksum, index| {
        checksum.wrapping_add(point_checksum(point_at(index)))
    })
}

fn selected_bytes<F>(payload: &[u8], value_at: F) -> String
where
    F: Fn(usize) -> u8,
{
    selected_expectations(payload.len(), value_at)
}

fn selected_expectations<F>(payload_len: usize, value_at: F) -> String
where
    F: Fn(usize) -> u8,
{
    if payload_len == 0 {
        return "none".to_owned();
    }
    let indices = [0, payload_len / 2, payload_len - 1];
    indices
        .into_iter()
        .map(|index| format!("{index}:{}", value_at(index)))
        .collect::<Vec<_>>()
        .join(",")
}

fn validate_selected_bytes<F>(payload: &[u8], value_at: F) -> ProbeResult<String>
where
    F: Fn(usize) -> u8,
{
    let selected = selected_bytes(payload, &value_at);
    if payload.is_empty() {
        return Ok(selected);
    }
    let indices = [0, payload.len() / 2, payload.len() - 1];
    for index in indices {
        let expected = value_at(index);
        let actual = payload[index];
        if actual != expected {
            return invalid(format!(
                "selected byte mismatch index={index} expected={expected} actual={actual}"
            ));
        }
    }
    Ok(selected)
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
        sleep_until(deadline, Duration::from_millis(1));
    }
}

fn timestamp_for(sequence: u64) -> ProbeResult<u64> {
    sequence
        .checked_mul(1_000_000)
        .ok_or_else(|| boxed_error(format!("sequence {sequence} timestamp overflows u64")))
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
        "READY role=publisher kind=plain-fixed service={} count={}",
        config.service, config.count
    ));
    let connected = wait_for_at_least(deadline, "plain-fixed subscriber", || {
        factory.dynamic_config().number_of_subscribers()
    })?;
    // The local count proves only that the source-side tunnel attached. Give
    // the independently discovered remote mirror/subscriber a bounded grace
    // period; this is startup coordination, not payload polling.
    thread::sleep(REMOTE_ATTACH_GRACE);

    let count =
        u64::try_from(config.count).map_err(|_| boxed_error("fixed count does not fit in u64"))?;
    let mut deliveries = 0usize;
    for sequence in 0..count {
        if factory.dynamic_config().number_of_subscribers() == 0 {
            return invalid(format!(
                "plain-fixed subscriber disconnected at sequence={sequence}"
            ));
        }
        let mut sample = publisher.loan()?;
        *sample.payload_mut() = sequence;
        let recipients = sample.send()?;
        if recipients == 0 {
            return invalid(format!(
                "plain-fixed sequence={sequence} was delivered to zero subscribers"
            ));
        }
        deliveries = deliveries.saturating_add(recipients);
    }
    emit(format!(
        "SUMMARY role=publisher kind=plain-fixed service={} count={} connected={} deliveries={}",
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
        match node
            .service_builder(name)
            .publish_subscribe::<u64>()
            .enable_safe_overflow(false)
            .subscriber_max_buffer_size(count.max(1))
            .history_size(0)
            .open()
        {
            Ok(factory) => {
                return Ok(factory
                    .subscriber_builder()
                    .buffer_size(count.max(1))
                    .create()?);
            }
            Err(error) if Instant::now() < deadline => {
                let _ = error;
                sleep_until(deadline, Duration::from_millis(1));
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
        "READY role=subscriber kind=plain-fixed service={} count={}",
        config.service, config.count
    ));
    let count =
        u64::try_from(config.count).map_err(|_| boxed_error("fixed count does not fit in u64"))?;
    let mut received = 0usize;
    let mut checksum = 0_u64;
    while received < config.count {
        if let Some(sample) = subscriber.receive()? {
            let expected = received as u64;
            let actual = *sample.payload();
            if actual != expected {
                return invalid(format!(
                    "plain-fixed sequence mismatch index={received} expected={expected} actual={actual}"
                ));
            }
            checksum = checksum.wrapping_add(actual);
            received += 1;
        } else if Instant::now() >= deadline {
            return invalid(format!(
                "timed out receiving plain-fixed samples received={} expected={count}",
                received
            ));
        } else {
            sleep_until(deadline, Duration::from_millis(1));
        }
    }
    emit(format!(
        "SUMMARY role=subscriber kind=plain-fixed service={} count={} received={} checksum={}",
        config.service, config.count, received, checksum
    ));
    Ok(())
}

fn event_factory(
    node: &iceoryx2::node::Node<ipc_threadsafe::Service>,
    name: &ServiceName,
    count: usize,
) -> ProbeResult<iceoryx2::service::port_factory::event::PortFactory<ipc_threadsafe::Service>> {
    Ok(node
        .service_builder(name)
        .event()
        .event_id_max_value(count.max(1))
        .open_or_create()?)
}

fn notify_events(config: BasicConfig) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let name = service_name(&config.service)?;
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let factory = event_factory(&node, &name, config.count)?;
    let notifier = factory.notifier_builder().create()?;
    emit(format!(
        "READY role=notifier kind=plain-event service={} count={}",
        config.service, config.count
    ));
    let connected = wait_for_at_least(deadline, "plain-event listener", || {
        factory.dynamic_config().number_of_listeners()
    })?;
    thread::sleep(REMOTE_ATTACH_GRACE);
    let mut notifications = 0usize;
    for sequence in 0..config.count {
        let event_id = EventId::new(sequence.saturating_add(1));
        let listeners = notifier.notify_with_custom_event_id(event_id)?;
        if listeners == 0 {
            return invalid(format!(
                "plain-event id={} was delivered to zero listeners",
                sequence + 1
            ));
        }
        notifications = notifications.saturating_add(listeners);
    }
    emit(format!(
        "SUMMARY role=notifier kind=plain-event service={} count={} connected={} notifications={}",
        config.service, config.count, connected, notifications
    ));
    Ok(())
}

fn listen_events(config: BasicConfig) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let name = service_name(&config.service)?;
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    // A consumer must never create the service: doing so on the destination
    // host before the mirror arrives creates a same-hash local Event that the
    // tunnel then (correctly) deduplicates instead of bridging.
    let factory = loop {
        match node
            .service_builder(&name)
            .event()
            .event_id_max_value(config.count.max(1))
            .open()
        {
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
        "READY role=listener kind=plain-event service={} count={}",
        config.service, config.count
    ));
    let mut received = 0usize;
    let mut checksum = 0_u64;
    while received < config.count {
        let remaining = deadline.saturating_duration_since(Instant::now());
        let Some(event_id) = listener.timed_wait_one(remaining)? else {
            return invalid(format!(
                "timed out receiving plain-event IDs received={} expected={}",
                received, config.count
            ));
        };
        let expected = received + 1;
        let actual = event_id.as_value();
        if actual != expected {
            return invalid(format!(
                "plain-event ID mismatch index={received} expected={expected} actual={actual}"
            ));
        }
        checksum = checksum.wrapping_add(actual as u64);
        received += 1;
    }
    emit(format!(
        "SUMMARY role=listener kind=plain-event service={} count={} received={} checksum={}",
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
        "READY role=publisher kind=plain-bytes service={} count={} payload_bytes={}",
        config.service, config.count, config.payload_bytes
    ));
    let connected = wait_for_at_least(deadline, "plain-bytes subscriber", || {
        factory.dynamic_config().number_of_subscribers()
    })?;
    thread::sleep(REMOTE_ATTACH_GRACE);
    let mut deliveries = 0usize;
    let mut checksum = 0_u64;
    let count = u64::try_from(config.count).map_err(|_| boxed_error("count overflows u64"))?;
    for sequence in 0..count {
        let sample = publisher.loan_slice_uninit(config.payload_bytes)?;
        let sample = sample.write_from_fn(|index| dynamic_byte(sequence, index));
        checksum = dynamic_checksum(sequence, config.payload_bytes);
        let recipients = sample.send()?;
        if recipients == 0 {
            return invalid(format!(
                "plain-bytes sequence={sequence} was delivered to zero subscribers"
            ));
        }
        deliveries = deliveries.saturating_add(recipients);
    }
    emit(format!(
        "SUMMARY role=publisher kind=plain-bytes service={} count={} connected={} payload_bytes={} checksum={} deliveries={}",
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
            .enable_safe_overflow(false)
            .subscriber_max_buffer_size(config.count.max(1))
            .history_size(0)
            .open()
        {
            Ok(factory) => {
                return Ok(factory
                    .subscriber_builder()
                    .buffer_size(config.count.max(1))
                    .create()?);
            }
            Err(error) if Instant::now() < deadline => {
                let _ = error;
                sleep_until(deadline, Duration::from_millis(1));
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
        "READY role=subscriber kind=plain-bytes service={} count={} payload_bytes={}",
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
            let expected_checksum = dynamic_checksum(received as u64, payload.len());
            let actual_checksum = payload
                .iter()
                .fold(0_u64, |sum, byte| sum.wrapping_add(u64::from(*byte)));
            if actual_checksum != expected_checksum {
                return invalid(format!(
                    "plain-bytes checksum mismatch sequence={} expected={} actual={}",
                    received, expected_checksum, actual_checksum
                ));
            }
            selected =
                validate_selected_bytes(payload, |index| dynamic_byte(received as u64, index))?;
            checksum = checksum.wrapping_add(actual_checksum);
            received += 1;
        } else if Instant::now() >= deadline {
            return invalid(format!(
                "timed out receiving plain-bytes samples received={} expected={}",
                received, config.count
            ));
        } else {
            sleep_until(deadline, Duration::from_millis(1));
        }
    }
    emit(format!(
        "SUMMARY role=subscriber kind=plain-bytes service={} count={} received={} payload_bytes={} checksum={} selected={}",
        config.service, config.count, received, config.payload_bytes, checksum, selected
    ));
    Ok(())
}

fn rbt_subscriber_count(snapshot: rbt_iox2::StreamPressure) -> usize {
    snapshot
        .publisher
        .map(|publisher| publisher.subscriber_count as usize)
        .unwrap_or(0)
}

fn image_descriptor(service: &str) -> StreamDescriptor {
    SliceStream::<Image>::descriptor(service.to_owned(), StreamProfile::Sensor)
}

fn pointcloud_descriptor(service: &str) -> StreamDescriptor {
    SliceStream::<PointCloud>::descriptor(service.to_owned(), StreamProfile::Sensor)
}

fn publish_image(config: ImageConfig) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let payload_bytes = config.layout.payload_bytes()?;
    let descriptor = image_descriptor(&config.service);
    let publisher = SliceStream::<Image>::publisher(&descriptor, payload_bytes)?;
    emit(format!(
        "READY role=publisher kind=rbt-image service={} count={} layout={} payload_bytes={} layout_hash={}",
        config.service,
        config.count,
        config.layout.name(),
        payload_bytes,
        descriptor.message_type().layout_hash
    ));
    let connected = wait_for_at_least(deadline, "rbt Image subscriber", || {
        rbt_subscriber_count(publisher.pressure())
    })?;
    thread::sleep(REMOTE_ATTACH_GRACE);
    let (width, height, stride) = config.layout.dimensions();
    let mut deliveries = 0usize;
    for sequence in 0..config.count as u64 {
        let mut sample = publisher
            .loan_slice_uninit(payload_bytes)?
            .write_from_fn(|index| image_byte(sequence, index));
        {
            let header = sample.header_mut();
            header.sequence = sequence;
            header.timestamp_ns = timestamp_for(sequence)?;
            header.width = width;
            header.height = height;
            header.stride = stride;
            header.encoding = PixelFormat::RGB8;
        }
        let recipients = sample.send()?;
        if recipients == 0 {
            return invalid(format!(
                "rbt Image sequence={sequence} was delivered to zero subscribers"
            ));
        }
        deliveries = deliveries.saturating_add(recipients);
        if config.pace_ms > 0 {
            thread::sleep(Duration::from_millis(config.pace_ms));
        }
    }
    emit(format!(
        "SUMMARY role=publisher kind=rbt-image service={} count={} connected={} layout={} payload_bytes={} checksum_last={} deliveries={} layout_hash={}",
        config.service,
        config.count,
        connected,
        config.layout.name(),
        payload_bytes,
        config
            .count
            .checked_sub(1)
            .map_or(0, |sequence| image_checksum(sequence as u64, payload_bytes)),
        deliveries,
        descriptor.message_type().layout_hash
    ));
    Ok(())
}

fn open_image_subscriber(
    descriptor: &StreamDescriptor,
    deadline: Instant,
) -> ProbeResult<rbt_iox2::SliceSubscriber<Image>> {
    loop {
        match SliceStream::<Image>::subscriber(descriptor) {
            Ok(subscriber) => return Ok(subscriber),
            Err(error) if Instant::now() < deadline => {
                let _ = error;
                sleep_until(deadline, Duration::from_millis(2));
            }
            Err(error) => {
                return invalid(format!("timed out opening rbt Image service: {error}"));
            }
        }
    }
}

fn wait_for_images(
    subscriber: &rbt_iox2::SliceSubscriber<Image>,
    config: &ImageConfig,
    payload_bytes: usize,
) -> ProbeResult<WaitStatsSnapshot> {
    let mut context = WaitContext::new(ProcessInstanceId::new(), ActivationId::first())?;
    let stats = context.stats();
    let source = subscriber.event_source(WaitToken::new(1));
    let timeout_done = Arc::new((Mutex::new(false), Condvar::new()));
    let timeout_done_for_thread = Arc::clone(&timeout_done);
    let wake_handle = context.wake_handle();
    let timeout = Duration::from_millis(config.timeout_ms);
    let (width, height, stride) = config.layout.dimensions();
    let mut received = 0usize;

    let exit = thread::scope(|scope| -> ProbeResult<WaitExit> {
        let timer = scope.spawn(move || -> ProbeResult<()> {
            let (done, condition) = &*timeout_done_for_thread;
            let done = done
                .lock()
                .map_err(|_| boxed_error("image timeout state was poisoned"))?;
            let (done, result) = condition
                .wait_timeout_while(done, timeout, |finished| !*finished)
                .map_err(|_| boxed_error("image timeout state was poisoned"))?;
            if result.timed_out() && !*done {
                wake_handle.wake()?;
            }
            Ok(())
        });

        let result = context.run(
            std::slice::from_ref(&source),
            |event: ReadyEvent, budget| -> ProbeResult<WaitProgress> {
                if event.is_cancellation() {
                    return Ok(WaitProgress::stop());
                }
                let mut drained = 0usize;
                for _ in 0..budget {
                    let Some(sample) = subscriber.receive()? else {
                        return Ok(if drained == 0 {
                            WaitProgress::empty()
                        } else {
                            WaitProgress::SamplesAndEmpty {
                                samples: drained,
                                empty_wakeups: 1,
                            }
                        });
                    };
                    let sequence = received as u64;
                    let header = sample.header();
                    if header.sequence != sequence
                        || header.timestamp_ns != timestamp_for(sequence)?
                        || header.width != width
                        || header.height != height
                        || header.stride != stride
                        || header.encoding != PixelFormat::RGB8
                    {
                        return invalid(format!(
                            "rbt Image header mismatch sequence={sequence} actual={header:?}"
                        ));
                    }
                    let payload = sample.payload();
                    if payload.len() != payload_bytes {
                        return invalid(format!(
                            "rbt Image payload length mismatch sequence={sequence} expected={payload_bytes} actual={}",
                            payload.len()
                        ));
                    }
                    let expected_checksum = image_checksum(sequence, payload_bytes);
                    let actual_checksum = payload.iter().fold(0_u64, |sum, byte| {
                        sum.wrapping_add(u64::from(*byte))
                    });
                    if actual_checksum != expected_checksum {
                        return invalid(format!(
                            "rbt Image checksum mismatch sequence={sequence} expected={expected_checksum} actual={actual_checksum}"
                        ));
                    }
                    validate_selected_bytes(payload, |index| image_byte(sequence, index))?;
                    drained = drained.saturating_add(1);
                    received = received.saturating_add(1);
                    if received == config.count {
                        return Ok(WaitProgress::stop_after_samples(drained));
                    }
                }
                Ok(WaitProgress::samples(drained))
            },
        );
        if let Ok(mut done) = timeout_done.0.lock() {
            *done = true;
            timeout_done.1.notify_one();
        }
        timer
            .join()
            .map_err(|_| boxed_error("image timeout thread panicked"))??;
        result
    })?;

    if exit == WaitExit::Cancelled || received != config.count {
        return invalid(format!(
            "timed out receiving rbt Images received={} expected={}",
            received, config.count
        ));
    }
    Ok(stats.snapshot())
}

fn subscribe_image(config: ImageConfig) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let payload_bytes = config.layout.payload_bytes()?;
    let descriptor = image_descriptor(&config.service);
    let subscriber = open_image_subscriber(&descriptor, deadline)?;
    emit(format!(
        "READY role=subscriber kind=rbt-image service={} count={} layout={} payload_bytes={} strategy=waitset layout_hash={}",
        config.service,
        config.count,
        config.layout.name(),
        payload_bytes,
        descriptor.message_type().layout_hash
    ));
    let stats = wait_for_images(&subscriber, &config, payload_bytes)?;
    let selected = selected_expectations(payload_bytes, |index| {
        image_byte(config.count.saturating_sub(1) as u64, index)
    });
    emit(format!(
        "SUMMARY role=subscriber kind=rbt-image service={} count={} received={} layout={} payload_bytes={} checksum_last={} selected_hint={} waits={} wakeups={} spurious_wakes={} samples={} strategy=waitset layout_hash={}",
        config.service,
        config.count,
        config.count,
        config.layout.name(),
        payload_bytes,
        config
            .count
            .checked_sub(1)
            .map_or(0, |sequence| image_checksum(sequence as u64, payload_bytes)),
        selected,
        stats.waits,
        stats.wakeups,
        stats.empty_wakeups,
        stats.samples_processed,
        descriptor.message_type().layout_hash
    ));
    Ok(())
}

fn publish_pointcloud(config: PointCloudConfig) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let descriptor = pointcloud_descriptor(&config.service);
    let publisher = SliceStream::<PointCloud>::publisher(&descriptor, config.points)?;
    emit(format!(
        "READY role=publisher kind=rbt-pointcloud service={} count={} points={} payload_bytes={} layout_hash={}",
        config.service,
        config.count,
        config.points,
        config
            .points
            .saturating_mul(std::mem::size_of::<PointXYZI>()),
        descriptor.message_type().layout_hash
    ));
    let connected = wait_for_at_least(deadline, "rbt PointCloud subscriber", || {
        rbt_subscriber_count(publisher.pressure())
    })?;
    thread::sleep(REMOTE_ATTACH_GRACE);
    let width = u32::try_from(config.points)
        .map_err(|_| boxed_error("PointCloud point count exceeds u32"))?;
    let mut deliveries = 0usize;
    for sequence in 0..config.count as u64 {
        let mut sample = publisher
            .loan_slice_uninit(config.points)?
            .write_from_fn(point_at);
        {
            let header = sample.header_mut();
            header.sequence = sequence;
            header.timestamp_ns = timestamp_for(sequence)?;
            header.width = width;
            header.height = 1;
            header.dense = 1;
            header.reserved = [0; 7];
        }
        let recipients = sample.send()?;
        if recipients == 0 {
            return invalid(format!(
                "rbt PointCloud sequence={sequence} was delivered to zero subscribers"
            ));
        }
        deliveries = deliveries.saturating_add(recipients);
        if config.pace_ms > 0 {
            thread::sleep(Duration::from_millis(config.pace_ms));
        }
    }
    emit(format!(
        "SUMMARY role=publisher kind=rbt-pointcloud service={} count={} connected={} points={} payload_bytes={} checksum={} deliveries={} layout_hash={}",
        config.service,
        config.count,
        connected,
        config.points,
        config
            .points
            .saturating_mul(std::mem::size_of::<PointXYZI>()),
        point_cloud_checksum(config.points),
        deliveries,
        descriptor.message_type().layout_hash
    ));
    Ok(())
}

fn open_pointcloud_subscriber(
    descriptor: &StreamDescriptor,
    deadline: Instant,
) -> ProbeResult<rbt_iox2::SliceSubscriber<PointCloud>> {
    loop {
        match SliceStream::<PointCloud>::subscriber(descriptor) {
            Ok(subscriber) => return Ok(subscriber),
            Err(error) if Instant::now() < deadline => {
                let _ = error;
                sleep_until(deadline, Duration::from_millis(2));
            }
            Err(error) => {
                return invalid(format!("timed out opening rbt PointCloud service: {error}"));
            }
        }
    }
}

fn same_point(actual: &PointXYZI, expected: PointXYZI) -> bool {
    actual.x.to_bits() == expected.x.to_bits()
        && actual.y.to_bits() == expected.y.to_bits()
        && actual.z.to_bits() == expected.z.to_bits()
        && actual.intensity.to_bits() == expected.intensity.to_bits()
}

fn wait_for_pointclouds(
    subscriber: &rbt_iox2::SliceSubscriber<PointCloud>,
    config: &PointCloudConfig,
) -> ProbeResult<WaitStatsSnapshot> {
    let mut context = WaitContext::new(ProcessInstanceId::new(), ActivationId::first())?;
    let stats = context.stats();
    let source = subscriber.event_source(WaitToken::new(1));
    let timeout_done = Arc::new((Mutex::new(false), Condvar::new()));
    let timeout_done_for_thread = Arc::clone(&timeout_done);
    let wake_handle = context.wake_handle();
    let timeout = Duration::from_millis(config.timeout_ms);
    let width = u32::try_from(config.points)
        .map_err(|_| boxed_error("PointCloud point count exceeds u32"))?;
    let mut received = 0usize;

    let exit = thread::scope(|scope| -> ProbeResult<WaitExit> {
        let timer = scope.spawn(move || -> ProbeResult<()> {
            let (done, condition) = &*timeout_done_for_thread;
            let done = done
                .lock()
                .map_err(|_| boxed_error("pointcloud timeout state was poisoned"))?;
            let (done, result) = condition
                .wait_timeout_while(done, timeout, |finished| !*finished)
                .map_err(|_| boxed_error("pointcloud timeout state was poisoned"))?;
            if result.timed_out() && !*done {
                wake_handle.wake()?;
            }
            Ok(())
        });

        let result = context.run(
            std::slice::from_ref(&source),
            |event: ReadyEvent, budget| -> ProbeResult<WaitProgress> {
                if event.is_cancellation() {
                    return Ok(WaitProgress::stop());
                }
                let mut drained = 0usize;
                for _ in 0..budget {
                    let Some(sample) = subscriber.receive()? else {
                        return Ok(if drained == 0 {
                            WaitProgress::empty()
                        } else {
                            WaitProgress::SamplesAndEmpty {
                                samples: drained,
                                empty_wakeups: 1,
                            }
                        });
                    };
                    let sequence = received as u64;
                    let header = sample.header();
                    if header.sequence != sequence
                        || header.timestamp_ns != timestamp_for(sequence)?
                        || header.width != width
                        || header.height != 1
                        || header.dense != 1
                        || header.reserved != [0; 7]
                    {
                        return invalid(format!(
                            "rbt PointCloud header mismatch sequence={sequence} actual={header:?}"
                        ));
                    }
                    let payload = sample.payload();
                    if payload.len() != config.points {
                        return invalid(format!(
                            "rbt PointCloud count mismatch sequence={sequence} expected={} actual={}",
                            config.points,
                            payload.len()
                        ));
                    }
                    if let Some(first) = payload.first()
                        && !same_point(first, point_at(0))
                    {
                        return invalid(format!(
                            "rbt PointCloud first point mismatch sequence={sequence} actual={first:?}"
                        ));
                    }
                    if let Some(last) = payload.last()
                        && !same_point(last, point_at(config.points - 1))
                    {
                        return invalid(format!(
                            "rbt PointCloud last point mismatch sequence={sequence} actual={last:?}"
                        ));
                    }
                    let actual_checksum = payload.iter().fold(0_u64, |checksum, point| {
                        checksum.wrapping_add(point_checksum(*point))
                    });
                    let expected_checksum = point_cloud_checksum(config.points);
                    if actual_checksum != expected_checksum {
                        return invalid(format!(
                            "rbt PointCloud checksum mismatch sequence={sequence} expected={expected_checksum} actual={actual_checksum}"
                        ));
                    }
                    drained = drained.saturating_add(1);
                    received = received.saturating_add(1);
                    if received == config.count {
                        return Ok(WaitProgress::stop_after_samples(drained));
                    }
                }
                Ok(WaitProgress::samples(drained))
            },
        );
        if let Ok(mut done) = timeout_done.0.lock() {
            *done = true;
            timeout_done.1.notify_one();
        }
        timer
            .join()
            .map_err(|_| boxed_error("pointcloud timeout thread panicked"))??;
        result
    })?;

    if exit == WaitExit::Cancelled || received != config.count {
        return invalid(format!(
            "timed out receiving rbt PointClouds received={} expected={}",
            received, config.count
        ));
    }
    Ok(stats.snapshot())
}

fn subscribe_pointcloud(config: PointCloudConfig) -> ProbeResult<()> {
    let deadline = deadline_after(config.timeout_ms)?;
    let descriptor = pointcloud_descriptor(&config.service);
    let subscriber = open_pointcloud_subscriber(&descriptor, deadline)?;
    emit(format!(
        "READY role=subscriber kind=rbt-pointcloud service={} count={} points={} payload_bytes={} strategy=waitset layout_hash={}",
        config.service,
        config.count,
        config.points,
        config
            .points
            .saturating_mul(std::mem::size_of::<PointXYZI>()),
        descriptor.message_type().layout_hash
    ));
    let stats = wait_for_pointclouds(&subscriber, &config)?;
    let first = (config.points > 0).then(|| point_at(0));
    let last = config.points.checked_sub(1).map(point_at);
    emit(format!(
        "SUMMARY role=subscriber kind=rbt-pointcloud service={} count={} received={} points={} payload_bytes={} checksum={} first={first:?} last={last:?} waits={} wakeups={} spurious_wakes={} samples={} strategy=waitset layout_hash={}",
        config.service,
        config.count,
        config.count,
        config.points,
        config
            .points
            .saturating_mul(std::mem::size_of::<PointXYZI>()),
        point_cloud_checksum(config.points),
        stats.waits,
        stats.wakeups,
        stats.empty_wakeups,
        stats.samples_processed,
        descriptor.message_type().layout_hash
    ));
    Ok(())
}

fn run_abi_mismatch(service: String) -> ProbeResult<()> {
    let actual = StreamDescriptor::new(
        service.clone(),
        PointCloud::descriptor(),
        StreamProfile::Sensor,
    );
    match preflight_stream(Image::descriptor(), &actual) {
        Err(StreamError::AbiMismatch {
            expected_layout,
            actual_layout,
            ..
        }) => {
            emit(format!(
                "SUMMARY kind=abi-mismatch service={service} result=PASS error=AbiMismatch expected_layout={expected_layout} actual_layout={actual_layout} endpoints_opened=0"
            ));
            Ok(())
        }
        Err(error) => invalid(format!("expected AbiMismatch, got {error}")),
        Ok(()) => invalid("ABI mismatch preflight unexpectedly succeeded"),
    }
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
        Command::AbiMismatch { service } => run_abi_mismatch(service),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_image_and_pointcloud_commands() {
        assert!(matches!(
            parse_args(["image-pub", "--layout", "vga-stride2048", "--count", "7"])
                .expect("parse image"),
            Command::ImagePublisher(ImageConfig {
                layout: ImageLayout::Stride640,
                count: 7,
                ..
            })
        ));
        assert!(matches!(
            parse_args(["pointcloud-sub", "--points", "1310720"]).expect("parse pointcloud"),
            Command::PointCloudSubscriber(PointCloudConfig {
                points: 1_310_720,
                ..
            })
        ));
    }

    #[test]
    fn deterministic_byte_checksums_match_iteration() {
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
    fn deterministic_point_checksum_matches_iteration() {
        for count in [0, 1, 100, 10_000] {
            let iterated = (0..count).fold(0_u64, |sum, index| {
                sum.wrapping_add(point_checksum(point_at(index)))
            });
            assert_eq!(point_cloud_checksum(count), iterated);
        }
    }

    #[test]
    fn abi_mismatch_is_rejected_without_opening_endpoints() {
        run_abi_mismatch("rbt/cross-host/test/abi".to_owned()).expect("AbiMismatch pass");
    }
}

use std::{fmt::Display, str::FromStr, time::Duration};

use anyhow::{Result, anyhow, bail};
use async_trait::async_trait;
use rbt::{
    component::{ActivationCtx, Component, run_component},
    stream::{SliceStream, Stream},
    types::{
        Capability, CapabilityInstanceId, ComponentId, ControlCommand, Image, PixelFormat,
        PointCloud, PointXYZI, RequirementSpec, StreamProfile,
    },
};
use tracing_subscriber::EnvFilter;

const LARGE_IMAGE_WIDTH: u32 = 1_920;
const LARGE_IMAGE_HEIGHT: u32 = 1_080;
const LARGE_IMAGE_STRIDE: u32 = LARGE_IMAGE_WIDTH * 3;
const LARGE_IMAGE_BYTES: usize = LARGE_IMAGE_STRIDE as usize * LARGE_IMAGE_HEIGHT as usize;
const SMALL_IMAGE_WIDTH: u32 = 8;
const SMALL_IMAGE_HEIGHT: u32 = 4;
const SMALL_IMAGE_STRIDE: u32 = SMALL_IMAGE_WIDTH * 3;
const SMALL_IMAGE_BYTES: usize = SMALL_IMAGE_STRIDE as usize * SMALL_IMAGE_HEIGHT as usize;
const BENCH_IMAGE_WIDTH: u32 = 8;
const BENCH_IMAGE_HEIGHT: u32 = 1;
const BENCH_IMAGE_STRIDE: u32 = BENCH_IMAGE_WIDTH * 3;
const BENCH_IMAGE_BYTES: usize = BENCH_IMAGE_STRIDE as usize * BENCH_IMAGE_HEIGHT as usize;

struct Camera;

impl Capability for Camera {
    const TYPE_NAME: &'static str = "Camera";
}

struct Detections;

impl Capability for Detections {
    const TYPE_NAME: &'static str = "Detections";
}

struct MotionCommand;

impl Capability for MotionCommand {
    const TYPE_NAME: &'static str = "MotionCommand";
}

#[derive(Clone, Copy)]
enum Mode {
    Mixed,
    SmallImage,
    LargeImage,
    LargePointCloud,
}

struct Source {
    mode: Mode,
}

#[async_trait]
impl Component for Source {
    type Config = Mode;
    type Error = anyhow::Error;

    async fn prepare(mode: &Self::Config) -> Result<Self> {
        Ok(Self { mode: *mode })
    }

    fn id(&self) -> ComponentId {
        ComponentId::from("mcap-source")
    }

    fn requirements(&self) -> Vec<RequirementSpec> {
        Vec::new()
    }

    async fn activate(&mut self, ctx: &mut ActivationCtx<'_>) -> Result<()> {
        match self.mode {
            Mode::Mixed => activate_mixed(ctx),
            Mode::SmallImage => activate_small_image(ctx),
            Mode::LargeImage => activate_image(ctx),
            Mode::LargePointCloud => activate_pointcloud(ctx),
        }
    }
}

fn activate_small_image(ctx: &mut ActivationCtx<'_>) -> Result<()> {
    let capability = CapabilityInstanceId::new();
    let descriptor =
        SliceStream::<Image>::descriptor(capability.stream_service_name(), StreamProfile::Sensor);
    let publisher = SliceStream::<Image>::publisher(&descriptor, BENCH_IMAGE_BYTES)?;
    ctx.provide::<Camera>(capability, descriptor);
    ctx.spawn_critical_blocking(
        "publish-small-image",
        move |token| {
            let rate_hz = env_or_default("RBT_MCAP_SOURCE_RATE_HZ", 500.0_f64)?;
            let period = checked_period(rate_hz)?;
            let mut sequence = 1_u64;
            while !token.is_cancelled() {
                publish_image(
                    &publisher,
                    sequence,
                    BENCH_IMAGE_WIDTH,
                    BENCH_IMAGE_HEIGHT,
                    BENCH_IMAGE_STRIDE,
                )?;
                sequence = sequence.saturating_add(1);
                sleep_cancellable(period, &token);
            }
            Ok::<(), anyhow::Error>(())
        },
        || {},
    );
    Ok(())
}

fn activate_mixed(ctx: &mut ActivationCtx<'_>) -> Result<()> {
    let image_capability = CapabilityInstanceId::new();
    let image_descriptor = SliceStream::<Image>::descriptor(
        image_capability.stream_service_name(),
        StreamProfile::Sensor,
    );
    let image = SliceStream::<Image>::publisher(&image_descriptor, SMALL_IMAGE_BYTES)?;
    ctx.provide::<Camera>(image_capability, image_descriptor);

    let point_capability = CapabilityInstanceId::new();
    let point_descriptor = SliceStream::<PointCloud>::descriptor(
        point_capability.stream_service_name(),
        StreamProfile::Sensor,
    );
    let points = SliceStream::<PointCloud>::publisher(&point_descriptor, 4)?;
    ctx.provide::<Detections>(point_capability, point_descriptor);

    let control_capability = CapabilityInstanceId::new();
    let control_descriptor = Stream::<ControlCommand>::descriptor(
        control_capability.stream_service_name(),
        StreamProfile::Control,
    );
    let control = Stream::<ControlCommand>::publisher(&control_descriptor)?;
    ctx.provide::<MotionCommand>(control_capability, control_descriptor);

    ctx.spawn_critical_blocking(
        "publish-mixed-mcap-source",
        move |token| {
            let rate_hz = env_or_default("RBT_MCAP_SOURCE_RATE_HZ", 20.0_f64)?;
            let period = checked_period(rate_hz)?;
            let mut sequence = 1_u64;
            while !token.is_cancelled() {
                publish_image(
                    &image,
                    sequence,
                    SMALL_IMAGE_WIDTH,
                    SMALL_IMAGE_HEIGHT,
                    SMALL_IMAGE_STRIDE,
                )?;
                publish_points(&points, sequence, 4)?;
                let mut command = control.loan()?;
                command.sequence = sequence;
                command.value = sequence as f32 * 0.25;
                command.reserved = 0x5242_5400 | sequence as u32 & 0xff;
                let control_recipients = command.send()?;
                println!(
                    "MCAP_SOURCE mixed sequence={sequence} control_recipients={control_recipients}"
                );
                sequence = sequence.saturating_add(1);
                sleep_cancellable(period, &token);
            }
            Ok::<(), anyhow::Error>(())
        },
        || {},
    );
    Ok(())
}

fn activate_image(ctx: &mut ActivationCtx<'_>) -> Result<()> {
    let capability = CapabilityInstanceId::new();
    let descriptor =
        SliceStream::<Image>::descriptor(capability.stream_service_name(), StreamProfile::Sensor);
    let publisher = SliceStream::<Image>::publisher(&descriptor, LARGE_IMAGE_BYTES)?;
    ctx.provide::<Camera>(capability, descriptor);
    ctx.spawn_critical_blocking(
        "publish-large-image",
        move |token| {
            let rate_hz = env_or_default("RBT_MCAP_SOURCE_RATE_HZ", 10.0_f64)?;
            let period = checked_period(rate_hz)?;
            let mut sequence = 1_u64;
            while !token.is_cancelled() {
                let recipients = publish_image(
                    &publisher,
                    sequence,
                    LARGE_IMAGE_WIDTH,
                    LARGE_IMAGE_HEIGHT,
                    LARGE_IMAGE_STRIDE,
                )?;
                println!(
                    "MCAP_SOURCE large-image sequence={sequence} bytes={LARGE_IMAGE_BYTES} recipients={recipients}"
                );
                sequence = sequence.saturating_add(1);
                sleep_cancellable(period, &token);
            }
            Ok::<(), anyhow::Error>(())
        },
        || {},
    );
    Ok(())
}

fn activate_pointcloud(ctx: &mut ActivationCtx<'_>) -> Result<()> {
    let count = env_or_default("RBT_MCAP_SOURCE_POINT_COUNT", 100_000_usize)?;
    if count == 0 || count > 2_000_000 || count > u32::MAX as usize {
        bail!("RBT_MCAP_SOURCE_POINT_COUNT must be in 1..=2000000");
    }
    let capability = CapabilityInstanceId::new();
    let descriptor = SliceStream::<PointCloud>::descriptor(
        capability.stream_service_name(),
        StreamProfile::Sensor,
    );
    let publisher = SliceStream::<PointCloud>::publisher(&descriptor, count)?;
    ctx.provide::<Detections>(capability, descriptor);
    ctx.spawn_critical_blocking(
        "publish-large-pointcloud",
        move |token| {
            let rate_hz = env_or_default("RBT_MCAP_SOURCE_RATE_HZ", 5.0_f64)?;
            let period = checked_period(rate_hz)?;
            let mut sequence = 1_u64;
            while !token.is_cancelled() {
                let recipients = publish_points(&publisher, sequence, count)?;
                println!(
                    "MCAP_SOURCE large-pointcloud sequence={sequence} points={count} bytes={} recipients={recipients}",
                    count * std::mem::size_of::<PointXYZI>(),
                );
                sequence = sequence.saturating_add(1);
                sleep_cancellable(period, &token);
            }
            Ok::<(), anyhow::Error>(())
        },
        || {},
    );
    Ok(())
}

fn publish_image(
    publisher: &rbt::stream::SlicePublisher<Image>,
    sequence: u64,
    width: u32,
    height: u32,
    stride: u32,
) -> Result<usize> {
    let payload_bytes = stride as usize * height as usize;
    let mut sample = publisher.loan_slice(payload_bytes)?;
    sample.header_mut().sequence = sequence;
    sample.header_mut().timestamp_ns = sequence.saturating_mul(1_000_000);
    sample.header_mut().width = width;
    sample.header_mut().height = height;
    sample.header_mut().stride = stride;
    sample.header_mut().encoding = PixelFormat::RGB8;
    for (index, byte) in sample.payload_mut().iter_mut().enumerate() {
        *byte = sequence.wrapping_add(index as u64) as u8;
    }
    sample.send().map_err(Into::into)
}

fn publish_points(
    publisher: &rbt::stream::SlicePublisher<PointCloud>,
    sequence: u64,
    count: usize,
) -> Result<usize> {
    let mut sample = publisher.loan_slice(count)?;
    sample.header_mut().sequence = sequence;
    sample.header_mut().timestamp_ns = sequence.saturating_mul(1_000_000);
    sample.header_mut().width = count as u32;
    sample.header_mut().height = 1;
    sample.header_mut().dense = 1;
    sample.header_mut().reserved = [0; 7];
    for (index, point) in sample.payload_mut().iter_mut().enumerate() {
        let value = index as f32;
        *point = PointXYZI {
            x: value,
            y: value * 2.0,
            z: -value,
            intensity: (index % 31) as f32,
        };
    }
    sample.send().map_err(Into::into)
}

fn checked_period(rate_hz: f64) -> Result<Duration> {
    if !rate_hz.is_finite() || rate_hz <= 0.0 {
        bail!("RBT_MCAP_SOURCE_RATE_HZ must be finite and positive");
    }
    Duration::try_from_secs_f64(1.0 / rate_hz)
        .map_err(|error| anyhow!("invalid RBT_MCAP_SOURCE_RATE_HZ: {error}"))
}

fn sleep_cancellable(duration: Duration, token: &tokio_util::sync::CancellationToken) {
    let deadline = std::time::Instant::now() + duration;
    while !token.is_cancelled() && std::time::Instant::now() < deadline {
        std::thread::sleep(
            deadline
                .saturating_duration_since(std::time::Instant::now())
                .min(Duration::from_millis(5)),
        );
    }
}

fn env_or_default<T>(name: &str, default: T) -> Result<T>
where
    T: FromStr,
    T::Err: Display,
{
    match std::env::var(name) {
        Ok(value) => value
            .parse()
            .map_err(|error| anyhow!("invalid {name}={value:?}: {error}")),
        Err(std::env::VarError::NotPresent) => Ok(default),
        Err(error) => Err(anyhow!("failed to read {name}: {error}")),
    }
}

fn composer_addr() -> String {
    std::env::var("RBT_COMPOSER_ADDR").unwrap_or_else(|_| "127.0.0.1:7447".to_owned())
}

fn init_tracing() {
    tracing_subscriber::fmt()
        .with_env_filter(
            EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info")),
        )
        // Plain text when stdout is redirected to a log file or supervisor.
        .with_ansi(std::io::IsTerminal::is_terminal(&std::io::stdout()))
        .init();
}

#[tokio::main]
async fn main() -> Result<()> {
    init_tracing();
    let mode = match std::env::args().nth(1).as_deref() {
        Some("mixed") => Mode::Mixed,
        Some("small-image") => Mode::SmallImage,
        Some("large-image") => Mode::LargeImage,
        Some("large-pointcloud") => Mode::LargePointCloud,
        _ => bail!("usage: mcap-source <mixed|small-image|large-image|large-pointcloud>"),
    };
    run_component::<Source, _>(mode, composer_addr())
        .await
        .map_err(Into::into)
}

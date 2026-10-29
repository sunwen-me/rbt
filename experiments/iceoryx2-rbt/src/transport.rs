use std::{
    fmt::Debug,
    thread,
    time::{Duration, Instant},
};

use anyhow::{Context, Result, bail, ensure};
use clap::{Args, ValueEnum};
use cross_host_upstream_probe::{ImageHeader, PixelFormat, PointCloudHeader, PointXYZI};
use iceoryx2::prelude::*;

pub fn reject_mismatch(service: &str) -> Result<()> {
    use iceoryx2::service::builder::publish_subscribe::PublishSubscribeOpenError;
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let name = ServiceName::new(service)?;
    let deadline = Instant::now() + Duration::from_secs(15);
    loop {
        match node
            .service_builder(&name)
            .publish_subscribe::<u64>()
            .open()
        {
            Err(PublishSubscribeOpenError::IncompatibleTypes) => {
                println!(
                    "SUMMARY kind=schema-mismatch result=PASS error=IncompatibleTypes sample_endpoints_opened=0 service={service}"
                );
                return Ok(());
            }
            Err(PublishSubscribeOpenError::DoesNotExist) if Instant::now() < deadline => {
                thread::sleep(Duration::from_millis(2));
            }
            Err(error) => bail!("expected schema mismatch, got {error}"),
            Ok(_) => bail!("incompatible fixed type opened the dynamic Image mirror"),
        }
    }
}

#[derive(Clone, Copy, Debug, ValueEnum)]
pub enum Kind {
    Image,
    Padded,
    Bytes,
    Points,
}

#[derive(Clone, Debug, Args)]
pub struct Options {
    #[arg(long)]
    pub service: String,
    #[arg(long, value_enum, default_value_t = Kind::Image)]
    pub kind: Kind,
    #[arg(long, default_value_t = 100)]
    pub count: usize,
    #[arg(long, default_value_t = 1024)]
    pub elements: usize,
    #[arg(long, default_value_t = 100)]
    pub period_ms: u64,
    #[arg(long, default_value_t = 2000)]
    pub start_delay_ms: u64,
    #[arg(long, default_value_t = 2000)]
    pub hold_ms: u64,
    #[arg(long, default_value_t = 60000)]
    pub timeout_ms: u64,
    #[arg(long, default_value_t = false)]
    pub control: bool,
    #[arg(long, default_value_t = false)]
    pub expect_all: bool,
    #[arg(long, default_value_t = 0)]
    pub slow_ms: u64,
}

trait Shape {
    type Header: ZeroCopySend + Copy + Debug + Default + 'static;
    type Element: ZeroCopySend + Copy + Debug + Default + 'static;
    fn length(&self) -> usize;
    fn header(&self, sequence: u64) -> Self::Header;
    fn element(&self, sequence: u64, index: usize) -> Self::Element;
    fn validate(&self, header: &Self::Header, payload: &[Self::Element]) -> Result<(u64, u64)>;
    fn label(&self) -> &'static str;
}

struct ImageShape {
    width: u32,
    height: u32,
    stride: u32,
    encoding: PixelFormat,
}

impl Shape for ImageShape {
    type Header = ImageHeader;
    type Element = u8;
    fn length(&self) -> usize {
        self.stride as usize * self.height as usize
    }
    fn header(&self, sequence: u64) -> ImageHeader {
        ImageHeader {
            sequence,
            timestamp_ns: sequence * 1_000_000,
            width: self.width,
            height: self.height,
            stride: self.stride,
            encoding: self.encoding,
        }
    }
    fn element(&self, sequence: u64, index: usize) -> u8 {
        sequence.wrapping_add(index as u64) as u8
    }
    fn validate(&self, header: &ImageHeader, payload: &[u8]) -> Result<(u64, u64)> {
        let expected = self.header(header.sequence);
        ensure!(
            header.timestamp_ns == expected.timestamp_ns
                && header.width == expected.width
                && header.height == expected.height
                && header.stride == expected.stride
                && header.encoding == expected.encoding,
            "Image header mismatch: {header:?}"
        );
        ensure!(payload.len() == self.length(), "Image length mismatch");
        let mut checksum = 0u64;
        for (index, byte) in payload.iter().enumerate() {
            ensure!(
                *byte == self.element(header.sequence, index),
                "Image byte mismatch at {index}"
            );
            checksum = checksum.wrapping_add(u64::from(*byte));
        }
        Ok((header.sequence, checksum))
    }
    fn label(&self) -> &'static str {
        "Image"
    }
}

struct CloudShape(usize);
impl Shape for CloudShape {
    type Header = PointCloudHeader;
    type Element = PointXYZI;
    fn length(&self) -> usize {
        self.0
    }
    fn header(&self, sequence: u64) -> PointCloudHeader {
        PointCloudHeader {
            sequence,
            timestamp_ns: sequence * 1_000_000,
            width: self.0 as u32,
            height: 1,
            dense: 1,
            reserved: [0; 7],
        }
    }
    fn element(&self, sequence: u64, index: usize) -> PointXYZI {
        PointXYZI {
            x: index as f32,
            y: sequence as f32,
            z: -(index as f32),
            intensity: (index % 31) as f32,
        }
    }
    fn validate(&self, header: &PointCloudHeader, payload: &[PointXYZI]) -> Result<(u64, u64)> {
        ensure!(
            header.timestamp_ns == header.sequence * 1_000_000
                && header.width == self.0 as u32
                && header.height == 1
                && header.dense == 1
                && header.reserved == [0; 7],
            "PointCloud header mismatch"
        );
        ensure!(payload.len() == self.0, "PointCloud length mismatch");
        let mut checksum = 0u64;
        for (index, point) in payload.iter().enumerate() {
            let expected = self.element(header.sequence, index);
            let actual = [
                point.x.to_bits(),
                point.y.to_bits(),
                point.z.to_bits(),
                point.intensity.to_bits(),
            ];
            ensure!(
                actual
                    == [
                        expected.x.to_bits(),
                        expected.y.to_bits(),
                        expected.z.to_bits(),
                        expected.intensity.to_bits()
                    ],
                "PointCloud point mismatch at {index}"
            );
            checksum = actual
                .into_iter()
                .fold(checksum, |sum, value| sum.wrapping_add(u64::from(value)));
        }
        Ok((header.sequence, checksum))
    }
    fn label(&self) -> &'static str {
        "PointCloud"
    }
}

fn image_shape(options: &Options) -> Result<ImageShape> {
    Ok(match options.kind {
        Kind::Image => ImageShape {
            width: 1920,
            height: 1080,
            stride: 5760,
            encoding: PixelFormat(1),
        },
        Kind::Padded => ImageShape {
            width: 640,
            height: 480,
            stride: 2048,
            encoding: PixelFormat(1),
        },
        Kind::Bytes => ImageShape {
            width: u32::try_from(options.elements)?,
            height: 1,
            stride: u32::try_from(options.elements)?,
            encoding: PixelFormat(2),
        },
        Kind::Points => bail!("PointCloud has its own shape"),
    })
}

pub fn publish(options: Options) -> Result<()> {
    match options.kind {
        Kind::Points => {
            u32::try_from(options.elements)?;
            publish_shape(&options, CloudShape(options.elements))
        }
        _ => publish_shape(&options, image_shape(&options)?),
    }
}

fn publish_shape<M: Shape>(options: &Options, shape: M) -> Result<()> {
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let name = ServiceName::new(&options.service)?;
    let ready = ServiceName::new(&format!("{}/ready", options.service))?;
    let depth = if options.control { 4 } else { 1 };
    let factory = node
        .service_builder(&name)
        .publish_subscribe::<[M::Element]>()
        .user_header::<M::Header>()
        .subscriber_max_buffer_size(depth)
        .subscriber_max_borrowed_samples(1)
        .enable_safe_overflow(!options.control)
        .history_size(0)
        .open_or_create()?;
    let event = node.service_builder(&ready).event().open_or_create()?;
    let notifier = event.notifier_builder().create()?;
    let publisher = factory
        .publisher_builder()
        .initial_max_slice_len(shape.length().max(1))
        .max_loaned_samples(2)
        .backpressure_strategy(BackpressureStrategy::DiscardData)
        .create()?;
    println!(
        "READY role=publisher service={} type={} count={} elements={} queue={} overflow={}",
        options.service,
        shape.label(),
        options.count,
        shape.length(),
        depth,
        !options.control
    );
    thread::sleep(Duration::from_millis(options.start_delay_ms));
    let mut delivered = 0usize;
    for sequence in 0..options.count as u64 {
        let mut sample = publisher
            .loan_slice_uninit(shape.length())?
            .write_from_fn(|index| shape.element(sequence, index));
        *sample.user_header_mut() = shape.header(sequence);
        delivered += sample.send()?;
        // Only the source-local readiness is sent. The maintained link consumes
        // it locally and reconstructs destination readiness after SHM insertion.
        notifier.notify()?;
        if options.period_ms > 0 {
            thread::sleep(Duration::from_millis(options.period_ms));
        }
    }
    thread::sleep(Duration::from_millis(options.hold_ms));
    println!(
        "SUMMARY role=publisher service={} samples={} deliveries={} local_ready={} network_ready=disabled_by_pair",
        options.service, options.count, delivered, options.count
    );
    Ok(())
}

pub fn consume(options: Options) -> Result<()> {
    ensure!(options.count > 0, "consumer count must be positive");
    match options.kind {
        Kind::Points => consume_shape(&options, CloudShape(options.elements)),
        _ => consume_shape(&options, image_shape(&options)?),
    }
}

fn consume_shape<M: Shape>(options: &Options, shape: M) -> Result<()> {
    let deadline = Instant::now() + Duration::from_millis(options.timeout_ms);
    let node = NodeBuilder::new().create::<ipc_threadsafe::Service>()?;
    let name = ServiceName::new(&options.service)?;
    let ready = ServiceName::new(&format!("{}/ready", options.service))?;
    let event = loop {
        match node.service_builder(&ready).event().open() {
            Ok(factory) => break factory,
            Err(error) if Instant::now() < deadline => {
                let _ = error;
                thread::sleep(Duration::from_millis(2));
            }
            Err(error) => return Err(error).context("readiness mirror never arrived"),
        }
    };
    let listener = event.listener_builder().create()?;
    let depth = if options.control { 4 } else { 1 };
    let factory = loop {
        match node
            .service_builder(&name)
            .publish_subscribe::<[M::Element]>()
            .user_header::<M::Header>()
            .subscriber_max_buffer_size(depth)
            .subscriber_max_borrowed_samples(1)
            .enable_safe_overflow(!options.control)
            .history_size(0)
            .open()
        {
            Ok(factory) => break factory,
            Err(error) if Instant::now() < deadline => {
                let _ = error;
                thread::sleep(Duration::from_millis(2));
            }
            Err(error) => return Err(error).context("typed mirror with exact QoS never arrived"),
        }
    };
    let subscriber = factory.subscriber_builder().buffer_size(depth).create()?;
    let waitset = WaitSetBuilder::new().create::<ipc_threadsafe::Service>()?;
    let _guard = waitset.attach_notification(&listener)?;
    println!(
        "READY role=subscriber service={} type={} queue={} overflow={} strategy=WaitSet initial_drain=0",
        options.service,
        shape.label(),
        depth,
        !options.control
    );
    let mut received = 0usize;
    let mut last = None;
    let mut checksum = 0u64;
    let mut wakes = 0usize;
    let mut spurious = 0usize;
    while last != Some(options.count as u64 - 1) {
        ensure!(
            Instant::now() < deadline,
            "WaitSet timeout received={received} last={last:?}"
        );
        let mut woke = false;
        waitset.wait_and_process_once_with_timeout(
            |_| {
                woke = true;
                CallbackProgression::Continue
            },
            deadline.saturating_duration_since(Instant::now()),
        )?;
        if !woke {
            continue;
        }
        wakes += 1;
        listener.try_wait(|activation| {
            assert_eq!(activation.id.as_value(), 0);
        })?;
        let mut drained = 0usize;
        while let Some(sample) = subscriber.receive()? {
            let (sequence, sum) = shape.validate(sample.user_header(), sample.payload())?;
            ensure!(
                last.is_none_or(|previous| sequence > previous),
                "duplicate or reordered payload"
            );
            ensure!(sequence < options.count as u64, "out-of-range sequence");
            if options.expect_all {
                ensure!(
                    sequence == received as u64,
                    "unexpected Sensor drop at {received}: {sequence}"
                );
            }
            checksum = checksum.wrapping_add(sum);
            last = Some(sequence);
            received += 1;
            drained += 1;
            if options.slow_ms > 0 {
                thread::sleep(Duration::from_millis(options.slow_ms));
            }
        }
        if drained == 0 {
            spurious += 1;
        }
    }
    if options.expect_all {
        ensure!(received == options.count, "missing expected samples");
    }
    println!(
        "SUMMARY role=subscriber service={} samples={} last={} wakes={} spurious={} checksum={} strategy=WaitSet payload_polling=false initial_drain=0 queue={} overflow={}",
        options.service,
        received,
        last.expect("final sample"),
        wakes,
        spurious,
        checksum,
        depth,
        !options.control
    );
    Ok(())
}

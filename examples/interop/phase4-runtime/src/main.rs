use std::{
    mem::{align_of, offset_of, size_of},
    sync::{Arc, Condvar, Mutex},
    thread,
    time::{Duration, Instant},
};

use anyhow::{Result, anyhow, bail};
use rbt_iox2::{
    ActivationId, ControlCommand, Image, ImageHeader, MessageTypeDescriptor, PixelFormat,
    PointCloud, PointCloudHeader, PointXYZI, ProcessInstanceId, SliceStream, Stream,
    StreamDescriptor, StreamProfile, StreamResourceConfig, WaitContext, WaitProgress, WaitToken,
};

const IMAGE_WIDTH: u32 = 1_920;
const IMAGE_HEIGHT: u32 = 1_080;
const IMAGE_CHANNELS: u32 = 3;
const IMAGE_STRIDE: u32 = IMAGE_WIDTH * IMAGE_CHANNELS;
const IMAGE_PAYLOAD_SIZE: usize = IMAGE_STRIDE as usize * IMAGE_HEIGHT as usize;
const IMAGE_FRAME_COUNT: u64 = 3;
const IMAGE_SMOKE_FRAME_COUNT: u64 = 60;
const IMAGE_SMOKE_PERIOD: Duration = Duration::from_nanos(33_333_333);
const POINT_COUNTS: [usize; 4] = [0, 1, 100, 100_000];

const POLL_INTERVAL: Duration = Duration::from_millis(1);
const OPEN_POLL_INTERVAL: Duration = Duration::from_millis(100);
const OPEN_TIMEOUT: Duration = Duration::from_secs(15);
const RECEIVE_TIMEOUT: Duration = Duration::from_secs(15);
const SEND_TIMEOUT: Duration = Duration::from_secs(15);

fn usage(program: &str) -> String {
    format!(
        "usage:\n  {program} layout\n  {program} image-publisher SERVICE [MAX_LOANS] [MAX_BORROWED]\n  {program} image-publisher-smoke SERVICE\n  {program} image-provider-hold SERVICE MILLIS\n  {program} image-subscriber SERVICE WIDTH HEIGHT STRIDE FRAMES [MAX_LOANS] [MAX_BORROWED]\n  {program} pointcloud-subscriber SERVICE\n  {program} control-subscriber SERVICE MILLIS"
    )
}

fn take_required(args: &mut impl Iterator<Item = String>, name: &str) -> Result<String> {
    args.next()
        .ok_or_else(|| anyhow!("missing {name} argument"))
}

fn reject_extra(args: &mut impl Iterator<Item = String>) -> Result<()> {
    if let Some(extra) = args.next() {
        bail!("unexpected argument {extra:?}");
    }
    Ok(())
}

fn parse_millis(value: String) -> Result<u64> {
    value
        .parse::<u64>()
        .map_err(|error| anyhow!("invalid MILLIS={value:?}: {error}"))
}

fn parse_u32(value: String, name: &str) -> Result<u32> {
    value
        .parse::<u32>()
        .map_err(|error| anyhow!("invalid {name}={value:?}: {error}"))
}

fn parse_u64(value: String, name: &str) -> Result<u64> {
    value
        .parse::<u64>()
        .map_err(|error| anyhow!("invalid {name}={value:?}: {error}"))
}

fn parse_usize(value: String, name: &str) -> Result<usize> {
    let value = parse_u64(value, name)?;
    usize::try_from(value).map_err(|error| anyhow!("invalid {name}={value}: {error}"))
}

fn parse_resources(args: &mut impl Iterator<Item = String>) -> Result<StreamResourceConfig> {
    let max_loans = args
        .next()
        .map(|value| parse_usize(value, "MAX_LOANS"))
        .transpose()?
        .unwrap_or(StreamResourceConfig::DEFAULT_MAX_LOANS);
    let max_borrowed = args
        .next()
        .map(|value| parse_usize(value, "MAX_BORROWED"))
        .transpose()?
        .unwrap_or(StreamResourceConfig::DEFAULT_MAX_BORROWED);
    reject_extra(args)?;
    Ok(StreamResourceConfig::new(max_loans, max_borrowed))
}

fn parse_command() -> Result<Command> {
    let mut args = std::env::args();
    let program = args.next().unwrap_or_else(|| "phase4-interop".to_owned());
    let Some(subcommand) = args.next() else {
        bail!("missing subcommand\n{}", usage(&program));
    };

    let command = match subcommand.as_str() {
        "layout" => {
            reject_extra(&mut args)?;
            Command::Layout
        }
        "image-publisher" => {
            let service = take_required(&mut args, "SERVICE")?;
            let resources = parse_resources(&mut args)?;
            Command::ImagePublisher { service, resources }
        }
        "image-publisher-smoke" => {
            let service = take_required(&mut args, "SERVICE")?;
            reject_extra(&mut args)?;
            Command::ImagePublisherSmoke { service }
        }
        "image-provider-hold" => {
            let service = take_required(&mut args, "SERVICE")?;
            let millis = parse_millis(take_required(&mut args, "MILLIS")?)?;
            reject_extra(&mut args)?;
            Command::ImageProviderHold { service, millis }
        }
        "image-subscriber" => {
            let service = take_required(&mut args, "SERVICE")?;
            let width = parse_u32(take_required(&mut args, "WIDTH")?, "WIDTH")?;
            let height = parse_u32(take_required(&mut args, "HEIGHT")?, "HEIGHT")?;
            let stride = parse_u32(take_required(&mut args, "STRIDE")?, "STRIDE")?;
            let frames = parse_u64(take_required(&mut args, "FRAMES")?, "FRAMES")?;
            let resources = parse_resources(&mut args)?;
            Command::ImageSubscriber {
                service,
                width,
                height,
                stride,
                frames,
                resources,
            }
        }
        "pointcloud-subscriber" => {
            let service = take_required(&mut args, "SERVICE")?;
            reject_extra(&mut args)?;
            Command::PointCloudSubscriber { service }
        }
        "control-subscriber" => {
            let service = take_required(&mut args, "SERVICE")?;
            let millis = parse_millis(take_required(&mut args, "MILLIS")?)?;
            reject_extra(&mut args)?;
            Command::ControlSubscriber { service, millis }
        }
        other => bail!("unknown subcommand {other:?}\n{}", usage(&program)),
    };

    Ok(command)
}

enum Command {
    Layout,
    ImagePublisher {
        service: String,
        resources: StreamResourceConfig,
    },
    ImagePublisherSmoke {
        service: String,
    },
    ImageProviderHold {
        service: String,
        millis: u64,
    },
    ImageSubscriber {
        service: String,
        width: u32,
        height: u32,
        stride: u32,
        frames: u64,
        resources: StreamResourceConfig,
    },
    PointCloudSubscriber {
        service: String,
    },
    ControlSubscriber {
        service: String,
        millis: u64,
    },
}

fn image_descriptor(service: &str) -> StreamDescriptor {
    SliceStream::<Image>::descriptor(service.to_owned(), StreamProfile::Sensor)
}

fn image_descriptor_with_resources(
    service: &str,
    resources: StreamResourceConfig,
) -> StreamDescriptor {
    StreamDescriptor::for_slice_with_resources::<Image>(
        service.to_owned(),
        StreamProfile::Sensor,
        resources,
    )
}

fn pointcloud_descriptor(service: &str) -> StreamDescriptor {
    SliceStream::<PointCloud>::descriptor(service.to_owned(), StreamProfile::Sensor)
}

fn control_descriptor(service: &str) -> StreamDescriptor {
    Stream::<ControlCommand>::descriptor(service.to_owned(), StreamProfile::Control)
}

fn print_descriptor(label: &str, descriptor: &MessageTypeDescriptor) {
    println!(
        "DESCRIPTOR type={label} namespace={} name={} abi_major={} shape={} header_size={} header_align={} element_size={} element_align={} layout_hash={}",
        descriptor.namespace,
        descriptor.name,
        descriptor.abi_major,
        descriptor.shape.as_str(),
        descriptor.header_size,
        descriptor.header_align,
        descriptor.element_size,
        descriptor.element_align,
        descriptor.layout_hash,
    );
}

fn print_layout() {
    println!(
        "LAYOUT type=ImageHeader size={} align={} sequence_offset={} timestamp_ns_offset={} width_offset={} height_offset={} stride_offset={} encoding_offset={}",
        size_of::<ImageHeader>(),
        align_of::<ImageHeader>(),
        offset_of!(ImageHeader, sequence),
        offset_of!(ImageHeader, timestamp_ns),
        offset_of!(ImageHeader, width),
        offset_of!(ImageHeader, height),
        offset_of!(ImageHeader, stride),
        offset_of!(ImageHeader, encoding),
    );
    println!(
        "LAYOUT type=PointCloudHeader size={} align={} sequence_offset={} timestamp_ns_offset={} width_offset={} height_offset={} dense_offset={} reserved_offset={}",
        size_of::<PointCloudHeader>(),
        align_of::<PointCloudHeader>(),
        offset_of!(PointCloudHeader, sequence),
        offset_of!(PointCloudHeader, timestamp_ns),
        offset_of!(PointCloudHeader, width),
        offset_of!(PointCloudHeader, height),
        offset_of!(PointCloudHeader, dense),
        offset_of!(PointCloudHeader, reserved),
    );
    println!(
        "LAYOUT type=PointXYZI size={} align={} x_offset={} y_offset={} z_offset={} intensity_offset={}",
        size_of::<PointXYZI>(),
        align_of::<PointXYZI>(),
        offset_of!(PointXYZI, x),
        offset_of!(PointXYZI, y),
        offset_of!(PointXYZI, z),
        offset_of!(PointXYZI, intensity),
    );
    println!(
        "LAYOUT type=ControlCommand size={} align={} sequence_offset={} value_offset={} reserved_offset={}",
        size_of::<ControlCommand>(),
        align_of::<ControlCommand>(),
        offset_of!(ControlCommand, sequence),
        offset_of!(ControlCommand, value),
        offset_of!(ControlCommand, reserved),
    );

    let image = image_descriptor("rbt/phase4/layout");
    let pointcloud = pointcloud_descriptor("rbt/phase4/layout");
    let control = control_descriptor("rbt/phase4/layout");
    print_descriptor("Image", image.message_type());
    print_descriptor("PointCloud", pointcloud.message_type());
    print_descriptor("ControlCommand", control.message_type());
}

fn image_byte(sequence: u64, index: usize) -> u8 {
    sequence.wrapping_add(index as u64) as u8
}

fn image_checksum(sequence: u64, payload_len: usize) -> u64 {
    (0..payload_len).fold(0_u64, |checksum, index| {
        checksum.wrapping_add(u64::from(image_byte(sequence, index)))
    })
}

fn send_image_frame(
    publisher: &rbt_iox2::SlicePublisher<Image>,
    sequence: u64,
) -> Result<(usize, u64)> {
    let mut sample = publisher.loan_slice(IMAGE_PAYLOAD_SIZE)?;
    {
        let header = sample.header_mut();
        header.sequence = sequence;
        header.timestamp_ns = sequence * 1_000_000;
        header.width = IMAGE_WIDTH;
        header.height = IMAGE_HEIGHT;
        header.stride = IMAGE_STRIDE;
        header.encoding = PixelFormat::RGB8;
    }
    let payload = sample.payload_mut();
    if payload.len() != IMAGE_PAYLOAD_SIZE {
        bail!(
            "image publisher loaned payload={} bytes; expected {}",
            payload.len(),
            IMAGE_PAYLOAD_SIZE
        );
    }
    let mut checksum = 0_u64;
    for (index, byte) in payload.iter_mut().enumerate() {
        let value = image_byte(sequence, index);
        *byte = value;
        checksum = checksum.wrapping_add(u64::from(value));
    }
    let expected_checksum = image_checksum(sequence, payload.len());
    if checksum != expected_checksum {
        bail!(
            "image checksum calculation failed: sequence={sequence} expected={expected_checksum} actual={checksum}"
        );
    }
    let recipients = sample.send()?;
    Ok((recipients, checksum))
}

fn publish_image(service: &str, resources: StreamResourceConfig) -> Result<()> {
    let descriptor = image_descriptor_with_resources(service, resources);
    let publisher =
        SliceStream::<Image>::publisher_with_resources(&descriptor, IMAGE_PAYLOAD_SIZE, resources)?;
    let layout_hash = descriptor.message_type().layout_hash;
    println!(
        "IMAGE_PUBLISHER ready service={} payload={} hash={} frames={IMAGE_FRAME_COUNT} max_loans={} max_borrowed={}",
        descriptor.service_name(),
        IMAGE_PAYLOAD_SIZE,
        layout_hash,
        resources.max_loans,
        resources.max_borrowed,
    );

    for sequence in 0..IMAGE_FRAME_COUNT {
        let deadline = Instant::now() + SEND_TIMEOUT;
        loop {
            if Instant::now() >= deadline {
                bail!(
                    "timed out sending image sequence={sequence} to service {:?} after {} seconds",
                    descriptor.service_name(),
                    SEND_TIMEOUT.as_secs()
                );
            }

            let (recipients, checksum) = send_image_frame(&publisher, sequence)?;
            if recipients > 0 {
                println!(
                    "IMAGE_PUBLISHER sequence={sequence} payload={} checksum={checksum} hash={} recipients={recipients}",
                    IMAGE_PAYLOAD_SIZE, layout_hash,
                );
                break;
            }

            thread::sleep(POLL_INTERVAL.min(deadline.saturating_duration_since(Instant::now())));
        }
        if sequence + 1 < IMAGE_FRAME_COUNT {
            thread::sleep(Duration::from_millis(100));
        }
    }

    println!(
        "IMAGE_PUBLISHER complete service={} frames={} payload={} hash={}",
        descriptor.service_name(),
        IMAGE_FRAME_COUNT,
        IMAGE_PAYLOAD_SIZE,
        layout_hash,
    );
    Ok(())
}

fn publish_image_smoke(service: &str) -> Result<()> {
    let descriptor = image_descriptor(service);
    let publisher = SliceStream::<Image>::publisher(&descriptor, IMAGE_PAYLOAD_SIZE)?;
    let layout_hash = descriptor.message_type().layout_hash;
    println!(
        "IMAGE_PUBLISHER_SMOKE ready service={} payload={} frames={} period_ns={} hash={}",
        descriptor.service_name(),
        IMAGE_PAYLOAD_SIZE,
        IMAGE_SMOKE_FRAME_COUNT,
        IMAGE_SMOKE_PERIOD.as_nanos(),
        layout_hash,
    );

    let started = Instant::now();
    let mut deadline = started;
    let mut sent = 0_u64;
    let mut delivered = 0_u64;
    let mut undelivered = 0_u64;

    for sequence in 0..IMAGE_SMOKE_FRAME_COUNT {
        if let Some(remaining) = deadline.checked_duration_since(Instant::now()) {
            thread::sleep(remaining);
        }

        let (recipients, _checksum) = send_image_frame(&publisher, sequence)?;
        sent += 1;
        if recipients > 0 {
            delivered += 1;
        } else {
            // Sensor sends are successful even when no subscriber receives the
            // sample; safe overflow also permits a slow subscriber to be
            // replaced without blocking this bounded publisher.
            undelivered += 1;
        }
        deadline += IMAGE_SMOKE_PERIOD;
    }

    println!(
        "IMAGE_PUBLISHER_SMOKE complete service={} sent={} delivered={} undelivered={} payload={} hash={}",
        descriptor.service_name(),
        sent,
        delivered,
        undelivered,
        IMAGE_PAYLOAD_SIZE,
        layout_hash,
    );
    Ok(())
}

fn hold_image_provider(service: &str, millis: u64) -> Result<()> {
    let descriptor = image_descriptor(service);
    let _publisher = SliceStream::<Image>::publisher(&descriptor, IMAGE_PAYLOAD_SIZE)?;
    println!(
        "IMAGE_PROVIDER_HOLD ready service={} millis={} payload={} hash={}",
        descriptor.service_name(),
        millis,
        IMAGE_PAYLOAD_SIZE,
        descriptor.message_type().layout_hash,
    );
    thread::sleep(Duration::from_millis(millis));
    println!(
        "IMAGE_PROVIDER_HOLD complete service={} millis={millis}",
        descriptor.service_name(),
    );
    Ok(())
}

fn image_payload_size(width: u32, height: u32, stride: u32) -> Result<usize> {
    if width == 0 || height == 0 {
        bail!("image dimensions must be greater than zero: width={width} height={height}");
    }
    let row_bytes = width
        .checked_mul(IMAGE_CHANNELS)
        .ok_or_else(|| anyhow!("WIDTH*{IMAGE_CHANNELS} overflowed"))?;
    if stride < row_bytes {
        bail!("STRIDE={stride} is smaller than row bytes={row_bytes}");
    }
    usize::try_from(stride)
        .map_err(|error| anyhow!("invalid STRIDE={stride}: {error}"))?
        .checked_mul(
            usize::try_from(height).map_err(|error| anyhow!("invalid HEIGHT={height}: {error}"))?,
        )
        .ok_or_else(|| anyhow!("STRIDE*HEIGHT overflowed: stride={stride} height={height}"))
}

fn open_image_subscriber(
    descriptor: &StreamDescriptor,
    resources: StreamResourceConfig,
) -> Result<rbt_iox2::SliceSubscriber<Image>> {
    let deadline = Instant::now() + OPEN_TIMEOUT;
    loop {
        match SliceStream::<Image>::subscriber_with_resources(descriptor, resources) {
            Ok(subscriber) => return Ok(subscriber),
            Err(error) if Instant::now() < deadline => {
                eprintln!(
                    "IMAGE_SUBSCRIBER waiting for existing service {:?}: {error}",
                    descriptor.service_name()
                );
                thread::sleep(
                    OPEN_POLL_INTERVAL.min(deadline.saturating_duration_since(Instant::now())),
                );
            }
            Err(error) => {
                return Err(anyhow!(
                    "timed out opening existing image service {:?} after {} seconds: {error}",
                    descriptor.service_name(),
                    OPEN_TIMEOUT.as_secs()
                ));
            }
        }
    }
}

fn validate_image_sample(
    sample: &rbt_iox2::ReceivedSliceSample<Image>,
    width: u32,
    height: u32,
    stride: u32,
    payload_size: usize,
) -> Result<(u64, [u8; 3])> {
    let header = sample.header();
    let expected_timestamp = header.sequence.saturating_mul(1_000_000);
    if header.timestamp_ns != expected_timestamp
        || header.width != width
        || header.height != height
        || header.stride != stride
        || header.encoding != PixelFormat::RGB8
    {
        bail!(
            "invalid image header: sequence={} timestamp_ns={} width={} height={} stride={} encoding={:?}; expected timestamp_ns={} width={} height={} stride={} encoding={:?}",
            header.sequence,
            header.timestamp_ns,
            header.width,
            header.height,
            header.stride,
            header.encoding,
            expected_timestamp,
            width,
            height,
            stride,
            PixelFormat::RGB8,
        );
    }

    let payload = sample.payload();
    if payload.len() != payload_size {
        bail!(
            "invalid image payload length: sequence={} expected={} actual={}",
            header.sequence,
            payload_size,
            payload.len()
        );
    }

    let checksum = payload.iter().fold(0_u64, |checksum, byte| {
        checksum.wrapping_add(u64::from(*byte))
    });
    let expected_checksum = image_checksum(header.sequence, payload.len());
    if checksum != expected_checksum {
        bail!(
            "invalid image checksum: sequence={} expected={} actual={}",
            header.sequence,
            expected_checksum,
            checksum
        );
    }

    let selected_indices = [0, payload.len() / 2, payload.len() - 1];
    let selected = selected_indices.map(|index| payload[index]);
    for (&index, &actual) in selected_indices.iter().zip(selected.iter()) {
        let expected = image_byte(header.sequence, index);
        if actual != expected {
            bail!(
                "invalid image byte: sequence={} index={} expected={} actual={}",
                header.sequence,
                index,
                expected,
                actual
            );
        }
    }

    let row_bytes = usize::try_from(width)
        .map_err(|error| anyhow!("invalid WIDTH={width}: {error}"))?
        .checked_mul(IMAGE_CHANNELS as usize)
        .ok_or_else(|| anyhow!("WIDTH*{IMAGE_CHANNELS} overflowed"))?;
    let stride =
        usize::try_from(stride).map_err(|error| anyhow!("invalid STRIDE={stride}: {error}"))?;
    if stride > row_bytes {
        for (row, chunk) in payload.chunks_exact(stride).enumerate() {
            for (padding_offset, &actual) in chunk[row_bytes..].iter().enumerate() {
                let index = row * stride + row_bytes + padding_offset;
                let expected = image_byte(header.sequence, index);
                if actual != expected {
                    bail!(
                        "invalid image padding byte: sequence={} row={} index={} expected={} actual={}",
                        header.sequence,
                        row,
                        index,
                        expected,
                        actual
                    );
                }
            }
        }
    }

    Ok((checksum, selected))
}

fn subscribe_image(
    service: &str,
    width: u32,
    height: u32,
    stride: u32,
    frames: u64,
    resources: StreamResourceConfig,
) -> Result<()> {
    let payload_size = image_payload_size(width, height, stride)?;
    let descriptor = image_descriptor_with_resources(service, resources);
    let subscriber = open_image_subscriber(&descriptor, resources)?;
    let layout_hash = descriptor.message_type().layout_hash;
    println!(
        "IMAGE_SUBSCRIBER ready service={} frames={} width={} height={} stride={} payload={} hash={} max_loans={} max_borrowed={}",
        descriptor.service_name(),
        frames,
        width,
        height,
        stride,
        payload_size,
        layout_hash,
        resources.max_loans,
        resources.max_borrowed,
    );

    if frames == 0 {
        println!(
            "IMAGE_SUBSCRIBER complete service={} frames=0 width={} height={} stride={} payload={} checksum=0 hash={}",
            descriptor.service_name(),
            width,
            height,
            stride,
            payload_size,
            layout_hash,
        );
        return Ok(());
    }

    let deadline = Instant::now() + RECEIVE_TIMEOUT;
    let mut context = WaitContext::new(ProcessInstanceId::new(), ActivationId::first())?;
    let source = subscriber.event_source(WaitToken::new(1));
    // WaitContext currently exposes no deadline-aware run method. A
    // cancellable timer preserves the existing bound without polling.
    let timeout_done = Arc::new((Mutex::new(false), Condvar::new()));
    let timeout_done_for_thread = Arc::clone(&timeout_done);
    let wake_handle = context.wake_handle();
    let mut received = 0_u64;
    let mut previous_sequence = None;
    let mut last_checksum = 0_u64;
    let wait_result = thread::scope(|scope| {
        let timer = scope.spawn(move || -> Result<()> {
            let (done, condition) = &*timeout_done_for_thread;
            let done = done
                .lock()
                .map_err(|_| anyhow!("image wait timeout state was poisoned"))?;
            let (done, timeout) = condition
                .wait_timeout_while(
                    done,
                    deadline.saturating_duration_since(Instant::now()),
                    |finished| !*finished,
                )
                .map_err(|_| anyhow!("image wait timeout state was poisoned"))?;
            if timeout.timed_out() && !*done {
                wake_handle
                    .wake()
                    .map_err(|error| anyhow!("wake image wait context at timeout: {error}"))?;
            }
            Ok(())
        });
        let wait_result = context.run(std::slice::from_ref(&source), |_event, budget| {
            if budget == 0 {
                return Ok(WaitProgress::Continue);
            }
            let mut drained = 0usize;
            for _ in 0..budget {
                if received >= frames {
                    return Ok(WaitProgress::stop_after_samples(drained));
                }
                if Instant::now() >= deadline {
                    bail!(
                        "timed out waiting for image frames: received {} of {} from service {:?} after {} seconds",
                        received,
                        frames,
                        descriptor.service_name(),
                        RECEIVE_TIMEOUT.as_secs()
                    );
                }
                let Some(sample) = subscriber.receive()? else {
                    return Ok(if drained == 0 {
                        WaitProgress::Empty
                    } else {
                        WaitProgress::SamplesAndEmpty {
                            samples: drained,
                            empty_wakeups: 1,
                        }
                    });
                };

                let (checksum, selected) =
                    validate_image_sample(&sample, width, height, stride, payload_size)?;
                let sequence = sample.header().sequence;
                if let Some(previous_sequence) = previous_sequence
                    && sequence <= previous_sequence
                {
                    bail!(
                        "image sequence is not increasing: previous={} current={}",
                        previous_sequence,
                        sequence
                    );
                }
                previous_sequence = Some(sequence);
                println!(
                    "IMAGE_SUBSCRIBER frame={} sequence={} timestamp_ns={} width={} height={} stride={} payload={} checksum={} selected={},{},{} hash={}",
                    received,
                    sequence,
                    sample.header().timestamp_ns,
                    width,
                    height,
                    stride,
                    payload_size,
                    checksum,
                    selected[0],
                    selected[1],
                    selected[2],
                    layout_hash,
                );
                received += 1;
                last_checksum = checksum;
                drained += 1;
                if received >= frames {
                    return Ok(WaitProgress::stop_after_samples(drained));
                }
            }
            Ok(WaitProgress::Samples(drained))
        });

        if let Ok(mut done) = timeout_done.0.lock() {
            *done = true;
            timeout_done.1.notify_one();
        }
        let timer_result = timer
            .join()
            .map_err(|_| anyhow!("image wait timeout thread panicked"))?;
        timer_result?;
        wait_result
    })?;
    if wait_result.is_cancelled() && received < frames {
        bail!(
            "timed out waiting for image frames: received {} of {} from service {:?} after {} seconds",
            received,
            frames,
            descriptor.service_name(),
            RECEIVE_TIMEOUT.as_secs()
        );
    }

    println!(
        "IMAGE_SUBSCRIBER complete service={} frames={} width={} height={} stride={} payload={} checksum={} hash={}",
        descriptor.service_name(),
        received,
        width,
        height,
        stride,
        payload_size,
        last_checksum,
        layout_hash,
    );
    Ok(())
}

fn point_at(index: usize) -> PointXYZI {
    let value = index as f32;
    PointXYZI {
        x: value,
        y: value * 2.0,
        z: -value,
        intensity: (index % 31) as f32,
    }
}

fn point_checksum(point: PointXYZI) -> u64 {
    u64::from(point.x.to_bits())
        .wrapping_mul(31)
        .wrapping_add(u64::from(point.y.to_bits()))
        .wrapping_mul(31)
        .wrapping_add(u64::from(point.z.to_bits()))
        .wrapping_mul(31)
        .wrapping_add(u64::from(point.intensity.to_bits()))
}

fn points_checksum(points: &[PointXYZI]) -> u64 {
    points.iter().copied().fold(0_u64, |checksum, point| {
        checksum.wrapping_add(point_checksum(point))
    })
}

fn expected_points_checksum(count: usize) -> u64 {
    (0..count).fold(0_u64, |checksum, index| {
        checksum.wrapping_add(point_checksum(point_at(index)))
    })
}

fn validate_point(actual: PointXYZI, expected: PointXYZI, position: &str) -> Result<()> {
    if actual.x != expected.x
        || actual.y != expected.y
        || actual.z != expected.z
        || actual.intensity != expected.intensity
    {
        bail!(
            "point-cloud {position} point mismatch: expected=({},{},{},{}) actual=({},{},{},{})",
            expected.x,
            expected.y,
            expected.z,
            expected.intensity,
            actual.x,
            actual.y,
            actual.z,
            actual.intensity,
        );
    }
    Ok(())
}

fn open_pointcloud_subscriber(
    descriptor: &StreamDescriptor,
) -> Result<rbt_iox2::SliceSubscriber<PointCloud>> {
    let deadline = Instant::now() + OPEN_TIMEOUT;
    loop {
        match SliceStream::<PointCloud>::subscriber(descriptor) {
            Ok(subscriber) => return Ok(subscriber),
            Err(error) if Instant::now() < deadline => {
                eprintln!(
                    "POINTCLOUD_SUBSCRIBER waiting for existing service {:?}: {error}",
                    descriptor.service_name()
                );
                thread::sleep(
                    OPEN_POLL_INTERVAL.min(deadline.saturating_duration_since(Instant::now())),
                );
            }
            Err(error) => {
                return Err(anyhow!(
                    "timed out opening existing point-cloud service {:?} after {} seconds: {error}",
                    descriptor.service_name(),
                    OPEN_TIMEOUT.as_secs()
                ));
            }
        }
    }
}

fn receive_pointcloud(
    subscriber: &rbt_iox2::SliceSubscriber<PointCloud>,
    descriptor: &StreamDescriptor,
    sequence: u64,
    count: usize,
) -> Result<()> {
    let deadline = Instant::now() + RECEIVE_TIMEOUT;
    let mut context = WaitContext::new(ProcessInstanceId::new(), ActivationId::first())?;
    let source = subscriber.event_source(WaitToken::new(1));
    // WaitContext currently exposes no deadline-aware run method. A
    // cancellable timer preserves the existing bound without polling.
    let timeout_done = Arc::new((Mutex::new(false), Condvar::new()));
    let timeout_done_for_thread = Arc::clone(&timeout_done);
    let wake_handle = context.wake_handle();
    let wait_result = thread::scope(|scope| {
        let timer = scope.spawn(move || -> Result<()> {
            let (done, condition) = &*timeout_done_for_thread;
            let done = done
                .lock()
                .map_err(|_| anyhow!("point-cloud wait timeout state was poisoned"))?;
            let (done, timeout) = condition
                .wait_timeout_while(
                    done,
                    deadline.saturating_duration_since(Instant::now()),
                    |finished| !*finished,
                )
                .map_err(|_| anyhow!("point-cloud wait timeout state was poisoned"))?;
            if timeout.timed_out() && !*done {
                wake_handle.wake().map_err(|error| {
                    anyhow!("wake point-cloud wait context at timeout: {error}")
                })?;
            }
            Ok(())
        });
        let wait_result = context.run(std::slice::from_ref(&source), |_event, budget| {
            if budget == 0 {
                return Ok(WaitProgress::Continue);
            }
            if Instant::now() >= deadline {
                bail!(
                    "timed out waiting for point-cloud sequence={sequence} elements={count} from service {:?} after {} seconds",
                    descriptor.service_name(),
                    RECEIVE_TIMEOUT.as_secs()
                );
            }
            let Some(sample) = subscriber.receive()? else {
                return Ok(WaitProgress::Empty);
            };

            let header = sample.header();
            if header.sequence != sequence
                || header.timestamp_ns != sequence * 1_000_000
                || header.width != count as u32
                || header.height != 1
                || header.dense != 1
                || header.reserved != [0; 7]
            {
                bail!(
                    "invalid point-cloud header: expected sequence={} timestamp_ns={} width={} height=1 dense=1 reserved=[0;7], actual sequence={} timestamp_ns={} width={} height={} dense={} reserved={:?}",
                    sequence,
                    sequence * 1_000_000,
                    count,
                    header.sequence,
                    header.timestamp_ns,
                    header.width,
                    header.height,
                    header.dense,
                    header.reserved,
                );
            }

            let payload = sample.payload();
            if payload.len() != count {
                bail!(
                    "invalid point-cloud payload length: sequence={sequence} expected={count} actual={}",
                    payload.len()
                );
            }
            if let Some(first) = payload.first() {
                validate_point(*first, point_at(0), "first")?;
            }
            if let Some(last) = payload.last() {
                validate_point(*last, point_at(count - 1), "last")?;
            }
            let checksum = points_checksum(payload);
            let expected_checksum = expected_points_checksum(count);
            if checksum != expected_checksum {
                bail!(
                    "invalid point-cloud checksum: sequence={sequence} elements={count} expected={expected_checksum} actual={checksum}"
                );
            }
            println!(
                "POINTCLOUD_SUBSCRIBER sequence={sequence} elements={count} first={} last={} checksum={} hash={}",
                format_point(payload.first()),
                format_point(payload.last()),
                checksum,
                descriptor.message_type().layout_hash,
            );
            let drained = 1;
            Ok(WaitProgress::stop_after_samples(drained))
        });

        if let Ok(mut done) = timeout_done.0.lock() {
            *done = true;
            timeout_done.1.notify_one();
        }
        let timer_result = timer
            .join()
            .map_err(|_| anyhow!("point-cloud wait timeout thread panicked"))?;
        timer_result?;
        wait_result
    })?;
    if wait_result.is_cancelled() {
        bail!(
            "timed out waiting for point-cloud sequence={sequence} elements={count} from service {:?} after {} seconds",
            descriptor.service_name(),
            RECEIVE_TIMEOUT.as_secs()
        );
    }
    Ok(())
}

fn format_point(point: Option<&PointXYZI>) -> String {
    match point {
        Some(point) => format!("({},{},{},{})", point.x, point.y, point.z, point.intensity),
        None => "none".to_owned(),
    }
}

fn subscribe_pointcloud(service: &str) -> Result<()> {
    let descriptor = pointcloud_descriptor(service);
    let subscriber = open_pointcloud_subscriber(&descriptor)?;
    println!(
        "POINTCLOUD_SUBSCRIBER ready service={} counts=0,1,100,100000 hash={}",
        descriptor.service_name(),
        descriptor.message_type().layout_hash,
    );
    for (sequence, count) in POINT_COUNTS.into_iter().enumerate() {
        receive_pointcloud(&subscriber, &descriptor, sequence as u64, count)?;
    }
    println!(
        "POINTCLOUD_SUBSCRIBER complete service={} frames={} hash={}",
        descriptor.service_name(),
        POINT_COUNTS.len(),
        descriptor.message_type().layout_hash,
    );
    Ok(())
}

fn open_control_subscriber(
    descriptor: &StreamDescriptor,
) -> Result<rbt_iox2::Subscriber<ControlCommand>> {
    let deadline = Instant::now() + OPEN_TIMEOUT;
    loop {
        match Stream::<ControlCommand>::subscriber(descriptor) {
            Ok(subscriber) => return Ok(subscriber),
            Err(error) if Instant::now() < deadline => {
                eprintln!(
                    "CONTROL_SUBSCRIBER waiting for existing service {:?}: {error}",
                    descriptor.service_name()
                );
                thread::sleep(
                    OPEN_POLL_INTERVAL.min(deadline.saturating_duration_since(Instant::now())),
                );
            }
            Err(error) => {
                return Err(anyhow!(
                    "timed out opening existing control service {:?} after {} seconds: {error}",
                    descriptor.service_name(),
                    OPEN_TIMEOUT.as_secs()
                ));
            }
        }
    }
}

fn subscribe_control(service: &str, millis: u64) -> Result<()> {
    let descriptor = control_descriptor(service);
    let subscriber = open_control_subscriber(&descriptor)?;
    println!(
        "CONTROL_SUBSCRIBER ready service={} hold_millis={} queue_depth=4 hash={}",
        descriptor.service_name(),
        millis,
        descriptor.message_type().layout_hash,
    );
    thread::sleep(Duration::from_millis(millis));
    drop(subscriber);
    println!(
        "CONTROL_SUBSCRIBER complete service={} hold_millis={millis}",
        descriptor.service_name(),
    );
    Ok(())
}

fn main() -> Result<()> {
    match parse_command()? {
        Command::Layout => {
            print_layout();
            Ok(())
        }
        Command::ImagePublisher { service, resources } => publish_image(&service, resources),
        Command::ImagePublisherSmoke { service } => publish_image_smoke(&service),
        Command::ImageProviderHold { service, millis } => hold_image_provider(&service, millis),
        Command::ImageSubscriber {
            service,
            width,
            height,
            stride,
            frames,
            resources,
        } => subscribe_image(&service, width, height, stride, frames, resources),
        Command::PointCloudSubscriber { service } => subscribe_pointcloud(&service),
        Command::ControlSubscriber { service, millis } => subscribe_control(&service, millis),
    }
}

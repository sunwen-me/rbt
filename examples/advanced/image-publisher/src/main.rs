use std::{
    fmt::Display,
    str::FromStr,
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

use anyhow::{Result, anyhow};
use rbt_iox2::{
    Image, PixelFormat, SliceStream, StreamDescriptor, StreamProfile::Sensor, StreamResourceConfig,
};

const WIDTH: u32 = 1_920;
const HEIGHT: u32 = 1_080;
const CHANNELS: u32 = 3;
const STRIDE: u32 = WIDTH * CHANNELS;
const PAYLOAD_SIZE: usize = STRIDE as usize * HEIGHT as usize;

fn required_service_name() -> Result<String> {
    let service = std::env::var("RBT_IMAGE_SERVICE").map_err(|_| {
        anyhow!(
            "RBT_IMAGE_SERVICE is required; use a service such as rbt/cap/<CapabilityInstanceId>/stream"
        )
    })?;
    if service.trim().is_empty() {
        return Err(anyhow!(
            "RBT_IMAGE_SERVICE is required; use a service such as rbt/cap/<CapabilityInstanceId>/stream"
        ));
    }
    Ok(service)
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

fn timestamp_ns() -> Result<u64> {
    let elapsed = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|error| anyhow!("system clock is before UNIX_EPOCH: {error}"))?;
    u64::try_from(elapsed.as_nanos()).map_err(|_| anyhow!("system timestamp does not fit u64"))
}

fn main() -> Result<()> {
    let service = required_service_name()?;
    let frames: usize = env_or_default("RBT_IMAGE_FRAMES", 30)?;
    let rate_hz: f64 = env_or_default("RBT_IMAGE_RATE_HZ", 10.0)?;
    let start_delay_ms: u64 = env_or_default("RBT_IMAGE_START_DELAY_MS", 1_000)?;
    let max_held: usize = env_or_default("RBT_TEST_MAX_HELD", 1)?;

    if !rate_hz.is_finite() || rate_hz <= 0.0 {
        return Err(anyhow!(
            "RBT_IMAGE_RATE_HZ must be a finite positive number"
        ));
    }
    if max_held == 0 {
        return Err(anyhow!("RBT_TEST_MAX_HELD must be greater than zero"));
    }

    let resources = StreamResourceConfig::default().with_max_borrowed(max_held);
    let descriptor =
        StreamDescriptor::for_slice_with_resources::<Image>(service, Sensor, resources);
    let message_type = descriptor.message_type();
    let publisher =
        SliceStream::<Image>::publisher_with_resources(&descriptor, PAYLOAD_SIZE, resources)?;

    println!(
        "IMAGE_PUBLISHER message={} layout={} profile={:?} payload={} service={}",
        message_type.qualified_name(),
        message_type.layout_hash,
        descriptor.profile(),
        PAYLOAD_SIZE,
        descriptor.service_name(),
    );

    if frames == 0 {
        println!("IMAGE_PUBLISHER sent=0 elapsed=0.000s rate=0.00Hz");
        return Ok(());
    }

    thread::sleep(Duration::from_millis(start_delay_ms));

    let period = Duration::from_secs_f64(1.0 / rate_hz);
    let started = Instant::now();
    let mut next_frame = started;
    let mut sent = 0usize;

    for sequence in 0..frames as u64 {
        let now = Instant::now();
        if next_frame > now {
            thread::sleep(next_frame.duration_since(now));
        }
        next_frame += period;

        let mut sample = publisher.loan()?;
        {
            let header = sample.header_mut();
            header.sequence = sequence;
            header.timestamp_ns = timestamp_ns()?;
            header.width = WIDTH;
            header.height = HEIGHT;
            header.stride = STRIDE;
            header.encoding = PixelFormat::RGB8;
        }

        let payload = sample.payload_mut();
        if payload.len() != PAYLOAD_SIZE {
            return Err(anyhow!(
                "publisher loaned payload of {} bytes, expected {PAYLOAD_SIZE}",
                payload.len()
            ));
        }

        let mut checksum = 0_u64;
        for (i, byte) in payload.iter_mut().enumerate() {
            let value = ((sequence + i as u64) % 256) as u8;
            *byte = value;
            checksum = checksum.wrapping_add(u64::from(value));
        }
        let pointer = payload.as_ptr();
        let recipients = sample.send()?;

        println!(
            "IMAGE_PUBLISHER seq={} checksum={} message={} layout={} profile={:?} payload={} pointer={pointer:p} recipients={}",
            sequence,
            checksum,
            message_type.qualified_name(),
            message_type.layout_hash,
            descriptor.profile(),
            PAYLOAD_SIZE,
            recipients,
        );
        sent += 1;
    }

    let elapsed = started.elapsed().as_secs_f64();
    let rate = if elapsed > 0.0 {
        sent as f64 / elapsed
    } else {
        0.0
    };
    let pressure = publisher
        .pressure()
        .publisher
        .expect("publisher pressure snapshot must contain publisher metrics");
    println!(
        "IMAGE_PUBLISHER_PRESSURE loans_in_use={} loans_high_water={} loan_failures={} send_failures={} queue_full={} unable_to_deliver={} readiness_notify_failures={}",
        pressure.loans_in_use,
        pressure.loans_high_water,
        pressure.loan_failures,
        pressure.send_failures,
        pressure.queue_full,
        pressure.unable_to_deliver,
        pressure.readiness_notify_failures,
    );
    println!(
        "IMAGE_PUBLISHER sent={} elapsed={elapsed:.3}s rate={rate:.2}Hz",
        sent
    );
    Ok(())
}

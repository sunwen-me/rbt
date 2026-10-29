//! Test-only Rust publisher used by the Python SDK's multiprocess gates.

use std::{
    env,
    error::Error,
    io::{self, Write},
    thread,
    time::{Duration, Instant},
};

use rbt_iox2::{Image, PixelFormat, SliceStream, StreamProfile};

type Result<T> = std::result::Result<T, Box<dyn Error>>;

fn required(args: &mut impl Iterator<Item = String>, name: &str) -> Result<String> {
    args.next().ok_or_else(|| format!("missing {name}").into())
}

fn parse<T: std::str::FromStr>(value: String, name: &str) -> Result<T>
where
    T::Err: Error + 'static,
{
    value
        .parse::<T>()
        .map_err(|error| format!("invalid {name}: {error}").into())
}

fn main() -> Result<()> {
    let mut args = env::args().skip(1);
    let service = required(&mut args, "SERVICE")?;
    let width: u32 = parse(required(&mut args, "WIDTH")?, "WIDTH")?;
    let height: u32 = parse(required(&mut args, "HEIGHT")?, "HEIGHT")?;
    let stride: u32 = parse(required(&mut args, "STRIDE")?, "STRIDE")?;
    let frames: u64 = parse(required(&mut args, "FRAMES")?, "FRAMES")?;
    let period_ms: u64 = parse(required(&mut args, "PERIOD_MS")?, "PERIOD_MS")?;
    if let Some(extra) = args.next() {
        return Err(format!("unexpected argument {extra:?}").into());
    }
    let row_bytes = width.checked_mul(3).ok_or("WIDTH*3 overflowed")?;
    if height > 1 && stride < row_bytes {
        return Err(format!("STRIDE={stride} is smaller than row bytes={row_bytes}").into());
    }
    let payload_size = usize::try_from(stride)?
        .checked_mul(usize::try_from(height)?)
        .ok_or("STRIDE*HEIGHT overflowed")?;
    let descriptor = SliceStream::<Image>::descriptor(service, StreamProfile::Sensor);
    let publisher = SliceStream::<Image>::publisher(&descriptor, payload_size)?;
    println!(
        "PYTHON_IMAGE_FIXTURE ready service={} width={width} height={height} stride={stride} payload={payload_size} frames={frames}",
        descriptor.service_name()
    );
    io::stdout().flush()?;

    let open_deadline = Instant::now() + Duration::from_secs(10);
    while publisher
        .pressure()
        .publisher
        .is_none_or(|pressure| pressure.subscriber_count == 0)
    {
        if Instant::now() >= open_deadline {
            return Err("timed out waiting for a subscriber".into());
        }
        thread::sleep(Duration::from_millis(1));
    }

    let period = Duration::from_millis(period_ms);
    for sequence in 0..frames {
        let mut sample = publisher.loan_slice(payload_size)?;
        {
            let header = sample.header_mut();
            header.sequence = sequence;
            header.timestamp_ns = sequence.saturating_mul(1_000_000);
            header.width = width;
            header.height = height;
            header.stride = stride;
            header.encoding = PixelFormat::RGB8;
        }
        for (index, byte) in sample.payload_mut().iter_mut().enumerate() {
            *byte = sequence.wrapping_add(index as u64) as u8;
        }
        let recipients = sample.send()?;
        println!("PYTHON_IMAGE_FIXTURE sequence={sequence} recipients={recipients}");
        if sequence + 1 < frames && !period.is_zero() {
            thread::sleep(period);
        }
    }
    println!("PYTHON_IMAGE_FIXTURE complete frames={frames}");
    Ok(())
}

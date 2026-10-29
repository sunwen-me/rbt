use std::{
    env,
    error::Error,
    fmt,
    io::{self, Write},
    sync::{Arc, Condvar, Mutex},
    thread,
    time::{Duration, Instant},
};

use rbt_iox2::{
    ActivationId, Image, PixelFormat, ProcessInstanceId, SliceStream, StreamDescriptor,
    StreamProfile, WaitContext, WaitProgress, WaitToken,
};

type Result<T> = std::result::Result<T, Box<dyn Error>>;

#[derive(Debug)]
struct MixedPressureError(String);

impl fmt::Display for MixedPressureError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl Error for MixedPressureError {}

fn error(message: impl Into<String>) -> Box<dyn Error> {
    Box::new(MixedPressureError(message.into()))
}

macro_rules! fail {
    ($($argument:tt)*) => {
        return Err(error(format!($($argument)*)))
    };
}

const IMAGE_WIDTH: u32 = 1_920;
const IMAGE_HEIGHT: u32 = 1_080;
const IMAGE_CHANNELS: u32 = 3;
const IMAGE_STRIDE: u32 = IMAGE_WIDTH * IMAGE_CHANNELS;
const IMAGE_PAYLOAD_SIZE: usize = IMAGE_STRIDE as usize * IMAGE_HEIGHT as usize;
const SAMPLE_COUNT: u64 = 40;
const SEND_PERIOD: Duration = Duration::from_millis(10);
const OPEN_TIMEOUT: Duration = Duration::from_secs(5);
const SUBSCRIBER_WINDOW: Duration = Duration::from_secs(2);
const POLL_INTERVAL: Duration = Duration::from_millis(1);

fn usage(program: &str) -> String {
    format!(
        "usage:\n  {program} publisher SERVICE EXPECTED_SUBSCRIBERS\n  {program} subscriber SERVICE DELAY_MS\n  {program} stalled SERVICE HOLD_MS"
    )
}

fn required(args: &mut impl Iterator<Item = String>, name: &str) -> Result<String> {
    args.next()
        .ok_or_else(|| error(format!("missing {name} argument")))
}

fn parse_u64(value: String, name: &str) -> Result<u64> {
    value
        .parse::<u64>()
        .map_err(|parse_error| error(format!("invalid {name}={value:?}: {parse_error}")))
}

fn reject_extra(args: &mut impl Iterator<Item = String>) -> Result<()> {
    if let Some(extra) = args.next() {
        fail!("unexpected argument {extra:?}");
    }
    Ok(())
}

fn image_descriptor(service: &str) -> StreamDescriptor {
    SliceStream::<Image>::descriptor(service.to_owned(), StreamProfile::Sensor)
}

fn flush_stdout() -> Result<()> {
    io::stdout().flush().map_err(Into::into)
}

fn image_byte(sequence: u64, index: usize) -> u8 {
    sequence.wrapping_add(index as u64) as u8
}

fn fill_image_sample(sample: &mut rbt_iox2::LoanedSliceSample<Image>, sequence: u64) -> Result<()> {
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
        fail!(
            "publisher loaned payload={} bytes; expected {}",
            payload.len(),
            IMAGE_PAYLOAD_SIZE
        );
    }
    for (index, byte) in payload.iter_mut().enumerate() {
        *byte = image_byte(sequence, index);
    }
    Ok(())
}

fn open_subscriber(descriptor: &StreamDescriptor) -> Result<rbt_iox2::SliceSubscriber<Image>> {
    let deadline = Instant::now() + OPEN_TIMEOUT;
    loop {
        match SliceStream::<Image>::subscriber(descriptor) {
            Ok(subscriber) => return Ok(subscriber),
            Err(_error) if Instant::now() < deadline => {
                thread::sleep(
                    POLL_INTERVAL.min(deadline.saturating_duration_since(Instant::now())),
                );
            }
            Err(error) => {
                fail!(
                    "timed out opening Sensor image service {:?} after {} seconds: {error}",
                    descriptor.service_name(),
                    OPEN_TIMEOUT.as_secs()
                );
            }
        }
    }
}

fn wait_for_subscribers(publisher: &rbt_iox2::SlicePublisher<Image>, expected: u64) -> Result<()> {
    let deadline = Instant::now() + OPEN_TIMEOUT;
    loop {
        let count = publisher
            .pressure()
            .publisher
            .map_or(0, |pressure| u64::from(pressure.subscriber_count));
        if count >= expected {
            println!(
                "MIXED_RUST_PUBLISHER subscribers={} expected={expected}",
                count
            );
            flush_stdout()?;
            return Ok(());
        }
        if Instant::now() >= deadline {
            fail!("timed out waiting for {expected} Sensor subscribers; observed={count}");
        }
        thread::sleep(POLL_INTERVAL.min(deadline.saturating_duration_since(Instant::now())));
    }
}

fn publish(service: &str, expected_subscribers: u64) -> Result<()> {
    if expected_subscribers == 0 {
        fail!("EXPECTED_SUBSCRIBERS must be greater than zero");
    }

    let descriptor = image_descriptor(service);
    let publisher = SliceStream::<Image>::publisher(&descriptor, IMAGE_PAYLOAD_SIZE)?;
    println!(
        "MIXED_RUST_PUBLISHER ready service={} frames={} period_ms={} expected_subscribers={} payload={}",
        descriptor.service_name(),
        SAMPLE_COUNT,
        SEND_PERIOD.as_millis(),
        expected_subscribers,
        IMAGE_PAYLOAD_SIZE,
    );
    flush_stdout()?;
    wait_for_subscribers(&publisher, expected_subscribers)?;

    let started = Instant::now();
    let mut delivered = 0_u64;
    let mut undelivered = 0_u64;
    let mut max_send_ms = 0_u128;
    for sequence in 0..SAMPLE_COUNT {
        let mut sample = publisher.loan_slice(IMAGE_PAYLOAD_SIZE)?;
        fill_image_sample(&mut sample, sequence)?;
        let send_started = Instant::now();
        let recipients = sample.send()?;
        max_send_ms = max_send_ms.max(send_started.elapsed().as_millis());
        if recipients > 0 {
            delivered += 1;
        } else {
            undelivered += 1;
        }
        if sequence + 1 < SAMPLE_COUNT {
            thread::sleep(SEND_PERIOD);
        }
    }

    println!(
        "MIXED_RUST_PUBLISHER complete service={} frames={} delivered={} undelivered={} send_errors=0 elapsed_ms={} max_send_ms={}",
        descriptor.service_name(),
        SAMPLE_COUNT,
        delivered,
        undelivered,
        started.elapsed().as_millis(),
        max_send_ms,
    );
    Ok(())
}

fn validate_image_sample(sample: &rbt_iox2::ReceivedSliceSample<Image>) -> Result<u64> {
    let header = sample.header();
    if header.timestamp_ns != header.sequence * 1_000_000
        || header.width != IMAGE_WIDTH
        || header.height != IMAGE_HEIGHT
        || header.stride != IMAGE_STRIDE
        || header.encoding != PixelFormat::RGB8
    {
        fail!(
            "invalid image header sequence={} timestamp_ns={} width={} height={} stride={} encoding={}",
            header.sequence,
            header.timestamp_ns,
            header.width,
            header.height,
            header.stride,
            header.encoding.0,
        );
    }

    let payload = sample.payload();
    if payload.len() != IMAGE_PAYLOAD_SIZE {
        fail!(
            "invalid image payload length sequence={} expected={} actual={}",
            header.sequence,
            IMAGE_PAYLOAD_SIZE,
            payload.len(),
        );
    }
    let middle = payload.len() / 2;
    for (index, actual) in [
        (0, payload[0]),
        (middle, payload[middle]),
        (payload.len() - 1, payload[payload.len() - 1]),
    ] {
        let expected = image_byte(header.sequence, index);
        if actual != expected {
            fail!(
                "invalid image pattern sequence={} index={} expected={} actual={}",
                header.sequence,
                index,
                expected,
                actual,
            );
        }
    }
    Ok(header.sequence)
}

fn subscribe(service: &str, delay_ms: u64) -> Result<()> {
    let descriptor = image_descriptor(service);
    let subscriber = open_subscriber(&descriptor)?;
    println!(
        "MIXED_RUST_SUBSCRIBER ready service={} delay_ms={} window_ms={}",
        descriptor.service_name(),
        delay_ms,
        SUBSCRIBER_WINDOW.as_millis(),
    );
    flush_stdout()?;

    let deadline = Instant::now() + SUBSCRIBER_WINDOW;
    let delay = Duration::from_millis(delay_ms);
    let mut received = 0_u64;
    let mut gaps = 0_u64;
    let mut estimated_dropped = 0_u64;
    let mut previous = None;
    let mut context = WaitContext::new(ProcessInstanceId::new(), ActivationId::first())?;
    let source = subscriber.event_source(WaitToken::new(1));
    // WaitContext currently exposes no deadline-aware run method. A
    // cancellable timer preserves the existing subscriber window without
    // polling.
    let timeout_done = Arc::new((Mutex::new(false), Condvar::new()));
    let timeout_done_for_thread = Arc::clone(&timeout_done);
    let wake_handle = context.wake_handle();
    let wait_result = thread::scope(|scope| {
        let timer = scope.spawn(move || -> std::result::Result<(), String> {
            let (done, condition) = &*timeout_done_for_thread;
            let done = done
                .lock()
                .map_err(|_| "subscriber wait timeout state was poisoned".to_owned())?;
            let (done, timeout) = condition
                .wait_timeout_while(
                    done,
                    deadline.saturating_duration_since(Instant::now()),
                    |finished| !*finished,
                )
                .map_err(|_| "subscriber wait timeout state was poisoned".to_owned())?;
            if timeout.timed_out() && !*done {
                wake_handle.wake().map_err(|wake_error| {
                    format!("wake subscriber wait context at timeout: {wake_error}")
                })?;
            }
            Ok(())
        });
        let wait_result = context.run(std::slice::from_ref(&source), |_event, budget| {
            let mut drained = 0usize;
            let mut empty = false;
            for _ in 0..budget {
                if Instant::now() >= deadline {
                    return Ok::<_, Box<dyn Error>>(if drained == 0 {
                        WaitProgress::Stop
                    } else {
                        WaitProgress::stop_after_samples(drained)
                    });
                }
                let Some(sample) = subscriber.receive()? else {
                    empty = true;
                    break;
                };

                let sequence = validate_image_sample(&sample)?;
                if let Some(previous_sequence) = previous {
                    let gap = sequence.saturating_sub(previous_sequence + 1);
                    if gap > 0 {
                        gaps += 1;
                        estimated_dropped += gap;
                    }
                }
                previous = Some(sequence);
                received += 1;
                drained += 1;
                drop(sample);
                if !delay.is_zero() {
                    thread::sleep(delay);
                }
            }

            if Instant::now() >= deadline {
                return Ok(if drained == 0 {
                    WaitProgress::Stop
                } else {
                    WaitProgress::stop_after_samples(drained)
                });
            }
            Ok(if empty {
                if drained == 0 {
                    WaitProgress::Empty
                } else {
                    WaitProgress::SamplesAndEmpty {
                        samples: drained,
                        empty_wakeups: 1,
                    }
                }
            } else {
                WaitProgress::Samples(drained)
            })
        });

        if let Ok(mut done) = timeout_done.0.lock() {
            *done = true;
            timeout_done.1.notify_one();
        }
        let timer_result = timer
            .join()
            .map_err(|_| error("subscriber wait timeout thread panicked"))?;
        timer_result.map_err(error)?;
        wait_result
    })?;
    let _ = wait_result;

    println!(
        "MIXED_RUST_SUBSCRIBER complete service={} frames={} gaps={} estimated_dropped={} delay_ms={}",
        descriptor.service_name(),
        received,
        gaps,
        estimated_dropped,
        delay_ms,
    );
    Ok(())
}

fn stalled(service: &str, hold_ms: u64) -> Result<()> {
    if hold_ms == 0 {
        fail!("HOLD_MS must be greater than zero");
    }
    let descriptor = image_descriptor(service);
    let _subscriber = open_subscriber(&descriptor)?;
    println!(
        "MIXED_RUST_STALLED ready service={} hold_ms={} received=0",
        descriptor.service_name(),
        hold_ms,
    );
    flush_stdout()?;
    thread::sleep(Duration::from_millis(hold_ms));
    println!(
        "MIXED_RUST_STALLED complete service={} hold_ms={} received=0",
        descriptor.service_name(),
        hold_ms,
    );
    Ok(())
}

fn main() -> Result<()> {
    let mut args = env::args();
    let program = args.next().unwrap_or_else(|| "mixed_pressure".to_owned());
    let command = required(&mut args, "COMMAND")
        .map_err(|parse_error| error(format!("{parse_error}\n{}", usage(&program))))?;
    match command.as_str() {
        "publisher" => {
            let service = required(&mut args, "SERVICE")?;
            let expected = parse_u64(
                required(&mut args, "EXPECTED_SUBSCRIBERS")?,
                "EXPECTED_SUBSCRIBERS",
            )?;
            reject_extra(&mut args)?;
            publish(&service, expected)
        }
        "subscriber" => {
            let service = required(&mut args, "SERVICE")?;
            let delay_ms = parse_u64(required(&mut args, "DELAY_MS")?, "DELAY_MS")?;
            reject_extra(&mut args)?;
            subscribe(&service, delay_ms)
        }
        "stalled" => {
            let service = required(&mut args, "SERVICE")?;
            let hold_ms = parse_u64(required(&mut args, "HOLD_MS")?, "HOLD_MS")?;
            reject_extra(&mut args)?;
            stalled(&service, hold_ms)
        }
        other => fail!("unknown command {other:?}\n{}", usage(&program)),
    }
}

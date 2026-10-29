use std::{
    io::Write,
    thread,
    time::{Duration, Instant},
};

use anyhow::{Context, Result, bail, ensure};
use rbt_iox2::{
    ActivationId, Image, ImageHeader, PixelFormat, ProcessInstanceId, SliceStream, StreamProfile,
    WaitContext, WaitExit, WaitProgress, WaitToken,
};

pub const DEFAULT_SERVICE: &str = "rbt/fork/sdk";

const WIDTH: u32 = 1920;
const HEIGHT: u32 = 1080;
const STRIDE: u32 = WIDTH * 3;
const PAYLOAD_LEN: usize = STRIDE as usize * HEIGHT as usize;
const SEQUENCE: u64 = 7;
const TIMESTAMP_NS: u64 = SEQUENCE * 1_000_000;
const SERVICE_OPEN_TIMEOUT: Duration = Duration::from_secs(15);
const RECEIVE_TIMEOUT: Duration = Duration::from_secs(15);
const RETRY_INTERVAL: Duration = Duration::from_millis(50);
const SOURCE_STARTUP_DELAY: Duration = Duration::from_secs(2);
const SOURCE_HOLD: Duration = Duration::from_secs(1);
const SOURCE_TOKEN: WaitToken = WaitToken::new(0);

pub fn publish(service: &str) -> Result<()> {
    let descriptor = SliceStream::<Image>::descriptor(service, StreamProfile::Sensor);
    let publisher = SliceStream::<Image>::publisher(&descriptor, PAYLOAD_LEN)
        .context("open production SDK image publisher")?;

    println!("READY role=sdk-publisher service={service}");
    std::io::stdout().flush()?;
    thread::sleep(SOURCE_STARTUP_DELAY);

    let mut sample = publisher
        .loan_slice_uninit(PAYLOAD_LEN)
        .context("loan production SDK image payload")?
        .write_from_fn(|index| pattern(SEQUENCE, index));
    *sample.header_mut() = expected_header();
    let recipients = sample.send().context("send production SDK image")?;

    thread::sleep(SOURCE_HOLD);
    println!(
        "SUMMARY sdk=true role=publisher service={service} sent=1 recipients={recipients} payload_bytes={PAYLOAD_LEN}"
    );
    Ok(())
}

pub fn consume(service: &str) -> Result<()> {
    let descriptor = SliceStream::<Image>::descriptor(service, StreamProfile::Sensor);
    let subscriber = open_subscriber(&descriptor)?;
    let mut context = WaitContext::new(ProcessInstanceId::new(), ActivationId::first())
        .context("create production SDK wait context")?;
    let source = subscriber.event_source(SOURCE_TOKEN);

    let exit = context
        .run_with_timeout(
            std::slice::from_ref(&source),
            RECEIVE_TIMEOUT,
            |event, budget| -> Result<WaitProgress> {
                ensure!(budget <= 64, "WaitContext drain budget exceeded: {budget}");
                ensure!(!event.is_cancellation(), "unexpected cancellation event");
                ensure!(event.token() == SOURCE_TOKEN, "unexpected source token");
                if event.is_synthetic() {
                    return Ok(WaitProgress::Continue);
                }

                let Some(sample) = subscriber
                    .receive()
                    .context("receive production SDK image after readiness")?
                else {
                    return Ok(WaitProgress::Empty);
                };
                validate_sample(sample.header(), sample.payload())?;
                Ok(WaitProgress::stop_after_samples(1))
            },
        )
        .context("run production SDK readiness wait")?;
    ensure!(
        exit == Some(WaitExit::Stopped),
        "SDK receive timed out: {exit:?}"
    );

    let stats = context.stats().snapshot();
    ensure!(
        stats.samples_processed == 1,
        "expected one received sample, got {stats:?}"
    );
    ensure!(
        stats.wakeups >= 1,
        "expected a real WaitContext wake, got {stats:?}"
    );
    println!(
        "SUMMARY sdk=true strategy=WaitContext payload_polling=false initial_drain=0 final={SEQUENCE} service={service} samples_processed={} wakes={}",
        stats.samples_processed, stats.wakeups
    );
    Ok(())
}

fn open_subscriber(
    descriptor: &rbt_iox2::StreamDescriptor,
) -> Result<rbt_iox2::SliceSubscriber<Image>> {
    let deadline = Instant::now() + SERVICE_OPEN_TIMEOUT;
    loop {
        let error = match SliceStream::<Image>::subscriber(descriptor) {
            Ok(subscriber) => return Ok(subscriber),
            Err(error) => error,
        };

        if Instant::now() >= deadline {
            bail!(
                "production SDK service {} did not open within {}s: {}",
                descriptor.service_name(),
                SERVICE_OPEN_TIMEOUT.as_secs(),
                error
            );
        }
        thread::sleep(RETRY_INTERVAL);
    }
}

fn expected_header() -> ImageHeader {
    ImageHeader {
        sequence: SEQUENCE,
        timestamp_ns: TIMESTAMP_NS,
        width: WIDTH,
        height: HEIGHT,
        stride: STRIDE,
        encoding: PixelFormat::RGB8,
    }
}

fn pattern(sequence: u64, index: usize) -> u8 {
    sequence.wrapping_add(index as u64) as u8
}

fn validate_sample(header: &ImageHeader, payload: &[u8]) -> Result<()> {
    let expected_header = expected_header();
    ensure!(
        header.sequence == expected_header.sequence
            && header.timestamp_ns == expected_header.timestamp_ns
            && header.width == expected_header.width
            && header.height == expected_header.height
            && header.stride == expected_header.stride
            && header.encoding == expected_header.encoding,
        "Image header mismatch: {header:?}"
    );
    ensure!(
        payload.len() == PAYLOAD_LEN,
        "Image payload length mismatch: expected {PAYLOAD_LEN}, got {}",
        payload.len()
    );
    for (index, byte) in payload.iter().enumerate() {
        ensure!(
            *byte == pattern(SEQUENCE, index),
            "Image payload mismatch at byte {index}"
        );
    }
    Ok(())
}

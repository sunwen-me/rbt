use std::{
    io::{self, Write},
    sync::{Arc, Condvar, Mutex},
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

use anyhow::{Context, Result, anyhow, bail};
use rbt_bench::{
    BenchConfig, FULL_LINEAR_FNV1A_VALIDATION, MessageKind, PAYLOAD_FINGERPRINT_OFFSET, Role,
    SubscriberMode, cpu_seconds_between, fold_payload_fingerprint, parse_args, percentile,
    process_stats, usage, validate_image_payload, validate_pointcloud_payload,
};
use rbt_iox2::{
    ActivationId, ControlCommand, EventSource, Image, PixelFormat, PointCloud, PointXYZI,
    ProcessInstanceId, ReadyEvent, SliceStream, Stream, StreamError, StreamProfile, WaitContext,
    WaitExit, WaitProgress, WaitToken,
};

// These delays belong to publisher startup/pacing/retry paths. The subscriber
// receive path blocks in WaitContext instead of polling empty receives.
const BACKPRESSURE_RETRY_INTERVAL: Duration = Duration::from_micros(100);
const OPEN_RETRY_INTERVAL: Duration = Duration::from_millis(20);

fn main() {
    let arguments: Vec<String> = std::env::args().skip(1).collect();
    if arguments
        .iter()
        .any(|argument| argument == "-h" || argument == "--help")
    {
        println!("{}", usage("rbt-bench"));
        return;
    }

    let config = match parse_args(arguments) {
        Ok(config) => config,
        Err(error) => {
            eprintln!("RBT_BENCH_ERROR {error}");
            eprintln!("{}", usage("rbt-bench"));
            std::process::exit(2);
        }
    };

    if let Err(error) = run(config) {
        eprintln!("RBT_BENCH_ERROR {error:#}");
        std::process::exit(1);
    }
}

fn run(config: BenchConfig) -> Result<()> {
    match config.role {
        Role::Publisher => run_publisher(&config),
        Role::Subscriber => run_subscriber(&config),
    }
}

fn run_publisher(config: &BenchConfig) -> Result<()> {
    let start_stats = process_stats();
    let total = config.total_samples().map_err(|error| anyhow!(error))?;
    let descriptor = descriptor(config);
    let (layout, publisher_ready) = match config.message {
        MessageKind::Image => {
            let publisher = SliceStream::<Image>::publisher(&descriptor, config.payload_bytes)
                .context("open Image publisher")?;
            (
                descriptor.message_type().layout_hash.to_string(),
                PublisherReady::Image(publisher),
            )
        }
        MessageKind::PointCloud => {
            let element_count = point_count(config.payload_bytes)?;
            let publisher = SliceStream::<PointCloud>::publisher(&descriptor, element_count)
                .context("open PointCloud publisher")?;
            (
                descriptor.message_type().layout_hash.to_string(),
                PublisherReady::PointCloud(publisher),
            )
        }
        MessageKind::ControlCommand => {
            let publisher = Stream::<ControlCommand>::publisher(&descriptor)
                .context("open Control publisher")?;
            (
                descriptor.message_type().layout_hash.to_string(),
                PublisherReady::Control(publisher),
            )
        }
    };

    print_ready(config, &layout, total)?;
    let subscriber_deadline = Instant::now() + config.timeout;
    while publisher_ready.subscriber_count() < config.subscriber_count {
        if Instant::now() >= subscriber_deadline {
            bail!(
                "timed out after {} ms waiting for {} subscribers; observed {}",
                config.timeout.as_millis(),
                config.subscriber_count,
                publisher_ready.subscriber_count(),
            );
        }
        thread::sleep(OPEN_RETRY_INTERVAL);
    }
    let connected_subscribers = publisher_ready.subscriber_count();
    thread::sleep(config.startup_delay);

    let warmup_deadline = Instant::now() + config.timeout;
    for ordinal in 0..config.warmup {
        send_one(&publisher_ready, config, ordinal, warmup_deadline)?;
        sleep_between_sends(config.interval);
    }

    let started = Instant::now();
    let mut delivered = 0_u64;
    let mut minimum_recipients = usize::MAX;
    let mut maximum_recipients = 0_usize;
    for ordinal in config.warmup..total {
        let recipients = send_one(
            &publisher_ready,
            config,
            ordinal,
            Instant::now() + config.timeout,
        )?;
        if recipients != config.subscriber_count as usize {
            bail!(
                "send at ordinal {ordinal} reached {recipients} subscribers, expected {}",
                config.subscriber_count,
            );
        }
        delivered = delivered.saturating_add(recipients as u64);
        minimum_recipients = minimum_recipients.min(recipients);
        maximum_recipients = maximum_recipients.max(recipients);
        sleep_between_sends(config.interval);
    }
    let elapsed = started.elapsed();
    let end_stats = process_stats();
    let published = config.count;
    let elapsed_seconds = elapsed.as_secs_f64();
    let publish_rate = rate(published, elapsed_seconds);
    let delivery_rate = rate(delivered, elapsed_seconds);
    let cpu_seconds = cpu_seconds_between(start_stats, end_stats);

    println!(
        "RBT_BENCH_RESULT role=publisher build_mode={} service={} profile={} message={} payload={} count={} warmup={} queue_config={} subscriber_count={} observed_subscriber_count={} sampling_duration_s={elapsed_seconds:.6} published={} delivered={} min_recipients_per_send={} max_recipients_per_send={} publish_rate_hz={publish_rate:.3} delivery_rate_hz={delivery_rate:.3} cpu_time_s={} rss_bytes={} layout_hash={layout} payload_generation={}",
        build_mode(),
        config.service,
        config.profile,
        config.message,
        config.payload_bytes,
        config.count,
        config.warmup,
        config.profile.queue_config(),
        config.subscriber_count,
        connected_subscribers,
        published,
        delivered,
        minimum_recipients,
        maximum_recipients,
        format_optional(cpu_seconds),
        format_optional_u64(end_stats.rss_bytes),
        payload_generation_method(config.message),
    );
    Ok(())
}

fn run_subscriber(config: &BenchConfig) -> Result<()> {
    let start_stats = process_stats();
    let total = config.total_samples().map_err(|error| anyhow!(error))?;
    let descriptor = descriptor(config);
    let layout = descriptor.message_type().layout_hash.to_string();
    let subscriber = match config.message {
        MessageKind::Image => SubscriberReady::Image(open_image_subscriber(&descriptor, config)?),
        MessageKind::PointCloud => {
            SubscriberReady::PointCloud(open_pointcloud_subscriber(&descriptor, config)?)
        }
        MessageKind::ControlCommand => {
            SubscriberReady::Control(open_control_subscriber(&descriptor, config)?)
        }
    };

    print_ready(config, &layout, total)?;
    if config.subscriber_mode == SubscriberMode::Stalled {
        let sampling_started = Instant::now();
        thread::sleep(config.timeout);
        return print_subscriber_result(
            config,
            &layout,
            SubscriberResult {
                elapsed: sampling_started.elapsed(),
                received: 0,
                latencies: &[],
                payload_validation_samples: 0,
                payload_fingerprint: None,
            },
            start_stats,
            process_stats(),
        );
    }

    let mut latencies = Vec::with_capacity(config.count.min(usize::MAX as u64) as usize);
    let mut received = 0_u64;
    let mut payload_validation_samples = 0_u64;
    let mut payload_fingerprint = PAYLOAD_FINGERPRINT_OFFSET;
    let mut sampling_started = None;
    let mut last_ordinal = None;

    let mut wait_context = WaitContext::new(ProcessInstanceId::new(), ActivationId::first())
        .context("create subscriber wait context")?;
    let source = subscriber.event_source(WaitToken::new(1));
    let timeout_done = Arc::new((Mutex::new(false), Condvar::new()));
    let timeout_done_for_thread = Arc::clone(&timeout_done);
    let wake_handle = wait_context.wake_handle();
    let wait_result = thread::scope(|scope| {
        // This watchdog only wakes the cancellation sidecar at the existing
        // benchmark deadline. Payload waiting and receiving stay on this
        // thread; it is not a payload-forwarding or notifier thread.
        let timer = scope.spawn(move || -> Result<()> {
            let (done, condition) = &*timeout_done_for_thread;
            let done = done
                .lock()
                .map_err(|_| anyhow!("subscriber wait timeout state was poisoned"))?;
            let (done, timeout) = condition
                .wait_timeout_while(done, config.timeout, |finished| !*finished)
                .map_err(|_| anyhow!("subscriber wait timeout state was poisoned"))?;
            if timeout.timed_out() && !*done {
                wake_handle
                    .wake()
                    .map_err(|error| anyhow!(error))
                    .context("wake subscriber wait context at timeout")?;
            }
            Ok(())
        });

        let wait_result = wait_context.run(
            std::slice::from_ref(&source),
            |event: ReadyEvent, budget| -> Result<WaitProgress> {
                if event.is_cancellation() {
                    return Ok(WaitProgress::stop());
                }

                let mut drained = 0_usize;
                let mut empty_wakeups = 0_usize;
                for _ in 0..budget {
                    let Some((ordinal, timestamp_ns, sample_fingerprint)) =
                        subscriber.receive(config.payload_bytes)?
                    else {
                        empty_wakeups = 1;
                        break;
                    };
                    drained = drained.saturating_add(1);

                    if let Some(sample_fingerprint) = sample_fingerprint {
                        payload_validation_samples = payload_validation_samples.saturating_add(1);
                        payload_fingerprint =
                            fold_payload_fingerprint(payload_fingerprint, sample_fingerprint);
                    }

                    if ordinal < config.warmup {
                        continue;
                    }
                    if ordinal >= total || last_ordinal.is_some_and(|last| ordinal <= last) {
                        continue;
                    }
                    last_ordinal = Some(ordinal);
                    if sampling_started.is_none() {
                        sampling_started = Some(Instant::now());
                    }
                    latencies.push(timestamp_ns_now()?.saturating_sub(timestamp_ns));
                    received = received.saturating_add(1);
                    if ordinal == total.saturating_sub(1) || received >= config.count {
                        return Ok(WaitProgress::stop_after_samples(drained));
                    }
                    if !config.receive_delay.is_zero() {
                        thread::sleep(config.receive_delay);
                    }
                }

                match (drained, empty_wakeups) {
                    (0, 0) => Ok(WaitProgress::continue_waiting()),
                    (0, _) => Ok(WaitProgress::empty()),
                    (drained, 0) => Ok(WaitProgress::samples(drained)),
                    (drained, empty_wakeups) => Ok(WaitProgress::SamplesAndEmpty {
                        samples: drained,
                        empty_wakeups,
                    }),
                }
            },
        );

        if let Ok(mut done) = timeout_done.0.lock() {
            *done = true;
            timeout_done.1.notify_one();
        }
        let timer_result = timer
            .join()
            .map_err(|_| anyhow!("subscriber wait timeout thread panicked"))?;
        timer_result?;
        wait_result
    })?;
    match wait_result {
        WaitExit::Stopped | WaitExit::Cancelled => {}
    }

    if received == 0 && config.count > 0 {
        bail!(
            "timed out after {} ms waiting for measured samples (service={})",
            config.timeout.as_millis(),
            config.service
        );
    }

    let elapsed = sampling_started.map_or(Duration::ZERO, |started| started.elapsed());
    let end_stats = process_stats();
    print_subscriber_result(
        config,
        &layout,
        SubscriberResult {
            elapsed,
            received,
            latencies: &latencies,
            payload_validation_samples,
            payload_fingerprint: (payload_validation_samples > 0).then_some(payload_fingerprint),
        },
        start_stats,
        end_stats,
    )
}

struct SubscriberResult<'a> {
    elapsed: Duration,
    received: u64,
    latencies: &'a [u64],
    payload_validation_samples: u64,
    payload_fingerprint: Option<u64>,
}

fn print_subscriber_result(
    config: &BenchConfig,
    layout: &str,
    result: SubscriberResult<'_>,
    start_stats: rbt_bench::ProcessStats,
    end_stats: rbt_bench::ProcessStats,
) -> Result<()> {
    let elapsed_seconds = result.elapsed.as_secs_f64();
    let receive_rate = rate(result.received, elapsed_seconds);
    let p50 = percentile(result.latencies, 50.0);
    let p90 = percentile(result.latencies, 90.0);
    let p99 = percentile(result.latencies, 99.0);
    let max = result.latencies.iter().copied().max();
    let dropped = config.count.saturating_sub(result.received);
    let cpu_seconds = cpu_seconds_between(start_stats, end_stats);
    let payload_validation =
        payload_validation_method(config.message, result.payload_validation_samples);
    let payload_fingerprint = result
        .payload_fingerprint
        .map_or_else(|| "na".to_owned(), |fingerprint| fingerprint.to_string());

    println!(
        "RBT_BENCH_RESULT role=subscriber build_mode={} service={} profile={} message={} payload={} count={} warmup={} queue_config={} subscriber_count={} subscriber_mode={} receive_delay_us={} sampling_duration_s={elapsed_seconds:.6} received={} dropped={} receive_rate_hz={receive_rate:.3} p50_latency_us={} p90_latency_us={} p99_latency_us={} max_latency_us={} latency_samples={} cpu_time_s={} rss_bytes={} layout_hash={layout} strategy=waitset payload_validation={} payload_validation_samples={} payload_fingerprint={}",
        build_mode(),
        config.service,
        config.profile,
        config.message,
        config.payload_bytes,
        config.count,
        config.warmup,
        config.profile.queue_config(),
        config.subscriber_count,
        config.subscriber_mode,
        config.receive_delay.as_micros(),
        result.received,
        dropped,
        format_latency(p50),
        format_latency(p90),
        format_latency(p99),
        format_latency(max),
        result.latencies.len(),
        format_optional(cpu_seconds),
        format_optional_u64(end_stats.rss_bytes),
        payload_validation,
        result.payload_validation_samples,
        payload_fingerprint,
    );
    Ok(())
}

fn descriptor(config: &BenchConfig) -> rbt_iox2::StreamDescriptor {
    match config.message {
        MessageKind::Image => {
            SliceStream::<Image>::descriptor(config.service.clone(), StreamProfile::Sensor)
        }
        MessageKind::PointCloud => {
            SliceStream::<PointCloud>::descriptor(config.service.clone(), StreamProfile::Sensor)
        }
        MessageKind::ControlCommand => {
            Stream::<ControlCommand>::descriptor(config.service.clone(), StreamProfile::Control)
        }
    }
}

enum PublisherReady {
    Image(rbt_iox2::SlicePublisher<Image>),
    PointCloud(rbt_iox2::SlicePublisher<PointCloud>),
    Control(rbt_iox2::Publisher<ControlCommand>),
}

impl PublisherReady {
    fn subscriber_count(&self) -> u32 {
        let pressure = match self {
            Self::Image(publisher) => publisher.pressure(),
            Self::PointCloud(publisher) => publisher.pressure(),
            Self::Control(publisher) => publisher.pressure(),
        };
        pressure
            .publisher
            .map_or(0, |publisher| publisher.subscriber_count)
    }
}

enum SubscriberReady {
    Image(rbt_iox2::SliceSubscriber<Image>),
    PointCloud(rbt_iox2::SliceSubscriber<PointCloud>),
    Control(rbt_iox2::Subscriber<ControlCommand>),
}

impl SubscriberReady {
    fn event_source(&self, token: WaitToken) -> EventSource<'_> {
        match self {
            Self::Image(subscriber) => subscriber.event_source(token),
            Self::PointCloud(subscriber) => subscriber.event_source(token),
            Self::Control(subscriber) => subscriber.event_source(token),
        }
    }

    fn receive(&self, expected_payload_bytes: usize) -> Result<Option<(u64, u64, Option<u64>)>> {
        match self {
            Self::Image(subscriber) => {
                let Some(sample) = subscriber.receive()? else {
                    return Ok(None);
                };
                let header = sample.header();
                let payload = sample.payload();
                if payload.len() != expected_payload_bytes {
                    bail!(
                        "received Image payload of {} bytes, expected {}",
                        payload.len(),
                        expected_payload_bytes
                    );
                }
                let fingerprint = validate_image_payload(payload, header.sequence)
                    .map_err(|error| anyhow!(error))
                    .context("validate Image payload")?;
                Ok(Some((
                    header.sequence,
                    header.timestamp_ns,
                    Some(fingerprint),
                )))
            }
            Self::PointCloud(subscriber) => {
                let Some(sample) = subscriber.receive()? else {
                    return Ok(None);
                };
                let header = sample.header();
                let payload = sample.payload();
                let payload_bytes = payload
                    .len()
                    .checked_mul(std::mem::size_of::<PointXYZI>())
                    .context("PointCloud payload byte length overflow")?;
                if payload_bytes != expected_payload_bytes {
                    bail!(
                        "received PointCloud payload of {payload_bytes} bytes, expected {expected_payload_bytes}"
                    );
                }
                let fingerprint = validate_pointcloud_payload(payload, header.sequence)
                    .map_err(|error| anyhow!(error))
                    .context("validate PointCloud payload")?;
                Ok(Some((
                    header.sequence,
                    header.timestamp_ns,
                    Some(fingerprint),
                )))
            }
            Self::Control(subscriber) => {
                let Some(sample) = subscriber.receive()? else {
                    return Ok(None);
                };
                // ControlCommand has no timestamp field.  The benchmark uses
                // its existing u64 sequence storage for timestamp_ns and the
                // reserved u32 field for the ordinal; the ABI is unchanged.
                Ok(Some((u64::from(sample.reserved), sample.sequence, None)))
            }
        }
    }
}

fn payload_generation_method(message: MessageKind) -> &'static str {
    match message {
        MessageKind::Image | MessageKind::PointCloud => "deterministic_sequence_pattern",
        MessageKind::ControlCommand => "fixed_header_fields",
    }
}

fn payload_validation_method(message: MessageKind, validated_samples: u64) -> &'static str {
    match message {
        MessageKind::Image | MessageKind::PointCloud if validated_samples > 0 => {
            FULL_LINEAR_FNV1A_VALIDATION
        }
        MessageKind::Image | MessageKind::PointCloud => "not_run_no_samples",
        MessageKind::ControlCommand => "header_sequence_only",
    }
}

fn send_one(
    publisher: &PublisherReady,
    config: &BenchConfig,
    ordinal: u64,
    deadline: Instant,
) -> Result<usize> {
    match publisher {
        PublisherReady::Image(publisher) => {
            let mut sample = publisher
                .loan_slice_uninit(config.payload_bytes)
                .context("loan Image payload")?
                .write_from_fn(|index| ordinal.wrapping_add(index as u64) as u8);
            {
                let header = sample.header_mut();
                header.sequence = ordinal;
                header.timestamp_ns = timestamp_ns_now()?;
                header.width = u32::try_from(config.payload_bytes)
                    .context("Image payload exceeds u32 width")?;
                header.height = 1;
                header.stride = header.width;
                header.encoding = PixelFormat::GRAY8;
            }
            sample.send().map_err(Into::into)
        }
        PublisherReady::PointCloud(publisher) => {
            let element_count = point_count(config.payload_bytes)?;
            let mut sample = publisher
                .loan_slice_uninit(element_count)
                .context("loan PointCloud payload")?
                .write_from_fn(|index| PointXYZI {
                    x: index as f32,
                    y: ordinal as f32,
                    z: -(index as f32),
                    intensity: (index & 0xff) as f32,
                });
            {
                let header = sample.header_mut();
                header.sequence = ordinal;
                header.timestamp_ns = timestamp_ns_now()?;
                header.width = u32::try_from(element_count)
                    .context("PointCloud element count exceeds u32 width")?;
                header.height = 1;
                header.dense = 1;
                header.reserved = [0; 7];
            }
            sample.send().map_err(Into::into)
        }
        PublisherReady::Control(publisher) => loop {
            let mut sample = publisher.loan().context("loan ControlCommand")?;
            sample.sequence = timestamp_ns_now()?;
            sample.value = config.payload_bytes as f32;
            sample.reserved = u32::try_from(ordinal).context("Control sample count exceeds u32")?;
            match sample.send() {
                Ok(recipients) => break Ok(recipients),
                Err(StreamError::QueueFull) if Instant::now() < deadline => {
                    thread::sleep(BACKPRESSURE_RETRY_INTERVAL);
                }
                Err(StreamError::QueueFull) => {
                    bail!(
                        "Control QueueFull persisted for {} ms at ordinal {}; retry timeout is deterministic",
                        config.timeout.as_millis(),
                        ordinal
                    );
                }
                Err(error) => return Err(error.into()),
            }
        },
    }
}

fn open_image_subscriber(
    descriptor: &rbt_iox2::StreamDescriptor,
    config: &BenchConfig,
) -> Result<rbt_iox2::SliceSubscriber<Image>> {
    let deadline = Instant::now() + config.timeout;
    loop {
        match SliceStream::<Image>::subscriber(descriptor) {
            Ok(subscriber) => return Ok(subscriber),
            Err(error) if Instant::now() < deadline => {
                thread::sleep(OPEN_RETRY_INTERVAL);
                let _ = error;
            }
            Err(error) => {
                return Err(anyhow!(error)).context("open Image subscriber before timeout");
            }
        }
    }
}

fn open_pointcloud_subscriber(
    descriptor: &rbt_iox2::StreamDescriptor,
    config: &BenchConfig,
) -> Result<rbt_iox2::SliceSubscriber<PointCloud>> {
    let deadline = Instant::now() + config.timeout;
    loop {
        match SliceStream::<PointCloud>::subscriber(descriptor) {
            Ok(subscriber) => return Ok(subscriber),
            Err(error) if Instant::now() < deadline => {
                thread::sleep(OPEN_RETRY_INTERVAL);
                let _ = error;
            }
            Err(error) => {
                return Err(anyhow!(error)).context("open PointCloud subscriber before timeout");
            }
        }
    }
}

fn open_control_subscriber(
    descriptor: &rbt_iox2::StreamDescriptor,
    config: &BenchConfig,
) -> Result<rbt_iox2::Subscriber<ControlCommand>> {
    let deadline = Instant::now() + config.timeout;
    loop {
        match Stream::<ControlCommand>::subscriber(descriptor) {
            Ok(subscriber) => return Ok(subscriber),
            Err(error) if Instant::now() < deadline => {
                thread::sleep(OPEN_RETRY_INTERVAL);
                let _ = error;
            }
            Err(error) => {
                return Err(anyhow!(error)).context("open Control subscriber before timeout");
            }
        }
    }
}

fn print_ready(config: &BenchConfig, layout: &str, total: u64) -> Result<()> {
    println!(
        "RBT_BENCH_READY role={} build_mode={} service={} profile={} message={} payload={} count={} warmup={} total={} queue_config={} subscriber_count={} subscriber_mode={} receive_delay_us={} layout_hash={layout}{}",
        config.role.as_str(),
        build_mode(),
        config.service,
        config.profile,
        config.message,
        config.payload_bytes,
        config.count,
        config.warmup,
        total,
        config.profile.queue_config(),
        config.subscriber_count,
        config.subscriber_mode,
        config.receive_delay.as_micros(),
        if config.role == Role::Subscriber {
            " strategy=waitset"
        } else {
            ""
        },
    );
    io::stdout().flush().context("flush benchmark ready line")
}

fn point_count(payload_bytes: usize) -> Result<usize> {
    let point_size = std::mem::size_of::<PointXYZI>();
    if !payload_bytes.is_multiple_of(point_size) {
        bail!("PointCloud payload size {payload_bytes} is not divisible by {point_size}");
    }
    Ok(payload_bytes / point_size)
}

fn timestamp_ns_now() -> Result<u64> {
    let elapsed = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .context("system clock is before UNIX_EPOCH")?;
    u64::try_from(elapsed.as_nanos()).context("system timestamp exceeds u64")
}

fn sleep_between_sends(interval: Duration) {
    if !interval.is_zero() {
        thread::sleep(interval);
    }
}

fn rate(count: u64, elapsed_seconds: f64) -> f64 {
    if elapsed_seconds > 0.0 {
        count as f64 / elapsed_seconds
    } else {
        0.0
    }
}

fn format_optional(value: Option<f64>) -> String {
    value.map_or_else(|| "na".to_owned(), |value| format!("{value:.6}"))
}

fn format_optional_u64(value: Option<u64>) -> String {
    value.map_or_else(|| "na".to_owned(), |value| value.to_string())
}

fn format_latency(value: Option<u64>) -> String {
    value.map_or_else(
        || "na".to_owned(),
        |value| format!("{:.3}", value as f64 / 1_000.0),
    )
}

fn build_mode() -> &'static str {
    if cfg!(debug_assertions) {
        "debug"
    } else {
        "release"
    }
}

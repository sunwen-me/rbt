use std::{
    collections::VecDeque,
    fmt::Display,
    str::FromStr,
    sync::{Arc, Condvar, Mutex},
    thread,
    time::{Duration, Instant},
};

use anyhow::{Result, anyhow};
use rbt_iox2::{
    ActivationId, Image, PixelFormat, ProcessInstanceId, ReceivedSliceSample, SliceStream,
    StreamDescriptor, StreamProfile::Sensor, StreamResourceConfig, WaitContext, WaitProgress,
    WaitToken,
};

const WIDTH: u32 = 1_920;
const HEIGHT: u32 = 1_080;
const CHANNELS: u32 = 3;
const STRIDE: u32 = WIDTH * CHANNELS;
const PAYLOAD_SIZE: usize = STRIDE as usize * HEIGHT as usize;
const RETRY_INTERVAL: Duration = Duration::from_millis(100);

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

fn env_flag(name: &str) -> Result<bool> {
    match std::env::var(name) {
        Ok(value) => parse_flag(name, &value),
        Err(std::env::VarError::NotPresent) => Ok(false),
        Err(error) => Err(anyhow!("failed to read {name}: {error}")),
    }
}

fn parse_flag(name: &str, value: &str) -> Result<bool> {
    match value.trim().to_ascii_lowercase().as_str() {
        "1" | "true" | "yes" | "on" => Ok(true),
        "0" | "false" | "no" | "off" => Ok(false),
        _ => Err(anyhow!(
            "invalid {name}={value:?}; expected 0/1 or false/true"
        )),
    }
}

fn receive_period(rate_hz: f64) -> Result<Option<Duration>> {
    if rate_hz == 0.0 {
        return Ok(None);
    }
    if !rate_hz.is_finite() || rate_hz < 0.0 {
        return Err(anyhow!(
            "RBT_TEST_CONSUMER_RATE_HZ must be zero or a finite positive number"
        ));
    }
    Ok(Some(Duration::from_secs_f64(1.0 / rate_hz)))
}

fn expected_checksum(sequence: u64, payload_len: usize) -> u64 {
    let complete_cycles = payload_len / 256;
    let remainder = payload_len % 256;
    let cycle_sum = (255_u64 * 256) / 2;
    let mut checksum = (complete_cycles as u64).wrapping_mul(cycle_sum);
    let start = sequence % 256;
    for i in 0..remainder {
        checksum = checksum.wrapping_add((start + i as u64) % 256);
    }
    checksum
}

fn main() -> Result<()> {
    let service = required_service_name()?;
    let frames: usize = env_or_default("RBT_IMAGE_FRAMES", 30)?;
    let timeout_secs: f64 = env_or_default("RBT_IMAGE_TIMEOUT_SECS", 15.0)?;
    let processing_delay_ms: u64 = env_or_default("RBT_IMAGE_PROCESSING_DELAY_MS", 0)?;
    let hold_sample_ms: u64 = env_or_default("RBT_TEST_HOLD_SAMPLE_MS", 0)?;
    let stop_receiving = env_flag("RBT_TEST_STOP_RECEIVING")?;
    let consumer_rate_hz: f64 = env_or_default("RBT_TEST_CONSUMER_RATE_HZ", 0.0)?;
    let max_held: usize = env_or_default("RBT_TEST_MAX_HELD", 1)?;
    if !timeout_secs.is_finite() || timeout_secs < 0.0 {
        return Err(anyhow!(
            "RBT_IMAGE_TIMEOUT_SECS must be a finite non-negative number"
        ));
    }
    if max_held == 0 {
        return Err(anyhow!("RBT_TEST_MAX_HELD must be greater than zero"));
    }
    let receive_period = receive_period(consumer_rate_hz)?;
    let timeout = Duration::from_secs_f64(timeout_secs);
    let resources = StreamResourceConfig::default().with_max_borrowed(max_held);
    let descriptor =
        StreamDescriptor::for_slice_with_resources::<Image>(service, Sensor, resources);
    let message_type = descriptor.message_type();
    let deadline = Instant::now() + timeout;

    let subscriber = loop {
        match SliceStream::<Image>::subscriber_with_resources(&descriptor, resources) {
            Ok(subscriber) => break subscriber,
            Err(error) if Instant::now() < deadline => {
                eprintln!("IMAGE_CONSUMER waiting for service: {error}");
                let remaining = deadline.saturating_duration_since(Instant::now());
                thread::sleep(RETRY_INTERVAL.min(remaining));
            }
            Err(error) => {
                return Err(anyhow!(
                    "timed out opening image service {:?}: {error}",
                    descriptor.service_name()
                ));
            }
        }
    };

    println!(
        "IMAGE_CONSUMER bound ABI compatible message={} layout={} profile={:?} payload={} service={}",
        message_type.qualified_name(),
        message_type.layout_hash,
        descriptor.profile(),
        PAYLOAD_SIZE,
        descriptor.service_name(),
    );

    if stop_receiving {
        println!(
            "IMAGE_CONSUMER stalled service={} timeout_ms={} received=0",
            descriptor.service_name(),
            timeout.as_millis(),
        );
        thread::sleep(deadline.saturating_duration_since(Instant::now()));
        let pressure = subscriber
            .pressure()
            .subscriber
            .expect("subscriber pressure snapshot must contain subscriber metrics");
        println!(
            "IMAGE_CONSUMER_PRESSURE borrowed_samples={} borrowed_high_water={} borrow_failures={} received_samples={}",
            pressure.borrowed_samples,
            pressure.borrowed_high_water,
            pressure.borrow_failures,
            pressure.received_samples,
        );
        println!("IMAGE_CONSUMER received=0 sequence_gaps=0 estimated_dropped=0 rate=0.00Hz");
        return Ok(());
    }

    let started = Instant::now();
    let mut received_count = 0usize;
    let mut previous_sequence = None;
    let mut sequence_gaps = 0usize;
    let mut estimated_dropped = 0_u64;
    let hold_sample = Duration::from_millis(hold_sample_ms);
    let processing_delay = Duration::from_millis(processing_delay_ms);
    let mut held: VecDeque<(Instant, ReceivedSliceSample<Image>)> = VecDeque::new();
    let mut next_receive = Instant::now();

    if frames > 0 {
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
            let mut drained = 0usize;
            let mut empty = false;
            for _ in 0..budget {
                let now = Instant::now();
                while held
                    .front()
                    .is_some_and(|(release_at, _)| *release_at <= now)
                {
                    held.pop_front();
                }
                if Instant::now() >= deadline {
                    return Err(anyhow!(
                        "timed out waiting for image frames: received {received_count} of {frames}"
                    ));
                }
                if held.len() >= max_held {
                    let release_at = held.front().map_or(deadline, |entry| entry.0);
                    // This is deliberate Borrow-retention pacing, not an
                    // empty-receive poll. Stay in the current readiness
                    // dispatch until the held guard can be released so a
                    // queued payload cannot lose its only readiness edge.
                    thread::sleep(
                        release_at
                            .saturating_duration_since(now)
                            .min(deadline.saturating_duration_since(now)),
                    );
                    continue;
                }
                if now < next_receive {
                    thread::sleep(
                        next_receive
                            .saturating_duration_since(now)
                            .min(deadline.saturating_duration_since(now)),
                    );
                    continue;
                }

                let Some(received) = subscriber.receive()? else {
                    empty = true;
                    break;
                };

                let header = received.header();
                if header.width != WIDTH
                    || header.height != HEIGHT
                    || header.stride != STRIDE
                    || header.encoding != PixelFormat::RGB8
                {
                    return Err(anyhow!(
                        "invalid image header: sequence={} timestamp_ns={} width={} height={} stride={} encoding={:?}",
                        header.sequence,
                        header.timestamp_ns,
                        header.width,
                        header.height,
                        header.stride,
                        header.encoding,
                    ));
                }

                let payload = received.payload();
                if payload.len() != PAYLOAD_SIZE {
                    return Err(anyhow!(
                        "received payload of {} bytes, expected {PAYLOAD_SIZE}",
                        payload.len()
                    ));
                }

                for &index in &[0, 1, PAYLOAD_SIZE / 2, PAYLOAD_SIZE - 1] {
                    let expected = ((header.sequence + index as u64) % 256) as u8;
                    if payload[index] != expected {
                        return Err(anyhow!(
                            "invalid image pattern at byte {index}: sequence={} expected={} actual={}",
                            header.sequence,
                            expected,
                            payload[index]
                        ));
                    }
                }

                let checksum = payload
                    .iter()
                    .fold(0_u64, |sum, &byte| sum.wrapping_add(u64::from(byte)));
                let expected = expected_checksum(header.sequence, payload.len());
                if checksum != expected {
                    return Err(anyhow!(
                        "invalid image checksum: sequence={} expected={} actual={checksum}",
                        header.sequence,
                        expected
                    ));
                }

                let gap = match previous_sequence {
                    None => 0,
                    Some(previous) if header.sequence > previous => header.sequence - previous - 1,
                    Some(previous) => {
                        return Err(anyhow!(
                            "image sequence is not increasing: previous={} current={}",
                            previous,
                            header.sequence
                        ));
                    }
                };
                if gap > 0 {
                    sequence_gaps += 1;
                    estimated_dropped = estimated_dropped.saturating_add(gap);
                }
                previous_sequence = Some(header.sequence);

                let pointer = payload.as_ptr();
                println!(
                    "IMAGE_CONSUMER seq={} payload={} checksum={} pointer={pointer:p} gap={gap}",
                    header.sequence,
                    payload.len(),
                    checksum,
                );
                received_count += 1;
                drained += 1;
                held.push_back((Instant::now() + hold_sample, received));
                if let Some(period) = receive_period {
                    next_receive = Instant::now() + period;
                }

                if !processing_delay.is_zero() {
                    thread::sleep(processing_delay);
                }
                if received_count >= frames {
                    return Ok(WaitProgress::stop_after_samples(drained));
                }
            }

            if Instant::now() >= deadline {
                return Err(anyhow!(
                    "timed out waiting for image frames: received {received_count} of {frames}"
                ));
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
                .map_err(|_| anyhow!("image wait timeout thread panicked"))?;
            timer_result?;
            wait_result
        })?;
        if wait_result.is_cancelled() && received_count < frames {
            return Err(anyhow!(
                "timed out waiting for image frames: received {received_count} of {frames}"
            ));
        }
    }

    while let Some((release_at, _)) = held.front() {
        let now = Instant::now();
        if now >= *release_at {
            held.pop_front();
        } else if now >= deadline {
            return Err(anyhow!(
                "timed out releasing {} retained image sample(s)",
                held.len()
            ));
        } else {
            thread::sleep(
                release_at
                    .saturating_duration_since(now)
                    .min(deadline.saturating_duration_since(now)),
            );
        }
    }

    let elapsed = started.elapsed().as_secs_f64();
    let rate = if elapsed > 0.0 {
        received_count as f64 / elapsed
    } else {
        0.0
    };
    let pressure = subscriber
        .pressure()
        .subscriber
        .expect("subscriber pressure snapshot must contain subscriber metrics");
    println!(
        "IMAGE_CONSUMER_PRESSURE borrowed_samples={} borrowed_high_water={} borrow_failures={} received_samples={}",
        pressure.borrowed_samples,
        pressure.borrowed_high_water,
        pressure.borrow_failures,
        pressure.received_samples,
    );
    println!(
        "IMAGE_CONSUMER received={} sequence_gaps={} estimated_dropped={} rate={rate:.2}Hz",
        received_count, sequence_gaps, estimated_dropped
    );
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{parse_flag, receive_period};

    #[test]
    fn fault_flag_accepts_shell_and_boolean_forms() {
        for value in ["1", "true", "yes", "on", "TRUE"] {
            assert!(parse_flag("TEST", value).unwrap());
        }
        for value in ["0", "false", "no", "off", "FALSE"] {
            assert!(!parse_flag("TEST", value).unwrap());
        }
        assert!(parse_flag("TEST", "sometimes").is_err());
    }

    #[test]
    fn consumer_rate_is_bounded_or_disabled() {
        assert_eq!(receive_period(0.0).unwrap(), None);
        assert_eq!(
            receive_period(4.0).unwrap(),
            Some(std::time::Duration::from_millis(250))
        );
        assert!(receive_period(-1.0).is_err());
        assert!(receive_period(f64::NAN).is_err());
    }
}

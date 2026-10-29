//! Process-level crash cleanup checks for outstanding Subscriber Borrows and
//! Publisher Loans.
//!
//! The child roles run from this same integration-test binary.  Keeping the
//! child protocol in the test means the test exercises the public rbt wrappers
//! across a real process boundary without adding a production binary solely
//! for fault injection.

#![cfg(target_os = "linux")]

use std::{
    env, fmt, fs,
    io::{self, BufRead, BufReader, Write},
    os::unix::process::ExitStatusExt,
    process::{Child, Command, ExitStatus, Stdio},
    sync::{
        atomic::{AtomicU64, Ordering},
        mpsc::{self, Receiver, RecvTimeoutError},
    },
    thread,
    time::{Duration, Instant},
};

use rbt_iox2::{ControlCommand, Image, SliceStream, Stream, StreamProfile, StreamResourceConfig};

const CHILD_ROLE: &str = "RBT_IOX2_PRESSURE_CHILD_ROLE";
const CHILD_SERVICE: &str = "RBT_IOX2_PRESSURE_CHILD_SERVICE";
const CHILD_READY: &str = "RBT_PRESSURE_CHILD_READY";
const CHILD_HELD: &str = "RBT_PRESSURE_CHILD_HELD";

const STARTUP_WATCHDOG: Duration = Duration::from_secs(10);
const RECOVERY_WATCHDOG: Duration = Duration::from_secs(20);
const CHILD_WATCHDOG: Duration = Duration::from_secs(20);

static NEXT_SERVICE: AtomicU64 = AtomicU64::new(0);

fn unique_service_name() -> String {
    let sequence = NEXT_SERVICE.fetch_add(1, Ordering::Relaxed);
    let value = (u64::from(std::process::id()) << 20) | (sequence & 0x000f_ffff);
    format!("rbt/cap/10000000-0000-4000-8000-{value:012x}/stream")
}

fn limits() -> StreamResourceConfig {
    StreamResourceConfig {
        max_borrowed: 1,
        max_loans: 2,
    }
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct Resources {
    rss_bytes: u64,
    file_descriptors: usize,
    threads: usize,
}

fn resources() -> Resources {
    const PAGE_SIZE: u64 = 4096;
    let rss_pages = fs::read_to_string("/proc/self/statm")
        .ok()
        .and_then(|line| line.split_whitespace().nth(1)?.parse::<u64>().ok())
        .unwrap_or(0);
    let file_descriptors = fs::read_dir("/proc/self/fd").map_or(0, |entries| entries.count());
    let threads = fs::read_dir("/proc/self/task").map_or(0, |entries| entries.count());
    Resources {
        rss_bytes: rss_pages.saturating_mul(PAGE_SIZE),
        file_descriptors,
        threads,
    }
}

fn flush_stdout() {
    io::stdout().flush().expect("flush child readiness marker");
}

fn child_service(role: &str) -> Option<String> {
    if env::var(CHILD_ROLE).ok().as_deref() != Some(role) {
        return None;
    }
    Some(
        env::var(CHILD_SERVICE)
            .unwrap_or_else(|_| panic!("{CHILD_SERVICE} must be set for child role {role}")),
    )
}

fn child_watchdog_deadline() -> Instant {
    Instant::now() + CHILD_WATCHDOG
}

fn sleep_until_killed(deadline: Instant) {
    while Instant::now() < deadline {
        thread::sleep(Duration::from_millis(25));
    }
    panic!("child watchdog expired before the parent sent SIGKILL");
}

/// Child role for Test G: retain a received sample until the process is
/// killed, proving that the transport has to reclaim a live Borrow held by a
/// dead process.
#[test]
fn child_subscriber_holds_sample() {
    let Some(service_name) = child_service("subscriber") else {
        return;
    };

    let descriptor = SliceStream::<Image>::descriptor(service_name.clone(), StreamProfile::Sensor);
    let subscriber = SliceStream::<Image>::subscriber_with_resources(&descriptor, limits())
        .expect("child subscriber opens existing image service");
    println!("{CHILD_READY} role=subscriber service={service_name}");
    flush_stdout();

    let deadline = child_watchdog_deadline();
    let held = loop {
        if let Some(sample) = subscriber.receive().expect("child subscriber receive") {
            break sample;
        }
        assert!(
            Instant::now() < deadline,
            "child subscriber receive watchdog expired"
        );
        thread::sleep(Duration::from_millis(2));
    };
    println!(
        "{CHILD_HELD} role=subscriber sequence={} resources={:?}",
        held.header().sequence,
        resources()
    );
    flush_stdout();

    // Keep `held` in scope.  The parent kills this process while the sample is
    // still borrowed, so the normal Drop path cannot release it.
    let _held = held;
    sleep_until_killed(deadline);
}

/// Child role for Test H: retain two unsent Loans until the process is killed.
#[test]
fn child_publisher_holds_loans() {
    let Some(service_name) = child_service("publisher") else {
        return;
    };

    let descriptor =
        Stream::<ControlCommand>::descriptor(service_name.clone(), StreamProfile::Control);
    let publisher = Stream::<ControlCommand>::publisher_with_resources(&descriptor, limits())
        .expect("child publisher creates control service");
    println!("{CHILD_READY} role=publisher service={service_name}");
    flush_stdout();

    let mut first = publisher.loan().expect("first child publisher loan");
    first.sequence = 1;
    first.value = 1.0;
    first.reserved = 0;
    let mut second = publisher.loan().expect("second child publisher loan");
    second.sequence = 2;
    second.value = 2.0;
    second.reserved = 0;
    println!(
        "{CHILD_HELD} role=publisher loans=2 resources={:?}",
        resources()
    );
    flush_stdout();

    // Keep both Loans in scope.  They are intentionally unsent when SIGKILL
    // arrives, so the parent observes process-level cleanup rather than Drop.
    let (_first, _second) = (first, second);
    sleep_until_killed(child_watchdog_deadline());
}

struct ChildProcess {
    child: Child,
    lines: Receiver<String>,
    output: Vec<String>,
}

impl fmt::Debug for ChildProcess {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("ChildProcess")
            .field("pid", &self.child.id())
            .field("output", &self.output)
            .finish_non_exhaustive()
    }
}

impl ChildProcess {
    fn spawn(test_name: &str, role: &str, service_name: &str) -> io::Result<Self> {
        let executable = env::current_exe()?;
        let mut command = Command::new(executable);
        command
            .args(["--exact", test_name, "--nocapture"])
            .env(CHILD_ROLE, role)
            .env(CHILD_SERVICE, service_name)
            .stdout(Stdio::piped())
            // Child failures are useful test diagnostics and must not block
            // the marker reader on a full stderr pipe.
            .stderr(Stdio::inherit());
        let mut child = command.spawn()?;
        let stdout = child.stdout.take().expect("child stdout was piped");
        let (sender, receiver) = mpsc::channel();
        thread::spawn(move || {
            let reader = BufReader::new(stdout);
            for line in reader.lines() {
                let Ok(line) = line else {
                    break;
                };
                if sender.send(line).is_err() {
                    break;
                }
            }
        });
        Ok(Self {
            child,
            lines: receiver,
            output: Vec::new(),
        })
    }

    fn wait_for_marker(&mut self, marker: &str, timeout: Duration) -> io::Result<()> {
        let deadline = Instant::now() + timeout;
        loop {
            let remaining = deadline.saturating_duration_since(Instant::now());
            if remaining.is_zero() {
                return Err(io::Error::new(
                    io::ErrorKind::TimedOut,
                    format!("timed out waiting for {marker}; output={:?}", self.output),
                ));
            }
            match self.lines.recv_timeout(remaining) {
                Ok(line) => {
                    let matched = line.contains(marker);
                    self.output.push(line);
                    if matched {
                        return Ok(());
                    }
                }
                Err(RecvTimeoutError::Timeout) => {
                    return Err(io::Error::new(
                        io::ErrorKind::TimedOut,
                        format!("timed out waiting for {marker}; output={:?}", self.output),
                    ));
                }
                Err(RecvTimeoutError::Disconnected) => {
                    return Err(io::Error::new(
                        io::ErrorKind::UnexpectedEof,
                        format!("child exited before {marker}; output={:?}", self.output),
                    ));
                }
            }
        }
    }

    fn sigkill(&mut self) -> io::Result<()> {
        // On Unix, Child::kill sends SIGKILL.  The subsequent ExitStatus
        // check below verifies that the intended signal, rather than a normal
        // child exit, ended the role.
        self.child.kill()
    }

    fn wait_for_exit(&mut self, timeout: Duration) -> io::Result<ExitStatus> {
        let deadline = Instant::now() + timeout;
        loop {
            if let Some(status) = self.child.try_wait()? {
                return Ok(status);
            }
            if Instant::now() >= deadline {
                return Err(io::Error::new(
                    io::ErrorKind::TimedOut,
                    format!("child did not exit after SIGKILL; output={:?}", self.output),
                ));
            }
            thread::sleep(Duration::from_millis(2));
        }
    }
}

impl Drop for ChildProcess {
    fn drop(&mut self) {
        if self.child.try_wait().ok().flatten().is_none() {
            let _ = self.child.kill();
        }
        let _ = self.child.wait();
    }
}

fn send_image(
    publisher: &rbt_iox2::SlicePublisher<Image>,
    sequence: u64,
) -> rbt_iox2::Result<usize> {
    let mut sample = publisher.loan_slice(1)?;
    sample.header_mut().sequence = sequence;
    sample.payload_mut()[0] = sequence as u8;
    sample.send()
}

fn receive_image(
    subscriber: &rbt_iox2::SliceSubscriber<Image>,
    minimum_sequence: u64,
    timeout: Duration,
) -> rbt_iox2::Result<u64> {
    let deadline = Instant::now() + timeout;
    loop {
        if let Some(sample) = subscriber.receive()? {
            let sequence = sample.header().sequence;
            if sequence >= minimum_sequence {
                return Ok(sequence);
            }
        }
        if Instant::now() >= deadline {
            panic!("timed out receiving image sequence >= {minimum_sequence}");
        }
        thread::sleep(Duration::from_millis(2));
    }
}

fn send_control(
    publisher: &rbt_iox2::Publisher<ControlCommand>,
    sequence: u64,
) -> rbt_iox2::Result<usize> {
    let mut sample = publisher.loan()?;
    sample.sequence = sequence;
    sample.value = sequence as f32;
    sample.reserved = 0;
    sample.send()
}

fn receive_control(
    subscriber: &rbt_iox2::Subscriber<ControlCommand>,
    minimum_sequence: u64,
    timeout: Duration,
) -> rbt_iox2::Result<u64> {
    let deadline = Instant::now() + timeout;
    loop {
        if let Some(sample) = subscriber.receive()? {
            let sequence = sample.sequence;
            if sequence >= minimum_sequence {
                return Ok(sequence);
            }
        }
        if Instant::now() >= deadline {
            panic!("timed out receiving control sequence >= {minimum_sequence}");
        }
        thread::sleep(Duration::from_millis(2));
    }
}

#[test]
fn test_g_subscriber_sigkill_reclaims_held_sample_and_publisher_recovers() -> rbt_iox2::Result<()> {
    let service_name = unique_service_name();
    let descriptor = SliceStream::<Image>::descriptor(service_name.clone(), StreamProfile::Sensor);
    let publisher = SliceStream::<Image>::publisher_with_resources(&descriptor, 1, limits())?;
    let before = resources();
    let mut child =
        ChildProcess::spawn("child_subscriber_holds_sample", "subscriber", &service_name)
            .expect("spawn subscriber crash child");

    child
        .wait_for_marker(CHILD_READY, STARTUP_WATCHDOG)
        .expect("subscriber child readiness");
    let subscriber_wait_deadline = Instant::now() + STARTUP_WATCHDOG;
    loop {
        let count = publisher
            .pressure()
            .publisher
            .expect("image publisher pressure")
            .subscriber_count;
        if count == 1 {
            break;
        }
        assert!(
            Instant::now() < subscriber_wait_deadline,
            "subscriber did not become visible; count={count}"
        );
        thread::sleep(Duration::from_millis(2));
    }

    assert_eq!(send_image(&publisher, 1)?, 1);
    child
        .wait_for_marker(CHILD_HELD, STARTUP_WATCHDOG)
        .expect("subscriber child held-sample marker");

    let killed_at = Instant::now();
    child.sigkill().expect("SIGKILL subscriber child");
    let status = child
        .wait_for_exit(STARTUP_WATCHDOG)
        .expect("subscriber child exit after SIGKILL");
    assert_eq!(
        status.signal(),
        Some(9),
        "subscriber child was not SIGKILLed: {status:?}"
    );

    // Opening a fresh subscriber and delivering through the original
    // Publisher forces the live process to observe and remove the dead
    // subscriber connection, releasing the held sample's transport offset.
    let recovery_deadline = killed_at + RECOVERY_WATCHDOG;
    let fresh_subscriber = SliceStream::<Image>::subscriber_with_resources(&descriptor, limits())?;
    let mut sequence = 2;
    let recovered_sequence = loop {
        let recipients = send_image(&publisher, sequence)?;
        if recipients > 0
            && let Ok(received) =
                receive_image(&fresh_subscriber, sequence, Duration::from_millis(50))
        {
            break received;
        }
        assert!(
            Instant::now() < recovery_deadline,
            "publisher did not recover after subscriber SIGKILL; recipients={recipients} pressure={:?}",
            publisher.pressure()
        );
        sequence += 1;
        thread::sleep(Duration::from_millis(2));
    };
    let recovery_elapsed = killed_at.elapsed();
    assert!(
        recovery_elapsed <= RECOVERY_WATCHDOG,
        "subscriber cleanup exceeded watchdog: {recovery_elapsed:?}"
    );
    let after = resources();
    eprintln!(
        "[pressure-g] service={service_name} held_sequence=1 recovered_sequence={recovered_sequence} recovery_ms={} resources_before={before:?} resources_after={after:?} publisher_pressure={:?}",
        recovery_elapsed.as_millis(),
        publisher.pressure()
    );
    Ok(())
}

#[test]
fn test_h_publisher_sigkill_reclaims_unsent_loans_and_fresh_publisher_delivers()
-> rbt_iox2::Result<()> {
    let service_name = unique_service_name();
    let descriptor =
        Stream::<ControlCommand>::descriptor(service_name.clone(), StreamProfile::Control);
    let mut child = ChildProcess::spawn("child_publisher_holds_loans", "publisher", &service_name)
        .expect("spawn publisher crash child");
    child
        .wait_for_marker(CHILD_READY, STARTUP_WATCHDOG)
        .expect("publisher child readiness");
    child
        .wait_for_marker(CHILD_HELD, STARTUP_WATCHDOG)
        .expect("publisher child held-loans marker");

    // The child has already created the service.  Opening this Subscriber
    // while the child still owns two unsent Loans proves that those Loans do
    // not destabilize an existing consumer.
    let subscriber = Stream::<ControlCommand>::subscriber_with_resources(&descriptor, limits())?;
    let before = resources();
    let stability_deadline = Instant::now() + Duration::from_millis(250);
    while Instant::now() < stability_deadline {
        assert!(
            subscriber.receive()?.is_none(),
            "subscriber observed an unexpected sample before publisher SIGKILL"
        );
        thread::sleep(Duration::from_millis(2));
    }

    let killed_at = Instant::now();
    child.sigkill().expect("SIGKILL publisher child");
    let status = child
        .wait_for_exit(STARTUP_WATCHDOG)
        .expect("publisher child exit after SIGKILL");
    assert_eq!(
        status.signal(),
        Some(9),
        "publisher child was not SIGKILLed: {status:?}"
    );

    // Keep polling the existing consumer for a bounded interval after the
    // crash.  A dead provider with only unsent Loans must not produce a
    // receive error or a phantom sample.
    let post_kill_stability_deadline = Instant::now() + Duration::from_millis(250);
    while Instant::now() < post_kill_stability_deadline {
        assert!(
            subscriber.receive()?.is_none(),
            "subscriber became unstable after publisher SIGKILL"
        );
        thread::sleep(Duration::from_millis(2));
    }

    // Reopen the same unique capability/service with a fresh Publisher.  The
    // open path performs bounded stale-node cleanup before allocating its
    // endpoint; successful delivery proves the dead publisher's unsent Loans
    // no longer pin the service.
    let recovery_started = Instant::now();
    let recovery_deadline = killed_at + RECOVERY_WATCHDOG;
    let fresh_publisher = loop {
        match Stream::<ControlCommand>::publisher_with_resources(&descriptor, limits()) {
            Ok(publisher) => break publisher,
            Err(error) => {
                assert!(
                    Instant::now() < recovery_deadline,
                    "fresh publisher did not open after publisher SIGKILL: {error}"
                );
                thread::sleep(Duration::from_millis(10));
            }
        }
    };
    let fresh_open_elapsed = recovery_started.elapsed();

    let sequence = 7;
    let recipients = loop {
        match send_control(&fresh_publisher, sequence) {
            Ok(recipients) if recipients > 0 => break recipients,
            Ok(_) | Err(_) => {
                assert!(
                    Instant::now() < recovery_deadline,
                    "fresh publisher did not deliver after stale cleanup; pressure={:?}",
                    fresh_publisher.pressure()
                );
                thread::sleep(Duration::from_millis(2));
            }
        }
    };
    let receive_timeout = recovery_deadline.saturating_duration_since(Instant::now());
    let received_sequence = receive_control(&subscriber, sequence, receive_timeout)?;
    let recovery_elapsed = killed_at.elapsed();
    assert!(
        recovery_elapsed <= RECOVERY_WATCHDOG,
        "publisher cleanup exceeded watchdog: {recovery_elapsed:?}"
    );
    let after = resources();
    eprintln!(
        "[pressure-h] service={service_name} held_loans=2 recipients={recipients} received_sequence={received_sequence} open_ms={} recovery_ms={} resources_before={before:?} resources_after={after:?} publisher_pressure={:?} subscriber_pressure={:?}",
        fresh_open_elapsed.as_millis(),
        recovery_elapsed.as_millis(),
        fresh_publisher.pressure(),
        subscriber.pressure()
    );
    Ok(())
}

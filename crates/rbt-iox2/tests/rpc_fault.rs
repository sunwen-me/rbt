//! Process-level RPC fault checks.
//!
//! The child roles are test functions in this same integration-test binary.
//! This keeps the fault injection on a real process boundary while avoiding a
//! production executable whose only purpose would be to be SIGKILLed.

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

use rbt_iox2::{
    Detection, Frame, RpcClient, RpcDescriptor, RpcError, RpcResourceConfig, RpcServer,
};

const CHILD_ROLE: &str = "RBT_IOX2_RPC_FAULT_CHILD_ROLE";
const CHILD_SERVICE: &str = "RBT_IOX2_RPC_FAULT_CHILD_SERVICE";
const CHILD_READY: &str = "RBT_RPC_FAULT_CHILD_READY";
const CHILD_RECEIVED: &str = "RBT_RPC_FAULT_CHILD_RECEIVED";
const CHILD_SENT: &str = "RBT_RPC_FAULT_CHILD_SENT";

const STARTUP_WATCHDOG: Duration = Duration::from_secs(10);
const RECOVERY_WATCHDOG: Duration = Duration::from_secs(20);
const CHILD_WATCHDOG: Duration = Duration::from_secs(20);

static NEXT_SERVICE: AtomicU64 = AtomicU64::new(0);

fn unique_service_name(test: &str) -> String {
    let sequence = NEXT_SERVICE.fetch_add(1, Ordering::Relaxed);
    let value = (u64::from(std::process::id()) << 20) | (sequence & 0x000f_ffff);
    format!("rbt/rpc/00000000-0000-4000-8000-{value:012x}/{test}")
}

fn descriptor(service_name: String, resources: RpcResourceConfig) -> RpcDescriptor {
    RpcDescriptor::new_with_resources(
        service_name,
        Frame::descriptor(),
        Detection::descriptor(),
        resources,
    )
}

fn one_in_flight_resources() -> RpcResourceConfig {
    RpcResourceConfig::default()
        .with_max_clients(1)
        .with_max_in_flight(1)
        .with_max_request_loans(2)
}

fn flush_stdout() {
    io::stdout().flush().expect("flush child marker");
}

fn child_service(role: &str) -> Option<String> {
    if env::var(CHILD_ROLE).ok().as_deref() != Some(role) {
        return None;
    }
    Some(
        env::var(CHILD_SERVICE).unwrap_or_else(|_| {
            panic!("{CHILD_SERVICE} must be set for RPC fault child role {role}")
        }),
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

/// Child role for the server-crash test.  It receives one request and keeps
/// the active request alive without producing a response until the parent
/// kills this process.
#[test]
fn child_rpc_server_holds_request() {
    let Some(service_name) = child_service("server") else {
        return;
    };
    let descriptor = descriptor(service_name.clone(), RpcResourceConfig::default());
    let mut server =
        RpcServer::<Frame, Detection>::open_or_create(&descriptor).expect("child RPC server opens");
    println!("{CHILD_READY} service={service_name}");
    flush_stdout();

    let request = loop {
        if let Some(request) = server
            .receive_timeout(Duration::from_millis(100))
            .expect("child RPC server receive")
        {
            break request;
        }
    };
    println!("{CHILD_RECEIVED} service={service_name}");
    flush_stdout();
    let _request = request;
    sleep_until_killed(child_watchdog_deadline());
}

/// Child role for the client-crash test.  It sends one request and retains the
/// pending native response until SIGKILL, so the parent can verify transport
/// reclamation rather than ordinary Rust drop behavior.
#[test]
fn child_rpc_client_holds_request() {
    let Some(service_name) = child_service("client") else {
        return;
    };
    let descriptor = descriptor(service_name.clone(), one_in_flight_resources());
    let client = RpcClient::<Frame, Detection>::open(&descriptor).expect("child RPC client opens");
    println!("{CHILD_READY} service={service_name}");
    flush_stdout();

    let pending = client
        .loan_request()
        .expect("child RPC request loan")
        .send()
        .expect("child RPC request send");
    println!("{CHILD_SENT} service={service_name}");
    flush_stdout();
    let _pending = pending;
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
            // Keep stderr inherited so a child panic cannot fill a pipe and
            // prevent the marker reader from observing its final output.
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
        // `Child::kill` is SIGKILL on Unix.  The caller verifies the signal in
        // the resulting status instead of accepting an ordinary child exit.
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

/// Killing a server with a request outstanding must wake the client within a
/// bounded watchdog.  A replacement server on the same physical service must
/// then handle a fresh call without reusing the old response.
#[test]
fn server_sigkill_wakes_call_and_replacement_server_works() -> Result<(), RpcError> {
    let service_name = unique_service_name("server-kill");
    let descriptor = descriptor(service_name.clone(), RpcResourceConfig::default());
    let before = resources();
    let mut child = ChildProcess::spawn("child_rpc_server_holds_request", "server", &service_name)
        .expect("spawn RPC server crash child");
    child
        .wait_for_marker(CHILD_READY, STARTUP_WATCHDOG)
        .expect("RPC server child readiness");

    let client = RpcClient::<Frame, Detection>::open(&descriptor)?;
    let pending = client.loan_request()?.send()?;
    child
        .wait_for_marker(CHILD_RECEIVED, STARTUP_WATCHDOG)
        .expect("RPC server child received outstanding request");

    // Keep the call's own deadline shorter than the process-recovery
    // watchdog.  A healthy transport returns a typed disconnect quickly;
    // `DeadlineExceeded` is still an allowed ambiguity if native cleanup is
    // delayed, but the test must not spend the full recovery budget waiting.
    let (result_tx, result_rx) = mpsc::channel();
    thread::spawn(move || {
        let _ = result_tx.send(pending.wait(Duration::from_secs(5)));
    });
    let killed_at = Instant::now();
    child.sigkill().expect("SIGKILL RPC server child");
    let status = child
        .wait_for_exit(STARTUP_WATCHDOG)
        .expect("RPC server child exit after SIGKILL");
    assert_eq!(
        status.signal(),
        Some(9),
        "RPC server child was not SIGKILLed"
    );

    let result = result_rx
        .recv_timeout(RECOVERY_WATCHDOG)
        .expect("outstanding RPC waiter exceeded recovery watchdog");
    let elapsed = killed_at.elapsed();
    assert!(
        elapsed <= RECOVERY_WATCHDOG,
        "server SIGKILL did not terminate the outstanding call within watchdog: {elapsed:?}"
    );
    assert!(
        matches!(
            result,
            Err(RpcError::ServerUnavailable)
                | Err(RpcError::DeadlineExceeded)
                | Err(RpcError::TransportFailure { .. })
        ),
        "server SIGKILL produced an unbounded or untyped result: {result:?}"
    );

    let replacement_started = Instant::now();
    let mut replacement = loop {
        match RpcServer::<Frame, Detection>::open_or_create(&descriptor) {
            Ok(server) => break server,
            Err(error) => {
                assert!(
                    replacement_started.elapsed() <= RECOVERY_WATCHDOG,
                    "replacement server did not open after SIGKILL: {error:?}"
                );
                thread::sleep(Duration::from_millis(10));
            }
        }
    };
    let replacement_thread = thread::spawn(move || -> Result<(), RpcError> {
        let request = replacement
            .receive_timeout(Duration::from_secs(5))?
            .expect("replacement request");
        let mut response = request.loan_response()?;
        response.sequence = 2;
        response.value = 2_002;
        response.send()
    });
    let response = client.call_copy(
        Frame {
            sequence: 2,
            value: 2_000,
        },
        Duration::from_secs(5),
    )?;
    assert_eq!((response.sequence, response.value), (2, 2_002));
    replacement_thread
        .join()
        .expect("replacement server thread")?;

    let after = resources();
    eprintln!(
        "[rpc-fault-server] service={service_name} outstanding_ms={} replacement_ms={} resources_before={before:?} resources_after={after:?}",
        elapsed.as_millis(),
        replacement_started.elapsed().as_millis(),
    );
    Ok(())
}

/// Killing a client while the server owns its request must release the server
/// request slot.  A fresh client can then use the same bounded endpoint.
#[test]
fn client_sigkill_releases_server_request_and_fresh_client_works() -> Result<(), RpcError> {
    let service_name = unique_service_name("client-kill");
    let descriptor = descriptor(service_name.clone(), one_in_flight_resources());
    let mut server = RpcServer::<Frame, Detection>::open_or_create(&descriptor)?;
    let before = resources();
    let mut child = ChildProcess::spawn("child_rpc_client_holds_request", "client", &service_name)
        .expect("spawn RPC client crash child");
    child
        .wait_for_marker(CHILD_READY, STARTUP_WATCHDOG)
        .expect("RPC client child readiness");

    child
        .wait_for_marker(CHILD_SENT, STARTUP_WATCHDOG)
        .expect("RPC client child sent outstanding request");
    let request = server
        .receive_timeout(STARTUP_WATCHDOG)?
        .expect("server received client child request");
    let held_metrics = server
        .pressure_snapshot()
        .server
        .expect("server pressure metrics while client request is held");
    assert_eq!(held_metrics.requests_current, 1);

    let killed_at = Instant::now();
    child.sigkill().expect("SIGKILL RPC client child");
    let status = child
        .wait_for_exit(STARTUP_WATCHDOG)
        .expect("RPC client child exit after SIGKILL");
    assert_eq!(
        status.signal(),
        Some(9),
        "RPC client child was not SIGKILLed"
    );
    drop(request);

    let server_thread = thread::spawn(move || -> Result<(), RpcError> {
        loop {
            if let Some(request) = server.receive_timeout(Duration::from_millis(100))? {
                let mut response = request.loan_response()?;
                response.sequence = 7;
                response.value = 7_007;
                response.send()?;
                return Ok(());
            }
        }
    });

    let recovery_deadline = killed_at + RECOVERY_WATCHDOG;
    let response = loop {
        match RpcClient::<Frame, Detection>::open(&descriptor).and_then(|client| {
            client.call_copy(
                Frame {
                    sequence: 7,
                    value: 7_000,
                },
                Duration::from_millis(250),
            )
        }) {
            Ok(response) => break response,
            Err(error) => {
                assert!(
                    Instant::now() < recovery_deadline,
                    "fresh client did not recover after client SIGKILL: {error:?}"
                );
                thread::sleep(Duration::from_millis(10));
            }
        }
    };
    assert_eq!((response.sequence, response.value), (7, 7_007));
    drop(response);
    server_thread.join().expect("fresh server thread")?;

    let elapsed = killed_at.elapsed();
    assert!(elapsed <= RECOVERY_WATCHDOG);
    let after = resources();
    eprintln!(
        "[rpc-fault-client] service={service_name} recovery_ms={} resources_before={before:?} resources_after={after:?}",
        elapsed.as_millis(),
    );
    Ok(())
}

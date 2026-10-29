//! Local development process launcher for the run command.
//!
//! The launcher deliberately stays outside the runtime protocol. It starts a
//! Composer and the configured component executables, observes lifecycle state
//! through the existing read-only inspection endpoint, and owns the child
//! process groups until they have all been reaped. Runtime truth continues to
//! come from Composer and component snapshots, not from this module.
//!
//! Process-group setup and signalling use the Linux setsid and kill
//! executables rather than an unsafe libc call. This keeps the launcher
//! implementation auditable and makes it explicit that every child belongs to
//! a group owned by the launcher.

use std::{
    collections::{BTreeMap, BTreeSet},
    env,
    io::{self, BufRead, BufReader, Write},
    path::{Path, PathBuf},
    process::{Child, Command, ExitStatus, Stdio},
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

use rbt_compose::InspectionView;
use rbt_core::ComponentState;
use serde::Serialize;
use tokio::{
    net::TcpStream,
    signal::unix::{Signal, SignalKind, signal},
    time::sleep,
};

use crate::{
    DEFAULT_COMPOSER_ADDR, fetch_view,
    project::{ComponentConfig, Project, ProjectCheckReport},
};

/// Options for the local launcher.
#[derive(Clone, Debug, Default, Eq, PartialEq)]
pub struct RunOptions {
    /// Optional address override. A configured rbt.toml address is used when
    /// this is absent, followed by the frozen local default.
    pub composer_address: Option<String>,
    /// Skip the configured build command. This is intended for tests and for a
    /// caller that has already built the project; normal run keeps it false.
    pub skip_build: bool,
}

/// A child-process result retained in the launcher summary.
#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
pub struct ProcessSummary {
    pub name: String,
    pub pid: u32,
    pub exit_code: Option<i32>,
    pub signal: Option<i32>,
}

/// Result of one complete local launcher session.
#[derive(Clone, Debug, Eq, PartialEq, Serialize)]
pub struct RunSummary {
    pub composer: ProcessSummary,
    pub components: Vec<ProcessSummary>,
    pub ready: bool,
    pub interrupted: bool,
    pub forced_shutdown: bool,
    pub runtime_failure: bool,
}

impl RunSummary {
    /// Returns the launcher exit status represented by this summary.
    pub const fn status_code(&self) -> i32 {
        if self.ready && !self.runtime_failure && !self.forced_shutdown {
            0
        } else {
            1
        }
    }
}

/// Failures that prevent a local launcher from owning a complete session.
#[derive(Debug, thiserror::Error)]
pub enum RunnerError {
    #[error("rbt run is supported only on Linux x86_64")]
    UnsupportedPlatform,
    #[error("project static check failed: {0}")]
    Preflight(String),
    #[error("invalid run configuration: {0}")]
    InvalidConfiguration(String),
    #[error("failed to run build command: {0}")]
    Build(String),
    #[error("build interrupted")]
    BuildInterrupted,
    #[error("failed to start {label}: {source}")]
    Spawn {
        label: String,
        #[source]
        source: io::Error,
    },
    #[error("{label} exited before startup completed: {status}")]
    StartupExit { label: String, status: String },
    #[error("Composer at {address} did not become reachable within {timeout_seconds}s")]
    ComposerTimeout {
        address: String,
        timeout_seconds: u64,
    },
    #[error(
        "configured Composer address {0} is already in use; rbt run will not attach to an existing Composer"
    )]
    AddressInUse(String),
    #[error("startup did not reach ACTIVE for every configured component: {0}")]
    StartupTimeout(String),
    #[error("startup interrupted before all components became ACTIVE")]
    StartupInterrupted,
    #[error("inspection failed while launching: {0}")]
    Inspection(String),
    #[error("failed to install signal handling: {0}")]
    Signal(#[source] io::Error),
    #[error("process supervision failed: {0}")]
    Supervision(#[source] io::Error),
}

type Result<T> = std::result::Result<T, RunnerError>;

enum InspectionOutcome {
    View(InspectionView),
    Unavailable,
    Protocol(String),
    Signal,
}

/// A direct child launched in a new process group.
struct ManagedProcess {
    name: String,
    pid: u32,
    child: Child,
    status: Option<ExitStatus>,
}

impl ManagedProcess {
    fn poll(&mut self) -> io::Result<Option<ExitStatus>> {
        if self.status.is_none()
            && let Some(status) = self.child.try_wait()?
        {
            self.status = Some(status);
            let _ = writeln!(
                io::stderr(),
                "[{}] exited ({})",
                self.name,
                display_status(status)
            );
        }
        Ok(self.status)
    }

    fn alive(&self) -> bool {
        self.status.is_none() || process_group_exists(self.pid)
    }

    fn summary(&self) -> ProcessSummary {
        let (exit_code, signal) = self.status.map_or((None, None), |status| {
            #[cfg(unix)]
            {
                use std::os::unix::process::ExitStatusExt;
                (status.code(), status.signal())
            }
            #[cfg(not(unix))]
            {
                (status.code(), None)
            }
        });
        ProcessSummary {
            name: self.name.clone(),
            pid: self.pid,
            exit_code,
            signal,
        }
    }
}

/// Run a project after its static project-check report has been computed.
///
/// The report is passed in explicitly so the command layer can print the same
/// diagnostics for check and run. A report containing any failure is refused
/// before build or process creation.
pub async fn run_project(
    project: &Project,
    check: &ProjectCheckReport,
    options: RunOptions,
) -> Result<RunSummary> {
    if !cfg!(all(target_os = "linux", target_arch = "x86_64")) {
        return Err(RunnerError::UnsupportedPlatform);
    }
    ensure_preflight(check)?;

    let mut sigint = signal(SignalKind::interrupt()).map_err(RunnerError::Signal)?;
    let mut sigterm = signal(SignalKind::terminate()).map_err(RunnerError::Signal)?;

    let address = options
        .composer_address
        .clone()
        .or_else(|| project.config.run.composer_address.clone())
        .unwrap_or_else(|| DEFAULT_COMPOSER_ADDR.to_owned());
    if TcpStream::connect(&address).await.is_ok() {
        return Err(RunnerError::AddressInUse(address));
    }

    let composer_argv = locate_composer(project)?;
    verify_composer_version(project, &composer_argv)?;
    if !options.skip_build {
        run_build(
            project,
            Duration::from_secs(project.config.run.shutdown_timeout_seconds.max(1)),
            &mut sigint,
            &mut sigterm,
        )
        .await?;
    }

    let mut processes = Vec::new();
    let result = run_owned(
        project,
        &address,
        &composer_argv,
        &mut processes,
        &mut sigint,
        &mut sigterm,
    )
    .await;
    if result.is_err() {
        force_kill_all(&mut processes);
    }
    result
}

fn ensure_preflight(check: &ProjectCheckReport) -> Result<()> {
    if check.valid {
        return Ok(());
    }
    let failures = check
        .diagnostics
        .iter()
        .filter(|diagnostic| matches!(diagnostic.status, crate::project::DiagnosticStatus::Fail))
        .map(|diagnostic| {
            diagnostic.details.as_deref().map_or_else(
                || diagnostic.summary.clone(),
                |details| format!("{}: {}", diagnostic.summary, details),
            )
        })
        .collect::<Vec<_>>();
    let message = if failures.is_empty() {
        "report is not valid".to_owned()
    } else {
        failures.join("; ")
    };
    Err(RunnerError::Preflight(message))
}

async fn run_build(
    project: &Project,
    shutdown_limit: Duration,
    sigint: &mut Signal,
    sigterm: &mut Signal,
) -> Result<()> {
    let command = &project.config.build.command;
    let Some(program) = command.first() else {
        return Err(RunnerError::InvalidConfiguration(
            "build.command must contain an executable".to_owned(),
        ));
    };
    if program.is_empty() {
        return Err(RunnerError::InvalidConfiguration(
            "build.command must contain a non-empty executable".to_owned(),
        ));
    }
    eprintln!("[rbt] build: {}", display_argv(command));
    let mut process = spawn_grouped("build", command, &project.root, &BTreeMap::new(), "", None)?;
    loop {
        process.poll().map_err(RunnerError::Supervision)?;
        if let Some(status) = process.status {
            if process_group_exists(process.pid) {
                force_kill_process(&mut process);
            }
            if status.success() {
                return Ok(());
            }
            return Err(RunnerError::Build(format!(
                "{} exited ({})",
                display_argv(command),
                display_status(status)
            )));
        }
        tokio::select! {
            _ = sigint.recv() => {
                let _ = graceful_shutdown_process(
                    &mut process,
                    shutdown_limit,
                    sigint,
                    sigterm,
                ).await;
                return Err(RunnerError::BuildInterrupted);
            }
            _ = sigterm.recv() => {
                let _ = graceful_shutdown_process(
                    &mut process,
                    shutdown_limit,
                    sigint,
                    sigterm,
                ).await;
                return Err(RunnerError::BuildInterrupted);
            }
            _ = sleep(Duration::from_millis(50)) => {}
        }
    }
}

fn locate_composer(project: &Project) -> Result<Vec<String>> {
    if let Some(command) = &project.config.run.composer {
        validate_argv(command, "run.composer")?;
        return Ok(command.clone());
    }

    let mut candidates = Vec::new();
    candidates.push(project.root.join("target/debug/composer"));
    candidates.push(project.root.join("target/debug/rbt-composer"));
    if let Ok(executable) = env::current_exe()
        && let Some(parent) = executable.parent()
    {
        candidates.push(parent.join("composer"));
        candidates.push(parent.join("rbt-composer"));
    }
    for name in ["composer", "rbt-composer"] {
        if let Some(path) = path_executable(name) {
            candidates.push(path);
        }
    }
    candidates.dedup();
    candidates
        .into_iter()
        .find(|candidate| candidate.is_file())
        .map(|path| vec![path.to_string_lossy().into_owned()])
        .ok_or_else(|| {
            RunnerError::InvalidConfiguration(
                "no Composer executable found; set [run].composer in rbt.toml".to_owned(),
            )
        })
}

fn verify_composer_version(project: &Project, argv: &[String]) -> Result<()> {
    let program = argv
        .first()
        .ok_or_else(|| RunnerError::InvalidConfiguration("Composer command is empty".to_owned()))?;
    let basename = Path::new(program)
        .file_name()
        .and_then(|value| value.to_str())
        .unwrap_or(program);
    if !matches!(basename, "composer" | "rbt-composer") {
        // A configured wrapper cannot be safely rewritten as a version probe.
        return Ok(());
    }
    let output = Command::new(program)
        .arg("--version")
        .current_dir(&project.root)
        .output()
        .map_err(|source| RunnerError::Spawn {
            label: "Composer version probe".to_owned(),
            source,
        })?;
    if !output.status.success() {
        return Err(RunnerError::InvalidConfiguration(format!(
            "Composer version probe failed ({}); use Composer {} from the same installation as rbt",
            display_status(output.status),
            env!("CARGO_PKG_VERSION")
        )));
    }
    let text = String::from_utf8_lossy(&output.stdout);
    let version = text
        .split_whitespace()
        .find(|word| {
            let mut pieces = word.split('.');
            pieces
                .next()
                .is_some_and(|piece| piece.chars().all(|c| c.is_ascii_digit()))
                && pieces
                    .next()
                    .is_some_and(|piece| piece.chars().all(|c| c.is_ascii_digit()))
                && pieces
                    .next()
                    .is_some_and(|piece| piece.chars().next().is_some_and(|c| c.is_ascii_digit()))
        })
        .unwrap_or("unknown");
    if version != env!("CARGO_PKG_VERSION") {
        return Err(RunnerError::InvalidConfiguration(format!(
            "Composer reports version {version}, but rbt is {}; use tools from the same installation",
            env!("CARGO_PKG_VERSION")
        )));
    }
    Ok(())
}

fn path_executable(name: &str) -> Option<PathBuf> {
    let path = env::var_os("PATH")?;
    env::split_paths(&path)
        .map(|directory| directory.join(name))
        .find(|candidate| candidate.is_file())
}

fn validate_argv(argv: &[String], field: &str) -> Result<()> {
    if argv.first().is_none_or(String::is_empty) {
        return Err(RunnerError::InvalidConfiguration(format!(
            "{field} must contain a non-empty executable"
        )));
    }
    Ok(())
}

fn component_argv(project: &Project, component: &ComponentConfig) -> Result<Vec<String>> {
    if let Some(command) = &component.command {
        validate_argv(command, &format!("component {} command", component.name))?;
        return Ok(command.clone());
    }
    let package = component.package.as_deref().ok_or_else(|| {
        RunnerError::InvalidConfiguration(format!(
            "component {} has neither package nor command",
            component.name
        ))
    })?;
    if package.is_empty() || package.contains('/') || package.contains('\\') {
        return Err(RunnerError::InvalidConfiguration(format!(
            "component {} package is not a local binary name: {package:?}",
            component.name
        )));
    }
    Ok(vec![
        project
            .root
            .join("target/debug")
            .join(package)
            .to_string_lossy()
            .into_owned(),
    ])
}

async fn run_owned(
    project: &Project,
    address: &str,
    composer_argv: &[String],
    processes: &mut Vec<ManagedProcess>,
    sigint: &mut Signal,
    sigterm: &mut Signal,
) -> Result<RunSummary> {
    validate_argv(composer_argv, "Composer command")?;
    let timeout_seconds = project.config.run.startup_timeout_seconds.max(1);
    let shutdown_seconds = project.config.run.shutdown_timeout_seconds.max(1);

    let composer = spawn_grouped(
        "composer",
        composer_argv,
        &project.root,
        &BTreeMap::new(),
        address,
        None,
    )?;
    processes.push(composer);

    let startup_deadline = Instant::now() + Duration::from_secs(timeout_seconds);
    let mut states = BTreeMap::new();
    let composer_view = loop {
        processes[0].poll().map_err(RunnerError::Supervision)?;
        if let Some(status) = processes[0].status {
            return Err(RunnerError::StartupExit {
                label: "composer".to_owned(),
                status: display_status(status),
            });
        }
        match inspect_with_signals(address, sigint, sigterm).await {
            InspectionOutcome::View(view) => break view,
            InspectionOutcome::Unavailable => {}
            InspectionOutcome::Protocol(error) => return Err(RunnerError::Inspection(error)),
            InspectionOutcome::Signal => {
                let forced_shutdown = graceful_shutdown(
                    processes,
                    &[],
                    Duration::from_secs(shutdown_seconds),
                    sigint,
                    sigterm,
                )
                .await;
                return complete_summary(processes, &[], false, true, forced_shutdown, false);
            }
        }
        if Instant::now() >= startup_deadline {
            return Err(RunnerError::ComposerTimeout {
                address: address.to_owned(),
                timeout_seconds,
            });
        }
        tokio::select! {
            _ = sigint.recv() => {
                let forced_shutdown = graceful_shutdown(
                    processes,
                    &[],
                    Duration::from_secs(shutdown_seconds),
                    sigint,
                    sigterm,
                ).await;
                return complete_summary(
                    processes,
                    &[],
                    false,
                    true,
                    forced_shutdown,
                    false,
                );
            }
            _ = sigterm.recv() => {
                let forced_shutdown = graceful_shutdown(
                    processes,
                    &[],
                    Duration::from_secs(shutdown_seconds),
                    sigint,
                    sigterm,
                ).await;
                return complete_summary(
                    processes,
                    &[],
                    false,
                    true,
                    forced_shutdown,
                    false,
                );
            }
            _ = sleep(Duration::from_millis(100)) => {}
        }
    };
    emit_transitions(&composer_view, &mut states, &BTreeSet::new());

    let mut component_indices = Vec::new();
    for component in &project.config.components {
        let argv = component_argv(project, component)?;
        let process = spawn_grouped(
            &component.name,
            &argv,
            &project.root,
            &component.environment,
            address,
            None,
        )?;
        component_indices.push(processes.len());
        processes.push(process);
    }

    let expected = project
        .config
        .components
        .iter()
        .map(|component| component.name.clone())
        .collect::<BTreeSet<_>>();
    let startup_deadline = Instant::now() + Duration::from_secs(timeout_seconds);
    let ready = loop {
        poll_all(processes)?;
        for &index in &component_indices {
            if let Some(status) = processes[index].status {
                return Err(RunnerError::StartupExit {
                    label: processes[index].name.clone(),
                    status: display_status(status),
                });
            }
        }
        match inspect_with_signals(address, sigint, sigterm).await {
            InspectionOutcome::View(view) => {
                emit_transitions(&view, &mut states, &expected);
                if view_is_ready(&view, &expected) {
                    eprintln!(
                        "[rbt] Project: {}\n[rbt] Composer running; {} component(s), {} capability/capabilities, 0 blocked, 0 failed",
                        project.config.project.name,
                        expected.len(),
                        view.capability_topology.len()
                    );
                    break true;
                }
                if let Some(failure) = lifecycle_failure(&view, &expected) {
                    return Err(RunnerError::StartupTimeout(failure));
                }
            }
            InspectionOutcome::Unavailable => {}
            InspectionOutcome::Protocol(error) => return Err(RunnerError::Inspection(error)),
            InspectionOutcome::Signal => {
                let forced_shutdown = graceful_shutdown(
                    processes,
                    &component_indices,
                    Duration::from_secs(shutdown_seconds),
                    sigint,
                    sigterm,
                )
                .await;
                return complete_summary(
                    processes,
                    &component_indices,
                    false,
                    true,
                    forced_shutdown,
                    false,
                );
            }
        }
        if Instant::now() >= startup_deadline {
            let missing = expected
                .iter()
                .filter(|name| states.get(*name).map(String::as_str) != Some("ACTIVE"))
                .cloned()
                .collect::<Vec<_>>();
            return Err(RunnerError::StartupTimeout(if missing.is_empty() {
                "component lifecycle did not reach ACTIVE".to_owned()
            } else {
                missing.join(", ")
            }));
        }
        tokio::select! {
            _ = sigint.recv() => {
                let forced_shutdown = graceful_shutdown(
                    processes,
                    &component_indices,
                    Duration::from_secs(shutdown_seconds),
                    sigint,
                    sigterm,
                ).await;
                return complete_summary(
                    processes,
                    &component_indices,
                    false,
                    true,
                    forced_shutdown,
                    false,
                );
            }
            _ = sigterm.recv() => {
                let forced_shutdown = graceful_shutdown(
                    processes,
                    &component_indices,
                    Duration::from_secs(shutdown_seconds),
                    sigint,
                    sigterm,
                ).await;
                return complete_summary(
                    processes,
                    &component_indices,
                    false,
                    true,
                    forced_shutdown,
                    false,
                );
            }
            _ = sleep(Duration::from_millis(150)) => {}
        }
    };

    let mut interrupted = false;
    let mut runtime_failure = false;
    let mut reported_exits = BTreeSet::new();
    eprintln!("[rbt] all configured components ACTIVE; Ctrl-C to stop");
    let mut tick = tokio::time::interval(Duration::from_millis(250));
    tick.set_missed_tick_behavior(tokio::time::MissedTickBehavior::Skip);
    loop {
        tokio::select! {
            _ = sigint.recv() => {
                interrupted = true;
                break;
            }
            _ = sigterm.recv() => {
                interrupted = true;
                break;
            }
            _ = tick.tick() => {
                poll_all(processes)?;
                for &index in &component_indices {
                    if processes[index].status.is_some() {
                        runtime_failure = true;
                        if reported_exits.insert(processes[index].name.clone())
                            && let Some(status) = processes[index].status
                        {
                            eprintln!(
                                "[rbt] component {} exited during runtime ({})",
                                processes[index].name,
                                display_status(status)
                            );
                        }
                    }
                }
                if processes[0].status.is_some() {
                    runtime_failure = true;
                    eprintln!("[rbt] Composer exited during runtime");
                    break;
                }
                match inspect_with_signals(address, sigint, sigterm).await {
                    InspectionOutcome::View(view) => {
                        emit_transitions(&view, &mut states, &expected);
                        if lifecycle_failure(&view, &expected).is_some() {
                            runtime_failure = true;
                        }
                    }
                    InspectionOutcome::Unavailable => {}
                    InspectionOutcome::Protocol(_) => {}
                    InspectionOutcome::Signal => {
                        interrupted = true;
                        break;
                    }
                }
            }
        }
    }

    let forced_shutdown = graceful_shutdown(
        processes,
        &component_indices,
        Duration::from_secs(shutdown_seconds),
        sigint,
        sigterm,
    )
    .await;

    let summary = complete_summary(
        processes,
        &component_indices,
        ready,
        interrupted,
        forced_shutdown,
        runtime_failure,
    )?;
    if summary.status_code() != 0 && !interrupted {
        eprintln!("[rbt] run finished with status {}", summary.status_code());
    }
    Ok(summary)
}

async fn inspect_with_signals(
    address: &str,
    sigint: &mut Signal,
    sigterm: &mut Signal,
) -> InspectionOutcome {
    tokio::select! {
        biased;
        _ = sigint.recv() => InspectionOutcome::Signal,
        _ = sigterm.recv() => InspectionOutcome::Signal,
        inspection = fetch_view(address.to_owned()) => match inspection {
            Ok(view) => InspectionOutcome::View(view),
            Err(crate::CliError::Protocol(error)) => InspectionOutcome::Protocol(error),
            Err(_) => InspectionOutcome::Unavailable,
        },
    }
}

fn complete_summary(
    processes: &mut [ManagedProcess],
    component_indices: &[usize],
    ready: bool,
    interrupted: bool,
    forced_shutdown: bool,
    runtime_failure: bool,
) -> Result<RunSummary> {
    poll_all(processes)?;
    let component_summaries = component_indices
        .iter()
        .map(|&index| processes[index].summary())
        .collect::<Vec<_>>();
    Ok(RunSummary {
        composer: processes[0].summary(),
        components: component_summaries,
        ready,
        interrupted,
        forced_shutdown,
        runtime_failure,
    })
}

fn spawn_grouped(
    name: &str,
    argv: &[String],
    cwd: &Path,
    environment: &BTreeMap<String, String>,
    address: &str,
    output: Option<Arc<Mutex<Vec<String>>>>,
) -> Result<ManagedProcess> {
    validate_argv(argv, name)?;
    let mut command = Command::new("setsid");
    command
        .arg("--")
        .arg(&argv[0])
        .args(&argv[1..])
        .current_dir(cwd)
        .envs(environment)
        .env("RBT_COMPOSER_ADDR", address)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    let mut child = command.spawn().map_err(|source| RunnerError::Spawn {
        label: name.to_owned(),
        source,
    })?;
    let pid = child.id();
    eprintln!("[rbt] started {name} pid={pid}");
    if let Some(stdout) = child.stdout.take() {
        spawn_output_reader(name.to_owned(), stdout, false, output.clone());
    }
    if let Some(stderr) = child.stderr.take() {
        spawn_output_reader(name.to_owned(), stderr, true, output.clone());
    }
    Ok(ManagedProcess {
        name: name.to_owned(),
        pid,
        child,
        status: None,
    })
}

fn spawn_output_reader<R: io::Read + Send + 'static>(
    name: String,
    reader: R,
    stderr: bool,
    output: Option<Arc<Mutex<Vec<String>>>>,
) {
    let _ = std::thread::Builder::new()
        .name(format!("rbt-run-{name}"))
        .spawn(move || {
            let lines = BufReader::new(reader).lines();
            for result in lines {
                let Ok(line) = result else { break };
                if let Some(sink) = &output
                    && let Ok(mut sink) = sink.lock()
                {
                    sink.push(format!("[{name}] {line}"));
                }
                if stderr {
                    let _ = writeln!(io::stderr(), "[{name}] {line}");
                } else {
                    let _ = writeln!(io::stdout(), "[{name}] {line}");
                }
            }
        });
}

fn poll_all(processes: &mut [ManagedProcess]) -> Result<()> {
    for process in processes {
        process.poll().map_err(RunnerError::Supervision)?;
    }
    Ok(())
}

fn force_kill_all(processes: &mut [ManagedProcess]) {
    for process in processes {
        force_kill_process(process);
    }
}

fn force_kill_process(process: &mut ManagedProcess) {
    if process.status.is_none() || process_group_exists(process.pid) {
        let _ = signal_group(process.pid, "KILL");
    }
    if process.status.is_none() {
        let _ = process.child.kill();
        process.status = process.child.wait().ok();
    }
}

async fn graceful_shutdown_process(
    process: &mut ManagedProcess,
    limit: Duration,
    sigint: &mut Signal,
    sigterm: &mut Signal,
) -> bool {
    if process.alive() {
        let _ = signal_group(process.pid, "TERM");
    }
    let deadline = Instant::now() + limit;
    loop {
        let _ = process.poll();
        if !process.alive() {
            return false;
        }
        if Instant::now() >= deadline {
            eprintln!("[rbt] forcing shutdown after build shutdown timeout");
            force_kill_process(process);
            return true;
        }
        tokio::select! {
            _ = sigint.recv() => {
                let _ = process.poll();
                if process.alive() {
                    eprintln!("[rbt] forcing shutdown after second signal");
                    force_kill_process(process);
                    return true;
                }
                return false;
            }
            _ = sigterm.recv() => {
                let _ = process.poll();
                if process.alive() {
                    eprintln!("[rbt] forcing shutdown after second signal");
                    force_kill_process(process);
                    return true;
                }
                return false;
            }
            _ = sleep(Duration::from_millis(50)) => {}
        }
    }
}

async fn graceful_shutdown(
    processes: &mut [ManagedProcess],
    component_indices: &[usize],
    limit: Duration,
    sigint: &mut Signal,
    sigterm: &mut Signal,
) -> bool {
    for &index in component_indices {
        if processes[index].alive() {
            let _ = signal_group(processes[index].pid, "TERM");
        }
    }
    let deadline = Instant::now() + limit;
    let mut composer_signalled = false;
    loop {
        let _ = poll_all(processes);
        let components_done = component_indices
            .iter()
            .all(|&index| !processes[index].alive());
        if components_done && !composer_signalled {
            composer_signalled = true;
            if processes[0].alive() {
                let _ = signal_group(processes[0].pid, "TERM");
            }
        }
        if processes.iter().all(|process| !process.alive()) {
            return false;
        }
        if Instant::now() >= deadline {
            eprintln!("[rbt] forcing shutdown after shutdown timeout");
            force_kill_all(processes);
            return true;
        }
        tokio::select! {
            _ = sigint.recv() => {
                let _ = poll_all(processes);
                if processes.iter().any(ManagedProcess::alive) {
                    eprintln!("[rbt] forcing shutdown after second signal");
                    force_kill_all(processes);
                    return true;
                }
                return false;
            }
            _ = sigterm.recv() => {
                let _ = poll_all(processes);
                if processes.iter().any(ManagedProcess::alive) {
                    eprintln!("[rbt] forcing shutdown after second signal");
                    force_kill_all(processes);
                    return true;
                }
                return false;
            }
            _ = sleep(Duration::from_millis(50)) => {}
        }
    }
}

fn signal_group(pid: u32, signal_name: &str) -> io::Result<()> {
    let group = format!("-{pid}");
    let status = Command::new("kill")
        .arg(format!("-{signal_name}"))
        .arg("--")
        .arg(group)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()?;
    if status.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!(
            "kill -{signal_name} process group {pid} exited {status}"
        )))
    }
}

#[cfg(unix)]
fn process_group_exists(pgid: u32) -> bool {
    Command::new("kill")
        .args(["-0", "--", &format!("-{pgid}")])
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .is_ok_and(|status| status.success())
}

#[cfg(not(unix))]
const fn process_group_exists(_pgid: u32) -> bool {
    false
}

fn view_is_ready(view: &InspectionView, expected: &BTreeSet<String>) -> bool {
    expected.is_empty()
        || expected.iter().all(|name| {
            view.components.iter().any(|component| {
                component.snapshot.component_id.to_string() == *name
                    && matches!(component.snapshot.state, ComponentState::Active)
            })
        })
}

fn lifecycle_failure(view: &InspectionView, expected: &BTreeSet<String>) -> Option<String> {
    view.components.iter().find_map(|component| {
        let name = component.snapshot.component_id.to_string();
        if !expected.contains(&name) {
            return None;
        }
        match &component.snapshot.state {
            ComponentState::Failed(reason) => Some(format!("{name}: FAILED ({reason})")),
            _ => None,
        }
    })
}

fn emit_transitions(
    view: &InspectionView,
    states: &mut BTreeMap<String, String>,
    expected: &BTreeSet<String>,
) {
    let mut components = view.components.iter().collect::<Vec<_>>();
    components.sort_by_key(|component| component.snapshot.component_id.to_string());
    for component in components {
        let name = component.snapshot.component_id.to_string();
        if !expected.is_empty() && !expected.contains(&name) {
            continue;
        }
        let state = state_name(&component.snapshot.state).to_owned();
        if states.get(&name) != Some(&state) {
            if let Some(previous) = states.get(&name) {
                eprintln!("[rbt] {name}: {previous} -> {state}");
            } else {
                eprintln!("[rbt] {name}: {state}");
            }
            states.insert(name, state);
        }
    }
}

fn state_name(state: &ComponentState) -> &'static str {
    match state {
        ComponentState::Declared => "DECLARED",
        ComponentState::Preparing => "PREPARING",
        ComponentState::Blocked(_) => "BLOCKED",
        ComponentState::Activating => "ACTIVATING",
        ComponentState::Active => "ACTIVE",
        ComponentState::Quiescing => "QUIESCING",
        ComponentState::Failed(_) => "FAILED",
    }
}

fn display_status(status: ExitStatus) -> String {
    #[cfg(unix)]
    {
        use std::os::unix::process::ExitStatusExt;
        if let Some(signal) = status.signal() {
            return format!("signal {signal}");
        }
    }
    status.code().map_or_else(
        || "unknown status".to_owned(),
        |code| format!("exit {code}"),
    )
}

fn display_argv(argv: &[String]) -> String {
    argv.iter()
        .map(|argument| shell_quote(argument))
        .collect::<Vec<_>>()
        .join(" ")
}

fn shell_quote(value: &str) -> String {
    if value
        .bytes()
        .all(|byte| byte.is_ascii_alphanumeric() || b"_./:-".contains(&byte))
    {
        return value.to_owned();
    }
    format!("'{}'", value.replace('\'', "'\\''"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::project::{BuildSection, IdlSection, ProjectConfig, ProjectSection, RunSection};
    use std::fs;

    fn fixture_project(root: &Path) -> Project {
        Project {
            root: root.to_path_buf(),
            manifest_path: root.join("rbt.toml"),
            config: ProjectConfig {
                schema_version: 1,
                project: ProjectSection {
                    name: "fixture".to_owned(),
                    sdk_version: "0.5.0".to_owned(),
                },
                build: BuildSection {
                    command: vec!["true".to_owned()],
                },
                idl: IdlSection::default(),
                run: RunSection {
                    composer: Some(vec!["composer".to_owned(), "--fixture".to_owned()]),
                    composer_address: None,
                    startup_timeout_seconds: 1,
                    shutdown_timeout_seconds: 1,
                },
                components: Vec::new(),
            },
        }
    }

    #[test]
    fn argv_is_direct_and_preserves_spaces() {
        let root = PathBuf::from("/tmp/project with spaces");
        let project = fixture_project(&root);
        let component = ComponentConfig {
            name: "fixture".to_owned(),
            package: None,
            command: Some(vec![
                "/tmp/project with spaces/run fixture".to_owned(),
                "argument with spaces".to_owned(),
            ]),
            environment: BTreeMap::new(),
        };
        assert_eq!(
            component_argv(&project, &component).unwrap(),
            vec![
                "/tmp/project with spaces/run fixture".to_owned(),
                "argument with spaces".to_owned()
            ]
        );
        assert_eq!(
            display_argv(&["program".to_owned(), "argument with spaces".to_owned()]),
            "program 'argument with spaces'"
        );
    }

    #[test]
    fn invalid_preflight_is_refused_before_process_creation() {
        let report = ProjectCheckReport {
            project_root: PathBuf::from("/tmp/project"),
            manifest: PathBuf::from("/tmp/project/rbt.toml"),
            valid: false,
            diagnostics: vec![crate::project::ProjectDiagnostic {
                check_id: "manifest".to_owned(),
                status: crate::project::DiagnosticStatus::Fail,
                summary: "invalid manifest".to_owned(),
                details: Some("line 2".to_owned()),
                suggested_action: None,
                path: None,
                line: Some(2),
                column: Some(1),
            }],
        };
        let error = ensure_preflight(&report).unwrap_err();
        assert!(error.to_string().contains("invalid manifest"));
        assert!(error.to_string().contains("line 2"));
    }

    #[test]
    fn shell_quote_is_deterministic() {
        assert_eq!(shell_quote("simple/path"), "simple/path");
        assert_eq!(shell_quote("it's here"), "'it'\\''s here'");
    }

    #[test]
    #[cfg(target_os = "linux")]
    fn setsid_fixture_is_reaped_by_process_group_kill() {
        if path_executable("setsid").is_none() || path_executable("kill").is_none() {
            return;
        }
        let mut process = spawn_grouped(
            "fixture",
            &[
                "sh".to_owned(),
                "-c".to_owned(),
                "trap 'exit 0' USR1; sleep 30 & wait".to_owned(),
            ],
            Path::new("."),
            &BTreeMap::new(),
            DEFAULT_COMPOSER_ADDR,
            None,
        )
        .expect("setsid fixture should start");
        let pgid = (0..40).find_map(|_| {
            let pgid = process_group_id(process.pid);
            if pgid == Some(process.pid) {
                pgid
            } else {
                std::thread::sleep(Duration::from_millis(25));
                None
            }
        });
        let pgid = pgid.expect("fixture process should become its own process group");
        let direct_child = (0..40).find_map(|_| {
            let children = direct_children(process.pid);
            if children.is_empty() {
                std::thread::sleep(Duration::from_millis(25));
                None
            } else {
                Some(children[0])
            }
        });
        let direct_child = direct_child.expect("fixture should have a sleep direct child");
        assert_eq!(process_group_id(direct_child), Some(pgid));

        Command::new("kill")
            .args(["-USR1", &process.pid.to_string()])
            .status()
            .expect("leader signal should succeed");
        for _ in 0..40 {
            process.poll().expect("poll should succeed");
            if process.status.is_some() && process_group_exists(pgid) {
                break;
            }
            std::thread::sleep(Duration::from_millis(25));
        }
        assert!(
            process.status.is_some(),
            "fixture shell should exit after the direct signal"
        );
        assert!(
            process_group_exists(pgid),
            "sleep descendant should keep the exact process group alive"
        );

        force_kill_process(&mut process);
        for _ in 0..40 {
            process.poll().expect("poll should succeed");
            if !process.alive() && !process_group_exists(pgid) {
                break;
            }
            std::thread::sleep(Duration::from_millis(25));
        }
        assert!(!process.alive(), "fixture process must be reaped");
        assert!(
            !process_group_exists(pgid),
            "no process should remain in fixture process group {pgid}"
        );
    }

    #[cfg(target_os = "linux")]
    fn process_group_id(pid: u32) -> Option<u32> {
        let stat = fs::read_to_string(format!("/proc/{pid}/stat")).ok()?;
        let fields = stat
            .rsplit_once(')')?
            .1
            .split_whitespace()
            .collect::<Vec<_>>();
        fields.get(2)?.parse().ok()
    }

    #[cfg(target_os = "linux")]
    fn direct_children(pid: u32) -> Vec<u32> {
        fs::read_to_string(format!("/proc/{pid}/task/{pid}/children"))
            .unwrap_or_default()
            .split_whitespace()
            .filter_map(|child| child.parse().ok())
            .collect()
    }
}

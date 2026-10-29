//! Read-only host and project diagnostics for the `rbt doctor` command.
//!
//! Doctor deliberately does not repair anything.  It only inspects the local
//! executable/toolchain environment, project metadata, shared-memory mount
//! points, and (on Linux) process names under `/proc`.  Runtime lifecycle
//! truth remains the composer's responsibility.

use crate::project::{DiagnosticStatus, PROJECT_SCHEMA_VERSION, Project, ProjectCheckReport};
use serde::{Deserialize, Serialize};
use std::{
    collections::BTreeSet,
    env,
    fs::{self, OpenOptions},
    io::Write,
    path::{Path, PathBuf},
    process::Command,
    sync::atomic::{AtomicU64, Ordering},
};

/// The platform supported by this milestone's local doctor checks.
pub const SUPPORTED_PLATFORM: &str = "linux-x86_64";

/// Explicit paths and optional project context used by [`run_doctor`].
///
/// Every field is optional so the CLI can use normal environment/PATH
/// discovery, while tests and embedders can make executable resolution
/// deterministic without changing the process environment.
#[derive(Clone, Debug, Default)]
pub struct DoctorContext {
    /// A project already loaded by the CLI, if one was discovered.
    pub project: Option<Project>,
    /// Results from the project's full static checker, when the caller has
    /// already run it.  These are copied into the doctor report verbatim.
    pub project_check: Option<ProjectCheckReport>,
    /// The currently executing `rbt` binary.  Defaults to `current_exe()`.
    pub current_executable: Option<PathBuf>,
    /// An explicitly selected Composer executable.
    pub composer_executable: Option<PathBuf>,
    /// An explicitly selected `rbt-idl` executable.
    pub rbt_idl_executable: Option<PathBuf>,
    /// An explicitly selected installed Rust SDK root.
    pub rust_sdk: Option<PathBuf>,
}

impl DoctorContext {
    /// Build a context from the current process and an optional loaded project.
    pub fn current(project: Option<Project>) -> Self {
        Self {
            project,
            current_executable: env::current_exe().ok(),
            ..Self::default()
        }
    }
}

/// One stable, machine-readable doctor result.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct DoctorDiagnostic {
    /// Stable identifier intended for scripts and editor integrations.
    pub check_id: String,
    /// PASS, WARN, FAIL, or SKIP; prose is not part of the interface.
    pub status: DiagnosticStatus,
    /// Short human-facing summary.
    pub summary: String,
    /// Optional evidence or context.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub details: Option<String>,
    /// Optional next action.  Doctor never performs this action itself.
    #[serde(skip_serializing_if = "Option::is_none")]
    pub suggested_action: Option<String>,
}

/// Complete doctor result.  `success` is derived solely from FAIL entries.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct DoctorReport {
    /// Explicitly records the platform scope of these checks.
    pub platform: String,
    pub checks: Vec<DoctorDiagnostic>,
    pub success: bool,
}

impl DoctorReport {
    /// Construct a report while deriving the overall result from diagnostics.
    pub fn new(checks: Vec<DoctorDiagnostic>) -> Self {
        let success = checks
            .iter()
            .all(|check| check.status != DiagnosticStatus::Fail);
        Self {
            platform: platform_label(),
            checks,
            success,
        }
    }

    /// Return true when at least one diagnostic is a hard failure.
    pub fn is_success(&self) -> bool {
        self.success
    }
}

/// Run all read-only environment and project checks.
pub fn run_doctor(context: DoctorContext) -> DoctorReport {
    let mut checks = Vec::new();
    checks.push(check_platform());

    let current_executable = context
        .current_executable
        .clone()
        .or_else(|| env::current_exe().ok());
    checks.push(check_rbt_cli(current_executable.as_deref()));

    let composer = resolve_tool(
        context.composer_executable.as_deref(),
        "composer",
        current_executable.as_deref(),
        context.project.as_ref(),
    );
    checks.push(check_tool(
        "composer",
        "composer_missing",
        "composer_version_mismatch",
        composer.as_deref(),
        true,
    ));

    let rbt_idl = resolve_tool(
        context.rbt_idl_executable.as_deref(),
        "rbt-idl",
        current_executable.as_deref(),
        None,
    );
    checks.push(check_tool(
        "rbt-idl",
        "rbt_idl_missing",
        "rbt_idl_version_mismatch",
        rbt_idl.as_deref(),
        true,
    ));

    let sdk_root = resolve_sdk(context.rust_sdk.as_deref(), current_executable.as_deref());
    checks.push(check_sdk(sdk_root.as_deref(), context.project.as_ref()));
    checks.push(check_rust_toolchain());
    checks.push(check_writable_directory(
        Path::new("/dev/shm"),
        "shm",
        "shm_unavailable",
        "/dev/shm",
        true,
    ));
    checks.push(check_writable_directory(
        Path::new("/tmp"),
        "tmp",
        "tmp_unavailable",
        "/tmp",
        true,
    ));

    checks.push(check_cpp_toolchain(context.project.as_ref()));
    checks.push(check_python_toolchain(context.project.as_ref()));
    let mut project_checks =
        check_project_diagnostics(context.project.as_ref(), context.project_check.as_ref());
    if let (Some(project), None) = (context.project.as_ref(), context.project_check.as_ref()) {
        match crate::project::check_project(project) {
            Ok(report) => {
                project_checks.extend(report.diagnostics.iter().map(from_project_diagnostic))
            }
            Err(error) => project_checks.push(fail(
                "project_check_failed",
                "project static check could not complete",
                error.to_string(),
                "fix the project manifest or IDL sources, then run `rbt check`",
            )),
        }
    }
    checks.extend(project_checks);
    checks.push(check_stale_processes());

    DoctorReport::new(checks)
}

/// Render a report for a terminal without hiding machine-readable status.
pub fn format_doctor(report: &DoctorReport) -> String {
    let mut output = String::new();
    for check in &report.checks {
        output.push_str(status_name(check.status));
        output.push(' ');
        output.push_str(&check.summary);
        output.push('\n');
        if let Some(details) = &check.details {
            for line in details.lines() {
                output.push_str("  ");
                output.push_str(line);
                output.push('\n');
            }
        }
        if let Some(action) = &check.suggested_action {
            output.push_str("  action: ");
            output.push_str(action);
            output.push('\n');
        }
    }
    output.push_str(if report.success {
        "doctor: ready\n"
    } else {
        "doctor: blocked by failed checks\n"
    });
    output
}

/// Render a stable JSON report for `rbt doctor --json`.
pub fn format_doctor_json(report: &DoctorReport) -> Result<String, serde_json::Error> {
    serde_json::to_string_pretty(report)
}

fn platform_label() -> String {
    if cfg!(all(target_os = "linux", target_arch = "x86_64")) {
        SUPPORTED_PLATFORM.to_owned()
    } else {
        format!("{}-{}", env::consts::OS, env::consts::ARCH)
    }
}

fn check_platform() -> DoctorDiagnostic {
    if cfg!(all(target_os = "linux", target_arch = "x86_64")) {
        pass("platform", "Linux x86_64 supported")
    } else {
        fail(
            "unsupported_platform",
            "unsupported platform",
            format!(
                "rbt doctor currently supports {SUPPORTED_PLATFORM}; detected {}",
                platform_label()
            ),
            "run doctor on a Linux x86_64 host",
        )
    }
}

fn check_rbt_cli(path: Option<&Path>) -> DoctorDiagnostic {
    let Some(path) = path else {
        return fail(
            "rbt_cli_missing",
            "rbt CLI executable not found",
            "the current executable path could not be determined",
            "invoke doctor through the installed `rbt` executable",
        );
    };
    if !is_executable(path) {
        return fail(
            "rbt_cli_missing",
            "rbt CLI executable not found",
            format!("{} is missing or not executable", path.display()),
            "reinstall the rbt CLI or pass the installed executable",
        );
    }

    // This is the current process in the normal CLI path.  Reading the
    // compile-time package version avoids recursively starting ourselves.
    if env::current_exe().ok().as_deref() == Some(path) {
        return pass(
            "rbt_cli",
            format!("rbt CLI available ({})", env!("CARGO_PKG_VERSION")),
        );
    }
    match probe_version(path) {
        Ok(Some(version)) if version == env!("CARGO_PKG_VERSION") => {
            pass("rbt_cli", format!("rbt CLI available ({version})"))
        }
        Ok(Some(version)) => fail(
            "rbt_cli_version_mismatch",
            "rbt CLI version mismatch",
            format!(
                "{} reports {version}; doctor expects {}",
                path.display(),
                env!("CARGO_PKG_VERSION")
            ),
            "use rbt and its installed tools from the same SDK prefix",
        ),
        Ok(None) => warn(
            "rbt_cli_version_unknown",
            "rbt CLI executable found",
            format!("{} did not report a parseable version", path.display()),
            "reinstall the matching rbt CLI if version checks remain unavailable",
        ),
        Err(error) => fail(
            "rbt_cli_unavailable",
            "rbt CLI executable could not be queried",
            error,
            "verify executable permissions and the installed rbt CLI",
        ),
    }
}

fn check_tool(
    label: &str,
    missing_id: &str,
    mismatch_id: &str,
    path: Option<&Path>,
    required: bool,
) -> DoctorDiagnostic {
    let Some(path) = path else {
        return if required {
            fail(
                missing_id,
                format!("{label} executable not found"),
                format!("no `{label}` executable was found beside rbt or on PATH"),
                format!("install `{label}` and ensure it is on PATH"),
            )
        } else {
            skip(
                missing_id,
                format!("{label} check skipped"),
                format!("no {label} executable was selected"),
            )
        };
    };
    if !is_executable(path) {
        return fail(
            missing_id,
            format!("{label} executable is not runnable"),
            format!(
                "{} is missing or lacks executable permission",
                path.display()
            ),
            format!("make `{label}` executable or select another installation"),
        );
    }
    match probe_version(path) {
        Ok(Some(version)) if version == env!("CARGO_PKG_VERSION") => pass(
            format!("{label}_tool"),
            format!("{label} executable available ({version})"),
        ),
        Ok(Some(version)) => fail(
            mismatch_id,
            format!("{label} version mismatch"),
            format!(
                "{} reports {version}; doctor expects {}",
                path.display(),
                env!("CARGO_PKG_VERSION")
            ),
            "use rbt tools installed from the same SDK prefix",
        ),
        Ok(None) => warn(
            format!("{label}_version_unknown"),
            format!("{label} executable found"),
            format!("{} did not report a parseable version", path.display()),
            format!("install a matching {label} release if compatibility is uncertain"),
        ),
        Err(error) => fail(
            format!("{label}_unavailable"),
            format!("{label} executable could not be queried"),
            error,
            format!("verify the {label} executable and its permissions"),
        ),
    }
}

fn check_sdk(path: Option<&Path>, project: Option<&Project>) -> DoctorDiagnostic {
    let Some(path) = path else {
        return fail(
            "rust_sdk_missing",
            "Rust SDK not found",
            "no SDK was found via RBT_RUST_SDK or the rbt installation prefix",
            "set RBT_RUST_SDK to an installed share/rbt/rust directory",
        );
    };
    let rbt_manifest = path.join("crates/rbt/Cargo.toml");
    let types_manifest = path.join("crates/rbt-types/Cargo.toml");
    if !rbt_manifest.is_file() || !types_manifest.is_file() {
        return fail(
            "rust_sdk_invalid",
            "Rust SDK installation is incomplete",
            format!(
                "expected {} and {}",
                rbt_manifest.display(),
                types_manifest.display()
            ),
            "reinstall the Rust SDK and point RBT_RUST_SDK at its root",
        );
    }
    if let Some(project) = project {
        let expected = project.config.project.sdk_version.trim();
        if !expected.is_empty() {
            match package_workspace_version(path) {
                Some(version) if version != expected => {
                    return fail(
                        "rust_sdk_version_mismatch",
                        "Rust SDK version mismatch",
                        format!("project requires {expected}; installed SDK reports {version}"),
                        "use an SDK matching project.sdk_version",
                    );
                }
                None => {
                    return warn(
                        "rust_sdk_version_unknown",
                        "Rust SDK available",
                        format!(
                            "{} has no parseable workspace package version",
                            path.display()
                        ),
                        "verify that the installed SDK matches project.sdk_version",
                    );
                }
                Some(_) => {}
            }
        }
    }
    pass(
        "rust_sdk",
        format!("Rust SDK available ({})", path.display()),
    )
}

fn check_rust_toolchain() -> DoctorDiagnostic {
    let rustc = resolve_program("rustc");
    let cargo = resolve_program("cargo");
    match (rustc, cargo) {
        (Some(rustc), Some(cargo)) => {
            let rustc_version = probe_version(&rustc).ok().flatten();
            let details = match rustc_version {
                Some(version) => format!("rustc {version}; cargo at {}", cargo.display()),
                None => format!("rustc at {}; cargo at {}", rustc.display(), cargo.display()),
            };
            pass("rust_toolchain", "Rust toolchain available").with_details(details)
        }
        (None, None) => fail(
            "rust_missing",
            "Rust toolchain not found",
            "neither rustc nor cargo was found on PATH",
            "install Rust and ensure rustc and cargo are on PATH",
        ),
        (None, Some(cargo)) => fail(
            "rust_missing",
            "Rust compiler not found",
            format!("cargo was found at {}, but rustc was not", cargo.display()),
            "install the Rust compiler (rustc)",
        ),
        (Some(rustc), None) => fail(
            "rust_missing",
            "Cargo not found",
            format!("rustc was found at {}, but cargo was not", rustc.display()),
            "install the complete Rust toolchain including cargo",
        ),
    }
}

fn check_writable_directory(
    path: &Path,
    pass_id: &str,
    unavailable_id: &str,
    label: &str,
    required: bool,
) -> DoctorDiagnostic {
    if !path.is_dir() {
        return if required {
            fail(
                unavailable_id,
                format!("{label} unavailable"),
                format!("{label} does not exist or is not a directory"),
                format!("mount or create a writable {label} before running rbt"),
            )
        } else {
            skip(
                unavailable_id,
                format!("{label} check skipped"),
                "directory not present",
            )
        };
    }
    static TEMP_COUNTER: AtomicU64 = AtomicU64::new(0);
    let nonce = TEMP_COUNTER.fetch_add(1, Ordering::Relaxed);
    let name = format!(
        ".rbt-doctor-{}-{}-{}",
        std::process::id(),
        nonce,
        unique_time_nonce()
    );
    let probe = path.join(name);
    let result = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(&probe)
        .and_then(|mut file| file.write_all(b"rbt-doctor"));
    let cleanup = fs::remove_file(&probe);
    match (result, cleanup) {
        (Ok(()), Ok(())) => pass(pass_id, format!("{label} writable")),
        (Err(error), Ok(())) => fail(
            unavailable_id,
            format!("{label} is not writable"),
            error.to_string(),
            format!("ensure the current user can write to {label}"),
        ),
        (Ok(()), Err(error)) => warn(
            format!("{pass_id}_cleanup"),
            format!("{label} writable"),
            format!("doctor could not remove its temporary probe: {error}"),
            format!(
                "remove the doctor probe {} after verifying ownership",
                probe.display()
            ),
        ),
        (Err(error), Err(cleanup_error)) => fail(
            unavailable_id,
            format!("{label} is not writable"),
            format!("write failed: {error}; cleanup failed: {cleanup_error}"),
            format!("ensure the current user can write to {label}"),
        ),
    }
}

fn check_cpp_toolchain(project: Option<&Project>) -> DoctorDiagnostic {
    if !project_requires_language(project, Language::Cpp) {
        return skip(
            "cpp_toolchain",
            "C++ checks skipped",
            "no C++ component was detected in the project",
        );
    }
    let compiler = resolve_program("c++").or_else(|| resolve_program("g++"));
    let cmake = resolve_program("cmake");
    if compiler.is_some() && cmake.is_some() {
        pass("cpp_toolchain", "C++ compiler and CMake available")
    } else {
        let mut missing = Vec::new();
        if compiler.is_none() {
            missing.push("C++ compiler");
        }
        if cmake.is_none() {
            missing.push("CMake");
        }
        warn(
            "cpp_missing",
            "C++ toolchain incomplete",
            format!("missing {}", missing.join(" and ")),
            "install the missing C++ build tools before building C++ components",
        )
    }
}

fn check_python_toolchain(project: Option<&Project>) -> DoctorDiagnostic {
    if !project_requires_language(project, Language::Python) {
        return skip(
            "python_toolchain",
            "Python checks skipped",
            "no Python component was detected in the project",
        );
    }
    let Some(python) = resolve_program("python3").or_else(|| resolve_program("python")) else {
        return warn(
            "python_missing",
            "Python executable not found",
            "a Python component was detected but python3/python is not on PATH",
            "install Python before building Python components",
        );
    };
    let numpy = Command::new(&python)
        .args(["-c", "import numpy"])
        .output()
        .map(|output| output.status.success())
        .unwrap_or(false);
    if numpy {
        pass("python_toolchain", "Python and NumPy available")
    } else {
        warn(
            "python_numpy_missing",
            "Python available but NumPy is missing",
            format!("{} could not import NumPy", python.display()),
            "install NumPy in the selected Python environment",
        )
    }
}

fn check_project_diagnostics(
    project: Option<&Project>,
    full_report: Option<&ProjectCheckReport>,
) -> Vec<DoctorDiagnostic> {
    let Some(project) = project else {
        return vec![skip(
            "project",
            "project checks skipped",
            "no rbt.toml was selected or discovered",
        )];
    };
    let mut checks = vec![pass(
        "project_manifest",
        format!(
            "project manifest found ({})",
            project.manifest_path.display()
        ),
    )];
    if project.config.schema_version != PROJECT_SCHEMA_VERSION {
        checks.push(fail(
            "project_schema_version",
            "project schema version is unsupported",
            format!(
                "{} declares {}; doctor expects {}",
                project.manifest_path.display(),
                project.config.schema_version,
                PROJECT_SCHEMA_VERSION
            ),
            "regenerate or update the project manifest with the matching rbt CLI",
        ));
    } else {
        checks.push(pass(
            "project_schema_version",
            "project schema version supported",
        ));
    }

    let mut names = BTreeSet::new();
    let mut duplicate_names = BTreeSet::new();
    for component in &project.config.components {
        if !names.insert(component.name.clone()) {
            duplicate_names.insert(component.name.clone());
        }
    }
    if duplicate_names.is_empty() {
        checks.push(pass(
            "project_components",
            "project component declarations are unique",
        ));
    } else {
        checks.push(fail(
            "duplicate_component",
            "duplicate component names in project",
            duplicate_names.into_iter().collect::<Vec<_>>().join(", "),
            "rename each component so names are unique",
        ));
    }

    let mut idl_files = Vec::new();
    for source in &project.config.idl.sources {
        let root = project.root.join(source);
        if !root.exists() {
            checks.push(fail(
                "project_idl_missing",
                "IDL source directory is missing",
                root.display().to_string(),
                "create the configured IDL source directory or update rbt.toml",
            ));
            continue;
        }
        collect_extension_files(&root, "rbt", &mut idl_files);
    }
    if idl_files.is_empty() {
        checks.push(warn(
            "project_idl_empty",
            "project contains no IDL files",
            "no .rbt files were found under configured IDL sources",
            "add a message with `rbt new message <Name>` or configure idl.sources",
        ));
    } else {
        let marker = project.root.join("generated/generation-manifest.json");
        if marker.is_file() {
            checks.push(pass(
                "idl_generated",
                format!("generated IDL manifest found ({})", marker.display()),
            ));
        } else {
            checks.push(warn(
                "idl_stale",
                "generated IDL artifacts are missing",
                format!(
                    "{} source file(s) have no generation manifest",
                    idl_files.len()
                ),
                "run `rbt idl generate`",
            ));
        }
    }

    if let Some(report) = full_report {
        checks.extend(report.diagnostics.iter().map(from_project_diagnostic));
    }
    checks
}

fn from_project_diagnostic(diagnostic: &crate::project::ProjectDiagnostic) -> DoctorDiagnostic {
    DoctorDiagnostic {
        check_id: diagnostic.check_id.clone(),
        status: diagnostic.status,
        summary: diagnostic.summary.clone(),
        details: diagnostic.details.clone(),
        suggested_action: diagnostic.suggested_action.clone(),
    }
}

fn check_stale_processes() -> DoctorDiagnostic {
    #[cfg(not(target_os = "linux"))]
    {
        return skip(
            "stale_processes",
            "stale-process check skipped",
            "this check is only available on Linux",
        );
    }

    #[cfg(target_os = "linux")]
    {
        let proc_root = Path::new("/proc");
        let Ok(entries) = fs::read_dir(proc_root) else {
            return skip(
                "stale_processes",
                "stale-process check skipped",
                "/proc is unavailable or unreadable",
            );
        };
        let current_pid = std::process::id();
        let mut found = Vec::new();
        for entry in entries.flatten() {
            let name = entry.file_name();
            let Ok(pid) = name.to_string_lossy().parse::<u32>() else {
                continue;
            };
            if pid == current_pid {
                continue;
            }
            let cmdline_path = entry.path().join("cmdline");
            let Ok(bytes) = fs::read(cmdline_path) else {
                continue;
            };
            let args = bytes
                .split(|byte| *byte == 0)
                .filter(|part| !part.is_empty())
                .map(|part| String::from_utf8_lossy(part).into_owned())
                .collect::<Vec<_>>();
            let Some(program) = args.first() else {
                continue;
            };
            let basename = Path::new(program)
                .file_name()
                .and_then(|value| value.to_str())
                .unwrap_or(program);
            if matches!(basename, "rbt" | "composer" | "rbt-idl") || basename.starts_with("rbt-") {
                found.push(format!("pid={pid} {program}"));
            }
        }
        if found.is_empty() {
            pass("stale_processes", "no other rbt-related processes detected")
        } else {
            found.sort();
            warn(
                "stale_processes",
                "possible stale rbt-related processes detected",
                found.into_iter().take(16).collect::<Vec<_>>().join("\n"),
                "inspect these processes and stop them manually only after verifying ownership",
            )
        }
    }
}

#[derive(Clone, Copy)]
enum Language {
    Cpp,
    Python,
}

fn project_requires_language(project: Option<&Project>, language: Language) -> bool {
    let Some(project) = project else {
        return false;
    };
    let components_root = project.root.join("components");
    let extensions: &[&str] = match language {
        Language::Cpp => &["cpp", "cc", "cxx", "hpp", "hh", "hxx", "cmake"],
        Language::Python => &["py"],
    };
    if has_extension_file(&components_root, extensions) {
        return true;
    }
    project.config.components.iter().any(|component| {
        component.command.as_ref().is_some_and(|command| {
            command.first().is_some_and(|program| {
                let lower = program.to_ascii_lowercase();
                match language {
                    Language::Cpp => lower.contains("cmake") || lower.ends_with("g++"),
                    Language::Python => lower.contains("python") || lower.ends_with(".py"),
                }
            })
        })
    })
}

fn has_extension_file(root: &Path, extensions: &[&str]) -> bool {
    let Ok(entries) = fs::read_dir(root) else {
        return false;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let name = entry.file_name();
        if name == ".git" || name == "target" || name == "generated" {
            continue;
        }
        if path.is_dir() {
            if has_extension_file(&path, extensions) {
                return true;
            }
        } else if path
            .extension()
            .and_then(|value| value.to_str())
            .is_some_and(|extension| extensions.contains(&extension))
        {
            return true;
        }
    }
    false
}

fn collect_extension_files(root: &Path, extension: &str, files: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(root) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let name = entry.file_name();
        if name == ".git" || name == "target" || name == "generated" {
            continue;
        }
        if path.is_dir() {
            collect_extension_files(&path, extension, files);
        } else if path
            .extension()
            .and_then(|value| value.to_str())
            .is_some_and(|value| value == extension)
        {
            files.push(path);
        }
    }
    files.sort();
}

fn resolve_sdk(explicit: Option<&Path>, current_executable: Option<&Path>) -> Option<PathBuf> {
    if let Some(path) = explicit {
        return Some(path.to_path_buf());
    }
    if let Some(path) = env::var_os("RBT_RUST_SDK") {
        return Some(PathBuf::from(path));
    }
    let binary = current_executable?;
    let bin_dir = binary.parent()?;
    let prefix = bin_dir.parent()?;
    Some(prefix.join("share/rbt/rust"))
}

fn resolve_tool(
    explicit: Option<&Path>,
    name: &str,
    current_executable: Option<&Path>,
    project: Option<&Project>,
) -> Option<PathBuf> {
    if let Some(path) = explicit {
        return Some(path.to_path_buf());
    }
    if let Some(project) = project
        && name == "composer"
        && let Some(command) = project.config.run.composer.as_ref()
        && let Some(program) = command.first()
    {
        let configured = Path::new(program);
        let resolved = if configured.is_absolute() {
            configured.to_path_buf()
        } else if program.contains(std::path::MAIN_SEPARATOR) {
            project.root.join(configured)
        } else {
            PathBuf::from(program)
        };
        if let Some(path) = resolve_program_path(&resolved.to_string_lossy()) {
            return Some(path);
        }
    }
    if let Some(binary) = current_executable
        && let Some(parent) = binary.parent()
    {
        let sibling = parent.join(name);
        if is_executable(&sibling) {
            return Some(sibling);
        }
    }
    resolve_program(name)
}

fn resolve_program(name: &str) -> Option<PathBuf> {
    resolve_program_path(name)
}

fn resolve_program_path(program: &str) -> Option<PathBuf> {
    let candidate = Path::new(program);
    if candidate.is_absolute() || program.contains(std::path::MAIN_SEPARATOR) {
        return is_executable(candidate).then(|| candidate.to_path_buf());
    }
    let path = env::var_os("PATH")?;
    for directory in env::split_paths(&path) {
        let candidate = directory.join(program);
        if is_executable(&candidate) {
            return Some(candidate);
        }
    }
    None
}

fn is_executable(path: &Path) -> bool {
    if !path.is_file() {
        return false;
    }
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        path.metadata()
            .map(|metadata| metadata.permissions().mode() & 0o111 != 0)
            .unwrap_or(false)
    }
    #[cfg(not(unix))]
    {
        true
    }
}

fn probe_version(path: &Path) -> Result<Option<String>, String> {
    let output = Command::new(path)
        .arg("--version")
        .output()
        .map_err(|error| format!("{}: {error}", path.display()))?;
    let mut text = String::from_utf8_lossy(&output.stdout).into_owned();
    if text.trim().is_empty() {
        text = String::from_utf8_lossy(&output.stderr).into_owned();
    }
    if !output.status.success() && text.trim().is_empty() {
        return Err(format!("{} exited with {}", path.display(), output.status));
    }
    Ok(extract_version(&text))
}

fn extract_version(text: &str) -> Option<String> {
    text.split_whitespace().find_map(|token| {
        let token = token.trim_matches(|character: char| {
            !character.is_ascii_digit() && character != '.' && character != '-'
        });
        let mut parts = token.splitn(3, '.');
        let major = parts.next()?;
        let minor = parts.next()?;
        let patch = parts.next()?;
        if major.chars().all(|value| value.is_ascii_digit())
            && minor.chars().all(|value| value.is_ascii_digit())
            && patch
                .chars()
                .next()
                .is_some_and(|value| value.is_ascii_digit())
        {
            Some(token.to_owned())
        } else {
            None
        }
    })
}

fn package_workspace_version(root: &Path) -> Option<String> {
    let source = fs::read_to_string(root.join("Cargo.toml")).ok()?;
    let mut in_workspace_package = false;
    for line in source.lines() {
        let line = line.trim();
        if line.starts_with('[') {
            in_workspace_package = line == "[workspace.package]";
            continue;
        }
        if in_workspace_package
            && let Some(value) = line.strip_prefix("version")
            && value.trim_start().starts_with('=')
        {
            let value = line.split_once('=')?.1.trim();
            return Some(value.trim_matches(['"', '\'']).to_owned());
        }
    }
    None
}

fn unique_time_nonce() -> u128 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|duration| duration.as_nanos())
        .unwrap_or_default()
}

fn status_name(status: DiagnosticStatus) -> &'static str {
    match status {
        DiagnosticStatus::Pass => "PASS",
        DiagnosticStatus::Warn => "WARN",
        DiagnosticStatus::Fail => "FAIL",
        DiagnosticStatus::Skip => "SKIP",
    }
}

impl DoctorDiagnostic {
    fn with_details(mut self, details: impl Into<String>) -> Self {
        self.details = Some(details.into());
        self
    }
}

fn pass(check_id: impl Into<String>, summary: impl Into<String>) -> DoctorDiagnostic {
    DoctorDiagnostic {
        check_id: check_id.into(),
        status: DiagnosticStatus::Pass,
        summary: summary.into(),
        details: None,
        suggested_action: None,
    }
}

fn warn(
    check_id: impl Into<String>,
    summary: impl Into<String>,
    details: impl Into<String>,
    action: impl Into<String>,
) -> DoctorDiagnostic {
    DoctorDiagnostic {
        check_id: check_id.into(),
        status: DiagnosticStatus::Warn,
        summary: summary.into(),
        details: Some(details.into()),
        suggested_action: Some(action.into()),
    }
}

fn fail(
    check_id: impl Into<String>,
    summary: impl Into<String>,
    details: impl Into<String>,
    action: impl Into<String>,
) -> DoctorDiagnostic {
    DoctorDiagnostic {
        check_id: check_id.into(),
        status: DiagnosticStatus::Fail,
        summary: summary.into(),
        details: Some(details.into()),
        suggested_action: Some(action.into()),
    }
}

fn skip(
    check_id: impl Into<String>,
    summary: impl Into<String>,
    details: impl Into<String>,
) -> DoctorDiagnostic {
    DoctorDiagnostic {
        check_id: check_id.into(),
        status: DiagnosticStatus::Skip,
        summary: summary.into(),
        details: Some(details.into()),
        suggested_action: None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    #[test]
    fn report_success_is_derived_only_from_failures() {
        let report = DoctorReport::new(vec![
            pass("one", "ok"),
            warn("two", "warning", "details", "action"),
            skip("three", "skipped", "not applicable"),
        ]);
        assert!(report.success);
        let report = DoctorReport::new(vec![fail("broken", "bad", "details", "fix")]);
        assert!(!report.success);
    }

    #[test]
    fn formatter_includes_stable_status_and_action() {
        let report = DoctorReport::new(vec![warn(
            "shm_unavailable",
            "shared memory unavailable",
            "permission denied",
            "mount /dev/shm",
        )]);
        let text = format_doctor(&report);
        assert!(text.contains("WARN shared memory unavailable"));
        assert!(text.contains("action: mount /dev/shm"));
        let json = format_doctor_json(&report).expect("json");
        assert!(json.contains("shm_unavailable"));
        assert!(json.contains("\"success\": true"));
    }

    #[test]
    fn version_parser_accepts_tool_prefixes_and_prereleases() {
        assert_eq!(extract_version("rbt 0.5.0"), Some("0.5.0".to_owned()));
        assert_eq!(
            extract_version("rbt-idl 0.5.0-beta.1"),
            Some("0.5.0-beta.1".to_owned())
        );
        assert_eq!(extract_version("unknown"), None);
    }

    #[test]
    fn sdk_resolution_prefers_explicit_then_environment_then_prefix() {
        let root = unique_test_path("doctor-sdk");
        let bin = root.join("bin");
        fs::create_dir_all(&bin).expect("bin");
        let executable = bin.join("rbt");
        fs::write(&executable, b"placeholder").expect("executable");
        let explicit = PathBuf::from("/explicit/sdk");
        assert_eq!(
            resolve_sdk(Some(&explicit), Some(&executable)),
            Some(explicit)
        );
        if env::var_os("RBT_RUST_SDK").is_none() {
            assert_eq!(
                resolve_sdk(None, Some(&executable)),
                Some(root.join("share/rbt/rust"))
            );
        }
        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn writable_probe_cleans_up_after_itself() {
        let root = unique_test_path("doctor-writable");
        fs::create_dir_all(&root).expect("directory");
        let diagnostic =
            check_writable_directory(&root, "tmp", "tmp_unavailable", "test temp", true);
        assert_eq!(diagnostic.status, DiagnosticStatus::Pass);
        assert_eq!(diagnostic.check_id, "tmp");
        let entries = fs::read_dir(&root)
            .expect("read directory")
            .collect::<Result<Vec<_>, _>>()
            .expect("entries");
        assert!(entries.is_empty(), "temporary probe was not cleaned up");
        let _ = fs::remove_dir_all(root);
    }

    fn unique_test_path(prefix: &str) -> PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("clock")
            .as_nanos();
        env::temp_dir().join(format!("{prefix}-{}-{nonce}", std::process::id()))
    }
}

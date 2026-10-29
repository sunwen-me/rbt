use std::{env, error::Error, fmt};

use rbt_cli::{
    CliError, Command, DEFAULT_COMPOSER_ADDR, Invocation, RecordOptions, command_help,
    doctor::{
        DoctorContext, DoctorDiagnostic, DoctorReport, format_doctor, format_doctor_json,
        run_doctor,
    },
    fetch_view, format_capability_list_filtered, format_capability_list_filtered_verbose,
    format_capability_list_json, format_graph, format_graph_json, format_json, format_mcap_info,
    format_ps, format_ps_json, format_rpc_info, format_rpc_info_json, format_stream_info,
    format_stream_info_verbose, format_stream_pressure, format_why_json, format_why_with_options,
    idl_command, parse_invocation,
    project::{
        DiagnosticStatus, Project, ProjectError, add_component, add_message, check_project,
        create_project, discover_project, format_check_report, format_project_idl,
        generate_project, load_project, project_idl_files,
    },
    run_play, run_record,
    runner::{RunOptions, RunnerError, run_project},
    version::format_version,
};

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u8)]
enum ExitClass {
    Success = 0,
    Operational = 1,
    Usage = 2,
    Validation = 3,
    Unavailable = 4,
}

struct AppError {
    class: ExitClass,
    source: Box<dyn Error + Send + Sync>,
}

impl AppError {
    fn new<E>(class: ExitClass, source: E) -> Self
    where
        E: Error + Send + Sync + 'static,
    {
        Self {
            class,
            source: Box::new(source),
        }
    }

    fn message(class: ExitClass, message: impl Into<String>) -> Self {
        Self::new(class, MessageError(message.into()))
    }
}

impl fmt::Display for AppError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        fmt::Display::fmt(&self.source, formatter)
    }
}

impl fmt::Debug for AppError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("AppError")
            .field("class", &self.class)
            .field("source", &self.source.to_string())
            .finish()
    }
}

impl Error for AppError {
    fn source(&self) -> Option<&(dyn Error + 'static)> {
        Some(self.source.as_ref())
    }
}

#[derive(Debug)]
struct MessageError(String);

impl fmt::Display for MessageError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str(&self.0)
    }
}

impl Error for MessageError {}

#[tokio::main]
async fn main() {
    let arguments = env::args().collect::<Vec<_>>();
    let verbose = arguments.iter().any(|argument| argument == "--verbose");
    match run(arguments).await {
        Ok(ExitClass::Success) => {}
        Ok(class) => std::process::exit(class as i32),
        Err(error) => {
            eprintln!("rbt: {error}");
            if verbose {
                let mut cause = error.source.source();
                while let Some(next) = cause {
                    eprintln!("  caused by: {next}");
                    cause = next.source();
                }
            }
            std::process::exit(error.class as i32);
        }
    }
}

async fn run(args: Vec<String>) -> Result<ExitClass, AppError> {
    let invocation = parse_invocation(args).map_err(map_cli_error)?;
    dispatch(invocation).await
}

async fn dispatch(invocation: Invocation) -> Result<ExitClass, AppError> {
    let Invocation {
        command,
        project: project_override,
        composer_address: composer_override,
        quiet,
        verbose_errors,
    } = invocation;
    match command {
        Command::Help { topic } => println!("{}", command_help(topic.as_deref())),
        Command::Version { verbose } => print!("{}", format_version(verbose)),
        Command::TimeStatus { json, sources } => {
            let report = rbt_cli::time_status::run_time_status(sources.as_deref())
                .map_err(|error| AppError::new(ExitClass::Validation, error))?;
            if json {
                println!(
                    "{}",
                    serde_json::to_string_pretty(&report).map_err(map_json_error)?
                );
            } else if !quiet {
                print!("{}", rbt_cli::time_status::format_time_status(&report));
            }
            if !report.success {
                return Ok(ExitClass::Unavailable);
            }
        }
        Command::TfStatus { json, snapshot } => {
            return dispatch_tf(
                json,
                snapshot,
                false,
                project_override.as_deref(),
                composer_override.as_deref(),
                quiet,
            )
            .await;
        }
        Command::TfTree { json, snapshot } => {
            return dispatch_tf(
                json,
                snapshot,
                true,
                project_override.as_deref(),
                composer_override.as_deref(),
                quiet,
            )
            .await;
        }
        Command::Doctor { json } => {
            let (project, project_error) = optional_project(project_override.as_deref())?;
            let project_check = project
                .as_ref()
                .map(check_project)
                .transpose()
                .map_err(map_project_error)?;
            let mut report = run_doctor(DoctorContext {
                project,
                project_check,
                current_executable: env::current_exe().ok(),
                ..DoctorContext::default()
            });
            if let Some(error) = project_error {
                report = append_doctor_error(report, error);
            }
            if json {
                println!("{}", format_doctor_json(&report).map_err(map_json_error)?);
            } else if !quiet {
                print!("{}", format_doctor(&report));
            }
            return Ok(if report.success {
                ExitClass::Success
            } else {
                ExitClass::Validation
            });
        }
        Command::NewProject { name } => {
            if project_override.is_some() {
                return Err(AppError::message(
                    ExitClass::Usage,
                    "--project cannot be used while creating a project",
                ));
            }
            let current =
                env::current_dir().map_err(|error| AppError::new(ExitClass::Operational, error))?;
            let project = create_project(current, &name, None).map_err(map_project_error)?;
            if !quiet {
                println!(
                    "created project {} at {}",
                    project.config.project.name,
                    project.root.display()
                );
                println!(
                    "next: cd {} && rbt check && rbt run",
                    project.root.display()
                );
            }
        }
        Command::NewComponent { name } => {
            let project = required_project(project_override.as_deref())?;
            let project = add_component(&project, &name, None).map_err(map_project_error)?;
            if !quiet {
                println!("created component {name} in {}", project.root.display());
            }
        }
        Command::NewMessage { name } => {
            let project = required_project(project_override.as_deref())?;
            let project = add_message(&project, &name).map_err(map_project_error)?;
            if !quiet {
                println!(
                    "created message {name} and refreshed generated bindings in {}",
                    project.root.display()
                );
            }
        }
        Command::Check { json } => {
            let project = required_project(project_override.as_deref())?;
            let report = check_project(&project).map_err(map_project_error)?;
            if json {
                println!("{}", format_json(&report).map_err(map_cli_error)?);
            } else if !quiet {
                print!("{}", format_check_report(&report));
            }
            return Ok(if report.valid {
                ExitClass::Success
            } else {
                ExitClass::Validation
            });
        }
        Command::IdlCheck { paths } => {
            let checked = if paths.is_empty() {
                let project = required_project(project_override.as_deref())?;
                let files = project_idl_files(&project).map_err(map_project_error)?;
                for path in &files {
                    rbt_idl::compile_file(path)
                        .map_err(|error| AppError::new(ExitClass::Validation, error))?;
                }
                files.len()
            } else {
                idl_command::check(&paths).map_err(map_idl_error)?
            };
            if !quiet {
                println!("checked {checked} IDL file(s)");
            }
        }
        Command::IdlFmt { check, paths } => {
            let changed = if paths.is_empty() {
                let project = required_project(project_override.as_deref())?;
                format_project_idl(&project, check).map_err(map_project_error)?
            } else {
                idl_command::format(&paths, check).map_err(map_idl_error)?
            };
            if !quiet {
                if check {
                    println!("IDL formatting is clean");
                } else {
                    println!("formatted {changed} IDL file(s)");
                }
            }
        }
        Command::IdlGenerate => {
            let project = required_project(project_override.as_deref())?;
            let manifest = generate_project(&project).map_err(map_project_error)?;
            if !quiet {
                println!("generated {} IDL schema(s)", manifest.sources.len());
            }
        }
        Command::Run => {
            let project = required_project(project_override.as_deref())?;
            let report = check_project(&project).map_err(map_project_error)?;
            if !report.valid {
                if !quiet {
                    print!("{}", format_check_report(&report));
                }
                return Ok(ExitClass::Validation);
            }
            let address = resolve_composer_address(composer_override.as_deref(), Some(&project));
            let summary = run_project(
                &project,
                &report,
                RunOptions {
                    composer_address: Some(address),
                    skip_build: false,
                },
            )
            .await
            .map_err(map_runner_error)?;
            return Ok(if summary.status_code() == 0 {
                ExitClass::Success
            } else {
                ExitClass::Operational
            });
        }
        Command::McapInfo { input, json } => {
            let info = rbt_mcap::inspect_file(input)
                .map_err(|error| AppError::new(ExitClass::Operational, error))?;
            if json {
                println!("{}", format_json(&info).map_err(map_cli_error)?);
            } else if !quiet {
                print!("{}", format_mcap_info(&info));
            }
        }
        Command::Record {
            output,
            capabilities,
            all,
            buffer_bytes,
            duration,
        } => {
            let project = load_project_for_address(project_override.as_deref())?;
            let address = resolve_composer_address(composer_override.as_deref(), project.as_ref());
            let view = fetch_view(address).await.map_err(map_cli_error)?;
            if !quiet {
                eprintln!(
                    "recording fixed initial stream set to {} (writer budget {} bytes; Ctrl+C to stop)",
                    output.display(),
                    buffer_bytes
                );
            }
            let summary = run_record(
                &view,
                RecordOptions {
                    output,
                    capabilities,
                    all,
                    buffer_bytes,
                    duration,
                },
            )
            .await
            .map_err(map_cli_error)?;
            println!("{}", format_json(&summary).map_err(map_cli_error)?);
        }
        Command::Play {
            input,
            rate,
            skip_unsupported,
        } => {
            let project = load_project_for_address(project_override.as_deref())?;
            let address = resolve_composer_address(composer_override.as_deref(), project.as_ref());
            let summary = run_play(input, rate, skip_unsupported, address)
                .await
                .map_err(map_cli_error)?;
            println!("{}", format_json(&summary).map_err(map_cli_error)?);
        }
        runtime_command => {
            let project = load_project_for_address(project_override.as_deref())?;
            let address = resolve_composer_address(composer_override.as_deref(), project.as_ref());
            let view = fetch_view(address).await.map_err(map_cli_error)?;
            match runtime_command {
                Command::Ps { full, json } => {
                    if json {
                        println!("{}", format_ps_json(&view).map_err(map_cli_error)?);
                    } else if !quiet {
                        print!("{}", format_ps(&view, full));
                    }
                }
                Command::Graph { dot, json } => {
                    if json {
                        println!("{}", format_graph_json(&view).map_err(map_cli_error)?);
                    } else if !quiet {
                        print!("{}", format_graph(&view, dot));
                    }
                }
                Command::Why { component, json } => {
                    if json {
                        println!(
                            "{}",
                            format_why_json(&view, &component).map_err(map_cli_error)?
                        );
                    } else if !quiet {
                        print!(
                            "{}",
                            format_why_with_options(&view, &component, verbose_errors)
                                .map_err(map_cli_error)?
                        );
                    }
                }
                Command::CapabilityList {
                    query,
                    provider,
                    json,
                    verbose,
                } => {
                    if json {
                        println!(
                            "{}",
                            format_capability_list_json(
                                &view,
                                query.as_deref(),
                                provider.as_deref(),
                            )
                            .map_err(map_cli_error)?
                        );
                    } else if !quiet {
                        let output = if verbose {
                            format_capability_list_filtered_verbose(
                                &view,
                                query.as_deref(),
                                provider.as_deref(),
                            )
                        } else {
                            format_capability_list_filtered(
                                &view,
                                query.as_deref(),
                                provider.as_deref(),
                            )
                        };
                        print!("{output}");
                    }
                }
                Command::StreamInfo {
                    query,
                    json,
                    verbose,
                } => {
                    if json {
                        println!(
                            "{}",
                            rbt_cli::format_stream_info_json(&view, &query)
                                .map_err(map_cli_error)?
                        );
                    } else if !quiet {
                        let output = if verbose {
                            format_stream_info_verbose(&view, &query)
                        } else {
                            format_stream_info(&view, &query)
                        }
                        .map_err(map_cli_error)?;
                        print!("{output}");
                    }
                }
                Command::StreamPressure { query, json } => {
                    if json {
                        println!(
                            "{}",
                            rbt_cli::format_stream_pressure_json(&view, &query)
                                .map_err(map_cli_error)?
                        );
                    } else if !quiet {
                        print!(
                            "{}",
                            format_stream_pressure(&view, &query).map_err(map_cli_error)?
                        );
                    }
                }
                Command::RpcInfo {
                    query,
                    json,
                    verbose,
                } => {
                    if json {
                        println!(
                            "{}",
                            format_rpc_info_json(&view, &query).map_err(map_cli_error)?
                        );
                    } else if !quiet {
                        print!(
                            "{}",
                            format_rpc_info(&view, &query, verbose).map_err(map_cli_error)?
                        );
                    }
                }
                _ => unreachable!("non-runtime command handled above"),
            }
        }
    }
    Ok(ExitClass::Success)
}

fn required_project(explicit: Option<&std::path::Path>) -> Result<Project, AppError> {
    let current =
        env::current_dir().map_err(|error| AppError::new(ExitClass::Operational, error))?;
    let manifest = discover_project(&current, explicit).map_err(map_project_error)?;
    load_project(manifest).map_err(map_project_error)
}

fn optional_project(
    explicit: Option<&std::path::Path>,
) -> Result<(Option<Project>, Option<ProjectError>), AppError> {
    let current =
        env::current_dir().map_err(|error| AppError::new(ExitClass::Operational, error))?;
    match discover_project(&current, explicit) {
        Ok(manifest) => match load_project(manifest) {
            Ok(project) => Ok((Some(project), None)),
            Err(error) => Ok((None, Some(error))),
        },
        Err(ProjectError::NotFound { .. }) if explicit.is_none() => Ok((None, None)),
        Err(error) => Ok((None, Some(error))),
    }
}

async fn dispatch_tf(
    json: bool,
    snapshot: Option<std::path::PathBuf>,
    tree: bool,
    project_override: Option<&std::path::Path>,
    composer_override: Option<&str>,
    quiet: bool,
) -> Result<ExitClass, AppError> {
    let report = if let Some(path) = snapshot {
        rbt_cli::tf_status::from_snapshot_file(&path)
            .map_err(|error| AppError::new(ExitClass::Validation, error))?
    } else {
        let project = load_project_for_address(project_override)?;
        let address = resolve_composer_address(composer_override, project.as_ref());
        match fetch_view(address).await {
            Ok(view) => rbt_cli::tf_status::from_inspection(&view),
            Err(error) => rbt_cli::tf_status::composer_unavailable(error.to_string()),
        }
    };
    if json {
        println!(
            "{}",
            serde_json::to_string_pretty(&report).map_err(map_json_error)?
        );
    } else if !quiet {
        print!("{}", rbt_cli::tf_status::format_human(&report, tree));
    }
    Ok(if report.availability == "unavailable" {
        ExitClass::Unavailable
    } else {
        ExitClass::Success
    })
}

fn load_project_for_address(
    explicit: Option<&std::path::Path>,
) -> Result<Option<Project>, AppError> {
    let (project, error) = optional_project(explicit)?;
    if let Some(error) = error {
        return Err(map_project_error(error));
    }
    Ok(project)
}

fn resolve_composer_address(explicit: Option<&str>, project: Option<&Project>) -> String {
    explicit
        .map(str::to_owned)
        .or_else(|| env::var("RBT_COMPOSER_ADDR").ok())
        .or_else(|| project.and_then(|project| project.config.run.composer_address.clone()))
        .unwrap_or_else(|| DEFAULT_COMPOSER_ADDR.to_owned())
}

fn append_doctor_error(report: DoctorReport, error: ProjectError) -> DoctorReport {
    let mut checks = report.checks;
    checks.push(DoctorDiagnostic {
        check_id: "project_manifest_invalid".to_owned(),
        status: DiagnosticStatus::Fail,
        summary: "project manifest could not be loaded".to_owned(),
        details: Some(error.to_string()),
        suggested_action: Some("fix rbt.toml or pass --project with a valid project".to_owned()),
    });
    DoctorReport::new(checks)
}

fn map_cli_error(error: CliError) -> AppError {
    let class = match &error {
        CliError::Usage(_) => ExitClass::Usage,
        CliError::Inspect(_) | CliError::Protocol(_) | CliError::NotFound(_) => {
            ExitClass::Unavailable
        }
        _ => ExitClass::Operational,
    };
    AppError::new(class, error)
}

fn map_project_error(error: ProjectError) -> AppError {
    let class = match &error {
        ProjectError::NotFound { .. }
        | ProjectError::Manifest { .. }
        | ProjectError::Invalid(_)
        | ProjectError::Idl(_) => ExitClass::Validation,
        _ => ExitClass::Operational,
    };
    AppError::new(class, error)
}

fn map_idl_error(error: idl_command::IdlCommandError) -> AppError {
    let class = match &error {
        idl_command::IdlCommandError::Idl(_)
        | idl_command::IdlCommandError::Empty(_)
        | idl_command::IdlCommandError::NeedsFormatting { .. } => ExitClass::Validation,
        idl_command::IdlCommandError::Io { .. } => ExitClass::Operational,
    };
    AppError::new(class, error)
}

fn map_runner_error(error: RunnerError) -> AppError {
    let class = match &error {
        RunnerError::Preflight(_) | RunnerError::InvalidConfiguration(_) => ExitClass::Validation,
        RunnerError::AddressInUse(_)
        | RunnerError::ComposerTimeout { .. }
        | RunnerError::Inspection(_)
        | RunnerError::Spawn { .. } => ExitClass::Unavailable,
        _ => ExitClass::Operational,
    };
    AppError::new(class, error)
}

fn map_json_error(error: serde_json::Error) -> AppError {
    AppError::new(ExitClass::Operational, error)
}

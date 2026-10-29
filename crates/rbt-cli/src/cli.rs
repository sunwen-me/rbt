//! Command-line parsing kept separate from command execution.
//!
//! The parser is deliberately dependency-free, but it exposes one consistent
//! global-option and error contract for every command.

use std::{path::PathBuf, time::Duration};

use crate::CliError;

/// One fully parsed CLI invocation.
#[derive(Clone, Debug, PartialEq)]
pub struct Invocation {
    pub command: Command,
    pub project: Option<PathBuf>,
    pub composer_address: Option<String>,
    pub quiet: bool,
    pub verbose_errors: bool,
}

/// Parsed command-line request.
#[derive(Clone, Debug, PartialEq)]
pub enum Command {
    Help {
        topic: Option<String>,
    },
    Version {
        verbose: bool,
    },
    Doctor {
        json: bool,
    },
    TimeStatus {
        json: bool,
        sources: Option<PathBuf>,
    },
    TfStatus {
        json: bool,
        snapshot: Option<PathBuf>,
    },
    TfTree {
        json: bool,
        snapshot: Option<PathBuf>,
    },
    NewProject {
        name: String,
    },
    NewComponent {
        name: String,
    },
    NewMessage {
        name: String,
    },
    Check {
        json: bool,
    },
    IdlCheck {
        paths: Vec<PathBuf>,
    },
    IdlFmt {
        check: bool,
        paths: Vec<PathBuf>,
    },
    IdlGenerate,
    Run,
    Ps {
        full: bool,
        json: bool,
    },
    Graph {
        dot: bool,
        json: bool,
    },
    Why {
        component: String,
        json: bool,
    },
    CapabilityList {
        query: Option<String>,
        provider: Option<String>,
        json: bool,
        verbose: bool,
    },
    StreamInfo {
        query: String,
        json: bool,
        verbose: bool,
    },
    StreamPressure {
        query: String,
        json: bool,
    },
    RpcInfo {
        query: String,
        json: bool,
        verbose: bool,
    },
    McapInfo {
        input: PathBuf,
        json: bool,
    },
    Record {
        output: PathBuf,
        capabilities: Vec<String>,
        all: bool,
        buffer_bytes: usize,
        duration: Option<Duration>,
    },
    Play {
        input: PathBuf,
        rate: f64,
        skip_unsupported: bool,
    },
}

/// Parse a complete invocation, including global project and Composer
/// overrides. Global flags precede the command so positional paths remain
/// unambiguous.
pub fn parse_invocation(args: impl IntoIterator<Item = String>) -> Result<Invocation, CliError> {
    let mut arguments = args.into_iter().skip(1).peekable();
    let mut project = None;
    let mut composer_address = None;
    let mut quiet = false;
    let mut verbose_errors = false;

    loop {
        match arguments.peek().map(String::as_str) {
            Some("--project") => {
                arguments.next();
                project = Some(PathBuf::from(next_global_value(
                    &mut arguments,
                    "--project",
                )?));
            }
            Some("--composer") => {
                arguments.next();
                composer_address = Some(next_global_value(&mut arguments, "--composer")?);
            }
            Some("--quiet") => {
                arguments.next();
                quiet = true;
            }
            Some("--verbose") => {
                arguments.next();
                verbose_errors = true;
            }
            _ => break,
        }
    }

    let command_arguments = arguments.collect::<Vec<_>>();
    let command = parse_command(&command_arguments)?;
    Ok(Invocation {
        command,
        project,
        composer_address,
        quiet,
        verbose_errors,
    })
}

/// Backwards-compatible command-only parser.
pub fn parse_args(args: impl IntoIterator<Item = String>) -> Result<Command, CliError> {
    parse_invocation(args).map(|invocation| invocation.command)
}

fn next_global_value(
    arguments: &mut impl Iterator<Item = String>,
    option: &str,
) -> Result<String, CliError> {
    arguments
        .next()
        .filter(|value| !value.starts_with('-'))
        .ok_or_else(|| CliError::Usage(format!("{option} requires a value")))
}

fn parse_command(args: &[String]) -> Result<Command, CliError> {
    let Some(command) = args.first().map(String::as_str) else {
        return Err(CliError::Usage(usage()));
    };
    if matches!(command, "--help" | "-h" | "help") {
        if args.len() > 2 {
            return Err(CliError::Usage("usage: rbt help [COMMAND]".to_owned()));
        }
        return Ok(Command::Help {
            topic: args.get(1).cloned(),
        });
    }
    if args
        .get(1)
        .is_some_and(|argument| matches!(argument.as_str(), "--help" | "-h"))
    {
        if args.len() != 2 {
            return Err(CliError::Usage(format!("usage: rbt {command} --help")));
        }
        return Ok(Command::Help {
            topic: Some(command.to_owned()),
        });
    }

    match command {
        "--version" if args.len() == 1 => Ok(Command::Version { verbose: false }),
        "version" => parse_version(&args[1..]),
        "doctor" => Ok(Command::Doctor {
            json: parse_json_only(&args[1..], "doctor")?,
        }),
        "time" => parse_time(&args[1..]),
        "tf" => parse_tf(&args[1..]),
        "new" => parse_new(&args[1..]),
        "check" => Ok(Command::Check {
            json: parse_json_only(&args[1..], "check")?,
        }),
        "idl" => parse_idl(&args[1..]),
        "run" if args.len() == 1 => Ok(Command::Run),
        "run" => Err(CliError::Usage("usage: rbt run".to_owned())),
        "ps" => parse_ps(&args[1..]),
        "graph" => parse_graph(&args[1..]),
        "why" => parse_why(&args[1..]),
        "capability" => parse_capability(&args[1..]),
        "stream" => parse_stream(&args[1..]),
        "rpc" => parse_rpc(&args[1..]),
        "mcap" => parse_mcap(&args[1..]),
        "record" => parse_record_args(&args[1..]),
        "play" => parse_play_args(&args[1..]),
        other => Err(CliError::Usage(format!(
            "unknown command {other:?}\n{}",
            usage()
        ))),
    }
}

fn parse_version(args: &[String]) -> Result<Command, CliError> {
    let mut verbose = false;
    for argument in args {
        match argument.as_str() {
            "--verbose" => verbose = true,
            other => return Err(CliError::Usage(format!("unknown version option {other:?}"))),
        }
    }
    Ok(Command::Version { verbose })
}

fn parse_time(args: &[String]) -> Result<Command, CliError> {
    if args.first().map(String::as_str) != Some("status") {
        return Err(CliError::Usage(
            "usage: rbt time status [--json] [--sources PATH]".to_owned(),
        ));
    }
    let mut json = false;
    let mut sources = None;
    let mut arguments = args[1..].iter();
    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            "--json" if !json => json = true,
            "--sources" if sources.is_none() => {
                sources = Some(PathBuf::from(
                    arguments
                        .next()
                        .filter(|value| !value.starts_with('-'))
                        .ok_or_else(|| CliError::Usage("--sources requires a path".to_owned()))?,
                ));
            }
            other => {
                return Err(CliError::Usage(format!(
                    "unknown or duplicate time status option {other:?}"
                )));
            }
        }
    }
    Ok(Command::TimeStatus { json, sources })
}

fn parse_tf(args: &[String]) -> Result<Command, CliError> {
    let Some(subcommand) = args.first().map(String::as_str) else {
        return Err(CliError::Usage(
            "usage: rbt tf <status|tree> [--json] [--snapshot PATH]".to_owned(),
        ));
    };
    if !matches!(subcommand, "status" | "tree") {
        return Err(CliError::Usage(
            "usage: rbt tf <status|tree> [--json] [--snapshot PATH]".to_owned(),
        ));
    }
    let mut json = false;
    let mut snapshot = None;
    let mut arguments = args[1..].iter();
    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            "--json" if !json => json = true,
            "--snapshot" if snapshot.is_none() => {
                snapshot = Some(PathBuf::from(
                    arguments
                        .next()
                        .filter(|value| !value.starts_with('-'))
                        .ok_or_else(|| CliError::Usage("--snapshot requires a path".to_owned()))?,
                ));
            }
            other => {
                return Err(CliError::Usage(format!(
                    "unknown or duplicate tf {subcommand} option {other:?}"
                )));
            }
        }
    }
    Ok(if subcommand == "status" {
        Command::TfStatus { json, snapshot }
    } else {
        Command::TfTree { json, snapshot }
    })
}

fn parse_json_only(args: &[String], command: &str) -> Result<bool, CliError> {
    let mut json = false;
    for argument in args {
        match argument.as_str() {
            "--json" if !json => json = true,
            "--json" => return Err(CliError::Usage(format!("duplicate --json for {command}"))),
            other => {
                return Err(CliError::Usage(format!(
                    "unknown {command} option {other:?}"
                )));
            }
        }
    }
    Ok(json)
}

fn parse_new(args: &[String]) -> Result<Command, CliError> {
    match args {
        [name] if !name.starts_with('-') => Ok(Command::NewProject { name: name.clone() }),
        [kind, name] if !name.starts_with('-') && kind == "component" => {
            Ok(Command::NewComponent { name: name.clone() })
        }
        [kind, name] if !name.starts_with('-') && kind == "message" => {
            Ok(Command::NewMessage { name: name.clone() })
        }
        _ => Err(CliError::Usage(
            "usage: rbt new <project-name> | rbt new component <name> | rbt new message <Name>"
                .to_owned(),
        )),
    }
}

fn parse_idl(args: &[String]) -> Result<Command, CliError> {
    let Some(subcommand) = args.first().map(String::as_str) else {
        return Err(CliError::Usage(
            "usage: rbt idl <check|fmt|generate>".to_owned(),
        ));
    };
    match subcommand {
        "check" => Ok(Command::IdlCheck {
            paths: positional_paths(&args[1..], "idl check")?,
        }),
        "fmt" => {
            let mut check = false;
            let mut paths = Vec::new();
            for argument in &args[1..] {
                match argument.as_str() {
                    "--check" if !check => check = true,
                    "--check" => {
                        return Err(CliError::Usage("duplicate --check for idl fmt".to_owned()));
                    }
                    value if value.starts_with('-') => {
                        return Err(CliError::Usage(format!("unknown idl fmt option {value:?}")));
                    }
                    value => paths.push(PathBuf::from(value)),
                }
            }
            Ok(Command::IdlFmt { check, paths })
        }
        "generate" if args.len() == 1 => Ok(Command::IdlGenerate),
        "generate" => Err(CliError::Usage(
            "usage: rbt idl generate (outputs come from rbt.toml)".to_owned(),
        )),
        _ => Err(CliError::Usage(
            "usage: rbt idl <check|fmt|generate>".to_owned(),
        )),
    }
}

fn positional_paths(args: &[String], command: &str) -> Result<Vec<PathBuf>, CliError> {
    args.iter()
        .map(|argument| {
            if argument.starts_with('-') {
                Err(CliError::Usage(format!(
                    "unknown {command} option {argument:?}"
                )))
            } else {
                Ok(PathBuf::from(argument))
            }
        })
        .collect()
}

fn parse_ps(args: &[String]) -> Result<Command, CliError> {
    let mut full = false;
    let mut json = false;
    for argument in args {
        match argument.as_str() {
            "--full" | "--verbose" => full = true,
            "--json" => json = true,
            other => return Err(CliError::Usage(format!("unknown ps option {other:?}"))),
        }
    }
    Ok(Command::Ps { full, json })
}

fn parse_graph(args: &[String]) -> Result<Command, CliError> {
    let mut dot = false;
    let mut json = false;
    for argument in args {
        match argument.as_str() {
            "--dot" => dot = true,
            "--json" => json = true,
            other => return Err(CliError::Usage(format!("unknown graph option {other:?}"))),
        }
    }
    if dot && json {
        return Err(CliError::Usage(
            "graph accepts only one of --dot or --json".to_owned(),
        ));
    }
    Ok(Command::Graph { dot, json })
}

fn parse_why(args: &[String]) -> Result<Command, CliError> {
    let Some(component) = args.first().filter(|value| !value.starts_with('-')) else {
        return Err(CliError::Usage(
            "usage: rbt [--verbose] why <component> [--json]".to_owned(),
        ));
    };
    let json = parse_json_only(&args[1..], "why")?;
    Ok(Command::Why {
        component: component.clone(),
        json,
    })
}

fn parse_capability(args: &[String]) -> Result<Command, CliError> {
    if args.first().map(String::as_str) != Some("list") {
        return Err(CliError::Usage(
            "usage: rbt capability list [TYPE] [--provider COMPONENT] [--verbose] [--json]"
                .to_owned(),
        ));
    }
    let mut query = None;
    let mut provider = None;
    let mut json = false;
    let mut verbose = false;
    let mut index = 1;
    while index < args.len() {
        match args[index].as_str() {
            "--provider" => {
                index += 1;
                provider = Some(required_value(args, index, "--provider")?.to_owned());
            }
            "--json" => json = true,
            "--verbose" | "--full" => verbose = true,
            value if value.starts_with('-') => {
                return Err(CliError::Usage(format!(
                    "unknown capability list option {value:?}"
                )));
            }
            value if query.is_none() => query = Some(value.to_owned()),
            value => {
                return Err(CliError::Usage(format!(
                    "unexpected capability selector {value:?}"
                )));
            }
        }
        index += 1;
    }
    Ok(Command::CapabilityList {
        query,
        provider,
        json,
        verbose,
    })
}

fn parse_stream(args: &[String]) -> Result<Command, CliError> {
    if args.len() < 2 || args[1].starts_with('-') {
        return Err(CliError::Usage(
            "usage: rbt stream <info|pressure> <capability-id-or-service> [--verbose] [--json]"
                .to_owned(),
        ));
    }
    let mut json = false;
    let mut verbose = false;
    for argument in &args[2..] {
        match argument.as_str() {
            "--json" => json = true,
            "--verbose" | "--full" => verbose = true,
            other => return Err(CliError::Usage(format!("unknown stream option {other:?}"))),
        }
    }
    match args[0].as_str() {
        "info" => Ok(Command::StreamInfo {
            query: args[1].clone(),
            json,
            verbose,
        }),
        "pressure" if !verbose => Ok(Command::StreamPressure {
            query: args[1].clone(),
            json,
        }),
        "pressure" => Err(CliError::Usage(
            "--verbose is only needed for stream info; pressure already shows transport detail"
                .to_owned(),
        )),
        _ => Err(CliError::Usage(
            "usage: rbt stream <info|pressure> <capability-id-or-service> [--verbose] [--json]"
                .to_owned(),
        )),
    }
}

fn parse_rpc(args: &[String]) -> Result<Command, CliError> {
    if args.len() < 2 || args[1].starts_with('-') {
        return Err(CliError::Usage(
            "usage: rbt rpc info <capability-id-or-service> [--verbose] [--json]".to_owned(),
        ));
    }
    let mut json = false;
    let mut verbose = false;
    for argument in &args[2..] {
        match argument.as_str() {
            "--json" => json = true,
            "--verbose" | "--full" => verbose = true,
            other => return Err(CliError::Usage(format!("unknown rpc option {other:?}"))),
        }
    }
    match args[0].as_str() {
        "info" => Ok(Command::RpcInfo {
            query: args[1].clone(),
            json,
            verbose,
        }),
        _ => Err(CliError::Usage(
            "usage: rbt rpc info <capability-id-or-service> [--verbose] [--json]".to_owned(),
        )),
    }
}

fn parse_mcap(args: &[String]) -> Result<Command, CliError> {
    if args.len() < 2 || args[0] != "info" || args[1].starts_with('-') {
        return Err(CliError::Usage(
            "usage: rbt mcap info <file.mcap> [--json]".to_owned(),
        ));
    }
    let json = parse_json_only(&args[2..], "mcap info")?;
    Ok(Command::McapInfo {
        input: PathBuf::from(&args[1]),
        json,
    })
}

fn parse_record_args(args: &[String]) -> Result<Command, CliError> {
    const DEFAULT_BUFFER_BYTES: usize = 64 * 1024 * 1024;
    let mut output = None;
    let mut capabilities = Vec::new();
    let mut all = false;
    let mut buffer_bytes = DEFAULT_BUFFER_BYTES;
    let mut duration = None;
    let mut index = 0;
    while index < args.len() {
        match args[index].as_str() {
            "--output" => {
                index += 1;
                output = Some(required_value(args, index, "--output")?.into());
            }
            "--capability" => {
                index += 1;
                capabilities.push(required_value(args, index, "--capability")?.to_owned());
            }
            "--all" => all = true,
            "--buffer-bytes" => {
                index += 1;
                buffer_bytes = required_value(args, index, "--buffer-bytes")?
                    .parse::<usize>()
                    .map_err(|_| {
                        CliError::Usage("--buffer-bytes must be a positive integer".to_owned())
                    })?;
                if buffer_bytes == 0 {
                    return Err(CliError::Usage(
                        "--buffer-bytes must be a positive integer".to_owned(),
                    ));
                }
            }
            "--duration" => {
                index += 1;
                let seconds = required_value(args, index, "--duration")?
                    .parse::<f64>()
                    .map_err(|_| {
                        CliError::Usage("--duration must be positive seconds".to_owned())
                    })?;
                if !seconds.is_finite() || seconds <= 0.0 {
                    return Err(CliError::Usage(
                        "--duration must be positive seconds".to_owned(),
                    ));
                }
                duration = Some(Duration::try_from_secs_f64(seconds).map_err(|_| {
                    CliError::Usage("--duration is outside the supported range".to_owned())
                })?);
            }
            other => return Err(CliError::Usage(format!("unknown record option {other:?}"))),
        }
        index += 1;
    }
    let output = output
        .ok_or_else(|| CliError::Usage("rbt record requires --output <file.mcap>".to_owned()))?;
    if (all && !capabilities.is_empty()) || (!all && capabilities.is_empty()) {
        return Err(CliError::Usage(
            "rbt record requires either one or more --capability selectors or --all".to_owned(),
        ));
    }
    Ok(Command::Record {
        output,
        capabilities,
        all,
        buffer_bytes,
        duration,
    })
}

fn parse_play_args(args: &[String]) -> Result<Command, CliError> {
    let Some(input) = args.first().filter(|value| !value.starts_with('-')) else {
        return Err(CliError::Usage(
            "usage: rbt play <file.mcap> [--rate <nonnegative>] [--skip-unsupported]".to_owned(),
        ));
    };
    let mut rate = 1.0;
    let mut skip_unsupported = false;
    let mut index = 1;
    while index < args.len() {
        match args[index].as_str() {
            "--rate" => {
                index += 1;
                rate = required_value(args, index, "--rate")?
                    .parse::<f64>()
                    .map_err(|_| {
                        CliError::Usage("--rate must be a nonnegative number".to_owned())
                    })?;
                if !rate.is_finite() || rate < 0.0 {
                    return Err(CliError::Usage(
                        "--rate must be a finite nonnegative number".to_owned(),
                    ));
                }
            }
            "--skip-unsupported" => skip_unsupported = true,
            other => return Err(CliError::Usage(format!("unknown play option {other:?}"))),
        }
        index += 1;
    }
    Ok(Command::Play {
        input: input.into(),
        rate,
        skip_unsupported,
    })
}

fn required_value<'a>(args: &'a [String], index: usize, option: &str) -> Result<&'a str, CliError> {
    args.get(index)
        .filter(|value| !value.starts_with('-'))
        .map(String::as_str)
        .ok_or_else(|| CliError::Usage(format!("{option} requires a value")))
}

/// Return the complete command synopsis without contacting Composer.
pub fn usage() -> String {
    "rbt 0.8 — local development and diagnostics for RBT applications\n\n\
Usage:\n  rbt [--project PATH] [--composer ADDRESS] [--quiet] [--verbose] <COMMAND>\n\n\
Start here:\n  doctor                 Check this machine and the current project\n  new <PROJECT>           Create a buildable Rust project\n  new component <NAME>    Add a lifecycle-aware component\n  new message <Name>      Add and generate an IDL message\n  check                   Validate a project without running it\n  run                     Build and supervise a local graph\n\n\
Inspect a running graph:\n  ps | graph | why <component>\n  capability list | stream info | stream pressure\n  rpc info <capability>\n  time status [--json]    Inspect local clocks and explicit calibration\n\n\
  tf status | tf tree     Inspect TF providers and partial local observations\n\n\
Schemas and recordings:\n  idl check | idl fmt | idl generate\n  record | play | mcap info\n\n\
Use `rbt help <command>` for command help. JSON output is the machine interface."
        .to_owned()
}

/// Help for one top-level workflow command.
pub fn command_help(topic: Option<&str>) -> String {
    match topic {
        None => usage(),
        Some("new") => "usage:\n  rbt new <project-name>\n  rbt new component <name>\n  rbt new message <Name>\n\nProjects are self-contained Cargo workspaces; component/message forms discover rbt.toml upward from the current directory.".to_owned(),
        Some("run") => "usage: rbt [--project PATH] [--composer ADDRESS] run\n\nRuns the static project check, builds, starts Composer and configured components, prefixes child output, and owns bounded SIGINT/SIGTERM shutdown. A second signal forces termination.".to_owned(),
        Some("doctor") => "usage: rbt [--project PATH] doctor [--json]\n\nRead-only machine and project readiness checks. PASS/WARN/FAIL/SKIP identify required, optional, and unavailable checks.".to_owned(),
        Some("time") => "usage: rbt time status [--json] [--sources PATH]\n\nRead local Linux clocks without Composer. Optional bounded source snapshots are explicit calibration observations, not an automatic synchronization service.".to_owned(),
        Some("tf") => "usage:\n  rbt tf status [--json] [--snapshot PATH]\n  rbt tf tree [--json] [--snapshot PATH]\n\nWithout --snapshot, reads TF capability descriptors and bindings from Composer. Composer does not expose TF payload graph/history, so graph visibility remains partial. --snapshot reads one bounded consumer-local diagnostic observation file; it is not TF payload transport and is never discovered automatically. Its JSON fields are observed_at (an identified rbt_time::Timestamp), optional consumer_name and consumer_physical_identity, and buffer (the rbt_tf::TfPressureSnapshot fields).".to_owned(),
        Some("why") => "usage: rbt [--verbose] [--composer ADDRESS] why <component> [--json]\n\nExplains lifecycle, requirement, provider, and relevant pressure facts known to the live Composer. Human output uses compact IDs by default; global --verbose shows full identities. It never guesses causes absent from the snapshot.".to_owned(),
        Some("idl") => "usage:\n  rbt idl check [PATH...]\n  rbt idl fmt [--check] [PATH...]\n  rbt idl generate\n\nWith no paths, sources come from the discovered project's rbt.toml.".to_owned(),
        Some("check") => "usage: rbt [--project PATH] check [--json]\n\nFast static validation only; it never starts Composer or a component.".to_owned(),
        Some("ps") => "usage: rbt [--composer ADDRESS] ps [--verbose|--full] [--json]".to_owned(),
        Some("graph") => "usage: rbt [--composer ADDRESS] graph [--dot|--json]".to_owned(),
        Some("rpc") => "usage: rbt [--composer ADDRESS] rpc info <capability-id-or-service> [--verbose] [--json]\n\nShows request/response ABI, readiness, bounded resources, and available RPC metrics.".to_owned(),
        Some(other) => format!("unknown help topic {other:?}\n{}", usage()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse(arguments: &[&str]) -> Result<Invocation, CliError> {
        parse_invocation(arguments.iter().copied().map(str::to_owned))
    }

    #[test]
    fn parses_project_override_and_new_workflow() {
        let invocation =
            parse(&["rbt", "--project", "a path/rbt.toml", "check", "--json"]).unwrap();
        assert_eq!(invocation.project, Some(PathBuf::from("a path/rbt.toml")));
        assert_eq!(invocation.command, Command::Check { json: true });
        assert_eq!(
            parse(&["rbt", "new", "component", "camera"])
                .unwrap()
                .command,
            Command::NewComponent {
                name: "camera".to_owned()
            }
        );
        assert_eq!(
            parse(&["rbt", "new", "Detection"]).unwrap().command,
            Command::NewProject {
                name: "Detection".to_owned()
            }
        );
    }

    #[test]
    fn parses_local_time_status_and_rejects_ambiguous_options() {
        assert_eq!(
            parse(&[
                "rbt",
                "time",
                "status",
                "--json",
                "--sources",
                "a path.json"
            ])
            .unwrap()
            .command,
            Command::TimeStatus {
                json: true,
                sources: Some(PathBuf::from("a path.json"))
            }
        );
        for args in [
            vec!["rbt", "time"],
            vec!["rbt", "time", "status", "--sources"],
            vec!["rbt", "time", "status", "--json", "--json"],
            vec!["rbt", "time", "seek"],
        ] {
            assert!(parse(&args).is_err());
        }
    }

    #[test]
    fn parses_tf_status_and_tree_and_rejects_ambiguous_options() {
        assert_eq!(
            parse(&["rbt", "tf", "status", "--json", "--snapshot", "local.json"])
                .unwrap()
                .command,
            Command::TfStatus {
                json: true,
                snapshot: Some(PathBuf::from("local.json")),
            }
        );
        assert_eq!(
            parse(&["rbt", "tf", "tree", "--snapshot", "local.json"])
                .unwrap()
                .command,
            Command::TfTree {
                json: false,
                snapshot: Some(PathBuf::from("local.json")),
            }
        );
        for args in [
            vec!["rbt", "tf"],
            vec!["rbt", "tf", "status", "--json", "--json"],
            vec!["rbt", "tf", "tree", "--snapshot"],
            vec![
                "rbt",
                "tf",
                "status",
                "--snapshot",
                "one",
                "--snapshot",
                "two",
            ],
            vec!["rbt", "tf", "tree", "--unknown"],
        ] {
            assert!(parse(&args).is_err(), "accepted {args:?}");
        }
    }

    #[test]
    fn parses_diagnostic_json_and_verbose_flags() {
        assert_eq!(
            parse(&["rbt", "graph", "--json"]).unwrap().command,
            Command::Graph {
                dot: false,
                json: true
            }
        );
        assert_eq!(
            parse(&["rbt", "why", "detector", "--json"])
                .unwrap()
                .command,
            Command::Why {
                component: "detector".to_owned(),
                json: true
            }
        );
        let verbose_why = parse(&["rbt", "--verbose", "why", "detector"]).unwrap();
        assert!(verbose_why.verbose_errors);
        assert_eq!(
            verbose_why.command,
            Command::Why {
                component: "detector".to_owned(),
                json: false
            }
        );
        assert!(parse(&["rbt", "graph", "--dot", "--json"]).is_err());
        assert_eq!(
            parse(&["rbt", "rpc", "info", "Localization", "--verbose", "--json",])
                .unwrap()
                .command,
            Command::RpcInfo {
                query: "Localization".to_owned(),
                verbose: true,
                json: true,
            }
        );
    }

    #[test]
    fn idl_paths_and_fmt_check_are_unambiguous() {
        assert_eq!(
            parse(&["rbt", "idl", "fmt", "--check", "types dir"])
                .unwrap()
                .command,
            Command::IdlFmt {
                check: true,
                paths: vec![PathBuf::from("types dir")]
            }
        );
        assert!(parse(&["rbt", "idl", "generate", "unexpected"]).is_err());
    }

    #[test]
    fn keeps_record_and_play_contracts() {
        assert!(matches!(
            parse(&[
                "rbt",
                "record",
                "--output",
                "run.mcap",
                "--all",
                "--duration",
                "1.5"
            ])
            .unwrap()
            .command,
            Command::Record { all: true, .. }
        ));
        assert!(
            parse(&[
                "rbt",
                "record",
                "--output",
                "run.mcap",
                "--all",
                "--capability",
                "Camera"
            ])
            .is_err()
        );
        assert!(parse(&["rbt", "play", "run.mcap", "--rate", "-1"]).is_err());
    }
}

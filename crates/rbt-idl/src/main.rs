use std::{
    env, fs,
    io::ErrorKind,
    path::{Path, PathBuf},
    process::ExitCode,
};

use rbt_idl::{OutputDirs, compile_file, compile_str, format_idl, generate};

fn usage() -> &'static str {
    "usage:\n  rbt-idl --version\n  rbt-idl check [PATH...]\n  rbt-idl fmt [--check] [PATH...]\n  rbt-idl generate <file...> --rust <dir> --cpp <dir> --descriptor <dir> [--python <dir>]"
}

fn run() -> rbt_idl::Result<()> {
    let summary = run_args(env::args().skip(1))?;
    if !summary.is_empty() {
        println!("{summary}");
    }
    Ok(())
}

fn run_args<I>(arguments: I) -> rbt_idl::Result<String>
where
    I: IntoIterator<Item = String>,
{
    let mut args = arguments.into_iter();
    let Some(command) = args.next() else {
        return Err(usage_error("missing command"));
    };
    match command.as_str() {
        "--version" | "version" => {
            if args.next().is_some() {
                Err(usage_error("version accepts no arguments"))
            } else {
                Ok(format!("rbt-idl {}", env!("CARGO_PKG_VERSION")))
            }
        }
        "check" => run_check(args.collect()),
        "fmt" => run_fmt(args.collect()),
        "generate" => run_generate(args.collect()),
        value => Err(usage_error(format!("unknown command {value:?}"))),
    }
}

fn run_check(arguments: Vec<String>) -> rbt_idl::Result<String> {
    let paths = parse_input_paths(&arguments, "check")?;
    let files = collect_input_files(&resolve_input_paths(paths)?)?;
    for path in &files {
        compile_file(path)?;
    }
    Ok(format!("checked {} file(s)", files.len()))
}

fn run_fmt(arguments: Vec<String>) -> rbt_idl::Result<String> {
    let mut check_only = false;
    let mut paths = Vec::new();
    for argument in arguments {
        if argument == "--check" {
            if check_only {
                return Err(usage_error("duplicate --check option"));
            }
            check_only = true;
        } else if argument.starts_with('-') {
            return Err(usage_error(format!("unknown option {argument}")));
        } else {
            paths.push(PathBuf::from(argument));
        }
    }
    let files = collect_input_files(&resolve_input_paths(paths)?)?;
    let mut plans = Vec::with_capacity(files.len());
    for path in &files {
        let source = read_source(path)?;
        let schema = compile_str(&source, path.to_string_lossy().into_owned())?;
        let formatted = format_idl(&schema);
        plans.push((path, source, formatted));
    }

    let changed = plans
        .iter()
        .filter(|(_, source, formatted)| source != formatted)
        .map(|(path, _, _)| (*path).clone())
        .collect::<Vec<_>>();
    if check_only {
        if !changed.is_empty() {
            let paths = changed
                .iter()
                .map(|path| path.display().to_string())
                .collect::<Vec<_>>()
                .join(", ");
            return Err(usage_error(format!(
                "{} file(s) need formatting: {paths}",
                changed.len()
            )));
        }
        return Ok(format!("formatting is clean for {} file(s)", files.len()));
    }

    for (path, source, formatted) in plans {
        if source != formatted {
            fs::write(path, formatted).map_err(|source| rbt_idl::Error::Io {
                path: path.to_owned(),
                source,
            })?;
        }
    }
    Ok(format!("formatted {} file(s)", changed.len()))
}

fn run_generate(arguments: Vec<String>) -> rbt_idl::Result<String> {
    let mut files = Vec::new();
    let mut rust = None;
    let mut cpp = None;
    let mut descriptor = None;
    let mut python = None;
    let mut arguments = arguments.into_iter();
    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            "--rust" => {
                rust = Some(PathBuf::from(next_option_value(&mut arguments, "--rust")?));
            }
            "--cpp" => {
                cpp = Some(PathBuf::from(next_option_value(&mut arguments, "--cpp")?));
            }
            "--descriptor" => {
                descriptor = Some(PathBuf::from(next_option_value(
                    &mut arguments,
                    "--descriptor",
                )?));
            }
            "--python" => {
                python = Some(PathBuf::from(next_option_value(
                    &mut arguments,
                    "--python",
                )?));
            }
            value if value.starts_with('-') => {
                return Err(usage_error(format!("unknown option {value}")));
            }
            value => files.push(PathBuf::from(value)),
        }
    }
    if files.is_empty() {
        return Err(usage_error("generate requires at least one input file"));
    }
    let mut dirs = OutputDirs::new(
        rust.ok_or_else(|| usage_error("missing --rust directory"))?,
        cpp.ok_or_else(|| usage_error("missing --cpp directory"))?,
        descriptor.ok_or_else(|| usage_error("missing --descriptor directory"))?,
    );
    if let Some(python) = python {
        dirs = dirs.with_python(python);
    }
    for file in &files {
        let schema = compile_file(file)?;
        generate(&schema, &dirs)?;
    }
    Ok(format!("generated {} schema(s)", files.len()))
}

fn next_option_value(
    arguments: &mut impl Iterator<Item = String>,
    option: &str,
) -> rbt_idl::Result<String> {
    let Some(value) = arguments.next() else {
        return Err(usage_error(format!("{option} needs a directory")));
    };
    if value.starts_with('-') {
        return Err(usage_error(format!("{option} needs a directory")));
    }
    Ok(value)
}

fn parse_input_paths(arguments: &[String], command: &str) -> rbt_idl::Result<Vec<PathBuf>> {
    let mut paths = Vec::new();
    for argument in arguments {
        if argument.starts_with('-') {
            return Err(usage_error(format!(
                "unknown option {argument} for {command}"
            )));
        }
        paths.push(PathBuf::from(argument));
    }
    Ok(paths)
}

fn resolve_input_paths(paths: Vec<PathBuf>) -> rbt_idl::Result<Vec<PathBuf>> {
    if !paths.is_empty() {
        return Ok(paths);
    }
    let default = PathBuf::from("types");
    if !default.is_dir() {
        return Err(usage_error(
            "no input paths provided and default directory `types/` does not exist; pass one or more .rbt paths",
        ));
    }
    Ok(vec![default])
}

fn collect_input_files(paths: &[PathBuf]) -> rbt_idl::Result<Vec<PathBuf>> {
    let mut files = Vec::new();
    for path in paths {
        collect_input_path(path, &mut files)?;
    }
    files.sort();
    files.dedup();
    if files.is_empty() {
        let description = paths
            .iter()
            .map(|path| path.display().to_string())
            .collect::<Vec<_>>()
            .join(", ");
        return Err(usage_error(format!(
            "no .rbt files found in the requested input path(s): {description}"
        )));
    }
    Ok(files)
}

fn collect_input_path(path: &Path, files: &mut Vec<PathBuf>) -> rbt_idl::Result<()> {
    let metadata = fs::metadata(path).map_err(|source| rbt_idl::Error::Io {
        path: path.to_owned(),
        source,
    })?;
    if metadata.is_file() {
        files.push(path.to_owned());
        return Ok(());
    }
    if !metadata.is_dir() {
        return Err(input_path_error(
            path,
            "input path is not a file or directory",
        ));
    }

    let mut entries = fs::read_dir(path)
        .map_err(|source| rbt_idl::Error::Io {
            path: path.to_owned(),
            source,
        })?
        .map(|entry| {
            entry
                .map(|entry| entry.path())
                .map_err(|source| rbt_idl::Error::Io {
                    path: path.to_owned(),
                    source,
                })
        })
        .collect::<rbt_idl::Result<Vec<_>>>()?;
    entries.sort();
    for entry in entries {
        let metadata = fs::metadata(&entry).map_err(|source| rbt_idl::Error::Io {
            path: entry.clone(),
            source,
        })?;
        if metadata.is_dir() {
            collect_input_path(&entry, files)?;
        } else if metadata.is_file()
            && entry
                .extension()
                .is_some_and(|extension| extension == "rbt")
        {
            files.push(entry);
        }
    }
    Ok(())
}

fn read_source(path: &Path) -> rbt_idl::Result<String> {
    fs::read_to_string(path).map_err(|source| rbt_idl::Error::Io {
        path: path.to_owned(),
        source,
    })
}

fn input_path_error(path: &Path, message: &str) -> rbt_idl::Error {
    rbt_idl::Error::Io {
        path: path.to_owned(),
        source: std::io::Error::new(ErrorKind::InvalidInput, message),
    }
}

fn usage_error(message: impl Into<String>) -> rbt_idl::Error {
    rbt_idl::Error::Parse {
        location: rbt_idl::SourceLocation {
            filename: "<command line>".to_owned(),
            line: 1,
            column: 1,
        },
        message: format!("{}\n{}", message.into(), usage()),
    }
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            eprintln!("{error}");
            ExitCode::FAILURE
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{
        sync::atomic::{AtomicU64, Ordering},
        time::{SystemTime, UNIX_EPOCH},
    };

    static NEXT_TEMP_DIRECTORY: AtomicU64 = AtomicU64::new(0);

    fn temporary_directory() -> PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system clock is before the Unix epoch")
            .as_nanos();
        let sequence = NEXT_TEMP_DIRECTORY.fetch_add(1, Ordering::Relaxed);
        let path = env::temp_dir().join(format!(
            "rbt-idl-cli-test-{}-{nonce}-{sequence}",
            std::process::id()
        ));
        fs::create_dir_all(&path).expect("create temporary test directory");
        path
    }

    fn argument_list(command: &str, path: &Path) -> Vec<String> {
        vec![command.to_owned(), path.display().to_string()]
    }

    #[test]
    fn directory_inputs_are_recursive_and_sorted() {
        let root = temporary_directory();
        fs::create_dir_all(root.join("nested")).expect("create nested directory");
        for path in [
            root.join("z.rbt"),
            root.join("nested").join("a.rbt"),
            root.join("ignore.txt"),
        ] {
            fs::write(&path, "not a schema").expect("write test input");
        }
        let files = collect_input_files(std::slice::from_ref(&root)).expect("collect test inputs");
        assert_eq!(
            files,
            vec![root.join("nested").join("a.rbt"), root.join("z.rbt")]
        );
        fs::remove_dir_all(root).expect("remove temporary test directory");
    }

    #[test]
    fn check_does_not_write_inputs() {
        let root = temporary_directory();
        let path = root.join("schema.rbt");
        let source = "package p; abi 1; message M { value: u32; }\n";
        fs::write(&path, source).expect("write test input");
        let result = run_args(argument_list("check", &path));
        assert!(result.is_ok(), "check failed: {result:?}");
        assert_eq!(fs::read_to_string(&path).expect("read test input"), source);
        fs::remove_dir_all(root).expect("remove temporary test directory");
    }

    #[test]
    fn invalid_commands_fail() {
        assert!(run_args(Vec::<String>::new()).is_err());
        assert!(run_args(vec!["unknown".to_owned()]).is_err());
        assert!(run_args(vec!["fmt".to_owned(), "--unknown".to_owned()]).is_err());
    }

    #[test]
    fn version_reports_product_version_without_inputs() {
        assert_eq!(
            run_args(vec!["--version".to_owned()]).unwrap(),
            format!("rbt-idl {}", env!("CARGO_PKG_VERSION"))
        );
        assert!(run_args(vec!["--version".to_owned(), "extra".to_owned()]).is_err());
    }
}

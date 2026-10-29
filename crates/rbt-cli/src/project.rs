//! Local project discovery, scaffolding, IDL generation, and static checks.
//!
//! `rbt.toml` is developer-tool configuration. It is deliberately not a
//! lifecycle or data-plane protocol: runtime truth continues to come from the
//! Composer and component snapshots.

use std::{
    collections::{BTreeMap, BTreeSet},
    env, fs,
    path::{Path, PathBuf},
};

use rbt_idl::{
    CanonicalSchema, Error as IdlError, OutputDirs, compile_file, format_idl, generate, parse_file,
};
use serde::{Deserialize, Serialize};

/// Current local project-file schema.
pub const PROJECT_SCHEMA_VERSION: u16 = 1;
/// Generation-manifest schema shared by `generate` and `check`.
pub const GENERATION_MANIFEST_VERSION: u16 = 1;

/// Failures while discovering, reading, or mutating a local project.
#[derive(Debug, thiserror::Error)]
pub enum ProjectError {
    #[error("no rbt.toml found from {start}; run `rbt new <name>` or pass `--project <path>`")]
    NotFound { start: PathBuf },
    #[error("{path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
    #[error("{path}:{line}:{column}: invalid rbt.toml: {message}")]
    Manifest {
        path: PathBuf,
        line: usize,
        column: usize,
        message: String,
    },
    #[error("{0}")]
    Invalid(String),
    #[error(transparent)]
    Idl(#[from] rbt_idl::Error),
    #[error("failed to encode project data: {0}")]
    Json(#[from] serde_json::Error),
    #[error("failed to encode rbt.toml: {0}")]
    TomlEncode(#[from] toml::ser::Error),
}

pub type Result<T> = std::result::Result<T, ProjectError>;

/// Top-level `rbt.toml` model.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ProjectConfig {
    pub schema_version: u16,
    pub project: ProjectSection,
    #[serde(default)]
    pub build: BuildSection,
    #[serde(default)]
    pub idl: IdlSection,
    #[serde(default)]
    pub run: RunSection,
    #[serde(default, rename = "component")]
    pub components: Vec<ComponentConfig>,
}

/// Stable project identity and expected SDK version.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ProjectSection {
    pub name: String,
    pub sdk_version: String,
}

/// Optional build command run by the local launcher before component start.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct BuildSection {
    #[serde(
        default = "default_build_command",
        deserialize_with = "deserialize_command_vec"
    )]
    pub command: Vec<String>,
}

impl Default for BuildSection {
    fn default() -> Self {
        Self {
            command: default_build_command(),
        }
    }
}

fn default_build_command() -> Vec<String> {
    ["cargo", "build", "--workspace"]
        .into_iter()
        .map(str::to_owned)
        .collect()
}

#[derive(Deserialize)]
#[serde(untagged)]
enum CommandValue {
    Array(Vec<String>),
    String(String),
}

fn command_value(value: CommandValue) -> Vec<String> {
    match value {
        CommandValue::Array(values) => values,
        CommandValue::String(value) => value.split_whitespace().map(str::to_owned).collect(),
    }
}

fn deserialize_command_vec<'de, D>(deserializer: D) -> std::result::Result<Vec<String>, D::Error>
where
    D: serde::Deserializer<'de>,
{
    Option::<CommandValue>::deserialize(deserializer)
        .map(|value| value.map(command_value).unwrap_or_default())
}

fn deserialize_optional_command<'de, D>(
    deserializer: D,
) -> std::result::Result<Option<Vec<String>>, D::Error>
where
    D: serde::Deserializer<'de>,
{
    Option::<CommandValue>::deserialize(deserializer).map(|value| value.map(command_value))
}

/// Canonical IDL source and output directories, relative to the project root.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct IdlSection {
    #[serde(default = "default_idl_sources")]
    pub sources: Vec<PathBuf>,
    #[serde(default = "default_rust_output")]
    pub rust: PathBuf,
    #[serde(default = "default_cpp_output")]
    pub cpp: PathBuf,
    #[serde(default = "default_descriptor_output")]
    pub descriptor: PathBuf,
    #[serde(default = "default_python_output")]
    pub python: Option<PathBuf>,
}

impl Default for IdlSection {
    fn default() -> Self {
        Self {
            sources: default_idl_sources(),
            rust: default_rust_output(),
            cpp: default_cpp_output(),
            descriptor: default_descriptor_output(),
            python: default_python_output(),
        }
    }
}

fn default_idl_sources() -> Vec<PathBuf> {
    vec![PathBuf::from("types")]
}
fn default_rust_output() -> PathBuf {
    "generated/rust".into()
}
fn default_cpp_output() -> PathBuf {
    "generated/cpp".into()
}
fn default_descriptor_output() -> PathBuf {
    "generated/descriptor".into()
}
fn default_python_output() -> Option<PathBuf> {
    Some("generated/python".into())
}

/// Bounded local-launch settings.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct RunSection {
    #[serde(default, deserialize_with = "deserialize_optional_command")]
    pub composer: Option<Vec<String>>,
    #[serde(default)]
    pub composer_address: Option<String>,
    #[serde(default = "default_startup_timeout")]
    pub startup_timeout_seconds: u64,
    #[serde(default = "default_shutdown_timeout")]
    pub shutdown_timeout_seconds: u64,
}

impl Default for RunSection {
    fn default() -> Self {
        Self {
            composer: None,
            composer_address: None,
            startup_timeout_seconds: default_startup_timeout(),
            shutdown_timeout_seconds: default_shutdown_timeout(),
        }
    }
}

const fn default_startup_timeout() -> u64 {
    15
}
const fn default_shutdown_timeout() -> u64 {
    10
}

/// One locally launched component.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(deny_unknown_fields)]
pub struct ComponentConfig {
    pub name: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub package: Option<String>,
    #[serde(
        default,
        deserialize_with = "deserialize_optional_command",
        skip_serializing_if = "Option::is_none"
    )]
    pub command: Option<Vec<String>>,
    #[serde(default, skip_serializing_if = "BTreeMap::is_empty")]
    pub environment: BTreeMap<String, String>,
}

impl ComponentConfig {
    pub fn rust_package(name: impl Into<String>) -> Self {
        let name = name.into();
        Self {
            package: Some(name.clone()),
            name,
            command: None,
            environment: BTreeMap::new(),
        }
    }
}

/// Loaded project and its deterministic root.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct Project {
    pub root: PathBuf,
    pub manifest_path: PathBuf,
    pub config: ProjectConfig,
}

/// Stable diagnostic status used by `check` and `doctor`.
#[derive(Clone, Copy, Debug, Deserialize, Eq, PartialEq, Serialize)]
#[serde(rename_all = "UPPERCASE")]
pub enum DiagnosticStatus {
    Pass,
    Warn,
    Fail,
    Skip,
}

/// Structured, actionable project diagnostic.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct ProjectDiagnostic {
    pub check_id: String,
    pub status: DiagnosticStatus,
    pub summary: String,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub details: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub suggested_action: Option<String>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub path: Option<PathBuf>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub line: Option<usize>,
    #[serde(skip_serializing_if = "Option::is_none")]
    pub column: Option<usize>,
}

impl ProjectDiagnostic {
    fn new(
        check_id: impl Into<String>,
        status: DiagnosticStatus,
        summary: impl Into<String>,
    ) -> Self {
        Self {
            check_id: check_id.into(),
            status,
            summary: summary.into(),
            details: None,
            suggested_action: None,
            path: None,
            line: None,
            column: None,
        }
    }

    fn details(mut self, value: impl Into<String>) -> Self {
        self.details = Some(value.into());
        self
    }

    fn action(mut self, value: impl Into<String>) -> Self {
        self.suggested_action = Some(value.into());
        self
    }

    fn at(mut self, path: impl Into<PathBuf>, line: Option<usize>, column: Option<usize>) -> Self {
        self.path = Some(path.into());
        self.line = line;
        self.column = column;
        self
    }
}

/// Complete static project-check result.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct ProjectCheckReport {
    pub project_root: PathBuf,
    pub manifest: PathBuf,
    pub valid: bool,
    pub diagnostics: Vec<ProjectDiagnostic>,
}

/// The SDK root used by generated external projects.
///
/// The CLI intentionally does not fall back to the source checkout (or to a
/// Cargo target directory).  That would make a generated project appear to
/// work on the maintainer's machine while being unusable by an external
/// consumer.  Resolution order is explicit argument, `RBT_RUST_SDK`, and the
/// installed prefix next to the running `rbt` executable.
pub fn resolve_sdk_root(explicit: Option<&Path>) -> Result<PathBuf> {
    let candidate = if let Some(path) = explicit {
        path.to_path_buf()
    } else if let Some(path) = env::var_os("RBT_RUST_SDK") {
        PathBuf::from(path)
    } else {
        let executable = env::current_exe().map_err(|source| ProjectError::Io {
            path: PathBuf::from("<current executable>"),
            source,
        })?;
        executable
            .parent()
            .and_then(Path::parent)
            .map(|prefix| prefix.join("share/rbt/rust"))
            .ok_or_else(|| {
                ProjectError::Invalid(
                    "cannot infer the installed SDK prefix from the current executable; set RBT_RUST_SDK"
                        .to_owned(),
                )
            })?
    };
    let candidate = absolute_from(
        &candidate,
        &env::current_dir().map_err(|source| ProjectError::Io {
            path: PathBuf::from("."),
            source,
        })?,
    );
    validate_sdk_root(&candidate)?;
    Ok(candidate)
}

fn validate_sdk_root(path: &Path) -> Result<()> {
    for relative in [
        "Cargo.toml",
        "crates/rbt/Cargo.toml",
        "crates/rbt-types/Cargo.toml",
        "third_party/iceoryx2-rbt/source/Cargo.toml",
        "third_party/iceoryx2-rbt/source/iceoryx2/Cargo.toml",
    ] {
        let candidate = path.join(relative);
        if !candidate.is_file() {
            return Err(ProjectError::Invalid(format!(
                "Rust SDK is unavailable at {}; missing {}. Install rbt or set RBT_RUST_SDK to an installed SDK root",
                path.display(),
                candidate.display()
            )));
        }
    }
    Ok(())
}

fn installed_sdk_version(path: &Path) -> Option<String> {
    let source = fs::read_to_string(path.join("Cargo.toml")).ok()?;
    let manifest = toml::from_str::<toml::Value>(&source).ok()?;
    manifest
        .get("workspace")
        .and_then(|workspace| workspace.get("package"))
        .and_then(|package| package.get("version"))
        .and_then(toml::Value::as_str)
        .or_else(|| {
            manifest
                .get("package")
                .and_then(|package| package.get("version"))
                .and_then(toml::Value::as_str)
        })
        .map(str::to_owned)
}

/// Return the sorted `.rbt` files under the configured source paths.
pub fn project_idl_files(project: &Project) -> Result<Vec<PathBuf>> {
    validate_idl_paths(project)?;
    let mut files = Vec::new();
    for source in &project.config.idl.sources {
        let path = project.root.join(source);
        if !path.exists() {
            return Err(ProjectError::Invalid(format!(
                "IDL source path {} does not exist; create it or update [idl].sources",
                path.display()
            )));
        }
        collect_idl_path(&path, &mut files)?;
    }
    files.sort();
    files.dedup();
    Ok(files)
}

fn collect_idl_path(path: &Path, files: &mut Vec<PathBuf>) -> Result<()> {
    let metadata = fs::metadata(path).map_err(|source| ProjectError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    if metadata.is_file() {
        if path.extension().is_some_and(|extension| extension == "rbt") {
            files.push(path.to_path_buf());
        }
        return Ok(());
    }
    let mut children = fs::read_dir(path)
        .map_err(|source| ProjectError::Io {
            path: path.to_path_buf(),
            source,
        })?
        .map(|entry| {
            entry
                .map(|entry| entry.path())
                .map_err(|source| ProjectError::Io {
                    path: path.to_path_buf(),
                    source,
                })
        })
        .collect::<Result<Vec<_>>>()?;
    children.sort();
    for child in children {
        collect_idl_path(&child, files)?;
    }
    Ok(())
}

fn validate_project_relative_path(path: &Path, label: &str) -> Result<()> {
    if path.as_os_str().is_empty() || path.is_absolute() {
        return Err(ProjectError::Invalid(format!(
            "{label} path {} must be a non-empty project-relative path",
            path.display()
        )));
    }
    if path.components().any(|component| {
        matches!(
            component,
            std::path::Component::RootDir
                | std::path::Component::Prefix(_)
                | std::path::Component::ParentDir
        )
    }) {
        return Err(ProjectError::Invalid(format!(
            "{label} path {} must not contain an absolute root or `..` component",
            path.display()
        )));
    }
    Ok(())
}

fn validate_idl_paths(project: &Project) -> Result<()> {
    for source in &project.config.idl.sources {
        validate_project_relative_path(source, "IDL source")?;
    }
    validate_project_relative_path(&project.config.idl.rust, "Rust output")?;
    validate_project_relative_path(&project.config.idl.cpp, "C++ output")?;
    validate_project_relative_path(&project.config.idl.descriptor, "descriptor output")?;
    if let Some(python) = &project.config.idl.python {
        validate_project_relative_path(python, "Python output")?;
    }
    let outputs = [
        &project.config.idl.rust,
        &project.config.idl.cpp,
        &project.config.idl.descriptor,
    ];
    for output in outputs.into_iter().chain(project.config.idl.python.iter()) {
        if output
            .components()
            .all(|component| matches!(component, std::path::Component::CurDir))
        {
            return Err(ProjectError::Invalid(format!(
                "generated output path {} must not be the project root",
                output.display()
            )));
        }
        let output = project.root.join(output);
        for source in &project.config.idl.sources {
            let source = project.root.join(source);
            if output == source || output.starts_with(&source) {
                return Err(ProjectError::Invalid(format!(
                    "generated output {} overlaps IDL source path {}",
                    output.display(),
                    source.display()
                )));
            }
        }
    }
    Ok(())
}

/// Create a minimal, buildable external project below `base`.
pub fn create_project(base: impl AsRef<Path>, name: &str, sdk: Option<&Path>) -> Result<Project> {
    let base = absolute_from(
        base.as_ref(),
        &env::current_dir().map_err(|source| ProjectError::Io {
            path: PathBuf::from("."),
            source,
        })?,
    );
    let target = if Path::new(name).is_absolute() {
        PathBuf::from(name)
    } else {
        base.join(name)
    };
    let target = absolute_from(&target, &base);
    if target.exists() {
        return Err(ProjectError::Invalid(format!(
            "refusing to overwrite existing project directory {}",
            target.display()
        )));
    }
    let project_name = target
        .file_name()
        .and_then(|value| value.to_str())
        .ok_or_else(|| ProjectError::Invalid("project name must be valid UTF-8".to_owned()))?;
    validate_kebab_identifier(project_name, "project name")?;
    let sdk_root = resolve_sdk_root(sdk)?;
    let sdk_version = env!("CARGO_PKG_VERSION").to_owned();
    let mut created = false;
    let result = (|| {
        fs::create_dir_all(&target).map_err(|source| ProjectError::Io {
            path: target.clone(),
            source,
        })?;
        created = true;
        for relative in [
            "types/src",
            "components/hello-source/src",
            "components/hello-sink/src",
            "generated/rust",
            "generated/cpp",
            "generated/descriptor",
            "generated/python",
        ] {
            fs::create_dir_all(target.join(relative)).map_err(|source| ProjectError::Io {
                path: target.join(relative),
                source,
            })?;
        }

        let components = vec![
            ComponentConfig::rust_package("hello-source"),
            ComponentConfig::rust_package("hello-sink"),
        ];
        let config = ProjectConfig {
            schema_version: PROJECT_SCHEMA_VERSION,
            project: ProjectSection {
                name: project_name.to_owned(),
                sdk_version: sdk_version.clone(),
            },
            build: BuildSection::default(),
            idl: IdlSection::default(),
            run: RunSection::default(),
            components,
        };
        write_manifest(&target.join("rbt.toml"), &config)?;
        let iox2_source = toml_string(&sdk_root.join("third_party/iceoryx2-rbt/source/iceoryx2"));
        write_string(
            &target.join("Cargo.toml"),
            &format!(
                "[workspace]\nresolver = \"2\"\nmembers = [\"types\", \"components/*\"]\n\n[patch.crates-io]\niceoryx2 = {{ path = \"{iox2_source}\" }}\n"
            ),
        )?;
        write_template(
            &target.join("README.md"),
            include_str!("../templates/project-README.md"),
            &[("{{project_name}}", project_name)],
        )?;
        write_string(
            &target.join(".gitignore"),
            include_str!("../templates/project.gitignore"),
        )?;
        write_template(
            &target.join("types/Cargo.toml"),
            include_str!("../templates/types-Cargo.toml"),
            &[
                ("{{types_package}}", &format!("{project_name}-types")),
                ("{{sdk_version}}", &sdk_version),
                ("{{sdk_root}}", &toml_string(&sdk_root)),
            ],
        )?;
        write_template(
            &target.join("types/src/lib.rs"),
            include_str!("../templates/types-lib.rs"),
            &[("{{modules}}", "")],
        )?;
        write_component_template(
            &target,
            "hello-source",
            include_str!("../templates/source-Cargo.toml"),
            include_str!("../templates/source-main.rs"),
            &sdk_root,
            &sdk_version,
        )?;
        write_component_template(
            &target,
            "hello-sink",
            include_str!("../templates/sink-Cargo.toml"),
            include_str!("../templates/sink-main.rs"),
            &sdk_root,
            &sdk_version,
        )?;
        let project = load_project(target.join("rbt.toml"))?;
        generate_project(&project)?;
        Ok(project)
    })();
    if result.is_err() && created {
        // The target did not exist before this operation; remove only that
        // exact target so a failed scaffold cannot leave a half-project.
        let _ = fs::remove_dir_all(&target);
    }
    result
}

/// Alias used by callers that name the command after its subcommand.
pub fn new_project(base: impl AsRef<Path>, name: &str, sdk: Option<&Path>) -> Result<Project> {
    create_project(base, name, sdk)
}

fn write_component_template(
    root: &Path,
    package: &str,
    cargo_template: &str,
    source_template: &str,
    sdk_root: &Path,
    sdk_version: &str,
) -> Result<()> {
    let component_root = root.join("components").join(package).join("src");
    let component_type = pascal_case(package);
    let sdk_root = toml_string(sdk_root);
    let replacements = [
        ("{{package_name}}", package.to_owned()),
        ("{{component_name}}", package.to_owned()),
        ("{{component_type}}", component_type),
        ("{{sdk_version}}", sdk_version.to_owned()),
        ("{{sdk_root}}", sdk_root),
    ];
    write_template_owned(
        &component_root
            .parent()
            .expect("component src has a parent")
            .join("Cargo.toml"),
        cargo_template,
        &replacements,
    )?;
    write_template_owned(
        &component_root.join("main.rs"),
        source_template,
        &replacements,
    )
}

/// Add a buildable component package and register it in `rbt.toml`.
pub fn add_component(project: &Project, name: &str, sdk: Option<&Path>) -> Result<Project> {
    validate_kebab_identifier(name, "component name")?;
    if project
        .config
        .components
        .iter()
        .any(|component| component.name == name)
    {
        return Err(ProjectError::Invalid(format!(
            "component {name:?} is already declared in {}",
            project.manifest_path.display()
        )));
    }
    let component_root = project.root.join("components").join(name);
    if component_root.exists() {
        return Err(ProjectError::Invalid(format!(
            "refusing to overwrite existing component directory {}",
            component_root.display()
        )));
    }
    let sdk_root = resolve_sdk_root(sdk)?;
    let sdk_version = env!("CARGO_PKG_VERSION");
    let result = (|| {
        fs::create_dir_all(component_root.join("src")).map_err(|source| ProjectError::Io {
            path: component_root.clone(),
            source,
        })?;
        write_component_template(
            &project.root,
            name,
            include_str!("../templates/component-Cargo.toml"),
            include_str!("../templates/component-main.rs"),
            &sdk_root,
            sdk_version,
        )?;
        let mut config = project.config.clone();
        config.components.push(ComponentConfig::rust_package(name));
        config
            .components
            .sort_by(|left, right| left.name.cmp(&right.name));
        write_manifest(&project.manifest_path, &config)?;
        load_project(&project.manifest_path)
    })();
    if result.is_err() {
        let _ = fs::remove_dir_all(&component_root);
    }
    result
}

/// Add a minimal canonical message source and immediately refresh bindings.
pub fn add_message(project: &Project, name: &str) -> Result<Project> {
    validate_message_identifier(name)?;
    let files = project_idl_files(project)?;
    for file in &files {
        let ast = parse_file(file)?;
        if ast
            .declarations
            .iter()
            .any(|declaration| declaration.name() == name)
        {
            return Err(ProjectError::Invalid(format!(
                "message or type {name:?} is already declared in {}",
                file.display()
            )));
        }
    }
    let source_root = project
        .config
        .idl
        .sources
        .first()
        .map(|source| {
            let path = project.root.join(source);
            if path.extension().is_some_and(|extension| extension == "rbt") {
                path.parent()
                    .map(Path::to_path_buf)
                    .unwrap_or_else(|| project.root.clone())
            } else {
                path
            }
        })
        .unwrap_or_else(|| project.root.join("types"));
    fs::create_dir_all(&source_root).map_err(|source| ProjectError::Io {
        path: source_root.clone(),
        source,
    })?;
    let path = source_root.join(format!("{}.rbt", snake_case(name)));
    if path.exists() {
        return Err(ProjectError::Invalid(format!(
            "refusing to overwrite existing IDL file {}",
            path.display()
        )));
    }
    let package = format!(
        "{}.{}",
        snake_case(&project.config.project.name),
        snake_case(name)
    );
    let source = format!(
        "package {package};\nabi 1;\n\nmessage {name} {{\n    sequence: u64;\n    value: u64;\n}}\n"
    );
    write_string(&path, &source)?;
    let result = (|| {
        let project = load_project(&project.manifest_path)?;
        generate_project(&project)?;
        Ok(project)
    })();
    if result.is_err() {
        let _ = fs::remove_file(&path);
    }
    result
}

/// Walk upward like Cargo until an `rbt.toml` is found.
pub fn discover_project(start: impl AsRef<Path>, explicit: Option<&Path>) -> Result<PathBuf> {
    let start = absolute_from(
        start.as_ref(),
        &env::current_dir().map_err(|source| ProjectError::Io {
            path: PathBuf::from("."),
            source,
        })?,
    );
    if let Some(explicit) = explicit {
        let base = if start.is_file() {
            start.parent().unwrap_or(Path::new("/"))
        } else {
            &start
        };
        let candidate = absolute_from(explicit, base);
        let manifest = if candidate.is_dir() {
            candidate.join("rbt.toml")
        } else {
            candidate
        };
        return manifest
            .is_file()
            .then_some(manifest.clone())
            .ok_or(ProjectError::NotFound { start: manifest });
    }
    let mut cursor = if start.is_file() {
        start.parent().unwrap_or(Path::new("/")).to_path_buf()
    } else {
        start.clone()
    };
    loop {
        let manifest = cursor.join("rbt.toml");
        if manifest.is_file() {
            return Ok(manifest);
        }
        if !cursor.pop() {
            return Err(ProjectError::NotFound { start });
        }
    }
}

/// Load and validate the TOML shape. Semantic checks are reported separately.
pub fn load_project(manifest_path: impl AsRef<Path>) -> Result<Project> {
    let manifest_path = manifest_path.as_ref().to_path_buf();
    let source = read_string(&manifest_path)?;
    let config = toml::from_str::<ProjectConfig>(&source).map_err(|error| {
        let (line, column) = error
            .span()
            .map(|span| line_column(&source, span.start))
            .unwrap_or((1, 1));
        ProjectError::Manifest {
            path: manifest_path.clone(),
            line,
            column,
            message: error.message().to_owned(),
        }
    })?;
    let root = manifest_path
        .parent()
        .unwrap_or(Path::new("."))
        .to_path_buf();
    Ok(Project {
        root,
        manifest_path,
        config,
    })
}

fn absolute_from(path: &Path, base: &Path) -> PathBuf {
    if path.is_absolute() {
        path.to_path_buf()
    } else {
        base.join(path)
    }
}

fn line_column(source: &str, offset: usize) -> (usize, usize) {
    let prefix = &source[..offset.min(source.len())];
    let line = prefix.bytes().filter(|byte| *byte == b'\n').count() + 1;
    let column = prefix
        .rsplit_once('\n')
        .map_or(prefix.chars().count() + 1, |(_, tail)| {
            tail.chars().count() + 1
        });
    (line, column)
}

fn read_string(path: &Path) -> Result<String> {
    fs::read_to_string(path).map_err(|source| ProjectError::Io {
        path: path.to_path_buf(),
        source,
    })
}

fn write_string(path: &Path, value: &str) -> Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|source| ProjectError::Io {
            path: parent.to_path_buf(),
            source,
        })?;
    }
    if fs::read_to_string(path).ok().as_deref() == Some(value) {
        return Ok(());
    }
    fs::write(path, value).map_err(|source| ProjectError::Io {
        path: path.to_path_buf(),
        source,
    })
}

fn write_manifest(path: &Path, config: &ProjectConfig) -> Result<()> {
    let contents = toml::to_string_pretty(config)?;
    write_string(path, &contents)
}

fn write_template(path: &Path, template: &str, replacements: &[(&str, &str)]) -> Result<()> {
    let mut output = template.to_owned();
    for (needle, replacement) in replacements {
        output = output.replace(needle, replacement);
    }
    write_string(path, &output)
}

fn write_template_owned(
    path: &Path,
    template: &str,
    replacements: &[(&str, String)],
) -> Result<()> {
    let mut output = template.to_owned();
    for (needle, replacement) in replacements {
        output = output.replace(needle, replacement);
    }
    write_string(path, &output)
}

fn toml_string(path: &Path) -> String {
    path.to_string_lossy()
        .replace('\\', "\\\\")
        .replace('"', "\\\"")
}

fn reserved_identifier(value: &str) -> bool {
    matches!(
        value,
        "as" | "break"
            | "const"
            | "continue"
            | "crate"
            | "else"
            | "enum"
            | "extern"
            | "false"
            | "fn"
            | "for"
            | "if"
            | "impl"
            | "in"
            | "let"
            | "loop"
            | "match"
            | "mod"
            | "move"
            | "mut"
            | "pub"
            | "ref"
            | "return"
            | "self"
            | "Self"
            | "static"
            | "struct"
            | "super"
            | "trait"
            | "true"
            | "type"
            | "unsafe"
            | "use"
            | "where"
            | "while"
            | "async"
            | "await"
            | "dyn"
            | "abstract"
            | "become"
            | "box"
            | "do"
            | "final"
            | "macro"
            | "override"
            | "priv"
            | "typeof"
            | "unsized"
            | "virtual"
            | "yield"
            | "try"
            | "union"
            | "package"
            | "abi"
            | "primitive"
            | "newtype"
            | "message"
    )
}

fn validate_kebab_identifier(value: &str, label: &str) -> Result<()> {
    let valid = !value.is_empty()
        && !value.starts_with('-')
        && !value.ends_with('-')
        && !value.contains("--")
        && value.chars().all(|character| {
            character.is_ascii_lowercase() || character.is_ascii_digit() || character == '-'
        });
    if !valid || reserved_identifier(value) {
        return Err(ProjectError::Invalid(format!(
            "invalid {label} {value:?}; use lower-kebab-case with letters, digits, and single hyphens"
        )));
    }
    Ok(())
}

fn validate_message_identifier(value: &str) -> Result<()> {
    let valid = !value.is_empty()
        && value
            .chars()
            .next()
            .is_some_and(|character| character.is_ascii_uppercase())
        && value
            .chars()
            .all(|character| character.is_ascii_alphanumeric());
    if !valid || reserved_identifier(value) {
        return Err(ProjectError::Invalid(format!(
            "invalid message name {value:?}; use PascalCase letters and digits and avoid reserved words"
        )));
    }
    Ok(())
}

fn pascal_case(value: &str) -> String {
    value
        .split(['-', '_'])
        .filter(|part| !part.is_empty())
        .map(|part| {
            let mut chars = part.chars();
            let mut output = String::new();
            if let Some(first) = chars.next() {
                output.push(first.to_ascii_uppercase());
                output.extend(chars);
            }
            output
        })
        .collect()
}

fn snake_case(value: &str) -> String {
    let mut output = String::new();
    for (index, character) in value.chars().enumerate() {
        if character == '-' || character == ' ' {
            if !output.ends_with('_') {
                output.push('_');
            }
        } else if character.is_ascii_uppercase() {
            if index > 0 && !output.ends_with('_') {
                output.push('_');
            }
            output.push(character.to_ascii_lowercase());
        } else {
            output.push(character);
        }
    }
    output.trim_matches('_').to_owned()
}

fn package_stem(package: &str) -> String {
    snake_case(package.rsplit('.').next().unwrap_or(package))
}

/// One source entry in the deterministic generated-file manifest.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct GenerationSource {
    pub source: PathBuf,
    pub hash: String,
    pub package: String,
    pub stem: String,
    pub outputs: Vec<PathBuf>,
}

/// The generated artifact ownership marker.
#[derive(Clone, Debug, Deserialize, Eq, PartialEq, Serialize)]
pub struct GenerationManifest {
    pub schema_version: u16,
    pub generator_version: String,
    pub sources: Vec<GenerationSource>,
}

struct CompiledSource {
    schema: CanonicalSchema,
    entry: GenerationSource,
}

fn output_dirs(project: &Project) -> OutputDirs {
    let idl = &project.config.idl;
    let rust = project.root.join(&idl.rust);
    let cpp = project.root.join(&idl.cpp);
    let descriptor = project.root.join(&idl.descriptor);
    let mut dirs = OutputDirs::new(rust, cpp, descriptor);
    if let Some(python) = &idl.python {
        dirs = dirs.with_python(project.root.join(python));
    }
    dirs
}

fn relative_path(root: &Path, path: &Path) -> PathBuf {
    path.strip_prefix(root).unwrap_or(path).to_path_buf()
}

fn compile_project_sources(project: &Project) -> Result<Vec<CompiledSource>> {
    let files = project_idl_files(project)?;
    let dirs = output_dirs(project);
    let mut compiled = Vec::with_capacity(files.len());
    let mut stems = BTreeMap::<String, PathBuf>::new();
    for path in files {
        let source = read_string(&path)?;
        let ast = parse_file(&path)?;
        let schema = compile_file(&path)?;
        let stem = package_stem(&ast.package);
        if let Some(previous) = stems.insert(stem.clone(), path.clone()) {
            return Err(ProjectError::Invalid(format!(
                "IDL output stem collision {stem:?}: {} and {}; change one package's final segment",
                previous.display(),
                path.display()
            )));
        }
        let mut outputs = vec![
            relative_path(
                project.root.as_path(),
                &dirs.rust.join(format!("{stem}.rs")),
            ),
            relative_path(
                project.root.as_path(),
                &dirs.cpp.join("rbt").join(format!("{stem}.hpp")),
            ),
            relative_path(
                project.root.as_path(),
                &dirs.descriptor.join(format!("{stem}.json")),
            ),
        ];
        if let Some(python) = &dirs.python {
            outputs.push(relative_path(
                project.root.as_path(),
                &python.join(format!("{stem}.py")),
            ));
        }
        outputs.sort();
        compiled.push(CompiledSource {
            entry: GenerationSource {
                source: relative_path(&project.root, &path),
                hash: blake3::hash(source.as_bytes()).to_hex().to_string(),
                package: ast.package.clone(),
                stem,
                outputs,
            },
            schema,
        });
    }
    compiled.sort_by(|left, right| left.entry.source.cmp(&right.entry.source));
    Ok(compiled)
}

fn module_index(_project: &Project, sources: &[CompiledSource]) -> String {
    let mut output = String::from(
        "//! Generated project message bindings. Do not edit by hand.\n\npub use rbt_types::*;\n",
    );
    let mut stems = sources
        .iter()
        .map(|source| source.entry.stem.clone())
        .collect::<Vec<_>>();
    stems.sort();
    for stem in stems {
        output.push_str(&format!(
            "\ninclude!(concat!(env!(\"CARGO_MANIFEST_DIR\"), \"/../generated/rust/{stem}.rs\"));\n"
        ));
    }
    output
}

fn generation_manifest(sources: &[CompiledSource]) -> GenerationManifest {
    GenerationManifest {
        schema_version: GENERATION_MANIFEST_VERSION,
        generator_version: env!("CARGO_PKG_VERSION").to_owned(),
        sources: sources.iter().map(|source| source.entry.clone()).collect(),
    }
}

fn manifest_path(project: &Project) -> PathBuf {
    project.root.join("generated/generation-manifest.json")
}

fn read_generation_manifest(project: &Project) -> Result<Option<GenerationManifest>> {
    let path = manifest_path(project);
    if !path.is_file() {
        return Ok(None);
    }
    let source = read_string(&path)?;
    serde_json::from_str(&source).map(Some).map_err(|error| {
        ProjectError::Invalid(format!(
            "{} is not valid generation metadata: {error}; run `rbt idl generate`",
            path.display()
        ))
    })
}

/// Generate all configured language outputs and update the exact ownership marker.
pub fn generate_project(project: &Project) -> Result<GenerationManifest> {
    let sources = compile_project_sources(project)?;
    let dirs = output_dirs(project);
    for directory in [&dirs.rust, &dirs.cpp, &dirs.descriptor] {
        fs::create_dir_all(directory).map_err(|source| ProjectError::Io {
            path: directory.to_path_buf(),
            source,
        })?;
    }
    if let Some(python) = &dirs.python {
        fs::create_dir_all(python).map_err(|source| ProjectError::Io {
            path: python.to_path_buf(),
            source,
        })?;
    }
    let previous = read_generation_manifest(project)?;
    for source in &sources {
        generate(&source.schema, &dirs)?;
    }
    let index_path = project.root.join("types/src/lib.rs");
    write_string(&index_path, &module_index(project, &sources))?;
    let manifest = generation_manifest(&sources);
    let expected = manifest
        .sources
        .iter()
        .flat_map(|source| source.outputs.iter().cloned())
        .collect::<BTreeSet<_>>();
    if let Some(previous) = previous {
        for old in previous
            .sources
            .iter()
            .flat_map(|source| source.outputs.iter())
        {
            if expected.contains(old) {
                continue;
            }
            let path = project.root.join(old);
            if path.starts_with(&project.root)
                && (path.starts_with(&dirs.rust)
                    || path.starts_with(&dirs.cpp)
                    || path.starts_with(&dirs.descriptor)
                    || dirs
                        .python
                        .as_ref()
                        .is_some_and(|python| path.starts_with(python)))
            {
                let _ = fs::remove_file(path);
            }
        }
    }
    let contents = serde_json::to_string_pretty(&manifest)? + "\n";
    write_string(&manifest_path(project), &contents)?;
    Ok(manifest)
}

/// Alias for callers using the command terminology.
pub fn generate_project_idl(project: &Project) -> Result<GenerationManifest> {
    generate_project(project)
}

/// Format project IDL deterministically. Returns the number of changed files.
pub fn format_project_idl(project: &Project, check_only: bool) -> Result<usize> {
    let files = project_idl_files(project)?;
    let mut changed = Vec::new();
    let mut formatted = Vec::with_capacity(files.len());
    for path in &files {
        let schema = compile_file(path)?;
        let output = format_idl(&schema);
        let input = read_string(path)?;
        if input != output {
            changed.push(path.clone());
        }
        formatted.push((path.clone(), output));
    }
    if check_only && !changed.is_empty() {
        let names = changed
            .iter()
            .map(|path| path.display().to_string())
            .collect::<Vec<_>>()
            .join(", ");
        return Err(ProjectError::Invalid(format!(
            "{} IDL file(s) need formatting: {names}; run `rbt idl fmt`",
            changed.len()
        )));
    }
    if !check_only {
        for (path, output) in formatted {
            write_string(&path, &output)?;
        }
    }
    Ok(changed.len())
}

/// Alias for callers using the command terminology.
pub fn format_project(project: &Project, check_only: bool) -> Result<usize> {
    format_project_idl(project, check_only)
}

fn idl_location(error: &IdlError) -> Option<(&str, usize, usize)> {
    let location = match error {
        IdlError::Lex { location, .. }
        | IdlError::Parse { location, .. }
        | IdlError::Semantic { location, .. } => location,
        IdlError::Io { .. } | IdlError::Json { .. } => return None,
    };
    Some((&location.filename, location.line, location.column))
}

fn idl_diagnostic(path: &Path, error: &IdlError) -> ProjectDiagnostic {
    let mut diagnostic = ProjectDiagnostic::new(
        "idl_invalid",
        DiagnosticStatus::Fail,
        format!("IDL validation failed for {}", path.display()),
    )
    .details(error.to_string())
    .action("Fix the reported IDL error, then run `rbt idl check` and `rbt idl generate`");
    if let Some((filename, line, column)) = idl_location(error) {
        let located_path = PathBuf::from(filename);
        diagnostic = diagnostic.at(
            if located_path.as_os_str().is_empty() {
                path.to_path_buf()
            } else {
                located_path
            },
            Some(line),
            Some(column),
        );
    } else {
        diagnostic = diagnostic.at(path, None, None);
    }
    diagnostic
}

fn path_executable(path: &Path) -> bool {
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

fn command_available(project: &Project, command: &[String]) -> bool {
    let Some(program) = command.first() else {
        return false;
    };
    let path = Path::new(program);
    if path.components().count() > 1 || path.is_absolute() {
        return path_executable(&absolute_from(path, &project.root));
    }
    env::var_os("PATH")
        .into_iter()
        .flat_map(|value| env::split_paths(&value).collect::<Vec<_>>())
        .map(|directory| directory.join(program))
        .any(|candidate| path_executable(&candidate))
}

fn locate_package_manifest(project: &Project, component_name: &str, package_name: &str) -> PathBuf {
    let conventional = project
        .root
        .join("components")
        .join(component_name)
        .join("Cargo.toml");
    if conventional.is_file() {
        return conventional;
    }
    let components = project.root.join("components");
    let mut candidates = Vec::new();
    if let Ok(entries) = fs::read_dir(&components) {
        for entry in entries.flatten() {
            let path = entry.path();
            if path.is_dir() {
                let manifest = path.join("Cargo.toml");
                if manifest.is_file() {
                    candidates.push(manifest);
                }
            }
        }
    }
    candidates.sort();
    for manifest in candidates {
        let matches = read_string(&manifest)
            .ok()
            .and_then(|source| toml::from_str::<toml::Value>(&source).ok())
            .and_then(|value| {
                value
                    .get("package")
                    .and_then(|package| package.get("name"))
                    .and_then(toml::Value::as_str)
                    .map(|name| name == package_name)
            })
            .unwrap_or(false);
        if matches {
            return manifest;
        }
    }
    conventional
}

fn component_diagnostics(project: &Project) -> Vec<ProjectDiagnostic> {
    let mut diagnostics = Vec::new();
    let mut names = BTreeSet::new();
    for component in &project.config.components {
        if !names.insert(component.name.clone()) {
            diagnostics.push(
                ProjectDiagnostic::new(
                    "component_duplicate",
                    DiagnosticStatus::Fail,
                    format!(
                        "component name {:?} is declared more than once",
                        component.name
                    ),
                )
                .at(&project.manifest_path, None, None)
                .action("Keep one [[component]] entry for each component name"),
            );
        }
        if let Err(error) = validate_kebab_identifier(&component.name, "component name") {
            diagnostics.push(
                ProjectDiagnostic::new(
                    "component_name_invalid",
                    DiagnosticStatus::Fail,
                    error.to_string(),
                )
                .at(&project.manifest_path, None, None),
            );
        }
        let package = component.package.is_some();
        let command = component.command.is_some();
        if package == command {
            diagnostics.push(
                ProjectDiagnostic::new(
                    "component_command_shape",
                    DiagnosticStatus::Fail,
                    format!(
                        "component {:?} must specify exactly one of package or command",
                        component.name
                    ),
                )
                .at(&project.manifest_path, None, None)
                .action("Set `package = \"...\"` for a workspace package or `command = [...]` for an external executable"),
            );
        }
        if let Some(package) = &component.package {
            let package_manifest = locate_package_manifest(project, &component.name, package);
            if !package_manifest.is_file() {
                diagnostics.push(
                    ProjectDiagnostic::new(
                        "component_package_missing",
                        DiagnosticStatus::Fail,
                        format!("Rust package {package:?} is missing its manifest"),
                    )
                    .at(&package_manifest, None, None)
                    .action("Create components/<name>/Cargo.toml or use an explicit command"),
                );
            }
            let source = package_manifest
                .parent()
                .map(|parent| parent.join("src/main.rs"))
                .unwrap_or_else(|| {
                    project
                        .root
                        .join("components")
                        .join(&component.name)
                        .join("src/main.rs")
                });
            if !source.is_file() {
                diagnostics.push(
                    ProjectDiagnostic::new(
                        "component_source_missing",
                        DiagnosticStatus::Fail,
                        format!("Rust package {package:?} has no src/main.rs"),
                    )
                    .at(source, None, None)
                    .action("Add the component entry point or update the package configuration"),
                );
            }
            if package_manifest.is_file() {
                match read_string(&package_manifest).and_then(|source| {
                    toml::from_str::<toml::Value>(&source).map_err(|error| {
                        ProjectError::Invalid(format!("invalid Cargo.toml: {error}"))
                    })
                }) {
                    Ok(manifest) => {
                        let actual_name = manifest
                            .get("package")
                            .and_then(|value| value.get("name"))
                            .and_then(toml::Value::as_str);
                        if actual_name != Some(package.as_str()) {
                            diagnostics.push(
                                ProjectDiagnostic::new(
                                    "component_package_name_mismatch",
                                    DiagnosticStatus::Fail,
                                    format!(
                                        "component {:?} declares package {:?}, but Cargo.toml names {:?}",
                                        component.name,
                                        package,
                                        actual_name.unwrap_or("<missing>"),
                                    ),
                                )
                                .at(&package_manifest, None, None)
                                .action("Make [package].name match the rbt.toml package field"),
                            );
                        }
                        if let Some(version) = manifest
                            .get("package")
                            .and_then(|value| value.get("version"))
                            .and_then(toml::Value::as_str)
                            && version != project.config.project.sdk_version
                        {
                            diagnostics.push(
                                ProjectDiagnostic::new(
                                    "component_package_version_mismatch",
                                    DiagnosticStatus::Fail,
                                    format!(
                                        "component package version {version:?} does not match project SDK {}",
                                        project.config.project.sdk_version
                                    ),
                                )
                                .at(&package_manifest, None, None)
                                .action("Regenerate the component with the matching rbt CLI/SDK version"),
                            );
                        }
                        if let Some(dependency) = manifest
                            .get("dependencies")
                            .and_then(|value| value.get("rbt"))
                        {
                            if let Some(version) =
                                dependency.get("version").and_then(toml::Value::as_str)
                                && version != env!("CARGO_PKG_VERSION")
                            {
                                diagnostics.push(
                                    ProjectDiagnostic::new(
                                        "component_sdk_version_mismatch",
                                        DiagnosticStatus::Fail,
                                        format!(
                                            "rbt dependency version {version:?} does not match CLI {}",
                                            env!("CARGO_PKG_VERSION")
                                        ),
                                    )
                                    .at(&package_manifest, None, None)
                                    .action("Use an rbt SDK dependency matching the installed CLI"),
                                );
                            }
                            let dependency_path = dependency
                                .get("path")
                                .and_then(toml::Value::as_str)
                                .map(PathBuf::from);
                            if let Some(dependency_path) = dependency_path {
                                let resolved = absolute_from(
                                    &dependency_path,
                                    package_manifest.parent().unwrap_or(&project.root),
                                );
                                if !resolved.join("Cargo.toml").is_file() {
                                    diagnostics.push(
                                        ProjectDiagnostic::new(
                                            "component_sdk_path_missing",
                                            DiagnosticStatus::Fail,
                                            format!(
                                                "rbt dependency path {} does not contain Cargo.toml",
                                                resolved.display()
                                            ),
                                        )
                                        .at(&package_manifest, None, None)
                                        .action("Set the rbt dependency path to the installed SDK's crates/rbt directory"),
                                    );
                                }
                            }
                        }
                    }
                    Err(error) => diagnostics.push(
                        ProjectDiagnostic::new(
                            "component_package_manifest_invalid",
                            DiagnosticStatus::Fail,
                            error.to_string(),
                        )
                        .at(&package_manifest, None, None)
                        .action("Fix the component Cargo.toml syntax"),
                    ),
                }
            }
        }
        if let Some(command) = &component.command {
            if command.is_empty() {
                diagnostics.push(
                    ProjectDiagnostic::new(
                        "component_command_missing",
                        DiagnosticStatus::Fail,
                        format!("component {:?} has an empty command", component.name),
                    )
                    .at(&project.manifest_path, None, None)
                    .action("Provide an executable as the first command argument"),
                );
            } else if !command_available(project, command) {
                diagnostics.push(
                    ProjectDiagnostic::new(
                        "component_executable_missing",
                        DiagnosticStatus::Fail,
                        format!("executable {:?} for component {:?} was not found", command[0], component.name),
                    )
                    .at(&project.manifest_path, None, None)
                    .action("Build the executable, use an absolute/relative path, or install it on PATH"),
                );
            }
        }
    }
    diagnostics
}

fn types_package_diagnostics(project: &Project) -> Vec<ProjectDiagnostic> {
    let mut diagnostics = Vec::new();
    let manifest_path = project.root.join("types/Cargo.toml");
    if !manifest_path.is_file() {
        diagnostics.push(
            ProjectDiagnostic::new(
                "types_package_missing",
                DiagnosticStatus::Fail,
                format!(
                    "generated types package manifest {} is missing",
                    manifest_path.display()
                ),
            )
            .at(&manifest_path, None, None)
            .action("Create the types package or run `rbt new project`"),
        );
        return diagnostics;
    }
    match read_string(&manifest_path).and_then(|source| {
        toml::from_str::<toml::Value>(&source)
            .map_err(|error| ProjectError::Invalid(format!("invalid Cargo.toml: {error}")))
    }) {
        Ok(manifest) => {
            let expected_name = format!("{}-types", project.config.project.name);
            let actual_name = manifest
                .get("package")
                .and_then(|value| value.get("name"))
                .and_then(toml::Value::as_str);
            if actual_name != Some(expected_name.as_str()) {
                diagnostics.push(
                    ProjectDiagnostic::new(
                        "types_package_name_mismatch",
                        DiagnosticStatus::Fail,
                        format!(
                            "types package is named {:?}; expected {:?}",
                            actual_name.unwrap_or("<missing>"),
                            expected_name
                        ),
                    )
                    .at(&manifest_path, None, None)
                    .action("Make types/Cargo.toml package name match <project>-types"),
                );
            }
            if let Some(version) = manifest
                .get("package")
                .and_then(|value| value.get("version"))
                .and_then(toml::Value::as_str)
                && version != project.config.project.sdk_version
            {
                diagnostics.push(
                    ProjectDiagnostic::new(
                        "types_package_version_mismatch",
                        DiagnosticStatus::Fail,
                        format!(
                            "types package version {version:?} does not match project SDK {}",
                            project.config.project.sdk_version
                        ),
                    )
                    .at(&manifest_path, None, None)
                    .action("Regenerate the types package with the matching rbt CLI/SDK version"),
                );
            }
            if let Some(dependency) = manifest
                .get("dependencies")
                .and_then(|value| value.get("rbt-types"))
            {
                if let Some(version) = dependency.get("version").and_then(toml::Value::as_str)
                    && version != env!("CARGO_PKG_VERSION")
                {
                    diagnostics.push(
                        ProjectDiagnostic::new(
                            "types_sdk_version_mismatch",
                            DiagnosticStatus::Fail,
                            format!(
                                "rbt-types dependency version {version:?} does not match CLI {}",
                                env!("CARGO_PKG_VERSION")
                            ),
                        )
                        .at(&manifest_path, None, None)
                        .action("Use an rbt-types SDK dependency matching the installed CLI"),
                    );
                }
                if let Some(dependency_path) = dependency
                    .get("path")
                    .and_then(toml::Value::as_str)
                    .map(PathBuf::from)
                {
                    let resolved = absolute_from(
                        &dependency_path,
                        manifest_path.parent().unwrap_or(&project.root),
                    );
                    if !resolved.join("Cargo.toml").is_file() {
                        diagnostics.push(
                            ProjectDiagnostic::new(
                                "types_sdk_path_missing",
                                DiagnosticStatus::Fail,
                                format!(
                                    "rbt-types dependency path {} does not contain Cargo.toml",
                                    resolved.display()
                                ),
                            )
                            .at(&manifest_path, None, None)
                            .action(
                                "Set rbt-types to the installed SDK's crates/rbt-types directory",
                            ),
                        );
                    }
                }
            }
        }
        Err(error) => diagnostics.push(
            ProjectDiagnostic::new(
                "types_package_manifest_invalid",
                DiagnosticStatus::Fail,
                error.to_string(),
            )
            .at(&manifest_path, None, None)
            .action("Fix types/Cargo.toml syntax"),
        ),
    }
    diagnostics
}

fn expected_manifest(project: &Project, sources: &[CompiledSource]) -> GenerationManifest {
    let _ = project;
    generation_manifest(sources)
}

fn generation_diagnostics(project: &Project, sources: &[CompiledSource]) -> Vec<ProjectDiagnostic> {
    let mut diagnostics = Vec::new();
    let marker_path = manifest_path(project);
    let Some(raw_marker) = fs::read_to_string(&marker_path).ok() else {
        diagnostics.push(
            ProjectDiagnostic::new(
                "idl_generated_missing",
                DiagnosticStatus::Fail,
                format!(
                    "generated-file manifest {} is missing",
                    marker_path.display()
                ),
            )
            .at(&marker_path, None, None)
            .action("Run `rbt idl generate`"),
        );
        return diagnostics;
    };
    let marker = match serde_json::from_str::<GenerationManifest>(&raw_marker) {
        Ok(marker) => marker,
        Err(error) => {
            diagnostics.push(
                ProjectDiagnostic::new(
                    "idl_generated_manifest_invalid",
                    DiagnosticStatus::Fail,
                    format!("generated-file manifest is invalid: {error}"),
                )
                .at(&marker_path, None, None)
                .action("Run `rbt idl generate` to replace the generated metadata"),
            );
            return diagnostics;
        }
    };
    let expected = expected_manifest(project, sources);
    if marker.schema_version != GENERATION_MANIFEST_VERSION {
        diagnostics.push(
            ProjectDiagnostic::new(
                "idl_generator_manifest_version",
                DiagnosticStatus::Fail,
                format!(
                    "generation manifest schema {} is not supported (expected {})",
                    marker.schema_version, GENERATION_MANIFEST_VERSION
                ),
            )
            .at(&marker_path, None, None)
            .action("Run `rbt idl generate` with the current CLI"),
        );
    }
    if marker.generator_version != expected.generator_version {
        diagnostics.push(
            ProjectDiagnostic::new(
                "idl_generator_version",
                DiagnosticStatus::Fail,
                format!(
                    "generated files use generator {}, but this CLI is {}",
                    marker.generator_version, expected.generator_version
                ),
            )
            .at(&marker_path, None, None)
            .action("Run `rbt idl generate` with the current CLI"),
        );
    }
    if marker.sources != expected.sources {
        let changed = marker
            .sources
            .iter()
            .map(|entry| entry.source.clone())
            .chain(expected.sources.iter().map(|entry| entry.source.clone()))
            .collect::<BTreeSet<_>>();
        diagnostics.push(
            ProjectDiagnostic::new(
                "idl_stale",
                DiagnosticStatus::Fail,
                format!(
                    "generated metadata is stale for {} source path(s)",
                    changed.len()
                ),
            )
            .at(&marker_path, None, None)
            .action("Run `rbt idl generate` after editing IDL or changing generator settings"),
        );
    }
    for source in &expected.sources {
        for output in &source.outputs {
            let path = project.root.join(output);
            if !path.is_file() {
                diagnostics.push(
                    ProjectDiagnostic::new(
                        "idl_generated_missing",
                        DiagnosticStatus::Fail,
                        format!("generated output {} is missing", path.display()),
                    )
                    .at(path, None, None)
                    .action("Run `rbt idl generate`"),
                );
            }
        }
    }
    let index_path = project.root.join("types/src/lib.rs");
    let expected_index = module_index(project, sources);
    match fs::read_to_string(&index_path) {
        Ok(actual) if actual == expected_index => {}
        Ok(_) => diagnostics.push(
            ProjectDiagnostic::new(
                "idl_module_index_stale",
                DiagnosticStatus::Fail,
                format!(
                    "generated types module index {} is stale",
                    index_path.display()
                ),
            )
            .at(&index_path, None, None)
            .action("Run `rbt idl generate`"),
        ),
        Err(_) => diagnostics.push(
            ProjectDiagnostic::new(
                "idl_module_index_missing",
                DiagnosticStatus::Fail,
                format!(
                    "generated types module index {} is missing",
                    index_path.display()
                ),
            )
            .at(&index_path, None, None)
            .action("Run `rbt idl generate`"),
        ),
    }
    diagnostics
}

/// Run fast static checks without starting a Composer or any component.
pub fn check_project(project: &Project) -> Result<ProjectCheckReport> {
    let mut diagnostics = Vec::new();
    if project.config.schema_version != PROJECT_SCHEMA_VERSION {
        diagnostics.push(
            ProjectDiagnostic::new(
                "project_schema_version",
                DiagnosticStatus::Fail,
                format!(
                    "project schema {} is not supported (expected {})",
                    project.config.schema_version, PROJECT_SCHEMA_VERSION
                ),
            )
            .at(&project.manifest_path, None, None)
            .action("Regenerate or update rbt.toml with the current CLI"),
        );
    }
    if project.config.project.name != project.config.project.name.trim() {
        diagnostics.push(
            ProjectDiagnostic::new(
                "project_name_invalid",
                DiagnosticStatus::Fail,
                "project name contains surrounding whitespace",
            )
            .at(&project.manifest_path, None, None),
        );
    } else if let Err(error) =
        validate_kebab_identifier(&project.config.project.name, "project name")
    {
        diagnostics.push(
            ProjectDiagnostic::new(
                "project_name_invalid",
                DiagnosticStatus::Fail,
                error.to_string(),
            )
            .at(&project.manifest_path, None, None),
        );
    }
    if project.config.project.sdk_version != env!("CARGO_PKG_VERSION") {
        diagnostics.push(
            ProjectDiagnostic::new(
                "sdk_version_mismatch",
                DiagnosticStatus::Fail,
                format!(
                    "project expects SDK {}, but this CLI is {}",
                    project.config.project.sdk_version,
                    env!("CARGO_PKG_VERSION")
                ),
            )
            .at(&project.manifest_path, None, None)
            .action("Regenerate the project with the matching rbt CLI/SDK"),
        );
    }
    if project.config.build.command.is_empty() {
        diagnostics.push(
            ProjectDiagnostic::new(
                "build_command_missing",
                DiagnosticStatus::Fail,
                "[build].command is empty",
            )
            .at(&project.manifest_path, None, None)
            .action("Set [build].command to an executable build command, for example [\"cargo\", \"build\", \"--workspace\"]"),
        );
    } else if !command_available(project, &project.config.build.command) {
        diagnostics.push(
            ProjectDiagnostic::new(
                "build_executable_missing",
                DiagnosticStatus::Fail,
                format!(
                    "build executable {:?} was not found",
                    project.config.build.command[0]
                ),
            )
            .at(&project.manifest_path, None, None)
            .action("Install the build tool, put it on PATH, or use an executable path"),
        );
    }
    let idl_paths_valid = match validate_idl_paths(project) {
        Ok(()) => true,
        Err(error) => {
            diagnostics.push(
                ProjectDiagnostic::new(
                    "idl_path_invalid",
                    DiagnosticStatus::Fail,
                    error.to_string(),
                )
                .at(&project.manifest_path, None, None)
                .action("Use project-relative IDL source and output paths without absolute roots or `..`"),
            );
            false
        }
    };
    match resolve_sdk_root(None) {
        Ok(sdk_root) => {
            if let Some(version) = installed_sdk_version(&sdk_root) {
                if version != project.config.project.sdk_version {
                    diagnostics.push(
                        ProjectDiagnostic::new(
                            "sdk_install_version_mismatch",
                            DiagnosticStatus::Fail,
                            format!(
                                "installed SDK is version {version}, but project expects {}",
                                project.config.project.sdk_version
                            ),
                        )
                        .at(sdk_root.join("Cargo.toml"), None, None)
                        .action("Use an installed SDK matching the project and CLI version"),
                    );
                } else {
                    diagnostics.push(ProjectDiagnostic::new(
                        "sdk_available",
                        DiagnosticStatus::Pass,
                        "Rust SDK is available",
                    ));
                }
            } else {
                diagnostics.push(
                    ProjectDiagnostic::new(
                        "sdk_version_unreadable",
                        DiagnosticStatus::Fail,
                        format!(
                            "could not read the installed SDK version at {}",
                            sdk_root.display()
                        ),
                    )
                    .at(sdk_root.join("Cargo.toml"), None, None)
                    .action("Install a complete rbt SDK"),
                );
            }
        }
        Err(error) => diagnostics.push(
            ProjectDiagnostic::new(
                "sdk_unavailable",
                DiagnosticStatus::Fail,
                "Rust SDK is unavailable",
            )
            .details(error.to_string())
            .action("Install rbt or set RBT_RUST_SDK to the installed SDK root"),
        ),
    }
    diagnostics.extend(component_diagnostics(project));
    if project
        .config
        .components
        .iter()
        .any(|component| component.package.is_some())
    {
        diagnostics.extend(types_package_diagnostics(project));
    }
    let mut compiled = Vec::new();
    let idl_compile_valid = match compile_project_sources(project) {
        Ok(sources) => {
            compiled = sources;
            diagnostics.push(ProjectDiagnostic::new(
                "idl_valid",
                DiagnosticStatus::Pass,
                "all IDL sources compile",
            ));
            true
        }
        Err(ProjectError::Idl(error)) => {
            let fallback = project_idl_files(project)
                .ok()
                .and_then(|files| files.first().cloned())
                .unwrap_or_else(|| project.root.join("types"));
            diagnostics.push(idl_diagnostic(&fallback, &error));
            false
        }
        Err(error) => {
            diagnostics.push(
                ProjectDiagnostic::new("idl_invalid", DiagnosticStatus::Fail, error.to_string())
                    .action("Fix the IDL source paths, then run `rbt idl check`"),
            );
            false
        }
    };
    if idl_paths_valid && idl_compile_valid {
        diagnostics.extend(generation_diagnostics(project, &compiled));
    }
    let valid = diagnostics
        .iter()
        .all(|diagnostic| diagnostic.status != DiagnosticStatus::Fail);
    Ok(ProjectCheckReport {
        project_root: project.root.clone(),
        manifest: project.manifest_path.clone(),
        valid,
        diagnostics,
    })
}

/// Discover a project and run its static checks.
pub fn check_project_at(
    start: impl AsRef<Path>,
    explicit: Option<&Path>,
) -> Result<ProjectCheckReport> {
    let manifest = discover_project(start, explicit)?;
    let project = load_project(manifest)?;
    check_project(&project)
}

/// Render stable human-readable diagnostics. JSON callers should serialize the report directly.
pub fn format_check_report(report: &ProjectCheckReport) -> String {
    let mut output = String::new();
    output.push_str(if report.valid {
        "PASS project check\n"
    } else {
        "FAIL project check\n"
    });
    for diagnostic in &report.diagnostics {
        let status = match diagnostic.status {
            DiagnosticStatus::Pass => "PASS",
            DiagnosticStatus::Warn => "WARN",
            DiagnosticStatus::Fail => "FAIL",
            DiagnosticStatus::Skip => "SKIP",
        };
        output.push_str(&format!(
            "{status} {}: {}\n",
            diagnostic.check_id, diagnostic.summary
        ));
        if let Some(path) = &diagnostic.path {
            output.push_str(&format!("  at {}", path.display()));
            if let Some(line) = diagnostic.line {
                output.push_str(&format!(":{line}"));
                if let Some(column) = diagnostic.column {
                    output.push_str(&format!(":{column}"));
                }
            }
            output.push('\n');
        }
        if let Some(details) = &diagnostic.details {
            output.push_str(&format!("  {details}\n"));
        }
        if let Some(action) = &diagnostic.suggested_action {
            output.push_str(&format!("  suggestion: {action}\n"));
        }
    }
    output
}

/// Backwards-compatible name for command/UI formatters.
pub fn format_project_check(report: &ProjectCheckReport) -> String {
    format_check_report(report)
}

/// Alias used by CLI frontends that call human output a renderer.
pub fn render_check_report(report: &ProjectCheckReport) -> String {
    format_check_report(report)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    struct TempTree {
        path: PathBuf,
    }

    impl TempTree {
        fn new(label: &str) -> Self {
            let stamp = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .expect("clock is after epoch")
                .as_nanos();
            let path =
                env::temp_dir().join(format!("rbt-cli-{label}-{}-{stamp}", std::process::id()));
            fs::create_dir_all(&path).expect("create temp tree");
            Self { path }
        }
    }

    impl Drop for TempTree {
        fn drop(&mut self) {
            let _ = fs::remove_dir_all(&self.path);
        }
    }

    fn fake_sdk(tree: &TempTree) -> PathBuf {
        let sdk = tree.path.join("installed sdk");
        for relative in [
            "crates/rbt",
            "crates/rbt-types",
            "third_party/iceoryx2-rbt/source",
            "third_party/iceoryx2-rbt/source/iceoryx2",
        ] {
            fs::create_dir_all(sdk.join(relative)).expect("create fake sdk crate");
            fs::write(
                sdk.join(relative).join("Cargo.toml"),
                "[package]\nname = \"rbt\"\n",
            )
            .expect("write fake sdk manifest");
        }
        fs::write(
            sdk.join("Cargo.toml"),
            "[workspace]\nmembers = [\"crates/*\"]\n",
        )
        .expect("write fake sdk workspace");
        sdk
    }

    #[test]
    fn partial_sdk_is_rejected_before_project_creation() {
        let tree = TempTree::new("partial sdk");
        let sdk = fake_sdk(&tree);
        for (index, missing) in [
            "third_party/iceoryx2-rbt/source/Cargo.toml",
            "third_party/iceoryx2-rbt/source/iceoryx2/Cargo.toml",
        ]
        .into_iter()
        .enumerate()
        {
            let manifest = sdk.join(missing);
            fs::remove_file(&manifest).expect("remove bundled source manifest");
            let project_name = format!("partial-sdk-{index}");
            let error = create_project(&tree.path, &project_name, Some(&sdk))
                .expect_err("partial SDK must be rejected");
            assert!(error.to_string().contains(&manifest.display().to_string()));
            assert!(!tree.path.join(project_name).exists());
            fs::write(&manifest, "[package]\nname = \"fake\"\n")
                .expect("restore bundled source manifest");
        }
    }

    #[test]
    fn scaffold_and_discover_project_below_space_path() {
        let tree = TempTree::new("space path");
        let sdk = fake_sdk(&tree);
        let base = tree.path.join("parent with spaces").join("nested");
        fs::create_dir_all(&base).expect("create base");
        let project = create_project(&base, "hello-robot", Some(&sdk)).expect("scaffold");
        assert!(project.manifest_path.is_file());
        assert!(
            project
                .root
                .join("generated/generation-manifest.json")
                .is_file()
        );
        assert!(
            project
                .root
                .join("components/hello-source/Cargo.toml")
                .is_file()
        );
        assert!(
            project
                .root
                .join("components/hello-sink/src/main.rs")
                .is_file()
        );
        assert!(project.root.join("types/Cargo.toml").is_file());
        let nested = project.root.join("components/hello-source/src");
        assert_eq!(
            discover_project(&nested, None).expect("discover upward"),
            project.manifest_path
        );
        for manifest in [
            project.root.join("Cargo.toml"),
            project.root.join("types/Cargo.toml"),
            project.root.join("components/hello-source/Cargo.toml"),
            project.root.join("components/hello-sink/Cargo.toml"),
        ] {
            let source = read_string(&manifest).expect("read generated manifest");
            toml::from_str::<toml::Value>(&source).expect("generated Cargo.toml syntax");
            assert!(
                !source.contains("{{"),
                "unexpanded placeholder in {}",
                manifest.display()
            );
        }
        let root_manifest = toml::from_str::<toml::Value>(
            &read_string(&project.root.join("Cargo.toml")).expect("read generated workspace"),
        )
        .expect("generated workspace Cargo.toml syntax");
        let expected_iox2_source = sdk.join("third_party/iceoryx2-rbt/source/iceoryx2");
        let expected_iox2_source = expected_iox2_source.to_string_lossy();
        assert_eq!(
            root_manifest["patch"]["crates-io"]["iceoryx2"]["path"].as_str(),
            Some(expected_iox2_source.as_ref()),
        );
        let types_manifest = toml::from_str::<toml::Value>(
            &read_string(&project.root.join("types/Cargo.toml")).expect("read generated types"),
        )
        .expect("generated types Cargo.toml syntax");
        assert_eq!(
            types_manifest["package"]["rust-version"].as_str(),
            Some("1.89"),
        );
        assert_eq!(
            types_manifest["dependencies"]["iceoryx2"]["version"].as_str(),
            Some("=0.10.0"),
        );
        assert_eq!(
            types_manifest["dependencies"]["iceoryx2"]["path"].as_str(),
            Some(expected_iox2_source.as_ref()),
        );
        for component_manifest in [
            project.root.join("components/hello-source/Cargo.toml"),
            project.root.join("components/hello-sink/Cargo.toml"),
        ] {
            let component = toml::from_str::<toml::Value>(
                &read_string(&component_manifest).expect("read generated component"),
            )
            .expect("generated component Cargo.toml syntax");
            assert_eq!(component["package"]["rust-version"].as_str(), Some("1.89"),);
        }
        assert!(project.root.join("generated/rust").is_dir());
        assert!(
            !read_string(&project.root.join(".gitignore"))
                .expect("read generated gitignore")
                .contains("generated")
        );
    }

    #[test]
    fn scaffold_refuses_overwrite_and_duplicate_names() {
        let tree = TempTree::new("duplicates");
        let sdk = fake_sdk(&tree);
        let project = create_project(&tree.path, "demo", Some(&sdk)).expect("scaffold");
        assert!(create_project(&tree.path, "demo", Some(&sdk)).is_err());
        let project = add_component(&project, "detector", Some(&sdk)).expect("component");
        let component_manifest = toml::from_str::<toml::Value>(
            &read_string(&project.root.join("components/detector/Cargo.toml"))
                .expect("read added component"),
        )
        .expect("added component Cargo.toml syntax");
        assert_eq!(
            component_manifest["package"]["rust-version"].as_str(),
            Some("1.89"),
        );
        assert!(add_component(&project, "detector", Some(&sdk)).is_err());
        let project = add_message(&project, "Detection").expect("message");
        assert!(add_message(&project, "Detection").is_err());
    }

    #[test]
    fn generation_is_deterministic_and_stale_is_reported() {
        let tree = TempTree::new("generation");
        let sdk = fake_sdk(&tree);
        let project = create_project(&tree.path, "demo", Some(&sdk)).expect("scaffold");
        let project = add_message(&project, "Detection").expect("message");
        let first_marker = read_string(&manifest_path(&project)).expect("marker");
        let first_rust =
            read_string(&project.root.join("generated/rust/detection.rs")).expect("rust output");
        generate_project(&project).expect("repeat generation");
        assert_eq!(
            first_marker,
            read_string(&manifest_path(&project)).expect("marker stable")
        );
        assert_eq!(
            first_rust,
            read_string(&project.root.join("generated/rust/detection.rs")).expect("rust stable")
        );
        let source_path = project.root.join("types/detection.rbt");
        let mut source = read_string(&source_path).expect("source");
        source.push_str("\n// changed after generation\n");
        write_string(&source_path, &source).expect("change source");
        let report = check_project(&project).expect("check report");
        assert!(!report.valid);
        assert!(
            report
                .diagnostics
                .iter()
                .any(|diagnostic| diagnostic.check_id == "idl_stale")
        );
    }

    #[test]
    fn output_stem_collisions_are_rejected() {
        let tree = TempTree::new("collision");
        let sdk = fake_sdk(&tree);
        let project = create_project(&tree.path, "demo", Some(&sdk)).expect("scaffold");
        write_string(
            &project.root.join("types/first.rbt"),
            "package one.detection;\nabi 1;\nmessage First { value: u64; }\n",
        )
        .expect("first schema");
        write_string(
            &project.root.join("types/second.rbt"),
            "package two.detection;\nabi 1;\nmessage Second { value: u64; }\n",
        )
        .expect("second schema");
        let error = generate_project(&project).expect_err("collision");
        assert!(error.to_string().contains("output stem collision"));
    }

    #[test]
    fn unsafe_project_paths_are_rejected_before_generation() {
        let tree = TempTree::new("paths");
        let sdk = fake_sdk(&tree);
        let project = create_project(&tree.path, "demo", Some(&sdk)).expect("scaffold");
        let mut config = project.config.clone();
        config.idl.rust = PathBuf::from("../outside");
        let unsafe_project = Project {
            root: project.root.clone(),
            manifest_path: project.manifest_path.clone(),
            config,
        };
        let report = check_project(&unsafe_project).expect("check report");
        assert!(
            report
                .diagnostics
                .iter()
                .any(|diagnostic| diagnostic.check_id == "idl_path_invalid")
        );
        assert!(generate_project(&unsafe_project).is_err());
    }
}

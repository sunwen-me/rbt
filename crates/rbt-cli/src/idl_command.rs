//! Standalone IDL command helpers used when explicit paths are supplied.

use std::{
    fs,
    path::{Path, PathBuf},
};

use rbt_idl::{compile_file, format_idl};

#[derive(Debug, thiserror::Error)]
pub enum IdlCommandError {
    #[error(transparent)]
    Idl(#[from] rbt_idl::Error),
    #[error("{path}: {source}")]
    Io {
        path: PathBuf,
        #[source]
        source: std::io::Error,
    },
    #[error("no .rbt files found under {0}")]
    Empty(String),
    #[error("{count} IDL file(s) need formatting: {paths}; run `rbt idl fmt {hint}`")]
    NeedsFormatting {
        count: usize,
        paths: String,
        hint: String,
    },
}

pub type Result<T> = std::result::Result<T, IdlCommandError>;

/// Expand files/directories recursively in deterministic path order.
pub fn collect_files(paths: &[PathBuf]) -> Result<Vec<PathBuf>> {
    let mut files = Vec::new();
    for path in paths {
        collect_path(path, &mut files)?;
    }
    files.sort();
    files.dedup();
    if files.is_empty() {
        return Err(IdlCommandError::Empty(
            paths
                .iter()
                .map(|path| path.display().to_string())
                .collect::<Vec<_>>()
                .join(", "),
        ));
    }
    Ok(files)
}

fn collect_path(path: &Path, files: &mut Vec<PathBuf>) -> Result<()> {
    let metadata = fs::metadata(path).map_err(|source| IdlCommandError::Io {
        path: path.to_path_buf(),
        source,
    })?;
    if metadata.is_file() {
        if path.extension().is_some_and(|extension| extension == "rbt") {
            files.push(path.to_path_buf());
        }
        return Ok(());
    }
    if !metadata.is_dir() {
        return Ok(());
    }
    let mut children = fs::read_dir(path)
        .map_err(|source| IdlCommandError::Io {
            path: path.to_path_buf(),
            source,
        })?
        .map(|entry| {
            entry
                .map(|entry| entry.path())
                .map_err(|source| IdlCommandError::Io {
                    path: path.to_path_buf(),
                    source,
                })
        })
        .collect::<Result<Vec<_>>>()?;
    children.sort();
    for child in children {
        collect_path(&child, files)?;
    }
    Ok(())
}

pub fn check(paths: &[PathBuf]) -> Result<usize> {
    let files = collect_files(paths)?;
    for path in &files {
        compile_file(path)?;
    }
    Ok(files.len())
}

pub fn format(paths: &[PathBuf], check_only: bool) -> Result<usize> {
    let files = collect_files(paths)?;
    let mut changes = Vec::new();
    for path in &files {
        let expected = format_idl(&compile_file(path)?);
        let current = fs::read_to_string(path).map_err(|source| IdlCommandError::Io {
            path: path.clone(),
            source,
        })?;
        if current != expected {
            changes.push((path.clone(), expected));
        }
    }
    if check_only && !changes.is_empty() {
        return Err(IdlCommandError::NeedsFormatting {
            count: changes.len(),
            paths: changes
                .iter()
                .map(|(path, _)| path.display().to_string())
                .collect::<Vec<_>>()
                .join(", "),
            hint: paths
                .iter()
                .map(|path| path.display().to_string())
                .collect::<Vec<_>>()
                .join(" "),
        });
    }
    if !check_only {
        for (path, expected) in &changes {
            fs::write(path, expected).map_err(|source| IdlCommandError::Io {
                path: path.clone(),
                source,
            })?;
        }
    }
    Ok(changes.len())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicU64, Ordering};

    static NEXT: AtomicU64 = AtomicU64::new(0);

    fn temporary_directory() -> PathBuf {
        let path = std::env::temp_dir().join(format!(
            "rbt-cli-idl-{}-{}",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir_all(&path).unwrap();
        path
    }

    #[test]
    fn recursively_checks_and_formats_paths_with_spaces() {
        let root = temporary_directory().join("schema source");
        fs::create_dir_all(root.join("nested")).unwrap();
        let path = root.join("nested/example.rbt");
        fs::write(
            &path,
            "package demo.example; abi 1; message Example { value: u64; }\n",
        )
        .unwrap();
        assert_eq!(check(std::slice::from_ref(&root)).unwrap(), 1);
        assert!(format(std::slice::from_ref(&root), true).is_err());
        assert_eq!(format(std::slice::from_ref(&root), false).unwrap(), 1);
        assert_eq!(format(std::slice::from_ref(&root), true).unwrap(), 0);
        fs::remove_dir_all(root.parent().unwrap()).unwrap();
    }
}

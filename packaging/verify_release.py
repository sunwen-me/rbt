#!/usr/bin/env python3
"""Verify release archives as installed packages from a relocated prefix."""

from __future__ import annotations

import argparse
import base64
import csv
import hashlib
import io
import json
import os
import posixpath
import re
import shutil
import signal
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import tomllib
import zipfile
from pathlib import Path, PurePosixPath
from typing import Any, Callable, Iterable


PRODUCT_VERSION = "0.8.0"
NATIVE_VERSION = "0.10.0-rbt.2"
SOURCE_FORBIDDEN_DIRS = {
    ".git",
    ".hg",
    ".svn",
    ".pytest_cache",
    ".mypy_cache",
    ".ruff_cache",
    ".tox",
    ".venv",
    "__pycache__",
    "build",
    "node_modules",
    "target",
}
SOURCE_FORBIDDEN_SUFFIXES = {".a", ".bin", ".dylib", ".o", ".pyc", ".pyd", ".so"}
SOURCE_REQUIRED_FILES = (
    "Cargo.toml",
    "Cargo.lock",
    "LICENSE-APACHE",
    "LICENSE-MIT",
    "THIRD_PARTY_NOTICES.md",
    "types/tf.rbt",
    "crates/rbt/Cargo.toml",
    "crates/rbt-cli/src/main.rs",
    "crates/rbt-idl/Cargo.toml",
    "crates/rbt-idl/src/main.rs",
    "generated/cpp/rbt/tf.hpp",
    "generated/descriptors/tf.json",
    "generated/rust/tf.rs",
    "third_party/iceoryx2-rbt/UPSTREAM.toml",
    "third_party/iceoryx2-rbt/SOURCE_DIFF_SHA256",
    "third_party/iceoryx2-rbt/patches/series",
    "third_party/iceoryx2-rbt/patches/SHA256SUMS",
    "third_party/iceoryx2-rbt/source/Cargo.toml",
    "third_party/iceoryx2-rbt/source/Cargo.lock",
    "third_party/iceoryx2-rbt/source/iceoryx2/Cargo.toml",
    "third_party/iceoryx2-rbt/source/LICENSE-APACHE",
    "third_party/iceoryx2-rbt/source/LICENSE-MIT",
)


class VerificationError(RuntimeError):
    """Raised when a release artifact violates the verification contract."""


def _archive_path(name: str) -> Path:
    if "\x00" in name or name.startswith(("/", "\\")) or re.match(r"^[A-Za-z]:", name):
        raise VerificationError(f"unsafe absolute archive member: {name!r}")
    normalized = name.replace("\\", "/")
    parts = [part for part in normalized.split("/") if part not in ("", ".")]
    if any(part == ".." for part in parts):
        raise VerificationError(f"archive traversal member is forbidden: {name!r}")
    return Path(*parts)


def _link_target_is_inside(member_path: Path, target: str, *, hardlink: bool) -> bool:
    if "\x00" in target or target.startswith(("/", "\\")) or re.match(r"^[A-Za-z]:", target):
        return False
    target_parts = target.replace("\\", "/").split("/")
    if hardlink:
        stack: list[str] = []
    else:
        stack = list(member_path.parent.parts)
    for part in target_parts:
        if part in ("", "."):
            continue
        if part == "..":
            if not stack:
                return False
            stack.pop()
        else:
            stack.append(part)
    return True


def _safe_extract_tar(archive_path: Path, destination: Path) -> None:
    """Extract regular files and contained links without following archive paths."""
    destination.mkdir(parents=True, exist_ok=True)
    root = destination.resolve()
    seen: dict[Path, str] = {}
    with tarfile.open(archive_path, mode="r:gz") as archive:
        members = archive.getmembers()
        parsed: list[tuple[tarfile.TarInfo, Path]] = []
        for member in members:
            relative = _archive_path(member.name)
            if not relative.parts:
                if member.isdir():
                    continue
                raise VerificationError(f"archive member resolves to extraction root: {member.name!r}")
            if member.isdir():
                kind = "dir"
            elif member.isreg():
                kind = "file"
            elif member.issym():
                kind = "symlink"
                if not _link_target_is_inside(relative, member.linkname, hardlink=False):
                    raise VerificationError(f"escaping or absolute symbolic link: {member.name!r} -> {member.linkname!r}")
            elif member.islnk():
                kind = "hardlink"
                if not _link_target_is_inside(relative, member.linkname, hardlink=True):
                    raise VerificationError(f"escaping or absolute hard link: {member.name!r} -> {member.linkname!r}")
            else:
                raise VerificationError(f"special archive member is forbidden: {member.name!r}")
            previous = seen.get(relative)
            if previous is not None and not (previous == kind == "dir"):
                raise VerificationError(f"duplicate archive member: {member.name!r}")
            seen[relative] = kind
            parsed.append((member, relative))

        for member, relative in parsed:
            target_path = root.joinpath(relative)
            parent = target_path.parent
            parent.mkdir(parents=True, exist_ok=True)
            resolved_parent = parent.resolve()
            try:
                resolved_parent.relative_to(root)
            except ValueError as error:
                raise VerificationError(f"archive path escapes extraction root: {member.name!r}") from error
            cursor = parent
            while cursor != root:
                if cursor.is_symlink():
                    raise VerificationError(f"archive member traverses a symbolic link: {member.name!r}")
                cursor = cursor.parent

            if member.isdir():
                target_path.mkdir(exist_ok=True)
                os.chmod(target_path, 0o755)
            elif member.isreg():
                source = archive.extractfile(member)
                if source is None:
                    raise VerificationError(f"could not read archive member: {member.name!r}")
                mode = 0o755 if member.mode & 0o111 else 0o644
                with source, target_path.open("xb") as output:
                    shutil.copyfileobj(source, output)
                os.chmod(target_path, mode)
            elif member.issym():
                os.symlink(member.linkname, target_path)
                try:
                    target_path.resolve(strict=False).relative_to(root)
                except ValueError as error:
                    raise VerificationError(f"symbolic link escapes extraction root: {member.name!r}") from error
            else:
                hardlink_target = _archive_path(member.linkname)
                source_path = root.joinpath(hardlink_target)
                try:
                    source_path.resolve(strict=True).relative_to(root)
                except (OSError, ValueError) as error:
                    raise VerificationError(f"invalid hard-link target: {member.name!r}") from error
                if source_path.is_symlink() or not source_path.is_file():
                    raise VerificationError(f"hard link target is not a regular file: {member.linkname!r}")
                os.link(source_path, target_path)


def _single_matching_root(staging: Path, marker: str) -> Path:
    candidates = [path for path in staging.rglob(marker) if path.is_file()]
    if len(candidates) != 1:
        raise VerificationError(f"expected one {marker} under extracted archive, found {len(candidates)}")
    return candidates[0]


def _discover_sdk_root(staging: Path) -> Path:
    executable = _single_matching_root(staging, "rbt")
    if executable.parent.name != "bin":
        raise VerificationError(f"SDK rbt executable is not under bin/: {executable}")
    root = executable.parent.parent
    entries = list(staging.iterdir())
    if root.name != "rbt-sdk-0.8.0-linux-x86_64" or entries != [root]:
        raise VerificationError(f"SDK archive must contain exactly rbt-sdk-0.8.0-linux-x86_64; found {[p.name for p in entries]}")
    return root


def _discover_source_root(staging: Path) -> Path:
    candidates = [
        path
        for path in staging.rglob("Cargo.toml")
        if path.parent.joinpath("crates/rbt-cli/Cargo.toml").is_file()
        and path.parent.joinpath("third_party/iceoryx2-rbt/source/iceoryx2/Cargo.toml").is_file()
    ]
    if len(candidates) != 1:
        raise VerificationError(f"expected one source tree in archive, found {len(candidates)}")
    root = candidates[0].parent
    entries = list(staging.iterdir())
    if root.name != "rbt-0.8.0-source" or entries != [root]:
        raise VerificationError(f"source archive must contain exactly rbt-0.8.0-source; found {[p.name for p in entries]}")
    return root


def _safe_relative_file(root: Path, relative: str) -> Path:
    candidate = root.joinpath(*PurePosixPath(relative).parts)
    try:
        candidate.resolve(strict=True).relative_to(root.resolve())
    except (OSError, ValueError) as error:
        raise VerificationError(f"required source file is missing or escapes source root: {relative}") from error
    if not candidate.is_file():
        raise VerificationError(f"required source file is missing: {relative}")
    return candidate


def _check_source_archive(source_root: Path) -> str:
    for relative in SOURCE_REQUIRED_FILES:
        _safe_relative_file(source_root, relative)

    for path in source_root.rglob("*"):
        relative = path.relative_to(source_root)
        if path.is_symlink():
            try:
                path.resolve(strict=False).relative_to(source_root.resolve())
            except ValueError as error:
                raise VerificationError(f"source archive contains escaping symlink: {relative}") from error
        if any(part in SOURCE_FORBIDDEN_DIRS or part.startswith(".env") for part in relative.parts):
            raise VerificationError(f"source archive contains forbidden build, cache, VCS, or env path: {relative}")
        if path.is_file() and path.suffix.lower() in SOURCE_FORBIDDEN_SUFFIXES:
            raise VerificationError(f"source archive contains binary or cache file: {relative}")
        if path.is_file():
            with path.open("rb") as stream:
                signature = stream.read(8)
            if signature.startswith((b"\x7fELF", b"MZ", b"PK\x03\x04")) or signature in {
                b"\xfe\xed\xfa\xce",
                b"\xce\xfa\xed\xfe",
                b"\xfe\xed\xfa\xcf",
                b"\xcf\xfa\xed\xfe",
            }:
                raise VerificationError(f"source archive contains binary file: {relative}")

    native = source_root / "third_party/iceoryx2-rbt"
    series = native / "patches/series"
    patch_names = [line.strip() for line in series.read_text(encoding="utf-8").splitlines() if line.strip()]
    if not patch_names:
        raise VerificationError("maintained native patch series is empty")
    for name in patch_names:
        if Path(name).name != name:
            raise VerificationError(f"native patch series contains an unsafe path: {name!r}")
        _safe_relative_file(native / "patches", name)
    patch_hashes = _parse_hash_list(native / "patches/SHA256SUMS", native / "patches")
    if set(patch_names) != set(patch_hashes):
        raise VerificationError("source native patch series and SHA256SUMS do not cover the same files")
    source_hash = (native / "SOURCE_DIFF_SHA256").read_text(encoding="ascii").split()[0]
    if not re.fullmatch(r"[0-9a-fA-F]{64}", source_hash):
        raise VerificationError("source maintained native fingerprint is malformed")
    return f"source tree contains {len(patch_names)} maintained native patches and no forbidden artifacts"


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def _parse_hash_list(path: Path, base: Path) -> dict[str, str]:
    entries: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        match = re.fullmatch(r"([0-9a-fA-F]{64})\s+[* ]?(.+)", line)
        if not match:
            raise VerificationError(f"malformed SHA256SUMS line in {path}: {line!r}")
        digest, name = match.groups()
        relative = _archive_path(name)
        if name in entries:
            raise VerificationError(f"duplicate hash entry in {path}: {name}")
        candidate = base / relative
        try:
            candidate.resolve(strict=True).relative_to(base.resolve())
        except (OSError, ValueError) as error:
            raise VerificationError(f"hash list entry is missing or escapes its root: {name}") from error
        if not candidate.is_file() or _sha256(candidate).lower() != digest.lower():
            raise VerificationError(f"SHA-256 mismatch for {candidate}")
        entries[name] = digest.lower()
    return entries


def _check_source_metadata(source_root: Path, cargo: str, runner: "CommandRunner") -> str:
    result = runner.run(
        [cargo, "metadata", "--offline", "--no-deps", "--format-version", "1"],
        cwd=source_root,
        mode="build",
        timeout=120,
    )
    data = json.loads(result.stdout)
    workspace_root = Path(data["workspace_root"]).resolve()
    if workspace_root != source_root.resolve():
        raise VerificationError(f"Cargo metadata resolved outside source archive: {workspace_root}")
    packages = data.get("packages", [])
    if not any(package.get("name") == "rbt" for package in packages):
        raise VerificationError("Cargo metadata did not include the maintained rbt crate")
    runner.run(["/usr/bin/bash", str(source_root / "scripts/bootstrap_iceoryx2_rbt.sh"), "--source-only"], cwd=source_root, mode="build", timeout=60)
    return f"native source export audit passed; offline Cargo metadata resolved {len(packages)} maintained workspace packages from the extracted source"


def _validate_sdk_provenance(sdk_root: Path) -> str:
    rust_root = sdk_root / "share/rbt/rust"
    required = (
        sdk_root / "bin/rbt",
        sdk_root / "bin/rbt-idl",
        sdk_root / "bin/composer",
        sdk_root / "env.sh",
        sdk_root / "share/rbt/iceoryx2-provenance.toml",
        rust_root / "Cargo.toml",
        rust_root / "crates/rbt/Cargo.toml",
        rust_root / "crates/rbt-types/Cargo.toml",
        rust_root / "third_party/iceoryx2-rbt/UPSTREAM.toml",
        rust_root / "third_party/iceoryx2-rbt/SOURCE_DIFF_SHA256",
        rust_root / "third_party/iceoryx2-rbt/patches/series",
        rust_root / "third_party/iceoryx2-rbt/patches/SHA256SUMS",
    )
    for path in required:
        if not path.is_file():
            raise VerificationError(f"required SDK file is missing: {path.relative_to(sdk_root)}")

    native_root = rust_root / "third_party/iceoryx2-rbt"
    upstream = tomllib.loads((native_root / "UPSTREAM.toml").read_text(encoding="utf-8"))
    fork_version = upstream.get("fork_version")
    if fork_version != NATIVE_VERSION:
        raise VerificationError(f"SDK maintained native version is {fork_version!r}, expected {NATIVE_VERSION}")
    source_hash = (native_root / "SOURCE_DIFF_SHA256").read_text(encoding="ascii").split()[0]
    if not re.fullmatch(r"[0-9a-fA-F]{64}", source_hash):
        raise VerificationError("SDK maintained native source hash is malformed")

    patches = native_root / "patches"
    patch_hashes = _parse_hash_list(patches / "SHA256SUMS", patches)
    series_names = [line.strip() for line in (patches / "series").read_text(encoding="utf-8").splitlines() if line.strip()]
    if not series_names or set(series_names) != set(patch_hashes):
        raise VerificationError("SDK patch series and SHA256SUMS do not cover the same patch files")

    provenance = tomllib.loads((sdk_root / "share/rbt/iceoryx2-provenance.toml").read_text(encoding="utf-8"))
    patch_manifest_digest = hashlib.sha256()
    for filename in ("series", "SHA256SUMS"):
        patch_manifest_digest.update(f"{_sha256(patches / filename)}  {filename}\n".encode("ascii"))
    actual_fields = {
        "fork_version": fork_version,
        "source_diff_sha256": source_hash.lower(),
        "patch_series_sha256": patch_manifest_digest.hexdigest(),
    }
    for key, expected in actual_fields.items():
        value = provenance.get(key)
        if key.endswith("sha256"):
            if not isinstance(value, str) or value.lower() != expected:
                raise VerificationError(f"SDK native provenance has an incorrect {key}")
        elif value != expected:
            raise VerificationError(f"SDK native provenance has an incorrect {key}")

    metadata_path = sdk_root / "share/rbt/release-metadata.json"
    if metadata_path.is_file():
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        for key in ("version", "rbt_version", "sdk_version"):
            if key in metadata and metadata[key] != PRODUCT_VERSION:
                raise VerificationError(f"SDK release metadata has incorrect {key}: {metadata[key]!r}")
    return f"relocated SDK provenance and {len(patch_hashes)} native patch hashes are valid"


def _rpath_entries(dynamic_output: str) -> list[str]:
    entries: list[str] = []
    for line in dynamic_output.splitlines():
        if "RPATH" not in line and "RUNPATH" not in line:
            continue
        match = re.search(r"\[(.*?)\]", line)
        if match:
            entries.extend(entry for entry in match.group(1).split(":") if entry)
    return entries


def _rpaths_are_origin_relative(entries: Iterable[str]) -> bool:
    return all(entry == "$ORIGIN" or entry.startswith("$ORIGIN/") for entry in entries)


def _rpaths_have_no_absolute_entries(entries: Iterable[str]) -> bool:
    return all(not Path(entry).is_absolute() and not re.match(r"^[A-Za-z]:[/\\]", entry) for entry in entries)


def _standard_system_library(path: Path) -> bool:
    resolved = path.resolve(strict=False)
    system_roots = (Path("/lib"), Path("/lib64"), Path("/usr/lib"), Path("/usr/lib64"), Path("/usr/local/lib"))
    return any(resolved == root or root in resolved.parents for root in system_roots)


def _elf_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for path in root.rglob("*"):
        if path.is_file():
            try:
                with path.open("rb") as stream:
                    if stream.read(4) == b"\x7fELF":
                        files.append(path)
            except OSError:
                continue
    return files


def _sdk_library_path(sdk_root: Path) -> str:
    candidates = [sdk_root / "lib", sdk_root / "lib64"]
    return os.pathsep.join(str(path) for path in candidates if path.is_dir())


def _check_sdk_elf(sdk_root: Path, readelf: str, ldd: str, runner: "CommandRunner") -> str:
    files = _elf_files(sdk_root)
    if not files:
        raise VerificationError("SDK contains no ELF executables or shared libraries")
    failures: list[str] = []
    env = {"LD_LIBRARY_PATH": _sdk_library_path(sdk_root)}
    for path in files:
        result = runner.run([readelf, "-d", str(path)], cwd=sdk_root, mode="runtime", timeout=30, extra_env=env)
        dynamic = result.stdout
        entries = _rpath_entries(dynamic)
        if not _rpaths_are_origin_relative(entries):
            failures.append(f"{path.relative_to(sdk_root)} has non-$ORIGIN RPATH/RUNPATH {entries!r}")
        needed = re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic)
        if not needed:
            continue
        dependencies = runner.run([ldd, str(path)], cwd=sdk_root, mode="runtime", timeout=30, extra_env=env)
        if "not found" in dependencies.stdout:
            failures.append(f"{path.relative_to(sdk_root)} has unresolved dynamic dependencies")
            continue
        for line in dependencies.stdout.splitlines():
            match = re.search(r"=>\s+(/.+?)\s+\(0x", line)
            if match:
                dependency = Path(match.group(1))
            else:
                match = re.match(r"\s*(/.+?)\s+\(0x", line)
                if not match:
                    continue
                dependency = Path(match.group(1))
            resolved = dependency.resolve(strict=False)
            if not (resolved == sdk_root.resolve() or sdk_root.resolve() in resolved.parents or _standard_system_library(resolved)):
                failures.append(f"{path.relative_to(sdk_root)} resolves dependency outside SDK and system libraries: {resolved}")
    if failures:
        raise VerificationError("; ".join(failures[:12]))
    return f"checked $ORIGIN-only RPATHs and dynamic dependencies for {len(files)} SDK ELF files"


def _check_cmake_exports(sdk_root: Path, toolchain: Path) -> str:
    cmake_files = [path for path in sdk_root.rglob("*.cmake") if path.is_file()]
    by_name = {path.name.lower(): path for path in cmake_files}
    required_names = (
        "rbtconfig.cmake",
        "rbttargets.cmake",
        "iceoryx2-cconfig.cmake",
        "iceoryx2-bb-cxxconfig.cmake",
        "iceoryx2-cxxconfig.cmake",
    )
    missing = [name for name in required_names if name not in by_name]
    if missing:
        raise VerificationError(f"SDK CMake exports are missing: {', '.join(missing)}")
    forbidden_paths = ["/home/sw/rbt", str(toolchain.resolve())]
    for path in cmake_files:
        text = path.read_text(encoding="utf-8", errors="replace")
        for forbidden in forbidden_paths:
            if forbidden and forbidden in text:
                raise VerificationError(f"CMake export {path.relative_to(sdk_root)} embeds build path {forbidden}")
    return f"found relocatable rbt and maintained native CMake exports ({len(cmake_files)} CMake files)"


def _check_env_script(sdk_root: Path, runner: "CommandRunner") -> str:
    env_script = sdk_root / "env.sh"
    if not env_script.is_file():
        raise VerificationError("SDK root env.sh is missing")
    script = 'source "$1" && env -0'
    result = runner.run(
        ["/usr/bin/bash", "-c", script, "verify-env", str(env_script)],
        cwd=sdk_root,
        mode="runtime",
        timeout=20,
        extra_env={"PATH": f"{sdk_root / 'bin'}:/usr/bin:/bin"},
    )
    values: dict[str, str] = {}
    for entry in result.stdout.split("\x00"):
        if "=" in entry:
            key, value = entry.split("=", 1)
            values[key] = value
    prefix = str(sdk_root)
    if not any(prefix in value for value in values.values()):
        raise VerificationError("sourcing env.sh did not compute any path into the moved SDK")
    if any(value.startswith(str(runner.repo_root)) for key, value in values.items() if key not in {"CARGO_HOME", "RUSTUP_HOME"}):
        raise VerificationError("sourcing env.sh retained a checkout or original-prefix path")
    return f"env.sh sourced with a clean environment and resolved paths into {sdk_root}"


def _check_versions(sdk_root: Path, runner: "CommandRunner") -> str:
    sdk_env = {"PATH": f"{sdk_root / 'bin'}:/usr/bin:/bin", "LD_LIBRARY_PATH": _sdk_library_path(sdk_root)}
    outputs: dict[str, str] = {}
    for executable in ("rbt", "rbt-idl", "composer"):
        result = runner.run(
            [str(sdk_root / "bin" / executable), "--version"],
            cwd=sdk_root,
            mode="runtime",
            timeout=30,
            extra_env=sdk_env,
        )
        outputs[executable] = result.stdout + result.stderr
    if not re.search(r"(?m)^rbt 0\.8\.0$", outputs["rbt"]):
        raise VerificationError(f"rbt reports an unexpected version: {outputs['rbt'].strip()!r}")
    if not re.search(r"(?m)^rbt-idl 0\.8\.0$", outputs["rbt-idl"]):
        raise VerificationError(f"rbt-idl reports an unexpected version: {outputs['rbt-idl'].strip()!r}")
    if not re.search(r"(?m)^composer 0\.8\.0$", outputs["composer"]):
        raise VerificationError(f"composer reports an unexpected version: {outputs['composer'].strip()!r}")
    verbose = runner.run(
        [str(sdk_root / "bin/rbt"), "version", "--verbose"],
        cwd=sdk_root,
        mode="runtime",
        timeout=30,
        extra_env=sdk_env,
    ).stdout
    expected = {
        "SDK 0.8.0": "SDK version",
        "IDL language 1": "IDL language axis",
        "message descriptor 1": "message descriptor axis",
        "control protocol 2": "control protocol axis",
        "Composer snapshot 4": "Composer snapshot axis",
    }
    missing = [label for token, label in expected.items() if token not in verbose.splitlines()]
    native_versions = re.findall(r"(?m)^iceoryx2 (\S+)$", verbose)
    if missing or not native_versions or native_versions[0] not in (NATIVE_VERSION, "0.10.0-rbt2"):
        raise VerificationError(f"verbose rbt version axes are incorrect: missing={missing}, output={verbose.strip()!r}")
    return "installed CLI, IDL, and Composer versions and frozen API axes match the release"


def _validate_wheel(wheel_path: Path, python_tag: str) -> str:
    with zipfile.ZipFile(wheel_path) as wheel:
        names = wheel.namelist()
        if len(names) != len(set(names)):
            raise VerificationError("wheel contains duplicate entries")
        for name in names:
            if name.startswith("/") or ".." in PurePosixPath(name).parts or "\\" in name:
                raise VerificationError(f"unsafe wheel member: {name}")
        dist_infos = {name.split("/", 1)[0] for name in names if ".dist-info/" in name}
        if len(dist_infos) != 1:
            raise VerificationError(f"wheel must contain one dist-info directory, found {sorted(dist_infos)}")
        dist_info = next(iter(dist_infos))
        metadata_name = f"{dist_info}/METADATA"
        wheel_name = f"{dist_info}/WHEEL"
        record_name = f"{dist_info}/RECORD"
        if not all(name in names for name in (metadata_name, wheel_name, record_name)):
            raise VerificationError("wheel is missing METADATA, WHEEL, or RECORD")
        metadata = wheel.read(metadata_name).decode("utf-8", errors="strict")
        metadata_fields = dict(
            line.split(":", 1) for line in metadata.splitlines() if ":" in line and not line[:1].isspace()
        )
        if metadata_fields.get("Name", "").strip().lower() != "rbt" or metadata_fields.get("Version", "").strip() != PRODUCT_VERSION:
            raise VerificationError("wheel METADATA must identify rbt 0.8.0")
        wheel_metadata = wheel.read(wheel_name).decode("utf-8", errors="strict")
        tags = [line.split(":", 1)[1].strip() for line in wheel_metadata.splitlines() if line.startswith("Tag:")]
        if tags != [f"{python_tag}-{python_tag}-manylinux_2_28_x86_64"]:
            raise VerificationError(f"wheel tags do not include the requested {python_tag} ABI: {tags!r}")
        license_paths = [name for name in names if name.startswith(f"{dist_info}/licenses/") and not name.endswith("/")]
        license_fields = [line.split(":", 1)[1].strip() for line in metadata.split("\n\n", 1)[0].splitlines() if line.startswith("License-File:")]
        if not license_fields or any(f"{dist_info}/licenses/{name}" not in license_paths for name in license_fields):
            raise VerificationError("wheel License-File metadata does not resolve under dist-info/licenses")
        license_basenames = {Path(name).name.upper() for name in license_paths}
        if not any("MIT" in name for name in license_basenames) or not any("APACHE" in name for name in license_basenames):
            raise VerificationError("wheel must include both MIT and Apache license files under dist-info/licenses")

        rows = list(csv.reader(io.StringIO(wheel.read(record_name).decode("utf-8", errors="strict"))))
        records: dict[str, tuple[str, str]] = {}
        for row in rows:
            if len(row) != 3:
                raise VerificationError("wheel RECORD contains a malformed row")
            name, digest, size = row
            if name in records:
                raise VerificationError(f"wheel RECORD contains duplicate entry: {name}")
            records[name] = (digest, size)
        for name in names:
            if name.endswith("/"):
                continue
            if name not in records:
                raise VerificationError(f"wheel RECORD omits file: {name}")
            digest, size = records[name]
            if name == record_name:
                if digest or size:
                    raise VerificationError("wheel RECORD must leave its self hash and size empty")
                continue
            payload = wheel.read(name)
            if not digest.startswith("sha256="):
                raise VerificationError(f"wheel RECORD has a missing or unsupported hash for {name}")
            expected_hash = base64.urlsafe_b64encode(hashlib.sha256(payload).digest()).decode("ascii").rstrip("=")
            if digest.split("=", 1)[1] != expected_hash or size != str(len(payload)):
                raise VerificationError(f"wheel RECORD hash or size mismatch for {name}")
        if set(records) != {name for name in names if not name.endswith("/")}:
            raise VerificationError("wheel RECORD includes entries absent from the archive")
    return f"wheel metadata, {len(tags)} compatibility tags, licenses, and all RECORD hashes are valid"


def _assert_sdk_paths(metadata: dict[str, Any], sdk_rust_root: Path) -> str:
    sdk_real = sdk_rust_root.resolve()
    checked = 0
    foreign: list[str] = []
    for package in metadata.get("packages", []):
        name = package.get("name", "")
        if name == "rbt" or name.startswith("rbt-") or name.startswith("iceoryx2"):
            manifest = Path(package["manifest_path"]).resolve()
            checked += 1
            try:
                manifest.relative_to(sdk_real)
            except ValueError:
                foreign.append(f"{name}: {manifest}")
    if not checked:
        raise VerificationError("external Cargo metadata contained no installed rbt or native packages")
    if foreign:
        raise VerificationError("external Rust consumer resolved SDK packages outside moved prefix: " + "; ".join(foreign))
    return f"Cargo metadata resolved all {checked} rbt and maintained native packages under {sdk_real}"


class CommandRunner:
    """Run bounded child commands in a networkless bubblewrap namespace."""

    def __init__(self, work_dir: Path, repo_root: Path, toolchain: Path, sdk_root: Path | None = None) -> None:
        self.work_dir = work_dir.resolve()
        self.repo_root = repo_root.resolve()
        self.toolchain = toolchain.resolve()
        self.sdk_root = sdk_root.resolve() if sdk_root is not None else None
        self.cargo_home = Path(os.environ.get("CARGO_HOME", Path.home() / ".cargo")).resolve()
        self.rustup_home = Path(os.environ.get("RUSTUP_HOME", Path.home() / ".rustup")).resolve()
        self.bwrap = shutil.which("bwrap")
        self.mode_description = "bubblewrap networkless, read-only root with task-specific mounts"

    def preflight(self) -> str:
        if not self.bwrap:
            raise VerificationError("strict isolation requires bubblewrap (`bwrap`); install it and rerun the release verifier")
        self.run(["/usr/bin/true"], cwd=self.work_dir, mode="runtime", timeout=15)
        return self.mode_description

    def _base(self, mode: str) -> list[str]:
        if not self.bwrap:
            raise VerificationError("bubblewrap is unavailable; refusing to run an unisolated release check")
        args = [
            self.bwrap,
            "--die-with-parent",
            "--unshare-net",
            "--ro-bind",
            "/",
            "/",
            "--proc",
            "/proc",
            "--dev",
            "/dev",
            "--bind",
            str(self.work_dir),
            str(self.work_dir),
        ]
        if mode == "runtime":
            if self.repo_root.exists():
                args.extend(["--tmpfs", str(self.repo_root)])
        elif mode == "build":
            cargo_home = self.cargo_home
            if cargo_home.is_dir():
                args.extend(["--bind", str(cargo_home), str(cargo_home)])
            for relative in ("crates", "generated", "third_party", "sdk", "python"):
                hidden = self.repo_root / relative
                if hidden.exists():
                    args.extend(["--tmpfs", str(hidden)])
            native_prefixes = (
                self.repo_root / "target/iox2-rbt-0.10.0-cpp-install",
                self.repo_root / "target/iox2-cpp-install",
            )
            for prefix in native_prefixes:
                if prefix.exists():
                    args.extend(["--tmpfs", str(prefix)])
        else:
            raise VerificationError(f"unknown isolation mode: {mode}")
        return args

    def run(
        self,
        command: list[str],
        *,
        cwd: Path,
        mode: str,
        timeout: int,
        extra_env: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        env = {
            "HOME": str(Path.home()),
            "LANG": "C.UTF-8",
            "LC_ALL": "C.UTF-8",
            "PATH": f"{self.toolchain / 'bin'}:{self.cargo_home / 'bin'}:/usr/bin:/bin",
            "CARGO_HOME": str(self.cargo_home),
            "RUSTUP_HOME": str(self.rustup_home),
            "TMPDIR": str(self.work_dir / "tmp"),
        }
        if self.sdk_root is not None:
            env["LD_LIBRARY_PATH"] = _sdk_library_path(self.sdk_root)
        if extra_env:
            env.update(extra_env)
        args = self._base(mode)
        for key, value in env.items():
            args.extend(["--setenv", key, value])
        args.extend(["--chdir", str(cwd), "--", *command])
        process = subprocess.Popen(
            args,
            cwd=self.work_dir,
            env={"PATH": "/usr/bin:/bin"},
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            start_new_session=True,
        )
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGTERM)
            try:
                stdout, stderr = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                stdout, stderr = process.communicate()
            raise VerificationError(f"command timed out after {timeout}s: {' '.join(command)}\n{stderr[-3000:]}")
        if process.returncode:
            raise VerificationError(
                f"command exited {process.returncode}: {' '.join(command)}\n"
                f"stdout: {stdout[-4000:]}\nstderr: {stderr[-4000:]}"
            )
        return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)


def _resolve_python(specification: str) -> tuple[Path, str]:
    match = re.fullmatch(r"(?i)cp(\d)(\d\d)", specification)
    if match:
        version = f"{match.group(1)}.{match.group(2)}"
        executable_name = f"python{version}"
        found = shutil.which(executable_name)
        if found is None:
            raise VerificationError(f"could not find {executable_name} for --python {specification}")
        return Path(found).absolute(), f"cp{match.group(1)}{match.group(2)}"
    executable = shutil.which(specification) if os.sep not in specification else specification
    if not executable:
        raise VerificationError(f"could not resolve --python interpreter: {specification}")
    path = Path(executable).absolute()
    process = subprocess.run(
        [str(path), "-c", "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')"],
        check=True,
        capture_output=True,
        text=True,
        timeout=10,
    )
    major, minor = process.stdout.strip().split(".")
    return path, f"cp{major}{minor}"


def _copy_numpy_dependency(python: Path, venv_site: Path) -> str:
    probe = subprocess.run(
        [str(python), "-c", "import pathlib, numpy; print(pathlib.Path(numpy.__file__).parent)"],
        check=True,
        capture_output=True,
        text=True,
        timeout=15,
    )
    numpy_source = Path(probe.stdout.strip()).resolve()
    if numpy_source.name != "numpy" or not numpy_source.is_dir():
        raise VerificationError(f"--python did not provide an importable NumPy package: {numpy_source}")
    shutil.copytree(numpy_source, venv_site / "numpy", dirs_exist_ok=True, symlinks=False)
    numpy_libs = numpy_source.parent / "numpy.libs"
    if numpy_libs.is_dir():
        shutil.copytree(numpy_libs, venv_site / "numpy.libs", dirs_exist_ok=True, symlinks=False)
    return f"copied NumPy dependency from {numpy_source} into the isolated CPython environment"


def _write_rust_consumer(project: Path, sdk_rust_root: Path) -> Path:
    consumer = project / "rust-api-consumer"
    (consumer / "src").mkdir(parents=True, exist_ok=True)
    manifest = f'''[package]
name = "release-api-consumer"
version = "0.0.0"
edition = "2024"

[workspace]

[dependencies]
rbt = {{ path = {json.dumps(str(sdk_rust_root / "crates/rbt"))} }}
rbt-types = {{ path = {json.dumps(str(sdk_rust_root / "crates/rbt-types"))} }}
'''
    source = '''use rbt::tf::{LookupOptions, LookupPolicy, Quaternion, StaticEdge, StaticSet, Transform3, TransformBuffer, Vec3};
use rbt::time::DeviceClock;
use rbt::types::CapabilityInstanceId;

fn main() {
    let clock = DeviceClock::new([1; 16], 7).expect("device clock");
    let stamp = clock.stamp(42);
    let buffer = TransformBuffer::new(Default::default(), LookupPolicy::default()).expect("TF buffer");
    let authority = buffer.bind_provider("release-check", CapabilityInstanceId::new()).expect("provider");
    buffer.apply_static_snapshot(&authority, StaticSet {
        revision: 1,
        effective_from: stamp,
        effective_uncertainty_ns: Some(0),
        transforms: vec![StaticEdge {
            parent: "world".parse().expect("parent"),
            child: "base".parse().expect("child"),
            transform: Transform3::new(Vec3::new(1.0, 2.0, 3.0).expect("translation"), Quaternion::identity()),
        }],
    }).expect("static snapshot");
    let result = buffer.lookup_at_with("world", "base", stamp, LookupOptions {
        timestamp_uncertainty_ns: Some(0), mappings: &[], now: None,
    }).expect("TF lookup");
    assert_eq!(result.transform.translation().x(), 1.0);
    let _: Option<rbt_types::DynamicTransform> = None;
}
'''
    (consumer / "Cargo.toml").write_text(manifest, encoding="utf-8")
    (consumer / "src/main.rs").write_text(source, encoding="utf-8")
    return consumer


def _check_external_rust(
    sdk_root: Path,
    work_dir: Path,
    runner: CommandRunner,
    cargo: str,
) -> str:
    consumers = work_dir / "external consumers"
    consumers.mkdir(parents=True, exist_ok=True)
    project = consumers / "generated-rust-project"
    project.mkdir()
    sdk_env = {"PATH": f"{sdk_root / 'bin'}:{runner.toolchain / 'bin'}:{runner.cargo_home / 'bin'}:/usr/bin:/bin"}
    runner.run([str(sdk_root / "bin/rbt"), "new", "release-check"], cwd=project, mode="runtime", timeout=60, extra_env=sdk_env)
    generated_project = project / "release-check"
    runner.run([str(sdk_root / "bin/rbt"), "check"], cwd=generated_project, mode="runtime", timeout=90, extra_env=sdk_env)
    generated_cargo_env = {
        "CARGO_TARGET_DIR": str(work_dir / "cargo-target"),
        "CARGO_NET_OFFLINE": "true",
        "CC": str(runner.toolchain / "bin/x86_64-conda-linux-gnu-gcc"),
        "CXX": str(runner.toolchain / "bin/x86_64-conda-linux-gnu-g++"),
        "AR": str(runner.toolchain / "bin/x86_64-conda-linux-gnu-ar"),
        "CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER": str(runner.toolchain / "bin/x86_64-conda-linux-gnu-gcc"),
        "LIBRARY_PATH": f"{runner.toolchain / 'lib'}:{sdk_root / 'lib'}",
    }
    metadata_process = runner.run(
        [cargo, "metadata", "--offline", "--format-version", "1"],
        cwd=generated_project,
        mode="build",
        timeout=120,
        extra_env=generated_cargo_env,
    )
    _assert_sdk_paths(json.loads(metadata_process.stdout), sdk_root / "share/rbt/rust")
    runner.run(
        [cargo, "build", "--offline", "--workspace"],
        cwd=generated_project,
        mode="build",
        timeout=600,
        extra_env=generated_cargo_env,
    )

    custom_consumer = _write_rust_consumer(consumers, sdk_root / "share/rbt/rust")
    custom_metadata = runner.run(
        [cargo, "metadata", "--offline", "--format-version", "1"],
        cwd=custom_consumer,
        mode="build",
        timeout=120,
        extra_env=generated_cargo_env,
    )
    _assert_sdk_paths(json.loads(custom_metadata.stdout), sdk_root / "share/rbt/rust")
    runner.run(
        [cargo, "build", "--offline"],
        cwd=custom_consumer,
        mode="build",
        timeout=600,
        extra_env=generated_cargo_env,
    )
    runner.run([str(work_dir / "cargo-target/debug/release-api-consumer")], cwd=custom_consumer, mode="runtime", timeout=30)
    return "rbt new/check auto-discovered the moved Rust SDK; generated and TF/time consumer crates built offline"


def _write_cpp_consumer(project: Path) -> None:
    project.mkdir(parents=True, exist_ok=True)
    (project / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.22)\n"
        "project(rbt_release_consumer LANGUAGES CXX)\n"
        "find_package(rbt CONFIG REQUIRED)\n"
        "add_executable(consumer main.cpp)\n"
        "target_link_libraries(consumer PRIVATE rbt::runtime rbt::iox2)\n",
        encoding="utf-8",
    )
    (project / "main.cpp").write_text(
        "#include <cmath>\n"
        "#include <rbt/tf_math.hpp>\n"
        "#include <rbt/time.hpp>\n"
        "#include <rbt/version.hpp>\n"
        "int main() {\n"
        "  if (rbt::SDK_VERSION != \"0.8.0\") return 1;\n"
        "  rbt::tf::Transform3 transform(rbt::tf::Vec3(1.0, 2.0, 3.0), rbt::tf::Quaternion::identity());\n"
        "  const auto point = transform.apply_point(rbt::tf::Vec3(4.0, 5.0, 6.0));\n"
        "  return std::abs(point.x() - 5.0) < 1e-12 && std::abs(point.y() - 7.0) < 1e-12 && std::abs(point.z() - 9.0) < 1e-12 ? 0 : 2;\n"
        "}\n",
        encoding="utf-8",
    )


def _check_external_cpp(sdk_root: Path, work_dir: Path, toolchain: Path, runner: CommandRunner) -> str:
    project = work_dir / "external consumers/cpp"
    _write_cpp_consumer(project)
    build_dir = work_dir / "cpp build"
    compiler = toolchain / "bin/x86_64-conda-linux-gnu-g++"
    ninja = toolchain / "bin/ninja"
    cmake = shutil.which("cmake")
    if not compiler.is_file() or not ninja.is_file() or not cmake:
        raise VerificationError("C++ consumer requires toolchain/bin/x86_64-conda-linux-gnu-g++, toolchain/bin/ninja, and cmake")
    configure_env = {
        "CXX": str(compiler),
        "PATH": f"{toolchain / 'bin'}:{sdk_root / 'bin'}:/usr/bin:/bin",
        "LD_LIBRARY_PATH": _sdk_library_path(sdk_root),
    }
    runner.run(
        [
            cmake,
            "-S",
            str(project),
            "-B",
            str(build_dir),
            "-G",
            "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={ninja}",
            f"-DCMAKE_CXX_COMPILER={compiler}",
            f"-DCMAKE_PREFIX_PATH={sdk_root}",
        ],
        cwd=work_dir,
        mode="build",
        timeout=120,
        extra_env=configure_env,
    )
    runner.run([cmake, "--build", str(build_dir), "--parallel", "2"], cwd=work_dir, mode="build", timeout=300, extra_env=configure_env)
    runner.run([str(build_dir / "consumer")], cwd=work_dir, mode="runtime", timeout=30)
    return "external CMake consumer found installed exports, linked both public targets, and passed TF math at runtime"


def _python_api_probe(venv_python: Path, sdk_root: Path, work_dir: Path, runner: CommandRunner) -> str:
    probe = work_dir / "python-api-probe.py"
    probe.write_text(
        "from pathlib import Path\n"
        "import rbt\n"
        "from rbt.time import DeviceClock\n"
        "from rbt.tf_math import Quaternion, Transform3, Vec3\n"
        f"venv_root = Path({str(venv_python.parent.parent)!r}).resolve()\n"
        "assert Path(rbt.__file__).resolve().is_relative_to(venv_root), rbt.__file__\n"
        "assert rbt.__version__ == '0.8.0', rbt.__version__\n"
        "stamp = DeviceClock('00000000-0000-0000-0000-000000000001', 2).stamp(42)\n"
        "assert stamp.nanoseconds == 42\n"
        "point = Transform3(Vec3(1, 2, 3), Quaternion.identity()).apply_point(Vec3(4, 5, 6))\n"
        "assert (point.x, point.y, point.z) == (5.0, 7.0, 9.0)\n",
        encoding="utf-8",
    )
    env = {"PATH": "/usr/bin:/bin", "LD_LIBRARY_PATH": ""}
    runner.run([str(venv_python), str(probe)], cwd=work_dir, mode="runtime", timeout=45, extra_env=env)
    return "fresh wheel import, time API, TF math fixture, and venv-only module resolution passed"


def _verify_wheel_runtime(
    wheel_path: Path,
    python: Path,
    python_tag: str,
    sdk_root: Path,
    work_dir: Path,
    runner: CommandRunner,
    readelf: str,
) -> str:
    _validate_wheel(wheel_path, python_tag)
    venv_root = work_dir / "fresh Python environment"
    # This bootstrap occurs before the whole checkout is hidden. The generated
    # environment's base interpreter is checked again after isolation begins.
    subprocess.run([str(python), "-m", "venv", str(venv_root)], check=True, timeout=60)
    venv_python = venv_root / "bin/python"
    site = next((venv_root / "lib").glob("python*/site-packages"))
    numpy_detail = _copy_numpy_dependency(python, site)
    copied_wheel = work_dir / wheel_path.name
    if copied_wheel.resolve() != wheel_path.resolve():
        shutil.copy2(wheel_path, copied_wheel)
    runner.run(
        [str(venv_python), "-m", "pip", "--disable-pip-version-check", "install", "--no-deps", "--no-index", str(copied_wheel)],
        cwd=work_dir,
        mode="runtime",
        timeout=120,
        extra_env={"PATH": "/usr/bin:/bin", "LD_LIBRARY_PATH": ""},
    )
    extension_files = [path for path in (site / "rbt").rglob("*") if path.is_file() and path.suffix == ".so"]
    if not extension_files:
        raise VerificationError("installed wheel contains no Python extension module")
    for extension in extension_files:
        dynamic = runner.run([readelf, "-d", str(extension)], cwd=work_dir, mode="runtime", timeout=30).stdout
        entries = _rpath_entries(dynamic)
        if entries:
            raise VerificationError(f"wheel extension {extension.name} contains an absolute RPATH/RUNPATH: {entries!r}")
    _python_api_probe(venv_python, sdk_root, work_dir, runner)
    return f"{numpy_detail}; installed wheel RPATHs and fresh-environment APIs passed"


def _write_result(path: Path, result: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{os.getpid()}.tmp")
    with temporary.open("w", encoding="utf-8") as stream:
        json.dump(result, stream, indent=2, sort_keys=True)
        stream.write("\n")
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


class CheckCollection:
    def __init__(self) -> None:
        self.checks: list[dict[str, Any]] = []

    def run(self, name: str, check: Callable[[], str]) -> bool:
        started = time.monotonic()
        try:
            detail = check()
            status = "PASS"
        except Exception as error:  # Every mandatory check contributes to the report.
            detail = f"{type(error).__name__}: {error}"
            status = "FAIL"
        self.checks.append(
            {
                "name": name,
                "status": status,
                "detail": detail,
                "duration_seconds": round(time.monotonic() - started, 3),
            }
        )
        print(f"[verify-release] {status} {name}: {detail}", flush=True)
        return status == "PASS"


def _arguments(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk-archive", type=Path, required=True)
    parser.add_argument("--source-archive", type=Path, required=True)
    parser.add_argument("--wheel", type=Path, required=True)
    parser.add_argument("--python", required=True, help="CP312, python tag, or Python interpreter path")
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True, help="fresh directory under /tmp")
    parser.add_argument("--result-json", type=Path, required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = _arguments(argv)
    checks = CheckCollection()
    result: dict[str, Any] = {
        "status": "FAIL",
        "isolation_mode": "strict bubblewrap isolation required; not yet established",
        "checks": checks.checks,
    }
    try:
        work_dir = args.work_dir.resolve()
        if not work_dir.is_absolute() or not work_dir.is_relative_to(Path("/tmp")):
            raise VerificationError(f"--work-dir must be a fresh directory under /tmp: {work_dir}")
        if work_dir.exists() and any(work_dir.iterdir()):
            raise VerificationError(f"--work-dir must be empty when verification starts: {work_dir}")
        work_dir.mkdir(parents=True, exist_ok=True)
        (work_dir / "tmp").mkdir(exist_ok=True)
        for artifact in (args.sdk_archive, args.source_archive, args.wheel):
            if not artifact.is_file():
                raise VerificationError(f"release input is missing: {artifact}")
        toolchain = args.toolchain.resolve(strict=True)
        if not toolchain.is_dir():
            raise VerificationError(f"--toolchain must be a directory: {toolchain}")
        python, python_tag = _resolve_python(args.python)

        sdk_staging = work_dir / "sdk archive staging"
        source_staging = work_dir / "source archive staging"
        runner = CommandRunner(work_dir, Path(__file__).resolve().parents[1], toolchain)
        isolation_ok = checks.run("strict_isolation_preflight", runner.preflight)
        result["isolation_mode"] = runner.mode_description if isolation_ok else "unavailable; strict checks refused"

        checks.run("sdk_archive_safe_extract", lambda: _safe_extract_tar(args.sdk_archive, sdk_staging) or "SDK archive extracted with traversal, special-file, and link checks")
        checks.run("source_archive_safe_extract", lambda: _safe_extract_tar(args.source_archive, source_staging) or "source archive extracted with traversal, special-file, and link checks")

        sdk_root: Path | None = None
        try:
            source_sdk_root = _discover_sdk_root(sdk_staging)
            relocated_base = work_dir / "relocated prefix with spaces"
            relocated_base.mkdir()
            sdk_root = relocated_base / "sdk root"
            shutil.move(str(source_sdk_root), str(sdk_root))
            shutil.rmtree(sdk_staging, ignore_errors=True)
            if source_sdk_root.exists() or sdk_staging.exists():
                raise VerificationError("staged SDK remained available after relocation")
        except Exception as error:
            checks.checks.append({"name": "relocate_sdk_prefix", "status": "FAIL", "detail": f"{type(error).__name__}: {error}"})

        source_root: Path | None = None
        try:
            source_root = _discover_source_root(source_staging)
        except Exception as error:
            checks.checks.append({"name": "discover_source_root", "status": "FAIL", "detail": f"{type(error).__name__}: {error}"})

        if sdk_root is not None and sdk_root.is_dir():
            runner.sdk_root = sdk_root.resolve()
            checks.run("sdk_provenance_and_hashes", lambda: _validate_sdk_provenance(sdk_root))
            checks.run("sdk_cmake_exports", lambda: _check_cmake_exports(sdk_root, toolchain))
            checks.run("sdk_env_script", lambda: _check_env_script(sdk_root, runner))
            checks.run("sdk_cli_versions", lambda: _check_versions(sdk_root, runner))
            readelf = shutil.which("readelf") or "/usr/bin/readelf"
            ldd = shutil.which("ldd") or "/usr/bin/ldd"
            checks.run("sdk_elf_rpaths_and_dependencies", lambda: _check_sdk_elf(sdk_root, readelf, ldd, runner))
        else:
            for name in ("sdk_provenance_and_hashes", "sdk_cmake_exports", "sdk_env_script", "sdk_cli_versions", "sdk_elf_rpaths_and_dependencies"):
                checks.run(name, lambda: (_ for _ in ()).throw(VerificationError("SDK relocation failed")))

        if source_root is not None:
            checks.run("source_contents", lambda: _check_source_archive(source_root))
            cargo = shutil.which("cargo") or str(runner.cargo_home / "bin/cargo")
            checks.run("source_offline_cargo_metadata", lambda: _check_source_metadata(source_root, cargo, runner))
        else:
            checks.run("source_contents", lambda: (_ for _ in ()).throw(VerificationError("source archive root was not found")))
            checks.run("source_offline_cargo_metadata", lambda: (_ for _ in ()).throw(VerificationError("source archive root was not found")))

        if sdk_root is not None and sdk_root.is_dir():
            cargo = shutil.which("cargo") or str(runner.cargo_home / "bin/cargo")
            checks.run("external_rust_consumer", lambda: _check_external_rust(sdk_root, work_dir, runner, cargo))
            checks.run("external_cpp_consumer", lambda: _check_external_cpp(sdk_root, work_dir, toolchain, runner))
        else:
            checks.run("external_rust_consumer", lambda: (_ for _ in ()).throw(VerificationError("SDK relocation failed")))
            checks.run("external_cpp_consumer", lambda: (_ for _ in ()).throw(VerificationError("SDK relocation failed")))

        copied_wheel = work_dir / args.wheel.name
        if copied_wheel.resolve() != args.wheel.resolve():
            shutil.copy2(args.wheel, copied_wheel)
        wheel = copied_wheel
        checks.run("wheel_metadata_and_record", lambda: _validate_wheel(wheel, python_tag))
        if sdk_root is not None and sdk_root.is_dir():
            checks.run(
                "wheel_fresh_python_install_and_api",
                lambda: _verify_wheel_runtime(wheel, python, python_tag, sdk_root, work_dir, runner, shutil.which("readelf") or "/usr/bin/readelf"),
            )
        else:
            checks.run("wheel_fresh_python_install_and_api", lambda: (_ for _ in ()).throw(VerificationError("SDK relocation failed")))

    except Exception as error:
        checks.checks.append({"name": "verifier_setup", "status": "FAIL", "detail": f"{type(error).__name__}: {error}"})
        result["isolation_mode"] = result.get("isolation_mode") or "not established"
    result["status"] = "PASS" if checks.checks and all(check["status"] == "PASS" for check in checks.checks) else "FAIL"
    result["checks"] = checks.checks
    _write_result(args.result_json, result)
    return 0 if result["status"] == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())

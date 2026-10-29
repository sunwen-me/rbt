#!/usr/bin/env python3
"""Build a relocatable, verified Linux x86_64 RBT release bundle."""

from __future__ import annotations

import argparse
import base64
import csv
import gzip
import hashlib
import io
import json
import os
import platform
import posixpath
import re
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import tomllib
import zipfile
from pathlib import Path
from typing import Any, Iterable

VERSION = "0.8.0"
NATIVE_VERSION = "0.10.0-rbt.2"
PLATFORM_TAG = "manylinux_2_28_x86_64"
NATIVE_LIBRARIES = ("libgcc_s.so.1", "libstdc++.so.6")
EXCLUDED_DIRS = {
    ".git", ".hg", ".svn", ".cache", ".pytest_cache", ".mypy_cache",
    ".ruff_cache", ".tox", ".nox", "__pycache__", "build", "dist",
    "node_modules", "target", "venv", ".venv",
}
EXCLUDED_SUFFIXES = {
    ".a", ".bin", ".dll", ".dylib", ".log", ".o", ".obj", ".pyc",
    ".pyd", ".pyo", ".so", ".whl",
}
GLIBC_OR_LOADER = re.compile(r"^libc\.so(?:\.|$)|^ld-linux|^ld64\.so", re.IGNORECASE)


class ReleaseError(RuntimeError):
    """A release validation or packaging failure."""


def log(message: str) -> None:
    print(f"[release] {message}", file=sys.stderr, flush=True)


def run(
    argv: list[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    capture: bool = False,
) -> subprocess.CompletedProcess[str]:
    command = " ".join(str(part) for part in argv)
    log(f"run: {command}")
    result = subprocess.run(
        argv,
        cwd=cwd,
        env=env,
        check=False,
        text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.PIPE if capture else None,
    )
    if result.returncode:
        if capture:
            if result.stdout:
                print(result.stdout, file=sys.stderr, end="")
            if result.stderr:
                print(result.stderr, file=sys.stderr, end="")
        raise ReleaseError(f"command failed with exit {result.returncode}: {command}")
    return result


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def load_toml(path: Path) -> dict[str, Any]:
    with path.open("rb") as stream:
        return tomllib.load(stream)


def rust_constant(path: Path, name: str) -> int:
    text = path.read_text(encoding="utf-8")
    match = re.search(rf"\b{name}\s*:\s*u16\s*=\s*(\d+)\s*;", text)
    if not match:
        raise ReleaseError(f"could not read {name} from {path}")
    return int(match.group(1))


def cpp_constant(path: Path, name: str) -> int:
    text = path.read_text(encoding="utf-8")
    match = re.search(rf"\bstd::uint16_t\s+{name}\s*=\s*(\d+)\s*U?\s*;", text)
    if not match:
        raise ReleaseError(f"could not read {name} from {path}")
    return int(match.group(1))


def product_identity(repo: Path) -> dict[str, Any]:
    cargo = load_toml(repo / "Cargo.toml")
    pyproject = load_toml(repo / "python/pyproject.toml")
    cmake_text = (repo / "sdk/cpp/CMakeLists.txt").read_text(encoding="utf-8")
    cmake_match = re.search(
        r"project\s*\(\s*rbt-cpp-sdk\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)",
        cmake_text,
        re.IGNORECASE,
    )
    versions = {
        "cargo": str(cargo["workspace"]["package"]["version"]),
        "python": str(pyproject["project"]["version"]),
        "cmake": cmake_match.group(1) if cmake_match else None,
    }
    if any(value != VERSION for value in versions.values()):
        raise ReleaseError(f"Cargo, Python, and CMake versions must agree at {VERSION}: {versions}")

    rust_axes = {
        "idl_language": rust_constant(repo / "crates/rbt-idl/src/lib.rs", "IDL_LANGUAGE_VERSION"),
        "message_descriptor": rust_constant(repo / "crates/rbt-idl/src/lib.rs", "MESSAGE_DESCRIPTOR_VERSION"),
        "control_protocol": rust_constant(repo / "crates/rbt-compose/src/protocol.rs", "CONTROL_PROTOCOL_VERSION"),
        "composer_snapshot": rust_constant(repo / "crates/rbt-compose/src/protocol.rs", "COMPOSER_SNAPSHOT_VERSION"),
        "mcap_file_encoding": rust_constant(repo / "crates/rbt-types/src/lib.rs", "CANONICAL_FILE_ENCODING_VERSION"),
    }
    expected_axes = {
        "idl_language": 1,
        "message_descriptor": 1,
        "control_protocol": 2,
        "composer_snapshot": 4,
        "mcap_file_encoding": 1,
    }
    if rust_axes != expected_axes:
        raise ReleaseError(f"frozen compatibility axes differ from {expected_axes}: {rust_axes}")
    header = repo / "sdk/cpp/include/rbt/version.hpp"
    cpp_axes = {
        "idl_language": cpp_constant(header, "IDL_LANGUAGE_VERSION"),
        "message_descriptor": cpp_constant(header, "MESSAGE_DESCRIPTOR_VERSION"),
        "control_protocol": cpp_constant(header, "CONTROL_PROTOCOL_VERSION"),
        "composer_snapshot": cpp_constant(header, "COMPOSER_SNAPSHOT_VERSION"),
    }
    if any(cpp_axes[key] != rust_axes[key] for key in cpp_axes):
        raise ReleaseError(f"Rust and C++ compatibility axes disagree: {rust_axes} / {cpp_axes}")
    return {
        "name": "rbt",
        "version": VERSION,
        "platform": "linux-x86_64",
        "product_versions": versions,
        "compatibility_axes": rust_axes,
        "native_version": NATIVE_VERSION,
    }


def is_within(path: Path, root: Path) -> bool:
    try:
        path.resolve().relative_to(root.resolve())
        return True
    except ValueError:
        return False


def native_fingerprint(repo: Path, prefix: Path) -> dict[str, str]:
    stamp = prefix / "share/rbt/iceoryx2-provenance.toml"
    if not stamp.is_file() or stamp.is_symlink():
        raise ReleaseError(f"native prefix provenance stamp is missing: {stamp}")
    upstream = load_toml(repo / "third_party/iceoryx2-rbt/UPSTREAM.toml")
    expected = {
        "fork_version": str(upstream.get("fork_version", "")),
        "upstream_commit": str(upstream.get("base_commit", "")),
        "source_diff_sha256": (repo / "third_party/iceoryx2-rbt/SOURCE_DIFF_SHA256")
        .read_text(encoding="utf-8").split()[0],
    }
    patch_dir = repo / "third_party/iceoryx2-rbt/patches"
    checksums = [
        f"{sha256_file(patch_dir / name)}  {name}"
        for name in ("series", "SHA256SUMS")
    ]
    expected["patch_series_sha256"] = hashlib.sha256(("\n".join(checksums) + "\n").encode()).hexdigest()
    stamp_data = load_toml(stamp)
    actual = {key: str(stamp_data.get(key, "")) for key in expected}
    for key, value in expected.items():
        if actual[key] != value:
            raise ReleaseError(
                f"native provenance mismatch for {key}: expected {value}, found {actual[key] or 'missing'}"
            )
    if actual["fork_version"] != NATIVE_VERSION:
        raise ReleaseError(f"native fork must be {NATIVE_VERSION}, found {actual['fork_version']}")
    for key in ("source_diff_sha256", "patch_series_sha256"):
        if not re.fullmatch(r"[0-9a-f]{64}", actual[key]):
            raise ReleaseError(f"invalid native fingerprint {key}: {actual[key]}")
    return actual


def merge_tree(source: Path, destination: Path) -> None:
    if source.is_symlink() or not source.is_dir():
        raise ReleaseError(f"expected a native prefix directory: {source}")
    destination.mkdir(parents=True, exist_ok=True)
    for entry in sorted(source.iterdir(), key=lambda path: path.name):
        target = destination / entry.name
        mode = entry.lstat().st_mode
        if stat.S_ISLNK(mode):
            link = os.readlink(entry)
            if target.is_symlink():
                if os.readlink(target) != link:
                    raise ReleaseError(f"native prefix merge collision: {target}")
            elif target.exists():
                raise ReleaseError(f"native prefix merge collision: {target}")
            else:
                target.symlink_to(link, target_is_directory=entry.is_dir())
        elif stat.S_ISDIR(mode):
            if target.exists() and (not target.is_dir() or target.is_symlink()):
                raise ReleaseError(f"native prefix merge collision: {target}")
            merge_tree(entry, target)
        elif stat.S_ISREG(mode):
            if target.exists() or target.is_symlink():
                if target.is_file() and not target.is_symlink() and sha256_file(target) == sha256_file(entry):
                    continue
                raise ReleaseError(f"native prefix merge collision: {target}")
            shutil.copy2(entry, target)
        else:
            raise ReleaseError(f"special file in native prefix: {entry}")


def copy_toolchain_licenses(toolchain: Path, repo: Path, sdk_root: Path) -> None:
    for name in ("gcc", "gcc-libs", "libstdc++"):
        source = toolchain / "share/licenses" / name
        destination = sdk_root / "share/licenses" / name
        if not source.is_dir():
            raise ReleaseError(f"toolchain license directory is missing: {source}")
        if destination.exists():
            raise ReleaseError(f"toolchain license destination already exists: {destination}")
        shutil.copytree(source, destination, symlinks=True)
    gcc_license = repo / "packaging/licenses/GCC-COPYING3"
    if not gcc_license.is_file():
        raise ReleaseError(f"bundled GCC license text is missing: {gcc_license}")
    shutil.copy2(gcc_license, sdk_root / "share/licenses/gcc/GCC-COPYING3")


def find_toolchain_library(toolchain: Path, name: str) -> Path:
    preferred = toolchain / "lib" / name
    if preferred.exists():
        return preferred
    matches = sorted(path for path in toolchain.rglob(name) if path.exists())
    if not matches:
        raise ReleaseError(f"required bundled runtime is missing from toolchain: {name}")
    return matches[0]


def copy_runtime_library(source: Path, destination_dir: Path) -> Path:
    destination_dir.mkdir(parents=True, exist_ok=True)
    destination = destination_dir / source.name
    if source.is_symlink():
        link = os.readlink(source)
        if Path(link).is_absolute():
            raise ReleaseError(f"absolute shared-library symlink is not relocatable: {source} -> {link}")
        target_name = Path(link).name
        target_path = destination_dir / target_name
        if not target_path.exists():
            shutil.copy2(source.resolve(strict=True), target_path)
        if destination.exists() or destination.is_symlink():
            if not destination.is_symlink() or os.readlink(destination) != target_name:
                raise ReleaseError(f"runtime library name collision: {destination}")
        else:
            destination.symlink_to(target_name)
    else:
        if destination.exists() and sha256_file(destination) != sha256_file(source):
            raise ReleaseError(f"runtime library name collision: {destination}")
        if not destination.exists():
            shutil.copy2(source, destination)
    return destination


def ldd_dependencies(path: Path, env: dict[str, str]) -> list[tuple[str, Path]]:
    result = run(["ldd", str(path)], env=env, capture=True)
    dependencies = []
    for line in (result.stdout or "").splitlines():
        match = re.match(r"\s*(\S+)\s+=>\s+(.+?)(?:\s+\(0x[0-9a-fA-F]+\))?$", line)
        if match:
            name, target = match.groups()
            if target == "not found":
                raise ReleaseError(f"unresolved ELF dependency for {path}: {name}")
            dependencies.append((name, Path(target)))
        else:
            direct = re.match(r"\s*(/.+?)\s+\(0x", line)
            if direct:
                dependencies.append((Path(direct.group(1)).name, Path(direct.group(1))))
    return dependencies


def is_system_library(path: Path) -> bool:
    resolved = path.resolve()
    roots = (Path("/lib"), Path("/lib64"), Path("/usr/lib"), Path("/usr/lib64"))
    return any(is_within(resolved, root) for root in roots)


def bundle_runtime_closure(toolchain: Path, sdk_root: Path, env: dict[str, str]) -> None:
    destination = sdk_root / "lib"
    sources = [find_toolchain_library(toolchain, name) for name in NATIVE_LIBRARIES]
    queue: list[Path] = []
    visited: set[Path] = set()
    for source in sources:
        if GLIBC_OR_LOADER.match(source.name):
            raise ReleaseError(f"refusing to bundle glibc or its loader: {source.name}")
        copy_runtime_library(source, destination)
        resolved = source.resolve(strict=True)
        queue.append(resolved)
        visited.add(resolved)
    toolchain_root = toolchain.resolve()
    while queue:
        current = queue.pop()
        for name, dependency in ldd_dependencies(current, env):
            if GLIBC_OR_LOADER.match(name) or not dependency.exists():
                if not dependency.exists() and not GLIBC_OR_LOADER.match(name):
                    raise ReleaseError(f"unresolved ELF dependency {name} needed by {current}")
                continue
            resolved = dependency.resolve()
            if GLIBC_OR_LOADER.match(resolved.name):
                continue
            if is_within(resolved, toolchain_root):
                candidate = toolchain / "lib" / name
                if not candidate.exists():
                    candidate = dependency
                copy_runtime_library(candidate, destination)
                if resolved not in visited:
                    visited.add(resolved)
                    queue.append(resolved)
            elif not is_system_library(resolved):
                copy_runtime_library(dependency, destination)
                if resolved not in visited:
                    visited.add(resolved)
                    queue.append(resolved)


def prepend_env(env: dict[str, str], key: str, value: Path) -> None:
    previous = env.get(key)
    env[key] = f"{value}{os.pathsep}{previous}" if previous else str(value)


def should_exclude(path: Path) -> bool:
    name = path.name.lower()
    if name in EXCLUDED_DIRS or name.startswith(".env") or name.endswith(tuple(EXCLUDED_SUFFIXES)):
        return True
    return bool(re.search(r"\.so(?:\.\d+)*$", name))


def collect_tree(
    root: Path,
    *,
    allowed_roots: Iterable[str] | None = None,
    filter_generated: bool = False,
) -> list[Path]:
    roots = sorted(allowed_roots) if allowed_roots is not None else ["."]
    entries: dict[str, Path] = {}

    def add(path: Path) -> None:
        entries[path.relative_to(root).as_posix()] = path

    for allowed in roots:
        start = root if allowed == "." else root / allowed
        if not start.exists() and not start.is_symlink():
            continue
        if start.is_symlink():
            add(start)
            continue
        if start.is_file():
            add(start)
            continue
        if not start.is_dir():
            raise ReleaseError(f"special file in package tree: {start}")
        for parent in reversed(start.relative_to(root).parents):
            if str(parent) != ".":
                add(root / parent)
        if start != root:
            add(start)
        stack = [start]
        while stack:
            directory = stack.pop()
            for path in sorted(directory.iterdir(), key=lambda item: item.name):
                if filter_generated and should_exclude(path):
                    continue
                mode = path.lstat().st_mode
                if stat.S_ISDIR(mode):
                    add(path)
                    stack.append(path)
                elif stat.S_ISREG(mode) or stat.S_ISLNK(mode):
                    add(path)
                else:
                    raise ReleaseError(f"special file in package tree: {path}")
    return [entries[key] for key in sorted(entries)]


def check_archive_symlinks(root: Path, entries: list[Path], archive_root: str) -> None:
    names = {archive_root}
    names.update(f"{archive_root}/{path.relative_to(root).as_posix()}" for path in entries)
    for path in entries:
        if not path.is_symlink():
            continue
        name = f"{archive_root}/{path.relative_to(root).as_posix()}"
        link = os.readlink(path)
        if Path(link).is_absolute():
            raise ReleaseError(f"absolute symlink would escape archive: {name} -> {link}")
        target = posixpath.normpath(posixpath.join(posixpath.dirname(name), link))
        if target == ".." or target.startswith("../") or target not in names:
            raise ReleaseError(f"symlink target is not packaged: {name} -> {link}")


def deterministic_tar(
    root: Path,
    entries: list[Path],
    destination: Path,
    archive_root: str,
    epoch: int,
) -> None:
    check_archive_symlinks(root, entries, archive_root)
    destination.parent.mkdir(parents=True, exist_ok=True)
    with destination.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, compresslevel=9, mtime=epoch) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                root_info = tarfile.TarInfo(archive_root)
                root_info.type = tarfile.DIRTYPE
                root_info.mode = 0o755
                root_info.uid = root_info.gid = 0
                root_info.uname = root_info.gname = ""
                root_info.mtime = epoch
                archive.addfile(root_info)
                for path in entries:
                    mode = path.lstat().st_mode
                    member = tarfile.TarInfo(f"{archive_root}/{path.relative_to(root).as_posix()}")
                    member.uid = member.gid = 0
                    member.uname = member.gname = ""
                    member.mtime = epoch
                    if stat.S_ISDIR(mode):
                        member.type = tarfile.DIRTYPE
                        member.mode = 0o755
                        archive.addfile(member)
                    elif stat.S_ISLNK(mode):
                        member.type = tarfile.SYMTYPE
                        member.mode = 0o777
                        member.linkname = os.readlink(path)
                        archive.addfile(member)
                    elif stat.S_ISREG(mode):
                        member.type = tarfile.REGTYPE
                        member.mode = 0o755 if mode & 0o111 else 0o644
                        member.size = path.stat().st_size
                        with path.open("rb") as stream:
                            archive.addfile(member, stream)
                    else:
                        raise ReleaseError(f"special file in archive: {path}")


def source_allowlist(repo: Path) -> list[str]:
    fixed = [
        "Cargo.toml", "Cargo.lock", "README.md", "THIRD_PARTY_NOTICES.md",
        ".gitignore", ".cargo", "crates", "sdk", "types", "generated",
        "python", "examples", "tests", "scripts", "packaging", "docs", "drivers",
        "third_party/iceoryx2", "third_party/iceoryx2-rbt",
    ]
    allowed = [name for name in fixed if (repo / name).exists()]
    allowed.extend(
        path.name
        for path in repo.iterdir()
        if path.is_file()
        and re.fullmatch(r"(?:LICENSE|NOTICE)(?:[-_.].*)?", path.name, re.IGNORECASE)
    )
    return sorted(set(allowed))


def source_hash(repo: Path, entries: list[Path]) -> str:
    digest = hashlib.sha256()
    for path in entries:
        relative = path.relative_to(repo).as_posix().encode("utf-8", errors="surrogateescape")
        mode = path.lstat().st_mode
        if stat.S_ISDIR(mode):
            kind, content = b"dir", b""
        elif stat.S_ISLNK(mode):
            kind, content = b"symlink", os.readlink(path).encode("utf-8", errors="surrogateescape")
        elif stat.S_ISREG(mode):
            kind, content = b"file", bytes.fromhex(sha256_file(path))
        else:
            raise ReleaseError(f"special file in source input: {path}")
        digest.update(kind + b"\0" + relative + b"\0" + (b"x" if mode & 0o111 else b"-") + b"\0" + content + b"\0")
    return digest.hexdigest()


def make_source_archive(repo: Path, destination: Path, epoch: int) -> dict[str, Any]:
    allowlist = source_allowlist(repo)
    entries = collect_tree(repo, allowed_roots=allowlist, filter_generated=True)
    digest = source_hash(repo, entries)
    deterministic_tar(repo, entries, destination, f"rbt-{VERSION}-source", epoch)
    if source_hash(repo, collect_tree(repo, allowed_roots=allowlist, filter_generated=True)) != digest:
        raise ReleaseError("source inputs changed while creating the source archive")
    return {"source_input_sha256": digest, "included_paths": len(entries), "allowlist": allowlist}


def write_sdk_environment(sdk_root: Path) -> None:
    dollar = chr(36)
    lines = [
        "# Source this file to use the relocatable RBT SDK.",
        'rbt_sdk_root="$(cd "$(dirname "$BASH_SOURCE")" && pwd)"',
        'export PATH="$rbt_sdk_root/bin' + dollar + "{PATH:+:" + dollar + 'PATH}"',
        'export CMAKE_PREFIX_PATH="$rbt_sdk_root' + dollar + "{CMAKE_PREFIX_PATH:+:" + dollar + 'CMAKE_PREFIX_PATH}"',
        'export LD_LIBRARY_PATH="$rbt_sdk_root/lib' + dollar + "{LD_LIBRARY_PATH:+:" + dollar + 'LD_LIBRARY_PATH}"',
        'export RBT_RUST_SDK="$rbt_sdk_root/share/rbt/rust"',
        "unset rbt_sdk_root",
        "",
    ]
    target = sdk_root / "env.sh"
    target.write_text("\n".join(lines), encoding="utf-8")
    target.chmod(0o644)


def collect_cargo_licenses(repo: Path, sdk_root: Path, env: dict[str, str]) -> dict[str, Any]:
    destination = sdk_root / "share/licenses/cargo"
    destination.mkdir(parents=True, exist_ok=True)
    try:
        result = run(
            ["cargo", "metadata", "--format-version", "1", "--locked", "--offline"],
            cwd=repo,
            env=env,
            capture=True,
        )
        metadata = json.loads(result.stdout or "{}")
    except (ReleaseError, json.JSONDecodeError) as error:
        return {"status": "unavailable", "reason": str(error), "files_copied": 0}
    copied = 0
    for package in sorted(metadata.get("packages", []), key=lambda item: (item["name"], item["version"])):
        if not package.get("source"):
            continue
        package_root = Path(package["manifest_path"]).parent
        candidates: set[Path] = set()
        license_file = package.get("license_file")
        if license_file:
            path = package_root / license_file
            if is_within(path, package_root) and path.is_file():
                candidates.add(path)
        else:
            try:
                candidates.update(
                    path for path in package_root.iterdir()
                    if path.is_file()
                    and re.match(r"(?:license|copying|notice)(?:[-_.].*)?$", path.name, re.IGNORECASE)
                )
            except OSError:
                continue
        for path in sorted(candidates):
            if path.is_symlink() and not is_within(path.resolve(), package_root.resolve()):
                continue
            name = re.sub(r"[^A-Za-z0-9_.+-]", "_", f"{package['name']}-{package['version']}")
            target_dir = destination / name
            target_dir.mkdir(parents=True, exist_ok=True)
            target = target_dir / path.name
            if not target.exists():
                shutil.copy2(path, target)
                copied += 1
    return {"status": "collected", "files_copied": copied}


def is_elf(path: Path) -> bool:
    try:
        with path.open("rb") as stream:
            return stream.read(4) == b"\x7fELF"
    except OSError:
        return False


def sdk_elf_paths(sdk_root: Path) -> list[Path]:
    paths: dict[Path, Path] = {}
    for root in (sdk_root / "bin", sdk_root / "lib"):
        if root.exists():
            for path in sorted(root.rglob("*")):
                if path.is_file() and not path.is_symlink() and is_elf(path):
                    paths[path.resolve()] = path
    return [paths[key] for key in sorted(paths)]


def readelf_output(readelf: str, option: str, path: Path) -> str:
    return run([readelf, option, str(path)], capture=True).stdout or ""


def patch_sdk_rpaths(sdk_root: Path, patchelf: str, readelf: str) -> list[str]:
    patched = []
    for path in sdk_elf_paths(sdk_root):
        relative = path.relative_to(sdk_root)
        dynamic = readelf_output(readelf, "-d", path)
        if "NEEDED" not in dynamic:
            continue
        expected = "$ORIGIN/../lib" if relative.parts[0] == "bin" else "$ORIGIN"
        run([patchelf, "--set-rpath", expected, str(path)])
        actual = re.findall(r"\((?:RPATH|RUNPATH)\).*?\[(.*?)\]", readelf_output(readelf, "-d", path))
        if actual != [expected]:
            raise ReleaseError(f"unexpected RPATH on {relative}: {actual}; expected {expected}")
        patched.append(relative.as_posix())
    return patched


def check_cmake_exports(sdk_root: Path, repo: Path, native_prefix: Path, toolchain: Path) -> None:
    cmake_root = sdk_root / "lib/cmake"
    if not cmake_root.is_dir():
        raise ReleaseError("SDK is missing lib/cmake")
    absolute_roots = [str(path.resolve()) for path in (repo, native_prefix, toolchain)]
    target_exports = []
    native_exports = []
    for path in sorted(cmake_root.rglob("*.cmake")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for root in absolute_roots:
            if root in text:
                raise ReleaseError(f"absolute build path remains in {path}: {root}")
        if "Targets" not in path.name:
            continue
        target_exports.append(path)
        if path.parent.name.startswith("iceoryx2-"):
            native_exports.append(path)
        for line in text.splitlines():
            if not re.search(
                r"\b(?:IMPORTED_LOCATION|IMPORTED_IMPLIB|INTERFACE_INCLUDE_DIRECTORIES|_IMPORT_CHECK_FILES_FOR_)",
                line,
            ):
                continue
            absolute = bool(re.search(r'"(?:/|[^"]*;/)', line))
            if absolute and "$" + "{_IMPORT_PREFIX}" not in line:
                raise ReleaseError(f"absolute imported path remains in {path}: {line.strip()}")
    if not target_exports or not native_exports:
        raise ReleaseError("SDK is missing CMake imported target exports")
    for path in native_exports:
        if "_IMPORT_PREFIX" not in path.read_text(encoding="utf-8", errors="replace"):
            raise ReleaseError(f"native CMake export is not relocatable: {path}")


def verify_sdk_elfs(sdk_root: Path, readelf: str, env: dict[str, str]) -> dict[str, Any]:
    paths = sdk_elf_paths(sdk_root)
    if not paths:
        raise ReleaseError("SDK contains no ELF executables or shared libraries")
    maximum: tuple[int, int] | None = None
    unresolved = []
    for path in paths:
        relative = path.relative_to(sdk_root).as_posix()
        dynamic = readelf_output(readelf, "-d", path)
        expected = "$ORIGIN/../lib" if relative.startswith("bin/") else "$ORIGIN"
        rpaths = re.findall(r"\((?:RPATH|RUNPATH)\).*?\[(.*?)\]", dynamic)
        if "NEEDED" in dynamic and rpaths != [expected]:
            raise ReleaseError(f"non-relocatable RPATH on {relative}: {rpaths}")
        if any(value.startswith("/") for value in rpaths):
            raise ReleaseError(f"absolute RPATH remains on {relative}: {rpaths}")
        for major, minor in re.findall(
            r"\bGLIBC_(\d+)\.(\d+)", readelf_output(readelf, "--version-info", path)
        ):
            current = (int(major), int(minor))
            maximum = max(maximum, current) if maximum else current
        output = run(["ldd", str(path)], env=env, capture=True).stdout or ""
        unresolved.extend(
            f"{relative}: {line.strip()}" for line in output.splitlines() if "=> not found" in line
        )
    if unresolved:
        raise ReleaseError("unresolved SDK ELF dependencies: " + "; ".join(unresolved))
    if maximum and maximum > (2, 28):
        raise ReleaseError(f"SDK requires GLIBC_{maximum[0]}.{maximum[1]}, above 2.28")
    return {
        "elf_count": len(paths),
        "maximum_glibc_symbol": f"GLIBC_{maximum[0]}.{maximum[1]}" if maximum else None,
        "elfs": [path.relative_to(sdk_root).as_posix() for path in paths],
    }


def validate_wheel_members(archive: zipfile.ZipFile) -> None:
    names = set()
    for info in archive.infolist():
        name = info.filename
        if name in names:
            raise ReleaseError(f"duplicate wheel member: {name}")
        names.add(name)
        if name.startswith("/") or "\\" in name or ".." in Path(name).parts:
            raise ReleaseError(f"unsafe wheel member path: {name}")
        if (info.external_attr >> 16) & 0o170000 == stat.S_IFLNK:
            raise ReleaseError(f"wheel contains a symlink: {name}")


def wheel_identity(path: Path, python: Path) -> dict[str, str]:
    if path.name != f"rbt-{VERSION}-cp312-cp312-{PLATFORM_TAG}.whl":
        raise ReleaseError(f"wheel filename has unexpected name, version, or tags: {path.name}")
    python_version = run([str(python), "--version"], capture=True)
    actual_python = (python_version.stdout or python_version.stderr or "").strip()
    if not re.search(r"Python 3\.12(?:\.|$)", actual_python):
        raise ReleaseError(f"wheel builder must be CPython 3.12, found {actual_python}")
    with zipfile.ZipFile(path) as archive:
        validate_wheel_members(archive)
        metadata_names = [name for name in archive.namelist() if name.endswith(".dist-info/METADATA")]
        if len(metadata_names) != 1:
            raise ReleaseError(f"wheel must contain one dist-info METADATA file: {metadata_names}")
        dist_info = metadata_names[0].split("/", 1)[0]
        metadata = archive.read(metadata_names[0]).decode("utf-8")
        wheel_metadata = archive.read(f"{dist_info}/WHEEL").decode("utf-8")
    fields: dict[str, list[str]] = {}
    for line in metadata.splitlines():
        if line and not line[0].isspace() and ":" in line:
            key, value = line.split(":", 1)
            fields.setdefault(key.lower(), []).append(value.strip())
    tags = [line.split(":", 1)[1].strip() for line in wheel_metadata.splitlines() if line.startswith("Tag:")]
    expected = f"cp312-cp312-{PLATFORM_TAG}"
    if [value.lower() for value in fields.get("name", [])] != ["rbt"]:
        raise ReleaseError(f"wheel METADATA Name must be rbt, found {fields.get('name')}")
    if fields.get("version") != [VERSION]:
        raise ReleaseError(f"wheel METADATA Version must be {VERSION}, found {fields.get('version')}")
    if tags != [expected] or "Root-Is-Purelib: false" not in wheel_metadata:
        raise ReleaseError(f"wheel WHEEL metadata must declare only {expected}, found {tags}")
    return {"dist_info": dist_info, "tag": expected}


def wheel_elf_names(path: Path) -> list[str]:
    with zipfile.ZipFile(path) as archive:
        return sorted(name for name in archive.namelist() if name.endswith(".so") or ".so." in name)


def patch_wheel_rpaths(
    path: Path, work_dir: Path, patchelf: str, readelf: str
) -> list[str]:
    if not wheel_elf_names(path):
        raise ReleaseError("wheel contains no native ELF extension")
    extracted = work_dir / "wheel-rpath-work"
    if extracted.exists():
        shutil.rmtree(extracted)
    extracted.mkdir()
    with zipfile.ZipFile(path) as archive:
        validate_wheel_members(archive)
        archive.extractall(extracted)
    changed = []
    for elf in sorted(extracted.rglob("*")):
        if not elf.is_file() or not is_elf(elf):
            continue
        dynamic = readelf_output(readelf, "-d", elf)
        if re.search(r"\((?:RPATH|RUNPATH)\)", dynamic):
            run([patchelf, "--remove-rpath", str(elf)])
            if re.search(r"\((?:RPATH|RUNPATH)\)", readelf_output(readelf, "-d", elf)):
                raise ReleaseError(f"wheel extension retains RPATH: {elf.relative_to(extracted)}")
            changed.append(elf.relative_to(extracted).as_posix())
    rebuilt = work_dir / "wheel-rpath-fixed.whl"
    with zipfile.ZipFile(rebuilt, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for member in sorted(extracted.rglob("*")):
            if member.is_file():
                archive.write(member, member.relative_to(extracted).as_posix())
    rebuilt.replace(path)
    return changed


def inject_wheel_licenses(
    source: Path, destination: Path, repo: Path, python: Path, epoch: int
) -> list[str]:
    dist_info = wheel_identity(source, python)["dist_info"]
    inputs = [
        ("rbt/LICENSE-APACHE", repo / "LICENSE-APACHE"),
        ("rbt/LICENSE-MIT", repo / "LICENSE-MIT"),
        ("rbt/THIRD_PARTY_NOTICES.md", repo / "THIRD_PARTY_NOTICES.md"),
        ("iceoryx2-rbt/LICENSE-APACHE", repo / "third_party/iceoryx2-rbt/source/LICENSE-APACHE"),
        ("iceoryx2-rbt/LICENSE-MIT", repo / "third_party/iceoryx2-rbt/source/LICENSE-MIT"),
        ("iceoryx2-rbt/NOTICE.md", repo / "third_party/iceoryx2-rbt/source/NOTICE.md"),
    ]
    for _, path in inputs:
        if not path.is_file():
            raise ReleaseError(f"wheel license input is missing: {path}")
    license_names = [f"{dist_info}/licenses/{name}" for name, _ in inputs]
    with zipfile.ZipFile(source) as archive:
        validate_wheel_members(archive)
        entries = {info.filename: (info, archive.read(info.filename)) for info in archive.infolist()}
    metadata_name, record_name = f"{dist_info}/METADATA", f"{dist_info}/RECORD"
    if metadata_name not in entries or record_name not in entries:
        raise ReleaseError("wheel is missing METADATA or RECORD")
    metadata = entries[metadata_name][1].decode("utf-8")
    headers, separator, body = metadata.partition("\n\n")
    if not separator:
        raise ReleaseError("wheel METADATA is missing its header/body separator")
    header_lines = [line for line in headers.splitlines() if not line.lower().startswith("license-file:")]
    header_lines.extend(f"License-File: {name[len(dist_info) + len('/licenses/'):]}" for name in license_names)
    entries[metadata_name] = (
        entries[metadata_name][0],
        ("\n".join(header_lines) + "\n\n" + body).encode("utf-8"),
    )
    entries.pop(record_name)
    for (_, path), name in zip(inputs, license_names):
        entries[name] = (zipfile.ZipInfo(name), path.read_bytes())

    buffer = io.StringIO(newline="")
    writer = csv.writer(buffer, lineterminator="\n")
    for name in sorted(entries):
        content = entries[name][1]
        digest = base64.urlsafe_b64encode(hashlib.sha256(content).digest()).decode().rstrip("=")
        writer.writerow((name, f"sha256={digest}", str(len(content))))
    writer.writerow((record_name, "", ""))
    entries[record_name] = (zipfile.ZipInfo(record_name), buffer.getvalue().encode("utf-8"))

    date_time = time.gmtime(max(epoch, 315532800))[:6]
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for name in sorted(entries):
            old_info, content = entries[name]
            info = zipfile.ZipInfo(name, date_time=date_time)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            mode = (old_info.external_attr >> 16) & 0o777
            info.external_attr = ((mode or 0o644) & 0o777) << 16
            archive.writestr(info, content)
    return [name[len(dist_info) + 1:] for name in license_names]


def max_wheel_glibc(path: Path, readelf: str, work_dir: Path) -> str | None:
    maximum: tuple[int, int] | None = None
    with zipfile.ZipFile(path) as archive:
        for name in wheel_elf_names(path):
            descriptor, filename = tempfile.mkstemp(prefix="rbt-wheel-elf-", suffix=".so", dir=work_dir)
            os.close(descriptor)
            temporary = Path(filename)
            try:
                temporary.write_bytes(archive.read(name))
                output = readelf_output(readelf, "--version-info", temporary)
            finally:
                temporary.unlink(missing_ok=True)
            for major, minor in re.findall(r"\bGLIBC_(\d+)\.(\d+)", output):
                current = (int(major), int(minor))
                maximum = max(maximum, current) if maximum else current
    return f"GLIBC_{maximum[0]}.{maximum[1]}" if maximum else None


def transform_wheel(
    source: Path,
    destination_dir: Path,
    repo: Path,
    work_dir: Path,
    python: Path,
    patchelf: str,
    readelf: str,
    auditwheel: str,
    epoch: int,
    prebuilt: bool,
) -> dict[str, Any]:
    if not source.is_file():
        raise ReleaseError(f"Python wheel is missing: {source}")
    wheel_identity(source, python)
    input_hash = sha256_file(source)
    destination_dir.mkdir(parents=True, exist_ok=True)
    wheel = destination_dir / source.name
    shutil.copy2(source, wheel)
    changed = patch_wheel_rpaths(wheel, work_dir, patchelf, readelf)
    licensed = work_dir / "wheel-licensed.whl"
    license_names = inject_wheel_licenses(wheel, licensed, repo, python, epoch)
    licensed.replace(wheel)
    metadata = wheel_identity(wheel, python)
    audit = run([auditwheel, "show", "--json", str(wheel)], capture=True)
    try:
        policy = json.loads(audit.stdout or "{}")
    except json.JSONDecodeError as error:
        raise ReleaseError(f"auditwheel returned invalid JSON: {error}") from error
    overall = str(policy.get("overall_tag", ""))
    match = re.fullmatch(r"manylinux_2_(\d+)_x86_64", overall)
    if not match or int(match.group(1)) > 28:
        raise ReleaseError(f"wheel policy {overall!r} exceeds the declared {PLATFORM_TAG} tag")
    if policy.get("unsupported_isa"):
        raise ReleaseError("auditwheel reports unsupported x86_64 ISA requirements")
    if policy.get("external_libs"):
        raise ReleaseError(f"wheel has unresolved external libraries: {policy['external_libs']}")
    glibc = max_wheel_glibc(wheel, readelf, work_dir)
    glibc_match = re.fullmatch(r"GLIBC_(\d+)\.(\d+)", glibc or "")
    if glibc_match and (int(glibc_match.group(1)), int(glibc_match.group(2))) > (2, 28):
        raise ReleaseError(f"wheel requires {glibc}, above GLIBC_2.28")
    return {
        "filename": wheel.name,
        "input_sha256": input_hash,
        "sha256": sha256_file(wheel),
        "size": wheel.stat().st_size,
        "prebuilt": prebuilt,
        "metadata": metadata,
        "transformations": [
            f"remove extension RPATHs from: {', '.join(changed) if changed else 'none present'}",
            "inject RBT and native license/notice files with License-File metadata",
            "rebuild wheel RECORD hashes and sizes",
            "repack with normalized ZIP timestamps",
        ],
        "extension_rpaths_removed": changed,
        "license_files": license_names,
        "auditwheel": {
            "overall_tag": overall,
            "symbol_tag": policy.get("sym_tag"),
            "external_libs": policy.get("external_libs", {}),
        },
        "maximum_glibc_symbol": glibc,
    }


def source_epoch() -> int:
    value = os.environ.get("SOURCE_DATE_EPOCH", "0")
    try:
        epoch = int(value)
    except ValueError as error:
        raise ReleaseError("SOURCE_DATE_EPOCH must be an integer") from error
    if epoch < 0:
        raise ReleaseError("SOURCE_DATE_EPOCH must not be negative")
    return epoch


def resolve_executable(value: str, repo: Path) -> Path:
    path = Path(value).expanduser()
    if not path.is_absolute() and ("/" in value or (repo / path).exists()):
        path = repo / path if not path.is_absolute() else path
    if path.exists():
        return path.absolute()
    found = shutil.which(value)
    if found:
        return Path(found).absolute()
    raise ReleaseError(f"executable does not exist or is not on PATH: {value}")


def validate_python(python: Path) -> str:
    result = run(
        [str(python), "-c", "import platform; print(platform.python_implementation()+' '+platform.python_version())"],
        capture=True,
    )
    version = (result.stdout or "").strip()
    if not version.startswith("CPython 3.12."):
        raise ReleaseError(f"--python must select CPython 3.12, found {version}")
    return version


def tool_version(command: str, *arguments: str) -> str:
    result = run([command, *arguments], capture=True)
    return (result.stdout or result.stderr or "").strip().splitlines()[0]


def check_output_dir(output: Path) -> None:
    if output.is_symlink():
        raise ReleaseError(f"refusing symlink output directory: {output}")
    if output.exists():
        if not output.is_dir():
            raise ReleaseError(f"output path is not a directory: {output}")
        if next(output.iterdir(), None) is not None:
            raise ReleaseError(f"refusing existing nonempty output directory: {output}")


def reject_source_input_path(path: Path, repo: Path) -> None:
    if not is_within(path, repo):
        return
    relative = path.resolve(strict=False).relative_to(repo.resolve()).as_posix()
    for name in source_allowlist(repo):
        allowed = repo / name
        if allowed.is_dir() and (relative == name or relative.startswith(name + "/")):
            raise ReleaseError(f"build output/work path overlaps source input: {path}")
        if relative == name:
            raise ReleaseError(f"build output/work path overlaps source input: {path}")


def fresh_work_dir(path: Path, output: Path, repo: Path) -> Path:
    reject_source_input_path(path, repo)
    resolved = path.resolve(strict=False)
    target = output.resolve(strict=False)
    if is_within(resolved, target) or is_within(target, resolved):
        raise ReleaseError("--work-dir and --output-dir must not contain one another")
    if path.exists():
        if not path.is_dir() or path.is_symlink():
            raise ReleaseError(f"work directory must be a real directory: {path}")
        if next(path.iterdir(), None) is not None:
            raise ReleaseError(f"refusing existing nonempty work directory: {path}")
    path.mkdir(parents=True, exist_ok=True)
    return path.resolve()


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n", encoding="utf-8")


def artifact_record(path: Path) -> dict[str, Any]:
    return {"filename": path.name, "sha256": sha256_file(path), "size": path.stat().st_size}


def write_checksums(stage: Path, filenames: list[str]) -> None:
    lines = [f"{sha256_file(stage / name)}  {name}" for name in sorted(filenames)]
    (stage / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="ascii")


def build_release(args: argparse.Namespace, repo: Path, identity: dict[str, Any]) -> Path:
    if platform.system() != "Linux" or platform.machine().lower() not in {"x86_64", "amd64"}:
        raise ReleaseError("release packaging requires a Linux x86_64 builder")
    output = (
        Path(args.output_dir).expanduser().absolute()
        if args.output_dir
        else (repo / "dist" / f"rbt-{VERSION}").absolute()
    )
    reject_source_input_path(output, repo)
    check_output_dir(output)
    output.parent.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    tool_bin = repo / "target/release-tools-venv/bin"
    if tool_bin.is_dir():
        prepend_env(env, "PATH", tool_bin)
    tools = {}
    for name in ("patchelf", "auditwheel", "readelf", "ldd"):
        executable = shutil.which(name, path=env.get("PATH"))
        if not executable:
            raise ReleaseError(f"required release tool is missing from PATH: {name}")
        tools[name] = str(Path(executable).resolve())
    patchelf, auditwheel, readelf = tools["patchelf"], tools["auditwheel"], tools["readelf"]

    python = resolve_executable(args.python_executable, repo)
    builder_python_version = validate_python(python)
    native_prefix = Path(args.native_prefix).expanduser().resolve()
    toolchain = Path(args.toolchain).expanduser().resolve()
    if not native_prefix.is_dir():
        raise ReleaseError(f"native prefix does not exist: {native_prefix}")
    if not toolchain.is_dir():
        raise ReleaseError(f"toolchain does not exist: {toolchain}")
    prepend_env(env, "PATH", toolchain / "bin")
    env["CC"] = str(toolchain / "bin/x86_64-conda-linux-gnu-gcc")
    env["CXX"] = str(toolchain / "bin/x86_64-conda-linux-gnu-g++")
    env["AR"] = str(toolchain / "bin/x86_64-conda-linux-gnu-ar")
    env["CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER"] = env["CC"]
    env["RBT_IOX2_PREFIX"] = str(native_prefix)
    env["RBT_PHASE4_TOOLCHAIN"] = str(toolchain)
    prepend_env(env, "LD_LIBRARY_PATH", toolchain / "lib")
    prepend_env(env, "LD_LIBRARY_PATH", native_prefix / "lib")
    prepend_env(env, "LIBRARY_PATH", toolchain / "lib")
    prepend_env(env, "LIBRARY_PATH", native_prefix / "lib")

    keep_work = bool(args.work_dir)
    work_arg = Path(args.work_dir).expanduser().absolute() if args.work_dir else None
    if work_arg is None:
        work_arg = Path(tempfile.mkdtemp(prefix=".rbt-release-work-", dir=output.parent))
    work = fresh_work_dir(work_arg, output, repo)
    stage = Path(tempfile.mkdtemp(prefix=f".{output.name}.staging-", dir=output.parent))
    published = False
    try:
        sdk_root = work / "sdk-root"
        run(
            ["bash", str(repo / "scripts/install_v01.sh"), "--prefix", str(sdk_root)],
            cwd=repo,
            env=env,
        )
        run(
            ["bash", str(repo / "scripts/check_iceoryx2_rbt_prefix.sh"), str(native_prefix)],
            cwd=repo,
            env=env,
        )
        fingerprint = native_fingerprint(repo, native_prefix)
        shutil.copytree(repo / "examples", sdk_root / "share/rbt/examples", ignore=shutil.ignore_patterns("target", "__pycache__"))
        for component in ("include", "lib", "share"):
            source = native_prefix / component
            if source.is_dir():
                merge_tree(source, sdk_root / component)

        copy_toolchain_licenses(toolchain, repo, sdk_root)
        bundle_runtime_closure(toolchain, sdk_root, env)
        cargo_licenses = collect_cargo_licenses(repo, sdk_root, env)
        write_sdk_environment(sdk_root)
        write_json(
            sdk_root / "share/rbt/release-metadata.json",
            {**identity, "native_fingerprint": fingerprint, "runtime_libraries": list(NATIVE_LIBRARIES)},
        )
        check_cmake_exports(sdk_root, repo, native_prefix, toolchain)

        sdk_check_env = env.copy()
        sdk_check_env["LD_LIBRARY_PATH"] = str(sdk_root / "lib")
        patched_elfs = patch_sdk_rpaths(sdk_root, patchelf, readelf)
        sdk_elf_report = verify_sdk_elfs(sdk_root, readelf, sdk_check_env)
        epoch = source_epoch()
        sdk_name = f"rbt-sdk-{VERSION}-linux-x86_64.tar.gz"
        sdk_archive_path = work / sdk_name
        deterministic_tar(
            sdk_root,
            collect_tree(sdk_root),
            sdk_archive_path,
            f"rbt-sdk-{VERSION}-linux-x86_64",
            epoch,
        )

        prebuilt = bool(args.python_wheel)
        if prebuilt:
            wheel_input = Path(args.python_wheel).expanduser().resolve()
        else:
            wheel_build = work / "wheel-input"
            wheel_build.mkdir()
            run(
                [
                    str(python), "-m", "maturin", "build", "--release", "--locked",
                    "--manylinux", "2_28", "--interpreter", str(python), "--out", str(wheel_build),
                ],
                cwd=repo / "python",
                env=env,
            )
            built = sorted(wheel_build.glob("*.whl"))
            if len(built) != 1:
                raise ReleaseError(f"maturin must produce one wheel, found {built}")
            wheel_input = built[0]
        wheel_dir = work / "wheel-normalized"
        wheel_dir.mkdir()
        wheel_record = transform_wheel(
            wheel_input, wheel_dir, repo, work, python, patchelf, readelf,
            auditwheel, epoch, prebuilt,
        )

        source_name = f"rbt-{VERSION}-source.tar.gz"
        source_archive_path = work / source_name
        source_record = make_source_archive(repo, source_archive_path, epoch)
        verifier = repo / "packaging/verify_release.py"
        if not verifier.is_file():
            raise ReleaseError(f"required independent verifier is missing: {verifier}")
        verifier_work = Path(tempfile.mkdtemp(prefix="rbt-release-verifier-", dir="/tmp"))
        verification_path = work / "verification.json"
        run(
            [
                str(python), str(verifier),
                "--sdk-archive", str(sdk_archive_path),
                "--source-archive", str(source_archive_path),
                "--wheel", str(wheel_dir / wheel_record["filename"]),
                "--python", str(python),
                "--toolchain", str(toolchain),
                "--work-dir", str(verifier_work),
                "--result-json", str(verification_path),
            ],
            cwd=repo,
            env=env,
        )
        if not verification_path.is_file():
            raise ReleaseError("release verifier exited successfully without result JSON")
        try:
            verification = json.loads(verification_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError as error:
            raise ReleaseError(f"release verifier result JSON is invalid: {error}") from error
        if not isinstance(verification, dict) or verification.get("status") != "PASS":
            status = verification.get("status") if isinstance(verification, dict) else type(verification).__name__
            raise ReleaseError(f"release verifier must report top-level status PASS, found {status}")
        shutil.copy2(verification_path, stage / "verification.json")
        shutil.rmtree(verifier_work)

        shutil.copy2(sdk_archive_path, stage / sdk_name)
        shutil.copy2(source_archive_path, stage / source_name)
        shutil.copy2(wheel_dir / wheel_record["filename"], stage / wheel_record["filename"])
        sdk_record = artifact_record(stage / sdk_name)
        source_record["archive"] = artifact_record(stage / source_name)
        wheel_record["sha256"] = sha256_file(stage / wheel_record["filename"])
        wheel_record["size"] = (stage / wheel_record["filename"]).stat().st_size
        verification_record = artifact_record(verification_path)
        tool_versions = {
            "python_builder": builder_python_version,
            "rustc": tool_version("rustc", "--version"),
            "cargo": tool_version("cargo", "--version"),
            "cmake": tool_version("cmake", "--version"),
            "ninja": tool_version(str(toolchain / "bin/ninja"), "--version"),
            "patchelf": tool_version(patchelf, "--version"),
            "auditwheel": tool_version(auditwheel, "--version"),
            "readelf": tool_version(readelf, "--version"),
        }
        manifest = {
            "schema_version": 1,
            "product": identity,
            "native_fingerprint": fingerprint,
            "tool_versions": tool_versions,
            "toolchain": {"target": "x86_64-unknown-linux-gnu"},
            "artifacts": {
                "sdk_archive": sdk_record,
                "source_archive": source_record["archive"],
                "wheel": wheel_record,
                "verification": verification_record,
            },
            "sdk": {
                "root": f"rbt-sdk-{VERSION}-linux-x86_64",
                "runtime_libraries": list(NATIVE_LIBRARIES),
                "cargo_licenses": cargo_licenses,
                "patched_elfs": patched_elfs,
                "elf_requirements": sdk_elf_report,
            },
            "source": source_record,
            "verification": verification,
            "verification_scope": (
                "Independent release verifier passed. This records packaging and relocation checks "
                "only; it does not claim a full regression suite or clean-container cross-distribution validation."
            ),
        }
        write_json(stage / "release.json", manifest)
        write_checksums(
            stage,
            [sdk_name, source_name, wheel_record["filename"], "release.json", "verification.json"],
        )

        check_output_dir(output)
        try:
            os.replace(stage, output)
        except OSError as error:
            raise ReleaseError(f"could not atomically publish {output}: {error}") from error
        published = True
        log(f"release PASS: {output}")
        return output
    finally:
        if not published and stage.exists():
            shutil.rmtree(stage)
        if not keep_work and work.exists():
            shutil.rmtree(work)


def parse_args(argv: list[str], repo: Path) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build and verify a relocatable Linux x86_64 RBT release."
    )
    parser.add_argument("--output-dir", help="publish directory (default: dist/rbt-0.8.0)")
    parser.add_argument("--work-dir", help="optional fresh work directory to retain build and verifier files")
    parser.add_argument(
        "--python", dest="python_executable",
        default=str(repo / "target/python-verify-venv/bin/python"),
        help="CPython 3.12 builder and verifier executable",
    )
    parser.add_argument("--python-wheel", help="prebuilt cp312 manylinux_2_28 wheel")
    parser.add_argument(
        "--native-prefix",
        default=os.environ.get("RBT_IOX2_PREFIX", str(repo / "target/iox2-rbt-0.10.0-cpp-install")),
        help="validated iceoryx2-rbt native install prefix",
    )
    parser.add_argument(
        "--toolchain",
        default=os.environ.get("RBT_PHASE4_TOOLCHAIN", str(repo / "target/phase4-toolchain")),
        help="Linux x86_64 compiler and runtime toolchain root",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    if sys.version_info < (3, 11):
        print("ERROR: release packaging requires Python 3.11 or newer", file=sys.stderr)
        return 2
    repo = Path(__file__).resolve().parent.parent
    try:
        identity = product_identity(repo)
        args = parse_args(sys.argv[1:] if argv is None else argv, repo)
        output = build_release(args, repo, identity)
    except (ReleaseError, OSError, KeyError, ValueError, tarfile.TarError, zipfile.BadZipFile) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

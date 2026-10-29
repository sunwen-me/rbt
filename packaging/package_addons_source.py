#!/usr/bin/env python3
"""Package the ARM64 Orbbec and M3T source add-ons."""

from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from pathlib import Path
from typing import Any


BUNDLE_NAME = "rbt-orbbec-m3t-source-20261023"
M3T_COMMIT = "f02106186a1e756450226b3b793e309506817c58"
ORBBEC_SDK_VERSION = "2.10.6"
RBT_REQUIREMENT = "0.8.0"
EXPECTED_ELF_MACHINE = 183  # EM_AARCH64
EXCLUDED_DIR_NAMES = {
    ".git", "__pycache__", "build", "target", "dist", ".venv",
}
EXCLUDED_BINARY_SUFFIXES = {".a", ".o", ".pyc"}
EXCLUDED_MEDIA_SUFFIXES = {
    # Meshes and captured camera/media data are external model/runtime assets.
    ".3ds", ".3mf", ".avi", ".bmp", ".dae", ".fbx", ".glb", ".gltf",
    ".heic", ".jpeg", ".jpg", ".m4v", ".mcap", ".mkv", ".mp4", ".obj",
    ".pcd", ".png", ".ply", ".stl", ".tif", ".tiff", ".webp",
}
EXCLUDED_MEDIA_DIR_NAMES = {
    "camera-snapshots", "camera_snapshots", "captures", "meshes", "recordings",
    "snapshots",
}
TEMPLATE_FILES = (
    "CMakeLists.txt",
    "build.sh",
    "env.sh.in",
    "README-ARM64.md",
    "verify_source.py",
)


class PackageError(RuntimeError):
    """A source packaging or validation failure."""


def log(message: str) -> None:
    print(f"[addons-source] {message}", file=sys.stderr, flush=True)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def checked_run(argv: list[str], *, cwd: Path | None = None) -> str:
    try:
        result = subprocess.run(
            argv, cwd=cwd, check=True, capture_output=True, text=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        detail = ""
        if isinstance(error, subprocess.CalledProcessError):
            detail = (error.stderr or error.stdout or "").strip()
        command = " ".join(str(part) for part in argv)
        raise PackageError(f"command failed: {command}{': ' + detail if detail else ''}") from error
    return result.stdout.strip()


def ensure_real_directory(path: Path, description: str) -> None:
    if path.is_symlink() or not path.is_dir():
        raise PackageError(f"{description} is not a directory: {path}")


def is_relative_to(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def reject_output_overlap(output: Path, inputs: list[Path]) -> None:
    for source in inputs:
        source_resolved = source.resolve()
        if is_relative_to(output, source_resolved):
            raise PackageError(f"output directory must not be inside an input tree: {source}")


def validate_m3t_checkout(upstream: Path) -> Path:
    ensure_real_directory(upstream, "M3T upstream checkout")
    if not (upstream / ".git").exists():
        raise PackageError(f"M3T upstream is not a Git checkout: {upstream}")
    head = checked_run(["git", "-C", str(upstream), "rev-parse", "HEAD"])
    if head != M3T_COMMIT:
        raise PackageError(f"M3T upstream must be pinned at {M3T_COMMIT}; found {head}")
    status = checked_run([
        "git", "-C", str(upstream), "status", "--porcelain=v1", "--untracked-files=all",
    ])
    if status:
        raise PackageError(f"M3T upstream checkout is not clean: {upstream}")
    m3t_root = upstream / "M3T"
    for required in (m3t_root / "src", m3t_root / "include", m3t_root / "third_party", upstream / "LICENSE"):
        if not required.exists():
            raise PackageError(f"required pinned M3T source path is missing: {required}")
    return upstream


def excluded_module_path(relative: Path) -> bool:
    if any(part in EXCLUDED_DIR_NAMES or part.lower() in EXCLUDED_MEDIA_DIR_NAMES for part in relative.parts[:-1]):
        return True
    name = relative.name.lower()
    suffix = relative.suffix.lower()
    if suffix in EXCLUDED_BINARY_SUFFIXES or suffix in EXCLUDED_MEDIA_SUFFIXES:
        return True
    # Exclude versioned shared objects as well as files whose suffix is just .so.
    if name.endswith(".so") or ".so." in name:
        return True
    return False


def set_directory_mode(path: Path, mode: int) -> None:
    path.mkdir(parents=True, exist_ok=True)
    path.chmod(stat.S_IMODE(mode))


def copy_filtered_tree(source: Path, destination: Path) -> None:
    ensure_real_directory(source, "source module tree")
    set_directory_mode(destination, source.stat().st_mode)
    for entry in sorted(source.iterdir(), key=lambda item: item.name):
        relative = Path(entry.name)
        if entry.name in EXCLUDED_DIR_NAMES or entry.name.lower() in EXCLUDED_MEDIA_DIR_NAMES:
            continue
        target = destination / entry.name
        mode = entry.lstat().st_mode
        if stat.S_ISDIR(mode):
            copy_filtered_tree(entry, target)
        elif stat.S_ISLNK(mode):
            if excluded_module_path(relative):
                continue
            target.symlink_to(os.readlink(entry), target_is_directory=entry.is_dir())
        elif stat.S_ISREG(mode):
            if excluded_module_path(relative):
                continue
            shutil.copy2(entry, target)
        else:
            raise PackageError(f"special file in source module tree: {entry}")


def copy_tree_exact(source: Path, destination: Path) -> None:
    ensure_real_directory(source, "source tree")
    set_directory_mode(destination, source.stat().st_mode)
    for entry in sorted(source.iterdir(), key=lambda item: item.name):
        target = destination / entry.name
        mode = entry.lstat().st_mode
        if stat.S_ISDIR(mode):
            copy_tree_exact(entry, target)
        elif stat.S_ISLNK(mode):
            target.symlink_to(os.readlink(entry), target_is_directory=entry.is_dir())
        elif stat.S_ISREG(mode):
            shutil.copy2(entry, target)
        else:
            raise PackageError(f"special file in source tree: {entry}")


def validate_bundle_symlinks(root: Path) -> None:
    root_resolved = root.resolve(strict=True)
    for path in iter_symlinks(root):
        target = os.readlink(path)
        if not target or os.path.isabs(target):
            raise PackageError(f"bundle symlink must have a relative target: {path.relative_to(root)}")
        try:
            resolved = path.resolve(strict=True)
        except (OSError, RuntimeError) as error:
            raise PackageError(f"bundle symlink does not resolve: {path.relative_to(root)} -> {target}") from error
        if not is_relative_to(resolved, root_resolved):
            raise PackageError(f"bundle symlink escapes its root: {path.relative_to(root)} -> {target}")


def iter_entries(root: Path) -> list[tuple[str, Path, int]]:
    entries: list[tuple[str, Path, int]] = []
    for current, directories, files in os.walk(root, topdown=True, followlinks=False):
        current_path = Path(current)
        directories.sort()
        files.sort()
        retained_dirs: list[str] = []
        for name in directories:
            path = current_path / name
            mode = path.lstat().st_mode
            if stat.S_ISLNK(mode):
                entries.append((path.relative_to(root).as_posix(), path, mode))
            else:
                entries.append((path.relative_to(root).as_posix(), path, mode))
                retained_dirs.append(name)
        directories[:] = retained_dirs
        for name in files:
            path = current_path / name
            entries.append((path.relative_to(root).as_posix(), path, path.lstat().st_mode))
    entries.sort(key=lambda item: item[0])
    return entries


def iter_symlinks(root: Path) -> list[Path]:
    return [path for _, path, mode in iter_entries(root) if stat.S_ISLNK(mode)]


def file_manifest(root: Path) -> dict[str, Any]:
    files: dict[str, Any] = {}
    for relative, path, mode in iter_entries(root):
        if stat.S_ISDIR(mode):
            continue
        if stat.S_ISLNK(mode):
            files[relative] = {
                "type": "symlink",
                "target": os.readlink(path),
                "mode": stat.S_IMODE(mode),
            }
        elif stat.S_ISREG(mode):
            files[relative] = {
                "type": "file",
                "sha256": sha256_file(path),
                "mode": stat.S_IMODE(mode),
            }
        else:
            raise PackageError(f"special file in bundle: {relative}")
    return files


def validate_arm64_elf(path: Path, display_path: str) -> bool:
    try:
        with path.open("rb") as source:
            header = source.read(20)
    except OSError as error:
        raise PackageError(f"cannot read vendor SDK file {display_path}: {error}") from error
    if len(header) < 4 or header[:4] != b"\x7fELF":
        return False
    if len(header) < 20:
        raise PackageError(f"truncated ELF file in vendor SDK: {display_path}")
    if header[4] != 2:
        raise PackageError(f"vendor SDK ELF is not ELF64: {display_path}")
    if header[5] != 1:
        raise PackageError(f"vendor SDK ELF is not little-endian: {display_path}")
    machine = struct.unpack_from("<H", header, 18)[0]
    if machine != EXPECTED_ELF_MACHINE:
        raise PackageError(
            f"vendor SDK ELF is not AArch64 (e_machine={machine}): {display_path}"
        )
    return True


def validate_vendor_sdk(prefix: Path) -> list[dict[str, str]]:
    ensure_real_directory(prefix, "OrbbecSDK staging prefix")
    required_paths = (
        prefix / "include",
        prefix / "lib",
        prefix / "licenses",
        prefix / "share/orbbec/udev",
        prefix / "share/rbt-orbbec",
    )
    for required in required_paths:
        ensure_real_directory(required, "required OrbbecSDK prefix path")
    if not any((prefix / "include").rglob("*")):
        raise PackageError("OrbbecSDK prefix has no headers")
    if not any((prefix / "lib").rglob("*")):
        raise PackageError("OrbbecSDK prefix has no libraries or CMake metadata")
    if not any((prefix / "licenses").rglob("*")):
        raise PackageError("OrbbecSDK prefix has no license files")
    if not any((prefix / "share/orbbec/udev").rglob("*")):
        raise PackageError("OrbbecSDK prefix has no udev rules")

    metadata_root = prefix / "share/rbt-orbbec"
    metadata_files: list[dict[str, str]] = []
    for relative, path, mode in iter_entries(metadata_root):
        if stat.S_ISDIR(mode):
            continue
        if stat.S_ISLNK(mode):
            # Hash the recorded relative target as provenance metadata too.
            digest = sha256_bytes(os.readlink(path).encode("utf-8"))
        elif stat.S_ISREG(mode):
            digest = sha256_file(path)
        else:
            raise PackageError(f"special file in OrbbecSDK provenance metadata: {path}")
        metadata_files.append({
            "path": (Path("share/rbt-orbbec") / relative).as_posix(),
            "sha256": digest,
        })
    if not metadata_files:
        raise PackageError("OrbbecSDK prefix has no provenance metadata under share/rbt-orbbec")

    elf_count = 0
    for relative, path, mode in iter_entries(prefix):
        if stat.S_ISREG(mode):
            elf_count += validate_arm64_elf(path, relative)
        elif stat.S_ISLNK(mode):
            try:
                resolved = path.resolve(strict=True)
            except (OSError, RuntimeError) as error:
                raise PackageError(f"OrbbecSDK symlink does not resolve: {relative}") from error
            if not is_relative_to(resolved, prefix.resolve()):
                raise PackageError(f"OrbbecSDK symlink escapes its prefix: {relative}")
            if resolved.is_file():
                elf_count += validate_arm64_elf(resolved, relative)
        elif not stat.S_ISDIR(mode):
            raise PackageError(f"special file in OrbbecSDK prefix: {relative}")
    if not elf_count:
        raise PackageError("OrbbecSDK prefix contains no ARM64 ELF libraries")
    return metadata_files


def cmake_project_version(path: Path, project_name: str) -> str:
    text = path.read_text(encoding="utf-8")
    match = re.search(
        rf"\bproject\s*\(\s*{re.escape(project_name)}\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)",
        text,
        re.IGNORECASE,
    )
    if not match:
        raise PackageError(f"cannot read {project_name} version from {path}")
    return match.group(1)


def package_metadata(repo: Path, sdk_metadata: list[dict[str, str]]) -> dict[str, Any]:
    orbbec_version = cmake_project_version(repo / "drivers/orbbec/CMakeLists.txt", "rbt-orbbec-driver")
    m3t_version = cmake_project_version(repo / "perception/m3t/CMakeLists.txt", "rbt-m3t")
    return {
        "schema_version": 1,
        "bundle": BUNDLE_NAME,
        "module_versions": {
            "orbbec": orbbec_version,
            "m3t": m3t_version,
        },
        "rbt_requirement": RBT_REQUIREMENT,
        "architectures": ["linux-aarch64"],
        "ARM_BUILD": "NOT_RUN",
        "vendor_sdk": {
            "name": "OrbbecSDK",
            "version": ORBBEC_SDK_VERSION,
            "architecture": "arm64",
            "binary_only": True,
            "note": "Vendor SDK binaries are included as supplied; the SDK is distributed in binary-only form.",
            "staging_metadata": sdk_metadata,
        },
        "upstream": {"m3t_commit": M3T_COMMIT},
    }


def write_json(path: Path, value: Any) -> None:
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    path.chmod(0o644)


def collect_archive_paths(root: Path) -> list[Path]:
    paths = [root]
    for _, path, _ in iter_entries(root):
        paths.append(path)
    return paths


def tar_info(path: Path, root: Path) -> tarfile.TarInfo:
    relative = path.relative_to(root.parent).as_posix()
    mode = path.lstat().st_mode
    info = tarfile.TarInfo(relative + ("/" if stat.S_ISDIR(mode) else ""))
    info.mode = stat.S_IMODE(mode)
    info.uid = 0
    info.gid = 0
    info.uname = ""
    info.gname = ""
    info.mtime = 0
    if stat.S_ISDIR(mode):
        info.type = tarfile.DIRTYPE
        info.size = 0
    elif stat.S_ISLNK(mode):
        info.type = tarfile.SYMTYPE
        info.linkname = os.readlink(path)
        info.size = 0
    elif stat.S_ISREG(mode):
        info.type = tarfile.REGTYPE
        info.size = path.stat().st_size
    else:
        raise PackageError(f"special file cannot be archived: {relative}")
    return info


def write_tar_gz(root: Path, destination: Path) -> None:
    paths = collect_archive_paths(root)
    with destination.open("wb") as raw:
        with gzip.GzipFile(filename="", mode="wb", fileobj=raw, compresslevel=9, mtime=0) as compressed:
            with tarfile.open(fileobj=compressed, mode="w", format=tarfile.PAX_FORMAT) as archive:
                for path in paths:
                    info = tar_info(path, root)
                    if stat.S_ISREG(path.lstat().st_mode):
                        with path.open("rb") as source:
                            archive.addfile(info, source)
                    else:
                        archive.addfile(info)


def write_zip(root: Path, destination: Path) -> None:
    paths = collect_archive_paths(root)
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9, allowZip64=True) as archive:
        for path in paths:
            mode = path.lstat().st_mode
            relative = path.relative_to(root.parent).as_posix()
            info = zipfile.ZipInfo(relative + ("/" if stat.S_ISDIR(mode) else ""), date_time=(1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = stat.S_IMODE(mode) << 16
            if stat.S_ISDIR(mode):
                info.external_attr |= stat.S_IFDIR << 16
                info.compress_type = zipfile.ZIP_STORED
                archive.writestr(info, b"")
            elif stat.S_ISLNK(mode):
                info.external_attr |= stat.S_IFLNK << 16
                info.compress_type = zipfile.ZIP_STORED
                archive.writestr(info, os.readlink(path).encode("utf-8"))
            elif stat.S_ISREG(mode):
                info.external_attr |= stat.S_IFREG << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                info.file_size = path.stat().st_size
                with path.open("rb") as source, archive.open(
                    info, mode="w", force_zip64=info.file_size >= zipfile.ZIP64_LIMIT,
                ) as target:
                    shutil.copyfileobj(source, target)
            else:
                raise PackageError(f"special file cannot be archived: {relative}")


def build_bundle(repo: Path, upstream: Path, sdk_prefix: Path, work_root: Path) -> tuple[Path, list[dict[str, str]]]:
    bundle = work_root / BUNDLE_NAME
    set_directory_mode(bundle, 0o755)

    for source, destination in (
        (repo / "drivers/orbbec", bundle / "drivers/orbbec"),
        (repo / "perception/m3t", bundle / "perception/m3t"),
    ):
        copy_filtered_tree(source, destination)

    for name in ("LICENSE-MIT", "LICENSE-APACHE"):
        source = repo / name
        if not source.is_file() or source.is_symlink():
            raise PackageError(f"required top-level license is missing: {source}")
        shutil.copy2(source, bundle / name)

    template_root = repo / "packaging/addons"
    for name in TEMPLATE_FILES:
        source = template_root / name
        if not source.is_file() or source.is_symlink():
            raise PackageError(f"required add-on packaging template is missing: {source}")
        shutil.copy2(source, bundle / name)

    copy_filtered_tree(upstream / "M3T/src", bundle / "third_party/m3t/M3T/src")
    copy_filtered_tree(upstream / "M3T/include", bundle / "third_party/m3t/M3T/include")
    copy_filtered_tree(upstream / "M3T/third_party", bundle / "third_party/m3t/M3T/third_party")
    shutil.copy2(upstream / "LICENSE", bundle / "third_party/m3t/LICENSE")
    bundled_sdk = bundle / "vendor/orbbec-sdk-2.10.6-arm64"
    copy_tree_exact(sdk_prefix, bundled_sdk)

    validate_bundle_symlinks(bundle)
    return bundle, validate_vendor_sdk(bundled_sdk)


def create_output(repo: Path, output: Path, upstream: Path, sdk_prefix: Path) -> None:
    requested_output = output.expanduser()
    if requested_output.exists() or requested_output.is_symlink():
        raise PackageError(f"refusing to overwrite existing or unsafe output directory: {output}")
    output = requested_output.resolve(strict=False)
    if output == Path(output.anchor) or output.exists() or output.is_symlink():
        raise PackageError(f"refusing to overwrite existing or unsafe output directory: {output}")
    output_parent = output.parent

    upstream = upstream.expanduser().resolve(strict=False)
    sdk_prefix = sdk_prefix.expanduser().resolve(strict=False)
    validate_m3t_checkout(upstream)
    validate_vendor_sdk(sdk_prefix)
    reject_output_overlap(output, [repo / "drivers/orbbec", repo / "perception/m3t", upstream, sdk_prefix])

    try:
        output_parent.mkdir(parents=True, exist_ok=True)
        temporary_root = Path(tempfile.mkdtemp(prefix=f".{output.name}.", dir=output_parent))
    except OSError as error:
        raise PackageError(f"cannot create temporary output in {output_parent}: {error}") from error
    try:
        bundle, sdk_metadata = build_bundle(repo, upstream, sdk_prefix, temporary_root)
        package_json = package_metadata(repo, sdk_metadata)
        write_json(bundle / "package.json", package_json)
        source_manifest = {
            "schema_version": 1,
            "bundle_root": BUNDLE_NAME,
            "files": file_manifest(bundle),
        }
        write_json(bundle / "SOURCE-MANIFEST.json", source_manifest)

        tar_name = f"{BUNDLE_NAME}.tar.gz"
        zip_name = f"{BUNDLE_NAME}.zip"
        write_tar_gz(bundle, temporary_root / tar_name)
        write_zip(bundle, temporary_root / zip_name)
        shutil.copy2(bundle / "package.json", temporary_root / "package.json")
        sum_names = sorted((tar_name, zip_name, "package.json"))
        sums = "".join(f"{sha256_file(temporary_root / name)}  {name}\n" for name in sum_names)
        (temporary_root / "SHA256SUMS").write_text(sums, encoding="ascii")
        (temporary_root / "SHA256SUMS").chmod(0o644)

        if output.exists() or output.is_symlink():
            raise PackageError(f"refusing to overwrite output directory created during packaging: {output}")
        temporary_root.rename(output)
    except Exception:
        shutil.rmtree(temporary_root, ignore_errors=True)
        raise
    log(f"created {output}")


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    repo = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=repo / "dist/rbt-addons-20261023",
        help="new output directory (default: dist/rbt-addons-20261023)",
    )
    parser.add_argument(
        "--m3t-upstream",
        type=Path,
        default=repo / "target/m3t-upstream",
        help="clean pinned 3DObjectTracking checkout (default: target/m3t-upstream)",
    )
    parser.add_argument(
        "--orbbec-sdk-prefix",
        type=Path,
        required=True,
        help="verified ARM64 OrbbecSDK 2.10.6 staging prefix",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    repo = Path(__file__).resolve().parents[1]
    try:
        output = args.output_dir
        if not output.is_absolute():
            output = Path.cwd() / output
        upstream = args.m3t_upstream
        if not upstream.is_absolute():
            upstream = Path.cwd() / upstream
        sdk_prefix = args.orbbec_sdk_prefix
        if not sdk_prefix.is_absolute():
            sdk_prefix = Path.cwd() / sdk_prefix
        create_output(repo, output, upstream, sdk_prefix)
    except (PackageError, OSError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

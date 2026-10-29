#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
repo_root="$(cd -- "${script_dir}/../../.." && pwd -P)"
upstream_dir="${RBT_ORBBEC_UPSTREAM_CLONE:-${repo_root}/target/orbbec-upstream/ros2}"
native_license="${repo_root}/drivers/orbbec/licenses/OrbbecSDK-LICENSE.txt"
repository='https://github.com/orbbec/OrbbecSDK_ROS2.git'
commit='c153462518ad674650bafa4464fda72c27ab797a'
native_license_sha256='010b2381c54551d1928784124282a7ff8cf6786f01846179bde3f810b7cd6b30'
case "$(uname -m)" in
    x86_64) arch='x86_64' ;;
    aarch64|arm64) arch='arm64' ;;
    *) arch='' ;;
esac
prefix=''
prefix_set=false
stage=''
temporary_clone=''

die() {
    printf 'ERROR: %s\n' "$*" >&2
    exit 1
}

usage() {
    printf '%s\n' \
        'Usage: drivers/orbbec/scripts/bootstrap_sdk.sh [--arch x86_64|aarch64|arm64] [--prefix PATH]' \
        'Fetch and verify the pinned OrbbecSDK 2.10.6 SDK package for Linux x86_64 or ARM64.'
}

while (($# > 0)); do
    case "$1" in
        --prefix)
            (($# >= 2)) || die '--prefix requires a path'
            prefix="$2"
            prefix_set=true
            shift 2
            ;;
        --arch)
            (($# >= 2)) || die '--arch requires x86_64, aarch64, or arm64'
            case "$2" in
                x86_64) arch='x86_64' ;;
                aarch64|arm64) arch='arm64' ;;
                *) die "unsupported architecture: $2 (expected x86_64, aarch64, or arm64)" ;;
            esac
            shift 2
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *) die "unknown argument: $1" ;;
    esac
done

for executable in git python3 sha256sum mktemp; do
    command -v "${executable}" >/dev/null 2>&1 || die "${executable} is required"
done
[[ "$(uname -s)" == Linux ]] || die 'the pinned OrbbecSDK package supports Linux only'
[[ -n "${arch}" ]] || die 'unsupported host architecture; use --arch x86_64 or --arch arm64 to select a supported SDK'
if [[ "${arch}" == x86_64 ]]; then
    manifest="${repo_root}/drivers/orbbec/sdk-manifest.json"
    manifest_sha256='729f0a39f5ca301549f438c7d3eb41de99e6f7bf5ed3ddfe7a45120854eb06f1'
    default_prefix='orbbec-sdk-2.10.6'
    sdk_dir='x64'
else
    manifest="${repo_root}/drivers/orbbec/sdk-manifest-arm64.json"
    manifest_sha256='53b5d2de4506a20c8d35098df1a87fd939b7f81a65f4149c88ccb0fbf968b29e'
    default_prefix='orbbec-sdk-2.10.6-arm64'
    sdk_dir='arm64'
fi
if [[ "${prefix_set}" == false ]]; then
    prefix="${repo_root}/target/${default_prefix}"
fi
[[ -f "${manifest}" ]] || die "SDK manifest is missing: ${manifest}"
[[ -f "${native_license}" ]] || die "native SDK license is missing: ${native_license}"
[[ "$(sha256sum "${manifest}" | awk '{print $1}')" == "${manifest_sha256}" ]] \
    || die 'SDK manifest checksum mismatch; refusing to continue'
[[ "$(sha256sum "${native_license}" | awk '{print $1}')" == "${native_license_sha256}" ]] \
    || die 'native SDK license checksum mismatch; refusing to continue'

prefix="$(python3 -c 'import os, sys; print(os.path.abspath(os.path.expanduser(sys.argv[1])))' "${prefix}")"
[[ "${prefix}" != / && "${prefix}" != "${repo_root}" ]] \
    || die "refusing unsafe installation prefix: ${prefix}"

cleanup() {
    if [[ -n "${stage}" && -d "${stage}" ]]; then
        rm -rf -- "${stage}"
    fi
    if [[ -n "${temporary_clone}" && -d "${temporary_clone}" ]]; then
        rm -rf -- "${temporary_clone}"
    fi
}
trap cleanup EXIT

if [[ -e "${upstream_dir}" || -L "${upstream_dir}" ]]; then
    [[ -d "${upstream_dir}" && ! -L "${upstream_dir}" ]] \
        || die "upstream checkout path is not a directory: ${upstream_dir}"
    [[ -e "${upstream_dir}/.git" ]] \
        || die "existing upstream path is not a Git checkout: ${upstream_dir}; preserved"
    [[ "$(git -C "${upstream_dir}" remote get-url origin 2>/dev/null || true)" == "${repository}" ]] \
        || die "existing upstream checkout has a different origin: ${upstream_dir}; preserved"
    [[ "$(git -C "${upstream_dir}" rev-parse HEAD 2>/dev/null || true)" == "${commit}" ]] \
        || die "existing upstream checkout is not at the pinned commit ${commit}; preserved"
    [[ -z "$(git -C "${upstream_dir}" status --porcelain=v1 --untracked-files=all)" ]] \
        || die "existing upstream checkout has local changes; preserved"
    if [[ "$(git -C "${upstream_dir}" config --bool core.sparseCheckout 2>/dev/null || true)" == true ]]; then
        mapfile -t sparse_patterns < <(git -C "${upstream_dir}" sparse-checkout list)
        if [[ "$(git -C "${upstream_dir}" config --bool core.sparseCheckoutCone 2>/dev/null || true)" == true ]]; then
            sparse_patterns+=(orbbec_camera/SDK/licenses "orbbec_camera/SDK/${sdk_dir}" orbbec_camera/scripts)
            git -C "${upstream_dir}" sparse-checkout set --cone "${sparse_patterns[@]}"
        else
            sparse_patterns+=(/LICENSE /NOTICE /orbbec_camera/SDK/licenses/ "/orbbec_camera/SDK/${sdk_dir}/" /orbbec_camera/scripts/99-obsensor-libusb.rules)
            git -C "${upstream_dir}" sparse-checkout set --no-cone "${sparse_patterns[@]}"
        fi
        [[ -z "$(git -C "${upstream_dir}" status --porcelain=v1 --untracked-files=all)" ]] \
            || die "could not extend the clean upstream sparse checkout for ${arch}; preserved"
    fi
else
    mkdir -p -- "$(dirname -- "${upstream_dir}")"
    temporary_clone="$(mktemp -d "${upstream_dir}.tmp.XXXXXXXX")"
    git -C "${temporary_clone}" init --quiet
    git -C "${temporary_clone}" remote add origin "${repository}"
    git -C "${temporary_clone}" sparse-checkout init --no-cone
    git -C "${temporary_clone}" sparse-checkout set --no-cone \
        /LICENSE \
        /NOTICE \
        /orbbec_camera/SDK/licenses/ \
        "/orbbec_camera/SDK/${sdk_dir}/" \
        /orbbec_camera/scripts/99-obsensor-libusb.rules
    git -C "${temporary_clone}" fetch --quiet --filter=blob:none --depth=1 origin "${commit}"
    git -C "${temporary_clone}" checkout --quiet --detach "${commit}"
    [[ "$(git -C "${temporary_clone}" rev-parse HEAD)" == "${commit}" ]] \
        || die 'fresh checkout did not resolve to the pinned commit'
    [[ -z "$(git -C "${temporary_clone}" status --porcelain=v1 --untracked-files=all)" ]] \
        || die 'fresh upstream checkout is unexpectedly dirty'
    mv -T -- "${temporary_clone}" "${upstream_dir}"
    temporary_clone=''
fi

if [[ -e "${prefix}" || -L "${prefix}" ]]; then
    [[ -d "${prefix}" && ! -L "${prefix}" ]] \
        || die "existing SDK prefix is not a directory: ${prefix}; preserved"
    mode='verify-prefix'
    target_prefix="${prefix}"
else
    mkdir -p -- "$(dirname -- "${prefix}")"
    stage="$(mktemp -d "$(dirname -- "${prefix}")/.orbbec-sdk-stage.XXXXXXXX")"
    mode='install-prefix'
    target_prefix="${stage}"
fi

python3 - "${upstream_dir}" "${manifest}" "${native_license}" "${manifest_sha256}" \
    "${native_license_sha256}" "${arch}" "${mode}" "${target_prefix}" <<'PY'
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys

COMMIT = "c153462518ad674650bafa4464fda72c27ab797a"
REPOSITORY = "https://github.com/orbbec/OrbbecSDK_ROS2.git"
SDK_VERSION = "2.10.6"
NATIVE_LICENSE_PATH = "licenses/OrbbecSDK-LICENSE.txt"
WRAPPER_LICENSE_PATH = "licenses/OrbbecSDK_ROS2-LICENSE.txt"
WRAPPER_NOTICE_PATH = "licenses/OrbbecSDK_ROS2-NOTICE.txt"
LICENSE_PREFIX = "orbbec_camera/SDK/licenses/"
UDEV_RULE = "orbbec_camera/scripts/99-obsensor-libusb.rules"
METADATA_PREFIX = "share/rbt-orbbec/"


def fail(message):
    raise RuntimeError(message)


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def sdk_source_prefix(arch):
    sdk_dir = "x64" if arch == "x86_64" else "arm64"
    return f"orbbec_camera/SDK/{sdk_dir}/"


def safe_relative(value):
    path = PurePosixPath(value)
    if path.is_absolute() or not path.parts or ".." in path.parts:
        fail(f"unsafe manifest path: {value}")
    return path


def mapped_path(source_path, arch):
    sdk_prefix = sdk_source_prefix(arch)
    if source_path.startswith(sdk_prefix):
        return source_path[len(sdk_prefix) :]
    if source_path.startswith(LICENSE_PREFIX):
        return "licenses/third-party/" + source_path[len(LICENSE_PREFIX) :]
    if source_path == "LICENSE":
        return WRAPPER_LICENSE_PATH
    if source_path == "NOTICE":
        return WRAPPER_NOTICE_PATH
    if source_path == UDEV_RULE:
        return "share/orbbec/udev/99-obsensor-libusb.rules"
    fail(f"manifest path is outside the SDK bootstrap allowlist: {source_path}")


def read_manifest(manifest_path, expected_sha, native_license, native_sha):
    raw = manifest_path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != expected_sha:
        fail("SDK manifest checksum mismatch")
    document = json.loads(raw)
    if (
        document.get("schema_version") != 1
        or document.get("repository") != REPOSITORY
        or document.get("commit") != COMMIT
        or document.get("sdk_version") != SDK_VERSION
    ):
        fail("SDK manifest provenance does not match the pinned SDK")
    if not isinstance(document.get("files"), dict) or not document["files"]:
        fail("SDK manifest has no file entries")
    if not native_license.is_file() or native_license.is_symlink():
        fail("native SDK license is missing or is not a regular file")
    if sha256(native_license) != native_sha:
        fail("native SDK license checksum mismatch")
    return raw, document


def validate_source(source_root, manifest_path, manifest_sha, native_license, native_sha, arch):
    raw_manifest, document = read_manifest(manifest_path, manifest_sha, native_license, native_sha)
    if not source_root.is_dir() or source_root.is_symlink():
        fail(f"upstream checkout is not a directory: {source_root}")
    remote = subprocess.run(
        ["git", "-C", str(source_root), "remote", "get-url", "origin"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    if remote != REPOSITORY:
        fail(f"upstream origin mismatch: {remote}")
    head = subprocess.run(
        ["git", "-C", str(source_root), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()
    if head != COMMIT:
        fail(f"upstream HEAD mismatch: {head}")
    selected_roots = [
        "LICENSE",
        "NOTICE",
        "orbbec_camera/SDK/licenses",
        sdk_source_prefix(arch).rstrip("/"),
        UDEV_RULE,
    ]
    tree = subprocess.run(
        ["git", "-C", str(source_root), "ls-tree", "-r", "--name-only", "-z", COMMIT, "--", *selected_roots],
        check=True,
        capture_output=True,
    ).stdout
    tree_paths = {value.decode("utf-8") for value in tree.split(b"\0") if value}
    manifest_paths = set(document["files"])
    if tree_paths != manifest_paths:
        missing = sorted(tree_paths - manifest_paths)
        extra = sorted(manifest_paths - tree_paths)
        fail(f"SDK manifest/tree entry mismatch (unmanifested={missing}, absent={extra})")

    entries = {}
    for source_name, metadata in document["files"].items():
        safe_relative(source_name)
        source_path = source_root.joinpath(*PurePosixPath(source_name).parts)
        if not isinstance(metadata, dict):
            fail(f"invalid manifest entry: {source_name}")
        kind = metadata.get("type")
        if kind == "file":
            if source_path.is_symlink() or not source_path.is_file():
                fail(f"upstream file is missing or not a regular file: {source_name}")
            actual_sha = sha256(source_path)
            if actual_sha != metadata.get("sha256"):
                fail(f"upstream file checksum mismatch: {source_name}")
            entry = {"type": "file", "sha256": actual_sha}
        elif kind == "symlink":
            if not source_path.is_symlink():
                fail(f"upstream symlink is missing: {source_name}")
            target = os.readlink(source_path)
            if target != metadata.get("target"):
                fail(f"upstream symlink target mismatch: {source_name}")
            resolved = (source_path.parent / target).resolve(strict=True)
            if not resolved.is_file():
                fail(f"upstream symlink does not point to a file: {source_name}")
            entry = {"type": "symlink", "target": target}
        else:
            fail(f"unsupported manifest entry type for {source_name}: {kind}")
        destination_name = mapped_path(source_name, arch)
        safe_relative(destination_name)
        if destination_name in entries:
            fail(f"multiple upstream entries map to {destination_name}")
        entries[destination_name] = entry

    native_rel = NATIVE_LICENSE_PATH
    entries[native_rel] = {"type": "file", "sha256": native_sha}
    entries[METADATA_PREFIX + "sdk-manifest.json"] = {
        "type": "file",
        "sha256": hashlib.sha256(raw_manifest).hexdigest(),
    }
    entries[METADATA_PREFIX + "sdk-manifest.sha256"] = {
        "type": "file",
        "sha256": hashlib.sha256((manifest_sha + "\n").encode("ascii")).hexdigest(),
    }
    return raw_manifest, entries


def verify_prefix(prefix, source_root, raw_manifest, expected_entries):
    if not prefix.is_dir() or prefix.is_symlink():
        fail(f"SDK prefix is not a directory: {prefix}")
    actual_files = set()
    for current, directories, files in os.walk(prefix, followlinks=False):
        current_path = Path(current)
        for name in list(directories):
            directory = current_path / name
            if directory.is_symlink():
                files.append(name)
                directories.remove(name)
        for name in files:
            actual_files.add((current_path / name).relative_to(prefix).as_posix())
    expected_files = set(expected_entries)
    if actual_files != expected_files:
        extra = sorted(actual_files - expected_files)
        missing = sorted(expected_files - actual_files)
        fail(f"SDK prefix file set mismatch (unexpected={extra}, missing={missing}); preserved")

    for relative, metadata in expected_entries.items():
        destination = prefix.joinpath(*PurePosixPath(relative).parts)
        if metadata["type"] == "file":
            if destination.is_symlink() or not destination.is_file():
                fail(f"SDK prefix file is missing or has the wrong type: {relative}; preserved")
            if sha256(destination) != metadata["sha256"]:
                fail(f"SDK prefix checksum mismatch: {relative}; preserved")
        else:
            if not destination.is_symlink() or os.readlink(destination) != metadata["target"]:
                fail(f"SDK prefix symlink mismatch: {relative}; preserved")
            if not (destination.parent / metadata["target"]).resolve(strict=True).is_file():
                fail(f"SDK prefix symlink target is missing: {relative}; preserved")
    manifest_target = prefix / (METADATA_PREFIX + "sdk-manifest.json")
    if manifest_target.read_bytes() != raw_manifest:
        fail("installed SDK manifest differs from the pinned manifest; preserved")


def install_prefix(prefix, source_root, manifest_path, native_license, entries, raw_manifest, manifest_sha, arch):
    if not prefix.is_dir() or prefix.is_symlink():
        fail(f"staging prefix is not a directory: {prefix}")
    if any(prefix.iterdir()):
        fail(f"staging prefix is not empty: {prefix}")
    for source_name in json.loads(raw_manifest)["files"]:
        source = source_root.joinpath(*PurePosixPath(source_name).parts)
        relative = mapped_path(source_name, arch)
        destination = prefix.joinpath(*PurePosixPath(relative).parts)
        destination.parent.mkdir(parents=True, exist_ok=True)
        metadata = entries[relative]
        if metadata["type"] == "file":
            shutil.copy2(source, destination)
        else:
            os.symlink(metadata["target"], destination)
    shutil.copy2(native_license, prefix / NATIVE_LICENSE_PATH)

    metadata_dir = prefix / METADATA_PREFIX
    metadata_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(manifest_path, metadata_dir / "sdk-manifest.json")
    (metadata_dir / "sdk-manifest.sha256").write_text(manifest_sha + "\n", encoding="ascii")
    verify_prefix(prefix, source_root, raw_manifest, entries)


def main():
    source_root, manifest_path, native_license = map(Path, sys.argv[1:4])
    manifest_sha, native_sha, arch, mode, *extra = sys.argv[4:]
    if arch not in {"x86_64", "arm64"}:
        fail(f"unsupported SDK architecture: {arch}")
    if mode not in {"verify-source", "verify-prefix", "install-prefix"}:
        fail("missing or invalid bootstrap operation")
    raw_manifest, entries = validate_source(
        source_root, manifest_path, manifest_sha, native_license, native_sha, arch
    )
    if mode == "verify-source":
        print(f"ORBBEC_SDK_SOURCE PASS arch={arch} commit={COMMIT} manifest_files={len(json.loads(raw_manifest)['files'])}")
    elif mode == "verify-prefix":
        if len(extra) != 1:
            fail("verify-prefix requires its prefix path")
        verify_prefix(Path(extra[0]), source_root, raw_manifest, entries)
    else:
        if len(extra) != 1:
            fail("install-prefix requires its staging path")
        install_prefix(Path(extra[0]), source_root, manifest_path, native_license, entries, raw_manifest, manifest_sha, arch)


try:
    main()
except Exception as error:
    print(f"ERROR: {error}", file=sys.stderr)
    sys.exit(1)
PY

if [[ "${mode}" == verify-prefix ]]; then
    printf 'ORBBEC_SDK_BOOTSTRAP PASS version=2.10.6 arch=%s prefix=%s (existing prefix verified)\n' "${arch}" "${prefix}"
    exit 0
fi

[[ ! -e "${prefix}" && ! -L "${prefix}" ]] \
    || die "SDK prefix appeared during installation; staged files preserved at ${stage}"
mv -T -- "${stage}" "${prefix}"
stage=''
printf 'ORBBEC_SDK_BOOTSTRAP PASS version=2.10.6 arch=%s commit=%s prefix=%s\n' "${arch}" "${commit}" "${prefix}"

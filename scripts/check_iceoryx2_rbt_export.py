#!/usr/bin/env python3
"""Verify the frozen, Git-free native source export shipped in releases."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import sys

# Pinned after verifying HEAD and the binary patch diff with the Git bootstrap.
MANIFEST_SHA256 = "8d46a0b31b20530c90440408034f84a0be36704c5fa4bc98ec0871d8cf67d7ff"


def verify(source: Path, manifest_path: Path) -> int:
    raw = manifest_path.read_bytes()
    if hashlib.sha256(raw).hexdigest() != MANIFEST_SHA256:
        raise ValueError("native export manifest differs from the audited manifest")
    manifest = json.loads(raw)
    if not source.is_dir() or source.is_symlink():
        raise ValueError("native source must be a real directory")
    for name, digest in manifest["provenance"].items():
        path = manifest_path.parent / name
        if path.is_symlink() or hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError(f"native provenance mismatch: {name}")
    expected = manifest["files"]
    actual = set()
    for directory, directories, files in os.walk(source, followlinks=False):
        # These are build output locations, never part of the exported input.
        directories[:] = [d for d in directories if d not in {".git", "target", "__pycache__"}]
        parent = Path(directory)
        links = [d for d in directories if (parent / d).is_symlink()]
        directories[:] = [d for d in directories if d not in links]
        for name in files + links:
            path = parent / name
            relative = path.relative_to(source).as_posix()
            actual.add(relative)
            entry = expected.get(relative)
            if entry is None:
                raise ValueError(f"unexpected native source file: {relative}")
            mode = path.lstat().st_mode
            if entry["type"] == "symlink":
                if not stat.S_ISLNK(mode) or os.readlink(path) != entry["target"]:
                    raise ValueError(f"native symlink mismatch: {relative}")
                target = PurePosixPath(entry["target"])
                if target.is_absolute() or not path.resolve().is_relative_to(source.resolve()):
                    raise ValueError(f"escaping native symlink: {relative}")
            elif not stat.S_ISREG(mode):
                raise ValueError(f"native source is not a regular file: {relative}")
            elif (hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]
                  or bool(mode & 0o111) != entry["executable"]):
                raise ValueError(f"native source content or executable mode mismatch: {relative}")
    missing = set(expected) - actual
    if missing:
        raise ValueError(f"missing native source file: {min(missing)}")
    return len(actual)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("manifest", type=Path)
    args = parser.parse_args()
    try:
        count = verify(args.source, args.manifest)
    except (ValueError, OSError, KeyError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(f"ICEORYX2_RBT_EXPORT PASS files={count} manifest={MANIFEST_SHA256}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

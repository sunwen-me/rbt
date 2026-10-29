#!/usr/bin/env python3
"""Verify the distributed sources and architecture-specific vendor SDK."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import sys


def main():
    root = Path(__file__).resolve().parent
    document = json.loads((root / "SOURCE-MANIFEST.json").read_text())
    if document.get("schema_version") != 1 or not document.get("files"):
        raise ValueError("invalid source manifest")
    checked = 0
    for name, metadata in document["files"].items():
        relative = PurePosixPath(name)
        if relative.is_absolute() or ".." in relative.parts or "\\" in name:
            raise ValueError("unsafe manifest path: " + name)
        path = root.joinpath(*relative.parts)
        if metadata["type"] == "symlink":
            if not path.is_symlink() or os.readlink(path) != metadata["target"]:
                raise ValueError("symlink mismatch: " + name)
            path.resolve(strict=True).relative_to(root)
        elif metadata["type"] == "file":
            if path.is_symlink() or not path.is_file():
                raise ValueError("missing regular file: " + name)
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(chunk)
            if digest.hexdigest() != metadata["sha256"]:
                raise ValueError("checksum mismatch: " + name)
            if name.startswith("vendor/"):
                with path.open("rb") as stream:
                    header = stream.read(20)
                if header.startswith(b"\x7fELF"):
                    if header[4] != 2 or header[5] != 1 or int.from_bytes(header[18:20], "little") != 183:
                        raise ValueError("vendor ELF is not little-endian ARM64: " + name)
        else:
            raise ValueError("unknown file type: " + name)
        checked += 1
    print("Verified {} source/vendor files; ARM64 build/runtime remains to be checked on the target.".format(checked))


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError) as error:
        print("Source verification failed: {}".format(error), file=sys.stderr)
        sys.exit(1)

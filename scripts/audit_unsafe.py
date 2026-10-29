#!/usr/bin/env python3
"""Audit the frozen production unsafe locations and approved TF markers."""

from __future__ import annotations

import argparse
from bisect import bisect_right
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any


BASELINE_MARKER_COUNTS = {
    "crates/rbt-bench/src/lib.rs": 2,
    "crates/rbt-python/src/lib.rs": 18,
    "crates/rbt-types/src/lib.rs": 11,
    "generated/rust/demo.rs": 4,
    "generated/rust/control.rs": 2,
    "generated/rust/sensor.rs": 12,
}
LEGACY_UNSAFE_COUNT = 49
TF_TYPE_NAMES = (
    "TfFrameId",
    "TfVec3",
    "TfQuaternion",
    "TfTransform3",
    "TfTimestamp",
    "StaticTransform",
    "StaticHeader",
    "StaticUpdateHeader",
    "DynamicTransform",
    "StaticSnapshotRequest",
    "StaticSnapshot",
    "StaticUpdate",
)

UNSAFE_WORD_RE = re.compile(r"\bunsafe\b")
RAW_STRING_START_RE = re.compile(r"(?:br|cr|r)(?P<hashes>#{0,})\"")
RAW_IDENTIFIER_RE = re.compile(r"r#(?P<identifier>[A-Za-z_][A-Za-z0-9_]*)")
TF_NAME_PATTERN = "|".join(re.escape(name) for name in TF_TYPE_NAMES)
TF_SHM_SAFE_RE = re.compile(
    rf"^unsafe\s+impl\s+crate::ShmSafe\s+for\s+({TF_NAME_PATTERN})\s*\{{\}}$"
)
TF_ZERO_COPY_RE = re.compile(
    rf'^unsafe\s+impl\s+iceoryx2::prelude::ZeroCopySend\s+for\s+'
    rf'({TF_NAME_PATTERN})\s*\{{\s*unsafe\s+fn\s+type_name\(\)\s*'
    rf"->\s*&'static\s+str\s*\{{\s*\"rbt\.tf/({TF_NAME_PATTERN})@1\"\s*"
    r"\}\s*\}$"
)


class AuditError(RuntimeError):
    """The observed unsafe locations differ from the reviewed boundary."""


@dataclass(frozen=True)
class UnsafeLocation:
    kind: str
    path: str
    line: int
    source: str

    def as_dict(self) -> dict[str, Any]:
        return {
            "kind": self.kind,
            "path": self.path,
            "line": self.line,
            "source": self.source,
        }


@dataclass(frozen=True)
class AuditReport:
    legacy_count: int
    tf_generated_count: int
    locations: tuple[UnsafeLocation, ...]

    @property
    def total_count(self) -> int:
        return self.legacy_count + self.tf_generated_count

    def as_dict(self) -> dict[str, Any]:
        return {
            "legacy": self.legacy_count,
            "tf_generated": self.tf_generated_count,
            "total": self.total_count,
            "locations": [location.as_dict() for location in self.locations],
        }


def _production_lines(relative_path: str, lines: list[str]) -> list[str]:
    if relative_path != "crates/rbt-types/src/lib.rs":
        return lines
    for index, line in enumerate(lines):
        if line.strip() == "#[cfg(test)]":
            return lines[:index]
    return lines


def _char_literal_end(source: str, quote: int) -> int | None:
    """Return the end of a Rust character literal, leaving lifetimes alone."""
    content = quote + 1
    if content >= len(source):
        return None

    if source[content] == "\\":
        escape = content + 1
        if escape >= len(source):
            return None
        if source[escape] in "nrt0\\'\"":
            content_end = escape + 1
        elif source[escape] == "x":
            digits = source[escape + 1 : escape + 3]
            if len(digits) != 2 or not all(char in "0123456789abcdefABCDEF" for char in digits):
                return None
            content_end = escape + 3
        elif source[escape] == "u" and escape + 1 < len(source) and source[escape + 1] == "{":
            close = source.find("}", escape + 2)
            if close < 0:
                return None
            digits = source[escape + 2 : close]
            compact_digits = digits.replace("_", "")
            if (
                not compact_digits
                or len(compact_digits) > 6
                or not all(char in "0123456789abcdefABCDEF" for char in compact_digits)
            ):
                return None
            content_end = close + 1
        else:
            return None
    else:
        if source[content] in "'\r\n":
            return None
        content_end = content + 1

    if content_end < len(source) and source[content_end] == "'":
        return content_end + 1
    return None


def _mask_comments_and_strings(source: str) -> str:
    """Keep Rust code while preserving offsets and newlines for diagnostics."""
    masked = list(source)
    length = len(source)

    def blank(start: int, end: int, *, token_boundary: bool = False) -> None:
        for position in range(start, end):
            if source[position] != "\n":
                masked[position] = "\0" if token_boundary else " "

    index = 0
    while index < length:
        if source.startswith("//", index):
            end = source.find("\n", index + 2)
            if end < 0:
                end = length
            blank(index, end)
            index = end
            continue

        if source.startswith("/*", index):
            end = index + 2
            depth = 1
            while end < length and depth:
                if source.startswith("/*", end):
                    depth += 1
                    end += 2
                elif source.startswith("*/", end):
                    depth -= 1
                    end += 2
                else:
                    end += 1
            blank(index, end)
            index = end
            continue

        is_token_start = index == 0 or not (
            source[index - 1].isalnum() or source[index - 1] == "_"
        )
        if is_token_start and source.startswith("b'", index):
            end = _char_literal_end(source, index + 1)
            if end is not None:
                blank(index, end, token_boundary=True)
                index = end
                continue

        if source[index] == "'":
            end = _char_literal_end(source, index)
            if end is not None:
                blank(index, end, token_boundary=True)
                index = end
                continue

        raw_start = None
        if is_token_start:
            raw_start = RAW_STRING_START_RE.match(source, index)
        if raw_start:
            hashes = raw_start.group("hashes")
            terminator = '"' + hashes
            close = source.find(terminator, raw_start.end())
            end = length if close < 0 else close + len(terminator)
            blank(index, end, token_boundary=True)
            index = end
            continue

        if is_token_start:
            raw_identifier = RAW_IDENTIFIER_RE.match(source, index)
            if raw_identifier:
                end = raw_identifier.end()
                blank(index, end, token_boundary=True)
                index = end
                continue

        if source[index] == '"':
            end = index + 1
            escaped = False
            while end < length:
                char = source[end]
                end += 1
                if escaped:
                    escaped = False
                elif char == "\\":
                    escaped = True
                elif char == '"':
                    break
            blank(index, end, token_boundary=True)
            index = end
            continue

        index += 1

    return "".join(masked)


def _line_starts(source: str) -> list[int]:
    return [0, *(index + 1 for index, char in enumerate(source) if char == "\n")]


def _lines_with_matches(lines: list[str], matches: list[int]) -> list[tuple[int, str]]:
    if not matches:
        return []
    source = "\n".join(lines)
    starts = _line_starts(source)
    by_line: dict[int, str] = {}
    for offset in matches:
        line_index = bisect_right(starts, offset) - 1
        if 0 <= line_index < len(lines):
            by_line[line_index] = lines[line_index]
    return [(line_index + 1, by_line[line_index]) for line_index in sorted(by_line)]


def _markers(path: Path, relative_path: str) -> list[tuple[int, str]]:
    lines = _production_lines(relative_path, path.read_text(encoding="utf-8").splitlines())
    return _unsafe_word_lines(lines)


def _unsafe_word_lines(lines: list[str]) -> list[tuple[int, str]]:
    code = _mask_comments_and_strings("\n".join(lines))
    matches = [match.start() for match in UNSAFE_WORD_RE.finditer(code)]
    return _lines_with_matches(lines, matches)


def _read_baseline(root: Path) -> dict[str, list[str]]:
    manifest_path = root / "tests/tf/unsafe-baseline.json"
    if not manifest_path.is_file():
        raise AuditError(f"pinned unsafe baseline manifest is missing: {manifest_path}")
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise AuditError(f"cannot read pinned unsafe baseline manifest: {error}") from error

    if manifest.get("schema_version") != 1 or not isinstance(manifest.get("files"), dict):
        raise AuditError("pinned unsafe baseline manifest has an unsupported shape")
    files = manifest["files"]
    if set(files) != set(BASELINE_MARKER_COUNTS):
        raise AuditError("pinned unsafe baseline file set changed")

    baseline: dict[str, list[str]] = {}
    snapshot_root = root / "target/tf-baseline/files"
    for relative_path, expected_count in BASELINE_MARKER_COUNTS.items():
        entry = files[relative_path]
        if not isinstance(entry, dict):
            raise AuditError(f"invalid baseline entry for {relative_path}")
        markers = entry.get("markers")
        if (
            entry.get("count") != expected_count
            or not isinstance(markers, list)
            or len(markers) != expected_count
            or not all(isinstance(marker, str) for marker in markers)
        ):
            raise AuditError(f"pinned baseline count changed for {relative_path}")

        snapshot = snapshot_root / relative_path
        # The committed manifest is the portable baseline. The pre-change
        # archive is additional review evidence in this checkout, not a
        # prerequisite that makes a clean checkout's verifier unusable.
        if snapshot.is_file():
            snapshot_markers = [line for _, line in _markers(snapshot, relative_path)]
            if snapshot_markers != markers:
                raise AuditError(f"unsafe baseline snapshot differs from pinned markers: {relative_path}")
        baseline[relative_path] = markers
    return baseline


def _source_files(root: Path) -> list[tuple[str, Path]]:
    files: list[tuple[str, Path]] = []
    for directory in ("crates", "examples", "generated/rust"):
        source_root = root / directory
        if not source_root.is_dir():
            continue
        for path in source_root.rglob("*.rs"):
            relative_path = path.relative_to(root).as_posix()
            if "tests" in path.relative_to(root).parts:
                continue
            if relative_path == "crates/rbt-idl/src/lib.rs":
                continue
            files.append((relative_path, path))
    return sorted(files)


def _validate_tf_markers(markers: list[tuple[int, str]]) -> None:
    expected = {(name, "shm") for name in TF_TYPE_NAMES}
    expected.update((name, "zero_copy") for name in TF_TYPE_NAMES)
    observed: list[tuple[str, str]] = []
    for line_number, source in markers:
        stripped = source.strip()
        shm_safe = TF_SHM_SAFE_RE.fullmatch(stripped)
        zero_copy = TF_ZERO_COPY_RE.fullmatch(stripped)
        if shm_safe:
            observed.append((shm_safe.group(1), "shm"))
        elif zero_copy and zero_copy.group(1) == zero_copy.group(2):
            observed.append((zero_copy.group(1), "zero_copy"))
        else:
            raise AuditError(
                "unapproved unsafe marker in generated/rust/tf.rs "
                f"at line {line_number}: {source}"
            )

    if len(observed) != len(expected) or set(observed) != expected:
        raise AuditError("TF generated unsafe markers are missing, duplicated, or unexpected")


def audit(root: Path) -> AuditReport:
    """Validate baseline markers and the exact approved generated TF marker set."""
    root = root.resolve()
    baseline = _read_baseline(root)

    for relative_path, expected_markers in baseline.items():
        source = root / relative_path
        if not source.is_file():
            raise AuditError(f"baseline production source is missing: {relative_path}")
        observed = [line for _, line in _markers(source, relative_path)]
        if observed != expected_markers:
            raise AuditError(f"legacy unsafe markers changed in {relative_path}")

    tf_path = "generated/rust/tf.rs"
    tf_source = root / tf_path
    if not tf_source.is_file():
        raise AuditError("generated/rust/tf.rs is missing")
    tf_markers = _markers(tf_source, tf_path)
    tf_unsafe_lines = _unsafe_word_lines(tf_source.read_text(encoding="utf-8").splitlines())
    if tf_unsafe_lines != tf_markers:
        raise AuditError("TF generated unsafe tokens must appear only in approved marker lines")
    _validate_tf_markers(tf_markers)

    observed_by_file: dict[str, list[tuple[int, str]]] = {}
    for relative_path, source in _source_files(root):
        markers = _markers(source, relative_path)
        if markers:
            observed_by_file[relative_path] = markers

    expected_paths = set(baseline) | {tf_path}
    if set(observed_by_file) != expected_paths:
        unexpected = sorted(set(observed_by_file) - expected_paths)
        missing = sorted(expected_paths - set(observed_by_file))
        details = []
        if unexpected:
            details.append(f"unexpected unsafe locations in {', '.join(unexpected)}")
        if missing:
            details.append(f"missing unsafe locations from {', '.join(missing)}")
        raise AuditError("; ".join(details))

    locations: list[UnsafeLocation] = []
    for relative_path in sorted(baseline):
        for line_number, source in observed_by_file[relative_path]:
            locations.append(UnsafeLocation("legacy", relative_path, line_number, source))
    for line_number, source in observed_by_file[tf_path]:
        locations.append(UnsafeLocation("tf-generated", tf_path, line_number, source))

    legacy_count = sum(len(markers) for markers in baseline.values())
    report = AuditReport(legacy_count, len(tf_markers), tuple(locations))
    if report.legacy_count != LEGACY_UNSAFE_COUNT or report.tf_generated_count != 24:
        raise AuditError(
            "unsafe source-location totals changed: "
            f"legacy={report.legacy_count} tf_generated={report.tf_generated_count}"
        )
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument("--label", default="RBT_UNSAFE_AUDIT")
    parser.add_argument("--json", action="store_true", help="print a JSON audit report")
    args = parser.parse_args()

    try:
        report = audit(args.root)
    except AuditError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(report.as_dict(), indent=2))
        return 0

    print(
        f"{args.label} legacy={report.legacy_count} "
        f"tf_generated={report.tf_generated_count} total={report.total_count}"
    )
    for location in report.locations:
        print(
            f"{location.kind} {location.path}:{location.line}:{location.source}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

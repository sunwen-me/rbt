#!/usr/bin/env python3
"""Validate an interop process's final machine-readable idle snapshot."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


IDLE_PREFIX = "RBT_RPC_INTEROP IDLE"
INTEGER = re.compile(r"[0-9]+\Z")
LANGUAGES = ("rust", "cpp", "python")


def expected_endpoint(role: str) -> str:
    if role.endswith("server"):
        return "server"
    if role.endswith("client"):
        return "client"
    raise ValueError(f"role must end in client or server: {role!r}")


def validate_idle_text(
    text: str, *, language: str, role: str, service: str, calls: int
) -> dict[str, str]:
    if language not in LANGUAGES:
        raise ValueError(f"unsupported language: {language!r}")
    if not service:
        raise ValueError("service must not be empty")
    if calls <= 0:
        raise ValueError("expected calls must be positive")
    endpoint = expected_endpoint(role)

    idle_lines = [
        line.strip()
        for line in text.splitlines()
        if re.match(r"^RBT_RPC_INTEROP IDLE(?:\s|$)", line)
    ]
    if len(idle_lines) != 1:
        raise ValueError(f"expected exactly one IDLE line, found {len(idle_lines)}")

    tokens = idle_lines[0].split()
    fields: dict[str, str] = {}
    for token in tokens[2:]:
        key, separator, value = token.partition("=")
        if not separator or not key or not value:
            raise ValueError(f"malformed IDLE field: {token!r}")
        if key in fields:
            raise ValueError(f"duplicate IDLE field: {key}")
        fields[key] = value

    identity = {
        "language": language,
        "role": role,
        "service": service,
        "endpoint": endpoint,
    }
    for key, expected in identity.items():
        actual = fields.get(key)
        if actual is None:
            raise ValueError(f"IDLE line is missing {key}")
        if actual != expected:
            raise ValueError(f"IDLE {key} mismatch: expected {expected!r}, got {actual!r}")

    timestamp = _integer_field(fields, "snapshot_timestamp_unix_ms")
    if timestamp <= 0:
        raise ValueError("snapshot_timestamp_unix_ms must be positive")
    actual_calls = _integer_field(fields, "calls")
    if actual_calls <= 0 or actual_calls != calls:
        raise ValueError(f"IDLE calls mismatch: expected {calls}, got {actual_calls}")

    if endpoint == "client":
        required = (
            "calls_started",
            "calls_completed",
            "in_flight_current",
            "response_guards_current",
        )
        forbidden = ("requests_received", "responses_sent", "requests_current")
        activity = ("calls_started", "calls_completed")
        gauges = ("in_flight_current", "response_guards_current")
    else:
        required = ("requests_received", "responses_sent", "requests_current")
        forbidden = (
            "calls_started",
            "calls_completed",
            "in_flight_current",
            "response_guards_current",
        )
        activity = ("requests_received", "responses_sent")
        gauges = ("requests_current",)

    for key in forbidden:
        if key in fields:
            raise ValueError(f"{endpoint} IDLE line must not include {key}")
    numeric = {key: _integer_field(fields, key) for key in required}
    for key in activity:
        if numeric[key] != calls:
            raise ValueError(f"IDLE {key} mismatch: expected {calls}, got {numeric[key]}")
    for key in gauges:
        if numeric[key] != 0:
            raise ValueError(f"IDLE {key} must be zero, got {numeric[key]}")
    return fields


def _integer_field(fields: dict[str, str], key: str) -> int:
    value = fields.get(key)
    if value is None:
        raise ValueError(f"IDLE line is missing {key}")
    if INTEGER.fullmatch(value) is None:
        raise ValueError(f"IDLE {key} must be a non-negative integer, got {value!r}")
    return int(value)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--language", required=True, choices=LANGUAGES)
    parser.add_argument("--role", required=True)
    parser.add_argument("--service", required=True)
    parser.add_argument("--calls", required=True, type=int)
    args = parser.parse_args()
    try:
        text = args.log.read_text(encoding="utf-8")
        validate_idle_text(
            text,
            language=args.language,
            role=args.role,
            service=args.service,
            calls=args.calls,
        )
    except (OSError, UnicodeError, ValueError) as error:
        print(f"RBT_RPC_INTEROP IDLE_INVALID log={args.log} error={error}", file=sys.stderr)
        return 1
    print(
        "RBT_RPC_INTEROP IDLE_VALIDATED "
        f"language={args.language} role={args.role} service={args.service} calls={args.calls}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

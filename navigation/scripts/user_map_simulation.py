#!/usr/bin/env python3
"""Run the native IPC navigation simulator against a user-provided static PGM map."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import sys

from complex_simulation import IntegrationError, ROOT, read_json, render_case, run_case, write_json


def yaml_image_path(map_yaml: Path) -> Path:
    """Resolve the top-level image scalar used by the navigation map YAML."""
    try:
        contents = map_yaml.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as error:
        raise IntegrationError(f"cannot read map YAML {map_yaml}: {error}") from error

    image_values: list[str] = []
    for line in contents.splitlines():
        if not line or line[0].isspace() or line.lstrip().startswith("#"):
            continue
        match = re.match(r"image\s*:\s*(.*?)\s*$", line)
        if not match:
            continue
        value = match.group(1).strip()
        quote: str | None = None
        escaped = False
        comment_at: int | None = None
        for index, char in enumerate(value):
            if quote == '"' and escaped:
                escaped = False
                continue
            if quote == '"' and char == "\\":
                escaped = True
                continue
            if quote and char == quote:
                quote = None
            elif not quote and char in ("'", '"'):
                quote = char
            elif not quote and char == "#" and (index == 0 or value[index - 1].isspace()):
                comment_at = index
                break
        if comment_at is not None:
            value = value[:comment_at].rstrip()
        if value.startswith('"'):
            try:
                parsed = json.loads(value)
            except json.JSONDecodeError as error:
                raise IntegrationError(f"map YAML has an invalid quoted image path: {error}") from error
        elif value.startswith("'") and value.endswith("'") and len(value) >= 2:
            parsed = value[1:-1].replace("''", "'")
        else:
            parsed = value
        if not isinstance(parsed, str) or not parsed:
            raise IntegrationError(f"map YAML {map_yaml} must have a non-empty image path")
        image_values.append(parsed)

    if len(image_values) != 1:
        raise IntegrationError(f"map YAML {map_yaml} must have exactly one top-level image field")
    image = Path(image_values[0])
    if image_values[0].startswith("~/"):
        image = Path.home() / image_values[0][2:]
    if not image.is_absolute():
        image = map_yaml.parent / image
    image = image.resolve()
    if image.suffix.lower() != ".pgm":
        raise IntegrationError(f"user map image must be a PGM file: {image}")
    if not image.is_file():
        raise IntegrationError(f"map image does not exist: {image}")
    return image


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def finite_cli_number(parser: argparse.ArgumentParser, value: float, label: str,
                      *, allow_zero: bool = False) -> float:
    if not math.isfinite(value) or (value < 0 if allow_zero else value <= 0):
        requirement = "finite and nonnegative" if allow_zero else "finite and positive"
        parser.error(f"{label} must be {requirement}")
    return value


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--map", type=Path, required=True, help="map YAML referencing a static PGM image")
    parser.add_argument("--start", type=float, nargs=3, required=True, metavar=("X", "Y", "YAW"))
    parser.add_argument("--goal", type=float, nargs=3, required=True, metavar=("X", "Y", "YAW"))
    parser.add_argument("--build-dir", type=Path, default=ROOT / "target/navigation-build",
                        help="CMake build directory containing the navigation executables")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "output/navigation-user-map",
                        help="directory for logs, captures, and rendered artifacts")
    parser.add_argument("--timeout", type=float, default=120.0,
                        help="goal timeout in seconds (default: 120)")
    parser.add_argument("--planning-clearance", type=float, default=0.30,
                        help="extra planning clearance in meters (default: 0.30)")
    parser.add_argument("--max-linear", type=float, default=0.2,
                        help="runtime maximum linear speed in m/s (default: 0.2)")
    parser.add_argument("--max-angular", type=float, default=0.5,
                        help="runtime maximum angular speed in rad/s (default: 0.5)")
    parser.add_argument("--fps", type=int, default=12, help="MP4 frame rate (default: 12)")
    parser.add_argument("--speed", type=float, default=2.0,
                        help="playback speed multiplier (default: 2)")
    parser.add_argument("--no-render", action="store_true",
                        help="validate captured IPC evidence without importing render packages")
    args = parser.parse_args(argv)
    for label, pose in (("start", args.start), ("goal", args.goal)):
        if not all(math.isfinite(value) for value in pose):
            parser.error(f"{label} pose must contain only finite values")
    finite_cli_number(parser, args.timeout, "timeout")
    finite_cli_number(parser, args.planning_clearance, "planning-clearance", allow_zero=True)
    finite_cli_number(parser, args.max_linear, "max-linear")
    finite_cli_number(parser, args.max_angular, "max-angular")
    finite_cli_number(parser, args.speed, "speed")
    if not 5 <= args.fps <= 60:
        parser.error("fps must be 5..60")
    return args


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    output_dir = args.output_dir.expanduser().resolve()
    summary: dict[str, object] = {"passed": False, "mode": "user_map_simulation"}
    try:
        output_dir.mkdir(parents=True, exist_ok=True)
        map_yaml = args.map.expanduser().resolve()
        if not map_yaml.is_file():
            raise IntegrationError(f"map YAML does not exist: {map_yaml}")
        map_image = yaml_image_path(map_yaml)
        provenance: dict[str, object] = {
            "map_source": "user-provided static PGM referenced by the map YAML",
            "map_yaml": str(map_yaml),
            "map_yaml_sha256": sha256(map_yaml),
            "map_image": str(map_image),
            "map_image_sha256": sha256(map_image),
            "motion_and_tf_source": "native IPC 2D kinematic simulation",
            "scope": "Static occupancy map with simulated motion and TF; no real LIO stream or physical hardware was used.",
        }
        scene: dict[str, object] = {
            "id": "user-map",
            "name": "User-provided static map simulation",
            "map": str(map_yaml),
            "start": list(args.start),
            "goal": list(args.goal),
            "goal_timeout_seconds": args.timeout,
            "planning_clearance": args.planning_clearance,
            "max_linear": args.max_linear,
            "max_angular": args.max_angular,
        }
        summary.update({"provenance": provenance, "fps": args.fps, "speed": args.speed,
                        "no_render": args.no_render})
        record = run_case(scene, args.build_dir.expanduser().resolve(), output_dir)
        case_dir = output_dir / str(record["case_dir"])
        result_path = case_dir / "result.json"
        record = read_json(result_path)
        record["provenance"] = provenance
        write_json(result_path, record)
        render_passed = False
        if record.get("passed") is True:
            if args.no_render:
                record["render_skipped"] = True
            else:
                try:
                    metadata = render_case(case_dir, scene, args.fps, args.speed)
                    record = read_json(result_path)
                    record["provenance"] = provenance
                    render_passed = True
                    summary["video_metadata"] = metadata
                except Exception as error:
                    record["render_passed"] = False
                    record["render_error"] = str(error)
                    summary["render_error"] = str(error)
                    print(f"[user-map] render failed: {error}", file=sys.stderr, flush=True)
        write_json(result_path, record)
        summary["case"] = {
            "scenario": "user-map",
            "case_dir": case_dir.name,
            "passed": bool(record.get("passed")),
            "render_passed": render_passed if not args.no_render else None,
            "process_exit_codes": record.get("process_exit_codes", {}),
            "error": record.get("error") or record.get("render_error"),
        }
        summary["passed"] = bool(record.get("passed")) and (args.no_render or render_passed)
    except Exception as error:
        summary["error"] = str(error)
        print(f"user map simulation failed: {error}", file=sys.stderr, flush=True)
    write_json(output_dir / "results.json", summary)
    print("User map simulation: " + ("PASS" if summary["passed"] else "FAIL"), flush=True)
    return 0 if summary["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

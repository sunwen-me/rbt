#!/usr/bin/env python3
"""Exercise runtime robot geometry selection without external hardware."""

from __future__ import annotations

import argparse
import math
import os
import re
import shutil
import subprocess
import tempfile
import uuid
from pathlib import Path


READY = "navigation runtime ready:"


def invoke(binary: Path, cwd: Path, *options: str) -> subprocess.CompletedProcess[str]:
    prefix = f"rbt/navigation/default-geometry-{uuid.uuid4()}"
    command = [
        str(binary), "--prefix", prefix,
        "--tf-map", str(uuid.uuid4()), "--tf-odom", str(uuid.uuid4()),
        "--duration", "0.1", *options,
    ]
    try:
        return subprocess.run(command, cwd=cwd, env=os.environ.copy(), text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              check=False, timeout=8)
    except subprocess.TimeoutExpired as error:
        raise AssertionError(f"runtime timed out: {command!r}\n{error.stdout!r}") from error


def assert_success(result: subprocess.CompletedProcess[str], geometry: str,
                   points: int, radius: float) -> None:
    if result.returncode != 0 or READY not in result.stdout:
        raise AssertionError(f"runtime did not start successfully ({result.returncode}):\n{result.stdout}")
    match = re.search(
        r"\bgeometry=(\S+) footprint_points=(\d+) robot_radius=([0-9.eE+-]+)", result.stdout
    )
    if match is None:
        raise AssertionError(f"ready line omitted geometry diagnostics:\n{result.stdout}")
    actual_geometry, actual_points, actual_radius = match.groups()
    if actual_geometry != geometry:
        raise AssertionError(f"expected geometry {geometry!r}, got {actual_geometry!r}")
    if int(actual_points) != points:
        raise AssertionError(f"expected {points} footprint points, got {actual_points}")
    if not math.isclose(float(actual_radius), radius, rel_tol=0.0, abs_tol=2e-6):
        raise AssertionError(f"expected radius {radius}, got {actual_radius}")


def assert_startup_failure(result: subprocess.CompletedProcess[str], reason: str) -> None:
    if result.returncode == 0 or READY in result.stdout or reason not in result.stdout:
        raise AssertionError(f"expected startup failure mentioning {reason!r}:\n{result.stdout}")


def write_geometry(path: Path, body: str) -> Path:
    path.write_text(
        "frame: base_footprint\ndrive_to_base: [0, 0, 0]\nfootprint:\n" + body,
        encoding="utf-8",
    )
    return path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--geometry", required=True, type=Path)
    parser.add_argument("--install-relative", required=True)
    args = parser.parse_args()
    binary = args.binary.resolve()
    packaged_geometry = args.geometry.resolve()

    with tempfile.TemporaryDirectory(prefix="navigation-default-geometry-") as temporary:
        root = Path(temporary)
        unrelated_cwd = root / "unrelated-cwd"
        unrelated_cwd.mkdir()

        build_geometry = (binary.parent / "config/front-drive-geometry.yaml").resolve()
        default = invoke(binary, unrelated_cwd)
        assert_success(default, str(build_geometry), 8, 0.402779)

        explicit_geometry = write_geometry(
            root / "explicit.yaml",
            "  - [-0.1, -0.1]\n  - [0.1, -0.1]\n  - [0.1, 0.1]\n  - [-0.1, 0.1]\n",
        )
        explicit = invoke(binary, unrelated_cwd, "--robot-geometry", str(explicit_geometry))
        assert_success(explicit, str(explicit_geometry.resolve()), 4, math.sqrt(0.02))

        override_prefix = root / "override-without-default"
        override_bin = override_prefix / "bin"
        override_binary = override_bin / binary.name
        override_bin.mkdir(parents=True)
        shutil.copy2(binary, override_binary)
        isolated_override = invoke(override_binary, unrelated_cwd,
                                   "--robot-geometry", str(explicit_geometry))
        assert_success(isolated_override, str(explicit_geometry.resolve()), 4, math.sqrt(0.02))

        config_directory = root / "config"
        config_directory.mkdir()
        relative_geometry = write_geometry(
            config_directory / "relative.yaml",
            "  - [-0.4, -0.1]\n  - [0.2, -0.1]\n  - [0.3, 0]\n"
            "  - [0.2, 0.1]\n  - [-0.4, 0.1]\n",
        )
        config = config_directory / "navigation.conf"
        config.write_text("robot-geometry=relative.yaml\n", encoding="utf-8")
        relative = invoke(binary, unrelated_cwd, "--config", str(config))
        assert_success(relative, str(relative_geometry.resolve()), 5, math.sqrt(0.17))

        precedence = invoke(binary, unrelated_cwd, "--config", str(config),
                            "--robot-geometry", str(explicit_geometry))
        assert_success(precedence, str(explicit_geometry.resolve()), 4, math.sqrt(0.02))

        circle = invoke(binary, unrelated_cwd, "--robot-geometry", "circle")
        assert_success(circle, "circle", 0, 0.18)
        circle_config = config_directory / "circle.conf"
        circle_config.write_text("robot-geometry=circle\n", encoding="utf-8")
        configured_circle = invoke(binary, unrelated_cwd, "--config", str(circle_config))
        assert_success(configured_circle, "circle", 0, 0.18)

        relocated_prefix = root / "relocated"
        relocated_bin = relocated_prefix / "bin"
        relocated_binary = relocated_bin / binary.name
        relocated_binary.parent.mkdir(parents=True)
        shutil.copy2(binary, relocated_binary)
        relocated_geometry = (relocated_bin / args.install_relative).resolve()
        try:
            relocated_geometry.relative_to(relocated_prefix.resolve())
        except ValueError as error:
            raise AssertionError("install-relative geometry escaped the install prefix") from error
        relocated_geometry.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(packaged_geometry, relocated_geometry)
        installed = invoke(relocated_binary, unrelated_cwd)
        assert_success(installed, str(relocated_geometry), 8, 0.402779)

        missing_prefix = root / "missing-default"
        missing_bin = missing_prefix / "bin"
        missing_binary = missing_bin / binary.name
        missing_bin.mkdir(parents=True)
        shutil.copy2(binary, missing_binary)
        missing = invoke(missing_binary, unrelated_cwd)
        assert_startup_failure(missing, "default robot geometry not found")

        invalid_geometry = root / "invalid.yaml"
        invalid_geometry.write_text("frame: wrong\n", encoding="utf-8")
        invalid = invoke(binary, unrelated_cwd, "--robot-geometry", str(invalid_geometry))
        assert_startup_failure(invalid, "invalid robot geometry")

        missing_override = invoke(binary, unrelated_cwd, "--robot-geometry",
                                  str(root / "missing-override.yaml"))
        assert_startup_failure(missing_override, "invalid robot geometry")

    print("PASS default, isolated override, config-relative, circle, relocated, and startup-failure geometry paths")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

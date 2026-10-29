#!/usr/bin/env python3
"""Reuse the existing replay renderer for VP path-yaw / tangent-yaw runs."""

from pathlib import Path
import importlib.util


def main() -> None:
    source = Path(__file__).with_name("render.py")
    spec = importlib.util.spec_from_file_location("robot_sim_ab_render", source)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load replay renderer: {source}")
    renderer = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(renderer)
    renderer.MODES = ("heading-on", "heading-off")
    renderer.MODE_TITLES = {
        "heading-on": "VP: heading from path (ON)",
        "heading-off": "VP: heading from XY tangent (OFF)",
    }
    renderer.main()


if __name__ == "__main__":
    main()

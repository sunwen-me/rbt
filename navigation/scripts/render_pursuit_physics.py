#!/usr/bin/env python3
"""Render saved MuJoCo pursuit runs and summarize the complete run matrix.

Video frames are reconstructed from each run's saved MuJoCo ``qpos`` rows.
The controller telemetry is used for outcome and error annotations only; it is
never used to draw a substitute kinematic robot.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import re
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable

# MuJoCo chooses its OpenGL backend during import. This renderer is intended
# for headless runs, so select EGL before importing MuJoCo.
os.environ.setdefault("MUJOCO_GL", "egl")

import mujoco
import numpy as np
from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[2]
CONTROLLERS = ("rpp", "rpp_dwpp", "vp", "vp_window")
CONTROLLER_LABELS = {
    "rpp": "RPP",
    "rpp_dwpp": "RPP + DWPP",
    "vp": "Vector pursuit",
    "vp_window": "Vector pursuit + window",
}
CONTROLLER_COLORS = {
    "rpp": (0.04, 0.56, 0.60, 1.0),
    "rpp_dwpp": (0.88, 0.43, 0.06, 1.0),
    "vp": (0.42, 0.31, 0.69, 1.0),
    "vp_window": (0.76, 0.23, 0.29, 1.0),
}
PREFERRED_CASES = (("S_curve", "delayed", 0), ("narrow_turn", "delayed", 0))
VIDEO_FPS = 20
MAX_VIDEO_FRAMES = 240
VIDEO_SIZE = (1280, 800)
PANEL_VIEW_SIZE = (608, 280)
PATH_RGBA = (0.96, 0.80, 0.22, 0.95)


@dataclass(frozen=True)
class RunKey:
    scenario: str
    profile: str
    seed: int
    controller: str

    @property
    def case(self) -> tuple[str, str, int]:
        return (self.scenario, self.profile, self.seed)


@dataclass
class Run:
    key: RunKey
    raw: dict[str, Any]
    result: dict[str, Any]
    artifacts: dict[str, Path]
    model: mujoco.MjModel
    data: mujoco.MjData
    renderer: mujoco.Renderer
    camera: mujoco.MjvCamera
    qpos_times: np.ndarray
    qpos: np.ndarray
    root_qpos: int
    path_xy: np.ndarray

    @property
    def end_time(self) -> float:
        return float(self.qpos_times[-1]) if len(self.qpos_times) else 0.0

    @property
    def outcome(self) -> str:
        return str(self.result.get("outcome", "unknown"))

    @property
    def successful(self) -> bool:
        return is_success(self.result)


@dataclass(frozen=True)
class UnavailableRun:
    result: dict[str, Any]
    end_time: float
    reason: str


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def artifact_label(path: Path) -> str:
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return str(path.resolve())


def finite_float(value: Any, name: str) -> float:
    if value is None or value == "":
        raise ValueError(f"missing numeric value for {name}")
    number = float(value)
    if not math.isfinite(number):
        raise ValueError(f"non-finite value for {name}")
    return number


def is_success(result: dict[str, Any] | None) -> bool:
    if result is None:
        return False
    value = str(result.get("outcome") or "").strip().lower().replace("-", "_")
    if "ground_truth_goal_within_existing_tolerances" in result or "truth_outcome" in result:
        truth_goal = result.get("ground_truth_goal_within_existing_tolerances")
        if isinstance(truth_goal, str):
            truth_goal = truth_goal.strip().lower() in {"1", "true", "yes"}
        if truth_goal is None:
            truth_goal = str(result.get("truth_outcome", "")).lower() == "truth_goal"
        return value == "reported_goal" and bool(truth_goal)
    return value in {
        "success", "succeeded", "pass", "passed", "complete", "completed", "goal_reached",
        "reached_goal", "goalreached", "reported_goal",
    }


def normalize_controller(value: Any) -> str:
    normalized = re.sub(r"[^a-z0-9]+", "_", str(value or "").strip().lower()).strip("_")
    aliases = {
        "pure_pursuit": "rpp",
        "rpp_dwpp": "rpp_dwpp",
        "rpp_dynamic_window": "rpp_dwpp",
        "rpp_plus_dwpp": "rpp_dwpp",
        "vector_pursuit": "vp",
        "vp": "vp",
        "vector_pursuit_window": "vp_window",
        "vp_window": "vp_window",
    }
    result = aliases.get(normalized, normalized)
    if result not in CONTROLLERS:
        raise ValueError(f"unsupported pursuit controller {value!r}")
    return result


def run_key(record: dict[str, Any]) -> RunKey:
    return RunKey(
        scenario=str(record["scenario"]),
        profile=str(record.get("profile", record.get("dynamics", "unknown"))),
        seed=int(record.get("seed", 0)),
        controller=normalize_controller(record.get("bridge_controller", record.get("controller"))),
    )


def run_directory(summary_path: Path, record: dict[str, Any]) -> Path:
    for field in ("run_directory", "run_dir", "directory"):
        value = record.get(field)
        if isinstance(value, str) and value:
            candidate = Path(value).expanduser()
            if not candidate.is_absolute():
                candidate = summary_path.parent / candidate
            if candidate.is_dir():
                return candidate.resolve()
    key = run_key(record)
    raw_controllers = tuple(dict.fromkeys(
        str(record.get(field)) for field in ("controller", "bridge_controller") if record.get(field)
    )) or (key.controller,)
    bases = (summary_path.parent, summary_path.parent / "runs", summary_path.parent.parent)
    for raw_controller in raw_controllers:
        parts = Path(key.scenario) / key.profile / f"seed-{key.seed}" / raw_controller
        for base in bases:
            candidate = base / parts
            if candidate.is_dir():
                return candidate.resolve()
    # Keep the canonical location in the error so incomplete pilot outputs are
    # easy to diagnose without silently searching unrelated evidence folders.
    return (summary_path.parent / parts).resolve()


def resolve_artifact(summary_path: Path, record: dict[str, Any], name: str, aliases: Iterable[str]) -> Path:
    artifacts = record.get("artifacts", {})
    value: Any = None
    if isinstance(artifacts, dict):
        value = next((artifacts[key] for key in aliases if artifacts.get(key)), None)
    if value is None:
        value = next((record[key] for key in aliases if record.get(key)), None)
    if not isinstance(value, str) or not value:
        raise ValueError(f"run {record.get('id', '<unknown>')} has no {name} artifact")
    candidate = Path(value).expanduser()
    if candidate.is_absolute():
        options = [candidate]
    else:
        options = [
            run_directory(summary_path, record) / candidate,
            summary_path.parent / candidate,
            summary_path.parent.parent / candidate,
            ROOT / candidate,
        ]
    for option in options:
        if option.is_file():
            return option.resolve()
    raise FileNotFoundError(f"could not resolve {name} artifact {value!r} for run {record.get('id')}")


def load_summary(path: Path) -> tuple[dict[str, Any], list[dict[str, Any]]]:
    with path.open(encoding="utf-8") as stream:
        summary = json.load(stream)
    records = summary.get("runs")
    if not isinstance(records, list):
        raise ValueError("summary.json must contain a runs array")
    checked: list[dict[str, Any]] = []
    for record in records:
        if not isinstance(record, dict):
            raise ValueError("every summary run must be an object")
        checked.append(record)
    return summary, checked


def read_csv(path: Path) -> tuple[list[str], list[dict[str, str]]]:
    with path.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if not reader.fieldnames:
            raise ValueError(f"{path} has no CSV header")
        return list(reader.fieldnames), list(reader)


def read_qpos(path: Path, model: mujoco.MjModel) -> tuple[np.ndarray, np.ndarray]:
    fields, rows = read_csv(path)
    if not rows:
        raise ValueError(f"{path} has no qpos rows")
    time_key = next((key for key in ("time_s", "sim_time_s") if key in fields), None)
    if time_key is None:
        raise ValueError(f"{path} must contain time_s or sim_time_s")
    qpos_fields = sorted(
        (field for field in fields if re.fullmatch(r"qpos_\d+", field)),
        key=lambda field: int(field.split("_")[1]),
    )
    if len(qpos_fields) != model.nq:
        raise ValueError(f"{path} has {len(qpos_fields)} qpos columns but model requires {model.nq}")
    times = np.asarray([finite_float(row[time_key], f"qpos {time_key}") for row in rows], dtype=np.float64)
    values = np.asarray(
        [[finite_float(row[field], field) for field in qpos_fields] for row in rows],
        dtype=np.float64,
    )
    if np.any(np.diff(times) < -1e-9):
        raise ValueError(f"{path} qpos timestamps are not monotonic")
    if not np.all(np.isfinite(values)):
        raise ValueError(f"{path} contains non-finite qpos values")
    return times, values


def read_path_xy(path: Path) -> np.ndarray:
    fields, rows = read_csv(path)
    if not rows:
        raise ValueError(f"{path} has no path rows")
    x_key = next((key for key in ("x", "path_x", "desired_x") if key in fields), None)
    y_key = next((key for key in ("y", "path_y", "desired_y") if key in fields), None)
    if x_key is None or y_key is None:
        raise ValueError(f"{path} must contain x and y path columns")
    return np.asarray(
        [[finite_float(row[x_key], x_key), finite_float(row[y_key], y_key)] for row in rows],
        dtype=np.float64,
    )


def root_qpos_address(model: mujoco.MjModel) -> int:
    joint_id = mujoco.mj_name2id(model, mujoco.mjtObj.mjOBJ_JOINT, "root_free")
    if joint_id < 0:
        for index in range(model.njnt):
            if model.jnt_type[index] == mujoco.mjtJoint.mjJNT_FREE:
                joint_id = index
                break
    if joint_id < 0:
        raise ValueError("model.xml has no free joint for the robot base")
    return int(model.jnt_qposadr[joint_id])


def load_render_model(path: Path) -> mujoco.MjModel:
    """Load the saved dynamics model with render-only lighting and colors."""
    model = mujoco.MjModel.from_xml_path(str(path))
    model.vis.headlight.ambient[:] = (0.45, 0.45, 0.45)
    model.vis.headlight.diffuse[:] = (0.75, 0.75, 0.75)
    model.vis.headlight.specular[:] = (0.20, 0.20, 0.20)
    for geom_id in range(model.ngeom):
        name = model.geom(geom_id).name
        if name == "ground":
            model.geom_rgba[geom_id] = (0.72, 0.76, 0.81, 1.0)
        elif name.startswith("obs_"):
            model.geom_rgba[geom_id] = (0.45, 0.51, 0.59, 1.0)
        elif name == "chassis":
            model.geom_rgba[geom_id] = (0.13, 0.39, 0.56, 1.0)
        elif name.startswith("drive_tire_") or name.startswith("caster_tire_"):
            model.geom_rgba[geom_id] = (0.12, 0.14, 0.17, 1.0)
        elif name.startswith("caster_bracket_"):
            model.geom_rgba[geom_id] = (0.64, 0.67, 0.70, 1.0)
    return model


def quaternion_slerp(left: np.ndarray, right: np.ndarray, ratio: float) -> np.ndarray:
    a = left / max(np.linalg.norm(left), 1e-12)
    b = right / max(np.linalg.norm(right), 1e-12)
    dot = float(np.dot(a, b))
    if dot < 0.0:
        b, dot = -b, -dot
    if dot > 0.9995:
        result = a + ratio * (b - a)
        return result / max(np.linalg.norm(result), 1e-12)
    angle = math.acos(min(1.0, max(-1.0, dot)))
    scale = math.sin(angle)
    return (math.sin((1.0 - ratio) * angle) / scale) * a + (math.sin(ratio * angle) / scale) * b


def qpos_at(run: Run, time_s: float) -> np.ndarray:
    if time_s <= run.qpos_times[0]:
        return run.qpos[0].copy()
    if time_s >= run.qpos_times[-1]:
        return run.qpos[-1].copy()
    after_index = int(np.searchsorted(run.qpos_times, time_s, side="right"))
    before_index = after_index - 1
    before_time = float(run.qpos_times[before_index])
    after_time = float(run.qpos_times[after_index])
    if after_time <= before_time:
        return run.qpos[after_index].copy()
    ratio = (time_s - before_time) / (after_time - before_time)
    before, after = run.qpos[before_index], run.qpos[after_index]
    state = before + ratio * (after - before)
    state[run.root_qpos + 3 : run.root_qpos + 7] = quaternion_slerp(
        before[run.root_qpos + 3 : run.root_qpos + 7],
        after[run.root_qpos + 3 : run.root_qpos + 7],
        ratio,
    )
    return state


def selected_case_keys(
    records: list[dict[str, Any]],
    scenario: str | None,
    profile: str | None,
    seed: int | None,
) -> list[tuple[str, str, int]]:
    cases = sorted({run_key(record).case for record in records})
    filters_active = scenario is not None or profile is not None or seed is not None
    if filters_active:
        return [
            case for case in cases
            if (scenario is None or case[0] == scenario)
            and (profile is None or case[1] == profile)
            and (seed is None or case[2] == seed)
        ]
    preferred = [case for case in PREFERRED_CASES if case in cases]
    if not preferred:
        raise ValueError(
            "the summary has no preselected physics cases S_curve/delayed/seed0 or "
            "narrow_turn/delayed/seed0; pass --scenario/--profile/--seed to select recorded cases"
        )
    return preferred


def aggregate_metrics(records: list[dict[str, Any]]) -> dict[str, Any]:
    keyed: dict[tuple[str, str, int, str], dict[str, Any]] = {}
    cases = sorted({run_key(record).case for record in records})
    for record in records:
        key = run_key(record)
        full_key = (*key.case, key.controller)
        if full_key in keyed:
            raise ValueError(f"duplicate summary run for {full_key}")
        keyed[full_key] = record

    controllers: dict[str, Any] = {}
    for controller in CONTROLLERS:
        matching = [keyed.get((*case, controller)) for case in cases]
        counts: dict[str, int] = {}
        for record in matching:
            label = "missing" if record is None else str(record.get("outcome", "unknown"))
            counts[label] = counts.get(label, 0) + 1
        successes = sum(is_success(record) for record in matching)
        controllers[controller] = {
            "matched_cases_denominator": len(cases),
            "recorded_runs": sum(record is not None for record in matching),
            "successes": successes,
            "failures_or_non_successes": sum(record is not None and not is_success(record) for record in matching),
            "missing_runs": sum(record is None for record in matching),
            "outcomes": counts,
        }

    common_success = [
        case for case in cases
        if all(is_success(keyed.get((*case, controller))) for controller in CONTROLLERS)
    ]
    all_metrics: dict[str, Any] = {}
    common_metrics: dict[str, Any] = {}
    for controller in CONTROLLERS:
        runs = [keyed.get((*case, controller)) for case in cases]
        common_runs = [keyed.get((*case, controller)) for case in common_success]
        all_pos = [_metric(record, "position_rms_m") for record in runs]
        all_yaw = [_metric(record, "body_yaw_rms_rad") for record in runs]
        cohort_pos = [_metric(record, "position_rms_m") for record in common_runs]
        cohort_yaw = [_metric(record, "body_yaw_rms_rad") for record in common_runs]
        all_pos = [value for value in all_pos if value is not None]
        all_yaw = [value for value in all_yaw if value is not None]
        cohort_pos = [value for value in cohort_pos if value is not None]
        cohort_yaw = [value for value in cohort_yaw if value is not None]
        all_metrics[controller] = {
            "position_rms_mean_m": _mean(all_pos),
            "position_rms_denominator": len(all_pos),
            "body_yaw_rms_mean_rad": _mean(all_yaw),
            "body_yaw_rms_denominator": len(all_yaw),
            "includes_non_success_outcomes": True,
        }
        common_metrics[controller] = {
            "position_rms_mean_m": _mean(cohort_pos),
            "position_rms_denominator": len(cohort_pos),
            "body_yaw_rms_mean_rad": _mean(cohort_yaw),
            "body_yaw_rms_denominator": len(cohort_yaw),
            "common_success_cases": len(common_success),
            "eligible_matched_cases": len(cases),
        }
    return {
        "schema_version": 1,
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "denominators": {
            "recorded_runs": len(records),
            "matched_cases": len(cases),
            "expected_controller_run_cells": len(cases) * len(CONTROLLERS),
            "all_controller_common_success_cases": len(common_success),
        },
        "outcomes_by_controller": controllers,
        "all_runs_moving_rms_including_non_successes": all_metrics,
        "four_controller_common_success_moving_rms": common_metrics,
        "common_success_cases": [
            {"scenario": case[0], "profile": case[1], "seed": case[2]}
            for case in common_success
        ],
    }


def _metric(record: dict[str, Any] | None, name: str) -> float | None:
    if record is None:
        return None
    value = record.get(name)
    if value in (None, ""):
        return None
    try:
        number = float(value)
    except (TypeError, ValueError):
        return None
    return number if math.isfinite(number) else None


def _mean(values: list[float]) -> float | None:
    return float(sum(values) / len(values)) if values else None


def load_font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    paths = (
        ("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")
        if bold else ("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",)
    )
    for path in paths:
        if Path(path).is_file():
            return ImageFont.truetype(path, size=size)
    return ImageFont.load_default()


def render_aggregate_png(path: Path, aggregate: dict[str, Any]) -> None:
    width, height = 1600, 690
    image = Image.new("RGB", (width, height), "#f2f4f7")
    draw = ImageDraw.Draw(image)
    title_font, subtitle_font = load_font(26, True), load_font(15)
    heading_font, body_font = load_font(15, True), load_font(14)
    draw.text((40, 26), "MuJoCo pursuit benchmark · full recorded run set", font=title_font, fill="#182538")
    den = aggregate["denominators"]
    draw.text(
        (40, 68),
        f"{den['recorded_runs']} recorded runs · {den['matched_cases']} matched cases · "
        "RMS is an equal-weight mean of per-run moving metrics",
        font=subtitle_font,
        fill="#566273",
    )
    left, top, table_width = 40, 112, width - 80
    widths = (255, 420, 420, 420)
    headers = ("Controller", "Outcome counts", "All-run moving RMS\n(non-successes included)", "All-four success cohort\n(moving RMS)")
    header_height = 62
    row_height = 118
    x = left
    for column, title in enumerate(headers):
        draw.rectangle((x, top, x + widths[column], top + header_height), fill="#dce3ec", outline="#bdc7d2")
        lines = title.split("\n")
        y = top + 10 if len(lines) == 2 else top + 21
        for line in lines:
            draw.text((x + 12, y), line, font=heading_font, fill="#20324a")
            y += 23
        x += widths[column]

    for index, controller in enumerate(CONTROLLERS):
        row_y = top + header_height + index * row_height
        background = "#ffffff" if index % 2 == 0 else "#f9fafb"
        data = aggregate["outcomes_by_controller"][controller]
        counts = data["outcomes"]
        outcome_text = [f"{label}: {count}" for label, count in sorted(counts.items())]
        success = data["successes"]
        outcome_text.insert(0, f"Success {success}/{data['matched_cases_denominator']} · failures "
                            f"{data['failures_or_non_successes']} · missing {data['missing_runs']}")
        raw = aggregate["all_runs_moving_rms_including_non_successes"][controller]
        cohort = aggregate["four_controller_common_success_moving_rms"][controller]
        raw_text = [
            f"Position {metric_text(raw['position_rms_mean_m'])} m  (n={raw['position_rms_denominator']})",
            f"Body yaw {metric_text(raw['body_yaw_rms_mean_rad'], 4)} rad  (n={raw['body_yaw_rms_denominator']})",
        ]
        cohort_text = [
            f"Cases {cohort['common_success_cases']}/{cohort['eligible_matched_cases']} all-four successes",
            f"Position {metric_text(cohort['position_rms_mean_m'])} m  (n={cohort['position_rms_denominator']})",
            f"Body yaw {metric_text(cohort['body_yaw_rms_mean_rad'], 4)} rad  (n={cohort['body_yaw_rms_denominator']})",
        ]
        values = (CONTROLLER_LABELS[controller], "\n".join(outcome_text), "\n".join(raw_text), "\n".join(cohort_text))
        x = left
        for column, text_value in enumerate(values):
            draw.rectangle((x, row_y, x + widths[column], row_y + row_height), fill=background, outline="#c8d0da")
            font = heading_font if column == 0 else body_font
            y = row_y + 15
            for line in text_value.split("\n"):
                draw.text((x + 12, y), line, font=font, fill="#26374d")
                y += 23
            x += widths[column]
    footer_y = top + header_height + len(CONTROLLERS) * row_height + 17
    if den["all_controller_common_success_cases"] == 0:
        footer = "No complete four-controller success cohort is available; cohort RMS is not ranked."
    else:
        footer = (
            f"Common-success metrics use {den['all_controller_common_success_cases']} of "
            f"{den['matched_cases']} matched cases. No ranking is inferred from an incomplete cohort."
        )
    draw.text((40, footer_y), footer, font=subtitle_font, fill="#566273")
    image.save(path, format="PNG", optimize=True)


def metric_text(value: Any, precision: int = 3) -> str:
    return "—" if value is None else f"{float(value):.{precision}f}"


def camera_for_paths(model: mujoco.MjModel, path_xy: np.ndarray, qpos: np.ndarray, root_qpos: int) -> mujoco.MjvCamera:
    positions = qpos[:, root_qpos : root_qpos + 2]
    points = np.vstack((path_xy, positions)) if len(path_xy) else positions
    if not len(points):
        points = np.zeros((1, 2), dtype=np.float64)
    min_xy = np.nanmin(points, axis=0)
    max_xy = np.nanmax(points, axis=0)
    span = np.maximum(max_xy - min_xy, np.array((1.0, 0.8)))
    center = (min_xy + max_xy) / 2.0
    camera = mujoco.MjvCamera()
    mujoco.mjv_defaultCamera(camera)
    camera.lookat[:] = (float(center[0]), float(center[1]), 0.27)
    camera.distance = float(max(2.2, max(span[0], span[1]) * 1.45 + 0.45))
    camera.azimuth = 315.0
    camera.elevation = -27.0
    return camera


def make_run(summary_path: Path, record: dict[str, Any], camera: mujoco.MjvCamera) -> Run:
    names = {
        "model_xml": ("model_xml", "model"),
        "physics_qpos_csv": ("physics_qpos_csv", "qpos_csv", "physics_qpos"),
        "path_csv": ("path_csv", "path"),
        "map_csv": ("map_csv", "map"),
        "telemetry_csv": ("telemetry_csv", "telemetry"),
        "result_json": ("result_json", "result"),
    }
    artifacts = {
        name: resolve_artifact(summary_path, record, name, aliases)
        for name, aliases in names.items()
        if name != "result_json" or (record.get("artifacts", {}).get("result_json") if isinstance(record.get("artifacts"), dict) else None)
    }
    model = load_render_model(artifacts["model_xml"])
    qpos_times, qpos = read_qpos(artifacts["physics_qpos_csv"], model)
    path_xy = read_path_xy(artifacts["path_csv"])
    raw_result = record
    if "result_json" in artifacts:
        with artifacts["result_json"].open(encoding="utf-8") as stream:
            loaded_result = json.load(stream)
        if isinstance(loaded_result, dict):
            raw_result = loaded_result
    result = dict(record)
    result.update(raw_result)
    controller_id = normalize_controller(record.get("bridge_controller", record.get("controller")))
    renderer = mujoco.Renderer(model, height=PANEL_VIEW_SIZE[1], width=PANEL_VIEW_SIZE[0])
    data = mujoco.MjData(model)
    root_qpos = root_qpos_address(model)
    selected_camera = mujoco.MjvCamera()
    mujoco.mjv_defaultCamera(selected_camera)
    selected_camera.lookat[:] = camera.lookat
    selected_camera.distance = camera.distance
    selected_camera.azimuth = camera.azimuth
    selected_camera.elevation = camera.elevation
    return Run(
        key=RunKey(run_key(record).scenario, run_key(record).profile, run_key(record).seed, controller_id),
        raw=record,
        result=result,
        artifacts=artifacts,
        model=model,
        data=data,
        renderer=renderer,
        camera=selected_camera,
        qpos_times=qpos_times,
        qpos=qpos,
        root_qpos=root_qpos,
        path_xy=path_xy,
    )


def add_line(scene: mujoco.MjvScene, start: np.ndarray, end: np.ndarray, rgba: tuple[float, float, float, float], width: float) -> None:
    index = int(scene.ngeom)
    if index >= int(scene.maxgeom):
        return
    geom = scene.geoms[index]
    mujoco.mjv_initGeom(
        geom,
        mujoco.mjtGeom.mjGEOM_LINE,
        np.asarray((0.01, 0.0, 0.0), dtype=np.float64),
        np.zeros(3, dtype=np.float64),
        np.eye(3, dtype=np.float64).reshape(9),
        np.asarray(rgba, dtype=np.float32),
    )
    mujoco.mjv_connector(geom, mujoco.mjtGeom.mjGEOM_LINE, width, start, end)
    scene.ngeom = index + 1


def add_trace_geoms(run: Run, time_s: float, sampled_qpos: np.ndarray) -> None:
    scene = run.renderer.scene
    if len(run.path_xy) > 1:
        stride = max(1, math.ceil((len(run.path_xy) - 1) / 150))
        for index in range(0, len(run.path_xy) - 1, stride):
            following = min(len(run.path_xy) - 1, index + stride)
            start = np.array((run.path_xy[index, 0], run.path_xy[index, 1], 0.014), dtype=np.float64)
            end = np.array((run.path_xy[following, 0], run.path_xy[following, 1], 0.014), dtype=np.float64)
            add_line(scene, start, end, PATH_RGBA, 2.0)
    before = int(np.searchsorted(run.qpos_times, min(time_s, run.end_time), side="right"))
    history = run.qpos[:max(1, before), run.root_qpos : run.root_qpos + 2]
    if len(history) == 1:
        history = np.vstack((history, sampled_qpos[run.root_qpos : run.root_qpos + 2]))
    elif time_s < run.end_time:
        history = np.vstack((history, sampled_qpos[run.root_qpos : run.root_qpos + 2]))
    stride = max(1, math.ceil((len(history) - 1) / 150))
    color = CONTROLLER_COLORS[run.key.controller]
    for index in range(0, len(history) - 1, stride):
        following = min(len(history) - 1, index + stride)
        start = np.array((history[index, 0], history[index, 1], 0.022), dtype=np.float64)
        end = np.array((history[following, 0], history[following, 1], 0.022), dtype=np.float64)
        add_line(scene, start, end, color, 3.0)


def render_run_view(run: Run, time_s: float) -> np.ndarray:
    run.data.qpos[:] = qpos_at(run, time_s)
    run.data.time = min(time_s, run.end_time)
    mujoco.mj_forward(run.model, run.data)
    run.renderer.update_scene(run.data, camera=run.camera)
    add_trace_geoms(run, time_s, run.data.qpos)
    pixels = run.renderer.render()
    return np.asarray(pixels, dtype=np.uint8)


def outcome_label(result: dict[str, Any]) -> str:
    outcome = str(result.get("outcome", "unknown")).strip().lower()
    if outcome == "reported_goal":
        value = "TRUTH GOAL" if is_success(result) else "TRUTH MISS"
    else:
        value = outcome.replace("_", " ")
    error = result.get("error_code")
    if error in (None, "", 0, "0") or str(error).strip().lower() == outcome:
        return value.upper()
    return f"{value.upper()} · #{error}"


def outcome_color(result: dict[str, Any]) -> tuple[int, int, int]:
    if is_success(result):
        return (37, 132, 91)
    label = str(result.get("outcome", "")).lower()
    if "timeout" in label or "time_limit" in label:
        return (190, 112, 30)
    return (184, 54, 63)


def _draw_badge(draw: ImageDraw.ImageDraw, x: int, y: int, label: str, color: tuple[int, int, int], font: ImageFont.ImageFont, max_width: int) -> None:
    text_width = int(draw.textbbox((0, 0), label, font=font)[2])
    badge_width = min(max_width, text_width + 18)
    draw.rounded_rectangle((x, y, x + badge_width, y + 24), radius=7, fill=color)
    draw.text((x + 9, y + 3), label, font=font, fill="white")


def compose_frame(
    case: tuple[str, str, int],
    runs: dict[str, Run],
    time_s: float,
    duration: float,
    unavailable: dict[str, UnavailableRun] | None = None,
) -> np.ndarray:
    image = Image.new("RGB", VIDEO_SIZE, "#edf0f4")
    draw = ImageDraw.Draw(image)
    title_font = load_font(24, True)
    global_font = load_font(15)
    controller_font = load_font(16, True)
    badge_font = load_font(12, True)
    detail_font = load_font(12)
    scenario, profile, seed = case
    draw.text((24, 19), f"MuJoCo physics replay · {scenario} · {profile} · seed {seed}", font=title_font, fill="#182538")
    draw.text((1018, 29), f"Virtual time  {time_s:5.2f} s", font=global_font, fill="#3d4a5b")
    panel_positions = ((24, 72), (648, 72), (24, 419), (648, 419))
    for controller, (x, y) in zip(CONTROLLERS, panel_positions):
        run = runs.get(controller)
        unavailable_run = (unavailable or {}).get(controller)
        draw.rounded_rectangle((x, y, x + 608, y + 333), radius=9, fill="white", outline="#c8d0da", width=1)
        if run is None and unavailable_run is None:
            draw.text((x + 14, y + 7), CONTROLLER_LABELS[controller], font=controller_font, fill="#26374d")
            _draw_badge(draw, x + 443, y + 6, "NO RECORDED RUN", (125, 133, 145), badge_font, 151)
            view = Image.new("RGB", PANEL_VIEW_SIZE, "#d8dde4")
            view_draw = ImageDraw.Draw(view)
            text = "NO PHYSICS REPLAY"
            box = view_draw.textbbox((0, 0), text, font=controller_font)
            view_draw.text(((PANEL_VIEW_SIZE[0] - (box[2] - box[0])) / 2,
                            (PANEL_VIEW_SIZE[1] - (box[3] - box[1])) / 2), text, font=controller_font, fill="#6d7785")
            image.paste(view, (x, y + 31))
            draw.text((x + 8, y + 314), "This controller has no run record in the selected case.", font=detail_font, fill="#697586")
            continue
        if unavailable_run is not None:
            result = unavailable_run.result
            draw.text((x + 14, y + 7), CONTROLLER_LABELS[controller], font=controller_font, fill="#26374d")
            _draw_badge(draw, x + 608 - 14 - 164, y + 6, outcome_label(result), outcome_color(result), badge_font, 164)
            view = Image.new("RGB", PANEL_VIEW_SIZE, "#e3e7ec")
            view_draw = ImageDraw.Draw(view)
            text = "PHYSICS REPLAY UNAVAILABLE"
            box = view_draw.textbbox((0, 0), text, font=controller_font)
            view_draw.text(((PANEL_VIEW_SIZE[0] - (box[2] - box[0])) / 2,
                            (PANEL_VIEW_SIZE[1] - (box[3] - box[1])) / 2 - 10), text,
                           font=controller_font, fill="#5e6876")
            image.paste(view, (x, y + 31))
            final_position = _metric(result, "final_position_error_m")
            final_yaw = _metric(result, "final_yaw_error_rad")
            frozen = "FROZEN ON FAILURE" if not is_success(result) else "NO SAVED STATE"
            detail = (
                f"{frozen} · end {unavailable_run.end_time:.2f}s · final epos "
                f"{metric_text(final_position)}m · eθ {metric_text(final_yaw, 3)}rad · "
                f"{unavailable_run.reason}"
            )
            draw.text((x + 8, y + 314), detail[:92], font=detail_font, fill="#8f3c43")
            continue
        label = outcome_label(run.result)
        _draw_badge(draw, x + 608 - 14 - 164, y + 6, label, outcome_color(run.result), badge_font, 164)
        draw.text((x + 14, y + 7), CONTROLLER_LABELS[controller], font=controller_font, fill="#26374d")
        if time_s >= run.end_time:
            status = "FROZEN AT END" if run.successful else "FROZEN ON FAILURE"
        else:
            status = "RUNNING"
        try:
            pixels = render_run_view(run, time_s)
            view = Image.fromarray(pixels, mode="RGB")
        except Exception as exc:
            view = Image.new("RGB", PANEL_VIEW_SIZE, "#f7e8e8")
            err_draw = ImageDraw.Draw(view)
            err_draw.text((14, 14), f"RENDER ERROR: {type(exc).__name__}: {exc}"[:90], font=detail_font, fill="#9b2d37")
        image.paste(view, (x, y + 31))
        final_position = _metric(run.result, "final_position_error_m")
        final_yaw = _metric(run.result, "final_yaw_error_rad")
        detail = (
            f"{status} · end {run.end_time:.2f}s · final epos {metric_text(final_position)}m · "
            f"eθ {metric_text(final_yaw, 3)}rad"
        )
        if time_s >= run.end_time and not run.successful:
            message = run.result.get("error_message")
            if message:
                detail += f" · {str(message)[:48]}"
        draw.text((x + 8, y + 314), detail[:92], font=detail_font, fill="#4e5c6e")
    footer = "Saved qpos state · model.xml · fixed path in yellow · recorded physical trace in controller color"
    draw.text((24, 773), footer, font=global_font, fill="#657183")
    return np.asarray(image, dtype=np.uint8)


def render_video(path: Path, case: tuple[str, str, int], records: dict[tuple[str, str, int, str], dict[str, Any]], summary_path: Path) -> tuple[dict[str, Any], dict[str, Run]]:
    import imageio_ffmpeg

    case_records = {controller: records.get((*case, controller)) for controller in CONTROLLERS}
    available_records = [record for record in case_records.values() if record is not None]
    # One camera from the union of all saved paths and physical trajectories
    # gives every controller panel exactly the same framing for this case.
    setup: list[tuple[dict[str, Any], mujoco.MjModel, np.ndarray, np.ndarray, int]] = []
    source_paths: set[Path] = set()
    unavailable: dict[str, UnavailableRun] = {}
    for controller, record in case_records.items():
        if record is None:
            continue
        try:
            model_path = resolve_artifact(summary_path, record, "model_xml", ("model_xml", "model"))
            qpos_path = resolve_artifact(summary_path, record, "physics_qpos_csv", ("physics_qpos_csv", "qpos_csv", "physics_qpos"))
            path_csv = resolve_artifact(summary_path, record, "path_csv", ("path_csv", "path"))
            model = load_render_model(model_path)
            _times, qpos = read_qpos(qpos_path, model)
            root = root_qpos_address(model)
            xy = read_path_xy(path_csv)
            setup.append((record, model, qpos, xy, root))
            source_paths.update((model_path, qpos_path, path_csv))
        except Exception as exc:
            result_path = None
            try:
                result_path = resolve_artifact(summary_path, record, "result_json", ("result_json", "result"))
            except (FileNotFoundError, ValueError):
                pass
            if result_path is not None:
                source_paths.add(result_path)
            end = max(
                0.0,
                float(record.get("task_seconds", 0.0) or 0.0) + float(record.get("settle_seconds", 0.0) or 0.0),
            )
            unavailable[controller] = UnavailableRun(
                result=record,
                end_time=end,
                reason=f"{type(exc).__name__}: {exc}",
            )
    if setup:
        camera = camera_for_paths(setup[0][1], np.vstack([item[3] for item in setup]), np.vstack([item[2][:, item[4] : item[4] + 2] for item in setup]), 0)
    else:
        camera = mujoco.MjvCamera()
        mujoco.mjv_defaultCamera(camera)
    runs: dict[str, Run] = {}
    try:
        for record in available_records:
            controller = run_key(record).controller
            try:
                run = make_run(summary_path, record, camera)
                runs[run.key.controller] = run
                source_paths.update(run.artifacts.values())
            except Exception as exc:
                result_path = None
                try:
                    result_path = resolve_artifact(summary_path, record, "result_json", ("result_json", "result"))
                except (FileNotFoundError, ValueError):
                    pass
                if result_path is not None:
                    source_paths.add(result_path)
                end = max(
                    0.0,
                    float(record.get("task_seconds", 0.0) or 0.0)
                    + float(record.get("settle_seconds", 0.0) or 0.0),
                )
                unavailable[controller] = UnavailableRun(
                    result=record,
                    end_time=end,
                    reason=f"{type(exc).__name__}: {exc}",
                )
        duration = max(
            [run.end_time for run in runs.values()]
            + [run.end_time for run in unavailable.values()]
            + [0.0]
        )
        frame_count = min(MAX_VIDEO_FRAMES, max(2, int(math.ceil(duration * VIDEO_FPS)) + 1))
        frame_times = np.linspace(0.0, duration, frame_count, dtype=np.float64)
        writer = imageio_ffmpeg.write_frames(
            str(path), VIDEO_SIZE, pix_fmt_in="rgb24", pix_fmt_out="yuv420p", fps=VIDEO_FPS,
            codec="libx264", macro_block_size=16, output_params=["-movflags", "+faststart"],
        )
        writer.send(None)
        try:
            for time_s in frame_times:
                frame = compose_frame(case, runs, float(time_s), duration, unavailable)
                if frame.shape != (VIDEO_SIZE[1], VIDEO_SIZE[0], 3):
                    raise RuntimeError(f"frame has unexpected dimensions {frame.shape}")
                writer.send(np.ascontiguousarray(frame))
        finally:
            writer.close()
        frame_total, encoded_duration = imageio_ffmpeg.count_frames_and_secs(str(path))
        if frame_total != frame_count:
            raise RuntimeError(f"decoded {frame_total} frames from {path}; expected {frame_count}")
        if (VIDEO_SIZE[0] % 16) or (VIDEO_SIZE[1] % 16):
            raise RuntimeError("video dimensions must be divisible by 16")
        return ({
            "path": artifact_label(path),
            "sha256": sha256_file(path),
            "width": VIDEO_SIZE[0],
            "height": VIDEO_SIZE[1],
            "fps": VIDEO_FPS,
            "frame_count": frame_total,
            "duration_s": encoded_duration,
            "virtual_end_time_s": duration,
            "frame_limit": MAX_VIDEO_FRAMES,
            "decoded_all_frames": True,
            "unavailable_physics_runs": {
                controller: {"reason": item.reason, "outcome": item.result.get("outcome")}
                for controller, item in unavailable.items()
            },
            "source_files": sorted(artifact_label(source) for source in source_paths),
        }, runs)
    except Exception:
        for run in runs.values():
            run.renderer.close()
        raise


def render(
    summary_path: Path,
    output_dir: Path,
    scenario: str | None = None,
    profile: str | None = None,
    seed: int | None = None,
) -> dict[str, Any]:
    summary_path = summary_path.resolve()
    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    _summary, records = load_summary(summary_path)
    if not records:
        raise ValueError("summary has no physics run records")
    selected_cases = selected_case_keys(records, scenario, profile, seed)
    case_records: dict[tuple[str, str, int, str], dict[str, Any]] = {}
    for record in records:
        key = run_key(record)
        full_key = (*key.case, key.controller)
        if full_key in case_records:
            raise ValueError(f"duplicate summary run for {full_key}")
        case_records[full_key] = record

    aggregate = aggregate_metrics(records)
    aggregate["selection"] = {
        "scenario": scenario,
        "profile": profile,
        "seed": seed,
        "video_cases": [{"scenario": case[0], "profile": case[1], "seed": case[2]} for case in selected_cases],
        "aggregate_scope": "all runs in source summary",
    }
    aggregate_path = output_dir / "aggregate.json"
    comparison_path = output_dir / "comparison.png"
    render_aggregate_png(comparison_path, aggregate)
    aggregate["outputs"] = {
        "comparison_png": {"path": artifact_label(comparison_path), "sha256": sha256_file(comparison_path)},
        "videos": [],
        "aggregate_json": {"path": artifact_label(aggregate_path)},
    }

    videos = []
    for case in selected_cases:
        name = f"trajectory-{case[0]}-{case[1]}-seed-{case[2]}.mp4"
        video_metadata, runs = render_video(output_dir / name, case, case_records, summary_path)
        videos.append(video_metadata)
        for run in runs.values():
            run.renderer.close()
    aggregate["outputs"]["videos"] = videos
    source_hashes: dict[str, dict[str, str]] = {
        artifact_label(summary_path): {"sha256": sha256_file(summary_path)},
    }
    for video in videos:
        for source in video["source_files"]:
            source_path = Path(source)
            if not source_path.is_absolute():
                source_path = ROOT / source_path
            if source_path.is_file():
                source_hashes[artifact_label(source_path)] = {"sha256": sha256_file(source_path)}
    aggregate["provenance"] = {
        "summary_json": {"path": artifact_label(summary_path), "sha256": sha256_file(summary_path)},
        "source_files": source_hashes,
        "renderer": {"path": artifact_label(Path(__file__)), "sha256": sha256_file(Path(__file__))},
    }
    aggregate_path.write_text(json.dumps(aggregate, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    metadata_path = output_dir / "render-metadata.json"
    metadata = {
        "schema_version": 1,
        "generated_utc": datetime.now(timezone.utc).isoformat(),
        "renderer": {"path": artifact_label(Path(__file__)), "sha256": sha256_file(Path(__file__))},
        "sources": source_hashes,
        "video_properties": videos,
        "outputs": {
            "aggregate_json": {"path": artifact_label(aggregate_path), "sha256": sha256_file(aggregate_path)},
            "comparison_png": {"path": artifact_label(comparison_path), "sha256": sha256_file(comparison_path)},
            "videos": videos,
        },
    }
    metadata_path.write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return aggregate


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summary", required=True, type=Path, help="physics experiment summary.json")
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--scenario", help="restrict video cases to one scenario")
    parser.add_argument("--profile", "--dynamics", dest="profile", help="restrict video cases to a physics profile")
    parser.add_argument("--seed", type=int, help="restrict video cases to one seed")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    render(args.summary, args.output_dir, args.scenario, args.profile, args.seed)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

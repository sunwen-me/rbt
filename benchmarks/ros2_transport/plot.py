#!/usr/bin/env python3
"""Plot validated CPU and PSS comparisons from retained ROS 2 results."""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import sys
from pathlib import Path

CASES = ("imu", "cloud", "video")
MODES = ("rbt", "shm", "loan")
METRICS = ("total_cpu_percent", "median_total_pss_kib")
LABELS = {
    "rbt": "RBT native loan",
    "shm": "ROS 2 SHM copy",
    "loan": "ROS 2 loan + Data Sharing",
}
COLORS = {"rbt": "#0072B2", "shm": "#E69F00", "loan": "#009E73"}
CASE_LABELS = {
    "imu": "IMU\n256 B · 200 Hz",
    "cloud": "Cloud\n1.60 MB · 10 Hz",
    "video": "Video\n1080p RGB8 · 30 Hz",
}
DEFAULT_RESULTS = Path(__file__).resolve().parent / "results" / "2026-10-29"


class DataError(ValueError):
    pass


def load_results(directory: Path):
    if not directory.is_dir():
        raise DataError(f"results directory does not exist: {directory}")
    try:
        summary = json.loads((directory / "summary.json").read_text(encoding="utf-8"))
        with (directory / "trials.csv").open(newline="", encoding="utf-8") as source:
            trials = list(csv.DictReader(source))
    except (OSError, json.JSONDecodeError, csv.Error) as exc:
        raise DataError(f"cannot read retained results: {exc}") from exc
    if not isinstance(summary, dict) or summary.get("all_trials_valid") is not True:
        raise DataError("summary.json does not certify all trials as valid")
    return summary, trials


def number(value, context: str) -> float:
    try:
        result = float(value)
    except (TypeError, ValueError) as exc:
        raise DataError(f"{context}: expected a number, got {value!r}") from exc
    if not math.isfinite(result) or result < 0:
        raise DataError(f"{context}: expected a finite nonnegative number, got {value!r}")
    return result


def validate(summary, trials):
    expected = {(case, mode) for case in CASES for mode in MODES}
    grouped = {key: [] for key in expected}
    for index, row in enumerate(trials, 1):
        key = (row.get("case"), row.get("mode"))
        if key not in expected:
            raise DataError(f"trial row {index}: unexpected case/mode {key!r}")
        if row.get("valid", "").lower() != "true" or row.get("status") != "valid":
            raise DataError(f"trial row {index}: not marked valid")
        grouped[key].append(row)
    if len(trials) != 27:
        raise DataError(f"expected 27 retained trials, found {len(trials)}")

    aggregates = summary.get("case_mode_aggregates")
    if not isinstance(aggregates, dict):
        raise DataError("summary.json is missing case_mode_aggregates")
    data = {metric: {case: {} for case in CASES} for metric in METRICS}
    for case in CASES:
        for mode in MODES:
            rows = grouped[(case, mode)]
            if len(rows) != 3:
                raise DataError(f"{case}/{mode}: expected 3 trials, found {len(rows)}")
            aggregate = aggregates.get(case, {}).get(mode, {})
            if (aggregate.get("aggregate_status") != "ok"
                    or aggregate.get("trial_count") != 3
                    or aggregate.get("valid_trial_count") != 3):
                raise DataError(f"summary.json has no valid triplicate for {case}/{mode}")
            for metric in METRICS:
                values = [number(row.get(metric), f"{case}/{mode} {metric}") for row in rows]
                stats = aggregate.get("metrics", {}).get(metric, {})
                if stats.get("n") != 3:
                    raise DataError(f"summary.json is missing triplicate {metric} for {case}/{mode}")
                actual = {"min": min(values), "median": statistics.median(values), "max": max(values)}
                for name, value in actual.items():
                    recorded = number(stats.get(name), f"summary {case}/{mode} {metric}.{name}")
                    if not math.isclose(value, recorded, rel_tol=1e-10, abs_tol=1e-10):
                        raise DataError(f"summary/trials mismatch for {case}/{mode} {metric}.{name}")
                data[metric][case][mode] = values
    return data


def render(plt, Line2D, Patch, directory: Path, data, *, title: str, ylabel: str,
           stem: str, divisor: float = 1.0) -> None:
    plt.rcParams.update({"svg.fonttype": "none", "svg.hashsalt": "rbt-transport-2026-10-29"})
    fig, ax = plt.subplots(figsize=(14, 7.2), facecolor="white")
    fig.subplots_adjust(left=0.085, right=0.985, top=0.82, bottom=0.24)
    fig.suptitle(title, fontsize=19, fontweight="bold", y=0.965)
    centers, width = range(len(CASES)), 0.22
    offsets = {"rbt": -width, "shm": 0.0, "loan": width}
    high = 0.0
    for ci, case in enumerate(CASES):
        for mode in MODES:
            values = [v / divisor for v in data[case][mode]]
            low, median, high_value = min(values), statistics.median(values), max(values)
            high = max(high, high_value)
            x = ci + offsets[mode]
            ax.bar(x, median, width * 0.88, color=COLORS[mode], edgecolor="white", zorder=2)
            ax.errorbar(x, median, yerr=[[median - low], [high_value - median]], fmt="none",
                        ecolor="#25313c", elinewidth=1.15, capsize=4, capthick=1.15, zorder=4)
            ax.scatter([x - 0.035, x, x + 0.035], values, s=25, color=COLORS[mode],
                       edgecolors="white", linewidths=0.65, zorder=5)
            ax.text(x, high_value + max(high_value, 1.0) * 0.018, f"{median:.2f}",
                    ha="center", va="bottom", fontsize=9, color="#202a33", zorder=6)

    handles = [Patch(facecolor=COLORS[m], label=LABELS[m]) for m in MODES]
    handles.extend([
        Line2D([], [], marker="o", linestyle="none", markersize=5, color="#25313c", label="Individual trial"),
        Line2D([], [], color="#25313c", marker="_", markersize=9, label="Observed min–max (not CI)"),
    ])
    fig.legend(handles=handles, loc="upper center", bbox_to_anchor=(0.5, 0.89), ncol=5,
               frameon=False, fontsize=9.5, columnspacing=1.45, handlelength=1.35)
    ax.set_xticks(list(centers), [CASE_LABELS[case] for case in CASES], fontsize=11)
    ax.set_ylabel(ylabel, fontsize=11)
    ax.set_xlim(-0.58, len(CASES) - 0.42)
    ax.set_ylim(0, high * 1.16 + 0.02)
    ax.set_axisbelow(True)
    ax.yaxis.grid(True, color="#d8dee5", linewidth=0.8, alpha=0.8)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color("#aab3bc")
    ax.tick_params(axis="y", labelsize=9.5, colors="#38434e")
    ax.tick_params(axis="x", length=0, pad=9, colors="#202a33")
    fig.text(0.085, 0.145,
             "Bars: medians across 3 trials; dots: each trial; whiskers: observed min–max range (not confidence intervals).",
             fontsize=9, color="#35414c")
    fig.text(0.085, 0.085,
             "Custom fixed-layout synthetic messages on both stacks · x86_64 · combined publisher + subscriber · standard sensor_msgs: capability probe only, not performance-measured.",
             fontsize=8.7, color="#35414c")
    fig.savefig(directory / f"{stem}.png", dpi=100, facecolor="white", metadata={"Software": "Matplotlib"})
    fig.savefig(directory / f"{stem}.svg", format="svg", facecolor="white",
                 metadata={"Date": None, "Title": title, "Creator": "benchmarks/ros2_transport/plot.py"})
    svg = directory / f"{stem}.svg"
    svg.write_text("\n".join(line.rstrip() for line in svg.read_text(encoding="utf-8").splitlines())
                   + "\n", encoding="utf-8")
    plt.close(fig)


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results-dir", type=Path, default=DEFAULT_RESULTS,
                        help="directory with summary.json and trials.csv; outputs go here")
    args = parser.parse_args(argv)
    try:
        summary, trials = load_results(args.results_dir)
        data = validate(summary, trials)
    except DataError as exc:
        parser.error(str(exc))
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from matplotlib.lines import Line2D
        from matplotlib.patches import Patch
    except ImportError as exc:
        parser.error(f"Matplotlib is required only for plotting: {exc}")

    render(plt, Line2D, Patch, args.results_dir, data["total_cpu_percent"],
           title="Transport endpoint CPU usage",
           ylabel="Combined publisher + subscriber CPU (%)\n100% = one logical core",
           stem="cpu-usage")
    render(plt, Line2D, Patch, args.results_dir, data["median_total_pss_kib"],
           title="Transport endpoint memory (PSS)",
           ylabel="Combined publisher + subscriber PSS (MiB)",
           stem="memory-pss", divisor=1024.0)
    print(f"Wrote CPU and PSS charts to {args.results_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

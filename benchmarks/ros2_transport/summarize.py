#!/usr/bin/env python3
"""Export portable trial rows and valid-trial aggregates from run.py results."""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import sys
from collections import Counter
from pathlib import Path
from typing import Any

AGGREGATE_METRICS = (
    "publisher_cpu_percent",
    "subscriber_cpu_percent",
    "total_cpu_percent",
    "median_total_pss_kib",
    "peak_total_pss_kib",
    "median_sum_rss_kib",
    "peak_sum_rss_kib",
    "latency_p50_us",
    "latency_p99_us",
    "publisher_rate_hz",
    "delivery_ratio",
    "shared_alias_matches",
)
BASE_COLUMNS = (
    "trial_index",
    "case",
    "mode",
    "repetition",
    "order_in_repetition",
    "status",
    "valid",
    "reason",
    "quality_reasons",
)
RESERVED_COLUMNS = set(BASE_COLUMNS)


def is_scalar(value: Any) -> bool:
    return value is None or isinstance(value, (str, bool, int, float))


def finite_number(value: Any) -> int | float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    if isinstance(value, float) and not math.isfinite(value):
        return None
    return value


def portable_path_name(value: Any) -> str | None:
    if not isinstance(value, str) or not value:
        return None
    return Path(value.rstrip("/")).name


def portable_configuration(configuration: dict[str, Any]) -> dict[str, Any]:
    result = {
        key: value
        for key, value in configuration.items()
        if key not in {"rbt_executable", "ros_executable", "ros_setup"}
    }
    executables = {
        "rbt": portable_path_name(configuration.get("rbt_executable")),
        "ros": portable_path_name(configuration.get("ros_executable")),
    }
    result["executables"] = {key: value for key, value in executables.items() if value}
    result["ros_setup_used"] = bool(configuration.get("ros_setup"))
    return result


def portable_runtime_metadata(metadata: dict[str, Any]) -> dict[str, Any]:
    result = dict(metadata)
    for key, value in result.items():
        if isinstance(value, str) and value.startswith("/"):
            result[key] = portable_path_name(value)
    return result


def valid_trial(trial: dict[str, Any]) -> bool:
    metrics = trial.get("metrics")
    metric_valid = metrics.get("valid") if isinstance(metrics, dict) else None
    valid = trial.get("valid")
    if valid is None:
        valid = metric_valid
    return trial.get("status") == "valid" and valid is True


def portable_text(value: Any, trial: dict[str, Any]) -> str:
    if not isinstance(value, str):
        return ""
    text = value
    trial_dir = trial.get("trial_dir")
    if isinstance(trial_dir, str) and trial_dir:
        text = text.replace(trial_dir, "<trial-dir>")
    executable = trial.get("executable")
    if isinstance(executable, str) and executable.startswith("/"):
        text = text.replace(executable, Path(executable).name)
    return text


def portable_reason(trial: dict[str, Any]) -> str:
    return portable_text(trial.get("reason"), trial)


def metric_values(trial: dict[str, Any]) -> dict[str, Any]:
    metrics = trial.get("metrics")
    if not isinstance(metrics, dict):
        return {}
    return {
        key: value
        for key, value in metrics.items()
        if key not in {"valid", "reasons"} and is_scalar(value)
    }


def summarize_results(
    data: dict[str, Any], source_revision: str
) -> tuple[dict[str, Any], list[dict[str, Any]], list[str]]:
    trials = data.get("trials")
    if not isinstance(trials, list) or any(not isinstance(item, dict) for item in trials):
        raise ValueError("results.json must contain a trials array of objects")
    configuration = data.get("configuration")
    if not isinstance(configuration, dict):
        raise ValueError("results.json must contain a configuration object")
    metadata = data.get("runtime_metadata", {})
    if not isinstance(metadata, dict):
        raise ValueError("runtime_metadata must be an object")
    if not source_revision.strip():
        raise ValueError("source revision must not be empty")

    metric_columns = sorted(
        {
            key
            for trial in trials
            for key in metric_values(trial)
            if key not in RESERVED_COLUMNS
        }
    )
    cases = {str(value) for value in configuration.get("cases", []) if isinstance(value, str)}
    modes = {str(value) for value in configuration.get("modes", []) if isinstance(value, str)}
    grouped: dict[tuple[str, str], list[dict[str, Any]]] = {
        (case, mode): [] for case in sorted(cases) for mode in sorted(modes)
    }
    status_counts: Counter[str] = Counter()
    valid_count = 0
    trial_rows: list[dict[str, Any]] = []
    profile_hashes: list[dict[str, Any]] = []
    verification: list[dict[str, Any]] = []

    for trial in trials:
        case = str(trial.get("case", "unknown"))
        mode = str(trial.get("mode", "unknown"))
        grouped.setdefault((case, mode), []).append(trial)
        status = str(trial.get("status", "unknown"))
        status_counts[status] += 1
        is_valid = valid_trial(trial)
        valid_count += int(is_valid)
        metrics = trial.get("metrics") if isinstance(trial.get("metrics"), dict) else {}
        quality_reasons = metrics.get("reasons", [])
        if not isinstance(quality_reasons, list):
            quality_reasons = []
        row: dict[str, Any] = {
            "trial_index": trial.get("trial_index"),
            "case": case,
            "mode": mode,
            "repetition": trial.get("repetition"),
            "order_in_repetition": trial.get("order_in_repetition"),
            "status": status,
            "valid": is_valid,
            "reason": portable_reason(trial),
            "quality_reasons": json.dumps(
                [portable_text(reason, trial) for reason in quality_reasons],
                separators=(",", ":"),
            ),
        }
        row.update(metric_values(trial))
        trial_rows.append(row)

        ready = trial.get("ready") or {}
        endpoint_summaries = trial.get("summaries") or {}
        check: dict[str, Any] = {
            "trial_index": trial.get("trial_index"),
            "case": case,
            "mode": mode,
            "endpoints": {},
        }
        for role in ("pub", "sub"):
            observed = ready.get(role) or {}
            measured = endpoint_summaries.get(role) or {}
            check["endpoints"][role] = {
                key: observed.get(key)
                for key in ("can_loan", "loan_enabled", "datasharing_kind", "participant_transport")
            }
            check["endpoints"][role]["loaned_count"] = measured.get("loaned_count")
        if isinstance(trial.get("cpu_sample_start_ns"), int) and isinstance(
            trial.get("cpu_sample_end_ns"), int
        ):
            check["cpu_interval_seconds"] = (
                trial["cpu_sample_end_ns"] - trial["cpu_sample_start_ns"]
            ) / 1e9
        verification.append(check)

        profile = trial.get("fastdds_profile")
        if isinstance(profile, dict):
            check["shm_transport"] = {
                key: profile.get(key) for key in ("max_message_size", "segment_size")
            }
            hashes = {
                key: profile[key]
                for key in ("template_sha256", "sha256")
                if isinstance(profile.get(key), str)
            }
            if hashes:
                profile_hashes.append({
                    "trial_index": trial.get("trial_index"),
                    "case": case,
                    "mode": mode,
                    "repetition": trial.get("repetition"),
                    **hashes,
                })

    aggregates: dict[str, dict[str, Any]] = {}
    for (case, mode), group in sorted(grouped.items()):
        valid_group = [trial for trial in group if valid_trial(trial)]
        metric_stats: dict[str, Any] = {}
        for metric in AGGREGATE_METRICS:
            values = [
                number
                for trial in valid_group
                if (number := finite_number(
                    (trial.get("metrics") or {}).get(metric)
                    if isinstance(trial.get("metrics"), dict) else None
                )) is not None
            ]
            metric_stats[metric] = {
                "n": len(values),
                "median": statistics.median(values) if values else None,
                "min": min(values) if values else None,
                "max": max(values) if values else None,
            }
        aggregates.setdefault(case, {})[mode] = {
            "trial_count": len(group),
            "valid_trial_count": len(valid_group),
            "aggregate_status": "ok" if valid_group else "no_valid_trials",
            "metrics": metric_stats,
        }

    run_status = data.get("status", "unknown")
    all_groups_present = bool(grouped) and all(group for group in grouped.values())
    repetitions = configuration.get("repetitions")
    expected_count = (
        len(cases) * len(modes) * repetitions
        if isinstance(repetitions, int) and repetitions > 0 else None
    )
    all_valid = (
        bool(trials) and valid_count == len(trials) and run_status == "completed"
        and all_groups_present
        and (expected_count is None or len(trials) == expected_count)
    )
    return {
        "schema_version": data.get("schema_version"),
        "created_at_utc": data.get("created_at_utc"),
        "source_revision": source_revision.strip(),
        "run_status": run_status,
        "all_trials_valid": all_valid,
        "trial_count": len(trials),
        "expected_trial_count": expected_count,
        "valid_trial_count": valid_count,
        "trial_status_counts": dict(sorted(status_counts.items())),
        "configuration": portable_configuration(configuration),
        "runtime_metadata": portable_runtime_metadata(metadata),
        "trial_profile_sha256": profile_hashes,
        "trial_verification": verification,
        "aggregate_metrics": list(AGGREGATE_METRICS),
        "case_mode_aggregates": aggregates,
        "outputs": ["trials.csv", "summary.json"],
    }, trial_rows, metric_columns


def write_outputs(
    output: Path,
    summary: dict[str, Any],
    rows: list[dict[str, Any]],
    metric_columns: list[str],
) -> None:
    output.mkdir(parents=True, exist_ok=False)
    columns = [*BASE_COLUMNS, *metric_columns]
    with (output / "trials.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(
            stream, fieldnames=columns, extrasaction="ignore", lineterminator="\n"
        )
        writer.writeheader()
        for row in rows:
            writer.writerow(
                {
                    key: "" if value is None else
                    "true" if value is True else
                    "false" if value is False else value
                    for key, value in row.items()
                }
            )
    (output / "summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True, allow_nan=False) + "\n",
        encoding="utf-8",
    )


def process_file(results_path: Path, output: Path, source_revision: str) -> int:
    with results_path.open("r", encoding="utf-8") as stream:
        data = json.load(stream)
    if not isinstance(data, dict):
        raise ValueError("results.json must contain a JSON object")
    summary, rows, metric_columns = summarize_results(data, source_revision)
    write_outputs(output, summary, rows, metric_columns)
    return 0 if summary["all_trials_valid"] else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("results_json", type=Path)
    parser.add_argument("--output", required=True, type=Path, help="new output directory")
    parser.add_argument("--source-revision", required=True)
    args = parser.parse_args(argv)
    try:
        result = process_file(args.results_json, args.output, args.source_revision)
    except (OSError, json.JSONDecodeError, ValueError) as error:
        parser.error(str(error))
    if result:
        print("summary written with invalid, unsupported, failed, or missing trials", file=sys.stderr)
    return result


if __name__ == "__main__":
    raise SystemExit(main())

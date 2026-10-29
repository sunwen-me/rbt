#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
export PATH="${repo_root}/target/phase4-toolchain/bin:${PATH}"
export CC="${repo_root}/target/phase4-toolchain/bin/x86_64-conda-linux-gnu-gcc"
export CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER="${CC}"
export LD_LIBRARY_PATH="${repo_root}/target/phase4-toolchain/lib:${LD_LIBRARY_PATH:-}"

queries=10000
original_args=("$@")
while (($#)); do
    case "$1" in
        --queries)
            (($# >= 2)) || { echo "--queries requires a value" >&2; exit 2; }
            queries="$2"
            shift 2
            ;;
        *)
            echo "unsupported benchmark argument: $1" >&2
            exit 2
            ;;
    esac
done
[[ "${queries}" =~ ^[0-9]+$ ]] && ((queries >= 10000 && queries <= 10000000)) || {
    echo "--queries must be in 10000..=10000000" >&2
    exit 2
}

cd "${repo_root}"
run_root="${RBT_TF_BENCH_LOG_DIR:-${repo_root}/target/tf-benchmark-logs}"
run_id="$(date -u +%Y%m%dT%H%M%SZ)-$$"
run_dir="${run_root}/${run_id}"
mkdir -p "${run_dir}"
binary="${repo_root}/target/debug/rbt-tf-tools"

set +e
cargo build -p rbt-tf-tools >"${run_dir}/build.log" 2>&1
build_status=$?
set -e

binary_hash=""
if ((build_status == 0)) && [[ -x "${binary}" ]]; then
    binary_hash="$(sha256sum "${binary}" | awk '{print $1}')"
else
    build_status=1
fi

python3 - "${run_dir}" "${queries}" "${build_status}" "${binary_hash}" "${original_args[@]}" <<'PY'
import datetime
import hashlib
import json
import pathlib
import sys

run_dir = pathlib.Path(sys.argv[1])
queries = int(sys.argv[2])
build_status = int(sys.argv[3])
binary_hash = sys.argv[4] or None
original_args = sys.argv[5:]
root = pathlib.Path.cwd()

def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()

metadata = {
    "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "probe_argv": ["scripts/benchmark_tf.sh", *original_args],
    "argv": [str(root / "target/debug/rbt-tf-tools"), "bench", "--queries", str(queries)],
    "build_mode": "debug",
    "build_status": build_status,
    "tool_binary": str(root / "target/debug/rbt-tf-tools"),
    "post_build_binary_sha256": binary_hash,
    "source_sha256": {
        "crates/rbt-tf-tools/src/main.rs": sha256(root / "crates/rbt-tf-tools/src/main.rs"),
        "crates/rbt-tf-tools/Cargo.toml": sha256(root / "crates/rbt-tf-tools/Cargo.toml"),
        "Cargo.lock": sha256(root / "Cargo.lock"),
        "scripts/benchmark_tf.sh": sha256(root / "scripts/benchmark_tf.sh"),
    },
    "queries_per_case_requested": queries,
}
(run_dir / "run-metadata.json").write_text(json.dumps(metadata, indent=2, sort_keys=True) + "\n")
PY

run_status=0
if ((build_status != 0)); then
    run_status=1
else
    if "${binary}" bench --queries "${queries}" >"${run_dir}/result.json" 2>"${run_dir}/stderr.log"; then
        run_status=0
    else
        run_status=$?
    fi
fi

set +e
python3 - "${run_dir}" "${queries}" "${run_status}" <<'PY'
import datetime
import json
import pathlib
import sys

run_dir = pathlib.Path(sys.argv[1])
queries = int(sys.argv[2])
run_status = int(sys.argv[3])
metadata = json.loads((run_dir / "run-metadata.json").read_text())
result = None
error = None
try:
    result = json.loads((run_dir / "result.json").read_text())
    cases = result["cases"]
    names = {case["name"] for case in cases}
    expected_names = {"static_1_edge", "static_3_edge", "static_10_edge", "history_interpolation"}
    if names != expected_names or len(cases) != 4:
        raise ValueError(f"unexpected benchmark cases: {sorted(names)}")
    for case in cases:
        if case["queries"] != queries or case["correct_results"] != queries:
            raise ValueError(f"case {case['name']} did not validate all {queries} queries")
        required = (
            "p50_latency_ns", "p90_latency_ns", "p99_latency_ns", "max_latency_ns",
            "queries_per_second", "rss_before_kib", "rss_after_kib",
        )
        if any(key not in case for key in required) or case["queries_per_second"] <= 0:
            raise ValueError(f"case {case['name']} is missing measured latency or RSS fields")
    if metadata["build_status"] != 0 or not metadata["post_build_binary_sha256"]:
        raise ValueError("build did not produce a hashed benchmark binary")
    if run_status != 0:
        raise ValueError(f"benchmark process exited with status {run_status}")
except Exception as exc:
    error = f"{type(exc).__name__}: {exc}"

summary = {
    "status": "PASS" if error is None else "FAIL",
    "completed_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "run_metadata_file": str(run_dir / "run-metadata.json"),
    "result_file": str(run_dir / "result.json"),
    "stderr_file": str(run_dir / "stderr.log"),
    "build_log_file": str(run_dir / "build.log"),
    "build_mode": metadata["build_mode"],
    "post_build_binary_sha256": metadata["post_build_binary_sha256"],
    "queries_per_case": queries,
    "cases": result.get("cases") if isinstance(result, dict) else None,
    "error": error,
}
(run_dir / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
print(json.dumps(summary, separators=(",", ":"), sort_keys=True))
sys.exit(0 if error is None else 1)
PY
validation_status=$?
set -e
if ((run_status != 0 || validation_status != 0)); then
    echo "TF benchmark failed; evidence retained in ${run_dir}" >&2
    exit 1
fi

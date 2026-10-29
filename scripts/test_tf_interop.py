#!/usr/bin/env python3
"""Build and run the directed native Rust/C++/Python TF interop matrix."""

from __future__ import annotations

import argparse
import json
import math
import os
import shlex
import signal
import subprocess
import sys
import time
import uuid
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Sequence


REPO_ROOT = Path(__file__).resolve().parent.parent
PYTHON_PROBE = REPO_ROOT / "python/tests/tf_interop_probe.py"
SEMANTICS_FIXTURE = REPO_ROOT / "tests/fixtures/tf/semantics.json"
DEFAULT_PAIRS = (
    ("rust", "cpp"),
    ("cpp", "rust"),
    ("rust", "python"),
    ("python", "rust"),
    ("cpp", "python"),
    ("python", "cpp"),
)
LANGUAGES = ("rust", "cpp", "python")
SEMANTICS_FIELDS = (
    "direction_point",
    "inverse_point",
    "chain_point",
    "interpolated_translation",
    "interpolated_quaternion_xyzw",
    "wrong_clock_error",
    "expired_mapping_error",
    "mapped_point",
)
INTEROP_STATUS_FIELDS = (
    "static_snapshot",
    "static_update",
    "dynamic",
    "lookup",
)


class HarnessError(RuntimeError):
    """Raised when a build, probe, or cross-language assertion fails."""


def repo_path(path: Path) -> Path:
    return path if path.is_absolute() else (REPO_ROOT / path).resolve()


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def read_semantics_fixture(path: Path = SEMANTICS_FIXTURE) -> dict[str, Any]:
    try:
        fixture = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise HarnessError(f"cannot read TF semantics fixture {path}: {error}") from error
    if fixture.get("schema") != "rbt.tf-semantics-v1":
        raise HarnessError("TF semantics fixture schema is invalid")
    expected = fixture.get("expected")
    if not isinstance(expected, dict) or set(expected) != set(SEMANTICS_FIELDS):
        raise HarnessError("TF semantics fixture has missing or unexpected result fields")
    tolerance = fixture.get("tolerance")
    if not isinstance(tolerance, dict) or set(tolerance) != {"absolute", "relative"}:
        raise HarnessError("TF semantics fixture tolerance is invalid")
    for name, value in tolerance.items():
        if (
            isinstance(value, bool)
            or not isinstance(value, (int, float))
            or not math.isfinite(float(value))
            or value < 0
        ):
            raise HarnessError(f"TF semantics fixture tolerance {name} must be nonnegative")
    return fixture


def _close(actual: float, expected: float, absolute: float, relative: float) -> bool:
    return math.isfinite(actual) and abs(actual - expected) <= absolute + relative * abs(expected)


def _compare_vector(
    name: str,
    actual: Any,
    expected: Any,
    absolute: float,
    relative: float,
    *,
    quaternion: bool = False,
) -> None:
    if not isinstance(actual, list) or not isinstance(expected, list) or len(actual) != len(expected):
        raise HarnessError(f"semantics field {name} must be a vector of length {len(expected)}")
    if any(isinstance(value, bool) or not isinstance(value, (int, float)) for value in actual):
        raise HarnessError(f"semantics field {name} contains a nonnumeric component")

    def matches(sign: float) -> bool:
        return all(
            _close(float(value), sign * float(target), absolute, relative)
            for value, target in zip(actual, expected)
        )

    if matches(1.0):
        return
    if quaternion and matches(-1.0):
        return
    suffix = " (up to quaternion sign)" if quaternion else ""
    raise HarnessError(f"semantics field {name} differs from the fixture{suffix}")


def validate_semantics_result(
    actual: Any,
    expected_language: str,
    fixture: dict[str, Any] | None = None,
) -> dict[str, Any]:
    fixture = read_semantics_fixture() if fixture is None else fixture
    if not isinstance(actual, dict) or not actual:
        raise HarnessError("semantics probe emitted no result object")
    expected_fields = fixture["expected"]
    allowed_fields = set(expected_fields) | {"language", "diagnostics"}
    unexpected = set(actual) - allowed_fields
    if unexpected:
        raise HarnessError(f"semantics result has unexpected fields: {', '.join(sorted(unexpected))}")
    missing = set(expected_fields) - set(actual)
    if missing:
        raise HarnessError(f"semantics result is missing fields: {', '.join(sorted(missing))}")
    language = actual.get("language")
    if language != expected_language:
        raise HarnessError(
            f"semantics language={language!r}, expected {expected_language!r}"
        )
    if "diagnostics" in actual and actual["diagnostics"] in (None, "", [], {}):
        raise HarnessError("semantics diagnostics field must not be empty when present")

    tolerance = fixture["tolerance"]
    absolute = float(tolerance["absolute"])
    relative = float(tolerance["relative"])
    for name, expected in expected_fields.items():
        value = actual[name]
        if value is None or value == "" or value == [] or value == {}:
            raise HarnessError(f"semantics field {name} is empty")
        if name in (
            "direction_point",
            "inverse_point",
            "chain_point",
            "interpolated_translation",
            "interpolated_quaternion_xyzw",
            "mapped_point",
        ):
            _compare_vector(
                name,
                value,
                expected,
                absolute,
                relative,
                quaternion=name == "interpolated_quaternion_xyzw",
            )
        elif value != expected:
            raise HarnessError(
                f"semantics field {name}={value!r}, expected {expected!r}"
            )
    return actual


def json_objects(text: str) -> list[dict[str, Any]]:
    decoder = json.JSONDecoder()
    objects: list[dict[str, Any]] = []
    offset = 0
    while offset < len(text):
        start = text.find("{", offset)
        if start < 0:
            break
        try:
            value, end = decoder.raw_decode(text, start)
        except json.JSONDecodeError:
            offset = start + 1
            continue
        if isinstance(value, dict):
            objects.append(value)
        offset = max(end, start + 1)
    return objects


def last_result_object(stdout_path: Path, *, semantics: bool) -> dict[str, Any]:
    try:
        text = stdout_path.read_text(encoding="utf-8")
    except OSError as error:
        raise HarnessError(f"cannot read probe stdout {stdout_path}: {error}") from error
    candidates = json_objects(text)
    if semantics:
        candidates = [
            item for item in candidates if "language" in item and any(key in item for key in SEMANTICS_FIELDS)
        ]
    else:
        candidates = [item for item in candidates if "status" in item or "static_snapshot" in item]
    if not candidates:
        kind = "semantics" if semantics else "interop"
        raise HarnessError(f"{kind} probe emitted no JSON result object: {stdout_path}")
    return candidates[-1]


def validate_interop_result(actual: Any, language: str, role: str) -> dict[str, Any]:
    if role not in ("publisher", "consumer"):
        raise HarnessError(f"invalid probe role {role!r}")
    if not isinstance(actual, dict) or not actual:
        raise HarnessError(f"{role} {language} probe emitted no result object")
    if actual.get("status") != "PASS":
        raise HarnessError(f"{role} {language} status={actual.get('status')!r}, expected 'PASS'")
    if actual.get("language") != language:
        raise HarnessError(
            f"{role} language={actual.get('language')!r}, expected {language!r}"
        )
    for name in INTEROP_STATUS_FIELDS:
        if actual.get(name) != "PASS":
            raise HarnessError(f"{role} {language} {name}={actual.get(name)!r}, expected 'PASS'")
    revision = actual.get("static_revision")
    if isinstance(revision, bool) or not isinstance(revision, int) or revision != 11:
        raise HarnessError(f"{role} {language} static_revision={revision!r}, expected 11")
    for name in ("dynamic_samples", "borrowed_samples", "pending_rpc"):
        value = actual.get(name)
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            raise HarnessError(f"{role} {language} {name} must be a nonnegative integer")
    if actual["dynamic_samples"] != 2:
        raise HarnessError(
            f"{role} {language} dynamic_samples={actual['dynamic_samples']}, expected 2"
        )
    if actual["borrowed_samples"] != 0:
        raise HarnessError(f"{role} {language} borrowed_samples must be 0")
    if actual["pending_rpc"] != 0:
        raise HarnessError(f"{role} {language} pending_rpc must be 0")
    return actual


def _linux_group_members(pgid: int) -> list[int] | None:
    proc_root = Path("/proc")
    if not proc_root.is_dir():
        return None
    members: list[int] = []
    for entry in proc_root.iterdir():
        if not entry.name.isdigit():
            continue
        try:
            stat = (entry / "stat").read_text(encoding="ascii")
            close = stat.rfind(")")
            fields = stat[close + 1 :].split()
            state = fields[0]
            process_group = int(fields[2])
        except (OSError, ValueError, IndexError):
            continue
        if process_group == pgid and state not in ("Z", "X"):
            members.append(int(entry.name))
    return members


def process_group_alive(pgid: int) -> bool:
    members = _linux_group_members(pgid)
    if members is not None:
        return bool(members)
    try:
        os.killpg(pgid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True


def live_group_members(pgid: int) -> list[int]:
    members = _linux_group_members(pgid)
    if members is not None:
        return members
    return [pgid] if process_group_alive(pgid) else []


@dataclass
class ManagedProcess:
    name: str
    argv: list[str]
    cwd: Path
    stdout_path: Path
    stderr_path: Path
    command_path: Path
    process: subprocess.Popen[bytes]
    started_at: float
    finished_at: float | None = None
    return_code: int | None = None

    @classmethod
    def start(
        cls,
        *,
        name: str,
        argv: Sequence[str | Path],
        cwd: Path,
        log_dir: Path,
        env: dict[str, str],
    ) -> "ManagedProcess":
        log_dir.mkdir(parents=True, exist_ok=True)
        command = [str(part) for part in argv]
        stdout_path = log_dir / f"{name}.stdout.log"
        stderr_path = log_dir / f"{name}.stderr.log"
        command_path = log_dir / f"{name}.command.json"
        write_json(
            command_path,
            {"argv": command, "display": shlex.join(command), "cwd": str(cwd)},
        )
        started = time.monotonic()
        try:
            with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
                process = subprocess.Popen(
                    command,
                    cwd=cwd,
                    env=env,
                    stdin=subprocess.DEVNULL,
                    stdout=stdout,
                    stderr=stderr,
                    start_new_session=True,
                )
        except OSError as error:
            raise HarnessError(f"could not start {name} command {shlex.join(command)}: {error}") from error
        return cls(
            name,
            command,
            cwd,
            stdout_path,
            stderr_path,
            command_path,
            process,
            started,
        )

    @property
    def pid(self) -> int:
        return self.process.pid

    @property
    def pgid(self) -> int:
        return self.process.pid

    def poll(self) -> int | None:
        result = self.process.poll()
        if result is not None and self.finished_at is None:
            self.finished_at = time.monotonic()
            self.return_code = result
        return result

    def wait(self, timeout: float | None = None) -> int:
        try:
            result = self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            self.terminate()
            raise HarnessError(f"{self.name} timed out; see {self.stdout_path} and {self.stderr_path}")
        self.finished_at = time.monotonic()
        self.return_code = result
        return result

    def terminate(self) -> list[int]:
        if self.poll() is None or process_group_alive(self.pgid):
            try:
                os.killpg(self.pgid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            except OSError:
                pass

        if self.process.poll() is None:
            try:
                self.process.wait(timeout=0.75)
            except subprocess.TimeoutExpired:
                pass

        deadline = time.monotonic() + 0.75
        while time.monotonic() < deadline and process_group_alive(self.pgid):
            time.sleep(0.02)
        if process_group_alive(self.pgid):
            try:
                os.killpg(self.pgid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            except OSError:
                pass

        if self.process.poll() is None:
            try:
                self.process.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                pass
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline and process_group_alive(self.pgid):
            time.sleep(0.02)

        result = self.poll()
        if result is not None:
            self.return_code = result
        return live_group_members(self.pgid)

    def evidence(self) -> dict[str, Any]:
        elapsed = (self.finished_at or time.monotonic()) - self.started_at
        return {
            "name": self.name,
            "argv": self.argv,
            "display": shlex.join(self.argv),
            "pid": self.pid,
            "pgid": self.pgid,
            "return_code": self.return_code,
            "elapsed_seconds": round(elapsed, 6),
            "stdout": str(self.stdout_path),
            "stderr": str(self.stderr_path),
            "command": str(self.command_path),
        }


def run_logged(
    *,
    name: str,
    argv: Sequence[str | Path],
    cwd: Path,
    log_dir: Path,
    env: dict[str, str],
    timeout: float | None = 600.0,
) -> dict[str, Any]:
    child = ManagedProcess.start(
        name=name,
        argv=argv,
        cwd=cwd,
        log_dir=log_dir,
        env=env,
    )
    error: BaseException | None = None
    leaked: list[int] = []
    try:
        result = child.wait(timeout)
        if result != 0:
            stderr = child.stderr_path.read_text(encoding="utf-8", errors="replace")
            raise HarnessError(
                f"{name} exited {result}; see {child.stdout_path} and {child.stderr_path}; "
                f"stderr tail: {stderr[-1000:]}"
            )
    except BaseException as caught:
        error = caught
    finally:
        leaked = child.terminate()
        write_json(child.command_path.with_name(f"{name}.process.json"), child.evidence())
    if leaked:
        message = f"{name} left live process-group members: {leaked}"
        if error is not None:
            raise HarnessError(f"{error}; {message}") from error
        raise HarnessError(message)
    if error is not None:
        raise error
    return child.evidence()


def command_for_probe(
    language: str,
    mode: str,
    args: argparse.Namespace,
    common: Sequence[str] = (),
) -> list[str]:
    if language == "rust":
        return [str(args.rust_probe), mode, *common]
    if language == "cpp":
        return [str(args.cpp_probe), mode, *common]
    if language == "python":
        return [str(args.python), str(PYTHON_PROBE), mode, *common]
    raise HarnessError(f"unknown probe language {language!r}")


def make_run_environment(*, strict_toolchain: bool) -> tuple[dict[str, str], dict[str, str]]:
    environment = os.environ.copy()
    overrides: dict[str, str] = {}
    toolchain_root = repo_path(Path(os.environ.get("RBT_PHASE4_TOOLCHAIN", "target/phase4-toolchain")))
    cpp_prefix = repo_path(Path(os.environ.get("RBT_IOX2_PREFIX", "target/iox2-rbt-0.10.0-cpp-install")))
    toolchain_bin = toolchain_root / "bin"
    cc = environment.get("CC", str(toolchain_bin / "x86_64-conda-linux-gnu-gcc"))
    cxx = environment.get("CXX", str(toolchain_bin / "x86_64-conda-linux-gnu-g++"))
    ar = environment.get("AR", str(toolchain_bin / "x86_64-conda-linux-gnu-ar"))

    if strict_toolchain:
        for compiler in (cc, cxx, ar):
            if not Path(compiler).is_file() or not os.access(compiler, os.X_OK):
                raise HarnessError(f"pinned compiler/tool is missing or not executable: {compiler}")
        path_value = f"{toolchain_bin}{os.pathsep}{environment.get('PATH', '')}"
        overrides.update(PATH=path_value, CC=cc, CXX=cxx, AR=ar)
        overrides["CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER"] = environment.get(
            "CARGO_TARGET_X86_64_UNKNOWN_LINUX_GNU_LINKER", cc
        )
        old_library = environment.get("LIBRARY_PATH", "")
        old_ld_library = environment.get("LD_LIBRARY_PATH", "")
        library_paths = [str(toolchain_root / "lib"), str(cpp_prefix / "lib")]
        overrides["LIBRARY_PATH"] = os.pathsep.join(library_paths + ([old_library] if old_library else []))
        overrides["LD_LIBRARY_PATH"] = os.pathsep.join(
            library_paths + ([old_ld_library] if old_ld_library else [])
        )
    else:
        library_paths = [str(toolchain_root / "lib"), str(cpp_prefix / "lib")]
        existing = [path for path in library_paths if Path(path).is_dir()]
        if existing:
            old = environment.get("LD_LIBRARY_PATH", "")
            overrides["LD_LIBRARY_PATH"] = os.pathsep.join(existing + ([old] if old else []))

    environment.update(overrides)
    return environment, overrides


def python_package_info(args: argparse.Namespace, run_dir: Path, env: dict[str, str]) -> dict[str, Any]:
    code = (
        "import json, rbt, rbt._rbt; "
        "print(json.dumps({'rbt_file': rbt.__file__, 'version': rbt.__version__, "
        "'extension_file': rbt._rbt.__file__}))"
    )
    evidence = run_logged(
        name="python-native-import",
        argv=[args.python, "-c", code],
        cwd=REPO_ROOT,
        log_dir=run_dir,
        env=env,
        timeout=30,
    )
    output = Path(evidence["stdout"]).read_text(encoding="utf-8")
    records = json_objects(output)
    if not records:
        raise HarnessError("Python native import check emitted no provenance JSON")
    info = records[-1]
    if not all(info.get(name) for name in ("rbt_file", "version", "extension_file")):
        raise HarnessError("Python native import provenance is incomplete")
    return info


def build_probes(
    args: argparse.Namespace,
    run_dir: Path,
    env: dict[str, str],
) -> list[dict[str, Any]]:
    toolchain_root = repo_path(Path(os.environ.get("RBT_PHASE4_TOOLCHAIN", "target/phase4-toolchain")))
    cpp_prefix = repo_path(Path(os.environ.get("RBT_IOX2_PREFIX", "target/iox2-rbt-0.10.0-cpp-install")))
    cc = env.get("CC", str(toolchain_root / "bin/x86_64-conda-linux-gnu-gcc"))
    cxx = env.get("CXX", str(toolchain_root / "bin/x86_64-conda-linux-gnu-g++"))
    evidence: list[dict[str, Any]] = []
    evidence.append(
        run_logged(
            name="cpp-prefix-provenance",
            argv=[REPO_ROOT / "scripts/check_iceoryx2_rbt_prefix.sh", cpp_prefix],
            cwd=REPO_ROOT,
            log_dir=run_dir,
            env=env,
        )
    )
    evidence.append(
        run_logged(
            name="rust-probe-build",
            argv=["cargo", "build", "--locked", "-p", "rbt-tf-tools"],
            cwd=REPO_ROOT,
            log_dir=run_dir,
            env=env,
        )
    )
    evidence.append(
        run_logged(
            name="cpp-probe-configure",
            argv=[
                "cmake",
                "-S",
                REPO_ROOT / "sdk/cpp",
                "-B",
                args.cpp_build,
                "-G",
                "Ninja",
                "-DCMAKE_BUILD_TYPE=Debug",
                f"-DCMAKE_C_COMPILER={cc}",
                f"-DCMAKE_CXX_COMPILER={cxx}",
                f"-DCMAKE_PREFIX_PATH={cpp_prefix}",
            ],
            cwd=REPO_ROOT,
            log_dir=run_dir,
            env=env,
        )
    )
    evidence.append(
        run_logged(
            name="cpp-probe-build",
            argv=["cmake", "--build", args.cpp_build, "--target", "tf_probe", "--parallel", "2"],
            cwd=REPO_ROOT,
            log_dir=run_dir,
            env=env,
        )
    )
    for path, label in ((args.rust_probe, "Rust"), (args.cpp_probe, "C++")):
        if not path.is_file() or not os.access(path, os.X_OK):
            raise HarnessError(f"{label} probe was not produced or is not executable: {path}")
    return evidence


def _new_id(used: set[str]) -> str:
    value = str(uuid.uuid4())
    while value in used:
        value = str(uuid.uuid4())
    used.add(value)
    return value


def _interop_args(
    args: argparse.Namespace,
    ids: dict[str, str],
    marker_dir: Path,
) -> list[str]:
    return [
        "--static-id",
        ids["static"],
        "--dynamic-id",
        ids["dynamic"],
        "--snapshot-id",
        ids["snapshot"],
        "--ready",
        str(marker_dir / "publisher-ready"),
        "--peer-ready",
        str(marker_dir / "consumer-ready"),
        "--done",
        str(marker_dir / "consumer-done"),
        "--timeout-sec",
        str(args.timeout_sec),
    ]


def run_semantics(
    args: argparse.Namespace,
    run_dir: Path,
    env: dict[str, str],
    fixture: dict[str, Any],
) -> dict[str, Any]:
    language_results: dict[str, Any] = {}
    for language in LANGUAGES:
        command = command_for_probe(language, "semantics", args)
        evidence = run_logged(
            name=f"semantics-{language}",
            argv=command,
            cwd=REPO_ROOT,
            log_dir=run_dir,
            env=env,
            timeout=args.timeout_sec + 10,
        )
        result = last_result_object(Path(evidence["stdout"]), semantics=True)
        language_results[language] = {
            "probe": evidence,
            "actual": validate_semantics_result(result, language, fixture),
        }
        print(f"RBT_TF_SEMANTICS language={language} status=PASS")
    return language_results


def wait_for_ready(child: ManagedProcess, ready: Path, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if ready.is_file():
            return
        exit_code = child.poll()
        if exit_code is not None:
            raise HarnessError(
                f"publisher exited {exit_code} before readiness marker {ready}; "
                f"see {child.stdout_path} and {child.stderr_path}"
            )
        time.sleep(0.025)
    raise HarnessError(f"publisher readiness timed out waiting for {ready}")


def wait_pair(children: Sequence[ManagedProcess], done: Path, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        statuses = [child.poll() for child in children]
        for child, status in zip(children, statuses):
            if status is not None and status != 0:
                raise HarnessError(
                    f"{child.name} exited {status}; see {child.stdout_path} and {child.stderr_path}"
                )
        consumer = children[1]
        if consumer.poll() == 0 and not done.is_file():
            raise HarnessError(
                f"consumer exited successfully without completion marker {done}; "
                f"see {consumer.stdout_path}"
            )
        if all(status is not None for status in statuses):
            return
        time.sleep(0.025)
    raise HarnessError(f"interop pair timed out after {timeout:g}s; completion marker={done}")


def run_pair(
    args: argparse.Namespace,
    run_dir: Path,
    publisher_language: str,
    consumer_language: str,
    env: dict[str, str],
    used_ids: set[str],
) -> dict[str, Any]:
    pair_name = f"{publisher_language}-to-{consumer_language}"
    pair_dir = run_dir / "pairs" / pair_name
    marker_dir = pair_dir / "metadata"
    marker_dir.mkdir(parents=True, exist_ok=True)
    ready = marker_dir / "publisher-ready"
    peer_ready = marker_dir / "consumer-ready"
    done = marker_dir / "consumer-done"
    ids = {
        "static": _new_id(used_ids),
        "dynamic": _new_id(used_ids),
        "snapshot": _new_id(used_ids),
    }
    if len(set(ids.values())) != 3:
        raise HarnessError("static, dynamic, and snapshot authority UUIDs must be distinct")
    pair_summary: dict[str, Any] = {
        "pair": f"{publisher_language}:{consumer_language}",
        "status": "FAIL",
        "ids": ids,
        "metadata_files": {
            "publisher_ready": str(ready),
            "consumer_ready": str(peer_ready),
            "consumer_done": str(done),
        },
        "publisher_ready_before_consumer_start": False,
        "results": {},
        "processes": {},
    }
    children: list[ManagedProcess] = []
    failure: BaseException | None = None
    cleanup_leaks: dict[str, list[int]] = {}
    try:
        common = _interop_args(args, ids, marker_dir)
        publisher = ManagedProcess.start(
            name="publisher",
            argv=command_for_probe(publisher_language, "interop", args, ["publish", *common]),
            cwd=REPO_ROOT,
            log_dir=pair_dir,
            env=env,
        )
        children.append(publisher)
        wait_for_ready(publisher, ready, args.timeout_sec + 1)
        pair_summary["publisher_ready_before_consumer_start"] = True

        consumer = ManagedProcess.start(
            name="consumer",
            argv=command_for_probe(consumer_language, "interop", args, ["consume", *common]),
            cwd=REPO_ROOT,
            log_dir=pair_dir,
            env=env,
        )
        children.append(consumer)
        wait_pair(children, done, args.timeout_sec + 5)
        if publisher.return_code != 0 or consumer.return_code != 0:
            raise HarnessError(
                f"pair {pair_name} exited publisher={publisher.return_code} "
                f"consumer={consumer.return_code}"
            )
        if not (ready.is_file() and peer_ready.is_file() and done.is_file()):
            missing = [str(path) for path in (ready, peer_ready, done) if not path.is_file()]
            raise HarnessError(f"pair {pair_name} is missing readiness/completion markers: {missing}")

        publisher_actual = last_result_object(publisher.stdout_path, semantics=False)
        pair_summary["results"]["publisher"] = publisher_actual
        validate_interop_result(publisher_actual, publisher_language, "publisher")
        consumer_actual = last_result_object(consumer.stdout_path, semantics=False)
        pair_summary["results"]["consumer"] = consumer_actual
        consumer_result = validate_interop_result(consumer_actual, consumer_language, "consumer")
        pair_summary["status"] = "PASS"
        print(
            f"RBT_TF_INTEROP_PAIR publisher={publisher_language} consumer={consumer_language} "
            f"static_revision={consumer_result['static_revision']} "
            f"dynamic_samples={consumer_result['dynamic_samples']} status=PASS"
        )
    except BaseException as error:
        failure = error
        pair_summary["error"] = f"{type(error).__name__}: {error}"
    finally:
        for child in reversed(children):
            leaked = child.terminate()
            pair_summary["processes"][child.name] = child.evidence()
            if leaked:
                cleanup_leaks[child.name] = leaked
            write_json(child.command_path.with_name(f"{child.name}.process.json"), child.evidence())
        pair_summary["cleanup_live_processes"] = cleanup_leaks
        if cleanup_leaks:
            pair_summary["status"] = "FAIL"
            leak_error = HarnessError(f"pair {pair_name} left live process-group members: {cleanup_leaks}")
            if failure is None:
                failure = leak_error
            else:
                pair_summary["error"] += f"; {leak_error}"
        write_json(pair_dir / "summary.json", pair_summary)

    if failure is not None:
        if isinstance(failure, KeyboardInterrupt):
            raise failure
        raise HarnessError(str(failure)) from failure
    return pair_summary


def parse_pair(value: str) -> tuple[str, str]:
    parts = value.lower().split(":")
    if len(parts) != 2 or parts[0] not in LANGUAGES or parts[1] not in LANGUAGES:
        raise argparse.ArgumentTypeError("pair must be one of rust:cpp, rust:python, cpp:rust, etc.")
    if parts[0] == parts[1]:
        raise argparse.ArgumentTypeError("same-language TF pairs are not part of this matrix")
    return parts[0], parts[1]


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--build-only", action="store_true", help="build probes and verify the installed Python native extension")
    mode.add_argument("--math-only", action="store_true", help="run and compare the three native semantics probes")
    parser.add_argument("--skip-build", action="store_true", help="use the already installed probe binaries and Python extension")
    parser.add_argument(
        "--python",
        type=Path,
        default=Path(os.environ.get("RBT_TF_INTEROP_PYTHON") or "target/python-verify-venv/bin/python"),
        help="Python interpreter with the native rbt extension",
    )
    parser.add_argument("--cpp-build", type=Path, default=Path("target/tf-cpp-debug"))
    parser.add_argument("--rust-probe", type=Path, default=Path("target/debug/rbt-tf-tools"))
    parser.add_argument("--log-dir", type=Path, default=Path("target/tf-interop-logs"))
    parser.add_argument("--timeout-sec", type=int, default=30)
    parser.add_argument("--pair", action="append", type=parse_pair, help="run only this directed pair; may be repeated")
    args = parser.parse_args(argv)
    args.python = repo_path(args.python)
    args.cpp_build = repo_path(args.cpp_build)
    args.rust_probe = repo_path(args.rust_probe)
    args.cpp_probe = args.cpp_build / "tf_probe"
    args.log_dir = repo_path(args.log_dir)
    if args.timeout_sec < 2 or args.timeout_sec > 300:
        parser.error("--timeout-sec must be between 2 and 300")
    if args.skip_build and args.build_only:
        parser.error("--skip-build cannot be combined with --build-only")
    if args.math_only and args.pair:
        parser.error("--pair cannot be combined with --math-only")
    if args.pair and len(args.pair) != len(set(args.pair)):
        parser.error("duplicate --pair value")
    return args


def run(args: argparse.Namespace) -> int:
    run_id = f"tf-interop-{time.strftime('%Y%m%dT%H%M%SZ', time.gmtime())}-{os.getpid()}"
    run_dir = args.log_dir / run_id
    run_dir.mkdir(parents=True, exist_ok=False)
    fixture = read_semantics_fixture()
    summary: dict[str, Any] = {
        "schema": "rbt.tf-interop-run-v1",
        "run_id": run_id,
        "mode": "build-only" if args.build_only else "math-only" if args.math_only else "matrix",
        "status": "RUNNING",
        "run_dir": str(run_dir),
        "timeout_sec": args.timeout_sec,
        "pairs": [],
        "semantics": {},
        "commands": [],
    }
    write_json(run_dir / "summary.json", summary)
    try:
        if not args.python.is_file() or not os.access(args.python, os.X_OK):
            raise HarnessError(f"Python interpreter is missing or not executable: {args.python}")
        env, env_overrides = make_run_environment(strict_toolchain=not args.skip_build)
        summary["environment_overrides"] = env_overrides
        if not args.skip_build:
            summary["commands"] = build_probes(args, run_dir / "build", env)
        for path, label in ((args.rust_probe, "Rust"), (args.cpp_probe, "C++")):
            if not path.is_file() or not os.access(path, os.X_OK):
                raise HarnessError(f"{label} probe is missing or not executable: {path}")
        summary["python_native"] = python_package_info(args, run_dir / "python", env)
        if args.build_only:
            print(f"RBT_TF_INTEROP_BUILD status=PASS run_dir={run_dir}")
        else:
            summary["semantics"] = run_semantics(args, run_dir / "semantics", env, fixture)
            if not args.math_only:
                pairs = args.pair if args.pair else list(DEFAULT_PAIRS)
                used_ids: set[str] = set()
                for publisher, consumer in pairs:
                    try:
                        pair = run_pair(args, run_dir, publisher, consumer, env, used_ids)
                    except BaseException:
                        # The failed pair has already persisted its complete logs and summary.
                        pair_dir = run_dir / "pairs" / f"{publisher}-to-{consumer}"
                        failed_summary = json.loads((pair_dir / "summary.json").read_text(encoding="utf-8"))
                        summary["pairs"].append(failed_summary)
                        raise
                    summary["pairs"].append(pair)
                    write_json(run_dir / "summary.json", summary)
        summary["status"] = "PASS"
        write_json(run_dir / "summary.json", summary)
        print(f"RBT_TF_INTEROP status=PASS summary={run_dir / 'summary.json'}")
        return 0
    except BaseException as error:
        summary["status"] = "FAIL"
        summary["error"] = f"{type(error).__name__}: {error}"
        write_json(run_dir / "summary.json", summary)
        print(f"RBT_TF_INTEROP status=FAIL error={error} summary={run_dir / 'summary.json'}", file=sys.stderr)
        return 130 if isinstance(error, KeyboardInterrupt) else 1


def main(argv: Sequence[str] | None = None) -> int:
    def interrupt(signum: int, _frame: Any) -> None:
        raise KeyboardInterrupt(f"received signal {signum}")

    signal.signal(signal.SIGTERM, interrupt)
    try:
        args = parse_args(argv)
        return run(args)
    except HarnessError as error:
        print(f"RBT_TF_INTEROP status=FAIL error={error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

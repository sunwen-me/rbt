#!/usr/bin/env python3
"""Sequential, isolated REAL_HARDWARE RBT/ROS 2 Gemini335 measurements."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import statistics
import subprocess
import time


ROOT = Path(__file__).resolve().parents[3]
WORK = ROOT / "target/orbbec-perf-20261020"
RBT = ROOT / "target/release-build-distribution-20261020/sdk-root"


def ros_environment(script: Path, discovery: str, transports: str | None) -> dict[str, str]:
    result = subprocess.run(["/bin/bash", "-c", 'source "$1" >&2; env -0', "bash", str(script)],
                            check=True, stdout=subprocess.PIPE)
    env = dict(item.split("=", 1) for item in result.stdout.decode().split("\0") if "=" in item)
    env["RMW_IMPLEMENTATION"] = "rmw_fastrtps_cpp"
    # Unique local domain, no ROS CLI daemon or other application involved.
    env["ROS_DOMAIN_ID"] = "187"
    env["ROS_AUTOMATIC_DISCOVERY_RANGE"] = discovery
    if transports is not None:
        env["FASTDDS_BUILTIN_TRANSPORTS"] = transports
    return env


def proc_stat(pid: int) -> dict[str, float]:
    text = Path(f"/proc/{pid}/stat").read_text()
    fields = text[text.rfind(")") + 2:].split()
    if fields[0] == "Z":
        raise RuntimeError(f"process {pid} became a zombie")
    ticks = int(fields[11]) + int(fields[12])
    pss = None
    for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines():
        if line.startswith("Pss:"):
            pss = int(line.split()[1]) / 1024
    if pss is None:
        raise RuntimeError(f"no PSS for process {pid}")
    return {"cpu_seconds": ticks / os.sysconf("SC_CLK_TCK"), "pss_mib": pss,
            "rss_mib": int(fields[21]) * os.sysconf("SC_PAGE_SIZE") / 1024**2,
            "threads": int(fields[17])}


def stop(process: subprocess.Popen, sig: int = signal.SIGINT) -> int:
    if process.poll() is None:
        os.killpg(process.pid, sig)
        try:
            process.wait(10)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            raise RuntimeError(f"process {process.pid} required SIGKILL")
    return process.returncode


def run_case(framework: str, number: int, args, ros_env) -> dict:
    folder = args.output / f"{number:02d}-{framework}"
    folder.mkdir()
    env = dict(ros_env) if framework == "ros2" else dict(os.environ)
    env["ORBBEC_LOG_DIR"] = str(folder)
    env["ROS_LOG_DIR"] = str(folder / "ros-log")
    env["RBT_BENCH_ROS_QOS"] = args.ros_qos
    handles, processes, commands = [], {}, {}

    def launch(name, command):
        commands[name] = [str(item) for item in command]
        log = (folder / f"{name}.log").open("wb")
        handles.append(log)
        processes[name] = subprocess.Popen(commands[name], env=env, cwd=folder,
                                            stdout=log, stderr=subprocess.STDOUT,
                                            start_new_session=True)
        return processes[name]

    def check_live():
        for name, process in processes.items():
            if process.poll() is not None:
                raise RuntimeError(f"{name} exited early ({process.returncode}); see {folder}")

    result = {"framework": framework, "number": number, "status": "FAIL"}
    try:
        if framework == "rbt":
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                env["RBT_COMPOSER_ADDR"] = f"127.0.0.1:{sock.getsockname()[1]}"
            launch("composer", [RBT / "bin/composer"])
            time.sleep(0.2)
            launch("driver", [ROOT / "target/orbbec-install/bin/rbt-orbbec-camera", "run",
                              "--serial", args.serial, "--width", "640", "--height", "480", "--fps", "30",
                              "--duration-sec", "90"])
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                check_live()
                if '"event":"activation"' in (folder / "driver.log").read_text(errors="replace"):
                    break
                time.sleep(0.05)
            else:
                raise TimeoutError("RBT camera did not activate")
            consumer = WORK / "rbt-consumer-build/ninja/rbt-orbbec-bench-consumer"
        else:
            config = folder / "camera.yaml"
            config.write_text(args.ros_config.read_text())
            launch("driver", [WORK / "ros2-install/orbbec_camera/lib/orbbec_camera/orbbec_camera_node",
                              "--ros-args", "-r", "__ns:=/camera", "-r", "__node:=camera",
                              "--params-file", config, "-p", f"serial_number:={args.serial}"])
            # A fixed warmup below is accepted only when both streams deliver enough frames.
            time.sleep(2)
            check_live()
            consumer = WORK / "ros2-consumer-build/ros2-perf-consumer"

        start = time.monotonic_ns() + int(args.warmup * 1e9)
        end = start + int(args.seconds * 1e9)
        launch("consumer", [consumer, "--start-ns", str(start), "--end-ns", str(end),
                            "--output", folder / "consumer.json"])
        (folder / "commands.json").write_text(json.dumps(commands, indent=2))
        (folder / "runtime-env.json").write_text(json.dumps({
            key: env.get(key) for key in (
                "RMW_IMPLEMENTATION", "ROS_DOMAIN_ID", "ROS_AUTOMATIC_DISCOVERY_RANGE",
                "FASTDDS_BUILTIN_TRANSPORTS", "FASTDDS_DEFAULT_PROFILES_FILE",
                "FASTRTPS_DEFAULT_PROFILES_FILE", "RMW_FASTRTPS_USE_QOS_FROM_XML",
                "RBT_BENCH_ROS_QOS")}, indent=2))
        while time.monotonic_ns() < start:
            check_live()
            time.sleep(0.05)
        before_time = time.monotonic_ns()
        before = {name: proc_stat(process.pid) for name, process in processes.items()}
        samples = []
        # Finish just before subscriber's independent end timer, keeping CPU counters readable.
        while time.monotonic_ns() < end - 30_000_000:
            check_live()
            sample = {"time_ns": time.monotonic_ns(), "loadavg": os.getloadavg(),
                      "processes": {name: proc_stat(process.pid) for name, process in processes.items()}}
            samples.append(sample)
            remaining = (end - 30_000_000 - time.monotonic_ns()) / 1e9
            if remaining > 0:
                time.sleep(min(0.5, remaining))
        after_time = time.monotonic_ns()
        after = {name: proc_stat(process.pid) for name, process in processes.items()}
        elapsed = (after_time - before_time) / 1e9
        cpu = {name: (after[name]["cpu_seconds"] - value["cpu_seconds"]) / elapsed * 100
               for name, value in before.items()}
        processes["consumer"].wait(5)
        if processes["consumer"].returncode != 0:
            raise RuntimeError("consumer failed")
        received = json.loads((folder / "consumer.json").read_text())
        for name, stream in received["streams"].items():
            if stream["count"] == 0 or not stream["ready_before_window"]:
                raise RuntimeError(f"{name} did not deliver frames before and during the measurement window")
        result.update(status="PASS", start_ns=start, end_ns=end, resource_interval_seconds=elapsed,
                      cpu_percent_one_core=cpu, total_cpu_percent_one_core=sum(cpu.values()),
                      pss_mib_mean={name: statistics.mean(s["processes"][name]["pss_mib"] for s in samples)
                                    for name in processes},
                      total_pss_mib_mean=statistics.mean(sum(p["pss_mib"] for p in s["processes"].values())
                                                        for s in samples),
                      total_pss_mib_peak=max(sum(p["pss_mib"] for p in s["processes"].values()) for s in samples),
                      received=received, resource_samples=samples)
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
    finally:
        cleanup = {}
        for name in ("consumer", "driver", "composer"):
            if name in processes:
                try:
                    cleanup[name] = stop(processes[name], signal.SIGTERM if name == "composer" else signal.SIGINT)
                except Exception as error:
                    cleanup[name] = str(error)
                    result["status"] = "FAIL"
        result["exit_codes"] = cleanup
        if cleanup.get("driver") != 0 or cleanup.get("consumer") != 0:
            result["status"] = "FAIL"
            result.setdefault("error", "driver or consumer did not exit cleanly")
        for handle in handles:
            handle.close()
        (folder / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=20, choices=range(5, 61))
    parser.add_argument("--warmup", type=int, default=5, choices=range(3, 16))
    parser.add_argument("--order", default="rbt,ros2,ros2,rbt,rbt,ros2")
    parser.add_argument("--ros-env", type=Path, default=WORK / "ros2-env.sh")
    parser.add_argument("--ros-qos", choices=("default", "sensor"), default="default")
    parser.add_argument("--ros-discovery", choices=("LOCALHOST", "SYSTEM_DEFAULT", "SUBNET"), default="LOCALHOST")
    parser.add_argument("--ros-transports", help="explicit Fast DDS builtin transport configuration")
    parser.add_argument("--ros-config", type=Path, default=Path(__file__).with_name("ros2.yaml"))
    args = parser.parse_args()
    args.output = args.output.absolute()
    args.ros_config = args.ros_config.absolute()
    args.output.mkdir(parents=True, exist_ok=False)
    order = args.order.split(",")
    if not order or any(framework not in ("rbt", "ros2") for framework in order):
        parser.error("--order must contain rbt/ros2")
    env = ros_environment(args.ros_env, args.ros_discovery, args.ros_transports) if "ros2" in order else None
    results = []
    metadata = {"kind": "REAL_HARDWARE", "serial": args.serial, "order": order,
                "seconds": args.seconds, "warmup_seconds": args.warmup,
                "cpu_definition": "100% = one logical core; own driver+consumer+Composer",
                "memory_definition": "sum of proportional resident set sizes (PSS)",
                "latency_boundary": "host-monotonic image packaging start to consumer receive; excludes exposure/USB/SDK queue",
                "same_host_separate_processes": True,
                "ros_driver_timing_instrumented": True,
                "ros_rmw": "rmw_fastrtps_cpp", "ros_domain_id": 187,
                "ros_qos": args.ros_qos,
                "ros_discovery": args.ros_discovery,
                "ros_builtin_transports": env.get("FASTDDS_BUILTIN_TRANSPORTS") if env else None,
                "uname": list(os.uname()), "created_at_unix_ns": time.time_ns(),
                "time_namespace": os.readlink("/proc/self/ns/time"),
                "boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip()}
    binaries = {"rbt_driver": ROOT / "target/orbbec-install/bin/rbt-orbbec-camera",
                "rbt_consumer": WORK / "rbt-consumer-build/ninja/rbt-orbbec-bench-consumer",
                "rbt_sdk": ROOT / "target/orbbec-sdk-2.10.6/lib/libOrbbecSDK.so.2.10.6"}
    if "ros2" in order:
        binaries.update(ros2_driver=WORK / "ros2-install/orbbec_camera/lib/orbbec_camera/orbbec_camera_node",
                        ros2_consumer=WORK / "ros2-consumer-build/ros2-perf-consumer",
                        ros2_sdk=WORK / "ros2-src/orbbec_camera/SDK/x64/lib/libOrbbecSDK.so.2.10.6")
    metadata["sha256"] = {name: hashlib.sha256(path.read_bytes()).hexdigest() for name, path in binaries.items()}
    metadata["cpuinfo"] = Path("/proc/cpuinfo").read_text().split("\n\n", 1)[0]
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    for number, framework in enumerate(order, 1):
        print(json.dumps({"event": "starting", "case": number, "framework": framework}), flush=True)
        result = run_case(framework, number, args, env)
        results.append(result)
        print(json.dumps({"event": "finished", "case": number, "framework": framework,
                          "status": result["status"], "error": result.get("error"),
                          "cpu": result.get("total_cpu_percent_one_core"),
                          "pss_mib": result.get("total_pss_mib_mean")}), flush=True)
        (args.output / "result.json").write_text(json.dumps({"metadata": metadata, "runs": results}, indent=2) + "\n")
        if result["status"] != "PASS":
            return 1
        time.sleep(2)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

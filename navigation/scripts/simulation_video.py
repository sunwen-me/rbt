#!/usr/bin/env python3
"""Record an actual RBT IPC obstacle run and render its sampled telemetry.

Optional visualization dependencies: matplotlib, pillow, imageio-ffmpeg.
Navigation libraries and executables do not depend on these packages.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]


def record(build: Path, output: Path) -> None:
    capture = output / "capture"
    capture.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    compiler_lib = ROOT / "target/phase4-toolchain/lib"
    if compiler_lib.is_dir():
        env["LD_LIBRARY_PATH"] = str(compiler_lib) + ":" + env.get("LD_LIBRARY_PATH", "")
    recorder = None
    with (output / "run.log").open("wb") as log, (output / "recorder.log").open("wb") as rec_log:
        runner = subprocess.Popen([
            sys.executable, str(ROOT / "navigation/scripts/native_integration.py"),
            "--build-dir", str(build), "--scenario", "obstacle",
            "--output-dir", str(output / "ipc"),
        ], stdout=log, stderr=subprocess.STDOUT, cwd=ROOT, env=env)
        try:
            runtime_log = output / "ipc/obstacle/runtime.log"
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                if runner.poll() is not None:
                    raise RuntimeError("IPC run ended before recording; see run.log")
                text = runtime_log.read_text(errors="replace") if runtime_log.exists() else ""
                match = re.search(r"navigation runtime ready: (\S+)", text)
                if match:
                    prefix = match.group(1)
                    recorder = subprocess.Popen([
                        str(build / "rbt_navigation_telemetry_recorder"),
                        "--prefix", prefix, "--cmd-vel-topic", "/" + prefix + "/cmd_vel",
                        "--output", str(capture), "--duration", "35",
                    ], stdout=rec_log, stderr=subprocess.STDOUT, cwd=ROOT, env=env)
                    break
                time.sleep(0.02)
            if recorder is None:
                raise RuntimeError("No runtime readiness within 8 s")
            if runner.wait(timeout=36) != 0:
                raise RuntimeError("Obstacle integration failed; see run.log and ipc logs")
        finally:
            if runner.poll() is None:
                runner.terminate()
                try:
                    runner.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    runner.kill()
                    runner.wait()
            if recorder is not None:
                if recorder.poll() is None:
                    recorder.terminate()
                try:
                    code = recorder.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    recorder.kill()
                    recorder.wait()
                    raise RuntimeError("Recorder did not stop cleanly")
                if code != 0:
                    raise RuntimeError("Recorder failed; see recorder.log")
    results = json.loads((output / "ipc/results.json").read_text())
    if not results["passed"]:
        raise RuntimeError("Refusing to render a failed navigation run")
    print("Actual four-process RBT obstacle run and passive recording: PASS", flush=True)


def render(output: Path, fps: int) -> None:
    import imageio_ffmpeg
    import matplotlib
    matplotlib.use("Agg")
    from matplotlib import font_manager, pyplot as plt
    from matplotlib.colors import ListedColormap
    from matplotlib.lines import Line2D
    from matplotlib.patches import Circle, FancyArrowPatch, Patch
    import numpy as np

    capture = output / "capture"
    data = np.genfromtxt(capture / "telemetry.csv", delimiter=",", names=True)
    path = np.genfromtxt(capture / "path.csv", delimiter=",", names=True)
    grid = json.loads((capture / "map.json").read_text())
    if data.size < 50 or path.size < 2:
        raise RuntimeError("Too few actual samples for a navigation video")
    if not np.all(np.diff(data["relative_stamp_seconds"]) > 0):
        raise RuntimeError("Telemetry time did not increase")
    if int(data["nav_state"][-1]) != 4:
        raise RuntimeError("Recorded navigation did not reach Succeeded")
    if abs(data["linear_x"][-1]) + abs(data["angular_z"][-1]) > 1e-3:
        raise RuntimeError("Recorded base did not stop")
    if abs(grid["origin_yaw"]) > 1e-9:
        raise RuntimeError("This demo renderer requires the recorded axis-aligned map")

    font = Path("/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc")
    if font.exists():
        font_manager.fontManager.addfont(str(font))
        plt.rcParams["font.family"] = font_manager.FontProperties(fname=str(font)).get_name()
    plt.rcParams.update({"axes.unicode_minus": False, "font.size": 11})
    moving = np.flatnonzero(abs(data["linear_x"]) + abs(data["angular_z"]) > .005)
    t0 = max(float(data["relative_stamp_seconds"][0]),
             float(data["relative_stamp_seconds"][moving[0]]) - .3)
    end = float(data["relative_stamp_seconds"][-1])
    times = data["relative_stamp_seconds"] - t0
    duration = end - t0
    frames = int(np.ceil((duration + 1.0) * fps))
    fig = plt.figure(figsize=(12.8, 8), dpi=100, facecolor="#f5f7fb")
    fig.text(.055, .94, "RBT 原生导航仿真", fontsize=25, weight="bold", color="#17243b")
    fig.text(.055, .892, "A* + ESDF  →  RPP + DWPP  →  VelocityGuard  →  模拟底盘",
             fontsize=12, color="#53657f")
    fig.text(.96, .945, "真实程序采样 · 1× 播放", ha="right", fontsize=12, color="#53657f")
    ax = fig.add_axes([.055, .16, .60, .67], facecolor="white")
    cells = np.array(grid["cells"]).reshape(grid["height"], grid["width"])
    occupied = (cells < 0) | (cells >= 50)
    ox, oy, res = grid["origin_x"], grid["origin_y"], grid["resolution"]
    ax.imshow(occupied, origin="lower", interpolation="nearest", zorder=1,
              extent=[ox, ox + grid["width"] * res, oy, oy + grid["height"] * res],
              cmap=ListedColormap(["#f8fafc", "#344155"]), vmin=0, vmax=1)
    ax.set(xlim=(0, 6), ylim=(0, 4), xlabel="x / m", ylabel="y / m", aspect="equal")
    ax.set_xticks(np.arange(0, 6.1, 1)); ax.set_yticks(np.arange(0, 4.1, 1))
    ax.grid(color="#e2e8f0", linewidth=.7, zorder=0)
    for spine in ax.spines.values():
        spine.set_color("#d8e0eb")
    plan_line, = ax.plot([], [], "--", color="#00a793", lw=2, zorder=3)
    trail, = ax.plot([], [], color="#2563eb", lw=2.7, zorder=4)
    ax.plot(1, 1, "o", color="#64748b", ms=5, zorder=3)
    goal = (float(path["x"][-1]), float(path["y"][-1]))
    ax.plot(*goal, marker="*", color="#e45756", ms=17, zorder=5)
    ax.text(.70, .62, "起点 (1, 1)", color="#64748b", fontsize=11)
    ax.text(goal[0] - .45, goal[1] - .38, "目标 (5, 1)", color="#b44342", fontsize=11)
    ax.text(3.10, 1.18, "障碍墙", rotation=90, ha="center", color="white", fontsize=10)
    robot = Circle((1, 1), .18, facecolor="#f6b93b", edgecolor="#a96500", lw=1.8, zorder=7)
    ax.add_patch(robot)
    heading = FancyArrowPatch((1, 1), (1.26, 1), color="#17243b", mutation_scale=13, lw=1.8, zorder=8)
    ax.add_patch(heading)
    ax.legend(handles=[Line2D([], [], color="#00a793", ls="--", lw=2, label="A* 规划路径"),
                       Line2D([], [], color="#2563eb", lw=2.7, label="实际执行轨迹"),
                       Patch(facecolor="#f6b93b", label="机器人 footprint")],
              loc="upper left", framealpha=.97, facecolor="white", edgecolor="#e2e8f0", fontsize=10)
    timer = fig.text(.71, .79, "", fontsize=22, weight="bold", color="#17243b")
    state_text = fig.text(.71, .735, "", fontsize=17, color="#2563eb")
    distance_text = fig.text(.71, .683, "", fontsize=12, color="#53657f")
    fig.text(.71, .631, "物理半径 0.18 m\n规划半径 0.28 m", fontsize=11, color="#53657f", linespacing=1.7)
    fig.text(.71, .565, "模拟底盘执行速度", fontsize=12, weight="bold", color="#17243b")
    speed_axes = [fig.add_axes([.71, .37, .25, .14], facecolor="white"),
                  fig.add_axes([.71, .17, .25, .14], facecolor="white")]
    speed_artists = []
    for a, field, label, color, ylim in zip(speed_axes, ("linear_x", "angular_z"),
          ("v / (m/s)", "ω / (rad/s)"), ("#2563eb", "#8b5cf6"), ((-.05, .7), (-1.7, 1.7))):
        a.plot(times, data[field], color="#dce3ee", lw=1.2)
        line, = a.plot([], [], color=color, lw=1.7)
        dot, = a.plot([], [], "o", color=color, ms=4)
        a.set(xlim=(0, duration), ylim=ylim, ylabel=label, xlabel="t / s")
        a.tick_params(labelsize=9); a.grid(color="#eef2f6", linewidth=.7)
        for spine in a.spines.values(): spine.set_color("#d8e0eb")
        speed_artists.append((line, dot, field))
    fig.text(.055, .055, "二维差速运动学 · 当前原生 IPC 实际运行数据 · 路径和位姿均来自进程消息",
             fontsize=11, color="#53657f")
    states = {-1: "等待状态", 0: "等待目标", 1: "路径规划", 2: "路径跟踪", 3: "重新规划",
              4: "已到达 · 已停车", 5: "已取消", 6: "导航失败"}
    ffmpeg = imageio_ffmpeg.get_ffmpeg_exe()
    video = output / "obstacle.mp4"
    with (output / "encode.log").open("wb") as encode_log:
        encoder = subprocess.Popen([ffmpeg, "-y", "-hide_banner", "-loglevel", "error",
            "-f", "rawvideo", "-pixel_format", "rgb24", "-video_size", "1280x800",
            "-framerate", str(fps), "-i", "pipe:0", "-an", "-c:v", "libx264",
            "-preset", "veryfast", "-crf", "20", "-pix_fmt", "yuv420p", "-movflags", "+faststart",
            str(video)], stdin=subprocess.PIPE, stderr=encode_log)
        try:
            for frame in range(frames):
                stamp = min(t0 + frame / fps, end)
                i = int(np.searchsorted(data["relative_stamp_seconds"], stamp, side="right") - 1)
                i = max(0, min(i, len(data) - 1))
                row = data[i]
                if stamp >= float(path["relative_stamp_seconds"][0]):
                    plan_line.set_data(path["x"], path["y"])
                trail.set_data(data["x"][:i+1], data["y"][:i+1])
                robot.center = (row["x"], row["y"])
                heading.set_positions(robot.center, (row["x"] + .26 * np.cos(row["yaw"]),
                                                       row["y"] + .26 * np.sin(row["yaw"])))
                timer.set_text(f"t = {max(0, stamp-t0):4.1f} s")
                state = int(row["nav_state"])
                state_text.set_text(states[state]); state_text.set_color("#09865a" if state == 4 else "#2563eb")
                distance_text.set_text(f"距目标  {np.hypot(row['x']-goal[0], row['y']-goal[1]):.2f} m")
                for line, dot, field in speed_artists:
                    line.set_data(times[:i+1], data[field][:i+1]); dot.set_data([times[i]], [row[field]])
                fig.canvas.draw()
                if frame in (0, frames//2, frames-1):
                    fig.savefig(output / f"frame-{frame:04d}.png", dpi=100, facecolor=fig.get_facecolor())
                encoder.stdin.write(np.asarray(fig.canvas.buffer_rgba())[:, :, :3].copy().tobytes())
            encoder.stdin.close()
            if encoder.wait(timeout=15) != 0:
                raise RuntimeError("Video encoder failed; see encode.log")
        finally:
            if encoder.poll() is None:
                encoder.kill(); encoder.wait()
            plt.close(fig)
    preview = output / "preview.gif"
    subprocess.run([ffmpeg, "-y", "-hide_banner", "-loglevel", "error", "-i", str(video),
        "-filter_complex", "fps=10,scale=896:-1:flags=lanczos,split[a][b];[a]palettegen[p];[b][p]paletteuse",
        "-loop", "0", str(preview)], check=True)
    (output / "video-metadata.json").write_text(json.dumps({
        "source": "actual native RBT IPC obstacle run", "playback_speed": 1,
        "sample_count": len(data), "path_points": len(path), "frames": frames,
        "fps": fps, "duration_seconds": frames/fps, "position_interpolation": False,
        "final_pose": [float(data[k][-1]) for k in ("x", "y", "yaw")],
        "final_velocity": [float(data[k][-1]) for k in ("linear_x", "angular_z")],
        "navigation_state": int(data["nav_state"][-1]),
    }, indent=2) + "\n")
    print(f"Video: {video}\nPreview: {preview}", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "target/navigation-build")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "output/navigation-simulation")
    parser.add_argument("--render-only", action="store_true")
    parser.add_argument("--fps", type=int, default=24)
    args = parser.parse_args()
    if not 5 <= args.fps <= 60: parser.error("fps must be 5..60")
    output = args.output_dir.resolve(); output.mkdir(parents=True, exist_ok=True)
    if not args.render_only: record(args.build_dir.resolve(), output)
    render(output, args.fps)


if __name__ == "__main__":
    main()

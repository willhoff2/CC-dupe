#!/usr/bin/env python3
"""Measure what a native Zero Hour frame costs windowed versus fullscreen on a Retina Mac.

Each case launches the real binary from a run directory (retail data symlinked in, nothing copied
into the repo), clicks through Single Player -> Skirmish -> Play Game with real OS events (or, with
--scene shellmap, stays on the main menu's animated 3D shell map) and reads the renderer's own
per-frame log (`ZH_RENDER_FRAME_LOG`, written by the Vulkan backend): the interval between
presents, the CPU time spent recording the scene, the acquire wait and the present wait (which is
the GPU finishing the frame plus the presentation blit, because the backend waits the queue idle
there). Alongside the timings it records the sizes the frame was rendered and presented at - the
engine's back buffer in points, the colour target and the swapchain in pixels - so "is the image
aspect-stretched" is answered from numbers rather than from a screenshot. A screenshot per case
goes to --log-dir (outside the repo: it shows retail art) to confirm the skirmish was reached.

The process is killed at the end of each case; its quit path is not what is being measured.

Usage (the run behind ci-baselines/fullscreen-frame-cost-skirmish-macos-arm64.json):
    python3 scripts/macos-fullscreen-scale-probe.py --run-dir ~/devin-work/fs-scale/run \
        --executable zh --log-dir ~/devin-work/fs-scale/logs \
        --out docs/porting/ci-baselines/fullscreen-frame-cost-skirmish-macos-arm64.json

A case is `name|game arguments[|ENV=value ...]`; the environment half is for renderer switches such
as ZH_VULKAN_VALIDATION. With no --case, DEFAULT_CASES are run: the user's two launch lines
(`-win -xres 1024 -yres 768` and fullscreen `-xres 1728 -yres 1117`), a large window as the
pixels-not-mode control, fullscreen at the retail default resolution, and two -noFPSLimit variants
(which a StaticGameLOD preset overrides: see docs/porting/fullscreen-frame-cost.md).
"""

import argparse
import csv
import importlib.util
import json
import os
import platform
import shlex
import signal
import statistics
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

DEFAULT_CASES = [
    "windowed-1024|-win -xres 1024 -yres 768",
    "windowed-1600x1000|-win -xres 1600 -yres 1000",
    "fullscreen-1728|-xres 1728 -yres 1117",
    "fullscreen-default-res|",
    "windowed-1024-uncapped|-win -xres 1024 -yres 768 -noFPSLimit",
    "fullscreen-1728-uncapped|-xres 1728 -yres 1117 -noFPSLimit",
]

# Two sizes disagree about aspect when their ratios differ by more than one pixel's worth.
ASPECT_TOLERANCE = 0.01

# Main menu -> Single Player -> Skirmish -> Play Game, as the centres of the live GameWindows at
# the WND files' 800x600 creation resolution (read with `macos-input-drive.py buttons`). The
# layouts scale linearly with the engine's resolution, so each click is scaled to the case's.
SKIRMISH_ROUTE = [
    ("ButtonSinglePlayer", 644, 134),
    ("ButtonSkirmish", 644, 294),
    ("ButtonStart", 180.5, 530.5),
]
REFERENCE_RESOLUTION = (800, 600)
# GameLogic::m_gameMode once a skirmish is running (see EngineReader.state in macos-input-drive.py).
GAME_SKIRMISH = 2


def load_input_drive():
    """`macos-input-drive.py` owns the window-server query; its name is not importable."""
    spec = importlib.util.spec_from_file_location(
        "macos_input_drive", REPO / "scripts" / "macos-input-drive.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def engine_resolution(arguments):
    """-xres/-yres from a case's arguments; the retail default is 800x600."""
    width, height = REFERENCE_RESOLUTION
    for index, argument in enumerate(arguments[:-1]):
        if argument == "-xres":
            width = int(arguments[index + 1])
        elif argument == "-yres":
            height = int(arguments[index + 1])
    return width, height


def drive_to_skirmish(input_drive, pid, arguments, settle):
    """Real clicks through the menus; in fullscreen this doubles as the points hit-testing check.
    A click that misses leaves the game in the shell, which engine_game_mode() catches."""
    width, height = engine_resolution(arguments)
    for name, reference_x, reference_y in SKIRMISH_ROUTE:
        window = input_drive.game_window(pid)
        input_drive.activate_through_accessibility(pid)
        time.sleep(0.4)
        client_x = reference_x * width / REFERENCE_RESOLUTION[0]
        client_y = reference_y * height / REFERENCE_RESOLUTION[1]
        input_drive.post_click(*input_drive.client_to_global(window, client_x, client_y, height))
        time.sleep(settle)


def engine_game_mode(binary, pid):
    """The running game's mode, read by `macos-input-drive.py snapshot` in one brief LLDB stop
    (the binary must be signed for attach, as the input-drive docs describe)."""
    snapshot = subprocess.run(
        [sys.executable, str(REPO / "scripts" / "macos-input-drive.py"), "snapshot",
         "--pid", str(pid), "--binary", str(binary)],
        capture_output=True, text=True, check=True).stdout
    for line in snapshot.splitlines():
        fields = line.split()
        if len(fields) == 2 and fields[0] == "game_mode":
            return None if fields[1] == "None" else int(fields[1])
    return None


def parse_case(text):
    parts = text.split("|")
    if len(parts) not in (2, 3) or not parts[0]:
        raise SystemExit("--case must be 'name|arguments[|ENV=value ...]', got %r" % text)
    env = {}
    if len(parts) == 3:
        for assignment in shlex.split(parts[2]):
            key, separator, value = assignment.partition("=")
            if not separator:
                raise SystemExit("case %s: %r is not ENV=value" % (parts[0], assignment))
            env[key] = value
    return {"name": parts[0], "arguments": shlex.split(parts[1]), "env": env}


def percentile(values, fraction):
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(round(fraction * (len(ordered) - 1))))
    return ordered[index]


def distribution(values):
    return {
        "median": round(statistics.median(values), 3),
        "p95": round(percentile(values, 0.95), 3),
        "mean": round(statistics.fmean(values), 3),
    }


def read_frame_log(path):
    with open(path, newline="") as handle:
        return list(csv.DictReader(handle))


def aspect(width, height):
    return width / height if height else 0.0


def summarise(rows, window):
    """Timing distributions over the measured rows, and the sizes they were rendered at."""
    last = rows[-1]
    points = (int(last["points_w"]), int(last["points_h"]))
    target = (int(last["target_w"]), int(last["target_h"]))
    swapchain = (int(last["swapchain_w"]), int(last["swapchain_h"]))
    sizes_changed = any(
        (int(row["target_w"]), int(row["target_h"]), int(row["swapchain_w"]),
         int(row["swapchain_h"])) != target + swapchain
        for row in rows)
    intervals = [float(row["interval_ms"]) for row in rows if float(row["interval_ms"]) > 0]
    median_interval = statistics.median(intervals)
    window_size = (window["bounds"]["Width"], window["bounds"]["Height"]) if window else None
    # Present blits the rendered colour target into the swapchain's full extent, and the swapchain
    # is the drawable (the content view at the render scale), so their aspects must agree.
    stretched = abs(aspect(*target) - aspect(*swapchain)) > ASPECT_TOLERANCE
    return {
        "frames": len(rows),
        "fps_from_median_interval": round(1000.0 / median_interval, 2),
        "interval_ms": distribution(intervals),
        "scene_record_ms": distribution([float(row["scene_ms"]) for row in rows]),
        "acquire_ms": distribution([float(row["acquire_ms"]) for row in rows]),
        "present_wait_ms": distribution([float(row["present_ms"]) for row in rows]),
        "engine_back_buffer_points": list(points),
        "colour_target_pixels": list(target),
        "colour_target_megapixels": round(target[0] * target[1] / 1e6, 2),
        "swapchain_pixels": list(swapchain),
        "render_scale": float(last["scale"]),
        "sizes_changed_during_window": sizes_changed,
        "window_bounds_points": (
            [window["bounds"]["X"], window["bounds"]["Y"], *window_size] if window else None),
        "aspect_stretched": stretched,
    }


def run_case(case, args, input_drive, log_dir):
    frame_log = log_dir / ("%s.frames.csv" % case["name"])
    stderr_log = log_dir / ("%s.stderr.log" % case["name"])
    env = dict(os.environ)
    env.update(case["env"])
    env["ZH_RENDER_FRAME_LOG"] = str(frame_log)
    command = [str(args.run_dir / args.executable), "-nologo", *case["arguments"]]
    print("case %s: %s %s" % (case["name"], " ".join(
        "%s=%s" % item for item in case["env"].items()), " ".join(command)), flush=True)
    with open(stderr_log, "w") as stderr:
        process = subprocess.Popen(command, cwd=args.run_dir, env=env,
                                   stdout=subprocess.DEVNULL, stderr=stderr)
    try:
        time.sleep(args.settle)
        if process.poll() is not None:
            raise RuntimeError("case %s: the game exited with %s during settle; see %s" %
                               (case["name"], process.returncode, stderr_log))
        if args.scene == "skirmish":
            drive_to_skirmish(input_drive, process.pid, case["arguments"], args.click_settle)
            time.sleep(args.load_seconds)
            game_mode = engine_game_mode(args.run_dir / args.executable, process.pid)
            if game_mode is None:
                raise RuntimeError("case %s: could not read the game mode under LLDB; the binary "
                                   "needs get-task-allow and debug info matching its build "
                                   "archives" % case["name"])
            if game_mode != GAME_SKIRMISH:
                raise RuntimeError("case %s: the menu clicks did not reach a skirmish (game mode "
                                   "%s, want %d); see %s" % (case["name"], game_mode,
                                                             GAME_SKIRMISH, stderr_log))
        first_row = len(read_frame_log(frame_log))
        window = input_drive.game_window(process.pid)
        time.sleep(args.seconds)
        rows = read_frame_log(frame_log)[first_row:]
        if window is not None:
            input_drive.screenshot(window, str(log_dir / ("%s.png" % case["name"])))
    finally:
        process.send_signal(signal.SIGKILL)
        process.wait()
    if len(rows) < 10:
        raise RuntimeError("case %s: only %d frames in the measured window; see %s" %
                           (case["name"], len(rows), stderr_log))
    result = {"name": case["name"], "arguments": case["arguments"], "env": case["env"]}
    result.update(summarise(rows, window))
    print("  %.1f fps, interval median %.2f ms, present wait median %.2f ms, target %s px, "
          "swapchain %s px, window %s, stretched %s" % (
              result["fps_from_median_interval"], result["interval_ms"]["median"],
              result["present_wait_ms"]["median"], result["colour_target_pixels"],
              result["swapchain_pixels"], result["window_bounds_points"],
              result["aspect_stretched"]), flush=True)
    return result


def machine():
    def run(command):
        return subprocess.run(command, capture_output=True, text=True).stdout.strip()
    return {
        "model": run(["sysctl", "-n", "machdep.cpu.brand_string"]),
        "macos": platform.mac_ver()[0],
        "display": [line.strip() for line in
                    run(["system_profiler", "SPDisplaysDataType"]).splitlines()
                    if "Resolution:" in line],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--run-dir", type=Path, required=True,
                        help="directory of symlinks into the retail install, holding the binary")
    parser.add_argument("--executable", default="zh", help="binary name inside --run-dir")
    parser.add_argument("--case", action="append", default=[],
                        help="name|game arguments[|ENV=value ...]; repeatable")
    parser.add_argument("--settle", type=float, default=30.0,
                        help="seconds from launch before measuring (shell map load)")
    parser.add_argument("--seconds", type=float, default=20.0, help="seconds measured per case")
    parser.add_argument("--scene", choices=("shellmap", "skirmish"), default="skirmish",
                        help="measure at the menu's shell map, or click into a skirmish first")
    parser.add_argument("--click-settle", type=float, default=3.0,
                        help="seconds between menu clicks")
    parser.add_argument("--load-seconds", type=float, default=45.0,
                        help="seconds from Play Game to measuring (skirmish load)")
    parser.add_argument("--log-dir", type=Path, default=None,
                        help="frame logs and stderr (default: beside --run-dir)")
    parser.add_argument("--label", default="", help="free text recorded with the results")
    parser.add_argument("--out", type=Path, help="write the JSON report here")
    args = parser.parse_args()
    if sys.platform != "darwin":
        raise SystemExit("this probe measures the Cocoa/MoltenVK path and needs macOS")

    args.run_dir = args.run_dir.expanduser().resolve()
    binary = args.run_dir / args.executable
    archs = subprocess.run(["lipo", "-archs", str(binary)], capture_output=True,
                           text=True).stdout.strip()
    if archs != "arm64":
        raise SystemExit("%s is %r, not a thin arm64 binary" % (binary, archs))
    # Absolute: the game runs with cwd=--run-dir, so a relative path would name two directories.
    log_dir = args.log_dir or (args.run_dir.parent / "fullscreen-scale-logs")
    log_dir = log_dir.expanduser().resolve()
    log_dir.mkdir(parents=True, exist_ok=True)

    input_drive = load_input_drive()
    cases = [parse_case(text) for text in (args.case or DEFAULT_CASES)]
    report = {
        "what": "Frame cost of the native macOS build per window mode, resolution and render "
                "scale, in the scene named by `scene`. Produced by "
                "scripts/macos-fullscreen-scale-probe.py from the Vulkan backend's "
                "ZH_RENDER_FRAME_LOG; see docs/porting/fullscreen-frame-cost.md.",
        "label": args.label,
        "date_utc": time.strftime("%Y-%m-%d", time.gmtime()),
        "machine": machine(),
        "binary_archs": archs,
        "scene": args.scene,
        "settle_seconds": args.settle,
        "measured_seconds": args.seconds,
        "cases": [],
    }
    for case in cases:
        report["cases"].append(run_case(case, args, input_drive, log_dir))
        if args.out:
            args.out.write_text(json.dumps(report, indent=2) + "\n")
        time.sleep(2.0)
    if not args.out:
        print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Play one recorded replay on one binary, headless or rendered, and keep everything it measured.

    python3 scripts/macos-late-game-replay.py --binary RUN/zh-o2 --run-dir RUN \
        --user-data-src UD --out OUT [--rendered --xres 1728 --yres 1117] \
        [--sample-at-frame 19800 --sample-seconds 10] [--env NAME=VALUE ...] [--timeout 3600]

UD is a user-data directory holding `Command and Conquer Generals Zero Hour Data/Replays/00000000.rep`.
It is copied to OUT/userdata first, so the run never writes the original or the real profile. The
run writes OUT/logic.csv (ZH_LOGIC_FRAME_LOG), and when rendered OUT/engine.csv
(ZH_ENGINE_FRAME_LOG) and OUT/frames.csv (ZH_RENDER_FRAME_LOG); stdout/stderr go to OUT/stdout.log,
`/usr/bin/time -l` to OUT/time.log, and OUT/run.json records how the run ended.

A headless run ends by itself; the binary may then crash in shutdown (the known ObjectPoolClass
SIGSEGV), so the exit code is recorded but the printed lines and logic.csv are the verdict. A
rendered replay returns to the shell when the recording ends, so the run is stopped once logic.csv
has not advanced for --idle-seconds after the last recorded frame. Refuses to start while another
`zh` process runs, or, rendered, while the screen is locked. See docs/porting/late-game-frame-cost.md
section 10.
"""

import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

REPLAY_NAME = "00000000.rep"
USER_DATA_LEAF = "Command and Conquer Generals Zero Hour Data"
GAME_TIME = re.compile(r"Elapsed Time: (\d+):(\d+) Game Time: (\d+):(\d+)/(\d+):(\d+)")


def other_game_processes():
    proc = subprocess.run(["pgrep", "-fl", "(^|/)zh"], capture_output=True, text=True)
    # This script's own command line names the binary, so it matches the pattern too.
    return [line for line in proc.stdout.splitlines()
            if line.strip() and Path(__file__).name not in line]


def screen_locked():
    proc = subprocess.run(["ioreg", "-n", "Root", "-d1", "-a"], capture_output=True, text=True)
    return re.search(r"<key>CGSSessionScreenIsLocked</key>\s*<true/>", proc.stdout) is not None


def last_logic_frame(path):
    """The last complete row's logic frame in a ZH_LOGIC_FRAME_LOG file, or None."""
    try:
        with open(path, "rb") as handle:
            handle.seek(0, os.SEEK_END)
            size = handle.tell()
            handle.seek(max(0, size - 4096))
            tail = handle.read().decode(errors="replace").splitlines()
    except FileNotFoundError:
        return None
    for line in reversed(tail):
        fields = line.split(",")
        if len(fields) == 5 and fields[0].isdigit():
            return int(fields[0])
    return None


def recorded_total_frames(stdout_path):
    """The replay's length in logic frames, from the first `Game Time: a/b` line headless prints."""
    match = GAME_TIME.search(Path(stdout_path).read_text(errors="replace"))
    if match is None:
        return None
    return (int(match.group(5)) * 60 + int(match.group(6))) * 30


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--run-dir", required=True, help="directory holding the retail data")
    parser.add_argument("--user-data-src", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--rendered", action="store_true")
    parser.add_argument("--xres", type=int, default=1728)
    parser.add_argument("--yres", type=int, default=1117)
    parser.add_argument("--last-frame", type=int,
                        help="rendered: the recording's last logic frame (from its headless run)")
    parser.add_argument("--idle-seconds", type=float, default=15.0)
    parser.add_argument("--sample-at-frame", type=int)
    parser.add_argument("--sample-seconds", type=int, default=10)
    parser.add_argument("--env", action="append", default=[], help="extra NAME=VALUE for the game")
    parser.add_argument("--timeout", type=float, default=3600.0)
    args = parser.parse_args()

    if args.rendered and args.last_frame is None:
        parser.error("--rendered needs --last-frame")
    others = other_game_processes()
    if others:
        raise SystemExit(f"refusing to start: another zh process runs: {others}")
    if args.rendered and screen_locked():
        raise SystemExit("refusing to start a rendered run: the screen is locked")

    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    user_data = out / "userdata"
    if user_data.exists():
        shutil.rmtree(user_data)
    shutil.copytree(Path(args.user_data_src), user_data)
    if not (user_data / USER_DATA_LEAF / "Replays" / REPLAY_NAME).is_file():
        raise SystemExit(f"no {REPLAY_NAME} in {args.user_data_src}")

    # A previous run's logs would read as progress before this run truncates them.
    for stale in ("logic.csv", "engine.csv", "frames.csv"):
        (out / stale).unlink(missing_ok=True)

    env = dict(os.environ)
    env["CNC_USER_DATA"] = str(user_data)
    env["ZH_LOGIC_FRAME_LOG"] = str(out / "logic.csv")
    if args.rendered:
        env["ZH_ENGINE_FRAME_LOG"] = str(out / "engine.csv")
        env["ZH_RENDER_FRAME_LOG"] = str(out / "frames.csv")
    for item in args.env:
        name, _, value = item.partition("=")
        env[name] = value

    binary = Path(args.binary).resolve()
    game_args = ["-headless", "-replay", REPLAY_NAME] if not args.rendered else \
        ["-nologo", "-xres", str(args.xres), "-yres", str(args.yres), "-replay", REPLAY_NAME]
    command = ["/usr/bin/time", "-l", "-o", str(out / "time.log"), str(binary), *game_args]
    started = time.time()
    with open(out / "stdout.log", "w") as log:
        proc = subprocess.Popen(command, cwd=args.run_dir, env=env, stdout=log, stderr=subprocess.STDOUT)

    game_pid = None
    sample_taken = None
    ended_by = None
    last_progress = (None, time.time())
    while True:
        if proc.poll() is not None:
            ended_by = "exit"
            break
        if time.time() - started > args.timeout:
            ended_by = "timeout"
            break
        if game_pid is None:
            children = subprocess.run(["pgrep", "-P", str(proc.pid)], capture_output=True, text=True)
            game_pid = int(children.stdout.split()[0]) if children.stdout.split() else None
        frame = last_logic_frame(out / "logic.csv")
        if frame != last_progress[0]:
            last_progress = (frame, time.time())
        if (args.sample_at_frame is not None and sample_taken is None and game_pid is not None
                and frame is not None and frame >= args.sample_at_frame):
            sample_path = out / f"sample-frame{frame}.txt"
            subprocess.run(["sample", str(game_pid), str(args.sample_seconds), "-file", str(sample_path)],
                           capture_output=True)
            sample_taken = {"at_frame": frame, "path": sample_path.name,
                            "end_frame": last_logic_frame(out / "logic.csv")}
        if args.rendered and frame is not None and frame >= args.last_frame \
                and time.time() - last_progress[1] > args.idle_seconds:
            ended_by = "replay-finished"
            break
        time.sleep(0.5)

    if ended_by != "exit" and game_pid is not None:
        os.kill(game_pid, signal.SIGTERM)
        try:
            proc.wait(timeout=15)
        except subprocess.TimeoutExpired:
            os.kill(game_pid, signal.SIGKILL)
            proc.wait()

    stdout_text = (out / "stdout.log").read_text(errors="replace")
    record = {
        "binary": str(binary),
        "arguments": game_args,
        "rendered": args.rendered,
        "extra_env": args.env,
        "ended_by": ended_by,
        "returncode": proc.returncode,
        "wall_seconds": round(time.time() - started, 1),
        "last_logic_frame": last_logic_frame(out / "logic.csv"),
        "recorded_total_frames": recorded_total_frames(out / "stdout.log"),
        "crc_mismatch_lines": [line for line in stdout_text.splitlines() if "CRC Mismatch" in line],
        "elapsed_lines": [line for line in stdout_text.splitlines() if line.startswith("Elapsed Time")],
        "sample": sample_taken,
    }
    (out / "run.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps(record, indent=2))


if __name__ == "__main__":
    sys.exit(main())

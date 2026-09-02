#!/usr/bin/env python3
"""Record and summarise the renderer's per-frame trace across the shell and a mission.

`run` launches the native binary with ZH_RENDER_TRACE set, drives the real shell into the USA
campaign with scripts/macos-input-drive.py (real CGEvent input), lets the mission render, and
stops the process. `summarize` reduces the trace to one row per traced frame -- draws, dynamic
ring DISCARDs, within-frame ring overruns, distinct render targets, viewports and scissors -- and
diffs the resources created between the last shell frame and the first mission frame. It is the
measurement behind docs/porting/mission-frame-corruption.md; nothing in it inspects pixels.

    python3 scripts/mission-frame-trace.py run --run-dir ~/devin-work/asv/run \\
        --out ~/devin-work/mfc/run1
    python3 scripts/mission-frame-trace.py summarize ~/devin-work/mfc/run1/trace.log
"""

import argparse
import collections
import os
import pathlib
import re
import signal
import subprocess
import sys
import time

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
INPUT_DRIVE = REPO_ROOT / "scripts" / "macos-input-drive.py"

# Main Menu -> Single Player -> USA -> Easy, in engine client points at 800x600
# (docs/porting/real-input-menu-drive.md section 3). The same three points reached
# GAME_SINGLE_PLAYER there; `snapshot` afterwards confirms it here.
CAMPAIGN_CLICKS = (("Single Player", "644,134"), ("USA", "644,134"), ("Easy", "644,173"))

FRAME_BEGIN = re.compile(r"^(\d+) frame \d+ begin device=(\d+)x(\d+) points=(\d+)x(\d+)")
FRAME_END = re.compile(
    r"^(\d+) frame \d+ end draws=(\d+) dropped=(\d+) ring_overruns=(\d+) pass_breaks=(\d+) "
    r"ring_discards_total=(\d+) ring_overruns_total=(\d+) clears_deferred_total=(\d+)")
ZFUNC = re.compile(r" zen=(\d) zwrite=(\d) zfunc=(\d+) ")
CLEAR = re.compile(r"^(\d+) clear (color=\d z=\d|deferred_from_before_begin_scene)")
PASS = re.compile(r"^(\d+) pass (begin|end) (\w+)=(\d+) ")
PERDRAW = re.compile(r"^(\d+) perdraw (\d+) changed=(\d+)(?: bbox=(\S+))?")
DRAW = re.compile(
    r"^(\d+) draw (\d+) (\w+) type=(\d+) prims=(\d+) .*?fvf=0x([0-9a-f]+) "
    r".*?vb=(0x[0-9a-f]+|\(nil\)|0) dyn=(\d) region=(\d+) .*?target=(0x[0-9a-f]+) (\d+)x(\d+) "
    r"device=(\d+)x(\d+) vp=(-?\d+),(-?\d+) (\d+)x(\d+) sc=(-?\d+),(-?\d+) (\d+)x(\d+) tex0=(\S+)")
LOCK = re.compile(r"^(\d+) lock (vb|ib)=(\S+) discard region=(\d+)/(\d+) .* overrun=(\d)")
CREATE = re.compile(r"^(\d+) create (texture|surface)=(\S+) (.*)$")
TARGET = re.compile(r"^(\d+) render_target (.*)$")


class Frame:
    def __init__(self, number):
        self.number = number
        self.device = None
        self.points = None
        self.draws = 0
        self.dropped = 0
        self.overruns = 0
        self.discards = 0
        self.dynamic_draws = 0
        self.overrun_locks = []
        self.targets = collections.Counter()
        self.viewports = collections.Counter()
        self.scissors = collections.Counter()
        self.fvfs = collections.Counter()
        self.kinds = collections.Counter()
        self.creates = []
        self.render_targets = []
        self.pass_breaks = 0
        self.clears = []
        self.clears_deferred_total = 0
        # (zenable, zwrite, zfunc) -> draws; D3DCMP_ALWAYS is 8, LESSEQUAL is 4.
        self.depth_modes = collections.Counter()
        self.pass_events = []
        self.per_draw = []
        # Draws recorded before an overrun that read the region the overrun rewrote.
        self.draws_overwritten = 0
        self._region_readers = collections.defaultdict(list)


def parse(path):
    frames = collections.OrderedDict()
    creates_by_frame = collections.OrderedDict()
    for line in pathlib.Path(path).read_text(errors="replace").splitlines():
        match = FRAME_BEGIN.match(line)
        if match:
            frame = Frame(int(match.group(1)))
            frame.device = (int(match.group(2)), int(match.group(3)))
            frame.points = (int(match.group(4)), int(match.group(5)))
            frames[frame.number] = frame
            continue
        number = int(line.split(" ", 1)[0]) if line[:1].isdigit() else None
        frame = frames.get(number)
        match = CREATE.match(line)
        if match:
            creates_by_frame.setdefault(int(match.group(1)), []).append(
                "%s=%s %s" % (match.group(2), match.group(3), match.group(4)))
            if frame is not None:
                frame.creates.append(line.split(" ", 2)[2])
            continue
        if frame is None:
            continue
        match = FRAME_END.match(line)
        if match:
            frame.draws = int(match.group(2))
            frame.dropped = int(match.group(3))
            frame.overruns = int(match.group(4))
            frame.pass_breaks = int(match.group(5))
            frame.clears_deferred_total = int(match.group(8))
            continue
        match = CLEAR.match(line)
        if match:
            frame.clears.append(match.group(2))
            continue
        match = PASS.match(line)
        if match:
            frame.pass_events.append("%s@%s" % (match.group(2), match.group(4)))
            continue
        match = PERDRAW.match(line)
        if match:
            frame.per_draw.append((int(match.group(2)), int(match.group(3)), match.group(4)))
            continue
        match = DRAW.match(line)
        if match:
            (_, index, kind, _ptype, _prims, fvf, vb, dynamic, region, target, tw, th, dw, dh,
             vx, vy, vw, vh, sx, sy, sw, sh, tex0) = match.groups()
            frame.kinds[kind] += 1
            frame.fvfs["0x" + fvf] += 1
            frame.targets["%s %sx%s device %sx%s" % (target, tw, th, dw, dh)] += 1
            frame.viewports["%s,%s %sx%s" % (vx, vy, vw, vh)] += 1
            frame.scissors["%s,%s %sx%s" % (sx, sy, sw, sh)] += 1
            zmatch = ZFUNC.search(line)
            if zmatch:
                frame.depth_modes["zen=%s zwrite=%s zfunc=%s" % zmatch.groups()] += 1
            if dynamic == "1":
                frame.dynamic_draws += 1
                frame._region_readers[(vb, int(region))].append(int(index))
            continue
        match = LOCK.match(line)
        if match:
            _, kind, handle, region, count, overrun = match.groups()
            frame.discards += 1
            if overrun == "1":
                frame.overruns_seen = True
                frame.overrun_locks.append("%s=%s region %s/%s" % (kind, handle, region, count))
                if kind == "vb":
                    readers = frame._region_readers.get((handle, int(region)), [])
                    frame.draws_overwritten += len(readers)
            continue
        match = TARGET.match(line)
        if match:
            frame.render_targets.append(match.group(2))
    return frames, creates_by_frame


def format_counter(counter, limit=4):
    items = counter.most_common(limit)
    text = ", ".join("%s (%d)" % (key, count) for key, count in items)
    if len(counter) > limit:
        text += ", +%d more" % (len(counter) - limit)
    return text


def summarize_missing_textures(game_log, out):
    """Count the engine's missing-texture fallbacks (magenta placeholder) from the game log."""
    if game_log is None or not game_log.exists():
        return
    names = collections.Counter()
    for line in game_log.read_text(errors="replace").splitlines():
        if line.startswith("missing-texture "):
            names[line.split()[1]] += 1
    out.write("\nmissing-texture fallbacks (each is MISSING DATA or a loader defect, not renderer "
              "corruption): %d\n" % sum(names.values()))
    for name, count in names.most_common(12):
        out.write("  %d x %s\n" % (count, name))


def summarize(path, out=sys.stdout, game_log=None):
    frames, creates_by_frame = parse(path)
    if not frames:
        out.write("no traced frames in %s\n" % path)
        return 1
    out.write("| frame | device | draws | dyn draws | DISCARDs | overruns | draws whose region was "
              "rewritten | targets | viewports | scissors |\n")
    out.write("|---|---|---|---|---|---|---|---|---|---|\n")
    for frame in frames.values():
        out.write("| %d | %dx%d | %d | %d | %d | %d | %d | %s | %s | %s |\n" % (
            frame.number, frame.device[0], frame.device[1], frame.draws, frame.dynamic_draws,
            frame.discards, frame.overruns, frame.draws_overwritten,
            format_counter(frame.targets, 2), format_counter(frame.viewports, 3),
            format_counter(frame.scissors, 3)))
    out.write("\nclears and render-pass breaks per traced frame (a Clear issued before "
              "Begin_Scene is recorded at Begin_Scene and counted as deferred; the total is "
              "cumulative since launch):\n")
    for frame in frames.values():
        out.write("  frame %d: clears [%s]; deferred total %d; pass breaks %d [%s]\n" % (
            frame.number, ", ".join(frame.clears), frame.clears_deferred_total,
            frame.pass_breaks, " ".join(frame.pass_events)))
    out.write("\ndepth modes per traced frame (D3DCMP_LESSEQUAL=4, D3DCMP_ALWAYS=8):\n")
    for frame in frames.values():
        out.write("  frame %d: %s\n" % (frame.number, format_counter(frame.depth_modes, 6)))
    for frame in frames.values():
        if frame.per_draw:
            out.write("\nper-draw colour-target deltas, frame %d (draw: pixels changed, bbox):\n"
                      % frame.number)
            for index, changed, bbox in frame.per_draw:
                out.write("  %3d: %8d %s\n" % (index, changed, bbox or ""))
    out.write("\nrender-target switches per traced frame (D3D8 SetRenderTarget):\n")
    for frame in frames.values():
        if frame.render_targets:
            out.write("  frame %d: %s\n" % (frame.number, "; ".join(frame.render_targets)))
    out.write("\nFVFs per traced frame:\n")
    for frame in frames.values():
        out.write("  frame %d: %s (%s)\n" % (
            frame.number, format_counter(frame.fvfs, 8), format_counter(frame.kinds, 3)))
    out.write("\nwithin-frame ring overruns (a DISCARD that rewrote a region an earlier draw of "
              "the same frame reads):\n")
    any_overrun = False
    for frame in frames.values():
        if frame.overrun_locks:
            any_overrun = True
            out.write("  frame %d: %s\n" % (frame.number, "; ".join(frame.overrun_locks)))
    if not any_overrun:
        out.write("  none\n")
    out.write("\nresources created (frame: line); a traced frame's creations are attributed to "
              "it, the rest to the frame counter when they happened:\n")
    for number, lines in creates_by_frame.items():
        for line in lines:
            out.write("  %d: %s\n" % (number, line))
    summarize_missing_textures(game_log, out)
    return 0


def run(args):
    out_dir = pathlib.Path(args.out).expanduser()
    out_dir.mkdir(parents=True, exist_ok=True)
    png_dir = out_dir / "png"
    png_dir.mkdir(exist_ok=True)
    env = dict(os.environ)
    env["ZH_RENDER_TRACE"] = str(out_dir / "trace.log")
    env["ZH_RENDER_TRACE_EVERY"] = str(args.every)
    env["ZH_RENDER_TRACE_PNG"] = str(png_dir)
    env["ZH_RENDER_DRAW_REPORT"] = str(args.every)
    if args.per_draw:
        env["ZH_RENDER_TRACE_PERDRAW"] = "1"
    run_dir = pathlib.Path(args.run_dir).expanduser()
    stderr_path = out_dir / "game.log"
    with open(stderr_path, "w") as stderr:
        process = subprocess.Popen(
            ["./zh", "-win", "-noshellmap", "-nologo", "-xres", "800", "-yres", "600"],
            cwd=str(run_dir), env=env, stdout=stderr, stderr=subprocess.STDOUT)
    print("== pid %d, trace %s" % (process.pid, env["ZH_RENDER_TRACE"]))
    try:
        time.sleep(args.startup)
        drive = [sys.executable, str(INPUT_DRIVE)]
        subprocess.run(drive + ["activate", "--pid", str(process.pid)], check=False)
        time.sleep(args.shell_seconds)
        for label, point in CAMPAIGN_CLICKS:
            print("== click %s at %s" % (label, point))
            subprocess.run(drive + ["post", "--pid", str(process.pid), "--client", point],
                           check=False)
            time.sleep(args.click_settle)
        time.sleep(args.mission_seconds)
        snapshot = subprocess.run(drive + ["snapshot", "--pid", str(process.pid)],
                                  capture_output=True, text=True, check=False)
        (out_dir / "snapshot.txt").write_text(snapshot.stdout + snapshot.stderr)
        print(snapshot.stdout)
    finally:
        process.send_signal(signal.SIGTERM)
        try:
            process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            process.kill()
    return summarize(out_dir / "trace.log", game_log=stderr_path)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = parser.add_subparsers(dest="command", required=True)
    run_parser = sub.add_parser("run", help="launch, drive into the USA campaign, trace")
    run_parser.add_argument("--run-dir", required=True,
                            help="directory holding `zh` and the retail data")
    run_parser.add_argument("--out", required=True)
    run_parser.add_argument("--every", type=int, default=60, help="trace every N-th frame")
    run_parser.add_argument("--startup", type=float, default=25.0)
    run_parser.add_argument("--shell-seconds", type=float, default=8.0)
    run_parser.add_argument("--click-settle", type=float, default=6.0)
    run_parser.add_argument("--mission-seconds", type=float, default=45.0)
    run_parser.add_argument("--per-draw", action="store_true",
                            help="read the colour target back after every draw of a traced "
                                 "frame (slow; splits the render pass at each draw)")
    sum_parser = sub.add_parser("summarize", help="reduce a trace to per-frame rows")
    sum_parser.add_argument("trace")
    args = parser.parse_args()
    if args.command == "run":
        return run(args)
    trace = pathlib.Path(args.trace)
    return summarize(trace, game_log=trace.with_name("game.log"))


if __name__ == "__main__":
    sys.exit(main())

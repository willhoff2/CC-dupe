#!/usr/bin/env python3
"""Sample the live native engine's renderer resource counts, RSS and frame times through GDB.

This is the Linux twin of `scripts/macos-playability-probe.py`'s `run` command, narrowed to the
three quantities `docs/porting/renderer-resource-lifetime.md` is about: the Vulkan backend's
owned-object vectors (`owned_surfaces_`, `owned_textures_`, ...), the process's resident set, and
the engine's own 30-entry instantaneous-FPS ring (`W3DDisplay::updateAverageFPS`), which is the only
per-frame cost the engine records.  Nothing in the engine is modified or instrumented: every number
is read out of a paused process and the process is resumed.

Sampling costs a stop.  The FPS ring is read every `--interval` seconds; with 30 entries and a
30 FPS cap that window is one second, so an interval above ~0.9 s misses frames.  The frame-time
figures are therefore a *sample* of frames, not every frame; the report says how many were seen.
Owned-object counts are read from `std::vector` internals (`_M_finish - _M_start`) because the
element types never instantiated `size()` in the target, exactly as the macOS probe does.

Usage (the process must already be running; `gdb` attaches with ptrace):

    python3 scripts/native-resource-probe.py --pid $(pgrep -f '^./zh ') --minutes 20 \\
        --interval 1.0 --out /tmp/probe.json

Re-executes itself under `gdb -batch -x` when not already inside GDB; all options travel through
the environment because GDB gives a sourced script no argv.
"""

import json
import os
import shutil
import subprocess
import sys
import threading
import time

ENV_PREFIX = "ZH_RESOURCE_PROBE_"

try:
    import gdb  # type: ignore  # noqa: F401  -- only present when sourced by GDB
    INSIDE_GDB = True
except ImportError:
    INSIDE_GDB = False


def parse_outer_args(argv):
    import argparse
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--minutes", type=float, required=True)
    parser.add_argument("--interval", type=float, default=1.0,
                        help="seconds between samples (default 1.0)")
    parser.add_argument("--out", required=True, help="JSON report path")
    parser.add_argument("--gdb", default=shutil.which("gdb") or "gdb")
    return parser.parse_args(argv)


def run_outer(argv):
    args = parse_outer_args(argv)
    env = dict(os.environ)
    env[ENV_PREFIX + "MINUTES"] = repr(args.minutes)
    env[ENV_PREFIX + "INTERVAL"] = repr(args.interval)
    env[ENV_PREFIX + "OUT"] = args.out
    env[ENV_PREFIX + "PID"] = str(args.pid)
    cmd = [args.gdb, "-q", "-batch", "-p", str(args.pid),
           "-ex", "set pagination off", "-ex", "set print object on",
           "-x", os.path.abspath(__file__)]
    return subprocess.call(cmd, env=env)


# ----------------------------------------------------------------------------------------------
# Inside GDB from here on.

def rss_bytes(pid):
    with open("/proc/%d/status" % pid) as status:
        for line in status:
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) * 1024
    return None


class Probe:
    # `Backend` is a `spike::RenderBackend*`; with `set print object on` GDB resolves the dynamic
    # type (`spike::VulkanBackend`) so the private vectors can be reached through the Python API
    # without spelling a cast the expression parser may not accept.
    BACKEND = "TheVulkanRenderBackend.Internals->Backend"
    VECTORS = ("owned_surfaces_", "owned_textures_", "owned_vbs_", "owned_ibs_")
    # Function-scope statics: GDB knows their addresses but not always their types, so both are
    # read through an explicit cast of the address.
    FPS_RING = "*(float(*)[30])&'W3DDisplay::updateAverageFPS()::fpsHistory'"
    FPS_OFFSET = "*(int*)&'W3DDisplay::updateAverageFPS()::historyOffset'"

    def __init__(self, pid):
        self.pid = pid
        self.last_offset = None
        self.frame_fps = []       # every distinct ring entry observed, in order
        self.ring_overruns = 0    # samples where more than 30 frames elapsed
        self.errors = {}

    def integer(self, expression):
        return int(gdb.parse_and_eval(expression))

    def try_integer(self, expression, label):
        try:
            return self.integer(expression)
        except gdb.error as error:
            self.errors.setdefault(label, str(error))
            return None

    def backend(self):
        value = gdb.parse_and_eval(self.BACKEND)
        return value.cast(value.dynamic_type).dereference()

    def vector_size(self, name):
        try:
            impl = self.backend()[name]["_M_impl"]
            element_bytes = impl["_M_start"].type.target().sizeof
            return (int(impl["_M_finish"]) - int(impl["_M_start"])) // element_bytes
        except gdb.error as error:
            self.errors.setdefault(name, str(error))
            return None

    def stats(self):
        """`resource_stats_` fields that exist in this binary (older builds have fewer)."""
        out = {}
        try:
            stats = self.backend()["resource_stats_"]
        except gdb.error as error:
            self.errors.setdefault("resource_stats_", str(error))
            return out
        for field in ("live_textures", "live_surfaces", "textures_created", "textures_destroyed",
                      "surfaces_created", "surfaces_destroyed", "retired_pending"):
            try:
                out[field] = int(stats[field])
            except gdb.error:
                pass
        return out

    def fps_ring(self):
        """Read the ring and append the entries written since the previous sample."""
        try:
            offset = self.integer(self.FPS_OFFSET)
            ring = gdb.parse_and_eval(self.FPS_RING)
            values = [float(ring[i]) for i in range(30)]
        except gdb.error as error:
            self.errors.setdefault("fps_ring", str(error))
            return None
        if self.last_offset is not None:
            advanced = (offset - self.last_offset) % 30
            if advanced == 0 and offset != self.last_offset:
                advanced = 30
            if advanced == 0:
                new = []
            else:
                if advanced == 30:
                    self.ring_overruns += 1
                new = [values[(self.last_offset + i) % 30] for i in range(advanced)]
            self.frame_fps.extend(v for v in new if v > 0)
        self.last_offset = offset
        return {"offset": offset, "ring": [round(v, 2) for v in values]}

    def sample(self):
        report = {"t": time.time(), "rss_bytes": rss_bytes(self.pid)}
        for name in self.VECTORS:
            report[name] = self.vector_size(name)
        report["stats"] = self.stats()
        report["logic_frame"] = self.try_integer(
            "(int)((W3DGameLogic*)TheGameLogic)->m_frame", "logic_frame")
        report["game_mode"] = self.try_integer(
            "(int)((W3DGameLogic*)TheGameLogic)->m_gameMode", "game_mode")
        report["average_fps"] = None
        try:
            report["average_fps"] = round(float(gdb.parse_and_eval(
                "((W3DDisplay*)TheDisplay)->m_averageFPS")), 3)
        except gdb.error as error:
            self.errors.setdefault("average_fps", str(error))
        ring = self.fps_ring()
        if ring is not None:
            report["fps_offset"] = ring["offset"]
        return report


def resume_for(seconds):
    """Let the inferior run for `seconds` of wall clock, then stop it with SIGINT (which GDB
    intercepts as a stop rather than delivering)."""
    pid = gdb.selected_inferior().pid
    timer = threading.Timer(seconds, lambda: os.kill(pid, 2))
    timer.start()
    started = time.monotonic()
    try:
        gdb.execute("continue", to_string=True)
    finally:
        timer.cancel()
    return time.monotonic() - started


def frame_time_summary(fps_values):
    """mean/p50/p99/worst frame time in ms from instantaneous-FPS ring entries."""
    times = sorted(1000.0 / v for v in fps_values if v > 0)
    if not times:
        return None

    def pct(p):
        index = min(len(times) - 1, int(round(p * (len(times) - 1))))
        return times[index]
    return {"frames": len(times), "mean_ms": round(sum(times) / len(times), 3),
            "p50_ms": round(pct(0.50), 3), "p99_ms": round(pct(0.99), 3),
            "worst_ms": round(times[-1], 3)}


def run_inside_gdb():
    minutes = float(os.environ[ENV_PREFIX + "MINUTES"])
    interval = float(os.environ[ENV_PREFIX + "INTERVAL"])
    out = os.environ[ENV_PREFIX + "OUT"]
    pid = int(os.environ[ENV_PREFIX + "PID"])
    probe = Probe(pid)
    samples = []
    report = {"pid": pid, "started": time.time(), "minutes_requested": minutes,
              "interval": interval, "samples": samples}
    deadline = time.monotonic() + minutes * 60.0
    started = time.monotonic()
    window_frames = []   # (elapsed_seconds, fps) so start/end windows can be cut afterwards
    while time.monotonic() < deadline:
        stopped = time.monotonic()
        before = len(probe.frame_fps)
        sample = probe.sample()
        sample["elapsed_s"] = round(time.monotonic() - started, 3)
        sample["stop_cost_s"] = round(time.monotonic() - stopped, 3)
        samples.append(sample)
        window_frames.extend((sample["elapsed_s"], v) for v in probe.frame_fps[before:])
        if len(samples) % 30 == 1:
            sys.stderr.write("[probe] %6.0fs surfaces=%s textures=%s rss=%.1fMB fps=%s\n" % (
                sample["elapsed_s"], sample["owned_surfaces_"], sample["owned_textures_"],
                (sample["rss_bytes"] or 0) / 1e6, sample["average_fps"]))
            sys.stderr.flush()
        resume_for(interval)
    report["finished"] = time.time()
    report["run_seconds"] = round(time.monotonic() - started, 1)
    report["ring_overruns"] = probe.ring_overruns
    report["errors"] = probe.errors
    total = report["run_seconds"]
    window = min(120.0, total / 4.0)
    report["frame_time_start"] = frame_time_summary(
        [v for t, v in window_frames if t <= window])
    report["frame_time_end"] = frame_time_summary(
        [v for t, v in window_frames if t >= total - window])
    report["frame_time_all"] = frame_time_summary([v for _, v in window_frames])
    report["frame_window_seconds"] = window
    with open(out, "w") as handle:
        json.dump(report, handle, indent=1)
    sys.stderr.write("[probe] wrote %s (%d samples, %d frames seen)\n" % (
        out, len(samples), len(window_frames)))
    gdb.execute("detach", to_string=True)


if INSIDE_GDB:
    run_inside_gdb()
elif __name__ == "__main__":
    sys.exit(run_outer(sys.argv[1:]))

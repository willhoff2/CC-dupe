#!/usr/bin/env python3
"""Fold replay runs from macos-late-game-replay.py into the tables and rule outcomes of
docs/porting/late-game-frame-cost.md section 10.

    python3 scripts/macos-late-game-replay-summary.py --headless LABEL=DIR [...] \
        [--rendered LABEL=DIR ...] [--json out.json]

LABEL is `<binary>/<recording>`, e.g. `A/before-2`; repeat a label for repeated runs. The rules
evaluated here are the pre-registered ones of section 10.1, with its operational definitions.
"""

import argparse
import csv
import json
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

FRAMES_PER_MINUTE = 30 * 60
HEADLESS_WINDOW_START_FRAME = 10 * FRAMES_PER_MINUTE
RENDERED_LATE_START_FRAME = 12 * FRAMES_PER_MINUTE
H1_HEADLESS_MAX_RATIO = 0.6
H1_RENDERED_MIN_GAIN_MS = 8.0
HEAVIER_GAME_MIN_RATIO = 2.0


def read_rows(path):
    with open(path, newline="") as handle:
        return list(csv.DictReader(handle))


def percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        return None
    index = min(len(ordered) - 1, max(0, round(fraction * (len(ordered) - 1))))
    return ordered[index]


def mean_or_none(values):
    values = list(values)
    return statistics.fmean(values) if values else None


def logic_rows(run_dir):
    """Rows of logic.csv as numbers, in playback order, one per logic frame after the map loaded.

    The first rows (frame 0) include map load; only the last row per frame number is kept, so a
    frame repeated while time was frozen counts once, with its last timing.
    """
    by_frame = {}
    for row in read_rows(Path(run_dir) / "logic.csv"):
        frame = int(row["frame"])
        by_frame[frame] = {
            "frame": frame,
            "start_ms": float(row["start_ms"]),
            "logic_ms": float(row["logic_ms"]),
            "ai_ms": float(row["ai_ms"]),
            "objects": int(row["objects"]),
        }
    return [by_frame[frame] for frame in sorted(by_frame)]


def peak_rss_mb(run_dir):
    path = Path(run_dir) / "time.log"
    if not path.is_file():
        return None
    match = re.search(r"(\d+)\s+maximum resident set size", path.read_text())
    return round(int(match.group(1)) / 1e6, 1) if match else None


def headless_summary(run_dir):
    record = json.loads((Path(run_dir) / "run.json").read_text())
    rows = logic_rows(run_dir)
    by_frame = {row["frame"]: row for row in rows}
    last = rows[-1]
    window_start = by_frame.get(HEADLESS_WINDOW_START_FRAME)
    window_wall_s = None
    if window_start is not None:
        window_wall_s = (last["start_ms"] + last["logic_ms"] - window_start["start_ms"]) / 1000.0
    minutes = defaultdict(list)
    for row in rows:
        if row["frame"] > 0:
            minutes[row["frame"] // FRAMES_PER_MINUTE].append(row)
    per_minute = {}
    for minute, bucket in sorted(minutes.items()):
        per_minute[minute] = {
            "frames": len(bucket),
            "logic_ms_mean": statistics.fmean(r["logic_ms"] for r in bucket),
            "ai_ms_mean": statistics.fmean(r["ai_ms"] for r in bucket),
            "objects_mean": statistics.fmean(r["objects"] for r in bucket),
        }
    return {
        "run_dir": str(run_dir),
        "last_frame": last["frame"],
        "crc_mismatch_lines": record["crc_mismatch_lines"],
        "elapsed_lines": record["elapsed_lines"],
        "returncode": record["returncode"],
        "window_wall_s": window_wall_s,
        "window_logic_ms_mean": mean_or_none(
            r["logic_ms"] for r in rows if r["frame"] >= HEADLESS_WINDOW_START_FRAME),
        "window_ai_ms_mean": mean_or_none(
            r["ai_ms"] for r in rows if r["frame"] >= HEADLESS_WINDOW_START_FRAME),
        "peak_rss_mb": peak_rss_mb(run_dir),
        "per_minute": per_minute,
    }


def rendered_summary(run_dir, recording_last_frame):
    record = json.loads((Path(run_dir) / "run.json").read_text())
    engine = [{key: float(value) for key, value in row.items()} for row in read_rows(Path(run_dir) / "engine.csv")]
    passes = []
    for current, following in zip(engine, engine[1:]):
        passes.append({
            "logic_frame": int(current["logic_frame"]),
            "start_ms": current["start_ms"],
            "frame_ms": following["start_ms"] - current["start_ms"],
            "client_ms": current["client_ms"],
            "logic_ms": current["logic_ms"],
        })
    late = [p for p in passes if RENDERED_LATE_START_FRAME <= p["logic_frame"] < recording_last_frame]
    frame_ms = [p["frame_ms"] for p in late]
    logic_passes = [p["logic_ms"] for p in late if p["logic_ms"] > 0]
    summary = {
        "run_dir": str(run_dir),
        "crc_mismatch_lines": record["crc_mismatch_lines"],
        "ended_by": record["ended_by"],
        "late_passes": len(late),
        "late_logic_frames": len(logic_passes),
        "frame_ms_median": statistics.median(frame_ms) if frame_ms else None,
        "frame_ms_p95": percentile(frame_ms, 0.95),
        "frame_ms_mean": statistics.fmean(frame_ms) if frame_ms else None,
        "fps": 1000.0 / statistics.fmean(frame_ms) if frame_ms else None,
        "client_ms_median": statistics.median(p["client_ms"] for p in late) if late else None,
        "logic_ms_median": statistics.median(logic_passes) if logic_passes else None,
        "logic_ms_mean": statistics.fmean(logic_passes) if logic_passes else None,
        "logic_frames_per_s": (len(logic_passes) / (sum(frame_ms) / 1000.0)) if frame_ms else None,
    }
    summary.update(backend_late_split(run_dir, engine, late))
    return summary


def backend_late_split(run_dir, engine, late):
    """Medians of the backend frame log over the late window.

    The two logs share no clock origin, so they are aligned at their ends: both flush every row
    and stop when the game is stopped, so the last present is ~ the last engine pass.
    """
    path = Path(run_dir) / "frames.csv"
    if not path.is_file() or not late:
        return {}
    frames = read_rows(path)
    cumulative = 0.0
    timeline = []
    for row in frames:
        cumulative += float(row["interval_ms"])
        timeline.append((cumulative, row))
    offset = engine[-1]["start_ms"] - timeline[-1][0]
    window_start, window_end = late[0]["start_ms"], late[-1]["start_ms"]
    rows = [row for at, row in timeline if window_start <= at + offset <= window_end]
    if not rows:
        return {}
    return {
        "backend_rows": len(rows),
        "scene_ms_median": statistics.median(float(r["scene_ms"]) for r in rows),
        "present_ms_median": statistics.median(float(r["present_ms"]) for r in rows),
        "backend_interval_ms_median": statistics.median(float(r["interval_ms"]) for r in rows),
    }


def fmt(value):
    return "-" if value is None else f"{value:.2f}"


def parse_labelled(items):
    runs = defaultdict(list)
    for item in items:
        label, _, directory = item.partition("=")
        binary, _, recording = label.partition("/")
        runs[(binary, recording)].append(directory)
    return runs


def evaluate_rules(headless, rendered, candidate="B"):
    """The section 10.1 rules with `candidate` in the role of B (the pre-registered rules use "B")."""
    outcomes = {}
    recordings = sorted({recording for _, recording in headless})

    # The recordings' headers carry no frame count (the probe killed the recording game), so a run
    # has played to the end when it reaches the last frame the -O0 runs of that recording reach.
    reference_last = {recording: max(run["last_frame"] for run in headless.get(("A", recording), [])
                                     or [{"last_frame": 0}]) for recording in recordings}
    validity = {}
    for (binary, recording), runs in headless.items():
        validity[f"{binary}/{recording}"] = [
            not run["crc_mismatch_lines"] and run["last_frame"] >= reference_last[recording] - 100
            for run in runs]
    outcomes["played_in_sync_to_end"] = validity
    valid = all(all(flags) for key, flags in validity.items() if key.startswith(candidate + "/"))
    outcomes["B_valid"] = valid

    headless_ratios = {}
    for recording in recordings:
        a_runs = headless.get(("A", recording), [])
        b_runs = headless.get((candidate, recording), [])
        if a_runs and b_runs and valid:
            a_mean = statistics.fmean(r["window_wall_s"] for r in a_runs)
            b_mean = statistics.fmean(r["window_wall_s"] for r in b_runs)
            headless_ratios[recording] = {"A_s": a_mean, "B_s": b_mean, "ratio": b_mean / a_mean}
    outcomes["H1_headless"] = headless_ratios
    headless_pass = bool(headless_ratios) and len(headless_ratios) == len(recordings) and all(
        value["ratio"] <= H1_HEADLESS_MAX_RATIO for value in headless_ratios.values())

    rendered_gain = {}
    for recording in sorted({recording for _, recording in rendered}):
        a_runs = rendered.get(("A", recording), [])
        b_runs = rendered.get((candidate, recording), [])
        if a_runs and b_runs:
            a_median = statistics.fmean(r["frame_ms_median"] for r in a_runs)
            b_median = statistics.fmean(r["frame_ms_median"] for r in b_runs)
            rendered_gain[recording] = {"A_ms": a_median, "B_ms": b_median, "gain_ms": a_median - b_median}
    outcomes["H1_rendered"] = rendered_gain
    rendered_pass = bool(rendered_gain) and all(
        value["gain_ms"] >= H1_RENDERED_MIN_GAIN_MS for value in rendered_gain.values())
    if not valid:
        outcomes["H1"] = "B INVALID (desync or incomplete playback)"
    elif not rendered_gain:
        outcomes["H1"] = ("headless part " + ("met" if headless_pass else "NOT met")
                          + "; rendered part not measured")
    else:
        outcomes["H1"] = "CONFIRMED" if headless_pass and rendered_pass else "NOT CONFIRMED"

    a_runs = {recording: headless.get(("A", recording), []) for recording in recordings}
    if len(recordings) == 2 and all(a_runs.values()):
        last_common = min(run["last_frame"] for runs in a_runs.values() for run in runs)
        means = {}
        for recording, runs in a_runs.items():
            values = []
            for run in runs:
                for minute, bucket in run["per_minute"].items():
                    if 10 <= int(minute) and (int(minute) + 1) * FRAMES_PER_MINUTE <= last_common:
                        values.extend([bucket["logic_ms_mean"]] * bucket["frames"])
            means[recording] = statistics.fmean(values) if values else None
        ratio = max(means.values()) / min(means.values())
        outcomes["heavier_game"] = {
            "common_window_minutes": [10, last_common // FRAMES_PER_MINUTE],
            "A_logic_ms_mean": means,
            "ratio": ratio,
            "verdict": "CONFIRMED" if ratio >= HEAVIER_GAME_MIN_RATIO else "NOT CONFIRMED",
        }
    return outcomes


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--headless", action="append", default=[])
    parser.add_argument("--rendered", action="append", default=[])
    parser.add_argument("--last-frame", action="append", default=[],
                        help="RECORDING=N: the recording's last logic frame, bounding the rendered late window")
    parser.add_argument("--json", dest="json_out")
    args = parser.parse_args()

    headless = {key: [headless_summary(d) for d in dirs] for key, dirs in parse_labelled(args.headless).items()}
    last_frames = dict(item.split("=") for item in args.last_frame)
    rendered = {key: [rendered_summary(d, int(last_frames[key[1]])) for d in dirs]
                for key, dirs in parse_labelled(args.rendered).items()}

    print("## headless")
    print("| binary | recording | last frame | CRC mismatch | wall s, min 10-end | logic ms/frame | AI ms/frame | peak RSS MB |")
    print("|---|---|---:|---|---:|---:|---:|---:|")
    for (binary, recording), runs in sorted(headless.items()):
        for run in runs:
            wall = f"{run['window_wall_s']:.1f}" if run["window_wall_s"] is not None else "-"
            print(f"| {binary} | {recording} | {run['last_frame']} | "
                  f"{'; '.join(run['crc_mismatch_lines']) or 'none'} | {wall} | "
                  f"{fmt(run['window_logic_ms_mean'])} | {fmt(run['window_ai_ms_mean'])} | {run['peak_rss_mb']} |")
    if rendered:
        print("\n## rendered, late window (logic frame >= 21600)")
        print("| binary | recording | passes | median ms | p95 ms | fps | client ms med | logic ms med | logic/s | scene med | present med | CRC |")
        print("|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|")
        for (binary, recording), runs in sorted(rendered.items()):
            for run in runs:
                print(f"| {binary} | {recording} | {run['late_passes']} | {run['frame_ms_median']:.1f} | "
                      f"{run['frame_ms_p95']:.1f} | {run['fps']:.1f} | {run['client_ms_median']:.1f} | "
                      f"{run['logic_ms_median']:.1f} | {run['logic_frames_per_s']:.1f} | "
                      f"{run.get('scene_ms_median', float('nan')):.1f} | "
                      f"{run.get('present_ms_median', float('nan')):.1f} | {len(run['crc_mismatch_lines'])} |")
    outcomes = {"pre-registered (B)": evaluate_rules(headless, rendered, "B")}
    candidates = sorted({binary for binary, _ in headless} - {"A", "B"})
    for candidate in candidates:
        outcomes[f"post hoc, same thresholds ({candidate} as B)"] = evaluate_rules(headless, rendered, candidate)
    print("\n## rules")
    print(json.dumps(outcomes, indent=2))
    if args.json_out:
        Path(args.json_out).write_text(json.dumps(
            {"headless": {f"{b}/{r}": v for (b, r), v in headless.items()},
             "rendered": {f"{b}/{r}": v for (b, r), v in rendered.items()},
             "rules": outcomes}, indent=2) + "\n")


if __name__ == "__main__":
    sys.exit(main())

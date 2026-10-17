#!/usr/bin/env python3
"""Attribute late-game frame time in heavy-skirmish probe runs, from data already recorded.

    python3 scripts/macos-late-game-attribution.py <run dir> [<run dir> ...]

Each argument is a run directory written by `probe/heavy-skirmish`'s macos-heavy-skirmish-probe.py
(`run.json`, `run-logs/round*.sample.txt`). Per window it prints the object count, the logic frames
advanced per wall second (game speed) next to the rendered fps, and the median frame split into
renderer and outside-renderer time. Per `sample` profile it converts main-thread shares into
milliseconds per frame (share x the adjacent windows' mean interval) for logic, pathfinding and the
renderer, so binaries can be compared in time rather than in shares of different frame lengths.
See docs/porting/late-game-frame-cost.md section 3.
"""

import argparse
import json
import re
import sys
from pathlib import Path

LINE = re.compile(r"^(?P<prefix>[ +!:|]*)(?P<count>\d+) (?P<name>.+?)  \(in ")

# Inclusive main-thread functions reported per profile, in milliseconds per frame.
PROFILE_FUNCTIONS = {
    "logic": "GameLogic::update()",
    "pathfind": "Pathfinder::processPathfindQueue()",
    "client": "W3DGameClient::update()",
    "draw": "W3DDisplay::draw()",
    "text": "Render2DSentenceClass::Build_Textures()",
    "flush": "spike::VulkanBackend::Flush_Frame_Commands(bool)",
    "one_shot": "spike::VulkanBackend::End_One_Shot(VkCommandBuffer_T*)",
    "present": "spike::VulkanBackend::Present()",
    "pacer": "FramePacer::update()",
}


def main_thread_nodes(path):
    """(depth, count, name) for the first thread of a `sample` call graph."""
    nodes = []
    in_graph = False
    for line in Path(path).read_text(errors="replace").splitlines():
        if line.startswith("Call graph:"):
            in_graph = True
            continue
        if not in_graph:
            continue
        if not line.strip():
            break
        if nodes and re.match(r"^    \d+ Thread_", line):
            break
        match = LINE.match(line)
        if match:
            depth = len(match.group("prefix"))
            nodes.append((depth, int(match.group("count")), match.group("name")))
    return nodes


def inclusive_counts(nodes):
    """Samples inside each function, counted once at its outermost occurrence on a stack."""
    counts = {}
    stack = []
    for depth, count, name in nodes:
        while stack and stack[-1][0] >= depth:
            stack.pop()
        if all(frame_name != name for _, frame_name in stack):
            counts[name] = counts.get(name, 0) + count
        stack.append((depth, name))
    return counts


def window_rows(run):
    """One row per probe window, with the logic frame advance taken from the LLDB state reads."""
    rows = []
    previous_end_frame = None
    for round_ in run["rounds"]:
        stats = round_.get("frames_since_last_round") or {}
        start_state = (round_.get("escape") or {}).get("before") or {}
        end_state = round_.get("state_end") or {}
        logic_frames = None
        if previous_end_frame is not None and start_state.get("frame") is not None:
            logic_frames = start_state["frame"] - previous_end_frame
        previous_end_frame = end_state.get("frame")
        if not stats.get("frames"):
            continue
        seconds = stats["seconds"]
        rows.append({
            "round": round_["index"],
            "game_minute": (start_state.get("frame") or 0) / 30 / 60,
            "objects": start_state.get("object_count"),
            "fps": stats["fps_effective"],
            "logic_per_s": logic_frames / seconds if logic_frames and seconds else None,
            "median_ms": stats["interval_median_ms"],
            "mean_ms": 1000.0 / stats["fps_effective"] if stats["fps_effective"] else None,
            "renderer_median_ms": stats["renderer_median_ms"],
            "outside_median_ms": stats["outside_scene_median_ms"],
        })
    return rows


def profile_rows(run_dir, windows):
    rows = []
    by_round = {window["round"]: window for window in windows}
    for sample_path in sorted((run_dir / "run-logs").glob("round*.sample.txt")):
        index = int(re.search(r"round(\d+)", sample_path.name).group(1))
        nodes = main_thread_nodes(sample_path)
        if not nodes:
            continue
        total = nodes[0][1]
        counts = inclusive_counts(nodes)
        # The profile is taken at round `index`: the window before it and the one after bracket it.
        adjacent = [by_round[i]["mean_ms"] for i in (index, index + 1)
                    if i in by_round and by_round[i]["mean_ms"]]
        mean_ms = sum(adjacent) / len(adjacent) if adjacent else None
        shares = {key: counts.get(name, 0) / total for key, name in PROFILE_FUNCTIONS.items()}
        rows.append({"round": index, "samples": total, "mean_frame_ms": mean_ms, "shares": shares,
                     "objects": by_round.get(index + 1, {}).get("objects")})
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("run_dirs", nargs="+", type=Path)
    parser.add_argument("--json", type=Path, help="also write the rows as JSON")
    args = parser.parse_args()
    report = {}
    for run_dir in args.run_dirs:
        run = json.loads((run_dir / "run.json").read_text())
        windows = window_rows(run)
        profiles = profile_rows(run_dir, windows)
        report[run_dir.name] = {"windows": windows, "profiles": profiles}
        print(f"== {run_dir.name}")
        print("  round  game_min  objects   fps  logic/s  median  renderer  outside")
        for window in windows:
            logic_rate = f"{window['logic_per_s']:7.1f}" if window["logic_per_s"] else "      -"
            objects = window["objects"] if window["objects"] is not None else "-"
            print(f"  {window['round']:5d}  {window['game_minute']:8.1f}  {objects!s:>7}  "
                  f"{window['fps']:4.1f}  "
                  f"{logic_rate}  {window['median_ms']:6.1f}  {window['renderer_median_ms']:8.1f}  "
                  f"{window['outside_median_ms']:7.1f}")
        for profile in profiles:
            mean_ms = profile["mean_frame_ms"]
            parts = []
            for key, share in profile["shares"].items():
                in_ms = f"{share * mean_ms:.1f}ms" if mean_ms else "?"
                parts.append(f"{key} {share:.0%}={in_ms}")
            heading = f"  profile round {profile['round']} (mean frame {mean_ms or 0:.1f} ms): "
            print(heading + ", ".join(parts))
    if args.json:
        args.json.write_text(json.dumps(report, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())

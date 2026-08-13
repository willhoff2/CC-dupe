#!/usr/bin/env python3
"""Gate the Vulkan backend's D3D8 coverage against the checked-in baseline.

`spikes/renderer/tools/backend-coverage-scan.py` measures, from source, what the engine
asks of D3D8 and what `spikes/renderer/` serves. This turns that measurement into a
ratchet, the same shape as `check-d3d8-surface.py`:

  * a method or state that was implemented and no longer is fails the gate (regression);
  * a method or state the engine newly reaches and the backend does not serve fails the
    gate (the scoreboard cannot silently go stale);
  * coverage that *improved* also fails, with the instruction to re-run with `--update`,
    so the committed figure is always the figure the tree produces.

Usage:
    check-backend-coverage.py [--update] [--sites-json J] [--states-json J]
"""

import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SCAN = os.path.join(ROOT, "spikes", "renderer", "tools", "backend-coverage-scan.py")
BASELINE = os.path.join(ROOT, "spikes", "renderer", "tools", "backend-coverage-baseline.json")

# The axes the gate compares, and the per-item field that has to match.
AXES = ["methods", "render_states", "stage_states", "cascade_ops", "primitive_types"]


def measure(sites_json, states_json):
    out = os.path.join(ROOT, ".backend-coverage.json")
    command = [sys.executable, SCAN, "--quiet", "--json", out]
    if sites_json:
        command += ["--sites-json", sites_json]
    if states_json:
        command += ["--states-json", states_json]
    proc = subprocess.run(command, cwd=ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout + proc.stderr)
        raise SystemExit("backend-coverage-scan.py failed")
    with open(out) as fh:
        payload = json.load(fh)
    os.remove(out)
    return payload


def statuses(payload):
    return {axis: {name: item["status"] for name, item in payload[axis].items()}
            for axis in AXES}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--update", action="store_true",
                    help="rewrite the baseline from the current tree")
    ap.add_argument("--sites-json")
    ap.add_argument("--states-json")
    args = ap.parse_args()

    payload = measure(args.sites_json, args.states_json)
    if args.update:
        with open(BASELINE, "w") as fh:
            json.dump(payload, fh, indent=1, sort_keys=True)
            fh.write("\n")
        print(f"wrote {os.path.relpath(BASELINE, ROOT)}")
        for key, value in sorted(payload["totals"].items()):
            print(f"  {key:32s} {value}")
        return 0

    with open(BASELINE) as fh:
        baseline = json.load(fh)

    # "Served" is what the gate protects: implemented, or deliberately not needed
    # (a platform/device-model method), or accepted-and-ignored with a stated reason.
    served = {"implemented", "ignored", "none-needed"}
    now, before = statuses(payload), statuses(baseline)
    regressions, improvements, additions = [], [], []
    for axis in AXES:
        for name, status in sorted(now[axis].items()):
            was = before[axis].get(name)
            if was is None:
                additions.append(f"{axis}/{name}: newly reached by the engine, "
                                 f"backend status {status}")
            elif was in served and status not in served:
                regressions.append(f"{axis}/{name}: was {was}, now {status}")
            elif was != status:
                improvements.append(f"{axis}/{name}: was {was}, now {status}")
        for name, status in sorted(before[axis].items()):
            if name not in now[axis]:
                improvements.append(f"{axis}/{name}: no longer reached by the engine")

    for key, value in sorted(payload["totals"].items()):
        print(f"{key:32s} {value}")

    if regressions:
        print("\nREGRESSION: the backend no longer serves what it used to:")
        for line in regressions:
            print(f"  {line}")
    if additions:
        print("\nNEW: the engine reaches something the baseline does not record:")
        for line in additions:
            print(f"  {line}")
    if improvements:
        print("\nDRIFT: coverage changed; re-run with --update and commit the baseline:")
        for line in improvements:
            print(f"  {line}")
    if regressions or additions or improvements:
        return 1
    print("\nOK: backend coverage matches the committed baseline exactly")
    return 0


if __name__ == "__main__":
    sys.exit(main())

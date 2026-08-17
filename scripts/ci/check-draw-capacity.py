#!/usr/bin/env python3
"""Assert the backend still carries the committed number of draws in one frame, per draw.

A mission frame issues thousands of draws and the backend gives each one its own descriptor
set and uniform-buffer slice, so "draws per frame" is a resource limit, not a speed figure.
`zh-draw-capacity` renders one tile per draw with that draw's own texture and its own
D3DRS_TEXTUREFACTOR alpha and reads the target back, so each failure mode has its own number:
a tile still the clear colour is a *dropped* draw, a tile carrying another draw's id is an
*aliased* descriptor set, and a tile whose alpha disagrees with its rgb is an aliased uniform
slice (docs/porting/draws-per-frame.md).

The floor lives in docs/porting/ci-baselines/draw-capacity.json so that lowering it is a
reviewable diff rather than an argument change. Regenerate it with --update; never hand-edit.

`--self-check` re-runs the workload with ZH_RENDER_MAX_DRAWS set to the old fixed
preallocation of 64 and *requires that it fail with dropped draws*, so a green run proves the
gate can still catch the defect it was written for instead of only that it passes.

    python3 scripts/ci/check-draw-capacity.py --binary build/spike/zh-draw-capacity --self-check
"""
import argparse
import json
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import vulkan_manifests  # noqa: E402

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
BASELINE_PATH = REPO_ROOT / "docs" / "porting" / "ci-baselines" / "draw-capacity.json"

# The fixed preallocation the growable allocator replaced. The self-check reproduces it.
OLD_FIXED_CAP = 64

TILE_FIELDS = {
    "correct": re.compile(r"^  tiles correct\s+(\d+)$", re.MULTILINE),
    "dropped": re.compile(r"^  tiles dropped\s+(\d+)$", re.MULTILINE),
    "aliased": re.compile(r"^  tiles aliased\s+(\d+)$", re.MULTILINE),
    "uniform_aliased": re.compile(r"^  tiles uniform-aliased\s+(\d+)$", re.MULTILINE),
    "mismatched": re.compile(r"^  tiles mismatched\s+(\d+)$", re.MULTILINE),
}
BACKEND_FIELDS = {
    "draws_requested": re.compile(r"^backend draws requested\s+(\d+)$", re.MULTILINE),
    "draws_issued": re.compile(r"^backend draws issued\s+(\d+)$", re.MULTILINE),
    "draws_dropped": re.compile(r"^backend draws dropped\s+(\d+)$", re.MULTILINE),
    "peak_draws_per_frame": re.compile(r"^backend peak draws\s+(\d+)$", re.MULTILINE),
}
DESCRIPTORS = re.compile(r"^descriptor sets\s+(\d+) in (\d+) block\(s\)$", re.MULTILINE)
VALIDATION_MESSAGES = re.compile(r"^validation messages: (\d+)$", re.MULTILINE)
FRAME_HEADER = re.compile(r"^frame (\d+)$", re.MULTILINE)


def run_workload(binary, draws, frames, limit=None):
    """-> the parsed output of one workload run, with the validation layer required."""
    env = vulkan_manifests.child_environment()
    if limit is not None:
        env["ZH_RENDER_MAX_DRAWS"] = str(limit)
    proc = subprocess.run([str(binary), "--draws", str(draws), "--frames", str(frames),
                           "--validation"], capture_output=True, text=True, env=env)
    text = proc.stdout + proc.stderr
    result = {
        "returncode": proc.returncode,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
        # The layer status is printed by the backend; on macOS the loader drops the layer
        # silently, so "validation messages: 0" from a run without it says nothing.
        "validation_loaded": "validation layer: loaded" in text,
        # Every frame is classified, so a run whose first frame is clean and whose tenth is
        # not cannot hide behind a single summary.
        "frames_reported": len(FRAME_HEADER.findall(proc.stdout)) or 1,
    }
    for name, pattern in list(TILE_FIELDS.items()) + list(BACKEND_FIELDS.items()):
        found = pattern.findall(proc.stdout)
        result[name] = [int(value) for value in found] if name in TILE_FIELDS else (
            int(found[0]) if found else None)
    descriptors = DESCRIPTORS.search(proc.stdout)
    result["descriptor_capacity"] = int(descriptors.group(1)) if descriptors else None
    result["descriptor_blocks"] = int(descriptors.group(2)) if descriptors else None
    messages = VALIDATION_MESSAGES.search(proc.stdout)
    result["validation_messages"] = int(messages.group(1)) if messages else None
    return result


def check_run(result, draws, frames, failures):
    """Every per-draw and per-frame number the run has to show for `draws` to be believed."""
    if result["returncode"] != 0:
        failures.append(f"the workload exited {result['returncode']}")
    if not result["validation_loaded"]:
        failures.append("the validation layer was not loaded, so this run says nothing about "
                        "validation cleanliness -- see scripts/ci/vulkan_manifests.py")
    if result["validation_messages"] not in (0, None):
        failures.append(f"{result['validation_messages']} validation message(s)")
    if result["validation_messages"] is None:
        failures.append("the workload printed no validation message count")

    if len(result["correct"]) != frames:
        failures.append(f"{len(result['correct'])} frame(s) classified, expected {frames}: the "
                        f"workload was cut down, not passed")
    for index in range(len(result["correct"])):
        if result["correct"][index] != draws:
            failures.append(
                f"frame {index}: {result['correct'][index]} of {draws} tiles correct "
                f"({result['dropped'][index]} dropped, {result['aliased'][index]} aliased, "
                f"{result['uniform_aliased'][index]} uniform-aliased, "
                f"{result['mismatched'][index]} mismatched)")

    if result["draws_requested"] != draws:
        failures.append(f"the backend saw {result['draws_requested']} draws requested, expected "
                        f"{draws}")
    if result["draws_issued"] != draws:
        failures.append(f"the backend issued {result['draws_issued']} of {draws} draws")
    if result["draws_dropped"] != 0:
        failures.append(f"the backend dropped {result['draws_dropped']} draw(s)")
    if result["peak_draws_per_frame"] != draws:
        failures.append(f"the backend's peak was {result['peak_draws_per_frame']} draws in a "
                        f"frame, expected {draws}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True)
    ap.add_argument("--baseline", default=str(BASELINE_PATH))
    ap.add_argument("--draws", type=int, default=None,
                    help="draws per frame to require; defaults to the baseline's floor")
    ap.add_argument("--frames", type=int, default=None,
                    help="frames to sustain it over; defaults to the baseline's")
    ap.add_argument("--self-check", action="store_true",
                    help="also prove the gate fails when the old fixed cap is reimposed")
    ap.add_argument("--update", action="store_true",
                    help="regenerate the baseline from this run")
    args = ap.parse_args()

    binary = pathlib.Path(args.binary)
    if not binary.is_file():
        raise SystemExit(f"FAIL: no such binary {binary}")
    baseline_path = pathlib.Path(args.baseline)

    if args.update:
        draws = args.draws if args.draws is not None else 4096
        frames = args.frames if args.frames is not None else 3
        result = run_workload(binary, draws, frames)
        failures = []
        check_run(result, draws, frames, failures)
        sys.stdout.write(result["stdout"])
        sys.stderr.write(result["stderr"])
        if failures:
            for line in failures:
                print(f"FAIL: {line}", file=sys.stderr)
            print("refusing to write a baseline from a run that did not pass", file=sys.stderr)
            return 2
        baseline_path.parent.mkdir(parents=True, exist_ok=True)
        baseline_path.write_text(json.dumps({
            "_comment": "Draws per frame the renderer backend sustains with every draw's own "
                        "texture and uniforms verified by pixel readback, enforced by "
                        "scripts/ci/check-draw-capacity.py. Generated -- do not hand-edit; "
                        "regenerate with --update.",
            "draws_per_frame": draws,
            "frames": frames,
            "measured": {
                "descriptor_capacity": result["descriptor_capacity"],
                "descriptor_blocks": result["descriptor_blocks"],
                "draws_issued": result["draws_issued"],
                "draws_dropped": result["draws_dropped"],
                "peak_draws_per_frame": result["peak_draws_per_frame"],
                "tiles_correct_per_frame": result["correct"],
            },
            "replaced_fixed_cap": OLD_FIXED_CAP,
        }, indent=2) + "\n")
        print(f"wrote {baseline_path}")
        return 0

    if not baseline_path.is_file():
        print(f"FAIL: no baseline at {baseline_path}; create one with --update", file=sys.stderr)
        return 2
    baseline = json.loads(baseline_path.read_text())
    draws = args.draws if args.draws is not None else int(baseline["draws_per_frame"])
    frames = args.frames if args.frames is not None else int(baseline["frames"])

    result = run_workload(binary, draws, frames)
    sys.stdout.write(result["stdout"])
    sys.stderr.write(result["stderr"])
    failures = []
    check_run(result, draws, frames, failures)

    if args.self_check and not failures:
        # The defect this gate exists for, reimposed deliberately: the fixed preallocation of
        # 64 descriptor sets. It has to fail, and it has to fail as *dropped* draws.
        control = run_workload(binary, draws, 1, limit=OLD_FIXED_CAP)
        control_failures = []
        check_run(control, draws, 1, control_failures)
        if not control_failures:
            failures.append(f"the self-check passed with ZH_RENDER_MAX_DRAWS={OLD_FIXED_CAP}: "
                            f"this gate cannot fail and proves nothing")
        elif not control["dropped"] or control["dropped"][0] == 0:
            failures.append(f"with ZH_RENDER_MAX_DRAWS={OLD_FIXED_CAP} the workload failed "
                            f"without reporting a dropped tile, so the failure is not the one "
                            f"this gate measures")
        else:
            print(f"self-check: with the old fixed cap of {OLD_FIXED_CAP} the same frame loses "
                  f"{control['dropped'][0]} of {draws} draws and the gate fails, as it must")

    if failures:
        print()
        print("FAIL: draw capacity check", file=sys.stderr)
        for line in failures:
            print(f"  - {line}", file=sys.stderr)
        return 1
    print(f"\nOK: {draws} draws per frame sustained over {frames} frame(s) with every draw's own "
          f"texture and uniforms verified in the readback, 0 dropped and 0 aliased, "
          f"{result['descriptor_capacity']} descriptor sets in {result['descriptor_blocks']} "
          f"block(s), the validation layer active and silent")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Gate the staging pool's host-visible cost against a committed ceiling.

`zh-staging-workload` replays a frame's worth of D3D8 locks in the usage-class mix
docs/porting/renderer-resource-seam.md measured, and prints what that cost in
host-visible memory. This turns those numbers into a pass/fail signal, because the
regression worth catching is silent: a change that goes back to allocating staging per
lock still renders correctly and still exits 0, it just costs a multiple of the memory.

Checked per swizzle mode:

  * the number of host-visible staging allocations, against the ceiling. This is the
    check that fails when per-lock allocation comes back -- the workload performs
    hundreds of locks, so an implementation without a pool cannot stay under it;
  * resident, peak and steady-state staging bytes, against their ceilings;
  * the reuse rate, against a floor;
  * the bytes preservation reads back. A pool that publishes zeroes on acquire is
    cheaper than one that honours D3D8's preserve-on-Lock contract, and the honest way
    to keep the cheaper one from creeping back is to bound what the contract costs
    rather than to leave it unmeasured;
  * allocations per lock, which must stay below a small fraction: it is the shape of
    the regression rather than its size, so it holds even if the workload grows;
  * the pixel check and the validation layer, both of which the workload itself
    enforces, re-asserted here so a silently loosened binary cannot pass the gate.

The ceiling lives in docs/porting/ci-baselines/ so that moving it is a reviewable
diff, and is refreshed with --update in the PR that earns it.
"""
import argparse
import json
import os
import pathlib
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
CEILING_PATH = REPO_ROOT / "docs" / "porting" / "ci-baselines" / "staging-cost-ceiling.json"

# Headroom over the measured figure when writing a ceiling with --update. Size-class
# rounding means a small change in a resource size can move the byte totals by one
# block, and that should not be a CI failure on its own.
BYTE_HEADROOM = 1.25
ALLOC_HEADROOM = 2
# A pool that recycles at all reuses nearly every acquire; without one the rate is 0.
REUSE_FLOOR = 0.90
# Per-lock allocation in this workload is ~0.7 allocations per lock. Anything above
# this is not a pool.
MAX_ALLOCATIONS_PER_LOCK = 0.05


def run_workload(binary, frames, no_swizzle, validation, retain=False):
    env = dict(os.environ)
    if no_swizzle:
        env["ZH_SPIKE_NO_VIEW_SWIZZLE"] = "1"
    else:
        env.pop("ZH_SPIKE_NO_VIEW_SWIZZLE", None)
    # The "before" mode must never be what the gate measures, except in the
    # self-check that proves the gate would catch it.
    if retain:
        env["ZH_SPIKE_STAGING_RETAIN"] = "1"
    else:
        env.pop("ZH_SPIKE_STAGING_RETAIN", None)
    cmd = [str(binary), "--frames", str(frames)]
    if not validation:
        cmd.append("--no-validation")
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if proc.stderr.strip():
        for line in proc.stderr.strip().splitlines():
            print(f"    {line}")
    if proc.returncode != 0:
        raise SystemExit(f"FAIL: {' '.join(cmd)} exited {proc.returncode}")
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        print(proc.stdout)
        raise SystemExit(f"FAIL: workload output is not JSON: {exc}")


def ceiling_from(result):
    def up(value, factor):
        return int(value * factor) + 1

    return {
        "frames": result["frames"],
        "staging_allocations": result["staging_allocations"] + ALLOC_HEADROOM,
        "staging_resident_bytes": up(result["staging_resident_bytes"], BYTE_HEADROOM),
        "staging_peak_bytes": up(result["staging_peak_bytes"], BYTE_HEADROOM),
        "staging_steady_state_bytes": up(result["staging_steady_state_bytes"], BYTE_HEADROOM),
        "staging_preserve_bytes": up(result["staging_preserve_bytes"], BYTE_HEADROOM),
        "measured": {
            "staging_allocations": result["staging_allocations"],
            "staging_resident_bytes": result["staging_resident_bytes"],
            "staging_peak_bytes": result["staging_peak_bytes"],
            "staging_steady_state_bytes": result["staging_steady_state_bytes"],
            "staging_reuse_rate": result["staging_reuse_rate"],
            "texture_locks": result["texture_locks"],
            "buffer_locks": result["buffer_locks"],
            "staging_preserve_readbacks": result["staging_preserve_readbacks"],
            "staging_preserve_bytes": result["staging_preserve_bytes"],
            "staging_preserve_skips": result["staging_preserve_skips"],
            "gpu_write_marks": result["gpu_write_marks"],
            "dirty_reads": result["dirty_reads"],
            "clean_reads": result["clean_reads"],
        },
    }


def check_mode(name, result, ceiling, failures):
    print(f"\n  {name}")
    print(f"    {'metric':32s} {'measured':>12s} {'ceiling':>12s}")
    for key in ("staging_allocations", "staging_resident_bytes", "staging_peak_bytes",
                "staging_steady_state_bytes", "staging_preserve_bytes"):
        if key not in ceiling:
            continue
        measured = result[key]
        limit = ceiling[key]
        ok = measured <= limit
        print(f"    {key:32s} {measured:12d} {limit:12d}  {'ok' if ok else 'OVER'}")
        if not ok:
            failures.append(f"{name}: {key} is {measured}, ceiling {limit}")

    locks = result["texture_locks"]
    per_lock = result["staging_allocations"] / locks if locks else 0.0
    print(f"    {'allocations per texture lock':32s} {per_lock:12.4f} "
          f"{MAX_ALLOCATIONS_PER_LOCK:12.4f}"
          f"  {'ok' if per_lock <= MAX_ALLOCATIONS_PER_LOCK else 'OVER'}")
    if per_lock > MAX_ALLOCATIONS_PER_LOCK:
        failures.append(f"{name}: {result['staging_allocations']} allocations for {locks} "
                        f"locks ({per_lock:.3f} per lock) -- staging is not being recycled")

    rate = result["staging_reuse_rate"]
    print(f"    {'staging reuse rate':32s} {rate:12.4f} {REUSE_FLOOR:12.4f}"
          f"  {'ok' if rate >= REUSE_FLOOR else 'UNDER'}")
    if rate < REUSE_FLOOR:
        failures.append(f"{name}: reuse rate {rate:.4f} is below the {REUSE_FLOOR} floor")

    if not result["pixels_ok"]:
        failures.append(f"{name}: the probe texture read back wrong, so the pool is not correct")
    if result["validation_messages"]:
        failures.append(f"{name}: {result['validation_messages']} validation message(s)")

    # A read that finds the resource clean must cost nothing, which is the whole point
    # of the GPU-dirty bit: if every read is a transfer again, this is 0.
    print(f"    {'reads served without a transfer':32s} {result['clean_reads']:12d}")
    if result["clean_reads"] == 0:
        failures.append(f"{name}: no read was served from a clean resource, so either the "
                        f"dirty bit is always set or the workload stopped reading")

    # The mode has to have actually been the mode.
    if result["mode"] != "pool":
        failures.append(f"{name}: workload ran in '{result['mode']}' mode, not 'pool'")
    if result["staging_allocations"] != result["staging_acquires"] - result["staging_reuses"]:
        failures.append(f"{name}: the pool's own accounting does not add up")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True, help="path to zh-staging-workload")
    ap.add_argument("--frames", type=int, default=12)
    ap.add_argument("--ceiling", default=str(CEILING_PATH))
    ap.add_argument("--update", action="store_true",
                    help="rewrite the ceiling from this run's measurement")
    ap.add_argument("--no-validation", action="store_true",
                    help="do not require the validation layer (for a device without it)")
    ap.add_argument("--self-check", action="store_true",
                    help="also run the pre-pool retain mode and require that it VIOLATES the "
                         "ceiling, so the gate is known to catch a regression rather than "
                         "only to pass")
    args = ap.parse_args()

    binary = pathlib.Path(args.binary)
    if not binary.is_file():
        raise SystemExit(f"FAIL: no such binary {binary}")

    results = {
        "swizzle": run_workload(binary, args.frames, False, not args.no_validation),
        "no_swizzle": run_workload(binary, args.frames, True, not args.no_validation),
    }

    ceiling_path = pathlib.Path(args.ceiling)
    if args.update:
        payload = {
            "_comment": "Ceiling for the staging pool's host-visible cost, enforced by "
                        "scripts/ci/check-staging-cost.py. Measured figures are the run "
                        "that produced this file; the ceilings carry headroom for "
                        "size-class rounding.",
            "frames": args.frames,
            "modes": {name: ceiling_from(result) for name, result in results.items()},
        }
        ceiling_path.parent.mkdir(parents=True, exist_ok=True)
        ceiling_path.write_text(json.dumps(payload, indent=2) + "\n")
        print(f"wrote {ceiling_path.relative_to(REPO_ROOT)}")
        return 0

    if not ceiling_path.is_file():
        print(f"FAIL: no ceiling at {ceiling_path}; create one with --update", file=sys.stderr)
        return 2
    ceiling = json.loads(ceiling_path.read_text())

    if ceiling.get("frames") != args.frames:
        print(f"FAIL: ceiling was measured over {ceiling.get('frames')} frames, this run used "
              f"{args.frames}; the totals are not comparable", file=sys.stderr)
        return 2

    print("staging cost, per swizzle mode (host-visible memory behind Lock/Unlock)")
    failures = []
    for name, result in results.items():
        if name not in ceiling["modes"]:
            failures.append(f"{name}: no ceiling for this mode")
            continue
        check_mode(name, result, ceiling["modes"][name], failures)

    if args.self_check and not failures:
        # The regression this gate exists for, reintroduced deliberately: every resource
        # pins its own staging, which is what the spike did before the pool.
        regression = run_workload(binary, args.frames, False, not args.no_validation,
                                  retain=True)
        limits = ceiling["modes"]["swizzle"]
        caught = [key for key in ("staging_allocations", "staging_resident_bytes",
                                  "staging_peak_bytes", "staging_steady_state_bytes")
                  if regression[key] > limits[key]]
        print("\n  self-check: pre-pool (retain) mode")
        for key in ("staging_allocations", "staging_resident_bytes",
                    "staging_peak_bytes", "staging_steady_state_bytes"):
            print(f"    {key:32s} {regression[key]:12d} {limits[key]:12d}"
                  f"  {'over (good)' if key in caught else 'within'}")
        if not caught:
            failures.append("self-check: the pre-pool behaviour stays within the ceiling, "
                            "so this gate would not catch a regression")
        else:
            print(f"    the ceiling rejects it on: {', '.join(caught)}")

    if failures:
        print("\nFAIL:", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1
    print("\nOK: staging cost within the committed ceiling in both swizzle modes")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Assert the backend frees the text path's per-frame surface and texture when they are released.

Render2DSentenceClass::Build_Textures creates, per string per frame, a system-memory A4R4G4B4
surface, a one-level A4R4G4B4 texture and that texture's level-0 surface, copies the glyphs
across, draws, and releases all three. Under D3D8 the last Release() frees the texture with
its level and the image surface with its memory. Until the Vulkan backend had a destroy path
every Release() reaching zero freed the wrapper alone and the backend kept the resource until
Shutdown: `owned_surfaces_` and `owned_textures_` grew without bound for the whole session,
and Get_Surface_Level's linear scan over the first of them grew with it
(docs/porting/renderer-resource-lifetime.md).

`zh-resource-lifetime` runs that exact sequence against the backend, K strings per frame for N
frames, verifies each frame's texel in the readback so the textures were really created,
copied and drawn, and requires the backend's live texture and surface counts to be back at
their pre-loop bound afterwards, created == destroyed, and nothing left pending after one idle
frame. It also times Get_Surface_Level in the first and last frames, separately, because a
scan that grows is a symptom of the leak and a scan fixed on its own would hide one.

The negative control is always run: with ZH_RENDER_NO_RESOURCE_DESTROY set the backend
ignores every destroy, as it did before the path existed, and the workload *must* fail with
the live counts above their bound by exactly N*K textures and 2*N*K surfaces -- and still
with every texel verified, so the control fails for the leak and nothing else. A gate whose
negative control passes cannot fail and proves nothing.

    python3 scripts/ci/check-resource-lifetime.py --binary build/spike/zh-resource-lifetime
"""
import argparse
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import vulkan_manifests  # noqa: E402

WORKLOAD = re.compile(r"^workload: (\d+) frames x (\d+) strings of", re.MULTILINE)
STATS = re.compile(r"^(before|after) (textures|surfaces) created (\d+) destroyed (\d+) live (\d+)$",
                   re.MULTILINE)
PENDING = re.compile(r"^after retired pending (\d+)$", re.MULTILINE)
TEXELS = re.compile(r"^texels verified (\d+) of (\d+)$", re.MULTILINE)
SCAN = re.compile(r"^get_surface_level ns/call first (\d+) frames (\d+) last (\d+) frames (\d+)$",
                  re.MULTILINE)
VALIDATION_MESSAGES = re.compile(r"^validation messages: (\d+)$", re.MULTILINE)
LIVE_EXCEEDED = re.compile(
    r"^resource-lifetime: live (textures|surfaces) (\d+) exceed bound (\d+)$", re.MULTILINE)

# The workload's own shape, asserted here so a cut-down workload cannot pass by printing less.
STRINGS_PER_FRAME = 8
SURFACES_PER_TEXTURE = 2  # the image surface and the texture's level 0

# The scan's cost at the end of the run may not exceed this multiple of its cost at the start.
# With the live set bounded the scan is over a handful of surfaces both times; the pre-fix
# backend measured 24x on 300 frames (271 -> 6549 ns/call on lavapipe).
SCAN_GROWTH_LIMIT = 4.0


def run_workload(binary, frames, disable=None):
    env = vulkan_manifests.child_environment()
    if disable is not None:
        env[disable] = "1"
    proc = subprocess.run([str(binary), "--frames", str(frames), "--validation"],
                          capture_output=True, text=True, env=env)
    text = proc.stdout + proc.stderr
    result = {
        "returncode": proc.returncode,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
        "validation_loaded": "validation layer: loaded" in text,
        "stats": {},
        "exceeded": {kind: (int(live), int(bound))
                     for kind, live, bound in LIVE_EXCEEDED.findall(text)},
    }
    workload = WORKLOAD.search(proc.stdout)
    result["frames"] = int(workload.group(1)) if workload else None
    result["strings"] = int(workload.group(2)) if workload else None
    for when, kind, created, destroyed, live in STATS.findall(proc.stdout):
        result["stats"][(when, kind)] = {
            "created": int(created), "destroyed": int(destroyed), "live": int(live)}
    pending = PENDING.search(proc.stdout)
    result["pending"] = int(pending.group(1)) if pending else None
    texels = TEXELS.search(proc.stdout)
    result["texels"] = (int(texels.group(1)), int(texels.group(2))) if texels else None
    scan = SCAN.search(proc.stdout)
    result["scan"] = (int(scan.group(2)), int(scan.group(4))) if scan else None
    messages = VALIDATION_MESSAGES.search(proc.stdout)
    result["validation_messages"] = int(messages.group(1)) if messages else None
    return result


def delta(result, kind, field):
    stats = result["stats"]
    if ("before", kind) not in stats or ("after", kind) not in stats:
        return None
    return stats[("after", kind)][field] - stats[("before", kind)][field]


def check_shape(result, frames, failures):
    """What both the run under test and the control must have done, leak or no leak."""
    if result["frames"] != frames or result["strings"] != STRINGS_PER_FRAME:
        failures.append(f"the workload reported {result['frames']} frames x {result['strings']} "
                        f"strings, expected {frames} x {STRINGS_PER_FRAME}: it was cut down, "
                        f"not passed")
        return False
    expected = frames * STRINGS_PER_FRAME
    if result["texels"] != (expected, expected):
        failures.append(f"texels verified {result['texels']}, expected every one of {expected}: "
                        f"a frame drew a stale or missing texture")
    if delta(result, "textures", "created") != expected:
        failures.append(f"{delta(result, 'textures', 'created')} textures created, expected "
                        f"{expected}")
    if delta(result, "surfaces", "created") != expected * SURFACES_PER_TEXTURE:
        failures.append(f"{delta(result, 'surfaces', 'created')} surfaces created, expected "
                        f"{expected * SURFACES_PER_TEXTURE}")
    if not result["validation_loaded"]:
        failures.append("the validation layer was not loaded, so this run says nothing about "
                        "validation cleanliness -- see scripts/ci/vulkan_manifests.py")
    if result["validation_messages"] is None:
        failures.append("the workload printed no validation message count")
    elif result["validation_messages"] != 0:
        failures.append(f"{result['validation_messages']} validation message(s)")
    return True


def check_run(result, frames, failures):
    if result["returncode"] != 0:
        failures.append(f"the workload exited {result['returncode']}")
    if not check_shape(result, frames, failures):
        return
    expected = frames * STRINGS_PER_FRAME
    for kind, per_texture in (("textures", 1), ("surfaces", SURFACES_PER_TEXTURE)):
        live = delta(result, kind, "live")
        destroyed = delta(result, kind, "destroyed")
        if live is None or live > 0:
            failures.append(f"live {kind} ended {live} above their bound: the leak")
        if destroyed != expected * per_texture:
            failures.append(f"{destroyed} {kind} destroyed, expected {expected * per_texture}")
    if result["pending"] != 0:
        failures.append(f"{result['pending']} resource(s) still pending after an idle frame")
    if result["scan"] is None:
        failures.append("the workload did not time Get_Surface_Level")
    else:
        first, last = result["scan"]
        if first > 0 and last > first * SCAN_GROWTH_LIMIT:
            failures.append(f"Get_Surface_Level cost grew {first} -> {last} ns/call over the run "
                            f"(limit {SCAN_GROWTH_LIMIT}x): the scan is over a growing list")


def check_control(control, frames, failures):
    if control["returncode"] == 0:
        failures.append("the negative control passed with ZH_RENDER_NO_RESOURCE_DESTROY set: "
                        "this gate cannot fail and proves nothing")
        return
    shape = []
    if not check_shape(control, frames, shape):
        failures.append("with ZH_RENDER_NO_RESOURCE_DESTROY set the workload did not run its "
                        "full shape, so the failure is not the one this gate measures")
        return
    if shape:
        failures.append("with ZH_RENDER_NO_RESOURCE_DESTROY set the workload failed for reasons "
                        "other than the leak: " + "; ".join(shape))
    expected = frames * STRINGS_PER_FRAME
    for kind, per_texture in (("textures", 1), ("surfaces", SURFACES_PER_TEXTURE)):
        if delta(control, kind, "live") != expected * per_texture:
            failures.append(f"with ZH_RENDER_NO_RESOURCE_DESTROY set live {kind} grew by "
                            f"{delta(control, kind, 'live')}, expected exactly "
                            f"{expected * per_texture}: the control did not reproduce the leak")
        if kind not in control["exceeded"]:
            failures.append(f"with ZH_RENDER_NO_RESOURCE_DESTROY set the workload did not report "
                            f"live {kind} exceeding their bound")
    if not failures:
        print(f"negative control: with ZH_RENDER_NO_RESOURCE_DESTROY every texel still verifies "
              f"and live textures/surfaces end {expected}/{expected * SURFACES_PER_TEXTURE} above "
              f"their bound, so the workload fails for the leak, as it must")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True)
    ap.add_argument("--frames", type=int, default=300)
    args = ap.parse_args()

    binary = pathlib.Path(args.binary)
    if not binary.is_file():
        raise SystemExit(f"FAIL: no such binary {binary}")

    result = run_workload(binary, args.frames)
    sys.stdout.write(result["stdout"])
    sys.stderr.write(result["stderr"])
    failures = []
    check_run(result, args.frames, failures)

    control = run_workload(binary, args.frames, disable="ZH_RENDER_NO_RESOURCE_DESTROY")
    if control["scan"] is not None:
        print(f"negative control: Get_Surface_Level {control['scan'][0]} -> {control['scan'][1]} "
              f"ns/call over {args.frames} frames with the leak")
    check_control(control, args.frames, failures)

    if failures:
        print()
        print("FAIL: resource lifetime check", file=sys.stderr)
        for line in failures:
            print(f"  - {line}", file=sys.stderr)
        return 1
    expected = args.frames * STRINGS_PER_FRAME
    print(f"\nOK: {expected} text textures (+{expected * SURFACES_PER_TEXTURE} surfaces) created, "
          f"drawn with every texel verified, and destroyed over {args.frames} frames; live counts "
          f"back at their bound, Get_Surface_Level {result['scan'][0]} -> {result['scan'][1]} "
          f"ns/call, the validation layer active and silent")
    return 0


if __name__ == "__main__":
    sys.exit(main())

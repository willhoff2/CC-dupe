#!/usr/bin/env python3
"""Assert the backend draws vertex buffers created with FVF 0 using the layout bound at draw time.

D3D8 lets CreateVertexBuffer take an FVF of 0 -- the buffer has no layout of its own and is
read with whatever SetVertexShader has bound when it is drawn: a fixed-function FVF, or a
program whose D3DVSD_* declaration describes the stream. Zero Hour's two shadow managers
create their dynamic buffers that way, and until this path existed the backend printed
`Decode_Fvf: unsupported FVF 0x0` and refused them, so the shadow passes had no buffer to
draw from (docs/porting/untyped-vertex-buffers.md).

`zh-untyped-vb` fills one such buffer with a row of tiles per layout the engine binds over it
(FVFs XYZ, XYZ|DIFFUSE, XYZ|DIFFUSE|TEX1, the last again at a padded SetStreamSource stride;
the water and tree declarations; and a declaration in an order no FVF can express), draws
them, reads the target back and requires every tile to carry its own id through the
attribute that layout supplies; one more draw with nothing bound has to be refused and
counted under `untyped_draws_dropped`, not read with a stale layout.

Two negative controls are always run, one per path: with ZH_RENDER_NO_UNTYPED_VB set the
backend refuses the FVF-0 buffer as it did before the path existed, and the workload *must*
fail at creation; with ZH_RENDER_NO_VERTEX_DECLARATION set the backend ignores the bound
program's declaration as it did before it had one, and exactly the declaration cases *must*
come back dropped. A gate whose negative control passes cannot fail and proves nothing.

    python3 scripts/ci/check-untyped-vertex-buffer.py --binary build/spike/zh-untyped-vb
"""
import argparse
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import vulkan_manifests  # noqa: E402

CASE_HEADER = re.compile(r"^case (\d+) (.+)$", re.MULTILINE)
TILE_FIELDS = {
    "correct": re.compile(r"^  tiles correct\s+(\d+)$", re.MULTILINE),
    "dropped": re.compile(r"^  tiles dropped\s+(\d+)$", re.MULTILINE),
    "aliased": re.compile(r"^  tiles aliased\s+(\d+)$", re.MULTILINE),
    "alpha_wrong": re.compile(r"^  tiles alpha-wrong\s+(\d+)$", re.MULTILINE),
    "mismatched": re.compile(r"^  tiles mismatched\s+(\d+)$", re.MULTILINE),
}
BACKEND_FIELDS = {
    "draws_requested": re.compile(r"^backend draws requested\s+(\d+)$", re.MULTILINE),
    "draws_issued": re.compile(r"^backend draws issued\s+(\d+)$", re.MULTILINE),
    "draws_dropped": re.compile(r"^backend draws dropped\s+(\d+)$", re.MULTILINE),
    "untyped_issued": re.compile(r"^backend untyped issued\s+(\d+)$", re.MULTILINE),
    "untyped_dropped": re.compile(r"^backend untyped dropped\s+(\d+)$", re.MULTILINE),
}
VALIDATION_MESSAGES = re.compile(r"^validation messages: (\d+)$", re.MULTILINE)
FRAME_HEADER = re.compile(r"^frame (\d+)$", re.MULTILINE)
CREATED = re.compile(r"^untyped vertex buffer: (\d+) bytes created with FVF 0$", re.MULTILINE)
REFUSED = "the backend refused a vertex buffer with FVF 0"

# The workload's own shape, asserted here so a cut-down workload cannot pass by printing less.
TILES_PER_CASE = 64
DRAWN_CASES = 7
DECLARATION_CASES = (4, 5, 6)
REFUSED_CASE = 7


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
        "created": CREATED.search(proc.stdout) is not None,
        "refused": REFUSED in text,
        "frames_reported": len(FRAME_HEADER.findall(proc.stdout)),
        "cases": [(int(n), name) for n, name in CASE_HEADER.findall(proc.stdout)],
    }
    for name, pattern in TILE_FIELDS.items():
        result[name] = [int(v) for v in pattern.findall(proc.stdout)]
    for name, pattern in BACKEND_FIELDS.items():
        result[name] = [int(v) for v in pattern.findall(proc.stdout)]
    messages = VALIDATION_MESSAGES.search(proc.stdout)
    result["validation_messages"] = int(messages.group(1)) if messages else None
    return result


def check_run(result, frames, failures):
    if result["returncode"] != 0:
        failures.append(f"the workload exited {result['returncode']}")
    if not result["created"]:
        failures.append("the backend did not create the FVF-0 vertex buffer")
    if not result["validation_loaded"]:
        failures.append("the validation layer was not loaded, so this run says nothing about "
                        "validation cleanliness -- see scripts/ci/vulkan_manifests.py")
    if result["validation_messages"] is None:
        failures.append("the workload printed no validation message count")
    elif result["validation_messages"] != 0:
        failures.append(f"{result['validation_messages']} validation message(s)")

    if result["frames_reported"] != frames:
        failures.append(f"{result['frames_reported']} frame(s) classified, expected {frames}")
    cases_per_frame = DRAWN_CASES + 1
    expected_cases = frames * cases_per_frame
    if len(result["correct"]) != expected_cases:
        failures.append(f"{len(result['correct'])} case(s) classified, expected "
                        f"{expected_cases}: the workload was cut down, not passed")
        return
    for index, (case, name) in enumerate(result["cases"]):
        frame = index // cases_per_frame
        correct = result["correct"][index]
        dropped = result["dropped"][index]
        if case == REFUSED_CASE:
            if dropped != TILES_PER_CASE or correct != 0:
                failures.append(f"frame {frame} case {case} ({name}): {TILES_PER_CASE - dropped} "
                                f"tile(s) written by a draw that had no layout to draw with")
        elif correct != TILES_PER_CASE:
            failures.append(
                f"frame {frame} case {case} ({name}): {correct} of {TILES_PER_CASE} tiles "
                f"correct ({dropped} dropped, {result['aliased'][index]} aliased, "
                f"{result['alpha_wrong'][index]} alpha-wrong, "
                f"{result['mismatched'][index]} mismatched)")

    expected_issued = DRAWN_CASES * TILES_PER_CASE
    for frame in range(frames):
        stats = {name: result[name][frame] if frame < len(result[name]) else None
                 for name in BACKEND_FIELDS}
        if stats["draws_requested"] != expected_issued + 1:
            failures.append(f"frame {frame}: the backend saw {stats['draws_requested']} draws "
                            f"requested, expected {expected_issued + 1}")
        if stats["draws_issued"] != expected_issued or stats["untyped_issued"] != expected_issued:
            failures.append(f"frame {frame}: the backend issued {stats['draws_issued']} draws "
                            f"({stats['untyped_issued']} untyped), expected {expected_issued}")
        if stats["draws_dropped"] != 1 or stats["untyped_dropped"] != 1:
            failures.append(f"frame {frame}: {stats['draws_dropped']} draw(s) dropped, "
                            f"{stats['untyped_dropped']} of them untyped; expected exactly the "
                            f"one draw with no FVF bound")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True)
    ap.add_argument("--frames", type=int, default=3)
    args = ap.parse_args()

    binary = pathlib.Path(args.binary)
    if not binary.is_file():
        raise SystemExit(f"FAIL: no such binary {binary}")

    result = run_workload(binary, args.frames)
    sys.stdout.write(result["stdout"])
    sys.stderr.write(result["stderr"])
    failures = []
    check_run(result, args.frames, failures)

    # The negative controls: the backend as it was before each path existed.
    control = run_workload(binary, 1, disable="ZH_RENDER_NO_UNTYPED_VB")
    if control["returncode"] == 0:
        failures.append("the negative control passed with ZH_RENDER_NO_UNTYPED_VB set: this "
                        "gate cannot fail and proves nothing")
    elif not control["refused"] or control["created"]:
        failures.append("with ZH_RENDER_NO_UNTYPED_VB set the workload failed for a reason other "
                        "than the FVF-0 buffer being refused, so the failure is not the one this "
                        "gate measures")
    else:
        print("negative control: with ZH_RENDER_NO_UNTYPED_VB the backend refuses the FVF-0 "
              "buffer and the workload fails, as it must")

    control = run_workload(binary, 1, disable="ZH_RENDER_NO_VERTEX_DECLARATION")
    if control["returncode"] == 0:
        failures.append("the negative control passed with ZH_RENDER_NO_VERTEX_DECLARATION set: "
                        "this gate cannot fail and proves nothing")
    elif len(control["correct"]) != DRAWN_CASES + 1:
        failures.append("with ZH_RENDER_NO_VERTEX_DECLARATION set the workload did not classify "
                        "every case, so the failure is not the one this gate measures")
    else:
        wrong = []
        for index, (case, name) in enumerate(control["cases"]):
            correct = control["correct"][index]
            dropped = control["dropped"][index]
            if case in DECLARATION_CASES:
                if correct != 0 or dropped != TILES_PER_CASE:
                    wrong.append(f"case {case} ({name}) was drawn without a declaration path")
            elif case != REFUSED_CASE and correct != TILES_PER_CASE:
                wrong.append(f"case {case} ({name}) broke, which is not what the control "
                             f"disables")
        if wrong:
            failures.append("with ZH_RENDER_NO_VERTEX_DECLARATION set: " + "; ".join(wrong))
        else:
            print("negative control: with ZH_RENDER_NO_VERTEX_DECLARATION the declaration cases "
                  "come back dropped and counted, the FVF cases still draw, and the workload "
                  "fails, as it must")

    if failures:
        print()
        print("FAIL: untyped vertex buffer check", file=sys.stderr)
        for line in failures:
            print(f"  - {line}", file=sys.stderr)
        return 1
    print(f"\nOK: {DRAWN_CASES} layouts ({DRAWN_CASES - len(DECLARATION_CASES)} FVF, "
          f"{len(DECLARATION_CASES)} declaration) x {TILES_PER_CASE} tiles drawn from one FVF-0 "
          f"vertex buffer over {args.frames} frame(s), every tile's id verified in the readback, "
          f"the layout-less draw refused and counted, the validation layer active and silent")
    return 0


if __name__ == "__main__":
    sys.exit(main())

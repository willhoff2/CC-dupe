#!/usr/bin/env python3
"""Assert the backend creates, fills, and samples real block-compressed (BC1/BC2/BC3) textures.

Zero Hour's art is almost entirely DXT1/DXT3/DXT5 `.dds`. Before this path existed the
backend's lockable texture path had no block layout at all -- `Source_Texel_Bytes` returned 0
for the DXT formats, so `Create_Lockable_Texture` refused them, `CreateTexture` failed, the
off-Windows D3DX substituted A8R8G8B8 and every level took the engine's software decode at
four times the memory, with not one compressed image ever created
(docs/porting/block-compressed-textures.md).

`zh-bc-textures` creates one 16x16, 5-level texture per format through the lockable path,
writes every level through LockRect at the pitch the backend reports, requires that pitch to
be the BLOCK pitch (bytes per row of 4x4 blocks: 8 per block for BC1, 16 for BC2/BC3), locks a
block-aligned sub-rect and rewrites it, requires a half-block sub-rect to be refused, reads a
level back through a READONLY lock, then draws the textures and classifies every block of the
readback against the colour it was encoded with. The mip case draws level 1 through the point
mip filter, so mip levels above 0 have to be laid out and uploaded at block pitch too.

A negative control is always run: with ZH_RENDER_NO_BLOCK_COMPRESSED set the backend refuses
the compressed formats as it did before the path existed, and the workload *must* fail at
creation. A gate whose negative control passes cannot fail and proves nothing.

    python3 scripts/ci/check-bc-textures.py --binary build/spike/zh-bc-textures
"""
import argparse
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import vulkan_manifests  # noqa: E402

CASE_HEADER = re.compile(r"^case (\d+) (.+)$", re.MULTILINE)
BLOCK_FIELDS = {
    "correct": re.compile(r"^  blocks correct\s+(\d+)$", re.MULTILINE),
    "wrong_colour": re.compile(r"^  blocks wrong-colour\s+(\d+)$", re.MULTILINE),
    "wrong_alpha": re.compile(r"^  blocks wrong-alpha\s+(\d+)$", re.MULTILINE),
    "missing": re.compile(r"^  blocks missing\s+(\d+)$", re.MULTILINE),
}
CREATED = re.compile(r"^(DXT\d \(BC\d\)): 16x16, 5 levels created through the lockable path$",
                     re.MULTILINE)
LOCK_PITCH = re.compile(r"^(DXT\d \(BC\d\)) level (\d) \((\d+) texels\): lock pitch (\d+), "
                        r"block pitch (\d+) (ok|WRONG)$", re.MULTILINE)
MISALIGNED = re.compile(
    r"^(DXT\d \(BC\d\)) misaligned sub-rect \(2,0\)-\(6,4\): (refused|ACCEPTED)$", re.MULTILINE)
READ_BACK = re.compile(r"^(DXT\d \(BC\d\)) level 2 read back: (\d+) block bytes (match|DIFFER)$",
                       re.MULTILINE)
ORIENTATION = re.compile(r"^orientation: .*: (top-left verified|NOT top-left)$", re.MULTILINE)
BC_TEXTURES = re.compile(r"^backend block-compressed textures (\d+)$", re.MULTILINE)
VALIDATION_MESSAGES = re.compile(r"^validation messages: (\d+)$", re.MULTILINE)
REFUSED = "the backend refused a block-compressed texture"

# The workload's own shape, asserted here so a cut-down workload cannot pass by printing less.
FORMATS = ("DXT1 (BC1)", "DXT3 (BC2)", "DXT5 (BC3)")
BLOCK_BYTES = {"DXT1 (BC1)": 8, "DXT3 (BC2)": 16, "DXT5 (BC3)": 16}
LEVELS = 5
CASES_PER_FORMAT = 3
MIP_CASE = 2
BLOCKS_LEVEL0 = 16
BLOCKS_LEVEL1 = 4


def run_workload(binary, disable=None):
    env = vulkan_manifests.child_environment()
    if disable is not None:
        env[disable] = "1"
    proc = subprocess.run([str(binary), "--validation"], capture_output=True, text=True, env=env)
    text = proc.stdout + proc.stderr
    result = {
        "returncode": proc.returncode,
        "stdout": proc.stdout,
        "stderr": proc.stderr,
        "validation_loaded": "validation layer: loaded" in text,
        "created": CREATED.findall(proc.stdout),
        "refused": REFUSED in text,
        "pitches": LOCK_PITCH.findall(proc.stdout),
        "misaligned": MISALIGNED.findall(proc.stdout),
        "read_back": READ_BACK.findall(proc.stdout),
        "orientation": ORIENTATION.findall(proc.stdout),
        "cases": [(int(n), name) for n, name in CASE_HEADER.findall(proc.stdout)],
    }
    for name, pattern in BLOCK_FIELDS.items():
        result[name] = [int(v) for v in pattern.findall(proc.stdout)]
    bc = BC_TEXTURES.search(proc.stdout)
    result["bc_textures"] = int(bc.group(1)) if bc else None
    messages = VALIDATION_MESSAGES.search(proc.stdout)
    result["validation_messages"] = int(messages.group(1)) if messages else None
    return result


def check_run(result, failures):
    if result["returncode"] != 0:
        failures.append(f"the workload exited {result['returncode']}")
    if sorted(result["created"]) != sorted(FORMATS):
        failures.append(f"created {result['created']}, expected one texture per format "
                        f"{list(FORMATS)}")
    if result["bc_textures"] != len(FORMATS):
        failures.append(f"the backend counted {result['bc_textures']} block-compressed textures, "
                        f"expected {len(FORMATS)}: the images were not created compressed")
    if not result["validation_loaded"]:
        failures.append("the validation layer was not loaded, so this run says nothing about "
                        "validation cleanliness -- see scripts/ci/vulkan_manifests.py")
    if result["validation_messages"] is None:
        failures.append("the workload printed no validation message count")
    elif result["validation_messages"] != 0:
        failures.append(f"{result['validation_messages']} validation message(s)")

    # Block pitch on every level of every format, checked against the arithmetic here as well
    # as against the workload's own verdict.
    if len(result["pitches"]) != len(FORMATS) * LEVELS:
        failures.append(f"{len(result['pitches'])} lock pitch lines, expected "
                        f"{len(FORMATS) * LEVELS}: the workload was cut down, not passed")
    for name, level, texels, pitch, _block_pitch, verdict in result["pitches"]:
        expected = ((int(texels) + 3) // 4) * BLOCK_BYTES[name]
        if int(pitch) != expected or verdict != "ok":
            failures.append(f"{name} level {level}: lock pitch {pitch}, block pitch is "
                            f"{expected} (a texel pitch would be {int(texels) * 4})")

    if len(result["misaligned"]) != len(FORMATS):
        failures.append("the misaligned sub-rect lock was not attempted on every format")
    for name, verdict in result["misaligned"]:
        if verdict != "refused":
            failures.append(f"{name}: a half-block sub-rect lock was accepted; a compressed "
                            f"sub-rect lock must be block-aligned")

    if len(result["read_back"]) != len(FORMATS):
        failures.append("the level-2 read-back was not performed on every format")
    for name, nbytes, verdict in result["read_back"]:
        if verdict != "match" or int(nbytes) != BLOCK_BYTES[name]:
            failures.append(f"{name}: the READONLY lock of level 2 did not return the block "
                            f"that was written")

    if result["orientation"] != ["top-left verified"]:
        failures.append("DXT1's block 0 did not land in the top-left block of its quad: the "
                        "image is flipped, transposed, or not the one drawn")

    expected_cases = len(FORMATS) * CASES_PER_FORMAT
    if len(result["correct"]) != expected_cases:
        failures.append(f"{len(result['correct'])} case(s) classified, expected "
                        f"{expected_cases}: the workload was cut down, not passed")
        return
    for index, (case, name) in enumerate(result["cases"]):
        kind = case % CASES_PER_FORMAT
        expected = BLOCKS_LEVEL1 if kind == MIP_CASE else BLOCKS_LEVEL0
        correct = result["correct"][index]
        if correct != expected:
            failures.append(
                f"case {case} ({name}): {correct} of {expected} blocks correct "
                f"({result['wrong_colour'][index]} wrong-colour, "
                f"{result['wrong_alpha'][index]} wrong-alpha, {result['missing'][index]} missing)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--binary", required=True)
    args = ap.parse_args()

    binary = pathlib.Path(args.binary)
    if not binary.is_file():
        raise SystemExit(f"FAIL: no such binary {binary}")

    result = run_workload(binary)
    sys.stdout.write(result["stdout"])
    sys.stderr.write(result["stderr"])
    failures = []
    check_run(result, failures)

    # The negative control: the backend as it was before the path existed.
    control = run_workload(binary, disable="ZH_RENDER_NO_BLOCK_COMPRESSED")
    if control["returncode"] == 0:
        failures.append("the negative control passed with ZH_RENDER_NO_BLOCK_COMPRESSED set: "
                        "this gate cannot fail and proves nothing")
    elif not control["refused"] or control["created"] or control["bc_textures"] not in (None, 0):
        failures.append("with ZH_RENDER_NO_BLOCK_COMPRESSED set the workload failed for a reason "
                        "other than the compressed texture being refused, so the failure is not "
                        "the one this gate measures")
    else:
        print("negative control: with ZH_RENDER_NO_BLOCK_COMPRESSED the backend refuses the "
              "compressed texture and the workload fails, as it must")

    if failures:
        print()
        print("FAIL: block-compressed texture check", file=sys.stderr)
        for line in failures:
            print(f"  - {line}", file=sys.stderr)
        return 1
    print(f"\nOK: {len(FORMATS)} block-compressed formats x {LEVELS} levels created, written at "
          f"block pitch, a block-aligned sub-rect rewritten, a half-block sub-rect refused, a "
          f"level read back byte-exact, {len(FORMATS) * CASES_PER_FORMAT} cases drawn and every "
          f"block verified in the readback, the validation layer active and silent")
    return 0


if __name__ == "__main__":
    sys.exit(main())

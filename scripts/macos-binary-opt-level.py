#!/usr/bin/env python3
"""Tell an unoptimised (-O0) arm64 Mach-O from an optimised one, from its machine code.

    python3 scripts/macos-binary-opt-level.py build/native/native_strict_link
    python3 scripts/macos-binary-opt-level.py --self-check

For each sampled function it disassembles with `otool -tV -p` and counts two -O0 signatures:
an unconditional `b` to the very next instruction (clang -O0 emits one per basic-block fall-through;
no optimised build does), and the share of instructions that load or store a frame slot
(`[sp, ...]` / `[x29, ...]`). `--self-check` compiles one control function at -O0 and -O2 and
asserts the verdicts, so the heuristic is shown to separate the two before it is trusted.
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# Hot functions in the late-game profiles (docs/porting/late-game-frame-cost.md), plus one backend
# function, so the verdict covers the engine and the renderer.
DEFAULT_SYMBOLS = [
    "__ZN10Pathfinder7getCellE17PathfindLayerEnumii",
    "__ZN10Pathfinder23examineNeighboringCellsEP12PathfindCellS1_RK12LocomotorSetbbiRK8ICoord2D"
    "PK6Objecti",
    "__ZNK6Object15getRelationshipEPKS_",
    "__ZN10Pathfinder16checkForMovementEPK6ObjectR18TCheckMovementInfo",
    "__ZN10DX8Wrapper26Apply_Render_State_ChangesEv",
    "__ZN5spike13VulkanBackend12Prepare_DrawEjRKNS_18VertexBufferHandleE",
]
INSTRUCTION = re.compile(r"^([0-9a-f]+)\s+(\S+)\s*(.*)$")
FRAME_SLOT = re.compile(r"\[(sp|x29)\b")
LOAD_STORE = ("ldr", "ldur", "str", "stur", "ldp", "stp", "ldrb", "strb", "ldrh", "strh", "ldrsw")


def disassemble(binary, symbol):
    command = ["otool", "-tV", "-p", symbol, binary]
    out = subprocess.run(command, capture_output=True, text=True).stdout
    instructions = []
    for line in out.splitlines():
        match = INSTRUCTION.match(line.strip())
        if not match:
            continue
        if instructions and line.strip().startswith("__"):
            break  # next symbol
        instructions.append((int(match.group(1), 16), match.group(2), match.group(3)))
    # otool -p prints from the symbol to the end of the section; cap at the first `ret`.
    for index, (_, mnemonic, _) in enumerate(instructions):
        if mnemonic == "ret":
            return instructions[: index + 1]
    return instructions


def measure(instructions):
    branch_to_next = 0
    frame_slot_accesses = 0
    for index, (address, mnemonic, operands) in enumerate(instructions):
        if mnemonic == "b" and index + 1 < len(instructions):
            target = operands.split()[0]
            if target.startswith("0x") and int(target, 16) == instructions[index + 1][0]:
                branch_to_next += 1
        if mnemonic in LOAD_STORE and FRAME_SLOT.search(operands):
            frame_slot_accesses += 1
    count = len(instructions)
    return {
        "instructions": count,
        "branch_to_next": branch_to_next,
        "frame_slot_share": frame_slot_accesses / count if count else 0.0,
    }


def verdict(stats):
    unoptimised = stats["branch_to_next"] > 0 or stats["frame_slot_share"] > 0.30
    return "O0-like" if unoptimised else "optimised"


def report(binary, symbols):
    verdicts = set()
    for symbol in symbols:
        instructions = disassemble(binary, symbol)
        if not instructions:
            print(f"  {symbol}: not found")
            continue
        stats = measure(instructions)
        verdicts.add(verdict(stats))
        print(f"  {symbol}: {stats['instructions']} insns, "
              f"{stats['branch_to_next']} branch-to-next, "
              f"{stats['frame_slot_share']:.0%} frame-slot loads/stores -> {verdict(stats)}")
    return verdicts


CONTROL = """
struct Cell { int cost; int x; int y; Cell *next; };
struct Grid { int lo_x, lo_y, hi_x, hi_y; Cell *cells; int width; };
extern "C" Cell *control_get_cell(Grid *grid, int layer, int x, int y) {
    if (x < grid->lo_x || x > grid->hi_x || y < grid->lo_y || y > grid->hi_y) return nullptr;
    if (layer > 1) return grid->cells + layer;
    return grid->cells + (y * grid->width + x);
}
"""


def self_check():
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp) / "control.cpp"
        source.write_text(CONTROL)
        verdicts = {}
        for level in ("-O0", "-O2"):
            obj = Path(tmp) / f"control{level}.o"
            command = ["clang++", "-arch", "arm64", "-c", level, "-g", str(source), "-o", str(obj)]
            subprocess.run(command, check=True)
            print(f"control {level}:")
            verdicts[level] = report(str(obj), ["_control_get_cell"])
    ok = verdicts["-O0"] == {"O0-like"} and verdicts["-O2"] == {"optimised"}
    print("self-check:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("binaries", nargs="*")
    parser.add_argument("--symbol", action="append",
                        help="mangled symbol (default: the hot pathfinder and renderer ones)")
    parser.add_argument("--self-check", action="store_true")
    args = parser.parse_args()
    if args.self_check:
        return self_check()
    for binary in args.binaries:
        print(binary)
        report(binary, args.symbol or DEFAULT_SYMBOLS)
    return 0


if __name__ == "__main__":
    sys.exit(main())

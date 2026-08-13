#!/usr/bin/env python3
"""Prove that the LAN packet has one layout on every target.

`LANMessage` is `#pragma pack(1)` and is handed to `Transport::queueSend()` as raw bytes, so its
size and every member offset are the wire format. It used to hold `WideChar` (`wchar_t`) text, which
is 2 bytes with MSVC and 4 bytes on macOS/Linux: `sizeof(LANMessage)` was 471 bytes on Windows and
536 on LP64, over the 476 byte packet limit, and the `static_assert` in `LANAPI.h` failed 17
translation units in the native build.

This check compiles the header in four configurations - 32 and 64 bit, each with a 2 byte and a 4
byte `wchar_t` - and requires that

1. all four compile, i.e. the layout assertions carried by `LANAPI.h` hold in all of them;
2. all four agree on `sizeof(LANMessage)` and on the offset of every member, read out of clang's own
   record layout dump rather than out of an assertion;
3. a *poisoned* header, one that spells the text fields the pre-port way, FAILS - and fails inside
   the layout assertions. An assertion that cannot fire is not a test.

`-fshort-wchar` is how the MSVC width is reproduced on Linux; `-m32` is how the MSVC pointer width
is. Neither is how the game is built - they are here to make the four layouts comparable.

Usage:
    python3 scripts/ci/check-lanmessage-layout.py [--verbose]
"""

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import native_probe_targets as probe  # noqa: E402  (needs REPO_ROOT on the path first)

TEST_TU = """
#include "GameNetwork/LANAPI.h"

// In the test as well as in the header, so that a header that stopped asserting is still caught.
STATIC_ASSERT_ALWAYS(sizeof(LANMessage) == 471, "LANMessage must be 471 bytes on the wire");
STATIC_ASSERT_ALWAYS(sizeof(LANWireChar) == 2, "LANWireChar must be a 16 bit code unit");
"""

# The pre-port spelling of the text fields: WideChar, whose width follows the target.
POISONED_WIRE_STRING = """
#pragma once
#include "Lib/BaseType.h"
#include "Common/UnicodeString.h"
typedef WideChar LANWireChar;
void lanWireStringSet( LANWireChar *dest, Int destCount, const WideChar *src );
UnicodeString lanWireStringGet( const LANWireChar *src, Int srcCount );
"""

# Configurations that must all produce the same layout. 2 byte wchar_t is the MSVC width, 32 bit is
# the MSVC pointer width, so ("32", short wchar) is the layout the shipped game has.
CONFIGS = [
    (32, True, "32-bit, 2 byte wchar_t (the MSVC/VC6 layout)"),
    (32, False, "32-bit, 4 byte wchar_t"),
    (64, False, "64-bit, 4 byte wchar_t (native macOS/Linux)"),
    (64, True, "64-bit, 2 byte wchar_t"),
]

LAYOUT_LINE = re.compile(r"^\s*(\d+) \|\s+(?:struct|union|class)?\s*(.*)$")


def compile_flags(clangxx, bits, short_wchar, extra_includes, dump_layouts):
    target = [t for t in probe.TARGETS if t.name == "Core/GameEngine"][0]
    includes = [str(REPO_ROOT / "scripts" / "native-port-shims")]
    includes += probe.target_includes(target, probe.DEFAULT_DEPS_DIR)
    cmd = [
        clangxx, "-fsyntax-only", "-std=c++20", f"-m{bits}", "-ferror-limit=0",
        "-fms-extensions", "-w",
        "-include", "Utility/CppMacros.h",
        "-DWIN32_LEAN_AND_MEAN", "-D_REENTRANT", "-DRTS_ZEROHOUR=1",
    ]
    if short_wchar:
        cmd.append("-fshort-wchar")
    if dump_layouts:
        cmd += ["-Xclang", "-fdump-record-layouts"]
    for inc in list(extra_includes) + includes:
        cmd += ["-I", str(inc)]
    return cmd


def compile_tu(clangxx, tu, bits, short_wchar, extra_includes=(), dump_layouts=False,
               verbose=False):
    cmd = compile_flags(clangxx, bits, short_wchar, extra_includes, dump_layouts) + [str(tu)]
    if verbose:
        print("  $", " ".join(cmd))
    proc = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True)
    return proc.returncode, proc.stdout + proc.stderr


def lanmessage_layout(dump):
    """The `offset | member` pairs clang printed for LANMessage, plus its size."""
    blocks = dump.split("*** Dumping AST Record Layout")
    wanted = [b for b in blocks if re.search(r"^\s*0 \| struct LANMessage$", b, re.M)]
    if not wanted:
        return None, None
    block = wanted[-1]
    size = re.search(r"\[sizeof=(\d+)", block)
    members = []
    for line in block.splitlines():
        match = LAYOUT_LINE.match(line)
        if not match:
            continue
        offset, name = match.group(1), match.group(2).strip()
        # Anonymous struct and union members are named by source location, which differs between
        # runs of the same file only if the file moved; keep them, they still pin the offsets.
        name = re.sub(r"\(unnamed at [^)]*\)|\(anonymous at [^)]*\)", "<anonymous>", name)
        members.append((int(offset), name))
    return (int(size.group(1)) if size else None), members


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--clangxx", default="clang++-14", help="compiler to measure with")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    failures = []
    layouts = {}

    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        tu = tmp / "lanmessage_layout_test.cpp"
        tu.write_text(TEST_TU)

        for bits, short_wchar, label in CONFIGS:
            rc, out = compile_tu(args.clangxx, tu, bits, short_wchar, dump_layouts=True,
                                 verbose=args.verbose)
            size, members = lanmessage_layout(out)
            if rc != 0:
                failures.append(f"{label}: the layout assertions do not hold")
                print(f"[FAIL] {label}")
                print("\n".join(line for line in out.splitlines() if "error:" in line)[:4000])
                continue
            if size is None:
                failures.append(f"{label}: clang printed no layout for LANMessage")
                print(f"[FAIL] {label}: no record layout dumped")
                continue
            layouts[label] = (size, members)
            print(f"[ ok ] {label}: sizeof(LANMessage) = {size}")

        if len(layouts) == len(CONFIGS):
            reference_label = CONFIGS[0][2]
            reference = layouts[reference_label]
            for label, layout in layouts.items():
                if layout[0] != reference[0]:
                    failures.append(f"{label}: sizeof(LANMessage) is {layout[0]}, "
                                    f"{reference[0]} in the {reference_label} build")
                elif layout[1] != reference[1]:
                    differing = [(a, b) for a, b in zip(layout[1], reference[1]) if a != b]
                    failures.append(f"{label}: member offsets differ from the {reference_label} "
                                    f"build, first at {differing[0] if differing else 'unknown'}")
            if not failures:
                print(f"[ ok ] all {len(CONFIGS)} configurations agree on the layout, "
                      f"{len(reference[1])} members")

        # The negative control. A shadow include directory whose LANWireString.h uses WideChar, so
        # that the text fields go back to following the target's wchar_t width.
        poison = tmp / "poison" / "GameNetwork"
        poison.mkdir(parents=True)
        (poison / "LANWireString.h").write_text(POISONED_WIRE_STRING)
        rc, out = compile_tu(args.clangxx, tu, 64, False, extra_includes=(poison.parent,),
                             verbose=args.verbose)
        assertion_errors = out.count("static_assert failed")
        if rc == 0:
            failures.append("negative control compiled; the layout assertions are inert")
            print("[FAIL] negative control: WideChar text fields compiled clean at 64 bits")
        elif assertion_errors == 0:
            failures.append("negative control failed, but not in a layout assertion")
            print("[FAIL] negative control failed for the wrong reason:")
            print(out[:4000])
        else:
            print(f"[ ok ] negative control fails in {assertion_errors} layout assertions")

    if failures:
        print("\nFAILED:\n  " + "\n  ".join(failures))
        return 1
    print("\nLANMessage has one layout on every target.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

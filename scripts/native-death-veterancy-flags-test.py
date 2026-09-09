#!/usr/bin/env python3
"""Build and run the bit-assignment test for DeathTypeFlags and VeterancyLevelFlags.

Both flag sets put enumerator `e` on bit `e - 1`, and both start at zero -- DEATH_NORMAL and
LEVEL_REGULAR -- so for those two the shift count is -1. That is undefined behaviour, and the two
platforms disagree about it in a way no compile and no link can see:

  * 32-bit retail: `unsigned long` is 32 bits, the shift count truncates to 5 bits, and the flag
    lands on bit 31, which DEATH_TYPE_FLAGS_ALL (0xffffffff) has set. The gate passes.
  * LP64 (macOS, Linux): `unsigned long` is 64 bits, the count truncates to 6 bits instead, and
    `1UL << 63` has no bit inside a 32-bit mask. The gate rejects EVERY normal death, so no die
    module body runs -- nothing is destroyed and corpses keep a live AI.

Windows is the oracle for this code: it is in the determinism path, and the replay gate
(`Replay Check GeneralsMD`) compares against Windows. So the fix reproduces the 32-bit assignment
rather than choosing a tidier one, and this test asserts that assignment literally: bit 31 for the
zero-valued enumerator, bit (e - 1) for the rest, get/set/clear mutually consistent, and no two
enumerators sharing a bit -- which is what makes INI's `DeathTypes = ALL -CRUSHED` mean what it
says. See docs/porting/death-flag-shift.md.

The test is header-only on purpose: it includes the real `Common/GameCommon.h` and
`GameLogic/Damage.h` and links nothing, so it can run anywhere the headers parse, including on
Windows, where it passes both before and after the fix. On LP64 it fails before the fix.

Usage:
    python3 scripts/native-death-veterancy-flags-test.py [--keep] [--verbose]

Exits non-zero if the headers do not compile or an assertion fails. `CLANGXX` selects the
compiler, as it does for native-build.py and the other standalone checks.
"""

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
CLANGXX = os.environ.get("CLANGXX", "clang++")

SOURCE = "Core/GameEngine/Source/Common/System/tests/death_veterancy_flags_test.cpp"

# Zero Hour's include set, because Zero Hour is what the replay gate replays. `Common/BitFlags.h`
# is the only per-game header Damage.h reaches, and the two headers under test are Core's, shared
# by both games.
INCLUDES = [
    "scripts/native-port-shims",
    "Dependencies/Utility",
    "Core/Libraries/Include",
    "Core/Libraries/Source/WWVegas",
    "Core/Libraries/Source/WWVegas/WWLib",
    "Core/Libraries/Source/WWVegas/WWDebug",
    "Core/GameEngine/Include",
    "GeneralsMD/Code/GameEngine/Include",
]

COMPILE_FLAGS = [
    "-std=c++20",
    "-g",
    "-O0",
    "-fms-extensions",
    "-include", "Utility/CppMacros.h",
    "-DWIN32_LEAN_AND_MEAN",
    "-D_REENTRANT",
    "-DRTS_ZEROHOUR=1",
]


def run(command, verbose, **kwargs):
    if verbose:
        print("+", " ".join(str(part) for part in command))
    return subprocess.run(command, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--keep", action="store_true",
                        help="keep the build directory instead of removing it")
    parser.add_argument("--verbose", action="store_true", help="echo the commands")
    args = parser.parse_args()

    build_dir = pathlib.Path(tempfile.mkdtemp(prefix="death-veterancy-flags-test-"))
    try:
        binary = build_dir / "death_veterancy_flags_test"
        command = [CLANGXX] + COMPILE_FLAGS
        for include in INCLUDES:
            command += ["-isystem", str(REPO_ROOT / include)]
        command += [str(REPO_ROOT / SOURCE), "-o", str(binary)]

        result = run(command, args.verbose, cwd=REPO_ROOT)
        if result.returncode != 0:
            print("FAILED: the death/veterancy flag headers did not compile")
            return result.returncode

        result = run([str(binary)], args.verbose)
        if result.returncode != 0:
            print("FAILED: the death/veterancy flag bits are not the ones 32-bit Windows assigns")
            return result.returncode

        print("OK: DEATH_NORMAL and LEVEL_REGULAR keep the bit 32-bit Windows gives them")
        return 0
    finally:
        if args.keep:
            print(f"build directory kept at {build_dir}")
        else:
            shutil.rmtree(build_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

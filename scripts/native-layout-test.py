#!/usr/bin/env python3
"""Compile the engine's on-disk structure definitions with a native 64-bit clang and check
their layout.

Three checks, all of which must pass:

1. **native 64-bit** - `chunkio.h`, `iostruct.h` and `w3d_file.h` compile with
   `clang++ -m64`, which means the `STATIC_ASSERT_ALWAYS` layout assertions in
   `w3d_file_layout.h` (and the width assertions in `bittype.h`) hold on LP64.
2. **32-bit reference** - the same headers compile with `clang++ -m32`. ILP32 reproduces the
   Windows/VC6 layout of these structures, so a build that passes both is a build whose
   64-bit layout equals the layout retail assets were written with.
3. **negative control** - the same headers compiled against a *poisoned* `bittype.h`, one
   that spells the types the way the pre-port header did (`typedef unsigned long uint32`),
   must FAIL, and must fail inside the layout assertions. Without this the first two checks
   prove nothing: an assertion that cannot fire is not a test.

Requires clang and 32-bit libstdc++ headers (`g++-multilib` on Debian/Ubuntu). Check 2 is
skipped, with a warning, if -m32 is unavailable; checks 1 and 3 are not skippable.

Usage:
    python3 scripts/native-layout-test.py [--verbose]

`CLANGXX` selects the compiler, as it does for `native-port-probe.py` and `native-build.py`.
"""

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent

# Same knob as the probe and the native build, so all three can be pointed at one pinned compiler.
CXX = os.environ.get("CLANGXX", "clang++")

INCLUDES = [
    "Dependencies/Utility",
    "Core/Libraries/Include",
    "resources/gitinfo",
    "Core/Libraries/Source/WWVegas",
    "Core/Libraries/Source/WWVegas/WWLib",
    "Core/Libraries/Source/WWVegas/WWMath",
    "Core/Libraries/Source/WWVegas/WWDebug",
    "GeneralsMD/Code/Libraries/Source/WWVegas",
]

TEST_TU = """
#include "WWLib/chunkio.h"
#include "WWLib/iostruct.h"
#include "WW3D2/w3d_file.h"

// Spot checks in the test itself, in addition to the assertions carried by the headers.
STATIC_ASSERT_ALWAYS(sizeof(ChunkHeader) == 8, "ChunkHeader must be 8 bytes");
STATIC_ASSERT_ALWAYS(sizeof(W3dChunkHeader) == 8, "W3dChunkHeader must be 8 bytes");
STATIC_ASSERT_ALWAYS(sizeof(W3dMeshHeader3Struct) == 116, "W3dMeshHeader3Struct must be 116 bytes");
STATIC_ASSERT_ALWAYS(sizeof(W3dHierarchyStruct) == 36, "W3dHierarchyStruct must be 36 bytes");

int main() { return 0; }
"""

# The pre-port spelling of the LP64-sensitive typedefs, used only by the negative control.
POISONED_BITTYPE = """
#pragma once
#include <Utility/CppMacros.h>
typedef unsigned char  uint8;
typedef unsigned short uint16;
typedef unsigned long  uint32;
typedef unsigned int   uint;
typedef signed char    sint8;
typedef signed short   sint16;
typedef signed long    sint32;
typedef signed int     sint;
typedef float          float32;
typedef double         float64;
typedef unsigned long  DWORD;
typedef unsigned short WORD;
typedef unsigned char  BYTE;
typedef int            BOOL;
typedef unsigned short USHORT;
typedef const char *   LPCSTR;
typedef unsigned int   UINT;
typedef unsigned long  ULONG;
"""


def compile_tu(tu_path, bits, extra_includes=(), verbose=False):
    cmd = [
        CXX, "-std=c++17", "-fsyntax-only", f"-m{bits}", "-ferror-limit=0",
        "-include", "Utility/CppMacros.h",
        str(tu_path),
    ]
    for inc in list(extra_includes) + INCLUDES:
        cmd += ["-I", str(inc if pathlib.Path(inc).is_absolute() else REPO_ROOT / inc)]
    if verbose:
        print("  $", " ".join(cmd))
    proc = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True)
    return proc.returncode, proc.stdout + proc.stderr


def multilib_available(tmp, verbose=False):
    """Whether -m32 has libstdc++ headers, asked directly instead of pattern-matched out of a
    failure. The diagnostic differs between compilers and versions, and guessing it wrong turns a
    missing g++-multilib into a reported layout failure."""
    probe = tmp / "multilib_probe.cpp"
    probe.write_text("#include <utility>\nint main() { return 0; }\n")
    cmd = [CXX, "-std=c++17", "-fsyntax-only", "-m32", str(probe)]
    if verbose:
        print("  $", " ".join(cmd))
    return subprocess.run(cmd, capture_output=True, text=True).returncode == 0


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    failures = []
    with tempfile.TemporaryDirectory() as tmp:
        tmp = pathlib.Path(tmp)
        tu = tmp / "layout_test.cpp"
        tu.write_text(TEST_TU)

        print("[1/3] native 64-bit layout ...")
        rc, out = compile_tu(tu, 64, verbose=args.verbose)
        if rc == 0:
            print("      PASS - on-disk structures have their documented layout on LP64")
        else:
            failures.append("64-bit layout check failed")
            print(out)

        print("[2/3] 32-bit reference layout ...")
        rc, out = compile_tu(tu, 32, verbose=args.verbose)
        if rc == 0:
            print("      PASS - identical assertions hold under ILP32 (the Windows layout)")
        elif not multilib_available(tmp, verbose=args.verbose):
            print("      SKIP - no 32-bit libstdc++ headers (install g++-multilib)")
        else:
            failures.append("32-bit layout check failed")
            print(out)

        print("[3/3] negative control: poisoned bittype.h must fail ...")
        # A shadow WWLib whose files are symlinks to the real ones, except bittype.h. Quoted
        # includes resolve next to the including file, so the shadow directory has to carry
        # the whole of WWLib for the poisoned header to be the one that gets picked up.
        poison = tmp / "poison"
        (poison / "WWLib").mkdir(parents=True)
        for f in (REPO_ROOT / "Core/Libraries/Source/WWVegas/WWLib").iterdir():
            if f.is_file() and f.name != "bittype.h":
                (poison / "WWLib" / f.name).symlink_to(f)
        (poison / "WWLib" / "bittype.h").write_text(POISONED_BITTYPE)
        rc, out = compile_tu(tu, 64, extra_includes=(poison, poison / "WWLib"), verbose=args.verbose)
        assertion_errors = out.count("error: static_assert failed")
        if rc == 0:
            failures.append("negative control compiled - the assertions cannot fire")
            print("      FAIL - poisoned build compiled clean; the assertions are inert")
        elif assertion_errors == 0:
            failures.append("negative control failed for the wrong reason")
            print("      FAIL - poisoned build failed, but not in a layout assertion:")
            print(out[:4000])
        else:
            print(f"      PASS - poisoned build fails in {assertion_errors} layout assertions")
            if args.verbose:
                print(out[:4000])

    if failures:
        print("\nFAILED: " + "; ".join(failures))
        return 1
    print("\nAll layout checks passed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

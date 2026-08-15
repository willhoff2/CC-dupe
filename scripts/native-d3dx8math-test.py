#!/usr/bin/env python3
"""Build and run the numerical test for the D3DX 8 math entry points.

Core/Libraries/Source/WWVegas/WWMath/d3dx8math.cpp implements the D3DX math functions the renderer
names -- the water renderer, the point-group renderer and the sorting renderer -- over WWMath off
Windows, where on Windows they come from d3dx8.lib. Every one of them is a *convention*: D3DX puts
the vector on the left (v' = v * M) and the translation in the fourth row, Matrix4x4 does the
opposite, and an implementation with the operands or the transpose the wrong way round compiles,
links and renders a subtly wrong picture. The native build cannot see that, so it is not the check
that matters here.

This links the implementation against WWMath/tests/d3dx8math_test.cpp, which asserts the D3DX
behaviour against the definition: the multiply is A * B on the stored elements and not (B * A)^T,
scaling and translation land in the row the row-vector convention puts them in, an inverse composed
with its original is the identity within tolerance, a singular matrix returns null and leaves its
output untouched, and the determinant out-parameter may be null.

Usage:
    python3 scripts/native-d3dx8math-test.py [--keep] [--verbose]

Exits non-zero if the implementation does not compile, does not link, or an assertion fails.
`CLANGXX` selects the compiler, as it does for native-build.py and the other standalone checks.
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

WWMATH_DIR = "Core/Libraries/Source/WWVegas/WWMath"

# The implementation, the WWMath translation unit that defines the two _D3DMATRIX conversions the
# implementation's contract is built on (To_Matrix4x4 and To_D3DMATRIX -- everything else it uses is
# header-inline), and the test.
SOURCES = [
    f"{WWMATH_DIR}/d3dx8math.cpp",
    f"{WWMATH_DIR}/matrix4.cpp",
    f"{WWMATH_DIR}/tests/d3dx8math_test.cpp",
]

# scripts/native-port-shims is what supplies <d3dx8math.h> and <d3d8types.h> off Windows. Without
# it the implementation is a different declaration set and the test would not be testing it.
INCLUDES = [
    "scripts/native-port-shims",
    "Dependencies/Utility",
    "Core/Libraries/Include",
    "Core/Libraries/Source/WWVegas",
    WWMATH_DIR,
]

COMPILE_FLAGS = [
    "-std=c++20",
    "-m64",
    "-g",
    "-O0",
    "-fms-extensions",
    "-include", "Utility/CppMacros.h",
    "-DWIN32_LEAN_AND_MEAN",
    "-D_REENTRANT",
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

    if sys.platform == "win32":
        print("On Windows these entry points are d3dx8.lib's and d3dx8math.cpp is empty; build "
              "WWMath/tests/d3dx8math_test.cpp against d3dx8.lib instead.")
        return 1

    build_dir = pathlib.Path(tempfile.mkdtemp(prefix="d3dx8math-test-"))
    try:
        binary = build_dir / "d3dx8math_test"
        command = [CLANGXX] + COMPILE_FLAGS
        for include in INCLUDES:
            command += ["-I", str(REPO_ROOT / include)]
        command += [str(REPO_ROOT / source) for source in SOURCES]
        command += ["-o", str(binary)]

        result = run(command, args.verbose, cwd=REPO_ROOT)
        if result.returncode != 0:
            print("FAILED: the D3DX math implementation did not compile or link")
            return result.returncode

        result = run([str(binary)], args.verbose)
        if result.returncode != 0:
            print("FAILED: the D3DX math entry points do not match D3DX's conventions")
            return result.returncode

        print("OK: the D3DX 8 math entry points match D3DX's row-vector conventions")
        return 0
    finally:
        if args.keep:
            print(f"build directory kept at {build_dir}")
        else:
            shutil.rmtree(build_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

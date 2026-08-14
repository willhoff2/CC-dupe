#!/usr/bin/env python3
"""Build and run the behaviour test for the Win32 file API seam.

The native build proves the seam links. It says nothing about whether FindFirstFile() enumerates
what the asset scan expects, whether "*." means "no extension", or whether GetFullPathName()
honours Win32's two-call buffer protocol -- all of which were argued from documentation and call
sites rather than observed. This links the seam against a test that asserts those answers on a real
directory tree. See docs/porting/win32-file-api-seam.md.

Usage:
    python3 scripts/native-win32-file-test.py [--keep] [--verbose]

Exits non-zero if the seam does not compile, does not link, or the assertions fail.
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

PLATFORM_DIR = "Core/Libraries/Source/WWVegas/WWLib/platform"

# The seam's file half, the path resolver it calls through, and the test. The locale and module
# halves are separate translation units with no bearing on these assertions.
SOURCES = [
    f"{PLATFORM_DIR}/platform_win32_file.cpp",
    f"{PLATFORM_DIR}/platform_path.cpp",
    "Core/Libraries/Source/WWVegas/WWLib/wwstring.cpp",
    f"{PLATFORM_DIR}/tests/win32_file_api_test.cpp",
]

# scripts/native-port-shims is what supplies <windows.h>: the seam compiles to nothing without it
# (platform_win32_compat.h checks for the header), so the test would link against an empty
# translation unit and pass vacuously.
INCLUDES = [
    "scripts/native-port-shims",
    "Dependencies/Utility",
    "Core/Libraries/Include",
    "Core/Libraries/Source/WWVegas",
    "Core/Libraries/Source/WWVegas/WWLib",
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
        print("This script builds the non-Windows seam; on Windows the real API is the "
              "implementation. Build the test through CMake instead.")
        return 1

    build_dir = pathlib.Path(tempfile.mkdtemp(prefix="win32-file-test-"))
    try:
        command = [CLANGXX] + COMPILE_FLAGS
        for include in INCLUDES:
            command += ["-I", str(REPO_ROOT / include)]
        command += [str(REPO_ROOT / source) for source in SOURCES]
        command += ["-o", str(build_dir / "win32_file_api_test")]

        result = run(command, args.verbose, cwd=REPO_ROOT)
        if result.returncode != 0:
            print("FAILED: the seam did not compile or link")
            return result.returncode

        result = run([str(build_dir / "win32_file_api_test")], args.verbose)
        if result.returncode != 0:
            print("FAILED: the seam's behaviour does not match Win32")
            return result.returncode

        print("OK: the Win32 file API seam behaves as the call sites expect")
        return 0
    finally:
        if args.keep:
            print(f"build directory kept at {build_dir}")
        else:
            shutil.rmtree(build_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

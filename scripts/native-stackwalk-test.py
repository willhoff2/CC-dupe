#!/usr/bin/env python3
"""Build and run the native stack walk test for the EA/Debug library.

The debug library's stack walker is DbgHelp on Windows and backtrace()/dladdr() off it. "It
compiles" and "it produces frames that name my functions" are different claims, and only the
second one helps when a native build crashes, so this script links the library for real and runs
a test that asserts on the symbolised frames. See docs/porting/debug-and-profile-libs.md.

Usage:
    python3 scripts/native-stackwalk-test.py [--keep] [--verbose]

Exits non-zero if the library does not compile, does not link, or the test's assertions fail.
"""

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
CLANGXX = os.environ.get("CLANGXX", "clang++")

DEBUG_DIR = "Core/Libraries/Source/debug"

# The translation units core_debug builds, plus the non-Windows platform implementation and the
# test itself. Kept in sync with Core/Libraries/Source/debug/CMakeLists.txt by
# scripts/ci/check-stackwalk-symbols.py, which reads the CMake list.
SOURCES = [
    f"{DEBUG_DIR}/debug_cmd.cpp",
    f"{DEBUG_DIR}/debug_debug.cpp",
    f"{DEBUG_DIR}/debug_except.cpp",
    f"{DEBUG_DIR}/debug_getdefaultcommands.cpp",
    f"{DEBUG_DIR}/debug_internal.cpp",
    f"{DEBUG_DIR}/debug_io_con.cpp",
    f"{DEBUG_DIR}/debug_io_flat.cpp",
    f"{DEBUG_DIR}/debug_io_net.cpp",
    f"{DEBUG_DIR}/debug_io_ods.cpp",
    f"{DEBUG_DIR}/debug_stack.cpp",
    f"{DEBUG_DIR}/platform/debug_platform.cpp",
    f"{DEBUG_DIR}/tests/native_stackwalk_test.cpp",
]

INCLUDES = [
    "Dependencies/Utility",
    "Core/Libraries/Include",
    "resources/gitinfo",
    "Core/Libraries/Source/WWVegas",
    "Core/Libraries/Source/WWVegas/WWLib",
    "Core/Libraries/Source/debug",
]

# -g and -fno-omit-frame-pointer are what a debuggable native build uses, and the point of the
# test is to check the stack walk under those conditions. -fms-extensions and the CppMacros
# force-include mirror the MSVC build, as the probe does.
COMPILE_FLAGS = [
    "-std=c++20",
    "-m64",
    "-g",
    "-O1",
    "-fno-omit-frame-pointer",
    "-fms-extensions",
    "-include", "Utility/CppMacros.h",
    "-DWIN32_LEAN_AND_MEAN",
    "-D_REENTRANT",
]


def link_flags():
    """Flags that make a function's name reachable from dladdr() at run time."""
    if sys.platform == "darwin":
        # Mach-O keeps the symbol table dladdr() reads without a flag; libdl is in libSystem.
        return []
    return ["-rdynamic", "-ldl"]


def run(cmd, verbose):
    if verbose:
        print("+", " ".join(str(c) for c in cmd))
    proc = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stdout.write(proc.stdout)
        sys.stderr.write(proc.stderr)
    return proc


def build(work_dir, verbose):
    """Compile and link the test binary. Returns its path, or None on failure."""
    includes = [f"-I{REPO_ROOT / inc}" for inc in INCLUDES]
    objects = []
    for source in SOURCES:
        obj = work_dir / (source.replace("/", "_") + ".o")
        cmd = [CLANGXX, *COMPILE_FLAGS, *includes, "-c", str(REPO_ROOT / source), "-o", str(obj)]
        if run(cmd, verbose).returncode != 0:
            print(f"FAIL: {source} does not compile natively")
            return None
        objects.append(obj)

    binary = work_dir / "native_stackwalk_test"
    cmd = [CLANGXX, "-m64", *[str(o) for o in objects], *link_flags(), "-o", str(binary)]
    if run(cmd, verbose).returncode != 0:
        print("FAIL: the debug library does not link natively")
        return None
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--keep", action="store_true",
                        help="keep the build directory (printed on exit)")
    parser.add_argument("--verbose", action="store_true", help="echo every command")
    args = parser.parse_args()

    work_dir = pathlib.Path(tempfile.mkdtemp(prefix="native-stackwalk-"))
    try:
        binary = build(work_dir, args.verbose)
        if binary is None:
            return 1

        print(f"linked {len(SOURCES)} translation units, running:")
        proc = subprocess.run([str(binary)], cwd=REPO_ROOT, capture_output=True, text=True)
        sys.stdout.write(proc.stdout)
        sys.stderr.write(proc.stderr)
        if proc.returncode != 0:
            print(f"FAIL: the native stack walk test exited {proc.returncode}")
            return 1
        return 0
    finally:
        if args.keep:
            print(f"build directory: {work_dir}")
        else:
            subprocess.run(["rm", "-rf", str(work_dir)], check=False)


if __name__ == "__main__":
    sys.exit(main())

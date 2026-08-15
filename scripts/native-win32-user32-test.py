#!/usr/bin/env python3
"""Build and run the behaviour test for the user32 cursor, window-state and window/GDI seam.

GetCursorPos(), ScreenToClient(), SetCursor(), IsIconic(), and the window-sizing group
GetClientRect()/GetWindowLongA()/AdjustWindowRect()/MonitorFromWindow()/GetMonitorInfoA()/
SetWindowPos()/GetDesktopWindow()/SetDeviceGammaRamp() are defined off Windows in
platform_win32_user.cpp over the window seam. The native build proves they link; it says nothing
about whether ScreenToClient() subtracts in the right direction, whether SetCursor(nullptr) hides
the system pointer or shows it, whether AdjustWindowRect() and SetWindowPos() are inverses (they
have to be: DX8Wrapper sizes the render window by composing them), or whether the coordinates
arrive unscaled -- and W3DMouse::draw() draws the game's cursor at the result of the first two, so a
sign error moves the cursor and a missed hide leaves two of them on screen. None of that is visible
in a link, and the points/pixels half of it is not visible on the CI runner either, whose backing
scale is 1 where a Retina display's is 2. See docs/porting/decisions-resolved.md and
docs/porting/window-gdi-seam.md.

The window seam is faked in the test (SDL and Cocoa both need a display server, and CI has none),
so what this checks is the translation layer, which is the part that was written here.

Usage:
    python3 scripts/native-win32-user32-test.py [--keep] [--verbose]

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

# The user32 half of the seam, the dialog and last-error bodies its other entry points call
# through, and the test -- which supplies the window seam itself.
SOURCES = [
    f"{PLATFORM_DIR}/platform_win32_user.cpp",
    f"{PLATFORM_DIR}/platform_win32_kernel.cpp",
    f"{PLATFORM_DIR}/platform_win32_file.cpp",
    f"{PLATFORM_DIR}/platform_path.cpp",
    f"{PLATFORM_DIR}/platform_dialog.cpp",
    f"{PLATFORM_DIR}/platform_mutex.cpp",
    f"{PLATFORM_DIR}/platform_thread.cpp",
    "Core/Libraries/Source/WWVegas/WWLib/wwstring.cpp",
    f"{PLATFORM_DIR}/tests/win32_user32_test.cpp",
]

# scripts/native-port-shims is what supplies <windows.h>: without it platform_win32_compat.h
# compiles the whole layer to nothing and the test would link against an empty translation unit
# and pass vacuously.
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
        print("This script builds the non-Windows seam; on Windows user32 is the implementation.")
        return 1

    build_dir = pathlib.Path(tempfile.mkdtemp(prefix="win32-user32-test-"))
    try:
        binary = build_dir / "win32_user32_test"
        command = [CLANGXX] + COMPILE_FLAGS
        for include in INCLUDES:
            command += ["-I", str(REPO_ROOT / include)]
        command += [str(REPO_ROOT / source) for source in SOURCES]
        command += ["-o", str(binary), "-lpthread"]

        if run(command, args.verbose, cwd=REPO_ROOT).returncode != 0:
            print("FAILED: the seam did not compile or link")
            return 1

        if run([str(binary)], args.verbose).returncode != 0:
            print("FAILED: the user32 cursor, window-state and window/GDI seam does not behave as "
                  "the call sites expect")
            return 1

        print("OK: the user32 cursor, window-state and window/GDI seam behaves as the call sites "
              "expect")
        return 0
    finally:
        if args.keep:
            print(f"build directory kept at {build_dir}")
        else:
            shutil.rmtree(build_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

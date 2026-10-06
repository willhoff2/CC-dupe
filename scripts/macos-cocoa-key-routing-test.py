#!/usr/bin/env python3
"""Build and run the Cocoa key-routing test against the real window pump.

Compiles platform_window_cocoa.mm with Core/Libraries/Source/WWVegas/WWLib/platform/tests/
cocoa_key_routing_test.mm and runs it in a windowed and a fullscreen (borderless) window. The test
posts Escape, the arrows and a few other keys as NSEvents through Window_Pump and asserts that each
reaches the seam's queue and that none falls off AppKit's responder chain into
-[NSResponder noResponderFor:], which is where the system alert sound comes from. Cmd-Q must still
reach the main menu. Each key and mouse event's Time_Ms must be on timeGetTime()'s clock, the one
Keyboard::checkKeyRepeat() measures a hold against. See docs/porting/cocoa-key-routing.md and
docs/porting/event-clock.md.

It needs a login session with a display: the window has to become key for AppKit to route keys to
it at all. Without one the test exits 77 and this script reports SKIP (exit 0), unless
--require-display is given. Opening the window takes keyboard focus from whatever else is running.

Usage:
    python3 scripts/macos-cocoa-key-routing-test.py [--require-display] [--windowed-only] [--keep]
"""

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
CLANGXX = os.environ.get("CLANGXX", "clang++")
PLATFORM_DIR = "Core/Libraries/Source/WWVegas/WWLib/platform"
UTILITY_DIR = "Dependencies/Utility"
SOURCES = [
    f"{PLATFORM_DIR}/platform_window_cocoa.mm",
    f"{PLATFORM_DIR}/tests/cocoa_key_routing_test.mm",
]
FRAMEWORKS = ["AppKit", "QuartzCore", "Metal", "CoreGraphics", "Foundation"]
SKIP_EXIT = 77


def build(work_dir):
    binary = work_dir / "cocoa_key_routing_test"
    command = [CLANGXX, "-std=c++20", "-ObjC++", "-Wall", "-Wextra",
               "-Wno-missing-field-initializers", f"-I{REPO_ROOT / PLATFORM_DIR}",
               f"-I{REPO_ROOT / UTILITY_DIR}",
               *[str(REPO_ROOT / source) for source in SOURCES],
               *[flag for name in FRAMEWORKS for flag in ("-framework", name)],
               "-o", str(binary)]
    result = subprocess.run(command, cwd=REPO_ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        return None
    return binary


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--require-display", action="store_true",
                        help="fail instead of skipping when the window cannot become key")
    parser.add_argument("--windowed-only", action="store_true",
                        help="skip the fullscreen case, which covers the whole screen briefly")
    parser.add_argument("--keep", action="store_true", help="keep the build directory")
    args = parser.parse_args()
    if sys.platform != "darwin":
        raise SystemExit("this test drives the Cocoa backend and needs macOS")

    work_dir = pathlib.Path(tempfile.mkdtemp(prefix="cocoa-key-routing-"))
    try:
        binary = build(work_dir)
        if binary is None:
            print("FAIL: the Cocoa key-routing test does not build")
            return 1
        command = [str(binary)] + (["--windowed-only"] if args.windowed_only else [])
        result = subprocess.run(command, cwd=REPO_ROOT)
        if result.returncode == SKIP_EXIT:
            print("SKIP: no windowing session; key routing is UNVERIFIED by this run")
            return 1 if args.require_display else 0
        if result.returncode != 0:
            print(f"FAIL: the Cocoa key-routing test exited {result.returncode}")
            return 1
        print("PASS: keys reach the seam's queue with no AppKit beep path, Cmd-Q still quits, "
              "event stamps are on timeGetTime()'s clock")
        return 0
    finally:
        if args.keep:
            print(f"build directory: {work_dir}")
        else:
            subprocess.run(["rm", "-rf", str(work_dir)], check=False)


if __name__ == "__main__":
    sys.exit(main())

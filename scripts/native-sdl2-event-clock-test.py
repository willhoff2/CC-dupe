#!/usr/bin/env python3
"""Build and run the SDL2 window backend's event-clock test.

Compiles platform_window_sdl2.cpp with Core/Libraries/Source/WWVegas/WWLib/platform/tests/
sdl2_event_clock_test.cpp, opens the backend's window, pushes key and mouse events through SDL's
queue and asserts that each queued WindowEvent's Time_Ms is on timeGetTime()'s clock -- the one
Keyboard::checkKeyRepeat() measures a key's hold against -- and keeps the event's age. SDL stamps
events with SDL_GetTicks() (milliseconds since SDL_Init), so a raw stamp makes every key repeat at
once. See docs/porting/event-clock.md.

The window needs an X server; with no DISPLAY the test runs under `xvfb-run -a` when it is
installed. Without either it reports SKIP (exit 0) unless --require-display is given. The backend
creates its window with SDL_WINDOW_VULKAN, so libvulkan must be loadable.

Usage:
    python3 scripts/native-sdl2-event-clock-test.py [--require-display] [--keep]
"""

import argparse
import os
import pathlib
import shlex
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
CLANGXX = os.environ.get("CLANGXX", "clang++")
PLATFORM_DIR = "Core/Libraries/Source/WWVegas/WWLib/platform"
UTILITY_DIR = "Dependencies/Utility"
SOURCES = [
    f"{PLATFORM_DIR}/platform_window_sdl2.cpp",
    f"{PLATFORM_DIR}/tests/sdl2_event_clock_test.cpp",
]
SKIP_EXIT = 77


def sdl2_flags():
    result = subprocess.run(["sdl2-config", "--cflags", "--libs"], capture_output=True,
                            text=True, check=True)
    return shlex.split(result.stdout)


def build(work_dir):
    binary = work_dir / "sdl2_event_clock_test"
    command = [CLANGXX, "-std=c++20", "-Wall", "-Wextra", "-Wno-missing-field-initializers",
               f"-I{REPO_ROOT / PLATFORM_DIR}", f"-I{REPO_ROOT / UTILITY_DIR}",
               *[str(REPO_ROOT / source) for source in SOURCES],
               *sdl2_flags(), "-o", str(binary)]
    result = subprocess.run(command, cwd=REPO_ROOT, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        return None
    return binary


def run_command(binary):
    if os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY"):
        return [str(binary)]
    if shutil.which("xvfb-run"):
        return ["xvfb-run", "-a", "--server-args=-screen 0 1280x1024x24", str(binary)]
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--require-display", action="store_true",
                        help="fail instead of skipping when no window can be opened")
    parser.add_argument("--keep", action="store_true", help="keep the build directory")
    args = parser.parse_args()
    if shutil.which("sdl2-config") is None:
        raise SystemExit("sdl2-config not found; install libsdl2-dev")

    work_dir = pathlib.Path(tempfile.mkdtemp(prefix="sdl2-event-clock-"))
    try:
        binary = build(work_dir)
        if binary is None:
            print("FAIL: the SDL2 event-clock test does not build")
            return 1
        command = run_command(binary)
        if command is None:
            returncode = SKIP_EXIT
        else:
            returncode = subprocess.run(command, cwd=REPO_ROOT).returncode
        if returncode == SKIP_EXIT:
            print("SKIP: no display to open the window on; the event clock is UNVERIFIED here")
            return 1 if args.require_display else 0
        if returncode != 0:
            print(f"FAIL: the SDL2 event-clock test exited {returncode}")
            return 1
        print("PASS: SDL2 event stamps are on timeGetTime()'s clock and keep their age")
        return 0
    finally:
        if args.keep:
            print(f"build directory: {work_dir}")
        else:
            shutil.rmtree(work_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

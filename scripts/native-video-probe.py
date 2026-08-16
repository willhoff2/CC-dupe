#!/usr/bin/env python3
"""Drive the engine's own FFmpeg video path over a real movie file, natively, at 64-bit.

This is a probe, not a port fix. It links `scripts/native-video-probe/video_decode_probe.cpp`
against the *real* build's `FFmpegFile.cpp` object and archives, so the measurement is of the
engine's code compiled exactly as `scripts/native-build.py` compiles it -- the flags come out of
`build/native/compile_commands.json` rather than being restated here, because a probe that
configures the code differently from the build measures something else.

Usage:

    python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 --with-shims \\
        --strict-link                          # produces build/native/, once
    python3 scripts/native-video-probe.py Data/Movies/EA_Logo.bik

What it reports is in docs/porting/video-path-findings.md. Movie files are retail data: never
commit one, and never commit this probe's output frames.
"""

import argparse
import json
import os
import pathlib
import shlex
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD_DIR = REPO_ROOT / "build" / "native"
COMPILE_DB = BUILD_DIR / "compile_commands.json"
PROBE_SRC = REPO_ROOT / "scripts" / "native-video-probe" / "video_decode_probe.cpp"
FFMPEG_LIB_DIR = REPO_ROOT / "build" / "docker" / "_deps" / "ffmpeg-lib" / "lib"

# The engine objects the probe links, out of the native build: the video translation unit under
# test, and the abstract File it takes. Deliberately nothing else -- pulling the engine archives in
# instead drags in the download manager and the Win32 window shims, none of which the video path
# touches. The handful of symbols File.cpp still wants are defined, and made to abort when reached,
# in video_decode_probe.cpp.
ENGINE_TU = "Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegFile.cpp"
ENGINE_OBJECTS = ["FFmpegFile.cpp.o", "File.cpp.o", "AsciiString.cpp.o", "UnicodeString.cpp.o"]
FFMPEG_LIBS = ["libavformat", "libavcodec", "libswscale", "libswresample", "libavutil"]


def die(message):
    print(f"error: {message}", file=sys.stderr)
    sys.exit(2)


def compile_flags():
    """The real build's flags for the video translation unit, minus its output arguments."""
    if not COMPILE_DB.exists():
        die(f"{COMPILE_DB} not found; run scripts/native-build.py first "
            "(see this file's docstring)")
    entries = json.loads(COMPILE_DB.read_text())
    entry = next((e for e in entries if e["file"].endswith("FFmpegFile.cpp")), None)
    if entry is None:
        die(f"{COMPILE_DB} has no entry for {ENGINE_TU}")

    argv = shlex.split(entry["command"])
    flags, skip = [], 0
    for index, arg in enumerate(argv):
        if skip:
            skip -= 1
            continue
        if index == 0 or arg == "-c":
            continue
        if arg == "-o":
            skip = 1
            continue
        if arg.endswith("FFmpegFile.cpp"):
            continue
        flags.append(arg)
    return argv[0], flags


def engine_objects():
    objects = []
    for name in ENGINE_OBJECTS:
        matches = [m for m in BUILD_DIR.rglob(name) if m.name == name]
        if not matches:
            die(f"build/native has no {name}; the build did not compile it")
        objects.append(str(matches[0]))
    return objects


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("movie", nargs="?", help="movie file to open (retail data; not committed)")
    parser.add_argument("--png", help="write the first converted frame here, as PNG")
    parser.add_argument("--build-only", action="store_true", help="build the probe and stop")
    args = parser.parse_args()

    if not args.build_only and args.movie is None:
        parser.error("a movie file is required unless --build-only is given")

    clangxx, flags = compile_flags()
    binary = BUILD_DIR / "native_video_probe"
    objects = engine_objects()

    ffmpeg_libs = []
    for stem in FFMPEG_LIBS:
        candidates = sorted(FFMPEG_LIB_DIR.glob(f"{stem}.so.*"))
        if not candidates:
            die(f"{stem} not in {FFMPEG_LIB_DIR}; run ./scripts/ci/fetch-probe-deps.sh")
        ffmpeg_libs.append(str(candidates[0]))

    command = [clangxx, *flags, str(PROBE_SRC), *objects,
               *ffmpeg_libs, "-lm", "-lpthread", "-ldl",
               f"-Wl,-rpath,{FFMPEG_LIB_DIR}", "-o", str(binary)]
    print("== building the probe with the native build's own flags")
    result = subprocess.run(command, cwd=REPO_ROOT)
    if result.returncode != 0:
        print("== probe build FAILED (this is itself a finding; see the diagnostics above)")
        return result.returncode
    print(f"   {binary}")

    if args.build_only:
        return 0

    movie = pathlib.Path(args.movie)
    if not movie.exists():
        die(f"{movie} not found")
    print(f"== running the probe over {movie.name}")
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = os.pathsep.join(
        [str(FFMPEG_LIB_DIR), env.get("LD_LIBRARY_PATH", "")])
    run = [str(binary), str(movie)] + ([args.png] if args.png else [])
    return subprocess.run(run, cwd=REPO_ROOT, env=env).returncode


if __name__ == "__main__":
    sys.exit(main())

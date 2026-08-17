#!/usr/bin/env python3
"""Measure what the engine's video path still lacks, over real movies, and classify each finding.

This is the third half of the video slice: the pixel proof
(`scripts/native-video-frame-run.py`) shows that a decoded retail frame reaches the framebuffer
through the existing texture seam, and this shows what is still wrong with *playing* one. Every
number below comes out of engine code executed over a real Bink file --
`native_video_frame_run --measure-gaps` runs the engine's `FFmpegFile` accessors, the real
`FFmpegVideoStream`, its own `isFrameReady()` gate spun exactly the way `LoadScreen.cpp` spins it,
and its own `frameGoto()` -- and is then compared against an independent `ffprobe` reading of the
same file. No render device is created, so this is cheap enough to run over the whole retail
inventory.

What it decides, per movie:

* `frameCount()` (`duration x avg_frame_rate`, truncated) against the frames the decoder will
  actually deliver. `Display::update()` and both load-screen movie loops stop at
  `frameCount() - 1`, so this is how many frames of the movie the game never shows.
* `getFrameTime()` (`1000u / fps`, integer) against the true frame period, as drift in
  milliseconds per frame and over the whole movie. This is the audio-sync budget: there is no
  audio clock anywhere on the path, so the video clock's error is the whole error.
* The interval the gate actually let frames through at, measured, against that same true period.
* `frameCount() / FRAME_FUDGE_ADD`, which `LoadScreen.cpp` divides the frame index by -- zero for
  any movie under 30 frames, which is a division by zero and not a cosmetic issue.
* What `frameGoto()` does, which the min-spec load-screen path depends on.
* Whether the file has an audio track at all, next to whether the build defines the macro every
  audio branch in `FFmpegVideoPlayer.cpp` sits behind (read out of the real build's
  `compile_commands.json`, not assumed).

Usage:

    python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \\
        --with-shims --strict-link              # must run first: this uses its archives
    python3 scripts/video-path-gaps.py --movie A.bik [--movie B.bik | --movie-dir DIR] \\
        [--pacing-frames 60] [--json out.json]

Movies are data: never commit one, and nothing this writes belongs in the repository either.
"""

import argparse
import importlib.util
import json
import pathlib
import re
import shutil
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD_DIR = REPO_ROOT / "build" / "native"
# The macro every audio branch in FFmpegVideoPlayer.cpp is behind, and the file whose real compile
# command is inspected for it.
AUDIO_MACRO = "RTS_USE_OPENAL"
AUDIO_TRANSLATION_UNIT = "FFmpegVideoPlayer.cpp"
# LoadScreen.cpp's own constant, the one the frame index is taken modulo of.
FRAME_FUDGE_ADD = 30


def load_module(name):
    path = REPO_ROOT / "scripts" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build_harness(validation=False):
    """Compile and link the same harness the pixel proof uses, with the real build's flags."""
    video = load_module("native-video-frame-run")
    runner = video.load_render_runner()
    scratch = BUILD_DIR / "video-path-gaps"
    if scratch.exists():
        shutil.rmtree(scratch)
    scratch.mkdir(parents=True)
    print("== compiling the harness with the engine's own flags")
    runner.compile_harness(scratch / "native_video_frame_run.o", harness=video.HARNESS,
                           donor=video.FLAG_DONOR, extra_includes=video.EXTRA_INCLUDES)
    print("== copying the archives")
    archives = runner.scratch_archives(scratch)
    print("== linking")
    binary = scratch / "native_video_frame_run"
    runner.link_harness([scratch / "native_video_frame_run.o"], binary, archives)
    environment, _ = runner.run_environment(validation=validation)
    return binary, archives, environment


def audio_macro_defined():
    """Is the movie-audio macro defined in the compile command the real build used?

    Read, not assumed: `compile_commands.json` is what `scripts/native-build.py` actually passed to
    the compiler for the translation unit that contains every audio branch.
    """
    database = BUILD_DIR / "compile_commands.json"
    if not database.is_file():
        return None
    for entry in json.loads(database.read_text()):
        if entry.get("file", "").endswith(AUDIO_TRANSLATION_UNIT):
            command = entry.get("command") or " ".join(entry.get("arguments", []))
            return f"-D{AUDIO_MACRO}" in command
    return None


def ffprobe_truth(movie):
    """The independent reading of the same file: frame rate, duration and a real frame count.

    `-count_frames` walks the stream, so `nb_read_frames` is a count and not a product of duration
    and rate -- which is the arithmetic the engine's `getNumFrames()` is being checked against.
    """
    result = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-count_frames",
         "-show_entries", "stream=avg_frame_rate,r_frame_rate,nb_read_frames,width,height",
         "-show_entries", "format=duration", "-of", "json", str(movie)],
        capture_output=True, text=True, check=True)
    data = json.loads(result.stdout)
    stream = data["streams"][0]
    numerator, denominator = (int(part) for part in stream["avg_frame_rate"].split("/"))
    fps = numerator / denominator if denominator else 0.0
    return {
        "fps": fps,
        "frames": int(stream["nb_read_frames"]),
        "width": int(stream["width"]),
        "height": int(stream["height"]),
        "duration": float(data["format"]["duration"]),
    }


def run_harness(binary, environment, movie, pacing_frames):
    """`--measure-gaps` over one movie, parsed into the numbers the engine reported."""
    result = subprocess.run([str(binary), "--movie", str(movie), "--measure-gaps",
                             "--pacing-frames", str(pacing_frames)],
                            capture_output=True, text=True, env=environment, cwd=REPO_ROOT)
    text = result.stdout
    if result.returncode != 0:
        return None, text + result.stderr

    def number(pattern, cast=int):
        match = re.search(pattern, text, re.MULTILINE)
        return cast(match.group(1)) if match else None

    pacing = re.search(r"^pacing((?: \d+)*)$", text, re.MULTILINE)
    audio = re.search(r"^audio (\d+) channels (\d+) sampleRate (\d+) bytesPerSample (\d+)$",
                      text, re.MULTILINE)
    goto = re.search(r"^frameGoto (-?\d+) -> frameIndex (-?\d+) -> after 4x frameNext (-?\d+)$",
                     text, re.MULTILINE)
    return {
        "frameCount": number(r"^frameCount (\d+)$"),
        "frameTime": number(r"^frameTime (\d+)$"),
        "progressUpdateCount": number(r"^progressUpdateCount (\d+)$"),
        "firstFrameIndex": number(r"^firstFrameIndex (-?\d+)$"),
        "pacedFrames": number(r"^pacedFrames (\d+)"),
        "gateWaits": number(r"gateWaits (\d+)"),
        "framesDecodable": number(r"^framesDecodable (\d+)"),
        "lastPacedFrameIndex": number(r"lastFrameIndex (\d+)"),
        "readyAt": [int(value) for value in pacing.group(1).split()] if pacing else [],
        "hasAudio": bool(int(audio.group(1))) if audio else None,
        "audioChannels": int(audio.group(2)) if audio else None,
        "audioSampleRate": int(audio.group(3)) if audio else None,
        "frameGoto": {"asked": int(goto.group(1)), "index": int(goto.group(2)),
                      "afterFourNext": int(goto.group(3))} if goto else None,
    }, text


def measure(movie, engine, truth, audio_built):
    """Turn the two readings into the findings, each with its arithmetic shown."""
    findings = []
    true_period = 1000.0 / truth["fps"] if truth["fps"] else 0.0

    missing = truth["frames"] - engine["frameCount"]
    findings.append({
        "gap": "frameCount() truncation",
        "kind": "pre-existing engine defect",
        "measured": f"frameCount() {engine['frameCount']}, frames in the file {truth['frames']}, "
                    f"frames the engine's own decoder delivered {engine['framesDecodable']}, "
                    f"so {missing} never shown (the loops stop at frameCount() - 1)",
        "bad": missing != 0,
    })

    drift_per_frame = true_period - engine["frameTime"]
    total_drift = drift_per_frame * truth["frames"]
    findings.append({
        "gap": "getFrameTime() integer truncation",
        "kind": "pre-existing engine defect",
        "measured": f"true period {true_period:.4f} ms at {truth['fps']:.4f} fps, getFrameTime() "
                    f"{engine['frameTime']} ms, so {drift_per_frame:+.4f} ms/frame and "
                    f"{total_drift:+.1f} ms ({total_drift / 1000.0:+.2f} s) over the movie",
        "bad": abs(total_drift) > 1.0,
    })

    ready = engine["readyAt"]
    if len(ready) >= 3:
        spans = [b - a for a, b in zip(ready, ready[1:])]
        observed = sum(spans) / len(spans)
        findings.append({
            "gap": "the pace frames were actually released at",
            "kind": "measurement",
            "measured": f"{len(ready)} frames through isFrameReady(), mean interval "
                        f"{observed:.2f} ms (min {min(spans)}, max {max(spans)}), "
                        f"true period {true_period:.4f} ms, {engine['gateWaits']} 1 ms waits",
            "bad": False,
        })

    findings.append({
        "gap": "LoadScreen.cpp's progress modulus",
        "kind": "pre-existing engine defect",
        "measured": f"frameCount() / {FRAME_FUDGE_ADD} = {engine['progressUpdateCount']}"
                    + (" -> `frame % progressUpdateCount` divides by zero"
                       if engine["progressUpdateCount"] == 0 else ""),
        "bad": engine["progressUpdateCount"] == 0,
    })

    goto = engine["frameGoto"]
    if goto is not None:
        landed = goto["index"] == goto["asked"]
        survived = goto["afterFourNext"] >= goto["index"] + 4
        findings.append({
            "gap": "frameGoto() / seek",
            "kind": "unimplemented path",
            "measured": f"frameGoto({goto['asked']}) left frameIndex() at {goto['index']}, and "
                        f"four frameNext() calls after it the index is {goto['afterFourNext']}"
                        + ("" if landed else " -- the stream did not land on the frame asked for")
                        + ("" if survived else " and stopped delivering frames"),
            "bad": not landed or not survived,
        })

    if engine["hasAudio"]:
        findings.append({
            "gap": "movie audio",
            "kind": "unimplemented path",
            "measured": f"the file carries {engine['audioChannels']} channels at "
                        f"{engine['audioSampleRate']} Hz; the build "
                        + ("defines" if audio_built else "does not define")
                        + f" {AUDIO_MACRO}, so every audio branch in FFmpegVideoPlayer.cpp is "
                          "compiled out and the movie plays silent",
            "bad": not audio_built,
        })

    findings.append({
        "gap": "audio/video synchronisation",
        "kind": "unimplemented path",
        "measured": "there is no audio clock on the path at all: isFrameReady() compares "
                    "system_clock against frameTime x frameIndex, and nothing reads the audio "
                    f"stream's position, so the {abs(total_drift) / 1000.0:.2f} s of clock error "
                    "above is uncorrected by construction",
        "bad": True,
    })

    findings.append({
        "gap": "the clock isFrameReady() uses",
        "kind": "pre-existing engine defect",
        "measured": "std::chrono::system_clock, which is not monotonic: an NTP step or a user "
                    "clock change during a movie moves the deadline for every remaining frame",
        "bad": True,
    })

    return findings


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--movie", action="append", default=[], help="a .bik to measure")
    parser.add_argument("--movie-dir", help="measure every .bik under this directory")
    parser.add_argument("--pacing-frames", type=int, default=60,
                        help="how many frames to let the isFrameReady() gate release in real time")
    parser.add_argument("--json", help="write the measurements out as JSON")
    parser.add_argument("--keep", action="store_true", help="keep the scratch archives")
    args = parser.parse_args()

    movies = [pathlib.Path(name).resolve() for name in args.movie]
    if args.movie_dir:
        movies += sorted(path.resolve() for path in
                         pathlib.Path(args.movie_dir).rglob("*")
                         if path.suffix.lower() == ".bik")
    if not movies:
        sys.exit("no movies: pass --movie or --movie-dir")
    for movie in movies:
        if not movie.is_file():
            sys.exit(f"{movie} is not a file")

    binary, archives, environment = build_harness()
    audio_built = audio_macro_defined()
    print(f"\n{AUDIO_MACRO} in the real build's {AUDIO_TRANSLATION_UNIT} command: "
          f"{'defined' if audio_built else 'not defined' if audio_built is False else 'unknown'}")

    report = {"audioMacroDefined": audio_built, "movies": []}
    failures = 0
    for movie in movies:
        print(f"\n== {movie.name}")
        engine, output = run_harness(binary, environment, movie, args.pacing_frames)
        if engine is None:
            print(output)
            print("   FAILED: the harness did not measure this movie")
            failures += 1
            continue
        truth = ffprobe_truth(movie)
        print(f"   {truth['width']}x{truth['height']}, {truth['fps']:.4f} fps, "
              f"{truth['duration']:.3f} s, {truth['frames']} frames counted by ffprobe")
        findings = measure(movie, engine, truth, audio_built)
        for finding in findings:
            print(f"   [{'GAP ' if finding['bad'] else '    '}] {finding['gap']}: "
                  f"{finding['measured']}")
        report["movies"].append({"movie": movie.name, "engine": engine, "ffprobe": truth,
                                 "findings": findings})

    if not args.keep:
        for archive in archives:
            archive.unlink()

    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(report, indent=2) + "\n")
        print(f"\nwrote {args.json}")

    print(f"\nmovies measured: {len(report['movies'])}, movies that could not be measured: "
          f"{failures}")
    # A gap is the output, not an error: this script fails only when a movie could not be measured.
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Run the engine's decode -> texture -> draw path over a Bink file and compare the frame it drew
against an independently decoded reference.

`Core/GameEngineDevice/Source/VideoDevice/FFmpeg/tests/native_video_frame_run.cpp` drives the
engine's own `FFmpegVideoPlayer::createStream`, `FFmpegVideoStream::frameRender`,
`W3DVideoBuffer::lock`/`unlock` (i.e. `TextureClass::Get_Surface_Level` + `SurfaceClass::Lock`) and
the `Render2DClass` quad `W3DDisplay::drawVideoBuffer` draws, then reads the colour target back and
writes each frame out as a PNG. This script gives it an executable -- with the flags and archives
`scripts/native-build.py` produced, borrowed from `scripts/native-render-backend-run.py` so the two
harnesses cannot drift -- and then does the part a harness must not be trusted to do for itself:
decide whether the picture is the movie's picture.

The comparison is deliberately not "does it look like a video":

* The reference frames are decoded by a **different** FFmpeg (the system `ffmpeg` binary, not the
  pinned libraries the engine links) and the YUV -> RGB conversion is redone here in integer
  arithmetic straight out of BT.601, so a swscale-side colour bug cannot cancel out.
* Three controls run against every frame: the same reference with R and B swapped, the same
  reference flipped vertically, and the same reference shifted by one row. A wrong channel order, a
  flipped origin and a wrong row pitch all produce a picture that looks approximately right, and
  each of them makes exactly one of these controls fit better than the straight comparison. The run
  fails unless the straight comparison is the best one by a wide margin.
* Frame-to-frame movement is checked too, because a texture that was uploaded once and then never
  updated would match frame 0 forever.

Usage:

    python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \\
        --with-shims --strict-link          # must run first: this uses its archives
    python3 scripts/native-video-frame-run.py --movie /path/to/movie.bik [--frames 3] \\
        [--format X8R8G8B8] [--validation] [--keep] [--out-dir DIR]

Movie files are data, not source: never commit one, and never commit the frames this writes.
Retail Zero Hour movies live outside the `.big` archives (docs/porting/replay-check-gamedata.md),
so a run over one of those is the retail claim; a run over any other Bink file measures the path
but not the inventory, and the output says which it was.
"""

import argparse
import importlib.util
import pathlib
import shutil
import subprocess
import sys

import numpy

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD_DIR = REPO_ROOT / "build" / "native"
HARNESS = (REPO_ROOT
           / "Core/GameEngineDevice/Source/VideoDevice/FFmpeg/tests/native_video_frame_run.cpp")
# The translation unit whose compile command is reused. W3DDisplay.cpp is the game's own video-draw
# site, so its include set already covers the video player, W3DVideoBuffer, WW3D2 and FFmpeg; only
# the renderer spike's headers (which vulkanrenderbackend.h includes) have to be added.
FLAG_DONOR = "W3DDisplay.cpp"
EXTRA_INCLUDES = ("spikes/renderer/src",)
# How close the drawn frame has to be to the reference, and how much worse every control has to be.
# Not zero: the engine converts with swscale's bicubic scaler into a D3D8-shaped texture format and
# the reference is integer BT.601 out of a different FFmpeg, so a couple of levels per channel is
# arithmetic, not a defect. A channel swap, a flip or a pitch error costs tens of levels.
MAX_MEAN_DELTA = 6.0
CONTROL_MARGIN = 3.0


def load_render_runner():
    """The render harness's script, imported: it owns the compile/link/run recipe."""
    path = REPO_ROOT / "scripts" / "native-render-backend-run.py"
    spec = importlib.util.spec_from_file_location("native_render_backend_run", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def reference_frames(movie, count, start=0):
    """Decode the first `count` frames with the system ffmpeg and convert them here.

    Two independent conversions of the same decode:

    * `swscale`, by asking the ffmpeg binary for rgb24 -- a different build of the same library the
      engine uses, which catches a mistake in *how the engine drives* swscale (pitch, format,
      height, plane pointers) but not a mistake inside swscale itself.
    * BT.601 limited-range arithmetic done here on the yuv420p planes, which shares nothing with the
      engine's path at all and is what makes a colour-space claim possible.
    """
    probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0",
                            "-show_entries", "stream=width,height", "-of", "csv=p=0", str(movie)],
                           capture_output=True, text=True, check=True)
    width, height = (int(value) for value in probe.stdout.strip().split(","))

    def raw(pixel_format):
        # Decoded from the start and then sliced, not seeked: seeking a Bink stream is a different
        # operation from the sequential decode the harness performs, and the frame numbering has to
        # be the same on both sides for a delta to mean anything.
        proc = subprocess.run(["ffmpeg", "-v", "error", "-i", str(movie),
                               "-frames:v", str(start + count),
                               "-pix_fmt", pixel_format, "-f", "rawvideo", "-"],
                              capture_output=True, check=True)
        return numpy.frombuffer(proc.stdout, dtype=numpy.uint8)

    rgb = raw("rgb24").reshape(-1, height, width, 3)[start:]

    planar = raw("yuv420p")
    frame_bytes = width * height * 3 // 2
    frames = planar.size // frame_bytes
    planar = planar[:frames * frame_bytes].reshape(frames, frame_bytes)
    luma = planar[:, :width * height].reshape(frames, height, width).astype(numpy.int32)
    chroma_size = (width // 2) * (height // 2)
    blue = planar[:, width * height:width * height + chroma_size]
    red = planar[:, width * height + chroma_size:]
    blue = blue.reshape(frames, height // 2, width // 2).astype(numpy.int32)
    red = red.reshape(frames, height // 2, width // 2).astype(numpy.int32)
    # Nearest-neighbour upsample, which is what makes this independent: no swscale filter kernel is
    # reproduced, so the comparison tolerates a chroma-edge difference and still pins the colours.
    blue = numpy.repeat(numpy.repeat(blue, 2, axis=1), 2, axis=2)[start:, :height, :width]
    red = numpy.repeat(numpy.repeat(red, 2, axis=1), 2, axis=2)[start:, :height, :width]
    luma = luma[start:]

    # BT.601, limited range (Bink's yuv420p is limited range and swscale treats it as such).
    y = (luma - 16) * 298
    u = blue - 128
    v = red - 128
    bt601 = numpy.empty(luma.shape + (3,), dtype=numpy.int32)
    bt601[..., 0] = (y + 409 * v + 128) >> 8
    bt601[..., 1] = (y - 100 * u - 208 * v + 128) >> 8
    bt601[..., 2] = (y + 516 * u + 128) >> 8
    return width, height, rgb, numpy.clip(bt601, 0, 255).astype(numpy.uint8)


def read_png(path):
    from PIL import Image
    with Image.open(path) as image:
        return numpy.asarray(image.convert("RGB"), dtype=numpy.uint8)


def delta(a, b):
    """Mean and max absolute per-channel difference between two same-shaped RGB images."""
    difference = numpy.abs(a.astype(numpy.int32) - b.astype(numpy.int32))
    return float(difference.mean()), int(difference.max())


def row_skew(reference):
    """What a row pitch taken as the wrong number of texels actually produces.

    Not a one-row shift: a pitch off by n texels displaces row y by y*n, so the image shears,
    and each row is off by a different amount.  A one-row shift is a poor control on video --
    consecutive rows of real footage are similar, so shifting by one is often *less* wrong than
    the honest sampling error -- while a shear is unmistakable.
    """
    return numpy.stack([numpy.roll(reference[y], y, axis=0) for y in range(reference.shape[0])])


def compare(drawn, reference):
    """The straight comparison plus the three wrong-image controls."""
    controls = {
        "channels swapped (R<->B)": reference[..., ::-1],
        "flipped vertically": reference[::-1, ...],
        "row skew (wrong row pitch)": row_skew(reference),
    }
    result = {"straight": delta(drawn, reference)}
    for name, image in controls.items():
        result[name] = delta(drawn, image)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--movie", required=True, help="the .bik to play")
    parser.add_argument("--frames", type=int, default=3, help="how many frames to draw")
    parser.add_argument("--start-frame", type=int, default=0,
                        help="walk the stream forward this many frames first. Logos open on black, "
                             "and a black frame drawn correctly cannot be told apart from nothing "
                             "drawn at all")
    parser.add_argument("--format", help="force a VideoBuffer format (R8G8B8, X8R8G8B8, R5G6B5, "
                                         "X1R5G5B5) instead of the one the display would pick")
    parser.add_argument("--validation", action="store_true",
                        help="ask the backend for the Vulkan validation layer")
    parser.add_argument("--seek-probe", action="store_true",
                        help="ask the stream to frameGoto() and report what it answered")
    parser.add_argument("--keep", action="store_true", help="keep the scratch link directory")
    parser.add_argument("--out-dir", default="/tmp/zh-video-frames",
                        help="where the drawn frames are written (not for committing)")
    args = parser.parse_args()

    movie = pathlib.Path(args.movie).resolve()
    if not movie.is_file():
        sys.exit(f"{movie} is not a file")
    out_dir = pathlib.Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    runner = load_render_runner()
    scratch = BUILD_DIR / "video-frame-run"
    if scratch.exists():
        shutil.rmtree(scratch)
    scratch.mkdir(parents=True)

    print("== compiling the harness with the engine's own flags")
    runner.compile_harness(scratch / "native_video_frame_run.o", harness=HARNESS,
                           donor=FLAG_DONOR, extra_includes=EXTRA_INCLUDES)

    print("== copying the archives")
    archives = runner.scratch_archives(scratch)
    runner.check_swapchain_compiled_in(archives)

    print("== linking")
    binary = scratch / "native_video_frame_run"
    runner.link_harness([scratch / "native_video_frame_run.o"], binary, archives)
    print(f"   {binary.relative_to(REPO_ROOT)}")

    print(f"== running over {movie.name}")
    environment, _ = runner.run_environment(validation=args.validation)
    prefix = out_dir / "drawn"
    command = [str(binary), "--movie", str(movie), "--frames", str(args.frames),
               "--start-frame", str(args.start_frame), "--png-prefix", str(prefix)]
    if args.format:
        command += ["--format", args.format]
    if args.seek_probe:
        command.append("--seek-probe")
    result = subprocess.run(command, env=environment, cwd=REPO_ROOT)
    if not args.keep:
        for archive in archives:
            archive.unlink()
    print(f"\nharness exit code: {result.returncode}")
    if result.returncode != 0:
        return result.returncode

    print(f"\n== the reference decode (system ffmpeg, {shutil.which('ffmpeg')})")
    width, height, swscale_rgb, bt601_rgb = reference_frames(movie, args.frames,
                                                             start=args.start_frame)
    print(f"   {width}x{height}, {swscale_rgb.shape[0]} frames decoded independently")

    failures = 0
    previous = None
    for frame in range(args.frames):
        drawn_path = pathlib.Path(f"{prefix}-frame{frame}.png")
        if not drawn_path.is_file():
            print(f"frame {frame}: the harness wrote no PNG")
            failures += 1
            continue
        drawn = read_png(drawn_path)
        if drawn.shape[:2] != (height, width):
            # Not a tolerance question: a readback of a different size means the quad was not the
            # 1:1 mapping the comparison assumes, and comparing anyway would invent a number.
            print(f"frame {frame}: readback is {drawn.shape[1]}x{drawn.shape[0]}, the movie is "
                  f"{width}x{height}; refusing to compare resampled pixels")
            failures += 1
            continue

        print(f"\nframe {frame}: {drawn_path}")
        for name, reference in (("swscale rgb24", swscale_rgb[frame]),
                                ("BT.601 integer", bt601_rgb[frame])):
            measurements = compare(drawn, reference)
            straight_mean, straight_max = measurements["straight"]
            print(f"  vs {name}: mean |delta| {straight_mean:.2f}, max {straight_max}")
            for control, (mean, maximum) in measurements.items():
                if control == "straight":
                    continue
                print(f"      control {control:<26} mean |delta| {mean:.2f}, max {maximum}")
            if straight_mean > MAX_MEAN_DELTA:
                print(f"      FAILED: mean delta {straight_mean:.2f} > {MAX_MEAN_DELTA}")
                failures += 1
            for control, (mean, _) in measurements.items():
                if control == "straight":
                    continue
                if mean < straight_mean * CONTROL_MARGIN:
                    print(f"      FAILED: the '{control}' control is not "
                          f"{CONTROL_MARGIN}x worse ({mean:.2f} vs {straight_mean:.2f}), so this "
                          "comparison cannot tell them apart")
                    failures += 1

        if previous is not None:
            mean, maximum = delta(drawn, previous)
            _, reference_maximum = delta(swscale_rgb[frame], swscale_rgb[frame - 1])
            print(f"  vs the previous drawn frame: mean |delta| {mean:.2f}, max {maximum} "
                  f"(the movie's own consecutive frames differ by max {reference_maximum})")
            # Stale content is a drawn frame that did not move while the movie did.  Judging it
            # against the movie rather than against zero matters: a held frame -- and the loops
            # this engine plays behind menus hold for seconds at a time -- is genuinely identical,
            # and calling that a texture-update failure would be a wrong claim.
            if maximum == 0 and reference_maximum != 0:
                print("      FAILED: identical to the previous frame while the movie changed -- "
                      "the texture was not updated for this frame")
                failures += 1
        previous = drawn

    print(f"\ncomparison failures: {failures}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Play a Bink file through the engine's video player with the AIL/OpenAL shim as its sound sink,
capture what the shim rendered, and compare it against an independent ffmpeg decode.

`Core/GameEngineDevice/Source/VideoDevice/FFmpeg/tests/native_movie_audio_run.cpp` drives the
engine's own `FFmpegVideoPlayer::createStream` / `FFmpegVideoStream::frameNext` over a movie with
an audio manager that lends out a real AIL `HSAMPLE` the way `MilesAudioManager::getHandleForBink`
does, and prints the counts: audio frames the decoder produced, whether the stream took the handle,
the audio clock against the video clock at every frame, and whether the handle went back. This
script gives it an executable (the flags and archives `scripts/native-build.py` produced, via the
render harness's recipe so the harnesses cannot drift), points OpenAL Soft's wave writer at a file
so the rendered mix is a measurable artefact rather than a speaker, and then does the part the
harness must not be trusted to do for itself: decide whether the sound is the movie's sound.

The comparison:

* The reference is decoded by a **different** FFmpeg (the system `ffmpeg` binary, not the pinned
  libraries the engine links) to s16le at the movie's own rate. The engine's path converts the
  decoder's planar float to interleaved s16 the way libswresample does (round to nearest,
  saturate) and the device is opened at the movie's rate, so nothing is resampled on either side
  and the two can be compared sample for sample after one gain.
* The rendered file starts when the device opens, not when the movie starts, so the lag is found
  by cross-correlation and reported; the gain is the least-squares fit (the shim applies the
  speech volume the engine asks for) and reported; the residual after both is what has to be
  small. Silence, a wrong rate (drifting lag), a wrong channel order (per-channel gain fit fails)
  and a dropped buffer (residual spike) all fail it.
* The movie -> menu transition is checked by opening an AIL stream on the same driver afterwards
  and requiring it to play, and by requiring the pooled sample to hold nothing once released.

Usage:

    python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \\
        --with-shims --strict-link          # must run first: this uses its archives
    python3 scripts/native-movie-audio-run.py --movie /path/to/EA_LOGO.BIK \\
        [--pump-ms 1] [--json out.json] [--keep] [--out-dir DIR]

Movie files are data, not source: never commit one, and never commit the captures this writes.
Retail Zero Hour movies live outside the `.big` archives (docs/porting/replay-check-gamedata.md),
so a run over one of those is the retail claim; a run over any other Bink file measures the path
but not the inventory, and the output says which it was.
"""

import argparse
import importlib.util
import json
import pathlib
import platform
import re
import shutil
import struct
import subprocess
import sys
import wave

import numpy

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD_DIR = REPO_ROOT / "build" / "native"
HARNESS = (REPO_ROOT
           / "Core/GameEngineDevice/Source/VideoDevice/FFmpeg/tests/native_movie_audio_run.cpp")
# The video player's own compile command: its include set already covers the FFmpeg headers and
# the AIL header the player now uses.
FLAG_DONOR = "FFmpegVideoPlayer.cpp"
EXTRA_INCLUDES = ("spikes/renderer/src",)

# How much residual is allowed after lag and gain are fitted, relative to the reference's RMS.
# Not zero: the shim's gain is applied by OpenAL Soft's float mixer and the result is re-quantised
# to 16 bits, so one LSB of rounding per sample is arithmetic, not a defect. A dropped or repeated
# buffer costs the full signal energy over its span.
MAX_RESIDUAL_RATIO = 0.02
MIN_CORRELATION = 0.999
# A source that never played is silence; Bink's own intro tracks sit far above this.
MIN_RMS = 200.0


def load_render_runner():
    path = REPO_ROOT / "scripts" / "native-render-backend-run.py"
    spec = importlib.util.spec_from_file_location("native_render_backend_run", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def probe_audio(movie):
    proc = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries",
                           "stream=codec_name,sample_rate,channels,sample_fmt", "-of",
                           "default=noprint_wrappers=1", str(movie)],
                          capture_output=True, text=True, check=True)
    info = dict(line.split("=", 1) for line in proc.stdout.strip().splitlines() if "=" in line)
    if "sample_rate" not in info:
        return None
    return {"codec": info.get("codec_name"), "rate": int(info["sample_rate"]),
            "channels": int(info["channels"]), "sample_fmt": info.get("sample_fmt")}


def reference_pcm(movie, rate, channels):
    """The system ffmpeg's s16le decode at the movie's own rate: no resampling on this side."""
    proc = subprocess.run(["ffmpeg", "-v", "error", "-i", str(movie), "-vn", "-ar", str(rate),
                           "-ac", str(channels), "-f", "s16le", "-"],
                          capture_output=True, check=True)
    return numpy.frombuffer(proc.stdout, dtype=numpy.int16).reshape(-1, channels)


def write_alsoft_conf(work, capture, rate):
    """OpenAL Soft's wave writer, at the movie's rate so the shim resamples nothing either."""
    conf = work / "alsoft-movie.conf"
    conf.write_text(
        "[general]\n"
        "drivers=wave\n"
        f"frequency={rate}\n"
        "channels=stereo\n"
        "sample-type=int16\n"
        f"[wave]\nfile={capture}\n"
    )
    return conf


def write_music_wav(path, rate, seconds=2.0):
    """A synthetic tone: the menu-music route only has to be shown to play after the movie."""
    frames = int(rate * seconds)
    t = numpy.arange(frames) / rate
    tone = (numpy.sin(2 * numpy.pi * 440.0 * t) * 8000).astype(numpy.int16)
    stereo = numpy.stack([tone, tone], axis=1)
    with wave.open(str(path), "wb") as out:
        out.setnchannels(2)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(stereo.tobytes())


def read_wav(path):
    """OpenAL Soft writes WAVE_FORMAT_EXTENSIBLE, which the wave module refuses; walk the RIFF
    chunks by hand."""
    blob = path.read_bytes()
    if blob[:4] != b"RIFF" or blob[8:12] != b"WAVE":
        sys.exit(f"{path}: not a RIFF/WAVE file")
    offset = 12
    rate = channels = width = None
    data = None
    while offset + 8 <= len(blob):
        tag = blob[offset:offset + 4]
        size = struct.unpack("<I", blob[offset + 4:offset + 8])[0]
        body = blob[offset + 8:offset + 8 + size]
        if tag == b"fmt ":
            _, channels, rate, _, _, width = struct.unpack("<HHIIHH", body[:16])
        elif tag == b"data":
            data = body
        offset += 8 + size + (size & 1)
    if data is None or rate is None:
        sys.exit(f"{path}: no fmt/data chunk")
    if width != 16:
        sys.exit(f"{path}: expected 16-bit capture, got {width}-bit")
    frames = len(data) // (2 * channels)
    return rate, numpy.frombuffer(data[:frames * 2 * channels], dtype=numpy.int16).reshape(
        -1, channels)


def best_lag(rendered, reference):
    """Sample lag of the reference inside the rendered capture, by FFT cross-correlation of the
    mono sums. Positive: the movie starts `lag` samples into the capture."""
    a = rendered.astype(numpy.float64).sum(axis=1)
    b = reference.astype(numpy.float64).sum(axis=1)
    size = 1 << (len(a) + len(b) - 1).bit_length()
    fa = numpy.fft.rfft(a, size)
    fb = numpy.fft.rfft(b, size)
    correlation = numpy.fft.irfft(fa * numpy.conj(fb), size)
    lag = int(numpy.argmax(correlation))
    if lag > size // 2:
        lag -= size
    return lag


def compare(rendered, reference):
    """Lag, per-channel gain, correlation and residual between what the shim rendered and what
    ffmpeg decoded. Returns a dict of the measurements and whether they pass."""
    result = {"rendered_frames": int(rendered.shape[0]),
              "reference_frames": int(reference.shape[0])}
    rendered_rms = float(numpy.sqrt(numpy.mean(rendered.astype(numpy.float64) ** 2)))
    result["rendered_rms"] = rendered_rms
    result["reference_rms"] = float(numpy.sqrt(numpy.mean(reference.astype(numpy.float64) ** 2)))
    result["non_silent"] = rendered_rms >= MIN_RMS
    if not result["non_silent"] or reference.shape[0] == 0:
        result["pass"] = False
        return result

    lag = best_lag(rendered, reference)
    result["lag_frames"] = lag
    start = max(lag, 0)
    ref_start = max(-lag, 0)
    overlap = min(rendered.shape[0] - start, reference.shape[0] - ref_start)
    result["overlap_frames"] = int(overlap)
    result["overlap_of_reference"] = float(overlap / reference.shape[0])
    if overlap <= 0:
        result["pass"] = False
        return result
    r = rendered[start:start + overlap].astype(numpy.float64)
    x = reference[ref_start:ref_start + overlap].astype(numpy.float64)

    gains = []
    correlations = []
    residuals = []
    exact_fraction = []
    for channel in range(min(r.shape[1], x.shape[1])):
        xc, rc = x[:, channel], r[:, channel]
        denominator = float(numpy.dot(xc, xc))
        gain = float(numpy.dot(xc, rc) / denominator) if denominator else 0.0
        gains.append(gain)
        if numpy.std(xc) > 0 and numpy.std(rc) > 0:
            correlations.append(float(numpy.corrcoef(xc, rc)[0, 1]))
        else:
            correlations.append(0.0)
        residual = rc - gain * xc
        residuals.append(float(numpy.sqrt(numpy.mean(residual ** 2))))
        # After undoing the gain, how many samples land within one LSB of the reference: the
        # "sample-exact up to the mixer's rounding" statement.
        undone = numpy.rint(rc / gain) if gain else rc
        exact_fraction.append(float(numpy.mean(numpy.abs(undone - xc) <= 1.0)))
    result["gain_per_channel"] = gains
    result["correlation_per_channel"] = correlations
    result["residual_rms_per_channel"] = residuals
    result["within_one_lsb_fraction_per_channel"] = exact_fraction
    ref_rms = result["reference_rms"] or 1.0
    result["residual_ratio"] = max(residuals) / ref_rms
    result["pass"] = (min(correlations) >= MIN_CORRELATION
                      and result["residual_ratio"] <= MAX_RESIDUAL_RATIO
                      and result["overlap_of_reference"] >= 0.999)
    return result


def parse_harness(text):
    """The counts the harness printed, as numbers."""
    out = {}
    m = re.search(r"FFmpegFile audio stream: (\w+ ?\w*), (\d+) audio frames, (\d+) samples, "
                  r"(\d+) Hz, (\d+) channels; (\d+) video frames", text)
    if m:
        out["decoder_audio_stream_found"] = m.group(1) == "found"
        out["decoder_audio_frames"] = int(m.group(2))
        out["decoder_audio_samples"] = int(m.group(3))
        out["decoder_video_frames"] = int(m.group(6))
    m = re.search(r"getHandleForBink calls during createStream: (\d+); handle (\S+)", text)
    if m:
        out["getHandleForBink_calls_in_createStream"] = int(m.group(1))
        out["handle_non_null"] = m.group(2) == "non-null"
    m = re.search(r"frames delivered (\d+) of (\d+); audio loaded (\d+) ms, played (\d+) ms", text)
    if m:
        out["video_frames_delivered"] = int(m.group(1))
        out["video_frame_count"] = int(m.group(2))
        out["audio_loaded_ms"] = int(m.group(3))
        out["audio_played_ms_at_last_frame"] = int(m.group(4))
    m = re.search(r"A/V: worst \(video - audio\) (-?\d+) ms; frames beyond one video frame "
                  r"\((\d+) ms\) \+ pump: (\d+)", text)
    if m:
        out["av_worst_ms"] = int(m.group(1))
        out["video_frame_ms"] = int(m.group(2))
        out["av_frames_beyond_one_frame"] = int(m.group(3))
    m = re.search(r"pooled sample after release: (\d+) ms loaded", text)
    if m:
        out["pooled_sample_ms_after_release"] = int(m.group(1))
    m = re.search(r"music stream: (\d+) ms long, (\d+) ms played", text)
    if m:
        out["music_stream_played_ms"] = int(m.group(2))
    out["RTS_USE_OPENAL_defined"] = "RTS_USE_OPENAL defined" in text
    out["MSS_SAMPLE_BUFFER_API_defined"] = "MSS_SAMPLE_BUFFER_API defined" in text
    out["stages_failed"] = int(re.search(r"failures: (\d+)", text).group(1)) \
        if re.search(r"failures: (\d+)", text) else None
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--movie", required=True, help="the .bik to play")
    parser.add_argument("--pump-ms", type=int, default=1,
                        help="how often the harness pumps the stream (1 = LoadScreen's spin; "
                             "33 = Display::update once per engine frame)")
    parser.add_argument("--max-frames", type=int, default=0,
                        help="stop after this many video frames (0 = the whole movie)")
    parser.add_argument("--json", help="write the measurements here")
    parser.add_argument("--keep", action="store_true", help="keep the scratch link directory")
    parser.add_argument("--out-dir", default="/tmp/zh-movie-audio",
                        help="where the capture and reference are written (not for committing)")
    args = parser.parse_args()

    movie = pathlib.Path(args.movie).resolve()
    if not movie.is_file():
        sys.exit(f"{movie} is not a file")
    out_dir = pathlib.Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    audio = probe_audio(movie)
    print(f"== ffprobe: {audio}")
    if audio is None:
        sys.exit("the movie has no audio stream according to ffprobe; nothing to compare")
    rate, channels = audio["rate"], audio["channels"]

    runner = load_render_runner()
    scratch = BUILD_DIR / "movie-audio-run"
    if scratch.exists():
        shutil.rmtree(scratch)
    scratch.mkdir(parents=True)

    print("== compiling the harness with the engine's own flags")
    runner.compile_harness(scratch / "native_movie_audio_run.o", harness=HARNESS,
                           donor=FLAG_DONOR, extra_includes=EXTRA_INCLUDES)
    print("== copying the archives")
    archives = runner.scratch_archives(scratch)
    print("== linking")
    binary = scratch / "native_movie_audio_run"
    runner.link_harness([scratch / "native_movie_audio_run.o"], binary, archives)
    print(f"   {binary.relative_to(REPO_ROOT)}")

    capture = out_dir / f"{movie.stem}-rendered.wav"
    if capture.exists():
        capture.unlink()
    music = out_dir / "music-tone.wav"
    write_music_wav(music, rate)

    environment, _ = runner.run_environment(validation=False)
    environment["ALSOFT_CONF"] = str(write_alsoft_conf(out_dir, capture, rate))
    environment.pop("ALSOFT_DRIVERS", None)
    command = [str(binary), "--movie", str(movie), "--music", str(music),
               "--pump-ms", str(args.pump_ms)]
    if args.max_frames:
        command += ["--max-frames", str(args.max_frames)]
    print(f"== running over {movie.name} (pump every {args.pump_ms} ms)")
    result = subprocess.run(command, env=environment, cwd=REPO_ROOT, capture_output=True,
                            text=True)
    sys.stdout.write(result.stdout)
    sys.stderr.write(result.stderr)
    if not args.keep:
        for archive in archives:
            archive.unlink()
    print(f"\nharness exit code: {result.returncode}")
    measurements = {"movie": movie.name, "movie_audio": audio, "pump_ms": args.pump_ms,
                    "platform": f"{platform.system()} {platform.machine()}",
                    "harness": parse_harness(result.stdout),
                    "harness_exit_code": result.returncode}

    print(f"\n== the reference decode (system ffmpeg, {shutil.which('ffmpeg')})")
    reference = reference_pcm(movie, rate, channels)
    print(f"   {reference.shape[0]} frames at {rate} Hz, {channels} ch decoded independently")
    if not capture.is_file():
        print("the shim rendered no file: OpenAL Soft's wave writer did not run")
        measurements["pcm"] = {"pass": False, "rendered": None}
        failures = 1
    else:
        capture_rate, rendered = read_wav(capture)
        print(f"   rendered {rendered.shape[0]} frames at {capture_rate} Hz, "
              f"{rendered.shape[1]} ch: {capture}")
        if capture_rate != rate:
            sys.exit(f"capture rate {capture_rate} != movie rate {rate}; the comparison assumes "
                     "no resampling")
        # The engine closes the stream on its last video frame, which ends the sample and drops
        # whatever audio was still queued (Bink did the same), and the harness then opens the
        # music stream. So the comparable span is what the sample had played by the last frame;
        # the trimmed tail is reported, not hidden. The clock is read once per mixer period, so
        # a period is kept back as well.
        played_ms = measurements["harness"].get("audio_played_ms_at_last_frame", 0)
        comparable = max(int((played_ms - 25) * rate / 1000), 0)
        measurements["reference_frames_total"] = int(reference.shape[0])
        measurements["tail_dropped_at_close_ms"] = round(
            (reference.shape[0] - comparable) * 1000 / rate, 1)
        reference = reference[:comparable]
        print(f"   comparable span {comparable} frames ({played_ms} ms played at the last frame); "
              f"{measurements['tail_dropped_at_close_ms']} ms of the track is after the close")
        pcm = compare(rendered, reference)
        measurements["pcm"] = pcm
        print(f"   non-silent: {pcm['non_silent']} (rendered RMS {pcm['rendered_rms']:.1f}, "
              f"reference RMS {pcm['reference_rms']:.1f})")
        if "lag_frames" in pcm:
            print(f"   lag {pcm['lag_frames']} frames ({pcm['lag_frames'] * 1000 / rate:.1f} ms), "
                  f"overlap {pcm['overlap_of_reference']:.3f} of the reference")
            print(f"   gain per channel {['%.4f' % g for g in pcm['gain_per_channel']]}, "
                  f"correlation {['%.6f' % c for c in pcm['correlation_per_channel']]}")
            print(f"   residual RMS per channel "
                  f"{['%.2f' % r for r in pcm['residual_rms_per_channel']]} "
                  f"(ratio {pcm['residual_ratio']:.5f} of reference RMS, limit "
                  f"{MAX_RESIDUAL_RATIO}); within one LSB after undoing the gain: "
                  f"{['%.4f' % f for f in pcm['within_one_lsb_fraction_per_channel']]}")
        print(f"   PCM comparison: {'pass' if pcm['pass'] else 'FAILED'}")
        failures = (0 if pcm["pass"] else 1)
    failures += result.returncode
    measurements["verdict"] = "pass" if failures == 0 else "fail"

    if args.json:
        with open(args.json, "w") as handle:
            json.dump(measurements, handle, indent=2, sort_keys=True)
            handle.write("\n")
        print(f"\nwrote {args.json}")
    print(f"\nverdict: {measurements['verdict']}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

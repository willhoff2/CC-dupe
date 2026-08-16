#!/usr/bin/env python3
"""Build and run the behaviour probe for the audio path off Windows.

The audio numbers in docs/porting/ are all compile and link counts: 19/19 WWAudio translation
units, 101 declared == 101 defined AIL_* symbols, 0 unresolved in the strict link. None of them
say whether a device opens, whether a sound decodes, or whether anything is audible. This script
answers those questions by running the OpenAL Miles replacement through the same AIL_* sequences
MilesAudioManager.cpp drives, and by capturing the mixer's output so that "it played" is a
statement about samples rather than about return codes.

What it asserts, and how:

* the device, driver, listener and voice pool are observed through the public API only;
* IMA ADPCM decoding is compared against ffmpeg's independent decode of the same file, so a wrong
  decoder cannot pass by agreeing with itself;
* playback is captured with OpenAL Soft's wave writer (ALSOFT_CONF drivers=wave) and measured,
  so a silent success fails;
* compressed music (MP3/MP2, what retail Zero Hour ships) is *expected* to be undecodable here and
  is reported as such rather than being worked around.

The test assets are synthetic (ffmpeg-generated) and that is a limitation, not a result: retail
audio lives in .big archives that are not redistributable and are not on CI boxes. Formats and
container layout are real; the bytes are not retail. See docs/porting/audio-path-probe.md.

Usage:
    python3 scripts/native-audio-probe.py [--json report.json] [--keep] [--verbose]

Exits non-zero if the probe does not build, does not run, or an expectation that does not depend
on retail data fails.
"""

import argparse
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
CLANGXX = os.environ.get("CLANGXX", "clang++")
FFMPEG = os.environ.get("FFMPEG", "ffmpeg")

BACKEND_DIR = "Core/Libraries/Source/OpenALAudioDevice"

SOURCES = [
    f"{BACKEND_DIR}/OpenALDriver.cpp",
    f"{BACKEND_DIR}/OpenALSample.cpp",
    f"{BACKEND_DIR}/OpenAL3DSample.cpp",
    f"{BACKEND_DIR}/OpenALStream.cpp",
    f"{BACKEND_DIR}/OpenALWaveFile.cpp",
    f"{BACKEND_DIR}/tests/openal_audio_probe.cpp",
]

INCLUDES = [BACKEND_DIR]

COMPILE_FLAGS = ["-std=c++17", "-m64", "-g", "-O0", "-Wall", "-Wextra"]


def run(command, **kwargs):
    return subprocess.run(command, capture_output=True, text=True, **kwargs)


def find_openal_include():
    """Where <AL/al.h> is. The build boxes have libopenal-dev; macOS has the Homebrew keg."""
    candidates = [
        "/usr/include",
        "/usr/local/include",
        "/opt/homebrew/include",
        "/opt/homebrew/opt/openal-soft/include",
        "/usr/local/opt/openal-soft/include",
    ]
    env = os.environ.get("OPENAL_INCLUDE_DIR")
    if env:
        candidates.insert(0, env)
    for candidate in candidates:
        if (pathlib.Path(candidate) / "AL" / "al.h").is_file():
            return candidate
    return None


def find_openal_lib():
    candidates = [
        "/usr/lib/x86_64-linux-gnu",
        "/usr/lib/aarch64-linux-gnu",
        "/usr/lib",
        "/usr/local/lib",
        "/opt/homebrew/lib",
        "/opt/homebrew/opt/openal-soft/lib",
    ]
    env = os.environ.get("OPENAL_LIB_DIR")
    if env:
        candidates.insert(0, env)
    for candidate in candidates:
        directory = pathlib.Path(candidate)
        for name in ("libopenal.so", "libopenal.dylib", "libopenal.so.1"):
            if (directory / name).exists():
                return candidate
    return None


def build(work, verbose):
    include_dir = find_openal_include()
    lib_dir = find_openal_lib()
    if include_dir is None or lib_dir is None:
        print("error: OpenAL headers or library not found; install libopenal-dev "
              "(or openal-soft) or set OPENAL_INCLUDE_DIR / OPENAL_LIB_DIR", file=sys.stderr)
        return None

    binary = work / "openal_audio_probe"
    command = [CLANGXX, *COMPILE_FLAGS]
    for include in INCLUDES:
        command += ["-I", str(REPO_ROOT / include)]
    command += ["-I", include_dir]
    command += [str(REPO_ROOT / source) for source in SOURCES]
    command += ["-L", lib_dir, "-lopenal", "-lpthread", "-o", str(binary)]

    if verbose:
        print(" ".join(command))
    result = run(command)
    if result.returncode != 0:
        print("error: the probe did not build", file=sys.stderr)
        print(result.stdout + result.stderr, file=sys.stderr)
        return None
    if result.stderr.strip() and verbose:
        print(result.stderr)
    return binary


def make_assets(work, verbose):
    """Synthesises the container/codec combinations Miles handled, with ffmpeg.

    Returns a dict of name -> path, and the reference PCM decode of the ADPCM file.
    """
    assets = {}
    tone = ["-f", "lavfi", "-i", "sine=frequency=440:duration=0.5"]

    def ffmpeg(args, output):
        command = [FFMPEG, "-y", "-loglevel", "error", *args, str(output)]
        if verbose:
            print(" ".join(command))
        result = run(command)
        if result.returncode != 0:
            print(f"error: ffmpeg failed for {output.name}", file=sys.stderr)
            print(result.stdout + result.stderr, file=sys.stderr)
            return False
        return True

    plan = [
        ("pcm16_mono_22050", ["-ar", "22050", "-ac", "1", "-c:a", "pcm_s16le"], "wav"),
        ("pcm8_stereo_11025", ["-ar", "11025", "-ac", "2", "-c:a", "pcm_u8"], "wav"),
        ("pcm16_stereo_44100", ["-ar", "44100", "-ac", "2", "-c:a", "pcm_s16le"], "wav"),
        # A PCM WAV whose entire data chunk fits inside the 1024-byte window the stream path
        # reads. Paired with the half-second file above it isolates *why* a stream does or does
        # not decode: the format, or the size of that window.
        ("pcm16_mono_tiny", ["-ar", "8000", "-ac", "1", "-c:a", "pcm_s16le", "-t", "0.02"],
         "wav"),
        ("ima_adpcm_mono_22050", ["-ar", "22050", "-ac", "1", "-c:a", "adpcm_ima_wav"], "wav"),
        ("ima_adpcm_stereo_22050", ["-ar", "22050", "-ac", "2", "-c:a", "adpcm_ima_wav"], "wav"),
        ("ms_adpcm_mono_22050", ["-ar", "22050", "-ac", "1", "-c:a", "adpcm_ms"], "wav"),
        ("mp3_stereo_44100", ["-ar", "44100", "-ac", "2", "-c:a", "libmp3lame", "-b:a", "128k"],
         "mp3"),
        ("mp2_stereo_44100", ["-ar", "44100", "-ac", "2", "-c:a", "mp2", "-b:a", "128k"], "mp2"),
    ]

    for name, args, extension in plan:
        path = work / f"{name}.{extension}"
        if ffmpeg([*tone, *args], path):
            assets[name] = path

    # The independent oracle for the ADPCM decoder: ffmpeg's own decode of the same file, as raw
    # little-endian 16-bit PCM.
    for name in ("ima_adpcm_mono_22050", "ima_adpcm_stereo_22050"):
        if name in assets:
            reference = work / f"{name}.reference.s16le"
            if ffmpeg(["-i", str(assets[name]), "-f", "s16le", "-acodec", "pcm_s16le"], reference):
                assets[f"{name}.reference"] = reference

    return assets


def write_alsoft_conf(work, capture):
    """OpenAL Soft's wave writer: renders the mix to a RIFF file instead of to a device."""
    conf = work / f"alsoft-{capture.stem}.conf"
    conf.write_text(
        "[general]\n"
        "drivers=wave\n"
        "frequency=44100\n"
        "channels=stereo\n"
        "sample-type=int16\n"
        f"[wave]\nfile={capture}\n"
    )
    return conf


def probe(binary, work, stage_args, capture_name=None, verbose=False):
    """Runs one probe stage. Returns (facts, capture path or None)."""
    env = dict(os.environ)
    capture = None
    if capture_name is not None:
        capture = work / f"{capture_name}.wav"
        env["ALSOFT_CONF"] = str(write_alsoft_conf(work, capture))
    else:
        env["ALSOFT_DRIVERS"] = "null"
    env.setdefault("ALSOFT_LOGLEVEL", "0")

    command = [str(binary), *stage_args]
    if verbose:
        print(" ".join(command))
    result = subprocess.run(command, capture_output=True, text=True, env=env, timeout=120)
    facts = {}
    try:
        facts = json.loads(result.stdout)
    except json.JSONDecodeError:
        facts = {"fatal": "probe produced no JSON", "stdout": result.stdout,
                 "stderr": result.stderr}
    facts["exit_code"] = result.returncode
    if result.stderr.strip():
        facts["stderr"] = result.stderr.strip()
    return facts, capture


def read_riff(path):
    """Minimal RIFF reader.

    The stdlib's wave module refuses OpenAL Soft's captures, which are WAVE_FORMAT_EXTENSIBLE
    (0xFFFE), so the layout is read from the fmt chunk directly.
    """
    raw = pathlib.Path(path).read_bytes()
    if len(raw) < 44 or raw[0:4] != b"RIFF" or raw[8:12] != b"WAVE":
        return None
    at = 12
    channels = 0
    width = 0
    code = 0
    data = b""
    while at + 8 <= len(raw):
        tag = raw[at:at + 4]
        size = struct.unpack("<I", raw[at + 4:at + 8])[0]
        body = raw[at + 8:at + 8 + size]
        if tag == b"fmt " and len(body) >= 16:
            code = struct.unpack("<H", body[0:2])[0]
            channels, _, _, _, bits = struct.unpack("<HIIHH", body[2:16])
            width = bits // 8
            if code == 0xFFFE and len(body) >= 40:
                # WAVE_FORMAT_EXTENSIBLE: the real code is the first field of the sub-format GUID.
                code = struct.unpack("<H", body[24:26])[0]
        elif tag == b"data":
            # The wave writer leaves a stale size behind when the device is not closed; the bytes
            # on disk are the truth.
            data = body if 0 < size <= len(raw) - at - 8 else raw[at + 8:]
            break
        at += 8 + size + (size & 1)
    if channels == 0 or width == 0:
        return None
    return channels, width, code, data


def measure_capture(path):
    """Per-channel RMS and peak of a captured mix, as fractions of full scale."""
    if path is None or not path.exists():
        return {"captured": False}
    parsed = read_riff(path)
    if parsed is None:
        return {"captured": False, "error": "capture is not a readable RIFF file"}
    channels, width, code, raw = parsed
    frames = len(raw) // (channels * width) if channels and width else 0

    # WAVE_FORMAT_PCM 16-bit, or WAVE_FORMAT_IEEE_FLOAT 32-bit: the two OpenAL Soft's wave writer
    # produces. Everything is normalised to a fraction of full scale.
    if frames == 0 or not ((code == 1 and width == 2) or (code == 3 and width == 4)):
        return {"captured": True, "frames": frames, "channels": channels, "width": width,
                "format_code": code, "rms": [0.0] * channels, "peak": [0.0] * channels,
                "measured": False}

    count = frames * channels
    if code == 1:
        samples = struct.unpack(f"<{count}h", raw[: count * 2])
        scale = 32768.0
    else:
        samples = struct.unpack(f"<{count}f", raw[: count * 4])
        scale = 1.0

    sums = [0.0] * channels
    peaks = [0.0] * channels
    for index, value in enumerate(samples):
        channel = index % channels
        sums[channel] += float(value) * float(value)
        peaks[channel] = max(peaks[channel], abs(float(value)))
    per_channel = max(1, count // channels)
    return {
        "captured": True,
        "measured": True,
        "frames": frames,
        "channels": channels,
        "format_code": code,
        "seconds": round(frames / 44100.0, 3),
        "rms": [round((total / per_channel) ** 0.5 / scale, 6) for total in sums],
        "peak": [round(peak / scale, 6) for peak in peaks],
    }


def payload_of(path):
    """The PCM bytes of a file that may be a bare sample dump or a RIFF/WAVE image.

    AIL_decompress_ADPCM returns a WAV image, as Miles' did, so the decoder's output has to be
    unwrapped before it can be compared with a raw reference decode.
    """
    raw = pathlib.Path(path).read_bytes()
    if raw[0:4] != b"RIFF":
        return raw
    parsed = read_riff(path)
    return parsed[3] if parsed is not None else raw


def compare_pcm(decoded_path, reference_path):
    """Compares the backend's ADPCM decode against ffmpeg's, sample by sample."""
    decoded = payload_of(decoded_path)
    reference = payload_of(reference_path)
    count = min(len(decoded), len(reference)) // 2
    if count == 0:
        return {"comparable": False, "decoded_bytes": len(decoded),
                "reference_bytes": len(reference)}
    left = struct.unpack(f"<{count}h", decoded[: count * 2])
    right = struct.unpack(f"<{count}h", reference[: count * 2])
    worst = 0
    total = 0.0
    for a, b in zip(left, right):
        difference = abs(a - b)
        worst = max(worst, difference)
        total += float(difference) * float(difference)
    return {
        "comparable": True,
        "decoded_bytes": len(decoded),
        "reference_bytes": len(reference),
        "compared_samples": count,
        "byte_length_matches": len(decoded) == len(reference),
        "max_abs_difference": worst,
        "rms_difference": round((total / count) ** 0.5, 3),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--json", help="write the collected facts to this path")
    parser.add_argument("--keep", action="store_true", help="keep the build and asset directory")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    if shutil.which(FFMPEG) is None:
        print(f"error: {FFMPEG} not found; the probe needs it to synthesise test assets",
              file=sys.stderr)
        return 1

    work = pathlib.Path(tempfile.mkdtemp(prefix="native-audio-probe-"))
    report = {"work_dir": str(work), "stages": {}}
    try:
        binary = build(work, args.verbose)
        if binary is None:
            return 1

        assets = make_assets(work, args.verbose)
        report["assets"] = {name: path.name for name, path in assets.items()}

        # 1. Initialisation: device, driver, providers, listener, voice pool.
        facts, _ = probe(binary, work, ["init"], verbose=args.verbose)
        report["stages"]["init"] = facts

        # 2/3. Every sample format, played to completion with the mix captured.
        for name in ("pcm16_mono_22050", "pcm8_stereo_11025", "pcm16_stereo_44100",
                     "pcm16_mono_tiny", "ima_adpcm_mono_22050", "ima_adpcm_stereo_22050",
                     "ms_adpcm_mono_22050"):
            if name not in assets:
                continue
            facts, capture = probe(binary, work, ["sample", str(assets[name]), "0.5"],
                                   capture_name=f"sample-{name}", verbose=args.verbose)
            facts["capture"] = measure_capture(capture)
            report["stages"][f"sample:{name}"] = facts

        # 3. 2D pan. Miles panned a voice in the mixer; OpenAL has no pan, so the layer offsets
        # the source along x instead. A mono voice and a stereo voice are both panned hard left,
        # because OpenAL Soft does not position multi-channel sources at all.
        for label, name, pan in (("mono-hard-left", "pcm16_mono_22050", "0.0"),
                                 ("mono-hard-right", "pcm16_mono_22050", "1.0"),
                                 ("stereo-hard-left", "pcm16_stereo_44100", "0.0")):
            if name not in assets:
                continue
            facts, capture = probe(binary, work, ["sample", str(assets[name]), pan],
                                   capture_name=f"pan-{label}", verbose=args.verbose)
            facts["capture"] = measure_capture(capture)
            report["stages"][f"pan:{label}"] = facts

        # 2. The ADPCM decoder against ffmpeg's decode of the same bytes.
        for name in ("ima_adpcm_mono_22050", "ima_adpcm_stereo_22050"):
            if name not in assets or f"{name}.reference" not in assets:
                continue
            decoded = work / f"{name}.decoded.s16le"
            facts, _ = probe(binary, work, ["adpcm", str(assets[name]), str(decoded)],
                             verbose=args.verbose)
            if decoded.exists():
                facts["comparison"] = compare_pcm(decoded, assets[f"{name}.reference"])
            report["stages"][f"adpcm:{name}"] = facts

        # 2/3. AudioFileCache::openFile() into MilesAudioManager::playSample(), verbatim: the
        # engine hands AIL_decompress_ADPCM's output to AIL_set_sample_file and ignores the result.
        for name in ("ima_adpcm_mono_22050", "ima_adpcm_stereo_22050"):
            if name not in assets:
                continue
            facts, capture = probe(binary, work, ["engine-adpcm", str(assets[name])],
                                   capture_name=f"engine-adpcm-{name}", verbose=args.verbose)
            facts["capture"] = measure_capture(capture)
            report["stages"][f"engine-adpcm:{name}"] = facts

        # 2/3. The streaming path: PCM WAV, then the compressed formats retail music uses.
        for name in ("pcm16_stereo_44100", "pcm16_mono_tiny", "mp3_stereo_44100",
                     "mp2_stereo_44100"):
            if name not in assets:
                continue
            facts, capture = probe(binary, work, ["stream", str(assets[name])],
                                   capture_name=f"stream-{name}", verbose=args.verbose)
            facts["capture"] = measure_capture(capture)
            report["stages"][f"stream:{name}"] = facts
        # And the same call with no file callbacks installed, which is how it fails.
        if "pcm16_stereo_44100" in assets:
            facts, _ = probe(binary, work,
                             ["stream", str(assets["pcm16_stereo_44100"]), "no-callbacks"],
                             verbose=args.verbose)
            report["stages"]["stream:no-callbacks"] = facts

        # 4. 3D positional audio: the same mono sample to the left, to the right, and occluded.
        if "pcm16_mono_22050" in assets:
            for label, x, occlusion in (("left", -20.0, 0.0), ("right", 20.0, 0.0),
                                        ("centre", 0.0, 0.0), ("occluded", -20.0, 1.0)):
                facts, capture = probe(
                    binary, work,
                    ["sample3d", str(assets["pcm16_mono_22050"]), str(x), str(occlusion)],
                    capture_name=f"sample3d-{label}", verbose=args.verbose)
                facts["capture"] = measure_capture(capture)
                report["stages"][f"sample3d:{label}"] = facts

        report["findings"] = summarise(report)
        print_report(report)

        if args.json:
            pathlib.Path(args.json).write_text(json.dumps(report, indent=2) + "\n")
            print(f"\nwrote {args.json}")

        gates = [f for f in report["findings"] if f["kind"] == "gate"]
        return 0 if all(finding["ok"] for finding in gates) else 1
    finally:
        if args.keep:
            print(f"\nkept {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


def summarise(report):
    """Turns the collected facts into findings of two kinds.

    A *gate* is a property that holds today and must keep holding; a failing gate is a regression
    and fails the run. An *observation* is a measured fact about a path that is incomplete --
    unimplemented decoders, approximated 3D behaviour, a defect this probe found. Observations
    never fail the run, because the point of the probe is to record them, not to hide them behind
    a red build.
    """
    stages = report["stages"]
    findings = []

    def add(name, ok, detail):
        findings.append({"finding": name, "kind": "gate", "ok": bool(ok), "detail": detail})

    def observe(name, detail):
        findings.append({"finding": name, "kind": "observation", "ok": True, "detail": detail})

    init = stages.get("init", {})
    add("device opens off Windows",
        init.get("alc_context_current") and init.get("dig_driver_opened"),
        f"device={init.get('alc_device_name')} version={init.get('ail_mss_version')}")
    add("AIL_waveOutOpen path also yields a driver",
        init.get("waveoutopen_result") == 0 and init.get("waveoutopen_driver"),
        f"emulated_ds={init.get('waveoutopen_emulated_ds')}")
    add("2D voice pool is non-trivial",
        (init.get("sample_handles_granted") or 0) >= 32,
        f"{init.get('sample_handles_granted')} of {init.get('sample_handles_requested')} granted")
    add("3D provider and listener open",
        init.get("open_3d_provider_result") == 0 and init.get("listener_opened"),
        f"providers={init.get('provider_names')} 3d_voices={init.get('sample_3d_handles_granted')}")

    for name, expected in (("pcm16_mono_22050", True), ("pcm8_stereo_11025", True),
                           ("pcm16_stereo_44100", True), ("pcm16_mono_tiny", True),
                           ("ima_adpcm_mono_22050", True),
                           ("ima_adpcm_stereo_22050", True), ("ms_adpcm_mono_22050", False)):
        stage = stages.get(f"sample:{name}")
        if stage is None:
            continue
        loaded = stage.get("set_sample_file_result") == 1
        capture = stage.get("capture", {})
        audible = max(capture.get("rms") or [0.0]) > 0.001
        if expected:
            add(f"sample loads and is audible: {name}", loaded and audible,
                f"set_sample_file={stage.get('set_sample_file_result')} "
                f"length_ms={stage.get('sample_length_ms')} rms={capture.get('rms')}")
        else:
            add(f"sample is refused, loudly, not silently: {name}", not loaded,
                f"set_sample_file={stage.get('set_sample_file_result')} "
                f"last_error={stage.get('set_sample_file_last_error')!r} rms={capture.get('rms')}")

    for name in ("ima_adpcm_mono_22050", "ima_adpcm_stereo_22050"):
        stage = stages.get(f"adpcm:{name}")
        if stage is None:
            continue
        comparison = stage.get("comparison", {})
        # Not bit-exactness: ffmpeg computes the step as ((2*delta+1)*step)>>3 while this decoder
        # uses the reference implementation's shift-and-add, and the two truncate differently. The
        # tolerance bounds that drift near -60 dBFS; more than this would mean a wrong table or a
        # wrong block layout rather than rounding.
        add(f"ADPCM decode tracks ffmpeg's within rounding: {name}",
            comparison.get("comparable")
            and comparison.get("byte_length_matches")
            and comparison.get("max_abs_difference", 1 << 20) <= 64,
            f"samples={comparison.get('compared_samples')} "
            f"max_abs_difference={comparison.get('max_abs_difference')} "
            f"rms_difference={comparison.get('rms_difference')} "
            f"byte_length_matches={comparison.get('byte_length_matches')}")

    for name in ("ima_adpcm_mono_22050", "ima_adpcm_stereo_22050"):
        stage = stages.get(f"engine-adpcm:{name}")
        if stage is None:
            continue
        capture = stage.get("capture", {})
        audible = max(capture.get("rms") or [0.0]) > 0.001
        # Miles' AIL_decompress_ADPCM produced a WAV *image*; the engine relies on that, because it
        # feeds the result straight back into AIL_set_sample_file. If this layer returns bare PCM
        # the sound is dropped, and the engine never looks at the return value to notice.
        detail = (f"decompressed_bytes={stage.get('decompressed_bytes')} "
                  f"decompressed_is_riff_wave={stage.get('decompressed_is_riff_wave')} "
                  f"set_sample_file_result={stage.get('set_sample_file_result')} "
                  f"last_error={stage.get('set_sample_file_last_error')!r} "
                  f"rms={capture.get('rms')}")
        add(f"the engine's own ADPCM sequence is audible: {name}",
            stage.get("decompressed_is_riff_wave")
            and stage.get("set_sample_file_result") == 1
            and audible,
            detail)

    # The short PCM WAV whose whole data chunk fits in the 1024-byte window the stream path reads
    # is the control for the long one; the pair localises the failure to the window, not the codec.
    short = stages.get("stream:pcm16_mono_tiny")
    if short is not None:
        capture = short.get("capture", {})
        add("PCM WAV stream plays when its data chunk fits the header window",
            short.get("open_stream_handle")
            and (short.get("stream_length_ms") or 0) > 0
            and max(capture.get("rms") or [0.0]) > 0.001,
            f"length_ms={short.get('stream_length_ms')} "
            f"high_water_ms={short.get('stream_high_water_position_ms')} "
            f"rms={capture.get('rms')}")

    long_stream = stages.get("stream:pcm16_stereo_44100")
    if long_stream is not None and short is not None:
        capture = long_stream.get("capture", {})
        # The regression guard for the header-window defect: the parse window is now sized by where
        # the data chunk starts, so a stream's length no longer depends on its payload fitting in
        # one read. A 0.5 s file is ~86 KB of payload against a 1024-byte first window.
        add("a PCM WAV stream far larger than the first header read still plays",
            (long_stream.get("stream_length_ms") or 0) >= 450
            and max(capture.get("rms") or [0.0]) > 0.001,
            f"0.5 s file: length_ms={long_stream.get('stream_length_ms')} "
            f"rms={capture.get('rms')}; "
            f"0.02 s file: length_ms={short.get('stream_length_ms')} "
            f"rms={short.get('capture', {}).get('rms')}")

    no_callbacks = stages.get("stream:no-callbacks")
    if no_callbacks is not None:
        add("a stream opened without file callbacks fails loudly",
            not no_callbacks.get("open_stream_handle")
            and no_callbacks.get("open_stream_last_error"),
            f"handle={no_callbacks.get('open_stream_handle')} "
            f"last_error={no_callbacks.get('open_stream_last_error')!r}")

    for name in ("mp3_stereo_44100", "mp2_stereo_44100"):
        stage = stages.get(f"stream:{name}")
        if stage is None:
            continue
        # The wall is still there -- there is no MPEG decoder -- but it is no longer silent: the
        # open fails and names the reason. Retail Zero Hour music is MP3, so this is an
        # UNIMPLEMENTED path that is REQUIRED, not a format that can be cut.
        add(f"compressed music fails to open, loudly, rather than playing silence: {name}",
            not stage.get("open_stream_handle") and stage.get("open_stream_last_error"),
            f"handle={stage.get('open_stream_handle')} "
            f"last_error={stage.get('open_stream_last_error')!r}")
        observe("UNIMPLEMENTED (required): no MPEG decoder is linked, so music cannot play: "
                f"{name}",
                f"last_error={stage.get('open_stream_last_error')!r}; retail MusicZH.big holds 7 "
                "MP3 tracks, so this blocks music entirely rather than being cuttable")

    mono_left = stages.get("pan:mono-hard-left", {}).get("capture", {})
    mono_right = stages.get("pan:mono-hard-right", {}).get("capture", {})
    if len(mono_left.get("rms") or []) == 2 and len(mono_right.get("rms") or []) == 2:
        add("2D pan reaches the mix for a mono voice",
            mono_left["rms"][0] > mono_left["rms"][1] * 4
            and mono_right["rms"][1] > mono_right["rms"][0] * 4,
            f"pan=0 rms={mono_left['rms']} pan=1 rms={mono_right['rms']}")

    stereo_left = stages.get("pan:stereo-hard-left", {}).get("capture", {})
    if len(stereo_left.get("rms") or []) == 2:
        balanced = abs(stereo_left["rms"][0] - stereo_left["rms"][1]) < 1e-4
        observe("2D pan is inert for a stereo voice (OpenAL does not position multi-channel "
                "sources)" if balanced else "2D pan affects a stereo voice",
                f"pan=0 rms={stereo_left['rms']}")

    left = stages.get("sample3d:left", {}).get("capture", {})
    right = stages.get("sample3d:right", {}).get("capture", {})
    if len(left.get("rms") or []) == 2 and len(right.get("rms") or []) == 2:
        add("3D position reaches the mix",
            max(left["rms"]) > 0.001 and max(right["rms"]) > 0.001
            and abs(left["rms"][0] - left["rms"][1]) > 0.001,
            f"x=-20 rms={left['rms']} x=+20 rms={right['rms']}")
        # Handedness. The engine's listener is (facing, up=(0,0,-1)) in Miles' left-handed frame
        # (MilesAudioManager.cpp:2537); OpenAL is right-handed, so the seam negates Z
        # (milesToAlZ in OpenALAudioInternal.h). Without that the image is mirrored, which is what
        # this asserts: +x must be louder on the right, -x louder on the left.
        add("3D pan is not mirrored: +x is heard on the right",
            right["rms"][1] > right["rms"][0] * 1.5 and left["rms"][0] > left["rms"][1] * 1.5,
            f"x=-20 rms={left['rms']} x=+20 rms={right['rms']}")

    plain = stages.get("sample3d:left", {}).get("capture", {})
    occluded = stages.get("sample3d:occluded", {}).get("capture", {})
    if plain.get("rms") and occluded.get("rms"):
        loud = max(plain["rms"])
        quiet = max(occluded["rms"])
        ratio = round(quiet / loud, 4) if loud > 0 else None
        # Miles low-passed an occluded voice. This layer attenuates gain instead
        # (OpenAL3DSample.cpp: 1 - occlusion*0.75), which is audible but is not the same effect.
        observe("APPROXIMATION: occlusion is gain attenuation, not a low-pass filter",
                f"occlusion=0 rms={plain['rms']} occlusion=1 rms={occluded['rms']} "
                f"ratio={ratio} (expected 0.25 from 1 - occlusion*0.75)")

    init = stages.get("init", {})
    observe("APPROXIMATION: the Bink DirectSound handoff yields null pointers",
            f"AIL_get_DirectSound_info object={init.get('directsound_object')} "
            f"buffer={init.get('directsound_buffer')}")

    return findings


def print_report(report):
    print("Audio path probe")
    print("=" * 72)
    init = report["stages"].get("init", {})
    print(f"device            : {init.get('alc_device_name')}")
    print(f"AIL_MSS_version   : {init.get('ail_mss_version')}")
    print(f"2D voices granted : {init.get('sample_handles_granted')}")
    print(f"3D voices granted : {init.get('sample_3d_handles_granted')}")
    print(f"3D providers      : {init.get('provider_names')}")
    print()
    gates = [f for f in report["findings"] if f["kind"] == "gate"]
    observations = [f for f in report["findings"] if f["kind"] == "observation"]

    print("Gates -- properties that hold today and must keep holding")
    failed = 0
    for finding in gates:
        status = "ok  " if finding["ok"] else "FAIL"
        failed += 0 if finding["ok"] else 1
        print(f"  [{status}] {finding['finding']}")
        print(f"          {finding['detail']}")
    print(f"\n  {len(gates) - failed} ok, {failed} failed\n")

    print("Observations -- measured facts about paths that are not finished")
    for finding in observations:
        print(f"  * {finding['finding']}")
        print(f"    {finding['detail']}")


if __name__ == "__main__":
    sys.exit(main())

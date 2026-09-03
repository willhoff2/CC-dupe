#!/usr/bin/env python3
"""Does the OpenAL Miles replacement render a known waveform without clicks, gaps or starvation?

The M1 Pro hears a constant crackle in menu music and ambient SFX (sound-effects-chain.md §6 item 0).
This driver isolates the part of that chain a Linux box can judge deterministically: the shim's data
path from the AIL_* surface to OpenAL Soft's mixer output, on the file-writing `wave` backend. A
file backend cannot underrun the way a real-time device does, so a clean run here rules the data
path out (decode, alBufferData format/rate, stream refill, one-shot restart) and says nothing about
the device; a dirty run is a deterministic shim defect.

It synthesises its own assets (pure tones, so any adjacent-sample jump above the tone's own slope is
a discontinuity), builds `Core/Libraries/Source/OpenALAudioDevice/tests/openal_render_test.cpp`
against the working tree's shim, renders the stream path (music/speech), the one-shot path (SFX) and
the EOS-callback restart path (startNextLoop's shape) with the shim's OPENAL_AUDIO_DIAG counters on,
then judges the rendered PCM with scripts/audio-pcm-discontinuity.py and the counters from the log:

  * the context runs at the engine's 44,100 Hz mixer rate and the ALC attribute list was accepted;
  * no stream ran its queue dry, no stopped source had to be restarted, no alBufferData call
    disagreed with its decoded data, no AL error;
  * the stream and one-shot renders have zero jumps and zero interior gaps; the loop render has zero
    jumps (its inter-loop gaps are the engine's callback latency, reported, not judged).

`--shim-rev REV` builds the shim from that git revision instead, with the same harness and assets.

Usage:
    python3 scripts/native-audio-render-test.py [--json report.json] [--verbose] [--keep]
    python3 scripts/native-audio-render-test.py --shim-rev b905296b3
"""

import argparse
import importlib.util
import json
import math
import os
import pathlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
BACKEND_DIR = "Core/Libraries/Source/OpenALAudioDevice"
HARNESS = f"{BACKEND_DIR}/tests/openal_render_test.cpp"
BACKEND_SOURCES = [
    "OpenALDriver.cpp",
    "OpenALMpeg.cpp",
    "OpenALSample.cpp",
    "OpenAL3DSample.cpp",
    "OpenALStream.cpp",
    "OpenALWaveFile.cpp",
    "OpenALAudioInternal.h",
]
MIXER_RATE = 44100

# A 440 Hz tone at 0.5 full scale moves at most 2*pi*440*0.5/rate per sample: 0.031 at 44.1 kHz,
# 0.063 at 22.05 kHz. Anything above this is not the tone.
JUMP_THRESHOLD = 0.1
GAP_MS = 1.0

RENDERS = (
    # (mode, asset name, rate, channels, seconds)
    ("stream", "tone-stream.wav", 44100, 2, 6.0),
    ("sample", "tone-sample.wav", 22050, 1, 1.5),
    ("loop", "tone-loop.wav", 22050, 1, 0.5),
)


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def write_tone(path, rate, channels, seconds, hz=440.0, amplitude=0.5):
    """A PCM16 RIFF tone with a 10 ms raised-cosine fade at both ends, so the asset itself has no
    edge click for the analyser to blame on the shim."""
    frames = int(rate * seconds)
    fade = int(rate * 0.010)
    out = bytearray()
    for i in range(frames):
        env = 1.0
        if i < fade:
            env = 0.5 - 0.5 * math.cos(math.pi * i / fade)
        elif i >= frames - fade:
            env = 0.5 - 0.5 * math.cos(math.pi * (frames - 1 - i) / fade)
        value = int(round(amplitude * env * math.sin(2.0 * math.pi * hz * i / rate) * 32767))
        out += struct.pack("<h", value) * channels
    with wave.open(str(path), "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(out))


def export_shim(rev, into):
    for name in BACKEND_SOURCES:
        result = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "show", f"{rev}:{BACKEND_DIR}/{name}"],
            capture_output=True, text=True)
        if result.returncode != 0:
            print(f"error: git show {rev}:{BACKEND_DIR}/{name} failed: {result.stderr.strip()}",
                  file=sys.stderr)
            return False
        (into / name).write_text(result.stdout)
    return True


def build(probe, shim_dir, work, verbose):
    include_dir = probe.find_openal_include()
    lib_dir = probe.find_openal_lib()
    minimp3_dir = probe.find_minimp3_include()
    if minimp3_dir is None:
        print("error: <minimp3.h> not found; run scripts/ci/fetch-probe-deps.sh", file=sys.stderr)
        return None
    if include_dir is None or lib_dir is None:
        print("error: OpenAL headers or library not found; install libopenal-dev", file=sys.stderr)
        return None

    binary = work / "openal_render_test"
    command = [probe.CLANGXX, *probe.COMPILE_FLAGS, "-pthread"]
    command += ["-I", str(shim_dir), "-I", str(REPO_ROOT / BACKEND_DIR)]
    command += ["-I", include_dir, "-I", minimp3_dir]
    command += [str(shim_dir / name) for name in BACKEND_SOURCES if name.endswith(".cpp")]
    command += [str(REPO_ROOT / HARNESS)]
    command += ["-L", lib_dir, "-lopenal", "-lpthread", "-o", str(binary)]
    if verbose:
        print(" ".join(command))
    result = probe.run(command)
    if result.returncode != 0:
        print("error: the harness did not build", file=sys.stderr)
        print(result.stdout + result.stderr, file=sys.stderr)
        return None
    return binary


def parse_diag(log_path):
    """The shim's `context ...` line and its final `counters shutdown ...` line as dicts."""
    context, counters = {}, {}
    if not log_path.exists():
        return context, counters
    for line in log_path.read_text(errors="replace").splitlines():
        body = re.sub(r"^\[openal-diag [0-9.]+\] ", "", line)
        if body.startswith("context "):
            context = {k: int(v) for k, v in re.findall(r"(\w+)=(-?\d+)", body)}
        elif body.startswith("counters shutdown"):
            counters = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", body)}
    return context, counters


def render(binary, analyser, work, mode, asset, seconds, verbose):
    out_wav = work / f"render-{mode}.wav"
    conf = work / f"alsoft-{mode}.conf"
    # No `frequency` key: OpenAL Soft lets the config file override the ALC_FREQUENCY attribute,
    # and the point is to observe what the shim's own request produced.
    conf.write_text(f"[general]\ndrivers = wave\n[wave]\nfile = {out_wav}\n")
    diag = work / f"diag-{mode}.log"
    env = dict(os.environ)
    env["ALSOFT_CONF"] = str(conf)
    env["OPENAL_AUDIO_DIAG"] = str(diag)
    env.setdefault("ALSOFT_LOGLEVEL", "0")
    limit = str(seconds * 12 + 5)
    result = subprocess.run([str(binary), mode, str(asset), limit], capture_output=True,
                            text=True, env=env, timeout=300)
    try:
        facts = json.loads(result.stdout)
    except json.JSONDecodeError:
        facts = {"fatal": "harness produced no JSON", "stdout": result.stdout}
    facts["exit_code"] = result.returncode
    if result.stderr.strip():
        facts["stderr"] = result.stderr.strip()

    context, counters = parse_diag(diag)
    facts["context"] = context
    facts["counters"] = counters

    if out_wav.exists() and out_wav.stat().st_size > 44:
        rate, channels, pcm = analyser.read_wave(out_wav)
        jumps = analyser.find_jumps(pcm, JUMP_THRESHOLD)
        gaps = analyser.find_gaps(pcm, rate, GAP_MS)
        facts["pcm"] = {
            "rate": rate,
            "channels": channels,
            "frames": int(pcm.shape[0]),
            "jump_threshold": JUMP_THRESHOLD,
            "jumps": len(jumps),
            "first_jumps_s": [round(float(i) / rate, 3) for i in jumps[:8]],
            "gap_min_ms": GAP_MS,
            "gaps": len(gaps),
            "first_gaps_s": [[round(float(s) / rate, 3), round(float(n) * 1000.0 / rate, 1)]
                             for s, n in gaps[:8]],
        }
    else:
        facts["pcm"] = {"fatal": "no PCM rendered"}
    if verbose:
        print(json.dumps({mode: facts}, indent=2))
    return facts


def judge(results):
    checks = []

    def add(name, ok, detail):
        checks.append({"check": name, "ok": bool(ok), "detail": detail})

    for mode, facts in results.items():
        add(f"{mode}: harness completed",
            facts.get("completed") is True and facts.get("exit_code") == 0,
            f"exit_code={facts.get('exit_code')} completions={facts.get('completions')} "
            f"fatal={facts.get('fatal')!r}")

    stream = results["stream"]
    context = stream.get("context", {})
    add("context created at the engine's mixer rate with the attribute list accepted",
        context.get("frequency") == MIXER_RATE and context.get("attributes_accepted") == 1,
        f"requested={context.get('requested_frequency')} effective={context.get('frequency')} "
        f"refresh={context.get('refresh')} attributes_accepted={context.get('attributes_accepted')}")

    for mode, facts in results.items():
        c = facts.get("counters", {})
        add(f"{mode}: no starvation, restart, format mismatch or AL error",
            c and c.get("stream_queue_emptied") == 0 and c.get("stream_stopped_with_data") == 0
            and c.get("sample_restarts_while_playing") == 0
            and c.get("buffer_data_mismatches") == 0 and c.get("al_errors") == 0,
            f"queue_emptied={c.get('stream_queue_emptied')} "
            f"stopped_with_data={c.get('stream_stopped_with_data')} "
            f"queued_min={c.get('stream_queued_min')} "
            f"service_gap_max_us={c.get('stream_service_gap_max_us')} "
            f"sample_restarts_while_playing={c.get('sample_restarts_while_playing')} "
            f"buffer_data_calls={c.get('buffer_data_calls')} "
            f"mismatches={c.get('buffer_data_mismatches')} al_errors={c.get('al_errors')}")

    for mode, facts in results.items():
        pcm = facts.get("pcm", {})
        judged_gaps = mode != "loop"
        ok = pcm.get("jumps") == 0 and (not judged_gaps or pcm.get("gaps") == 0)
        what = "zero jumps and zero interior gaps" if judged_gaps else "zero jumps"
        add(f"{mode}: rendered PCM has {what}",
            ok,
            f"rate={pcm.get('rate')} frames={pcm.get('frames')} jumps={pcm.get('jumps')} "
            f"first_jumps_s={pcm.get('first_jumps_s')} gaps={pcm.get('gaps')} "
            f"first_gaps_s={pcm.get('first_gaps_s')}")
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--json", type=pathlib.Path, help="write the report here")
    parser.add_argument("--shim-rev", help="build the shim from this git revision")
    parser.add_argument("--keep", action="store_true", help="keep the work directory")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    probe = load_module("native_audio_probe", REPO_ROOT / "scripts" / "native-audio-probe.py")
    analyser = load_module("audio_pcm_discontinuity",
                           REPO_ROOT / "scripts" / "audio-pcm-discontinuity.py")
    work = pathlib.Path(tempfile.mkdtemp(prefix="openal-render-"))
    results = {}
    try:
        if args.shim_rev:
            shim_dir = work / "shim"
            shim_dir.mkdir()
            if not export_shim(args.shim_rev, shim_dir):
                return 2
        else:
            shim_dir = REPO_ROOT / BACKEND_DIR

        binary = build(probe, shim_dir, work, args.verbose)
        if binary is None:
            return 2
        for mode, name, rate, channels, seconds in RENDERS:
            asset = work / name
            write_tone(asset, rate, channels, seconds)
            results[mode] = render(binary, analyser, work, mode, asset, seconds, args.verbose)
    finally:
        if args.keep:
            print(f"work directory kept: {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)

    checks = judge(results)
    report = {
        "shim": args.shim_rev or "working tree",
        "backend": "OpenAL Soft wave (file; synthetic, cannot underrun)",
        "renders": results,
        "checks": checks,
    }
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n")

    for check in checks:
        print(f"[{'ok' if check['ok'] else 'FAIL'}] {check['check']}: {check['detail']}")
    ok = all(check["ok"] for check in checks)
    print("verdict: " + ("pass" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Does the OpenAL Miles replacement reset a 3D voice's playback rate when a file is set?

MilesAudioManager::initFilters3D applies an event's pitch shift by reading the voice's rate and
writing it back multiplied, after AIL_set_3D_sample_file, on every 3D play: each loop of a looping
sound and each event reusing a pooled voice. Unless setting a file resets the rate to the file's
own, the shifts compound across plays, and a voice that moves to a file of a different rate keeps
the old file's Hz (a 22050 Hz rate on a 44100 Hz file is half speed). sound-effects-chain.md
section 10 records the defect.

`Core/Libraries/Source/OpenALAudioDevice/tests/openal_3d_playback_rate_test.cpp` drives the
public AIL_* API the way the engine does and reads the source's AL_PITCH; this script builds it
against the working tree's shim and judges:

  * a rate set on one file does not survive setting the file again;
  * ten plays of the engine's per-loop sequence all run at one pitch shift of the file's rate;
  * a voice moving from a 22050 Hz file to a 44100 Hz one reports 44100 and mixes at AL_PITCH 1.

Runs on OpenAL Soft's null driver: no audio device is needed, and nothing here is audible.

Usage:
    python3 scripts/native-audio-3d-rate-test.py [--json report.json] [--verbose]
"""

import argparse
import importlib.util
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
BACKEND_DIR = "Core/Libraries/Source/OpenALAudioDevice"
HARNESS = f"{BACKEND_DIR}/tests/openal_3d_playback_rate_test.cpp"
BACKEND_SOURCES = [
    "OpenALDriver.cpp",
    "OpenALMpeg.cpp",
    "OpenALSample.cpp",
    "OpenAL3DSample.cpp",
    "OpenALStream.cpp",
    "OpenALWaveFile.cpp",
]
PITCH_TOLERANCE = 1e-4


def load_audio_probe():
    """scripts/native-audio-probe.py owns the compile recipe and the dependency lookups."""
    path = REPO_ROOT / "scripts" / "native-audio-probe.py"
    spec = importlib.util.spec_from_file_location("native_audio_probe", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def build(probe, work, verbose):
    include_dir = probe.find_openal_include()
    lib_dir = probe.find_openal_lib()
    minimp3_dir = probe.find_minimp3_include()
    if minimp3_dir is None:
        print("error: <minimp3.h> not found; run scripts/ci/fetch-probe-deps.sh", file=sys.stderr)
        return None
    if include_dir is None or lib_dir is None:
        print("error: OpenAL headers or library not found; install libopenal-dev", file=sys.stderr)
        return None

    backend = REPO_ROOT / BACKEND_DIR
    binary = work / "openal_3d_playback_rate_test"
    command = [probe.CLANGXX, *probe.COMPILE_FLAGS, "-pthread"]
    command += ["-I", str(backend), "-I", include_dir, "-I", minimp3_dir]
    command += [str(backend / name) for name in BACKEND_SOURCES]
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


def run_harness(binary, verbose):
    env = dict(os.environ)
    env["ALSOFT_DRIVERS"] = "null"
    env.setdefault("ALSOFT_LOGLEVEL", "0")
    result = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=120)
    try:
        facts = json.loads(result.stdout)
    except json.JSONDecodeError:
        facts = {"fatal": "harness produced no JSON", "stdout": result.stdout}
    facts["exit_code"] = result.returncode
    if result.stderr.strip():
        facts["stderr"] = result.stderr.strip()
    if verbose:
        print(json.dumps(facts, indent=2))
    return facts


def pitch_is(value, expected):
    return value is not None and abs(value - expected) < PITCH_TOLERANCE


def judge(facts):
    """(name, ok, detail) per expectation."""
    checks = []

    def add(name, ok, detail):
        checks.append({"check": name, "ok": bool(ok), "detail": detail})

    add("harness completed", facts.get("completed") is True and facts.get("exit_code") == 0,
        f"exit_code={facts.get('exit_code')} fatal={facts.get('fatal')!r}")

    add("setting the file again restores the file's rate",
        facts.get("refile_rate_before") == 22050
        and facts.get("refile_rate_shifted") != 22050
        and facts.get("refile_rate_after") == 22050
        and pitch_is(facts.get("refile_al_pitch_after"), 1.0),
        f"before={facts.get('refile_rate_before')} shifted={facts.get('refile_rate_shifted')} "
        f"after={facts.get('refile_rate_after')} "
        f"AL_PITCH={facts.get('refile_al_pitch_after')}")

    rates = facts.get("loop_rates") or []
    expected = facts.get("loop_expected_rate")
    add("ten loops of the engine sequence apply one pitch shift, without drift",
        len(rates) == 10 and all(rate == expected for rate in rates)
        and pitch_is(facts.get("loop_al_pitch_last"), expected / 22050 if expected else 0),
        f"expected={expected} rates={rates} AL_PITCH last={facts.get('loop_al_pitch_last')}")

    add("a 22050 Hz rate does not carry onto a 44100 Hz file",
        facts.get("pooled_rate_after") == 44100
        and pitch_is(facts.get("pooled_al_pitch_after"), 1.0),
        f"rate={facts.get('pooled_rate_after')} AL_PITCH={facts.get('pooled_al_pitch_after')}")
    return checks


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--json", type=pathlib.Path, help="write the report here")
    parser.add_argument("--keep", action="store_true", help="keep the work directory")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    probe = load_audio_probe()
    work = pathlib.Path(tempfile.mkdtemp(prefix="openal-3d-rate-"))
    try:
        binary = build(probe, work, args.verbose)
        if binary is None:
            return 2
        facts = run_harness(binary, args.verbose)
    finally:
        if args.keep:
            print(f"work directory kept: {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)

    checks = judge(facts)
    if args.json:
        args.json.write_text(json.dumps({"facts": facts, "checks": checks}, indent=2) + "\n")

    for check in checks:
        print(f"[{'ok' if check['ok'] else 'FAIL'}] {check['check']}: {check['detail']}")
    ok = all(check["ok"] for check in checks)
    print("verdict: " + ("pass" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

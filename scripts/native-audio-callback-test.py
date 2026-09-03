#!/usr/bin/env python3
"""Which thread does the OpenAL Miles replacement deliver end-of-sample callbacks on?

MilesAudioManager::notifyOfAudioCompletion rewrites PlayingAudio/AudioEventRTS state with no lock
against the main thread (`rg AIL_lock MilesAudioManager.cpp` finds nothing), so it is only safe
when the callback runs on the thread that drives the AIL_* API, while that thread is inside the
library. The M1 Pro skirmish crash in docs/porting/playability-probe.md 1.3 is the shim's service
thread calling it while the main thread tore the same PlayingAudio down.

`Core/Libraries/Source/OpenALAudioDevice/tests/openal_callback_thread_test.cpp` drives the public
AIL_* API from one thread and records, per callback, the thread it arrived on and whether the API
thread was inside an AIL_* call. This script builds it against the working tree's shim and judges:

  * nothing arrives while the API thread is out of the library (2D voice, 3D voice, stream);
  * every completion arrives on the API thread, inside an AIL_* call, on the next engine frame;
  * a voice retired the way releaseMilesHandles retires one (unregister, stop) after its source
    ran dry gets no callback at all;
  * a handler that restarts its voice from inside the callback (startNextLoop's shape) keeps
    getting completions, on the API thread, never during the idle window.

`--shim-rev REV` builds the shim from that git revision instead, with the same harness. With
`--expect-defect` the judgement is inverted: the run passes only if the pre-fix delivery on the
service thread is observed. That is the reproduction; the default run is the regression test.

Usage:
    python3 scripts/native-audio-callback-test.py [--json report.json] [--verbose]
    python3 scripts/native-audio-callback-test.py --shim-rev e1f8de610 --expect-defect
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
HARNESS = f"{BACKEND_DIR}/tests/openal_callback_thread_test.cpp"
BACKEND_SOURCES = [
    "OpenALDriver.cpp",
    "OpenALMpeg.cpp",
    "OpenALSample.cpp",
    "OpenAL3DSample.cpp",
    "OpenALStream.cpp",
    "OpenALWaveFile.cpp",
    "OpenALAudioInternal.h",
]


def load_audio_probe():
    """scripts/native-audio-probe.py owns the compile recipe and the dependency lookups."""
    path = REPO_ROOT / "scripts" / "native-audio-probe.py"
    spec = importlib.util.spec_from_file_location("native_audio_probe", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def export_shim(rev, into):
    """The shim's sources at git revision `rev`, written under `into`."""
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

    binary = work / "openal_callback_thread_test"
    command = [probe.CLANGXX, *probe.COMPILE_FLAGS, "-pthread"]
    # The shim's own mss/mss.h (the Miles surface it implements) comes from the working tree even
    # for an exported revision: the fix is below that surface.
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


def judge(facts):
    """(name, ok, detail) per expectation of the fixed shim."""
    checks = []

    def add(name, ok, detail):
        checks.append({"check": name, "ok": bool(ok), "detail": detail})

    add("harness completed", facts.get("completed") is True and facts.get("exit_code") == 0,
        f"exit_code={facts.get('exit_code')} fatal={facts.get('fatal')!r}")

    idle = [facts.get(f"idle_{kind}_callbacks") for kind in ("sample", "object", "stream")]
    add("nothing delivered while the API thread is out of the library",
        idle == [0, 0, 0], f"idle sample/object/stream={idle}")

    for kind in ("sample", "object", "stream"):
        count = facts.get(f"{kind}_callbacks")
        on_thread = facts.get(f"{kind}_on_api_thread")
        inside = facts.get(f"{kind}_inside_api_call")
        add(f"{kind} completion delivered once, on the API thread, inside an AIL_* call",
            count == 1 and on_thread == 1 and inside == 1,
            f"callbacks={count} on_api_thread={on_thread} inside_api_call={inside}")

    add("a voice retired after its source ran dry gets no callback",
        facts.get("retired_voice_callbacks") == 0
        and facts.get("retired_voice_callbacks_after_unregister") == 0,
        f"callbacks={facts.get('retired_voice_callbacks')} "
        f"after_unregister={facts.get('retired_voice_callbacks_after_unregister')}")

    loop = facts.get("loop_callbacks")
    add("a handler restarting its voice keeps completing on the API thread",
        loop == 4 and facts.get("loop_restarts") == 3
        and facts.get("loop_on_api_thread") == loop
        and facts.get("loop_inside_api_call") == loop
        and facts.get("loop_idle_callbacks") == 0,
        f"callbacks={loop} restarts={facts.get('loop_restarts')} "
        f"on_api_thread={facts.get('loop_on_api_thread')} "
        f"inside_api_call={facts.get('loop_inside_api_call')} "
        f"idle={facts.get('loop_idle_callbacks')}")
    return checks


def defect_observed(facts):
    """The pre-fix shim: completions arrive off the API thread while it is idle."""
    off_thread = sum(
        (facts.get(f"{kind}_callbacks") or 0) - (facts.get(f"{kind}_on_api_thread") or 0)
        for kind in ("sample", "object", "stream"))
    idle = sum(facts.get(f"idle_{kind}_callbacks") or 0 for kind in ("sample", "object", "stream"))
    return off_thread > 0 and idle > 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--json", type=pathlib.Path, help="write the report here")
    parser.add_argument("--shim-rev", help="build the shim from this git revision")
    parser.add_argument("--expect-defect", action="store_true",
                        help="pass only if service-thread delivery is observed")
    parser.add_argument("--keep", action="store_true", help="keep the work directory")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    probe = load_audio_probe()
    work = pathlib.Path(tempfile.mkdtemp(prefix="openal-callback-thread-"))
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
        facts = run_harness(binary, args.verbose)
    finally:
        if args.keep:
            print(f"work directory kept: {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)

    checks = judge(facts)
    report = {
        "shim": args.shim_rev or "working tree",
        "expect_defect": args.expect_defect,
        "facts": facts,
        "checks": checks,
        "defect_observed": defect_observed(facts),
    }
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n")

    for check in checks:
        print(f"[{'ok' if check['ok'] else 'FAIL'}] {check['check']}: {check['detail']}")
    print(f"defect observed (service-thread delivery while idle): {report['defect_observed']}")

    if args.expect_defect:
        ok = report["defect_observed"] and facts.get("exit_code") == 0
        print("reproduction: " + ("defect observed" if ok else "DEFECT NOT OBSERVED"))
    else:
        ok = all(check["ok"] for check in checks)
        print("verdict: " + ("pass" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

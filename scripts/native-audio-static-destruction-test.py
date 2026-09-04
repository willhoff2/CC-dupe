#!/usr/bin/env python3
"""Does the OpenAL Miles replacement survive process exit without AIL_shutdown?

The shim's process-wide state (`OpenALAudio::lib()`, a function-local static) owns the service
thread, and a `std::thread` still joinable when its destructor runs is `std::terminate`. A quit that
reached static destruction without the engine's AIL_shutdown therefore aborted:
`terminate called without an active exception`, `std::thread::~thread` <-
`OpenALAudio::Library::~Library` <- `exit`. docs/porting/memory-shutdown-order.md has the account.

`Core/Libraries/Source/OpenALAudioDevice/tests/openal_static_destruction_test.cpp` starts the
library, plays a 2D voice, a 3D voice and a looping stream, and returns from main with all of them
playing and AIL_shutdown never called. This script builds it against the working tree's shim and
runs it three times: diagnostics off, `OPENAL_AUDIO_DIAG=stderr`, and `OPENAL_AUDIO_DIAG=<file>`.
Each run must exit 0 with `completed: true` on stdout, and the file run must leave a readable log
whose last line is the `static-destruction` counters report.

`--shim-rev REV` builds the shim from that git revision instead. With `--expect-defect` the
judgement is inverted: the run passes only if every variant died of SIGABRT after printing its
JSON. That is the reproduction (c6fd1bd7c and fbfc0f574 both abort); the default run is the
regression test.

Usage:
    python3 scripts/native-audio-static-destruction-test.py [--json report.json] [--verbose]
    python3 scripts/native-audio-static-destruction-test.py --shim-rev fbfc0f574 --expect-defect
"""

import argparse
import importlib.util
import json
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
BACKEND_DIR = "Core/Libraries/Source/OpenALAudioDevice"
HARNESS = f"{BACKEND_DIR}/tests/openal_static_destruction_test.cpp"
BACKEND_SOURCES = [
    "OpenALDriver.cpp",
    "OpenALMpeg.cpp",
    "OpenALSample.cpp",
    "OpenAL3DSample.cpp",
    "OpenALStream.cpp",
    "OpenALWaveFile.cpp",
    "OpenALAudioInternal.h",
]

VARIANTS = ("off", "stderr", "file")


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

    binary = work / "openal_static_destruction_test"
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


def run_variant(binary, variant, work, verbose):
    env = dict(os.environ)
    env["ALSOFT_DRIVERS"] = "null"
    env.setdefault("ALSOFT_LOGLEVEL", "0")
    env.pop("OPENAL_AUDIO_DIAG", None)
    env.pop("OPENAL_AUDIO_DIAG_STALL", None)
    log = None
    if variant == "stderr":
        env["OPENAL_AUDIO_DIAG"] = "stderr"
    elif variant == "file":
        log = work / f"diag-{variant}.log"
        if log.exists():
            log.unlink()
        env["OPENAL_AUDIO_DIAG"] = str(log)

    result = subprocess.run([str(binary)], capture_output=True, text=True, env=env, timeout=120)
    try:
        facts = json.loads(result.stdout)
    except json.JSONDecodeError:
        facts = {"fatal": "harness produced no JSON", "stdout": result.stdout}
    facts["variant"] = variant
    facts["exit_code"] = result.returncode
    facts["aborted"] = result.returncode == -signal.SIGABRT
    stderr = result.stderr.strip()
    facts["terminate_message"] = "terminate called" in stderr
    facts["stderr_static_destruction_report"] = "counters static-destruction" in stderr
    if log is not None:
        text = log.read_text() if log.exists() else ""
        lines = [line for line in text.splitlines() if line.strip()]
        facts["log_lines"] = len(lines)
        facts["log_last_line_is_static_destruction_report"] = (
            bool(lines) and " counters static-destruction " in lines[-1])
    if stderr:
        facts["stderr"] = stderr[-4000:]
    if verbose:
        print(json.dumps(facts, indent=2))
    return facts


def judge(runs):
    """(name, ok, detail) per expectation of the fixed shim."""
    checks = []

    def add(name, ok, detail):
        checks.append({"check": name, "ok": bool(ok), "detail": detail})

    for facts in runs:
        variant = facts["variant"]
        add(f"[{variant}] harness ran every voice and returned from main without AIL_shutdown",
            facts.get("completed") is True and facts.get("shutdown_called") is False,
            f"completed={facts.get('completed')} shutdown_called={facts.get('shutdown_called')} "
            f"fatal={facts.get('fatal')!r}")
        add(f"[{variant}] process exited 0 through static destruction, no std::terminate",
            facts.get("exit_code") == 0 and not facts.get("terminate_message"),
            f"exit_code={facts.get('exit_code')} "
            f"terminate_message={facts.get('terminate_message')}")

    by_variant = {facts["variant"]: facts for facts in runs}
    stderr_run = by_variant.get("stderr", {})
    add("[stderr] the destructor reported the skipped shutdown to the diagnostics log",
        stderr_run.get("stderr_static_destruction_report") is True,
        f"report_seen={stderr_run.get('stderr_static_destruction_report')}")
    file_run = by_variant.get("file", {})
    add("[file] the diagnostics file was flushed and closed; its last line is the report",
        file_run.get("log_last_line_is_static_destruction_report") is True,
        f"log_lines={file_run.get('log_lines')} "
        f"last_is_report={file_run.get('log_last_line_is_static_destruction_report')}")
    return checks


def defect_observed(runs):
    """The pre-fix shim: every variant prints its JSON and then dies of std::terminate."""
    return bool(runs) and all(
        facts.get("completed") is True and facts.get("aborted") and facts.get("terminate_message")
        for facts in runs)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--json", type=pathlib.Path, help="write the report here")
    parser.add_argument("--shim-rev", help="build the shim from this git revision")
    parser.add_argument("--expect-defect", action="store_true",
                        help="pass only if every variant aborts with std::terminate")
    parser.add_argument("--keep", action="store_true", help="keep the work directory")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    probe = load_audio_probe()
    work = pathlib.Path(tempfile.mkdtemp(prefix="openal-static-destruction-"))
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
        runs = [run_variant(binary, variant, work, args.verbose) for variant in VARIANTS]
    finally:
        if args.keep:
            print(f"work directory kept: {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)

    checks = judge(runs)
    report = {
        "shim": args.shim_rev or "working tree",
        "expect_defect": args.expect_defect,
        "runs": runs,
        "checks": checks,
        "defect_observed": defect_observed(runs),
    }
    if args.json:
        args.json.write_text(json.dumps(report, indent=2) + "\n")

    for check in checks:
        print(f"[{'ok' if check['ok'] else 'FAIL'}] {check['check']}: {check['detail']}")
    print(f"defect observed (std::terminate at static destruction): {report['defect_observed']}")

    if args.expect_defect:
        ok = report["defect_observed"]
        print("reproduction: " + ("defect observed" if ok else "DEFECT NOT OBSERVED"))
    else:
        ok = all(check["ok"] for check in checks)
        print("verdict: " + ("pass" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

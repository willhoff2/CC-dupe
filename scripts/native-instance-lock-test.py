#!/usr/bin/env python3
"""Make rts::ClientInstance::initialize() refuse to start, and require it to say why.

A leftover client still holding the instance lock turned a dozen launches into silence: the process
returned from main with nothing on stdout or stderr, and lsof was the only way to find the holder
(docs/porting/real-input-menu-drive.md 4.4). The refusal is right; being unable to see it is not.

Core/GameEngine/Source/GameClient/tests/instance_lock_test.cpp drives the engine's real
ClientInstance and the real POSIX lock out of the archives scripts/native-build.py produced, in
three states -- lock free, lock held, lock unusable. This script supplies the states and the
judgement:

  * held: the diagnostic must name the lock file and the holder's pid, because those are the two
    things a person needs to act. A message that only says "already running" is what the process
    effectively said before.
  * unusable: a directory that cannot be written produces the same `false` for a completely
    different reason, and must be reported as that reason rather than as another instance.
  * free: nothing may be printed at all. This is the control -- a diagnostic that also fires on the
    success path teaches everyone to ignore it -- and it is also what proves the two failing cases
    printed *because* they failed.

Usage:
    python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \\
        --with-shims --strict-link          # must run first: this uses its archives
    python3 scripts/native-instance-lock-test.py [--keep] [--verbose]

Exits non-zero if a refusal is silent, if it names neither the file nor the holder, or if the
success path has become chatty. CLANGXX selects the compiler.
"""

import argparse
import importlib.util
import os
import pathlib
import shutil
import stat
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
HARNESS = REPO_ROOT / "Core/GameEngine/Source/GameClient/tests/instance_lock_test.cpp"
# ClientInstance.cpp's own flags: it is what the harness drives.
FLAG_DONOR = "ClientInstance.cpp"


def load_render_runner():
    """The render harness's script, imported: it owns the compile/link recipe."""
    path = REPO_ROOT / "scripts" / "native-render-backend-run.py"
    spec = importlib.util.spec_from_file_location("native_render_backend_run", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def run_case(binary, case, runtime_directory, environment):
    child = dict(environment)
    child["XDG_RUNTIME_DIR"] = str(runtime_directory)
    # TMPDIR is the fallback the lock uses on macOS; pinning both keeps the case in the scratch
    # directory on either platform.
    child["TMPDIR"] = str(runtime_directory)
    return subprocess.run([str(binary), case], capture_output=True, text=True, env=child)


def show(label, proc):
    print(f"--- {label} (exit {proc.returncode})")
    sys.stdout.write(proc.stdout)
    if proc.stderr.strip():
        sys.stdout.write(proc.stderr)
    print()


def value_after(stdout, prefix):
    """-> what the harness printed after `prefix`, or None."""
    for line in stdout.splitlines():
        if line.startswith(prefix):
            return line[len(prefix):].strip()
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--keep", action="store_true", help="keep the scratch directory")
    parser.add_argument("--verbose", action="store_true", help="echo the compiler output")
    args = parser.parse_args()

    if sys.platform.startswith("win"):
        print("the lock is a named mutex on Windows and this diagnostic is the POSIX one; nothing "
              "to run here")
        return 0

    runner = load_render_runner()
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="instance-lock-test-"))
    failures = []
    try:
        harness_object = scratch / "instance_lock_test.o"
        output = runner.compile_harness(harness_object, harness=HARNESS, donor=FLAG_DONOR)
        if args.verbose and output.strip():
            print(output)

        (scratch / "archives").mkdir()
        archives = runner.scratch_archives(scratch / "archives")
        binary = scratch / "instance_lock_test"
        output = runner.link_harness([harness_object], binary, archives)
        if args.verbose and output.strip():
            print(output)

        environment, _ = runner.run_environment()

        # ---- the lock is free: it works, and quietly
        free_directory = scratch / "runtime-free"
        free_directory.mkdir()
        free = run_case(binary, "free", free_directory, environment)
        show("the lock is free", free)
        if free.returncode != 0:
            failures.append("initialize() did not succeed with the lock free, so the two failing "
                            "cases below cannot be attributed to the lock being held")
        if free.stderr.strip():
            failures.append("initialize() wrote to stderr on the path where it succeeded: a "
                            "diagnostic that fires when nothing is wrong is one nobody reads")

        # ---- the lock is held: the case that cost a dozen launches
        held_directory = scratch / "runtime-held"
        held_directory.mkdir()
        held = run_case(binary, "held", held_directory, environment)
        show("the lock is held", held)
        if held.returncode != 0:
            failures.append(f"the held case failed its own checks (exit {held.returncode}): "
                            "flock() did not refuse the second descriptor, so this platform cannot "
                            "reproduce the state that made launches fail")
        else:
            expected_path = value_after(held.stdout, "lock file:")
            holder = value_after(held.stdout, "holder pid:")
            if not held.stderr.strip():
                failures.append("initialize() refused to start and said nothing on stderr, which "
                                "is the whole defect this is about")
            if expected_path and expected_path not in held.stderr:
                failures.append(f"the diagnostic does not name the lock file ({expected_path}), so "
                                "it does not say where to look")
            if holder and holder not in held.stderr:
                failures.append(f"the diagnostic does not name the holder's pid ({holder}), which "
                                "is the one fact that makes it actionable")
            else:
                print(f"the diagnostic named the holder: pid {holder}")

        # ---- the lock cannot be taken at all: same false, different cause
        unusable_directory = scratch / "runtime-unusable"
        unusable_directory.mkdir()
        unusable_directory.chmod(stat.S_IRUSR | stat.S_IXUSR)
        if os.geteuid() == 0:
            print("running as root, so an unwritable directory is still writable: skipping the "
                  "unusable case rather than passing it\n")
        else:
            unusable = run_case(binary, "unusable", unusable_directory, environment)
            show("the lock cannot be taken at all", unusable)
            if unusable.returncode != 0:
                failures.append(f"the unusable case failed its own checks (exit "
                                f"{unusable.returncode})")
            elif "Cannot take the single instance lock" not in unusable.stderr:
                failures.append("an unusable lock is still reported as another instance already "
                                "running; the two causes have the same return value and only the "
                                "diagnostic can tell them apart")
        unusable_directory.chmod(stat.S_IRWXU)

        if failures:
            print()
            for failure in failures:
                print(f"FAILED: {failure}")
            return 1

        print("\nOK: a refused launch names the lock file and the process holding it, an unusable "
              "lock is reported as that and not as a running game, and a successful launch is "
              "silent")
        return 0
    finally:
        if args.keep:
            print(f"scratch directory kept at {scratch}")
        else:
            shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

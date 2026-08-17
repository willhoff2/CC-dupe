#!/usr/bin/env python3
"""Run the engine's allocator through a static destructor that allocates after main returned.

`Core/GameEngine/Source/Common/System/tests/memory_shutdown_test.cpp` initialises the game's memory
manager the way main()'s prologue does, calls shutdownMemoryManager(), and then -- from a static
destructor, after main returned -- frees and allocates again through the engine's global operator
new/delete. That is the shape that killed every Apple Silicon run with SIGSEGV after a clean
shutdown: OpenAL Soft's static destructors allocate, the allocator takes a CriticalSection that
main()'s plain statics had already destroyed, locking it reports an error, building that error's
message allocates, and the recursion runs the stack out
(docs/porting/apple-silicon-verification.md 8.5, docs/porting/memory-shutdown-order.md).

What this script adds to the harness is the part a harness cannot do for itself: it decides the run
was clean.

  * the process exits 0 -- not a signal. A stack overflow shows up here as a negative return code,
    which is what the retail process did (-11).
  * the late destructor's own lines are on stdout. They are printed after the memory manager was
    shut down, so a process that dies on the way out cannot produce them however clean its main
    looked.
  * the harness's in-process control held: a plain static CriticalSection -- the arrangement
    PlatformMain.cpp used to have -- was already destroyed at the moment the late allocation
    arrived, while the immortal one the allocator now holds was not. A run where the plain section
    was still alive fails here rather than passing, because it would not have tested the ordering
    at all.

The crash itself is not portable: locking a destroyed std::recursive_mutex is undefined and glibc
accepts it, so this asserts the order that caused it, on Linux, and the exit status on macOS.

The harness is linked against the archives scripts/native-build.py produced, with the compile flags
and the link recipe read out of scripts/native-render-backend-run.py so the two cannot drift.

Usage:
    python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \\
        --with-shims --strict-link          # must run first: this uses its archives
    python3 scripts/native-memory-shutdown-test.py [--keep] [--verbose]
"""

import argparse
import importlib.util
import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
HARNESS = REPO_ROOT / "Core/GameEngine/Source/Common/System/tests/memory_shutdown_test.cpp"
# The translation unit whose compile command is reused: the memory manager's own, so the harness is
# compiled with exactly the flags the allocator it links against was compiled with.
FLAG_DONOR = "GameMemory.cpp"
# Printed from the static destructor, i.e. after main returned and after the manager was shut down.
LATE_FREE_MARKER = "late free completed"
# The harness's in-process control: the pre-fix arrangement was destroyed before this point. Matched
# by the question rather than the whole line, so the column the answer is padded to is not a
# protocol between the two files.
CONTROL_QUESTION = "a plain static section is already destroyed by now"
SECTION_ALIVE_QUESTION = "the section the allocator holds is still alive"


def load_render_runner():
    """The render harness's script, imported: it owns the compile/link recipe."""
    path = REPO_ROOT / "scripts" / "native-render-backend-run.py"
    spec = importlib.util.spec_from_file_location("native_render_backend_run", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def answered_yes(stdout, question):
    """-> True when the harness printed `question` and answered it "yes"."""
    for line in stdout.splitlines():
        if line.startswith(question):
            return line.split()[-1] == "yes"
    return False


def classify(proc):
    """-> [reason]. Empty when the run demonstrated the ordering; a reason each when it did not."""
    failures = []
    if proc.returncode != 0:
        failures.append(f"the process exited {proc.returncode}; a negative status is the signal a "
                        "stack overflow in the static destructors raises")
    if LATE_FREE_MARKER not in proc.stdout:
        failures.append("the static destructor's own output is missing, so the allocation after "
                        "shutdownMemoryManager() did not complete")
    if not answered_yes(proc.stdout, CONTROL_QUESTION):
        failures.append("the harness's plain static critical section was still alive when the late "
                        "allocation arrived, so this run did not exercise the destruction order "
                        "the crash came from and its pass would mean nothing")
    if not answered_yes(proc.stdout, SECTION_ALIVE_QUESTION):
        failures.append("the section the allocator holds was gone by the late allocation: that is "
                        "the pre-fix lifetime, which is what this fix removes")
    if "FAILED" in proc.stdout:
        failures.append("a harness check failed")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--keep", action="store_true", help="keep the scratch link directory")
    parser.add_argument("--verbose", action="store_true", help="echo the compiler output")
    args = parser.parse_args()

    if sys.platform.startswith("win"):
        print("this harness measures the off-Windows static-destruction order; Windows destroys "
              "the pools inside shutdownMemoryManager() and always has")
        return 0

    runner = load_render_runner()
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="memory-shutdown-test-"))
    try:
        output = runner.compile_harness(scratch / "memory_shutdown_test.o",
                                        harness=HARNESS, donor=FLAG_DONOR)
        if args.verbose and output.strip():
            print(output)
        archives = runner.scratch_archives(scratch)
        binary = scratch / "memory_shutdown_test"
        output = runner.link_harness([scratch / "memory_shutdown_test.o"], binary, archives)
        if args.verbose and output.strip():
            print(output)

        environment, _ = runner.run_environment()
        proc = subprocess.run([str(binary)], capture_output=True, text=True, env=environment)
        sys.stdout.write(proc.stdout)
        if proc.stderr.strip():
            sys.stdout.write(proc.stderr)
        print(f"exit status: {proc.returncode}")

        failures = classify(proc)

        if failures:
            print()
            for failure in failures:
                print(f"FAILED: {failure}")
            return 1

        print("\nOK: a static destructor allocated and freed through the engine's operator "
              "new/delete after shutdownMemoryManager(), the critical section the allocator takes "
              "was still alive while a plain static one was already gone, and the process exited 0")
        return 0
    finally:
        if args.keep:
            print(f"scratch directory kept at {scratch}")
        else:
            shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Destroy the WWLib object pool from the quit crash report the way exit() destroys it.

`Core/GameEngine/Source/Common/System/tests/exit_teardown_test.cpp` takes 300 nodes out of the real
`ObjectPoolClass<MultiListNodeClass, 256>` -- the pool named in every Apple Silicon quit crash
report (docs/porting/playability-probe.md 7) -- writes to every one of them, calls
shutdownMemoryManager() the way PlatformMain.cpp does, and lets exit() destroy the pool afterwards.
A second pool of the same template lives in the harness with its block list readable, so the chain
the faulting destructor walks is checked directly rather than inferred from a clean exit.

What this script adds is the judgement and the negative control:

  * the shipping binary must exit 0 and must reach the reporter that is destroyed after the pools.
    Before the fix it died of SIGSEGV in that destructor, which is exit -11 here and a crash report
    on macOS, so a negative status is the retail symptom rather than a harness accident.
  * `EXIT_TEARDOWN_CASE=control` builds the pre-fix pool template, arithmetic verbatim, and must
    find the block's next-block pointer overwritten by the first object -- read, never dereferenced.
    Without that control a clean shipping run would prove nothing: a 32-bit build cannot have this
    defect at all, which is exactly why Windows never crashed here.

The harness is linked against the archives scripts/native-build.py produced, with the compile flags
and the link recipe read out of scripts/native-render-backend-run.py so the two cannot drift.

Usage:
    python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \\
        --with-shims --strict-link          # must run first: this uses its archives
    python3 scripts/native-exit-teardown-test.py [--keep] [--verbose]
"""

import argparse
import importlib.util
import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
HARNESS = REPO_ROOT / "Core/GameEngine/Source/Common/System/tests/exit_teardown_test.cpp"
# The memory manager's own translation unit, so the harness is compiled with the flags the allocator
# it links against was compiled with.
FLAG_DONOR = "GameMemory.cpp"
# WWLib's own include roots: mempool.h and multilist.h are below the GameEngine ones the donor has.
EXTRA_INCLUDES = ("Core/Libraries/Source/WWVegas", "Core/Libraries/Source/WWVegas/WWLib")

# Printed from the reporter destroyed after the pools, so a process that dies during static
# destruction cannot produce it.
COMPLETION_MARKER = "exit teardown completed"
# The two checks that are the mechanism itself, on either side of the fix.
INTACT_QUESTION = "the block list is walkable and terminated after the objects were written"
CONTROL_QUESTION = "the pre-fix arithmetic overwrites the block's next-block pointer"


def load_render_runner():
    """The render harness's script, imported: it owns the compile/link recipe."""
    path = REPO_ROOT / "scripts" / "native-render-backend-run.py"
    spec = importlib.util.spec_from_file_location("native_render_backend_run", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check_passed(stdout, question):
    """-> True when the harness printed `question` and answered it "ok"."""
    for line in stdout.splitlines():
        if line.startswith(question):
            return line.split()[len(question.split())] == "ok"
    return False


def run_case(binary, case, environment):
    """Run one case. -> the CompletedProcess."""
    child = dict(environment)
    child["EXIT_TEARDOWN_CASE"] = case
    return subprocess.run([str(binary)], capture_output=True, text=True, env=child)


def show(label, proc):
    print(f"--- {label} (exit {proc.returncode})")
    sys.stdout.write(proc.stdout)
    if proc.stderr.strip():
        sys.stdout.write(proc.stderr)
    print()


def classify_shipping(proc):
    """-> [reason]. Empty when the run demonstrated a clean exit-time teardown."""
    failures = []
    if proc.returncode != 0:
        failures.append(f"the shipping case exited {proc.returncode}; -11 is the SIGSEGV the "
                        "retail quit raised in this very destructor, and any non-zero status "
                        "means a check in the harness failed")
    if COMPLETION_MARKER not in proc.stdout:
        failures.append("the reporter destroyed after the pool destructors did not run, so the "
                        "process did not survive its own static destruction")
    if not check_passed(proc.stdout, INTACT_QUESTION):
        failures.append("the pool's block list was not intact after its objects were written, "
                        "which is the corruption the faulting destructor walked")
    if "FAILED" in proc.stdout:
        failures.append("a harness check failed")
    return failures


def classify_control(proc):
    """-> [reason]. Empty when the control reproduced the pre-fix corruption."""
    failures = []
    if not check_passed(proc.stdout, CONTROL_QUESTION):
        failures.append("the control did not see the pre-fix arithmetic overwrite the block's "
                        "next-block pointer, so it is not reproducing the defect and the "
                        "shipping case's clean run proves nothing")
    if proc.returncode != 0:
        failures.append(f"the control exited {proc.returncode}: it is meant to detect the "
                        "corruption without dereferencing it, so it should not fail or crash")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--keep", action="store_true", help="keep the scratch link directory")
    parser.add_argument("--verbose", action="store_true", help="echo the compiler output")
    args = parser.parse_args()

    runner = load_render_runner()
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="exit-teardown-test-"))
    failures = []
    try:
        output = runner.compile_harness(scratch / "exit_teardown_test.o", harness=HARNESS,
                                        donor=FLAG_DONOR, extra_includes=EXTRA_INCLUDES)
        if args.verbose and output.strip():
            print(output)
        archives = runner.scratch_archives(scratch)
        binary = scratch / "exit_teardown_test"
        output = runner.link_harness([scratch / "exit_teardown_test.o"], binary, archives)
        if args.verbose and output.strip():
            print(output)

        environment, _ = runner.run_environment()

        shipping = run_case(binary, "shipping", environment)
        show("shipping: exit() destroys the real MultiListNodeClass pool", shipping)
        failures += classify_shipping(shipping)

        control = run_case(binary, "control", environment)
        show("control: the pre-fix arithmetic, which puts the first object on the block header",
             control)
        failures += classify_control(control)

        if failures:
            print()
            for reason in failures:
                print(f"FAILED: {reason}")
            return 1

        print("\nOK: the pre-fix arithmetic overwrites the block's next-block pointer (control), "
              "the shipping pools hand out aligned objects clear of that header, their block "
              "lists stay walkable, and exit() destroys the real pool with the process exiting 0")
        print("UNMEASURED: this is Linux x86-64. Apple Silicon is the same LP64 arithmetic, but "
              "the three retail quit paths on the Mac are owed as a follow-up "
              "(docs/porting/memory-shutdown-order.md)")
        return 0
    finally:
        if args.keep:
            print(f"scratch directory kept at {scratch}")
        else:
            shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

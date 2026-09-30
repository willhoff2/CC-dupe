#!/usr/bin/env python3
"""End a game mid-scroll through the real LookAtTranslator and check the tactical view unlocks.

`Core/GameEngine/Source/GameClient/MessageStream/tests/lookat_reset_modes_test.cpp` starts a key
scroll and a right-drag scroll with the raw messages a player produces, runs what
`ScriptActions::doDisableInput()` runs when a skirmish is won or lost, and asserts that
`TheTacticalView->isMouseLocked()` is false afterwards -- the condition under which
`WindowTranslator` drops every message before the score screen's buttons see it. Before the fix
`LookAtTranslator::resetModes()` cleared the scroll flag without `stopScrolling()`, the lock stayed
set, and the checks fail. See docs/porting/game-end-mouse-lock.md.

The harness is linked against the archives scripts/native-build.py produced, with the compile flags
and the link recipe read out of scripts/native-render-backend-run.py, as
scripts/native-exit-teardown-test.py does.

Usage:
    python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \\
        --with-shims --strict-link          # must run first: this uses its archives
    python3 scripts/native-lookat-reset-modes-test.py [--keep] [--verbose]
"""

import argparse
import importlib.util
import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
HARNESS = (REPO_ROOT /
           "Core/GameEngine/Source/GameClient/MessageStream/tests/lookat_reset_modes_test.cpp")
# The translation unit under test, so the harness is compiled with exactly its flags.
FLAG_DONOR = "LookAtXlat.cpp"


def load_render_runner():
    """The render harness's script, imported: it owns the compile/link recipe."""
    path = REPO_ROOT / "scripts" / "native-render-backend-run.py"
    spec = importlib.util.spec_from_file_location("native_render_backend_run", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--keep", action="store_true", help="keep the scratch link directory")
    parser.add_argument("--verbose", action="store_true", help="echo the compiler output")
    args = parser.parse_args()

    runner = load_render_runner()
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="lookat-reset-modes-test-"))
    try:
        harness_object = scratch / "lookat_reset_modes_test.o"
        output = runner.compile_harness(harness_object, harness=HARNESS, donor=FLAG_DONOR)
        if args.verbose and output.strip():
            print(output)
        binary = scratch / "lookat_reset_modes_test"
        output = runner.link_harness([harness_object], binary,
                                     runner.scratch_archives(scratch))
        if args.verbose and output.strip():
            print(output)

        environment, _ = runner.run_environment()
        proc = subprocess.run([str(binary)], capture_output=True, text=True, env=environment)
        sys.stdout.write(proc.stdout)
        if proc.stderr.strip():
            sys.stdout.write(proc.stderr)
        if proc.returncode < 0:
            print(f"\nFAILED: the harness died on signal {-proc.returncode} before answering")
            return 1
        if proc.returncode != 0:
            print("\nFAILED: a game that ends while the camera scrolls leaves the tactical view "
                  "mouse-locked, and every shell button on the score screen is dead")
            return 1
        print("\nOK: ending the game mid-scroll unlocks the tactical view, and scrolling still "
              "locks and unlocks it afterwards")
        return 0
    finally:
        if args.keep:
            print(f"scratch directory kept at {scratch}")
        else:
            shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

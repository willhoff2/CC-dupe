#!/usr/bin/env python3
"""Leave a tunnel-network guard state current without its attack state, and update it.

The tunnel guard's inner, outer and attack-aggressor states return STATE_SUCCESS from onEnter()
before allocating m_attackState when there is no nemesis (the outer state also in
GUARDMODE_GUARD_WITHOUT_PURSUIT), and their update() then dereferenced it unconditionally. Such a
state stays current when State::friend_checkForTransitions() refuses its success transition at the
20-level nesting cutoff. The measured crash, an 8-player skirmish on an M1 Pro, was a read at 0x38
-- State::m_machine through the null AIAttackState -- in AITNGuardOuterState::update(). AIGuard's
twin states have checked since the source release.
See docs/porting/tunnel-guard-null-attack-state.md.

Two halves, because the states are not equally reachable without a real Object:

  * A COMPILED test (tests/tunnel_guard_null_attack_state_test.cpp) links the real StateMachine and
    AITNGuard.cpp from the native build's archives, enters INNER with no nemesis with the
    transitions nested to the cutoff, checks which state the machine leaves current, and updates
    OUTER (both guard modes) and the attack-aggressor state, each in a forked child that reports a
    fault's address. Before the fix each faults at 0x38.
  * A SOURCE SCAN over both game trees' AITNGuard.cpp, asserting that every `m_attackState->` in
    the three update() bodies is dominated by `if (m_attackState == nullptr) return ...;` in an
    enclosing block. That is what pins AITNGuardInnerState::update(), which reads the owner's team
    and player before it reaches its attack state, so it cannot run without a constructed Object.

The harness is linked with the compile flags and the link recipe in
scripts/native-render-backend-run.py, as scripts/native-lookat-reset-modes-test.py is.

Usage:
    python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \\
        --with-shims --strict-link          # must run first: this uses its archives
    python3 scripts/native-tunnel-guard-null-attack-state-test.py [--keep] [--verbose]
"""

import argparse
import importlib.util
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
HARNESS = (REPO_ROOT / "GeneralsMD/Code/GameEngine/Source/GameLogic/AI/tests/"
           "tunnel_guard_null_attack_state_test.cpp")
# The translation unit under test, so the harness is compiled with exactly its flags.
FLAG_DONOR = "AITNGuard.cpp"

SCANNED = [
    "GeneralsMD/Code/GameEngine/Source/GameLogic/AI/AITNGuard.cpp",
    "Generals/Code/GameEngine/Source/GameLogic/AI/AITNGuard.cpp",
]
GUARDED_UPDATES = ["AITNGuardInnerState", "AITNGuardOuterState", "AITNGuardAttackAggressorState"]
GUARD = re.compile(r"if\s*\(\s*m_attackState\s*==\s*nullptr\s*\)\s*return\b")


def load_render_runner():
    """The render harness's script, imported: it owns the compile/link recipe."""
    path = REPO_ROOT / "scripts" / "native-render-backend-run.py"
    spec = importlib.util.spec_from_file_location("native_render_backend_run", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def update_body(lines, class_name):
    """The lines of `class_name::update()`, from its opening brace to the matching close."""
    header = re.compile(rf"^StateReturnType\s+{class_name}::update\(\)")
    start = next(index for index, line in enumerate(lines) if header.match(line))
    depth = 0
    for index in range(start, len(lines)):
        depth += lines[index].count("{") - lines[index].count("}")
        if depth == 0 and "}" in lines[index]:
            return start, lines[start:index + 1]
    raise ValueError(f"unterminated {class_name}::update()")


def unguarded_dereferences(body):
    """Line offsets of `m_attackState->` not dominated by an early-return null guard.

    In structured code with no goto, a guard that returns dominates every later statement of its
    own block and of the blocks nested in it, so one flag per open block is the whole analysis.
    """
    guarded = [False]
    found = []
    for offset, line in enumerate(body):
        code = line.split("//", 1)[0]
        if GUARD.search(code):
            guarded[-1] = True
        elif "m_attackState->" in code and not guarded[-1]:
            found.append(offset)
        for character in code:
            if character == "{":
                guarded.append(guarded[-1])
            elif character == "}":
                guarded.pop()
    return found


def scan_sources():
    findings = []
    for relative in SCANNED:
        lines = (REPO_ROOT / relative).read_text().splitlines()
        for class_name in GUARDED_UPDATES:
            start, body = update_body(lines, class_name)
            for offset in unguarded_dereferences(body):
                findings.append(f"{relative}:{start + offset + 1}: {class_name}::update() "
                                f"dereferences m_attackState without a null guard")
    if findings:
        print("FAILED: the source scan found unguarded attack-state dereferences")
        for finding in findings:
            print(f"  {finding}")
        return 1
    print(f"OK: every m_attackState dereference in {len(GUARDED_UPDATES)} update() bodies in "
          f"{len(SCANNED)} files is null-guarded")
    return 0


def run_harness(args):
    runner = load_render_runner()
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="tunnel-guard-null-attack-state-test-"))
    try:
        harness_object = scratch / "tunnel_guard_null_attack_state_test.o"
        output = runner.compile_harness(harness_object, harness=HARNESS, donor=FLAG_DONOR)
        if args.verbose and output.strip():
            print(output)
        binary = scratch / "tunnel_guard_null_attack_state_test"
        output = runner.link_harness([harness_object], binary, runner.scratch_archives(scratch))
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
            print("\nFAILED: a tunnel guard state with no attack state faults in update()")
            return 1
        print("\nOK: tunnel guard states with no attack state return STATE_SUCCESS from update()")
        return 0
    finally:
        if args.keep:
            print(f"scratch directory kept at {scratch}")
        else:
            shutil.rmtree(scratch, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--keep", action="store_true", help="keep the scratch link directory")
    parser.add_argument("--verbose", action="store_true", help="echo the compiler output")
    args = parser.parse_args()

    harness_result = run_harness(args)
    scan_result = scan_sources()
    return harness_result or scan_result


if __name__ == "__main__":
    sys.exit(main())

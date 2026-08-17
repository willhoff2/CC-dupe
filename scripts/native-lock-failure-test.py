#!/usr/bin/env python3
"""Make the allocator's critical-section lock fail on purpose, twice: with the fix and without it.

`Core/GameEngine/Source/Common/System/tests/lock_failure_test.cpp` links the real engine archives
and puts the real allocator in front of a critical section whose lifetime has ended -- once mid-run
and once through static destruction order, which is how the retail process reached it. What this
script adds is the judgement, and the negative control:

  * the binary as it ships must **abort with exactly one diagnostic line**. The reporter in
    Common/System/CriticalSectionFailure.cpp does not allocate, so the failure is bounded: one
    write(2) naming the section, the errno and the mutex's bytes, then abort(). SIGABRT (-6), not
    SIGSEGV (-11).
  * the same source compiled with -DLOCK_FAILURE_NEGATIVE_CONTROL, linked against archives with
    CriticalSectionFailure.cpp.o removed, supplies the standard library's failure path instead --
    throw std::system_error, whose message is a std::string, whose memory is the engine's operator
    new. That binary must die of the recursion, and must print a depth well past anything a
    bounded path could reach. Without that control the fix has nothing to be a fix *of*
    (docs/porting/real-input-menu-drive.md 4.3 was intermittent on hardware and unreproducible
    here).
  * the platform case is the control for both: locking a destroyed mutex is undefined, and if this
    platform's runtime grants the lock, the destroyed-section cases cannot fail and their pass
    would be meaningless. That is a real difference between runtimes -- a destroyed
    std::recursive_mutex was accepted by libstdc++, which is why Linux CI never saw the defect the
    Apple runtime crashed on. A pthread_mutex_t is destroyed by pthread_mutex_destroy() on both.
  * the exit-immortal case is the lifetime fix #113 landed, measured here as the contrast: with
    immortal sections the failure does not happen at all, and the late free completes.

The cases are separate processes, chosen with LOCK_FAILURE_CASE because two of them act before main
runs.

Usage:
    python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \\
        --with-shims --strict-link          # must run first: this uses its archives
    python3 scripts/native-lock-failure-test.py [--keep] [--verbose]

On macOS this is also the script that answers the question the crash reports do not: run it and read
the platform case's four lines, and the diagnostic each failing case prints, which carries the errno
and the mutex's first bytes.
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
HARNESS = REPO_ROOT / "Core/GameEngine/Source/Common/System/tests/lock_failure_test.cpp"
# The allocator's own translation unit, so the harness is compiled with the flags the allocator it
# links against was compiled with.
FLAG_DONOR = "GameMemory.cpp"
# The member the negative control's archives must not contain: the fix itself.
REPORTER_MEMBER = "CriticalSectionFailure.cpp.o"

DIAGNOSTIC_RE = re.compile(r"^CriticalSection::(\w+) failed: (\S+) \((\d+)\) on (\S+)")
# A bounded failure path cannot get anywhere near this; the control's counter prints every 1000.
CONTROL_MINIMUM_DEPTH = 1000

SIGABRT = -6
SIGSEGV = -11
SIGBUS = -10


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
        if line.startswith(question) and line.split()[-1] == "yes":
            return True
    return False


def diagnostics(text):
    """-> [(operation, errno name, errno, section)] for every reporter line in `text`."""
    found = []
    for line in text.splitlines():
        match = DIAGNOSTIC_RE.match(line)
        if match:
            found.append(match.groups())
    return found


def control_depth(text):
    """-> the deepest recursion the control reported, or 0."""
    depths = [int(m.group(1)) for m in re.finditer(r"^control: recursion depth (\d+)$", text,
                                                   re.MULTILINE)]
    return max(depths) if depths else 0


def strip_reporter(archives):
    """Remove the fix from the scratch archives. -> the archive it was removed from.

    `ar d` on the copy scratch_archives() made, the same way the render runner deletes the game's
    main() from its copy: the control differs from the shipping binary in the failure path and
    nothing else.
    """
    for archive in archives:
        listing = subprocess.run(["ar", "t", str(archive)], capture_output=True, text=True).stdout
        if REPORTER_MEMBER in listing.split():
            subprocess.run(["ar", "d", str(archive), REPORTER_MEMBER], check=True)
            return archive
    sys.exit(f"no archive contains {REPORTER_MEMBER}: nothing to revert, so there is no negative "
             "control. Re-run scripts/native-build.py.")


def run_case(binary, case, environment):
    child = dict(environment)
    child["LOCK_FAILURE_CASE"] = case
    return subprocess.run([str(binary)], capture_output=True, text=True, env=child)


class Runner:
    """Runs the cases and collects the reasons a run did not demonstrate what it claims to."""

    def __init__(self, fixed, control, environment, verbose):
        self.fixed = fixed
        self.control = control
        self.environment = environment
        self.verbose = verbose
        self.failures = []
        self.destroyed_mutex_fails = False

    def show(self, label, proc):
        print(f"--- {label} (exit {proc.returncode})")
        sys.stdout.write(proc.stdout)
        if proc.stderr.strip():
            sys.stdout.write(proc.stderr)
        print()

    def fail(self, reason):
        self.failures.append(reason)

    # -------------------------------------------------------------------------- the cases

    def platform(self):
        proc = run_case(self.fixed, "platform", self.environment)
        self.show("platform: what this runtime does with a mutex in each state", proc)
        if proc.returncode != 0:
            self.fail("the platform case failed its own checks, so the states it measured are not "
                      "this platform's answers")
        self.destroyed_mutex_fails = answered_yes(
            proc.stdout, "a destroyed mutex refuses to lock")
        if not self.destroyed_mutex_fails:
            self.fail("this runtime grants the lock on a destroyed mutex, so the destroyed-section "
                      "cases below cannot fail and neither the fix nor the control means anything "
                      "here; record that rather than reading a pass")

    def premain(self):
        proc = run_case(self.fixed, "premain", self.environment)
        self.show("premain: what an allocation before main can reach", proc)
        if proc.returncode != 0:
            self.fail("the pre-main case failed its own checks")
        if not answered_yes(proc.stdout, "TheDmaCriticalSection was still null after it"):
            self.fail("a pre-main allocation saw a non-null TheDmaCriticalSection, which would "
                      "mean the crash reports could be pre-main after all -- the opposite of what "
                      "this case has measured so far, and worth the doc being changed for")

    def frames(self):
        proc = run_case(self.fixed, "frames", self.environment)
        self.show("frames: the state the preMainInitMemoryManager() call sees", proc)
        if proc.returncode != 0:
            self.fail("the frames case failed its own checks")
        if not answered_yes(proc.stdout, "an allocation after shutdownMemoryManager() is still "
                                         "served"):
            self.fail("an allocation after shutdownMemoryManager() was not served by the engine's "
                      "allocator, so this run cannot say what serves the late allocations the "
                      "crash reports are full of")

    def failing_case(self, case, label):
        """A case that must abort with one diagnostic, and whose control must recurse to death."""
        fixed = run_case(self.fixed, case, self.environment)
        self.show(f"{label}: as it ships", fixed)
        control = run_case(self.control, case, self.environment)
        self.show(f"{label}: negative control (the fix reverted)", control)

        lines = diagnostics(fixed.stderr)
        if fixed.returncode != SIGABRT:
            self.fail(f"{label}: the shipping binary exited {fixed.returncode}; the fix's failure "
                      f"path reports once and abort()s, which is {SIGABRT}. "
                      f"{SIGSEGV} or {SIGBUS} would mean it is still recursing")
        if len(lines) != 1:
            self.fail(f"{label}: the shipping binary printed {len(lines)} diagnostic lines and the "
                      "bound this fix is about is exactly one: a path that can report a lock "
                      "failure twice is a path that allocated to report it once")
        else:
            operation, name, number, section = lines[0]
            print(f"{label}: reported {operation} {name} ({number}) on {section}")
            if name == "some other errno" or name == "unknown errno":
                self.fail(f"{label}: the diagnostic did not name the errno ({number}), so the next "
                          "occurrence on hardware still would not say what failed")

        if control.returncode not in (SIGSEGV, SIGBUS):
            self.fail(f"{label}: the negative control exited {control.returncode}. The pre-fix "
                      f"path allocates to report the failure and must die of the recursion "
                      f"({SIGSEGV}); a control that survives is not a control")
        depth = control_depth(control.stderr)
        print(f"{label}: the control recursed at least {depth} deep")
        if depth < CONTROL_MINIMUM_DEPTH:
            self.fail(f"{label}: the negative control reported a recursion depth of {depth}, under "
                      f"{CONTROL_MINIMUM_DEPTH}: it died of something other than the recursion "
                      "this slice is about")

    def exit_immortal(self):
        proc = run_case(self.fixed, "exit-immortal", self.environment)
        self.show("exit-immortal: the same late free with the sections #113 made immortal", proc)
        if proc.returncode != 0:
            self.fail(f"the immortal arrangement exited {proc.returncode}: the lifetime fix is "
                      "what stops the failure from happening at all, and this is the case that "
                      "says it still does")
        if "late free completed" not in proc.stdout:
            self.fail("the late destructor's own output is missing, so the allocation after static "
                      "destruction did not complete")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--keep", action="store_true", help="keep the scratch link directory")
    parser.add_argument("--verbose", action="store_true", help="echo the compiler output")
    args = parser.parse_args()

    if sys.platform.startswith("win"):
        print("this measures the off-Windows failure path; a Win32 CRITICAL_SECTION does not "
              "report failures, so there is nothing here to reproduce")
        return 0

    runner = load_render_runner()
    scratch = pathlib.Path(tempfile.mkdtemp(prefix="lock-failure-test-"))
    try:
        fixed_object = scratch / "lock_failure_test.o"
        control_object = scratch / "lock_failure_test_control.o"
        for out, extra in ((fixed_object, ()),
                           (control_object, ("-DLOCK_FAILURE_NEGATIVE_CONTROL",))):
            output = runner.compile_harness(out, harness=HARNESS, donor=FLAG_DONOR,
                                            extra_arguments=extra)
            if args.verbose and output.strip():
                print(output)

        # Two copies of the archives: the control's has the fix removed from it, and the shipping
        # one must not be touched by that.
        for directory in ("fixed", "control"):
            (scratch / directory).mkdir()
        fixed_archives = runner.scratch_archives(scratch / "fixed")
        control_archives = runner.scratch_archives(scratch / "control")
        stripped = strip_reporter(control_archives)
        print(f"the negative control's {stripped.name} has {REPORTER_MEMBER} removed; its own "
              "definition of the failure path is linked instead\n")

        fixed = scratch / "lock_failure_test"
        control = scratch / "lock_failure_test_control"
        for objects, binary, archives in ((fixed_object, fixed, fixed_archives),
                                          (control_object, control, control_archives)):
            output = runner.link_harness([objects], binary, archives)
            if args.verbose and output.strip():
                print(output)

        environment, _ = runner.run_environment()
        cases = Runner(fixed, control, environment, args.verbose)
        cases.platform()
        cases.premain()
        cases.frames()
        cases.failing_case("destroyed-dma", "destroyed section, mid-run")
        cases.failing_case("exit-destroyed", "destroyed section, through static destruction order")
        cases.exit_immortal()

        if cases.failures:
            print()
            for failure in cases.failures:
                print(f"FAILED: {failure}")
            return 1

        print("\nOK: a lock failure in the allocator's critical section reports once, names the "
              "errno and the section, and abort()s; the same failure with the pre-fix path linked "
              "recurses until the stack is gone; and with the immortal sections it does not happen "
              "at all")
        return 0
    finally:
        if args.keep:
            print(f"scratch directory kept at {scratch}")
        else:
            shutil.rmtree(scratch, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Build and run the negative control for ChallengeLoadScreen's null video stream.

Two halves, both about a Generals Challenge screen whose movie the video player cannot open:

  * the C++ half compiles Core/GameEngine/Source/GameClient/tests/loadscreen_video_null_test.cpp,
    which carries both shapes of ChallengeLoadScreen::init()'s video block, and runs three
    processes: the shape it has now over a null stream (must survive, allocate nothing, leak
    nothing and still initialise the rest of the screen), the shape it had over the same null
    stream (must die on a signal -- the control is the crash, not a description of it), and both
    shapes over a stream that opens (must be indistinguishable, because Windows is the oracle for
    the working path).

  * the scan half reads the real Core/GameEngine/Source/GameClient/GUI/LoadScreen.cpp and asserts
    that in ChallengeLoadScreen::init() every use of m_videoStream is preceded by a null test, and
    that the pre-fix body -- kept here verbatim from 30a7a3434 -- is what the same classifier calls
    unguarded. Without that second half a passing gate would prove nothing: it could be a
    classifier that calls everything guarded.

Usage:
    python3 scripts/native-loadscreen-video-test.py [--keep] [--verbose]

Exits non-zero if the test does not compile, an assertion fails, the pre-fix shape does *not*
crash, or the classifier cannot tell the two shapes apart. CLANGXX selects the compiler.
"""

import argparse
import os
import pathlib
import re
import signal
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
CLANGXX = os.environ.get("CLANGXX", "clang++")

TEST_SOURCE = "Core/GameEngine/Source/GameClient/tests/loadscreen_video_null_test.cpp"
LOAD_SCREEN = "Core/GameEngine/Source/GameClient/GUI/LoadScreen.cpp"

COMPILE_FLAGS = [
    "-std=c++20",
    "-m64",
    "-g",
    "-O0",
    # The control dereferences null on purpose, and at -O0 with these two the compiler neither
    # deletes the load nor assumes it cannot happen: the process faults, which is the measurement.
    "-fno-delete-null-pointer-checks",
    "-fno-strict-aliasing",
    "-Wall",
    "-Werror=return-type",
]

# The video block of ChallengeLoadScreen::init() as it was before this slice, for the classifier
# half. From 30a7a3434 Core/GameEngine/Source/GameClient/GUI/LoadScreen.cpp, verbatim apart from
# the tabs, which are spaces here so that flake8 reads the physical lines of this string.
PRE_FIX_BODY = """
    // create the new background video stream
    m_videoStream = TheVideoPlayer->open( TheCampaignManager->getCurrentMission()->m_movieLabel );

    // Create the new buffer
    m_videoBuffer = TheDisplay->createVideoBuffer();
    if (m_videoBuffer == nullptr || !m_videoBuffer->allocate( m_videoStream->width(),
        m_videoStream->height() ))
    {
        delete m_videoBuffer;
        m_videoBuffer = nullptr;

        if ( m_videoStream )
        {
            m_videoStream->close();
            m_videoStream = nullptr;
        }

        return;
    }
"""

# `m_videoStream` on the left of an assignment, or being compared, is not a use of the pointee.
USE_PATTERN = re.compile(r"m_videoStream\s*->")
GUARD_PATTERN = re.compile(r"if\s*\(\s*m_videoStream\s*(!=\s*nullptr|!=\s*NULL)?\s*[&)]")


def classify(body):
    """`guarded` if every m_videoStream-> use has a null test before it, else `unguarded`.

    Deliberately crude and line-ordered: it answers "does a null test come first", which is the
    whole question the crash turned on. A guard is any `if (m_videoStream ...)`; the tracking is
    not scope-aware, so it is generous to the code under test rather than to this test.
    """
    guarded = False
    for line in body.splitlines():
        stripped = line.strip()
        if GUARD_PATTERN.search(stripped):
            guarded = True
        if USE_PATTERN.search(stripped) and not guarded:
            return "unguarded", stripped
    return "guarded", None


def challenge_init_body(text):
    """The body of ChallengeLoadScreen::init(), by brace depth from its opening line."""
    start = text.find("void ChallengeLoadScreen::init(")
    if start < 0:
        sys.exit(f"{LOAD_SCREEN}: no ChallengeLoadScreen::init() found")
    open_brace = text.find("{", start)
    depth = 0
    for index in range(open_brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[open_brace:index + 1]
    sys.exit(f"{LOAD_SCREEN}: ChallengeLoadScreen::init() is not brace-balanced")


def run(command, verbose, **kwargs):
    if verbose:
        print("+", " ".join(str(part) for part in command))
    return subprocess.run(command, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--keep", action="store_true", help="keep the built binary")
    parser.add_argument("--verbose", action="store_true", help="echo the commands")
    args = parser.parse_args()

    failures = 0

    print("== the classifier over the real file and the pre-fix body")
    load_screen = (REPO_ROOT / LOAD_SCREEN).read_text(encoding="utf-8", errors="replace")
    verdict, offender = classify(challenge_init_body(load_screen))
    print(f"   ChallengeLoadScreen::init() as it stands: {verdict}")
    if verdict != "guarded":
        print(f"   FAILED: an unguarded use of the video stream: {offender}")
        failures += 1
    pre_verdict, pre_offender = classify(PRE_FIX_BODY)
    print(f"   the pre-fix body: {pre_verdict}"
          + (f" (first unguarded use: {pre_offender})" if pre_offender else ""))
    if pre_verdict != "unguarded":
        print("   FAILED: the classifier calls the pre-fix body guarded, so it cannot tell the "
              "two shapes apart and a pass above means nothing")
        failures += 1

    with tempfile.TemporaryDirectory() as scratch:
        binary = pathlib.Path(scratch) / "loadscreen_video_null_test"
        if args.keep:
            binary = REPO_ROOT / "build" / "native" / "loadscreen_video_null_test"
            binary.parent.mkdir(parents=True, exist_ok=True)

        print("\n== compiling the test")
        compile_command = [CLANGXX, *COMPILE_FLAGS, str(REPO_ROOT / TEST_SOURCE),
                           "-o", str(binary)]
        if run(compile_command, args.verbose).returncode != 0:
            sys.exit("the test does not compile")

        print("\n== the fixed shape over a null stream (must survive)")
        fixed = run([str(binary), "--shape=fixed"], args.verbose)
        if fixed.returncode != 0:
            print(f"   FAILED: exit {fixed.returncode}")
            failures += 1

        print("\n== both shapes over a stream that opens (must be indistinguishable)")
        working = run([str(binary), "--shape=working"], args.verbose)
        if working.returncode != 0:
            print(f"   FAILED: exit {working.returncode}")
            failures += 1

        print("\n== the negative control: the pre-fix shape over the same null stream")
        control = run([str(binary), "--shape=pre-fix"], args.verbose)
        if control.returncode < 0:
            name = signal.Signals(-control.returncode).name
            print(f"   ok: the pre-fix shape died on {name}, which is the crash this slice fixes")
        else:
            print(f"   FAILED: the pre-fix shape exited {control.returncode} instead of faulting, "
                  "so this control proves nothing about the fix")
            failures += 1

    print(f"\nfailures: {failures}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())

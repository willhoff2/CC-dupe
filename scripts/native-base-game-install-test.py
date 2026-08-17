#!/usr/bin/env python3
"""Build and run the negative control for the base-game install-path diagnostic.

Zero Hour mounts the base game's archives from the `InstallPath` setting (`init()` of both archive
file systems, under `RTS_ZEROHOUR`). When that mount does not happen a release build used to say
nothing: the only diagnostic was a debug-build assertion on the empty setting, and what the player
got instead was a placeholder backdrop, no sky, and a crash in a sky-drawing map. Two halves, both
about that silence:

  * the C++ half compiles Core/GameEngine/Source/Common/System/tests/
    base_game_install_report_test.cpp against the real Common/BaseGameInstallReport.h, asserts that
    the three failures (nothing configured, unreadable path, readable path with no archives)
    classify apart and produce three different messages that name the path and the setting, and
    asserts that the pre-fix shape -- kept in the test verbatim -- reports nothing at all. Without
    that control a passing gate would only say the new code prints something.

  * the source half asserts the engine actually calls the seam, in *both* archive file systems:
    Win32BIGFileSystem is the one the ported engine instantiates (Win32GameEngine::
    createArchiveFileSystem) and StdBIGFileSystem is its C++17 twin, so a report added to only one
    of them is a report the running game never makes. Neither may put it inside a debug-only
    conditional, which is the whole point of the change.

Usage:
    python3 scripts/native-base-game-install-test.py [--keep] [--verbose]

Exits non-zero if the seam does not compile, the assertions fail, or the engine does not report
through it. CLANGXX selects the compiler.
"""

import argparse
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import os

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
CLANGXX = os.environ.get("CLANGXX", "clang++")

TEST_SOURCE = ("Core/GameEngine/Source/Common/System/tests/"
               "base_game_install_report_test.cpp")

# Both archive file systems, keyed by the class whose init() must report. Win32BIGFileSystem is the
# one the ported engine actually creates; StdBIGFileSystem is the non-VS6 twin of the same code.
ENGINE_SOURCES = {
    "Win32BIGFileSystem":
        "Core/GameEngineDevice/Source/Win32Device/Common/Win32BIGFileSystem.cpp",
    "StdBIGFileSystem":
        "Core/GameEngineDevice/Source/StdDevice/Common/StdBIGFileSystem.cpp",
}

INCLUDES = [
    "Core/GameEngine/Include",
]

COMPILE_FLAGS = [
    "-std=c++20",
    "-m64",
    "-g",
    "-O0",
    "-Wall",
    "-Werror=return-type",
]

# Anything that would compile the report out of a release build. `DEBUG_LOG` is allowed alongside
# the release-visible write, not instead of it, so it is not on this list; a conditional around the
# whole reporter is.
DEBUG_ONLY_GUARDS = ("RTS_DEBUG", "DEBUG_CRASHING", "DEBUG_LOGGING", "ALLOW_DEBUG_UTILS",
                     "RTS_INTERNAL")


def strip_comments(text):
    """Line and block comments removed, so a commented-out call does not read as a call."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def extract_function_body(text, signature_pattern):
    """The braced body of the first function whose declaration matches, or None."""
    match = re.search(signature_pattern, text)
    if match is None:
        return None
    start = text.find("{", match.end() - 1)
    if start < 0:
        return None
    depth = 0
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start:index + 1]
    return None


def check_engine_reports():
    """Both archive file systems report through the seam, in every configuration."""
    for class_name, source in sorted(ENGINE_SOURCES.items()):
        text = strip_comments((REPO_ROOT / source).read_text())

        init_body = extract_function_body(
            text, r"void\s+%s::init\s*\(\s*\)" % class_name)
        if init_body is None:
            print("FAILED: %s::init() was not found in %s" % (class_name, source))
            return 1
        if "reportBaseGameInstall(" not in init_body:
            print("FAILED: %s::init() does not report the base-game install path; a missing "
                  "InstallPath would be silent again" % class_name)
            return 1

        if "baseGameInstallNeedsSeparator(" not in init_body:
            print("FAILED: %s::init() mounts the install path without the trailing separator the "
                  "search mask is concatenated onto; a real depot would report no archives"
                  % class_name)
            return 1

        report_body = extract_function_body(
            text, r"static\s+void\s+reportBaseGameInstall\s*\([^)]*\)")
        if report_body is None:
            print("FAILED: reportBaseGameInstall() was not found in %s" % source)
            return 1
        if "fprintf(stderr" not in report_body:
            print("FAILED: %s's reportBaseGameInstall() writes nothing a release build can see"
                  % class_name)
            return 1
        for guard in DEBUG_ONLY_GUARDS:
            if guard in report_body:
                print("FAILED: %s's reportBaseGameInstall() is guarded by %s, so a release build "
                      "is silent again" % (class_name, guard))
                return 1

        print("ok: %s::init() reports the install path through the seam, outside any debug-only "
              "conditional" % class_name)
    return 0


def run(command, verbose, **kwargs):
    if verbose:
        print("+", " ".join(str(part) for part in command))
    return subprocess.run(command, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--keep", action="store_true",
                        help="keep the build directory instead of removing it")
    parser.add_argument("--verbose", action="store_true", help="echo the commands")
    args = parser.parse_args()

    build_dir = pathlib.Path(tempfile.mkdtemp(prefix="base-game-install-test-"))
    try:
        binary = build_dir / "base_game_install_report_test"
        command = [CLANGXX] + COMPILE_FLAGS
        for include in INCLUDES:
            command += ["-I", str(REPO_ROOT / include)]
        command += [str(REPO_ROOT / TEST_SOURCE), "-o", str(binary)]

        if run(command, args.verbose, cwd=REPO_ROOT).returncode != 0:
            print("FAILED: the install-path diagnostic did not compile")
            return 1

        if run([str(binary)], args.verbose).returncode != 0:
            print("FAILED: the three install-path failures are not reported apart")
            return 1

        if check_engine_reports() != 0:
            return 1

        print("\nOK: an unmountable base game reports which of the three repairs it needs, and the "
              "pre-fix shape provably reported nothing")
        return 0
    finally:
        if args.keep:
            print("kept %s" % build_dir)
        else:
            shutil.rmtree(build_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Build and run the negative control for init-failure reporting.

Two halves, both about a subsystem that cannot initialise:

  * the C++ half compiles Core/GameEngine/Source/Common/System/tests/init_failure_test.cpp against
    the real Common/InitFailure.h and the real Common/INIException.h, forces a fake subsystem's
    init() to fail, and asserts the failure reaches the caller with the subsystem's name and the
    file that could not be read in the message. The same driver is run over the shape
    GameTextManager::init() had before this slice -- detect the failure, return -- and asserts that
    that shape reports nothing, which is the control the fix is measured against.

  * the scan half asserts the same two shapes are what scripts/init-reporting-scan.py's classifier
    says they are: the body of GameTextManager::init() as it stands classifies as `throws`, and the
    pre-fix body, kept here verbatim, classifies as `silent-return`. Without this, a gate that
    passes proves nothing -- it could be a classifier that says `throws` about everything.

Usage:
    python3 scripts/native-init-failure-test.py [--keep] [--verbose]

Exits non-zero if the seam does not compile, the assertions fail, or the classifier does not tell
the two shapes apart. CLANGXX selects the compiler.
"""

import argparse
import importlib.util
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
CLANGXX = os.environ.get("CLANGXX", "clang++")

TEST_SOURCE = "Core/GameEngine/Source/Common/System/tests/init_failure_test.cpp"

# InitFailure.h is in Core; INIException.h is per-game, and Zero Hour's is the one the ported
# engine compiles against. Dependencies/Utility supplies snprintf on VC6 and is harmless here.
INCLUDES = [
    "Core/GameEngine/Include",
    "GeneralsMD/Code/GameEngine/Include",
    "Dependencies/Utility",
]

COMPILE_FLAGS = [
    "-std=c++20",
    "-m64",
    "-g",
    "-O0",
    "-Wall",
    "-Werror=return-type",
]

# GameTextManager::init() as it was before this slice, for the classifier half. From
# e14893c32 Core/GameEngine/Source/GameClient/GameText.cpp, trimmed to the failure paths, with the
# tabs turned into spaces because flake8 reads the physical lines of this string.
PRE_FIX_BODY = """
    if ( m_initialized )
    {
        return;
    }
    if ( m_useStringFile && getStringCount( g_strFile, m_textCount ) )
    {
        format = STRING_FILE;
    }
    else if ( getCSFInfo ( csfFile.str() ) )
    {
        format = CSF_FILE;
    }
    else
    {
        return;
    }
    if( m_textCount == 0 )
    {
        return;
    }
    m_stringInfo = NEW StringInfo[m_textCount];
    if( m_stringInfo == nullptr )
    {
        deinit();
        return;
    }
    if ( format == STRING_FILE )
    {
        if( parseStringFile( g_strFile ) == FALSE )
        {
            deinit();
            return;
        }
    }
    else
    {
        if ( !parseCSF ( csfFile.str() ) )
        {
            deinit();
            return;
        }
    }
"""


def load_scanner():
    path = REPO_ROOT / "scripts" / "init-reporting-scan.py"
    spec = importlib.util.spec_from_file_location("init_reporting_scan", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def check_classifier(verbose):
    """The classifier tells the pre-fix shape and the current one apart."""
    scanner = load_scanner()

    classification, evidence = scanner.classify(PRE_FIX_BODY)
    if verbose:
        print("pre-fix body:", classification, evidence)
    if classification != "silent-return":
        print("FAILED: the classifier calls the pre-fix GameTextManager::init() %s, not "
              "silent-return; the scan gate cannot be trusted" % classification)
        return 1

    sources = scanner.Sources()
    found = sources.find_init_chain("GameTextManager")
    if found is None:
        print("FAILED: GameTextManager::init() was not found")
        return 1
    classification, evidence = scanner.classify(found["body"])
    if verbose:
        print("current body:", classification, evidence)
    if classification != "throws":
        print("FAILED: GameTextManager::init() classifies as %s; it must be able to report a "
              "failure (%s)" % (classification, found["file"]))
        return 1

    print("ok: the classifier reads the pre-fix string manager as silent-return and the current "
          "one as throws")
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

    build_dir = pathlib.Path(tempfile.mkdtemp(prefix="init-failure-test-"))
    try:
        binary = build_dir / "init_failure_test"
        command = [CLANGXX] + COMPILE_FLAGS
        for include in INCLUDES:
            command += ["-I", str(REPO_ROOT / include)]
        command += [str(REPO_ROOT / TEST_SOURCE), "-o", str(binary)]

        if run(command, args.verbose, cwd=REPO_ROOT).returncode != 0:
            print("FAILED: the init-failure seam did not compile")
            return 1

        if run([str(binary)], args.verbose).returncode != 0:
            print("FAILED: a forced init failure is not diagnosable")
            return 1

        if check_classifier(args.verbose) != 0:
            return 1

        print("\nOK: a forced subsystem init failure reports what failed and where, and the "
              "pre-fix shape provably did not")
        return 0
    finally:
        if args.keep:
            print(f"build directory kept at {build_dir}")
        else:
            shutil.rmtree(build_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

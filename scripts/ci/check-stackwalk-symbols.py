#!/usr/bin/env python3
"""Gate the native debug/profile libraries: they compile, and their symbols are really defined.

Two things this locks down, both of which regressed silently before:

1. Every translation unit CMake compiles into core_debug and core_profile_legacy compiles
   natively, and the source lists here do not drift from CMakeLists.txt.
2. The stack walk entry points (DebugStackwalk::StackWalk, Signature::GetSymbol, ...) are
   *defined* in the objects, and nothing in them still refers to DbgHelp, SEH or the other Win32
   symbols this seam replaced. A header-only stub satisfies the compiler and then fails the link,
   which is exactly the state this library was in.

scripts/native-stackwalk-test.py goes further and runs a symbolised backtrace; this script is the
cheap check that also covers the profile library. See docs/porting/debug-and-profile-libs.md.

Usage:
    python3 scripts/ci/check-stackwalk-symbols.py [--verbose]
"""

import argparse
import os
import pathlib
import re
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
CLANGXX = os.environ.get("CLANGXX", "clang++")
NM = os.environ.get("NM", "nm")

DEBUG_DIR = REPO_ROOT / "Core/Libraries/Source/debug"
PROFILE_DIR = REPO_ROOT / "Core/Libraries/Source/profile"

INCLUDES = [
    "Dependencies/Utility",
    "Core/Libraries/Include",
    "resources/gitinfo",
    "Core/Libraries/Source/WWVegas",
    "Core/Libraries/Source/WWVegas/WWLib",
    "Core/Libraries/Source/debug",
    "Core/Libraries/Source/profile",
]

COMPILE_FLAGS = [
    "-std=c++20",
    "-m64",
    "-g",
    "-fms-extensions",
    "-include", "Utility/CppMacros.h",
    "-DWIN32_LEAN_AND_MEAN",
    "-D_REENTRANT",
]

# Demangled names that must be defined in the objects for a native link to succeed.
REQUIRED_SYMBOLS = [
    "DebugStackwalk::StackWalk(DebugStackwalk::Signature&, _CONTEXT*)",
    "DebugStackwalk::Signature::GetAddress(int) const",
    "DebugStackwalk::Signature::GetSymbol(unsigned long long, char*, unsigned int)",
    "DebugStackwalk::Signature::Signature(DebugStackwalk::Signature const&)",
    "DebugStackwalk::GetDbghelpHandle()",
    "DebugStackwalk::IsOldDbghelp()",
    "DebugPlatform::CaptureStack(void**, unsigned int, unsigned int)",
    "DebugPlatform::ResolveAddress(unsigned long long, char*, unsigned int, unsigned long long*,"
    " char*, unsigned int, unsigned long long*)",
]

# Win32-only imports that must not survive natively; a leftover here is an unresolved symbol at
# link time, or worse, a shim that silently does nothing.
FORBIDDEN_UNDEFINED = re.compile(
    r"^_?(Sym[A-Z]\w*|StackWalk64|MiniDumpWriteDump|ImagehlpApiVersion"
    r"|GlobalAlloc|GlobalReAlloc|GlobalFree|GlobalSize"
    r"|OutputDebugStringA?|MessageBoxA?|IsBadReadPtr|IsBadCodePtr"
    r"|CreateFileA?|WriteFile|ReadFile|CloseHandle|CopyFileA?"
    r"|CreateNamedPipeA?|ConnectNamedPipe|AllocConsole|WriteConsoleA?"
    r"|SetUnhandledExceptionFilter|_set_se_translator"
    r"|QueryPerformanceCounter|QueryPerformanceFrequency|timeGetTime"
    r"|EnumProcessModules|CreateToolhelp32Snapshot)$"
)


def cmake_sources(cmake_file, variable):
    """The .cpp files a CMakeLists.txt lists in the given set()/list(APPEND) variable."""
    text = (cmake_file).read_text()
    sources = []
    for block in re.findall(r"(?:set|list\(APPEND)\s*\(?\s*" + variable + r"\b(.*?)\)",
                            text, re.S):
        sources += [s for s in re.findall(r'"?([\w./]+\.cpp)"?', block)]
    return sources


def compile_all(sources, work_dir, verbose):
    includes = [f"-I{REPO_ROOT / inc}" for inc in INCLUDES]
    objects = []
    failed = []
    for source in sources:
        obj = work_dir / (str(source.relative_to(REPO_ROOT)).replace("/", "_") + ".o")
        cmd = [CLANGXX, *COMPILE_FLAGS, *includes, "-c", str(source), "-o", str(obj)]
        if verbose:
            print("+", " ".join(cmd))
        proc = subprocess.run(cmd, cwd=REPO_ROOT, capture_output=True, text=True)
        if proc.returncode != 0:
            failed.append(source.relative_to(REPO_ROOT))
            sys.stderr.write(proc.stderr)
        else:
            objects.append(obj)
    return objects, failed


def symbols(objects):
    """(defined demangled names, undefined mangled names) across the objects."""
    proc = subprocess.run([NM, "-C", "--defined-only", *[str(o) for o in objects]],
                          capture_output=True, text=True, check=True)
    defined = {line.split(" ", 2)[2].strip()
               for line in proc.stdout.splitlines() if len(line.split(" ", 2)) == 3}

    proc = subprocess.run([NM, "-u", *[str(o) for o in objects]],
                          capture_output=True, text=True, check=True)
    undefined = {line.split()[-1] for line in proc.stdout.splitlines()
                 if line.strip().startswith("U ") or " U " in line}
    return defined, undefined


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--verbose", action="store_true", help="echo every command")
    args = parser.parse_args()

    failures = []

    debug_sources = cmake_sources(DEBUG_DIR / "CMakeLists.txt", "DEBUG_SRC")
    profile_sources = cmake_sources(PROFILE_DIR / "CMakeLists.txt", "PROFILE_SRC")
    if "platform/debug_platform.cpp" not in debug_sources:
        failures.append("core_debug does not build platform/debug_platform.cpp off Windows")

    # Kept in step with scripts/native-stackwalk-test.py, which links the same set plus the test.
    runner = (REPO_ROOT / "scripts/native-stackwalk-test.py").read_text()
    for source in debug_sources:
        if source not in runner:
            failures.append(f"{source} is in core_debug but not in native-stackwalk-test.py")

    sources = [DEBUG_DIR / s for s in debug_sources] + [PROFILE_DIR / s for s in profile_sources]
    work_dir = pathlib.Path(tempfile.mkdtemp(prefix="stackwalk-symbols-"))
    try:
        objects, failed = compile_all(sources, work_dir, args.verbose)
        for source in failed:
            failures.append(f"{source} does not compile natively")

        if objects:
            defined, undefined = symbols(objects)
            for required in REQUIRED_SYMBOLS:
                if required not in defined:
                    failures.append(f"{required} is declared but not defined natively")
            for name in sorted(undefined):
                if FORBIDDEN_UNDEFINED.match(name):
                    failures.append(f"{name} is still referenced natively, so the link fails")
    finally:
        subprocess.run(["rm", "-rf", str(work_dir)], check=False)

    print(f"checked {len(sources)} translation units "
          f"({len(debug_sources)} core_debug, {len(profile_sources)} core_profile_legacy)")
    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        return 1
    print("OK: both libraries compile natively and the stack walk symbols are defined")
    return 0


if __name__ == "__main__":
    sys.exit(main())

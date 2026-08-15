#!/usr/bin/env python3
"""Check the embedded-browser seam: ATL/COM stays on Windows, consumers stay untouched.

The embedded Internet Explorer control (Core/GameEngine/.../WOLBrowser, EABrowserDispatch,
GeneralsMD/.../W3DWebBrowser) is cut scope for the native port, and it used to be the last thing
stopping GeneralsMD/Code/Main from producing an object file. It is now compiled only where
RTS_HAS_EMBEDDED_BROWSER is defined, which is the Windows BrowserDispatch target and nowhere else.
Three things would quietly undo that, none of them visible in a Windows build:

  * an ATL/COM spelling (CComObject, CComModule, IDispatch, atlbase.h, IID_IBrowserDispatch)
    appears in a seam file outside an RTS_HAS_EMBEDDED_BROWSER branch, so the native build stops
    compiling again;
  * RTS_HAS_EMBEDDED_BROWSER gets defined somewhere other than the Windows-only CMake branch --
    a bare `#define` in a header would switch ATL back on for everybody;
  * the excision leaks into the online menus, which the seam exists to avoid: consumers keep the
    Win32 spelling (TheWebBrowser, GameEngine::createWebBrowser) and carry no browser #ifdef.

With --results <native-build.py --json>, the structural check is joined by the numeric one that
motivated the seam: no compile failure mentions ATL/COM any more, and GeneralsMD/Code/Main -- the
game's entry point -- produces an object and an archive.

Usage:
    python3 scripts/ci/check-embedded-browser.py
    python3 scripts/ci/check-embedded-browser.py --results native-build.json
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

FEATURE = "RTS_HAS_EMBEDDED_BROWSER"

# The seam itself: the files that are allowed to name ATL/COM at all, and only inside the feature's
# own branch. Everything else in the engine must not name it, checked by NO_ATL_ANYWHERE below.
SEAM_FILES = [
    os.path.join("Core", "GameEngine", "Include", "GameNetwork", "WOLBrowser", "WebBrowser.h"),
    os.path.join("Core", "GameEngine", "Source", "GameNetwork", "WOLBrowser", "WebBrowser.cpp"),
    os.path.join("GeneralsMD", "Code", "GameEngine", "Source", "Common", "GameEngine.cpp"),
    os.path.join("GeneralsMD", "Code", "GameEngineDevice", "Include", "W3DDevice", "GameClient",
                 "W3DWebBrowser.h"),
    os.path.join("GeneralsMD", "Code", "GameEngineDevice", "Source", "W3DDevice", "GameClient",
                 "W3DWebBrowser.cpp"),
    os.path.join("GeneralsMD", "Code", "GameEngineDevice", "Include", "Win32Device", "Common",
                 "Win32GameEngine.h"),
]

# Consumers of the browser. They must still be written against the Win32 spelling, and must not
# have grown a browser #ifdef -- that would mean the seam was avoided rather than written.
CONSUMERS = {
    os.path.join("Core", "GameEngine", "Source", "Common", "INI",
                 "INIWebpageURL.cpp"): ["TheWebBrowser"],
    os.path.join("GeneralsMD", "Code", "GameEngine", "Source", "GameClient", "GUI", "GUICallbacks",
                 "Menus", "WOLLoginMenu.cpp"): ["TheWebBrowser"],
    os.path.join("GeneralsMD", "Code", "GameEngine", "Source", "GameClient", "GUI", "GUICallbacks",
                 "Menus", "WOLLadderScreen.cpp"): ["TheWebBrowser"],
    os.path.join("GeneralsMD", "Code", "GameEngine", "Include", "Common",
                 "GameEngine.h"): ["createWebBrowser"],
}

# Where the feature may be defined: the Windows-only branch of the BrowserDispatch library.
FEATURE_DEFINITION_FILE = os.path.join("Core", "Libraries", "Source", "EABrowserDispatch",
                                       "CMakeLists.txt")

ATL_SPELLINGS = re.compile(
    r"\b(?:CComObject|CComModule|CComCoClass|IDispatch|IUnknown|IBrowserDispatch|"
    r"IID_IBrowserDispatch|FEBDispatch|STDMETHODIMP|OLEInitializer)\b|"
    r"[<\"](?:atlbase\.h|atlcom\.h|EABrowserDispatch/BrowserDispatch\.h|FEBDispatch\.h)[>\"]")

ATL_FAILURE = re.compile(r"CComObject|CComModule|IDispatch|IBrowserDispatch|atlbase")

GAME_ENTRY_LIBRARY = "GeneralsMD/Code/Main"


def read(path):
    with open(path, "r", errors="replace") as handle:
        return handle.read()


def lines_outside_feature(text):
    """Lines that are compiled when RTS_HAS_EMBEDDED_BROWSER is *not* defined.

    Only conditions on the feature are tracked; every other #if is transparent, because a line
    inside `#ifdef _WIN32` is still a line the native build would have to swallow if the feature
    were absent.
    """
    out = []
    # Stack of booleans: True while the lines need the feature.
    stack = []
    for number, line in enumerate(text.splitlines(), 1):
        directive = re.match(r"#\s*(ifdef|ifndef|if|else|elif|endif)\b(.*)", line.strip())
        if directive:
            kind, rest = directive.group(1), directive.group(2).strip()
            positive = FEATURE in rest and "!" not in rest.replace("!=", "")
            if kind == "ifdef":
                stack.append(rest == FEATURE)
            elif kind == "ifndef":
                stack.append(False)
            elif kind == "if":
                stack.append(positive and "defined" in rest)
            elif kind == "elif":
                if stack:
                    stack[-1] = positive and "defined" in rest
            elif kind == "else":
                if stack:
                    stack[-1] = not stack[-1]
            elif kind == "endif":
                if stack:
                    stack.pop()
            continue
        if not any(stack):
            out.append((number, line))
    return out


def is_comment(line):
    return re.match(r"(//|\*|/\*)", line.lstrip()) is not None


def check_sources(failures):
    for relative in SEAM_FILES:
        path = os.path.join(ROOT, relative)
        if not os.path.isfile(path):
            failures.append("%s: missing" % relative)
            continue
        text = read(path)
        if FEATURE not in text:
            failures.append("%s: names no %s branch, so its ATL half is unconditional"
                            % (relative, FEATURE))
        for number, line in lines_outside_feature(text):
            if is_comment(line):
                continue
            found = ATL_SPELLINGS.search(line)
            if found:
                failures.append("%s:%d: %s outside an #ifdef %s: %s"
                                % (relative, number, found.group(0).strip(), FEATURE,
                                   line.strip()))
    print("seam files: %d checked for ATL/COM outside the feature branch" % len(SEAM_FILES))

    for relative, spellings in sorted(CONSUMERS.items()):
        path = os.path.join(ROOT, relative)
        if not os.path.isfile(path):
            failures.append("%s: missing" % relative)
            continue
        text = read(path)
        for spelling in spellings:
            if spelling not in text:
                failures.append("%s: no longer uses %s; the seam exists so consumers keep the "
                                "Win32 spelling" % (relative, spelling))
        if FEATURE in text:
            failures.append("%s: mentions %s; the excision must not leak into consumers"
                            % (relative, FEATURE))
    print("consumers: %d checked for the unchanged Win32 spelling" % len(CONSUMERS))


def check_feature_definition(failures):
    """The feature may only be turned on by the Windows BrowserDispatch target."""
    definers = []
    for directory, _, names in os.walk(ROOT):
        if any(part in directory.split(os.sep) for part in (".git", "build", "docs", "scripts")):
            continue
        for name in names:
            if not name.endswith((".h", ".cpp", ".hpp", ".inl", "CMakeLists.txt", ".cmake")):
                continue
            relative = os.path.relpath(os.path.join(directory, name), ROOT)
            text = read(os.path.join(directory, name))
            if re.search(r"^\s*#\s*define\s+%s\b" % FEATURE, text, re.M) or \
                    re.search(r"(?:add_compile_definitions|target_compile_definitions)"
                              r"[^)]*%s" % FEATURE, text):
                definers.append(relative)
    if definers != [FEATURE_DEFINITION_FILE]:
        failures.append("%s must be defined by %s and nothing else; found: %s"
                        % (FEATURE, FEATURE_DEFINITION_FILE, ", ".join(definers) or "nothing"))
    else:
        # ...and only inside that file's Windows branch.
        text = read(os.path.join(ROOT, FEATURE_DEFINITION_FILE))
        if "if(WIN32" not in text:
            failures.append("%s defines %s outside a Windows branch"
                            % (FEATURE_DEFINITION_FILE, FEATURE))
    print("feature definition: %s defined by %s" % (FEATURE, ", ".join(definers) or "nothing"))


def check_results(path, failures):
    results = json.loads(read(path))
    atl_failures = sorted(unit for unit, diagnostic in results["compile_failures"].items()
                          if ATL_FAILURE.search(diagnostic))
    if atl_failures:
        failures.append("still failing to compile on ATL/COM: %s" % ", ".join(atl_failures))
    print("compile failures mentioning ATL/COM: %d" % len(atl_failures))

    entry = results["compiled"].get(GAME_ENTRY_LIBRARY)
    if entry is None:
        failures.append("%s is not in the measured build at all" % GAME_ENTRY_LIBRARY)
    elif entry["objects"] < 1:
        failures.append("%s produced %d/%d objects; the game's entry point must compile"
                        % (GAME_ENTRY_LIBRARY, entry["objects"], entry["total"]))
    else:
        print("%s: %d/%d objects" % (GAME_ENTRY_LIBRARY, entry["objects"], entry["total"]))
    if GAME_ENTRY_LIBRARY in results.get("libraries_without_archive", []):
        failures.append("%s produced no archive" % GAME_ENTRY_LIBRARY)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--results", help="JSON written by native-build.py --json")
    args = parser.parse_args()

    failures = []
    check_sources(failures)
    check_feature_definition(failures)
    if args.results:
        check_results(args.results, failures)

    if failures:
        print("", file=sys.stderr)
        for failure in failures:
            print("FAIL: %s" % failure, file=sys.stderr)
        return 1

    print("OK: the embedded browser is Windows-only and its consumers are unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main())

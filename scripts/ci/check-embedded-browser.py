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

The same three things apply one layer down, in WW3D2's DX8WebBrowser -- the D3D8 host the control is
drawn through. Its `ENABLE_EMBEDDED_BROWSER` is a *derived* switch: dx8webbrowser.h defines it from
RTS_HAS_EMBEDDED_BROWSER, so `#if ENABLE_EMBEDDED_BROWSER` is the feature under another name and is
treated as such here. A bare `#define ENABLE_EMBEDDED_BROWSER 1` -- how that header was written
before -- would switch <windows.h>, <d3d8.h> and LPDISPATCH back on for the whole renderer, which is
why the derivation itself is checked.

With --results <native-build.py --json>, the structural check is joined by the numeric one that
motivated the seam: no compile failure mentions ATL/COM or LPDISPATCH any more, GeneralsMD/Code/Main
-- the game's entry point -- produces an object and an archive, and the translation units the two
waves of this seam unblocked still compile.

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
    # The DX8 layer: the control's D3D8 host, and the only place LPDISPATCH/IDispatch may be named.
    os.path.join("Core", "Libraries", "Source", "WWVegas", "WW3D2", "dx8webbrowser.h"),
    os.path.join("Core", "Libraries", "Source", "WWVegas", "WW3D2", "dx8webbrowser.cpp"),
]

# `#if ENABLE_EMBEDDED_BROWSER` means the same as `#ifdef RTS_HAS_EMBEDDED_BROWSER`, because
# dx8webbrowser.h derives the one from the other. DERIVED_DEFINITION_FILE is where that derivation
# has to live, and DERIVED_FROM_FEATURE is the shape it has to have.
DERIVED_FEATURE = "ENABLE_EMBEDDED_BROWSER"

DERIVED_DEFINITION_FILE = os.path.join("Core", "Libraries", "Source", "WWVegas", "WW3D2",
                                       "dx8webbrowser.h")

DERIVED_FROM_FEATURE = re.compile(
    r"#\s*ifdef\s+%s\s*\n\s*#\s*define\s+%s\s+1\s*\n\s*#\s*else\s*\n\s*#\s*define\s+%s\s+0"
    % (FEATURE, DERIVED_FEATURE, DERIVED_FEATURE))

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
    # The DX8 layer's consumers. dx8wrapper.cpp drives the browser from Begin_Scene()/End_Scene()
    # and W3DDisplay owns its lifetime; both are why the entry points survive the excision.
    os.path.join("Core", "Libraries", "Source", "WWVegas", "WW3D2",
                 "dx8wrapper.cpp"): ["DX8WebBrowser::Update", "DX8WebBrowser::Render"],
    os.path.join("GeneralsMD", "Code", "GameEngineDevice", "Source", "W3DDevice", "GameClient",
                 "W3DDisplay.cpp"): ["DX8WebBrowser::Initialize", "DX8WebBrowser::Shutdown"],
}

# Translation units the two waves of this seam unblocked. A regression here is invisible in a
# Windows build and would not show up as an ATL diagnostic if the cause were, say, a re-added
# unconditional <windows.h>.
REQUIRED_UNITS = [
    os.path.join("Core", "Libraries", "Source", "WWVegas", "WW3D2", "dx8webbrowser.cpp"),
    os.path.join("GeneralsMD", "Code", "GameEngineDevice", "Source", "W3DDevice", "GameClient",
                 "W3DDisplay.cpp"),
]

# Where the feature may be defined: the Windows-only branch of the BrowserDispatch library.
FEATURE_DEFINITION_FILE = os.path.join("Core", "Libraries", "Source", "EABrowserDispatch",
                                       "CMakeLists.txt")

ATL_SPELLINGS = re.compile(
    r"\b(?:CComObject|CComModule|CComCoClass|IDispatch|IUnknown|IBrowserDispatch|"
    r"IID_IBrowserDispatch|FEBDispatch|STDMETHODIMP|OLEInitializer)\b|"
    r"[<\"](?:atlbase\.h|atlcom\.h|EABrowserDispatch/BrowserDispatch\.h|FEBDispatch\.h)[>\"]")

ATL_FAILURE = re.compile(r"CComObject|CComModule|IDispatch|LPDISPATCH|IBrowserDispatch|atlbase")

GAME_ENTRY_LIBRARY = "GeneralsMD/Code/Main"


def read(path):
    with open(path, "r", errors="replace") as handle:
        return handle.read()


def lines_outside_feature(text):
    """Lines that are compiled when RTS_HAS_EMBEDDED_BROWSER is *not* defined.

    Only conditions on the feature are tracked; every other #if is transparent, because a line
    inside `#ifdef _WIN32` is still a line the native build would have to swallow if the feature
    were absent. ENABLE_EMBEDDED_BROWSER counts as the feature: check_derived_definition() proves
    it is derived from it.
    """
    out = []
    # Stack of booleans: True while the lines need the feature.
    stack = []
    for number, line in enumerate(text.splitlines(), 1):
        directive = re.match(r"#\s*(ifdef|ifndef|if|else|elif|endif)\b(.*)", line.strip())
        if directive:
            kind, rest = directive.group(1), directive.group(2).strip()
            named = FEATURE in rest or DERIVED_FEATURE in rest
            positive = named and "!" not in rest.replace("!=", "")
            if kind == "ifdef":
                stack.append(rest in (FEATURE, DERIVED_FEATURE))
            elif kind == "ifndef":
                stack.append(False)
            elif kind == "if":
                # `#if ENABLE_EMBEDDED_BROWSER` needs no `defined`: it is a value, not a guard.
                stack.append(positive and ("defined" in rest or rest == DERIVED_FEATURE))
            elif kind == "elif":
                if stack:
                    stack[-1] = positive and ("defined" in rest or rest == DERIVED_FEATURE)
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
        if FEATURE not in text and DERIVED_FEATURE not in text:
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


def check_derived_definition(failures):
    """ENABLE_EMBEDDED_BROWSER must be derived from the feature, in one place.

    It is the switch the whole WW3D2 half is written against, and it used to be a bare
    `#define ENABLE_EMBEDDED_BROWSER 1`. If it goes back to being one, every `#if` in
    dx8webbrowser.{h,cpp} silently means "on" again and the renderer needs <d3d8.h>, <windows.h>
    and LPDISPATCH once more -- with no ATL spelling anywhere for the checks above to catch.
    """
    definers = []
    for directory, _, names in os.walk(ROOT):
        if any(part in directory.split(os.sep) for part in (".git", "build", "docs", "scripts")):
            continue
        for name in names:
            if not name.endswith((".h", ".cpp", ".hpp", ".inl", "CMakeLists.txt", ".cmake")):
                continue
            path = os.path.join(directory, name)
            if re.search(r"^\s*#\s*define\s+%s\b" % DERIVED_FEATURE, read(path), re.M) or \
                    re.search(r"(?:add_compile_definitions|target_compile_definitions)"
                              r"[^)]*%s" % DERIVED_FEATURE, read(path)):
                definers.append(os.path.relpath(path, ROOT))
    if sorted(definers) != [DERIVED_DEFINITION_FILE]:
        failures.append("%s must be defined by %s and nothing else; found: %s"
                        % (DERIVED_FEATURE, DERIVED_DEFINITION_FILE,
                           ", ".join(sorted(definers)) or "nothing"))
    elif not DERIVED_FROM_FEATURE.search(read(os.path.join(ROOT, DERIVED_DEFINITION_FILE))):
        failures.append("%s: %s is not derived from %s; the DX8 half would compile "
                        "unconditionally again"
                        % (DERIVED_DEFINITION_FILE, DERIVED_FEATURE, FEATURE))
    print("derived switch: %s defined by %s, from %s"
          % (DERIVED_FEATURE, ", ".join(sorted(definers)) or "nothing", FEATURE))


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

    libraries = results["compiled"]
    for unit in REQUIRED_UNITS:
        posix = unit.replace(os.sep, "/")
        if not any(posix.startswith(library + "/") for library in libraries):
            # Levels 1-3 do not build WW3D2; only judge what this measurement compiled.
            print("%s: not in this measurement" % posix)
            continue
        diagnostic = results["compile_failures"].get(posix)
        if diagnostic:
            failures.append("%s must compile with the browser absent, but: %s"
                            % (posix, diagnostic.splitlines()[0]))
        else:
            print("%s: compiles" % posix)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--results", help="JSON written by native-build.py --json")
    args = parser.parse_args()

    failures = []
    check_sources(failures)
    check_feature_definition(failures)
    check_derived_definition(failures)
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

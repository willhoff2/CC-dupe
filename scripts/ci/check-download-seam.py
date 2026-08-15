#!/usr/bin/env python3
"""Check the patch-downloader seam: WWDownload is measured, and its registry half is portable.

`Core/Libraries/Source/WWVegas/WWDownload` is the FTP patch downloader. It is adjacent to the
online functionality the port cuts, but it is not on the online side of the cut: the single-player
main menu calls `CancelPatchCheckCallback()` unconditionally and pumps `TheDownloadManager` every
frame, so the native link has to resolve `Cftp` and `CDownload` whether or not a patch is ever
fetched. It is therefore *ported*, not excluded like GameSpy -- see
docs/porting/ww3d2-and-download-headers.md. Three things would quietly undo that:

  * a translation unit of the library stops compiling, so level 4 links a partial archive (or, as
    before this seam, no archive at all) and its symbols reappear in the "defined in a translation
    unit that failed to compile" category rather than in the binary;
  * `registry.cpp` grows a `Reg*` call outside its `_WIN32` branch, i.e. the settings store seam in
    `WWLib/platform/` is bypassed instead of used -- or loses the Win32 branch, which is the
    behavioural oracle;
  * a consumer grows a portability `#ifdef`. The seam exists so the call sites keep the Win32
    spelling on both platforms.

It also pins the measurement denominator. The probe and the native build read the translation units
from CMake rather than walking the directories, so a source that CMake comments out is not counted.
`textdraw.cpp` is the only such file in the renderer/audio/download tree -- MSVC has never compiled
it, which is why it references a `Peek_Texture()` overload that no longer exists -- and this check
fails if any other appears, because that would silently shrink the denominator.

Usage:
    python3 scripts/ci/check-download-seam.py
    python3 scripts/ci/check-download-seam.py --results native-build-level4.json
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

DOWNLOAD_DIR = os.path.join("Core", "Libraries", "Source", "WWVegas", "WWDownload")
DOWNLOAD_LIBRARY = "Core/Libraries/Source/WWVegas/WWDownload"

# The registry file, and the settings-store seam it must be written against.
REGISTRY_FILE = os.path.join(DOWNLOAD_DIR, "registry.cpp")
SETTINGS_SPELLING = "WWPlatform::Settings::"
WIN32_REGISTRY_CALL = re.compile(r"\bReg(?:OpenKeyEx|CloseKey|QueryValueEx|SetValueEx|CreateKeyEx)"
                                 r"[AW]?\s*\(")

# Consumers of the downloader, which must keep the Win32-era spelling and carry no port #ifdef.
CONSUMERS = {
    os.path.join("GeneralsMD", "Code", "GameEngine", "Source", "GameClient", "GUI", "GUICallbacks",
                 "Menus", "MainMenu.cpp"): ["CancelPatchCheckCallback"],
    os.path.join("Core", "GameEngine", "Source", "GameNetwork",
                 "DownloadManager.cpp"): ["CDownload"],
}
PORT_IFDEF = re.compile(r"#\s*(?:if|ifdef|ifndef|elif)[^\n]*"
                        r"\b(?:_WIN32|WIN32|_MSC_VER|__APPLE__|__clang__)\b")

# Sources present on disk but commented out of their CMakeLists.txt. Exactly one is expected.
KNOWN_UNBUILT = {"Core/Libraries/Source/WWVegas/WW3D2/textdraw.cpp",
                 "GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/textdraw.cpp"}
CMAKE_DIRS = [
    os.path.join("Core", "Libraries", "Source", "WWVegas", "WW3D2"),
    os.path.join("Core", "Libraries", "Source", "WWVegas", "WWAudio"),
    DOWNLOAD_DIR,
    os.path.join("GeneralsMD", "Code", "Libraries", "Source", "WWVegas", "WW3D2"),
    os.path.join("GeneralsMD", "Code", "Libraries", "Source", "WWVegas", "WWAudio"),
    os.path.join("GeneralsMD", "Code", "Libraries", "Source", "WWVegas", "WWDownload"),
]
COMMENTED_SOURCE = re.compile(r"^\s*#\s*([\w./-]+\.cpp)\b")


def read(path):
    with open(path, "r", errors="replace") as handle:
        return handle.read()


def check_registry(failures):
    text = read(os.path.join(ROOT, REGISTRY_FILE))
    if SETTINGS_SPELLING not in text:
        failures.append("%s: does not use %s; the registry-as-settings store in WWLib/platform/ "
                        "is the seam this file must go through off Windows"
                        % (REGISTRY_FILE, SETTINGS_SPELLING))
    if "#ifdef _WIN32" not in text and "#if defined(_WIN32)" not in text:
        failures.append("%s: has no _WIN32 branch; the Win32 registry path is the behavioural "
                        "oracle and must stay byte-for-byte" % REGISTRY_FILE)

    # Every Win32 registry call must sit inside a _WIN32 branch.
    depth_win32 = 0
    depth = 0
    stray = []
    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        if re.match(r"#\s*if", stripped):
            depth += 1
            if re.search(r"\b_WIN32\b", stripped) and "!" not in stripped:
                depth_win32 = depth
        elif re.match(r"#\s*else\b", stripped) and depth == depth_win32:
            depth_win32 = 0
        elif re.match(r"#\s*endif\b", stripped):
            if depth == depth_win32:
                depth_win32 = 0
            depth = max(0, depth - 1)
        elif not depth_win32 and WIN32_REGISTRY_CALL.search(stripped) \
                and not stripped.startswith("//"):
            stray.append((number, stripped))
    for number, line in stray:
        failures.append("%s:%d: Win32 registry call outside an _WIN32 branch: %s"
                        % (REGISTRY_FILE, number, line))
    print("%s: %s, Win32 branch kept, %d stray Reg* calls"
          % (REGISTRY_FILE, SETTINGS_SPELLING.rstrip(":"), len(stray)))


def check_consumers(failures):
    for relative, spellings in sorted(CONSUMERS.items()):
        path = os.path.join(ROOT, relative)
        if not os.path.isfile(path):
            failures.append("%s: missing" % relative)
            continue
        text = read(path)
        for spelling in spellings:
            if spelling not in text:
                failures.append("%s: no longer uses %s; the seam exists so the downloader's call "
                                "sites are unchanged" % (relative, spelling))
        found = PORT_IFDEF.search(text)
        if found:
            failures.append("%s: grew a portability #ifdef (%s); the portable half belongs under "
                            "the Win32 spelling, not in the consumer"
                            % (relative, found.group(0).strip()))
    print("consumers: %d checked for the unchanged Win32 spelling" % len(CONSUMERS))


def check_denominator(failures):
    """No source may drop out of the measured set by being commented out of CMake."""
    unbuilt = []
    for directory in CMAKE_DIRS:
        lists = os.path.join(ROOT, directory, "CMakeLists.txt")
        if not os.path.isfile(lists):
            failures.append("%s/CMakeLists.txt: missing" % directory)
            continue
        for line in read(lists).splitlines():
            match = COMMENTED_SOURCE.match(line)
            if match and os.path.isfile(os.path.join(ROOT, directory, match.group(1))):
                unbuilt.append("%s/%s" % (directory.replace(os.sep, "/"), match.group(1)))
    unexpected = sorted(set(unbuilt) - KNOWN_UNBUILT)
    if unexpected:
        failures.append("sources on disk but commented out of CMake, so absent from the measured "
                        "denominator: %s. Either build them or add them here with a reason."
                        % ", ".join(unexpected))
    print("measured denominator: %d source(s) commented out of CMake, all known" % len(unbuilt))


def check_results(path, failures):
    results = json.loads(read(path))
    library = results["compiled"].get(DOWNLOAD_LIBRARY)
    if library is None:
        failures.append("%s is not in the measured build; excluding it would relabel its symbols "
                        "rather than let the single-player binary link" % DOWNLOAD_LIBRARY)
        return
    if library["objects"] != library["total"]:
        failures.append("%s produced %d/%d objects; every unit of the patch downloader must "
                        "compile" % (DOWNLOAD_LIBRARY, library["objects"], library["total"]))
    if DOWNLOAD_LIBRARY in results.get("libraries_without_archive", []):
        failures.append("%s produced no archive" % DOWNLOAD_LIBRARY)
    print("%s: %d/%d objects, archive produced"
          % (DOWNLOAD_LIBRARY, library["objects"], library["total"]))


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--results", help="JSON written by native-build.py --json (levels 1-4)")
    args = parser.parse_args()

    failures = []
    check_registry(failures)
    check_consumers(failures)
    check_denominator(failures)
    if args.results:
        check_results(args.results, failures)

    if failures:
        print("", file=sys.stderr)
        for failure in failures:
            print("FAIL: %s" % failure, file=sys.stderr)
        return 1

    print("OK: the patch downloader is built, portable, and its consumers are unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main())

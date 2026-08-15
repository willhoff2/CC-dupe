#!/usr/bin/env python3
"""Gate the video path and the third-party headers the native harness provisions.

Three failure classes are closed by docs/porting/video-and-harness-headers.md and each of them
would come back silently:

* **Bink off Windows.** `bink.h` ships only with the 32-bit Windows SDK stub, so any unguarded
  include of it (or of `VideoDevice/Bink/BinkVideoPlayer.h`) breaks every translation unit that
  reaches it -- six did, five of them only transitively through `W3DGameClient.h`. Video goes
  through `RTS_HAS_FFMPEG`; Bink compiles out. Checked structurally, so it fails on a re-added
  include even without a build.
* **Provisioning.** `stb_image_write.h` and `libavcodec/avcodec.h` come from
  `scripts/ci/fetch-probe-deps.sh`. Whether a dependency is present changes the measured clean
  count, so a run that silently lost one is a measurement, not a result.
* **Include case.** `#include "Common/File.h"` resolved on Windows and on macOS's default
  case-insensitive filesystem, and only ever failed on a case-sensitive one. Since a Linux CI run
  is the only place that notices, the count of quoted includes whose case does not match the file
  on disk is ratcheted rather than left to the next porter to rediscover.

The translation-unit half needs the build results; the structural halves do not:

    python3 scripts/ci/check-video-headers.py                       # structural checks only
    python3 scripts/ci/check-video-headers.py --results build.json  # and the compiled units
    python3 scripts/ci/check-video-headers.py --deps-dir build/docker/_deps
"""
import argparse
import json
import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

# The video translation units that must produce an object file natively. BinkVideoPlayer.cpp is
# here deliberately: it must compile (to an empty translation unit off Windows), not be excluded
# from the build, so that the Windows path stays in the source list where the next porter can see
# it.
REQUIRED_OBJECTS = [
    "Core/GameEngineDevice/Source/VideoDevice/Bink/BinkVideoPlayer.cpp",
    "Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegFile.cpp",
    "Core/GameEngineDevice/Source/VideoDevice/FFmpeg/FFmpegVideoPlayer.cpp",
    "Core/GameEngineDevice/Source/W3DDevice/GameClient/stb_image_write_impl.cpp",
]

# Diagnostics that mean one of these classes is back. Matched against the recorded first
# diagnostic of every failed translation unit.
FORBIDDEN_DIAGNOSTICS = ("bink.h", "stb_image_write.h", "libavcodec/", "libswscale/",
                         "libavformat/", "Common/File.h")

# Headers only Windows provides them, and the guard that must wrap every include of them.
BINK_INCLUDES = re.compile(r'#\s*include\s+[<"](?:bink\.h|VideoDevice/Bink/BinkVideoPlayer\.h)[>"]')
BINK_GUARD = re.compile(r'#\s*(?:if|ifdef|elif)\b[^\n]*\bRTS_HAS_BINK\b')

# Directories the fetch script provisions, and one header out of each that a translation unit in
# the tree includes.
PROVISIONED = {
    "stb-src": "stb_image_write.h",
    "ffmpeg-src": "libavcodec/avcodec.h",
}

SOURCE_ROOTS = ("Core", "GeneralsMD/Code")
SOURCE_SUFFIXES = (".cpp", ".c", ".h", ".hpp", ".inl", ".mm")
QUOTED_INCLUDE = re.compile(r'#\s*include\s+"([^"]+)"')

# Quoted includes whose case does not match the file on disk, and that this slice is not fixing:
# `Core/Tools/**` is out of scope for the port entirely, and the WWLib/WW3D2 ones belong to
# translation units the renderer slice owns. The point of the number is that it may not grow.
CASE_MISMATCH_BUDGET = 7


def source_files():
    for root in SOURCE_ROOTS:
        for path in sorted((REPO_ROOT / root).rglob("*")):
            if path.is_file() and path.suffix.lower() in SOURCE_SUFFIXES:
                yield path


def check_bink_guarded(failures):
    """Every include of a Bink header sits *inside* an `#ifdef RTS_HAS_BINK` region.

    Positional rather than "the file mentions RTS_HAS_BINK somewhere": a re-added include above or
    below the guarded block is exactly the regression this is here to catch.
    """
    for path in source_files():
        text = path.read_text(errors="replace")
        if not BINK_INCLUDES.search(text):
            continue
        guards = []  # one entry per open conditional: does it test RTS_HAS_BINK?
        guarded = True
        for line in text.splitlines():
            stripped = line.lstrip()
            if re.match(r"#\s*if", stripped):
                guards.append(bool(BINK_GUARD.match(stripped)))
            elif re.match(r"#\s*(elif|else)", stripped) and guards:
                guards[-1] = bool(BINK_GUARD.match(stripped))
            elif re.match(r"#\s*endif", stripped) and guards:
                guards.pop()
            elif BINK_INCLUDES.match(stripped) and not any(guards):
                guarded = False
                failures.append(
                    f"{path.relative_to(REPO_ROOT)} includes a Bink header outside an "
                    f"RTS_HAS_BINK region ({stripped}); bink.h exists only in the 32-bit "
                    "Windows build")
        if guarded:
            print(f"ok: {path.relative_to(REPO_ROOT)} includes Bink under RTS_HAS_BINK")


def check_include_case(failures):
    """Quoted includes that only resolve on a case-insensitive filesystem."""
    headers = {}
    for path in source_files():
        if path.suffix.lower() in (".h", ".hpp", ".inl"):
            headers.setdefault(str(path.relative_to(REPO_ROOT)).lower(), []).append(
                str(path.relative_to(REPO_ROOT)))

    mismatches = []
    for path in source_files():
        text = path.read_text(errors="replace")
        for included in QUOTED_INCLUDE.findall(text):
            suffix = "/" + included.lower()
            candidates = [name for key, names in headers.items() if key.endswith(suffix)
                          for name in names]
            if not candidates:
                continue  # a shim, a third-party header, or a path this check cannot resolve
            if not any(name.endswith("/" + included) or name == included for name in candidates):
                mismatches.append(f"{path.relative_to(REPO_ROOT)}: #include \"{included}\" "
                                  f"but the file is {candidates[0]}")

    print(f"quoted includes whose case does not match the file on disk: {len(mismatches)} "
          f"(budget {CASE_MISMATCH_BUDGET})")
    for line in mismatches:
        print(f"    {line}")
    if len(mismatches) > CASE_MISMATCH_BUDGET:
        failures.append(f"case-mismatched includes: {len(mismatches)} found, budget is "
                        f"{CASE_MISMATCH_BUDGET}; these compile on Windows and on a "
                        "case-insensitive macOS filesystem and fail everywhere else")


def check_provisioned(deps_dir, failures):
    for subdir, header in PROVISIONED.items():
        path = deps_dir / subdir / header
        if path.is_file():
            print(f"ok: {subdir}/{header} provisioned")
        else:
            failures.append(f"{path} is missing; run scripts/ci/fetch-probe-deps.sh -- the "
                            "measured clean count depends on it being there")


def check_results(results, failures):
    compile_failures = results.get("compile_failures", {})
    for source in REQUIRED_OBJECTS:
        if source in compile_failures:
            failures.append(f"{source} produced no object file: "
                            f"{compile_failures[source] or 'no diagnostic recorded'}")
        else:
            print(f"ok: {source} compiled")

    for source, diagnostic in sorted(compile_failures.items()):
        for needle in FORBIDDEN_DIAGNOSTICS:
            if needle in diagnostic:
                failures.append(f"{source} failed on a header this slice closed: {diagnostic}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", help="JSON written by native-build.py --json")
    ap.add_argument("--deps-dir", default=str(REPO_ROOT / "build" / "docker" / "_deps"),
                    help="where fetch-probe-deps.sh put the third-party headers")
    args = ap.parse_args()

    failures = []
    check_bink_guarded(failures)
    check_include_case(failures)

    deps_dir = pathlib.Path(args.deps_dir)
    if deps_dir.is_dir():
        check_provisioned(deps_dir, failures)
    else:
        print(f"note: {deps_dir} does not exist; skipping the provisioning check")

    if args.results:
        check_results(json.loads(pathlib.Path(args.results).read_text()), failures)
    else:
        print("note: no --results; skipping the compiled-translation-unit check")

    if failures:
        print()
        print("FAIL: the video path or the harness's headers regressed", file=sys.stderr)
        for line in failures:
            print(f"  - {line}", file=sys.stderr)
        return 1

    print()
    print("OK: Bink compiles out off Windows, the provisioned headers are present, and the video "
          "translation units build")
    return 0


if __name__ == "__main__":
    sys.exit(main())

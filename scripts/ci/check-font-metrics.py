#!/usr/bin/env python3
"""Compare the port's glyph metrics against recorded GDI metrics.

The GDI font seam replaces GDI's rasteriser off Windows (docs/porting/gdi-font-seam.md). What the
WND GUI consumes from it is not the pixels but the *numbers*: the advance width of each character
and the line height, which is what every authored menu coordinate was laid out against. A
rasteriser that draws beautiful text with different advances silently breaks every screen, and
nothing else in the build would notice.

So this builds the port's CreateFont/GetTextMetrics/GetTextExtentPoint32W --
WWLib/platform/platform_win32_gdi_font.cpp -- against the same dumper the reference was recorded
with, runs it on the same font file, and compares:

  * tmHeight, tmAscent, tmDescent and tmOverhang exactly. Line height is a layout input and an
    off-by-one there moves every line of every multi-line label.
  * every printable ASCII advance to within one pixel, and the width the run of them sums to
    within one percent. A handful of glyphs per size differ by exactly one pixel because GDI's
    advances are grid-fitted by the font's own hinting instructions and stb_truetype does not run
    them; the sum bound is what keeps those from being allowed to lean the same way and walk a
    label's right edge off the widget.

The reference is recorded rather than live: scripts/ci/font-metrics-reference.json holds what Wine's
gdi32 reported in the container scripts/docker-build.sh builds the Windows executables with, and its
"provenance" block says so. Re-record with scripts/ci/record-gdi-font-metrics.sh, or on Windows by
building the dumper there.

Usage:
    python3 scripts/ci/check-font-metrics.py [--reference FILE] [--font-file FILE]
                                            [--advance-tolerance N] [--verbose]
"""

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
CLANGXX = os.environ.get("CLANGXX", "clang++")

PLATFORM_DIR = REPO_ROOT / "Core/Libraries/Source/WWVegas/WWLib/platform"
DEFAULT_REFERENCE = REPO_ROOT / "scripts/ci/font-metrics-reference.json"
DEFAULT_DEPS_DIR = REPO_ROOT / "build/docker/_deps"

# The seam plus the stub reporter it calls when a face has no file at all, plus the dumper. The
# other halves of the Win32 compatibility layer have no bearing on the metrics.
SOURCES = [
    PLATFORM_DIR / "platform_win32_gdi_font.cpp",
    PLATFORM_DIR / "platform_win32_file.cpp",
    PLATFORM_DIR / "platform_path.cpp",
    REPO_ROOT / "Core/Libraries/Source/WWVegas/WWLib/wwstring.cpp",
    PLATFORM_DIR / "tests/gdi_font_metrics_dump.cpp",
]

INCLUDES = [
    REPO_ROOT / "scripts/native-port-shims",
    REPO_ROOT / "Dependencies/Utility",
    REPO_ROOT / "Core/Libraries/Include",
    REPO_ROOT / "Core/Libraries/Source/WWVegas",
    REPO_ROOT / "Core/Libraries/Source/WWVegas/WWLib",
]

COMPILE_FLAGS = [
    "-std=c++20",
    "-m64",
    "-g",
    "-O0",
    "-fms-extensions",
    "-include", "Utility/CppMacros.h",
    "-DWIN32_LEAN_AND_MEAN",
    "-D_REENTRANT",
]

EXACT_FIELDS = ["tm_height", "tm_ascent", "tm_descent", "tm_overhang"]


def stb_include(deps_dir):
    """<stb_truetype.h>, from the same place scripts/native-build.py takes it."""
    candidates = [deps_dir / "stb-src", REPO_ROOT / "build/native/_deps/stb-src"]
    for candidate in candidates:
        if (candidate / "stb_truetype.h").is_file():
            return candidate
    return None


def build(build_dir, deps_dir, verbose):
    stb = stb_include(deps_dir)
    if stb is None:
        print("stb_truetype.h was not found. Run scripts/ci/fetch-probe-deps.sh first.")
        return None

    binary = build_dir / "gdi_font_metrics_dump"
    command = [CLANGXX] + COMPILE_FLAGS
    for include in INCLUDES + [stb]:
        command += ["-I", str(include)]
    command += [str(source) for source in SOURCES]
    command += ["-o", str(binary)]

    if verbose:
        print("+", " ".join(command))
    result = subprocess.run(command, cwd=REPO_ROOT)
    if result.returncode != 0:
        print("FAILED: the font seam did not compile or link")
        return None

    return binary


def measure(binary, font_file, face_name, verbose):
    # WW_FONT_PATH is how the seam is pointed at one directory: the comparison has to be against
    # the same file the reference was recorded with, not whatever the machine's font database
    # would substitute.
    environment = dict(os.environ)
    environment["WW_FONT_PATH"] = str(pathlib.Path(font_file).parent)

    command = [str(binary), str(font_file), face_name]
    if verbose:
        print("+", " ".join(command))
    result = subprocess.run(command, capture_output=True, text=True, env=environment)
    if result.returncode != 0:
        print(f"FAILED: the dumper exited {result.returncode}: {result.stderr.strip()}")
        return None

    return json.loads(result.stdout)


def compare_case(reference, measured, first_char, tolerance, run_tolerance, failures):
    label = (f"{reference['point_size']}pt"
             f"{' bold' if reference['bold'] else ''}"
             f"{' condensed' if reference['condensed'] else ''}")

    for field in EXACT_FIELDS:
        if reference[field] != measured[field]:
            failures.append(f"{label}: {field} is {measured[field]}, "
                            f"GDI reports {reference[field]}")

    reference_advances = reference["advances"]
    measured_advances = measured["advances"]
    if len(reference_advances) != len(measured_advances):
        failures.append(f"{label}: {len(measured_advances)} advances against "
                        f"{len(reference_advances)} recorded")
        return

    worst = 0
    worst_char = None
    for index, (expected, actual) in enumerate(zip(reference_advances, measured_advances)):
        difference = abs(actual - expected)
        if difference > worst:
            worst = difference
            worst_char = chr(first_char + index)
        if difference > tolerance:
            failures.append(f"{label}: advance of '{chr(first_char + index)}' is {actual}, "
                            f"GDI reports {expected}")

    # The sum is what a line of text is actually laid out with, so a per-character tolerance that
    # always leans the same way would still move a label's right edge. Hold the string width to
    # the same tolerance the individual characters get.
    reference_total = sum(reference_advances)
    measured_total = sum(measured_advances)
    allowed = max(float(tolerance), reference_total * run_tolerance / 100.0)
    if abs(measured_total - reference_total) > allowed:
        failures.append(f"{label}: the printable ASCII run measures {measured_total} pixels "
                        f"against GDI's {reference_total}, outside {allowed:.1f}")

    return worst, worst_char


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--reference", default=str(DEFAULT_REFERENCE))
    parser.add_argument("--font-file", help="override the font file the reference names")
    parser.add_argument("--deps-dir", default=str(DEFAULT_DEPS_DIR))
    parser.add_argument("--advance-tolerance", type=int, default=1,
                        help="how many pixels an advance may differ from GDI's (default 1)")
    parser.add_argument("--run-tolerance", type=float, default=1.0,
                        help="percent the printable ASCII run's width may differ (default 1.0)")
    parser.add_argument("--keep", action="store_true", help="keep the build directory")
    parser.add_argument("--verbose", action="store_true", help="echo the commands")
    args = parser.parse_args()

    if sys.platform == "win32":
        print("On Windows the real GDI is the implementation; there is nothing to compare.")
        return 0

    with open(args.reference) as handle:
        reference = json.load(handle)

    font_file = args.font_file
    if font_file is None:
        # The reference records the path inside the build container; the file itself is the
        # system's, so it is looked up by name here.
        font_name = reference.get("font_name")
        for directory in ["/usr/share/fonts", "/usr/local/share/fonts", "/Library/Fonts",
                          "/System/Library/Fonts"]:
            match = next(pathlib.Path(directory).rglob(font_name), None) \
                if pathlib.Path(directory).is_dir() else None
            if match is not None:
                font_file = str(match)
                break

    if font_file is None or not pathlib.Path(font_file).is_file():
        print(f"FAILED: {reference.get('font_name')} is not installed, so there is nothing to "
              f"compare the recorded metrics against. Install it or pass --font-file.")
        return 1

    build_dir = pathlib.Path(tempfile.mkdtemp(prefix="font-metrics-"))
    try:
        binary = build(build_dir, pathlib.Path(args.deps_dir), args.verbose)
        if binary is None:
            return 1

        measured = measure(binary, font_file, reference["face_requested"], args.verbose)
        if measured is None:
            return 1

        failures = []
        worst = 0
        for reference_case, measured_case in zip(reference["cases"], measured["cases"]):
            result = compare_case(reference_case, measured_case, reference["first_char"],
                                  args.advance_tolerance, args.run_tolerance, failures)
            if result is not None and result[0] > worst:
                worst = result[0]

        provenance = reference.get("provenance", {})
        recorded = "" if provenance.get("is_retail_windows") else " (recorded, not retail Windows)"
        print(f"reference: {provenance.get('implementation', 'unknown')}{recorded}")
        print(f"font: {font_file}")
        print(f"cases: {len(reference['cases'])}, worst advance difference: {worst}px, "
              f"tolerance: {args.advance_tolerance}px per glyph, "
              f"{args.run_tolerance}% per run")

        if failures:
            print(f"FAILED: {len(failures)} metric(s) differ from the recorded GDI numbers")
            for failure in failures[:40]:
                print(f"  {failure}")
            if len(failures) > 40:
                print(f"  ... and {len(failures) - 40} more")
            return 1

        print("OK: the port's glyph metrics match the recorded GDI metrics")
        return 0
    finally:
        if args.keep:
            print(f"build directory kept at {build_dir}")
        else:
            shutil.rmtree(build_dir, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())

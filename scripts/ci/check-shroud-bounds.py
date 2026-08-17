#!/usr/bin/env python3
"""The gate for the shroud's out-of-grid lookup, the crash the river water path reached.

`WaterRenderObjClass::drawRiverWater` shades every river vertex with the shroud, and a river
polygon is authored into the map's border ring, whose world coordinates are negative -- MD_USA01's
`Water Area 5` has a vertex at world (1923, -79), which is cell (48, -2) of a 118x80 grid. The
lookup only tested the far end of the grid, so that cell indexed the shroud data from before its
allocation: silent on Windows, `EXC_BAD_ACCESS`/`SIGSEGV` on this port.

Two halves, because one alone would let the defect back in:

  * the measurement, refused outright if the probe is older than the sources it measures.
    `sim_probe shroudbounds` calls `W3DShroud::getShroudLevelAtWorldPos` -- the
    engine's own code, on a grid mmap'ed between two `PROT_NONE` pages, so an index outside the grid
    faults here on every platform instead of reading a neighbouring allocation. The vertices fed in
    are the ones measured out of the retail MD_USA01 with `sim_probe rivers`, and each has to come
    back with the border shroud level rather than a level read out of the grid or a signal. With the
    bound or the flooring removed the probe dies on the first out-of-grid vertex and this fails.
  * the source check, over both trees. The native build is GeneralsMD only, so nothing runnable
    covers Generals' copy of `W3DShroud.cpp`; without this half the bound could be dropped there and
    every gate would stay green until a Generals river map faulted.

    python3 scripts/ci/check-shroud-bounds.py --build-dir build/native

See docs/porting/shroud-river-water-bounds.md. No retail data is used: the vertices are numbers, and
the grid is generated, so this runs anywhere `scripts/native-sim-probe.py --build` builds.
"""
import argparse
import pathlib
import re
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

# The grid MD_USA01 produces (610x460 map, border 70, PartitionCellSize 40), and a border level
# distinct from every level the filled grid can report.
CELLS_X = 118
CELLS_Y = 80
CELL_SIZE = 40
BORDER_LEVEL = 60
GRID_LEVEL = 255

# (world x, world y, inside the grid?, why this point is here)
POINTS = [
    (1923, -79, False, "MD_USA01 'Water Area 5' point 55, the vertex #103 crashed on"),
    (1910, -149, False, "MD_USA01 'Water Area 5' point 54, deeper into the border ring"),
    (2069, -158, False, "MD_USA01 'Water Area 5' point 52"),
    (-320, 1759, False, "MD_USA01 'Water Area 5' point 0, off the left edge instead of the top"),
    (-1, -1, False, "just outside the near corner: truncation would call this cell (0,0)"),
    (-40.5, 10, False, "one cell left of the grid, where truncation and flooring differ"),
    (0, 0, True, "the grid's own near corner"),
    (100, 100, True, "well inside the grid"),
    (CELLS_X * CELL_SIZE - 1, CELLS_Y * CELL_SIZE - 1, True, "the grid's last cell"),
    (CELLS_X * CELL_SIZE, CELLS_Y * CELL_SIZE, False, "one cell past the far end"),
]

# Both trees' copies of the lookup and its one caller.
SHROUD_SOURCES = [
    "GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DShroud.cpp",
    "Generals/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DShroud.cpp",
]
WATER_SOURCE = "Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp"

LOW_BOUND = re.compile(r"x\s*>=\s*0\s*&&\s*y\s*>=\s*0")
WORLD_LOOKUP = re.compile(
    r"W3DShroudLevel W3DShroud::getShroudLevelAtWorldPos\(Real x, Real y\)\s*\{(.*?)\n\}",
    re.DOTALL)


def require(condition, message, failures):
    if not condition:
        failures.append(message)


def check_probe_fresh(probe, failures):
    """A probe older than the code it measures answers for the previous tree.

    Found the hard way: with the bound reverted but the probe left alone, the measurement half of
    this gate passed. Rather than trust whoever built last, refuse to measure at all.
    """
    built = probe.stat().st_mtime
    sources = [REPO_ROOT / "spikes" / "sim" / "src" / "sim_probe.cpp"]
    sources += [REPO_ROOT / relative for relative in SHROUD_SOURCES]
    stale = [str(source.relative_to(REPO_ROOT)) for source in sources
             if source.stat().st_mtime > built]
    require(not stale,
            "{} is older than {} -- rebuild it with `python3 scripts/native-sim-probe.py "
            "--build-dir {} --build`, or the measurement answers for the previous tree".format(
                probe.relative_to(REPO_ROOT), ", ".join(stale),
                probe.parent.relative_to(REPO_ROOT)), failures)


def run_probe(probe, failures):
    """The engine's lookup over a guarded grid; returns the probe's output."""
    coords = []
    for x, y, _inside, _why in POINTS:
        coords += [repr(float(x)), repr(float(y))]
    result = subprocess.run(
        [str(probe), "shroudbounds", str(CELLS_X), str(CELLS_Y), str(CELL_SIZE),
         str(BORDER_LEVEL)] + coords,
        capture_output=True, text=True, errors="replace", cwd=str(REPO_ROOT))
    output = result.stdout + result.stderr

    require(result.returncode == 0,
            "the probe exited {} -- an out-of-grid vertex read outside the shroud grid, which is "
            "the crash this gate exists for".format(result.returncode), failures)
    require("RESULT shroudbounds points={} survived=yes".format(len(POINTS)) in output,
            "the probe did not report every vertex answered", failures)
    require("RESULT shroudbounds cellsX={} cellsY={} cellSize={:.2f} borderLevel={} "
            "gridLevel={}".format(CELLS_X, CELLS_Y, float(CELL_SIZE), BORDER_LEVEL, GRID_LEVEL)
            in output,
            "the fixture grid is not the one the expectations below are written against", failures)

    for x, y, inside, why in POINTS:
        expected = GRID_LEVEL if inside else BORDER_LEVEL
        line = "RESULT shroudpoint world=({:.2f},{:.2f}) cell=".format(float(x), float(y))
        matches = [ln for ln in output.splitlines() if ln.startswith(line)]
        if not matches:
            failures.append("no result for world ({}, {}) -- {}".format(x, y, why))
            continue
        require("inGrid={}".format("yes" if inside else "no") in matches[0],
                "world ({}, {}) was classified the wrong side of the grid: {}".format(
                    x, y, matches[0]), failures)
        require(matches[0].endswith("level={}".format(expected)),
                "world ({}, {}) reported {} rather than level={} -- {}".format(
                    x, y, matches[0].split()[-1], expected, why), failures)
    return output


def check_sources(failures):
    """The bound and the flooring conversion, in both trees, and the caller that needs them."""
    for relative in SHROUD_SOURCES:
        text = (REPO_ROOT / relative).read_text(encoding="latin-1")
        require(LOW_BOUND.search(text) is not None,
                "{}: getShroudLevel() does not test x >= 0 && y >= 0, so a cell left of or above "
                "the grid indexes the shroud data before its allocation".format(relative),
                failures)

        body = WORLD_LOOKUP.search(text)
        if body is None:
            failures.append("{}: no W3DShroud::getShroudLevelAtWorldPos, so a caller holding world "
                            "coordinates has to convert to a cell itself".format(relative))
            continue
        require("REAL_TO_INT_FLOOR" in body.group(1),
                "{}: getShroudLevelAtWorldPos does not floor the conversion -- truncation maps the "
                "cell of world (-1, -1) to (0, 0) and shades the border ring with the grid's near "
                "corner".format(relative), failures)
        require("m_boderShroudLevel" in body.group(1),
                "{}: getShroudLevelAtWorldPos does not answer with the border shroud level, which "
                "is what the shroud pass shows outside the grid".format(relative), failures)

    water = (REPO_ROOT / WATER_SOURCE).read_text(encoding="latin-1")
    require("getShroudLevelAtWorldPos" in water,
            "{}: getRiverVertexDiffuse no longer asks the shroud for a world position".format(
                WATER_SOURCE), failures)
    require("getShroudLevel(" not in water,
            "{}: a cell is converted here again; the conversion belongs to the shroud, which is "
            "the only thing that knows where its grid ends".format(WATER_SOURCE), failures)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default="build/native",
                        help="build directory holding sim_probe (default: build/native)")
    args = parser.parse_args()

    probe = (REPO_ROOT / args.build_dir / "sim_probe").resolve()
    if not probe.is_file():
        print("sim_probe not found at {}".format(probe))
        print("build it with: python3 scripts/native-sim-probe.py --build-dir {} --build".format(
            args.build_dir))
        return 2

    failures = []
    check_probe_fresh(probe, failures)
    if failures:
        print("FAIL: {}".format(failures[0]))
        return 1

    output = run_probe(probe, failures)
    check_sources(failures)

    for line in output.splitlines():
        if line.startswith("RESULT") or "ASSERTION" in line:
            print(line)

    if failures:
        print("\nFAIL: the shroud's out-of-grid lookup is not bounded")
        for failure in failures:
            print("  - {}".format(failure))
        return 1

    print("\nOK: every river vertex outside the shroud grid answers with the border shroud level, "
          "and neither tree's bound or flooring conversion is missing")
    return 0


if __name__ == "__main__":
    sys.exit(main())

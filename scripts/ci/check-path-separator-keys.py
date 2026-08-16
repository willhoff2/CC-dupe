#!/usr/bin/env python3
"""The negative control for the map cache half of the path-separator seam.

The other two defects in that seam announce themselves: an `open()` that fails returns nullptr, and
`MapUtil.cpp`'s missing-backslash assertion stops a debug build. The map cache defect does neither.
A map cache key is an *identifier*, and two spellings of one map are two keys, so a cache lookup
misses quietly and the engine carries on with re-derived or absent metadata -- including
`m_isMultiplayer`, which decides whether a map is offered as a multiplayer map at all. Wrong state,
no failure. So it needs a control that fails on the unfixed code, and this is it.

What is fed in is a `Maps/MapCache.ini` written the way both kinds of run write one: one entry keyed
the way the retail cache spells it (`Maps\\Name\\Name.map`) and one keyed the way a native build
spelled it before this seam landed (`Maps/Name/Name.map`). Each entry is then looked up under *both*
spellings through `MapCache::findMap`, which is the engine's own lookup, and every lookup has to
find the entry with `isMultiplayer` and the player count intact.

On the unfixed code two of those four lookups miss, and the gate fails. Nothing about the lookup is
relaxed to make it pass -- the fix is that a key is canonicalized where it is formed.

A second fixture covers the same seam's other silent consequence: `INI::parseMapCacheDefinition`
derives a display name for an entry with no localization tag with `name.reverseFind('\\') + 1`,
which on a `/`-spelled key adds one to a null pointer. That entry has to survive the read.

    python3 scripts/ci/check-path-separator-keys.py --build-dir build/native-debug

No retail data is used: the fixture is generated here, so this gate runs anywhere the headless
harness (`scripts/native-sim-probe.py --build`) builds. See docs/porting/path-separator-seam.md.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

MAGIC_CHAR = "_"

# The two entries of the key fixture: (map path as the cache would have keyed it, players).
WINDOWS_SPELLED = "Maps\\Fixture Beta\\Fixture Beta.map"
POSIX_SPELLED = "Maps/Fixture Alpha/Fixture Alpha.map"


def to_quoted_printable(text):
    """The encoding `AsciiStringToQuotedPrintable` uses for a cache key: `_HH` per non-alnum byte."""
    out = []
    for char in text:
        if char.isalnum() and char.isascii():
            out.append(char)
        else:
            out.append("{}{:02X}".format(MAGIC_CHAR, ord(char)))
    return "".join(out)


def cache_entry(key, players, multiplayer, name_tag):
    """One `MapCache` block, in the layout `MapCache::writeCacheINI` produces."""
    lines = [
        "",
        "MapCache {}".format(to_quoted_printable(key)),
        "  fileSize = 275491",
        "  fileCRC = 3735677156",
        "  timestampLo = 1398715739",
        "  timestampHi = 29585265",
        "  isOfficial = yes",
        "  isMultiplayer = {}".format("yes" if multiplayer else "no"),
        "  numPlayers = {}".format(players),
        "  extentMin = X:0.00 Y:0.00 Z:0.00",
        "  extentMax = X:2400.00 Y:2600.00 Z:0.00",
        "  nameLookupTag = {}".format(name_tag),
        "  InitialCameraPosition = X:100.00 Y:100.00 Z:0.00",
    ]
    for player in range(1, players + 1):
        lines.append("  Player_{}_Start = X:{}.00 Y:100.00 Z:0.00".format(player, player * 100))
    lines.append("END")
    lines.append("")
    return "\n".join(lines)


def write_fixture(directory, entries):
    maps = directory / "Maps"
    maps.mkdir(parents=True, exist_ok=True)
    # Zero Hour's archive file system reads Generals' install path out of the settings store and
    # asserts when it is unset, which has nothing to do with this gate: it is pointed at the fixture
    # itself, which holds no archives, so the read succeeds and loads nothing.
    (directory / "Registry.ini").write_text(
        "[SOFTWARE\\Electronic Arts\\EA Games\\Generals]\n"
        "STRING_InstallPath={}\n".format(directory), encoding="latin-1")
    text = "; auto-generated fixture for check-path-separator-keys.py\n"
    text += "".join(entries)
    (maps / "MapCache.ini").write_text(text, encoding="latin-1")


def run_probe(probe, directory, names):
    result = subprocess.run(
        [str(probe), "mapcachekeys", str(directory)] + names,
        capture_output=True, text=True, errors="replace", cwd=str(REPO_ROOT),
        # The settings store is per user and the harness must not pick up a developer's own one, or
        # a stale install path would change what the archive file system loads.
        env={"HOME": str(directory), "PATH": "/usr/bin:/bin",
             "CNC_SETTINGS_FILE": str(directory / "Registry.ini"),
             "CNC_USER_DATA": str(directory / "userdata")},
    )
    return result


def require(condition, message, failures):
    if not condition:
        failures.append(message)


def check_key_resolution(probe, workdir, failures):
    """Both spellings of both maps must resolve, with their multiplayer state intact."""
    directory = workdir / "keys"
    write_fixture(directory, [
        cache_entry(POSIX_SPELLED, 4, True, "MAP:FixtureAlpha"),
        cache_entry(WINDOWS_SPELLED, 2, True, "MAP:FixtureBeta"),
    ])

    lookups = [
        (POSIX_SPELLED, 4),
        (POSIX_SPELLED.replace("/", "\\"), 4),
        (WINDOWS_SPELLED, 2),
        (WINDOWS_SPELLED.replace("\\", "/"), 2),
    ]
    result = run_probe(probe, directory, [name for name, _ in lookups])
    output = result.stdout + result.stderr

    require(result.returncode == 0,
            "the probe exited {} (a lookup missed, or the read did not survive)".format(
                result.returncode), failures)
    require("RESULT mapcachekeys entries=2 lookups=4 missed=0" in output,
            "expected 2 entries and 4 resolved lookups", failures)

    for name, players in lookups:
        expected = "RESULT lookup name={} found=yes multiplayer=yes players={}".format(
            name, players)
        require(expected in output,
                "no line '{}' -- that spelling did not resolve to the cached map, or its "
                "multiplayer state was lost".format(expected), failures)

    return output


def check_untagged_entry_survives(probe, workdir, failures):
    """A `/`-spelled key with no localization tag: the display-name path must not walk off a null."""
    directory = workdir / "untagged"
    write_fixture(directory, [cache_entry(POSIX_SPELLED, 4, True, "")])

    result = run_probe(probe, directory, [POSIX_SPELLED.replace("/", "\\")])
    output = result.stdout + result.stderr

    require(result.returncode == 0,
            "the probe exited {} reading an untagged, '/'-spelled entry".format(result.returncode),
            failures)
    require("RESULT mapcachekeys entries=1 lookups=1 missed=0" in output,
            "the untagged entry was not readable and findable under the Windows spelling", failures)
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", default="build/native-debug",
                        help="build directory holding sim_probe (default: build/native-debug)")
    parser.add_argument("--keep", action="store_true", help="keep the generated fixtures")
    args = parser.parse_args()

    probe = (REPO_ROOT / args.build_dir / "sim_probe").resolve()
    if not probe.is_file():
        print("sim_probe not found at {}".format(probe))
        print("build it with: python3 scripts/native-sim-probe.py --build-dir {} --build".format(
            args.build_dir))
        return 2

    workdir = pathlib.Path(tempfile.mkdtemp(prefix="path-separator-keys-"))
    failures = []
    try:
        key_output = check_key_resolution(probe, workdir, failures)
        untagged_output = check_untagged_entry_survives(probe, workdir, failures)
    finally:
        if args.keep:
            print("fixtures kept in {}".format(workdir))
        else:
            shutil.rmtree(workdir, ignore_errors=True)

    for line in key_output.splitlines():
        if line.startswith("RESULT") or "ASSERTION" in line:
            print(line)
    for line in untagged_output.splitlines():
        if line.startswith("RESULT") or "ASSERTION" in line:
            print(line)

    if failures:
        print("\nFAIL: the map cache does not resolve both spellings of a map to one entry")
        for failure in failures:
            print("  - {}".format(failure))
        return 1

    print("\nOK: both spellings of a cached map resolve to the same entry, multiplayer state intact")
    return 0


if __name__ == "__main__":
    sys.exit(main())

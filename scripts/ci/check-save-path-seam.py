#!/usr/bin/env python3
"""The negative control for the save game half of the path-separator seam.

`GameState::getSaveDirectory()` spells its result `<user data>Save\\` on every platform, because
that spelling is what the retail build writes and what a save file's portable map path carries.
The save/load loop then hands that string to five different filesystem calls, and every one of
them has to cross the path seam. A call that reaches the C runtime untranslated does not fail: it
creates a file whose *name* contains a backslash, beside an empty `Save/` directory, and the
player is told `*** Game Saved ***` while the next process lists nothing. That is what the real
Mac measured (docs/porting/playability-probe.md), so it needs a control that fails on the unfixed
code, and this is it.

The headless harness's `savepath` mode runs the engine's own functions in the order the game
does: `findNextSaveFilename` (existence probe), `CreateDirectory` + `XferSave` (write),
`findNextSaveFilename` again (the probe must now see the file), `iterateSaveFiles` (the SELECT
GAME listing) and `doesSaveGameExist` + `XferLoad` (reopen and read back). This script then looks
at the disk: the file has to be `<user data>/Save/00000000.sav` on the host and nothing in the
user data directory may have a backslash in its name.

A second run puts a legacy `Save\\00000000.sav` file -- one written by a build before this seam --
into the user data directory first. The decision recorded in docs/porting/save-load-path-boundary.md
is that such a file is left alone and never listed, so the run must still name `00000000.sav`,
list exactly one save, and leave the legacy file untouched.

    python3 scripts/ci/check-save-path-seam.py --build-dir build/native-debug

No retail data is used. See docs/porting/save-load-path-boundary.md.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]

# The leaf GlobalData::BuildUserDataPathFromRegistry() appends under the user data root.
USER_DATA_LEAF = "Command and Conquer Generals Zero Hour Data"
LEGACY_NAME = "Save\\00000000.sav"
LEGACY_BYTES = b"legacy save written with a backslash in its name"


def write_registry(directory):
    # Zero Hour's archive file system reads Generals' install path out of the settings store and
    # asserts when it is unset; it is pointed at the fixture, which holds no archives.
    (directory / "Registry.ini").write_text(
        "[SOFTWARE\\Electronic Arts\\EA Games\\Generals]\n"
        "STRING_InstallPath={}\n".format(directory), encoding="latin-1")


def run_probe(probe, directory):
    return subprocess.run(
        [str(probe), "savepath"],
        capture_output=True, text=True, errors="replace", cwd=str(directory),
        env={"HOME": str(directory), "PATH": "/usr/bin:/bin",
             "CNC_SETTINGS_FILE": str(directory / "Registry.ini"),
             "CNC_USER_DATA": str(directory / "userdata"),
             "SIM_PROBE_LOCALFS": "win32"},
    )


def require(condition, message, failures):
    if not condition:
        failures.append(message)


def check_disk(user_data, failures, label):
    names = sorted(p.name for p in user_data.iterdir())
    stray = [n for n in names if "\\" in n and n != LEGACY_NAME]
    require(not stray,
            "{}: names with a backslash were created in the user data directory: {}".format(
                label, stray), failures)
    save = user_data / "Save" / "00000000.sav"
    require(save.is_file() and save.stat().st_size > 0,
            "{}: {} was not written (user data holds {})".format(label, save, names), failures)


def check_round_trip(probe, workdir, failures):
    """Fresh user data: name, write, renumber, list one, reopen -- and the file is in Save/."""
    directory = workdir / "fresh"
    directory.mkdir()
    write_registry(directory)
    result = run_probe(probe, directory)
    output = result.stdout + result.stderr

    require(result.returncode == 0,
            "fresh: the probe exited {}".format(result.returncode), failures)
    for expected in ("RESULT nextname before=00000000.sav",
                     "RESULT nextname after=00000001.sav",
                     "RESULT listed name=00000000.sav",
                     "RESULT listed count=1",
                     "RESULT cwd restored=yes",
                     "RESULT reopen exists=yes readback=yes",
                     "RESULT savepath ok=yes"):
        require(expected in output, "fresh: no line '{}'".format(expected), failures)

    user_data = directory / "userdata" / USER_DATA_LEAF
    require(user_data.is_dir(), "fresh: user data directory {} was not created".format(user_data),
            failures)
    if user_data.is_dir():
        check_disk(user_data, failures, "fresh")
    return output


def check_legacy_file_left_alone(probe, workdir, failures):
    """A pre-seam `Save\\00000000.sav` file is neither listed, renamed, nor in the way."""
    directory = workdir / "legacy"
    user_data = directory / "userdata" / USER_DATA_LEAF
    user_data.mkdir(parents=True)
    write_registry(directory)
    legacy = user_data / LEGACY_NAME
    legacy.write_bytes(LEGACY_BYTES)

    result = run_probe(probe, directory)
    output = result.stdout + result.stderr

    require(result.returncode == 0,
            "legacy: the probe exited {}".format(result.returncode), failures)
    for expected in ("RESULT nextname before=00000000.sav",
                     "RESULT listed count=1",
                     "RESULT savepath ok=yes"):
        require(expected in output, "legacy: no line '{}'".format(expected), failures)
    require(legacy.is_file() and legacy.read_bytes() == LEGACY_BYTES,
            "legacy: the pre-seam file was modified or removed; the decision is to leave it alone",
            failures)
    check_disk(user_data, failures, "legacy")
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

    workdir = pathlib.Path(tempfile.mkdtemp(prefix="save-path-seam-"))
    failures = []
    outputs = []
    try:
        outputs.append(check_round_trip(probe, workdir, failures))
        outputs.append(check_legacy_file_left_alone(probe, workdir, failures))
    finally:
        if args.keep:
            print("fixtures kept in {}".format(workdir))
        else:
            shutil.rmtree(workdir, ignore_errors=True)

    for output in outputs:
        for line in output.splitlines():
            if line.startswith("RESULT") or "ASSERTION" in line:
                print(line)

    if failures:
        print("\nFAIL: the save game path does not round trip through the path seam")
        for failure in failures:
            print("  - {}".format(failure))
        return 1

    print("\nOK: a save lands in Save/, is renumbered, listed and reopened by engine code")
    return 0


if __name__ == "__main__":
    sys.exit(main())

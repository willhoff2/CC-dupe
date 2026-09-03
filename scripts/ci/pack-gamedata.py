#!/usr/bin/env python3
"""Pack the trimmed game data the replay check needs, on macOS or Linux.

This is the portable twin of scripts/ci/pack-gamedata.ps1, which needs Windows PowerShell and
7-Zip. The archives it writes are the two `.github/workflows/check-replays.yml` downloads:
only what a headless replay run reads (the .big data files, the two DLLs the executable links
against, and the script files), no textures, audio or GUI data. None of it is redistributable,
which is why the workflow fetches it from a private bucket and checks its hash.

The retail installs are only ever read. Nothing is written outside --output-dir and a temporary
staging directory.

`--archives full` instead packs zerohour104_gamedata_full.7z: every .big from both install
roots, for the native-port probes, which read GUI, audio and texture data the replay gate never
touches. It is a separate object; the trimmed pair keeps the hashes the gate pins.

`--archives movies` and `--archives loose` cover what the installer puts down outside the .big
files: the .bik video, and the Data/ tree's cursors, WaterPlane textures and scripts.

Usage:
    python3 scripts/ci/pack-gamedata.py \\
        --generals   ~/zh-data/ZH_Generals \\
        --generalsmd ~/zh-data \\
        --output-dir gamedata-out

Then upload the archives and set the hash variables it prints. See
docs/porting/replay-check-gamedata.md.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# Kept in step with the file lists documented in check-replays.yml. The names are the retail
# spellings; lookup is case-insensitive, because installs and copies of them disagree about case
# and a case-sensitive filesystem would otherwise reject a perfectly good install.
GENERALS_FILES = [
    "BINKW32.DLL",
    "English.big",
    "INI.big",
    "Maps.big",
    "mss32.dll",
    "W3D.big",
    "Data/Scripts/MultiplayerScripts.scb",
    "Data/Scripts/SkirmishScripts.scb",
]

GENERALSMD_FILES = [
    "BINKW32.DLL",
    "INIZH.big",
    "MapsZH.big",
    "mss32.dll",
    "W3DZH.big",
    "Data/Scripts/MultiplayerScripts.scb",
    "Data/Scripts/Scripts.ini",
    "Data/Scripts/SkirmishScripts.scb",
]

# A Zero Hour install carries its own copies of these, and a Generals *data* tree (one without
# the executable) does not. See the note in resolve_file() before relaxing this list.
SHAREABLE_DLLS = {"binkw32.dll", "mss32.dll"}

TRIMMED_GENERALS_ARCHIVE = "generals108_gamedata_trimmed.7z"
TRIMMED_GENERALSMD_ARCHIVE = "zerohour104_gamedata_trimmed.7z"

# The GUI, audio and texture data the trimmed archives deliberately omit. The replay gate does
# not read any of it; the native-port probes do, and stall without it. Packed as one object
# separate from the trimmed pair, whose pinned hashes must keep matching.
FULL_ARCHIVE = "zerohour104_gamedata_full.7z"

# The .bik movies, which are not in any .big - they are loose files under these directories.
# Zero Hour's own tree holds the two menu backgrounds and the campaign cutscenes; the base
# Generals tree holds 29 more, and Zero Hour reads them.
MOVIES_ARCHIVE = "zerohour104_movies.7z"
MOVIE_SUFFIX = ".bik"

# Everything else the installer puts under Data/ loose: the .ANI mouse cursors (which no .big
# holds, so docs/porting/mouse-cursor-seam.md could not measure the retail row), the WaterPlane
# textures and the .scb/.ini scripts.
#
# .bik is excluded because the movies object already carries it. .big is excluded because the
# full object carries every .big in an install root, and the only one that lives under Data/ -
# Data/INI/INIZH.big - is the duplicate StdBIGFileSystem.cpp deliberately skips to avoid a CRC
# mismatch, so packing it would ship ~17.8 MiB the engine ignores.
LOOSE_ARCHIVE = "zerohour104_loose_data.7z"
LOOSE_EXCLUDED_SUFFIXES = {MOVIE_SUFFIX, ".big"}
CURSOR_SUFFIX = ".ani"

# -mx=9 for the trimmed pair, because the hashes CI pins were produced with it and repacking
# must keep reproducing them.
#
# -mx=5 for the full archive, measured rather than assumed. On a 216 MB slice of the real
# payload (one highly compressible .big, one incompressible, one very compressible), -mx=5
# packed it in 78s; -mx=9 had not finished the same slice after 510s. Extrapolated over the
# ~2.4 GB the full archive holds that is ~15 minutes against 96+, to land ~1% smaller.
#
# -mx=1 for the movies, because Bink is already-compressed video and does not deflate: measured
# on a retail cutscene, -mx=1 gives 1.018x and -mx=5 gives 1.020x. Anything above 1 buys nothing
# and costs time.
#
# -mx=9 for the loose data, which is a few MiB of small files: the whole object packs in about a
# second, so the level that compresses best is also free.
#
# The level is part of what the SHA256 covers, so changing any of these re-hashes that object.
TRIMMED_COMPRESSION_LEVEL = 9
FULL_COMPRESSION_LEVEL = 5
MOVIES_COMPRESSION_LEVEL = 1
LOOSE_COMPRESSION_LEVEL = 9


def find_seven_zip() -> str:
    for candidate in ("7z", "7zz", "7za"):
        found = shutil.which(candidate)
        if found:
            return found
    raise SystemExit(
        "No 7z binary found. Install it with `brew install sevenzip` on macOS or "
        "`apt-get install p7zip-full` on Debian/Ubuntu, then re-run."
    )


def resolve_file(root: Path, relative: str) -> Path | None:
    """Resolve `relative` under `root`, one component at a time, ignoring case."""
    current = root
    for component in relative.split("/"):
        direct = current / component
        if direct.exists():
            current = direct
            continue
        try:
            entries = list(current.iterdir())
        except (FileNotFoundError, NotADirectoryError, PermissionError):
            return None
        match = next((e for e in entries if e.name.lower() == component.lower()), None)
        if match is None:
            return None
        current = match
    return current


def stage_files(label: str, root: Path, files: list[str], stage: Path,
                dll_source: Path | None) -> None:
    if not root.is_dir():
        raise SystemExit(f"{label} path is not a directory: {root}")

    missing: list[str] = []
    borrowed: list[str] = []

    for relative in files:
        source = resolve_file(root, relative)

        if source is None and dll_source is not None and relative.lower() in SHAREABLE_DLLS:
            # A Generals tree that was copied for its data alone has no BINKW32.DLL or mss32.dll,
            # because those ship next to the executable. The archive still has to contain them:
            # the workflow's file list is what a future reader will compare against, and a
            # short archive is a silent difference. Inference, not verified: the Zero Hour
            # copies are interchangeable here, since the headless run under test is Zero Hour's
            # executable and only ever reads the Generals tree as data.
            source = resolve_file(dll_source, relative)
            if source is not None:
                borrowed.append(relative)

        if source is None or not source.is_file():
            missing.append(relative)
            continue

        target = stage / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)

    if missing:
        raise SystemExit(
            f"{label} install is missing these files, which the replay check needs:\n  "
            + "\n  ".join(missing)
        )

    for relative in borrowed:
        print(f"  note: {relative} was not in the {label} tree; used the copy from "
              f"{dll_source} instead.", flush=True)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest().upper()


def pack_stage_dir(label: str, stage: Path, archive: Path, seven_zip: str,
                   level: int) -> None:
    if archive.exists():
        archive.unlink()

    # Archive the staged tree's *contents*, not the staging directory itself, so entry paths
    # are exactly what the caller laid out. For the trimmed pair that is the install layout the
    # workflow's `7z x -o<install path>` expects; callers that stage under a per-root directory
    # (the full and movies objects) get that directory as a prefix, and unpack accordingly.
    #
    # Timestamps are not stored, which makes the archive - and therefore the SHA256 the workflow
    # pins - reproducible: repacking the same install twice would otherwise produce two different
    # hashes, because the staging directory entries carry the time they were created.
    result = subprocess.run(
        [seven_zip, "a", "-t7z", f"-mx={level}", "-mtm=off", "-mtc=off", "-mta=off",
         str(archive), "."],
        cwd=stage, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    if result.returncode != 0:
        sys.stdout.write(result.stdout)
        raise SystemExit(f"7z failed packing {label} (exit {result.returncode})")


def build_archive(label: str, root: Path, files: list[str], archive: Path, seven_zip: str,
                  dll_source: Path | None) -> str:
    print(f"Packing {label} data from {root}", flush=True)
    with tempfile.TemporaryDirectory(prefix="packgamedata-") as temp:
        stage = Path(temp)
        stage_files(label, root, files, stage, dll_source)
        pack_stage_dir(label, stage, archive, seven_zip, TRIMMED_COMPRESSION_LEVEL)

    return sha256(archive)


def stage_source(source: Path, target: Path) -> None:
    """Put `source` at `target`, hardlinking rather than copying where the filesystem allows it.

    The full archive stages several GB, and both the installs and the temporary directory are
    normally on the same volume. A copy is the fallback when they are not.
    """
    target.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(source, target)
    except OSError:
        shutil.copy2(source, target)


def collect_big_archives(label: str, root: Path) -> list[Path]:
    """Every .big directly in an install root, sorted, so the manifest is stable."""
    if not root.is_dir():
        raise SystemExit(f"{label} path is not a directory: {root}")
    archives = sorted((entry for entry in root.iterdir()
                       if entry.is_file() and entry.suffix.lower() == ".big"),
                      key=lambda entry: entry.name.lower())
    if not archives:
        raise SystemExit(f"{label} install has no .big files in {root}")
    return archives


def build_full_archive(generals: Path, generalsmd: Path, archive: Path,
                       seven_zip: str) -> tuple[str, list[tuple[str, Path, int]]]:
    """Pack every .big from both install roots, keeping the two trees apart.

    The roots are *not* flattened together: both ship a `Music.big`, and they are different
    files (Zero Hour's is a ~787 KB security stub, Generals' is ~159 MB of streamed music).
    Merging them would silently drop one, so each root gets its own top-level directory and
    the consumer extracts each to the install path it belongs to.
    """
    trees = [("Generals", generals), ("GeneralsMD", generalsmd)]
    manifest: list[tuple[str, Path, int]] = []

    with tempfile.TemporaryDirectory(prefix="packgamedata-full-") as temp:
        stage = Path(temp)
        for tree_name, root in trees:
            staged_tree = stage / tree_name
            staged_tree.mkdir()
            for source in collect_big_archives(tree_name, root):
                manifest.append((tree_name, source, source.stat().st_size))
                stage_source(source, staged_tree / source.name)

        staged_bytes = sum(size for _, _, size in manifest)
        print(f"Packing {len(manifest)} .big files ({staged_bytes / 1024**3:.2f} GiB) from "
              f"{generals} and {generalsmd}", flush=True)
        pack_stage_dir("full game data", stage, archive, seven_zip,
                       FULL_COMPRESSION_LEVEL)

    return sha256(archive), manifest


def collect_movies(label: str, root: Path, exclude: Path | None) -> list[Path]:
    """Every .bik under an install root, at whatever depth, sorted for a stable manifest.

    `exclude` is the other install root. It matters because a depot copied for its data often
    nests the Generals tree *inside* the Zero Hour one, and a recursive sweep of the Zero Hour
    root would then claim Generals' movies as well - packing them twice, under the wrong root.
    """
    if not root.is_dir():
        raise SystemExit(f"{label} path is not a directory: {root}")

    excluded_root = exclude.resolve() if exclude is not None else None
    found = [path for path in sorted(root.rglob("*"))
             if path.suffix.lower() == MOVIE_SUFFIX and path.is_file()
             and not (excluded_root is not None and excluded_root in path.resolve().parents)]

    if not found:
        raise SystemExit(f"{label} install has no {MOVIE_SUFFIX} files under {root}")
    return found


def build_movies_archive(generals: Path, generalsmd: Path, archive: Path,
                         seven_zip: str) -> tuple[str, list[tuple[str, Path, int]]]:
    """Pack every .bik from both install roots, preserving each one's path within its root.

    The movies are loose files, not entries in a .big, so they are absent from every other
    object this script produces.

    Layout matches the full archive: each install root gets its own top-level directory
    (`Generals/`, `GeneralsMD/`), and paths below it are relative to that root - so an entry
    reads `GeneralsMD/Data/English/Movies/MD_USA02_0.bik`. Extract to a staging directory and
    copy each top-level directory's contents into the install path it belongs to. Extracting
    straight over an install with `7z x -o<install path>` would bury them a level deep, where
    the game does not look.
    """
    trees = [("Generals", generals, None), ("GeneralsMD", generalsmd, generals)]
    manifest: list[tuple[str, Path, int]] = []

    with tempfile.TemporaryDirectory(prefix="packgamedata-movies-") as temp:
        stage = Path(temp)
        for tree_name, root, exclude in trees:
            for source in collect_movies(tree_name, root, exclude):
                relative = source.relative_to(root)
                manifest.append((tree_name, relative, source.stat().st_size))
                stage_source(source, stage / tree_name / relative)

        staged_bytes = sum(size for _, _, size in manifest)
        print(f"Packing {len(manifest)} {MOVIE_SUFFIX} files "
              f"({staged_bytes / 1024**2:.1f} MiB) from {generals} and {generalsmd}", flush=True)
        pack_stage_dir("movies", stage, archive, seven_zip, MOVIES_COMPRESSION_LEVEL)

    return sha256(archive), manifest


def collect_loose_data(label: str, root: Path) -> list[Path]:
    """Every file under an install root's Data/ that no other object carries, sorted.

    The sweep is confined to Data/, so the nesting trap collect_movies() guards against does not
    arise: a depot copy that puts the Generals tree inside the Zero Hour one puts it at the root
    (`zh-data/ZH_Generals`), not under `zh-data/Data`.
    """
    data_dir = resolve_file(root, "Data")
    if data_dir is None or not data_dir.is_dir():
        raise SystemExit(f"{label} install has no Data directory under {root}")

    found = [path for path in sorted(data_dir.rglob("*"))
             if path.is_file() and path.suffix.lower() not in LOOSE_EXCLUDED_SUFFIXES]

    if not found:
        raise SystemExit(f"{label} install has no loose data files under {data_dir}")

    # Data/Cursors is why this object exists, and a tree copied for its data can lack it. Without
    # this check such a tree packs its WaterPlane and Scripts, hashes cleanly, and silently omits
    # the cursors - a failure that would surface a long way from its cause, as a mouse seam still
    # showing the default arrow. The names are not enumerated: the SKUs differ (see the INIZH.big
    # note above), so a fixed list would reject installs that are perfectly good.
    cursors = resolve_file(root, "Data/Cursors")
    if cursors is None or not cursors.is_dir() or not any(
            path.suffix.lower() == CURSOR_SUFFIX for path in cursors.iterdir()):
        raise SystemExit(
            f"{label} install has no {CURSOR_SUFFIX} cursors: {root}/Data/Cursors is missing or "
            "holds none. They are installed loose, so a tree copied for its .big files alone will "
            "not have them - point --generals/--generalsmd at a full install."
        )
    return found


def build_loose_archive(generals: Path, generalsmd: Path, archive: Path,
                        seven_zip: str) -> tuple[str, list[tuple[str, Path, int]]]:
    """Pack the loose Data/ files from both install roots, preserving each path within its root.

    These are installed loose and are in no .big: the .ANI cursors the mouse seam reads with
    `LoadCursorFromFile("data\\cursors\\<Name>.ANI")`, the WaterPlane textures, and the .scb/.ini
    scripts. Without them a native install shows the platform's default arrow everywhere.

    Layout matches the full and movies archives: each install root gets its own top-level
    directory (`Generals/`, `GeneralsMD/`), and paths below it are relative to that root - so an
    entry reads `GeneralsMD/Data/Cursors/SCCAttack.ani`. Extract to a staging directory and copy
    each top-level directory's contents into the install path it belongs to.
    """
    trees = [("Generals", generals), ("GeneralsMD", generalsmd)]
    manifest: list[tuple[str, Path, int]] = []

    with tempfile.TemporaryDirectory(prefix="packgamedata-loose-") as temp:
        stage = Path(temp)
        for tree_name, root in trees:
            for source in collect_loose_data(tree_name, root):
                relative = source.relative_to(root)
                manifest.append((tree_name, relative, source.stat().st_size))
                stage_source(source, stage / tree_name / relative)

        staged_bytes = sum(size for _, _, size in manifest)
        print(f"Packing {len(manifest)} loose data files "
              f"({staged_bytes / 1024**2:.1f} MiB) from {generals} and {generalsmd}", flush=True)
        pack_stage_dir("loose data", stage, archive, seven_zip, LOOSE_COMPRESSION_LEVEL)

    return sha256(archive), manifest


def print_directory_summary(archive_name: str, manifest: list[tuple[str, Path, int]]) -> None:
    """One line per directory the archive drew from: file count and bytes."""
    by_directory: dict[str, list[int]] = {}
    for tree_name, relative, size in manifest:
        by_directory.setdefault(f"{tree_name}/{relative.parent}", []).append(size)

    print(f"\n{archive_name} holds, by directory:")
    for directory, sizes in sorted(by_directory.items()):
        print(f"  {directory:<40} {len(sizes):>3} files  {sum(sizes):>12,} bytes")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Pack the trimmed Generals 1.08 and Zero Hour 1.04 replay-check data.",
    )
    parser.add_argument("--generals", required=True, type=Path,
                        help="Generals 1.08 install or data tree (read only).")
    parser.add_argument("--generalsmd", required=True, type=Path,
                        help="Zero Hour 1.04 install (read only).")
    parser.add_argument("--output-dir", default=Path("gamedata-out"), type=Path,
                        help="Where to write the archives (default: gamedata-out).")
    parser.add_argument("--no-dll-fallback", action="store_true",
                        help="Fail instead of taking a missing BINKW32.DLL or mss32.dll for the "
                             "Generals archive from the Zero Hour install.")
    parser.add_argument("--archives", choices=("trimmed", "full", "movies", "loose", "all"),
                        default="trimmed",
                        help="Which objects to pack: the replay gate's trimmed pair (default), "
                             f"the {FULL_ARCHIVE} object holding every .big from both installs, "
                             f"the {MOVIES_ARCHIVE} object holding every loose .bik, the "
                             f"{LOOSE_ARCHIVE} object holding the rest of the loose Data/ files "
                             "(cursors, WaterPlane, scripts), or all four.")
    args = parser.parse_args()

    seven_zip = find_seven_zip()
    generals = args.generals.expanduser()
    generalsmd = args.generalsmd.expanduser()

    output_dir = args.output_dir.expanduser()
    output_dir.mkdir(parents=True, exist_ok=True)
    output_dir = output_dir.resolve()

    bucket = os.environ.get("GAMEDATA_S3_BASE_URI", "s3://your-bucket")
    uploads: list[Path] = []
    variables: list[tuple[str, str]] = []

    if args.archives in ("trimmed", "all"):
        generals_archive = output_dir / TRIMMED_GENERALS_ARCHIVE
        generalsmd_archive = output_dir / TRIMMED_GENERALSMD_ARCHIVE
        generals_hash = build_archive(
            "Generals", generals, GENERALS_FILES, generals_archive, seven_zip,
            None if args.no_dll_fallback else generalsmd,
        )
        generalsmd_hash = build_archive(
            "Zero Hour", generalsmd, GENERALSMD_FILES, generalsmd_archive, seven_zip, None,
        )
        uploads += [generals_archive, generalsmd_archive]
        variables += [("GAMEDATA_GENERALS_SHA256", generals_hash),
                      ("GAMEDATA_GENERALSMD_SHA256", generalsmd_hash)]

    if args.archives in ("full", "all"):
        full_archive = output_dir / FULL_ARCHIVE
        full_hash, manifest = build_full_archive(generals, generalsmd, full_archive, seven_zip)
        uploads.append(full_archive)
        variables.append(("GAMEDATA_FULL_SHA256", full_hash))

        print(f"\n{FULL_ARCHIVE} holds:")
        for tree_name, source, size in manifest:
            print(f"  {tree_name}/{source.name:<24} {size:>12,} bytes")

    if args.archives in ("movies", "all"):
        movies_archive = output_dir / MOVIES_ARCHIVE
        movies_hash, movies_manifest = build_movies_archive(
            generals, generalsmd, movies_archive, seven_zip)
        uploads.append(movies_archive)
        variables.append(("GAMEDATA_MOVIES_SHA256", movies_hash))
        print_directory_summary(MOVIES_ARCHIVE, movies_manifest)

    if args.archives in ("loose", "all"):
        loose_archive = output_dir / LOOSE_ARCHIVE
        loose_hash, loose_manifest = build_loose_archive(
            generals, generalsmd, loose_archive, seven_zip)
        uploads.append(loose_archive)
        variables.append(("GAMEDATA_LOOSE_SHA256", loose_hash))
        print_directory_summary(LOOSE_ARCHIVE, loose_manifest)

    print(f"\nArchives written to {output_dir}")
    for archive in uploads:
        print(f"  {archive.name:<32} {archive.stat().st_size:>13,} bytes")
    print("\nUpload to your bucket, for example:")
    for archive in uploads:
        print(f"  aws s3 cp {archive} {bucket}/")
    print("\nthen set these repository variables "
          "(Settings -> Secrets and variables -> Actions):")
    print(f"  {'GAMEDATA_S3_BASE_URI':<27} = {bucket}")
    for name, value in variables:
        print(f"  {name:<27} = {value}")
    print("\nand the bucket secrets described in docs/porting/replay-check-gamedata.md. The "
          "replay gate re-enables itself once R2_ACCESS_KEY_ID and R2_SECRET_ACCESS_KEY exist.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Pack the trimmed game data the replay check needs, on macOS or Linux.

This is the portable twin of scripts/ci/pack-gamedata.ps1, which needs Windows PowerShell and
7-Zip. The archives it writes are the two `.github/workflows/check-replays.yml` downloads:
only what a headless replay run reads (the .big data files, the two DLLs the executable links
against, and the script files), no textures, audio or GUI data. None of it is redistributable,
which is why the workflow fetches it from a private bucket and checks its hash.

The retail installs are only ever read. Nothing is written outside --output-dir and a temporary
staging directory.

Usage:
    python3 scripts/ci/pack-gamedata.py \\
        --generals   ~/zh-data/ZH_Generals \\
        --generalsmd ~/zh-data \\
        --output-dir gamedata-out

Then upload both archives and set the two hash variables it prints. See
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


def build_archive(label: str, root: Path, files: list[str], archive: Path, seven_zip: str,
                  dll_source: Path | None) -> str:
    print(f"Packing {label} data from {root}", flush=True)
    with tempfile.TemporaryDirectory(prefix="packgamedata-") as temp:
        stage = Path(temp)
        stage_files(label, root, files, stage, dll_source)

        if archive.exists():
            archive.unlink()

        # Archive the staged tree's *contents*, so paths inside are relative exactly as the
        # workflow's `7z x -o<install path>` expects. Timestamps are not stored, which makes
        # the archive - and therefore the SHA256 the workflow pins - reproducible: repacking
        # the same install twice would otherwise produce two different hashes, because the
        # staging directory entries carry the time they were created.
        result = subprocess.run(
            [seven_zip, "a", "-t7z", "-mx=9", "-mtm=off", "-mtc=off", "-mta=off",
             str(archive), "."],
            cwd=stage, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
        )
        if result.returncode != 0:
            sys.stdout.write(result.stdout)
            raise SystemExit(f"7z failed packing {label} (exit {result.returncode})")

    return sha256(archive)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Pack the trimmed Generals 1.08 and Zero Hour 1.04 replay-check data.",
    )
    parser.add_argument("--generals", required=True, type=Path,
                        help="Generals 1.08 install or data tree (read only).")
    parser.add_argument("--generalsmd", required=True, type=Path,
                        help="Zero Hour 1.04 install (read only).")
    parser.add_argument("--output-dir", default=Path("gamedata-out"), type=Path,
                        help="Where to write the two archives (default: gamedata-out).")
    parser.add_argument("--no-dll-fallback", action="store_true",
                        help="Fail instead of taking a missing BINKW32.DLL or mss32.dll for the "
                             "Generals archive from the Zero Hour install.")
    args = parser.parse_args()

    seven_zip = find_seven_zip()
    generals = args.generals.expanduser()
    generalsmd = args.generalsmd.expanduser()

    output_dir = args.output_dir.expanduser()
    output_dir.mkdir(parents=True, exist_ok=True)
    output_dir = output_dir.resolve()

    generals_archive = output_dir / "generals108_gamedata_trimmed.7z"
    generalsmd_archive = output_dir / "zerohour104_gamedata_trimmed.7z"

    generals_hash = build_archive(
        "Generals", generals, GENERALS_FILES, generals_archive, seven_zip,
        None if args.no_dll_fallback else generalsmd,
    )
    generalsmd_hash = build_archive(
        "Zero Hour", generalsmd, GENERALSMD_FILES, generalsmd_archive, seven_zip, None,
    )

    print(f"\nArchives written to {output_dir}")
    print(f"  {generals_archive.name}  {generals_hash}")
    print(f"  {generalsmd_archive.name}  {generalsmd_hash}")
    print("\nUpload both to your bucket, for example:")
    bucket = os.environ.get("GAMEDATA_S3_BASE_URI", "s3://your-bucket")
    print(f"  aws s3 cp {generals_archive} {bucket}/")
    print(f"  aws s3 cp {generalsmd_archive} {bucket}/")
    print("\nthen set these repository variables "
          "(Settings -> Secrets and variables -> Actions):")
    print(f"  GAMEDATA_S3_BASE_URI        = {bucket}")
    print(f"  GAMEDATA_GENERALS_SHA256    = {generals_hash}")
    print(f"  GAMEDATA_GENERALSMD_SHA256  = {generalsmd_hash}")
    print("\nand the bucket secrets described in docs/porting/replay-check-gamedata.md. The "
          "replay gate re-enables itself once R2_ACCESS_KEY_ID and R2_SECRET_ACCESS_KEY exist.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

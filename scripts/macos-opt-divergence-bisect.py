#!/usr/bin/env python3
"""Find which optimised archive (then which object file) makes a replay desync.

    python3 scripts/macos-opt-divergence-bisect.py --link-cmd link.json --o0 BUILD_O0 --o2 BUILD_O2 \
        --run-dir RUN --user-data-src UD --work WORK archives
    python3 scripts/macos-opt-divergence-bisect.py ... objects --archive libgeneralsmd_code_gameengine.a
    python3 scripts/macos-opt-divergence-bisect.py ... variants --archive libgeneralsmd_code_gameengine.a \
        --member Locomotor.cpp.o --flags="-O2 -fno-strict-aliasing" --flags="-O2 -fmath-errno"

`link.json` is the strict-link command `scripts/native-build.py` ran for BUILD_O2 (a JSON list; capture
it by wrapping subprocess.run around a re-run of native-build.py on BUILD_O2, which relinks only).
`archives` links one archive from BUILD_O2 with every other archive from BUILD_O0, for each archive
in turn. `objects` takes one archive and swaps halves of its members between the two builds, so a
bisection ends at the member whose optimised code changes the simulation. `variants` recompiles one
member from its -O0 command plus each --flags set, with the rest of the build -O0, to find which
optimisation is responsible. Each mixed binary plays
the replay headless until --until-frame and reports whether a `CRC Mismatch` line appeared before
it. The decisive checkpoint must lie before --until-frame; docs/porting/late-game-frame-cost.md
section 10 used frame 700 of heavy-before-2, so --until-frame 800.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

REPLAY_NAME = "00000000.rep"


def link(link_cmd, o2_dir, archive_for, out):
    """Relink with each archive taken from the build archive_for(name) names."""
    command = []
    for arg in link_cmd:
        path = Path(arg)
        if arg.endswith(".a") and path.parent == o2_dir:
            command.append(str(archive_for(path.name) / path.name))
        elif path.name == "native_strict_link" and arg.startswith("/"):
            command.append(str(out))
        else:
            command.append(arg)
    subprocess.run(command, check=True, capture_output=True, text=True)
    subprocess.run(["codesign", "-f", "-s", "-", str(out)], check=True, capture_output=True)


def plays_in_sync(binary, run_dir, user_data_src, work, until_frame, timeout_s):
    """True when the replay reaches until_frame with no CRC mismatch; False when it mismatches."""
    user_data = work / "userdata"
    if user_data.exists():
        shutil.rmtree(user_data)
    shutil.copytree(user_data_src, user_data)
    logic_log = work / "logic.csv"
    # A previous run's log would read as progress before this run truncates it.
    logic_log.unlink(missing_ok=True)
    env = dict(os.environ, CNC_USER_DATA=str(user_data), ZH_LOGIC_FRAME_LOG=str(logic_log))
    with open(work / "stdout.log", "w") as log:
        proc = subprocess.Popen([str(binary), "-headless", "-replay", REPLAY_NAME], cwd=run_dir, env=env,
                                stdout=log, stderr=subprocess.STDOUT)
    started = time.time()
    reached = 0
    while proc.poll() is None and time.time() - started < timeout_s:
        time.sleep(0.5)
        if logic_log.is_file():
            lines = logic_log.read_text().splitlines()
            if len(lines) > 1 and lines[-1].split(",")[0].isdigit():
                reached = int(lines[-1].split(",")[0])
        if reached >= until_frame:
            break
    if proc.poll() is None:
        proc.kill()
        proc.wait()
    mismatch = "CRC Mismatch" in (work / "stdout.log").read_text(errors="replace")
    if not mismatch and reached < until_frame:
        raise SystemExit(f"{binary}: reached only frame {reached} in {timeout_s}s without a verdict")
    return not mismatch


def run_variants(args, o0_dir, mixed_dir, verdict):
    """Rebuild one archive member from its -O0 compile command plus each set of extra flags."""
    library_dir = Path(args.archive).stem.removeprefix("lib") + ".dir/"
    entries = [e for e in json.loads((o0_dir / "compile_commands.json").read_text())
               if library_dir in e["command"] and e["command"].split(" -o ")[1].split()[0].endswith(
                   "/" + args.member)]
    if len(entries) != 1:
        raise SystemExit(f"{len(entries)} compile commands produce {args.member} in {args.archive}")
    entry = entries[0]
    for flags in args.flags:
        target = mixed_dir / args.member
        command = entry["command"].split(" -o ")
        command = f"{command[0]} {flags} -o {target} " + command[1].split(" ", 1)[1]
        subprocess.run(command, shell=True, cwd=entry["directory"], check=True)
        archive = mixed_dir / args.archive
        shutil.copy(o0_dir / args.archive, archive)
        subprocess.run(["ar", "rs", str(archive), str(target)], check=True, capture_output=True)
        verdict(f"{args.member} with [{flags}], rest -O0",
                lambda name: mixed_dir if name == args.archive else o0_dir)
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--link-cmd", required=True)
    parser.add_argument("--o0", required=True)
    parser.add_argument("--o2", required=True)
    parser.add_argument("--run-dir", required=True)
    parser.add_argument("--user-data-src", required=True)
    parser.add_argument("--work", required=True)
    parser.add_argument("--until-frame", type=int, default=800)
    parser.add_argument("--timeout", type=float, default=120.0)
    sub = parser.add_subparsers(dest="mode", required=True)
    sub.add_parser("archives")
    objects = sub.add_parser("objects")
    objects.add_argument("--archive", required=True)
    variants = sub.add_parser("variants", help="recompile one member with extra flags over the -O0 build")
    variants.add_argument("--archive", required=True)
    variants.add_argument("--member", required=True, help="e.g. Locomotor.cpp.o")
    variants.add_argument("--flags", action="append", required=True,
                          help="flags appended to the member's -O0 compile command; repeatable")
    args = parser.parse_args()

    link_cmd = json.loads(Path(args.link_cmd).read_text())
    o0_dir, o2_dir = Path(args.o0).resolve(), Path(args.o2).resolve()
    work = Path(args.work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    # The game resolves its data next to its own executable, so the mixed binary lives in the run dir.
    binary = Path(args.run_dir).resolve() / "zh-bisect"

    def verdict(label, archive_for):
        link(link_cmd, o2_dir, archive_for, binary)
        in_sync = plays_in_sync(binary, args.run_dir, Path(args.user_data_src), work, args.until_frame,
                                args.timeout)
        print(f"{label}: {'in sync' if in_sync else 'CRC MISMATCH'}", flush=True)
        return in_sync

    archives = [Path(a).name for a in link_cmd if a.endswith(".a")]
    if args.mode == "archives":
        verdict("all -O0", lambda name: o0_dir)
        verdict("all -O2", lambda name: o2_dir)
        for archive in archives:
            verdict(f"only {archive} -O2", lambda name, chosen=archive: o2_dir if name == chosen else o0_dir)
        return 0

    mixed_dir = work / "mixed"
    mixed_dir.mkdir(exist_ok=True)
    if args.mode == "variants":
        return run_variants(args, o0_dir, mixed_dir, verdict)

    # Members of one archive: optimised for the candidates, unoptimised for the rest.
    members = subprocess.run(["ar", "t", str(o2_dir / args.archive)], check=True, capture_output=True,
                             text=True).stdout.split()
    members = [m for m in members if m.endswith(".o")]
    extract_o0, extract_o2 = work / "x-o0", work / "x-o2"
    for target, source in ((extract_o0, o0_dir), (extract_o2, o2_dir)):
        shutil.rmtree(target, ignore_errors=True)
        target.mkdir()
        subprocess.run(["ar", "x", str(source / args.archive)], cwd=target, check=True)

    def with_optimised(optimised):
        archive = mixed_dir / args.archive
        archive.unlink(missing_ok=True)
        paths = [str((extract_o2 if m in optimised else extract_o0) / m) for m in members]
        subprocess.run(["ar", "rcs", str(archive), *paths], check=True)
        return verdict(f"{len(optimised)} of {len(members)} members -O2",
                       lambda name: mixed_dir if name == args.archive else o0_dir)

    candidates = list(members)
    if with_optimised(set(candidates)):
        print("this archive alone does not desync; nothing to bisect")
        return 1
    while len(candidates) > 1:
        half = candidates[: len(candidates) // 2]
        candidates = half if not with_optimised(set(half)) else candidates[len(candidates) // 2:]
        print(f"  {len(candidates)} candidate members left", flush=True)
    print(f"culprit member: {candidates[0]}")
    in_sync_alone = with_optimised(set(candidates))
    print(f"  confirmed alone: {'no (needs another member)' if in_sync_alone else 'yes'}")
    everything_else = set(members) - set(candidates)
    print(f"  rest of the archive -O2 without it: "
          f"{'in sync' if with_optimised(everything_else) else 'CRC MISMATCH (a second culprit)'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

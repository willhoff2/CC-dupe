#!/usr/bin/env python3
"""Replay a Zero Hour recording headless under LLDB and check every mob nexus's spawn count.

A mob nexus (`SpawnBehavior` with `AggregateHealth = Yes`) is `ImmortalBody` and leaves the world
only when `m_spawnCount` reaches 0. Each time a member's removal reaches
`SpawnBehavior::onSpawnDeath`, this logs the nexus's count, its listed ids, and how many of those
ids still resolve to an object. Count running ahead of live members means a member left without
being subtracted, and that nexus can never die. See docs/porting/mob-nexus-orphaned-spawn.md.

Usage (needs a -g build copied into the run dir; attach is refused with developer mode off):
    macos-mob-nexus-probe.py --binary ~/devin-work/run0911/run/zh-g \\
        --run-dir ~/devin-work/run0911/run --replay mob.rep --out mob-nexus.json

Exits 1 if any nexus's count ever exceeded its live members.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

LLDB_COMMANDS = """\
settings set target.env-vars CNC_CRC_DIAG=/dev/null CNC_CRC_DIAG_CONTINUE=1
breakpoint set -n SpawnBehavior::onSpawnDeath
breakpoint command add -s python 1
    def read(expr):
        return frame.EvaluateExpression(expr).GetValueAsSigned()
    if frame.EvaluateExpression("this->m_aggregateHealth").GetValueAsUnsigned():
        live_expr = ("({ int n = 0; for (auto it = this->m_spawnIDs.begin(); "
                     "it != this->m_spawnIDs.end(); ++it) "
                     "if (TheGameLogic->findObjectByID(*it)) ++n; n; })")
        print("MOBDEATH frame=%d nexus=%d dead=%d count=%d listed=%d live=%d" % (
            read("(int)TheGameLogic->getFrame()"),
            read("(int)this->getObject()->getID()"),
            frame.FindVariable("deadSpawn").GetValueAsSigned(),
            read("this->m_spawnCount"),
            read("(int)this->m_spawnIDs.size()"),
            read(live_expr)), flush=True)
    return False
DONE
breakpoint set -n SpawnBehavior::onDelete
breakpoint command add -s python 2
    if frame.EvaluateExpression("this->m_aggregateHealth").GetValueAsUnsigned():
        print("NEXUSGONE frame=%d nexus=%d" % (
            frame.EvaluateExpression("(int)TheGameLogic->getFrame()").GetValueAsSigned(),
            frame.EvaluateExpression("(int)this->getObject()->getID()").GetValueAsSigned()),
            flush=True)
    return False
DONE
run
quit
"""

EVENT = re.compile(r"^(MOBDEATH|NEXUSGONE) (.*)$")


def parse_event(line):
    match = EVENT.match(line)
    if not match:
        return None
    fields = dict(pair.split("=") for pair in match.group(2).split())
    return {"kind": match.group(1), **{key: int(value) for key, value in fields.items()}}


def summarize(events):
    nexuses = {}
    for event in events:
        nexus = nexuses.setdefault(event["nexus"], {"deaths": 0, "drift_events": 0,
                                                    "first_drift_frame": None, "gone_frame": None})
        if event["kind"] == "NEXUSGONE":
            nexus["gone_frame"] = event["frame"]
            continue
        nexus["deaths"] += 1
        if event["count"] > event["live"]:
            nexus["drift_events"] += 1
            if nexus["first_drift_frame"] is None:
                nexus["first_drift_frame"] = event["frame"]
    return nexuses


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--binary", required=True,
                        help="engine built with -g, copied into --run-dir (elsewhere it exits 1)")
    parser.add_argument("--run-dir", required=True, help="directory the game runs from")
    parser.add_argument("--replay", required=True, help="replay name inside the user Replays dir")
    parser.add_argument("--out", help="write events and per-nexus summary as JSON")
    parser.add_argument("--lldb-log", help="also write LLDB's raw output here")
    args = parser.parse_args()

    with tempfile.NamedTemporaryFile("w", suffix=".lldb", delete=False) as command_file:
        command_file.write(LLDB_COMMANDS)
    lldb = subprocess.Popen(
        ["arch", "-arm64", "lldb", "-b", "-s", command_file.name, "--",
         os.path.abspath(os.path.expanduser(args.binary)), "-headless", "-replay", args.replay],
        cwd=os.path.expanduser(args.run_dir), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True)
    log = open(args.lldb_log, "w") if args.lldb_log else None
    events = []
    for line in lldb.stdout:
        if log:
            log.write(line)
            log.flush()
        event = parse_event(line.rstrip("\n"))
        if event:
            events.append(event)
    lldb.wait()
    os.unlink(command_file.name)

    nexuses = summarize(events)
    for nexus_id, nexus in sorted(nexuses.items()):
        print(f"nexus {nexus_id}: deaths={nexus['deaths']} drift_events={nexus['drift_events']} "
              f"first_drift_frame={nexus['first_drift_frame']} gone_frame={nexus['gone_frame']}")
    if args.out:
        with open(args.out, "w") as out:
            json.dump({"events": events, "nexuses": nexuses}, out, indent=1)
    if not events:
        sys.exit("no mob events recorded; is the binary built with -g, and does the replay run?")
    drifted = [nexus_id for nexus_id, nexus in nexuses.items() if nexus["drift_events"]]
    print(f"{len(nexuses)} nexuses, {len(drifted)} with count ahead of live members")
    return 1 if drifted else 0


if __name__ == "__main__":
    sys.exit(main())

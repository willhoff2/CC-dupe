#!/usr/bin/env python3
"""Sample combat state out of a running Zero Hour process on macOS through LLDB.

Companion to `macos-playability-probe.py` (process health) and `macos-input-drive.py` (real
input). This one answers the slice-6 questions from engine state: does anything acquire a
target, fire, spawn a projectile, take damage, die and leave the object list, and did the
order come from the player or from the AI?

Two evidence sources, both read from the live process and never from pixels:

* an object table, rebuilt every sample by walking `TheGameLogic->m_objList` inside one
  JIT-compiled expression (id, owner, template, health, AI state, victim, current weapon's
  status/ammo/last-fire frame, position, team AI state);
* hit counters on the combat-path functions (`Weapon::privateFireWeapon`,
  `ActiveBody::attemptDamage`, projectile launch/detonate, `Object::onDie`,
  `GameLogic::destroyObject`, the `AIUpdateInterface::private*` order entry points), with the
  first N events of each kind decoded (owner/victim ids, damage type and amount,
  `CommandSourceType`).

Usage:
    macos-combat-probe.py --pid PID --binary build/.../native_strict_link \\
        --minutes 6 --interval 10 --out /path/combat.json

The process runs between samples; the breakpoints auto-continue. Expect the game to run slower
than real time while `attemptDamage` and `privateFireWeapon` breakpoints are live in a fight —
the logic frame counter in every sample says how far the simulation actually advanced.
"""

import argparse
import json
import os
import re
import struct
import subprocess
import sys
import time

XCODE_PYTHON = ("/Applications/Xcode.app/Contents/Developer/Library/Frameworks/"
                "Python3.framework/Versions/Current/bin/python3")

MAX_OBJECTS = 2048
RECORD_INTS = 40
NAME_INTS = 8
NAME_BYTES = NAME_INTS * 4
DECODED_EVENTS_PER_ENTRY = 24

# The record layout the JIT expression writes; the Python side unpacks by these offsets.
F_ID, F_PLAYER, F_HEALTH, F_MAX_HEALTH, F_AI_STATE, F_VICTIM, F_WEAPON_STATUS, \
    F_LAST_FIRE, F_AMMO, F_NEXT_SHOT, F_PRIVATE_STATUS, F_KINDOF_LO, F_POS_X, F_POS_Y, \
    F_POS_Z, F_WEAPON_SLOT, F_TEMPLATE, F_TEAM_STATE = list(range(16)) + [16, 24]

# GeneralsMD/Code/GameEngine/Include/Common/KindOf.h, bit index in the first 64-bit word.
KINDOF_PROJECTILE_BIT = 27

# Names resolved from GeneralsMD/Code/GameEngine/Include/GameLogic/AIStateMachine.h at run time
# so the numeric ids can never drift from the enum.
REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
AI_STATE_HEADER = os.path.join(REPO, "GeneralsMD", "Code", "GameEngine", "Include",
                               "GameLogic", "AIStateMachine.h")


def ai_state_names():
    with open(AI_STATE_HEADER) as handle:
        body = handle.read().split("enum AIStateType", 1)[1].split("};", 1)[0]
    names = {}
    for line in body.splitlines():
        match = re.match(r"\s*(AI_[A-Z_0-9]+)\s*,?", line)
        if match:
            names[len(names)] = match.group(1)
    return names


def damage_line():
    with open(ACTIVE_BODY_CPP) as handle:
        for number, line in enumerate(handle, 1):
            if line.strip() == DAMAGE_ANCHOR:
                return number
    raise RuntimeError("damage anchor not found in %s" % ACTIVE_BODY_CPP)


AI_STATE_NAMES = ai_state_names()
COMMAND_SOURCE_NAMES = {0: "CMD_FROM_PLAYER", 1: "CMD_FROM_SCRIPT", 2: "CMD_FROM_AI",
                        3: "CMD_FROM_DOZER"}

# Function -> (register carrying CommandSourceType, kind). arm64 AAPCS: this=x0, args x1..
ORDER_ENTRIES = {
    "AIUpdateInterface::privateAttackObject": ("x3", "attack_object"),
    "AIUpdateInterface::privateForceAttackObject": ("x3", "force_attack_object"),
    "AIUpdateInterface::privateAttackPosition": ("x3", "attack_position"),
    "AIUpdateInterface::privateAttackMoveToPosition": ("x3", "attack_move"),
    "AIUpdateInterface::privateHunt": ("x1", "hunt"),
    "AIUpdateInterface::privateMoveToPosition": ("x2", "move_to"),
    "AIUpdateInterface::privateGuardPosition": ("x3", "guard_position"),
}
ACTIVE_BODY_CPP = os.path.join(
    REPO, "GeneralsMD/Code/GameEngine/Source/GameLogic/Object/Body/ActiveBody.cpp")
# Damage is counted at the statement after the indestructible/already-dead early returns so
# `Object::kill()` re-runs on a dead parachute (ParachuteContain::update) do not count.
DAMAGE_ANCHOR = "Object *damager = TheGameLogic->findObjectByID( damageInfo->in.m_sourceID );"
# Hit every frame by AI target scoring and dozer repair; disabled once their decode budget is
# spent so the target keeps running at a usable rate. Hit counts up to that frame are kept.
HIGH_FREQUENCY_ENTRIES = {"ArmorTemplate::adjustDamage", "ActiveBody::internalChangeHealth"}
COUNTER_ENTRIES = [
    "Weapon::privateFireWeapon",
    "SlowDeathBehavior::beginSlowDeath",
    "SlowDeathBehavior::update",
    "DestroyDie::onDie",
    "DumbProjectileBehavior::projectileFireAtObjectOrPosition",
    "DumbProjectileBehavior::detonate",
    "MissileAIUpdate::projectileFireAtObjectOrPosition",
    "MissileAIUpdate::detonate",
    "ActiveBody::attemptDamage",
    "ActiveBody::internalChangeHealth",
    "ArmorTemplate::adjustDamage",
    "Object::onDie",
    "GameLogic::destroyObject",
    "ScoreKeeper::addObjectLost",
    "ScoreKeeper::addObjectDestroyed",
    "OpenContain::addToContain",
    "OpenContain::removeFromContain",
]

OBJECT_TABLE = """
int* probeOut = (int*)$combat_buf;
int probeN = 0;
for (Object* o = ((W3DGameLogic*)TheGameLogic)->m_objList; o != 0 && probeN < %(max)d;
     o = o->m_next) {
    int* r = probeOut + 1 + probeN * %(stride)d;
    for (int k = 0; k < %(stride)d; k++) r[k] = 0;
    r[0] = (int)o->m_id;
    Player* p = o->getControllingPlayer();
    r[1] = p ? (int)p->m_playerIndex : -1;
    float h = -1.0f; float mh = -1.0f;
    if (o->m_body) { h = o->m_body->getHealth(); mh = o->m_body->getMaxHealth(); }
    r[2] = *(int*)&h; r[3] = *(int*)&mh;
    r[4] = -2; r[5] = -1;
    if (o->m_ai) {
        r[4] = (o->m_ai->m_stateMachine && o->m_ai->m_stateMachine->m_currentState)
            ? (int)o->m_ai->m_stateMachine->m_currentState->m_ID : -1;
        r[5] = (int)o->m_ai->m_currentVictimID;
    }
    int slot = (int)o->m_weaponSet.m_curWeapon;
    r[15] = slot;
    Weapon* w = (slot >= 0 && slot < 3) ? o->m_weaponSet.m_weapons[slot] : (Weapon*)0;
    r[6] = w ? (int)w->m_status : -1;
    r[7] = w ? (int)w->m_lastFireFrame : -1;
    r[8] = w ? (int)w->m_ammoInClip : -1;
    r[9] = w ? (int)w->m_whenWeCanFireAgain : -1;
    r[10] = (int)o->m_privateStatus;
    const ThingTemplate* t = o->getTemplate();
    r[11] = t ? (int)((*(unsigned long long*)&t->m_kindof) >> 27 & 1) : 0;
    float px = o->m_cachedPos.x, py = o->m_cachedPos.y, pz = o->m_cachedPos.z;
    r[12] = *(int*)&px; r[13] = *(int*)&py; r[14] = *(int*)&pz;
    const char* name = t ? t->m_nameString.str() : "";
    char* dst = (char*)(r + 16);
    for (int k = 0; k < %(name_bytes)d - 1 && name[k]; k++) dst[k] = name[k];
    const char* st = o->m_team ? o->m_team->m_state.str() : "";
    char* dst2 = (char*)(r + 24);
    for (int k = 0; k < %(name_bytes)d - 1 && st[k]; k++) dst2[k] = st[k];
    probeN++;
}
probeOut[0] = probeN;
probeN;
""" % {"max": MAX_OBJECTS, "stride": RECORD_INTS, "name_bytes": NAME_BYTES}

SCORE = """
int probeScore[8];
probeScore[0] = ThePlayerList->getNthPlayer(%(player)d)->m_scoreKeeper.m_totalUnitsLost;
probeScore[1] = ThePlayerList->getNthPlayer(%(player)d)->m_scoreKeeper.m_totalBuildingsLost;
probeScore[0] * 65536 + probeScore[1];
"""


def require_lldb():
    """Re-exec under Xcode's interpreter, the only one carrying the `lldb` module."""
    try:
        import lldb  # noqa: F401
        return
    except ImportError:
        pass
    if os.environ.get("COMBAT_PROBE_REEXEC"):
        sys.exit("no lldb python module, even under %s" % sys.executable)
    lldb_python_path = subprocess.run(
        ["lldb", "-P"], capture_output=True, text=True, check=True).stdout.strip()
    environment = dict(os.environ, PYTHONPATH=lldb_python_path, COMBAT_PROBE_REEXEC="1")
    interpreter = XCODE_PYTHON if os.path.exists(XCODE_PYTHON) else sys.executable
    os.execve(interpreter, [interpreter, os.path.abspath(__file__)] + sys.argv[1:], environment)


def float_bits(value):
    return struct.unpack("<f", struct.pack("<i", value))[0]


def c_string(chunk):
    return chunk.split(b"\0", 1)[0].decode("ascii", "replace")


class Probe:
    """A long-lived LLDB attachment in asynchronous mode.

    Every combat-path breakpoint auto-continues and is read through `GetHitCount()`. A few
    of them are additionally made to stop until `DECODED_EVENTS_PER_ENTRY` events have been
    decoded from their argument registers, then flipped back to auto-continue so a long fight
    does not stall on the debugger.
    """

    def __init__(self, pid, binary):
        import lldb
        self.lldb = lldb
        self.debugger = lldb.SBDebugger.Create()
        self.debugger.SetAsync(False)
        self.listener = self.debugger.GetListener()
        self.target = self.debugger.CreateTarget(str(binary))
        error = lldb.SBError()
        self.process = self.target.AttachToProcessWithID(self.listener, pid, error)
        if not self.process.IsValid() or error.Fail():
            raise RuntimeError("attach to %d failed: %s" % (pid, error))
        self.breakpoints = {}
        self.decoded = []
        self.decoded_per_entry = {}
        self.disabled_at = {}
        self.refresh_frame()
        self.buffer = self.evaluate(
            "void* $combat_buf = (void*)malloc(%d); (unsigned long long)$combat_buf"
            % ((1 + MAX_OBJECTS * RECORD_INTS) * 4)).GetValueAsUnsigned()
        if not self.buffer:
            raise RuntimeError("could not allocate the object table in the target")

    def refresh_frame(self):
        # Thread 0 is the game thread. Expressions run on it alone (no try-all-threads), so
        # the simulation cannot advance between the frame read and the object table read.
        self.frame = self.process.GetThreadAtIndex(0).GetFrameAtIndex(0)

    def evaluate(self, expression):
        options = self.lldb.SBExpressionOptions()
        options.SetIgnoreBreakpoints(True)
        options.SetTryAllThreads(False)
        options.SetTimeoutInMicroSeconds(30 * 1000 * 1000)
        result = self.frame.EvaluateExpression(expression, options)
        if result.GetError().Fail() and "is running" in str(result.GetError().GetCString()):
            # An auto-continue breakpoint can restart the target between our interrupt and the
            # first expression; interrupt again and retry once.
            self.process.Stop()
            self.wait_for_stop(30)
            self.refresh_frame()
            result = self.frame.EvaluateExpression(expression, options)
        if result.GetError().Fail():
            raise RuntimeError("%s\n  in: %s" % (result.GetError().GetCString(),
                                                 expression.strip()[:120]))
        return result

    def integer(self, expression):
        try:
            return self.evaluate(expression).GetValueAsSigned()
        except RuntimeError:
            return None

    def register(self, frame, name):
        value = frame.FindRegister(name)
        return value.GetValueAsUnsigned() if value.IsValid() else None

    def wait_for_stop(self, seconds):
        event = self.lldb.SBEvent()
        deadline = time.time() + seconds
        while time.time() < deadline:
            if self.listener.WaitForEvent(1, event):
                if self.lldb.SBProcess.GetRestartedFromEvent(event):
                    continue  # an auto-continue breakpoint: the process never really stopped
                state = self.lldb.SBProcess.GetStateFromEvent(event)
                if state in (self.lldb.eStateStopped, self.lldb.eStateExited,
                             self.lldb.eStateCrashed):
                    return state
        return self.process.GetState()

    def install_breakpoints(self, decode, names=None):
        installed = {}
        for name in names or COUNTER_ENTRIES + list(ORDER_ENTRIES):
            if name == "ActiveBody::attemptDamage":
                bp = self.target.BreakpointCreateByLocation(ACTIVE_BODY_CPP, damage_line())
                # One statement can resolve to several addresses; keep one so a call hits once.
                for index in range(1, bp.GetNumLocations()):
                    bp.GetLocationAtIndex(index).SetEnabled(False)
            else:
                bp = self.target.BreakpointCreateByName(name)
            bp.SetAutoContinue(not decode)
            self.breakpoints[bp.GetID()] = name
            installed[name] = bp.GetNumLocations()
        return installed

    def event_counts(self):
        counts = {}
        for bp_id, name in self.breakpoints.items():
            counts[name] = self.target.FindBreakpointByID(bp_id).GetHitCount()
        return counts

    def handle_stop(self):
        """Decode the breakpoint each stopped thread hit; return once all are recorded."""
        for thread in self.process:
            if thread.GetStopReason() != self.lldb.eStopReasonBreakpoint:
                continue
            bp_id = thread.GetStopReasonDataAtIndex(0)
            name = self.breakpoints.get(bp_id)
            if name is None:
                continue
            self.frame = thread.GetFrameAtIndex(0)
            try:
                self.decoded.append(self.decode_event(name, self.frame))
            except RuntimeError as error:
                self.decoded.append({"event": name, "decode_error": str(error)})
            self.decoded_per_entry[name] = self.decoded_per_entry.get(name, 0) + 1
            if self.decoded_per_entry[name] >= DECODED_EVENTS_PER_ENTRY:
                bp = self.target.FindBreakpointByID(bp_id)
                bp.SetAutoContinue(True)
                if name in HIGH_FREQUENCY_ENTRIES:
                    bp.SetEnabled(False)
                    self.disabled_at[name] = self.decoded[-1].get("frame")

    def decode_event(self, name, frame):
        """Read the arguments of the interrupted call from arm64 argument registers."""
        this = self.register(frame, "x0")
        event = {"event": name,
                 "frame": self.integer("(int)((W3DGameLogic*)TheGameLogic)->m_frame")}
        if name in ORDER_ENTRIES:
            register, kind = ORDER_ENTRIES[name]
            source = self.register(frame, register)
            event.update(kind=kind, source=COMMAND_SOURCE_NAMES.get(source, source),
                         actor=self.object_summary("((AIUpdateInterface*)%d)->m_object" % this))
            if kind in ("attack_object", "force_attack_object"):
                event["victim"] = self.object_summary("(Object*)%d" % self.register(frame, "x1"))
        elif name == "ActiveBody::attemptDamage":
            this = frame.FindVariable("this").GetValueAsUnsigned()
            info = frame.FindVariable("damageInfo").GetValueAsUnsigned()
            event.update(
                victim=self.object_summary("((ActiveBody*)%d)->m_object" % this),
                damage_type=self.integer("(int)((DamageInfo*)%d)->in.m_damageType" % info),
                amount=self.real("((DamageInfo*)%d)->in.m_amount" % info),
                source_id=self.integer("(int)((DamageInfo*)%d)->in.m_sourceID" % info),
                health_before=self.real("((ActiveBody*)%d)->m_currentHealth" % this))
        elif name == "ArmorTemplate::adjustDamage":
            event.update(damage_type=self.register(frame, "x1"),
                         amount_in=self.float_register(frame, "s0"))
        elif name in ("Object::onDie", "GameLogic::destroyObject", "ScoreKeeper::addObjectLost",
                      "ScoreKeeper::addObjectDestroyed"):
            pointer = this if name == "Object::onDie" else self.register(frame, "x1")
            event["object"] = self.object_summary("(Object*)%d" % pointer)
        elif name == "Weapon::privateFireWeapon":
            event.update(source=self.object_summary("(Object*)%d" % self.register(frame, "x1")),
                         victim=self.object_summary("(Object*)%d" % self.register(frame, "x2")))
        elif name.startswith("SlowDeathBehavior::") or name == "DestroyDie::onDie":
            event["object"] = self.object_summary("((ObjectModule*)%d)->m_object" % this)
        elif name.endswith("projectileFireAtObjectOrPosition") or name.endswith("detonate"):
            event["projectile"] = self.object_summary(
                "((UpdateModule*)%d)->m_object" % this)
        elif name.startswith("OpenContain::"):
            event.update(container=self.object_summary("((OpenContain*)%d)->m_object" % this),
                         rider=self.object_summary("(Object*)%d" % self.register(frame, "x1")))
        return event

    def real(self, expression):
        try:
            return float(self.evaluate(expression).GetValue())
        except (RuntimeError, TypeError, ValueError):
            return None

    def float_register(self, frame, name):
        value = frame.FindRegister(name)
        try:
            return float(value.GetValue())
        except (TypeError, ValueError):
            return None

    def object_summary(self, pointer_expression):
        summary = {}
        summary["id"] = self.integer("(int)(%s)->m_id" % pointer_expression)
        summary["template"] = self.text(
            "(%s)->getTemplate()->m_nameString.str()" % pointer_expression)
        summary["player"] = self.integer(
            "(%s)->getControllingPlayer() ? (int)(%s)->getControllingPlayer()->m_playerIndex : -1"
            % (pointer_expression, pointer_expression))
        return summary

    def text(self, expression):
        try:
            return self.evaluate(expression).GetSummary().strip('"')
        except (RuntimeError, AttributeError):
            return None

    def object_table(self):
        count = self.evaluate(OBJECT_TABLE).GetValueAsSigned()
        error = self.lldb.SBError()
        raw = self.process.ReadMemory(self.buffer + 4, count * RECORD_INTS * 4, error)
        if error.Fail():
            raise RuntimeError("reading object table: %s" % error)
        objects = []
        for index in range(count):
            chunk = raw[index * RECORD_INTS * 4:(index + 1) * RECORD_INTS * 4]
            ints = struct.unpack("<%di" % RECORD_INTS, chunk)
            objects.append({
                "id": ints[F_ID], "player": ints[F_PLAYER],
                "template": c_string(chunk[F_TEMPLATE * 4:(F_TEMPLATE + NAME_INTS) * 4]),
                "team_state": c_string(chunk[F_TEAM_STATE * 4:(F_TEAM_STATE + NAME_INTS) * 4]),
                "health": round(float_bits(ints[F_HEALTH]), 2),
                "max_health": round(float_bits(ints[F_MAX_HEALTH]), 2),
                "ai_state": AI_STATE_NAMES.get(ints[F_AI_STATE], ints[F_AI_STATE]),
                "victim": ints[F_VICTIM],
                "weapon_slot": ints[F_WEAPON_SLOT],
                "weapon_status": ints[F_WEAPON_STATUS],
                "last_fire_frame": ints[F_LAST_FIRE],
                "ammo_in_clip": ints[F_AMMO],
                "next_shot_frame": ints[F_NEXT_SHOT],
                "private_status": ints[F_PRIVATE_STATUS],
                "projectile": bool(ints[F_KINDOF_LO]),
                "pos": [round(float_bits(ints[F_POS_X]), 1), round(float_bits(ints[F_POS_Y]), 1),
                        round(float_bits(ints[F_POS_Z]), 1)],
            })
        return objects

    def screen_position(self, world):
        """Where `TheTacticalView` puts a world point on the render surface, for real clicks."""
        packed = self.integer(
            "Coord3D probeW; probeW.x = %ff; probeW.y = %ff; probeW.z = %ff;"
            " ICoord2D probeS; probeS.x = -1; probeS.y = -1;"
            " int probeR = (int)((W3DView*)TheTacticalView)"
            "->worldToScreenTriReturn(&probeW, &probeS);"
            " (probeS.x + 4096) * 16384 * 4 + (probeS.y + 4096) * 4 + probeR;" % tuple(world))
        if packed is None:
            return None
        return {"x": packed // (16384 * 4) - 4096, "y": (packed // 4) % 16384 - 4096,
                "in_frustum": packed % 4 == 0}

    def locate(self, template_fragment, player):
        found = []
        for obj in self.object_table():
            if template_fragment.lower() not in obj["template"].lower():
                continue
            if player is not None and obj["player"] != player:
                continue
            obj["screen"] = self.screen_position(obj["pos"])
            found.append(obj)
        return found

    def sample(self):
        self.refresh_frame()
        record = {
            "time": time.time(),
            "frame": self.integer("(int)((W3DGameLogic*)TheGameLogic)->m_frame"),
            "game_mode": self.integer("(int)((W3DGameLogic*)TheGameLogic)->m_gameMode"),
            "event_counts": self.event_counts(),
        }
        losses = {}
        for player in range(8):
            packed = self.integer(SCORE % {"player": player})
            if packed is not None:
                losses[player] = {"units_lost": packed >> 16, "buildings_lost": packed & 0xFFFF}
        record["losses"] = losses
        record["objects"] = self.object_table()
        # `evaluate` may let the target run while it re-stops it; the table then belongs to a
        # later frame than the one read above, so record where the simulation actually was.
        record["frame_after"] = self.integer("(int)((W3DGameLogic*)TheGameLogic)->m_frame")
        return record

    def resume_for(self, seconds):
        """Run the game for `seconds`, decoding any stopping breakpoint hits on the way."""
        deadline = time.time() + seconds
        self.debugger.SetAsync(True)
        self.process.Continue()
        running = True
        handled = False
        while True:
            remaining = deadline - time.time()
            if remaining <= 0:
                break
            state = self.wait_for_stop(remaining)
            if state in (self.lldb.eStateExited, self.lldb.eStateCrashed):
                return
            if state == self.lldb.eStateStopped:
                running = False
                self.handle_stop()
                handled = True
                if deadline - time.time() <= 0:
                    break
                self.process.Continue()
                running = True
                handled = False
        for _attempt in range(3):
            if not running and self.process.GetState() == self.lldb.eStateStopped:
                break
            # GetState() can lag a Continue(), so the explicit stop keys off our own bookkeeping.
            self.process.Stop()
            self.wait_for_stop(30)
            running = self.process.GetState() != self.lldb.eStateStopped
            handled = False
        if self.process.GetState() != self.lldb.eStateStopped:
            raise RuntimeError("process did not stop for sampling (state %d)"
                               % self.process.GetState())
        if not handled:
            self.handle_stop()
        self.debugger.SetAsync(False)
        self.refresh_frame()

    def detach(self):
        self.process.Detach()


def summarise(samples):
    """Per-object health/state trajectories, so the document can quote transitions."""
    by_id = {}
    for sample in samples:
        for obj in sample["objects"]:
            entry = by_id.setdefault(obj["id"], {"template": obj["template"],
                                                 "player": obj["player"], "track": []})
            entry["track"].append((sample["frame"], obj["health"], obj["ai_state"],
                                   obj["victim"], obj["last_fire_frame"]))
    seen_last = {obj["id"] for obj in samples[-1]["objects"]} if samples else set()
    damaged = {}
    removed = {}
    for object_id, entry in by_id.items():
        healths = [point[1] for point in entry["track"] if point[1] >= 0]
        if healths and min(healths) < max(healths):
            damaged[object_id] = entry
        if object_id not in seen_last:
            removed[object_id] = entry
    return {"objects_tracked": len(by_id), "damaged": damaged, "removed": removed}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--minutes", type=float, default=5.0)
    parser.add_argument("--interval", type=float, default=10.0)
    parser.add_argument("--out", help="JSON report path (required unless --locate)")
    parser.add_argument("--no-breakpoints", action="store_true",
                        help="object table only; no combat-path hit counters")
    parser.add_argument("--no-decode", action="store_true",
                        help="count combat-path hits but never stop to decode arguments")
    parser.add_argument("--locate", metavar="TEMPLATE",
                        help="print objects whose template contains TEMPLATE, with their screen"
                             " position, then detach (no sampling)")
    parser.add_argument("--player", type=int, help="restrict --locate to this player index")
    parser.add_argument("--trace", metavar="SYMBOL[,SYMBOL...]",
                        help="count hits on these functions for --seconds while the game runs"
                             " (post real input from another shell meanwhile), then detach")
    parser.add_argument("--seconds", type=float, default=8.0)
    parser.add_argument("--expr-file", metavar="PATH",
                        help="evaluate one C++ expression per line in the stopped target,"
                             " print each result, then detach (no sampling)")
    args = parser.parse_args()
    if args.expr_file is not None:
        require_lldb()
        probe = Probe(args.pid, args.binary)
        try:
            with open(args.expr_file) as handle:
                for line in handle:
                    expression = line.strip()
                    if not expression or expression.startswith("#"):
                        continue
                    try:
                        value = probe.evaluate(expression)
                        print("%s\n  = %s" % (expression, value.GetValue() or value.GetSummary()),
                              flush=True)
                    except RuntimeError as error:
                        print("%s\n  ! %s" % (expression, str(error).splitlines()[0]), flush=True)
        finally:
            probe.detach()
        return
    if args.trace is not None:
        require_lldb()
        probe = Probe(args.pid, args.binary)
        try:
            names = args.trace.split(",")
            locations = probe.install_breakpoints(decode=not args.no_decode, names=names)
            start = probe.integer("(int)((W3DGameLogic*)TheGameLogic)->m_frame")
            probe.resume_for(args.seconds)
            end = probe.integer("(int)((W3DGameLogic*)TheGameLogic)->m_frame")
            print(json.dumps({"frames": [start, end], "locations": locations,
                              "hits": probe.event_counts(), "events": probe.decoded},
                             indent=1))
        finally:
            probe.detach()
        return
    if args.locate is None and args.out is None:
        parser.error("--out is required when sampling")
    require_lldb()

    probe = Probe(args.pid, args.binary)
    if args.locate is not None:
        try:
            print(json.dumps({"frame": probe.integer("(int)((W3DGameLogic*)TheGameLogic)->m_frame"),
                              "objects": probe.locate(args.locate, args.player)}, indent=1))
        finally:
            probe.detach()
        return
    breakpoints = {}
    if not args.no_breakpoints:
        breakpoints = probe.install_breakpoints(decode=not args.no_decode)
    report = {"pid": args.pid, "binary": args.binary, "started": time.time(),
              "breakpoints": breakpoints, "samples": []}
    deadline = time.time() + args.minutes * 60
    try:
        while True:
            sample = probe.sample()
            report["samples"].append(sample)
            report["events"] = probe.decoded
            report["breakpoints_disabled_at_frame"] = probe.disabled_at
            report["summary"] = summarise(report["samples"])
            with open(args.out, "w") as handle:
                json.dump(report, handle, indent=1)
            alive = sum(1 for obj in sample["objects"] if obj["health"] > 0)
            print("frame %s objects %d alive %d events %s" % (
                sample["frame"], len(sample["objects"]), alive,
                {k.split("::")[-1]: v for k, v in sample["event_counts"].items()}), flush=True)
            if time.time() >= deadline:
                break
            probe.resume_for(args.interval)
            if probe.process.GetState() in (probe.lldb.eStateExited, probe.lldb.eStateCrashed):
                report["exited"] = probe.process.GetExitStatus()
                break
    finally:
        with open(args.out, "w") as handle:
            json.dump(report, handle, indent=1)
        if probe.process.GetState() not in (probe.lldb.eStateExited, probe.lldb.eStateCrashed):
            probe.detach()


if __name__ == "__main__":
    main()

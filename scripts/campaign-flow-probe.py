#!/usr/bin/env python3
"""Measure whether a campaign map's script logic runs in the native Zero Hour binary (Linux).

Launches `zh -headless -file <map>` under gdb and counts, at every logic frame, what the script
engine actually does: scripts evaluated, conditions by type, actions by type, which scripts fired
and when, timer/counter/flag state, and whether the audio / video / UI / mission-end actions
reach their engine entry points. Nothing is inferred from the frame counter alone.

    python3 scripts/campaign-flow-probe.py --run-dir ~/zh-data/run --frames 3000 \\
        --json /tmp/campaign-flow.json

    # synthetic mission end: call ScriptActions::doVictory() from gdb at a chosen frame
    python3 scripts/campaign-flow-probe.py --run-dir ~/zh-data/run --frames 3000 \\
        --force-end victory --force-at-frame 600 --json /tmp/campaign-flow-victory.json

`--run-dir` is a disposable directory holding the retail `.big` files and a `zh` symlink to
`build/native-debug/native_strict_link` (see docs/porting/startup-to-mission-start.md). The
`-file <map>` parser is `RTS_DEBUG`-only in CommandLine.cpp, so the binary must come from
`scripts/native-build.py --config debug`; a release binary silently runs the shell map instead,
which is why the summary prints `map loaded` and `game_mode` first. The base game's
`English.big` is located through `~/.config/CommandAndConquerGeneralsZeroHour/Registry.ini`.
Retail data is never committed. Linux gdb only; the same file is both the launcher (plain python)
and the gdb script (it re-imports itself inside gdb's embedded interpreter).

Breakpoints only count and resume, except the per-frame sampler in `ScriptEngine::update`, which
reads counters/timers/flags and is the only place the inferior is called into (forced end).
Every hit stops all threads, so the logic rate under the probe is ~20 frames/s; frame counts,
not wall time, are the unit everywhere in the output.

`docs/porting/campaign-flow-probe.md` reads the output.
"""

import json
import os
import sys
import time

try:
    import gdb  # type: ignore  # only exists inside gdb's embedded interpreter
except ImportError:
    gdb = None

# ---------------------------------------------------------------------------------------------
# Engine entry points that get a counting breakpoint. (link name, function). Names are what
# `info functions` prints. Missing symbols are reported, not fatal.
# ---------------------------------------------------------------------------------------------
LINKS = [
    # script engine
    ("script_new_map", "ScriptEngine::newMap"),
    ("script_evaluated", "ScriptEngine::executeScript"),
    ("condition_dispatched", "ScriptConditions::evaluateCondition"),
    ("action_dispatched", "ScriptActions::executeAction"),
    ("timer_set", "ScriptEngine::setTimer"),
    ("timer_evaluated", "ScriptEngine::evaluateTimer"),
    ("subroutine_called", "ScriptEngine::callSubroutine"),
    # UI-facing actions (headless: TheInGameUI is real, TheWindowManager is the Dummy)
    ("ui_message", "InGameUI::message"),
    ("ui_popup", "InGameUI::popupMessage"),
    ("ui_military_subtitle", "InGameUI::militarySubtitle"),
    ("ui_window_from_script", "GameWindowManagerDummy::winCreateFromScript"),
    ("ui_disable_input", "ScriptActions::doDisableInput"),
    ("ui_enable_input", "ScriptActions::doEnableInput"),
    # audio chain (engine side; the headless manager is MilesAudioManagerDummy)
    ("audio_raised", "AudioManager::addAudioEvent"),
    ("audio_removed", "AudioManager::removeAudioEvent"),
    ("audio_queued", "AudioManager::appendAudioRequest"),
    ("audio_processed", "MilesAudioManager::playAudioEvent"),
    ("audio_stream_handle", "MilesAudioManager::playStream"),
    ("audio_sample_handle", "MilesAudioManager::playSample"),
    ("audio_sample3d_handle", "MilesAudioManager::playSample3D"),
    ("audio_started_stream", "AIL_start_stream"),
    ("audio_started_sample", "AIL_start_sample"),
    ("audio_started_sample3d", "AIL_start_3D_sample"),
    # video chain
    ("video_display_play", "Display::playMovie"),
    ("video_ingameui_play", "InGameUI::playMovie"),
    ("video_player_open", "FFmpegVideoPlayer::open"),
    ("video_file_open", "FFmpegFile::open"),
    ("video_stream_created", "FFmpegVideoPlayer::createStream"),
    ("video_frame_decoded", "FFmpegVideoStream::frameDecompress"),
    ("video_frame_rendered", "FFmpegVideoStream::frameRender"),
    # mission end
    ("end_victory", "ScriptActions::doVictory"),
    ("end_quick_victory", "ScriptActions::doQuickVictory"),
    ("end_defeat", "ScriptActions::doDefeat"),
    ("end_local_defeat", "ScriptActions::doLocalDefeat"),
    ("end_campaign_set_victorious", "CampaignManager::SetVictorious"),
    ("end_exit_game", "GameLogic::exitGame"),
    ("end_clear_game_data", "GameLogic::clearGameData"),
    ("end_shell_push", "Shell::push"),
]

# Script actions that raise audio; the addAudioEvent they make is traced to its return value.
SCRIPT_AUDIO_ORIGINS = [
    ("SPEECH_PLAY", "ScriptActions::doSpeechPlay"),
    ("MUSIC_SET_TRACK", "ScriptActions::doMusicTrackChange"),
    ("PLAY_SOUND_EFFECT", "ScriptActions::doPlaySoundEffect"),
    ("PLAY_SOUND_EFFECT_AT", "ScriptActions::doPlaySoundEffectAt"),
]

# Links whose argument names an AsciiString worth keeping (argument, field or None).
NAMED_ARGUMENT = {
    "audio_raised": ("eventToAdd", "m_eventName"),
    "audio_processed": ("req", "m_pendingEvent.m_eventName"),
    "audio_stream_handle": ("event", "m_eventName"),
    "audio_sample_handle": ("event", "m_eventName"),
    "audio_sample3d_handle": ("event", "m_eventName"),
    "ui_message": ("stringManagerLabel", None),
    "ui_military_subtitle": ("label", None),
    "ui_popup": ("message", None),
    "video_display_play": ("movieName", None),
    "video_ingameui_play": ("movieName", None),
    "video_player_open": ("movieTitle", None),
    "end_shell_push": ("filename", None),
}

# Condition types that are "triggers" in the task's sense (area / destruction / built / etc).
TRIGGER_PREFIXES = (
    "PLAYER_ENTERED", "TEAM_ENTERED", "NAMED_ENTERED", "TEAM_EXITED", "NAMED_EXITED",
    "PLAYER_EXITED", "NAMED_INSIDE", "NAMED_OUTSIDE", "TEAM_INSIDE", "TEAM_OUTSIDE",
    "NAMED_DESTROYED", "TEAM_DESTROYED", "PLAYER_DESTROYED", "NAMED_DYING", "NAMED_KILLED",
    "PLAYER_HAS_UNITS_IN_AREA", "PLAYER_HAS_NO_UNITS_IN_AREA", "NAMED_ATTACKED_BY",
    "TEAM_ATTACKED_BY", "TEAM_STATE_IS", "NAMED_BUILDING_IS_EMPTY", "BUILDING_ENTERED",
    "ENEMY_SIGHTED", "TYPE_SIGHTED", "NAMED_DISCOVERED", "TEAM_DISCOVERED",
    "PLAYER_UNIT_EXISTS", "PLAYER_HAS_UNIT", "PLAYER_HAS_OBJECT", "NAMED_HEALTH",
    "TEAM_HEALTH", "UNIT_HEALTH", "NAMED_CAPTURED", "TEAM_CAPTURED", "PLAYER_CAPTURED",
    "NAMED_HAS_FREE_CONTAINER", "PLAYER_LOST_OBJECT", "PLAYER_TRIGGERED_SPECIAL_POWER",
    "PLAYER_HAS_COMPLETED_UPGRADE", "PLAYER_HAS_SCIENCE", "PLAYER_HAS_POWER",
    "PLAYER_BUILT_UPGRADE", "PLAYER_SELECTED_UNIT",
)
AUDIO_ACTION_PREFIXES = ("PLAY_SOUND", "SPEECH_", "MUSIC_", "SOUND_", "EVA_", "SET_AMBIENT")
VIDEO_ACTION_PREFIXES = ("MOVIE_",)
UI_ACTION_PREFIXES = (
    "DISPLAY_", "SHOW_MILITARY", "HIDE_MILITARY", "CAMERA_", "IN_GAME_POPUP",
    "SET_CINEMATIC", "LETTERBOX", "DISABLE_INPUT", "ENABLE_INPUT", "RADAR_",
    "OBJECT_FLASH", "SET_TOPPLE", "UI_",
)
END_ACTIONS = ("VICTORY", "DEFEAT", "LOCALDEFEAT", "QUICK_VICTORY")

RECENT_LIMIT = 40
FIRING_LIMIT = 4000

ENV_PREFIX = "CAMPAIGN_PROBE_"


# =============================================================================================
# gdb side
# =============================================================================================
def ascii_string(value):
    """Read an AsciiString without calling into the inferior (stop handlers cannot)."""
    try:
        data = value["m_data"]
        if int(data) == 0:
            return ""
        chars = (data + 1).cast(gdb.lookup_type("char").pointer())
        return chars.string(errors="replace")
    except gdb.error as error:
        return "<unreadable: %s>" % error


def enum_name(type_name, number, cache):
    key = (type_name, number)
    if key not in cache:
        try:
            text = str(gdb.parse_and_eval("(%s)%d" % (type_name, number)))
        except gdb.error:
            text = "%s(%d)" % (type_name, number)
        cache[key] = text.split("::")[-1]
    return cache[key]


def eval_int(expression, default=None):
    try:
        return int(gdb.parse_and_eval(expression))
    except gdb.error:
        return default


def game_mode_name():
    try:
        return str(gdb.parse_and_eval("TheGameLogic->m_gameMode"))
    except gdb.error:
        return None


def count_objects(limit=20000):
    """Walk TheGameLogic->m_objList (no inferior call)."""
    try:
        cursor = gdb.parse_and_eval("TheGameLogic->m_objList")
        n = 0
        while int(cursor) != 0 and n < limit:
            n += 1
            cursor = cursor["m_next"]
        return n
    except gdb.error:
        return None


class Probe(object):
    def __init__(self, options):
        self.options = options
        self.enum_cache = {}
        self.link_counts = {}
        self.link_first_frame = {}
        self.link_names = {}
        self.link_recent = {}
        self.missing_symbols = []
        self.updates = 0
        self.logic_frame = -1
        self.first_update_frame = None
        self.new_map_frame = None
        self.scripts = {}          # pointer -> {name, evaluated, fired_true, fired_false}
        self.conditions = {}       # type name -> evaluations
        self.conditions_engine = {}  # COUNTER/FLAG/TIMER/TRUE/FALSE handled inside ScriptEngine
        self.actions = {}          # type name -> executions (from the executeActions walk)
        self.firings = []          # [{frame, script, branch, actions:[...]}]
        self.timeline = []         # [{frame, link, name}] for the interesting links
        self.samples = []          # periodic state snapshots
        self.timer_events = []     # [{frame, name, event, value}]
        self.last_timer_values = {}
        self.forced_end = None
        self.map_loaded = None
        self.assertions = {}
        self.script_audio = []      # [{frame, action, event, result, handle}]
        self.script_audio_origin = None
        self.inventory = None       # static script inventory taken at newMap
        self.crash = None
        self.exit_code = None
        self.started = time.monotonic()
        self.stop_reason = None

    # -- bookkeeping ---------------------------------------------------------------------------
    def bump(self, link, name=None):
        self.link_counts[link] = self.link_counts.get(link, 0) + 1
        if link not in self.link_first_frame:
            self.link_first_frame[link] = self.logic_frame
        if name is not None:
            names = self.link_names.setdefault(link, {})
            names[name] = names.get(name, 0) + 1
            recent = self.link_recent.setdefault(link, [])
            recent.append([self.logic_frame, name])
            del recent[:-RECENT_LIMIT]
        if not link.startswith(("script_evaluated", "condition_", "action_dispatched",
                                "timer_evaluated")) and len(self.timeline) < FIRING_LIMIT:
            self.timeline.append({"frame": self.logic_frame, "link": link, "name": name})

    def condition_name(self, number):
        return enum_name("Condition::ConditionType", number, self.enum_cache)

    def action_name(self, number):
        return enum_name("ScriptAction::ScriptActionType", number, self.enum_cache)

    def script_record(self, script):
        key = int(script)
        record = self.scripts.get(key)
        if record is None:
            record = {"name": ascii_string(script["m_scriptName"]), "evaluated": 0,
                      "fired_true": 0, "fired_false": 0, "first_fired_frame": None}
            self.scripts[key] = record
        return record

    def walk_actions(self, head):
        out = []
        cursor = head
        guard = 0
        while int(cursor) != 0 and guard < 256:
            guard += 1
            number = int(cursor["m_actionType"])
            name = self.action_name(number)
            self.actions[name] = self.actions.get(name, 0) + 1
            params = []
            try:
                count = min(int(cursor["m_numParms"]), 12)
                for i in range(count):
                    parm = cursor["m_parms"][i]
                    if int(parm) == 0:
                        continue
                    text = ascii_string(parm["m_string"])
                    params.append(text if text else int(parm["m_int"]))
            except gdb.error:
                pass
            out.append({"type": name, "params": params})
            cursor = cursor["m_nextAction"]
        return out

    # -- per-frame sampling --------------------------------------------------------------------
    def read_counters(self):
        counters = []
        try:
            n = int(gdb.parse_and_eval("TheScriptEngine->m_numCounters"))
            table = gdb.parse_and_eval("TheScriptEngine->m_counters")
            for i in range(1, min(n, 256)):
                entry = table[i]
                counters.append({"name": ascii_string(entry["name"]),
                                 "value": int(entry["value"]),
                                 "timer": bool(int(entry["isCountdownTimer"]))})
        except gdb.error:
            pass
        return counters

    def read_flags(self):
        flags = []
        try:
            n = int(gdb.parse_and_eval("TheScriptEngine->m_numFlags"))
            table = gdb.parse_and_eval("TheScriptEngine->m_flags")
            for i in range(1, min(n, 256)):
                entry = table[i]
                flags.append({"name": ascii_string(entry["name"]),
                              "value": bool(int(entry["value"]))})
        except gdb.error:
            pass
        return flags

    def track_timers(self, counters):
        for entry in counters:
            if not entry["timer"]:
                continue
            name, value = entry["name"], entry["value"]
            previous = self.last_timer_values.get(name)
            if previous is None:
                self.timer_events.append({"frame": self.logic_frame, "timer": name,
                                          "event": "first_seen", "value": value})
            elif previous > 0 and value < previous and value == previous - 1:
                pass  # normal tick
            elif previous >= 0 and value < 0:
                self.timer_events.append({"frame": self.logic_frame, "timer": name,
                                          "event": "expired", "value": value})
            elif value > previous:
                self.timer_events.append({"frame": self.logic_frame, "timer": name,
                                          "event": "set", "value": value})
            self.last_timer_values[name] = value

    def sample(self):
        counters = self.read_counters()
        self.track_timers(counters)
        if self.updates % self.options["sample_every"] == 0 or self.updates == 1:
            self.samples.append({
                "update": self.updates,
                "frame": self.logic_frame,
                "elapsed_s": round(time.monotonic() - self.started, 1),
                "objects": count_objects(),
                "game_mode": game_mode_name(),
                "end_game_timer": eval_int("TheScriptEngine->m_endGameTimer"),
                "engine_quitting": eval_int("(int)TheGameEngine->m_quitting"),
                "campaign_victorious": eval_int("(int)TheCampaignManager->m_victorious"),
                "close_window_timer": eval_int("TheScriptEngine->m_closeWindowTimer"),
                "fade": eval_int("(int)TheScriptEngine->m_fade"),
                "current_track": ascii_string(
                    gdb.parse_and_eval("TheScriptEngine->m_currentTrackName")),
                "counters": counters,
                "flags": self.read_flags(),
            })

    def scripts_summary(self):
        by_name = {}
        for record in self.scripts.values():
            by_name[record["name"]] = record
        return by_name

    def result(self):
        return {
            "method": "gdb counting breakpoints on the native strict-link binary, "
                      "`-headless -file <map>`, Linux",
            "options": self.options,
            "stop_reason": self.stop_reason,
            "exit_code": self.exit_code,
            "elapsed_s": round(time.monotonic() - self.started, 1),
            "script_updates": self.updates,
            "first_update_frame": self.first_update_frame,
            "last_logic_frame": self.logic_frame,
            "new_map_frame": self.new_map_frame,
            "map_loaded": self.map_loaded,
            "assertions": self.assertions,
            "script_audio": self.script_audio,
            "inventory": self.inventory,
            "crash": self.crash,
            "missing_symbols": self.missing_symbols,
            "links": {k: {"count": self.link_counts[k], "first_frame": self.link_first_frame[k],
                          "names": self.link_names.get(k, {})}
                      for k in sorted(self.link_counts)},
            "scripts": self.scripts_summary(),
            "conditions": dict(sorted(self.conditions.items())),
            "conditions_engine_internal": dict(sorted(self.conditions_engine.items())),
            "actions": dict(sorted(self.actions.items())),
            "firings": self.firings,
            "timer_events": self.timer_events,
            "timeline": self.timeline,
            "samples": self.samples,
            "forced_end": self.forced_end,
        }


PROBE = None


class LinkCounter(gdb.Breakpoint if gdb else object):
    def __init__(self, link, function):
        super().__init__(function, internal=True)
        self.link = link

    def stop(self):
        name = None
        spec = NAMED_ARGUMENT.get(self.link)
        if spec is not None:
            argument, field = spec
            try:
                value = gdb.parse_and_eval(argument)
                if field is not None:
                    for part in field.split("."):
                        if int(value) == 0:
                            value = None
                            break
                        value = value[part]
                    name = ascii_string(value) if value is not None else None
                else:
                    if value.type.code == gdb.TYPE_CODE_PTR:
                        value = value.dereference()
                    name = ascii_string(value)
            except gdb.error:
                name = "<unreadable>"
        PROBE.bump(self.link, name)
        if self.link == "audio_raised" and PROBE.script_audio_origin is not None:
            AddAudioEventReturn(gdb.newest_frame(), PROBE.script_audio_origin, name)
            PROBE.script_audio_origin = None
        return False


AHSV_NAMES = {0: "AHSV_Error", 1: "AHSV_NoSound", 2: "AHSV_Muted", 3: "AHSV_NotForLocal",
              4: "AHSV_StopTheMusic", 5: "AHSV_StopTheMusicFade"}


class ScriptAudioOrigin(gdb.Breakpoint if gdb else object):
    """Marks that the next AudioManager::addAudioEvent comes from a script action, so its
    return value (the handle, or an AHSV_* refusal) can be attributed to the action."""

    def __init__(self, action, function):
        super().__init__(function, internal=True)
        self.action = action

    def stop(self):
        PROBE.script_audio_origin = self.action
        return False


class AddAudioEventReturn(gdb.FinishBreakpoint if gdb else object):
    def __init__(self, frame, action, event_name):
        super().__init__(frame, internal=True)
        self.action = action
        self.event_name = event_name
        self.frame_number = PROBE.logic_frame

    def stop(self):
        handle = int(self.return_value) if self.return_value is not None else None
        outcome = AHSV_NAMES.get(handle, "handle" if handle is not None else "<unknown>")
        PROBE.script_audio.append({"frame": self.frame_number, "action": self.action,
                                   "event": self.event_name, "result": outcome,
                                   "handle": handle})
        return False

    def out_of_scope(self):
        PROBE.script_audio.append({"frame": self.frame_number, "action": self.action,
                                   "event": self.event_name, "result": "<out of scope>",
                                   "handle": None})


class ScriptCounter(gdb.Breakpoint if gdb else object):
    def __init__(self):
        super().__init__("ScriptEngine::executeScript", internal=True)

    def stop(self):
        PROBE.bump("script_evaluated")
        try:
            script = gdb.parse_and_eval("pScript")
            if int(script) != 0:
                PROBE.script_record(script)["evaluated"] += 1
        except gdb.error:
            pass
        return False


class ConditionCounter(gdb.Breakpoint if gdb else object):
    """ScriptEngine::evaluateCondition sees every condition (TRUE/FALSE/COUNTER/FLAG/TIMER are
    handled there; everything else is dispatched to ScriptConditions)."""

    def __init__(self):
        super().__init__("ScriptEngine::evaluateCondition", internal=True)

    def stop(self):
        try:
            condition = gdb.parse_and_eval("pCondition")
            if int(condition) != 0:
                name = PROBE.condition_name(int(condition["m_conditionType"]))
                PROBE.conditions[name] = PROBE.conditions.get(name, 0) + 1
                if name in ("CONDITION_TRUE", "CONDITION_FALSE", "COUNTER", "FLAG",
                            "TIMER_EXPIRED"):
                    PROBE.conditions_engine[name] = PROBE.conditions_engine.get(name, 0) + 1
        except gdb.error:
            pass
        return False


class ActionsWalker(gdb.Breakpoint if gdb else object):
    """ScriptEngine::executeActions is entered once per fired script branch (and per sequential
    script step); walking its list gives every action type including the engine-internal ones."""

    def __init__(self):
        super().__init__("ScriptEngine::executeActions", internal=True)

    def stop(self):
        try:
            head = gdb.parse_and_eval("pActionHead")
        except gdb.error:
            return False
        if int(head) == 0:
            return False
        actions = PROBE.walk_actions(head)
        script_name, branch = None, "sequential-or-team"
        frame = gdb.selected_frame()
        try:
            older = frame.older()
            if older is not None and older.name() and "executeScript" in older.name():
                script = older.read_var("pScript")
                record = PROBE.script_record(script)
                script_name = record["name"]
                if int(head) == int(script["m_action"]):
                    branch = "true"
                    record["fired_true"] += 1
                else:
                    branch = "false"
                    record["fired_false"] += 1
                if record["first_fired_frame"] is None:
                    record["first_fired_frame"] = PROBE.logic_frame
        except (gdb.error, ValueError, RuntimeError):
            pass
        if len(PROBE.firings) < FIRING_LIMIT:
            PROBE.firings.append({"frame": PROBE.logic_frame, "script": script_name,
                                  "branch": branch, "actions": actions})
        return False


class UpdateSampler(gdb.Breakpoint if gdb else object):
    """Stops in ScriptEngine::update; the only breakpoint that returns True (to allow calls)."""

    def __init__(self):
        super().__init__("ScriptEngine::update", internal=True)

    def stop(self):
        PROBE.updates += 1
        PROBE.logic_frame = eval_int("TheGameLogic->m_frame", -1)
        if PROBE.first_update_frame is None:
            PROBE.first_update_frame = PROBE.logic_frame
        PROBE.sample()
        options = PROBE.options
        if PROBE.updates >= options["frames"]:
            PROBE.stop_reason = "frame budget reached"
            return True
        force = options["force_end"]
        if force != "none" and PROBE.forced_end is None and \
                PROBE.logic_frame >= options["force_at_frame"]:
            return True
        return False


class AssertionCounter(gdb.Breakpoint if gdb else object):
    """DEBUG_ASSERTCRASH sites (debug configuration only); the message is read from the
    DEBUG_CRASH_AT() globals, not by calling into the inferior."""

    def __init__(self):
        super().__init__("DebugCrash", internal=True)

    def stop(self):
        site = "?"
        try:
            path = gdb.parse_and_eval("TheCurrentCrashFile")
            if int(path) != 0:
                site = "%s:%d" % (path.string(errors="replace").split("/")[-1],
                                  int(gdb.parse_and_eval("TheCurrentCrashLine")))
            condition = gdb.parse_and_eval("TheCurrentCrashCondition")
            if int(condition) != 0:
                site += " !(%s)" % condition.string(errors="replace")
        except gdb.error:
            pass
        entry = PROBE.assertions.setdefault(site, {"count": 0, "first_frame": PROBE.logic_frame})
        entry["count"] += 1
        return False


class NewMap(gdb.Breakpoint if gdb else object):
    def __init__(self):
        super().__init__("ScriptEngine::newMap", internal=True)

    def stop(self):
        PROBE.new_map_frame = eval_int("TheGameLogic->m_frame", -1)
        PROBE.bump("script_new_map")
        PROBE.map_loaded = {
            "map_name": ascii_string(gdb.parse_and_eval("TheWritableGlobalData->m_mapName")),
            "pending_file": ascii_string(
                gdb.parse_and_eval("TheWritableGlobalData->m_pendingFile")),
            "initial_file": ascii_string(
                gdb.parse_and_eval("TheWritableGlobalData->m_initialFile")),
            "game_mode": game_mode_name(),
        }
        PROBE.inventory = script_inventory()
        return False


def script_inventory():
    """Static walk of every script the map carries (TheSidesList) at newMap: which action and
    condition types exist at all, independent of whether they fire within the frame budget."""
    inventory = {"sides": 0, "scripts": 0, "actions": {}, "conditions": {}, "error": None}

    def visit_actions(head):
        cursor = head
        guard = 0
        while int(cursor) != 0 and guard < 256:
            guard += 1
            name = PROBE.action_name(int(cursor["m_actionType"]))
            inventory["actions"][name] = inventory["actions"].get(name, 0) + 1
            cursor = cursor["m_nextAction"]

    def visit_conditions(head):
        clause = head
        guard = 0
        while int(clause) != 0 and guard < 256:
            guard += 1
            term = clause["m_firstAnd"]
            inner = 0
            while int(term) != 0 and inner < 256:
                inner += 1
                name = PROBE.condition_name(int(term["m_conditionType"]))
                inventory["conditions"][name] = inventory["conditions"].get(name, 0) + 1
                term = term["m_nextAndCondition"]
            clause = clause["m_nextOr"]

    def visit_scripts(head):
        script = head
        guard = 0
        while int(script) != 0 and guard < 4096:
            guard += 1
            inventory["scripts"] += 1
            visit_conditions(script["m_condition"])
            visit_actions(script["m_action"])
            visit_actions(script["m_actionFalse"])
            script = script["m_nextScript"]

    try:
        sides = gdb.parse_and_eval("TheSidesList->m_sides")
        count = eval_int("TheSidesList->m_numSides", 0)
        inventory["sides"] = count
        for i in range(count):
            scripts = sides[i]["m_scripts"]
            if int(scripts) == 0:
                continue
            visit_scripts(scripts["m_firstScript"])
            group = scripts["m_firstGroup"]
            guard = 0
            while int(group) != 0 and guard < 1024:
                guard += 1
                visit_scripts(group["m_firstScript"])
                group = group["m_nextGroup"]
    except gdb.error as error:
        inventory["error"] = str(error)
    return inventory


def install_breakpoints():
    gdb.execute("set breakpoint pending on")
    for link, function in LINKS:
        if link == "script_new_map":
            continue
        counter = LinkCounter(link, function)
        if counter.pending:
            PROBE.missing_symbols.append(function)
            counter.delete()
    NewMap()
    for action, function in SCRIPT_AUDIO_ORIGINS:
        origin = ScriptAudioOrigin(action, function)
        if origin.pending:
            PROBE.missing_symbols.append(function)
            origin.delete()
    assertion = AssertionCounter()
    if assertion.pending:
        assertion.delete()
    ScriptCounter()
    ConditionCounter()
    ActionsWalker()
    UpdateSampler()


def force_end(kind):
    """Synthetic mission end: what the script action VICTORY/DEFEAT would do."""
    call = {"victory": "((ScriptActions*)TheScriptActions)->doVictory()",
            "defeat": "((ScriptActions*)TheScriptActions)->doDefeat()"}[kind]
    before = {"frame": PROBE.logic_frame,
              "end_game_timer": eval_int("TheScriptEngine->m_endGameTimer")}
    try:
        gdb.execute("call (void)%s" % call, to_string=True)
        error = None
    except gdb.error as exc:
        error = str(exc)
    PROBE.forced_end = {"kind": kind, "call": call, "before": before, "error": error,
                        "end_game_timer_after": eval_int("TheScriptEngine->m_endGameTimer")}
    PROBE.timeline.append({"frame": PROBE.logic_frame, "link": "forced_end", "name": kind})


def gdb_main():
    global PROBE
    options = {
        "frames": int(os.environ.get(ENV_PREFIX + "FRAMES", "3000")),
        "sample_every": int(os.environ.get(ENV_PREFIX + "SAMPLE_EVERY", "100")),
        "force_end": os.environ.get(ENV_PREFIX + "FORCE_END", "none"),
        "force_at_frame": int(os.environ.get(ENV_PREFIX + "FORCE_AT_FRAME", "600")),
        "json": os.environ.get(ENV_PREFIX + "JSON", ""),
        "map": os.environ.get(ENV_PREFIX + "MAP", ""),
    }
    PROBE = Probe(options)
    gdb.execute("set pagination off")
    gdb.execute("set print thread-events off")
    gdb.execute("handle SIGPIPE nostop noprint pass")
    install_breakpoints()

    gdb.execute("run")
    while True:
        inferior = gdb.selected_inferior()
        alive = inferior.is_valid() and any(t.is_valid() for t in inferior.threads())
        if not alive:
            break
        if PROBE.stop_reason == "frame budget reached":
            break
        if options["force_end"] != "none" and PROBE.forced_end is None:
            force_end(options["force_end"])
            gdb.execute("continue")
            continue
        # unexpected stop (signal / crash): record and finish
        try:
            PROBE.stop_reason = "stopped: " + gdb.execute("info program", to_string=True).strip()
            detail = gdb.execute("bt 12", to_string=True)
            for command in ("x/3i $pc", "info registers rip rax rbx rcx rdx rsi rdi",
                            "info locals", "info args"):
                try:
                    detail += "\n$ %s\n%s" % (command, gdb.execute(command, to_string=True))
                except gdb.error as error:
                    detail += "\n$ %s\n(%s)" % (command, error)
            PROBE.crash = detail
            PROBE.timeline.append({"frame": PROBE.logic_frame, "link": "crash",
                                   "name": detail})
        except gdb.error:
            PROBE.stop_reason = "stopped: unknown"
        break

    inferior = gdb.selected_inferior()
    if inferior.is_valid() and any(t.is_valid() for t in inferior.threads()):
        if PROBE.stop_reason is None:
            PROBE.stop_reason = "still running at end of budget"
        gdb.execute("kill")
    else:
        try:
            PROBE.exit_code = int(gdb.parse_and_eval("$_exitcode"))
        except gdb.error:
            PROBE.exit_code = None
        if PROBE.stop_reason is None or PROBE.stop_reason == "frame budget reached":
            PROBE.stop_reason = "process exited"

    result = PROBE.result()
    if options["json"]:
        with open(options["json"], "w") as handle:
            json.dump(result, handle, indent=1, sort_keys=False)
    sys.stdout.write("CAMPAIGN_PROBE_RESULT " + json.dumps({
        "stop_reason": result["stop_reason"], "exit_code": result["exit_code"],
        "script_updates": result["script_updates"], "last_logic_frame": result["last_logic_frame"],
        "scripts": len(result["scripts"]), "firings": len(result["firings"]),
        "missing_symbols": result["missing_symbols"]}) + "\n")
    sys.stdout.flush()


# =============================================================================================
# launcher side
# =============================================================================================
def summarize(result, out):
    def line(text=""):
        out.write(text + "\n")

    links = result["links"]

    def count(link):
        return links.get(link, {}).get("count", 0)

    line("# campaign-flow-probe summary")
    line("stop: %s, exit code: %s, script updates: %d, logic frames %s..%s, %.0fs wall" % (
        result["stop_reason"], result["exit_code"], result["script_updates"],
        result["first_update_frame"], result["last_logic_frame"], result["elapsed_s"]))
    line("map loaded: %s" % json.dumps(result["map_loaded"]))
    if result["missing_symbols"]:
        line("missing symbols: " + ", ".join(result["missing_symbols"]))
    if result["inventory"]:
        inventory = result["inventory"]
        line("## static script inventory at newMap: %d sides, %d scripts%s" % (
            inventory["sides"], inventory["scripts"],
            " (error: %s)" % inventory["error"] if inventory["error"] else ""))
        executed = result["actions"]
        for name, total in sorted(inventory["actions"].items(), key=lambda kv: (-kv[1], kv[0])):
            line("  action    %-48s %6d  executed %d" % (name, total, executed.get(name, 0)))
        evaluated = result["conditions"]
        for name, total in sorted(inventory["conditions"].items(),
                                  key=lambda kv: (-kv[1], kv[0])):
            line("  condition %-48s %6d  evaluated %d" % (name, total, evaluated.get(name, 0)))
    if result["script_audio"]:
        line("## script audio -> AudioManager::addAudioEvent result")
        for entry in result["script_audio"]:
            line("  frame %-6s %-22s %-34s %s" % (
                entry["frame"], entry["action"], entry["event"], entry["result"]))
    if result["crash"]:
        line("## crash")
        line(result["crash"])
    if result["assertions"]:
        line("## assertions (debug configuration, -ignoreAsserts)")
        for site, entry in sorted(result["assertions"].items(),
                                  key=lambda kv: -kv[1]["count"]):
            line("  %6d  first frame %-6s %s" % (entry["count"], entry["first_frame"], site))
    scripts = result["scripts"]
    fired = [s for s in scripts.values() if s["fired_true"] or s["fired_false"]]
    line("scripts seen: %d, evaluations: %d, fired: %d (true-branch %d, false-branch %d)" % (
        len(scripts), count("script_evaluated"), len(fired),
        sum(s["fired_true"] for s in scripts.values()),
        sum(s["fired_false"] for s in scripts.values())))
    line()
    line("## conditions by type (evaluations)")
    for name, n in sorted(result["conditions"].items(), key=lambda kv: -kv[1]):
        line("  %-48s %d" % (name, n))
    line()
    line("## actions by type (executions)")
    for name, n in sorted(result["actions"].items(), key=lambda kv: -kv[1]):
        line("  %-48s %d" % (name, n))
    line()
    line("## categories")
    conditions, actions = result["conditions"], result["actions"]
    line("  trigger conditions evaluated: %d" % sum(
        n for k, n in conditions.items() if k.startswith(TRIGGER_PREFIXES)))
    for label, prefixes in (("audio", AUDIO_ACTION_PREFIXES), ("video", VIDEO_ACTION_PREFIXES),
                            ("ui", UI_ACTION_PREFIXES)):
        hits = {k: n for k, n in actions.items() if k.startswith(prefixes)}
        line("  %s actions executed: %d  %s" % (label, sum(hits.values()), json.dumps(hits)))
    line("  mission-end actions executed: %d" % sum(
        n for k, n in actions.items() if k in END_ACTIONS))
    line()
    line("## chain links")
    for name, entry in links.items():
        names = entry["names"]
        top = ", ".join("%s x%d" % kv for kv in sorted(names.items(), key=lambda kv: -kv[1])[:6])
        line("  %-30s %6d  first frame %-6s %s" % (name, entry["count"], entry["first_frame"],
                                                   top))
    line()
    line("## timer events")
    for event in result["timer_events"][:60]:
        line("  frame %-6d %-10s %-32s %s" % (event["frame"], event["event"], event["timer"],
                                              event["value"]))
    line()
    line("## first 40 firings")
    for firing in result["firings"][:40]:
        acts = ", ".join(a["type"] for a in firing["actions"])
        line("  frame %-6d %-40s %-6s %s" % (firing["frame"], firing["script"],
                                             firing["branch"], acts))
    if result["forced_end"]:
        line()
        line("## forced end: %s" % json.dumps(result["forced_end"]))
    if result["samples"]:
        last = result["samples"][-1]
        line()
        line("## last sample: frame %s objects %s game_mode %s end_game_timer %s track %r" % (
            last["frame"], last["objects"], last["game_mode"], last["end_game_timer"],
            last["current_track"]))


def launcher_main(argv):
    import argparse
    import subprocess

    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(here)
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--run-dir", required=True,
                        help="disposable dir with the retail .big files and a `zh` symlink")
    parser.add_argument("--binary", default="./zh")
    parser.add_argument("--map", default="Maps\\MD_USA01.map")
    parser.add_argument("--frames", type=int, default=3000,
                        help="stop after this many ScriptEngine::update calls")
    parser.add_argument("--sample-every", type=int, default=100)
    parser.add_argument("--force-end", choices=["none", "victory", "defeat"], default="none",
                        help="synthetic mission end: call ScriptActions::doVictory/doDefeat")
    parser.add_argument("--force-at-frame", type=int, default=600)
    parser.add_argument("--json", default="")
    parser.add_argument("--summary", default="", help="write the text summary here too")
    parser.add_argument("--ffmpeg-lib",
                        default=os.path.join(repo, "build", "docker", "_deps", "ffmpeg-lib",
                                             "lib"))
    parser.add_argument("--gdb", default="gdb")
    parser.add_argument("--timeout", type=int, default=3600, help="seconds, whole gdb run")
    args = parser.parse_args(argv)

    env = dict(os.environ)
    if os.path.isdir(args.ffmpeg_lib):
        env["LD_LIBRARY_PATH"] = args.ffmpeg_lib + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    env.setdefault("ALSOFT_DRIVERS", "null")  # no sound card on CI boxes; OpenAL null backend
    env[ENV_PREFIX + "FRAMES"] = str(args.frames)
    env[ENV_PREFIX + "SAMPLE_EVERY"] = str(args.sample_every)
    env[ENV_PREFIX + "FORCE_END"] = args.force_end
    env[ENV_PREFIX + "FORCE_AT_FRAME"] = str(args.force_at_frame)
    env[ENV_PREFIX + "MAP"] = args.map
    json_path = os.path.abspath(args.json) if args.json else \
        os.path.join(args.run_dir, "campaign-flow-probe.json")
    env[ENV_PREFIX + "JSON"] = json_path

    # -ignoreAsserts: the debug configuration aborts on the first DEBUG_ASSERTCRASH off Windows;
    # the probe records every assertion site instead (see `assertions` in the JSON).
    command = [args.gdb, "-q", "-batch", "-x", os.path.abspath(__file__), "--args",
               args.binary, "-headless", "-ignoreAsserts", "-file", args.map]
    sys.stderr.write("+ %s (cwd %s)\n" % (" ".join(command), args.run_dir))
    try:
        completed = subprocess.run(command, cwd=args.run_dir, env=env, timeout=args.timeout,
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except subprocess.TimeoutExpired:
        sys.stderr.write("gdb run exceeded %ds\n" % args.timeout)
        return 2
    output = completed.stdout.decode("utf-8", "replace")
    marker = [ln for ln in output.splitlines() if ln.startswith("CAMPAIGN_PROBE_RESULT")]
    if not marker or not os.path.exists(json_path):
        sys.stderr.write(output[-6000:])
        sys.stderr.write("\nprobe produced no result\n")
        return 1
    with open(json_path) as handle:
        result = json.load(handle)
    summarize(result, sys.stdout)
    if args.summary:
        with open(args.summary, "w") as handle:
            summarize(result, handle)
    sys.stdout.write("\njson: %s\n" % json_path)
    return 0


if __name__ == "__main__":
    if gdb is not None:
        gdb_main()
    else:
        sys.exit(launcher_main(sys.argv[1:]))

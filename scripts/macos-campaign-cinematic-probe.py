#!/usr/bin/env python3
"""Reproduce "after the USA-01 train cinematic the camera kept scrolling left".

Launches the real binary under LLDB from a run directory (retail data symlinked in, nothing copied
into the repo) with a private copy of the user-data directory, clicks Single Player -> USA -> Easy
with real OS events and lets the mission's intro run to its `ENABLE_INPUT`.

USA-01's second cinematic (`CINE_Train`) starts when the player has filled both train cars at the
depot: the `Check the Train` subroutine sets the flag `Go Train Go`, `Leave The Depot` enables
`CINE_TRSetup`, which runs `DISABLE_INPUT`, and `CINE_TRReturnToPlayer` runs `ENABLE_INPUT` about
40 s later. Loading the cars needs a player, so the probe sets `Go Train Go` itself (and, if the
train has not reached the depot, starts the expired `Time_To_Wait_At_Station` timer `CINE_TRSetup`
waits on). Those two writes are the only engine state it changes; the cinematic, its input
disable/enable and every key and mouse event are the game's own.

`--hold-key left` plays the player who is scrolling with an arrow key when the train leaves: the
key goes down (a real key event) while input is enabled, stays down across `DISABLE_INPUT`, and
comes up during the cinematic. After `ENABLE_INPUT` the probe samples the camera idle, then taps
Right, Up and finally Left, sampling after each, so a drift is a number per phase.

Read in every sample: `TheTacticalView->m_pos` / `m_mouseLocked` / `m_cameraLock`,
`TheLookAtTranslator->m_isScrolling` / `m_scrollType`, LookAtXlat.cpp's `scrollDir[]`,
`TheInGameUI->m_isScrolling` / `m_inputEnabled`, `TheMouse`'s position, visibility and capture,
and the keyboard's arrow-key state. Hits of the scroll start/stop functions are recorded with the
same state and a backtrace. See docs/porting/campaign-cinematic-camera-drift.md.

The binary must carry `com.apple.security.get-task-allow` (codesign -s - -f --entitlements ...).

Usage:
    python3 scripts/macos-campaign-cinematic-probe.py --run-dir ~/devin-work/cine-drift/run \\
        --user-data-src ~/devin-work/cine-drift/ud-src --hold-key left --out /tmp/held.json
"""

import argparse
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import time
import traceback
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
XCODE_PYTHON = Path("/Applications/Xcode.app/Contents/Developer/usr/bin/python3")
USER_DATA_LEAF = "Command and Conquer Generals Zero Hour Data"

# Main menu -> Single Player -> USA -> Easy at the WND files' 800x600 creation resolution
# (docs/porting/real-input-menu-drive.md section 3); the layouts scale with the resolution.
CAMPAIGN_ROUTE = [("ButtonSinglePlayer", 644, 134), ("ButtonUSA", 644, 134),
                  ("ButtonEasy", 644, 173)]
REFERENCE_RESOLUTION = (800, 600)
GAME_SINGLE_PLAYER = 0

# macOS virtual keycodes of the arrow keys.
ARROW_KEYS = {"left": 123, "right": 124, "down": 125, "up": 126}

STATE_EXPRESSIONS = {
    "frame": "(int)((W3DGameLogic*)TheGameLogic)->m_frame",
    "game_mode": "(int)((W3DGameLogic*)TheGameLogic)->m_gameMode",
    "camera_x": "(int)(((W3DView*)TheTacticalView)->m_pos.x * 100)",
    "camera_y": "(int)(((W3DView*)TheTacticalView)->m_pos.y * 100)",
    "camera_lock": "(int)((W3DView*)TheTacticalView)->m_cameraLock",
    "view_mouse_locked": "(int)TheTacticalView->m_mouseLocked",
    "lookat_is_scrolling": "(int)TheLookAtTranslator->m_isScrolling",
    "lookat_scroll_type": "(int)TheLookAtTranslator->m_scrollType",
    "ingameui_is_scrolling": "(int)((W3DInGameUI*)TheInGameUI)->m_isScrolling",
    "ingameui_input_enabled": "(int)((W3DInGameUI*)TheInGameUI)->m_inputEnabled",
    "mouse_x": "TheMouse->m_currMouse.pos.x",
    "mouse_y": "TheMouse->m_currMouse.pos.y",
    "mouse_visible": "(int)TheMouse->m_visible",
    "mouse_captured": "(int)TheMouse->m_isCursorCaptured",
    "key_left_state": "(int)TheKeyboard->m_keyStatus[0xCB].state",
    "key_right_state": "(int)TheKeyboard->m_keyStatus[0xCD].state",
}

# LookAtXlat.cpp's `static Bool scrollDir[4]`, which -O2 can split into four byte globals that LLDB
# cannot evaluate by name; read by address (scroll_dir_expressions).
SCROLL_DIRECTIONS = ("up", "down", "left", "right")

RECORDED_FUNCTIONS = ["LookAtTranslator::resetModes", "LookAtTranslator::setScrolling",
                      "LookAtTranslator::stopScrolling"]

# The script engine's flags and timers are named slots; a name's characters follow m_data.
SCRIPT_SLOT = """
int found = -1;
for (int i = 0; i < TheScriptEngine->m_num%(kind)s; ++i) {
  if (TheScriptEngine->m_%(field)s[i].name.m_data &&
      (int)strcmp((const char*)(TheScriptEngine->m_%(field)s[i].name.m_data + 1), "%(name)s") == 0)
    found = i;
}
found;
"""


def scroll_dir_expressions(target):
    def load_address(name):
        symbols = target.FindSymbols(name)
        if symbols.GetSize() != 1:
            return None
        return symbols[0].GetSymbol().GetStartAddress().GetLoadAddress(target)

    whole = load_address("_ZL9scrollDir")
    expressions = {}
    for index, direction in enumerate(SCROLL_DIRECTIONS):
        address = whole + index if whole is not None else load_address("_ZL9scrollDir.%d" % index)
        if address is None:
            raise RuntimeError("no scrollDir symbol for index %d" % index)
        expressions["scroll_dir_" + direction] = "(int)*(unsigned char*)%#x" % address
    return expressions


def require_lldb():
    try:
        import lldb  # noqa: F401
        return
    except ImportError:
        pass
    if os.environ.get("CINEMATIC_PROBE_REEXEC"):
        sys.exit("no lldb python module, even under %s" % sys.executable)
    lldb_path = subprocess.run(["lldb", "-P"], capture_output=True, text=True,
                               check=True).stdout.strip()
    environment = dict(os.environ, PYTHONPATH=lldb_path, CINEMATIC_PROBE_REEXEC="1")
    interpreter = str(XCODE_PYTHON) if XCODE_PYTHON.exists() else sys.executable
    os.execve(interpreter, [interpreter, os.path.abspath(__file__)] + sys.argv[1:], environment)


def load_module(name, filename):
    spec = importlib.util.spec_from_file_location(name, REPO / "scripts" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def other_games():
    # By process name: a pattern over full command lines also matches the shell that started this.
    found = subprocess.run(["pgrep", "-lx", "zh"], capture_output=True, text=True)
    return found.stdout.strip()


def private_user_data(source, out_dir):
    """A copy of the user-data directory's settings under the report's directory, so the game
    never writes the real one. -> the CNC_USER_DATA root."""
    root = out_dir / "userdata"
    if root.exists():
        shutil.rmtree(root)
    (root / USER_DATA_LEAF).mkdir(parents=True)
    source_leaf = Path(source).expanduser() / USER_DATA_LEAF
    for name in ("Options.ini", "Skirmish.ini"):
        if (source_leaf / name).is_file():
            shutil.copy2(source_leaf / name, root / USER_DATA_LEAF / name)
    return root


class Probe:
    def __init__(self, session, drive, resolution):
        self.session = session
        self.drive = drive
        self.resolution = resolution

    def state(self, reader):
        return {name: reader.integer(expression) for name, expression in STATE_EXPRESSIONS.items()}

    def snapshot(self):
        return self.session.stopped(self.state)

    def client_point(self, client_x, client_y):
        window = self.drive.game_window(self.session.pid)
        return self.drive.client_to_global(window, client_x, client_y, self.resolution[1])

    def move(self, client_x, client_y):
        self.drive.post_click(*self.client_point(client_x, client_y), move_only=True)

    def click(self, client_x, client_y):
        self.drive.activate_through_accessibility(self.session.pid)
        time.sleep(0.3)
        self.drive.post_click(*self.client_point(client_x, client_y))

    def key(self, name, down):
        event = self.drive._create_keyboard_event(None, ARROW_KEYS[name], down)
        self.drive._post_event(self.drive.KCG_HID_EVENT_TAP, event)
        self.drive._cfrelease(event)

    def tap(self, name):
        self.key(name, True)
        time.sleep(0.15)
        self.key(name, False)

    def wait_for_input(self, enabled, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            snap = self.snapshot()
            if snap["ingameui_input_enabled"] == int(enabled):
                return snap
            if self.session.exited:
                break
            time.sleep(1.0)
        raise RuntimeError("input never became %s within %.0f s" % (enabled, timeout))

    def sample(self, seconds, interval=0.5):
        """Snapshots over `seconds`, plus how far the camera pivot moved, in world units."""
        samples = []
        deadline = time.time() + seconds
        while time.time() < deadline:
            samples.append(self.snapshot())
            time.sleep(interval)
        first, last = samples[0], samples[-1]
        moved = {"dx": (last["camera_x"] - first["camera_x"]) / 100.0,
                 "dy": (last["camera_y"] - first["camera_y"]) / 100.0,
                 "frames": last["frame"] - first["frame"],
                 "scrolling_at_end": last["lookat_is_scrolling"],
                 "scroll_dir_left_at_end": last["scroll_dir_left"]}
        return {"camera_moved": moved, "samples": samples}

    def script_slot(self, kind, field, name):
        return self.session.stopped(lambda reader: reader.integer(
            SCRIPT_SLOT % {"kind": kind, "field": field, "name": name}))

    def start_train_cinematic(self):
        """Set `Go Train Go`; start `Time_To_Wait_At_Station` expired if the train has not."""
        flag = self.script_slot("Flags", "flags", "Go Train Go")
        timer = self.script_slot("Counters", "counters", "Time_To_Wait_At_Station")
        if flag is None or flag < 0:
            raise RuntimeError("no 'Go Train Go' flag in the script engine")

        def work(reader):
            before = {"flag_slot": flag, "timer_slot": timer}
            if timer is not None and timer >= 0:
                slot = "TheScriptEngine->m_counters[%d]" % timer
                before["timer_value"] = reader.integer("%s.value" % slot)
                before["timer_started"] = reader.integer("(int)%s.isCountdownTimer" % slot)
                if not before["timer_started"]:
                    reader.integer("(int)(%s.isCountdownTimer = 1)" % slot)
                    reader.integer("%s.value = -1" % slot)
            else:
                before["timer_missing"] = True
            reader.integer("(int)(TheScriptEngine->m_flags[%d].value = 1)" % flag)
            return before
        return self.session.stopped(work)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--executable", default="zh")
    parser.add_argument("--user-data-src", type=Path, required=True,
                        help="directory holding '%s' (Options.ini is copied)" % USER_DATA_LEAF)
    parser.add_argument("--hold-key", choices=("left", "none"), default="left",
                        help="left: the Left arrow is held as the train cinematic disables input "
                             "and released during it; none: the control")
    parser.add_argument("--xres", type=int, default=1728)
    parser.add_argument("--yres", type=int, default=1117)
    parser.add_argument("--windowed", action="store_true")
    parser.add_argument("--settle", type=float, default=35.0, help="launch to main menu, seconds")
    parser.add_argument("--intro-timeout", type=float, default=900.0,
                        help="seconds to wait for the intro's ENABLE_INPUT")
    parser.add_argument("--out", type=Path, required=True, help="JSON report (outside the repo)")
    args = parser.parse_args()
    if sys.platform != "darwin":
        raise SystemExit("this probe drives the Cocoa build and needs macOS")
    running = other_games()
    if running:
        raise SystemExit("refusing to start: a game is already running:\n%s" % running)

    require_lldb()
    import lldb
    game_end = load_module("macos_game_end_mouse_lock_probe", "macos-game-end-mouse-lock-probe.py")
    # Its recorded breakpoint hits read this probe's state, camera included.
    game_end.STATE_EXPRESSIONS = STATE_EXPRESSIONS
    drive = load_module("macos_input_drive", "macos-input-drive.py")
    run_dir = args.run_dir.expanduser().resolve()
    out_dir = args.out.resolve().parent
    out_dir.mkdir(parents=True, exist_ok=True)
    # LLDB's launched process inherits this environment.
    os.environ["CNC_USER_DATA"] = str(private_user_data(args.user_data_src, out_dir))
    resolution = (args.xres, args.yres)
    arguments = ["-nologo", "-xres", str(args.xres), "-yres", str(args.yres)]
    if args.windowed:
        arguments.insert(0, "-win")
    session = game_end.GameSession(lldb, drive, run_dir / args.executable, run_dir, arguments,
                                   args.out.with_suffix(".stderr.log"))
    STATE_EXPRESSIONS.update(scroll_dir_expressions(session.target))
    probe = Probe(session, drive, resolution)
    report = {"arguments": arguments, "hold_key": args.hold_key, "pid": session.pid,
              "phases": {}}
    phases = report["phases"]

    def save():
        report["records"] = session.records
        args.out.write_text(json.dumps(report, indent=2) + "\n")

    status = 1
    try:
        time.sleep(args.settle)
        phases["main_menu"] = game_end.wait_for(
            session, lambda snap: (snap.get("shell_top") or "").endswith("MainMenu.wnd"), 120)
        for _name, reference_x, reference_y in CAMPAIGN_ROUTE:
            probe.click(reference_x * args.xres / REFERENCE_RESOLUTION[0],
                        reference_y * args.yres / REFERENCE_RESOLUTION[1])
            time.sleep(4.0)
        phases["mission_started"] = game_end.wait_for(
            session, lambda snap: snap["game_mode"] == GAME_SINGLE_PLAYER
            and (snap["frame"] or 0) > 30, 300)
        centre = (args.xres // 2, args.yres // 2)
        probe.move(*centre)
        phases["intro_disabled"] = probe.wait_for_input(False, 60)
        phases["intro_enabled"] = probe.wait_for_input(True, args.intro_timeout)
        save()
        for function in RECORDED_FUNCTIONS:
            report.setdefault("breakpoint_locations", {})[function] = \
                session.record_function(function)

        if args.hold_key == "left":
            probe.key("left", True)
            phases["key_held_before_cinematic"] = probe.sample(2.0)
        phases["forced"] = probe.start_train_cinematic()
        phases["train_disabled"] = probe.wait_for_input(False, 60)
        if args.hold_key == "left":
            time.sleep(2.0)
            probe.key("left", False)
            time.sleep(1.0)
            phases["key_released_during_cinematic"] = probe.snapshot()
        save()

        phases["train_enabled"] = probe.wait_for_input(True, 180)
        phases["after_enable_idle"] = probe.sample(4.0)
        probe.tap("right")
        phases["after_right_tap"] = probe.sample(4.0)
        probe.tap("up")
        phases["after_up_tap"] = probe.sample(4.0)
        probe.move(*centre)
        phases["after_pointer_centred"] = probe.sample(3.0)
        probe.tap("left")
        phases["after_left_tap"] = probe.sample(4.0)
        report["summary"] = {name: phase["camera_moved"] for name, phase in phases.items()
                             if isinstance(phase, dict) and "camera_moved" in phase}
        print(json.dumps({"hold_key": args.hold_key, "summary": report["summary"]}, indent=2))
        status = 0
    except Exception:  # recorded in the report: os._exit below would otherwise swallow it
        report["error"] = traceback.format_exc()
        sys.stderr.write(report["error"])
        status = 1
    finally:
        save()
        session.kill()
        sys.stdout.flush()
        sys.stderr.flush()
        os._exit(status)


if __name__ == "__main__":
    main()

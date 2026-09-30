#!/usr/bin/env python3
"""Reproduce "the score screen's buttons are dead after a skirmish that ended naturally".

Launches the real binary under LLDB from a run directory (retail data symlinked in, nothing copied
into the repo), clicks Single Player -> Skirmish -> Play Game with real OS events, and then ends the
skirmish through the game's own end-of-game path: at the top of a `GameLogic::update` every
computer player except the neutral one is `killPlayer()`ed, so the skirmish scripts' victory
condition fires `ScriptActions::doVictory` on their own. The kill is the only engine call the probe
makes; nothing on the end-of-game path is invoked by it.

`--scroll edge` parks the pointer on the right screen edge before the end, so the game is edge
scrolling when it ends, and moves it back to the centre once `doVictory` has run -- what a player
does when the victory banner appears. `--scroll none` is the control. On the score screen the probe
clicks `ButtonOk` (found in the live GameWindow tree) and reports whether the shell moved on, and
how many times `GameWindowManager::winProcessMouseEvent` ran during the click: `WindowTranslator`
returns before calling it when it drops a message.

Read at each phase: `TheTacticalView->m_mouseLocked`, `TheLookAtTranslator->m_isScrolling` /
`m_scrollType`, `TheInGameUI->m_isScrolling` / `m_inputEnabled`, and `TheWindowManager`'s
`m_modalHead` / `m_grabWindow` / `m_mouseCaptor`. A hardware watchpoint records every write to
`m_mouseLocked` with its backtrace. See docs/porting/game-end-mouse-lock.md.

The binary must carry `com.apple.security.get-task-allow` (codesign -s - -f --entitlements ...).

Usage:
    python3 scripts/macos-game-end-mouse-lock-probe.py --run-dir ~/devin-work/fs-scale/run \\
        --executable zh --scroll edge --out /tmp/edge.json
"""

import argparse
import importlib.util
import json
import os
import signal
import subprocess
import sys
import threading
import time
import traceback
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
XCODE_PYTHON = Path("/Applications/Xcode.app/Contents/Developer/usr/bin/python3")

# Main menu -> Single Player -> Skirmish -> Play Game at the WND files' 800x600 creation resolution;
# the layouts scale linearly with the engine's resolution (macos-fullscreen-scale-probe.py).
SKIRMISH_ROUTE = [("ButtonSinglePlayer", 644, 134), ("ButtonSkirmish", 644, 294),
                  ("ButtonStart", 180.5, 530.5)]
REFERENCE_RESOLUTION = (800, 600)
GAME_SKIRMISH = 2
PLAYER_COMPUTER = 1

STATE_EXPRESSIONS = {
    "view_mouse_locked": "(int)TheTacticalView->m_mouseLocked",
    "lookat_is_scrolling": "(int)TheLookAtTranslator->m_isScrolling",
    "lookat_scroll_type": "(int)TheLookAtTranslator->m_scrollType",
    "ingameui_is_scrolling": "(int)((W3DInGameUI*)TheInGameUI)->m_isScrolling",
    "ingameui_input_enabled": "(int)((W3DInGameUI*)TheInGameUI)->m_inputEnabled",
    "wm_modal_head": "(long)((W3DGameWindowManager*)TheWindowManager)->m_modalHead",
    "wm_grab_window": "(long)((W3DGameWindowManager*)TheWindowManager)->m_grabWindow",
    "wm_mouse_captor": "(long)((W3DGameWindowManager*)TheWindowManager)->m_mouseCaptor",
    "mouse_x": "TheMouse->m_currMouse.pos.x",
    "mouse_y": "TheMouse->m_currMouse.pos.y",
    "mouse_captured": "(int)TheMouse->m_isCursorCaptured",
    "game_mode": "(int)((W3DGameLogic*)TheGameLogic)->m_gameMode",
    "frame": "(int)((W3DGameLogic*)TheGameLogic)->m_frame",
}

KILL_COMPUTER_PLAYERS = """
int killed = 0;
for (int i = 1; i < ThePlayerList->m_playerCount; ++i) {
  Player *player = ThePlayerList->m_players[i];
  if (player && player != ThePlayerList->m_local && (int)player->m_playerType == %d) {
    player->killPlayer();
    ++killed;
  }
}
killed;
""" % PLAYER_COMPUTER

# Breakpoints whose hits are recorded, with the state, and resumed.
RECORDED_FUNCTIONS = ["ScriptActions::doVictory", "ScriptActions::doDefeat",
                      "LookAtTranslator::resetModes", "LookAtTranslator::stopScrolling",
                      "LookAtTranslator::setScrolling", "InGameUI::reset"]


def require_lldb():
    try:
        import lldb  # noqa: F401
        return
    except ImportError:
        pass
    if os.environ.get("GAME_END_PROBE_REEXEC"):
        sys.exit("no lldb python module, even under %s" % sys.executable)
    lldb_path = subprocess.run(["lldb", "-P"], capture_output=True, text=True,
                               check=True).stdout.strip()
    environment = dict(os.environ, PYTHONPATH=lldb_path, GAME_END_PROBE_REEXEC="1")
    interpreter = str(XCODE_PYTHON) if XCODE_PYTHON.exists() else sys.executable
    os.execve(interpreter, [interpreter, os.path.abspath(__file__)] + sys.argv[1:], environment)


def load_input_drive():
    spec = importlib.util.spec_from_file_location(
        "macos_input_drive", REPO / "scripts" / "macos-input-drive.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class GameSession:
    """The game launched under LLDB in async mode. A pump thread consumes process events: a stop on
    a recorded breakpoint or watchpoint is logged with the state and resumed; a stop the probe asked
    for (`stopped()`) is parked until the probe resumes it."""

    def __init__(self, lldb, drive, binary, run_dir, arguments, log_path):
        self.lldb = lldb
        self.drive = drive
        self.debugger = lldb.SBDebugger.Create()
        self.debugger.SetAsync(True)
        self.target = self.debugger.CreateTarget(str(binary))
        self.listener = self.debugger.GetListener()
        launch = lldb.SBLaunchInfo(arguments)
        launch.SetWorkingDirectory(str(run_dir))
        launch.AddOpenFileAction(1, str(log_path), False, True)
        launch.AddOpenFileAction(2, str(log_path), False, True)
        error = lldb.SBError()
        self.process = self.target.Launch(launch, error)
        if not self.process.IsValid() or error.Fail():
            raise RuntimeError("launch failed: %s" % error)
        self.pid = self.process.GetProcessID()
        self.records = []
        self.recorders = {}      # breakpoint id -> label
        self.watch_ids = set()
        self.actions = {}        # breakpoint id -> callable(reader), run once, then deleted
        self.want_stop = False
        self.parked = threading.Event()
        self.exited = False
        self.pump_thread = threading.Thread(target=self.pump, daemon=True)
        self.pump_thread.start()

    # ---- event pump -------------------------------------------------------------------------
    def pump(self):
        lldb = self.lldb
        event = lldb.SBEvent()
        while not self.exited:
            if not self.listener.WaitForEvent(1, event):
                continue
            if not lldb.SBProcess.EventIsProcessEvent(event):
                continue
            state = lldb.SBProcess.GetStateFromEvent(event)
            if state in (lldb.eStateExited, lldb.eStateCrashed, lldb.eStateDetached):
                self.exited = True
                self.records.append({"event": "process-ended", "state": state,
                                     "exit_status": self.process.GetExitStatus()})
                self.parked.set()
                return
            if state != lldb.eStateStopped or lldb.SBProcess.GetRestartedFromEvent(event):
                continue
            crashed = self.handle_stop()
            if crashed:
                self.parked.set()
                return
            if self.want_stop:
                self.parked.set()
            else:
                self.process.Continue()

    def handle_stop(self):
        """Record breakpoint/watchpoint hits and run one-shot actions. -> True on a crash stop."""
        lldb = self.lldb
        for thread in self.process:
            reason = thread.GetStopReason()
            if reason == lldb.eStopReasonException or (
                    reason == lldb.eStopReasonSignal and thread.GetStopReasonDataAtIndex(0) != 17):
                self.records.append({"event": "crash",
                                     "description": thread.GetStopDescription(256),
                                     "backtrace": self.backtrace(thread, 20)})
                return True
            if reason == lldb.eStopReasonBreakpoint:
                breakpoint_id = thread.GetStopReasonDataAtIndex(0)
                self.process.SetSelectedThread(thread)
                if breakpoint_id in self.actions:
                    action = self.actions.pop(breakpoint_id)
                    self.target.BreakpointDelete(breakpoint_id)
                    self.records.append({"event": "action", "result": action(self.reader()),
                                         "state": self.state()})
                elif breakpoint_id in self.recorders:
                    self.records.append({"event": "breakpoint",
                                         "function": self.recorders[breakpoint_id],
                                         "state": self.state(),
                                         "backtrace": self.backtrace(thread, 8)})
            elif reason == lldb.eStopReasonWatchpoint:
                self.process.SetSelectedThread(thread)
                self.records.append({"event": "watchpoint-write",
                                     "state": self.state(),
                                     "backtrace": self.backtrace(thread, 10)})
        return False

    @staticmethod
    def backtrace(thread, depth):
        return [str(thread.GetFrameAtIndex(index).GetFunctionName())
                for index in range(min(depth, thread.GetNumFrames()))]

    # ---- reading ----------------------------------------------------------------------------
    def reader(self):
        return self.drive.EngineReader.over_stopped_process(self.lldb, self.target, self.process)

    def state(self):
        reader = self.reader()
        return {name: reader.integer(expression) for name, expression in STATE_EXPRESSIONS.items()}

    def stopped(self, work):
        """Stop the process, run `work(reader)`, resume. -> what `work` returned."""
        self.parked.clear()
        self.want_stop = True
        self.process.Stop()
        if not self.parked.wait(20):
            raise RuntimeError("the process did not stop within 20 s")
        if self.exited:
            raise RuntimeError("the process ended: %s" % self.records[-1])
        main_thread = self.process.GetThreadAtIndex(0)
        self.process.SetSelectedThread(main_thread)
        try:
            return work(self.reader())
        finally:
            self.want_stop = False
            self.process.Continue()

    def snapshot(self):
        def work(reader):
            state = self.state()
            count = reader.integer("TheShell->m_screenCount") or 0
            state["shell_top"] = reader.text(
                "(const char*)TheShell->m_screenStack[%d]->m_filenameString.str()" % (count - 1)
            ) if count else None
            return state
        return self.stopped(work)

    # ---- instrumentation --------------------------------------------------------------------
    def record_function(self, name):
        breakpoint = self.target.BreakpointCreateByName(name)
        self.recorders[breakpoint.GetID()] = name
        return breakpoint.GetNumLocations()

    def run_at(self, function, action):
        breakpoint = self.target.BreakpointCreateByName(function)
        breakpoint.SetOneShot(True)
        self.actions[breakpoint.GetID()] = action

    def watch_mouse_lock(self):
        def work(reader):
            address = reader.integer("(long)&TheTacticalView->m_mouseLocked")
            error = self.lldb.SBError()
            watchpoint = self.target.WatchAddress(address, 1, False, True, error)
            return {"address": hex(address or 0), "ok": watchpoint.IsValid(),
                    "error": str(error) if error.Fail() else None}
        return self.stopped(work)

    def count_calls(self, function, seconds, during):
        """Hits of `function` while `during()` runs and `seconds` pass afterwards."""
        def arm(reader):
            breakpoint = self.target.BreakpointCreateByName(function)
            breakpoint.SetAutoContinue(True)
            return breakpoint.GetID()
        breakpoint_id = self.stopped(arm)
        during()
        time.sleep(seconds)

        def disarm(reader):
            breakpoint = self.target.FindBreakpointByID(breakpoint_id)
            hits = breakpoint.GetHitCount()
            self.target.BreakpointDelete(breakpoint_id)
            return hits
        return self.stopped(disarm)

    def kill(self):
        # SBProcess.Kill() can wait forever on an event the pump thread has already consumed.
        self.exited = True
        os.kill(self.pid, signal.SIGKILL)


def client_point(drive, pid, client_x, client_y, resolution):
    window = drive.game_window(pid)
    return drive.client_to_global(window, client_x, client_y, resolution[1])


def post_move(drive, pid, client_x, client_y, resolution):
    drive.activate_through_accessibility(pid)
    time.sleep(0.3)
    drive.post_click(*client_point(drive, pid, client_x, client_y, resolution), move_only=True)


def post_click(drive, pid, client_x, client_y, resolution):
    drive.activate_through_accessibility(pid)
    time.sleep(0.3)
    drive.post_click(*client_point(drive, pid, client_x, client_y, resolution))


def wait_for(session, predicate, timeout, interval=3.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        snapshot = session.snapshot()
        if predicate(snapshot):
            return snapshot
        time.sleep(interval)
    raise RuntimeError("timed out after %.0f s; last snapshot %s" % (timeout, snapshot))


def find_window(session, name):
    def work(reader):
        for window in reader.named_windows(top_layout_only=True):
            if window["name"] == name and window["hidden"] != "true":
                return window
        return None
    return session.stopped(work)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--executable", default="zh")
    parser.add_argument("--scroll", choices=("edge", "none"), required=True,
                        help="edge: the pointer sits on the right screen edge as the game ends")
    parser.add_argument("--xres", type=int, default=1728)
    parser.add_argument("--yres", type=int, default=1117)
    parser.add_argument("--windowed", action="store_true")
    parser.add_argument("--settle", type=float, default=35.0, help="launch to main menu, seconds")
    parser.add_argument("--out", type=Path, required=True, help="JSON report (outside the repo)")
    args = parser.parse_args()
    if sys.platform != "darwin":
        raise SystemExit("this probe drives the Cocoa build and needs macOS")
    require_lldb()
    import lldb

    drive = load_input_drive()
    run_dir = args.run_dir.expanduser().resolve()
    resolution = (args.xres, args.yres)
    arguments = ["-nologo", "-xres", str(args.xres), "-yres", str(args.yres)]
    if args.windowed:
        arguments.insert(0, "-win")
    log_path = args.out.with_suffix(".stderr.log")
    session = GameSession(lldb, drive, run_dir / args.executable, run_dir, arguments, log_path)
    report = {"arguments": arguments, "scroll": args.scroll, "pid": session.pid, "phases": {}}
    phases = report["phases"]

    def save():
        report["records"] = session.records
        args.out.write_text(json.dumps(report, indent=2) + "\n")

    status = 1
    try:
        time.sleep(args.settle)
        phases["main_menu"] = wait_for(
            session, lambda snap: (snap.get("shell_top") or "").endswith("MainMenu.wnd"), 120)
        for name, reference_x, reference_y in SKIRMISH_ROUTE:
            post_click(drive, session.pid,
                       reference_x * args.xres / REFERENCE_RESOLUTION[0],
                       reference_y * args.yres / REFERENCE_RESOLUTION[1], resolution)
            time.sleep(4.0)
        in_game = wait_for(session, lambda snap: snap["game_mode"] == GAME_SKIRMISH
                           and (snap["frame"] or 0) > 150, 180)
        phases["in_game"] = in_game
        save()

        for function in RECORDED_FUNCTIONS:
            report.setdefault("breakpoint_locations", {})[function] = \
                session.record_function(function)
        report["watchpoint"] = session.watch_mouse_lock()
        if args.scroll == "edge":
            post_move(drive, session.pid, args.xres - 1, args.yres // 2, resolution)
        else:
            post_move(drive, session.pid, args.xres // 2, args.yres // 2, resolution)
        time.sleep(2.0)
        phases["before_end"] = session.snapshot()
        session.run_at("GameLogic::update", lambda reader: reader.integer(KILL_COMPUTER_PLAYERS))
        save()

        deadline = time.time() + 120
        while not any(record.get("function") in ("ScriptActions::doVictory",
                                                 "ScriptActions::doDefeat")
                      for record in session.records):
            if time.time() > deadline or session.exited:
                raise RuntimeError("no doVictory/doDefeat within 120 s")
            time.sleep(1.0)
        time.sleep(2.0)
        phases["after_end_pointer_at_start"] = session.snapshot()
        # The player moves the mouse off the edge while the victory banner is up.
        post_move(drive, session.pid, args.xres // 2, args.yres // 2, resolution)
        time.sleep(1.5)
        phases["after_end_pointer_centred"] = session.snapshot()
        save()

        phases["score_screen"] = wait_for(
            session, lambda snap: (snap.get("shell_top") or "").endswith("ScoreScreen.wnd"), 180)
        time.sleep(3.0)
        phases["score_screen_settled"] = session.snapshot()
        button = find_window(session, "ButtonOk")
        report["button_ok"] = button
        if button is None:
            raise RuntimeError("no visible ButtonOk on the score screen")
        centre = (button["x"] + button["w"] // 2, button["y"] + button["h"] // 2)
        report["click"] = {"client": centre}
        report["click"]["winProcessMouseEvent_calls"] = session.count_calls(
            "GameWindowManager::winProcessMouseEvent", 3.0,
            lambda: post_click(drive, session.pid, centre[0], centre[1], resolution))
        phases["after_click"] = session.snapshot()
        report["click"]["shell_moved_on"] = not (
            phases["after_click"].get("shell_top") or "").endswith("ScoreScreen.wnd")
        print(json.dumps({"scroll": args.scroll, "phases": phases, "click": report["click"]},
                         indent=2))
        status = 0
    except Exception:  # recorded in the report: os._exit below would otherwise swallow it
        report["error"] = traceback.format_exc()
        sys.stderr.write(report["error"])
        status = 1
    finally:
        save()
        session.kill()
        # The LLDB listener thread can keep the interpreter alive after the game is gone.
        sys.stdout.flush()
        sys.stderr.flush()
        os._exit(status)


if __name__ == "__main__":
    main()

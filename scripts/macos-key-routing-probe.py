#!/usr/bin/env python3
"""Measure what Escape and the arrow keys do in a live skirmish, windowed or fullscreen.

User report (M1 Pro): the macOS alert sound plays on Escape and on the arrow keys, and in
fullscreen Escape does not open the pause menu. This launches the real binary under LLDB from a
run directory (retail data symlinked in, nothing copied into the repo), clicks Single Player ->
Skirmish -> Play Game with real OS events, then posts real `CGEventPost` key events (HID tap,
synthetic: not a human at the keyboard) and records, per key:

- hits on `NSBeep`, `-[NSResponder noResponderFor:]` and `-[NSWindow cancelOperation:]` (the AppKit
  path that plays the alert sound) and on `ToggleQuitMenu`, by auto-continuing breakpoints;
- engine state: `quit_menu_visible`, `TheGameEngine->m_isActive`, the seam's `Window_Is_Active`, the
  tactical camera's pivot (arrow scrolling moves it);
- AppKit state: whether the game window is key, and the class of its first responder.

The binary must carry `com.apple.security.get-task-allow` (codesign -s - -f --entitlements ...).
See docs/porting/cocoa-key-routing.md.

Usage:
    python3 scripts/macos-key-routing-probe.py --run-dir ~/devin-work/fs-scale/run \\
        --executable zh [--windowed --xres 1024 --yres 768] --out /tmp/keys.json
"""

import argparse
import importlib.util
import json
import os
import subprocess
import sys
import time
import traceback
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
XCODE_PYTHON = Path("/Applications/Xcode.app/Contents/Developer/usr/bin/python3")
KEY_ESCAPE = 53
ARROWS = {"left": 123, "right": 124, "down": 125, "up": 126}
BEEP_FUNCTIONS = ["NSBeep", "-[NSResponder noResponderFor:]",
                  "-[NSWindow(NSEventRouting) cancelOperation:]", "ToggleQuitMenu"]

ENGINE_EXPRESSIONS = {
    "quit_menu_visible": "(int)((W3DInGameUI*)TheInGameUI)->m_isQuitMenuVisible",
    "engine_is_active": "(int)TheGameEngine->m_isActive",
    "seam_window_is_active": "(int)WWPlatform::Window_Is_Active(WWPlatform::Window_Current())",
    "camera_x": "(double)TheTacticalView->m_pos.x",
    "camera_y": "(double)TheTacticalView->m_pos.y",
    "game_mode": "(int)((W3DGameLogic*)TheGameLogic)->m_gameMode",
    "frame": "(int)((W3DGameLogic*)TheGameLogic)->m_frame",
}

# Evaluated as Objective-C++. The binary has no AppKit debug info, so every receiver is cast.
APPKIT_EXPRESSIONS = {
    "app_is_active": "(int)(BOOL)[(id)NSApp isActive]",
    "key_window_class": "(const char *)object_getClassName((id)[(id)NSApp keyWindow])",
    "first_responder_class": "(const char *)object_getClassName("
                             "(id)[(id)[(id)NSApp keyWindow] firstResponder])",
}


def require_lldb():
    """Re-exec this script under a Python that can import LLDB's module, as the other probes do."""
    try:
        import lldb  # noqa: F401
        return
    except ImportError:
        pass
    if os.environ.get("KEY_ROUTING_PROBE_REEXEC"):
        sys.exit("no lldb python module, even under %s" % sys.executable)
    lldb_path = subprocess.run(["lldb", "-P"], capture_output=True, text=True,
                               check=True).stdout.strip()
    environment = dict(os.environ, PYTHONPATH=lldb_path, KEY_ROUTING_PROBE_REEXEC="1")
    interpreter = str(XCODE_PYTHON) if XCODE_PYTHON.exists() else sys.executable
    os.execve(interpreter, [interpreter, os.path.abspath(__file__)] + sys.argv[1:], environment)


def load_module(name, filename):
    spec = importlib.util.spec_from_file_location(name, REPO / "scripts" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_state(lldb, session, reader):
    state = {}
    for name, expression in ENGINE_EXPRESSIONS.items():
        value = reader.value(expression)
        state[name] = None if value is None else value.GetValue()
    options = lldb.SBExpressionOptions()
    options.SetLanguage(lldb.eLanguageTypeObjC_plus_plus)
    frame = session.process.GetThreadAtIndex(0).GetFrameAtIndex(0)
    for name, expression in APPKIT_EXPRESSIONS.items():
        value = frame.EvaluateExpression(expression, options)
        if value.GetError().Fail():
            state[name] = "error: %s" % value.GetError().GetCString()
        else:
            state[name] = (value.GetSummary() or value.GetValue() or "").strip('"')
    return state


def hold_key(drive, pid, virtual_key, seconds):
    """Activate, then a real key down, a hold and a key up (arrow scrolling needs the hold)."""
    drive.activate_through_accessibility(pid)
    time.sleep(0.4)
    for down in (True, False):
        event = drive._create_keyboard_event(None, virtual_key, down)
        drive._post_event(drive.KCG_HID_EVENT_TAP, event)
        drive._cfrelease(event)
        time.sleep(seconds if down else 0.1)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--executable", default="zh")
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
    session_module = load_module("game_end_probe", "macos-game-end-mouse-lock-probe.py")

    drive = session_module.load_input_drive()
    run_dir = args.run_dir.expanduser().resolve()
    resolution = (args.xres, args.yres)
    arguments = ["-nologo", "-xres", str(args.xres), "-yres", str(args.yres)]
    if args.windowed:
        arguments.insert(0, "-win")
    session = session_module.GameSession(lldb, drive, run_dir / args.executable, run_dir,
                                         arguments, args.out.with_suffix(".stderr.log"))
    report = {"arguments": arguments, "input": "CGEventPost HID tap (synthetic)", "steps": []}

    def save():
        report["records"] = session.records
        args.out.write_text(json.dumps(report, indent=2) + "\n")

    def measure(label, action):
        """Hits of each beep-path function while `action()` runs, then the state after it."""
        def arm(reader):
            ids = {}
            for function in BEEP_FUNCTIONS:
                breakpoint = session.target.BreakpointCreateByName(function)
                breakpoint.SetAutoContinue(True)
                ids[function] = (breakpoint.GetID(), breakpoint.GetNumLocations())
            return ids
        ids = session.stopped(arm)
        action()
        time.sleep(1.5)

        def disarm(reader):
            hits = {}
            for function, (breakpoint_id, locations) in ids.items():
                hits[function] = {"hits": session.target.FindBreakpointByID(breakpoint_id)
                                  .GetHitCount(), "locations": locations}
                session.target.BreakpointDelete(breakpoint_id)
            return {"hits": hits, "state": read_state(lldb, session, reader)}
        step = {"step": label}
        step.update(session.stopped(disarm))
        report["steps"].append(step)
        print(json.dumps(step), flush=True)
        save()

    status = 1
    try:
        time.sleep(args.settle)
        session_module.wait_for(
            session, lambda snap: (snap.get("shell_top") or "").endswith("MainMenu.wnd"), 120)
        measure("main menu, after launch", lambda: time.sleep(0.5))
        measure("main menu, after accessibility activation",
                lambda: drive.activate_through_accessibility(session.pid))
        for _, reference_x, reference_y in session_module.SKIRMISH_ROUTE:
            session_module.post_click(
                drive, session.pid,
                reference_x * args.xres / session_module.REFERENCE_RESOLUTION[0],
                reference_y * args.yres / session_module.REFERENCE_RESOLUTION[1], resolution)
            time.sleep(4.0)
        session_module.wait_for(session, lambda snap: snap["game_mode"] ==
                                session_module.GAME_SKIRMISH and (snap["frame"] or 0) > 150, 180)
        session_module.post_move(drive, session.pid, args.xres // 2, args.yres // 2, resolution)
        time.sleep(1.0)

        measure("idle (no key)", lambda: time.sleep(0.5))
        measure("escape (open)", lambda: hold_key(drive, session.pid, KEY_ESCAPE, 0.1))
        measure("escape (close)", lambda: hold_key(drive, session.pid, KEY_ESCAPE, 0.1))
        for name, virtual_key in ARROWS.items():
            measure("arrow %s held 1 s" % name,
                    lambda key=virtual_key: hold_key(drive, session.pid, key, 1.0))
        status = 0
    except Exception:  # recorded: os._exit below would otherwise swallow it
        report["error"] = traceback.format_exc()
        sys.stderr.write(report["error"])
    finally:
        save()
        session.kill()
        sys.stdout.flush()
        sys.stderr.flush()
        os._exit(status)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Drive the native macOS build of the game with real OS-level mouse input, and read back what the
engine saw.

Everything this script sends goes through the operating system: `CGEventPost` into the HID event
tap, which is the same path a physical mouse takes. Nothing is injected into the engine, no callback
is called and no engine state is written -- the engine is only ever *read*, either by walking its
live `GameWindow` tree or by stopping it on a breakpoint. That is deliberate: the question this
harness answers is whether a *user* can reach a screen, and an injected event cannot answer it.

Posting events into another application's window requires the hosting terminal to hold the
Accessibility grant; `capabilities` reports whether it does, and every other subcommand is worthless
without it. Screenshots additionally require the Screen Recording grant.

The engine-reading subcommands need LLDB's Python module, which only exists inside the Xcode
toolchain's interpreter, so the script re-executes itself there when `import lldb` fails.

Usage:
    python3 scripts/macos-input-drive.py capabilities
    python3 scripts/macos-input-drive.py windows --pid 1234
    python3 scripts/macos-input-drive.py activate --pid 1234
    python3 scripts/macos-input-drive.py snapshot --pid 1234
    python3 scripts/macos-input-drive.py buttons --pid 1234
    python3 scripts/macos-input-drive.py click --pid 1234 --client 644,134
    python3 scripts/macos-input-drive.py click --pid 1234 --window ButtonSinglePlayer
    python3 scripts/macos-input-drive.py post --pid 1234 --client 644,134
    python3 scripts/macos-input-drive.py catch-click --pid 1234 --client 644,134
    python3 scripts/macos-input-drive.py backtrace --pid 1234
    python3 scripts/macos-input-drive.py screenshot --pid 1234 --out /tmp/menu.png
"""

import argparse
import ctypes
import ctypes.util
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_BINARY = REPO / "build" / "native" / "native_strict_link"
XCODE_PYTHON = Path("/Applications/Xcode.app/Contents/Developer/usr/bin/python3")

KCF_STRING_ENCODING_UTF8 = 0x08000100
KCF_NUMBER_SINT64 = 4
KCF_NUMBER_DOUBLE = 13
KCG_WINDOW_LIST_ON_SCREEN_ONLY = 1
KCG_WINDOW_LIST_EXCLUDE_DESKTOP = 16
KCG_EVENT_LEFT_MOUSE_DOWN = 1
KCG_EVENT_LEFT_MOUSE_UP = 2
KCG_EVENT_MOUSE_MOVED = 5
KCG_EVENT_LEFT_MOUSE_DRAGGED = 6
KCG_MOUSE_BUTTON_LEFT = 0
KCG_HID_EVENT_TAP = 0
KCG_MOUSE_EVENT_CLICK_STATE = 1
# NSApplicationActivateAllWindows | NSApplicationActivateIgnoringOtherApps.
NS_ACTIVATE_ALL_WINDOWS_IGNORING_OTHERS = 3

core_foundation = ctypes.CDLL(ctypes.util.find_library("CoreFoundation"))
core_graphics = ctypes.CDLL(ctypes.util.find_library("CoreGraphics"))
application_services = ctypes.CDLL(ctypes.util.find_library("ApplicationServices"))
objc_runtime = ctypes.CDLL(ctypes.util.find_library("objc"))
ctypes.CDLL(ctypes.util.find_library("AppKit"))


class CGPoint(ctypes.Structure):
    _fields_ = [("x", ctypes.c_double), ("y", ctypes.c_double)]


def _declare(library, name, restype, argtypes):
    function = getattr(library, name)
    function.restype = restype
    function.argtypes = argtypes
    return function


_cfstring_create = _declare(
    core_foundation, "CFStringCreateWithCString", ctypes.c_void_p,
    [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32])
_cfstring_get = _declare(
    core_foundation, "CFStringGetCString", ctypes.c_bool,
    [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_long, ctypes.c_uint32])
_cfarray_count = _declare(
    core_foundation, "CFArrayGetCount", ctypes.c_long, [ctypes.c_void_p])
_cfarray_at = _declare(
    core_foundation, "CFArrayGetValueAtIndex", ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_long])
_cfdict_get = _declare(
    core_foundation, "CFDictionaryGetValue", ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_void_p])
_cfnumber_get = _declare(
    core_foundation, "CFNumberGetValue", ctypes.c_bool,
    [ctypes.c_void_p, ctypes.c_int, ctypes.c_void_p])
_cfrelease = _declare(core_foundation, "CFRelease", None, [ctypes.c_void_p])
_window_list = _declare(
    core_graphics, "CGWindowListCopyWindowInfo", ctypes.c_void_p,
    [ctypes.c_uint32, ctypes.c_uint32])
_create_mouse_event = _declare(
    core_graphics, "CGEventCreateMouseEvent", ctypes.c_void_p,
    [ctypes.c_void_p, ctypes.c_uint32, CGPoint, ctypes.c_uint32])
_set_integer_field = _declare(
    core_graphics, "CGEventSetIntegerValueField", None,
    [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_int64])
_create_keyboard_event = _declare(
    core_graphics, "CGEventCreateKeyboardEvent", ctypes.c_void_p,
    [ctypes.c_void_p, ctypes.c_uint16, ctypes.c_bool])
_post_event = _declare(core_graphics, "CGEventPost", None, [ctypes.c_uint32, ctypes.c_void_p])
_warp_cursor = _declare(
    core_graphics, "CGWarpMouseCursorPosition", ctypes.c_int32, [CGPoint])
_associate_cursor = _declare(
    core_graphics, "CGAssociateMouseAndMouseCursorPosition", ctypes.c_int32, [ctypes.c_bool])
_ax_create_application = _declare(
    application_services, "AXUIElementCreateApplication", ctypes.c_void_p, [ctypes.c_int32])
_ax_set_attribute = _declare(
    application_services, "AXUIElementSetAttributeValue", ctypes.c_int32,
    [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p])
_ax_copy_attribute = _declare(
    application_services, "AXUIElementCopyAttributeValue", ctypes.c_int32,
    [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)])
_ax_perform_action = _declare(
    application_services, "AXUIElementPerformAction", ctypes.c_int32,
    [ctypes.c_void_p, ctypes.c_void_p])
_cf_boolean_true = ctypes.c_void_p.in_dll(core_foundation, "kCFBooleanTrue")
_get_class = _declare(objc_runtime, "objc_getClass", ctypes.c_void_p, [ctypes.c_char_p])
_selector = _declare(objc_runtime, "sel_registerName", ctypes.c_void_p, [ctypes.c_char_p])


def _objc_send(restype, argtypes):
    """A correctly-prototyped `objc_msgSend`. Each signature needs its own, because variadic
    dispatch through ctypes' default promotion mangles struct and float arguments."""
    return ctypes.CFUNCTYPE(restype, *argtypes)(("objc_msgSend", objc_runtime))


def cfstr(text):
    return _cfstring_create(None, text.encode("utf-8"), KCF_STRING_ENCODING_UTF8)


def cf_to_str(ref):
    if not ref:
        return ""
    buffer = ctypes.create_string_buffer(1024)
    if not _cfstring_get(ref, buffer, len(buffer), KCF_STRING_ENCODING_UTF8):
        return ""
    return buffer.value.decode("utf-8", "replace")


def cf_to_int(ref):
    out = ctypes.c_int64(0)
    _cfnumber_get(ref, KCF_NUMBER_SINT64, ctypes.byref(out))
    return int(out.value)


def cf_to_float(ref):
    out = ctypes.c_double(0.0)
    _cfnumber_get(ref, KCF_NUMBER_DOUBLE, ctypes.byref(out))
    return float(out.value)


def on_screen_windows():
    """Every on-screen window, front to back, as the window server orders them."""
    keys = {name: cfstr(name) for name in
            ("kCGWindowNumber", "kCGWindowOwnerPID", "kCGWindowOwnerName", "kCGWindowName",
             "kCGWindowBounds", "kCGWindowLayer")}
    bounds_keys = {name: cfstr(name) for name in ("X", "Y", "Width", "Height")}
    array = _window_list(
        KCG_WINDOW_LIST_ON_SCREEN_ONLY | KCG_WINDOW_LIST_EXCLUDE_DESKTOP, 0)
    windows = []
    for index in range(_cfarray_count(array)):
        entry = _cfarray_at(array, index)
        bounds_ref = _cfdict_get(entry, keys["kCGWindowBounds"])
        bounds = {name: cf_to_float(_cfdict_get(bounds_ref, ref))
                  for name, ref in bounds_keys.items()}
        windows.append({
            "order": index,
            "number": cf_to_int(_cfdict_get(entry, keys["kCGWindowNumber"])),
            "pid": cf_to_int(_cfdict_get(entry, keys["kCGWindowOwnerPID"])),
            "owner": cf_to_str(_cfdict_get(entry, keys["kCGWindowOwnerName"])),
            "name": cf_to_str(_cfdict_get(entry, keys["kCGWindowName"])),
            "layer": cf_to_int(_cfdict_get(entry, keys["kCGWindowLayer"])),
            "bounds": bounds,
        })
    _cfrelease(array)
    return windows


def game_window(pid):
    """The frontmost on-screen window belonging to `pid`, plus how many windows sit in front."""
    windows = on_screen_windows()
    for window in windows:
        if window["pid"] == pid and window["bounds"]["Width"] > 1:
            window["in_front"] = sum(1 for other in windows if other["order"] < window["order"])
            return window
    return None


def capabilities():
    """What the process is allowed to do, without asserting anything about the game."""
    report = {}
    for library, name in ((application_services, "AXIsProcessTrusted"),
                          (core_graphics, "CGPreflightPostEventAccess"),
                          (core_graphics, "CGPreflightScreenCaptureAccess")):
        report[name] = bool(_declare(library, name, ctypes.c_bool, [])()) \
            if hasattr(library, name) else None
    return report


def activate_through_appkit(pid):
    """Bring `pid` to the front through AppKit, the way a user clicking the Dock would."""
    running_application_class = _get_class(b"NSRunningApplication")
    with_pid = _objc_send(ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int32])
    application = with_pid(
        running_application_class,
        _selector(b"runningApplicationWithProcessIdentifier:"), pid)
    if not application:
        return False
    activate_with_options = _objc_send(
        ctypes.c_bool, [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ulong])
    return bool(activate_with_options(
        application, _selector(b"activateWithOptions:"),
        NS_ACTIVATE_ALL_WINDOWS_IGNORING_OTHERS))


def activate_through_accessibility(pid):
    """Raise and focus the application's window through the Accessibility API, the way an assistive
    app does. This is what the Accessibility grant buys that `activateWithOptions:` does not: a
    non-frontmost caller can raise another application's window and make it key.

    Every value is an `AXError`; 0 is `kAXErrorSuccess`."""
    application = _ax_create_application(pid)
    report = {}
    windows = ctypes.c_void_p()
    report["copy AXWindows"] = _ax_copy_attribute(
        application, cfstr("AXWindows"), ctypes.byref(windows))
    if windows and _cfarray_count(windows):
        window = _cfarray_at(windows, 0)
        report["perform AXRaise"] = _ax_perform_action(window, cfstr("AXRaise"))
        report["set AXMain"] = _ax_set_attribute(window, cfstr("AXMain"), _cf_boolean_true)
    report["set AXFrontmost"] = _ax_set_attribute(
        application, cfstr("AXFrontmost"), _cf_boolean_true)
    return report


def client_to_global(window, client_x, client_y, client_height):
    """Convert a point in the engine's client coordinates (top-left origin, points) to the global
    display coordinates `CGEventPost` uses. The window's `kCGWindowBounds` includes the title bar,
    so the content view's top edge is `height - client_height` below the window's top edge."""
    bounds = window["bounds"]
    title_bar = bounds["Height"] - client_height
    return (bounds["X"] + client_x, bounds["Y"] + title_bar + client_y)


def post_key(virtual_key, settle=0.08):
    """A real key press and release, by macOS virtual keycode (53 is Escape)."""
    for down in (True, False):
        event = _create_keyboard_event(None, virtual_key, down)
        _post_event(KCG_HID_EVENT_TAP, event)
        _cfrelease(event)
        time.sleep(settle)


def post_drag(from_point, to_point, steps=6, settle=0.08):
    """A real left-button drag: down at the start, dragged events along the way, up at the end.
    The engine's box selection needs the intermediate drags, not just the two ends."""
    from_x, from_y = from_point
    to_x, to_y = to_point
    _warp_cursor(CGPoint(from_x, from_y))
    _associate_cursor(True)

    def post(event_type, x, y):
        event = _create_mouse_event(
            None, event_type, CGPoint(x, y), KCG_MOUSE_BUTTON_LEFT)
        if event_type != KCG_EVENT_MOUSE_MOVED:
            _set_integer_field(event, KCG_MOUSE_EVENT_CLICK_STATE, 1)
        _post_event(KCG_HID_EVENT_TAP, event)
        _cfrelease(event)
        time.sleep(settle)

    post(KCG_EVENT_MOUSE_MOVED, from_x, from_y)
    post(KCG_EVENT_LEFT_MOUSE_DOWN, from_x, from_y)
    for step in range(1, steps + 1):
        fraction = step / float(steps)
        post(KCG_EVENT_LEFT_MOUSE_DRAGGED,
             from_x + (to_x - from_x) * fraction, from_y + (to_y - from_y) * fraction)
    post(KCG_EVENT_LEFT_MOUSE_UP, to_x, to_y)


def post_click(global_x, global_y, move_only=False, settle=0.08):
    point = CGPoint(global_x, global_y)
    _warp_cursor(point)
    _associate_cursor(True)
    for event_type in (KCG_EVENT_MOUSE_MOVED,) if move_only else (
            KCG_EVENT_MOUSE_MOVED, KCG_EVENT_LEFT_MOUSE_DOWN, KCG_EVENT_LEFT_MOUSE_UP):
        event = _create_mouse_event(None, event_type, point, KCG_MOUSE_BUTTON_LEFT)
        if event_type != KCG_EVENT_MOUSE_MOVED:
            _set_integer_field(event, KCG_MOUSE_EVENT_CLICK_STATE, 1)
        _post_event(KCG_HID_EVENT_TAP, event)
        _cfrelease(event)
        time.sleep(settle)


def screenshot(window, out_path):
    """`screencapture -l` captures one window by id; it needs the Screen Recording grant."""
    result = subprocess.run(
        ["screencapture", "-x", "-o", "-l", str(window["number"]), str(out_path)],
        capture_output=True, text=True)
    return result.returncode, result.stdout.strip(), result.stderr.strip()


# --------------------------------------------------------------------------------------------------
# Engine-side reading, through LLDB.
# --------------------------------------------------------------------------------------------------

MENU_BUTTON_PREFIXES = ("Button", "Radio", "Check", "Combo", "List", "Text", "Static")


class EngineReader:
    """A short-lived LLDB attachment. The process is stopped for the duration and resumed by
    `detach()`, so the game keeps running between subcommands."""

    def __init__(self, pid, binary):
        import lldb
        self.lldb = lldb
        self.debugger = lldb.SBDebugger.Create()
        self.debugger.SetAsync(False)
        self.target = self.debugger.CreateTarget(str(binary))
        error = lldb.SBError()
        self.process = self.target.AttachToProcessWithID(
            self.debugger.GetListener(), pid, error)
        if not self.process.IsValid():
            raise RuntimeError("attach to %d failed: %s" % (pid, error))
        self.frame = self.process.GetSelectedThread().GetFrameAtIndex(0)

    def value(self, expression):
        result = self.frame.EvaluateExpression(expression)
        if result.GetError().Fail():
            return None
        return result

    def integer(self, expression):
        result = self.value(expression)
        return None if result is None else result.GetValueAsSigned()

    def text(self, expression):
        result = self.value(expression)
        if result is None:
            return None
        summary = result.GetSummary() or result.GetValue() or ""
        return summary.strip('"')

    def state(self):
        """Everything the slice needs to know about the engine, in one stop."""
        report = {
            "window_is_active": self.text(
                "(bool)WWPlatform::Window_Is_Active(WWPlatform::Window_Current())"),
            "mouse_x": self.integer("TheMouse->m_currMouse.pos.x"),
            "mouse_y": self.integer("TheMouse->m_currMouse.pos.y"),
            "mouse_left_state": self.integer("(int)TheMouse->m_currMouse.leftState"),
            "mouse_left_event": self.integer("(int)TheMouse->m_currMouse.leftEvent"),
            "mouse_limit_x": self.integer("TheMouse->m_maxX"),
            "mouse_limit_y": self.integer("TheMouse->m_maxY"),
            "shell_screen_count": self.integer("TheShell->m_screenCount"),
            "shell_top": None,
            "mute_reason_bits": self.integer("(int)TheAudio->m_muteReasonBits"),
            "music_volume": self.text("TheAudio->m_musicVolume"),
            "sound_volume": self.text("TheAudio->m_soundVolume"),
            # `isInGame()` and `getFrame()` are inline, so there is nothing to call: read the
            # fields they read. `GAME_SKIRMISH` is 2, `GAME_SHELL` 4, `GAME_NONE` 6. The cast is
            # required because `TheGameLogic`'s declared type is not in the debug info the
            # static archives left behind, only its concrete subclass is.
            "game_mode": self.integer("(int)((W3DGameLogic*)TheGameLogic)->m_gameMode"),
            "frame": self.integer("(int)((W3DGameLogic*)TheGameLogic)->m_frame"),
            "selected_count": self.integer("(int)((W3DInGameUI*)TheInGameUI)->m_selectCount"),
            "quit_menu_visible": self.integer(
                "(int)((W3DInGameUI*)TheInGameUI)->m_isQuitMenuVisible"),
            "frame_selection_changed": self.integer(
                "(int)((W3DInGameUI*)TheInGameUI)->m_frameSelectionChanged"),
        }
        count = report["shell_screen_count"] or 0
        report["shell_stack"] = [
            self.text("(const char*)TheShell->m_screenStack[%d]->m_filenameString.str()" % index)
            for index in range(count)]
        if report["shell_stack"]:
            report["shell_top"] = report["shell_stack"][-1]
        report["render_ledger"] = self.render_ledger()
        return report

    def render_ledger(self):
        """The Vulkan backend's unimplemented/unserviceable-call ledger with counts, read from the
        live process; the game is stopped with a signal, so nothing dumps it at exit."""
        kinds = self.integer("(unsigned)VulkanRenderBackendClass::Unimplemented_Call_Kinds()")
        if kinds is None:
            return None
        entries = []
        for index in range(kinds):
            call = "VulkanRenderBackendClass::Unimplemented_Call(%d)" % index
            entries.append({
                "name": self.text("(const char*)%s->Name" % call),
                "count": self.integer("(unsigned)%s->Count" % call),
            })
        return entries

    def named_windows(self, top_layout_only=False):
        """Walk the live `GameWindow` tree, reporting each named window's screen rect. This is what
        the engine believes is on screen, independent of what the renderer drew.

        `top_layout_only` walks the layout on top of the shell's screen stack instead of every
        window the manager holds, which is the only way to tell two identically-named buttons in
        two different `.wnd` files apart."""
        found = []

        def walk(pointer, depth, sibling_field="m_next"):
            while pointer:
                expression = "((GameWindow*)%s)" % hex(pointer)
                name = self.text(
                    "(const char*)(%s->m_instData.m_decoratedNameString).str()" % expression) or ""
                self.value("int $x=0; int $y=0; int $w=0; int $h=0;")
                self.value("(int)%s->winGetScreenPosition(&$x,&$y)" % expression)
                self.value("(int)%s->winGetSize(&$w,&$h)" % expression)
                found.append({
                    "pointer": hex(pointer),
                    "depth": depth,
                    "name": name.split(":")[-1],
                    "x": self.integer("$x"), "y": self.integer("$y"),
                    "w": self.integer("$w"), "h": self.integer("$h"),
                    "hidden": self.text("(bool)%s->winIsHidden()" % expression),
                    "enabled": self.text("(bool)%s->winIsEnabled()" % expression),
                })
                child = self.integer("%s->m_child" % expression)
                if child:
                    walk(child, depth + 1)
                pointer = self.integer("%s->%s" % (expression, sibling_field))

        if top_layout_only:
            top = (self.integer("TheShell->m_screenCount") or 0) - 1
            if top < 0:
                return found
            root = self.integer("TheShell->m_screenStack[%d]->m_windowList" % top)
            if root:
                walk(root, 0, "m_nextLayout")
            return found
        root = self.integer("TheWindowManager->m_windowList")
        if root:
            walk(root, 0)
        return found

    def backtrace(self, all_threads=False):
        lines = []
        threads = self.process if all_threads else [self.process.GetSelectedThread()]
        for thread in threads:
            lines.append("thread #%d %s" % (thread.GetIndexID(), thread.GetStopDescription(256)))
            for frame in thread:
                lines.append("  %s" % frame)
            if not all_threads:
                break
        return "\n".join(lines)

    def catch_mouse_event(self, poster, timeout=10.0):
        """Stop the engine the first time it converts a platform mouse event into a `MouseIO`, and
        report that `MouseIO`. `poster` is called once the process is running again, so the event it
        posts is the one caught."""
        breakpoint = self.target.BreakpointCreateByName("Mouse::processMouseEvent")
        if breakpoint.GetNumLocations() == 0:
            return {"error": "no location for Mouse::processMouseEvent"}
        watchdog = threading.Timer(timeout, self.process.SendAsyncInterrupt)
        threading.Timer(0.4, poster).start()
        watchdog.start()
        self.process.Continue()
        watchdog.cancel()
        self.frame = self.process.GetSelectedThread().GetFrameAtIndex(0)
        stopped_here = breakpoint.GetHitCount() > 0
        report = {"hit": stopped_here, "hit_count": breakpoint.GetHitCount(),
                  "function": str(self.frame.GetFunctionName())}
        if stopped_here:
            index = self.integer("index")
            report.update({
                "index": index,
                "event_x": self.integer("this->m_mouseEvents[index].pos.x"),
                "event_y": self.integer("this->m_mouseEvents[index].pos.y"),
                "event_left_state": self.integer("(int)this->m_mouseEvents[index].leftState"),
                "event_right_state": self.integer("(int)this->m_mouseEvents[index].rightState"),
                "events_this_frame": self.integer("this->m_eventsThisFrame"),
            })
        self.target.BreakpointDelete(breakpoint.GetID())
        return report

    def detach(self):
        self.process.Detach()
        self.lldb.SBDebugger.Destroy(self.debugger)


def require_lldb():
    """Re-exec under Xcode's interpreter, the only one with the `lldb` module."""
    try:
        import lldb  # noqa: F401
        return
    except ImportError:
        pass
    if os.environ.get("MACOS_INPUT_DRIVE_REEXEC"):
        sys.exit("no lldb python module, even under %s" % sys.executable)
    lldb_python_path = subprocess.run(
        ["lldb", "-P"], capture_output=True, text=True, check=True).stdout.strip()
    environment = dict(os.environ,
                       PYTHONPATH=lldb_python_path, MACOS_INPUT_DRIVE_REEXEC="1")
    interpreter = str(XCODE_PYTHON) if XCODE_PYTHON.exists() else sys.executable
    os.execve(interpreter, [interpreter, os.path.abspath(__file__)] + sys.argv[1:], environment)


def print_report(title, report):
    print("== %s" % title)
    for key, value in report.items():
        print("   %-22s %s" % (key, value))


def describe_window(window):
    if window is None:
        return "   no on-screen window for that pid"
    bounds = window["bounds"]
    return ("   window #%d %r owner=%r layer=%d bounds=%.0f,%.0f %.0fx%.0f "
            "order=%d windows_in_front=%d" % (
                window["number"], window["name"], window["owner"], window["layer"],
                bounds["X"], bounds["Y"], bounds["Width"], bounds["Height"],
                window["order"], window["in_front"]))


def parse_point(text):
    x, _, y = text.partition(",")
    return int(x), int(y)


def resolve_target(reader, args):
    """Where to click, in client points: either given directly or the centre of a named window as
    the engine itself reports it."""
    if args.client:
        return parse_point(args.client), None
    matches = [window for window in reader.named_windows(not args.all_windows)
               if window["name"] == args.window]
    if not matches:
        raise SystemExit("no window named %r in the live tree" % args.window)
    target = matches[0]
    centre = (target["x"] + target["w"] // 2, target["y"] + target["h"] // 2)
    return centre, target


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=[
        "capabilities", "windows", "activate", "snapshot", "buttons", "click", "move",
        "drag", "key", "post", "catch-click", "backtrace", "screenshot"])
    parser.add_argument("--pid", type=int)
    parser.add_argument("--binary", default=str(DEFAULT_BINARY))
    parser.add_argument("--client", help="click target in engine client points, `x,y`")
    parser.add_argument("--window", help="click the centre of this named GameWindow")
    parser.add_argument("--to", help="drag destination in engine client points, `x,y`")
    parser.add_argument("--key", type=int, help="macOS virtual keycode to press, 53 is Escape")
    parser.add_argument("--client-height", type=int, default=600,
                        help="engine client height in points, to locate the content view")
    parser.add_argument("--out", help="screenshot destination")
    parser.add_argument("--all-threads", action="store_true")
    parser.add_argument("--all-windows", action="store_true",
                        help="walk every window the manager holds, not just the top shell layout")
    parser.add_argument("--no-activate", action="store_true",
                        help="post without raising the window first, to measure what that changes")
    parser.add_argument("--settle", type=float, default=0.4,
                        help="seconds to let the engine run after posting, before reading back")
    args = parser.parse_args()

    if args.command == "capabilities":
        print_report("capabilities", capabilities())
        return

    if args.pid is None:
        raise SystemExit("--pid is required for %s" % args.command)

    if args.command == "windows":
        print("== on-screen windows: %d" % len(on_screen_windows()))
        print(describe_window(game_window(args.pid)))
        return

    if args.command == "activate":
        print("== activateWithOptions: returned %s" % activate_through_appkit(args.pid))
        time.sleep(args.settle)
        print(describe_window(game_window(args.pid)))
        print_report("accessibility activation (0 == kAXErrorSuccess)",
                     activate_through_accessibility(args.pid))
        time.sleep(args.settle)
        print(describe_window(game_window(args.pid)))
        return

    if args.command == "screenshot":
        window = game_window(args.pid)
        if window is None:
            raise SystemExit("no on-screen window for pid %d" % args.pid)
        code, out, err = screenshot(window, args.out)
        print("== screencapture -l%d -> %s exit=%d %s %s" % (
            window["number"], args.out, code, out, err))
        return

    if args.command == "post":
        # Post without attaching LLDB, for when the game is already running under a debugger:
        # only one debugger can own the process, and the engine state is read there instead.
        os_window = game_window(args.pid)
        if os_window is None:
            raise SystemExit("no on-screen window for pid %d" % args.pid)
        if args.key is not None:
            print_report("accessibility activation (0 == kAXErrorSuccess)",
                         activate_through_accessibility(args.pid))
            time.sleep(args.settle)
            post_key(args.key)
            return
        client_x, client_y = parse_point(args.client)
        global_x, global_y = client_to_global(
            os_window, client_x, client_y, args.client_height)
        print("== target client=(%d,%d) global=(%.0f,%.0f)" % (
            client_x, client_y, global_x, global_y))
        if not args.no_activate:
            print_report("accessibility activation (0 == kAXErrorSuccess)",
                         activate_through_accessibility(args.pid))
            time.sleep(args.settle)
        if args.to:
            post_drag((global_x, global_y),
                      client_to_global(os_window, *parse_point(args.to),
                                       client_height=args.client_height))
        else:
            post_click(global_x, global_y)
        return

    require_lldb()
    reader = EngineReader(args.pid, args.binary)
    try:
        if args.command == "snapshot":
            print_report("engine state", reader.state())
        elif args.command == "buttons":
            windows = reader.named_windows(not args.all_windows)
            print("== live GameWindow tree: %d windows" % len(windows))
            for window in windows:
                if args.all_windows and not window["name"].startswith(MENU_BUTTON_PREFIXES):
                    continue
                print("   %s%-26s at %4d,%-4d %3dx%-3d hidden=%s enabled=%s" % (
                    "  " * window["depth"], window["name"], window["x"], window["y"],
                    window["w"], window["h"], window["hidden"], window["enabled"]))
        elif args.command == "backtrace":
            print(reader.backtrace(args.all_threads))
        elif args.command == "key":
            print_report("engine state before", reader.state())
            reader.detach()
            print_report("accessibility activation (0 == kAXErrorSuccess)",
                         activate_through_accessibility(args.pid))
            time.sleep(args.settle)
            post_key(args.key)
            time.sleep(args.settle)
            reader = EngineReader(args.pid, args.binary)
            print_report("engine state after", reader.state())
        elif args.command in ("click", "move", "drag", "catch-click"):
            (client_x, client_y), target = resolve_target(reader, args)
            os_window = game_window(args.pid)
            if os_window is None:
                raise SystemExit("no on-screen window for pid %d" % args.pid)
            global_x, global_y = client_to_global(
                os_window, client_x, client_y, args.client_height)
            print("== target client=(%d,%d) global=(%.0f,%.0f) %s" % (
                client_x, client_y, global_x, global_y,
                "window=%s" % target["name"] if target else ""))
            print("   before:")
            print_report("engine state", reader.state())

            def raise_and_post():
                # Raise immediately before posting and while the engine is running: the window
                # server routes a HID event to whatever window is topmost at that point, and an
                # LLDB stop in between costs the window its key status again.
                if not args.no_activate:
                    print_report("accessibility activation (0 == kAXErrorSuccess)",
                                 activate_through_accessibility(args.pid))
                    time.sleep(args.settle)
                if args.command == "drag":
                    post_drag((global_x, global_y),
                              client_to_global(os_window, *parse_point(args.to),
                                               client_height=args.client_height))
                else:
                    post_click(global_x, global_y, move_only=args.command == "move")

            if args.command == "catch-click":
                report = reader.catch_mouse_event(raise_and_post)
                print_report("MouseIO at Mouse::processMouseEvent", report)
            else:
                reader.detach()
                raise_and_post()
                time.sleep(args.settle)
                reader = EngineReader(args.pid, args.binary)
            print("   after:")
            print_report("engine state", reader.state())
    finally:
        reader.detach()


if __name__ == "__main__":
    main()

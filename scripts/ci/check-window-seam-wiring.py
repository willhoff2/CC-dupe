#!/usr/bin/env python3
"""Check that the engine's non-Windows window/event-loop/input path goes through the seam.

The window seam is only worth having if the engine actually uses it, and there are three ways
that quietly stops being true:

  * a function is declared in PlatformWindowHost.h and never defined, so the wiring links only
    because nothing calls it yet;
  * a WWPlatform::WindowEventType grows, or an existing one loses its case label, and the event
    is silently dropped instead of being triaged in docs/porting/window-event-loop.md;
  * a file on the wired path includes <windows.h> or reaches for a WndProc-era Win32 call
    outside an #ifdef _WIN32, which breaks the native build for everyone;
  * the renderer's window-sizing call, DX8Wrapper::Resize_And_Position_Window(), is compiled
    only on Windows, so the seam's GetClientRect/AdjustWindowRect/SetWindowPos implementations
    exist but never run and the window stays at its creation size whatever -xres/-yres asked.

None of these are visible in a Windows build, which is why this runs in CI. It is a text check:
it does not compile anything, and it says nothing about whether the native behaviour is right -
docs/porting/window-event-loop.md records what is deliberately not reproduced.

Usage:
    python3 scripts/ci/check-window-seam-wiring.py
"""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

HOST_HEADER = os.path.join(ROOT, "Core", "GameEngine", "Include", "GameClient",
                           "PlatformWindowHost.h")
HOST_SOURCE = os.path.join(ROOT, "Core", "GameEngine", "Source", "GameClient",
                           "PlatformWindowHost.cpp")
WINDOW_HEADER = os.path.join(ROOT, "Core", "Libraries", "Source", "WWVegas", "WWLib", "platform",
                             "platform_window.h")

# The seam's engine-side consumers: file -> the seam call it must make. This is the "somebody
# calls it" half of the check, and the list is the wiring itself, so adding a consumer means
# adding a row here.
CONSUMERS = {
    os.path.join("GeneralsMD", "Code", "GameEngineDevice", "Source", "Win32Device", "Common",
                 "Win32GameEngine.cpp"): ["PlatformWindowHost::serviceOS",
                                          "PlatformWindowHost::isMinimized"],
    os.path.join("Core", "GameEngineDevice", "Source", "Win32Device", "GameClient",
                 "Win32Mouse.cpp"): ["PlatformWindowHost::getNextMouseEvent",
                                     "PlatformWindowHost::setCursorClip"],
    os.path.join("Core", "GameEngineDevice", "Source", "Win32Device", "GameClient",
                 "Win32DIKeyboard.cpp"): ["PlatformWindowHost::getNextKeyEvent",
                                          "PlatformWindowHost::isCapsLockOn"],
    os.path.join("GeneralsMD", "Code", "Main",
                 "PlatformMain.cpp"): ["PlatformWindowHost::createAppWindow",
                                       "PlatformWindowHost::destroyAppWindow"],
    os.path.join("Core", "GameEngine", "Source", "Common", "System",
                 "Debug.cpp"): ["WWPlatform::Dialog_Message_Box"],
}

# Every file above, plus the seam itself, must keep its Win32 half behind _WIN32.
GUARDED_FILES = sorted(CONSUMERS) + [
    os.path.join("Core", "GameEngineDevice", "Include", "Win32Device", "GameClient",
                 "Win32Mouse.h"),
    os.path.join("Core", "GameEngineDevice", "Include", "Win32Device", "GameClient",
                 "Win32DIKeyboard.h"),
    os.path.join("Core", "GameEngine", "Source", "GameClient", "Input", "Keyboard.cpp"),
]

DECLARATION = re.compile(r"^(?:Bool|void|Int|UnsignedInt)\s+(\w+)\s*\(", re.M)
DEFINITION = re.compile(r"^(?:Bool|void|Int|UnsignedInt)\s+(\w+)\s*\(", re.M)
EVENT_ENUMERATOR = re.compile(r"^\s*(WINDOW_EVENT_[A-Z_]+)\b", re.M)
CASE_LABEL = re.compile(r"case\s+WWPlatform::(WINDOW_EVENT_[A-Z_]+)\s*:")

# Win32-only spellings that must never appear outside an #ifdef _WIN32 in the wired files.
WIN32_ONLY = re.compile(
    r"\b(?:<windows\.h>|PeekMessage|DispatchMessage|TranslateMessage|GetMessage|CreateWindow|"
    r"RegisterClass|ClipCursor|SetCursorPos|IsIconic|GetKeyboardLayout|"
    r"GetKeyState|SetErrorMode|DirectInput8Create)\b")

# The renderer resizes the window to the requested resolution through the seam's Win32 user
# functions. Every call to it must be compiled off Windows too, or the seam is wired but idle.
RESIZE_SOURCE = os.path.join("Core", "Libraries", "Source", "WWVegas", "WW3D2", "dx8wrapper.cpp")
RESIZE_CALL = re.compile(r"\bResize_And_Position_Window\s*\(\s*\)\s*;")

# MessageBox() and ShowWindow() are deliberately absent from WIN32_ONLY: Debug.cpp keeps calling
# them under those names, and off Windows they resolve to the seam's own definitions. That is the
# pattern, not a leak. LoadCursorFromFile() and SetCursor() are absent for the same reason:
# Win32Mouse.cpp calls them under their Win32 names and platform_win32_user.cpp defines them off
# Windows over the seam's Window_Create_Cursor()/Window_Set_Cursor()
# (docs/porting/mouse-cursor-seam.md).


def read(path):
    with open(path, "r", errors="replace") as handle:
        return handle.read()


def unguarded_lines(text):
    """Lines of text that are not inside an #ifdef _WIN32 / #ifndef !_WIN32 Windows branch."""
    out = []
    # Stack of booleans: True while the lines are compiled on Windows only.
    stack = []
    for number, line in enumerate(text.splitlines(), 1):
        stripped = line.strip()
        directive = re.match(r"#\s*(ifdef|ifndef|if|else|elif|endif)\b(.*)", stripped)
        if directive:
            kind, rest = directive.group(1), directive.group(2).strip()
            if kind == "ifdef":
                stack.append(rest == "_WIN32")
            elif kind == "ifndef":
                stack.append(False)
            elif kind == "if":
                stack.append(bool(re.match(r"defined\s*\(\s*_WIN32\s*\)", rest)))
            elif kind == "elif":
                if stack:
                    stack[-1] = bool(re.match(r"defined\s*\(\s*_WIN32\s*\)", rest))
            elif kind == "else":
                if stack:
                    # The else of an #ifndef _WIN32 is the Windows branch, and vice versa. Both
                    # are covered by flipping, because only _WIN32 conditions are tracked.
                    stack[-1] = not stack[-1]
            elif kind == "endif":
                if stack:
                    stack.pop()
            continue
        if not any(stack):
            out.append((number, line))
    return out


def main():
    failures = []

    for path in [HOST_HEADER, HOST_SOURCE, WINDOW_HEADER]:
        if not os.path.isfile(path):
            print("FAIL: missing %s" % os.path.relpath(path, ROOT), file=sys.stderr)
            return 1

    # 1. Everything PlatformWindowHost.h declares is defined.
    header = read(HOST_HEADER)
    source = read(HOST_SOURCE)
    declared = set(DECLARATION.findall(header))
    defined = set(DEFINITION.findall(source))
    if not declared:
        failures.append("parsed no declarations out of PlatformWindowHost.h")
    missing = sorted(declared - defined)
    if missing:
        failures.append("declared in PlatformWindowHost.h but not defined: %s"
                        % ", ".join(missing))
    print("PlatformWindowHost.h: %d functions declared, %d defined"
          % (len(declared), len(declared) - len(missing)))

    # 2. Every window event has a case label, so a new one cannot be dropped without a decision.
    # A case label is not a handler: WINDOW_EVENT_TEXT and WINDOW_EVENT_NONE deliberately fall
    # into the no-op group, which the triage table records.
    events = set(EVENT_ENUMERATOR.findall(read(WINDOW_HEADER)))
    labels = CASE_LABEL.findall(source)
    handled = set(labels)
    for event in sorted({e for e in handled if labels.count(e) > 1}):
        failures.append("%s has %d case labels in PlatformWindowHost.cpp; a duplicate case value"
                        " does not compile" % (event, labels.count(event)))
    if not events:
        failures.append("parsed no WINDOW_EVENT_* values out of platform_window.h")
    for event in sorted(events - handled):
        failures.append("%s has no case label in PlatformWindowHost.cpp: triage it in"
                        " docs/porting/window-event-loop.md and give it one, even a no-op"
                        % event)
    print("platform_window.h: %d event types, %d with a case label in PlatformWindowHost.cpp"
          % (len(events), len(events & handled)))

    # 3. Every consumer still calls the seam.
    for relative, calls in sorted(CONSUMERS.items()):
        path = os.path.join(ROOT, relative)
        if not os.path.isfile(path):
            failures.append("%s: missing" % relative)
            continue
        text = read(path)
        for call in calls:
            if call not in text:
                failures.append("%s: does not call %s" % (relative, call))
    print("consumers: %d files checked for %d seam calls"
          % (len(CONSUMERS), sum(len(c) for c in CONSUMERS.values())))

    # 4. No Win32-only spelling outside a Windows branch in the wired files.
    for relative in GUARDED_FILES:
        path = os.path.join(ROOT, relative)
        if not os.path.isfile(path):
            failures.append("%s: missing" % relative)
            continue
        for number, line in unguarded_lines(read(path)):
            if re.match(r"(//|\*|/\*)", line.lstrip()):
                continue
            found = WIN32_ONLY.search(line)
            if found:
                failures.append("%s:%d: %s outside an #ifdef _WIN32: %s"
                                % (relative, number, found.group(0).strip(), line.strip()))
    print("guarded files: %d checked for unguarded Win32 calls" % len(GUARDED_FILES))

    # 5. The window-sizing call runs off Windows: every Resize_And_Position_Window() call site
    # in dx8wrapper.cpp is outside a Windows-only branch.
    resize_path = os.path.join(ROOT, RESIZE_SOURCE)
    if not os.path.isfile(resize_path):
        failures.append("%s: missing" % RESIZE_SOURCE)
    else:
        resize_text = read(resize_path)
        total = len(RESIZE_CALL.findall(resize_text))
        reachable = sum(1 for _, line in unguarded_lines(resize_text)
                        if RESIZE_CALL.search(line))
        if total == 0:
            failures.append("%s: no call to Resize_And_Position_Window(); the requested "
                            "resolution is never applied to the window" % RESIZE_SOURCE)
        elif reachable != total:
            failures.append("%s: %d of %d Resize_And_Position_Window() calls are inside an "
                            "#ifdef _WIN32; off Windows the window keeps its creation size "
                            "while TheDisplay believes -xres/-yres"
                            % (RESIZE_SOURCE, total - reachable, total))
        print("dx8wrapper.cpp: %d of %d Resize_And_Position_Window() calls compiled off Windows"
              % (reachable, total))

    if failures:
        print("", file=sys.stderr)
        for failure in failures:
            print("FAIL: %s" % failure, file=sys.stderr)
        return 1

    print("OK: the engine's non-Windows window/input path goes through the seam")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Enumerate the Win32 window, message-loop and input surface the engine uses.

This is the measurement behind docs/porting/window-event-loop.md. It exists because the
window/event-loop/input slice is the one platform seam whose size was never counted: the
`HWND` file counts in docs/porting/next-slice-scope.md say how many files mention a window
handle, not how many calls have to be replaced, and they do not separate the message pump
from the input reads at all.

Methodology, deliberately conservative so the numbers can be audited:

  * Comments and string literals are stripped before counting, so a `WM_SIZE` in a comment
    or in `messageToString()`'s log strings is not counted as a call site. That matters
    here more than anywhere else in the repo: `GeneralsMD/Code/Main/WinMain.cpp` contains a
    ~280-case `switch` whose only job is to turn `WM_*` values into strings for a debug
    log, and counting it raw triples the apparent `WM_*` surface.
  * Every match is attributed to an area (see AREAS), and each area is either in scope for
    the native single-player port or not. Out of scope: `*/Tools/*` (WorldBuilder, W3DView,
    GUIEdit, ImagePacker, ParticleEditor, the MFC tools generally) and `Generals/` (the base
    game; the port targets Zero Hour only), matching the split already published in
    docs/porting/next-slice-scope.md.
  * Symbols are grouped into the categories the seam has to answer for (window handles, the
    message pump, the WndProc/WM_* model, polled input, cursor capture/clipping, window
    placement, and the fullscreen/mode-change path), because "N references to HWND" is not
    a unit of work but "N cursor-clipping call sites" is.

Usage:
    python3 scripts/window-input-scan.py                        # human-readable report
    python3 scripts/window-input-scan.py --json out.json        # machine-readable
    python3 scripts/window-input-scan.py --markdown out.md      # the doc's tables
    python3 scripts/window-input-scan.py --check                # gate against the baseline
    python3 scripts/window-input-scan.py --check --update       # re-baseline
"""
import argparse
import collections
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
BASELINE = os.path.join(ROOT, "docs", "porting", "ci-baselines", "window-input-scan.json")

SOURCE_EXT = (".cpp", ".h", ".hpp", ".c", ".inl", ".mm")

# Areas, in the order they are tested. `in_scope` is the port plan's cut, not a judgement
# about the code: the MFC tools stay on Wine and the base game is not being ported.
AREAS = [
    # The port harness itself: the Win32 header stand-ins and the renderer spike. They
    # mention these symbols by construction (the shims *declare* HWND), so counting them as
    # engine work would inflate every number here. It is not scanned at all (see SKIP_AREAS):
    # the spike is Vulkan code, whose VK_STRUCTURE_TYPE_* and VK_FORMAT_* tokens are
    # indistinguishable from Win32 virtual-key codes to a regex, so every renderer edit would
    # otherwise move a number that has nothing to do with the Win32 surface.
    ("harness", False, lambda rel: rel.startswith((
        "scripts/native-port-shims/", "spikes/"))),
    ("tools", False, lambda rel: "/Tools/" in "/" + rel),
    ("generals", False, lambda rel: rel.startswith("Generals/")),
    ("main", True, lambda rel: rel.startswith("GeneralsMD/Code/Main/")),
    ("device", True, lambda rel: rel.startswith((
        "GeneralsMD/Code/GameEngineDevice/", "Core/GameEngineDevice/"))),
    ("engine", True, lambda rel: rel.startswith((
        "GeneralsMD/Code/GameEngine/", "Core/GameEngine/"))),
    ("libraries", True, lambda rel: rel.startswith((
        "Core/Libraries/", "GeneralsMD/Code/Libraries/"))),
    ("other", False, lambda rel: True),
]

# Areas whose files are not read at all, as opposed to counted as out of scope.
SKIP_AREAS = frozenset(["harness"])

# The categories the seam is answerable for. Each entry is (identifier -> regex); a symbol
# is counted once per textual occurrence outside comments and string literals.
CATEGORIES = collections.OrderedDict([
    ("window_handle", [
        "HWND", "ApplicationHWnd", "CreateWindow", "CreateWindowEx", "RegisterClass",
        "RegisterClassEx", "DestroyWindow", "DefWindowProc", "GetClientRect",
        "GetWindowRect", "IsIconic", "SetFocus", "SetForegroundWindow", "GetActiveWindow",
        "GetForegroundWindow", "AdjustWindowRect",
    ]),
    ("message_pump", [
        "PeekMessage", "GetMessage", "DispatchMessage", "TranslateMessage",
        "PostQuitMessage", "PostMessage", "SendMessage", "MSG",
    ]),
    ("wndproc", [
        "WndProc", "WNDCLASS", "WNDPROC", "LRESULT", "WPARAM", "LPARAM",
        r"WM_[A-Z0-9_]+",
    ]),
    ("polled_input", [
        "GetAsyncKeyState", "GetKeyState", "GetKeyboardState", "GetKeyboardLayout",
        "MapVirtualKey", "MapVirtualKeyEx", "ToAscii", "ToUnicode", "HKL",
        r"VK_[A-Z0-9_]+", "DirectInput8Create", "IDirectInputDevice8",
        "GetDoubleClickTime",
    ]),
    ("cursor", [
        "ClipCursor", "SetCapture", "ReleaseCapture", "SetCursorPos", "GetCursorPos",
        "ShowCursor", "SetCursor", "LoadCursor", "ScreenToClient", "ClientToScreen",
    ]),
    ("placement", [
        "SetWindowPos", "ShowWindow", "UpdateWindow", "MoveWindow", "SetWindowLong",
        "GetWindowLong", "GetSystemMetrics", "InvalidateRect", "BeginPaint", "EndPaint",
    ]),
    ("mode_change", [
        "ChangeDisplaySettings", "ChangeDisplaySettingsEx", "EnumDisplaySettings",
        "DEVMODE", "D3DPRESENT_PARAMETERS", "Reset_Device", "Set_Render_Device",
        "Set_Device_Resolution", "Toggle_Windowed",
    ]),
])

# Files whose Win32 window/input use the seam has to replace, called out individually in the
# doc because they are the ones that currently fail to compile natively on these types.
SPOTLIGHT = [
    "GeneralsMD/Code/Main/WinMain.cpp",
    "GeneralsMD/Code/GameEngineDevice/Source/Win32Device/Common/Win32GameEngine.cpp",
    "Core/GameEngine/Source/Common/System/Debug.cpp",
    "Core/GameEngine/Source/GameClient/Input/Keyboard.cpp",
    "Core/GameEngineDevice/Source/Win32Device/GameClient/Win32DIKeyboard.cpp",
    "Core/GameEngineDevice/Source/Win32Device/GameClient/Win32Mouse.cpp",
    "Core/GameEngineDevice/Source/Win32Device/GameClient/Win32DIMouse.cpp",
]


def strip_comments_and_strings(text):
    """Blank out // and /* */ comments and the contents of string/char literals."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            end = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, end))
            i = end
        elif c in "\"'":
            quote = c
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == quote:
                    j += 1
                    break
                if text[j] == "\n":
                    break
                j += 1
            out.append(quote + quote)
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def area_of(rel):
    for name, in_scope, test in AREAS:
        if test(rel):
            return name, in_scope
    raise AssertionError("AREAS must end in a catch-all")


def compile_patterns():
    patterns = collections.OrderedDict()
    for category, symbols in CATEGORIES.items():
        compiled = []
        for symbol in symbols:
            # A bare identifier is matched whole; an explicit regex (WM_[A-Z0-9_]+ and
            # friends) is used as written, still with word boundaries around it.
            compiled.append((symbol, re.compile(r"\b(?:%s)\b" % symbol)))
        patterns[category] = compiled
    return patterns


def scan():
    patterns = compile_patterns()
    # counts[area][category] = hits, files[area][category] = {rel, ...}
    counts = collections.defaultdict(lambda: collections.Counter())
    files = collections.defaultdict(lambda: collections.defaultdict(set))
    # symbols[bucket][symbol] and symbol_files[bucket][symbol], so that every number quoted
    # in the doc can be traced back to a symbol and a file list rather than a category total.
    symbols = collections.defaultdict(lambda: collections.Counter())
    symbol_files = collections.defaultdict(lambda: collections.defaultdict(set))
    per_file = collections.defaultdict(lambda: collections.Counter())
    hwnd_files = collections.defaultdict(set)

    for dirpath, dirnames, filenames in os.walk(ROOT):
        dirnames[:] = [d for d in dirnames if d not in (".git", "build", "_deps")]
        for name in sorted(filenames):
            if not name.endswith(SOURCE_EXT):
                continue
            path = os.path.join(dirpath, name)
            rel = os.path.relpath(path, ROOT).replace(os.sep, "/")
            area, _ = area_of(rel)
            if area in SKIP_AREAS:
                continue
            with open(path, "r", errors="replace") as handle:
                text = strip_comments_and_strings(handle.read())
            if "HWND" in text:
                hwnd_files[area].add(rel)
            for category, compiled in patterns.items():
                for symbol, regex in compiled:
                    found = regex.findall(text)
                    if not found:
                        continue
                    bucket = "in_scope" if area_of(rel)[1] else "out_of_scope"
                    counts[area][category] += len(found)
                    files[area][category].add(rel)
                    symbols[bucket][symbol] += len(found)
                    symbol_files[bucket][symbol].add(rel)
                    per_file[rel][category] += len(found)
    return counts, files, symbols, per_file, hwnd_files, symbol_files


def summarise(counts, files, hwnd_files):
    in_scope_areas = [name for name, in_scope, _ in AREAS if in_scope]
    out_areas = [name for name, in_scope, _ in AREAS if not in_scope]
    totals = {"in_scope": collections.Counter(), "out_of_scope": collections.Counter()}
    file_totals = {"in_scope": collections.defaultdict(set),
                   "out_of_scope": collections.defaultdict(set)}
    for area in counts:
        bucket = "in_scope" if area in in_scope_areas else "out_of_scope"
        totals[bucket].update(counts[area])
        for category, paths in files[area].items():
            file_totals[bucket][category] |= paths
    hwnd = {
        "in_scope": sorted(p for a in in_scope_areas for p in hwnd_files.get(a, ())),
        "out_of_scope": sorted(p for a in out_areas for p in hwnd_files.get(a, ())),
    }
    return totals, file_totals, hwnd


def report(counts, files, symbols, per_file, hwnd_files, symbol_files, stream=sys.stdout):
    totals, file_totals, hwnd = summarise(counts, files, hwnd_files)
    w = stream.write
    w("Win32 window / event loop / input surface\n")
    w("=" * 72 + "\n\n")
    w("%-16s %10s %8s %12s %8s\n" % ("category", "in-scope", "files", "out-of-scope",
                                     "files"))
    for category in CATEGORIES:
        w("%-16s %10d %8d %12d %8d\n" % (
            category,
            totals["in_scope"][category], len(file_totals["in_scope"][category]),
            totals["out_of_scope"][category], len(file_totals["out_of_scope"][category])))

    def distinct(bucket):
        groups = file_totals[bucket].values()
        return len(set().union(*groups) if groups else set())

    w("%-16s %10d %8d %12d %8d\n" % (
        "TOTAL",
        sum(totals["in_scope"].values()), distinct("in_scope"),
        sum(totals["out_of_scope"].values()), distinct("out_of_scope")))

    w("\nfiles mentioning HWND: %d in scope, %d out of scope\n"
      % (len(hwnd["in_scope"]), len(hwnd["out_of_scope"])))
    for rel in hwnd["in_scope"]:
        w("  %s\n" % rel)

    w("\nper area\n")
    for area, _, _ in AREAS:
        if area not in counts:
            continue
        w("  %-10s %5d hits across %3d files\n"
          % (area, sum(counts[area].values()),
             len(set().union(*files[area].values()) if files[area] else set())))

    w("\nthe files the seam has to replace\n")
    for rel in SPOTLIGHT:
        row = per_file.get(rel)
        if row is None:
            w("  %-70s MISSING\n" % rel)
            continue
        w("  %-70s %4d\n" % (rel, sum(row.values())))
        for category, n in row.most_common():
            w("      %-16s %4d\n" % (category, n))

    w("\nin-scope symbols, most used first\n")
    for symbol, n in symbols["in_scope"].most_common():
        w("  %-24s %4d in %2d file(s)\n" % (symbol, n, len(symbol_files["in_scope"][symbol])))


def as_json(counts, files, symbols, per_file, hwnd_files, symbol_files):
    totals, file_totals, hwnd = summarise(counts, files, hwnd_files)
    return {
        "_comment": "Produced by scripts/window-input-scan.py; gated in CI by the same "
                    "script's --check mode. Update with --check --update.",
        "totals": {bucket: dict(totals[bucket]) for bucket in totals},
        "files": {bucket: {c: len(p) for c, p in file_totals[bucket].items()}
                  for bucket in file_totals},
        "hwnd_files": hwnd,
        "per_area": {area: dict(counts[area]) for area in sorted(counts)},
        "symbols": {bucket: dict(symbols[bucket]) for bucket in symbols},
        "in_scope_symbol_files": {symbol: sorted(paths) for symbol, paths
                                  in symbol_files["in_scope"].items()},
        "spotlight": {rel: dict(per_file.get(rel, {})) for rel in SPOTLIGHT},
    }


def markdown(counts, files, symbols, per_file, hwnd_files, symbol_files):
    totals, file_totals, hwnd = summarise(counts, files, hwnd_files)
    lines = ["| Category | In-scope references | In-scope files | Out-of-scope references |",
             "|---|---:|---:|---:|"]
    for category in CATEGORIES:
        lines.append("| `%s` | %d | %d | %d |" % (
            category, totals["in_scope"][category],
            len(file_totals["in_scope"][category]), totals["out_of_scope"][category]))
    lines.append("| **Total** | **%d** | **%d** | **%d** |" % (
        sum(totals["in_scope"].values()),
        len(set().union(*file_totals["in_scope"].values())),
        sum(totals["out_of_scope"].values())))
    lines.append("")
    lines.append("| File | References |")
    lines.append("|---|---:|")
    for rel in SPOTLIGHT:
        lines.append("| `%s` | %d |" % (rel, sum(per_file.get(rel, {}).values())))
    lines.append("")
    lines.append("| Symbol | In-scope references | In-scope files |")
    lines.append("|---|---:|---:|")
    for symbol, n in symbols["in_scope"].most_common():
        lines.append("| `%s` | %d | %d |"
                     % (symbol, n, len(symbol_files["in_scope"][symbol])))
    return "\n".join(lines) + "\n"


def check(payload, update):
    if update:
        with open(BASELINE, "w") as handle:
            json.dump(payload, handle, indent=2, sort_keys=True)
            handle.write("\n")
        print("wrote %s" % os.path.relpath(BASELINE, ROOT))
        return 0
    if not os.path.isfile(BASELINE):
        print("FAIL: no baseline at %s" % os.path.relpath(BASELINE, ROOT), file=sys.stderr)
        return 1
    with open(BASELINE) as handle:
        expected = json.load(handle)
    failures = []
    for bucket in ("in_scope", "out_of_scope"):
        want = expected["totals"].get(bucket, {})
        got = payload["totals"].get(bucket, {})
        for category in sorted(set(want) | set(got)):
            if want.get(category, 0) != got.get(category, 0):
                failures.append("totals.%s.%s: %s -> %s"
                                % (bucket, category, want.get(category, 0),
                                   got.get(category, 0)))
    want_hwnd = expected["hwnd_files"]["in_scope"]
    got_hwnd = payload["hwnd_files"]["in_scope"]
    for rel in sorted(set(want_hwnd) ^ set(got_hwnd)):
        failures.append("hwnd_files.in_scope: %s %s" % (
            rel, "removed" if rel in want_hwnd else "added"))

    print("in-scope references: %d (baseline %d), in-scope HWND files: %d (baseline %d)" % (
        sum(payload["totals"]["in_scope"].values()),
        sum(expected["totals"]["in_scope"].values()),
        len(got_hwnd), len(want_hwnd)))
    if failures:
        print("\nFAIL: the Win32 window/input surface moved and the baseline was not updated",
              file=sys.stderr)
        for line in failures:
            print("  - %s" % line, file=sys.stderr)
        print("\nIf the change is intentional, in the same PR run:\n"
              "  python3 scripts/window-input-scan.py --check --update", file=sys.stderr)
        return 1
    print("OK: matches %s" % os.path.relpath(BASELINE, ROOT))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json", help="write the machine-readable counts here")
    ap.add_argument("--markdown", help="write the doc's tables here")
    ap.add_argument("--check", action="store_true", help="gate against the checked-in baseline")
    ap.add_argument("--update", action="store_true", help="with --check, re-baseline")
    args = ap.parse_args()

    counts, files, symbols, per_file, hwnd_files, symbol_files = scan()
    payload = as_json(counts, files, symbols, per_file, hwnd_files, symbol_files)

    if args.json:
        with open(args.json, "w") as handle:
            json.dump(payload, handle, indent=2, sort_keys=True)
            handle.write("\n")
    if args.markdown:
        with open(args.markdown, "w") as handle:
            handle.write(markdown(counts, files, symbols, per_file, hwnd_files, symbol_files))
    if args.check:
        return check(payload, args.update)
    report(counts, files, symbols, per_file, hwnd_files, symbol_files)
    return 0


if __name__ == "__main__":
    sys.exit(main())

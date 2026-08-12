#!/usr/bin/env python3
"""Check the window backends' scan-code tables against KeyScanCodes.h.

The seam hands the engine PC/AT set-1 scan codes, because that is what KeyDefType stores and
what the saved key bindings and the INI files contain. Each backend therefore carries a table
from its own key numbering (SDL scan codes, macOS kVK_* virtual keys) to a set-1 value, and a
wrong entry in one of those tables is a silently remapped key rather than a build error - the
exact failure the DIK_ static_asserts in KeyScanCodes.h exist to prevent on Windows.

This is the equivalent gate for the non-Windows backends: every set-1 value in every table must
match the KEYSCAN_* constant it claims to be, no two entries may claim the same physical key,
and the set of KEYSCAN_* constants a backend does not produce must be exactly the documented
gap. It cannot check the *left* column - only a real X11/Wayland session or a real Mac can say
whether SDL_SCANCODE_INTERNATIONAL2 is the key labelled Kana - so that stays an unverified
claim, called out in docs/porting/window-event-loop.md.

Usage:
    python3 scripts/ci/check-window-scancodes.py
"""
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
HEADER = os.path.join(ROOT, "Core", "GameEngine", "Include", "GameClient", "KeyScanCodes.h")
PLATFORM = os.path.join(ROOT, "Core", "Libraries", "Source", "WWVegas", "WWLib", "platform")

# backend file -> the KEYSCAN_* names that backend legitimately cannot produce.
BACKENDS = {
    "platform_window_sdl2.cpp": set(),
    # macOS has no Num Lock, no Pause and no Menu key, and its JIS layout does not expose
    # separate Convert/NoConvert/Circumflex keys, so these three have no kVK_ to map from.
    "platform_window_cocoa.mm": {"KEYSCAN_CONVERT", "KEYSCAN_NOCONVERT", "KEYSCAN_CIRCUMFLEX"},
}

HEADER_ENTRY = re.compile(r"^\s*(KEYSCAN_[A-Z0-9_]+)\s*=\s*(0x[0-9A-Fa-f]+)\s*,", re.M)
TABLE_ENTRY = re.compile(
    r"\{\s*([A-Za-z0-9_]+)\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*\"(KEYSCAN_[A-Z0-9_]+)\"\s*\}")


def read(path):
    with open(path, "r", errors="replace") as handle:
        return handle.read()


def main():
    failures = []
    header = dict((name, int(value, 16)) for name, value in HEADER_ENTRY.findall(read(HEADER)))
    if not header:
        print("FAIL: parsed no KEYSCAN_* constants out of %s"
              % os.path.relpath(HEADER, ROOT), file=sys.stderr)
        return 1
    print("KeyScanCodes.h: %d KEYSCAN_* constants" % len(header))

    for filename, allowed_gap in sorted(BACKENDS.items()):
        path = os.path.join(PLATFORM, filename)
        if not os.path.isfile(path):
            failures.append("%s: missing" % filename)
            continue
        entries = TABLE_ENTRY.findall(read(path))
        if not entries:
            failures.append("%s: parsed no scan-code table entries" % filename)
            continue

        seen_native = {}
        seen_set1 = {}
        for native, value_text, name in entries:
            value = int(value_text, 16)
            if name not in header:
                failures.append("%s: %s is not a KEYSCAN_* constant" % (filename, name))
            elif header[name] != value:
                failures.append("%s: %s mapped to %#04x but KeyScanCodes.h says %#04x"
                                % (filename, name, value, header[name]))
            if native in seen_native:
                failures.append("%s: %s mapped twice (%s and %s)"
                                % (filename, native, seen_native[native], name))
            seen_native[native] = name
            if value in seen_set1:
                failures.append("%s: set-1 %#04x claimed by both %s and %s"
                                % (filename, value, seen_set1[value], name))
            seen_set1[value] = name

        produced = set(name for _, _, name in entries)
        gap = set(header) - produced
        if gap != allowed_gap:
            for name in sorted(gap - allowed_gap):
                failures.append("%s: %s is no longer mapped; map it or add it to the "
                                "documented gap in this script" % (filename, name))
            for name in sorted(allowed_gap - gap):
                failures.append("%s: %s is now mapped but still listed as a gap in this "
                                "script; remove it from BACKENDS" % (filename, name))
        print("%-32s %3d entries, %d unmapped (%s)"
              % (filename, len(entries), len(gap),
                 ", ".join(sorted(gap)) if gap else "none"))

    if failures:
        print("\nFAIL: %d scan-code problem(s)" % len(failures), file=sys.stderr)
        for line in failures:
            print("  - %s" % line, file=sys.stderr)
        return 1
    print("\nOK: every backend table agrees with KeyScanCodes.h")
    return 0


if __name__ == "__main__":
    sys.exit(main())

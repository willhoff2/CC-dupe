#!/usr/bin/env python3
"""Audit `SurfaceClass::Lock` callers, including the ones outside the 19 D3D8 files.

docs/porting/renderer-resource-seam.md §7.1 left a hazard open: `SurfaceClass::Lock`
hands out a read-write pointer, so the seam has to assume usage class C8 (read-write),
and a caller that *reads* a surface the GPU wrote would see the staging bytes rather
than the GPU's result. The classification in `d3d8-lock-classes.json` covers the 19
files that contain a direct D3D8 lock call; `SurfaceClass::Lock` callers live anywhere
in the engine, and those were never counted.

This counts them, mechanically:

  * a call site is `X->Lock(&pitch)` or `X->Lock(&pitch, min, max)` -- one or three
    arguments with an address-of first argument. That shape is unique to
    `SurfaceClass::Lock`: the D3D8 buffer `Lock` takes four, and `MutexClass::Lock`
    takes none;
  * for each site, the pointer variable it assigns is followed to the matching
    `Unlock()` in the same function, and every mention of it is classified as a store
    (the variable, indexed or dereferenced, on the left of an assignment) or a load
    (anything else: on the right of an assignment, passed to a call, compared);
  * the enclosing function's provenance is checked for whether the surface can hold
    GPU-written contents: a render target, a back buffer, or a surface obtained from
    a texture that the GPU renders into. Only a *load* from such a surface is the §7.1
    hazard; a load from a surface the CPU itself filled is not.

`--check` compares the result against the committed classification in
`surface-lock-audit.json`, so a new `SurfaceClass::Lock` caller cannot be added without
someone deciding which of those it is.

Run:  python3 spikes/renderer/tools/surface-lock-audit.py
      python3 spikes/renderer/tools/surface-lock-audit.py --check
"""
import argparse
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
CLASSES_JSON = os.path.join(os.path.dirname(__file__), "d3d8-lock-classes.json")
AUDIT_JSON = os.path.join(os.path.dirname(__file__), "surface-lock-audit.json")

TREES = ["Core", "Generals/Code", "GeneralsMD/Code"]
SKIP_DIRS = {"Tools", "Babylon"}
SOURCE_EXT = (".cpp", ".h", ".inl")

# `x->Lock(&pitch)` / `x.Lock(&pitch, min, max)`: the SurfaceClass shape.
LOCK_RE = re.compile(r"(?P<object>[A-Za-z_][A-Za-z0-9_]*)\s*(?:->|\.)Lock\s*\("
                     r"\s*(?P<args>[^;]*?)\)\s*")
FUNC_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_:<>,\s\*&]*?"
                     r"(?P<name>[A-Za-z_][A-Za-z0-9_]*(?:::[~A-Za-z_][A-Za-z0-9_]*)?)\s*\([^;]*$")

# Provenance of a surface whose contents can come from the GPU.
GPU_SOURCE_RE = re.compile(r"Get_Render_Target|GetBackBuffer|Get_Back_Buffer|"
                           r"GetRenderTarget|Peek_Render_Target|_Get_DX8_Back_Buffer|"
                           r"Get_D3D_Back_Buffer|CreateCopy\s*\(")


def audited_files():
    data = json.load(open(CLASSES_JSON))
    return {key.split("::", 1)[0] for key in data["assignments"]}


def source_files():
    for tree in TREES:
        for dirpath, dirnames, filenames in os.walk(os.path.join(ROOT, tree)):
            dirnames[:] = [d for d in dirnames if d not in SKIP_DIRS]
            for name in sorted(filenames):
                if name.endswith(SOURCE_EXT):
                    yield os.path.relpath(os.path.join(dirpath, name), ROOT)


def split_args(text):
    """Top-level comma split, so `Lock(&p, Vector2i(a,b), c)` counts as three."""
    args, depth, current = [], 0, ""
    for ch in text:
        if ch in "([":
            depth += 1
        elif ch in ")]":
            depth -= 1
        if ch == "," and depth == 0:
            args.append(current.strip())
            current = ""
        else:
            current += ch
    if current.strip():
        args.append(current.strip())
    return args


def enclosing_function(lines, index):
    """Nearest preceding line that looks like a function definition."""
    for i in range(index, -1, -1):
        line = lines[i].rstrip()
        if not line or line.lstrip().startswith(("//", "*", "/*", "#")):
            continue
        match = FUNC_RE.match(line)
        if match and not line.lstrip().startswith(("if", "for", "while", "switch", "return",
                                                   "else")):
            return match.group("name")
    return "?"


def classify_uses(lines, start, variable):
    """-> (stores, loads, evidence) for `variable` between the lock and its Unlock."""
    stores = loads = 0
    evidence = []
    base = re.escape(variable)
    use_re = re.compile(rf"\b{base}\b")
    # Bare name, optionally indexed or dereferenced, on the left of a single '='.
    store_re = re.compile(rf"(?:\*\s*)?\(?\s*(?:\([^)]*\)\s*)?{base}\s*(?:\[[^\]]*\])?\s*"
                          r"(?:\+\+|--)?\s*(?:[+\-*/|&^]?=)(?!=)")
    for i in range(start + 1, min(start + 160, len(lines))):
        line = lines[i]
        if re.search(r"(?:->|\.)Unlock\s*\(\s*\)", line):
            break
        code = line.split("//", 1)[0]
        if not use_re.search(code):
            continue
        if store_re.search(code.strip()):
            stores += 1
        else:
            loads += 1
            if len(evidence) < 3:
                evidence.append(f"{i + 1}: {code.strip()[:100]}")
    return stores, loads, evidence


def function_body(lines, index):
    """A window around the lock, used only for provenance sniffing."""
    return "\n".join(lines[max(0, index - 60):index + 5])


def scan():
    audited = audited_files()
    sites = []
    for path in source_files():
        with open(os.path.join(ROOT, path), "r", errors="replace") as handle:
            lines = handle.read().splitlines()
        for i, line in enumerate(lines):
            code = line.split("//", 1)[0]
            for match in LOCK_RE.finditer(code):
                args = split_args(match.group("args"))
                if len(args) not in (1, 3) or not args[0].lstrip("(").startswith(("&", "(Int*)",
                                                                                 "(int*)")):
                    continue
                if "&" not in args[0]:
                    continue
                before = code[:match.start()]
                variable = None
                if "=" in before:
                    lhs = before.rsplit("=", 1)[0]
                    names = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", lhs)
                    variable = names[-1] if names else None
                stores = loads = 0
                evidence = []
                if variable:
                    stores, loads, evidence = classify_uses(lines, i, variable)
                body = function_body(lines, i)
                sites.append({
                    "file": path,
                    "line": i + 1,
                    "function": enclosing_function(lines, i),
                    "object": match.group("object"),
                    "rect": len(args) == 3,
                    "variable": variable,
                    "audited_file": path in audited,
                    "stores": stores,
                    "loads": loads,
                    "gpu_provenance": bool(GPU_SOURCE_RE.search(body)),
                    "evidence": evidence,
                })
    return sites


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="compare against the committed audit")
    ap.add_argument("--update", action="store_true", help="rewrite the committed audit's counts")
    ap.add_argument("--json", help="write the raw scan here")
    args = ap.parse_args()

    sites = scan()
    outside = [s for s in sites if not s["audited_file"]]
    readers = [s for s in outside if s["loads"] > 0]
    hazards = [s for s in readers if s["gpu_provenance"]]

    print(f"SurfaceClass::Lock call sites: {len(sites)} "
          f"({len(sites) - len(outside)} in the 19 audited files, {len(outside)} outside)")
    print(f"\n{'file:line':78s} {'function':34s} {'st':>3s} {'ld':>3s} gpu")
    for site in outside:
        print(f"{site['file'] + ':' + str(site['line']):78s} {site['function'][:34]:34s} "
              f"{site['stores']:3d} {site['loads']:3d} {'yes' if site['gpu_provenance'] else '-'}")
        for line in site["evidence"]:
            print(f"    load {line}")
    print(f"\noutside the audited files:      {len(outside)}")
    print(f"  of those, ones that read:     {len(readers)}")
    print(f"  of those, on a surface the GPU can have written (the §7.1 hazard): {len(hazards)}")
    for site in hazards:
        print(f"    {site['file']}:{site['line']} {site['function']}")

    if args.json:
        with open(args.json, "w") as handle:
            json.dump(sites, handle, indent=1)

    expected = json.load(open(AUDIT_JSON)) if os.path.isfile(AUDIT_JSON) else None
    if args.update:
        payload = {
            "_comment": expected["_comment"] if expected else [],
            "expected": {
                "sites_total": len(sites),
                "sites_outside_audited_files": len(outside),
                "readers_outside_audited_files": len(readers),
                "gpu_written_readers": len(hazards),
            },
            "sites": [{k: s[k] for k in ("file", "line", "function", "stores", "loads",
                                         "gpu_provenance", "audited_file")} for s in sites],
        }
        with open(AUDIT_JSON, "w") as handle:
            json.dump(payload, handle, indent=1)
            handle.write("\n")
        print(f"\nwrote {os.path.relpath(AUDIT_JSON, ROOT)}")
        return 0

    if args.check:
        if expected is None:
            print("FAIL: no committed audit; create one with --update", file=sys.stderr)
            return 2
        want = expected["expected"]
        got = {
            "sites_total": len(sites),
            "sites_outside_audited_files": len(outside),
            "readers_outside_audited_files": len(readers),
            "gpu_written_readers": len(hazards),
        }
        failures = [f"{key}: {value} != committed {want[key]}"
                    for key, value in got.items() if want.get(key) != value]
        if failures:
            print("\nFAIL: the SurfaceClass::Lock audit is out of date:", file=sys.stderr)
            for failure in failures:
                print(f"  {failure}", file=sys.stderr)
            print("  a new caller needs a class decision -- see renderer-resource-seam.md §7.1,"
                  " then rerun with --update", file=sys.stderr)
            return 1
        print("\nOK: matches the committed audit")
    return 0


if __name__ == "__main__":
    sys.exit(main())

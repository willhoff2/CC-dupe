#!/usr/bin/env python3
"""Characterise the D3D8 *lock* surface: what every Lock/Unlock call site actually does.

`d3d8-resource-scan.py` answers "how many resource-interface call sites are there".
It does not answer the question that decides whether the resource seam can be made
backend-neutral: **what contract does each lock site rely on?**  A lock that fills a
freshly created system-memory texture once is a staging upload; a lock that takes
`D3DLOCK_DISCARD` on a dynamic vertex buffer every frame is a ring allocator; a
`D3DLOCK_READONLY` lock on a surface is a GPU->CPU read-back with a stall in it.  Those
three map onto completely different Vulkan code.

So this scanner, per lock site, extracts the mechanical facts:

  * interface and method, enclosing function, file and line;
  * the lock flags argument, verbatim (`D3DLOCK_*`, `0`, or a non-constant expression);
  * whether the rect/box argument is NULL (whole sub-resource) or a rect (partial);
  * for textures, the mip-level argument (constant or a loop variable);
  * whether the returned `Pitch` / `RowPitch` is read at all;
  * whether the pointer is stored into a member/array (i.e. the lock outlives the
    statement, and possibly the function) or consumed locally;
  * how far away the matching Unlock is, in lines.

and then checks each site against `d3d8-lock-classes.json`, which assigns a usage
class to each *(file, enclosing function)*.  Keying on the function rather than the
line number means the table survives edits above it; a lock site in a function that
is not in the table fails the run, so a new lock site cannot be added without
someone deciding which class it is in.

It also re-derives the `d3d8-resource-scan.py` totals, with three heuristic differences
that `docs/porting/renderer-resource-seam.md` accounts for site by site: a call regex
that tolerates a call split over several lines, literal and comment handling that does
not misread log strings as calls, and per-file resolution of variable names too generic
to resolve globally.  Both totals are printed so the difference is visible rather than
silently corrected.

Run:  python3 spikes/renderer/tools/d3d8-lock-scan.py
      python3 spikes/renderer/tools/d3d8-lock-scan.py --check   # CI gate
"""
import argparse
import collections
import json
import os
import re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
CLASSES_JSON = os.path.join(os.path.dirname(__file__), "d3d8-lock-classes.json")

UNKNOWN = set("QueryInterface AddRef Release".split())
RESOURCE = set("""GetDevice SetPrivateData GetPrivateData FreePrivateData SetPriority
GetPriority PreLoad GetType""".split())
BASETEXTURE = RESOURCE | set("SetLOD GetLOD GetLevelCount".split())

INTERFACES = {
    "IDirect3DBaseTexture8": BASETEXTURE,
    "IDirect3DTexture8": BASETEXTURE | set(
        "GetLevelDesc GetSurfaceLevel LockRect UnlockRect AddDirtyRect".split()),
    "IDirect3DCubeTexture8": BASETEXTURE | set(
        "GetLevelDesc GetCubeMapSurface LockRect UnlockRect AddDirtyRect".split()),
    "IDirect3DVolumeTexture8": BASETEXTURE | set(
        "GetLevelDesc GetVolumeLevel LockBox UnlockBox AddDirtyBox".split()),
    "IDirect3DVertexBuffer8": RESOURCE | set("Lock Unlock GetDesc".split()),
    "IDirect3DIndexBuffer8": RESOURCE | set("Lock Unlock GetDesc".split()),
    "IDirect3DSurface8": set(
        "GetDevice SetPrivateData GetPrivateData FreePrivateData GetContainer GetDesc "
        "LockRect UnlockRect".split()),
    "IDirect3DVolume8": set(
        "GetDevice SetPrivateData GetPrivateData FreePrivateData GetContainer GetDesc "
        "LockBox UnlockBox".split()),
    "IDirect3DSwapChain8": set("Present GetBackBuffer".split()),
}
for _iface in INTERFACES:
    INTERFACES[_iface] |= UNKNOWN

LP_ALIASES = {"LP" + name.upper()[1:]: name for name in INTERFACES}

LOCKS = {"LockRect", "Lock", "LockBox"}
UNLOCKS = {"UnlockRect", "Unlock", "UnlockBox"}

# Which positional argument is what, per interface method.  None where the method
# does not have that argument.
#            (level_arg, rect_arg, flags_arg, arg_count)
ARG_SHAPE = {
    ("IDirect3DSurface8", "LockRect"): (None, 1, 2, 3),
    ("IDirect3DTexture8", "LockRect"): (0, 2, 3, 4),
    ("IDirect3DCubeTexture8", "LockRect"): (1, 3, 4, 5),
    ("IDirect3DVolumeTexture8", "LockBox"): (0, 2, 3, 4),
    ("IDirect3DVolume8", "LockBox"): (None, 1, 2, 3),
    ("IDirect3DVertexBuffer8", "Lock"): (None, None, 3, 4),
    ("IDirect3DIndexBuffer8", "Lock"): (None, None, 3, 4),
}

NULLISH = {"NULL", "nullptr", "0", "(RECT*)0", "(D3DBOX*)0"}

# Operation groups, for the per-operation table.
OP_GROUP = {
    "AddRef": "reference counting", "Release": "reference counting",
    "QueryInterface": "reference counting",
    "LockRect": "lock", "Lock": "lock", "LockBox": "lock",
    "UnlockRect": "unlock", "Unlock": "unlock", "UnlockBox": "unlock",
    "GetDesc": "describe", "GetLevelDesc": "describe", "GetLevelCount": "describe",
    "GetSurfaceLevel": "sub-resource", "GetCubeMapSurface": "sub-resource",
    "GetVolumeLevel": "sub-resource", "GetBackBuffer": "sub-resource",
    "GetContainer": "sub-resource",
    "SetLOD": "residency", "GetLOD": "residency",
    "SetPriority": "residency", "GetPriority": "residency", "PreLoad": "residency",
    "AddDirtyRect": "dirty region", "AddDirtyBox": "dirty region",
}


def in_scope(rel):
    if rel.startswith("Generals/"):
        return False
    if "/Tools/" in "/" + rel:
        return False
    return rel.startswith("Core/") or rel.startswith("GeneralsMD/")


# Strings, character literals and both comment forms, as one alternation.  Stripping them
# in separate passes is wrong: `W3DTreeBuffer.cpp` has a `//` comment containing `/*`, so a
# `/\*.*?\*/` pass run first swallows 22 KB of real code up to the next `*/`, which silently
# lost that file's `UnlockRect` call.
SKIP_RE = re.compile(r'"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|//[^\n]*|/\*.*?\*/', re.S)


def sanitize(text):
    """Drop comments and blank out literal contents, preserving line numbers.

    Blanking literals is load-bearing, not hygiene: `dx8vertexbuffer.cpp`'s
    VERTEX_BUFFER_LOG block contains the literal "VertexBuffer->Lock(start_index: %d,
    ...)", which any regex looking for `x->Lock(` matches.  `d3d8-resource-scan.py` does
    not strip literals and therefore counts those log strings as call sites.
    """
    def repl(m):
        s = m.group(0)
        if s.startswith("//"):
            return ""
        if s.startswith("/*"):
            return "\n" * s.count("\n")
        # A literal can span lines: `\` at the end of a line continues it, and
        # `(?:\\.)` with re.S consumes that newline.  Blanking it away shifts every
        # line number after it -- W3DWater.cpp lost 25 lines that way -- so the
        # newlines are kept and only the rest of the content is blanked.
        body = "".join("\n" if c == "\n" else " " for c in s[1:-1])
        return s[0] + body + s[-1]
    return SKIP_RE.sub(repl, text)


DECL_RE = re.compile(
    r"\b(?P<type>IDirect3D(?:Base|Cube|Volume)?(?:Texture|Surface|Volume|VertexBuffer|"
    r"IndexBuffer|SwapChain)8|LPDIRECT3D(?:BASE|CUBE|VOLUME)?(?:TEXTURE|SURFACE|VOLUME|"
    r"VERTEXBUFFER|INDEXBUFFER|SWAPCHAIN)8)\s*(?P<stars>\**)\s*(?P<name>\w+)")

# `obj->Method(`, tolerating newlines anywhere the compiler tolerates them, which the
# resource scanner's line-at-a-time regex does not.
CALL_RE_MULTILINE = re.compile(
    r"([A-Za-z_]\w*)\s*(?:\(\s*[^()]*\))?\s*->\s*([A-Za-z_]\w*)\s*\(", re.S)
# The resource scanner's regex, applied line by line, kept here only to report the
# difference between the two.
CALL_RE_ONELINE = re.compile(r"([A-Za-z_]\w*)\s*(?:\([^()]*\))?\s*->\s*([A-Za-z_]\w*)\s*\(")

GENERIC_NAMES = ("texture", "surface", "buffer", "tex", "surf", "p", "ptr")


def load_sources():
    sources = {}
    for dirpath, _, filenames in os.walk(ROOT):
        if "/.git" in dirpath:
            continue
        for f in filenames:
            if not f.endswith((".cpp", ".h", ".hpp", ".inl")):
                continue
            rel = os.path.relpath(os.path.join(dirpath, f), ROOT)
            if in_scope(rel):
                with open(os.path.join(dirpath, f), errors="replace") as fh:
                    sources[rel] = sanitize(fh.read())
    return sources


# Any `Type *name` declaration, used only to notice that a name is *also* declared as a
# pointer to something that is not a D3D8 interface, in which case attributing its calls
# would be a guess.
ANY_PTR_DECL_RE = re.compile(r"\b([A-Za-z_]\w*)\s*\*+\s*(?:const\s+)?([A-Za-z_]\w*)\s*[=;,)\[]")


def collect_aliases(sources):
    """(global, per_file, ambiguous): name -> interface.

    `global` is every D3D8-typed name declared anywhere in scope, minus names too generic
    to attribute across files -- this is `d3d8-resource-scan.py`'s model, and it is what
    resolves accessors declared in a header and called from a .cpp (`Peek_D3D_Texture`).

    `per_file` additionally resolves the generic names (`surface`, `tex`, ...) *within the
    file that declares them*, which the global model has to drop.  A name that the same
    file also declares as a pointer to some other type lands in `ambiguous` instead of
    being counted, so the residual uncertainty is a printed number rather than a silent
    guess.
    """
    glob = {}
    per_file = {}
    ambiguous = collections.defaultdict(set)
    for rel, text in sources.items():
        local = {}
        for m in DECL_RE.finditer(text):
            typename = m.group("type")
            iface = LP_ALIASES.get(typename, typename)
            glob[m.group("name")] = iface
            local[m.group("name")] = iface
        for m in ANY_PTR_DECL_RE.finditer(text):
            typename, name = m.group(1), m.group(2)
            if name in local and typename not in INTERFACES and typename not in LP_ALIASES:
                ambiguous[rel].add(name)
        per_file[rel] = {k: v for k, v in local.items() if k not in ambiguous[rel]}
    for generic in GENERIC_NAMES:
        glob.pop(generic, None)
    return glob, per_file, {k: v for k, v in ambiguous.items() if v}


def split_args(text, open_paren):
    """Return (arg_strings, index_just_past_closing_paren) for the call at open_paren."""
    depth = 0
    args = []
    cur = []
    i = open_paren
    while i < len(text):
        c = text[i]
        if c in "([{":
            depth += 1
            if depth == 1:
                i += 1
                continue
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                args.append("".join(cur))
                return [" ".join(a.split()) for a in args], i + 1
        if depth == 1 and c == ",":
            args.append("".join(cur))
            cur = []
        else:
            cur.append(c)
        i += 1
    return [" ".join(a.split()) for a in args], len(text)


BLOCK_KEYWORDS = {"if", "else", "for", "while", "switch", "do", "try", "catch",
                  "sizeof", "return", "case", "default", "struct", "class", "union",
                  "enum", "namespace", "extern", "WWASSERT", "DEBUG_ASSERTCRASH"}


def enclosing_block_start(text, pos):
    """Index of the `{` opening the innermost block containing `pos`, or None."""
    depth = 0
    for i in range(pos - 1, -1, -1):
        c = text[i]
        if c == "}":
            depth += 1
        elif c == "{":
            if depth == 0:
                return i
            depth -= 1
    return None


def matching_open_paren(text, close):
    depth = 0
    for i in range(close, -1, -1):
        if text[i] == ")":
            depth += 1
        elif text[i] == "(":
            depth -= 1
            if depth == 0:
                return i
    return None


def block_owner(text, brace):
    """The name introducing the block that `text[brace] == '{'` opens.

    `switch (VertexBuffer->Type()) {` must answer `switch`, not `Type`, so the
    parenthesised part is skipped as a unit rather than scanned for names.
    """
    head = text[:brace].rstrip()
    if head.endswith("else"):
        return "else"
    # `Class::Class(args) : Base(x), Member(y) {` -- peel the member initialisers, whose
    # names are not the function's name, until the parenthesised group is the argument
    # list itself.
    for _ in range(24):
        if not head.endswith(")"):
            break
        open_paren = matching_open_paren(head, len(head) - 1)
        if open_paren is None:
            return ""
        head = head[:open_paren].rstrip()
        m = re.search(r"([A-Za-z_~]\w*(?:\s*::\s*~?\w+)*)\s*$", head)
        if m is None:
            return ""
        before = head[:m.start()].rstrip()
        if before.endswith(":") or before.endswith(","):
            head = before.rstrip(":,").rstrip()
            continue
        return re.sub(r"\s+", "", m.group(1))
    m = re.search(r"([A-Za-z_~]\w*(?:\s*::\s*~?\w+)*)\s*$", head.rstrip(":").rstrip())
    return re.sub(r"\s+", "", m.group(1)) if m else ""


def enclosing_function(text, pos):
    """Name of the function whose body contains `pos`.

    Walks outwards one block at a time, because the innermost block is usually an `if`,
    a `for` or a `switch`, and keying the class table on control flow would be useless.
    """
    at = pos
    for _ in range(16):
        start = enclosing_block_start(text, at)
        if start is None:
            return "?"
        name = block_owner(text, start)
        if name and name.split("::")[-1] not in BLOCK_KEYWORDS:
            return name
        at = start
    return "?"


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def scan(sources, aliases, per_file=None):
    """Return (calls, lock_sites).  calls: list of (rel, line, iface, method)."""
    calls = []
    locks = []
    per_file = per_file or {}
    for rel, text in sorted(sources.items()):
        local = per_file.get(rel, {})
        for m in CALL_RE_MULTILINE.finditer(text):
            obj, method = m.group(1), m.group(2)
            iface = local.get(obj) or aliases.get(obj)
            if iface is None or method not in INTERFACES[iface]:
                continue
            line = line_of(text, m.start())
            calls.append((rel, line, iface, method))
            if method not in LOCKS and method not in UNLOCKS:
                continue
            open_paren = text.index("(", m.end() - 1)
            args, end = split_args(text, open_paren)
            fn = enclosing_function(text, m.start())
            site = {
                "file": rel, "line": line, "interface": iface, "method": method,
                "function": fn, "object": obj, "args": args,
            }
            if method in LOCKS:
                level_i, rect_i, flags_i, argc = ARG_SHAPE[(iface, method)]

                def arg(i, missing="?"):
                    return args[i] if i is not None and len(args) > i else missing

                site["flags"] = arg(flags_i)
                if rect_i is not None and len(args) > rect_i:
                    site["extent"] = ("whole" if args[rect_i] in NULLISH
                                      else "partial:" + args[rect_i])
                else:
                    site["extent"] = "range:%s+%s" % (args[0], args[1]) if len(args) > 1 else "?"
                site["level"] = arg(level_i, "-")
                # Does anything read the pitch within the next 40 lines?
                window = text[m.start():m.start() + 4000]
                site["reads_pitch"] = bool(re.search(r"\.\s*(?:Row)?Pitch|SlicePitch", window))
                # Does the raw pointer escape the statement -- stored into a member or an
                # array indexed by mip level, rather than consumed in place?
                site["stores_pointer"] = bool(
                    re.search(r"(?:Locked\w*Ptr|locked_rects\s*\[)", window)
                    or re.search(r"\w+\s*\[[^\]]*\]\s*=[^;]*pBits", window))
                locks.append(site)
            else:
                locks.append(site)
    return calls, locks


def oneline_total(sources, aliases):
    n = 0
    for text in sources.values():
        for line in text.splitlines():
            for obj, method in CALL_RE_ONELINE.findall(line):
                iface = aliases.get(obj)
                if iface is not None and method in INTERFACES[iface]:
                    n += 1
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true",
                    help="fail if a lock site is not covered by d3d8-lock-classes.json")
    ap.add_argument("--json-out", help="write every lock site's facts to this file")
    args = ap.parse_args()

    sources = load_sources()
    aliases, per_file, ambiguous = collect_aliases(sources)
    calls, locks = scan(sources, aliases, per_file)

    per_iface = collections.Counter()
    per_method = collections.defaultdict(collections.Counter)
    files = collections.Counter()
    per_group = collections.Counter()
    for rel, _line, iface, method in calls:
        per_iface[iface] += 1
        per_method[iface][method] += 1
        files[rel] += 1
        per_group[OP_GROUP.get(method, "other")] += 1

    total = len(calls)
    refcount = per_group["reference counting"]
    lock_unlock = per_group["lock"] + per_group["unlock"]
    print(f"D3D8 resource-interface call sites: {total}"
          f"  (the resource scanner's line-at-a-time regex, on the"
          f" same text, sees {oneline_total(sources, aliases)})")
    print(f"  reference counting: {refcount}")
    print(f"  real resource operations: {total - refcount}")
    print(f"  lock/unlock: {lock_unlock}"
          f"  ({per_group['lock']} lock, {per_group['unlock']} unlock)")
    print(f"  files: {len(files)}")
    ambiguous_names = sum(len(v) for v in ambiguous.values())
    print(f"  names left unattributed because one file declares them both as a D3D8"
          f" pointer and as something else: {ambiguous_names}")
    for rel in sorted(ambiguous):
        print(f"      {rel}: {', '.join(sorted(ambiguous[rel]))}")
    print()

    print("| interface | sites | methods |")
    print("| --- | ---: | --- |")
    for iface, n in sorted(per_iface.items(), key=lambda kv: (-kv[1], kv[0])):
        methods = ", ".join(f"`{m}` {c}" for m, c in
                            sorted(per_method[iface].items(), key=lambda kv: (-kv[1], kv[0])))
        print(f"| `{iface}` | {n} | {methods} |")
    print()

    print("| operation group | sites |")
    print("| --- | ---: |")
    for group, n in sorted(per_group.items(), key=lambda kv: (-kv[1], kv[0])):
        print(f"| {group} | {n} |")
    print()

    print("| file | sites |")
    print("| --- | ---: |")
    for rel, n in files.most_common():
        print(f"| `{rel}` | {n} |")
    print()

    with open(CLASSES_JSON) as fh:
        table = json.load(fh)
    classes = table["classes"]
    assignments = table["assignments"]

    per_class = collections.Counter()
    unclassified = []
    for site in locks:
        key = f"{site['file']}::{site['function']}"
        cls = assignments.get(key)
        site["class"] = cls
        if cls is None:
            unclassified.append(key)
        else:
            per_class[cls] += 1

    print("| usage class | lock/unlock sites | what the caller does with the pointer |")
    print("| --- | ---: | --- |")
    for cls in classes:
        print(f"| {cls['id']} — {cls['name']} | {per_class[cls['id']]} | {cls['contract']} |")
    print(f"| (unclassified) | {len(unclassified)} | |")
    print()

    print("lock sites, with the facts the class assignment is based on:")
    for site in sorted(locks, key=lambda s: (s["class"] or "~", s["file"], s["line"])):
        if site["method"] in UNLOCKS:
            continue
        print(f"  [{site['class']}] {site['file']}:{site['line']} "
              f"{site['function']} {site['interface']}::{site['method']}")
        print(f"        flags={site['flags']!r} extent={site['extent']!r} "
              f"level={site['level']!r} reads_pitch={site['reads_pitch']} "
              f"stores_pointer={site['stores_pointer']}")

    known_classes = {c["id"] for c in classes}
    bad_classes = sorted(set(assignments.values()) - known_classes)
    stale = sorted(set(assignments) -
                   {f"{s['file']}::{s['function']}" for s in locks})

    print()
    print(f"lock/unlock sites: {len(locks)}; classified: {len(locks) - len(unclassified)}; "
          f"classes: {len(classes)}")
    if stale:
        print(f"table entries matching no call site ({len(stale)}):")
        for key in stale:
            print(f"  {key}")
    if unclassified:
        print(f"call sites not in the table ({len(unclassified)}):")
        for key in sorted(set(unclassified)):
            print(f"  {key}")

    if args.json_out:
        with open(args.json_out, "w") as fh:
            json.dump(locks, fh, indent=1)

    if args.check:
        problems = []
        if unclassified:
            problems.append(f"{len(unclassified)} lock/unlock site(s) with no usage class")
        if stale:
            problems.append(f"{len(stale)} table entry/entries matching no call site")
        if bad_classes:
            problems.append(f"unknown class id(s): {', '.join(bad_classes)}")
        expected = table["expected"]
        for name, value in (("total", total), ("refcount", refcount),
                            ("lock_unlock", lock_unlock), ("files", len(files))):
            if expected[name] != value:
                problems.append(f"{name}: expected {expected[name]}, measured {value}")
        if problems:
            print()
            for p in problems:
                print(f"FAIL: {p}")
            raise SystemExit(1)
        print("OK: every lock/unlock site is classified and the counts match the baseline")


if __name__ == "__main__":
    main()

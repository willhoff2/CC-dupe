#!/usr/bin/env python3
"""Enumerate the raw-block serialisation sites in the save/replay path and classify
each one by whether the type it writes changes size or layout under LP64.

A "raw block" is any call that hands a pointer and a byte count to the serialiser
(``Xfer::xferUser``, ``Xfer::xferImplementation``) or writes an object straight to a
file (``fwrite``/``fread`` of an address, ``memcpy`` into a save buffer).

The classification is deliberately conservative: anything whose width or padding is
not pinned down by the C++ standard on both ILP32-Windows and LP64 is reported as
unstable, and everything else has to be provably fixed width.

Usage:
    python3 scripts/xfer-blob-audit.py [--json out.json] [--markdown out.md]
"""

from __future__ import annotations

import argparse
import collections
import json
import pathlib
import re
import subprocess
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent

# Areas that the native port actually has to carry. Tools and the Generals base game
# are cut from the port (see docs/porting/next-slice-scope.md) but are still counted
# separately so the "how big is this really" question has an honest answer.
IN_SCOPE = ("Core/GameEngine", "Core/GameEngineDevice", "GeneralsMD/Code/GameEngine",
            "GeneralsMD/Code/GameEngineDevice")
OUT_OF_SCOPE = ("Generals/Code", "Core/Tools", "GeneralsMD/Code/Tools")

SEARCH_ROOTS = ("Core", "Generals", "GeneralsMD")

CALL_RE = re.compile(r"\b(xferUser|xferImplementation)\s*\(")
FWRITE_RE = re.compile(r"\b(fwrite|fread)\s*\(\s*&")
MEMCPY_RE = re.compile(r"\bmemcpy\s*\(")

# `memcpy` is everywhere and most of it has nothing to do with a save file. These are the files
# that implement the save game, the replay and the CRC, so a memcpy in one of them is copying
# bytes that are about to be written to disk or folded into a checksum.
SAVE_PATH_FILES = (
    "Core/GameEngine/Source/Common/System/Xfer",
    "Core/GameEngine/Source/Common/System/Snapshot.cpp",
    "Core/GameEngine/Source/Common/System/DataChunk.cpp",
    "GeneralsMD/Code/GameEngine/Source/Common/Recorder.cpp",
    "GeneralsMD/Code/GameEngine/Source/Common/System/SaveGame/",
)

# Types whose size is fixed by <cstdint> or by the language on every target we care about.
STABLE_SCALARS = {
    "Byte", "UnsignedByte", "Bool", "bool", "char", "signed char", "unsigned char",
    "Short", "UnsignedShort", "Int", "UnsignedInt", "Int64", "UnsignedInt64",
    "Real", "float", "double", "int8_t", "uint8_t", "int16_t", "uint16_t",
    "int32_t", "uint32_t", "int64_t", "uint64_t",
}

# Types that are known to move under LP64 regardless of where they are used.
UNSTABLE_SCALARS = {
    "long", "unsigned long", "size_t", "time_t", "ptrdiff_t", "intptr_t", "uintptr_t",
    "HANDLE", "HWND", "HMODULE", "wchar_t", "WideChar", "void *", "SYSTEMTIME",
    "FILETIME", "LARGE_INTEGER",
}


def git_files():
    out = subprocess.run(["git", "ls-files", "*.cpp", "*.h", "*.inl"],
                         cwd=REPO_ROOT, capture_output=True, text=True, check=True)
    return [p for p in out.stdout.splitlines()
            if p.startswith(SEARCH_ROOTS)]


def area_of(path: str) -> str:
    for a in OUT_OF_SCOPE:
        if path.startswith(a):
            return "out-of-scope"
    for a in IN_SCOPE:
        if path.startswith(a):
            return "in-scope"
    return "other"


def balanced_args(text: str, open_idx: int):
    """Return the argument list of a call whose '(' is at open_idx, or None."""
    depth = 0
    for i in range(open_idx, min(len(text), open_idx + 4000)):
        c = text[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth == 0:
                return text[open_idx + 1:i]
    return None


def split_top_level(args: str):
    parts, depth, cur = [], 0, []
    for c in args:
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        if c == "," and depth == 0:
            parts.append("".join(cur).strip())
            cur = []
        else:
            cur.append(c)
    if cur:
        parts.append("".join(cur).strip())
    return parts


SIZEOF_RE = re.compile(
    r"sizeof\s*(?:\(\s*(?P<paren>[^()]*(?:\([^()]*\))?[^()]*)\s*\)"
    r"|\s+(?P<bare>[A-Za-z_][\w:]*))"
)


def sizeof_operands(expr: str):
    return [(m.group("paren") or m.group("bare") or "").strip()
            for m in SIZEOF_RE.finditer(expr)]


class Index:
    """A cheap whole-repo index of enum/struct/class/typedef declarations."""

    def __init__(self, files):
        self.enums = {}          # name -> underlying spelling or None
        self.records = set()     # struct/class/union names
        self.typedefs = {}       # name -> spelled-out type
        self.members = {}        # member name -> declared type spelling
        mem_re = re.compile(
            r"^[ \t]*(?:mutable\s+|static\s+|const\s+)*"
            r"([A-Za-z_][\w:]*(?:\s*\*)?)\s+(m_[A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*[;=]",
            re.M)
        enum_re = re.compile(
            r"\benum\s+(?:class\s+)?([A-Za-z_]\w*)\s*"
            r"(?:CPP_11\s*\(\s*:\s*([\w ]+?)\s*\)|:\s*([\w ]+?))?\s*[{;]")
        rec_re = re.compile(
            r"\b(?:struct|class|union)\s+([A-Za-z_]\w*)\s*(?:final\s*)?(?::[^{;]*)?\{")
        td_re = re.compile(r"\btypedef\s+([^;{}()]+?)\s+([A-Za-z_]\w*)\s*;")
        comment_re = re.compile(r"//[^\n]*|/\*.*?\*/", re.S)
        for rel in files:
            try:
                text = comment_re.sub("", (REPO_ROOT / rel).read_text(errors="ignore"))
            except OSError:
                continue
            for m in enum_re.finditer(text):
                name = m.group(1)
                underlying = (m.group(2) or m.group(3) or "").strip() or None
                prev = self.enums.get(name)
                if prev is None or underlying:
                    self.enums[name] = underlying
            for m in rec_re.finditer(text):
                self.records.add(m.group(1))
            for m in td_re.finditer(text):
                self.typedefs.setdefault(m.group(2), m.group(1).strip())
            for m in mem_re.finditer(text):
                ty, name = m.group(1).strip(), m.group(2)
                if ty in ("return", "case", "else", "typedef"):
                    continue
                self.members.setdefault(name, ty)

    def resolve(self, name: str, depth: int = 0):
        """Follow typedefs to a base spelling."""
        n = name.strip()
        seen = set()
        while n in self.typedefs and n not in seen and depth < 8:
            seen.add(n)
            n = self.typedefs[n].strip()
            depth += 1
        return n


_LOCAL_DECLS = {}


def local_decl_type(text: str, pos: int, name: str):
    """Type of the nearest preceding declaration of `name` in the same file."""
    key = (id(text), name)
    decls = _LOCAL_DECLS.get(key)
    if decls is None:
        pat = re.compile(r"\b([A-Za-z_][\w:]*)\s+" + re.escape(name) +
                         r"\s*(?:\[[^\]]*\])?\s*(?:=[^;]*)?;")
        kw = {"return", "case", "else", "delete", "const", "static", "typedef", "break"}
        decls = [(m.start(), m.group(1)) for m in pat.finditer(text)
                 if m.group(1) not in kw]
        _LOCAL_DECLS[key] = decls
    best = None
    for start, ty in decls:
        if start < pos:
            best = ty
    return best


def classify(operand: str, idx: Index, text: str = "", pos: int = 0):
    """Return (category, detail) for a sizeof operand."""
    op = operand.strip()
    if not op:
        return "unknown", "empty"
    if op.endswith("*") or op == "this":
        return "pointer", "pointer-sized operand"
    base = re.sub(r"\bconst\b|\bstruct\b|\bclass\b", "", op).strip()
    base = base.split("[")[0].strip()
    if base.startswith("*"):
        base = base[1:].strip()
    resolved = idx.resolve(base)
    if base in UNSTABLE_SCALARS:
        # Report the spelling, not what it resolves to: WideChar is a typedef of unsigned short
        # on Windows and of a four-byte wchar_t natively, which is the whole problem with it.
        return "unstable-scalar", base
    if resolved in UNSTABLE_SCALARS:
        return "unstable-scalar", resolved
    if resolved in STABLE_SCALARS or base in STABLE_SCALARS:
        return "stable-scalar", resolved
    if resolved in idx.enums or base in idx.enums:
        underlying = idx.enums.get(resolved, idx.enums.get(base))
        if underlying:
            return "enum-fixed", f"{resolved} : {underlying}"
        return "enum-open", resolved
    if resolved in idx.records or base in idx.records:
        return "record", base if base in idx.records else resolved
    if resolved.startswith("BitFlags<") or base.endswith("MaskType"):
        return "record", "BitFlags<>"
    tail = re.split(r"\.|->", base)[-1].strip()
    tail = tail.split("[")[0].strip()
    if text and re.match(r"^\w+$", tail):
        ty = local_decl_type(text, pos, tail)
        if ty:
            cat, detail = classify(ty, idx)
            if cat != "unknown":
                return cat, f"{detail} ({tail})"
    if tail in idx.members:
        cat, detail = classify(idx.members[tail], idx)
        if cat != "unknown":
            return cat, f"{detail} ({tail})"
    if re.match(r"^m_|^[a-z]\w*(\.\w+)*$", base) or "." in base or "->" in base:
        return "expression", base
    return "unknown", base


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--json")
    ap.add_argument("--markdown")
    args = ap.parse_args()

    files = git_files()
    # Generals and GeneralsMD are two copies of the same engine and share type names, so a name
    # has to be resolved against the copy it was used in or a fix in one game reads as a fix in
    # both.
    indexes = {
        "GeneralsMD": Index([f for f in files if not f.startswith("Generals/")]),
        "Generals": Index([f for f in files if not f.startswith("GeneralsMD/")]),
    }

    sites = []
    fwrite_sites = []
    memcpy_sites = []

    for rel in files:
        try:
            text = (REPO_ROOT / rel).read_text(errors="ignore")
        except OSError:
            continue
        if "xfer" not in text and "fwrite" not in text and "memcpy" not in text:
            continue
        line_starts = [0]
        for i, c in enumerate(text):
            if c == "\n":
                line_starts.append(i + 1)

        def line_of(pos):
            lo, hi = 0, len(line_starts) - 1
            while lo < hi:
                mid = (lo + hi + 1) // 2
                if line_starts[mid] <= pos:
                    lo = mid
                else:
                    hi = mid - 1
            return lo + 1

        idx = indexes["Generals" if rel.startswith("Generals/") else "GeneralsMD"]

        for m in CALL_RE.finditer(text):
            open_idx = m.end() - 1
            arglist = balanced_args(text, open_idx)
            if arglist is None:
                continue
            parts = split_top_level(arglist)
            if len(parts) != 2:
                continue          # declaration or definition, not a call
            if re.match(r"^(?:const\s+)?void\s*\*\s*\w+$", parts[0].strip()):
                continue          # the declaration of xferUser itself, not a call
            operands = sizeof_operands(parts[1])
            if operands:
                cats = [classify(o, idx, text, m.start()) for o in operands]
                # the widest hazard wins
                order = ["pointer", "unstable-scalar", "enum-open", "record",
                         "unknown", "expression", "enum-fixed", "stable-scalar"]
                cat, detail = sorted(cats, key=lambda c: order.index(c[0]))[0]
            else:
                cat, detail = "no-sizeof", parts[1].strip()
            sites.append({
                "file": rel, "line": line_of(m.start()), "entry": m.group(1),
                "size_expr": parts[1].strip(), "operands": operands,
                "category": cat, "detail": detail, "area": area_of(rel),
            })

        for m in FWRITE_RE.finditer(text):
            fwrite_sites.append({"file": rel, "line": line_of(m.start()),
                                 "entry": m.group(1), "area": area_of(rel)})
        for m in MEMCPY_RE.finditer(text):
            memcpy_sites.append({"file": rel, "line": line_of(m.start()),
                                 "area": area_of(rel),
                                 "save_path": rel.startswith(SAVE_PATH_FILES)})

    result = {
        "xfer_sites": sites,
        "fwrite_addr_of_sites": fwrite_sites,
        "memcpy_sites": memcpy_sites,
    }

    by_cat = collections.Counter(s["category"] for s in sites)
    in_scope = [s for s in sites if s["area"] == "in-scope"]
    by_cat_in = collections.Counter(s["category"] for s in in_scope)

    lines = []
    lines.append("## Raw-block serialisation sites\n")
    lines.append(f"- `xferUser`/`xferImplementation` call sites: **{len(sites)}** "
                 f"in **{len(set(s['file'] for s in sites))}** files")
    lines.append(f"- of those, in ported scope (Core + GeneralsMD engine/device): "
                 f"**{len(in_scope)}** in **{len(set(s['file'] for s in in_scope))}** files")
    lines.append(f"- `fwrite`/`fread` of an address: **{len(fwrite_sites)}** "
                 f"in **{len(set(s['file'] for s in fwrite_sites))}** files")
    save_memcpy = [s for s in memcpy_sites if s["save_path"]]
    lines.append(f"- `memcpy` call sites: **{len(memcpy_sites)}** "
                 f"in **{len(set(s['file'] for s in memcpy_sites))}** files, of which "
                 f"**{len(save_memcpy)}** are in the save/replay/CRC implementation itself\n")
    lines.append("| Category | All | In ported scope |")
    lines.append("|---|---:|---:|")
    for cat, n in by_cat.most_common():
        lines.append(f"| {cat} | {n} | {by_cat_in.get(cat, 0)} |")

    def base_detail(s):
        return s["detail"].split(" (")[0]

    records = collections.Counter(base_detail(s) for s in sites if s["category"] == "record")
    lines.append(f"\n### Distinct struct/class types written as raw blocks: "
                 f"**{len(records)}**\n")
    lines.append("| Type | Sites |")
    lines.append("|---|---:|")
    for name, n in records.most_common():
        lines.append(f"| `{name}` | {n} |")

    open_enums = collections.Counter(base_detail(s) for s in sites if s["category"] == "enum-open")
    lines.append(f"\n### Enums with no fixed underlying type: **{len(open_enums)}** distinct\n")
    for name, n in open_enums.most_common():
        lines.append(f"- `{name}` ({n} sites)")

    md = "\n".join(lines) + "\n"
    if args.markdown:
        pathlib.Path(args.markdown).write_text(md)
    else:
        sys.stdout.write(md)
    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(result, indent=1))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Check the OpenAL backend is interface-complete against its own mss.h.

Building is not enough: the backend is a drop-in replacement for mss32, so every `AIL_*` entry
point it declares must also be defined, or the eventual link fails long after this job was
green. Declarations come from the header, definitions from `nm` over the built archive.
"""
import argparse
import pathlib
import re
import subprocess
import sys

DECL = re.compile(r"\b(AIL_\w+)\s*\(", re.M)


def declared(header):
    text = pathlib.Path(header).read_text(errors="replace")
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    names = set()
    for line in text.splitlines():
        # Declarations only: a prototype line, not a macro alias or a call.
        if line.lstrip().startswith("#"):
            continue
        for m in DECL.finditer(line):
            names.add(m.group(1))
    return names


def defined(archive):
    out = subprocess.run(["nm", "--defined-only", str(archive)],
                         capture_output=True, text=True, check=True).stdout
    names = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in ("T", "t", "W"):
            names.add(parts[2])
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--header", required=True)
    ap.add_argument("--archive", required=True)
    args = ap.parse_args()

    decls = declared(args.header)
    defs = defined(args.archive)
    missing = sorted(n for n in decls if n not in defs)

    print(f"{len(decls)} AIL_* entry points declared in {args.header}")
    print(f"{len(defs & decls)} of them defined in {args.archive}")
    if missing:
        print(f"\nFAIL: {len(missing)} declared AIL_* entry point(s) have no definition:",
              file=sys.stderr)
        for name in missing:
            print(f"  - {name}", file=sys.stderr)
        return 1
    print("OK: the OpenAL backend defines every AIL_* entry point it declares")
    return 0


if __name__ == "__main__":
    sys.exit(main())

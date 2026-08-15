#!/usr/bin/env python3
"""Enumerate the Miles (`AIL_*`) API surface the engine uses, and how much of it the OpenAL
backend actually implements.

Two independent measurements, both auditable:

 1. **Demand.** Every `AIL_*` identifier referenced by the engine's audio consumers
    (`Core/Libraries/Source/WWVegas/WWAudio` and `Core/GameEngineDevice`, excluding the OpenAL
    backend itself and excluding any `*/Tools/*` tree, which the port plan cuts). Counted two
    ways, because they differ and the difference matters: `raw` over the file text as written
    (which is how the numbers in docs/porting/audio-surface.md were originally measured, so
    comments and log strings mentioning an `AIL_*` name are included), and `code` with comments
    and string literals stripped, which is the number of references a compiler sees.
    An identifier is classified as a *function* if it is followed by `(` at any reference site or
    is declared as a prototype in `mss.h`; otherwise it is a constant/macro.

 2. **Supply.** For each demanded function, what the OpenAL backend
    (`Core/Libraries/Source/OpenALAudioDevice`) provides:

      * `alias`       -- a `#define` in `mss.h` onto another entry point; no symbol of its own.
      * `missing`     -- not declared or not defined at all: the link would fail.
      * `no-op`       -- defined, but the body does nothing observable (empty, or a single
                         `return <literal>`). The call is accepted and discarded.
      * `recorded`    -- defined, mutates backend state (so queries stay self-consistent) but
                         never reaches OpenAL, directly or transitively.
      * `implemented` -- defined, and reaches a real `al*`/`alc*` OpenAL entry point, directly
                         or through a helper defined in the backend (call graph closed to fixed
                         point over the backend's own functions).

    `implemented` is a claim about *plumbing*, not about audio fidelity: the classifier proves the
    call reaches OpenAL, not that what OpenAL then does matches Miles. Approximations are called
    out per function in docs/porting/audio-device-seam.md.

Usage:
    python3 scripts/audio-surface-scan.py [--json out.json] [--markdown out.md]
"""
import argparse
import collections
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

# Consumers: the engine code that calls Miles.
CONSUMER_DIRS = [
    "Core/Libraries/Source/WWVegas/WWAudio",
    "Core/GameEngineDevice",
]
# The OpenAL backend: the supply side, never counted as a consumer.
BACKEND_DIR = "Core/Libraries/Source/OpenALAudioDevice"
BACKEND_HEADER = BACKEND_DIR + "/mss/mss.h"

SOURCE_EXT = (".cpp", ".h", ".hpp", ".c", ".inl")

AIL = re.compile(r"\bAIL_[A-Za-z0-9_]+")
AIL_CALL = re.compile(r"\b(AIL_[A-Za-z0-9_]+)\s*\(")
OPENAL_CALL = re.compile(r"\b(al[A-Z][A-Za-z0-9_]*|alc[A-Z][A-Za-z0-9_]*)\s*\(")


def strip_comments_and_strings(text):
    """Remove // and /* */ comments and the contents of string/char literals."""
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


def walk(rel_dir):
    base = os.path.join(ROOT, rel_dir)
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = [d for d in dirnames if d != "Tools"]
        for name in sorted(filenames):
            if name.endswith(SOURCE_EXT):
                yield os.path.join(dirpath, name)


def read(path):
    with open(path, "r", errors="replace") as handle:
        return handle.read()


class Demand:
    """AIL_* references in the engine, counted over raw text and over code-only text."""

    def __init__(self):
        self.raw_per_file = collections.Counter()
        self.raw_refs = collections.Counter()
        self.code_per_file = collections.Counter()
        self.code_refs = collections.Counter()
        self.called = set()


def measure_demand():
    demand = Demand()
    backend_abs = os.path.join(ROOT, BACKEND_DIR)
    for rel_dir in CONSUMER_DIRS:
        for path in walk(rel_dir):
            if path.startswith(backend_abs):
                continue
            rel = os.path.relpath(path, ROOT)
            raw = read(path)
            raw_names = AIL.findall(raw)
            if raw_names:
                demand.raw_per_file[rel] += len(raw_names)
                demand.raw_refs.update(raw_names)
            code = strip_comments_and_strings(raw)
            code_names = AIL.findall(code)
            if code_names:
                demand.code_per_file[rel] += len(code_names)
                demand.code_refs.update(code_names)
                demand.called.update(AIL_CALL.findall(code))
    return demand


def header_aliases(header_text):
    """`#define AIL_a AIL_b` aliases: names with no symbol of their own."""
    aliases = {}
    for match in re.finditer(r"^\s*#\s*define\s+(AIL_\w+)\s+(AIL_\w+)\s*$",
                             header_text, re.M):
        aliases[match.group(1)] = match.group(2)
    return aliases


def declared_prototypes(header_text):
    """Names declared as a prototype (not a macro alias) in a Miles-shaped header."""
    names = set()
    for line in header_text.splitlines():
        if line.lstrip().startswith("#"):
            continue
        names.update(AIL_CALL.findall(line))
    return names


def split_top_level_functions(text):
    """Yield (name, body) for every brace-balanced definition in a C++ translation unit.

    Deliberately crude: it keys on `<name>(` followed by a balanced `{...}` with no `;` between
    the closing paren and the brace, which catches free functions, static functions and
    out-of-line member definitions (`Class::method`). That is enough for a call graph over the
    backend's own code.
    """
    pattern = re.compile(r"([A-Za-z_~][A-Za-z0-9_:~]*)\s*\(")
    for match in pattern.finditer(text):
        name = match.group(1)
        # Balance the parameter list.
        depth, i, n = 0, match.end() - 1, len(text)
        while i < n:
            if text[i] == "(":
                depth += 1
            elif text[i] == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        if i >= n:
            continue
        j = i + 1
        # Skip trailing qualifiers (const, noexcept, initialiser lists...) up to `{` or `;`.
        while j < n and text[j] not in "{;":
            if text[j] == ")":  # unbalanced: not a definition head
                break
            j += 1
        if j >= n or text[j] != "{":
            continue
        depth, k = 0, j
        while k < n:
            if text[k] == "{":
                depth += 1
            elif text[k] == "}":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        yield name, text[j + 1:k]


def measure_supply():
    """Classify every AIL_* definition in the backend. Returns (classes, defined, declared)."""
    bodies = {}
    for path in walk(BACKEND_DIR):
        if not path.endswith((".cpp", ".c")):
            continue
        text = strip_comments_and_strings(read(path))
        for name, body in split_top_level_functions(text):
            # Later definitions never occur; keep the first.
            bodies.setdefault(name, body)

    header = strip_comments_and_strings(read(os.path.join(ROOT, BACKEND_HEADER)))
    declared = declared_prototypes(header)

    # Direct OpenAL use per backend function, plus the intra-backend call graph.
    direct = {}
    edges = {}
    for name, body in bodies.items():
        direct[name] = bool(OPENAL_CALL.search(body))
        callees = set()
        for match in re.finditer(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(", body):
            callees.add(match.group(1))
        member_call_re = r"\.\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(|->\s*([A-Za-z_][A-Za-z0-9_]*)\s*\("
        for match in re.finditer(member_call_re, body):
            callees.add(match.group(1) or match.group(2))
        edges[name] = callees

    # Method definitions are keyed as Class::method; index them by bare method name too, since
    # call sites say `obj->method(...)`.
    by_bare = collections.defaultdict(set)
    for name in bodies:
        by_bare[name.split("::")[-1]].add(name)

    reaches = dict(direct)
    changed = True
    while changed:
        changed = False
        for name, callees in edges.items():
            if reaches.get(name):
                continue
            for callee in callees:
                for target in by_bare.get(callee, ()):  # noqa: B007
                    if reaches.get(target):
                        reaches[name] = True
                        changed = True
                        break
                if reaches.get(name):
                    break

    classes = {}
    for name, body in bodies.items():
        if not name.startswith("AIL_"):
            continue
        # `(void)arg;` discards are how the backend spells "deliberately ignored", so a body
        # made only of those (plus at most a literal return) is a no-op, not recorded state.
        stripped = re.sub(r"\(\s*void\s*\)\s*[A-Za-z0-9_]+\s*;", "", body).strip()
        trivial = stripped == "" or re.fullmatch(
            r"return\s*[A-Za-z0-9_:\-\.\(\)]*\s*;", stripped)
        if reaches.get(name):
            classes[name] = "implemented"
        elif trivial:
            classes[name] = "no-op"
        else:
            classes[name] = "recorded"
    return classes, set(n for n in bodies if n.startswith("AIL_")), declared


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--json")
    ap.add_argument("--markdown")
    ap.add_argument("--check", action="store_true",
                    help="exit non-zero if the backend does not cover every demanded function")
    args = ap.parse_args()

    demand = measure_demand()
    classes, defined, _ = measure_supply()

    backend_header = read(os.path.join(ROOT, BACKEND_HEADER))
    header_protos = declared_prototypes(strip_comments_and_strings(backend_header))
    aliases = header_aliases(backend_header)

    # The coverage table is over what the compiler sees: names that only occur in comments
    # cannot make a link fail.
    refs = demand.code_refs
    functions = sorted(n for n in refs if n in demand.called or n in header_protos
                       or n in aliases)
    constants = sorted(n for n in refs if n not in functions)

    coverage = {}
    for name in functions:
        target = aliases.get(name)
        if target is not None:
            coverage[name] = f"alias -> {target} ({classes.get(target, 'missing')})"
        elif name in classes:
            coverage[name] = classes[name]
        else:
            coverage[name] = "missing"

    def bucket(kind):
        return kind.split(" ")[0]

    buckets = collections.Counter(bucket(k) for k in coverage.values())
    missing = sorted(n for n, k in coverage.items() if bucket(k) == "missing")
    unused = sorted(n for n in defined if n not in refs)

    print("Demand -- AIL_* references in the engine's audio consumers (code only;")
    print("          raw text, comments and log strings included, in parentheses)")
    print(f"  distinct identifiers       {len(refs):>4}"
          f"  ({len(demand.raw_refs)})")
    print(f"    ... functions            {len(functions):>4}")
    print(f"    ... constants / macros   {len(constants):>4}")
    print(f"  reference sites            {sum(refs.values()):>4}"
          f"  ({sum(demand.raw_refs.values())})")
    print(f"  files                      {len(demand.raw_per_file):>4}")
    for rel, count in demand.code_per_file.most_common():
        print(f"    {count:>4}  ({demand.raw_per_file[rel]})  {rel}")
    print()
    print("Supply -- what Core/Libraries/Source/OpenALAudioDevice provides")
    for kind in ("implemented", "recorded", "no-op", "alias", "missing"):
        print(f"  {kind:<13} {buckets.get(kind, 0):>4}")
    print(f"  entry points defined by the backend   {len(defined)}")
    print(f"  prototypes declared by mss.h          {len(header_protos)}")
    print(f"  defined but never referenced          {len(unused)}")
    if missing:
        print("\nDemanded but MISSING from the backend:")
        for name in missing:
            print(f"  - {name}")

    result = {
        "distinct_identifiers_code": len(refs),
        "distinct_identifiers_raw": len(demand.raw_refs),
        "functions_code": len(functions),
        "constants_code": len(constants),
        "reference_sites_code": sum(refs.values()),
        "reference_sites_raw": sum(demand.raw_refs.values()),
        "files": len(demand.raw_per_file),
        "per_file_raw": dict(demand.raw_per_file.most_common()),
        "per_file_code": dict(demand.code_per_file.most_common()),
        "refs": dict(refs.most_common()),
        "function_names": functions,
        "constant_names": constants,
        "coverage": coverage,
        "coverage_counts": dict(buckets),
        "backend_defines": sorted(defined),
        "backend_declares": sorted(header_protos),
        "backend_aliases": aliases,
        "defined_but_unreferenced": unused,
    }
    if args.json:
        with open(args.json, "w") as handle:
            json.dump(result, handle, indent=2, sort_keys=True)
            handle.write("\n")
    if args.markdown:
        with open(args.markdown, "w") as handle:
            handle.write("| Function | Coverage | Sites |\n|---|---|---:|\n")
            for name in functions:
                handle.write(f"| `{name}` | {coverage[name]} | {refs[name]} |\n")
    return 1 if (missing and args.check) else 0


if __name__ == "__main__":
    sys.exit(main())

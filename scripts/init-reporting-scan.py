#!/usr/bin/env python3
"""Enumerate the Zero Hour startup initialisation path and classify how each entry point
reports failure.

`GameEngine::init()` is the single-player startup path: it creates the file system, loads the INI
tree, and initialises ~50 subsystems through `SubsystemInterfaceList::initSubsystem()`. Every one
of those `init()` entry points returns `void`, so the only way an initialisation failure can reach
the caller is by throwing. This script reads the call list out of `GameEngine::init()`, resolves
each entry to the class whose `init()` actually runs, finds that function's body, and classifies
what the body does when it cannot do its job:

    throws          - `throw` (or a helper that never returns), so `GameEngine::init()`'s catch
                      clauses turn it into a `RELEASE_CRASH` with a message.
    release-fatal   - `RELEASE_CRASH`/`RELEASE_CRASHLOCALIZED`: the process reports and exits in
                      every configuration.
    debug-only      - `DEBUG_CRASH`/`DEBUG_ASSERTCRASH` only: a release build carries on.
    silent-return   - a guarded `return;` with no report of any kind: the subsystem is left
                      half-initialised and startup continues as if it had succeeded.
    no-failure-path - no early return and no diagnostic: nothing was found to classify. This is
                      *not* a claim that the body cannot fail.

The classification is a static heuristic over the function body, deliberately a coarse one: it
answers "can this entry point report a failure at all", which is the question this slice exists to
answer, and it is the column a CI gate can hold. The failure *consequences* in
docs/porting/init-failure-reporting.md are hand-verified on top of it.

Usage:
    python3 scripts/init-reporting-scan.py [--json OUT] [--markdown OUT] [--quiet]
    python3 scripts/init-reporting-scan.py --check    # CI: no entry point may get quieter

Written by an LLM and reviewed by hand; the entry list it produces was checked against
`GameEngine::init()` by reading both.
"""

import argparse
import json
import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent

# Zero Hour first, per docs/porting/native-port-plan.md: the enumeration is of the GeneralsMD
# startup path. Generals/Code is out of scope for port purposes and is not scanned.
ENGINE_INIT = "GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp"

BASELINE = REPO_ROOT / "docs" / "porting" / "ci-baselines" / "init-reporting.json"
DOC = REPO_ROOT / "docs" / "porting" / "init-failure-reporting.md"

# Where an init() body may live. Core is shared; the GeneralsMD tree holds the Zero Hour
# implementations. Tools are out of scope.
SEARCH_DIRS = [
    "Core/GameEngine",
    "Core/GameEngineDevice",
    "Core/Libraries/Source/OpenALAudioDevice",
    "GeneralsMD/Code/GameEngine",
    "GeneralsMD/Code/GameEngineDevice",
]

SOURCE_SUFFIXES = (".cpp", ".h")

CLASSIFICATIONS = (
    "throws",
    "release-fatal",
    "debug-only",
    "silent-return",
    "no-failure-path",
    "not-found",
)


def read(path):
    return (REPO_ROOT / path).read_text(errors="replace")


def strip_comments(text):
    """Remove comments and preprocessor directives.

    Comments go so that a `DEBUG_CRASH` in one is not counted as evidence. Directives go because
    the scan is not a preprocessor: it classifies the union of every configuration's code, and a
    `#endif` immediately above a definition would otherwise be read as part of its return type.
    Conditional bodies are therefore all present at once, which is noted per row where it changes
    the reading.
    """
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", "", text)
    return re.sub(r"^[ \t]*#[^\n]*", "", text, flags=re.M)


def extract_body(text, open_index):
    """Return the text between the brace at or after open_index and its match."""
    start = text.find("{", open_index)
    if start < 0:
        return None
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:i]
    return None


def find_function(text, pattern):
    """Find `pattern` (a compiled regex over the signature) and return (return_type, body)."""
    for match in pattern.finditer(text):
        body = extract_body(text, match.end())
        if body is not None:
            return match, body
    return None, None


class Sources:
    """The in-scope source tree, comment-stripped, read once."""

    def __init__(self):
        self.files = {}
        for directory in SEARCH_DIRS:
            root = REPO_ROOT / directory
            if not root.is_dir():
                continue
            for path in sorted(root.rglob("*")):
                if path.suffix in SOURCE_SUFFIXES and path.is_file():
                    rel = path.relative_to(REPO_ROOT).as_posix()
                    self.files[rel] = strip_comments(path.read_text(errors="replace"))

    def base_class(self, class_name):
        """The first public base of `class_name`, so an unoverridden init() can be attributed to
        the class that actually defines it (`RadarDummy` has no init(); `Radar::init()` runs)."""
        pattern = re.compile(r"class\s+" + re.escape(class_name) +
                             r"\s*:\s*(?:public|protected|private)\s+([A-Za-z_][\w:]*)")
        for text in self.files.values():
            match = pattern.search(text)
            if match:
                return match.group(1)
        return None

    def find_init_chain(self, class_name):
        """find_init() over the class and then its bases, reporting which class defined it."""
        seen = []
        current = class_name
        while current and current not in seen:
            seen.append(current)
            found = self.find_init(current)
            if found is not None:
                found["defined_by"] = current
                return found
            current = self.base_class(current)
        return None

    def find_init(self, class_name):
        """The out-of-line `<class>::init(...)` definition, or an inline one in the class body."""
        out_of_line = re.compile(
            r"(?P<ret>[A-Za-z_][\w:*&<>\s]*?)\s+" + re.escape(class_name) +
            r"::init\s*\(\s*(?P<args>[^)]*)\)\s*(?:const\s*)?(?=\{)")
        for rel, text in self.files.items():
            match, body = find_function(text, out_of_line)
            if body is not None:
                return {
                    "file": rel,
                    "line": text[:match.start()].count("\n") + 1,
                    "returns": " ".join(match.group("ret").split()),
                    "args": " ".join(match.group("args").split()),
                    "body": body,
                }
        inline = re.compile(
            r"class\s+" + re.escape(class_name) + r"\b[^;{]*\{")
        for rel, text in self.files.items():
            match = inline.search(text)
            if not match:
                continue
            class_body = extract_body(text, match.end() - 1)
            if class_body is None:
                continue
            # Preprocessor lines would otherwise be picked up as part of a signature.
            class_body = re.sub(r"^[ \t]*#[^\n]*", "", class_body, flags=re.M)
            decl = re.compile(
                r"(?:virtual\s+)?(?P<ret>[A-Za-z_][\w:*&<>\s]*?)\s+init\s*\(\s*(?P<args>[^)]*)\)"
                r"\s*(?:const\s*)?(?:(?:override|final)\s*)*(?=\{)")
            inline_match, body = find_function(class_body, decl)
            if body is not None:
                return {
                    "file": rel,
                    "line": text[:match.start()].count("\n") + 1,
                    "returns": " ".join(inline_match.group("ret").split()),
                    "args": " ".join(inline_match.group("args").split()),
                    "body": body,
                    "inline": True,
                }
        return None


def classify(body):
    """Classify a function body by the loudest failure report it contains."""
    evidence = []
    if re.search(r"\bthrow\b", body):
        evidence.append("throw")
    if re.search(r"\bRELEASE_CRASH(LOCALIZED)?\s*\(", body):
        evidence.append("RELEASE_CRASH")
    if re.search(r"\bthrowInitFailure\s*\(", body):
        evidence.append("throwInitFailure")
    if re.search(r"\bDEBUG_(ASSERT)?CRASH\s*\(", body):
        evidence.append("DEBUG_CRASH")
    # A `return;` that is not the function's last statement is a guard: an early exit taken on
    # some condition. `return;` as the final statement is not evidence of anything.
    guards = len(re.findall(r"\breturn\s*;", body.rstrip().rsplit("}", 1)[0] or body))
    if guards:
        evidence.append("%d guarded return%s" % (guards, "" if guards == 1 else "s"))

    if "throw" in evidence or "throwInitFailure" in evidence:
        return "throws", evidence
    if "RELEASE_CRASH" in evidence:
        return "release-fatal", evidence
    if "DEBUG_CRASH" in evidence:
        return "debug-only", evidence
    if guards:
        return "silent-return", evidence
    return "no-failure-path", evidence


FACTORY_RE = re.compile(
    r"inline\s+[\w:*&<>\s]+?\b(?P<factory>\w+)::(?P<name>create\w+)\s*\([^)]*\)\s*"
    r"(?P<body>\{[^}]*\}|\s*\{)", re.S)


def factory_map(sources):
    """factory function name -> the concrete class(es) it hands back.

    Handles the `inline GameClient *Win32GameEngine::createGameClient()
    { return NEW W3DGameClient; }` shape the engine uses, plus out-of-line factories elsewhere.
    """
    classes = {}
    for rel, text in sources.files.items():
        for match in re.finditer(
                r"(?P<ret>[\w:]+)\s*\*\s*(?:(?P<owner>\w+)::)?(?P<name>[Cc]reate\w+)\s*"
                r"\([^)]*\)\s*(?=\{)", text):
            body = extract_body(text, match.end())
            if body is None:
                continue
            found = re.findall(r"\b(?:NEW|MSGNEW\s*\([^)]*\))\s+([A-Za-z_]\w*)", body)
            if found:
                classes.setdefault(match.group("name"), [])
                for name in found:
                    if name not in classes[match.group("name")]:
                        classes[match.group("name")].append(name)
    return classes


def resolve_class(expression, factories):
    """The class whose init() runs, from the expression that constructs the subsystem."""
    expression = expression.strip()
    direct = re.match(r"(?:NEW|MSGNEW\s*\([^)]*\))\s+([A-Za-z_]\w*)", expression)
    if direct:
        return [direct.group(1)]
    call = re.match(r"(?:\w+::)?([A-Za-z_]\w*)\s*\(", expression)
    if call:
        name = call.group(1)
        if name in factories:
            return list(factories[name])
        return []
    plain = re.match(r"([A-Za-z_]\w*)$", expression)
    if plain:
        # `initSubsystem(TheWritableGlobalData, ..., TheWritableGlobalData, ...)`: the object
        # already exists. Its class is the global's declared type.
        return [plain.group(1)]
    return []


GLOBAL_TYPES = {
    # Globals initialised directly in GameEngine::init() rather than through a factory, and the
    # pre-existing objects passed to initSubsystem() by name.
    "TheWritableGlobalData": "GlobalData",
    "TheNameKeyGenerator": "NameKeyGenerator",
    "TheCommandList": "CommandList",
    "TheGameLODManager": "GameLODManager",
    "TheMapCache": "MapCache",
    "TheSubsystemList": "SubsystemInterfaceList",
}


def split_args(text):
    """Split a call's argument list on commas that are not nested in brackets or strings."""
    args, depth, current, quote = [], 0, "", None
    for ch in text:
        if quote:
            current += ch
            if ch == quote:
                quote = None
            continue
        if ch in "\"'":
            quote = ch
            current += ch
            continue
        if ch in "([{<":
            depth += 1
        elif ch in ")]}>":
            depth -= 1
        if ch == "," and depth == 0:
            args.append(current)
            current = ""
            continue
        current += ch
    if current.strip():
        args.append(current)
    return [a.strip() for a in args]


def scan_entries(engine_body):
    """The ordered init entry points of GameEngine::init()."""
    entries = []
    for match in re.finditer(r"(initSubsystem\s*\(|\b(\w+)\s*->\s*init\s*\(\s*\))",
                             engine_body):
        if match.group(0).startswith("initSubsystem"):
            args_text = extract_call_args(engine_body, match.end() - 1)
            if args_text is None:
                continue
            args = split_args(args_text)
            if len(args) < 3:
                continue
            name = args[1].strip('"')
            entries.append({
                "entry": name,
                "how": "initSubsystem",
                "expression": args[2],
                "ini": [a.strip('"') for a in args[4:] if a.strip() != "nullptr"],
            })
        else:
            entries.append({
                "entry": match.group(2),
                "how": "direct init() call",
                "expression": match.group(2),
                "ini": [],
            })
    return entries


def extract_call_args(text, open_paren):
    depth = 0
    for i in range(open_paren, len(text)):
        if text[i] == "(":
            depth += 1
        elif text[i] == ")":
            depth -= 1
            if depth == 0:
                return text[open_paren + 1:i]
    return None


def build(sources):
    engine_text = strip_comments(read(ENGINE_INIT))
    match, engine_body = find_function(
        engine_text, re.compile(r"void\s+GameEngine::init\s*\(\s*\)\s*(?=\{)"))
    if engine_body is None:
        raise SystemExit("could not find GameEngine::init() in " + ENGINE_INIT)

    factories = factory_map(sources)
    results = []
    for entry in scan_entries(engine_body):
        expression = entry["expression"]
        candidates = resolve_class(expression, factories)
        candidates = [GLOBAL_TYPES.get(c, c) for c in candidates]
        records = []
        for class_name in candidates:
            found = sources.find_init_chain(class_name)
            if found is None:
                records.append({"class": class_name, "classification": "not-found",
                                "evidence": [], "returns": None, "file": None})
                continue
            classification, evidence = classify(found["body"])
            records.append({
                "class": class_name,
                "defines_init": found.get("defined_by", class_name),
                "classification": classification,
                "evidence": evidence,
                "returns": found["returns"],
                "file": found["file"],
                "line": found["line"],
            })
        if not records:
            records = [{"class": None, "classification": "not-found", "evidence": [],
                        "returns": None, "file": None}]
        results.append({
            "entry": entry["entry"],
            "how": entry["how"],
            "expression": " ".join(expression.split()),
            "ini": entry["ini"],
            "implementations": records,
        })
    return results


def worst(entry):
    """The weakest reporting among an entry's implementations - what a caller can rely on."""
    order = {name: i for i, name in enumerate(CLASSIFICATIONS)}
    return max((impl["classification"] for impl in entry["implementations"]),
               key=lambda c: order.get(c, len(order)))


def summarise(entries):
    counts = {name: 0 for name in CLASSIFICATIONS}
    for entry in entries:
        counts[worst(entry)] += 1
    return counts


def markdown(entries):
    lines = [
        "| # | Entry point | Class whose `init()` runs | Returns | Failure reporting | Evidence |",
        "|---|---|---|---|---|---|",
    ]
    for i, entry in enumerate(entries, 1):
        impls = entry["implementations"]
        classes = ", ".join("`%s`" % impl["class"] if impl["class"] else "?" for impl in impls)
        returns = ", ".join(sorted({impl["returns"] or "?" for impl in impls}))
        evidence = "; ".join(sorted({", ".join(impl["evidence"]) or "none" for impl in impls}))
        lines.append("| %d | `%s` | %s | `%s` | %s | %s |"
                     % (i, entry["entry"], classes, returns, worst(entry), evidence))
    return "\n".join(lines)


def check(entries, baseline):
    """Ratchet the scan against the baseline: reporting may improve, never regress.

    Per entry point rather than per count, because a total hides a swap: one subsystem learning to
    throw while another stops is the same number and a worse tree. New entry points have to be able
    to report at all, which is what stops this slice from being undone by the next one.
    """
    order = {name: i for i, name in enumerate(CLASSIFICATIONS)}
    recorded = {entry["entry"]: worst(entry) for entry in baseline["entries"]}
    current = {entry["entry"]: worst(entry) for entry in entries}

    failures, notes = [], []
    for name, classification in current.items():
        if name in recorded:
            if order[classification] > order[recorded[name]]:
                failures.append("%s: failure reporting regressed from %s to %s"
                                % (name, recorded[name], classification))
        elif order[classification] > order["debug-only"]:
            failures.append("%s: new init entry point cannot report a failure (%s). Report it, or "
                            "record the exception in the baseline with a row in "
                            "docs/porting/init-failure-reporting.md."
                            % (name, classification))
        else:
            notes.append("%s: new init entry point, %s" % (name, classification))
    for name in recorded:
        if name not in current:
            notes.append("%s: was in the baseline and is no longer initialised here" % name)

    for note in notes:
        print("note: " + note)
    if failures:
        print("", file=sys.stderr)
        for failure in failures:
            print("FAIL: " + failure, file=sys.stderr)
        print("\nRegenerate with:\n  python3 scripts/init-reporting-scan.py --json %s"
              % BASELINE.relative_to(REPO_ROOT), file=sys.stderr)
        return 1
    print("OK: %d init entry points, none reports failure less loudly than the baseline"
          % len(entries))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--json", help="write the scan to this file")
    parser.add_argument("--markdown", help="write the table to this file")
    parser.add_argument("--doc", nargs="?", const=str(DOC), default=None,
                        help="rewrite the generated table inside %s" % DOC.relative_to(REPO_ROOT))
    parser.add_argument("--check", action="store_true",
                        help="compare against %s and fail on a regression"
                             % BASELINE.relative_to(REPO_ROOT))
    parser.add_argument("--baseline", default=str(BASELINE))
    parser.add_argument("--quiet", action="store_true")
    args = parser.parse_args()

    sources = Sources()
    entries = build(sources)
    counts = summarise(entries)

    payload = {
        "engine_init": ENGINE_INIT,
        "entry_count": len(entries),
        "counts": counts,
        "entries": entries,
    }
    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n")
    if args.markdown:
        pathlib.Path(args.markdown).write_text(markdown(entries) + "\n")
    if args.doc:
        path = pathlib.Path(args.doc)
        text = path.read_text()
        begin, end = "<!-- BEGIN GENERATED TABLE -->", "<!-- END GENERATED TABLE -->"
        if begin not in text or end not in text:
            raise SystemExit("%s has no generated-table markers" % args.doc)
        head, rest = text.split(begin, 1)
        _, tail = rest.split(end, 1)
        path.write_text(head + begin + "\n" + markdown(entries) + "\n" + end + tail)
    if not args.quiet:
        print(markdown(entries))
        print()
        print("%d init entry points in GameEngine::init()" % len(entries))
        for name in CLASSIFICATIONS:
            print("  %-16s %d" % (name, counts[name]))
        print()
    if args.check:
        return check(entries, json.loads(pathlib.Path(args.baseline).read_text()))
    return 0


if __name__ == "__main__":
    sys.exit(main())

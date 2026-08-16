#!/usr/bin/env python3
"""Assert the checked-in measurement baselines are readable at all.

Every other gate in docs/porting/ci-baselines/ compares numbers. None of them notices a baseline
that stopped being a JSON document, because they all load it and crash -- and a crashing gate looks
like a broken script rather than a broken file, which is exactly how
`native-build-shimmed-level1-2-3-4.json` reached `main` with two `compile_failures` objects
concatenated by a hand-resolved merge conflict:

    json.decoder.JSONDecodeError: Expecting ',' delimiter: line 14 column 5

So this runs first and cheaply: every baseline parses, is an object, and carries the keys the gate
that owns it will ask for. It deliberately checks *shape*, never a measurement -- the ratchets in
check-native-build-baseline.py own the numbers, and duplicating them here would mean two files to
update per improvement.

    python3 scripts/ci/check-generated-baselines.py
"""
import json
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
BASELINE_DIR = REPO_ROOT / "docs" / "porting" / "ci-baselines"

# Keys whose absence would make the owning gate fail confusingly rather than clearly. Baselines not
# listed here still have to parse and still have to be an object; a new one needs no edit to be
# covered by that much.
REQUIRED_KEYS = {
    "native-build-shimmed-level1-2-3.json": (
        "objects", "translation_units", "compile_failures", "undefined_total",
        "undefined_by_category", "levels", "with_shims", "archives",
    ),
    "native-build-shimmed-level1-2-3-4.json": (
        "objects", "translation_units", "compile_failures", "undefined_total",
        "undefined_by_category", "levels", "with_shims", "archives",
    ),
    # The same build in the debug configuration. `config` is required as well as the rest, because
    # comparing a debug measurement with a release baseline compares different compiled code, and
    # check-native-build-baseline.py can only refuse that while the field is present.
    "native-build-shimmed-debug-level1-2-3-4.json": (
        "objects", "translation_units", "compile_failures", "undefined_total",
        "undefined_by_category", "levels", "with_shims", "archives", "config",
    ),
    "native-port-probe-native.json": ("targets", "clean", "total", "mode"),
    "native-port-probe-shimmed.json": ("targets", "clean", "total", "mode"),
}


def main():
    baselines = sorted(BASELINE_DIR.glob("*.json"))
    if not baselines:
        print(f"FAIL: no baselines under {BASELINE_DIR.relative_to(REPO_ROOT)}", file=sys.stderr)
        return 2

    failures = []
    for path in baselines:
        name = path.name
        try:
            loaded = json.loads(path.read_text())
        except json.JSONDecodeError as exc:
            failures.append(f"{name}: not valid JSON: {exc}")
            continue
        if not isinstance(loaded, dict):
            failures.append(f"{name}: top level is {type(loaded).__name__}, expected an object")
            continue
        missing = [key for key in REQUIRED_KEYS.get(name, ()) if key not in loaded]
        if missing:
            failures.append(f"{name}: missing key(s) {', '.join(missing)}")
            continue
        print(f"ok: {name} parses, {len(loaded)} top-level key(s)")

    if failures:
        print("", file=sys.stderr)
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        print("\nThese files are generated. Do not hand-merge them: take one side and regenerate\n"
              "with scripts/native-build.py / scripts/native-port-probe.py, which is what\n"
              ".gitattributes' merge=generated driver assumes you will do.", file=sys.stderr)
        return 1

    print(f"\nOK: all {len(baselines)} checked-in baselines are readable")
    return 0


if __name__ == "__main__":
    sys.exit(main())

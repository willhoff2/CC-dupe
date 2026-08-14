#!/usr/bin/env python3
"""Gate the native link's "Win32 API" undefined-symbol category against a committed budget.

`scripts/native-build.py` classifies every unresolved symbol in the native 64-bit link. The "Win32
API" bucket is the one the port can always close on its own: every name in it is a function the
engine calls and Windows provides, so it needs a portable definition under the same spelling (see
docs/porting/win32-file-api-seam.md), not a shim header.

`check-native-build-baseline.py` already ratchets the *total*, which lets a new Win32 dependency
hide behind an unrelated drop somewhere else. This check pins the category by name: the budget
lists exactly which Win32 symbols are allowed to be unresolved, so a new `#include <windows.h>`
call site fails CI with the symbol that caused it.

Refresh the budget in the PR that changes it:

    python3 scripts/ci/check-win32-undefined.py --results <results.json> --update

The budget lives in docs/porting/ci-baselines/ so that moving it is a reviewable diff.
"""
import argparse
import json
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
BUDGET_PATH = REPO_ROOT / "docs" / "porting" / "ci-baselines" / "win32-undefined-budget.json"
CATEGORY = "Win32 API"


def load(path):
    return json.loads(pathlib.Path(path).read_text())


def symbols_of(results):
    return sorted(results.get("undefined_symbols", {}).get(CATEGORY, []))


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", required=True, help="JSON written by native-build.py --json")
    ap.add_argument("--budget", help=f"budget file (default: {BUDGET_PATH.name})")
    ap.add_argument("--update", action="store_true", help="rewrite the budget from the results")
    args = ap.parse_args()

    results = load(args.results)
    if not results.get("with_shims"):
        print("FAIL: the budget is measured with the shimmed headers, because the unshimmed build "
              "does not get far enough to link", file=sys.stderr)
        return 2

    budget_path = pathlib.Path(args.budget) if args.budget else BUDGET_PATH
    found = symbols_of(results)

    if args.update:
        budget_path.parent.mkdir(parents=True, exist_ok=True)
        budget_path.write_text(json.dumps({"category": CATEGORY,
                                           "allowed": found,
                                           "levels": results["levels"]},
                                          indent=2, sort_keys=True) + "\n")
        print(f"wrote budget {budget_path.relative_to(REPO_ROOT)}: {len(found)} allowed "
              f"unresolved {CATEGORY} symbols")
        return 0

    if not budget_path.is_file():
        print(f"FAIL: no budget at {budget_path}; create one with --update", file=sys.stderr)
        return 2
    allowed = sorted(load(budget_path)["allowed"])

    print(f"{CATEGORY}: {len(found)} unresolved (budget {len(allowed)})")
    for name in found:
        print(f"  {name}{'' if name in allowed else '  <-- NEW'}")

    new = [name for name in found if name not in allowed]
    gone = [name for name in allowed if name not in found]

    if gone:
        print()
        print("Resolved since the budget was written (refresh it in this PR):")
        for name in gone:
            print(f"  - {name}")

    if new:
        print()
        print(f"FAIL: {len(new)} Win32 symbol(s) are newly unresolved in the native link:",
              file=sys.stderr)
        for name in new:
            print(f"  + {name}", file=sys.stderr)
        print("\nDefine them under their Win32 spelling in "
              "Core/Libraries/Source/WWVegas/WWLib/platform/, or, if the call site is genuinely "
              "Windows-only, guard it. Only widen the budget deliberately:", file=sys.stderr)
        print(f"  python3 scripts/ci/check-win32-undefined.py --results {args.results} --update",
              file=sys.stderr)
        return 1

    print()
    print("OK: no new unresolved Win32 symbols")
    return 0


if __name__ == "__main__":
    sys.exit(main())

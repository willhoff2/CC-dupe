#!/usr/bin/env python3
"""Gate the single-player menu path against the online path's absence.

Zero Hour's main menu is single-player UI that *reaches* online code: it includes the download
manager and the GameSpy headers, and calls `TheDownloadManager->update()`. Off Windows the GameSpy
SDK is cut scope and is not linked, so the menu's translation units must compile and link with the
online path absent -- see docs/porting/online-absent-menu-seam.md.

Two things have to hold, and the total-unresolved ratchet in `check-native-build-baseline.py` sees
neither of them:

1. The menu path's translation units compile. They are listed by name, because "objects went up" is
   satisfied by any other file and would let the menu regress silently.
2. Nothing new hides in the "GameSpy SDK (cut scope, not linked)" bucket. That category is expected
   to be non-empty -- those are the SDK entry points the online code calls and nothing defines here
   -- so it is pinned by symbol name: a new online dependency anywhere in the engine fails CI with
   the symbol that caused it, instead of being absorbed into a category that is allowed to be large.

Refresh the budget in the PR that changes it:

    python3 scripts/ci/check-online-absent-seam.py --results <results.json> --update

The budget lives in docs/porting/ci-baselines/ so that moving it is a reviewable diff.
"""
import argparse
import json
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
BUDGET_PATH = REPO_ROOT / "docs" / "porting" / "ci-baselines" / "online-absent-seam.json"
CATEGORY = "GameSpy SDK (cut scope, not linked)"

# The translation units the seam exists to keep compiling: the main menu itself, the download menu
# it opens, the download manager they both drive, and the two GameSpy threads that are only Winsock
# spellings away from portable. Not listed: MainMenuUtils.cpp, PingThread.cpp and
# StagingRoomGameInfo.cpp, which need Win32 surfaces other slices own (see the seam doc).
MUST_COMPILE = [
    "Core/GameEngine/Source/GameNetwork/DownloadManager.cpp",
    "Core/GameEngine/Source/GameNetwork/GameSpy/Thread/GameResultsThread.cpp",
    "Core/GameEngine/Source/GameNetwork/GameSpy/Thread/PeerThread.cpp",
    "GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/DownloadMenu.cpp",
    "GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/MainMenu.cpp",
]


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
                                           "must_compile": MUST_COMPILE,
                                           "levels": results["levels"]},
                                          indent=2, sort_keys=True) + "\n")
        print(f"wrote budget {budget_path.relative_to(REPO_ROOT)}: {len(found)} allowed "
              f"unresolved SDK symbols, {len(MUST_COMPILE)} translation units required to compile")
        return 0

    if not budget_path.is_file():
        print(f"FAIL: no budget at {budget_path}; create one with --update", file=sys.stderr)
        return 2
    allowed = sorted(load(budget_path)["allowed"])

    failures = []

    failed_to_compile = results["compile_failures"]
    print("menu path translation units:")
    for path in MUST_COMPILE:
        broken = path in failed_to_compile
        note = f"  <-- FAILS: {failed_to_compile[path]}" if broken else ""
        print(f"  {'FAIL' if broken else 'ok  '} {path}{note}")
        if broken:
            failures.append(f"{path} no longer compiles")

    print()
    print(f"{CATEGORY}: {len(found)} unresolved (budget {len(allowed)})")
    new = [name for name in found if name not in allowed]
    gone = [name for name in allowed if name not in found]
    for name in new:
        print(f"  {name}  <-- NEW")
    if gone:
        print("Resolved since the budget was written (refresh it in this PR):")
        for name in gone:
            print(f"  - {name}")
    if new:
        failures.append(f"{len(new)} GameSpy SDK symbol(s) newly unresolved")

    if failures:
        print()
        print("FAIL: " + "; ".join(failures), file=sys.stderr)
        print("The menu path must compile and link with the online path absent. If an online "
              "dependency is genuinely new and intended, widen the budget deliberately:",
              file=sys.stderr)
        print(f"  python3 scripts/ci/check-online-absent-seam.py --results {args.results} "
              "--update", file=sys.stderr)
        return 1

    print()
    print("OK: the menu path compiles and no new GameSpy SDK symbol is unresolved")
    return 0


if __name__ == "__main__":
    sys.exit(main())

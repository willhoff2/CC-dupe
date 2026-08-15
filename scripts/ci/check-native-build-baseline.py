#!/usr/bin/env python3
"""Gate the native 64-bit build against a checked-in baseline.

`scripts/native-build.py` measures how many translation units produce object files and how many
symbols are unresolved once they are linked. These are ratchets: objects must not go down, and
unresolved symbols, compile failures and probe-clean-but-uncompilable units must not go up, or a
future upstream merge quietly undoes the port work with nothing in CI to notice.

One baseline per (mode, level set), named after both, so `--level 1 --level 2 --level 3` with
`--with-shims` ratchets against `native-build-shimmed-level1-2-3.json` and adding a level cannot be
mistaken for progress against the smaller build's figures. Facts a bare objects/unresolved pair
would hide are checked too: a changed denominator, a library that produced no archive at all, and a
shrinking archive count -- unresolved symbols also fall when a whole library drops out of the link.

Two further ratchets exist because the unresolved total alone can move for the wrong reason. The
`no-definition-anywhere` pile -- the symbols no library, dependency or source file defines, i.e. the
real remaining port work -- must not grow even when the total falls, which it would if a library
were added to the link in the same change. And when the results come from `--strict-link`, the
number of symbols the linker itself refuses is ratcheted too, since that is the only figure here
that answers "is there an executable"; the tolerant link produces a file and exits 0 regardless.

Improvements are reported, not failed, and the baseline should be refreshed in the same PR that
earns them:

    python3 scripts/ci/check-native-build-baseline.py --results <results.json> --update

The baselines live in docs/porting/ci-baselines/ so that moving them is a reviewable diff.
"""
import argparse
import json
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
BASELINE_DIR = REPO_ROOT / "docs" / "porting" / "ci-baselines"


def load(path):
    return json.loads(pathlib.Path(path).read_text())


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", required=True, help="JSON written by native-build.py --json")
    ap.add_argument("--baseline", help="baseline file (default: chosen from the build mode)")
    ap.add_argument("--update", action="store_true", help="rewrite the baseline from the results")
    args = ap.parse_args()

    results = load(args.results)
    mode = "shimmed" if results["with_shims"] else "native"
    levels = "-".join(str(x) for x in results["levels"])
    baseline_path = pathlib.Path(args.baseline) if args.baseline else \
        BASELINE_DIR / f"native-build-{mode}-level{levels}.json"

    if args.update:
        baseline_path.parent.mkdir(parents=True, exist_ok=True)
        baseline_path.write_text(json.dumps(results, indent=2, sort_keys=True) + "\n")
        print(f"wrote baseline {baseline_path.relative_to(REPO_ROOT)}: "
              f"{results['objects']}/{results['translation_units']} objects, "
              f"{results['undefined_total']} unresolved symbols ({mode}, levels {levels})")
        return 0

    if not baseline_path.is_file():
        print(f"FAIL: no baseline at {baseline_path}; create one with --update", file=sys.stderr)
        return 2
    baseline = load(baseline_path)

    if baseline.get("clang_major") != results.get("clang_major"):
        print(f"FAIL: baseline was measured with clang {baseline.get('clang_major')}, these "
              f"results with clang {results.get('clang_major')}; codegen and libstdc++ differ "
              "between majors, so the counts are not comparable", file=sys.stderr)
        return 2

    failures = []
    improvements = []

    print(f"mode: {mode}   levels: {levels}   toolchain: {results['compiler']}")
    print(f"{'library':45s} {'objects':>14s} {'baseline':>10s}")
    for name, base in baseline["compiled"].items():
        got = results["compiled"].get(name)
        if got is None:
            failures.append(f"library '{name}' disappeared from the build")
            continue
        mark = ""
        if got["objects"] < base["objects"]:
            mark = "  <-- REGRESSION"
            failures.append(f"{name}: {base['objects']}/{base['total']} objects in the baseline, "
                            f"{got['objects']}/{got['total']} now")
        elif got["objects"] > base["objects"]:
            mark = "  <-- improved"
            improvements.append(f"{name}: {base['objects']} -> {got['objects']} objects")
        print(f"{name:45s} {got['objects']:>6d}/{got['total']:<7d} "
              f"{base['objects']:>4d}/{base['total']:<5d}{mark}")

    for name in results["compiled"]:
        if name not in baseline["compiled"]:
            improvements.append(f"{name}: new library, not in the baseline")

    print()
    print(f"TOTAL {results['objects']}/{results['translation_units']} objects "
          f"(baseline {baseline['objects']}/{baseline['translation_units']})")
    print(f"unresolved symbols {results['undefined_total']} "
          f"(baseline {baseline['undefined_total']})")

    # A changed denominator is not progress, and it makes the objects ratchet alone unsafe: 40 more
    # objects out of 40 more translation units is standing still. Say so, and ratchet the failure
    # count as well, which a denominator change cannot flatter.
    if results["translation_units"] != baseline["translation_units"]:
        print(f"denominator change: {baseline['translation_units']} -> "
              f"{results['translation_units']} translation units (files added or removed; the "
              "objects figure must be read as clean/total, never as a percentage)")

    base_failures = len(baseline["compile_failures"])
    got_failures = len(results["compile_failures"])
    print(f"compile failures {got_failures} (baseline {base_failures})")
    if got_failures > base_failures:
        failures.append(f"compile failures: {base_failures} in the baseline, {got_failures} now")
    elif got_failures < base_failures:
        improvements.append(f"compile failures: {base_failures} -> {got_failures}")

    # An archive that stops existing takes its symbols out of the link, so unresolved symbols can
    # *fall* because a whole library vanished. Both facts are checked so neither can hide.
    if results["archives"] < baseline["archives"]:
        failures.append(f"archives: {baseline['archives']} in the baseline, "
                        f"{results['archives']} now")
    lost = set(results.get("libraries_without_archive", [])) - \
        set(baseline.get("libraries_without_archive", []))
    if lost:
        failures.append("every translation unit failed, so no archive was produced for: "
                        + ", ".join(sorted(lost)))

    if results["objects"] < baseline["objects"]:
        failures.append(f"overall: {baseline['objects']} objects in the baseline, "
                        f"{results['objects']} now")
    elif results["objects"] > baseline["objects"]:
        improvements.append(f"overall: {baseline['objects']} -> {results['objects']} objects")

    if results["undefined_total"] > baseline["undefined_total"]:
        failures.append(f"unresolved symbols: {baseline['undefined_total']} in the baseline, "
                        f"{results['undefined_total']} now")
    elif results["undefined_total"] < baseline["undefined_total"]:
        improvements.append(f"unresolved symbols: {baseline['undefined_total']} -> "
                            f"{results['undefined_total']}")

    # A translation unit the probe calls clean but that will not compile is the measurement error
    # this build exists to quantify; it must not grow silently either.
    base_divergence = len(baseline["divergence"]["probe_clean_compile_failed"])
    got_divergence = len(results["divergence"]["probe_clean_compile_failed"])
    print(f"probe-clean but uncompilable {got_divergence} (baseline {base_divergence})")
    if got_divergence > base_divergence:
        failures.append(f"probe-clean but uncompilable: {base_divergence} in the baseline, "
                        f"{got_divergence} now")

    # Per-category before/after. Not a ratchet: the categories are re-derived from the sources and
    # SDK headers on every run, so a renamed or better-attributed category would fail a gate for
    # being more honest. The total above is the ratchet; this is the explanation of it.
    base_categories = baseline["undefined_by_category"]
    got_categories = results["undefined_by_category"]
    print()
    print(f"{'undefined symbols by cause':70s} {'now':>5s} {'base':>5s}")
    for category in sorted(set(base_categories) | set(got_categories)):
        before = base_categories.get(category)
        now = got_categories.get(category)
        print(f"{category[:70]:70s} {'-' if now is None else now:>5} "
              f"{'-' if before is None else before:>5}")

    # The piles say what would resolve each symbol, and only one of them is remaining port work, so
    # that one is a ratchet of its own: the total can fall while port work grows, if a library is
    # added to the link in the same change.
    base_piles = baseline.get("undefined_by_pile") or {}
    got_piles = results.get("undefined_by_pile") or {}
    if got_piles:
        print()
        print(f"{'unresolved symbols by what would resolve them':45s} {'now':>5s} {'base':>5s}")
        for pile in sorted(set(base_piles) | set(got_piles)):
            print(f"{pile:45s} {got_piles.get(pile, 0):>5} "
                  f"{'-' if pile not in base_piles else base_piles[pile]:>5}")
    port_pile = "no-definition-anywhere"
    if port_pile in base_piles and port_pile in got_piles:
        if got_piles[port_pile] > base_piles[port_pile]:
            failures.append(f"symbols nothing defines: {base_piles[port_pile]} in the baseline, "
                            f"{got_piles[port_pile]} now")
        elif got_piles[port_pile] < base_piles[port_pile]:
            improvements.append(f"symbols nothing defines: {base_piles[port_pile]} -> "
                                f"{got_piles[port_pile]}")

    # The strict link is the only figure here that answers "is there an executable", so it is
    # ratcheted whenever both runs attempted it, and a disagreement with the `nm` scan is a failure
    # in its own right: it would mean the categorised list is not the list the linker fails on.
    base_strict = baseline.get("strict_link") or {}
    got_strict = results.get("strict_link") or {}
    if got_strict.get("attempted"):
        print(f"strict link: {'clean' if got_strict['clean'] else 'failed'}, "
              f"{got_strict['unresolved_total']} unresolved "
              f"(baseline {base_strict.get('unresolved_total', '-')}), executable produced: "
              f"{'yes' if got_strict['binary_produced'] else 'no'}")
        if not got_strict["agrees_with_nm"]:
            failures.append(
                f"the strict link's unresolved list and the nm scan disagree: "
                f"{len(got_strict['only_in_linker_report'])} symbol(s) only the linker reports, "
                f"{len(got_strict['only_in_nm_scan'])} only the scan does")
        if base_strict.get("attempted"):
            if got_strict["unresolved_total"] > base_strict["unresolved_total"]:
                failures.append(
                    f"strict link: {base_strict['unresolved_total']} unresolved symbols in the "
                    f"baseline, {got_strict['unresolved_total']} now")
            elif got_strict["unresolved_total"] < base_strict["unresolved_total"]:
                improvements.append(f"strict link: {base_strict['unresolved_total']} -> "
                                    f"{got_strict['unresolved_total']} unresolved symbols")
    elif base_strict.get("attempted"):
        failures.append("the baseline was measured with --strict-link and these results were not, "
                        "so the executable figure it ratchets went unmeasured")

    if improvements:
        print()
        print("Improvements (not a failure -- refresh the baseline in this PR):")
        for line in improvements:
            print(f"  + {line}")

    if failures:
        print()
        print(f"FAIL: the native build regressed against {baseline_path.relative_to(REPO_ROOT)}",
              file=sys.stderr)
        for line in failures:
            print(f"  - {line}", file=sys.stderr)
        print("\nIf the change is intentional, update the baseline in this PR:", file=sys.stderr)
        print("  python3 scripts/ci/check-native-build-baseline.py "
              f"--results {args.results} --update", file=sys.stderr)
        return 1

    print("OK: no regression against the baseline")
    return 0


if __name__ == "__main__":
    sys.exit(main())

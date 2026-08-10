#!/usr/bin/env python3
"""Gate the native probe against a checked-in baseline.

`scripts/native-port-probe.py` prints numbers; nobody reads printed numbers. This turns them
into a pass/fail signal: if the clean translation-unit count drops -- overall or for any single
probe target -- the job fails and names the target that regressed.

Going *up* is not a failure, but it is reported and the baseline should be refreshed in the same
PR that earns it, with:

    python3 scripts/ci/check-probe-baseline.py --results <results.json> --update

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
    ap.add_argument("--results", required=True, help="JSON written by native-port-probe.py --json")
    ap.add_argument("--baseline", help="baseline file (default: chosen from the probe mode)")
    ap.add_argument("--update", action="store_true", help="rewrite the baseline from the results")
    args = ap.parse_args()

    results = load(args.results)
    mode = results["mode"]
    baseline_path = pathlib.Path(args.baseline) if args.baseline else \
        BASELINE_DIR / f"native-port-probe-{mode}.json"

    if args.update:
        baseline_path.parent.mkdir(parents=True, exist_ok=True)
        baseline_path.write_text(json.dumps(results, indent=2) + "\n")
        print(f"wrote baseline {baseline_path.relative_to(REPO_ROOT)}: "
              f"{results['clean']}/{results['total']} clean ({mode})")
        return 0

    if not baseline_path.is_file():
        print(f"FAIL: no baseline at {baseline_path}; create one with --update", file=sys.stderr)
        return 2
    baseline = load(baseline_path)

    if baseline["mode"] != mode:
        print(f"FAIL: baseline is for mode '{baseline['mode']}', results are '{mode}'",
              file=sys.stderr)
        return 2

    # The probe's answer depends on whether the fetched dx8/gamespy/miles/lzhl headers were
    # present (Core/Libraries/Source/Compression needs lzhl.h -- this is exactly the 114-vs-115
    # discrepancy in docs/porting/review-and-decisions.md 1.2). Comparing against a baseline
    # captured with a different set of dependencies would compare two different measurements.
    if baseline.get("deps_present") != results.get("deps_present"):
        print("FAIL: dependency set differs from the baseline, so the numbers are not comparable",
              file=sys.stderr)
        print(f"  baseline: {baseline.get('deps_present')}", file=sys.stderr)
        print(f"  results:  {results.get('deps_present')}", file=sys.stderr)
        return 2

    if baseline.get("clang_major") != results.get("clang_major"):
        print(f"FAIL: baseline was measured with clang {baseline.get('clang_major')}, "
              f"these results with clang {results.get('clang_major')}; the counts are "
              "compiler-version dependent and not comparable", file=sys.stderr)
        return 2

    failures = []
    improvements = []

    print(f"probe mode: {mode}   deps: {', '.join(results['deps_present']) or 'none'}")
    print(f"{'target':45s} {'clean':>12s} {'baseline':>10s}")
    for name, base in baseline["targets"].items():
        got = results["targets"].get(name)
        if got is None:
            failures.append(f"target '{name}' disappeared from the probe")
            continue
        mark = ""
        if got["clean"] < base["clean"]:
            mark = "  <-- REGRESSION"
            failures.append(
                f"{name}: {base['clean']}/{base['total']} clean in the baseline, "
                f"{got['clean']}/{got['total']} now")
        elif got["clean"] > base["clean"]:
            mark = "  <-- improved"
            improvements.append(f"{name}: {base['clean']} -> {got['clean']}")
        print(f"{name:45s} {got['clean']:>5d}/{got['total']:<6d} {base['clean']:>4d}/{base['total']:<5d}{mark}")

    for name in results["targets"]:
        if name not in baseline["targets"]:
            improvements.append(f"{name}: new probe target, not in the baseline")

    print()
    print(f"TOTAL {results['clean']}/{results['total']} clean "
          f"(baseline {baseline['clean']}/{baseline['total']})")

    if results["clean"] < baseline["clean"]:
        failures.append(f"overall: {baseline['clean']} clean in the baseline, "
                        f"{results['clean']} now")

    if improvements:
        print()
        print("Improvements (not a failure -- refresh the baseline in this PR):")
        for line in improvements:
            print(f"  + {line}")

    if failures:
        print()
        print("FAIL: the native probe regressed against "
              f"{baseline_path.relative_to(REPO_ROOT)}", file=sys.stderr)
        for line in failures:
            print(f"  - {line}", file=sys.stderr)
        print("\nIf the drop is intentional, update the baseline in this PR:", file=sys.stderr)
        print(f"  python3 scripts/ci/check-probe-baseline.py --results {args.results} --update",
              file=sys.stderr)
        return 1

    print("OK: no regression against the baseline")
    return 0


if __name__ == "__main__":
    sys.exit(main())

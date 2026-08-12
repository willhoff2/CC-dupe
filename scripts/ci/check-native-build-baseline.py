#!/usr/bin/env python3
"""Gate the native 64-bit build against a checked-in baseline.

`scripts/native-build.py` measures how many translation units produce object files and how many
symbols are unresolved once they are linked. Both are ratchets: objects must not go down, and
unresolved symbols must not go up, or a future upstream merge quietly undoes the port work with
nothing in CI to notice.

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

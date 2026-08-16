#!/usr/bin/env python3
"""Gate the native 64-bit build against a checked-in baseline.

`scripts/native-build.py` measures how many translation units produce object files and how many
symbols are unresolved once they are linked. These are ratchets: objects must not go down, and
unresolved symbols, compile failures and probe-clean-but-uncompilable units must not go up, or a
future upstream merge quietly undoes the port work with nothing in CI to notice.

One baseline per (mode, configuration, level set), named after all three, so `--level 1 --level 2
--level 3` with `--with-shims` ratchets against `native-build-shimmed-level1-2-3.json` and adding a
level cannot be mistaken for progress against the smaller build's figures. `--config debug` gets
`native-build-shimmed-debug-level1-2-3-4.json`: it compiles the assertions, debug logging and
profiling hooks the release build compiles out, so it has a larger surface and its figures are not
comparable with the release build's in either direction. Facts a bare objects/unresolved pair
would hide are checked too: a changed denominator, a library that produced no archive at all, and a
shrinking archive count -- unresolved symbols also fall when a whole library drops out of the link.

Two further ratchets exist because the unresolved total alone can move for the wrong reason. The
`no-definition-anywhere` pile -- the symbols no library, dependency or source file defines, i.e. the
real remaining port work -- must not grow even when the total falls, which it would if a library
were added to the link in the same change. And when the results come from `--strict-link`, the
number of symbols the linker itself refuses is ratcheted too, since that is the only figure here
that answers "is there an executable"; the tolerant link produces a file and exits 0 regardless.

Once a baseline records a produced executable, the strict link stops being a ratchet and becomes a
BINARY GATE: the link must not break. A count that grew from 0 to 4 and a count that grew from 73
to 77 are not the same kind of event -- the first means there is no longer an executable at all --
so from the first binary onwards the assertion is that the strict link is still clean, still
produces a file, and that the file is still a 64-bit executable of this host's machine, rather than
that some number did not increase. A regression is a broken build, not a bigger number.

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


def check_binary_gate(base_strict, got_strict):
    """Once an executable has been linked, assert it still is -- the inverted ratchet.

    Returns the failure lines. Nothing here compares counts: a link that stops completing is the
    failure, whether it is short by one symbol or by seventy. The file's own header is checked too,
    because a 32-bit or foreign-machine file at that path would satisfy `binary_produced` while
    being useless, and the whole point of the figure is that it means a loadable 64-bit binary.
    """
    if not base_strict.get("binary_produced"):
        return []
    failures = []
    if not got_strict.get("binary_produced"):
        failures.append("the strict link produced an executable in the baseline and produces none "
                        f"now: {got_strict.get('unresolved_total', '?')} unresolved symbol(s). "
                        "This is a broken build, not a bigger number")
    if not got_strict.get("clean"):
        failures.append("the strict link exited non-zero; the baseline's link was clean")
    if got_strict.get("unresolved_total"):
        failures.append(f"the strict link reports {got_strict['unresolved_total']} unresolved "
                        "symbol(s); the baseline's link had none")
    binary = got_strict.get("binary")
    base_binary = base_strict.get("binary") or {}
    if binary and binary.get("format") == "Mach-O universal":
        failures.append(
            f"the executable is a universal Mach-O of {binary.get('architectures', '?')} "
            "architectures; this port measures a thin binary of the host architecture, and a fat "
            "file says nothing about which slice was actually built or run")
    # `lipo -archs` is the platform's own answer, decoded by a different tool from the header
    # fields above. A build that silently came out for another architecture -- x86-64 on an Apple
    # Silicon machine, say -- would satisfy every other check here while invalidating every
    # conclusion drawn from the run.
    if binary and binary.get("lipo_archs") and binary.get("machine") \
            and set(binary["lipo_archs"]) != {binary["machine"]}:
        failures.append(
            f"`lipo -archs` reports {' '.join(binary['lipo_archs'])} for a file whose header says "
            f"{binary['machine']}; the two tools must agree on one architecture")
    if got_strict.get("binary_produced") and not binary:
        failures.append("the strict link reports an executable but describes no file, so this run "
                        "cannot say what was produced; re-run with a harness that records it")
    elif binary:
        print(f"executable: {binary['path']} {binary['bytes'] / (1024 * 1024):.1f} MiB "
              f"{binary['format']} {binary['word_size']}-bit {binary['machine']} "
              f"(baseline {base_binary.get('word_size', '-')}-bit "
              f"{base_binary.get('machine', '-')})")
        if binary.get("word_size") != 64:
            failures.append(f"the executable is {binary.get('word_size')}-bit; this port targets "
                            "64-bit and the baseline's file was 64-bit")
        if base_binary.get("format") and binary.get("format") != base_binary["format"]:
            failures.append(f"the executable is a {binary['format']} file, the baseline's was "
                            f"{base_binary['format']}; the baselines are per-platform, so this is "
                            "a baseline measured on another OS rather than a comparison")
        if base_binary.get("machine") and binary.get("machine") != base_binary["machine"]:
            failures.append(f"the executable is for {binary['machine']}, the baseline's was for "
                            f"{base_binary['machine']}")
    return failures


def check_entry_point(baseline, results):
    """The link must be anchored by the game's own entry point once that target is in the build.

    A generated stub `main()` is legitimate only below level 3, where the entry-point target is not
    built at all. With it in the build, a stub turns a link of the game into a link of a three-line
    program while `binary_produced` and every other figure keep their names -- #87's Darwin run,
    where the scan looked for the ELF spelling of `main` in Mach-O archives. `native-build.py` now
    refuses to generate one, and this is the assertion that a baseline can never record having had
    one either.
    """
    failures = []
    if results.get("game_entry_target_built") and results.get("link_entry_point_stub"):
        failures.append(
            "the link was anchored by a generated stub main() while "
            "GeneralsMD/Code/Main was in the build: the figures would describe a link of the stub, "
            "not of the game")
    if baseline.get("link_entry_point_archives") and not results.get("link_entry_point_archives"):
        failures.append(
            "the baseline's link was anchored by the game's own entry point ("
            + ", ".join(baseline["link_entry_point_archives"])
            + ") and this run's is not")
    # An arm64 measurement taken by an x86-64 toolchain under Rosetta 2 reports the host as Apple
    # Silicon and produces x86-64 objects. The platform answers this directly, so it is asserted
    # rather than inferred from the file.
    if results.get("host_translated"):
        failures.append(
            "this run was measured under Rosetta 2 (`sysctl.proc_translated` is 1), so the "
            "toolchain and everything it produced are x86-64 regardless of the host's "
            "architecture; re-run in a native shell")
    return failures


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--results", required=True, help="JSON written by native-build.py --json")
    ap.add_argument("--baseline", help="baseline file (default: chosen from the build mode)")
    ap.add_argument("--update", action="store_true", help="rewrite the baseline from the results")
    args = ap.parse_args()

    results = load(args.results)
    mode = "shimmed" if results["with_shims"] else "native"
    # Absent in every baseline measured before --config existed, which is exactly the release
    # configuration, so the default keeps those baselines addressable under their existing names.
    config = results.get("config", "release")
    mode_key = mode if config == "release" else f"{mode}-{config}"
    levels = "-".join(str(x) for x in results["levels"])
    baseline_path = pathlib.Path(args.baseline) if args.baseline else \
        BASELINE_DIR / f"native-build-{mode_key}-level{levels}.json"

    if args.update:
        baseline_path.parent.mkdir(parents=True, exist_ok=True)
        baseline_path.write_text(json.dumps(results, indent=2, sort_keys=True) + "\n")
        print(f"wrote baseline {baseline_path.relative_to(REPO_ROOT)}: "
              f"{results['objects']}/{results['translation_units']} objects, "
              f"{results['undefined_total']} unresolved symbols ({mode_key}, levels {levels})")
        return 0

    if not baseline_path.is_file():
        print(f"FAIL: no baseline at {baseline_path}; create one with --update", file=sys.stderr)
        return 2
    baseline = load(baseline_path)

    # A debug result compared against a release baseline would read as hundreds of new compile
    # failures, and a release result against a debug baseline as an improvement. Neither is a
    # measurement, so the mismatch is refused rather than reported.
    if baseline.get("config", "release") != config:
        print(f"FAIL: {baseline_path.name} was measured in the "
              f"{baseline.get('config', 'release')} configuration, these results in the {config} "
              "configuration; the two compile different code and are not comparable",
              file=sys.stderr)
        return 2

    if baseline.get("clang_major") != results.get("clang_major"):
        print(f"FAIL: baseline was measured with clang {baseline.get('clang_major')}, these "
              f"results with clang {results.get('clang_major')}; codegen and libstdc++ differ "
              "between majors, so the counts are not comparable", file=sys.stderr)
        return 2

    failures = []
    improvements = []

    print(f"mode: {mode}   config: {config}   levels: {levels}   "
          f"toolchain: {results['compiler']}")
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
    # gated whenever both runs attempted it, and a disagreement with the `nm` scan is a failure
    # in its own right: it would mean the categorised list is not the list the linker fails on.
    failures += check_entry_point(baseline, results)

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
        failures += check_binary_gate(base_strict, got_strict)
        if base_strict.get("attempted"):
            # Below the gate, and only while a baseline predates the first binary, the count is
            # still worth ratcheting: it is the measure of distance to a link that completes.
            if got_strict["unresolved_total"] > base_strict["unresolved_total"] \
                    and not base_strict.get("binary_produced"):
                failures.append(
                    f"strict link: {base_strict['unresolved_total']} unresolved symbols in the "
                    f"baseline, {got_strict['unresolved_total']} now")
            elif got_strict["unresolved_total"] < base_strict["unresolved_total"]:
                improvements.append(f"strict link: {base_strict['unresolved_total']} -> "
                                    f"{got_strict['unresolved_total']} unresolved symbols")
            if got_strict.get("binary_produced") and not base_strict.get("binary_produced"):
                improvements.append("strict link: no executable in the baseline, one now -- "
                                    "refresh the baseline and the gate becomes 'must not break'")
    elif base_strict.get("attempted"):
        failures.append("the baseline was measured with --strict-link and these results were not, "
                        "so the executable it gates on went unmeasured")

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

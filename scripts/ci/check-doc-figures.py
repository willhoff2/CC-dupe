#!/usr/bin/env python3
"""Assert the headline figures quoted in docs/porting prose match the committed baselines.

`STATUS.md` cannot rot because it is generated. The rest of `docs/porting/*.md` can, and did: the
plan's Phase 0 "superseded" note quoted 673 / 761 for two weeks after the baseline said 674 / 762,
and `next-slice-scope.md` said the direct D3D8 count was 0 for a month after the allowlist budgeted
4. The nightly sweep caught both -- thirteen nights in a row, in thirteen open PRs nobody read.
This is that catch as a lint-tier CI gate instead: a quoted figure that disagrees with the baseline
fails the push that made it stale, not a sweep a month later.

Only prose that presents itself as *current* is checked. Dated measurements ("Measured 2026-08-17 at
commit 3098ef1: ...") and slice records ("after this slice the scan reports ...") are history and
are supposed to keep their numbers. Each entry names the file, a regex whose groups are the quoted
figures, and the baseline values they must equal.

    python3 scripts/ci/check-doc-figures.py
"""
import json
import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
BASELINES = REPO_ROOT / "docs" / "porting" / "ci-baselines"
ALLOWLIST = REPO_ROOT / "spikes" / "renderer" / "tools" / "d3d8-direct-allowlist.json"


def _json(path):
    return json.loads(path.read_text())


def expected_values():
    native = _json(BASELINES / "native-port-probe-native.json")
    shimmed = _json(BASELINES / "native-port-probe-shimmed.json")
    l123 = _json(BASELINES / "native-build-shimmed-level1-2-3.json")
    l1234 = _json(BASELINES / "native-build-shimmed-level1-2-3-4.json")
    allowlist = _json(ALLOWLIST)
    return {
        "probe_native_clean": native["clean"],
        "probe_native_total": native["total"],
        "probe_shimmed_clean": shimmed["clean"],
        "probe_shimmed_total": shimmed["total"],
        "l123_unresolved": l123["strict_link"]["unresolved_total"],
        "l1234_unresolved": l1234["strict_link"]["unresolved_total"],
        "l1234_objects": l1234["objects"],
        "d3d8_direct": allowlist["total"],
    }


# (relative path, regex, names of the expected values its groups must equal, in group order).
# A regex that does not match at all is a failure too: the sentence it guards was reworded, and the
# entry has to follow it or be removed on purpose.
CHECKS = [
    (
        "docs/porting/native-port-plan.md",
        r"The probe now covers nine targets:\s*>\s*\*\*(\d+) / (\d+)\*\* clean native and "
        r"\*\*(\d+) / (\d+)\*\* shimmed",
        ("probe_native_clean", "probe_native_total", "probe_shimmed_clean", "probe_shimmed_total"),
    ),
    (
        "docs/porting/next-slice-scope.md",
        r"the direct count is \*\*(\d+)\*\*, all allowlisted",
        ("d3d8_direct",),
    ),
    (
        ".agents/skills/native-port-measure/SKILL.md",
        r"the strict link is expected to fail \((\d+) unresolved on main\)",
        ("l123_unresolved",),
    ),
]


def main():
    expected = expected_values()
    failures = []
    for rel, pattern, names in CHECKS:
        text = (REPO_ROOT / rel).read_text()
        match = re.search(pattern, text)
        if not match:
            failures.append(f"{rel}: no sentence matches /{pattern}/ -- reworded? update CHECKS")
            continue
        quoted = tuple(int(g) for g in match.groups())
        wanted = tuple(expected[n] for n in names)
        if quoted != wanted:
            pairs = ", ".join(f"{n}: quoted {q}, baseline {w}"
                              for n, q, w in zip(names, quoted, wanted) if q != w)
            failures.append(f"{rel}: {pairs}")
        else:
            print(f"ok    {rel}: {' / '.join(str(q) for q in quoted)}")

    if failures:
        print("FAIL: prose figures disagree with docs/porting/ci-baselines/:", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        print("Re-measure and fix the prose (or mark the sentence superseded); never edit a"
              " baseline to match a document.", file=sys.stderr)
        return 1
    print(f"ok    {len(CHECKS)} prose figures agree with the baselines")
    return 0


if __name__ == "__main__":
    sys.exit(main())

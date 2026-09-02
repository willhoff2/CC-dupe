#!/usr/bin/env python3
"""Assert every gate native-port-ci.yml runs is named in a repository skill.

The skills under .agents/skills/ are how a session learns which gates exist and how to run them.
Gates get added to the workflow one slice at a time and the skills lag: when the nightly sweep was
written it swept probe counts, the D3D8 budget, the strict link and the window scan, and for a
month it did not know about draw capacity, HiDPI, staging cost, the spike render, backend coverage,
the debug build's negative controls or the seams' behaviour tests -- the gates most likely to rot.
Ten of thirteen nightly PRs then each rediscovered a subset of that gap and proposed the same skill
edit. This gate makes the gap a lint failure on the push that opens it.

The rule is deliberately weak: the script's basename has to appear somewhere in some SKILL.md. It
does not check arguments, environment or ordering -- the skill prose owns those -- only that the
gate is discoverable at all.

    python3 scripts/ci/check-skill-coverage.py
"""
import pathlib
import re
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
WORKFLOW = REPO_ROOT / ".github" / "workflows" / "native-port-ci.yml"
SKILLS = sorted((REPO_ROOT / ".agents" / "skills").glob("*/SKILL.md"))

# Scripts the workflow runs that are CI plumbing rather than a gate a session would run by hand.
PLUMBING = {
    "scripts/ci/classify-changes.py",       # decides which jobs a pull request pays for
}

INVOCATION = re.compile(r"(?<![\w/.-])((?:scripts|spikes)/[\w./-]+\.(?:py|sh))\b")


def main():
    if not SKILLS:
        print("FAIL: no .agents/skills/*/SKILL.md found", file=sys.stderr)
        return 2
    skill_text = "\n".join(p.read_text() for p in SKILLS)
    invoked = sorted(set(INVOCATION.findall(WORKFLOW.read_text())) - PLUMBING)
    invoked = [s for s in invoked if (REPO_ROOT / s).exists()]

    missing = [s for s in invoked if pathlib.PurePosixPath(s).name not in skill_text]
    for s in invoked:
        print(f"{'MISSING' if s in missing else 'ok     '} {s}")
    if missing:
        print(f"FAIL: {len(missing)} of {len(invoked)} gates in {WORKFLOW.relative_to(REPO_ROOT)} "
              f"are named in no .agents/skills/*/SKILL.md; add each to the skill that owns its "
              f"build, with the arguments CI uses.", file=sys.stderr)
        return 1
    print(f"ok      all {len(invoked)} gates the workflow runs are named in a skill")
    return 0


if __name__ == "__main__":
    sys.exit(main())

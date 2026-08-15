#!/usr/bin/env python3
"""Classify a change by which CI measurements it can move.

`native-port-ci.yml` is twelve jobs, three of them macOS, and about forty-five runner-minutes. Most
changes cannot move most of those numbers -- measured on this repository, roughly 44% of recent
commits touch nothing but `docs/` and `.agents/` -- so the workflow gates each heavy job on one of
the areas below and skips the rest. This is the single place those areas are defined.

    git diff --name-only "$(git merge-base origin/main HEAD)" HEAD \\
        | python3 scripts/ci/classify-changes.py

Prints `area=true|false` per line, suitable for appending to `$GITHUB_OUTPUT`, and a human-readable
table on stderr.

Two properties this file is written for, in preference order:

  * A gate that silently does not run is worse than a runner-minute wasted. Every area therefore
    includes the CI plumbing itself (`scripts/ci/**` and the workflow), the patterns are coarse, and
    anything not recognised as prose counts as code. There is no default-skip: `code` is the
    complement of the prose list, so a new top-level directory is covered the day it appears rather
    than the day someone remembers to add it here.
  * The rules are testable without pushing a branch. `--self-check` asserts the classification of a
    set of paths taken from real commits, so the negation logic -- the part that a glob-matching
    action gets wrong quietly -- is checked here instead of on main.

Note that the workflow only consults this on `pull_request`. On push to `main` and on
`workflow_dispatch` every area is forced true, because `main` is the branch every number in
`docs/porting/` is quoted from and it is held to the whole suite regardless of the diff.
"""
import argparse
import fnmatch
import sys

# Prose. A change confined to these can still break the cheap tier -- docs/porting/STATUS.md is
# generated and gated by scripts/porting-status.py --check -- which is why that tier is ungated in
# the workflow rather than gated on `code`.
PROSE = (
    "docs/*",
    ".agents/*",
    "*.md",
)

# Positive patterns per area, matched with fnmatch against repository-relative paths. `code` is not
# here: it is the complement of PROSE.
AREAS = {
    # The renderer spike, the D3D8 surface it emulates, and the engine's D3D8 consumers.
    "renderer": (
        "spikes/*",
        "*/WW3D2/*",
        "*/W3DDevice/*",
        "cmake/dx8.cmake",
    ),
    # The window / event loop / input seam, on both backends.
    "window": (
        "spikes/*",
        "*/WWLib/platform/*",
        "*/KeyScanCodes.h",
        "scripts/window-input-scan.py",
    ),
    # The OpenAL replacement for Miles and its WWAudio consumers. CMakeLists.txt is in because the
    # non-Windows branch of the root build is what supplies `milesstub`.
    "audio": (
        "*/OpenALAudioDevice/*",
        "*/WWAudio/*",
        "*/GameAudio/*",
        "cmake/openal.cmake",
        "CMakeLists.txt",
        "scripts/audio-surface-scan.py",
    ),
}

# The CI plumbing moves every measurement by definition: it is what runs them.
PLUMBING = (
    ".github/workflows/native-port-ci.yml",
    "scripts/ci/*",
)


def _matches(path, patterns):
    return any(fnmatch.fnmatch(path, pattern) for pattern in patterns)


def is_prose(path):
    return _matches(path, PROSE)


def classify(paths):
    """Map area name -> whether any of `paths` can move that area's measurements."""
    result = {"code": any(not is_prose(path) for path in paths)}
    for area, patterns in AREAS.items():
        result[area] = any(
            _matches(path, patterns) or _matches(path, PLUMBING)
            for path in paths
            if not is_prose(path)
        )
    return result


# Paths lifted from real commits on this repository, with the classification they must get. The
# renderer/window/audio expectations are the interesting half: each of these landed as a slice whose
# gate had to run, and the docs commits are the ones that were paying for all twelve jobs.
NOTHING = {"code": False, "renderer": False, "window": False, "audio": False}

SELF_CHECK = (
    (["docs/porting/STATUS.md"], NOTHING),
    (
        [".agents/skills/port-slice-workflow/SKILL.md", "docs/porting/native-build.md"],
        NOTHING,
    ),
    (
        ["GeneralsMD/Code/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp"],
        {"code": True, "renderer": True, "window": False, "audio": False},
    ),
    (
        ["Core/Libraries/Source/WWVegas/WWLib/platform/platform_window_cocoa.mm"],
        {"code": True, "renderer": False, "window": True, "audio": False},
    ),
    (
        ["Core/Libraries/Source/OpenALAudioDevice/OpenALAudioManager.cpp"],
        {"code": True, "renderer": False, "window": False, "audio": True},
    ),
    (
        ["spikes/renderer/src/backend_vulkan.cpp"],
        {"code": True, "renderer": True, "window": True, "audio": False},
    ),
    # The plumbing case: a change to a gate script runs every area's gate.
    (
        ["scripts/ci/check-probe-baseline.py"],
        {"code": True, "renderer": True, "window": True, "audio": True},
    ),
    # Docs plus one source file is a source change: the prose does not dilute it.
    (
        ["docs/porting/audio-device-seam.md", "cmake/openal.cmake"],
        {"code": True, "renderer": False, "window": False, "audio": True},
    ),
    # A file in no area still builds and still moves the probe and build counts.
    (
        ["GeneralsMD/Code/GameEngine/Source/GameLogic/Object/Update/AIUpdate.cpp"],
        {"code": True, "renderer": False, "window": False, "audio": False},
    ),
    # No files at all (an empty diff) must not claim everything changed.
    ([], NOTHING),
)


def self_check():
    failures = 0
    for paths, expected in SELF_CHECK:
        got = classify(paths)
        if got != expected:
            failures += 1
            print(f"FAIL {paths}", file=sys.stderr)
            for area in sorted(expected):
                if got[area] != expected[area]:
                    print(
                        f"  {area}: expected {expected[area]}, got {got[area]}",
                        file=sys.stderr,
                    )
    total = len(SELF_CHECK)
    print(f"{total - failures} / {total} classification case(s) as expected")
    return 1 if failures else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--self-check",
        action="store_true",
        help="check the rules against paths from real commits instead of reading a diff",
    )
    args = parser.parse_args()

    if args.self_check:
        return self_check()

    paths = [line.strip() for line in sys.stdin if line.strip()]
    result = classify(paths)
    for area in ("code", "renderer", "window", "audio"):
        print(f"{area}={'true' if result[area] else 'false'}")
    print(f"{len(paths)} changed path(s):", file=sys.stderr)
    for path in paths:
        print(f"  {path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())

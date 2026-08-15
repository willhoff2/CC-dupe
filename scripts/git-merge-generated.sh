#!/bin/sh
# Merge driver for generated measurement artefacts (see .gitattributes).
#
# git calls this as: git-merge-generated.sh %O %A %B %P
#   %O  the common ancestor's copy
#   %A  the current branch's copy, and the file the driver must leave the result in
#   %B  the other branch's copy
#   %P  the path in the worktree
#
# It keeps %A untouched -- i.e. resolves to the current branch's copy -- and exits 0, so a rebase
# does not stop on a file no human should be editing. That is only correct because the copy is then
# regenerated: `scripts/native-build.py --json <baseline>` and `scripts/porting-status.py` rewrite
# these, and CI re-measures and ratchets on every push, so a stale copy shows up as a failing gate
# rather than as a wrong number nobody checked.
set -e

ancestor=$1
ours=$2
theirs=$3
path=${4:-$ours}

if cmp -s "$ours" "$theirs"; then
	exit 0
fi

cat >&2 <<EOF
merge=generated: kept this branch's $path
  It is generated, so the two sides were not interleaved. Regenerate it before quoting a number:
    CLANGXX=clang++-14 python3 scripts/native-build.py --level 1 --level 2 --level 3 --with-shims \\
      --report docs/porting/native-build-report.md \\
      --json docs/porting/ci-baselines/native-build-shimmed-level1-2-3.json
    python3 scripts/porting-status.py
EOF

# Silence "unused" for the ancestor: it is part of git's calling convention, not a mistake.
: "$ancestor"
exit 0

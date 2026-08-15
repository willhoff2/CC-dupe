#!/bin/sh
# Point this clone's hooks at the tracked .githooks directory, so the hooks are versioned with the
# code instead of living in each contributor's .git/hooks, and register the merge driver
# .gitattributes names. Idempotent; safe to re-run.
set -e

REPO_ROOT=$(git rev-parse --show-toplevel)
cd "$REPO_ROOT"

EXISTING=$(git config --local --get core.hooksPath || true)
if [ -n "$EXISTING" ] && [ "$EXISTING" != ".githooks" ]; then
	echo "core.hooksPath is already set to '$EXISTING'; leaving it alone." >&2
	echo "Set it yourself with: git config core.hooksPath .githooks" >&2
	exit 1
fi

chmod +x .githooks/*
git config core.hooksPath .githooks

echo "Installed the hooks in .githooks:"
for hook in .githooks/*; do
	echo "  $(basename "$hook")"
done

# .gitattributes marks the generated measurement artefacts merge=generated, but a merge driver
# cannot be defined in-tree: the attribute names it and local config has to supply the command.
# Without this the attribute is inert and those files conflict textually, as they did through
# waves 3-5.
chmod +x scripts/git-merge-generated.sh
git config merge.generated.name "keep this branch's copy of a generated file, then regenerate it"
git config merge.generated.driver "scripts/git-merge-generated.sh %O %A %B %P"
echo "Registered the merge=generated driver for docs/porting/ci-baselines/*.json and friends."

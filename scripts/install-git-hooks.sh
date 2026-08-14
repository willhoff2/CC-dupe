#!/bin/sh
# Point this clone's hooks at the tracked .githooks directory, so the hooks are versioned with the
# code instead of living in each contributor's .git/hooks. Idempotent; safe to re-run.
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

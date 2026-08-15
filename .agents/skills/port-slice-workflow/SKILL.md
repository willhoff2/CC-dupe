---
name: port-slice-workflow
description: How a native-port slice is scoped, branched, verified and landed in this repo, and how concurrent slices stay compatible. Use at the start of any port work, and when coordinating several sessions.
---

# Working a port slice

## Scope

Target: single-player **Zero Hour** (skirmish + campaign) natively on Apple Silicon, 64-bit, no Wine.
Out of scope, permanently: the Win32/MFC tools (WorldBuilder, W3DView, GUIEdit, ImagePacker,
ParticleEditor — 140 of the repo's `HWND` files), GameSpy online play, retail save/replay binary
compatibility, and `Generals/Code` for port purposes. If a build error leads you into any of those,
stop and re-read `docs/porting/native-port-plan.md`.

Zero Hour first: land the change in `GeneralsMD`, replicate to `Generals` afterwards as identically as
possible.

## The seam pattern

Do not rewrite call sites. Put the portable implementation *under the existing Win32/vendor spelling*
so consumers compile unchanged — that is how `WWLib/platform/` (threads, mutexes, clock, filesystem,
settings, process), the Winsock-spelled BSD sockets, `milesstub` for `AIL_*` and `RenderBackendClass`
under `DX8Wrapper` all work. Extend `scripts/native-port-shims/` rather than `#ifdef`-ing consumers.

## Branch and PR rules

- Branch from current `main`. **Never** base a slice on another slice's branch: PRs #2–#4 were opened
  against a feature branch and merged into it, so four slices' worth of work was absent from `main`
  while it was being quoted as done (`docs/porting/review-and-decisions.md` section 0).
- One seam per PR. Keep refactors separate from logic changes, per `CONTRIBUTING.md`.
- After any rebase onto `main`, re-measure before quoting a number. Upstream changes touch the same
  areas and pre-rebase figures do not transfer.
- Announce LLM-generated code as generated and state the extent of human polishing, per
  `CONTRIBUTING.md`.
- Run `./scripts/install-git-hooks.sh` once per clone. Its `commit-msg` hook rejects the subjects the
  **Validate Title and Commits** job rejects — conventional type, optional `(scope)`, colon, one
  space, capitalised description — so a bad subject fails at commit time rather than after the push.
  It also registers the `merge=generated` driver described below. It cannot police commits made in the
  GitHub web UI (`Update <file>`, `Apply suggestion ...`): squash-merge those so the PR title becomes
  the subject.

## Rebasing onto a moved main

Every slice moves the measured numbers, so `docs/porting/ci-baselines/*.json`,
`docs/porting/native-build-report.md` and `docs/porting/STATUS.md` conflict in every rebase of every
concurrent slice. **Never hand-merge them.** A hand resolution once concatenated both sides'
`compile_failures` objects and put a `native-build-shimmed-level1-2-3-4.json` on `main` that
`json.load` refused outright, and no reviewer spotted it because nobody reads these files by eye.

- `.gitattributes` marks those paths `merge=generated`, so the driver keeps the copy already on the
  branch you are rebasing onto instead of interleaving two generated documents. **It discards your
  measurement on purpose**; regenerate afterwards. A commit that touched only generated files becomes
  empty and git drops it, which is correct.
- Regenerate rather than resolve, then re-run the gates:

  ```sh
  CLANGXX=clang++-14 python3 scripts/native-build.py --level 1 --level 2 --level 3 --with-shims \
    --report docs/porting/native-build-report.md \
    --json docs/porting/ci-baselines/native-build-shimmed-level1-2-3.json
  python3 scripts/porting-status.py
  python3 scripts/ci/check-generated-baselines.py   # parses; catches a corrupt hand-merge
  ```

- Update the PR body's figures too: a description quoting pre-rebase numbers is the most common way a
  wrong number reaches a reader.
- Keep the source change and the regeneration as separate commits, so a reviewer can see that the
  slice changed source and that the numbers followed from it.

## Exit criteria

A slice is not defined until its exit criterion is a number or a gate — "147/147 translation units
clean", "the direct D3D8 budget stays at 0", "every declared `AIL_*` symbol is defined". Turn the
improvement into a committed check under `scripts/ci/` so it cannot regress, and write
`docs/porting/<slice>.md` recording what was measured, what is stubbed, and what is still open.

## Verification ladder

Cheapest first; run the rungs your change can affect. Details in the `native-port-measure`,
`renderer-spike-verify` and `windows-build-and-replays` skills.

```sh
# Both of these are clean on main and both are gated by the `lint` job in native-port-ci.yml, so
# a failure here is yours. The 909 violations in the upstream scripts/cpp/ tools are excluded in
# .flake8 -- reformatting them would conflict with every future upstream merge.
python3 -m flake8 scripts/                        # settings come from .flake8; do not pass overrides
python3 scripts/ci/classify-changes.py --self-check   # which CI jobs a diff is allowed to skip
actionlint .github/workflows/*.yml                # the glob, not the directory, which is rejected
./scripts/ci/fetch-probe-deps.sh && CLANGXX=clang++-14 python3 scripts/native-port-probe.py --json probe-native.json
python3 scripts/ci/check-probe-baseline.py --results probe-native.json
python3 scripts/porting-status.py --check
```

If `actionlint` is missing, install it rather than skipping that rung — the blueprint's install step
has failed silently before:

```sh
curl -fsSL -o /tmp/dl.bash \
  https://raw.githubusercontent.com/rhysd/actionlint/main/scripts/download-actionlint.bash
(cd /tmp && bash /tmp/dl.bash 1.7.7) && sudo install -m 0755 /tmp/actionlint /usr/local/bin/actionlint
```

## Concurrent slices

`docs/porting/concurrent-slices.md` is the ownership register: before starting, claim your files there
and check nobody else holds them. Files that only one in-flight slice may touch at a time:
`docs/porting/ci-baselines/*.json`, `scripts/native-port-probe.py`, `dx8wrapper.*` /
`d3d8renderbackend.cpp`, and the root `CMakeLists.txt`. Merge one port PR at a time and re-measure
after each merge.

When several slices are in flight and one lands, the others do not just conflict — their *numbers* are
stale, including the ones already written into their PR descriptions. Rebase them one at a time in the
order they will merge, re-measure each, and correct its description; a slice's own gate can also start
failing on a symbol that only becomes visible at a deeper build level, which is a real finding rather
than a rebase artefact.

Raise, do not guess: product decisions (where macOS settings live, which behaviour is authoritative
when native and Windows disagree), scope changes into cut areas, a measured number that contradicts
`docs/porting/`, or a third consecutive failure on the same problem.

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

## Exit criteria

A slice is not defined until its exit criterion is a number or a gate — "147/147 translation units
clean", "the direct D3D8 budget stays at 0", "every declared `AIL_*` symbol is defined". Turn the
improvement into a committed check under `scripts/ci/` so it cannot regress, and write
`docs/porting/<slice>.md` recording what was measured, what is stubbed, and what is still open.

## Verification ladder

Cheapest first; run the rungs your change can affect. Details in the `native-port-measure`,
`renderer-spike-verify` and `windows-build-and-replays` skills.

```sh
python3 -m flake8 --max-line-length=100 scripts/ && actionlint .github/workflows/
./scripts/ci/fetch-probe-deps.sh && CLANGXX=clang++-14 python3 scripts/native-port-probe.py --json probe-native.json
python3 scripts/ci/check-probe-baseline.py --results probe-native.json
python3 scripts/porting-status.py --check
```

## Concurrent slices

`docs/porting/concurrent-slices.md` is the ownership register: before starting, claim your files there
and check nobody else holds them. Files that only one in-flight slice may touch at a time:
`docs/porting/ci-baselines/*.json`, `scripts/native-port-probe.py`, `dx8wrapper.*` /
`d3d8renderbackend.cpp`, and the root `CMakeLists.txt`. Merge one port PR at a time and re-measure
after each merge.

Raise, do not guess: product decisions (where macOS settings live, which behaviour is authoritative
when native and Windows disagree), scope changes into cut areas, a measured number that contradicts
`docs/porting/`, or a third consecutive failure on the same problem.

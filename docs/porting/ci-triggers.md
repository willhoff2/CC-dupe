# CI triggers: what runs, when, and why

Measured on 2026-08-15, on the workflows as they stood before this change.

## What it cost

`Native Port CI` fans out to 12 jobs, 3 of them on `macos-15`. Observed durations from successful
runs: native probe ~8 min, native build ~7-15 min, native build + renderer ~11-17 min; the whole
fan-out is roughly 45 runner-minutes, and GitHub bills macOS minutes at 10x Linux.

Two things multiplied that:

* The workflow triggered on `push` to `main` **and** `devin/**`, and on `pull_request` to `main`.
  Every port branch has a pull request, so each commit on a `devin/**` branch started the same
  12-job fan-out twice. `concurrency` cancels a superseded *push* run and a superseded *pull
  request* run, but the two are separate groups, so both halves survived and then queued behind
  each other.
* Every job ran for every diff. Of the recent commits on this repo, ~44% touch only `docs/` and
  `.agents/` — prose that cannot move a probe count, a renderer pixel comparison or a packet
  layout — and each of those paid the full 12 jobs.

Queue snapshot while measuring: **18 runs queued against 2 running**, and GenCI's cheap
"Detect File Changes" job — a `paths-filter` and two `echo`s — sat 10 minutes in the queue.

`ci.yml` (the upstream Windows/VC6 matrix, 13 build jobs) treated `.github/workflows/**` as a
`shared` path, so editing `native-port-ci.yml` rebuilt every Windows preset.

## What runs now

| event | what runs |
| --- | --- |
| `pull_request` to `main` | lint, the source scans, the D3D8 surface scanner, and only the heavy jobs whose measurement the diff can move |
| `push` to `main` | everything, unconditionally |
| `workflow_dispatch` | everything, unconditionally |
| `push` to `devin/**` | nothing — the pull request covers it |

The asymmetry is deliberate. Path gating decides what a pull request pays for; it never decides
what `main` is held to, because `main` is where every number quoted in `docs/porting/` comes from.

The `changes` job classifies the diff against the merge base with
`scripts/ci/classify-changes.py`, which prints four booleans:

| output | gates | matched by |
| --- | --- | --- |
| `code` | native probe, native build, native build + renderer, debug/profile seam (Linux + macOS), LAN packet layout | any changed path that is not prose (`docs/**`, `.agents/**`, `*.md`) |
| `renderer` | renderer spike (Linux + macOS) | `spikes/**`, `WW3D2/`, `W3DDevice/`, `cmake/dx8.cmake` |
| `window` | window seam spike builds (Linux + macOS) | `spikes/**`, `WWLib/platform/`, `KeyScanCodes.h`, `PlatformWindowHost.*`, `PlatformMain.cpp`, `Win32Device/`, `scripts/window-input-scan.py` |
| `audio` | OpenAL backend build | `OpenALAudioDevice/`, `WWAudio/`, `GameAudio/`, `cmake/openal.cmake`, `CMakeLists.txt`, `scripts/audio-surface-scan.py` |

Two properties matter more than the table:

* **Conservative by construction.** `code` is the complement of the prose list, so a path nobody
  thought about lands in `code` rather than being skipped. A change to CI plumbing itself
  (`native-port-ci.yml`, `scripts/ci/**`) sets *every* area, because a change to a gate has to be
  run through that gate. `docs/porting/ci-baselines/**` counts as plumbing rather than prose for the
  same reason: those files *are* the recorded numbers, so a diff that loosens one is the last diff
  that should skip the gate comparing against it.
* **Asserted, not trusted.** The rules are Python with a `--self-check` covering paths taken from
  real commits on this repo, and the `lint` job runs it. A misclassification does not fail
  anything by itself — it silently does not run a gate — which is exactly the failure mode that
  has to be tested rather than reviewed.

Three jobs are ungated, because they need no build and a docs-only change can genuinely break them:
`lint`, `d3d8-surface` (`docs/porting/STATUS.md` is generated, and `check-generated-baselines.py` is
the first diagnosis when a baseline was hand-merged) and `source-scans`.

`source-scans` is new and holds the three source-only window checks that used to sit inside the
build-heavy, gated window-seam jobs: `window-input-scan.py --check`,
`ci/check-window-scancodes.py` and `ci/check-window-seam-wiring.py`. The first walks the **whole
repository** and compares the Win32 window/event/input surface against a checked-in baseline, so any
engine file can move its count — gating it on a path list would let an unrelated file grow the
surface with the count unchecked. Splitting it out means the whole-repo ratchet runs on every PR
while the expensive spike builds stay gated.

## Branch protection

Require the single check named **`Native port CI`** (job `required-checks`). It runs with
`if: always()`, depends on every job, treats `skipped` as a pass and fails on any `failure` or
`cancelled`, and prints a table of every job's result. Requiring the individual jobs instead would
either block every docs-only pull request forever — a skipped required check never reports — or
need editing every time a job is added or renamed.

## The nightly measurement sweep

A scheduled Devin automation ("CC-dupe nightly port measurement sweep", daily 07:00 UTC, playbook
`port_measure`) re-measures `main` with the pinned toolchain and reconciles the prose and skills
with the baselines. It is the only thing that opens measurement PRs; no workflow in
`.github/workflows/` does.

What went wrong, measured on PRs #121–#133: thirteen consecutive sweeps ran against the **same**
commit (`632ba201f`, `main` had not moved), every one of them reported "no measurement drift", and
every one of them opened a new PR titled `docs(port): Nightly measurement sweep`. The prompt's
"open a PR only if something changed" test compared the tree against reality, and the tree was
stale in the same three places every night because the previous night's PR was never merged — so
"something changed" was true thirteen times for the same three findings. Nothing looked for an
existing sweep PR, and the title was a constant.

The contract now (enforced by the automation prompt; the repository side is the CI gates above):

* **What moves goes to CI, not to a sweep.** A quoted figure that disagrees with its baseline
  fails `check-doc-figures.py`; a gate the workflow runs that no skill names fails
  `check-skill-coverage.py`; both in the ungated `lint` job, on the push that caused them. Of the
  thirteen PRs' findings, every one was in one of those two classes or was a script bug — none was a
  moved measurement.
* **Nothing to say, nothing opened.** If `main` is the SHA the last sweep measured and its
  `Native port CI` run is green, the sweep stops and reports the SHA. No branch, no PR.
* **One long-lived branch.** Findings go on `nightly/measurement-sweep`, pushed to the existing
  open PR from that branch if there is one; a new PR only when there is none.
* **The title says what moved**: `docs(port): Sweep <sha7>: <what moved>` — e.g.
  `docs(port): Sweep 632ba20: shimmed probe 718 -> 719 / 762`. The bare title is forbidden.
* **A regression or a finding the sweep cannot land fails loudly**: the session ends reporting the
  regression and the causing commit, and opens nothing. A regression is never baselined by a sweep.
* **Coverage.** The sweep runs the same gates `native-port-ci.yml` runs on Linux — including the
  debug build and its negative controls, draw capacity, staging cost, the spike render, backend
  coverage, HiDPI, the swapchain check and the seam behaviour tests — as listed in
  `.agents/skills/native-port-measure/SKILL.md` and `renderer-spike-verify/SKILL.md`. It cannot run
  the macOS/arm64 jobs (no Mac runner in the automation); those stay CI-only, and the sweep says so.

## Also fixed here

* The `lint` rung of the verification ladder (`flake8`, `actionlint`) was documented but wired into
  no workflow. It is a job now. `.flake8` excludes the upstream tools under `scripts/cpp/` and
  `run-clang-tidy.py`/`fix_compile_commands.py`, which hold all 909 pre-existing violations;
  reformatting them would conflict with every future upstream merge. Everything the port owns is
  clean and gated. Two determinism notes, both learned from this branch's own CI:
  * The job pins Python to 3.10, the version the dev environment provides. Since PEP 701 (3.12)
    pycodestyle tokenizes inside f-strings, so a 3.12 runner reports E741/E501 inside f-string
    expressions that a 3.10 one cannot — the local rung passed while CI failed.
  * `SHELLCHECK_OPTS=--severity=error`. `actionlint` shells out to `shellcheck` whenever it is on
    PATH; it is on the hosted runners and usually absent locally. At `info`/`style`/`warning` the
    workflows carry 45 findings, mostly `SC2086` unquoted `$GITHUB_OUTPUT` in upstream files.
* `replaycheck-generalsmd` in `ci.yml` read `needs.detect-changes.outputs` without listing
  `detect-changes` in `needs`, so those outputs were the empty string and the replay check only
  ever ran on `workflow_dispatch`. `actionlint` reports it; adding the lint job surfaced it.
* `ci.yml`'s `shared` filter is now the three workflows that define the Windows builds instead of
  `.github/workflows/**`.

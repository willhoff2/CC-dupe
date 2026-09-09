---
name: native-port-measure
description: Measure native 64-bit port readiness in this repo — provision the pinned vendor headers, run the clang probe native and shimmed, gate against the checked-in baselines, and regenerate docs/porting/STATUS.md. Use whenever a change could affect portability or a doc quotes a probe number.
---

# Measuring native port readiness

The probe's answer is input-dependent. Run it exactly like this, or the number you get is not
comparable with the ones in `docs/porting/`.

## 1. Toolchain

Every checked-in figure was measured with **clang 14 on Ubuntu 22.04**. Use `clang++-14` if it is
available; if it is not, say which clang you used, because the clean count differs between versions.
`zlib1g-dev` must be installed — without `<zlib.h>`, `CompressionManager.cpp` fails to preprocess and
the clean count silently drops by one.

## 2. Provision the vendor headers

```sh
./scripts/ci/fetch-probe-deps.sh
```

This fetches dx8, gamespy, miles and lzhl at the commits `cmake/*.cmake` pin. Do **not** rely on an
untracked `build/docker/_deps` tree instead: whether it exists changes the result (the difference is
`Core/Libraries/Source/Compression`, which needs `lzhl.h`), which is exactly the reproducibility
defect `docs/porting/review-and-decisions.md` section 1.2 records.

## 3. Probe, both modes

```sh
CLANGXX=clang++-14 python3 scripts/native-port-probe.py --json probe-native.json
CLANGXX=clang++-14 python3 scripts/native-port-probe.py --with-shims --json probe-shimmed.json
```

`--with-shims` adds `scripts/native-port-shims/`. Report both: native is the honest portability
figure, shimmed is the one that moves as seams land.

## 4. Gate against the baselines

```sh
python3 scripts/ci/check-probe-baseline.py --results probe-native.json
python3 scripts/ci/check-probe-baseline.py --results probe-shimmed.json
```

A count *below* the baseline is a regression — find the commit, do not update the baseline. A count
*above* it is progress: copy the JSON over `docs/porting/ci-baselines/` in the same PR that earned it.
A changed `total` means the denominator moved (files added or removed); say so explicitly rather than
reporting the ratio as progress.

## 5. The rest of the gates, with their exact arguments

```sh
python3 scripts/ci/check-d3d8-surface.py
python3 scripts/ci/check-backend-coverage.py   # always with the line above, never alone
CLANGXX=clang++-14 python3 scripts/native-layout-test.py
python3 scripts/xfer-blob-audit.py
CLANGXX=clang++-14 python3 scripts/ci/check-crt-compat.py
```

The window/input seam has a baseline of its own (`ci-baselines/window-input-scan.json`) and two
structural gates; a measurement sweep runs all three:

```sh
python3 scripts/window-input-scan.py --check
python3 scripts/ci/check-window-scancodes.py
python3 scripts/ci/check-window-seam-wiring.py
```

The native build and its gate, whose denominator must equal the probe's for the two to be
comparable. CI measures it at two depths plus the debug configuration, and there is a checked-in
baseline for each of the three. Measure into a scratch path first and copy over the baseline only
after classifying the change — writing straight into `docs/porting/ci-baselines/` destroys the
before-state you are supposed to compare against:

```sh
# levels 1-3: the strict link is expected to fail (389 unresolved on main)
CLANGXX=clang++-14 python3 scripts/native-build.py --level 1 --level 2 --level 3 --with-shims \
  --strict-link --report /tmp/nb123.md --json /tmp/nb123.json
python3 scripts/ci/check-native-build-baseline.py --results /tmp/nb123.json
python3 scripts/ci/check-win32-undefined.py --results /tmp/nb123.json
python3 scripts/ci/check-audio-backend-linked.py --results /tmp/nb123.json
python3 scripts/ci/check-embedded-browser.py --results /tmp/nb123.json
python3 scripts/ci/check-online-absent-seam.py --results /tmp/nb123.json

# levels 1-4 (renderer and audio included): the strict link must produce a 64-bit executable,
# so here native-build.py exits 0 and a non-zero exit is a real failure
CLANGXX=clang++-14 python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \
  --with-shims --strict-link --report /tmp/nb1234.md --json /tmp/nb1234.json
python3 scripts/ci/check-native-build-baseline.py --results /tmp/nb1234.json
python3 scripts/ci/check-win32-undefined.py --results /tmp/nb1234.json
python3 scripts/ci/check-download-seam.py --results /tmp/nb1234.json
python3 scripts/ci/check-audio-backend-linked.py --results /tmp/nb1234.json
python3 scripts/ci/check-video-headers.py --results /tmp/nb1234.json
# reads the archive the levels 1-4 build just produced in build/native, so after it, not before
python3 scripts/ci/check-swapchain-compiled.py
```

The debug configuration is a measured configuration of its own (CI job `native-build-debug`) with
its own baseline: it compiles different code (`-DRTS_DEBUG -DWWDEBUG -DDEBUG`), so its numbers are
not comparable with release's and `check-native-build-baseline.py` refuses to compare them. Use
`--build-dir build/native-debug` — the default `build/native` holds the release configuration and
reusing it mixes the two. The four negative controls run out of that directory; they are what make
the debug build worth measuring, because a debug build whose asserts are compiled out or swallowed
by the portable dialog stub passes every other check. They generate their own bad input and need no
retail data:

```sh
CLANGXX=clang++-14 python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \
  --with-shims --config debug --strict-link --build-dir build/native-debug \
  --report /tmp/nbdebug.md --json /tmp/nbdebug.json
python3 scripts/ci/check-native-build-baseline.py --results /tmp/nbdebug.json
CLANGXX=clang++-14 python3 scripts/native-sim-probe.py --build-dir build/native-debug --build
python3 scripts/ci/check-assert-fires.py --build-dir build/native-debug
python3 scripts/ci/check-path-separator-keys.py --build-dir build/native-debug
python3 scripts/ci/check-shroud-bounds.py --build-dir build/native-debug
python3 scripts/ci/check-save-path-seam.py --build-dir build/native-debug   # save lands in Save/, listed, reopened
```

Linking a seam says nothing about what it does, so the `native-build*` and `debug-profile-seam` CI
jobs follow the build with the seams' behaviour tests. Each compiles its fixture with `CLANGXX`
against the tree and runs it; none needs retail data, and a sweep runs them because they are the
gates a header-only change is most likely to break without moving a single count:

```sh
export CLANGXX=clang++-14
python3 scripts/native-win32-file-test.py            # FindFirstFile patterns, GetFullPathName, case
python3 scripts/native-win32-runtime-test.py         # recursive mutex, Interlocked*, lstrcpyn
python3 scripts/native-win32-user32-test.py
python3 scripts/native-d3dx8math-test.py
python3 scripts/native-d3dx8-entrypoints-test.py
python3 scripts/native-init-failure-test.py
python3 scripts/native-base-game-install-test.py
python3 scripts/native-memory-shutdown-test.py       # allocator critical sections outlive statics
python3 scripts/native-exit-teardown-test.py         # object pools destroyed after the allocator
python3 scripts/native-lock-failure-test.py
python3 scripts/native-instance-lock-test.py
python3 scripts/native-death-veterancy-flags-test.py # zero-valued enumerators keep bit 31 at 64 bits
python3 scripts/native-stackwalk-test.py             # real symbolised backtrace; also run on macOS
```

The reports and baselines those replace when a measurement is accepted are
`docs/porting/native-build-report.md`,
`docs/porting/ci-baselines/native-build-shimmed-level1-2-3.json`,
`docs/porting/ci-baselines/native-build-shimmed-level1-2-3-4.json` and
`docs/porting/ci-baselines/native-build-shimmed-debug-level1-2-3-4.json`.

At levels 1-3 `--strict-link` is not optional even though it exits non-zero: the strict link is
expected to fail today, and the checker refuses to compare a result measured without it — the
executable figure it ratchets simply went unmeasured, so it reports `regressed` on an otherwise
identical tree. Read the non-zero exit of `native-build.py` as the strict link's own status and the
checker's exit as the gate.

The remaining source-only gates a sweep runs, none of which need a build:

```sh
python3 scripts/ci/check-generated-baselines.py           # every baseline still parses
CLANGXX=clang++-14 python3 scripts/ci/check-bool-pointer.py
CLANGXX=clang++-14 python3 scripts/ci/check-stackwalk-symbols.py
python3 scripts/ci/check-lanmessage-layout.py --clangxx clang++-14
python3 scripts/ci/check-widechar-wire.py --clangxx clang++-14   # lanmessage-layout job's second gate
CLANGXX=clang++-14 python3 scripts/ci/check-font-metrics.py       # needs fonts-dejavu-core + stb headers
python3 scripts/init-reporting-scan.py --check --quiet            # ci-baselines/init-reporting.json
python3 spikes/renderer/tools/d3d8-lock-scan.py --check
python3 spikes/renderer/tools/surface-lock-audit.py --check
```

`init-reporting-scan.py` ratchets how loudly each init entry point reports failure and runs for a
couple of minutes with no output until it finishes. `check-font-metrics.py` builds
`gdi_font_metrics_dump.cpp` against the GDI font seam and compares advance widths and line height
with `scripts/ci/font-metrics-reference.json`; it needs the `fonts-dejavu-core` font both sides are
pointed at and the stb headers `fetch-probe-deps.sh` provisions.

Two self-checks of the measurement harness itself, which a sweep runs because a broken categoriser
makes every number above unattributable:

```sh
python3 scripts/native-build-categorise-test.py
python3 scripts/ci/classify-changes.py --self-check
```

Two of the checked-in baselines are neither source scans nor build measurements:
`draw-capacity.json` and `staging-cost-ceiling.json` are measured by running the renderer spike under
lavapipe, and `check-hidpi-scale.py` and `check-spike-render.py` need the same spike binaries. A
sweep that skips the spike leaves them unmeasured. The build and the exact gate invocations are in
`.agents/skills/renderer-spike-verify/SKILL.md`; without their binary those gates print a usage
error or a "not found" hint and tell you nothing.

`scripts/ci/check-skill-coverage.py` asserts that every gate `native-port-ci.yml` runs is named in
some `.agents/skills/*/SKILL.md`, so a gate added to CI without a skill entry fails lint rather than
going unswept for a month. `scripts/ci/check-doc-figures.py` compares the headline figures quoted in
`docs/porting/native-port-plan.md` and `next-slice-scope.md` against the committed baselines, so a
prose figure that goes stale fails the same lint job.

The audio gates need the backend *built*, so they need the top-level CMake build (CMake >= 3.25,
`libopenal-dev`), and `check-openal-symbols.py` needs both of its paths. Use a build directory other
than `build/native`: that one belongs to `scripts/native-build.py`, which configures it from
`cmake/native/CMakeLists.txt`, so pointing `cmake -S .` at it after running the native build above
fails with `The source "…/CMakeLists.txt" does not match the source "…/cmake/native/CMakeLists.txt"
used to generate cache`. CI does not hit this only because the two run in separate jobs.

```sh
CC=clang-14 CXX=clang++-14 cmake -S . -B build/native-cmake -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_GENERALS=OFF
cmake --build build/native-cmake --target core_openalaudiodevice
python3 scripts/ci/check-openal-symbols.py \
  --header Core/Libraries/Source/OpenALAudioDevice/mss/mss.h \
  --archive build/native-cmake/Core/Libraries/Source/OpenALAudioDevice/libcore_openalaudiodevice.a
python3 scripts/audio-surface-scan.py --check
```

The backend's callback-thread contract (end-of-sample callbacks reach the engine on the thread that
calls `AIL_*`, inside one of its calls, never from the service thread — `sound-effects-chain.md` §4.1)
is gated by `scripts/native-audio-callback-test.py`, which compiles
`Core/Libraries/Source/OpenALAudioDevice/tests/openal_callback_thread_test.cpp` against the shim
sources directly (no CMake) and runs it on OpenAL Soft's `null` driver, so it needs only
`libopenal-dev` and `clang++-14`. `--shim-rev e1f8de610 --expect-defect` rebuilds the pre-fix shim
from git and must report the defect; `--json` writes the facts for a doc row.

```sh
CLANGXX=clang++-14 python3 scripts/native-audio-callback-test.py
```

The rendered-PCM gate is `scripts/native-audio-render-test.py` (`tests/openal_render_test.cpp`,
OpenAL Soft `wave` backend, needs `libopenal-dev`, `clang++-14`, NumPy and the minimp3 header —
`MINIMP3_INCLUDE_DIR=build/native/_deps/minimp3-src` after a native build). It plays synthetic
tones through the stream, one-shot and EOS-loop paths, judges the output with
`scripts/audio-pcm-discontinuity.py` and the shim's `OPENAL_AUDIO_DIAG` counters, and its `stall`
case injects a 1.2 s service-thread stall (`OPENAL_AUDIO_DIAG_STALL`) that the 4-buffer stream
queue fails and the 8-buffer queue survives (`ci-baselines/audio-render-discontinuity.json`).

```sh
CLANGXX=clang++-14 MINIMP3_INCLUDE_DIR=build/native/_deps/minimp3-src \
  python3 scripts/native-audio-render-test.py
```

The static-destruction gate is `scripts/native-audio-static-destruction-test.py`
(`tests/openal_static_destruction_test.cpp`, `null` driver, same deps as the callback test). It
starts the library, plays a 2D voice, a 3D voice and a looping stream, and returns from main with
AIL_shutdown never called; the process must exit 0 with diagnostics off, `OPENAL_AUDIO_DIAG=stderr`
and `OPENAL_AUDIO_DIAG=<file>` (the file's last line must be the `static-destruction` report).
`--shim-rev fbfc0f574 --expect-defect` (or `c6fd1bd7c`) rebuilds the pre-fix shim and must observe
the `std::terminate` abort (`memory-shutdown-order.md`, the OpenAL static-destruction section).

```sh
CLANGXX=clang++-14 MINIMP3_INCLUDE_DIR=build/native/_deps/minimp3-src \
  python3 scripts/native-audio-static-destruction-test.py
```

Ubuntu 22.04 ships CMake 3.22, which the top-level `cmake_minimum_required(3.25)` rejects; the
`audio-surface-scan.py --check` half of the pair still runs without a build, but the symbol gate does
not. `pip install --user 'cmake==4.1.2'` provides a new enough `~/.local/bin/cmake` without touching
the system one; that is the version the audio CI job pins, so it configures the same way.

It does, however, shadow `/usr/bin/cmake` on `PATH` for the rest of the session, and CMake 4's
`FindVulkan` fails on jammy with `Could NOT find Vulkan (missing: Vulkan_LIBRARY)` even though
`libvulkan-dev` is installed. Configure the renderer spike with an explicit `/usr/bin/cmake` (see the
`renderer-spike-verify` skill) once you have installed this one.

## 6. On a Mac, and only on a Mac

The procedure, the Homebrew keg paths macOS needs and what each shim is for live in
`docs/porting/native-build.md` §"Building it on macOS (Apple Silicon), and proving the result is
arm64". Two things belong here because they invalidate a whole session's conclusions if skipped:

```sh
lipo -archs build/native/native_strict_link   # must say arm64, on its own line, and nothing else
sysctl -n sysctl.proc_translated              # must say 0
```

An x86-64 build under Rosetta reports "Apple Silicon" in every other field, and a universal binary is
not the thin native artefact these numbers describe. `native-build.py` records both (`lipo_archs`,
`host_translated`) and `check-native-build-baseline.py` fails on either, so the check is not something
you have to remember — but reading it in the output is.

Do **not** overwrite `docs/porting/ci-baselines/*.json` from a Mac: they are the clang-14 Linux
ratchet and AppleClang's figures are not comparable (decision 5 of `decisions-resolved.md`; macOS
gets an advisory baseline of its own, which does not exist yet). Put the macOS numbers in the PR
description or a doc that names the compiler.

## 7. Regenerate the status document

```sh
python3 scripts/porting-status.py          # rewrite docs/porting/STATUS.md
python3 scripts/porting-status.py --check  # what CI runs
```

The native build's `First diagnostic` column is attributed by following clang's `In file included
from` chain back to the translation unit at its head, not by looking for the file's own name in the
log: most failures report their first error inside an included header, so a name scan silently leaves
the column empty. If a re-measurement changes only that column, suspect the attribution and not the
tree.

`STATUS.md` is generated from the baselines and must never be hand-edited. Then grep the rest of
`docs/porting/*.md` for any figure your change moved and update or mark it superseded.

## Pitfalls that have each cost a session

- The probe always writes its markdown report, even when you only asked for `--json`: it leaves an
  untracked `native-port-probe.md` in the repo root, whose content is whichever mode ran last. It is
  not the checked-in report and must not be committed — delete it, and never `git add .` after a
  sweep.
- The linked binary's `bytes` and `file_output` (`strict_link.binary`) are recorded but not
  ratcheted, and neither is stable between boxes: two runs on the same tree give the same byte count
  but a different `BuildID`, and a box without `file(1)` records `file_output: null` while every gate
  still passes (`check-native-build-baseline.py` reads `word_size`/`machine`). A byte count that
  differs from the baseline by a few tens of KiB is build environment, not drift — do not rebaseline
  it and do not chase it in the prose. `apt-get install -y file` before overwriting a baseline so the
  field is populated the way CI records it.
- Never add `Core/Libraries/Source` as a blanket `-I`: its `debug/` and `profile/` subdirectories
  shadow libstdc++'s internal `<debug/...>` and `<profile/...>` headers and produce ~6,000 spurious
  errors inside the standard library.
- The probe force-includes `Utility/CppMacros.h`, as the MSVC build does via `/FIUtility/CppMacros.h`.
  Without it the VC6 compatibility macros are undefined everywhere.
- Keep `-fms-extensions`. Dropping it costs ~65 errors from `__int64` and `__forceinline` alone.
- Report `clean / total`, never a bare percentage.
- `scripts/ci/check-crt-compat.py`, `scripts/ci/check-bool-pointer.py`,
  `scripts/ci/check-stackwalk-symbols.py` and `scripts/ci/check-font-metrics.py` take their compiler
  from the probe, i.e. from `CLANGXX`, whose default is plain `clang++`. On a box that only has
  `clang++-14` they die with `FileNotFoundError: 'clang++'`, which is a missing env var and not a
  gate failure — set `CLANGXX=clang++-14` as CI does. `check-lanmessage-layout.py` and
  `check-widechar-wire.py` take `--clangxx` instead.
- The layout test's 32-bit check needs `g++-multilib`; without it that check is skipped, not failed,
  and the sweep is incomplete until you install it and see the ILP32 assertions actually pass.
- Opt-in backends (`probe.OPTIONAL_BACKENDS`, currently the SDL2 window backend) are excluded from
  both the probe and the native build. If only one of the two excludes them, their denominators
  drift apart and the native-build gate reports a denominator change that means nothing.

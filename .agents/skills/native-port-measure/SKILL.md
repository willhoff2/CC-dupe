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
comparable. CI measures it at two depths and there is a checked-in baseline for each. Measure into a
scratch path first and copy over the baseline only after classifying the change — writing straight
into `docs/porting/ci-baselines/` destroys the before-state you are supposed to compare against:

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
python3 scripts/ci/check-swapchain-compiled.py   # reads the archive this build produced
```

The reports and baselines those replace when a measurement is accepted are
`docs/porting/native-build-report.md`,
`docs/porting/ci-baselines/native-build-shimmed-level1-2-3.json` and
`docs/porting/ci-baselines/native-build-shimmed-level1-2-3-4.json`.

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
python3 scripts/ci/check-widechar-wire.py --clangxx clang++-14
CLANGXX=clang++-14 python3 scripts/ci/check-font-metrics.py
python3 scripts/init-reporting-scan.py --check --quiet
python3 spikes/renderer/tools/d3d8-lock-scan.py --check
python3 spikes/renderer/tools/surface-lock-audit.py --check
```

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

Ubuntu 22.04 ships CMake 3.22, which the top-level `cmake_minimum_required(3.25)` rejects; the
`audio-surface-scan.py --check` half of the pair still runs without a build, but the symbol gate does
not. `pip install --user 'cmake==4.1.2'` provides a new enough `~/.local/bin/cmake` without touching
the system one; that is the version the audio CI job pins, so it configures the same way.

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

- Never add `Core/Libraries/Source` as a blanket `-I`: its `debug/` and `profile/` subdirectories
  shadow libstdc++'s internal `<debug/...>` and `<profile/...>` headers and produce ~6,000 spurious
  errors inside the standard library.
- The probe force-includes `Utility/CppMacros.h`, as the MSVC build does via `/FIUtility/CppMacros.h`.
  Without it the VC6 compatibility macros are undefined everywhere.
- Keep `-fms-extensions`. Dropping it costs ~65 errors from `__int64` and `__forceinline` alone.
- Report `clean / total`, never a bare percentage.
- `scripts/ci/check-crt-compat.py`, `scripts/ci/check-bool-pointer.py` and
  `scripts/ci/check-stackwalk-symbols.py` take their compiler from the probe, i.e. from `CLANGXX`,
  whose default is plain `clang++`. On a box that only has `clang++-14` they die with
  `FileNotFoundError: 'clang++'`, which is a missing env var and not a gate failure — set
  `CLANGXX=clang++-14` as CI does. `check-lanmessage-layout.py` takes `--clangxx` instead.
- The layout test's 32-bit check needs `g++-multilib`; without it that check is skipped, not failed,
  and the sweep is incomplete until you install it and see the ILP32 assertions actually pass.
- Opt-in backends (`probe.OPTIONAL_BACKENDS`, currently the SDL2 window backend) are excluded from
  both the probe and the native build. If only one of the two excludes them, their denominators
  drift apart and the native-build gate reports a denominator change that means nothing.

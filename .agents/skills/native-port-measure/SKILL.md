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
CLANGXX=clang++-14 python3 scripts/native-layout-test.py
python3 scripts/xfer-blob-audit.py
```

The native build and its gate, whose denominator must equal the probe's for the two to be
comparable:

```sh
CLANGXX=clang++-14 python3 scripts/native-build.py --level 1 --level 2 --with-shims \
  --report docs/porting/native-build-report.md \
  --json docs/porting/ci-baselines/native-build-shimmed-level1-2.json
python3 scripts/ci/check-native-build-baseline.py --results docs/porting/ci-baselines/native-build-shimmed-level1-2.json
```

The audio gates need the backend *built*, so they need the top-level CMake build (CMake >= 3.25,
`libopenal-dev`), and `check-openal-symbols.py` needs both of its paths:

```sh
CC=clang-14 CXX=clang++-14 cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DRTS_BUILD_ZEROHOUR=ON -DRTS_BUILD_GENERALS=OFF
cmake --build build/native --target core_openalaudiodevice
python3 scripts/ci/check-openal-symbols.py \
  --header Core/Libraries/Source/OpenALAudioDevice/mss/mss.h \
  --archive build/native/Core/Libraries/Source/OpenALAudioDevice/libcore_openalaudiodevice.a
python3 scripts/audio-surface-scan.py --check
```

## 6. Regenerate the status document

```sh
python3 scripts/porting-status.py          # rewrite docs/porting/STATUS.md
python3 scripts/porting-status.py --check  # what CI runs
```

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
- The layout test's 32-bit check needs `g++-multilib`; without it that check is skipped, not failed,
  and the sweep is incomplete until you install it and see the ILP32 assertions actually pass.
- Opt-in backends (`probe.OPTIONAL_BACKENDS`, currently the SDL2 window backend) are excluded from
  both the probe and the native build. If only one of the two excludes them, their denominators
  drift apart and the native-build gate reports a denominator change that means nothing.

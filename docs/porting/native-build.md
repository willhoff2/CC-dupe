# Native 64-bit build: what a real compiler and a real linker say

Every earlier number in `docs/porting/` was produced by `clang++ -fsyntax-only`. That answers "does
this parse and type-check", and nothing else: no object file had ever been produced for a 64-bit
non-Windows target, and no linker had ever run over the engine. Code generation failures and
undefined symbols were entirely unmeasured, so there was no way to know how much the probe's
`621/742` native and `650/742` shimmed clean counts were worth.

This slice closes that gap. The deliverable is not a binary — it is the categorised undefined-symbol
list, plus an explicit count of translation units the probe called clean that then failed to
compile.

- Driver: `scripts/native-build.py` (`scripts/native_probe_targets.py` reuses the probe's own target,
  source and include definitions, so the two measurements cannot silently diverge)
- CMake project: `cmake/native/CMakeLists.txt`
- Generated report: `docs/porting/native-build-report.md`
- CI ratchet: the `Native build (Linux, clang 14, 64-bit)` job in `.github/workflows/native-port-ci.yml`,
  gated by `scripts/ci/check-native-build-baseline.py` against
  `docs/porting/ci-baselines/native-build-shimmed-level1-2.json`

> The figures below are this slice's own measurement and are kept as its record. They have since
> been superseded: [`crt-and-widechar-compat.md`](crt-and-widechar-compat.md) took levels 1+2 from
> 663/716 objects and 522 unresolved symbols to 686/715 and 469, and corrected the translation-unit
> denominator (the SDL2 backend and `GameMemoryNull.cpp` were being counted although the configured
> build compiles neither). For current numbers see [`STATUS.md`](STATUS.md), which is generated.

## Measured, on Linux x86-64 with clang 14

| | Level 1 (core libraries, no shims) | Levels 1+2 (adds GameEngine, shimmed) |
|---|---:|---:|
| Translation units | 127 | 716 |
| Object files produced | 116 | 663 |
| Probe-clean units | 116 | 663 |
| **Probe-clean but failed to compile** | **0** | **0** |
| Archives linked | 5 | 7 |
| Unresolved symbols after linking | 18 | 522 |

The headline answer to "how much should we trust the probe's figures": in the scope built here, the
divergence is zero. Not one translation unit that `-fsyntax-only` accepted failed once it had to
produce code. The probe over-reports readiness in a different way instead — it is per-file, so it
never sees a symbol that no file in the build defines, which is what §3 of the generated report is
for.

`--whole-archive` forces every object in, so nothing is hidden by the linker being allowed to skip
unused members, and `--warn-unresolved-symbols` lets the link finish so the *whole* list is
produced rather than the first error. `nm` on the archives, not the linker log, is the source of the
symbol set; symbols exported by libc, libstdc++, libm, libpthread, libdl and libgcc are discounted.

### Level 1: the portable core links

18 unresolved symbols, and every one of them is expected: 9 from lzhl/zlib (real libraries this
harness deliberately does not link) and 9 defined in the 11 translation units that did not compile.
No Win32, no D3D8, no Miles. `Compression`, `WWMath`, `WWLib`, `WWDebug` and `WWSaveLoad` are, on
this evidence, genuinely portable to 64-bit.

### Levels 1+2: the shape of the remaining work

522 unresolved symbols. Read the categories in the generated report rather than the total — 321 of
them are simply defined in one of the 53 files that failed to compile, so they are the same blocker
counted again, and 103 are `TheKey_*` well-known Dict keys whose single definition lives in
`GameEngineDevice`, a layer this build does not include. What is left is small and specific: 18
GameSpy, 17 in unbuilt layers, 10 Win32 API, 5 D3DX, 9 lzhl/zlib.

Only **10 distinct Win32 entry points** are actually referenced by the engine code that compiles.
That is the size of the platform layer at this level, and it is much smaller than the `windows.h`
include count suggested.

## Two blockers this found that no syntax check could

**`LANMessage` no longer fits its own packet.** 17 of the 53 failures are one static assertion:
`sizeof(LANMessage) <= MAX_LANAPI_PACKET_SIZE` (`Core/GameEngine/Include/GameNetwork/LANAPI.h:270`).
At 64-bit the struct grows past the retail wire packet size. It is a genuine 64-bit correctness
blocker, it belongs to the save/serialisation slice rather than this one, and LAN play is out of
scope for single-player anyway — but it blocks compilation of a large part of `GameNetwork`, so it
cannot simply be ignored.

**`_strlwr` had the wrong language linkage.** `Dependencies/Utility/Utility/string_compat.h` defined
`_strlwr` with C++ linkage while the GameSpy SDK declares it for non-Windows targets inside its own
`extern "C"` block. Every translation unit reaching both failed with "different language linkage" —
33 of them. Fixed here (and `_strupr` added alongside, which the SDK also declares); that single
change moved the shimmed level 1+2 count from 642 to 663 objects. Windows is unaffected: the header
is only reachable through the `#ifndef _WIN32` branch of `Utility/compat.h`.

A third, harness-level: the GameSpy SDK's headers include their siblings unqualified, so
`_deps/gamespy-src/include/gamespy` has to be on the include path as well as `.../include`. The
build adds it (`EXTRA_DEP_INCLUDES` in `scripts/native-build.py`); the probe's baselines are left
alone so they stay comparable with the numbers already published.

## What this does not show

- **This is Linux x86-64, not macOS arm64.** Nothing here has been run on Apple Silicon. The
  categorisation of "which symbols the system supplies" is Linux-specific (`nm` output and the
  system library set differ on Mach-O), and the `--whole-archive`/`--warn-unresolved-symbols` flags
  are GNU ld spellings. Treat every count as a Linux measurement until a Mac session reproduces it.
- **The link is not clean, and is not meant to be.** A binary is produced because unresolved symbols
  are warnings, not errors; the report records `link_binary_produced` and `link_clean` separately so
  the difference cannot be glossed over.
- **Symbol attribution to failed files is a source-text scan.** Recovering it properly would need
  the object files that by definition do not exist, so `native-build.py` reads definitions out of
  the sources. It never attributes a cause to a symbol none of those files mentions, but overloads
  and macro-generated definitions can be misfiled — the `TheKey_*` keys are exactly that case,
  handled explicitly.
- **Levels 3+ are not built.** `WW3D2`, `GameEngineDevice` and `Main` need the renderer resource
  seam and the window/event-loop replacement first.

## Reproducing

```sh
bash scripts/ci/fetch-probe-deps.sh
python3 scripts/native-build.py --level 1 --level 2 --with-shims \
    --report docs/porting/native-build-report.md --json native-build.json
python3 scripts/ci/check-native-build-baseline.py --results native-build.json
```

Add `--update` to the last command, in the PR that earns it, when the counts improve.

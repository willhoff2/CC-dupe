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
  `docs/porting/ci-baselines/native-build-shimmed-level1-2-3.json` (one baseline per mode and level
  set: the file is named after what it measures, so adding a level cannot be read as progress
  against the smaller build's figures)

> The level 1 and levels 1+2 figures below are the original slice's own measurement and are kept as
> its record; they were already superseded once by
> [`crt-and-widechar-compat.md`](crt-and-widechar-compat.md) (679/717 objects and 376 unresolved to
> 704/716 and 273, plus a corrected denominator: the SDL2 backend and `GameMemoryNull.cpp` were
> counted although the configured build compiles neither). The **levels 1+2+3** section is the
> current measurement and the one CI ratchets.

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

## Levels 1+2+3, measured 2026-08-14 on b9199724 with clang 14 (current)

Level 3 adds `Core/GameEngineDevice`, `GeneralsMD/Code/GameEngineDevice` and `GeneralsMD/Code/Main`
— the device layer and the entry point. It was added to stop two of the largest undefined-symbol
categories being artefacts of the build's own scope, and lzhl and zlib are now linked.

| | Levels 1+2 (previous baseline) | Levels 1+2+3 (this baseline) |
|---|---:|---:|
| Translation units | 718 | 829 |
| Object files produced | 708 | 748 |
| Probe-clean units | 708 | 748 |
| **Probe-clean but failed to compile** | **0** | **0** |
| Compile failures | 10 | 81 |
| Archives linked | 7 | 9 |
| Unresolved symbols after linking | 280 | 393 |

Read that as clean/total, never as a percentage: the denominator moved by 111 translation units.
Level 3 contributes 40 objects out of 111 units, and 71 of the 81 failures. `GeneralsMD/Code/Main`
produces **no archive at all** — both its translation units fail (`eh.h`, and a `sizeof(long)`
static assertion) — which is why the JSON carries `libraries_without_archive` and the gate fails if
any *further* library joins it.

The two libraries are linked separately from the engine archives and are excluded from the object,
translation-unit and archive counts above: `liblzhl` is built from the pinned
`_deps/lzhl-src/CompLibHeader` sources, zlib is the system `libz`. Measuring vendor code as port
progress would be the same mistake as measuring the denominator.

### What the 125 "artefact" symbols turned out to be

| Levels 1+2 category | Symbols | Where they are now |
|---|---:|---|
| Well-known Dict keys | 104 | all 104 still unresolved, and all 104 blocked by **one named file**: `Core/GameEngineDevice/.../WorldHeightMap.cpp`, the only unit that sets `INSTANTIATE_WELL_KNOWN_KEYS`, fails on `TheD3D8RenderBackend` (renderer slice) — since resolved, see the current baseline below |
| Defined in a layer not built here | 21 | 2 resolved (`MOTDSystem`, `ReloadAllTextures`), 14 now attributed to a named failed unit, 3 still in the unbuilt `WW3D2` renderer, 2 behind a disabled `#if` |

So of the 125, **2 were resolved by building the layer and 123 were real** — but none of them is
"defined in a layer we chose not to build" any more: each is now attributable to a named file or a
named cut. The category renaming is the deliverable here, not the total.

### Undefined symbols, before and after, by cause

| Cause | Levels 1+2 | Levels 1+2+3 |
|---|---:|---:|
| Defined in a translation unit that failed to compile | 33 | 109 |
| Well-known Dict keys (now: `WorldHeightMap.cpp` failed to compile) | 104 | 104 |
| Defined in a layer not built here (renderer / audio) | 21 | 72 |
| Win32 API | 43 | 44 |
| GameSpy SDK (cut scope, not linked) | 18 | 33 |
| Defined only in a backend this configuration excludes (SDL2 / Cocoa) | — | 11 |
| Defined in a built translation unit behind a disabled `#if` | — | 6 |
| Generated gitinfo (build-time, not a blocker) | 6 | 6 |
| Direct3D 8 / DirectX | 5 | 5 |
| COM / OLE (browser embedding, cut scope) | — | 3 |
| Engine C++ not built at this level | 22 | **0** |
| Other / unclassified | 19 | **0** |
| Third-party library not linked (lzhl, zlib) | 9 | **0** |
| **Total** | **280** | **393** |

The three zeroes are the point. `Other / unclassified` held `IID_IUnknown`/`IID_IBrowserDispatch`
(COM GUIDs from the cut browser embedding; `_com_util::ConvertStringToBSTR` joins them from the
fallback bucket) and the `GenerateAuthA`/`PersistThink`/`NewGame`/`*StatsConnection` cluster, which
is GameSpy: those names
are not prefixed `gp`/`qr2`/`sb`, so a prefix rule could never have found them. They are now matched
against the declarations — including the SDK's own `#define GenerateAuth GenerateAuthA` aliases — in
the fetched GameSpy headers, which is a fact about the SDK rather than a guess about a name. Nothing
GameSpy was implemented; it is cut scope and is now labelled as such.

`Engine C++ not built at this level` was the fallback bucket, and emptying it needed two things
beyond a name scan: definitions whose return type sits on the previous line, and the symbols no text
scan can ever find — `typeinfo for X`, `vtable for X`, thunks. Those are attributed through the
*class*: whichever file defines `X`'s members answers for them.

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

## The renderer-seam slice, measured 2026-08-14 on eb9e98110 with clang 14 (current baseline)

Same command, same box (Linux x86-64, `clang++-14`), before and after making the renderer layer
compile off Windows:

| | Before (`eb9e98110`) | After |
|---|---:|---:|
| Translation units | 835 | 834 |
| Object files produced | 756 | 783 |
| Compile failures | 79 | 51 |
| `use of undeclared identifier 'TheD3D8RenderBackend'` failures | **37** | **0** |
| Archives linked | 9 | 9 |
| Unresolved symbols after linking | 341 | 435 |

The translation-unit count drops by one because `GeneralsMD/Code/Main/WinMain.cpp` is no longer
measured: `GeneralsMD/Code/Main/CMakeLists.txt` compiles it only under `if(WIN32)` and compiles
`PlatformMain.cpp` otherwise, so it is a mutually exclusive alternative in the sense
`native-port-probe.py` already had a list for, alongside `GameMemoryNull.cpp`. Counting the MSVC
SEH entry point as a native port blocker measured the harness; its `eh.h` failure was never going
to be fixed, because the native build does not compile the file.

**Unresolved symbols went up, and that is not a regression — it is what "more code links" looks
like at this level.** The 27 new objects are `W3DDevice` units that reference the `WW3D2`
renderer library, which levels 1+2+3 do not build. By cause:

| Cause | Before | After |
|---|---:|---:|
| Well-known Dict keys (`WorldHeightMap.cpp` failed to compile) | 104 | **0** |
| Defined in a layer not built here (renderer / audio) | 72 | 221 |
| Defined in a translation unit that failed to compile | 104 | 146 |
| Win32 API | 0 | 3 |
| Direct3D 8 / DirectX | 0 | 3 |
| Defined in a built translation unit behind a disabled `#if` | 7 | 8 |
| unchanged (GameSpy 33, SDL2/Cocoa 12, gitinfo 6, COM/OLE 3) | 54 | 54 |
| **Total** | **341** | **435** |

The 104 `TheKey_*` well-known Dict keys are the headline: they were all blocked by one file,
`Core/GameEngineDevice/.../WorldHeightMap.cpp`, which is one of the 37, and they are now defined.
The +149 in "layer not built here" is entirely `WW3D2`, and it is the next slice's number, not
this one's; the total will not fall until the renderer library itself is built.

The three new "Win32 API" symbols are `GetCursorPos`, `ScreenToClient` and `SetCursor`, all from
`W3DMouse.cpp`, which is one of the 37 and now compiles. `check-win32-undefined.py`'s budget is
**deliberately widened** from 0 to those three names, rather than defined or guarded: a portable
definition needs the pointer position from the window seam (`platform/window-seam-wiring`), and
`WWLib/platform/platform_win32_*` belongs to `platform/win32-file-api`, so both fixes are edits to
another in-flight slice's files. Whoever lands the cursor path should take the budget back to 0.

`GeneralsMD/Code/Main` still produces **no archive**, now out of one unit rather than two.
`PlatformMain.cpp` has exactly one remaining diagnostic, and it is not this slice's:

```
Core/GameEngineDevice/Include/VideoDevice/Bink/BinkVideoPlayer.h:49: fatal error: 'bink.h' file not found
```

It is fatal, it is reached through `Win32GameEngine.h`, and `VideoDevice/**` belongs to the
concurrent Bink/FFmpeg slice. The other two blockers are fixed: the `sizeof(long)` static assertion,
and `MilesAudioManager.h`'s `HANDLE m_mutex`, which is now held as the `void*` that `HANDLE` is —
the same treatment as `rts::ClientInstance::s_instanceLock`, and no Win32 type shim was touched.

So criterion "Main produces objects" is **not met**, and cannot be met from inside this slice: it
unblocks when the Bink slice lands.

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
- **`WW3D2` is still not built.** Level 3 covers `GameEngineDevice` and `Main`; the renderer library
  itself needs the renderer resource seam first, which is where the 72 "layer not built here"
  symbols and `WorldHeightMap.cpp`'s `TheD3D8RenderBackend` failure lead. The 72 has since become
  221 for exactly that reason.
- **The remaining compile failures are reported, not fixed.** The Win32 surface (`HWND`/`HFONT`/
  `HRESULT`/`SetWindowText`), the GameSpy socket units and the Bink/FFmpeg/stb video devices belong
  to other slices or to cut scope. This build's job is to count them and name them.
- **The level-3 numbers are Linux only.** zlib is found by probing a list of platform library paths
  that includes `/usr/lib/libz.dylib`; that macOS entry has never been executed and is written blind.

## Reproducing

```sh
bash scripts/ci/fetch-probe-deps.sh
python3 scripts/native-build.py --level 1 --level 2 --level 3 --with-shims \
    --report docs/porting/native-build-report.md --json native-build.json
python3 scripts/ci/check-native-build-baseline.py --results native-build.json
```

Add `--update` to the last command, in the PR that earns it, when the counts improve.

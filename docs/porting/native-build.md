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
- Generated report: `docs/porting/native-build-report.md` (levels 1-4, the largest build measured)
- CI ratchets: the `Native build (Linux, clang 14, 64-bit)` and
  `Native build + renderer (Linux, clang 14, 64-bit)` jobs in
  `.github/workflows/native-port-ci.yml`, gated by `scripts/ci/check-native-build-baseline.py`
  against `docs/porting/ci-baselines/native-build-shimmed-level1-2-3.json` and
  `...-level1-2-3-4.json` (one baseline per mode and level set: the file is named after what it
  measures, so adding a level cannot be read as progress against the smaller build's figures)

> The level 1 and levels 1+2 figures below are the original slice's own measurement and are kept as
> its record; they were already superseded once by
> [`crt-and-widechar-compat.md`](crt-and-widechar-compat.md) (679/717 objects and 376 unresolved to
> 704/716 and 273, plus a corrected denominator: the SDL2 backend and `GameMemoryNull.cpp` were
> counted although the configured build compiles neither). The **levels 1+2+3+4** section at the
> end is the current measurement; levels 1+2+3 and 1+2+3+4 are both ratcheted, each against its own
> baseline file.

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

## The wave-3 stack, measured together on that branch with clang 14

The three wave-3 slices — video/Bink excision, the renderer layer off Windows, and the GDI font
seam — were measured separately, each against `main`, so their numbers do not add up. Measured as
one branch:

| | `main` | Stacked |
|---|---:|---:|
| Translation units | 835 | 836 |
| Object files produced | 756 | 816 |
| Compile failures | 79 | 20 |
| Probe-clean but uncompilable | 0 | 0 |
| Unresolved symbols after linking | 341 | 457 |

Two facts the per-slice reports could not show:

- **`GeneralsMD/Code/Main` still produces no archive**, and the reason has changed. `bink.h` is gone
  and the `sizeof(long)` assertion is gone, so the remaining diagnostic is
  `allocating an object of abstract class type 'CComObject<W3DWebBrowser>'` — the COM browser
  embedding, which is cut scope and now the sole thing between this build and a game target.
- **Five units failed with `redefinition of '_D3DMATRIX'`, which was a shim collision rather than a
  port defect** — fixed here. Once the renderer slice makes the vendored `d3d8types.h` reachable off
  Windows, a unit can reach both it and `scripts/native-port-shims/d3d8types.h` (through the shim's
  `d3dx8math.h`), and `#pragma once` does not deduplicate two different files defining the same type.
  The shim now carries the vendored header's own `_D3D8TYPES_H_` guard, so whichever is found first
  is the only definition. The renderer slice's report recorded these units with an empty diagnostic,
  so the cause only became visible with this branch's diagnostic attribution and the vendored headers
  in one tree: +4 objects (`W3DView`, `W3DWater` and the three shadow units).

Unresolved symbols rise for the reason the slices each predicted: 272 of the 457 are defined in
`WW3D2`, which levels 1+2+3 do not build. The total will not fall until the renderer library is
built.

## The browser excision, and the first link of the real game target (current baseline)

| | wave-3 stack | with the browser excised |
|---|---:|---:|
| Translation units | 836 | 836 |
| Object files produced | 816 | **819** |
| Compile failures | 20 | **17** |
| Unresolved symbols after linking | 457 | **467** |
| `GeneralsMD/Code/Main` | 0 / 1, no archive | **1 / 1, archive** |
| Link entry point | this script's stub `main()` | **`PlatformMain.cpp`'s `main()`** |

The `CComObject<W3DWebBrowser>` blocker above is gone, so the sentence "`GeneralsMD/Code/Main`
produces no archive" is no longer true and the link is anchored by the game's own entry point rather
than by a `main()` the harness writes. `docs/porting/embedded-browser-seam.md` has the seam, the
per-category movement (the total rises because three more translation units contribute their
references) and what the categorised link says is missing for a binary that could start. Two facts
that only appear once the game target links: `native-build.py` had to reproduce `Main`'s
CMake-generated `BuildVersion.h`/`GeneratedVersion.h`, and it removes the two standalone test tools'
`main()` objects from the archives, since a duplicate entry point is otherwise all the linker reports.

The kernel32/CRT gap slice has since moved this on to 826/839 objects with 13 compile failures, and
the unresolved total to 546 for the same reason — see `docs/porting/win32-runtime-and-crt-gaps.md`.
`docs/porting/native-build-report.md` is the generated, authoritative version of these counts.

## Level 4: the renderer and audio libraries, measured 2026-08-15 on ac520adf8 with clang 14 (current baseline)

Level 4 adds the four libraries `native-port-probe.py` already knew as `RENDERER_TARGETS`:
`Core/Libraries/Source/WWVegas/{WW3D2,WWAudio,WWDownload}` and `GeneralsMD`'s `WWVegas` fork. They
were excluded from levels 1-3 because they did not compile off Windows at all; the wave-3 renderer
slice changed that, and until they are built the unresolved total is not a portability measurement —
272 of the 457 symbols levels 1-3 could not resolve were simply defined in a layer the build had
excluded.

Same command, same box (Linux x86-64, `clang++-14`, `--with-shims`), all three columns measured in
this session:

| | Levels 1-3 on `ac520adf8` | Levels 1-3 + the compat fix | Levels 1-4 + the compat fix |
|---|---:|---:|---:|
| Translation units | 836 | 836 | 968 |
| Object files produced | 816 | 817 | 935 |
| Probe-clean but uncompilable | 0 | 0 | 0 |
| Compile failures | 20 | 19 | 33 |
| Archives linked | 9 | 9 | 12 |
| Unresolved symbols after linking | 457 | 503 | 386 |

Read the middle column before the right one. The compat fix (below) makes one more level-1-3 unit
compile, `W3DAssetManager.cpp`, and that *raises* the unresolved total by 46, because everything it
calls lives in `WW3D2`. That is the artefact this level exists to remove, caught in isolation: at
levels 1-3 the same change looks like a 46-symbol regression, and at level 4 it is 8 fewer failures
and 71 fewer unresolved symbols than `main`.

### Per library

| Level-4 library | Objects | Translation units | Compile failures |
|---|---:|---:|---:|
| `Core/Libraries/Source/WWVegas/WW3D2` | 66 | 74 | 8 |
| `Core/Libraries/Source/WWVegas/WWAudio` | 19 | 19 | **0** |
| `Core/Libraries/Source/WWVegas/WWDownload` | **0** | 4 | 4 |
| `GeneralsMD/Code/Libraries/Source/WWVegas` | 33 | 35 | 2 |
| **Level 4 total** | **118** | **132** | **14** |

`WWAudio` compiles clean, all 19 units, which no measurement had ever shown. `WWDownload` produces
**no archive** — it joins `GeneralsMD/Code/Main` in `libraries_without_archive`, so the gate fails if
a third library ever does.

Both of those have since been resolved and `libraries_without_archive` is now empty: the browser
excision earned `GeneralsMD/Code/Main` its archive, and the patch downloader's four units were ported
rather than excluded — 968/972 objects at levels 1-4 as of `8bb8aff56`, with `check-download-seam.py`
holding the downloader. See
[`ww3d2-and-download-headers.md`](ww3d2-and-download-headers.md). `docs/porting/native-build-report.md`
and `STATUS.md` are the generated, authoritative counts.

### Undefined symbols, by cause

| Cause | Levels 1-3 on `ac520adf8` | Levels 1-4 |
|---|---:|---:|
| Defined in a layer not built here (renderer / audio) | 272 | **0** |
| Defined in a translation unit that failed to compile | 86 | 214 |
| Miles Sound System (`AIL_*`) | 0 | 74 |
| GameSpy SDK (cut scope, not linked) | 33 | 33 |
| Other / unclassified | 29 | 30 |
| Defined only in a backend this configuration excludes (SDL2 / Cocoa) | 12 | 12 |
| Generated gitinfo (build-time, not a blocker) | 6 | 6 |
| Defined in a built translation unit behind a disabled `#if` | 10 | 5 |
| Direct3D 8 / DirectX (`D3DX*`) | 3 | 5 |
| Win32 API | 3 | 3 |
| COM / OLE (browser embedding, cut scope) | 3 | 3 |
| Engine C++ not built at this level | 0 | 1 |
| **Total** | **457** | **386** |

The 272 is gone, which was the deliverable. The total falls by 71 rather than by 272, and the two
categories that absorb the difference are both honest:

- **`Defined in a translation unit that failed to compile` 86 → 214.** The renderer's own symbols are
  now attributed to the 14 named level-4 files below instead of to "a layer we chose not to build".
  Same symbols, an owner each.
- **`Miles Sound System` 0 → 74.** `AIL_*`, referenced by the `WWAudio` units that now compile. This
  is *not* an unported surface: `Core/Libraries/Source/OpenALAudioDevice`, wired in as the
  `milesstub` target off 32-bit Windows, defines all 101 entry points `mss.h` declares
  (`audio-surface-scan.py --check` gates that count), and this harness simply does not link it.
  Linking it is a `cmake/native/CMakeLists.txt` change, i.e. an engine change, so it is deliberately
  not in this measurement PR; it should take the category to 0.

`Direct3D 8 / DirectX` 3 → 5 adds `D3DXGetFVFVertexSize` and `D3DXLoadSurfaceFromSurface`, both from
`WW3D2`, for the D3DX math slice.

`Engine C++ not built at this level` 0 → 1 is a real find that only a link could produce:
`ListenerHandleClass::Initialize(SoundBufferClass*)` is declared `override` in
`WWAudio/listenerhandle.h` and **defined nowhere in the tree** — `listenerhandle.cpp` compiles
cleanly and does not contain it. That is a missing definition in retail `WWAudio` rather than a port
defect; whether the Windows link tolerates it has not been checked here, and this harness's
`--whole-archive` is stricter than a normal link, so it is reported rather than diagnosed.

Nothing new appears in `Win32 API`, so `check-win32-undefined.py`'s budget is unchanged at the
three cursor calls, and it now gates the renderer libraries too.

### The 14 new compile failures, by cause

No level-1-3 failure disappeared or appeared: the 33 are the 19 above plus these 14.

| Cause | Units | Owner |
|---|---:|---|
| The Windows-only patch downloader (`HRESULT`, `KEY_READ` registry access, `Common/Debug.h`, an undeclared `sku`) | 4 — all of `WWDownload` | cut scope (online/patching), same cut as GameSpy |
| D3DX math not yet in the vendored surface (`D3DX_PI`, `D3DXMATRIX * D3DXMATRIX`) | 2 — `pointgr.cpp`, `sortingrenderer.cpp` | the D3DX math slice, which already owns `D3DXMatrixInverse` in `W3DWater.cpp`; since done, `docs/porting/d3dx8-math-seam.md` |
| COM browser embedding (`LPDISPATCH`) | 2 — `dx8webbrowser.cpp`, `dx8wrapper.cpp` | the COM browser excision slice, which owns the same failure in `W3DDisplay.cpp`/`W3DWebBrowser.cpp` |
| Harness headers with no stand-in (`windowsx.h`, `ddraw.h`) | 3 — `FramGrab.cpp`, `ww3d.cpp`, `ddsfile.cpp` | this harness (`scripts/native-port-shims/`), reachable only through the shimmed build |
| `_D3DADAPTER_IDENTIFIER8` has no `DriverVersion` in the vendored `d3d8.h` | 1 — `dx8caps.cpp` | the Win32-residue slice, which owns the identical failure in `W3DShaderManager.cpp` |
| A missing `<stddef.h>` for `size_t` in a file-local STL allocator | 1 — `w3d_dep.cpp` | unowned, one line, left for whoever holds `WW3D2` next |
| **A denominator defect, not a port failure** | 1 — `textdraw.cpp` | see below |

`textdraw.cpp` is commented out of `Core/Libraries/Source/WWVegas/WW3D2/CMakeLists.txt` as
`# textdraw.cpp # unused`, and its two diagnostics are a stale call to `Peek_Texture(ch)` against a
zero-argument `Peek_Texture()` — a source bug in dead code, which is why it is not in the Windows
build. It is measured only because `RENDERER_TARGETS` enumerate their sources by globbing
`source_dirs` while the level-1-3 targets read the CMake lists (`Target.cmake_lists`, honoured by
`native_probe_targets.target_sources`). Giving the renderer targets their `cmake_lists` would take
132 units to 131 and 14 failures to 13, and it changes no probe baseline because the probe does not
measure these targets — but `scripts/native-port-probe.py` is claimed by another in-flight slice, so
it is reported here rather than fixed.

### The one cause fixed here

Eight of the level-4 failures were a single cause, and it is a CRT-alias header rather than any
slice's implementation file: the W3D renderer calls the kernel32 string entry points fully qualified
(`::lstrcpy`, `::lstrcpyn`, `::lstrcat`, `::lstrcmpi`, `::_strdup`), which a macro alias cannot
satisfy — the name has to exist in the global namespace. `Dependencies/Utility/Utility/string_compat.h`
now defines them, `lstrcpyn` written out because its count includes the terminator. They are
spellings of the C library, not Win32 functionality, so nothing has to be implemented behind them
later, and Windows is unaffected: the header is only reachable through the `#ifndef _WIN32` branch of
`Utility/compat.h`.

Measured effect at level 4: 927 → 935 objects, 41 → 33 failures, 432 → 386 unresolved symbols
(`agg_def.cpp`, `rendobj.cpp`, `soundrobj.cpp`, `assetmgr.cpp`, `hlod.cpp`, `part_emt.cpp`,
`part_ldr.cpp` and `W3DAssetManager.cpp`).

### Since: the D3DX math entry points

The three D3DX units above are gone, and with them the five matrix symbols they left unresolved:
952/971 → 956/972 objects, 19 → 16 failures, 412 → 364 unresolved at level 4 (834/839 → 836/840, 5 →
4, 577 → 561 at levels 1-3). `Direct3D 8 / DirectX` 8 → 4, and `D3DXAssembleShader` appears in it for
the first time because `W3DWater.cpp` now compiles and its pixel-shader path calls it. The denominator
grows by one for `WWMath/tests/d3dx8math_test.cpp`, whose `main()` is removed from the archives like
the other standalone tests'. Full account, including why an object count is the *weaker* half of that
slice's evidence: `docs/porting/d3dx8-math-seam.md`.

### Is level 4 stable enough for CI

Yes, and it is wired: the `Native build + renderer` job. Two consecutive runs of
`--level 1 --level 2 --level 3 --level 4 --with-shims` produced byte-identical JSON, including the
per-symbol category lists, and every remaining failure is a named diagnostic rather than a resource
or ordering effect. It is a separate job rather than a second step in `native-build` because it
recompiles all 968 units and doubling that job's compile time would put it near its timeout, and it
has its own baseline file so the larger build cannot be read as progress against the smaller one.
Levels 1-3 keep their own ratchet.

## The audio backend, measured 2026-08-15 on b81226000 with clang 14 (current baseline)

Level 4's `Miles Sound System 0 → 74` entry above ended by predicting its own fix: *"this is not an
unported surface … this harness simply does not link it … it should take the category to 0."* It
does, and the prediction was right in full. By `b81226000` the category had grown to 89 as more of
`GameEngineDevice` and `WWAudio` compiled; building
`Core/Libraries/Source/OpenALAudioDevice` — the `milesstub` implementation `cmake/openal.cmake`
supplies to every audio consumer off 32-bit Windows — takes it to 0.

| | Levels 1-3 | Levels 1-4 |
|---|---:|---:|
| Undefined `AIL_*` before | 60 | 89 |
| Undefined `AIL_*` after | **0** | **0** |
| Unresolved symbols, all causes, before | 608 | 339 |
| Unresolved symbols, all causes, after | **548** | **250** |

The totals fall by exactly 60 and 89: no other category moves, no object count changes, and **no
`AIL_*` function was written or stubbed**. All 89 were already defined — the backend defines 101
entry points and `mss.h` declares 101, which `check-openal-symbols.py` has gated all along. This was
purely the harness excluding the layer that defines them, the same artefact level 4 removed for the
renderer, and it means the audio category never belonged in any "remaining port work" figure.

How it is built, and why that keeps the numbers comparable:

- A **support** archive (`libsupport_openalaudiodevice`), like lzhl: a dependency of the measured
  libraries rather than one of them, so it stays out of the objects and translation-unit
  denominators, which have to keep matching the probe's. 972 units and 968 objects at level 4, both
  unchanged.
- Its file list is read out of `Core/Libraries/Source/OpenALAudioDevice/CMakeLists.txt`, so the
  harness cannot drift from the real build. (That file's list is a flat `set()` of bare names, not
  the `Source/...` layout `probe.cmake_sources` parses, hence a four-line reader of its own.)
- OpenAL is linked from the system library, found by path — the runtime `libopenal.so.1` is enough,
  so a box without the `-dev` symlink can still link. `libopenal-dev` is installed in both CI jobs.
  If no OpenAL library is found the build says so and `al*`/`alc*` land in their own
  `OpenAL (not linked here)` category, which is the backend's own dependency and never an engine
  call site — the one thing that must not happen is those being counted as the engine's problem.
- `scripts/ci/fetch-probe-deps.sh` provisions `<AL/al.h>` from openal-soft at the tag
  `cmake/openal.cmake` pins (a blobless partial clone of `include/AL/*.h`), so the backend *compiles*
  on a box with no OpenAL installed at all, and the headers are pinned rather than whatever the
  distribution ships. System headers are the fallback, not the first choice, for the same
  reproducibility reason `review-and-decisions.md` §1.2 records.

Gated by `scripts/ci/check-audio-backend-linked.py` in both `native-build` jobs, asserting two things
the baseline ratchet cannot: that the backend archive is *in* the link (its absence would take the
total back up by 60/89 while every ratchet still held), and that no `AIL_*` is unresolved by name. The
second is deliberately a link-time check — `check-openal-symbols.py` compares declared against
defined inside the backend, `audio-surface-scan.py --check` compares demanded against declared by
source scan, and neither can see an entry point the engine references that `mss.h` never declares.

What this does *not* claim: nothing here plays a sound. It is the same compile-and-link statement as
every other figure in this document. `audio-device-seam.md` §7 lists what is still open behind the
API (MP2/MP3 streams, EFX, the Bink handoff), and none of it is visible to a linker.

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
- **`WW3D2` was not built until level 4.** Level 3 covers `GameEngineDevice` and `Main`; the renderer library
  itself needs the renderer resource seam first, which is where the 72 "layer not built here"
  symbols and `WorldHeightMap.cpp`'s `TheD3D8RenderBackend` failure lead. The 72 became 221, then
  272, for exactly that reason, and level 4 takes the category to 0.
- **Level 4 links no renderer, and runs nothing.** Every level-4 figure is "does this library
  produce objects, and what do those objects reference". `WW3D2` producing 66 objects says nothing
  about whether the D3D8-to-Vulkan backend draws a frame; that is `spikes/renderer` and
  `renderer-spike-verify`.
- **The compat-header change is guarded, and CI confirms it.** `string_compat.h` is reachable only
  through the `#ifndef _WIN32` branch of `Utility/compat.h`, so the Windows configurations cannot see
  the added names; all 13 `Build Generals`/`Build GeneralsMD` jobs are green on the branch that added
  them. Behaviour under those builds is unchanged by construction, not by replay evidence — the
  replay jobs need game-data credentials this fork does not have
  (`docs/porting/replay-check-gamedata.md`).
- **The remaining compile failures are reported, not fixed.** The Win32 surface (`HWND`/`HFONT`/
  `HRESULT`/`SetWindowText`), the GameSpy socket units and the Bink/FFmpeg/stb video devices belong
  to other slices or to cut scope. This build's job is to count them and name them.
  (The 18 `HFONT` ones are since gone: `docs/porting/gdi-font-seam.md`.)
- **The level-3 numbers are Linux only.** zlib is found by probing a list of platform library paths
  that includes `/usr/lib/libz.dylib`; that macOS entry has never been executed and is written blind.

## Reproducing

```sh
bash scripts/ci/fetch-probe-deps.sh
python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 --with-shims \
    --report docs/porting/native-build-report.md --json native-build.json
python3 scripts/ci/check-native-build-baseline.py --results native-build.json
```

Drop `--level 4` for the smaller build; the baseline the gate compares against is chosen from the
levels in the results, so the two cannot be confused with each other.

Add `--update` to the last command, in the PR that earns it, when the counts improve.

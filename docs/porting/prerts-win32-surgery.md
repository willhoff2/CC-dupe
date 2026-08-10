# `PreRTS.h` surgery: removing the shared Win32 header wall

Every GameEngine translation unit is compiled with a forced include of `PreRTS.h`. Until this
change that header pulled in `windows.h`, `atlbase.h`, `imagehlp.h`, `dinput.h` and around twenty
more Win32 headers, so **no** GameEngine translation unit could be compiled without a Windows SDK,
regardless of what the file itself used. `scripts/native-port-probe.py` measured this as 0/586.

This document records what was measured, what moved, and what was deliberately left alone.

## Method — measured, not inferred

1. Removed the Win32 block from both `PreRTS.h` copies (Generals and GeneralsMD).
2. Compiled every probe translation unit and recorded *every* `clang++` diagnostic with the file
   and line that produced it, rather than only a pass/fail count.
3. Attributed each diagnostic to the header or source file it came from, and each undeclared
   symbol to the shim header that declares it — the shim directory is a machine-readable index of
   exactly which Win32 header supplies which symbol, so the symbol → header mapping is derived
   rather than guessed.
4. Added the indicated include to that file, re-measured, and repeated until the diagnostics
   stopped moving.
5. Repeated the same loop against the real VC6 Windows build (`ninja -k 0`), because clang with
   shims and MSVC 6 with the real SDK do not agree on which header declares what (VC6's
   `shellapi.h`, `shlobj.h`, `snmp.h` and `imagehlp.h` require `windows.h` *first*, for example).

Two independent oracles were used on purpose. Symbols like `_spawnl`, `getcwd`, `_access`,
`timeGetTime` and `MultiByteToWideChar` were only surfaced by the Windows build; the native probe
never reaches them because it stops at the missing header.

## Result

### Native (no Windows SDK, no shims) — the headline number

Both columns were measured with `scripts/native-port-probe.py` against the same fetched
dependencies, `main` in a separate worktree versus this branch, so the two runs differ only by this
change.

| | Before | After |
|---|---:|---:|
| GameEngine TUs compiling natively | **0 / 586** | **374 / 586** |
| — `Core/GameEngine` | 0 / 207 | 107 / 207 |
| — `GeneralsMD/Code/GameEngine` | 0 / 379 | 267 / 379 |
| All probe targets | 115 / 737 | 489 / 737 |

Of the 212 GameEngine TUs that still fail natively, grouped by the first diagnostic:

- **102** stop at a `windows.h` that a file on their include path now asks for on purpose, plus
  5 × `io.h`, 4 × `winsock.h`, 3 × `objbase.h`, 1 × `atlbase.h`, 1 × `direct.h`, 1 × `process.h`.
  This is the real Win32 boundary: still large, but now attributable to a named file instead of
  being hidden behind one shared include.
- **82** fail inside the fetched DX8 SDK's `dinput.h` (`build/docker/_deps/dx8-src`), which is
  Windows-SDK-dependent vendor code, not engine code. These never had a chance of compiling.
- **9** fail on GameSpy's missing `gscommon.h` (the GameSpy SDK is not vendored).
- **4** are individual pre-existing items: `itoa`, `_fpreset`, `mbstring.h`, and a
  `DynamicMemoryAllocator` redefinition in `GameMemoryNull.cpp`.

An earlier custom measurement run reported 492 rather than 489. The difference was an artefact: a
scripted include-insertion pass had modified `build/docker/_deps/dx8-src/d3d8types.h`, which is
fetched dependency code and was reverted. The committed state measures 489; the probe's own output
is the number of record.

### Shimmed (Win32 headers declaration-shimmed) — engine C++ portability

| | Before | After |
|---|---:|---:|
| GameEngine TUs | 514 / 586 | 515 / 586 |
| All probe targets | 637 / 737 | 638 / 737 |

The shimmed number is the control, and the interesting thing about it is that it barely moved:
pushing an include down cannot make the engine's own C++ more or less portable, and it didn't. The
+1 comes from `<new.h>` → `<new>` in `GameMemory.h`. Had this number dropped, the surgery would
have broken something.

So the honest summary of the two columns is: the engine's own C++ was always ~88% portable, and
now 64% of it can be *seen* to be portable without any Win32 stand-ins at all.

> Measured with `clang++ -fsyntax-only`. This is blind to type widths, struct layout, calling
> conventions and anything that only shows up at link or run time. "Compiles" does not mean
> "correct" — the `sizeof(LANMessage)` static-assert failures in the shimmed run are exactly the
> kind of bug that syntax-only probing cannot rule out elsewhere.

### How many files needed a Windows include?

Three defensible counts, because the answer depends on what you count:

| Definition | Count |
|---|---:|
| Files carrying the `pushed down from PreRTS.h` marker added here | **39** |
| In-scope GameEngine files that include a Win32/MSVC-only header directly, including those that already did before this change | **49** |
| The same, collapsing the Generals/GeneralsMD duplicate copies of one logical source file | **37** |

The review predicted ~50, and that held — the count came in slightly under it on every definition.
It is under partly for a reason worth recording: several *headers* (`GameState.h`, `Recorder.h`,
`CriticalSection.h`, `ClientInstance.h`, `WorkerProcess.h`, `StackDump.h`) expose Win32 types in
their public interfaces, so one include there serves many translation units. That concentration is
also why 102 TUs still fail natively on `windows.h`: the number of files that need Win32 is small,
but their fan-out is not. Eliminating the remainder is a type-abstraction problem, not an include
problem — which is precisely the next slice.

### Renderer, audio, device and entry-point code — measured for the first time

The probe used to skip these 241 translation units entirely, which meant every portability figure
in this project excluded the code most likely to be unportable. `scripts/native-port-probe.py`
now takes `--include-renderer`, which adds `WW3D2`, `WWAudio`, `WWDownload`, the GeneralsMD
`WWVegas` copies, both `GameEngineDevice` libraries and `Main`. It is opt-in so that the headline
numbers above keep meaning the same thing over time. Both columns were measured with the *same*
(this branch's) probe script, run in a `main` worktree and here. The shimmed "before" column uses
`main`'s shim directory and the "after" column this branch's, since the shims changed too:

| | Before | After |
|---|---:|---:|
| Renderer/device/`Main` TUs, native | **47 / 241** | **53 / 241** |
| — `Core/.../WW3D2` | 30 / 73 | 30 / 73 |
| — `Core/.../WWAudio` | 0 / 19 | 0 / 19 |
| — `Core/.../WWDownload` | 0 / 4 | 0 / 4 |
| — `GeneralsMD/.../WWVegas` | 11 / 35 | 12 / 35 |
| — `Core/GameEngineDevice` | 1 / 70 | 6 / 70 |
| — `GeneralsMD/Code/GameEngineDevice` | 5 / 39 | 5 / 39 |
| — `GeneralsMD/Code/Main` | 0 / 1 | 0 / 1 |
| Renderer/device/`Main` TUs, shimmed | 121 / 241 | 137 / 241 |
| Every target, native | 162 / 978 | 542 / 978 |
| Every target, shimmed | 758 / 978 | 775 / 978 |

Reading this honestly: this change barely helps here, and that is expected — `WW3D2`/`WWAudio` do
not force-include `PreRTS.h` at all, so only the two `GameEngineDevice` libraries (which do) moved.
The number is worth having because it is the first measurement of this code off Windows, not
because it improved.

What the 188 native failures actually are, by first diagnostic: **82** stop in the fetched DX8
SDK (`objbase.h` via `d3d8.h`), **47** are `wcslen` used without `<wchar.h>` (the engine got it
from `windows.h`), **18** want `HANDLE`, **17** ask for `windows.h` directly, and the rest are
scattered (`HFONT`, `HKEY`, `LPDISPATCH`, `_strdup`, missing `bink.h`). Shimmed, 104 still fail,
and those are the interesting ones: the `wcslen`/`HANDLE`/`HFONT` cases are real portability work
in engine code, plus two `_D3DADAPTER_IDENTIFIER8.DriverVersion` uses that the shims cannot fake.

The probe's include paths for these targets mirror CMake: the Core renderer/device libraries are
`INTERFACE` libraries whose sources are compiled inside the game-specific targets, so they are
probed with the GeneralsMD include tree ahead of the Core one. Without that, 28 TUs fail on a
`w3d_file.h` that only exists in the game-specific copy — an artefact of the probe, not of the
code. Getting that wrong is exactly the sort of thing that produces a wrong number, so it is
recorded here.

## What moved

- Both `PreRTS.h` copies now include only standard C/C++ headers and project headers. A comment
  marks the removed block; nothing platform-specific should go back in, because everything in that
  file is a dependency of all 586 TUs.
- 39 files gained a local platform include, each marked
  `// TheSuperHackers @port Win32 header pushed down from PreRTS.h`. The include goes *after*
  `#include "PreRTS.h"`, since that must stay first in every GameEngine source file.
- `AsciiString.h` gained `<string.h>` for `_stricmp`, which it had been getting from `windows.h`.
  This one header is on nearly every TU's include path, so it accounts for a large share of the
  native improvement by itself.
- `WebBrowser.h` gained the MinGW `atl_compat.h` include that it previously inherited from
  `PreRTS.h`.
- `GameMemory.h`: `<new.h>` → `<new>`. MSVC's `<new.h>` exists only for `_set_new_handler`, which
  the engine does not use; the standard spelling works on both. This single line was blocking all
  586 TUs natively even after the Win32 block was gone.
- `Utility/string_compat.h`: added `_stricmp`/`_strnicmp`/`_strcmpi` aliases (the non-underscore
  spellings were already there). `AsciiString::compareNoCase` uses the underscore form and is
  included by essentially everything.
- `Utility/compat.h`: added `__min`/`__max`/`_isnan`/`_finite` aliases for non-Windows builds.
- `TransportContain.cpp` (both games): `if (hisLocomotor == FALSE)` → `== NULL`. The measurement
  shim defined `FALSE` as `0`, which hid a pointer-vs-`bool` comparison that `BaseTypeCore.h`'s
  `#define FALSE false` makes ill-formed. Not part of the surgery, but it is a one-token fix and
  the code plainly means "null".
- `scripts/native-port-probe.py` gained `--include-renderer` (7 new targets, 241 TUs, opt-in) and
  `miles-src/mss` on the fetched-dependency include path, so `WWAudio` fails on its own code rather
  than on a missing `mss.h`.

## Platform behaviour left for a later slice

These files now include a Win32 header locally, which keeps the Windows build identical but does
not make them portable. Each needs an abstraction, not an include, and none of that is in this
change:

| Area | Files | Needs |
|---|---|---|
| ~~High-resolution timing~~ | `FrameRateLimit.cpp`, `ProcessAnimateWindow.cpp`, `LANAPI.cpp` | **done** — [`timing-and-threading.md`](timing-and-threading.md). `LANAPI.cpp` still includes `<windows.h>`, for `GetUserNameA`/`GetComputerNameA`, i.e. the process/identity row |
| ~~Process / single-instance~~ | `ClientInstance.{h,cpp}`, `WorkerProcess.{h,cpp}`, `MainMenu.cpp` | **done** — see [`process-and-crash-seam.md`](process-and-crash-seam.md) |
| Registry | `registry.cpp` (both games) | `HKEY`/`RegQueryValueEx` → settings store |
| Filesystem / paths | `GameStateMap.cpp`, `GameState.cpp`, `Image.cpp`, `INIWebpageURL.cpp`, `MiniDumper.cpp`, `GlobalData.cpp`, `ReplayMenu.cpp` | `WIN32_FIND_DATA`, `_access`, `getcwd`, `SHGetKnownFolderPath`/`CSIDL_*` → path + directory API |
| Wall-clock structs in interfaces | `GameState.h`, `Recorder.h` | `SYSTEMTIME` in saved-game and replay records — also a **file-format** concern, not just an API one |
| ~~Threading primitives~~ | `CriticalSection.h` | **done** — [`timing-and-threading.md`](timing-and-threading.md). `CRITICAL_SECTION` is recursive, so it is `std::recursive_mutex`. This row's "very large fan-out" was wrong: 8 including files, no headers |
| ~~Crash reporting~~ | `StackDump.h`, `MiniDumper.cpp` | **done** (Windows-only by design, stub elsewhere) — see [`process-and-crash-seam.md`](process-and-crash-seam.md) |
| ~~Text encoding~~ | ~~`ThreadUtils.cpp`, `GlobalLanguage.cpp`~~ | done, see [sockets-and-text-encoding.md](sockets-and-text-encoding.md) |
| ~~Winsock~~ | ~~`udp.cpp`, `Transport.cpp`, `IPEnumeration.cpp`~~ | done, see [sockets-and-text-encoding.md](sockets-and-text-encoding.md) |
| SNMP / COM / ATL / DirectInput | `StagingRoomGameInfo.cpp`, `WebBrowser.h`, `FEBDispatch.h` | subsystem-level decisions, not header moves |

## Shim status

`scripts/native-port-shims/` stays a measurement instrument. This change **deleted** `new.h` and
removed the `_stricmp`, `_strnicmp`, `_strcmpi`, `_isnan`, `_finite`, `__min`, `__max` aliases from
its `windows.h`, because real compatibility headers now provide them.

Two shim files were **added**, and they are honest debt rather than progress: `stdlib.h` and
`stdio.h` stand-ins that `#include_next` the real header and add the MSVC CRT spellings `itoa`,
`_itoa`, `_ultoa`, `_snprintf`, `_vsnprintf`. Those spellings used to reach the engine through
`PreRTS.h`'s Windows headers; nothing in the repository provides them off Windows yet. They live in
the shim directory precisely so that they are counted as unfinished CRT-compatibility work instead
of quietly looking solved.

## Verification

- `./scripts/docker-build.sh --clean --game zh` — clean VC6 build, `Build completed`, and
  `build/docker/GeneralsMD/generalszh.exe` produced.
- All Generals targets (`g_generals`, `g_worldbuilder`, `g_guiedit`, `g_imagepacker`,
  `g_mapcachebuilder`, `g_w3dview`) also build, because half the pushed-down includes had to be
  mirrored into the Generals copies of shared sources.
- `python3 scripts/native-port-probe.py`, `--with-shims`, and both again with `--include-renderer`,
  for the numbers above. The probe itself gained the `--include-renderer` targets in this change;
  no other probe behaviour was touched, and the default target list is unchanged, so the headline
  before/after numbers are comparable.

One known warning-hygiene regression, MinGW-only: `PreRTS.h` used to end with a
`#pragma GCC diagnostic pop` balancing the push inside `atl_compat.h`. Both moved out together, so
in a MinGW build the ATL warning suppressions from `atl_compat.h` now stay in effect to the end of
any TU that includes `WebBrowser.h` or `FEBDispatch.h` instead of being popped. `atl_compat.h`
already exposes `ATL_COMPAT_RESTORE_WARNINGS()` for this; wiring it up belongs with whoever owns
the MinGW build.

Not verified: the `win32` (modern MSVC) and `*-debug` / `*-profile` CI presets were not built
locally, and the MinGW-w64 configuration was not built at all. Debug presets enable `RTS_DEBUG`
code paths that this build does not compile, so they are a plausible source of further missing
includes; CI is the check for that.

## Reproducing the attribution

The probe reports pass/fail counts. The per-diagnostic attribution used here came from running the
probe's own job list and keeping every diagnostic with its originating file, then grouping. That
tooling is not committed: it is a few dozen lines around `scripts/native-port-probe.py`'s
`collect_jobs()`, and the useful output — which file needs which header — is now recorded in the
source tree as the includes themselves.

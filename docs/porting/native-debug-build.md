# The native debug configuration: making the engine's own assertions run off Windows

Wave 8, slice 2. Everything below was measured on this tree with `clang++-14` on Linux x86-64, at
64-bit, levels 1-4, shimmed. Numbers in this file are re-measurable with the commands quoted next to
them; the authoritative copies live in `docs/porting/ci-baselines/*.json`.

## 0. Why the slice exists

`DEBUG_ASSERTCRASH` expands to `((void)0)` unless `DEBUG_CRASHING` is defined, and `Debug.h` derives
that from `RTS_DEBUG`. Until this slice, `RTS_DEBUG` did not compile off Windows, so **no native
figure this project has ever published came from a build in which a single one of the engine's
assertions could fire**. `headless-simulation-probe.md` §3-§4 is what that costs: a retail map was
parsed, reported as loaded, and simulated for 13,500 frames with zero of its objects in the world and
an unchanging frame CRC, while the assertion written for exactly that condition sat two lines away,
compiled out.

## 1. Measurement before any fix

The configuration is `-DRTS_DEBUG -DWWDEBUG -DDEBUG`, which is what `cmake/config-build.cmake`
defines for `RTS_BUILD_OPTION_DEBUG=ON`; `scripts/native-build.py --config debug` now sets the same
three, so the probe cannot drift from the CMake presets.

```
CLANGXX=clang++-14 python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 \
    --with-shims --config debug --strict-link --build-dir build/native-debug
objects 967/978, compile failures 11, undefined symbols 114, binary produced: no
```

The narrative figure this slice was scoped from (10 TUs, 113 symbols) was measured before #96/#98/#99/
#100; on this tree it is **11 and 114**. Release on the same tree, for comparison: 978/978, 0
unresolved, binary produced.

| Failing translation unit | First diagnostic |
|---|---|
| `Core/GameEngine/.../Common/CRCDebug.cpp` | use of undeclared identifier `CreateDirectory` |
| `Core/GameEngine/.../System/Debug.cpp` | no member named `DebugBreak` in the global namespace |
| `Core/GameEngine/.../System/SubsystemInterface.cpp` | no matching function for call to `GetPrecisionTimerTicksPerSec` |
| `GeneralsMD/.../Common/GameEngine.cpp` | no matching function for call to `GetPrecisionTimerTicksPerSec` |
| `GeneralsMD/.../GameLogic/GameLogic.cpp` | no matching function for call to `GetPrecisionTimerTicksPerSec` |
| `GeneralsMD/.../W3DDevice/GameClient/W3DAssetManager.cpp` | no matching function for call to `GetPrecisionTimer` |
| `Core/.../GameSpy/Thread/GameResultsThread.cpp` | use of undeclared identifier `WSABASEERR` |
| `Core/.../GameNetwork/LANAPI.cpp` | use of undeclared identifier `GetUserNameA` |
| `GeneralsMD/.../Common/Recorder.cpp` | use of undeclared identifier `MAX_COMPUTERNAME_LENGTH` |
| `Core/GameEngineDevice/.../StdLocalFileSystem.cpp` | subscript of pointer to incomplete type / `const Char*` used as a struct |
| `Core/Libraries/.../WWDebug/wwdebug.cpp` | use of undeclared identifier `Is_Trying_To_Exit` |

Unresolved symbols, in the categories `scripts/native-build.py` already assigns:

| Category | Count |
|---|---|
| compile-blocked | 113 |
| no-definition-anywhere | 1 (`GlobalSize`) |
| library-not-linked | 0 |
| cut-scope-not-linked | 0 |
| harness-artefact | 0 |

So 113 of the 114 are consequences of the 11 translation units above (`DebugCrash`, `DebugLog`,
`TheGameLogic`, `TheGameEngine`, `addCRCDebugLine`, the `SubsystemInterface` vtable, …) and exactly
one is a genuinely absent platform symbol. That is the shape the slice was scoped against: no engine
logic, no new subsystem, six debug-only Win32 spellings and one heap call.

## 2. Measurement after

```
objects 978/978, compile failures 0, undefined symbols 0
strict link succeeded: 0 unresolved symbol(s), binary produced: yes
build/native-debug/native_strict_link: 87.9 MiB, ELF 64-bit x86-64
```

Release, re-measured on the same tree after these changes, unmoved: 978/978, 0 unresolved, binary
produced. Baseline: `ci-baselines/native-build-shimmed-debug-level1-2-3-4.json`, its own file because
the two configurations compile different code — `check-native-build-baseline.py` refuses to compare a
debug measurement with a release baseline rather than silently ratcheting against the wrong one.

The seams, each under the existing spelling so no consumer changed:

| Gap | Where it was closed |
|---|---|
| `GetPrecisionTimer`/`GetPrecisionTimerTicksPerSec` taking `Int64*` where the debug callers pass `__int64*` | `BaseTypeCore.h`: off MSVC, `Int64` is now `long long`, the same type `__int64` maps to, so the two spellings are one type at LP64 |
| `CreateDirectory`, `DeleteFile` undeclared in `CRCDebug.cpp` | include `<windows.h>`, which off Windows is the port's own compatibility header |
| `GetUserNameA`, `GetComputerNameA`, `MAX_COMPUTERNAME_LENGTH` | declared in the shim header, implemented in `platform_win32_kernel.cpp` over `gethostname`/`getpwuid` with Win32 error codes |
| `unsigned long` buffer length passed to `GetComputerNameA` | `Recorder.cpp` uses `DWORD`; `unsigned long` is 64-bit at LP64 and `LPDWORD` is not |
| `WSABASEERR` and the rest of a debug-only WinSock result switch | portable values in `socket_compat.h`. GameSpy stays cut: this makes its *logging* compile, nothing else |
| `Is_Trying_To_Exit` declared only under `_WIN32` | declaration moved out, non-Windows definition added next to the Windows one |
| `GlobalSize` | `platform_win32_module.cpp`, over `malloc_size`/`malloc_usable_size`, beside the existing `GlobalAlloc`/`GlobalFree` |
| debug log statement indexing a `const Char*` as a struct | the call site was simply wrong, and nothing had ever compiled it |

## 3. What an assertion does off Windows now

The Windows path is a modal dialog with Abort/Retry/Ignore. Off Windows there is no dialog, and the
portable stub previously answered it, which would have made a compiled-in assertion indistinguishable
from a compiled-out one. Off Windows an assertion now:

- prints `ASSERTION FAILURE:` with the **file and line** of the `DEBUG_ASSERTCRASH`/`DEBUG_CRASH`
  site, the **condition text** when the macro carries one, and the message, on **stderr**, whether or
  not the debug log was ever initialised;
- prints a **stack dump** through the existing `StackDump.cpp` seam, with frames demangled via
  `abi::__cxa_demangle`; Unix debug builds link `-rdynamic` so exported frames have names at all;
- **exits non-zero** and does not continue, unless `-ignoreAsserts` is passed, which is the documented
  way to get the Windows "Ignore" behaviour without a dialog.

Windows behaviour is untouched: the file/line/condition prefix is `#ifndef _WIN32`, because on Windows
the dialog is the report and its text is behaviour the Windows build owns.

`DEBUG_CRASH`/`DEBUG_ASSERTCRASH` now record `__FILE__`, `__LINE__` and the stringised condition in
`TheCurrentCrash*` globals next to the existing `TheCurrentIgnoreCrashPtr`, so no call site changed.

## 4. Negative control: a known-bad input tripping a real assertion

`scripts/ci/check-assert-fires.py` (in CI, in the debug job) generates a map whose chunk table of
contents does not name the chunk ID the file then uses, runs the headless harness over it in a
scratch directory, and requires a non-zero exit, `ASSERTION FAILURE` on stderr, the engine's own
source file and message, a stack dump, and at least one *named* frame:

```
ASSERTION FAILURE: .../GeneralsMD/Code/GameEngine/Source/Common/System/DataChunk.cpp:477: name not found in DataChunkTableOfContents::getName for id 7
Stack Dump:
  .../sim_probe(DataChunkTableOfContents::getName(unsigned int)+0xd0)
  .../sim_probe(DataChunkInput::openDataChunk(unsigned short*)+0x16d)
  ...
[Abort: assertion failed off Windows, where there is no assert dialog to ignore it. Pass -ignoreAsserts to continue instead.]
exit status: 1
```

That assertion is the engine's, unmodified. The bad input is generated, so the gate needs no retail
data and runs in CI.

## 5. The harness re-run under the debug build, against retail data

`scripts/native-sim-probe.py --build-dir build/native-debug` builds the harness from the debug build's
own compile database, so the harness inherits `-DRTS_DEBUG` and its assertions are live. Against the
retail Zero Hour install and the retail map and replay used by `headless-simulation-probe.md`:

| Harness mode | Result under the debug build | Class |
|---|---|---|
| `chunks` on a retail map | 8 top-level chunks including `ObjectsList` (60,623 bytes); no assertion fires | passes; #88's fix confirmed with assertions live |
| `filecrc` / `xfercrc` on the same map | `6314F5FD` and `2465AFE3`, stable over 3 runs, identical to the release-configuration values | passes; the debug configuration does not perturb the CRCs |
| `mapcache` over a directory of retail maps | **`MapUtil.cpp:536` "Couldn't find \ in map name!"**, loudly, then exit 1 | **port defect, now audible** — blocker 5 of `headless-simulation-probe.md`, which previously reported `maps=0` with no error at all |
| `replayhdr` on a retail `.rep` | SIGSEGV in `GameSlot::setState` (`GameInfo.cpp:239`, `TheGameText->fetch`) via `RecorderClass::init`; **no** assertion precedes it | harness limitation (blocker 10): the harness never creates `TheGameText`, and the deref is not guarded by an assertion, so the debug configuration adds nothing here |

Two findings, both stated at full strength:

**Finding 1 — a port defect this slice introduced and fixed here, because nothing else could run
until it was.** `MemoryPool::createBlob` asserts that a blob's block *count* is a multiple of
`MEM_BOUND_ALIGNMENT`. An earlier slice raised `MEM_BOUND_ALIGNMENT` from 4 to 16 at 64-bit for a
real reason (the pool allocator must satisfy `alignof(max_align_t)`), which silently tightened this
unrelated *count* check, and the retail pool table has `{ "NameKeyBucketPool", 9000, 1024 }` — legal
at 4, illegal at 16. Every debug run therefore aborted inside the first `NameKeyGenerator` lookup,
before reaching any engine code worth measuring. The count is not an alignment: a blob is
`allocationCount` blocks of `m_allocationSize` bytes and `m_allocationSize` is already rounded up to
the bound, so every block is aligned whatever the count. The check now uses its own
`MEM_POOL_COUNT_GRANULARITY` of 4 — the value the retail tables were authored against, and the value
`GameMemoryInit.cpp` still rounds counts to. The predicate on 32-bit Windows is unchanged (4 either
way), and the assertion is not weakened: it still rejects zero, negative and off-granularity counts.

**Finding 2 — for a later slice, not fixed here.** `MapUtil.cpp:536` requires a backslash in an
enumerated map path. The engine's own file enumeration returns forward slashes off Windows, so the
assertion is about a Windows-only assumption rather than about corrupt data. It is a real port defect
in the filesystem seam and it is blocker 5 already; what this slice changes is that it is no longer
silent — the same code path previously produced an empty map cache and reported success. Fixing it
belongs with the rest of the path-separator seam, not here.

## 6. What was not measured

- **Apple Silicon.** All of the above is Linux x86-64. The assertion path uses `backtrace()`/
  `backtrace_symbols_fd()` and `abi::__cxa_demangle`, all of which exist on macOS, and the existing
  `debug-profile-seam-macos` job already proves the stack walk there — but the debug configuration
  itself has not been built on arm64.
- **The full replay simulation under the debug build.** §4 of `headless-simulation-probe.md` was
  produced by the game binary under gdb with the CRC mismatch suppressed, which is a measurement
  device rather than something to put in CI; this slice re-ran the *harness*, not that. Whether the
  13,500-frame run now trips assertions is the obvious next measurement and is not answered here.
- **Any assertion beyond the ones reached.** Three code paths were exercised. The claim proved is
  "assertions compile, fire, report and stop the process", not "the engine is assertion-clean".

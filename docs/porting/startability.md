# What "startable" means, and the state of each condition now that a binary exists and runs

This document was written for a world in which the strict link produced no file, and its whole content
was a checklist of what would have to be true. **A file now exists, and it has been run on the target.**
So the checklist is a status table, and the interesting part has moved from "what is missing" to "how
far the process gets and where exactly it stops".

## 0. Where it gets to, at a glance

One row per claim, each with the platform it was measured on. "Apple Silicon" is
[`first-native-run-arm64.md`](first-native-run-arm64.md) — M1 Pro, macOS 26.6.1, Apple clang 21,
against a complete retail Zero Hour install; "Linux" is the clang-14 ratchet described from §1 down.
Where the two disagree about the platform, the hardware run is the one measured on the target.

| Claim | Status | Measured on | Named evidence |
|---|---|---|---|
| A binary exists | **yes** | Linux and Apple Silicon | `strict_link.binary_produced`; 80.7 MiB ELF x86-64 PIE here, 26.6 MiB thin Mach-O `arm64` there |
| It is genuinely arm64, not x86-64 under Rosetta | **yes** | Apple Silicon | `lipo -archs` → `arm64`, `sysctl.proc_translated` → `0` (`first-native-run-arm64.md` §5) |
| The process executes | **yes** | Linux and Apple Silicon | it loads, runs, and fails in the game's own code rather than the loader's |
| It runs against retail data | **yes** | Apple Silicon | 20 `.big` archives opened, a 928 KB `.csf` member decompressed and read |
| It reaches the engine's own initialisation | **yes**, `GameEngine::init()` | Apple Silicon | a Cocoa window is created and 2104 thing templates are built from INI inside it |
| **It stops here** | `DX8Wrapper::Init` dereferences a null `DX8Wrapper::RenderBackend` | Apple Silicon | `first-native-run-arm64.md` §4. `RenderBackend` is `nullptr` off Windows: there is no device to create. Slice B's territory, deliberately not fixed here |
| One defect passes quietly before that | `GameTextManager::init()` desyncs the `.csf` table on `sizeof(WideChar)` | Apple Silicon | `first-native-run-arm64.md` §3, `widechar-fallout.md`. Slice A's territory, deliberately not fixed here |
| It stops earlier without retail data | `INI::loadFileDirectory("Data\INI\Default\GameData")` throws; `GameEngine::init()` calls `ReleaseCrash` | Linux — CI has no install | §4 |
| The main menu renders | **no** | — | needs the renderer row above; no frame has been drawn |
| Every measured translation unit compiles | **yes** on Linux, 977/977 | Linux; 976/977 on Apple Silicon | the one Darwin holdout, `WWLib/regexpr.cpp`, is ported off glibc's GNU regex to POSIX `<regex.h>` in the slice that wrote this row and has **not** been recompiled on a Mac: [`regexpr-posix-port.md`](regexpr-posix-port.md) |
| The entry point is the game's own | **yes** | Linux; unverified on macOS | `link_entry_point_stub` false with `game_entry_target_built` true. A missing entry point now fails the run instead of being replaced by a generated stub, and the Mach-O spelling (`_main`) is measured rather than hardcoded — `native-build.md` §"What the harness had to learn about Mach-O" |

Measured with `clang++-14` on Ubuntu 22.04, levels 1-4 shimmed:

```sh
./scripts/ci/fetch-probe-deps.sh
CLANGXX=clang++-14 python3 scripts/native-build.py \
  --level 1 --level 2 --level 3 --level 4 --with-shims --strict-link \
  --json native-build-level4.json
```

Every figure below comes from that run and from
`docs/porting/ci-baselines/native-build-shimmed-level1-2-3-4.json`, which the command regenerates.
None of it is quoted from prose.

## 1. The headline, and its exact limits

"Before" is the branch point, main at `51322470b` — the window/GDI and D3DX entry-point slice (#85)
as merged, whose baseline is the last one measured without a binary.

| | before this slice | now |
|---|---|---|
| objects | 976/976 | **977/977**, 0 compile failures |
| strict link (`--strict-link`, no `--warn-unresolved-symbols`) | fails | **succeeds** |
| unresolved symbols | 68 | **0** |
| `strict_link.binary_produced` | `false` | **`true`** |
| the file | none | `build/native/native_strict_link`, **80.7 MiB, ELF 64-bit x86-64, `ET_DYN`** (PIE) |
| `strict_link.agrees_with_nm` | true | true — the linker's verdict and the `nm` scan of the archives agree, at 0. It was **false** on Darwin, where Mach-O's underscore prefix made the two lists incomparable; the fix is one measured symbol namespace rather than a relaxed check, and is itself unverified on a Mac |

The object count rose by one because the tree now compiles a file it did not have:
`d3dx8texcreate.cpp` (§2). The selected window backend adds an object to `WWLib`'s archive without
changing the measured denominator, since the harness counts the sources it is given. The piles
`library-not-linked` (was 54), `harness-artefact` (was 9), `no-definition-anywhere` (was 5),
`cut-scope-not-linked` and `compile-blocked` are all **0**.

**Nothing was stubbed to get there.** That is a checkable claim, not a promise: every symbol was
resolved either by putting a real library on the link line, by building an in-tree file that already
had the definition, by running the build step the harness was skipping, or by writing an
implementation with a behaviour test. The one thing that is *deliberately unimplemented* —
`D3DXCreateTextureFromFileExA` — prints a line naming itself and returns `E_NOTIMPL` rather than
pretending to succeed; it is a refusal, not a stub, and §4 says what it costs.

## 2. How each of the 68 was resolved

| Was | Symbols | Resolved by | Not a stub because |
|---|---:|---|---|
| SDL2/Cocoa window seam | 24 | `scripts/native-build.py` selects a backend: `platform_window_sdl2.cpp` off Apple, `platform_window_cocoa.mm` on macOS, matching `CORE_WWLIB_WINDOW_BACKEND` in the real build. `libSDL2` is linked by path. | the definitions were already in the tree; the harness built neither. Decision 6 of `decisions-resolved.md`. |
| FFmpeg `av_*`/`sws_*`/`swr_*` | 29 | `fetch-probe-deps.sh` builds the shared libraries at the tag `vcpkg-lock.json` pins (n7.1.1) and the link uses them. | it is the upstream library, at the version the headers came from. Decision 7. |
| `gitinfo` | 6 | the harness runs `resources/gitinfo/git_watcher.cmake`, the real generator, into `build/native/generated/support_gitinfo/gitinfo.cpp`. | the values are this checkout's, produced by the same code the CMake build uses — not hand-written. |
| `FillStackAddresses`, `StackDumpFromAddresses`, `g_LastErrorDump` | 3 | the probe defines `IG_DEBUG_STACKTRACE`, which selects the `backtrace()`/`backtrace_symbols()` implementation #47 landed. | the implementation walks a real stack; the symbols were behind an `#if` this configuration was not taking. |
| `MSS_auto_cleanup` | 1 | an include-order fix: the fetched Miles SDK `mss.h` was shadowing the OpenAL backend's own header, so the backend's definition was never compiled. | it was a **measurement bug**. The definition existed the whole time. |
| D3DX texture creation | 5 | `d3dx8texcreate.cpp`: `D3DXCreateTexture`, `D3DXCreateCubeTexture`, `D3DXCreateVolumeTexture` fit the request to `D3DCAPS8` and call the device; `D3DXGetErrorStringA` returns the D3D8 error names; `D3DXCreateTextureFromFileExA` loudly refuses. | 74 assertions in `d3dx8texcreate_test.cpp` over the fitting rules, and the refusal is explicit and audible. `d3dx8-texture-seam.md` §4. |
| `ff_aom_uninit_film_grain_params` | (appeared, then gone) | the reduced FFmpeg configuration was pulling H.264/AAC code whose own dependency was disabled; the configuration now enables Bink/`.binka`/PCM and the demuxers the game needs. | it was FFmpeg's internal symbol, not the game's. Not resolvable by any port work — a configuration error, found by `nm -D` on the built library. |

## 3. Definition: startable, with real answers

**Startable** = the strict link produces an executable, and running it against an installed Zero Hour
reaches the main menu's first rendered frame without an assert or a fatal error.

The first half is now true. The second is not, and this table is the point of the document:

| Condition | Status | Evidence |
|---|---|---|
| Strict link produces a binary | **yes** | `strict_link.binary_produced` true; 80.7 MiB ELF 64-bit x86-64 PIE |
| Zero unresolved symbols, agreed by two methods | **yes** | linker exit 0 with no tolerance flag; `agrees_with_nm` true |
| Every measured translation unit compiles | **yes** | 977/977, 0 failures |
| Nothing stubbed to achieve it | **yes**, and auditable | §2, per-symbol |
| A window backend is selected | **yes** | SDL2 here, Cocoa on macOS (decision 6). The Cocoa one creates a real, visible window in the real game process on hardware: `first-native-run-arm64.md` §2, §5 |
| `gitinfo` generated by the real generator | **yes** | `git_watcher.cmake` runs in the harness |
| The process starts | **yes** — it runs, initialises, and fails in the game's own code | §4 |
| A renderer device exists | **no** | `dx8wrapper.cpp` compiles and links; nothing has created a D3D8 device. The Vulkan/MoltenVK backend is the renderer slices' work and is not in this binary's path. |
| An audio device exists | **partly** | the OpenAL `AIL_*` implementation is linked (#64); no device has been opened and no sound played |
| Video decodes | **no** | FFmpeg is linked; no `.bik` has been decoded by this binary |
| Retail `Data/` available to the process | **no** on CI, and out of scope there — but **yes**, measured | §4 is where it stops *here*; on the Mac with the retail install the same code opens 20 archives and reads a 928 KB member out of one (`first-native-run-arm64.md` §2) |
| The main menu renders | **no** | requires the two rows above plus data. Measured on Apple Silicon against retail data: `first-native-run-arm64.md` §4 — `DX8Wrapper::RenderBackend` is null off Windows, so the process dereferences null in `DX8Wrapper::Init` before any frame |
| Runtime behaviour (layouts, `Xfer` blobs, wide chars, endianness) | measured separately, and **not** by linking | `native-layout-test.py`, `xfer-blob-audit.py` |

## 4. What the binary does when you run it

This is the part worth more than the symbol count it replaces. Run on the measuring box, with the
pinned FFmpeg on the loader path and no retail data present:

```sh
LD_LIBRARY_PATH=build/docker/_deps/ffmpeg-lib/lib ./build/native/native_strict_link
```

It loads, runs, and reaches the engine's own initialisation. It then prints:

```text
!!! MESSAGE BOX (no native dialog; answering "OK")
!!! Technical Difficulties...
!!! You have encountered a serious error.  Serious errors can be caused by many things including
!!! viruses, overheated hardware and hardware that does not meet the minimum specifications for the
!!! game. ...
```

Under `gdb`, with a catchpoint on `throw`, the first exception is thrown in:

```text
INI::loadFileDirectory(fileDirName="Data\INI\Default\GameData", loadType=INI_LOAD_OVERWRITE,
                       subdirs=true)
```

and is caught by `GameEngine::init()`'s handler, which calls
`ReleaseCrash("Uncaught Exception during initialization.")`.

**That is the expected and correct failure.** `GameEngine::init()` loads and CRCs ~25 `Data/INI/...`
directories through `TheFileSystem` and instantiates every store from them; there is no fallback
dataset, and this box has no Zero Hour install. The message is the game's own retail
technical-difficulties path, reached through the game's own `main()` (`PlatformMain.cpp` anchors the
link, and the 5 test-tool entry points are dropped from the archives only *after* that real one is
found). The harness generates no stub `main()` when `GeneralsMD/Code/Main` is in the build: if the
entry point cannot be found there, the run fails and says so, because a link anchored by
`int main() { return 0; }` would report a binary that starts nothing.

So, precisely:

* **Proved:** the whole tree compiles and links at 64-bit off Windows into a loadable native
  executable, the loader accepts it, it executes, and it gets as far as asking the filesystem for
  game data.
* **Not proved by this Linux run:** that it is a *game*. It has not opened a window, created a
  rendering device, played a sound, decoded a video, or loaded a single INI. A linked binary is not a
  running game and must not be described as one anywhere in this repository. The window and the INI
  loading are proven on Apple Silicon against retail data (§0); the rendering device is not proven
  anywhere.
* **Also not proved *by this file*:** anything about Apple Silicon. This file is x86-64 ELF, built on
  Linux. The macOS runners compile the Cocoa backend; producing and running an `arm64` Mach-O against
  a retail install has since been done on real hardware, and
  `first-native-run-arm64.md` is the result — including the build differences macOS needed (its §1),
  which are the measure of the gap between what CI proves and what the target requires.

## 5. What CI now asserts, and why it inverted

Until this slice, `scripts/ci/check-native-build-baseline.py` ratcheted a *count*: the number of
unresolved symbols, and each pile, must not grow. That is the right gate while no binary exists and
the wrong one afterwards — once a file is produced, "73 became 74" is not the regression anyone cares
about; "there is no file" is.

So the gate inverts, and the inversion is data-driven rather than a flag day: when the **baseline**
records `strict_link.binary_produced` true, `check_binary_gate()` activates and requires

1. the strict link is clean, with **0** unresolved symbols (a "clean" flag with a non-empty list
   fails, because the two came from different places),
2. an executable is produced **and described** — `binary_produced` true with no description of the
   file is a failure, not a pass,
3. the file is **64-bit**, and its format and machine match the baseline's,

and it says so as a broken build rather than as a bigger number. Before a baseline has a binary, the
old count ratchet still applies unchanged, so levels 1-3 (which do not link an entry point) keep
being measured the way they always were. Binary *size* is reported and deliberately not gated: it
moves with the compiler and the debug level, and gating it would produce failures with no defect
behind them.

Both halves of that logic — the pile attribution and the binary gate — are pinned by
`scripts/native-build-categorise-test.py` in the cheap CI tier, because both were wrong in ways no
compile and no link could reveal. The level 1-4 job additionally runs `ls -l` and `file` on the
produced executable into the step summary, so the artefact's identity is visible in the run rather
than asserted in a document.

## 6. What the first *launch* now requires

Everything on this list used to be preceded by "and a binary". That part is done.

1. A rendering device: the D3D8-shaped Vulkan/MoltenVK backend the renderer slices are building,
   wired to `dx8wrapper.cpp`'s device creation instead of the D3DX file loader's refusal. This is now
   the *observed* stopping point on hardware and not a prediction: `RenderBackend` is `nullptr` off
   Windows and `DX8Wrapper::Init` dereferences it.
1. A 16-bit `WideChar`: the `.csf` string table does not survive a 4-byte `wchar_t`, measured rather
   than inferred (`first-native-run-arm64.md` §3, `widechar-fallout.md`). The engine runs on without
   it — `GameTextManager::init()` fails silently — so this one will not announce itself later.
2. Retail data reachable off Windows: `review-and-decisions.md` §2.6's three sub-decisions
   (discovery, path separators and case, packaging) — the failure in §4 is *this* item, and it is a
   decision, not a bug.
3. ~~An `arm64` Mach-O produced by the same harness on macOS, with the Cocoa backend selected~~ —
   **done**, 976/977 objects and a clean strict link, 26.6 MiB, thin arm64, not Rosetta. The one
   macOS-only compile failure (`WWLib/regexpr.cpp`, GNU-only `reg_syntax_t`) has since been ported to
   POSIX `<regex.h>` rather than excluded or `#ifdef`-ed away — `regexpr-posix-port.md` records the
   decision, the three syntax differences it accepts, and the fact that the compile is expected to
   reach 977/977 but has **not** been observed on a Mac. The advisory macOS gate (decision 5) still
   measures this without blocking. Reproducing the macOS build is `native-build.md`
   §"Building it on macOS (Apple Silicon)".
4. ~~A run against an install~~ — **done**, and the prediction in this sentence held: the failures are
   now behavioural. The first one is a layout/width defect of exactly the kind
   `native-layout-test.py` and `xfer-blob-audit.py` were proxies for, caught this time by the retail
   data rather than by a proxy. Windows remains the oracle for every one of them.

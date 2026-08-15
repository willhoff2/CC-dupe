# What "startable" means, and what stands between 250 symbols and a running process

> **Superseded in part.** Every total below predates the slice that made `dx8wrapper.cpp` compile.
> The current figures are 969/972 objects, 3 compile failures, 173 strict-link unresolved symbols,
> `compile-blocked` 18 and `no-definition-anywhere` 22; see
> `docs/porting/dx8wrapper-native-compile.md` and the level 1-4 baseline JSON. The definitions of
> "startable" and of the five piles below are unaffected.

Measured at `219d9130b` with `clang++-14` on Ubuntu 22.04, levels 1-4 shimmed — i.e. after #64 put
the OpenAL backend on the link line, which is why every total here is 89 lower than this slice's
first measurement without a line of port work being done.

```sh
./scripts/ci/fetch-probe-deps.sh
CLANGXX=clang++-14 python3 scripts/native-build.py \
  --level 1 --level 2 --level 3 --level 4 --with-shims --strict-link \
  --json native-build-level4.json
```

Every number below comes from that run and from
`docs/porting/ci-baselines/native-build-shimmed-level1-2-3-4.json`, which the command regenerates.
None of it is quoted from prose.

## 1. The claim this document exists to kill

Levels 1-4 produce **968/972 object files**, and the link is anchored by
`GeneralsMD/Code/Main`'s own `main()` (`PlatformMain.cpp`) rather than by a stub the harness writes.
That link also **produces a file**, and the file means nothing: it is linked with
`-Wl,--warn-unresolved-symbols`, so 250 unresolved symbols are warnings and the linker exits 0. The
loader would reject the result immediately.

`--strict-link` drops the tolerance and asks the linker for a verdict:

| | tolerant link (`link_probe`) | strict link (`--strict-link`) |
|---|---|---|
| flags | `-Wl,--warn-unresolved-symbols` | none |
| unresolved symbols | 250, reported as warnings | 250, reported as errors |
| exit status | 0 | non-zero |
| file produced | yes | **no** |

The two lists are identical, which is the useful part: the strict link's 250 and the `nm` scan's 250
agree symbol for symbol (`strict_link.agrees_with_nm`), so the categorised list is exactly the list
standing between this build and an executable. Nothing is stubbed to obtain that, and nothing may
be — a green strict link bought with stubs would hide precisely the work these numbers count.

## 2. The 250, by what would resolve each one

The existing categories say what a symbol *is* (`Win32 API`, `FFmpeg`, `Miles Sound System`). They do
not say what makes it go away, and conflating the two is what made the pre-level-4 figure
misleading: 272 of the symbols levels 1-3 could not resolve were only "this build does not compile
the renderer", and building the layer removed them without a line of port work. Each symbol is
therefore also assigned to exactly one **pile**:

| Pile | Symbols | What resolves it | Owner |
|---|---:|---|---|
| `library-not-linked` | 42 | Adding a library to the link line. | see §3 |
| `cut-scope-not-linked` | 82 | Excising the call sites of a cut feature. | `online/absent-menu-seam` |
| `compile-blocked` | 108 | Making an in-tree translation unit compile; the definition already exists. | the slice that owns the file |
| `harness-artefact` | 9 | A build step this harness does not run, or an `#if` this configuration does not take. | this slice |
| `no-definition-anywhere` | **9** | Writing code. **This is the remaining port work.** | see §5 |

Of the 108 `compile-blocked`, roughly 90 are `dx8wrapper.cpp`'s (`DX8Wrapper::*`, matched by scanning
the four failed files for each name) and the remaining ~18 are the three GameSpy units, which are cut
scope anyway. `dx8wrapper.cpp` is therefore the only non-cut compile failure left in the level 1-4
build, and it is the largest single lever on this list after the audio link.

Only the last row is port work. It is the number to ratchet, and
`scripts/ci/check-native-build-baseline.py` now fails if it grows, independently of the total —
otherwise adding FFmpeg to the link would let real port work grow behind a falling headline.

The pile assignment is evidence-based, not a keyword list: for each provider the script scans the
sources or headers `scripts/ci/fetch-probe-deps.sh` provisions and matches the symbols they define.
It deliberately does **not** decide the pile from libraries installed on the measuring machine (this
box has FFmpeg, OpenAL and SDL2; the CI container has none), because a split that changed with the
box would not be a measurement. Where a real library is present it is used as a cross-check and
reported in `providers.*.system_libraries`: all 29 FFmpeg symbols are exported by this box's
`libavcodec.so.58` and friends.

## 3. `library-not-linked`: 42 symbols, and why each library is absent

| Library | Symbols | Why it is not linked here | Slice that will remove them |
|---|---:|---|---|
| Miles `AIL_*` API | 1 | Was 90. #64 builds `Core/Libraries/Source/OpenALAudioDevice` as a support archive and links `libopenal`, so 89 of the 90 resolved with no `AIL_*` written or stubbed. What remains is `MSS_auto_cleanup`, which that backend does not implement. | `platform/audio-device` |
| FFmpeg | 29 | The video path is the engine's own `RTS_BUILD_OPTION_FFMPEG` route (`cmake/config-build.cmake`, default `OFF`). This harness compiles source lists rather than honouring that option, so `VideoDevice/FFmpeg/*.cpp` is built with the pinned headers `fetch-probe-deps.sh` provisions, while nothing installs an FFmpeg runtime to link against. | `video/bink-excision-and-harness-headers` |
| SDL2 / Cocoa window backend | 12 | `probe.OPTIONAL_BACKENDS` keeps `platform_window_sdl2.cpp` opt-in, and `platform_window_cocoa.mm` is Objective-C++ so no target lists it. Both definitions are in the tree. | `platform/macos-window-compile`, `platform/window-seam-wiring` |
| zlib, LZHL | 0 | Already linked: `libz` from the system and the provisioned LZHL sources as a support archive. Listed because their absence used to be counted. | — |

These 42 are the same class of artefact level 4 removed for the renderer, and the drop from 131
demonstrates it: the strict link must be given the same libraries as the tolerant one (`libopenal`
included) or it would report 90 symbols as unresolved that the engine can already resolve, which
would read as port work. They are not a smaller
number of "real" problems; they are a link line and a decision. The decision matters, which is §4.

`cut-scope-not-linked` is 82 GameSpy SDK symbols. The SDK's own sources define all of them, so this
is a link *refused*, not one that is missing: online matchmaking is cut scope, and the symbols
disappear when `online/absent-menu-seam` removes the call sites. Linking the SDK to make a binary
appear would be the worst available outcome.

## 4. What an executable needs beyond symbol resolution

Symbol resolution is necessary and nowhere near sufficient. In order:

1. **An entry point — done.** `GeneralsMD/Code/Main`'s `main()` anchors the link; the harness's stub
   `main()` is unused, and the three standalone test-tool `main()` objects inside the measured
   directories are removed from the archives first so the game's entry point is unique
   (`link_dropped_entry_points`).
2. **A window/input backend, and a renderer device — chosen, not merely excluded.** The 12 backend
   symbols are unresolved because this configuration picks *no* backend. A real executable must pick
   one: SDL2 on Linux, Cocoa on macOS. That is a decision with consequences (event loop, input
   mapping, HiDPI), not a link flag, which is why the pile is "not linked" rather than "missing".
   The same is true of the renderer: `dx8wrapper.cpp` is one of the 4 translation units that do not
   compile (its first diagnostic here is `_D3DADAPTER_IDENTIFIER8::DriverVersion`), so even with
   every symbol resolved this build contains no display device. (#65's browser
   excision made `W3DDisplay.cpp` itself compile, which is why its `IsIconic` call now shows up as an
   unresolved symbol rather than hiding behind a compile failure — the shape of the list changes as
   files start compiling, which is the point of splitting the piles.)
3. **Generated `gitinfo` — 6 symbols, build-time.** `resources/CMakeLists.txt` configures
   `gitinfo/gitinfo.cpp.in` into the build tree via `git_watcher.cmake`, defining `GitRevision`,
   `GitShortSHA1`, `GitTag`, `GitCommitTimeStamp`, `GitCommitAuthorName` and
   `GitUncommittedChanges`. This harness compiles source lists rather than running the real CMake
   project, so it does not generate the file. A real build has these; measuring their absence
   measures the harness.
4. **The `#if`-disabled definitions — 3 of the 4.** `FillStackAddresses`,
   `StackDumpFromAddresses` and `g_LastErrorDump` are defined in translation units that *did* build,
   inside `#if` blocks this configuration does not take (the Win32 debug/crash path). The fourth,
   `getQR2HostingStatus`, is defined by the GameSpy SDK and therefore counted as cut scope. Each is
   a build-configuration fact: either the platform equivalent is written (crash dumps are
   `platform/process-and-crash-seam`) or the call sites go.
5. **Retail data.** Even a fully linked binary cannot start without the retail install.
   `GameEngine::init()` loads `Data\INI\Default\GameData` and ~25 further `Data\INI\...`
   directories through `TheFileSystem`, CRCs them, and instantiates every store
   (`TheThingFactory`, `TheWeaponStore`, `TheArmorStore`, `TheMetaMap`, …) from them. There is no
   built-in fallback dataset: without `Data/` and the `.big` archives from an installed Zero Hour,
   startup fails in `initSubsystem` long before a window appears. The port cannot ship data and does
   not try to; the launch test is "point it at an install". How the data is *located* off Windows is
   an open decision, not a solved one — `review-and-decisions.md` §2.6 has the three sub-decisions
   (discovery, path separators and case, packaging) and the measured Windows-shaped path handling
   they have to survive.
6. **Runtime behaviour, which no link proves.** Structure layouts, `Xfer` blob sizes, wide-character
   handling and endianness are measured separately (`native-layout-test.py`, `xfer-blob-audit.py`)
   because a 64-bit build that links can still read its own save files wrongly.

## 5. Definition: startable

**Startable** = the strict link produces an executable, and running it against an installed Zero
Hour reaches the main menu's first rendered frame without an assert or a fatal error.

That decomposes into checkable conditions, none of which is met today:

| Condition | Status |
|---|---|
| Strict link produces a binary | **no** — 250 unresolved symbols |
| `no-definition-anywhere` is empty | **no** — 9 symbols |
| Every measured translation unit compiles | **no** — 4 failures, 3 of them cut-scope GameSpy units |
| A window backend is selected | **no** — the configuration picks none |
| A renderer device exists | **no** — `dx8wrapper.cpp` does not compile (`DriverVersion`) |
| An audio device exists | **partly** — the OpenAL `AIL_*` implementation is linked (#64), but nothing has opened a device or played a sound |
| `gitinfo` is generated | **no** — the harness does not run the CMake step |
| Retail `Data/` available to the process | out of scope for CI; required for any launch |

The 9 `no-definition-anywhere` symbols, with the object file that needs each:

| Symbol | Needed by | Note |
|---|---|---|
| `D3DXFilterTexture`, `D3DXLoadSurfaceFromSurface`, `D3DXGetFVFVertexSize`, `D3DXAssembleShader` | `W3DTreeBuffer.cpp.o`, `missingtexture.cpp.o`, `dx8fvf.cpp.o`, `W3DWater.cpp.o` | what is left of the D3DX surface after #66 landed the matrix family: texture/surface helpers and the shader assembler, none of which is matrix maths |
| `GetCursorPos`, `ScreenToClient`, `SetCursor` | `W3DMouse.cpp.o` | the mouse-cursor Win32 path; pinned by name in `win32-undefined-budget.json` |
| `IsIconic` | `W3DDisplay.cpp.o` | the window-minimised query, unresolved only since #65 made that unit compile; the window seam owns it |
| `ListenerHandleClass::Initialize(SoundBufferClass*)` | `listenerhandle.cpp.o` | declared in WWAudio and defined nowhere in the tree, upstream included |

So the honest headline is: **9 symbols of real port work — 4 D3DX texture/shader helpers, 4
cursor/window calls the window seam owns, and one WWAudio member function** — on top of
`dx8wrapper.cpp`, a backend that must be chosen, and a data dependency CI will never satisfy. The 241
others are a link line, a cut feature, or this harness.

## 6. What the first launch attempt would actually require

1. `platform/audio-device` has landed (#64): the Miles category is 0, the total 339 → 250, and the
   prediction it made — that this was a link line and not unported code — held exactly.
   `online/absent-menu-seam` removes the GameSpy call sites, which is 82 symbols plus ~18 of the 108
   `compile-blocked` ones; finishing `dx8wrapper.cpp` removes the other ~90. Those plus the 9 harness
   symbols account for 200 of the 250, leaving FFmpeg's 29, the backend's 12 and the 9 that are port
   work.
2. The harness gains a real link configuration rather than a measurement one: an SDL2 backend
   selected, `gitinfo.cpp` generated, `libavcodec` either linked or the video path compiled out.
3. `--strict-link` goes green, at which point the ratchet flips from "the count must not grow" to
   "the link must not break".
4. Only then is a launch meaningful: run the binary with `Data/` and the `.big` files from a Zero
   Hour install, and the first failure will be a runtime assert, not a symbol. That is the point at
   which `native-layout-test.py` and `xfer-blob-audit.py` stop being proxies.

Steps 1-3 are measurable today and gated in CI. Step 4 needs a machine with retail data, which is
why "startable" is defined above in terms a CI job can check plus one condition it explicitly
cannot.

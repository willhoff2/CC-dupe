# The first native arm64 run against a retail install

Every earlier number in `docs/porting/` counted what did not compile or did not link. This document is
the first record of the binary *running*: an `arm64` Mach-O built on an Apple Silicon Mac, executed
against a complete retail Zero Hour installation. It stops where the binary stopped, and it names two
failures rather than one, because the first one is silent.

Nothing was stubbed, hardcoded or short-circuited to get further, and neither failure was fixed here.

## 0. The machine, the binary, and the proof it is not Rosetta

| Thing | Value | How it was read |
|---|---|---|
| OS | macOS 26.6.1, build 25G76 | `sw_vers` |
| Host | `arm64-apple-darwin25.6.0` | the harness's `host` field |
| Compiler | Apple clang 21.0.0 (clang-2100.1.1.101) | the harness's `compiler` field |
| GPU | Apple M1 Pro | `MTLCreateSystemDefaultDevice()` in the spike |
| Retail data | a complete Zero Hour install, read-only, outside the repo | run from a directory of symlinks to it |

```
$ file build/native-macos-arm64-slice5/native_strict_link
Mach-O 64-bit executable arm64
$ lipo -archs build/native-macos-arm64-slice5/native_strict_link
arm64
$ sysctl sysctl.proc_translated
sysctl.proc_translated: 0
```

`lldb` reports the launched process as `(arm64)`. No Rosetta, no `x86_64` slice — the file is thin
arm64, so there is nothing for Rosetta to translate.

The build, from `scripts/native-build.py --level 1..4 --with-shims --strict-link`:

| | Linux x86-64 (#86) | this machine |
|---|---|---|
| translation units | 977 | 977 |
| objects | 977 | **976** — `WWLib/regexpr.cpp` fails, §1 (since ported to POSIX regex; unverified here) |
| strict link | clean | **clean** (`strict_link.clean` true, 0 unresolved) |
| the file | 80.7 MiB ELF x86-64 PIE | **26.6 MiB Mach-O 64-bit executable arm64** |
| entry point | `libgeneralsmd_code_main`, no stub `main` | same, after the Mach-O `_main` fix (§1) |
| window backend | `platform_window_sdl2.cpp` | **`platform_window_cocoa.mm`** |
| `strict_link.agrees_with_nm` | true | **false**, and that is a harness bug, §1 (since fixed; unverified here) |

The size difference is the toolchain's, not the tree's: different compiler, different default debug
level, `ld64` versus GNU `ld`. It is reported and not gated, for exactly that reason.

## 1. What had to change to build it here, and what each one costs

Each of these is a gap between what CI proves on Linux and what the target needs.

| Change | Why macOS needed it | Cost / risk |
|---|---|---|
| `ZLIB_CANDIDATES`, `OPENAL_CANDIDATES` in `scripts/native-build.py` gained Homebrew keg paths | macOS ships zlib only inside the dyld shared cache — there is no `/usr/lib/libz.dylib` to put on a link line — and no system OpenAL dylib the harness can find | harness only; the two libraries are the same upstream libraries CI links |
| `MAIN_SYMBOL = "_main"` on Darwin, in the entry-point scan | Mach-O `nm` prefixes symbols with `_`, so the scan for `main` found none: the harness both generated an unnecessary stub `main` **and** kept the 5 standalone test-tool `main()`s, and `ld64` failed with `17 duplicate symbols` | harness only. On Linux the symbol is still `main`, so the Linux result is unchanged. **Superseded**: the prefix is measured rather than hardcoded per platform, and a build containing `GeneralsMD/Code/Main` that finds no entry point now fails instead of generating a stub — see the follow-up note below |
| `StdLocalFileSystem.cpp`: `for (const auto& p : path)` | libc++'s `std::filesystem::path::iterator::reference` is `path` *by value*; libstdc++'s is `const path&`. A mutable reference binds on Linux and not on macOS | engine source, but a `const`-correctness fix with no behaviour change on any platform |
| `scripts/native-port-shims/malloc.h` (new) | `<malloc.h>` is glibc's, not standard. The engine includes it for `malloc`/`free`/`alloca`, which BSD puts in `<stdlib.h>`/`<alloca.h>` | shim only, and it `#include_next`es a real `<malloc.h>` where one exists, so Linux is untouched |
| `stringex.h`: `wcslcpy`/`wcslcat` declarations guarded on Apple/BSD | the platform declares both with C linkage; the tree declared inline C++ ones, so clang rejected the redeclaration (`different language linkage`) | engine header, guarded by platform macros. The Windows and Linux paths compile the same code as before |
| `regexpr.cpp` is **not** worked around | it uses GNU `regex`'s `reg_syntax_t`/`re_syntax_options`, which BSD `<regex.h>` does not have | it is the one macOS-only compile failure, 976/977. Nothing on the path to the running game references it; it is not stubbed and not excluded from the count |

> **Closed out since, on Linux.** The `regexpr.cpp` row is now a POSIX `<regex.h>` port with the
> decision, the three accepted syntax differences and the `re_match` anchoring recorded in
> [`regexpr-posix-port.md`](regexpr-posix-port.md), and the vendored `gnu_regex.h` shim is deleted. The
> other five rows landed here; how to reproduce all of it on a Mac, including the `lipo -archs` and
> `sysctl.proc_translated` checks below, is now `native-build.md` §"Building it on macOS (Apple
> Silicon)" rather than only this document. **Nothing in that follow-up has been compiled on a Mac**;
> it is measured on Linux and unit-tested.

Two harness measurement defects surfaced and are recorded rather than silently tolerated:

* `strict_link.agrees_with_nm` is **false** here while the link is clean at 0 unresolved. The
  discount list of libc/libstdc++/CRT symbols is ELF-shaped, so Mach-O's underscore-prefixed
  `_malloc`, `___cxa_throw`, `__NSGetArgv` are counted as unresolved by the `nm` scan and not by the
  linker. Same class of bug as the one `check-openal-symbols.py` had. The linker's verdict is the
  authoritative one and it is 0.

  **Fixed since, and not by relaxing it.** Both defects were one root cause: the harness compared
  symbol names from four sources that do not spell them the same way. Names now enter a single
  prefix-free namespace whose prefix is **measured** by compiling a one-line probe (not switched on
  `sys.platform`), removing exactly one prefix so `__ZN...` and `___cxa_throw` survive; the discount
  list is read from the SDK's `.tbd` stubs and `dyld_info -exports` on Darwin, and an empty discount
  list now fails the run instead of reporting every libc symbol as port work. `native-build.md`
  §"What the harness had to learn about Mach-O" is the detail. Unverified on a Mac.
* `check-spike-render.py` cannot see `DYLD_LIBRARY_PATH`: it runs the spike through
  `subprocess.run`, and every `python3` on this machine is reached through a SIP-protected binary,
  which strips `DYLD_*` from the environment it passes on. Homebrew's validation-layer manifest names
  its dylib relatively, so the layer could not be loaded and `vkCreateInstance` returned
  `VK_ERROR_LAYER_NOT_PRESENT`. Worked around **outside** the repo by pointing `VK_LAYER_PATH` at a
  copy of the manifest with an absolute `library_path`; §4 is measured that way. A macOS invocation
  of that gate needs this or an equivalent, or it fails for a reason unrelated to the renderer.

## 2. How far it gets

In order, all observed under `lldb` on the binary above, against the retail install:

| Question the slice asked | Answer | Evidence |
|---|---|---|
| Does it reach `GameEngine::init()`? | **yes** | it fails *inside* it, backtrace in §3 |
| Does a window appear? | **yes**, one Cocoa window | in-process `NSApp.windows` count 1, `visible` yes, `CAMetalLayer`, `backingScaleFactor` 2.00, `drawableSize` 1600x1200 for an 800x600 window |
| Does `TheFileSystem` find and open the `.big` archives? | **yes**, 20 archives | `TheArchiveFileSystem->m_archiveFileMap` size 20; `doesFileExist("data\english\Generals.csf", 0)` returns 1 through the archive system, and the file's 928,775 bytes are decompressed into a `RAMFile` and read |
| Does INI parsing complete? | **it gets far enough to build the object stores**, and no INI failure was observed | `TheThingFactory`'s template hash map holds **2104** entries, first template `MiGFirestorm`; `TheWritableGlobalData` holds parsed values: 800x600, windowed false, FPS limit 30 |
| Does a frame render? | **no** | §3: there is no render backend to render one |
| First failure | **two**, §3 and §4 | |

The `Data\INI\...` load that ends the Linux run in #86 is therefore *passed* here: that failure was
the absence of retail data, exactly as it was described, and with data present the INI path proceeds.

## 3. Failure one: the `.csf` string table desyncs on `sizeof(WideChar)` — a port defect

This is the **first** genuine failure in execution order and the more interesting of the two, because
nothing reports it. `GameTextManager::init()` has no failure return: it calls `deinit()` and leaves
the manager empty, so the game runs on with every localized string missing.

Observed live, stopped at `GameText.cpp:910` (`goto quit` on a non-`CSF_LABEL` id) in
`GameTextManager::parseCSF`:

```
filename    = "data\english\Generals.csf"
header      = (id ' FSC', version 3, num_labels 6422, num_strings 6421, skip 0, langid 0)
listCount   = 1              // the first label parsed; the second did not
id          = 1919252833  -> the bytes "ayer"
len         = 12
file->position() = 111
sizeof(WideChar) = 4
```

The first 128 bytes of the file, as the archive system delivered them:

```
off 24   ' LBL'  num_strings=1  len=15  "GUI:GameOptions"
off 51   ' RTS'  len=12         24 bytes = 12 x 2-byte inverted UTF-16LE units, ending at off 82
off 83   ' LBL'  <- where the next record actually starts
off 95           "GUI:SinglePlayer"
```

`parseCSF` reads the string body as

```cpp
file->read ( m_tbuffer, len*sizeof(WideChar) );
```

`WideChar` is `typedef wchar_t` (`Core/Libraries/Include/Lib/BaseType.h:41`), which is 2 bytes with
MSVC and **4 bytes with Apple clang**. So it consumed `12 * 4 = 48` bytes where 24 are on disk and
left the read head at offset 107 instead of 83 — `len * 2` bytes too far. The next id read landed
inside the *text* of the second label, at `"GUI:SinglePlayer"[12..15]` = `"ayer"`, which is not
`CSF_LABEL`, so the parse quit.

Confirmed from the other side in a second run: `m_initialized` 0, `m_textCount` 0,
`m_stringInfo` null after `init()`, while calling `getCSFInfo()` by hand on the same path returns 1
and sets `m_textCount` to 6422. The header path works; only the body parse fails.

The only user-visible trace is the fallback string, which reached the window title:

```
Community Patch ~ ***FATAL*** String Manager failed to initialize properly ...
```

**Classification: port defect.** Not the data path — the archive lookup, the decompression, the
header and the first label are all correct on this machine. `docs/porting/widechar-fallout.md`
predicted this exact site by inspection ("`.csf` string table reads, `len*sizeof(WideChar)` straight
into the buffer"); this is the first time it has been *seen* happening. The fix is that document's
`WideChar` → 16-bit conversion, which it measures at ~340 files, and it is not this slice's.

## 4. Failure two: the process dies on a null render backend — an unimplemented path

The hard crash, and the end of the run:

```
stop reason = EXC_BAD_ACCESS (code=1, address=0x0)
frame #0: DX8Wrapper::Init(hwnd=0x..., lite=false) at dx8wrapper.cpp:307:23
frame #1: WW3D::Init at ww3d.cpp:278
frame #2: W3DDisplay::init at W3DDisplay.cpp:821
frame #3: GameClient::init at GameClient.cpp:336
frame #4: W3DGameClient::init at W3DGameClient.cpp:83
frame #5: SubsystemInterfaceList::initSubsystem at SubsystemInterface.cpp:157
frame #6: void initSubsystem<GameClient> at GameEngine.cpp:173
frame #7: GameEngine::init at GameEngine.cpp:624
frame #8: Win32GameEngine::init at Win32GameEngine.cpp:96
frame #9: GameMain() at GameMain.cpp:46
frame #10: main at PlatformMain.cpp:137
frame #11: dyld`start
```

`dx8wrapper.cpp:307` is `if (!RenderBackend->Open())`, and `RenderBackend` is null by construction
off Windows:

```cpp
#ifdef _WIN32
RenderBackendClass *DX8Wrapper::RenderBackend = &TheD3D8RenderBackend;
#else
RenderBackendClass *DX8Wrapper::RenderBackend = nullptr;
#endif
```

**Classification: genuine unimplemented path.** The `RenderBackendClass` seam exists, D3D8 is its one
implementation, and the Vulkan/MoltenVK implementation is the renderer slices' work. The right
failure for a missing backend is arguably a named refusal rather than a null dereference, but making
it one is a change, and this slice reports.

So the honest boundary is: **the engine initialises, finds and reads retail assets, builds its object
stores from INI, and creates a Cocoa window; it has no renderer, so no frame exists.**

> **This failure has since been implemented away, off Windows.** `VulkanRenderBackendClass` is now the
> non-Windows `RenderBackendClass`, so `DX8Wrapper::Init()` and the device enumeration and creation
> all succeed; on Linux/lavapipe the engine reaches `Do_Onetime_Device_Dependent_Inits()` and stops on
> a null texture from the D3DX creation entry points instead. `docs/porting/renderer-integration.md`
> §4 is that measurement. Whether the Mac reaches the same wall is unmeasured — re-running this
> hardware record is the way to find out.

## 5. What this run says about points, pixels and Retina

`decisions-resolved.md` puts the renderer in pixels and the engine, UI and mouse in points, converting
at the renderer boundary only. On a scale-1 CI runner a half-resolution render cannot fail. Measured
here, on the real 2x panel, in the *game* process:

| | value |
|---|---|
| `backingScaleFactor` / `NSScreen` scale | **2.00** |
| `GlobalData` resolution, from INI | 800x600 (points, what the engine asked for) |
| `CAMetalLayer.contentsScale` | 2.00 |
| `CAMetalLayer.drawableSize` | **1600x1200** (pixels) |
| layer class | `CAMetalLayer` |

and in the window-metrics tool on this branch, which drives the same seam through resize and
fullscreen (`build/spike/zh-macos-window-metrics`, 68 events, `checks failed: 0`):

| phase | points | backing pixels | `Window_Client_Size()` | `vkSurfaceCapabilities.currentExtent` |
|---|---|---|---|---|
| created | 800x600 | 1600x1200 | 800x600 | 1600x1200 |
| after `Window_Set_Mode(1024x768)` | 1024x768 | 2048x1536 | 1024x768 | 2048x1536 |
| borderless fullscreen | 1728x1117 | 3456x2234 | 1728x1117 | 3456x2234 |
| back to windowed | 800x600 | 1600x1200 | 800x600 | 1600x1200 |

The ratio is 2.00 x 2.00 in every phase, the swapchain extent tracks the pixel size, and
`Window_Client_Size()` — what the engine and the mouse use — stays in points. That is the decision
holding, measured on hardware that can break it.

**What this does *not* prove, and cannot yet:** that a click at the bottom-right of an 800x600 window
hits the bottom-right of the UI. There is no UI. That check needs a rendered frame, so it needs a
render backend, and it stays on the list until there is one.

Fullscreen, same run: `styleMask` 0x0, window level 25, `NSApp.presentationOptions` 0xA
(`HideDock|HideMenuBar`), menu bar reported not visible, content 1728x1117 points covering the whole
screen including the notch strip (`auxiliaryTopLeftArea` 771x32, screen `safeAreaInsets.top` 32,
window `safeAreaInsets` all 0). Control Center's own windows sit at level 25 too and remain in front —
the game window does not cover them. The Window Server's list, not the process's belief, is the source
for all of that.

## 6. MoltenVK on real Metal, on this branch

Re-run here rather than quoted, with the validation layer loaded (`VK_LOADER_DEBUG=layer` shows the
insert; the environment fix is in §1):

```
device: Apple M1 Pro          driver: MoltenVK 1.4.2, driverVersion 0.2.2210, apiVersion 1.1.357
zh-renderer-spike        centre pixel rgba = 131,152,170,255, validation messages: 0
check-spike-render.py    0/480000 pixels differ by more than 8/255; worst channel delta 1 at (400,76)
  re-run with --tolerance 2 --max-differing 0   passes
zh-metal-surface-probe   vkCreateMetalSurfaceEXT, swapchain, acquire and present all VK_SUCCESS
zh-feature-probe         validation messages: 0, 0 case(s) failed
zh-fixedfunc-tests --validation        validation messages: 0, 0 failed, 6 pending
zh-resource-lock-tests --validation    validation messages: 0, 0 failed, 0 skipped
  both again with ZH_SPIKE_NO_VIEW_SWIZZLE=1    validation messages: 0, 0 failed
check-d3d8-surface.py / check-backend-coverage.py   both OK
```

This is the *spike* on real Metal, not the engine: the engine's renderer is the null pointer in §4.

## 7. What is proven on hardware, and what is still only CI's word

| Claim | Status |
|---|---|
| A native `arm64` Mach-O of the whole tree exists and runs | **proven on hardware** (§0) |
| Not Rosetta, no x86_64 slice | **proven** (`lipo -archs`, `proc_translated` 0) |
| The Cocoa window backend creates a real window in the real game process | **proven** (§2) |
| Retail `.big` archives are found, opened and read through the #45 seam | **proven** — 20 archives, a 928 KB member decompressed and parsed (§2, §3) |
| INI data reaches the object stores | **proven** — 2104 thing templates, `GlobalData` populated (§2) |
| `backingScaleFactor == 2` handled end to end, points vs pixels | **proven** for the window/renderer boundary (§5) |
| Borderless fullscreen, menu bar, notch | **proven** as measured; Control Center stays in front (§5) |
| MoltenVK matches the lavapipe reference on a real GPU, validation silent | **proven** on this branch (§6) |
| A frame renders | **not proven, and cannot be** — no render backend exists off Windows (§4) |
| Mouse points map to UI hit-testing correctly | **not proven** — needs a UI (§5) |
| Physical keyboard and mouse translation | **partly**: §8 |
| The `WideChar` width is safe on disk | **disproven** on hardware (§3) |
| Anything about gameplay, audio output, video, performance | **not proven** — nothing played a sound, decoded a video, or timed a frame |

## 8. Physical input, and the honest limit on it

The window-metrics run in §5 was made **without** `--inject`, so the test process posted no
`CGEvent`s: every one of its 68 events came from outside the process, delivered by the Window Server
while the window had focus, and included real key-down/key-up pairs (set-1 scan code `0x0E`, text
`U+007F`) and a long continuous stream of `mouse-move` events with client coordinates in points. The
translation path from AppKit to the engine's scan codes therefore ran on events this process did not
create, and reported `checks failed: 0`.

What stops that from being a clean "physical input verified": the harness does not record each event's
`CGEventSourceStateID`, which is what actually distinguishes HID hardware from another process's
synthetic events. The machine's owner was using it during the run, so the natural reading is a real
keyboard and trackpad — but it is an inference, not a measurement, and it is written down as one.
Recording the source state ID in the metrics tool would turn it into one, and is a small follow-up.

Not attempted: non-US keyboard layouts, dead keys, IME, an external mouse, cursor clipping semantics.
No Accessibility grant was requested for synthetic injection, and no Full Disk Access grant was
requested or needed — the retail install was read through the one directory that was already readable.

## 9. No screenshots, and why

There are none, and the reason is not permissions this time. Whole-display capture works (Screen
Recording is granted), but a capture of the *desktop* is a photograph of the machine owner's private
screen and cannot go in a repository; the two taken while checking were deleted. A capture of the
game window alone is what would be publishable, and there is nothing to capture: the window never
receives content, because §4 means nothing ever presents to its `CAMetalLayer`, and the Window Server
does not list a window that has never been drawn into. The substitutes are the in-process AppKit and
Core Animation numbers in §5 and the GPU read-backs in §6, which are pixels that came off the Metal
driver.

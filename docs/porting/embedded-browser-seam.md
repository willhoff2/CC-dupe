# The embedded browser seam (and the first real game-target link)

`GeneralsMD/Code/Main` had never produced an object file. One cause was left, and it was cut scope:
the embedded Internet Explorer control the WOL/online screens host. Three translation units failed
on it, and one of them was `PlatformMain.cpp`, the game's entry point:

```
GeneralsMD/Code/Main/PlatformMain.cpp
  error: allocating an object of abstract class type 'CComObject<W3DWebBrowser>'
GeneralsMD/Code/GameEngineDevice/Source/Win32Device/Common/Win32GameEngine.cpp
  error: allocating an object of abstract class type 'CComObject<W3DWebBrowser>'
GeneralsMD/Code/GameEngineDevice/Source/W3DDevice/GameClient/W3DWebBrowser.cpp
  error: 'IDispatch' does not refer to a value
```

Online play is out of scope for this port (`docs/porting/native-port-plan.md`), so the control does
not need to work. It needed to stop being a compile blocker off Windows without changing Windows at
all. This slice does that, and the consequence is the point of the exercise: with the entry point
compiling, the link is no longer anchored by a stub `main()` written by the measurement harness —
it is a link of the real game target, and every unresolved symbol in it can be attributed.

## 1. Measured

Linux/x86-64, clang 14, `scripts/native-build.py --level 1 --level 2 --level 3 --with-shims`; the
figures are the committed baseline (`docs/porting/ci-baselines/native-build-shimmed-level1-2-3.json`)
and the report (`docs/porting/native-build-report.md`). Nothing here was measured on macOS or arm64.

| | before (`main`, ac520adf8) | after |
|---|---:|---:|
| `GeneralsMD/Code/Main` objects | **0 / 1** | **1 / 1** |
| Archives produced | 9 | **10** |
| Libraries with no archive at all | `GeneralsMD/Code/Main` | **none** |
| Link entry point | this script's stub `main()` | **`PlatformMain.cpp`'s `main()`** |
| Object files, all libraries | 816 / 836 | **819 / 836** |
| Translation units that fail to compile | 20 | **17** |
| Undefined symbols, total | 457 | **467** |
| Undefined `COM / OLE (browser embedding, cut scope)` | 3 | **0** |
| Undefined `Other / unclassified` | 29 | **0** |

Three compile failures fixed, three objects gained (`PlatformMain.cpp`, `Win32GameEngine.cpp`,
`W3DWebBrowser.cpp` — the last now compiles to an empty object off Windows).

### Why the unresolved total went *up*

+10, and it is the expected direction: unresolved symbols are counted from what the linked objects
*reference*, so compiling three more translation units adds their references. The categories say
exactly where the movement is:

| Category | before | after |
|---|---:|---:|
| Defined in a translation unit that failed to compile | 86 | **100** |
| COM / OLE (browser embedding, cut scope) | 3 | **0** |
| Defined in a built translation unit behind a disabled `#if` | 10 | **9** |
| Other / unclassified | 29 | 0 |
| FFmpeg (not linked here) | – | 29 |

The +14 is `PlatformMain.cpp` and `Win32GameEngine.cpp` now referencing the online-menu and
renderer symbols whose own translation units still fail (`MainMenu.cpp`, `DownloadMenu.cpp`,
`W3DWater.cpp`, …) — a dependency that was always there and was previously invisible because the
referencing objects did not exist. The −3 is the browser's own GUIDs leaving the link. The last two
rows are the same 29 FFmpeg symbols, renamed: they were the whole of `Other / unclassified`, and
now have a category (§5).

## 2. What Windows does, unchanged

`WebBrowser` is a COM object: `FEBDispatch<WebBrowser, IBrowserDispatch, &IID_IBrowserDispatch>`
plus `SubsystemInterface`, hosted by an ATL `CComModule` that `GameEngine`'s constructor initialises
and its destructor terminates, with a global `OLEInitializer` calling `OleInitialize` before either.
`W3DWebBrowser` derives from it and asks `DX8WebBrowser` to draw Internet Explorer into a D3D8
texture; `Win32GameEngine::createWebBrowser()` hands back `NEW CComObject<W3DWebBrowser>`. All of
that is byte-for-byte as it was, and the Wine/VC6 build across all 13 configurations still builds it.

## 3. What the portable side does

`RTS_HAS_EMBEDDED_BROWSER` is defined by `Core/Libraries/Source/EABrowserDispatch/CMakeLists.txt`,
in the `if(WIN32 ...)` branch that builds the BrowserDispatch COM server, and by nothing else. So
the feature is present exactly where the thing it needs is built, rather than being keyed off
`_WIN32` in a dozen places.

Where it is absent:

* `WebBrowser` keeps its name, its `SubsystemInterface` lifecycle and its `Webpages` INI table
  (`makeNewURL`/`findURL`, so `INIWebpageURL.cpp` is untouched). It loses the ATL/COM bases, which
  is what made it abstract without the dispatch server.
* `TheWebBrowser` is a `WebBrowser *` rather than a `CComObject<WebBrowser> *`, and stays null —
  as it does in retail, where `GameEngine::init()`'s `initSubsystem` call for it is commented out.
* `W3DWebBrowser` is not declared and its implementation compiles to nothing: it is the D3D8/IE
  half, with no meaning without either.
* `Win32GameEngine::createWebBrowser()` returns `nullptr`. Callers already handle a null browser.
* `GameEngine`'s `CComModule _Module` declaration, `Init()` and `Term()` are compiled out. The ATL
  module existed only to host this server.

No consumer was rewritten, no `#ifdef` was added to a menu, and nothing in `MainMenu.cpp` or
`DownloadMenu.cpp` was touched — the parallel GameSpy/online slice owns those. There is no fake
COM/ATL runtime and no browser engine.

## 4. Deliberately stubbed

`WebBrowser::createBrowserWindow()` and `closeBrowserWindow()` are reachable only by a caller that
constructed a `WebBrowser` itself, since the factory hands out none. They `DEBUG_CRASH` and open
nothing rather than returning a quiet success: a caller that gets there believes it is showing the
player a web page. Nothing on the path to running a skirmish calls them.

The WOL screens that host the control (`WOLLoginMenu.cpp`, `WOLLadderScreen.cpp`) keep their code;
they will get an empty window where the browser was, which is the cut-scope outcome, not this
slice's business.

## 5. The link, and what is missing for a binary that could start

The 10 archives now link with `PlatformMain.cpp`'s `main()` as the entry point. A file *is* produced,
but only because `--warn-unresolved-symbols` downgrades the 467 unresolved references to warnings:
this is not a runnable game, it is an inventory. Two standalone test tools that live inside measured
directories (`WWLib/platform/tests/gdi_font_metrics_dump.cpp`, `win32_file_api_test.cpp`) define a
`main()` of their own; `native-build.py` removes those two objects from the archives before linking,
otherwise the duplicate entry point is all the linker reports.

Every one of the 467 symbols is attributed — `Other / unclassified` is 0, which it has never been
before:

| Cause | Symbols | What it would take |
|---|---:|---|
| Defined in a layer not built here (renderer / audio) | 272 | WW3D2/WWAudio are level 4; the renderer slices own them |
| Defined in a translation unit that failed to compile | 100 | the remaining 17 failures: GameSpy screens, D3D8 residue, Win32 residue |
| GameSpy SDK (cut scope, not linked) | 33 | nothing — compile the online path out |
| FFmpeg (not linked here) | 29 | a link line; the headers are provisioned and the code compiles |
| Defined only in a backend this configuration excludes (SDL2 / Cocoa) | 12 | build the SDL2 window backend into this configuration |
| Defined in a built translation unit behind a disabled `#if` | 9 | build options (`RTS_BUILD_OPTION_*`), stack-dump and registry paths |
| Generated gitinfo (build-time, not a blocker) | 6 | run `git_watcher.cmake`; a harness artefact |
| Direct3D 8 / DirectX | 3 | `D3DXMatrix*`/`D3DXFilterTexture`, renderer slice |
| Win32 API | 3 | `GetCursorPos`, `ScreenToClient`, `SetCursor` — the mouse seam's remainder |

So, plainly: **the renderer is the wall.** 272 + 3 of the unresolved symbols are WW3D2/D3D8, and
most of the 100 blocked-behind-a-failure symbols are renderer or online. Nothing in the list is the
browser, the entry point, the engine core, the file system, timing, threading or audio device
selection any more. To get a binary that could *start*, in order: the renderer libraries at level 4
(the D3D8→Vulkan spike's territory), the online compile-out that clears the GameSpy screens, the
SDL2 window backend in the link, and the FFmpeg and gitinfo link lines, which are bookkeeping.

## 6. Gate

`scripts/ci/check-embedded-browser.py`, run in `native-port-ci.yml` against the same
`native-build.json` as the baseline gate. Structurally: ATL/COM spellings appear in the six seam
files only inside an `RTS_HAS_EMBEDDED_BROWSER` branch; the feature is defined by the Windows
BrowserDispatch target and nowhere else (a stray `#define` in a header would switch ATL back on for
everyone); the four consumers still use `TheWebBrowser`/`createWebBrowser` and mention the feature
nowhere, so the excision cannot leak into them. Numerically: no compile failure mentions ATL/COM,
and `GeneralsMD/Code/Main` produces at least one object and an archive.

The baseline gate's own ratchet was moved up by 10 unresolved symbols in this PR, with §1 as the
justification; the objects, failure-count and archive ratchets all improve.

## 7. Wave two: the DX8 layer underneath

§1-§6 excised the control from the engine. One layer below it there is a second half of the same
control — `DX8WebBrowser` in WW3D2, which hosts Internet Explorer *inside the D3D8 device*: it is
what draws the page into the back buffer, and it is called from the renderer's own frame loop rather
than from an online screen. Levels 1-3 never built WW3D2, so wave one could not see it. With level 4
(renderer and audio) measured, three translation units failed on it, all with the same diagnostic:

```
Core/Libraries/Source/WWVegas/WW3D2/dx8webbrowser.cpp                     unknown type name 'LPDISPATCH'
Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp                        unknown type name 'LPDISPATCH'
GeneralsMD/Code/GameEngineDevice/.../W3DDisplay.cpp                       unknown type name 'LPDISPATCH'
```

### Why the layer below needed anything different

Wave one's switch is `RTS_HAS_EMBEDDED_BROWSER`, absence of which removes declarations. That is not
enough here, for two reasons:

* `dx8webbrowser.h` had its **own, older switch**: a hand-set `#define ENABLE_EMBEDDED_BROWSER 1`
  with a comment saying "set this to 0 to remove all embedded browser code". Turning it to 0 by hand
  would also have removed the code from Windows. It is now *derived* from the feature — 1 where the
  BrowserDispatch server is built, 0 otherwise — so the existing `#if` structure the file was
  already written around becomes the seam, and Windows keeps every line.
* the header is included by `dx8wrapper.cpp`, the D3D8 chokepoint every renderer consumer goes
  through, and it included `<windows.h>` and `"d3d8.h"` and typed one default argument as
  `LPDISPATCH` **unconditionally**. So the OLE type surface was a dependency of the whole renderer,
  not of the browser: the entry points are what the callers need, and the OLE spelling in the
  signature is what they do not. `LONG options`/`LPDISPATCH gamedispatch` therefore became
  `DX8WebBrowserOptions`/`DX8WebBrowserDispatch`, which *are* `LONG`/`LPDISPATCH` where OLE exists
  and `long`/`void *` where it does not. `(LPDISPATCH)this` at the one real call site
  (`W3DWebBrowser.cpp`) is inside wave one's Windows-only branch and is untouched.

Neither `dx8wrapper.cpp` nor `W3DDisplay.cpp` was edited: both keep `DX8WebBrowser::Update()`,
`Render()`, `Initialize()` and `Shutdown()` exactly as written, which is the point of the seam.

### Measured

`scripts/native-build.py --with-shims`, clang 14, Linux/x86-64, before = `main` at `a6ef778ed`:

| | levels 1-3 before | after | levels 1-4 before | after |
|---|---:|---:|---:|---:|
| Objects | 834 / 839 | **835 / 839** | 952 / 971 | **954 / 971** |
| Translation units that fail to compile | 5 | **4** | 19 | **17** |
| Undefined symbols, total | 577 | 628 | 412 | 427 |
| `Defined in a translation unit that failed to compile` | 46 | **42** | 178 | 192 |
| `Defined in a layer not built here (renderer / audio)` | 323 | 377 | – | – |
| `Win32 API` | 3 | 4 | 3 | 4 |

Two compile failures fixed: `dx8webbrowser.cpp` (WW3D2, level 4) and `W3DDisplay.cpp` (which is
level 3, hence the one-object gain there too).

`Defined in a translation unit that failed to compile` moves in **both** directions, and the two
movements have the same cause. At levels 1-3 it falls 46 → 42: `W3DDisplay.cpp` compiles, so the
symbols it defines stop being blocked. At levels 1-4 it rises 178 → 192: `W3DDisplay.cpp`'s object
now *exists* in the link and references `DX8Wrapper`'s symbols, and `dx8wrapper.cpp` still fails —
so those references move into this category from nowhere. They were always there; the object that
made them was not. The same arithmetic explains the `renderer / audio` rise at levels 1-3 (+54: a
level-3 object referencing level-4 symbols) and the `Win32 API` +1 (§below).

### `dx8wrapper.cpp` is *not* fixed by this slice, and why

The browser was its first diagnostic, not its only one. With the seam in place it fails on:

| Remaining diagnostic in `dx8wrapper.cpp` | Owner |
|---|---|
| `no member named 'DriverVersion' in '_D3DADAPTER_IDENTIFIER8'` (×4) | `compat/win32-runtime-and-crt-gaps` — `Dependencies/Utility/Utility/d3d8_compat.h` already has the accessor (`D3D8AdapterDriverVersion`); the four call sites are that slice's remaining work, and `dx8caps.cpp` fails on the same thing |
| `GetWindowLong`, `GWL_STYLE`, `AdjustWindowRect`, `MONITORINFO`, `MONITOR_DEFAULTTOPRIMARY`, `SWP_NOZORDER`, `GetDesktopWindow`, `SetDeviceGammaRamp` | the WW3D2 header/type-residue and window slices — the window/monitor/gamma corner of `<windows.h>` |
| `DWORD *` vs `unsigned long *` at `ValidateDevice(&passes)` | the same `d3d8_compat.h`/LP64 cluster |

`dx8wrapper.{h,cpp}` is a serialised path held by `renderer/native-compile-and-entrypoint`
(`docs/porting/concurrent-slices.md`), and `DriverVersion` is an explicitly claimed failure class, so
this slice reports them rather than fixing them. Nothing in that list is the browser: the 192 symbols
blocked behind `dx8wrapper.cpp` are now waiting on those two slices alone.

### `W3DDisplay.cpp` and `WinMain.h`

Its `#include "WinMain.h"` resolved through nothing in the probe's include list, so the file reported
a missing header instead of its real state. It is not a portability problem: `GeneralsMD/Code/Main` is
on this library's include path in the real build too — `GameEngineDevice` links `zi_main`, whose only
content is that directory. `scripts/native-port-probe.py` now says so for the
`GeneralsMD/Code/GameEngineDevice` target (measurement-only; no engine file changed).

That left one genuinely missing Win32 declaration, `::IsIconic(ApplicationHWnd)` in
`W3DDisplay::draw()` — the "don't render while minimised" early-out. It is declared in the shim's
user32 block alongside `ShowWindow`/`GetWindowRect` and added to
`docs/porting/ci-baselines/win32-undefined-budget.json`, which is why that category is 4 rather than
3. It is the same situation as the three cursor calls already in the budget: the real answer is
`platform_window.h`'s `Window_Is_Minimised()`, and rewiring this call site belongs to
`platform/window-seam-wiring`, not here.

### Deliberately stubbed, wave two

`DX8WebBrowser`'s per-frame entry points — `Update()`, `Render()` — do nothing, silently: they are
called from `DX8Wrapper::Begin_Scene()`/`End_Scene()` on every frame of a skirmish, and "no browsers
to draw" is exactly what they mean when no browser was created. `Initialize()` returns false after
saying so once; `Is_Browser_Open()` returns false; `DestroyBrowser()` does nothing.
`CreateBrowser()` and `Navigate()` `WWASSERT_PRINT` and log, on the same reasoning as
`WebBrowser::createBrowserWindow()` in §4: reaching them means a caller believes it is putting a web
page in front of the player. Nothing on the path to a campaign or skirmish frame does.

### Gate, wave two

`check-embedded-browser.py` covers the DX8 layer as well now. `dx8webbrowser.{h,cpp}` join the seam
files, so ATL/COM/`LPDISPATCH` spellings in them must sit inside the feature's branch —
`ENABLE_EMBEDDED_BROWSER` is understood to *be* the feature, because a separate check proves it is
derived from it and defined in exactly one place. That check is the important one: a bare
`#define ENABLE_EMBEDDED_BROWSER 1`, which is how the header used to read, would switch
`<windows.h>`, `<d3d8.h>` and `LPDISPATCH` back on for the whole renderer while leaving no ATL
spelling anywhere for the other checks to catch. `dx8wrapper.cpp` and `W3DDisplay.cpp` join the
consumer list (they must keep calling `DX8WebBrowser::` and must not grow a browser `#ifdef`), the
compile-failure regex now includes `LPDISPATCH`, and `dx8webbrowser.cpp`/`W3DDisplay.cpp` are
required to compile in whichever measurement builds them.

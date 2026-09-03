# The user32 window/GDI seam

`W3DMouse.cpp`, `W3DDisplay.cpp` and `dx8wrapper.cpp` call twelve `user32`/`gdi32` entry points
directly. This document is about what those twelve mean off Windows, and about the one mistake in
this area that no gate in this repository can catch.

The definitions are in
`Core/Libraries/Source/WWVegas/WWLib/platform/platform_win32_user.cpp`, compiled only off Windows —
Windows links the real `user32`, so the port must not introduce a duplicate symbol into the 13
Windows configurations. Declarations are in `scripts/native-port-shims/windows.h` for the native
build. Every one of them is implemented over the portable window seam in
`platform/platform_window.h`, backed by `platform_window_sdl2.cpp` and `platform_window_cocoa.mm`;
none of them talks to a windowing system directly.

| Entry point | Portable seam it uses | Called by |
|---|---|---|
| `GetCursorPos` | `Window_Cursor_Position` | `W3DMouse.cpp` |
| `ScreenToClient` | `Window_Client_Origin` | `W3DMouse.cpp` |
| `SetCursor` | `Window_Show_System_Cursor`, `Window_Set_Cursor` — see `mouse-cursor-seam.md` | `W3DMouse.cpp`, `Win32Mouse.cpp` |
| `IsIconic` | `Window_Is_Minimised` | `W3DDisplay.cpp` |
| `GetClientRect` | `Window_Client_Size` | `dx8wrapper.cpp` |
| `GetWindowLongA` | `Window_Is_Fullscreen`, `Window_Is_Minimised` | `dx8wrapper.cpp`, `Debug.cpp` |
| `AdjustWindowRect` | `Window_Frame_Insets` | `dx8wrapper.cpp` |
| `MonitorFromWindow` | `Window_Display_For_Window` | `dx8wrapper.cpp` |
| `GetMonitorInfoA` | `Window_Display_Bounds`, `Window_Display_Work_Area` | `dx8wrapper.cpp` |
| `SetWindowPos` | `Window_Set_Position`, `Window_Set_Client_Size`, `Window_Set_Always_On_Top`, `Window_Show` | `dx8wrapper.cpp` |
| `GetDesktopWindow` | *nothing — see §4* | `dx8wrapper.cpp` |
| `SetDeviceGammaRamp` | *nothing — see §4* | `dx8wrapper.cpp` |

## 1. Points, not pixels — the bug this seam exists to prevent

`docs/porting/decisions-resolved.md` fixes the coordinate policy: **engine, UI and mouse coordinates
are in points; renderer and backing-store dimensions are in pixels; the conversion happens at the
renderer boundary and nowhere else.** Every number crossing this seam is therefore in points.
`Window_Client_Size()` is not changed to return pixels, and nothing here multiplies by a scale
factor.

Why this needs saying: on a Retina Mac `backingScaleFactor` is 2, and on the CI runner it is 1.
A `GetClientRect()` that returned the backing size instead of the point size would produce
identical numbers in CI and a window that is resized on every call on real hardware —
`DX8Wrapper::Resize_And_Position_Window()` compares `rect.right - rect.left` against
`ResolutionWidth` and resizes when they differ, so a doubled width never converges. **No gate in
this workflow can fail on that**, which is why the test asserts the point size explicitly rather
than asserting only that the call succeeded.

## 2. Frame versus client, and the direction of the insets

Win32 distinguishes the *client* area from the *frame* that surrounds it, and the four entry points
in the sizing path do not deal in the same one:

- `GetClientRect()` answers a client rectangle with its origin at `(0, 0)`: `right`/`bottom` are the
  client's **width and height**, never screen coordinates.
- `AdjustWindowRect()` converts a client rectangle to the frame that would contain it. Win32 moves
  `left`/`top` **negative** and `right`/`bottom` positive, so `-rect.left` is the left border and
  `-rect.top` the title bar. `DX8Wrapper` subtracts `rect.left`/`rect.top` from its centred
  position; with the signs reversed the window would move by twice the border.
- `SetWindowPos()` is given a **frame** rectangle, and the seam places a **client** area, so the
  client origin is the frame origin plus the top-left insets and the client size is the frame size
  minus both. Reversed, the render window grows by the border every time it is positioned.
- `GetMonitorInfoA()`'s `rcWork` is *not* `rcMonitor`: it excludes the menu bar and the Dock, as it
  excludes the taskbar on Windows, and it is the rectangle the window is centred inside. Returning
  the display bounds for both puts the title bar behind the menu bar.

The insets come from the platform: `SDL_GetWindowBordersSize()`, or AppKit's own
`-frameRectForContentRect:` arithmetic rather than a hard-coded 22-point title bar.

`AdjustWindowRect()` reads the style it is handed, so a borderless (`WS_POPUP`) window gets no
border added — which is what keeps a fullscreen back buffer from being sized larger than the
screen. `GetWindowLongA(GWL_STYLE)` is what produces that style, and it reports the frame bits from
`Window_Is_Fullscreen()` rather than from a stored `LONG`: there is no per-window `LONG` store off
Windows.

macOS's screen coordinates have their origin at the bottom-left of the **primary** screen, so every
conversion in the Cocoa backend flips against that screen's height. `+[NSScreen mainScreen]` is a
different screen — the one with keyboard focus — and using it puts every converted `y` out by the
difference between the two screens' heights, on multi-monitor hardware only.

## 3. What the test asserts

`Core/Libraries/Source/WWVegas/WWLib/platform/tests/win32_user32_test.cpp`, run by
`scripts/native-win32-user32-test.py` and by the `native-port-ci` workflow: 151 checks, and it is
headless. SDL2 and Cocoa both need a display server, so the test **fakes the seam** — it defines the
`WWPlatform::Window_*` functions itself with a known geometry (a 1920x1200 primary display with a
24-point menu bar and a 1280x1024 display at x = -1280, and 3/28/3/5-point frame insets) and links
`platform_win32_user.cpp` against that. What is under test is therefore the translation from Win32
semantics to seam calls, not SDL2's or AppKit's own behaviour.

Beyond the per-entry-point assertions, the test replays
`DX8Wrapper::Resize_And_Position_Window()`'s exact composition — `GetClientRect`, `GetWindowLongA`,
`AdjustWindowRect`, `MonitorFromWindow`, `GetMonitorInfoA`, `SetWindowPos` — and asserts the two
properties that only the composition has: the client area ends up **exactly** the requested
resolution, and the frame including its title bar lands inside `rcWork`. Individually-correct entry
points can still compose into a window one title bar out of place.

Not covered, and deliberately: the real SDL2 and Cocoa backends. Those need a display, are exercised
by `spikes/renderer/tools/macos-window-check.sh` and the renderer spike, and the multi-monitor and
Retina paths in them have not been run on hardware with two displays.

## 4. The three that are not implemented, and what they cost

Nothing here is a silent stub. Each reports through `Report_Stub()` — once, loudly, naming itself —
and returns the value Win32 returns on failure, so a caller's own fallback runs.

- **`SetDeviceGammaRamp` returns `FALSE`.** There is no portable display gamma ramp; macOS's
  equivalent (`CGSetDisplayTransferByTable`) affects the whole display, persists past a crash, and
  needs no privilege — a game that dies mid-frame would leave the user's desktop discoloured. The
  caller is `DX8Wrapper::Set_Gamma()`'s fallback path, taken only when the device reports no gamma
  support, and it ignores the result. **Cost: the brightness slider does nothing off Windows.** A
  `TRUE` here would be the invisible lie — the slider would appear to work and the screen would not
  change — which is why the test asserts `FALSE`.
- **`GetDesktopWindow` returns a non-null sentinel that is not a window.** The gamma path needs a
  handle to pass to `GetDC()`; a null one makes the caller skip its own `ReleaseDC()`. The sentinel
  is deliberately not a window the seam will act on, so `IsIconic()`/`GetClientRect()` on it fail
  rather than reaching the game's window. **Cost: none for this call site**; any future caller that
  wants the desktop's *size* must ask `GetMonitorInfoA()` instead.
- **`SetCursor` selects a shape** since the mouse-cursor slice: a non-null `HCURSOR` is a handle
  `LoadCursorFromFile()` made from a decoded `.ANI`, and the shape goes to `Window_Set_Cursor()`.
  What remains a cost is that a cursor whose file is missing selects the platform arrow — see
  `docs/porting/mouse-cursor-seam.md`.

`GetWindowLongA` answers `0` for any index other than `GWL_STYLE`/`GWL_EXSTYLE` and reports it:
there is no `WndProc` or `HINSTANCE` to hand back. `AdjustWindowRect` reports and falls back to the
client rectangle when the platform will not give up its insets — the client size, which is what the
back buffer matches, stays right and only the frame's placement is off.

## 5. Measured

Linux/x86-64, clang 14, `./scripts/ci/fetch-probe-deps.sh` then
`scripts/native-build.py --with-shims --strict-link`, re-measured on `main` `210bc9857` (after #82
removed the last three compile failures, so every object figure here is now n/n). This slice also
landed the D3DX texture/FVF/shader entry points (`docs/porting/d3dx8-texture-seam.md`), so the D3DX
column moves here too.

| | before | after |
|---|---:|---:|
| Levels 1-4: object files | 972 / 976 | **976 / 976** |
| Levels 1-4: unresolved symbols | 73 | **68** |
| Levels 1-4: `no-definition-anywhere` | 22 | **5** |
| Levels 1-4: `Win32 API` | 12 | **0** |
| Levels 1-4: `Direct3D 8 / DirectX` | 9 | **5** |
| Levels 1-4: `Engine C++ not built at this level` | 1 | **0** |
| Levels 1-4: `... backend this configuration excludes` | 12 | **24** |
| Levels 1-3: `no-definition-anywhere` | 6 | **2** |
| Levels 1-3: unresolved symbols | 548 | **456** |

The "before" column is `main`'s checked-in baselines. Its levels 1-3 copy predates #82, which is why
that row also shows #82's effect (`cut-scope-not-linked` 82 → 0, `compile-blocked` 18 → 0) rather than
only this slice's: the file is regenerated by the script, never hand-merged, so a re-measurement here
picks up whatever `main` had not re-measured. Only the levels 1-4 rows are this slice alone.

Three of those figures would be read wrongly without a note.

**`Win32 API` 12 → 0, so `win32-undefined-budget.json`'s allowance is now empty.** #53 widened it by
three cursor names and #79 by eight window/GDI names, both disclosed; this slice earns all twelve
back, and `check-win32-undefined.py` now gates at zero.

**The excluded-backend category grew by 12, which is why the unresolved total falls by only 5 at
levels 1-4.** `platform_win32_user.cpp` now references twelve seam
functions it did not before — `Window_Current`, `Window_Cursor_Position`, `Window_Client_Origin`,
`Window_Frame_Insets`, `Window_Is_Fullscreen`, `Window_Set_Position`, `Window_Set_Client_Size`,
`Window_Set_Always_On_Top`, `Window_Show_System_Cursor`, `Window_Display_For_Window`,
`Window_Display_Bounds`, `Window_Display_Work_Area` — and this harness links neither window backend
(`probe.OPTIONAL_BACKENDS` keeps SDL2 opt-in; the Cocoa backend is Objective-C++). **The definitions
are in the tree**, in both backends, so a configuration that picks one resolves all 24 at once. This
is the "new symbols your implementations reference" figure: 17 resolved, 12 new references, net 5.

**The two names left in `no-definition-anywhere` at levels 1-3 are a classification artefact, not
work — and the ordering that causes it will misclassify future symbols too.** The two are
`D3DXFilterTexture` and `D3DXAssembleShader`. Both are defined in this tree, in
`Core/Libraries/Source/WWVegas/WW3D2/{d3dx8texture.cpp,d3dx8shader.cpp}`, i.e. in `WW3D2` — a
**level-4** library. At a level set that does not build `WW3D2` there is no archive to resolve them
from, and `native-build.py` attributes them by their `D3DX` **name pattern before** it consults its
"defined in a layer not built here" **evidence**, so they land in `no-definition-anywhere` (port work)
instead of the harness/not-built pile. At levels 1-4, where `WW3D2` is built, both resolve and the
pile is 5.

The fix is a precedence change, not a special case: **evidence first, name pattern last** — if an
`nm` scan of the tree finds a definition in a library this level set does not build, that attribution
wins, and a vendor-prefix name pattern is only the fallback for symbols with no definition anywhere.
As written, any symbol that shares a vendor prefix (`D3DX`, `AIL_`, `av*`, …) but lives in a level-4
library will be counted as port work at lower levels. `scripts/native-build.py` is slice 4's file
(the harness), so this is reported for slice 4 to act on rather than fixed here.

**`window-input-scan.json`: the in-scope Win32 window/input surface goes 665 → 731 references, and
51 of those 66 are the test file.** That gate measures how much Win32 window/input surface is still
*referenced* in scope, and this slice is supposed to shrink what is *undefined* without growing what
is referenced, so the delta was measured by re-running the scan with pieces removed rather than
baselined on sight:

| tree | in-scope references | in-scope HWND files |
|---|---:|---:|
| `main` `210bc9857` | 665 | 26 |
| + the eight implementations in `platform_win32_user.cpp` | 680 | 26 |
| + `platform/tests/win32_user32_test.cpp` | **731** | **27** |

So **+15 is the seam implementation and +51 (77%) is the test**, and the one new HWND file in scope is
the test. No engine or consumer call site was added: the +15 are the *definitions* of the eight names
under their Win32 spelling — the file where `GetClientRect` and friends stop being undefined is
necessarily a file that mentions `GetClientRect` — and the +51 are the 151 behaviour checks that call
them. The scanner counts mentions, not consumers, so the number it reports is now inflated by the
tests that make the surface trustworthy; the count of *consumer* files is unchanged. If that
distinction matters to the ratchet later, the scanner would have to exclude `platform/tests/**` (or
count it separately), which is `platform/macos-window-compile`'s file, not this slice's.

Nothing in this seam changes what Windows compiles: `platform_win32_user.cpp` is `#if`-ed out there,
the shim header is native-only, and the rest is new files. The one change this slice makes to live
Windows code is `WWAudio/listenerhandle.h` — see §6 — so the Wine/VC6 Windows build was run for that
locally (green), and `check-replays.yml`'s retail replay gate ran on this pull request (the diff
touches `Core/**`) and **passed** — `Replay Check GeneralsMD / vc6+t+e` and
`vc6-releaselog+t+e` both green — so the deleted declaration is behaviour-checked against retail
replays rather than only argued.

## 6. `ListenerHandleClass::Initialize(SoundBufferClass*)`

Not a window symbol, but the last name in this slice's pile. It was declared `override` in
`WWAudio/listenerhandle.h` and **defined nowhere** — not in `listenerhandle.cpp`, not upstream. The
resolution is to delete the declaration, so the slot inherits `Sound3DHandleClass::Initialize()`.

The argument: `ListenerHandleClass` is never constructed anywhere in the tree, and
`As_ListenerHandleClass()` is never called, so the constructor's COMDAT — and with it the vtable that
would have referenced the missing definition — is discarded before the Windows link ever asks for it.
That is why retail links and this harness does not: `--whole-archive` asks. Every other override in
the class is an empty body, so a no-op `Initialize()` would have been the *plausible* invention, and
it would have been an invention: it would skip `SoundHandleClass::Initialize()`'s bookkeeping on a
class whose behaviour no caller can observe. Deleting a declaration that cannot be right either way
is the honest resolution, and it leaves no new symbol behind.


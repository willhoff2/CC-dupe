# Window, event loop and input

Measured, not estimated. Every number here is produced by `scripts/window-input-scan.py`, which
strips comments and string literals before matching and is gated in CI against
`docs/porting/ci-baselines/window-input-scan.json`:

```
python3 scripts/window-input-scan.py               # the tables below
python3 scripts/window-input-scan.py --check       # what CI runs
```

**Read this first.**

| Piece | Status |
|---|---|
| The seam (`platform_window.h`) and its SDL2 backend | built and run; a real X11 window presented 240 Vulkan frames, read back and pixel-checked, 0 validation messages |
| The Cocoa/`CAMetalLayer`/MoltenVK backend (`platform_window_cocoa.mm`) | **written blind, now executed.** On a `macos-15` arm64 runner it compiles, creates an `NSWindow`, gets a `VkSurfaceKHR` through `vkCreateMetalSurfaceEXT`, presents 240 frames, survives a mode change to 1024x768 and reads the pixels back with 0 validation messages. What remains unverified is a display: see [section 4](#4-what-is-still-unverified-macos) |
| The engine's non-Windows window/message/input path going through the seam | wired and compiled on Linux (clang 14, `-m64`, C++20); **never linked and never run**, because the renderer and audio device layers do not compile natively yet — see [6. The engine wiring](#6-the-engine-wiring) |

## 1. The Win32 surface being replaced

Categories are symbol groups, not lines: `wndproc` counts `WM_*`/`WPARAM`/`LPARAM`/`WndProc`,
`message_pump` counts `PeekMessage`/`GetMessage`/`DispatchMessage`/`MSG`, and so on. In-scope means
Zero Hour engine, device layer and libraries; out of scope is `Tools/` (WorldBuilder, W3DView,
GUIEdit, ImagePacker, ParticleEditor) and the base `Generals/` tree. The port harness itself
(`spikes/`, `scripts/native-port-shims/`) is not scanned at all: the shims *declare* `HWND` and the
whole `VK_*` table, and the spike is Vulkan code whose `VK_STRUCTURE_TYPE_*` tokens a regex cannot
tell from `VK_LBUTTON`, so counting it would both inflate the figures and make them move whenever
the renderer changes. The same collision reaches in-scope files now that the portable window
backends name `VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT`, so `polled_input`'s `VK_*` pattern
excludes Vulkan's prefixes (`VK_STRUCTURE_TYPE_`, `VK_USE_PLATFORM_`, `VK_FORMAT_`, …) rather than
the files. That exclusion changes no figure in the table below.

| Category | In-scope refs | In-scope files | Out-of-scope refs | Out-of-scope files |
|---|---:|---:|---:|---:|
| `window_handle` | 150 | 29 | 1333 | 258 |
| `message_pump` | 25 | 7 | 332 | 54 |
| `wndproc` | 342 | 9 | 2163 | 311 |
| `polled_input` | 29 | 10 | 101 | 33 |
| `cursor` | 36 | 9 | 281 | 95 |
| `placement` | 21 | 5 | 576 | 120 |
| `mode_change` | 53 | 8 | 26 | 8 |
| **Total** | **656** | **41** | **4812** | **425** |

The in-scope figure rose from 630 to 656 when the engine was wired to the seam ([section
6](#6-the-engine-wiring)), which looks like the wrong direction and is not: the seam keeps the
Win32 *spellings*, so `ApplicationHWnd` (52 -> 74) and `HWND` (50 -> 54) are now also counted in
the portable files that define and use them. What the scan cannot distinguish, and the wiring
changed, is a `WM_*` case dispatched by `DispatchMessage` from the same name appearing in a
comment-free `#ifdef _WIN32` branch that no longer runs off Windows.

**24 in-scope files mention `HWND`, against 176 out of scope.** That is consistent with
`next-slice-scope.md`'s "roughly 20 `HWND` files in scope": the extra four are the `Core/Libraries/Source/debug`
dialog library, which that count did not walk. So 88% of this repo's `HWND` files belong to
the tools that are staying on Wine.

The individual API calls, in scope only — this is the actual work list:

| Symbol | Refs | Files |
|---|---:|---:|
| `WM_*` | 272 | 7 |
| `ApplicationHWnd` | 74 | 15 |
| `HWND` | 54 | 24 |
| `LPARAM` / `WPARAM` / `LRESULT` | 43 / 15 / 5 | 7 / 7 / 2 |
| `Set_Render_Device` | 19 | 5 |
| `MSG` | 16 | 6 |
| `VK_*` virtual keys | 13 | 6 |
| `D3DPRESENT_PARAMETERS` | 12 | 4 |
| `Set_Device_Resolution` / `Reset_Device` / `Toggle_Windowed` | 9 / 8 / 5 | 5 / 3 / 4 |
| `SetCursor` / `ShowCursor` / `LoadCursor` | 9 / 5 / 4 | 3 / 4 / 2 |
| `GetAsyncKeyState` | 6 | 1 |
| `ShowWindow` / `SetWindowPos` / `UpdateWindow` | 7 / 4 / 2 | 3 / 2 / 2 |
| `WndProc` / `DefWindowProc` | 5 / 3 | 2 / 2 |
| `ScreenToClient` / `ClientToScreen` / `GetClientRect` / `GetWindowRect` | 4 / 4 / 4 / 3 | 4 / 2 / 4 / 3 |
| `ClipCursor` / `GetCursorPos` / `SetCursorPos` / `SetCapture` / `ReleaseCapture` | 3 / 3 / 2 / 1 / 1 | 2 / 3 / 1 / 1 / 1 |
| `IsIconic` | 2 | 2 |
| `GetKeyState` / `GetKeyboardLayout` / `HKL` / `GetDoubleClickTime` | 2 / 2 / 2 / 2 | 1 / 2 / 2 / 2 |
| `DirectInput8Create` | 2 | 2 |
| `CreateWindow` / `RegisterClass` / `RegisterClassEx` / `DestroyWindow` / `AdjustWindowRect` | 2 / 1 / 1 / 1 / 2 | 2 / 1 / 1 / 1 / 2 |
| `GetMessage` / `DispatchMessage` / `TranslateMessage` / `PeekMessage` / `PostQuitMessage` | 2 / 2 / 2 / **1** / 2 | 2 / 2 / 2 / **1** / 2 |
| `SetFocus` / `SetForegroundWindow` / `GetWindowLong` / `GetSystemMetrics` | 1 / 2 / 2 / 2 | 1 / 1 / 2 / 1 |
| `BeginPaint` / `EndPaint` | 2 / 2 | 2 / 2 |

Two things fall out of that table.

**There is exactly one `PeekMessage` in the whole in-scope tree**, in
`GeneralsMD/Code/GameEngineDevice/Source/Win32Device/Common/Win32GameEngine.cpp`. The message loop
is not spread through the engine; it is thirteen lines in one function.

**`ChangeDisplaySettings` does not appear at all.** The game never changes the desktop video
mode — "fullscreen" is a `WS_POPUP` window the size of the screen plus a D3D8 device reset. That is
borderless fullscreen, which is exactly what macOS and Wayland can do, so the hardest-looking item
on the list turns out not to exist.

### The files that have to change

41 in-scope files, but the mass is in four of them:

| File | Refs | What it is |
|---|---:|---|
| `GeneralsMD/Code/Main/WinMain.cpp` | 247 | `WndProc` (217 of the 272 `WM_*`), `RegisterClass`/`CreateWindow`, the fullscreen/windowed style choice, `SetWindowPos`/`ShowWindow` |
| `Core/GameEngine/Source/GameClient/GUI/IMEManager.cpp` | 44 | IME composition via `WM_IME_*`; a separate problem, not this seam |
| `Core/Libraries/Source/debug/*` (4 files) | 103 | the debug dialog: its own window class and `DialogProc`s. Not the game window |
| `Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.{cpp,h}` | 41 | `Set_Render_Device`/`Reset_Device`/`Toggle_Windowed`, `D3DPRESENT_PARAMETERS`, the `HWND` D3D8 presents to |
| `Core/GameEngineDevice/.../Win32Mouse.cpp` + `Win32DIMouse.cpp` | 50 | `ClipCursor`, `SetCapture`, `ScreenToClient`, the `WM_?BUTTON*` translation |
| `Win32GameEngine.cpp` | 13 | the message pump and the `IsIconic` idle loop |
| `Win32DIKeyboard.cpp` | 6 | DirectInput device state, `GetKeyState` for the modifiers |
| `Core/GameEngine/Source/Common/System/Debug.cpp` | 14 | `MessageBox` on assert, `ShowWindow` |
| `Core/GameEngine/Source/GameClient/Input/Keyboard.cpp` | 2 | `GetKeyboardLayout` for the layout-dependent character mapping |

`Debug.cpp` and `Keyboard.cpp` are the two files `native-build-report.md` lists as failing to
compile natively *purely* on window/input types; both are in the tail of this table, not the head.

## 2. The seam

`Core/Libraries/Source/WWVegas/WWLib/platform/platform_window.h`, 279 lines, in the same shape as
`platform_thread`/`platform_time`/`platform_process`: a `WWPlatform` namespace, free functions,
`#ifndef _WIN32` around the whole file, opaque `void *` handles so that no Xlib, SDL, Cocoa or
Vulkan header reaches an engine header.

```
Window_Create / Window_Destroy                    CreateWindow / DestroyWindow
Window_Poll_Event / Window_Pump                   PeekMessage+GetMessage+TranslateMessage+DispatchMessage
Window_Is_Minimised / Window_Is_Active            IsIconic / the isWinMainActive flag
Window_Client_Size                                GetClientRect
Window_Set_Mode / Window_Show / Window_Set_Title  SetWindowPos+ShowWindow / ShowWindow / SetWindowText
Window_Set_Cursor_Clip                            ClipCursor + SetCapture/ReleaseCapture
Window_Warp_Cursor / Window_Show_System_Cursor    SetCursorPos / ShowCursor
Window_Key_Is_Down / Window_Modifier_State        GetAsyncKeyState / GetKeyState
Window_Vulkan_Instance_Extensions                 (new: the renderer handoff)
Window_Create_Vulkan_Surface / Window_Native_Surface
```

Three decisions worth stating, because they are where this diverges from Win32:

**Events are pulled, not dispatched.** `Window_Poll_Event()` returns them one at a time so the
existing `while` loop keeps its shape. Win32 instead *calls* `WndProc` from inside
`DispatchMessage()`, so today the engine's handling runs at an arbitrary stack depth, re-entrantly,
including from inside `MessageBox` and D3D device resets. Anything in `WinMain.cpp`'s `WndProc` that
relies on running at that moment rather than at the top of the frame has to be re-examined one case
at a time. That is the real cost of this cutover and it is not paid yet.

**Keyboard events carry PC/AT set-1 scan codes**, i.e. literally the `KEYSCAN_*` values in
`Core/GameEngine/Include/GameClient/KeyScanCodes.h`, which is what `KeyDefType` already stores and
what the DirectInput path already produced (`DIK_*` and `KEYSCAN_*` are the same numbers; that file
static-asserts it on Windows). So the seam feeds `KeyboardIO::key` without a translation table in
between, and `keyboard-scancodes.md`'s existing seam is reused rather than duplicated. WWLib must
not depend on GameEngine, so each backend carries its own numeric table and
`scripts/ci/check-window-scancodes.py` proves the tables against `KeyScanCodes.h` in CI:

```
KeyScanCodes.h: 107 KEYSCAN_* constants
platform_window_cocoa.mm         104 entries, 3 unmapped (KEYSCAN_CIRCUMFLEX, KEYSCAN_CONVERT, KEYSCAN_NOCONVERT)
platform_window_sdl2.cpp         107 entries, 0 unmapped (none)
```

The three unmapped Cocoa entries are JIS keys with no `kVK_` constant; the check knows about them
by name, so if a fourth appears it fails.

**Mouse events keep the fields `MouseIO` needs**: client-area coordinates with a top-left origin,
`Wheel_Delta` in `WHEEL_DELTA` (120) units with `WM_MOUSEWHEEL`'s sign (because `Win32Mouse.cpp`
already divides by 120), a `Click_Count` of 2 for the `WM_?BUTTONDBLCLK` case, and a monotonic
`Time_Ms` replacing `MSG::time`, which the engine currently smuggles through the global
`TheMessageTime` into `MouseIO::time`.

### Why SDL2 for the portable backend

Chosen, with a caveat. In favour: it is already an optional dependency of the spike; it covers X11
*and* Wayland *and* Cocoa from one file; it owns `VK_KHR_xlib_surface`/`VK_KHR_wayland_surface`
selection, which is otherwise a compile-time guess; its scancode set is a strict superset of set-1,
so the mapping is total (107/107 above); and it removes the need to hand-write two Linux backends
before either is exercised. Against, and the reason the Cocoa backend exists anyway: on macOS
SDL2's `NSView`/`CAMetalLayer` handling is opaque, and the plan needs a `CAMetalLayer` whose
`contentsScale` and `drawableSize` the renderer controls for Retina and for mode changes. A game
that ships a Mac binary should own its `NSWindow`. So: SDL2 is the portable backend and the Linux
answer, and `platform_window_cocoa.mm` is the native macOS one, both behind the same header, with
the same driver run against both.

`Window_Native_Surface()` is deliberately not implemented by the SDL2 backend (it reports so
through `Window_Last_Error()`): producing an X11 `Display *` needs `SDL_syswm.h`, which drags Xlib
headers into a WWLib translation unit for no gain, since `Window_Create_Vulkan_Surface()` is the
only thing the renderer needs. The Cocoa backend does implement it, returning the `CAMetalLayer`.

## 3. What was verified, on Linux

`spikes/renderer/src/window_spike.cpp` (`zh-window-spike`) runs the same two draws as the headless
spike — a textured, modulated, depth-tested `XYZ|DIFFUSE|TEX1` triangle and a blended `XYZRHW` HUD
quad — through the seam and into a real window. It is a check, not a demo: it asserts and prints
PASS/FAIL per claim, reads the final frame back and fails if the centre pixel is still the clear
colour, and fails on any validation message.

On an X session on this box (Ubuntu 24.04, lavapipe, `DISPLAY=:0`):

```
window backend: sdl2
PASS   Window_Create()
PASS   Window_Vulkan_Instance_Extensions() names >= 2 extensions
       instance extension: VK_KHR_surface
       instance extension: VK_KHR_xlib_surface
PASS   a platform surface extension is required by the window
PASS   VkSurfaceKHR + swapchain from the seam's window
device: llvmpipe (LLVM 15.0.7, 256 bits)
PASS   Window_Set_Mode(1024x768, windowed)
       resize to 1024x768
PASS   frames presented to the window's swapchain
       presented 240 frame(s)
       client size after the mode change: 1024x768
PASS   the swapchain was rebuilt for the new window size
PASS   Read_Back_Color_Target()
       centre pixel rgba = 132,151,170,255
PASS   the presented frame contains the geometry, not just the clear colour
events seen: resize=1 move=1 focus-gained=1 mouse-move=2 mouse-enter=2 mouse-leave=1
validation messages: 0
PASS   no Vulkan validation messages
```

That is the first window this port has ever opened: before it, both the Linux and macOS spike paths
rendered to an offscreen image and read it back. The `vulkan_backend.cpp` change behind it is small
— the instance-extension query and `vkCreateSurfaceKHR` call now go through the seam instead of
`SDL_Vulkan_*` — which is the point: the renderer no longer knows what a window is.

Opening a window turned up two real defects in that renderer, neither of which the headless path
could ever have shown, and both of which are fixed here:

* the swapchain was rebuilt only on `VK_ERROR_OUT_OF_DATE_KHR`, which lavapipe/X11 never returns
  for a programmatic resize, so after `Window_Set_Mode()` the window grew but the presented image
  stayed 800×600 in the corner with the rest never painted. It now rebuilds on
  `WINDOW_EVENT_RESIZE` (`RenderBackend::Resize_Presentation()`) and the present blit stretches
  the colour target to the swapchain extent, which is the cheap half of a D3D8 `Reset_Device`. The
  spike now *checks* this, because the readback cannot see it: the colour target keeps its own size,
  so a frozen window still produces a correct PNG;
* the present path reused one acquire semaphore every frame, which is
  `VUID-vkAcquireNextImageKHR-semaphore-01779` — 121 validation errors on a CI runner with
  validation 1.3.275, and silence locally on 1.3.204, which is a good argument for CI pinning the
  newer layers. The image is now acquired with a fence: every submission in the spike is already
  CPU-waited, so a fence is the whole of the synchronisation and the present needs no wait
  semaphore.

One usability note that matters when a human is watching rather than a script: unpaced, this presents
several hundred frames a second even on llvmpipe, so `--frames 240` is over in under two seconds.
`--frame-ms 30` paces the loop.

CI job `Window seam (Linux, Xvfb + lavapipe)` runs the scanner check, the scan-code check, and 240
frames under Xvfb. Xvfb is a real X server, so the surface, swapchain, present and event
translation are all genuinely exercised; it has no monitor, so **CI does not prove pixels reached a
screen.** That part was checked by hand on a desktop session, as above.

## 4. What is still unverified: macOS

`platform_window_cocoa.mm` was written on Linux with no macOS SDK and had never been compiled when
PR #32 merged. It has now been compiled and run on a `macos-15` arm64 GitHub runner (macOS 15.7.7,
Xcode 16.4 AppleClang 17.0.0, Homebrew MoltenVK, Vulkan header 1.4.357, `Apple Paravirtual device`).
CI job `Window seam (macOS arm64, CAMetalLayer)` is that run, and it is a gate, not informational.

### What the runner proved

| Claim that was a guess | How it is now checked | Result |
|---|---|---|
| the Objective-C++ compiles at all | the job builds `zh-window-spike-cocoa`, `-Wall -Wextra` | compiles and links, no warnings from the backend |
| the 104 `kVK_*` literals are the real HIToolbox values | `WWLIB_COCOA_VERIFY_KVK` turns each row into a `static_assert` against `<Carbon/Carbon.h>` | all 104 correct as written |
| `VkMetalSurfaceCreateInfoEXT`'s layout and `sType` | the local mirror is `static_assert`ed field-by-field against the real header when one is present | correct: `sType` 1000217000, 32 bytes, offsets match |
| `VK_EXT_metal_surface` is advertised | `zh-metal-surface-probe` enumerates instance extensions | advertised (spec 1), among 19; `VK_MVK_macos_surface` is there too, deprecated |
| `dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr")` finds the loader, and it resolves `vkCreateMetalSurfaceEXT` | the probe uses exactly the seam's two-step lookup | both resolve |
| a `VkSurfaceKHR` can be made from a `CAMetalLayer` | the probe creates one from a layer in no window at all | `VK_SUCCESS` |
| the surface supports what the renderer asks of it | the probe prints the capabilities | queue family 0 presents; `supportedUsageFlags` 0x1F, so `TRANSFER_DST` (needed for readback) is allowed; 60 format/colour-space pairs; 2..3 images |
| swapchain and present | the probe, and then the full spike | `vkCreateSwapchainKHR` `VK_SUCCESS`; present returns `VK_SUBOPTIMAL_KHR` for the windowless layer (`currentExtent` 0x0), `VK_SUCCESS` for the window's |
| `NSApplication`/`NSWindow` on the main thread, event pumping, the mode change | `zh-window-spike-cocoa`, which the runner turned out to be able to run | window created, `WINDOW_EVENT_FOCUS_GAINED` seen, 240 frames presented, `Window_Set_Mode(1024x768)` resized the window and rebuilt the swapchain, read-back centre pixel `131,152,170` is the geometry and not the clear colour, 0 validation messages |

Two things the blind version had wrong, both found this way:

* the `CAMetalLayer` was created with `framebufferOnly = YES`. The renderer's swapchain asks for
  `VK_IMAGE_USAGE_TRANSFER_DST_BIT`, which a framebuffer-only drawable cannot serve;
* the view was made layer-*backed* (`setWantsLayer:` before `setLayer:`), which lets AppKit replace
  the layer with one of its own. Layer-hosting requires the opposite order.

### What a real Mac then measured

Items 1, 2, 3 and 5 below were the runner's gaps; a real Apple Silicon Mac with a Retina display has
since closed most of them. The measurements, the commands and the caveats are in
[macos-hardware-verification.md](macos-hardware-verification.md); in summary, on an M1 Pro with a
`backingScaleFactor` 2.00 built-in display, macOS 26.6.1, AppleClang 16, MoltenVK 1.4.2:

* the window is on-screen, front of its app and `NSWindowOcclusionStateVisible` per
  `CGWindowListCopyWindowInfo`, with the title it was given, and survives `Window_Set_Mode`;
* Retina is *not* half-resolution: 800x600 points -> `contentsScale` 2.00, `drawableSize`
  1600x1200, swapchain `currentExtent` 1600x1200; 1024x768 -> 2048x1536; fullscreen 1728x1117 ->
  3456x2234;
* the scan-code table and the mouse flip are right for synthetic `CGEvent`/`NSEvent` injection
  (`A` -> `0x1E`, `LeftArrow` -> `0xCB`, `F1` -> `0x3B`, wheel -> 120, client corners exact in
  windowed, resized and fullscreen windows) — *synthetic*, not a human at the keyboard;
* borderless fullscreen hides the Dock and the menu bar, and the notch is handled;
* two bugs were found and fixed there: mouse events with `windowNumber == 0` were converted in the
  wrong coordinate space, and fullscreen did not raise the window above the Dock or restore the
  presentation options afterwards.

Still not verified even with the hardware: a desktop screenshot (no Screen Recording grant for the
worker), physical human input, a non-US layout, an external or non-Retina display, a scale change,
`VK_ERROR_OUT_OF_DATE_KHR` from the compositor, cursor-clip feel, and performance. One structural
finding is written up rather than fixed: `Window_Client_Size()` and mouse coordinates are Cocoa
points while the render target is backing pixels, so on a 2x display the engine sees 800x600 for a
1600x1200 framebuffer.

### The runner's gaps, as they stood before that Mac session

1. **that anything is visible.** The runner has no attached display. Window ordering, activation
   actually raising the app, and fullscreen covering the menu bar are all uncovered;
2. **Retina.** `backingScaleFactor` is 1 on the runner, so the `contentsScale`/`drawableSize`
   arithmetic that keeps a 2x display from rendering at half resolution has never executed with a
   scale of 2;
3. **keyboard and mouse.** Nobody types on a runner. The `kVK_*` -> set-1 table is now proven
   against HIToolbox's constants, but not against a real keypress, and *no* mouse event has been
   translated: the bottom-left-to-top-left flip in `Translate_Event` is unexercised, as is the
   scroll wheel;
4. **cursor semantics.** `Window_Set_Cursor_Clip` uses `CGAssociateMouseAndMouseCursorPosition`
   because macOS has no `ClipCursor`; that is a deliberate behaviour difference, and untested;
5. **display changes.** A resize was exercised; a monitor change, a scale change and a
   `VK_ERROR_OUT_OF_DATE_KHR` from the compositor were not;
6. **a non-US layout.** The table is positional, so it should not matter, and nothing has checked
   that it does not.

### How a Mac session covers the rest

```
spikes/renderer/tools/macos-window-check.sh                 # build + 240 frames + pixel check
spikes/renderer/tools/macos-window-check.sh --interactive    # window stays up; try the keyboard
spikes/renderer/tools/macos-window-check.sh --allow-no-display --no-sdl2   # what CI runs
```

Self-contained: it checks the toolchain, locates MoltenVK's ICD manifest and exports the loader
environment (Homebrew's `molten-vk` is keg-only, and without `VK_ICD_FILENAMES` the loader has no
driver and blames the backend), runs the scan-code check, builds the Cocoa backend and the surface
probe, runs the probe, runs the window spike, and then runs the *same driver* on the SDL2 backend as
a control so a failure can be attributed to the Cocoa backend rather than to the Mac. One
`SUMMARY: PASS`/`SUMMARY: FAIL` line at the end, exit status to match. Only `--interactive` on a
login session with a display covers items 1-3 and 6 above.

## 5. What the seam PR did not do

The state this section described — "the engine still calls `PeekMessage`, nothing in `GeneralsMD/`
or `Core/` uses the seam" — is what [section 6](#6-the-engine-wiring) changed for everything
except the renderer. The plan it laid out is kept here because the two rows that are still open
(the renderer's `HWND` and IME) are still true, and because the cost estimates are worth checking
against what the wiring actually took.

The remaining work to actually replace the Win32 loop, in the order it has to happen:

| Step | Where | Cost |
|---|---|---|
| Split `WndProc`'s 217 `WM_*` cases into handlers that take a `WindowEvent` | `WinMain.cpp` | the bulk of it. Mechanical for input, judgement for the re-entrant cases (`WM_PAINT` during load, `WM_ACTIVATEAPP` during device reset) |
| Replace the pump and the `IsIconic` idle loop | `Win32GameEngine.cpp` | small — 13 lines, and `Window_Poll_Event` keeps the loop shape |
| Feed `KeyboardIO` from `WINDOW_EVENT_KEY_*` instead of DirectInput | `Win32DIKeyboard.cpp`, `Keyboard.cpp` | moderate. Set-1 codes match already; the layout-dependent `GetKeyboardLayout` character mapping does not, and needs the `WINDOW_EVENT_TEXT` path |
| Feed `MouseIO` from `WINDOW_EVENT_MOUSE_*`, and capture/clip through the seam | `Win32Mouse.cpp`, `Win32DIMouse.cpp` | moderate. 50 references, but they are concentrated |
| Give `DX8Wrapper` a surface instead of an `HWND`, and route `Toggle_Windowed`/`Set_Device_Resolution` through `Window_Set_Mode` | `dx8wrapper.cpp` | belongs to the renderer slice, not this one |
| `MessageBox` on assert, and the debug dialog | `Debug.cpp`, `Core/Libraries/Source/debug/*` | separate; needs a native message box, not a window |
| IME | `IMEManager.cpp` | separate problem: 44 references to `WM_IME_*`, and macOS IME is `NSTextInputClient`, not a message |

Two known semantic gaps, listed so they are not discovered later: the seam has no
video-mode change (deliberately — the game never used one, see above), and `Window_Key_Is_Down()`
reflects state as of the last pump rather than doing a hardware read the way `GetAsyncKeyState()`
does. The only in-scope caller of `GetAsyncKeyState` is `W3DWaterTracks.cpp` (6 references, debug
code), so that difference is cheap here — but it is a difference.

## 6. The engine wiring

This is the second half: `#ifndef _WIN32` only, Windows untouched. The pump, the window, the
keyboard, the mouse and the assert dialog now go through the seam, and `WndProc` has no
non-Windows counterpart at all — the events are pulled at one point in the frame instead.

| Win32 | Off Windows | File |
|---|---|---|
| `WinMain()` | `main()` | `GeneralsMD/Code/Main/PlatformMain.cpp` (new; `WinMain.cpp` is Windows-only in CMake now) |
| `initializeAppWindows()`: `RegisterClass` + `CreateWindow` + `SetWindowPos` + `SetFocus`/`SetForegroundWindow`/`ShowWindow` | `Window_Create()` | `PlatformWindowHost::createAppWindow()` |
| `WndProc()` | `handleEvent()`, called from the pump | `Core/GameEngine/Source/GameClient/PlatformWindowHost.cpp` |
| `PeekMessage`/`GetMessage`/`TranslateMessage`/`DispatchMessage` | `Window_Poll_Event()` in a `while` | `Win32GameEngine::serviceWindowsOS()` |
| `IsIconic()` idle loop | `Window_Is_Minimised()` | `Win32GameEngine::update()` |
| `TheMessageTime` (`MSG::time`) | `WindowEvent::Time_Ms`, latched per event | `PlatformWindowHost::getMessageTime()` |
| DirectInput 8 keyboard device | `WINDOW_EVENT_KEY_DOWN`/`_UP`, scan code passed through | `Win32DIKeyboard.cpp` |
| `addWin32Event(msg, wParam, lParam, time)` | `addWin32Event(const WindowEvent &)` | `Win32Mouse.{h,cpp}` |
| `ClipCursor()`/`ReleaseCapture()` | `Window_Set_Cursor_Clip()` | `Win32Mouse::capture()`/`releaseCapture()` |
| `GetKeyState(VK_CAPITAL)` | `Window_Modifier_State()` | `Win32DIKeyboard::getCapsState()` |
| `GetKeyboardLayout()` LCID | `LC_ALL`/`LC_CTYPE`/`LANG` prefix `fr` | `Keyboard.cpp` |
| `MessageBox()` | `WWPlatform::Dialog_Message_Box()` | `Debug.cpp`, `platform_dialog.{h,cpp}` (new) |

### The `WM_*` triage

All 20 live cases in `WinMain.cpp`'s `WndProc`, plus the two message-time consumers. "Skirmish or
campaign needs it" means single-player play or start-up breaks without it, not that it is nice to
have.

| `WM_*` case | Single-player? | Off Windows |
|---|---|---|
| `WM_CLOSE` | yes (Alt+F4 opens the quit menu) | `WINDOW_EVENT_CLOSE` -> same body: `MSG_META_DEMO_INSTANT_QUIT`, or `setQuitting()` before the message stream is up |
| `WM_QUERYENDSESSION` | no (logout/shutdown) | folded into `WINDOW_EVENT_CLOSE`; the seam does not distinguish a session ending from a close, and the Win32 body was identical |
| `WM_ACTIVATEAPP` | yes | `WINDOW_EVENT_FOCUS_GAINED`/`_LOST` -> `setActive()`: `setIsActive()` and the custom cursor |
| `WM_ACTIVATE` (`WA_INACTIVE`) | yes | same `setActive()`: audio mute/unmute and `refreshCursorCapture()` |
| `WM_SETFOCUS` | yes | same `setActive()`: `resetKeys()`, `regainFocus()` |
| `WM_KILLFOCUS` | yes | same `setActive()`: `resetKeys()`, `loseFocus()`, `onCursorMovedOutside()`, and the queues are dropped |
| `WM_MOVE` | yes (cursor clip follows the window) | `WINDOW_EVENT_MOVE` -> `refreshCursorCapture()` |
| `WM_SIZE` | yes | `WINDOW_EVENT_RESIZE` -> `refreshCursorCapture()`; `SIZE_MINIMIZED` is `WINDOW_EVENT_MINIMISED`, which the idle loop reads. The `gDoPaint` half is splash-screen only (below) |
| `WM_MOUSEMOVE` | yes | `WINDOW_EVENT_MOUSE_MOVE`, dropped while inactive as before. The client-rect hit test becomes the backend's enter/leave, which is *more* reliable: `WINDOW_EVENT_MOUSE_LEAVE` fires when the pointer leaves, where Win32 only noticed on the next move inside |
| `WM_LBUTTON*`, `WM_MBUTTON*`, `WM_RBUTTON*` (9 cases) | yes | `WINDOW_EVENT_MOUSE_DOWN`/`_UP` with `Mouse_Button` and `Click_Count`; the `DBLCLK` cases become `Click_Count == 2` |
| `WM_MOUSEWHEEL` (`0x020A`) | yes (zoom) | `WINDOW_EVENT_MOUSE_WHEEL`, still dropped when the pointer is outside |
| `WM_KEYDOWN` | **no** | not reproduced. Its only body is `VK_ESCAPE` -> `PostQuitMessage(0)`, and `serviceWindowsOS()` dispatches `WM_QUIT` to `DefWindowProc` (the `GetMessage` return check is commented out), so pressing Escape does nothing on Windows today. Reproducing it would *add* a quit-on-Escape bug. In-game Escape reaches the engine through DirectInput, not this case |
| `WM_SETCURSOR` | yes on Windows only | not reproduced, and not needed: the cursor is set from `setActive()`, and the window is created with `Hide_System_Cursor`, which is what the null-cursor window class did |
| `WM_SYSCOMMAND` (`SC_KEYMENU`, `SC_MOVE`, `SC_SIZE`, `SC_MAXIMIZE`, `SC_MONITORPOWER`) | yes on Windows only | nothing to reproduce: these suppress Windows' own system menu, Alt-halts-the-game and screensaver behaviour. SDL2 does not synthesise them; `SDL_DISABLE_SCREENSAVER` is the equivalent of `SC_MONITORPOWER` and is the backend's business, not the engine's |
| `WM_NCHITTEST` | no | Win32-only non-client geometry; a borderless window has none |
| `WM_POWERBROADCAST` | no | suspend/resume acknowledgement. Not reproduced; a deferred item, not a stub |
| `WM_PAINT`, `WM_ERASEBKGND` | no | the pre-W3D loading splash (`gLoadScreenBitmap`, `BitBlt`). Deliberately dropped: it needs GDI, it stops as soon as W3D initialises (`gDoPaint = false`), and the native window simply starts black |
| `WM_DEVICECHANGE` | no | already `#if 0` in `WinMain.cpp` |
| `WM_IME_*` (44 refs) | no | `TheIMEManager->serviceIMEMessage()` runs before the switch and is untouched. `WINDOW_EVENT_TEXT` is deliberately a no-op case: macOS IME is `NSTextInputClient`, which is its own slice |
| `DEBUG_WINDOWS_MESSAGES` tracing (`messageToString`, ~130 `WM_*` names) | no | Windows-only debug aid, left where it is. It is most of `WinMain.cpp`'s 217 `WM_*` references |

What this costs, stated once: **`DispatchMessage`'s re-entrancy is gone.** On Windows `WndProc`
runs at whatever stack depth the OS chooses, including inside `Reset_Device()` and inside a modal
`MessageBox`. Off Windows the events are applied at one point in `serviceWindowsOS()`. Nothing in
the cases above depends on that — the bodies are idempotent state pokes, and the one case that did
care (`WM_ACTIVATEAPP` calling `Reset_Device`) was already removed as a bugfix upstream. Two
places would notice if they were reached natively, and neither is on the single-player path yet: a
modal assert dialog does not pump, so the window stops repainting while it is up (the portable
dialog is not modal and does not have this problem, because it is not a dialog — see below), and a
long device reset queues events instead of interleaving them.

### Deliberately not reproduced, in one list

| Thing | Why, and what happens instead |
|---|---|
| The loading splash bitmap | GDI `BitBlt` of an 800x600 resource. The native window starts black |
| `MessageBox()` | `platform_dialog.cpp` writes the text to `stderr` and returns a fixed answer (OK, No, Ignore). **It is not a dialog**: no window, no modality, no user choice. An assert that would have offered Abort/Retry/Ignore now continues as if Ignore was clicked. A real `NSAlert`/SDL message box belongs to whoever needs asserts to be interactive |
| `.ANI` animated cursors | `Win32Mouse::loadCursorResources()` returns early; `HCURSOR` and the `.ANI` format are Win32. The game's own software cursor is unaffected |
| Layout-dependent French detection | `GetKeyboardLayout()` reads the *keyboard layout*; the fallback reads the *locale*. A French layout under an English locale is not detected, which mis-picks the key *names* in the control-mapping UI. Behaviourally cheap, and it goes away with `WINDOW_EVENT_TEXT` |
| `SetErrorMode(SEM_FAILCRITICALERRORS)` | no equivalent; nothing suppresses the OS's own error UI |
| `SetUnhandledExceptionFilter()`, `_CrtSetDbgFlag()`, the duplicate-instance `FindWindow()` | Win32-only start-up niceties in `WinMain.cpp`, left out of `PlatformMain.cpp` |

### What was measured

All figures from this session's own runs on Linux, clang 14, `-std=c++20 -m64`:

| Measurement | Before | After |
|---|---:|---:|
| Probe-clean translation units, native | 639 / 742 | 643 / 744 |
| Probe-clean translation units, shimmed | 683 / 742 | 687 / 744 |
| `native-build.json` objects (shimmed, levels 1-2) | 704 / 716 | 708 / 718 |
| Unresolved symbols at link | 273 | 280 |

The four newly clean translation units are `Debug.cpp` (`HWND`), `Keyboard.cpp` (`HKL`), and the
two new files. `Win32Mouse.cpp` and `Win32DIKeyboard.cpp` also compile clean now, checked with the
probe's own flags, but they live in `GameEngineDevice`, which no probe or build level covers yet,
so they do not show up in either total.

Unresolved symbols rose by 7, on purpose: `PlatformWindowHost.cpp` now references eleven
`WWPlatform::Window_*` functions, and `CORE_WWLIB_WINDOW_BACKEND` still defaults to `OFF`, so the
SDL2 backend is not in the build. Turning it on would make the native build depend on SDL2 being
installed, which is a decision for the slice that first links an executable. `Debug.cpp` adds
`FillStackAddresses` and `StackDumpFromAddresses` now that it compiles, since `StackDump.cpp` does
not compile natively yet; the rest of the movement is other files in the same link becoming
compilable, and §3 of `docs/porting/native-build-report.md` is the authoritative list.

### What is not verified

**Nothing in this section has been run.** It compiles on Linux and it is not linked into anything:
the renderer (`dx8wrapper`), the audio device (Miles) and Bink do not compile natively, so there is
no native executable to run. Specifically unverified: whether `createAppWindow()` opens a usable
window in the engine's start-up order, whether the event queue depth is right under load, whether
the mouse coordinates land where the game expects them, and the whole macOS/arm64 path, which has
no compiler in this environment at all.

## Files

| Path | Lines | Executed? |
|---|---:|---|
| `Core/Libraries/Source/WWVegas/WWLib/platform/platform_window.h` | 279 | header only |
| `Core/Libraries/Source/WWVegas/WWLib/platform/platform_window_sdl2.cpp` | 649 | yes, on Linux/X11 |
| `Core/Libraries/Source/WWVegas/WWLib/platform/platform_window_cocoa.mm` | 996 | yes, on a `macos-15` arm64 runner: compiled, window created, 240 frames presented. No display, no keypress |
| `spikes/renderer/src/window_spike.cpp` | 389 | yes, on Linux/X11 and on macOS/arm64 |
| `spikes/renderer/src/metal_surface_probe.mm` | 416 | yes, on a `macos-15` arm64 runner |
| `spikes/renderer/tools/macos-window-check.sh` | — | yes, in `--allow-no-display --no-sdl2` mode; the SDL2 control and `--interactive` are untried |
| `scripts/window-input-scan.py` | 389 | yes |
| `scripts/ci/check-window-scancodes.py` | 106 | yes |
| `Core/GameEngine/Include/GameClient/PlatformWindowHost.h` | 104 | compiled on Linux, never run |
| `Core/GameEngine/Source/GameClient/PlatformWindowHost.cpp` | 397 | compiled on Linux, never run |
| `GeneralsMD/Code/Main/PlatformMain.cpp` | 176 | **compiles only against a stub `Win32GameEngine.h`** — the real header pulls in Miles and d3d8, which do not compile natively yet |
| `Core/Libraries/Source/WWVegas/WWLib/platform/platform_dialog.{h,cpp}` | 75 + 59 | compiled on Linux, never run |
| `scripts/ci/check-window-seam-wiring.py` | 192 | yes |

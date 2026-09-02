# `-xres`/`-yres` off Windows: the window that stayed 800x600

`docs/porting/menu-path-probe.md` §0 recorded a reproducibility trap: the native window was created at
800x600 whatever `-xres`/`-yres` asked for, while `TheDisplay` believed the requested size, so the GUI
laid out for a window that did not exist and every control below logical y=600 — including skirmish's
`Play Game` at 1024x768 — could not be clicked. Every probe since has passed `-xres 800 -yres 600` to
work around it. This slice finds where the size was dropped, restores the one path that applies it,
and measures the result at three sizes.

**Classification: PORT DEFECT (an `#ifdef _WIN32` left around a call whose off-Windows implementation
already existed), not missing data and not an unimplemented path.** The fix deletes two lines; no
dimension is hard-coded, `TheDisplay` is not clamped, and the Windows build compiles the same code it
compiled before.

## 1. Where the size was dropped

The requested size travels intact almost all the way:

| Stage | What it holds | Measured |
|---|---|---|
| `CommandLine.cpp` `-xres`/`-yres` | `TheWritableGlobalData->m_xResolution/m_yResolution` | — |
| `W3DDisplay::init()` → `WW3D::Set_Render_Device(0, getWidth(), getHeight(), …, resize_window=true)` | `TheDisplay` 1024x768 / 1280x720 | `TheDisplay width=1280 height=720` under gdb (§3) |
| `DX8Wrapper::Set_Render_Device()` | `ResolutionWidth/Height` = the request | — |
| `PlatformWindowHost::createAppWindow()` → `WWPlatform::Window_Create()` | `DEFAULT_DISPLAY_WIDTH/HEIGHT` = 800x600 | `xwininfo`: `800x600` before the fix at every `-xres` |

The 800x600 creation size is **not** the defect: `WinMain.cpp` creates the Windows window at the same
`DEFAULT_DISPLAY_WIDTH/HEIGHT`, and on Windows it is `DX8Wrapper::Resize_And_Position_Window()` — called
from `Set_Render_Device()` and `Set_Device_Resolution()` — that afterwards resizes the client area to
`ResolutionWidth x ResolutionHeight` (`GetClientRect` → `AdjustWindowRect` → `SetWindowPos`, centred on
the monitor's work area). That call is the truth for "honoured" on Windows, and it is the call this
port never made:

```cpp
// Core/Libraries/Source/WWVegas/WW3D2/dx8wrapper.cpp, DX8Wrapper::Set_Render_Device(), before
#ifdef _WIN32
	if (resize_window) {
		Resize_And_Position_Window();
	}
#endif
```

The guard came in with the upstream unification of `dx8wrapper.cpp` into `Core` (`6317722e5`), when
nothing off Windows defined `GetClientRect`, `AdjustWindowRect`, `MonitorFromWindow`, `GetMonitorInfo`
or `SetWindowPos`. `platform/window-gdi-and-d3dx-entrypoints` later implemented every one of them in
`WWLib/platform/platform_win32_user.cpp` on top of `Window_Client_Size`, `Window_Frame_Insets`,
`Window_Set_Client_Size` and `Window_Set_Position` — its header comment even names
`Resize_And_Position_Window()` as the caller it serves — but the guard stayed, so the seam was wired
and idle. The second call site, in `Set_Device_Resolution()`, was never guarded; only the startup path
was.

## 2. The fix

The `#ifdef _WIN32` / `#endif` pair around the startup call is removed. `Resize_And_Position_Window()`
now runs off Windows exactly where it runs on Windows, through the seam's Win32 user functions, which
do the client-area arithmetic in **points**: `GetClientRect` reports `Window_Client_Size` (points),
`AdjustWindowRect` adds `Window_Frame_Insets`, and `SetWindowPos` subtracts them again before
`Window_Set_Client_Size`. Nothing in this path sees backing pixels; the backing scale is applied on the
device side of the seam only (`docs/porting/hidpi-scale.md` §1), which is why the Retina rule is not
disturbed.

Rejected alternatives, for the record:

- Passing `m_xResolution/m_yResolution` into `createAppWindow()` would honour the size for the first
  window but leave `Set_Render_Device()` and `Set_Device_Resolution()` disagreeing with Windows about
  who owns the client size, and the in-game resolution change would still need the call.
- Clamping `TheDisplay` to the window would "fix" hit-testing by making the GUI wrong at the size the
  user asked for.

`scripts/ci/check-window-seam-wiring.py` gains a fifth check: every `Resize_And_Position_Window();`
in `dx8wrapper.cpp` must be outside a Windows-only branch. Against `main` before this change it fails
with `1 of 2 Resize_And_Position_Window() calls are inside an #ifdef _WIN32`; with the change,
`2 of 2 … compiled off Windows`.

## 3. Measured, Linux x86-64, X11, SDL2 backend

Retail Zero Hour 1.04 data, `zerohour104_gamedata_full.7z` (sha256 `d9ddd811…` verified), base-game
archives mounted through `STRING_InstallPath`, strict-link binary of this branch, launched
`-win -noshellmap -nologo -noaudio -xres W -yres H`. Window size is what the X server reports for the
SDL window (`xwininfo -root -tree`); `TheDisplay` is read under gdb from the running process; the
hit-test is a real X pointer click (`xdotool mousemove … click 1`) at the on-screen position of a
button, judged by the screen that button opens.

| `-xres -yres` | X11 window (before) | X11 window (after) | `TheDisplay` | Clicks that reached their target |
|---|---|---|---|---|
| 800x600 | 800x600 | **800x600** | 800x600 | Solo Play → Skirmish → the slot combos, `Play Game` (skirmish started, §4 of `skirmish-slot-controls.md`) |
| 1024x768 | 800x600 | **1024x768** | 1024x768 | Solo Play → Skirmish (both buttons lie below y=600, the previously unreachable band) |
| 1280x720 (16:9) | — | **1280x720** | 1280x720 | Solo Play → Skirmish; hover highlight tracked the pointer onto `Credits` before the click |

At 1280x720 the main-menu column and the skirmish screen lay out to the full 16:9 window (the WND
tree scales; the map preview and the eight slot rows are drawn once, in place), and clicks resolve to
the drawn buttons, so the GUI and the window agree on one coordinate space at a non-4:3 aspect.

The window is centred in the work area (SDL reports `+160+247` for 1280x720 on the 1600x1200 display,
`+400+329` for 800x600), which is the `Resize_And_Position_Window()` centring branch running, not
`SDL_WINDOWPOS_CENTERED` from creation: the creation size was 800x600 in every run.

Windows behaviour: the change removes a preprocessor guard; the Windows translation unit is
unchanged token-for-token, so there is nothing to re-measure there.

## 4. Not measured here

- **macOS at backing scale 2.00.** No Apple Silicon machine was reachable from this session. The path
  is the same seam functions the SDL2 run exercised, but with the Cocoa backend's `Window_Client_Size`
  / `Window_Set_Client_Size` (points) — the expected reading is `Cocoa bounds W x H` points and
  `drawableSize 2W x 2H` pixels, as `apple-silicon-verification.md` §8.4 recorded for 800x600. Until
  someone reads that off the M1 Pro with `scripts/macos-input-drive.py`, the Retina result is an
  inference from the seam contract, not a measurement.
- **Fullscreen** (`-win` absent). `Resize_And_Position_Window()`'s fullscreen branch calls
  `SetWindowPos(HWND_TOPMOST, 0, 0, …)`; the seam maps that to `Window_Set_Always_On_Top` and a client
  resize. Not exercised.
- **In-game resolution change** through the Options screen (`Set_Device_Resolution()`). That call was
  never guarded, so its behaviour is unchanged by this slice; whether it worked before is not
  measured.
- Frame counts. As in `menu-path-probe.md` §0.1, screenshots are root-window captures that establish
  the window's contents were composited on screen; they are not a presentation measurement.

## 5. Where this leaves the trap

`-xres 800 -yres 600` is no longer required for any probe; `menu-path-probe.md` §0 is updated to say
so. The renderer is untouched beyond the two deleted lines in `dx8wrapper.cpp` (disclosed in
`concurrent-slices.md`; no draw, resource or present path changed).

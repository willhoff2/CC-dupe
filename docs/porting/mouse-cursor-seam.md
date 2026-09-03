# The mouse-cursor seam

`docs/porting/combat-probe.md` §8.1 found, ranked #2 among player-visible defects, that no mouse
cursor is visible over the native game window. This document is the slice that makes one exist:
the decision between the two ways of doing it, the evidence for the decision, the mechanism, what
is proven on Linux, what is inferred about the Mac, and what remains UNMEASURED.

Numbers in this document were measured on the commit that introduced it. Re-measure before
quoting them; JSON under `docs/porting/ci-baselines/` is the truth for anything CI enforces.

## 1. The defect, step by step

1. `TheGlobalData->m_winCursors` is `TRUE` by default, so `W3DMouse` runs in `RM_WINDOWS`: the
   engine expects the **operating system** to draw the cursor, and `W3DMouse::draw()` draws nothing
   in that mode.
2. On Windows, `Win32Mouse::initCursorResources()` loads every cursor named in its table from
   `data\cursors\<Name>.ANI` (plus `<Name>0`..`<Name>7` for the eight-direction `Scroll` cursor)
   with `LoadCursorFromFile()`, and `Win32Mouse::setCursor()` selects one each frame with
   `SetCursor()`, or `SetCursor(nullptr)` for `NONE`/invisible.
3. Off Windows, before this slice, `initCursorResources()` returned immediately and `setCursor()`'s
   `SetCursor()` call was `#ifdef _WIN32`; the seam's `SetCursor()` in `platform_win32_user.cpp`
   could only hide or show the system pointer, and the pointer had been hidden by the window setup.
   Nothing ever drew a cursor.

## 2. The decision: present the retail `.ANI` as the OS cursor (option 1)

Two ways were on the table:

1. **OS cursor through the seam.** Decode the retail `Data\Cursors\*.ANI` (RIFF `ACON` wrapping
   `.CUR` frames), hand the first frame and its hotspot to the window backend, and let the OS draw
   it — `NSCursor` from an `NSImage` on macOS, `SDL_CreateColorCursor` on Linux.
2. **Renderer-drawn cursor.** Switch the native default redraw mode to `RM_W3D` (a `W3DModel`
   animated model per cursor) or `RM_POLYGON` (a textured quad from the cursor's `Image=` entry in
   `MappedImages`), which `W3DMouse` already knows how to draw.

Measured against the retail Zero Hour 1.04 data (`zerohour104_gamedata_full.7z`, SHA-256
`d9ddd811…ae4`; `Mouse.ini` extracted from `INIZH.big`):

| Measurement | Result | Consequence |
|---|---|---|
| `W3DModel=` entries in `Mouse.ini` | **0** of 27 cursors | `RM_W3D` has no model to draw for any cursor |
| `Image=` names in `Mouse.ini` that exist in `MappedImages` | **1** of 27 (`SCCAttack`) | `RM_POLYGON` would draw a blank quad for 26 cursors |
| `Directions = 8` entries | 1 (`Scroll`) | only the scroll cursor is directional |
| loose `Data/Cursors/*.ani` in the archive | **0** in the `.big` bundle; **52** in `zerohour104_loose_data.7z` — see §6 | the retail data for option 1 exists and decodes (measured on Linux, §6) |

Option 2 is rejected: the renderer's own cursor path is *wired* but the retail data does not feed
it, so switching to it would produce an invisible or blank cursor for 26 of 27 shapes, and fixing
that means authoring assets or changing `W3DMouse` — a renderer change Windows would also see.
Option 1 keeps Windows byte-identical (the `_WIN32` code in `Win32Mouse.cpp` is unchanged; the two
`#ifdef _WIN32`/`#ifndef _WIN32` guards around it are removed, so the same lines now compile on
both), adds no renderer code, and is the smallest seam: three functions.

## 3. Mechanism

The established pattern — a portable implementation under the Win32 spelling so call sites compile
unchanged — applied to the two entry points `Win32Mouse.cpp` already uses.

```
Win32Mouse::initCursorResources()   -- unchanged code, now compiled off Windows too
  LoadCursorFromFile("data\cursors\SCCAttack.ANI")
    platform_win32_user.cpp::LoadCursorFromFileA()
      Path::Open_Stream()          -- case-insensitive, '\' -> '/'
      Cursor_Decode()              -- platform_cursor.cpp: RIFF ACON / ICO -> first frame BGRA + hotspot
      Window_Create_Cursor(image)  -- platform_window.h: backend copies the pixels, returns a handle
Win32Mouse::setCursor()             -- unchanged code, now compiled off Windows too
  SetCursor(handle)
    Window_Set_Cursor(window, backend_cursor)   -- the shape
    Window_Show_System_Cursor(window, true)     -- the visibility
  SetCursor(nullptr)
    Window_Show_System_Cursor(window, false)    -- hidden, as Win32 hides for a null cursor
```

New or changed:

| Where | What |
|---|---|
| `WWLib/platform/platform_cursor.{h,cpp}` | `Cursor_Decode()`: RIFF `ACON` (`anih`, optional `seq `, `LIST fram`/`icon`) around ICO-format `.CUR` frames, and bare `.CUR`. Decodes the **first step's frame** only: 1/4/8/24/32-bit `BI_RGB`, bottom-up DIB, AND mask, palette; 32-bit alpha is authoritative unless all zero, then the mask. Rejects raw-DIB (`AF_ICON` clear) and PNG frames, truncated input, and any non-`ACON` RIFF, always with a reason. |
| `WWLib/platform/platform_window.h` | `CursorImage` (BGRA, top-down, straight alpha, hotspot from the top-left) and `Window_Create_Cursor()` / `Window_Destroy_Cursor()` / `Window_Set_Cursor()`. `Window_Set_Cursor(window, nullptr)` selects the platform's default arrow. Shape and visibility are separate: `Window_Show_System_Cursor()` is untouched. |
| `platform_window_sdl2.cpp` | `SDL_CreateRGBSurfaceWithFormatFrom(…, SDL_PIXELFORMAT_ARGB8888)` (BGRA in little-endian memory) → `SDL_CreateColorCursor()` → `SDL_SetCursor()`; null → `SDL_GetDefaultCursor()`. |
| `platform_window_cocoa.mm` | The content view becomes a `WWGameView : NSView` that owns the current `NSCursor` and answers `-resetCursorRects` with it (the AppKit way of making a cursor stick over a view). `Window_Create_Cursor()` builds an `NSBitmapImageRep` (BGRA → RGBA) → `NSImage` → `NSCursor` with the hotspot. Manual retain/release: the file is built with ARC off. |
| `platform_win32_user.cpp` | `LoadCursorFromFileA()`, `DestroyCursor()`, and `SetCursor()` reworked as above. An `HCURSOR` is a heap `Cursor_Handle` {backend cursor, decoded header, path}. |
| `scripts/native-port-shims/windows.h` | Declares `LoadCursorFromFileA`, `DestroyCursor`, `#define LoadCursorFromFile LoadCursorFromFileA`. |
| `Win32Mouse.cpp` | The `#ifndef _WIN32 return;` in `initCursorResources()` and the `#ifdef _WIN32` around `SetCursor()` are gone. Off Windows the two functions are declared `extern "C"` locally (the file does not include `<windows.h>`). |
| `scripts/ci/check-window-seam-wiring.py` | `LoadCursorFromFile` leaves the WIN32_ONLY list: it is now a seam spelling, like `MessageBox` and `ShowWindow`. |

### Missing data at runtime

`LoadCursorFromFileA()` **never returns null.** If the file is absent, undecodable, or the backend
refuses to make a cursor, the handle comes back without a backend cursor and one line is printed on
stderr (`!!! LoadCursorFromFile("data\cursors\X.ANI"): file not found; the default arrow is shown
instead`). `SetCursor()` on such a handle calls `Window_Set_Cursor(window, nullptr)` — **the
platform's default arrow** — and shows the pointer. Returning null would make `Win32Mouse::setCursor()`
pass null to `SetCursor()`, which *hides* the pointer: exactly the defect this slice removes. So a
native install without `Data/Cursors` gets an arrow everywhere, never a crash and never no cursor.
`DEBUG_ASSERTCRASH` is not reached: the Windows code only asserts on a null return.

## 4. Platform table

| Platform | Cursor shape | Status |
|---|---|---|
| Windows | real `user32` `LoadCursorFromFile`/`SetCursor`; animation by the OS | unchanged, byte-identical source in the `_WIN32` path |
| Linux / SDL2 | `SDL_CreateColorCursor` from the first frame | **compiles and links** (native build, level 4, strict link); behaviour needs a display server — UNMEASURED |
| macOS / Cocoa | `NSCursor` from `NSImage`, set via `-resetCursorRects` on the content view | **MEASURED, Mac / live / real input** (§6.1): visible at fresh launch, main menu and in-mission; `NSCursor.currentSystem` is a 32×32 non-arrow image whose hotspot matches the retail file for each state (13,13 pointer; 15,15 in-mission; 22,16 `SCCScroll0` at the right screen edge); real coordinate clicks land on menu buttons. Hidden-during-cinematic, button-*edge* click and the other seven scroll directions — UNMEASURED (§6.1) |
| Headless test (fake seam) | `win32_user32_test.cpp` records what reached `Window_Create_Cursor`/`Window_Set_Cursor` | **proven** — §5 |

Animation is not presented on any non-Windows platform: the first step's frame is a still cursor.
`CursorFile` carries `Frame_Count`, `Step_Count` and `Display_Rate_Jiffies` so a follow-up can
animate without touching the parser's callers.

## 5. Evidence

### Proven (Linux, unit / synthetic)

`CLANGXX=clang++-14 python3 scripts/native-win32-user32-test.py` — **225 checks, 0 failures**.
The new checks, against fixtures built byte by byte from the RIFF
`ACON` and ICO layouts in the test itself:

- `Cursor_Decode()`: a 32×32 32-bit `.CUR` decodes with the directory's hotspot (5, 7), the XOR
  height (not the doubled DIB height), rows un-flipped to top-down, alpha honoured; a 24-bit `.CUR`
  takes transparency from the AND mask; a 3-frame `.ANI` reports `cFrames` 3, `cSteps` 3,
  `JifRate` 6 and decodes frame 0; a 3-frame `.ANI` with a `seq ` chunk reports `cSteps` 4 and
  decodes the frame the **first step** names (the last one), not frame 0; empty input, junk, a
  truncated file and a `RIFF WAVE` are rejected with a reason.
- `LoadCursorFromFile()` on two synthetic `.ANI` files creates two backend cursors with the decoded
  width, hotspot and pixels; `SetCursor(handle)` reaches `Window_Set_Cursor()` with **that**
  handle's backend cursor and shows the pointer; switching handles selects the other; re-selecting
  the current one is not a seam call; `SetCursor(nullptr)` hides without touching the shape.
- A missing file, an undecodable file and a refusing backend each yield a non-null handle that
  selects the default arrow with the pointer **visible**.
- `DestroyCursor()` destroys only the backend cursors that exist and clears the current selection.
- The retail-set test **SKIPs** and prints why when `$GENERALSMD_PATH/Data/Cursors` and
  `./Data/Cursors` are both absent (see §6). With an empty directory it fails all 34 presence
  checks, which is how the non-skip path was exercised.

Native build: `CLANGXX=clang++-14 python3 scripts/native-build.py --level 1 --level 2 --level 3
--level 4 --with-shims --strict-link` compiles `platform_cursor.cpp` and the reworked
`Win32Mouse.cpp` / `platform_win32_user.cpp` and strict-links (figures in
`docs/porting/ci-baselines/`, unchanged in count by this slice apart from the added source).

### Inferred (not run)

- **Cocoa.** `-[NSView resetCursorRects]` + `-addCursorRect:cursor:` is AppKit's documented route
  to a per-view cursor; `-[NSCursor initWithImage:hotSpot:]` takes the hotspot in the image's
  top-left-origin point space, which is what the decoder produces. The BGRA→RGBA byte swap into a
  non-premultiplied `NSBitmapImageRep` is the ordering AppKit documents for
  `NSCalibratedRGBColorSpace` with alpha last.
- **SDL2.** `SDL_PIXELFORMAT_ARGB8888` is a packed format; on little-endian hosts (every target
  here) its bytes in memory are B, G, R, A — the decoder's output.

### Measured since on the Mac (Wave 12) — see §6.1

Visibility at fresh launch / main menu / in-mission, retail identity by hotspot, one scroll
direction at the right edge, Retina logical size, and coordinate clicks reaching menu buttons.

### UNMEASURED (still)

- The remaining seven `SCCScroll1..7` directions and the swap latency; a click at a menu button's
  *edge* (only centre clicks were driven); the cursor hidden while the game hides it (cinematic /
  `SetCursor(nullptr)`) — no cinematic was reached in the skirmish run.
- `NSCursor` over a fullscreen/borderless `NSWindow` with the MoltenVK layer as content (the run
  used `-win`).
- Whether the decoded retail frames *look* right on a display beyond "a non-arrow 32×32 image is
  shown": pixel-level comparison with the decoded frame is not done.

## 6. MEASURED (Linux, decode only): the retail cursor set

The retail `Data\Cursors\*.ANI` are installed **loose** by the Zero Hour installer. They are **not
in any of the 35 `.big` archives** (`zerohour104_gamedata_full.7z` holds only `.big` files; a direct
parse of every `.big` directory found zero `.ani`/`.cur` entries). `pack-gamedata.py --archives loose`
packs the loose `Data/` tree into `s3://cc-mac-game-data/zerohour104_loose_data.7z` (206,614 bytes,
SHA-256 pinned as `GAMEDATA_LOOSE_SHA256`; see
[`replay-check-gamedata.md`](replay-check-gamedata.md#the-fifth-object-zerohour104_loose_data7z)),
which extracts to `staged/GeneralsMD/Data/Cursors/` with **52** `.ani` files (and the same 52 names
under `staged/Generals/Data/Cursors/`) — 52 files backing the 27 names below, because `SCCScroll` is
eight of them and most shapes ship an `_S` variant. None of them is committed.

The retail-set test in `scripts/native-win32-user32-test.py` was run against that directory on
Linux x86-64 (`clang++-14`, `GENERALSMD_PATH=<staged>/GeneralsMD`): **429 checks, 0 failures**. It
loads each of the 34 names the engine asks for (`Mouse.ini`'s 27 cursors, `Scroll` expanded to
`SCCScroll0..7`) through `LoadCursorFromFile()` → `Cursor_Decode()`, prints what it decoded, and
asserts the hotspot lies inside the frame. Every file is a RIFF `ACON` whose frames are `AF_ICON`
`.CUR` images, **32×32, 4 bits per pixel (16-colour palette) + AND mask**, `JifRate` 10 — so the
"32-bit alpha or palette + mask" question of §5 is answered: palette + mask, which the decoder
already handled (it is the fixture format the synthetic tests also cover).

| Cursor | Frame size | Hotspot (x,y) | Frames | Steps | Rate (jiffies) | bpp | Status |
|---|---|---|---|---|---|---|---|
| `SCCPointer` | 32x32 | (13,13) | 1 | 1 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCNoAction` | 32x32 | (15,15) | 1 | 1 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCSelect` | 32x32 | (15,16) | 8 | 14 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCMove` | 32x32 | (15,15) | 7 | 12 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCAttMov` | 32x32 | (15,15) | 7 | 12 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCAttack` | 32x32 | (15,15) | 8 | 10 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCEnter` | 32x32 | (15,15) | 3 | 3 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCExit` | 32x32 | (15,15) | 3 | 3 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCFriendly` | 32x32 | (15,15) | 8 | 8 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCHostile` | 32x32 | (15,15) | 4 | 6 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCHostile2` | 32x32 | (16,26) | 11 | 11 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCHostile3` | 32x32 | (13,13) | 33 | 33 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCKnifeAttack` | 32x32 | (0,0) | 5 | 5 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCNoBomb` | 32x32 | (15,15) | 1 | 1 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCNoKnife` | 32x32 | (15,15) | 1 | 1 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCPlaceBeacon` | 32x32 | (15,10) | 9 | 9 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCRallyPnt` | 32x32 | (15,15) | 5 | 8 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCRemoteChg` | 32x32 | (6,8) | 12 | 12 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCRepair` | 32x32 | (4,4) | 7 | 12 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCResumeC` | 32x32 | (4,4) | 23 | 23 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCSDIUplink` | 32x32 | (15,16) | 21 | 21 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCSniper` | 32x32 | (15,16) | 20 | 20 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCTNTAttack` | 32x32 | (5,5) | 6 | 6 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCTimedChg` | 32x32 | (6,7) | 11 | 11 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCWaypoint` | 32x32 | (16,16) | 2 | 2 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCCashHack` | 32x32 | (3,3) | 10 | 10 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCScroll0` | 32x32 | (22,16) | 1 | 1 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCScroll1` | 32x32 | (24,24) | 1 | 1 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCScroll2` | 32x32 | (16,22) | 1 | 1 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCScroll3` | 32x32 | (7,24) | 1 | 1 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCScroll4` | 32x32 | (9,16) | 1 | 1 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCScroll5` | 32x32 | (7,7) | 1 | 1 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCScroll6` | 32x32 | (16,9) | 1 | 1 | 10 | 4 | MEASURED (Linux, decode only) |
| `SCCScroll7` | 32x32 | (24,7) | 1 | 1 | 10 | 4 | MEASURED (Linux, decode only) |

Observations from the table, not defects: `SCCKnifeAttack` carries hotspot (0,0) in its directory
entry (that is what the retail file says; the decoder reports it unchanged); the
directional `SCCScroll0..7` hotspots sit on the arrow tips at the frame edges; 22 cursors animate
(`Frames > 1`), and `SCCSelect`, `SCCMove`, `SCCAttMov`, `SCCAttack`, `SCCHostile`, `SCCRallyPnt`,
`SCCRepair` carry a `seq ` chunk (`Steps ≠ Frames`), so the first *step* is what the port shows.
The 18 files the engine never names (`*_S.ani`, `SCCGuard`, `SCCHeal`, `SCCNoEntry`, `SCCOutrange`,
`SCCPlace`, `SCCSell`, `SCCSpyDrone`, `SCCStop`) are not measured.

**What this does not measure:** the cursor on a display. Linux SDL2 visibility remains
**UNMEASURED**; macOS is measured in §6.1.

### 6.1 MEASURED (Mac / live / real input): the cursor over the native window

**M1 Pro, macOS 26.x, 3456×2234 device pixels at backing scale 2× (1728×1117 points), 2026-09-03,
`main` `c6fd1bd7c`, `arch -arm64 ./zh -win`.** Two session-only tools, neither inside the game: a
Swift `NSCursor.currentSystem` reader (logical size, hotspot, `NSBitmapImageRep` pixel and point
size, `isEqual` to `NSCursor.arrow`) run while the OS pointer was over the game window, and the
retail decoder test `python3 scripts/native-win32-user32-test.py` with
`GENERALSMD_PATH=~/devin-work/zh-data` on this Mac: **429 checks, 0 failures**, the same 34-row
table as above (the Mac's install and the S3 bundle decode identically).

| State | Evidence label | Result |
|---|---|---|
| Fresh launch (intro/shell) | Mac / live, screenshot + `NSCursor` | visible; `logical_size=32x32 hotspot=13,13 is_system_arrow=false` = `SCCPointer` (13,13) |
| Main menu | Mac / live, screenshot + `NSCursor` | visible; same 32×32 (13,13) non-arrow image |
| In-mission (over terrain) | Mac / live, screenshot + `NSCursor` | visible; `logical_size=32x32 hotspot=15,15 is_system_arrow=false` — the in-mission set (`SCCNoAction`/`SCCMove`/… all carry (15,15)); `rep[0] pixels=32x32 points=32x32 class=NSBitmapImageRep` |
| Retail identity | Mac / live vs. Linux+Mac decode | the hotspots the OS reports are the decoded files' hotspots, so the image set is the retail set, not the system arrow (pixel-for-pixel comparison of the frame: UNMEASURED) |
| Hotspot / click | Mac / live / real input | coordinate clicks (SKIRMISH, PLAY GAME, Escape menu EXIT GAME / YES, score EXIT, MAIN MENU, EXIT GAME) all activated the button under the pointer tip; a click at a button *edge* pixel: UNMEASURED |
| Scroll direction | Mac / live / real input, pointer dragged to the right screen edge | `logical_size=32x32 hotspot=22,16 is_system_arrow=false` = `SCCScroll0` (22,16) — one direction measured, the other seven UNMEASURED |
| Hidden when the game hides it | — | UNMEASURED: no cinematic in a skirmish, `SetCursor(nullptr)` path not reached |
| Retina | Mac / live | the `NSImage` has a single 32×32-pixel rep at 32×32 points, so AppKit draws it at 32 points = 64 device pixels (2× upscale) — the same physical size as Windows' 32-px cursor on a 1× display; crispness by eye not assessed |

The `CGCursorIsVisible()` API is gone from the current SDK, so "visible" is a screenshot of the
window with the cursor drawn plus a non-nil `NSCursor.currentSystem`; screenshots are session
artefacts, not committed.

The path the game expects at runtime, relative to the game directory (the process's working
directory), is exactly `data\cursors\<Name>.ANI` (`data\cursors\SCCScroll<0-7>.ANI` for the
directional one), with a `<ModDir>\data\cursors\` override checked first when a mod is active. The
seam's `Path::Open_Stream()` makes that case-insensitive and slash-agnostic, so a `Data/Cursors/`
folder copied from a Windows install is found — the loose bundle mixes cases (`sccpointer.ani`,
`SCCSelect.ani`) and all 34 were found.

To re-run the retail row: `aws s3 cp s3://cc-mac-game-data/zerohour104_loose_data.7z .`,
`7z x -ostaged zerohour104_loose_data.7z`, then
`GENERALSMD_PATH=$PWD/staged/GeneralsMD python3 scripts/native-win32-user32-test.py` (on the
project's Mac, `~/devin-work/zh-data` is an install that already has `Data/Cursors`). Without the
data the test says `SKIP: retail cursor set: no Data/Cursors directory …` and passes; it never
fabricates a row.

## 7. Residual risks, ranked

1. **Mac partly measured (§6.1).** Visible, retail image, hotspots matching the files, centre
   clicks landing, one scroll direction, Retina size — seen. Still UNMEASURED: hide-on-cinematic,
   button-edge click, seven scroll directions, fullscreen window.
2. **Retail `.ANI` format assumptions — resolved on Linux (§6).** All 34 engine-named cursors are
   `AF_ICON` 32×32 4-bpp `BI_RGB` frames and decode with in-range hotspots; no fallback to the
   arrow was taken. What remains is whether the decoded pixels look right on screen (UNMEASURED).
3. **No animation.** Windows animates `SCCAttack` and friends; the port shows the first step. A
   follow-up can drive `Window_Set_Cursor()` from `Display_Rate_Jiffies`.
4. **Directional scroll on the Mac.** Eight `NSCursor`s swapped per frame through
   `-invalidateCursorRectsForView:`; `SCCScroll0` was observed at the right edge (§6.1), the other
   seven and the swap latency have not been.
5. **Cursor handle lifetime.** `Win32Mouse` never destroys its cursors (Windows didn't either);
   `DestroyCursor()` exists for completeness and is tested, but nothing calls it at exit.

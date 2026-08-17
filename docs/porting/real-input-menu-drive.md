# Driving the retail shell with real OS input on Apple Silicon

`docs/porting/apple-silicon-verification.md` §8.6 recorded a measurement limitation, not a port defect:
`CGEventPost` succeeded, `MouseIO` never changed, `Window_Is_Active` stayed `false`, 26 windows in front.
The machine has since been granted **Accessibility** and **Screen Recording** for the terminal that hosts
this worker. This document re-measures that limitation, and then drives the real retail shell with real OS
events as far as it goes.

**Headline: the limitation is lifted, and a *user* can reach both mission entries through the real shell.**
A window raised through the Accessibility API becomes key (`Window_Is_Active true`), a `CGEventPost` click
lands in `MouseIO` at the right coordinates in *points*, and the focus-mute clears
(`m_muteReasonBits 1 → 0`). Driven only with posted clicks, drags and keystrokes: Main Menu → Single Player
→ Skirmish → `SkirmishGameOptionsMenu` → `Play Game` reaches `GAME_SKIRMISH` and simulates; Escape → quit
menu → `Yes` reaches `Menus/ScoreScreen.wnd`; `Back` returns to the Main Menu; then Single Player → USA →
Easy reaches `GAME_SINGLE_PLAYER` and a campaign mission that keeps simulating (frame 454 → 1693) while
accepting further in-game clicks. One engine change was needed to get there and it is the only code change
in this PR: `WaterRenderObjClass::Render` dereferenced a null `m_skyBox`. Everything else is reported, not
fixed.

## 0. The machine, the binary, the data

| Thing | Value |
|---|---|
| Machine | Apple M1 Pro, macOS 26.6.1 (Darwin 25.6.0 arm64), display 1728x1117 points at `backingScaleFactor` 2.0 |
| Binary | `build/native/native_strict_link`, built by `scripts/native-build.py --level 1 --level 2 --level 3 --level 4 --with-shims --strict-link`: 979 objects, 0 failures, 0 unresolved symbols, Mach-O 64-bit arm64, 26.4 MiB |
| Run | `zh -win -noshellmap -nologo -xres 800 -yres 600`, client area 800x600 points, window 800x632 points including the title bar |
| Zero Hour data | retail install in a disposable run directory; no retail data committed |
| Harness | `scripts/macos-input-drive.py` (new in this PR) — posts real events through `CGEventPost` and reads engine state through LLDB |

Screenshots are attached to the session, not committed: retail-derived images stay out of the repo.

## 1. What the grant changed, and what it did not

```text
AXIsProcessTrusted             True
CGPreflightPostEventAccess     True
CGPreflightScreenCaptureAccess True
```

Two things had to be true at once, and only one of them is about the grant.

**Activation.** `-[NSRunningApplication activateWithOptions:]` returns `true` and does **not** activate the
game: it stayed non-key with dozens of windows in front, exactly as §8.6 measured. What works is the
Accessibility route — `AXUIElementCreateApplication(pid)`, `AXRaise` on its first window, `AXMain` on that
window, `AXFrontmost` on the application. Every call returns `kAXErrorSuccess`:

```text
== accessibility activation (0 == kAXErrorSuccess)
   copy AXWindows         0
   perform AXRaise        0
   set AXMain             0
   set AXFrontmost        0
```

`Window_Is_Active` then flips `false → true`. This is what the Accessibility grant buys that AppKit does
not: a non-frontmost caller raising another application's window and making it key.

**Timing.** The window server routes a HID event to whatever window is topmost *at the moment it is
posted*. Activating, stopping the process in LLDB to read state, and only then posting loses the window its
key status: the click goes elsewhere and `MouseIO` never changes. The harness therefore raises and posts
from inside the callback that runs while the process is *running*, and reads engine state before and after.
This ordering, not the grant, is what §8.6's negative result would still reproduce.

**The click lands, in points.** A breakpoint on `Mouse::processMouseEvent(int)`, with the click posted from
that breakpoint's own resume path:

```text
hit                    True
hit_count              1
function               Mouse::processMouseEvent(int)
event_x                644
event_y                294
event_left_state       -1
event_right_state      -1
events_this_frame      1
```

Posted at global `(1108,457)`, which is client `(644,294)` in the 800x600 client area — the engine received
the point-space coordinate, not the 1288x588 backing-pixel one. §4.3 measured hit-testing directly, one
layer above the OS event; this went through the OS and agrees.

**The focus-mute lifts.** §5.4 recorded `m_muteReasonBits = 1` because the window never activated. With the
window actually key:

```text
   mute_reason_bits       0
   music_volume           0.550000012
   sound_volume           0.720000029
```

So retail audio is not gated behind a debugger-driven API on this machine any more; the mute was a
consequence of the activation failure, and it clears with it.

**Screenshots are now possible.** `screencapture -l<windowid>` writes a real image of the game window. This
project's first photographs of the retail game running natively are attached to the session. They show the
known-bad frame (§3.3) — magenta/noise composition, black tiles, doubled labels — which is why every claim
below is anchored in engine state instead.

## 2. The click-by-click path

Every step is a real `CGEventPost` event. Coordinates are the client-point centre of the real window
control, read out of the live `GameWindow` tree (`TheShell->m_screenStack[top]->m_windowList`, walked by
`m_nextLayout`); no callback was invoked directly.

| # | Real event | Control | Engine state after |
|---|---|---|---|
| 1 | click `(644,134)` | `ButtonSinglePlayer` | `MapBorder2` hidden, `MapBorder` shown — the Single Player submenu is live |
| 2 | click `(644,294)` | `ButtonSkirmish` | `shell_top Menus/SkirmishGameOptionsMenu.wnd` |
| 3 | click | `ButtonStart` (`Play Game`) | `game_mode 2` (`GAME_SKIRMISH`), `frame` advancing, `window_is_active true`, `mute_reason_bits 0` |
| 4 | drag across the terrain | — | a selection box is drawn; `selected_count` stayed `1`, `frame_selection_changed 0` (see §4.5) |
| 5 | key 53 (Escape) | — | `quit_menu_visible 1` |
| 6 | click | `ButtonYes` | `game_mode 6` (`GAME_NONE`), `shell_top Menus/ScoreScreen.wnd`, score screen advancing frames |
| 7 | click | `ButtonOk` | back to `Menus/SkirmishGameOptionsMenu.wnd` — not to the Main Menu |
| 8 | click | `ButtonBack` | `shell_top Menus/MainMenu.wnd` |
| 9 | click `(644,134)` | `ButtonSinglePlayer` | `MapBorder` shown again |
| 10 | click `(644,134)` | `ButtonUSA` | `MapBorder4` shown, `StaticTextSelectDifficulty` visible — the difficulty menu |
| 11 | click `(644,173)` | `ButtonEasy` | `game_mode 0` (`GAME_SINGLE_PLAYER`), `frame 454` — the campaign mission is loaded and running |
| 12 | click `(400,300)` in the mission | — | `mouse_x 400`, `mouse_y 300`, `mouse_left_event 6`; `frame 1693`, still simulating |

`GameMode` values are from `GeneralsMD/Code/GameEngine/Include/GameLogic/GameLogic.h`:
`GAME_SINGLE_PLAYER 0`, `GAME_SKIRMISH 2`, `GAME_SHELL 4`, `GAME_NONE 6`.

Two notes on reading this. The Main Menu never leaves the shell stack for the Single Player and difficulty
submenus — they are `MapBorderN` sibling layouts inside `MainMenu.wnd`, so `shell_top` staying
`Menus/MainMenu.wnd` is correct behaviour and the *visibility swap* is the transition evidence. And step 7
landing back on the skirmish options menu rather than the Main Menu is the retail flow, not a defect.

So the answer to the slice's question: **yes, a user can reach both mission entries through the shell.**
#103 started them programmatically through `CampaignManager` and `TheSkirmishGameInfo`; this run started
them with clicks a mouse could have made.

## 3. The one wall that was fixed: a null skybox

Where the campaign route stopped, before the fix, under LLDB:

```text
Process stopped
* thread #1, stop reason = EXC_BAD_ACCESS (code=1, address=0x0)
    frame #0: WaterRenderObjClass::Render(RenderInfoClass&) at W3DWater.cpp:1737
   1736 	if (TheGlobalData && TheGlobalData->m_drawSkyBox)
   1737 		m_skyBox->Set_Position(pos);

(lldb) expr m_skyBox
(RenderObjClass *) nullptr
```

`m_skyBox` comes from `Create_Render_Obj("new_skybox", …)` (`W3DWater.cpp:1108`), which returns null when
the asset is not there, and the render path then dereferenced it unconditionally whenever a map enabled the
sky. The guard is one condition, in
`Core/GameEngineDevice/Source/W3DDevice/GameClient/Water/W3DWater.cpp`:

```cpp
if (m_skyBox && TheGlobalData && TheGlobalData->m_drawSkyBox)
```

This cannot change Windows behaviour where the asset loads: `m_skyBox` is non-null there and the condition
is unchanged. Where it does not load, Windows would crash too — the same latent bug, found because this
depot layout does not resolve the asset.

The asset really is missing here, measured in the live process *with* `STRING_InstallPath` pointing at the
base Generals install:

```text
(int) TheArchiveFileSystem->doesFileExist("Art\\W3D\\new_skybox.w3d", 0)                 = 0
(bool) W3DAssetManager::Get_Instance()->Load_3D_Assets("new_skybox.w3d")                 = false
```

`Art\W3D\new_skybox.W3D` does exist inside the base game's `W3D.big`. So the guard is what makes the
campaign mission run, and the sky is simply not drawn — the mission above ran with the guard exercised.

**Superseded:** the lookup above failed because the base game's archives were not mounted at all, not
because the depot lacks the asset. See
[`base-game-install-path.md`](base-game-install-path.md) §1: `loadBigFilesFromDirectory` concatenates
the search mask onto the install path, so a value without a trailing separator mounts nothing. With
that fixed the asset resolves and `m_skyBox` is non-null.

## 4. The walls that were not fixed, classified

### 4.1 `new_skybox.W3D` from the base game is not mounted — resolved, and it was a code defect

Zero Hour mounts the base game's archives via `STRING_InstallPath` (`StdBIGFileSystem::init`, `RTS_ZEROHOUR`
block). The setting was written to the portable store
(`~/Library/Application Support/Command and Conquer Generals Zero Hour/Registry.ini`, section
`[SOFTWARE\Electronic Arts\EA Games\Generals]`, key `STRING_InstallPath`) and Zero Hour's own archives
resolve (`Data\INI\GameData.ini` → 1), but the base game's do not. Whether that is the settings key, the
depot layout, or `loadBigFilesFromDirectory` off Windows is the next slice's question; the guard above means
it no longer costs a mission.

That question is answered in [`base-game-install-path.md`](base-game-install-path.md): it was
`loadBigFilesFromDirectory`, and the classification here — data/config rather than a code defect — was
wrong.

### 4.2 `W3DShroud::getShroudLevel` under river water — port defect, not this slice

The first campaign attempt died here, matching what #103 saw on Linux:

```text
EXC_BAD_ACCESS (KERN_INVALID_ADDRESS)
W3DShroud::getShroudLevel(int, int)
getRiverVertexDiffuse(W3DShroud*, float, float, float, float, float, int)
WaterRenderObjClass::drawRiverWater(PolygonTrigger*)
WaterRenderObjClass::renderWater()
WaterRenderObjClass::Render(RenderInfoClass&)
DefaultStaticSortListClass::Render_And_Clear(RenderInfoClass&)
WW3D::Render_And_Clear_Static_Sort_Lists(RenderInfoClass&)
RTS3DScene::Flush(RenderInfoClass&) → RTS3DScene::Render → WW3D::Render → RTS3DScene::draw
```

`getShroudLevel` reads `m_srcTextureData` with an out-of-range cell (#103 measured `x=48, y=-1`) and its own
`DEBUG_ASSERTCRASH` says the shroud texture can be empty. The USA campaign mission driven above does not
reach it, so it did not block this slice and is left alone: it is a shroud/water bounds defect, and fixing
it needs the shroud slice's context rather than a second guard bolted on from here.

### 4.3 Intermittent recursive death in early startup — port defect

Nine crash reports from this session share one shape: an allocation takes a `CriticalSection`,
`std::recursive_mutex::lock()` fails, `__throw_system_error` builds a `std::string` for the message, that
allocates, and the allocator takes the critical section again — until the stack guard page is hit
(`Thread stack size exceeded due to excessive recursion` / `Could not determine thread index for stack guard
region`):

```text
preMainInitMemoryManager()
operator new(unsigned long)
std::__1::basic_string<...>::basic_string(char const*)
std::__1::system_error::system_error(std::__1::error_code, char const*)
std::__1::__throw_system_error(int, char const*)
std::__1::recursive_mutex::lock()
CriticalSection::enter()
ScopedCriticalSection::ScopedCriticalSection(CriticalSection*)
DynamicMemoryAllocator::allocateBytesDoNotZeroImplementation(int)
operator new(unsigned long)                     ← and around again
```

Two properties matter for the next slice. The failure is in the memory manager's *pre-main* initialization,
so it precedes anything a session can drive. And the recursion is what turns a recoverable mutex error into
a crash: an allocation failure path that allocates cannot report a mutex failure. This is the same family as
§8.5's exit-time stack overflow in OpenAL's static destructors (also reproduced here, `__cxa_finalize_ranges`
→ `SimpleVecClass<Vector4>::~SimpleVecClass` → `DynamicMemoryAllocator::freeBytes`): static-lifetime
allocation outside the memory manager's live window. Not fixed here — it needs the allocator/static-init
seam, not the input seam.

### 4.4 A stale instance-lock holder silently refuses new launches — port defect (lifecycle)

Repeated launch failures were partly one leftover process (PID 23569) still holding
`$TMPDIR/685EAFF2-3216-4265-B047-251C5F4B82F3.lock` on descriptor 3 (`lsof`, and a Python `flock` probe
reported the file busy). `ClientInstance::initialize` takes that `flock` and, in single-instance mode,
returns `false` when it is held — the process then leaves without saying why on stdout or stderr. Killing
the holder was enough for the next launch to reach a live window. The lock itself is correct; the missing
piece is a diagnostic, which belongs with `docs/porting/init-failure-reporting.md`.

### 4.5 Drag selection did not change the selection — unclassified, deliberately

The posted drag (button down, six `kCGEventLeftMouseDragged` steps, button up) draws a selection box, so the
events reach the in-game UI. But `selected_count` stayed `1` and `frame_selection_changed` stayed `0`. That
is consistent with several different causes — the box covering no selectable object on a corrupted-looking
frame, an already-selected object, or an input defect — and this slice has no evidence to choose between
them. It is recorded as an open question rather than classified.

### 4.6 LLDB needed the binary re-signed — measurement limitation (procedure)

`process attach` failed with `Not allowed to attach to process` until the binary was ad-hoc signed with
`com.apple.security.get-task-allow`. Worth doing once per build on this machine; it is not a property of the
port.

### 4.7 Encountered and deliberately not touched — other slices' work

The magenta/noise 2D composition (`Copy_Rects: source and destination formats differ`), `Decode_Fvf:
unsupported FVF 0x0`, the half-resolution HiDPI render, and `spike limit: more than 64 draws per frame`
flooding the log during the mission. They made every screen unreadable, which is exactly why control
coordinates were taken from the live `GameWindow` tree instead of from the picture. Nothing in the 2D
composition path or the swapchain/present sizing code was modified.

## 5. The harness

`scripts/macos-input-drive.py` is the reusable part. It posts real events and reads engine state; it never
calls a GUI callback:

| Command | What it does |
|---|---|
| `capabilities` | `AXIsProcessTrusted`, `CGPreflightPostEventAccess`, `CGPreflightScreenCaptureAccess` |
| `windows` | on-screen window list and the game window's bounds |
| `activate` | the `AXRaise`/`AXMain`/`AXFrontmost` sequence, with each `AXError` |
| `snapshot` | mouse state, shell stack, `GameMode`, frame, selection, mute bits, volumes |
| `buttons` | the live `GameWindow` tree of the top shell layout, with client rects |
| `click` / `move` / `drag` / `key` | post a real event, with a state snapshot either side |
| `post` | post without attaching LLDB, for when a debugger already owns the process |
| `catch-click` | breakpoint on `Mouse::processMouseEvent` and post from its resume path |
| `backtrace` | the current backtrace |
| `screenshot` | `screencapture -l<windowid>` of the game window |

It re-executes itself under Xcode's Python with `PYTHONPATH=$(lldb -P)` when the `lldb` module is not
importable, and reads inline-heavy state through casts (`(int)((W3DGameLogic*)TheGameLogic)->m_gameMode`)
because the accessors are inlined away.

## 6. What the next slice needs

1. **Mount the base game's archives.** §4.1 is the one thing standing between a campaign mission and its
   sky, and probably between other missions and other base-game assets. Measure
   `loadBigFilesFromDirectory(installPath, "*.big")` directly rather than inferring it from a missing model.
2. **The allocator's static-init and teardown window** (§4.3, §8.5). While startup is intermittently fatal,
   every session pays a retry tax, and no measurement of the shell is reliably reproducible.
3. **`W3DShroud::getShroudLevel` bounds under river water** (§4.2), with a map that reaches it.
4. **Selection semantics** (§4.5): drive a drag with a known selectable object under it, with the renderer
   slice's fix in, and settle whether in-game selection works.
5. **Init failure diagnostics** (§4.4): say why the process is leaving.

## 7. Reproducing this

```sh
# once per build on this machine
codesign -s - -f --entitlements <get-task-allow.plist> build/native/native_strict_link

cd <run dir> && ./zh -win -noshellmap -nologo -xres 800 -yres 600 &

python3 scripts/macos-input-drive.py capabilities
python3 scripts/macos-input-drive.py activate  --pid <pid>
python3 scripts/macos-input-drive.py snapshot  --pid <pid>
python3 scripts/macos-input-drive.py buttons   --pid <pid>
python3 scripts/macos-input-drive.py click     --pid <pid> --client 644,134   # Single Player
python3 scripts/macos-input-drive.py click     --pid <pid> --client 644,134   # USA
python3 scripts/macos-input-drive.py click     --pid <pid> --client 644,173   # Easy
python3 scripts/macos-input-drive.py screenshot --pid <pid> --out /tmp/shot.png
```

Both the Accessibility and Screen Recording grants must be held by the terminal that hosts the harness; the
`capabilities` command says whether they are.

## 8. The Windows oracle cannot be built on this host

`./scripts/docker-build.sh --game zh` provisions its image successfully and then dies in `wineboot`, before
compiling anything:

```text
wine: dlls/ntdll/unix/virtual.c:267: anon_mmap_fixed: Assertion `!((UINT_PTR)start & host_page_mask)' failed.
qemu: uncaught target signal 6 (Aborted) - core dumped
```

Wine's 4 KiB page assumption under qemu-x86-64 emulation on a 16 KiB-page arm64 host, i.e. the emulation
layer, not the change under test. On this machine the Windows oracle is therefore GenCI, not the local
gate — the replay-compatibility check depends on that same VC6 build and is equally unavailable here.

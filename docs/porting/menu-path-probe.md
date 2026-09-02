# The menu path off Windows: `MainMenu.wnd`, the WND tree, and the callbacks to a mission

`docs/porting/startup-to-mission-start.md` could reach both mission entries **headless**, but could not
execute the GUI at all: the real WND parser died in text rendering on a device-less renderer. #101 wired
the D3DX creation helpers through `RenderBackendClass`, so this probe re-ran the menu path with a real
window and a real renderer, and took it as far as it goes — parse `MainMenu.wnd`, build the window tree,
click the real callbacks, and start both a campaign mission and a skirmish.

**One engine change was needed and it is in its own commit: `WindowMsgData` was `UnsignedInt` (32-bit)
while the GUI pushes pointers through it, and that truncation segfaults on the FIRST push button of the
FIRST WND file, before a menu can exist. It is widened to `uintptr_t`, which is the same 32-bit type on
the 32-bit Windows build. Nothing else was patched, stubbed or worked around; every failure below is
reported, not fixed.**

**Headline: the real Zero Hour main menu now runs natively off Windows at 64-bit and is interactive.
Its six buttons are the real ones parsed from retail `MainMenu.wnd`; hover highlights follow a real
mouse; Solo Play → Campaign → USA → Easy runs the real `MainMenu.cpp` callbacks and loads a campaign
mission, which then dies in the first 3D frames in `W3DShroud::getShroudLevel(x=48, y=-1)` under river
water rendering. Skirmish → the real `SkirmishGameOptionsMenu` → a map picked from a populated retail
map list → `Play Game` reaches `TheGameLogic->isInGame() == TRUE` and simulates (frame 1879 → 2165 over
~10 s), with the HUD, command bar and radar drawn — but the 3D viewport stays black while the renderer
backend refuses every draw past its 64-per-frame cap. So: the menu path and both mission entries are
reachable through the real UI; *visible* gameplay is not, for renderer reasons that belong to the
renderer slice.**

## 0. The box, the binary, the data

Linux x86-64, clang 14 — this is evidence about **64-bit, off-Windows** execution, not about arm64 or
macOS specifically. Nothing here should be quoted as an Apple Silicon result.

| Thing | Value |
|---|---|
| Source | `eef47ec5f` (`main` with #101, #102, #103 merged) plus the one-line `WindowMsgData` commit in this PR |
| Binary | `build/native/native_strict_link`, `scripts/native-build.py --level 1..4 --with-shims --strict-link` |
| Build result | objects 978/978, probe-clean 978, compile failures 0, undefined symbols 0, strict link succeeded, binary produced |
| Zero Hour data | `s3://cc-mac-game-data/zerohour104_gamedata_full.7z`, 1,162,350,139 bytes, sha256 `d9ddd811…` verified; unpacked to a disposable run directory. Nothing repacked, no retail data committed |

```sh
cd <disposable run dir>
DISPLAY=:0 LD_LIBRARY_PATH=.../build/docker/_deps/ffmpeg-lib/lib \
  ./zh -win -noshellmap -nologo -noaudio -xres 800 -yres 600
```

**Reproducibility trap — use `-xres 800 -yres 600`.** The SDL window is created at 800x600 regardless of
`-xres/-yres`, but `TheDisplay` believes the requested size. At `-xres 1024 -yres 768` the GUI lays out
for 1024x768 inside an 800x600 window: hit-testing no longer matches what is on screen and every control
below logical y=600 — including skirmish's `Play Game` — is physically unreachable. That mismatch is a
finding in its own right (the window seam does not honour the requested backbuffer size), but it is not
chased here.

### 0.1 What presentation was and was not measured

Per §3.1 of `docs/porting/renderer-first-frame.md`, no inherited "presented N frames" figure is quoted
here, and this probe does not produce one either. What it measured instead: the menu and in-game screens
below were captured **from the X server's root window**, i.e. the pixels were composited into a real
on-screen window, not read back out of an offscreen framebuffer. That establishes presentation
qualitatively for this run; it is not a frame-count measurement and must not be quoted as one.

## 1. The blocker that actually stopped the menu path: 32-bit `WindowMsgData`

`docs/porting/startup-to-mission-start.md` §6 reported this as a latent defect. It is not latent. It is
the first thing that happens on the menu path.

```cpp
// Core/GameEngine/Include/GameClient/GameWindow.h (before)
typedef UnsignedInt WindowMsgData;
```

```cpp
// Core/GameEngine/Source/GameClient/GUI/Gadget/GadgetPushButton.cpp
void GadgetButtonSetText( GameWindow *g, UnicodeString text )
{
  TheWindowManager->winSendSystemMsg( g, GGM_SET_LABEL, (WindowMsgData)&text, 0 );
}
// ... and in GadgetPushButtonSystem():
case GGM_SET_LABEL:
  window->winSetText( *(UnicodeString*)mData1 );   // GadgetPushButton.cpp:452
```

A stack address cast to a 32-bit integer loses its top half. Measured `mData1` at the dereference:
`4294946176`. Backtrace, on the very first push button of the very first WND file the engine parses
(the control bar, before `MainMenu.wnd` is even reached):

```text
UnicodeString::UnicodeString
GadgetPushButtonSystem            GadgetPushButton.cpp:452
GameWindowManager::winSendSystemMsg
GadgetButtonSetText
GameWindowManager::gogoGadgetPushButton
createGadget / createWindow / parseWindow / parseChildWindows
GameWindowManager::winCreateFromScript
InGameUI::createControlBar  →  InGameUI::init  →  W3DInGameUI::init
GameClient::init  →  GameEngine::init
```

**Classification: port defect, and it blocks the entire menu path.** Every gadget message that carries a
pointer goes through this typedef, so no WND file with a labelled button can be built at 64-bit. The fix
in this PR is a one-line widening to `uintptr_t` plus a comment; on the 32-bit Windows build `uintptr_t`
is the same 32-bit type `UnsignedInt` was, so the oracle's behaviour is unchanged. It is deliberately
isolated in its own commit so the slice that owns this seam can adopt or replace it.

### 1.1 Do the other two reported defects block the menu path?

| Defect | Blocks the menu path? |
|---|---|
| `StdLocalFileSystem` never instantiated (Zero Hour builds `Win32LocalFileSystem`, so `Data\Scripts\…` reaches `open()` verbatim) | **No.** Everything the menu reads — `Window/…wnd`, `.csf` strings, textures, map list — comes out of the `.big` archives, whose lookups are backslash-spelled internally. It still costs the loose `.scb` files: a skirmish start does not find `Data\Scripts\SkirmishScripts.scb` on disk, so its scripts are absent rather than the run failing. |
| `MapCache` misses POSIX-spelled map paths and silently loses `m_isMultiplayer` | **No.** The GUI feeds `MapCache` the Windows-spelled names it stored, so the map list populates and `Play Game` takes the skirmish branch correctly. The defect bites callers that construct paths themselves, which is what the headless probe did. |

Both remain open and unfixed; they are their own slices.

## 2. What the menu actually contains (not "a frame appeared")

With the widening in place, `MainMenu.wnd` parses, the window tree is built, and the screen is drawn with
its retail background art and the Zero Hour logo. Verified **contents**, read off the rendered buttons
and confirmed against the WND tree:

1. Solo Play
2. Multiplayer
3. Load / Replay
4. Options
5. Credits
6. Exit Game

Real input reaches them: moving the physical mouse over a button changes its highlight state, and the
highlight follows the cursor between buttons — so the platform event → `Mouse` → `GameWindowManager`
hit-test chain is connected at the engine layer, not just at the platform layer.

Two cosmetic defects are visible and **not** chased here: button art is missing (buttons draw as plain
outlined rectangles) and every label is drawn twice with a horizontal offset and clipped
(`SOLO PL SOLO PL / AY AY`). That is a GUI text/image-drawing defect in the new backend path, worth its
own slice; it does not impede interaction.

## 3. The callbacks, exercised with real clicks

Each step below was a real mouse click delivered to the window, driving the real `MainMenu.cpp` /
`SkirmishGameOptionsMenu.cpp` callbacks. No callback was simulated.

| Step | Result |
|---|---|
| `Solo Play` | Real submenu opens (Campaign / Skirmish / Challenge) |
| `Campaign` → `USA` | Difficulty menu opens |
| `Easy` | `setupGameStart()` runs, `m_pendingFile` set, mission load starts |
| Campaign mission load | Loads, then **crashes in the first 3D frames** — §4 |
| `Skirmish` | Real `SkirmishGameOptionsMenu.wnd` opens |
| Map list | Populated from the retail `MapCache`: `Alpine Assault`, `Barren Badlands`, `Bitter Winter`, `Bombardment Beach`, `Desert Fury`, … |
| Select map → `Accept` | Selection highlights, propagates, preview image changes |
| `Play Game` | `reallyDoStart()` runs, `TheGameLogic->isInGame() == TRUE`, simulation advances — §5 |

**Answer to #82's question, now measured rather than audited: the single-player menu route is intact off
Windows.** Nothing on it reaches for anything the GameSpy excision cut.

~~One incomplete area on the skirmish screen: the **player-slot controls do not populate**.~~ Superseded:
the empty `Players`/`Color`/`Army` columns and the `None`-only `Team` column seen at this SHA were the
renderer's 64-draw descriptor cap (removed in #119) truncating a 122-draw screen; the symptom is
reproduced on demand with `ZH_RENDER_MAX_DRAWS=64` and a configuration chosen through the real controls
is measured reaching `TheSkirmishGameInfo` and `ThePlayerList` in
[skirmish-slot-controls.md](skirmish-slot-controls.md). Not missing data, not a GUI port defect, not the
GameSpy excision.

## 4. Campaign: reaches the mission, dies rendering river water

USA `Mission01` at Easy loads and then faults deterministically in the first 3D frames:

```text
#0 W3DShroud::getShroudLevel (x=48, y=-1)      W3DShroud.cpp:269
#1 getRiverVertexDiffuse (x=1923, y=-79)       W3DWater.cpp:180
#2 WaterRenderObjClass::drawRiverWater
#3 WaterRenderObjClass::renderWater  →  Render
#5 DefaultStaticSortListClass::Render_And_Clear
#7 RTS3DScene::Flush → Render → draw
#13 W3DView::draw → drawView → Display::drawViews → W3DDisplay::draw
#19 GameClient::update → GameEngine::update → GameEngine::execute
```

The faulting read, with the measured state (`m_numCellsX=118`, `m_numCellsY=80`,
`m_srcTexturePitch=236`, `x=48`, `y=-1`):

```cpp
if (x < m_numCellsX && y < m_numCellsY)      // upper bounds only
{
  UnsignedShort pixel = *(UnsignedShort *)((Byte *)m_srcTextureData + x*2 + y*m_srcTexturePitch);
```

`getRiverVertexDiffuse` divides a river vertex's world coordinate (`y = -79`, i.e. off the north edge of
the shroud grid) by the cell height, producing `y = -1`, and `getShroudLevel` never rejects negative
indices. The read lands 236 bytes *before* the shroud texture data.

**Classification: a pre-existing engine bounds defect (the same missing `>= 0` check is in the code the
Windows build compiles) that is only fatal off Windows.** On Windows `m_srcTextureData` points into a
locked D3D surface inside a larger allocation, so reading one row short is silently harmless; through the
native backend's lock the byte before the mapping is not necessarily readable. It is not missing data and
not a synthetic artefact. Not fixed here: it sits in renderer-owned files.

Not determined: whether every campaign mission with river water does this, and whether the shroud grid is
correctly sized off Windows in the first place (118x80 cells for this map was not cross-checked against
the Windows build).

## 5. Skirmish: the logic runs, the viewport is black

`Play Game` on `Alpine Assault` starts the game: `TheGameLogic->isInGame()` is `TRUE`, and the logic
frame advances **1879 → 2165 over ~10 s**. The HUD, command bar and radar are drawn. The 3D viewport is
**black**, and successive captures ~8 s apart differ by zero pixels.

stderr floods with:

```text
spike limit: more than 64 draws per frame
```

which comes from the backend itself:

```cpp
// spikes/renderer/src/vulkan_backend.cpp:52,3349
constexpr uint32_t kMaxDrawsPerFrame = 64;
...
bool VulkanBackend::Prepare_Draw(...) {
  if (draw_index_ >= kMaxDrawsPerFrame) {
    std::fprintf(stderr, "spike limit: more than %u draws per frame\n", kMaxDrawsPerFrame);
    return false;   // the draw is silently dropped
```

A game frame issues far more than 64 draws, so most of the scene is discarded before it is submitted.

**Classification: renderer/backend limitation — an unimplemented path in the spike backend, not a
gameplay or UI defect.** Do not read the black viewport as "skirmish does not work": the logic side of a
skirmish start is proven, the visible side is not. Not investigated further, per this slice's scope. What
was *not* determined is whether the 64-draw cap is the whole explanation for the black viewport (the HUD
does draw, so draw ordering matters) — the cap is the measured cause of dropped draws, and attributing
the black terrain specifically to it would need the renderer slice.

## 6. Side observation: `-file <map>` under the GUI did not start the map

Headlessly, `-file Maps\MD_USA01\MD_USA01.map` starts the map (that is the whole basis of
`startup-to-mission-start.md`). With a window (`-win -noshellmap`), the same command line showed the
**main menu** for 90 s with no mission and no error, despite `GameEngine.cpp:736-756` appending
`MSG_NEW_GAME` during init. Recorded as an observation only; not chased, and it invalidated an intended
control run (comparing the §4 crash against a direct start of the same map), so no such comparison is
claimed here.

## 7. Where this leaves the path, and what the next slices are

Reachable now, off Windows, at 64-bit, through the real UI: window → `GameEngine::init()` → WND parse →
main menu drawn and interactive → real callbacks → campaign mission load, and → skirmish game start with
live simulation.

Ordered by what blocks *visible* single-player play:

1. **Renderer draw budget.** `kMaxDrawsPerFrame = 64` in the spike backend drops nearly every scene draw.
   Nothing about gameplay can be seen until this is a real command-buffer budget. Renderer slice.
2. **Shroud negative-index read** (`W3DShroud.cpp:269` via `W3DWater.cpp:180`). Deterministically kills a
   campaign mission in its first frames. A bounds check is the obvious fix but it changes the Windows
   build's behaviour on the same input, so it needs the oracle consulted, not a drive-by patch.
3. ~~**Skirmish player-slot controls empty.**~~ Was the #119 draw cap; measured closed in
   [skirmish-slot-controls.md](skirmish-slot-controls.md).
4. **GUI text and button art** (doubled/clipped labels, missing button images). Cosmetic but pervasive.
5. **Window size not honoured** (`-xres/-yres` vs. the 800x600 SDL window) — a real hit-testing hazard.
6. Still open from the previous probe and untouched here: `StdLocalFileSystem` never instantiated,
   `MapCache` POSIX-path misses.

Could not determine here: anything about arm64/macOS specifically; whether the campaign crash generalises
across missions; whether the empty player slots are a data or an initialisation problem; and any frame
count for presentation (§0.1).

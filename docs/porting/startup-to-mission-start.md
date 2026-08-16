# From `GameEngine::init()` to a mission actually starting

`docs/porting/first-native-run-arm64.md` recorded the binary running as far as `GameEngine::init()` and
dying on a null `RenderBackend`. This document takes the next stretch: everything between a completed
`GameEngine::init()` and a campaign or skirmish mission running. It is a probe report, not a change:
the only edits made to the engine were temporary, local, and reverted before this file was committed
(§8 quotes them). Nothing here was stubbed to get further, and no failure was fixed.

**This document was re-measured in full on `8c6dfaab0` (post-#88, #91, #98, #100) against the complete
retail `.big` set. Two of its previous headline findings are superseded — see §0.1. Do not quote the
git history of this file; quote the numbers below, and re-measure before quoting them anywhere else.**

**Headline: off Windows at 64-bit, headless, both a campaign mission and a skirmish now start and
simulate with a populated world. A campaign mission (`Maps\MD_USA01.map`, USA `Mission01` selected
through the real `CampaignManager`) loads 1160 objects on frame 1 and 1388 by frame 98 with 16 players,
and runs 5000+ frames. A skirmish driven through the real `TheSkirmishGameInfo` (USA vs. an Easy AI on
`Alpine Assault`) reaches 224 objects and 5 players, places its starting Command Centers, loads
`SkirmishScripts.scb` (14 scripts for 14 sides) and runs 1800+ frames. What is still blocked is the
GUI: the real WND path dies in text rendering on a device-less renderer, and loose-file lookups off
Windows fail because Zero Hour still instantiates `Win32LocalFileSystem`, which passes `Data\Scripts\…`
to `open()` verbatim.**

## 0. The box, the binary, the data

This probe ran on the Linux x86-64 CI-equivalent box, not on Apple Silicon. It is therefore evidence
about **64-bit, off-Windows** execution, which is what every finding below turns out to be about; it is
not evidence about arm64 or macOS specifically.

| Thing | Value |
|---|---|
| Source | `8c6dfaab0 feat(ci): pack a full game-data object for the native-port probes (#100)` |
| Binary | `build/native/native_strict_link`, ELF x86-64, `scripts/native-build.py --level 1..4 --with-shims --strict-link` |
| Build result | objects 978/978, probe-clean 978, undefined symbols 0, strict link succeeded, binary produced |
| Zero Hour data | `s3://cc-mac-game-data/zerohour104_gamedata_full.7z`, 1,162,350,139 bytes, sha256 `d9ddd811…` verified, 35 `.big` files unpacked to a disposable run directory |

The full set contains the GUI data the previous run of this probe did not have (`WindowZH.big`,
`PatchWindow.big`), plus textures, terrain, audio, speech and music. Nothing was repacked, and no
retail data is committed. Launch:

```sh
cd <disposable run dir>
LD_LIBRARY_PATH=.../build/docker/_deps/ffmpeg-lib/lib ./zh -headless
```

The registry-substitute install path still works unmodified off Windows and is still how Zero Hour
locates the base game's `English.big` for `GlobalLanguage::init()`:

```ini
# ~/.config/CommandAndConquerGeneralsZeroHour/Registry.ini
[SOFTWARE\Electronic Arts\EA Games\Generals]
STRING_InstallPath = /home/ubuntu/zh-data/gen/
```

### 0.1 What this re-measurement supersedes

| Previous finding | Status now |
|---|---|
| "The map loads with **zero objects**; `SidesList` desyncs at its first `UnicodeString` because `WideChar` is 4 bytes off Windows; the parse silently returns success" | **Fixed and confirmed fixed.** #88 landed the disk/wire seam. The same `-file Maps\MD_USA01.map` probe now loads **1160 objects at frame 1, 1379 by frame 2, 1393 by frame 17**, sustained. The old conclusion is dead. |
| "Pooled allocations are 8-mod-16 on LP64; `ScienceStore` init faults on `movaps`" | **Fixed on `main`.** No allocator patch was needed for any run in this document. |
| "The trimmed archives contain no WND data, so the menu path cannot be executed at all" | **Resolved by data.** The full set has WND data; §4 is the result of actually running the real WND parser. |
| "Archive SHA-256s do not match `check-replays.yml`" | **False alarm** — the workflow's in-file constants are stale fallbacks; the repo sets the real values in `vars`. |

## 1. What runs between `GameEngine::init()` and an interactive main menu

Order, from `GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp`:

1. `TheSubsystemList->initSubsystem(...)` for ~35 subsystems (file systems, global data, `TheGameText`,
   science/multiplayer/terrain/road/language INI, audio, function lexicon, module factory, message
   stream, sides, cave, rank, player templates, particles, FX/weapon/OCL/locomotor/special power/
   damage/armor/build assistant, thing factory, upgrade center, **game client**, AI, **game logic**,
   team, crate, player, recorder, radar, victory, meta map, action manager, game state, results queue).
2. `TheSubsystemList->postProcessLoadAll()`.
3. `TheFramePacer->setFramesPerSecondLimit(...)`, the `TheAudio->setOn(...)` calls, `TheNetwork = nullptr`.
4. `TheMapCache = MSGNEW(...) MapCache; TheMapCache->updateCache();`
5. `while (!m_quitting) { update(); TheFramePacer->update(); }`

The initial shell push is commented out upstream, so the menu is **not** reached from `init()`:

```cpp
// load the initial shell screen
//TheShell->push( "Menus/MainMenu.wnd" );
```

It is reached from the frame loop. `GameClient::update()` appends `MSG_FRAME_TICK`, runs the intro
(`Intro.cpp`: `doEALogoMovie()`, `doSizzleMovie()`, `TheDisplay->playMovie(...)`), then:

```cpp
TheShell->showShellMap(TRUE);
TheShell->showShell();
```

`Shell::showShellMap()` starts the shell map as a real game (`MSG_NEW_GAME` with `GAME_SHELL`),
`Shell::showShell()` pushes `Menus/MainMenu.wnd` — and **both return immediately when
`TheGlobalData->m_initialFile` is non-empty**, which is what lets a map be driven without the GUI.
`GameLogic::tryStartNewGame` additionally skips the `MainMenu.wnd` push entirely under `-headless`.

| Stage | Headless? | Evidence |
|---|---|---|
| The ~35 `initSubsystem` calls | **yes** | all complete under `-headless` with full data |
| `MapCache::updateCache()` | **yes** | ran; §3 reads it back by name |
| Frame loop / `MSG_FRAME_TICK` | **yes** | 5000+ frames observed |
| Intro movies | **no** — `TheDisplay->playMovie` | display-owned; skipped headless. No `VideoPlayer::open` call was observed in any headless run |
| `showShellMap()` → `GAME_SHELL` | **yes** | shell map is a logic path; reached `prepareNewGame`/`loadMap`/`ScriptEngine::newMap` headless |
| `InGameUI::init()` → `createControlBar()` → `ControlBar.wnd` | **no** — see §4 | real WND parse runs, then stops in W3D text rendering |
| `showShell()` → `MainMenu.wnd` | **not reached** | `ControlBar.wnd` in `GameClient::init()` stops first (§4) |
| Menu interaction | **no** | needs a window, device keyboard/mouse; `GameClient::init()` skips `createKeyboard()` under `-headless` |
| `W3DDisplay::init()` + `WW3D::Init()` | **no** | non-headless still dies at `MissingTexture::_Init()` on a null `D3DXCreateTexture` device (renderer slice) |

So the logic half of the path to a mission is headless-reachable and now *works*; the presentation half
is not, and `-headless` is a genuine, already-wired bypass rather than something this probe invented.

## 2. `MainMenu.cpp` after the GameSpy excision (#82)

Static reading is unchanged and still holds: the single-player routes are intact
(`ChallengeMenu.wnd`, `MapSelectMenu.wnd`, `SaveLoad.wnd`, `ReplayMenu.wnd`,
`SkirmishGameOptionsMenu.wnd`, `CreditsMenu.wnd`, `LanLobbyMenu.wnd` are all still pushed), and the
campaign entry (`prepareCampaignGame` → `setupGameStart` → `m_pendingFile` → `MSG_NEW_GAME` with
`GAME_SINGLE_PLAYER`, difficulty and rank points) is complete. It still *includes* GameSpy headers and
calls `GameSpyUpdateOverlays()` / `TearDownGameSpy()` guarded on `TheGameSpyPeerMessageQueue`.

**With GUI data in hand the answer is still "statically yes, dynamically unknown" — but for a different
reason than before.** `MainMenu.wnd` is never reached because `ControlBar.wnd` is parsed earlier, in
`GameClient::init()`, and that parse stops in the renderer (§4). So no menu callback has run off
Windows yet, and this is now blocked on the renderer, not on data.

## 3. Starting a skirmish — measured through the real `TheSkirmishGameInfo`

`TheSkirmishGameInfo` is populated only by `SkirmishGameOptionsMenu`, which needs the GUI. To measure
the rest without the renderer, a temporary harness (§8) performed the **non-GUI half** of
`skirmishGameOptionsInit()` / `reallyDoStart()` — the same calls in the same order, no reimplementation:
`SkirmishGameInfo::init()`, `clearSlotList()`, `reset()`, `setLocalIP()`, `enterGame()`, two real
`GameSlot`s (`SLOT_PLAYER` + `SLOT_EASY_AI`), `setMap()`, `TheMapCache->findMap()`, `setMapCRC()` /
`setMapSize()`, `closeOpenSlots()`, `startGame(0)`, `InitRandom(seed)`, then the identical
`MSG_NEW_GAME` + `GAME_SKIRMISH` message `reallyDoStart()` sends. All of that executed:

```
PROBE_SKIRMISH map=Maps\Alpine Assault\Alpine Assault.map
PROBE_SKIRMISH templates usa=2 china=3
PROBE_SKIRMISH mapcache=found players=2 multiplayer=1
SKIRMISH_SCB reached
SKIRMISH_SCB scripts_read=14 skirmishSides=14
MULTIPLAYER_SCB reached
FRAME 198  objects=224 players=5
FRAME 798  objects=225 players=5
FRAME 1798 objects=227 players=5
```

and `GameLogic` then ran `PREPARE_NEW_GAME` → `START_NEW_GAME` → `loadMapINI` →
`W3DTerrainLogic::loadMap` → `SidesList::prepareForMP_or_Skirmish` → slot-to-`ThePlayerList` walk →
`ScriptEngine::newMap`, with the starting buildings placed:

```
PLACE slot=0 startPos=0 building=AmericaCommandCenter
```

Three findings fell out of this, all of them about the seams around the skirmish path rather than the
path itself:

* **`MapCache` keys are Windows-spelled.** `TheMapCache->findMap("Maps/Alpine Assault/Alpine Assault.map")`
  returns `MISSING`; the same map with backslashes returns `found players=2 multiplayer=1`. The map
  still loads either way, so a forward-slash caller silently loses the player count and the
  `m_isMultiplayer` flag — which is exactly the flag `reallyDoStart()` uses to choose `GAME_SKIRMISH`
  over `GAME_SINGLE_PLAYER`. Anything that hands the engine a POSIX-spelled map path off Windows will
  start the wrong kind of game, quietly. Classification: **port hazard**, cheap to fix at the cache.
* **A non-playable `PlayerTemplate` index produces an instant, silent defeat.** The harness first used
  template indices 0 and 1; those are non-playable entries whose `StartingBuilding` is empty, so
  `placeNetworkBuildingsForPlayer()` returned early (`building=EMPTY`) for both slots, no Command
  Center existed, and `ScriptActions::doDefeat` → `ScriptEngine::startEndGameTimer` →
  `GameLogic::exitGame` fired at frame 302. With the real playable templates
  (`getTemplateNumByName("FactionAmerica")` = 2, `"FactionChina"` = 3) the game runs indefinitely.
  This was a **harness artefact**, not a port defect — recorded because it is exactly the failure mode
  a future menuless test rig will hit, and because the engine's only complaint is a `DEBUG_ASSERTCRASH`
  that is compiled out of a release build.
* **`.scb` loading works — but only if the file can be opened at all.** See §5; the `.scb` lookup is
  the clearest port defect this probe found.

Everything after game-info selection is renderer-independent under `-headless`: the load screen, the
mouse hide and the shell-background teardown are display work that headless skips.

## 4. The WND path: the real parser runs, and stops in the renderer

Under `-headless` the engine installs `GameWindowManagerDummy`, which fabricates windows and never
parses a `.wnd` file, so it proves nothing. Two probes were run instead.

**Probe A (rejected as synthetic).** A subclass that called the real `winCreateFromScript()` but kept
dummy windows reached `ControlBar.wnd` and then crashed in `GadgetTextEntrySystem`, because
`window->winGetUserData()` was null on a fabricated window. That is a **synthetic-only** failure and is
recorded here purely so nobody re-reports it as a GUI bug.

**Probe B (the real one).** With the real `W3DGameWindowManager` (`createWindowManager()`) running
headless, the actual WND parser and the actual W3D gadget code execute. The first real `.wnd` file
parsed off Windows is `ControlBar.wnd`, from `InGameUI::init()`, and it gets as far as setting the text
of a push button before dying with no render device:

```
WND_PARSE ControlBar.wnd

Thread 1 "zh" received signal SIGSEGV, Segmentation fault.
SurfaceClass::Lock                      (Core/Libraries/Source/WWVegas/WW3D2/surfaceclass.cpp:219)
Render2DSentenceClass::Build_Sentence_Not_Centered()
Render2DSentenceClass::Build_Sentence()
W3DGameWindow::winSetText()
GadgetPushButtonSystem()
GadgetButtonSetText()
GameWindowManager::gogoGadgetPushButton()
createGadget() / createWindow() / parseWindow() / parseChildWindows()
GameWindowManager::winCreateFromScript()
InGameUI::createControlBar() / InGameUI::init() / W3DInGameUI::init()
GameClient::init()
```

Reading: **the WND parser, the layout/gadget construction and the callback wiring all execute off
Windows at 64-bit; the stop is the renderer**, one call short of a font surface, in the same
device-less territory as `MissingTexture::_Init()`. This is *not* evidence that the UI path is broken,
and it is not a data problem — the WND data is present and parsed. It is blocked on the renderer slice.

**One genuine 64-bit defect in the GUI seam was found on the way.** `Core/GameEngine/Include/GameClient/GameWindow.h`
has:

```cpp
typedef UnsignedInt WindowMsgData;
```

and the GUI stuffs pointers through it — compiling the WND path emits
`warning: cast to smaller integer type 'unsigned int' from 'GameWindow *'` (also from
`UnicodeString *` and gadget data pointers). Every window message that carries a pointer truncates it
on LP64. Widening the typedef to `uintptr_t` builds clean and was tried locally to get the probe
moving, but it is a behaviour-affecting change to the GUI seam, so it was **reverted and is reported
here instead**. Classification: **port defect**, small, self-contained, and a prerequisite for any
real GUI interaction.

## 5. `Data\Scripts\*.scb`, and the loose-file lookup defect underneath it

`SidesList.cpp` loads `data\Scripts\SkirmishScripts.scb` for a skirmish map with no scripts of its own,
and `Data\Scripts\MultiplayerScripts.scb` for a multiplayer start, both through
`CachedFileInputStream::open()` → `TheFileSystem->openFile()`. Both call sites were reached (§3).

With the scripts placed at the correct POSIX location (`Data/Scripts/SkirmishScripts.scb`), the open
**fails**. `strace` shows why — the Windows spelling reaches the kernel verbatim:

```
openat(AT_FDCWD, "Data\\Scripts\\SkirmishScripts.scb", O_RDONLY) = -1 ENOENT
openat(AT_FDCWD, "Data\\Scripts\\MultiplayerScripts.scb", O_RDONLY) = -1 ENOENT
openat(AT_FDCWD, "data\\Scripts\\SkirmishScripts.scb", O_RDONLY) = -1 ENOENT
```

Root cause, and it is a clean one: `Core/GameEngineDevice/Source/StdDevice/Common/StdLocalFileSystem.cpp`
already does the right thing — `fixFilenameFromWindowsPath()` replaces `\` with `/` and then walks the
path case-insensitively — but **nothing instantiates it**. `Win32GameEngine.h` still says

```cpp
inline LocalFileSystem *Win32GameEngine::createLocalFileSystem() { return NEW Win32LocalFileSystem; }
```

and `Win32LocalFileSystem::openFile()` passes `filename` straight to `Win32LocalFile::open()`. Grepping
both games: `StdLocalFileSystem` and `StdBIGFileSystem` are referenced only by their own translation
units — they are compiled into the binary (17 symbols present) and never used. The `.big` archives work
because they are opened by bare name in the working directory and matched case-insensitively in memory;
it is only **loose** files with Windows-spelled paths that fail, and `.scb` scripts, `.sav` games,
`Data\Scripts\Scripts.ini`-style loose overrides and user-config writes are all in that class.

Classification: **port defect** (unwired seam, not a missing implementation). Fixing it is one factory
line plus whatever `StdBIGFileSystem` needs, and it should be its own slice.

To measure what happens *after* the open succeeds, the run directory was given disposable copies of the
two scripts under literal backslash-containing filenames — a **data-layout workaround in a throwaway
directory, not a source change and not a port fix**. With the open satisfied:

```
SKIRMISH_SCB scripts_read=14 skirmishSides=14
```

So the `.scb` chunk format parses correctly off Windows at 64-bit after #88 — 14 script lists for 14
skirmish sides — which also answers the previous version of this report's open question about whether
`.scb` survived the wide-character defect. It does.

## 6. The campaign path

`CampaignManager::init()` loads `Data\INI\Campaign` from `INIZH.big`. Beyond skirmish, a campaign start
pulls in the campaign/mission chain (`FirstMission`, `Mission`, `NextMission`, `Map`), difficulty
persisted through `OptionPreferences` into `TheScriptEngine->setGlobalDifficulty()`, rank points carried
in `MSG_NEW_GAME`, `IntroMovie` / `FinalVictoryMovie`, `BriefingVoice`, `ObjectiveLine0..4`,
`UnitNames0..2`, `GeneralName`, and the challenge-campaign fork (`m_isChallengeCampaign` →
`TheChallengeGameInfo`, `ChallengeMenu.wnd`).

A temporary harness (§8) performed the non-GUI half of `MainMenu`'s `setupGameStart()`/`doGameStart()`
campaign route through the **real** `TheCampaignManager` — `setGameDifficulty()`, `setCampaign("USA")`,
`getCurrentMission()`, `getCurrentMap()` into `m_pendingFile`, then the identical `MSG_NEW_GAME` +
`GAME_SINGLE_PLAYER` + difficulty + rank points. Measured:

```
PROBE_CAMPAIGN campaign=usa mission=mission01 map=Maps\MD_USA01\MD_USA01.map movie=MD_USA01 objective0=
PROBE_CAMPAIGN pendingFile=Maps\MD_USA01\MD_USA01.map
PREPARE_NEW_GAME / START_NEW_GAME
FRAME 1    objects=1160 players=16
FRAME 98   objects=1388 players=16
FRAME 598  objects=1299 players=16
FRAME 1998 objects=1296 players=16
FRAME 5318 objects=918  players=16
```

Findings:

* **Campaign mission entry works headless.** Campaign selection, first-mission selection, the mission's
  map, terrain, sides, 16 players, ~1300 objects and script execution all run off Windows at 64-bit.
  Objects decline over time (1388 → 918), i.e. the mission is actually *simulating* — units are dying —
  not sitting still.
* **No generic campaign `.scb` load exists.** Campaign missions carry their scripts inside the map's
  `SidesList`/`PlayerScriptsList` chunks; the only `.scb` loads in the engine are skirmish and
  multiplayer. So the §5 lookup defect does not block a campaign mission's scripts, only skirmish's.
* **The movie/briefing layer was not exercised.** `IntroMovie MD_USA01` is parsed from the campaign INI,
  but no `InGameUI::playMovie`, `Display::playMovie` or `VideoPlayer::open` call occurred in any
  headless run — by construction, since movies are display-owned. `objective0` came back empty for
  `Mission01`, which is expected (objectives are `.csf` label lookups resolved for display).
* **Mission progression, `MissionEnd`, briefings and save/load remain undetermined** — all of them are
  entered from the GUI or the display, both of which are renderer-blocked.

## 7. Input: a real keystroke reaches the engine as a `GameMessage`

Unchanged from the previous measurement and still true on `main`. Measured on Linux/SDL2 at 64-bit with
a harness that stood up `TheNameKeyGenerator`, `TheRankInfoStore`, `ThePlayerList` and `TheCommandList`
and pumped real X events:

```
PlatformWindowHost -> DirectInputKeyboard::getKey() -> Keyboard::createStreamMessages() -> TheMessageStream

raw   frame 343: type=8 scan=0x1E repeat=0 time=5590 mods=0x0
frame 474: MSG_RAW_KEY_DOWN scan=0x1E state=0x02 args=2
frame 474: MSG_RAW_KEY_UP   scan=0x1E state=0x01 args=2
```

Caveats: this is the *raw* end of the stream — the translator chain (`WindowTranslator` 10,
`MetaEventTranslator` 20, `HotKeyTranslator` 25, `PlaceEventTranslator` 30, `GUICommandTranslator` 40,
`SelectionTranslator` 50, `LookAtTranslator` 60, `CommandTranslator` 70, `HintSpyTranslator` 100,
`GameClientMessageDispatcher`) was not exercised and most of it wants a window or GUI. And
`GameMessage`'s constructor dereferences `ThePlayerList` unconditionally, so an input event arriving
before `PlayerList` exists crashes rather than being dropped — a latent port hazard, reported, not
fixed. `GameClient::init()` creates no keyboard under `-headless`, so the mission probes above have no
input by construction.

## 8. The temporary edits, quoted so nobody repeats the work

All reverted; the tree committed with this document is clean (`git status` empty apart from this file).
None of them changed engine behaviour on any existing path — each is env-var gated or a debug-only
parser made available in release:

* `CommandLine.cpp`: a release-build copy of the `RTS_DEBUG`-only `parseFile`, registered as `-file`.
  Its argument must be the **short** map path — `ConvertShortMapPathToLongMapPath()` expands
  `Maps\MD_USA01.map`; passing the long path yields `Maps\MD_USA01\MD_USA01\MD_USA01.map` and silently
  starts nothing.
* `GameEngine.cpp`: `ZH_PROBE_SKIRMISH=<map>` (§3) and `ZH_PROBE_CAMPAIGN=<campaign>` (§6) harnesses,
  each replaying the non-GUI half of the corresponding menu path against the real game-info/campaign
  objects, plus `#include "GameClient/CampaignManager.h"`.
* `GameClient.cpp` + `GameLogic.cpp`: `ZH_PROBE_WND=1` selects the real window manager and pushes the
  shell under `-headless` (§4).
* `GameWindowManager.h`: the rejected synthetic `GameWindowManagerProbe` subclass (§4, probe A).
* `GameWindow.h`: `typedef uintptr_t WindowMsgData` (§4) — a real fix, deliberately **not** kept.

The strict build with all of the above in place stayed clean: 978/978 objects, 0 undefined symbols,
strict link succeeded.

## 9. Verdict per blocker

| # | Blocker | Kind | Owner |
|---|---|---|---|
| 1 | Loose-file lookups pass Windows paths to `open()` verbatim; `StdLocalFileSystem`/`StdBIGFileSystem` exist but are never instantiated, so `Data\Scripts\*.scb`, saves and loose overrides cannot be opened off Windows | **port defect** (unwired seam) | needs a slice; one factory line + `StdBIGFileSystem` review |
| 2 | `WindowMsgData` is 32-bit while the GUI passes pointers through it (`GameWindow*`, `UnicodeString*`) | **port defect** (LP64 truncation) | needs a slice; prerequisite for GUI interaction |
| 3 | Real WND parse of `ControlBar.wnd` dies in `SurfaceClass::Lock()` building button text | **renderer-dependent**, not a UI or data problem | renderer slice |
| 4 | `MainMenu.wnd` and its callbacks (incl. the GameSpy remnants) still never run | blocked by #3 | renderer slice, then a menu slice |
| 5 | `MapCache` is keyed on Windows-spelled paths; a POSIX-spelled path returns `MISSING`, losing `m_isMultiplayer` and hence the skirmish/single-player choice | **port hazard**, silent | small slice |
| 6 | Non-playable `PlayerTemplate` → empty `StartingBuilding` → no Command Center → silent `doDefeat` at frame ~300 | **harness artefact**, but the engine's only warning is compiled out in release | worth a release-visible warning |
| 7 | `GameMessage` ctor dereferences `ThePlayerList` unconditionally; pre-`PlayerList` input crashes | **port hazard** | input/menu slice |
| 8 | Intro/briefing movies, objectives display, `MissionEnd`, save/load | **unimplemented-for-headless / renderer-dependent**; not reached | after the renderer |
| 9 | `SetThreadExecutionState()` warning, ALSA/OpenAL device warnings | benign env gaps | none |

Previously reported and now **closed**: the `SidesList`/`WideChar` desync (#88), the silent
negative-`dataLeft` success (in the same blast radius, re-verified as no longer triggered on `.map` or
`.scb`), the pooled-allocation misalignment, and the "no GUI data" data limit.

## 10. What could not be determined

* Whether the menu callbacks — including `MainMenu.cpp`'s GameSpy remnants — behave correctly, because
  no `.wnd` layout can be finished without a render device.
* Whether the WND *slot controls* produce the same `TheSkirmishGameInfo` contents the §3 harness set by
  hand. The harness used the real objects and the real call order, but a GUI-driven setup is still
  unverified.
* Whether a skirmish or campaign mission can be *won or lost correctly*: scripts run and a defeat path
  was observed (from the artefact in §3/#6), but no intentional victory/defeat condition was tested.
* Mission progression between campaign missions, `MissionEnd`, movie playback, briefings, objectives,
  and save/load — all renderer- or GUI-entered.
* Whether any of this reproduces identically on arm64 macOS. Every mechanism here is 64-bit and
  off-Windows rather than x86-specific, so it should, but this probe did not run there.
* Audio: the runs were headless with OpenAL warnings; nothing about mission audio was measured.

## 11. Scope for the next slice on this subsystem

1. **Wire the `StdDevice` file systems** (blocker 1). Smallest change with the largest reach: it unblocks
   `.scb`, saves, loose data overrides and user-config writes in one go, and it is measurable exactly
   the way §5 measured it.
2. **Widen `WindowMsgData` to a pointer-sized type** (blocker 2) as its own tiny seam, before anyone
   tries to make the GUI interactive.
3. **Key `MapCache` lookups canonically** (blocker 5) so a POSIX path cannot silently downgrade a
   skirmish to single-player.
4. **After the renderer lands a frame**: take the menu/WND slice — `ControlBar.wnd` to completion,
   `MainMenu.wnd`, the translator chain, `MapSelectMenu`, and `SkirmishGameOptionsMenu` driving
   `TheSkirmishGameInfo` for real, which is the last synthetic step in §3.
5. **Then mission outcome**: intentional victory/defeat, `MissionEnd`, campaign progression to
   `Mission02`, and save/load — none of which this probe could enter.

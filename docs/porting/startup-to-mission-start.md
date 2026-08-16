# From `GameEngine::init()` to a mission actually starting

`docs/porting/first-native-run-arm64.md` recorded the binary running as far as `GameEngine::init()` and
dying on a null `RenderBackend`. This document takes the next stretch: everything between a completed
`GameEngine::init()` and a campaign or skirmish mission running. It is a probe report, not a change:
the only edits made to the engine were temporary, local, and reverted before this file was committed
(§7 quotes them in full so the next slice does not have to rediscover them).

Nothing here was stubbed to get further, and no failure was fixed.

**Headline: a single-player map load now executes off Windows at 64-bit — `MSG_NEW_GAME` →
`GameLogic::prepareNewGame` → `startNewGame` → `TerrainLogic::loadMap` → `ScriptEngine::newMap` all
run, terrain dimensions come out right, and the player list is populated — but the map arrives with
zero objects, because the map's `SidesList` chunk desynchronises the file stream at its first
`UnicodeString` and the `ObjectsList` chunk is then never read at all. The parse reports success. That
is the mission-start blocker, and it has nothing to do with the renderer.**

## 0. The box, the binary, the data — and what the data is not

This probe ran on the Linux x86-64 CI-equivalent box, not on Apple Silicon. It is therefore evidence
about **64-bit, off-Windows** execution, which is what every failure below turns out to be about; it is
not evidence about arm64 or macOS specifically. Where a finding could be x86-64-specific it is called
out.

| Thing | Value |
|---|---|
| Source | `e14893c32 feat(port): Build and run the native binary on Apple Silicon (#87)`, clean tree |
| Binary | `build/native/native_strict_link`, ELF x86-64, from `scripts/native-build.py --level 1..4 --with-shims --strict-link` |
| Zero Hour data | `s3://cc-mac-game-data/zerohour104_gamedata_trimmed.7z` → `INIZH.big`, `MapsZH.big`, `W3DZH.big`, `Data/Scripts/{SkirmishScripts.scb,MultiplayerScripts.scb,Scripts.ini}` |
| Generals data | `s3://cc-mac-game-data/generals108_gamedata_trimmed.7z` → `INI.big`, `Maps.big`, `W3D.big`, `English.big` |

Zero Hour alone is **not** enough to initialise: `GlobalLanguage::init()` loads
`Data\English\Language.ini`, which lives in the Generals `English.big`. Zero Hour finds it the way
retail does, through the shared install path, which off Windows means the native settings store:

```ini
# ~/.config/CommandAndConquerGeneralsZeroHour/Registry.ini
[SOFTWARE\Electronic Arts\EA Games\Generals]
STRING_InstallPath = /home/ubuntu/zh-data/gen/
```

`Win32BIGFileSystem::init()` then loads the local `*.big` and, under `RTS_ZEROHOUR`, the Generals ones
from that path. That whole registry-substitute path works off Windows unmodified — a small but real
result, since it is how every Zero Hour install locates its base game.

Launch, from a directory of symlinks so `PlatformMain.cpp`'s `chdir()` lands on the data:

```sh
cd /home/ubuntu/zh-data/extract
LD_LIBRARY_PATH=.../build/docker/_deps/ffmpeg-lib/lib ./zh -headless
```

Two caveats that bound every claim in this document:

1. **The archives do not match the SHA-256s in `.github/workflows/check-replays.yml`.** Zero Hour
   hashes `2d137f6c…` where the workflow expects `6837FE1E…`; Generals hashes `15332b5d…` where it
   expects `37A351AA…`. The contents behave like real retail data (2104 thing templates build, real
   maps load), but they are **unverified** and may be a different pack than CI gates on. Worth
   reconciling before anyone quotes replay results from them.
2. **They are trimmed** — `scripts/ci/pack-gamedata.py` says "No textures, audio or GUI data", and that
   is measurable: no `.big` in either set contains a WND layout (`ENDLAYOUTBLOCK`: 0 in all seven
   archives). There is no `Window/*.wnd` data anywhere on this box. **The main menu, the WND GUI, map
   selection and the skirmish options screen therefore cannot be executed here at all.** That is a data
   limit, not a port finding, and this report never dresses one up as the other.

## 1. What runs between `GameEngine::init()` and an interactive main menu

`GameEngine::init()` finishing is not close to an interactive menu. The order, from
`GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp`:

1. `TheSubsystemList->initSubsystem(...)` for ~35 subsystems (file systems, global data, `TheGameText`,
   science/multiplayer/terrain/road/language INI, audio, function lexicon, module factory, message
   stream, sides, cave, rank, player templates, particles, FX/weapon/OCL/locomotor/special power/
   damage/armor/build assistant, thing factory, upgrade center, **game client**, AI, **game logic**,
   team, crate, player, recorder, radar, victory, meta map, action manager, game state, results queue).
2. `TheSubsystemList->postProcessLoadAll()`.
3. `TheFramePacer->setFramesPerSecondLimit(...)`, the four `TheAudio->setOn(...)` calls, `TheNetwork = nullptr`.
4. `TheMapCache = MSGNEW(...) MapCache; TheMapCache->updateCache();`
5. `while (!m_quitting) { update(); TheFramePacer->update(); }`

Note what is **not** there: the initial shell push is commented out upstream —

```cpp
// load the initial shell screen
//TheShell->push( "Menus/MainMenu.wnd" );
```

— so the menu is not reached from `init()` at all. It is reached from the frame loop.
`GameClient::update()` appends `MSG_FRAME_TICK` every frame, runs the intro
(`Intro.cpp`: `doEALogoMovie()`, `doSizzleMovie()`, `TheDisplay->playMovie(...)`), and only then:

```cpp
TheShell->showShellMap(TRUE);
TheShell->showShell();
```

`Shell::showShellMap()` starts the shell *map* as a real game (`MSG_NEW_GAME` with `GAME_SHELL`, after
`InitRandom(0)`), and `Shell::showShell()` pushes `Menus/MainMenu.wnd` — but **both return immediately
if `TheGlobalData->m_initialFile` is non-empty**. That single condition is what lets a map be driven
without the GUI, and it is what §5 uses.

Per stage, headless or not:

| Stage | Headless? | Evidence / why |
|---|---|---|
| The ~35 `initSubsystem` calls | **yes**, with data | all ran to completion under `-headless`; INI-driven, no device |
| `MapCache::updateCache()` | **yes** | ran; reads maps off the archives |
| Frame loop / `MSG_FRAME_TICK` | **yes** | ran for 900+ frames headless |
| Intro movies | **no**, needs `TheDisplay->playMovie` | display-owned; skipped headless |
| `showShellMap()` → shell map as `GAME_SHELL` | **yes** — reached `prepareNewGame`/`loadMap`/`ScriptEngine::newMap` headless | the shell map is a *logic* path; only its rendering needs a frame |
| `showShell()` → `MainMenu.wnd` | **untestable here** | needs WND data, absent from the trimmed archives (§0) |
| Menu interaction | **no** | needs a window, a device keyboard/mouse and GUI assets; `GameClient::init()` skips `createKeyboard()` entirely when `m_headless` |
| `W3DDisplay::init()` scenes + `WW3D::Init()` | **no** | both are inside `if (!TheGlobalData->m_headless)`; non-headless dies on the null `RenderBackend` (#87, other slice) |

So: the logic half of the path to a mission is headless-reachable; the presentation half is not, and
`-headless` is a genuine, already-wired bypass rather than something this probe invented.

## 2. `MainMenu.cpp` after the GameSpy excision (#82)

Static reading of
`GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/MainMenu.cpp`:

* The single-player routes are **intact**: `ChallengeMenu.wnd`, `MapSelectMenu.wnd`, `SaveLoad.wnd`,
  `ReplayMenu.wnd`, `SkirmishGameOptionsMenu.wnd`, `CreditsMenu.wnd`, `LanLobbyMenu.wnd` are all still
  pushed, and the campaign entry (`prepareCampaignGame` → `setupGameStart` → `m_pendingFile` →
  `MSG_NEW_GAME` with `GAME_SINGLE_PLAYER`, difficulty and rank points) is complete.
* It still **reaches into GameSpy**: `GameSpyOverlay.h`, `PeerDefs.h`, `PeerThread.h`, `BuddyThread.h`,
  `MainMenuUtils.h` are included, and the frame path calls `GameSpyUpdateOverlays()` and
  `TearDownGameSpy()` guarded on `TheGameSpyPeerMessageQueue`.

The honest answer to "is the single-player menu path intact off Windows now" is: **statically yes,
dynamically unknown**, and it cannot be made known with this data — no WND layouts, so
`MainMenu.wnd` never loads and none of those callbacks ever run (§0). The guards look null-safe by
inspection, which is not the same as observed. Any slice that wants this answered needs GUI data
first; that is the single highest-value data request in this whole report.

## 3. What starting a skirmish requires

From `GameLogic::startNewGame` / `tryStartNewGame`
(`GeneralsMD/Code/GameEngine/Source/GameLogic/System/GameLogic.cpp`), in order: pick `TheGameInfo`
(`TheSkirmishGameInfo` when `m_gameMode == GAME_SKIRMISH`, `TheChallengeGameInfo` for a challenge
campaign, the recorder's when replaying, otherwise none); apply superweapon restrictions; reject
duplicate colours; populate random sides/colours/start positions over the slots; build the load screen;
hide the mouse; destroy the shell background; reset the frame and campaign victory state;
`loadMapINI()`; `TheTerrainLogic->loadMap()`; `TheSidesList->prepareForMP_or_Skirmish()`; walk the
occupied slots into `ThePlayerList`; `TheScriptEngine->newMap()`; and load
`Data\Scripts\MultiplayerScripts.scb` where required. `SidesList.cpp` additionally loads
`data\Scripts\SkirmishScripts.scb` for a skirmish map that carries no scripts of its own.

Reachability, measured:

* `TheSkirmishGameInfo` is populated **only** by `SkirmishGameOptionsMenu` — map choice, side, colour,
  team, AI slots. With no WND data that screen cannot run, so **`GAME_SKIRMISH` proper was never
  entered in this probe**. Anything below about skirmish is inference from the single-player map path,
  which shares everything except the game-info source and the slot population.
* Everything after game-info selection is renderer-independent in `-headless`: the load screen, the
  mouse hide and the shell-background teardown are all display work that headless skips, and terrain +
  scripts are pure logic (§5 proves they execute).
* The `.scb` loaders were **not** exercised: `MultiplayerScripts.scb` needs a multiplayer/skirmish
  start, and `SkirmishScripts.scb` is only consulted from inside the `SidesList` parse — which is
  exactly the parse that desynchronises in §5, so control never reaches it. Both files are present on
  disk. Whether the `.scb` chunk format survives the same wide-character defect is **undetermined** and
  is the first thing the next slice should measure, because `ScriptList::ParseScriptsDataChunk` reads
  dictionaries through the same `DataChunkInput`.

## 4. What the campaign path touches that skirmish does not

`CampaignManager::init()` loads `Data\INI\Campaign` (present in `INIZH.big`; the manager initialised
without complaint). Beyond skirmish, a campaign start pulls in: the campaign/mission chain
(`Mission`, `FirstMission`, `NextMission`, `Map`), difficulty persisted through `OptionPreferences` and
pushed into `TheScriptEngine->setGlobalDifficulty()`, rank points carried in `MSG_NEW_GAME`,
`IntroMovie` / `FinalVictoryMovie`, `BriefingVoice`, `ObjectiveLine0..4`, `UnitNames0..2` and
`GeneralName`, plus the challenge-campaign fork (`m_isChallengeCampaign` → `TheChallengeGameInfo`,
`ChallengeMenu.wnd`).

Two findings worth having:

* **The campaign map path is the one that executed.** `TheCampaignManager->getCurrentMap()` feeds
  `m_pendingFile`, and the `-file` probe in §5 drives the identical `GAME_SINGLE_PLAYER` code path with
  a campaign map (`Maps\MD_USA01.map`). So the campaign's *mission entry* is what §5 measures.
* **No generic campaign `.scb` load exists.** Searching the engine sources found `.scb` loads only for
  skirmish and multiplayer scripts; campaign missions carry their scripts inside the `.map`'s
  `SidesList`/`PlayerScriptsList` chunks. Which means the §5 defect does not merely cost a campaign
  mission its objects — **it costs it its scripts too**, and a campaign mission is its scripts.
  Mission progression, objectives, briefings and `MissionEnd` were not reached and are undetermined.

## 5. How far a mission start actually got, and where it stopped

Driving a mission without the GUI needs `-file`, which upstream is `#if defined(RTS_DEBUG)`-only, and
whose argument must be the **short** path — `ConvertShortMapPathToLongMapPath()` expands
`Maps\MD_USA01.map` into `Maps\MD_USA01\MD_USA01.map`, so passing the long path yields
`Maps\MD_USA01\MD_USA01\MD_USA01.map` and silently starts nothing. With a temporary release-build copy
of that parser (§7) and the temporary allocator patch (§6):

```sh
gdb -q -batch -x probe.gdb --args ./zh -headless -file "Maps\\MD_USA01.map"
```

reaches, in order:

```
GameLogic::prepareNewGame( ... gameMode=GAME_SINGLE_PLAYER ... )
GameLogic::startNewGame(...)
W3DTerrainLogic::loadMap(...)
ScriptEngine::newMap(...)
```

and keeps running. State sampled from the live process:

| | frame 600 | frame 900 |
|---|---|---|
| `m_gameMode` | `GAME_SINGLE_PLAYER` | `GAME_SINGLE_PLAYER` |
| `m_loadingMap` | false | false |
| `ThePlayerList->m_playerCount` | 3 | 2 |
| `TheTerrainLogic->m_mapDX / m_mapDY` | 610 / 460 | 610 / 460 |
| `TheGameLogic->m_objList` | **nullptr** | **nullptr** |

Terrain is right, sides and players are real, scripts were handed a new map — and there is not a single
object in the world. `MapObject::getFirstMapObject()` is null too, so nothing was ever created for
`GameLogic` to instantiate.

### The stopping point: the `SidesList` parse eats the rest of the map file

`WorldHeightMap`'s logical-data pass registers parsers for `HeightMapData`, `WorldInfo`, `ObjectsList`,
`PolygonTriggers` and `SidesList`. Logging every `DataChunkInput::openDataChunk()` shows what each pass
actually read:

```
pass 1 (logicalDataOnly): HeightMapData BlendTileData WorldInfo SidesList      <- stops here
pass 2 (terrain):         HeightMapData BlendTileData WorldInfo SidesList ObjectsList PolygonTriggers GlobalLighting WaypointsList
pass 3 (terrain):         HeightMapData BlendTileData WorldInfo SidesList ObjectsList PolygonTriggers GlobalLighting WaypointsList
```

Passes 2 and 3 read all eight chunks because they do **not** register `SidesList` or `ObjectsList` —
unparsed chunks are skipped by seeking, which is exact. Pass 1 does register `SidesList`, and it never
gets to `ObjectsList`. Instrumenting `SidesList::ParseSidesDataChunk`:

```
=== sides parse entry:      version=3  dataSize=119768  dataLeft=119768  chunkStart=2841790  pos=2841790
=== after the sides data:                               dataLeft=-16208214                   pos=3413679
```

The chunk is 119,768 bytes. The parse consumed 571,889 bytes of file and drove `dataLeft` to
**-16,208,214**. Because `dataLeft` is signed and the outer loop only tests
`dataLeft < CHUNK_HEADER_BYTES`, the enclosing `DataChunkInput::parse()` treats the file as finished,
breaks, and **returns `true`**. `ObjectsList`, `PolygonTriggers`, `GlobalLighting` and `WaypointsList`
are never read; nothing throws; the map "loads".

### The root cause: `WideChar` is 4 bytes off Windows and the on-disk field is 2

```
(gdb) print sizeof(WideChar)
$1 = 4
```

`Core/Libraries/Include/Lib/BaseType.h` has `typedef wchar_t WideChar;`. On Windows that is 2 bytes; on
Linux and macOS it is 4. `DataChunkInput::readUnicodeString()` reads `len*sizeof(WideChar)` bytes for a
`len`-character on-disk string and decrements the chunk accounting by the same amount:

```cpp
WideChar *str = theString.getBufferForRead(len);
m_file->read( (char*)str, len*sizeof(WideChar) );
decrementDataLeft( len*sizeof(WideChar) );
```

Exactly one `UnicodeString` is read during that sides parse, `len=7` — 28 bytes consumed where the file
holds 14. From that 14-byte desynchronisation every subsequent length prefix is garbage, so the dict and
`AsciiString` reads run away, which is the 452 KiB over-read and the -16 M `dataLeft`. One 7-character
string costs the map its objects.

This is the **wide-character disk boundary**, which a parallel slice owns, so it is reported and not
touched. Two things that slice may not have on its list: the defect is not confined to `.csf`/replay
files — it is in `DataChunkInput`, so it applies to **every `.map` and `.scb` dict**; and the failure is
silent, because `atEndOfChunk()`/`parse()` interpret a negative `dataLeft` as "done" instead of
"corrupt". The silence is arguably the worse half: a map that loads with no objects looks like a
gameplay bug, not a parser bug.

## 6. The other blocker on the way there: pooled allocations are not 16-byte aligned

Before any of the above, a clean-tree `-headless` run died in `ScienceStore` initialisation on aligned
SSE stores into a freshly pooled object:

```asm
movaps %xmm0,0x10(%rdi)
movaps %xmm0,(%rdi)      <- SIGSEGV
```

`Core/GameEngine/Source/Common/System/GameMemory.cpp` has `#define MEM_BOUND_ALIGNMENT 4` (with an
upstream `/// @todo srj -- make this work for 8`), and `MPSB_DLINK` adds a third pointer to
`MemoryPoolSingleBlock`. On LP64 that header is 24 bytes, so every pooled user pointer is 8-mod-16,
and 64-bit codegen's aligned vector stores fault. Raising the bound to 16 **and** dropping the backlink
got past it; that is a diagnosis, not a fix, and it was reverted.

Classification: **port defect**, in the allocator, and it is on the critical path for everything —
it is x86-64 SSE that faults here, but arm64 has the same 16-byte expectations for `q`-register pair
stores, so this is very likely to bite on Apple Silicon too. It deserves its own slice, and the real
fix is a properly aligned pooled-block header, not a `#define` bump.

## 7. The temporary edits, quoted so nobody repeats the work

Both were reverted; the tree committed with this document is clean. The full diff is reproduced here
rather than committed:

* `GameMemory.cpp`: `#define MEM_BOUND_ALIGNMENT 16` and `#define DISABLE_MEMORYPOOL_MPSB_DLINK 1`
  (§6).
* `CommandLine.cpp`: a release-build copy of the `RTS_DEBUG`-only `parseFile`, registered as `-file` in
  `paramsForEngineInit` (§5).

The strict build with those edits in place stayed clean: 977/977 objects, 977 probe-clean, 0 undefined
symbols, strict link succeeded.

## 8. Input: a real keystroke does reach the engine as a `GameMessage`

The platform seam was verified on the Mac in #87; that says nothing about the engine layer. It is now
connected, measured on Linux/SDL2 at 64-bit with a harness that stood up
`TheNameKeyGenerator`, `TheRankInfoStore`, `ThePlayerList` and `TheCommandList` and pumped real X
events:

```
PlatformWindowHost -> DirectInputKeyboard::getKey() -> Keyboard::createStreamMessages() -> TheMessageStream

raw   frame 343: type=8 scan=0x1E repeat=0 time=5590 mods=0x0
raw   frame 343: type=9 scan=0x1E repeat=0 time=5590 mods=0x0
frame 474: MSG_RAW_KEY_DOWN scan=0x1E state=0x02 args=2
frame 474: MSG_RAW_KEY_UP   scan=0x1E state=0x01 args=2
```

So physical key → `GameMessage` works off Windows. Two caveats: this is the *raw* end of the stream —
the translator chain that turns raw keys into commands (`WindowTranslator` 10, `MetaEventTranslator`
20, `HotKeyTranslator` 25, `PlaceEventTranslator` 30, `GUICommandTranslator` 40, `SelectionTranslator`
50, `LookAtTranslator` 60, `CommandTranslator` 70, `HintSpyTranslator` 100,
`GameClientMessageDispatcher`) was not exercised, and most of it wants a window or GUI. And
`GameMessage`'s constructor dereferences `ThePlayerList` unconditionally, so an input event arriving
before `PlayerList` exists crashes rather than being dropped — a latent port hazard, reported, not
fixed. Also note `GameClient::init()` does not create a keyboard at all under `-headless`, so the
headless mission probe in §5 has no input by construction.

## 9. Verdict per blocker

| # | Blocker | Kind | Owner |
|---|---|---|---|
| 1 | `SidesList` parse desyncs on the first `UnicodeString`; `ObjectsList` never read; map loads with 0 objects and 0 scripts, silently | **port defect** (`WideChar` = 4 bytes off Windows) | the wide-character disk-boundary slice — it may not know `DataChunkInput` and `.map`/`.scb` are in scope |
| 2 | Negative `dataLeft` is read as end-of-file, so a corrupt parse returns success | **port defect**, and the reason #1 is invisible | same slice, or its own tiny one |
| 3 | Pooled allocations 8-mod-16 on LP64; aligned vector stores fault in `ScienceStore` init | **port defect** (allocator) | needs a slice; blocks everything |
| 4 | `-file` is `RTS_DEBUG`-only, and takes only the short map path | **unimplemented path** / ergonomics; it is the only GUI-free way to start a mission | worth making release-available for testing |
| 5 | Null `DX8Wrapper::RenderBackend` | port defect | renderer slice (#87), untouched here |
| 6 | No WND/GUI data → main menu, map select, skirmish options unexecutable | **data problem** (trimmed archive) | needs GUI data from the user's Mac |
| 7 | Zero Hour data alone cannot initialise (`Data\English\Language.ini`) | **data problem**, resolved by the Generals archive + install path | none |
| 8 | Archive SHA-256s do not match `check-replays.yml` | **data problem**, unresolved | worth reconciling before trusting replay numbers |
| 9 | `SetThreadExecutionState()` warning, ALSA/OpenAL device warnings | benign platform/env gaps | none |

## 10. What could not be determined

* Whether `MainMenu.wnd` and its callbacks actually run off Windows — no GUI data (blocker 6). The
  GameSpy remnants in `MainMenu.cpp` are null-guarded by inspection only.
* Whether `GAME_SKIRMISH` proper works: `TheSkirmishGameInfo` is only ever filled by the options
  screen, so it was never populated.
* Whether `.scb` parsing survives the wide-character defect — control never reached either loader.
* Everything past mission entry: objectives, briefings, `MissionEnd`, campaign progression, save/load.
* Whether blockers 1–3 reproduce identically on arm64 macOS. The mechanisms are 64-bit and
  off-Windows, not x86-specific, so they should — but this probe did not run there.
* Whether the map would otherwise be intact: with the parse desyncing at the first dict string, no
  statement can be made about the rest of the map data.

## 11. Scope for the next slice on this subsystem

1. Fix `WideChar`-on-disk in `DataChunkInput` (2-byte on-disk fields, whatever the host `wchar_t` is),
   then re-run the §5 probe and report the object count. That one change plausibly turns "map loads
   empty" into "map loads". It belongs to the wide-character slice, not to a new one.
2. Make `DataChunkInput` fail loudly: a negative `dataLeft`, or a parser that leaves a chunk
   over-consumed, should throw `ERROR_CORRUPT_FILE_FORMAT`, not return success.
3. Align the pooled-block header properly on LP64 (blocker 3) as its own slice.
4. Then, and only with GUI data in hand, take the menu/WND/input-dispatch slice: `MainMenu.wnd` load,
   the translator chain, map select, `TheSkirmishGameInfo`.
5. Ask the user for a GUI-inclusive data pack, and reconcile the archive hashes with
   `check-replays.yml`.

# Is a campaign mission a mission? USA-01 script logic measured natively on Linux

Everything before this document proved the native binary *enters* USA-01 (`startup-to-mission-start.md`,
`mission-frame-corruption.md`) and that skirmish is a game (`playability-probe.md`, `combat-probe.md`).
Nobody had measured whether the map's *scripts* run: whether `ScriptEngine` executes them each frame,
which conditions and actions fire, whether the objective caption, timers, area/destroyed triggers, speech,
music and mission end go anywhere. This document is that measurement, on Linux, headless, native. It
contains exactly one fix (§6) and a ranked residual list (§8).

**Headline: USA-01 is a mission on the native binary.** Over 9,000 `ScriptEngine::update` calls
(logic frames 0..8318) the engine evaluated 4,053,638 script passes and 308,165 condition dispatches
across 239 of the map's 291 scripts, fired 88 distinct scripts (284 true-branch firings, 0 false-branch),
ran the whole scripted intro cinematic (camera cuts, letterbox, screen shake, unit waypointing, deletes),
set and expired 67 timers, played the seven-part mission briefing through `SPEECH_PLAY`, handed control
to the player at frame 2468 (`ENABLE_INPUT`), showed the objective caption `MAP:MD_USA01Objective1a` at
frame 2529 and re-showed it 3600 frames later when the `Rebrief Phase One` timer expired, and then sat
waiting for the player to capture the train depot — which no headless run can do. A synthetic
`doVictory()` / `doDefeat()` at frame 2600 armed the 120-frame end-game timer, which counted down to
`GameLogic::exitGame` at 2719 and `GameLogic::clearGameData` at 2720; no hang. One port defect was
found, and it blocked everything past frame 430 in the debug configuration: the debug allocator's
bounding wall broke 16-byte alignment (§6). It is fixed here; every number in this file is from
re-measurement after the fix.

What is **not** shown, and why, is in §5 and §7: audible playback (dummy audio device), movies (no
movie action on USA-01's script path through `-file`), a *natural* mission end (needs a player), the
score screen / next-mission load (`-file` quits to desktop by design), and any Apple Silicon row.

## 0. Method

| Thing | Value |
|---|---|
| Machine | Linux x86_64 (Ubuntu), 8 vCPU, no GPU |
| Binary | `CLANGXX=clang++-14 python3 scripts/native-build.py --level 1 --level 2 --level 3 --level 4 --with-shims --strict-link --config debug --build-dir build/native-debug`: 981/981 objects, 0 undefined symbols |
| Why debug | `-file <map>` is parsed only under `RTS_DEBUG` (`CommandLine.cpp`); a release binary silently runs the shell map. The probe prints `map loaded` + `game_mode` first so that mistake is visible. |
| Data | retail 1.04 (`zerohour104_gamedata_full.7z`, SHA-256 `d9ddd881…ac1ae4`, verified before extraction), unpacked to a disposable run dir outside the repo; base game `English.big` located through `~/.config/CommandAndConquerGeneralsZeroHour/Registry.ini`. Nothing retail is committed. |
| Route | **Native headless simulation**: `zh -headless -ignoreAsserts -file Maps\MD_USA01.map` under gdb, driven by `scripts/campaign-flow-probe.py` (new). Not lavapipe (no renderer is needed to reach the script engine) and not `scripts/native-sim-probe.py` (that harness links a subset; the script engine only runs inside the full game loop). |
| Audio | `ALSOFT_DRIVERS=null` so OpenAL does not probe ALSA. Headless still constructs the real `MilesAudioManager` over the `AIL_*`→OpenAL seam (the crash in §6 is in `AIL_open_stream`), but with zero sample pools and no provider selected (§4). |
| Base | `main` at `e1f8de610` (this branch), fix applied |

`-ignoreAsserts` is required: the debug binary hits a `ControlBar.cpp:2466` assertion before the
first frame (no control bar in headless) and would `_exit(1)`. Every assertion that fires is recorded
with its first frame (§5.4) instead.

The probe is the launcher and the gdb script in one file. It puts *counting* breakpoints on the engine
entry points listed in `LINKS` (script engine, UI, audio, video, mission end) and one *sampling*
breakpoint on `ScriptEngine::update` that reads `TheGameLogic->m_objList` length, `m_endGameTimer`,
`TheCampaignManager->m_victorious`, the current music track, and every counter/flag/timer in
`TheScriptEngine`. Nothing else calls into the inferior except `--force-end`. It also walks
`TheSidesList` once at `ScriptEngine::newMap` for the **static inventory** (every script, condition and
action the map contains), so "executed 0" can be told apart from "not on this map". The gdb stop cost
holds the logic rate at 5–13 frames/s depending on host load; all figures are in logic frames, never
wall time. If the binary is not the debug build the probe stops at `newMap` with
`wrong map loaded: Maps\ShellMapMD\...` and exit status 3 rather than measuring the shell map.

```sh
# ~/zh-data/run/zh-debug -> build/native-debug/native_strict_link; ALSOFT_DRIVERS=null is the default
# live: 9000 script updates, sample every 300
python3 scripts/campaign-flow-probe.py --run-dir ~/zh-data/run --binary ./zh-debug \
  --frames 9000 --sample-every 300 --json ~/zh-data/cfp-usa01-live.json --summary ~/zh-data/cfp-usa01-live.txt
# synthetic end: call ScriptActions::doVictory() at frame 2600, run on to see what happens
python3 scripts/campaign-flow-probe.py --run-dir ~/zh-data/run --binary ./zh-debug \
  --frames 3400 --force-end victory --force-at-frame 2600 --json ~/zh-data/cfp-usa01-victory.json
```

Evidence labels used below, never blended:

* **LINUX-LIVE** — the map's own scripts running in the native binary, headless, nothing forced.
* **LINUX-SYNTHETIC** — same, plus one gdb call into `ScriptActions::doVictory()`/`doDefeat()`.
* **STATIC** — what the map file contains (inventory walk at `newMap`), not what ran.
* **UNMEASURED** — with the reason.

Frame counter advancing is *not* used as evidence anywhere: every row below names the engine call that
was counted.

## 1. Per-feature table

| Feature | Measured (LINUX-LIVE unless stated) | Classification |
|---|---|---|
| `ScriptEngine::update` runs every logic frame | 9,000 updates over logic frames 0..8318 (update runs in load frames too; first update at frame 0); `ScriptEngine::newMap` ×1 at frame 0 | works |
| Scripts evaluated | 4,053,638 `executeScript` passes over 239 distinct scripts; STATIC inventory 291 scripts / 16 sides (52 are subroutines or disabled-until-enabled) | works |
| Scripts fired | 88 distinct scripts, 284 true-branch firings, 0 false-branch (`m_actionFalse` lists are empty on this map — STATIC: no false actions) | works |
| Conditions dispatched | 308,165 `ScriptConditions::evaluateCondition` + 413,576 engine-internal `TIMER_EXPIRED` + 123,272 `FLAG` + 11,712 `COUNTER` + 219 `CONDITION_TRUE`; 17 condition types evaluated of 23 STATIC types (§2) | works |
| First failing condition | none: every condition type present on the map that is *reachable* was dispatched; the 6 never evaluated (`HAS_FINISHED_SPEECH`, `HAS_FINISHED_AUDIO`, `CAMERA_MOVEMENT_FINISHED`, `NAMED_NOT_DESTROYED`, `NAMED_HAS_FREE_CONTAINER_SLOTS`, `PLAYER_HAS_OBJECT_COMPARISON`) belong to scripts that are only enabled after player progress (§2) | UNMEASURED (needs player) |
| Actions executed | 513 `ScriptActions::executeAction` over 88 action types; 115 action types STATIC (§3) | works |
| First failing action / native stub | none observed: every dispatched action returned and the script advanced; no `DEBUG_CRASH`/assert fired inside `ScriptActions` (§5.4) | works |
| Objective appears | `SHOW_MILITARY_CAPTION` ×6 → `InGameUI::militarySubtitle` ×6 with labels `MAP:MD_USA01CINELocation01/02`, `MAP:MD_USA01Title`, `MAP:Intro_Breifing_01`, `MAP:MD_USA01Objective1a` ×2 (frames 2529 and 6129, the latter from the 3600-frame `Rebrief Phase One` timer); `OBJECT_CREATE_RADAR_EVENT` and `RadarVanPing` spawn fired alongside | works (state); display UNMEASURED (headless: `GameWindowManagerDummy`, no draw) |
| Area-entered triggers | `NAMED_ENTERED_AREA` 29,227 evaluations, `TEAM_INSIDE_AREA_PARTIALLY` 35,992, `PLAYER_HAS_COMPARISON_UNIT_{KIND,TYPE}_IN_TRIGGER_AREA` 71,984 + 35,992 — all evaluated every frame once enabled, none true (no player unit moved) | evaluates; true-branch UNMEASURED (needs player) |
| Unit-destroyed triggers | `NAMED_DESTROYED` 17,996, `PLAYER_ALL_DESTROYED` 8,998, `NAMED_ATTACKED_BY_PLAYER` 26,994, `TEAM_ATTACKED_BY_PLAYER` 17,996, `TEAM_ATTACKED_BY_OBJECTTYPE` 8,998, `NAMED_OWNED_BY_PLAYER` 17,996 — evaluated every frame, none true | evaluates; true-branch UNMEASURED (needs player) |
| Timers count and expire | 67 `setTimer` calls; 62 `SET_MILLISECOND_TIMER` + 5 `SET_TIMER` actions; every cinematic timer seen at a positive value, counted down by 1 per sampled frame, and reported `expired` (value −1) on the expected frame — e.g. `CINE_StartTrack01Delay` 430 set at frame 1, expired at 432; `Rebrief Phase One` set 3601 at 2529, re-set at 6130 after firing `Mission Breifing One` | works |
| Counters / flags | 49 `SET_COUNTER` STATIC, 4 executed + 1 `INCREMENT_COUNTER`; `USA Music Track`=6, `USA Music Attempts` incremented; flags `INTRO_DONE`=1, `BOOL-PlayedUSA06`=1, `BOOL-PlayingMusic?`=1 after frame 2468 | works |
| Subroutines | `CALL_SUBROUTINE` ×13 → `ScriptEngine::callSubroutine` ×13 (USA music manager) | works |
| Speech (`SPEECH_PLAY`) reaches the audio manager | 10 actions → 10 `AudioManager::addAudioEvent` calls (`MisUSA01xBrf01..07`, `SpyCameraDrama` ×3), each returning `AHSV_NoSound` (§4) | reaches manager; audibility UNMEASURED (dummy device) |
| Music (`MUSIC_SET_TRACK`) | 4 actions → 4 `addAudioEvent` returning a real handle → 4 `startStream`/`stream_handle` (`Cin_China01c`, `Game_GLA_09`, `Game_USA_08`, `Game_USA_06`); `MUSIC_TRACK_HAS_COMPLETED` evaluated 8,998× | reaches stream open; audibility UNMEASURED |
| Sound effects (`PLAY_SOUND_EFFECT[_AT]`) | 13 actions → 13 `addAudioEvent`, all `AHSV_NoSound` (§4) | reaches manager; audibility UNMEASURED |
| EVA | `EVA_SET_ENABLED_DISABLED` ×2 executed (frame 0 disables, 2467 re-enables); no EVA *event* was raised because nothing happened to the player's units | dispatch works; EVA event chain UNMEASURED (needs player) |
| Movies / cinematics reach FFmpeg | STATIC: USA-01's scripts contain **no** `PLAY_MOVIE`/`PLAY_MOVIE_FULLSCREEN`/`PLAY_MOVIE_RADAR` action; 0 video actions executed; `Display::playMovie`, `InGameUI::playMovie`, `FFmpegVideoPlayer::open` never hit. The mission's `IntroMovie` (`Mission::m_movieLabel`) is played by `LoadScreen.cpp` on the *campaign* route, which `-file` bypasses | UNMEASURED (route) — decoder itself proven in #102/#106 |
| Forced victory | LINUX-SYNTHETIC: `doVictory()` at frame 2600 → `m_endGameTimer` −1→120, `TheCampaignManager->m_victorious` 0→1, timer sampled 2 at frame 2718, `GameLogic::exitGame` at 2719, `clearGameData` at 2720; process exits (§5.3) | works to `clearGameData` |
| Forced defeat | LINUX-SYNTHETIC: `doDefeat()` at 2600 → same sequence, `m_victorious` stays 0 | works to `clearGameData` |
| Scripted (natural) victory/defeat | STATIC: `VICTORY` ×2, `DEFEAT` ×1 present; 0 executed — they sit behind `Train_Depot_Secured`/`Captured_Radio`/`PLAYER_ALL_DESTROYED` which need the player | UNMEASURED (needs player) |
| Score screen / next mission | `clearGameData` suppresses `ScoreScreen.wnd` when `m_headless`, and quits when `m_initialFile` is set (`-file`), so neither is reachable on this route | UNMEASURED (route) |
| Windows behaviour comparison | none run: the Wine/VC6 build (`./scripts/docker-build.sh`) compiles the fix, but has no `-headless` and its replays already mismatch on unmodified `main`; used as a build gate only | UNMEASURED |
| Apple Silicon | not run (machine reserved) | UNMEASURED |

## 2. Conditions: what the map contains vs what was evaluated

STATIC count is instances in the map's script tree; evaluated is dispatches over the 9,000-update run.
`TIMER_EXPIRED`, `FLAG`, `COUNTER` and `CONDITION_TRUE` are short-circuited inside `ScriptEngine`
and counted there (`evaluateTimer` etc.), the rest go through `ScriptConditions::evaluateCondition`.

| Condition | STATIC | evaluated |
|---|---:|---:|
| `TIMER_EXPIRED` | 128 | 413,576 |
| `CONDITION_TRUE` | 65 | 219 |
| `COUNTER` | 46 | 11,712 |
| `FLAG` | 32 | 123,272 |
| `PLAYER_HAS_COMPARISON_UNIT_KIND_IN_TRIGGER_AREA` | 13 | 71,984 |
| `MUSIC_TRACK_HAS_COMPLETED` | 12 | 8,998 |
| `PLAYER_HAS_COMPARISON_UNIT_TYPE_IN_TRIGGER_AREA` | 7 | 35,992 |
| `NAMED_ENTERED_AREA` | 5 | 29,227 |
| `NAMED_NOT_DESTROYED` | 5 | 0 |
| `TEAM_INSIDE_AREA_PARTIALLY` | 5 | 35,992 |
| `NAMED_ATTACKED_BY_PLAYER` | 3 | 26,994 |
| `HAS_FINISHED_SPEECH` | 2 | 0 |
| `NAMED_DESTROYED` | 2 | 17,996 |
| `NAMED_HAS_FREE_CONTAINER_SLOTS` | 2 | 0 |
| `NAMED_OWNED_BY_PLAYER` | 2 | 17,996 |
| `TEAM_ATTACKED_BY_PLAYER` | 2 | 17,996 |
| `TEAM_DISCOVERED` | 2 | 8,998 |
| `UNIT_HAS_OBJECT_STATUS` | 2 | 17,996 |
| `CAMERA_MOVEMENT_FINISHED` | 1 | 0 |
| `HAS_FINISHED_AUDIO` | 1 | 0 |
| `PLAYER_ALL_DESTROYED` | 1 | 8,998 |
| `PLAYER_HAS_OBJECT_COMPARISON` | 1 | 0 |
| `TEAM_ATTACKED_BY_OBJECTTYPE` | 1 | 8,998 |

The multiples of 8,998 (= evaluations per always-active script after frame 0) show that the active
scripts are evaluated once per logic frame, every frame. The six zero rows are all in scripts that
`ENABLE_SCRIPT` turns on only after `Train_Depot_Secured`, `Inside Base` or `Mission_Phase_Three`
becomes true — none of which a headless run can reach. `HAS_FINISHED_SPEECH`/`HAS_FINISHED_AUDIO`
would be answered by `ScriptEngine::isSpeechComplete`, which derives a frame count from
`TheAudio->getAudioLengthMS`; that path is intact in code but was not exercised here.

## 3. Actions: what the map contains vs what executed

115 action types STATIC, 88 executed, 513 executions. Full table in the probe's summary; the rows that
answer the questions asked of this probe:

| Action | STATIC | executed | Goes to |
|---|---:|---:|---|
| `SET_MILLISECOND_TIMER` / `SET_TIMER` / `SET_RANDOM_MSEC_TIMER` | 144 / 11 / 10 | 62 / 5 / 0 | `ScriptEngine::setTimer` ×67 |
| `CALL_SUBROUTINE` | 109 | 13 | `ScriptEngine::callSubroutine` ×13 |
| `SET_FLAG` / `SET_COUNTER` / `INCREMENT_COUNTER` | 66 / 49 / 1 | 15 / 4 / 1 | `ScriptEngine` state, read back by sampler |
| `ENABLE_SCRIPT` / `DISABLE_SCRIPT` | 63 / 22 | 11 / 0 | script `m_isActive` |
| `SPEECH_PLAY` | 61 | 10 | `AudioManager::addAudioEvent` ×10 |
| `MUSIC_SET_TRACK` / `MUSIC_SET_VOLUME` | 35 / 2 | 4 / 2 | `addAudioEvent` ×4 → stream open ×4 |
| `PLAY_SOUND_EFFECT_AT` / `PLAY_SOUND_EFFECT` | 12 / 8 | 9 / 4 | `addAudioEvent` ×13 |
| `SOUND_DISABLE_TYPE` / `SOUND_ENABLE_TYPE` / `SOUND_ENABLE_ALL` / `SOUND_REMOVE_ALL_DISABLED` / `AUDIO_OVERRIDE_VOLUME_TYPE` / `AUDIO_RESTORE_VOLUME_ALL_TYPE` | 10 / 4 / 1 / 6 / 13 / 2 | 8 / 3 / 1 / 5 / 11 / 1 | `TheAudio` |
| `EVA_SET_ENABLED_DISABLED` | 7 | 2 | `TheEva` |
| `SHOW_MILITARY_CAPTION` | 10 | 6 | `InGameUI::militarySubtitle` ×6 |
| `DISABLE_INPUT` / `ENABLE_INPUT` | 5 / 2 | 1 / 1 | `ScriptActions::doDisableInput` frame 0 / `doEnableInput` frame 2468 |
| `SETUP_CAMERA` / `MOVE_CAMERA_TO` / `ZOOM_CAMERA` / `PITCH_CAMERA` / `RESET_CAMERA` / `CAMERA_LETTERBOX_*` / `CAMERA_FADE_*` / `CAMERA_BW_MODE_*` / `SCREEN_SHAKE` | 21 / 7 / 2 / 1 / 1 / 8 / 12 / 2 / 23 | 16 / 4 / 2 / 1 / 1 / 3 / 8 / 2 / 11 | `TheTacticalView` (dummy in headless) |
| `NAMED_*` / `TEAM_*` unit orders (follow waypoints, garrison, guard, delete, kill, load transports, panic, wander, ability use) | 117 | 121 | `AIUpdate`, `TheGameLogic` — all returned; object count 924 at the end |
| `NAMED_FLASH_WHITE` | 9 | 195 | `Drawable` (dummy) — the `Begin_Flashing Depot` script fires every 30 frames after 2467 |
| `COMMANDBAR_REMOVE_BUTTON_OBJECTTYPE` / `PLAYER_SCIENCE_AVAILABILITY` / `PLAYER_SET_RANKLEVELLIMIT` / `PLAYER_EXCLUDE_FROM_SCORE_SCREEN` | 25 / 18 / 1 / 7 | 25 / 18 / 1 / 7 | `TheControlBar`, `Player` |
| `VICTORY` / `DEFEAT` | 2 / 1 | 0 / 0 | not reached (needs player) |
| `PLAY_MOVIE*` | 0 | 0 | not on this map |

Not executed (STATIC only) and gated behind player progress: `NAMED_FIRE_SPECIAL_POWER_AT_WAYPOINT`,
`CREATE_REINFORCEMENT_TEAM`, `TEAM_MERGE_INTO_TEAM`, `TEAM_GUARD_*`, `NAMED_HUNT`, `NAMED_ENTER_NAMED`,
`EXIT_SPECIFIC_BUILDING`, `PLAYER_SET_MONEY`, `PLAYER_RELATES_PLAYER`, `IDLE_ALL_UNITS`,
`MAP_REVEAL_PERMANENTLY_AT_WAYPOINT`, `DISABLE_COUNTDOWN_TIMER_DISPLAY`, `ROTATE_CAMERA`,
`CAMERA_LOOK_TOWARD_WAYPOINT`, `TEAM_EXECUTE_SEQUENTIAL_SCRIPT`, `SOUND_SET_VOLUME`, `NO_OP`.

The mission's actual flow, as fired (first 2,530 frames): `CINE_MapSetup` (frame 0: letterbox, fade,
input off, 7 timers) → cinematic timers expire in order (`CINE_ActionDelay` 78, `CINE_BikeGangJumpDelay`
184, `CINE_SingleFlyBySoundDelay` 281 …) → `CINE_StartTrack01` sets music `Cin_China01c` at 430 →
Scud/rocket/nuke sequence 1053–2133 → briefing `SPEECH_PLAY` ×9 at 2286 → `Give Player The Game` at
2467 (`INTRO_DONE`, `ENABLE_INPUT`, `Game_USA_08`) → USA music manager at 2468 (`Game_USA_06`) →
`Mission Breifing One` at 2529 (objective caption, radar ping). From there `Begin_Flashing Depot` and
the depot/area/destroyed triggers evaluate each frame waiting for the player.

## 4. Audio: what "reaches the audio manager" means here

Chain, LINUX-LIVE, 9,000 updates:

| Link | Count | Note |
|---|---:|---|
| script action → `AudioManager::addAudioEvent` | 27 script-originated calls | attributed by a finish-breakpoint on the action (§4 table in the summary) |
| `addAudioEvent` total | 409,047 | almost all ambient loops re-raised per object per frame (`Amb_WaterRiverLoop` ×59,220 …) |
| `removeAudioEvent` | 2,865,092 | |
| returned a handle | 4 | the 4 `MUSIC_SET_TRACK` events |
| returned `AHSV_NoSound` | 23 | every `SPEECH_PLAY` and `PLAY_SOUND_EFFECT[_AT]` |
| `processRequestList` → request processed | 37 | 4 music + 33 `SentryDroneWeapon` |
| `startStream` / stream handle | 4 / 4 | `Cin_China01c`, `Game_GLA_09`, `Game_USA_08`, `Game_USA_06` |

`AHSV_NoSound` is the headless backend, not a defect: in `-headless` the audio manager reports
`m_num2DSamples = m_num3DSamples = m_numStreams = 0`, `m_selectedProvider = -1`, and
`SoundManager::canPlayNow` refuses any sample when the pool is zero. Music bypasses the sample pool
(streams), which is why those four return a handle and reach `AIL_open_stream` — the exact call that
crashed before the fix (§6). Engine state (`m_musicOn/m_soundOn/m_speechOn` all true,
`m_disallowSpeech` false) was read from the inferior. **None of this proves audibility**; the decoder
was proven in `audio-mpeg-decode.md` and the sound-effect chain in `sound-effects-chain.md`; joining
them in a *real game* with a real device is residual #3.

## 5. What did not happen, with the first broken link

### 5.1 No movie action (MISSING on this route, not a defect)

The static inventory has zero `PLAY_MOVIE*` actions on USA-01, so the in-mission FFmpeg chain
(`Display::playMovie` → `InGameUI::playMovie` → `FFmpegVideoPlayer::open` → decode → render → present)
had nothing to do and none of those breakpoints fired. The mission *does* have an intro movie
(`Mission::m_movieLabel`, parsed by `CampaignManager.cpp` and played by `LoadScreen.cpp`), but that
runs on the campaign route (`TheCampaignManager->setCampaignAndMission` → load screen), which the
`-file` route skips. First link never reached: `LoadScreen::init` with a movie label. UNMEASURED.

### 5.2 No natural mission end (needs a player)

Every `VICTORY`/`DEFEAT` on the map is behind `Train_Depot_Secured`, `Captured_Radio` or
`PLAYER_ALL_DESTROYED`. Headless has no input driver on Linux (the Mac driver is `CGEventPost`); the
probe's `--force-end` is the substitute. UNMEASURED; residual #1 is the cheap way to close it.

### 5.3 After the synthetic end the process exits with status 1 (design + debug asserts)

LINUX-SYNTHETIC, both runs: `clearGameData` runs at frame 2720, then — because `-file` sets
`m_initialFile` — `TheGameEngine->setQuitting(TRUE)` (`GameLogicDispatch.cpp`), i.e. quit to desktop
is the *designed* behaviour of this route, not a hang and not a port defect. During that teardown the
debug binary reports `Object.cpp:802` (`findObjectByID(m_containedByID) == m_containedBy`, a contained
unit torn down after its container) and then either `GameMemory.cpp:2907` (`used == m_usedBytes`) or
`GameMemory.cpp:3221` (42 leaked blocks); `-ignoreAsserts` continues past each but the leak report
makes `main` return 1. Whether the Windows debug build reports the same at `clearGameData` is not
known — the Windows oracle here is the retail *release* build, which compiles these checks out.
Classification: UNMEASURED (needs a Windows debug run), noted as residual #4; it is after the
transition this probe was asked about.

### 5.4 Assertions during the live run

All with `-ignoreAsserts`, none fatal, none inside `ScriptEngine`/`ScriptActions`/`ScriptConditions`:
`ControlBar.cpp:2466` (headless, no control bar; before frame 0), `HeightMap.cpp:973/974`,
`GameMemory.cpp:3586/3602/2686` (pool-size lookups, frame 0), `W3DModelDraw.cpp:694/852`,
`Object.cpp:5583`, `GarrisonContain.cpp:695`, `SparseMatchFinder.h:184` (frame 1),
`GameAudio.cpp:996` (frame 1957), `TurretAI.cpp:696` (frame 2887). Each fired once. Whether any is
Linux-specific is UNMEASURED (same Windows-debug caveat); none altered the script flow measured above.

## 6. The one fix: debug bounding wall vs 16-byte alignment (PORT DEFECT, fixed)

Before the fix the debug binary died at logic frame 430 — the first `MUSIC_SET_TRACK`:

```
SIGSEGV  movaps %xmm0,0xf0(%rdi)      rdi = 0x555564d5dbf8   (8 mod 16)
#0 AIL_open_stream (…"Data\\Audio\\Tracks\\C_Chi01c.mp3") OpenALStream.cpp:625   new StreamVoice()
#1 MilesAudioManager::playAudioEvent … #4 MilesAudioManager::update … #9 GameMain
```

`new StreamVoice()` goes through the game allocator. An earlier slice made the pool header
`MEM_BOUND_ALIGNMENT`-aligned (16 on LP64, `GameMemory.cpp` `@bugfix 16/08/2026`), but under `MEMORYPOOL_BOUNDINGWALL` (debug) two `Int`s of
wall sit between the header and the user data, so every debug allocation was returned at 8 mod 16 and
clang's aligned SSE stores into a freshly constructed object faulted. The release build has no wall
and was unaffected — which is why the skirmish hours in `playability-probe.md` (release) never saw it,
and why anything measured in debug (this probe, `mission-frame-trace.py`) stopped at the first
`operator new` of a ≥16-aligned type.

Fix (`Core/GameEngine/Source/Common/System/GameMemory.cpp`, one macro): `WALLCOUNT` is
`max(2, MEM_BOUND_ALIGNMENT / sizeof(Int))`. On 32-bit Windows `MEM_BOUND_ALIGNMENT` is 4, so
`WALLCOUNT` stays 2 and the layout is byte-identical; on LP64 it is 4 `Int`s (16 bytes). Debug only;
the release allocator does not compile the wall. Re-measured after the fix: the same run goes to 9,000
updates with no signal, and the four music streams open.

Windows build gate: `./scripts/docker-build.sh` (Wine/VC6) compiles with the change; replays were not
run (the change is a debug-only macro and the replay gate already mismatches on unmodified `main`).

## 7. Proven / inferred / unmeasured

**Proven (LINUX-LIVE, counted at the engine call):** script engine runs every frame; 239 scripts
evaluated, 88 fired; 17 condition types dispatched including every area/destroyed/attacked type
present; 88 action types executed, none asserting; timers set, count down and expire on the right frame;
counters and flags mutate; subroutines called; objective caption reaches `InGameUI::militarySubtitle`
with the right labels on the right frames; `SPEECH_PLAY`/`PLAY_SOUND_EFFECT*`/`MUSIC_SET_TRACK` reach
`AudioManager::addAudioEvent`; music reaches stream open. **LINUX-SYNTHETIC:** `doVictory`/`doDefeat`
arm the 120-frame timer, `m_victorious` follows, `exitGame` and `clearGameData` run, no hang.

**Inferred:** `AHSV_NoSound` for samples is caused by the zero headless sample pools (read from the
inferior; consistent with `SoundManager::canPlayNow`, not observed with a real device). The frame-430
crash cause is the wall offset (fault address 8 mod 16 at a `movaps` immediately after `new`, gone with
the fix; not bisected further). The six never-evaluated conditions and the un-executed actions are
gated on player progress (read from the map's `ENABLE_SCRIPT` chains, not exercised).

**Unmeasured:** audibility; visible caption/objective panel/score screen; movies on the campaign route;
natural victory/defeat; next-mission load; EVA events; Windows-vs-native behavioural equality;
Windows-debug status of the §5.3/§5.4 assertions; Apple Silicon.

## 8. Ranked residuals (wave 12), with cost

1. **Drive the mission to a natural end headless** — set `Train_Depot_Secured` via gdb (`--set-flag`
   on the probe, or a `NAMED_OWNED_BY_PLAYER` satisfied by `Object::setTeam` from gdb) and watch the
   scripted `VICTORY` fire, then `Rebrief Phase Two/Three`, `Eva_Reinforcements`, `Final_Blurb`.
   Closes the six zero condition rows and the `VICTORY` row. ~½ session; probe change only.
2. **Campaign route instead of `-file`** — enter USA-01 through `TheCampaignManager` so `LoadScreen`
   plays the intro movie (FFmpeg open/decode/render measurable, present still UNMEASURED headless)
   and `clearGameData` goes to the score screen and `Mission::m_nextMission`. Needs the shell
   driven headless (the `-headless` shell path from `startup-to-mission-start.md`). ~1 session.
3. **Real audio device in a real mission** — run the live route with `ALSOFT_DRIVERS` unset on a box
   with a sink (or the Mac) and confirm `SPEECH_PLAY` returns a handle and `HAS_FINISHED_SPEECH`
   times out correctly. ~½ session on the Mac; blocked on machine availability.
4. **`clearGameData` teardown asserts (`Object.cpp:802`, `GameMemory.cpp:2907/3221`)** — decide
   UPSTREAM vs PORT with a Windows debug build (`docker-build.sh` in Debug) or by checking whether the
   contained-object teardown order depends on pool iteration order (which the wall change moved on
   LP64 only). ~½–1 session.
5. **Objective panel** — `SHOW_MILITARY_CAPTION` is the caption; the objectives *list* is
   `InGameUI::addObjective`-style UI reached from `ControlBar`, not from scripts on this map. Verify
   on a map that uses it (USA-02+) with a headless window manager that records `winCreateFromScript`
   payloads. ~½ session.
6. **Apple Silicon rows** — the probe is gdb-only; port it to LLDB (as `macos-combat-probe.py` did) and
   re-run §1 on the M1. ~½ session once the machine is free.

## 9. Re-running

`scripts/campaign-flow-probe.py --help`. Requires: Linux, gdb with Python, the debug native binary,
retail data in `--run-dir`, `ALSOFT_DRIVERS=null` (default in the script). A 9,000-update run is
~12 minutes; the 200-update smoke (`--frames 200`) is ~50 s and prints the static inventory, which is
deterministic for a given map. It is **not** gated in CI: it needs retail data.

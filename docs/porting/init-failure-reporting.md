# Startup initialisation: who can report a failure

Wave 7 slice C. Companion to [process-and-crash-seam.md](process-and-crash-seam.md) (what the
codebase does about fatal errors) and [first-native-run-arm64.md](first-native-run-arm64.md) (the
run that made this worth writing).

## Why

The first native Apple Silicon run hit a `.csf` string-table parse that desyncs on 4-byte
`wchar_t`. Finding it took a debugger, not because the bug is subtle, but because
`GameTextManager::init()` returns `void` and detected the failure without saying so:

```cpp
	if ( !parseCSF ( csfFile.str() ) )
	{
		deinit();
		return;          // startup continues with zero strings loaded
	}
```

The only visible symptom was the constructor's fallback string,
`***FATAL*** String Manager failed to initialize properly`, appearing in the **window title** —
`GameEngine::init()` calls `updateWindowTitle()` on the line after `TheGameText` is initialised, so
a subsystem's own announcement of a fatal failure was rendered as UI and the process carried on.

On Windows against retail data these paths do not fail, so the missing return value cost nothing. A
port makes them reachable: files parse differently, types are wider, APIs are absent. An init path
that cannot say "I failed" turns a crisp startup error into arbitrary later misbehaviour.

So the first question is not "which of these should be fixed" but "which of these *could tell us*".
That is what the enumeration below answers.

## Method, and what the table does and does not claim

`scripts/init-reporting-scan.py` reads the call list out of `GameEngine::init()`
(`GeneralsMD/Code/GameEngine/Source/Common/GameEngine.cpp`), resolves each entry to the class whose
`init()` actually runs — through the `Win32GameEngine::createX()` factories, and through base
classes where the subsystem does not override `init()` — finds that function's body, and classifies
the loudest failure report in it:

| Classification | Meaning |
|---|---|
| `throws` | `throw`, or `rts::throwInitFailure()`. `GameEngine::init()`'s catch clauses turn it into a `RELEASE_CRASH` with a message. |
| `release-fatal` | `RELEASE_CRASH`/`RELEASE_CRASHLOCALIZED`: reports and exits in every configuration. |
| `debug-only` | `DEBUG_CRASH`/`DEBUG_ASSERTCRASH` only. A release build prints nothing and continues. |
| `silent-return` | A guarded `return;` and no diagnostic: the subsystem is left half-initialised and startup continues as if it had succeeded. |
| `no-failure-path` | No early return and no diagnostic found. **Not** a claim that the body cannot fail. |

Three limits worth stating plainly, because they bound what the table proves:

* it is a text scan, not a compiler. It does not follow calls *out* of the body, so
  `no-failure-path` on `W3DGameClient::init()` means that four-line function; the display
  initialisation it reaches through `GameClient::init()` is a separate row in the hand-verified
  table below.
* it reads every configuration at once (`#if` branches are not evaluated), so a `return;` that only
  exists under `INTENSE_DEBUG` still counts as a guarded return. Where that changes the reading, the
  hand-verified table says so.
* "does anyone check the status" has one answer for all 46 rows and it is *no*, structurally:
  `SubsystemInterface::init()` returns `void`, and `SubsystemInterfaceList::initSubsystem()` calls
  it and then registers the subsystem regardless. There is nothing to check, which is why throwing
  is the mechanism this slice uses rather than a return value — see
  [decisions-resolved.md](decisions-resolved.md) on not redesigning subsystem lifecycle for the
  port.

Where a factory can return one of two classes (the `-headless` dummies), both are listed and the row
is classified by the **weakest** of the two, because that is what a caller can rely on — which is why
rows 22 and 40 read `no-failure-path` with `DEBUG_CRASH` in the evidence.

Everything in `GameEngine::init()` is on the single-player startup path: skirmish and campaign both
start behind it. The exceptions are noted per row.

## The enumeration

Generated — do not hand-edit. Regenerate both copies with

```sh
python3 scripts/init-reporting-scan.py --doc --json docs/porting/ci-baselines/init-reporting.json
```

The machine copy is `ci-baselines/init-reporting.json` and CI holds it with
`python3 scripts/init-reporting-scan.py --check`, which fails if any entry point's reporting gets
quieter than the baseline or if a new entry point cannot report at all.

<!-- BEGIN GENERATED TABLE -->
| # | Entry point | Class whose `init()` runs | Returns | Failure reporting | Evidence |
|---|---|---|---|---|---|
| 1 | `TheNameKeyGenerator` | `NameKeyGenerator` | `void` | debug-only | DEBUG_CRASH |
| 2 | `TheCommandList` | `CommandList` | `void` | no-failure-path | none |
| 3 | `TheLocalFileSystem` | `Win32LocalFileSystem` | `void` | no-failure-path | none |
| 4 | `TheArchiveFileSystem` | `Win32BIGFileSystem` | `void` | debug-only | DEBUG_CRASH, 1 guarded return |
| 5 | `TheWritableGlobalData` | `GlobalData` | `void` | no-failure-path | none |
| 6 | `TheGameLODManager` | `GameLODManager` | `void` | no-failure-path | none |
| 7 | `TheDeepCRCSanityCheck` | `DeepCRCSanityCheck` | `void` | no-failure-path | none |
| 8 | `TheGameText` | `GameTextManager` | `void` | throws | throwInitFailure, 1 guarded return |
| 9 | `TheScienceStore` | `ScienceStore` | `void` | debug-only | DEBUG_CRASH |
| 10 | `TheMultiplayerSettings` | `MultiplayerSettings` | `void` | no-failure-path | none |
| 11 | `TheTerrainTypes` | `TerrainTypeCollection` | `void` | no-failure-path | none |
| 12 | `TheTerrainRoads` | `TerrainRoadCollection` | `void` | no-failure-path | none |
| 13 | `TheGlobalLanguageData` | `GlobalLanguage` | `void` | debug-only | DEBUG_CRASH |
| 14 | `TheAudio` | `MilesAudioManagerDummy`, `MilesAudioManager` | `void` | silent-return | 1 guarded return |
| 15 | `TheFunctionLexicon` | `W3DFunctionLexicon` | `void` | no-failure-path | none |
| 16 | `TheModuleFactory` | `W3DModuleFactory` | `void` | no-failure-path | none |
| 17 | `TheMessageStream` | `MessageStream` | `void` | no-failure-path | none |
| 18 | `TheSidesList` | `SidesList` | `void` | no-failure-path | none |
| 19 | `TheCaveSystem` | `CaveSystem` | `void` | no-failure-path | none |
| 20 | `TheRankInfoStore` | `RankInfoStore` | `void` | debug-only | DEBUG_CRASH |
| 21 | `ThePlayerTemplateStore` | `PlayerTemplateStore` | `void` | no-failure-path | none |
| 22 | `TheParticleSystemManager` | `ParticleSystemManagerDummy`, `W3DParticleSystemManager` | `void` | no-failure-path | DEBUG_CRASH; none |
| 23 | `TheFXListStore` | `FXListStore` | `void` | no-failure-path | none |
| 24 | `TheWeaponStore` | `WeaponStore` | `void` | no-failure-path | none |
| 25 | `TheObjectCreationListStore` | `ObjectCreationListStore` | `void` | no-failure-path | none |
| 26 | `TheLocomotorStore` | `LocomotorStore` | `void` | no-failure-path | none |
| 27 | `TheSpecialPowerStore` | `SpecialPowerStore` | `void` | no-failure-path | none |
| 28 | `TheDamageFXStore` | `DamageFXStore` | `void` | no-failure-path | none |
| 29 | `TheArmorStore` | `ArmorStore` | `void` | no-failure-path | none |
| 30 | `TheBuildAssistant` | `BuildAssistant` | `void` | no-failure-path | none |
| 31 | `TheThingFactory` | `W3DThingFactory` | `void` | no-failure-path | none |
| 32 | `TheUpgradeCenter` | `UpgradeCenter` | `void` | no-failure-path | none |
| 33 | `TheGameClient` | `W3DGameClient` | `void` | no-failure-path | none |
| 34 | `TheAI` | `AI` | `void` | no-failure-path | none |
| 35 | `TheGameLogic` | `W3DGameLogic` | `void` | no-failure-path | none |
| 36 | `TheTeamFactory` | `TeamFactory` | `void` | no-failure-path | none |
| 37 | `TheCrateSystem` | `CrateSystem` | `void` | no-failure-path | none |
| 38 | `ThePlayerList` | `PlayerList` | `void` | no-failure-path | none |
| 39 | `TheRecorder` | `RecorderClass` | `void` | no-failure-path | none |
| 40 | `TheRadar` | `RadarDummy`, `W3DRadar` | `void` | no-failure-path | DEBUG_CRASH; none |
| 41 | `TheVictoryConditions` | `VictoryConditions` | `void` | no-failure-path | none |
| 42 | `TheMetaMap` | `MetaMap` | `void` | no-failure-path | none |
| 43 | `TheActionManager` | `ActionManager` | `void` | no-failure-path | none |
| 44 | `TheGameStateMap` | `GameStateMap` | `void` | no-failure-path | none |
| 45 | `TheGameState` | `GameState` | `void` | no-failure-path | none |
| 46 | `TheGameResultsQueue` | `GameResultsQueue` | `void` | no-failure-path | none |
<!-- END GENERATED TABLE -->

Counts: 1 `throws`, 0 `release-fatal`, 5 `debug-only`, 1 `silent-return`, 39 `no-failure-path`,
0 unresolved.

`TheDeepCRCSanityCheck` is compiled only under `DEBUG_CRC`; `TheAudio`,
`TheParticleSystemManager` and `TheRadar` have a dummy alternative chosen by `-headless`, and both
candidates are listed. Every other row runs on every start of a skirmish or campaign game.

## The rows that can fail, hand-verified

"Consequence" is what the process does on a release Windows build today, after this slice.

| Entry point | Detects a failure? | Reports it? | Consequence |
|---|---|---|---|
| `TheGameText` | yes: string file missing, `.csf` missing, zero strings, allocation, parse | **yes, new in this slice** | `rts::throwInitFailure()` → `INIException` → `GameEngine::init()`'s `catch` → `RELEASE_CRASH` with the subsystem name and the file. Previously: returned with zero strings, and every localized string in the game was missing. |
| `TheArchiveFileSystem` (`Win32BIGFileSystem::init()`) | partly: `DEBUG_ASSERTCRASH` on a missing `TheLocalFileSystem`; `loadBigFilesFromDirectory()`'s return value is discarded | debug builds only | release: continues with no `.big` archives mounted. Every subsequent asset load fails one at a time. **Not fixed here**: whether a missing archive directory is tolerated on retail installs is a data question, and `loadMods()` deliberately tolerates absent paths. |
| `TheNameKeyGenerator` | `DEBUG_CRASH` on a name-key/CRC mismatch | debug builds only | release: continues with mismatched keys. Load-bearing for `RETAIL_COMPATIBLE_CRC` — see `verifyNameKeyID()` calls in `GameEngine::init()`. Changing it would change replay CRCs, so it is out of scope for this slice. |
| `TheScienceStore`, `TheRankInfoStore`, `TheGlobalLanguageData` | `DEBUG_CRASH` on malformed data | debug builds only | release: continues with partial data. All three are fed by INI files whose *parse* errors already throw (below), so the debug-only checks cover residual semantic checks, not file loading. |
| `TheAudio` (`MilesAudioManager::init()`) | the `return;` the scan sees is `#ifdef INTENSE_DEBUG` (sound deliberately disabled in that configuration), not a failure path. `openDevice()` failures are handled inside Miles | n/a in shipped configurations | audio is initialised with a dummy manager when `-headless`; a failing device leaves the game silent by design. Nothing to make loud here. |
| `TheGameClient` → `GameClient::init()` → `W3DDisplay::init()` | yes: `WW3D::Init()` / render-device creation | yes, already | `throw ERROR_INVALID_D3D` → `RELEASE_CRASHLOCALIZED("ERROR:D3DFailurePrompt", ...)`. This is the shape the rest of the tree should look like, and the one the native null-backend work (slice B) inherits. |
| every `initSubsystem()` with an INI path | yes: the INI parser | yes, already | `INI::loadFileDirectory()` throws `INIException` carrying file and line; `GameEngine::init()` turns it into `RELEASE_CRASH((e.mFailureMessage))`. This is why 39 rows have no failure path of their own: their failure surface is the INI load next to them, which is already loud. |

## Startup steps that are not `init()` entry points

Same walk, hand-verified, in `GameEngine::init()` order. These are not in the scan because they are
not `init()` calls, and two of them are the largest remaining silent surfaces.

| Step | Returns a status? | Checked? | Consequence if it fails |
|---|---|---|---|
| `TheFileSystem = createFileSystem()` | pointer | no | allocation failure throws from `MSGNEW`; nothing else can fail. |
| `xferCRC.open("lightCRC")` | `void` | n/a | — |
| `TheWritableGlobalData->parseCustomDefinition()` | `void` | n/a | parse errors throw `INIException`; an absent custom definition is intentionally tolerated. |
| `ini.loadFileDirectory(...)` × 8 (`GameData`, water, weather, command maps) | `void`, throws | yes, by `GameEngine::init()`'s `catch (INIException)` | loud: `RELEASE_CRASH` with file and line. |
| `CommandLine::parseCommandLineForEngineInit()` | `void` | n/a | an unknown switch is ignored. Deliberate. |
| `TheArchiveFileSystem->loadMods()` | `void` | n/a | an absent mod path is tolerated by design. |
| `updateTGAtoDDS()` | `void` | no | **silent**: a texture that fails to convert is simply not converted, and shows up later as a missing texture. Not on the critical path — only runs with `m_shouldUpdateTGAToDDS`. |
| `TheMetaMap->generateMetaMap()` / `verifyMetaMap()` | `void` | n/a | `verifyMetaMap()` `DEBUG_CRASH`es on an unbound command; release builds continue with unbound keys. |
| `TheSubsystemList->postProcessLoadAll()` | `void`, calls every `postProcessLoad()` | no | **silent, and a mirror of the init problem**: `SubsystemInterface::postProcessLoad()` also returns `void`. Half the cross-references between INI stores are resolved here. Next slice's candidate. |
| `TheMapCache->updateCache()` | `void` | no | **silent**: an unreadable map directory yields an empty cache; the shell map is then absent, which `GameEngine::init()` handles by clearing `m_shellMapOn` — so a total map-load failure degrades to "no maps in the listbox" rather than an error. |
| `updateWindowTitle()` | `void` | n/a | the step that rendered `TheGameText`'s failure string as UI. Unchanged; it is now unreachable with an uninitialised string manager because the throw comes first. |

## What this slice changed

1. `Core/GameEngine/Include/Common/InitFailure.h` — `rts::throwInitFailure(subsystem, format, ...)`.
   No new mechanism: it formats a message naming the subsystem and throws the `INIException` that
   `GameEngine::init()` already catches and already converts into a message-carrying
   `RELEASE_CRASH`, which writes `ReleaseCrashInfo.txt` with a stack trace and exits. It is
   `[[noreturn]]`, so callers do not need a `return` after it and the compiler knows the code below
   is unreachable.
2. `GameTextManager::init()` — the five failure returns now report. **Only** the failure-reporting
   seam is touched; the `.csf` width bug belongs to slice A and is untouched, including
   `parseCSF()`'s body.
3. `Generals` shares `GameText.cpp`, and its `GameEngine::init()` has the same
   `catch (INIException)` → `RELEASE_CRASH` clause, so the string manager's failures are loud in
   both games. Its `catch (ErrorCode)` is *not* touched: Generals is out of scope for port purposes
   (`decisions-resolved.md`), and the swallowed code below is a Zero Hour startup-path fix.
4. `GameEngine::init()`'s `catch (ErrorCode ec)` — any code other than `ERROR_INVALID_D3D` used to
   be caught and dropped, so a `throw ERROR_BUG` or `throw ERROR_OUT_OF_MEMORY` during
   initialisation resumed at the bottom of the `try` having skipped everything after the throw. It
   now `RELEASE_CRASH`es with the code.

Nothing changes on the success path: no call order changes, no return type changes, no subsystem is
added, removed or reordered, and the throw sites are all inside branches that were already
`return`ing. That is what makes it safe to land beside slices A, B and D.

## Negative control

`python3 scripts/native-init-failure-test.py` (CI: a step of the `native-build` job, and it needs no
game data). It compiles `Core/GameEngine/Source/Common/System/tests/init_failure_test.cpp` against the
real `InitFailure.h` and `INIException.h`, forces a stand-in subsystem's `init()` to fail, and drives
it through the shape the engine uses — `initSubsystem()` calling a `void init()`, with the engine's
`catch (INIException)` clause producing the text a `RELEASE_CRASH` would be handed. It asserts:

* the failure reaches the caller, and the message names the subsystem (`TheGameText`) and the file
  (`generals.csf`);
* the **pre-fix shape** — detect, `return` — reaches the caller as *success* with no diagnostic at
  all, which is the control the fix is measured against;
* `throwInitFailure()` does not return, and an over-long detail truncates rather than overrunning
  the buffer while reporting a failure;
* the scan classifier tells the two shapes apart: the pre-fix body (kept verbatim in the test)
  classifies as `silent-return` and the current one as `throws`. A gate that says `throws` about
  everything would pass without this.

## Findings, not fixed

* **`INIException` cannot be copied safely.** It owns its message through a raw `char*` with a
  destructor and no copy constructor, and `GameEngine::init()` catches it *by value*. The copy's
  destructor frees the message the temporary also frees. It is harmless as written only because
  `RELEASE_CRASH` has already reported and exited by the time the second destructor would run —
  every existing `throw INIException` in `INI.cpp` has the same property. This slice adds throw
  sites and therefore makes the path more reachable, but does not change the ownership, because
  fixing it means touching a shared class in the INI path, which is a different seam. Anything that
  wants to *recover* from an `INIException` has to fix this first.
* **`postProcessLoad()` is the same problem one phase later**, across every subsystem, and
  `postProcessLoadAll()` is where INI cross-references resolve. It is the obvious next slice.
* **The five `debug-only` rows are the remaining single-player exposure.** Making them fatal is a
  behaviour change against retail data — a `DEBUG_CRASH` that fires on a release install today is
  tolerated, and we have no evidence yet whether any of them do. Each needs the replay gate and a
  retail run of its own, so each should be its own slice rather than a sweep.

## Windows-oracle note

Three behaviours change **only when initialisation has already failed**, i.e. where Windows
currently tolerates a failure and continues:

| Condition | Before | After |
|---|---|---|
| `generals.csf`/`generals.str` missing, unparseable, or empty | continued; every localized string missing; `***FATAL*** String Manager failed to initialize properly` in the window title | `RELEASE_CRASH` naming the file |
| `StringInfo` allocation fails | continued with no strings | `RELEASE_CRASH` (unreachable in practice: `NEW` throws first) |
| An `ErrorCode` other than `ERROR_INVALID_D3D` thrown during init | caught, dropped, initialisation resumed mid-sequence | `RELEASE_CRASH` naming the code |

None of these is reachable on a working retail install, which is the case the replay gate and the
Windows build cover. The first one is reachable on a *broken* install, and it is a deliberate trade:
a missing string table is not a state the game can play in — it announces itself as `***FATAL***`
already — so reporting it is strictly better than rendering that announcement as the window title.
If any of these turns out to be load-bearing on real retail data, this table is where to revert
from.

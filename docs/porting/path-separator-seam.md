# The path-separator seam

Three defects, found independently by three sessions, are one seam:

1. loose Windows-spelled paths reaching the host filesystem (`Data\Scripts\*.scb`, `Maps\MapCache.ini`);
2. `MapUtil.cpp`'s assertion that a map file name contains a `\`, audible only in the `-DRTS_DEBUG`
   native build that landed in #104;
3. `MapCache` missing on POSIX-spelled map paths and **silently losing `m_isMultiplayer`**.

They are one seam because they are all the same question — *who is allowed to change the spelling of a
path, and where* — and the answers have to agree, or a fix at one point moves the failure to another.

Everything below was measured on this branch with the retail Zero Hour 1.04 data
(`zerohour104_gamedata_trimmed.7z`, 53,307,598 bytes, unmodified) through the headless simulation
probe, in both the debug and the release configuration, on old and new code.

## 1. The rule

**Game data is Windows-spelled and stays Windows-spelled.** The retail archives, `MapCache.ini`, the
INI literals and every path the engine builds itself use `\`. That spelling is not an accident to be
migrated away from: it is the on-disk and on-the-wire format, it is what the Windows oracle build
produces, and a map cache key made of it is written to disk by one run and read by the next.

Therefore:

| Kind of string | Rule | Where it is enforced |
|---|---|---|
| A **path** on its way *into* the host filesystem | translated to the host spelling at the boundary, and only there | `WWPlatform::Path::Resolve()`, called from `LocalFile::open()`, the `platform_path` entry points and now `Open_Stream()` |
| A **path** on its way *out* of the host filesystem (a directory listing) | translated back to the engine's spelling before any engine code sees it | `FileSystem::getFileListInDirectory()` |
| An **identifier/key** (a map cache key) | canonicalized where it is formed: lower case *and* `\`-spelled | `makeCanonicalMapCacheKey()` |

The distinction in the third row is the point of this slice. A key is not a path: nothing ever opens
it, two spellings of it must be *equal*, and it survives a process restart inside `MapCache.ini`. So
it is normalised, in one direction, by value — while a path is *translated at a boundary* and keeps
the host prefix it arrived with, because the user-data directory genuinely is a host path
(`/home/you/GeneralsZH Data/Maps\Foo\Foo.map` is the honest shape of a path in this engine off
Windows, and it works).

What was deliberately *not* done: no global rewrite of engine strings to `/`, and no fake Win32 layer.
Both would replace a two-boundary rule with a spelling convention that every future call site has to
remember.

### Why the enumeration boundary is `FileSystem`, not each `LocalFileSystem`

Zero Hour has two local filesystem implementations compiled into a native build, and **the game's
factory returns the Win32 one on every platform** — see §2. They disagree about the spelling they
return, and `TheArchiveFileSystem` (which lists archive members, always `\`-spelled) is a third
producer. Since the guarantee callers need is "the listing is spelled the way I asked", it belongs to
the interface every engine caller goes through, not to one back end. That is also the ownership-safe
place: `StdLocalFileSystem.cpp` belongs to the `compat/win32-runtime-and-crt-gaps` slice, and this
slice does not touch it.

## 2. Defect 1 — why `StdLocalFileSystem` is never instantiated

This was the finding the fix depended on, so it was answered before anything was edited.
`GameEngine::init()` does not name an implementation; it calls a virtual factory:

```cpp
initSubsystem(TheLocalFileSystem, "TheLocalFileSystem", createLocalFileSystem(), nullptr);
```

and Zero Hour's engine is `Win32GameEngine`, whose factory is unconditional
(`Core/GameEngineDevice/Include/Win32Device/Common/Win32GameEngine.h`):

```cpp
inline LocalFileSystem *Win32GameEngine::createLocalFileSystem()
{
	return NEW Win32LocalFileSystem;
}
```

`StdLocalFileSystem` *is* compiled off Windows (`Core/GameEngineDevice/CMakeLists.txt` adds it for
every non-VS6 build) and it is the implementation that translates separators and resolves components
case-insensitively — it is simply never selected. So the original report ("loose paths reach `open()`
verbatim and fail") had the consequence right and the cause one step off: nothing is missing from the
build, the wrong implementation is chosen, and `Win32LocalFileSystem` reaches the host through the
Win32 file-API seam instead.

**This slice does not change the factory.** Measured behaviour, not assumption, is why: the harness can
now bring up *either* implementation (`SIM_PROBE_LOCALFS=std|win32`), and with the seam in place both
produce the same 3 retail maps with the same CRCs (§5). Swapping the factory is a separate,
independently verifiable change — the finding is recorded here, and the divergence it was supposed to
explain is closed at the boundary instead, where it also covers the archive listing and the second
implementation.

Classification: **port defect** (implementation selection), fixed at the boundary rather than by
re-selection; the selection itself is recorded as an **unimplemented path** for a later slice.

## 3. Defect 2 — `MapUtil.cpp` asserting on the `\`

`MapCache::loadMapsFromDisk()` takes each enumerated map path, lower-cases it, and does:

```cpp
const char *szFilenameLower = filepathLower.reverseFind('\\');
if (!szFilenameLower) { DEBUG_CRASH(("Couldn't find \\ in map name!")); continue; }
```

Native enumeration returned `/`-spelled names, so this fired. Observed, old code, debug configuration,
retail maps, `SIM_PROBE_LOCALFS=std`:

```
ASSERTION FAILURE: Core/GameEngine/Source/GameClient/MapUtil.cpp:536: Couldn't find \ in map name!
```

The release configuration is worse, because `DEBUG_CRASH` compiles out and the `continue` remains.
Old code, **release** configuration, same retail maps, with the shipped `MapCache.ini` removed so the
disk pass is the only source of truth:

| Local filesystem | Maps found | Cache file written |
|---|---:|---|
| `std` | **0** — every map on disk silently invisible | none |
| `win32` (what the factory returns) | 3 | a file literally named `Maps\MapCache.ini`, *beside* `Maps/` |

New code, same configuration, same data: **3 maps for both implementations**, cache written to
`Maps/MapCache.ini`, keys `\`-spelled (`maps_5Calpine_20assault_5Calpine_20assault_2Emap`), and a
second run reads it back as a hit.

The fix is the enumeration boundary (§1), so `reverseFind('\\')` is given the spelling it has always
required. The key canonicalization in `loadMapsFromDisk` is a second, cheap guarantee at the point the
key is formed: a key must be canonical whatever produced the name.

Classification: **port defect**, reproduced before the fix on retail data in both configurations.

## 4. Defect 3 — the map cache key, and its negative control

A cache key is an identifier. `MapCache::findMap()`, `isValidMap()`, `isOfficialMap()` and the map
list all lower-cased the name and looked it up; `writeCacheINI()`, `prepareUnseenMaps()` and
`clearUnseenMaps()` compared keys against a `\`-built directory prefix. So a `/`-spelled name — from
native enumeration, or from a `MapCache.ini` a pre-seam native build wrote — is a *different key*. The
lookup does not fail: it misses, and the engine carries on with re-derived or absent metadata, losing
`m_isMultiplayer` and the player count. Wrong state, no error.

Because it is silent, it gets a **negative control**: `scripts/ci/check-path-separator-keys.py`, wired
into the `native-build-debug` job. It writes a `MapCache.ini` with one retail-spelled entry and one
`/`-spelled entry — both `isMultiplayer = yes` — then looks each up under *both* spellings through
`MapCache::findMap` in the engine's own probe (`sim_probe mapcachekeys`). No retail data needed.

Old code (`missed=2` of 4, and the gate fails):

```
RESULT entry key=maps/fixture alpha/fixture alpha.map multiplayer=yes players=4
RESULT entry key=maps\fixture beta\fixture beta.map multiplayer=yes players=2
RESULT lookup name=Maps/Fixture Alpha/Fixture Alpha.map found=yes multiplayer=yes players=4
RESULT lookup name=Maps\Fixture Alpha\Fixture Alpha.map found=no
RESULT lookup name=Maps\Fixture Beta\Fixture Beta.map found=yes multiplayer=yes players=2
RESULT lookup name=Maps/Fixture Beta/Fixture Beta.map found=no
RESULT mapcachekeys entries=2 lookups=4 missed=2
```

New code — two entries, four lookups, `missed=0`, both keys stored `\`-spelled, `multiplayer=yes` and
the player count intact for every spelling.

What it proves: two spellings of one map are one cache entry, and the state that entry carries is
returned rather than lost. What it does not prove: anything about case (§7).

The gate's second fixture is a smaller silent defect on the same seam, found while writing it.
`INI::parseMapCacheDefinition()` derives a display name for an entry with no localization tag with:

```cpp
tempdisplayname = name.reverseFind('\\') + 1;
```

On a `/`-spelled key `reverseFind` returns `nullptr`, and `nullptr + 1` is undefined behaviour. Old
code **segfaults** reading such an entry (probe exit `-11`); new code reads it and finds it, because
the key is canonicalized before that line. Classification: **port defect**, mutation-verified by the
control failing on old code.

### Where a key stops being an identifier: the host prefix

A *user* map's key begins with the user data directory, and off Windows that prefix is a genuine host
path — `GlobalData::BuildUserDataPathFromRegistry()` builds it with `/`, `getUserMapDir()` hands it
out that way, and code outside the cache still matches a key against it:
`GameState::realMapPathToPortableMapPath()` tests `in.startsWithNoCase(TheMapCache->getUserMapDir())`
(separator-sensitive), `portableMapPathToRealMapPath()` rebuilds a name as
`getUserMapDir()` + `\` + the map's folder and file, and the map preview name is derived from
`getPath_UserData()`. So canonicalization stops at that prefix: `makeCanonicalMapCacheKey()` keeps a
leading `getPath_UserData()` exactly as it arrived and respells only the part that *names the map
inside it*, which is the identifier. That is what the rule in §1 means by "keeps the host prefix it
arrived with", and it is what makes the key produced by enumeration equal to the name rebuilt by
`portableMapPathToRealMapPath()` and to the ones the direct `find(lowerName)` call sites in
`GameNetwork/GameInfo.cpp` and `WOLGameSetupMenu.cpp` pass — none of which had to change.

Respelling the whole key instead would have been silent in exactly the seam's own style: the portable
path conversion would fall into its `DEBUG_CRASH("this is impossible")` branch for every user map and
return a bogus path into save games, map transfer and the preview image name, and a rebuilt real path
would no longer equal its cache entry, so a user map would report as unavailable with
`getMaxPlayers() == -1`. That is a third fixture in the control, not an argument: it seeds a user map
key under the user data directory, requires the stored key to still read
`<user data>/.../maps\fixture delta\fixture delta.map` — host prefix intact, suffix canonical — and
requires it to resolve under both suffix spellings. It fails on a build that respells the whole key
(`entry key=\tmp\...\maps\fixture delta\fixture delta.map`) and passes on this one.

## 5. What changed, and the retail before/after

| File | Change |
|---|---|
| `Core/GameEngine/Source/Common/System/FileSystem.cpp` | non-Windows only: respell a directory listing in the engine's spelling, preserving the requested directory prefix verbatim |
| `Core/GameEngine/Include/GameClient/MapUtil.h`, `.../GameClient/MapUtil.cpp` | `makeCanonicalMapCacheKey()`, used everywhere a cache key is formed or matched; `writeCacheINI` writes through the path seam |
| `Core/GameEngine/Source/Common/INI/INIMapCache.cpp` | canonicalize the key read from `MapCache.ini`, so a pre-seam native cache and a retail cache name the same maps |
| `Core/Libraries/Source/WWVegas/WWLib/platform/platform_path.{h,cpp}` | `Open_Stream()`: `fopen()` on Windows, `Resolve()` then `fopen()` off it |
| `spikes/sim/src/sim_probe.cpp` | `SIM_PROBE_LOCALFS` selects the implementation the game's factory returns; new `mapcachekeys` mode; the map cache mode brings up the string manager, the thing factory and `TheMapCache` for real |
| `scripts/ci/check-path-separator-keys.py`, `.github/workflows/native-port-ci.yml` | the negative control, and the CI step that runs it |

Nothing is `#ifdef`-ed away on Windows except the enumeration respelling, which is `#ifndef _WIN32`
because on Windows the listing is already `\`-spelled. `Open_Stream()` on Windows *is* `fopen()`.
`makeCanonicalMapCacheKey()` returns after the existing `toLower()` when the string contains no `/`,
which is every string the Windows build ever hands it (see §7 for the case where it would not).

Retail evidence, gathered with the trimmed archive's `MapsZH.big` maps extracted to a scratch
directory outside the repo (299 archive entries, `Maps\MapCache.ini` with 150 cache definitions, 116
`.map` entries; three maps taken: Alpine Assault, Bitter Winter, Tournament Desert):

| Configuration | Old code | New code |
|---|---|---|
| debug, `std` | assertion at `MapUtil.cpp:536`, run stops | 3 maps, CRCs `DEA9E8E4`/`6C90F128`/`15F63DEA` |
| debug, `win32` | 3 maps, cache written to a literal `Maps\MapCache.ini` never read again | 3 maps, cache written into `Maps/` |
| release, `std`, no shipped cache | **0 maps**, exit 3, silent | 3 maps, same CRCs |
| release, `win32`, no shipped cache | 3 maps, literal `Maps\MapCache.ini` | 3 maps, cache written into `Maps/` |

The three CRCs are identical across every configuration above and equal to the values the shipped
`MapCache.ini` records for those maps, which is the property that matters: the seam changed *where*
bytes are read from, not *what* was read.

## 6. Paths found and deliberately not fixed

All of these are Windows-spelled strings handed to the C runtime or to a lower layer off Windows.
None is on the map cache path, and each is another slice's territory or needs its own evidence:

| Site | Spelling source | Why not here |
|---|---|---|
| `GeneralsMD/.../Common/Recorder.cpp` (`fopen` on `Replays\`, `ArchivedReplays\`) | `getPath_UserData()` + `"Replays\\"` | replays are the compatibility oracle; changing where they are read from wants the replay gate as its evidence, in its own slice |
| `.../System/SaveGame/GameState.cpp` (`Save\`), `XferSave.cpp`/`XferLoad.cpp`/`XferCRC.cpp` (`fopen` on a save identifier) | same shape | the save path has its own slice's worth of behaviour (and `PORTABLE_SAVE` is written into save files) |
| `Core/GameEngine/Source/Common/CRCDebug.cpp`, `CRCDiag.cpp`, `MiniLog.cpp`, `Debug.cpp` crash/log files | user-data path + `\`-spelled subdirectory | diagnostics-only; `diagnostics/init-failure-reporting` owns that area |
| `LANAPI.cpp` MOTD, `GameSpy/MainMenuUtils.cpp` downloads | INI-configured paths | the online path is being excised in another slice |
| `GameEngine.cpp`'s `TheLocalFileSystem->getFileListInDirectory("Art\\Textures\\", ...)` | direct local-filesystem call, bypasses the boundary in §1 | the texture path is the concurrent video/renderer slice; out of scope by instruction |
| `Std/Win32BIGFileSystem.cpp` archive enumeration | direct local-filesystem call | the names are only reopened through the filesystem, so the spelling never leaves the layer; no observed defect |
| `Core/GameEngineDevice/Source/StdDevice/Common/StdLocalFileSystem.cpp` | — | owned by `compat/win32-runtime-and-crt-gaps` |
| the `Win32GameEngine` factory choosing `Win32LocalFileSystem` | — | §2: recorded, measured to be equivalent with the seam in place, and left to a slice that can verify the swap on its own |

`UserPreferences.cpp`'s `fopen` was checked and is *not* on this list: its file name is the user-data
path plus a bare file name, with no embedded separator.

## 7. Case sensitivity — decided, and the risk

**Decision: out of scope for the path half, inside scope for the key half.**

- Keys: already effectively case-insensitive, because every key is lower-cased when it is formed and
  every lookup lower-cases its argument. This slice only added the separator half of that same
  canonicalization, so no case behaviour changed.
- Paths: `StdLocalFileSystem` resolves each component with `strcasecmp`, so a case-mismatched loose
  path already works through it. `Win32LocalFileSystem` — the implementation the factory actually
  returns — does not, and this slice does not change that.

The risk that leaves, stated plainly: on a case-sensitive filesystem, an INI literal whose case does
not match the file on disk fails to open under the implementation the game selects, and the failure
mode is the same quiet one as this seam's (a missing asset, not an error). It is not in this slice
because closing it means either choosing the other implementation (§2) or duplicating its resolver,
and both want their own before/after evidence. `docs/porting/filesystem-and-registry.md` records the
existing resolver; a slice that swaps the factory should carry that risk with it.

One more decision worth recording: `makeCanonicalMapCacheKey()` is *not* `#ifdef`-ed off on Windows.
It short-circuits when there is no `/`, which is every key the Windows build forms from retail data.
If a `/` ever did reach it on Windows, the Windows CRT already treats both separators as equivalent,
so folding them into one key agrees with the platform rather than diverging from it — but that path is
unreachable from retail data, so it is not an observed behaviour change to the oracle.

## 8. Gates run

- `check-path-separator-keys.py` — the negative control: fails on old code (`missed=2`, and a
  segfault on the untagged fixture), passes on new.
- `check-assert-fires.py` — still green: the debug configuration's asserts still fire.
- native build, levels 1-4, shimmed, debug **and** release, `--strict-link`: 979/979 objects, 0
  unresolved symbols, executable produced in both.
- `check-d3d8-surface.py` + `check-backend-coverage.py` (run together), `check-generated-baselines.py`,
  `classify-changes.py --self-check`, `flake8 scripts/`, `actionlint`.
- The Wine/VC6 Windows build (`scripts/docker-build.sh`): clean, `generalszh.exe` 6,352,964 bytes,
  from the `Release` / `RTS_BUILD_OPTION_DEBUG=OFF` cache the replay run requires.
- The replay **differential** under wine, per the rule in `replay-check-gamedata.md` — this branch and
  its base (`30a7a3434`) built with the identical toolchain in a `git worktree`, run by the same
  harness over the ten `GeneralsReplays/GeneralsZH/1.04` replays with the two trimmed retail archives
  (neither repacked, both extracted read-only). Both: `CRC Mismatch` on all ten at frames 9724, 3810,
  8310, 3310, 6210, 110, 4810, 4210, 4510, 3611, `Errors occurred: 10`, and a `diff` of the two
  filtered logs is empty. Those are also the frames this document's baseline records for unmodified
  `main`, so the harness is not the variable. **This is not the replay gate** and is not quoted as
  one: it says the change moved no simulation behaviour these ten replays observe. The authoritative
  run is `Replay Check GeneralsMD` on `windows-2022` in CI.

  Worth recording because it cost a run to notice: a stage that puts the replays anywhere but the
  current wine user's `Documents\Command and Conquer Generals Zero Hour Data\Replays` prints
  `Simulation of all replays completed. Errors occurred: 0` — a green from simulating nothing.
  Any replay result without a `1/10 … 10/10 Simulating` list above it is that false green.
- No baseline was regenerated: no translation unit was added or removed and no measured number in
  `docs/porting/ci-baselines/*.json` is affected by this change.

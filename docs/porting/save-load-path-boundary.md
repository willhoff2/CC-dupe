# Save and load: the path boundary the save file escaped through

Slice 2 of the playability list ([playability-probe.md](playability-probe.md) §9). A player saved a
skirmish on the real Mac, was told `*** Game Saved ***`, and a fresh process listed nothing to load.

**Classification: PORT DEFECT** (two of them, §1 and §5.2), same class as #110 and [path-separator-seam.md](path-separator-seam.md):
a Windows-spelled path reached the host filesystem without passing the separator boundary. Not an
unimplemented path (every function on the route exists and runs), not missing data (the save was
written, 100 % of it), not a serialisation fault (§6 measures that separately and finds none in this
slice's scope).

Retail 1.04 save-file **binary compatibility is out of scope** for this slice and for the port. Nothing
here makes a native build read a save written by the Windows retail game, or vice versa. The bar is
self-consistency: the same binary must read back what it wrote, and the Windows build must keep writing
exactly what it wrote before this change.

## 0. Machines, compilers, what each one proves

| Label | Machine | Compiler | What it can prove |
|---|---|---|---|
| **Linux** | Ubuntu 22.04.5, Linux 5.15 x86-64 | `clang++-14` (Ubuntu clang 14.0.0) | headless engine behaviour through the sim probe; build/link/layout gates; *not* MoltenVK, input, or Apple Silicon |
| **Mac** | `will-mac` outpost: Apple M1 Pro, macOS 26.6.1 (25G76), Darwin 25.6.0, arm64 native (`sysctl.proc_translated` = 0 under `arch -arm64`; the login shell is Rosetta x86-64, so every claim was made from an arm64 process) | Apple clang 21.0.0 (`clang-2100.1.1.101`), target `arm64-apple-darwin25.6.0` | the real shell driven by real `CGEventPost` input, real on-disk result, fresh-process load list, restored simulation |
| **Windows** | GitHub Actions `GenCI` (VC6 + win32 builds, retail replay gate) | MSVC | the behavioural oracle: Windows behaviour must be byte-identical |

Branch measured for §1–§4 and §6: `devin/1788356719-save-path-boundary` at `d50fcfdc6` (merged as #140).
The Mac measurement in §5 was made on `main` at `e71268b92` (which contains #140); the fix for the
defect it found is on `devin/1788368076-save-load-mac-evidence` (§5.2).

## 1. What escaped, and where

`GameState::getSaveDirectory()` is

```cpp
AsciiString tmp = TheGlobalData->getPath_UserData();
tmp.concat("Save\\");
return tmp;
```

That spelling is **correct and stays**. It is what the retail build writes, it is what
`isInSaveDirectory()` and `realMapPathToPortableMapPath()` compare against, and the rule from
[path-separator-seam.md](path-separator-seam.md) §1 is that engine paths are Windows-spelled and only
change spelling at the boundary into the host filesystem.

The boundary already existed — `WWPlatform::Path` (`Core/Libraries/Source/WWVegas/WWLib/platform/`) is
direct on Windows and translates `\` to `/` off it, and `CreateDirectory`, `SetCurrentDirectory`,
`FindFirstFile` and `Win32LocalFileSystem::doesFileExist()` all go through it. Four operations on the
save route did not:

| Site | Was | Why it escaped |
|---|---|---|
| `GameState::findNextSaveFilename()` (both the `_%04d` and the `%08d` branches) | `_access(path, 0)` | `scripts/native-port-shims/io.h` maps `_access` to POSIX `access()`: an existence test on the untranslated string, so the check was against `.../Save\00000000.sav` as a single file name |
| `XferSave::open()` | `fopen(identifier, "w+b")` | the C runtime creates a file whose *name* contains the backslash |
| `XferLoad::open()` | `fopen(identifier, "rb")` | reads the same wrong name — which is why the old file *could* be reopened by the process that wrote it, and only a fresh process lost it |
| `XferCRC::open()` | `fopen(identifier, "w+b")` | same, on the CRC dump path |
| `GameStateMap.cpp` embedded-map copy (`embedInUseMap` reads the in-use map, `extractAndSaveMap` writes it back under `Save\`) | `fopen` ×2 | same, for the map file copied into and out of the save |

So the sequence on the Mac was: `CreateDirectory("…\Save\")` correctly created `Save/` (empty);
`findNextSaveFilename` correctly picked `00000000.sav` because `access("…/Save\00000000.sav")` failed;
`XferSave::open` created a file literally named `Save\00000000.sav` in the user-data directory; and
`iterateSaveFiles()` — which does `SetCurrentDirectory(getSaveDirectory())` then `FindFirstFile("*")`,
both through the seam — looked inside the real, empty `Save/`.

This is also why #103's observation that Zero Hour did not instantiate a local file system was *not* the
cause here: `Win32LocalFileSystem` is instantiated (the sim probe selects it with
`SIM_PROBE_LOCALFS=win32`, and the game does the same), and `doesFileExist()` on it already resolved
paths. The save path simply never called it; it called the C runtime directly.

## 2. The change

Every site in the table now calls the seam it was bypassing:

```cpp
// GameState.cpp, both branches
if( !WWPlatform::Path::Exists( path.str() ) ) return leaf;

// XferSave.cpp / XferLoad.cpp / XferCRC.cpp / GameStateMap.cpp
m_fileFP = WWPlatform::Path::Open_Stream( identifier.str(), "w+b" );   // resp. "rb"
```

On Windows `WWPlatform::Path::Exists()` **is** `::_access(path, 0) == 0` and `Open_Stream()` **is**
`fopen()` (`platform_path.cpp`, Windows half: "every entry point is the call the call site used to
make"), so the Windows build's behaviour — and the `Save\00000000.sav` path it passes to the CRT — is
unchanged. No `#ifdef` was added at a call site; no `/` was hard-coded; no internal path spelling
changed.

Two declarations moved from `private` to `public` in `GameState.h` (`findNextSaveFilename`,
`iterateSaveFiles`) so the headless control in §3 can call the engine's own sequence instead of a
re-implementation of it. Nothing else in the header changed.

`iterateSaveFiles()` was read and deliberately **not** changed. Its early `return` when
`FindFirstFile("*")` fails leaves the process in the save directory, but that is retail behaviour and
cannot trigger differently here: the native `FindFirstFileA` (`platform_win32_file.cpp`) returns `.`
and `..` for an existing directory exactly as Win32 does, so `*` on an existing `Save/` never fails.

## 3. Headless proof — Linux, `clang++-14`, debug configuration (`-DRTS_DEBUG -DWWDEBUG`)

`spikes/sim/src/sim_probe.cpp` gained a `savepath` mode that constructs `GlobalData` + `GameState`
with a temporary user-data directory and runs the engine's own sequence:
`getSaveDirectory()` → `findNextSaveFilename()` → `CreateDirectory` → `XferSave` write →
`findNextSaveFilename()` again → `iterateSaveFiles()` → `doesSaveGameExist()` → `XferLoad` read-back,
with the current directory checked before and after. `scripts/ci/check-save-path-seam.py` runs it
twice and then checks the **host** disk itself:

```text
RESULT savedir spelling=<tmp>/fresh/userdata/Command and Conquer Generals Zero Hour Data/Save\
RESULT nextname before=00000000.sav
RESULT write path=<tmp>/.../Command and Conquer Generals Zero Hour Data/Save\00000000.sav ok=yes
RESULT nextname after=00000001.sav
RESULT listed name=00000000.sav
RESULT listed count=1
RESULT cwd restored=yes
RESULT reopen exists=yes readback=yes
RESULT savepath ok=yes
[legacy fixture: same nine lines]
OK: a save lands in Save/, is renumbered, listed and reopened by engine code
```

What the script asserts beyond the probe's own lines: the file exists at `Save/00000000.sav` on the
host (a real directory entry under a real `Save` directory), no entry of the user-data directory
contains a literal `\`, `Save/` contains exactly one `*.sav`, and in the second ("legacy") fixture a
pre-planted file literally named `Save\00000000.sav` is byte-identical afterwards, is not listed, and
does not stop the engine choosing `00000000.sav` for the new save (they are different names to the
host). The `savedir spelling` line is printed on purpose: it shows the engine string still ends in
`Save\` while the file lands in `Save/`.

This is wired into `native-port-ci.yml` after the debug build. It is a negative control for this
defect, not a save-format test: the payload is a two-field `Xfer` block, not a game.

Honesty note on this control: it was written after the fix and passes on the fixed tree. It was **not**
run against the tree without the §2 change (that would have needed a second full debug build);
that it would fail there is inferred from §1, not measured. The later `mapname` line (§5.2) *was*
run both ways.

After §5 the probe also performs the map-name round trip the loader makes —
`realMapPathToPortableMapPath(getFilePathInSaveDirectory("Alpine Assault.map"))` →
`portableMapPathToRealMapPath()` → `isInSaveDirectory()` — and the script requires
`RESULT mapname portable=save\alpine assault.map real=… insavedir=yes`.

## 4. Linux verification ladder — all on this branch, `clang++-14`

| Rung | Result |
|---|---|
| `flake8 --max-line-length=100 scripts/` | clean |
| `actionlint .github/workflows/*.yml` | clean |
| `native-build.py --level 1..4 --with-shims --strict-link` (release) | 980/980 objects, 0 compile failures, 0 unresolved, strict link produced `build/native/native_strict_link` (83.3 MiB ELF x86-64) |
| same, `--config debug` | 980/980, 0 unresolved, 88.4 MiB |
| `check-native-build-baseline.py` against both checked-in baselines | "OK: no regression against the baseline" (both) |
| `check-generated-baselines.py` | 11/11 baselines readable |
| `porting-status.py` | regenerated `STATUS.md`; no diff |
| `check-save-path-seam.py --build-dir build/native-debug` | §3; on `devin/1788368076-save-load-mac-evidence` re-run with the §5.2 `mapname` line: `OK` with the fix, `FAIL` (`insavedir=no`, probe exit 3) without it |
| `native-layout-test.py` | 3/3 PASS (LP64 layout, ILP32 reference, poisoned negative control fails in 221 assertions) |
| `xfer-blob-audit.py` | §6 |

The Windows build and the retail replay gate run in GitHub Actions `GenCI` on the PR (the `Core/**` and
`GeneralsMD/**` filters match this diff). Their result is the PR's check status, not a claim in this file.

## 5. Real Apple Silicon, real input — the four-step evidence

Measured by the `will-mac` child session (`devin-c2d89b686ff04c5b808625ee025549a7`) on the machine in
§0, `main` at `e71268b92e049ac4585dcc076ac8a25775f2014e`, built with `native-build.py` levels 1–4
`--with-shims --strict-link` (980/980 objects, 0 compile failures, 0 unresolved; `native_strict_link`
Mach-O arm64 27.8 MB, run as `zh` 27.7 MB). Input is `scripts/macos-input-drive.py` (`CGEventPost`),
engine state is read through LLDB. User data is
`~/Library/Application Support/Command and Conquer Generals Zero Hour Data/`.

### 5.1 What was measured on `e71268b92`

| Step | Observation | Result |
|---|---|---|
| Save from an in-progress skirmish | `GAME_SKIRMISH`; pre-save logic frame 6539, paused 0, 29.9997 avg FPS; per-player money / object counts snapshotted. `*** Game Saved ***` shown; frame 6719 at confirmation, game paused after. Between the two snapshots only the AI (China Tank General) moved: money 10950 → 11250, buildings 5 → 6, earned 7500 → 7800, spent 9000 → 10200; every other player and object count identical | **PASS** |
| File lands where the engine looks, sane name | `Save/00000000.sav`, 1,399,241 bytes, sha256 `8641c95202abb8de3a86a79dec05e6c3af52403183cb733ba9ec3172542127f2`; **no** new `Save\…` entry in the user-data directory. The pre-fix `Save\00000000.sav` (2,023,468 bytes) is still there, byte-identical, same mtime — §7 | **PASS** |
| Fresh process lists it, `LOAD GAME` enabled | `MainMenu.wnd` → `SaveLoad.wnd`; `SELECT GAME` row `Alpine Assault`; `ButtonLoad m_status=0x00000088` (enabled); the backslash file is not listed. Screenshot `shots/15-fresh-loadgame-list.png` on the Mac (not committed: retail art) | **PASS** |
| Load restores a game that keeps simulating | `LOAD GAME` → `Error loading game ''`; `game_mode` stays 6 (`GAME_NONE`). `XferLoad::open` **did** open `Save/00000000.sav`; the throw is `SC_INVALID_DATA` from `GameStateMap::xfer` (`GameStateMap.cpp:343`) ← `XferLoad::xferSnapshot` ← `GameState::xferSaveData:1481` ← `GameState::loadGame:717` ← `PopupSaveLoad doLoadGame:421`, i.e. **before** any game state is restored | **FAIL — PORT DEFECT, §5.2** |
| Restored state matches saved state; frames at +60 s / +120 s | never reached | UNMEASURED on this revision |
| Campaign save path | USA → Easy by real input; `GAME_SINGLE_PLAYER`, frames 707 → 2286 → 3580; save/load buttons disabled during the intro cinematic, enabled after. Save described `U 1`, `*** Game Saved ***`, `Save/00000001.sav`, 2,802,843 bytes, sha256 `fe9596f96246927d02488c67571f39acd2c91cb938680185a52c8cd9dc1c369b`. Fresh process lists `row0 ['U 1','06:18 PM','09/02/26']`, `row1 Alpine Assault`, `LOAD GAME` enabled | save/place/list **PASS** |
| Campaign load | `Error loading game ''`, same throw as the skirmish row | **FAIL — same defect** |

So on `e71268b92` the defect this slice set out to fix — the file landing where the engine looks — is
fixed on the real machine, for skirmish and campaign, and the fresh-process list is correct. **The loop
as a whole did not work**: neither save loaded. That is the honest state of #140 after merge; the
"proven end to end" claim was not made there and is not made here.

Incidental, not this slice: the process crashes on quit (`EXC_BAD_ACCESS` in
`ObjectPoolClass<MultiListNodeClass,256>::~ObjectPoolClass()` via `__cxa_finalize_ranges`/`exit`) —
recorded, not investigated. `native-sim-probe.py --build` cannot link on Apple `ld` because it passes
GNU `--start-group`/`--end-group`; the child linked the probe by hand outside the repo. Harness
limitation, not fixed here.

### 5.2 The second defect: containment is compared case-sensitively off Windows

`GameStateMap::xfer` reads the portable map path (`save\alpine assault.map`) from the file, turns it
back into a real path with `GameState::portableMapPathToRealMapPath()` — which **lower-cases** the
result, as the retail code always has — and then requires
`TheGameState->isInSaveDirectory(saveGameMapName)`, else `throw SC_INVALID_DATA`. `isInSaveDirectory`
is `FileSystem::isPathInDirectory(path, getSaveDirectory())`, which normalised both strings and then:

```cpp
#ifdef _WIN32
    if (!testPathNormalized.startsWithNoCase(basePathNormalized))
#else
    if (!testPathNormalized.startsWith(basePathNormalized))
#endif
```

On the Mac the base is `/Users/willhoff/Library/Application Support/Command and Conquer Generals Zero
Hour Data/Save\` (case preserved) and the test path is the same string lower-cased. `startsWith` says
no; the loader throws. LLDB confirmed both strings at the throw site. The Windows build never sees this
because its comparison is `NoCase`.

**Classification: PORT DEFECT** — a host-conditional in engine code that changed a Windows semantic.
The engine's path rules are Windows rules on every host (`path-separator-seam.md` §1), and the native
resolver in `platform_path.cpp` already matches path components with `strcasecmp`, so a lower-cased
path that *opens* must also *compare* as inside its directory. Fix: `isPathInDirectory` uses
`startsWithNoCase` unconditionally (`Core/GameEngine/Source/Common/System/FileSystem.cpp`). Windows is
byte-for-byte unchanged (that was already its branch).

Measured on Linux (`clang++-14`, debug): the `mapname` control in §3 prints `insavedir=no` and
`check-save-path-seam.py` fails on the tree **without** this one-file change, and prints
`insavedir=yes` / `OK` with it — this control was run both ways, unlike the §3 note.

### 5.3 Re-measurement of the load steps with §5.2 applied

Same machine, same child session, same recipe, this branch at `e7f89f94c5a4b1e351cb5bb6d5ab3af62671da5c`
(980/980, 0 unresolved, strict link clean; `native_strict_link` 27,806,704 B, run as `zh`
27,663,648 B, sha256 `3663116d…e2766b2`). The saves loaded are the ones written in §5.1 by the
`e71268b92` build — the file format did not change, only the containment test.

| Step | Observation | Result |
|---|---|---|
| Headless control on arm64 | `check-save-path-seam.py`: fresh and legacy fixtures `OK`, including `mapname … insavedir=yes` (probe linked by hand, see §5.1) | PASS (SYNTHETIC) |
| Skirmish load restores a game that keeps simulating | Fresh process, real input, `Alpine Assault` selected (`selectPos=1`), `LOAD GAME` 0x88. Loaded into `game_mode` 2 `GAME_SKIRMISH`, `paused` 0, restored mission rendered, no error dialog. First observed frame 6887; a controlled probe read frame 6882, then 8679 after 60.125 s of resumed process time; independent wall snapshots 6887 → 9208 at +90.2 s → 11099 at +166.3 s (28.6–30.0 logic FPS). Snapshots are not at exactly +60/+120 s because LLDB attach time shifts them; the frame beyond 120 s is what the row requires | **PASS** |
| Restored state matches saved state | Post-save-confirm (frame 6719) vs first post-load (6887): player 0 money/objects 0/14, Civilian 10000/205, America 10000/2, Observer 10000/0, total objects 235 — all exact. China Tank General objects 14 exact; money 11250 → 11550 (+300) over +168 frames, which is the same AI income rate seen between the two pre-save snapshots (§5.1). `GameLogic` CRC before/after: UNMEASURED, the harness has no CRC reader | **PASS** (state), CRC UNMEASURED |
| Campaign load | Fresh process, `U 1` selected (`selectPos=0`), `LOAD GAME` 0x88. Loaded into `game_mode` 0 `GAME_SINGLE_PLAYER`, frame 3826, `paused` 0, 911 objects, America 32000 money / 40 objects. After 89.9 s wall: frame 5706 (+1880), 904 objects, America 34500 / 33 — units fighting and economy running | **PASS** |
| Legacy files after both runs | `Save\00000000.sav` still 2,023,468 B, same sha256 and mtime; never listed. `Save/` holds `00000000.sav` and `00000001.sav` | as §7 |

With §5.1 and §5.3 together, every step the slice asked for has been observed on the real machine
with real input: save → file in `Save/` with a sane name → listed in a fresh process with `LOAD GAME`
enabled → load → simulation continues past 120 s → restored per-player state equals the saved state.
Skirmish *and* campaign were measured. What this does **not** show: a CRC-level identity of the
restored `GameLogic` (UNMEASURED), and anything about retail 1.04 saves (out of scope, top of file).
The quit-time `SIGSEGV` in §5.1 still occurs on exit from the loaded game.

The prior measurement of the defect itself (`Save\00000000.sav` beside an empty `Save/`, empty
`SELECT GAME` in a fresh process) is in [playability-probe.md](playability-probe.md) §6 and was made on
`main` at `0e52ccd4b`, before this change. Its "2,023,468-byte" figure is that document's, not
re-measured here.

## 6. The save blob itself — measured separately, not absorbed

Per the slice brief, layout was measured, not touched.

`scripts/xfer-blob-audit.py` on this branch (Linux, static analysis of the tracked sources — the same
on any host):

| In ported scope | Count |
|---|---:|
| raw-block sites | 292 in 82 files |
| `pointer` | **0** |
| `enum-open` | **0** |
| `unknown` | 2 — both `sizeof(WeaponRecoilInfo::RecoilState)` in `W3DModelDraw.cpp`; the enum is declared `CPP_11(: Int)` in `W3DModelDraw.h`, the audit just cannot resolve the qualified name |
| `expression` | 7 |
| distinct record types written raw | 9, all asserted in `XferLayout.cpp` |

`scripts/native-layout-test.py` (Linux, `clang++-14`): PASS / PASS / negative control fails as it must.

Wide-character payloads go through `WideCharWire.h` (16 bits on disk regardless of `wchar_t`), see
[widechar-wire.md](widechar-wire.md). So on this evidence there is **no separate layout finding to
report from this slice**; what is *not* covered by either tool is whether a save written by the arm64
Mac build is byte-for-byte what the x86-64 Linux build writes for the same game — no cross-host
comparison exists, and this slice did not add one.

## 7. Files already written with a backslash in the name — the decision

**Leave them alone. Do not migrate, do not delete, do not list.** Stated, so it is a decision and not an
accident:

* They are not saves the engine can *name*: the enumerator looks in `Save/` and matches `*.sav`; a
  file called `Save\00000000.sav` in the parent directory is invisible to it, before and after this
  change. §3's legacy fixture measures exactly this.
* Renaming them into `Save/` would put a pre-fix file in front of the player as a loadable save. The
  fix changed only *where* the bytes land, not *what* they are, so such a file would probably load —
  but "probably" is the word, and a migration path is code with no test oracle on the one machine
  that has such a file.
* The only known instance is one file on one development machine, from a build before this change.

A player who has one can rename it into `Save/` by hand; nothing in the engine prevents that, and
nothing recommends it.

The same defect class is visible for `Screenshots\sshot_*.png` (playability-probe.md §6). That path
is not part of this slice and was not changed.

## 8. What remains unmeasured, and what it would take

* **`GameLogic` CRC of the restored game vs the saved one** (§5.3). The per-player money/object
  comparison is what was measured; a CRC read needs a harness reader for `TheGameLogic->getCRC()`
  (or the `-saveStats`-style dump) that `macos-input-drive.py` does not have.
* **Frames at exactly +60 s / +120 s after load** (§5.3): LLDB attach shifts the sampling; what was
  measured is +60.1 s of resumed process time and frames beyond +120 s.
* **The quit-time `SIGSEGV`** (§5.1) — a separate defect, not part of this slice.
* **`native-sim-probe.py --build` on Apple `ld`** (§5.1) — GNU `--start-group` flags; the control ran
  from a hand-linked probe. A small harness fix for a later slice.
* **The §3 control against the tree without the §2 change.** One extra debug build of pre-#140 `main`
  plus `check-save-path-seam.py`; expected to fail at "no user-data entry contains a literal
  backslash". (The §5.2 `mapname` control *was* run both ways.)
* **Cross-host byte identity of a save** (§6). Would need the same deterministic headless game saved on
  x86-64 Linux and arm64 macOS and the two files diffed; the sim probe can run a fixed skirmish but
  has no save call today.
* **`missionSave()` (the between-missions campaign save)** — a different entry point that ends in the
  same `saveGame()` and therefore the same seam; not driven in this slice.
* **Windows byte-identity** is asserted from `platform_path.cpp`'s Windows half, and *checked* only by
  the CI Windows build and the retail replay gate on the PR.

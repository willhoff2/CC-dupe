# Save and load: the path boundary the save file escaped through

Slice 2 of the playability list ([playability-probe.md](playability-probe.md) §9). A player saved a
skirmish on the real Mac, was told `*** Game Saved ***`, and a fresh process listed nothing to load.

**Classification: PORT DEFECT**, same class as #110 and [path-separator-seam.md](path-separator-seam.md):
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
| **Mac** | Apple Silicon `will-mac` outpost, see §5 for the exact machine | Apple clang, see §5 | the real shell driven by real `CGEventPost` input, real on-disk result, fresh-process load list, restored simulation |
| **Windows** | GitHub Actions `GenCI` (VC6 + win32 builds, retail replay gate) | MSVC | the behavioural oracle: Windows behaviour must be byte-identical |

Branch measured: `devin/1788356719-save-path-boundary` at `d50fcfdc6`, based on `main`.

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
run against the unfixed tree in this session (that would have needed a second full debug build);
that it would fail there is inferred from §1, not measured.

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
| `check-save-path-seam.py --build-dir build/native-debug` | §3 |
| `native-layout-test.py` | 3/3 PASS (LP64 layout, ILP32 reference, poisoned negative control fails in 221 assertions) |
| `xfer-blob-audit.py` | §6 |

The Windows build and the retail replay gate run in GitHub Actions `GenCI` on the PR (the `Core/**` and
`GeneralsMD/**` filters match this diff). Their result is the PR's check status, not a claim in this file.

## 5. Real Apple Silicon, real input — the four-step evidence

> **PENDING AT THE TIME OF WRITING.** The `will-mac` outpost child session
> (`devin-c2d89b686ff04c5b808625ee025549a7`) was spawned for exactly this measurement; the outpost
> machine disconnected during its repo setup and had not produced a measurement when this document
> was written. Every row below is therefore **UNMEASURED on this revision** until this section is
> replaced by the child's figures. Nothing in §1–§4 depends on it; the claim "the whole loop works
> for a player" does.

| Step | Required observation | Status |
|---|---|---|
| Save from an in-progress skirmish (real `CGEventPost` input) | `*** Game Saved ***`, pre-save LLDB snapshot (frame, `GameMode`, object counts, money) | UNMEASURED |
| File lands where the engine looks, sane name | `ls -la` of `…/Zero Hour Data/Save/`: `00000000.sav` (or the next free number) with a byte size; **no** new `Save\…` entry beside it | UNMEASURED |
| Fresh process lists it, `LOAD GAME` enabled | `macos-input-drive.py buttons` dump of `SELECT GAME` listbox + `LOAD GAME` enabled bits, screenshot | UNMEASURED |
| Load restores a game that keeps simulating | `GameMode` 2, logic frame at load, +60 s, +120 s; post-load object counts/money vs pre-save | UNMEASURED |
| Campaign save path | same four steps from a USA/Easy mission | UNMEASURED |

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

* **Everything in §5** — the real-input loop on Apple Silicon. Needs the `will-mac` outpost to
  provide a machine; the child prompt and structured-output schema already exist.
* **The control against the unfixed tree** (§3). One extra debug build of `main` plus
  `check-save-path-seam.py`; expected to fail at "no user-data entry contains a literal backslash".
* **Cross-host byte identity of a save** (§6). Would need the same deterministic headless game saved on
  x86-64 Linux and arm64 macOS and the two files diffed; the sim probe can run a fixed skirmish but
  has no save call today.
* **`missionSave()` (the between-missions campaign save)** — a different entry point that ends in the
  same `saveGame()` and therefore the same seam; not driven in this slice.
* **Windows byte-identity** is asserted from `platform_path.cpp`'s Windows half, and *checked* only by
  the CI Windows build and the retail replay gate on the PR.

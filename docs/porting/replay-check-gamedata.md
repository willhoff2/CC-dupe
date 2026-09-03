# Replay check game data — running the gate in a fork

The replay determinism gate (`Replay Check GeneralsMD` in `GenCI`, implemented by
`.github/workflows/check-replays.yml`) runs the built `generalszh.exe` headless over the `.rep`
files in `GeneralsReplays/` and fails if the simulation diverges. It needs retail game data, which
is not redistributable and so is not in the repository.

Upstream downloads that data from a private bucket. A fork does not inherit its credentials, so the
gate cannot run there by default — which matters for this port: it is the only check that would
catch a save/serialisation or 64-bit change desyncing the simulation. Everything else in CI only
proves the code still builds.

There are two ways to run it, and this document covers both: **from your own bucket in CI** (what
this fork now does) and **manually against a local data directory**.

## Status in this fork: running (first pass 2026-08-15)

The fork hosts its own copy of the data in a private S3 bucket, and the gate passes. Measured, in
[run 31912666731](https://github.com/willhoff2/CC-dupe/actions/runs/31912666731), presets `vc6+t+e`
and `vc6-releaselog+t+e`:

```
download: s3://<bucket>/generals108_gamedata_trimmed.7z
Downloaded file SHA256: 15332B5D…  Expected file SHA256: 15332B5D…
download: s3://<bucket>/zerohour104_gamedata_trimmed.7z
Downloaded file SHA256: 2D137F6C…  Expected file SHA256: 2D137F6C…
Run build/generalszh.exe -jobs 4 -headless -replay *.rep
1/10 … 10/10 Simulating Replay …
Simulation of all replays completed. Errors occurred: 0
```

Before this the gate had never passed in this fork, so nothing had checked the simulation against
desync — including PR #27's save/serialisation change, which these ten replays now exercise.

If the two credential secrets are ever removed, `Replay Check GeneralsMD` **skips** with a reason in
the job summary rather than failing red: the `Game data available?` job in `check-replays.yml`
checks whether `R2_ACCESS_KEY_ID` and `R2_SECRET_ACCESS_KEY` are both set. A job that fails for a
missing input tells you nothing about the code.

## What the data is

Two 7z archives holding only what a headless replay run reads — the INI/map/W3D `.big` files, the
two DLLs the executable links against, and the script files. No textures, audio or GUI data, so the
result is not playable. The exact file lists are in `scripts/ci/pack-gamedata.py` (and its Windows
twin `pack-gamedata.ps1`) and in the comments of `check-replays.yml`.

Retail game data is not redistributable. Keep the bucket private, and do not commit the archives.

### The third object: `zerohour104_gamedata_full.7z`

The trimmed pair is what the *replay gate* reads. It is not what the *native-port probes* read, and
two of them have now stalled on the difference rather than on any code defect: the startup probe
could not execute the menu path or `TheSkirmishGameInfo` without GUI data, and the audio probe found
three real defects but decoded zero retail bytes.

So there is a third object, packed by `pack-gamedata.py --archives full`: **every `.big` from both
install roots**, which is what those probes need and then some. Measured with
`spikes/assets/zh-asset-inspect`, the parts that matter are `WindowZH.big` (80 `.wnd`),
`EnglishZH.big` (`generals.csf` plus 66 tga), `AudioZH`/`AudioEnglishZH`/`SpeechZH` (1,093 wav),
`MusicZH.big` (7 mp3 — the MP2/MP3 stream path `audio-surface.md` records as never exercised), and
`TexturesZH.big` (3,496 dds).

Two structural facts about it:

- **The two install roots are not flattened together.** Both ship a `Music.big` and they are
  different files — Zero Hour's is a 787 KB security stub, Generals' is 159 MB of streamed music.
  Merging the roots would silently drop one. The archive therefore has two top-level directories,
  `Generals/` and `GeneralsMD/`, each extracted to the install path it belongs to.
- **It is a separate object, and the trimmed pair is not repacked.** The gate compares
  `generals108_gamedata_trimmed.7z` and `zerohour104_gamedata_trimmed.7z` against hashes pinned in
  repository variables; repacking either would break that comparison for no gain.

`.bik` movies are *not* in any `.big`, so none of them are in this object. They have their own,
below.

### The fourth object: `zerohour104_movies.7z`

Retail video is loose files, not `.big` entries, which is why a slice that downloaded the full
archive, hash-verified it and parsed all 35 `.big` found no video at all. `--archives movies` packs
the 70 `.bik` both installs carry, 589.5 MiB:

| Directory | Files | Bytes |
|---|---:|---:|
| `Generals/Data/Movies` | 29 | 322,622,656 |
| `GeneralsMD/Data/English/Movies` | 39 | 295,002,048 |
| `GeneralsMD/Data/Movies` | 2 | 459,828 |

Note where the weight is. `Data/Movies` holds only the two menu backgrounds
(`GC_Background.bik`, `VS_small.bik`); the campaign cutscenes are under `Data/English/Movies`, and
**29 more live in the base Generals tree**, which Zero Hour reads. Looking only at
`GeneralsMD/Data/Movies` undercounts the video data by three orders of magnitude.

The layout matches the full archive: two top-level directories, `Generals/` and `GeneralsMD/`, with
paths below each one relative to that install root — so an entry reads
`GeneralsMD/Data/English/Movies/MD_USA02_0.bik`. Extract to a staging directory and copy each
top-level directory's contents into the install path it belongs to. Extracting straight over an
install with `7z x -o<install path>` buries them one level deep, where the game does not look.

> One trap worth knowing if you re-derive this: a depot copied for its data often nests the
> Generals tree *inside* the Zero Hour one (`zh-data/ZH_Generals`), so a recursive `.bik` sweep of
> the Zero Hour root claims Generals' movies too and packs them twice, under the wrong root.
> `collect_movies()` takes the other root as `exclude` for exactly this reason.

### The fifth object: `zerohour104_loose_data.7z`

The `.big` files and the `.bik` movies are not the whole install. The installer also puts a
`Data/` tree down loose, and none of it was hosted — which is what stalled the mouse-cursor slice:
`mouse-cursor-seam.md` §6 could not measure a single retail cursor, because
`LoadCursorFromFile("data\cursors\<Name>.ANI")` reads files that are in no `.big` and in no object.
`--archives loose` packs what is left, 173 files and 4.1 MiB (measured 2026-09-03):

| Directory | Files | Bytes |
|---|---:|---:|
| `Generals/Data/Cursors` | 52 | 236,304 |
| `Generals/Data/Scripts` | 2 | 474,440 |
| `Generals/Data/WaterPlane` | 32 | 525,696 |
| `GeneralsMD/Data/Cursors` | 52 | 236,304 |
| `GeneralsMD/Data/Scripts` | 3 | 2,351,753 |
| `GeneralsMD/Data/WaterPlane` | 32 | 525,696 |

Two exclusions, both deliberate:

- **`.bik`**, because [the movies object](#the-fourth-object-zerohour104_movies7z) carries it. The
  two objects together are exactly the `Data/` tree.
- **`.big`**, because [the full object](#the-third-object-zerohour104_gamedata_full7z) carries every
  `.big` in an install root — and the one that lives *under* `Data/`, `Data/INI/INIZH.big`, is the
  duplicate `StdBIGFileSystem.cpp` skips by name to avoid a CRC mismatch (the English, Chinese and
  Korean SKUs shipped two). Packing it would ship 17.8 MiB the engine ignores.

The packer refuses a root whose `Data/Cursors` is missing or holds no `.ani`, naming it. A tree
copied for its `.big` files alone would otherwise pack its WaterPlane and Scripts, hash cleanly and
silently omit the cursors — which would surface much later, as a mouse seam still showing the
default arrow. The cursor *names* are not checked, because the SKUs differ.

The 52 `.ani` per root are byte-identical between the two trees, and both are packed: the game
resolves `data\cursors\` against whichever root it is running from, so dropping either would make
the object depend on which install the reader points at. Note that 52 files back the **27** cursors
`Mouse.ini` names — `SCCScroll` alone is eight files, and most shapes have an `_S` variant.

Layout matches the other two multi-root objects: `Generals/` and `GeneralsMD/` on top, paths below
relative to that install root, so an entry reads `GeneralsMD/Data/Cursors/SCCAttack.ani`. The
nesting trap `collect_movies()` guards against does not apply here — the sweep is confined to
`Data/`, and a depot copy nests the Generals tree at the root (`zh-data/ZH_Generals`), not under it.

### Compression levels are per-object, and measured

| Object | Level | Why |
|---|---|---|
| trimmed pair | `-mx=9` | the hashes CI pins were produced with it; repacking must reproduce them |
| full | `-mx=5` | on a 216 MB slice of the real payload, `-mx=5` finished in 78 s and `-mx=9` had not finished after 510 s — ~15 min against 96+ over the whole object, to land ~1% smaller |
| movies | `-mx=1` | Bink is already-compressed video: `-mx=1` gives 1.018x, `-mx=5` gives 1.020x. The whole 589.5 MiB packs in 18 s |
| loose | `-mx=9` | 4.1 MiB of small files, and the best level is free: the whole object packs in 1.8 s, 4.1 MiB down to 202 KiB |

The level is part of what the SHA256 covers, so changing one re-hashes that object. A first attempt
at packing the full archive at `-mx=9` was abandoned after 40 minutes, having written 184 MB with
its output offset frozen.

## Running it manually

`scripts/ci/run-replays-local.ps1` is the manual equivalent of the CI job, against local
directories and with no bucket in the path. It stages a copy of the build, stages the data files,
points the `InstallPath` registry value at the staged Generals data, places the repository's replays
and maps where the game looks for them, runs the headless replay, and restores the registry value
and the user data folder afterwards. The retail installs and the build directory are not written to.

```pwsh
pwsh scripts/ci/run-replays-local.ps1 `
    -BuildDir       .\build\vc6 `
    -GeneralsPath   "C:\Program Files (x86)\EA Games\Command & Conquer Generals" `
    -GeneralsMDPath "C:\Program Files (x86)\EA Games\Command & Conquer Generals Zero Hour"
```

The exit code is the verdict; the `DebugLogFile*.txt` paths it prints say which replay diverged.

Requirements, each of which makes the result meaningless if got wrong:

- **A VC6 build with optimisations and `RTS_BUILD_OPTION_DEBUG=OFF`**, per `TESTING.md`. No other
  configuration is retail-compatible by construction. The script reads `CMakeCache.txt` in the build
  directory and refuses a debug build; if there is no cache to read it warns instead.
- **Retail Generals 1.08 *and* Zero Hour 1.04 data.** Zero Hour reads Generals files, which is why
  the CI job sets `HKCU\SOFTWARE\Electronic Arts\EA Games\Generals\InstallPath`. Either full installs
  or the trimmed file sets work — the script takes the same file lists as
  `scripts/ci/pack-gamedata.ps1`, and names exactly what is missing rather than running a partial
  check.
- **Windows.** See the next section.

> Not verified end to end: this script has never been executed against real retail data on a real
> Windows machine. It mirrors the workflow's steps and its PowerShell parses and passes
> PSScriptAnalyzer, and that is all that is currently claimed for it. The first person to run it
> should expect to fix something, and should record what they found here.

## Why this cannot run on a Mac

`generalszh.exe` is a 32-bit x86 Windows binary produced by the VC6 build, and the CI job runs it on
`windows-2022`. There is no native macOS binary to substitute — and note that the "522 unresolved
symbols at levels 1+2+3" this section used to cite is long superseded (the tree now links strictly
with 0 unresolved; see `docs/porting/ci-baselines/`). What is still true is that the native binary is
not a *replay* substitute: retail replay compatibility is explicitly out of the port's scope, so its
frame CRCs are not expected to match the 1.04 stream.

Measured on the project's Apple Silicon machine (M1 Pro, macOS 26.6.1), read-only:

- Rosetta 2 is installed and works, but translates **x86-64 only**; macOS has had no i386 user-space
  loader since 10.15, so a 32-bit x86 executable has nothing to run on.
- No Wine, `wine32on64`, CrossOver, box64, FEX or `qemu-i386` is installed; the Docker CLI is
  present but its daemon was not running.
- Every Windows binary in the retail tree reports `PE32 executable ... Intel 80386, for MS Windows`.

Inference, not measured: the only known way to execute 32-bit Windows code on Apple Silicon is
CrossOver's commercial `wine32on64` under Rosetta, which is unproven for this game and would be a
project of its own. The repository's own Wine/VC6 image
(`resources/dockerbuild/Dockerfile`) is amd64 Debian, so using it on arm64 means nested emulation of
32-bit x86. **Run the replay check on a Windows machine or VM.**

## Wine is not a replay oracle — only a differential check (measured 2026-08-16)

The section above says to run the check on Windows, and that is still the rule, but it left the
impression that the check simply cannot be run elsewhere. It can be *executed* on x86_64 Linux inside
the repository's own `zerohour-build` image (wine 11.0): the VC6 build runs, `generalszh.exe -jobs N
-headless -replay …` reaches the engine's replay driver, and all ten Zero Hour replays simulate to
completion. **Its verdict there is worthless in absolute terms, and this is measured, not inferred:**

- Branch `devin/1786906471-native-debug-build` under wine: `CRC Mismatch` on **all ten** replays,
  `Errors occurred: 10`, exit 1.
- Unmodified `main` (`8c6dfaab0`), built with the identical toolchain in a `git worktree` and run by
  the same harness: `CRC Mismatch` on all ten, at the **identical frames** —
  9724, 3810, 8310, 3310, 6210, 110, 4810, 4210, 4510, 3611. A `grep` of `Simulating`,
  `CRC Mismatch` and `Errors occurred` over the two stdout logs is byte-identical; the whole logs
  differ only in two wall-clock `Elapsed Time` lines.
- `-jobs 1` on the earliest-diverging replay reproduces `CRC Mismatch in Frame 110` exactly, so
  parallelism is not the cause.
- The data is exonerated: the trimmed archives hash to the values pinned for the green CI run
  (`15332b5d…600f3c`, `2d137f6c…6b1b13`), and the golden replay matches bit-exactly for 9724 frames
  — 5:24 of simulation — before diverging. Neither archive was repacked; both were mounted read-only.

So a green wine run does not exist to be had, and a red one proves nothing about the tree. What the
wine run *does* give, cheaply and without a Windows machine, is a **differential** answer: build the
base commit and the branch with the same toolchain and compare the mismatch frames. Identical frames
mean the change moved no simulation behaviour these ten replays observe. Diverging frames mean it
did, and then the authoritative Windows run is mandatory before landing.

Corollary, since a previous session assumed otherwise: **never quote a wine replay result as the
retail replay gate**, in a PR, a doc or a status line. The gate is `check-replays.yml` on
`windows-2022`, or `run-replays-local.ps1` on a Windows machine, and nothing else. Because the
frames above are reproducible on unmodified `main`, they are the baseline the differential check
compares against; if a future session sees a *different* set on `main`, the harness or the image
changed and the comparison must be re-baselined before it means anything.

A second, independent differential oracle costs less than either and is worth running first for a
change that only claims to add off-Windows code: compare the two Windows builds' object files. Of
1322 common objects, 1316 differed only in bytes 4–5 (the COFF `TimeDateStamp`) and the other six
are the TUs that embed the `git describe` string (`gitinfo.cpp`, `WinMain.cpp`, `Shell.cpp`,
`ClientInstance.cpp`, `ReplaySimulation.cpp`, `INIMultiplayer.cpp`). Comparing the linked `.exe`s is
useless — those version strings shift the sections and the two differ by 233 KB with no code change.

## The retail data on hand (measured 2026-08-14)

Measured, read-only, on the Apple Silicon machine's copy of the retail data:

- Zero Hour 1.04: all eight files the packer wants are present.
- Base Generals: the data files are present (`English.big`, `INI.big`, `maps.big`, `W3D.big` and
  both `.scb` scripts), but `BINKW32.DLL` and `mss32.dll` are **not** in that tree. The only copies
  of those two DLLs are the Zero Hour ones, and both are `PE32 ... Intel 80386` DLLs.

**Now demonstrated.** The archives the gate consumes were packed from exactly this tree with
`pack-gamedata.py`, taking `BINKW32.DLL` and `mss32.dll` from the Zero Hour install, and the run
above passed all ten replays. The Generals tree is only ever read as data; the executable under test
is Zero Hour's and links its own copies.

## Hosting the data yourself

Packing does **not** need Windows; only running the check does.

1. **Pack the archives** from a copy of the retail data. On macOS or Linux
   (`brew install sevenzip` / `apt-get install p7zip-full`):

   ```sh
   python3 scripts/ci/pack-gamedata.py \
       --generals   ~/devin-work/zh-data/ZH_Generals \
       --generalsmd ~/devin-work/zh-data \
       --output-dir gamedata-out
   ```

   or on Windows against retail installs (7-Zip required: `winget install 7zip.7zip`):

   ```pwsh
   pwsh scripts/ci/pack-gamedata.ps1 `
       -GeneralsPath   "C:\Program Files (x86)\EA Games\Command & Conquer Generals" `
       -GeneralsMDPath "C:\Program Files (x86)\EA Games\Command & Conquer Generals Zero Hour" `
       -OutputDir      .\gamedata-out
   ```

   Either fails loudly listing any missing file rather than packing a partial archive, and prints
   the SHA256 of each archive at the end. Both read the installs only.

   The Python packer additionally: matches file names case-insensitively, so a copied tree with
   `maps.big` packs (and lands in the archive under the retail spelling); stores no timestamps, so
   packing the same data twice gives the same hash; and, because a Generals *data* tree has no
   `BINKW32.DLL`/`mss32.dll` (see [above](#the-retail-data-on-hand-measured-2026-08-14)), takes
   those two from the Zero Hour install, saying so. Pass `--no-dll-fallback` to forbid that.

   `--archives` selects what to pack: `trimmed` (the default, the replay gate's pair), `full`
   ([the probe object](#the-third-object-zerohour104_gamedata_full7z)), `movies`
   ([the video object](#the-fourth-object-zerohour104_movies7z)), `loose`
   ([the cursors and the rest of `Data/`](#the-fifth-object-zerohour104_loose_data7z)), or `all`.
   Nothing but `trimmed` touches the trimmed pair, so their pinned hashes keep matching. Only the
   Python packer has this; `pack-gamedata.ps1` still packs the trimmed pair alone.

2. **Upload both files** to a private bucket, keeping the file names
   (`generals108_gamedata_trimmed.7z`, `zerohour104_gamedata_trimmed.7z`). Plain AWS S3 and any
   S3-compatible service (Cloudflare R2, MinIO, Backblaze B2) all work:

   ```sh
   aws s3 cp gamedata-out/generals108_gamedata_trimmed.7z  s3://your-bucket/
   aws s3 cp gamedata-out/zerohour104_gamedata_trimmed.7z  s3://your-bucket/
   ```

   On AWS, give CI its own IAM user with an inline policy allowing `s3:GetObject` on
   `arn:aws:s3:::your-bucket/*` and nothing else, and keep the write-capable credentials off
   GitHub. Note that AWS charges egress on every cache miss (~$0.09/GB, and the pair is ~1 GB);
   Cloudflare R2 does not, which is the only reason to prefer it here.

   Add `s3:ListBucket` on the **bucket** ARN (`arn:aws:s3:::your-bucket`, no `/*`) as well. It is
   not needed to fetch an object by key, so the gate runs without it — but without it S3 answers a
   GET for a key that does not exist with `403 Access Denied` rather than `404 NoSuchKey`, to avoid
   confirming existence to a principal that cannot list. That turns "the object was never uploaded"
   into a message that reads as a credentials fault, which is the same class of misdiagnosis as the
   hash-check failure described under [Notes](#notes). Note the ARN: `s3:ListBucket` on the `/*`
   form matches nothing and fails silently.

3. **Set repository variables** (Settings → Secrets and variables → Actions → *Variables*):

   | Variable | Value |
   |---|---|
   | `GAMEDATA_S3_BASE_URI` | `s3://your-bucket` (optionally with a key prefix, e.g. `s3://your-bucket/ci`) |
   | `GAMEDATA_GENERALS_SHA256` | hash printed for `generals108_gamedata_trimmed.7z` |
   | `GAMEDATA_GENERALSMD_SHA256` | hash printed for `zerohour104_gamedata_trimmed.7z` |
   | `GAMEDATA_FULL_SHA256` | hash printed for `zerohour104_gamedata_full.7z`, if that object is hosted |
   | `GAMEDATA_MOVIES_SHA256` | hash printed for `zerohour104_movies.7z`, if that object is hosted |
| `GAMEDATA_LOOSE_SHA256` | hash printed for `zerohour104_loose_data.7z`, if that object is hosted |

4. **Set repository secrets** (same page, *Secrets* tab):

   | Secret | Value |
   |---|---|
   | `R2_ACCESS_KEY_ID` | access key id (an AWS key works; the name is kept for upstream compatibility) |
   | `R2_SECRET_ACCESS_KEY` | secret access key |
   | `R2_ENDPOINT_URL` | the S3-compatible endpoint, e.g. `https://<account>.r2.cloudflarestorage.com`. **Leave unset for plain AWS S3.** |

   Give the credentials read-only access to those two objects and nothing else.

5. **Re-run `GenCI`.** Once the two credential secrets exist the `Game data available?` job stops
   skipping the run (this is how the passing run above was obtained). The job is change-gated, so if it does not trigger, dispatch it or push a
   commit touching engine code.

Unset variables fall back to the upstream bucket and its hashes, so this changes nothing for a
checkout that does have upstream access.

## Notes

- The runner caches the extracted data under a key that includes both expected hashes, so
  re-packing the archives invalidates the cache instead of silently reusing the old copy.
- A download failure now fails with the aws exit code and a pointer at the bucket configuration.
  Previously the `aws s3 cp` return value was ignored and the run died at the hash check with
  "Hash verification failed! File may be corrupted or tampered with.", which reads as data
  corruption when the real cause is missing credentials.
- `pack-gamedata.ps1` requires Windows; `pack-gamedata.py` is the macOS/Linux equivalent. The
  archives the passing run consumed were produced by `pack-gamedata.py` from a macOS copy of the
  retail data; `pack-gamedata.ps1` has still only been exercised against a synthetic tree.
- The gate proves determinism only for the **Windows 32-bit** build. There is no equivalent for a
  native 64-bit build yet, and there cannot be until one links — see
  [`native-build.md`](native-build.md).

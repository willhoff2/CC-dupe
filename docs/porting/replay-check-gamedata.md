# Replay check game data — running the gate in a fork

The replay determinism gate (`Replay Check GeneralsMD` in `GenCI`, implemented by
`.github/workflows/check-replays.yml`) runs the built `generalszh.exe` headless over the `.rep`
files in `GeneralsReplays/` and fails if the simulation diverges. It needs retail game data, which
is not redistributable and so is not in the repository.

Upstream downloads that data from a private bucket. A fork does not inherit its credentials, so the
gate cannot run there by default — which matters for this port: it is the only check that would
catch a save/serialisation or 64-bit change desyncing the simulation. Everything else in CI only
proves the code still builds.

There are two ways to run it, and this document covers both: **manually against a local data
directory** (what this fork does), and **from your own bucket in CI** (how to get the automatic
gate back).

## Status in this fork: manual

No game data is configured, so `Replay Check GeneralsMD` **skips** with a reason in the job summary
instead of failing red. The decision is deliberate: hosting the data was not wanted, and a job that
fails for a missing input tells you nothing about the code.

> **The cost of that choice: with the gate manual, replay determinism — specifically PR #27's
> save/serialisation change — is unverified against desync until somebody runs it.**

The skip is driven by the `Game data available?` job in `check-replays.yml`, which checks whether
`R2_ACCESS_KEY_ID` and `R2_SECRET_ACCESS_KEY` are both set. Setting them (see
[Hosting the data yourself](#hosting-the-data-yourself)) re-enables the gate with no change to any
workflow file.

## What the data is

Two 7z archives holding only what a headless replay run reads — the INI/map/W3D `.big` files, the
two DLLs the executable links against, and the script files. No textures, audio or GUI data, so the
result is not playable. The exact file lists are in `scripts/ci/pack-gamedata.ps1` and in the
comments of `check-replays.yml`.

Retail game data is not redistributable. Keep the bucket private, and do not commit the archives.

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
`windows-2022`. There is no native macOS binary to substitute: `docs/porting/native-build.md`
records 522 unresolved symbols at levels 1+2+3, so nothing links yet.

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

## The retail data on hand (measured 2026-08-14)

Measured, read-only, on the Apple Silicon machine's copy of the retail data:

- Zero Hour 1.04: all eight files the packer wants are present.
- Base Generals: the data files are present (`English.big`, `INI.big`, `maps.big`, `W3D.big` and
  both `.scb` scripts), but `BINKW32.DLL` and `mss32.dll` are **not** in that tree. The only copies
  of those two DLLs are the Zero Hour ones, and both are `PE32 ... Intel 80386` DLLs.

Inference, not verified: a Generals archive packed without those two DLLs is expected to be enough
for the headless Zero Hour run, because the Generals tree is only ever read as data — the executable
under test is Zero Hour's and links its own copies. This has not been demonstrated; the way to
demonstrate it is to make the check pass.

## Hosting the data yourself

1. **Pack the archives** on a Windows machine with retail Generals 1.08 and Zero Hour 1.04
   installed (7-Zip required: `winget install 7zip.7zip`):

   ```pwsh
   pwsh scripts/ci/pack-gamedata.ps1 `
       -GeneralsPath   "C:\Program Files (x86)\EA Games\Command & Conquer Generals" `
       -GeneralsMDPath "C:\Program Files (x86)\EA Games\Command & Conquer Generals Zero Hour" `
       -OutputDir      .\gamedata-out
   ```

   It fails loudly listing any missing file rather than packing a partial archive, and prints the
   SHA256 of each archive at the end.

2. **Upload both files** to a private bucket, keeping the file names
   (`generals108_gamedata_trimmed.7z`, `zerohour104_gamedata_trimmed.7z`). Plain AWS S3 and any
   S3-compatible service (Cloudflare R2, MinIO, Backblaze B2) all work.

3. **Set repository variables** (Settings → Secrets and variables → Actions → *Variables*):

   | Variable | Value |
   |---|---|
   | `GAMEDATA_S3_BASE_URI` | `s3://your-bucket` (optionally with a key prefix, e.g. `s3://your-bucket/ci`) |
   | `GAMEDATA_GENERALS_SHA256` | hash printed for `generals108_gamedata_trimmed.7z` |
   | `GAMEDATA_GENERALSMD_SHA256` | hash printed for `zerohour104_gamedata_trimmed.7z` |

4. **Set repository secrets** (same page, *Secrets* tab):

   | Secret | Value |
   |---|---|
   | `R2_ACCESS_KEY_ID` | access key id (an AWS key works; the name is kept for upstream compatibility) |
   | `R2_SECRET_ACCESS_KEY` | secret access key |
   | `R2_ENDPOINT_URL` | the S3-compatible endpoint, e.g. `https://<account>.r2.cloudflarestorage.com`. **Leave unset for plain AWS S3.** |

   Give the credentials read-only access to those two objects and nothing else.

5. **Re-run `GenCI`.** Once the two credential secrets exist the `Game data available?` job stops
   skipping the run. The job is change-gated, so if it does not trigger, dispatch it or push a
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
- `pack-gamedata.ps1` requires Windows and a machine with both retail installs; there is no
  POSIX packer, so packing from a macOS or Linux copy of the data is not currently possible.
- The gate proves determinism only for the **Windows 32-bit** build. There is no equivalent for a
  native 64-bit build yet, and there cannot be until one links — see
  [`native-build.md`](native-build.md).

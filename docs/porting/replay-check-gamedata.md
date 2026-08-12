# Replay check game data — hosting it yourself

The replay determinism gate (`Replay Check GeneralsMD` in `GenCI`, implemented by
`.github/workflows/check-replays.yml`) runs the built `generalszh.exe` headless over the `.rep`
files in `GeneralsReplays/` and fails if the simulation diverges. It needs retail game data, which
it downloads from a private bucket and hash-checks before use.

That bucket belongs to the upstream project. A fork does not inherit its credentials, so the job
fails at the download step and the gate never runs — which matters for this port: it is the only
check that would catch a save/serialisation or 64-bit change desyncing the simulation. Everything
else in CI only proves the code still builds.

This document covers pointing the job at your own copy.

## What the data is

Two 7z archives holding only what a headless replay run reads — the INI/map/W3D `.big` files, the
two DLLs the executable links against, and the script files. No textures, audio or GUI data, so the
result is not playable. The exact file lists are in `scripts/ci/pack-gamedata.ps1` and in the
comments of `check-replays.yml`.

Retail game data is not redistributable. Keep the bucket private, and do not commit the archives.

## Steps

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

5. **Re-run `GenCI`.** The job is change-gated, so if it does not trigger, dispatch it or push a
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
- The gate proves determinism only for the **Windows 32-bit** build. There is no equivalent for a
  native 64-bit build yet, and there cannot be until one links — see
  [`native-build.md`](native-build.md).

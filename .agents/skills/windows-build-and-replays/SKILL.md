---
name: windows-build-and-replays
description: Run the expensive gates — the Wine/VC6 Windows game build and the retail replay-compatibility check. Use before claiming a refactor is behaviour-preserving, and never as a per-push check.
---

# The Windows build and replay compatibility

The retail Windows build is the **oracle** for engine behaviour: the port is only allowed to be a
refactor, so if this build breaks or the replays stop matching, the change is wrong regardless of what
the native probe says.

## Wine/VC6 game build

```sh
./scripts/docker-build.sh
```

Takes roughly 10 minutes, which is why it is deliberately excluded from `native-port-ci.yml` and
treated as a manual release gate. `scripts/docker-install.sh` provisions the toolchain image.

Run it for any change that touches engine sources, `cmake/`, or the vendored headers. A native-only
change (a script, a doc, a shim used solely by the probe) does not need it — say which you skipped and
why.

## Replay compatibility

Per `TESTING.md`, `GeneralsReplays/` holds replays plus the maps they need, and CI checks them to prove
retail compatibility.

Requirements that are easy to get wrong:

- The build must be a **VC6 build with optimisations and `RTS_BUILD_OPTION_DEBUG = OFF`**. Any other
  configuration is not retail-compatible by construction, so a failure tells you nothing.
- Replays go in a subfolder of
  `%USERPROFILE%/Documents/Command and Conquer Generals Zero Hour Data/Replays`, maps in the
  sibling `Maps` folder.

```bat
START /B /W generalszh.exe -jobs 4 -headless -replay subfolder/*.rep > replay_check.log
echo %errorlevel%
```

The exit code is the verdict; `replay_check.log` says which replay diverged.

## Reporting

Say explicitly which of these two gates you ran, on which commit, and which you skipped. "The build is
green" without naming the gate is not a verifiable claim — the native jobs and this build fail for
completely different reasons.

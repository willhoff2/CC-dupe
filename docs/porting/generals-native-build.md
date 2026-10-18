# Generals links natively

The second step of the base-game target (`native-port-plan.md`). [`generals-compile.md`](generals-compile.md)
brought the `Generals/` tree to compile parity with Zero Hour under the probe. This slice gives it a
native entry point and builds and links it through `scripts/native-build.py`.

## 1. What changed

- **`Generals/Code/Main/PlatformMain.cpp`** (new). The non-Windows twin of `WinMain.cpp`, copied from
  Zero Hour's (`9342cad50`, with `3098ef1f5`'s immortal critical sections). The one difference
  follows Generals' own `WinMain.cpp`: at exit it clears four critical-section pointers, including
  the ASCII-string one, where Zero Hour's clears three.
- **`Generals/Code/Main/CMakeLists.txt`**. `WinMain.cpp` under `if(WIN32)`, `PlatformMain.cpp`
  otherwise, and `binkstub` linked only `if(TARGET binkstub)`. Both are Zero Hour's
  (`9342cad50`, `989d71170`). Windows compiles the same sources and links the same libraries as
  before.
- **`scripts/native-build.py --game zerohour|generals`** (default `zerohour`). The level table names
  `{game}/Code/...`, targets come from the probe's `game_targets()`, and Generals' entry point is
  `Generals/Code/Main`. Its generated `BuildVersion.h` is 1.8.601, from the non-VS6 branch of its
  CMake. It defaults to `build/native-generals`, so it does not overwrite a Zero Hour build.
  Results record `game`.
- **`scripts/ci/check-native-build-baseline.py`**. Picks `native-build-generals-<mode>-level<N>.json`
  for Generals results and refuses to compare one game's results with the other's baseline. A
  baseline with no `game` field is Zero Hour, so every existing baseline keeps its name and meaning.

## 2. Result

**Apple clang 21.0.0, macOS arm64** (levels 1-4, shimmed, release, strict link):

| | Generals |
|---|---:|
| objects | **946 / 946** |
| unresolved symbols, strict link | **0** |
| executable | 14.0 MiB, Mach-O 64-bit arm64 |
| entry point | `libgenerals_code_main` (`_main`, no stub) |

**Probe, clang 14** (the CI image): the new `PlatformMain.cpp` makes the shimmed-with-renderer
count **928 / 974** (was 928 / 973). It fails the probe on `'BuildVersion.h' file not found`, a header
the build generates and the probe never has, as Zero Hour's `PlatformMain.cpp` does. Native mode does
not include `Code/Main` and is unchanged at 641 / 729.

The Linux clang 14 build figure is CI's: `native-build-generals` writes it, and
`ci-baselines/native-build-generals-shimmed-level1-2-3-4.json` is that job's own output.

## 3. Gate

CI job `native-build-generals` runs this build and gates it with `check-native-build-baseline.py`.
It runs only when `classify-changes.py`'s `generals` area is true (`Generals/`, the shared `Core/`
and the build plumbing), so a `GeneralsMD/`-only pull request skips it. A push to `main` runs it.

## 4. First launch

Run from a directory of symlinks into the retail base game, as `docs/working/running-on-macos.md`
§2 does for Zero Hour. The executable opens its window and creates the Vulkan device
(`backing scale 2.00`), then exits 139 (SIGSEGV):

```
EXC_BAD_ACCESS (address=0x0)
W3DVolumetricShadowManager::ReAcquireResources() at W3DVolumetricShadow.cpp:3600
    m_pDev->CreateIndexBuffer(...)
```

`m_pDev` is the D3D8 device, and there is none off Windows. Zero Hour stopped calling it directly in
`75c62d52e` ("Route the shadow renderers through DX8Wrapper") and `dca1b5231`. Neither touched
Generals' copies. Replicating those two, and Zero Hour's other runtime-only fixes, is the next slice:

| Commit | Zero Hour change |
|---|---|
| `75c62d52e`, `dca1b5231` | shadow renderers and the last direct device calls through DX8Wrapper |
| `e471aac3e` | the three mission-frame corruption mechanisms |
| `9808b9ccd`, `38b3dd96e` | the first native frame-CRC divergence |
| `d50fcfdc6` | save/load file operations through the path seam |
| `ca4c1c348`, second half | 64-bit save/replay enum widths and `XferLayout.cpp` |

This list is the set of commits since the port began (`1eb5a61e2`) that touch `GeneralsMD/Code` and
not `Generals/Code`, without what `generals-compile.md` already replicated and without the test-only
`28d31df65`.

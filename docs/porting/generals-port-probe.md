# How far the base game (Generals) is from a native build

The port targets Zero Hour (`GeneralsMD/`). The base game's tree (`Generals/`) shares `Core/` with
it, and most of the port's work landed there, so this measures what is left for the base game. It
uses `scripts/native-port-probe.py --game generals`, which builds the same target set from the
`Generals/` tree with `RTS_GENERALS=1`. `scripts/native-port-probe-game-test.py` checks that no
Generals target reads a `GeneralsMD/` path or define.

**Measured 2026-10-18 at commit `f3b194a8a`**, on macOS with Apple clang 21.0.0. These figures are
**not** comparable with the clang 14 CI baselines. On this compiler the Zero Hour default run gives
670 / 763 native and 719 / 763 shimmed, against the baselines' 675 and 719.

## 1. Headline

Translation units that compile clean:

| Target | Zero Hour, shimmed | Generals, shimmed | Zero Hour, native | Generals, native |
|---|---:|---:|---:|---:|
| `Core/GameEngine` | 197 / 211 | 196 / 211 | 184 / 211 | 164 / 211 |
| `<game>/Code/GameEngine` | 361 / 381 | 322 / 347 | 345 / 381 | 269 / 347 |
| `<game>/Code/Libraries/Source/WWVegas` | 35 / 35 | 34 / 34 | | |
| `Core/GameEngineDevice` | 70 / 70 | 67 / 70 | | |
| `<game>/Code/GameEngineDevice` | 39 / 39 | 35 / 39 | | |
| `<game>/Code/Main` | 0 / 1 | **0 / 0** | | |
| **all targets** (`--include-renderer`) | 964 / 1009 | 916 / 973 | | |
| **default targets** | 719 / 763 | 679 / 729 | 670 / 763 | 574 / 729 |

`Generals/Code/Main` has no translation units because it has only `WinMain.cpp`, which the probe
excludes off Windows. Zero Hour has `PlatformMain.cpp` there.

## 2. What fails for Generals and not for Zero Hour

Failing files were compared by twin path (`Generals/X` against `GeneralsMD/X`). Failures both
games share are not counted. That includes the ~30 files that fail on `gscommon.h` from the fetched
GameSpy headers in both trees. In shimmed mode with the renderer, **13 files fail for Generals and
not for Zero Hour**. Each cause below was traced to its source, not just read off the first
diagnostic.

| # | Cause | Files (shimmed) | Zero Hour's fix |
|---|---|---:|---|
| 1 | `Generals/.../Common/GameState.h` still includes `<windows.h>` for `SYSTEMTIME`. Shimmed, the shim `windows.h` then collides with the `MB_*` enum `Core/.../Debug.cpp` declares for itself. | 1 | `SerializedDateTime` replaced `SYSTEMTIME` (`ca4c1c348`, #27) and the include went |
| 2 | `Generals/.../W3DGameClient.h` names `BinkVideoPlayer` unconditionally. Reached from 3 `Core/` draw modules, `W3DDisplay.cpp`, `W3DGameClient.cpp` and `Win32GameEngine.cpp`. | 6 | the `RTS_HAS_FFMPEG` / `RTS_HAS_BINK` guard in the Zero Hour header |
| 3 | `StackDump.cpp` has no non-Windows side, so it hits its `#error` for the register capture. | 1 | the `backtrace()` stub, [process-and-crash-seam.md](process-and-crash-seam.md) |
| 4 | `GameEngine.cpp` includes the GameSpy headers unguarded. | 1 | the `#ifndef _WIN32` excision, [online-path-excision.md](online-path-excision.md) |
| 5 | One-offs: `GlobalData.cpp` (`FOLDERID_Documents`), `InGamePopupMessage.cpp` (`pause` redefined), `KeyboardOptionsMenu.cpp` (`VK_RETURN`), `W3DWebBrowser.cpp` (`IDispatch`) | 4 | the Zero Hour twins compile; not traced individually |
| 6 | No native entry point: `PlatformMain.cpp` does not exist in `Generals/Code/Main`. | (0 / 0) | `GeneralsMD/Code/Main/PlatformMain.cpp` |

Native mode adds about 60 more Generals-only failures. All of the ones sampled (`Object.cpp`,
`Team.cpp`, `AI.cpp`) are cause 1: `GameState.h` is reached through `PreRTS.h` from most of the
engine. So one header accounts for most of the native gap.

## 3. What this does not measure

These are compile results only. Linking, startup and playing a mission are unmeasured for Generals.
For Zero Hour those took most of the port's work (window, audio, video, renderer, save and replay
slices). Most of those fixes are in `Core/` and are already shared, but a Generals binary has never
been linked or run off Windows. `scripts/native-build.py` builds only `Core/` and `GeneralsMD/`.

## 4. Reproduce

```sh
python3 scripts/native-port-probe.py --game generals --with-shims --include-renderer \
  --report generals-shimmed.md --json generals-shimmed.json
python3 scripts/native-port-probe.py --game generals --report generals-native.md
```

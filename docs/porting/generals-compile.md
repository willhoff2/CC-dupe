# Generals compiles where Zero Hour does

The base game's single-player campaign is the port's second target (decided 2026-10-18; see
`native-port-plan.md`). This slice brings the `Generals/` tree to compile parity with `GeneralsMD/`
under the native probe. It replicates fixes that already landed for Zero Hour and adds no new seam.
[`generals-port-probe.md`](generals-port-probe.md) is the measurement that scoped it.

## 1. How a fix is replicated

Each commit here replays one Zero Hour commit's `GeneralsMD/` hunks onto the matching `Generals/`
paths with a 3-way apply. Every result is then read and compiled with the probe's flags. A hunk is
**not** replicated when it is:

- Zero Hour's own retail code rather than port work. Example: the single-player replay cut in
  `Recorder.cpp`.
- a change that already covered both trees. #26's `registry.cpp` and #23's `KeyDefs.h` are
  examples.
- outside this slice's goal (section 4).

| Commit | Zero Hour change | Generals files |
|---|---|---|
| `ca4c1c348` (#27), compile half | `SerializedDateTime` replaces `SYSTEMTIME`; `GameState.h` and `Recorder.h` drop `<windows.h>` | `GameState.{h,cpp}`, `Recorder.{h,cpp}`, and the 5 files that now include `<windows.h>` themselves |
| `812dd5613` (#24), `7a1580810` | process seam; `StackDump`'s `backtrace()` side | `StackDump.{h,cpp}`, `MainMenu.cpp` |
| `cfdfc0272`, `7a1580810` | `CopyFile`'s include; `GetComputerName`'s `DWORD` | `Recorder.cpp` |
| `69455455d` | CRT/misc-header cluster, including the `pause` rename | `InGamePopupMessage.cpp`, `PopupPlayerInfo.cpp`, `PopupReplay.cpp` |
| `989d71170`, `13faf3c8e` (#714) | FFmpeg / Bink / Null video player selection | `W3DGameClient.h` |
| `469b043d8` | window title through the window seam | `GameEngine.cpp` |
| `d4d355aff`, `9342cad50` | embedded-browser excision; device layer through the window seam | `GameEngine.cpp`, `W3DWebBrowser.{h,cpp}`, `Win32GameEngine.{h,cpp}` |
| `dddda24c5` | loud startup failures | `GameEngine.cpp` |
| `1e1da326c` (#26) | filesystem/path seam | `GlobalData.cpp`, `GameStateMap.cpp`, `ReplayMenu.cpp` |
| `4868d22ab` (#23) | `<windows.h>` for `VK_RETURN` | `KeyboardOptionsMenu.cpp` |
| none | `<limits.h>` for `UINT_MAX`; Zero Hour's header does not use it | `StealthUpdate.h` |

`StealthUpdate.h` is the one fix with no Zero Hour commit behind it. Generals'
`StealthUpdateModuleData()` defaults a delay to `UINT_MAX`, which MSVC's headers supply indirectly and
libstdc++'s do not. It failed 14 device-layer files under clang 14 on Linux; Apple's libc++ hid it,
which is why the macOS figures below did not show it.

`GlobalData.cpp` is the one that is not a copy. Generals builds its user data directory from the INI
leaf name in `parseGameDataDefinition()`; Zero Hour reads it from the registry in the constructor.
The Win32 calls are replaced where Generals makes them, and that difference between the games is
kept.

On Windows each hunk behaves as it does in Zero Hour, where its slice made it behaviour-preserving.
Most are `#ifdef`-guarded or call a platform function whose Windows side is the code it replaced.
The rest are a rename (`pause` to `s_pause`), a type spelling (`DWORD`) and `GameStateMap.cpp`'s
directory walk, which now lists the directory before deleting rather than interleaving the two.
`SerializedDateTime` has `SYSTEMTIME`'s eight 16-bit fields in the same order, and Zero Hour's
`XferLayout.cpp` asserts it is 16 bytes. The Windows build itself is CI's to confirm (section 4).

## 2. Result

**clang 14, Ubuntu 22.04, x86-64**: the CI probe job's image and compiler, run locally under
emulation. The same container reproduced the committed Zero Hour baseline exactly (675 / 763 native),
which is what makes its Generals figures usable as baselines. These are the figures in
`ci-baselines/native-port-probe-generals-*.json`.

| Generals | After | Zero Hour, same mode |
|---|---:|---:|
| shimmed, with renderer | **928 / 973** | 964 / 1009 |
| native | **641 / 729** | 675 / 763 |

**Apple clang 21.0.0, macOS**, base `ec7caf52f`, before and after. "After" was measured before the
`StealthUpdate.h` fix; the 14 files it fixes on Linux were already clean on this compiler.

| Generals | Before | After | Zero Hour, same mode |
|---|---:|---:|---:|
| shimmed, with renderer | 916 / 973 | **928 / 973** | 964 / 1009 |
| native | 574 / 729 | **636 / 729** | 670 / 763 |

Files that fail for Generals and not for their Zero Hour twin. The "after" column holds on both
compilers:

| | Before | After |
|---|---:|---:|
| shimmed, with renderer | 13 | **1** |
| native | ~60 | **0** |

The one left is `Generals/.../GameEngine.cpp`, which fails on `gscommon.h`. It is a gap in the
probe's include path, not in the code: `native-build.py` adds `gamespy-src/include/gamespy` for
exactly this reason (its `EXTRA_DEP_INCLUDES`), and with that directory the file compiles. Zero Hour
has about 30 files failing the same way, so the probe undercounts both games by the same mechanism.
Fixing it changes the Zero Hour baselines and is its own slice.

Zero Hour's `GameEngine.cpp` never included `PeerDefs.h` and `PersistentStorageThread.h`. Generals'
includes them in its retail code, which is why only Generals' twin hits the gap.

## 3. Gate

The CI probe job also probes the Generals tree (native, and shimmed with the renderer) and gates both
with `check-probe-baseline.py` against `ci-baselines/native-port-probe-generals-*.json`. It is a
ratchet on Generals' own counts. It does **not** require parity with Zero Hour, so a Zero Hour slice
still lands first and is replicated afterwards, as `port-slice-workflow` says.

Both steps run only when `classify-changes.py`'s `generals` area is true. That covers `Generals/`,
the shared `Core/`, `Dependencies/`, `resources/`, the renderer interface in `spikes/renderer/src`, the
dependency pins in `cmake/`, the probe and its shims, and the CI plumbing. A `GeneralsMD/`-only pull
request skips them; a push to `main` runs them, as it runs every area.

## 4. Still open

- **No native entry point.** `Generals/Code/Main` has only `WinMain.cpp`; `PlatformMain.cpp` and
  `9342cad50`'s Main wiring are the next slice, with `989d71170`'s `Main/CMakeLists.txt` `binkstub`
  guard.
- **Not built or linked.** `scripts/native-build.py` builds `Core/` and `GeneralsMD/` only. The probe
  answers "does it parse"; nothing here has produced a Generals object file.
- **Save/replay 64-bit layout.** #27's other half (enum widths, `XferLayout.cpp`) and the save/load
  path boundary (`d50fcfdc6`, `Open_Stream` in `GameStateMap.cpp`) are runtime correctness, needed
  before a Generals save or replay is trusted off Windows.
- **Gates that scan only Zero Hour**: `init-reporting-scan.py` and `check-embedded-browser.py`.
- **Not measured**: any Windows build of these changes. That is CI's GenCI `Build Generals` job.
